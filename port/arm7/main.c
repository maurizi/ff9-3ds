/* The ARM7 for DSMODE=1 builds: BlocksDS's minimal core (sound, input, clock, firmware settings, system FIFO), linked
 * without the 32 KB of shared WRAM BlocksDS's cores take (Makefile: __shared_wram_size=0), so the ARM9 gets it
 * (port/ds/platform.c). Maxmod isn't needed: the DS mode build mixes with the hardware channels (HWSOUND). */
#include <nds.h>

static void vblank(void) { inputGetAndSend(); }

int main(void) {
    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();
    irqInit();
    fifoInit();
    installSoundFIFO();
    installSystemFIFO();
    initClockIRQTimer(3);
    irqSet(IRQ_VBLANK, vblank);
    irqEnable(IRQ_VBLANK);
    for (;;) swiWaitForVBlank();
}
