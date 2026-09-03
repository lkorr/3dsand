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

**Look test (W1-B, 2026-09-02, four `--shot`s of the mid-morning overlook: stock,
`grainAmp=grainAmpFar=0`, `paletteJitter` forced to `color0`, both; `build/look_*.bmp`).**
The two knobs are not equal partners: killing the grain moves the mean pixel by 0.39/255
(never more than 3/255) — invisible; flattening the palette moves it by 2.8 with peaks of
96/255. "Colour speckle" in practice means `paletteJitter`; `grainAmp`'s default does
almost nothing. Turning both off barely changes contrast where there is geometry (near-grass
crop σ 20.6→20.1, distant ridge 24.1→24.2, tree crown 36.4→36.2): on broken, vegetated,
multi-material ground the variety is carried by face normals, AO, micro strands and the
material mix, and the palette is a garnish. It reads catastrophically flatter in exactly one
place: **any large single-material flat surface** — a table top becomes a dead-uniform brown
polygon with one hard edge and no gradient, same for bare dirt or a clean wall. Verdict: the
colour variety is not hiding missing lighting across the landscape at large; it is hiding it
completely on every flat unbroken face, which is where indirect light would be the only thing
giving the surface shape. Flat single-material faces are where P0/P1 must prove themselves,
and `paletteJitter` is what would become redundant if they succeed.

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

### Verdict (2026-09-02, `--shot` frames `openness_out/in/floor`, `ground`, `tallgrass_eye`; GI off, so this is P0 alone)

Caves and rooms read dark: the stamped room's interior goes to near-black away from the door,
with a plausible gradient across the floor from the sunlit wedge back to the far wall, and the
`openness_floor` frame (straight down at the doorway from inside) shows that gradient as a
smooth ramp with no visible 40 cm tiling — the bilinear filter is doing its job. Nothing wrong
darkens: the meadow right up against the room's outer wall in `openness_out` is bit-identical
to the pre-P0 look, the terrace risers beside it shade as they did (the "no opinion" rule is
what keeps them so), and the `ground` frame's ruin walls and arch are unchanged where they
face open sky. The one honest criticism is the interior: at `opennessStrength = 1.0` a room
with a 7-voxel doorway is TOO dark — the ceiling and back wall are essentially black, which is
correct for a sky-visibility term and wrong for a lit room, and it is exactly the gap P1 was
specified to fill. The default stays at 1.0: the fix for a black room is bounce light, not a
weaker sky term that would also brighten every cave. `debris.wgsl` is wired (per-vertex,
`opennessScaleAtBody` from the cube's position, `occupancy`/`openness`/`opennessGen` widened
to the vertex stage), so an ember in a cave no longer glows at full sky ambient.

### Cave verdict (2026-09-02, later, from a real cave, not the stamped room)

The room frames passed and the caves were wrong anyway: daylight-grey walls with soft dark
splotches, and warm glowing patches once the sun came up, all fully underground. Three
mechanisms, each fixed in the same commit and each visible in the `openness_in` A/B arms
(`build/openness_in_{nogi,nearest}.png` during the session; GI off changed nothing, nearest-
block openness showed the truth under the filter):

1. **"Unblocked within reach" is not "sees the sky".** A chamber wider than
   `opennessReach` read as open. Now a ray that clears the reach casts one straight-UP coarse
   ray from where it stopped; blocked there means under something, and the ray does not
   count. Overhangs and canopies still shade (the sideways endpoints clear them); a hill does
   not leak.
2. **255 meant "no measurement" to the writer and "fully open" to the reader.** Every wall
   face with a blocker in the block in front of it (a protruding voxel, a stalactite) lit at
   full daylight, and every wall edge next to an empty-air block smeared daylight along the
   wall through the bilinear filter. Now the byte's range is 0..254 (`OPEN_MAX`), 255 is a
   sentinel `opennessByteAt` returns -1 for, and an unmeasured tap drops out of the filter
   with weight 0 so the face inherits its measured neighbours. The terrace-riser argument
   for "no opinion" still holds: a riser between two measured hillside faces takes their
   value instead of full sky.
3. **The shadow lift went through rock.** The distance-only penumbra gave any face whose
   blocker is past `shadowSoftFar` 45% direct sun, including a cave floor under 10 m of
   roof, and P1 then bounced it onto the walls. `shadowLiftCap` (common.wgsl) scales the
   LIFTED part of a shadow by the face's openness at every site that turns a ray distance
   into light: the fragment shader's read, the resolve pass's deposit (which now binds the
   openness bytes) and the walk's sun sample. The published cache value stays the pure ray
   answer, so `shadow-cache` still compares like with like.

Hash unmoved (all render data); `openness`, `gi-bounce`, `shadow-cache`, `determinism` pass.

**Second round, same day** (a dug tunnel alternating pitch black and full daylight in hard-
edged slabs; meadow step faces black at night; daylight in sealed near-surface caves):

4. **The writer measures from a real exposed cell.** The "blocker in the neighbouring block =
   no opinion" early-out left every face of a rough tunnel unmeasured, and the fallback was
   full daylight next to measured zeros. `openValueAt` now walks four columns of the face
   inward from just outside the block, starts its rays at the first air-over-blocker cell it
   finds, and marches FINE for the first two blocks before going coarse. Only a face with no
   exposed column in those four is unmeasured.
5. **A vertical face does not ask the ground.** One of its four diagonals pointed 45° down
   into the ground it stood on, every time; re-aimed to 63° up. Floors and ceilings keep the
   symmetric fan.
6. **`render.opennessFloor` (0.3).** A measured 0 no longer multiplies the ambient to black.
   The lift cap still reads the raw openness, so direct sun still cannot enter a cave.
7. **Out of steps is not daylight.** With block termination off a shadow ray that exhausted
   384 fine steps underground reported "lit". Exhaustion now takes the far blocker's lift
   (capped by openness at the reader); `shadowCoarseDist` is back at 16 m so exhaustion is
   rare outdoors.

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

### P1 status — DONE 2026-09-02, branch `lin-followups` (DESIGN.md §9.y is the binding summary)

What was built, and where it departs from the sketch above:

| Sketch | Built | Why |
|---|---|---|
| `atomicAdd` sum + weight word | ONE RGB9E5 word per block-face, blended (EMA: 1/16 per resolve deposit, 1/2 per walk sample), `irrDeposit` in `common.wgsl` | 256 patches per block-face per frame overflow any bounded count in a handful of frames; a per-frame reset needs a frame stamp the word has no room for. The blend converges within a frame where deposits are dense and is bounded by construction. 48 MiB dense (`kIrradianceBytes`, derived). |
| `irradiance *= 1-k` on the walk | the walk deposits ONE coarse sun sample per face it marches (`openSunSample`: a `traceOpaque` block ray from the face centre, albedo from the first blocker voxel in the face's centre column or the 2x2 quincunx around it), zeroes faces of blocks with no surface, and fades every face it CANNOT measure -- could not march, or no blocker in any of the five columns -- by `giDecay` per visit (the centre-only probe left the latter frozen at daylight: gate `gi-nightfall`) | a blend toward a fresh sample IS the sun-awareness; a plain decay would leave an off-screen face dark until something looked at it. The walk reads `RenderParams` (binding 10 of the sim group, already there for `sim_pick`) for the key light. |
| 3 directions, `cos θ / d²`, openness-gated | 9 coarse `traceOpaque` rays (normal + 4 tangent diagonals + 4 corners) from a point pushed clear of the receiver's block, solid-angle weights (0.25 / 0.09375), no distance term, no openness gate | stored values are RADIANCE (no falloff, no emitter cosine); an unlit face reads 0 and gates itself. Three rays is asymmetric on every axis-aligned face, five leaves a wall's floor to one ray. |

**Two lessons `--gate gi-bounce` taught**, both now in the code and its comments: (1) the
gather origin must LEAVE the receiver's own 4³ block along the normal — a wall's block column
reaches three voxels in front of its face, and a ray starting on the face reported the wall as
its own emitter on every direction leaning toward the column; (2) the emitter face must come
from the ray's DIRECTION, not from the block-entry axis — a 45° ray enters a floor block through
its side as often as its top, and a floor block's side face holds no surface. The first
version (five rays, cosine × 1/d, entry face) lit a wall beside a sunlit floor at 6% of the
floor's radiance and then at exactly 0; the true form factor is 0.5, the nine-ray quadrature
lands at 0.28, and `giStrength` carries the rest. The gate also had to put its floor IN THE
FRAME (the resolve pass deposits only for requested patches) and write the sun BEFORE the
ticks that exercise the walk.

**Numbers (RTX 3060 Ti, 1080p, 2026-09-02, one `--render-budget` boot, `SANDVOX_RUN_EXCLUSIVE=1`, no stray process):**

| | noon | dusk |
|---|---|---|
| baseline | 11.48 ms | 13.67 ms |
| `nogi` (gather + both injection paths const-folded) | 10.28 ms | — |
| P1 cost | **1.20 ms (10.5%)** | — |
| `noopenness` (same boot) | 11.47 ms | — |
| `halfres` | 3.31 ms | 3.93 ms |

`--shader-stats`, `raymarch` FS: 128 registers before and after (the cap), spill 64 → 80
B/thread, binary 1,969 → 1,981 KB. `shadowResolve` 53 → 58 registers, the openness kernels
40 → 56. `--gate gi-bounce` PASS (wall delta R +2.26, G +5.80, B +1.01 /255; floor word
(0.085, 0.189, 0.062)); `openness`, `sleep`, `shadow-cache` PASS; `determinism` 01dc3219
UNMOVED; `--vk-smoke-loud --vk-validation` 19/19, clean. The `shadow-cache` gate pins
`giStrength = 0` for its arms — its reference arm has no resolve pass and therefore no
injection, and the bounce (1.87 mean |dL|, 45k pixels still converging between warm frames 3
and 4) was otherwise the whole disagreement.

**Open, deliberately:** the flicker budget of the EMA at shadow edges (α × the lit/unlit
spread per face) is not measured by any gate; the walk's coarse sample and the resolve pass's
fine one disagree by construction on a half-shadowed face and the resolve wins within the
frame only for faces on screen. The verdict below is what the eye says about both.

**Verdict (2026-09-02, `--shot` GI-off vs GI-on pairs, `build/shots_cmp/*_x2.png`).** The
`--shot` harness had to change first: one frame per camera showed no bounce at all (the
resolve pass deposits a frame after it is asked), and the settle ticks ran before any sun was
written (so the walk deposited nothing) — it renders four frames per camera now and writes the
sun before the ticks. At `giStrength = 1.0` the effect was there and too soft to matter: the
ruin's walls beside the meadow in `ground` did not change to the eye (mean delta 0.19/255).
At **2.0, the shipped default**, the room's outer wall in `openness_out` picks up a warm green
from the meadow along its lower half and its shaded face reads warmer rather than flat grey;
in `openness_floor` the wall beside the doorway shows a soft green patch thrown by the sunlit
floor wedge — the first thing in the engine that looks like light arriving from somewhere;
the ruin walls in `ground` tint faintly toward the grass. Nothing wrongly brightens: open
meadow, water, the far cascade and both lava frames are bit-identical between arms (P1 is a
near-field term and lava is emissive, not sun-lit). The criticisms: the bounce patch on the
room wall has a visible straight edge where the 4³ emitter faces quantise — a bilinear over
the emitter faces would soften it, at 4× the reads; and the room interior is still mostly
dark, because a 7-voxel doorway lights a small floor wedge and one bounce of a small patch is
a small thing — that is P2's write-back. Per-frame deltas (mean |Δ| per channel /255,
pixels moved > 2): `openness_out` (1.56, 3.36, 0.77) 29.5%; `openness_floor` (1.94, 2.69,
1.30) 25.4%; `openness_in` (0.71, 1.00, 0.41) 7.7%; `ground` (0.24, 0.37, 0.11) 4.0%;
`tallgrass_eye` (0.40, 0.86, 0.13) 9.4%; `pond`/`water`/`far`/`lava*` ≤ 0.16, ≤ 1.2%.

## 4. P2 — write-back (multi-bounce)

The gathered result at a receiver feeds the receiver's own block-face at low weight
(`render.giFeedback`, ~0.2), so bounces accumulate over frames — Lin's "unlimited bounces".
Requires the decay from P1 to be the same clock, or the grid brightens without bound:
`feedback < decay` must hold and be asserted in `LoadTuning`. Verify with the P1 gate run for
600 frames: the wall's term converges (last 100 frames within 5%), never diverges.

### P2 status — DONE 2026-09-02, `lin-followups`

In `raymarch.wgsl` after the gather: the pixel's OUTGOING radiance (`albedo × sun + bounce`)
is blended into its own block-face word at `render.giFeedback` (0.2 shipped) — the third
buffer a fragment shader writes, `irradiance` bound `read_write` at render binding 19. Micro
hits skip it (their cell is the ground below). `LoadTuning` keeps `giFeedback < giDecay`
(warning + clamp, not a crash — a bad value is one keystroke away in the tuner) and, the bound
that actually matters for divergence, clamps `giStrength ≤ 3` while feedback is on: each
bounce is albedo × the gather's 0.28 × giStrength of the last, and the series converges only
below 1. `--gate gi-bounce` grew the convergence arm: with feedback on it renders 500 frames,
reads the wall's +X word, renders 100 more and reads again; luminance 0.0301 → 0.0301, within
the 5% `giBounce.convergePct` allows. With feedback on the wall's bounce delta at
`giStrength 2` is R +7.79, G +19.60, B +3.46 /255 (G-led by 2.5×).

## 5. P3 — emissives

Lava and fire inject from the `sim_occupancy` dirty walk (`mainDirty`, indirect on the
compacted dirty list): for each block containing `emission > 0` cells, add
`color0 × emission × count` to its six faces. `heatSpill` becomes a special case of the
gather and is **deleted** (with its `TUNE_HEAT_SPILL_*` rows and the occupancy probe gate at
`raymarch.wgsl:6839`). Verify: `--shot` of the authored lava pool at night — the rim rock and
the far wall are lit, and the `heatSpill` arm in `--render-budget` no longer exists.

### P3 status — DONE 2026-09-02, `lin-followups` (built differently from the sketch)

Not a third writer in `sim_occupancy`: emission is part of the ONE sample both injection
paths already deposit (`irrSample` in `common.wgsl`: `albedo × (sun × Lambert × lit +
emission × TUNE_EMISSIVE_STRENGTH)`). The shadow resolve pass deposits it for every visible
lava/ember patch each frame; the openness walk deposits it for every marched face on every
DIRTY chunk every tick (and a lava pool is dirty every tick) and on the rolling refresh for
the rest — so a third pass would have re-deposited the same term into the same word. Faces
turned from the sun now still run the walk's sample (they carry emission). `heatSpill` is
DELETED: the function, its call site and the occupancy probe gate in `fs`, and
`render.heatSpillStrength` in all five tuning places. Verdict frame:
`screenshot_lava_spatter` at `--time 0` (night), GI on vs off — the meadow beside every lava
voxel picks up a warm orange pool of light, mean |Δ| (1.15, 0.47, 0.06) /255 over 7.6% of
the frame; nothing else in the frame moves. **The authored lava pool frames are a stale
fixture**: `screenshot_lava/_down/_close` look at (220, 520, y 64..86) where the ground at
this seed is y ≈ 211 — the cameras are buried, the frames are flat dark facets with GI on or
off, and they have been since the window shrank to 512 (the section pinned the origin at 0,
which put z 496..546 in the far cascade even before the pool moved). The section's origin is
now z 8 chunks so the cameras are at least in-window; whoever re-authors the pool moves them.

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
