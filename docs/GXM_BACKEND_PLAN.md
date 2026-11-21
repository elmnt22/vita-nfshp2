# Native GXM backend plan (replace vitaGL)

Status: research done 2026-11-21. Waiting for the user's go on Phase 0.

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
