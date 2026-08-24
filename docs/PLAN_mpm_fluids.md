# PLAN: MLS-MPM fluid rewrite

**Status update 2026-08-24: `docs/PLAN_fluid_overhaul.md` is now the active
work queue** — diagnosis of the goopy/clumpy look + perf strain, the fluid-lab
test world, and the concrete path through this plan's Phases 3–7. This document
remains the architecture of record for the hybrid design.

**Status update 2026-08-22 (worktree mls-mpm-liquidsim): Phase 0 PASSED and a
Phase-0+1 prototype is implemented** — `assets/shaders/sim_fluid.wgsl` +
the `fluid-det` gate (twice-run particle-buffer hash equality on the RTX
3060 Ti; cross-vendor still open, as it is for the CA). The prototype is a
side-by-side comparison build per the user's direction: the CA liquid is
untouched, the MPM liquid is placeable with the `mpm` tool, and the §7
excite/settle seam plus Phases 2+ remain unbuilt pending the comparison
verdict. See DESIGN.md §5 "MLS-MPM liquid prototype" for what shipped.

**Original status: research/design. Nothing implemented. Do not start until
the determinism spike (Phase 0) passes.** This document is the plan of record for
replacing the CA liquid simulation with a hybrid MLS-MPM particle fluid. It
was produced from a design conversation on 2026-08-22 and is written to be
handed to an implementing agent as the primary prompt. Read DESIGN.md §2–§5
and CLAUDE.md ("the three rules") before this document; every decision below
is downstream of those.

When implementation lands, the relevant sections of DESIGN.md must be updated
in the same commits (CLAUDE.md rule). This plan then becomes historical.

---

## 1. What we are doing and why

The CA liquid (powder rule + lateral flow + fullness equalization, DESIGN.md
§4) is mass-conserving and settles flat, but has **zero inertia**: no pressure
waves, no sloshing, no momentum, no real splashes, no buoyancy dynamics. John
Lin's fluid videos demonstrate what a voxel world gains from a real fluid
solver: volumetric water that flows through buildings, falls into caves, fills
containers, and navigates tunnels — fully dynamically.

His method (confirmed from his video description's citation) is **MLS-MPM**:

> Hu, Fang, Ge, Qu, Zhu, Pradhana, Jiang. *A Moving Least Squares Material
> Point Method with Displacement Discontinuity and Two-Way Rigid Body
> Coupling.* ACM TOG 37(4), SIGGRAPH 2018. DOI 10.1145/3197517.3201293.
> - PDF: https://yzhu.io/publication/mpmmls2018siggraph/paper.pdf
> - Reference code (MIT, incl. the 88-line core): https://github.com/yuanming-hu/taichi_mpm

**The decision:** liquids get exactly two states, with one seam between them.

- **Settled liquid** = fullness voxels in the world grid, exactly as today
  (material id + fullness-eighths in the state nibble). But *inert*: the CA
  liquid **movement** rules (lateral flow, fullness equalization, the liquid
  branch of the powder rule) are **deleted**. Settled water does not move.
  It still participates in everything else: reactions, staining, decay,
  save/load, streaming, the world hash, chunk sleep, rendering.
- **Excited liquid** = MLS-MPM particles in a sparse GPU solver. This is the
  **only** code path that ever moves liquid.
- **Two converters** — `excite` (cells → particles) and `settle` (particles →
  cells) — are the entire seam. They are the hard design work (see §7).

This is simultaneously "a full rewrite of the fluid system" (the CA fluid sim
is deleted; one movement model; one dynamics authoring vocabulary) and "a
hybrid" (bulk resting water is stored, saved, hashed, and reacted-with as
voxels). The two framings converge because of the memory argument in §5.3 —
at our world scale, resting water *cannot* be stored as particles, and the
compressed resting form is byte-for-byte our existing voxel representation.

Scope guard: **liquids only.** Powders (sand, ash) stay CA. MLS-MPM can
simulate granular media (Drucker–Prager) and the temptation to migrate sand
into it will arise; resist it. The falling-sand CA identity — authored
reactions, tags, emergent piles — is the product. Sand-in-water keeps the
existing CA density-displacement interaction against *settled* water and gets
a simple drag/impulse coupling against excited water (§6.4).

---

## 2. The paper, summarized for the implementer

MLS-MPM is a hybrid Lagrangian/Eulerian method. Particles are the state; a
background Eulerian grid is per-substep scratch, cleared every step.

Per substep:

1. **P2G (particle-to-grid).** Each particle scatters mass and momentum to
   the 3×3×3 = 27 grid nodes around it, weighted by quadratic B-splines.
   The paper's core trick: the stress/pressure force *fuses* into the same
   scatter. Traditional MPM computes forces via interpolation-function
   gradients ∇N; MLS-MPM shows that with an MLS/linear-polynomial view, the
   force term becomes `N_i(x_p) · Q_p · (x_i − x_p)` with
   `Q_p = Δt · V0_p · M_p⁻¹ · ∂Ψ/∂F · Fᵀ + m_p · C_p`
   (their §6; `M_p⁻¹ = 4/Δx²` for quadratic B-splines). One matrix-vector
   multiply per node in the inner loop; no kernel gradients anywhere in the
   algorithm. This is their measured ~2× speedup over optimized traditional
   MPM (their Table 2), and it makes the kernel small — the reference
   implementation's core is 88 lines.
2. **Grid update.** Per node: divide momentum by mass, apply gravity, apply
   boundary conditions (velocity projection: sticky / slip / separate with
   friction — their Eq. 25). One line of symplectic Euler.
3. **G2P (grid-to-particle).** Each particle gathers new velocity from the
   same 27 nodes and reconstructs its affine velocity matrix
   `C_p = D⁻¹ Σ N_i v_i (x_i − x_p)ᵀ` (APIC). Advect: `x += Δt v`.
   Deformation update for fluids reduces to tracking volume ratio
   `J *= 1 + Δt·tr(C)`.

**Per-particle state (fluid case):** position `x` (3), velocity `v` (3),
affine matrix `C` (3×3, traceless part matters for angular momentum), volume
ratio `J` (1). Mass is uniform per material. ~16 scalars.

**Pressure without a pressure solve:** liquids use a weakly compressible
equation of state (per Tampubolon et al. 2017, which the paper's water demos
use): pressure is a stiff function of `J` (e.g. `p = K·(1/J^γ − 1)` or the
simpler `p = K·(1 − J)`), entering as stress `σ = −p·I`. No global linear
solve. This is what makes the method real-time-viable and GPU-trivial — every
step is embarrassingly parallel except the P2G scatter.

**CPIC** (the paper's other half): colored distance fields let particles and
grid nodes on opposite sides of a thin rigid surface refuse to exchange data
("compatibility"), giving cutting, thin boundaries, and two-way rigid
coupling — incompatible nodes' momentum becomes impulses on the nearest rigid
body; particles near surfaces get projected "ghost" velocities and a weak
penetration-recovery penalty force. Overhead measured at ~6% because only the
narrow band near rigid surfaces runs CPIC. We need the *coupling* half of
CPIC (impulses, velocity projection), not the *cutting* half — our terrain is
voxels, not thin shells (§6.1), and thin rigid plates are rare for us.

**Performance reality check.** The paper is offline: their demos run 16–288
*seconds per frame* at 1–8M particles, `Δt` = 1e-4…5e-6 s (CFL-limited, many
substeps per frame). Their optimized CPU benchmark: 8M particles ≈ 0.5 s per
substep on 4 threads. Lin's real-time result (≤8 ms on 4 threads) is the
method scaled down: far fewer active particles, sparse dispatch, soft
stiffness, large `Δt`, few substeps. Our numbers must be sized the same way
(§8, open question O-11). Lin also reported CPU↔GPU transfer costs hurt him
badly; our solver is GPU-resident next to the sim buffers, so that specific
pain does not apply — but it means *we* must keep it GPU-resident: no
per-tick particle readbacks (rule: CPU↔GPU under ~1 MB/tick, async).

---

## 3. Architecture: the grid is the lingua franca

The single most load-bearing design fact: MPM's Eulerian scratch grid is
**colocated with the world voxel grid** (same cells, or an exact 2× coarsening
— decide in Phase 1; start 1:1). Every cross-system interaction happens in
grid cells, the coordinate system all existing systems already speak. No
pairwise special-casing.

| Interaction | Mechanism |
|---|---|
| terrain → fluid | boundary conditions on grid nodes read *directly from the voxel buffer* (solid/powder ⇒ project node velocity). No SDF, no meshing. Terrain edits change flow automatically. |
| fluid → CA / reactions | P2G already produces per-cell mass per material — expose it as a "fluid occupancy" field the CA kernels read (like `hasLiquid` today) |
| CA → fluid (consume) | cell→particle bins (free byproduct of the P2G sort, §6.2) let a reaction delete/convert specific particles deterministically |
| fluid ↔ rigid bodies | CPIC impulses accumulate on grid nodes → async readback (one tick latent, existing pattern) → Jolt; body poses upload as boundary data |
| fluid ↔ ballistic particles | debris DDA already visits cells; check fluid occupancy, apply drag, deposit momentum into local grid nodes |
| fluid ↔ player/mobs | swimming queries the per-cell occupancy field via the existing CPU mirror readback path |
| settled ↔ excited | the two converters, §7 |

The world grid remains the **single source of truth for what occupies
space**. MPM is the *excited state* of liquid, not a fourth peer
representation. Per DESIGN.md guideline §3, the fluid solver's particle
buffer + scratch grid are derived-plus-transient state with one authoritative
fact each way across the seam, and the converters are the named conversion
functions.

---

## 4. The solver, concretely

- **Particle pool:** structure-of-arrays GPU buffer, fixed capacity (budget
  knob, tuning.json). Ring/freelist allocation. Every particle: position
  (cell-relative fixed point), velocity, C (9 fixed-point scalars — evaluate
  quantization, O-13), J, material id (a few bits), flags.
- **Scratch grid:** sparse, allocated per active fluid *block* (align blocks
  to our 16³ chunks). Per node: mass + momentum accumulators (fixed-point
  integers, §5.1) and a fluid-occupancy output field. Cleared per substep.
  Only blocks in the active list are touched — dispatch via
  `DispatchWorkgroupsIndirect` over a compacted block list, exactly like the
  dirty-chunk machinery (rule 2).
- **Substep loop:** N substeps per 30 Hz tick (N from CFL: `Δt < dx/c`,
  `c = √(K/ρ)`; stiffness K is the knob trading incompressibility-look
  against substep count — O-11). Each substep: sort/bin particles by cell →
  P2G → grid update (gravity, terrain BCs, rigid projection) → G2P + advect.
- **Where it runs:** new WGSL kernels under `assets/shaders/` (e.g.
  `fluid_p2g.wgsl`, `fluid_grid.wgsl`, `fluid_g2p.wgsl`, `fluid_sort.wgsl`,
  the converters), new rows in `src/sim/pass_table.def` with **complete R/W
  sets** (the Vulkan backend generates barriers from those rows — an omitted
  read is an invisible cross-vendor hazard; `scripts/check_pass_table.py`
  enforces). Slim bind-group layout discipline applies (16 storage buffers
  per stage — see the particle/explosion pipelines' `simSlimBGL_` precedent).

---

## 5. Compliance with the three rules

### 5.1 Rule 1 — bit-determinism. THE gate. Unproven. Phase 0.

The problem: P2G is a scatter-with-accumulation — many particles add momentum
into the same node concurrently. Float atomics are non-associative; the sum
depends on scheduling order. That is exactly the nondeterminism class the
engine forbids.

The plan: **fixed-point integer accumulation via integer atomics.** Integer
addition is associative, so accumulation order stops mattering. Precedent:
QuanTaichi (Hu et al. 2021) runs MPM on quantized fixed-point grids; our own
spell VM is integer 24.8 for the same lockstep reason. But per-particle math
(B-spline weights, C, the EOS) must *also* be integer/fixed-point end-to-end
— WGSL float math is not cross-vendor-stable (FMA contraction, fast-math),
which is why the sim is integer-only in the first place. Nobody, to our
knowledge, has shipped a bit-deterministic cross-vendor GPU MPM. **This is
novel work and it is the project's kill criterion.**

Phase 0 (do first, in isolation, before any engine integration):

- Standalone fixed-point MLS-MPM kernel (small 3D box, dam-break scene, a few
  tens of thousands of particles). Selftest-shaped gate: run the identical
  sim twice, hash particle+grid state per N substeps, require equality. Then
  run on a second adapter (`--adapter low` precedent) and require equality
  across adapters. If cross-vendor hashing cannot be made to pass at
  acceptable cost/precision, **the project stops here, cheaply.**
- Fixed-point format decisions to make in Phase 0: position as
  cell-index + fractional offset (bounded range ⇒ no catastrophic
  cancellation); velocity/C/J ranges and overflow analysis (velocity is
  CFL-clamped, which bounds everything downstream); weight computation in
  integer (quadratic B-spline weights are small polynomials of the
  fractional offset — exactly representable to chosen precision); the EOS
  in fixed point.
- Also prove: **deterministic binning.** Sorting particles by cell must be
  stable and scheduling-independent (counting-sort/prefix-sum by cell id with
  stable tie-break on particle index is deterministic; verify on GPU).
  Deletion/compaction order likewise (O-2).

Everything else in this document is conditional on Phase 0 passing.

Note: the RNG rule carries over — any stochastic element (e.g. jittered
excite seeding) uses `hash3(seed, tick, cellIndex)`-style stateless counter
RNG, never anything seeded by dispatch order.

### 5.2 Rule 2 — cost scales with activity

- Weakly compressible MPM water **never settles on its own** — particles
  jitter forever. Lin's open-faucet problem ("waterfalls… eventually fill up
  the whole world", his own description) is what a fluid system without a
  resting representation looks like. Our settle converter is the answer:
  aggressive demotion to inert voxels (§7). A calm lake costs **zero** — not
  "cheap", zero: no particles exist, no blocks are active, chunks sleep.
- Sparse dispatch over the active-block list only; indirect dispatch; block
  activation/deactivation mirrors dirty-chunk semantics including the
  "mover crossing a boundary marks the *neighbor*" rule.
- **Hard particle budget**, charged *before* emission, op refused if it
  doesn't fit (the budget lesson from CLAUDE.md: emit-then-check overruns).
  Overflow policy precedent from DESIGN.md §5: when the pool is full, oldest
  /calmest particles force-settle immediately.
- The selftest's settled-world active-chunk ceiling extends to fluids: a
  poured-then-stilled pool must reach zero active fluid blocks within a
  bounded tick count. This is a new gate (§9).

### 5.3 Rule 3 — MutationQueue, save, hash, replication

- **Why resting water must be voxels (the memory argument):** ~8 particles
  per cell × 32–64 B ≈ **256–512 B per water cell** vs **4 B** as a voxel.
  A 1M-voxel lake: ~4 MB settled vs ~0.25–0.5 GB as particles — the latter
  exceeds the entire 512 MB world buffer. Sparse-sleeping particles save
  compute but not memory, save size, or streaming bandwidth. There is no
  version of this design where lakes persist as particles.
- **Save/load:** force-settle all particles at save time. A mid-splash save
  loses momentum — accepted, with precedent (the particle ring's overflow
  policy). Saves stay pure voxel RLE; the save format does not change.
  (Remember `kPersistMask` / SVR2 lessons: whatever transient bits the fluid
  system uses in the voxel word must be correctly masked.)
- **World hash:** decision needed (O-9). Recommended: hash ticks force-settle
  (or the hash includes a canonicalized, cell-sorted digest of particle
  state). The naive option — hashing the particle buffer in memory order —
  is wrong: buffer order is scheduling-dependent even when the physics is
  deterministic.
- **Mutations:** brush/spell/worldgen water continues to land through the
  MutationQueue as *voxel* ops (paint fullness cells); the sim excites them
  if they're unsupported. Excite/settle themselves are GPU-internal sim
  steps (like CA movement, not queue traffic). Faucet/emitter *sources* are
  queue ops. Net: the queue remains the complete replication/replay stream,
  because everything that *enters* the world still flows through it and the
  sim is deterministic given the op stream.

---

## 6. Cross-system coupling, in detail

### 6.1 Terrain (voxel solids/powders)

Grid nodes read the voxel buffer: a node whose support touches solid/powder
gets its velocity projected (start: separate + friction; slip/sticky
per-material via a JSON field). All neighbor reads bounds-check against the
residency window (`inWindow` + origin uniform — kernels think in world
coords, memory is slot-indexed, world→slot is the bitmask). **Out-of-window
is solid and inert** — this is also what keeps fluid from pouring into
unloaded space (Lin had to build this explicitly; we inherit it from the
residency rule). Terrain destruction under water needs no special code: the
boundary condition disappears, water flows.

### 6.2 Reactions and the CA

- **Read direction (free):** P2G's per-cell mass per material is exposed as a
  fluid-occupancy field. Authored cell rules can test "excited liquid X
  present here" the same way they test neighbors today. Settled liquid needs
  no changes at all — it *is* voxels.
- **Consume direction:** particles are binned per cell each substep anyway
  (P2G locality sort). A reaction consuming water in cell c takes particles
  from c's bin in fixed (sorted) order. Mass granularity: one voxel of
  fullness ≈ 8 particle-masses; partial consumption needs fractional
  accounting (a per-cell remainder, or particles carrying quantized mass).
  Bookkeeping, not architecture — but it must be deterministic (5.1).
- Most liquid reactions (evaporation, shore freezing, staining, soak)
  predominantly concern *settled* water and keep working untouched.

### 6.3 Rigid bodies (Jolt + microbodies)

CPIC's coupling half. Body poses/velocities upload per tick (they already do
for other systems); grid nodes near body surfaces project velocities against
the body's local velocity; the rejected momentum accumulates per-body as
fixed-point impulse sums; async readback applies them to Jolt one tick
latent. For our mostly-convex voxel-skinned bodies, a coarse voxelized
occupancy of the body (we already voxelize for carving) may suffice instead
of full triangle-splatted CDFs — evaluate in Phase 5 (O-8). Buoyancy is the
paper's own showcase (their Fig. 6); expect it to fall out.

### 6.4 Ballistic particle system

For liquids, MPM *replaces* most ballistic use (splash is native). Keep
ballistic voxels-in-flight for solids/powders/gore. Coupling: a ballistic
particle whose DDA enters a cell with fluid occupancy takes drag and deposits
its momentum into that cell's grid nodes (one-way is fine to start).
Sand-through-excited-water: same drag mechanism on CA movers is *not*
possible (CA has no impulses) — settled-water displacement stays CA;
excited-water regions simply also excite adjacent unsupported powder into
ballistic particles with the local fluid velocity. Keep this crude; refine
only if it reads badly.

### 6.5 Player, mobs, swimming

Existing swimming reads liquid state from the CPU mirror. Excited water must
appear in that mirror's per-cell liquid answer (fold the occupancy field into
the same readback product). Remember the mirror is 3×3×3 chunks — same
`Unknown` caveats as today. `inLiquid` is a *fraction, not a bool* (existing
gotcha) — occupancy gives a fraction naturally.

### 6.6 Gases and phase transitions

Gases stay CA. Evaporation: settle first (or consume from bins) → spawn CA
steam voxels. Condensation: CA rule produces settled water cells, which
excite if unsupported. Every phase transition crosses the seam through the
voxel form — never particle↔gas directly. This keeps all transition rules in
authored JSON.

### 6.7 Rendering (open area — O-10)

The raymarcher marches voxels; excited water is not in the voxel grid.
Options, in rough order of preference: (a) splat particles into a transient
per-cell fullness/velocity volume (the occupancy field again) and let the
existing translucent-liquid path march it — cheapest to integrate, blobby;
(b) screen-space particle splatting + bilateral smoothing composited with
the voxel ray result — the standard real-time particle-water look, but a new
render path with depth-composition questions; (c) surface reconstruction —
out of budget. Occupancy/ray-blocker bits (`packOcc`) must account for
excited-water cells so shadow rays and media marching behave. Start with (a)
for correctness, evaluate (b) for looks. Lin lists rendering among his own
"to be improved" items — this is genuinely unfinished territory for everyone.

---

## 7. The seam: excite and settle

The two converters are the highest-design-risk engineering (after Phase 0).

**Excite (cells → particles).** Trigger: a settled liquid cell becomes
unsupported (neighbor became air / terrain carved / adjacent block active
with sufficient momentum). Emit `fullness` × (particles-per-eighth) particles
— the fullness nibble maps 1:1 onto particle count (8 particles/cell ⇒ one
per eighth). Seed positions on a jittered lattice (counter-RNG), velocities
from local context, and — critically — **J consistent with hydrostatic
depth** (O-4): particles spawned at rest in a deep column under a weak-
compressibility EOS must start pre-compressed by their depth's pressure, or
every reawakened lake bounces ("jello pop"). Depth here = distance to the
liquid's local free surface; a per-column scan within the active block at
excite time is affordable.

**Settle (particles → cells).** Trigger per block: kinetic energy below
threshold for M consecutive ticks, near-hydrostatic J distribution, and
support beneath. Convert per-cell binned mass into fullness eighths;
**conserve mass exactly** across quantization by carrying the sub-eighth
remainder deterministically (per-column or per-cell residual pushed into a
neighbor/next eighth — never dropped; O-5). Deactivate the block. Result
cells are written with `kStampNever` (everything entering the world unstamped
uses the sentinel — CLAUDE.md).

**Progressive wake (O-6).** Poking one end of a frozen lake must propagate:
excite a shell around the disturbance; pressure at an active/frozen interface
above threshold excites the neighbor block (dirty-chunk-style frontier).
The far shore wakes only if the wave actually reaches it, and re-settles
after it passes. Waves across a large lake will be somewhat damped by the
freeze/thaw boundary — acceptable; tune thresholds.

**Hysteresis (O-7).** Excite and settle thresholds must be well-separated or
a pool edge thrashes (excite → jello wobble → settle → excite …). Budget-
charge on excite (5.2) also naturally back-pressures thrash.

---

## 8. Authoring surface

- Liquid materials keep their `materials/*.json` entry (id, tags, colors,
  stains, sounds, reactions — all unchanged) and gain a fluid-dynamics block,
  e.g. `"fluid": { "density", "stiffness", "gamma", "viscosity",
  "friction", "surfaceTension"? }`. One schema location: rows in
  `assets/tuner_schema.js`, wiki integration like every other material block.
  This *upgrades* authoring: viscosity/goo/honey become continuous sliders
  where the CA gave discrete hacks (lava's "quarters + every 3rd tick").
- Solver-global knobs (substep count/CFL target, particle budget, excite/
  settle thresholds) go through the standard tuning pipeline: row in
  `src/sim/tuning_params.def`, `sim.*` namespace (⇒ integer-only, hash-
  affecting, selftest required — rule 1 applies to these values).
- The `class: "liquid"` movement semantics change (inert when settled);
  `RENDER_PATHS` / wiki predicates in `assets/tuner.html` must be updated in
  step (standing invariant).

---

## 9. Phasing

Each phase lands green (`--selftest`) and is separately abandonable. Board
claims per phase (`scripts/board.sh`), build via `scripts/build.sh`, one gate
iterated via `--selftest --gate <name>`, full run before landing.

- **Phase 0 — determinism spike (kill criterion).** Standalone fixed-point
  MLS-MPM, twice-run hash equality, then cross-adapter equality. New gate
  `fluid_det` (needsRender=false). No engine coupling. *Go/no-go.*
- **Phase 1 — solver core.** Particle pool, sparse blocks, sort/bin, P2G /
  grid / G2P kernels, pass_table rows, terrain BCs from the voxel buffer.
  Debug-render as points. Gate: dam-break in a closed box conserves mass and
  hashes deterministically.
- **Phase 2 — the seam.** Excite + settle converters, hydrostatic seeding,
  mass-exact quantization, progressive wake, hysteresis. Gates: pour → still
  → zero active blocks (sleep gate); carve-under-lake drains correctly;
  mass conserved end-to-end.
- **Phase 3 — delete the CA liquid movement rules.** Migrate `class:liquid`
  semantics; excited-path replaces flow everywhere; density displacement vs
  settled water retained. Baseline any intentionally-changed gates
  (`tests/baseline.json` discipline).
- **Phase 4 — world integration.** Occupancy field into the CA + CPU mirror
  (swimming), reactions consume from bins, phase transitions via the seam,
  ballistic-particle drag, MutationQueue sources (faucets/emitters as ops).
- **Phase 5 — rigid coupling.** CPIC velocity projection + impulse readback
  to Jolt; buoyancy gate.
- **Phase 6 — rendering + audio.** Occupancy-volume path first; evaluate
  screen-space splatting; `packOcc` correctness; flowing-water sound cues
  through the standard `Cues` surface.
- **Phase 7 — content + tuning.** Water/lava/oil/blood fluid blocks, tuner
  schema + wiki, `DESIGN.md` rewrite of §4-liquids/§5, this plan marked
  historical.

---

## 10. Open research areas (explicitly unresolved)

- **O-1. Cross-vendor fixed-point GPU determinism** — the gate; entirely
  unproven in the wild. Includes fixed-point format/overflow design. Phase 0.
- **O-2. Deterministic GPU sort/compaction** — stable binning and freelist
  behavior that is scheduling-independent. Phase 0.
- **O-3. Fixed-point precision vs fluid quality** — how much quantization the
  C matrix and EOS tolerate before the water looks wrong (QuanTaichi
  suggests substantial tolerance; verify ourselves).
- **O-4. Hydrostatic excite seeding** — J-vs-depth initialization; free-
  surface finding within a block; avoiding the jello pop.
- **O-5. Mass-exact settle quantization** — remainder-carrying scheme into
  eighths; also the excited-consumption fractional accounting (6.2).
- **O-6. Progressive wake fidelity** — how badly freeze/thaw boundaries damp
  long-range waves; threshold tuning; worst-case "whole ocean wakes" bound.
- **O-7. Excite/settle hysteresis** — anti-thrash margins; interaction with
  the particle budget under sustained faucets.
- **O-8. Rigid CDF representation** — full CPIC triangle splatting vs coarse
  voxelized body occupancy; thin-shell bodies (sheet-metal debris) may need
  true CPIC compatibility bits.
- **O-9. World-hash treatment of particle state** — force-settle on hash
  ticks vs canonical cell-sorted digest. Force-settle perturbs the thing
  being verified; digest costs a sort (which we run anyway). Lean digest.
- **O-10. Rendering** — the largest visual unknown (6.7). Also: does excited
  water cast/receive shadows via occupancy correctly; underwater camera.
- **O-11. Performance envelope** — substeps × particles per tick budget on
  RTX 3060 Ti at dx=0.10 m; stiffness (compressibility look) vs CFL substep
  count tradeoff; where the budget ceiling lands (500k? 2M?). Note dx=0.10 m
  is 40× the paper's demo dx — our CFL substep counts at a given stiffness
  are far more favorable than the paper's numbers suggest.
- **O-12. Multi-material excited fluid** — oil-on-water stratification is
  native to multi-material MPM (densities differ); lava needs viscosity +
  temperature-driven settle-to-stone via the seam; check bin/occupancy
  fields per-material don't blow the bind-group/storage budget.
- **O-13. Streaming/eviction under live particles** — window edge crossing
  mid-splash: force-settle the evicting block (accept momentum loss) and
  RLE as usual; excited blocks near the edge may pin eviction briefly.
- **O-14. The 3-bit tick stamp and bits 19..23** — the fluid system must not
  quietly appropriate voxel-word bits; anything stored there is scratch
  unless hash mask + `kPersistMask` are widened (save-format + determinism
  change; CLAUDE.md).

---

## 11. Alternatives considered (surveyed 2026-08-22, rejected with reasons)

Judged against the four hard requirements: bit-determinism (rule 1), cost
scales with activity not volume (rule 2), fully-3D flow (caves/tunnels/
overhangs — no heightfield assumption), and memory at streamed-world scale.

- **FLIP/APIC with pressure projection** (NVIDIA *Cataclysm* UE4 demo, ~2M
  particles real-time; Wu et al. 2018 sparse-volume GPU FLIP). Best-in-class
  incompressible water look. Rejected: the global pressure solve is an
  iterative float solver — cost scales with total water volume (not
  activity), and iteration-order/convergence behavior is hostile to
  cross-vendor bit-determinism. MLS-MPM's weakly-compressible EOS trades a
  little incompressibility for locality, which is exactly the trade our
  rules force.
- **Lattice-Boltzmann + volume-of-fluid free surface** (FluidX3D; GPU Gems 2
  ch. 47). The serious runner-up. Grid-native (terrain coupling trivial),
  and — uniquely — a pure *gather* stencil: no P2G scatter atomics, so
  determinism is structurally easier, and integer/fixed-point LBM is
  established. Rejected on rule 2 + memory: D3Q19 stores ~19 distribution
  values per fluid cell (~150 B/cell double-buffered) for *every* cell of
  water including still lakes, cost scales with fluid volume, free-surface
  interface tracking (VOF/PLIC mass exchange) is the fiddly part, and
  splashes/droplets need a supplemental particle system anyway — at which
  point MPM does both jobs with one mechanism. Revisit only if Phase 0
  fails: a fixed-point LBM with the same excite/settle seam would be the
  fallback plan, since it dodges the scatter-determinism problem entirely.
- **Position-based fluids / PBD** (Macklin & Müller; NVIDIA FleX; Gustafsson's
  *Sprinkle* and his Teardown-era fluid prototypes). Unconditionally stable
  at big timesteps (no CFL substepping). Rejected: neighbor-list constraint
  solving is order-sensitive (determinism), incompressibility needs solver
  iterations, and per-particle neighbor search on GPU is the expensive part
  MPM's grid replaces for free. Good for small effects volumes, not
  world-scale water.
- **SPH proper** (WCSPH/DFSPH). Same neighbor-search and determinism issues
  as PBD plus stiffness/timestep pain; no voxel-world precedent at scale.
- **Heightfield methods: virtual pipes / shallow water / tall-cell grids**
  (*From Dust*; *Cities: Skylines*; *Timberborn* — whose Update 6 "3D water"
  extends the pipes model to layered vertical flow; Chentanez & Müller 2011
  restricted tall-cell grid, the basis of Cataclysm's deep water). Cheap,
  shipped, deterministic-friendly (structured flux exchange ≈ CA with
  momentum). Rejected as the *primary* sim: column/heightfield assumptions
  break exactly where our game lives (caves, tunnels, sealed containers,
  vertical worlds). Worth stealing from later: a tall-cell/column summary is
  a candidate *far-field* or deep-ocean representation if we ever need open
  seas (render-only or coarse-sim, DESIGN.md far-field precedent).
- **Advanced CA with mass/pressure/momentum** (Dwarf Fortress 7-level +
  pressure, Terraria compression, Oxygen Not Included's per-tile mass +
  pressure, Powder Toy's coarse Eulerian pressure field coupled to CA,
  Noita). This is the family we are already in — our fullness CA is the 3D
  state of its art. The ceiling of the family is the reason for this plan:
  no representation of momentum per se, so no waves, inertia, or splash
  dynamics at any tuning.
- **Watch item:** Keen's VRAGE3 "volumetric water" for Space Engineers 2 —
  the highest-profile voxel-world water in development; implementation
  details unpublished as of 2026-08. Check their dev blogs before Phase 1
  in case their approach surfaces useful data points.

## 12. Prior art pointers for the implementer

- Paper PDF + supplement: https://yzhu.io/publication/mpmmls2018siggraph/paper.pdf
- 88-line MLS-MPM reference (`mls-mpm88.cpp`, MIT): https://github.com/yuanming-hu/taichi_mpm
  — the correct starting point for the Phase 0 kernel; port to fixed point.
- QuanTaichi (Hu et al. 2021) — quantized/fixed-point MPM grids; precedent
  for O-1/O-3.
- Tampubolon et al. 2017 — the weakly compressible water EOS the paper's
  demos use.
- Grant Kot (twitter.com/kotsoft) — real-time MLS-MPM water reference points.
- John Lin's descriptions (paraphrased above): sparse + inline with worldgen,
  99% multithreaded w/ minimal critical section, ≤8 ms on 4 CPU threads,
  save/load + no-pour-into-unloaded implemented, open problems he names:
  sound, rendering, supplemental particles, and no evaporative cycle (his
  finite-water/faucet problem — which our settle-to-voxel design answers).
