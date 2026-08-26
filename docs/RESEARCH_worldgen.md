# RESEARCH: terrain and world generation for a 10 cm falling-sand voxel world

Status: research only. No engine code changed, no hash moved. This is a decision
document for the next worldgen pass, written against `main` @ `2d35fd3`.

Revision 2 (2026-08-26): every number in revision 1 was re-derived from source.
The scale audit survived intact; the *plan built on it* did not. Three hard
blockers sit between this document and its own stage 1, and none of them were in
revision 1 — see **§4**. They are all cheap to fix and all of them move the world
hash, which is exactly why they need to be sequenced deliberately rather than
discovered halfway through a terrain rewrite.

Sources: the VoxelGameDev `#procedural-generation` archive (2020-01 → 2025,
133,783 lines, mined for the ~30 messages that carry real technique), the papers
and repos cited in it, and a line-by-line read of `assets/shaders/worldgen.wgsl`,
`src/sim/world.h`, `src/sim/world.cpp`, `src/sim/stream.cpp`,
`src/sim/tuning_params.def`, `assets/materials/tuning.json`,
`assets/materials/reactions.json`, `docs/PLAN_surface_flight_perf.md`,
`docs/ROADMAP_scale.md` and the far-field section of `DESIGN.md`. Full source
list at the bottom.

**Related plans of record.** `DESIGN.md` wins on any conflict.
`docs/ROADMAP_scale.md` is the standing post-Vulkan-port work queue and is
*orthogonal* to this document: it is about making the sim bigger and cheaper,
this is about making the world worth looking at. They touch in exactly one
place — §3.3's page-residency argument.

---

## 0. The short version

1. **The scale audit is the headline, not the algorithms.** This world renders
   to 6.6 km and has 5.4 m of vertical relief. Every landform you asked about —
   mountain ranges, chasms, giant lakes — is *absent* not because the algorithms
   are hard, but because the terrain function's amplitude and wavelength are set
   two to three orders of magnitude below what the renderer can show. §3.
2. **You cannot do the scale pass today: `vnoise` overflows i32 above a noise
   cell of ~2,901 voxels (290 m).** This is a hard arithmetic ceiling on
   `hillWavelength` and `biomeScale`, it sits in two places that must agree
   (WGSL and the C++ mirror), and revision 1's headline recommendations
   (640 m hills, 2 km biomes) are both above it. Fixing it is ~10 lines and one
   rebaseline. **§4.1. Do this first or nothing else in this document is
   reachable.**
3. **Scaling amplitude without scaling wavelength does not make mountains, it
   makes cliffs.** Slope is `A/W`; the existing terrain is already at ~50°
   because of it. The scale pass is a *proportional* move plus added octaves at
   the fine end, not an amplitude knob. §4.2.
4. **The binding constraint is regenerability, not per-cell purity** — and
   almost nothing is actually ruled out. `genCell` being pure is the *cheapest*
   way to be regenerable, not the only way. Erosion, DLA, WFC and worms can all
   be baked; what decides the cost is *where* the bake lives and whether the
   far-field sieve can see it. §2.
5. **The single highest-value technique for you is `a² + b² < ε`** — two 3D
   noise fields, carve where both are near zero. Pure per-cell, one branch,
   produces branching tunnel networks with no state. It is a drop-in replacement
   for `caveAt`'s current two-band slab carve. §6.4.
6. **Your page table already paid for mountains.** The `JITTER(mat)` sentinel
   means a solid stone mountain interior costs *zero pages*. Pre-page-table, a
   300 m massif was unaffordable; today the buried bulk is a sentinel and only
   the skin is resident. §3.3.
7. **On your biome-layout question: do both, in three tiers.** Fixed compass
   semantics (snow north, desert east) come from a *seed-independent* coarse
   field; which mountains and which lakes come from the seed; what's inside any
   given cave comes from the seed. This is the standard resolution, and it costs
   one extra noise term. §8.

---

## 1. Verification log

Revision 1's claims, checked against source. Kept here because the wrong ones
are wrong in instructive ways.

| Claim | Verdict |
|---|---|
| Relief = `baseHeight 32 + hillAmp 42 + detailAmp 12` = 54 vox = 5.4 m | **Confirmed** (`tuning.json`) |
| Biome cell 384 vox = 38.4 m; tree tile 144; pond tile 448, r 68–127, depth 26 | **Confirmed** |
| Cascade: 8 levels, level *k* cell `2^(k+1)` vox, level 8 = 51.2 m / 6.6 km | **Confirmed** (`kFarN=256`, `kFarShiftBase=kFarShiftAlign=1`) |
| Cave band 1 roof `h − 40`; magma table `y = −80` | **Confirmed** (`caveAt`, `LAVA_LEVEL`) |
| The window streams in Y; `ShiftAxis` handles `axis == 1` with no clamp | **Confirmed**; the stale comment at `worldgen.wgsl:221` has since been fixed |
| `genChunk` 20.76 → 5.37 ms; `--autofly-surface` p50 57.1 → 25.3 ms | **Confirmed** (`PLAN_surface_flight_perf.md:108,651`) |
| 91% of leaving-plane slots unmodified; mixed-chunk RLE **expands** to 32 KiB | **Confirmed** (`stream.cpp:310`, `:543`) |
| `kPoolPages = 32768`; pool exhaustion is a fatal abort | **Confirmed** (`world.h:590`) |
| "`sim.*` is integer-only, so terrain must be fixed point" | **Misattributed.** Terrain knobs are the `worldgen` group, which is `TP_I`/`TP_U` — integer by declaration, not by the `sim.*` float exception. The conclusion (integer-only) is right; the reason given is wrong, and it matters because the `sim.fluid*`/`sim.wind*` float escape hatch is *not* available here. |
| "§3.2 needs a `sin`… a small integer LUT" | **Already exists.** `isin()` at `worldgen.wgsl:742`. But its phase resolution is 8 bits, which is *not* enough for the sine-contour trick — §5.2. |
| "`TerrainHeight` is a mirror of `baseHeight`/`genColumn`" | **Half right, and the half that's wrong is load-bearing.** There are **four** height notions, not two. §4.4. |
| "Overhangs: keep the mirror as *topmost solid*, which is already how it behaves" | **False.** `World::TerrainHeight` is a mirror of `baseHeight` *alone* — it does not see the authored pools, the pond bowl carve, or the arena. It is already not "topmost solid". §4.4. |
| "A 1 km lake needs more than 24 rim samples" | **Understated — the guarantee is already stale at current tuning.** §6.5. |
| "`genCell` cost is a real budget" on the far path | **Right, wrong function.** The far sieve's cost is dominated by `farSurfaceMat`, not `genCell`. §7. |
| Effort/risk table in §7 | **Rewritten.** Stage 1 was rated S/M; it is S/M *only after* §4's three prerequisites, which revision 1 did not know about. |

Two stale comments found in passing, unrelated to any recommendation here but
worth someone's five minutes:

- `worldgen.wgsl:539` still opens the tree block with "one voxel is
  VOXEL_METERS = 6.25 cm, so there are 16 voxels to the metre and the player
  capsule is 27 voxels tall… the residency window is 256 voxels tall and does
  not stream in Y". All four numbers are from the pre-`kVoxelMeters = 0.10` era.
  This is the *same* stale block that was fixed at line 221; the second copy was
  missed.
- That one is not only a comment. **`const VOX_PER_M : i32 = 16`** is the live
  metres→voxels factor for every tree and cactus dimension, and the world is
  10 voxels to the metre. Every tree is therefore **1.6× the metre size its own
  table says it is** — the "11.9 m" great oak trunk is 190 voxels = 19 m, the
  "4.2 m" crown radius is 6.7 m. They may well look *better* at 1.6×; the
  problem is that the file documents them as metre-true, so the next person to
  tune "in metres" will silently get 1.6× what they asked for. Decide which it
  is and write it down. (`TREE_MAX_*` are derived from `VOX_PER_M`, so changing
  it moves the hash and needs `check_invariants.py` re-run.)
- `POND_TILE`'s comment says "14 m between pond sites" (it is 44.8 m) and
  `TREE_TILE`'s says "9 m" (it is 14.4 m). Both are pre-third-scale-pass.

---

## 2. The constraint that decides everything

From `worldgen.wgsl:2` and reaffirmed in the magma-table comment at line 1800:

> `genCell()` is a pure function of WORLD coordinates + seed, so a chunk that is
> generated, evicted unmodified, and re-entered regenerates bit-identically.
>
> The hard part is that genCell is a PURE PER-CELL FUNCTION of (world coords,
> seed): no flood fill, no neighbourhood walk, no way to find the rim of a basin.

This is not a style preference. Three separate systems depend on it:

- **Eviction/regen.** `Stream::ShiftAxis` throws away unmodified chunks and
  regenerates them from scratch. Any generation that depends on generation order
  produces a different world on re-entry.
- **The far cascades.** `worldgen.wgsl far` calls `genCell` at the *centre* of a
  2^k region to fill the horizon — a point sample, at arbitrary stride, with no
  neighbours available and no chunk context. Level 8 samples one point per 512
  fine voxels (51.2 m).
- **Bit-determinism** (CLAUDE.md rule 1). Same seed+tick → same hash.

### 2.1 What is actually ruled out (short answer: almost nothing)

The invariant the engine needs is **regenerability**: a chunk generated at any
time, in any order, from any window position, must produce identical words.
Per-cell purity is a *sufficient* condition for that, and the cheapest one. It
is not necessary. Anything is legal if the bake is deterministic from the seed
and available before the first chunk that reads it.

So the real question is never "is it per-cell?" — it is **where does the bake
live**, and there are three answers, all already load-bearing here:

**(A) The tile-hash structure pattern.** Already used for trees (`treeAt`, 5×5
tile scan), ponds (`pondAt`), cacti and ruins. A structure lives in a tile; its
parameters come from one hash of the tile coord; any cell computes which tiles
could reach it and evaluates them all. Cost is bounded by
`ceil(maxReach / tileSize)`² tiles. This is exactly what the archive calls the
canonical solution:

> fundamentally, what allows chunk generation to be one-step in isolation while
> allowing structures to span many chunks, is the fact that two nearby chunks
> will see the same structures (or at least the same structures that can overlap
> both!) and will both generate the common structures in the exact same way

Everything with a *bounded footprint* — a dungeon, a mountain peak, a canyon
segment, a village — can be a tile-hash structure. The bound is the price: the
per-cell loop scales with `(reach / tile)²`, so a 500 m structure on a 25 m tile
is a 400-tile scan and is not viable. Big things need big tiles.

**(B) A CPU-side precomputed coarse map, uploaded as a read-only buffer.**
Nothing in rule 1 forbids `genCell` reading a *seed-derived constant table*. A
2048² R16 heightmap of the whole "home region" is 8 MiB — trivial VRAM, and it
can be produced by a real erosion sim on the CPU at load time (or offline, and
shipped). `genCell` then becomes `bilinear(coarseMap) + local detail`. This is
**terrain amplification**, and it is the actual state of the art:

> Modeling high-resolution terrains is a perennial challenge... we focus on the
> amplification of a low-resolution input terrain into a high-resolution,
> hydrologically consistent terrain featuring complex patterns by a multi-scale
> approach.
> — *Terrain Amplification using Multi-scale Erosion*, hal-04565030

Note it dodges §4.1's overflow entirely: a table lookup has no noise cell.

Cost and caveat: it makes the world **finite** at the coarse layer, or forces
tiling. Both are fine — see §8. Also note the archive's dissent:

> The upscaling mostly retains the validity of the input hydrology... If your
> base terrain is shaped like a muffin pan then you will get a gorgeously
> amplified hydrological muffin pan.

**(C) The MutationQueue.** CLAUDE.md rule 3 says everything flows through it,
and worldgen is one of two exceptions. But the *reverse* is also available:
worldgen can lay the terrain, and a CPU-side pass can then stamp authored `.vox`
prefabs and generated structures through `sim_mutate.wgsl` exactly as brush edits
do. This is the right path for **authored dungeons and landmark buildings** —
the content is data, the placement is deterministic from the seed, and none of it
has to be expressible per-cell. `voxload.cpp` and the prefab pipeline already
exist. The cost is that these edits are no longer re-derivable, so they become
`Stream::modified_` chunks that must be persisted rather than regenerated.

### 2.2 The real gate: can the far-field sieve see it?

`worldgen.wgsl far` fills the horizon by calling `genCell` at the centre of each
cascade cell — a bare point sample, no chunk, no neighbours, no live grid.
`DESIGN.md` requires that the sieve and the live downsample (`fardown`) agree
**exactly**, or there is a visible seam where refilled cascade planes meet
downsampled ones. So:

| Bake target | Sieve sees it? | Cost |
|---|---|---|
| (A) tile-hash | **Yes** — sieve re-evaluates the same hash | Free |
| (B) coarse buffer | **Yes** — sieve samples the same buffer | One binding: a `pass_table.def` row + BGL entry, guarded by `check_pass_table.py` |
| (C) stamped via MutationQueue | **No** | Needs the `farPatch`/`fardown` edit-persistence path — which exists and works (`worldgen.wgsl:3135`), at one index entry per edited cell |
| A CA/smoothing post-pass over generated chunks | **No, and there is no patch path** | The one genuinely awkward case |

That last row: the multi-pass isn't the problem — a halo handles it, and
CLAUDE.md already names mark+apply as a known pattern. The problem is that a
post-pass result is *not an edit*, so `farPatch` can't carry it, and the sieve
would keep reporting the pre-pass material forever. Avoid post-passes; put the
same effect in the density function.

### 2.3 Technique by technique

| Technique | Verdict | Target | Real cost / obstacle |
|---|---|---|---|
| Hydraulic / thermal / stream-power erosion | **Usable** | B | Not "iterative and therefore impossible" — iterative and therefore **finite**. ~20k iterations to converge at 512²; the archive's WGSL implementation runs 4096² at 70 fps/iteration. A world-creation cost, cached to disk. The only thing you lose is an infinite coarse layer. |
| Basin filling / watershed / correct lake levels | **Usable** | B | Same. Second channel in the coarse map. Archive's method is one line: *"add 1 m of sediment to any cell that is a local minimum"*. |
| Perlin worms | **Per-cell-evaluable** | A | A worm's start is `hash3(seed, tileX, tileY, tileZ)`, so it is not order-dependent and you *do* know where it started. A cell asks which tiles are within `maxReach` and re-walks each worm. Exactly what `bartwe` describes Starbound doing: *"just figure out which worms are relevant for that chunk and do those every time."* Cost is `candidateWorms × steps`, hoistable per chunk like `genColumn`. Worse than §6.4's sum-of-squares on cost, not on legality. |
| DLA mountain ranges | **Usable** | B, or A per-region | Sequential aggregation, so it can't run per-cell — but the archive's own resolution is *"you do the second method in groups of like 5x5 chunks-ish, with the voronoi method… just like villages and dungeons"*. Bake per tile, cache, sample. |
| WFC / graph-grammar dungeons | **Usable** | B or C | Constraint propagation on the CPU per tile at stream-in, then stamped. §6.7(iii). |
| Poisson-disc scattering | **Not needed** | A | True dart-throwing needs rejection, but nobody needs true Poisson. A **jittered lattice** is gap-free, blue-noise-ish and per-cell-pure — it's what `treeAt` already does, and what KdotJPG's scattered blender uses (jittered *triangular*, specifically, for less axis alignment). |
| CA smoothing post-pass | **Avoid** | — | See §2.2. Not a compute problem, a cascade-agreement problem. |

**The one thing genuinely ruled out** is unbounded global iteration over an
*infinite* domain. Everything else is a trade between finiteness, VRAM, and
bake time — none of which this engine is short of.

### 2.4 Is per-chunk worldgen the fastest approach? — measured, and yes

**Worldgen is not the bottleneck, and the alternative you'd reach for first was
already measured and rejected as slower.**

From `docs/PLAN_surface_flight_perf.md` (RTX 3060 Ti, quiet machine,
2026-08-24, after Correction 3):

```
genChunk per shift            5.37 ms   (was 20.76 before the genColumn hoist)
--autofly-surface p50         25.3 ms
```

- 5.37 ms covers **1,024 chunks** (a 32×32 shift plane) — ~5.2 µs/chunk,
  ~1.3 ns/voxel.
- A shift fires every `kHysteresis = 2` chunks of travel = 32 voxels = **3.2 m**.
  At 10 m/s that's ~3 shifts/s, so worldgen is ~16 ms of every wall-clock
  second — delivered as a 5.4 ms spike on ~3 frames in 50, not as steady load.
- The plan's own conclusion after that work: *"What is left in the surface frame
  is **render + CA**, not paging."*

**Worldgen already had its 4× win.** It came from hoisting the pure-(x,z) half
out of the per-cell path (`genColumn`), which cut a chunk from 4,096 evaluations
to 256. One named lever remains unpulled: the `treeAt` per-column hoist. In
revision 1 that was listed as a 1–2 ms nicety. **It is now a prerequisite** —
see §4.3.

**The obvious alternative — cache chunks instead of regenerating them — was
measured here and is worse.** `Stream::CompleteOldest` deliberately *refuses* to
persist unmodified chunks:

> Measured under `--autofly-surface`: 363 of the 397 real-page slots on a
> leaving plane are unmodified — 91%. Every one of them was paying a 16 KiB GPU
> copy, a 4,096-word RLE encode and a store insert, and the RLE of a mixed
> surface chunk **EXPANDS to 32 KiB** because worldgen's per-cell palette jitter
> makes nearly every word its own run.

Regenerating is cheaper than reading back what you already know how to compute.

**The third option is the interesting one:** escape hatch (B) is plausibly
*both* cheaper and more expressive. Today `genColumn` evaluates `baseHeight`
(8 hashes) + `biomeAt` (16) + `pondAt` + `shoreAt` — and `shoreAt` can trigger a
`pondSurface`, which is **24 more `baseHeight` samples**, i.e. ~200 hashes for
one shore column. A bilinear fetch from a coarse map is 4 loads.

Two honest caveats:
1. It trades **ALU for memory traffic**, and GPUs have far more of the former.
   Across a 1,024-chunk dispatch the access pattern is coherent (neighbouring
   columns hit neighbouring texels), which is the good case — but it is an
   A/B, not a theorem.
2. It only helps if the coarse map replaces work rather than adding a layer on
   top of it.

The A/B is cheap and needs no file edits: build the coarse-map path behind a
`TUNE_*` switch, then `--sweep` it and read whole-frame p50 from
`run.sh sandvox.exe --frames 400`. Per CLAUDE.md, a headless gate cannot see
this — the selftest measures the sim, and this is a fill-path cost.

**Bottom line:** cost is a function of *what you evaluate*, not of the per-chunk
architecture. The current forest is expensive (24-sample pond rims, 5×5 tree-tile
scans); mountains and sum-of-squares caves are cheap by comparison.

---

## 3. The scale audit — read this before choosing any algorithm

Numbers from `assets/materials/tuning.json` and `src/sim/world.h`, at
`kVoxelMeters = 0.10`.

### 3.1 What the world currently is

| Feature | Voxels | Metres |
|---|---:|---:|
| Total vertical relief (`baseHeight 32 + hillAmp 42 + detailAmp 12`) | 54 | **5.4 m** |
| Hill wavelength | 64 | 6.4 m |
| Detail wavelength | 16 | 1.6 m |
| Biome cell | 384 | 38.4 m |
| Tree tile spacing | 144 | 14.4 m |
| Pond tile / radius / depth | 448 / 68–127 / 26 | 44.8 m / 6.8–12.7 m / 2.6 m |
| Ruin tile | 256 | 25.6 m |
| Cave band 1 roof | h − 40 | 4 m below surface |
| Magma table | y = −80 | −8 m |
| Residency window | 512³ | **51.2 m cube** |

### 3.2 What the renderer can show

`kFarLevels = 8`, `kFarN = 256`, `kFarShiftBase = 1`. Level *k* cell =
`2^(k+1)` fine voxels; half-extent = `12.8 × 2^(k+1)` m.

| Level | Cell size | Half-extent |
|---:|---:|---:|
| 1 | 0.4 m | 51 m |
| 4 | 3.2 m | 410 m |
| 6 | 12.8 m | 1.6 km |
| 8 | **51.2 m** | **6.6 km** |

`DESIGN.md:2367` labels levels 6–8 "terrain-scale work only". **There is no
terrain at that scale.** The entire vertical relief of the world (5.4 m) is one
tenth of a single level-8 cell — at the horizon the world is *literally one cell
thick*. The biome field (38.4 m) is smaller than one level-8 cell, so the biome
pattern point-samples into white noise there. The far field is currently
rendering a flat plane with 6.5 km of nothing on it, and the cascade machinery —
which is expensive, correct, and beautifully built — is doing no work that a
single skybox couldn't do.

**This is the finding.** Before implementing a single new algorithm, the terrain
function's amplitude and wavelength need to move by ~2 orders of magnitude.

### 3.3 Why that is now affordable (and wasn't before)

The blocker used to be memory: a 300 m mountain is 3,000 voxels of solid stone in
Y, and a dense voxel buffer pays for every one. The **page table** changed this.
From CLAUDE.md:

> Sentinels are `EMPTY`, `UNIFORM(mat)` and **`JITTER(mat)`** (one material +
> worldgen's positional palette variant) — the last is what compresses buried
> bulk, since a stone chunk's per-cell `% 3` variant makes it non-uniform. […]
> The adversarial descent that measured 32,365/32,768 slots resident (98.8%) and
> exhausted the pool now settles at **14,697 (−55%)**.

A mountain's interior is exactly `JITTER(stone)`. It costs a sentinel per chunk
and zero pages. The residency cost of a mountain is its **surface area**, not its
volume. Deep terrain is now nearly free.

The second blocker — the 51.2 m window — is **not real**. `Stream::Update`
computes `d[3]` over all three axes and `ShiftAxis` handles `axis == 1` by moving
`no.y += dir`. There is no Y clamp anywhere; the window follows the player
vertically.

So the real constraints on vertical scale are:
- **51.2 m of world is resident at once.** A 300 m cliff means the player at the
  bottom cannot see the top as sim voxels — they see it as cascade level 3–4
  (3.2–6.4 m cells). That's what the cascades are for, but it means *sim-driven*
  features (a sand avalanche down the whole face) can only run in the 51.2 m band
  the player is in.
- **Streaming cost during vertical travel.** Falling 300 m is 6 full window
  shifts. `gotcha-paged-streaming-fly-cost` applies: measure with
  `--autofly-hard`, not with a standing player.
- **Surface *crenellation*, not depth, is what costs pages.** A scale pass that
  keeps slopes constant (§4.2) adds roughly zero resident slots per unit of
  relief, because the extra volume is all sentinel interior. A scale pass that
  *steepens* slopes adds surface area superlinearly. This is the whole reason
  §4.2 is a prerequisite and not a stylistic note.

---

## 4. The three prerequisites

Nothing in §5–§9 is reachable until these land. All three move the world hash.
All three are small. **Land them as one commit** — they touch the same functions
and rebaselining three times in a row is the waste CLAUDE.md's verification
budget section is written about.

### 4.1 `vnoise` overflows i32 above a noise cell of 2,901 voxels

`worldgen.wgsl:207`, and its C++ mirror at `world.cpp:533`:

```wgsl
let v0 = h00 * (cs - fx) + h10 * fx;      // h* are 0..255, fx/fz are 0..cs-1
let v1 = h01 * (cs - fx) + h11 * fx;
return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
```

`v0 ≤ 255·cs`, so the numerator reaches `255·cs²`. That crosses `2^31 − 1` at:

```
cs² ≤ (2^31 − 1) / 255 = 8,421,504     →     cs ≤ 2,901 voxels = 290.1 m
```

Today's largest noise cell is `biomeScale = 384` (`255·384² = 3.76e7`, 1.7% of
the budget), so nothing is broken right now. But every headline recommendation in
this document is above the ceiling:

| Want | Noise cell | `255·cs²` | |
|---|---:|---:|---|
| today's hills | 64 | 1.0e6 | ok |
| today's biomes | 384 | 3.8e7 | ok |
| **ceiling** | **2,901** | **2.1e9** | **ok, barely** |
| §4.2's hill octave | 6,400 | 1.0e10 | **overflow, 4.9×** |
| §8's 2 km biome cell | 20,000 | 1.0e11 | **overflow, 48×** |

The failure mode is the worst kind: signed overflow is UB in C++ and wraps in
WGSL, so the CPU mirror and the GPU would diverge *silently and
seed-dependently*. `World::TerrainHeight` feeds terrain collision — the symptom
is a player falling through ground they can see, in some places, at some seeds.
`check_invariants.py` does not check this.

**The fix — normalise the interpolation weights to 8 bits before multiplying:**

```wgsl
fn vnoise(x : i32, z : i32, cs : i32, seed : u32) -> i32 {
  let gx = fdiv(x, cs);  let gz = fdiv(z, cs);
  // fractional position within the cell, rescaled to 0..256 ONCE. This is the
  // whole fix: every product below is now bounded by 255*256*256 = 1.67e7
  // regardless of cs, so the noise cell may be as large as the coordinate
  // space allows instead of topping out at 2,901 voxels.
  let fx = (fmodp(x, cs) << 8) / cs;      // 0..255
  let fz = (fmodp(z, cs) << 8) / cs;
  let h00 = ...; let h10 = ...; let h01 = ...; let h11 = ...;
  let v0 = h00 * (256 - fx) + h10 * fx;   // <= 255 * 256 = 65,280
  let v1 = h01 * (256 - fx) + h11 * fx;
  return (v0 * (256 - fz) + v1 * fz) >> 16;
}
```

Notes that decide whether this is a 20-minute change or a two-day one:

- **It moves the world hash** — the rounding is different (a divide by `cs²`
  becomes two 8-bit quantisations). Expect a full `--selftest --rebaseline` plus
  both smoke tables. Nothing else about the world changes shape.
- **It must land in `world.cpp` in the same commit.** Two implementations, one
  arithmetic. Add a row to `check_invariants.py` while you're there — this is
  precisely the "two places must agree" class that script exists for.
- **`cs` must stay a power of two, or the two `/ cs` divides cost.** Today's
  values (64, 16, 384, 40, 48, 32, 12, 128, 130) are mostly not. If you make the
  new large wavelengths powers of two, both divides become shifts and the fix is
  *faster* than what it replaces.
- **This is also where §5.1's quintic interpolant goes.** `fx` is already
  isolated as a 0..255 fraction; `fx' = (3fx² − 2fx³)/2^16` is two multiplies at
  that point and nowhere else. Doing both in one hash move is strictly cheaper
  than doing them in two.

### 4.2 The scale pass is proportional, not an amplitude knob

`worldgen.wgsl:258` already states the invariant, and it is the reason the third
scale pass worked:

> Wavelength and amplitude were both halved […] so slopes are unchanged — the
> hills are the same shape, half the size.

Slope of one octave is `~2A/W`. The current terrain sits at `2·42/64 ≈ 1.3`
(≈52°) for the hill octave and `2·12/16 = 1.5` for the detail octave — **the
world is already at the angle of repose and steeper.** Scaling amplitude alone
by 50× produces a 270 m *cliff face*, not a mountain: unwalkable, an avalanche
generator (rule 2), and a page-residency disaster (§3.3).

The correct move is `A` and `W` together by the same factor `k`, which changes
nothing about how the terrain looks locally and everything about how big it is.
Relief then comes from **adding octaves at the fine end**, not from amplitude.

A concrete ladder, sized to fit under §4.1's ceiling *even before the fix*, and
to keep the octave count (and therefore the hash cost) at 5:

| Octave | Wavelength (vox / m) | Amplitude (vox / m) | Slope `2A/W` |
|---|---:|---:|---:|
| continental | 2,048 / 204.8 | 1,400 / 140 | 1.37 |
| range | 512 / 51.2 | 350 / 35 | 1.37 |
| hill | 128 / 12.8 | 88 / 8.8 | 1.37 |
| detail | 32 / 3.2 | 22 / 2.2 | 1.37 |
| grain | 8 / 0.8 | 6 / 0.6 | 1.50 |

Total relief ≈ 1,866 voxels = **187 m**, slope character identical to today,
noise cost identical to today +3 octaves (≈24 extra hashes per column, against
`genColumn`'s current ~200 on a shore column). Lacunarity 4, persistence 0.25 —
a *steeper* rolloff than standard fBm on purpose, because at lacunarity 4 the
standard `p = 0.5` would make each octave *four times* steeper than the last.

Two things this ladder does not do, deliberately:

- **It is not fBm and should not be.** Summed octaves give you lumps. The
  *shape* comes from §5.1 (derivative attenuation, so ridges stay sharp and
  valleys go flat) and §5.2 (sine contours, so ranges are long and connected).
  This ladder is the substrate those operate on; landing it alone gets you a
  bigger version of today's world, which is already a large improvement but not
  the destination.
- **It does not reach 600 m.** Revision 1's "200–600 m" was written without the
  slope constraint. 600 m of relief at 1.37 slope needs a 6,600-voxel
  continental octave, which needs §4.1's fix and pushes the hash count up.
  187 m is what fits cleanly; treat 600 m as a stretch goal that arrives with
  the coarse map (escape hatch B), where relief is a table value and slope is
  whatever the erosion sim produced.

**`TREELINE` and the pond-depth clamp both key off this band and must move with
it.** `TUNE_TREELINE = 72` sits inside today's y32..y86; at 187 m of relief a
literal 72 puts the treeline underground almost everywhere. Make it a
*fraction of the band* rather than an absolute Y, or the first thing the scale
pass produces is a world with no trees.

### 4.3 The scale pass kills `treeAt`'s sky short-circuit

`worldgen.wgsl:1256`, reject #1 — the fix that took `genChunk` from 20.76 ms to
5.37 ms (`PLAN_surface_flight_perf.md` B2):

```wgsl
if (y > TREE_MAX_TOP) { return MAT_AIR; }
```

with

```wgsl
const TREE_MAX_BASE : i32 = min(TUNE_TREELINE - 1,
    TUNE_BASE_HEIGHT + TUNE_HILL_AMPLITUDE + TUNE_DETAIL_AMPLITUDE);
const TREE_MAX_TOP : i32 = TREE_MAX_BASE + TREE_MAX_ABOVE;   // ~349 today
```

`TREE_MAX_TOP` is a **world-wide constant derived from the terrain band**. Today
it is ~349, so the ~160 chunk-layers of sky above it in a 512-tall window cost
one compare per cell. Widen the band to 187 m and — since `TREELINE` has to move
with it (§4.2) — `TREE_MAX_TOP` becomes ~2,150 and the short-circuit **never
fires inside the window at any altitude**.

What's left is rejects #2 and #3, which are cheap but not free: ~4 surviving
tiles × (1 hash for `treeSite` + 8 hashes for `baseHeight`) ≈ 36 hashes for
every air cell that used to cost one compare. Across a shift plane that is the
B2 regression coming back at roughly a third of its original severity — and it
would show up as a 5.37 → ~12 ms `genChunk` spike, i.e. exactly the kind of
fill-path cost no headless gate can see (`--frames 400` p50 is the instrument).

**The fix is the `treeAt` per-column hoist that §2.4 already names as the last
unpulled lever.** The tile set for a column is column-invariant, so `treeSite`
and its `baseHeight` belong in `Col`, evaluated once per column and shared by
the 16 cells. That turns reject #3 into a per-cell compare against a value
already in a register, and gives you a correct *local* ceiling
(`max(sbase) + TREE_MAX_ABOVE` over the scanned tiles) to put back in reject #1.

Two more sites need the same audit before the band widens:

- **`cactusAt`** (`worldgen.wgsl:1529`) has **no** world-wide Y reject at all —
  it runs a 9-tile `cactusInfo` scan and rejects per-tile on `y < c.base`. It is
  cheaper per tile than `treeAt` and gated by biome at its call site, but it has
  no equivalent of reject #1 to lose, so confirm the call-site gate actually
  short-circuits before paying the scan.
- **`undergrowthSite`** and the flower/cover path are per-column, so they are
  unaffected — but they are also the reason the hoist is worth doing properly
  rather than as a patch: `Col` is already the right home for all of it.

### 4.4 There are four height functions, not two

Revision 1 said "`TerrainHeight` is a mirror and it will desync". True, but the
count is wrong and the correction changes a recommendation.

| # | Function | Where | Includes |
|---|---|---|---|
| 1 | `baseHeight(x,z,seed)` | `worldgen.wgsl:257` | noise only |
| 2 | `genColumn(...).h` | `worldgen.wgsl:1894` | 1 + authored pools + pond bowl carve. **Not** the arena. |
| 3 | `surfHeightAt(x,z,seed)` | `worldgen.wgsl:2749` | 1 + pools + pond carve + **the arena**. **Not** the lab slab. |
| 4 | `World::TerrainHeight` | `world.cpp:552` | 1 + the lab slab. **Nothing else.** |

They are all correct for their own consumer — #2 is what the near field builds,
#3 is what the far sieve colours, #4 is what CPU collision and spawns ask — but
the differences are not documented anywhere and two of them look like bugs:

- **#3 has no `T.labMode` guard**, while #2 does. In `--lab`/`--fluid-bench` the
  far field paints the original hillside instead of the slab. Cosmetic, lab-only,
  but it is a real divergence in the one function `DESIGN.md` singles out as
  needing exact agreement.
- **#4 sees none of the carving.** Stand a mob at the centre of a disc pond and
  `TerrainHeight` reports the *uncarved* hillside, 26 voxels above the bowl
  floor. This is presumably why `pondInfo` carries a keep-out list for every
  fixture site and the streaming-ball column — the exclusions are papering over
  the mirror, not over the terrain.

**What this changes:** revision 1's §4.3 recommended, for overhangs, "keep the
mirror as *topmost surface*, which is already how it behaves". It is not how it
behaves. Redefining #4 as "topmost solid" is still the right call — it is
well-defined, it is what every current consumer wants, and it matches the
`KindAt`/`Unknown` contract — but it is a **change**, not a restatement, and it
would incidentally fix the pond case and let `pondInfo` drop half its keep-out
list. Budget it as such, and write the four-way contract into `DESIGN.md` when
it lands.

---

## 5. The two per-cell-pure "fake erosion" techniques

Both produce eroded-looking terrain from a closed-form function of `(x, z)`,
with no simulation. Either is implementable in `baseHeight` in an afternoon —
*after* §4.

### 5.1 Derivative-attenuated fBm ("the gradient trick") — iq's `morenoise`

Standard fBm sums octaves at fixed amplitude. This variant divides each octave's
contribution by one plus the squared magnitude of the *accumulated gradient*:

```
d = 0                       // accumulated 2D derivative
a = 0                       // accumulated height
for each octave:
    (n, dn) = noiseWithDerivative(p * f)
    d += dn * f
    a += amp * n / (1.0 + dot(d, d))
    f *= 2; amp *= 0.5
```

Effect: **steep places stop accumulating detail; flat places keep accumulating
it.** You get flat valley floors, sharp ridgelines, and a strong "weathered"
read — for the price of one extra vec2 in the octave loop.

> he does mention breaking up the world to do the DLA approach, but I feel like
> that'd just create a bunch of separate mountains
> — vitulus\_\_ / shiv2k3, 2024-04-02, on why the gradient trick is the one that
> works chunk-by-chunk

**Fit for this engine: excellent, and it is now cheaper than revision 1
thought.** `vnoise` is bilinear value noise, so analytic derivatives are trivial
and the integer arithmetic survives (you accumulate a fixed-point gradient).
iq's caveat applies: with bilinear (`u(x) = x`) the derivative is piecewise
constant and the attenuation shows grid creases, so you need at least the cubic
`3x² − 2x³`.

The reason it got cheaper: **§4.1's rewrite already isolates `fx` as a 0..255
fraction**, which is exactly the value a smoothstep operates on and exactly the
value a derivative is taken with respect to. Doing the interpolant upgrade
inside the overflow fix costs a few lines; doing it later costs a second hash
move across the whole world *and* the CPU mirror. **Merge it into §4.1.**

### 5.2 Sine-of-contours "fake tectonics" — mountain ranges from one noise

From `extra_witchy` (2024-11-27), who was simultaneously running a real
stream-power erosion sim at 4k×4k/70 fps and reaching for this as the *infinite*
alternative. Verbatim:

> these are just the contours near 0.0 of 6-octave opensimplex FBM noise with
> persistence of 0.48
>
> sine of a large multiple of the noise to turn isolines into ridges, attenuated
> by distance from reference isoline, with secondary noise for weight of the
> sample

```rust
fn tectonic_noise(xy, ridges, highlands, peaks, attenuation,
                  ridge_count, ridge_scale, plateau_scale) -> f64 {
  let r_sample  = ridges(xy);
  let r_sin     = sin(r_sample * PI * ridge_count);
  let r_height  = r_sin * r_sin * ridge_scale * attenuation(r_sample);
  let m_height  = r_height * peaks(xy);          // break ridges into peaks
  let h_sample  = max(0.0, highlands(xy));       // where mountains exist at all
  h_sample * (plateau_scale + m_height)
}
```

With their suggested attenuation `0.1 * ridge_sample / (base_sample + 0.1)`.

Why this is the right shape for **mountain ranges** specifically: real ranges are
long, curved, connected, and *parallel* — a property fBm never produces and
ridged noise only fakes locally. Taking the level sets of a smooth field gives
you genuinely long connected curves for free, and `sin²` turns each level set
into a ridge. The `highlands` term is what gives you "mountains here, plains
there" instead of uniform corrugation, and the archive's note on **correlating**
those terms is what makes it read as geology:

> I'm using the mountainousness parameter both as a multiplier to ridge height,
> and an additive factor to the base height, so that high ridges also mean high
> valleys, and it can form plateaus where the base height is large but the ridges
> are small

**Fit for this engine: very good, with one arithmetic gotcha revision 1 missed.**

You do not need to write a sine LUT — `isin()` already exists at
`worldgen.wgsl:742`. But you cannot use it as-is here:

```wgsl
fn isin(a : i32) -> i32 {
  let p = a & 255;                     // 0..255 == 0..2pi   <-- 8-bit phase
  let half = p & 127;
  let v = (4 * half * (128 - half) * 256) / (128 * 128);   // parabola, +-256
  return select(v, -v, p >= 128);
}
```

Two problems, both from the 8-bit phase:

1. **The argument here is `r_sample · π · ridge_count`.** `vnoise` returns
   0..255, and `ridge_count` is ~6–10, so the phase is `~8 × r`, i.e. the 8-bit
   input is multiplied up and then masked back to 8 bits — you keep only the low
   5 bits of the noise value. Ridge flanks come out in 32 visible terraces.
2. **`isin` is a parabola, not a sine** (max error ~5.6% of amplitude). Squared
   (`sin²`) that error lands directly on the ridge profile, and since the ridge
   *is* the silhouette, it is visible.

The fix is small and belongs with §4.1: widen `vnoise`'s output and `isin`'s
phase to 16 bits. `isin16(a : i32) -> i32` with `p = a & 65535` and the same
parabola scaled up is ~6 lines and costs one extra multiply. Once §4.1 has
already moved the hash, this rides along free.

With that, every remaining term is a `vnoise` call plus arithmetic. Composes
cleanly with §5.1 as the base field. **This is the recommendation for mountain
ranges.**

### 5.3 The sediment trick — free stratigraphy and valley fill

Also `extra_witchy`, and directly relevant to a falling-sand game where "what
material is here" matters as much as "is there material here":

> you imagine setting the terrain to its maximum height, then you apply your
> contour function *subtractively* and add some fraction of the difference as
> sediment
>
> let's say your terrain goes from 0–1000 m, and you make 10% of the erosion into
> sediment, and you erase the top 30 m of sediment. Where your bedrock is at
> 700 m or above, the contours are bare bedrock. Where the bedrock is 500 m,
> 10% of that is 50 m minus 30 m leaves 20 m sediment, for a total of 520 m.
>
> this method works for infinite terrain, and creates deeply sedimented lowlands
> with bare highlands

One subtraction and one clamp, per column. It gives you, for free: bare rock on
peaks, dirt/sand in valleys, deeper soil the lower you go, and a *material
boundary* that follows terrain rather than a constant depth. For this engine that
is the difference between "grass skin over stone" and a world where digging into a
valley floor gives you 8 m of loose sand that actually flows, while digging into a
ridge gives you rock.

**Fit: trivial.** It is arithmetic on `Col.h` in `genColumn`. Highest
value-per-line of anything in this document, and — unlike everything else in §5 —
**it does not depend on §4.** It works at today's 5.4 m band (it just gives you
0.8 m of sand in the valleys instead of 8 m), so it can land first, alone, as a
cheap independent win while §4 is being built.

---

## 6. Landform by landform

### 6.1 Mountains and mountain ranges

**Recommendation:** §5.2 sine-contour ridges as the range layer, over §5.1
derivative-attenuated fBm on §4.2's octave ladder, with §5.3 sediment on top.

Target scale: ranges 2–8 km long (wavelength 20,000–80,000 voxels — **note this
is 7–28× over §4.1's pre-fix ceiling; it is only reachable after the overflow
fix**), peaks 150–200 m on the analytic path or 600 m on the coarse-map path
(§4.2). That puts a range at level-7/8 cascade scale, which is what those levels
were built for.

**Alternatives considered and rejected:**
- *DLA* (sp4cerat / voxels.blogspot, and the Josh's Channel video the archive
  cites): produces the best-looking dendritic ridge networks of anything here,
  but the aggregation is sequential, so it has to be baked: (B) precompute a
  DLA map for the home region, or (A) bake per ~5×5-chunk tile and cache, which
  is what the archive suggests ("just like villages and dungeons"). Deferred on
  cost, not legality.
- *Voronoi ridge graph* (archive, `extra_witchy`): "each cell centre is linked
  to its highest neighbour… areas near a link are higher". Per-cell-evaluable
  if you bound the neighbour search, and it produces genuinely connected ridge
  chains. More expensive than §5.2 for a similar result; keep it in reserve as a
  *modulator* to break §5.2's over-uniform ridge lengths, which the archive
  explicitly flags as the weakness of both this and real erosion:
  > Many erosion functions tend to create homogeneous ridges that stay
  > unrealistically consistent along their length
- *Real uplift + stream-power erosion* (hal-01262376, hal-04049125): the actual
  SOTA, and someone in the archive has it running at 4096² / 70 fps in WGSL under
  Bevy — but it is a converging simulation (~20k iterations at 512²) and can only
  reach you through escape hatch (B).

### 6.2 Valleys, chasms and canyons

Canyons are the one landform that is *easier* in a density field than a
heightmap, because a canyon is defined by its walls, not its floor.

- **Layer-hardness banding.** From the archive's geological-history writeup:
  > a preliminary surface is produced by applying an erosion function, using
  > large-amplitude noise, which is affected by the hardness of the layers (in
  > this example, white rock is soft, while green, red and yellow are harder,
  > creating terraced mesas and canyons)

  Per-cell: define `hardness(y)` as a function of absolute Y (a few noise bands
  or a literal table), and make the erosion amplitude at a column proportional
  to `1/hardness`. You get mesas and stepped canyon walls with one extra term.
  In a falling-sand game the payoff is doubled: hardness bands can be *actual
  different materials* with different CA behaviour. It also composes with §5.3 —
  hardness bands *are* stratigraphy.
- **Chasms** are the level set of a second, much lower-frequency ridged field,
  used subtractively rather than additively — i.e. §5.2 with the sign flipped and
  a hard `min()` against the base. The archive's warning applies:
  > My main problem is that it can lead to poor gameplay when it carves through
  > tall terrain like a hill or mountain (as it produces steep slopes where the
  > river is carved into the ground)

  Fix: attenuate the carve by the local base height, so chasms live in lowlands
  and become gorges only where they cross a ridge.

### 6.3 Overhangs

The clean statement of the technique is `k_jpg`'s:

> For overhangs with 3D noise, come up with a terrain formula for each biome, and
> set it up like a heightmap. But, instead of making it 2D noise, make it 3D
> noise, so the heightmap changes over the vertical direction too. For
> efficiency, figure out the min and max height range of your formula, because
> outside of that you don't need to calculate any noise to know whether it's
> solid/air.

Concretely: `solid(x,y,z) = (h2d(x,z) + warp3d(x,y,z)·amp) > y`, evaluated only
for `y ∈ [hmin − amp, hmax + amp]`. Above and below that band, one comparison
answers it. **That Y-band reject is the same structural device as `treeAt`'s
reject #1 — and it degrades the same way when the terrain band widens (§4.3).
Make it per-column (`col.h ± amp`) from the start, not a world-wide constant.**

The architectural cost is `World::TerrainHeight`, which is a CPU mirror used by
spawn placement, mob ground probes, selftest fixtures, `--shot`, and 12 call
sites in `main.cpp`. A heightmap CPU mirror **cannot represent an overhang** —
`TerrainHeight(x,z)` stops being a well-defined question.

Options, in order of preference:
1. **Redefine the mirror as "the topmost solid Y"** — still well-defined, still
   correct for every current consumer (spawns, fixtures, mob probes all want the
   top). Overhangs then exist in the world but are invisible to the CPU mirror,
   which is how the mirror already treats caves and ruin interiors. Per §4.4 this
   is a *change* (the mirror is currently `baseHeight`, not "topmost solid"), and
   it incidentally fixes the pond-carve divergence.
2. Restrict overhangs to a bounded warp amplitude and make the mirror sample the
   3D function down from `hmax`. Correct, but the mirror gets 10–40× more
   expensive and it is called in tick paths.

Take option 1. Write the four-way contract from §4.4 into `DESIGN.md` when it
lands.

### 6.4 Caves and cave systems — the best single technique in the archive

`k_jpg` (KdotJPG, author of OpenSimplex2), stated twice, two years apart:

> take two gradient noise evaluations a,b with different seeds (ideally not
> unmitigated Perlin), then check `a² + b² < 0.02 : carve : ignore`
>
> It makes tunnels because it carves out round ish areas around where both noises
> are zero. One noise zero means winding surfaces. Two intersects those to make
> paths
>
> Skip the second eval if the first already exceeds
>
> In a rudimentary test a long time ago, it seemed faster than MC caves

That is the whole algorithm. Two 3D noise evaluations, one early-out, and you get
a connected, branching, non-grid-aligned tunnel network — the thing everyone else
reaches for stateful Perlin worms to get. It is **pure per-cell**, which makes it
the only serious cave technique in the literature that fits `genCell` unmodified.

Note the engine's current `vnoise` is **2D**. `caveAt` fakes 3D by carving
vertical column *bands* whose floor and ceiling are 2D noise — which is why the
existing comment can promise "this cannot create free-floating stone blobs".
A sum-of-squares carver needs a genuine 3D `vnoise3`, and that reintroduces the
floater question: **the island detector will convert generated floaters into
debris the moment anything moves nearby.** Two mitigations, both cheap: bias the
threshold so carved volume stays well under 50% (floaters need surrounded air),
and keep the ε small enough that tunnels are tubes rather than caverns. Verify
with a settle-count gate, not by eye.

Extension, from the same author:

> The sum-squares technique can be extended further with derivative checking to
> close off areas where it goes funky, introducing otherwise-missing dead-ends
> — [`DerivativeTunnelClosingCaveCarver.java`](https://github.com/KdotJPG/Cave-Tech-Demo/blob/master/src/main/java/jpg/k/cavetechdemo/carver/DerivativeTunnelClosingCaveCarver.java)

Dead-ends matter for a game: a network with no dead ends has no exploration
payoff.

**A full cave stack** — three independent layers, mirroring what MC 1.18
converged on (the archive discusses "noodle caves" vs "cheese caves" vs "cave
carvers" at length):

| Layer | Technique | Reads as |
|---|---|---|
| Tunnels | `a² + b² < ε` on two 3D fields, ~4–20 m tunnel width | Winding passages, the connective tissue |
| Chambers | Single low-frequency 3D field thresholded hard (`n > 0.75`) | Rare big rooms — "cheese caves" |
| Ravines | `a² + b² < ε` with strongly anisotropic scaling in Y | Tall narrow slots |
| Karst | See below | Geologically coherent limestone systems |

Set the tunnel ε and chamber threshold as `TUNE_*` params so they're
`--sweep`-able without a rebuild.

**Karst**, if you want the good version later: *Synthesizing Geologically
Coherent Cave Networks* (Paris et al., CGF 2021, [hal-03331697](https://hal.science/hal-03331697/document),
code at [aparis69/Karst-Synthesis](https://github.com/aparis69/Karst-Synthesis))
generates a skeleton by anisotropic shortest path on a 3D graph respecting
inception horizons, then realises geometry as an **SDF construction tree** of
blended primitives. The skeleton is not per-cell (escape hatch A or B), but the
*second half is* — an SDF tree evaluated per cell is exactly `genCell`. Bake
skeletons per 512 m tile via tile hash, evaluate the SDF per cell. The inception
horizons are §6.2's hardness bands under another name.

**The trap this engine specifically will hit:** whatever you carve, the fill rule
must produce matter **at rest**. The magma-table comment (`worldgen.wgsl:1786`)
is the hard-won statement of this and it generalises to every new feature:

> matter that takes 90 seconds to stop moving is a rule-2 bug in the AUTHORING,
> not in the CA
>
> a flat cut does not NEED to find the basin, because the cave's own complement
> already is one. […] the only lava/air interface in the world is the single
> plane `y == LAVA_LEVEL`.

Any new cave system that floods must route its fill through **one** function that
depends on **depth alone**, exactly as `caveFill` does. A per-chamber water level
is a per-cell-uncomputable basin, and every attempt to fake it produces a
permanent world-wide flow.

### 6.5 Giant lakes, and the water-level problem

This is the hardest item on your list, harder than caves, and the existing code
already documents why:

> A disc pond cannot leak by construction: its water surface is set 2 below the
> LOWEST terrain sample on its own rim […] The previous design filled basin-noise
> contours up to a noise water table, and wherever the basin MASK edge crossed
> ground that sat below the local table, the pond poured onto dry lower land and
> crept downhill — the sleep gate caught exactly that (82 chunks around one pond
> still awake after 600 settle ticks; the CPU-mirror scan found 175 such spill
> edges in that region alone).

**And that guarantee is arithmetically stale at today's tuning.** The comment at
`worldgen.wgsl:333` justifies 24 rim samples like this:

> 24 samples on the largest (r=36) pond puts one every ~9 voxels of arc — dense
> enough that the 16-voxel fine terrain octave cannot hide a below-water notch
> between two samples, which is what the -2 margin then absorbs.

`pondRadiusMin = 68`, `pondRadiusSpan = 60`, so the largest pond is **r = 127**,
not 36. Twenty-four samples on that circumference is one every **33 voxels** —
two full lattice cells of the 16-voxel detail octave between adjacent samples,
against a `-2` margin and a detail amplitude of 12. The stated invariant no
longer holds; whether it *bites* at the pinned seed is a separate question (the
settle gate would have caught a large spill), but the reasoning that made the
feature safe is no longer valid reasoning.

Two things follow. First, this is worth a targeted check now —
`--gate` the settle count around a large pond, or scan for `baseHeight < surf`
on the rim annulus of the largest disc in the CPU mirror. Second, it is the
*empirical* form of the argument revision 1 made hypothetically about 1 km
lakes: **rim sampling does not scale, and it has already stopped scaling.**

Three real options for big water, in increasing order of ambition:

**(a) Sea level as a global constant — the magma-table trick, applied to water.**
Pick `SEA_LEVEL`. Every cell with `y ≤ SEA_LEVEL` that is not solid is water.
Containment is then a property of the terrain, exactly as the magma table's
containment is a property of the carve, and the only water/air interface in the
world is one plane. Guaranteed at rest, guaranteed non-leaking, zero settle cost,
and **it is the only option that scales to arbitrary size**. It gives you oceans,
seas, fjords, flooded canyons and coastlines the moment terrain relief crosses
it — which is to say, the moment §4.2 lands, and not before: at 5.4 m of relief a
sea plane is either "everything is ocean" or "nothing is".

The cost is the honest one the magma comment already names: *one level
everywhere*. No mountain tarns, no perched lakes. Accept it for the big water and
keep the disc-pond mechanism for small perched ponds — the two compose, since a
disc pond above sea level is unaffected and a disc pond below it is subsumed.

This is my recommendation and it is not a compromise; it is what Minecraft does,
and for the same per-cell reason.

**⚠ It does not compose with `caveFill` as written, and the failure is a
rule-2 catastrophe.** Extend the sea into carved cells the way the magma table
does and you get, at `y == LAVA_LEVEL`, a water cell directly above a lava cell —
**everywhere in the world that a cave crosses that plane at once.**
`assets/materials/reactions.json` has both directions live:

```json
{ "self": "water", "neighbor": "tag:hot", "chance": 180, "selfBecomes": "steam" }
{ "self": "lava",  "neighbor": "water",   "chance": 300, "selfBecomes": "stone" }
```

That is a world-sized reaction front lighting up on tick 1 of every new world:
steam production (a gas, which then rises and wakes everything above it) and
stone conversion across the entire cave volume. It is the magma-slab bug again,
one order of magnitude worse, and it would be found by the sleep gate about four
hours into a terrain rewrite.

The fix is one line in `caveFill` — a **solid lid** between the two liquids, so
they can never share a face:

```wgsl
fn caveFill(y : i32) -> i32 {
  if (y <= LAVA_LEVEL)            { return 2; }   // lava, flat, still
  if (y <= LAVA_LEVEL + LAVA_LID) { return 0; }   // stone plug: lava and sea
                                                  // may never touch, at any
                                                  // (x,z), by construction
  if (y <= SEA_LEVEL)             { return 3; }   // flooded cave, flat, still
  return 1;                                       // open cave
}
```

`LAVA_LID` need only exceed the CA's reaction reach (1), but make it several
voxels so a single explosion near the boundary doesn't open a global front
either. The same argument that makes the magma table safe then makes the flooded
band safe: flatness from the constant, containment from the carve, one interface
plane per liquid, and now a guaranteed separation between them.

**(b) Terraced sea levels by region.** `SEA_LEVEL` becomes a function of a *very*
low-frequency field, quantised to a small set of levels, with the terrain
function forced to rise above the higher level along the boundary between two
regions. Gets you a highland lake district. Considerably fiddlier; the forced
boundary is the whole difficulty and it is easy to get a one-voxel notch that
drains a lake for 90 seconds of sim.

**(c) Precomputed lake basins in the coarse map** (escape hatch B). If you go
finite-region, a CPU pass can flood-fill basins properly and store a per-cell
water level in the coarse map. This is the only way to get *correct* arbitrary
lakes. The archive's basin-filling note is the relevant prior art:
> an entirely custom parallel basin-filling algorithm that is practically
> embarrassing in its simplicity (add 1 m of sediment to any cell that is a local
> minimum)

**Rivers.** Worth being blunt: rivers are the hardest problem in procedural
terrain and there is no per-cell-pure solution. The archive is unanimous that
they must come from either erosion (escape hatch B) or a graph:
> For rivers I'd recommend calculating their approximate paths in the form of
> some kind of a graph, then applying a forcing function that erodes the terrain
> based on the distance from the river […] before doing the erosion simulation

A graph *is* per-cell-evaluable if the graph is derived by tile hash (escape hatch
A) — a river as a chain of splines seeded per 512 m tile, each tile's segment
connecting to its downhill neighbour's, with terrain forced to the river's height
within a distance falloff. It works, it's ~200 lines, and it's a separate project
from everything above. Defer it; it wants sea level (a) to exist first so rivers
have somewhere to go. Note also that a river is *flowing* water, which is the one
thing the depth-only rest argument cannot give you — expect it to be a genuine
CA-activity cost, budgeted, not free.

### 6.6 Deserts, snowy regions, biome fields

Two independent questions: **where** biomes are (§8) and **how they blend**.

On blending, the current implementation is a hard `if` chain on one noise value
plus a break-up octave. That's fine at 38 m biome cells; at kilometre biomes it
becomes visible as a hard line across the horizon.

`k_jpg`'s **scattered biome blending** ([article](https://noiseposti.ng/posts/2021-03-13-Fast-Biome-Blending-Without-Squareness.html),
CC0 code at [Scattered-Biome-Blender](https://github.com/KdotJPG/Scattered-Biome-Blender))
is the reference solution and it is designed for exactly this:

- Jittered **triangular** lattice (not square — "less possibility for visible
  axis alignment").
- For a coordinate, find the nearest unjittered vertex, search outward in hex
  layers until enough points are found.
- Weight each point by `max(0, r² − dx² − dy²)²` — "a circular bump like a
  Gaussian filter, but it goes zero smoothly at a finite radius".
- **Normalised sparse convolution**: total weight varies wildly, so compute it
  and multiply by its reciprocal.
- ~200–800 ns/coordinate optimised, vs 21–90 ns for a lerped grid and
  5,000–25,000 ns for full-resolution blending.

Three engine-specific notes:
1. The archive has a *recurring* failure report from people implementing this:
   chunk seams appear where the blending happens. The cause is always the same —
   the blend is computed per chunk with chunk-local coordinates or a chunk-local
   point search. Here, `genColumn` is already per-column in world coordinates, so
   you sidestep it, but the `far` cascade sampler must use the identical code path
   or the horizon will seam against the near field.
2. There's a subtler trap the archive names precisely, and it applies the moment
   biomes have different height functions:
   > when the desert interpolates the heightmap with the test biome you get the
   > spikes. I can fix that really easily by generating the biome noise twice, one
   > with white noise one without, and using the one without to interpolate the
   > height

   i.e. **blend the height with a smooth field, pick the biome with the
   broken-up field.** If you break up the biome boundary and then blend heights
   with the same broken-up value, the height becomes non-monotonic and you get
   spikes at the boundary.
3. The "normalised sparse convolution" reciprocal is a **divide**, and a divide
   by a value that varies per column is the one place this could quietly break
   rule 1 if anyone reaches for `f32` to do it. Keep it integer: the weights are
   bounded, so a fixed-point reciprocal with a `>> 16` is exact and portable.

For snow specifically, you already have the right mechanism (`TREELINE`, snow cap
above it). With real mountains that becomes a *proper* snowline — and per §4.2 it
has to become a fraction of the band rather than the literal `72` it is today.
It should also become latitude-dependent as well as altitude-dependent; the
archive's formulation is exactly right:
> temperature goes from high bias near the equator to low bias near the poles,
> with lower values at higher elevations. Humidity could be high bias near
> equator, low bias in the horse latitudes, relatively higher levels again in the
> temperate regions but generally again going down towards the poles

### 6.7 Complex multi-level dungeons

Nothing in this category is per-cell-pure and you should not try to make it so.
Three viable architectures, and they are genuinely different products:

**(i) Prefab stamping through the MutationQueue (escape hatch C).**
Deterministic placement (tile hash picks a site + a prefab id + a rotation), then
the CPU streams the `.vox` in via `sim_mutate.wgsl` when the chunk enters the
window. You already have `voxload.cpp`, the prefab pipeline, the editor, and
`kCellOpIfAir`. **This is the shortest path to good dungeons** and the only one
where the content is authored rather than emergent.
Costs: stamped chunks diverge from procgen and must be persisted
(`Stream::modified_`); a dungeon that spans more chunks than the window is a
staged stamp; and the far cascades will show a hole until `fardown` runs over
them (which it will — the `farPatch` block at `worldgen.wgsl:3135` handles
exactly this).

**(ii) Per-cell BSP-within-a-tile.** A dungeon occupies one 512-voxel tile.
`hash3(seed, tileX, tileZ)` seeds a deterministic recursive split; a cell asks
"which room/corridor am I in" by walking the split tree analytically (no
allocation, ~8 iterations). Multi-level is free — the split is 3D. Purely
per-cell, regenerable, no persistence cost. Produces *architecturally coherent*
but *aesthetically generic* dungeons. Roughly a weekend.

**(iii) WFC / graph-grammar, baked.** The literature (nested WFC for large-scale
generation, IEEE ToG 2024; CG-WFC hybrid cyclic-graph + WFC for designer-guided
mission structure) is real and good, but every variant is constraint propagation
and none of it fits a GPU per-cell kernel. Bake per-tile on the CPU at stream-in,
cache the result, stamp through (i). Best output, most machinery. Note the
control trick worth remembering: **pre-collapse the modules you care about**
(the boss arena) and let WFC build around them.

Recommendation: **(i) for landmark dungeons, (ii) for filler**, which is also
what the archive says most shipped projects converge on.

---

## 7. What point-sampled cascades do to all of this

`worldgen.wgsl far` takes **one** `genCell` sample at the centre of each level
cell. This is stated as deliberate and correct ("Features thinner than a coarse
cell vanish — correct LOD behavior, not data loss"), and it is — but it has a
design consequence that constrains §5 and §6:

**A feature survives the horizon only if it is large *and* has coherent
large-scale structure.** Specifically:

- §5.2's sine-contour ridges survive beautifully: the ridge lines are level sets
  of a low-frequency field, so a 51.2 m sample still lands on the range.
- §5.1's derivative-attenuated detail vanishes above level ~4 — correct, it's
  detail.
- Caves at any scale vanish (they're below the surface; the sample takes the
  material at the centre point, so a level-8 cell over a cave system reports
  stone). Correct.
- A **1 km sea at a constant level survives**, because it's a half-space test —
  every sample inside it reports water. Another argument for §6.5(a): a
  constant-level sea is the only water formulation that renders correctly at
  every cascade level with no extra work.
- **Biomes only survive if biome cells are ≫ 51.2 m.** At the current 38.4 m the
  horizon biome pattern is aliased white noise. Any biome scheme you adopt should
  put the *dominant* biome scale at ≥ 500 m.

### 7.1 The far-path cost model — corrected

Revision 1 said "`genCell` cost is a real budget". Right conclusion, wrong
function. What a far cell actually costs (`worldgen.wgsl:3123`):

```
per cell:   genCell(fine)                              -- full genColumn + genCellIn
if non-air: farSurfaceMat(mat, fine, shift, seed)
              -> surfHeightAt()                        -- baseHeight + pondAt again
              -> if straddles surface and shift >= 5:
                   treeCanopyAt()                      -- 25 x treeInfo, ~500 hashes
                   genCell(x, h, z)                    -- a SECOND full genColumn
```

So a surface-straddling cell at level ≥5 costs roughly **two full `genCell`s plus
a 25-tile tree scan** — call it ~1,000 hashes against a plain interior cell's
~200. `genColumn` is *not* hoisted on this path: `far` calls `genCell`, which
rebuilds `Col` from scratch for every one of the 4,096 cells in a level chunk,
even though a 16×16 chunk has only 256 distinct columns.

Two consequences for planning:

1. **There is an unclaimed 16× on the far path**, structurally identical to the
   `genColumn` hoist that bought 4× on `genChunk`. `far` should build the 256
   columns for its level chunk once (they are `2^shift`-strided, but still only
   256 of them) and reuse them. Nobody has measured this because the far path has
   never been the bottleneck — but a terrain with real relief puts far more cells
   on the surface-straddling branch, and this is where that cost lands.
2. **The budget is `kFarListCap = 4096` level-chunks per tick** (`world.h:1284`),
   which is 4096 × 4,096 = **16.7 M `genCell` calls per tick** at the ceiling.
   That ceiling is only approached on a teleport or world load, and it *is* a
   budget rather than a cliff, but it means "make `genCell` 3× more expensive" is
   a decision with a visible worst case. Measure it the only way it is visible:
   `run.sh sandvox.exe --frames 400`, p50 and p99 whole-frame ms.

---

## 8. Your actual question: fixed biomes or seeded random?

You wrote:

> im thinking of having the land somewhat defined as in biomes will be
> predictable in that there will be a snowy wasteland to the north, desert to the
> east etc consistently, with a couple predefined buildings or features, but maybe
> the generated stuff within will be random??? or itll just be like minecraft in
> that everything in the seed is random. idk

**Do the first one. It is strictly better for this game, and it costs almost
nothing.**

### 8.1 Why fixed compass semantics wins here

The trade-off table is standard — fixed maps buy authored meaning and lose
replayability; procedural buys scale and loses control — but *this project sits
on the fixed side of it for reasons specific to it*:

1. **A falling-sand sandbox's replay value is in the simulation, not the map.**
   Nobody replays Noita for a different map layout; they replay it for a
   different run through the same *structure*. The map's job is to be a stage
   you learn. Randomising the stage costs you the thing a learnable world buys
   (players saying "go east until the sand starts") and buys you a variety you
   don't need.
2. **You have a save format, a replay log, and determinism gates.** A world with
   stable landmark semantics is dramatically easier to test, to bug-report
   against, and to write selftest fixtures for. You already have a spawn clearing
   hack (`inSpawnClearing`, `onFixturePad`) plus a five-entry keep-out list in
   `pondInfo` that exist *because* random terrain kept eating the test fixtures.
   Fixed regional semantics makes that class of problem go away by design rather
   than by exclusion box — and §4.4's mirror fix removes another slice of it.
3. **Wiki-ability.** `assets/tuner.html` already has a Wiki tab. A world with
   named regions is documentable; a world of seeded noise is not.

### 8.2 The mechanism — three tiers, one extra term

The whole thing is: **make the coarse field a function of absolute position that
does not take the seed, and let the seed enter only below it.**

```
Tier A — WORLD MAP (seed-independent)
  climate(x,z) = f(x, z)          // NO seed input
  → temperature falls with +Z (north), aridity rises with +X (east)
  → continent shape, ocean placement, the 4-8 named super-regions
  Scale: 2-20 km. Authored curve + one fixed-seed noise for raggedness.
  Every seed has a snowy north and a desert east. Always.

Tier B — REGIONAL (seeded)
  Which mountain ranges, where the big lakes are, where the ruins are,
  which sub-biome fills a given valley within its climate envelope.
  Scale: 200 m - 2 km. hash3(seed, ...) throughout.
  Two seeds share a geography and share nothing else.

Tier C — LOCAL (seeded)
  Trees, ponds, caves, boulders, dungeon interiors, ore.
  Scale: < 200 m. What you have today.
```

Implementation is one function and one discipline rule: **Tier A calls `hash3`
with a literal constant where the seed would go.** `biomeAt(x, z, seed)` becomes
`climateAt(x, z)` (no seed) combined with a seeded sub-biome pick. The world hash
still moves per seed, because Tiers B and C still consume it.

**Tier A's 2–20 km scale is above §4.1's overflow ceiling by 7–70×.** It is the
single largest noise cell anywhere in the design, so it is the strongest reason
to do §4.1 as a prerequisite rather than as a fix-when-it-breaks.

Two refinements worth building in from the start:

- **Make the compass anchor a `TUNE_*` param**, e.g. `worldgen.climateScale` and
  `worldgen.climateRotation`. Then "how far is the desert" is a slider, provable
  with `--sweep`, and you can tune the whole world's geography without a
  rebuild.
- **Predefined features get absolute coordinates**, exactly as the existing
  origin-area set pieces do (the authored pools at (420,420), (260,300),
  (220,520) and the arena at (180,110) live at their absolute coordinates and
  appear when those chunks generate). That mechanism already works and already
  survives streaming; landmark buildings are the same pattern at a bigger scale,
  plus escape hatch C for the geometry. Note that each one currently costs a
  keep-out entry in `pondInfo` and a branch in `surfHeightAt` — at Tier-A scale
  that hand-maintenance does not survive, so the *first* landmark added under
  this scheme should introduce a proper table rather than a fourth copy of the
  pattern.

### 8.3 The one thing to decide early: finite or infinite

Tier A being seed-independent doesn't force finiteness — a global noise field is
infinite. But if you ever want **real erosion, real rivers, or correct lake
basins** (escape hatch B), you need a finite coarse map. The natural shape is:

> a finite, authored **home region** of, say, 32 × 32 km, backed by a
> precomputed coarse heightmap, surrounded by infinite procedural wilderness
> generated by the same per-cell functions with the coarse map's boundary values
> extrapolated.

That gets you both. It is also what the archive keeps circling back to:
> You can get more complex generation if you are able to generate the whole world
> in 1 go. So if you have a finite world, or make the regions defined planets
> instead of just "infinite", you can do things like really nice biome borders,
> erosion simulation, tectonic plates, foliage growth […]

I would not build the coarse map yet. But I would **write Tier A as a function
call with a single implementation point**, so that swapping "analytic climate
noise" for "bilinear lookup into a coarse map buffer" later is a one-function
change and not a rewrite.

---

## 9. Suggested order of work

Each stage is independently shippable, moves the world hash, and is gated by
`--selftest --rebaseline` + fresh smoke tables. Nothing here is a rewrite of
`worldgen.wgsl`; the tile-hash structure machinery, the `genColumn` split, the
shore band, the flora and the ruin POIs all survive.

**Stage 0 is new and it is not optional.** Revision 1's stage 1 was unbuildable
as written.

| # | Work | Effort | Risk | Payoff |
|---|---|---|---|---|
| **0a** | **Sediment trick** (§5.3). Arithmetic on `Col.h`. Independent of everything below — land it first for a cheap win while 0b is built. | XS | L | Bare rock on peaks, deep loose material in valleys. Directly feeds the falling-sand sim. |
| **0b** | **THE ARITHMETIC PASS** (§4.1 + §5.1 + §5.2's `isin16`). Normalise `vnoise`'s interpolation to 8 bits (lifts the 2,901-voxel ceiling), upgrade the interpolant to cubic/quintic, widen `isin` to 16-bit phase. Mirror all of it in `world.cpp`. Add a `check_invariants.py` row. **One commit, one rebaseline.** | S | M — one hash move, but it is the *whole world* moving; verify the CPU mirror with a fixture walk, not just the hash | Unblocks every other stage. Nothing looks different. |
| **0c** | **`treeAt` per-column hoist** (§4.3). `treeSite` + its `baseHeight` into `Col`; reject #1 becomes a local ceiling. | S | L — pure refactor, hash must NOT move (that is the gate) | Prevents the B2 perf regression that stage 1 would otherwise reintroduce; ~1–2 ms on shift frames today. |
| **0d** | **`TerrainHeight` → "topmost solid"** (§4.4). Fixes the pond-carve divergence; document the four-way contract in `DESIGN.md`; drop the `pondInfo` keep-outs it makes redundant. Add the `labMode` guard to `surfHeightAt`. | S | M — 12 call sites in `main.cpp` | Removes a whole class of fixture-vs-terrain bug, and is a prerequisite for overhangs. |
| 1 | **Scale pass** (§4.2). Proportional A/W move onto the 5-octave ladder; `TREELINE` becomes a fraction of the band; pond-depth clamp re-derived. Verify residency with `--autofly-hard` and frame cost with `--frames 400`. | M | M — stresses the page pool and the settle budget; interiors should be `JITTER(stone)` and cost nothing, but measure | Enormous. Makes the entire cascade system do work. |
| 2 | **Sea level constant** (§6.5a) **with the `LAVA_LID`** — do not ship one without the other. | S | M — must be *provably* at rest; reuse the `caveFill` argument verbatim and re-read §6.5's warning before writing the fill | Oceans, coastlines, flooded canyons. |
| 3 | **Sum-of-squares caves** (§6.4). Needs a 3D `vnoise`. Add derivative dead-ends. Watch the island detector (floaters). | M | M — floater risk is real; gate on settle count | Actual cave systems. |
| 4 | **Sine-contour mountain ranges** (§5.2), now that `isin16` exists. | M | M | Ranges, not lumps. Levels 6–8 finally show something. |
| 5 | **Tier A/B/C climate split** (§8.2) + scattered biome blending (§6.6). | M | M — the far sampler must share the code path or the horizon seams | The world becomes learnable. |
| 6 | **`far` per-column hoist** (§7.1). The unclaimed 16× on the cascade fill path. Do it when stage 1 makes it hurt, not before. | S | L | Pays for stages 1–5's added `genCell` cost. |
| 7 | **Overhangs** (§6.3), on 0d's redefined mirror. Per-column Y band, not a world constant. | M | M | Cliffs, arches, undercuts. |
| 8 | **Dungeons** — (ii) per-cell BSP for filler, (i) prefab stamping for landmarks. | L | M | Content. |
| 9 | Rivers, karst skeletons, coarse-map amplification, DLA. | XL | H | Later. |

Stage 1 still probably changes the game's character more than 2–8 combined. It
just isn't stage 1 any more.

**One measurement to take before any of this**, because it is nearly free and it
sizes stage 1's risk: run `--autofly-hard` on stock HEAD and record peak resident
slots. §3.3's argument says a proportional scale pass adds ~0 to that number. If
it does, stage 1 is M-risk. If it doesn't, the ladder in §4.2 needs its slope
lowered before anything else happens.

---

## 10. Traps this engine specifically will hit

Collected from the existing code comments — which have already paid for most of
these once — plus §4's findings.

1. **Generated matter must be generated at rest.** (`worldgen.wgsl:1786`.) Every
   new liquid, powder or loose feature must reach a CA fixpoint in ~1 tick or it
   is a rule-2 bug in the authoring. The proven technique is *depth-only fill
   rules* — a constant level makes flatness free and lets the carve geometry
   supply the containment. Applies to sea level, to any new magma, and to any
   sand dune you might be tempted to place on a slope.
2. **Two liquids at rest can still be a rule-2 catastrophe if they touch.**
   §6.5. Depth-only fills give you flatness and containment; they do **not** give
   you separation. Any two fill bands whose materials react need a solid lid
   between them, by construction, in the same function.
3. **Integer overflow is the silent one.** §4.1. `vnoise` wraps at a 2,901-voxel
   noise cell; C++ signed overflow is UB and WGSL's is defined, so CPU and GPU
   diverge *differently*. Every new large-scale noise term must be range-checked
   against `2^31` at authoring time. Nothing in the toolchain catches it.
4. **There are four height functions and none of them documents the other
   three.** §4.4. `baseHeight`, `genColumn.h`, `surfHeightAt`,
   `World::TerrainHeight`. Any new surface rule must be placed in the right one
   (or in a shared function both WGSL sites call), and
   `scripts/check_invariants.py` is the place to assert it.
5. **The cascade sieve and the near field must agree exactly.** `farSurfaceMat`
   is shared verbatim by `far` and `fardown` for this reason. Any new surface
   rule (a snowline, a sediment skin, a biome blend) must go through one function
   that both call, or there will be a visible seam at the residency boundary.
6. **Y-band rejects derived from world-wide constants die when the band widens.**
   §4.3. `treeAt`'s reject #1 is the live example and it is worth 15 ms of
   `genChunk`; §6.3's overhang band would be the next one. Make them per-column
   from the start.
7. **A new tuning knob is 5 edits and one of them is silent.**
   `tuning_params.def` → `tuning.h` → `LoadTuning` → `tuning.json` →
   `tuner_schema.js`. Prove it reaches the kernel with
   `--sweep worldgen.yourKnob=0,100`.
8. **The `worldgen` group is integer-only by declaration** (`TP_I`/`TP_U` in
   `tuning_params.def`). The `sim.fluid*`/`sim.wind*` float escape hatch is a
   *different* group and is not available here. §5.1's derivatives and §5.2's
   sine both need integer formulations — fixed-point accumulate and `isin16`
   respectively.
9. **Bigger structures mean bigger tile scans.** `treeAt` scans 5×5 tiles because
   a great oak reaches ~67 voxels past its trunk tile. A structure whose reach is
   large relative to its tile turns the per-cell loop quadratic. Size tiles to
   reach, always.
10. **No headless gate can see a render-occupancy or fill-path cliff.** From the
    wind-perf post-mortem: the selftest measures the sim. A terrain change that
    triples cascade fill cost shows up in `run.sh sandvox.exe --frames 400` p50
    frame ms and nowhere else. §7.1 is the specific exposure.
11. **The page pool is a fatal abort, not a degradation.** `kPoolPages = 32768`.
    A scale pass that makes the surface more crenellated increases resident slots
    even though the bulk is a sentinel. Measure with `--autofly-hard`, which is
    the only scenario that reports honestly (a standing player under-reports 2×).
12. **Comments in this file go stale silently and they are load-bearing.** §1
    lists four found in one read, one of which (`VOX_PER_M = 16` against
    `kVoxelMeters = 0.10`) is not a comment at all but a live 1.6× factor. When
    a scale constant moves, grep for every comment that quotes a metre value.

---

## 11. Sources

**Community archive** — `VoxelGameDev.com / Programming / procedural-generation
[663794240022773760]`, mined lines cited inline. Principal contributors of
technique: `k_jpg` (KdotJPG — OpenSimplex2, Scattered-Biome-Blender,
Cave-Tech-Demo), `extra_witchy` (real-time stream-power erosion + hyper-
amplification in WGSL/Bevy; the fake-tectonics and sediment tricks),
`vitulus__`/`shiv2k3` (gradient-trick discussion), `bartwe` (Starbound cave
worms), and a long multi-participant thread on cross-chunk structure placement.

**Articles**
- Inigo Quilez, [*Better fBm / noise derivatives*](https://iquilezles.org/articles/morenoise/) — the gradient trick; note the quintic-interpolant caveat.
- KdotJPG, [*Fast Biome Blending Without Squareness*](https://noiseposti.ng/posts/2021-03-13-Fast-Biome-Blending-Without-Squareness.html) + [Scattered-Biome-Blender](https://github.com/KdotJPG/Scattered-Biome-Blender) (CC0).
- KdotJPG, [*The Perlin Problem: Moving Past Square Noise*](https://noiseposti.ng/posts/2022-01-16-The-Perlin-Problem-Moving-Past-Square-Noise.html).
- Amit Patel, [*Polygon Map Generation*](http://www-cs-students.stanford.edu/~amitp/game-programming/polygon-map-generation/).
- sp4cerat, [*Terrain Heightmap Generation using DLA*](http://voxels.blogspot.com/2014/01/procedural-terrain-heightmap-generation.html) + [code](https://github.com/sp4cerat/Terrain-HeightMap-Generator).

**Code**
- [KdotJPG/Cave-Tech-Demo](https://github.com/KdotJPG/Cave-Tech-Demo) — sum-of-squares carver + derivative tunnel closing.
- [aparis69/Karst-Synthesis](https://github.com/aparis69/Karst-Synthesis) — CGF 2021 karst networks.
- [eric-guerin/terrain-amplification](https://github.com/eric-guerin/terrain-amplification) — sparse terrain representation.
- [Arches-Team/Real-Time-Hyper-Amplification-of-Planets](https://github.com/Arches-Team/Real-Time-Hyper-Amplification-of-Planets).
- [Auburn/FastNoise2](https://github.com/Auburn/FastNoise2).

**Papers**
- Cordonnier et al., [*Large Scale Terrain Generation from Tectonic Uplift and Fluvial Erosion*](https://inria.hal.science/hal-01262376v1) — the reference uplift+erosion pipeline.
- [*Terrain Amplification using Multi-scale Erosion*](https://hal.science/hal-04565030/).
- [hal-04049125](https://hal.science/hal-04049125v1/), [hal-04525371](https://hal.science/hal-04525371/), [hal-04574826](https://hal.science/hal-04574826/) — stream-power erosion, sediment deposition, directional-artifact elimination.
- Paris et al., [*Synthesizing Geologically Coherent Cave Networks*](https://hal.science/hal-03331697/document), CGF 40(3):277–287, 2021.
- Dey et al., [*Procedural feature generation for volumetric terrains using voxel grammars*](https://eprints.bournemouth.ac.uk/31174/1/1-s2.0-S1875952117301349-main.pdf) — rule-based caves/overhangs.
- Nie, Zheng, Zhuang & Togelius, *Nested Wave Function Collapse Enables Large-Scale Content Generation*, IEEE ToG 16(4), 2024.
- [*A Hybrid Cyclic-Graph & WFC Method for Designer-Guided Generation*](https://blog.ptidej.net/content/files/2025/11/_ICSE_GAS_Laurent____Graph_WFC_Procedural_Gen-1_compressed.pdf) (CG-WFC).
- [*InfiniteDiffusion*](https://arxiv.org/pdf/2512.08309) — training-free diffusion with noise-like seamless/constant-time-random-access properties. Heightmap-only; flagged as the direction the field is moving, not as something to build on now.

**Video**
- Henrik Kniberg, [*Minecraft's new terrain generation*](https://www.youtube.com/watch?v=ob3VwY4JyzE) — continentalness/erosion/peaks-and-valleys + splines.
- Josh's Channel, [*Better Mountain Generators That Aren't Perlin Noise or Erosion*](https://www.youtube.com/watch?v=gsJHzBTPG0Y) — the gradient trick and DLA, side by side.

**Engine sources read for this revision** — `assets/shaders/worldgen.wgsl`,
`src/sim/world.h`, `src/sim/world.cpp`, `src/sim/stream.cpp`,
`src/sim/farfield.h`, `src/sim/tuning_params.def`,
`assets/materials/tuning.json`, `assets/materials/reactions.json`,
`scripts/check_invariants.py`, `docs/PLAN_surface_flight_perf.md`,
`docs/ROADMAP_scale.md`, `DESIGN.md`.
