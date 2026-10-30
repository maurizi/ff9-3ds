/* The DS's battle effect modules, linked on their own at psx_fx_area (Makefile, ds_FXOVL) and packed into
 * ff9ds/fxmods.bin (tools/pack_fxmods.py): reads the one the module slot holds, as the PS1 reads it from the disc
 * (port/game/effect_native.c). Kept apart from effect_native.c, whose PS1 headers clash with libnds and libc. */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "psx_runtime.h"

#define FX_HEAD_MAGIC 0x314D5846u     /* "FXM1": a module's first word (gen_link.py fxhead_<module>.s) */
#define FX_PACK_MAGIC 0x31505846u     /* "FXP1" */
/* in ovl_0b's dead MIPS code range while battle is resident (ovl0b_layout.h; the link defines it, fx_extern.py);
 * each module's link asserts it fits. Weak for the executable's first (scan) link. */
extern u8 psx_fx_area[] __attribute__((weak));
extern const u8 psx_fx_area_size[] __attribute__((weak)); /* its address is the size */
#define AREA_SIZE ((u32)(uintptr_t)psx_fx_area_size)
extern unsigned char psx_ram[];
extern const u32 psx_fx_build_id __attribute__((weak)); /* the Makefile's (fxid.s), new each link; weak for its scan */

static FILE *pack;
static u32 pack_count; /* (each module's {offset, size} read from the pack as it loads: the 3 KB index in the heap
                         * made the first effect of a battle fail when a few KB were left) */
static int pack_state;                /* 0 not opened yet, 1 usable, -1 not */
static int loaded[2] = {-1, -1}; /* each slot's module (its area: the bottom, or the top FX_SLOT2_SIZE bytes) */
#define FX_SLOT2_SIZE 0x3000u /* (the Makefile's: the _1 modules are linked there) */

static int open_pack(void) {
    if (pack_state) return pack_state > 0;
    pack_state = -1;
    char path[256];
    snprintf(path, sizeof path, "%s/fxmods.bin", plat_data_dir());
    pack = fopen(path, "rb");
    if (pack) setvbuf(pack, NULL, _IONBF, 0); /* (no buffer from the heap: the reads are whole modules) */
    u32 head[4];
    if (!pack || fread(head, 4, 4, pack) != 4) {
        port_log("effect modules: no %s\n", path);
        return 0;
    }
    if (head[0] != FX_PACK_MAGIC || head[1] != psx_fx_build_id || head[3] != (u32)(uintptr_t)psx_fx_area) {
        port_log("effect modules: %s is from another build\n", path);
        return 0;
    }
    pack_count = head[2];
    pack_state = 1;
    return 1;
}

void port_fxmod_forget(void); /* effect_native.c */

/* port/psx/mem.c: PS1 RAM [off, off + len) was overwritten (an overlay landing): a module there is gone */
void port_fx_area_overwritten(unsigned int off, unsigned int len) {
    u32 at = (u32)(psx_fx_area - psx_ram);
    if ((loaded[0] >= 0 || loaded[1] >= 0) && off < at + AREA_SIZE && at < off + len) {
        loaded[0] = loaded[1] = -1;
        port_fxmod_forget();
    }
}

/* Module `index` in psx_fx_area (read unless it is already there), or NULL. Its header: magic, the zero-filled
 * range, then what effect_native.c uses. */
const void *port_fx_pack_load(unsigned int index, int slot) {
    if (!open_pack() || index >= pack_count) return NULL;
    u8 *const at = psx_fx_area + (slot ? AREA_SIZE - FX_SLOT2_SIZE : 0);
    const u32 room = slot ? FX_SLOT2_SIZE : AREA_SIZE;
    const u32 *h = (const u32 *)at;
    if ((int)index == loaded[slot]) return h;
    loaded[slot] = -1;
    u32 entry[2];
    if (fseek(pack, (long)(16 + 8 * index), SEEK_SET) != 0 || fread(entry, 4, 2, pack) != 2) {
        port_log("effect modules: module %u's index unreadable\n", index);
        return NULL;
    }
    const u32 offset = entry[0], size = entry[1];
    if (size > room || fseek(pack, (long)offset, SEEK_SET) != 0 ||
        fread(at, 1, size, pack) != size || h[0] != FX_HEAD_MAGIC || h[2] < h[1] ||
        h[1] < (u32)(uintptr_t)at || h[2] > (u32)(uintptr_t)(at + room)) {
        port_log("effect modules: module %u unreadable\n", index);
        return NULL;
    }
    memset((void *)(uintptr_t)h[1], 0, h[2] - h[1]);
    /* the other slot's module, if this one reached into its part of the area */
    if (!slot && h[2] > (u32)(uintptr_t)(psx_fx_area + AREA_SIZE - FX_SLOT2_SIZE) && loaded[1] >= 0) {
        loaded[1] = -1;
        port_fxmod_forget();
    }
    if (slot && loaded[0] >= 0 && *(const u32 *)psx_fx_area == FX_HEAD_MAGIC &&
        ((const u32 *)psx_fx_area)[2] > (u32)(uintptr_t)at) {
        loaded[0] = -1;
        port_fxmod_forget();
    }
    /* new code: out of the data cache, then a fresh instruction cache */
    DC_FlushRange(psx_fx_area, AREA_SIZE);
    IC_InvalidateAll();
    loaded[slot] = (int)index;
    return h;
}
