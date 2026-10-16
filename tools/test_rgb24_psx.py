#!/usr/bin/env python3
"""Generate a disc-free RGB24 PSX executable and compare an emulator screenshot.

Build build/tests/display_rgb24_probe first. Boot fixture.exe in a PSX emulator
with software rendering, 1x resolution, uncorrected internal-resolution PNG
screenshots and borders cropped. Pass its unmodified PNG back with --reference.
No game data is included. Images/EXE/reports must stay under ignored build/.
This validates synthetic scanout, not movie playback or original hardware.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--origin', type=int, nargs=2, default=(0,0), metavar=('X','Y'))
p.add_argument('--reference', type=Path)
p.add_argument('--probe', type=Path, default=ROOT/'build/tests/display_rgb24_probe')
a = p.parse_args()
out = a.out.resolve()
if not out.is_relative_to(ROOT/'build'):
    p.error('output must be under build/')
x,y = a.origin
if not (0 <= x <= 544 and 0 <= y <= 272):
    p.error('reference fixture must fit a 320x240 RGB24 image in physical VRAM')
if out.exists() and not a.reference:
    p.error('use a new output directory to generate the fixture')
out.mkdir(parents=True,exist_ok=True)
def words(values):
    return struct.pack('<'+'I'*len(values), *values)
vram = bytes((col*73+row*19+5)&255 for row in range(512) for col in range(2048))
packet = words([0xa0000000,0,(512<<16)|1024]) + vram
commands = words([0x03000000,0x05000000|(y<<10)|x,
                  0x06c60260,0x07040010,0x08000011])
base = 0x80010000
upload = base+0x1000
control = base+0x102000
def load(register,value):
    return [0x3c000000|(register<<16)|(value>>16),
            0x34000000|(register<<21)|(register<<16)|(value&65535)]
# After initializing argument pointers, transfer GP0, then GP1, then park.
code = load(4,upload)+load(5,upload+len(packet))+load(6,control)+load(7,control+len(commands))
code += [0x3c091f80,0x35291810,0xad200004,
         0x8c880000,0x24840004,0xad280000,0x1485fffc,0,
         0x8cc80000,0x24c60004,0xad280004,0x14c7fffc,0,
         0x1000ffff,0]
body = bytearray(0x103000)
body[:len(code)*4] = words(code)
body[0x1000:0x1000+len(packet)] = packet
body[0x102000:0x102000+len(commands)] = commands
header = bytearray(0x800)
header[:8] = b'PS-X EXE'
for offset,value in [(0x10,base),(0x18,base),(0x1c,len(body)),(0x30,0x801fff00)]:
    struct.pack_into('<I',header,offset,value)
fixture = bytes(header+body)
if a.reference:
    if not (out/'fixture.exe').exists() or (out/'fixture.exe').read_bytes()!=fixture:
        p.error('existing fixture does not match the requested origin')
else:
    (out/'fixture.exe').write_bytes(fixture)
native = subprocess.run([str(a.probe.resolve()),str(x),str(y)],
                        input=vram,capture_output=True,check=True).stdout
if len(native)!=320*240*3+256*192*2:
    raise RuntimeError('incomplete scanout probe output')
full = Image.frombytes('RGB',(320,240),native[:320*240*3])
full.save(out/'port.png')
if not a.reference:
    print(f'Boot {out}/fixture.exe; capture PNG and rerun with --reference.')
    raise SystemExit(0)
reference = Image.open(a.reference).convert('RGB')
reference.save(out/'psx.png')
same = reference.size==full.size and reference.tobytes()==full.tobytes()
scaled = struct.unpack('<49152H',native[320*240*3:])
ds_matches = reference.size==full.size
if ds_matches:
    pixels = reference.load()
    for oy in range(192):
        for ox in range(256):
            r,g,b = pixels[ox*5//4,oy*5//4]
            expected = 0x8000|(r>>3)|((g>>3)<<5)|((b>>3)<<10)
            ds_matches &= scaled[oy*256+ox]==expected
report = dict(x=x,y=y,reference_size=list(reference.size),
              rgb8_match=same,ds_rgb555_match=ds_matches)
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(report)
raise SystemExit(not (same and ds_matches))
