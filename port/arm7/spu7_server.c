/* The ARM7's side of the SPU (spu7.h): the voices and the hardware channels, driven by the ARM9's commands in its
 * order; what the game reads back published after each advance. */
#include <nds.h>
#include <string.h>
#include "spu7.h"
#include "psx_spu.h"

void port_hw_sound_init_arm7(int16_t *rings, int16_t *echo);
void port_hw_sound_frame(void);

unsigned short psx_spu_regs[0x200];
unsigned char *psx_spu_ram;
extern unsigned psx_spu_ram_size, psx_spu_ram_split[2];
extern unsigned char *psx_spu_ram_mid, *psx_spu_ram_hi;

/* (the software mixer's output: never reached with hardware mixing) */
void plat_audio_samples(const int16_t *samples, unsigned frames) {
    (void)samples; (void)frames;
}

static Spu7Shared *shared;
static u32 ram_at;

static void kicked(u32 value, void *user) {
    (void)user;
    if (value && !shared) {
        Spu7Shared *s = (Spu7Shared *)value;
        psx_spu_ram = s->ram;
        psx_spu_ram_mid = s->ram_mid;
        psx_spu_ram_hi = s->ram_hi;
        psx_spu_ram_size = s->ram_size;
        psx_spu_ram_split[0] = s->ram_split[0];
        psx_spu_ram_split[1] = s->ram_split[1];
        port_hw_sound_init_arm7(s->rings, s->echo);
        shared = s;
    }
}

/* n bytes of SPU RAM at `at`, piece by piece (as spu.c's spu_ram_span): in from src, out to dst, or cleared */
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
        at += len; n -= len;
    }
}

static void publish(u32 done) {
    shared->published++; /* (odd: being written) */
    Spu7Readback *r = &shared->readback;
    for (int i = 0; i < SPU7_VOICES; ++i) {
        r->voice[i][0] = psx_spu_read(i * 16 + 12);
        r->voice[i][1] = psx_spu_read(i * 16 + 14);
        r->volume[i][0] = psx_spu_read(0x200 + i * 4);
        r->volume[i][1] = psx_spu_read(0x202 + i * 4);
    }
    r->ended[0] = psx_spu_read(0x19c);
    r->ended[1] = psx_spu_read(0x19e);
    r->main_volume[0] = psx_spu_read(0x1b8);
    r->main_volume[1] = psx_spu_read(0x1ba);
    shared->published_at = done;
    shared->published++;
}

/* the commands the ARM9 has written so far */
void spu7_service(void) {
    if (!shared) return;
    u32 tail = shared->tail;
    while (tail != shared->head) {
        const Spu7Cmd c = shared->cmd[tail % SPU7_RING];
        u32 arg = c.a & 0xFFFFFF;
        switch (c.a >> 24) {
        case SPU7_WRITE: psx_spu_write(arg, (uint16_t)c.b); break;
        case SPU7_ADVANCE: psx_spu_advance(c.b * 96); publish(tail + 1); break;
        case SPU7_FRAME: port_hw_sound_frame(); break;
        case SPU7_ADDR: ram_at = arg; break;
        case SPU7_RAM_IN: ram_span(ram_at, arg, (const u8 *)c.b, NULL); shared->transfers++; break;
        case SPU7_RAM_OUT: ram_span(ram_at, arg, NULL, (u8 *)c.b); shared->transfers++; break;
        case SPU7_RAM_CLEAR: ram_span(ram_at, arg, NULL, NULL); shared->transfers++; break;
        case SPU7_RESET: psx_spu_reset(); publish(tail + 1); break;
        default: break;
        }
        shared->tail = ++tail;
    }
}

void spu7_init(void) {
    fifoSetValue32Handler(SPU7_FIFO, kicked, NULL);
}
