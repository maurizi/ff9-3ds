#!/usr/bin/env python3
"""A local server for testing the web builder (python3 builder/web/test/serve.py DIR PORT): http.server with HTTP
range requests, which the page's test mode reads disc images through (?test=url,...: only on localhost)."""
import http.server
import os
import re
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def send_head(self):
        rng = self.headers.get('Range')
        path = self.translate_path(self.path)
        m = re.match(r'bytes=(\d+)-(\d*)$', rng or '')
        if not m or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        lo = int(m.group(1))
        hi = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
        f = open(path, 'rb')
        f.seek(lo)
        self.send_response(206)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Range', f'bytes {lo}-{hi}/{size}')
        self.send_header('Content-Length', str(hi - lo + 1))
        self.end_headers()
        self.range_left = hi - lo + 1
        return f

    def copyfile(self, source, outputfile):
        left = getattr(self, 'range_left', None)
        if left is None:
            return super().copyfile(source, outputfile)
        while left:
            b = source.read(min(left, 1 << 20))
            if not b:
                break
            outputfile.write(b)
            left -= len(b)
        self.range_left = None

    def log_message(self, *a):
        pass


if __name__ == '__main__':
    os.chdir(sys.argv[1])
    http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[2])), Handler).serve_forever()
