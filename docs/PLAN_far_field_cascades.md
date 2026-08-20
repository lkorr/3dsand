# Plan: far-field cascades — kilometer view distance for the raymarcher

Status: phase 1 in progress. Whoever implements this owns updating DESIGN.md in
the same commits (CLAUDE.md: a change that contradicts DESIGN.md must update it
or not happen).

---

## 1. The problem

The residency window is a 256³-voxel cube (256·kVoxelMeters per edge — 32 m at
the 0.125 m voxels this was designed at, 179 m at the current 0.7 m); the
player is recentered, so the horizon is ~half a window edge away and the
window edge is a hard cut to sky (`raymarch.wgsl` returns `skyColor` on window
exit). DESIGN.md §3 called this an open problem and deferred it; §9 planned
"distance fog + LOD bricks for far terrain, later". This is that work.

Growing the window is not the answer: 512³ costs 8× memory and sim dispatch and
only doubles the distance. The sim window is the right size for the *sim*; the
renderer needs its own, cheaper representation of everything beyond it.

## 2. Prior art (what raymarched microvoxel engines do)

- **Teardown** (shipped): dense occupancy volume with a 3-level mip chain,
  DDA that takes coarse steps through empty mips — proof that mip-DDA over
  dense data carries a fully dynamic destructible game.
- **Voxel clipmaps** (NVIDIA VXGI patent; Godot Voxel Tools "Clipbox"; Douglas
  Dwyer's WebGPU engine): nested toroidal volumes centered on the viewer, same
  cell count per level, 2× cell size per level — memory O(levels), view
  distance 2× per level. The standard answer for *editable* worlds.
- **SVO/DAG** far fields were rejected everywhere the world is editable:
  pointer-tree incremental update cost + GPU allocator complexity buy nothing
  over flat cascades at this scale.
- The consensus from devs who shipped kilometers: view distance "is a data
  loading problem, not a rendering problem" — generate the far bands directly
  at 1/N resolution from worldgen (the "sieve"), never by downsampling full-res
  data you don't have resident.

## 3. Design

K cascade levels around the residency window (6 at phase 1; **8 since phase
4**, the ceiling the default WebGPU storage-binding limit allows). Level k
(1-based) has cells of 2^k fine voxels and a box edge of 2^k window edges, so
the outermost half-extent is **2^K× the window radius** at any voxel scale —
256× = 2048 m at the current 6.25 cm voxels. At the current
kVoxelMeters = 0.7 (window edge 179 m):

| level | cell size | box edge | half-extent from player |
|---|---|---|---|
| 1 | 1.4 m | 359 m | 179 m |
| 2 | 2.8 m | 717 m | 359 m |
| 3 | 5.6 m | 1.4 km | 717 m |
| 4 | 11.2 m | 2.9 km | 1.4 km |
| 5 | 22.4 m | 5.7 km | 2.9 km |
| 6 | 44.8 m | 11.5 km | **5.7 km** |

Each level is a 256³ toroidal volume — deliberately the same shape as the
residency window so ALL of the existing addressing (`cellIndexW`,
`chunkIndexW`, `inWindow`, `slotToWorldChunk`, the POT masks) is reused
verbatim, just interpreted in "level cells" instead of fine voxels. A level-k
cell covers 2^k fine voxels per axis; a level-k chunk is 16 level cells =
2^k fine chunks; each level has its own origin in level-chunk units.

**Storage (render-only, never read by the sim):**
- `farVox` — 1 byte material ID per cell, packed 4/u32, levels concatenated:
  6 × 16 MB = **96 MB**. Byte 0 = air. Only non-gas materials are stored
  (liquids render as opaque surfaces at distance; smoke doesn't exist out
  there). Material IDs are clamped to 255 (33 exist today).
- `farOcc` — per level-chunk non-air count, 6 × 4096 u32: the same
  empty-space-skip trick the fine march uses.
- `farUBO` — per-level origins (vec4<i32>, w unused), shared by fill + render.
- `farList` — fill work queue: one u32 per level-chunk, `(level-1) << 12 | slot`.

**Fill (the sieve):** a `far` entry point in `worldgen.wgsl` — it lives there
to share `genCell()`, which is already a pure function of (world cell, seed).
One 64-thread workgroup per level-chunk; each thread owns 64 *consecutive*
cells (16 whole u32 words — no partial-word write races, no atomics), samples
`genCell` at the fine-voxel center of each coarse cell, and the workgroup
reduces its non-air count into `farOcc` (same shape as `genChunk`).
Center-sampling means features thinner than a coarse cell vanish at distance —
correct behavior for an LOD, not a bug to fix.

**CPU manager (`src/sim/farfield.{h,cpp}`):** per level, recenter toward the
player with hysteresis (in that level's chunk units — level k shifts 2^k×
less often than the streaming window), enqueue the incoming plane (256
level-chunks) on shift, full refill on init/load/regen. Fills are dispatched
inside the tick submit (`EncodeFarFill`), capped per tick; the queue drains in
FIFO order so a full refill (24,576 level-chunks ≈ 6 worldgens) amortizes over
~12 ticks. Buffers are zero-initialized (= air), so unfilled regions render as
sky, never garbage.

**Render:** no new pass. When the fine march exits the window without a hit
(and not media-saturated), `traceFar` continues the same ray: for each level,
clip to the level box, start at max(entry, previous level's exit) — the
t-ordering automatically skips the region covered by finer data — and DDA in
level-cell coordinates with `farOcc` chunk skipping. Hits shade with the
material palette, face term, N·L (no shadow rays in the far field — fog eats
the difference), emission, and fog; `t` converts back to fine-voxel units so
depth and fog use the existing math.

**Fog** becomes a uniform (`RenderParams.fogDensity`) pinned so opacity ≈ 1 at
the outermost level's half-extent (4.5 / 1024 m ≈ 0.0044/m) instead of the
hardcoded 0.0128/m. Phase 3 makes it adaptive (track the actually-filled
radius during fast motion).

**Pipeline plumbing:** `renderBGL_` grows bindings 4–6 (farVox, farOcc,
farUBO; fragment-only — `debris.wgsl` doesn't declare them, which is legal).
The fill pipeline pairs `simSlimBGL_` (group 0 — the `far` entry statically
uses only materials + TickParams) with a new `farBGL_` (group 1), staying
far under the 16-storage-buffer layout limit. `TickParams` spare `_p1`
becomes `farCount`; `RenderParams` spare `_p1` becomes `fogDensity`.

## 4. Why this is legal under the three rules

1. **Determinism:** cascades are derived, render-only data. `farVox` is bound
   in no sim pipeline, feeds no voxel state, and is excluded from the world
   hash. The fill kernel reads only (coords, seed) through `genCell` — the
   same integer path worldgen already uses.
2. **Cost scales with activity:** an idle world dispatches zero fill work;
   levels shift only when the player moves (level k at 1/2^k the window's
   rate). Render cost is bounded per ray by chunk-skip; it replaces sky
   pixels' wasted window traversal with useful far traversal.
3. **MutationQueue:** nothing mutates the world. The two sanctioned snapshot
   paths (worldgen, LoadWorld) are joined by a third *derived* path that
   writes only non-sim buffers.

## 5. Phases

1. **This plan:** constants + buffers, sieve fill, CPU manager, raymarch
   continuation, pinned fog. Exit: stand on a hill, see ~1 km, selftest
   passes (hash unchanged, sleep unchanged, perf gate holds), screenshot
   shows a horizon.
2. **Edits at distance — IMPLEMENTED (2026-08-19).** `worldgen.wgsl fardown`:
   one workgroup per entry of the compacted dirty list, dispatched indirect
   off the SAME `dispatchArgs` the per-tick occupancy update uses (right after
   `occupancyDirty_` in `EncodeTick`), so a settled world dispatches nothing.
   Each workgroup walks levels 1..6 and, for every cascade cell whose *sample
   point* lands inside its 16³ chunk, reads that fine voxel from the LIVE
   `voxels` buffer and writes the material byte into `farVox`. Sampling the
   same center voxel as the sieve is the whole trick: downsampled and pristine
   regions agree exactly at their boundary, so there is no seam, and a chunk
   edited while resident keeps its ghost after eviction with no eviction hook.
   Cells whose center lands in a *neighboring* chunk are that chunk's job —
   at k ≥ 5 one cell is wider than a chunk, so most chunks contribute nothing
   at those levels, which is correct.
   - **Atomics:** a level-k word packs 4 cells = 4·2^k fine voxels of x-extent,
     wider than one chunk for k ≥ 2, so two workgroups race on different bytes
     of one word. `farVox`/`farOcc` are therefore declared
     `array<atomic<u32>>`; the byte update is `atomicAnd(clear) | atomicOr(set)`
     and `farOcc` gets `atomicMax(.,1)` (conservative — a stale over-estimate
     only costs marching an empty level chunk; a stale zero would hide new
     terrain). WGSL forbids one buffer being both atomic and non-atomic in a
     module, so `far`'s whole-word stores became `atomicStore` — uncontended,
     free. Legal only because cascades carry no determinism requirement.
   - **Plumbing:** `farBGL_` gained binding 4 = `dirtyList` (read-only storage);
     `farPL_` is unchanged otherwise (slim group 0 + far group 1 = 8 storage
     layout entries, well under Dawn's 16/stage).
   - **Known gap:** hash ticks (`tick % 15 == 0`) take the whole-world
     occupancy branch and never compact `dirtyOut`, so there is no work list
     and the downsample is skipped. The chunk is still dirty the next tick and
     propagates then — only an edit that lands exactly on a hash tick *and*
     settles immediately is one tick late. Accepted, documented in
     `EncodeTick`.
   - **Gate:** selftest `far downsample` paints glass into open air at
     (140,200,140), asserts all 27 covering level-1 cells read air *before*
     (guards against a vacuous pass) and glass *after* 4 ticks, reading back
     `farVox` directly (`CopySrc` added to the buffer — selftest only; the
     frame path stays readback-free). Verified to FAIL when the dispatch is
     removed, and the world hash is bit-identical with it on and off.
3. **Transition polish — IMPLEMENTED (2026-08-19).**
   - **(A) Dithered level transitions** (`raymarch.wgsl` `farDither` +
     `traceFar`). Every handoff (window→L1 and each Lk→Lk+1) is offset NEARER
     by a per-pixel hash of the fragment coordinate, scaled to up to half a
     cell of the OUTER level at that seam. The hash takes no time input (a
     crawling stipple is worse than the seam) and is keyed on screen space,
     not world space — the seam is a screen-space artifact of camera-centered
     boxes, and a world-space key would re-align into arcs as the boxes
     recenter. Cost is zero: each pixel still marches exactly one level per
     stretch.
   - **The offset must be one-sided (nearer only).** The levels tile t-space
     exactly, so pushing a handoff FARTHER opens a band no level marches and
     rays fall through it. Measured: a two-sided ±half-cell jitter punched
     ~3.2k pixels of hole-speckle through tree edges in the 1080p far view;
     the one-sided version has none while still perturbing ~4.4k seam-adjacent
     pixels. `evidence_dither_baseline.png` / `_twosided_holes.png` /
     `_onesided_clean.png` (repo root, phase-3 working set) are the same crop
     under all three conditions; the two-sided one is visibly speckled.
   - **(B) Adaptive fog** (`FarField::SafeRadiusMeters` +
     `WriteRenderParams(..., fogDensity)`). `FarField` keeps a per-level
     pending-fill counter alongside the FIFO queue (incremented on enqueue,
     decremented as `PrepareTick` pops). The trusted radius is the half-extent
     of the level below the INNERMOST incomplete one — boxes are nested, so a
     gap at level k makes everything from level k's half-extent outward
     suspect. Fog density = `kFogOpticalDepths / safeRadius`, clamped to
     `[kFarFogDensity, kFarFogDensityMax]` (never thinner than the
     full-horizon pin; never so thick that the residency window itself is
     hidden — the ceiling is pinned to level 2's half-extent, 4× the window
     radius) and eased toward its target at `kFogLerpPerFrame` so the horizon
     opens smoothly instead of stepping with each landed plane. All three
     constants live in `world.h`. `WriteRenderParams` defaults the parameter
     to `kFarFogDensity`, so every selftest call site (which renders against
     fully-filled cascades) is unchanged.
   - **(C) Coarse cave suppression — ASSESSED, NOT WARRANTED.** Rendered the
     cascades from 900 voxels up with fog at 3% of nominal, so levels 4–6 fill
     the frame across kilometers: the coarse surface is completely solid, no
     swiss-cheese on any hillside. This is structural, not luck — `caveAt`
     carves *enclosed column bands* whose ceiling is capped at `h - 10`, so a
     cave never breaks the surface and a coarse cell's center sample lands in
     terrain, never in a cave void, wherever the surface is. Suppression would
     have been a no-op that risked the sieve/`fardown` boundary agreement the
     `far downsample` gate protects. Revisit only if worldgen ever gains
     surface-breaching caves or overhangs.
   - **Selftest addition:** `screenshot_far.bmp` — an elevated near-level
     camera at (140,130,140) whose frame is mostly cascade. The standard
     `screenshot.bmp` looks down at the near forest and shows almost no far
     field, so it could not have caught any of this.
4. **Distance look — IMPLEMENTED (2026-08-19).** The v0.5.4 far field was
   structurally sound but visually dismal: a milky white-out past ~150 m (fog
   pinned to a 512 m horizon), distant hills reading as gray stone with green
   contour stripes, and flat unshadowed shading that made LOD terrain look
   like a different (worse) world than the near field. Survey of shipped
   microvoxel engines (Teardown frame breakdowns, Dwyer's Octo, Distant
   Horizons/Voxy, GigaVoxels/ESVO lineage) says the seam artifact that
   matters is SHADING consistency, not geometry. Changes:
   - **kFarLevels 6 → 8**: horizon 512 m → 2048 m. farVox is now 128 MiB —
     exactly the WebGPU default maxStorageBufferBindingSize, so this is the
     ceiling without raising limits. Fog pin follows automatically
     (kFarFogDensity derives from kFarLevels): density fell 4×, which is what
     actually removed the white-out.
   - **Surface skin (worldgen.wgsl `farSurfaceMat`)**: cell shape still comes
     from the center sample, but the topmost solid cell of a column
     (center ≤ h < center + 2^k — NOT "span contains h", which misses half of
     all surface cells) recolors with `genCell(x, h, z)` — the 1-voxel
     grass/sand/snow skin a coarse center sample almost never hits. Shared
     verbatim by `far` and `fardown`, preserving their agreement invariant
     (the skin lookup is pristine-procgen on both sides; an edited chunk's
     rim keeps a slightly stale skin color while its center voxel survives —
     invisible at cascade distances, unlike a seam).
   - **Canopy flattening (`treeCanopyAt`)**: at levels ≥ 5 (cells 2 m+), a
     surface cell under a tree crown's XZ footprint takes the leaf material —
     the "flatten props into the macro chunk" trick; distant forest keeps its
     canopy color after individual trees stop surviving the sieve.
   - **Far shading (raymarch.wgsl)**: same constants as the near field, plus
     `farShadowed` — one occupancy-skipped DDA toward the sun at the hit's
     own level, SOFT (lambert ×0.3): a debug-tint pass showed the casters are
     mostly single-cell terrace steps, and a hard shadow term renders as
     ant-trail speckle. One-sample AO from the cell above; palette jitter
     re-keyed to a fixed ~0.5 m world frequency (per-cell keying flattened
     coarse cells into single-color slabs); sky reflection on water top faces.
   - **Aerial perspective (`applyAerial`)**: fog converges surfaces exactly to
     `skyColor(rd)`; the old `×0.9` target kept every distant surface slightly
     darker than the sky behind it — the "gray veil" look.
   - **Sun lowered to ~41°** (main.cpp): at 52° shadows were 1–2 cells long
     and the world lit flat.
   - **`--shot` mode (main.cpp `RunShots`)**: worldgen + cascade fill +
     3 standard screenshots in ~15 s, for look iteration without the full
     selftest. `screenshot_ground.bmp` (eye-height, horizon in frame) joined
     the selftest captures — the elevated shots kept hiding first-person
     artifacts.
   - Verified: full selftest PASS, world hash bit-identical with all of the
     above on (d936c328) — the sim is untouched by construction. 1080p
     shadows-on 5.2 ms/frame (193 fps) on the RTX 3060 Ti.
5. **512³ residency window + far-grid decoupling — IMPLEMENTED (2026-08-19).**
   The simulated world doubled to 32 m per edge (kWorldN = 512; voxels buffer
   512 MiB — exactly the storage limits context.cpp requests, and the ceiling
   without raising them). The cascades moved onto their OWN kFarN = 256 grid
   (`FAR_*` prelude constants; `farCellIndexG`/`farChunkIndexG`/`farInBox`/
   `farSlotToChunk` in common.wgsl; kFarNChunk-based math in farfield.cpp):
   level k cells span 2^(k + kFarShiftBase) fine voxels, the shift base pinned
   so a level's box edge is always 2^k WINDOW edges. Consequences: cascade
   memory stays 128 MiB whatever the window size, every cascade distance
   scales with the window (outermost half-extent now 4096 m), and reverting
   kWorldN to 256 reproduces the old geometry exactly (shift base 0). The
   `far downsample` gate re-derives farVox indices with far-grid math and
   paints radius 12 so the 3×3×3 level-1 cell block (cells are now 4 fine
   voxels) stays covered. The sleep gate's settle budget became adaptive
   (tick until quiet, hard cap 3000) — 8× the resident content legitimately
   needs more than the fixed 500 ticks tuned for 256³. Selftest PASS end to
   end at 512³: sleep 0/32768 after ~500 ticks (with the disc-pond worldgen
   rework that replaced leaking basin-contour ponds), 1080p shadows-on
   8.8 ms/frame (114 fps) on the RTX 3060 Ti.
6. **Later:** beam optimization (⅛-res depth prepass) if far-march cost ever
   shows in profiles — the strongest structural win per the survey (ESVO-style
   low-res conservative pre-pass; also lets near-field-occluded pixels skip
   the far march entirely); cascade persistence alongside region files;
   ray-guided fill priority (GigaVoxels-style usage feedback through the
   existing async readback ring); water-surface flattening in the sieve
   (shoreline terrace rings come from the per-column water table, not the
   renderer).

## 6. Known accepted limitations (phase 1)

- Far terrain is *pristine procgen*: player edits are invisible beyond the
  window until phase 2. The window edge can pop where recent edits meet the
  cascade (16 m away, at the old draw distance — strictly better than the
  current sky cut).
- Center-sampling aliases the surface by ±half a coarse cell. Phase 3's dither
  breaks up the *seam lines between levels*; the terracing WITHIN a level is
  inherent to the representation and is not addressed (a blend band would mean
  marching two levels per pixel). At the current kVoxelMeters = 0.0625 the
  nominal fog reaches full opacity at 512 m, well inside level 4, so levels 4–6
  are effectively never visible and the outer seams are moot.
- Level boxes recenter independently with hysteresis; containment margin is
  large (each box is 2× the previous) but not formally guaranteed — a
  non-contained sliver would render as a sky wedge for a frame. Not observed;
  revisit only if seen.
- Cascade fill is seed-deterministic but NOT hashed: two clients always agree
  on it (pure function of seed), so multiplayer needs no cascade sync.
