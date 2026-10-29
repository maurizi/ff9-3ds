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


def simplify(g, ratio, max_error=1e9, sharp=40.0):
    """Remove about `ratio` of the group's triangles. Returns (removed triangles, max error, collapses)."""
    nv = len(g.V)
    P = [v[:3] for v in g.V]
    alive = [True] * len(g.tris)
    vt = [set() for _ in range(nv)]
    for i, t in enumerate(g.tris):
        for v in g.tv(t):
            vt[v].add(i)
    in_quad = set()
    for q in g.quads:
        in_quad.update(struct.unpack_from('<4H', q, 0))
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

    def candidate(u):
        """The best collapse of u: (cost, v, uv index for u's corners) or None."""
        if removed_v[u] or u in in_quad or not vt[u]:
            return None
        fan = [g.tris[i] for i in vt[u]]
        a0 = g.attr(fan[0])
        uv0 = g.uvs[corner_uv(fan[0], u)]
        edges = {}
        for t in fan:
            if g.attr(t) != a0 or g.uvs[corner_uv(t, u)] != uv0:
                return None
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
        for v in edges:
            # v's uv where u's triangles touch it must be one value (the corners moving onto v take it)
            uvs_v = {g.uvs[corner_uv(t, v)] for t in fan if v in g.tv(t)}
            if len(uvs_v) != 1:
                continue
            uvi = next(corner_uv(t, v) for t in fan if v in g.tv(t))
            ok = True
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
            struct.pack_into('<H', t, 2 * k, v)
            struct.pack_into('<H', t, 0xC + 2 * k, uvi)
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
    g.tris = [t for t, a in zip(g.tris, alive) if a]
    # the vertices still used, in bone order (vertexCounts per bone), and the polygons' indices remapped
    used = set(x for t in g.tris for x in g.tv(t)) | in_quad
    keep = [k for k in range(nv) if k in used]
    remap = {k: i for i, k in enumerate(keep)}
    g.vcounts = [sum(1 for k in keep if g.bone[k] == b) for b in range(len(g.vcounts))]
    g.V = [g.V[k] for k in keep]
    for t in g.tris:
        struct.pack_into('<3H', t, 0, *[remap[x] for x in g.tv(t)])
    g.quads = [bytearray(q) for q in g.quads]
    for q in g.quads:
        struct.pack_into('<4H', q, 0, *[remap[x] for x in struct.unpack_from('<4H', q, 0)])
    return gone, max_err, collapses


def rewrite(data, mid, ratio, report, max_error=1e9, sharp=40.0):
    """The model file with its groups simplified in place (same size); per-group stats."""
    out = bytearray(data)
    pack = ec.db_find(data, bm.KIND_PACK << 16)
    h = ec.db_find(data, bm.KIND_MODEL << 16 | mid, pack)
    nbones, nmesh = data[h + 2], data[h + 3]
    mesh_o = h + u32(data, h + 0x10)
    total_packet = 0
    stats = []
    for i in range(nmesh):
        m = mesh_o + i * 0x28
        g = Group(data, h, m, nbones)
        before = (len(g.quads), len(g.tris), len(g.V))
        gone, err, n = simplify(g, ratio, max_error, sharp)
        stats.append((i, before, (len(g.quads), len(g.tris), len(g.V)), err))
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
            new, stats = rewrite(data, mid, a.ratio, a.report, a.max_error, a.sharp)
            if a.report or disc == 1:
                for i, b, c, err in stats:
                    print(f'disc {disc} model {mid} group {i}: FT4 {b[0]}, FT3 {b[1]} -> {c[1]}, vertices {b[2]} -> {c[2]}, '
                          f'max error {err:.1f}')
            for k in range(n):
                if new[k * SECTOR:(k + 1) * SECTOR] != data[k * SECTOR:(k + 1) * SECTOR]:
                    patches.append((sec + k, new[k * SECTOR:(k + 1) * SECTOR]))
        if a.out and patches:
            os.makedirs(a.out, exist_ok=True)
            patches.sort()
            # "FF9P", run count, {IMG sector, sectors, file offset} per run of consecutive sectors, then the sectors
            runs = []
            for s_, d_ in patches:
                if runs and runs[-1][0] + len(runs[-1][1]) == s_:
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
