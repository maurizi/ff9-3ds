/* The ARM9's side of the SPU on the ARM7 (spu7.h): the register file as the game reads it, everything that changes
 * the voices sent to the ARM7 in order, its readbacks copied in at each advance. */
#ifdef PORT_SPU_ARM7
#include <nds.h>
#include <string.h>
#include "spu7.h"
#include "psx_spu.h"
#include "psx_mem.h"
#include "audio_hw.h"
#include "psx_runtime.h"

extern unsigned short psx_spu_regs[0x200];
extern unsigned psx_spu_ram_size, psx_spu_ram_split[2];
extern unsigned char *psx_spu_ram, *psx_spu_ram_mid, *psx_spu_ram_hi;

#define RINGS 16
#define RING 4096
#define ECHO_MAX 3072

unsigned int spu7_backlog_max; /* (post-mortems: the most commands the ARM7 was behind) */
static Spu7Shared shared_block __attribute__((aligned(32)));
static Spu7Shared *shared; /* (main RAM's uncached mirror: the ARM7 writes it behind the ARM9's cache) */
static unsigned sample_fraction;
static unsigned published;

static Spu7Shared *uncached(Spu7Shared *p) { return (Spu7Shared *)((u32)p | 0x400000u); }

void port_hw_sound_init(void) {
    soundEnable();
    DC_FlushRange(&shared_block, sizeof shared_block);
    shared = uncached(&shared_block);
    memset((void *)shared, 0, sizeof *shared);
    shared->rings = psx_spare_alloc(RINGS * RING * sizeof(s16));
    shared->echo = psx_spare_alloc(2 * ECHO_MAX * sizeof(s16));
    shared->ram = psx_spu_ram;
    shared->ram_mid = psx_spu_ram_mid;
    shared->ram_hi = psx_spu_ram_hi;
    shared->ram_size = psx_spu_ram_size;
    shared->ram_split[0] = psx_spu_ram_split[0];
    shared->ram_split[1] = psx_spu_ram_split[1];
    DC_FlushAll(); /* (the rings and SPU RAM as the ARM9 left them) */
    fifoSendValue32(SPU7_FIFO, (u32)shared);
}

static void kick(void) {
    if (shared) fifoSendValue32(SPU7_FIFO, 0);
}

static void put(u32 type, u32 arg, u32 b) {
    if (!shared) return;
    u32 head = shared->head;
    u32 backlog = head - shared->tail;
    if (backlog > spu7_backlog_max) spu7_backlog_max = backlog;
    if (backlog >= SPU7_RING) {
        kick();
        while (head - shared->tail >= SPU7_RING) {} /* (full: the ARM7 catches up) */
    }
    Spu7Cmd *c = &shared->cmd[head % SPU7_RING];
    c->a = type << 24 | (arg & 0xFFFFFF);
    c->b = b;
    shared->head = head + 1;
}

/* per voice, the command count just past its last key-on: until a readback includes it, the voice reads as the key-on
 * leaves it: attacking (a level above 0: AKAO detaches a voice whose envelope reads 0, which it then never keys off,
 * and on the ARM9 the attack had begun by the next read), at its start address, ENDX clear */
static u32 keyed_on[SPU7_VOICES];

/* the ARM7's latest readbacks into the register file */
static void readbacks(void) {
    if (!shared) return;
    u32 seq = shared->published;
    if (seq == published || (seq & 1)) return;
    static Spu7Readback r;
    r = *(const Spu7Readback *)&shared->readback;
    u32 at = shared->published_at;
    if (shared->published != seq) return; /* (written meanwhile: the next time) */
    published = seq;
    u32 ended = r.ended[0] | (u32)r.ended[1] << 16;
    for (int i = 0; i < SPU7_VOICES; ++i) {
        if ((s32)(at - keyed_on[i]) < 0) {
            r.voice[i][0] = 1;
            r.voice[i][1] = psx_spu_regs[i * 8 + 3];
            ended &= ~(1u << i);
        }
        psx_spu_regs[i * 8 + 6] = r.voice[i][0];
        psx_spu_regs[i * 8 + 7] = r.voice[i][1];
        psx_spu_regs[0x100 + i * 2] = r.volume[i][0];
        psx_spu_regs[0x101 + i * 2] = r.volume[i][1];
    }
    psx_spu_regs[0x19c / 2] = (u16)ended;
    psx_spu_regs[0x19e / 2] = (u16)(ended >> 16);
    psx_spu_regs[0x1b8 / 2] = r.main_volume[0];
    psx_spu_regs[0x1ba / 2] = r.main_volume[1];
}

/* a key-on's effect on what the game reads back, at once (readbacks keeps it until the ARM7 has it) */
static void key_on(u32 bits) {
    for (int i = 0; i < SPU7_VOICES; ++i)
        if (bits >> i & 1) {
            keyed_on[i] = shared ? shared->head : 0;
            psx_spu_regs[i * 8 + 6] = 1;
            psx_spu_regs[i * 8 + 7] = psx_spu_regs[i * 8 + 3];
            psx_spu_regs[0x19c / 2 + (i >> 4)] &= (u16)~(1u << (i & 15));
        }
}

void psx_spu_set_rendering(int enabled) {
    (void)enabled;
}

void psx_spu_reset(void) {
    memset(psx_spu_regs, 0, 0x400);
    sample_fraction = 0;
    put(SPU7_RESET, 0, 0);
    kick();
    psx_spu_write(0x1aa, 0xc000); /* (as spu_core.c's reset: libspu initializes enabled and unmuted) */
}

uint16_t psx_spu_read(unsigned offset) {
    if ((offset & 1) || offset >= 0x400) return 0xffff;
    readbacks();
    return psx_spu_regs[offset / 2];
}

void psx_spu_write(unsigned offset, uint16_t value) {
    if ((offset & 1) || offset >= 0x400) return;
    if (offset == 0x19c || offset == 0x19e || offset == 0x1ae) return;
    psx_spu_regs[offset / 2] = value;
    if (offset == 0x1aa) psx_spu_regs[0x1ae / 2] = (psx_spu_regs[0x1ae / 2] & ~0x3f) | (value & 0x3f);
    put(SPU7_WRITE, offset, value);
    if (offset == 0x188) key_on(value);
    else if (offset == 0x18a) key_on((u32)value << 16);
}

void psx_spu_voice_register(unsigned voice, unsigned offset, uint16_t value) {
    if (voice < SPU7_VOICES && offset < 16) psx_spu_write(voice * 16 + offset, value);
}

/* (as spu_core.c's: 96 ticks of the 33.8688 MHz clock a sample) */
void psx_spu_advance(unsigned ticks) {
    unsigned frames = ticks / 96;
    sample_fraction += ticks % 96;
    frames += sample_fraction / 96;
    sample_fraction %= 96;
    if (frames) put(SPU7_ADVANCE, 0, frames);
    readbacks();
}

void port_hw_sound_frame(void) {
    put(SPU7_FRAME, 0, 0);
    kick();
}

/* SPU RAM transfers (spu.c): done by the ARM7 in its order; the ARM9 waits, as its source may change after */
int psx_spu7_ram(unsigned at, unsigned n, const unsigned char *src, unsigned char *dst) {
    if (!shared) return 0;
    if (src) DC_FlushRange(src, n);
    if (dst) DC_FlushRange(dst, n); /* (no dirty line of it to land over the ARM7's copy later) */
    u32 want = shared->transfers + 1;
    put(SPU7_ADDR, at, 0);
    put(src ? SPU7_RAM_IN : dst ? SPU7_RAM_OUT : SPU7_RAM_CLEAR, n, (u32)(src ? (const void *)src : (void *)dst));
    kick();
    while ((s32)(shared->transfers - want) < 0) {}
    if (dst) DC_InvalidateRange(dst, n);
    return 1;
}
#endif
