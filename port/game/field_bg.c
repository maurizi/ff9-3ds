/* The field background's layers in the OT (fldbg_draw, ovl_0a; renamed by config/renames.txt so the port wraps it).
 *
 * The game links each layer's tiles (fldbg_drawOverlay: prebuilt DR_TPAGE + SPRT_16 pairs) into the OT one by one.
 * With the hybrid renderer drawing fields (FIELDGX=1), each layer's run of tiles at an OT entry is lifted out after
 * the game's call and replaced by one marker (gpu.c's port markers), so the walk reaches the run as a whole, at the
 * same place among the entry's other packets. The tiles keep their links and positions as the game left them.
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

/* a run of one layer's tiles linked at one OT entry: its first (the walk's first: the newest linked) and how many */
typedef struct { u32 tag; u32 *head; u16 count, entry; } BgMarker;
#define BG_MARKERS 64 /* per draw buffer (more: the rest stay linked as they are) */
static BgMarker bg_markers[2][BG_MARKERS];
static int bg_marker_count[2];
static u32 bg_entries[0x1000 / 32];
/* per layer (by its index): the OT entries its tiles take, kept while its depth and tile table are the same */
#define BG_LAYERS 48
#define BG_LAYER_ENTRIES 6
static struct { const BgOverlay *o; const BgPos *pos; u16 n; s16 z; u8 count; u16 entry[BG_LAYER_ENTRIES]; } bg_layer[BG_LAYERS];

static inline u32 *bg_link(u32 word) { return (u32 *)(((u32)(uintptr_t)bg_markers & 0xFF000000) | (word & 0xFFFFFF)); }

/* one layer drawn this frame: its runs at each OT entry its tiles can take (the game's z + the tile's) */
/* a run of the layer's tiles at the head of entry d: lifted into a marker */
static inline void bg_lift_entry(u32 *ot, int d, uintptr_t first, uintptr_t size, int buf) {
    u32 *slot = &ot[d], *p = bg_link(*slot);
    u32 count = 0;
    while ((uintptr_t)p - first < size && ((uintptr_t)p - first) % sizeof(BgSprt) == 0) {
        ++count;
        p = bg_link(*p);
    }
    if (!count || bg_marker_count[buf] == BG_MARKERS) return;
    BgMarker *m = &bg_markers[buf][bg_marker_count[buf]++];
    m->tag = (u32)(uintptr_t)p & 0xFFFFFF; /* (the entry's next packet after the run) */
    m->head = bg_link(*slot);
    m->count = (u16)count;
    m->entry = (u16)d;
    *slot = (*slot & 0xFF000000) | ((u32)(uintptr_t)m & 0xFFFFFF);
}

PORT_OVERLAY_CODE(0a) static void bg_lift(int j, const BgOverlay *o, const BgFile *h, u32 *ot, int buf) {
    const BgSprt *spr = o->sprites;
    const u32 n = o->tileCount;
    if (buf != 0) spr += n;
    const BgPos *pos = (const BgPos *)((const u8 *)h + o->posOffset);
    const s16 z = (s16)(h->baseDepth + o->depth);
    const uintptr_t first = (uintptr_t)spr, size = (uintptr_t)n * sizeof(BgSprt);
    if (j < BG_LAYERS && bg_layer[j].o == o && bg_layer[j].pos == pos && bg_layer[j].n == n && bg_layer[j].z == z) {
        for (int k = 0; k < bg_layer[j].count; ++k) bg_lift_entry(ot, bg_layer[j].entry[k], first, size, buf);
        return;
    }
    int lo = 0x1000, hi = -1;
    for (u32 i = 0; i < n; ++i) {
        const s16 d = (s16)(z + pos[i].pad);
        if (d < 8 || d >= 0xFF9) continue;
        bg_entries[d >> 5] |= 1u << (d & 31);
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    int count = 0;
    for (int w = lo >> 5; w <= hi >> 5; ++w) {
        u32 bits = bg_entries[w];
        bg_entries[w] = 0;
        for (; bits; bits &= bits - 1) {
            const int d = w * 32 + __builtin_ctz(bits);
            if (j < BG_LAYERS && count < BG_LAYER_ENTRIES) bg_layer[j].entry[count] = (u16)d;
            ++count;
            bg_lift_entry(ot, d, first, size, buf);
        }
    }
    if (j < BG_LAYERS) { /* (a layer over more entries: worked out each frame) */
        bg_layer[j].o = count <= BG_LAYER_ENTRIES ? o : NULL;
        bg_layer[j].pos = pos; bg_layer[j].n = (u16)n; bg_layer[j].z = z; bg_layer[j].count = (u8)count;
    }
}

/* gpu.c's walk at a marker: the run's tiles, in the walk's order */
void port_ot_marker(unsigned int *marker) {
    const BgMarker *m = (const BgMarker *)marker;
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
