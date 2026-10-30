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
/* gx_hybrid.c: the run's tiles taken by the renderer (0: drawn as packets, one by one; 2: so, and holding still) */
int gxh_bg_run(unsigned int *head, int count);
void gxh_bg_runs_reset(void);
int gxh_bg_runs_on(void);

/* a run of one layer's tiles linked at one OT entry: its first (the walk's first: the newest linked) and how many */
typedef struct { u32 tag; u32 *head; u16 count, entry; } BgMarker;
#define BG_MARKERS 32 /* per draw buffer (more: the rest stay linked as they are) */
#define BG_RUN_MIN 4  /* (shorter runs stay linked: a marker would only add a step) */
/* (in ovl_0a's area: loaded, cleared, with it; nothing of this is used without it) */
#define BG_DATA __attribute__((section(".psxovl_0a.data")))
static int bg_marker_count[2] BG_DATA;
/* per draw buffer, runs (by first tile) the renderer found still: left linked, the walk's as they are, until the
 * list clears (every BG_SKIP_FRAMES draws: a layer that starts moving is lifted again) */
#define BG_SKIPS 16
#define BG_SKIP_FRAMES 32
static u32 *bg_skip[2][BG_SKIPS] BG_DATA;
static int bg_skip_count[2] BG_DATA, bg_skip_age BG_DATA;

/* Units (while the runs are off: still backgrounds): each visible layer's tiles lifted out of the OT and chained
 * through their own links, its marker at the farthest entry of them all (the walk's first); gx_hybrid.c's gxh_bg_unit
 * records them all there, or they go back where they were */
#define BG_UNITS 16
typedef struct {
    u32 tag;          /* the marker's link */
    u32 *ot;
    u32 *head;        /* the chain's first tile */
    const u8 *sprites;
    const u32 *pos;   /* the layer's BgPos words (an entry: z + the low 12 bits) */
    s16 z;
    u16 count, far;
} BgUnit;
static struct {
    BgMarker markers[2][BG_MARKERS];
    BgUnit units[2][BG_UNITS];
} bg_mk BG_DATA; /* (one range for gpu.c's walk to tell markers by) */
#define bg_markers bg_mk.markers
#define bg_units bg_mk.units
static int bg_unit_count[2] BG_DATA;

static inline u32 *bg_link(u32 word) { return (u32 *)(((u32)(uintptr_t)&bg_mk & 0xFF000000) | (word & 0xFFFFFF)); }

/* a run of the layer's tiles at the head of entry d: lifted into a marker */
PORT_OVERLAY_CODE(0a) static void bg_lift_entry(u32 *ot, int d, uintptr_t first, uintptr_t size, int buf) {
    u32 *slot = &ot[d], *const head = bg_link(*slot), *p = head;
    for (int i = 0; i < bg_skip_count[buf]; ++i)
        if (bg_skip[buf][i] == head) return;
    u32 count = 0;
    while ((uintptr_t)p - first < size && ((uintptr_t)p - first) % sizeof(BgSprt) == 0) {
        ++count;
        p = bg_link(*p);
    }
    if (count < BG_RUN_MIN || bg_marker_count[buf] == BG_MARKERS) return;
    BgMarker *m = &bg_markers[buf][bg_marker_count[buf]++];
    m->tag = (u32)(uintptr_t)p & 0xFFFFFF; /* (the entry's next packet after the run) */
    m->head = head;
    m->count = (u16)count;
    m->entry = (u16)d;
    *slot = (*slot & 0xFF000000) | ((u32)(uintptr_t)m & 0xFFFFFF);
}

/* one layer drawn this frame: its runs at the OT entries its tiles take (the game's z + the tile's), each entry
 * once in a row (consecutive tiles mostly share one; an entry met again finds its run already lifted) */
PORT_OVERLAY_CODE(0a) static void bg_lift(const BgOverlay *o, const BgFile *h, u32 *ot, int buf) {
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

/* one layer drawn this frame as a unit: every run of its tiles at the head of the entries they take out of the OT,
 * the runs chained (each run's last tile linked to the next run's first) */
PORT_OVERLAY_CODE(0a) static void bg_unit_lift(const BgOverlay *o, const BgFile *h, u32 *ot, int buf) {
    const BgSprt *spr = o->sprites;
    const u32 n = o->tileCount;
    if (buf != 0) spr += n;
    if (!n || bg_unit_count[buf] == BG_UNITS) return;
    if (((const u32 *)spr)[3] >> 24 & 2) return; /* (semi-transparent: not the unit's path) */
    const u32 *pos = (const u32 *)((const u8 *)h + o->posOffset);
    const s16 z = (s16)(h->baseDepth + o->depth);
    const uintptr_t first = (uintptr_t)spr, size = (uintptr_t)n * sizeof(BgSprt);
    u32 *head = NULL, *tail = NULL, count = 0;
    int last = -1, far = -1;
    for (u32 i = 0; i < n; ++i) {
        const s16 d = (s16)(z + (int)(pos[i] & 0xFFF));
        if (d < 8 || d >= 0xFF9 || d == last) continue;
        last = d;
        u32 *slot = &ot[d], *const run = bg_link(*slot), *p = run, *run_tail = NULL;
        u32 k = 0;
        while ((uintptr_t)p - first < size && ((uintptr_t)p - first) % sizeof(BgSprt) == 0) {
            ++k;
            run_tail = p;
            p = bg_link(*p);
        }
        if (!k) continue;
        *slot = (*slot & 0xFF000000) | ((u32)(uintptr_t)p & 0xFFFFFF); /* (the entry goes on after the run) */
        if (tail) *tail = (*tail & 0xFF000000) | ((u32)(uintptr_t)run & 0xFFFFFF); else head = run;
        tail = run_tail;
        count += k;
        if (d > far) far = d;
    }
    if (!count) return;
    BgUnit *u = &bg_units[buf][bg_unit_count[buf]++];
    u->ot = ot; u->head = head; u->sprites = (const u8 *)spr; u->pos = pos; u->z = z;
    u->count = (u16)count; u->far = (u16)far;
}
/* the frame's units' markers, all at the farthest entry any of them takes, in the order they were lifted (the last
 * drawn layer first, as an entry's tiles are walked: so the records of tiles of two layers at one entry come in the
 * walk's order, which units_order keeps for equal depths) */
PORT_OVERLAY_CODE(0a) static void bg_units_link(u32 *ot, int buf) {
    const int nu = bg_unit_count[buf];
    if (!nu) return;
    int far = 0;
    for (int i = 0; i < nu; ++i) if (bg_units[buf][i].far > far) far = bg_units[buf][i].far;
    u32 *slot = &ot[far];
    for (int i = nu - 1; i >= 0; --i) { /* (each at the head: the first lifted ends first) */
        BgUnit *u = &bg_units[buf][i];
        u->far = (u16)far;
        u->tag = *slot & 0xFFFFFF;
        *slot = (*slot & 0xFF000000) | ((u32)(uintptr_t)u & 0xFFFFFF);
    }
}

/* a unit the renderer didn't take: its runs (its chain's tiles of one entry each) back at the heads of their
 * entries (the walk is at the farthest, the units' markers: that entry's go on from this marker, the nearer ones'
 * are met later) */
static void bg_unit_relink(BgUnit *u) {
    u32 *p = u->head;
    for (u32 left = u->count; left;) {
        const int d = (s16)(u->z + (int)(u->pos[((const u8 *)p - u->sprites) / sizeof(BgSprt)] & 0xFFF));
        u32 *run = p, *tail = p;
        --left;
        p = bg_link(*p);
        while (left && (s16)(u->z + (int)(u->pos[((const u8 *)p - u->sprites) / sizeof(BgSprt)] & 0xFFF)) == d) {
            tail = p;
            --left;
            p = bg_link(*p);
        }
        u32 *at = d == u->far ? &u->tag : &u->ot[d];
        *tail = (*tail & 0xFF000000) | (*at & 0xFFFFFF);
        *at = (*at & 0xFF000000) | ((u32)(uintptr_t)run & 0xFFFFFF);
    }
}

/* gpu.c's walk at a marker: the run, the renderer's or drawn tile by tile in the walk's order */
void port_ot_marker(unsigned int *marker) {
    if ((uintptr_t)marker - (uintptr_t)bg_units < sizeof bg_units) { /* a layer's unit */
        BgUnit *u = (BgUnit *)marker;
        if (psx_ovl_resident[0] == 0x0A && gxh_bg_unit(u->head, u->count, u->sprites, u->pos, u->z, u->ot)) return;
        bg_unit_relink(u);
        return;
    }
    const BgMarker *m = (const BgMarker *)marker;
    if (psx_ovl_resident[0] == 0x0A) { /* (its code is ovl_0a's) */
        const int r = gxh_bg_run(m->head, m->count);
        if (r == 1) return;
        const int buf = m >= bg_markers[1];
        if (r == 2 && bg_skip_count[buf] < BG_SKIPS) bg_skip[buf][bg_skip_count[buf]++] = m->head;
    }
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
    psx_ot_marker_lo = (const unsigned char *)&bg_mk;
    psx_ot_marker_hi = (const unsigned char *)(&bg_mk + 1);
    const int buf = *g_fieldBgCtx.bufIndex;
    bg_marker_count[buf] = 0;
    bg_unit_count[buf] = 0;
    BgFile *h = g_fieldBgCtx.data;
    static u32 last_h BG_DATA; /* (the background file: its place and header, which another field's changes) */
    const u32 hs = (u32)(uintptr_t)h ^ (u32)h->overlayCount << 16 ^ (u32)h->overlayOffset ^ (u32)h->cameraOffset << 8 ^
                   (u32)h->orgX << 20 ^ (u32)h->orgY << 4;
    if (hs != last_h) { last_h = hs; gxh_bg_runs_reset(); bg_skip_count[0] = bg_skip_count[1] = 0; }
    if (++bg_skip_age >= BG_SKIP_FRAMES) { bg_skip_age = 0; bg_skip_count[0] = bg_skip_count[1] = 0; }
    const BgOverlay *o = (const BgOverlay *)((const u8 *)h + h->overlayOffset);
    if (!gxh_bg_runs_on()) { /* (runs off: the layers as units, when the renderer takes them) */
        if (gxh_bg_units_on()) {
            for (int j = h->overlayCount - 1; j >= 0; --j)
                if ((o[j].flags & 2) && o[j].cameraId == g_fieldBgState.cameraId) bg_unit_lift(&o[j], h, g_fieldBgCtx.ot[buf], buf);
            bg_units_link(g_fieldBgCtx.ot[buf], buf);
        }
        return r;
    }
    /* (the last drawn first: an entry's run of a layer is at its head once the later layers' are lifted) */
    for (int j = h->overlayCount - 1; j >= 0; --j)
        if ((o[j].flags & 2) && o[j].cameraId == g_fieldBgState.cameraId) bg_lift(&o[j], h, g_fieldBgCtx.ot[buf], buf);
#endif
    return r;
}
