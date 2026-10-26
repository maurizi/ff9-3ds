#!/usr/bin/env python3
"""Transcode a disc's movies (STR, and MBG: fields show some full-screen too) to FastVideoDS files for the DS
(docs/movie-validation.md).

Each movie: build/movie_export decodes it with the port's own MDEC path to 320x224 RGB24, FFmpeg scales it to
256x179 (the movies have square pixels) and crops it to 256x176 (a multiple of 8; padding rows instead would leave
the encoder's blocks ringing at their edge with the picture), and the
FastVideoDS encoder (an external checkout, run in the `fvenc` docker image: not part of this repository) writes
OUT/dN/LBA.fv, video only. The game's player still streams the STR for its audio and camera data and looks the
.fv up by the movie's first sector.

Usage: tools/fmv_transcode.py --disc 1 [--only NAME] [--out build/fmv]   (outputs stay under build/ or ff9ds/)
"""
import argparse, os, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import movie_catalog

ROOT = Path(__file__).resolve().parents[1]
OVL64 = ROOT / 'decomp/disks/us/ovl/64.bin'
OVL64_VRAM = 0x80198700
TABLES = [(0x8019DCFC, 216), (0x8019DEC8, 128), (0x8019DF4C, 128)]  # g_mbgVlcPairs, quantization, scale
ENCODER = Path(os.environ.get('FV_ENCODER', ROOT.parent / 'FastVideoDSEncoder/out'))
SCALE = 'scale=256:179:flags=area,crop=256:176:0:1'


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--disc', type=int, choices=range(1, 5), required=True)
    p.add_argument('--image', type=Path, help='raw disc image (default disks/us/raw/FF9 CDn.img)')
    p.add_argument('--out', type=Path, default=ROOT / 'build/fmv')
    p.add_argument('--only', help='only movies whose name contains this')
    p.add_argument('--force', action='store_true', help='re-encode existing outputs')
    p.add_argument('--lambda', dest='lam', type=int, help='the encoder\'s trellis rate weight (its default 200; lower keeps '
                                         'more detail, larger files): an encoder taking FV_LAMBDA')
    a = p.parse_args()
    out = a.out.resolve()
    if not (out.is_relative_to(ROOT / 'build') or out.is_relative_to(ROOT / 'ff9ds')):
        p.error('output must be under build/ or ff9ds/')
    image = (a.image or ROOT / f'disks/us/raw/FF9 CD{a.disc}.img').resolve()
    work = out / 'work'
    work.mkdir(parents=True, exist_ok=True)
    tables = work / 'tables.bin'
    ovl = OVL64.read_bytes()
    tables.write_bytes(b''.join(ovl[addr - OVL64_VRAM:addr - OVL64_VRAM + size] for addr, size in TABLES))
    subprocess.run(['tools/docker.sh', 'make', 'build/movie_export'], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
    dest = out / f'd{a.disc}'
    dest.mkdir(exist_ok=True)
    for movie in movie_catalog.movie_extents(image):
        name = movie['name'].removesuffix(';1')
        if not name.endswith(('.STR', '.MBG')) or (a.only and a.only not in name):
            continue
        fv = dest / f"{movie['lba']}.fv"
        if fv.exists() and not a.force:
            print(f'{name}: {fv.name} exists', flush=True)
            continue
        mkv = work / 'movie.mkv'
        export = subprocess.Popen([str(ROOT / 'build/movie_export'), str(image), str(movie['lba']), str(movie['sectors']),
                                   str(tables)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '320x224', '-r', '15',
                        '-i', '-', '-vf', SCALE, '-c:v', 'ffv1', str(mkv)], stdin=export.stdout, check=True)
        export.stdout.close()
        report = export.stderr.read().decode().strip()
        if export.wait() or ' bad 0' not in report:
            sys.exit(f'{name}: {report}')
        tmp = fv.with_suffix('.tmp')
        subprocess.run(['docker', 'run', '--rm', '-t', '-u', f'{os.getuid()}:{os.getgid()}'] +
                       (['-e', f'FV_LAMBDA={a.lam}'] if a.lam else []) + ['-v', f'{ENCODER}:/enc:ro',
                        '-v', f'{work}:/w', '-w', '/w', 'fvenc', 'dotnet', '/enc/FastVideoDSEncoder.dll', 'movie.mkv',
                        'movie.fv'], check=True, stdout=subprocess.DEVNULL)
        (work / 'movie.fv').rename(tmp)
        tmp.rename(fv)
        mkv.unlink()
        print(f"{name}: {report} -> {fv.relative_to(ROOT) if fv.is_relative_to(ROOT) else fv} "
              f"({fv.stat().st_size // 1024} KB)", flush=True)


if __name__ == '__main__':
    main()
