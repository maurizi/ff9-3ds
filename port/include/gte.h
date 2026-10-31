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
/* NCLIP and AVSZ3/AVSZ4 in place, as gte.c computes them (MAC0's flags, OTZ's saturation): about half of a
 * battle's commands, each a few multiplies behind a call and a dispatch otherwise. */
void gte_cmd(gte_u32 op);
/* DS mode: MVMVA sf=1 lm=0 with RT (rtir, rtv0tr, rtv0) and GPF sf=1 lm=0 in 32 bits (gte.c) */
void psx_gte_mvmva_rt12(gte_u32 op);
/* a vertex projected as RTPS/RTPT would, the registers untouched (gte.c) */
void psx_gte_project(gte_u32 vxy, gte_u32 vz, gte_u32 *sxy, gte_u32 *sz, gte_u32 *mac0);
/* the UNR division's table (gte.c), for port code dividing as RTPS does */
const unsigned short *psx_gte_unr(void);
void psx_gte_gpf12(void);
/* RTPT's depths alone, for a caller that overwrites the rest before reading it (gte.c) */
void psx_gte_rtpt_depths(void);
/* RTPT's projection over many vertices (gte.c) */
void gte_rtpt_batch(const gte_u32 *v, int count, gte_u32 *sxy, gte_u32 *sz);
static inline __attribute__((always_inline)) void gte_mac0_flags(long long m, gte_u32 flag) {
    if (m > 0x7FFFFFFFLL) flag |= 1u << 16;
    else if (m < -0x80000000LL) flag |= 1u << 15;
    psx_gte.d[24] = (gte_u32)(int)m;
    psx_gte.c[31] = flag & 0x7F87E000 ? flag | 0x80000000u : flag;
}
static inline __attribute__((always_inline)) void gte_cmd_inline(gte_u32 op) {
    if (__builtin_constant_p(op) && (op & 0x3F) == 0x06) { /* NCLIP */
        const gte_u32 *d = psx_gte.d;
        int x0 = (short)d[12], y0 = (short)(d[12] >> 16), x1 = (short)d[13], y1 = (short)(d[13] >> 16);
        int x2 = (short)d[14], y2 = (short)(d[14] >> 16);
        /* (coordinates within +-8192, as on screen: (x1-x0)(y2-y0) - (x2-x0)(y1-y0), the same value, fits 32
         * bits with no MAC0 flag; Thumb code has no long multiply, each 64-bit product a library call) */
        if ((unsigned)(x0 + 8192) < 16384u && (unsigned)(y0 + 8192) < 16384u && (unsigned)(x1 + 8192) < 16384u &&
            (unsigned)(y1 + 8192) < 16384u && (unsigned)(x2 + 8192) < 16384u && (unsigned)(y2 + 8192) < 16384u) {
            psx_gte.d[24] = (gte_u32)((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
            psx_gte.c[31] = 0;
        } else
            gte_mac0_flags((long long)x0 * y1 + (long long)x1 * y2 + (long long)x2 * y0 - (long long)x0 * y2 -
                           (long long)x1 * y0 - (long long)x2 * y1, 0);
    } else if (__builtin_constant_p(op) && ((op & 0x3F) == 0x2D || (op & 0x3F) == 0x2E)) { /* AVSZ3, AVSZ4 */
        const gte_u32 *d = psx_gte.d;
        unsigned int sum = (unsigned short)d[17] + (unsigned short)d[18] + (unsigned short)d[19];
        short zsf = (short)psx_gte.c[29];
        if ((op & 0x3F) == 0x2E) { sum += (unsigned short)d[16]; zsf = (short)psx_gte.c[30]; }
        if (sum < 0x10000u) { /* (|zsf * sum| < 2^31: 32 bits, no MAC0 flag) */
            int m = zsf * (int)sum, z = m >> 12;
            if (z > 0xFFFF || z < 0) { psx_gte.d[7] = z < 0 ? 0 : 0xFFFF; psx_gte.d[24] = (gte_u32)m; psx_gte.c[31] = 0x80000000u | 1u << 18; }
            else { psx_gte.d[7] = (gte_u32)z; psx_gte.d[24] = (gte_u32)m; psx_gte.c[31] = 0; }
            return;
        }
        long long m = (long long)zsf * sum, z = m >> 12;
        gte_u32 flag = 0;
        if (z > 0xFFFF) { z = 0xFFFF; flag = 1u << 18; } else if (z < 0) { z = 0; flag = 1u << 18; }
        psx_gte.d[7] = (gte_u32)z;
        gte_mac0_flags(m, flag);
#ifdef PORT_GTE_APPROX
    } else if (__builtin_constant_p(op) && ((op & 0x1FFFFFF) == 0x049E012 || (op & 0x1FFFFFF) == 0x0480012 ||
                                            (op & 0x1FFFFFF) == 0x0486012)) {
        psx_gte_mvmva_rt12(op); /* (gte.c: a call no larger than gte_cmd's) */
    } else if (__builtin_constant_p(op) && (op & 0x1FFFFFF) == 0x198003D) {
        psx_gte_gpf12();
#endif
    } else {
        gte_cmd(op);
    }
}

#ifndef PORT_GTE_IMPL
#define gte_cmd(op) gte_cmd_inline(op)
#define gte_mtc2(reg, value) gte_mtc2_inline((reg), (value))
#define gte_mfc2(reg) gte_mfc2_inline(reg)
#define gte_ctc2(reg, value) gte_ctc2_inline((reg), (value))
#endif

/* Execute a COP2 command word (the low 25 bits of the `.word 0x4Axxxxxx` the macros emit). */
void gte_cmd(gte_u32 op);

/* lwc2/swc2 */
#define GTE_LWC2(reg, base, off) gte_mtc2((reg), *(const gte_u32 *)((const unsigned char *)(base) + (off)))
#ifdef PORT_GTE_DEPTH
/* Depth capture (docs/design.md 4.3): each SXY register's sub-pixel fraction and SZ, tagged with the word it
 * belongs to (a direct mtc2 replaces the word, which then no longer matches), recorded wherever the game stores
 * an SXY register, so the renderer finds a projected vertex's depth by its address. info: SZ << 16 | the y
 * fraction (1/256 pixel) << 8 | the x fraction; 0 = none. */
typedef struct { gte_u32 word, info; } GteDepth;
extern GteDepth psx_gte_depth[3];
void gte_store_sxy(int reg, void *p);
/* the depth info recorded for the vertex word at p (in psx_ram or the scratchpad), 0 if none or stale */
gte_u32 psx_depth_lookup(const gte_u32 *p);
/* port code that projects vertices itself (gte_rtpt_batch, psx_gte_project) records them like a store */
void psx_depth_record(const void *p, gte_u32 word, gte_u32 info);
#define GTE_SWC2(reg, base, off) ((reg) >= 12 && (reg) <= 14 ? gte_store_sxy((reg), (unsigned char *)(base) + (off)) \
                                  : (void)(*(gte_u32 *)((unsigned char *)(base) + (off)) = gte_mfc2(reg)))
#else
#define GTE_SWC2(reg, base, off) (*(gte_u32 *)((unsigned char *)(base) + (off)) = gte_mfc2(reg))
#endif

#endif
