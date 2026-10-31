/* PS1 GPU command decoding (gp0.h), following psx-spx "GPU Render Polygon/Line/Rectangle Commands" and
 * "GPU Rendering Attributes". */
#include <stdio.h>
#include <stdlib.h>
#include "blackbox.h"
#include "gp0.h"
#include "profile.h"
#include "psx_display.h"
#include "psx_runtime.h"

unsigned int psx_gpu_scene_prims; /* primitives drawn since the display last moved (gpu.c PutDispEnv) */

#if defined(PORT_GTE_ITCM) && defined(__arm__)
__attribute__((section(".dtcm"))) /* read for every primitive (DS: beside the GTE's registers) */
#endif
Gp0State gp0_state = {0, 0, 1023, 511, 0, 0, 0, 0, 0, 0, 0, 0, 0};
/* DS mode: the decoder in ITCM (gx_hybrid.c GXH_HOT: what the software polygon rasteriser left) */
#if defined(__arm__) && defined(PORT_DS_MODE) && defined(PORT_GX_HYBRID)
#define GP0_HOT __attribute__((section(".itcm"), noinline))
/* (ITCM code that isn't a tight loop: Thumb, half the size, as fast there) */
#define GP0_HOT_THUMB __attribute__((section(".itcm"), noinline, target("thumb")))
#else
#define GP0_HOT
#define GP0_HOT_THUMB
#endif
void gp0_note_sprites(unsigned int texpage, unsigned int n);
const Gp0Backend *gp0_backend;
unsigned int psx_probe_top;
/* psx_display.h; weak here and in gpu.c, since unit tests link either file alone */
__attribute__((weak)) unsigned int psx_vram_generation;
__attribute__((weak)) unsigned int psx_display_generation;
__attribute__((weak)) int psx_display_watch[4];
__attribute__((weak)) unsigned int psx_display_watch_version;
/* whether draws inside the drawing area touch the watched display: recomputed after E3/E4 or a new watch (other
 * code that changes the area for a draw of its own restores it before the next command) */
static int area_hits_valid, area_hits;
static unsigned int area_hits_version;
/* psx_runtime.h; weak here too, for unit tests that link gp0.c without runtime.c */
__attribute__((weak)) void port_canary(const char *where) { (void)where; }
/* Snapshot counters for diagnosing the DS font presentation handoff. */
unsigned int psx_gp0_font_probe[16];

/* DS presentation may preserve font coverage when shrinking the display. */
__attribute__((weak)) void psx_display_glyph(int x, int y, int w, int h) {}
__attribute__((weak)) void psx_display_drawing(void) {}
/* (set by the display once drawing started, so the decoder skips the call per primitive: display.c) */
__attribute__((weak)) int psx_display_drawing_on;

static inline int sext11(int v) {
    return (v & 0x7FF) - ((v & 0x400) << 1);
}

/* a vertex, filled in place (returned by value it was a 28-byte copy per vertex) */
static inline void vtx(Gp0Vtx *v, const unsigned int *xyp, unsigned int col) {
    const unsigned int xy = *xyp;
    v->xyw = xyp;
    v->u = v->v = 0;
    v->x = sext11((int)(xy & 0x7FF)) + gp0_state.ofs_x;
    v->y = sext11((int)((xy >> 16) & 0x7FF)) + gp0_state.ofs_y;
    v->r = col & 0xFF;
    v->g = (col >> 8) & 0xFF;
    v->b = (col >> 16) & 0xFF;
}

static void apply_texpage(Gp0Mode *m, unsigned int tp) {
    m->tx = (tp & 15) * 64;
    m->ty = ((tp >> 4) & 1) * 256;
    m->abr = (tp >> 5) & 3;
    m->depth = (tp >> 7) & 3;
    if (m->depth == 3) {
        m->depth = 2;
    }
}

static void set_clut(Gp0Mode *m, unsigned int uv) {
    m->clut_x = ((uv >> 16) & 0x3F) * 16;
    m->clut_y = (uv >> 22) & 0x1FF;
}

/* One GP0 command at w[0]; returns the words it used. */
/* The drawing-state commands (E1-E6; others from E0 up do nothing): about half of all commands, so they're
 * decided before a draw's mode is set up. */
static unsigned int state_command(unsigned int w0) {
    switch (w0 >> 24) {
    case 0xE1:
        gp0_state.texpage = w0 & 0xFFFFFF;
        break;
    case 0xE2:
        gp0_state.twin_mask_x = (w0 & 31) * 8;
        gp0_state.twin_mask_y = ((w0 >> 5) & 31) * 8;
        gp0_state.twin_off_x = ((w0 >> 10) & 31) * 8;
        gp0_state.twin_off_y = ((w0 >> 15) & 31) * 8;
        break;
    case 0xE3:
        gp0_state.area_x0 = w0 & 0x3FF;
        gp0_state.area_y0 = (w0 >> 10) & 0x1FF;
        area_hits_valid = 0;
        break;
    case 0xE4:
        gp0_state.area_x1 = w0 & 0x3FF;
        gp0_state.area_y1 = (w0 >> 10) & 0x1FF;
        area_hits_valid = 0;
        break;
    case 0xE5:
        gp0_state.ofs_x = sext11((int)(w0 & 0x7FF));
        gp0_state.ofs_y = sext11((int)((w0 >> 11) & 0x7FF));
        break;
    case 0xE6:
        gp0_state.set_mask = w0 & 1;
        gp0_state.check_mask = (w0 >> 1) & 1;
#ifdef PORT_PROBE_AT
        { extern long ctr_frames(void); static int n; if ((w0 & 3) && ctr_frames() >= PORT_PROBE_AT - 12 && ctr_frames() <= PORT_PROBE_AT && n++ < 20)
            port_log("probe: E6 set %d check %d\n", gp0_state.set_mask, gp0_state.check_mask); }
#endif
        break;
    default:
        break;
    }
    return 1;
}

GP0_HOT_THUMB static unsigned int command(const unsigned int *w, unsigned int avail) {
    unsigned int cmd = w[0] >> 24;
    if (cmd >= 0xE0) return state_command(w[0]);
    if ((cmd >= 0x20 && cmd < 0x80) || cmd == 2) {
        if (!psx_display_drawing_on) psx_display_drawing();
        psx_vram_generation++;
        if (cmd != 2) { /* draws stay inside the drawing area; fills count their own rectangle (vram_fill) */
            if (!area_hits_valid || area_hits_version != psx_display_watch_version) {
                area_hits = psx_display_hits(gp0_state.area_x0, gp0_state.area_y0,
                                             gp0_state.area_x1 - gp0_state.area_x0 + 1,
                                             gp0_state.area_y1 - gp0_state.area_y0 + 1);
                area_hits_version = psx_display_watch_version;
                area_hits_valid = 1;
            }
            if (area_hits) psx_display_generation++;
        }
    }
    Gp0Mode m; /* every field set (an initialiser compiles to a memset call, one per command) */
    m.textured = m.raw = m.semi = m.gouraud = 0;
    m.clut_x = m.clut_y = 0;
    apply_texpage(&m, gp0_state.texpage);

    if (cmd >= 0x20 && cmd < 0x40) { /* polygon */
        m.gouraud = (cmd >> 4) & 1;
        int quad = (cmd >> 3) & 1;
        m.textured = (cmd >> 2) & 1;
        m.semi = (cmd >> 1) & 1;
        m.raw = cmd & 1;
        int nv = quad ? 4 : 3;
        unsigned int need = 1 + nv * (1 + m.textured) + (m.gouraud ? nv - 1 : 0);
        if (need > avail) {
            return avail;
        }
        Gp0Vtx v[4];
        unsigned int k = 1;
        for (int i = 0; i < nv; i++) {
            unsigned int col = (i == 0 || !m.gouraud) ? w[0] : w[k++];
            vtx(&v[i], &w[k], col);
            k++;
            if (m.textured) {
                unsigned int uv = w[k++];
                v[i].u = uv & 0xFF;
                v[i].v = (uv >> 8) & 0xFF;
                if (i == 0) {
                    set_clut(&m, uv);
                } else if (i == 1) {
                    unsigned int tp = uv >> 16;
                    gp0_state.texpage = (gp0_state.texpage & ~0x9FFu) | (tp & 0x9FF);
                    apply_texpage(&m, tp);
                }
            }
        }
#if defined(PORT_GP0_PROBE) || defined(PORT_PROBE_AT)
        { /* polygons with a vertex in the top 6 rows of the drawing area, per frame (debug) */
            extern unsigned int psx_probe_top;
            for (int i = 0; i < nv; i++) if (v[i].y - gp0_state.area_y0 < 6 && v[i].y >= gp0_state.area_y0) { psx_probe_top++; break; }
        }
#endif
#if defined(PORT_GP0_PROBE) || defined(PORT_PROBE_AT)
        { /* (debug: FF9_PROBE=x,y on the host, PROBE_AT=frame on the 3DS: every polygon whose bounds hold that point of
           * the drawing area) */
            static int px = -1, py;
#ifdef PORT_PROBE_AT
            extern long ctr_frames(void);
            static int nlog;
            px = ctr_frames() >= PORT_PROBE_AT - 12 && ctr_frames() <= PORT_PROBE_AT && nlog++ < 4000 ? 60 : -2;
            py = 60;
#else
            if (px == -1) { const char *e = getenv("FF9_PROBE"); px = -2; if (e) sscanf(e, "%d,%d", &px, &py); }
#endif
            int qx = gp0_state.area_x0 + px, qy = gp0_state.area_y0 + py, inx = 0, iny = 0;
            for (int i = 0; i < nv; i++) { inx |= (v[i].x <= qx) | (v[i].x >= qx) << 1; iny |= (v[i].y <= qy) | (v[i].y >= qy) << 1; }
            if (px >= 0 && inx == 3 && iny == 3)
                port_log("probe: cmd %02x abr %d page %d,%d d%d clut %d,%d %d,%d %d,%d %d,%d %d,%d rgb %02x%02x%02x %02x%02x%02x %02x%02x%02x\n", cmd, m.abr, m.tx, m.ty, m.depth, m.clut_x, m.clut_y,
                         v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y, nv > 3 ? v[3].x : 0, nv > 3 ? v[3].y : 0,
                         v[0].r, v[0].g, v[0].b, v[1].r, v[1].g, v[1].b, v[2].r, v[2].g, v[2].b);
        }
#endif
        if (gp0_backend && gp0_backend->poly) {
            ++psx_gpu_scene_prims;
            gp0_backend->poly(&m, v, nv);
        }
        return need;
    }
    if (cmd >= 0x40 && cmd < 0x60) { /* line, polyline (terminated by 0x5xxx5xxx) */
        m.gouraud = (cmd >> 4) & 1;
        m.semi = (cmd >> 1) & 1;
        int poly = (cmd >> 3) & 1;
        unsigned int k = 1;
        Gp0Vtx prev;
        vtx(&prev, &w[k], w[0]);
        k++;
        for (;;) {
            if (k >= avail) {
                return avail;
            }
            if (poly && (w[k] & 0xF000F000) == 0x50005000) {
                return k + 1;
            }
            unsigned int col = w[0];
            if (m.gouraud) {
                col = w[k++];
                if (k >= avail) {
                    return avail;
                }
            }
            Gp0Vtx next;
            vtx(&next, &w[k], col);
            k++;
            if (gp0_backend && gp0_backend->line) {
                ++psx_gpu_scene_prims;
                gp0_backend->line(&m, &prev, &next);
            }
            prev = next;
            if (!poly) {
                return k;
            }
        }
    }
    if (cmd >= 0x60 && cmd < 0x80) { /* rectangle */
        m.textured = (cmd >> 2) & 1;
        m.semi = (cmd >> 1) & 1;
        m.raw = cmd & 1;
        int size = (cmd >> 3) & 3;
        unsigned int need = 2 + m.textured + (size == 0);
        if (need > avail) {
            return avail;
        }
        Gp0Vtx v;
        vtx(&v, &w[1], w[0]);
        unsigned int k = 2;
        if (m.textured) {
            unsigned int uv = w[k++];
            v.u = uv & 0xFF;
            v.v = (uv >> 8) & 0xFF;
            set_clut(&m, uv);
        }
        int wd = 1, ht = 1;
        if (size == 0) {
            wd = w[k] & 0x3FF;
            ht = (w[k] >> 16) & 0x1FF;
        } else if (size == 2) {
            wd = ht = 8;
        } else if (size == 3) {
            wd = ht = 16;
        }
#if defined(PORT_GP0_PROBE) || defined(PORT_PROBE_AT)
        { /* (debug: FF9_PROBE as for polygons, rectangles too) */
            static int px = -1, py;
#ifdef PORT_PROBE_AT
            extern long ctr_frames(void);
            px = ctr_frames() >= PORT_PROBE_AT - 12 && ctr_frames() <= PORT_PROBE_AT ? 60 : -2;
            py = 60;
#else
            if (px == -1) { const char *e = getenv("FF9_PROBE"); px = -2; if (e) sscanf(e, "%d,%d", &px, &py); }
#endif
            int qx = gp0_state.area_x0 + px, qy = gp0_state.area_y0 + py;
            if (px >= 0 && v.x <= qx && v.x + wd > qx && v.y <= qy && v.y + ht > qy)
                port_log("probe: rect %02x tex%d semi%d abr%d d%d page %d,%d clut %d,%d %d,%d %dx%d uv %d,%d rgb %02x%02x%02x\n",
                         cmd, m.textured, m.semi, m.abr, m.depth, m.tx, m.ty, m.clut_x, m.clut_y, v.x, v.y, wd, ht, v.u, v.v,
                         v.r, v.g, v.b);
        }
#endif
        if (gp0_backend && gp0_backend->rect) {
            if (ht == 13 && wd <= 16 && m.textured) {
                psx_gp0_font_probe[0]++;
                psx_gp0_font_probe[1]=m.tx;
                psx_gp0_font_probe[2]=m.ty;
                psx_gp0_font_probe[3]=m.depth;
                psx_gp0_font_probe[4]=v.x;
                psx_gp0_font_probe[5]=v.y;
                psx_gp0_font_probe[6]=gp0_state.area_x0;
                psx_gp0_font_probe[7]=gp0_state.area_y0;
                psx_gp0_font_probe[8]=gp0_state.area_x1;
                psx_gp0_font_probe[9]=gp0_state.area_y1;
            }
            ++psx_gpu_scene_prims;
            gp0_backend->rect(&m, &v, wd, ht);
            if (!m.textured || m.ty != 256 || (m.tx != 896 && m.tx != 960)) return need; /* no font page */
            /* Battle names (F2DC0) and HP/MP strings (F2ED0/F3A8C)
             * are cached sprites, so their original glyphs are not emitted
             * every frame. Preserve coverage for these documented atlas
             * regions too, rather than treating the HUD as scene pixels. */
            int glyph = m.depth == 0 &&
                (((m.tx == 896 || m.tx == 960) && ht == 13 && wd <= 16) ||
                 (m.tx == 960 && ht == 8 && wd == 8));
            int battle_name = m.depth == 0 && m.tx == 896 &&
                ht == 12 && wd == 84 && v.v == 240 && (v.u == 0 || v.u == 84);
            int battle_digits = m.depth == 0 && m.tx == 960 &&
                ht == 10 && wd == 8 && v.v == 64 && v.u >= 152 && v.u <= 232 && !(v.u & 7);
            int battle_values = m.depth == 2 && m.tx == 960 && ht == 10 &&
                ((wd == 29 && v.u == 0) || (wd == 22 && v.u == 32)) &&
                v.v >= 176 && v.v <= 212 && !((v.v - 176) % 12);
            if (m.textured && m.ty == 256 && (glyph || battle_name || battle_digits || battle_values)) {
                psx_gp0_font_probe[10]++;
                int x0=v.x>gp0_state.area_x0 ? v.x : gp0_state.area_x0;
                int y0=v.y>gp0_state.area_y0 ? v.y : gp0_state.area_y0;
                int x1=v.x+wd<gp0_state.area_x1+1 ? v.x+wd : gp0_state.area_x1+1;
                int y1=v.y+ht<gp0_state.area_y1+1 ? v.y+ht : gp0_state.area_y1+1;
                if(x1>x0 && y1>y0) {
                    psx_gp0_font_probe[11]++;
                    psx_display_glyph(x0,y0,x1-x0,y1-y0);
                }
            }
        }
        return need;
    }
    switch (cmd) {
    case 0x02:
        if (avail >= 3 && gp0_backend && gp0_backend->fill) {
            gp0_backend->fill(w[1] & 0x3F0, (w[1] >> 16) & 0x1FF, ((w[2] & 0x3FF) + 15) & ~15, (w[2] >> 16) & 0x1FF,
                              w[0] & 0xFFFFFF);
        }
        return 3;
    case 0x80:
        return 4;
    default:
        break;
    }
    return 1;
}

/* A packet of exactly a texture page (E1) and a 16x16 sprite (SPRT_16, 7C-7F): a field background tile (BgSprt,
 * MargePrim'd). Both commands as command() runs them, without its dispatch; the font pages' sprites (their glyph
 * coverage, below) are left to it. 0: not run. */
GP0_HOT_THUMB int gp0_tpage_sprite16(const unsigned int *w) {
    unsigned int tp = w[0] & 0xFFFFFF, cmd = w[1] >> 24;
    if ((tp & 0x10) && (tp & 15) >= 14) return 0; /* (page 896 or 960, 256) */
    if (!gp0_backend || !gp0_backend->rect) return 0;
    gp0_state.texpage = tp;
    if (!psx_display_drawing_on) psx_display_drawing();
    psx_vram_generation++;
    if (!area_hits_valid || area_hits_version != psx_display_watch_version) {
        area_hits = psx_display_hits(gp0_state.area_x0, gp0_state.area_y0, gp0_state.area_x1 - gp0_state.area_x0 + 1,
                                     gp0_state.area_y1 - gp0_state.area_y0 + 1);
        area_hits_version = psx_display_watch_version;
        area_hits_valid = 1;
    }
    if (area_hits) psx_display_generation++;
    ++psx_gpu_scene_prims;
    if (gp0_backend->sprite16 && gp0_backend->sprite16(tp, w[1], w[2], w[3])) return 1;
    Gp0Mode m;
    m.textured = 1;
    m.raw = cmd & 1;
    m.semi = (cmd >> 1) & 1;
    m.gouraud = 0;
    apply_texpage(&m, tp);
    set_clut(&m, w[3]);
    Gp0Vtx v;
    vtx(&v, &w[2], w[1]);
    v.u = w[3] & 0xFF;
    v.v = (w[3] >> 8) & 0xFF;
    gp0_backend->rect(&m, &v, 16, 16);
    return 1;
}

/* gp0_tpage_sprite16's bookkeeping for n such sprites drawn at once (a field background's run, gx_hybrid.c
 * gxh_bg_run), the texture page left as the last one's */
#ifdef PORT_FIELD_GX
__attribute__((section(".psxovl_0a.port"), noinline)) /* (its only caller is ovl_0a's) */
#endif
void gp0_note_sprites(unsigned int texpage, unsigned int n) {
    gp0_state.texpage = texpage & 0xFFFFFF;
    if (!psx_display_drawing_on) psx_display_drawing();
    psx_vram_generation += n;
    if (!area_hits_valid || area_hits_version != psx_display_watch_version) {
        area_hits = psx_display_hits(gp0_state.area_x0, gp0_state.area_y0, gp0_state.area_x1 - gp0_state.area_x0 + 1,
                                     gp0_state.area_y1 - gp0_state.area_y0 + 1);
        area_hits_version = psx_display_watch_version;
        area_hits_valid = 1;
    }
    if (area_hits) psx_display_generation += n;
    psx_gpu_scene_prims += n;
}

/* The commands from w on that are drawing primitives (polygons, lines, rectangles) or drawing state (E0 up), up to
 * the first other one: their words used. One loop for a packet's run of them (gpu.c draw_packet). */
unsigned int gp0_run_plain(const unsigned int *w, unsigned int len) {
    unsigned int i = 0;
    while (i < len) {
        unsigned int cmd = w[i] >> 24;
        if (!((cmd >= 0x20 && cmd < 0x80) || cmd >= 0xE0)) break;
        i += command(w + i, len - i);
    }
    return i;
}

void gp0_run(const unsigned int *w, unsigned int len) {
    BB(0x47500001, w);
    unsigned int profile_start=port_profile_begin();
#ifdef PORT_CANARY
    port_canary("before GP0 commands");
#endif
    for (unsigned int i = 0; i < len;) {
        i += command(w + i, len - i);
    }
#ifdef PORT_CANARY
    port_canary("GP0 commands");
#endif
    port_profile_end(PORT_PROFILE_GPU,profile_start);
}
