/* 3DS platform: libctru services behind port/include/platform.h.
 *
 * The game owns the main loop (psx_run_game never returns); every virtual VBlank comes through plat_wait_frame,
 * which services the applet manager, reads the buttons, presents the frame and paces to the 3DS's VBlank.
 *
 * Memory: psx_ram is a bss array inside the executable's image (loaded at 0x00100000), so every OT-linked packet
 * the game builds there lies in the same 16 MB window and a 24-bit OT link rebuilds as (psx_ram & 0xFF000000) |
 * addr24 (gpu.c), as on the PC host.
 *
 * Data: sdmc:/3ds/ff9/ (another release's: ff9-es/, ...: FF9_SD_NAME) holds raw/FF9 CDn.img (or the packed discs.bin), as the PC's --data directory does.
 */
#include <3ds.h>
#include "port_buttons.h"
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
    {KEY_A, PORT_PAD_CONFIRM}, /* cross (JP: circle) */
    {KEY_B, PORT_PAD_CANCEL},  /* circle (JP: cross) */
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
/* (the beat's four longest spans of the game's own time between two VBlanks' ends, and of a present: the heartbeat's) */
static struct { unsigned us; long frame; } ctr_long[2][4];
static unsigned long long ctr_exit_at;
static void long_note(int k, unsigned long long us, long frame) {
    int m = 0;
    for (int i = 1; i < 4; i++) if (ctr_long[k][i].us < ctr_long[k][m].us) m = i;
    if (us > ctr_long[k][m].us) ctr_long[k][m].us = (unsigned)us, ctr_long[k][m].frame = frame;
}
static unsigned long long ctr_present_us, ctr_hud_us, ctr_plat_us; /* (... and plat_wait_frame's whole time) */
unsigned long long plat_time_us(void);
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

int ctr_tl_from = -1, ctr_tl_to = -1;
static struct { unsigned us; const char *what; int v; long frame; } tl_ring[4096];
static volatile unsigned tl_n;
void port_tl(const char *what, int v) {
    if (frames < ctr_tl_from || frames > ctr_tl_to || tl_n >= 4096) return;
    const unsigned i = __atomic_fetch_add(&tl_n, 1, __ATOMIC_RELAXED);
    if (i >= 4096) return;
    tl_ring[i].us = (unsigned)plat_time_us(), tl_ring[i].what = what, tl_ring[i].v = v, tl_ring[i].frame = frames;
}
static void tl_dump(void) {
    static int done;
    if (done || ctr_tl_to < 0 || frames <= ctr_tl_to) return;
    done = 1;
    const unsigned n = tl_n < 4096 ? tl_n : 4096;
    for (unsigned i = 0; i < n; i++)
        port_log("tl %ld %u +%u %s %d\n", tl_ring[i].frame, tl_ring[i].us - tl_ring[0].us, i ? tl_ring[i].us - tl_ring[i - 1].us : 0, tl_ring[i].what, tl_ring[i].v);
}
void plat_wait_frame(void) {
    const unsigned long long pf0 = plat_time_us();
    { void psx_span_mark(int); psx_span_mark(4); }
    if (ctr_exit_at) long_note(0, pf0 - ctr_exit_at, frames); /* (the game's own time since the last VBlank's end) */
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
            { /* (the GX commands sent and their interrupts: render.c) */
                extern volatile unsigned ctr_gx_commands, ctr_gx_interrupts;
                static unsigned c0, i0;
                extern volatile unsigned ctr_gx_cmd_kind[8], ctr_gx_irq_kind[4];
                static unsigned k0[8], q0[4];
                { extern unsigned ctr_flushes, ctr_flush_failed, ctr_flush_error; static unsigned f0, x0;
                  port_log("flush: %u calls, %u refused (last result %08x)\n", ctr_flushes - f0, ctr_flush_failed - x0, ctr_flush_error);
                  f0 = ctr_flushes, x0 = ctr_flush_failed; }
                port_log("gx: %u commands, %u interrupts | commands: lists %u, fills %u, transfers %u, copies %u, others %u | "
                         "interrupts: fill %u, transfer/copy %u, list %u, dma %u\n", ctr_gx_commands - c0, ctr_gx_interrupts - i0,
                         ctr_gx_cmd_kind[1] - k0[1], ctr_gx_cmd_kind[2] - k0[2], ctr_gx_cmd_kind[3] - k0[3], ctr_gx_cmd_kind[4] - k0[4],
                         ctr_gx_cmd_kind[0] - k0[0] + ctr_gx_cmd_kind[5] - k0[5], ctr_gx_irq_kind[0] - q0[0], ctr_gx_irq_kind[1] - q0[1],
                         ctr_gx_irq_kind[2] - q0[2], ctr_gx_irq_kind[3] - q0[3]);
                c0 = ctr_gx_commands, i0 = ctr_gx_interrupts;
                for (int i = 0; i < 8; i++) k0[i] = ctr_gx_cmd_kind[i];
                for (int i = 0; i < 4; i++) q0[i] = ctr_gx_irq_kind[i];
            }
            extern volatile unsigned long long psx_gpu_defer_us[4];
            static unsigned long long u0[4], idle0;
            const unsigned n = psx_gpu_deferred - d0;
            if (n) /* (per walk: what the other core walked, the wait for it to stop, the rest walked here; and the time
                    * this thread waited for a VBlank, of the beat) */
                port_log("walks: %u on the other core, %u finished here; a walk: %u us there, %u us to stop it, %u us here; idle %u ms\n", n,
                         psx_gpu_defer_helped - h0, (unsigned)((psx_gpu_defer_us[0] - u0[0]) / n), (unsigned)((psx_gpu_defer_us[1] - u0[1]) / n),
                         (unsigned)((psx_gpu_defer_us[2] - u0[2]) / n), (unsigned)((ctr_idle_us - idle0) / 1000));
            { static unsigned long long pl0; /* (the rest of this thread's beat: the calls kept for the sync, and everything else here) */
              extern volatile unsigned long long psx_irq_us; extern unsigned int psx_gpu_early_frames;
              static unsigned long long iq0; static unsigned ef0;
              if (n) port_log("this thread: the kept calls %u us a walk; the VBlanks' interrupt work %u ms of the beat (%u VBlanks' while DrawSync "
                              "waited); this function %u ms (idle, presents and UI within)\n", (unsigned)((psx_gpu_defer_us[3] - u0[3]) / n),
                              (unsigned)((psx_irq_us - iq0) / 1000), psx_gpu_early_frames - ef0, (unsigned)((ctr_plat_us - pl0) / 1000));
              pl0 = ctr_plat_us, iq0 = psx_irq_us, ef0 = psx_gpu_early_frames; }
            { extern unsigned ctr_tex_n[6]; static unsigned t0[6];
              if (ctr_tex_n[0] != t0[0])
                  port_log("textures: %u blocks converted (%u new to their slot); pages moved: %u to a slot of their own, %u to a free one, "
                           "%u to another page's; %u waits for the frame in flight\n", ctr_tex_n[0] - t0[0], ctr_tex_n[1] - t0[1], ctr_tex_n[2] - t0[2],
                           ctr_tex_n[3] - t0[3], ctr_tex_n[4] - t0[4], ctr_tex_n[5] - t0[5]);
              for (int i = 0; i < 6; i++) t0[i] = ctr_tex_n[i]; }
            { extern unsigned ctr_ui_same; static unsigned s0;
              if (ctr_ui_same != s0) port_log("presents not made (the UI drawn again as it was): %u\n", ctr_ui_same - s0);
              s0 = ctr_ui_same; }
            { extern unsigned ctr_late_skips; static unsigned l0;
              if (ctr_late_skips != l0) port_log("presents left out (late, the UI's alone): %u\n", ctr_late_skips - l0);
              l0 = ctr_late_skips; }
            { extern volatile unsigned long long ctr_gpu_us; static unsigned long long g0;
              port_log("the GPU busy %u ms of the beat (each frame's end to its queue done)\n", (unsigned)((ctr_gpu_us - g0) / 1000));
              g0 = ctr_gpu_us; }
            { extern unsigned ctr_tex_why[7], ctr_batches; static unsigned w0[7], b0;
              port_log("blocks converted by cause: %u new to the slot, %u texels and CLUT, %u texels, %u CLUT; by depth: %u 4-bit, %u 8-bit, %u 15-bit; "
                       "%u batches drawn\n", ctr_tex_why[0] - w0[0], ctr_tex_why[1] - w0[1], ctr_tex_why[2] - w0[2], ctr_tex_why[3] - w0[3],
                       ctr_tex_why[4] - w0[4], ctr_tex_why[5] - w0[5], ctr_tex_why[6] - w0[6], ctr_batches - b0);
              for (int i = 0; i < 7; i++) w0[i] = ctr_tex_why[i];
              b0 = ctr_batches; }
            { extern unsigned ctr_sync_line[8], ctr_sync_n[8]; char b[160]; int o = 0; /* (the GPU waited for mid-frame: render.c's lines) */
              for (int i = 0; i < 8; i++) {
                  if (ctr_sync_n[i]) o += snprintf(b + o, sizeof b - o, " %u:%u", ctr_sync_line[i], ctr_sync_n[i]);
                  ctr_sync_n[i] = 0;
              }
              if (o) port_log("syncs (render.c line:count):%s\n", b); }
            { extern unsigned int psx_sync_line[8], psx_sync_n[8]; char b[160]; int o = 0; /* (gpu.c: psx_gpu_sync) */
              for (int i = 0; i < 8; i++) {
                  if (psx_sync_n[i]) o += snprintf(b + o, sizeof b - o, " %u:%u", psx_sync_line[i], psx_sync_n[i]);
                  psx_sync_n[i] = 0;
              }
              if (o) port_log("walks finished on this thread, by what asked (gpu.c line:count, 0 elsewhere):%s\n", b); }
            { extern unsigned ctr_rb_n[7]; extern short ctr_rb_rect[7][4]; char b[240]; int o = 0; /* (render.c: RB_CAUSE) */
              for (int i = 0; i < 7; i++) {
                  if (ctr_rb_n[i]) o += snprintf(b + o, sizeof b - o, " %d:%u (%d,%d-%d,%d)", i, ctr_rb_n[i], ctr_rb_rect[i][0], ctr_rb_rect[i][1],
                                                 ctr_rb_rect[i][2], ctr_rb_rect[i][3]);
                  ctr_rb_n[i] = 0;
              }
              if (o) port_log("readbacks (cause:count, its last rectangle):%s\n", b); }
            { extern unsigned ctr_bg_replays, ctr_bg_records; static unsigned r0, k0;
              if (ctr_bg_replays != r0 || ctr_bg_records != k0)
                  port_log("background runs: %u drawn from their copies, %u kept anew\n", ctr_bg_replays - r0, ctr_bg_records - k0);
              r0 = ctr_bg_replays, k0 = ctr_bg_records; }
            { static unsigned long long p0, hd0;
              port_log("presents %u ms, the bottom screen's and battle's UI %u ms of the beat\n", (unsigned)((ctr_present_us - p0) / 1000), (unsigned)((ctr_hud_us - hd0) / 1000));
              p0 = ctr_present_us, hd0 = ctr_hud_us; }
            { char b[200]; int o = 0;
              for (int k = 0; k < 2; k++) {
                  o += snprintf(b + o, sizeof b - o, k ? "; presents" : " the game's own time");
                  for (int i = 0; i < 4; i++) o += snprintf(b + o, sizeof b - o, " %u at %ld", ctr_long[k][i].us / 1000, ctr_long[k][i].frame);
              }
              port_log("the beat's longest, ms at frame:%s\n", b);
              { extern unsigned int psx_gpu_pauses, psx_gpu_pause_syncs, psx_gpu_sync_ret[4]; static unsigned p0, q0;
                if (psx_gpu_pauses != p0 || psx_gpu_pause_syncs != q0)
                    port_log("walks paused and resumed: %u; syncs asked while one was paused: %u; the last walks ended from elsewhere by %08x %08x %08x %08x\n",
                             psx_gpu_pauses - p0, psx_gpu_pause_syncs - q0, psx_gpu_sync_ret[0], psx_gpu_sync_ret[1], psx_gpu_sync_ret[2], psx_gpu_sync_ret[3]);
                p0 = psx_gpu_pauses, q0 = psx_gpu_pause_syncs; }
              { extern unsigned long long psx_gpu_pause_us[4]; static unsigned long long u0[4];
                if (psx_gpu_pause_us[2] != u0[2])
                    port_log("pauses, ms of the beat: %u waiting for the GPU before, %u for the worker to stop, %u paused, %u resuming\n",
                             (unsigned)((psx_gpu_pause_us[0] - u0[0]) / 1000), (unsigned)((psx_gpu_pause_us[1] - u0[1]) / 1000),
                             (unsigned)((psx_gpu_pause_us[2] - u0[2]) / 1000), (unsigned)((psx_gpu_pause_us[3] - u0[3]) / 1000));
                for (int i = 0; i < 4; i++) u0[i] = psx_gpu_pause_us[i]; }
              { extern unsigned long long psx_span_us[5]; static unsigned long long s0[5];
                port_log("this thread's ms of the beat: %u before the scene's walk begins, %u from there to DrawSync, %u in DrawSync, %u after it, %u at the VBlanks' ends\n",
                         (unsigned)((psx_span_us[0] - s0[0]) / 1000), (unsigned)((psx_span_us[1] - s0[1]) / 1000), (unsigned)((psx_span_us[2] - s0[2]) / 1000),
                         (unsigned)((psx_span_us[3] - s0[3]) / 1000), (unsigned)((psx_span_us[4] - s0[4]) / 1000));
                for (int i = 0; i < 5; i++) s0[i] = psx_span_us[i]; }
              memset(ctr_long, 0, sizeof ctr_long); }
            { extern int psx_packet_timing; extern unsigned int psx_packet_us[2][2][256], psx_packet_n[2][2][256];
              if (psx_packet_timing) {
                  for (int th = 0; th < 2; th++) {
                      char b[400]; int o = 0; unsigned tot = 0, totn = 0;
                      for (int k = 0; k < 512; k++) tot += psx_packet_us[th][k >> 8][k & 255], totn += psx_packet_n[th][k >> 8][k & 255];
                      for (int r = 0; r < 12; r++) {
                          int m = 0;
                          for (int k = 1; k < 512; k++) if (psx_packet_us[th][k >> 8][k & 255] > psx_packet_us[th][m >> 8][m & 255]) m = k;
                          if (!psx_packet_us[th][m >> 8][m & 255]) break;
                          o += snprintf(b + o, sizeof b - o, " %02x%s %u ms (%u)", m & 255, m >> 8 ? "o" : "", psx_packet_us[th][m >> 8][m & 255] / 1000, psx_packet_n[th][m >> 8][m & 255]);
                          psx_packet_us[th][m >> 8][m & 255] = 0;
                      }
                      port_log("packets on %s, by command (o: into an area at y 256 or under): %u ms (%u):%s\n", th ? "the worker" : "this thread", tot / 1000, totn, b);
                  }
                  memset(psx_packet_us, 0, sizeof psx_packet_us), memset(psx_packet_n, 0, sizeof psx_packet_n);
              } }
            { /* (the walk's parts, when they are timed: render.c ctr_sw_timing) */
                extern int ctr_sw_timing; extern unsigned long long ctr_part_us[8]; extern unsigned ctr_part_n[8];
                static unsigned long long pu[8]; static unsigned pn[8];
                if (ctr_sw_timing)
                    port_log("parts, ms and calls a beat: texture blocks %u (%u), submits %u (%u), GPU waits %u (%u), background runs %u (%u), "
                             "chains %u (%u, their texture blocks within)\n", (unsigned)((ctr_part_us[0] - pu[0]) / 1000), ctr_part_n[0] - pn[0],
                             (unsigned)((ctr_part_us[1] - pu[1]) / 1000), ctr_part_n[1] - pn[1], (unsigned)((ctr_part_us[2] - pu[2]) / 1000),
                             ctr_part_n[2] - pn[2], (unsigned)((ctr_part_us[3] - pu[3]) / 1000), ctr_part_n[3] - pn[3],
                             (unsigned)((ctr_part_us[4] - pu[4]) / 1000), ctr_part_n[4] - pn[4]);
                if (ctr_sw_timing)
                    port_log("parts: texel loops %u ms (%u); flushes %u ms (%u, %u KB)\n", (unsigned)((ctr_part_us[5] - pu[5]) / 1000),
                             ctr_part_n[5] - pn[5], (unsigned)((ctr_part_us[7] - pu[7]) / 1000), ctr_part_n[7] - pn[7],
                             (unsigned)((ctr_part_us[6] - pu[6]) / 1024));
                for (int i = 0; i < 8; i++) pu[i] = ctr_part_us[i], pn[i] = ctr_part_n[i];
            }
            d0 = psx_gpu_deferred, h0 = psx_gpu_defer_helped, idle0 = ctr_idle_us;
            for (int i = 0; i < 4; i++) u0[i] = psx_gpu_defer_us[i];
        }
        void port_world_where(void);
        port_world_where(); /* (on the world map: where, for a spot to rebuild) */
    }
    void port_voice_poll(void);
    port_voice_poll();
    /* (a battle's walk still on the other core: paused for this VBlank's own drawing and its present, then on) */
    int psx_gpu_pause(void); void psx_gpu_resume(void);
    const int walk_paused = psx_gpu_pause();
    {
        const unsigned long long t0 = plat_time_us();
        void port_bottom_hud(void);
        port_bottom_hud();
        void port_battle_ui_frame(void);
        port_battle_ui_frame();
        ctr_hud_us += plat_time_us() - t0;
    }
    { /* (the present's time on this thread, for the heartbeat) */
        const unsigned long long t0 = plat_time_us();
        { extern int ctr_present_late; ctr_present_late = (s32)(C3D_FrameCounter(0) - next_vblank) >= 1; }
        port_tl("present", (int)(C3D_FrameCounter(0) - next_vblank));
        ctr_present(frames);
        port_tl("present done", 0);
#ifdef PORT_PXWATCH
        { extern long ctr_pxwatch_frame; void ctr_pxwatch_end(void); ctr_pxwatch_frame = frames + 1; ctr_pxwatch_end(); }
#endif
        ctr_present_us += plat_time_us() - t0;
        long_note(1, plat_time_us() - t0, frames);
    }
    if (walk_paused) psx_gpu_resume();
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
    port_tl("VBlank's end", (int)(C3D_FrameCounter(0) - next_vblank));
    tl_dump();
#else
    unsigned int w = port_profile_wait_begin();
    gspWaitForVBlank();
    port_profile_wait_end(w);
#endif
    ctr_profile_frame();
    ctr_plat_us += plat_time_us() - pf0;
    ctr_exit_at = plat_time_us();
    { void psx_span_mark(int); psx_span_mark(0); }
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
    return "sdmc:/3ds/" FF9_SD_NAME;
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
    static const char *const full[] = {"/3ds/" FF9_SD_NAME "-full.3dsx", "/3ds/" FF9_SD_NAME "/" FF9_SD_NAME "-full.3dsx"};
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
