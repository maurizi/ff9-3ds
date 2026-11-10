/* The web builder's page: picks the disc files, shows which disc each is, runs the build in worker.js and saves the
 * result (a save dialog where the browser has one, else a download). */
const $ = (id) => document.getElementById(id);
const chosen = new Map(); // file name -> File (all the user has picked: sheets and images)
let discs = {};           // disc number -> {name, boot}: the file that is it
let worker = null, busy = false, testUrls = null;

function newWorker() {
  if (worker) worker.terminate();
  worker = new Worker('worker.js', { type: 'module' });
  worker.onmessage = (e) => onMessage(e.data);
  worker.onerror = (e) => fail(e.message || 'the builder stopped');
}

function setStatus(s) { $('status').textContent = s; }
function log(s) { const l = $('log'); l.textContent += s + '\n'; l.scrollTop = l.scrollHeight; }
function fail(msg) {
  busy = false;
  refresh();
  setStatus('Stopped.');
  log('Error: ' + msg);
  alert(msg);
}

function refresh() {
  const t = $('discs');
  t.innerHTML = '';
  for (let n = 1; n <= 4; n++) {
    const tr = t.insertRow();
    tr.insertCell().textContent = 'Disc ' + n;
    const c = tr.insertCell();
    if (discs[n]) { c.textContent = '✓ ' + discs[n].name + '  (' + discs[n].boot + ')'; c.className = 'ok'; }
    else { c.textContent = 'not chosen yet'; c.className = 'muted'; }
  }
  const all = [1, 2, 3, 4].every((n) => discs[n]);
  $('build').disabled = busy || !all;
  if (!busy) setStatus(all ? 'Ready to build.' : 'Add your four discs to begin.');
}

function identify() {
  newWorker();
  setStatus('Checking the files…');
  worker.postMessage(testUrls ? { type: 'identify', urls: testUrls } : { type: 'identify', files: [...chosen.values()] });
}

$('files').addEventListener('change', (e) => {
  for (const f of e.target.files) chosen.set(f.name, f);
  e.target.value = '';
  if (chosen.size) identify();
});

$('build').addEventListener('click', () => {
  busy = true;
  $('save').hidden = true;
  $('log').textContent = '';
  $('bar').style.width = '0';
  refresh();
  const fmt = document.querySelector('input[name=fmt]:checked').value;
  // a fresh worker for each build (the last one closed its storage when it finished): the files again, then the build
  identify();
  worker.postMessage({ type: 'build', names: [1, 2, 3, 4].map((n) => discs[n].name), fmt });
});

async function onMessage(m) {
  if (m.type === 'identified') {
    if (busy) return; // (the files again before a build: already shown)
    discs = {};
    const errors = $('errors');
    errors.innerHTML = '';
    for (const r of m.results) {
      if (r.error) { const li = document.createElement('li'); li.textContent = r.error; errors.appendChild(li); }
      else discs[r.disc] = { name: r.name, boot: r.boot };
    }
    refresh();
  } else if (m.type === 'progress') {
    $('bar').style.width = (m.fraction * 100).toFixed(1) + '%';
    if (m.message) setStatus(m.message + '…');
  } else if (m.type === 'log') {
    log(m.message);
  } else if (m.type === 'error') {
    fail(m.message);
  } else if (m.type === 'done') {
    busy = false;
    $('bar').style.width = '100%';
    refresh();
    setStatus('Done.');
    offerSave(m.file, m.name);
  }
}

async function offerSave(opfsName, name) {
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('ff9-builder');
  const file = await (await dir.getFileHandle(opfsName)).getFile();
  const how = name.endsWith('.cia') ? 'Copy it to the SD card and install it with FBI.'
                                    : 'Copy it to /3ds/ on the SD card and start it from the Homebrew Launcher.';
  $('save').hidden = false;
  $('save').querySelector('p').textContent = `${name} is ready (${(file.size / 1e9).toFixed(2)} GB). ${how}`;
  $('saveBtn').onclick = async () => {
    if (window.showSaveFilePicker) { // Chrome, Edge: straight to the chosen file
      try {
        const h = await showSaveFilePicker({ suggestedName: name });
        setStatus('Saving…');
        await file.stream().pipeTo(await h.createWritable());
        setStatus('Saved.');
      } catch (err) {
        if (err.name !== 'AbortError') fail('Saving failed: ' + err.message);
      }
    } else { // elsewhere: a download of the file (from the browser's storage, not memory)
      const a = document.createElement('a');
      a.href = URL.createObjectURL(file);
      a.download = name;
      a.click();
    }
  };
}

refresh();

// Local testing only (a page served from localhost, ?test=url,url,...): those files, read on demand by the worker,
// instead of a file dialog, which an automated browser can't use. Anywhere else the page fetches only its own code.
(() => {
  const urls = new URLSearchParams(location.search).get('test');
  if (!urls || !['localhost', '127.0.0.1'].includes(location.hostname)) return;
  testUrls = urls.split(',').map((u) => new URL(u, location.href).href);
  identify();
})();
