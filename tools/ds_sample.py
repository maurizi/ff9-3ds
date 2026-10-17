#!/usr/bin/env python3
"""Statistical ARM9 profile of a DS ROM in melonDS: sample the PC through melonDS's GDB stub.

Enables melonDS's ARM9 GDB stub (and the interpreter, which the stub needs) in a temporary configuration,
runs the ROM on a private display, waits until the game reaches --from (read from the live frame counter),
then interrupts it every --interval seconds, reads PC and LR, and continues, until --to. PCs are mapped to
the matching ELF's symbols. Samples follow host time, which tracks emulated time closely with the
interpreter. The melonDS configuration is restored afterwards.

  python3 tools/ds_sample.py --rom build/validation/x.nds --elf build/validation/x.elf --from 400 --to 650
"""
import argparse
import bisect
import collections
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CONFIG = Path.home() / '.var/app/net.kuribo64.melonDS/config/melonDS/melonDS.toml'


def set_option(text, section, key, value):
    start = text.index(f'[{section}]\n')
    end = text.find('\n[', start + 1)
    end = len(text) if end < 0 else end
    block = text[start:end]
    if re.search(rf'^{key} = .*$', block, re.M):
        block = re.sub(rf'^{key} = .*$', f'{key} = {value}', block, flags=re.M)
    else:
        block = block.rstrip('\n') + f'\n{key} = {value}\n'
    return text[:start] + block + text[end:]


class Stub:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buf = b''

    def send(self, payload):
        data = payload.encode()
        self.sock.sendall(b'$' + data + b'#' + f'{sum(data) & 255:02x}'.encode())
        self.ack()

    def ack(self):
        while True:
            c = self.read(1)
            if c == b'+':
                return
            if c == b'-':
                raise RuntimeError('stub NAK')

    def read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def packet(self):
        while self.read(1) != b'$':
            pass
        data = b''
        while True:
            c = self.read(1)
            if c == b'#':
                self.read(2)
                self.sock.sendall(b'+')
                return data.decode(errors='replace')
            data += c

    def attach(self):
        """melonDS answers only after the usual handshake; it halts the core when a client attaches."""
        self.sock.sendall(b'+')
        self.send('qSupported:swbreak+;hwbreak+')
        self.packet()
        self.send('?')
        self.packet()

    def interrupt(self):
        self.sock.sendall(b'\x03')
        while not self.packet().startswith(('S', 'T')):
            pass

    def register(self, n):
        self.send(f'p{n:x}')
        while True:  # an interrupt can produce a duplicate stop reply
            reply = self.packet()
            if re.fullmatch(r'[0-9a-fA-F]{8}', reply):
                return int.from_bytes(bytes.fromhex(reply), 'little')

    def resume(self):
        self.send('c')


def frames_reader(pid, sym):
    bases = []
    for row in Path(f'/proc/{pid}/maps').read_text().splitlines():
        parts = row.split()
        lo, hi = (int(x, 16) for x in parts[0].split('-'))
        if 'melondsfastmem' in row and parts[1].startswith('rw') and int(parts[2], 16) == 0 and hi - lo >= 1 << 24:
            bases.append(lo)
    mem = open(f'/proc/{pid}/mem', 'rb', buffering=0)

    def read():
        best = 0
        for base in bases:
            mem.seek(base + sym['frames'] - 0x02000000)
            v = int.from_bytes(mem.read(4), 'little')
            if v < 10 ** 7:
                best = max(best, v)
        return best
    return read if bases else None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--elf', type=Path, required=True)
    p.add_argument('--from', dest='start', type=int, required=True)
    p.add_argument('--to', dest='stop', type=int, required=True)
    p.add_argument('--interval', type=float, default=0.005)
    p.add_argument('--port', type=int, default=3333)
    p.add_argument('--display', default=':99')
    p.add_argument('--top', type=int, default=40)
    a = p.parse_args()
    rom, elf = a.rom.resolve(), a.elf.resolve()
    funcs = []
    for row in subprocess.check_output(['nm', '-C', '--defined-only', '-n', str(elf)], text=True).splitlines():
        parts = row.split(maxsplit=2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            funcs.append((int(parts[0], 16) & ~1, parts[2]))
    funcs.sort()
    addrs = [f[0] for f in funcs]
    sym = {name: addr for addr, name in funcs}
    for row in subprocess.check_output(['nm', '-a', str(elf)], text=True).splitlines():
        parts = row.split()
        if len(parts) == 3:
            sym.setdefault(parts[2], int(parts[0], 16))

    def name(pc):
        i = bisect.bisect_right(addrs, pc & ~1) - 1
        return funcs[i][1] if i >= 0 else f'{pc:08x}'

    backup = CONFIG.with_suffix('.toml.ds_sample')
    shutil.copy(CONFIG, backup)
    text = CONFIG.read_text()
    text = set_option(text, 'JIT', 'Enable', 'false')
    text = set_option(text, 'Instance0.Gdb', 'Enabled', 'true')
    text = set_option(text, 'Instance0.Gdb.ARM9', 'Port', str(a.port))
    text = set_option(text, 'Instance0.Gdb.ARM9', 'BreakOnStartup', 'false')
    CONFIG.write_text(text)
    xvfb = None
    if subprocess.run(['xdpyinfo', '-display', a.display], capture_output=True).returncode != 0:
        xvfb = subprocess.Popen(['Xvfb', a.display, '-screen', '0', '1024x768x24'],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
    env = dict(os.environ, DISPLAY=a.display, XAUTHORITY='/dev/null')
    emu = subprocess.Popen(['flatpak', 'run', 'net.kuribo64.melonDS', str(rom)], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples = collections.Counter()
    callers = collections.Counter()
    try:
        pid = None
        for _ in range(120):
            time.sleep(0.5)
            for proc in Path('/proc').iterdir():
                if proc.name.isdigit():
                    try:
                        cmd = proc.joinpath('cmdline').read_bytes().split(b'\0')
                    except OSError:
                        continue
                    if cmd and cmd[0].endswith(b'melonDS') and os.fsencode(str(rom)) in cmd:
                        pid = int(proc.name)
            if pid:
                reader = frames_reader(pid, sym)
                if reader:
                    break
        else:
            sys.exit('emulator did not start')
        while reader() < a.start:
            time.sleep(0.2)
        stub = Stub(a.port)
        stub.attach()
        stub.resume()
        print(f'sampling from frame {reader()}', flush=True)
        while True:
            time.sleep(a.interval)
            stub.interrupt()
            pc, lr = stub.register(15), stub.register(14)
            samples[name(pc)] += 1
            callers[(name(pc), name(lr))] += 1
            stub.resume()
            if sum(samples.values()) % 200 == 0 and reader() >= a.stop:
                break
        print(f'stopped at frame {reader()}')
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
        shutil.copy(backup, CONFIG)
        backup.unlink()
    total = sum(samples.values())
    print(f'{total} samples')
    for fn, count in samples.most_common(a.top):
        print(f'{100 * count / total:6.2f}%  {fn}')
    print('\nhottest (function <- caller):')
    for (fn, caller), count in callers.most_common(20):
        print(f'{100 * count / total:6.2f}%  {fn} <- {caller}')


if __name__ == '__main__':
    main()
