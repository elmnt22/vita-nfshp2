import test from 'node:test';
import assert from 'node:assert/strict';
import { inspectIso, PAL_BOOT_SHA256 } from '../iso.mjs';
import { validateNro, readRelease, consoles } from '../release.mjs';
import { ZipWriter } from '../zip-writer.mjs';

const sector = 2048, encoder = new TextEncoder();
function number(bytes, offset, value) {
  const view = new DataView(bytes.buffer);
  view.setUint32(offset, value, true); view.setUint32(offset + 4, value, false);
}
function entry(name, extent, size, directory = false) {
  const nameBytes = encoder.encode(name), length = 33 + nameBytes.length + !(nameBytes.length % 2);
  const bytes = new Uint8Array(length); bytes[0] = length;
  number(bytes, 2, extent); number(bytes, 10, size);
  bytes[25] = directory ? 2 : 0; bytes[32] = nameBytes.length; bytes.set(nameBytes, 33);
  return bytes;
}
class SparseIso {
  size = 3121184768;
  pieces = [];
  put(start, bytes) { this.pieces.push({ start, bytes }); }
  slice(start, end) {
    const size = end - start;
    return { size, arrayBuffer: async () => {
      assert.ok(size <= 4 * 1024 * 1024, 'parser must not load archive payloads');
      const result = new Uint8Array(size);
      for (const piece of this.pieces) {
        const left = Math.max(start, piece.start), right = Math.min(end, piece.start + piece.bytes.length);
        if (left < right) result.set(piece.bytes.subarray(left - piece.start, right - piece.start), left - start);
      }
      return result.buffer;
    } };
  }
}
function fixture(settings = 'BOOT2 = cdrom0:\\SLES_507.31;1\r\nVER = 1.00\r\nVMODE = PAL\r\n') {
  const iso = new SparseIso(), pvd = new Uint8Array(sector);
  pvd[0] = 1; pvd.set(encoder.encode('CD001'), 1); pvd[6] = 1;
  number(pvd, 80, iso.size / sector);
  const view = new DataView(pvd.buffer); view.setUint16(128, sector, true); view.setUint16(130, sector, false);
  pvd.set(entry('\0', 261, sector, true), 156); iso.put(16 * sector, pvd);
  const cnf = encoder.encode(settings);
  let at = 261 * sector;
  for (const record of [entry('\0',261,sector,true),entry('\x01',261,sector,true),entry('SYSTEM.CNF;1',3003,cnf.length),entry('SLES_507.31;1',302,2321608),entry('NFSHP2',262,sector,true)]) {
    iso.put(at, record); at += record.length;
  }
  iso.put(3003 * sector, cnf);
  const zdir = entry('ZDIR.BIN;1',2304,4872), data = entry('ZZDATA.BIN;1',2307,3116437504);
  iso.put(262 * sector,zdir);iso.put(262 * sector + zdir.length,data);
  return iso;
}
test('PAL metadata and streamed archive slices, without reading the 3.1 GB payload', async context => {
  const digest = context.mock.method(crypto.subtle, 'digest', async (algorithm, bytes) => {
    assert.equal(algorithm,'SHA-256');assert.equal(bytes.byteLength,2321608);
    return Uint8Array.from(PAL_BOOT_SHA256.match(/../g), s => parseInt(s,16)).buffer;
  });
  const result = await inspectIso(fixture());
  assert.equal(digest.mock.calls.length,1);
  assert.deepEqual(result.files.map(f=>[f.name,f.size]),[['ZDIR.BIN',4872],['ZZDATA.BIN',3116437504]]);
  assert.equal(result.bytes,3116442376);
});
test('wrong regions, wrong revisions, patched executables and truncated ISOs are rejected', async () => {
  await assert.rejects(inspectIso(new Blob([new Uint8Array(100)])),/complete/);
  await assert.rejects(inspectIso(fixture('BOOT2 = cdrom0:\\SLUS_203.62;1\nVER = 1.00\nVMODE = NTSC')),/PAL 1.00/);
  await assert.rejects(inspectIso(fixture('BOOT2 = cdrom0:\\SLES_507.31;1\nVER = 2.00\nVMODE = PAL')),/PAL 1.00/);
  await assert.rejects(inspectIso(fixture()),/executable does not match/);
  const truncated=fixture();truncated.size-=sector;
  await assert.rejects(inspectIso(truncated),/truncated/);
  const invalid=fixture();invalid.pieces[0].bytes[84]^=1;
  await assert.rejects(inspectIso(invalid),/inconsistent/);
});
test('loose NRO identity, size and checksum are checked', async context => {
  const bytes=new Uint8Array(128);bytes.set(encoder.encode('NRO0'),16);
  validateNro(bytes);
  assert.throws(()=>validateNro(new Uint8Array(128)),/valid NRO/);
  const sha256=Buffer.from(await crypto.subtle.digest('SHA-256',bytes)).toString('hex');
  context.mock.method(globalThis,'fetch',async()=>new Response(bytes));
  const manifest={builds:{'NFSHP2-Switch.nro':{size:128,sha256}}};
  assert.equal((await readRelease(consoles.switch,manifest)).size,128);
  manifest.builds['NFSHP2-Switch.nro'].sha256='0'.repeat(64);
  await assert.rejects(readRelease(consoles.switch,manifest),/does not match/);
  assert.equal(consoles.switch.folder,'NFSHP2-Switch');
  assert.equal(consoles.switch2.file,'NFSHP2-Switch2.nro');
});
test('ZIP64 output carries the console folder, payload, sizes and CRC', async () => {
  const chunks=[];let closed=false;
  const zip=new ZipWriter({async write(b){chunks.push(b.slice());},async close(){closed=true;}});
  const path='switch/NFSHP2-Switch2/ZDIR.BIN';
  await zip.add(path,new Blob(['123456789']).stream());await zip.close();
  const bytes=Buffer.concat(chunks);assert.ok(closed);
  assert.equal(bytes.readUInt32LE(0),0x04034b50);
  assert.equal(bytes.subarray(30,30+path.length).toString(),path);
  const descriptor=bytes.indexOf(Buffer.from([0x50,0x4b,0x07,0x08]));
  assert.equal(bytes.readUInt32LE(descriptor+4),0xcbf43926);
  assert.equal(bytes.readBigUInt64LE(descriptor+8),9n);
  assert.equal(bytes.readBigUInt64LE(descriptor+16),9n);
  assert.ok(bytes.includes(Buffer.from([0x50,0x4b,0x06,0x06])));
  assert.equal(bytes.readUInt32LE(bytes.length-22),0x06054b50);
});
