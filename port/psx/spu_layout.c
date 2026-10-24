/* How the shadow keeps SPU RAM (psx_mem_init sets it; spu.c's transfers and spu_source.c's block fetches follow it).
 * On the PC, and with software mixing, all of it at psx_spu_ram. With the DS's hardware mixing, which reads it only a
 * 16-byte ADPCM block at a time and never runs the PS1 reverb:
 * - not FF9's reverb work area at the top: from psx_spu_ram_size up it reads as zeros, as that unused area always
 *   did there; a game write there is dropped (logged once if it isn't zeros);
 * - in up to three pieces: below psx_spu_ram_split[0] at psx_spu_ram + address, then below psx_spu_ram_split[1]
 *   at psx_spu_ram_mid + address, the rest at psx_spu_ram_hi + address. */
unsigned psx_spu_ram_size = 0x80000, psx_spu_ram_split[2] = {0x80000, 0x80000};
unsigned char *psx_spu_ram_mid, *psx_spu_ram_hi;
