// common.wgsl — prepended to every shader at load time (see resources.cpp).
// All simulation math is integer-only and stateless: determinism is a day-one
// invariant (DESIGN.md §2/§4). Do not introduce floats, atomics-ordering
// dependence, or stateful RNG into anything that feeds voxel state.

// WORLD_N, CHUNK, NCHUNK, NUM_CHUNKS, CHUNK_VOL and VOXEL_METERS are GENERATED
// from src/sim/world.h and prepended ahead of this file by LoadShader
// (see ShaderConstantPrelude in resources.cpp). world.h is the single source of
// truth — do not redeclare them here.

const MAT_AIR : u32 = 0u;

const CLASS_SOLID  : u32 = 0u;
const CLASS_POWDER : u32 = 1u;
const CLASS_LIQUID : u32 = 2u;
const CLASS_GAS    : u32 = 3u;

// Air participates in density ordering: powders/liquids sink through it,
// gases rise through it.
const AIR_DENSITY : i32 = TUNE_AIR_DENSITY;

// Liquids use the state nibble as fullness: code 0..7 = 1..8 eighths
// (mass-conserving flow, DESIGN.md §4). Solids/powders/gases keep the state
// nibble as a palette variant.
const LIQ_FULL_STATE : u32 = 7u;

// Material flags (bitfield).
const MATF_WANDER : u32 = 1u;  // powder scuttles laterally / hops (critters)
const MATF_OPAQUE : u32 = 2u;  // liquid renders as a surface hit (lava), not media
// Static micro-detail: the raymarcher substitutes a subdiv^3 brick for this
// material's cells (see MicroBrick below and trace() in raymarch.wgsl). Set by
// the CPU micro loader, never authored — sim/microvox.h.
const MATF_MICRO  : u32 = 4u;
// Soft vegetation (pond weed, reeds, kelp): moving bodies pass through it.
// A COLLISION property only — the CPU capsule sweep, the mob ground probes and
// spell projectiles read these cells as empty. Everything on the GPU still
// treats them as ordinary solids: the CA runs on them, they burn, the brush and
// explosions remove them, and the renderer draws them as solid geometry. Kept
// here so the flag bits cannot drift from sim/materials.h (kMatFlagPassable).
const MATF_PASSABLE : u32 = 8u;
// ---- wind response, packed into the SAME word (docs/RESEARCH_wind.md §4.5) ----
// `flags` bits 0..7 are the MATF_* booleans above (4 used, 4 spare); bits 8..15
// are two 4-bit AUTHORED numbers, and 16..31 are still free.
//
// They live here rather than in two new fields because `MaterialGpu` is exactly
// 64 bytes with no spare word, and growing a struct every sim thread reads to
// buy eight bits is the worse trade — the same call `stainPack` made. Packing
// into `flags` specifically is safe because every existing reader of it, on both
// sides of the language boundary, tests it with a MASK (`(m.flags & MATF_x) !=
// 0`); not one compares it whole, so a nibble in the high half is invisible to
// all of them.
const MATF_WIND_RESP_SHIFT : u32 = 8u;
const MATF_WIND_FRIC_SHIFT : u32 = 12u;
const MATF_WIND_NIBBLE     : u32 = 0xFu;

struct Material {
  klass       : u32,
  density     : i32,
  color0      : u32,   // RGBA8 palette variants
  color1      : u32,
  color2      : u32,
  emission    : u32,   // 0..255 glow strength (render + media)
  flags       : u32,   // MATF_* in bits 0..7, wind response/friction in 8..15
  tagMask     : u32,   // bit per tag (registry built at load from JSON)
  reactOffset : u32,   // bucket into the reactions[] array
  reactCount  : u32,
  moveEvery   : u32,   // viscosity: only move on ticks where tick % moveEvery == 0
  opacity     : u32,   // 0..255 media absorbance (translucent liquids/gases)
  hardness    : u32,   // 0..255 blast/dig resistance (DESIGN.md §7)
  molten      : u32,   // laser/heat product material (0 = vaporize to air)
  // ---- staining (was _p3/_p4 — no struct growth) ----
  // stainPack: what this material does to what it touches.
  //   bits 0..2   : stain TYPE it applies (0 = this material does not stain)
  //   bits 3..6   : amount added per successful contact, 1..15
  //   bits 7..16  : per-mille chance per tick to stain a touching neighbour
  //   bits 17..26 : per-mille chance that a stain CONSUMES the voxel (to air)
  //   bits 27..30 : ABSORB CAPACITY 0..15 — how much stain THIS material soaks
  //                 up before a liquid pools on top of it. Authored on the
  //                 SUBSTRATE ("absorb"), so it is read off the NEIGHBOUR being
  //                 stained, never off the stainer.
  //   bit  31     : WASHES — this liquid rinses foreign stains out instead of
  //                 overwriting them with its own.
  // stainColor: RGBA8 the renderer composites for THIS material's stain type.
  // Both are authored in materials.json under "stain" (see materials.h).
  stainPack   : u32,
  stainColor  : u32,
};

// stainPack accessors — must match kStainPack* in src/sim/materials.h.
fn matStainType(m : Material)    -> u32 { return m.stainPack & 0x7u; }
fn matStainAmount(m : Material)  -> u32 { return (m.stainPack >> 3u) & 0xFu; }
fn matStainChance(m : Material)  -> u32 { return (m.stainPack >> 7u) & 0x3FFu; }
fn matStainConsume(m : Material) -> u32 { return (m.stainPack >> 17u) & 0x3FFu; }
// Read off the SUBSTRATE, not the stainer: how deep a stain this material will
// take before it is saturated and the liquid has to pool on top instead.
fn matAbsorbCapacity(m : Material) -> u32 { return (m.stainPack >> 27u) & 0xFu; }
fn matAbsorbs(m : Material) -> bool { return ((m.stainPack >> 27u) & 0xFu) != 0u; }
// Does this liquid rinse foreign stains out rather than repaint them?
fn matWashes(m : Material) -> bool { return (m.stainPack & 0x80000000u) != 0u; }
// Does this material stain what it touches at all? One comparison, so the sim
// can reject the overwhelmingly common "no" before doing any other work.
fn matStains(m : Material) -> bool { return (m.stainPack & 0x7u) != 0u; }

// ---- wind coupling, authored per material (invariant 7) --------------------
// Both are 0..15 and both are AUTHORED in materials.json ("wind": {"response":
// n, "friction": n}); materials.cpp derives a default from density when the
// block is absent. Nothing about wind is keyed on a material id anywhere in any
// shader, which is the whole content of invariant 7 — "iron filings blow around
// and pebbles do not" has to be a line in a JSON file, not a branch in here.
//
// RESPONSE is how hard the field pushes this material: 0 = wind does not touch
// it at all (and every consumer below early-outs on that, so the overwhelming
// majority of materials pay one comparison), 15 = it goes where the air goes.
// Physically this is acceleration per unit wind, i.e. ~1/density at a fixed
// voxel size — but real-world susceptibility is area-over-mass, which is SIZE,
// and a uniform grid has erased size. So the derived default is a starting
// point and the authored value is the truth (the Powder Toy's `Advection` is
// hand-tuned per element for exactly this reason).
//
// FRICTION is the entrainment threshold: how hard a per-axis wind has to blow
// before a SETTLED grain of this material is pulled loose (§4.5, saltation).
// It is a different axis from response, not a scaling of it — snow lifts in a
// breeze and then flies far; wet sand needs a gale and then barely moves.
fn matWindResponse(m : Material) -> u32 {
  return (m.flags >> MATF_WIND_RESP_SHIFT) & MATF_WIND_NIBBLE;
}
fn matWindFriction(m : Material) -> u32 {
  return (m.flags >> MATF_WIND_FRIC_SHIFT) & MATF_WIND_NIBBLE;
}

// Can a cell of this material do ANYTHING on its own — move, react, or stain?
//
// This is the sleep predicate (CLAUDE.md rule 2) expressed once, and its two
// callers are deliberately opposite ends of the same statement:
//
//   * sim_step's main() returns immediately when it is false. That return is a
//     no-op refactor of what the kernel already did — a cell with no reaction
//     bucket skips doReactions, a non-staining cell skips doStaining, and a
//     SOLID then hits `if (m.klass == CLASS_SOLID) { return; }` having written
//     nothing. Routing it through this one function is what makes the claim
//     "this cell cannot act" a property of the code rather than of a reading.
//   * worldgen's genChunk refuses to WAKE a chunk in which no cell satisfies
//     it (the streaming wake). Because sim_step provably does nothing for such
//     a chunk, dispatching it or not is bit-identical — which is why the world
//     hash is the gate on that change.
//
// CONSERVATIVE DIRECTION: false must mean "provably inert". Every clause is
// therefore the WIDEST reading of its half — any reaction bucket at all counts
// (not "a bucket with an unconditional rule"), and every non-solid class counts
// whether or not it has anywhere to go. Being woken and doing nothing costs one
// tick; not being woken when you could have acted is frozen matter.
//
// NOT covered here, and not needing to be: a cell CHANGED BY A NEIGHBOUR. The
// CA's write reach is <= 1 cell, so the actor is at most one cell away, its own
// chunk satisfies this predicate, and markDirty marks every chunk that cell
// borders. The passive side is woken by the actor, exactly as it is for every
// other wake in the engine.
fn matCanAct(m : Material) -> bool {
  return m.klass != CLASS_SOLID || m.reactCount > 0u || matStains(m);
}

// Is this a VISCOUS liquid — blood, and anything authored like it?
//
// Render-side classification, and it is deliberately made of AUTHORED DATA
// rather than a material id or a tag bit index. Tag bits are assigned
// first-seen at load, so a shader cannot name one without hardcoding an order
// that materials.json is free to change; opacity and moveEvery are stable
// per-material numbers that already mean exactly the right things:
//   * moveEvery > 1 is the sim's own definition of viscous (the material only
//     flows every Nth tick), which is why lava and blood both carry it.
//   * high opacity separates blood from a thin liquid that happens to be slow.
// The pairing is what makes it specific: lava is viscous AND opaque but is
// MATF_OPAQUE, so it takes the molten path long before this is consulted.
fn isViscousLiquid(m : Material) -> bool {
  return m.klass == CLASS_LIQUID && (m.flags & MATF_OPAQUE) == 0u &&
         m.moveEvery > 1u && m.opacity >= 150u;
}

// Is this a TRANSLUCENT SOLID — ice, glass, and anything authored like them?
//
// Same principle as isViscousLiquid above: authored data, no material ids. The
// signal is `opacity` on a SOLID, which is free to carry this meaning because
// nothing else reads it for solids — the media path only ever consults opacity
// for gases and non-opaque liquids, and every solid defaults to 255. So an
// authored `"opacity": 40` on a solid is unambiguous and every existing solid
// keeps rendering exactly as before.
//
// This is why translucency did NOT need a new Material field: the struct is a
// hard 64 bytes (static_assert in materials.h) and is read by every sim thread,
// so re-using a byte that already means "how much does this absorb" beats
// growing it. The value is absorption per unit depth, not an alpha: it feeds
// Beer-Lambert in shadeTranslucent, so thin ice is nearly clear and a thick
// block is deep cyan from the SAME number.
fn isTranslucentSolid(m : Material) -> bool {
  return m.klass == CLASS_SOLID && m.opacity < 255u;
}

// ---- static micro-detail (docs/PLAN_voxel_editor.md §A, DESIGN.md §9) -------
// A material with MATF_MICRO keeps an ordinary 16-bit world cell — the CA, the
// hash and occupancy all see one plain voxel — and the RAYMARCHER substitutes a
// finer subdiv^3 model when a primary ray lands on it. Grass, flowers and
// foliage are therefore free to simulate, and cost nothing at all when off
// screen.
//
// DETERMINISM (rule 1): this whole path is render-only. `microBricks` and
// `microPool` are bound to the raymarch pipeline and to NOTHING else — binding
// either in a sim shader would put render data on the sim's dependency graph.
// Per-cell yaw/jitter come from hash3(seed, 0, cellIndexW), and the flipbook
// frame is an integer function of `tick`, so every machine draws the same thing
// without any of it being sim state.
//
// Must match struct MicroBrickGpu in src/sim/microvox.h (16 bytes).
struct MicroBrick {
  // Word index into microPool, or MICRO_NONE. Layout at `base`:
  //   [0, frameCount)  cumulative tick offsets: entry i is the tick frame i
  //                    ENDS at within one loop, so the last is the period.
  //   then             frameCount bricks of (subdiv^3 / 4) words, 4 packed
  //                    8-bit palette indices per word, index (z*S + y)*S + x.
  // Palette index == material id, so a micro voxel shades through the ordinary
  // material table with no extra mapping.
  base       : u32,
  subdivLog2 : u32,  // 1, 2 or 3
  frameInfo  : u32,  // bits 0..7 frame count, bits 8..31 loop period in ticks
  flags      : u32,  // MICROF_*
};
const MICRO_NONE : u32 = 0xFFFFFFFFu;
const MICROF_YAW    : u32 = 1u;  // hash-keyed quarter-turn yaw about Y
const MICROF_JITTER : u32 = 2u;  // hash-keyed sub-cell XZ offset
// Render-time wind bend (see traceMicro in raymarch.wgsl). Also keys the
// yaw/jitter identity hash per-COLUMN instead of per-cell, because swaying
// materials are the ones worldgen stacks into multi-cell plants.
const MICROF_SWAY   : u32 = 4u;  // per-column wind bend (render-only)
// Analytic strand plants: no brick, the pool holds parametric blade params and
// traceStrands (raymarch.wgsl) intersects each blade in closed form. Always
// set together with MICROF_SWAY (the plant-extent probes key on it).
const MICROF_STRANDS : u32 = 8u; // parametric blades, no brick

fn microFrameCount(b : MicroBrick) -> u32 { return b.frameInfo & 0xFFu; }
fn microPeriod(b : MicroBrick) -> u32 { return b.frameInfo >> 8u; }

// ---- dynamic microvoxel BODIES (docs/PLAN_voxel_editor.md §C) --------------
// A mob limb authored at "scale": 2|4 keeps its voxels in MICRO units and is
// drawn by rasterizing its oriented bounding box and marching this brick in
// the fragment shader (microbody.wgsl) — one 36-vertex box per limb instead of
// one cube per voxel, so cost tracks screen area rather than voxel count.
//
// Render-only, exactly like MicroBrick above: bound to the microbody pipeline
// and to nothing else. Voxels never change after load, so the record is shared
// by every instance of the def and there is no per-instance storage at all.
//
// Must match struct MicroBodyModelGpu in src/sim/microbody.h (16 bytes).
struct MicroBodyModel {
  // Word index into microBodyPool. Payload is dims.x*dims.y*dims.z micro
  // voxels, 4 packed 8-bit palette indices per word, idx = (z*dy + y)*dx + x.
  // Palette index == material id, so a micro voxel shades through the ordinary
  // material table with no extra mapping.
  base  : u32,
  dims  : u32,  // bits 0..9 x, 10..19 y, 20..29 z (micro voxels)
  scale : u32,  // micro voxels per world voxel: 2 or 4
  _pad  : u32,  // padding to 16 bytes; no flag bits are defined
};

fn microBodyDims(m : MicroBodyModel) -> vec3<i32> {
  return vec3<i32>(i32(m.dims & 0x3FFu), i32((m.dims >> 10u) & 0x3FFu),
                   i32((m.dims >> 20u) & 0x3FFu));
}

// Reaction kinds (bits 0..1 of packed) — DESIGN.md §6, authored in
// assets/materials/reactions.json and compiled per-material at load.
const RK_PAIR  : u32 = 0u;  // self + matching neighbor -> products
const RK_DECAY : u32 = 1u;  // self -> product after probabilistic time
const RK_EMIT  : u32 = 2u;  // self emits product into an adjacent air cell
// Direction mask (bits 2..4 of packed).
const RDIR_DOWN : u32 = 1u;
const RDIR_UP   : u32 = 2u;
const RDIR_SIDE : u32 = 4u;
// Product sentinel: "keep current material".
const PROD_KEEP : u32 = 0xFFFFu;
// nbrMat sentinel: "no exact-id match" (match by tags / class / any instead).
const NBR_ANY : u32 = 0xFFFFu;

struct Reaction {
  packed   : u32,  // bits 0..1 kind, bits 2..4 dir mask
  nbrMat   : u32,  // exact neighbor material id, or NBR_ANY
  nbrTags  : u32,  // neighbor matches if (tagMask & nbrTags) != 0 (when nonzero)
  nbrClass : u32,  // bit-per-class filter (1<<klass); 0 = any class
  chance   : u32,  // odds per 30 Hz tick, in units of 1/REACT_CHANCE_DEN
                   // (authored per-mille, pre-multiplied by the compiler)
  prodSelf : u32,  // what self becomes (PROD_KEEP = unchanged, 0 = air)
  prodNbr  : u32,  // pair: neighbor product; emit: emitted material
  // Light/day-phase gate: bits 0..7 RCOND_*, bits 8..15 min daylight 0..255.
  // Neighbour-count scaling: bits 16..23 RSCALE_*.
  // 0 = unconditional (every rule that predates the day/night cycle).
  cond     : u32,
};
// Reaction light conditions — must match kCond* in src/sim/materials.h.
const RCOND_SKY   : u32 = 1u;  // cell must have open sky above it
const RCOND_DAY   : u32 = 2u;  // only while the sun is up
const RCOND_NIGHT : u32 = 4u;  // only while the sun is down

// Neighbour-count scaling — must match kScale* in src/sim/materials.h.
// Chance scales with how many of the 6 face neighbours match the rule's
// nbrMat/nbrTags/nbrClass predicate; a count of 0 blocks the rule outright.
const RSCALE_ON     : u32 = 0x10000u;  // bit 16: scaling armed
const RSCALE_INVERT : u32 = 0x20000u;  // bit 17: count neighbours NOT matching
const RSCALE_MIN_SHIFT : u32 = 18u;    // bits 18..19: minCount-1, a floor on
const RSCALE_MIN_MASK  : u32 = 0x3u;   // ... the count (0 => any count >= 1)
const RSCALE_MUL_SHIFT : u32 = 20u;    // bits 20..23: max multiplier ...
const RSCALE_MUL_MASK  : u32 = 0xFu;   // ... in quarters, biased by 1.0x
const RSCALE_MUL_UNIT  : u32 = 4u;
// Reaction chance is authored per-mille but rolled in a finer denominator, for
// two reasons. First, a scaled rule authored at chance 1 must still resolve all
// 6 ramp steps instead of truncating them onto 4 — that needs the numerator to
// carry sub-per-mille precision, so SCALE must be a multiple of
// RSCALE_MUL_UNIT*5 (= 20). Second, per-mille bottoms out at one event per 1000
// ticks ~= 33 s at 30 Hz, which is far too frequent for rare-event rules (a
// once-an-hour ambient event, a slow ore vein). SCALE is the reciprocal of the
// finest authorable step: at 2000 a rule can be authored down to 0.0005 per-
// mille, i.e. a mean wait of ~18.5 hours of wall clock.
//
// ReactionGpu.chance is stored ALREADY multiplied by this, so roll sites
// compare `rr % REACT_CHANCE_DEN < rule.chance` with no scaling in the shader.
const REACT_CHANCE_SCALE : u32 = 2000u;
const REACT_CHANCE_DEN   : u32 = 1000u * REACT_CHANCE_SCALE;

struct TickParams {
  tick       : u32,
  seed       : u32,
  opsCount   : u32,
  hashEnable : u32,
  expCount   : u32,  // explosion ops this tick
  page       : u32,  // particle read page (0/1) this tick
  cellCount  : u32,  // exact-cell ops this tick
  genCount   : u32,  // chunks in genList this dispatch (worldgen streaming)
  origin     : vec3<i32>,  // residency window origin, CHUNK units (DESIGN.md §3)
  spawnCount : u32,  // CPU particle spawns this tick (debris shatter)
  farCount   : u32,  // far-field fill entries in farList this tick
  // Integer day phase 0..DAY_PHASE_MASK for this tick, derived from `tick`
  // alone. Gates the daylight reactions, so it is determinism-critical:
  // integer only, and never sourced from frame timing.
  dayPhase   : u32,
  // MLS-MPM fluid: the disturbance-excite switch (sim.fluidExciteMode, read
  // CPU-side each tick like dayPhase — part of the tick input stream, so
  // replays and the determinism gates capture it) and this tick's spawn-op
  // count. The live particle count is GPU-owned now (fluidArgs[FA_LIVE]); the
  // old CPU append base is gone with it.
  fluidExciteEnable : u32,
  fluidSpawnCount   : u32,
  // Material id each MPM species splashes micro droplets as (0 = species was
  // never poured, so it emits none). CPU-owned, recorded from the pour's brush
  // material — blood MPM sprays blood droplets that stain, water sprays water.
  // Part of the tick input stream like every field above, so replays and the
  // determinism gates capture it for free.
  fluidSplashMat  : vec4<u32>,
  // WORLD chunk coord of the 3x3x3 CPU-mirror corner (the same clamp the
  // readback uses). The seam's mirrorFold packs excited-fluid occupancy for
  // these 27 chunks — the swimming query's view of the particles.
  mirrorBase : vec3<i32>,
  // Fluid-lab worldgen mode (world.h kLabSlabY): 1 = genColumn generates the
  // flat stone slab, 0 = normal terrain. Rides the tick stream like dayPhase;
  // always 0 outside --lab/--fluid-bench (it was the padMb pad word).
  labMode    : u32,
  // ---- wind, the SIM's copy (docs/RESEARCH_wind.md §4.2; must match
  // TickParams in world.h) ----
  // The same three weather numbers RenderParams carries, quantised to Q16.16
  // and authored by the SAME C++ function (WindWeather, sim/wind.h) on the same
  // tick. One author is the point: if the renderer and the CA each derived
  // their own weather, grass and smoke would blow different ways in the same
  // frame and it would read as a shader bug.
  //
  // They ride the TICK INPUT STREAM rather than being recomputed on the GPU for
  // the dayPhase reason — a replay reproduces the stream and the twice-run
  // determinism gate compares it, so anything the world hash can see has to
  // arrive this way.
  windDirQ   : vec2<i32>,  // unit XZ pointing DOWNWIND, Q16.16
  windSpeedQ : i32,        // mean speed, Q16.16 world cells/s
  windGustQ  : i32,        // gust band amplitude, Q16.16 world cells/s
  // The gate (sim.windMode; the fluidExciteMode precedent). 0 = no sim kernel
  // reads wind and the pinned world hash cannot move; 1 = particles, MPM and
  // the CA drift bias are live; 2 = also settled-powder entrainment. Read
  // CPU-side per tick, so a per-gate SetCurrentTuning moves it with no pipeline
  // rebuild.
  windMode   : u32,
  // Dev force multipliers, Q8 (256 = 1x), ONE PER TIER — see windAtScaledQ and
  // the drift-bias site in sim_step.wgsl. On the tick stream rather than
  // const-folded like the rest of the wind coupling, because they are dev-panel
  // sliders: a knob you have to press F5 to see is a knob nobody drags.
  windGasScaleQ  : i32,   // CA voxel tier: scales the drift-bias PROBABILITY
  windPartScaleQ : i32,   // particle tier: scales the wind VELOCITY chased
  // Ramp reference for the drag-tier RATE, Q16.16 world cells/s — the wind
  // speed at which sim.windDrag applies in full. See windDragRampQ below.
  windDragRefQ   : i32,
};

// Must match kWindScaleOne / kWindScaleMax in src/sim/world.h.
const WINDQ_SCALE_ONE : i32 = 256;

// sim.windMode ladder — must match kWindMode* in src/sim/world.h.
const WIND_MODE_OFF     : u32 = 0u;
const WIND_MODE_DRIFT   : u32 = 1u;
const WIND_MODE_ENTRAIN : u32 = 2u;

// ---- day phase helpers (integer; sim-side) ----
// 0 = midnight, 0x4000 = sunrise, 0x8000 = noon, 0xC000 = sunset.
const DAY_PHASE_MAX  : u32 = 65536u;
const DAY_PHASE_MASK : u32 = 65535u;
const DAY_SUNRISE : u32 = 16384u;
const DAY_NOON    : u32 = 32768u;
const DAY_SUNSET  : u32 = 49152u;

// Integer "how high is the sun", 0 at the horizon rising to 255 at noon and
// back, 0 all night. A triangle rather than a sine: exact in integers, and the
// reaction chances that key off it only need a monotone daylight strength.
// This is the sim's ONLY notion of sun elevation — the renderer uses its own
// float version, and the two never need to agree bit-for-bit because the
// renderer cannot write voxel state.
fn daylightStrength(phase : u32) -> u32 {
  let p = phase & DAY_PHASE_MASK;
  if (p <= DAY_SUNRISE || p >= DAY_SUNSET) { return 0u; }  // night
  // map sunrise..sunset onto 0..512..0
  let d = p - DAY_SUNRISE;                 // 0 .. 32768
  let half = DAY_NOON - DAY_SUNRISE;       // 16384
  let up = select(2u * half - d, d, d <= half);  // 0..16384 triangle
  return (up * 255u) / half;               // 0..255
}
fn isDaytime(phase : u32) -> bool { return daylightStrength(phase) > 0u; }

struct PassParams {
  colorPhase : vec3<u32>,  // (0..2)^3 — 3x3x3 cell-coloring phase of this pass
  substep    : u32,        // gravity substep (0..SUBSTEPS-1) within the tick
};

// A voxel's "already acted" stamp for tick t, substep s. Movers can act once
// per substep, so things fall SUBSTEPS cells per tick.
//
// WIDTH: 3 bits, cycling 1..7 — NOT a tick counter. The gate only ever asks
// "was this word written during THIS substep?", so the field needs exactly
// enough phases to make the current one distinguishable from any other, not
// enough to count ticks. With 2 substeps the cycle length just has to be
// coprime-ish with nothing in particular; 7 gives every substep in a tick a
// distinct code and leaves 0 free as the sentinel below.
//
// WHY NOT WIDER: it used to be 8 bits (wrapping every 128 ticks), which spent
// 5 bits buying a longer wrap that buys nothing — a stale stamp is either
// STAMP_NEVER or it aliases, and the alias probability is 1/cycle regardless
// of field width. See the sleep note on STAMP_NEVER for the case that made
// this matter.
const STAMP_SHIFT : u32 = 16u;
const STAMP_MASK  : u32 = 0x7u;
const STAMP_BITS  : u32 = 0x70000u;   // bits 16..18
const STAMP_CYCLE : u32 = 7u;
// "Has never acted": no stampFor() output equals this, so a voxel carrying it
// is always free to move. Voxels are BORN with it — worldgen (genCell), RLE
// decode after a stream-in or a load, prefab stamps, brush paints. That is
// what makes narrowing this field safe across sleep: a chunk can sleep for any
// number of ticks and its voxels keep whatever stamp they last had, and on
// wake they are compared against the current one. With a short cycle a whole
// chunk that slept an exact multiple of the cycle would otherwise wake with
// every voxel falsely reading "already acted" and stall for a substep — a
// correlated, visible hitch rather than the 1-in-256 single-voxel skip the
// 8-bit field had. Anything that ENTERS the world unstamped uses this instead
// of a live code, so the alias can only ever cost one voxel one substep.
const STAMP_NEVER : u32 = 0u;
fn stampFor(tick : u32, substep : u32) -> u32 {
  return ((tick * 2u + substep) % STAMP_CYCLE) + 1u;
}

struct BrushOp {
  cx : i32, cy : i32, cz : i32,
  radius   : i32,
  material : u32,
  mode     : u32,   // 0 = paint only into air, 1 = overwrite (erase = overwrite with air)
  _p0 : u32, _p1 : u32,
};

struct RenderParams {
  camPos     : vec3f,  tanHalfFov : f32,
  camRight   : vec3f,  aspect     : f32,
  camUp      : vec3f,  time       : f32,
  camFwd     : vec3f,  flags      : u32,   // bit0 = sun shadows
  sunDir     : vec3f,  fogDensity : f32,   // per meter (pinned to far extent)
  origin     : vec3<i32>,                  // residency window origin, CHUNK units
  viewPx     : f32,    // render target HEIGHT in pixels — pixel angular size
                       // (tanHalfFov*2/viewPx) is the LOD footprint the water
                       // ripple bands are damped against
  // ---- day/night (must match RenderParams in world.h) ----
  moonDir    : vec3f,  // unit vector toward the moon (not just -sunDir)
  dayT       : f32,    // 0..1 phase, 0 = midnight, 0.5 = noon
  sunUp      : f32,    // smoothed 0..1 daylight weight
  moonPhase  : f32,    // 0 = new, 0.5 = full
  starRot    : f32,    // radians the starfield has wheeled
  // ---- static micro-detail (render-only) ----
  // `tick` is the flipbook clock and `seed` keys per-cell yaw/jitter. Both are
  // integers the sim owns, passed in here so the render bind group needs no sim
  // uniform. A flipbook on WALL TIME would run at a different rate per machine
  // and would not reproduce in a replay, which is why it is the tick.
  tick       : u32,
  seed       : u32,
  // Live MPM fluid particle count (0 = none anywhere, so the fluid surface
  // march in raymarch.wgsl is skipped wholesale — rule 2 for the render path).
  fluidCount : u32,
  // ---- second moon + eclipses (must match RenderParams in world.h) ----
  // Moon B is a real second body on its own Keplerian orbit (sim/celestial.*),
  // 9-day synodic period against moon A's 8 — coprime, so the phase pair takes
  // 72 days to repeat.
  eclipseBody : u32,   // 0 none, 1 moon A in front of the sun, 2 moon B
  _pdn0      : u32,
  moon2Dir   : vec3f,  // unit vector toward moon B
  moon2Phase : f32,    // 0 = new, 0.5 = full
  // Apparent angular radii in RADIANS, modulated by orbital distance (perigee
  // is genuinely bigger). The discs and the eclipse test read the same number
  // so they cannot disagree about how large a moon is.
  moonAngRadius  : f32,
  moon2AngRadius : f32,
  // +1/-1: which limb is lit. Without it waxing and waning are the same
  // picture and the terminator flips as a moon passes full.
  moonPhaseSign  : f32,
  moon2PhaseSign : f32,
  // Fraction of the SUN's area occulted right now (circle-circle lens area).
  // 0 = clear. Dims the disc, the dome and the key light together, so a
  // partial eclipse is a partial dimming rather than a switch.
  solarEclipse : f32,
  // Fraction of moon B's disc hidden behind moon A.
  lunarEclipse : f32,
  _pdn1      : f32,
  _pdn2      : f32,
  // The celestial pole in the horizon frame — the axis the starfield wheels
  // about. Derived from latitude on the CPU (see RenderParams in world.h);
  // there is no knob because the stars and the sun must agree on the axis.
  // On its own row: a vec3 aligns to 16 bytes and cannot start mid-row.
  poleDir    : vec3f,
  _pdn3      : f32,
  // ---- MPM fluid render bounds (PLAN_fluid_overhaul.md §7 item 5) ----
  // Inclusive world-voxel AABB of everything the fluid surface march can hit.
  // Rays that miss it skip the march entirely; rays that hit march only the
  // [enter, exit] span. fluidLo > fluidHi on any axis = no fluid at all, which
  // is the post-settle idle state (see the RenderParams block in world.h — the
  // CPU-side fluidCount is monotone, so this box is what actually sleeps).
  fluidLo    : vec3<i32>,
  _pfb0      : i32,
  fluidHi    : vec3<i32>,
  _pfb1      : i32,
  // ---- wind (docs/RESEARCH_wind.md §4.2 — must match RenderParams in
  // world.h) ----
  // The evolving half of the wind field: everything else about it is a TUNE_*
  // constant, but the weather vector drifts over minutes and so has to ride a
  // per-frame uniform. Resolved by ONE C++ function (WindWeather, sim/wind.h)
  // so the automatic weather and the manual override cannot disagree, and so
  // phase 4's TickParams copy has the same author.
  //
  // One whole std140 row: vec2f + 2 x f32.
  windDir    : vec2f,  // unit XZ, pointing DOWNWIND
  windSpeed  : f32,    // mean speed, world cells/s (m/s x 10)
  windGust   : f32,    // gust band amplitude, world cells/s
};

// Reversed-Z depth (clear 0, compare GreaterEqual): depth = KNEAR / viewZ.
// Shared by the raymarcher (frag_depth) and every raster pipeline so raster
// geometry (particles, debris bodies, sprites) composites exactly against the
// raymarched terrain. KNEAR is in voxels.
const KNEAR : f32 = 0.4;

// ---- shared raster-geometry helpers ----------------------------------------
// Everything below is used by BOTH raster body paths — the instanced cubes in
// debris.wgsl and the OBB brick march in microbody.wgsl. They live here rather
// than in either file because the two paths draw the SAME limbs (a live mob's
// arm vs the severed one lying beside it), so a lighting or palette tweak that
// reached only one of them would show as two halves of one creature shading
// differently. A copy cannot enforce that; a shared definition can.

fn quatRotate(q : vec4f, v : vec3f) -> vec3f {
  let t = 2.0 * cross(q.xyz, v);
  return v + q.w * t + cross(q.xyz, t);
}

fn axisUnit(a : u32) -> vec3f {
  if (a == 0u) { return vec3f(1.0, 0.0, 0.0); }
  if (a == 1u) { return vec3f(0.0, 1.0, 0.0); }
  return vec3f(0.0, 0.0, 1.0);
}

fn unpackColor(c : u32) -> vec3f {
  return vec3f(f32(c & 0xFFu), f32((c >> 8u) & 0xFFu), f32((c >> 16u) & 0xFFu)) / 255.0;
}

// The 3-variant palette is a property of Material (declared above), so its
// decode belongs here rather than being re-derived by each render path.
fn paletteColor(m : Material, state : u32) -> vec3f {
  switch (state % 3u) {
    case 0u: { return unpackColor(m.color0); }
    case 1u: { return unpackColor(m.color1); }
    default: { return unpackColor(m.color2); }
  }
}

// ---- shared day/night lighting ---------------------------------------------
// The key light, ambient and tonemap live HERE (not in raymarch.wgsl) because
// the raster body paths must light with the SAME terms as the raymarched
// terrain. Before this the raster paths used fixed daytime constants and bare
// gamma: debris read fine at noon and glowed like a lamp at midnight.
// raymarch.wgsl keeps thin `keyLightColor()`-style wrappers over the *P
// versions so its many call sites stay unchanged (it binds R at module scope;
// these take R as a parameter so any shader can call them).

// Rayleigh scattering is proportional to 1/lambda^4, which is where the sky's
// blue and the sunset's red BOTH come from. These are the relative
// coefficients at 680/550/440 nm, normalized — the ratio is the whole look, so
// they are derived rather than art-directed.
const RAYLEIGH_RGB : vec3f = vec3f(0.1440, 0.3125, 0.7940);

// Relative air mass along a ray leaving the ground at elevation sin(theta) = y.
// 1.0 straight up, ~38 at the horizon. The naive 1/y diverges at y = 0; this is
// the Kasten-Young fit, which stays finite and is accurate to well under a
// percent across the whole range.
fn airMass(y : f32) -> f32 {
  let c = clamp(y, -0.02, 1.0);
  let zdeg = degrees(acos(clamp(c, -1.0, 1.0)));
  return 1.0 / (c + 0.50572 * pow(max(96.07995 - zdeg, 1e-3), -1.6364));
}

// Colour of direct sunlight after crossing `mass` air masses. Blue is removed
// first (Rayleigh again), so the disc and everything it lights goes amber then
// red as it sets. This is ONE function driving the disc, the direct lighting
// term and the horizon glow, so they cannot disagree.
const SUN_TRANSMIT_K : f32 = 0.09;
fn sunTransmittance(mass : f32) -> vec3f {
  return exp(-RAYLEIGH_RGB * mass * SUN_TRANSMIT_K * TUNE_SUN_REDDENING);
}

// ---- how much light each body is actually giving ----------------------------
// One place computes "how much moonlight is this body contributing", so the
// key light, the ambient and the fog tint cannot disagree about which moon is
// up. Falls to zero below the horizon and scales with the illuminated
// fraction squared — a crescent gives far less than half a full moon's light,
// which is why moonlit nights vary so much.
fn moonContribP(mDir : vec3f, mPhase : f32, intensity : f32) -> f32 {
  let up = smoothstep(-0.10, 0.18, mDir.y);
  return up * intensity * (0.15 + 1.70 * mPhase * mPhase);
}

// Daylight weight AFTER an eclipse. R.solarEclipse is the fraction of the
// sun's area a moon covers; TUNE_ECLIPSE_CURVE is the perceptual curve (a
// half-eclipsed sun is barely dimmer to the eye — the collapse is in the last
// few percent). Mirrors dayWeight()/eclipseDim() in raymarch.wgsl; the two
// MUST agree, including the exponent, or the world lights at a different
// brightness than the sky it stands under.
fn eclipseDayWeightP(R : RenderParams) -> f32 {
  let f = clamp(R.solarEclipse, 0.0, 1.0);
  return R.sunUp * (1.0 - pow(f, TUNE_ECLIPSE_CURVE) * TUNE_ECLIPSE_DARKNESS);
}

// Direct light: sun by day, the brighter moon by night. Returns colour x
// intensity; callers multiply by their own N.L / shadow terms.
fn keyLightColorP(R : RenderParams) -> vec3f {
  let sunCol = sunTransmittance(airMass(R.sunDir.y)) * TUNE_SUN_COLOR *
               TUNE_SUN_INTENSITY;
  // Two moons now. The BRIGHTER one is the key light (its direction is what
  // casts the shadows, below); the other is folded into ambient rather than
  // given a second shadowed lambert term, because two sets of soft shadows at
  // moonlight levels costs a whole extra shadow march for something the eye
  // cannot separate at these intensities.
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY);
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY);
  let keyMoon = select(TUNE_MOON2_LIGHT_COLOR * b, TUNE_MOON_LIGHT_COLOR * a,
                       a >= b);
  return mix(keyMoon, sunCol, eclipseDayWeightP(R));
}

// Direction of the key light. A hard switch at sunUp = 0.5 rather than a
// blend: a lerp between two directions would swing shadows wildly through
// twilight, and at the crossover both lights are dim enough to hide the swap.
//
// The same argument picks BETWEEN the two moons — whichever is contributing
// more light owns the shadows, and the swap happens where they are equal and
// therefore each half as bright as the pair, which is the least visible moment
// available. Note this uses the RAW sunUp, not the eclipse-dimmed weight: a
// total eclipse must not swing every shadow in the world round to a moon
// direction (they would be the same direction anyway, and the swing would be
// the most visible thing on screen).
fn keyLightDirP(R : RenderParams) -> vec3f {
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY);
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY);
  let moonDir = select(R.moon2Dir, R.moonDir, a >= b);
  return normalize(mix(moonDir, R.sunDir, step(0.5, R.sunUp)));
}

// Two-tone hemisphere ambient (cool sky above, warm bounce below), scaled to
// a dim blue moon/starlight version at night. Both moons contribute here —
// the secondary one adds real fill on a night when they are both up, which is
// the payoff for having two of them.
fn ambientAtP(n : vec3f, R : RenderParams) -> vec3f {
  let base = mix(TUNE_AMB_GROUND, TUNE_AMB_SKY, n.y * 0.5 + 0.5);
  let nightAmb = mix(TUNE_NIGHT_AMB_GROUND, TUNE_NIGHT_AMB_SKY, n.y * 0.5 + 0.5);
  // Normalised against moon A's own intensity so the existing 0.30/1.40 ramp
  // (tuned when there was one moon) still means the same thing when only A is
  // up, and B can only ever ADD to it.
  let inv = 1.0 / max(TUNE_MOON_LIGHT_INTENSITY, 1e-4);
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY) * inv;
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY) * inv;
  let moonAmt = 0.30 * step(0.001, a + b) + 1.40 * (a + b) * 0.5;
  return mix(nightAmb * (0.45 + moonAmt), base, eclipseDayWeightP(R));
}

// Reinhard-with-white-point applied to LUMINANCE then reapplied to the colour
// (per-channel Reinhard desaturates: saturated ember orange comes out tan),
// with a controlled per-channel blend at the very top so genuinely hot cores
// bleach toward white like real blackbody progression. Includes the output
// gamma — this is the LAST thing a fragment shader does. See the long-form
// rationale where this lived originally (raymarch.wgsl fs history).
fn tonemapHdr(colorIn : vec3f) -> vec3f {
  const WHITE : f32 = TUNE_EXPOSURE_WHITE;
  let color = max(colorIn, vec3f(0.0));
  let lum = max(dot(color, vec3f(0.2126, 0.7152, 0.0722)), 1e-5);
  let mapped = (lum * (1.0 + lum / (WHITE * WHITE))) / (1.0 + lum);
  let hueKept = color * (mapped / lum);
  let perCh = (color * (1.0 + color / (WHITE * WHITE))) / (1.0 + color);
  let bleach = clamp(mapped * mapped * mapped * TUNE_BLEACH_AMOUNT, 0.0,
                     TUNE_BLEACH_AMOUNT);
  return pow(mix(hueKept, perCh, bleach), vec3f(1.0 / TUNE_GAMMA));
}

// Key-light lambert + hemisphere ambient + emissive + distance fog for raster
// geometry, in the same linear HDR space as the terrain. Callers MUST run the
// result through tonemapHdr() — bare gamma clips the highlights and drifts
// from the raymarched look in both directions.
fn litColor(albedo : vec3f, n : vec3f, worldPos : vec3f, emission : f32,
            R : RenderParams) -> vec3f {
  let lambert = max(dot(n, keyLightDirP(R)), 0.0);
  var c = albedo * (ambientAtP(n, R) + keyLightColorP(R) * lambert);
  c += albedo * emission * 1.7;
  let dist = length(worldPos - R.camPos);
  let fog = 1.0 - exp(-dist * VOXEL_METERS * 0.0128);
  // cheap sky tint for fog, dimmed through the night like the real sky (a
  // fixed day-blue tint here was a second source of midnight glow). Both moons
  // count, and an eclipse dims it with everything else.
  let inv = 1.0 / max(TUNE_MOON_LIGHT_INTENSITY, 1e-4);
  let moonLit =
      (moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY) +
       moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY)) * inv;
  let fogTint = vec3f(0.55, 0.65, 0.85) *
                mix(0.015 + 0.05 * moonLit, 1.0, eclipseDayWeightP(R));
  return mix(c, fogTint, fog);
}

// Emissive voxels (embers on burning debris, lava) pulse rather than sitting at
// a flat value. `hash` is a per-voxel phase key so neighbours don't blink in
// lockstep; every raster path must use the SAME rate or a severed limb flickers
// out of step with the corpse it came off.
fn emberFlicker(emission : f32, hash : u32, time : f32) -> f32 {
  if (emission <= 0.0) { return emission; }
  return emission * (0.82 + 0.28 * sin(time * 9.0 + f32(hash & 0xFFu) * 0.0245));
}

// Camera-basis projection for raster geometry — no matrices; identical math to
// the raymarcher's ray construction, so depth agrees across both paths.
fn projectView(rel : vec3f, R : RenderParams) -> vec4f {
  let vx = dot(rel, R.camRight);
  let vy = dot(rel, R.camUp);
  let vz = dot(rel, R.camFwd);
  return vec4f(vx / (R.tanHalfFov * R.aspect), vy / R.tanHalfFov, KNEAR, vz);
}

// ============================== WIND FIELD ==================================
// docs/RESEARCH_wind.md is the plan of record; DESIGN.md §12 states the
// invariants. Wind is a PURE FUNCTION of (world position, time). There is no
// per-chunk vector, no per-voxel wind bits, no relaxation pass — and therefore
// nothing to save, hash, stream, replicate or wake (invariant 1). The field
// costs only where it is sampled, so an unsampled world pays nothing, which is
// how a wind system obeys rule 2 without a sleep mechanism of its own.
//
// THIS IS THE ONE IMPLEMENTATION (invariant 2). Phase 1's consumers are the
// two foliage sway sites in raymarch.wgsl and the arrow overlay in
// debug_wind.wgsl; phases 2-5 add primitives, particle/MPM forces and an
// integer `windAtQ` for the CA. A consumer that builds its own bands is a bug,
// not an optimisation: the whole illusion is that everything is standing in
// the SAME wind (Sucker Punch's "volume over accuracy" — one shared input,
// sampled consistently, sells it, and no solver is needed). The debug overlay
// is only evidence BECAUSE it calls the same function the grass does.
//
// UNITS. The field is a VELOCITY in world CELLS PER SECOND. kVoxelMeters is
// 0.10, so cells/s = m/s x 10, and the m/s knobs are converted once on the CPU
// (src/sim/wind.h). Positions are world voxels; `t` is seconds.
//
// COMPOSITION (research doc §4.1):
//     windAt(p, t) = (mean + gustBands(p, t)) * altRamp(p.y)
// The altitude ramp scales the WHOLE field, mean included, because wind aloft
// is faster wind rather than the same wind with bigger gusts. §4.1's updraft
// and primitive terms are phases 5 and 2; they add in here.

// Gust bands are ANISOTROPIC about the wind direction: the along-wind
// component gets the full band and the crosswind component 60% of it, so the
// motion traces an ellipse with a dominant axis. That is what separates wind
// from jiggle. It is also the one thing the sway code this was promoted from
// had HARDCODED ("X leads, Z trails at ~60%") — here it is a projection onto
// the weather vector instead, which is precisely why turning windDirDeg turns
// the grass.
const WIND_GUST_CROSS : f32 = 0.6;
// The vertical band. Gust fronts have genuine updraft/downdraft structure, and
// without it the debug arrow field is a flat sheet that says nothing about the
// third dimension. Foliage ignores it — a blade bends sideways, not upward.
const WIND_GUST_VERT : f32 = 0.18;
// Band mix for a consumer with no opinion (brick sway, the debug field). The
// strand path overrides these PER BLADE, and that per-blade disagreement is
// what decorrelates neighbours; see the note at its call site.
const WIND_BAND_W1 : f32 = 0.7;
const WIND_BAND_W2 : f32 = 0.3;
// Radians of gust phase per world cell travelled downwind: 2pi / wavelength,
// with the wavelength authored in METRES. The guard keeps a wavelength typo
// from producing an infinite spatial frequency (which aliases to noise).
const WIND_GUST_K : f32 =
    6.28318531 / max(TUNE_WIND_GUST_WAVELENGTH / VOXEL_METERS, 1.0);
// Wind speed corresponding to the AUTHORED foliage bend amplitude
// (TUNE_MICRO_SWAY_AMP, in sub-voxels). Sway is a displacement and wind is a
// velocity, so something has to relate them; putting the reference here rather
// than in a knob is deliberate — it is a calibration of the existing authored
// amplitude, not a thing to tune. 120 cells/s = 12 m/s, chosen so the DEFAULT
// windSpeed + gustStrength reproduce the peak bend the sway code shipped with.
const WIND_SWAY_REF : f32 = 120.0;

// One evaluation of the field, kept in its component parts. The parts exist
// because the strand path must re-weight the GUSTS per blade without
// re-weighting the MEAN (every blade stands in the same average wind, they
// only disagree about the gusts) — and doing that through this struct is what
// keeps it the same arithmetic as windAt() instead of a second field.
struct WindSample {
  along : vec2f,   // unit XZ, downwind
  crossw: vec2f,   // unit XZ, 90 degrees to the left of `along`
  mean  : vec2f,   // XZ mean wind, cells/s, altitude ramp applied
  b1    : vec3f,   // band 1 as (along, cross, up), unit-ish amplitude
  b2    : vec3f,   // band 2, same frame
  amp   : f32,     // gust amplitude, cells/s, altitude ramp applied
};

// Altitude gain. altitudeGain is the fractional speed-up per 100 world voxels
// (10 m) above altitudeRefY, so it is signed: below the reference the boundary
// layer slows the wind down. Clamped at both ends because a knob is allowed to
// be silly and a negative or exploding wind is not a look, it is a bug report.
fn windAltRamp(y : f32) -> f32 {
  return clamp(1.0 + TUNE_WIND_ALT_GAIN * (y - TUNE_WIND_ALT_REF_Y) * 0.01,
               0.15, 4.0);
}

// `ph` is the consumer's per-instance phase scatter (per grass column, per
// blade). It is a decorrelation offset, NOT part of the field: windAt passes
// 0.0, which is what the debug overlay draws and what "the wind at this point"
// means. Keeping it a parameter rather than baking a hash in here is what lets
// the sway sites keep the exact scatter they were tuned with.
fn windSampleAt(p : vec3f, t : f32, ph : f32, R : RenderParams) -> WindSample {
  var s : WindSample;
  // R.windDir is a unit XZ vector resolved on the CPU. WindWeather()
  // (src/sim/wind.h) is its ONLY author — auto weather and the manual override
  // come out of the same function, and phase 4's TickParams copy will too, so
  // the sim and the renderer cannot end up in different weather.
  let d = R.windDir;
  s.along = d;
  s.crossw = vec2f(-d.y, d.x);
  let alt = windAltRamp(p.y);
  s.mean = d * (R.windSpeed * alt);
  s.amp = R.windGust * alt;
  // Travelling gust phase: distance DOWNWIND, in radians. Phase as a function
  // of world position is what makes a gust FRONT cross a meadow instead of the
  // whole field breathing as one (Crysis / GPU Gems 3 ch.16, and the same
  // trick the global colour lattice plays). Measuring it along `along` rather
  // than on a fixed axis is what makes the fronts travel with the wind.
  let gp = dot(p.xz, d) * WIND_GUST_K;
  let tt = t * TUNE_WIND_GUST_SPEED;
  // Two incommensurate bands: a slow whole-field breath plus a faster flutter,
  // never periodic together. These four rates and the phase constants are the
  // ones the sway code shipped with — they ARE the look, do not tidy them.
  s.b1 = vec3f(sin(tt + gp + ph),
               sin(tt * 0.83 + gp * 1.2 + ph + 2.1) * WIND_GUST_CROSS,
               sin(tt * 1.31 + gp * 0.7 + ph * 2.3) * WIND_GUST_VERT);
  s.b2 = vec3f(sin(tt * 1.73 + gp * 0.5 + ph * 3.1),
               sin(tt * 2.19 + ph * 1.7) * WIND_GUST_CROSS,
               sin(tt * 2.61 + gp * 0.3 + ph) * WIND_GUST_VERT);
  return s;
}

// One band, rotated out of the (along, cross, up) frame into world space and
// scaled to cells/s.
fn windBandWS(s : WindSample, b : vec3f) -> vec3f {
  let xz = s.along * b.x + s.crossw * b.y;
  return vec3f(xz.x, b.z, xz.y) * s.amp;
}

// The mean, in world space. Kept separate from the bands for the reason in the
// WindSample note above.
fn windMeanWS(s : WindSample) -> vec3f {
  return vec3f(s.mean.x, 0.0, s.mean.y);
}

// THE FIELD. Everything above exists so that this and the per-blade path are
// the same arithmetic. Call this unless you need per-instance decorrelation.
fn windAt(p : vec3f, t : f32, R : RenderParams) -> vec3f {
  let s = windSampleAt(p, t, 0.0, R);
  return windMeanWS(s) + windBandWS(s, s.b1) * WIND_BAND_W1
                       + windBandWS(s, s.b2) * WIND_BAND_W2;
}

// THE FIELD, scaled by a dev multiplier. Used by the PARTICLE TIER (ballistic
// debris and spray in sim_particle.wgsl, MPM grid nodes in sim_fluid.wgsl),
// where "more wind" straightforwardly means "a faster wind to be dragged
// toward", so scaling the velocity is the whole story.
//
// The CA tier does NOT use this and the asymmetry is deliberate, not an
// oversight: its drift bias is a PROBABILITY that saturates at windDriftMax
// once the field passes windDriftSpeed, which the default weather already
// nearly does. Scaling the velocity there would move the slider for about the
// first 2x and then do nothing at all, and a control that goes dead halfway is
// worse than no control. So sim_step scales the probability instead, past its
// own cap, up to certainty. Two tiers, two quantities, one meaning: "how hard
// does the wind push THIS".
//
// The == comparison is an exact-identity guard, not an optimisation. At the
// shipping 1x it returns windAtQ untouched, so "the slider is at 1x" and "the
// world hash is the pinned one" are the same statement. Scaling first and
// multiplying second is required, not stylistic: the field is Q16.16 cells per
// second and reaches ~2^26 in a storm, so the multiply has to come after the
// divide or it leaves i32 at the top of the range.
fn windAtScaledQ(p : vec3<i32>, T : TickParams, scaleQ8 : i32) -> vec3<i32> {
  let w = windAtQ(p, T);
  if (scaleQ8 == WINDQ_SCALE_ONE) { return w; }
  return vec3<i32>((w.x / WINDQ_SCALE_ONE) * scaleQ8,
                   (w.y / WINDQ_SCALE_ONE) * scaleQ8,
                   (w.z / WINDQ_SCALE_ONE) * scaleQ8);
}

// How much of a tier's authored drag RATE actually applies in the air at this
// sample: Q16, 0 in dead calm and 65536 at T.windDragRefQ and above. One
// definition, because both drag sites (sim_particle.wgsl, sim_fluid.wgsl) must
// answer this the same way or "the wind is calm" would mean two things.
//
// WHY THE RATE RIDES THE WIND AT ALL. A drag law pulls velocity toward the
// local air on EVERY axis, so with a horizontal field the vertical target is
// zero and the term is pure air resistance: terminal fall = gravity/rate,
// which at the authored windDrag 3 is 0.86 vox/tick against a ballistic cap of
// 6. Turning wind on therefore made everything in the world fall at a seventh
// of its old speed, and turning the dev multiplier to 0 made it WORSE (zero
// field, undiminished rate = drag toward a standstill on all three axes).
// That is not a tuning value, it is the wrong shape: real drag scales with
// relative airspeed, and a fixed rate models an atmosphere that resists motion
// just as hard when nothing is blowing.
//
// Ramping the rate by the wind magnitude puts ballistic flight back as the
// LIMIT rather than as a special case: at the default 6 m/s weather the
// terminal fall is 5.7 vox/tick (visually the old behaviour), at a 40 m/s
// storm it is the 0.86 that a storm should mean. The dev multiplier falls out
// for free, because the vector passed in is the SCALED field — at 0x the
// magnitude is 0, the ramp is 0, and the whole term vanishes with no
// `if (scale == 0)` anywhere.
//
// CHEBYSHEV, not a true length: the components reach ~2^26 in a storm and
// squaring them leaves i32. Max-abs is within 1.7x, monotone, and exact.
// The divide is by cells/s (refQ >> 16) so the Q16.16 numerator lands directly
// in Q16 — full precision at a whisper, where a >>10 on both sides would
// quantise the ramp into visible steps.
fn windDragRampQ(w : vec3<i32>, T : TickParams) -> i32 {
  let mag = max(max(abs(w.x), abs(w.y)), abs(w.z));
  return min(65536, mag / max(T.windDragRefQ >> 16u, 1));
}

// Wind velocity -> foliage bend, in the units TUNE_MICRO_SWAY_AMP is authored
// in. Dividing by a fixed reference (rather than normalising) is what keeps a
// retuned windSpeed from throwing blades clean out of their own cell: the bend
// stays proportional to the wind, and the authored amplitude stays the ceiling
// it was tuned to be.
fn windSway(v : vec3f) -> vec2f { return v.xz * (1.0 / WIND_SWAY_REF); }

// ======================= THE FIELD, IN INTEGERS (windAtQ) ===================
// docs/RESEARCH_wind.md §4.1. Everything above is f32 and is read by the
// renderer, which is allowed to be approximate because it cannot write a voxel.
// Everything BELOW is read by sim kernels whose output reaches the grid — the
// ballistic particles reinsert themselves as voxels, MPM settles back through
// the excite seam, and phase 4's drift bias steers the CA outright — so it obeys
// rule 1: integer only, no f32 anywhere in the evaluation, sine included.
//
// WHY NOT JUST CALL windAt(). Because f32 `sin()` is not bit-identical between
// GPU vendors (nor between drivers), and the world hash is compared across
// machines. One ulp of disagreement in a gust would, over a few thousand ticks,
// put a grain of sand in a different cell on two machines, and the pinned hash
// exists to catch exactly that. This is the same reason sim_fluid.wgsl has
// isqrtI() instead of sqrt().
//
// SAME FIELD, NOT A SECOND ONE (invariant 2). Every term below is the integer
// transcription of the identically-named f32 term above: the same two bands, the
// same four incommensurate rates, the same phase constants, the same anisotropy,
// the same altitude ramp, the same 0.7/0.3 mix. It is transcribed rather than
// shared because the two number systems cannot share code, and the ONLY defence
// against them drifting apart is that they sit here, adjacent, in one file. If
// you change a rate above, change it below in the same edit.
//
// AGREEMENT IS APPROXIMATE, AND THAT IS THE DESIGN. The integer field tracks the
// float one to well under a percent, which is what "the sand blows the way the
// grass leans" needs; it is not, and does not need to be, the same number. Two
// deliberate, bounded divergences:
//   * the gust CLOCK. windAt() is driven by R.time (wall seconds, so foliage
//     animates smoothly between ticks); windAtQ is driven by T.tick. Tick debt
//     makes those drift apart slowly over a session, so the crest the grass
//     shows and the crest a grain feels are the same crest to within a fraction
//     of a wavelength, not to the bit.
//   * quantisation. The per-cell gust gradient is rounded to whole BAM units and
//     wq() keeps 8 fractional bits, so the pattern is right and its last digit
//     is not.
//
// UNITS. Q16.16 world cells per SECOND, matching windAt() exactly, so the two
// can be compared directly by anyone debugging them. Each consumer converts to
// its own per-tick/per-substep units at its own call site with a const factor.
// Angles are BAM (binary angle measure): 65536 = one full turn = 2*pi. That is
// what makes the modular reduction below exact — see windPhaseQ.

// Q16.16 fixed-point multiply for the wind block. Eight bits of each operand's
// fraction go before the multiply, which is what lets it take a storm-force
// wind (Q16.16 of ~700 cells/s, 26 bits) against the 4x altitude ramp without
// leaving i32 — a range sim_fluid's mq() staging cannot cover. The lost
// precision is ~0.4% of a term, i.e. under a tenth of a degree of direction and
// invisible in a field whose whole point is that it wanders.
//
// Computed on MAGNITUDES with the sign restored, for the reason mq() documents
// at length: an arithmetic-shift truncation rounds toward -inf, which biases
// every product slightly negative, and a field of such products reads as a
// permanent drift toward -x/-z that no knob explains.
fn wq(a : i32, b : i32) -> i32 {
  let m = (abs(a) >> 8u) * (abs(b) >> 8u);
  return select(m, -m, (a ^ b) < 0);
}

// Integer sine. Input: BAM (65536 = 2*pi), any i32, wrapped here. Output:
// Q16.16 in [-65536, 65536].
//
// Two-term polynomial rather than a lookup table, because a table has to be
// indexed dynamically and a WGSL `const` array cannot be; a `var<private>`
// table would be mutable state in a kernel that must not have any. The shape is
// the standard parabola-plus-correction: sin(pi*u) ~= 4u(1-|u|), then one
// Newton-ish pass y += 0.225*(y|y| - y). Worst-case error ~0.1% of full scale,
// which is a hundredth of the gust quantisation and therefore free.
//
// Every intermediate is bounded by construction: |x| <= 65536, and the peak of
// x*(65536-|x|) is at |x| = 32768 where it is 2^30 — inside i32 with a bit to
// spare, so no staging shift is needed and the sine keeps its full precision.
fn windSinQ(a : i32) -> i32 {
  // Fold to [-pi, pi): (a mod 2pi) - pi, so the parabola's symmetric form
  // applies. The pi shift is undone by the negation at the end (sin(t-pi) =
  // -sin(t)) rather than by a branch on the quadrant.
  let x = ((a & 65535) - 32768) * 2;    // u in [-1, 1) as Q16.16
  let ax = abs(x);
  let par = (ax * (65536 - ax)) / 65536;
  var y = 4 * select(par, -par, x < 0);
  let ay = abs(y);
  let sq = select((ay * ay) / 65536, -((ay * ay) / 65536), y < 0);
  y = y + (14746 * (sq - y)) / 65536;   // 0.225 in Q16.16
  return -y;
}

// Gust phase at a world cell, in BAM: the distance travelled DOWNWIND times the
// spatial frequency, which is what makes a gust front cross a meadow instead of
// the whole field breathing at once (windSampleAt's note; Crysis / GPU Gems 3).
//
// `k` is the frequency in BAM per cell, already scaled by the band's own gp
// coefficient. Folding the coefficient into `k` rather than multiplying the
// phase afterwards is REQUIRED, not tidier: the phase is reduced mod 2^16 here,
// and (x mod m)*c is not (x*c) mod m unless c is 1. Getting that wrong shows up
// as a gust pattern that jumps discontinuously every 65536 cells, which is a
// long way from anywhere anyone tests.
//
// The reduction itself is exact because u32 arithmetic wraps mod 2^32 and
// 65536 divides 2^32, so masking a wrapped product to 16 bits gives the true
// product mod 65536 — including for negative world coordinates, whose two's
// complement bit pattern is already their residue.
fn windPhaseQ(p : vec3<i32>, d : vec2<i32>, k : i32) -> u32 {
  let kx = (d.x * k) / 65536;
  let kz = (d.y * k) / 65536;
  return (u32(p.x) * u32(kx) + u32(p.z) * u32(kz)) & 65535u;
}

// Band clock, in BAM. `k` is BAM-per-tick in Q16 (so the slow bands keep their
// rate instead of rounding to zero), and the u32 multiply's wrap at 2^32 IS the
// modular reduction: 2^32 is exactly one turn in these units, so a session can
// run for as long as it likes without the phase ever needing a fixup.
fn windClockQ(tick : u32, k : u32) -> u32 { return (tick * k) >> 16u; }

// Radians -> BAM, at const-eval only.
const WINDQ_RAD : f32 = 65536.0 / 6.28318531;
// Ticks -> BAM, Q16, per unit rate: one second is 30 ticks.
const WINDQ_HZ : f32 = 4294967296.0 / (6.28318531 * 30.0);
// The gust clock rate, clamped where it is READ rather than where it is
// authored, because these constants are const-eval'd from tuning.json and a
// hand-edited file must not be able to produce an out-of-range u32 conversion
// (which is a shader compile error, i.e. a black screen, not a bad look).
const WINDQ_RATE : f32 = clamp(TUNE_WIND_GUST_SPEED, 0.0, 64.0);
// The six band rates of windSampleAt, transcribed. Do not tidy them: they are
// deliberately incommensurate so the two bands never come back into phase.
const WINDQ_T_100 : u32 = u32(WINDQ_RATE * 1.00 * WINDQ_HZ);
const WINDQ_T_083 : u32 = u32(WINDQ_RATE * 0.83 * WINDQ_HZ);
const WINDQ_T_131 : u32 = u32(WINDQ_RATE * 1.31 * WINDQ_HZ);
const WINDQ_T_173 : u32 = u32(WINDQ_RATE * 1.73 * WINDQ_HZ);
const WINDQ_T_219 : u32 = u32(WINDQ_RATE * 2.19 * WINDQ_HZ);
const WINDQ_T_261 : u32 = u32(WINDQ_RATE * 2.61 * WINDQ_HZ);
// Spatial frequency, BAM per cell travelled downwind = WIND_GUST_K in BAM.
// Capped at 27000 so that the largest gp coefficient (1.2) still leaves
// dirQ * k inside i32; the cap is ~10x above the tuner's shortest authorable
// wavelength, so it never engages in practice.
const WINDQ_K_BASE : i32 = min(
    i32(round(65536.0 / max(TUNE_WIND_GUST_WAVELENGTH / VOXEL_METERS, 1.0))),
    27000);
const WINDQ_K_100 : i32 = WINDQ_K_BASE;
const WINDQ_K_120 : i32 = i32(round(f32(WINDQ_K_BASE) * 1.2));
const WINDQ_K_070 : i32 = i32(round(f32(WINDQ_K_BASE) * 0.7));
const WINDQ_K_050 : i32 = i32(round(f32(WINDQ_K_BASE) * 0.5));
const WINDQ_K_030 : i32 = i32(round(f32(WINDQ_K_BASE) * 0.3));
// The 2.1-radian offset on band 1's cross component, in BAM.
const WINDQ_PH_21 : i32 = i32(round(2.1 * WINDQ_RAD));
// Anisotropy and band mix — WIND_GUST_CROSS / WIND_GUST_VERT / WIND_BAND_W1 /
// WIND_BAND_W2 above, in Q16.16.
const WINDQ_CROSS : i32 = i32(round(0.6 * 65536.0));
const WINDQ_VERT  : i32 = i32(round(0.18 * 65536.0));
const WINDQ_W1    : i32 = i32(round(0.7 * 65536.0));
const WINDQ_W2    : i32 = i32(round(0.3 * 65536.0));
// Altitude ramp, windAltRamp() in integers: 1.0 + gain * (y - refY) / 100,
// clamped to the same [0.15x, 4x].
const WINDQ_ALT_GAIN  : i32 = i32(round(TUNE_WIND_ALT_GAIN * 0.01 * 65536.0));
const WINDQ_ALT_REF_Y : i32 = i32(round(TUNE_WIND_ALT_REF_Y));
const WINDQ_ALT_MIN   : i32 = 9830;     // 0.15 in Q16.16
const WINDQ_ALT_MAX   : i32 = 262144;   // 4.0

// THE FIELD, for the sim. Q16.16 world cells per second at world cell `p`.
//
// Returns the ZERO vector when sim.windMode is off, which is what makes phase 1
// through 4 hash-neutral by construction — but do NOT rely on that alone at a
// call site: a drag term reading zero wind still drags. Every consumer gates on
// T.windMode itself, and says so.
//
// COST. Six integer sines and ~30 multiplies, about 150 integer ops. That is
// heavy next to a gravity add and cheap next to the neighbourhood loads the
// callers already did; it is only ever paid inside the movement tail of an
// already-awake chunk, and never at all with the gate off. If it ever profiles
// hot in the CA, the contained fallback is research doc §3's per-chunk cache —
// one evaluation per chunk per tick, the God of War shape — NOT a cheaper field.
fn windAtQ(p : vec3<i32>, T : TickParams) -> vec3<i32> {
  if (T.windMode == WIND_MODE_OFF) { return vec3<i32>(0, 0, 0); }
  let alt = clamp(65536 + (p.y - WINDQ_ALT_REF_Y) * WINDQ_ALT_GAIN,
                  WINDQ_ALT_MIN, WINDQ_ALT_MAX);
  let d = T.windDirQ;                    // unit XZ, downwind, Q16.16
  let cw = vec2<i32>(-d.y, d.x);         // 90 degrees to its left
  let spd = wq(T.windSpeedQ, alt);
  let amp = wq(T.windGustQ, alt);

  // Band components in the (along, cross, up) frame. Five spatial phases and
  // six clocks, exactly as windSampleAt builds them with ph = 0.
  let b1a = windSinQ(i32(windPhaseQ(p, d, WINDQ_K_100) +
                         windClockQ(T.tick, WINDQ_T_100)));
  let b1c = wq(WINDQ_CROSS,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_120) +
                            windClockQ(T.tick, WINDQ_T_083)) + WINDQ_PH_21));
  let b1u = wq(WINDQ_VERT,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_070) +
                            windClockQ(T.tick, WINDQ_T_131))));
  let b2a = windSinQ(i32(windPhaseQ(p, d, WINDQ_K_050) +
                         windClockQ(T.tick, WINDQ_T_173)));
  let b2c = wq(WINDQ_CROSS, windSinQ(i32(windClockQ(T.tick, WINDQ_T_219))));
  let b2u = wq(WINDQ_VERT,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_030) +
                            windClockQ(T.tick, WINDQ_T_261))));

  // Rotate each band out of that frame into world space and scale by the gust
  // amplitude — windBandWS(), in integers.
  let w1 = vec3<i32>(wq(amp, wq(d.x, b1a) + wq(cw.x, b1c)),
                     wq(amp, b1u),
                     wq(amp, wq(d.y, b1a) + wq(cw.y, b1c)));
  let w2 = vec3<i32>(wq(amp, wq(d.x, b2a) + wq(cw.x, b2c)),
                     wq(amp, b2u),
                     wq(amp, wq(d.y, b2a) + wq(cw.y, b2c)));
  // windMeanWS() + the 0.7/0.3 mix.
  return vec3<i32>(wq(d.x, spd), 0, wq(d.y, spd)) +
         vec3<i32>(wq(WINDQ_W1, w1.x), wq(WINDQ_W1, w1.y), wq(WINDQ_W1, w1.z)) +
         vec3<i32>(wq(WINDQ_W2, w2.x), wq(WINDQ_W2, w2.y), wq(WINDQ_W2, w2.z));
}

// ---- particles: voxels in flight (DESIGN.md §5) ----
// All particle state is fixed-point integer (24.8, 1 voxel = 256): the
// particle system is part of the deterministic sim, so the float ban applies.
// Behavior is keyed ONLY on particle state + tick (never on buffer slot), so
// the scheduling-dependent append ORDER of the ring cannot affect the grid.
const PARTICLE_CAP  : u32 = 262144u;
const CLAIM_SIZE    : u32 = 262144u;   // reinsertion claim hash (power of two)
const PART_ONE      : i32 = 256;       // 1.0 voxel in fixed point
const PART_GRAVITY  : i32 = TUNE_PART_GRAVITY;        // 9.81 m/s^2 in voxels/tick^2, fixed point
const PART_MAX_VEL  : i32 = TUNE_PART_MAX_VEL;      // 6 voxels/tick terminal speed
const PFLAG_ALIVE   : u32 = 1u;
const PFLAG_PENDING : u32 = 2u;        // proposed a reinsertion this tick

// ---- MICROVOXEL particles (sub-voxel spray) --------------------------------
// A micro particle is a particle that is SMALLER than a grid cell, so the grid
// has nowhere to put it. That single fact drives everything else about it:
//
//   * It NEVER reinserts as a voxel. On contact it either vanishes or, if its
//     material stains (materials.json `stain`), it deposits that stain into the
//     surface it hit. Reinserting would round a 1/4-voxel droplet up to a whole
//     voxel and manufacture matter — a mob would bleed out more blood than it
//     could hold, and a spray of a hundred droplets would tile a wall solid.
//   * It carries a LIFETIME. Ordinary particles are conserved matter and are
//     entitled to persist; spray is an effect and must die down on its own or
//     a fight leaves the ring permanently full (rule 2 — a settled world must
//     cost nothing, and "settled" has to include "after the blood dried").
//
// Both live in spare bits of `flags`, so the 32-byte Particle does not grow:
//   bit 2      PFLAG_MICRO   — this is a micro particle
//   bits 3..4  scale index   — 0..3 maps to MICRO_SCALES (2,3,4,6 per voxel)
//   bits 5..12 life          — ticks remaining, decremented each integrate
//
// DETERMINISM (rule 1): a micro particle that stains WRITES THE GRID, so it is
// sim state, not decoration. Its stain therefore goes through the same claim
// hash that reinsertion uses (see microStainPriority) — atomicMax over a
// state-derived priority, so exactly one droplet per cell per tick applies and
// the winner does not depend on dispatch order. Reading a word, or-ing a stain
// in and storing it from every droplet that landed would be a read-modify-write
// race and would break the world hash.
const PFLAG_MICRO   : u32 = 4u;
const PMICRO_SCALE_SHIFT : u32 = 3u;
const PMICRO_SCALE_MASK  : u32 = 3u;
const PMICRO_LIFE_SHIFT  : u32 = 5u;
const PMICRO_LIFE_MASK   : u32 = 0xFFu;   // 255 ticks max
const PMICRO_LIFE_ONE    : u32 = 1u << PMICRO_LIFE_SHIFT;

// Micro voxels per world voxel, indexed by the 2-bit scale field. Rendering
// divides the cube by this; it is presentation only (the sim never subdivides
// a cell), which is why non-power-of-two entries are fine here.
fn microScaleOf(flags : u32) -> u32 {
  let i = (flags >> PMICRO_SCALE_SHIFT) & PMICRO_SCALE_MASK;
  if (i == 0u) { return 2u; }
  if (i == 1u) { return 3u; }
  if (i == 2u) { return 4u; }
  return 6u;
}
fn microLifeOf(flags : u32) -> u32 {
  return (flags >> PMICRO_LIFE_SHIFT) & PMICRO_LIFE_MASK;
}
fn withMicroLife(flags : u32, life : u32) -> u32 {
  return (flags & ~(PMICRO_LIFE_MASK << PMICRO_LIFE_SHIFT)) |
         ((life & PMICRO_LIFE_MASK) << PMICRO_LIFE_SHIFT);
}
fn isMicro(p : Particle) -> bool { return (p.flags & PFLAG_MICRO) != 0u; }

// PAYLOAD bit 31 — "this is FOAM": render it in the tuner's foam colour rather
// than by looking its material up in the table. Foam is entrained air, not a
// substance; giving it a material id would mean authoring a material whose
// only job is to be white, and would put its colour in materials.json instead
// of next to the other fluid look knobs. The material bits are still valid
// underneath (foam inherits the fluid's material for its LANDING behaviour —
// stain, wetness — so MPM blood still foams pink-white and stains red).
// Written by sim_fluid.wgsl's g2p, read by debris.wgsl's vsParticle.
const PPAY_FOAM : u32 = 0x80000000u;

struct Particle {
  px : i32, py : i32, pz : i32,   // position, fixed 24.8 voxels
  vx : i32, vy : i32, vz : i32,   // velocity, fixed 24.8 voxels/tick
  payload : u32,                  // bits 0..11 material, 12..15 state,
                                  // bit 31 PPAY_FOAM
  flags   : u32,                  // PFLAG_*
};

// State-derived priority for reinsertion claims: atomicMax over these values
// is order-independent, so the claim winner is bit-deterministic. Never mix
// buffer slot indices into this.
fn particlePriority(p : Particle) -> u32 {
  let h = pcg(u32(p.px) ^ pcg(u32(p.py) ^ pcg(u32(p.pz) ^
          pcg(u32(p.vx) ^ pcg(u32(p.vy) ^ pcg(u32(p.vz) ^ p.payload))))));
  return h | 1u;  // nonzero (0 = empty claim slot)
}
fn claimSlot(cellIdx : u32) -> u32 {
  return pcg(cellIdx) & (CLAIM_SIZE - 1u);
}

// Priority for a micro particle's STAIN claim. Same contract as
// particlePriority — derived purely from particle state so atomicMax picks an
// order-independent winner — but deliberately a DIFFERENT hash, and it forces
// the low bit CLEAR where particlePriority forces it set.
//
// That parity split is what lets both populations share one claim array: a
// reinserting particle can never tie with a staining droplet, so a droplet
// landing on the same cell a voxel wants to occupy cannot steal the cell (it
// would lose the max) nor be mistaken for the winner (it checks its own value
// back). Two independent claim buffers would be the alternative and would cost
// another megabyte for nothing.
fn microStainPriority(p : Particle) -> u32 {
  let h = pcg(0x5D1A17u ^ u32(p.px) ^ pcg(u32(p.py) ^ pcg(u32(p.pz) ^
          pcg(u32(p.vx) ^ pcg(u32(p.vy) ^ pcg(u32(p.vz) ^ p.payload))))));
  return (h | 2u) & ~1u;  // nonzero, and always even
}

// ---- MLS-MPM fluid (docs/PLAN_mpm_fluids.md; sim_fluid.wgsl + seam) --------
// The EXCITED state of liquid: particles simulated by the fixed-point MLS-MPM
// solver. Settled liquid is voxels, exactly as always. The two converters in
// sim_fluid_seam.wgsl (excite: cells -> particles, settle: particles -> cells)
// are the only seam between the representations, and they DO write voxels —
// deterministically (integer math, slot-order scans, state-keyed RNG), so the
// world hash moves only when and where fluid actually converts. A world that
// never spawns fluid is byte-identical to one before this system existed.
//
// Units: 1 grid cell = 1 world voxel = 1.0; 1 tick = 1.0.
//   position  Q16.16 absolute world cells (range +-32768 cells — fine for the
//             playable region this prototype targets)
//   velocity  Q16.16 cells/tick
//   C matrix  Q16.16 1/tick (APIC affine velocity field)
//   J         Q16    volume ratio, 1.0 = 65536
// Must match FluidParticle consumers: sim_fluid.wgsl, sim_fluid_seam.wgsl and
// debris.wgsl vsFluid.
const FLUID_CAP       : u32 = 262144u;  // kFluidCap
const FLUID_BLOCKS    : u32 = 256u;     // kFluidBlocks
// MPM substeps per 30 Hz tick — the CFL BUDGET, and a tuning knob since WP3
// (sim.fluidSubsteps; EncodeTick records the substep table this many times, so
// the C++ side reads the same knob and world.h's kFluidSubsteps is only the
// fallback default). Everything CFL-derived hangs off it, right here, so the
// three numbers can never drift apart:
//   FLUID_VMAX     0.45 cell/substep, expressed in cells/TICK (velocity is a
//                  per-tick quantity everywhere in the solver; g2p advects
//                  v/FLUID_SUBSTEPS). This is the fluid's terminal velocity.
//   FLUID_MARK_PAD the block map is built once per tick, so `mark` must cover
//                  a whole tick of travel: ceil(0.45 * substeps) cells.
// The pseudo sound speed sqrt(stiffness)/30 must fit under 0.45 cells/substep
// or the clamp starts eating pressure work as energy loss (plan §1.2 item 1),
// which is the arithmetic behind the default: sqrt(14000)/13.5 = 8.7 -> 9.
const FLUID_SUBSTEPS  : i32 = clamp(TUNE_FLUID_SUBSTEPS, 1, 32);
const FLUID_VMAX      : i32 = i32(round(0.45 * f32(FLUID_SUBSTEPS) * 65536.0));
// Capped at 7 so the padded span [base-pad, base+2+pad] stays <= CHUNK (16)
// and `mark`'s 8-corner loop remains exhaustive over the chunks it touches.
// The cap binds only past 15 substeps, where a particle at terminal velocity
// can outrun the map and simply freezes for the substep — the same bounded,
// deterministic degradation the kFluidBlocks budget already accepts.
const FLUID_MARK_PAD  : i32 = min(i32(ceil(0.45 * f32(FLUID_SUBSTEPS))), 7);
const FLUID_ONE       : i32 = 65536;    // 1.0 in Q16.16
// Words per fluid grid node: [0] mass Q10, [1..3] momentum->velocity Q16.16,
// [4..6] species 1..3 mass Q10, [7] foam field (persistent). Shared by the
// solver (sim_fluid.wgsl) and the surface renderer (raymarch.wgsl reads mass,
// velocity and species words of the LAST substep's grid); world.cpp sizes
// fluidGrid by this.
const FLUID_GW        : u32 = 8u;

// ---- fluid block map: the Y-OCCUPANCY half (world.h fluidBlockMap) ---------
// The buffer is 2 * NUM_CHUNKS words. The first half is the chunk -> block
// index map the solver builds. The second half is one 16-bit mask per chunk:
// bit L set means "local y level L of this chunk can carry fluid density".
//
// It exists because `mark` pads the block set by FLUID_MARK_PAD cells so the
// map can be built once per tick (plan §7 item 4) — which broke the renderer's
// only test for "is there water here", namely "does this chunk have a block".
// A gravity-fed fluid is a THIN HORIZONTAL LAYER inside a 16-cell chunk, so a
// per-y-level bit is both the cheapest and the best-matched summary available:
// the march skips a whole y slab on one buffer read instead of taking ~13
// trilinear field samples (8 taps each) to discover it is air.
//
// Written by fluidGridUp (node mass) and seamStainApply (settled liquid, which
// contributes virtual mass to the isosurface), cleared by the same whole-buffer
// Fill that clears the index half. Render-only DERIVED data: no solver kernel
// reads it, so it is not hashed and cannot move the sim.
fn fbmYMaskIndex(slot : u32) -> u32 { return NUM_CHUNKS + slot; }
// The bits a source at local y level L lights up. The B-spline support is 1.5
// cells and the trilinear tap cube adds another half, so a source at L can
// raise the field at L-2 .. L+2.
fn fbmYBits(ly : i32) -> u32 {
  let lo = u32(clamp(ly - 2, 0, 15));
  let hi = u32(clamp(ly + 2, 0, 15));
  return ((1u << (hi - lo + 1u)) - 1u) << lo;
}

// The fluid particle, 32 words / 128 B (power-of-two stride for coalesced
// access; world.cpp sizes the two ping-pong buffers and the fluid-det gate
// strides by this count). Words 0..17 are the solver state the kernels touch
// every substep; 18..19 are the seam's identity words; 20..31 are reserved
// (zeroed at spawn) so the next attribute does not force another restride.
struct FluidParticle {
  px : i32, py : i32, pz : i32,   // Q16.16 world cells
  vx : i32, vy : i32, vz : i32,   // Q16.16 cells/tick
  // APIC affine matrix C, row-major (c00 c01 c02 / c10 c11 c12 / c20 c21 c22),
  // Q16.16. The traceless part carries angular momentum; the trace updates J.
  c00 : i32, c01 : i32, c02 : i32,
  c10 : i32, c11 : i32, c12 : i32,
  c20 : i32, c21 : i32, c22 : i32,
  j   : i32,                      // Q16 volume ratio. Diagnostic for a poured
                                  // particle (the EOS reads density), but the
                                  // excite converter seeds it from hydrostatic
                                  // depth so a reawakened column starts
                                  // pre-compressed instead of jello-popping.
  species : u32,                  // 0..3, grid species-mass slot (render color
                                  // + attraction). Derived from attr's mat at
                                  // excite time; the pour picks it directly.
  density : i32,                  // Q16.16 masses/cell sampled by p2g2 last
                                  // substep (render shading + attraction)
  // ---- seam identity (word 18): what this particle IS in voxel terms ------
  // mat (bits 0..11): the actual material id — reactions, staining and settle
  //   write-back all key on it. mat == 0 means DEAD: the slot is a corpse the
  //   next compaction pass removes. fullness == 0 means the same.
  // fullness (bits 12..14): how many voxel-eighths this particle carries.
  //   1 by default (8 particles == one full voxel); settle sums these, so the
  //   round trip is exact integer mass accounting.
  // stainType (bits 15..17) / stainAmt (bits 18..21): the stain the particle
  //   was excited with (voxel bits 28..30 / 24..27). Settle writes it back;
  //   contact staining spreads it to surfaces it touches.
  attr : u32,
  birthTick : u32,                // T.tick at spawn/excite (age diagnostics,
                                  // future force-settle-oldest under pressure)
  _r0 : i32, _r1 : i32, _r2 : i32, _r3 : i32,
  _r4 : i32, _r5 : i32, _r6 : i32, _r7 : i32,
  _r8 : i32, _r9 : i32, _r10 : i32, _r11 : i32,
};

// attr accessors — the one definition of the packing above.
fn fpMat(attr : u32) -> u32 { return attr & 0xFFFu; }
fn fpFullness(attr : u32) -> u32 { return (attr >> 12u) & 0x7u; }
fn fpStainType(attr : u32) -> u32 { return (attr >> 15u) & 0x7u; }
fn fpStainAmt(attr : u32) -> u32 { return (attr >> 18u) & 0xFu; }
fn fpAlive(attr : u32) -> bool {
  return (attr & 0xFFFu) != 0u && ((attr >> 12u) & 0x7u) != 0u;
}
fn fpPack(mat : u32, fullness : u32, stainType : u32, stainAmt : u32) -> u32 {
  return (mat & 0xFFFu) | ((fullness & 0x7u) << 12u) |
         ((stainType & 0x7u) << 15u) | ((stainAmt & 0xFu) << 18u);
}
// ORIGIN (bit 22): set by exciteEmit, clear on everything spawnAppend makes.
//
// It exists because sim.fluidExciteCeiling means "the standing size of the
// EXCITED region" and was being charged against the whole pool, spawns
// included. A pour is a spawn, spawns are never refused, and the lab's own
// basin scene runs 15,360 particles — so the moment anything poured, `live`
// sat above the 8,000 ceiling and excite's budget was zero from then on.
// Measured on `--fluid-bench basin` before this bit existed: 479 excite
// candidates, 0 eighths emitted, every slot refused, for all 400 ticks. That is
// the reported "water pouring down a waterfall never turns into MPM" — the
// waterfall was competing for headroom against the pour that made it.
// Charging the ceiling against excited-origin particles only restores the
// meaning the tuner documents; the POOL is still bounded, separately, by
// FLUID_CAP.
const FP_EXCITED : u32 = 1u << 22u;
fn fpExcited(attr : u32) -> bool { return (attr & FP_EXCITED) != 0u; }

// ---- fluidArgsStage word map (32 u32) --------------------------------------
// [0..3]  node-pass dispatch args + active block count (alloc, per substep)
// [4..6]  per-particle-pass dispatch args ((live+63)/64, 1, 1) — written by
//         the seam's excite scan once per tick, copied to the indirect buffer
// [7]     LIVE PARTICLE COUNT — the authoritative population, GPU-owned.
//         compact writes the survivor count, spawn ops and excite add to it.
//         Every per-particle kernel bounds itself on this word.
// [8]     dead-this-tick counter (settle kills, reaction consumption)
// [9]     excite-emitted particle count this tick
// [10]    settled eighths this tick (event counter: sound cues, mass audits)
// [11]    excited eighths this tick (event counter)
// [12]    excite refusals this tick (budget pressure diagnostics)
// [13]    settling block count this tick
// [14]    last excite chunk slot (sound cue positioning, coarse)
// [15]    eighths binned by settle this tick (mass audits)
// [16]    eighths consumed by CA reactions this tick (mass audits)
// [17]    contact stains applied this tick (parity gates, telemetry)
// [18]    node-substeps the FLUID_VMAX clamp truncated this tick (gridUpdate;
//         the CFL-honesty probe — plan §5 item 1). ~0 in steady flow, or the
//         stiffness/substep budget is wrong. Diagnostic only, never keyed on.
// [19..21] COMPACTION dispatch args ((live+255)/256, 1, 1) — written by the
//         excite scan alongside FA_LIVE, staged into fluidPDispatchArgs at the
//         head of the seam. compactCount/compactScatter used to dispatch 1024
//         fixed workgroups (all 262,144 pool slots) whatever the population was
//         (plan §7 item 1).
// [22..24] CONSUME dispatch args ((compactLive+63)/64, 1, 1) — written by the
//         compaction scan, staged before consumeApply, which used to dispatch
//         4,096 fixed workgroups.
// [25]    settle blocks REFUSED this tick (settleCheck: infeasible column or
//         a resulting cell that would immediately satisfy an excite trigger —
//         WP3's hysteresis-by-construction). Diagnostic: the number that says
//         whether "nothing settled" means "never went calm" or "went calm and
//         was refused", which are opposite bugs.
// [26]    settle blocks refused as excite-UNSTABLE this tick (the resulting
//         configuration would immediately satisfy an excite trigger). Split
//         from [25] because the two are opposite diagnoses.
// [27..31] spare
const FA_LIVE      : u32 = 7u;
const FA_DEAD      : u32 = 8u;
const FA_EMITTED   : u32 = 9u;
const FA_SETTLED   : u32 = 10u;
const FA_EXCITED   : u32 = 11u;
const FA_REFUSED   : u32 = 12u;
const FA_SETBLOCKS : u32 = 13u;
const FA_LASTSLOT  : u32 = 14u;
const FA_BINNED    : u32 = 15u;
const FA_CONSUMED  : u32 = 16u;
const FA_STAINED   : u32 = 17u;
const FA_CLAMPED   : u32 = 18u;
const FA_SETREFUSED : u32 = 25u;
const FA_SETUNSTABLE : u32 = 26u;
// WP5 excite-reach probe. "Nothing excited" has three completely different
// causes and only these two words tell them apart: SEEN counts settled
// seam-liquid cells exciteDetect actually LOOKED at (zero means the chunk was
// never in the dirty list — a WAKE problem), CANDID counts the ones that
// satisfied a trigger (SEEN high, CANDID zero means a TRIGGER problem).
// FA_REFUSED then covers the third case, the budget.
const FA_EXSEEN    : u32 = 27u;
const FA_EXCANDID  : u32 = 28u;
// Byte offsets of the two arg triples are what pass_table.def's copy rows use;
// keep the three in step (76 = 19*4, 88 = 22*4).
const FA_ARGS_COMPACT : u32 = 19u;
const FA_ARGS_CONSUME : u32 = 22u;

// Must match FluidSpawnOp in world.h (32 bytes). mat carries the pour's brush
// material into the particle's attr word (stainless, fullness 1).
struct FluidSpawnOp {
  px : i32, py : i32, pz : i32,   // Q16.16 world cells
  vx : i32, vy : i32, vz : i32,   // Q16.16 cells/tick
  species : u32, mat : u32,
};

// ---- excite scratch bits (voxel word bits 19..23, per the allocation table
// in the voxMat block above) -------------------------------------------------
// exciteDetect marks a candidate cell by setting EXCITE_PEND and stashing the
// cell's hydrostatic depth (capped 15) in the four bits above it; exciteEmit
// consumes the mark the SAME tick — either converting the cell to particles or
// (budget refusal) clearing the bits and leaving the water settled. The bits
// are excluded from the world hash and stripped on save, and no other kernel
// runs between the two passes, so they can never leak.
const EXCITE_PEND_BIT   : u32 = 0x80000u;       // bit 19
const EXCITE_DEPTH_SHIFT : u32 = 20u;           // bits 20..23
const EXCITE_SCRATCH_BITS : u32 = 0xF80000u;    // bits 19..23

// Must match ExplosionOp in world.h (32 bytes).
struct ExplosionOp {
  cx : i32, cy : i32, cz : i32,
  radius : i32,        // <= EXP_R_MAX
  power  : i32,        // hardness budget at the center
  _p0 : u32, _p1 : u32, _p2 : u32,
};
const EXP_R_MAX : i32 = 20;
// dispatch box per op: (2*EXP_R_MAX+1)^3 cells in 4^3 workgroups
const EXP_WG : u32 = 11u;  // ceil(41 / 4)
const EXP_BOX : i32 = 41;  // 2*EXP_R_MAX + 1
// per-op destruction scratch: one u32 per box cell holding (surviving power
// + 1), 0 = not destroyed. Each mark thread writes only its own cell.
const EXP_MASK_STRIDE : u32 = 68928u;  // 41^3 padded

fn isqrt(v : u32) -> u32 {
  var x = v;
  var res = 0u;
  var bit = 1u << 30u;
  while (bit > v) { bit = bit >> 2u; }
  while (bit != 0u) {
    if (x >= res + bit) {
      x -= res + bit;
      res = (res >> 1u) + bit;
    } else {
      res = res >> 1u;
    }
    bit = bit >> 2u;
  }
  return res;
}

// ---- toroidal residency addressing (DESIGN.md §3) ----
// World cell coords are unbounded i32. The resident cube covers world chunks
// [origin, origin + NCHUNK) per axis; world chunk c lives in slot c mod NCHUNK
// (a bitmask — sizes are powers of two, so this is correct for negatives too).
// Memory layout is chunk-major by SLOT and never shifts; moving the window
// recycles slots in place. Every kernel does its logic in WORLD coords and
// maps to slots only to touch memory. Cells outside the window are solid and
// inert (unloaded space rule) — each shader wraps inWindow() as its inBounds()
// against its own origin uniform (T.origin for sim, R.origin for render).

// Chunk-major addressing over SLOT coords (0..WORLD_N-1): each 16^3 chunk is
// one contiguous 16 KB block, so chunk streaming/readback is one copy each.
fn chunkIndexOf(c : vec3<u32>) -> u32 {
  let ch = c / CHUNK;
  return (ch.z * NCHUNK + ch.y) * NCHUNK + ch.x;
}
fn cellIndex(c : vec3<u32>) -> u32 {
  let lo = c % CHUNK;
  let localIdx = (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  return chunkIndexOf(c) * CHUNK_VOL + localIdx;
}

// World-coord variants: mask to the slot, then index. Callers must have
// checked inWindow() first — two world cells WORLD_N apart alias one slot.
fn cellIndexW(c : vec3<i32>) -> u32 {
  return cellIndex(vec3<u32>(c & vec3<i32>(WORLD_MASK)));
}
fn chunkIndexW(c : vec3<i32>) -> u32 {
  return chunkIndexOf(vec3<u32>(c & vec3<i32>(WORLD_MASK)));
}
fn inWindow(c : vec3<i32>, originChunk : vec3<i32>) -> bool {
  let d = c - originChunk * i32(CHUNK);
  let n = i32(WORLD_N);
  return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
}
// world CHUNK coord of a world cell (arithmetic shift = floor div, POT)
fn worldChunkOf(c : vec3<i32>) -> vec3<i32> {
  return c >> vec3<u32>(CHUNK_SHIFT);
}
fn chunkInWindow(wc : vec3<i32>, originChunk : vec3<i32>) -> bool {
  let d = wc - originChunk;
  let n = i32(NCHUNK);
  return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
}
fn chunkSlotIndex(wc : vec3<i32>) -> u32 {
  let s = vec3<u32>(wc & vec3<i32>(NCHUNK_MASK));
  return (s.z * NCHUNK + s.y) * NCHUNK + s.x;
}
// world chunk resident in slot chunk sc under window origin o
fn slotToWorldChunk(sc : vec3<i32>, o : vec3<i32>) -> vec3<i32> {
  return o + ((sc - o) & vec3<i32>(NCHUNK_MASK));
}

// ---- far-field cascades (render-only LOD — DESIGN.md §9) ----
// FAR_LEVELS nested toroidal FAR_N^3 volumes around the residency window, on
// their OWN grid (decoupled from WORLD_N so growing the window doesn't
// multiply cascade memory). Level k (1-based) cells span
// 2^(k + FAR_SHIFT_BASE) fine voxels — the shift base is chosen in world.h so
// level k's box edge is always 2^k WINDOW edges. Each level has its own
// origin (level-chunk units) in FarParams. farVox packs one material byte per
// cell (0 = air, IDs clamped to 255); farOcc holds one non-air count per
// level chunk for empty-space skipping. Derived data: the sim never reads any
// of it, and the world hash never covers it.
struct FarParams {
  origins : array<vec4<i32>, FAR_LEVELS>,  // xyz = origin, w unused
};
// fine voxels per level-k cell, as a shift amount
fn farCellShift(level : u32) -> u32 { return level + FAR_SHIFT_BASE; }
// Far-grid twins of the window addressing above, with FAR_N masks. Callers
// must have checked farInBox() first — level cells FAR_N apart alias a slot.
fn farCellIndexG(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(FAR_MASK));
  let ch = s / CHUNK;
  let lo = s % CHUNK;
  return ((ch.z * FAR_NCHUNK + ch.y) * FAR_NCHUNK + ch.x) * CHUNK_VOL +
         (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
}
fn farChunkIndexG(c : vec3<i32>) -> u32 {
  let ch = vec3<u32>(c & vec3<i32>(FAR_MASK)) / CHUNK;
  return (ch.z * FAR_NCHUNK + ch.y) * FAR_NCHUNK + ch.x;
}
fn farInBox(c : vec3<i32>, originChunk : vec3<i32>) -> bool {
  let d = c - originChunk * i32(CHUNK);
  let n = i32(FAR_N);
  return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
}
// level chunk resident in far slot chunk sc under that level's origin o
fn farSlotToChunk(sc : vec3<i32>, o : vec3<i32>) -> vec3<i32> {
  return o + ((sc - o) & vec3<i32>(FAR_NCHUNK_MASK));
}
fn farVoxByteIndex(level : u32, c : vec3<i32>) -> u32 {
  return (level - 1u) * FAR_VOX + farCellIndexG(c);
}
fn farOccIndex(level : u32, c : vec3<i32>) -> u32 {
  return (level - 1u) * FAR_NUM_CHUNKS + farChunkIndexG(c);
}

// ---- per-chunk occupancy packing ----
// Low 16 bits: total non-air voxels (chunk-skip for media-aware rays, CPU
// streaming/save-worthiness). High 16 bits: ray BLOCKERS — voxels that stop a
// ray as a surface hit (solids, powders, opaque liquids). Shadow rays skip
// chunks with zero blockers, so smoke/steam plumes stay cheap to shadow.
// Writers: sim_occupancy (both entries), worldgen genChunk, stream.cpp
// FillSlots (CPU). Readers must mask; the raw word is not a count.
// MICRO MATERIALS ARE NOT BLOCKERS, and this exclusion is load-bearing rather
// than cosmetic. A grass cell is mostly air: a ray that enters it usually
// misses the blades and has to CONTINUE. The blocker count exists so a shadow
// ray can skip a whole chunk in one step, and a chunk full of grass would
// otherwise report itself as solid and terminate every shadow ray at the
// meadow surface — a field of grass would cast the shadow of a wall.
//
// Safe for determinism because occupancy is derived, render-only data: it is
// written by sim_occupancy/worldgen and read ONLY by the renderer and by CPU
// streaming decisions. No sim kernel reads it, and the world hash does not
// cover it (see the hash comment in sim_occupancy.wgsl). The `total` count is
// deliberately left alone — a grass voxel IS present, and streaming/save
// worthiness keys off that.
fn isRayBlocker(m : Material) -> bool {
  if ((m.flags & MATF_MICRO) != 0u) { return false; }
  return m.klass == CLASS_SOLID || m.klass == CLASS_POWDER ||
         (m.klass == CLASS_LIQUID && (m.flags & MATF_OPAQUE) != 0u);
}
// OCCUPANCY WORD: [31] anyStain | [30..16] rayBlockers | [15..0] nonAir count.
//
// Both counts are bounded by CHUNK_VOL = 4,096, so each needs 13 bits and bit
// 31 was dead space. It now carries "some cell in this chunk has STAIN bits
// set", which is what lets the page-table free path decide from the snapshot
// the CPU ALREADY HAS instead of reading the chunk's 16 KiB of words back.
//
// Why the free path needs it at all: occupancy counts NON-AIR cells, but the
// determinism hash also covers the stain layer (bits 24..30 of a voxel word).
// A chunk can be entirely air and still carry stain — blood on a floor that
// then eroded, a bank that water soaked before evaporating — so `nonAir == 0`
// alone is NOT enough to demote a page to PT_EMPTY; doing that silently drops
// hashed state (gotcha-save-format-drops-stain, in a new place). Confirming
// that used to cost a blocking WaitIdle + 16 KiB readback PER CANDIDATE, which
// is why reclamation was capped at 128 chunks/tick against an allocation rate
// measured at 1,270/tick. With the answer folded in here it is one bit test.
//
// occBlockers MASKS to 15 bits: it used to be a bare `>> 16`, which would now
// read the stain flag as a blocker count of 32,768 and make every stained
// chunk fully opaque to the raymarcher's chunk-skip test.
fn occTotal(occ : u32) -> u32 { return occ & 0xFFFFu; }
fn occBlockers(occ : u32) -> u32 { return (occ >> 16u) & 0x7FFFu; }
fn occAnyStain(occ : u32) -> bool { return (occ & 0x80000000u) != 0u; }
fn packOcc(total : u32, blockers : u32) -> u32 { return total | (blockers << 16u); }
fn packOccStain(total : u32, blockers : u32, anyStain : bool) -> u32 {
  return total | (blockers << 16u) | select(0u, 0x80000000u, anyStain);
}

// ---- SUB-CHUNK OCCUPANCY BITMASK (PLAN_surface_flight_perf.md A2) ----------
// The chunk skip above is all-or-nothing at 1.6 m and keyed on a COUNT, so a
// single grass voxel in 4,096 defeats it and the ray steps 16-48 voxels through
// the chunk. A meadow is a slab of never-skippable chunks (micro is excluded
// from `blockers` but not from `total`, and `total` is what primary rays test);
// a canopy is a wall of half-full ones (foliage has no micro block at all, so
// leaves are plain solid voxels). This mask lets both skip INTERNALLY.
//
// SUBOCC_DIM^3 blocks per chunk, each (1 << SUBOCC_SHIFT) voxels on a side, one
// bit each: set = "this block holds at least one cell of this class". It lives
// in the TAIL OF THE `occupancy` BUFFER, past the count words, and not in a
// buffer of its own — see the world.h block for why (every Occupancy barrier,
// pass_table `uses` row and bind-group entry already covers it).
//
// TWO CLASSES per chunk, in this order: TOTAL (any non-air cell, what a primary
// ray needs) then BLOCKERS (isRayBlocker, what a media-blind shadow/reflection
// ray needs). They are NOT interchangeable in one direction: skipping on the
// blocker mask under a primary ray would drop gas, water and grass. The other
// direction is merely pessimistic. Exactly the occTotal/occBlockers split one
// level down, which is what keeps the chunk test and the block test from
// disagreeing about the same meadow.
//
// The blocker mask is also EXACTLY what a media-blind ray treats as a hit, cell
// by cell: gas and thin liquid fall through `if (wantMedia)`, micro falls
// through its own branch, ice/glass are CLASS_SOLID and so ARE blockers. So
// skipping a clear blocker block cannot change a shadow ray's answer.
//
// CONSERVATIVE DIRECTION: a set bit costs a march, a clear bit skips. Every
// producer that cannot compute the exact mask must therefore write ONES, never
// zeros — sentinels do, and JITTER page materialization deliberately leaves the
// sentinel's all-ones mask in place.
const SUBOCC_BLOCK : u32 = 1u << SUBOCC_SHIFT;    // voxels per block edge
// cls: 0 = total, 1 = blockers. Word w of that class for this chunk SLOT.
fn subOccIndex(slot : u32, cls : u32, w : u32) -> u32 {
  return SUBOCC_BASE + slot * SUBOCC_STRIDE + cls * SUBOCC_WORDS + w;
}
// Block bit for a chunk-local cell (0..CHUNK-1 per axis).
fn subOccBitLocal(lo : vec3<u32>) -> u32 {
  let b = lo >> vec3<u32>(SUBOCC_SHIFT);
  return (b.z * SUBOCC_DIM + b.y) * SUBOCC_DIM + b.x;
}
// Same, from the chunk-linear index the producers already iterate:
// i = (lz * CHUNK + ly) * CHUNK + lx.
fn subOccBitOfLocalIdx(i : u32) -> u32 {
  return subOccBitLocal(vec3<u32>(i % CHUNK, (i / CHUNK) % CHUNK,
                                  i / (CHUNK * CHUNK)));
}
// "Every block occupied" — the conservative fill, and what a non-air sentinel
// reports. SUBOCC_DIM^3 may be under 32 (shift 3 uses 8 of 64 bits); the extra
// ones are never tested, so setting them costs nothing and keeps the constant
// free of a per-word special case.
fn subOccAllOnes() -> u32 { return 0xFFFFFFFFu; }

// Voxel word: bits 0..11 material, 12..15 state, 16..18 tick-stamp,
//             19..23 FREE, 24..27 stain amount, 28..30 stain type,
//             31 CELLOP_IF_AIR (transient, never stored).
//
// Bits 19..23 are the only unallocated span in the word. They are NOT hashed
// (sim_occupancy hashes bits 0..15 + the stain field) and NOT persisted
// (kPersistMask in stream.cpp strips 16..23), so anything put there is
// per-tick scratch unless BOTH of those are changed to cover it. Whatever
// claims them must also say so here — this comment is the allocation table.
// CURRENT CLAIM: the MPM excite converter (sim_fluid_seam.wgsl) uses bit 19
// as its candidate mark and 20..23 as the hydrostatic depth, set by
// exciteDetect and consumed by exciteEmit within the same command buffer —
// see EXCITE_PEND_BIT below the FluidParticle block.
fn voxMat(w : u32) -> u32 { return w & 0xFFFu; }
fn voxState(w : u32) -> u32 { return (w >> 12u) & 0xFu; }
fn voxStamp(w : u32) -> u32 { return (w >> STAMP_SHIFT) & STAMP_MASK; }
fn packVox(mat : u32, state : u32, stamp : u32) -> u32 {
  return (mat & 0xFFFu) | ((state & 0xFu) << 12u) |
         ((stamp & STAMP_MASK) << STAMP_SHIFT);
}

// ---- the stain layer (DESIGN.md §3, §6) -------------------------------------
// Bits 24..30 of the voxel word: 4 bits of AMOUNT and 3 bits of TYPE. This is
// the "extra per-voxel state" DESIGN.md §3 anticipated, taken out of the spare
// byte the u32 word already carries rather than out of a new buffer — so it
// costs zero additional memory across 134M resident voxels, which is the whole
// reason it is 7 bits and not a byte-per-voxel side layer.
//
// TYPE is a small palette (1..7) registered at load from materials.json, NOT a
// material id: a 12-bit id would not fit, and the renderer only needs to know
// which of a handful of stain LOOKS to composite. Type 0 means unstained, so a
// clean world is bit-identical to one from before this feature existed.
//
// AMOUNT is 1..15 saturation. It drives coverage and darkness in the renderer
// and lets a stain build up from repeated contact instead of being binary.
//
// DETERMINISM (rule 1): stain is written by a sim kernel, so it is sim state.
// It is integer, it is rolled from the stateless RNG, and it IS covered by the
// world hash (see sim_occupancy.wgsl, which hashes bits 0..30). Bit 31 stays
// out of both: it is CELLOP_IF_AIR, a transient CPU->GPU flag that sim_mutate
// masks off before storing, and hashing it would be hashing a message field.
const STAIN_AMT_SHIFT  : u32 = 24u;
const STAIN_AMT_MASK   : u32 = 0xFu;
const STAIN_TYPE_SHIFT : u32 = 28u;
const STAIN_TYPE_MASK  : u32 = 0x7u;
const STAIN_AMT_MAX    : u32 = 15u;
const STAIN_TYPE_MAX   : u32 = 7u;
// The whole stain field, for masking it off or carrying it across a rewrite.
const STAIN_BITS : u32 = 0x7F000000u;

fn voxStainAmt(w : u32) -> u32 { return (w >> STAIN_AMT_SHIFT) & STAIN_AMT_MASK; }
fn voxStainType(w : u32) -> u32 { return (w >> STAIN_TYPE_SHIFT) & STAIN_TYPE_MASK; }
// A voxel is stained only if it has BOTH a type and an amount — either half at
// zero means unstained, which keeps "no stain" a single comparison.
fn voxStained(w : u32) -> bool { return (w & STAIN_BITS) != 0u && voxStainAmt(w) != 0u; }
fn packStain(stainType : u32, amt : u32) -> u32 {
  return ((amt & STAIN_AMT_MASK) << STAIN_AMT_SHIFT) |
         ((stainType & STAIN_TYPE_MASK) << STAIN_TYPE_SHIFT);
}
// Re-pack a word, KEEPING whatever stain it already carries. Every sim write
// that means "this voxel is still the same voxel, it just moved or changed
// fullness" must use this instead of packVox, or the stain is scrubbed off by
// ordinary liquid flow.
fn packVoxKeepStain(mat : u32, state : u32, stamp : u32, prev : u32) -> u32 {
  return packVox(mat, state, stamp) | (prev & STAIN_BITS);
}

// Stateless counter-based RNG (PCG output permutation).
fn pcg(v : u32) -> u32 {
  let s = v * 747796405u + 2891336453u;
  let w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
fn hash3(a : u32, b : u32, c : u32) -> u32 {
  return pcg(a ^ pcg(b ^ pcg(c)));
}

fn lateralDir(i : u32) -> vec2<i32> {
  switch (i & 3u) {
    case 0u: { return vec2<i32>( 1, 0); }
    case 1u: { return vec2<i32>( 0, 1); }
    case 2u: { return vec2<i32>(-1, 0); }
    default: { return vec2<i32>( 0,-1); }
  }
}

// ============================ PAGE TABLE ACCESSORS ==========================
// docs/PLAN_page_table.md §2. THE SEAM: every world-coordinate voxel access in
// every kernel routes through these, so no sim kernel's own code has to know
// there is a page table (ROADMAP §1: "the CA is unaware of it"). That property
// is not aspiration — it holds because sim_step.wgsl never names a buffer
// offset it did not get from cellIndexW.
//
// STANDING OBLIGATION: a kernel that computes a `voxels[]` subscript by any
// means other than these helpers bypasses the page table and reads physical
// memory that may belong to another chunk. The three chunk-linear paths
// (sim_occupancy's whole-chunk sweeps, worldgen's genChunk) use pageBaseOf
// instead, which is the same resolve hoisted out of their inner loop.
//
// THIS BLOCK IS STRIPPED for shaders that do not address voxels (sim_compact,
// debris, microbody, debug_lines) — see kPageBlockBegin in gpu/resources.cpp.
// It is delimited rather than conditionally generated so that this file stays
// the one place the translation is written.
//
// Integer-only throughout (rule 1): every operation is a u32 mask, shift,
// compare or multiply-add. There is no arithmetic a compiler can contract or
// reassociate into a different answer, and pageTable is READ-ONLY during a
// dispatch — it is written only by CPU-driven fills between dispatches, never
// by a sim kernel and never by an atomic. Two threads translating the same
// address in the same dispatch get the same answer because the input did not
// change. The page table is not sim state; it is dispatch-invariant
// configuration.
// >>>PAGE_TABLE_BEGIN<<<

// The word a sentinel chunk's cells read as. THIS IS THE HASH CONTRACT (§4.1):
// bit-identical to what a materialized page holds, because the materializing
// vkCmdFillBuffer pattern comes from this same rule (mirrored in C++ as
// SynthWord in world.h, which the page-roundtrip gate asserts agrees).
//
// synthWord(PT_EMPTY) == 0u, and that is not a coincidence to rely on loosely:
// it is why the empty case is free. An all-air materialized page is a
// fillBuffer(0), which is what every buffer already gets at creation. Zero is
// air by construction and always has been.
//
// STAMP_NEVER, not a live stamp: a sentinel chunk is by definition one that has
// not been simulated in place, so every voxel in it must be free to act on the
// first tick it is dispatched. A live stamp here would be the 0xFF-masked-to-7
// bug correlated across a whole chunk.
//
// State nibble 0: a UNIFORM sentinel carries only 12 bits of material and
// cannot represent a chunk whose cells differ in state, which is why promotion
// is by WHOLE-WORD equality and never by material equality (§2.3, risk 3).
fn synthWord(entry : u32) -> u32 {
  let mat = entry & PT_MAT_MASK;
  if (mat == MAT_AIR) { return 0u; }
  return packVox(mat, 0u, STAMP_NEVER);
}

// ---- JITTER synthesis: the positional half of the hash contract -----------
// A JITTER(mat) sentinel says "every cell is `mat`, stainless, STAMP_NEVER,
// state = the worldgen palette variant for that cell's WORLD position". The
// variant is a pure function of position and seed, so the chunk is describable
// by 4 bytes and this formula instead of a 16 KiB page. See the JITTER block
// in world.h for why this exists (2,115 buried chunks vs UNIFORM's 41).
//
// EXACT MIRROR of genCell (worldgen.wgsl) and of JitterStateFor / SynthWordAt
// (world.h). All four must agree bit-for-bit or a materialized page differs
// from what the sentinel read as, which is a lost voxel the hash reports one
// tick later somewhere unrelated. The page-roundtrip gate asserts the
// agreement rather than trusting it.
//
// THE SEED IS A PARAMETER here, supplied by callers from ptSeed() (see
// voxWordAt). These two stay pure so the page-roundtrip gate can evaluate them
// against the C++ mirror without a uniform in scope.
fn synthJitterState(c : vec3<i32>, seed : u32) -> u32 {
  let rnd = hash3(seed ^ 0xC0FFEEu,
                  bitcast<u32>(c.x) ^ (bitcast<u32>(c.z) << 12u),
                  bitcast<u32>(c.y));
  return rnd % 3u;
}

// The word the cell at WORLD position `c` reads as under sentinel `entry`.
// Falls through to synthWord for EMPTY and plain UNIFORM, so this is the one
// synthesis entry point every positional reader wants.
fn synthWordAt(entry : u32, c : vec3<i32>, seed : u32) -> u32 {
  let mat = entry & PT_MAT_MASK;
  if (mat == MAT_AIR) { return 0u; }
  if ((entry & PT_JITTER_BIT) == 0u) { return packVox(mat, 0u, STAMP_NEVER); }
  return packVox(mat, synthJitterState(c, seed), STAMP_NEVER);
}

// The table entry for a slot chunk index. The three chunk-linear paths resolve
// once with this and index their page directly, which is what they want anyway.
fn pageEntryOf(chunkSlot : u32) -> u32 { return pageTable[chunkSlot]; }

// THE read accessor. Replaces every `voxels[cellIndexW(c)]` in a sim kernel.
// Callers must have checked inWindow() first — that test is unchanged and
// still the outer guard. A sentinel is a statement about a RESIDENT chunk's
// contents; out-of-window is a statement about residency. Two different tests.
// The JITTER sentinel needs the world seed to synthesize a cell's palette
// variant. It comes from ptSeed(), a one-line accessor GENERATED per shader by
// LoadShader (gpu/resources.cpp) because the uniform holding the seed is named
// `T` in a sim kernel and `R` in the renderer, and common.wgsl is prepended
// before either is declared. Generating the accessor keeps all 46 voxWordAt
// call sites unchanged and keeps the seed out of every signature.
fn voxWordAt(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let e = pageTable[chunkIndexOf(s)];
  if ((e & PT_SENTINEL_BIT) != 0u) { return synthWordAt(e, c, ptSeed()); }
  let lo = s % CHUNK;
  return voxels[e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x];
}

// voxWordAt for a caller that ALREADY HAS the chunk's table entry. The
// raymarcher's DDA is such a caller: it reads pageTable[] once per chunk for
// the sentinel test and then walks 16-48 cells of that same chunk, so the
// generic accessor above re-issues an identical, already-cached load on every
// step — and worse, makes the `voxels[]` address DEPEND on it, serialising two
// round trips per cell (PLAN_surface_flight_perf.md A4). Hoisting the entry
// turns that into one independent load.
//
// `e` MUST be pageTable[chunkIndexOf(c & WORLD_MASK)] for this exact c, which
// is why this takes the entry rather than a chunk index: a caller that cannot
// say which chunk the entry came from should use voxWordAt.
fn voxWordAtEntry(e : u32, c : vec3<i32>) -> u32 {
  if ((e & PT_SENTINEL_BIT) != 0u) { return synthWordAt(e, c, ptSeed()); }
  let lo = vec3<u32>(c & vec3<i32>(CHUNK_MASK));
  return voxels[e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x];
}

// The ONLY way to obtain a WRITABLE word index. Returns PT_NO_WORD for a
// sentinel chunk — which is a BUG at every sim call site, because §3
// guarantees every chunk a kernel may write is materialized before dispatch.
//
// This is the invariant the whole phase rests on, and it is a SHAPE rather
// than a rule to remember: there is no writable accessor that takes a
// sentinel. The write accessor takes a physical word index, and the only way
// to get one is a function that returns a distinguished no-word value.
fn voxWordIndex(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let e = pageTable[chunkIndexOf(s)];
  if ((e & PT_SENTINEL_BIT) != 0u) { return PT_NO_WORD; }
  let lo = s % CHUNK;
  return e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
}

// Word index within a chunk-linear path: the page base for a slot plus a local
// offset. Returns PT_NO_WORD for a sentinel, same contract as voxWordIndex.
// This is the entry point for the three chunk-linear sites — sim_occupancy's
// two whole-chunk sweeps and worldgen's genChunk — which already have the
// chunk index in hand and want the resolve hoisted out of their inner loop.
fn voxWordInChunk(chunkSlot : u32, localIdx : u32) -> u32 {
  let e = pageTable[chunkSlot];
  if ((e & PT_SENTINEL_BIT) != 0u) { return PT_NO_WORD; }
  return e * CHUNK_VOL + localIdx;
}

// The chunk-linear READ: the word at (chunkSlot, localIdx), synthesized when
// the chunk is a sentinel. A branch rather than a select, because select
// evaluates both arms and voxels[PT_NO_WORD] is an out-of-bounds subscript.
// A JITTER sentinel's word depends on the cell's WORLD position, and a slot
// index is not one: the residency window is toroidal, so recovering the world
// chunk needs the window origin (ptOrigin(), generated next to ptSeed()). This
// is the one place where "slot index" and "world position" genuinely differ and
// the difference is load-bearing — using the slot directly would give every
// chunk in the window the variant pattern of whichever world chunk last
// occupied that slot.
fn worldCellOfSlotLocal(chunkSlot : u32, localIdx : u32) -> vec3<i32> {
  let sc = vec3<i32>(i32(chunkSlot % NCHUNK),
                     i32((chunkSlot / NCHUNK) % NCHUNK),
                     i32(chunkSlot / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, ptOrigin());
  let lo = vec3<i32>(i32(localIdx % CHUNK),
                     i32((localIdx / CHUNK) % CHUNK),
                     i32(localIdx / (CHUNK * CHUNK)));
  return wc * i32(CHUNK) + lo;
}

fn voxWordInChunkAt(chunkSlot : u32, localIdx : u32) -> u32 {
  let e = pageTable[chunkSlot];
  if ((e & PT_SENTINEL_BIT) != 0u) {
    if ((e & PT_JITTER_BIT) != 0u) {
      return synthWordAt(e, worldCellOfSlotLocal(chunkSlot, localIdx), ptSeed());
    }
    return synthWord(e);
  }
  return voxels[e * CHUNK_VOL + localIdx];
}

// >>>PAGE_TABLE_END<<<

// ---- the WRITE half, for the read_write shaders only -----------------------
// Split from the block above because raymarch.wgsl declares `voxels` as `read`
// (it is the render path and must never write the world) and has no
// `pageFaults` binding at all. A function that stores into `voxels` cannot
// resolve there, so the renderer gets translation without the write path —
// which is exactly the access the render path should have.
// >>>PAGE_TABLE_WRITE_BEGIN<<<

// Every sim write goes through this. A sentinel write is a NO-OP and is ALWAYS
// counted — the counter is unconditional, permanently bound, and every gate
// asserts it is zero (§5.1, user decision). The cost in a correct build is a
// branch that never fires; the benefit is that "zero page faults" is a claim
// the suite makes on EVERY run rather than in a special configuration, and
// that there is exactly one bind-group layout and one pass_table.def.
//
// A sentinel write is a no-op, not an out-of-bounds store: PT_NO_WORD is not
// an index into voxels, it is a value tested BEFORE indexing. The failure mode
// is a lost voxel (bad, visible in the hash) rather than a corrupted stranger
// (worse, invisible until it isn't). There is no harmless somewhere to write —
// any physical index is some other chunk's voxel.
fn voxStore(idx : u32, w : u32) {
  if (idx == PT_NO_WORD) {
    atomicAdd(&pageFaults[0], 1u);
    return;
  }
  voxels[idx] = w;
}
// >>>PAGE_TABLE_WRITE_END<<<
