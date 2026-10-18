/* Battle effect modules compiled from the decomp's C (FXNATIVE in the Makefile) run natively; the others stay
 * in the effect VM (effect_vm.c). A module's image reaches 0x801E7700 as raw disc bytes. The first call into it
 * (an op through port_effect_call, its address read from the raw header) identifies it by a hash of its code
 * bytes (tools/gen_link.py psx_fxmods), applies its relocations as an overlay's (data pointers to psx_ram, code
 * pointers to the native functions: the header's ops become native, so later calls skip the VM), and answers
 * that call with the native function at the same PS1 address. */
#include <stdint.h>
#include "psyq_port.h"

typedef struct {
    u32 vram, text_lo, text_hi, hash; /* code range [text_lo, text_hi), FNV-1a of its bytes */
    const u32 *relocs;                /* as an overlay's (port/psx/mem.c psx_overlay_patch) */
    u32 nfuncs;
    const u32 *funcs;                 /* (PS1 address, native address) pairs */
} PsxFxmod;
extern const PsxFxmod psx_fxmods[];

#define SLOT 0x801E7700u

static const PsxFxmod *current;
static u32 seen[4]; /* the words that tell the slot changed: header pointer and three code words */

static u32 at(u32 vram) { return *(const u32 *)PSX_ADDR(vram); }

static int matches(const PsxFxmod *m) {
    u32 h = 0x811C9DC5u;
    for (const u8 *p = (const u8 *)PSX_ADDR(m->text_lo), *end = (const u8 *)PSX_ADDR(m->text_hi); p < end; ++p)
        h = (h ^ *p) * 0x01000193u;
    return h == m->hash;
}

static void relocate(const PsxFxmod *m) {
    for (const u32 *r = m->relocs; *r != 0xFFFFFFFFu; r += (*r >> 29) == 2 ? 2 : 1) {
        u32 *word = (u32 *)(psx_ram + (*r & 0x1FFFFF)), kind = *r >> 29;
        if (kind == 0 && (*word >> 24) == 0x80) *word = (u32)(uintptr_t)(psx_ram + (*word & 0x1FFFFF));
        else if (kind == 1 && (*word >> 16) == 0x1F80) *word = (u32)(uintptr_t)(psx_scratchpad + (*word & 0x3FF));
        else if (kind == 2) *word = r[1];
    }
}

/* The native function for a call into the module slot at PS1 address `target`, or 0 (the VM runs it). */
u32 port_fxmod_native(u32 target) {
    u32 now[4] = {at(SLOT), at(SLOT + 4), at(SLOT + 0x100), at(SLOT + 0x400)};
    if (now[0] != seen[0] || now[1] != seen[1] || now[2] != seen[2] || now[3] != seen[3]) {
        current = NULL;
        for (const PsxFxmod *m = psx_fxmods; m->vram; ++m)
            if (m->vram == SLOT && matches(m)) { current = m; break; }
        if (current) relocate(current);
        seen[0] = at(SLOT); seen[1] = at(SLOT + 4); seen[2] = at(SLOT + 0x100); seen[3] = at(SLOT + 0x400);
    }
    if (!current) return 0;
    for (u32 i = 0; i < current->nfuncs; ++i)
        if (current->funcs[2 * i] == target) return current->funcs[2 * i + 1];
    return 0;
}
