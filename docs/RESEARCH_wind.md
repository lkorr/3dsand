# Wind — architecture decision record

Date: 2026-08-25. Status: **decided, phase 1 not yet started.** This is the plan of
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
| 1 | `windAt` f32 + weather state + sway/strands rewire + **debug slope-field viz + toggle** + `wind.*` knobs | none (render-only) | pinned hash unchanged; viz shows field; grass lean follows direction knob; screenshots |
| 2 | Primitive list + op plumbing (spell VM op, fan tag), cull mask, footprint wake (gated), viz shows primitives | none while gated | gust-bolt op visibly bends grass wake in viz |
| 3 | Debris + MPM wind force; `windResponse`/`windFriction` authoring + packing | none (unhashed systems) | embers/spray drift downwind; twice-run equality holds |
| 4 | `windAtQ` + CA drift bias + entrainment, behind `sim.windMode=0`; then flip commit | rebaseline on flip | smoke drifts, dunes only in storms/footprints, ≤32 chunks at rest still holds |
| 5 | Heat counts → updraft term; violent-wind excite-to-particle; capes when cloth exists | rebaseline (with 4 or after) | fire columns loft smoke/embers; tornado lifts sand |

## 8. Open questions

- MPM wind: all grid nodes vs low-mass-only (current lean: low-mass only).
- Derived (memoryless) heat vs stored accumulator — decide after phase 5 look.
- Altitude ramp reference: absolute world Y vs terrain-relative (lean: absolute
  Y, simplest and deterministic; terrain-relative needs a height query).
- Water wave bands keyed off wind direction (render-only, cheap, do eventually).
- Storm-wake budget shape for ambient dune drift (per-tick chunk cap, surface
  selection heuristic).
