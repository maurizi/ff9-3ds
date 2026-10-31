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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    pad_now = b | ctr_ui_pad_buttons();
}

unsigned int plat_pad_buttons(int port) {
    if (port != 0) return 0;
    return pad_now | input_script_buttons(frames);
}

/* ---- frame pacing ----------------------------------------------------------------------------------------- */

/* The PS1 counts VBlanks at 59.94 Hz; the 3DS's LCDs refresh at 59.83 Hz. The game paces itself in VBlanks
 * (VSync(n)), so pacing to the 3DS's VBlank keeps it 0.2% slow, which nobody hears. VBlanks that passed while the
 * game worked count toward a VSync(n) (plat_vsync_begin), as on the PS1, so a slow frame doesn't wait again. */
static unsigned int vsync_owed;
static u64 last_vblank_tick;
#define TICKS_PER_VBLANK (SYSCLOCK_ARM11 / 59.83)

void plat_vsync_begin(unsigned int n) {
    vsync_owed = n;
}

void plat_wait_frame(void) {
    frames++;
    if (!aptMainLoop()) {
        ctr_shutdown();
        exit(0);
    }
    read_input();
    ctr_present(frames);
#ifdef PORT_DUMP_AT
    if (frames == PORT_DUMP_AT) { void ctr_render_dump(void); ctr_render_dump(); }
#endif
    /* wait for the next VBlank unless one already passed since the last one we waited for */
    u64 now = svcGetSystemTick();
    if (now - last_vblank_tick < (u64)(TICKS_PER_VBLANK * 0.9)) {
        unsigned int w = port_profile_wait_begin();
        gspWaitForVBlank();
        port_profile_wait_end(w);
        last_vblank_tick = svcGetSystemTick();
    } else {
        last_vblank_tick += (u64)TICKS_PER_VBLANK;
        if (now - last_vblank_tick > (u64)(TICKS_PER_VBLANK * 4)) last_vblank_tick = now;
    }
    ctr_profile_frame();
}

/* ---- misc ------------------------------------------------------------------------------------------------- */

void plat_vlog(const char *fmt, va_list ap) {
    ctr_console_vlog(fmt, ap);
}

void plat_halt(void) {
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

const char *plat_data_dir(void) {
    return "sdmc:/3ds/ff9";
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
    ctr_console_init();
    printf("ff9-3ds (%s 3DS)\n", is_new3ds ? "New" : "Old");
    ctr_audio_init();
    ctr_async_init();
#ifdef PORT_AUTOPRESS
    input_script_add_list(PORT_AUTOPRESS);
#endif
#ifdef PORT_SCENARIO
    port_scenario_load(PORT_SCENARIO);
#endif
    psx_spu_set_rendering(1);
    psx_run_game();
    return 0;
}
