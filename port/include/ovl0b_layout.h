/* What the DS keeps in ovl_0b's MIPS code range while the battle overlay is resident (the port never runs that
 * code: it is dead PS1 RAM until another overlay lands there, port/psx/mem.c psx_overlay_patch). Offsets are
 * from the overlay's load address (0x800A7000); code runs 0x159C-0x28580 and 0x28C0C-0x4E1E8 (the 36 bytes at
 * 0x28580 are data the game reads: decomp-requests F33). tools/fx_extern.py reads these too. */
#ifndef OVL0B_LAYOUT_H
#define OVL0B_LAYOUT_H
#define OVL0B_AT            0xA7000   /* in psx_ram */
#define OVL0B_PRIMS_AT_0    0x15A0    /* the hybrid's two scene buffers (gx_hybrid.c) */
#define OVL0B_PRIMS_AT_1    0x28C20
#define OVL0B_FX_AREA_AT    0x1EA80   /* the loaded effect module's native code (port/ds/fx_pack.c) */
#define OVL0B_FX_AREA_END   0x26000   /* (the largest module's native code is 14 KB) */
#define OVL0B_RAMCODE_AT    0x26000   /* battle-only port code (native_mesh.h PORT_RAM_CODE(0b)), read with the overlay */
#define OVL0B_RAMCODE_END   0x28580
#define OVL0B_RANKS_AT      0x46100   /* the hybrid's native-mesh rank tables (2 x 8 KB) */
#define OVL0B_PRIMTAB_AT    0x4A160   /* its per-primitive tables (4 bytes a primitive) */
#define OVL0B_HEADS_AT      0x4B440   /* its native polygons' list heads per main-OT entry (8 KB) */
#define OVL0B_PLACES_AT     0x4D460   /* its drawing places per 4 main-OT entries (2 KB, model strips) */
#define OVL0B_OCC_BG_AT     0x4DC60   /* its occlusion cull's arena polygons' opacity (1 KB); after it free */
#define OVL0B_CODE_END      0x4E1E8
#endif
