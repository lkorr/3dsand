# RESEARCH: water architecture — the bulk-transport question

**Status: research + design conversation, 2026-08-24. Nothing implemented, nothing
decided. Written as a handoff for further consultation on an architectural
decision that is larger than any single work package.** Read CLAUDE.md (the three
inviolable rules) and DESIGN.md §4–§5 first. Every file:line and commit reference
is against the 2026-08-24 working tree — re-verify before acting.

This document supersedes nothing. `docs/PLAN_mpm_fluids.md` remains the
architecture of record and `docs/PLAN_fluid_overhaul.md` the active work queue;
**the open question below is whether that plan's final step (WP5) should happen at
all.**

---

## 1. What prompted this

Two questions from the owner, in sequence:

1. Could shallow-water equations (SWE) carry general water, with MLS-MPM taking
   over at ledges, waterfalls and other places the heightfield fails? Is that
   hybrid too difficult? And more broadly — what is the optimal architecture for
   water that is performant, interacts with a falling-sand voxel world, and can
   reach **massive volumes**?
2. Then, after working through it: **should we actually delete the CA liquid
   movement rules at all?** The CA looks acceptable for a still lake with a plug
   pulled, it is extremely performant, and MPM may never be performant at that
   scale.

Question 2 emerged from question 1 and is the one that matters. It is currently
**open**.

## 2. The constraint envelope (measured, not estimated)

Everything downstream depends on these. Sources are committed bench JSONs and
`src/sim/world.h`.

| Fact | Value | Source |
|---|---|---|
| Voxel size | 0.10 m | `kVoxelMeters` |
| Sim window | 512³ ≈ **51.2 m across** | `kWorldN` |
| Particle pool | `kFluidCap` = 262,144 | `world.h:313` |
| Simultaneously-active fluid blocks | `kFluidBlocks` = 256 | `world.h:318` |
| Particles per water voxel | 8 (`fluidRestDensity` 8.0; 2×2×2 seeding) | `tuning_params.def:395` |
| **Solver cost** | **7.9e-5 ms/particle + ~1.3 ms fixed** | `PLAN_fluid_overhaul.md` §9, WP4 item 6 |
| Hard ceiling on water in motion | 262,144 / 8 = **32,768 voxels ≈ a 3.2 m cube** | derived |
| Ceiling at a ≤5 ms solver budget | ~45,000 particles = **~5,600 voxels ≈ an 18³ box** | derived |
| Largest existing lab scene | `hill`, 39,600 particles | §9 tables |
| Render vs solver at `hill` | fluid isosurface march **9.52 ms** vs solver **4.57 ms** | §9, WP4 attribution |
| Thin-film capture gap | ~51% (WP3), attributed to the solver, not the seam | §9, WP3 block item 5 |

**The load-bearing consequence:** a 10 m × 10 m × 2 m lake is 200,000 water voxels
= 1.6M particles ≈ 126 ms/tick. **MPM cannot hold bulk water. Not slowly — at
all.** Any architecture that makes MPM the only mover has an ~18³ box as its
working budget for water in motion.

External calibration that this is the method's ceiling and not a local bug:
matsuoka's WebGPU-Ocean reports ~100k particles on integrated graphics and ~300k
on a decent GPU at **2** substeps/frame (we run 6, and WP3 made that a knob).
Research-grade GPU-MPM at millions of particles is seconds-to-minutes per frame.

## 3. The architecture as built today

Two liquid states, one seam:

- **Settled = voxels.** Material id + fullness in eighths. Free: chunks sleep,
  no dispatch. Hashed, saved, reacts, stains, streams.
- **Excited = MLS-MPM particles.** The only thing with real momentum.
- **The seam:** `excite` (cells → particles) and `settle` (particles → cells),
  in `sim_fluid_seam.wgsl`.

Status: WP1–WP4 of `PLAN_fluid_overhaul.md` landed; **WP3 merged at `cb1b4b9`**;
only **WP5 — "the flip"** remains. WP5 as written would delete the CA liquid
movement rules entirely and set `sim.fluidExciteMode = 1`, making MPM the sole
mover of liquid and moving the pinned world hash `7cfa2420`.

Also true today and relevant: `fluidExciteMode` defaults to 0, so **world water
is still entirely CA**; the MPM only touches water placed with the `mpm` dev tool.
The owner's tuning defaults (commit `987c595`: stiffness 14000, gravity 900,
viscosity 0, cohesion/attracts/damping 0) are aesthetic choices with
do-not-revert markers, and supersede WP2's physically-derived 3600 / 98.1.

## 4. Research survey

### 4.1 The SWE / heightfield family

**The literal proposal exists and ships.** Chentanez, Müller & Kim, *Coupling 3D
Eulerian, Heightfield and Particle Methods for Interactive Simulation of Large
Scale Liquid Phenomena* (SCA 2014; extended TVCG 2015) couples all three: bulk
near the surface in both particles and grid, switching between them during the
sim; water outside the 3D domain solved as SWE on a height field, with explicit
coupling so waves cross the border naturally. Real-time to interactive.
Chentanez & Müller's earlier *Real-time Simulation of Large Bodies of Water with
Small Scale Details* (SCA 2010) already does heightfield↔particle exchange
**specifically for the cases a heightfield cannot represent — breaking waves,
waterfalls, splashes** — converting them to spray/foam particles that exchange
mass and momentum back. So "SWE for general, particles for the hard parts" is not
speculative; it is a known, working architecture.

**Why it is nonetheless a poor fit for the near field here:**

1. **The failure mode is silent incorrectness, not slowness.** A single-layer
   heightfield stores one water height per XZ column. Two vertically stacked
   bodies (a flooded cave above another, water on a roof and on the ground) merge
   into one number — mass teleports or vanishes. Its valid domain is "exactly one
   water span with air above," and in a game where the player digs, invalid
   columns appear *scattered through the middle of a lake* and move every pick
   swing.
2. **Multi-layer spans — the obvious fix — converge on a 3D solver.** Kellomäki
   (one blocking layer), then Dagenais et al., *Real-Time Virtual Pipes…*
   (VRIPHYS 2017) and *Extended virtual pipes…* (Computers & Graphics 2018),
   which add pipes that resolve flow through **fully flooded passages** — the
   thing multi-layer techniques otherwise cannot do. Costs: the neighbour lookup
   becomes a per-pair connectivity *search* (this destroys the flat-texture
   coalesced-read layout that made heightfields fast); variable-length span lists
   per column; constant merge/split topology changes, each needing an exact
   deterministic mass carry; and a fully flooded passage has no free surface, so
   its "height" is physically meaningless. Published result: **10 cm × 10 cm at
   0.5 mm, ≤5 layers.** A lab demo, not a streamed voxel world.
3. **Two seams instead of one.** voxel↔heightfield *and* heightfield↔MPM. The
   single existing seam took four work packages to make behave, and `fluid-react`
   has been red from seam mass accounting. Doubling that audit surface is the real
   cost and it shows up in no performance comparison.
4. **Where the heightfield genuinely wins:** *draining*. A lake emptying through a
   hole must physically move mass through every cell in a 3D solver, but a
   heightfield moves the same mass by updating one number per column — a real
   ~20× advantage. This is the argument for a heightfield at **world scale**
   (oceans, past the 51 m window), where the "no roof" assumption is actually safe
   and it is largely a render problem.

**Reference implementations and their verdicts:**
- lisyarus/webgpu-shallow-water — virtual pipes with outflow scaling, WebGPU,
  256² default, float textures. Clean starting point. Explicitly single-layer:
  no vertical gaps, no vertical flow.
- Stephen Thompson's Shallow Water Demo (Kurganov–Petrova 2007, 3 GPU passes/step)
  — author is **sceptical about SWE for games**: needs high grid resolution before
  it looks right, and produces spiky surface artifacts once depth exceeds the
  shallow assumption. Suggests low-res SWE for large-scale *planning* plus
  graphical techniques for the look.
- Nilles et al., *3D Real-Time Hydraulic Erosion using Multi-Layered Heightmaps*
  (VMV 2024) — multi-layer pipes producing overhangs/arches/some caves, CUDA,
  **~6 ms @ 2048², ~23 ms @ 4096² on an RTX 3070**. Useful cost calibration for a
  world-scale column layer.
- The Reddit thread the owner supplied (r/VoxelGameDev): a developer running a
  grid-based SWE solver **visualised with GPU particles grid-locked to the height
  buffer as "voxels."** Two admissions worth recording: *"the cost is the same, no
  matter how much water you have"*, and *"it's not really suitable for pipes and
  such, there your CA would be much better suited."* He is separately writing an
  MLS-MPM solver for the multi-fluid case. **Note the speed claim is about a 2.5D
  heightfield with a particle *renderer*, not a 3D solver.**

### 4.2 The MPM family

- Hu et al. 2018, *A Moving Least Squares Material Point Method…* (SIGGRAPH) —
  the method we implement. Reference: `github.com/yuanming-hu/taichi_mpm`
  (the 88-line core). Offline in the paper: 16–288 s/frame at 1–8M particles.
- QuanTaichi (Hu et al. 2021) — quantized/fixed-point MPM grids; the precedent
  for our integer determinism bet.
- matsuoka-601/WebGPU-Ocean — the real-time calibration point above. Also
  independently confirms our approach to the P2G scatter: **since `atomicAdd`
  exists only for 32-bit integers, encode in fixed point and scatter as
  integers.** Our Q16.16 `atomicAdd` is the standard trick, not a determinism tax.
- Müller & Chentanez, *A Fluid Pressure Solver Handling Separating Solid Boundary
  Conditions* (SCA 2011) — validates the separate BC with tangential preservation
  that WP2 implemented by hand after diagnosing sticky in-solid BCs as the cause
  of water clinging to slopes.

### 4.3 What is worth stealing regardless of the decision

- **Virtual pipes over Kurganov–Petrova**, if any pipes solver is built: flux from
  hydrostatic head with outflow scaling is robust, integer-friendly, gather-
  formulated (no atomics), and what shipped games use.
- **Dagenais "extended pipes"** — flow through fully flooded passages. On a 3D
  voxel grid this falls out free: every cell is already a node.
- **hfFluid's spray/foam mass exchange** — the template if a far-field ocean ever
  needs breaking waves at a shoreline.
- **Render-only surface displacement.** Most games animate lake surfaces without
  simulating them. Nearly free, no seam, no mass conservation, no determinism
  exposure, because it is not in the sim. Relevant because it decouples "the lake
  looks alive" from "the lake is simulated."

## 5. Options considered

### Option A — WP5 as planned: delete the CA, MPM is the only mover

- **For:** one movement model, one authoring vocabulary; removes the documented
  CA defects by removing the CA; the settled state becomes purely storage.
- **Against:** hard-caps water in motion at ~5,600 voxels at frame budget. Makes
  the **excite burst** the default world behaviour (§6). Removes the only thing
  that currently moves sub-cell films, which WP3 proved MPM cannot push — done
  naively this makes stranded puddles *worse*, which is the complaint that started
  the whole plan.

### Option B — add a new hydrostatic flux tier (3D virtual pipes on voxels)

Sparse per-chunk auxiliary layer holding integer per-face flux for awake water
cells; gather pass, reach ≤1 cell, no atomics, sleeps when flux decays. Pressure-
driven bulk transport that works in caves, U-bends and sealed containers because
the domain is the voxel grid, not columns.

- **For:** answers communicating vessels and the bulk behind a flood front;
  topology-safe; deterministic by construction; one concept, and the same idea
  scales to a coarse world-scale layer.
- **Against, and this is decisive:** **the CA already is this tier.** See §7.

### Option C — heightfield/SWE as the general tier, MPM for the rest

The owner's original proposal, and Chentanez/Müller 2014's actual architecture.
Rejected for the near field on §4.1 grounds (validity domain, span complexity, two
seams). **Retained for the far field** — see Option E.

### Option D — reduce MPM's cost per unit volume

Two variants, and the arithmetic matters:

- **Coarsen the MPM grid to 2× voxel size (0.20 m nodes), keeping 8 particles per
  MPM cell.** 2× *linear* = 2³ = **8× cell volume**, so one particle covers one
  voxel — an 8× reduction. Second multiplier: the same physical speed is half as
  many cells per substep, so substeps may drop at equal CFL. **Costs:** the exact
  `1 particle = 1 eighth of fullness` mapping breaks (a 3/8 voxel cannot excite
  3/8 of a particle), so particles need a **mass field**; and the thin-film
  threshold doubles from 0.10 m to 0.20 m, which makes WP3's known film gap
  **directionally worse, not ambiguously so.**
- **Keep the 1:1 grid, drop particles-per-cell 8 → 4.** 2× reduction. The seam
  stays exact and integer (1 particle = 2 eighths); cell size, BCs, block map and
  render field are all untouched; the film threshold does not move. Roughly
  `fluidRestDensity` 8.0 → 4.0 plus a 4-point seeding pattern. Cost: 4 ppc in 3D
  is sparse (2×2×2 = 8 is the standard lattice), so density noise and cell-
  crossing artifacts increase. **Cheap enough to simply try, and it answers
  whether particle count is even the binding constraint** versus the ~1.3 ms fixed
  term and the render march.

### Option E — a far-field column layer (oceans), promoted by a design requirement

The owner ruled: **no Minecraft-style infinite source blocks; a determined player
must be able to empty an entire lake or ocean; drained water stays drained.**
Weather is an explicitly open boundary (rain and evaporation may add and remove
water) — the rule is "nothing in the world is an endless spring," not a closed
hydrological cycle.

That ruling **promotes the far-field layer from a read-only worldgen input to
mutable, saved, authoritative state**, because far water must be debited when it
drains. Proposed structure — two readings of one store:

- **Primary: water volume per chunk column, stored sparsely as a delta from the
  procedural sea level.** Untouched world stores nothing. Topology-agnostic: a
  flooded cavern with a roof is just "this chunk holds N eighths," the exact case
  a heightfield cannot represent.
- **Derived, for open-sky columns only: a surface height**, used for rendering the
  distant sea and for a world-scale pipes solve. Confining the heightfield
  assumption to roofless columns is what makes it safe.

Seam: chunk load → fill voxels from the store; chunk evict → sum water voxels,
write the volume back. Total water = resident voxels + far-field store.

**Staging, because the two halves have very different risk:**
- **E-a: the store and the two-way load/evict seam.** Low risk. Delivers "drain a
  lake, walk away, come back — still drained," makes the ledger real, and deletes
  the worldgen hack described in §8.
- **E-b: background frontier relaxation** (what actually lowers an *ocean*).
  **The one item here with genuine research risk, and the compute is not the hard
  part.** (i) *Determinism* — a process whose cadence depends on which chunks
  happen to be resident is scheduling-dependent, which rule 1 forbids; the fix is
  to make relaxation a pure function of tick over the whole sparse store
  regardless of residency, which makes the store fully resident, hashed and saved
  — a system on the order of the page table. (ii) *Set size* — at chunk-column
  resolution a 4 km × 4 km ocean is ~6.25M columns, so once a drain frontier
  spreads, "sparse" stops meaning "small." A uniform sea-level drop is a
  low-frequency change, so the likely mechanism is a **multigrid/pyramid
  relaxation** (precedent: the specialised multigrid Poisson solve in Chentanez &
  Müller's tall-cell paper, SIGGRAPH 2011) — an open design, not a known quantity.
  **Ship E-a first and find out whether E-b is needed in practice.**

## 6. The excite burst — a live bug that constrains the decision

Observed by the owner at `fluidExciteMode = 1`: **digging a small hole under a
pond in the main world converts the entire pond to particles on one tick**,
dropping the frame to ~2 fps, then settling back. His words: "it turns the whole
lake into fluid for a sec and back." At `exciteMode = 0` (the CA path) the same
drain is "no biggie."

Diagnosis: excite is a per-cell trigger with no notion of "only wake what the
disturbance can reach," and it emits one particle per eighth of fullness, so a
pond ~8 eighths deep becomes ~8 particles per water voxel instantly. The budget
does refuse over-capacity excitation, so it is bounded — but bounded by
`kFluidCap` ≈ 22 ms of solver, roughly 4× the playable count, which is not a
useful bound. `PLAN_mpm_fluids.md` §7 named this (**"progressive wake"**, open
question O-6) and it was never built; what exists is the always-on wake trigger,
a per-node speed test rather than a bounded frontier.

Two things follow:

- It is a **transient** — settle does catch up — so a per-tick excite conversion
  budget well below `kFluidCap` would let a pond convert over many ticks with
  settle keeping pace, instead of the whole burst landing on one. WP3's
  settle-stability veto already governs the steady state. A true spatial frontier
  (one shell per tick, the dirty-chunk propagation pattern) is the escalation.
- **WP5 makes this the default behaviour of every body of water in the world.** It
  cannot be split out: deleting CA liquid movement without arming the triggers
  leaves settled water unable to respond to digging at all, so the deletion, the
  flip and the bound are one change.

**No existing lab scene can reproduce it.** basin/hill/faucet/pool/slosh are all
small authored boxes, the largest ~40,000 particles. A WP5 measured only in the
lab would report five green scenes and still stutter at every pond in the world.

## 7. Where the reasoning landed — and where it changed

Recorded honestly, because the reversal is the most important content here.

**First conclusion:** keep the two-state design, add a third tier (Option B) for
bulk transport, plus Option E for oceans, plus Option D for headroom. SWE rejected
for the near field.

**The correction, raised by the owner:** *why gut the CA at all?* It looks
acceptable for a still lake with a plug pulled, it is extremely performant, and
his own A/B shows the CA path handles a pond drain that the MPM path cannot.

**This is right, and it collapses Option B.** The CA **is** the bulk-transport
tier Option B proposed to build. What it lacks is not the concept but four
specific defects, all named with line numbers in `PLAN_fluid_overhaul.md` §1.1
(`assets/shaders/sim_step.wgsl`):

- `if (f >= 2u)` (:814) — a fullness-1 cell can never spread laterally into air,
  and lateral spread is repeated halving, so every blob decays into inert films
- `liquidEqualize = 2` (:809) — a neighbour holding one eighth less never
  receives, which makes a staircase down a slope a **stable equilibrium**
- down-diagonal moves are whole-cell, 4 axis directions, no corners (:794)
- the settled path does not `markDirty` (:822), so the chunk sleeps and never
  retries

Those four are the owner's founding complaint ("it clumps and settles on the hill
instead of flowing down"). The plan said *"Decision: do not surgery the CA rules"*
**only because Phase 3 / WP5 was going to delete them.** Drop that assumption and
those four fixes become the cheapest available win in the entire document.

**The resulting shape** (the current recommendation, unratified):

- **CA owns supported water** — bulk in containers, levels equalising, slow
  drains, still lakes. Fix its four defects.
- **MPM owns water with momentum** — falling, poured, splashed, blown up, sheeting
  off a ledge. Narrow the excite predicate toward "this water is falling," which
  trigger (a) ("air below") already approximates.
- **Budget exhaustion becomes graceful**: when the particle pool is full, water
  **stays CA** rather than being refused, turning a hard ceiling into degradation.
- **The excite burst dissolves without a frontier algorithm**: puncture a pond and
  only the water actually falling through the hole excites; the rest drains as CA.
- **WP5's deletion is the part to cut.** Its *narrowing* survives.

**The honest cost of this shape:** the CA has no momentum, so anything routed to
it looks inert — a large lake surface sits like glass. Mitigations: route
*visible, disturbed* water to MPM, and/or render-only surface ripples (§4.3),
which cost nearly nothing and are not in the sim.

**The counter-argument that deserves a hearing before deciding:** two movement
models means two sets of bugs, two authoring vocabularies, and an ownership
predicate that must be airtight or mass double-moves. `PLAN_mpm_fluids.md` chose
one model deliberately. Nothing in this document proves that choice wrong — it
argues the *cost* of it was underestimated.

## 8. Corroborating detail: worldgen is already working around this

`assets/shaders/worldgen.wgsl:307-425`: ponds are **bounded discs deliberately
constrained so they can never spill and can never intersect a tunnel**, because a
spilling pond meant "the world never settles" (175 spill edges found in one
region). Comments in that file record the reasoning. The hack exists precisely
because no tier reliably transports bulk water, and it is the cheapest available
evidence that the gap is real rather than theoretical. It also currently *masks*
§6: ponds are small enough that the excite burst stays latent.

## 9. The open decision, and what evidence would resolve it

**Decide:** does the CA liquid movement stay as the bulk-transport tier (fixed),
or is it deleted in favour of MPM-only (WP5 as planned)?

Cheap experiments that would inform it, roughly in order of value-per-hour:

1. **A `pond` lab scene** sized from what `pondAt` / `pondSurface` actually
   generate, with a plug pulled, wired into `--fluid-bench`, judged on frame
   **p95/p99** (a one-tick burst is invisible in p50). Plus one hand-run
   main-world pond puncture, before and after. Nothing currently measures the
   case that broke.
2. **Fix the four CA defects** and re-run the `hill` scene. Directly tests whether
   the founding complaint is a CA-rules bug or a CA-model limit. Cheapest win in
   the document if it works.
3. **The 4-ppc experiment** (Option D, second variant) — a day's work, and it
   answers whether particle count is the binding constraint at all, versus the
   ~1.3 ms fixed term and the 9.52 ms render march.
4. **A stated maximum drainable body size** — sweep body size until p95 leaves
   budget. Converts a latent surprise into a known limit, and gives any bulk tier
   its trigger condition.

Whatever is decided, two facts should survive the decision: **the render march is
currently the larger half of the fluid frame cost** (9.52 ms vs 4.57 ms at
`hill`), and **no lab scene is currently large enough to reproduce the bug that
matters.**

---

## 10. Sources

- Matthias Müller publication index — https://matthias-research.github.io/pages/publications/publications.html
- Chentanez, Müller, Kim — *Coupling 3D Eulerian, Heightfield and Particle Methods for Interactive Simulation of Large Scale Liquid Phenomena*, SCA 2014 / TVCG 21(10) 2015 — https://matthias-research.github.io/pages/publications/hybridsim_preprinted.pdf
- Chentanez, Müller — *Real-time Simulation of Large Bodies of Water with Small Scale Details*, SCA 2010 — https://matthias-research.github.io/pages/publications/hfFluid.pdf
- Chentanez, Müller — *Real-Time Eulerian Water Simulation Using a Restricted Tall Cell Grid*, SIGGRAPH 2011 — https://matthias-research.github.io/pages/publications/tallCells.pdf
- Müller, Chentanez — *A Fluid Pressure Solver Handling Separating Solid Boundary Conditions*, SCA 2011 — https://matthias-research.github.io/pages/publications/separatingBoundaries.pdf
- Solenthaler, Bucher, Chentanez, Müller, Gross — *SPH Based Shallow Water Simulation*, VRIPhys 2011
- Thuerey, Müller, Schirm, Gross — *Real-time Breaking Waves for Shallow Water Simulations*, Pacific Graphics 2007
- Macklin, Müller — *Position Based Fluids*, SIGGRAPH 2013 (surveyed and rejected in `PLAN_mpm_fluids.md` §11 — order-sensitive constraint solving)
- Dagenais, Guzman, Vervondel, Hay, Delorme, Mould, Paquette — *Real-Time Virtual Pipes Simulation and Modeling for Small-Scale Shallow Water*, VRIPHYS 2017 — https://profs.etsmtl.ca/epaquette/Research/Papers/Dagenais.2018/
- Dagenais et al. — *Extended virtual pipes for the stable and real-time simulation of small-scale shallow water*, Computers & Graphics 2018
- Kellomäki — multi-layer virtual pipes (one blocking layer; the predecessor Dagenais extends)
- Nilles, Günther, Wagner, Müller — *3D Real-Time Hydraulic Erosion Simulation using Multi-Layered Heightmaps*, VMV 2024
- Hu, Fang, Ge, Qu, Zhu, Pradhana, Jiang — *A Moving Least Squares Material Point Method with Displacement Discontinuity and Two-Way Rigid Body Coupling*, SIGGRAPH 2018 — https://yzhu.io/publication/mpmmls2018siggraph/paper.pdf ; reference code https://github.com/yuanming-hu/taichi_mpm
- Hu et al. — *QuanTaichi*, SIGGRAPH 2021 (quantized/fixed-point MPM grids)
- Tampubolon et al. 2017 — the weakly compressible water EOS used by the MLS-MPM paper's demos
- matsuoka-601/WebGPU-Ocean — https://github.com/matsuoka-601/webgpu-ocean
- lisyarus/webgpu-shallow-water — https://github.com/lisyarus/webgpu-shallow-water
- Stephen Thompson, Shallow Water Demo (Kurganov–Petrova 2007) — https://www.solarflare.org.uk/shallow_water
- Gao et al. — *GPU Optimization of Material Point Methods*, and the SIGGRAPH 2020 multi-GPU MPM work (offline scale calibration)
- r/VoxelGameDev thread supplied by the owner (SWE solver + grid-locked GPU particles as voxel renderer)

### In-repo references
- `docs/PLAN_mpm_fluids.md` — architecture of record; §7 (the seam, progressive wake), §10 (open questions O-1..O-14), §11 (alternatives surveyed and rejected, incl. LBM, PBD/FleX, heightfield methods, advanced CA)
- `docs/PLAN_fluid_overhaul.md` — active work queue; §1.1 (the four CA defects), §1.2 (goo diagnosis), §9 (all measured results)
- `docs/bench/wp1_*.json`, `wp2_fluid_bench_after.json`, `wp4_fluid_bench_{baseline,after}.json`
- Commits: `987c595` (owner's look defaults), `b219f43` (WP2+WP4), `cb1b4b9` (WP3)
