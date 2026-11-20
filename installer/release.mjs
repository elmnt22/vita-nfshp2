export const consoles = {
  switch: { name: 'Nintendo Switch', folder: 'NFSHP2-Switch', file: 'NFSHP2-Switch.nro' },
  switch2: { name: 'Nintendo Switch 2', folder: 'NFSHP2-Switch2', file: 'NFSHP2-Switch2.nro' },
};
export function validateNro(bytes) {
  if (bytes.length < 0x80 || bytes.length > 64 * 1024 * 1024) throw new Error('Invalid release executable size.');
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(16, false) !== 0x4e524f30 || v.getUint32(24, true) > bytes.length)
    throw new Error('The release file is not a valid NRO.');
}
export async function readRelease(target, manifest) {
  const entry = manifest.builds[target.file];
  if (!entry || !Number.isInteger(entry.size) || entry.size < 0x80 || entry.size > 64 * 1024 * 1024 || !/^[a-f0-9]{64}$/.test(entry.sha256))
    throw new Error('This console build is missing from the installer release.');
  const response = await fetch(`./release/${target.file}`, { cache: 'no-cache' });
  if (!response.ok) throw new Error('Could not download the NRO. Reload the page and try again.');
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.length !== entry.size) throw new Error('The NRO download is incomplete.');
  const hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), n => n.toString(16).padStart(2, '0')).join('');
  if (hash !== entry.sha256) throw new Error('The NRO does not match this release. Reload the page and try again.');
  validateNro(bytes);
  return new Blob([bytes]);
}
