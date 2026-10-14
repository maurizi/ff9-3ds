/* Included by render_gx.c. Exact, demand-driven PSX framebuffer replay.
 * Commands retain their original interpolation and drawing state. Each 8x8
 * tile records how far it has replayed, so repeated/blended reads are stable.
 * Source writes finish dependent commands before changing texture/CLUT RAM. */
#if !defined(PORT_GX_SHADOW) && !defined(PORT_GX_EXPERIMENTAL)
#error GXLAZY requires GXSHADOW or GXEXPERIMENTAL
#endif
#define SHADOW_COMMANDS 4096
#define SHADOW_COLS (SHADOW_W / 8)
#define SHADOW_ROWS (SHADOW_H / 8)
typedef struct {
    Gp0Mode mode;
    Gp0Vtx vertices[4];
    Gp0State state;
    int kind, n, w, h, bounds[4], source[4];
} ShadowCommand;
typedef struct {
    int valid, ready, target[4], count, reload;
    unsigned int stamp;
    u16 pixels[SHADOW_W * SHADOW_H];
    u8 known[SHADOW_W * SHADOW_H];
    u16 progress[SHADOW_ROWS][SHADOW_COLS];
    u16 source_blocks[64];
    ShadowCommand commands[SHADOW_COMMANDS];
} ShadowContext;
static ShadowContext shadow_contexts[2];
static int shadow_current;
static unsigned int shadow_stamp;
static ShadowContext *shadow_sync_owner;
#define sprite_shadow (shadow_contexts[shadow_current].pixels)
#define sprite_known (shadow_contexts[shadow_current].known)
#define shadow_ready (shadow_contexts[shadow_current].ready)
int psx_gx_stats_shadow_replay, psx_gx_stats_shadow_flush, psx_gx_stats_shadow_retired;
void psx_vram_read(int x, int y, int w, int h);
static void shadow_sync_context(ShadowContext *, int, int, int, int);

static int shadow_overlap(int x, int y, int w, int h, const int *target) {
    return w > 0 && h > 0 && x < target[0] + target[2] && x + w > target[0]
        && y < target[1] + target[3] && y + h > target[1];
}
static int shadow_wrap_overlap(int a, int na, int b, int nb, int limit) {
    return na > 0 && nb > 0 && (na >= limit || nb >= limit ||
        ((b - a) & (limit - 1)) < na || ((a - b) & (limit - 1)) < nb);
}

static int shadow_source_blocks(ShadowContext *c, int x, int y, int w, int h, int mark) {
    int nx = ((x & 63) + w + 63) / 64, ny = ((y & 7) + h + 7) / 8;
    if (nx > 16) nx = 16;
    if (ny > 64) ny = 64;
    unsigned int mask = 0;
    for (int bx = 0; bx < nx; bx++) mask |= 1u << (((x & 1023) / 64 + bx) & 15);
    for (int by = 0; by < ny; by++) {
        u16 *row = &c->source_blocks[(((y & 511) / 8) + by) & 63];
        if (mark) *row |= mask;
        else if (*row & mask) return 1;
    }
    return 0;
}

static void shadow_refresh(ShadowContext *c) {
    if (!c->reload) return;
    /* A partial CPU write follows the before-write notification. Import its
     * result on the next use, retaining unknown coverage from prior draws. */
    for (int py = 0; py < c->target[3]; py++)
        memcpy(c->pixels + py * SHADOW_W,
               psx_vram + (c->target[1] + py) * 1024 + c->target[0], c->target[2] * sizeof(u16));
    c->reload = 0;
}

static void shadow_materialize(ShadowContext *c, int x, int y, int w, int h) {
    shadow_refresh(c);
    int left = x - c->target[0], top = y - c->target[1];
    int right = left + w, bottom = top + h;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > c->target[2]) right = c->target[2];
    if (bottom > c->target[3]) bottom = c->target[3];
    if (left >= right || top >= bottom || !c->count) return;
    int first_x = left / 8, end_x = (right + 7) / 8;
    int first_y = top / 8, end_y = (bottom + 7) / 8;
    Gp0State saved = gp0_state;
    /* Merge equal-progress tile rows/runs. A full untouched framebuffer needs
     * one replay per command, rather than one replay per tile. */
    for (int ty = first_y; ty < end_y;) {
        int rows = 1;
        while (ty + rows < end_y && !memcmp(c->progress[ty] + first_x,
               c->progress[ty + rows] + first_x, (end_x - first_x) * sizeof(u16))) rows++;
        for (int tx = first_x; tx < end_x;) {
            int start = c->progress[ty][tx], cols = 1;
            while (tx + cols < end_x && c->progress[ty][tx + cols] == start) cols++;
            int x0 = c->target[0] + tx * 8, y0 = c->target[1] + ty * 8;
            int x1 = x0 + cols * 8 - 1, y1 = y0 + rows * 8 - 1;
            for (int i = start; i < c->count; i++) {
                const ShadowCommand *cmd = &c->commands[i];
                if (cmd->kind == 3) continue; /* Its complete footprint has already replayed. */
                if (cmd->bounds[0] > x1 || cmd->bounds[1] > y1 ||
                    cmd->bounds[2] < x0 || cmd->bounds[3] < y0) continue;
                gp0_state = cmd->state;
                if (gp0_state.area_x0 < x0) gp0_state.area_x0 = x0;
                if (gp0_state.area_y0 < y0) gp0_state.area_y0 = y0;
                if (gp0_state.area_x1 > x1) gp0_state.area_x1 = x1;
                if (gp0_state.area_y1 > y1) gp0_state.area_y1 = y1;
                if (cmd->kind == 0) {
                    gp0_state = cmd->state;
                    psx_softgpu_shadow_poly_region(&cmd->mode, cmd->vertices, cmd->n, c->pixels, c->known,
                                                  c->target[0], c->target[1], SHADOW_W, SHADOW_H,
                                                  x0, y0, cols * 8, rows * 8);
                }
                else if (cmd->kind == 2)
                    psx_softgpu_shadow_rect(&cmd->mode, cmd->vertices, cmd->w, cmd->h, c->pixels, c->known,
                                           c->target[0], c->target[1], SHADOW_W, SHADOW_H);
                else {
                    int bx0 = cmd->bounds[0] > x0 ? cmd->bounds[0] : x0;
                    int by0 = cmd->bounds[1] > y0 ? cmd->bounds[1] : y0;
                    int bx1 = cmd->bounds[2] < x1 ? cmd->bounds[2] : x1;
                    int by1 = cmd->bounds[3] < y1 ? cmd->bounds[3] : y1;
                    for (int py = by0; py <= by1; py++)
                        memset(c->known + (py - c->target[1]) * SHADOW_W + bx0 - c->target[0], 0, bx1 - bx0 + 1);
                }
                psx_gx_stats_shadow_replay++;
            }
            for (int ry = ty; ry < ty + rows; ry++) for (int rx = tx; rx < tx + cols; rx++)
                c->progress[ry][rx] = c->count;
            tx += cols;
        }
        ty += rows;
    }
    gp0_state = saved;
}

static void shadow_finish_commands(ShadowContext *c) {
    shadow_materialize(c, c->target[0], c->target[1], c->target[2], c->target[3]);
    c->count = 0;
    memset(c->progress, 0, sizeof c->progress);
    memset(c->source_blocks, 0, sizeof c->source_blocks);
    psx_gx_stats_shadow_flush++;
}

static void shadow_prepare_sources(int x, int y, int w, int h) {
    for (int ci = 0; ci < 2; ci++) {
        ShadowContext *c = &shadow_contexts[ci];
        if (!c->valid || !shadow_source_blocks(c, x, y, w, h, 0)) continue;
        for (int i = 0; i < c->count; i++) {
            ShadowCommand *cmd = &c->commands[i];
            const Gp0Mode *m = &cmd->mode;
            if (!m->textured || cmd->kind == 1 || cmd->kind == 3) continue;
            if ((shadow_wrap_overlap(x, w, cmd->source[0], cmd->source[2], 1024) &&
                 shadow_wrap_overlap(y, h, cmd->source[1], cmd->source[3], 512)) ||
                (m->depth < 2 && shadow_wrap_overlap(x, w, m->clut_x, m->depth ? 256 : 16, 1024) &&
                 shadow_wrap_overlap(y, h, m->clut_y, 1, 512))) {
                /* Finish every destination pixel which used these old source
                 * bytes. Other geometry can remain deferred. All tiles in the
                 * command's footprint now have progress past this command, so
                 * retiring it cannot change a later query or repeat a blend. */
                shadow_materialize(c, cmd->bounds[0], cmd->bounds[1],
                    cmd->bounds[2] - cmd->bounds[0] + 1, cmd->bounds[3] - cmd->bounds[1] + 1);
                cmd->kind = 3;
                psx_gx_stats_shadow_retired++;
            }
        }
    }
}

static void shadow_before_write(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    shadow_prepare_sources(x, y, w, h);
    for (int ci = 0; ci < 2; ci++) {
        ShadowContext *c = &shadow_contexts[ci];
        if (!c->valid || c == shadow_sync_owner ||
            !shadow_wrap_overlap(x, w, c->target[0], c->target[2], 1024) ||
            !shadow_wrap_overlap(y, h, c->target[1], c->target[3], 512)) continue;
        /* A complete replacement needs no old destination pixels. Partial CPU
         * writes first preserve the other rendered pixels in real PSX VRAM. */
        if (!(x <= c->target[0] && y <= c->target[1] &&
              x + w >= c->target[0] + c->target[2] && y + h >= c->target[1] + c->target[3])) {
            shadow_sync_context(c, c->target[0], c->target[1], c->target[2], c->target[3]);
            shadow_finish_commands(c);
            c->reload = 1;
        } else c->valid = c->ready = c->count = c->reload = 0;
    }
}

static void shadow_sync_context(ShadowContext *c, int x, int y, int w, int h) {
    if (!c->valid || !shadow_overlap(x, y, w, h, c->target)) return;
    int x0 = x > c->target[0] ? x : c->target[0];
    int y0 = y > c->target[1] ? y : c->target[1];
    int x1 = x + w < c->target[0] + c->target[2] ? x + w : c->target[0] + c->target[2];
    int y1 = y + h < c->target[1] + c->target[3] ? y + h : c->target[1] + c->target[3];
    shadow_materialize(c, x0, y0, x1 - x0, y1 - y0);
    ShadowContext *saved_owner = shadow_sync_owner;
    shadow_sync_owner = c;
    psx_vram_touch(x0, y0, x1 - x0, y1 - y0);
    shadow_sync_owner = saved_owner;
    for (int py = y0; py < y1; py++) for (int px = x0; px < x1; px++) {
        int at = (py - c->target[1]) * SHADOW_W + px - c->target[0];
        if (c->known[at]) psx_vram[py * 1024 + px] = c->pixels[at];
    }
}

static void shadow_start(void) {
    int target[4] = {origin_x, origin_y, target_w, target_h};
    if (target[2] > SHADOW_W) target[2] = SHADOW_W;
    if (target[3] > SHADOW_H) target[3] = SHADOW_H;
    for (int ci = 0; ci < 2; ci++) {
        ShadowContext *c = &shadow_contexts[ci];
        if (c->valid && !memcmp(c->target, target, sizeof target)) {
            shadow_refresh(c);
            shadow_current = ci; c->stamp = ++shadow_stamp; return;
        }
    }
    for (int ci = 0; ci < 2; ci++) {
        ShadowContext *c = &shadow_contexts[ci];
        if (c->valid && shadow_overlap(target[0], target[1], target[2], target[3], c->target)) {
            shadow_sync_context(c, c->target[0], c->target[1], c->target[2], c->target[3]);
            c->valid = 0;
        }
    }
    int ci = !shadow_contexts[0].valid ? 0 : !shadow_contexts[1].valid ? 1 :
        shadow_contexts[0].stamp < shadow_contexts[1].stamp ? 0 : 1;
    ShadowContext *c = &shadow_contexts[ci];
    if (c->valid) shadow_sync_context(c, c->target[0], c->target[1], c->target[2], c->target[3]);
    c->valid = c->ready = 1; c->count = c->reload = 0; c->stamp = ++shadow_stamp;
    memcpy(c->target, target, sizeof target);
    memset(c->known, 1, sizeof c->known);
    memset(c->progress, 0, sizeof c->progress);
    memset(c->source_blocks, 0, sizeof c->source_blocks);
    for (int py = 0; py < target[3]; py++)
        memcpy(c->pixels + py * SHADOW_W, psx_vram + (target[1] + py) * 1024 + target[0], target[2] * sizeof(u16));
    shadow_current = ci;
}

static void shadow_source_bounds(const Gp0Mode *m, const Gp0Vtx *v, int n, int w, int h, int *out) {
    int u0 = v[0].u, u1 = u0, v0 = v[0].v, v1 = v0;
    for (int i = 1; i < n; i++) {
        if (v[i].u < u0) u0 = v[i].u;
        if (v[i].u > u1) u1 = v[i].u;
        if (v[i].v < v0) v0 = v[i].v;
        if (v[i].v > v1) v1 = v[i].v;
    }
    if (w) u1 = u0 + w - 1;
    if (h) v1 = v0 + h - 1;
    /* Fixed-point gradients can put a covered edge one fraction below its
     * vertex UV. Include that neighbouring texel, including wrapped index 255. */
    if (n >= 3) { u0--; v0--; }
    if (u0 < 0 || u1 > 255) { u0 = 0; u1 = 255; }
    if (v0 < 0 || v1 > 255) { v0 = 0; v1 = 255; }
    if (gp0_state.twin_mask_x) {
        int lo = 255, hi = 0;
        for (int u = u0; u <= u1; u++) {
            int s = (u & ~gp0_state.twin_mask_x) | (gp0_state.twin_off_x & gp0_state.twin_mask_x);
            if (s < lo) lo = s;
            if (s > hi) hi = s;
        }
        u0 = lo; u1 = hi;
    }
    if (gp0_state.twin_mask_y) {
        int lo = 255, hi = 0;
        for (int v = v0; v <= v1; v++) {
            int s = (v & ~gp0_state.twin_mask_y) | (gp0_state.twin_off_y & gp0_state.twin_mask_y);
            if (s < lo) lo = s;
            if (s > hi) hi = s;
        }
        v0 = lo; v1 = hi;
    }
    int shift = m->depth == 0 ? 2 : m->depth == 1 ? 1 : 0;
    out[0] = (m->tx + (u0 >> shift)) & 1023;
    out[1] = (m->ty + v0) & 511;
    out[2] = (u1 >> shift) - (u0 >> shift) + 1; out[3] = v1 - v0 + 1;
}

static void shadow_require_texture(const Gp0Mode *m, const Gp0Vtx *v, int n, int w, int h) {
    if (!m->textured) return;
    int source[4];
    shadow_source_bounds(m, v, n, w, h, source);
    psx_vram_read(source[0], source[1], source[2], source[3]);
    if (m->depth < 2) psx_vram_read(m->clut_x, m->clut_y, m->depth ? 256 : 16, 1);
}

static void shadow_record(int kind, const Gp0Mode *m, const Gp0Vtx *v, int n, int w, int h) {
    shadow_require_texture(m, v, n, w, h);
    shadow_start();
    ShadowContext *c = &shadow_contexts[shadow_current];
    if (c->count == SHADOW_COMMANDS) shadow_finish_commands(c);
    ShadowCommand *cmd = &c->commands[c->count];
    cmd->kind = kind; cmd->mode = *m; cmd->n = n; cmd->w = w; cmd->h = h;
    memcpy(cmd->vertices, v, n * sizeof(*v)); cmd->state = gp0_state;
    int x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (int i = 1; i < n; i++) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    if (w) x1 = x0 + w - 1;
    if (h) y1 = y0 + h - 1;
    if (kind == 1) {
        cmd->state.area_x0 = c->target[0]; cmd->state.area_y0 = c->target[1];
        cmd->state.area_x1 = c->target[0] + c->target[2] - 1;
        cmd->state.area_y1 = c->target[1] + c->target[3] - 1;
    }
    if (x0 < cmd->state.area_x0) x0 = cmd->state.area_x0;
    if (y0 < cmd->state.area_y0) y0 = cmd->state.area_y0;
    if (x1 > cmd->state.area_x1) x1 = cmd->state.area_x1;
    if (y1 > cmd->state.area_y1) y1 = cmd->state.area_y1;
    if (x0 < c->target[0]) x0 = c->target[0];
    if (y0 < c->target[1]) y0 = c->target[1];
    if (x1 >= c->target[0] + c->target[2]) x1 = c->target[0] + c->target[2] - 1;
    if (y1 >= c->target[1] + c->target[3]) y1 = c->target[1] + c->target[3] - 1;
    if (x0 > x1 || y0 > y1) return;
    cmd->bounds[0] = x0; cmd->bounds[1] = y0; cmd->bounds[2] = x1; cmd->bounds[3] = y1;
    if (m->textured) {
        shadow_source_bounds(m, v, n, w, h, cmd->source);
        shadow_source_blocks(c, cmd->source[0], cmd->source[1], cmd->source[2], cmd->source[3], 1);
        if (m->depth < 2) shadow_source_blocks(c, m->clut_x, m->clut_y, m->depth ? 256 : 16, 1, 1);
    }
    c->count++;
}
