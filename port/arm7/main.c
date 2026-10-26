/* The ARM7 for DSMODE=1 builds: BlocksDS's minimal core (sound, input, clock, firmware settings, system FIFO), linked
 * without the 32 KB of shared WRAM BlocksDS's cores take (Makefile: __shared_wram_size=0), so the ARM9 gets it
 * (port/ds/platform.c). Maxmod isn't needed: the DS mode build mixes with the hardware channels (HWSOUND).
 * With SPU7=1 (the default there) it also runs the emulated SPU's voices and those channels (port/arm7/spu7_server.c,
 * spu7.h), and the shared WRAM is its (SPU RAM's second piece). */
#include <nds.h>

static void vblank(void) { inputGetAndSend(); }

#ifdef PORT_SPU_ARM7
void spu7_init(void);
void spu7_service(void);
#endif

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
#ifdef PORT_SPU_ARM7
    /* the SPU (spu7.h): the ARM9's commands whenever it kicks (or at the next VBlank) */
    spu7_init();
    for (;;) {
        spu7_service();
        swiIntrWait(1, IRQ_VBLANK | IRQ_FIFO_NOT_EMPTY);
    }
#else
    for (;;) swiWaitForVBlank();
#endif
}
