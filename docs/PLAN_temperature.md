# PLAN: the temperature layer — heat, melting, ignition, climate, freezing

Status: **IMPLEMENTED 2026-10-02** (revision 3 + the deltas in the first section). Branch
`worktree-agent-aa9fa70a928a593d7` (fast-forwarded to main 6c55214).
Revision 3 folds in the owner's decisions: temperature changes GRADUALLY toward
a capped target (several sources add up, bounded radius), per-biome ambient with
a day/night swing, and FREEZING in scope (surface-limited, rate-bounded, with a
load-time validation that no biome straddles the freeze point and worldgen that
pre-ices permanently frozen water).

## AUDIT (2026-10-02, after the build) -- what it found and changed

DESIGN.md §4 "Heat" carries each of these as shipped.

- **The ceiling broke at dawn (fixed).** X is an excess over the ambient and
  survived the dawn's ambient step: T read day ambient + night X for the ticks
  the relaxation took (lava's neighbour: 44 + 226 = 270 > 230; embers could
  heat-ignite wood at a desert or meadow dawn). heatShift now lowers every kept
  page's X and X* by the block's step at dawn. `heat-ambient` part E asserts it
  every tick round a real dawn (and fails with the step disabled).
- **Tundra lakes stacked ice lids (fixed).** A fresh tarn soaked ~3 cells into
  its mud bed under its worldgen ice; each new surface froze: 1,538 freezes in
  a fresh default-map tundra window (the harness has no frozen climate, so no
  gate looked). Absorption now skips ground below 0 (frozen ground does not
  soak). `heat-ambient` part D asserts zero firings there.
- **The halo (fixed).** heatRelax woke the CA over EVERY moving chunk. It now
  wakes it only where a transition-bearing block's [T, T*] reaches the chunk's
  trigger; every other moving chunk relaxes on a PEND list without the CA. The
  limitation "a chunk refused the mark keeps its X" is gone with it: the field
  always reaches its target. Village fire: CA 30.1 -> 28.7 ms, awake 4,086 ->
  3,876 (no-heat main: 3,868).
- **Window edge (fixed).** After a shift the kept edge chunks are re-targeted,
  so a target cannot outlive sources that left the window.
- **F1 (fixed).** The probe was written only when the player's chunk relaxed,
  so it froze on the last warm reading; it is now heatBegin's, tagged with its
  block. The biome name was looked up by palette slot with a biome ID.
- **Dead switches removed** (`weather.iceMelts` / `waterFreezes`).
- **Instruments**: the heatMeta firing log (first 8 transitions by cell) and
  the check_invariants DIRTY_R_HEAT check.

## HEAT RISES (2026-10-02, after the audit) -- directional falloff

Owner: "a campfire must NOT scorch a bush beside it, but SHOULD ignite a bush
directly above it." The isotropic tent could not: flames emitted 110 against
foliage's 135, and with the ceiling no source lifts anything past itself.

- **Flames emit 140** (materials.json `fire`; ether_burning stays 110). Still
  under wood's 150, so flames never heat-ignite wood.
- **Direction gains**, one factor per axis on the tent weight (sim_heat.wgsl
  heatTent): y gives `sim.heatUpGain` to a source below the block,
  `sim.heatDownGain` to one above, 1 level; x and z give `sim.heatSideGain`
  off the block's column, 1 on it. Diagonals are products. Chosen over a
  dominant-axis classification (a cone edge at 45 degrees) and over a true
  angular blend (not separable: 13^3 taps a block). The gain weights the sum
  and the coverage alike: the ceiling is untouched.
- **Defaults: up 20, side 0.5, down 0.25.** The owner proposed 2 / 0.5 / 0.25;
  2 cannot work. A 4x4x4 flame covers ~2% of a block's 13-block surroundings
  (G x sf = 0.13 directly over it), so saturating the block 3-4 cells above
  takes ~8x before any margin; 20 gives 1.22 there, 0.82 for the block 1-2
  above and 2 cells beside (T 116), 0.47 for the bush 2 cells to the side at
  flame height (T 63-71). 24 would light that diagonal (T 138).
- **Passes reordered x, z, y** so the large up gain lands in the last pass
  (u32 registers) and the 16-bit planes only ever carry side <= 1 sums; the
  final mean is floored exactly from the raw sums (three-step division).
- **Knobs** are TP_F NO_WGSL rows converted to x16 fixed point in PrepareHeat
  (no prelude miss). Ranges: up 0..32, side 0..1, down 0..4.
- **What moved.** heat-ignite: wood across the gap from lava still lights (tick
  21) -- through its upper blocks; the bottom block, level with the pool's
  floor, now reads ~40 (the probe takes the post's hottest block). heat-ambient
  part E's probe moved to the lava's top row (the bottom row read night X 32:
  no teeth). heat-bound: unchanged (0 heat ignitions, ratios 1.00).
- **Gate `heat-plume`** (TickCursor; thresholds `heat.plume*`): above 3-4 cells
  lights at tick 150 (max 300), peaks 140; side 2 cells peaks 63, below 2
  cells 12, both never light over 900 ticks; burning leaves with foliage above
  peak 132 (their burn rule turns a little of the chamber to flame each tick)
  and never light it; the firing log names only the plate above.
- **Open**: `--sweep sim.heatUpGain=...` reports identical hashes -- the sweep
  scenario has no thermal transition in its 100 ticks and the heat pool is not
  hashed; reach is proven by tuning-reach (load) and heat-plume (kernel).

## LIVE KNOBS (2026-10-02, after heat rises) -- F1 sliders that reach settled heat

- **Panel.** F1 -> Temperature, under the readout: "heat on" (sim.heatMode),
  "reset to defaults", rise (up) 0..32, spread (side) 0..1, sink (down) 0..4,
  reach 2..8, intensity (gain) 1..64, snowline base -64..63 / swing 0..63 (the
  base + swing < 0 rule applied as LoadTuning does). Ranges are the def rows'.
  Same pattern as the wind / weather sections (CurrentTuning, sliders,
  SetCurrentTuning).
- **The mechanism.** Targets are rebuilt only when sources change, so a knob
  move never reached a settled field. PrepareHeat diffs the header's knob words
  (not the probe) against its last upload; on a change it bumps
  `kHpKnobEpoch` and SubmitTick wakes the world that tick (EncodeWakeAll).
  heatBegin sees the epoch differ from `heatMeta kHmKnobEpoch` and arms
  heatShift with `kHeatShiftRetarget`: every kept page goes on the recompute
  list (the edge re-target, applied to all pages). The wake lets a chunk whose
  new target crosses a trigger mark itself for the CA (the dirty bound); it
  stays marked until X reaches X*. Epoch and tracker restart on worldgen /
  load, so a fresh world never re-targets. Cost: one wake-all tick + one
  recompute of the live pages per tick a knob moved; nothing while idle.
- **Replay / net.** Outside the contract, like every live sim.* knob: the op
  record carries no tuning, the handshake compares tuning file stamps only.
  Deterministic under the same tuning sequence.
- **Found on the way: the wake was latched off after 3 ticks.** NoteWakeAll
  stamped the previous tick (SubmitTick wakes before NoteTickInputs), so the
  settled snapshot of the tick before the wake (4 ticks latent) proved the
  world settled at wake + 3 and skipped every C_CAACTIVE row. Stamp is now
  curTick_ + 1. The day-flip wake had the same hole.
- **Gate `heat-live`** (TickCursor; thresholds `heat.live*`): lava chamber,
  foliage wall 2 cells beside it settles at T* 114 unlit; side 0.5 -> 1.0 via
  SetCurrentTuning: T* 230 at tick 1, X walks 104 -> 220, the wall lights at
  tick 50 (max 200), lava unchanged, knob restored, twice-run identical. The
  detail line carries a per-tick X / X* / awake trace of the first 8 ticks.
- **Still open.** A field relaxing on the PEND list (no trigger in reach)
  stops walking when the whole window is proven settled (the heat rows are
  C_CAACTIVE): X can stall short of X* in a fully asleep world. Harmless to
  the sim (no transition can fire there) but visible in the F1 readout.

## IMPLEMENTED (2026-10-02) -- what changed from revision 3 below

The orchestrator approved revision 3 with these decisions, and the build
differs from the text below where listed. DESIGN.md §4 "Heat" is the
description of record for what shipped.

- **Authoring: materials.json `thermal` is the ONE surface.** There is no
  rule-level `heat` condition and no RCOND_HEAT bit: the transitions are run
  by `heatReact` (sim_step.wgsl) after the material's reaction bucket, from a
  per-material table in heatParams, so no CPU rule evaluator ever sees them.
  The four dead sun/night rules and `fill.surfaceMaterial` are deleted.
- **The target**: the tent filter carries the coverage-weighted emitter sum and
  the coverage; `T* = A + min(1, G x sf) x (mean E - A)` with `G =
  sim.heatGain` (8). A plain coverage mean made a lava pool's face reach only
  ~40% of its temperature a block away; the saturating gain lets big sources
  reach their own temperature a few blocks out while a lone burning voxel
  barely warms its neighbour. Still a convex combination: the ceiling holds.
- **Ignition tiers** in the shipped data: oil 100, foliage / grass / plants /
  thatch / straw / dust 135, cloth 135, wood family 150 (all `above`); emit
  fire 110, burning foliage / cloth / flesh 130, ember 140, oil_burning 150,
  molten 200-230. Melt snow 0, ice 4; freeze water below 0.
- **Pool**: 5,120 pages (30 MiB), > 2x the worst measured peak (village-fire
  2,278; forestfire 1,704). heat-bound fails on any refusal.
- **Persistence**: not saved; every resident chunk with matter is WOKEN by its
  fill (genChunk's act set, the store-hit RefilledSlot path), caMask finds its
  emitters and heatWant pages it -- one path for load, streaming and peers.
  heat-ignite saves and loads lava beside wood and asserts the wood still
  lights within the bound (it did: tick 20 after the load).
- **Dirty bound**: heatRelax marks a chunk only when a chunk of its 3x3x3 is
  dirty this tick, the CPU page-table mirror's one-ring bound.
- **Gates** use climate PINS (Simulation::SetHeatColumnPins), CA-only water
  (fluidExciteMode 0) for exact mass audits, and run melt / ignite / freeze
  twice comparing cells AND a slot-keyed hash of the heat pool.

**Scope**
1. Snow and ice melt into water when made hot — by a local source, or by a warm
   climate they were carried into.
2. Certain combustibles ignite on their own once heated past a threshold.
3. Ambient temperature per biome with a day/night swing. A desert day melts
   snow and ice and can never ignite anything.
4. Water freezes to ice where the ACTUAL temperature is below its freeze point:
   surface only, so a lake skins over instead of freezing solid.

**Out**: boiling, heat damage to creatures, updrafts, glowing/melting metal, lava
cooling. Each is meant to be a data edit later (a material's `thermal` block or
a rule's `heat` condition), not a new kernel.

---

## 0. What exists today (inventory)

There is no temperature anywhere in the world sim. "Hot" is a BINARY TAG and
every heat effect is an adjacency reaction rule that reads it.

| mechanism | where | sim / render | what it does |
|---|---|---|---|
| `tag:hot` | materials.json: fire, ember, lava, molten_glass, molten_salt, molten_iron, oil_burning, ether_burning, leaf/pine/autumn_burning, cloth/linen/hair/flesh/undercloth_burning, slaking_lime, smoke_charge | sim | the tag every ignition / melt rule's neighbour predicate names |
| ignition pairs | reactions.json `X + tag:hot -> burning form` (wood 12, leaves 133, grass 220, oil 700/1000, cloth 200, ...) | sim, hashed | fire spreads cell to cell by ADJACENCY, all x`combustion.spreadPct` (29 shipped); `neighborChance {fire}` tails x`flamePct` |
| lava / molten_iron + `tag:organic` | R:559, R:2154 | sim | adjacent organic -> fire, 60 |
| snow / ice + `tag:hot` | R:637 (500), R:624 (400) | sim | adjacent melt; snow keeps its eighths (`carriedState`), ice comes out full |
| sun melt / night freeze | R:872 / 882 / 895 / 630 | sim | **dropped at load today** (`weather.iceMelts` / `waterFreezes` false) |
| day/night wake | `SubmitTick` (support.cpp:1886) -> `EncodeWakeAll` | sim | re-dirties the WHOLE world on the tick daylight switches. Already paid twice per in-game day |
| lava's hot COAT | lava `coat`, `MobSystem::BurnOneLimb` | body-only, CPU | not on the grid |
| body burning | `BurnOneLimb` reads `tag:hot` cells off the CPU mirror | CPU, fire written through CellOps | untouched |
| `heatSpill` | — | — | deleted (DESIGN.md §9.g) |
| `burnTint`, `render.fire*` / `lava*`, glow field, far fire plumes | common.wgsl, sim_glow, farplumes | render-only | look only |
| melt brush | `tools.laserMeltRadius` + material `molten` | sim, MutationQueue | a tool, not a field |
| biome `climate.temperature` (0..1) | biomes.h:101 | parsed, **read by nothing** | — |
| water preset `fill.surfaceMaterial` | assets/water/*.json, biomes.cpp:244 | authored, **never applied by worldgen** | — |
| bench vessel heat | flasksim | bench-only | untouched |

**Coexistence rule:** the adjacency rules stay exactly as they are and keep
owning "touching fire catches / touching heat melts". Heat ADDS what adjacency
cannot express: matter NEAR a hot source but not touching it, and matter in the
wrong CLIMATE. A cell adjacent to flame gets both rolls; the heat roll is
authored small beside the adjacency one (§7), so a fire front's speed stays set
by the existing rules. Bodies never see the field.

---

## 1. One temperature scale

Abstract **heat units, 0 = water's freezing point**, signed.

- **Ambient** `A(column, day)`: per biome, -64..+63, two-level (§5).
- **Local excess** `X(block)`: u8 0..255, stored only near sources (§2). 0 =
  nothing local.
- **Actual temperature** a rule sees: `T = A + X`.
- **Target** `T* = max(A, min(A + Σ, M))` where `Σ` is the saturating, distance
  -weighted SUM of source contributions within radius R and `M` is the hottest
  emitter within R. Stored as the target excess `X* = max(0, min(Σ, M - A))`.

The excess `X` relaxes toward `X*` gradually (§3). Several sources ADD (Σ),
capped by `M`: a passive thing never gets hotter than the hottest source near
it, and never colder than the climate.

---

## 2. Sources and the target field (bounded radius, explicit cap)

**Sources.** A block's source is `E` = the highest `thermal.emit` among its 8
cells and `S = E * n / 8`, n = emitting cells (the fill factor: a lone flame
cell is 1/8 of a full block of flame — mass matters).

**Target, per 2³ block: a separable tent filter, three axis passes.**

```
A_x(b) = sum_{|d|<=R} (R+1-|d|) * S(b + d x) / (R+1),  Mx(b) = max_{|d|<=R} E(b + d x)
A_y    = same over A_x along y,                       My = max of Mx along y
Σ, M   = same over A_y along z,                       M  = max of My along z
X*(b)  = max(0, min(Σ, M - A(b)))
```

- **Radius is exact and explicit**: `R = sim.heatRadius` blocks (default 6 =
  12 voxels, max 8 = one chunk). Beyond R a source contributes NOTHING. The
  footprint is the product of three linear tents — a point source gives `E` at
  its own block, falling linearly to 0 at R+1 along each axis.
- **Sum with a cap**: sources add (`Σ`), the cap is `M`, the hottest emitter in
  reach. Intermediate sums saturate at u16 (any stage value that large already
  implies `Σ >= 255 >= M`, so saturation never changes `X*`).
- Integer, sums and maxes are order-free, each pass reads only the previous
  pass's data -> deterministic. Each pass is its own dispatch: pass Y reads
  OTHER chunks' A_x, so it must not share a dispatch with pass X.
- **Why not the alternatives** (both measured on paper, both rejected):
  *light-style max propagation* has a hard radius and settles fast but cannot
  ADD sources (owner requirement); a "propagated sum" (`S + Σ neighbours - f`)
  counts every path to a source and grows to the cap everywhere in a connected
  region (a fixpoint of `x = 6(x - f)`), so it has no radius at all. *Analytic
  per-source sums* cost `(2R+1)^3` taps per block. The separable tent is
  `3(2R+1)` taps per block (39 at R 6) with the exact radius.
- **Paging consequence**: with R <= 8 blocks every nonzero `A_x`, `A_y`, `X*`
  lies inside the 27 chunks around a source chunk, so the layer pages exactly
  N27 of every emitting chunk (§4). A chunk whose `X*` is 0 everywhere also has
  `A_x = A_y = 0` everywhere, so an unpaged chunk reading as 0 is exact.

**Recompute only on change.** Targets are recomputed in N27 of a chunk whose
sources changed (or on the day/night tick, because `X*` depends on `A`). A
standing lava pool next to wood computes its targets once.

---

## 3. Actual temperature: gradual approach, the ceiling, sleep

Per block, per tick, only where `X != X*`:

```
d  = X* - X
X += sign(d) * max(1, |d| >> k)          k = the block's inertia
```

- `k` = the largest `thermal.inertia` (0..7) among the block's cells; default
  by class (gas 1, powder 3, solid 3, liquid 4), authored where it matters
  (water 5, stone / metal 4). An all-air block uses 1. Water heats slowly,
  air quickly — that is the time lag.
- `max(1, ...)` makes the approach reach `X*` EXACTLY in finite ticks (<= ~40
  at k 3 from 255), so "T == T*" is a reachable, testable state.
- **No neighbour exchange.** The spatial coupling lives in the target (the
  tent), the temporal lag in the relaxation. A diffusion term on top would pull
  a block below its target wherever a neighbour is cooler, so the fixpoint would
  no longer be `X == X*` (breaking the sleep test) and a neighbour above the
  target could push a block past it (breaking the ceiling).

**The ceiling, stated so it can be checked.** Each step moves `X` toward `X*`
without overshoot, so `X <= max(X_prev, X*)`; by induction `X` never exceeds the
largest target the block has had, and `X* <= M - A`, so

> `T = A + X <= max(A, hottest emitter within R) <= CAP = max(ambientMax, max emit in the material table)` (= 230, lava).

**No runaway.** Ignition creates new emitters, which raise `Σ` — but `T*` is
capped by `M`, the hottest emitter in reach, and an ignited cell's emitter is
its burning product, whose `emit` is data. The ignition tiers (§6) put every
burning product's `emit` at or below the `above` of the material that produced
it where a chain must not form (embers 140 cannot heat-ignite wood, `above`
150; burning foliage 130 cannot heat-ignite foliage, `above` 135). So for those
materials the feedback loop has ZERO gain however much is burning; where a gain
exists (embers / lava onto foliage) it is bounded by radius R, by the rate
(§7) and by finite fuel, and `heat-bound` measures it.

**Sleep.** A block is done when `X == X*`; a chunk is done when every block is
and no source in its N27 changed. `heatRelax` marks its own chunk dirty
(`DIRTY_R_HEAT`) only while some `X != X*`, so a settled region marks nothing
and sleeps WITH its page (a standing lava pool keeps its warm halo, costs
nothing, and the CA reads it the moment anything enters it).

---

## 4. Storage and kernels

**Storage (`world.h kHeat*`, GPU-owned):**
- `heatPool`: `kHeatPoolPages` pages, 512 blocks x 3 words = 6 KiB:
  - word 0, persistent: `X` (u8) | `X*` (u8) | n<<4 + k (u8) | `E` (u8);
  - words 1, 2, scratch for the A_x / A_y passes (`Σ` u16 | `M` u8).
  **4,096 pages = 24 MiB** (12.5% of window slots). Free-stack pages are
  ALL-ZERO, always, so allocation needs no clear.
- `heatMeta`: per WINDOW slot (`kNumChunks`; tickets get none) `entry` (page |
  HAS_PAGE), `flags` (EMITTER from caMask, RECOMPUTE, RELAX, FREE), `wc` (the
  world chunk the page belongs to); want bitset; free stack + top; the three
  work lists + counts + indirect args; per-rule heat words (§6); the AMBIENT
  column table (§5); the last day/night bit; stats (pages in use / peak,
  refused, melts, ignitions, freezes, list sizes, player / crosshair probe),
  folded into the snapshot like `solMeta`'s header (world.cpp:948).
- Per material: `MaterialGpu._r2` bits 12..23 (low 12 = catch form; the rest
  unused): emit (8 bits), inertia (3 bits). No struct growth.

**Rows (`assets/shaders/sim_heat.wgsl`, new).** All but `heatShift` run after the
CA under `C_HEAT` = `C_CAACTIVE && sim.heatMode`. A settled world records none.
An active world with no heat dispatches zero groups in all but `heatWant` (one
group per dirty chunk, a few loads, return).

| row | dispatch | what |
|---|---|---|
| `heatShift` | 128 groups, only on a tick whose window origin moved (CPU condition off TickParams: an input, deterministic); top of the tick | release every page whose `wc` != `slotWorldChunk(slot, origin)` |
| `heatWant` | indirect, per dirty chunk | window only. Paged -> SRC list. EMITTER flag (caMask) -> want every unpaged window chunk of N27. Clear EMITTER |
| `heatArgs` + `heatAlloc` | 1 thread + ONE workgroup | prefix sum over the want bitset in SLOT ORDER: rank r takes stack[top-1-r] if r < top, else REFUSED (counted). New pages -> SRC list, record `wc` |
| `heatSrc` | per SRC chunk | read the 4,096 voxels: per block `E`, n, k. Changed (or new page, or the day/night bit flipped) -> RECOMPUTE flag on every paged chunk of N27 (atomicOr; first setter appends to the RECOMPUTE list). Self -> RELAX list |
| `heatPassX`, `heatPassY`, `heatPassZ` | per RECOMPUTE chunk, 3 dispatches | the tent passes; Z writes `X*` (reads the ambient table) and puts the chunk on the RELAX list |
| `heatRelax` | per RELAX chunk | the approach step; `X != X*` anywhere -> mark self dirty; all of `X`, `X*`, n == 0 -> FREE |
| `heatFree` | per RELAX chunk | FREE: zero the page, push it, clear the entry; clear the chunk's list flags |

- **Rank-by-slot allocation, not an atomic pop**: with a bounded pool an atomic
  pop makes WHICH chunks get pages a function of scheduling (the solute pool
  avoids that only by aborting). A one-workgroup prefix sum over a 4 KiB bitset
  is microseconds and turns exhaustion into a deterministic, counted refusal:
  the refused chunk is a cold gap and retries next tick.
- **caMask hook** (sim_step.wgsl): caMask already loads every cell and Material
  of every dirty chunk; one added test raises the chunk's EMITTER flag. A new
  fire asks for heat without a second 4,096-voxel scan over every dirty chunk
  (~4,000 in a smoke-filled scene).
- **Lists**: appended by the first setter of a per-slot flag (atomicOr's old
  value), so each slot is listed once and each list is a deterministic SET;
  each row processes chunks independently, so list order is unobservable.
- **Races**: every pass writes only its OWN page; passes Y and Z read other
  chunks' output of the PREVIOUS dispatch; the CA reads byte `X` of word 0,
  written by last tick's `heatRelax`. No atomics on data, only on flags/lists.
- **Tickets: no heat**. `heatWant` skips slots >= `kNumChunks`; a neighbour
  outside the window (or in a ticket) is cold; ambient is window-only. Heat
  rules are therefore inert in a ticket, consistent with tickets running decay
  only so things finish. Resolution is always `chunkInWindow` +
  `chunkSlotIndex` or `slotWorldChunk(slot, T.origin)` — never a bare mask.
- **Pass table**: one self-contained block after the solute rows, plus in-place
  edits: `caMask`/`caMask1` gain `A(HeatMeta)`; `ca`/`ca1` gain `R(HeatPool)
  A(HeatMeta)`; worldgen / load-reset tables gain the heat reset rows. Nothing
  touches the queue tags the async-compute branch adds.

---

## 5. Ambient climate — analytic, never stored per cell, never diffused

**Data.** Each `assets/biomes/<name>.json` `climate` gains
`"ambient": {"base": <units>, "swing": <units>}`.

**Evaluated, not stored.** `A = base(column) + (isDaytime(T.dayPhase) ? swing :
-swing)`, computed only where a cell carrying a heat rule is already being
evaluated (the CA's `heatChance`) and in pass Z. `base`/`swing` live in a table
per WINDOW chunk column (32 x 32 words in `heatMeta`), filled by the CPU from
`World::MapBiomeAt` at each column's centre (the seeded map read worldgen uses)
on a window shift and on a map / biome reload, through the tick's ordinary
upload (4 KiB on a shift tick). Its content is a function of the world column
only, so it is replay-exact.

**Two-level on purpose.** Day and night switch exactly on the daylight
boundary, which is the tick on which `SubmitTick` ALREADY re-dirties the whole
world for the light-gated rules. Ambient therefore changes only on ticks when
every chunk is being re-evaluated anyway: this feature adds no wake of its own,
and no crossing can be missed. A smooth curve would change ambient on ticks
nothing wakes for, and each crossing would then need a wake (the dawn-wake
hazard) or be missed (a bug). The ambient step itself is instantaneous: a
gradual ambient would need a stored temperature in every cell of the world
(rule 2). The validation below guarantees the step never crosses a threshold of
anything the biome itself holds, so it is visible only to matter carried in.

**Load-time validation — REFUSES bad data** (biomes.cpp, the `biomes` gate
re-checks; a refused biome is a load error naming the biome and the numbers):
1. **No straddling the freeze point**: for every material with a `freeze`
   transition (water: below 0), a biome's night and day must be on the SAME side
   — both below (permanently frozen: ice stays) or both at/above (water never
   freezes from ambient). No freeze-at-night / melt-at-dawn cycle anywhere, by
   construction.
2. **No ambient ignition**: `day < min(ignite.above)` over every material.
3. **No self-melting**: for every material the biome GENERATES (skin, cover,
   features, water presets — the names biomes.cpp already resolves) with a
   `melt` transition, `day <= melt.above`. A dawn can never melt a biome's own
   landscape.
4. **Frozen water is born frozen**: a permanently frozen biome gets ice on every
   water surface from worldgen (§8), so a cold lake never "freezes over on first
   load" — a one-time world-wide wake.

**Proposed values** (none exist today; revision 2's alpine and pine straddled
the freeze point and are changed here):

| biome | base | swing | day | night | class |
|---|---|---|---|---|---|
| tundra | -24 | 12 | -12 | -36 | permanently frozen (generates snow; tarns iced) |
| alpine | 10 | 8 | 18 | 2 | never freezes (was -8/10: straddled) |
| pine | 8 | 6 | 14 | 2 | never freezes (was -2/8: straddled) |
| forest | 10 | 6 | 16 | 4 | never freezes (was 6/6: night exactly 0) |
| meadow | 12 | 8 | 20 | 4 | never freezes |
| swamp | 14 | 4 | 18 | 10 | never freezes |
| ocean | 10 | 3 | 13 | 7 | never freezes |
| desert | 24 | 20 | 44 | 4 | never freezes; day melts snow/ice; 44 < 100, the lowest ignition |

What follows: tundra snow and ice never melt from climate; snow carried to the
desert melts day and night (desert night is 4, above snow's 0; faster by day);
meadow and forest melt carried snow slowly; water poured in the tundra skins
over; water anywhere else never freezes from climate.

---

## 6. Material schema, compiled into the existing reaction machinery

```json
"lava":  { ..., "thermal": { "emit": 230, "inertia": 4 } },
"fire":  { ..., "thermal": { "emit": 110 } },
"snow":  { ..., "thermal": { "melt":   { "above": 0,   "full": 64,  "chance": 20, "into": "water" } } },
"ice":   { ..., "thermal": { "melt":   { "above": 4,   "full": 80,  "chance": 15, "into": "water" } } },
"water": { ..., "thermal": { "inertia": 5,
                             "freeze": { "below": 0, "full": -24, "chance": 3,
                                         "into": "ice", "partialInto": "snow", "surface": true } } },
"leaves": { ..., "thermal": { "ignite": { "above": 135, "full": 220, "chance": 20 } } },
"wood":  { ..., "thermal": { "ignite": { "above": 150, "full": 230, "chance": 15 } } }
```

- `emit` 0..255; `inertia` 0..7.
- `melt` / `ignite` fire when `T > above`; `freeze` when `T < below`. The chance
  ramps linearly from 0 at the threshold to `chance` per-mille at `full`.
  `into` defaults: melt -> the material's `molten` (snow/ice already author
  `water`); ignite -> its CATCH FORM (`bodyreact.h CatchFormTable`: wood ->
  ember, leaves -> leaf_burning, grass -> fire).
- The loader COMPILES each into DECAY rules appended at the END of the
  material's bucket (existing rule indices, and so every existing
  `hash3(rnd, ri, slot)` draw, unchanged), carrying `RCOND_HEAT` (cond bit 29;
  24..28 are the fx id) and a per-rule heat word in `heatMeta` (threshold,
  full, kind, flags), plus a FRONTIER scale through the existing
  `scaleByNeighbors` machinery:
  - melt counts faces that are NOT this material (`invert`): a bank's inside
    cannot melt before its surface — the local, receding look;
  - ignite counts AIR faces: a buried log has nothing to burn in;
  - freeze counts faces that are not water (banks first) and adds the SURFACE
    flag (§7.1). `freeze` compiles to TWO rules: full water -> `into`
    (ice), partial water -> `partialInto` (snow, which keeps the eighths), so
    a freeze is mass-exact both ways (ice melts back to a full cell; snow
    melts back to its eighths).
  - ignition rules take the same `spreadPct` every other ignition takes.
- `reactions.json` rules may carry `"heat": {"above"|"below", "full"}` directly —
  the same compiled form, for later uses (boil: water -> steam above 200).
  Refused (warning, rule dropped) on anything but a DECAY on a non-gas self.
- The four dropped sun-melt / night-freeze rules (R:630/872/882/895) are
  DELETED: the climate model replaces them, and keeping a second, disabled
  authoring of the same physics is the two-surfaces problem guideline 4 names.
  (They are dropped at load today, so deleting them moves nothing.)

**In the CA** (`doReactions`' DECAY arm, before `scaledChance`; not on
`synthSelf` excited fluid, which is in motion):

```
if (rule.cond & RCOND_HEAT) {
  chance = heatChance(ruleIndex, c, rule.chance)   // 0 => inert: no keepAwake
  if (chance == 0) continue;
}
```

`heatChance`: `A` from the column table, `X` from the block (window slot ->
entry -> no page: 0; else one byte), `T = A + X`, threshold / full / ramp, and
for SURFACE rules "the cell above is air" (one read at distance 1, inside the
lattice guarantee). A rule that can fire holds its chunk awake — it consumes its
input (snow -> water, wood -> ember, water -> ice), the finite-process exception
the light-gate note allows; one that cannot is inert and holds nothing. A firing
`atomicAdd`s its kind's counter. Everything after is the existing path
(`reactWriteSelf`: eighths carried, support-loss flags, solute, dirty marks).
No heat kernel writes a voxel; nothing CPU-side writes one (rule 3).

Other rule evaluators SKIP `RCOND_HEAT` rules (one line each): gas parcels
(`sim_gas` `gasDecayProduct`), the CPU body evaluator (`reactcpu.h`), the
alchemy bench. Bodies are out of scope; the bench has its own burner.

CA cost: one small function at one call site, reached only by materials that
carry a heat rule (the solute "+7% from binary size" lesson).

**Proposed data (v1, starting points tuned against the gates):**

| emit | materials |
|---|---|
| 230 | lava, molten_iron |
| 200 | molten_glass, molten_salt |
| 150 | oil_burning |
| 140 | ember |
| 130 | leaf/pine/autumn_burning, cloth/linen/undercloth/flesh/hair_burning |
| 110 | fire, ether_burning |
| 40  | slaking_lime |

| transition | materials | threshold / full / chance |
|---|---|---|
| melt | snow; ice | above 0 / 64 / 20; above 4 / 80 / 15 |
| freeze | water (full -> ice, partial -> snow, surface) | below 0 / -24 / 3 |
| ignite | oil | above 100 / 180 / 30 |
| ignite | leaves family (9), grass, plant, stem, flower, petal, vine, thatch, straw_bed, dust | above 135 / 220 / 20 |
| ignite | cloth, robe_trim, robe_shadow, linen | above 135 / 220 / 15 |
| ignite | wood, birch_wood, bark_dark, bark_light, plank, timber, door_wood, staff_wood, charcoal | above 150 / 230 / 15 |

Not given an `ignite` (deliberately): body materials (skin, flesh, muscle,
leather); the ~50 flammable plants with no catch form; the chemistry
(gunpowder, ether, phosphorus, hydrogen, sodium...).

---

## 7. Bounds

### 7.1 Freezing: a lake skins over

- **Structural thickness bound**: a freeze rule fires only on a water cell whose
  cell ABOVE is air. Once a column's top is ice, the water under it has ice
  above and can never freeze. Ice is therefore at most ONE cell thick per
  water column, ever — a lake cannot freeze solid, in one night or in a
  thousand.
- **Rate**: `chance` 3 per-mille at full cold, ramped by the non-water face
  count (a bank cell with 3 such faces goes ~3x faster than open water with
  only the air above). Open surface: mean ~330 ticks (11 s) per cell; banks
  first, the middle last.
- **Why there is no global "N freezes per tick" counter**: a shared counter
  makes WHICH cells win depend on GPU scheduling (rule 1). The per-cell chance
  is the rate budget, and the work it can ever do is bounded by the number of
  surface cells (finite, consumed).
- **When it can happen at all**: only where `T < 0`, i.e. in a permanently
  frozen biome (validation 5.1) on water that worldgen did not make (poured,
  melted, carried), or after a heat source that had melted ice is gone. Bounded
  by the water present and by the source.
- **Sleep**: a matching surface cell holds its chunk awake until it has frozen;
  water under ice and water in a never-freezing biome are inert. A dusk can
  never match a lake: no biome freezes at night only (5.1).

### 7.2 Ignition: subcritical

1. **Zero gain where a chain must not form** (§3): `T* <= M`; embers (140)
   cannot heat-ignite wood (150); burning foliage (130) cannot heat-ignite
   foliage (135); flames (110) cannot heat-ignite anything but oil — however
   much is burning, in any biome (ambient <= 44).
2. **Bounded reach**: no ignition farther than R blocks (12 voxels) from an
   emitter hotter than the threshold.
3. **Small rate**: the remote gains that remain (embers / lava / molten onto
   foliage and wood) ramp from 0 at the threshold; e.g. foliage at 140 next to
   embers: (140-135)/(220-135) x 20 x spreadPct 0.29 ~ 0.3 per-mille per tick
   against the adjacency rule's ~38.
4. **Needs air** (ignite's frontier scale).

**Proved in a gate** (`heat-bound`): one grove burned twice in one process,
`sim.heatMode` 0 then 1; thresholds in baseline.json: peak awake chunks on/off
<= 1.25, burn-out time ratio, heat ignitions / all ignitions <= 0.25, pool
peak; after burn-out + cool-down: 0 pages in use, the fixture asleep.

---

## 8. Worldgen: frozen water is born frozen

`fill.surfaceMaterial` is authored on every water preset and never applied.
This package applies the RULE instead of the per-preset field: in `genCellIn`,
the TOP cell of any water column (`L.fluid == water`, `y == L.fluidTop - 1` —
ponds, authored lakes and the sea all go through the one `fluidTop` decision at
worldgen.wgsl:3658) in a column whose biome is permanently frozen becomes
`ice`; the far field (`farSurfaceMat`) paints the same. The biome table packed
into the worldMap buffer gains the derived "frozen" bit (day < water's freeze
point); no preset changes, and a modder's frozen biome is iced automatically.
This moves the world hash (rebaselined once at the end) and is the one
worldgen.wgsl edit (paid inside the same single shader-recompile batch).

The gate (`heat-ambient` part C): the first N ticks after generation show ZERO
freeze, melt and ignite firings — on the harness map, and over a tundra region
of the default map located by `MapBiomeAt` (the harness lists tundra but its
pad may not stand in it).

---

## 9. Persistence, determinism, window

1. **Not saved, not hashed.** `X` is ephemeral process state like the MPM excite
   scratch, rebuilt from the emitters standing in the world; ambient is a pure
   function of map + tick. `LoadWorld`, F7 regen and every worldgen reset the
   layer and refill the ambient table. After a load a burning chunk is dirty
   (fire always is), so its targets are recomputed on the first tick and `X`
   climbs back within ~40 ticks; a SETTLED lava pool's halo returns when its
   chunk is next woken — at the latest the next dawn/dusk wake-all. The whole
   save/load difference: heat-driven melt/ignite/freeze lag around sleeping
   emitters until then. Adjacency rules and ambient-driven melt/freeze are
   unaffected. (The determinism gate, ops-replay and the twice-run comparison
   all start from worldgen, where the layer is empty in both runs.)
2. **Determinism.** Integer only; the CA's dice unchanged (rules appended); every
   pass writes only its own page and reads only a previous dispatch's output;
   atomics only on flags / lists (order-free set semantics) and counters; the
   allocation rank is a prefix sum; ambient from `T.dayPhase` and a CPU table
   that is a function of map + origin. Paged == dense: heat reads voxels only
   through `voxWordAt`, and its pages are a separate pool. Proven by `--gate
   determinism` plus each heat gate running its fixture twice and comparing the
   melted / ignited / frozen cell SETS.
3. **Window shift**: `heatShift` releases mismatched pages before the CA reads;
   the ambient table is refilled in the same tick's upload; a chunk that leaves
   and returns starts at ambient.

---

## 10. Gates (selftest_heat.cpp; thresholds in tests/baseline.json `heat.*`; each runs alone with `--gate`)

Sealed stone-room fixtures under a roof (no sky), dim-dawn pinned like
`oil-fire`, built through cell ops, ticked with `support::RunTicks` /
`TickCursor` (the `oil-fire` W2-O pattern only if the rig disturbs a sealed
fixture). Each fixture pins its ambient by overriding its columns' table entries
in-process (restored on exit, the way `repose` patches a material), so the gates
do not depend on which biome the harness puts under them.

- **`heat-melt`**: lava trench behind a 1-cell stone wall; snow bank and ice
  block 2-6 cells beyond; a second bank 3R blocks away; ambient pinned frozen.
  Asserts: near snow and ice melt within N ticks; water eighths gained ==
  snow eighths + 8 x ice cells lost (exact); the far bank untouched;
  surface-first (no interior cell melted while all 6 faces were still snow);
  `X` rises gradually (sampled: no block reaches `X*` in one tick at k >= 2) and
  never exceeds `X*`'s running max; lava removed -> `X` back to 0, pages 0,
  fixture asleep.
- **`heat-ignite`**: lava pool; wood post 2 cells away across an air gap
  (asserted never touching, so `lava + tag:organic` cannot be what lit it); a
  wood post R+2 blocks away; stone and glass posts at the near distance; a pile
  of EMBERS next to wood in a second room (zero gain: never heat-ignites wood,
  only adjacency may). Asserts: near wood ignites within `heat.igniteTicksMax`;
  far wood never; stone / glass unchanged; the ember room's heat ignitions 0.
- **`heat-bound`**: §7.2.
- **`heat-freeze`**: frozen ambient; an open-topped basin of full water and a
  shallow partial film. Asserts: after N ticks every surface column has exactly
  one ice (or snow, for the film) cell on top and liquid under it; eighths
  exact; no column has two frozen cells; the first M ticks froze less than
  `heat.freezeEarlyMax` of the surface (gradual, banks first); then a lava block
  melts a hole, is removed, and the hole refreezes — and the fixture sleeps.
- **`heat-ambient`**: (A) two pinned columns, tundra-like and desert-like, snow
  and ice in each, real day/night with a shortened day so the real `SubmitTick`
  wake path runs dusk -> night -> dawn -> day: the cold column's snow is
  untouched all cycle; the desert column's melts; awake chunks fall back under
  `heat.ambientAwakeMax` within N ticks of each boundary. (B) the REAL biome
  data passes validations 5.1-5.4, and a deliberately straddling biome built
  in-process is refused. (C) §8: zero heat firings in the first N ticks after
  generation on the harness and over a default-map tundra region.
- **`heat-idle`**: an active world with NO heat source (water poured for 200
  ticks): step / pass counters stay 0, pages 0, no `DIRTY_R_HEAT`.

Final verify (owner rules: no acceptance / bare selftest / smoke):
`--verify heat-melt,heat-ignite,heat-bound,heat-freeze,heat-ambient,heat-idle,sleep,determinism,oil-fire,stain-react,fire-down,biomes,terrain`
(+ a `--shot` if the readout gets a visual), then `--selftest --gate
determinism --rebaseline` once.

---

## 11. Knobs (`tuning_params.def`, ONE batched edit — a prelude-wide SPIR-V miss)

| row | default | range | meaning |
|---|---|---|---|
| `sim.heatMode` | 1 | 0..1 | 0 = no heat row recorded, layer reset on the transition, heat rules inert (ambient included) |
| `sim.heatRadius` | 6 | 2..8 | R, in 2³ blocks; <= 8 keeps every contribution inside N27 |

Inertia, emit and thresholds are material data. The tent arithmetic and block
geometry are consts in `sim_heat.wgsl`; the layout `sim_step` and `sim_heat`
share goes through world.h -> `ShaderConstantPrelude()` (+ check_shaders.sh);
`RCOND_HEAT` beside the other `RCOND_*` in common.wgsl, mirrored in
materials.h. With the worldgen edit, all in ONE batch: one ~10-15 min shader
recompile for the package.

---

## 12. Dev visibility

- F1 -> Sim -> **Heat**: at the player — biome, ambient base, the day/night
  term, the local excess `X` and target `X*` at the player's feet, the actual
  `T`; the same at the crosshair (a probe word filled by `heatRelax` / the pick
  pass, folded in the snapshot); pages in use / peak / pool, refused, list
  sizes, melts / ignitions / freezes per second.
- No render overlay planned (binding the pool in `raymarch` is a register-cliff
  risk for a dev view); proposed later if the readout is not enough.
- last_run.json `heat` block: pagesPeak, refused, melts, ignitions, freezes,
  recomputeChunksPeak, relaxChunksPeak.

---

## 13. Cost estimate

- **No heat anywhere**: settled world — nothing recorded. Active world without
  heat — `heatWant` one group per dirty chunk (~3 loads, return), one thread,
  zero-group indirects: < 0.05 ms.
- **Ambient**: zero passes. One table load inside `heatChance` per evaluated
  heat-rule cell (snow, ice, water, fuels in dirty chunks).
- **CA**: one function at one call site; +1-2 loads per heat-rule cell.
- **Standing source** (a lava pool): one recompute, then nothing.
- **Busy fire**: sources flicker every tick, so N27 of every burning chunk
  recomputes each tick: 3 x 13 taps x 512 blocks ~ 20 K reads per chunk, plus
  `heatSrc`'s 4,096 voxel reads per burning chunk. At ~1,000 burning chunks
  (~2,500 recompute chunks) estimated 0.5-1 ms. Lever if measured too high:
  recompute targets at most every 4th tick per chunk (slot-phased), which the
  relaxation lag hides. Measured before/after on `--perf --scenario forestfire`
  and reported.
- **Memory**: 24 MiB pool + ~0.8 MiB meta.

---

## 14. Open questions for the orchestrator

1. **No neighbour exchange** (§3): spatial coupling in the target, temporal lag
   in the per-material approach. Exchange would break the exact `X == X*` sleep
   fixpoint and the ceiling. OK?
2. **Ambient is two-level and steps instantly** at the daylight switch (§5): the
   only way to add no wake and miss no crossing without storing temperature
   everywhere. The validation makes the step invisible to native matter.
3. **Authoring surface**: materials.json `thermal` (melt / ignite / freeze /
   emit / inertia) compiled to rules, plus rule-level `heat` in reactions.json
   for later uses (same compiled form); the four dead sun/night rules deleted.
4. **Not hashed / not saved** (§9.1).
5. **Tiers**: flames 110 < foliage 135, burning foliage 130 < foliage 135,
   embers 140 < wood 150: heat never chains foliage-to-foliage or wood-to-wood.
   If the owner wants "a bonfire scorches the bush beside it" from flames, the
   foliage `above` drops under 110 and `heat-bound` decides if that is safe.
6. **Pool 4,096 pages (24 MiB)** with deterministic refusal; `heat-bound`
   records the peak and the pool can grow if a forest fire refuses.
7. **Worldgen ices frozen-biome water by a rule** (top cell of every water
   column in a frozen biome), not by `fill.surfaceMaterial` per preset (§8).
