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
    p.add_argument('--break', dest='brk', default='defaultHandler', help='function to stop at')
    p.add_argument('--watch', type=lambda v: int(v, 16), help='hex address: report each write (PC/LR) instead')
    p.add_argument('--hits', type=int, default=5)
    p.add_argument('--args', action='store_true', help='with --break: print r0-r3 and LR at each of --hits stops '
                   'instead of the exception registers (a call trace of that function)')
    p.add_argument('--dump-r0', type=int, default=0, help='with --args: also print this many bytes at r0')
    p.add_argument('--dump', action='append', default=[], metavar='SYMBOL:BYTES',
                   help='with --args: also print bytes at a symbol at each stop (repeatable)')
    p.add_argument('--regs', default='exceptionRegisters', help='saved r0-r15 array to print (ITCMGUARD builds: '
                   'port_itcm_first_fault, whose r0 is at +4)')
    a = p.parse_args()
    rom, elf = a.rom.resolve(), a.elf.resolve()
    syms = {}
    for row in subprocess.check_output(['nm', str(elf)], text=True).splitlines():
        parts = row.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    for need in (a.brk, a.regs):
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
        if a.watch is not None:
            stub.send(f'Z2,{a.watch:x},4')
            print('watchpoint:', stub.packet() or 'OK', flush=True)
            for hit in range(a.hits):
                stub.resume()
                print(f'hit {hit}: {stub.packet()}', flush=True)
                pc, lr, sp = stub.register(15), stub.register(14), stub.register(13)
                stub.send(f'm{a.watch:x},4')
                value = int.from_bytes(bytes.fromhex(stub.packet()), 'little')
                print(f'  value {value:08x}  pc {pc:08x} {where(pc)}  lr {lr:08x} {where(lr)}  sp {sp:08x}', flush=True)
            return
        bp = syms[a.brk] & ~1
        stub.send(f'Z1,{bp:x},2')
        print('breakpoint:', stub.packet() or 'OK', flush=True)
        if a.args:
            for hit in range(a.hits):
                stub.resume()
                stub.packet()
                r = [stub.register(i) for i in range(4)]
                lr = stub.register(14)
                print(f'hit {hit}: r0 {r[0]:08x} r1 {r[1]:08x} r2 {r[2]:08x} r3 {r[3]:08x}  lr {lr:08x} {where(lr)}',
                      flush=True)
                for spec in a.dump:
                    sym, count = spec.rsplit(':', 1)
                    stub.send(f'm{syms[sym]:x},{int(count, 0):x}')
                    raw = bytes.fromhex(stub.packet())
                    print(f'  {sym}: ' + ' '.join(f'{int.from_bytes(raw[j:j + 4], "little"):08x}'
                                                  for j in range(0, len(raw), 4)), flush=True)
                if a.dump_r0:
                    stub.send(f'm{r[0]:x},{a.dump_r0:x}')
                    raw = bytes.fromhex(stub.packet())
                    for i in range(0, len(raw), 16):
                        print(f'  {r[0] + i:08x} ' + ' '.join(f'{int.from_bytes(raw[j:j + 4], "little"):08x}'
                                                             for j in range(i, min(i + 16, len(raw)), 4)), flush=True)
            return
        stub.resume()
        reply = stub.packet()  # stop reply when the handler is reached
        print('stopped:', reply, flush=True)
        regs_at = syms[a.regs] + (4 if a.regs == 'port_itcm_first_fault' else 0)
        stub.send(f'm{regs_at:x},40')
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
