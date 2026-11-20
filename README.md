# Need for Speed: Hot Pursuit 2 — Native Port

Nintendo Switch and Nintendo Switch 2 port of the PlayStation 2 game engine.

**Game data required: PlayStation 2 PAL version 1.00 (SLES-50731).** Use the original `ZDIR.BIN` and `ZZDATA.BIN` from the disc's `NFSHP2` directory. Game data is not included.

## Web installer and downloads

Use the [web installer](https://HDErick.github.io/NFSHP2_Switch_Port/) with your
unmodified PS2 **PAL 1.00 (SLES-50731) ISO**. Switch 1 is selected by default;
choose Switch 2 if needed, then download a ZIP and extract it onto your SD card
root. The ISO stays on your device. The installer verifies the revision,
extracts `ZDIR.BIN` and `ZZDATA.BIN`, and adds the matching NRO. It leaves saves
and settings alone.

Both NROs are also available as **loose files** in the
[latest release](https://github.com/HDErick/NFSHP2_Switch_Port/releases/latest).
No game data is included in releases. For manual installation, use
`switch/NFSHP2-Switch/` or `switch/NFSHP2-Switch2/` on the SD card.
When migrating the original Switch port, rename the old `switch/NFSHP2/`
folder to `switch/NFSHP2-Switch/` to retain saves, and replace its NRO.

## Nintendo Switch

### Build requirements

- devkitPro with devkitA64, libnx, and Switch tools (`elf2nro` and `nacptool`).
- CMake 3.24 or newer and GNU Make. On Windows, use the devkitPro MSYS2 shell and its CMake.
- Host Python 3 with Pillow to package the project-root `icon.png` as the Homebrew Menu icon. Set `HP2_ICON_PYTHON` at configure time if Pillow is installed in a different interpreter.
- The original Switch Mesa NVK Vulkan SDK, as used by OpenBLOPS-NX. Install the Horizon SDK headers and static libraries into `$DEVKITPRO/portlibs/switch`. The build needs `include/vulkan/vulkan_vi.h`, `lib/libvulkan.a`, and the Switch libelf, Expat, zlib, zstd, and libpng libraries.

From the repository root in the devkitPro shell:

```sh
cmake -S . -B build-nx -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-nx -j 8
```

### Install and run

Copy `build-nx/NFSHP2-Switch.nro` and the two PAL 1.00 archives to your SD card.

```text
sdmc:/switch/NFSHP2-Switch/
  NFSHP2-Switch.nro
  ZDIR.BIN
  ZZDATA.BIN
```

Launch through the Homebrew Menu in full application mode. The main scene renders at **1280×720** in both handheld and docked modes. New Vulkan pipelines compile in the background, and draws using an unfinished pipeline may briefly be absent on first use. The driver cache is saved on normal exit. Handheld mode requests the 460.8 MHz GPU APM profile. Docked mode uses the system profile. Saves and `NFSHP2_log.txt` are in `Saves/`.

### Frame rate and FPS counter

On first boot the port creates `config.txt` beside the running NRO. Defaults:

```ini
frame_limit=30
show_fps=0
```

`frame_limit` caps gameplay presentation (including the pause scene); use `0`
for uncapped or a value from `1` to `60`. Simulation and audio keep their existing
update rates. Frontend menus retain VSync pacing. The same cap and 720p output
apply in handheld and docked mode.

**Display Options** in the frontend and **Pause > Options > Display** include
**Frame Rate** and **Show FPS** as the first two rows. Change them with left/right; leaving Display
saves both to `config.txt`. Show FPS defaults to Off. Config edits take effect
on the next launch. The menu offers 20, 24, 25, 30, 40, 45, 50, 60, and Uncapped;
manual config values between these choices are also accepted.

Choose the language on the PAL selector each time the game starts. The choice
is kept only for the current session, including changes in Options. Loading a
profile preserves that session choice. Language is not stored in `config.txt`;
legacy `language=` entries are ignored.


Main-view draw distance is fixed at **0.3×**, mirror distance at **0.5×**, and
normal scene quality is retained. Switch 1 solo play uses the slower split-screen
reflection refresh schedule, updating one camera face per source refresh. Vehicle LOD uses **1.75×** and scenery keeps
the **35/50** near-detail range. FPS no longer changes distance, LOD policy,
shadows or reflections. Legacy `adaptive_draw_distance` and `test_low_quality`
config entries are ignored; no config edit is needed after updating.

Diagnostic logging is **off by default** (`HP2_SWITCH_DIAGNOSTICS=OFF`). The
no-log NRO keeps the FPS display available. Enable the CMake option only when
collecting diagnostic logs; those writes use a background worker.

### Vulkan pipeline cache

The project-root `VulkanPipelineCache.bin` is embedded in the NRO during the
build. It contains driver-generated pipeline/shader compilation data from the
Switch renderer, and is used to warm the pipeline cache on startup. The port
checks the cache header, GPU IDs and driver cache UUID before using it.

Compatible data already in `sdmc:/switch/NFSHP2-Switch/Saves/VulkanPipelineCache.bin`
is merged with the bundled seed. Missing, incompatible or rejected cache data
falls back safely. New pipeline states still compile asynchronously, and the
expanded cache is saved to the SD card on normal exit. The embedded seed stays
read-only and does not need to be copied beside the NRO.

### Controllers

Menus and single-player races accept input from any connected controller slot,
including handheld controls. Controllers share player one until a two-player
Challenge session is confirmed. Single Joy-Con input is mapped for horizontal
use; the Minus button on a single left Joy-Con acts as Start.

Confirming a two-player Challenge before car selection opens the libnx controller
connection screen for two players. Controller order there assigns P1 and P2.
Cancelling keeps you in race setup. Inputs remain separate through two-player
car selection and gameplay; returning to Challenge setup or the main menu
restores shared controls. Buttons held while closing the connection screen are
ignored until released.

### Custom controller artwork

Place your own **8-bit RGBA PNGs** with transparent backgrounds in
`sdmc:/switch/NFSHP2-Switch/Glyphs/`. Restart the game after changing them.

| File | Size | Replaces |
| --- | --- | --- |
| `buttons.png` | 128×256 | The complete `BUTTONSOUT` atlas used for menu and help prompts |
| `controller.png` | 256×152 | The controller picture for all four Switch gamepad layouts |
| `controller30.png` through `controller33.png` | 256×152 each | Optional per-layout pictures that take precedence over `controller.png` |

The Switch uses B to back out of menus and maps the original Square driving action (reverse in the default layout) to B in a race. The game remaps the face-button prompts for each context.

The atlas uses 26×28 cells with no spacing. Coordinates start at the
top-left. Keep each icon inside its cell and leave unused pixels transparent.
The NRO includes the project-root `buttons.png` by default. A valid SD
card `buttons.png` takes precedence over the embedded atlas.

| Top Y | X = 0 | X = 26 | X = 52 | X = 78 |
| --- | --- | --- | --- | --- |
| 0 | + | X | Right stick down | D-pad variant |
| 28 | − | B | Right stick up | Left stick click |
| 56 | Right stick left/right | ZL | Left stick down | |
| 84 | D-pad down | ZR | Left stick up | |
| 112 | D-pad up | L | Right stick click | |
| 140 | D-pad right | R | D-pad variant | |
| 168 | D-pad left | Left stick up/down | Steering wheel | |
| 196 | Y | Left stick left/right | Pedal | |
| 224 | A | Right stick up/down | Pedal | |

## Nintendo Switch 2

The Switch 2 build uses **libounce + the Switch 2 Mesa NVK SDK**. It renders
at **1920×1080 in both handheld and docked modes**, fitting the full frame
regardless of resolution preferences in a saved profile, with upstream scene quality:
main and mirror distance **1.0×**, scenery LOD **3.0×**, vehicle LOD **2.5×**,
and the upstream **125/165** scenery near-detail range. Driver and renderer
worker threads can use all available application CPU cores. At 120 FPS or Uncapped, the scene profile changes as described below. The
Switch 1 460.8 MHz GPU profile does not apply to Switch 2.

### Build requirements and command

- devkitPro/devkitA64, CMake 3.24+, GNU Make, Switch tools, and host Python 3
  with Pillow, as for Switch 1.
- [ouncebrew ounce-devkit](https://github.com/ouncebrew/ounce-devkit), including
  libounce and `$DEVKITPRO/libounce/cmake/Switch2.cmake`. This selects the
  Cortex-A78C CPU, `__SWITCH2__`, and libounce instead of libnx.
- An **external Switch 2 Horizon Mesa NVK SDK**, with `lib/libmesa_vulkan.a`,
  `lib/libvulkan.a`, `lib/cmake/Vulkan/VulkanConfig.cmake`, and the matching
  headers. Set the SDK directory below to its `opt/devkitpro/portlibs/switch2`
  directory. Graphics drivers/SDKs are not included in this repository.
- Switch portlibs for the shared C libraries: libpng, zlib, zstd, and Expat.

From the repository root in the devkitPro MSYS2 shell:

```sh
export DEVKITPRO=/opt/devkitpro
export HP2_SWITCH2_MESA_SDK=/path/to/external-sdk/opt/devkitpro/portlibs/switch2
# If the default Python does not have Pillow:
# export HP2_ICON_PYTHON=/path/to/python
JOBS=8 bash tools/nx/build-switch2.sh
```

Output: `build-nx/switch2/NFSHP2-Switch2.nro`, with its matching `.elf` for
crash symbolization. This is a Release/LTO build with logging and PGO training
disabled. Switch 1 training data is not used. The project-root `icon.png` and
`buttons.png` are embedded; author **HDErick**, version **1.0**.

### Install and display settings

Use **PAL PS2 version 1.00 (SLES-50731)**, the same archives as Switch 1:

```text
sdmc:/switch/NFSHP2-Switch2/
  NFSHP2-Switch2.nro
  ZDIR.BIN
  ZZDATA.BIN
  config.txt          (created on first boot)
  Saves/             (created automatically)
```

Launch with the Switch 2 homebrew environment (kaiHEN/ounce-hbmenu) in full
application mode. Saves, config, optional `Glyphs/` overrides, and the writable
Vulkan cache are separate from the Switch 1 installation. No language setting
is stored: use the PAL language selector each boot.

Default gameplay cap is **60 FPS**, with Show FPS off. Set **Frame Rate** to
**120 FPS** in Display Options, or use `frame_limit=120` in `config.txt`.
The Switch 2 menu also offers 75 and 90 FPS; config accepts 1–120, or 0 for
uncapped presentation. Above 60 FPS (or uncapped), the window requests **120 Hz
with VRR** through libounce, falling back where the display/system requires
60 Hz. Both modes keep a 1920×1080 render window. The cap changes presentation;
PAL simulation and audio retain their existing timing.

**120 FPS and Uncapped use the Switch 1 scene-detail constraints**: scenery LOD
1.0×, vehicle LOD 1.75×, the 35/50 near-detail range, reduced AI vehicle meshes
and secondary effects, and 0.5× mirror distance. Main draw distance stays
**1.0×** and output stays **1080p**. Other caps, including 60, 75, and 90 FPS,
use the upstream quality values above. Changes apply live at a frame boundary,
including changes from the pause menu. Actual FPS and 120 Hz
output need validation on the console and attached display.

The persistent reflection-map fix from upstream `136ac47d` prevents render-only
frames from blending the same environment image again. Fresh captures between
logical ticks still update reflections under a cap; the RT/LT clean-scene
history fix remains enabled. Async shader compilation continues to skip draws
whose pipelines are unfinished.

The Switch 1 bundled pipeline cache is not embedded in this build because it
belongs to a different GPU/driver. New pipelines still compile asynchronously
with unfinished draws skipped; the Switch 2 cache is saved on normal exit to
`sdmc:/switch/NFSHP2-Switch2/Saves/VulkanPipelineCache.bin`.

Porting references: [OpenBLOPS-NX](https://github.com/HDErick/OpenBLOPS-NX) and
[SafeHouse Switch 2 PR #65](https://github.com/FridiNaTor1/SafeHouse/pull/65).

Third-party notices: [SDL2](third_party/SDL2/LICENSE.txt) · [Vulkan headers](third_party/vulkan/LICENSE.md) · [Vulkan loader](third_party/vulkan/LICENSE-Loader.txt).
