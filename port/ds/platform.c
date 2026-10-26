/* DS platform: entry point, console, frame pacing. */
#include "blackbox.h"
#include <fat.h>
#include <nds.h>
#include "audio.h"
#include <stdio.h>

#include "display.h"
#include "input_script.h"
#include "platform.h"
#include "psx_mem.h"
#include "psx_runtime.h"
#include "profile.h"

unsigned char psx_ram[PSX_RAM_SIZE] __attribute__((aligned(32)));

static long frames;
#ifdef PORT_PC_SAMPLER
volatile u32 port_pc_frame; /* (pc_sampler.c counts from a frame) */
#endif

/* Keep each status row inside 31 columns: writing the final column wraps and scrolls the console. */
static void status_row(int row, const char *fmt, ...) {
    char text[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    printf("\x1b[%d;0H%-31s", row, text);
}

/* libcd's debug snapshot and the game's CD state, shown on the status line (port/psx/cd.c) */
extern unsigned int psx_cd_debug[6];
extern unsigned char g_cdLoad[];

extern unsigned char *g_gameState; /* PS1 0x8006794C: GameState * */
extern int psx_gx_stats_polys, psx_gx_stats_dropped;
extern int psx_gx_stats_texture_fail, psx_gx_stats_budget_fail;
extern int psx_gx_stats_composited, psx_gx_stats_blend_fallback;
extern int psx_gx_fail_reason[8];

/* The ARM9 stack is the 16 KiB DTCM, and deep game call chains (battles, with the SPU running inside VSync)
 * come close to filling it; an overflow corrupts main RAM below 0x02FF4000 silently. Paint the free part at boot
 * and measure what is still untouched: the status line shows it, and states expose port_stack_free_min. */
extern char __sbss_end[];
#define STACK_PAINT 0x5AC0FFEEu
volatile unsigned int port_stack_free_min = ~0u;
static __attribute__((noinline)) void stack_paint(void) {
    unsigned int sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    for (unsigned int *p = (unsigned int *)(((unsigned int)__sbss_end + 3) & ~3u); (unsigned int)p < sp - 256; ++p)
        *p = STACK_PAINT;
}
static unsigned int stack_free(void) {
    const unsigned int *start = (const unsigned int *)(((unsigned int)__sbss_end + 3) & ~3u), *p = start;
    while (*p == STACK_PAINT) ++p;
    return (unsigned int)((const char *)p - (const char *)start);
}

#ifdef PORT_CANARY
/* CANARY=1: the BIOS IRQ handler jumps through the word at the top of DTCM; a corrupt value there crashes the
 * next interrupt far from the culprit. Check it between phases and stop at the first phase that broke it. */
void IntrMain(void);
extern unsigned short *psx_vram_tail;
extern unsigned char *psx_spu_ram_tail;
static int tail_intact(const void *tail) {
    const unsigned int *p = tail;
    if (!p) return 1;
#ifdef PORT_DS_MODE
    for (unsigned int i = 0; i < 0x2000 / 4; ++i) /* (mem.c SHADOW_TAIL) */
#else
    for (unsigned int i = 0; i < 0x8000 / 4; ++i)
#endif
        if (p[i] != 0xA5A5A5A5u) {
            printf("\ntail %p word %u: %08x\n", tail, i, p[i]);
            return 0;
        }
    return 1;
}
void port_canary(const char *where) {
    static const char *previous = "boot";
    volatile unsigned int *vector = (volatile unsigned int *)0x02FF7FFC;
    if (*vector != (unsigned int)IntrMain || !tail_intact(psx_vram_tail) || !tail_intact(psx_spu_ram_tail)) {
        unsigned int bad = *vector;
        REG_IME = 0;
        printf("\nIRQ vector corrupt: %08x\nfirst seen after: %s\nprevious check: %s\nframe %ld\n", bad, where,
               previous, frames);
        for (;;) {}
    }
    previous = where;
}
#endif

#ifdef PORT_HW_SOUND
#include "audio_hw.h"
#endif

#ifdef PORT_BLACKBOX
volatile unsigned int port_blackbox[2 + 64 * 2] DTCM_BSS;
#endif
void plat_wait_frame(void) {
#ifdef PORT_BLACKBOX
    port_blackbox[0] = 0x42424F58u;
#endif
    BB(0x46520001, frames);
#ifdef PORT_HW_SOUND
    port_hw_sound_frame();
#endif
#ifndef PORT_SOFT_DISPLAY
    swiWaitForVBlank();
#endif
    display_present();
    frames++;
#ifdef PORT_PC_SAMPLER
    port_pc_frame = (u32)frames;
#endif
    port_profile_frame(g_gameState ? g_gameState[8] : ~0u,(unsigned int)frames);
#ifdef PORT_CAPTURE_FRAME
    if(frames==PORT_CAPTURE_FRAME) {
#ifdef PORT_CAPTURE_READBACK
        /* A diagnostic checkpoint can explicitly exercise full PSX readback.
         * Published timing windows end before this operation. */
        extern void psx_vram_read(int, int, int, int);
        psx_vram_read(0, 0, 1024, 512);
#endif
        /* Diagnostic checkpoints freeze game time. Stop hardware playback too,
         * so waiting to save a state cannot inflate starvation counters. */
        port_audio_stop();
#ifdef PORT_PC_SAMPLER
        { extern volatile u32 port_pc_from_frame; port_pc_from_frame = ~0u; } /* (the wait for the state isn't the scene) */
#endif
        status_row(19,"capture frame %ld",frames);
        for(;;) swiWaitForVBlank();
    }
#endif
    if (frames % 30 == 0) {
#ifndef PORT_SOFT_DISPLAY
        status_row(18, "gxerr %d/%d/%d/%d/%d", psx_gx_fail_reason[0], psx_gx_fail_reason[1],
                   psx_gx_fail_reason[2], psx_gx_fail_reason[3], psx_gx_fail_reason[4]);
        status_row(19, "texfail %d budget %d", psx_gx_stats_texture_fail, psx_gx_stats_budget_fail);
        status_row(20, "gx %d polys drop %d", psx_gx_stats_polys, psx_gx_stats_dropped);
        status_row(21, "blend %d exact %d approx", psx_gx_stats_composited, psx_gx_stats_blend_fallback);
#endif
#ifdef PORT_GX_HYBRID
        {
            extern int gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail, gxh_stats_palette_fail,
                gxh_stats_blocks, gxh_stats_regions, gxh_stats_units, gxh_stats_unplaced, gxh_stats_units_peak;
            extern int gxh_stats_soft;
            status_row(20, "gx %d dr%d tx%d pl%d sw%x", gxh_stats_polys, gxh_stats_dropped, gxh_stats_texture_fail,
                       gxh_stats_palette_fail, gxh_stats_soft);
            extern int gxh_stats_upload_bytes, gxh_stats_upload_regions, gxh_stats_upload_palettes;
            status_row(18, "up %dK %d rg %d pal", gxh_stats_upload_bytes >> 10, gxh_stats_upload_regions,
                       gxh_stats_upload_palettes);
            extern int gxh_stats_ui_drawn, gxh_stats_ui_skipped;
            extern unsigned int gxh_debug[4];
            extern unsigned int psx_heap_free(void);
            static unsigned int heap_min = ~0u;
            unsigned int heap = psx_heap_free();
            if (heap < heap_min) heap_min = heap;
            status_row(9, "heap %u (min %u) dbg %x %x", heap, heap_min, gxh_debug[0], gxh_debug[1]);
            status_row(21, "rg %d u%d/%d np%d col %d ui %d/%d", gxh_stats_regions, gxh_stats_units, gxh_stats_units_peak,
                       gxh_stats_unplaced,
                       gxh_stats_blocks, gxh_stats_ui_drawn, gxh_stats_ui_skipped);
        }
#endif
#ifdef PORT_HW_SOUND
        status_row(17, "snd %d on, %d starts, %d waiting", port_hw_sound_stats[2], port_hw_sound_stats[0],
                   port_hw_sound_stats[1]);
#endif
#ifdef PORT_AUDIO
        status_row(17, "pcm %lu/%lu u%lu", (unsigned long)port_audio_stats[2],
                   (unsigned long)port_audio_stats[3], (unsigned long)port_audio_stats[4]);
#endif
        unsigned int free_stack = stack_free();
        if (free_stack < port_stack_free_min) port_stack_free_min = free_stack;
        status_row(16, "stack free %u (min %u)", free_stack, port_stack_free_min);
        unsigned char *gs = g_gameState;
        /* fixed line at the bottom of the console */
        status_row(23, "f%6ld m%d ovl %02X/%02X/%02X", frames, gs ? gs[8] : -1, psx_ovl_resident[0],
               psx_ovl_resident[1], psx_ovl_resident[2]);
        status_row(22, "cd%02X c%02X n%u s%u@%u p%X q%u", g_cdLoad[0], psx_cd_debug[0], psx_cd_debug[1],
               psx_cd_debug[2], psx_cd_debug[3], psx_cd_debug[4], psx_cd_debug[5]);
    }
}

/* DS keys to the PS1 pad (docs/dual-screen.md): A = Cross (confirm), B = Circle (cancel), X = Triangle (menu),
 * Y = Square, L/R = L1/R1, L+R together = L2+R2, plus the scripted presses of PORT_AUTOPRESS builds. */
unsigned int plat_pad_buttons(int port) {
    if (port != 0) {
        return 0;
    }
    scanKeys();
    unsigned int k = keysHeld(), b = 0;
    if (k & KEY_A) b |= 0x4000;
    if (k & KEY_B) b |= 0x2000;
    if (k & KEY_X) b |= 0x1000;
    if (k & KEY_Y) b |= 0x8000;
    if (k & KEY_L) b |= 0x0400;
    if (k & KEY_R) b |= 0x0800;
    if ((k & KEY_L) && (k & KEY_R)) b |= 0x0300;
    if (k & KEY_START) b |= 0x0008;
    if (k & KEY_SELECT) b |= 0x0001;
    if (k & KEY_UP) b |= 0x0010;
    if (k & KEY_RIGHT) b |= 0x0020;
    if (k & KEY_DOWN) b |= 0x0040;
    if (k & KEY_LEFT) b |= 0x0080;
    return b | input_script_buttons(frames);
}

void plat_vlog(const char *fmt, va_list ap) {
    vprintf(fmt, ap);
}

void plat_halt(void) {
    port_audio_stop();
    for (;;) {
        swiWaitForVBlank();
    }
}

/* ff9ds/ at the root of the boot drive: the DSi's SD card ("sd:"), or a DS flashcart via DLDI ("fat:"). */
static char data_dir[32] = "fat:/ff9ds";

void plat_video_init(void) {
    display_init();
}

const char *plat_data_dir(void) {
    return data_dir;
}

#ifdef PORT_DS_MODE
/* (CP15 writes need ARM code; this file is Thumb) */
__attribute__((target("arm"), noinline)) static void shared_wram_region(void) {
    __asm__ volatile("mcr p15, 0, %0, c6, c3, 0" : : "r"(0x03000000u | 14u << 1 | 1u)); /* 32 KB at 0x03000000 */
}
#endif

int main(void) {
    defaultExceptionHandler();
#ifdef PORT_DS_MODE
    WRAM_CR = 0; /* both shared WRAM banks to the ARM9, at 0x03000000: its ARM7 doesn't use them (port/arm7/main.c) */
    /* and readable: in DS mode BlocksDS's protection regions don't cover 0x03000000 (main RAM's ends at 0x02FFFFFF),
     * so the first access to the SPU RAM piece there (psx_mem_init) was a data abort, a minute into a field when a
     * sound first used it. Region 3 (the Slot-2 cartridge space, unused) becomes the 32 KB of shared WRAM. */
    shared_wram_region();
    /* BlocksDS's crt0 keeps the top 48 KB of main RAM from the heap, for a loader's return-to-loader stub; the game
     * never exits, so the heap goes up to the system area (__eheap_end) */
    extern char *fake_heap_end, __eheap_end[];
    fake_heap_end = __eheap_end;
#endif
    stack_paint();
    /* A 4bpp font and one tile map fit in bank H. Leave all four 128 KiB banks for GX textures. */
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 0, 1, false, true);
    if (isDSiMode()) {
        setCpuClock(true); /* the DSi's 134 MHz ARM9 (BlocksDS leaves it at 67 MHz) */
    }
    printf("ff9-ds (%s mode)\n", isDSiMode() ? "DSi" : "DS");
    printf("psx_ram %p\n", (void *)psx_ram);
    psx_mem_init(); /* the VRAM and SPU RAM shadows, before anything else takes heap */
    if (!fatInitDefault()) {
        printf("no SD card / flashcart filesystem\n");
    } else {
        snprintf(data_dir, sizeof data_dir, "%sff9ds", fatGetDefaultDrive());
    }
    printf("data: %s\n", data_dir);
#ifdef PORT_SCENARIO
    port_scenario_load(PORT_SCENARIO);
#endif
#ifdef PORT_ITCM_GUARD
    void port_itcm_guard_init(void);
    extern volatile unsigned port_itcm_guard_bytes;
    port_itcm_guard_init();
    printf("ITCM guard: %u bytes\n",port_itcm_guard_bytes);
#endif
#ifdef PORT_AUTOPRESS
    input_script_add_list(PORT_AUTOPRESS);
#endif
#ifndef PORT_AUTORUN /* `make AUTORUN=1`: run straight away, for automated emulator checks */
    printf("START: run the game\n");
    do {
        swiWaitForVBlank();
        scanKeys();
    } while (!(keysDown() & KEY_START));
#endif
    port_profile_init();
#ifdef PORT_PC_SAMPLER
    { void port_pc_sampler_init(void); port_pc_sampler_init(); }
#endif
#ifdef PORT_AUDIO
    printf("SPU PCM: %s\n", port_audio_init() ? "44100 Hz stereo" : "initialization failed");
#endif
#ifdef PORT_HW_SOUND
    port_hw_sound_init();
    printf("SPU: DS hardware channels\n");
#endif
    psx_run_game();
}
