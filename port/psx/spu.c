/* libspu: the SPU as the game sees it, for the port's sound path.
 *
 * - SPU RAM (512 KB) is a shadow; SpuWrite/SpuRead transfer to and from it at the transfer address.
 * - Voice/global writes reach spu_core.c through the PsyQ API or AKAO boundary overrides.
 *   psx_spu_regs[] remains a shadow for direct hardware readbacks.
 * - Transfers complete immediately; the transfer callback runs before SpuWrite returns.
 * Host --audio enables sample-clocked dry PCM rendering. DS stream output and
 * reverb/IRQ/DMA fidelity remain unfinished; see docs/audio-validation.md.
 */
#include "psyq_port.h"
#include "psx_spu.h"

#define SPU_RAM 0x80000

u8 *psx_spu_ram; /* allocated by psx_mem_init (NULL on a 4 MB DS: transfers are dropped) */

static u32 transfer_addr;
static void (*transfer_cb)(void);
static void (*irq_cb)(void);
static int reverb_on;
static long reverb_type;
static SpuReverbAttr reverb_attr;

void SpuInit(void) {
    psx_spu_reset();
    if (psx_spu_ram) {
        __builtin_memset(psx_spu_ram, 0, SPU_RAM);
    }
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

/* SpuWrite (decomp func_8005709C): copy `size` bytes (clamped like libspu, to 0x7EFF0) into SPU RAM. */
u32 func_8005709C(u8 *addr, u32 size) {
    if (size > 0x7EFF0) {
        size = 0x7EFF0;
    }
    if (transfer_addr + size > SPU_RAM) {
        size = SPU_RAM - transfer_addr;
    }
    if (psx_spu_ram) {
        __builtin_memcpy(psx_spu_ram + transfer_addr, addr, size);
    }
    if (transfer_cb) {
        transfer_cb();
    }
    return size;
}

/* SpuRead (decomp func_8005703C) */
s32 func_8005703C(s32 addr, s32 size) {
    u32 n = (u32)size;
    /* Original libspu uses an unsigned comparison, just like SpuWrite. Bound
     * before adding transfer_addr so a negative/oversized size cannot wrap. */
    if (n > 0x7eff0) n = 0x7eff0;
    if (transfer_addr + n > SPU_RAM) {
        n = SPU_RAM - transfer_addr;
    }
    if (psx_spu_ram) {
        __builtin_memcpy((void *)addr, psx_spu_ram + transfer_addr, n);
    }
    if (transfer_cb) {
        transfer_cb();
    }
    return (s32)n;
}

void SpuSetReverb(int on_off) {
    reverb_on = on_off;
    psx_spu_write(0x1aa, (psx_spu_read(0x1aa) & ~0x80) | (on_off ? 0x80 : 0));
}

long SpuSetReverbModeType(long mode) {
    reverb_type = mode;
    return 0;
}

long SpuSetReverbModeParam(SpuReverbAttr *attr) {
    reverb_attr = *attr;
    return 0;
}

void SpuSetReverbDepth(SpuReverbAttr *attr) {
    reverb_attr.depth = attr->depth;
}

void SpuSetReverbModeDepth(short l, short r) {
    reverb_attr.depth.left = l;
    reverb_attr.depth.right = r;
    psx_spu_write(0x184, l);
    psx_spu_write(0x186, r);
}

long SpuClearReverbWorkArea(long mode) {
    return 0;
}

long SpuSetNoiseClock(long n) {
    if (n < 0) n = 0;
    if (n > 63) n = 63;
    psx_spu_write(0x1aa, (psx_spu_read(0x1aa) & 0xc0ff) | (n << 8));
    return n;
}

int SpuSetIRQ(int on_off) {
    if (on_off == 0 || on_off == 3) psx_spu_write(0x1aa, psx_spu_read(0x1aa) & ~0x40);
    if (on_off == 1 || on_off == 3) psx_spu_write(0x1aa, psx_spu_read(0x1aa) | 0x40);
    return on_off;
}

void SpuSetIRQAddr(u32 addr) {
    psx_spu_write(0x1a4, addr >> 3);
}

void *SpuSetIRQCallback(void (*func)(void)) {
    void (*old)(void) = irq_cb;
    irq_cb = func;
    return (void *)old;
}

void SpuGetVoiceEnvelope(int v, short *envx) {
    *envx = v >= 0 && v < 24 ? (short)psx_spu_read(v * 16 + 0xc) : 0;
}
