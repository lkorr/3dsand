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

K = 6 cascade levels around the residency window. Level k (1-based) has cells
of 2^k fine voxels and a box edge of 2^k window edges, so the outermost
half-extent is **64× the window radius** at any voxel scale. At the current
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
2. **Edits at distance:** dirty-driven downsample of fine chunks into the
   cascades (a chunk evicted from the window leaves its downsampled ghost),
   so craters/buildings survive in the far field. Rides the existing dirty
   list; still render-only.
3. **Polish:** dithered blend band at level transitions, adaptive fog radius
   during fast motion, cave-band suppression at coarse levels if swiss-cheese
   artifacts show on cliff faces.
4. **Later:** beam optimization (⅛-res depth prepass) if far-march cost ever
   shows in profiles; cascade persistence alongside region files; distant
   prop baking.

## 6. Known accepted limitations (phase 1)

- Far terrain is *pristine procgen*: player edits are invisible beyond the
  window until phase 2. The window edge can pop where recent edits meet the
  cascade (16 m away, at the old draw distance — strictly better than the
  current sky cut).
- Center-sampling aliases the surface by ±half a coarse cell — visible as
  gentle terracing at level seams until phase 3 dithers it.
- Level boxes recenter independently with hysteresis; containment margin is
  large (each box is 2× the previous) but not formally guaranteed — a
  non-contained sliver would render as a sky wedge for a frame. Not observed;
  revisit only if seen.
- Cascade fill is seed-deterministic but NOT hashed: two clients always agree
  on it (pure function of seed), so multiplayer needs no cascade sync.
