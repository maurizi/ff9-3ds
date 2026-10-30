/* The emulated SPU on the ARM7 (DS mode with hardware mixing; docs/design.md 7.0, the user's choice of 2026-10-26).
 *
 * The ARM9 runs the game and AKAO and keeps the SPU's register file (psx_spu_regs) as the game reads it; the ARM7
 * owns the voices (spu_core.c, spu_voice.c, spu_source.c, spu_adpcm.c, spu_envelope.c) and the hardware channels
 * (audio_hw.c). The ARM9 sends what changes them through a ring in main RAM, in its order: register writes, sample
 * advances (in the game's virtual time, as psx_spu_advance had them), once a frame the channels' update, and SPU RAM
 * transfers (the ARM7 copies; part of SPU RAM is in the shared WRAM, which only the ARM7 can reach). The ARM7
 * publishes what the game reads back (envelope levels, current addresses, volumes, ENDX) after each advance, and the
 * ARM9 copies it into its register file at its own advances: what AKAO reads back is up to a frame older than on
 * the PS1. */
#ifndef SPU7_H
#define SPU7_H
#include <stdint.h>

#define SPU7_RING 512 /* commands */
#define SPU7_VOICES 24

enum {
    SPU7_WRITE = 1,   /* a: offset, b: value */
    SPU7_ADVANCE,     /* b: samples */
    SPU7_FRAME,       /* the channels' update (port_hw_sound_frame) */
    SPU7_ADDR,        /* a: the next transfer's SPU RAM address */
    SPU7_RAM_IN,      /* a: bytes, b: source (main RAM) */
    SPU7_RAM_OUT,     /* a: bytes, b: destination (main RAM) */
    SPU7_RAM_CLEAR,   /* a: bytes */
    SPU7_RESET,
};

typedef struct {
    uint32_t a; /* type << 24 | argument */
    uint32_t b;
} Spu7Cmd;

typedef struct {
    uint16_t voice[SPU7_VOICES][2]; /* registers 6 and 7: the envelope level, the current address */
    uint16_t volume[SPU7_VOICES][2]; /* the voices' current volumes (0x200 up) */
    uint16_t ended[2];               /* ENDX (0x19c) */
    uint16_t main_volume[2];         /* (0x1b8) */
} Spu7Readback;

typedef struct {
    volatile uint32_t head;          /* the ARM9's: commands written */
    volatile uint32_t tail;          /* the ARM7's: commands done */
    volatile uint32_t transfers;     /* the ARM7's: SPU RAM transfers done */
    volatile uint32_t published;     /* the ARM7's: readbacks published (odd while it writes one) */
    volatile uint32_t published_at;  /* the ARM7's: the commands done when the last readback was taken */
    volatile uint32_t busy, frames;  /* the ARM7's: time spent on the commands (its clock, 64 bus cycles a tick) and
                                      * channel updates done, for its load (post-mortems) */
    Spu7Readback readback;
    Spu7Cmd cmd[SPU7_RING];
    /* set up by the ARM9 before it starts the ARM7's loop */
    uint8_t *ram, *ram_mid, *ram_hi; /* SPU RAM's pieces (spu_layout.c) */
    uint32_t ram_size, ram_split[2];
    int16_t *rings;                  /* the voices' sample rings (audio_hw.c) */
    int16_t *echo;                   /* the echo's capture rings */
} Spu7Shared;

#define SPU7_FIFO FIFO_USER_02       /* the ARM9's kick (a value: the shared block's address the first time) */

#endif
