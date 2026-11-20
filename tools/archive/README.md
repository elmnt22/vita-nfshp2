# NFS HP2 Archive Utility

Native Windows and Linux utility for the original PS2 Hot Pursuit 2 ZDIR/ZZDATA format. Python and external libraries are not required to run it.

## Extract

Put `NFSHP2Unpack.exe` beside `ZDIR.BIN` and `ZZDATA.BIN`, then run it. It extracts into a new `@ZZDATA` folder. If that folder already exists, it uses a new `@ZZDATA_verified` folder and preserves the old files. On Windows, if no adjacent archive is found, it opens an archive selection dialog.

Alternatively, drag `ZZDATA.BIN` onto the EXE, or run:

```text
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --output "F:\Game\NFSHP2\@ZZDATA_NEW"
```

Every record is checked before any output directory is created. Payloads are copied exactly and read back to verify their SHA256. Nonempty records never produce accepted empty files. The original zero-length records remain empty and are explicitly marked in `NFSHP2_EXTRACTION_MANIFEST.json`.

The built-in dictionary provides known resource paths. Resources without a known filename remain available as `__Unknown/HASH.bin`; they are not discarded or assigned guessed names. Different payloads with the same name are preserved under `__duplicates`. LZC/COMP files retain their original bytes and extension.

The exact original index is preserved as `__archive/ZDIR.BIN`. The native game's loose-data reader can use this index to open unresolved resources by hash. If two archive entries with one hash have different actual contents, the tool preserves both and writes their hash to `__archive/AMBIGUOUS_HASHES.BIN` (sorted little-endian 32-bit values). The game refuses that ambiguous resource instead of silently selecting a version. Identical duplicate extents are collapsed only after their actual SHA256 matches. Sidecar checksums and this verification scope are recorded in the manifest.

## Report filename coverage

```text
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --inventory "F:\Game\archive_names.json"
```

This writes a new JSON inventory without extracting files. It lists every original record, its hash, offset and size, the dictionary path when available, and the preserved hash path for unresolved records. Header signatures provide separate data-type hints; they never become guessed filenames. This command checks index bounds and reads at most 4096 bytes of each unnamed payload. Use `--verify` for complete payload verification.

The archive index contains three 32-bit fields per record: name hash, sector and size. It does not contain original filename strings. The inventory distinguishes dictionary paths from unresolved hash paths.

## Address a resource without inventing its name

The game file reader uppercases ASCII pathname bytes, replaces `/` with `\`, then computes a 32-bit key from seed `FFFFFFFF` using `key = key * 33 + byte`. Index lookup returns the sector and size for that key. `--lookup-path` and `--lookup-id` inspect resources through the same name hash.

```text
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --lookup-path "REPLAY\JUL23\T11BlueElise.bin"
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --lookup-id 74BF07B7
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --lookup-id 74BF07B7 --raw-output "F:\Work\74BF07B7.bin"
```

These commands return JSON with the exact ID, matching record numbers, sectors, offsets, sizes, dictionary names when known, and SHA256 of complete original payloads. A supplied pathname is a query, never new evidence for the dictionary. Direct ID selection is an offline PC tool operation. It neither assigns an original name nor changes runtime resource ownership. Raw output is copied unchanged, read back, and published atomically as a new file; its parent directory must already exist. This is an individual payload export, not a complete mounted game directory.

Every matching record is reported. If different payloads share the ID, raw export requires an explicit `--record <index>` from the response. The utility never silently picks one such extent. Identical duplicate payloads may share one raw output only after their full hashes and sizes match.

Extraction does not rename or overwrite a previous extraction. Select a new output directory to obtain the current dictionary paths, or use `--verify` to check an existing extraction in place.

## Verify an existing extraction

```text
NFSHP2Unpack.exe --input "F:\Game\NFSHP2\ZZDATA.BIN" --verify "F:\Game\NFSHP2\@ZZDATA"
```

This compares every original archive extent with the extracted file. It reports unexpected empty files, missing files, wrong lengths and different contents without modifying those files. Legacy `FileN` extraction names are recognized only when the complete ZDIR SHA256 matches the retained PAL index. Ekey's `__Unknown/HASH` names are also recognized.

Exit codes: `0` success, `2` input/I/O error, `3` verification mismatch. Verification writes a new JSON report. Existing outputs and reports are never overwritten.

Additional options: `--zdir`, `--zzdata`, `--names` (user aliases), `--list`, `--report`, `--hash`, `--help`.

## User aliases preserve the original ID

`--names <UTF-8 text file>` accepts one safe relative pathname per line, with optional `#` comments. These are user aliases. The inventory and extraction manifest list them separately as `external_aliases`.

For an unresolved resource, extraction always retains `__Unknown/HASH.bin`, even when an alias is supplied. Each alias is an additional byte-identical copy verified by length and complete SHA256. This keeps the game's original pathname-to-ID lookup working when the supplied alias differs from the original spelling. Multiple aliases may share one ID. Supplying a built-in name again does not alter its provenance.

If one ID has different source payloads, no alias is materialized: each extent remains preserved under its canonical or `__duplicates` path, with `REQUIRES_EXPLICIT_RECORD` recorded for the aliases. Use `--lookup-id ... --record ... --raw-output ...` to select that extent explicitly. Runtime refuses the ambiguous ID.

Use the same `--names` file with `--verify` to check additional alias copies as well as canonical payloads and runtime sidecars. Without that file, verification covers canonical archive resources; it does not infer external aliases from filesystem names. Altering an alias cannot replace the canonical resource or make an unknown original filename become known.

## Prepare a ready game folder

Extract into a new directory, verify it, and put the current `NFSHP2.exe` and `SDL2.dll` beside that directory. For example:

```text
NFSHP2Unpack.exe --input "F:\Original\NFSHP2\ZZDATA.BIN" --output "F:\ReadyGame\@ZZDATA"
NFSHP2Unpack.exe --input "F:\Original\NFSHP2\ZZDATA.BIN" --verify "F:\ReadyGame\@ZZDATA"
NFSHP2.exe --data "F:\ReadyGame\@ZZDATA"
```

Keep the complete extracted directory, including `__Unknown`, `__archive/ZDIR.BIN`, any ambiguity sidecar, and the extraction manifest. The game's existing hash lookup resolves unnamed resources through that index. The original `ZZDATA.BIN` is not needed at launch when `--data` selects the complete extraction. Do not discard unnamed files or original zero-length entries while preparing the folder. Preserve `Saves`, `settings.db`, `config.bin` and `NFSHP2_pc.ini` when updating an existing installation.

## Build

The utility is ordinary C++17. Its local CMake target is `NFSHP2Unpack`; Windows uses the system `comdlg32` and `user32` libraries. MSVC builds use `/W4 /WX /MT` in Release. There is no dependency on the game libraries, Vulkan, XAudio2 or an installed Python interpreter.

Game archives are separate from this source package.