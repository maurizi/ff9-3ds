/* The web builder's worker: Pyodide with the builder (ff9builder, the vendored tools, the program), run on the user's
 * disc files. Nothing leaves the browser: the files are read where they are (WORKERFS), the large outputs are written
 * to the page's private storage (OPFS, bigfs.js) and handed back to the page by name.
 *
 * Messages in:  {type: 'identify', files: [File]}       -> {type: 'identified', results: [...]}
 *               {type: 'build', names: [...], fmt}       -> {type: 'log' | 'progress', ...}, then {type: 'done', file}
 * Anything that fails: {type: 'error', message}. */
import { loadPyodide } from './pyodide/pyodide.mjs';
import { makeBigFS } from './bigfs.js';

const POOL = 8; // large files open at once at most (the pack's two pools and itself, the outputs)
let py = null, web = null, mounted = false, loading = null;

const bytes = async (url) => new Uint8Array(await (await fetch(url)).arrayBuffer());

function init() { return loading || (loading = load()); } // (once, however many messages wait on it)

async function load() {
  post({ type: 'progress', fraction: 0, message: 'Loading the builder (Python in the browser)' });
  py = await loadPyodide({ indexURL: 'pyodide/' });
  await py.loadPackage(['pyyaml', 'tqdm', 'sortedcontainers'], { messageCallback: () => {} });
  const site = py.runPython('import site; site.getsitepackages()[0]');
  const wheels = await (await fetch('wheels/wheels.json')).json();
  for (const w of wheels) py.unpackArchive(await bytes('wheels/' + w), 'wheel', { extractDir: site });
  py.unpackArchive(await bytes('bundle.zip'), 'zip', { extractDir: '/app' });
  py.runPython(new TextDecoder().decode(await bytes('n64_stubs.py')));
  py.runPython("import sys, importlib; sys.path.insert(0, '/app'); importlib.invalidate_caches()");

  // the large files' storage: OPFS files of a folder of our own, emptied of a previous run's
  const root = await navigator.storage.getDirectory();
  await root.removeEntry('ff9-builder', { recursive: true }).catch(() => {});
  const dir = await root.getDirectoryHandle('ff9-builder', { create: true });
  const pool = [];
  for (let i = 0; i < POOL; i++) {
    const h = await (await dir.getFileHandle('f' + i, { create: true })).createSyncAccessHandle();
    h.opfsName = 'f' + i;
    pool.push(h);
  }
  self.pool = pool;
  py.FS.mkdir('/big');
  self.big = makeBigFS(py.FS, pool);
  py.FS.mount(self.big, {}, '/big');
  web = py.pyimport('ff9builder.web');
}

function post(m) { self.postMessage(m); }

function mountDiscs(files, urls) {
  if (mounted) py.FS.unmount('/discs');
  else py.FS.mkdir('/discs');
  if (urls) py.FS.mount(makeUrlFS(py.FS, urls), {}, '/discs');
  else py.FS.mount(py.FS.filesystems.WORKERFS, { files }, '/discs');
  mounted = true;
}

/* Local testing only (the page's ?test= on localhost): the disc files read on demand from URLs, with synchronous
 * range requests (a worker may make them), where a test can't pick files in a dialog. Reads aren't cached: the
 * images are gigabytes. */
function makeUrlFS(FS, urls) {
  const MEMFS = FS.filesystems.MEMFS;
  const files = urls.map((u) => {
    const x = new XMLHttpRequest();
    x.open('HEAD', u, false);
    x.send();
    return { url: u, name: decodeURIComponent(u.split('/').pop()), size: +x.getResponseHeader('Content-Length') };
  });
  const fileNode = {
    getattr(node) { const a = MEMFS.node_ops.getattr(node); a.size = node.url.size; a.blksize = 1 << 22; return a; },
    setattr() {},
  };
  const fileStream = {
    read(stream, buffer, offset, length, position) {
      const f = stream.node.url;
      if (position >= f.size || !length) return 0;
      const x = new XMLHttpRequest();
      x.open('GET', f.url, false);
      x.responseType = 'arraybuffer';
      x.setRequestHeader('Range', `bytes=${position}-${Math.min(f.size, position + length) - 1}`);
      x.send();
      const b = new Uint8Array(x.response);
      buffer.set(b, offset);
      return b.length;
    },
    llseek(stream, offset, whence) {
      return offset + (whence === 1 ? stream.position : whence === 2 ? stream.node.url.size : 0);
    },
  };
  return {
    mount() {
      const root = MEMFS.createNode(null, '/', 0o40555, 0);
      for (const f of files) {
        const n = MEMFS.createNode(root, f.name, 0o100444, 0);
        n.node_ops = fileNode;
        n.stream_ops = fileStream;
        n.url = f;
      }
      return root;
    },
  };
}

self.onmessage = async (e) => {
  const m = e.data;
  try {
    await init();
    if (m.type === 'identify') {
      mountDiscs(m.files, m.urls);
      const names = m.urls ? m.urls.map((u) => decodeURIComponent(u.split('/').pop())) : m.files.map((f) => f.name);
      const r = web.identify(py.toPy(names));
      post({ type: 'identified', results: r.toJs({ dict_converter: Object.fromEntries }) });
      r.destroy();
    } else if (m.type === 'build') {
      const log = (s) => post({ type: 'log', message: s });
      const report = (f, msg) => post({ type: 'progress', fraction: f, message: msg === undefined ? null : msg });
      const out = web.build(py.toPy(m.names), m.fmt, log, report);
      // the output's OPFS file, closed (and every other handle: this worker is done) so the page can read it
      const h = self.big.handleOf(out);
      const name = h.opfsName;
      for (const p of self.pool) { p.flush(); p.close(); }
      post({ type: 'done', file: name, name: out.split('/').pop() });
    }
  } catch (err) {
    post({ type: 'error', message: String(err && err.message || err) });
  }
};
