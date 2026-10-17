/* Software GTE (PS1 COP2): a register-level model, so the decomp's GTE macro sequences (load, command, store,
 * split across C statements) translate one instruction at a time and keep their state between statements.
 * Behaviour follows psx-spx ("Geometry Transformation Engine"), including saturation and the UNR division.
 */
#ifndef PORT_GTE_H
#define PORT_GTE_H

typedef unsigned int gte_u32;

/* MIPS mtc2/mfc2 (data registers 0-31) and ctc2/cfc2 (control registers 0-31). */
void gte_mtc2(int reg, gte_u32 value);
gte_u32 gte_mfc2(int reg);
void gte_ctc2(int reg, gte_u32 value);
gte_u32 gte_cfc2(int reg);

/* The register file (gte.c). The decomp's macros pass constant register numbers: the plain moves below compile
 * to a load or store, and only the registers with side effects or special reads call the functions above. */
extern struct psx_gte_regs { gte_u32 d[32], c[32]; } psx_gte;

static inline __attribute__((always_inline)) void gte_mtc2_inline(int reg, gte_u32 value) {
    if (__builtin_constant_p(reg) && reg != 15 && (reg < 28 || reg > 31)) psx_gte.d[reg] = value;
    else gte_mtc2(reg, value);
}
static inline __attribute__((always_inline)) gte_u32 gte_mfc2_inline(int reg) {
    if (__builtin_constant_p(reg)) {
        if (reg == 1 || reg == 3 || reg == 5 || (reg >= 8 && reg <= 11))
            return (gte_u32)(int)(short)psx_gte.d[reg];
        if (reg == 7 || (reg >= 16 && reg <= 19)) return psx_gte.d[reg] & 0xFFFF;
        if (reg == 15) return psx_gte.d[14];
        if (reg != 28 && reg != 29) return psx_gte.d[reg];
    }
    return gte_mfc2(reg);
}
static inline __attribute__((always_inline)) void gte_ctc2_inline(int reg, gte_u32 value) {
    if (__builtin_constant_p(reg) && reg != 31) psx_gte.c[reg] = value;
    else gte_ctc2(reg, value);
}
#ifndef PORT_GTE_IMPL
#define gte_mtc2(reg, value) gte_mtc2_inline((reg), (value))
#define gte_mfc2(reg) gte_mfc2_inline(reg)
#define gte_ctc2(reg, value) gte_ctc2_inline((reg), (value))
#endif

/* Execute a COP2 command word (the low 25 bits of the `.word 0x4Axxxxxx` the macros emit). */
void gte_cmd(gte_u32 op);

/* lwc2/swc2 */
#define GTE_LWC2(reg, base, off) gte_mtc2((reg), *(const gte_u32 *)((const unsigned char *)(base) + (off)))
#define GTE_SWC2(reg, base, off) (*(gte_u32 *)((unsigned char *)(base) + (off)) = gte_mfc2(reg))

#endif
