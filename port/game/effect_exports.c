/* Battle effect modules read $v0 after three exports that ovl_0b defines as void (decomp docs/effect-modules.md,
 * "Void exports whose result is read"): on the PS1 the register still holds what each one's tail call returned.
 * Native void functions leave nothing defined there, so their entries in the export table modules call through
 * (g_btlfxModuleExports, ovl_0b data) point at these instead, for the effect VM and native modules alike; ovl_0b
 * calls the originals directly. Each does what the export does and returns that tail call's value. */
#include <stdint.h>
#include "psyq_port.h"

/* The layouts used here, as decomp include/ovl_0b.h defines them (that header needs the decomp's asm glue). */
typedef struct { u16 unk0; u16 unk2; } Unk800E8F50Entry;
typedef struct {
    u16 unk0; /* magic 'so' */
    u16 unk2;
    u16 unk4; /* offset */
    u16 unk6; /* offset */
    Unk800E8F50Entry unk8[1];
} Unk800E8F50;
typedef struct { void *unk0; /* BtlUnit9C * */ } BtlObjSlotHead;

u16 o0b_Hi_RegisterTexListModel(void *, void *, s16 *, s32);
u16 o0b_Hi_RegisterGouEffModel(void *);
BtlObjSlotHead *o0b_btlfx_getEffModel(s32, char *);
u8 o0b_func_800DD10C(void *);
u16 o0b_func_800DF7A0(void *, u16, s32, s32 *, s32);
void o0b_func_800DD0DC(s32);
void SystemError(s32, s32);
extern char D_800A81A8[], D_800A825C[];

/* export 0x338, func_800E8F50: register a texture-list or Gouraud effect model; $v0 = the handle */
u32 port_effect_export_800E8F50(Unk800E8F50 *arg0, s32 arg1) {
    Unk800E8F50 *h = arg0;
    if (arg0->unk0 != 0x6F73) {
        SystemError(0x53, 0x190);
    }
    void *p = (u8 *)arg0 + h->unk4;
    if (h->unk2 != 0) {
        s32 n = (arg0->unk4 - 8) >> 3;
        Unk800E8F50Entry *e = h->unk8;
        for (s32 i = 0; i < n; i++) {
            e[i].unk0 |= (arg1 & 3) << 5;
        }
        return o0b_Hi_RegisterTexListModel(p, e, (s16 *)((u8 *)h + h->unk6), n);
    }
    return o0b_Hi_RegisterGouEffModel(p);
}

/* export 0x318, func_800DD0DC: $v0 = func_800DD10C's byte (a read) */
u32 port_effect_export_800DD0DC(s32 arg0) {
    o0b_func_800DD0DC(arg0);
    return o0b_func_800DD10C(o0b_btlfx_getEffModel(arg0, D_800A81A8)->unk0);
}

/* export 0x310, Hi_GetMdlVertexPtr: its whole body is the tail call to func_800DF7A0 */
u32 port_effect_export_Hi_GetMdlVertexPtr(s32 arg0, u16 arg1, s32 arg2, s32 *arg3, s32 arg4) {
    return o0b_func_800DF7A0(o0b_btlfx_getEffModel(arg0, D_800A825C)->unk0, arg1, arg2, arg3, arg4);
}

/* ovl_0b's sectors land and are relocated: point the three entries at the wrappers */
#define EXPORTS_AT 0xF7510 /* g_btlfxModuleExports, psx_ram offset */
void port_overlay_relocated(unsigned int id, unsigned int off, unsigned int len) {
    static const struct { unsigned int entry; void *wrapper; } fix[] = {
        {0x310, port_effect_export_Hi_GetMdlVertexPtr}, {0x318, port_effect_export_800DD0DC},
        {0x338, port_effect_export_800E8F50}};
    if (id != 0x0B) return;
    for (unsigned int i = 0; i < sizeof fix / sizeof *fix; ++i) {
        unsigned int at = EXPORTS_AT + fix[i].entry;
        if (at >= off && at + 4 <= off + len) *(u32 *)(psx_ram + at) = (u32)(uintptr_t)fix[i].wrapper;
    }
}
