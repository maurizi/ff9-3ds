#include "psyq_port.h"
#include <stdio.h>
#include <stdlib.h>

unsigned char psx_ram[PSX_RAM_SIZE] __attribute__((aligned(32)));
unsigned char psx_scratchpad[0x400] __attribute__((aligned(32)));
void port_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void o0b_btlbg_setOtShift(s32);
void o0b_btlbg_setBrightness(s32);
s32 o0b_btlbg_getBrightness(void);
s32 o0b_btlfx_rand(void);
s32 o0b_btlfx_randRange(s32, s32);
s32 o0b_btlfx_randCentered(s32);
void o0b_btlfx_randRotation(SVECTOR *);
s32 o0b_btlfx_sqrt(s32, s32);
s32 o0b_btlfx_lerpRatio(s32, s32, s32, s32);
int main(void) {
    unsigned int kind, seed, a, b, c, d, phase;
    while (scanf("%u %x %x %x %x %x %x", &kind, &seed, &a, &b, &c, &d, &phase) == 7) {
        *(u32 *)(psx_scratchpad + 0x3FC) = seed;
        *(u32 *)(psx_ram + 0xCF588) = c;
        *(u32 *)(psx_ram + 0xCF5A0) = d;
        *(u16 *)(psx_ram + 0xFE528) = phase;
        SVECTOR rot = {0};
        u32 value = 0;
        switch (kind) {
        case 0: o0b_btlbg_setOtShift(a); break;
        case 1: o0b_btlbg_setBrightness(a); break;
        case 2: value = o0b_btlbg_getBrightness(); break;
        case 3: value = o0b_btlfx_rand(); break;
        case 4: value = o0b_btlfx_randRange(a, b); break;
        case 5: value = o0b_btlfx_randCentered(a); break;
        case 6: o0b_btlfx_randRotation(&rot); break;
        case 7: value = o0b_btlfx_sqrt(a, b); break;
        case 8: value = o0b_btlfx_lerpRatio(a, b, c, d); break;
        default: abort();
        }
        printf("%08x %08x %08x %08x %04x %04x %04x %04x\n", value,
               *(u32 *)(psx_scratchpad + 0x3FC), *(u32 *)(psx_ram + 0xCF588),
               *(u32 *)(psx_ram + 0xCF5A0), *(u16 *)(psx_ram + 0xFE528),
               (u16)rot.vx, (u16)rot.vy, (u16)rot.vz);
    }
    return 0;
}
