/* libspu: the SPU as the game sees it, for the port's sound path.
 *
 * - SPU RAM (512 KB) is a shadow; SpuWrite/SpuRead transfer to and from it at the transfer address.
 * - Voice/global writes reach spu_core.c through the PsyQ API or AKAO boundary overrides.
 *   psx_spu_regs[] remains a shadow for direct hardware readbacks.
 * - Transfers complete immediately; the transfer callback runs before SpuWrite returns.
 * Host --audio and the DS stream use sample-clocked stereo PCM. DS playback
 * still underruns when the game cannot keep pace; the IRQ is delivered at the frame step (SpuSetIRQ), DMA timing is
 * unfinished; see docs/audio-validation.md.
 */
#include "psyq_port.h"
#include "psx_spu.h"
#include "psx_spu_presets.h"
#include "psx_spu_trace.h"

#define SPU_RAM 0x80000

u8 *psx_spu_ram; /* allocated by psx_mem_init (NULL on a 4 MB DS: transfers are dropped) */
/* spu_layout.c: how the shadow keeps SPU RAM (weak: the SPU unit tests have one plain SPU RAM) */
extern unsigned psx_spu_ram_size __attribute__((weak)), psx_spu_ram_split[2] __attribute__((weak));
extern u8 *psx_spu_ram_mid __attribute__((weak)), *psx_spu_ram_hi __attribute__((weak));
__attribute__((weak)) void port_log(const char *fmt, ...); /* absent from the SPU unit tests */

/* Copy `n` bytes at SPU RAM address `at` in (src), out (dst) or clear them (neither), piece by piece; what lies
 * past the kept size is dropped (out: zeros). */
#if defined(PORT_SPU_ARM7) || defined(PORT_SPU_WORKER)
/* the core on the other processor keeps SPU RAM in its own order (port/3ds/spu_client.c) */
int psx_spu7_ram(unsigned at, unsigned n, const unsigned char *src, unsigned char *dst);
#endif
static void spu_ram_span(u32 at, u32 n, const u8 *src, u8 *dst) {
    static int logged;
#if defined(PORT_SPU_ARM7) || defined(PORT_SPU_WORKER)
    if (psx_spu7_ram(at, n, src, dst)) return;
#endif
    if (!&psx_spu_ram_size) { /* one plain SPU RAM */
        u8 *p = psx_spu_ram + at;
        if (src) __builtin_memcpy(p, src, n);
        else if (dst) __builtin_memcpy(dst, p, n);
        else __builtin_memset(p, 0, n);
        if (!dst) psx_spu_trace(PSX_SPU_TRACE_RAM, at, n, p);
        return;
    }
    while (n) {
        if (at >= psx_spu_ram_size) {
            if (dst) __builtin_memset(dst, 0, n);
            for (u32 i = 0; src && i < n && !logged; ++i)
                if (src[i] && port_log) { port_log("SPU RAM write at 0x%X beyond the 0x%X bytes kept: dropped\n", at + i, psx_spu_ram_size); logged = 1; }
            return;
        }
        int piece = at < psx_spu_ram_split[0] ? 0 : at < psx_spu_ram_split[1] ? 1 : 2;
        u32 end = piece < 2 ? psx_spu_ram_split[piece] : psx_spu_ram_size;
        u32 len = n < end - at ? n : end - at;
        u8 *p = (piece == 0 ? psx_spu_ram : piece == 1 ? psx_spu_ram_mid : psx_spu_ram_hi) + at;
        if (src) __builtin_memcpy(p, src, len), src += len;
        else if (dst) __builtin_memcpy(dst, p, len), dst += len;
        else __builtin_memset(p, 0, len);
        if (!dst) psx_spu_trace(PSX_SPU_TRACE_RAM, at, len, p);
        at += len; n -= len;
    }
}

static u32 transfer_addr;
static void (*transfer_cb)(void);
static void (*irq_cb)(void);
static long reverb_type;
static SpuReverbAttr reverb_attr;

void SpuInit(void) {
    psx_spu_reset();
    if (psx_spu_ram) {
        spu_ram_span(0, SPU_RAM, NULL, NULL);
    }
    reverb_type = 0;
    __builtin_memset(&reverb_attr, 0, sizeof reverb_attr);
    psx_spu_write(0x1a2, 0xfffe);
    transfer_addr = 0;
    transfer_cb = NULL;
}

void SpuStart(void) {
}

void SpuQuit(void) {
}

long SpuInitMalloc(long num, char *top) {
    return num;
}

u32 SpuSetTransferStartAddr(u32 addr) {
    if (addr > SPU_RAM - 8) {
        return 0;
    }
    transfer_addr = addr & ~7u;
    return transfer_addr;
}

long SpuSetTransferMode(long mode) {
    return mode;
}

void *SpuSetTransferCallback(void (*func)(void)) {
    void (*old)(void) = transfer_cb;
    transfer_cb = func;
    return (void *)old;
}

/* SpuWrite (decomp SpuWrite): copy `size` bytes (clamped like libspu, to 0x7EFF0) into SPU RAM. */
u32 SpuWrite(u8 *addr, u32 size) {
    if (size > 0x7EFF0) {
        size = 0x7EFF0;
    }
    if (transfer_addr + size > SPU_RAM) {
        size = SPU_RAM - transfer_addr;
    }
    if (psx_spu_ram) {
        spu_ram_span(transfer_addr, size, addr, NULL);
    }
    if (transfer_cb) {
        transfer_cb();
    }
    return size;
}

/* SpuRead (decomp SpuRead) */
s32 SpuRead(s32 addr, s32 size) {
    u32 n = (u32)size;
    /* Original libspu uses an unsigned comparison, just like SpuWrite. Bound
     * before adding transfer_addr so a negative/oversized size cannot wrap. */
    if (n > 0x7eff0) n = 0x7eff0;
    if (transfer_addr + n > SPU_RAM) {
        n = SPU_RAM - transfer_addr;
    }
    if (psx_spu_ram) {
        spu_ram_span(transfer_addr, n, NULL, (u8 *)addr);
    }
    if (transfer_cb) {
        transfer_cb();
    }
    return (s32)n;
}

void SpuSetReverb(int on_off) {
    psx_spu_write(0x1aa, (psx_spu_read(0x1aa) & ~0x80) | (on_off ? 0x80 : 0));
}

/* Mode changes mute EVOL and replace all DSP registers; depth-only and echo
 * parameter updates must preserve the other registers and the RAM cursor. */
static void reverb_registers(const u16 regs[32], u32 selected) {
    unsigned i;
    for (i = 0; i < 32; ++i)
        if (selected & (1u << i)) psx_spu_write(0x1c0 + i * 2, regs[i]);
}

long SpuSetReverbModeType(long mode) {
    SpuReverbAttr attr;
    __builtin_memset(&attr, 0, sizeof attr);
    attr.mask = 1;
    attr.mode = mode;
    return SpuSetReverbModeParam(&attr);
}

long SpuSetReverbModeParam(SpuReverbAttr *attr) {
    u32 mask = attr->mask ? attr->mask : 0x1f;
    int change = !!(mask & 1);
    long mode = change ? attr->mode & ~0x100L : reverb_type;
    u16 regs[32];
    u32 selected = change ? 0xffffffffu : 0;
    unsigned control = psx_spu_read(0x1aa);
    if ((unsigned long)mode >= 10) return -1;
    __builtin_memcpy(regs, psx_spu_presets[mode].registers, sizeof regs);
    if (change) {
        reverb_type = mode;
        reverb_attr.delay = mode == 7 || mode == 8 ? 127 : 0;
        reverb_attr.feedback = mode == 7 ? 127 : 0;
    }
    if ((mode == 7 || mode == 8) && (mask & 8)) {
        /* MIPS shifts wrap at 32 bits before signed division by 127. */
        s32 full = (s32)((u32)attr->delay << 13) / 127;
        s32 half = (s32)((u32)attr->delay << 12) / 127;
        regs[10] = full - regs[0];
        regs[11] = half - regs[1];
        regs[12] = regs[13] + half;
        regs[16] = regs[17] + half;
        regs[26] = regs[28] + half;
        regs[27] = regs[29] + half;
        selected |= 0x0c011c00u;
        reverb_attr.delay = attr->delay;
    }
    if ((mode == 7 || mode == 8) && (mask & 16)) {
        regs[7] = (s32)((u32)attr->feedback * 33024u) / 127;
        selected |= 0x80;
        reverb_attr.feedback = attr->feedback;
    }
    if (change) {
        psx_spu_write(0x1aa, control & ~0x80);
        SpuSetReverbModeDepth(0, 0);
    } else {
        SpuSetReverbDepth(attr);
    }
    reverb_registers(regs, selected);
    if (change) {
        if (attr->mode & 0x100) SpuClearReverbWorkArea(mode);
        psx_spu_write(0x1a2, (SPU_RAM - psx_spu_presets[mode].bytes) / 8);
        psx_spu_write(0x1aa, control);
    }
    return 0;
}

void SpuSetReverbDepth(SpuReverbAttr *attr) {
    if (!attr->mask || (attr->mask & 2)) {
        reverb_attr.depth.left = attr->depth.left;
        psx_spu_write(0x184, attr->depth.left);
    }
    if (!attr->mask || (attr->mask & 4)) {
        reverb_attr.depth.right = attr->depth.right;
        psx_spu_write(0x186, attr->depth.right);
    }
}

void SpuSetReverbModeDepth(short l, short r) {
    reverb_attr.depth.left = l;
    reverb_attr.depth.right = r;
    psx_spu_write(0x184, l);
    psx_spu_write(0x186, r);
}

long SpuClearReverbWorkArea(long mode) {
    unsigned bytes;
    if ((unsigned long)mode >= 10) return -1;
    bytes = mode ? psx_spu_presets[mode].bytes : 128;
    /* Synchronous transfer policy, like SpuWrite. Preserve the caller's DMA
     * address/callback; this internal clear must not invoke its callback. */
    if (psx_spu_ram) {
        spu_ram_span(SPU_RAM - bytes, bytes, NULL, NULL);
    }
    return 0;
}

long SpuSetNoiseClock(long n) {
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    psx_spu_write(0x1aa, (psx_spu_read(0x1aa) & 0xc0ff) | (n << 8));
    return n;
}

/* The SPU IRQ: a voice reading the 16-byte block that holds the IRQ address (in 8-byte units, as register 0x1A4)
 * while it is enabled. The mixer notes the hit (psx_spu_source_fetch, possibly on another core) and the game thread
 * runs the callback at its next frame step (psx_spu_irq_poll, irq.c), as libspu's interrupt handler would: AKAO's
 * ring streams (movie audio) refill their next block from it. */
volatile unsigned psx_spu_irq_on, psx_spu_irq_addr, psx_spu_irq_hit;

int SpuSetIRQ(int on_off) {
    if (on_off == 0 || on_off == 3) psx_spu_write(0x1aa, psx_spu_read(0x1aa) & ~0x40), psx_spu_irq_on = 0, psx_spu_irq_hit = 0;
    if (on_off == 1 || on_off == 3) psx_spu_write(0x1aa, psx_spu_read(0x1aa) | 0x40), psx_spu_irq_on = 1;
    return on_off;
}

void SpuSetIRQAddr(u32 addr) {
    psx_spu_write(0x1a4, addr >> 3);
    psx_spu_irq_addr = addr >> 3;
    psx_spu_irq_hit = 0;
}

void psx_spu_irq_poll(void) {
    if (!psx_spu_irq_hit || !psx_spu_irq_on) return;
    psx_spu_irq_hit = 0;
    if (irq_cb) irq_cb();
}

void *SpuSetIRQCallback(void (*func)(void)) {
    void (*old)(void) = irq_cb;
    irq_cb = func;
    return (void *)old;
}

void SpuGetVoiceEnvelope(int v, short *envx) {
    *envx = v >= 0 && v < 24 ? (short)psx_spu_read(v * 16 + 0xc) : 0;
}
