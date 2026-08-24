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

### WP4 results (measured 2026-08-24, RTX 3060 Ti, 1080p offscreen)

`bash scripts/run.sh ./build/Release/sandvox.exe --fluid-bench all --json out.json`
on the CURRENT `tuning.json` (the .def reconciliation plus the user's three
retuned knobs — so the WP1 rows above, measured at pure .def and at old-live,
are the right *shape* of baseline but not the same configuration).

Provenance of the "before" numbers, exactly:
`docs/bench/wp4_fluid_bench_baseline.json` is a run whose ONLY changes from the
fork point were render-side, so its `fluid(substep)` / `fluidSeam` /
`fluidSettle` / live curves ARE the unmodified fork point's, and its render avg
(hill 25.08) reproduces the WP1 DEF row (25.83) to within noise. The per-pass
RENDER split could not exist before the attribution probe was built, and that
probe landed together with item 5a — so the "fluid march" before-column below
already has 5a's AABB and tetrahedral normal in it and **understates the true
starting point by ~0.7-1.0 ms per scene**. `docs/bench/wp4_fluid_bench_after.json`
is the final tree.

**The bench now attributes its own render time.** Every 16th tick it re-renders
the same frame twice more: once with `fluidCount = 0` (which skips the fluid
march wholesale) and once also without the droplet raster. That was the first
thing WP4 did, and it immediately corrected §9's WP1 reading of "rendering
dominates, scaling with fluid pixels on screen":

| scene  | fluid march | droplet raster | world+sky |
|--------|-------------|----------------|-----------|
| basin  |  7.80 ms | 0.08 ms | 2.91 ms |
| hill   | 20.07 ms | 0.28 ms | 4.15 ms |
| faucet | 16.38 ms | 0.17 ms | 3.71 ms |
| pool   | 13.28 ms | 0.24 ms | 3.72 ms |
| slosh  | 12.34 ms | 0.04 ms | 3.77 ms |

The droplets are free, the world+sky floor is a flat ~3-4 ms, and **the fluid
isosurface march is the whole render cost** — and was the whole WP4
opportunity: at 39,600 particles `hill` spent 20 ms of render against 5.5 ms of
solver.

#### Before -> after

| scene       | fluid(substep) | seam+settle | fluid march | render avg | frame p50 |
|-------------|----------------|-------------|-------------|------------|-----------|
| basin       | 2.53 -> 2.09 | 0.29 -> 0.27 |  7.80 -> 2.88 | 11.8 -> 5.90 | 15.2 -> 9.36 |
| hill        | 5.47 -> 4.69 | 0.40 -> 0.37 | 20.07 -> 9.52 | 25.8 -> 13.86 | 34.9 -> 21.86 |
| hill0       | 5.56 -> 4.65 | 0.40 -> 0.37 | 19.88 -> 9.44 | 25.9 -> 13.73 | 34.9 -> 21.68 |
| faucet      | 3.01 -> 2.46 | 0.30 -> 0.27 | 16.38 -> 7.67 | 19.3 -> 11.28 | 24.2 -> 15.57 |
| pool        | 3.63 -> 3.03 | 0.34 -> 0.31 | 13.28 -> 6.39 | 17.7 -> 10.34 | 22.9 -> 14.65 |
| slosh       | 1.79 -> 1.38 | 0.27 -> 0.23 | 12.34 -> 4.37 | 16.6 ->  8.16 | 20.1 -> 11.05 |
| pool-settle | (new) 2.05 | 0.29 | 6.92 | 11.03 | 14.67 |

Whole-frame p50 is **1.5-1.9x better in every scene**. Mass ledgers unchanged
(see the `pool` note below). `pool-settle` is new — see item 2.

#### Per item

1. **Indirect-everything.** `seam_compact_count` / `seam_compact_scatter`
   (1,024 fixed workgroups = all 262,144 pool slots) and `seam_consume_apply`
   (4,096) now dispatch off new FA_* arg triples (19..21 written by
   `exciteScan`, 22..24 by `compactScan`), and the 8 MiB `fluidCellScratch`
   fill became a compute clear over active blocks. Worth **~0.03 ms/tick**, not
   the ~0.35 the structure suggested, and the bench says why: **the lab scenes
   allocate 8-22 node blocks, not the 256 the buffers are sized for**, so those
   fixed dispatches were mostly launching threads that returned immediately.
   What is left of seam+settle is per-dispatch/barrier overhead across ~21 rows.
   Size any further work here against measured block counts first.
2. **True sleep.** `alloc` and `settleScan` were the only fluid passes not
   dispatched off an indirect arg — single-workgroup walks of all 32,768 chunk
   slots, running on every tick of any world that had ever poured water, because
   the CPU-side `fluidCount` is monotone. That monotonicity stays (recording
   must be a pure function of tick-deterministic inputs, never of readback
   timing — §7's opening paragraph), so both now early-out on `FA_LIVE == 0`.
   **Idle cost with the fluid tables fully recorded and nothing alive: 0.0000
   ms/tick median (below timer resolution).** Because nothing settles at stock
   tuning, the bench gained a `pool-settle` run that applies the `fluid-excite`
   gate's overrides through the F5 reload path; it converts 19,635 of 26,400
   eighths back to voxels inside 500 ticks.
3. **Solid-mask cache for `gridUpdate` — NOT IMPLEMENTED, and the measurement
   is why.** §7 sized it at "~44 M voxel reads/tick at 256 blocks". Measured
   block counts are 8-22 (hill peaks at 28 after item 4), and `gridUpdate`
   already skips its 7 probes for any node below `FLUID_MASS_MIN`, so the real
   figure on `hill` is ~1 M probes/tick, about 0.05 ms of a 4.7 ms substep
   table. A bit-exact change with a real blast radius (per-block bitmask, build
   pass, pass-table rows) for under 1% — deliberately left undone.
4. **Block map once per tick** (`PT_FLUIDMAP`), `mark` padded by 3 cells against
   the 2.7 cell/tick CFL bound. **14-24% off the substep table.** The padded set
   is a superset of every substep's exact set and the extra chunks receive no
   particle support, so nothing changes: `fluid-det`'s particle hash is
   unchanged at `14650fb0739d0383` and every scene's liveEnd and ledger are
   identical. Not free — the pad grew hill 22 -> 28 blocks and cost the renderer
   1.26 ms until item 5c.
5. **Render bounds.** Four changes, in order of what they bought:
   * **5a, the AABB.** `RenderParams.fluidLo/fluidHi`, CPU-built from the
     snapshot block list plus the tick's spawns, dilated 2 chunks. Rays that
     miss pay one slab test; rays that hit march only [enter, exit]; the
     thickness walk stops at the exit. It is also what makes fluid *sleep* on
     the render side, since `fluidCount` never decays.
   * **5b, the seam ring was 16 cells thick.** `fluidChunkActive` called a chunk
     active when a FACE NEIGHBOUR had a block, so the march fine-stepped a whole
     chunk of air for an isosurface that reaches 2 cells in. Split into a class
     (block / ring / nothing) plus a per-cell shell test.
   * **5c, the Y-occupancy mask.** `fluidBlockMap` doubles to 2*kNumChunks; the
     second half is 16 bits per chunk, one per local y level, written by
     `fluidGridUp` (node mass) and `seamStainApply` (settled liquid's virtual
     mass). Gravity-fed fluid is a thin horizontal layer, so an empty y slab is
     skipped on ONE buffer read instead of ~13 trilinear samples of 8 taps each.
     Pays back item 4's pad and more (hill 12.17 -> 9.52).
   * **5d, cheaper per-pixel work.** The chunk class was recomputed every
     0.5-1.25 cells for an answer that changes every 16 (now held); the
     thickness walk was 28 fixed samples feeding an exponential that is blind
     past ~10 cells (now <=14, geometric); `fluidNormalAt` was 6 field samples =
     48 buffer reads of central differences (now 4-tap tetrahedral = 32, same
     smoothing radius via the 1/sqrt(3) factor).

   **Target check:** §9 asks for <=2.5 ms at 1080p with fluid on screen. Reached
   only by `basin` (2.88, close). `hill` at 9.52 ms is a 2.1x miss — but hill
   fills most of a 1080p frame with water at 30-60 voxels' range, the
   adversarial case rather than the typical one. "~0 with fluid off-screen" IS
   met, by the AABB. What remains is the per-sample price of `fluidFieldAt` (8
   trilinear taps, each a block-map resolve + grid read + page-table resolve +
   voxel read); the next levers are hoisting the per-chunk resolves out of the
   tap loop (~82% of samples have all 8 taps in one chunk) and a finer
   (4³-brick) occupancy than the y mask.
6. **Particle slimming — EVALUATED, NOT DONE.** The bench does not support it.
   Compaction moves 128 B x live particles per tick: 5.1 MB/tick at hill's
   39,600, roughly 0.02 ms of bandwidth on a 3060 Ti against a 4.7 ms substep
   table. The count-INDEPENDENT part of seam+settle is ~0.27 ms and is
   per-dispatch overhead across ~21 rows, not traffic — halving the particle
   would not move it. The substep table's own scaling solves to 7.9e-5
   ms/particle with a 1.3 ms fixed term (from basin 15,360 -> 2.09 ms and slosh
   ~6,000 -> 1.38 ms), i.e. the per-particle cost is the 27-node P2G/G2P atomic
   scatter, not the fetch. Slimming is still right for CAPACITY (105 MiB
   resident, 12 of 32 words are zeroed reserves) — it is not a frame-time lever,
   and it forecloses WP2's FLIP option. Revisit if WP2 pushes live counts past
   ~150 k.

#### Two findings that are not WP4's to fix

* **`pool`'s mass ledger reads LEAK (26,400 = 6 standing + 26,341 carried) and
  did so before any WP4 edit** — identical numbers on the unmodified fork point.
  It is a BENCH-BOUNDS artifact, not a sim leak: water splashes over the pool's
  10-high wall and lands on the slab outside `LabSceneBounds`, which is the box
  the standing count sweeps. Plainly visible in
  `screenshot_lab_pool-settle.bmp`. Widening the scene bounds fixes the
  accounting.
* **`fluid-react` fails and has since the WP1 merge (c4f4ba7).** Now recorded in
  `tests/baseline.json`, with the attribution and three corroborating board
  notes in `tests/BASELINE.md`. Not chased here: it is seam mass accounting,
  i.e. WP3 §6 item 5, which already names it.

---

### WP2 results (measured 2026-08-24, RTX 3060 Ti, 1080p offscreen)

`bash scripts/run.sh ./build/Release/sandvox.exe --fluid-bench all --json
docs/bench/wp2_fluid_bench_after.json`, on top of WP4's tree. Before = the
WP4-after block above (same harness, WP4 tree, pre-WP2 physics).

**Defaults (the .def, tuning.json and tuning.h all agree now):**

| knob | old | new | why |
|---|---|---|---|
| fluidStiffness | 5400 | **3600** | c = √3600 = 60 vox/s = 2 cells/tick = 0.33 cells/substep at 6 substeps — honest headroom under FLUID_VMAX 0.45. 5400 → 0.41, at the edge; live 11500 → 0.60, past it |
| fluidGravity | 98.1 | 98.1 | already real (9.81 m/s² at 0.1 m voxels) |
| fluidEosPower | 4 | 4 | unchanged |
| fluidCohesion | 90 | **0** | zero-tension water: EOS floor exactly p ≥ 0 |
| fluidAttractSame | 45 | **0** | sticky-ropes term, authoring-only now |
| fluidAttractDiff | −90 | **0** | same |
| fluidViscosity | 1.5 | **0.1** | references run 0.02–0.1 grid units; hill A/B at 0.5 was capture-identical (53.3% both), so the look picks |
| fluidDamping | 0 | 0 | stays 0 (rule: fix explosions with stiffness/substeps, never damping) |
| fluidFriction | — | **0 (new)** | tangential (1−friction) in the separate BC; 0 = free-slip water, the mud/goo knob |

**Solver changes** (sim_fluid.wgsl): separate BC with tangential preservation
in `gridUpdate` (in-solid surface nodes keep face-parallel components; only
into-solid normal motion is removed; 1-cell walls zero the axis —
anti-tunneling); `FA_CLAMPED` counts VMAX truncations per tick (§5 item 1's
probe). Seam: `SEAM_SETTLE8/WAKE8` → `SETTLE4/WAKE4` (threshold quantization
0.117 → 0.0073 vox/s, monotone slider; overflow audit in the const comment).
The predictive wall spring (§5 item 2's escalation) was NOT needed — no
crusting in any scene screenshot. Item 5 (FLIP) was NOT needed: the failure
mode after items 1–4 is *excess* liveliness (pools churn above settleEps
indefinitely), not dead water.

| scene | frame p50 (WP4 → WP2) | fluid substep | fluid march | clamps/tick | tick-of-settle | end state |
|---|---|---|---|---|---|---|
| basin | 9.36 → 9.61 | 2.09 → 2.13 | 2.88 → 3.53 | **0** | never | 633 settled, rest churns |
| hill | 21.86 → 20.54 | 4.69 → 4.57 | 9.52 → 9.38 | **0** | never | capture 53.3%, 0 settled |
| hill0 | 21.68 → 20.60 | 4.65 → 4.61 | 9.44 → 9.54 | **0** | never | capture 53.3% (no trapdoor: nothing freezes mid-slope at eps 0.9) |
| faucet | 15.57 → 14.78 | 2.46 → 2.23 | 7.67 → 7.69 | **0** | n/a | 500 settled under sustained pour |
| pool | 14.65 → 14.05 | 3.03 → 3.13 | 6.39 → 5.92 | **0** | never | 0 settled — churns at stock |
| slosh | 11.05 → 11.32 | 1.38 → 1.56 | 4.37 → 4.34 | **0** | never | 702 settled at the calm ends |
| pool-settle | 14.67 → 14.26 | 2.05 → 2.53 | 6.92 → 6.69 | **0** | never (18,511 of 26,400 settled) | ledger EXACT now |

What the numbers and screenshots say:

- **The hill flows.** The pour sheets down the ramp as a connected film and
  the catch basin (now carved 9 deep — the old slab-level basin could hold
  only 22,528 of the 39,600 poured eighths, capping the capture metric at 57%
  by geometry) fills to 53.3%. No mid-slope freeze at either excite mode —
  the user's reported failure is gone. The remaining ~47% is a quasi-static
  terraced pond ladder on the stepped treads plus the pour deck, in slow
  recirculation with the plunge pool: a 1200-tick probe showed capture
  *decreasing* 53.3 → 47.9% as plunge-churn backsplash transports mass back
  up-ramp. The 85–90% capture target therefore waits on WP3: the plunge pool
  never calms below settleEps 0.9 at damping 0, so it keeps splashing; once
  settle criteria let basin water become voxels, the recirculation source
  dies. (Raising settleEps toward the churn floor was deliberately NOT done
  here — that re-opens the mid-slope-freeze trapdoor WP3 exists to close.)
- **CFL honesty is measured, not asserted**: FA_CLAMPED reads 0 across all
  7 runs × 400–1200 ticks. The VMAX clamp is now a genuine safety net.
- **Settle happens at stock for genuinely calm water** (fluid-settle gate:
  1280/1280 eighths converted, quiet at tick 121, at PURE stock — first time
  ever; basin/faucet/slosh films settle mid-scene) but **churning pools do
  not**: a stock pool (eps 0.9, damping 0, sealed-ish geometry) rings
  indefinitely. tick-of-settle (full conversion) is still −1 everywhere at
  stock; the pool-settle run (settle trio 6.0/24/0.9 via F5) converts 70%.
- **Mass ledger EXACT in all runs** including pool-settle — WP4's "LEAK" was
  confirmed as the audit-bounds artifact and fixed (the sweep now looks 12
  cells beyond the scene walls for over-the-wall splash).
- **Cost is unchanged** (within noise) — WP2 changed the physics regime, not
  the price; WP4's 1.5–1.9× stands.
- **fluid-excite override shrinkage** (the §5 success signal): 7 → 3. Gone:
  stiffness 2400, cohesion 0, attractSame 0, attractDiff 0 (all stock now).
  Stays: settleEps 6.0 / wakeSpeed 24.0 / damping 0.9 — a sealed drained
  chamber genuinely rings (~18 vox/s measured at eps 3/wake 12/damping 0.45,
  which FAILED); that trio is WP3's problem, not a solver defect.

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
