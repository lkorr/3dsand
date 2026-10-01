# Wind — architecture decision record

Date: 2026-08-25. Status: **decided; phases 1, 2, 3, 4 and the flip landed —
wind is ON (`sim.windMode` = 1) and PRIMITIVES ARE IN, which is what makes wind
a gameplay tool and what makes entrainment safe. Phase 6 (drafts: walls
shelter the wind) landed 2026-09-30, §14. Phase 5 open.**
§10 records what phase 4 found; DESIGN.md §9b is the binding summary. This is
the plan of
record for the wind system; DESIGN.md gets its section when phase 1 lands (same
commit). Owner: Luke. Research: two-agent sweep (industry + codebase), condensed
in §2 — full citations inline.

---

## 0. The decision in one paragraph

Wind is a **pure function, not a stored field**: `windAt(worldPos, t)` evaluated
on demand, composed of a deterministic CPU-computed weather state (global vector,
evolves chaotically over minutes), two traveling gust bands (promoted from the
sway code that already ships in the raymarcher), an altitude ramp, a
chunk-granular **derived** updraft term (from per-chunk hot-material counts, no
stored heat state), and a bounded list of **wind primitives** (fans, spell gusts
— parametric objects riding the op stream, like point lights). There is no
per-chunk vector storage, no ±θ neighbor-constraint relaxation, no resolution to
choose — the function is continuous and costs only where sampled: active CA
voxels, particles, MPM grid nodes, visible foliage. The CA consumes a fixed-point
evaluation (`windAtQ`, Q16.16, sine LUT) behind `sim.windMode=0` until its own
rebaseline commit; everything else consumes f32 with zero hash risk. Rejected:
per-chunk stored vectors with angular smoothing constraints (rule-2 violation,
new authoritative state, propagation latency, resolution ceiling — see §3).

## 1. Requirements (owner)

- Ambient 3D wind field, continuous-looking, magnitude scaling with altitude,
  evolving/chaotic over time like real weather.
- Influences movement of gases (strongly), falling liquids/powders (weakly, by
  material), **only while actively moving** — wind must never fight settling.
- Settled powder has a **friction threshold**: unmoved until a per-axis wind
  component exceeds a per-material coefficient; then it can be pushed (fans
  blowing sand piles, sandstorms drifting dunes).
- Velocity semantics: airborne matter under sustained wind (updrafts) should
  **accelerate over time**, not just displace per-tick.
- Applies forces to particle systems: explosion debris, MPM fluid spray.
- Drives animation: grass, tree sway, foliage, capes/cloth (future) — players
  should literally *see the field* in a grass plain.
- **Player-interactable**: spells emit gusts; fan objects blow continuously;
  both push voxels around.
- Near-zero overhead. No constant per-tick cost proportional to world size
  (inviolable rule 2). No FPS regression.
- Debug: in-game toggle showing the 3D slope field (arrows/field lines) to
  verify behavior.

## 2. Research findings (condensed)

Full sweep 2026-08-25. Headline: **nobody stores a world-scale wind field.**

| System | Representation | Storage | Cost |
|---|---|---|---|
| Ghost of Tsushima (GDC 2021) | global vector + Perlin gust noise + "vorticles" (sparse vortex/gust primitives, brute-force summed) | ~0 (primitive list) | "small fraction of PS4" for grass+cloth+100k particles |
| God of War 2018 (GDC 2019) | the ONE shipped stored 3D grid: 32×32×16 cells @ 1 m³, camera-local, + procedural wind motors injecting | <1 MB | first CS dispatch of frame, trivial |
| Just Cause 4 | fully analytic (tornado = stacked cylinders on a spline; wind tunnels = capsuloid Béziers); per-object aero baked to 7 KB cubemaps | ~0 | ~1 ms/4 threads incl. all aerodynamics |
| The Powder Toy | stored 2D grid, 4× coarser/axis than particles; damped physically-wrong-but-stable stencil | ~235 KB | negligible |
| Noita | no wind system at all | — | — |
| Crysis / GPU Gems 3 ch.16 (the Unreal/Unity/SpeedTree template) | per-instance vector + 4 triangle-wave octaves in VS; **phase = f(worldPos)** makes gust fronts travel | vertex colors | constant per-vertex |
| Sea of Thieves / Valheim | one global vector per server/world | ~0 | ~0 |

Key lessons adopted:
- **"Volume over accuracy"** (Sucker Punch): one shared wind input sampled
  consistently by every consumer sells realism; no solver needed.
- **Curl noise** (Bridson SIGGRAPH 2007): divergence-free wind as a pure
  function of (pos, time); vortex primitives have closed forms. GoT's vorticles
  are this paper evaluated brute-force.
- **Per-material response is authored, not derived** (Powder Toy: every element
  has hand-tuned `Advection` + `AirDrag`; nothing computed from density).
- **Phase-as-function-of-world-position** (Crysis) = traveling gust fronts for
  free — our engine already does this in the sway bands and the global color
  lattice.
- GoW proves the fallback: if a stored field is ever wanted, ~32³ cells,
  camera-anchored, is what shipped.

## 3. Rejected: stored per-chunk vectors + ±θ neighbor constraint

The original sketch (a vector per chunk / per 2×2×2 sub-chunk, neighbors
constrained within ±θ, relaxed over time). Rejected because:

1. **Rule 2 violation**: constraint relaxation is a per-tick pass over all
   32,768 window chunk slots whether anything moves or not.
2. **What relaxation converges to is a smooth low-frequency field** — which an
   analytic function IS, exactly, for free, with no convergence latency.
3. **New authoritative state**: must be saved, hashed or explicitly excluded,
   streamed as the window scrolls (the page-table work shows scrolling stored
   state is where cost hides), and someday replicated.
4. **Resolution ceiling**: 1 or 8 vectors/chunk quantizes the field; a function
   has infinite resolution (sample per blade, per voxel, per particle).
5. **Propagation latency**: constrained info moves ~1 chunk/tick; analytic
   gusts travel at authored speed.

The per-chunk *concept* survives in three supporting, value-invisible roles:
primitive culling masks, primitive footprint wake, and the heat term's input
granularity (§4.4, §4.5). If profiling ever shows base-field eval too hot in the
CA inner loop, the contained fallback is caching 1 (or 2×2×2 lerped) evaluations
per chunk per tick for CA use only — the GoW shape. Not expected.

## 4. Architecture

### 4.1 The field function

```
windAt(p, t) = weather(t)                        // global vec, CPU-computed per tick
             + gustBands(p, t)                   // 2 traveling sine bands, world-phase
             * altRamp(p.y)                      // altitude gain
             + updraft(heatBelow(p))             // derived, chunk-granular input (§4.4)
             + Σ primitives_i(p, t)              // fans/spells, culled per chunk (§4.3)
```

Two evaluations of the SAME function, both in `common.wgsl` (single
authoritative source — if a C++ mirror is ever needed, add a
`check_invariants.py` entry):

- `windAt` — f32. Consumers: sway/strands, debug viz, MPM, ballistic particles,
  future cloth. No determinism constraint beyond what each consumer already has.
- `windAtQ` — Q16.16 integer, sine via LUT (or const-eval polynomial), for the
  CA only (phase 4). Human-unit float knobs convert at WGSL const-eval exactly
  like `sim_fluid.wgsl:98-188` (IEEE-exact, kernel stays integer).

Units: knobs in m/s and meters; `kVoxelMeters = 0.10` (cells/s = m/s × 10),
30 Hz ticks. Gas movement tail runs 2 substeps/tick → max CA drift ≈ 2
cells/tick ≈ 6 m/s; wind bias saturates below that (§4.5).

### 4.2 Weather state — deterministic chaos on the CPU

Per-tick CPU function `WindWeather(seed, tick) -> {dirRad, speed, gustiness}`:
epoch = `tick >> 11` (~68 s); targets drawn via `hash3(seed ^ 0xAE01, epoch,
component)` (distinct salt per the worldgen salt rule — never a bit-slice of an
existing stream); smoothstep between epoch targets; optional storm episodes as
occasional high-speed epochs. Values ship in **RenderParams** (phase 1, render
consumers) and later **TickParams** (phase 4, sim consumers) — the `dayPhase`
precedent (`world.h:785-788`): CPU-computed inputs that must be captured by
replay/determinism gates ride the tick input stream. One C++ function is the
only author of these values for both UBOs.

Manual override knobs (fixed direction/speed, weather-auto bool) for testing.

### 4.3 Wind primitives — the player interaction surface

A primitive is a parametric object, ~32 bytes: `{kind (directional cone /
vortex / sphere burst), posQ, dirQ, strengthQ, radius, falloff, spawnTick,
TTL}`. World list bounded (cap ~64). **Not** per-chunk, not a sample lattice —
evaluated analytically at any sample point like a point light.

- **Moving primitives are analytic in time**: position =
  `f(spawnParams, t - spawnTick)` — no per-tick mutation, pure input data.
  A gust bolt is a traveling sphere; a wind wall a long-TTL slab; a tornado a
  vortex; a fan the same primitive with infinite TTL anchored to its object.
- **Op-stream only** (rule-3 philosophy): spells emit primitive ops through the
  spell VM (already op-stream-only); fan objects register/deregister on
  place/break, authored by prefab/material tag (no closed-ended systems). No
  side channels — the op is the only way player wind exists (replay, save,
  future net all inherit it).
- **Per-chunk cull mask**: tiny pass between dispatches (dispatch-invariant,
  like `pageTable` — safe to read at any radius during the CA) marks which
  primitives touch which chunk, so an active voxel sums 0–2, not 64.
- **Footprint wake**: THE invariant amendment. The ambient field NEVER wakes a
  chunk. A primitive dirty-marks only its own footprint (via mutation-path wake
  ops each tick it lives). Bounded and player-caused ⇒ rule-2 clean. Cap cone
  length + primitive count; budget-charged before emission.
- Visual skin: cosmetic particles spawned along the same analytic path show the
  gust; they carry zero sim authority. (GoT: invisible vorticle does the work,
  VFX + bending grass show it. Our grass shows it automatically since sway
  samples `windAt`.)

### 4.4 Heat / updrafts — derived, not simulated (v1)

There is no temperature anywhere in the sim today (fire is reaction-tag driven,
`tag:hot`). Do NOT introduce simulated heat state in v1. Instead: a per-chunk
**hot-material count** maintained like the occupancy counts (recomputed when a
chunk changes — fire chunks are active by definition while burning). The
updraft term gathers counts from the few chunks below the sample point —
chunk-granular input, lerp between neighbors if stepping shows. Derived ⇒
reconstructible ⇒ no save/hash/streaming surface (microvox principle).

Upgrade path if plumes need memory (lingering thermals): a stored per-chunk
accumulator patterned on `fluidCalm` (`world.h:1489` — the existing persistent
per-chunk u32), updated by a small kernel over the compacted dirty list only.
That is a contained escalation, not the v1 design.

### 4.5 CA coupling (phase 4, hash-gated)

Today gas movement draws one `hash3` per cell and rotates a fixed cyclic
direction order by RNG bit-slices (`sim_step.wgsl:1310-1330`; draw at `:1247`).
Wind hooks in as:

- **Drift bias (moving voxels only)**: with probability ∝ axis-projected
  `windAtQ` × material `windResponse` (capped ~50%), start the rotation at the
  downwind direction instead of random. One RNG draw, write reach ≤1
  (`tryMove` untouched), swap semantics preserved. Applies to gas
  diagonal/lateral stages and powder/liquid diagonal-fall stages.
  Physics note: instantaneous advection is CORRECT for gases (massless
  parcels move with the wind — no inertia to model).
- **Entrainment (settled powder, primitive footprints + budgeted storm wake
  only)**: per-axis test `|windAtQ·axis| > windFriction(mat)` unlocks a
  lateral/up-diagonal move candidate — saltation. Two-threshold hysteresis is
  emergent: settled needs the entrainment threshold, already-moving gets the
  cheap drift bias (Bagnold's fluid vs impact threshold, free from the sleep
  machinery).
- **Sleep law**: wind bias runs only inside the movement tail of active chunks.
  A becalmed voxel that can't move marks nothing and sleeps. Ambient wind can
  never wake anything (the "light-gated rules never sleep" lesson, applied).
- **Materials**: `windResponse` (0–15) + `windFriction` (0–15) authored in
  materials JSON, default `windResponse ~ k/density` (physically: accel ∝ 1/ρ
  at fixed voxel size), overridable (iron shavings high — real-world
  susceptibility is A/m i.e. SIZE, which the grid erases, so authoring is
  honest — and it's the Powder Toy's proven pattern). `MaterialGpu` is 64 B
  with NO spare words: bit-pack into an existing word per the `stainPack`
  precedent (`materials.h:59-90`); do not grow the struct.
- **Gate**: everything above behind `sim.windMode = 0` (the `fluidExciteMode`
  precedent). Flip = its own rebaseline commit. Pinned hash at time of writing:
  `882a30f3` — it is moving fast (fluid sessions); ALWAYS read current
  CLAUDE.md and coordinate the flip.

### 4.6 Particle tier — where velocity accumulation lives

- Ballistic debris: add wind force at the single gravity site
  `sim_particle.wgsl:147` (Q24.8). Particles deposit back as voxels on landing
  (`:273`) — the round trip exists.
- MPM: add at the grid-node gravity site `sim_fluid.wgsl:736` — per-node ⇒
  spatially varying for free. Open question: all nodes vs low-mass
  (surface/spray) nodes only — full-body wind on a pond reads as current, not
  wind.
- Both systems are deterministic but OUTSIDE the world hash ⇒ no rebaseline.
  Jolt debris is free-running CPU float by design — mirror the field shape in
  f32 C++.
- **Violent wind promotes voxels to particles** (phase 5): a strong primitive
  (tornado, big gust) excites grains into ballistic particles (velocity from
  wind × `windResponse`), wind force accumulates in flight, deposit on landing
  — the MPM excite/settle seam pattern with existing budgets/ceilings. CA
  drift-bias handles ambient; the particle tier handles violence; the excite
  threshold is the seam knob.

### 4.7 Animation

- Phase 1 rewires the two existing band-construction sites — brick sway
  `raymarch.wgsl:879-883` (applied `:943-949`) and strands `:1091-1094` — to
  sample `windAt`. Direction stops being hardcoded ("X leads, Z trails");
  per-column hash scatter and per-blade band weights stay (decorrelation is
  what makes a field read as wind, per Crysis). Default knobs ≈ current look.
- Trees/foliage: already per-material via `MICROF_SWAY`; they inherit the
  rewire.
- Capes/cloth: none exist yet. The seam is the spring/jiggle `goal`
  (`mob.cpp:1662-1687`, `avatar.cpp:1183-1185`): `goal += windAt(pos) × gain`
  when cloth arrives.
- Water: the shader's five wave bands are documented as "wind-driven gravity
  waves" (`raymarch.wgsl:3648-3655`) with no wind input — keying their
  direction/amplitude off `windAt` is a cheap future look win (open question).

### 4.8 Debug visualization (phase 1 deliverable)

In-game toggle → instanced arrow overlay: lattice of sample points around the
camera (spacing/radius tunable, default 8 vox / 48 vox), vertex shader derives
the lattice point from instance ID + camera, evaluates the SAME `windAt` the
consumers use (this is the point — no copy), emits an oriented arrow colored by
magnitude (cool→hot), alpha-faded by distance, depth-tested against the scene
like the debris raster path. One draw call, zero CPU per-arrow work, zero cost
when toggled off. Stretch: streamline mode (12–16 Euler steps per seed — the
integral curves of the slope field). Toggle = in-game key + tuner bool.

## 5. Cost model

- Base field: 0 bytes, 0 dispatches. ~20 int/float ALU per sample at sites
  already paying a `sin()` (sway) or a gravity add (particles). Unsampled ⇒
  uncosted. (1M active-voxel samples ≈ 20M ALU ≈ deep sub-ms.)
- Primitives: list upload (<2 KB/tick), cull mask pass over active chunks,
  0–2 extra evaluations per sample inside footprints. Zero primitives ⇒ zero.
- Heat counts: piggyback on already-active chunk maintenance; 128 KB buffer.
- CA bias: a few int ops per voxel already in the movement tail.
- New authoritative state in v1: **none** (weather = tick input; heat =
  derived; field = function; primitives = ops).

## 6. Invariants (bind into DESIGN.md as phases land)

1. Wind is a function; **no stored wind field, no per-voxel wind state ever**
   (voxel bits 19–23 stay free).
2. One authoritative field implementation in `common.wgsl`; mirrors (C++)
   guarded by `check_invariants.py`.
3. **The ambient field never wakes a chunk. Primitives wake only their bounded,
   budget-charged footprint.**
4. Wind bias applies only to voxels already executing the movement tail;
   settled matter moves only via the entrainment threshold inside awake
   footprints.
5. Player/world wind exists ONLY as primitive ops on the input stream — no
   side-channel writes.
6. Sim consumption is integer (`windAtQ`), gated by `sim.windMode`, flipped
   only in a dedicated rebaseline commit.
7. `windResponse`/`windFriction` are authored material data (JSON), never
   hardcoded per material in shaders.

## 7. Phases

| # | Scope | Hash risk | Acceptance |
|---|---|---|---|
| 1 | `windAt` f32 + weather state + sway/strands rewire + **debug slope-field viz + toggle** + `wind.*` knobs | none (render-only) | **DONE** — pinned hash unchanged; viz shows the field; grass lean follows the direction knob |
| 3 | Debris + MPM wind force; `windResponse`/`windFriction` authoring + packing | none while gated | **DONE** — packed into `flags` bits 8..15, since `MaterialGpu` has no spare word |
| 4 | `windAtQ` + CA drift bias + entrainment, behind `sim.windMode=0` | none while gated | **DONE** — the `wind` gate: reversing the direction knob reverses the smoke, the settled bed is bitwise unmoved under drift, twice-run equality holds |
| 2 | Primitive list + op plumbing (spell VM op, dev placement), footprint wake, viz shows primitives | none (empty list is an exact identity) | **DONE 2026-08-26** — the `wind-prim` gate: a licensed fan creeps a settled bed 12.65 cells downwind in a chamber that is ASLEEP, waking 10 chunks and losing no grains, with the suite's page-fault counter at 0. An unlicensed fan blows smoke and leaves the bed bitwise unmoved |
| 4b | Flip `sim.windMode` to 1 | rebaseline | **DONE** — `882a30f3` → `47dd1520`; sleep still 0/32768 chunks active, dense reproduces the same hash, both smoke tables re-recorded with `worldgen` byte-identical |
| 5 | Heat counts → updraft term; violent-wind excite-to-particle; capes when cloth exists | rebaseline | fire columns loft smoke/embers; tornado lifts sand |
| 6 | Draft volumes: local coarse relaxation so a room vents through its openings (§11) | rebaseline | **DONE 2026-09-30 (§14)** -- the `drafts` gate: sealed hut 0.037, door + leeward window 0.42 along +X, smoke leaves through the window; the pinned hash did not move |

Phases 3 and 4 landed together, and in that order, because §4.6 is wrong about
one thing: it has the particle and MPM consumers reading the f32 `windAt`. They
cannot. Both feed the voxel grid — particles reinsert as voxels, MPM settles
back through the excite seam — so both are inside rule 1 and both need the
integer field phase 4 was going to build. `windAtQ` therefore came first and all
three consumers share it, which is also why phase 3 reads "none while gated"
above rather than the "none (unhashed systems)" this document originally
claimed. The gate is what makes it hash-neutral, not the systems being outside
the hash.

## 8. Future unification: ocean currents and rivers (planned, no phase yet)

The architecture is medium-agnostic. Underwater, `currentAt` is `windAt` with a
different term set: tides/large-scale circulation = the weather-state pattern at
lower frequency; kelp = underwater grass (same MICROF_SWAY/strands path);
swimmers/mobs/debris/MPM nodes take the force at the same sites; vortex
primitive = whirlpool; the heat/updraft term = hydrothermal vents literally.
The sleep law binds even harder in water: ambient current moves things IN the
ocean, never the settled ocean voxels (the definitional settled system). Water
that must genuinely move = a strong primitive exciting settled voxels into MPM
through the existing seam (ceiling/budgets/mass-exact accounting already
built) — the tornado path, wet. Rivers (none exist yet): flow direction is
terrain-derived, and worldgen is analytic, so river current = function
composition (downhill gradient of the same height field, or a generated channel
spline evaluated like a JC4 wind tunnel) — zero storage. Bulk river transport
stays fake (surface flow-map advection in the water shader + forces on
everything in it; real transport only at rapids/waterfalls where water is
excited anyway) — a truly-flowing river would keep its whole length awake,
violating rule 2.

**Binding consequences for earlier phases**: (a) the phase-2 primitive struct
carries a medium mask (air/water/both) from day one so currents need no
op-format change; (b) the field core (bands, ramps, primitive summation) is
written as shared guts with `windAt`/`currentAt` as thin wrappers, not
wind-specific.

## 9. Open questions

- ~~MPM wind: all grid nodes vs low-mass-only.~~ **ANSWERED (phase 3): low-mass
  only.** Wind on every node of a pond is a *current* — the whole body
  translates, the surface stays flat, and it reads as the lake being poured
  sideways. Wind acts on the interface and the body follows through the fluid's
  own viscosity. The node's own mass is the exposure test, and the solver has
  already computed it, so the answer costs nothing.
- Derived (memoryless) heat vs stored accumulator — decide after phase 5 look.
- Altitude ramp reference: absolute world Y vs terrain-relative (lean: absolute
  Y, simplest and deterministic; terrain-relative needs a height query).
- Water wave bands keyed off wind direction (render-only, cheap, do eventually).
- Storm-wake budget shape for ambient dune drift (per-tick chunk cap, surface
  selection heuristic).

## 10. What phase 4 found: entrainment needs phase 2 first

§4.5 gates entrainment on "primitive footprints + budgeted storm wake only" and
gives the reason as rule 2 — an exposed dune, once woken, keeps re-marking its
own chunks for as long as the wind blows. That reason is right and it is not the
only one.

Entrainment is **the first rule in the engine that makes settled matter move**,
and the page table's materialization set leans on the opposite. The set is
`[ (cpuDirty n hasMatter) u N26(...) ] u N26(opTargets)`, and `cpuDirty` is
TIGHTENED against a lagging snapshot (PLAN_page_table.md §3.2). Under every
pre-wind rule that tightening is sound *because a chunk of settled powder writes
nothing*: dropping it from the mirror, and letting its empty neighbour's page
retire, costs nothing that can happen. Turn entrainment on and a grain steps
into a neighbour the CPU was told would never be written — and the write is a
lost voxel, not an error.

Measured with the `wind` gate at `SANDVOX_WIND_ENTRAIN=1`: **62 faults across
two 160-tick runs, at the same ticks in both**, so deterministic rather than a
race. The test chamber's own bed creeps 22 cells downwind and conserves every
grain; the losses are elsewhere in the world, where the same rule is mobilising
terrain powders that had settled.

The fix is the one phase 2 was already going to build. A wind primitive
dirty-marks its own bounded footprint **through the mutation path**, which makes
those chunks `opTargets` — CPU-known, materialized with their 26-ring, and
charged against a budget before emission. One mechanism closes both holes, which
is usually the sign of a real mechanism rather than a patch.

So the ordering changed: **phase 2 is a prerequisite for switching entrainment
on**, not merely the next feature. `sim.windMode = 2` ships implemented, warned
about by `LoadTuning`, excluded from the default suite, and reachable in one
environment variable by anyone who wants to look at it.

**RESOLVED 2026-08-26 by §12.** Entrainment is on, per primitive, and the
`wind-prim` gate runs in the default suite with the fault counter at zero. Mode
2 — the same rule with the licence removed — is still not a default and is not
expected to become one; the shipping way to blow a dune flat is to point
something at it.

There is a wider version of this worth stating, because it will be true again:
the page table's soundness argument quietly rests on **"settled matter does not
move"**, a property no rule had ever contradicted. Any future rule that makes
resting voxels move without a CPU-visible cause — not just wind — lands in the
same hole. The tell is a non-zero page-fault count with no obvious lost voxel
near the thing you were testing.

## 11. Drafts through openings (planned "phase 6": the local refinement volume; REFINED by §14)

Owner requirement (2026-08-25): a room with a door and a window should carry a
draft; smoke inside should find the exits. This is the first requirement the
pure function CANNOT satisfy — the answer depends on geometry — and it is the
planned use of §3's contained fallback (the GoW shape): a **draft volume**,
small, local, coarse, sleeping.

- Grid at ~4-voxel cells (fine enough to tell a door from a wall), 64–128 vox
  on a side ⇒ 4k–32k cells ≈ GoW's entire shipped wind sim. Solid cells come
  from the EXISTING sub-chunk occupancy bitmasks (`world.h` occupancy +
  kSubOccStride) — the walls mask is already maintained.
- Boundary seeded from ambient `windAtQ`; a fixed handful of damped Jacobi
  relaxation iterations, solids blocking ⇒ flow threads door→window (The
  Powder Toy's air sim, in 3D, locally). Phase-5 heat counts as a pressure
  source (TPT `HotAir`) make burning rooms vent with no ambient wind.
- Smoke consumption reuses the phase-4 drift-bias plumbing: sample the draft
  volume where covered, ambient elsewhere. No new CA rule.
- Determinism: TICK-cadence updates (never frame), fixed iterations,
  fixed-point, ping-pong (no atomics), deterministic inputs only (occupancy +
  `windAtQ` + heat counts), written between dispatches (dispatch-invariant,
  the `pageTable` rule), anchor rides TickParams (`mirrorBase` precedent).
- Rule 2: volume exists only while active gas is nearby; sleeps to zero cost.
  Bounded concurrent-volume count, budget-charged like primitives.
- Cost: ~32k cells × ~8 iterations every ~8 ticks ⇒ microseconds on GPU,
  zero CPU. Update cadence is a knob; drafts do not need 30 Hz.
- Ordering: after phase 2 (reuses footprint/budget machinery; §10's
  CPU-visibility lesson applies), paired naturally with phase 5 (heat).

## 12. What phase 2 shipped (2026-08-26)

### The object

`src/sim/windprim.h` / `.cpp`. A primitive is ~48 bytes: position, unit axis
(Q16.16), core strength (Q16.16 cells/s), radius, axial reach, a vortex swirl
and rise share, a kind, flags, a spawn tick, a TTL and an opaque owner handle.
The GPU form is three `vec4<i32>` rows; the ceilings and the encoding live in
`world.h` next to `TickParams`, because they are the GPU layout.

Three kinds — `cone` (fans, gust bolts, wind walls), `burst` (blast fronts, and
a **vacuum** at negative strength), `vortex` (tornadoes; whirlpools when the
medium mask says water). They span the requirements rather than enumerate
shapes, and anything else is these summed, which is the point of making them
additive rather than exclusive.

**No square root anywhere.** The radial profile is quadratic in `r^2`, the axial
one linear in the dot product, and the burst takes its DIRECTION from the offset
vector itself rather than normalising it. That is what keeps a primitive
affordable inside the CA's movement tail. It has one honest consequence: a burst
has no wind at its exact centre, which is true of a blast anyway — there is no
preferred outward direction at a stagnation point.

### Where they live, and why that was the whole cost saving

**In `TickParams` and `RenderParams`**, not a storage buffer. 32 x 48 bytes of
per-tick CPU-authored configuration is what a uniform is for, and the
consequence is that the entire feature costs **no new binding, no new barrier
and no new dispatch** — except the wake kernel, which is a genuinely new thing
to do. A storage buffer would have meant a new binding in BOTH group-0 layouts
(`common.wgsl` is prepended to every shader, so one identifier cannot carry two
binding numbers), a new pass-table row set, a new barrier class, and the same
again on the render side.

The render copy is what makes §4.7 and §4.8 free: the sway sites and the arrow
overlay sum the same list, so the grass leans in a fan's blast and the arrows
show the fan, with nothing wiring foliage to fans. Invariant 2, still holding.

**Zero primitives is an exact identity.** `windPrimCount == 0` early-outs in
`windPrimAt`, `windPrimAtQ` and `windPrimEntrainsQ`, and the union AABB is
shipped empty (`lo > hi`, the fluid-render-box convention) so a sample outside
every footprint rejects in four compares. The pinned hash did not move.

### Movement is analytic, and resolved on the CPU

§4.3 said "moving primitives are analytic in time". The refinement phase 2
makes is that the evaluation happens ONCE PER TICK ON THE CPU, not per sample on
the GPU: `origin + vel * (tick - spawnTick)` is 32 evaluations rather than
millions, and the shader is handed a primitive that is already where it is. The
same pass applies the lifetime envelope (attack/release), because a 40 m/s gust
that switches on between two ticks reads as smoke teleporting — the CA's drift
bias is a probability, not a force, so it has no inertia to smooth the step.

### The footprint wake — §10's fix, built

`sim_mutate.wgsl` gains a third entry point, `windWake`, and it is the only
thing in the engine that dirty-marks a chunk without writing a voxel. The CPU
side (`WindPrimSystem::BuildWake`) does the work that makes it safe:

1. only primitives holding `kWindPrimEntrain` produce a wake at all, so a
   decorative gust is free;
2. the footprint is the primitive's SWEPT box, not `pos ± max(radius, reach)` —
   a 36-cell fan declares a 36x16x16 box rather than a 72-cube;
3. it is filtered against the snapshot's per-slot occupancy, which is what turns
   a fan's footprint from "a box" into "the surface it is aimed at" (the
   snapshot is one tick latent, which is the safe direction: a chunk that just
   gained matter is already dirty from the op that put it there);
4. it is charged against `sim.windWakeChunks` before emission, and refusals are
   counted rather than hidden;
5. and the SAME list is handed to `PageTable::AddOpTarget`, so those chunks are
   materialized with their 26-ring before the command buffer exists.

Step 5 is the one that matters. A grain that hops into a neighbouring chunk hops
into one the CPU had already declared writable, so the tightening argument the
page table rests on is repaired rather than worked around.

The licence is bounded at SPAWN, not trimmed at wake time: a primitive whose
footprint exceeds `kWindWakeMaxChunks` (512 chunks) is refused the flag and
still blows. Trimming would leave entrainment working in an arbitrary corner of
the blast, which is worse than not working, and it would also leave the wake
SCAN unbounded — a 512-cell vortex would walk 274,625 chunk slots a tick to
discover that most of them are sky.

### The entrainment gate, restated

`sim_step.wgsl`'s step 5 now reads

    windMode >= DRIFT && substep 0 && powder &&
        (windMode >= ENTRAIN || windPrimEntrainsQ(cell))

so the licence is per primitive and per cell. `windPrimEntrainsQ` is a separate,
cheaper question than "what is the wind here" — being inside is a yes/no, and
asking the full evaluator would pay for a weight nobody reads.

### Producers

- **`gust`**, a modifier glyph in `assets/spells/glyphs.json` with a `wind`
  block (`kind`, `speed`, `radius`, `reach`, `ticks`, `swirl`, `rise`,
  `entrain`). It emits through `SpellEmission` like every other spell effect,
  which means it is position-parameterized and a FATAL gust goes off in the
  caster's own chest for free (spell.h thesis 2). Repetition amplifies SPEED,
  not size: widening the footprint would multiply the wake cost eightfold for
  one extra word. A `duststorm` conjoined example ships with it.
- **The dev panel**, which can place one where the camera is looking, with the
  kind/speed/radius/reach/licence exposed. It goes through the same
  `WindPrims().Spawn()`; there is no dev-only path into the wind system, which
  is what makes what you see there the same thing a spell produces.

Both are refused rather than silently displacing something when the world list
is full, and the refusal is shown in the HUD next to the projectile count.

### The gate

`wind-prim`, in the default suite. A sealed chamber with a settled sand bed on
the floor and — in most arms — a smoke blob as a witness. It asserts: a licensed
fan creeps the bed +12.65 cells downwind and reversing it reverses the creep; an
UNLICENSED fan visibly blows the smoke (+8.79 cells) and leaves the bed BITWISE
unmoved; in a chamber with NO smoke, so genuinely asleep, the fan wakes 10 chunks
and the bed still creeps by the same amount — which is the wake proving itself
rather than riding on the smoke keeping the CA alive; grain count is conserved
across every arm; twice-run equality holds; the wake stays inside its budget;
and the suite's page-fault counter is 0.

Note what the gate does NOT need: the `wind` gate above it writes one
`kCellOpIfAir` per chamber chunk per tick purely to keep the chunks awake. This
one has no scaffolding at all. That difference IS the feature.

### One bug worth recording, because its shape recurs

The wake did nothing at first, silently. The per-tick counts cross THREE
hand-written structs on their way to the recorder —
`Simulation::RecordCtx` → `rhi::TableCtx` → the recorder's own `RecordCtx` —
and one copy was missing, so the row's condition read a default zero and the row
was never recorded. No error, no validation message, a green build, a green
selftest, and a CPU cheerfully shipping a correct 10-slot wake list every tick
to a kernel that never ran.

`scripts/check_invariants.py` now has a `counts` check that compares all three
structs field-by-field and asserts both copy sites exist. It was negative-tested
in both directions.

### What is deliberately NOT here

- **The per-chunk cull mask** (§4.3). With a cap of 32 and a union-AABB reject
  that costs four compares, the loop is only ever entered near a primitive. A
  cull-mask pass would be a new dispatch and a new buffer to save what is
  already cheap; if profiling ever disagrees, the mask is still the answer.
- **A fan as a placed OBJECT.** The primitive, the op path, the owner handle and
  `RetireOwner` are all in place, so a prefab/material tag that registers one on
  place and retires it on break is a small content-side addition rather than an
  engine change. It waits on there being a fan to place.

## 13. The weather-driven field (2026-09-30)

Six changes that make the ambient field behave like real near-surface wind:
it depends on height above the ground and on terrain, its CHARACTER (not
just its speed) changes with the weather, its gust fronts travel the right
way, and there is something to see it by. Code: `src/sim/windfield.{h,cpp}`,
`src/sim/wind.h`, the WIND FIELD section of `common.wgsl`,
`assets/shaders/wind_streak.wgsl`, `assets/wind/regimes.json`. Gate:
`wind-field` (CPU-only, through the C++ mirror of `windAtQ`).

### 13.1 Gust fronts travel downwind (the bug)

The bands were `sin(rate * t + K * s)`. A crest sits where `rate t + K s` is
constant, so it moves at `ds/dt = -rate / K`: **upwind**, at ~0.8 m/s, at any
wind speed. Measured on the shipped integer field with the wind toward +X: a
crest at x = 12 at tick 0 was at x = 8 fifteen ticks later (−8 cells/s,
analytic −8.4).

Now `sin(c * (K s - adv) - rate_c * tt)`: `adv = K * ∫U dt` is how far the air
has travelled, so the pattern is frozen into the moving air and crosses the
meadow downwind at the mean. The integral is NOT `U * t` — U changes, and
`U(t) * t` flings the whole pattern by `U' * t` (kilometres after an hour) at
every weather change. It is summed on the CPU over 16-tick blocks of the
reference speed `WindWeatherQ` produces (`windfield::AdvPhase`), memoised per
(seed, tuning fingerprint) — a cache, not state: recomputing from tick 0 gives
the same integers. It is reduced mod 10 turns because every band coefficient
is a multiple of 0.1. The old incommensurate rates survive as a slow
EVOLUTION term (`wind.gustSpeed`, now 0.35 rad/s) in the air's own frame
(Taylor's frozen turbulence is approximate), with the advection's sign. The
spatial wavelength is unchanged, so a stronger wind means more frequent gusts
at a point. Fronts ride the REFERENCE speed, not the local one — a local U
would shred the pattern (`K t ∇U · p` grows without bound); gust momentum is
brought down from aloft anyway. `wind.gustAdvect` scales it. The clouds drift
by the same sum (`AirDrift`), so sky and grass share one wind.

Found on the way: `windSinQ`'s correction square `ay * ay` overflowed i32 for
every |sin| > 0.707 (sin 45° read 0.48, the peak 0.77), so every sim-side gust
had been clipped flat on top. Staged as `(ay >> 1)² / 16384`.

### 13.2 Height above ground, exposure, altitude

`ramp = profile(hAGL) × exposure(x, z) × absTerm(y)` replaced
`1 + 0.6 (y - 64) / 100`, which was anchored 136 voxels below the default map's
ground and stood every player in 1.8x the authored wind.

- **profile** — log-law `ln(h/z0 + 1) / ln(href/z0 + 1)`, clamped to [floor,
  cap], with `href` (1.5 m) the height where the mean equals the authored
  speed. Shipped as a 16-knot table (h = 0, 1, 2, 4 … 16384 voxels), built on
  the CPU with exact integer log2 (`imath::Log2Q16`; the base cancels in the
  ratio) and interpolated linearly between doubling knots on both sides, so
  the sim reads the same integers on every machine.
- **exposure** — `1 + gain × clamp(TPI / scale, -1, 1)`, TPI the ground minus
  its neighbourhood mean (100 m). With `scale` = radius / 2 a gain of 1 is the
  textbook hill speed-up 2H/L. Separate ridge (s > 0) and hollow (s < 0) gains,
  each blended light → strong by intensity; fades with height (40 m).
- **absTerm** — `1 + absGain (y - seaLevel)`, small (4% / 100 m), the only term
  that still reads absolute altitude and the only one past the table.

**The terrain table** (`world.h kWindTerrN`): 64 × 64 cells of 32 voxels =
204.8 m centred on the window, one word each — surface height i16 (standing
water counts), exposure i8, water fraction u8 — stored toroidally (a window
shift leaves surviving cells in place). Filled from `World::TerrainColumn`
(the bit-exact CPU twin of `genColumn`): fine heights at cell centres, the
neighbourhood mean and water fraction from a 128-voxel coarse lattice through
a prefix-sum box filter, bilinear to the fine cell. Cached by world cell, so a
window shift queries one new row or column. It rides TickParams and
RenderParams (+16 KiB each) rather than a storage binding: a binding would
mean a new entry in every consumer's layout (sim_step, particle, fluid, gas,
raymarch, arrows, clouds) and pass-table rows, for data that is a pure
function of (seed, map, cell) and changes only when the window shifts. Sampled
bilinearly — in exact integer weights on the sim side — with the gradient
coming free from the same four taps. Outside coverage: `absTerm × neutral`,
blended over the outer two cells. It is WORLDGEN height: digging a pit does not
shelter it (accepted; DESIGN.md §9b).

This is the one STORED field in the wind system. DESIGN.md §9b's invariant 1
is amended accordingly: the terrain table is static, seed-derived, derived
data (rebuilt, never saved); the weather is still not stored.

### 13.3 The regime: intensity changes the pattern

Three inputs, all Q16 (`wind.h WeatherRegime`): **intensity** (≈ Beaufort/12),
**gale** weight, **convective** weight, plus the sky **cover**. Named presets
live in `assets/wind/regimes.json` (calm, light, breezy, windy, gale,
thunderstorm — a new one is a row). **One weather:** every sky preset in
`assets/weather/*.json` names a regime in its `"wind"` field and the sky's
integer ladder (`weather::SimWindRegime`, the same `ScheduledQ` the rain word
walks) blends them, so a storm sky blows a storm and fog is calm. The wind's
own ~68 s epochs swing the sky's intensity ±`moodSpread`, and an epoch that
draws a storm under a windy sky becomes a gale. `SetWindRegimeSource` is the
hook a future weather system installs (it must be a pure function);
`wind.regime` pins a preset and `wind.intensity` overrides the intensity
(both live, the F1 picker and slider). With no sky, the epochs drive it.

`WindWeatherQ` (integer end to end) turns the regime and the day phase into
field parameters, each a knob:

| term | light / convective | strong / gale |
|---|---|---|
| mean speed | windSpeed × curve(I): 0.1x at 0, **1x at 0.3**, 3.2x at 0.75, 5x at 1 | |
| stability | + by day (sun), − at night, × (1 − 0.7 cover), mixed to 0 by I = 0.55 | neutral |
| ground coupling | 1 − 0.6 × stable, recovering by 25 m | 1 |
| gust fraction | 0.8 (+0.4 convective) | 0.5 (gust factor ~1.5) |
| thermals | 1.2 m/s isotropic, 25 m cells, 14 s | 0 |
| heading meander | ±45°, 80 m, 45 s | ±12°; a gale suppresses it 80% and holds an ~18 min heading |
| ridge / hollow gain | 0.25 / 1.2 (hollows shelter) | 1.0 / 0.5 (2H/L) |
| lee rotor | off | past I 0.55: slopes falling downwind steeper than 0.5 get +0.8 gust and reverse flow (0.35) in a 12 m layer |

On the shader side the meander turns the mean and the band FRAME but not the
gust PHASE (turning the phase would put a far-from-origin sample through
`K |p| dθ` of phase); thermals and the local winds are an additive `extra`
term that `windMeanWS` adds, so every consumer sees them; the lee reads the
table's own bilinear gradient.

### 13.4 Local winds and storms

- **Slope winds** — up the table's slope by day (anabatic), down it at night
  (katabatic, 0.7x), 2 m/s at full stability in a 15 m layer.
- **Sea / lake breeze** — along the water-fraction gradient, onshore by day,
  offshore at night (0.4x), 3 m/s in a 60 m layer. Both fade to nothing as the
  mean passes 6 m/s.
- **Thunderstorm** — a timeline in cycles of `stormCycle` (240 s) weighted by
  the convective input: lull (0.3x), the gust front at u = 0.25 (heading jump
  ±~90° drawn per cycle, 2.5x spike), decay to 1.4x, then gusty decay home. Its
  spatial half is emitted through the existing primitives
  (`windfield::WeatherPrims`): a wide CONE jet sweeping across the window along
  the new heading at the storm's speed, and downbursts (BURST, half their
  radius above `TerrainHeight`) after the front. A pure function of (tuning,
  seed, tick, window), appended to both primitive lists after the placed ones;
  Air only, so no entrainment licence and no footprint wake.
- **Gale** — sustained intensity, steady heading, gust factor ~1.5 (§13.3).

### 13.5 The sim's response

Saltation was a threshold then a FLAT rate: a wind 1% over the line moved a
dune as fast as a gale. Sand flux grows roughly with the cube of the excess,
so the hop chance is now `rate × min(excess / (threshold × span), 1)^power`,
power 3 (`sim.windEntrainPower/Span`). `windEntrainSpeed` 2 → 1.2 m/s: sand
(friction 5, derived) lifts at 6 m/s AT THE GRAIN — the observed 5–7 m/s near
the ground; the profile puts a grain at ~0.3x the reference wind. The drift
bias stays linear on purpose: it is ADVECTION of a passive tracer (smoke, a
falling grain carried by the air), which is linear in U; force ∝ U² is the
particle tier's drag law, which is already `rate(|w|) × Δv`.

### 13.6 Gust streaks (render-only)

A fixed pool (2048, trail 16) near the camera, one per-frame compute row and
one ribbon draw, both skipped at visibility 0. A dead slot respawns only
where the gust excess (bands along the mean + primitives) passes a threshold,
so calm days show nothing; each streak is advected by `windAt` (primitives
included), i.e. the streaks are the field, drawn. All knobs are live uniforms.

### 13.7 The C++ mirror

`windfield::Probe/ProbeMany` transcribe the integer field over the same wf*
block the tick ships (i32 wraps emulated) — the F1 readout and the
`wind-field` gate read it. `check_invariants windmirror` holds its constants to
the shader's; nothing compares the two bit for bit at run time.

## 14. Drafts: the shelter volume (phase 6, 2026-09-30)

Status: **LANDED 2026-09-30** (DESIGN.md §9b "Drafts" is the binding
summary). §14.1-14.9 are the plan as written; **§14.10 records where the
build departed from it, and why** -- read it before the solver sections. Refines §11. Owner requirement (restated
2026-09-30): now that rain is blocked by roofs and walls (DESIGN.md §9.w
"Where the rain lands"), the wind has to be too. Indoors the field should die
down, air should move through a room only where it has a way in AND a way
out, and smoke inside should find the exits. Today every consumer reads the
ambient field straight through walls: smoke in a sealed hut leans at the
full outdoor speed, embers inside drift with the storm, and the gust streaks
fly through the walls of the room the camera stands in.

### 14.1 What changes from §11, and why

§11 planned a volume that relaxes the LIVE wind each tick (ambient boundary
in, Jacobi iterations, field out) and exists only near active gas. Two
problems surfaced while planning it against the engine as it stands now:

1. **History.** A Jacobi solve that converges over several ticks carries its
   iterate from tick to tick. The rain exposure map was designed to have no
   history at all ("nothing to carry across a save, a load, a replay start or
   a window move"), and the same constraint binds here: the sim reads this
   field, so the field at tick t must be a pure function of the state at tick
   t. A cold solve per tick satisfies that but has to converge from zero
   every tick, which with plain Jacobi over a room-sized domain takes
   hundreds of iterations.
2. **Sleep.** The live wind changes every tick (gust bands, meander), so a
   live-wind solve can never sleep while anything reads it, and the renderer
   reads it every frame near the camera.

**The fix is linearity.** The pressure projection is a linear operator, and
the gust bands' shortest wavelength (8 m) is long next to a room. So the
volume does not solve for the wind. It solves for the **transfer** of the
wind: how a unit horizontal wind at infinity, along +X and along +Z, is
redirected by the geometry. Those two responses are a function of geometry
ALONE:

```
R_x(p), R_z(p) = P(e_x), P(e_z)        // 3-vectors per cell; P = projection
windIn(p)      = R_x(p) * amb.x(p) + R_z(p) * amb.z(p)
               + (0, amb.y(p) * open(p), 0)
               + primitives(p)          // unprojected, as today
```

`amb` is today's ambient field (`windAtQ` without primitives), evaluated at
the sample point, so the log profile, terrain exposure, meander, gusts and
storm envelope all still apply. Only the GEOMETRY response is stored.
`open(p)` scales the ambient's vertical component (thermals, band updraft) by
the cell's mean |R|.

What this buys:

- **The volume sleeps.** R changes only when the solid geometry inside the
  volume changes (a wall dug, a door opened, a house burning) or the volume
  moves. A settled world costs a mask check over the active chunks inside the
  volume and nothing else (rule 2).
- **No history.** R is recomputed in full, from zero, inside the tick whose
  geometry changed. A tick whose geometry did NOT change gets the same R by
  purity. After a load, a replay start or a window move, the volume
  rebuilds from what is there, exactly as the rain map does.
- **One field.** `windAtQ` and `windAt` both apply R inside the volume, so
  every consumer (CA drift, entrainment, particles, MPM nodes, streaks, the
  debug arrows) stands in the same sheltered wind. That is invariant 2 held.

What it costs in accuracy, accepted for v1:

- Gusts indoors are the outdoor gust at the same point, redirected and
  scaled. Indoor air has no gust pattern of its own.
- **Potential flow, no turbulent wake.** Behind a wall the projection gives
  a calm stagnation pocket about one obstacle size deep. A real wake runs
  5-10 heights. A wall still shelters, just over a shorter distance.
- Fans and spell gusts (primitives) are added on top, unprojected. A fan
  inside a room is not confined by the room's walls.
- **No stack effect.** Drafts come from wind pressure only. A fire in a
  sealed room does not drive air through it; hot smoke still rises through
  the CA's buoyancy. The heat term is phase 5 (§4.4), and it would enter as a
  pressure source in this same solve.

### 14.2 What happens physically, and what the gate checks

The projection makes the wind divergence-free subject to no flow through
solids, with the volume's outer faces open (phi = 0). The result:

| geometry | result | real behaviour |
|---|---|---|
| sealed room | R = 0 inside: a uniform flow cannot exist in a closed box | still air |
| one opening, windward | small recirculation inside the opening, near zero beyond it | weak pulsing only |
| door windward + window leeward | flow threads door to window, speed set by the openings' areas | cross-ventilation draft |
| small window, big room | interstitial speed = flux / porosity, so the jet through the gap is fast | wind whistles through gaps |
| open pavilion (roof on posts) | flow passes under the roof, slightly accelerated | breezy shade |
| gap between buildings | continuity speeds it up | venturi gusts in alleys |
| cave | dies with depth past the mouth | still air |
| lee of a wall | calm pocket about one wall height deep | shelter (shorter than real) |

### 14.3 The grid

- **Cells of 4 voxels (0.4 m)**, aligned to the 4^3 sub-occupancy blocks.
  The volume is `kDraftNX x kDraftNY x kDraftNZ` cells, starting at
  **64 x 32 x 64 = 131k cells (25.6 m x 12.8 m x 25.6 m)**. These are
  `world.h` constants generated into the prelude, so measurement can grow
  them. Centred on the CPU-mirror centre (`TickParams.mirrorBase` + 1 chunk,
  i.e. the player), snapped to whole chunks. A window shift moves it, which
  forces a full rebuild.
- **Porosity per face, from voxel ROWS, not "any blocker in the cell."**
  Testing "any blocker" at 4 voxels closes a 1-voxel wall, which is correct,
  but it also closes an 8-voxel doorway to one cell column and a 4-voxel
  window to nothing. Instead each cell stores three 16-bit row masks: bit
  (u, v) of the axis-a mask is set if the 4-voxel row through the cell along
  a, at cross-section (u, v), holds a blocker. Reading a cell's 64 voxels once
  fills all three masks. The open fraction of the face between cells A and
  B on axis a is `popcount(~(maskA_a | maskB_a) & 0xFFFF) / 16`. A one-voxel
  wall blocks every row it crosses. A 2x2-voxel hole leaves 4/16 of a face
  open. The solve's coefficient is beta = that fraction, which makes it
  variable-coefficient (porous-media) Poisson.
- **Which voxels block:** the CA's non-gas matter (solid, powder, liquid).
  Air, gas and fire pass. Foliage materials (a material tag; check
  `materials.json` for the leaf family) do NOT block in v1: a crown as a
  solid lump would squeeze flow and speed it up underneath, which is worse
  than letting the crown be transparent (today's behaviour). Porous foliage
  is a follow-up.
- **Door leaves are voxels**, so opening or closing a `refs` door is a
  geometry change and switches a draft on or off. That falls out for free.

### 14.4 The tick, in passes (all on the sim table, before the CA)

1. **`draftMask`.** Recomputes the row masks for every cell of every ACTIVE
   chunk (`dirtyList`) inside the volume, plus every chunk of the volume on a
   rebuild tick (window shift, load, first tick, `draftEpoch` change). It
   writes the masks and raises a "changed" word if any mask differs from the
   stored one. Only active chunks can change geometry, since settled matter
   writes nothing and every op dirty-marks what it touches, so the cost
   tracks activity. The stored masks are a pure function of current geometry:
   an untouched chunk's masks are unchanged because its voxels are.
2. **`draftSolve`, only when changed.** The dispatches are indirect, with a
   zero count when unchanged (the `rainExpo` pattern for conditional work).
   This is a cold solve, from phi = 0, of both right-hand sides at once (one
   vec2 of phi per cell), by **geometric multigrid with a FIXED V-cycle
   count**. Levels: 64x32x64 down to 4x2x4. Coarse face beta = the mean of
   the four fine faces it covers. Smoother: red-black Gauss-Seidel. The
   coarse levels from 16x8x16 down fit in ONE workgroup and run in one
   dispatch with `workgroupBarrier` between sweeps, which takes most of the
   dispatch count off the bill.
   **Integer fixed point throughout** (rule 1: the sim reads the output). phi
   is i32 Q16, beta Q4 (sixteenths, exact from the popcount), and the per-cell
   divide is integer. Do not use CG: it needs dot-product reductions, and an
   i32-only reduction of Q16 products overflows.
3. **`draftResolve`.** Converts phi to the per-cell transfer: face flux =
   beta * (e - grad phi). Cell velocity = the mean of the opposite faces'
   fluxes / max(the cell's open fraction, 1/16), so a gap's jet is
   interstitial speed. The result is packed as 6 x i16 Q12 (R_x.xyz, R_z.xyz)
   in 3 words per cell. Solid cells get 0. The outer two cells blend to
   identity (R_x = e_x, R_z = e_z) so the volume edge has no seam.

Record the dispatch count and the GPU time of a solve tick in `perfnodes.h`
(`draftMask`, `draftSolve`). Target: under 1 ms per solve tick at 64x32x64 on
the 3060 Ti, and about 0 on a settled tick.

### 14.5 Readers

- `common.wgsl`: `windAtQ` gains the volume term. The ambient part is
  computed as today, then `draftApplyQ(p, amb)` applies R when p is inside
  the volume. This needs a new **storage binding, `draftField`**: sim group
  binding 46, plus a pass-table R on every consumer row (sim_step,
  sim_particle, sim_fluid) and W on draftResolve. The volume origin and the
  enable word ride TickParams (`draftOrigin[3]`, `draftMode`; the
  `sim.windMode` gate shape: 0 = off, an exact identity, which keeps the old
  hash reachable as a differential). Sim sampling is NEAREST cell (the drift
  bias is a probability; 3 loads). Render sampling is trilinear.
- Render: `windAt` gets the same term, with the binding added to the render
  BGL for `wind_streak.wgsl` and `debug_wind.wgsl`. Streaks stop flying
  through walls, and the F4 arrows show the draft. **raymarch.wgsl sway is
  NOT wired in v1**: its fs has no register headroom (memory: raymarch
  register cliff), and grass indoors is rare. Wire it only if
  `--shader-stats` shows no spill growth. `cloud.wgsl` never needs it.
- C++ mirror: `windfield::Probe` cannot see GPU geometry. The F1 readout
  shows the volume's state instead (solves so far, last solve tick, ms, open
  fraction at the camera). The gate reads `draftField` back.

### 14.6 Tuning

`tuning_params.def` rows, following the CLAUDE.md recipe:
- `sim.draftMode` (0/1)
- `sim.draftVCycles` (fixed count, default 3)
- `sim.draftSmooth` (sweeps per level, default 2)
- `wind.draftOpenVert` (the vertical component's scale)

Grid size stays a `world.h` constant, because it sizes buffers.

### 14.7 Gate `drafts` (`selftest_wind.cpp`, through `support::RunTicks`)

On the harness map, pinned wind (weatherAuto off, 8 m/s along +X, gusts 0),
four floating stone boxes (floating, so terrain stays out of it), interior
3 m x 2.4 m x 3 m, walls 1 voxel thick:

1. sealed: |windIn| at the centre < 5% of ambient;
2. one 8x22-voxel doorway in the windward wall: < 20% at the centre;
3. doorway windward + 6x6 window in the leeward wall: the centre-line flow
   points door to window (dot with +X > 0.7) at >= 30% of ambient;
4. the same box with the window on a SIDE wall: the flow turns toward it.

Then the CA, end to end: release a smoke puff at the centre of boxes 1 and 3
and run 150 ticks. Box 1 keeps >= 90% of its smoke cells inside. Box 3
loses more than half through the window and less than 10% through the door.
Then two more checks:

- **Sleep:** 60 more ticks with nothing changing, and the solve counter does
  not move.
- **Purity:** force a rebuild (bump `draftEpoch`) and require the
  `draftField` readback to be byte-identical.

Expected pass/fail thresholds go in `tests/baseline.json`.
`--gate determinism` covers twice-run equality. The hash MOVES (gas near any
structure in the volume): rebaseline once at the end.

### 14.8 Invariants this adds (DESIGN.md §9b, in the landing commit)

- The draft field is the second stored field in the wind system, and it is
  DERIVED: a pure function of (voxel geometry inside the volume, volume
  origin, tuning), rebuilt in full on any change, never saved, never hashed
  on its own.
- It is geometry ONLY. No wind, weather or tick value enters it, which is
  what lets it sleep.
- The ambient field still never wakes a chunk. The volume reads the dirty
  list and never marks it.

### 14.9 Follow-ups (not v1)

- Several volumes: put a second bounded volume where active gas meets
  structure away from the player (a burning house 60 m off), budget-charged
  like primitives.
- Heat as a pressure source (phase 5): stack effect, burning rooms that vent.
- Wake extension: an upwind-blockage shelter term for the lee beyond the
  potential-flow pocket. Directional, so it needs 4 transfers or a sample-time
  march.
- Porous foliage (a beta per material); raymarch sway inside the volume;
  primitives projected with the geometry.

### 14.10 What shipped, and where it departed from the plan

The model (the transfer, the row masks, geometry-only re-solves, the readers)
shipped as planned. Building and measuring it changed the solver four times.
Each change answered a measurement:

1. **The slab rule.** The first `drafts` run read a sealed hut at 0.26 of the
   outside wind. A 4-voxel cell holding a one-voxel floor or roof is ONE node,
   so the room above the pad and the open air under it were the same air. A
   cell with a complete slab on any axis is now solid. The cost is at most
   0.3 m of air beside a thin slab, and a closed cell takes its open
   neighbours' mean transfer, so smoke under a ceiling still drafts.
2. **The pocket rule.** After the slab rule the sealed hut read 0.05 and a
   one-door hut 0.3, and more iterations changed neither. A dump of the coarse
   potential (`SANDVOX_DRAFT_DUMP=1`) showed a slope of 0.95 per cell where a
   sealed room needs 1.0. The leak was the coarse cell at the roof/wall corner,
   which held a fine cell inside the room and one diagonally outside it,
   joined only through solid. Each coarse cell now keeps its largest air
   pocket (min-label propagation over its fine cells, weighted by open rows)
   and its faces count only fine faces between members. With that, the
   one-door hut read 0.035.
3. **The coarse grid moved from 8 voxels to the chunk (16), into workgroup
   memory.** §14.4's coarse multigrid ran in one workgroup over 16k cells
   through L2 and cost ~20 ms a solve in `--perf --scenario explosion`. It is
   now 16 x 8 x 16 cells in one workgroup's memory, relaxed by integer
   red-black SOR (omega 7/4, 1,024 threads). The pocket and face passes run
   256 groups wide. Because the coarse grid is now too coarse to trust next to
   a door, the fine pass became **four overlapping-tile (Schwarz) passes**,
   each tile's halo ring held at the neighbours' previous answer, so the fine
   field stands on its own. The fine faces are packed once per solve.
4. **The solve is a pipeline**, one stage a tick (pocket + faces, coarse solve,
   four fine passes). A solve starts only when none is in flight, and a burst
   runs every stage at once when the box moves. Whole solves on every changing
   tick cost 668 µs/frame in `explosion` and 2.4 ms in `forestfire`; pipelined
   they cost 166 µs and 538 µs. The price is a lag of up to 6 ticks (0.2 s)
   between a geometry change and the field, plus one piece of history: the
   stage word of a solve in flight. A load or a replay start bursts instead,
   so it sees the current geometry up to 6 ticks earlier than an uninterrupted
   run would. Twice-run determinism is unaffected (`--gate determinism`), and
   a burst reproduces a pipelined result bit for bit (`drafts`' purity check).

Measured (`--gate drafts`):

| Hut / point | Result |
|---|---|
| Sealed | 0.037 |
| Windward door only | 0.035 |
| Door + leeward window | 0.42 along +X |
| Door + side window | turns toward it |
| Alley | 1.27 |
| Open air above the roofs | 1.05 |

Smoke leaves the draft hut out of its leeward window (277 samples to 0), and
smoke moving in the huts never triggers a solve. The pinned determinism hash
did not move: the harness scenario's gas never meets a structure inside the
box.

Still open (§14.9 stands):

- several volumes, for fires away from the player;
- heat as a pressure source;
- wake extension;
- porous foliage;
- grass sway inside the volume, which needs raymarch register headroom;
- an F1 readout of the volume's state (solves, stage, last publish).
