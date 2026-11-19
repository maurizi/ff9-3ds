/* 3DS platform: libctru services behind port/include/platform.h.
 *
 * The game owns the main loop (psx_run_game never returns); every virtual VBlank comes through plat_wait_frame,
 * which services the applet manager, reads the buttons, presents the frame and paces to the 3DS's VBlank.
 *
 * Memory: psx_ram is a bss array inside the executable's image (loaded at 0x00100000), so every OT-linked packet
 * the game builds there lies in the same 16 MB window and a 24-bit OT link rebuilds as (psx_ram & 0xFF000000) |
 * addr24 (gpu.c), as on the PC host.
 *
 * Data: sdmc:/3ds/ff9/ holds raw/FF9 CDn.img (or the packed discs.bin), as the PC's --data directory does.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "input_script.h"
#include "platform.h"
#include "psx_mem.h"
#include "psx_runtime.h"
#include "psx_spu.h"
#include "ctr.h"
#include "profile.h"

unsigned char psx_ram[PSX_RAM_SIZE] __attribute__((aligned(4096)));

static long frames;
static int is_new3ds;

/* ---- input ---------------------------------------------------------------------------------------------- */

/* 3DS keys -> PS1 digital pad bits (active high). Confirm is A, as on every Nintendo handheld; the circle pad
 * doubles the d-pad (FF9 reads a digital pad). ZL/ZR (New 3DS) are L2/R2; L+R together also gives L2+R2 (flee). */
static const struct { u32 key; unsigned int psx; } keymap[] = {
    {KEY_A, 0x4000},      /* cross */
    {KEY_B, 0x2000},      /* circle */
    {KEY_X, 0x1000},      /* triangle */
    {KEY_Y, 0x8000},      /* square */
    {KEY_L, 0x0400},      /* L1 */
    {KEY_R, 0x0800},      /* R1 */
    {KEY_ZL, 0x0100},     /* L2 */
    {KEY_ZR, 0x0200},     /* R2 */
    {KEY_SELECT, 0x0001},
    {KEY_START, 0x0008},
    {KEY_UP, 0x0010},
    {KEY_RIGHT, 0x0020},
    {KEY_DOWN, 0x0040},
    {KEY_LEFT, 0x0080},
};

static unsigned int pad_now;

static void read_input(void) {
    hidScanInput();
    u32 held = hidKeysHeld();
    unsigned int b = 0;
    for (unsigned i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
        if (held & keymap[i].key) b |= keymap[i].psx;
    if ((held & (KEY_L | KEY_R)) == (KEY_L | KEY_R)) b |= 0x0300;
    /* (the port's own screens may take the buttons first: port/game/field_hud.c, its options panel) */
    unsigned int port_pad_filter(unsigned int buttons);
    pad_now = port_pad_filter(b | ctr_ui_pad_buttons(frames) | input_script_buttons(frames));
}

unsigned int plat_pad_buttons(int port) {
    if (port != 0) return 0;
    return pad_now;
}

/* ---- frame pacing ----------------------------------------------------------------------------------------- */

/* The PS1 counts VBlanks at 59.94 Hz; the 3DS's LCDs refresh at 59.83 Hz. The game paces itself in VBlanks
 * (VSync(n)), so pacing to the 3DS's VBlank keeps it 0.2% slow, which nobody hears. VBlanks that passed while the
 * game worked count toward a VSync(n) (plat_vsync_begin), as on the PS1, so a slow frame doesn't wait again. */
static unsigned int vsync_owed;
static unsigned long long ctr_idle_us; /* waited for a VBlank (the heartbeat's) */
unsigned long long plat_time_us(void) { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }
unsigned int ctr_vblank_waits, ctr_vblank_skips;
static u32 next_vblank; /* the VBlank count (citro3d's) at which the current virtual frame ends */

void plat_vsync_begin(unsigned int n) {
    vsync_owed = n;
}

/* Watchdog: the main loop marks where it is (ctr_where) and counts its frames; a thread on another core logs the mark
 * and the frame when a frame hasn't ended for 3 seconds (a hang on hardware: where the game is stuck, even when the
 * HOME button no longer answers) */
volatile const char *ctr_where = "start";

/* A CPU exception on one of our threads (a data or prefetch abort, an undefined instruction): what it was and where,
 * into the log at once, before the system's handler (Luma's crash dump, which a crash can keep from being written:
 * the user's escape from a Memoria battle left neither dump nor log line). Then the system's handler as before. Its own
 * stack: the faulting thread's may be what went wrong. */
static u8 exception_stack[16 * 1024] __attribute__((aligned(8)));
static const char *const exception_names[4] = {"prefetch abort", "data abort", "undefined instruction", "VFP"};
static void __attribute__((noreturn)) on_exception(ERRF_ExceptionInfo *e, CpuRegisters *r) {
    static volatile int inside;
    if (!inside++) {
        u32 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        port_log("FATAL: %s in thread %lu at pc %08lx lr %08lx (address %08lx, status %08lx) sp %08lx, frame %ld, in %s\n",
                 e->type < 4 ? exception_names[e->type] : "exception", (unsigned long)tid, (unsigned long)r->pc,
                 (unsigned long)r->lr, (unsigned long)e->far, (unsigned long)e->fsr, (unsigned long)r->sp, (long)frames,
                 ctr_where ? (const char *)ctr_where : "?");
        port_log("FATAL: r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx r7 %08lx\n", (unsigned long)r->r[0],
                 (unsigned long)r->r[1], (unsigned long)r->r[2], (unsigned long)r->r[3], (unsigned long)r->r[4],
                 (unsigned long)r->r[5], (unsigned long)r->r[6], (unsigned long)r->r[7]);
        int port_game_mode(void);
        int port_field_id(void);
        port_log("FATAL: mode %d field %d\n", port_game_mode(), port_field_id());
        void ctr_trace_dump(void);
        ctr_trace_dump();
    }
    svcBreak(USERBREAK_PANIC);
    for (;;) {}
}
/* (per thread: platform.c's main, async.c's worker, spu_client.c's sound thread) */
void ctr_exception_install(void) {
    threadOnException(on_exception, exception_stack + sizeof exception_stack, WRITE_DATA_TO_HANDLER_STACK);
}
static volatile unsigned wd_frames;
static volatile int watchdog_quit, watchdog_done, watchdog_on, watchdog_paused;
static LightEvent watchdog_wake;
/* (render.c ctr_shutdown: the thread woken and gone before the process's memory, its stack's, is freed. A dump it is
 * writing is cut short: an Old 3DS closed from the HOME Menu after 3 s there (no frames: the watchdog's dump of
 * thousands of lines, at core 1's 5% while suspended) went on past the 100 ms waited, and the system stopped with a
 * kernel panic on core 1 as the process ended under it) */
void ctr_watchdog_stop(void) {
    if (!watchdog_on) return;
    extern volatile int ctr_trace_quit;
    ctr_trace_quit = 1;
    watchdog_quit = 1;
    LightEvent_Signal(&watchdog_wake);
    for (int i = 0; i < 2000 && !watchdog_done; i++) svcSleepThread(1000000);
}
/* (suspended in the HOME Menu or asleep, the game draws no frames: nothing to report) */
static aptHookCookie watchdog_hook;
static void watchdog_event(APT_HookType t, void *unused) {
    (void)unused;
    if (t == APTHOOK_ONSUSPEND || t == APTHOOK_ONSLEEP) watchdog_paused = 1;
    else if (t == APTHOOK_ONRESTORE || t == APTHOOK_ONWAKEUP) watchdog_paused = 2; /* (counted afresh) */
}
static void watchdog_main(void *unused) {
    (void)unused;
    unsigned last = 0, still = 0;
    for (;;) {
        LightEvent_WaitTimeout(&watchdog_wake, 1000000000LL);
        if (watchdog_quit) break;
        if (watchdog_paused == 1) continue;
        if (watchdog_paused == 2) watchdog_paused = 0, last = wd_frames, still = 0;
        unsigned f = wd_frames;
        if (f != last) { last = f, still = 0; continue; }
        if (++still % 3 == 0) {
            char line[160];
            int n = snprintf(line, sizeof line, "WATCHDOG: no frame for %us at frame %u, in %s\n", still, f, (const char *)ctr_where);
            void ctr_console_raw(const char *, int);
            ctr_console_raw(line, n);
            if (still == 3) { void ctr_trace_dump(void); ctr_trace_dump(); }
        }
#ifdef PORT_AGENT
        { void ctr_agent_hang(unsigned); ctr_agent_hang(still); } /* (a hung job ends: its log to the PC, the next job) */
#endif
    }
    watchdog_done = 1;
}
void ctr_watchdog_start(void) {
    static Thread t;
    if (t) return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    LightEvent_Init(&watchdog_wake, RESET_ONESHOT);
    t = threadCreate(watchdog_main, NULL, 8 * 1024, prio - 3, ctr_is_new3ds() ? 2 : 1, true);
    watchdog_on = t != NULL;
    if (watchdog_on) aptHook(&watchdog_hook, watchdog_event, NULL);
}

void plat_wait_frame(void) {
    frames++;
    wd_frames = (unsigned)frames;
    ctr_where = "game frame";
#ifdef PORT_PROBE_AT
    { extern int psx_trace_gpu; psx_trace_gpu = frames >= PORT_PROBE_AT - 5 && frames < PORT_PROBE_AT; }
    { extern unsigned int psx_probe_top; if (frames >= PORT_PROBE_AT - 10 && frames <= PORT_PROBE_AT) port_log("top polys f%ld: %u\n", frames, psx_probe_top); psx_probe_top = 0; }
#endif
    if (!aptMainLoop()) {
        ctr_shutdown();
        exit(0);
    }
    read_input();
    if (frames % 120 == 0) { /* (a heartbeat in the log: where a hang on hardware happens) */
        extern unsigned ctr_async_jobs, ctr_async_done, psx_cd_sectors_read, psx_cd_last_lba, psx_cd_read_starts;
        int port_game_mode(void);
        int port_field_id(void);
        extern unsigned int psx_cd_debug[];
        extern unsigned int ctr_audio_dropped, ctr_audio_underruns, ctr_audio_frames, ctr_audio_peak;
        static u64 last_tick;
        u64 tick = svcGetSystemTick();
        unsigned ms = last_tick ? (unsigned)((tick - last_tick) / (SYSCLOCK_ARM11 / 1000)) : 0;
        last_tick = tick;
        int ctr_texture_pages(void); /* (memory over a long session: linear, VRAM, the texture cache's pages) */
        port_log("beat %ld: %u ms, mode %d field %d, worker jobs %u/%u, presents %u, cd %u reads %u sectors (last %u) state %03x q %u, "
                 "audio %u frames peak %u dropped %u underruns %u, linear %u KB VRAM %u KB pages %d\n",
                 frames, ms, port_game_mode(), port_field_id(), ctr_async_done, ctr_async_jobs, ctr_frames_presented(),
                 psx_cd_read_starts, psx_cd_sectors_read, psx_cd_last_lba, psx_cd_debug[4], psx_cd_debug[5],
                 ctr_audio_frames, ctr_audio_peak, ctr_audio_dropped, ctr_audio_underruns, (unsigned)(linearSpaceFree() >> 10),
                 (unsigned)(vramSpaceFree() >> 10), ctr_texture_pages());
        ctr_audio_peak = 0;
        { /* (the scene's walks sent to the other core since the last beat, and those of them finished here: gpu.c) */
            extern unsigned int psx_gpu_deferred, psx_gpu_defer_helped;
            static unsigned int d0, h0;
            extern volatile unsigned long long psx_gpu_defer_us[3];
            static unsigned long long u0[3], idle0;
            const unsigned n = psx_gpu_deferred - d0;
            if (n) /* (per walk: what the other core walked, the wait for it to stop, the rest walked here; and the time
                    * this thread waited for a VBlank, of the beat) */
                port_log("walks: %u on the other core, %u finished here; a walk: %u us there, %u us to stop it, %u us here; idle %u ms\n", n,
                         psx_gpu_defer_helped - h0, (unsigned)((psx_gpu_defer_us[0] - u0[0]) / n), (unsigned)((psx_gpu_defer_us[1] - u0[1]) / n),
                         (unsigned)((psx_gpu_defer_us[2] - u0[2]) / n), (unsigned)((ctr_idle_us - idle0) / 1000));
            d0 = psx_gpu_deferred, h0 = psx_gpu_defer_helped, idle0 = ctr_idle_us;
            for (int i = 0; i < 3; i++) u0[i] = psx_gpu_defer_us[i];
        }
        void port_world_where(void);
        port_world_where(); /* (on the world map: where, for a spot to rebuild) */
    }
    void port_voice_poll(void);
    port_voice_poll();
    void port_bottom_hud(void);
    port_bottom_hud();
    void port_battle_ui_frame(void);
    port_battle_ui_frame();
    ctr_present(frames);
#ifdef PORT_DUMP_EVERY
    if (frames % PORT_DUMP_EVERY == 0) {
        void ctr_render_dump_display(long);
        ctr_render_dump_display(frames);
        /* the game's whole state as a hash (PS1 RAM and scratchpad): two runs of the same build match here, so a change
         * in what it does shows at the first dump after it. Only the same build: the game's native pointers (into
         * psx_ram, whose place moves with the build's layout, and into the program) differ between builds, and can't be
         * told from data reliably. Across builds, compare the PC host's RAM dumps (--ram-dump: psx_ram at a fixed
         * address) */
        u32 h = 0x811C9DC5u;
        for (const u32 *w = (const u32 *)psx_ram; w < (const u32 *)(psx_ram + PSX_RAM_SIZE); w++) h = (h ^ *w) * 0x01000193u;
        for (const u32 *w = (const u32 *)psx_scratchpad; w < (const u32 *)(psx_scratchpad + 0x400); w++) h = (h ^ *w) * 0x01000193u;
        port_log("ramhash %ld %08lx\n", frames, (unsigned long)h);
    }
#endif
#ifdef PORT_DUMP_AT
    if (frames == PORT_DUMP_AT) { void ctr_render_dump(void); ctr_render_dump(); }
#endif
#ifdef PORT_AGENT
    { void ctr_agent_frame(long); ctr_agent_frame(frames); } /* (the job's switches, captures and end: agent.c) */
#endif
    /* each virtual frame ends at its own VBlank: wait for it unless the game is already late (then catch up, but
     * never by more than 4: after a long stall, start again from now) */
#ifndef PORT_SOFT_PRESENT
    u32 now = C3D_FrameCounter(0);
    if ((s32)(now - next_vblank) < 0) {
        unsigned int w = port_profile_wait_begin();
        ctr_vblank_waits++;
        /* (the wait used for a movie's macroblocks decoded ahead, one at a time, while there are any: movie.c) */
        int port_idle_step(void);
        const unsigned long long idle_t0 = plat_time_us();
        while ((s32)(C3D_FrameCounter(0) - next_vblank) < 0)
            if (!port_idle_step()) gspWaitForVBlank();
        ctr_idle_us += plat_time_us() - idle_t0;
        port_profile_wait_end(w);
    } else {
        ctr_vblank_skips++;
        if (now - next_vblank > 4) next_vblank = now;
    }
    next_vblank++;
#else
    unsigned int w = port_profile_wait_begin();
    gspWaitForVBlank();
    port_profile_wait_end(w);
#endif
    ctr_profile_frame();
}

/* ---- misc ------------------------------------------------------------------------------------------------- */

void plat_vlog(const char *fmt, va_list ap) {
    ctr_console_vlog(fmt, ap);
}

void plat_halt(void) {
#ifdef PORT_AGENT
    { void ctr_agent_end(const char *); ctr_agent_end("fatal"); } /* (a job: its log to the PC, then the next) */
#endif
    ctr_console_show();
    printf("\nstopped. START: exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    ctr_shutdown();
    exit(2);
}

void plat_video_init(void) {
    ctr_video_init();
}

/* path and each folder above it that isn't there: a CIA install needs nothing on the card but the DSP firmware, so
 * /3ds/ff9 itself may be missing, and mkdir of its cards folder alone failed: the first save stayed in memory, the
 * game said it had saved, and it was gone once the app closed (a player's report) */
void plat_mkdir(const char *path) {
    char p[256];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = strchr(p, ':') ? strchr(p, ':') + 2 : p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0777); *c = '/'; }
    mkdir(p, 0777);
}

const char *plat_data_dir(void) {
    return "sdmc:/3ds/ff9";
}

/* Where the disc pack is when the build carries it: the single-file build (make 3ds-full) has it in its own romfs;
 * any other build (a netloaded test build has no romfs) uses the one inside ff9-full.3dsx on the card, if there, so
 * the card needs only one copy of the discs. NULL: the data folder's discs.bin or images (cd.c). */
static const char *disc_pack;

static void find_disc_pack(void) {
    struct stat st;
    if (R_SUCCEEDED(romfsInit()) && stat("romfs:/discs.bin", &st) == 0) {
        port_log("data: the disc pack in the app\n");
        disc_pack = "romfs:/discs.bin";
        return;
    }
    static const char *const full[] = {"/3ds/ff9-full.3dsx", "/3ds/ff9/ff9-full.3dsx"};
    for (unsigned i = 0; i < sizeof full / sizeof *full; i++) {
        Handle fd;
        u32 got = 0, hdr[11];
        if (R_FAILED(FSUSER_OpenFileDirectly(&fd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                             fsMakePath(PATH_ASCII, full[i]), FS_OPEN_READ, 0)))
            continue;
        /* the 3DSX header (0x20 bytes) and its extended header: SMDH offset, SMDH size, romfs offset (+0x28) */
        if (R_FAILED(FSFILE_Read(fd, &got, 0, hdr, sizeof hdr)) || got != sizeof hdr || hdr[0] != 0x58534433u ||
            (hdr[1] & 0xFFFF) < 0x2C || !hdr[10] || R_FAILED(romfsMountFromFile(fd, hdr[10], "full"))) {
            FSFILE_Close(fd);
            continue;
        }
        if (stat("full:/discs.bin", &st) == 0) { /* (the mount keeps the handle) */
            port_log("data: the disc pack in sdmc:%s\n", full[i]);
            disc_pack = "full:/discs.bin";
            return;
        }
        romfsUnmount("full");
    }
}

const char *plat_disc_pack(void) {
    return disc_pack;
}

long ctr_frames(void) {
    return frames;
}

int ctr_is_new3ds(void) {
    return is_new3ds;
}

int main(void) {
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    is_new3ds = n3ds;
    if (is_new3ds) osSetSpeedupEnable(true); /* 804 MHz and the L2 cache */
    gfxInitDefault();
    plat_mkdir(plat_data_dir()); /* (sdmc:/3ds/ff9: its log, settings and memory cards) */
    ctr_console_init();
    ctr_exception_install();
#ifdef PORT_AGENT
    const char *ctr_agent_start(void);
    const char *agent_cfg = ctr_agent_start(); /* (waits for a job from the PC: agent.c) */
#endif
    find_disc_pack();
    port_log("ff9-3ds (%s 3DS)\n", is_new3ds ? "New" : "Old");
    ctr_audio_init();
    ctr_async_init();
    { void ctr_watchdog_start(void); ctr_watchdog_start(); }
#ifdef PORT_AUTOPRESS
    input_script_add_list(PORT_AUTOPRESS);
#endif
#ifdef PORT_SCENARIO
    port_scenario_load(PORT_SCENARIO);
#endif
#ifdef PORT_AGENT
    if (agent_cfg) port_scenario_load(agent_cfg);
#endif
    psx_spu_set_rendering(1);
    psx_run_game();
    return 0;
}
