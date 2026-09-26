# PLAN: powder mass in eighths (sub-voxel grains)

Status (2026-09-26): **P0-P3 LANDED on branch `powder-mass`; P4 PARTIAL; P5
DEFERRED** (owner: "p5 can wait until i inspect behavior").

- P0 encoding, P1 CA (merge / sink / diagonal merge / reaction mass /
  scoop + pour eighths), P2 render (tracePowder in the deferred-detail pass,
  isRayBlockerW in traceOpaque / AO / occupancy, openness signature, grain
  speckle, grains take the cell's TOP shadow patch), P3 sources (brush grain
  slider "powder grain (eighths)", burning bodies drop ash in eighths,
  vessels) -- all in. Raymarch FS still 128 registers / 112 B local memory.
- P4 done: films under 3/8 are walked through (KindOfWord) and are not Jolt
  ground (debris MC occupancy). NOT done: player/mob SLAB collision for
  partials of 3/8..7/8 (they collide as whole cells, so feet can hover up to
  7/8 of a voxel over one), and density-weighted marching cubes.
- NOT done from §3.4: partial powder under liquid still shows an air slot.
- NOT done from §1: material remap across a liquid<->powder CLASS change does
  not normalise the nibble (MatRemap has no class table; a liquid remapped to
  a powder keeps fullness 0..7 = full/full/full/1..5 eighths).
- Gate `powder-mass` (arms A rain / B sink / C dusting + screenshot_powder.bmp).

Goal: a powder cell carries a MASS of 1..8 eighths, so a single 1/8-voxel grain
of sand can exist, fall, pile, merge and be scooped. A partial cell is DRAWN as
a 2x2x2 sub-voxel arrangement that the renderer DERIVES from the cell's mass
and its neighbours (grains stack downward and lean toward the higher side), so
surfaces made of partials read as ramps and fine grain rather than cubes.

This is option B from the 2026-09-26 investigation. Option A (render-only
chamfering of full cells) was rejected by the owner because it cannot express
real sub-voxel matter.

Non-goals: no new voxel bits, no new sim bindings, no new dispatches, no change
to the full-cell CA for cells that are full.

---

## 1. The encoding: full keeps its old values

The word is full (CLAUDE.md), so mass has to live in the state nibble (bits
12-15). Today a powder's nibble is a 3-way palette variant, 0..2.

**Encoding (powder class only, MATF_WANDER excluded):**

| nibble | meaning |
|---|---|
| 0, 1, 2 | FULL (8/8), palette variant 0/1/2: exactly today's meaning |
| 3 .. 9 | PARTIAL, mass = nibble - 2 (1..7 eighths) |
| 10 .. 15 | reserved; read as FULL, and refused by a debug assert |

```wgsl
fn powderMass(w : u32) -> u32 { let s = voxState(w); return select(8u, s - 2u, s >= 3u && s <= 9u); }
fn powderCode(mass : u32, cell : vec3i) -> u32   // mass 8 -> positional jitter (JitterStateFor formula)
```

This choice is what makes the feature affordable. Every path that CREATES
powder today writes 0..2, and under this encoding they all keep meaning "full":

- worldgen (`worldgen.wgsl:4327`);
- the page-table UNIFORM/JITTER sentinels (`synthWordAt`, `world.h JitterStateFor`),
  the RLE fusion in `stream.cpp`, and the chunkstore codec's predictor;
- brush (`sim_mutate.wgsl:148`), `productState`, gas decay, debris rubble,
  prefabs, mob-burn ash, and the edit layers;
- every fixture that writes `state 0` (repose, wind, vessel, lab).

So:

- **Saves stay valid.** No format bump.
- **Sentinels do not change.** Buried bulk powder is still JITTER.
- **Classify and the codec are untouched.**
- **Phase 1 lands with the world hash BYTE-IDENTICAL**, because no partial
  cell exists until something makes one. That is the cheapest possible
  proof that the plumbing changed nothing.

When a merge fills a cell back to 8, it writes the **positional** jitter
(`JitterStateFor(cell)`), not a hash of the tick. That keeps a refilled chunk
eligible to demote back to a JITTER sentinel.

**Known writers of a nibble above 2 into a powder, which must be fixed in P0:**

- vessel pour and spill: payload `(spend-1)<<12` = 7 for powder
  (`container.cpp:344-386, 663-690`). Under the new encoding 7 means 5/8.
- material remap across a class change: the nibble is carried verbatim
  (`mattable.cpp:58`). A liquid→powder remap must normalise it.

All other writers were audited and write 0..2. See §9 for the audit list.

C++ mirror: `PowderMassOf(word)` / `PowderCode(mass, cell)` in `world.h` next
to `JitterStateFor`. `check_invariants.py` gets a row pinning the WGSL and C++
copies together.

---

## 2. CA rules (`sim_step.wgsl`)

Concurrency does not change. The CA writes directly under the 3x3x3 colour
lattice, and an acting cell owns every cell within reach 1 for that pass. So a
two-cell mass transfer is a read-modify-write of two words in one thread, which
is exactly how `transferLiquid` (`sim_step.wgsl:894`) conserves liquid mass.
No claims, atomics or two-phase passes are needed.

**Termination invariant for every new rule:** potential energy
`E = Σ over grains of height` must strictly decrease. Mass only ever moves to a
lower column top. No rule is neutral, so none needs a film-style licence, and
none can hold a chunk awake at rest. Powder mass never equalises laterally, so
the liquid levelling limit cannot arise.

### 2.1 Straight fall (stage 1): add merge and sink

The order is: `tryMove` → **`tryPowderMerge`** → **`tryPowderSink`** → `tryCrush`.

- **Merge.** The target below is the SAME powder and partial (mass `nf < 8`).
  Transfer `t = min(f, 8 - nf)`. The source becomes air or a partial of
  `f - t`; the target becomes `nf + t` (positional jitter if it reaches 8).
  Mark MOVE on both cells, plus `DIRTY_M_POWDER`. This is `tryDescend`'s merge
  with the powder encoding.
- **Sink.** The target below is a DIFFERENT powder and is partial with
  `nf < f`. Do a whole-word swap (`tryMove`'s body, bypassing the
  `canDisplace` refusal). The energy change is `nf - f < 0`.

  Without this, full sand resting on a 1/8 dust film floats 7/8 of a voxel
  above it, which shows as a visible slot of air. With it, the heavier cell
  settles and the film rises to the top, which is also what real fine dust does.

The cost is one comparison on a word `tryMove` has already read. It only runs
when the straight fall was refused.

### 2.2 Diagonals (stage 2): merge onto a partial

`reposeDiagAllowed` gates exactly as today. When the diagonal target is the
same powder and partial, transfer as in 2.1 instead of refusing. That is
reach 1, the same footprint as a diagonal `tryMove`.

### 2.3 Sub-voxel repose (stage 2c). Phase P5, behind `sim.powderFineRepose`

This rule creates ramps from whole-cell piles, and it is the only one that
turns full cells into partials by itself.

It is the repose rule re-expressed on **column-top heights in eighths**. For a
surface cell `c` (air above) with mass `m`:

- its top is `H = 8y + m`;
- a lateral neighbour column's top at the same or lower level is read at
  reach 1:
  - lateral cell partial `mL`: `8y + mL`;
  - lateral cell air and the cell below it partial `mB`: `8(y-1) + mB`;
  - the cell below it full: `8y`;
  - the cell below it air: less than or equal to `8(y-1)`, confirmed through
    `reposeSnap`.

The rule: transfer `k = min(m, ceil((Δ - T) / 2))` eighths into the neighbour
column's top cell when `Δ = H - H2 > T`.

`T` is the repose threshold in eighths per cell, `round(8 * tan(repose))`:

| repose | tier | T |
|---|---|---|
| 18° | 3:1 | 3 |
| 27° | 2:1 | 4 |
| 45° | 1:1 | 8 |
| 63° | 1:2 | 16 |
| 72° | 1:3 | 24 |

For whole cells Δ is a multiple of 8. At 45° (T = 8) the rule moves at
Δ = 16 and holds at Δ = 8, which is exactly today's 1:1 behaviour. It is the
same equilibrium, now with eighths in between. The first shed from a full
cell is k = 4, so a pile dumped from whole cells develops half-cell steps,
which the renderer draws as ramps.

Termination: the moved grains land at most at `H2 + k ≤ H - k`, so E strictly
decreases.

**Cost risk, which is why this is its own phase:** settling a pile takes about
3x more surface moves, because a cell leaves in pieces (4, 2, 1…). Measure
ticks-to-sleep and peak awake chunks on the `repose` fixture and in
`--perf forestfire`/`--autofly-hard` before turning it on by default.

**Worldgen must be a fixed point of this rule.** A surface cell born with a
fractional top (§6) must satisfy `Δ ≤ T` against its neighbours, or the whole
map wakes at load (the 2026-09-13 sand-at-34° lesson). This rule must NOT use
the positional tier blend, because that makes T differ between neighbours. Use
one T per material, i.e. the material's rounded repose, and have worldgen
quantise to the same T.

### 2.4 What stays a FULL blocker in v1 (by decision, not by accident)

A partial powder is a wall to all of these:

- `canDisplace` for non-powder movers: water and gas cannot enter the empty
  7/8;
- the `reposeSnap` bit;
- `liquidWall`, `soloSolid`, `seamSupport`, MPM `FSOLID_HARD`;
- `flagSupportLoss`, and particle `canOccupy` / `blocksParticle`.

Each one is a place a later phase may relax. None is needed for correctness,
and relaxing them one at a time keeps every step bisectable.

**The visible consequence: water over a partial powder** leaves an air slot
between the water and the grains. P2's render fix for this is in §3.4.

### 2.5 Reactions: mass is carried

`reactWriteSelf` (`sim_step.wgsl:1122`) and the PAIR self-write:

- The source is a partial powder, or a liquid with `f < 8`, and the product is
  a POWDER or LIQUID: the product gets the **same mass**. For example, 3/8
  snow melts to 3/8 water. This also fixes today's bug where 1/8 water
  transmutes to a full cell.
- The source is partial and the product is SOLID: the rule fires only when the
  source is full, so a 1/8 seed never becomes a whole sprout.
- The source is partial and the product is GAS: the rule fires, but the gas
  cell is written only with probability mass/8 and is otherwise air. (§10 Q2.)
- The neighbour product in PAIR is unchanged: it is a fresh full cell, as today.

This moves the hash only where partials exist.

### 2.6 Wander (mite)

Excluded from the encoding: mites are creatures, always full, never merge.
Test for `MATF_WANDER` before any `powderMass` use.

---

## 3. Rendering

The hard constraint is that the raymarch fragment shader sits at **128
registers**, the documented cliff; 168 costs about 3.5 ms
(`raymarch.wgsl:483-490, 2729-2753`). Nothing may be added inside the
`trace()` loop that stays live across it. Partial powder therefore uses the
existing **record-then-resolve** detail machinery (`DETAIL_SLOTS`, phase 2 at
`raymarch.wgsl:3461-3602`), the same path that already carries micro bricks
and analytic plants with the march's registers dead.

### 3.1 Primary ray

- **In the loop, one branch** beside the `MATF_MICRO` record (`raymarch:3181`):
  `klass == CLASS_POWDER && voxState(w) >= 3` means record the cell as a detail
  of kind POWDER and continue marching. Full powder (state 0..2) takes the
  existing full-cube branch, unchanged. That covers every buried and bulk
  cell, so the cost is paid only on surface partials.
- **Budget.** Powder shares `detMax`. Past the budget, a partial with
  `mass ≥ 4` is a full-cube hit, as today's micro fallback does, and one with
  `mass < 4` passes through. The ray then hits the cell below, which on any
  settled surface is solid. Instrument with `RENDER_STATS` how often a
  grazing dune ray exhausts the budget, before deciding whether powder gets
  its own slots.
- **LOD.** Past `TUNE_POWDER_LOD_DIST` (a new `render.*` knob, about 30 m)
  there is no brick: an analytic slab `[y, y + m/8)` is intersected in phase 2,
  which is one ray-box test. A 1/8 dust film stays a film at distance instead
  of popping to a cube.
- **Phase 2: `tracePowder(cell, mass, ro, rd)`.**
  1. Read the 4 lateral neighbours and the cell above (registers are free
     here). Neighbour height `hN` is: partial gives its mass, full / solid /
     out-of-window gives 8, air gives 0 (optionally minus the cell below, so a
     drop reads as negative).
  2. **Arrangement**, a pure integer function of `(mass, hN[4])`:
     - If `mass ≤ 4`, fill `mass` bottom-layer sub-cells; otherwise fill all 4
       bottom cells plus `mass - 4` top cells.
     - Within a layer, rank the 4 sub-cells by the sum of the two neighbour
       heights they touch; highest first, so grains pile against walls and
       lean uphill.
     - Ties break in a fixed order, not a hash. A falling 1/8 grain must stay
       in the same corner from cell to cell, not flicker.
     - Implement it as bit arithmetic on a 4-bit rank, **not** as an indexed
       `const` array. Dynamic indexing spills (`common.wgsl:1501-1512`).
  3. March the 2³ brick with the `traceMicro` DDA pattern (at most 3·2+4 = 10
     steps), or better, test up to 8 AABBs analytically. A miss lets the ray
     continue, like a micro miss.
  4. On a hit: the sub-face normal, the material, and a sub-grain colour from
     `paletteJitter(m, hash(subcellWorldCoord))`. The colour is keyed on
     world sub-cell coordinates, so a settled pile is stable and each grain
     reads as its own speck, which is the "fine sand" look.
- **Do not reuse `micMat`.** That would inherit every micro-only behaviour:
  AO forced to 1, GI and openness read from the cell below, GI write-back
  skipped, and brick yaw on the normal. Use a separate `detKind == POWDER`
  that keeps normal terrain AO, GI and shadow-cache keying on the enclosing
  cell's face.

### 3.2 Shadow, AO, occupancy, openness: one word-level blocker predicate

Today `isRayBlocker` looks at material only, so a 1/8 dust film on stone would
cast a full-voxel shadow and darken AO like a wall. Add:

```wgsl
fn isRayBlockerW(m : Material, w : u32) -> bool   // powder: blocker iff powderMass(w) >= 5
```

and use it at the three places that hold the word:

- `traceOpaque`'s per-cell test (`common.wgsl:5842`);
- `aoSolidAt` (`raymarch.wgsl:5452`);
- **`sim_occupancy`'s blocker count** (`sim_occupancy.wgsl:130-148`).

The occupancy change is required, not optional. The `MATF_MICRO` rule (DESIGN
§ static micro-detail) says the traced blocker and the occupancy blocker must
agree, or chunk-skipping shadow rays terminate on the wrong thing. Here the
disagreement would be in the SAFE direction (occupancy counts too much, so a
chunk simply is not skipped), but it is cheap to make exact.

The sentinel branch of occupancy is unaffected, since sentinels are always
full. The coarse 4³ block path past `coarseFromT` stays conservative.

**Openness and far-downsample skip signatures** hash material only
(`sim_openness.wgsl:532-539`, `worldgen.wgsl:5568-5575`). Fold `powderMass`
into the openness signature for powder cells, or a mass-only edit never
re-walks. The far downsample keeps material only: a 2-voxel far cell cannot
see eighths.

### 3.3 Everything else that calls `paletteColor(m, state)`

The callers are the secondary hit, the GI deposit, `sim_openness`, `sim_glow`,
debris particles and rigid-body cubes. `paletteJitter(state % 3)` already gives
a valid variant for codes 3..9. It correlates with mass, but these are
secondary and low-resolution paths. No change is needed.

### 3.4 Partial powder under liquid (P2b)

When the cell above a partial powder is a liquid, the renderer treats the
empty sub-cells as that liquid. The media path continues the water column
down into the partial cell to the brick hit, so the visible air slot
disappears. This is render-only. The CA fix, letting water occupy the empty
7/8, is deliberately not planned: it would mean two materials in one cell.

### 3.5 Verification (one `--verify` launch)

- `--shader-stats`: the raymarch FS must stay at 128 registers or fewer, with
  no new local memory.
- A `--render-budget` arm on a new `powder` budget camera (a dusted slope plus
  a pile at a grazing angle), with the feature on and off.
- `--shot` frames: the dusted slope, a pile, and 1/8 grains mid-fall.

---

## 4. CPU side and physics

- **`KindOfWord`** (`world.h:4227`) maps powder with mass ≤ 2 to `Air` for
  walking, so a film is walked through. Mass ≥ 3 stays `Solid`. That is one
  line, and every CPU consumer (player, mobs, spells, AI support) inherits it.
- **Player slab collision (P4).** `Collides` / `SweepAxis` / `GroundProbe`
  (`player.cpp:53-122, 1161`) learn that a partial powder cell's box is
  `[y, y + m/8)`. `SweepAxis` currently snaps flush to integer faces, so it
  needs the cell's top from the word. That removes the up-to-7/8-voxel float
  above a half cell. The mob `supports` probe gets the same treatment.
- **Jolt terrain** (`debris.cpp:6154-6231`) is marching cubes over a binary
  18³ occupancy. Feed it `mass/8` as the density instead of 0/1, and a
  partial surface becomes a smooth collision surface at no extra cost. This
  change is small and has a large payoff.
- **Debris island scan:** unchanged, since it floods SOLID only. A partial
  powder with mass ≥ 4 still counts as a `powderAnchor` / settle support;
  below that it does not.

---

## 5. Sources of partials (without these the feature is invisible)

In landing order:

1. **Vessels (P1).**
   - The scoop ledger (`sim_mutate.wgsl:217-221`) credits `powderMass` instead
     of a flat 8.
   - Pour and spill (`container.cpp:344-386, 663-690`) write the remainder as
     a partial cell instead of discarding it "as dust".
   - The "a powder cannot be a fraction of a cell" rationale in
     `container.h:1-31`, `:210` and `item.h:458-466` is retired, and the
     `vessel` gate's remainder expectations are rewritten (20 eighths → 2 full
     plus one 4/8, not "2 plus dust").
2. **Brush (P3).** `BrushOp` gains a mass field (0 meaning full, for
   back-compat). `sim_mutate` writes `powderCode(mass)`. Tuner and F-key UI
   get a "fine" brush. `opWouldWrite` is keyed on material, which is fine,
   because the op owns the cell.
3. **Burning bodies (P3).** A burning limb currently emits one full ash cell
   per world cell touched (`mob.cpp:13957-13977`). Emit
   `count(burnt sub-voxels in that cell) / (skinScale³/8)` eighths instead, so
   a corpse leaves a dusting rather than a pile of cubes.
4. **Spells and particles (P3).** Powder particles already carry a payload
   nibble, and reinsertion keeps it (`sim_particle.wgsl:722-745`). A spell
   spray can set a partial code, and the landed grain then falls and merges in
   the CA. Two partial particles cannot merge in one claim (`atomicMax`, one
   winner); they land in separate air cells and the CA merges them. That keeps
   the claim unchanged.
5. **Worldgen fractional surface (P5, with §2.3).** `genCell` knows the
   continuous height field. The top powder cell of a column gets
   `mass = clamp(round(fract(h) * 8), 1, 8)`, quantised so that neighbour
   deltas are at most T. Buried bulk is unchanged (full, JITTER), so page
   residency does not move: surface chunks are real pages already. This is
   the one change that makes EVERY sand slope a ramp, and it has the biggest
   blast radius. The `gen-settle`, `terrain` pass D and `sleep` gates are the
   judges.

Micro spray stays a stain, not a deposit (`common.wgsl:3417` explains why:
rounding a sub-voxel spray up to matter invents mass).

---

## 6. Rule 2 (cost scales with activity)

- **Dispatches and bindings:** none new. The CA work is a few comparisons on
  words already read, in branches that run only when a fall or diagonal was
  refused.
- **Sleep:** every new move strictly lowers E (§2). There are no neutral
  moves and no lateral equalisation. A settled partial surface writes
  nothing, so its chunk drops off the dirty list the same tick a full-cell
  surface would. The repose tier blend is not used by the fine rule (§2.3),
  so there is no positional disagreement that could oscillate.
- **Mass-per-cell amplification.** A 1/8-grain source makes 8 times as many
  moving cells per unit of matter. Sources charge budgets per CELL, not per
  mass. A "fine sandstorm" is bounded by the same per-tick op and particle
  caps as a coarse one, and just carries less matter.
- **Render:** the partial path runs only on hits on surface partials, and is
  bounded by `detMax` plus a 10-step brick. Bulk and full terrain are
  untouched until P5 makes surfaces partial. P5 is therefore where the render
  budget is re-measured on the desert.

---

## 7. Gates

- **New `powder-mass` gate** (`selftest_ca.cpp`, verifiable standalone):
  1. rain 1/8 grains onto a partial bed: exact Σmass, and all cells full or a
     single top partial per column;
  2. full sand onto a dust film: the sink swap happens, and Σmass per
     material is exact;
  3. 3/8 snow melted by heat gives 3/8 water: Σ eighths conserved across the
     reaction;
  4. the room is asleep within N ticks.

  Thresholds (N, and the awake bound) go in `tests/baseline.json`.
- **`repose`**: change "mass exact" from counting cells to summing
  `powderMass`. Identical today, and correct after.
- **`vessel`**: new remainder expectations (§5.1).
- **P5 only:** `repose` gains a fine-repose arm (measured run:rise must stay
  within tolerance of the whole-cell arms). `gen-settle`, `terrain` and
  `sleep` must pass with `sim.powderFineRepose` on.
- **`determinism`, `ops-replay`, `ca-skip`**: P0 must leave the hash
  byte-identical, and that is the check for P0. From P1 on, the hash moves
  wherever partials exist; rebaseline once per landed phase.

---

## 8. Phases

| Phase | Content | Hash | Main risk |
|---|---|---|---|
| **P0** encoding | `powderMass`/`powderCode` in `common.wgsl` + `world.h` + `check_invariants.py`; fix pour/spill nibble 7; normalise remap | **byte-identical** (vessel gate may move) | `common.wgsl` edit = one ~9 min full shader recompile. Batch ALL `common.wgsl` changes of P0-P2 into this one edit |
| **P1** CA + ledger | merge, sink, diagonal merge, reaction mass carry, scoop credits mass, pour partial remainder, `powder-mass` gate | moves where partials exist | sink swap vs `flagSupportLoss`; reactions |
| **P2** render | detail kind POWDER, `tracePowder`, LOD slab, `isRayBlockerW` in traceOpaque / AO / occupancy, openness signature, under-liquid fill | none (render-only) | register cliff (§3); must stay ≤128 regs |
| **P3** sources | fine brush (BrushOp mass), ash in eighths from burning limbs, spell spray partials, tuner UI | moves | tuner/hub-file claims |
| **P4** collision | `KindOfWord` film rule, player/mob slab boxes, Jolt MC density | player gates may move | player.cpp is contended; gait/step-up regressions |
| **P5** ramps | fine repose rule (§2.3) + worldgen fractional surface, behind `sim.powderFineRepose` | moves everywhere | whole-map wake at load; settle cost ×3; must be measured, default OFF until it passes |

P0-P2 deliver discrete 1/8 grains that fall, pile, merge, are scooped and
poured, and draw as grains, with ramps wherever partial matter has
accumulated. P5 is what turns every existing sand slope into a ramp. It is
kept separate because it is the only phase that touches the world's resting
state.

Hub files to claim on the board:

- P0: `common.wgsl`, `world.h`
- P1: `sim_step.wgsl`, `sim_mutate.wgsl`, `container.*`
- P2: `raymarch.wgsl`, `sim_occupancy.wgsl`, `sim_openness.wgsl`
- P4: `player.cpp`, `debris.cpp`
- P5: `worldgen.wgsl`, `tuning_params.def`
- Update `DESIGN.md` (voxel word table: the powder nibble) in P0.

---

## 9. Audit: every powder creator, and what it writes

Writes 0..2, so it means FULL and needs no change:

- worldgen `genCell`, and the sentinels (GPU and CPU copies)
- `stream.cpp` RLE and the chunkstore predictor
- brush `sim_mutate:148` and exact-cell ops from prefabs (`prefab.cpp:13`)
- rubble (`debris.cpp:1560, 1715`) and settle-back `GridStateFor` (solids only)
- mob-burn products (`mob.cpp:14529, 14903, 15236`)
- `productState` and gas decay (`sim_gas.wgsl:677, 687`)
- the edit layer (`worldview.js:1559`; its liquid `8<<12` is a separate
  pre-existing bug)
- `lab.cpp` state 0, tree stamp `main.cpp:7050`
- spell spray payload nibble 0
- explosion payload and particle reinsertion (verbatim carry)

Writes above 2, so it must change in P0: vessel pour and spill (7), and
material remap across a class change.

---

## 10. Owner decisions (2026-09-26)

- **Q1. Films are walked through.** Powder with mass ≤ 2 is `Air` to
  `KindOfWord`. Partials of 3/8 or more get slab collision in P4.
- **Q2. A partial source whose product is a SOLID or GAS fires with its
  product scaled to its mass.** A gas product is spawned only if
  `hash < mass/8`; the owner had no preference, and this is the conserving
  default. A solid product needs the source to be FULL. So a 1/8 seed grain
  never becomes a whole sprout, but a 1/8 dust grain ignites 1/8 of the
  time a full one would. It is one constant per branch, so it is easy to
  flip.
- **Q3. Shadow/AO blocker at mass ≥ 5**, as the implementer's call. More than
  half a cell of grains blocks light like a cell. Films and half-cells do not.
- **Q4. P5 deferred.** Land P0-P4 and wait for the owner to inspect the
  behaviour before starting fine repose or worldgen fractional surfaces.
