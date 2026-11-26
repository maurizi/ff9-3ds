#!/usr/bin/env python3
"""The HOME Menu banner and icon, made from the game's own data on the user's discs (the builder runs it; nothing
of it is in the repository or in the program it ships).

The banner is a 3D model the HOME Menu draws with its stereo camera, on nothing (the HOME Menu's background shows
through): the eight heroes' field models (FF9.IMG dir 10, docs/field-models.md in the decomp) in a row, as on the
game's key art, each playing one of the game's own looping motions (its standing idle or a held pose near the art's:
tools/battle_pose.py), Freya her battle idle with a spear (a battle weapon model, dir 8), and behind the title
(below). The banner's sound is the chocobo's call (sound(): tools/akao_se.py). The icon is the
crystal of the gold-crystal logo on the opening movie's title card, cut out of its scene (logo(); the frames decoded
here as the port's player does, movie_frame()). Plain Python, as the builder runs it in a browser too.

The program's own banner, made when it is built (`--title`, with Pillow: no game data in it), is the title alone:
"FINAL FANTASY IX" in a serif like the logo's, as bannertool's flat banner model (one textured quad) with its quad
replaced by the title's and its texture by a 512x256 atlas. build() takes that banner and the program's SMDH from
the CIA, adds the heroes to the model, and returns the new ones.

    tools/banner3d.py --title FLAT.bnr OUT.bnr   (the build's)
    tools/banner3d.py IN.cia OUT.cia   (a developer's check, from the local discs: decomp's FF9.IMG, disks/us/raw)
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (os.path.join(HERE, '..', 'decomp', 'tools'), os.path.join(HERE, '..', 'vendor', 'decomp', 'tools')):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

SECTOR = 2048
DIR_COMMON, DIR_FIELDS, DIR_WEAPON_MODELS, DIR_CHAR_MODELS = 1, 4, 8, 10
KIND_PACK, KIND_MODEL = 0x1B, 0x02

# The heroes' field models (decomp/docs/field-models.md, "The party's field models") in a row as on the key art,
# left to right; x, z, feet in the banner model's units (the banner is 26 wide and 13 high, y from -7.5 to 5.5; + z is
# out of the screen; the HOME Menu draws anticlockwise faces: seen with a calibration banner in its menu in Azahar):
# the ones the art shows in front of a neighbour stand nearer. Each plays one of the game's own looping motions, whole,
# at the game's rate: (source, motion), the source None for a motion in the model's own file (its standing idle: the
# one of its five that stays in place), (disc, field) for a loop a field holds a character in (found among the
# motions of the fields that load the model, for its likeness to the art's pose), 'battle' for a battle model (the
# same directory and format) in a motion of its own file, stood where its root bone is, with its turn in degrees.
HEROES = [
    # model, name,      x,    z,   feet,  motion
    (290, 'Freya', -12.2, -0.6, -7.0, ('battle', 0x31D19, 60)),  # her battle model's idle (g_btlPartyMotionIds[12][0])
    (443, 'Eiko', -9.2, 0.6, -7.2, ((2, 1600), 0x30EC4)),     # hands together
    (8, 'Vivi', -7.0, -0.7, -7.1, (None, 0x30094)),
    (5489, 'Steiner', -3.5, 1.3, -7.3, (None, 0x307D1)),
    (185, 'Garnet', -0.5, -0.7, -7.1, ((1, 312), 0x32062)),   # hands clasped
    (98, 'Zidane', 2.6, 0.8, -7.3, ((3, 2802), 0x32165)),     # a hand on his hip
    (273, 'Quina', 7.1, -0.4, -7.1, (None, 0x30C9C)),
    (509, 'Amarant', 12.3, 0.5, -7.0, (None, 0x32073)),
]
# Freya's spear, as a battle shows it: a weapon model (dir 8) hanging from her battle model's weapon bone
# (btl_initWeapon: Character.weaponBone 6 for CHR_KIND_FREYA; the weapon's bone 0 is that bone)
SPEAR = (290, 0x1D0, 6)
# A motion's frame lasts two of the HOME Menu's 60ths in a field (the field runs at 30 a second), four in a battle
# (15); a loop is stretched to the nearest whole number of turns in the menu's 600-frame animation (it starts over
# there). MOTION_BONES: the bones animated in all: the HOME Menu takes 64 animated bones (it crashed with more,
# whatever their keys: tools/banner_model.py), one of them the stage; they are the ones whose motion shows most
# (Figure.picks), each hero having at least MOTION_LEAST
MOTION_TICKS = {None: 2, 'battle': 4}
MOTION_LOOP = 600
MOTION_BONES, MOTION_LEAST = 63, 4
IDENTITY3 = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
TALLEST = 8.6      # the tallest hero's height (model units)
TURN = 0.03        # radians of turn towards the middle per unit of x
TITLE_GROW, TITLE_Y = 1.3, 6.6  # the program's title in the full banner: larger (about its middle), its top here
# The icon: the crystal of the gold-crystal "FINAL FANTASY IX" of the opening movie's title card (disc 1 SEQ01/FMV002.STR, frames
# 805-880, over Alexandria at dusk), cut out of the scene: it holds still while the camera drifts and, through the
# fade at the end, fades at its own rate; so with each frame scaled back by the scene's fade (measured on what held
# still before it), what still changes is the logo (its gold, its whites), and what is dark is its lettering. In the
# icon is the part of it inside the crystal's outline.
LOGO_MOVIE = ('SEQ01', 'FMV002.STR')
LOGO_FRAMES = range(807, 881, 2)     # (movie frames, from 1)
LOGO_STILL = 858                     # the fade starts after it
LOGO_REF = range(841, 858, 2)        # averaged for the logo's colours
LOGO_BOX = (15, 8, 300, 106)         # where the logo is in the frame (x0, y0, x1, y1)
LOGO_UNDERLINE = 78                  # its underline's row (in the box); below it, only the crystal's tip
# the crystal's outline (box coordinates)
LOGO_CRYSTAL = [(102, 9), (90, 19), (86, 28), (85, 40), (92, 52), (96, 62), (97, 78), (103, 89), (110, 78),
                (112, 62), (118, 52), (125, 44), (120, 30), (117, 22), (108, 12)]
# The banner's title: "FINAL FANTASY IX" in a condensed bold serif like the logo's lettering (Nimbus Roman Bold at
# 60% width: black, a white edge for dark themes, the IX gold), drawn when the program is built (title_banner: it is
# no game data, so the program's own banner has it), on nothing, behind the heroes the builder adds
TITLE_FONT = '/usr/share/fonts/opentype/urw-base35/NimbusRoman-Bold.otf'  # (fonts-urw-base35, in the build container)
TITLE_WORDS = ('FINAL', 'FANTASY', 'IX')
TITLE_SQUEEZE = 0.6
TITLE_GOLD = ((150, 104, 36), (238, 206, 120), (128, 84, 24))  # the IX: top, middle, bottom
TITLE_WIDTH = 432  # texels (the atlas's left 320 columns hold it in two pieces: FINAL; FANTASY IX)
TITLE_Z = -1.4
TITLE_SCALE = 0.5 * 26 / 256  # model units per texel (two texels a banner pixel)
TITLE_TOP = 3      # banner pixels from the top
ATLAS_W, ATLAS_H = 512, 256
HERO_CELLS_X = 320  # the models' textures are right of it (three 64-texel columns), the title left
CGFX_MAX = 0x80000  # the HOME Menu's limit on a banner's model (decompressed)


# ---- images: (w, h, bytearray RGBA) ------------------------------------------------------------------------------

def ps1_rgba(c):
    """a PS1 15-bit colour; 0 is transparent (the GPU skips it)"""
    return ((c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3, 0 if c == 0 else 255)


def tim(data, off):
    """a TIM at data[off] -> (w, h, RGBA, vram rect)"""
    flag, = struct.unpack_from('<I', data, off + 4)
    o, mode, clut = off + 8, flag & 3, None
    if flag & 8:
        bl, _, _, w, h = struct.unpack_from('<I4H', data, o)
        clut = [ps1_rgba(c) for c in struct.unpack_from(f'<{w * h}H', data, o + 12)]
        o += bl
    bl, x, y, w, h = struct.unpack_from('<I4H', data, o)
    raw = data[o + 12:o + 12 + 2 * w * h]
    out = bytearray()
    if mode == 0:
        for b in raw:
            out += bytes(clut[b & 15]) + bytes(clut[b >> 4])
        pw = 4 * w
    elif mode == 1:
        for b in raw:
            out += bytes(clut[b])
        pw = 2 * w
    else:
        for c in struct.unpack(f'<{w * h}H', raw):
            out += bytes(ps1_rgba(c))
        pw = w
    return pw, h, out, (x, y, w, h)


def half(img):
    """2x2 boxes; a texel is transparent when most of its four are"""
    w, h, px = img
    out = bytearray()
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            q = [px[4 * (yy * w + xx):4 * (yy * w + xx) + 4] for yy in (y, y + 1) for xx in (x, x + 1)]
            solid = [c for c in q if c[3]]
            if len(solid) < 2:
                out += b'\0\0\0\0'
            else:
                out += bytes(sum(c[k] for c in solid) // len(solid) for k in range(3)) + b'\xff'
    return w // 2, h // 2, out


def paste(dst, src, at):
    dw, _, dp = dst
    sw, sh, sp = src
    x0, y0 = at
    for y in range(sh):
        dp[4 * ((y0 + y) * dw + x0):4 * ((y0 + y) * dw + x0 + sw)] = sp[4 * y * sw:4 * (y + 1) * sw]


# ---- the game's data ---------------------------------------------------------------------------------------------

def _file(img, dirx, want, low16=False):
    """a file of FF9.IMG by id (by the id's low 16 bits for dir 10, and with low16)"""
    for fid, sec, n in img.files(dirx):
        if fid == want if not (low16 or dirx == DIR_CHAR_MODELS) else fid & 0xFFFF == want:
            return img.read(sec * SECTOR, n * SECTOR)
    raise KeyError(f'FF9.IMG has no file {dirx}/{want:#x}')


def model_polys(data, m):
    """the FT4/FT3 polygons of a model (battle format: field, battle or weapon) as triangles
    [((group, vertex) * 3, (u, v) * 3, texture index)], anticlockwise on screen where the PS1 draws them (its front
    faces are clockwise: NCLIP > 0)"""
    h = m.header
    groups, = struct.unpack_from('<I', data, h + 0x10)
    out = []
    for g in range(data[h + 3]):
        gb = h + groups + 0x28 * g
        counts = struct.unpack_from('<8H', data, gb + 2)
        prims, uvs = (h + v for v in struct.unpack_from('<2I', data, gb + 0x1C))

        def uv(i):
            return data[uvs + 2 * i], data[uvs + 2 * i + 1]
        o = prims
        for _ in range(counts[0]):  # FT4 (WmSrcFT4): the GPU's triangles (0, 1, 2) and (1, 2, 3)
            vi, ui, tex = struct.unpack_from('<4H', data, o), struct.unpack_from('<4H', data, o + 8), data[o + 0x13]
            for a, b, c in ((0, 2, 1), (1, 2, 3)):
                out.append((((g, vi[a]), (g, vi[b]), (g, vi[c])), (uv(ui[a]), uv(ui[b]), uv(ui[c])), tex))
            o += 0x18
        for _ in range(counts[1]):  # FT3 (WmSrcFT3)
            vi, tex, ui = struct.unpack_from('<3H', data, o), data[o + 6], struct.unpack_from('<3H', data, o + 0xC)
            out.append((((g, vi[0]), (g, vi[2]), (g, vi[1])), (uv(ui[0]), uv(ui[2]), uv(ui[1])), tex))
            o += 0x14
        if any(counts[2:]):
            raise ValueError(f'model {m.id}: polygon kinds other than FT4/FT3')
    return out


def _textures(data):
    """[(w, h, RGBA, VRAM rectangle)] of a model file's TIMs"""
    from encounter_catalog import db_find, db_items
    return [tim(data, off) for _, off in db_items(data, db_find(data, KIND_PACK << 16 | 1))]


class Figure:
    """A hero of HEROES: its model (battle_pose.Model), the frames of its motion (a loop), their bone matrices, its
    polygons and textures; with its weapon's, for SPEAR's"""

    def __init__(self, imgs, mid, pose):
        import battle_pose
        from encounter_catalog import db_find
        img = imgs[min(imgs)]
        self.data = _file(img, DIR_CHAR_MODELS, mid)
        pack = db_find(self.data, KIND_PACK << 16)
        sources, base = [], None
        source, key = pose[:2]
        self.turn = math.radians(pose[2]) if source == 'battle' else None
        if source not in (None, 'battle'):
            fdata = _file(imgs[source[0]], DIR_FIELDS, source[1], low16=True)
            sources = [(fdata, db_find(fdata, KIND_PACK << 16 | 1))]
        self.m = m = battle_pose.Model(self.data, mid, pack=pack, motion_sources=sources)
        if source == 'battle':  # (a battle motion's root is away from the origin: back over it)
            root = m.bone_matrices(key, 0)[0][1]
            base = (battle_pose.IDENTITY[0], (-root[0], 0, -root[2]))
        count = m.frame_count(key)
        self.frames = [m.bone_matrices(key, i, base) for i in range(count)]
        natural = count * MOTION_TICKS[source if source == 'battle' else None]
        self.step = MOTION_LOOP / max(1, round(MOTION_LOOP / natural)) / count  # (the menu's frames a motion frame)
        self.polys = model_polys(self.data, m)
        self.textures = _textures(self.data)
        self.weapon = None
        if SPEAR[0] == mid:
            wdata = _file(img, DIR_WEAPON_MODELS, SPEAR[1], low16=True)
            wm = battle_pose.Model(wdata, SPEAR[1], pack=db_find(wdata, KIND_PACK << 16))
            along = [0]  # (no motion: each of its bones its length along its parent's z, from the holder's bone)
            for length, _, parent in wm.bones[1:]:
                along.append(along[parent] + length)
            wtex = _textures(wdata)
            # (a weapon's uvs are where its texture is in a 128-row slot of its page: the TIM's place in VRAM, mod
            # 128 rows; checked on 0x1D0's, v 80-127 for a 48-row TIM at y 464)
            polys = [(vids, [(u - wtex[t][3][0] % 64 * 2, v - wtex[t][3][1] % 128) for u, v in uvs], t)
                     for vids, uvs, t in model_polys(wdata, wm)]
            self.weapon = (SPEAR[2], [[(x, y, z + along[bone]) for bone, (x, y, z) in g] for g in wm.meshes],
                           polys, wtex)

    def local(self, k, j):
        """(rotation, translation) of bone j in its parent, in frame k of those played"""
        B = self.frames[k]
        R = [[B[j][0][r][c] / 4096 for c in range(3)] for r in range(3)]
        if j == 0:
            return R, tuple(float(v) for v in B[0][1])
        P = B[self.m.bones[j][2]][0]
        return ([[sum(P[i][r] * R[i][c] for i in range(3)) / 4096 for c in range(3)] for r in range(3)],
                (0.0, 0.0, float(self.m.bones[j][0])))

    def picks(self, frames=10, points=3):
        """[(gain, bone)]: the bones to animate, the most telling first; gain: the mean square distance (model
        units), over sampled vertices and frames, from where the whole motion puts them, that the bone takes off
        when it moves with the ones before it (the others held as in the first frame)"""
        nb, count = len(self.m.bones), len(self.frames)
        ks = sorted({i * count // frames for i in range(frames)}) if count > frames else list(range(count))
        loc = [[self.local(k, j) for j in range(nb)] for k in ks]
        held = [self.local(0, j) for j in range(nb)]
        parent = [self.m.bones[j][2] for j in range(nb)]
        pts = [[] for _ in range(nb)]
        for g in self.m.meshes:
            per = {}
            for bone, v in g:
                per.setdefault(bone, []).append(v)
            for bone, vs in per.items():
                pts[bone] += vs[::max(1, len(vs) // points)][:points]
        if self.weapon:
            ws = [v for g in self.weapon[1] for v in g]
            pts[self.weapon[0]] += [min(ws, key=lambda v: v[2]), max(ws, key=lambda v: v[2])]
        total = sum(map(len, pts))

        def placed(k, moving):
            W, out = [], []
            for j in range(nb):
                R, T = loc[k][j] if moving is None or j in moving else held[j]
                if j:
                    PR, PT = W[parent[j]]
                    T = [PT[i] + PR[i][0] * T[0] + PR[i][1] * T[1] + PR[i][2] * T[2] for i in range(3)]
                    R = [[PR[i][0] * R[0][c] + PR[i][1] * R[1][c] + PR[i][2] * R[2][c] for c in range(3)]
                         for i in range(3)]
                W.append((R, T))
                for x, y, z in pts[j]:
                    out += [T[i] + R[i][0] * x + R[i][1] * y + R[i][2] * z for i in range(3)]
            return out
        true = [placed(k, None) for k in range(len(ks))]

        def off(moving):
            return sum((a - b) ** 2 for k in range(len(ks)) for a, b in zip(placed(k, moving), true[k])) / (
                total * len(ks))
        turning = {j for j in range(nb) if any(abs(loc[k][j][0][r][c] - held[j][0][r][c]) > 0.004
                                               for k in range(len(ks)) for r in range(3) for c in range(3))
                   or any(abs(loc[k][j][1][c] - held[j][1][c]) > 0.5 for k in range(len(ks)) for c in range(3))}
        # from all of them moving, hold one at a time: the one missed least (a bone often undoes its parent's
        # turn, as legs under swaying hips: adding bones one at a time would never take either)
        out, err = [], off(turning)
        while turning:
            now, j = min((off(turning - {j}), j) for j in turning)
            out.append((max(now - err, 0.0), j))
            turning.discard(j)
            err = now
        return out[::-1]

    def height(self):
        import battle_pose
        return -min(battle_pose.mvmva_tr(*self.frames[0][bone], v)[1] for g in self.m.meshes for bone, v in g)


# ---- the game's addresses, by the decomp's names for them: in the release's symbol files ---------------------------

VERSION = 'us'  # the release the discs are (the decomp's VERSION: build(version=...))
_SYMBOLS = {}


def _address(name, binary='main'):
    """a symbol's address in the release: config/symbols[.<binary>].<version>.txt of the decomp (beside this file in a
    checkout, in the builder's vendor folder)"""
    key = (VERSION, binary)
    if key not in _SYMBOLS:
        table = {}
        for base in (os.path.join(HERE, '..', 'decomp', 'config'), os.path.join(HERE, '..', 'vendor', 'decomp', 'config')):
            stem = 'symbols' if binary == 'main' else f'symbols.{binary}'
            path = os.path.join(base, f'{stem}.{VERSION}.txt')
            if os.path.isfile(path):
                for line in open(path):
                    f = line.split('//')[0].split('=')
                    if len(f) == 2 and f[1].strip().rstrip(';').startswith('0x'):
                        table[f[0].strip()] = int(f[1].strip().rstrip(';'), 16)
                break
        _SYMBOLS[key] = table
    if name not in _SYMBOLS[key]:
        raise KeyError(f'{name} is not in the {VERSION} symbols of {binary}')
    return _SYMBOLS[key][name]


# ---- one frame of a movie (the port's decoder, port/psx/movie_stream.c, movie_vlc.c, mdec_decode.c, in Python) ---

OVL_MOVIE, OVL_MOVIE_VRAM = 0x64, 0x80198700
# g_mbgVlcPairs, the quantization and scale tables: (name, size) in the movie overlay (its address: _address)
MOVIE_TABLES = (('g_mbgVlcPairs', 216), ('D_8019DEC8', 128), ('D_8019DF4C', 128))
CHUNK = 0x8F4  # video bytes a sector (Mode 2 Form 2: 2324 less the 32-byte header)
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
          60, 61, 54, 47, 55, 62, 63]


def movie_tables(img):
    """(run/level pairs, quantization, scale) from the movie player overlay"""
    ovl = _file(img, 0, OVL_MOVIE)
    out = []
    for name, n in MOVIE_TABLES:
        a = _address(name, 'ovl_64') - OVL_MOVIE_VRAM
        out.append(ovl[a:a + n])
    return out


def movie_bitstream(sectors, frame):
    """the BS stream of movie frame `frame` (from 1) from its raw 2352-byte sectors: ten a frame, two of sound and
    eight of video, the video's chunks in reverse order (movie_stream.c)"""
    video = {}
    for s in sectors:
        d = s[24:]
        if struct.unpack_from('<H', d, 0)[0] != 0x160 or struct.unpack_from('<I', d, 8)[0] != frame:
            continue
        index, = struct.unpack_from('<H', d, 4)
        if index >= 2:
            words, = struct.unpack_from('<I', d, 12)
            video[index] = (d[32:32 + CHUNK], words * 4)
    if sorted(video) != list(range(2, 10)):
        raise ValueError(f'movie frame {frame}: incomplete')
    data = b''.join(video[i][0] for i in range(9, 1, -1))
    return data[:video[2][1]]


def movie_vlc(bs, pairs):
    """BS v2 to run-level halfwords (movie_vlc.c, ovl_64 mbg_vlcDecode)"""
    count, magic, qscale, version = struct.unpack_from('<4H', bs, 0)
    if magic != 0x3800 or version != 2:
        raise ValueError('movie frame: not BS v2')
    d = bs[8:]
    nbits = len(d) * 8
    hw = struct.unpack(f'<{len(d) // 2}H', d[:len(d) // 2 * 2]) + (0, 0, 0)
    out = [count, magic]
    pos = 0

    def peek():
        i, sh = pos >> 4, pos & 15
        w = hw[i] << 32 | hw[i + 1] << 16 | hw[i + 2]
        return (w << sh >> 16) & 0xFFFFFFFF
    while True:
        dc = peek() >> 22
        pos += 10
        if dc == 0x1FF or pos > nbits:
            break
        out.append(qscale << 10 | dc)
        while True:
            v = peek()
            table, eob = False, False
            if v & 0x80000000:
                if not v & 0x40000000:
                    value, bits, eob = 0xFE00, 2, True
                else:
                    value, bits = (0x3FF if v & 0x20000000 else 1), 3
            elif v & 0x40000000:
                if v & 0x20000000:
                    value, bits = (0x7FF if v & 0x10000000 else 0x401), 4
                else:
                    run = 2 if v & 0x10000000 else 0
                    level = 1 if run else 2
                    value, bits = run << 10 | ((1024 - level) if v & 0x08000000 else level), 5
            elif v & 0x20000000:
                table = True
                if v & 0x18000000:
                    offset, bits = 0x19C + (v >> 25 & 14), 6
                else:
                    offset, bits = 0x180 + (v >> 22 & 30), 9
            elif v >> 26 == 1:
                value, bits = v >> 10 & 0xFFFF, 22
            else:
                zeros = 32 - v.bit_length()
                if zeros < 3 or zeros == 5 or zeros > 11:
                    raise ValueError('movie frame: bad code')
                table = True
                sh = v << zeros & 0xFFFFFFFF
                if zeros < 5:
                    offset, bits = (zeros - 3) * 16 + (sh >> 27 & 14), zeros + 4
                elif zeros == 6:
                    offset, bits = 0x20 + (sh >> 26 & 30), 11
                else:
                    offset, bits = (zeros - 6) * 64 + (sh >> 25 & 62), zeros + 6
            pos += bits
            if table:
                run, level = pairs[offset // 4 * 2], pairs[offset // 4 * 2 + 1]
                value = run << 10 | (((1024 - level) & 1023) if offset & 2 else level)
            out.append(value)
            if eob:
                break
    out += [0xFE00] * (count * 2 + 2 - len(out))
    return out


def _s10(x):
    x &= 1023
    return x - 1024 if x & 512 else x


def _clamp9(x):
    x &= 511
    x = x - 512 if x >= 256 else x
    return -128 if x < -128 else 127 if x > 127 else x


def movie_frame(sectors, frame, tables, rows=(0, 224)):
    """movie frame `frame` as (320, 224, RGBA) (mdec_decode.c: the integer IDCT with the game's scale matrix); only
    the macroblocks of `rows` are decoded (the others are black)"""
    pairs, quant, scale_b = tables
    S = [s - 65536 if s & 32768 else s for s in struct.unpack('<64H', scale_b)]
    rl = movie_vlc(movie_bitstream(sectors, frame), pairs)
    command = rl[0] | rl[1] << 16
    flip = 128 if command & 0x04000000 else 0
    pos = 2
    W, H = 320, 224
    px = bytearray(4 * W * H)

    def block(qt):
        nonlocal pos
        c = [0] * 64
        first = rl[pos]
        pos += 1
        while first == 0xFE00:
            first = rl[pos]
            pos += 1
        q, level = first >> 10, _s10(first)
        dc = level * qt[0] * 16 + (8 if level < 0 else -8 if level > 0 else 0) if q else level * 32
        c[0] = max(-16384, min(16383, dc))
        rank = 0
        while rank < 63:
            code = rl[pos]
            pos += 1
            rank += (code >> 10) + 1
            if rank > 63:
                break
            level = _s10(code)
            qq = q * qt[rank]
            v = ((level * qq) >> 3) * 16 + (8 if level < 0 else -8 if level > 0 else 0) if qq else level * 32
            c[ZIGZAG[rank]] = max(-16384, min(16383, v))
        cols = [x for x in range(8) if any(c[v * 8 + x] for v in range(8))]
        rows = [v for v in range(8) if any(c[v * 8:v * 8 + 8])]
        t = [[(131072 + sum(c[v * 8 + x] * S[v * 8 + y] for v in rows)) >> 18 if x in cols else 0 for x in range(8)]
             for y in range(8)]
        return [_clamp9((131072 + sum(t[y][u] * S[u * 8 + x] for u in cols)) >> 18) for y in range(8) for x in range(8)]
    def skip():  # (a block's codes, not decoded)
        nonlocal pos
        while rl[pos] == 0xFE00:
            pos += 1
        pos += 1
        rank = 0
        while rank < 63:
            rank += (rl[pos] >> 10) + 1
            pos += 1
    for mx in range(0, W, 16):
        for my in range(0, H, 16):
            if my + 16 <= rows[0] or my >= rows[1]:
                for _ in range(6):
                    skip()
                continue
            cr, cb = block(quant[64:]), block(quant[64:])
            ys = [block(quant[:64]) for _ in range(4)]
            for yy in range(16):
                for xx in range(16):
                    ci = (yy >> 1) * 8 + (xx >> 1)
                    r_, b_ = cr[ci], cb[ci]
                    dr = (359 * r_ + 128) >> 8
                    db = (454 * b_ + 128) >> 8
                    dg = (((-88 * b_) >> 5) * 32 + ((-183 * r_) >> 3) * 8 + 128) >> 8
                    lum = ys[(yy >> 3) * 2 + (xx >> 3)][(yy & 7) * 8 + (xx & 7)]
                    o = 4 * ((my + yy) * W + mx + xx)
                    px[o:o + 4] = bytes(((_clamp9(lum + d) + 128) ^ flip) for d in (dr, dg, db)) + b'\xff'
    return W, H, px


# ---- the scene ---------------------------------------------------------------------------------------------------

def crop(img, box):
    w, _, px = img
    x0, y0, x1, y1 = box
    return x1 - x0, y1 - y0, bytearray(b''.join(px[4 * (y * w + x0):4 * (y * w + x1)] for y in range(y0, y1)))


def bleed_edges(img, steps=2):
    """the colours of the transparent texels next to opaque ones, from those (bilinear filtering at the edges)"""
    w, h, px = img
    px = bytearray(px)
    for _ in range(steps):
        src = bytes(px)
        for y in range(h):
            for x in range(w):
                o = 4 * (y * w + x)
                if src[o + 3]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    X, Y = x + dx, y + dy
                    if 0 <= X < w and 0 <= Y < h and src[4 * (Y * w + X) + 3]:
                        px[o:o + 3] = src[4 * (Y * w + X):4 * (Y * w + X) + 3]
                        px[o + 3] = 1  # (a neighbour for the next step; transparent again below)
                        break
    for o in range(3, len(px), 4):
        if px[o] == 1:
            px[o] = 0
    return w, h, px


def _inside(poly, x, y):
    c = False
    for (x0, y0), (x1, y1) in zip(poly, poly[1:] + poly[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            c = not c
    return c


def _morph(m, w, h, offsets, dilate):
    out = [False] * (w * h)
    for y in range(h):
        for x in range(w):
            vals = [m[(y + dy) * w + x + dx] if 0 <= x + dx < w and 0 <= y + dy < h else False for dx, dy in offsets]
            out[y * w + x] = any(vals) if dilate else all(vals)
    return out


def _components(m, w, h):
    """[[index]] of the 4-connected pieces of a mask"""
    seen, out = [False] * (w * h), []
    for i in range(w * h):
        if m[i] and not seen[i]:
            seen[i] = True
            stack, piece = [i], []
            while stack:
                j = stack.pop()
                piece.append(j)
                x, y = j % w, j // w
                for X, Y in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    k = Y * w + X
                    if 0 <= X < w and 0 <= Y < h and m[k] and not seen[k]:
                        seen[k] = True
                        stack.append(k)
            out.append(piece)
    return out


def logo(sectors, tables):
    """(letters, crystal): the opening movie's logo as two RGBA images of LOGO_BOX's size"""
    x0, y0, x1, y1 = LOGO_BOX
    w, h = x1 - x0, y1 - y0
    frames = {}
    for f in LOGO_FRAMES:
        fw, _, px = movie_frame(sectors, f, tables, rows=(y0, y1))
        frames[f] = [tuple(px[4 * ((y0 + y) * fw + x0 + x):4 * ((y0 + y) * fw + x0 + x) + 3]) for y in range(h)
                     for x in range(w)]
    n = w * h
    ref = [tuple(sum(frames[f][i][c] for f in LOGO_REF) / len(LOGO_REF) for c in range(3)) for i in range(n)]

    def std(vals):
        m = sum(vals) / len(vals)
        return (sum((v - m) ** 2 for v in vals) / len(vals)) ** 0.5
    still = [f for f in LOGO_FRAMES if f <= LOGO_STILL]
    seed = [i for i in range(n) if max(ref[i]) > 150 and
            sum(std([frames[f][i][c] for f in still]) for c in range(3)) / 3 < 8]
    def median(vals):
        vals = sorted(vals)
        return vals[len(vals) // 2]
    gains = {f: median(sum(frames[f][i]) / max(sum(ref[i]), 1) for i in seed) for f in LOGO_FRAMES}
    lit = [f for f in LOGO_FRAMES if gains[f] > 0.35]
    vary = [sum(std([frames[f][i][c] / gains[f] for f in lit]) for c in range(3)) / 3 > 16 for i in range(n)]
    dark = [max(ref[i]) < 95 for i in range(n)]
    crystal = [_inside(LOGO_CRYSTAL, i % w + 0.5, i // w + 0.5) for i in range(n)]
    top = next(y for y in range(h) if sum(dark[y * w:(y + 1) * w]) >= 20)  # the letters' tops
    m = [(vary[i] or dark[i]) and (crystal[i] or top <= i // w <= LOGO_UNDERLINE + 1) for i in range(n)]
    sq2, sq3 = [(0, 0), (1, 0), (0, 1), (1, 1)], [(dx, dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
    m = _morph(_morph(m, w, h, sq2, False), w, h, [(-dx, -dy) for dx, dy in sq2], True)  # (opening)
    keep = [False] * n
    for piece in _components(m, w, h):
        if len(piece) > 60:
            for i in piece:
                keep[i] = True
    m = _morph(_morph(keep, w, h, sq3, True), w, h, sq3, False)  # (closing)
    outside = [False] * n  # (holes filled: what the border can't reach through the background)
    stack = [i for i in range(n) if not m[i] and (i % w in (0, w - 1) or i // w in (0, h - 1))]
    for i in stack:
        outside[i] = True
    while stack:
        j = stack.pop()
        x, y = j % w, j // w
        for X, Y in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            k = Y * w + X
            if 0 <= X < w and 0 <= Y < h and not m[k] and not outside[k]:
                outside[k] = True
                stack.append(k)
    m = [not o for o in outside]
    alpha = []  # (soft edges: 1-6-1 each way)
    for y in range(h):
        for x in range(w):
            acc = 0
            for dy, wy in ((-1, 1), (0, 6), (1, 1)):
                for dx, wx in ((-1, 1), (0, 6), (1, 1)):
                    X, Y = min(max(x + dx, 0), w - 1), min(max(y + dy, 0), h - 1)
                    acc += wx * wy * m[Y * w + X]
            alpha.append(acc * 255 // 64)
    layers = []
    for front in (False, True):
        px = bytearray()
        for i in range(n):
            a = alpha[i] if crystal[i] == front else 0
            px += bytes(int(c + 0.5) for c in ref[i]) + bytes((a,))
        layers.append(bleed_edges((w, h, px)))
    return layers


def title_parts():
    """the title as [(w, h, RGBA)] (FINAL; FANTASY IX), with Pillow (build time only)"""
    from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont
    font = ImageFont.truetype(TITLE_FONT, 240)
    space = font.getlength(' ') * 1.15
    box = font.getbbox(''.join(TITLE_WORDS), anchor='ls')  # (from the baseline: its top is negative)
    ascent, height = -box[1], box[3] - box[1]
    xs, x = [], 0.0
    for wd in TITLE_WORDS:
        xs.append(x)
        x += font.getlength(wd) + space
    full = (int(x - space) + 40, height + 40)
    masks = []
    for group in ((0, 1), (2,)):  # (the black words, the gold one)
        m = Image.new('L', full, 0)
        for i in group:
            ImageDraw.Draw(m).text((20 + xs[i], 20 + ascent), TITLE_WORDS[i], font=font, fill=255, anchor='ls')
        masks.append(m)
    k = (TITLE_WIDTH - 12) / (full[0] * TITLE_SQUEEZE)
    size = (TITLE_WIDTH, round(full[1] * k * TITLE_SQUEEZE / TITLE_SQUEEZE) + 12)
    inner = (TITLE_WIDTH - 12, size[1] - 12)
    black, gold = (Image.new('L', size, 0) for _ in masks)
    for dst, m in zip((black, gold), masks):
        dst.paste(m.resize(inner, Image.LANCZOS), (6, 6))
    both = ImageChops.lighter(black, gold)
    edge = both.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(0.7))
    edge = ImageChops.lighter(edge, both)
    ramp = Image.new('RGB', size)
    for y in range(size[1]):
        t = min(1.0, max(0.0, (y - 6) / inner[1]))
        c = [TITLE_GOLD[0][n] + (TITLE_GOLD[1][n] - TITLE_GOLD[0][n]) * t * 2 if t < 0.5 else
             TITLE_GOLD[1][n] + (TITLE_GOLD[2][n] - TITLE_GOLD[1][n]) * (t * 2 - 1) for n in range(3)]
        ramp.paste(tuple(int(v) for v in c), (0, y, size[0], y + 1))
    rgb = Image.new('RGB', size, (255, 255, 255))
    rgb.paste((0, 0, 0), (0, 0), black)
    rgb.paste(ramp, (0, 0), gold)
    art = rgb.convert('RGBA')
    art.putalpha(edge)
    cut = 6 + round((xs[1] - space / 2) * k * TITLE_SQUEEZE) + round(20 * k * TITLE_SQUEEZE)
    return [(im.width, im.height, bytearray(im.tobytes()))
            for im in (art.crop((0, 0, cut, size[1])), art.crop((cut, 0, size[0], size[1])))]


def title_scene(parts):
    """(atlas, triangles): the title alone (the program's own banner; scene() adds the heroes to it)"""
    atlas = (ATLAS_W, ATLAS_H, bytearray(4 * ATLAS_W * ATLAS_H))
    tris = []
    width = sum(p[0] for p in parts)
    top = 5.5 - TITLE_TOP * 13 / 128
    x, v = -width / 2 * TITLE_SCALE, 2
    for w, h, px in parts:
        u = 2  # (off the atlas's edge: the sampler repeats, and would filter the far edge's texels in)
        if u + w + 2 > HERO_CELLS_X or v + h > ATLAS_H:
            raise ValueError('title: its pieces do not fit the atlas')
        paste(atlas, (w, h, px), (u, v))
        x1, y0, z = x + w * TITLE_SCALE, top - h * TITLE_SCALE, TITLE_Z
        u0, u1, v0, v1 = u / ATLAS_W, (u + w) / ATLAS_W, 1 - v / ATLAS_H, 1 - (v + h) / ATLAS_H
        bl, br, tl, tr = (x, y0, z, u0, v1), (x1, y0, z, u1, v1), (x, top, z, u0, v0), (x1, top, z, u1, v0)
        tris += [(bl, br, tl), (br, tr, tl)]
        x, v = x1, v + h + 4
    return atlas, tris


def title_banner(flat):
    """bannertool's flat banner (a CBMD) -> the program's banner: its model with the title alone"""
    template, cwav = cbmd_parts(flat)
    check_template(template)
    return cbmd(cgfx(template, *title_scene(title_parts())), cwav)


def title_words(atlas, tris):
    """The title of the program's banner (title_scene's two pieces, from its atlas and triangles), cut into its
    words and packed into a 256x128 texture (FANTASY over FINAL and IX: the model has to be small):
    (texture, [(x0, y0, x1, y1 in the banner, u0, v0, u1, v1 in texels)], z)"""
    quads = [tris[k] for k in range(0, len(tris), 2)]  # (bl, br, tl) of each piece
    rects = []
    for bl, br, tl in quads:
        u0, u1 = round(bl[3] * ATLAS_W), round(br[3] * ATLAS_W)
        v0, v1 = round((1 - tl[4]) * ATLAS_H), round((1 - bl[4]) * ATLAS_H)
        rects.append((crop(atlas, (u0, v0, u1, v1)), bl[0], tl[1], (br[0] - bl[0]) / (u1 - u0)))
    left, unit = rects[0][1], rects[0][3]  # (the strip's left in the banner, units a texel)
    sw, sh = sum(r[0][0] for r in rects), rects[0][0][1]
    strip = (sw, sh, bytearray(4 * sw * sh))
    x = 0
    for r in rects:
        paste(strip, r[0], (x, 0))
        x += r[0][0]
    px = strip[2]

    def ink(x, y):  # (a letter, not its white edge)
        o = 4 * (y * sw + x)
        return px[o + 3] > 200 and sum(px[o:o + 3]) < 600
    used = [any(ink(x, y) for y in range(sh)) for x in range(sw)]
    runs, x = [], 0
    while x < sw:  # (runs of columns with ink, apart by more than a letter space)
        if not used[x]:
            x += 1
            continue
        e = x
        while True:
            nxt = next((c for c in range(e + 1, sw) if used[c]), None)
            if nxt is None or nxt - e > 7:
                break
            e = nxt
        runs.append((max(x - 4, 0), min(e + 5, sw)))
        x = e + 1
    if len(runs) != 3:
        raise ValueError(f'banner title: {len(runs)} words')
    rows = [y for y in range(sh) if any(ink(x, y) for x in range(sw))]
    y0, y1 = max(rows[0] - 3, 0), min(rows[-1] + 4, sh)
    h = y1 - y0
    tex = (256, 128, bytearray(4 * 256 * 128))
    spots = [(1, h + 3), (1, 1), (1 + runs[0][1] - runs[0][0] + 2, h + 3)]
    out = []
    for (x0, x1), (ax, ay) in zip(runs, spots):
        if ax + x1 - x0 > 256 or ay + h > 128:
            raise ValueError('banner title: its words do not fit their texture')
        paste(tex, crop(strip, (x0, y0, x1, y1)), (ax, ay))
        mid = left + sw * unit / 2
        gx, gy = (lambda x: (left + x * unit - mid) * TITLE_GROW), (lambda y: TITLE_Y - (y - y0) * unit * TITLE_GROW)
        out.append((gx(x0), gy(y1), gx(x1), gy(y0), ax, ay, ax + x1 - x0, ay + h))
    return tex, out, quads[0][0][2]


def _clashes(figures, cell=0.3):
    """[(name, name, cells)]: figures (name, vertices, triangle indices) whose surfaces share cells of a grid (they
    stand in each other: with a depth test that shows as one cutting through the other)"""
    grids = []
    for name, verts, idx in figures:
        cells = set()
        for k in range(0, len(idx), 3):
            a, b, c = (verts[i] for i in idx[k:k + 3])
            for wa, wb in ((1, 0), (0, 1), (0, 0), (.5, .5), (.5, 0), (0, .5), (1 / 3, 1 / 3)):
                wc = 1 - wa - wb
                cells.add(tuple(int((a[i] * wa + b[i] * wb + c[i] * wc) // cell) for i in range(3)))
        grids.append((name, cells))
    return [(a, b, len(ca & cb)) for i, (a, ca) in enumerate(grids) for b, cb in grids[i + 1:] if ca & cb]


def scene(imgs, atlas, tris, clashes=None):
    """The animated banner's model (banner_model.build's CGFX): the program's banner's title (its atlas and
    triangles) and the heroes, each skinned to its game bones and playing its motion's loop. clashes: a list that gets (name, name, cells) for heroes standing in each other (_clashes)"""
    import banner_model
    img = imgs[min(imgs)]
    del img
    figures = [(h, Figure(imgs, h[0], h[5])) for h in HEROES]
    scale = TALLEST / max(f.height() for _, f in figures)
    title_tex, words, title_z = title_words(atlas, tris)
    hero_tex = (256, 256, bytearray(4 * 256 * 256))
    columns = [0] * 4

    def place(small):  # (the models' textures at half size, in 64-texel columns)
        c = next((i for i in range(len(columns)) if columns[i] + small[1] <= 256), None)
        if small[0] > 64 or c is None:
            raise ValueError('banner: the models\' textures do not fit their texture')
        at = (64 * c, columns[c])
        paste(hero_tex, small, at)
        columns[c] += small[1]
        return at
    smalls = {}
    for _, f in figures:
        for t in f.textures + (f.weapon[3] if f.weapon else []):
            smalls[id(t)] = half(t[:3])
    where = {k: None for k in smalls}
    for k in sorted(smalls, key=lambda k: -smalls[k][1]):  # (tallest first)
        where[k] = place(smalls[k])

    bones, meshes, anims, picks = [('ttl', None, (0.0, 0.0, 0.0), IDENTITY3, 1.0)], [], {}, []
    for n, ((mid, name, hx, hz, feet, _), f) in enumerate(figures):
        a = f.turn if f.turn is not None else -TURN * hx
        ca, sa = math.cos(a), math.sin(a)
        # PS1 model space (x right, y down, z away) -> the banner's (x right, y up, z out), turned: a half turn
        # about x, then the turn about y
        root = len(bones)
        bones.append((f'h{n}r', None, (hx, feet, hz), [[ca, 0, sa], [0, -1, 0], [sa, 0, -ca]], scale))
        first = len(bones)
        for j in range(len(f.m.bones)):
            R, T = f.local(0, j)
            bones.append((f'h{n}b{j:02d}', root if j == 0 else first + f.m.bones[j][2], T, R, 1.0))

        meshes.append([f'h{n}', 0, [], [], [], first, f])
        picks.append([(gain * scale * scale, n, j) for gain, j in f.picks()])
    # the bones animated: every hero's first few picks, then the picks that bring most, to MOTION_BONES in all
    chosen = [p for seq in picks for p in seq[:MOTION_LEAST]]
    rest_picks = sorted((p for seq in picks for p in seq[MOTION_LEAST:]), reverse=True)
    for gain, n, j in chosen + rest_picks[:max(0, MOTION_BONES - len(chosen))]:
        first, f = meshes[n][5], meshes[n][6]
        count = len(f.frames)
        keys = [(k * f.step, f.local(k % count, j)) for k in range(count + 1)]  # (round to the first again)
        anims[first + j] = ('repeat', [(t, R) for t, (R, _) in keys],
                            [(t, T) for t, (_, T) in keys] if j == 0 else None)
    rest = banner_model.rest(bones)
    for name, mat, verts, idx, vbone, first, f in meshes:
        parts = [(f.polys, f.textures, lambda g, v: f.m.meshes[g][v])]
        if f.weapon:
            wbone, wverts, wpolys, wtex = f.weapon
            parts.append((wpolys, wtex, lambda g, v: (wbone, wverts[g][v])))
        for polys, texs, vertex in parts:
            seen = {}
            for vids, uvs, tex in polys:
                for (g, v), (u, vv) in zip(vids, uvs):
                    key = (g, v, u, vv, tex)
                    if key not in seen:
                        bone, loc = vertex(g, v)
                        M, T = rest[first + bone]
                        px, py = where[id(texs[tex])]
                        seen[key] = len(verts)
                        verts.append(tuple(T[i] + sum(M[i][c] * loc[c] for c in range(3)) for i in range(3))
                                     + ((px + u / 2) / 256, (py + vv / 2) / 256))
                        vbone.append(first + bone)
                    idx.append(seen[key])
    if clashes is not None:
        clashes.extend(_clashes([(HEROES[n][1], m[2], m[3]) for n, m in enumerate(meshes)]))
    tv, ti = [], []
    for x0, y0, x1, y1, u0, v0, u1, v1 in words:
        k = len(tv)
        tv += [(x0, y0, title_z, u0 / 256, v1 / 128), (x1, y0, title_z, u1 / 256, v1 / 128),
               (x0, y1, title_z, u0 / 256, v0 / 128), (x1, y1, title_z, u1 / 256, v0 / 128)]
        ti += [k, k + 1, k + 2, k + 1, k + 3, k + 2]
    out = [tuple(m[:5]) for m in meshes] + [('ttl', 1, tv, ti, [0] * len(tv))]  # (the blended title last)
    return banner_model.build([('heroes', hero_tex), ('title', title_tex)], [('hero', 0, False), ('title', 1, True)],
                              bones, out, anims)


# ---- texture and model formats -----------------------------------------------------------------------------------

def morton(x, y):
    r = 0
    for i in range(3):
        r |= (x >> i & 1) << 2 * i | (y >> i & 1) << 2 * i + 1
    return r


ORDER = [(x, y) for i in range(64) for y in range(8) for x in range(8) if morton(x, y) == i]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def tiled(img, pixel):
    """the PICA's 8x8 tiles in Z order, from the image's top (as bannertool and tex3ds write them)"""
    w, h, px = img
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for x, y in ORDER:
                X, Y = tx + x, ty + y
                out += pixel(px[4 * (Y * w + X):4 * (Y * w + X) + 4], X, Y)
    return bytes(out)


def rgba4(p, x, y):
    d = (BAYER[y & 3][x & 3] + 0.5) / 16  # (a colour already on a level stays on it: d < 1)
    r, g, b = (min(15, int(c / 17 + d)) for c in p[:3])
    return struct.pack('<H', r << 12 | g << 8 | b << 4 | (p[3] + 8) * 15 // 255)


def rgb565(p, x, y):
    return struct.pack('<H', (p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3)


# bannertool's flat model: where the parts that change are (checked against its values before anything is written)
OFS = {
    'file_size': 0x0C, 'bbox_center': 0xD98, 'bbox_size': 0xDC8,
    'idx_format': 0xE08, 'idx_count': 0xE10,  # (idx_count: the indices' bytes)
    'idx_ptr': 0xE14,
    'vtx_size': 0xE48, 'vtx_ptr': 0xE4C, 'vtx_stride': 0xE58,
    'tex_h': 0x1024, 'tex_w': 0x1028, 'tex_fmt': 0x1040, 'img_h': 0x1048, 'img_w': 0x104C,
    'tex_size': 0x1050, 'tex_ptr': 0x1054, 'imag': 0x14F8,
}
GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT = 0x1401, 0x1403


def _u32(c, o):
    return struct.unpack_from('<I', c, o)[0]


def check_template(c, own=False):
    """bannertool's flat banner model, or (own) one cgfx() made of it"""
    o = OFS
    for at, magic in ((0, b'CGFX'), (o['imag'], b'IMAG'), (0x1010, b'TXOB'), (0xD4C, b'SOBJ')):
        if c[at:at + 4] != magic:
            raise ValueError(f'banner model: no {magic} at {at:#x} (not bannertool\'s flat banner)')
    want = [(o['idx_ptr'] + _u32(c, o['idx_ptr']), 0x1500), (_u32(c, o['vtx_stride']), 20),
            (_u32(c, o['tex_fmt']), 4), (_u32(c, o['file_size']), len(c))]
    if own:
        want += [(_u32(c, o['idx_format']), GL_UNSIGNED_SHORT), (_u32(c, o['tex_w']), ATLAS_W),
                 (_u32(c, o['tex_h']), ATLAS_H)]
    else:
        want += [(_u32(c, o['idx_format']), GL_UNSIGNED_BYTE), (_u32(c, o['idx_count']), 6),
                 (_u32(c, o['vtx_size']), 0x50), (o['vtx_ptr'] + _u32(c, o['vtx_ptr']), 0x1508),
                 (_u32(c, o['tex_w']), 256), (_u32(c, o['tex_h']), 128), (o['tex_ptr'] + _u32(c, o['tex_ptr']), 0x1580)]
    for got, exp in want:
        if got != exp:
            raise ValueError(f'banner model: {got:#x} where {exp:#x} is expected')


def cgfx_scene(c):
    """(atlas, triangles) of a model cgfx() made"""
    check_template(c, own=True)
    o = OFS
    at = o['idx_ptr'] + _u32(c, o['idx_ptr'])
    idx = struct.unpack_from(f'<{_u32(c, o["idx_count"]) // 2}H', c, at)
    at = o['vtx_ptr'] + _u32(c, o['vtx_ptr'])
    verts = [struct.unpack_from('<5f', c, at + 20 * i) for i in range(_u32(c, o['vtx_size']) // 20)]
    at = o['tex_ptr'] + _u32(c, o['tex_ptr'])
    px = bytearray(4 * ATLAS_W * ATLAS_H)
    for ty in range(0, ATLAS_H, 8):
        for tx in range(0, ATLAS_W, 8):
            for x, y in ORDER:
                v, = struct.unpack_from('<H', c, at)
                at += 2
                d = 4 * ((ty + y) * ATLAS_W + tx + x)
                px[d:d + 4] = bytes(((v >> 12) * 17, (v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17))
    return (ATLAS_W, ATLAS_H, px), [tuple(verts[i] for i in idx[k:k + 3]) for k in range(0, len(idx), 3)]


def cgfx(template, atlas, tris):
    """the template (bannertool's flat model, or one made here: only its first 0x1500 bytes are kept) with the
    scene's triangles (u16 indices into shared vertices) and the atlas (RGBA4)"""
    verts, index, idx = [], {}, []
    for t in tris:
        for v in t:
            key = tuple(round(c, 5) for c in v)
            if key not in index:
                index[key] = len(verts)
                verts.append(key)
            idx.append(index[key])
    if len(verts) > 0xFFFF:
        raise ValueError('banner model: too many vertices')
    o = OFS
    ibuf = struct.pack(f'<{len(idx)}H', *idx)
    vbuf = b''.join(struct.pack('<5f', *v) for v in verts)
    vtx_at = 0x1500 + (len(ibuf) + 15) // 16 * 16
    tex_at = (vtx_at + len(vbuf) + 127) // 128 * 128
    tex = tiled(atlas, rgba4)
    out = bytearray(template[:0x1500])
    out += ibuf + bytes(vtx_at - 0x1500 - len(ibuf)) + vbuf + bytes(tex_at - vtx_at - len(vbuf)) + tex

    def put(at, v):
        struct.pack_into('<I', out, at, v)
    put(o['idx_format'], GL_UNSIGNED_SHORT)
    put(o['idx_count'], len(ibuf))  # (the index buffer's bytes: the HOME Menu drew half of them as a count)
    put(o['vtx_size'], len(vbuf))
    put(o['vtx_ptr'], vtx_at - o['vtx_ptr'])
    for k, v in (('tex_h', ATLAS_H), ('tex_w', ATLAS_W), ('img_h', ATLAS_H), ('img_w', ATLAS_W),
                 ('tex_size', len(tex))):
        put(o[k], v)
    put(o['tex_ptr'], tex_at - o['tex_ptr'])
    put(o['imag'] + 4, len(out) - o['imag'])
    put(o['file_size'], len(out))
    lo = [min(v[k] for v in verts) for k in range(3)]
    hi = [max(v[k] for v in verts) for k in range(3)]
    struct.pack_into('<3f', out, o['bbox_center'], *((a + b) / 2 for a, b in zip(lo, hi)))
    struct.pack_into('<3f', out, o['bbox_size'], *(b - a for a, b in zip(lo, hi)))
    if len(out) > CGFX_MAX:
        raise ValueError(f'banner model: {len(out)} bytes, past the HOME Menu\'s {CGFX_MAX}')
    return bytes(out)


# ---- LZ11 and the CBMD -------------------------------------------------------------------------------------------

def lz11_decompress(d):
    if d[0] != 0x11:
        raise ValueError('not LZ11')
    size, p = d[1] | d[2] << 8 | d[3] << 16, 4
    if size == 0:
        size, p = struct.unpack_from('<I', d, 4)[0], 8
    out = bytearray()
    while len(out) < size:
        flags = d[p]
        p += 1
        for b in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> b):
                out.append(d[p])
                p += 1
                continue
            ind = d[p] >> 4
            if ind == 0:
                n, disp, p = ((d[p] & 0xF) << 4 | d[p + 1] >> 4) + 0x11, ((d[p + 1] & 0xF) << 8 | d[p + 2]) + 1, p + 3
            elif ind == 1:
                n = ((d[p] & 0xF) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111
                disp, p = ((d[p + 2] & 0xF) << 8 | d[p + 3]) + 1, p + 4
            else:
                n, disp, p = ind + 1, ((d[p] & 0xF) << 8 | d[p + 1]) + 1, p + 2
            for _ in range(n):
                out.append(out[-disp])
    return bytes(out)


def lz11_compress(d):
    """greedy, the last few places each 3-byte string was seen"""
    n = len(d)
    out = bytearray(b'\x11' + struct.pack('<I', n)[:3]) if n < 1 << 24 else bytearray(b'\x11\0\0\0' + struct.pack('<I', n))
    seen = {}
    i = 0
    while i < n:
        flag_at = len(out)
        out.append(0)
        flags = 0
        for b in range(8):
            if i >= n:
                break
            best, bdisp = 0, 0
            if i + 3 <= n:
                key = d[i:i + 3]
                for j in reversed(seen.get(key, ())):
                    if i - j > 0x1000:
                        break
                    k = 3
                    lim = min(0x10110, n - i)
                    while k < lim and d[j + k] == d[i + k]:
                        k += 1
                    if k > best:
                        best, bdisp = k, i - j
                        if k == lim:
                            break
            if best >= 3:
                flags |= 0x80 >> b
                disp = bdisp - 1
                if best <= 0x10:
                    out += bytes(((best - 1) << 4 | disp >> 8, disp & 0xFF))
                elif best <= 0x110:
                    m = best - 0x11
                    out += bytes((m >> 4, (m & 0xF) << 4 | disp >> 8, disp & 0xFF))
                else:
                    m = best - 0x111
                    out += bytes((0x10 | m >> 12, m >> 4 & 0xFF, (m & 0xF) << 4 | disp >> 8, disp & 0xFF))
                step = best
            else:
                out.append(d[i])
                step = 1
            for q in range(i, min(i + step, n - 2)):
                lst = seen.setdefault(d[q:q + 3], [])
                lst.append(q)
                if len(lst) > 8:
                    del lst[0]
            i += step
        out[flag_at] = flags
    return bytes(out)


def cbmd_parts(bnr):
    """(the common CGFX, decompressed; the CWAV) of a CBMD (bannertool's: no per-language models)"""
    if bnr[:4] != b'CBMD':
        raise ValueError('banner: not a CBMD')
    offs = struct.unpack_from('<14I', bnr, 8)
    if any(offs[1:]):
        raise ValueError('banner: has per-language models')
    cwav_at, = struct.unpack_from('<I', bnr, 0x84)
    return lz11_decompress(bnr[offs[0]:cwav_at]), bnr[cwav_at:]


def cbmd(model, cwav):
    head = bytearray(0x88)
    head[:4] = b'CBMD'
    struct.pack_into('<I', head, 8, 0x88)
    body = lz11_compress(model)
    cwav_at = (0x88 + len(body) + 0xF) // 16 * 16
    struct.pack_into('<I', head, 0x84, cwav_at)
    return bytes(head) + body + bytes(cwav_at - 0x88 - len(body)) + cwav


# ---- the sound: the chocobo's call ---------------------------------------------------------------------------------

# The banner's sound is the world map's "Kweeeh!" (SE_CHOCOBO_CALL, the item menu's chocobo call), rendered from the
# disc by tools/akao_se.py: the sound effect, its instruments' bank and the world map's song (whose reverb the call
# is heard in) are in the world map's common file, the pitch and pan tables in the boot executable (the driver's
# g_akaoSemitonePitch and g_akaoPanCurve). The HOME Menu plays three seconds at most, and two channels only: a
# one-channel sound it plays as a row of twelve beeps (heard in the emulated menu and on the console).
SOUND_FILE = (DIR_COMMON, 0x2712)
SOUND_SE, SOUND_BANK, SOUND_SONG = 0x8044A, 0x90139, 0x70064
EXE_VRAM, EXE_HEADER = 0x80010000, 0x800
SOUND_SECONDS = 3.0


def sound(img, exe):
    """(rate, [(left, right)]): the chocobo's call from disc 1's FF9.IMG and boot executable"""
    import akao_se
    from encounter_catalog import db_items
    data = _file(img, *SOUND_FILE)
    items = dict(db_items(data, 0))
    at = lambda vram: vram - EXE_VRAM + EXE_HEADER
    semitones = struct.unpack_from('<12I', exe, at(_address('g_akaoSemitonePitch')))
    centre, = struct.unpack_from('<h', exe, at(_address('g_akaoPanCurve')) + 2 * 0x80)
    song = items[SOUND_SONG]
    if data[song:song + 4] != b'AKAO':
        raise ValueError('sound: not a song')
    mode, = struct.unpack_from('<H', data, song + 8)
    depth = data.find(b'\xFE\x02', song, song + 0x400)   # (the song's first reverb depth opcode: akao_opFE02)
    reverb = None if depth < 0 else (mode, (data[depth + 2] | struct.unpack_from('<b', data, depth + 3)[0] << 8))
    se = items[SOUND_SE]
    return akao_se.render(data[se:se + 0x400], akao_se.Bank(data[items[SOUND_BANK]:]), semitones, centre,
                          reverb=reverb, limit=SOUND_SECONDS)


def cwav(rate, pcm):
    """a CWAV (16-bit PCM, no loop) of [(left, right)] (or [(mono,)]), as bannertool writes one: the banner's sound"""
    n, chans = len(pcm), len(pcm[0])
    info = struct.pack('<BBHIII', 1, 0, 0, rate, 0, n) + struct.pack('<I', 0)
    table = 4 + 8 * chans
    refs = b''.join(struct.pack('<HHI', 0x7100, 0, table + 0x14 * c) for c in range(chans))
    step = 2 * n                            # (one channel's samples straight after the other's)
    infos = b''.join(struct.pack('<HHIHHII', 0x1F00, 0, 0x18 + step * c, 0, 0, 0xFFFFFFFF, 0) for c in range(chans))
    body = info + struct.pack('<I', chans) + refs + infos
    info_block = b'INFO' + struct.pack('<I', (8 + len(body) + 0x1F) // 0x20 * 0x20) + body
    info_block += bytes(-len(info_block) % 0x20)
    samples = b''
    for c in range(chans):
        one = struct.pack(f'<{n}h', *(p[c] for p in pcm))
        samples += one
    data_block = b'DATA' + struct.pack('<I', 8 + 0x18 + len(samples)) + bytes(0x18) + samples
    head = (b'CWAV' + struct.pack('<HHIIHH', 0xFEFF, 0x40, 0x02010000, 0x40 + len(info_block) + len(data_block), 2, 0)
            + struct.pack('<HHII', 0x7000, 0, 0x40, len(info_block))
            + struct.pack('<HHII', 0x7001, 0, 0x40 + len(info_block), len(data_block)))
    return head + bytes(0x40 - len(head)) + info_block + data_block


def smdh_icons(smdh, icon):
    """an SMDH with its 24x24 and 48x48 icons replaced"""
    big = tiled(icon, rgb565)
    w, _, px = icon
    small = bytearray()  # 24x24: 2x2 averages
    for y in range(0, 48, 2):
        for x in range(0, 48, 2):
            small += bytes(sum(px[4 * ((y + j) * w + x + i) + k] for j in (0, 1) for i in (0, 1)) // 4
                           for k in range(4))
    out = bytearray(smdh)
    if out[:4] != b'SMDH':
        raise ValueError('not an SMDH')
    out[0x2040:0x2040 + 0x480] = tiled((24, 24, small), rgb565)
    out[0x24C0:0x24C0 + 0x1200] = big
    return bytes(out)


def movie_sectors(read, lba):
    """the raw sectors of the logo's frames: read(lba, count) -> [2352-byte sectors] (ten a frame)"""
    first, last = LOGO_FRAMES[0], LOGO_FRAMES[-1]
    return read(lba + (first - 1) * 10 - 20, (last - first + 1) * 10 + 40)


ICON_BG = (226, 231, 240)  # (white: the crystal's whites would melt into it)


def icon(crystal):
    """the logo's crystal on a pale blue-grey, 48x48"""
    xs = [x for x, _ in LOGO_CRYSTAL]
    ys = [y for _, y in LOGO_CRYSTAL]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    half_side = (max(ys) - min(ys)) / 2 + 5
    w, _, px = crystal
    out = bytearray()
    f = 2 * half_side / 48
    for j in range(48):
        for i in range(48):
            acc, n = [0, 0, 0], 0
            for sy in range(4):  # (4x4 samples a pixel, over white)
                for sx in range(4):
                    x, y = int(cx - half_side + (i + (sx + 0.5) / 4) * f), int(cy - half_side + (j + (sy + 0.5) / 4) * f)
                    p = px[4 * (y * w + x):4 * (y * w + x) + 4] if 0 <= x < w and 0 <= y < crystal[1] else b'\0\0\0\0'
                    a = p[3] / 255
                    for k in range(3):
                        acc[k] += p[k] * a + ICON_BG[k] * (1 - a)
                    n += 1
            out += bytes(int(c / n + 0.5) for c in acc) + b'\xff'
    return 48, 48, out


def build(imgs, sectors, banner, smdh, clashes=None, exe=None, notes=None, version='us'):
    """(new banner, new SMDH): imgs {disc: FF9.IMG reader} (every disc a pose comes from), sectors: the icon's
    movie frames' (movie_sectors); banner and SMDH: the program's own (title_banner's, the plain icon); banner
    None: only the SMDH (a 3DSX has no banner). exe: disc 1's boot executable, for the banner's sound (None, or a
    sound that cannot be made: the program's own chime stays, and why is added to the list `notes`); version: the
    discs' release (the decomp's VERSION: where the tables are)"""
    global VERSION
    VERSION = version
    _, crystal = logo(sectors, movie_tables(imgs[min(imgs)]))
    new_smdh = smdh_icons(smdh, icon(crystal))
    if banner is None:
        return None, new_smdh
    template, wav = cbmd_parts(banner)
    if exe is not None:
        try:
            wav = cwav(*sound(imgs[1], exe))
        except (ValueError, KeyError, IndexError, struct.error) as e:
            if notes is not None:
                notes.append(f'the banner keeps its plain sound ({type(e).__name__}: {e})')
    return cbmd(scene(imgs, *cgfx_scene(template), clashes=clashes), wav), new_smdh


# ---- a developer's check: a CIA from build/ with the scene, from the local discs ----------------------------------

def main():
    import argparse
    sys.path.insert(0, os.path.join(HERE, '..', 'builder'))
    from ff9builder import ctr
    from ff9builder.discs import Disc
    from encounter_catalog import Img, find_image
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cia', help='the program-only CIA (with --title: bannertool\'s flat banner)')
    ap.add_argument('out')
    ap.add_argument('--title', action='store_true', help='make the program\'s banner (the title alone) of a flat one')
    ap.add_argument('--raw', default=os.path.join(HERE, '..', 'disks', 'us', 'raw', 'FF9 CD1.img'),
                    help='disc 1 as a raw image (the movie)')
    a = ap.parse_args()
    if a.title:
        with open(a.cia, 'rb') as f:
            out = title_banner(f.read())
        with open(a.out, 'wb') as f:
            f.write(out)
        return
    imgs = {n: Img(find_image(n, None)) for n in (1, 2, 3, 4)}
    with open(a.cia, 'rb') as f:
        cia = f.read()
    exefs = ctr.cia_exefs(cia)
    disc = Disc(a.raw)
    clashes = []
    notes = []
    banner, smdh = build(imgs, movie_sectors(disc.raw_sectors, disc.movie_lba(*LOGO_MOVIE)), exefs['banner'],
                         exefs['icon'], clashes, exe=disc.file_view(disc.boot).read(), notes=notes)
    for note in notes:
        print(note)
    for a_, b_, n in clashes:
        print(f'{a_} and {b_} stand in each other ({n} cells)')
    with open(a.out, 'wb') as f:
        f.write(ctr.cia_replace_exefs(cia, {'banner': banner, 'icon': smdh}))
    print(f'{a.out}: banner {len(banner)} bytes')


if __name__ == '__main__':
    main()
