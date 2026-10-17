#!/usr/bin/env python3
"""Catch the first ARM9 exception of a DS ROM in melonDS and print the faulting registers with symbols.

libnds's exception entry saves the faulting r0-r15 in `exceptionRegisters` before its handler draws the guru
screen, whose own register dump describes the handler instead. This runs the ROM under melonDS's GDB stub
(interpreter), breaks at `defaultHandler`, reads that array and the words around the faulting stack pointer, and
maps them to the ELF's symbols. The melonDS configuration is restored afterwards.

  python3 tools/ds_catch.py --rom build/validation/x.nds --elf build/validation/x.elf \\
      [--sd-folder build/sdroot_disc3 --sd-image build/melon_sd_disc3.img]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ds_sample  # noqa: E402  (stub client and config helpers)

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--port', type=int, default=3333)
    p.add_argument('--display', default=':99')
    p.add_argument('--timeout', type=float, default=3600)
    p.add_argument('--sd-folder', type=Path)
    p.add_argument('--sd-image', type=Path)
    a = p.parse_args()
    rom, elf = a.rom.resolve(), a.elf.resolve()
    syms = {}
    for row in subprocess.check_output(['nm', str(elf)], text=True).splitlines():
        parts = row.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    for need in ('defaultHandler', 'exceptionRegisters'):
        if need not in syms:
            sys.exit(f'{need} not in ELF')

    def where(addr):
        out = subprocess.run(['arm-none-eabi-addr2line', '-f', '-C', '-e', str(elf), f'{addr:#x}'],
                             capture_output=True, text=True).stdout.split('\n') if shutil.which(
            'arm-none-eabi-addr2line') else ['?']
        return out[0]

    config = ds_sample.CONFIG
    backup = config.with_suffix('.toml.ds_catch')
    shutil.copy(config, backup)
    text = config.read_text()
    text = ds_sample.set_option(text, 'JIT', 'Enable', 'false')
    text = ds_sample.set_option(text, 'Instance0.Gdb', 'Enabled', 'true')
    text = ds_sample.set_option(text, 'Instance0.Gdb.ARM9', 'Port', str(a.port))
    text = ds_sample.set_option(text, 'Instance0.Gdb.ARM9', 'BreakOnStartup', 'false')
    if a.sd_folder:
        text = ds_sample.set_option(text, 'DSi.SD', 'FolderPath', f'"{a.sd_folder.resolve()}"')
        text = ds_sample.set_option(text, 'DSi.SD', 'FolderSync', 'true')
        if a.sd_image:
            text = ds_sample.set_option(text, 'DSi.SD', 'ImagePath', f'"{a.sd_image.resolve()}"')
    config.write_text(text)
    xvfb = None
    if subprocess.run(['xdpyinfo', '-display', a.display], capture_output=True).returncode != 0:
        xvfb = subprocess.Popen(['Xvfb', a.display, '-screen', '0', '1024x768x24'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
    env = dict(os.environ, DISPLAY=a.display, XAUTHORITY='/dev/null')
    emu = subprocess.Popen(['flatpak', 'run', 'net.kuribo64.melonDS', str(rom)], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        stub = None
        for _ in range(60):
            time.sleep(1)
            try:
                stub = ds_sample.Stub(a.port)
                break
            except OSError:
                continue
        if not stub:
            sys.exit('no GDB stub')
        stub.sock.settimeout(a.timeout)
        stub.attach()
        bp = syms['defaultHandler'] & ~1
        stub.send(f'Z1,{bp:x},2')
        print('breakpoint:', stub.packet() or 'OK', flush=True)
        stub.resume()
        reply = stub.packet()  # stop reply when the handler is reached
        print('stopped:', reply, flush=True)
        stub.send(f'm{syms["exceptionRegisters"]:x},40')
        regs_hex = stub.packet()
        regs = [int.from_bytes(bytes.fromhex(regs_hex[i:i + 8]), 'little') for i in range(0, 128, 8)]
        names = [f'r{i}' for i in range(13)] + ['sp', 'lr', 'pc']
        for n, v in zip(names, regs):
            print(f'{n:>3} {v:08x}  {where(v) if 0x01000000 <= v < 0x03000000 else ""}')
        sp = regs[13]
        stub.send(f'm{sp:x},100')
        stack = stub.packet()
        print('\nstack words that look like code addresses:')
        for i in range(0, len(stack), 8):
            v = int.from_bytes(bytes.fromhex(stack[i:i + 8]), 'little')
            if 0x01000000 <= v < 0x02200000:
                print(f'  sp+{i // 2:#05x} {v:08x}  {where(v)}')
    finally:
        emu.terminate()
        for proc in Path('/proc').iterdir():
            if proc.name.isdigit():
                try:
                    cmd = proc.joinpath('cmdline').read_bytes().split(b'\0')
                    if cmd and cmd[0].endswith(b'melonDS') and os.fsencode(str(rom)) in cmd:
                        os.kill(int(proc.name), 9)
                except OSError:
                    pass
        if xvfb:
            xvfb.terminate()
        shutil.copy(backup, config)
        backup.unlink()


if __name__ == '__main__':
    main()
