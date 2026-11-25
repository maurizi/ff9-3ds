"""A skinned, animated HOME Menu banner model (CGFX) from plain data, through pycgfx's glTF converter
(third_party/pycgfx: its convert.py takes objects with a glTF's attributes; this module makes them, and corrects the
result where the HOME Menu, run in Azahar with a console's system files, showed it must be). Plain Python.

What that menu showed (2026-11, `build/artic` holds the scripts that asked it):
- It turns every model about its y axis, once round in 600 frames; only a mesh's own "billboard" bone stands still,
  and a skinned mesh has none. So everything here hangs from one bone, `stage`, that the animation turns the other
  way at the same rate: the picture stands still.
- An animation and a mesh without a skin crashed it; so did a bone dictionary whose order pycgfx changed (it moves
  bones with blended meshes to the end: such meshes are given last here, where the sort leaves them); so did more
  than 64 animated bones, the stage among them (64 ran, 90 and 190 did not, with few keys as with many: its
  evaluator is made for 64 members): the caller chooses which.
- A model over 512 KB is not shown at all.
- pycgfx's angles from quaternions go wrong near a right-angle pitch: they are computed here, from the matrices.
- It draws anticlockwise faces, with a depth test where the material has one (alpha-tested meshes write depth,
  blended ones only test it).
"""
import math
import os
import struct
import sys

# (third_party/ is beside tools/ in the checkout and in the builder's bundle)
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'third_party'))

SPIN_FRAMES = 600.0  # the HOME Menu's turn of a model
MAX_BYTES = 0x80000
STILL_TURN, STILL_MOVE = 0.006, 0.25  # an angle (radians), a place (the bone's units) that changes less is constant
MAX_SKIN_BONES = 18  # (the PICA takes 20 a draw; pycgfx adds the mesh's own)
FLOAT, UBYTE, USHORT = 5126, 5121, 5123


class NS:
    """an object with the attributes given, None for any other (as gltflib's)"""

    def __init__(self, **kw):
        self.__dict__.update(kw)

    def __getattr__(self, name):
        return None


def mat_mul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(A):
    return [[A[j][i] for j in range(3)] for i in range(3)]


def orthonormal(R):
    """the nearest thing to R with unit, perpendicular columns (the game's matrices are 12-bit)"""
    c = [[R[i][j] for i in range(3)] for j in range(3)]

    def unit(v):
        n = math.sqrt(sum(x * x for x in v))
        return [x / n for x in v]
    c0 = unit(c[0])
    d = sum(a * b for a, b in zip(c0, c[1]))
    c1 = unit([b - d * a for a, b in zip(c0, c[1])])
    c2 = [c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]]
    return [[c0[i], c1[i], c2[i]] for i in range(3)]


def euler(R):
    """(x, y, z) with R = Rz Ry Rx (the CGFX's bone angles), also where the pitch is a right angle"""
    R = orthonormal(R)
    if abs(R[2][0]) > 0.999999:
        return math.atan2(-R[1][2], R[1][1]), math.copysign(math.pi / 2, -R[2][0]), 0.0
    return math.atan2(R[2][1], R[2][2]), math.asin(-R[2][0]), math.atan2(R[1][0], R[0][0])


def tiled_rgba4(img):
    """(w, h, RGBA) -> the PICA's RGBA4 texels (8x8 tiles in Z order), the image's bottom row first, as pycgfx
    stores a texture (v 0 is then the image's top)"""
    w, h, px = img

    def morton(x, y):
        r = 0
        for i in range(3):
            r |= (x >> i & 1) << 2 * i | (y >> i & 1) << 2 * i + 1
        return r
    order = [(x, y) for i in range(64) for y in range(8) for x in range(8) if morton(x, y) == i]
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for x, y in order:
                o = 4 * ((h - 1 - (ty + y)) * w + tx + x)
                r, g, b, a = (min(15, (c + 8) // 17) for c in px[o:o + 4])
                out += struct.pack('<H', r << 12 | g << 8 | b << 4 | a)
    return bytes(out)


def rest(bones):
    """[(3x3 with the scale, translation)] of build's bones in the model's space, as they are given (their matrices
    made orthonormal: the skins' inverses and the caller's vertices both come from this)"""
    world = []
    for name, parent, t, R, s in bones:
        R = orthonormal(R)
        L = [[R[i][j] * s for j in range(3)] for i in range(3)]
        if parent is None:
            world.append((L, tuple(t)))
        else:
            PR, PT = world[parent]
            world.append((mat_mul(PR, L), tuple(PT[i] + sum(PR[i][k] * t[k] for k in range(3)) for i in range(3))))
    return world


def build(textures, materials, bones, meshes, anims):
    """The CGFX.
    textures:  [(name, (w, h, RGBA))], sides multiples of 8 (colours as they are: dither them before, if wanted)
    materials: [(name, texture index, blended)]: blended (soft edges; drawn over what is behind) or alpha-tested
    bones:     [(name, parent index or None, (x, y, z), R (3x3), scale)], a parent before its children; all hang
               from the stage
    meshes:    [(name, material index, [(x, y, z, u, v)] in the model's space at the bones' rest, [index],
               [bone index of each vertex])], blended ones last; cut here into draws of few bones
    anims:     {bone index: (repeat, [(frame, R)] or None, [(frame, (x, y, z))] or None)}: keys by 60ths of a
               second; repeat 'mirror' plays them there and back for ever, 'repeat' over and over, 'hold' stops at
               the last
    """
    from pycgfx import convert
    from pycgfx.cgfx.canm import RepeatMethod
    from pycgfx.cgfx.sobj import BoneFlag

    accessors = []

    def acc(data, ctype, typ, count):
        accessors.append(NS(data=bytes(data), componentType=ctype, type=typ, count=count))
        return len(accessors) - 1

    # nodes: the stage, the bones under it, then one node a draw
    stage = len(bones)
    nodes = [NS(name=name, translation=list(t), rotation=[0, 0, 0, 1], scale=[s] * 3, children=[])
             for name, _, t, _, s in bones]
    nodes.append(NS(name='stage', children=[]))
    for i, (_, parent, _, _, _) in enumerate(bones):
        nodes[stage if parent is None else parent].children.append(i)
    for n in nodes:
        if not n.children:
            del n.__dict__['children']

    world = rest(bones)

    def inverse(M, T):
        det = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
               + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
        inv = [[(M[(j + 1) % 3][(i + 1) % 3] * M[(j + 2) % 3][(i + 2) % 3]
                 - M[(j + 1) % 3][(i + 2) % 3] * M[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
        return inv, [-sum(inv[i][k] * T[k] for k in range(3)) for i in range(3)]

    gl_meshes, skins, scene_nodes = [], [], [stage]
    for name, mat, verts, idx, vbone in meshes:
        draws = []  # [bones, triangles]
        for k in range(0, len(idx), 3):
            tri = idx[k:k + 3]
            bs = {vbone[v] for v in tri}
            for d in draws:
                if len(d[0] | bs) <= MAX_SKIN_BONES:
                    d[0].update(bs)
                    d[1].append(tri)
                    break
            else:
                draws.append([set(bs), [tri]])
        for n, (bs, tris) in enumerate(draws):
            joints = sorted(bs)
            slot = {j: i for i, j in enumerate(joints)}
            remap, vs, ix = {}, [], []
            for tri in tris:
                for v in tri:
                    if v not in remap:
                        remap[v] = len(vs)
                        vs.append(v)
                    ix.append(remap[v])
            ibm = b''
            for j in joints:
                inv, it = inverse(*world[j])
                ibm += struct.pack('<16f', inv[0][0], inv[1][0], inv[2][0], 0, inv[0][1], inv[1][1], inv[2][1], 0,
                                   inv[0][2], inv[1][2], inv[2][2], 0, it[0], it[1], it[2], 1)
            skins.append(NS(joints=joints, inverseBindMatrices=acc(ibm, FLOAT, 'MAT4', len(joints))))
            attrs = NS(POSITION=acc(b''.join(struct.pack('<3f', *verts[v][:3]) for v in vs), FLOAT, 'VEC3', len(vs)),
                       TEXCOORD_0=acc(b''.join(struct.pack('<2f', *verts[v][3:5]) for v in vs), FLOAT, 'VEC2', len(vs)),
                       JOINTS_0=acc(bytes(slot[vbone[v]] for v in vs), UBYTE, 'SCALAR', len(vs)),
                       WEIGHTS_0=acc(struct.pack('<f', 1) * len(vs), FLOAT, 'SCALAR', len(vs)))
            gl_meshes.append(NS(name=f'{name}{n}', primitives=[NS(
                attributes=attrs, indices=acc(struct.pack(f'<{len(ix)}H', *ix), USHORT, 'SCALAR', len(ix)),
                material=mat)]))
            scene_nodes.append(len(nodes))
            nodes.append(NS(name=f'{name}m{n}', mesh=len(gl_meshes) - 1, skin=len(skins) - 1))

    # the animation: a channel a moving bone (their values are written afterwards), and the stage's turn
    channels, samplers = [], []

    def channel(node, path, count, width):
        times = acc(struct.pack(f'<{count}f', *(i / 60 for i in range(count))), FLOAT, 'SCALAR', count)
        out = acc(struct.pack(f'<{width}f', *([0, 0, 0, 1] if width == 4 else [0, 0, 0])) * count, FLOAT,
                  'VEC4' if width == 4 else 'VEC3', count)
        samplers.append(NS(input=times, output=out, interpolation='LINEAR'))
        channels.append(NS(sampler=len(samplers) - 1, target=NS(node=node, path=path)))
    channel(stage, 'rotation', 2, 4)
    for b, (_, rot, pos) in anims.items():
        if rot:
            channel(b, 'rotation', len(rot), 4)
        if pos:
            channel(b, 'translation', len(pos), 3)

    images = [NS(name=name, width=img[0], height=img[1], data=tiled_rgba4(img)) for name, img in textures]
    gl_materials = [NS(name=name, alphaMode='BLEND' if blended else 'MASK', alphaCutoff=0.5,
                       pbrMetallicRoughness=NS(baseColorTexture=NS(index=tex)))
                    for name, tex, blended in materials]
    model = NS(scene=0, scenes=[NS(nodes=scene_nodes)], nodes=nodes, meshes=gl_meshes, skins=skins,
               accessors=accessors, images=images, materials=gl_materials,
               samplers=[NS(magFilter=9729, minFilter=9729, wrapS=33071, wrapT=33071)],
               textures=[NS(source=i, sampler=0) for i in range(len(images))],
               animations=[NS(name='COMMON', channels=channels, samplers=samplers)])
    gltf = NS(model=model, default_sampler=model.samplers[0],
              default_material=NS(name='none', pbrMetallicRoughness=NS()))
    cg = convert.convert_gltf(gltf)
    cm = cg.data.models['COMMON']

    for name in cm.materials:  # unlit: the texture alone
        mt = cm.materials[name]
        mt.flags = 0
        tc = mt.fragment_shader.texture_combiners
        tc[0].src_rgb = tc[0].src_alpha = 0x003
        tc[0].combine_rgb = tc[0].combine_alpha = 0
        for i in (1, 2):
            tc[i].src_rgb = 0x00F
            tc[i].combine_rgb = 0

    for name, _, _, R, _ in bones:  # the angles at rest
        bone = cm.skeleton.bones[name]
        bone.rotation.x, bone.rotation.y, bone.rotation.z = euler(R)
        if any(euler(R)):
            bone.flags &= ~(BoneFlag.IsRotateZero | BoneFlag.IsIdentity)

    an = cg.data.skeletal_animations['COMMON']
    an.frame_size = SPIN_FRAMES
    for name in an.member_animations_data:
        t = an.member_animations_data[name]
        bone = cm.skeleton.bones[name]
        if name == 'stage':
            curves = {'rot_y': ('hold', [(0.0, 0.0), (SPIN_FRAMES, 2 * math.pi)])}
        else:
            repeat, rot, pos = anims[[b[0] for b in bones].index(name)]
            curves = {}
            if rot:
                es, prev = [], None
                for _, R in rot:
                    e = list(euler(R))
                    if prev:  # (the nearest turn to the key before)
                        for k in range(3):
                            e[k] += 2 * math.pi * round((prev[k] - e[k]) / (2 * math.pi))
                    es.append(e)
                    prev = e
                for k, attr in enumerate(('rot_x', 'rot_y', 'rot_z')):
                    curves[attr] = (repeat, [(f, e[k]) for (f, _), e in zip(rot, es)])
            if pos:
                for k, attr in enumerate(('pos_x', 'pos_y', 'pos_z')):
                    curves[attr] = (repeat, [(f, p[k]) for f, p in pos])
        at_rest_values = {'scale_x': bone.scale.x, 'scale_y': bone.scale.y, 'scale_z': bone.scale.z,
                'rot_x': bone.rotation.x, 'rot_y': bone.rotation.y, 'rot_z': bone.rotation.z,
                'pos_x': bone.position.x, 'pos_y': bone.position.y, 'pos_z': bone.position.z}
        for attr, at_rest in at_rest_values.items():
            # every one of the nine is a curve or a constant: none is left for the menu to "ignore"
            keys = curves.get(attr)
            cur = getattr(t, attr)
            still = STILL_TURN if attr.startswith('rot') else STILL_MOVE
            if not keys or cur is None or max(v for _, v in keys[1]) - min(v for _, v in keys[1]) < still:
                setattr(t, attr, float(keys[1][0][1] if keys else at_rest))
                continue
            repeat, kv = keys
            seg = cur.segments[0]
            del seg.keys[len(kv):]
            for key, (f, v) in zip(seg.keys, kv):
                key.frame, key.value = float(f), float(v)
            cur.start_frame = seg.start_frame = float(kv[0][0])
            cur.end_frame = seg.end_frame = float(kv[-1][0])
            if repeat != 'hold':
                cur.post_repeat_method = (RepeatMethod.MirroredRepeat if repeat == 'mirror'
                                          else RepeatMethod.Repeat)
    out = convert.write(cg)
    if len(out) > MAX_BYTES:
        raise ValueError(f'banner model: {len(out)} bytes, past the HOME Menu\'s {MAX_BYTES}')
    return out
