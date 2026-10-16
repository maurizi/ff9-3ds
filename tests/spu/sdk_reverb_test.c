#include <assert.h>
#include <stdio.h>
#undef NULL
#include "psyq_port.h"
#include "psx_spu.h"
#include "sdk_reverb_reference.h"

extern u8 *psx_spu_ram;
u32 func_8005709C(u8 *, u32);
static u8 ram[0x80000];
static u16 registers[0x200];
void psx_spu_reset(void) { __builtin_memset(registers, 0, sizeof registers); }
void psx_spu_write(unsigned offset, u16 value) { registers[offset / 2] = value; }
u16 psx_spu_read(unsigned offset) { return registers[offset / 2]; }
static u32 hash(u32 state, u32 value) { return (state ^ value) * 16777619u; }
static unsigned callbacks;
static void complete(void) { ++callbacks; }

int main(void) {
    psx_spu_ram = ram;
    SpuInit();
    SpuSetReverbModeType(0);
    SpuSetReverb(1);
    for (unsigned i = 0; i < sizeof sdk_reverb_cases / sizeof *sdk_reverb_cases; ++i) {
        SpuReverbAttr attr = {sdk_reverb_cases[i].mask, sdk_reverb_cases[i].mode,
            {sdk_reverb_cases[i].left, sdk_reverb_cases[i].right},
            sdk_reverb_cases[i].delay, sdk_reverb_cases[i].feedback};
        long result = 0;
        switch (sdk_reverb_cases[i].api) {
        case 0: result = SpuSetReverbModeType(attr.mode); break;
        case 1: result = SpuSetReverbModeParam(&attr); break;
        case 2: SpuSetReverbDepth(&attr); break;
        case 3: SpuSetReverb(attr.mode); break;
        }
        u32 h = hash(2166136261u, result);
        h = hash(h, psx_spu_read(0x184));
        h = hash(h, psx_spu_read(0x186));
        h = hash(h, psx_spu_read(0x1a2));
        h = hash(h, psx_spu_read(0x1aa) & 0x80);
        for (unsigned reg = 0x1c0; reg < 0x200; reg += 2) h = hash(h, psx_spu_read(reg));
        if (h != sdk_reverb_cases[i].hash) {
            printf("SDK reverb case %u: %08x != %08x\n", i, h, sdk_reverb_cases[i].hash);
            return 1;
        }
    }
    /* Clear is synchronous, preserves caller transfer address and callback,
     * and mode zero clears 128 bytes rather than the preset's 16-byte span. */
    SpuSetTransferCallback(complete);
    SpuSetTransferStartAddr(0x1000);
    __builtin_memset(ram, 0x5a, sizeof ram);
    assert(SpuClearReverbWorkArea(0) == 0 && callbacks == 0);
    assert(ram[0x7ff7f] == 0x5a && ram[0x7ff80] == 0 && ram[0x7ffff] == 0);
    u8 byte = 0x12;
    assert(func_8005709C(&byte, 1) == 1 && callbacks == 1 && ram[0x1000] == byte);
    assert(SpuSetReverbModeType(0x101) == 0);
    assert(ram[0x7d93f] == 0x5a && ram[0x7d940] == 0);
    assert(SpuClearReverbWorkArea(10) == -1);
    psx_spu_ram = NULL;
    assert(SpuClearReverbWorkArea(1) == 0);
    puts("SPU SDK reverb: original register oracle, masks and RAM clearing PASS");
    return 0;
}
