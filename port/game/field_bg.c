/* The field background's layers in the OT (fldbg_draw, ovl_0a; renamed by config/renames.txt so the port wraps it).
 *
 * The game links each layer's tiles (fldbg_drawOverlay: prebuilt DR_TPAGE + SPRT_16 pairs) into the OT one by one.
 * With the hybrid renderer drawing fields (FIELDGX=1), a layer's run of tiles at one OT entry (a moving layer puts
 * a hundred or more at its depth) is lifted out after the game's call and replaced by one marker (gpu.c's port
 * markers): the walk reaches the run as a whole, at the same place among the entry's other packets, and the
 * renderer takes it as one (gxh_bg_run). The tiles keep their links and positions as the game left them.
 */
#include "psyq_port.h"
#include "ovl_0a.h"
#include "port_hw.h"
#include "native_mesh.h" /* (PORT_OVERLAY_CODE) */
#include <stdint.h>

s32 o0a_fldbg_draw_game(void);
s32 o0a_fldbg_draw(void);

#if defined(PORT_GX_HYBRID) && defined(PORT_FIELD_GX)
extern const unsigned char *psx_ot_marker_lo, *psx_ot_marker_hi;
/* gx_hybrid.c: the run's tiles taken by the renderer (0: drawn as packets, one by one) */
int gxh_bg_run(unsigned int *head, int count);

/* a run of one layer's tiles linked at one OT entry: its first (the walk's first: the newest linked) and how many */
typedef struct { u32 tag; u32 *head; u16 count, entry; } BgMarker;
#define BG_MARKERS 32 /* per draw buffer (more: the rest stay linked as they are) */
#define BG_RUN_MIN 4  /* (shorter runs stay linked: a marker would only add a step) */
/* (in ovl_0a's area: loaded, cleared, with it; nothing of this is used without it) */
#define BG_DATA __attribute__((section(".psxovl_0a.data")))
static BgMarker bg_markers[2][BG_MARKERS] BG_DATA;
static int bg_marker_count[2] BG_DATA;

/* per layer (by index): where it was drawn last, and how many frames since that changed. Only a moving layer's
 * runs are lifted: a still one's tiles stay recorded one by one, for the composite (gx_hybrid.c bgc_use), which
 * holds a still background and would otherwise see it change each time a layer went from one form to the other. */
#define BG_LAYERS 64
#define BG_STILL_FRAMES 8
static u32 bg_layer_at[BG_LAYERS] BG_DATA;
static u8 bg_layer_still[BG_LAYERS] BG_DATA;

static inline u32 *bg_link(u32 word) { return (u32 *)(((u32)(uintptr_t)bg_markers & 0xFF000000) | (word & 0xFFFFFF)); }

/* a run of the layer's tiles at the head of entry d: lifted into a marker */
PORT_OVERLAY_CODE(0a) static void bg_lift_entry(u32 *ot, int d, uintptr_t first, uintptr_t size, int buf) {
    u32 *slot = &ot[d], *p = bg_link(*slot);
    u32 count = 0;
    while ((uintptr_t)p - first < size && ((uintptr_t)p - first) % sizeof(BgSprt) == 0) {
        ++count;
        p = bg_link(*p);
    }
    if (count < BG_RUN_MIN || bg_marker_count[buf] == BG_MARKERS) return;
    BgMarker *m = &bg_markers[buf][bg_marker_count[buf]++];
    m->tag = (u32)(uintptr_t)p & 0xFFFFFF; /* (the entry's next packet after the run) */
    m->head = bg_link(*slot);
    m->count = (u16)count;
    m->entry = (u16)d;
    *slot = (*slot & 0xFF000000) | ((u32)(uintptr_t)m & 0xFFFFFF);
}

/* one layer drawn this frame: its runs at the OT entries its tiles take (the game's z + the tile's), each entry
 * once in a row (consecutive tiles mostly share one; an entry met again finds its run already lifted) */
PORT_OVERLAY_CODE(0a) static void bg_lift(int j, const BgOverlay *o, const BgFile *h, u32 *ot, int buf) {
    if (j < BG_LAYERS) { /* (what places its tiles: the layer's and the screen's position, its scroll, its viewport's) */
        const s16 *vp = g_fieldBgState.viewportPos[o->viewport & 3];
        u32 at = (u32)(u16)(o->x + h->screenX) | (u32)(u16)(o->y + h->screenY) << 16;
        at = at * 31u ^ (u32)(u16)o->scrollX ^ (u32)(u16)o->scrollY << 16;
        at = at * 31u ^ (u32)(u16)o->fracX ^ (u32)(u16)o->fracY << 16;
        at = at * 31u ^ (u32)(u16)vp[0] ^ (u32)(u16)vp[1] << 16;
        at = at * 31u ^ (u32)(u16)g_fieldBgState.scroll.unk0 ^ (u32)(u16)g_fieldBgState.scroll.unk2 << 16;
        if (at != bg_layer_at[j]) { bg_layer_at[j] = at; bg_layer_still[j] = 0; }
        else if (bg_layer_still[j] < BG_STILL_FRAMES) bg_layer_still[j]++;
        if (bg_layer_still[j] >= BG_STILL_FRAMES) return; /* (still: recorded, for the composite) */
    }
    const BgSprt *spr = o->sprites;
    const u32 n = o->tileCount;
    if (buf != 0) spr += n;
    const BgPos *pos = (const BgPos *)((const u8 *)h + o->posOffset);
    const s16 z = (s16)(h->baseDepth + o->depth);
    const uintptr_t first = (uintptr_t)spr, size = (uintptr_t)n * sizeof(BgSprt);
    int last = -1;
    for (u32 i = 0; i < n; ++i) {
        const s16 d = (s16)(z + pos[i].pad);
        if (d < 8 || d >= 0xFF9 || d == last) continue;
        last = d;
        bg_lift_entry(ot, d, first, size, buf);
    }
}

/* gpu.c's walk at a marker: the run, the renderer's or drawn tile by tile in the walk's order */
void port_ot_marker(unsigned int *marker) {
    const BgMarker *m = (const BgMarker *)marker;
    if (psx_ovl_resident[0] == 0x0A && gxh_bg_run(m->head, m->count)) return; /* (its code is ovl_0a's) */
    u32 *p = m->head;
    for (u32 i = 0; i < m->count; ++i) {
        DrawPrim(p);
        p = bg_link(*p);
    }
}
#endif

PORT_OVERLAY_CODE(0a) s32 o0a_fldbg_draw(void) {
    s32 r = o0a_fldbg_draw_game();
#if defined(PORT_GX_HYBRID) && defined(PORT_FIELD_GX)
    psx_ot_marker_lo = (const unsigned char *)bg_markers;
    psx_ot_marker_hi = (const unsigned char *)(bg_markers + 2);
    const int buf = *g_fieldBgCtx.bufIndex;
    bg_marker_count[buf] = 0;
    BgFile *h = g_fieldBgCtx.data;
    const BgOverlay *o = (const BgOverlay *)((const u8 *)h + h->overlayOffset);
    /* (the last drawn first: an entry's run of a layer is at its head once the later layers' are lifted) */
    for (int j = h->overlayCount - 1; j >= 0; --j)
        if ((o[j].flags & 2) && o[j].cameraId == g_fieldBgState.cameraId) bg_lift(j, &o[j], h, g_fieldBgCtx.ot[buf], buf);
#endif
    return r;
}
