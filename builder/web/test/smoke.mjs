// The assembled web builder, checked in Node without a disc (CI): Pyodide and every wheel load from the site's own
// files, splat (with the N64 stand-ins), the builder and its vendored tools import, the program is in the bundle, and
// rabbitizer returns addresses above 2^31 unsigned (its wasm32 build's patch: else the split would differ).
//   node builder/web/test/smoke.mjs SITE_DIR
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const site = path.resolve(process.argv[2]);
const { loadPyodide } = await import(pathToFileURL(path.join(site, 'pyodide/pyodide.mjs')).href);
const py = await loadPyodide({ indexURL: path.join(site, 'pyodide') + '/' });
const bytes = (f) => new Uint8Array(fs.readFileSync(path.join(site, f)));
await py.loadPackage(['pyyaml', 'tqdm', 'sortedcontainers'].map((n) => {
  const lock = JSON.parse(fs.readFileSync(path.join(site, 'pyodide/pyodide-lock.json'), 'utf8'));
  return path.join(site, 'pyodide', lock.packages[n].file_name);
}), { messageCallback: () => {} });
const sitePackages = py.runPython('import site; site.getsitepackages()[0]');
for (const w of JSON.parse(fs.readFileSync(path.join(site, 'wheels/wheels.json'), 'utf8')))
  py.unpackArchive(bytes('wheels/' + w), 'wheel', { extractDir: sitePackages });
py.unpackArchive(bytes('bundle.zip'), 'zip', { extractDir: '/app' });
py.runPython(fs.readFileSync(path.join(site, 'n64_stubs.py'), 'utf8'));
try {
  py.runPython(`
import sys, importlib; sys.path.insert(0, '/app'); importlib.invalidate_caches()
import splat.segtypes, spimdisasm, rabbitizer
from ff9builder import web, pipeline
for name in ('ff9.3dsx', 'ff9.cia', 'linkinfo.json'):
    assert pipeline.payload(name), f'no {name} in the bundle'
pipeline._modules()
t = rabbitizer.RegistersTracker()
lui, lo, ld = (rabbitizer.Instruction(w, 0x80020000 + 4 * i) for i, w in enumerate((0x3C048001, 0x24840BD0, 0x90820000)))
t.processLui(lui, 0); t.preprocessLoAndGetInfo(lo, 4); t.processLo(lo, 0x80010BD0, 4)
assert t.getAddressIfCanSetType(ld, 8) == 0x80010BD0, 'rabbitizer returns addresses signed: its wasm32 patch is missing'
`);
} catch (e) {
  console.error(String(e.message || e));
  process.exit(1);
}
console.log('smoke: ok');
