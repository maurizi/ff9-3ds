#!/usr/bin/env python3
"""Drive a 3DS running the test agent (make 3ds AGENT=<this PC's ip>; port/3ds/agent.c) from this PC:
docs/hardware-harness.md.

  tools/hwrun.py serve                      listen for the console and hand it the queued jobs (leave it running)
  tools/hwrun.py run --scenario disc3_world_airship --frames 4800 --press "CROSS@900+3000" \\
        --capture 2000-2120 --set ctr_slot_keep=0@2600 --out build/hw/airship
                                            queue a job, wait for it, write OUT/log.txt, OUT/top_f*.png, OUT/top.mp4
  tools/hwrun.py quit                       the console back to its launcher
  tools/hwrun.py status                     the console last heard of, the queue
  tools/hwrun.py decode capture.bin --out DIR   frames the console kept on its card (no connection at a job's end)
  tools/hwrun.py sim                        a stand-in for the console (the protocol, without one)

Python 3 standard library only; ffmpeg, if there, makes the .mp4.
"""
import argparse, array, glob, json, os, re, shutil, socket, struct, subprocess, sys, threading, time, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 5009
SILENT_S = 180  # a job's connection without a byte for this long: the console is taken for lost


# ---- the job's text (agent.c's header has the format) ----------------------------------------------------------

def elf_info(path):
    """.text's (address, bytes) and {name: [addresses]} of the 4-byte data objects"""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 1:
        raise SystemExit(f'{path}: not a 32-bit ELF')
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
    strs = secs[shstrndx]

    def name_at(table, off):
        start = table[4] + off
        return d[start:d.index(b'\0', start)].decode()
    text = None
    syms = {}
    for s in secs:
        if name_at(strs, s[0]) == '.text':
            text = (s[3], d[s[4]:s[4] + s[5]])
        if s[1] == 2:  # SHT_SYMTAB
            strtab = secs[s[6]]
            for o in range(s[4], s[4] + s[5], 16):
                n, value, size, info, _, shndx = struct.unpack_from('<IIIBBH', d, o)
                if info & 15 == 1 and size == 4 and shndx:
                    syms.setdefault(name_at(strtab, n), set()).add(value)
    return text, syms


def parse_capture(spec, frames):
    """A-B[/STEP][,bottom|both][,half|third|quarter] or /STEP for the whole run -> the job's capture line"""
    parts = spec.split(',')
    m = re.fullmatch(r'(?:(\d+)(?:-(\d+))?)?(?:/(\d+))?', parts[0])
    if not m or not parts[0]:
        raise SystemExit(f'--capture {spec}: A-B[/STEP][,bottom|both][,half]')
    step = int(m.group(3) or 1)
    a = int(m.group(1)) if m.group(1) else step
    b = int(m.group(2)) if m.group(2) else (a if m.group(1) else frames)
    screens, scale = 1, 1
    for p in parts[1:]:
        if p in ('top', 'bottom', 'both'):
            screens = {'top': 1, 'bottom': 2, 'both': 3}[p]
        elif p in ('half', 'third', 'quarter', 'eighth'):  # (eighth: 50x30 thumbnails, next to no time a frame)
            scale = {'half': 2, 'third': 3, 'quarter': 4, 'eighth': 8}[p]
        else:
            raise SystemExit(f'--capture {spec}: unknown option {p}')
    return f'capture {a} {b} {step} {scale} {screens}'


def job_text(args):
    lines = [f'frames {args.frames}', f'hang {args.hang}']
    if args.bufkb:
        lines.append(f'bufkb {args.bufkb}')
    for p in args.press or []:
        lines.append('press ' + ','.join(p.replace(',', ' ').split()))
    if args.touch:
        lines.append('touch ' + ' '.join(args.touch))
    for c in args.capture or []:
        lines.append(parse_capture(c, args.frames))
    if args.set:
        elf = args.elf or next((p for p in (os.path.join(ROOT, 'build/ff9-agent.elf'), os.path.join(ROOT, 'build/3ds/ff9ds.elf'))
                                if os.path.exists(p)), None)
        text, syms = elf_info(elf) if elf and elf != 'none' else (None, {})
        if text:  # (the console checks its own .text against this before it takes an address)
            lines.append(f'check {text[0]:x} {len(text[1]):x} {zlib.adler32(text[1]):x}')
        for s in args.set:
            m = re.fullmatch(r'(\w+)=(-?\w+)(?:@(\d+))?', s)
            if not m:
                raise SystemExit(f'--set {s}: NAME=VALUE[@FRAME]')
            name, value, frame = m.group(1), int(m.group(2), 0), int(m.group(3) or 0)
            addrs = syms.get(name, ())
            if len(addrs) > 1:
                raise SystemExit(f'--set {name}: {len(addrs)} variables of that name in {elf}')
            lines.append(f'set {name} {value} {frame}' + ''.join(f' @{a:x}' for a in addrs))
    if args.scenario:
        path = args.scenario
        if not os.path.exists(path):
            path = os.path.join(ROOT, 'tests/scenarios', args.scenario + ('' if args.scenario.endswith('.cfg') else '.cfg'))
        for l in open(path):
            l = l.strip()
            if l and not l.startswith('#'):
                lines.append('cfg ' + l)
    return '\n'.join(lines) + '\n'


# ---- frames ------------------------------------------------------------------------------------------------------

_lut = None


def write_png(path, w, h, rows):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def decode_frames(data, out, log=print):
    """the capture stream -> OUT/top_fNNNNNN.png, bot_fNNNNNN.png, frames.csv; returns the rows of frames.csv"""
    global _lut
    if _lut is None:
        _lut = [bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)) for v in range(65536)]
    state = {}
    rows_csv = []
    pos = 0
    while pos + 24 <= len(data):
        magic, frame, serial, screen, scale, _, _, cols, rows, n = struct.unpack_from('<4sIIBBBBHHI', data, pos)
        if magic != b'FRM1' or pos + 24 + n > len(data):
            log(f'frames: stream broken at byte {pos} ({len(rows_csv)} frames read)')
            break
        tok = array.array('H')
        tok.frombytes(data[pos + 24:pos + 24 + n])
        if sys.byteorder == 'big':
            tok.byteswap()
        pos += 24 + n
        key = (screen, cols, rows)
        cur = state.get(key)
        if cur is None:
            cur = state[key] = array.array('H', bytes(2 * cols * rows))
        i = p = changed = 0
        while p < len(tok):
            skip, cnt = tok[p], tok[p + 1]
            i += skip
            cur[i:i + cnt] = tok[p + 2:p + 2 + cnt]
            i += cnt
            p += 2 + cnt
            changed += cnt
        # the buffer is in columns, each from the screen's bottom up
        name = f'{"bot" if screen else "top"}_f{frame:06d}.png'
        lut = _lut
        write_png(os.path.join(out, name), cols, rows,
                  [b''.join([lut[v] for v in cur[rows - 1 - y::rows]]) for y in range(rows)])
        rows_csv.append((('bottom' if screen else 'top'), frame, serial, changed, name))
    with open(os.path.join(out, 'frames.csv'), 'w') as f:
        f.write('screen,frame,present,changed_pixels,file\n')
        for r in rows_csv:
            f.write(','.join(map(str, r)) + '\n')
    return rows_csv


def make_video(out, prefix, rows, log=print):
    frames = sorted(r[1] for r in rows)
    if len(frames) < 2 or not shutil.which('ffmpeg'):
        return
    gaps = sorted(b - a for a, b in zip(frames, frames[1:]))
    rate = max(1.0, 60.0 / max(1, gaps[len(gaps) // 2]))
    r = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', f'{rate:g}', '-pattern_type', 'glob', '-i',
                        os.path.join(out, prefix + '_f*.png'), '-vf', 'scale=iw*2:ih*2:flags=neighbor', '-c:v', 'libx264',
                        '-crf', '12', '-pix_fmt', 'yuv420p', os.path.join(out, prefix + '.mp4')],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode:
        log(f'ffmpeg: {r.stderr.strip()[:200]}')


def finish_frames(out, log=print):
    path = os.path.join(out, 'frames.bin')
    if not os.path.exists(path):
        return 0
    rows = decode_frames(open(path, 'rb').read(), out, log)
    for prefix, screen in (('top', 'top'), ('bot', 'bottom')):
        make_video(out, prefix, [r for r in rows if r[0] == screen], log)
    return len(rows)


# ---- the server --------------------------------------------------------------------------------------------------

class State:
    def __init__(self, path):
        self.dir = path
        self.queue = os.path.join(path, 'queue')
        os.makedirs(self.queue, exist_ok=True)
        self.lock = threading.Lock()

    def write(self, name, obj):
        tmp = os.path.join(self.dir, name + '.tmp')
        with open(tmp, 'w') as f:
            json.dump(obj, f)
        os.replace(tmp, os.path.join(self.dir, name))

    def read(self, name):
        try:
            return json.load(open(os.path.join(self.dir, name)))
        except (OSError, ValueError):
            return None

    def take(self):
        with self.lock:
            for p in sorted(glob.glob(os.path.join(self.queue, '*.json'))):
                try:
                    job = json.load(open(p))
                    os.remove(p)
                    return job
                except (OSError, ValueError):
                    continue
        return None


def write_status(out, status):
    tmp = os.path.join(out, 'status.json.tmp')
    with open(tmp, 'w') as f:
        json.dump(status, f, indent=1)
    os.replace(tmp, os.path.join(out, 'status.json'))


def recv_exact(conn, n):
    buf = bytearray()
    while len(buf) < n:
        d = conn.recv(min(1 << 16, n - len(buf)))
        if not d:
            return None
        buf += d
    return bytes(buf)


def handle(conn, addr, st):
    def log(msg):
        print(f'[{time.strftime("%H:%M:%S")}] {addr[0]}: {msg}', flush=True)
    try:
        conn.settimeout(10)
        hello = b''
        while not hello.endswith(b'\n') and len(hello) < 256:
            d = conn.recv(1)
            if not d:
                return
            hello += d
        words = hello.decode(errors='replace').split()
        if words[:2] != ['HELLO', '1']:
            log(f'not a console: {hello[:40]!r}')
            return
        info = dict(w.split('=', 1) for w in words[2:] if '=' in w)
        info['addr'] = addr[0]
        log('console ' + ' '.join(f'{k}={v}' for k, v in info.items()))
        while True:
            info['seen'] = time.time()
            st.write('console.json', info)
            job = st.take()
            if job:
                break
            conn.sendall(b'WAIT\n')  # (raises once the console is gone)
            time.sleep(2)
        if job.get('quit'):
            conn.sendall(b'QUIT\n')
            log('quit sent')
            return
        if job.get('put'):  # (a file for the console's card: a new agent build)
            import zlib
            data = open(job['put'], 'rb').read()
            conn.settimeout(600)  # (sendall's timeout is for the whole file: minutes over an Old 3DS's Wi-Fi)
            conn.sendall(f'PUT {job["path"]} {len(data)} {zlib.adler32(data) & 0xffffffff:x}\n'.encode() + data)
            conn.settimeout(60)
            reply = b''
            try:
                while True:
                    b = conn.recv(4096)
                    if not b:
                        break
                    reply += b
            except OSError:
                pass
            log(f'put {job["put"]} -> {job["path"]}: {reply[-80:]!r}')
            return
        out = job['out']
        text = job['text'].encode()
        try:
            conn.sendall(f'JOB {job["id"]} {len(text)}\n'.encode() + text)
        except OSError:
            enqueue(st, job)  # (the console had gone: the job back for the next one)
            raise
        log(f'job {job["id"]} sent -> {out}')
        status = {'id': job['id'], 'status': 'lost', 'console': info, 'started': time.time()}
        conn.settimeout(SILENT_S)
        nframes = 0
        with open(os.path.join(out, 'log.txt'), 'ab') as flog:
            try:
                while True:
                    h = recv_exact(conn, 5)
                    if not h:
                        status['reason'] = 'the connection closed before the job ended (a crash, or power)'
                        break
                    kind, n = chr(h[0]), struct.unpack_from('<I', h, 1)[0]
                    data = recv_exact(conn, n) if n else b''
                    if data is None:
                        status['reason'] = 'the connection closed inside a chunk'
                        break
                    if kind == 'L':
                        flog.write(data)
                        flog.flush()
                    elif kind == 'F':
                        with open(os.path.join(out, 'frames.bin'), 'ab') as f:
                            f.write(data)
                        nframes += len(data)
                    elif kind == 'E':
                        end = dict(w.split('=', 1) for w in data.decode(errors='replace').split(' ') if '=' in w)
                        if end.get('status') == 'refused':
                            end['reason'] = data.decode(errors='replace').split('reason=', 1)[-1]
                        status.update(end)
                        break
            except socket.timeout:
                status['reason'] = f'nothing from the console for {SILENT_S} s'
            except OSError as e:
                status['reason'] = f'connection error: {e}'
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        status['frames_decoded'] = finish_frames(out, log)
        status['seconds'] = round(time.time() - status.pop('started'), 1)
        write_status(out, status)
        log(f'job {job["id"]}: {status["status"]}' + (f' ({status["reason"]})' if 'reason' in status else '') +
            f', {status["frames_decoded"]} frames, {status["seconds"]} s')
    except (OSError, ValueError) as e:
        log(f'connection ended: {e}')
    finally:
        conn.close()


def cmd_serve(args):
    st = State(args.state)
    srv = socket.create_server((args.bind, args.port))
    print(f'hwrun: listening on {args.bind or "*"}:{args.port}, state in {args.state}', flush=True)

    def beat():
        while True:
            st.write('serve.json', {'pid': os.getpid(), 'port': args.port, 'time': time.time()})
            time.sleep(2)
    threading.Thread(target=beat, daemon=True).start()
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr, st), daemon=True).start()


# ---- the client commands -------------------------------------------------------------------------------------------

def server_note(st):
    s = st.read('serve.json')
    if not s or time.time() - s['time'] > 10:
        print('hwrun: no server running (start `tools/hwrun.py serve` and leave it up)', file=sys.stderr)
    c = st.read('console.json')
    if not c or time.time() - c['seen'] > 15:
        print('hwrun: no console waiting right now (the job stays queued until one asks)', file=sys.stderr)


def enqueue(st, job):
    name = f'{time.time():017.6f}-{job["id"]}.json'
    tmp = os.path.join(st.dir, name + '.tmp')
    with open(tmp, 'w') as f:
        json.dump(job, f)
    os.replace(tmp, os.path.join(st.queue, name))
    return os.path.join(st.queue, name)


def summarize(out, status):
    print(f'status: {status.get("status")}' + (f' ({status["reason"]})' if 'reason' in status else ''))
    for k in ('frame', 'captured', 'capture_ms', 'send_ms', 'relaunch', 'frames_decoded', 'seconds'):
        if k in status:
            print(f'  {k}: {status[k]}')
    try:
        log = open(os.path.join(out, 'log.txt'), errors='replace').read()
    except OSError:
        log = ''
    beats = sorted(int(m) for m in re.findall(r'^beat \d+: (\d+) ms', log, re.M) if int(m))
    if beats:
        print(f'  beats (ms per 120 frames; 2006 is full speed): min {beats[0]} median {beats[len(beats) // 2]} max {beats[-1]}'
              f' ({len(beats)})')
    for l in re.findall(r'^.*(?:FATAL|WATCHDOG).*$', log, re.M)[:5]:
        print('  ' + l)
    if status.get('relaunch') == '0':
        print('  the console could not start itself again: launch the agent again for the next job')
    print(f'  {out}/log.txt' + (f', {status["frames_decoded"]} frames (frames.csv)' if status.get('frames_decoded') else ''))


def cmd_run(args):
    st = State(args.state)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for p in glob.glob(os.path.join(out, '*_f*.png')) + [os.path.join(out, n) for n in
                                                         ('status.json', 'log.txt', 'frames.bin', 'frames.csv', 'top.mp4', 'bot.mp4')]:
        if os.path.exists(p):
            os.remove(p)
    text = job_text(args)
    job = {'id': f'{int(time.time()) % 1000000:06d}', 'out': out, 'text': text}
    with open(os.path.join(out, 'job.txt'), 'w') as f:
        f.write(text)
    server_note(st)
    queued = enqueue(st, job)
    print(f'hwrun: job {job["id"]} queued', flush=True)
    if args.no_wait:
        return 0
    deadline = time.time() + (args.timeout or args.frames / 10 + 300)
    done = os.path.join(out, 'status.json')
    try:
        while not os.path.exists(done):
            if time.time() > deadline:
                if os.path.exists(queued):
                    os.remove(queued)
                    print('hwrun: timed out: no console took the job (removed from the queue)', file=sys.stderr)
                else:
                    print(f'hwrun: timed out waiting for the result (the log so far: {out}/log.txt)', file=sys.stderr)
                return 2
            time.sleep(0.5)
    except KeyboardInterrupt:
        if os.path.exists(queued):
            os.remove(queued)
        return 130
    status = json.load(open(done))
    summarize(out, status)
    return 0 if status.get('status') == 'done' else 1


def cmd_quit(args):
    st = State(args.state)
    server_note(st)
    enqueue(st, {'id': 'quit', 'quit': True})
    print('hwrun: quit queued')
    return 0


def cmd_update(args):
    """A new agent build onto the console's card over the harness's own connection; the console starts it."""
    st = State(args.state)
    server_note(st)
    enqueue(st, {'id': 'put', 'put': os.path.abspath(args.file), 'path': args.path})
    print(f'hwrun: {args.file} -> {args.path} queued')
    return 0


def cmd_status(args):
    st = State(args.state)
    s, c = st.read('serve.json'), st.read('console.json')
    now = time.time()
    print('server: ' + (f'pid {s["pid"]} port {s["port"]}' if s and now - s['time'] < 10 else 'not running'))
    if c:
        print(f'console: {c.get("addr")} model={c.get("model")} relaunch={c.get("relaunch")} heap={c.get("heap")} KB'
              f' linear={c.get("linear")} KB, heard {now - c["seen"]:.0f} s ago' + (' (waiting for a job)' if now - c['seen'] < 6 else ''))
    else:
        print('console: never heard of')
    q = sorted(glob.glob(os.path.join(st.queue, '*.json')))
    print(f'queue: {len(q)} job(s)')
    return 0


def cmd_decode(args):
    os.makedirs(args.out, exist_ok=True)
    shutil.copyfile(args.file, os.path.join(args.out, 'frames.bin'))
    print(f'{finish_frames(args.out)} frames in {args.out}')
    return 0


# ---- a stand-in for the console ----------------------------------------------------------------------------------

def encode_frame(fb, prev, frame, serial, screen, cols, rows):
    """as agent.c's encode(), simply: one run per stretch of changed pixels"""
    tok = array.array('H')
    i, n = 0, cols * rows
    while i < n:
        j = i
        while j < n and fb[j] == prev[j] and j - i < 0xFFFF:
            j += 1
        k = j
        while k < n and fb[k] != prev[k] and k - j < 0xFFFF:
            k += 1
        if k == j and j == n:
            break
        tok.append(j - i)
        tok.append(k - j)
        tok.extend(fb[j:k])
        i = k
    if sys.byteorder == 'big':
        tok.byteswap()
    data = tok.tobytes()
    return struct.pack('<4sIIBBBBHHI', b'FRM1', frame, serial, screen, 1, 0, 0, cols, rows, len(data)) + data


def cmd_sim(args):
    done = 0
    while args.jobs == 0 or done < args.jobs:
        try:
            conn = socket.create_connection((args.host, args.port), timeout=20)
        except OSError as e:
            print(f'sim: no server ({e})')
            return 1
        conn.sendall(b'HELLO 1 model=sim relaunch=1 heap=0 linear=0\n')
        f = conn.makefile('rb')
        line = f.readline().decode().strip()
        while line == 'WAIT':
            line = f.readline().decode().strip()
        if line == 'QUIT' or not line:
            print('sim: quit')
            return 0
        _, jid, n = line.split()
        text = f.read(int(n)).decode()
        frames, caps = 600, []
        for l in text.splitlines():
            w = l.split()
            if w[0] == 'frames':
                frames = int(w[1])
            elif w[0] == 'capture':
                caps.append(tuple(map(int, w[1:6])))

        def chunk(kind, data):
            conn.sendall(kind + struct.pack('<I', len(data)) + data)
        prev = {0: array.array('H', bytes(2 * 400 * 240)), 1: array.array('H', bytes(2 * 320 * 240))}
        log, cap = f'sim: job {jid}\n', b''
        ncap = 0
        for fr in range(1, frames + 1):
            if fr % 120 == 0:
                chunk(b'L', (log + f'beat {fr}: 2006 ms, mode 1 field 0\n').encode())
                log = ''
            for a, b, step, scale, screens in caps:
                if a <= fr <= b and (fr - a) % step == 0:
                    for scr, cols in ((0, 400), (1, 320)):
                        if screens & (1 << scr):
                            fb = array.array('H', bytes(2 * cols * 240))  # a white bar moving right over black
                            x = fr % cols
                            fb[x * 240:(x + 1) * 240] = array.array('H', [0xFFFF] * 240)
                            fb[0] = 0xF800  # (the screen's bottom left red)
                            cap += encode_frame(fb, prev[scr], fr, fr, scr, cols, 240)
                            prev[scr] = fb
                            ncap += 1
                    break
        if cap:
            chunk(b'F', cap)
        chunk(b'L', (log + f'agent: job {jid} done\n').encode())
        chunk(b'E', f'status=done frame={frames} captured={ncap} capture_ms=0 send_ms=0 relaunch=1'.encode())
        conn.close()
        done += 1
        print(f'sim: job {jid} done ({frames} frames, {ncap} captured)')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--state', default=os.path.join(ROOT, 'build/hwrun'), help='the queue and the server\'s notes (build/hwrun)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('serve')
    p.add_argument('--port', type=int, default=PORT)
    p.add_argument('--bind', default='')
    p = sub.add_parser('run')
    p.add_argument('--scenario', help='a name in tests/scenarios or a .cfg file (none: the title screen)')
    p.add_argument('--frames', type=int, default=1800, help='virtual frames to run (60 a second at full speed)')
    p.add_argument('--press', action='append', help='"CROSS@900+3000,UP@1000" (BUTTON@FRAME[+FRAMES])')
    p.add_argument('--touch', action='append', help='"X,Y@FRAME" taps, "X,Y@FRAME+N" holds, "X,Y>X2,Y2@FRAME+N" drags')
    p.add_argument('--capture', action='append', metavar='A-B[/STEP][,bottom|both][,half]',
                   help='frames to capture: 2000-2120, 2000-2120/4, /60 (every 60th of the run); the top screen unless said')
    p.add_argument('--set', action='append', metavar='NAME=VALUE[@FRAME]', help='an int of the build set at that frame (0: before the game starts)')
    p.add_argument('--elf', help='the agent build\'s ELF, for --set names it has no switch for (build/ff9-agent.elf, else build/3ds/ff9ds.elf; "none")')
    p.add_argument('--bufkb', type=int, help='the console\'s capture buffer in KB (3072)')
    p.add_argument('--hang', type=int, default=30, help='seconds without a frame that end the job as hung (0: never)')
    p.add_argument('--out', required=True)
    p.add_argument('--timeout', type=float, help='seconds to wait for the result (frames/10 + 300)')
    p.add_argument('--no-wait', action='store_true')
    sub.add_parser('quit')
    p = sub.add_parser('update')
    p.add_argument('file', nargs='?', default='build/ff9-agent.3dsx')
    p.add_argument('--path', default='sdmc:/3ds/ff9-agent.3dsx')
    sub.add_parser('status')
    p = sub.add_parser('decode')
    p.add_argument('file')
    p.add_argument('--out', required=True)
    p = sub.add_parser('sim')
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--port', type=int, default=PORT)
    p.add_argument('--jobs', type=int, default=0, help='stop after this many (0: until quit)')
    args = ap.parse_args()
    return {'serve': cmd_serve, 'run': cmd_run, 'quit': cmd_quit, 'update': cmd_update, 'status': cmd_status, 'decode': cmd_decode,
            'sim': cmd_sim}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
