# Need for Speed: Hot Pursuit 2 — PS Vita Native Port

A native PlayStation Vita port of **Need for Speed: Hot Pursuit 2**, built on a
C++ reimplementation of the PS2 game's engine and running the PS2 disc's own
data on the console. That covers the front end, cars, tracks, cops, HUD,
movies, speech, sound effects and music.

The goal is to play like the PS2 original. Gameplay, physics, AI and handling
keep the retail behaviour; every Vita optimisation is checked against the
reference code for identical results. There is no emulation and no
recompiler: the game runs as native ARM code.

> **No game data is included.** This repository has no EA assets. You need
> your own legally owned copy of the PS2 game. This project is not
> affiliated with, authorized or endorsed by Electronic Arts.

Disclosure: Whilst I know a bit of programming and coding, I used AI to help accelerate areas of this port that otherwise would have taken me forever. AI used was Claude Opus.

---

## Contents

- [What's in the port](#whats-in-the-port)
- [What you need](#what-you-need)
- [Installation](#installation)
- [Controls](#controls)
- [Options](#options)
- [Reporting problems](#reporting-problems)
- [Building from source](#building-from-source)
- [Repository layout](#repository-layout)
- [Credits](#credits)
- [License and legal](#license-and-legal)

---

## What's in the port

- **The PS2 game.** The front end, race modes, Hot Pursuit with its cops
  and pursuits, AI opponents and traffic, results screens and saving, all
  from the PS2 PAL engine.
- **30 FPS.** Locked to an even 30 FPS by default, with the rear-view
  mirror on.
- **Graphics.** A Vita renderer that draws straight to the GPU (GXM), at
  960×544 in 16:9. The original world, car and particle effects are kept.
  Smoke, dust and other screen-filling particles are drawn at half or
  quarter resolution to hold the frame rate. AI opponent cars use their
  simpler detail level (no driver or road shadow), and in the heaviest
  frames distant scenery switches to lower detail a little earlier.
- **Audio.** Engine, collision, cop, speech, music and ambient sounds from the
  game's own banks. The voice mixer uses the PS2 sound chip's Gaussian
  interpolation, and tunnels get the PS2's hardware reverb. A peak limiter
  keeps busy race moments from clipping.
- **Movies.** The game's MV3 movies are decoded on the Vita.
- **Multicore.** Game logic, rendering, GPU submission, audio and helper
  jobs (scenery, skinning, particles, collision queries) are spread across
  the Vita's cores. Results are merged in order, so the output matches
  single-threaded play.

**Known issues:** very busy moments (big pileups, wide open views) can still
dip briefly below 30 FPS.

---

## What you need

- **Need for Speed: Hot Pursuit 2 for PS2, PAL version 1.00 (SLES-50731).**
  Other regions and versions are not supported.
- A **PS Vita or PS TV** with **HENkaku / Ensō** custom firmware.
- **VitaShell**, to install the VPK and copy files.
- **`libshacccg.suprx`** extracted to `ur0:data/libshacccg.suprx`. The
  renderer compiles some shaders on the console and needs it. If you don't
  have it yet, extract it with **ShaRKBR33D**.
- About **3.2 GB** free on the memory card for the game data.

No plugins are needed.

---

## Installation

### 1. Install the VPK

Copy `NFSHP2.vpk` from the [Releases](https://github.com/elmnt22/vita-nfshp2/releases)
page to your Vita and install it with VitaShell. It shows up as
**NFS Hot Pursuit 2** (title ID `NFSHP2002`).

### 2. Copy the game data

Copy `ZDIR.BIN` and `ZZDATA.BIN` from the disc's `NFSHP2` folder to
**`ux0:data/NFSHP2/`** on the Vita:

```
ux0:data/NFSHP2/
├── ZDIR.BIN
└── ZZDATA.BIN       <- about 3 GB
```

Saves and the log file (`NFSHP2_log.txt`) are written to the same folder.

### 3. Play

Start **NFS Hot Pursuit 2** from the LiveArea.

---

## Controls

The Vita buttons map directly to the PS2 pad: face buttons, D-pad,
Start/Select, L → L1, R → R1, and both analog sticks.

---

## Options

Empty marker files in `ux0:data/NFSHP2/` change settings at boot. Normal play
needs none of them.

| File | Effect |
| --- | --- |
| `fps_uncapped.txt` | Unlock the frame rate (up to 60, varies; experimental) |
| `audio_dry.txt` | Turn the reverb off |
| `audio_linear.txt` | Use plain linear voice interpolation instead of the PS2 Gaussian one |
| `speedtrap_retail.txt` | Keep the PS2's original race-start behaviour for the parked speed-trap cop (costs a slow first second on some tracks) |
| `reduced_opponents_off.txt` | Draw AI opponent cars at full detail (costs frame rate in busy scenes) |
| `scenery_cut_off.txt` | Never lower scenery detail in heavy frames (costs frame rate) |
| `audio_limiter_off.txt` | Turn the peak limiter off (loud moments clip) |
| `audio_gain.txt` | Not empty: a number such as `0.6` sets the overall volume before the limiter (default `1.0`) |

The other `*_off.txt` and `*_serial.txt` files turn individual optimisations off
for testing. See `src/vita/main.cpp`.

---

## Reporting problems

Open an [issue](https://github.com/elmnt22/vita-nfshp2/issues). Attach
`ux0:data/NFSHP2/NFSHP2_log.txt` from the run with the problem, plus a
screenshot for a visual problem.

If [kubridge](https://github.com/bythos14/kubridge) is installed, a crash also
writes `crash.txt` to the same folder; please attach that too. Without kubridge
the game runs normally.

---

## Building from source

You only need this to build the port yourself. Players can use the
release files.

**Requirements:** [VitaSDK](https://vitasdk.org/) with vitaGL, vitashark and
mathneon, plus CMake 3.24+ and Ninja.

```sh
cmake -S . -B build-vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita     # -> build-vita/NFSHP2.vpk (+ eboot.bin)
```

Build options are in `cmake/Vita.cmake`:

| Option | Default | Effect |
| --- | --- | --- |
| `HP2_VITA_GL` | `ON` | Vita renderer (`OFF` builds a CPU-only null renderer) |
| `HP2_OWN_VITAGL` | `ON` | Link a patched vitaGL from `../deps/vitaGL_hp2` when present, else the VitaSDK one |
| `HP2_VITA_LTO` | `ON` | Link-time optimisation. Much faster on device, but the link is slow (~10 min) |
| `HP2_VITA_LTO_JOBS` | `4` | Parallel LTO link jobs |

The LiveArea art is in `src/vita/livearea/`.

---

## Repository layout

| Path | Contents |
| --- | --- |
| `src/vita/` | Vita platform layer: boot, input, audio mixer and reverb, crash handler, LiveArea assets |
| `src/ecstasy/` | Renderer: the shared native gather and the Vita GXM / vitaGL backends |
| `indep/` | Game code: race, cars, physics, AI, cops, front end, HUD, sound scape |
| `bsound/`, `bware/` | Sound library and the engine's base library |
| `inc/`, `cmn/` | Shared headers and common code |
| `cmake/` | Build files for each platform (`Vita.cmake`) |
| `docs/`, `tools/` | Notes and helper tools |

The upstream Windows and Nintendo Switch targets are still in the tree but
aren't the focus of this port. See [docs/SWITCH.md](docs/SWITCH.md) and the
[upstream repository](https://github.com/HDErick/NFSHP2_Switch_Port).

---

## Credits

- **[NFSHP2_Switch_Port](https://github.com/HDErick/NFSHP2_Switch_Port)** by
  **HDErick**. It is the C++ reimplementation of the PS2 PAL 1.00 engine this
  port is built on.
- **[vitaGL](https://github.com/Rinnegatamante/vitaGL)** by Rinnegatamante,
  and the **[VitaSDK](https://vitasdk.org/)** team.
- **[VitaShell](https://github.com/TheOfficialFloW/VitaShell)** and
  **HENkaku / Ensō** by TheFloW and contributors. Players need them to run
  homebrew.
- **ShaRKBR33D** / **SharkF00D** for extracting `libshacccg.suprx`.
- **[vitacompanion](https://github.com/devnoname120/vitacompanion)** by
  devnoname120, for FTP and remote launch during development.
- **[kubridge](https://github.com/bythos14/kubridge)** by bythos14, for the
  optional crash reports.
- **[psx-spx](https://psx-spx.consoledev.net/)** by Martin Korth (nocash) and
  contributors, for the documentation of the sound chip's reverb and
  Gaussian interpolation.
- **Electronic Arts / EA Black Box** for the original game.

## License and legal

Licensed under the **GPL-3.0** (see [LICENSE](LICENSE)), the same as the
upstream engine. Third-party code keeps its own license.

Need for Speed and Need for Speed: Hot Pursuit 2 are trademarks of Electronic
Arts. All game content belongs to its owners. This project ships none of it:
the port loads the data from your own copy at runtime.
