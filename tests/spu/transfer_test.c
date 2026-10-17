#include <assert.h>
#include <stdio.h>
#undef NULL
#include "psyq_port.h"

extern u8 *psx_spu_ram;
s32 SpuRead(s32, s32);
u32 SpuWrite(u8 *, u32);
static u8 ram[0x80000], buffer[0x80000];
static unsigned callbacks;
static void complete(void) { ++callbacks; }

int main(void) {
    psx_spu_ram = ram;
    for (unsigned i = 0; i < sizeof ram; ++i) ram[i] = i ^ (i >> 8);
    __builtin_memset(buffer, 0xcc, sizeof buffer);
    SpuSetTransferCallback(complete);
    assert(SpuSetTransferStartAddr(0) == 0);
    /* Assembly at 8005703C clamps via SLTU to 0x7eff0, including negative s32.
     * The former stub added UINT_MAX to the address before clamping, which
     * could wrap and pass an enormous count to memcpy. */
    assert(SpuRead((s32)buffer, -1) == 0x7eff0);
    assert(callbacks == 1 && !__builtin_memcmp(ram, buffer, 0x7eff0));
    assert(buffer[0x7eff0] == 0xcc && buffer[0x7ffff] == 0xcc);
    assert(SpuSetTransferStartAddr(9) == 8);
    assert(SpuRead((s32)buffer, -16) == 0x7eff0);
    assert(!__builtin_memcmp(ram + 8, buffer, 0x7eff0));
    /* Port RAM extent policy also bounds transfers near the final byte. */
    assert(SpuSetTransferStartAddr(0x7fff8) == 0x7fff8);
    assert(SpuRead((s32)buffer, -1) == 8);
    assert(!__builtin_memcmp(ram + 0x7fff8, buffer, 8));
    assert(SpuWrite(buffer, 0xffffffffu) == 8);
    assert(SpuSetTransferStartAddr(0x80000) == 0);
    assert(callbacks == 4);
    puts("SPU transfers: unsigned size clamp, extents and callbacks PASS");
    return 0;
}
