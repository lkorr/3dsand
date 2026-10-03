# PLAN: gas particles — voxel smoke inside the window, particle smoke outside it

**Status (2026-10-02 audit): P0, P1 (stage 1) and stage 1b are ON MAIN** (rebaselined
`c747298`, crossfade `ec5769a`, 2026-09-09). P2 (stage 2) is still unstarted.

Original status: **P1 stage 1 COMPLETE on branch `gas-stage1`**, rebased onto chunk
tickets P0 (`e808931`) and finished with the one batched `common.wgsl` edit
§2.2 always called for. The gas motion model — `gasRndK`, `windLateralStartK`,
`windAxisFrac`, `gasLateralRot`, `GasIntent`, `gasIntentK`, `gasLadderStep` and
the two wind ramp constants — has ONE definition there, which both the CA and
the particle kernel call; `PFLAG_GAS` and `PT_K_GAS` joined their own blocks;
and `sim.gasMode` is on the tick stream through the full `TUNE_*` pipeline.

**`sim.gasMode = 0` reproduces the world exactly as it was without any of
this** — determinism hash `ffd2807b`, which is the value chunk tickets P0
recorded, and `gas-leave` then FAILS with the top chunk plane sheeting across
47 awake chunks. At 1 the same plane is quiet (0 awake) and 2,656 voxels
converted at the edge. That pair is the whole of stage 1 in two numbers.

P0's measurement stands as written in §0, and its headline finding — that
`treeburn` does not run on the shipped map — is still open for whoever owns
worldgen. P2 (stage 2, §2.10) is optional and unstarted.

**STAGE 1b IS ALSO COMPLETE (2026-09-09): the two representations crossfade.**
Stage 1 left the seam it created: voxel smoke inside the window, parcel smoke
outside it, meeting at the face with no overlap, so there was a visible line
where 0.1 m voxels became 0.8 m cells. Three changes, all render-side, sim
untouched, `determinism` hash unmoved at `9bfed213`:

1. `gasOuter`'s cell went from a BYTE to a u16 (4 MiB). A byte capped a 0.8 m
   cell at 192/512 = 37.5% full, invisible while the box held only distant
   parcels and a visible thinning the moment a dense in-window plume crossfaded
   into it. Measured by `gas-leave`: the in-window splat reaches **204 in one
   cell** twenty ticks after the 4,096-voxel puff, so 192 clips this fixture.
2. The CA splats every in-window gas VOXEL into the same box, once per tick
   (`sim_step.wgsl` `gasOuterSplat`, `simBGL_` binding 34, `A(GasOuter)` on the
   `ca` row). Without this there is nothing on the coarse side to fade IN.
3. `raymarch.wgsl` crossfades over the outer shell of the window: a smoothstep
   on the max-norm distance from the WINDOW CENTRE (so the weight is exactly 1
   at all six faces regardless of camera), inner edge at the new
   `render.gasBlendStart` (default 0.5 = 12.8 m). Voxels fade out by `1 - b`,
   and only into a non-empty coarse cell; `gasOuterFill` grew a second segment
   with its own step budget covering the same shell and now runs for rays that
   HIT inside the window. RenderParams bit 3 keeps the whole path at zero cost
   in a world with no gas.

`gas-leave` grew two hard assertions for the two things that would otherwise
turn the whole crossfade off with every other number still green: the render
flag must arm (320/400 ticks) and `gasOuter` must be non-empty INSIDE the
window at `gasLeaveSplatTick` (sum 3,437, max 204).

DESIGN.md §5 "Stage 1b" has the full argument. Known and stated rather than
solved: gas in a SLEEPING chunk is not visited by the CA and so is not splatted
(mitigated — a voxel never fades into an empty cell), and the coarse
contribution still does not feed `gasHalfT` or `fireGlow`.
Companion: `docs/PLAN_chunk_tickets.md` (independent; tickets are for matter
that LANDS, particles are for gas that KEEPS GOING).

**What stage 1 shipped, against §4's acceptance.** `gas-leave` and
`gas-reenter` are in `src/test/selftest_gas.cpp`, thresholds in
`tests/baseline.json` (`gasLeave*` / `gasReenter*`), both green:

    gas-leave    top chunk plane at t150: 0 awake (bound 8), while 2,658
                 voxels converted at the face; parcels 2,522 by t100, peak
                 2,522, 0 left at t400; above the window at t200: 680 (GPU
                 counter) / 669 (CPU page walk); gasOuter above the top: max
                 44, sum 669; refusals 0 list + 0 pool over 400/400 snapshot
                 ticks; twice-run identical over 400 hashed ticks with the
                 gas digest compared at t200 over a live population
    gas-reenter  256/256 queued and drained; 251 reconverted, first landing 2
                 ticks after the spawn; 207 smoke voxels still standing 20
                 ticks later (decay predicts ~210); population back to 0

Deviations from this document, all deliberate and all recorded in `world.h` or
DESIGN.md §5 beside the code:

- `gasOuter`'s cell is **0.8 m, not the 0.4 m of §2.5**. The section asked for
  0.4 m cells AND a 2x-window span AND 2 MiB and those three never agreed:
  128³ bytes IS 2 MiB, and 128 × 0.4 m is 51.2 m, half the stated span. Span
  and memory are kept.
- **The splat is folded into `gasResolve`** rather than given its own dispatch
  (§2.5 implies a `gasSplat` pass): resolve already walks every live parcel
  exactly once, and the splat is one atomic per parcel.
- **`--residency dense` is not an arm a gate can run.** §4 asks `gas-leave` for
  "paged == dense"; `World::Residency` is fixed at Init and process-level, and
  a gate that switched it would re-point every other gate's page table
  underneath it. The gate asserts twice-run equality (hash series, gas digest,
  population) and the dense arm is run the way `determinism`'s is, by giving
  the binary `--residency dense`.

**The rule-1 problem this section used to state is CLOSED (2026-10-03).**
`gasLeave` charged a shared `atomicAdd` cursor, so WHICH voxels were refused
when the per-tick list filled was decided by which workgroup arrived first, and
a refused voxel stays in the grid. The pool's overflow had the same shape (and
"a dropped parcel cannot move a voxel" was wrong: an undropped one can blow back
in and land). The fix is not a separate mark pass over the grid — the voxel word
has no spare bit to mark with — but a deterministic BUDGET: fixed before the CA
(`gasLeavePrep`, pool room included), split evenly over the dirty chunks that
touch the edge (counted by `camask`), and spent within a dispatch by a
workgroup-local rank on a per-cell hash after the colour's cells have run
(`gasLeaveDefer` + main's leave-resolve tail). The spawn pass places records by
rank, not cursor. DESIGN.md "The edge's refusals are a function of the world"
has the argument; `gas-leave-overflow` forces the overflow and checks the
twice-run agrees tick for tick.

**Revision 2 changelog.** Revision 1 moved smoke off the grid everywhere. The
owner's requirement that gases stay full reaction participants (combine, fuse,
combust, new substances) is met most cheaply by NOT doing that: a smoke voxel
inside the window already has the entire reaction system. So stage 1 keeps
smoke a voxel wherever reactions happen and converts it to a particle only at
the window edge — the edge becomes a sink instead of a wall. Revision 1's
whole-grid migration is kept as an OPTIONAL stage 2 (§2.10) and nothing in
stage 1 has to be redone to reach it.

## 0. Why (read from the code, not assumed)

- `smoke` is a `CLASS_GAS` voxel (`materials.json:135`), moves every tick, one
  decay rule at 9/1000 (`reactions.json:29`) = mean life ~111 ticks, ~11 m of
  rise. `fire` dies at ~150/1000 = ~6.5 ticks. Smoke is ~17x longer-lived and
  is essentially all of a plume's volume.
- `tryMove` returns false out of window (`sim_step.wgsl:305`). A gas voxel at
  the window's top face cannot leave, falls through to stage 3 lateral spread
  (`:2047`) and sheets across the whole top chunk plane — up to 1,024 chunks
  plus the dilated layer below — until it decays. The sheet is clipped at a
  chunk-aligned plane that jumps 1.6 m per `Stream::ShiftAxis`; that is the
  visible "chunk outline" in the sky.
- The CA charges per AWAKE CHUNK: 27 colour phases partition the chunk, x2
  substeps = 2 full sweeps of 4,096 cells regardless of fill
  (`sim_step.wgsl:1858`, `simulation.cpp:125`).
- The particle system (`sim_particle.wgsl`) is one thread per particle, keyed
  on particle STATE never on buffer slot (`particlePriority`,
  `common.wgsl:2765`), bit-deterministic, `kParticleCap = 262,144` x 32 B,
  already covered by the determinism hash (`support.cpp:1787`). A particle
  leaving the window is deleted at exactly two sites (`:135`, `:235`).
- `windAtQ` is pure and already called from the particle kernel.
- **MEASURED 2026-09-09 (P0), and the headline is that `treeburn` DOES NOT
  RUN on the shipped map.** `--scenario treeburn` refuses before it starts:

      canopy scan: 38 candidate chunks above ground, best holds 0 foliage
                   voxels at chunk (0,-1,0)
      treeburn SKIPPED - no chunk with 200+ foliage voxels within 14 chunks of
               the window centre

  ZERO foliage voxels in 38 above-ground chunks, with `debug.vegetation = 1`
  and a `treeline` of 228 in `assets/worldmap/default/map.json`. That is not a
  thin canopy on an unlucky seed, it is no vegetation at all at the default
  spawn — so the plan's phase-0 scenario is currently unavailable, and this is
  a finding for whoever owns worldgen rather than something the gas package can
  work around. (RTX 3060 Ti, 1920x1080, paged residency, base commit f9fbcb9.)

  The substitute is `--scenario explosion`, the other CA-wide scenario, which
  answers the sim/render shape of the question if not the smoke part of it:

  | | |
  |---|---|
  | frame, p50 / p95 / p99 | **23.18 / 25.65 / 27.07 ms** (43 fps p50) |
  | GPU passes, total attributed | **9.04 ms/frame** |
  | ... CA loop (`caLoop`) | 2.61 ms |
  | ... MLS-MPM fluid | 2.40 ms |
  | ... shadow cache | 2.43 ms |
  | ... openness + glow | 1.09 ms |
  | ... occupancy / worldgen / readback / particles / pages / compact / explode | 0.52 ms combined |
  | remainder (raymarch draw + present) | ~14.1 ms |
  | **peak awake chunks** | **87** (p50 61) |
  | ballistic particles | p50 200, peak 21,017 |

  So on this scenario the frame is RENDER-bound (~61% of it is the raymarch
  draw), the CA is 2.6 ms, and the awake-chunk count never approaches the
  1,024-chunk sheet this plan exists to delete — because there is no fire and
  therefore no smoke. **The number this plan actually needs is still
  unmeasured, and it cannot be measured until the default map grows a tree.**

- **THE `sleep` GATE'S GAS COLUMN WAS A BLANK, and P0 fixed that instead.**
  `DIRTY_M_GAS` already existed (bit 22) and the histogram already printed it,
  so the plan's "add a DIRTY_M_GAS count" was nominally done. It was also
  useless: the bit was set at exactly two of the gas ladder's five stages (the
  bare rise and the four up-diagonals), and a plume in calm air reaches
  NEITHER — `gasIntent` returns straight up, the tryMove in front of the chain
  succeeds, and the cell returns having marked only `DIRTY_R_MOVE`. The
  top-plane SHEET is stage 3, the flat lateral ring, which never marked either.
  The one column that exists to name smoke read 0 for a world full of it.

  Now every successful gas move marks, split by the axis it moved on:
  `gas` (the move changed the parcel's HEIGHT — progress, and the only one of
  the three in `FILM_LICENCE`), `gas-lat` (purely horizontal — the sheet, and
  NEUTRAL, so it must not license a film step), `gas-edge` (the intent pointed
  out of the window — the cells at the sink). `kDirtyReasonName` in `world.h`
  is now the single list; it existed twice, in `World`'s snapshot fold and in
  the `sleep` gate, each with a comment saying it must match the other.

## 1. Goals and non-goals

Stage 1 — THE DELIVERABLE
1. A gas voxel that would leave the window through any face is converted into
   a gas PARTICLE instead of being refused. The window edge is a sink.
2. The particle keeps the grid's behaviour: integer cell position, one cell
   per tick, the same `gasIntent` buoyancy/wind model and the same fallback
   ladder. It rises and drifts without a ceiling, bounded only by its
   authored decay and an outer box.
3. Plumes are visible beyond the window (far-march sampling of a coarse
   density box).
4. A particle that drifts back into the window becomes a voxel again
   (re-entry = reconvert), so it rejoins the reaction system.
5. Inside the window nothing changes: smoke is a voxel, every pair / emit /
   decay rule in `reactions.json` applies to it exactly as today. Gas
   combinations are authored as they are now.

Stage 2 — OPTIONAL, LATER (§2.10)
6. Smoke (then any gas flagged `"particle": true`) off the grid inside the
   window too, with the reaction system extended to particles (partner
   table). Only worth it if stage 1's measurement says in-window gas cost
   still matters.

Non-goals
- `fire` stays a voxel at every stage (igniter, emitter, light source).
- No reduced fidelity: a particle takes the moves a voxel would.
- Outside the window, gas does not react. Nothing outside the window reacts
  today either; nothing is lost.

## 2. Design

### 2.1 Representation

`Particle` unchanged (32 B): `payload` bits 0..11 material, 12..15 state; new
`PFLAG_GAS`. Position is 24.8 fixed with a zero fraction — it lives ON a cell
and moves in whole cells. Velocity words stay zero so `particlePriority`
remains a pure function of the visible state. No age field: death is rolled
from `hash3(seed, tick, stateHash)` like the CA's per-cell roll.

### 2.2 Motion — the grid model, verbatim

Move `gasIntent`, `windAxisFrac`, `gasLateralRot`, `lateralDir` and the gas
fallback ladder into `common.wgsl` so the particle kernel calls the SAME
function the CA does. ONE deliberate `common.wgsl` edit (the ~9-minute
far-cascade recompile, paid once — CLAUDE.md "a `common.wgsl` edit is NOT a
cheap WGSL edit"); "two shaders must AGREE" is exactly the criterion for
putting something there, and `windAtQ` already lives there. The alternative —
duplicate into `sim_particle.wgsl` with a `check_invariants.py` byte-equality
rule — is cheaper today and a trap later.

Per tick, per gas particle at cell `c`, material `m`, identity key `k`
(hash of `c`, `m`, and the particle's spawn tick — position-derived, never a
buffer slot):
1. `g = gasIntent(c, m, k, rnd)` → try `c + g.dir`.
2. Failing that, the ladder from `sim_step.wgsl:2018-2052`: up, four
   up-diagonals in `windLateralStart` order, four laterals.
3. A target is free iff it is not a blocker (§2.4). Gas particles do not
   exclude each other; several may share a cell.

### 2.3 The edge conversion (stage 1's core change)

In the gas branch of `sim_step.wgsl`'s movement tail, each `tryMove` that
fails because `!inBounds(dst)` — as opposed to failing on `canDisplace` — is
today indistinguishable from a solid wall. Split it:

```
// gas voxel at the window face: leave as a particle, do not sheet along it
if (rising && !inBounds(c + g.dir)) { gasLeave(c, w, m); return; }
```

`gasLeave`: `voxStore(idx, 0)` (the cell becomes air), `markVoxActive`,
`markDirty(c)` (the cell changed), and append `{cell = c + g.dir, mat, state,
tick}` to `gasSpawn` — a GPU append buffer (`atomicAdd` slot, `kGasSpawnPerTick`
cap, charged BEFORE the write, refused if full: the existing budget
convention). Refused → the voxel stays and behaves as today (falls through to
the lateral ladder). So the edge is a sink with a rate limit, never a hole
that loses mass silently.

Any face, not only +Y: a plume drifting out of an X/Z face keeps drifting
downwind outside. The -Y face is included for uniformity (a gas rarely goes
down; a downdraft-pinned one may).

The particle `integrate` pass consumes `gasSpawn` next tick (one-tick latent,
like every CPU→GPU path). Spawn-list slot order is scheduling-dependent;
nothing keys on it — each entry is a complete particle state.

Reach: `gasLeave` writes only its own cell (reach 0). The dropped voxel's
target cell is outside the window and was never writable. Rule 1 holds.

### 2.4 Outside the window

Blocking: `inWindow(c) == false` → the far cascade's level-1 byte at `c`
(`farVox`, the same test the far march uses) is a blocker → blocked; else
free. Dense toroidal array, direct index — **not the page table**. Coarse,
and right for a plume.

Kill bounds (rule 2): leaving the OUTER gas box (§2.5), or rising above
`origin.y + WORLD_N + kGasCeilingVox`, or the authored decay (§2.7). All
three are stateless tests.

### 2.5 Rendering beyond the window — the outer density box

`gasOuter`: u8 per 0.4 m cell, 128³ over 2x the window edge (102.4 m
centred on the window), 2 MiB as packed u32 words. Cleared and re-splatted
every tick by `gasSplat` (`atomicAdd(1 << 8*(cell&3))`; integer addition is
commutative so the result is scheduling-independent, though render-only).
Saturation: 255 particles in one 0.4 m cell is unreachable under the cap; a
saturating second pass is not needed in v1 — note it.

The far march samples `gasOuter` with trilinear filtering after `Hit.tExit`,
adding `density * materials[smoke].opacity` to the same media accumulator the
near march uses, so tint / `gasHalfT` / fire-glow attenuation are one code
path. Soft at that distance, which is correct. Colour: one gas material
(`smoke`) in stage 1; a packed `(mat, density)` `atomicMax` byte is the stage
2 option.

No inner volume in stage 1: inside the window smoke is a voxel and renders
as it does today.

Register pressure: this is a fetch in the FAR loop, not `trace()`'s DDA;
still, `--shader-stats` before / after (`gotcha-raymarch-register-cliff`).

### 2.6 Re-entry = reconvert

A gas particle whose next cell is inside the window has nowhere to render
(no inner volume) and would be invisible. It RECONVERTS: it proposes itself
as a `smoke` voxel at that cell through the existing claim / `resolve` path
(`atomicMax` priority, deterministic winner, losers retry next tick). That
write touches the page table and dirties the chunk — correctly, since the
voxel has rejoined the simulated world. Bounded: one claim per re-entering
particle per tick. This is the ONE place a gas particle meets the page
system.

### 2.7 Lifetime — the material's decay rules, read from the bucket

The particle kernel binds `reactions` (new binding; `materials` is already
bound) and walks `m.reactOffset..+reactCount` for `RK_DECAY` entries, one
roll per rule, first hit wins — the same as `doReactions`:
- product `air` → dies;
- product a gas material → morphs (`payload` material changes);
- product a grid material (`steam → water`) → proposes a landing through the
  claim path at its current cell if that cell is in the window, else dies
  (rain outside the window is nothing today either).

`reactions.json` stays the single authoring surface; the decay chance you
tune for the voxel is the one the particle obeys. Pair / emit rules are not
evaluated outside the window (stage 1).

### 2.8 Determinism

Every roll keys on `(seed, tick, position-derived key)`; the splat is
commutative; spawn-list and claim order are irrelevant by construction;
`gasLeave` is a reach-0 self write. `--gate determinism` twice-run and
`--residency dense == paged` are the tests. **The world hash MOVES** (smoke
that used to sheet now leaves): `--selftest --rebaseline` once at the end of
phase 1.

### 2.9 Bounds and cost model

Per gas particle per tick: its 32 B in and out, one `windAtQ`, one `farVox`
byte, one atomic into a 2 MiB buffer, one hash and a short bucket walk. No
`voxWordAt`, no `markDirty`, no chunk wake, no CPU readback. `O(particles)`
in one dispatch versus the CA's `O(chunks touched) × 4,096 × 2`.

- Steady state = edge-crossing rate × lifetime (~111 ticks). 1,000
  voxels/tick → ~111k particles.
- Pool: `kParticleCap` is shared with ballistic debris. Raise to 524,288
  (+8 MiB) with a `kGasCapShare`, or give gas its own buffer pair. Decision at
  phase 1; own buffers is cleaner for the splat and the hash. At the cap,
  `gasLeave` is refused and the voxel behaves as today — degradation, not
  failure.
- Residual costs live in RENDER (one fetch per far step, per pixel not per
  particle) and in splat atomic contention on a dense plume (many particles
  per 0.4 m word serialise; tens–hundreds of µs worst case; workgroup-local
  pre-aggregation if it ever shows in a profile — not before).
- Compounding: the top-plane sheet stops existing, so the CA inside the
  window sheds up to ~1,000 awake chunks per big fire without any in-window
  smoke change.

### 2.10 Stage 2 — smoke off the grid everywhere (optional)

**Attributed 2026-10-03 (fire-gpu, DESIGN.md "The fire's CA, attributed"):
not the fire's lever.** On `--perf village-fire` gas cells are about a third
of the CA (substep-1 arm: ~40% of the move pass) and the per-chunk floor of
the gas-only chunks is most of the rest that smoke costs; on the forest fire
burning foliage outnumbers gas ~3:1. Taking in-window smoke off the grid could
remove that third and the gas-only chunks' floor, not the whole CA. The
cheaper lever measured instead is `sim.gasThinDecayMul` (thin smoke fades
sooner; off by default, a look decision).

Kept from revision 1, unchanged in substance, none of it required by stage 1:
- `"particle": true` on a material → `MATF_PARTICLE`; every product write in
  `doReactions` (~6 `voxStore` sites), `sim_mutate`, `sim_explode`, the
  `molten` field and the C++ body evaluator (`reactcpu.h`) go through one
  `emitProduct` helper that branches voxel / spawn.
- An INNER 1:1 density volume over the window (128 MiB) or 1:1 in a 256³ box
  around the camera (16 MiB), sampled in `trace()`'s DDA where voxel media is
  accumulated today.
- A per-cell partner table (`atomicMin(gasCell[cell], particlePriority)`) so
  each cell has one deterministic acting particle; it walks pair / emit rules
  against the six grid neighbours and the partners of the six neighbouring
  cells. Gas–gas combine / fuse is then an ordinary pair rule with
  `neighborClass: gas`. Grid-side products go through the claim path.
- Stage 1's `gasLeave`, motion, outer box, decay and re-entry are all reused
  as-is. Re-entry becomes "keep being a particle" once an inner volume exists.

## 3. Phases

**P0 — measure. DONE 2026-09-09** (§0): `treeburn` is unavailable on the
shipped map (0 foliage voxels near spawn), `explosion` measured as the
substitute, and the `sleep` histogram's gas column — which was a blank for the
case it exists to name — split into `gas` / `gas-lat` / `gas-edge`.

**P1 — stage 1. LANDED 2026-09-09 on branch `gas-stage1`** (see the status
block at the top of this file for the measured acceptance, the three
deviations and the one open rule-1 problem). One thing in the list below is
deferred rather than done: the motion port is a TEMP-DUP copy inside
`sim_gas.wgsl` compared byte-for-byte by `scripts/check_invariants.py`, not a
move into `common.wgsl`, because package T0 held that file — phase B moves it
and nothing else changes.
`PFLAG_GAS`; motion port (§2.2, the one `common.wgsl`
edit); `gasLeave` + `gasSpawn` (§2.3); `farVox` blocking + kill bounds
(§2.4); `gasOuter` + splat + far-march sampling (§2.5); re-entry (§2.6);
decay from the bucket (§2.7); pool decision (§2.9); gate `gas-leave`.
**Hash moves; rebaseline once.** Update DESIGN.md §5 (particles) and the
"unloaded space is solid and inert" sentence — it is now "solid, inert, and
a sink for gas".

**P2 — stage 2** (optional, §2.10), only after P1 is measured in play.

## 4. Gates (each `--gate <name>` alone; thresholds in `tests/baseline.json`)

- `gas-leave` (P1): emit 4,096 smoke by mutation at a fixture 6 chunks below
  the window's top face, open sky above; 400 ticks. Assert: by tick 150 the
  top chunk plane has ≤ `gasLeaveSheetMax` awake chunks (baseline: today's
  sheet, expected ~hundreds → ≤ 8); gas particle count > 0 by tick 100 and
  monotone non-increasing after emission stops; `gasOuter` non-zero above
  `origin.y + WORLD_N`; ≥1 particle above the window top at tick 200;
  spawn refusals == 0 (the cap was not the bound); twice-run hash equal;
  paged == dense.
- `gas-reenter` (P1): spawn 256 gas particles 2 cells outside an X face with a
  wind blowing inward; assert they become `smoke` voxels inside the window
  within 20 ticks and the particle count reaches 0.
- Existing: `sleep`, `determinism`, `mob-burn`, `fire-down` (their smoke now
  leaves through the ceiling instead of sheeting; if a threshold moves, that
  is a rebaseline, not a finding).

## 5. Risks and open decisions

- **Pool sharing vs own buffers** (§2.9). Recommend own buffers.
- **Re-entry is reconvert** (§2.6), not die. Stated; the `gas-reenter` gate
  pins it.
- **`common.wgsl` edit**: one, deliberate, in the P1 commit, with everything
  else that needs a shared definition batched into it.
- **Far-loop register pressure** (§2.5): measure.
- **Saturation at 255/cell** in `gasOuter`: unreachable under the cap; note
  rather than build.
- **What outside-window gas cannot do**: react. Same as today. If stage 2 is
  never built, gas chemistry is a window-only phenomenon — which is what it
  is now.
