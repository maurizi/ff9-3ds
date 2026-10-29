#!/usr/bin/env python3
"""Build-time simplification of the heaviest battle enemy models (the user's choice of 2026-10-29: enemies that put
battles past the DS 3D engine's 6144 vertices, never the party), as sector patches the port's CD layer serves in place
of the disc image's (port/psx/cd.c, ff9ds/patches/discN.bin). Outputs only under build/ or ff9ds/ (game data).

Format and constraints: decomp docs/battle-models.md (R26). A model file (FF9.IMG dir index 7, file id = model id)
keeps its size, its split and its layout: per mesh group only the vertex and polygon arrays shrink, in place (the
uv array, the colours, the motions and the TIMs stay where they are; a moved corner takes the uv entry of the vertex
it moved onto, which already exists). Bones, groups, texture indices, otBias, height and radius are untouched.

Simplification: half-edge collapses (vertex u onto a neighbour v), cheapest first by quadric error, only where
  - u and v belong to the same bone, and so does every triangle around u (positions are bone-local),
  - the triangles around u share one texture, colour and flags, and one uv at u (no seam), and form a closed fan,
  - u is in no quad (FT4s stay as they are),
  - no remaining triangle around u flips or degenerates,
  - u is no sharp feature (its triangles' normals within 40 degrees of each other: teeth and folds stay).

    python3 tools/simplify_battle_models.py --img DIR --manifest config/model_patches.txt --out build/patches
    python3 tools/simplify_battle_models.py --img DIR --manifest config/model_patches.txt --out build/patches --update
    python3 tools/simplify_battle_models.py --img DIR --models 95,145 --ratio 0.25 --out ff9ds/patches
    python3 tools/simplify_battle_models.py --img DIR --models 95 --ratio 0.25 --report   # counts and errors only
"""
import argparse, hashlib, heapq, math, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'decomp', 'tools'))
import battle_models as bm          # noqa: E402
import encounter_catalog as ec      # noqa: E402

SECTOR = 2048
FT4, FT3 = 0x18, 0x14
PATCH_MAGIC = b'FF9P'


def u16(d, o): return struct.unpack_from('<H', d, o)[0]


def u32(d, o): return struct.unpack_from('<I', d, o)[0]


class Group:
    """One mesh group as stored (offsets absolute in the file)."""
    def __init__(self, data, h, m, nbones):
        self.m = m
        self.counts = list(struct.unpack_from('<8H', data, m + 2))
        if any(self.counts[2:]):
            raise ValueError('only FT4/FT3 groups are handled')
        self.vc_o, self.v_o, self.p_o, self.uv_o, self.c_o = (h + x for x in struct.unpack_from('<5I', data, m + 0x14))
        self.vcounts = list(struct.unpack_from(f'<{nbones}H', data, self.vc_o))
        self.bone = [b for b, n in enumerate(self.vcounts) for _ in range(n)]
        nv = len(self.bone)
        self.V = [struct.unpack_from('<4h', data, self.v_o + 8 * k) for k in range(nv)]
        self.uvs = [u16(data, self.uv_o + 2 * k) for k in range((self.c_o - self.uv_o) // 2)]
        p = self.p_o
        self.quads = [bytes(data[p + k * FT4:p + (k + 1) * FT4]) for k in range(self.counts[0])]
        p += self.counts[0] * FT4
        self.tris = [bytearray(data[p + k * FT3:p + (k + 1) * FT3]) for k in range(self.counts[1])]

    # FT3 fields
    @staticmethod
    def tv(t): return list(struct.unpack_from('<3H', t, 0))

    @staticmethod
    def tuv(t): return list(struct.unpack_from('<3H', t, 0xC))

    @staticmethod
    def attr(t): return (t[6], bytes(t[8:11]), t[0x12])


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def plane_quadric(p0, p1, p2):
    n = cross(sub(p1, p0), sub(p2, p0))
    l = math.sqrt(dot(n, n))
    if not l:
        return None
    a, b, c = n[0] / l, n[1] / l, n[2] / l
    d = -(a * p0[0] + b * p0[1] + c * p0[2])
    w = 1.0  # (unweighted: the cost is a squared distance)
    v = (a, b, c, d)
    return [w * v[i] * v[j] for i in range(4) for j in range(4)]


def q_add(a, b): return [x + y for x, y in zip(a, b)]


def q_cost(q, p):
    v = (p[0], p[1], p[2], 1.0)
    return sum(q[i * 4 + j] * v[i] * v[j] for i in range(4) for j in range(4))


def simplify(g, ratio, max_error=1e9, sharp=40.0, bake=False, split_quads=True):
    """Remove about `ratio` of the group's triangles. Returns (removed triangles, max error, collapses)."""
    nv = len(g.V)
    P = [v[:3] for v in g.V]
    # the quads as their two triangles (the GPU draws an FT4 as (0, 1, 2) and (1, 2, 3)); pairs that come through
    # untouched are quads again at the end
    n_tris = len(g.tris)
    halves = []
    for q in (g.quads if split_quads else []):
        v4 = struct.unpack_from('<4H', q, 0)
        uv4 = struct.unpack_from('<4H', q, 8)
        for order in ((0, 1, 2), (1, 3, 2)):
            t = bytearray(FT3)
            struct.pack_into('<3H', t, 0, *[v4[k] for k in order])
            t[6] = q[0x13]
            t[8:11] = q[0x10:0x13]
            struct.pack_into('<3H', t, 0xC, *[uv4[k] for k in order])
            t[0x12] = q[0x15]
            g.tris.append(t)
            halves.append(bytes(t))
    quads0 = g.quads if split_quads else []
    kept_quads = [] if split_quads else g.quads
    g.quads = []
    orig_bytes = [bytes(t) for t in g.tris]  # (each triangle as it was, quads' halves included: changed ones are baked)
    alive = [True] * len(g.tris)
    vt = [set() for _ in range(nv)]
    for i, t in enumerate(g.tris):
        for v in g.tv(t):
            vt[v].add(i)
    in_quad = set(x for q in kept_quads for x in struct.unpack_from('<4H', q, 0))  # (quads kept whole: theirs stay)
    Q = [[0.0] * 16 for _ in range(nv)]
    for t in g.tris:
        a, b, c = g.tv(t)
        q = plane_quadric(P[a], P[b], P[c])
        if q:
            for v in (a, b, c):
                Q[v] = q_add(Q[v], q)
    removed_v = [False] * nv

    def corner_uv(t, v):
        vs = g.tv(t)
        return g.tuv(t)[vs.index(v)]

    def chart(t, v):
        return g.attr(t), g.uvs[corner_uv(t, v)]

    def candidate(u):
        """The best collapse of u: (cost, v, uv index for u's corners) or None."""
        if removed_v[u] or u in in_quad or not vt[u]:
            return None
        fan = [g.tris[i] for i in vt[u]]
        # the texture charts around u (texture, colour, flags and u's uv): a seam's vertex has several, and moves along
        # it (each chart's corners at u take that chart's uv at v; the baking redraws the texels that moved)
        if not bake and len({chart(t, u) for t in fan}) > 1:
            return None
        edges = {}
        for t in fan:
            vs = g.tv(t)
            if any(g.bone[x] != g.bone[u] for x in vs):
                return None
            for x in vs:
                if x != u:
                    edges[x] = edges.get(x, 0) + 1
        if any(n != 2 for n in edges.values()):  # a closed fan: each neighbour on two triangles
            return None
        # a sharp feature (a tooth's tip, a fold): its triangles' normals more than `sharp` degrees apart
        ns = []
        for t in fan:
            vs = g.tv(t)
            n = cross(sub(P[vs[1]], P[vs[0]]), sub(P[vs[2]], P[vs[0]]))
            l = math.sqrt(dot(n, n))
            if not l:
                return None
            ns.append((n[0] / l, n[1] / l, n[2] / l))
        if any(dot(a, b) < cos_sharp for i, a in enumerate(ns) for b in ns[i + 1:]):
            return None
        best = None
        charts = {chart(t, u) for t in fan}
        for v in edges:
            # in each chart, v's uv where u's triangles touch it: one value (the corners moving onto v take it)
            uvi = {}
            ok = True
            for c in charts:
                vals = {g.uvs[corner_uv(t, v)] for t in fan if chart(t, u) == c and v in g.tv(t)}
                if len(vals) != 1:
                    ok = False
                    break
                uvi[c] = next(corner_uv(t, v) for t in fan if chart(t, u) == c and v in g.tv(t))
            if not ok:
                continue
            for t in fan:
                vs = g.tv(t)
                if v in vs:
                    continue
                old = cross(sub(P[vs[1]], P[vs[0]]), sub(P[vs[2]], P[vs[0]]))
                nvs = [v if x == u else x for x in vs]
                new = cross(sub(P[nvs[1]], P[nvs[0]]), sub(P[nvs[2]], P[nvs[0]]))
                if dot(old, new) <= 0.2 * math.sqrt(dot(old, old) * dot(new, new)):
                    ok = False
                    break
                # its texture's stretch (surface per texel) kept within 1.5x: a small texture area spread over more
                # surface (a nostril's dark texels over the back) is beyond what baking can redraw
                uvs_t = [g.uvs[k] for k in g.tuv(t)]
                nuv = [g.uvs[uvi[chart(t, u)]] if x == u else uvs_t[j] for j, x in enumerate(vs)]
                def uv_area(w):
                    return abs(((w[1] & 0xFF) - (w[0] & 0xFF)) * ((w[2] >> 8) - (w[0] >> 8)) -
                               ((w[2] & 0xFF) - (w[0] & 0xFF)) * ((w[1] >> 8) - (w[0] >> 8)))
                a_old, a_new = math.sqrt(dot(old, old)), math.sqrt(dot(new, new))
                t_old, t_new = uv_area(uvs_t), uv_area(nuv)
                if t_old and t_new:
                    ratio = (a_new / t_new) / (a_old / t_old)
                    if ratio > 1.5 or ratio < 1 / 1.5:
                        ok = False
                        break
                elif t_old != t_new:
                    ok = False
                    break
            if not ok:
                continue
            cost = q_cost(q_add(Q[u], Q[v]), P[v])
            if best is None or cost < best[0]:
                best = (cost, v, uvi)
        return best

    cos_sharp = math.cos(math.radians(sharp))
    heap = []
    stamp = [0] * nv
    for u in range(nv):
        c = candidate(u)
        if c:
            heapq.heappush(heap, (c[0], u, stamp[u]))
    target = int(len(g.tris) * ratio)
    gone = 0
    max_err = 0.0
    collapses = 0
    while heap and gone < target:
        cost, u, st = heapq.heappop(heap)
        if st != stamp[u] or removed_v[u]:
            continue
        c = candidate(u)
        if not c:
            continue
        if c[0] > cost + 1e-6:  # stale: costs only grow
            stamp[u] += 1
            heapq.heappush(heap, (c[0], u, stamp[u]))
            continue
        if math.sqrt(max(c[0], 0.0)) > max_error:
            break  # (the cheapest left costs more: done)
        _, v, uvi = c
        touched = set()
        for i in list(vt[u]):
            t = g.tris[i]
            vs = g.tv(t)
            if v in vs:
                alive[i] = False
                gone += 1
                for x in vs:
                    vt[x].discard(i)
                    touched.add(x)
                continue
            k = vs.index(u)
            ui = uvi[chart(t, u)]
            struct.pack_into('<H', t, 2 * k, v)
            struct.pack_into('<H', t, 0xC + 2 * k, ui)
            vt[v].add(i)
            touched.update(vs)
        vt[u] = set()
        removed_v[u] = True
        Q[v] = q_add(Q[v], Q[u])
        max_err = max(max_err, math.sqrt(max(cost, 0.0)))
        collapses += 1
        touched.add(v)
        for x in touched:
            if x == u or removed_v[x]:
                continue
            stamp[x] += 1
            cc = candidate(x)
            if cc:
                heapq.heappush(heap, (cc[0], x, stamp[x]))
            for y in {w for i in vt[x] for w in g.tv(g.tris[i])} - {x}:
                if not removed_v[y]:
                    stamp[y] += 1
                    cy = candidate(y)
                    if cy:
                        heapq.heappush(heap, (cy[0], y, stamp[y]))
    g.quads = [bytearray(q) for q in kept_quads]
    # the quads whose two triangles are as they were
    for qi, q in enumerate(quads0):
        a, b = n_tris + 2 * qi, n_tris + 2 * qi + 1
        if alive[a] and alive[b] and bytes(g.tris[a]) == halves[2 * qi] and bytes(g.tris[b]) == halves[2 * qi + 1]:
            alive[a] = alive[b] = False
            g.quads.append(bytearray(q))
    g.changed = [bytes(t) != orig_bytes[i] for i, (t, a) in enumerate(zip(g.tris, alive)) if a]
    g.tris = [t for t, a in zip(g.tris, alive) if a]
    # the vertices still used, in bone order (vertexCounts per bone), and the polygons' indices remapped
    used = set(x for t in g.tris for x in g.tv(t)) | set(x for q in g.quads for x in struct.unpack_from('<4H', q, 0))
    keep = [k for k in range(nv) if k in used]
    remap = {k: i for i, k in enumerate(keep)}
    g.vcounts = [sum(1 for k in keep if g.bone[k] == b) for b in range(len(g.vcounts))]
    g.bone_of = [g.bone[k] for k in keep]
    g.V = [g.V[k] for k in keep]
    for t in g.tris:
        struct.pack_into('<3H', t, 0, *[remap[x] for x in g.tv(t)])
    g.quads = [bytearray(q) for q in g.quads]
    for q in g.quads:
        struct.pack_into('<4H', q, 0, *[remap[x] for x in struct.unpack_from('<4H', q, 0)])
    return gone, max_err, collapses



def load_tims(data):
    """The model file's textures (block 0x1B item 1: TIMs 0x04xxxx in key order; texture index t is the t-th): per
    texture its CLUT (and where it is), where its 8-bit pixels are, and the CLUT entries no pixel uses."""
    tims = []
    for k, o in ec.db_items(data):
        if k >> 16 != bm.KIND_PACK:
            continue
        for kk, oo in sorted(ec.db_items(data, o)):
            if kk >> 16 != 4:
                continue
            flag = u32(data, oo + 4)
            p = oo + 8
            if (flag & 3) != 1 or not flag & 8:
                tims.append(None)  # (not 8-bit with a CLUT: never baked)
                continue
            n, _, _, cw, ch = struct.unpack_from('<IHHHH', data, p)
            clut_at = p + 12
            clut = list(struct.unpack_from(f'<{cw}H', data, clut_at))
            p += n
            n, _, _, w, h = struct.unpack_from('<IHHHH', data, p)
            used = set(data[p + 12:p + 12 + w * 2 * h])
            tims.append({'clut': clut, 'clut_at': clut_at, 'pix': p + 12, 'w': w * 2, 'h': h, 'near': {},
                         'free': [i for i in range(1, cw) if i not in used], 'dirty': set()})
    return tims


def rgb(c):
    return (c & 31, (c >> 5) & 31, (c >> 10) & 31)


FAR = 6  # (squared 5-bit RGB distance past which a colour gets a free CLUT entry)


def nearest_index(tim, colour):
    """The CLUT index whose colour is nearest (the same semi-transparency bit; colour 0, transparent, only as itself);
    a colour far from all of them takes an entry no pixel uses, while there is one."""
    if colour in tim['near']:
        return tim['near'][colour]
    best, best_d = None, None
    r, g_, b = rgb(colour)
    for i, c in enumerate(tim['clut']):
        if (c == 0) != (colour == 0) or (c >> 15) != (colour >> 15):
            continue
        cr, cg, cb = rgb(c)
        d = (cr - r) ** 2 + (cg - g_) ** 2 + (cb - b) ** 2
        if best_d is None or d < best_d:
            best, best_d = i, d
    if colour and (best_d is None or best_d > FAR) and tim['free']:
        best = tim['free'].pop(0)
        tim['clut'][best] = colour
        tim['dirty'].add(best)
        tim['near'] = {}  # (earlier choices could now be nearer: start over)
    tim['near'][colour] = best
    return best


def tri_texels(uv):
    """The texels a triangle with these (u, v) corners covers (centres within half a texel of it)."""
    (u0, v0), (u1, v1), (u2, v2) = uv
    area = (u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)
    if not area:
        return []
    out = []
    edges = [((u1, v1), (u2, v2)), ((u2, v2), (u0, v0)), ((u0, v0), (u1, v1))]
    lens = [math.hypot(b[0] - a[0], b[1] - a[1]) or 1.0 for a, b in edges]
    for y in range(min(v0, v1, v2), max(v0, v1, v2) + 1):
        for x in range(min(u0, u1, u2), max(u0, u1, u2) + 1):
            px, py = x + 0.5, y + 0.5
            bary = []
            inside = True
            for (a, b), l in zip(edges, lens):
                w = ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) / area
                if w * abs(area) / l < -0.5:  # (more than half a texel outside that edge)
                    inside = False
                    break
                bary.append(w)
            if inside:
                sb = sum(bary)
                out.append((x, y, [w / sb if sb else 1 / 3 for w in bary]))  # (unclamped: an edge texel's centre
                                                                                # lies just outside)
    return out


def ray_tri(o, d, p0, p1, p2):
    """Möller-Trumbore: (t, b1, b2) where o + t d meets the triangle, or None."""
    e1, e2 = sub(p1, p0), sub(p2, p0)
    pv = cross(d, e2)
    det = dot(e1, pv)
    if abs(det) < 1e-9:
        return None
    tv = sub(o, p0)
    b1 = dot(tv, pv) / det
    if b1 < -1e-6 or b1 > 1 + 1e-6:
        return None
    qv = cross(tv, e1)
    b2 = dot(d, qv) / det
    if b2 < -1e-6 or b1 + b2 > 1 + 1e-6:
        return None
    return dot(e2, qv) / det, b1, b2


def bake(out, data, orig, g, tims, reach):
    """Redraw the texels of the triangles the collapses changed from the original surface: for each, the point of the
    simplified triangle it shows, along that triangle's normal the outermost original triangle of the same bone within
    `reach`, the nearest one (the same surface, or a removed feature in front of it), its texel there in the target
    texture's nearest colour (a colour far from all of them gets a CLUT entry no texel uses). Texels an unchanged triangle also shows stay.
    Returns the texels redrawn."""
    P0 = [v[:3] for v in orig.V]
    # the original surface: every triangle (quads as two), by bone, with what it shows
    src = []
    for t in orig.tris:
        src.append((g.tv(t), g.tuv(t), t[6]))
    for q in orig.quads:
        v4, uv4 = struct.unpack_from('<4H', q, 0), struct.unpack_from('<4H', q, 8)
        for order in ((0, 1, 2), (1, 2, 3)):
            src.append(([v4[k] for k in order], [uv4[k] for k in order], q[0x13]))
    by_bone = {}
    for vs, uvs, tex in src:
        b = orig.bone[vs[0]]
        if all(orig.bone[x] == b for x in vs):
            ps = [P0[x] for x in vs]
            c = tuple(sum(p[i] for p in ps) / 3 for i in range(3))
            r = max(math.dist(c, p) for p in ps)
            by_bone.setdefault(b, []).append((ps, uvs, tex, c, r))
    # the simplified triangles' positions: g's vertices (remapped) and quads
    P = [v[:3] for v in g.V]
    cover = {}
    for i, t in enumerate(g.tris):
        if tims[t[6]] is None:
            continue
        for x, y, _ in tri_texels([(g.uvs[k] & 0xFF, g.uvs[k] >> 8) for k in g.tuv(t)]):
            cover.setdefault((t[6], x, y), []).append(i)
    for q in g.quads:
        uv4 = struct.unpack_from('<4H', q, 8)
        for order in ((0, 1, 2), (1, 2, 3)):
            for x, y, _ in tri_texels([(g.uvs[uv4[k]] & 0xFF, g.uvs[uv4[k]] >> 8) for k in order]):
                cover.setdefault((q[0x13], x, y), []).append(-1)
    changed = g.changed
    done = 0
    for i, t in enumerate(g.tris):
        if not changed[i] or tims[t[6]] is None:
            continue
        tim = tims[t[6]]
        vs = g.tv(t)
        ps = [P[x] for x in vs]
        n = cross(sub(ps[1], ps[0]), sub(ps[2], ps[0]))
        nl = math.sqrt(dot(n, n))
        if not nl:
            continue
        n = (n[0] / nl, n[1] / nl, n[2] / nl)
        c = tuple(sum(p[k] for p in ps) / 3 for k in range(3))
        r = max(math.dist(c, p) for p in ps) + reach
        near = [s_ for s_ in by_bone.get(g.bone_of[vs[0]], []) if math.dist(s_[3], c) <= r + s_[4]]
        for x, y, bary in tri_texels([(g.uvs[k] & 0xFF, g.uvs[k] >> 8) for k in g.tuv(t)]):
            owners = cover.get((t[6], x, y), [])
            if any(j < 0 or not changed[j] for j in owners) or owners[:1] != [i]:
                continue  # (an unchanged triangle shows this texel too; or another changed one bakes it)
            o = tuple(sum(bary[k] * ps[k][a] for k in range(3)) for a in range(3))
            hit = None
            edge = min(bary) < 0  # (its centre outside: only a triangle of the same texture may show it)
            for sps, suv, stex, _, _ in near:
                if edge and stex != t[6]:
                    continue
                sn = cross(sub(sps[1], sps[0]), sub(sps[2], sps[0]))
                if dot(sn, n) <= 0:
                    continue  # (a surface facing the other way: an inside, never what the outside showed)
                rt = ray_tri(o, n, *sps)
                if rt and -reach <= rt[0] <= reach and (hit is None or abs(rt[0]) < abs(hit[0])):
                    hit = (rt[0], rt[1], rt[2], suv, stex)
            if not hit or tims[hit[4]] is None:
                continue
            _, b1, b2, suv, stex = hit
            uvc = [(g.uvs[k] & 0xFF, g.uvs[k] >> 8) for k in suv]
            # (texel centres were sampled at x + 0.5: the texel a point lies in is the floor)
            su = math.floor((1 - b1 - b2) * uvc[0][0] + b1 * uvc[1][0] + b2 * uvc[2][0])
            sv = math.floor((1 - b1 - b2) * uvc[0][1] + b1 * uvc[1][1] + b2 * uvc[2][1])
            st = tims[stex]
            su = min(max(su, 0), st['w'] - 1); sv = min(max(sv, 0), st['h'] - 1)
            colour = st['clut'][data[st['pix'] + sv * st['w'] + su]]
            idx = nearest_index(tim, colour)
            if idx is None or not (0 <= x < tim['w'] and 0 <= y < tim['h']):
                continue
            out[tim['pix'] + y * tim['w'] + x] = idx
            done += 1
    for tim in tims:
        if tim:
            for i in tim['dirty']:
                struct.pack_into('<H', out, tim['clut_at'] + 2 * i, tim['clut'][i])
            tim['dirty'] = set()
    return done


def rewrite(data, mid, ratio, report, max_error=1e9, sharp=40.0, do_bake=False):
    """The model file with its groups simplified in place (same size); per-group stats."""
    out = bytearray(data)
    pack = ec.db_find(data, bm.KIND_PACK << 16)
    h = ec.db_find(data, bm.KIND_MODEL << 16 | mid, pack)
    nbones, nmesh = data[h + 2], data[h + 3]
    mesh_o = h + u32(data, h + 0x10)
    total_packet = 0
    stats = []
    tims = load_tims(data) if do_bake else None
    for i in range(nmesh):
        m = mesh_o + i * 0x28
        g = Group(data, h, m, nbones)
        before = (len(g.quads), len(g.tris), len(g.V))
        orig = Group(data, h, m, nbones) if do_bake else None
        gone, err, n = simplify(g, ratio, max_error, sharp, do_bake)
        if g.v_o + 8 * len(g.V) + FT4 * len(g.quads) + FT3 * len(g.tris) > g.uv_o:
            # (quads that stayed two triangles take more room than the lists had: this group with its quads whole)
            g = Group(data, h, m, nbones)
            gone, err, n = simplify(g, ratio, max_error, sharp, do_bake, split_quads=False)
        baked = bake(out, data, orig, g, tims, max(2.0, 2 * err)) if do_bake and tims else 0
        stats.append((i, before, (len(g.quads), len(g.tris), len(g.V)), err, baked))
        # counts and packet size
        struct.pack_into('<H', out, m + 2, len(g.quads))
        struct.pack_into('<H', out, m + 4, len(g.tris))
        packet = len(g.quads) * 0x28 + len(g.tris) * 0x20
        struct.pack_into('<H', out, m, packet)
        total_packet += packet
        struct.pack_into(f'<{nbones}H', out, g.vc_o, *g.vcounts)
        for k, v in enumerate(g.V):
            struct.pack_into('<4h', out, g.v_o + 8 * k, *v)
        # the polygon lists right after the (fewer) vertices: the group's prims offset moves with them
        p_new = g.v_o + 8 * len(g.V)
        struct.pack_into('<I', out, m + 0x1C, p_new - h)
        p = p_new
        for q in g.quads:
            out[p:p + FT4] = q
            p += FT4
        for t in g.tris:
            out[p:p + FT3] = t
            p += FT3
        assert p <= g.uv_o
        out[p:g.uv_o] = bytes(g.uv_o - p)
    if total_packet > u16(data, h + 4):
        raise ValueError(f'model {mid}: packets {total_packet:#x} past the original {u16(data, h + 4):#x}')
    struct.pack_into('<H', out, h + 4, total_packet)
    return bytes(out), stats


def find_image(disc, root):
    for sub in (f'disc{disc}', f'us/disc{disc}', f'disks/us/disc{disc}'):
        p = os.path.join(root, sub, 'FF9.IMG')
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--img', required=True, help='a directory with discN/FF9.IMG (or us/discN, disks/us/discN)')
    ap.add_argument('--models', help='model ids, comma-separated')
    ap.add_argument('--manifest', help='config/model_patches.txt: the models, options and each disc\'s expected SHA-256')
    ap.add_argument('--update', action='store_true', help='with --manifest: record the hashes written')
    ap.add_argument('--ratio', type=float, default=0.15, help='share of each group\'s triangles to remove')
    ap.add_argument('--out', help='directory for discN.bin sector patches (under ff9ds/ or build/)')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--bake', action='store_true', help='redraw the textures where collapses moved or removed surface')
    ap.add_argument('--sharp', type=float, default=40.0, help='a vertex whose triangles bend more (degrees) stays')
    ap.add_argument('--max-error', type=float, default=1e9, help='largest collapse error (model units) taken')
    a = ap.parse_args()
    expect = {}
    if a.manifest:
        for line in open(a.manifest):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            key, _, val = line.partition(' ')
            if key == 'models':
                a.models = val.strip()
            elif key in ('ratio', 'sharp', 'max_error'):
                setattr(a, key, float(val))
            elif key == 'bake':
                a.bake = val.strip() == '1'
            elif re.fullmatch(r'disc[1-4]', key):
                expect[key] = val.strip()
    if not a.models:
        ap.error('--models or a manifest with a models line')
    models = [int(x) for x in a.models.split(',')]
    written = {}
    for disc in (1, 2, 3, 4):
        path = find_image(disc, a.img)
        if not path:
            continue
        img = ec.Img(path)
        files = {f: (s, n) for f, s, n in img.files(bm.DIR_MODELS)}
        patches = []
        for mid in models:
            if mid not in files:
                continue
            sec, n = files[mid]
            data = img.read(sec * SECTOR, n * SECTOR)
            new, stats = rewrite(data, mid, a.ratio, a.report, a.max_error, a.sharp, a.bake)
            if a.report or disc == 1:
                for i, b, c, err, baked in stats:
                    c0, c1 = 4 * b[0] + 3 * b[1], 4 * c[0] + 3 * c[1]
                    print(f'disc {disc} model {mid} group {i}: FT4 {b[0]} -> {c[0]}, FT3 {b[1]} -> {c[1]}, vertices {b[2]} -> '
                          f'{c[2]}, polygon vertices {c0} -> {c1} ({100 * (c0 - c1) / c0:.0f}%), max error {err:.1f}, texels baked {baked}')
            for k in range(n):
                if new[k * SECTOR:(k + 1) * SECTOR] != data[k * SECTOR:(k + 1) * SECTOR]:
                    patches.append((sec + k, new[k * SECTOR:(k + 1) * SECTOR]))
        if a.out and patches:
            os.makedirs(a.out, exist_ok=True)
            patches.sort()
            # "FF9P", run count, {IMG sector, sectors, file offset} per run of consecutive sectors, then the sectors
            runs = []
            for s_, d_ in patches:
                end = runs[-1][0] + len(runs[-1][1]) if runs else None
                if runs and s_ - end <= 8:  # (a short gap: the image's own sectors in between, one run fewer)
                    for k in range(end, s_):
                        runs[-1][1].append(img.read(k * SECTOR, SECTOR))
                    runs[-1][1].append(d_)
                else:
                    runs.append((s_, [d_]))
            if len(runs) > 32:
                sys.exit(f'disc {disc}: {len(runs)} runs (the port reads 32)')
            base = (8 + 12 * len(runs) + SECTOR - 1) // SECTOR * SECTOR
            body = bytearray(PATCH_MAGIC + struct.pack('<I', len(runs)))
            off = base
            for s_, ds in runs:
                body += struct.pack('<III', s_, len(ds), off)
                off += len(ds) * SECTOR
            body += bytes(base - len(body))
            for _, ds in runs:
                for d_ in ds:
                    body += d_
            with open(os.path.join(a.out, f'disc{disc}.bin'), 'wb') as f:
                f.write(body)
            written[f'disc{disc}'] = hashlib.sha256(body).hexdigest()
            print(f'disc {disc}: {len(patches)} sectors patched in {len(runs)} runs')
    if a.manifest and a.out:
        check_manifest(a.manifest, written, expect, a.update)


def check_manifest(path, written, expect, update):
    if update:
        lines = [l for l in open(path) if not re.match(r'\s*disc[1-4]\s', l)]
        lines += [f'{k} {v}\n' for k, v in sorted(written.items())]
        open(path, 'w').writelines(lines)
        print(f'{path}: hashes recorded')
        return
    bad = [k for k in sorted(set(written) | set(expect)) if written.get(k) != expect.get(k)]
    if bad:
        sys.exit(f'{path}: {", ".join(bad)} differ from the recorded SHA-256 (a different disc image, or the tool '
                 f'changed: rerun with --update once the new output is checked)')


if __name__ == '__main__':
    main()
