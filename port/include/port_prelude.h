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

/* tools/portify.py puts this on each 8/16-bit scalar parameter at the top of its function: the PS1's callee narrows
 * such arguments itself, while ARM AAPCS trusts the caller to have done it (wrong when the caller saw a wider
 * prototype). The empty asm makes GCC forget the incoming value is already extended, so it re-extends at use. */
#ifdef __arm__
#define PORT_NARROW_ARG(p) __asm__("" : "+r"(p))
#else
#define PORT_NARROW_ARG(p) ((void)0)
#endif

/* Fixed PS1 RAM addresses the decomp uses as memory (ff9.h's PSX_RAM): into psx_ram */
#include "psx_mem.h"
#define PSX_RAM(type, addr) ((type)PSX_ADDR(addr))
/* A pointer the original code dereferences while NULL (ff9.h's PSX_NULLABLE, at every known site): low RAM there */
#define PSX_NULLABLE(p) PSX_NZ(p)
/* a value holding either a small number or a pointer, which the PS1 tells apart by the sign (KSEG0, 0x8xxxxxxx):
 * a native pointer is positive (evt_moveObjectTo's flags for EVT_OP_MOVE_TO_OBJ: decomp-requests F38) */
#define PSX_IS_PTR(v) ((u32)(v) > 0xFFFFu)
#include PORT_HEADER
#include "port_hw.h"
#ifdef BTLFX_EXPORTS
/* Battle effect modules (btlfx_mod.h) reach ovl_0b through env->unk10, which always holds g_btlfxModuleExports.
 * Some ops read it before checking whether they were given an env at all (mode 0 calls pass NULL): harmless on
 * the PS1 (a low-RAM read), a fault natively. Modules built for the port take the table's address directly. */
#include "psx_mem.h"
#undef BTLFX_EXPORTS
#define BTLFX_EXPORTS(env) ((void)(env), (BtlfxExports *)PSX_ADDR(0x800F7510))
#endif
#ifdef RAND16
/* ovl_0b.h: unsequenced in C; the PS1 reads the byte (twice) before the increment */
#undef RAND16
#define RAND16() ({ u32 port_b = *g_randPtr++; (s32)(port_b << 8 | port_b); })
#endif
#include "psx_runtime.h"
#include "effect_bridge.h"
void battleEnd_main(void);
void port_draw_ot_buffer(unsigned int *head, const void *buffer, unsigned int size);
void port_tm_frame(void *tm_state); /* port/game/tm_ui.c: the card game's frame, after its pad read (a patch) */

#endif
