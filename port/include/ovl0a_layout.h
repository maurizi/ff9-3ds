/* What the DS keeps in ovl_0a's MIPS code range while the field overlay is resident and the hybrid renderer draws
 * fields (FIELDGX=1): the port never runs that code, and nothing else lands there until another slot-0 overlay does
 * (port/psx/mem.c psx_overlay_patch). Offsets are from the overlay's load address (0x800A7000); its code runs
 * 0xA48-0x22CCC and holds no data the game reads. */
#ifndef OVL0A_LAYOUT_H
#define OVL0A_LAYOUT_H
#define OVL0A_AT            0xA7000   /* in psx_ram */
#define OVL0A_FIELD_PRIMS   1450      /* a field scene's primitives */
#define OVL0A_PRIMS_AT_0    0xA60     /* the hybrid's two scene buffers (gx_hybrid.c), 48 bytes a primitive */
#define OVL0A_PRIMS_AT_1    0x11A40
#define OVL0A_CODE_END      0x22CCC
#endif
