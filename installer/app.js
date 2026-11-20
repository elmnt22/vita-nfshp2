import { inspectIso } from './iso.mjs';
import { consoles, readRelease } from './release.mjs';
import { ZipWriter } from './zip-writer.mjs';
import { downloadWritable } from './download.mjs';

const el = id => document.getElementById(id);
const controls = [...document.querySelectorAll('button, input')];
const saveSupport = typeof window.showSaveFilePicker === 'function';
const streamSupport = 'serviceWorker' in navigator;
let target = consoles.switch, source, manifest, busy = false, selecting = 0;
if (streamSupport) navigator.serviceWorker.register('./sw.js').catch(() => {});
function status(message) { el('result').hidden = false; el('status').textContent = message; }
function buttons() {
  for (const control of controls) control.disabled = busy;
  el('zip').disabled = busy || !source || !manifest || !(saveSupport || streamSupport);
}
function showTarget() {
  el('output-path').textContent = `switch/${target.folder}/`;
}
for (const radio of document.querySelectorAll('[name="console"]')) radio.addEventListener('change', () => {
  target = consoles[radio.value]; showTarget();
});
el('iso').addEventListener('change', async () => {
  const token = ++selecting, iso = el('iso').files[0];
  source = null; buttons();
  if (!iso) return;
  el('source-label').textContent = iso.name;
  status('Checking the disc and PAL 1.00 revision…');
  el('progress').value = 0;
  try {
    const checked = await inspectIso(iso);
    if (token !== selecting) return;
    source = checked;
    status('PAL 1.00 verified. Choose Download ZIP, then extract it onto your SD card root.');
  } catch (error) { if (token === selecting) status(error.message); }
  if (token === selecting) buttons();
});
function progressStream(blob, update) {
  return blob.stream().pipeThrough(new TransformStream({ transform(chunk, controller) {
    update(chunk.length); controller.enqueue(chunk);
  } }));
}
async function install() {
  if (busy || !source || !manifest) return;
  const consoleBuild = target, disc = source;
  busy = true; buttons(); el('progress').value = 0;
  let writable, zip, success = false;
  try {
    // Ask for a save destination directly from the user's click, before awaits.
    let zipHandle;
    if (saveSupport) zipHandle = await showSaveFilePicker({
      id: 'nfshp2-zip', suggestedName: `${consoleBuild.folder}.zip`,
      types: [{ description: 'ZIP archive', accept: { 'application/zip': ['.zip'] } }],
    });
    status(`Downloading ${consoleBuild.file}…`);
    const nro = await readRelease(consoleBuild, manifest);
    const files = [{ name: consoleBuild.file, blob: nro, size: nro.size }, ...disc.files];
    const total = disc.bytes + nro.size;
    let copied = 0, refreshed = 0, current = '';
    const update = count => {
      copied += count;
      if (performance.now() - refreshed > 120 || copied === total) {
        el('progress').value = copied / total;
        status(`Packaging ${current} · ${Math.floor(100 * copied / total)}%`);
        refreshed = performance.now();
      }
    };
    writable = zipHandle ? await zipHandle.createWritable() : await downloadWritable(`${consoleBuild.folder}.zip`);
    zip = new ZipWriter(writable);
    for (const file of files) {
      current = file.name;
      const stream = progressStream(file.blob, update);
      await zip.add(`switch/${consoleBuild.folder}/${file.name}`, stream);
    }
    await zip.close();
    success = true;
    el('progress').value = 1;
    status('ZIP prepared. When the download finishes, extract it onto your SD card root.');
  } catch (error) {
    status(error.name === 'AbortError' ? 'Cancelled.' : `Installation stopped: ${error.message}`);
  } finally {
    if (!success && writable?.abort) await writable.abort().catch(() => {});
    busy = false; buttons();
  }
}
el('zip').addEventListener('click', () => install());
async function loadRelease() {
  try {
    const response = await fetch('./release/manifest.json', { cache: 'no-cache' });
    if (!response.ok) throw new Error('The release is not available yet. Try again shortly or use the GitHub downloads.');
    const value = await response.json();
    if (!Object.values(consoles).every(c => value.builds?.[c.file])) throw new Error('The release is missing a console build.');
    manifest = value; el('version').textContent = `Release ${value.version}`;
  } catch (error) { status(error.message); }
  buttons();
}
buttons(); loadRelease();
