#!/usr/bin/env python3
"""Sampling profiler over a GDB remote stub (Azahar's: tools/azahar_prof.sh).

    rsp_sample.py [--port 24689] [--warmup 40] [--samples 2000] [--period 0.01] --elf build/3ds/ff9ds.elf

Lets the target run, then repeatedly interrupts it, reads PC and LR and continues; prints the functions the
samples fell in (by PC: self time) and their callers (by LR), using the ELF's symbol table (nm)."""
import argparse, bisect, collections, socket, subprocess, sys, time


class Rsp:
    def __init__(self, port):
        for _ in range(200):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.25)
        else:
            sys.exit("no stub on port %d" % port)
        self.buf = b""

    def _read_packet(self):
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i + 1) if i >= 0 else -1
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                pkt = self.buf[i + 1:j]
                self.buf = self.buf[j + 3:]
                self.s.sendall(b"+")
                return pkt.decode("latin1")
            self.buf += self.s.recv(65536)

    def send(self, body, reply=True):
        ck = sum(body.encode()) & 0xFF
        self.s.sendall(b"$%s#%02x" % (body.encode(), ck))
        return self._read_packet() if reply else None

    def interrupt(self):
        self.s.sendall(b"\x03")
        return self._read_packet()

    def reg(self, n):
        r = self.send("p%x" % n)
        return int.from_bytes(bytes.fromhex(r[:8]), "little")


def symbols(elf):
    out = subprocess.run(["nm", "-n", "-C", elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split(maxsplit=2)
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=24689)
    ap.add_argument("--warmup", type=float, default=40)
    ap.add_argument("--samples", type=int, default=2000)
    ap.add_argument("--period", type=float, default=0.01)
    ap.add_argument("--elf", default="build/3ds/ff9ds.elf")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--duration", type=float, default=60, help="seconds of sampling at most")
    a = ap.parse_args()
    addrs, names = symbols(a.elf)

    def name(x):
        i = bisect.bisect_right(addrs, x & ~1) - 1
        return names[i] if i >= 0 else "?%08x" % x

    r = Rsp(a.port)
    r.send("?")
    r.send("c", reply=False)
    time.sleep(a.warmup)
    self_, callers, threads = collections.Counter(), collections.Counter(), collections.Counter()
    got = 0
    end = time.time() + a.duration
    for _ in range(a.samples):
        if time.time() > end:
            break
        stop = r.interrupt()
        while not stop.startswith("T"):  # (anything queued before the stop reply)
            stop = r._read_packet()
        regs = dict(f.split(":", 1) for f in stop[3:].split(";") if ":" in f)
        le = lambda h: int.from_bytes(bytes.fromhex(h[:8]), "little")
        pc, lr = le(regs.get("0f", "00000000")), le(regs.get("0e", "00000000"))
        if pc:
            f = name(pc)
            self_[f] += 1
            callers[f + "  <-  " + name(lr)] += 1
            got += 1
        r.send("c", reply=False)
        time.sleep(a.period)
    r.interrupt()
    r.send("D")
    print("%d samples" % got)
    print("-- self --")
    for f, c in self_.most_common(a.top):
        print("%5.1f%%  %s" % (100.0 * c / got, f))
    print("-- with caller (by LR; leaf functions only are reliable) --")
    for f, c in callers.most_common(a.top):
        print("%5.1f%%  %s" % (100.0 * c / got, f))


if __name__ == "__main__":
    main()
