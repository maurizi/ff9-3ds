/* Battle effect modules' own GTE and stack macros (decomp include/fxmod/fx_XXX.h), as the port's GTE calls.
 * tools/portify.py includes this right after a module's header, so these definitions replace its MIPS asm. */
#ifndef PORT_FXMOD_GTE_H
#define PORT_FXMOD_GTE_H

/* mfc2 $19 (SZ3); sra 2; sw */
#undef gte_stszotz
#define gte_stszotz(r0) (*(s32 *)(r0) = (s32)gte_mfc2(19) >> 2)
/* VZ0, VZ1, VZ2 = 0 */
#undef gte_ldVZ012_0
#define gte_ldVZ012_0() (gte_mtc2(1, 0), gte_mtc2(3, 0), gte_mtc2(5, 0))
/* lwc2 VXY0, VXY1, VXY2 */
#undef gte_ldv3xy
#define gte_ldv3xy(r0, r1, r2) do { GTE_LWC2(0, (r0), 0); GTE_LWC2(2, (r1), 0); GTE_LWC2(4, (r2), 0); } while (0)
#undef gte_ldv0xy
#define gte_ldv0xy(r0) GTE_LWC2(0, (r0), 0)
/* cfc2 $26 (H); sw */
#undef gte_ReadGeomScreen
#define gte_ReadGeomScreen(r0) (*(u32 *)(r0) = gte_cfc2(26))
/* swc2 SXY0, SXY1 */
#undef gte_stsxy01
#define gte_stsxy01(r0, r1) do { GTE_SWC2(12, (r0), 0); GTE_SWC2(13, (r1), 0); } while (0)
/* fx_1b6/fx_1b7 run a routine on a stack in the scratchpad (the PS1's fast RAM); natively the stack stays put */
#undef FX1B6_SPAD_STACK
#define FX1B6_SPAD_STACK(save) ((void)(save))
#undef FX1B6_RESET_STACK
#define FX1B6_RESET_STACK(save) ((void)(save))
#undef FX1B7_SPAD_STACK
#define FX1B7_SPAD_STACK(save) ((void)(save))
#undef FX1B7_RESET_STACK
#define FX1B7_RESET_STACK(save) ((void)(save))

#endif
