/* The SPU on the audio core (3DS; the design is the DS port's spu7, docs/design.md §5).
 *
 * The game's thread runs AKAO and keeps the SPU register file the game reads (psx_spu_regs); a thread on the other
 * core owns the emulated SPU (spu_core.c compiled as spuw_*: voices, Gaussian interpolation, envelopes, reverb, the
 * mix) and plays its output through NDSP. Everything that changes the voices goes to it through a ring, in the
 * game's order: register writes, sample advances (in the game's virtual time), SPU RAM transfers. After each
 * advance it publishes what the game reads back (envelope levels, current addresses, volumes, ENDX), which the game
 * thread copies into its register file at its own advances: what AKAO reads back is up to one AKAO tick older than
 * on the PS1. A key-on shows at once (the voice reads as attacking from its start address) until a readback
 * includes it: AKAO frees a voice whose envelope reads 0.
 * The two cores' caches are coherent; the ring only needs ordering (barriers). */
#include <3ds.h>
#include <string.h>

#include "ctr.h"
#include "psx_runtime.h"
#include "psx_spu.h"
#include "spu7.h"

extern unsigned short psx_spu_regs[0x200];
unsigned short spuw_regs[0x200]; /* the audio core's register file (spu_core.c's, renamed) */
extern unsigned psx_spu_ram_size, psx_spu_ram_split[2];
extern unsigned char *psx_spu_ram, *psx_spu_ram_mid, *psx_spu_ram_hi;

void spuw_set_rendering(int enabled);
void spuw_reset(void);
uint16_t spuw_read(unsigned offset);
void spuw_write(unsigned offset, uint16_t value);
void spuw_advance(unsigned ticks);

static Spu7Shared shared;
static Thread thread;
static LightEvent kick_event;
static volatile int quit;
static unsigned sample_fraction, published;
unsigned int ctr_spu_backlog_max;

/* ---- the audio core -------------------------------------------------------------------------------------- */

static u32 ram_at;

static void ram_span(u32 at, u32 n, const u8 *src, u8 *dst) {
    while (n) {
        if (at >= psx_spu_ram_size) {
            if (dst) memset(dst, 0, n);
            return;
        }
        int piece = at < psx_spu_ram_split[0] ? 0 : at < psx_spu_ram_split[1] ? 1 : 2;
        u32 end = piece < 2 ? psx_spu_ram_split[piece] : psx_spu_ram_size;
        u32 len = n < end - at ? n : end - at;
        u8 *p = (piece == 0 ? psx_spu_ram : piece == 1 ? psx_spu_ram_mid : psx_spu_ram_hi) + at;
        if (src) memcpy(p, src, len), src += len;
        else if (dst) memcpy(dst, p, len), dst += len;
        else memset(p, 0, len);
        at += len;
        n -= len;
    }
}

static void publish(u32 done) {
    shared.published++; /* (odd: being written) */
    __sync_synchronize();
    Spu7Readback *r = &shared.readback;
    for (int i = 0; i < SPU7_VOICES; ++i) {
        r->voice[i][0] = spuw_read(i * 16 + 12);
        r->voice[i][1] = spuw_read(i * 16 + 14);
        r->volume[i][0] = spuw_read(0x200 + i * 4);
        r->volume[i][1] = spuw_read(0x202 + i * 4);
    }
    r->ended[0] = spuw_read(0x19c);
    r->ended[1] = spuw_read(0x19e);
    r->main_volume[0] = spuw_read(0x1b8);
    r->main_volume[1] = spuw_read(0x1ba);
    shared.published_at = done;
    __sync_synchronize();
    shared.published++;
}

static void service(void) {
    u32 tail = shared.tail;
    while (tail != shared.head) {
        __sync_synchronize(); /* (the command after its index) */
        const Spu7Cmd c = shared.cmd[tail % SPU7_RING];
        u32 arg = c.a & 0xFFFFFF;
        switch (c.a >> 24) {
        case SPU7_WRITE: spuw_write(arg, (uint16_t)c.b); break;
        case SPU7_ADVANCE: spuw_advance(c.b * 96); publish(tail + 1); break;
        case SPU7_ADDR: ram_at = arg; break;
        case SPU7_RAM_IN: ram_span(ram_at, arg, (const u8 *)c.b, NULL); shared.transfers++; break;
        case SPU7_RAM_OUT: ram_span(ram_at, arg, NULL, (u8 *)c.b); shared.transfers++; break;
        case SPU7_RAM_CLEAR: ram_span(ram_at, arg, NULL, NULL); shared.transfers++; break;
        case SPU7_RESET: spuw_reset(); publish(tail + 1); break;
        default: break;
        }
        __sync_synchronize();
        shared.tail = ++tail;
    }
}

static void audio_main(void *unused) {
    (void)unused;
    spuw_set_rendering(1);
    while (!quit) {
        LightEvent_WaitTimeout(&kick_event, 2000000); /* (2 ms at most: a missed kick only delays) */
        service();
    }
}

static int inline_mode; /* SPUSYNC=1: no thread (the SPU in step with the game, as on the host: deterministic runs) */
static void start(void) {
    if (thread || inline_mode) return;
#ifdef PORT_SPU_SYNC
    inline_mode = 1;
    spuw_set_rendering(1);
    port_log("spu: in line (SPUSYNC)\n");
    return;
#endif
    LightEvent_Init(&kick_event, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    void ctr_core1_share(int any_model);
    ctr_core1_share(0); /* (core 1 gives an application's threads only the share set: async.c) */
    /* above the MDEC worker (async.c) on the same core: sound must not wait for a movie frame */
    int core = ctr_is_new3ds() ? 2 : 1;
    thread = threadCreate(audio_main, NULL, 32 * 1024, prio - 2, core, false);
    if (!thread && core == 2) ctr_core1_share(1), thread = threadCreate(audio_main, NULL, 32 * 1024, prio - 2, 1, false);
    port_log("spu: %s\n", thread ? (core == 2 ? "on core 2" : "on core 1") : "on the game's core (no thread)");
}

void ctr_spu_exit(void) {
    if (!thread) return;
    quit = 1;
    LightEvent_Signal(&kick_event);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
    thread = NULL;
}

/* ---- the game's side ----------------------------------------------------------------------------------------- */

static void kick(void) {
    if (thread) LightEvent_Signal(&kick_event);
    else service(); /* (no thread: in line, as on one core) */
}

static void put(u32 type, u32 arg, u32 b) {
    if (!thread) start();
    u32 head = shared.head;
    u32 backlog = head - shared.tail;
    if (backlog > ctr_spu_backlog_max) ctr_spu_backlog_max = backlog;
    if (backlog >= SPU7_RING) {
        extern volatile const char *ctr_where;
        ctr_where = "waiting for the audio thread (SPU ring full)";
        kick();
        /* (full: the audio core catches up; if it makes no progress at all for a second, it isn't being scheduled:
         * the SPU runs on the game's thread from then on) */
        u32 tail0 = shared.tail;
        u64 t0 = osGetTime();
        while (head - shared.tail >= SPU7_RING) {
            svcSleepThread(100000);
            if (shared.tail == tail0 && osGetTime() - t0 > 1000) {
                port_log("spu: the audio thread isn't running; the SPU runs on the game's thread\n");
                quit = 1;
                thread = NULL; /* (left behind: it never ran) */
                inline_mode = 1;
                spuw_set_rendering(1);
                service();
                break;
            }
        }
    }
    Spu7Cmd *c = &shared.cmd[head % SPU7_RING];
    c->a = type << 24 | (arg & 0xFFFFFF);
    c->b = b;
    __sync_synchronize(); /* (the command before its index) */
    shared.head = head + 1;
}

static u32 keyed_on[SPU7_VOICES];

/* the audio core's latest readbacks into the register file */
static void readbacks(void) {
    u32 seq = shared.published;
    if (seq == published || (seq & 1)) return;
    __sync_synchronize();
    static Spu7Readback r;
    r = shared.readback;
    u32 at = shared.published_at;
    __sync_synchronize();
    if (shared.published != seq) return; /* (written meanwhile: the next time) */
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

static void key_on(u32 bits) {
    for (int i = 0; i < SPU7_VOICES; ++i)
        if (bits >> i & 1) {
            keyed_on[i] = shared.head;
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
    if (frames) {
        put(SPU7_ADVANCE, 0, frames);
        kick();
    }
    readbacks();
}

/* SPU RAM transfers (spu.c): done by the audio core in its order; the game's side waits, as its source may change */
int psx_spu7_ram(unsigned at, unsigned n, const unsigned char *src, unsigned char *dst) {
    u32 want = shared.transfers + 1;
    put(SPU7_ADDR, at, 0);
    put(src ? SPU7_RAM_IN : dst ? SPU7_RAM_OUT : SPU7_RAM_CLEAR, n, (u32)(src ? (const void *)src : (void *)dst));
    kick();
    while ((s32)(shared.transfers - want) < 0) svcSleepThread(50000);
    __sync_synchronize();
    return 1;
}
