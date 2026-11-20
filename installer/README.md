# Browser installer

Static GitHub Pages installer for the loose `NFSHP2-Switch.nro` and
`NFSHP2-Switch2.nro` release assets. Both console builds must be present in the
latest stable release. The Pages workflow snapshots that release into the site,
avoiding GitHub release-download CORS restrictions, and generates a SHA-256
manifest checked before installation.

The ISO is never uploaded. `iso.mjs` reads ISO9660 metadata, validates the
SLES-50731 PAL 1.00 boot executable's size and SHA-256, and exposes the two
archives as Blob slices. ZIP64 output streams those slices;
the 3.1 GB payload is not loaded into memory. Patched executables and other
regions/revisions are rejected. No game data or executables are committed here.

Output is always a ZIP, with Switch 1 selected by default. Chrome/Edge use a
save picker; other browsers use the service worker's streaming ZIP download.
Extract that ZIP onto the SD card root. The ZIP contains only the selected NRO,
`ZDIR.BIN`, and `ZZDATA.BIN` under the console's `switch/` folder, so it does not
replace saves or config. There is no folder-installation mode.

For local development, copy both locally built NROs into a temporary `release/`
directory, run `python installer/package.py <release-dir> <site-dir> <version>`,
and serve the resulting site over localhost, for example with
`python -m http.server 8000 --directory <site-dir>`. Do not commit the output.
Run `node --test installer/tests/*.test.mjs` for the parser/release/ZIP checks.

The ZIP64 writer and streaming-download helper are adapted from
[OpenBLOPS-NX](https://github.com/HDErick/OpenBLOPS-NX), under GPL-3.0; see the
repository-root LICENSE. No external scripts, analytics, or upload services are used.
