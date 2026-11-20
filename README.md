# Need for Speed: Hot Pursuit 2 — PS Vita Native Port

A native PlayStation Vita port of Need for Speed: Hot Pursuit 2 (PS2 version).
The game runs as native ARM code on the Vita with a vitaGL renderer, its own
audio output and a multicore job system. No emulation and no recompiler are
involved.

This branch is a fork of HDErick's
[NFSHP2_Switch_Port](https://github.com/HDErick/NFSHP2_Switch_Port), a C++
re-implementation of the PS2 PAL 1.00 engine. The engine code comes from there.
This branch adds the Vita platform layer and the Vita-specific work.

> **Status: work in progress.** The game boots, runs menus, movies and races, and
> plays audio. Frame rate depends on the track and scene. The main goal is a
> steady 30 fps in heavy scenes, including with the rear-view mirror on.

## Game data

**You need your own copy of the PS2 PAL version 1.00 (SLES-50731).** No game data
is included and none will be provided.

Copy `ZDIR.BIN` and `ZZDATA.BIN` from the disc's `NFSHP2` directory to:

```text
ux0:data/NFSHP2N/
  ZDIR.BIN
  ZZDATA.BIN
```

Saves and `NFSHP2_log.txt` are written to the same folder.

## Install

Install `NFSHP2.vpk` with VitaShell (title ID `NFSHP2002`), then launch it from
the LiveArea. You don't need any plugins. If
[kubridge](https://github.com/bythos14/kubridge) is installed, the port uses it
to write `crash.txt` when it crashes. Without kubridge it runs normally.

## Controls

The Vita buttons map directly to the PS2 pad: face buttons, D-pad,
Start/Select, L → L1, R → R1, and both analog sticks.

## What's Vita-specific

- **Renderer:** a vitaGL backend for the engine's shared native gather path. It
  renders at 960×544 using the 16:9 layout. Rear-view mirror targets are drawn as
  FBOs. Smoke, dust and other particles switch to half or quarter resolution when
  they fill the screen.
- **Threading:** game logic runs on core 0. Gather runs on a render thread and GL
  submission on its own thread. Worker pools on cores 1–2 handle scenery emit,
  skinning, particles and speculative collision queries. Results are merged in
  order, so the output matches serial execution.
- **Exact optimisations:** NEON paths for matrix and vector math, bounds
  classification, skinning and MV3 movie decode (IDCT and YUV→RGBA). Draw
  batching and strip runs for scenery and car models. Each was checked against
  the reference path for bit-exact results, and gameplay and physics keep the
  retail behaviour.
- **Platform:** `sceIoPread` reads the 3 GB `ZZDATA.BIN`, because Vita newlib has
  a 32-bit `off_t`. Audio is output through `sceAudioOut`. Movie, speech and
  stream decodes run asynchronously.

## Building

Requirements: [VitaSDK](https://vitasdk.org/) with vitaGL, vitashark and
mathneon, plus CMake 3.24+ and Ninja.

```sh
cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita
```

The build writes `build-vita/NFSHP2.vpk` and `eboot.bin`. Build options are in
`cmake/Vita.cmake`:

| Option | Default | Effect |
| --- | --- | --- |
| `HP2_VITA_GL` | `ON` | vitaGL renderer (`OFF` builds a CPU-only null renderer) |
| `HP2_VITA_LTO` | `ON` | Link-time optimisation. Much faster on device, but the link is slow (~10 min) |
| `HP2_VITA_LTO_JOBS` | `4` | Parallel LTO link jobs |

## Debug and test toggles

Empty marker files in `ux0:data/NFSHP2N/` switch features at boot. They are for
testing and A/B comparisons only. Normal play needs none of them. Examples:

| File | Effect |
| --- | --- |
| `noaudio.txt` | Disable audio output |
| `nopipeline.txt` | Render synchronously (no render-thread pipelining) |
| `halfres_off.txt` / `quarterres_off.txt` | Full-resolution particles |
| `demo_bench.txt` | Contents `<track> [mirror]`: run the attract-mode pursuit on that track as a benchmark |
| `quickrace_track.txt` | Contents `<track> [reverse]`: force Quick Race onto that track |

The other `*_off.txt` and `*_serial.txt` files turn individual optimisations off
for verification. See `src/vita/main.cpp`.

## Other platforms

The upstream Windows, Nintendo Switch and Switch 2 targets are still in the tree
but aren't the focus of this branch. For those, see
[docs/SWITCH.md](docs/SWITCH.md) and the
[upstream repository](https://github.com/HDErick/NFSHP2_Switch_Port).

## Credits and licence

- Native engine: [HDErick/NFSHP2_Switch_Port](https://github.com/HDErick/NFSHP2_Switch_Port).
- Vita graphics: [vitaGL](https://github.com/Rinnegatamante/vitaGL) by Rinnegatamante.

Licensed under the GPL-3.0 (see [LICENSE](LICENSE)), the same as upstream.
Need for Speed and Hot Pursuit 2 are trademarks of Electronic Arts. This project
is not affiliated with or endorsed by Electronic Arts.
