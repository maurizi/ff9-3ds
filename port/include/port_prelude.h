/* Force-included (-include) before every decomp C file.
 *
 * 1. Turn off the decomp's asm includes: INCLUDE_ASM/INCLUDE_RODATA become nothing, so a function still in asm is
 *    an undefined symbol (supplied by src_equiv/ or port/, or reported by the link).
 * 2. Include the file's own header (PORT_HEADER: ff9.h or ovl_XX.h) now, then replace the hardware macros it
 *    defined (GTE, SPU registers, scratchpad stack, inline-asm addPrim) with port versions. The file's own
 *    #include of that header is then a no-op thanks to its include guard.
 */
#ifndef PORT_PRELUDE_H
#define PORT_PRELUDE_H

#define INCLUDE_ASM(FOLDER, NAME)
#define INCLUDE_RODATA(FOLDER, NAME)
#define M2CTX /* include_asm.h: skip the top-level `.include "labels.inc"` */

/* BIOS calls the decomp names after their libc counterparts. Their PSX behaviour differs (memory card file
 * descriptors, KUSEG string pointers), so they link to port/psx's psx_bios_* instead of replacing the C library. */
#define strcpy psx_bios_strcpy
#define memcpy psx_bios_memcpy
#define strcat psx_bios_strcat
#define open psx_bios_open
#define lseek psx_bios_lseek
#define read psx_bios_read
#define write psx_bios_write
#define close psx_bios_close
#define nextfile psx_bios_nextfile

#include PORT_HEADER
#include "port_hw.h"
#include "psx_runtime.h"
#include "effect_vm.h"
void port_measure_text_into(void *out, const void *text);
void battleEnd_main(void);
void port_draw_ot_buffer(unsigned int *head, const void *buffer, unsigned int size);

#endif
