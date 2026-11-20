/* The hardware test agent (make 3ds AGENT=<pc-ip>[:port]; docs/hardware-harness.md): a test build that takes its
 * scenario, its scripted input, its switches and what to capture from a PC (tools/hwrun.py serve), runs it, sends back
 * the log and the frames, and starts itself again for the next job, so a 3DS is driven from the PC after one launch.
 *
 * One TCP connection per launch, the console the client:
 *   console: "HELLO 1 model=old|new relaunch=0|1 heap=KB linear=KB\n"
 *   PC:      "WAIT\n" every 2 s while it has no job, "QUIT\n", or "JOB <id> <bytes>\n" and the job's text:
 *              frames N                     virtual frames to run (plat_wait_frame's count)
 *              press LIST                   scripted buttons (input_script.c: "CROSS@900+30,UP@1000")
 *              touch LIST                   scripted taps (touch.c's AUTOTOUCH)
 *              capture A B STEP SCALE SCR   frames A..B every STEP, 1/SCALE size, SCR 1 top, 2 bottom, 3 both
 *              set NAME VALUE FRAME [@HEX]  an int set at FRAME (0: before the game starts): a switch named below,
 *                                           else the int at that address, if the check line before it held
 *              check ADDR LEN ADLER32       the build the addresses are from: the Adler-32 of its .text
 *              bufkb N                      the capture buffer (default 3072)
 *              hang N                       seconds without a frame that end the job as hung (default 30, 0 never)
 *              cfg TEXT                     a line of the scenario's cfg (none: the title screen)
 *   console: chunks, a type byte and a 32-bit little-endian length: 'L' log text, 'F' captured frames, 'E' the job's
 *            end ("status=done ..."); then it closes, and starts itself again (relaunch: through Luma's hb:ldr).
 *
 * A captured frame is what the screen shows: the top screen's presented buffer (render.c's own, the GPU's display
 * transfer of the frame before: nothing is waited for) and the bottom screen's front buffer, as RGB565 in the
 * buffer's own order (columns, bottom to top), each frame as its difference from the one before of that screen:
 *   "FRM1", frame u32, present serial u32, screen u8, scale u8, 0, 0, columns u16, rows u16, payload bytes u32,
 *   then 16-bit words: [pixels unchanged][pixels that follow]... (the rest of the frame unchanged).
 * They are kept in memory and sent when the buffer is full (a stall, logged with its length) and at the job's end. */
#ifdef PORT_AGENT
#include <3ds.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "ctr.h"
#include "input_script.h"

#define AGENT_DIR "sdmc:/3ds/ff9/agent"
#define JOB_CFG AGENT_DIR "/job.cfg"
#define SELF_DEFAULT "/3ds/ff9-agent.3dsx" /* (when the launcher gave no path: where the docs say to put it) */
#define GIVE_UP_S 60                         /* without the PC for this long: back to the launcher */

/* the switches a job may set by name; any other int of the build by its address (hwrun.py finds it in the ELF).
 * Weak: a build without one still links, and says so when a job names it. */
extern int ctr_defer __attribute__((weak)), ctr_slot_keep __attribute__((weak)), ctr_defer_world __attribute__((weak)),
    ctr_sw_chain __attribute__((weak)), ctr_sw_poly1 __attribute__((weak)), ctr_sw_drawmemo __attribute__((weak)),
    ctr_sw_reorder __attribute__((weak)), ctr_sw_overlap __attribute__((weak)), ctr_sw_defer_thread __attribute__((weak)), ctr_sw_kflush __attribute__((weak)), ctr_sw_flush __attribute__((weak));
static const struct { const char *name; int *p; } switches[] = {
    {"ctr_defer", &ctr_defer},
    {"ctr_slot_keep", &ctr_slot_keep},
    {"ctr_defer_world", &ctr_defer_world},
    {"ctr_sw_chain", &ctr_sw_chain},
    {"ctr_sw_poly1", &ctr_sw_poly1},
    {"ctr_sw_drawmemo", &ctr_sw_drawmemo},
    {"ctr_sw_reorder", &ctr_sw_reorder},
    {"ctr_sw_overlap", &ctr_sw_overlap},
    {"ctr_sw_defer_thread", &ctr_sw_defer_thread},
    {"ctr_sw_kflush", &ctr_sw_kflush},
    {"ctr_sw_flush", &ctr_sw_flush},
};

extern volatile const char *ctr_where;
void ctr_console_raw(const char *text, int n);
const unsigned char *ctr_capture_top(unsigned *serial) __attribute__((weak)); /* render.c */
extern const char *ctr_touch_script;                                           /* touch.c */
extern char __start__[], __end__[];
extern char *fake_heap_end;
extern int __system_argc;
extern char **__system_argv;

static struct {
    int active;
    char id[32];
    long frames;
    int hang;
    int quiet; /* the network down while the job runs (its sysmodules take some 12% of an Old 3DS: speed jobs) */
    unsigned bufkb;
    struct { long a, b; int step, scale, screens; } cap[8];
    int ncap;
    struct { int *p, value; long frame; char name[32]; } set[32];
    int nset;
} job;

static int sock = -1;
static volatile int net_busy, finishing;
static u32 *soc, soc_size;
static const char *agent_host;
static int *agent_port, quiet_now, radio_off;
static volatile u64 net_idle_at; /* when the last send ended: the watchdog's seconds without a frame include it */
static int can_relaunch;

/* ---- the log: every line of console.c's, sent every 2 s -------------------------------------------------------- */

static char logbuf[48 * 1024];
static int loglen, logdropped;
static LightLock loglock;
static int loglock_ok;

void ctr_agent_log(const char *text, int n) {
    if (!loglock_ok) LightLock_Init(&loglock), loglock_ok = 1; /* (the first line is the main thread's, before any other) */
    LightLock_Lock(&loglock);
    if (n > (int)sizeof logbuf - loglen) logdropped += n;
    else memcpy(logbuf + loglen, text, n), loglen += n;
    LightLock_Unlock(&loglock);
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) { /* (a line of the log from any thread: console.c's raw path) */
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof line) n = sizeof line - 1;
    ctr_console_raw(line, n);
}

/* ---- the connection ------------------------------------------------------------------------------------------- */

static unsigned ms_since(u64 tick) {
    return (unsigned)((svcGetSystemTick() - tick) / (SYSCLOCK_ARM11 / 1000));
}

static void net_close(void) {
    if (sock >= 0) close(sock);
    sock = -1;
}

/* all of it, or the connection is given up (10 s without a byte taken): nothing here waits for ever */
static int send_all(const void *data, size_t n) {
    if (sock < 0) return -1;
    const char *p = data;
    u64 last = svcGetSystemTick();
    const char *where = (const char *)ctr_where;
    ctr_where = "agent: sending";
    net_busy = 1;
    while (n) {
        ssize_t r = send(sock, p, n > 0x4000 ? 0x4000 : n, 0);
        if (r > 0) {
            p += r, n -= (size_t)r;
            last = svcGetSystemTick();
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) && ms_since(last) < 10000) {
            svcSleepThread(1000000);
        } else break;
    }
    net_idle_at = svcGetSystemTick();
    net_busy = 0;
    ctr_where = where;
    if (n) {
        net_close();
        return -1;
    }
    return 0;
}

static int send_chunk(char type, const void *data, unsigned n) {
    unsigned char h[5] = {(unsigned char)type, (unsigned char)n, (unsigned char)(n >> 8), (unsigned char)(n >> 16), (unsigned char)(n >> 24)};
    if (send_all(h, 5)) return -1;
    return n ? send_all(data, n) : 0;
}

static void log_flush(void) {
    static char out[sizeof logbuf + 64];
    if (sock < 0 || !loglen) return;
    LightLock_Lock(&loglock);
    int n = loglen;
    memcpy(out, logbuf, n);
    if (logdropped) n += snprintf(out + n, 64, "[agent: %d bytes of log dropped]\n", logdropped);
    loglen = logdropped = 0;
    LightLock_Unlock(&loglock);
    send_chunk('L', out, (unsigned)n);
}

/* received bytes, a line or a count at a time; 0: not yet, -1: the connection is gone */
static char rx[2048];
static int rxn;
static int rx_fill(void) {
    if (sock < 0) return -1;
    if (rxn == (int)sizeof rx) return 0;
    ssize_t r = recv(sock, rx + rxn, sizeof rx - rxn, 0);
    if (r > 0) { rxn += r; return 1; }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    net_close();
    return -1;
}
static int rx_line(char *line, int max) {
    int r = rx_fill();
    char *nl = memchr(rx, '\n', rxn);
    if (!nl) return r < 0 ? -1 : 0;
    int n = (int)(nl - rx);
    snprintf(line, max, "%.*s", n, rx);
    rxn -= n + 1;
    memmove(rx, nl + 1, rxn);
    return 1;
}

static int net_connect(const char *host, int port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (!inet_aton(host, &a.sin_addr)) return -1;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    /* (a blocking connect, as libctru's own 3dslink client: refused at once without the server, a few seconds
     * without the PC) */
    if (connect(sock, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno;
        net_close();
        errno = e;
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    rxn = 0;
    return 0;
}

/* ---- starting again: the Homebrew Launcher's own way of starting a .3dsx under Luma3DS -------------------------- */

/* Luma's loader (Rosalina's hb:ldr port) is told which .3dsx the title it runs homebrew in loads next, and libctru
 * leaves by starting that title again (aptExit: a chainload to self, which the homebrew environment asks for, and
 * how every exit returns to the launcher: its .3dsx is the default target). So: the target set to this file, then
 * an ordinary exit. */
static Result hbldr(Handle h, u32 header, const void *buf, u32 size) {
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = header;
    cmd[1] = IPC_Desc_StaticBuffer(size, header == IPC_MakeHeader(2, 0, 2) ? 0 : 1);
    cmd[2] = (u32)buf;
    Result rc = svcSendSyncRequest(h);
    return R_SUCCEEDED(rc) ? (Result)cmd[1] : rc;
}

static int relaunch_possible(void) {
    Handle h;
    if (R_FAILED(svcConnectToPort(&h, "hb:ldr"))) return 0;
    svcCloseHandle(h);
    return 1;
}

static int relaunch_set(void) {
    static char path[256];
    static u32 args[0x400 / 4]; /* (the launcher's: the count, then the strings) */
    Handle h;
    const char *self = __system_argc > 0 && __system_argv && __system_argv[0] ? __system_argv[0] : "";
    if (!strncmp(self, "sdmc:/", 6)) self += 5;
    if (self[0] != '/') self = SELF_DEFAULT;
    snprintf(path, sizeof path, "%s", self);
    args[0] = 1;
    snprintf((char *)&args[1], sizeof args - 4, "sdmc:%s", path);
    if (R_FAILED(svcConnectToPort(&h, "hb:ldr"))) return 0;
    Result rc = hbldr(h, IPC_MakeHeader(2, 0, 2), path, strlen(path) + 1); /* SetTarget */
    if (R_SUCCEEDED(rc)) rc = hbldr(h, IPC_MakeHeader(3, 0, 2), args, sizeof args); /* SetArgv */
    svcCloseHandle(h);
    if (R_FAILED(rc)) {
        say("agent: hb:ldr refused %s (%08lx)\n", path, (unsigned long)rc);
        return 0;
    }
    aptSetChainloaderToSelf(); /* (set already where the environment asks for it) */
    say("agent: starting %s again\n", path);
    return 1;
}

/* ---- capture -------------------------------------------------------------------------------------------------- */

static unsigned char *cap;
static unsigned cap_size, cap_used;
static u16 *prev[2];
static const unsigned char *bottom_fb[2];
static unsigned n_captured, capture_us, capture_max_us, n_flushes, flush_ms;
static long last_frame;

static unsigned heap_free_kb(void) {
    struct mallinfo mi = mallinfo();
    return (unsigned)(((size_t)(fake_heap_end - (char *)sbrk(0)) + mi.fordblks) >> 10);
}

static void frames_flush(void) {
    if (!cap_used) return;
    u64 t = svcGetSystemTick();
    if (send_chunk('F', cap, cap_used) == 0) {
        unsigned ms = ms_since(t);
        n_flushes++, flush_ms += ms;
        say("agent: %u KB of frames sent in %u ms at frame %ld\n", cap_used >> 10, ms, last_frame);
        cap_used = 0;
    }
}

/* a screen's buffer (cols columns of 240 pixels, BGR8) as RGB565, every scale-th pixel each way, its difference
 * from prev; returns the bytes written (at most 2 per pixel and 64) */
static unsigned encode(const unsigned char *fb, int cols, int scale, u16 *prev_px, u16 *out) {
    u16 *o = out, *lit = NULL;
    unsigned skip = 0, same = 0;
    u16 *pv = prev_px;
#define PUT(c) do { \
        if (!lit || *lit == 0xFFFF) { \
            while (skip > 0xFFFF) *o++ = 0xFFFF, *o++ = 0, skip -= 0xFFFF; \
            *o++ = (u16)skip, lit = o++, *lit = 0, skip = 0; \
        } \
        *o++ = (c), ++*lit; \
    } while (0)
    for (int c = 0; c < cols; c += scale) {
        const unsigned char *p = fb + c * 240 * 3;
        for (int r = 0; r < 240; r += scale, p += 3 * scale, pv++) {
            u16 v = (u16)((p[2] >> 3) << 11 | (p[1] >> 2) << 5 | p[0] >> 3);
            if (v == *pv) { same++; continue; }
            if (same) {
                /* (a few unchanged pixels inside a changed run stay in it: a new run costs two words) */
                if (lit && same < 4) for (unsigned k = same; k; k--) PUT(pv[-(int)k]);
                else skip += same, lit = NULL;
                same = 0;
            }
            PUT(v);
            *pv = v;
        }
    }
#undef PUT
    return (unsigned)((char *)o - (char *)out);
}

static void capture_screen(int screen, long frame, int scale) {
    static int failed;
    unsigned serial = 0;
    const unsigned char *fb;
    int cols = screen ? 320 : 400;
    if (failed) return;
    if (screen) { /* the front buffer: the one citro3d is not about to copy the frame into */
        const unsigned char *back = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        fb = back == bottom_fb[0] ? bottom_fb[1] : bottom_fb[0];
    } else fb = ctr_capture_top ? ctr_capture_top(&serial) : NULL;
    if (!fb) return; /* (nothing presented yet) */
    if (!cap) {
        unsigned want = job.bufkb << 10, before = heap_free_kb();
        while (want >= 512 * 1024 && !(cap = malloc(want))) want /= 2;
        cap_size = cap ? want : 0;
        say("agent: capture buffer %u KB (heap %u KB free before, linear %u KB)\n", cap_size >> 10, before,
            (unsigned)(linearSpaceFree() >> 10));
        if (!cap) { failed = 1; return; }
    }
    if (!prev[screen] && !(prev[screen] = calloc((size_t)cols * 240, 2))) { failed = 1; return; }
    static int prev_scale[2];
    if (prev_scale[screen] != scale) memset(prev[screen], 0, (size_t)cols * 240 * 2), prev_scale[screen] = scale;
    unsigned n = (unsigned)((cols + scale - 1) / scale) * (unsigned)((240 + scale - 1) / scale);
    if (cap_used + 24 + 2 * n + 64 > cap_size) frames_flush();
    if (cap_used + 24 + 2 * n + 64 > cap_size) { /* (the PC is gone: what is held goes to the card at the end) */
        say("agent: capture stopped at frame %ld (no connection, buffer full)\n", frame);
        failed = 1;
        return;
    }
    u64 t = svcGetSystemTick();
    GSPGPU_FlushDataCache(fb, (u32)cols * 240 * 3); /* (the GPU wrote it: nothing of it kept in the CPU's cache) */
    unsigned char *h = cap + cap_used;
    unsigned len = encode(fb, cols, scale, prev[screen], (u16 *)(h + 24));
    u32 hdr[6] = {0x314D5246u /* "FRM1" */, (u32)frame, serial,
                  (u32)screen | (u32)scale << 8,
                  (u32)((cols + scale - 1) / scale) | (u32)((240 + scale - 1) / scale) << 16, len};
    memcpy(h, hdr, 24);
    cap_used += 24 + len;
    unsigned us = (unsigned)((svcGetSystemTick() - t) / (SYSCLOCK_ARM11 / 1000000));
    capture_us += us;
    if (us > capture_max_us) capture_max_us = us;
    n_captured++;
}

/* ---- a job's end ---------------------------------------------------------------------------------------------- */

static void finish(const char *status) __attribute__((noreturn));
static void finish(const char *status) {
    if (finishing++) for (;;) svcSleepThread(1000000000LL); /* (the other thread is ending it) */
    if (quiet_now) { /* (the network up again, the job taken up with the PC where it left it) */
        quiet_now = 0;
        if (radio_off && R_SUCCEEDED(nwmExtInit())) {
            NWMEXT_ControlWirelessEnabled(true);
            nwmExtExit();
            svcSleepThread(4000000000LL); /* (the access point found again) */
        }
        if (R_SUCCEEDED(socInit(soc, soc_size)))
            for (int i = 0; i < (radio_off ? 90 : 20) && sock < 0; i++) {
                if (net_connect(agent_host, *agent_port) == 0) {
                    char line[96];
                    int n = snprintf(line, sizeof line, "HELLO 1 model=%s resume=%s\n", ctr_is_new3ds() ? "new" : "old", job.id);
                    send_all(line, (unsigned)n);
                } else svcSleepThread(500000000LL);
            }
    }
    frames_flush();
    say("agent: job %s %s at frame %ld: %u frames captured in %u ms (longest %u us), %u sends %u ms\n", job.id, status,
        last_frame, n_captured, capture_us / 1000, capture_max_us, n_flushes, flush_ms);
    if (cap_used) { /* (not sent: to the card, for tools/hwrun.py decode) */
        FILE *f = fopen(AGENT_DIR "/capture.bin", "wb");
        if (f) {
            fwrite(cap, 1, cap_used, f);
            fclose(f);
            say("agent: %u KB of frames kept in " AGENT_DIR "/capture.bin\n", cap_used >> 10);
        }
    }
    int again = relaunch_set();
    log_flush();
    char end[160];
    int n = snprintf(end, sizeof end, "status=%s frame=%ld captured=%u capture_ms=%u send_ms=%u relaunch=%d", status,
                     last_frame, n_captured, capture_us / 1000, flush_ms, again);
    if (send_chunk('E', end, (unsigned)n) == 0) { /* (and the PC's close waited for, so the exit doesn't cut it) */
        shutdown(sock, SHUT_WR);
        u64 t = svcGetSystemTick();
        char b[64];
        while (sock >= 0 && ms_since(t) < 3000) {
            ssize_t r = recv(sock, b, sizeof b, 0);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            svcSleepThread(5000000);
        }
    }
    net_close();
    ctr_shutdown();
    exit(0);
}

void ctr_agent_end(const char *status) {
    if (job.active) finish(status);
}

/* the watchdog's thread: no frame for `seconds` */
void ctr_agent_hang(unsigned seconds) {
    /* (not while a send is under way, nor counting one that just ended: a buffer of frames takes most of a minute over
     * an Old 3DS's Wi-Fi, and the first job that sent one ended as hung the moment it had, 2026-11-19) */
    if (job.active && job.hang && seconds >= (unsigned)job.hang && !net_busy && !finishing &&
        ms_since(net_idle_at) >= (u64)job.hang * 1000)
        finish("hang");
}

void ctr_agent_frame(long frame) {
    if (!job.active) return;
    last_frame = frame;
    for (int i = 0; i < job.nset; i++)
        if (job.set[i].frame == frame) {
            say("agent: %s = %d at frame %ld (was %d)\n", job.set[i].name, job.set[i].value, frame, *job.set[i].p);
            *job.set[i].p = job.set[i].value;
        }
    for (int i = 0; i < job.ncap; i++)
        if (frame >= job.cap[i].a && frame <= job.cap[i].b && (frame - job.cap[i].a) % job.cap[i].step == 0) {
            if (job.cap[i].screens & 1) capture_screen(0, frame, job.cap[i].scale);
            if (job.cap[i].screens & 2) capture_screen(1, frame, job.cap[i].scale);
            break;
        }
    /* (the log to the PC every 20 s, right after a heartbeat's line, and its time said: a send every 120 frames took
     * some 250 ms of each heartbeat on an Old 3DS, small packets waiting for their acknowledgements: every job read
     * 12% slow. The beat after a send's line holds it.) */
    if (frame % 1200 == 1 || loglen > (int)sizeof logbuf / 2) {
        u64 t = svcGetSystemTick();
        log_flush();
        say("agent: log sent at frame %ld, %u ms (in the heartbeat that follows)\n", frame, ms_since(t));
    }
    if (frame >= job.frames) finish("done");
}

/* ---- a job's start -------------------------------------------------------------------------------------------- */

static unsigned adler32(const unsigned char *p, unsigned n) {
    unsigned a = 1, b = 0;
    while (n) {
        unsigned k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) a += *p++, b += a;
        a %= 65521, b %= 65521;
    }
    return b << 16 | a;
}

/* the job's text; an error's reason, or NULL */
static const char *job_parse(char *text, FILE *cfg, int *ncfg) {
    static char reason[96];
    static char *touch;
    int checked = 0;
    job.frames = 600, job.hang = 30, job.bufkb = 3072, job.ncap = job.nset = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        else arg = line + strlen(line);
        if (!strcmp(line, "frames")) job.frames = strtol(arg, NULL, 10);
        else if (!strcmp(line, "hang")) job.hang = (int)strtol(arg, NULL, 10);
        else if (!strcmp(line, "bufkb")) job.bufkb = (unsigned)strtoul(arg, NULL, 10);
        else if (!strcmp(line, "quiet")) job.quiet = (int)strtol(arg, NULL, 10);
        else if (!strcmp(line, "cfg")) fprintf(cfg, "%s\n", arg), ++*ncfg;
        else if (!strcmp(line, "press")) {
            if (!input_script_add_list(arg)) return "press: not BUTTON@FRAME[+FRAMES],...";
        } else if (!strcmp(line, "touch")) {
            free(touch);
            ctr_touch_script = touch = strdup(arg);
        } else if (!strcmp(line, "capture")) {
            if (job.ncap == 8) return "capture: at most 8 windows";
            long a, b;
            int step = 1, scale = 1, screens = 1;
            if (sscanf(arg, "%ld %ld %d %d %d", &a, &b, &step, &scale, &screens) < 2 || step < 1 || scale < 1 || scale > 8 ||
                !(screens & 3))
                return "capture: A B [STEP SCALE SCREENS]";
            job.cap[job.ncap].a = a, job.cap[job.ncap].b = b, job.cap[job.ncap].step = step;
            job.cap[job.ncap].scale = scale, job.cap[job.ncap++].screens = screens;
        } else if (!strcmp(line, "check")) {
            unsigned long addr, len, sum;
            if (sscanf(arg, "%lx %lx %lx", &addr, &len, &sum) != 3) return "check: ADDR LEN ADLER32";
            checked = addr >= (unsigned long)__start__ && len <= (unsigned long)(__end__ - __start__) &&
                      addr + len <= (unsigned long)__end__ && adler32((const unsigned char *)addr, len) == sum ? 1 : -1;
        } else if (!strcmp(line, "set")) {
            char name[32];
            int value;
            long frame = 0;
            int *p = NULL;
            if (job.nset == 32) return "set: at most 32";
            if (sscanf(arg, "%31s %d %ld", name, &value, &frame) < 2) return "set: NAME VALUE [FRAME]";
            for (unsigned i = 0; i < sizeof switches / sizeof *switches; i++)
                if (!strcmp(name, switches[i].name)) p = switches[i].p;
            char *at = strchr(arg, '@');
            if (!p && at) {
                unsigned long addr = strtoul(at + 1, NULL, 16);
                if (checked < 0) return "set: the PC's ELF is not this build (hwrun.py --elf): its addresses refused";
                if (!checked || addr & 3 || addr < (unsigned long)__start__ || addr + 4 > (unsigned long)__end__)
                    return "set: a bad address";
                p = (int *)addr;
            }
            if (!p) {
                snprintf(reason, sizeof reason, "set: this build has no switch %s (hwrun.py --elf finds any int)", name);
                return reason;
            }
            job.set[job.nset].p = p, job.set[job.nset].value = value, job.set[job.nset].frame = frame;
            snprintf(job.set[job.nset++].name, sizeof job.set[0].name, "%s", name);
        } else {
            snprintf(reason, sizeof reason, "unknown line %.40s", line);
            return reason;
        }
    }
    return NULL;
}

/* the keys and the system served while waiting; 0: leave (START, or the system closes the app) */
static int serve_system(void) {
    if (!aptMainLoop()) return 0;
    hidScanInput();
    if (hidKeysDown() & KEY_START) return 0;
    gspWaitForVBlank();
    return 1;
}

static void leave(const char *why) __attribute__((noreturn));
static void leave(const char *why) {
    say("agent: %s: leaving\n", why);
    printf("\n%s\n", why);
    for (int i = 0; i < 90 && aptMainLoop(); i++) gspWaitForVBlank();
    net_close();
    gfxExit();
    exit(0);
}

/* Called first thing (main): waits for a job from the PC, sets it up, and returns its scenario's file, or NULL for
 * the title screen. Leaves for the launcher on QUIT, on START, or after GIVE_UP_S without the PC. */
const char *ctr_agent_start(void) {
    static PrintConsole con;
    static char host[64] = PORT_AGENT;
    char line[128];
    static int port = 5009;
    agent_host = host, agent_port = &port;
    aptSetSleepAllowed(false); /* (the lid may close: the console stays up for the next job) */
    consoleInit(GFX_BOTTOM, &con);
    FILE *f = fopen("sdmc:/3ds/ff9/agent.txt", "r"); /* (the PC's address changed: ip[:port] here, no rebuild) */
    if (f) {
        if (fgets(line, sizeof line, f) && strlen(line) > 6) snprintf(host, sizeof host, "%.63s", line);
        fclose(f);
    }
    host[strcspn(host, "\r\n ")] = 0;
    char *colon = strchr(host, ':');
    if (colon) *colon = 0, port = atoi(colon + 1);
    printf("FF9 test agent\n\nPC: %s port %d\nSTART: back to the launcher\n\n", host, port);
    /* (console.c has it up already when 3dslink sent the build: then this fails, and that is fine) */
    for (u32 size = 0x100000; !soc && size >= 0x40000; size >>= 1)
        if ((soc = memalign(0x1000, size)) && R_FAILED(socInit(soc, size))) {
            free(soc);
            break;
        } else soc_size = size;
    can_relaunch = relaunch_possible();
    if (!can_relaunch) printf("no hb:ldr: one job, then the launcher\n");
    mkdir(AGENT_DIR, 0777);

    u64 since = svcGetSystemTick(), heard = 0;
    int waits = 0;
    for (;;) {
        if (!serve_system()) leave("stopped");
        if (sock < 0) {
            if (ms_since(since) > GIVE_UP_S * 1000) leave("no PC");
            printf("\rconnecting...            ");
            if (net_connect(host, port) < 0) {
                printf("\rno answer (%d)           ", errno);
                for (int i = 0; i < 120; i++)
                    if (!serve_system()) leave("stopped");
                continue;
            }
            int n = snprintf(line, sizeof line, "HELLO 1 model=%s relaunch=%d heap=%u linear=%u\n",
                             ctr_is_new3ds() ? "new" : "old", can_relaunch, heap_free_kb(), (unsigned)(linearSpaceFree() >> 10));
            send_all(line, (size_t)n);
            heard = svcGetSystemTick();
            continue;
        }
        int r = rx_line(line, sizeof line);
        if (r < 0 || (r == 0 && ms_since(heard) > 15000)) { /* (the server says WAIT every 2 s) */
            net_close();
            continue;
        }
        if (r == 0) continue;
        heard = since = svcGetSystemTick();
        if (!strcmp(line, "QUIT")) leave("quit");
        if (!strcmp(line, "WAIT")) {
            printf("\rwaiting for a job %c       ", "|/-\\"[waits++ & 3]);
            continue;
        }
        unsigned len = 0;
        { /* PUT PATH BYTES ADLER32: a file for the card (a new agent build, in this one's place: hwrun.py update). Written
           * beside it and moved over it once whole and checked; then this one starts again, as after a job. */
            char path[160];
            unsigned long sum = 0;
            if (sscanf(line, "PUT %150s %u %lx", path, &len, &sum) == 3 && !strncmp(path, "sdmc:/", 6) && len && len <= 8u << 20) {
                char tmp[168];
                snprintf(tmp, sizeof tmp, "%s.new", path);
                FILE *f = fopen(tmp, "wb");
                unsigned got = 0;
                u32 a = 1, b = 0;
                u64 t = svcGetSystemTick();
                printf("\nreceiving %s (%u KB)\n", path, len >> 10);
                while (f && got < len && sock >= 0 && ms_since(t) < 15000) {
                    unsigned k = (unsigned)rxn < len - got ? (unsigned)rxn : len - got;
                    if (k) {
                        for (unsigned i = 0; i < k; i++) a = (a + (u8)rx[i]) % 65521, b = (b + a) % 65521;
                        if (fwrite(rx, 1, k, f) != k) break;
                        got += k, rxn -= (int)k;
                        memmove(rx, rx + k, (size_t)rxn);
                        t = svcGetSystemTick();
                    }
                    if (got < len && rx_fill() == 0) svcSleepThread(2000000);
                }
                int ok = f && got == len && (b << 16 | a) == sum;
                if (f) fclose(f);
                if (ok) remove(path), ok = rename(tmp, path) == 0;
                else remove(tmp);
                say("agent: %s: %u of %u bytes, %s\n", path, got, len, ok ? "in place" : "not taken");
                int n = snprintf(line, sizeof line, "status=%s relaunch=%d", ok ? "updated" : "refused reason=file not whole",
                                 ok ? relaunch_set() : 0);
                send_chunk('E', line, (unsigned)n);
                shutdown(sock, SHUT_WR);
                for (int i = 0; i < 30; i++) gspWaitForVBlank();
                net_close();
                if (ok) { gfxExit(); exit(0); }
                continue;
            }
        }
        if (sscanf(line, "JOB %31s %u", job.id, &len) != 2 || len > 256 * 1024) continue;
        char *text = malloc(len + 1);
        unsigned got = 0;
        u64 t = svcGetSystemTick();
        while (text && got < len && sock >= 0 && ms_since(t) < 10000) {
            unsigned k = (unsigned)rxn < len - got ? (unsigned)rxn : len - got;
            memcpy(text + got, rx, k);
            got += k, rxn -= (int)k;
            memmove(rx, rx + k, (size_t)rxn);
            if (got < len && rx_fill() == 0) svcSleepThread(2000000);
        }
        if (!text || got < len) {
            free(text);
            net_close();
            continue;
        }
        text[len] = 0;
        FILE *cfg = fopen(JOB_CFG, "w");
        int ncfg = 0;
        const char *bad = cfg ? job_parse(text, cfg, &ncfg) : "can't write " JOB_CFG;
        if (cfg) fclose(cfg);
        free(text);
        if (bad) { /* (nothing started yet: say why, and ask again) */
            say("agent: job %s refused: %s\n", job.id, bad);
            log_flush();
            int n = snprintf(line, sizeof line, "status=refused reason=%s", bad);
            send_chunk('E', line, (unsigned)n);
            net_close();
            for (int i = 0; i < 60; i++)
                if (!serve_system()) leave("stopped");
            continue;
        }
        say("agent: job %s: %ld frames, %d capture windows, %d switches, heap %u KB free, relaunch %d\n", job.id,
            job.frames, job.ncap, job.nset, heap_free_kb(), can_relaunch);
        if (job.quiet && soc) { /* (the PC told, the network down: finish brings it up again and resumes the job there) */
            send_chunk('Q', job.id, strlen(job.id));
            shutdown(sock, SHUT_WR);
            for (int i = 0; i < 20; i++) gspWaitForVBlank();
            net_close();
            socExit();
            /* (quiet 2: the radio off as well. With the socket service down the console stays on the access point,
             * and that alone is measured here) */
            if (job.quiet >= 2 && R_SUCCEEDED(nwmExtInit())) {
                Result r = NWMEXT_ControlWirelessEnabled(false);
                say("agent: wireless off (%08lx)\n", (unsigned long)r);
                nwmExtExit();
                radio_off = 1;
            }
            quiet_now = 1;
        }
        job.active = 1;
        for (int i = 0; i < job.nset; i++)
            if (job.set[i].frame <= 0) {
                say("agent: %s = %d before the start (was %d)\n", job.set[i].name, job.set[i].value, *job.set[i].p);
                *job.set[i].p = job.set[i].value;
            }
        /* the bottom screen back as the renderer expects it (the console made it RGB565, single-buffered), and its
         * two buffers' addresses for the capture */
        memset(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL), 0, 240 * 320 * 2);
        gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
        bottom_fb[1] = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        bottom_fb[0] = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        return ncfg ? JOB_CFG : NULL;
    }
}
#endif
