#!/usr/bin/env python3
"""Compare cached direct/generated texels with actual DS texture/palette banks.

Reads a melonDS 1.1 state and its matching GX ELF. Requires nm and Python-enabled
GDB for native structure layouts. This checks stored texels, not rendered screen
pixels, draw completeness, modulation, blending, timing or animation parity.
"""
import argparse
import collections
import json
from pathlib import Path
import struct
import subprocess


def layouts(elf):
    code = """import gdb,json
names=('TexEntry','TextureImage','DynamicArray','gl_hidden_globals','gl_texture_data','gl_palette_data')
result={}
for name in names:
 t=gdb.lookup_type(name)
 result[name]={'size':t.sizeof,'fields':{f.name:[int(f.bitpos)//8,f.type.sizeof] for f in t.fields() if f.name}}
result['cache_count']=int(gdb.parse_and_eval("sizeof('port/ds/render_gx.c'::cache)/sizeof(TexEntry)"))
result['image_count']=int(gdb.parse_and_eval("sizeof('port/ds/render_gx.c'::images)/sizeof(TextureImage)"))
print('GX_LAYOUT='+json.dumps(result))
"""
    result = subprocess.check_output(['gdb', '-nx', '-q', '-batch', '-iex', 'set auto-load off',
                                     '-iex', 'set debuginfod enabled off', '--se', str(elf),
                                     '-ex', 'python exec(' + repr(code) + ')'], text=True)
    return json.loads(next(row.removeprefix('GX_LAYOUT=') for row in result.splitlines()
                           if row.startswith('GX_LAYOUT=')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('state', type=Path)
    parser.add_argument('--elf', type=Path, required=True)
    args = parser.parse_args()
    data = args.state.read_bytes()
    if data[:4] != b'MELN' or struct.unpack_from('<I', data, 8)[0] != len(data):
        parser.error('expected a complete melonDS state')
    sections, pos = {}, 16
    while pos < len(data):
        size = struct.unpack_from('<I', data, pos + 4)[0]
        if size < 16 or pos + size > len(data):
            parser.error('invalid state section length')
        sections[data[pos:pos + 4]] = data[pos:pos + size]
        pos += size
    ram = sections[b'NDSG'][20:20 + 16 * 1024 * 1024]
    # GPUG: 16-byte section header, 16 bytes of scanline/status fields,
    # 2 KiB palettes, 2 KiB OAM, then physical VRAM banks A through I.
    gpu = sections[b'GPUG'][4128:4128 + 656 * 1024]
    controls = sections[b'GPUG'][4128 + 656 * 1024:4128 + 656 * 1024 + 9]
    if len(gpu) != 656 * 1024 or list(controls[:4]) != [0x83, 0x8B, 0x93, 0x9B]:
        parser.error('expected GX banks A-D mapped to texture slots 0-3')
    symbols = {}
    for row in subprocess.check_output(['nm', '-a', str(args.elf)], text=True).splitlines():
        fields = row.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    layout = layouts(args.elf)

    def read(address, size):
        offset = address - 0x02000000
        if offset < 0 or offset + size > len(ram):
            raise ValueError(f'pointer outside captured main RAM: {address:08x}')
        return ram[offset:offset + size]

    def member(address, name, field):
        offset, size = layout[name]['fields'][field]
        return int.from_bytes(read(address + offset, size), 'little')

    def array_element(name, index):
        array = symbols['glGlob'] + layout['gl_hidden_globals']['fields'][name][0]
        count = member(array, 'DynamicArray', 'cur_size')
        if not 0 < index < count:
            raise ValueError(f'invalid {name} index {index}')
        return int.from_bytes(read(member(array, 'DynamicArray', 'data') + 4 * index, 4), 'little')

    def texture(name):
        pointer = array_element('texturePtrs', name)
        fmt = member(pointer, 'gl_texture_data', 'texFormat')
        width, height = 8 << ((fmt >> 20) & 7), 8 << ((fmt >> 23) & 7)
        kind = (fmt >> 26) & 7
        if kind not in (3, 4, 7):
            raise ValueError(f'unsupported GPU texture format {kind}')
        size = width * height * (2 if kind == 7 else 1) // (2 if kind == 3 else 1)
        physical = member(pointer, 'gl_texture_data', 'vramAddr') - 0x06800000
        if physical < 0 or physical + size > 512 * 1024 or (fmt & 0xFFFF) * 8 != physical:
            raise ValueError('texture address does not match the expected bank mapping')
        return fmt, width, height, kind, gpu[physical:physical + size]

    byte_mismatches, uploaded = [], 0
    for index in range(layout['image_count']):
        image = symbols['images'] + index * layout['TextureImage']['size']
        if not member(image, 'TextureImage', 'used'):
            continue
        uploaded += 1
        name = member(image, 'TextureImage', 'tex')
        fmt, width, height, kind, pixels = texture(name)
        cpu = read(member(image, 'TextureImage', 'pixels'), len(pixels))
        if pixels != cpu:
            byte_mismatches.append(index)

    entries = composites = texels = mismatches = 0
    formats, examples = collections.Counter(), []
    for index in range(layout['cache_count']):
        entry = symbols['cache'] + index * layout['TexEntry']['size']
        if not member(entry, 'TexEntry', 'used'):
            continue
        key = member(entry, 'TexEntry', 'key')
        composite = bool(key >> 63)
        if not composite and ((key >> 30) & 3) != 2:
            continue
        width = (key >> 32) & 511 if composite else ((key >> 48) & 63) * 8
        height = (key >> 41) & 511 if composite else ((key >> 54) & 63) * 8
        entries += 1
        composites += composite
        image_id = member(entry, 'TexEntry', 'image')
        logical_name = member(entry, 'TexEntry', 'tex')
        name, dx, dy = logical_name, 0, 0
        if image_id:
            image = symbols['images'] + (image_id - 1) * layout['TextureImage']['size']
            name = member(image, 'TextureImage', 'tex')
            if 'image_u' in layout['TexEntry']['fields']:
                dx = member(entry, 'TexEntry', 'image_u')
                dy = member(entry, 'TexEntry', 'image_v')
        fmt, gpu_width, gpu_height, kind, pixels = texture(name)
        if dx + width > gpu_width or dy + height > gpu_height:
            raise ValueError('logical region exceeds its GPU texture')
        palette = ()
        if kind != 7:
            logical = array_element('texturePtrs', logical_name)
            palette_id = member(logical, 'gl_texture_data', 'palIndex')
            pal = array_element('palettePtrs', palette_id)
            start = member(pal, 'gl_palette_data', 'vramAddr') - 0x06800000
            size = member(pal, 'gl_palette_data', 'palSize')
            if start < 512 * 1024 or start + size > 608 * 1024:
                raise ValueError('palette outside banks E-G')
            if member(pal, 'gl_palette_data', 'addr') * 16 != start - 512 * 1024:
                raise ValueError('unexpected palette bank mapping')
            palette = struct.unpack('<' + str(size // 2) + 'H', gpu[start:start + size])
        cpu = struct.unpack('<' + str(width * height) + 'H',
                            read(member(entry, 'TexEntry', 'pixels'), width * height * 2))
        formats[{3: 'RGB16', 4: 'RGB256', 7: 'RGBA'}[kind]] += 1
        for y in range(height):
            for x in range(width):
                at = (dy + y) * gpu_width + dx + x
                expected = cpu[y * width + x]
                expected = expected if expected & 0x8000 else 0
                if kind == 7:
                    actual = struct.unpack_from('<H', pixels, at * 2)[0]
                    actual = actual if actual & 0x8000 else 0
                else:
                    colour = ((pixels[at // 2] >> ((at & 1) * 4)) & 15) if kind == 3 else pixels[at]
                    actual = 0 if (fmt & (1 << 29)) and not colour else 0x8000 | (palette[colour] & 0x7FFF)
                texels += 1
                if actual != expected:
                    mismatches += 1
                    if len(examples) < 8:
                        examples.append({'cache': index, 'x': x, 'y': y,
                                         'expected': f'{expected:04x}', 'gpu': f'{actual:04x}'})
    if not entries:
        parser.error('no resident direct/generated textures to verify')
    print(json.dumps({'state': str(args.state), 'elf': str(args.elf),
                      'scope': 'Resident direct/generated texels and uploaded image bytes; screen rendering is not checked',
                      'uploaded_images': uploaded, 'upload_mismatch_images': byte_mismatches,
                      'direct_entries': entries - composites, 'composite_entries': composites,
                      'gpu_formats': dict(formats), 'sampled_texels': texels,
                      'different_texels': mismatches, 'examples': examples}, indent=2))
    return bool(mismatches or byte_mismatches)


if __name__ == '__main__':
    raise SystemExit(main())
