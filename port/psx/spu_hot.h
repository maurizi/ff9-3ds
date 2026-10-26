#ifndef PORT_SPU_HOT_H
#define PORT_SPU_HOT_H
/* DS instruction placement only; arithmetic and sequencing stay shared. */
#if defined(ARM7)
#define SPU_HOT /* (the ARM7 runs it from its own WRAM) */
#elif defined(PORT_SPU_ITCM) && defined(__arm__)
#define SPU_HOT __attribute__((section(".itcm")))
#else
#define SPU_HOT
#endif
/* With the voices on the hardware channels (HWSOUND) the mixer leaves ITCM; the per-sample voice stepping and block
 * decoding it still runs take some of the room. */
#if !defined(PORT_SPU_ITCM) && defined(PORT_HW_SOUND) && defined(__arm__) && !defined(ARM7)
#define SPU_HW_HOT __attribute__((section(".itcm")))
#define SPU_HW_ONLY_HOT __attribute__((section(".itcm"))) /* code only the hardware path runs */
#else
#define SPU_HW_HOT SPU_HOT
#define SPU_HW_ONLY_HOT
#endif
#endif
