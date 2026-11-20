"""Snapshot loose release NROs and the static installer into a Pages artifact."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import sys

source, output, version = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
installer = Path(__file__).resolve().parent
output.mkdir(parents=True, exist_ok=True)
(output / "release").mkdir(exist_ok=True)
builds = {}
for filename in ("NFSHP2-Switch.nro", "NFSHP2-Switch2.nro"):
    payload = (source / filename).read_bytes()
    if not 0x80 <= len(payload) <= 64 * 1024 * 1024 or payload[16:20] != b"NRO0":
        raise SystemExit(f"Invalid release asset: {filename}")
    assets = struct.unpack_from("<I", payload, 24)[0]
    if payload[assets:assets + 4] != b"ASET":
        raise SystemExit(f"Missing NRO metadata: {filename}")
    nacp_offset, nacp_size = struct.unpack_from("<QQ", payload, assets + 24)
    nacp = payload[assets + nacp_offset:assets + nacp_offset + nacp_size]
    if len(nacp) < 0x3070 or nacp[0x200:0x300].split(b"\0")[0] != b"HDErick":
        raise SystemExit(f"Unexpected NRO author: {filename}")
    if filename.endswith("Switch2.nro") != (b"Switch 2" in nacp[:0x200]):
        raise SystemExit(f"Wrong console metadata: {filename}")
    builds[filename] = {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
    shutil.copyfile(source / filename, output / "release" / filename)
for filename in ("index.html", "style.css", "app.js", "iso.mjs", "release.mjs", "download.mjs", "zip-writer.mjs", "sw.js"):
    shutil.copyfile(installer / filename, output / filename)
shutil.copyfile(installer.parent / "icon.png", output / "icon.png")
(output / ".nojekyll").touch()
(output / "release" / "manifest.json").write_text(
    json.dumps({"version": version, "builds": builds}, indent=2) + "\n", encoding="utf-8")
print(f"Packaged {version} with both console builds into {output}")
