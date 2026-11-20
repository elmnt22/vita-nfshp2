// ISO9660 reader for an unmodified PS2 PAL 1.00 disc. Reads only directory
// records and the 2.3 MB boot executable; archive payloads remain Blob slices.
const SECTOR = 2048;
export const PAL_BOOT_SHA256 = '7335b8ed479481082c648958f4b5be4da65b74d042d0ab2f5237df801eafacc2';
const decoder = new TextDecoder('ascii', { fatal: true });
const fail = message => { throw new Error(message); };
function dual32(bytes, offset) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const n = v.getUint32(offset, true);
  if (n !== v.getUint32(offset + 4, false)) fail('The ISO has inconsistent directory metadata.');
  return n;
}
function range(iso, start, size) {
  if (!Number.isSafeInteger(start) || !Number.isSafeInteger(size) || start < 0 || size < 0 || start + size > iso.size)
    fail('The ISO is truncated or has an invalid file extent.');
  return iso.slice(start, start + size);
}
function record(iso, bytes) {
  if (bytes.length < 34 || bytes[0] !== bytes.length || 33 + bytes[32] > bytes.length)
    fail('The ISO has an invalid directory record.');
  if (bytes[1] || bytes[26] || bytes[27] || (bytes[25] & 0x80))
    fail('Interleaved or multi-extent ISO files are not supported. Use a standard PAL 1.00 ISO.');
  const extent = dual32(bytes, 2), size = dual32(bytes, 10);
  const start = extent * SECTOR;
  range(iso, start, size);
  const name = decoder.decode(bytes.subarray(33, 33 + bytes[32])).replace(/;\d+$/, '').toUpperCase();
  return { name, start, size, directory: !!(bytes[25] & 2) };
}
async function directory(iso, entry) {
  if (!entry.directory || entry.size > 1024 * 1024) fail('The ISO directory is invalid or too large.');
  const bytes = new Uint8Array(await range(iso, entry.start, entry.size).arrayBuffer());
  const entries = new Map();
  for (let at = 0; at < bytes.length;) {
    const size = bytes[at];
    if (!size) { at = (Math.floor(at / SECTOR) + 1) * SECTOR; continue; }
    if (at + size > bytes.length || at % SECTOR + size > SECTOR) fail('The ISO directory is truncated.');
    const child = record(iso, bytes.subarray(at, at + size));
    at += size;
    if (child.name === '\0' || child.name === '\x01') continue;
    if (entries.has(child.name)) fail('The ISO contains duplicate directory entries.');
    entries.set(child.name, child);
  }
  return entries;
}
function file(entries, name) {
  const entry = entries.get(name);
  if (!entry || entry.directory || !entry.size) fail(`The ISO is missing ${name}. Use the PS2 PAL 1.00 disc.`);
  return entry;
}
export async function inspectIso(iso) {
  if (!iso || iso.size < 17 * SECTOR) fail('Choose a complete PS2 PAL 1.00 ISO.');
  let pvd;
  for (let sector = 16; sector < 32; ++sector) {
    const bytes = new Uint8Array(await range(iso, sector * SECTOR, SECTOR).arrayBuffer());
    if (decoder.decode(bytes.subarray(1, 6)) !== 'CD001' || bytes[6] !== 1)
      fail('This is not a standard 2048-byte-sector ISO. Choose an ISO, not a BIN/CUE image.');
    if (bytes[0] === 1) { pvd = bytes; break; }
    if (bytes[0] === 255) break;
  }
  if (!pvd) fail('No ISO9660 filesystem was found on this disc.');
  const view = new DataView(pvd.buffer);
  if (view.getUint16(128, true) !== SECTOR || view.getUint16(130, false) !== SECTOR)
    fail('Unsupported ISO sector size.');
  range(iso, 0, dual32(pvd, 80) * SECTOR);
  const root = await directory(iso, record(iso, pvd.subarray(156, 156 + pvd[156])));
  const cnf = file(root, 'SYSTEM.CNF');
  if (cnf.size > 65536) fail('Invalid SYSTEM.CNF.');
  const settings = decoder.decode(await range(iso, cnf.start, cnf.size).arrayBuffer());
  if (!/BOOT2\s*=\s*cdrom0:\\SLES_507\.31;1/i.test(settings) ||
      !/\bVER\s*=\s*1\.00\b/i.test(settings) || !/\bVMODE\s*=\s*PAL\b/i.test(settings))
    fail('This disc is not the supported PAL 1.00 release (SLES-50731).');
  const boot = file(root, 'SLES_507.31');
  if (boot.size !== 2321608) fail('Unsupported game revision. Use an unmodified PAL 1.00 ISO.');
  const digest = await crypto.subtle.digest('SHA-256', await range(iso, boot.start, boot.size).arrayBuffer());
  const hash = Array.from(new Uint8Array(digest), n => n.toString(16).padStart(2, '0')).join('');
  if (hash !== PAL_BOOT_SHA256) fail('The executable does not match PAL 1.00. Modified discs and other revisions are unsupported.');
  const game = root.get('NFSHP2');
  if (!game?.directory) fail('The ISO is missing its NFSHP2 directory.');
  const entries = await directory(iso, game);
  const files = ['ZDIR.BIN', 'ZZDATA.BIN'].map(name => {
    const entry = file(entries, name);
    return { name, size: entry.size, blob: range(iso, entry.start, entry.size) };
  });
  // The supported disc's archive table and payload must be complete.
  if (files[0].size !== 4872 || files[1].size !== 3116437504)
    fail('The game archives do not match the PAL 1.00 disc.');
  return { files, bytes: files.reduce((sum, f) => sum + f.size, 0) };
}
