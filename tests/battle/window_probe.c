#define M2CTX
#define printf ff9_unused_printf
#include "ff9.h"
#undef printf
#include "psx_mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
s32 func_800632B4(s32 count, s8 *values) {
    s32 sum = 0;
    if (values) for (s32 i = 0; i < count; i++) sum += values[i];
    return sum;
}
int main(void) {
    if (mmap(psx_ram, PSX_RAM_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != psx_ram) abort();
    u32 args[2];
    while (fread(args, sizeof args, 1, stdin) == 1) {
        if (fread(psx_ram + 0x180000, 96, 1, stdin) != 1) abort();
        WinDef *w = (WinDef *)(psx_ram + 0x180000);
        TweenDef *t = (TweenDef *)(psx_ram + 0x180010);
        t->unk4 = args[0] & 2 ? (s8 *)(psx_ram + 0x180020) : NULL;
        t->unk8 = args[0] & 4 ? (s8 *)(psx_ram + 0x180040) : NULL;
        func_80063118(w, args[0] & 1 ? t : NULL, args[1]);
        /* Normalize only the stored native pointer for the PSX comparison. */
        u32 pointer = w->unkC ? 0x80180010 : 0;
        __builtin_memcpy((u8 *)w + 12, &pointer, 4);
        fwrite(w, 16, 1, stdout);
    }
}
