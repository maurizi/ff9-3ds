#!/usr/bin/env python3
"""Run a PROFILE=1 DS ROM in melonDS until a game frame, save its state and print the timing profile.

Build the ROM with CAPTURE_FRAME=N (and usually AUTORUN=1, SCENARIO=...) so the game freezes at frame N and
the published 128-frame window is the one just before it; pass the same N here. Uses a private Xvfb display
(:99 by default) and melonDS's fast-memory backend to read the live frame counter (as capture_ds_at_frame.py).
Closes the emulator afterwards and restores the melonDS configuration. Outputs stay under build/.

  python3 tools/ds_profile.py --rom build/validation/x.nds --elf build/validation/x.elf --frame 600 \\
      --out build/validation/x_600.ml8
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CONFIG = Path.home() / '.var/app/net.kuribo64.melonDS/config/melonDS/melonDS.toml'


def symbols(elf):
    out = {}
    for row in subprocess.check_output(['nm', '-a', str(elf)], text=True).splitlines():
        parts = row.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def melon_pids(rom):
    pids = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            cmd = proc.joinpath('cmdline').read_bytes().split(b'\0')
        except OSError:
            continue
        if cmd and cmd[0].endswith(b'melonDS') and os.fsencode(str(rom)) in cmd:
            pids.append(int(proc.name))
    return pids


def window(display):
    env = dict(os.environ, DISPLAY=display)
    try:
        tree = subprocess.run(['xwininfo', '-root', '-tree'], env=env, capture_output=True, text=True).stdout
    except FileNotFoundError:
        sys.exit('xwininfo is required')
    for line in tree.splitlines():
        if 'melonDS 1.1' in line and '"[' in line:
            return line.split()[0]
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--frame', type=int, required=True)
    p.add_argument('--out', type=Path, required=True, help='new .ml8 state path under build/')
    p.add_argument('--display', default=':99')
    p.add_argument('--timeout', type=float, default=900)
    p.add_argument('--sd-folder', type=Path, help='DSi SD folder to sync (e.g. one holding ff9ds/raw/FF9 CD3.img)')
    p.add_argument('--sd-image', type=Path, help='SD image for --sd-folder; a new path makes melonDS size it to fit')
    p.add_argument('--ds-mode', action='store_true', help='run as an original DS (4 MB, 67 MHz): the --sd-folder is '
                   'given through DLDI (the game sees fat:/), --sd-image is the DLDI image')
    p.add_argument('--screenshot', type=Path, help='new PNG under build/: the emulator window at the capture frame '
                   '(the only view of GX output, which is not in RAM)')
    a = p.parse_args()
    rom, elf, out = a.rom.resolve(), a.elf.resolve(), a.out.resolve()
    if not out.is_relative_to(ROOT / 'build') or out.exists():
        p.error('--out must be a new path under build/')
    if a.screenshot and (not a.screenshot.resolve().is_relative_to(ROOT / 'build') or a.screenshot.exists()):
        p.error('--screenshot must be a new path under build/')
    sym = symbols(elf)
    if 'frames' not in sym:
        p.error('ELF has no frames counter')
    backup = out.with_suffix('.melonDS.toml')
    shutil.copy(CONFIG, backup)
    if a.ds_mode:
        import re
        text = CONFIG.read_text()
        text = re.sub(r'^ConsoleType = .*$', 'ConsoleType = 0', text, flags=re.M)
        start = text.index('[DLDI]\n')
        end = text.find('\n[', start + 1)
        block = '[DLDI]\nEnable = true\nFolderSync = true\nReadOnly = false\n'
        if a.sd_folder:
            block += f'FolderPath = "{a.sd_folder.resolve()}"\n'
        if a.sd_image:
            block += f'ImagePath = "{a.sd_image.resolve()}"\nImageSize = 0\n'
        CONFIG.write_text(text[:start] + block + text[end:])
    elif a.sd_folder:
        import re
        text = CONFIG.read_text()
        start = text.index('[DSi.SD]\n')
        end = text.find('\n[', start + 1)
        block = text[start:end]
        block = re.sub(r'^FolderPath = .*$', f'FolderPath = "{a.sd_folder.resolve()}"', block, flags=re.M)
        if a.sd_image:
            block = re.sub(r'^ImagePath = .*$', f'ImagePath = "{a.sd_image.resolve()}"', block, flags=re.M)
        block = re.sub(r'^FolderSync = .*$', 'FolderSync = true', block, flags=re.M)
        CONFIG.write_text(text[:start] + block + text[end:])
    xvfb = None
    if subprocess.run(['xdpyinfo', '-display', a.display], capture_output=True).returncode != 0:
        xvfb = subprocess.Popen(['Xvfb', a.display, '-screen', '0', '1024x768x24'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
    env = dict(os.environ, DISPLAY=a.display, XAUTHORITY='/dev/null')
    emu = subprocess.Popen(['flatpak', 'run', 'net.kuribo64.melonDS', str(rom)], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + a.timeout
        pid = None
        while pid is None:
            if time.monotonic() > deadline:
                sys.exit('emulator did not start')
            time.sleep(0.5)
            pids = melon_pids(rom)
            pid = pids[0] if pids else None
        bases = []
        while not bases:
            if time.monotonic() > deadline:
                sys.exit('no fast-memory mapping')
            time.sleep(0.5)
            for row in Path(f'/proc/{pid}/maps').read_text().splitlines():
                parts = row.split()
                lo, hi = (int(x, 16) for x in parts[0].split('-'))
                if 'melondsfastmem' in row and parts[1].startswith('rw') and int(parts[2], 16) == 0 \
                        and hi - lo >= 16 * 1024 * 1024:
                    bases.append(lo)
        frame = 0
        with open(f'/proc/{pid}/mem', 'rb', buffering=0) as memory:
            while frame < a.frame:
                if time.monotonic() > deadline:
                    win = window(a.display)
                    if a.screenshot and win:  # what stopped it (a fatal error shows on the bottom screen)
                        subprocess.run(['import', '-window', win, str(a.screenshot.resolve())], env=env)
                    sys.exit(f'timed out at frame {frame}')
                time.sleep(0.25)
                for base in bases:
                    memory.seek(base + sym['frames'] - 0x02000000)
                    value = int.from_bytes(memory.read(4), 'little')
                    if value < 10 ** 7:
                        frame = max(frame, value)
        time.sleep(1)
        win = window(a.display)
        if not win:
            sys.exit('no melonDS window')
        if a.screenshot:
            subprocess.run(['import', '-window', win, str(a.screenshot.resolve())], env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / 'tools/save_ds_state.py'), '--window', win, '--rom', str(rom),
                        '--elf', str(elf), '--out', str(out)], env=env, check=True)
    finally:
        emu.terminate()
        for pid in melon_pids(rom):
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        if xvfb:
            xvfb.terminate()
        shutil.copy(backup, CONFIG)
        backup.unlink()
    report = json.loads(subprocess.check_output([sys.executable, str(ROOT / 'tools/inspect_ds_profile.py'),
                                                 '--elf', str(elf), str(out)]))
    out.with_suffix('.json').write_text(json.dumps(report, indent=2))
    print(f"frame {report['ending_frame']} mode {report['mode']}: {report['interval_ms_per_frame']} ms/frame")
    for group in ('phases', 'raster_phases', 'compatibility_phases'):
        for name, v in (report.get(group) or {}).items() if isinstance(report.get(group), dict) else ():
            if v['ms_per_frame']:
                print(f"  {name:28s} {v['ms_per_frame']:8.3f} ms  calls {v['calls_per_frame']}")


if __name__ == '__main__':
    main()
