/* The DS's native code overlays (field, battle, world map, menus...), linked one slot's over each other outside the
 * executable's load segments (gen_link.py psx_ovl_areas.ld) and packed into ff9ds/ovls.bin
 * (tools/pack_overlays.py): when the game loads an overlay (port/psx/mem.c psx_overlay_patch), its native code is
 * read into place, as the PS1 reads the MIPS code from the disc. Without a pack built with this executable the
 * overlays can't run: that is fatal, with the reason. */
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"
#include "psx_runtime.h"
#include "psx_mem.h"

#define OVL_PACK_MAGIC 0x3150564Fu    /* "OVP1" */
extern const u32 psx_fx_build_id __attribute__((weak)); /* the Makefile's (fxid.s), new each link */

static FILE *pack;
static u32 (*entries)[4], count;      /* (id, address, size, offset) */
static int loaded[256];               /* by overlay id: its code is in place (another one of its slot clears it) */

static void open_pack(void) {
    if (pack) return;
    char path[256];
    snprintf(path, sizeof path, "%s/ovls.bin", plat_data_dir());
    pack = fopen(path, "rb");
    u32 head[3];
    if (!pack || fread(head, 4, 3, pack) != 3) port_fatal("code overlays: can't read %s", path);
    if (head[0] != OVL_PACK_MAGIC || head[1] != psx_fx_build_id)
        port_fatal("code overlays: %s is from another build", path);
    count = head[2];
    entries = malloc(count * sizeof *entries);
    if (!entries || fread(entries, sizeof *entries, count, pack) != count) port_fatal("code overlays: bad %s", path);
}

void port_ovl_native_load(unsigned int id) {
    if (id < 256 && loaded[id]) return;
    open_pack();
    for (u32 i = 0; i < count; ++i) {
        if (entries[i][0] != id) continue;
        u32 address = entries[i][1], size = entries[i][2];
        if (fseek(pack, (long)entries[i][3], SEEK_SET) != 0 || fread((void *)address, 1, size, pack) != size)
            port_fatal("code overlays: can't read overlay %02X", id);
        DC_FlushRange((void *)address, size); /* new code: out of the data cache, then a fresh instruction cache */
        IC_InvalidateAll();
        /* the overlays sharing this one's place are gone */
        for (u32 k = 0; k < count; ++k)
            if (entries[k][1] == address) loaded[entries[k][0] & 255] = 0;
        loaded[id & 255] = 1;
        return;
    }
}

/* The overlays linked in their own MIPS code range in psx_ram (gen_link.py: those of slots 1 and 2): disc bytes just
 * landed at psx_ram + [off, off + len) for overlay `id`; read its native code there again. */
void port_ovl_native_sector(unsigned int id, unsigned int off, unsigned int len) {
    open_pack();
    for (u32 i = 0; i < count; ++i) {
        if (entries[i][0] != id) continue;
        u32 lo = entries[i][1], hi = lo + entries[i][2], s_lo = (u32)psx_ram + off, s_hi = s_lo + len;
        if (lo < (u32)psx_ram || lo >= (u32)psx_ram + 0x200000) return; /* in an overlay slot, not in psx_ram */
        if (s_lo < lo) s_lo = lo;
        if (s_hi > hi) s_hi = hi;
        if (s_lo >= s_hi) return;
        if (fseek(pack, (long)(entries[i][3] + (s_lo - lo)), SEEK_SET) != 0 ||
            fread((void *)s_lo, 1, s_hi - s_lo, pack) != s_hi - s_lo)
            port_fatal("code overlays: can't read overlay %02X", id);
        DC_FlushRange((void *)s_lo, s_hi - s_lo);
        IC_InvalidateAll();
        return;
    }
}
