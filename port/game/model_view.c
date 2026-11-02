/* A party member's 3D model on the top screen (the Equip screen's view, docs/ui-design.md), turning slowly in its
 * battle idle with the weapon it holds. Port-side, as the decomp recommends (ff9-decomp docs/r32/3ds-requests-5-models.md
 * section 3: the game's loaders need the battle work area and VRAM layout, unsafe while a menu is up):
 *   1. kind = Character.info.b.kind (chr_calcAppearance keeps it current, also in Equip) -> the battle body model,
 *      its idle motion and the bone that holds the weapon (the table below, from g_btlCharModelIds,
 *      g_btlPartyMotionIds[kind][0] and btl_initWeapon); the weapon's model from g_weaponTable[equip[0]] (+2)
 *   2. the body file (FF9.IMG dir 10, file id & 0xFFFF = model id) and the weapon's (dir 8) read from the disc
 *   3. DB containers walked to the header 0x02<id>, the motion 0x03<..> and the TIMs 0x04<n> (offsets in the header and
 *      the motion are relative to their own start)
 *   4. TIMs decoded to RGBA (8-bit, own CLUT, texel 0 clear): the body's uvs are TIM-local, the weapon's v is 0x50 less
 *   5. motion frame decoded and the bones composed as btl_calcBoneMatrices does (tools/battle_pose.py)
 *   6. vertices posed (rigid: one bone each), turned about y, projected and handed to the renderer (render.c
 *      ctr_model_*), quads as triangles (0,1,2) + (1,2,3), back faces dropped unless double-sided */
#define M2CTX
#include "ff9.h"

#include <stdlib.h>


void psx_cd_read_data(unsigned int lba, void *buf, unsigned int n);
void ctr_model_texture(int slot, const u32 *rgba);
void ctr_model_tri(int tex, const float *x, const float *y, const float *z, const float *u, const float *v,
                   const float *par);
void port_log(const char *, ...);

/* per appearance kind: battle body model, idle motion (0x03xxxx), weapon bone */
static const struct { u16 body; u32 idle; u8 bone; } kinds[19] = {
    {5414, 0x03007D, 0x0D}, {285, 0x0335F8, 0x0D}, {5415, 0x030BDC, 0x10}, {281, 0x0317D5, 0x0F},
    {283, 0x032CE2, 0x0F},  {288, 0x032BA2, 0x0F}, {287, 0x032D72, 0x0F}, {655, 0x030CE1, 0x10},
    {286, 0x030CE1, 0x10},  {289, 0x032996, 0x0E}, {284, 0x0331ED, 0x0F}, {291, 0x0337F4, 0x0F},
    {290, 0x031D19, 0x06},  {444, 0x0337AE, 0x10}, {39, 0x0313DA, 0x19},  {45, 0x0315A8, 0x06},
    {42, 0x03155A, 0x0E},   {608, 0x032F3E, 0x0E}, {358, 0x0332A9, 0x10},
};

/* ---- DB containers (main db_lookup; tools/encounter_catalog.py db_items) ---- */
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }

/* offset of item `key` (kind << 16 | id) in the container at base, -1 none */
static long db_find(const u8 *d, long size, long base, u32 key) {
    if (base < 0 || base + 4 > size || d[base] != 0xDB) return -1;
    for (int i = 0; i < d[base + 1]; i++) {
        long ent = base + 4 + i * 4;
        if (ent + 4 > size) return -1;
        u32 v = rd32(d + ent);
        long blk = ent + (v & 0xFFFFFF);
        if (blk + 4 > size) return -1;
        int cnt = d[blk + 1];
        long ofs = (blk + 4 + cnt * 2 + 3) & ~3L;
        for (int j = 0; j < cnt; j++) {
            if (((v >> 24 & 0x7F) << 16 | rd16(d + blk + 4 + j * 2)) != key) continue;
            long o = ofs + 4 * j;
            return o + (rd32(d + o) & 0xFFFFFF);
        }
    }
    return -1;
}

/* offset of the n-th item of the container at base (in its blocks' order), -1 none */
static long db_nth(const u8 *d, long size, long base, int n) {
    if (base < 0 || base + 4 > size || d[base] != 0xDB) return -1;
    for (int i = 0; i < d[base + 1]; i++) {
        long ent = base + 4 + i * 4;
        u32 v = rd32(d + ent);
        long blk = ent + (v & 0xFFFFFF);
        if (blk + 4 > size) return -1;
        int cnt = d[blk + 1];
        long ofs = (blk + 4 + cnt * 2 + 3) & ~3L;
        if (n < cnt) return ofs + 4 * n + (rd32(d + ofs + 4 * n) & 0xFFFFFF);
        n -= cnt;
    }
    return -1;
}

static u8 *read_file(int dir, int id, long *size) {
    FileEntry *f = file_find(dir, id);
    if (!f) return NULL;
    unsigned n = (unsigned)(f->link - f->sector);
    if (!n || n > 512) return NULL;
    u8 *d = malloc(n * 2048);
    if (!d) return NULL;
    psx_cd_read_data((unsigned)(g_gameState->imgSector + f->sector), d, n);
    *size = (long)n * 2048;
    return d;
}

/* a TIM (8-bit, own CLUT) at d+o into a 256x256 RGBA image, its pixels from (0, voff) on; 0 if not one */
static int tim_rgba(const u8 *d, long size, long o, u32 *out, int voff) {
    if (o < 0 || o + 8 > size || rd32(d + o) != 0x10) return 0;
    u32 flags = rd32(d + o + 4);
    if ((flags & 7) != 1 || !(flags & 8)) return 0;
    long c = o + 8;
    int cw = rd16(d + c + 8), ch = rd16(d + c + 10);
    const u8 *clut = d + c + 12;
    long im = c + rd32(d + c);
    if (im + 12 > size) return 0;
    int w = rd16(d + im + 8) * 2, h = rd16(d + im + 10);
    const u8 *px = d + im + 12;
    if (im + 12 + (long)w * h > size || cw * ch < 256) return 0;
    for (int y = 0; y < h && y + voff < 256; y++)
        for (int x = 0; x < w && x < 256; x++) {
            u16 col = rd16(clut + px[y * w + x] * 2);
            u32 r = (col & 31) << 3, g = (col >> 5 & 31) << 3, b = (col >> 10 & 31) << 3;
            out[(y + voff) * 256 + x] = col ? r << 24 | g << 16 | b << 8 | 0xFF : 0;
        }
    return 1;
}

/* ---- posing (btl_calcBoneMatrices, tools/battle_pose.py) ---- */
typedef struct { s32 m[3][3]; s32 t[3]; } Mat;

static s32 isin(s32 a) { return math_sin(a & 0xFFF); }
static s32 icos(s32 a) { return math_cos(a & 0xFFF); }

static void rot_zyx(s32 vx, s32 vy, s32 vz, s32 r[3][3]) {
    s32 sx = isin(vx), cx = icos(vx), sy = isin(vy), cy = icos(vy), sz = isin(vz), cz = icos(vz);
    /* R = Rz . Ry . Rx */
    r[0][0] = cz * cy >> 12;
    r[0][1] = ((cz * sy >> 12) * sx >> 12) - (sz * cx >> 12);
    r[0][2] = ((cz * sy >> 12) * cx >> 12) + (sz * sx >> 12);
    r[1][0] = sz * cy >> 12;
    r[1][1] = ((sz * sy >> 12) * sx >> 12) + (cz * cx >> 12);
    r[1][2] = ((sz * sy >> 12) * cx >> 12) - (cz * sx >> 12);
    r[2][0] = -sy;
    r[2][1] = cy * sx >> 12;
    r[2][2] = cy * cx >> 12;
}

static void compose(const s32 p[3][3], const s32 l[3][3], s32 out[3][3]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) out[r][c] = (p[r][0] * l[0][c] + p[r][1] * l[1][c] + p[r][2] * l[2][c]) >> 12;
}

static void apply(const Mat *b, const s16 *v, s32 *o) {
    for (int r = 0; r < 3; r++) o[r] = b->t[r] + ((b->m[r][0] * v[0] + b->m[r][1] * v[1] + b->m[r][2] * v[2]) >> 12);
}

/* one frame of motion at a (offsets relative to it) for n bones: the root translation and angles per bone */
static int decode_frame(const u8 *a, long avail, int n, int frame, s32 root[3], s16 (*ang)[3]) {
    if (avail < 0x14) return 0;
    u16 rx = rd16(a + 4), ry = rd16(a + 6), rz = rd16(a + 8), cmask = rd16(a + 0xA);
    s32 keys = (s32)rd32(a + 0xC), lo = (s32)rd32(a + 0x10);
    u16 rv[3] = {rx, ry, rz};
    for (int j = 0; j < 3; j++)
        root[j] = cmask >> j & 1 ? (s16)rv[j] : (s16)rd16(a + rv[j] + frame * 2);
    int half = frame >> 1, odd = frame & 1;
    for (int i = 0; i < n; i++) {
        const u8 *k = a + keys + 8 * i;
        u16 km = rd16(k + 6);
        for (int j = 0; j < 3; j++) {
            u16 kv = rd16(k + j * 2);
            s32 hi = km >> j & 1 ? kv : (s8)a[kv + frame];
            s32 nib = 0;
            if (lo) {
                const u8 *l = a + lo + 8 * i;
                u16 lm = rd16(l + 6), lv = rd16(l + j * 2);
                u32 v = lm >> j & 1 ? lv : ((u32)(s32)(s8)a[lv + half]) >> (odd ? 4 : 0);
                nib = v & 0xF;
            }
            ang[i][j] = (s16)((hi << 4 & 0xFFFF) | nib);
        }
    }
    return 1;
}

/* ---- the loaded models: one per slot (Equip's view uses slot 0, Order's formation 0-3), each with its three
 * textures in the renderer's slots 3 slot .. 3 slot + 2 ---- */
#define SLOTS 4
typedef struct {
    int kind, weapon_item, ok;
    u8 *body, *wpn;
    long body_size, wpn_size, head, motion, whead;
    int frames;
} Loaded;
static Loaded ms[SLOTS] = {{-1, -1, 0, NULL, NULL, 0, 0, 0, 0, 0, 0}, {-1, -1, 0, NULL, NULL, 0, 0, 0, 0, 0, 0},
                           {-1, -1, 0, NULL, NULL, 0, 0, 0, 0, 0, 0}, {-1, -1, 0, NULL, NULL, 0, 0, 0, 0, 0, 0}};
static u32 texbuf[256 * 256];

static void unload(Loaded *m) {
    free(m->body);
    free(m->wpn);
    m->body = m->wpn = NULL;
    m->ok = 0;
}

static int load(int slot, int kind, int item) {
    Loaded *m = &ms[slot];
    unload(m);
    m->kind = kind;
    m->weapon_item = item;
    if (kind < 0 || kind >= 19) return 0;
    int id = kinds[kind].body;
    m->body = read_file(10, id, &m->body_size);
    if (!m->body) return 0;
    long pack = db_find(m->body, m->body_size, 0, 0x1B0000), tims = db_find(m->body, m->body_size, 0, 0x1B0001);
    m->head = db_find(m->body, m->body_size, pack, 0x020000 | id);
    m->motion = db_find(m->body, m->body_size, pack, kinds[kind].idle);
    if (m->head < 0 || m->motion < 0) { port_log("model view: model %d: no header/motion\n", id); return 0; }
    m->frames = rd16(m->body + m->motion + 2);
    for (int n = 0; n < 2; n++) { /* (the body's two textures: TIM-local uvs) */
        __builtin_memset(texbuf, 0, sizeof texbuf);
        long to = db_nth(m->body, m->body_size, tims, n); /* (keyed by their VRAM place: in order) */
        int ok = tim_rgba(m->body, m->body_size, to, texbuf, 0);
        if (ok) ctr_model_texture(slot * 3 + n, texbuf);
    }
    m->whead = -1;
    int wid = item >= 0 && item < 0x58 ? rd16((const u8 *)g_weaponTable + item * 0xC + 2) : 0;
    if (wid) m->wpn = read_file(8, wid, &m->wpn_size);
    if (m->wpn) {
        long wpack = db_find(m->wpn, m->wpn_size, 0, 0x1B0000), wtims = db_find(m->wpn, m->wpn_size, 0, 0x1B0001);
        m->whead = db_find(m->wpn, m->wpn_size, wpack, 0x020000 | wid);
        __builtin_memset(texbuf, 0, sizeof texbuf);
        if (!tim_rgba(m->wpn, m->wpn_size, db_nth(m->wpn, m->wpn_size, wtims, 0), texbuf, 0x50)) m->whead = -1;
        else ctr_model_texture(slot * 3 + 2, texbuf); /* (its v is 0x50 less: placed at 0x50) */
    }
    port_log("model view %d: body %d (motion 0x%06x, %d frames), weapon %d%s\n", slot, id, kinds[kind].idle,
             m->frames, wid, m->whead >= 0 ? "" : " (none)");
    m->ok = 1;
    return 1;
}

/* the view: model space -> the turn (and place: its t) -> the camera's tilt, then a perspective about (cx, cy) */
typedef struct {
    Mat turn, cam;
    float cx, cy, scale, dist;
} View;

/* the groups of the model at d+h posed by bones[], drawn with texture slots tex_base + (0, 1) (body) or tex_base (a
 * weapon) */
static void draw(const u8 *d, long h, const Mat *bones, int nbones_used, int tex_base, int weapon, const View *w) {
    int nb = d[h + 2], ng = d[h + 3];
    long mesh = h + (s32)rd32(d + h + 0x10);
    static s32 pos[1024][3];
    for (int g = 0; g < ng; g++) {
        const u8 *gm = d + mesh + g * 0x28;
        const u8 *vc = d + h + rd32(gm + 0x14);
        const u8 *vs = d + h + rd32(gm + 0x18);
        const u8 *pr = d + h + rd32(gm + 0x1C);
        const u8 *uv = d + h + rd32(gm + 0x20);
        int k = 0;
        for (int b = 0; b < nb && b < nbones_used; b++) {
            int n = rd16(vc + b * 2);
            for (int i = 0; i < n && k < 1024; i++, k++) {
                s16 v[3] = {(s16)rd16(vs + k * 8), (s16)rd16(vs + k * 8 + 2), (s16)rd16(vs + k * 8 + 4)};
                s32 p[3], q[3];
                apply(&bones[b], v, p);
                for (int r = 0; r < 3; r++) /* (placed: past s16) */
                    q[r] = w->turn.t[r] + ((w->turn.m[r][0] * p[0] + w->turn.m[r][1] * p[1] + w->turn.m[r][2] * p[2]) >> 12);
                for (int r = 0; r < 3; r++)
                    pos[k][r] = (w->cam.m[r][0] * q[0] + w->cam.m[r][1] * q[1] + w->cam.m[r][2] * q[2]) >> 12;
            }
        }
        int nft4 = rd16(gm + 2), nft3 = rd16(gm + 4);
        for (int i = 0; i < nft4 + nft3; i++) {
            int quad = i < nft4;
            const u8 *r = quad ? pr + i * 0x18 : pr + nft4 * 0x18 + (i - nft4) * 0x14;
            int nv = quad ? 4 : 3, tex = quad ? r[0x13] : r[6], flags = quad ? r[0x15] : r[0x12];
            int vi[4], ui[4];
            for (int c = 0; c < nv; c++) {
                vi[c] = rd16(r + c * 2);
                ui[c] = rd16(r + (quad ? 8 : 0xC) + c * 2);
            }
            for (int t = 0; t < (quad ? 2 : 1); t++) {
                static const int corners[2][3] = {{0, 1, 2}, {1, 2, 3}};
                float x[3], y[3], z[3], u[3], v[3], par[3];
                for (int c = 0; c < 3; c++) {
                    int a = corners[t][c];
                    if (vi[a] >= k) goto next;
                    float px = (float)pos[vi[a]][0], py = (float)pos[vi[a]][1], pz = (float)pos[vi[a]][2];
                    float f = w->dist / (w->dist + pz); /* (a gentle perspective) */
                    x[c] = w->cx + px * w->scale * f;
                    y[c] = w->cy + py * w->scale * f;
                    z[c] = pz;
                    u[c] = uv[ui[a] * 2];
                    v[c] = uv[ui[a] * 2 + 1];
                    par[c] = -pz * 0.02f;
                }
                if (!(flags & 2)) { /* (front faces only: clockwise on screen, y down) */
                    float cr = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
                    if ((t == 0 ? cr : -cr) <= 0) continue;
                }
                ctr_model_tri(weapon ? tex_base : tex_base + (tex & 1), x, y, z, u, v, par);
            next:;
            }
        }
    }
}

/* character c in slot `slot`, in its idle motion `phase` frames along, seen through w */
static void draw_member(int slot, Character *c, int phase, const View *w) {
    int kind = (int)c->info.b.kind, item = c->equip[0];
    Loaded *m = &ms[slot];
    unsigned ctr_model_generation(void);
    static unsigned gen;
    if (gen != ctr_model_generation()) { /* (the renderer gave their textures back: load again) */
        gen = ctr_model_generation();
        for (int i = 0; i < SLOTS; i++) unload(&ms[i]), ms[i].kind = -1;
    }
    if (kind != m->kind || item != m->weapon_item) load(slot, kind, item);
    if (!m->ok) return;
    int ticks = (int)VSync(-1);
    int frame = (ticks / 4 + phase) % (m->frames ? m->frames : 1); /* (15 frames a second, as in battle) */
    const u8 *d = m->body;
    long h = m->head;
    int nb = d[h + 2];
    if (nb > 64) return;
    Mat bones[64];
    s32 root[3];
    s16 ang[64][3];
    if (!decode_frame(d + m->motion, m->body_size - m->motion, nb, frame, root, ang)) return;
    const u8 *bd = d + h + (s32)rd32(d + h + 0xC);
    for (int i = 0; i < nb; i++) {
        s32 l[3][3];
        rot_zyx(ang[i][0], ang[i][1], ang[i][2], l);
        if (i == 0) {
            __builtin_memcpy(bones[0].m, l, sizeof l);
            bones[0].t[0] = root[0], bones[0].t[1] = root[1], bones[0].t[2] = root[2];
            continue;
        }
        int p = bd[i * 4 + 3];
        if (p >= i) p = 0;
        compose(bones[p].m, l, bones[i].m);
        s16 len[3] = {0, 0, (s16)rd16(bd + i * 4)};
        apply(&bones[p], len, bones[i].t);
    }
    draw(d, h, bones, nb, slot * 3, 0, w);
    if (m->whead >= 0) {
        int wb = kinds[kind].bone < nb ? kinds[kind].bone : 0;
        draw(m->wpn, m->whead, &bones[wb], 1, slot * 3 + 2, 1, w);
    }
}

/* the Equip view: character c turning at (cx, cy) on the top screen, about `height` pixels tall */
void port_model_view(Character *c, float cx, float cy, float height) {
    if (!c) return;
    View w;
    rot_zyx(0, ((int)VSync(-1) * 6) & 0xFFF, 0, w.turn.m); /* (turntable: about y, the feet (y 0) low on the screen) */
    w.turn.t[0] = w.turn.t[1] = w.turn.t[2] = 0;
    rot_zyx(0, 0, 0, w.cam.m);
    w.cx = cx, w.cy = cy, w.scale = height / 520.0f, w.dist = 1400.0f;
    draw_member(0, c, 0, &w);
}

/* Order: the party as they stand in battle, seen from behind them and above, turned toward where the enemies would
 * be (up the screen): members across in party order, the back row a step nearer the camera. Each member's feet on
 * the screen go to feet[i] (x, y; x < 0: not drawn), for their names and the cursor. */
#define FORM_SPACING 470
#define FORM_BACK 420
#define FORM_YAW 0xA00
#define FORM_TILT 0x130
void port_party_view(Character *const party[4], const int backrow[4], float height, float feet[4][2]) {
    View w;
    rot_zyx(FORM_TILT, 0, 0, w.cam.m);
    w.cx = 200, w.cy = 140, w.scale = height / 520.0f, w.dist = 3000.0f;
    for (int i = 0; i < 4; i++) {
        feet[i][0] = -1;
        if (!party[i]) continue;
        rot_zyx(0, FORM_YAW, 0, w.turn.m);
        w.turn.t[0] = (s32)((i - 1.5f) * FORM_SPACING);
        w.turn.t[1] = 0;
        w.turn.t[2] = backrow[i] ? -FORM_BACK : 0;
        draw_member(i, party[i], i * 3, &w);
        /* (where its feet land: the same transform of its place) */
        s32 q[3] = {w.turn.t[0], 0, w.turn.t[2]}, p[3];
        for (int r = 0; r < 3; r++) p[r] = (w.cam.m[r][0] * q[0] + w.cam.m[r][1] * q[1] + w.cam.m[r][2] * q[2]) >> 12;
        float f = w.dist / (w.dist + p[2]);
        feet[i][0] = w.cx + p[0] * w.scale * f;
        feet[i][1] = w.cy + p[1] * w.scale * f;
    }
}
