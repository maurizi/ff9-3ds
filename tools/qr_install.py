#!/usr/bin/env python3
"""Install a CIA on the 3DS by QR code: this computer serves the file on the local network, and FBI on the 3DS
(Remote Install > Scan QR Code) reads its address from the code and downloads it.

    tools/qr_install.py [build/builder-out/ff9-full.cia] [--port 5009] [--host 192.168.1.24]

The CIA holds data from the user's own discs, so it is only ever served here, on the local network, never hosted.
The code is printed in the terminal and written next to the file as <name>.qr.png (to show on screen). The server
serves that one file, and stops once it has been sent whole, or after --timeout seconds. Needs the segno package
(a local venv: python3 -m venv build/venv-qr && build/venv-qr/bin/pip install segno, then run it with
build/venv-qr/bin/python). The port must be open to the LAN in this computer's firewall: 5009 is the hardware harness's
(docs/hardware-harness.md), use it while that isn't running."""
import argparse
import http.server
import os
import socket
import sys
import threading
import time
from urllib.parse import quote


def lan_address():
    """this computer's address on the local network: the one its route to the internet goes out of"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # (a documentation address: nothing is sent, only the route chosen)
        return s.getsockname()[0]
    finally:
        s.close()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("cia", nargs="?", default="build/builder-out/ff9-full.cia")
    p.add_argument("--port", type=int, default=5009)
    p.add_argument("--host", help="the address the 3DS reaches this computer at (default: its LAN address)")
    p.add_argument("--timeout", type=int, default=3600, help="seconds to serve at most")
    a = p.parse_args()
    try:
        import segno
    except ImportError:
        sys.exit("qr_install.py: needs segno (python3 -m venv build/venv-qr && build/venv-qr/bin/pip install segno)")
    path = os.path.abspath(a.cia)
    if not os.path.isfile(path):
        sys.exit(f"qr_install.py: no {a.cia} (the builder writes it: --format cia)")
    name, size = os.path.basename(path), os.path.getsize(path)
    host = a.host or lan_address()
    url = f"http://{host}:{a.port}/{quote(name)}"
    done = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_HEAD(self):
            self.serve(body=False)

        def do_GET(self):
            self.serve(body=True)

        def serve(self, body):
            if self.path.split("?")[0] != "/" + quote(name):
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            if not body:
                return
            sent, last, t0 = 0, 0.0, time.time()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(1 << 20)
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        print(f"\n{self.client_address[0]} stopped at {sent >> 20} of {size >> 20} MB")
                        return
                    sent += len(chunk)
                    now = time.time()
                    if now - last >= 2:
                        last = now
                        rate = sent / max(now - t0, 0.001)
                        print(f"\r{self.client_address[0]}: {sent >> 20} of {size >> 20} MB, {rate / 1e6:.2f} MB/s, "
                              f"{(size - sent) / max(rate, 1) / 60:.0f} min left   ", end="", flush=True)
            print(f"\n{self.client_address[0]}: all {size >> 20} MB sent in {(time.time() - t0) / 60:.1f} min")
            done.set()

        def log_message(self, fmt, *args):
            pass

    server = http.server.ThreadingHTTPServer(("", a.port), Handler)
    qr = segno.make(url, error="m")
    png = os.path.splitext(path)[0] + ".qr.png"
    qr.save(png, scale=10, border=4)
    print(f"Serving {name} ({size >> 20} MB) at {url}")
    print("On the 3DS: FBI > Remote Install > Scan QR Code, then point the camera at this code")
    qr.terminal(compact=True)
    print(f"(also as {png})")
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        done.wait(a.timeout)
    except KeyboardInterrupt:
        pass
    server.shutdown()


if __name__ == "__main__":
    main()
