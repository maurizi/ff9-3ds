/* What the DS keeps in ovl_0c's MIPS code range while the world map overlay is resident and the hybrid renderer
 * draws it (WORLDGX=1): the port never runs that code, and nothing else lands there until another slot-0 overlay
 * does (port/psx/mem.c psx_overlay_patch). Offsets are from the overlay's load address (0x800A7000); its code runs
 * 0x1860-0x25A10 (config/overlays.us.txt) and holds no data the game reads. */
#ifndef OVL0C_LAYOUT_H
#define OVL0C_LAYOUT_H
#define OVL0C_AT            0xA7000   /* in psx_ram */
#define OVL0C_PRIMS         560       /* a world scene's recorded primitives per buffer (the terrain is native) */
#define OVL0C_PRIMS_AT_0    0x1880    /* the hybrid's two scene buffers (gx_hybrid.c), 48 bytes a primitive */
#define OVL0C_PRIMS_AT_1    (OVL0C_PRIMS_AT_0 + OVL0C_PRIMS * 48)
#define OVL0C_PRIMTAB_AT    (OVL0C_PRIMS_AT_1 + OVL0C_PRIMS * 48)   /* the per-primitive tables, 4 bytes each */
#define OVL0C_SLOTS_AT      (OVL0C_PRIMTAB_AT + OVL0C_PRIMS * 4)    /* each buffer's primitives' depths, 2 bytes */
#define OVL0C_TERRAIN_AT    (OVL0C_SLOTS_AT + 2 * OVL0C_PRIMS * 2)  /* three frames' terrain (world terrain) */
#define OVL0C_TERRAIN_BYTES 0x7200
#define OVL0C_CODE_END      0x25A10
#endif
