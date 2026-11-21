# Native GXM backend plan (replace vitaGL)

Status 2026-11-21: Phase 0 done; Phases 1-3 done as a direct GXM draw path inside vitaGL's context (Stage A, below). Replacing the rest of vitaGL is not worth it: the model shows no further gain in misses from a faster submit.

## Why

- Hard target: no frame over 33 ms during the whole race.
- The latest bench (`base42`, track 64 demo) has **114 frames over 41.7 ms (a missed 30 fps frame) per run, about 4% of race frames**. Average is 40.3 fps.
- Those misses are GPU frames of 34-45 ms, which vsync turns into 50 ms frames. Runs of them produce the 90-130 ms hitches.

Measurements (`gpu_probe`, per-window averages, ms):

| Mode | CPU submit (GL thread) | GPU after submit | Drained GPU frame (heavy windows) |
|---|---|---|---|
| normal | 8-10 | 12-27 | 31-39 |
| diag 7: 1x1 textures, trivial VS, no alpha test | 8-12 | 6-18 | 24-30 |
| diag 6: no draws | 3-8 | 1-4 | 8-15 |

- **vitaGL CPU cost.** It costs 8-10 ms per frame to issue about 500 draws, and 3-8 ms even when every draw is skipped. The research found these per-draw costs:
  - about 10-20 `sceGxmSet*` calls per draw;
  - the fragment program is re-patched whenever the blend state differs from the program's cached state;
  - the full default uniform buffer is copied into uncached RAM on every change;
  - every texture unit is rebound every draw;
  - scissor is two full-screen stencil quads per change and per scene, plus 1-2 extra draws at every scene begin;
  - `glClear` is a full-screen draw with about 15 state restores.
- **Open question (Phase 0 answers it).** Hitch frames show the GL thread waiting 30-38 ms at swap, sometimes with only about 170 draws. In those frames the GPU is still busy with the earlier frame, so the GPU is the limit, not the CPU submit.
  - On the Vita's unified shader cores (SGX543MP4), vertex and fragment work share the same cores.
  - A native backend saves GPU time only where vitaGL adds GPU work (scissor and stencil quads, clear draws, redundant state). That is probably 1-3 ms.
  - CPU submit savings (8-10 ms down to about 2-3 ms on core 2) only help frames where the GPU starves while waiting for the scene.
- **Journey's experience** (`E:/Journey Vita/docs/NATIVE_GXM.md`, `PERF_PLAN.md`):
  - Its first native layer had the same API shape as GL, and the frame time was the same as vitaGL (about 30 ms).
  - The gains came later from GXM-specific work: a bind cache, no depth store, client arrays copied once, and pre-built draw packets.
  - For us, the draw loop is already ours (`ecstasy_gl.inc` drawPass/drawOneBody over gathered `VkDrawBatch` lists). We would write GXM directly from the batches, with no GL-subset shim.

## Phase 0: decide with numbers (about half a day, 2-3 Vita runs)

Goal: split the heavy-window frame into GPU busy time and GPU starved time.

1. Add a fragment notification after the main scene and record GPU completion timestamps per frame on the GL thread. Log per-frame `gpu_done[N] - gpu_done[N-1]` (steady-state GPU throughput, not drained) next to CPU submit end times.
2. Bench `24 1` with vsync on, and again with `vsync_off.txt`. With vsync off, heavy-window frame intervals show true GPU throughput.
3. Decision:
   - **GPU starved:** heavy GPU intervals are mostly under 33 ms with vsync off, and the GPU waits for submits. Native GXM fixes most misses. Go to Phase 1.
   - **GPU busy:** heavy GPU intervals stay at 34 ms or more. Native GXM saves only 1-3 ms of GPU time, which is not enough alone. Report back. The levers are then GPU work: lower resolution in heavy frames (needs approval), cheaper cutout shading, fewer mirror draws.

### Phase 0 results (2026-11-21, `gpu_timeline.txt`, bench 24 1, track 64)

Per-frame GPU completion stamps (fence watcher thread), race frames only. Vsync on and off gave the same numbers, so vsync is not a factor.

| | vsync on | vsync off |
|---|---|---|
| race frames | 4755 | 4896 |
| GPU interval >= 33.4 ms (heavy) | 687 (14%) | 689 |
| GPU interval >= 41.7 ms | 95 | 91 |
| heavy frames where the GPU sat idle > 3 ms waiting for the kick | 458 | 462 |
| ... GL thread started only after the GPU was idle (packet late from upstream), avg 8.2 ms late, then 14 ms submit | 350 | 361 |
| ... GL thread started in time but the submit was too slow (13 ms) | 108 | 101 |
| heavy frames whose post-kick GPU work alone is >= 33.4 ms | 232 | 231 |

- Heavy frames: GL submit averages 12 ms (20 ms in > 42 ms frames). It sits on the GPU critical path.
- Projection with the same GPU work and a faster submit: x0.5 -> 301 heavy / 64 over 42; x0.3 -> 265 / 57; x0.2 -> 255 / 55.
- **Verdict: mixed.** A native submit removes about 60% of the heavy frames and about 40% of the > 42 ms misses. The rest is true GPU work (about 230 frames, ~5%) and must be cut separately: cutout discard, mirror draws, resolution in heavy frames (needs approval).
- Packets arriving late from the gather thread (350 frames, 8 ms) are a second CPU lever outside the renderer.

## Phase 1: GXM core (if Phase 0 says go)

- **Location:** new `hp2/src/vita/gxm/`, our own code. vitaGL source is reference only (LGPL; nothing copied). Journey's `rgx` and NFS Hot Pursuit 2010's `hp_gxm.c` are the user's own designs to learn from.
- **Memory:**
  - CDRAM: textures, display, depth, render targets.
  - Uncached LPDDR: fenced per-frame rings for streams, indices, instance rows and uniforms.
  - Patcher and context rings mapped READ|WRITE (`hp_gxm` lesson: read-only crashed on spilling shaders).
- **Display:** 3 buffers, display queue with flip callback, vsync wait in the callback (same 30/60 behaviour as now).
- **Shaders:**
  - Port the uber-shader to Cg once, by hand: `ecstasy_gl.inc` 90-281, plus the 4 auxiliary shaders.
  - Compile at runtime with vitaShaRK and SceShaccCg (already linked, `ur0:data/libshacccg.suprx`). Cache the GXP pairs on ux0, keyed by source hash plus a salt.
  - Prewarm the keys at boot, as now.
  - Keep the `#define` key scheme: FUNC, TEXTURING, ATEST, SHOWCASE, AFIX, CLAMPTL, GPUPOS, SHADE, AFFINE, DEPTHTEX, INST.
  - Watch the Cg/GLSL matrix conventions (Journey note) and never alias varyings with `#define` (Journey bug).
- **Programs:** vertex programs patched per (shader key, stream layout). Fragment programs patched per (shader key, blend info, colour mask). Both cached and never re-patched per draw.

## Phase 2: draw path written for GXM

- **State:** set only on change (programs, textures, depth, blend). There are about 6-10 distinct blend states, all from `glBlendFor` mapped to GXM blend info.
- **Uniforms:** written straight into `sceGxmReserve{Vertex,Fragment}DefaultUniformBuffer`, with only the fields that changed. Matrices are already per-instance attributes.
- **Scissor:** `sceGxmSetRegionClip` alone for tile-aligned rectangles; a stencil path only for unaligned ones. Measure how many are unaligned.
- **Clears:** background objects or depth-stencil background values instead of full-screen draws.
- **Instancing:** `sceGxmDrawInstanced` with the existing row table.
- **Scenes:**
  - Display scene, mirror target (id 7), offscreen targets 7-15, and half/quarter particle targets.
  - Depth force-store and force-load only on half-res frames, as now.
  - Depth-as-texture via the display depth surface (replaces `vglTexImageDepthBuffer`).
- **Sun queries:** visibility buffer (`sceGxmSetVisibilityBuffer`, increment op, fragment notification).
- **Deletes:** fenced by notification, not by frame count.
- **Screenshots:** read the colour surface after a notification wait (`shots.txt` path unchanged).

## Phase 3: parity and switch

- **Off switch:** `gxm_off.txt` selects the vitaGL path for at least two weeks. Both backends build from the same batches.
- **Parity:**
  - The existing in-renderer checks: `alpha_check`, `gpu_diag` modes and sun query logs.
  - Screenshot diffs at the same journal frames against the vitaGL build, using a control run (the same build twice) to measure run-to-run noise first. Lesson from 2026-11-21: on/off shots differed by 3k-212k pixels with no control.
  - Every track type (city, canyon, forest), mirror, half-res smoke, sun flare, HUD, menus and FMV.
- **Benches:** `intervals_over_42ms`, `gpu_probe`, per-thread windows.

## Risks

- Shader translation bugs (matrix order, precision). Mitigated by shot diffs per key.
- Memory: 280 MB newlib heap plus GPU pools; the budget must match what vitaGL takes now.
- Parameter-buffer overflow causes partial renders. Keep the 32 MB parameter buffer and memory-backed depth.
- Size: Phases 1-3 are an estimated 4-7 days of work.

## Not affected

- The game thread at race start (about 30 ms) needs its own CPU work either way.

## Stage A result: direct GXM draw path (2026-11-21, hp2 b7e97b63 + 7292cd4b)

What changed:
- Every race draw goes straight to `sceGxmDraw`/`sceGxmDrawInstanced` from the gathered batches, inside vitaGL's own context and scenes.
  - It uses the same GXP programs (`vglHp2ProgramInfo`), its own patched vertex program (stream 0 = `VkVertex` 68 B, stream 1 = instance rows 80 B) and fragment programs per blend word.
  - Uniform blocks go to vitaGL's ring; state is set only on change.
- vitaGL keeps: init, display, scenes, clears, FBOs, the half-res pass, sun queries and screenshots.
  - `vglHp2Sync` runs before the first direct draw after vitaGL work.
  - `vglHp2Invalidate` runs before vitaGL draws again.
  - The fork API is in `third_party/vitagl/hp2_vitagl.patch`.
- Switches:
  - `gxm_off.txt` selects vitaGL draws.
  - `gxm_parity.txt` with `shots.txt` takes an in-frame A/B screenshot.
  - `gxm_prof.txt` logs the pass and pre-pass profile.
  - `gxm_flush.txt N` adds a mid-scene flush (no gain).
- Submit trims: batch prefetch, instance rows written straight into the mapped stream, program/fragment lookup caches, no per-draw clock probe.

Parity: in-frame direct vs vitaGL screenshots are pixel-identical (41/41 frames, then 38/38 after the trims).

Results (bench 24 1, track 64):

| | base42 (vitaGL) | direct + trims |
|---|---|---|
| GL thread per frame (timing windows) | 8.6 ms | 4.6 ms |
| heavy-frame submit (GPU >= 33.4 ms) | 12 ms | 7.3 ms |
| avg fps | 40.3 | 41.3 |
| present intervals > 42 ms per bench | 129 | 87-109 (run-to-run noise ~20) |

### What the remaining misses are (gpu_timeline with gather stamps, draw mix, auto shots)

GPU intervals >= 41.7 ms are about 80 per bench. They come in two kinds:

1. **Gather-bound, about 40%.** Gather (render thread, core 1) takes 33-39 ms for stretches of 2 s or more.
   - This happens in wide-FOV demo views: 89° FOV, ~900 visible instances, 950-1600 draws.
   - Gather scales with draws (412 draws → 15 ms, 950 → 33-37 ms).
   - It is not resident-geometry churn (new/captured entries are 0 in these frames).
   - The PC profile of gather is flat; resident cache lookups (`FindSource`/`Acquire`/`Touch`) are the largest group.
2. **GPU-bound, about 60%.** These are crash and stuck scenes: a car against roadside trees or signs, heavy tyre smoke or dust, cops close by.
   - The GPU takes 42-47 ms per frame. Regression on GPU-busy frames (R² 0.7): ~9 ms fixed, ~5-6 ms for the half-res split, ~0.14 ms per 1000 indices, ~0.08 ms per screen of smoke.
   - Diagnostic modes on the heavy bucket (normal 30.8 ms): no smoke draws (split kept) 28.9; alpha test off 26.5; trivial SHADE vertex shader 30.5; 1x1 textures 28.7; resident geometry in CDRAM 30.5 (`resident_vram.txt`, kept off: -30 MB VRAM).
   - No single GPU cost dominates; each lever is 2-4 ms.

### What would still cut misses
- Gather: cut per-draw CPU cost in the resident cache path, or spread gather over core 2. With direct GXM, the GL thread is idle ~25 ms per frame.
- GPU: no exact lever left is bigger than ~3 ms. Visual levers (need approval): smoke and dust at quarter resolution in crash scenes, LOD bias for wide FOV, cheaper half-res split (particles in the main scene at full resolution when coverage is low).
