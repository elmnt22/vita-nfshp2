# Nintendo Switch build

This bring-up target runs the native engine through libnx and the existing
Vulkan renderer. It is cross-built for the original Nintendo Switch.
The libnx audout backend mixes music, effects, speech, and movie audio to
stereo PCM. Switch reverb is pending. Audio, racing, controller vibration,
and saves are supported; changes still need testing on hardware.

See the [README](../README.md#nintendo-switch) for the Switch toolchain,
build command, and PAL 1.00 data requirements.

## Hardware smoke test

Copy the NRO and your PAL 1.00 PS2 archive pair to this layout.

```text
sdmc:/switch/NFSHP2-Switch/
  NFSHP2-Switch.nro
  ZDIR.BIN
  ZZDATA.BIN
```

Launch through the Homebrew Menu in full application mode. On first run,
check the title screen, controller navigation, a race, HOME suspend and
resume, and whether a profile survives closing and reopening the game.
Startup failures are logged to `sdmc:/switch/NFSHP2-Switch/Saves/NFSHP2_log.txt`.

## Race performance logs

`render.timing_window` and `render.phase_window` are written every five seconds
while frames are submitted. The second line breaks CPU time down into world
thinking, driving, collision, effects, replay and race bookkeeping, audio, input,
scenery, and resource service. The averages are milliseconds of accumulated
work per submitted frame, so they include logical steps between presents. Peak
fields measure a single call or the longest interval between accepted presents.

## Switch LTO and PGO experiment

Keep the ordinary `build-nx/NFSHP2-Switch.nro` as the baseline. Configure these
variants with the same toolchain and Release options as the README command.
If that command uses `HP2_ICON_PYTHON`, pass the same value to each variant.

| Build folder | Extra configure options |
| --- | --- |
| `build-nx/lto` | `-DHP2_SWITCH_LTO=ON` |
| `build-nx/pgo-train` | `-DHP2_SWITCH_LTO=ON -DHP2_SWITCH_PGO=generate` |
| `build-nx/pgo-use` | `-DHP2_SWITCH_LTO=ON -DHP2_SWITCH_PGO=use` |
| `build-nx/pgo-only` | `-DHP2_SWITCH_LTO=OFF -DHP2_SWITCH_PGO=use` |

Run the training NRO in full application mode with the usual PAL 1.00 game
data. Spend most of the session racing with traffic and opponents. Use a few
menu and loading transitions too. The training build instruments scene capture
and Vulkan preparation, with cumulative snapshots saved at startup, every
20 seconds, on loss of app focus, and on clean exit, under
`sdmc:/switch/NFSHP2-Switch/Saves/pgo/`. After training, press HOME and return to the
game once. A `pgo.saved` line in `NFSHP2_log.txt` names the finished stream.
Use a checkpoint collected during or after racing. The training NRO carries
instrumentation and is only for collecting counters.

To prepare the optimized build, merge the newest `.stream` from each
separate game launch into a fresh profile-use directory. For files
`nfshp2-<session>-<checkpoint>.stream`, select the highest checkpoint number
for each session. Older files from the same launch include overlapping counts.
Run the matching devkitA64 `gcov-tool` under MSYS2 (or the shell used to build):

```sh
mkdir -p build-nx/pgo-use
(cd build-nx/pgo-use && aarch64-none-elf-gcov-tool merge-stream /path/to/nfshp2-session-3.stream)
```

Repeat the merge command for other training launches if needed. It writes
`profile-data/` into that build directory. Configure and build the `pgo-use`
variant with the same source revision and devkitA64 version used to train it.
Keep GCC's profile mismatch checks enabled to detect stale profiles.
In the combined build, the four profiled source files are compiled with PGO and
the other files use LTO. devkitA64 GCC 16.1 crashes during whole-program LTO
analysis if the profiled files also use LTO. For a PGO-only build, merge the
same streams from the `build-nx/pgo-only` folder and use its options above.

Benchmark the baseline, LTO-only and optimized NROs at the same clocks,
settings and 720p resolution, using the same replay. Save each
`NFSHP2_log.txt` before launching another build; the game opens a new log at
startup. Compare wall-clock FPS, `cpu_capture_ms`, and `cpu_gather_packets_ms`.

## Performance constraints

Keep the Switch CPU at its stock clock. Handheld mode can use the 460.8 MHz
GPU APM profile. When race traces show CPU stages limiting frame rate and GPU
headroom, prioritize moving measured rendering work to the GPU. Compare the
same race before and after changes using the timing windows above.

## Runtime performance settings

The first launch creates `config.txt` next to the NRO. Gameplay defaults to a
30 FPS cap and an FPS counter set to Off. Both are available in frontend and
pause Display settings and saved when leaving the screen. See the README for
config values and the fixed scene profile.

Capped-out race frames retain logical updates and presentation side effects,
while suppressing race geometry capture. The cap includes capture time in its
interval. It does not change the simulation clock, video-blank notifications,
audio sample rate, or replay time.

Main-view distance is permanently 0.3, mirror distance 0.5, vehicle LOD 1.75,
and scenery near-detail hysteresis 35/50. Adaptive/emergency tiers and their
configuration switches have been removed. Existing legacy keys are ignored.
Scene quality remains normal, independent of frame rate and the chosen cap.
`audio.backend_window` separates music decode, device command processing,
software mixing/PCM conversion, and output-service work. These are accumulated
milliseconds per fresh submitted frame across the same five-second window;
logical audio steps between submissions are included. Movie decoding happens
in device command processing. `voices_peak`, `voice_buffers`, and `buffers`
help distinguish a busy mixer from expensive command/decode work. Subtract
backend timings from `render.phase_window audio_ms` only approximately: the
latter also includes game sound logic and measurement scopes differ slightly.

The Switch mixer processes invariant rate/channel state once per PCM chunk
and advances constant sustain/silent envelopes once per buffer. Interpolation,
voice timing, sample rate, and audible content remain the same.

## Controller routing smoke test

Test menu navigation and a one-player race with a controller assigned to No2
or higher while No1 is disconnected, then with handheld controls. Two connected
controllers should both operate player one outside a confirmed two-player
Challenge session. Check that changing controllers does not leave the previous
controller vibrating.

In Challenge, choose two players and confirm a match. The libnx controller
connection screen should appear before car selection even with two controllers
already connected. Verify cancellation, P1/P2 isolation, a horizontal single
Joy-Con per player, and return to shared input after backing out to Challenge
or the main menu. HOME/applet time must not fast-forward the simulation.

Check Frame Rate and Show FPS at the top of both Display menus, including
scrolling down to the last original option and back. The initial Press Start
screen should show `Port made by: HDErick` below the original copyright when
the title image is visible, and hide it while introduction movies play.

## Language selection

The PAL archives provide English, French, German, Italian, Spanish and Swedish.
Native menu text and layout now resolve the selected table independently of
whether localized speech is loaded. Both the startup selector and Options use
this same path. Literal port additions such as Frame Rate, Show FPS and the
port credit currently remain English.

`language.selected` records the requested language and startup/menu context.
`language.applied` records the loaded table, language ID, string count and
localized-speech state. IDs are 0 English, 1 French, 2 German, 3 Italian,
4 Spanish and 5 Swedish. A normal PAL table has 2,437 strings. These events
are written on language changes, rather than on each rendered frame.

## Selected upstream fixes

Adapted from Romalu86/NFSHP2_NativePS2Port commit
`136ac47d731edc86584b50de2efb0c9314e13e68` (Release Update):

- Geometry revision stamps skip repeated byte comparisons for unchanged decoded
  meshes. Topology/UV repair and car damage stamp changes; animated copies and
  unstamped dynamic geometry retain exact byte validation.
- Reuse the last resolved Vulkan pipeline when the complete pipeline key matches.
  Pending async pipelines retain the existing skip-draw behavior.
- Sample the mapped environment target with opaque alpha, including history,
  matching the original framebuffer-alpha behavior for car reflections/glass.
- Accept terminated short inline text in localized bundles within chunk bounds.
- Release the prior menu movie before title playback and track the actual
  selected title-movie target.

Switch menus, controller mapping, renderer resolution, async shader behavior,
CPU/GPU clock policy and current gameplay settings remain owned by this port.
The PC interface, desktop controls, DualSense output and new graphics options
were not imported. Descriptor-indexed batching and the new particle backend
require a separate Switch adaptation and hardware validation.

The PAL language selector is shown on every boot. Explicit choices are held
only in memory for that session and never read from or written to `config.txt`.
Legacy `language=` config entries are ignored. Startup uses the original
English bootstrap table until a language is chosen. Both the startup screen
and Options update the session choice, which takes priority over a saved
profile's language. `language.profile_override` records a profile mismatch
against that session choice, preserving the fix for Spanish reverting to
English after a profile load.


## Chase effects and camera history

World and Frame effects share native free lists for the original 1,000 particle
and 400 group slots. Allocation and pool exhaustion no longer scan occupied
slots for every spawn. Live list order, simulation, particle limits and visual
definitions are preserved. Standalone externally supplied pools retain the
linear allocation fallback.

The five-second `render.chase_window` event separates nearby-car interactions,
controller surface feedback, particle updates and car surface effects. Timings
are milliseconds per presented frame, including all simulation steps in that
frame. Cop counts, cops within 150 metres, particles and emitters are averages
per simulation step. These complement the existing collision, capture, Gather,
audio and GPU timings without per-particle logging.

RT scout and LT orbit camera history uses the clean scene immediately before
the radial mask/history blend. A current-scene snapshot is taken there, and is
published to previous-frame history after the old history has been sampled.
Neither the HUD nor the history-composited result enters the next snapshot;
this prevents opaque blur-mask regions from retaining the entry frame forever.
The first historical blend on entry is omitted until that clean source exists.
History refreshes on every newly captured submitted image, including images
captured between logical simulation ticks at the 30 FPS cap. Active display
history rejects prepared-frame replay so its temporal input cannot become stale.
Normal frames without a history reader do not copy or split the scene pass.
FXAA reuses feedback only after history publication. `render.history_window`
reports active frames, clean copies, entry seeds and captures without a logical
tick; active fresh frames and clean copies should match. The visible RT/LT
result still needs console validation.


## Renderer diagnostic summaries

Three extra summaries are written with the existing five-second timing window:

- `render.capture_detail` separates view/menu setup, background, world capture,
  particle capture, solid chains/cars, alpha sorting/mirrors/dashboard, screen
  effects and HUD. Times include all logical captures and are divided by the
  number of presented frames. `emitted` counts captures that requested race
  geometry; `discarded` counts state-only captures with geometry suppressed.
- `render.submit_detail` separates preflight checks, swapchain acquisition,
  completed-frame cleanup/command-buffer reset, texture retirement, target
  setup, geometry uploads, draw-plan/transform-table construction, final
  effects/blit/depth transfers, and command-buffer finalization. Existing
  pipeline, command-recording and queue-submit times remain in
  `render.detail_window`. Gather and the explicit GPU fence wait are excluded
  from these submission phases. `other_ms` is the remaining unaccounted time.
  `retire_capture_ms` measures CPU capture release and renderer bookkeeping
  after queue submission, outside the existing record/submit interval.
- `render.gather_detail` samples one submission in 32. Category times and the
  `commands` tuple are means over sampled Gather calls, in this order: cached
  scenery candidates, other solids (including cars), particles, world polygons,
  UI, and screen effects. `sampled_frames=0` means no detailed category sample
  landed in that window. Timing reads occur at category transitions rather
  than at every strip. Vertex-job and resident-index preparation times, cold
  job counts and cold vertex counts use all presented frames as their divisor.

These are CPU wall times. Swapchain acquisition and driver calls can include
blocking, so the GPU timestamps and explicit fence-wait metric remain necessary
when interpreting them. No extra GPU waits, per-draw log writes or game options
are introduced. Existing resolution, clocks, LOD and rendering behavior apply.


The per-frame transform table uses an exact-key hash lookup rather than an
ordered tree. First-use order still determines table offsets; all 22 source
words (including float bits and shading index) participate in equality.
No draw order, transform precision, material or LOD changes are made.


## Slow-frame diagnostic build

Diagnostic logging is disabled by default. Configure
`HP2_SWITCH_DIAGNOSTICS=ON` to collect the following records. This does not
change the fixed scene profile.

`render.slow_frame` records each accepted presentation interval over 50 ms.
Its `frame` is the current CPU/Vulkan submission ID. Timing covers the entire
interval, including deferred logical updates, discarded captures, the final
capture, and resource service after submission. World/audio/resource peaks
are local to that presentation interval, independently of summary resets.
`tick_ms` includes all application ticks since the previous accepted frame;
`outside_tick_ms` exposes work or delays between those ticks. `poll_ms`,
`wait_ms`, `diagnostics_ms` and `tail_ms` help account for the rest. Capture,
simulation and renderer subfields overlap their parent durations and must not
be added together as independent costs.

`gpu_frame` explicitly identifies the completed GPU submission whose timestamp
is available. It can precede the CPU `frame`; no extra GPU wait is introduced.
`render.slow_gpu` separately reports completed GPU durations over 50 ms, once
per submission, even when the current CPU presentation interval is shorter.

`distance` and `next_distance` remain 0.3 in the Switch build; the legacy
`emergency` and `quality_changed` fields remain zero. HOME/controller-applet timing
epochs clear the interval baseline so user interaction is not reported as a
game stutter. Rows are buffered in RAM and written with the existing five-second
summary flush or shutdown; a roughly 64 KiB bound forces a batch write if needed.
This avoids a file flush for every slow frame. The diagnostic logging duration
is measured too, including a buffer-full write affecting the next interval.


The Switch log now uses a low-priority asynchronous writer. The previous
foreground summary flush caused measured 305 ms and 650 ms stalls, followed
by simulation catch-up. Formatting and bounded enqueueing stay on the game
thread; file writes and flushes run on the worker. A 256 KiB queue drops excess
diagnostic bytes instead of waiting for a blocked SD card and reports
`log.dropped bytes=...` when writing resumes. Shutdown drains retained records.
`log.backend asynchronous` confirms this logger is active. This change keeps
the fixed 0.3 distance, normal scene quality and slow-frame measurements.


Release builds default to `HP2_SWITCH_DIAGNOSTICS=OFF`. The
`build-nx/pgo-use-no-logs/NFSHP2-Switch.nro` build creates no NFSHP2 log file, starts
no log writer, and omits timing summaries, slow-frame records, sampled draw
category/shadow timing and detailed GPU pass sampling. Basic presentation/FPS
timing, game saves/configuration and the shader cache remain functional. It
uses the same PGO training data, fixed 0.3 distance and normal scene
quality as the diagnostic build. An existing log file is left untouched.


## Bundled Vulkan pipeline cache

`VulkanPipelineCache.bin` is a tracked startup seed captured from this renderer.
`tools/nx/embed_pipeline_cache.py` validates its version-one header and emits
an immutable C++ byte array in the build directory. `src/nx/pipeline_cache.cpp`
links those exact bytes into the NRO; no external ROMFS or runtime seed copy is
needed. Replacing the root file makes CMake regenerate the embedded array.

At device creation, both the SD cache and the bundled seed must match the live
Vulkan vendor/device IDs and pipeline-cache UUID. If both are usable, the SD
cache is the primary owner and the seed is merged into it before pipeline
workers start. Identical blobs are not merged twice. A rejected SD cache falls
back to the seed, and rejected or incompatible sources fall back to an empty
cache. The temporary merge source is destroyed after use. Normal shutdown
continues saving the expanded cache to `Saves/VulkanPipelineCache.bin`.

Pipeline caches accelerate compilation for states they contain; they do not
pre-create every Vulkan pipeline object or guarantee coverage for every scene.
See the [Vulkan pipeline-cache documentation](https://docs.vulkan.org/spec/latest/chapters/pipelines.html).
