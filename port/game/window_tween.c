/* The window-animation entry at 80063118 follows 63108's BIOS wrapper. */
#define M2CTX
#include "ff9.h"
void func_80063118(void *window, void *animation, s32 rewind) {
    WinDef *w = window;
    TweenDef *t = animation;
    w->unkC = t;
    w->unk2 = 0;
    w->unk0 = 0;
    if (!t) return;
    /* The two bytes named pad0 actually hold the initial delay. */
    __builtin_memcpy(&w->unk0, t, sizeof w->unk0);
    if (!(rewind & 255)) return;
    s32 x = func_800632B4(t->unk2, t->unk4);
    w->unk4.x = (s16)((u16)w->unk4.x - (u32)x);
    s32 y = func_800632B4(t->unk2, t->unk8);
    w->unk4.y = (s16)((u16)w->unk4.y - (u32)y);
}
