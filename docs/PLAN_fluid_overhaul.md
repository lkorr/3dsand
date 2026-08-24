# PLAN: fluid overhaul — flow, look, perf, and the fluid lab

**Status: work queue, written 2026-08-24 from a research session (codebase audit +
reference-implementation study). This document extends `PLAN_mpm_fluids.md`,
which remains the architecture of record for the hybrid settled/excited design —
nothing here contradicts it; this is the concrete path through its Phases 3–7,
reordered around what the user actually reported.** Read CLAUDE.md (the three
rules) and DESIGN.md §5 before any WP. File:line refs are the 2026-08-24 working
tree; re-verify before editing.

The user's report: *"no matter what settings I tune it looks very viscous and
goopy and doesn't flow like water. It clumps, and on a hill it'll clump and
settle on the hill instead of flowing down. It's also massively straining frame
time and the CPU."*

---

## 1. Diagnosis

### 1.1 What the user is observing: MPM via the dev tool, ending in the settle trapdoor

**Correction 2026-08-24 (user clarified): the reported water is placed directly
with the `mpm` dev-panel tool — so the observed motion IS the MLS-MPM solver,
and §1.2 is the primary diagnosis.** `sim.fluidExciteMode = 0`
(`tuning_params.def:439`) still matters, but differently than a first read
suggests: it means world water never *enters* the MPM — and, the trapdoor, MPM
water that settles never comes back.

The clump-on-the-hill endpoint is a three-stage pipeline:

1. **The solver bleeds downhill velocity** (sticky in-solid BC + the
   CFL-hiding `FLUID_VMAX` clamp — §1.2 items 1–2), so tool-poured water on a
   slope decays toward rest instead of sheeting down.
2. **Settle converts it mid-slope.** Once a 16³ chunk's max particle speed
   stays under ~0.82 vox/s for 45 ticks (1.5 s), the whole block becomes CA
   voxels right where it stalled (`sim_fluid_seam.wgsl:685-798`). At some
   tunings it never quite crosses the threshold and instead churns in place
   as a mushy heap (the "sealed pool churns forever" note,
   `selftest_sim.cpp:978-999`) — either way it parks on the hill.
3. **Settled output is permanently stuck**, held by the CA in the
   clump-on-a-hill equilibrium — three specific lines in `sim_step.wgsl`
   (`stepLiquid`, :770–823):
   - **`if (f >= 2u)` (:814)** — a fullness-1 cell can never spread laterally
     into air, and lateral spread is repeated halving (8→4→2→1), so every
     blob decays into inert films. Documented as intended (DESIGN.md:2985 —
     films sleep).
   - **`liquidEqualize = 2` (:809, `tuning_params.def:378`)** — a neighbor
     holding one eighth less never receives. A staircase of 1-eighth steps
     down a slope is a **stable equilibrium**. (equalize=1 is not a fix by
     itself — `transferLiquid(..., (f-nf)/2u)` transfers 0 when the diff
     is 1.)
   - **Down-diagonal moves are whole-cell only, 4 axis directions, no
     corners (:794)** — on a slope the diagonal target is usually terrain.
   The settled path also does not `markDirty` (:822), so the chunk sleeps and
   never retries. And excite cannot rescue it: trigger (a) requires air
   *directly below* — false on a slope — and is gated off by `exciteMode=0`
   anyway; trigger (b) needs ≥3.5 vox/s of neighboring *particle* motion
   (`sim_fluid_seam.wgsl:336-374`). One-way trapdoor.

The CA itself cannot be tuned into water; it has no momentum — that is the
whole reason `PLAN_mpm_fluids.md` exists. **Decision: do not surgery the CA
rules.** Phase 3 of the plan of record deletes CA liquid movement entirely;
interim CA patches are wasted motion and risk the sleep guarantees. The
trapdoor is closed by WP2 (stop bleeding the velocity), WP3 (refuse to settle
flow-unstable configurations), and WP5 (the CA stops owning any liquid).

### 1.2 Why the MPM reads as goopy — the primary diagnosis (ranked, with evidence)

The solver (`sim_fluid.wgsl`, MLS-MPM, integer Q16.16, two-pass P2G in the
grantkot/matsuoka shape, APIC — v overwritten from grid each substep) is
structurally sound. The goo lives in parameters and boundary conditions:

1. **CFL is violated and hidden by clamps.** Live `fluidStiffness` is 11500
   (vox/s)² (tuning.json; the .def says 5400) → sound speed c≈107 vox/s →
   0.60 cells/substep at 6 substeps, over the `FLUID_VMAX` clamp of 0.45. The
   clamp silently converts pressure work into energy loss every substep —
   which is *exactly* the "mushy under agitation" failure mode (reference
   checklist §2). The `fluid-excite` gate comment already says it: "stock
   stiffness sits at the CFL edge and a sealed pool churns forever"
   (`selftest_sim.cpp:978-999`). Live gravity is also 181.9 vox/s² — 2× real
   (98.1 = 9.81 m/s² at 0.1 m voxels) — which worsens CFL further.
2. **Sticky boundary inside solids.** `gridUpdate` zeroes ALL velocity
   components of any node whose cell is solid (`sim_fluid.wgsl:560-627`);
   only face-adjacent nodes get the separate (normal-only) treatment. A
   particle sliding down a slope has in-solid nodes inside its 3³ support, so
   it loses tangential velocity every substep → water piles on inclines
   instead of sheeting down. This is reference-checklist cause #2 verbatim.
3. **EOS tension terms.** `pr = max(pr, -FLUID_COHESION)` plus
   `attractSame`/`attractDiff` allow negative pressure. At live values the
   floor is small (−32.9 (vox/s)²) but the .def defaults (cohesion 90,
   attractSame 45) are the classic sticky-ropes configuration. The
   best-looking real-time references clamp at exactly 0 (matsuoka) or −0.1
   (nialltl). Default should be 0.
4. **PIC+APIC at a 10 cm grid is inherently smooth** — acceptable, but any
   additional damping compounds it. Audit: the J-relax (`p.j += (1-p.j)/64`),
   the ±0.5 hydrostatic blend clamp, `fluidDamping` (live 0 — keep), and
   viscosity via `(C+Cᵀ)` (live 0.5 vox²/s — references use ~0.02–0.1 grid
   units; start lower).
5. **Settle freezes slow flow mid-slope.** Per-chunk `atomicMax` speed < ~0.82
   vox/s for 45 ticks (1.5 s) settles the whole 16³ block
   (`sim_fluid_seam.wgsl:685-798`). Gravity-driven sheet flow on a gentle
   slope lives in exactly that band. Once settled it is CA voxels and
   **nothing can re-excite it**: excite trigger (a) needs air directly below
   (`:341-345`) — false on a slope — and trigger (b) needs ≥3.5 vox/s at an
   existing particle interface. One-way trapdoor onto the hillside.

Additional traps found: `SEAM_SETTLE8` is truncated `>>8` so the settle-eps
slider has ~0.117 vox/s granularity (non-monotone tuner behavior near the
interesting range); a refused settle block resets its calm counter to 0
(`:1015-1018`) so awkward pools re-run the 45-tick window forever (cost +
thrash); and the *live tuning.json disagrees with the .def defaults* on 8 knobs
(stiffness, gravity, eosPower, cohesion, attracts, viscosity, splash), so
nobody currently knows which configuration anyone has been judging.

### 1.3 Why it strains frame time

Measured evidence is thin (no recorded fluid ms in-repo — fixing that is WP1),
but the structural exposure is unambiguous:

- **The solver never sleeps.** CPU-side `fluidCount` is a monotone estimate
  (only regen/load resets it, `main.cpp:1603`) — one pour pays
  `kFluidSubsteps`(6) × the full substep table + seam + settle **every tick
  forever** (`simulation.cpp:1122-1130`). Known, deliberately deferred; the
  deferral ends here.
- **Fixed-size dispatches ignore the live count**: `seam_compact_count` and
  `seam_compact_scatter` walk all 262,144 pool slots, `seam_consume_apply`
  the same (`pass_table.def:505,511,520`), `seam_settle_judge` all 32,768
  chunk slots (:572) — regardless of whether 100 particles exist.
- **Whole-buffer clears every tick**: 8 MiB `fluidCellScratch` (:554) +
  ~1.2 MiB of seam scratches + the 128 KiB block map ×6 substeps.
- **`fluidGridUp` does 7 `voxWordAt` probes per node** (`sim_fluid.wgsl:
  608-619`) × blocks×4096 nodes × 6 substeps — up to ~44 M voxel reads/tick,
  re-reading terrain that cannot change between substeps.
- **Rendering**: any `fluidCount > 0` adds a per-ray block-map march for the
  whole screen, even pixels facing away; a fluid pixel costs 48 buffer reads
  for the normal alone (`raymarch.wgsl:4430-4442`), a 320-step march, a
  28-step thickness walk, and two traced secondary rays.
- 105 MiB of fluid buffers are permanently resident; `FluidParticle` is 128 B
  of which **12 words are zeroed reserves** (`common.wgsl:773-807`), and the
  compaction ping-pong drags all of it through memory every tick.

### 1.4 Why "no matter what I tune" — the regime is dead and the loop is broken

Two compounding reasons:

1. **The regime.** The live values (stiffness 11500, gravity 181.9) sit past
   the CFL ceiling, where the `FLUID_VMAX` clamp dominates the dynamics. In
   that regime most sliders genuinely stop changing the look — extra
   stiffness is simply eaten by the clamp as energy loss. The user's tuning
   attempts were honest; the parameter space they were exploring was almost
   entirely inside the clamp-saturated dead zone.
2. **The loop.** The in-app ImGui sliders do apply live (writeback +
   `ReloadShaders`, `main.cpp:3335-3340`), but tuner.html edits never reach a
   running app without a manual F5 (every `sim.fluid*` knob is a WGSL
   compile-time constant), and the only place to observe fluid is the default
   world: trees, grass sway, mobs, streaming, celestial — all confounding
   both the look and every measurement (the board records 12↔490 ms on
   identical shaders under GPU contention). There is no flat test world, no
   scripted scene, no per-pass fluid timing. Tuning rounds that should take
   seconds take minutes and produce unattributable results.

**This is why WP1 (the fluid lab) is first and blocks everything else.**

---

## 2. Reference findings (what we steal, what we don't)

Full reports in the session that produced this doc; the load-bearing facts:

**capslpop/Atomic-Fluid-Simulation** (C++/Vulkan/Slang, 640k particles, 128³
grid, 3 dispatches/frame): per-particle state is **one float4 (position only,
16 B)**. Velocity and the APIC C matrix are *re-gathered from the momentum grid
every frame* ("gather twice": gather v+C at the pre-advection position, advect,
gather density at the post-advection position — the density-splat pass exists
only to make gather #2 possible). EOS is linear `p = 4·(ρ−1)`, no substeps
(displacement clamped to 1 cell instead — the mushy shortcut we should NOT
copy), walls are velocity overrides. Rendering is nothing clever: oversized
depth-scaled point sprites. **Steal: the storage model** (our particles already
take v from the grid each substep — pure PIC+APIC — so dropping stored v/C/
density/J loses nothing algorithmically and cuts 128 B → ~32–48 B, keeping
attr/species/birthTick which the seam's mass accounting needs). **Don't steal:
the CFL clamp or the float atomics** (our fixed-point i32 atomicAdd is already
the deterministic, WGSL-legal equivalent — same pattern as matsuoka's
WebGPU-Ocean).

**John Lin's CPU fluid**: the repo was never public (GitHub README search for
his video-description phrases returns zero; his known account has no fluid
repo). Confirmed from his own citation: it is MLS-MPM (Hu et al. 2018),
single-core + AVX2, GPU rendering. No particle counts or ms/frame were ever
published. Treat it as an existence proof — *the method scaled down runs in
real time on almost nothing* — not as a source of implementation detail. Do not
spend more time chasing it.

**Grant Kot / nialltl / matsuoka (the goopy-water checklist, ranked)**:
1. Unclamped negative EOS pressure → sticky ropes, water clings to inclines.
   Fix: `p = max(0, k((ρ/ρ₀)^γ − 1))`; at most a −0.1-ish floor once
   everything else flows (nialltl: "a little" tension gives nice filaments).
2. Sticky solid BCs → slopes hold water. Fix: separate-type BC (remove only
   the into-solid normal component, keep tangential), plus nialltl/matsuoka's
   *predictive* particle-level wall spring (`x_n = x + v·dt·3; v += 0.3·(wall
   − x_n)`) instead of hard clamps.
3. rest_density ≠ seeded particles-per-cell → self-compacting blobs that sit
   on slopes. Ours is consistent (8.0 rest, 2×2×2 seeding) — keep pinned,
   re-derive if seeding changes.
4. Too-soft EOS + CFL hidden by clamps → dense oozing heap. Fix: stiffness
   3–10 grid units, γ 4–5, substeps sized so max displacement < ~0.5
   cell/substep, remove the clamps to see the truth.
5. Transfer dissipation (PIC/APIC at coarse grids) → dead water. Verify
   `C = 4·B` exactly (ours is — the `<< 2u` in g2p), consider a FLIP-blend
   knob (~0.95) only if APIC still reads dead after 1–4 are fixed (note:
   FLIP requires keeping per-particle v, which conflicts with the capslpop
   slimming — decide in the lab, not in the abstract).
6. Explicit damping an order too high = honey. Water wants μ ≈ 0.02–0.1 grid
   units, zero extra damping.
7. Noisy density → clumping/popping. Our p2g2 already re-gathers density
   fresh per substep (correct); Clavet-style near-pressure position
   correction is the escalation if clumping persists.

---

## 3. Direction

Keep the architecture (hybrid settled-voxel / excited-MPM, integer solver,
GPU-owned counts, the seam). The work is: **(WP1)** build the place where fluid
can be seen, tuned, and measured; **(WP2)** fix the four physics causes of goo;
**(WP3)** fix the seam so slopes excite and slow flow doesn't freeze mid-hill;
**(WP4)** make cost proportional to live particles and make sleep real;
**(WP5)** flip the default (Phase 3 of the plan of record) and rebaseline.

Order: WP1 strictly first. WP2 and WP4 can run in parallel worktrees after it
(different files, coordinate via board). WP3 after WP2 (settle criteria depend
on the new flow behavior). WP5 last, gated on user sign-off in the lab.

---

## 4. WP1 — the fluid lab (build FIRST; everything else is blocked on it)

**Why this exists** (user requirement, verbatim intent): a dedicated test world
that is just a simple box — no trees, foliage, mobs, POIs — so fluid can be
placed and observed; params tunable on the fly with immediate effect; used for
all benchmarking because the frame then contains mostly fluid work, not
everything else. Without it, every look judgment and every perf number in
WP2–WP5 is confounded (see §1.4).

**Deliverables:**

1. **`--lab [scene]` world mode.** A worldgen mode flag that generates a flat
   stone slab (say, solid below y=128, air above, no caves/trees/ponds/POIs/
   grass), fed through the normal worldgen path (`worldgen.wgsl` mode uniform
   — do NOT fork genChunk; guard the feature taps). Mob spawning off. Player
   spawns at a fixed pose per scene, fly enabled. The default-world pinned
   hash 7cfa2420 must be untouched — lab is a different worldgen input, and
   `--selftest` never runs in lab mode.
2. **Scripted scenes**, built as CellOp lists on top of the slab (reuse the
   selftest plumbing — `support.cpp` SubmitWorldgen + gate-style CellOps, see
   `GateFluidExcite` `selftest_sim.cpp:1008-1052` for the pattern):
   - `basin` — dam break into a walled box (baseline look + timing);
   - `hill` — **the money scene**: a ~30° stepped stone ramp, pour at the
     top, catch basin at the bottom. Pass = water sheets down and arrives;
     fail = mid-slope freeze (this is the user's exact complaint);
   - `faucet` — sustained pour (budget pressure, settle churn, monotone-count
     behavior);
   - `pool` — pour then still (the sleep scene: after settle, fluid GPU cost
     must reach zero);
   - `slosh` — a channel wave (inertia/liveliness A/B).
   Scenes must be deterministic: fixed seed, fixed tick schedule, `hash3`
   discipline for any jitter. A **reset key** re-runs the scene's CellOps
   from the post-worldgen snapshot without regenerating the world, so A/B
   comparisons replay the identical pour. Scenes pour through `FluidSpawnOp`
   — the same path as the `mpm` dev tool, which is how the user has been
   placing fluid. The lab defaults `sim.fluidExciteMode = 1` (the only
   live CPU-read fluid knob) so the full excite/settle loop is always
   exercised; `hill` also gets an A/B run at exciteMode 0 to reproduce the
   reported mid-slope trapdoor until WP3 closes it.
3. **Live tuning loop.** Poll `assets/materials/tuning.json` mtime (~4 Hz);
   on change, run the existing F5 path (`SetCurrentTuning` +
   `sim.ReloadShaders`, `main.cpp:1885-1925`). The tuner.html Play/save
   round-trip then updates a running lab within a second — the sim.fluid*
   consts recompile is the F5 path working as designed. The in-app ImGui
   sliders already write back + reload (`main.cpp:3335-3340`); keep both but
   beware the documented gotcha (running app clobbers tuning.json): while the
   file watcher is on, the app must not write tuning.json except on explicit
   ImGui edit, and must not fight the tuner (last-writer-wins with an mtime
   check, or disable app writeback in lab mode).
4. **`--fluid-bench [scene]`**: headless-ish scripted run (fixed camera, N
   ticks) that wires up `passtimer.h` (labels already exist:
   `fluid(substep)`, `fluidSeam`, `fluidSettle`, plus render) and emits JSON:
   per-pass GPU ms (avg/p95), whole-frame p50/p95/p99, live-particle curve,
   active-block curve, tick-of-settle, and the mass ledger (eighths in ==
   out, from the existing FA_* counters). Runs under `scripts/run.sh` like
   everything else. **First action after building it: record the baseline
   numbers for all five scenes at both the live tuning.json and the .def
   defaults, and commit them to the plan doc.** Every WP2–WP4 change quotes
   before/after from this harness.
5. **Cleanup while in there:** reconcile tuning.json vs tuning_params.def
   defaults (pick one truth — the .def; regenerate tuning.json from it) so
   the lab starts from a known configuration.

Files: `src/main.cpp` (args, watcher, scenes), `assets/shaders/worldgen.wgsl`
(mode taps), `src/sim/` (worldgen params plumb), `src/test/support.*` (reused,
not changed if possible), `src/gpu/passtimer.h` wiring. Board-claim main.cpp
and worldgen.wgsl. No sim-kernel changes in this WP. Gates: full `--selftest`
must stay green (lab is additive); determinism 7cfa2420 untouched.

---

## 5. WP2 — make it flow like water (solver look fixes)

All changes inside `sim_fluid.wgsl` (+ tuning rows). The fluid solver's own
determinism gate is `fluid-det` (twice-run equality — solver changes are
allowed and expected to change its hash; it is not the pinned world hash).
Iterate every change in the lab against `hill` + `basin` + `slosh`.

Ranked, per the diagnosis §1.2 and reference checklist §2:

1. **Restore CFL honesty.** New starting defaults: gravity 98.1 vox/s²,
   stiffness ≈3600 (c≈60 vox/s → 0.33 cell/substep at 6 substeps), eosPower
   4. Treat `FLUID_VMAX` as a safety net that should almost never engage —
   consider a debug counter (lab HUD) for clamp engagements/tick; if it fires
   in steady flow, stiffness or substeps are wrong. Do not "fix" explosions
   with damping; fix them with substeps/stiffness.
2. **Separate BC with tangential preservation.** Rework `gridUpdate`'s solid
   handling: for nodes in/adjacent to solids, remove only the velocity
   component into the surface (per-axis face tests generalized), keep
   tangential (optionally × (1−friction), material-JSON later per plan §6.1).
   Kill the unconditional `v=0` inside solids for nodes reachable by fluid
   support (deep-interior nodes get no mass anyway). Add the predictive
   particle-level wall spring from nialltl/matsuoka in g2p if face BCs alone
   still let particles crust against walls. **Acceptance: `hill` scene sheets
   to the bottom.**
3. **Zero-tension default.** cohesion → 0, attractSame → 0, attractDiff → 0
   in the shipped defaults; keep the knobs (they are the honey/goo authoring
   surface for other liquids — that's a feature, just not water's default).
   Re-audit the EOS floor semantics so cohesion=0 means `p = max(0, ·)`.
4. **Damping audit.** viscosity default 0.5 → ~0.1 vox²/s pending lab A/B;
   damping stays 0; re-examine the J-relax (`/64`) and the hydrostatic blend
   clamp (both were churn fixes — re-test them under the new stiffness; the
   seam memory warns the blend must stay symmetric).
5. **Optional (only if still dead after 1–4):** a `fluidFlip` blend knob
   (0=APIC now, ~0.95 FLIP) — but note this requires keeping per-particle v,
   which forecloses the WP4 slimming option; decide with lab evidence, not
   preference. Alternative liveliness lever with no storage cost: run the
   fluid grid at 2× resolution in active blocks (large change; evaluate
   last).

Also fix here (small, same files): `SEAM_SETTLE8` quantization (compute the
squared threshold at ≥Q12.4 effective precision — shift 4, not 8), since WP3
tuning needs a monotone slider.

Verification per change: `--fluid-bench` before/after + `--selftest --gate
fluid-det` while iterating; full suite once at WP end (budget rules,
CLAUDE.md). fluid-excite's tuning overrides (`selftest_sim.cpp:978-999`)
should shrink or disappear as stock values become stable — treat that as a
success signal.

---

## 6. WP3 — the seam: slopes must excite, slow flow must not freeze

1. **New excite triggers** (in `seam_excite_detect`, `sim_fluid_seam.wgsl:
   336-374`), for settled liquid cells, all gated by `fluidExciteEnable`:
   - (a) air directly below — existing;
   - (b) **diagonal fall**: a lateral air neighbor whose own below-cell is
     air (the cell could fall diagonally — catches steep-slope water);
   - (c) **lateral pressure gradient**: a lateral same-liquid neighbor with
     fullness ≥2 lower, or lateral air with own fullness ≥2 (catches the
     staircase clumps the CA locks into).
   Budget-charge before emission as today; refusal leaves water settled.
2. **Hysteresis by construction:** `settleCheck` refuses to settle a column
   whose resulting cells would immediately satisfy any excite trigger — the
   settled configuration must be excite-stable. Critical detail: this
   stability test evaluates the *geometric* excite conditions regardless of
   the `exciteMode` gate — so it also protects `mpm`-tool water at
   exciteMode 0, which makes it the immediate fix for the user-reported
   mid-slope freeze (the new triggers in item 1 stay gated for hash safety;
   this test does not). It replaces threshold hand-tuning as the primary
   anti-thrash mechanism (plan-of-record O-7). With it, slow sheet flow on a
   slope simply never settles until it reaches flat ground — correct, and
   bounded because it arrives.
3. **Refusal must not reset calm** (`:1015-1018`): keep (or halve) the calm
   counter on `MARK_REFUSED` instead of zeroing, so geometrically-awkward
   pools don't re-run the full window forever.
4. Re-tune `settleEps` / `wakeSpeed` / `settleTicks` in the lab `pool` and
   `hill` scenes with the fixed quantization. Wake (trigger b of the old
   code) likely needs to drop below 3.5 vox/s once slopes are live.
5. Keep the mass ledger exact — `fluid-settle` / `fluid-excite` / `fluid-det`
   are the audits; sealed-box test discipline per the seam memory (2-thick
   shells, dim-dawn pinning). Note `fluid-react` is **already failing on
   main** (board 2026-08-24, MPM mass accounting) — attribute before
   touching, don't inherit the blame; fixing it lands in this WP if the
   cause is seam-side.

---

## 7. WP4 — performance: cost proportional to live particles, sleep is real

**The determinism trap to state up front:** CPU-side gating of *whether* fluid
passes are recorded must remain deterministic. `fluidCount` is monotone
precisely so recording doesn't depend on GPU readback timing. **Never gate
recording on an async readback.** The sanctioned shape: keep recording the
passes, make them cost ~zero via GPU-owned indirect args — a
`DispatchWorkgroupsIndirect` with 0 groups is the sleep state. (An agent WILL
be tempted by the readback; this paragraph is why not.)

Ranked by structural exposure (measure each against `--fluid-bench pool` +
`faucet`; provisional targets in §9):

1. **Indirect-everything off GPU counters.** Compaction count/scatter and
   consume walk a GPU-maintained high-water mark (a new FA_* word), not the
   262,144-slot pool; `seam_settle_judge` walks the active-block list, not
   all 32,768 slots; scratch clears become compute passes over active blocks
   only (kills the 8 MiB/tick `fluidCellScratch` fill; `pass_table.def` rows
   change — full R/W sets, `check_pass_table.py`).
2. **True sleep.** GPU-side: when live==0 (or all-calm for N ticks) and no
   pending spawns, zero ALL fluid indirect args including the 6 substeps.
   Idle cost falls to recording ~30 empty indirect dispatches. The monotone
   CPU `fluidCount` stays as the recording gate (determinism).
3. **Solid-mask cache for `gridUpdate`.** Terrain cannot change between the 6
   substeps (CA runs before the fluid tables in `EncodeTick`). Build a
   per-active-block 16³ solid bitmask once per tick (512 B/block, can live in
   the freed cellScratch space); `fluidGridUp` reads bits, not 7 `voxWordAt`
   probes/node. ~6× fewer voxel reads at equal behavior — bit-exact, so
   `fluid-det` hash must NOT change (differential check).
4. **Block-map once per tick, not ×6.** Max displacement is ≤0.45 cell/
   substep ≤ 2.7 cells/tick; mark with a conservative pad (3 cells) and
   rebuild the map once. Verify the alloc's neighbor padding covers it.
5. **Rendering bounds.** A per-frame world AABB of active fluid blocks in
   RenderParams; rays that miss it skip the fluid march entirely (kills the
   screen-wide cost for off-screen fluid); march clipped to the AABB;
   reduce the 48-read normal (shared samples / 4-tap tetrahedral); distance-
   LOD the secondary refract/reflect rays like the foam LOD precedent
   (`debris.wgsl`, commit 499b507).
6. **Particle slimming — evaluate AFTER 1–5 with bench evidence.** The
   capslpop option: drop stored v/C/density/J (g2p re-derives from the grid;
   settle's calm test moves to grid-node speeds, which is cheaper anyway),
   keep pos + attr + species/birthTick → 128 B → ~32–48 B, compaction traffic
   ∝. Blocked if WP2 adopted the FLIP knob. Cheap partial regardless: stop
   ping-ponging the 12 reserved words (80 B effective) — but only if the
   bench says compaction bandwidth matters.

CPU-side strain: expected to mostly be the GPU backpressure + the never-sleep
tick cost; re-measure CPU frame time in the lab after items 1–2 before
inventing CPU work. (An unmutexed concurrent sandvox.exe poisons every number —
run.sh always, check tasklist, measure twice.)

---

## 8. WP5 — Phase 3 of the plan of record (the flip)

Only after WP1–WP4 are green and the user has visually signed off the lab
scenes: delete the CA liquid movement rules (`stepLiquid`'s move branches,
`canFlowAnywhere`'s liquid arm), flip `sim.fluidExciteMode` default to 1,
rebaseline the pinned world hash 7cfa2420 (this is THE hash-moving commit —
its own change, nothing else in it), re-baseline affected gates, update
DESIGN.md §4/§5 in the same commit, mark this plan and the relevant sections
of PLAN_mpm_fluids.md historical. Persistence stays as designed (saves
force-settle; excite reconstructs on disturbance). PLAN_mpm_fluids.md Phases
4–7 (occupancy/mirror integration, rigid coupling, audio, content) continue
after, unchanged.

---

## 9. Acceptance criteria and bench targets

Look (lab scenes, user is the judge — screenshots via `--shot`-style capture
per scene):
- `hill`: poured water sheets down the 30° ramp and collects in the basin; no
  mid-slope freeze; slope is dry (no beads/ropes) within a bounded time after
  the pour stops.
- `basin`: dam break splashes, sloshes with visible inertia, rings down, and
  settles flat.
- `pool`: after settling, the scene is bit-idle (see below).
- Filaments/droplets read as water, not mucus, at 6 substeps/tick.

Perf targets (WP1 baselines are MEASURED below, 2026-08-24 — quote before/after
against that block; these remain the targets):
- `pool` post-settle: fluid GPU time ≈ 0 ms (all indirect args zero); frame
  time equals the no-fluid lab baseline.
- `faucet` steady-state at ~50–100k live particles: fluid sim+seam ≤ ~4 ms
  GPU on the RTX 3060 Ti; fluid rendering ≤ ~2.5 ms at 1080p with fluid
  on-screen, ≈0 ms with fluid off-screen.
- No fixed-size dispatch in any fluid pass table scales with pool capacity or
  world size (code-inspection criterion, enforced by reading pass_table.def).

Correctness (existing machinery):
- `fluid-det` twice-run equality at every landing; world hash 7cfa2420
  untouched until WP5's dedicated flip commit.
- Mass ledger exact in every scene and every gate (eighths in == out).
- `check_pass_table.py` + `check_invariants.py` green; `--vk-validation`
  clean smoke at each WP end (full acceptance is an end-of-work event, per
  CLAUDE.md — not per-edit).

### WP1 baselines (measured 2026-08-24, RTX 3060 Ti, 1080p offscreen)

`bash scripts/run.sh ./build/Release/sandvox.exe --fluid-bench all --json out.json`
— per-pass GPU ms from passtimer (avg/p95 of per-tick deltas), render is
WaitIdle-bracketed wall (no render-pass queries exist), frame = tick+render
wall in the serialized harness. OLD = the pre-reconciliation live tuning.json
(stiffness 11500, gravity 181.9, eosPower 3, cohesion 25, attractSame 12,
attractDiff −25, viscosity 0.5, splashRate 1.5/idx 1 — preserved in the
bench JSON and in `tuning_old_live_wp1.json` side file); DEF = the pure
tuning_params.def defaults (5400 / 98.1 / 4 / 90 / 45 / −90 / 1.5 / 4.0 /
idx 2). hill0 = hill at exciteMode 0.

Merge note (WP1 landing): the live tuning.json carries the DEF reconciliation
EXCEPT cohesion 32.9 / attractSame 0 / attractDiff −1.08, which are the user's
deliberate tuner-session retune (commit ac949df, same day) and were preserved
over the .def values (90 / 45 / −90). The DEF bench rows above were measured at
pure .def. Note the user's retune independently moved the attract terms to
≈zero — the WP2 item-3 zero-tension direction.

| scene  | cfg | fluid(substep) avg/p95 | seam avg | settle avg | render avg | frame p50/p95 | live end/max | settled@ |
|--------|-----|------------------------|----------|------------|------------|---------------|--------------|----------|
| basin  | OLD | 2.55 / 2.92 | 0.17 | 0.12 | 10.97 | 15.3 / 16.4 | 15,300 / 15,360 | never |
| basin  | DEF | 2.55 / 2.92 | 0.18 | 0.12 | 11.84 | 15.2 / 16.6 | 15,058 / 15,360 | never |
| hill   | OLD | 5.40 / 6.42 | 0.24 | 0.16 | 26.78 | 36.6 / 39.2 | 39,282 / 39,600 | never |
| hill   | DEF | 5.52 / 6.62 | 0.24 | 0.15 | 25.83 | 34.9 / 39.2 | 39,600 / 39,600 | never |
| hill0  | OLD | 5.46 / 6.41 | 0.25 | 0.16 | 26.45 | 35.7 / 39.1 | 39,240 / 39,600 | never |
| hill0  | DEF | 5.60 / 6.63 | 0.25 | 0.16 | 25.91 | 34.9 / 39.4 | 39,600 / 39,600 | never |
| faucet | OLD | 3.00 / 5.15 | 0.18 | 0.12 | 19.04 | 24.0 / 26.9 | 33,096 / 33,096 | n/a |
| faucet | DEF | 3.03 / 5.21 | 0.19 | 0.12 | 19.31 | 24.2 / 27.0 | 33,096 / 33,096 | n/a |
| pool   | OLD | 3.79 / 4.44 | 0.20 | 0.14 | 17.49 | 22.9 / 27.4 | 26,400 / 26,400 | never |
| pool   | DEF | 3.56 / 4.35 | 0.20 | 0.13 | 17.65 | 22.9 / 27.4 | 26,397 / 26,400 | never |
| slosh  | OLD | 1.76 / 2.10 | 0.16 | 0.11 | 17.40 | 21.1 / 22.7 | 5,311 / 6,400 | never |
| slosh  | DEF | 1.79 / 2.12 | 0.15 | 0.10 | 16.61 | 20.1 / 22.5 | 6,251 / 6,400 | never |

What the numbers say (the diagnosis, now measured):

- **Nothing settles. Anywhere.** Every scene ends its run (400–600 ticks =
  13–20 s) with ~all mass still live — even `pool`, the sleep scene, sits at
  26,397 of 26,400 live at tick 500, and basin's sealed dam-break churns at
  15,300 live at tick 400. §1.2 item 1's "clamp-saturated churn" holds at
  BOTH tunings: the fluid-excite gate's finding that stock stiffness sits at
  the CFL edge is now a bench fact for every scene. Consequently
  `tick-of-settle`, hill-vs-hill0 (the trapdoor A/B) and "pool post-settle
  ≈ 0 ms" are all UNMEASURABLE until WP2 restores CFL honesty — the excite
  trapdoor cannot even be reached when nothing ever settles.
- **Solver cost scales with live count** (rule-2 behaviour on the substep
  table itself): ~2.5 ms at 15 k → ~5.5 ms at 40 k particles, ≈ 0.13–0.17 ms
  per 1k live per tick. The ≤4 ms @ 50–100 k target above therefore needs
  roughly a 2–4× improvement (WP4 items 1/3/4/6).
- **Seam + settle are count-independent** (~0.35–0.40 ms combined at 6 k and
  at 40 k alike) — the fixed-size 262,144-slot/32,768-slot dispatches of
  §1.3, measured. WP4 item 1's target is ≈ 0 at low counts.
- **Rendering dominates the frame**: 11–27 ms of the 15–37 ms frame is the
  render, scaling with fluid pixels on screen (basin 11 ms → hill 26 ms).
  The ≤2.5 ms @ 1080p target is a ~10× ask — WP4 item 5 is not optional.
- **Old-live vs .def-defaults is cost-neutral** (differences are within run
  noise) — the reconciliation changed the look regime, not the price. The
  parameter dead zone of §1.4 is real: 2× gravity + 2× stiffness moved no
  timing needle because the VMAX clamp was already the binding constraint.
- **Mass ledger EXACT in all 12 runs** (eighths in == standing + carried;
  day phase pinned sunrise+1024 so evaporation cannot leak the audit).
- Worldgen residency footnote: the lab slab worldgens at 89 resident pages
  (1.4 MiB) vs 2,861 (44.7 MiB) for the default world — the flat world is
  effectively free, so the frame is fluid + render and nothing else, which
  was the point of WP1.

---

## 10. Handoff rules for implementing agents

- Board-claim per WP (`bash scripts/board.sh claim`), one worktree per WP if
  parallel; WP2 and WP4 both touch `sim_fluid.wgsl` (§5.2 vs §7.3) —
  sequence those two items explicitly via board notes.
- Build only via `scripts/build.sh`; every run via `scripts/run.sh`;
  `SANDVOX_NO_CRASH_DIALOG=1`; verify exe mtime; the verification budget
  rules in CLAUDE.md apply — `--fluid-bench` + one gate while iterating,
  full suite once per WP.
- `gen_tuning_prelude.py` cannot parse trailing comments on TP_ rows; new
  knobs follow the 5-place TUNE_* pipeline; sim.fluid* stays in the
  human-unit-floats-const-eval exception lane.
- Known-failing on main as of 2026-08-24: `fluid-react` (board note,
  post-baseline gate). Attribute failures against clean HEAD before blaming
  your diff (standing repo lesson).
- Every landed WP updates DESIGN.md in the same commit where behavior
  changed, and appends measured numbers to §9 of this doc.
