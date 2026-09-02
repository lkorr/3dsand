# PLAN: indirect light — openness grid, then irradiance

**Parent:** `docs/PLAN_lin_followups.md` (Wave 3). **Source:** `docs/RESEARCH_john_lin.md`
§2.4 (photon map + final gather, his settled answer), §2.6 (denoising), §12 (why
"re-light where matter changed" is not enough for us). **Written:** 2026-09-01 against
`73bc9c6`. Each phase is judged by eye AND by `--render-budget` before the next starts.

## 0. Why (and the look test)

Shading today is `albedo * face * (ambientAt(n) * ao + sun)` (`raymarch.wgsl:6715`).
`ambientAt` (`:2273`) is a hemisphere lerp on `n.y` between `TUNE_AMB_GROUND` and
`TUNE_AMB_SKY`, with a night twin and moon terms — **zero spatial term**. `voxelAO` is three
taps in the plane of the face. `heatSpill` (`:4889`) is the only indirect term in the engine:
four taps along the normal, molten cells only, ~6 voxels reach. Nothing knows a cave is dark,
a room is lit by its window, or that grass under a canopy is in shade. That is the largest
visual gap in the engine.

*(W1-B's zero-jitter look test paragraph goes here when it lands: does the world read flat
without palette variety and grain, i.e. is colour speckle doing the job lighting should?)*

## 1. Verified constraints that shape the design

- **The shadow cache cannot carry the payload.** Its state word is fully allocated
  (`value 8 | resolvedFrame 4 | requestedFrame 4 | valid 1 | verifier 15`,
  `common.wgsl:3043`); its key is a 32-bit hash of a **per-voxel-face sub-patch** (2.5 cm at
  subdiv 4), far finer than a lighting grid wants; and the resolve pass is
  **locality-bound** (22 ns/ray vs 1.7 ns coherent, `shadow_resolve.wgsl:32-63`) — adding a
  second march per request costs more than the arithmetic says.
- **The 4³ grid already exists.** `kSubOccShift=2`: 64 blocks per chunk, two classes
  (total, blockers), in the tail of the `occupancy` buffer (`world.h:495-506`,
  `common.wgsl:2838-2889`), maintained by `sim_occupancy` on every dirty walk, barriers and
  bind-group entries already wired everywhere. "Surface-bearing block" ≈ blockers bit set and
  not all eight neighbours full. Sentinel chunks (`PT_SENTINEL_BIT`) are uniform and need no
  entries.
- **A scalar per block leaks** (Lin tried probes, then planes, then landed on anisotropic
  faces). Our world is full of 1-voxel walls, floors, trunks. **Six values per block**, one
  per face; a receiver reads only the face that faces it.
- **The sun moves and the CA moves.** His world does not evolve without input, so
  "re-light where matter changed" was enough for him. For us the grid ages while idle: every
  sun-dependent phase needs a rolling refresh budget, not just the dirty list.
- **Determinism:** trivially excluded. A render buffer written by a render-side pass, never
  read by the sim; `determinismHash` must not move in any phase, and that is each phase's
  cheapest correctness proof.
- **Raster paths shade with `ambientAtP`** (`common.wgsl:1227`) — mobs, microbodies,
  debris. Whatever replaces `ambientAt` must reach them too or a mob glows in a cave.
- Memory today: `farVox` 1024 MiB, `shadowCache` 8 MiB, `shadowReq` 4 MiB, occupancy
  640 KiB. `world.h:820` notes ~5 GiB committed on an 8 GiB card. A dense 12.6 MiB byte grid
  is noise; a dense 50 MiB RGB grid is acceptable; anything per-voxel is not.

## 2. P0 — openness (sky visibility) grid

**Plain words:** caves get dark, overhangs get shade, rooms get dim. The engine finally knows
how much sky each surface can see.

**Data.** `openness : array<u32>` viewed as bytes, index
`((slot * 64 + block) * 6 + face)`, `kNumChunks × 64 × 6 = 12,582,912` bytes. Plus
`opennessGen : array<u32>` of `kNumChunks` — the page-table generation (or the chunk's world
coord) the entries were computed for, so a chunk streaming into a reused slot invalidates the
stale bytes on read (compare, treat mismatch as "unknown", fall back to the `n.y` lerp).
Value: 0..255 = unblocked fraction of the face's hemisphere within `render.opennessReach`
metres (default ~12 m — enough that a 16 m ruin roof reads as cover and a 2 m overhang
reads as shade; sky beyond that is the `n.y` prior).

**Writer: `sim_openness.wgsl`, a new compute pass**, two rows in `pass_table.def`:
`openness_dirty` (indirect over the compacted dirty list, same args as `occupancyDirty`) and
`openness_refresh` (a fixed `render.opennessChunksPerFrame` walk of non-sentinel resident
slots, round-robin cursor in a params word, so the whole window revisits in
`kNumChunks / budget` frames). One workgroup per chunk, one thread per (block, face) =
384 threads → two dispatches of 192 or a 64-thread loop; skip a thread whose block has no
blockers bit or whose face-neighbour block is full. March **5 fixed directions** in the
face's hemisphere (the face normal, and four at ~45° toward the hemisphere's tangents) through
the **blockers-class 4³ mask** in block steps (a 4-voxel DDA over `subOcc`, chunk-skipping on
`occBlockers == 0`, sentinel-aware: `EMPTY`/air sentinel = open, blocker-material sentinel =
blocked). Openness = unblocked count / 5, biased so the normal direction counts double.
Write the byte; write the chunk's generation. Cost: bounded by the chunk budget; a dirty
chunk costs 384 threads × 5 rays × ≤30 block steps of one-word reads.

**Reader.** `ambientAt(n)` gains a spatial arm: at a near-field opaque hit, read the byte for
the hit cell's block and face (`face = axis*2 + (sgn>0)` — the same encoding the shadow cache
uses); if the generation matches, `amb = mix(ambGround, ambSky, openness)` shaped by the
existing day/night/moon mixing; else the old lerp. Start with nearest-block reads; add a
bilinear over the 4 neighbouring blocks in the face plane **only if it visibly tiles** (it
will at 40 cm blocks on flat walls — expect to need it). Far-cascade hits keep the `n.y` lerp.
`ambientAtP` takes the same byte, sampled once per raster fragment from the body's world
position (mobs are ≤2 blocks tall; one read per fragment is fine).

**Knobs** (`render.*`, TUNE pipeline, five places): `opennessReach`, `opennessChunksPerFrame`,
`opennessStrength` (0 = old lerp, for the A/B arm), `opennessBilinear`.

**Verify.** `--gate openness` in `selftest_render.cpp`: settle, run N frames so the refresh
has covered the window, read back the grid for (a) a block on the floor of the shallow cave
band under the spawn plain (find it with `caveBands` mirrored on the CPU, or place a known
roofed box through the MutationQueue and wait for its dirty walk) and (b) an open-field block;
assert `a < 0.2` and `b > 0.8` (ranges, never equality — refresh order is not deterministic
across GPUs, and it need not be). `--render-budget` gains an `noopenness` arm
(`opennessStrength=0`); the baseline delta is the cost, state it. `--shot` a cave mouth, a
ruin interior, and under a canopy; judge by eye. `determinismHash` unmoved.

**Consumers beyond lighting** (not built here, but the buffer is theirs too): worldgen
openness placement is a different thing (a generation-time closed form, W1-C); audio's "can I
hear the sky" (research §9) and the wind emitter at the cave mouth read this grid at the
listener's block.

## 3. P1 — direct injection + one-bounce gather

Only after P0 is judged good by eye.

**Plain words:** sunlight hitting green grass tints the rock next to it green; a window lights
up the room it looks into.

**Data.** `irradiance`: RGB9E5 (one u32) per block-face, `kNumChunks × 64 × 6 × 4` =
50 MiB dense. Sparse (blocks with a blockers bit) halves it in the harness window; do dense
first, measure, then decide.

**Injection.** The **shadow resolve pass is the right writer here** — it already knows each
resolved patch's cell, face and `lit` fraction. On publish, `atomicAdd`-accumulate
`albedo(word) × sunColour × lit × patchArea` into the block-face and a per-face weight (a
second u32; or pack a 16-bit count next to a lower-precision colour — decide by measuring
banding). Accumulation order is racy and that is fine: this buffer is render-only and the
result is averaged. A `sim_occupancy`-adjacent decay step (`irradiance *= 1-k` on the refresh
walk) is what makes it sun-aware: when the sun moves, old deposits fade and the currently
resolved patches re-deposit. Because the resolve pass only sees requested patches (on-screen
or near), light comes from what has been seen — the refresh walk of P0 fills in off-screen
blocks at a low rate by casting one sun ray per surface block-face (block steps, blockers
mask; cheap) and depositing the same term.

**Gather.** At the primary hit: step 3–4 blocks along the normal hemisphere through the block
grid (the P0 march, reused; 3 directions is enough for a first look), accumulate the facing
faces' irradiance weighted by `cos θ / d²`-ish and by their openness (an unlit face
contributes nothing). Add `albedo × gathered × render.giStrength` to the shade. Rough
reflections (research §2.8, distance-widened blur) are a later use of the same data.

**Verify.** `--gate gi-bounce`: a lit green floor next to a white wall in shade; the wall's
gathered term has G > R. `--render-budget` `nogi` arm. `--shot` the lake shore at noon and a
ruin interior with one doorway. Hash unmoved.

## 4. P2 — write-back (multi-bounce)

The gathered result at a receiver feeds the receiver's own block-face at low weight
(`render.giFeedback`, ~0.2), so bounces accumulate over frames — Lin's "unlimited bounces".
Requires the decay from P1 to be the same clock, or the grid brightens without bound:
`feedback < decay` must hold and be asserted in `LoadTuning`. Verify with the P1 gate run for
600 frames: the wall's term converges (last 100 frames within 5%), never diverges.

## 5. P3 — emissives

Lava and fire inject from the `sim_occupancy` dirty walk (`mainDirty`, indirect on the
compacted dirty list): for each block containing `emission > 0` cells, add
`color0 × emission × count` to its six faces. `heatSpill` becomes a special case of the
gather and is **deleted** (with its `TUNE_HEAT_SPILL_*` rows and the occupancy probe gate at
`raymarch.wgsl:6839`). Verify: `--shot` of the authored lava pool at night — the rim rock and
the far wall are lit, and the `heatSpill` arm in `--render-budget` no longer exists.

## 6. Later

- Rough reflections by distance-widened blur (research §2.8) over the irradiance grid, once
  anything glossy is authored.
- A reprojection history buffer (research §2.6) the moment any of the above is stochastic —
  shared with a half-res primary (`PLAN_lin_followups.md` §5).
- Sparse allocation of `irradiance` if the dense 50 MiB ever matters.

## 7. What this plan refuses

- Per-voxel lighting storage (rule: extra state goes to a sparse auxiliary layer, and 32 bpv
  is the budget; a per-voxel light byte is 134 MB in-window before faces).
- Putting the payload in the shadow cache slot (no bits; wrong granularity; wrong pass).
- Photon rays from the sun (the resolve pass and the refresh walk already know what is lit).
- Anything the sim can read. The grid is render data, like `farVox` and `shadowCache`.
