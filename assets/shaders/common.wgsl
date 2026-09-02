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
// TINTED: this material's state nibble is a COLOUR index, not a jitter variant.
// Mirrors kMatFlagTinted in sim/materials.h, which carries the full rationale.
// The nibble indexes this material's own run of the tint palette, which is what
// gives a GRID cell a per-voxel colour — art colour never reaches the grid.
// Liquids can never carry it (their nibble is fullness); refused at load.
const MATF_TINTED : u32 = 16u;
// Start of this material's 16-entry tint run, packed into the free high half of
// `flags` (bits 16..23) — see the wind note below for why `flags` and not a new
// field. Mirrors kMatTintBaseShift / kMatTintBaseMask in sim/materials.h.
const MATF_TINT_BASE_SHIFT : u32 = 16u;
const MATF_TINT_BASE_MASK  : u32 = 0xFFu;
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
  // ---- WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3; src/sim/windprim.h) ----
  // Fans, spell gusts, tornadoes: a bounded list of parametric objects summed
  // analytically at every sample, like point lights. See the long note on the
  // C++ side of this struct for why they ride the uniform (no new binding, no
  // new barrier) and why a count of zero is an exact identity.
  windPrimCount : u32,
  windWakeCount : u32,   // chunk slots to dirty-mark — sim_mutate's windWake
  padWp0 : u32,
  padWp1 : u32,
  windPrimLo : vec3<i32>,   // union AABB, inclusive world cells; the whole-loop
  padWp2 : i32,             // early-out. lo > hi means "no primitives".
  windPrimHi : vec3<i32>,
  padWp3 : i32,
  // WIND_PRIM_CAP primitives x 3 rows. The literal 96 is deliberate: this file
  // and world.h are compared by scripts/check_invariants.py on TOTAL SIZE, so a
  // cap changed on one side and not the other fails the check rather than
  // silently reading zeros. Keep 96 = 3 * kWindPrimCap.
  windPrims : array<vec4<i32>, 96>,
  // kWindWakeCap slots, four to a std140 row.
  windWake : array<vec4<u32>, 32>,
  // Nonzero while the per-voxel activity overlay is on. THE THIRD HALF of the
  // same field: it was added to the C++ TickParams and to the pass table's
  // VizActive condition without ever reaching this struct, so the sizes
  // disagreed (2224 vs 2240) and check_invariants.py failed. A shorter WGSL
  // struct does not error — the shader simply cannot see the field — which is
  // why that checker compares total size rather than trusting the declaration.
  vizActive : u32,
  // ---- WATER BODIES (docs/PLAN_water_master.md M2; must match TickParams in
  // src/sim/world.h) ----
  // The CPU sends geometry and thresholds ONLY. Quiescence and adoption are
  // decided GPU-side in sim_waterbody.wgsl, because the M1 ladder read the
  // async snapshot and "adopted when the CPU got around to noticing" is a
  // scheduling-dependent outcome the moment adoption gates a voxel write
  // (rule 1). See the long note on the C++ side of this struct.
  //
  // waterBodyMode 0 => waterBodyCount and waterChunkCount are 0, every pass row
  // is skipped, and the pinned world hash cannot move.
  waterBodyMode   : u32,
  waterBodyCount  : u32,
  waterChunkCount : u32,
  waterTestDrain  : i32,   // eighths/tick/body, TEST-ONLY source (M2)
  waterQuietTicks : i32,   // sim.waterBodyQuietTicks
  waterMinVolume  : i32,   // sim.waterBodyMinVolume, EIGHTHS
  // M3 (components 6 + 7). The discharge emits through the CPU-sized
  // spawnAppend stream, so the CPU reserves a contiguous op block per body and
  // wbDrain fills it; `waterDrainMax` is the per-hole per-tick bound (rule 2)
  // and `waterExciteRadius` is component 7's v1 radius, 0 = no shell.
  waterDrainSpawnBase : u32,
  waterDrainBodies    : u32,
  waterDrainMax       : i32,
  waterExciteRadius   : i32,
  // M5: the scheduled re-derive. Both are pure functions of the tick — see the
  // note in world.h and plan section 3.4. WATERBODY_CAP means "nothing is
  // scheduled", which is every tick of a lake nobody has dug into.
  waterSweepSlot  : u32,
  waterSweepLevel : i32,
  padWb2 : u32,            // see the alignment arithmetic in world.h
  padWb3 : u32,
  padWb4 : u32,
  // WATERBODY_CAP bodies x 2 rows. The literal 128 is deliberate, exactly like
  // windPrims' 96: this file and world.h are compared on TOTAL SIZE by
  // scripts/check_invariants.py, so a cap changed on one side and not the other
  // fails the check rather than silently reading zeros.
  waterBodies : array<vec4<i32>, 128>,
  // WATER_CHUNK_CAP entries, four to a std140 row. 256 rows = 1024 entries;
  // M5 doubled it because a dug basin lists its split child's footprint too.
  waterChunks : array<vec4<u32>, 256>,
  // ---- THE CURRENT FIELD, the SIM's copy (docs/PLAN_water_master.md
  // component 8; src/sim/currentprim.h; must match TickParams in world.h) ----
  //
  // A clone of the wind primitive block above, deliberately and structurally:
  // same cap, same three-row packing, same union-AABB early-out, same
  // float/integer transcription pair. Water flow is the same KIND of object as
  // wind — a bounded list of parametric shapes summed analytically at a sample
  // point — and building it as a second thing shaped differently would have
  // meant a second set of overflow arguments to get wrong.
  //
  // MODE 0 IS AN EXACT IDENTITY, and it is the shipping default. currentAtQ
  // returns the zero vector on `currentMode == 0` before it looks at anything,
  // so no sim kernel can see this block and the pinned world hash cannot move.
  // Same shape as windMode and waterBodyMode.
  currentMode      : u32,
  currentPrimCount : u32,
  padCp0 : u32,
  padCp1 : u32,
  currentPrimLo : vec3<i32>,   // union AABB, inclusive world cells; lo > hi
  padCp2 : i32,                // means "no primitives"
  currentPrimHi : vec3<i32>,
  padCp3 : i32,
  // CURRENT_PRIM_CAP primitives x 3 rows. The literal 96 is deliberate for
  // windPrims' reason: this file and world.h are compared on TOTAL SIZE by
  // scripts/check_invariants.py. Keep 96 = 3 * kCurrentPrimCap.
  currentPrims : array<vec4<i32>, 96>,
};

// ---- WATER BODIES: the GPU-owned ledger's word map (M2/M3) -----------------
//
// Must match kWaterBodyStateWords in src/sim/world.h and the WBS_* reader in
// src/test/selftest_water.cpp. It lives HERE rather than in sim_waterbody.wgsl
// because from M3 a second module reads it: sim_fluid_seam.wgsl's exciteDetect
// asks whether a settled cell is inside a draining hole's excite SHELL
// (component 7), and a word map transcribed into two shaders is a
// two-places-must-agree bug with nothing checking it.
//
// Twenty-four words is more than the state needs, and that is deliberate: plan
// §7 asks for attribution words BEFORE they are needed, because "conservation
// failed by 37" with nothing attached is the bare count CLAUDE.md rule 6 says
// costs a dozen elimination runs to un-ask.
const WBS_STATE     : u32 = 0u;   // WB_* below
const WBS_LEVEL     : u32 = 1u;   // world Y of the free surface
const WBS_AREA      : u32 = 2u;   // surface cells the ledger paces against
const WBS_DEBIT     : u32 = 3u;   // eighths owed but not yet off the voxels
const WBS_SHAVED    : u32 = 4u;   // what LAST tick's shave actually removed
const WBS_SEEN      : u32 = 5u;   // free-surface cells that shave saw
const WBS_ATLEVEL   : u32 = 6u;   // of those, how many sat at exactly LEVEL
const WBS_STEPS     : u32 = 7u;   // published: whole eighths per surface cell
const WBS_FRAC      : u32 = 8u;   // published: dither numerator, in [0, area)
const WBS_DRAINED   : u32 = 9u;   // cumulative eighths that left forever
const WBS_VOLUME    : u32 = 10u;  // the reduce's voxel-eighth sum at adoption
const WBS_QUIET     : u32 = 11u;  // consecutive undisturbed ticks
const WBS_RSUM      : u32 = 12u;  // reduce scratch: running eighth sum
const WBS_RDIRTY    : u32 = 13u;  // quiescence scratch: disturbed chunks
const WBS_CAPPED    : u32 = 14u;  // attribution: eighths the cells did not have
const WBS_ADOPTTICK : u32 = 15u;  // attribution: the tick adoption happened on
// ---- M3: the hole record (component 6) ------------------------------------
// Two copies of the hole, CURRENT and NEXT, because plan section 3.3 is not
// optional: the scan that finds a hole must not be the pass that spends it.
// wbHole accumulates into the *N words this tick; the ledger promotes them next
// tick.
const WBS_HOLEKEY   : u32 = 16u;  // packed hole cell in use (WB_HOLE_NONE = none)
const WBS_HOLEAREA  : u32 = 17u;  // orifice A: cells at the hole's own level
const WBS_HOLEKEYN  : u32 = 18u;  // this tick's scan, atomicMin target
const WBS_HOLEAREAN : u32 = 19u;  // this tick's scan, count at WBS_HOLEKEY's y
// TICKS THE HOLE SURVIVES WITHOUT BEING RE-SEEN. A hole is found on the
// chunk-dirty path (holes appear when someone digs), and a hole that is
// actively draining keeps its own chunk hot — the jet's MPM block and the CA
// re-levelling above it both report. So this is what makes a PLUGGED hole stop
// draining rather than a timer the drain depends on, and it is what bounds the
// process (rule 2): nothing can keep emitting into a world gone quiet around it.
const WBS_HOLETTL   : u32 = 20u;
// PUBLISHED BY THE LEDGER, CONSUMED BY wbDrain. `emit` is the eighths granted
// this tick after every cap; `jetv` is the exit speed from the SAME evaluation
// of h. One evaluation, two consumers — that is plan section 6's whole rule,
// and the reason these are ledger words rather than two recomputations.
const WBS_EMIT      : u32 = 21u;
const WBS_JETV      : u32 = 22u;  // Q16.16 cells/tick, downward
// COMPONENT 7's measurement, cumulative while the body stays adopted: cells the
// drain shell handed to the solver. Plan section 9 item 2 ranks the shell's
// particle budget as the second-most-likely way this work fails and calls the
// mitigation UNMEASURED — this word is the measurement, and it is cumulative
// rather than per-tick so a gate reading it once still sees the whole window.
const WBS_EXSHELL   : u32 = 23u;
// ---- M5: the re-audit (component 10) --------------------------------------
// ARMED by the ledger on the first level of each sweep cycle, CONSUMED by it on
// the next tick after wbReduce has refilled WBS_RSUM. This is the fix for the
// leftover M2 named and M3 did not close: a body adopted once carries the
// volume it had at adoption, and someone who digs into it makes that number a
// lie — which bounds the discharge through `held = VOLUME - DRAINED` and stops
// a lake draining that has plenty left.
//
// It refreshes VOLUME ONLY, as `rsum + drained`, so `held` becomes the measured
// voxel sum while WBS_DRAINED — the cumulative term `--gate waterbody` passes A
// and H balance their identity on — is not touched. A re-audit that reset
// DRAINED would look exactly like a leak to both of them.
const WBS_REAUDIT   : u32 = 24u;
const WBS_AUDITTICK : u32 = 25u;  // attribution: the last re-audit's tick
// The RESOLVED sweep level for this tick, published by the ledger and read by
// wbSweep and wbSplit. See kWaterSweepLive in world.h for why it is published
// rather than recomputed. WB_SWEEP_NONE means nothing sweeps this tick.
const WBS_SWEEPY    : u32 = 26u;
const WB_SWEEP_LIVE : i32 = -0x7FFFFFFF;
const WB_SWEEP_NONE : i32 = -0x40000001;

// The ladder, GPU side. Candidate -> Measuring -> Adopted, and Releasing is the
// way out. Measuring is its own state rather than a flag because the reduce is
// a WHOLE-FOOTPRINT pass and must run exactly once per adoption: a body that
// re-measured every tick would be the O(volume)-per-tick cost this design
// exists to delete.
const WB_CANDIDATE : i32 = 0;
const WB_MEASURING : i32 = 1;
const WB_ADOPTED   : i32 = 2;
const WB_RELEASING : i32 = 3;

// CPU-sent per-body flags (TickParams.waterBodies row 1, word 3).
const WBF_PROPOSE : i32 = 1;   // the CPU's deterministic tests all passed
const WBF_RELEASE : i32 = 2;   // an EXIT test failed; hand this body back
// M5: which component of a SPLIT basin this body governs, in bits 2..3. A body
// with component 0 and a basin with no split map is exactly what M2/M3/M4
// shipped — the map reads all-zero, every cell answers component 0, and the
// footprint test collapses to the disc test it always was. That is why M5's
// split machinery is an exact identity on a lake nobody has dug into.
const WBF_COMP_SHIFT : u32 = 2u;
const WBF_COMP_MASK  : i32 = 3;
// M5: the body is a SPLIT CHILD. Carried separately from the component index
// because "component 0" is also what an unsplit parent answers, and the ledger
// needs to tell "I am the original body" from "I am the second pool that
// appeared when the level fell past a partition" — a child with no cells is a
// candidate that never adopts, and that must not be reported as a body the CPU
// refused.
const WBF_CHILD : i32 = 16;
// M5: a child's PARENT slot, in bits 8..15. Carried so the ledger can arm a
// child's re-audit on the same cycle boundary as its parent's — two pools that
// re-measured on different ticks would report a `held` sum that briefly does
// not add up to the water in the basin, and a conservation gate has no way to
// tell that apart from a leak.
const WBF_PARENT_SHIFT : u32 = 8u;
const WBF_PARENT_MASK  : i32 = 255;
// "No split found." atomicMax's identity, and deliberately far below any legal
// world Y so the first disconnected level found always wins.
const WB_SPLIT_NONE : i32 = -0x40000000;

// ============================================================================
// M5 — THE MEASURED CONTAINER CURVE AND THE SPLIT MAP (plan components 2 case
// 2 and 10). Word map for the region of `waterBodyState` past the ledger.
//
// Must match kWaterCurve*/kWaterSplit*/kWaterSweep* in src/sim/world.h and the
// SW_* reader in src/test/selftest_water.cpp.
//
// WHY THIS IS DERIVED DATA AND NOT AN AUTHORITY. Plan section 3.2: the surface
// shave counts what it ACTUALLY removed and debits that, so nothing here can
// ever become a mass number. area(y) paces a descent, the spill elevation gates
// a jurisdiction test, the split map picks which pool a cell belongs to. An
// approximate or few-ticks-stale table costs pace and picks a slightly wrong
// moment to split. It cannot lose an eighth, and that is the property that lets
// the sweep be scheduled at all.
// ============================================================================
// WATER_SPLIT_GRID / _CELLS / _WORDS, WATER_CURVE_MAXY / _WORDS,
// WATER_SWEEP_HEADER, WATER_CURVE_BASE and WATER_SCRATCH_BASE are GENERATED
// from world.h by ShaderConstantPrelude — the engine's standing rule that a
// world constant a shader must agree with is never restated in WGSL. Only the
// FIELD NAMES below are this file's, because a word map is a protocol rather
// than a size.
// ---- header ----
// The area table's floor. Stored rather than re-derived from TickParams so a
// reader (the gate, the ledger) can interpret the table without also knowing
// which tick wrote it.
const SW_FLOORY   : u32 = 0u;
// How many Y entries the table actually covers, and whether the basin was
// deeper than WATER_CURVE_MAXY and got truncated. Reported, not hidden: an
// approximation nobody can see is an approximation nobody can bound.
const SW_SPANY    : u32 = 1u;
const SW_TRUNC    : u32 = 2u;
// OUTPUT 2 of the sweep: the SPILL ELEVATION — the lowest world Y at which the
// basin's rim is open to the outside. atomicMin target, so WB_HOLE_NONE is the
// identity. Above it the container curve is meaningless because the body is not
// a body, it is a flow (plan component 5's first enter test).
const SW_SPILLY   : u32 = 3u;
// OUTPUT 3 of the sweep: the SPLIT ELEVATION — the highest level at which the
// basin's wet region is disconnected, i.e. the merge tree's first merge going
// up and therefore the first split going down. WB_HOLE_NONE means "no split
// found in any level scanned so far".
const SW_SPLITY   : u32 = 4u;
// Components found at the level the ACTIVE map describes, and which level that
// is. The active map is written only when the swept level equals the body's own
// live level, because that is the only level a footprint test ever asks about.
const SW_COMPS    : u32 = 5u;
const SW_MAPY     : u32 = 6u;
// Bumped every time the active map CHANGES. The ledger watches it and re-audits
// the body's volume when it moves — that is component 10's whole trigger, and
// it is tick-deterministic because the sweep that bumps it is.
const SW_MAPGEN   : u32 = 7u;
// THE ACCUMULATORS' "NEXT" HALF, and it is plan section 3.3 at cycle
// granularity rather than tick granularity.
//
// SW_SPILLY and SW_SPLITY are atomic reductions over EVERY LEVEL of the basin,
// and a sweep visits one level per scheduled tick — so the reduction is only
// meaningful once a whole cycle has been walked. Accumulated in place they were
// worse than useless: a reader between the cycle's reset and its end sees the
// maximum over however many levels happened to have been scanned, which for a
// draining lake is "roughly the live level" and looks exactly like a correct
// answer. Measured on the first version of `--gate waterbody` pass B as a split
// elevation of 188 against a partition whose top is at 205.
//
// So the sweep accumulates into the *N words and the cycle boundary PROMOTES
// them, which makes SW_SPILLY/SW_SPLITY the result of the last COMPLETE pass
// over the basin and nothing else.
const SW_SPILLYN  : u32 = 8u;
const SW_SPLITYN  : u32 = 9u;
// area(y): WATER_CURVE_MAXY entries, cells at y = SW_FLOORY + 1 + i. CELLS, not
// columns (plan section 2) — a cave, an overhang or a flooded tunnel under the
// lake is counted, and counting columns would silently reimplement the
// single-span assumption that got heightfields rejected.
const SW_AREA0    : u32 = WATER_SWEEP_HEADER;
// The split map: 2 bits per grid cell, WATER_SPLIT_WORDS words.
const SW_SPLIT0   : u32 = WATER_SWEEP_HEADER + WATER_CURVE_MAXY;
// "This grid cell holds no water at the mapped level." Distinct from component
// 0 so a blocked cell cannot silently enlarge the parent's footprint.
const WB_COMP_NONE : u32 = 3u;
// Smallest component the split will name, in GRID CELLS. Below this a
// "component" is an artefact of the downsample — the two or three cells
// where the disc's edge clips a grid cell — and naming it costs a whole
// descriptor and hands the parent a pool with no water in it. 8 grid cells
// is ~72 columns at the harness lake's step of 3.
const WB_SPLIT_MIN_CELLS : u32 = 8u;
// Most whole eighths one tick's shave may take off a surface cell: one whole
// voxel. See the note at the `steps` computation in sim_waterbody.wgsl.
const WB_MAX_STEPS : i32 = 8;

fn wbCurveBase(b : u32) -> u32 { return WATER_CURVE_BASE + b * WATER_CURVE_WORDS; }
// The basin's column AABB -> grid step. Derived from the geometry the CPU
// already sends (the disc bound) rather than sent as its own field, so the two
// cannot disagree: one number, one owner.
fn wbGridStep(radius : i32) -> i32 {
  return max(1, (2 * radius + 1 + i32(WATER_SPLIT_GRID) - 1) /
                    i32(WATER_SPLIT_GRID));
}
// World column -> grid cell index, or WATER_SPLIT_CELLS when outside the grid.
fn wbGridIndex(x : i32, z : i32, cx : i32, cz : i32, radius : i32) -> u32 {
  let step = wbGridStep(radius);
  let gx = (x - (cx - radius)) / step;
  let gz = (z - (cz - radius)) / step;
  if (gx < 0 || gz < 0 || gx >= i32(WATER_SPLIT_GRID) ||
      gz >= i32(WATER_SPLIT_GRID)) {
    return WATER_SPLIT_CELLS;
  }
  return u32(gz) * WATER_SPLIT_GRID + u32(gx);
}

// "No hole." atomicMin's identity, and deliberately i32-positive so the packed
// key ordering below is a plain integer compare.
const WB_HOLE_NONE : i32 = 0x7FFFFFFF;
// How many ticks a hole outlives its last sighting. 8 is a quarter second:
// long enough that a tick where the throat chunk happened to go clean does not
// stutter the jet, short enough that plugging a hole stops the drain visibly.
const WB_HOLE_TTL : i32 = 8;
// The hole key packs (y, x, z) so that a plain atomicMin picks the LOWEST cell
// — the deepest escape point, i.e. the greatest head — and, among cells at that
// depth, one specific winner by position. Order-independent and therefore
// legal: integer min is associative and commutative, so which thread arrives
// first cannot change the answer (the atomicCAS ban is about the other kind).
//
//   bits 22..30  y - floorY + WB_HOLE_YBIAS, 0..511
//   bits 11..21  x - cx + 1024, 0..2047
//   bits  0..10  z - cz + 1024, 0..2047
//
// The bias lets a hole be cut BELOW the analytic basin floor (a shaft driven
// under a lake is exactly that) without the key going negative.
const WB_HOLE_YBIAS : i32 = 8;
fn wbHoleKey(dy : i32, dx : i32, dz : i32) -> i32 {
  let y = clamp(dy + WB_HOLE_YBIAS, 0, 511);
  let x = clamp(dx + 1024, 0, 2047);
  let z = clamp(dz + 1024, 0, 2047);
  return (y << 22u) | (x << 11u) | z;
}
fn wbHoleY(key : i32, floorY : i32) -> i32 {
  return floorY + ((key >> 22u) & 511) - WB_HOLE_YBIAS;
}
fn wbHoleX(key : i32, cx : i32) -> i32 { return cx + ((key >> 11u) & 2047) - 1024; }
fn wbHoleZ(key : i32, cz : i32) -> i32 { return cz + (key & 2047) - 1024; }

// Must match kWindScaleOne / kWindScaleMax in src/sim/world.h.
const WINDQ_SCALE_ONE : i32 = 256;

// sim.windMode ladder — must match kWindMode* in src/sim/world.h.
const WIND_MODE_OFF     : u32 = 0u;
const WIND_MODE_DRIFT   : u32 = 1u;
const WIND_MODE_ENTRAIN : u32 = 2u;

// ---- wind primitive encoding — must match src/sim/world.h ------------------
const WIND_PRIM_CAP  : u32 = 32u;
const WIND_PRIM_ROWS : u32 = 3u;    // vec4<i32> rows per primitive
const WPRIM_CONE   : u32 = 0u;
const WPRIM_BURST  : u32 = 1u;
const WPRIM_VORTEX : u32 = 2u;
const WPRIM_KIND_MASK : u32 = 0xFu;
const WPRIM_F_SHIFT   : u32 = 4u;
const WPRIM_F_AIR     : u32 = 1u;
const WPRIM_F_WATER   : u32 = 2u;
const WPRIM_F_ENTRAIN : u32 = 4u;

// ---- current primitive encoding — must match src/sim/world.h ---------------
// docs/PLAN_water_master.md component 8. Four shapes, and the set is closed for
// the same reason the wind set is: they are SUMMABLE, and superposition of
// sources, sinks and vortices is a real solution of Laplace's equation rather
// than a pile of special cases. A river entering a pool and spreading outward
// is what a point SOURCE does for free — the plan's §0 correction 4 — so there
// is no diffusion step, no relaxation, no stored field and nothing to wake.
const CURRENT_PRIM_CAP  : u32 = 32u;
const CURRENT_PRIM_ROWS : u32 = 3u;
const CPRIM_SINK   : u32 = 0u;   // drain inflow: radial IN, 1/r^2, clamped
const CPRIM_SOURCE : u32 = 1u;   // river mouth / jet base: radial OUT, 1/r^2
const CPRIM_VORTEX : u32 = 2u;   // swirl about an axis: Gamma/2*pi*r tangential
const CPRIM_STREAM : u32 = 3u;   // uniform flow down a bed gradient
const CPRIM_KIND_MASK : u32 = 0xFu;
const CPRIM_F_SHIFT   : u32 = 4u;
// A primitive the SIM is allowed to read. Seeded primitives derived from
// anything the CPU learned from a readback carry this CLEAR, so they reach the
// renderer and the player and never the hashed world — see the authority note
// over currentAtQ.
const CPRIM_F_SIM     : u32 = 1u;
// Impact-ripple ring size — must match kWaveImpactCap in src/sim/world.h.
const WAVE_IMPACT_CAP : u32 = 16u;

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

// ===== WIND UNIFORMS ARE PASSED BY POINTER, NOT BY VALUE ====================
// MEASURED, 2026-08-26, RTX 3060 Ti: passing these two structs BY VALUE into
// the wind functions cost the game 220.1 ms/frame p50 against 23.6 ms with the
// calls removed. A 9.3x collapse, in a world with ZERO wind primitives alive.
//
// The mechanism, because the naive reading of it is wrong. The cost is NOT the
// struct being big, and it is NOT the loop: with zero primitives the loop never
// runs and the function returns on its first compare. It is that windPrimAt
// DYNAMICALLY INDEXES `windPrims[b]` on a BY-VALUE COPY. A by-value uniform
// whose members are only ever read at STATIC offsets gets scalarised by the
// driver after inlining and the copy evaporates — which is why the ~400-byte
// RenderParams was passed by value here for a year at no cost, and why
// windSampleAt (scalars only) is still free. A dynamic index has no static
// offset to fold, so the compiler must materialise the whole 1936-byte struct
// in SCRATCH MEMORY (VRAM-backed spill) at every call, then index that. In the
// raymarcher's per-micro-detail-cell sway path that is thousands of 1.9 KB
// spills per pixel, and occupancy dies with it.
//
// THE RULE, therefore: any function that indexes windPrims / windWake takes the
// uniform as `ptr<uniform, T>` and dereferences it, and so does every function
// that CALLS one — a `*R` at any point in that chain reinstates the copy and
// the whole 9.3x with it. Tint compiles uniform pointer parameters to direct
// OpAccessChains into the uniform block: no copy, no scratch, and the
// count == 0 early-out becomes one constant-cache scalar load.
//
// This is the same family as the far-shadow 45x and cascade-shadow 48x cliffs
// (see CLAUDE.md): a small-looking change in a raymarcher inner loop that
// destroys occupancy. Cheapest check that it has not come back is one run of
//   bash scripts/run.sh ./build/Release/sandvox.exe --frames 400
// and reading the `whole-frame ms p50` line — the headless gates measure the
// SIM and cannot see a render-occupancy regression at all.
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
  // ---- wind primitives, the RENDER copy (§4.3) ---------------------------
  // The same resolved list TickParams carries, in the same encoding, from the
  // same WindPrimSystem in the same frame — which is why the grass leans in a
  // fan's blast with nothing wiring grass to fans, and why the debug arrows
  // are evidence rather than decoration (invariant 2: ONE field function).
  windPrimCount : u32,
  _pwpr0 : u32,
  _pwpr1 : u32,
  _pwpr2 : u32,
  windPrimLo : vec3<i32>,
  _pwpr3 : i32,
  windPrimHi : vec3<i32>,
  _pwpr4 : i32,
  windPrims : array<vec4<i32>, 96>,   // 3 * WIND_PRIM_CAP — see TickParams
  // ---- the current field, the RENDER copy (plan component 8) --------------
  // The same resolved list TickParams carries, in the same encoding, from the
  // same CurrentPrimSystem in the same frame — the windPrims argument exactly.
  // It is what makes the surface waves drift with the flow (component 9), what
  // the arrow overlay draws, and what the foam reads its convergence from; a
  // consumer with its own idea of where the water is going would be a picture
  // of a different current.
  //
  // The render copy carries its OWN mode word rather than reading the sim's,
  // because the two answer different questions: `currentMode` gates whether a
  // sim kernel may read the field (rule 1 territory), and `currentRenderOn`
  // gates whether the renderer draws it. The render arm ships ON while the sim
  // arm ships OFF, which is the whole shape of M4 — see DESIGN.md §9c.
  currentRenderOn  : u32,
  currentPrimCount : u32,
  _pcpr0 : u32,
  _pcpr1 : u32,
  currentPrimLo : vec3<i32>,
  _pcpr2 : i32,
  currentPrimHi : vec3<i32>,
  _pcpr3 : i32,
  currentPrims : array<vec4<i32>, 96>,   // 3 * CURRENT_PRIM_CAP
  // ---- component 9: impact ripples ---------------------------------------
  // A BOUNDED ring of recent water impacts, each drawn as an analytic
  // expanding ring with amplitude decay. Plan component 9 names the upgrade
  // path (a per-body 2D wave-equation texture, i.e. stored state plus a
  // solver) and says DO NOT START THERE — so this is the windAt() idiom
  // instead: the displacement is a pure function of (eventList, t), the list is
  // fixed-size, and an empty list costs one compare.
  //
  // (x, z) in world VOXELS, `t0` the R.time the impact happened at, `amp` its
  // strength in metres of initial crest. amp <= 0 is a dead slot.
  waveImpactCount : u32,
  _pwi0 : u32,
  _pwi1 : u32,
  _pwi2 : u32,
  waveImpacts : array<vec4f, 16>,   // WAVE_IMPACT_CAP
  // ---- shadow cache (must match RenderParams in world.h) ----
  // frameIdx is the RENDER frame counter, not `tick` (30 Hz, several frames per
  // tick) and not `time` (a float that cannot be compared for equality). Only
  // its low 4 bits are ever used.
  frameIdx : u32,
  shadowSubdiv : u32,   // voxel-face subdivision per axis, <= SHADOW_SUBDIV_MAX
  _psc0 : u32,
  _psc1 : u32,
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

// The COSMETIC 3-variant decode, keyed on an arbitrary number rather than on a
// voxel's state nibble. Every ordinary material's texture is this: `% 3` over
// the three authored `colors`.
//
// It is a SEPARATE function from paletteColor() below, and the split is the
// whole point. A caller has one of two things in hand and they are not
// interchangeable:
//
//   * a real STATE NIBBLE, read off a voxel word — paletteColor()
//   * a positional JITTER HASH it invented, because the thing it is shading has
//     no nibble of its own (a micro-body brick cell, a far-cascade LOD cell, a
//     sub-voxel strand) — paletteJitter()
//
// paletteColor() reads a MATF_TINTED material's nibble as a DYE INDEX, so
// handing it a hash dithers the surface across the material's entire dye run,
// voxel by voxel — plus, past the authored count, across the unwritten black
// entries of that run. Measured the day tints were first authored: 90.6% of
// sword.vox is steel, and every one of those voxels drew one of 16 dyes with
// 49.4% of them landing on a black entry. A held sword and every legacy mob
// (asha/mina/wizard: one material per colour, so `hitArt == 0` for 66-76% of
// their voxels) rendered as a zebra.
//
// A hash is not a state. A dyed material still has its three cosmetic variants
// and this is what asks for them, so an unpainted tinted voxel textures exactly
// as it did before tints existed.
fn paletteJitter(m : Material, jitter : u32) -> vec3f {
  switch (jitter % 3u) {
    case 0u: { return unpackColor(m.color0); }
    case 1u: { return unpackColor(m.color1); }
    default: { return unpackColor(m.color2); }
  }
}

// The state-nibble palette is a property of Material (declared above), so its
// decode belongs here rather than being re-derived by each render path. There
// are TWO decodes and the material picks which: the 3-variant cosmetic jitter
// every ordinary material uses, and the 16-entry TINT run a MATF_TINTED one
// uses instead (sim/materials.h).
//
// `state` MUST be a real state nibble — see paletteJitter() above for the one
// that takes a hash, and for what feeding one to this function costs.
//
// `pal` is the material table itself, threaded in as a pointer because a tinted
// material resolves against the reserved tint run and common.wgsl is prepended
// BEFORE each shader declares its own `materials` binding — so this function
// cannot name it. Passing it keeps ONE definition shared by the raymarcher, the
// cube path and the brick march, which is the whole point of this function
// living here: a live mob's arm and the severed one beside it must shade
// identically, and a copy cannot enforce that.
fn paletteColor(m : Material, state : u32,
                pal : ptr<storage, array<Material>, read>) -> vec3f {
  if ((m.flags & MATF_TINTED) != 0u) {
    // Direct index, NOT `% 3`: state 0..15 maps straight onto the run, and tint
    // 0 is authored as the material's natural colour so a voxel that arrives
    // with state 0 (an old save, a CPU path that never heard of tints) reads as
    // undyed rather than as some arbitrary dye.
    let base = (m.flags >> MATF_TINT_BASE_SHIFT) & MATF_TINT_BASE_MASK;
    return unpackColor((*pal)[TINT_PALETTE_BASE + base + (state & 0xFu)].color0);
  }
  return paletteJitter(m, state);
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
// THE UNIFORM ARRIVES BY POINTER, NOT BY VALUE, AND THAT IS LOAD-BEARING.
// See the WIND UNIFORMS ARE PASSED BY POINTER note above RenderParams.
fn windSampleAt(p : vec3f, t : f32, ph : f32,
                R : ptr<uniform, RenderParams>) -> WindSample {
  var s : WindSample;
  // R.windDir is a unit XZ vector resolved on the CPU. WindWeather()
  // (src/sim/wind.h) is its ONLY author — auto weather and the manual override
  // come out of the same function, and phase 4's TickParams copy will too, so
  // the sim and the renderer cannot end up in different weather.
  let d = (*R).windDir;
  s.along = d;
  s.crossw = vec2f(-d.y, d.x);
  let alt = windAltRamp(p.y);
  s.mean = d * ((*R).windSpeed * alt);
  s.amp = (*R).windGust * alt;
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

// ====================== WIND PRIMITIVES (research doc §4.3) =================
// Fans, spell gusts, tornadoes. A primitive is ~48 bytes of parameters summed
// ANALYTICALLY at the sample point, exactly the way a point light is: no
// lattice, no resolution to pick, nothing stored per voxel or per chunk. The
// whole list rides the uniform, so a scene with none of them costs one compare.
//
// THREE SHAPES, and they are summable, which is what stops the set from having
// to grow: a directed jet (fan / gust bolt / wind wall), an omnidirectional
// push or pull (blast front / vacuum), and a swirl about an axis (tornado /
// whirlpool). A moving primitive is not animated here — its position was
// resolved on the CPU this tick from `origin + vel * age`, so the shader sees
// where it IS and never has to know when it started.
//
// PROFILES. Radial falloff is QUADRATIC in the distance from the axis
// (1 - r²/R²) and axial falloff is LINEAR (1 - a/L). Quadratic radially
// because it costs no square root — r² is what the dot products already give —
// and because a soft-shouldered jet reads better than a hard-edged one. The
// integer transcription below relies on that choice: there is no isqrt
// anywhere in the primitive path, which is what keeps it affordable in the CA's
// inner loop.
//
// THE BURST HAS NO WIND AT ITS EXACT CENTRE, and that is honest rather than a
// bug: `v = d * k` takes its direction from the offset itself (again, no square
// root), so the magnitude rises from zero at the origin, peaks partway out and
// tapers to nothing at the rim. A blast centre genuinely has no preferred
// outward direction, and one cell of stagnation is invisible next to the shell.

// Vortex inflow as a fraction of the tangential swirl. A tornado that only
// span would never gather anything into itself; one that only sucked would be
// a drain. 0.30 is the ratio that makes debris spiral IN rather than orbit or
// fall straight down, and it is a constant rather than a knob because it is a
// property of the shape, not of a particular tornado.
const WPRIM_INFLOW : f32 = 0.30;
const WPRIM_INFLOW_Q8 : i32 = 77;   // the same 0.30, Q8, for the integer path

// One primitive's contribution at a float sample point, world cells/s.
// TRANSCRIBED from — and by — the integer evaluator windPrimEvalQ below; the
// two are adjacent for the reason windAt and windAtQ are, and the same standing
// obligation applies: change a profile in one, change it in the other in the
// same edit. Float here rather than a conversion of the integer answer because
// a blade of grass sits at a fractional position and quantising it to whole
// cells makes a fan's rim band visibly across a meadow.
fn windPrimEvalF(p : vec3f, w0 : vec4<i32>, w1 : vec4<i32>,
                 w2 : vec4<i32>) -> vec3f {
  let kind = u32(w0.w) & WPRIM_KIND_MASK;
  let pos = vec3f(f32(w0.x), f32(w0.y), f32(w0.z));
  let dir = vec3f(f32(w1.x), f32(w1.y), f32(w1.z)) * (1.0 / 65536.0);
  let s = f32(w1.w) * (1.0 / 65536.0);      // cells/s, envelope already applied
  let rad = max(f32(w2.x), 1.0);
  let len = max(f32(w2.y), 1.0);
  let swirl = f32(w2.z) * (1.0 / 65536.0);
  let rise = f32(w2.w) * (1.0 / 65536.0);

  let d = p - pos;
  let d2 = dot(d, d);
  let rr = rad * rad;

  if (kind == WPRIM_BURST) {
    if (d2 > rr) { return vec3f(0.0); }
    return d * ((s / rad) * (1.0 - d2 / rr));
  }

  let ax = dot(d, dir);
  let r2 = max(0.0, d2 - ax * ax);
  if (r2 > rr) { return vec3f(0.0); }
  let radW = 1.0 - r2 / rr;

  if (kind == WPRIM_VORTEX) {
    if (abs(ax) > len) { return vec3f(0.0); }
    let axW = 1.0 - abs(ax) / len;
    let perp = d - dir * ax;              // radial offset from the axis
    let tang = cross(dir, perp);          // same length as perp, 90 deg round
    let g = (s / rad) * radW * axW;
    return (tang * swirl - perp * WPRIM_INFLOW) * g +
           dir * (rise * s * radW * axW);
  }

  // WPRIM_CONE: a jet, live only in front of the mouth.
  if (ax < 0.0 || ax > len) { return vec3f(0.0); }
  return dir * (s * radW * (1.0 - ax / len));
}

// The primitive sum at a point, world cells/s. Zero primitives is one compare;
// a point outside every footprint is four.
fn windPrimAt(p : vec3f, R : ptr<uniform, RenderParams>) -> vec3f {
  if ((*R).windPrimCount == 0u) { return vec3f(0.0); }
  // The UNION AABB reject. Without it every sample in the world would run the
  // per-primitive test 32 times to discover it is nowhere near any of them;
  // with it, the loop is paid only where a primitive actually is. `lo > hi` is
  // the empty convention, so this also covers a count that outran the list.
  let pi = vec3<i32>(floor(p));
  if (any(pi < (*R).windPrimLo) || any(pi > (*R).windPrimHi)) {
    return vec3f(0.0);
  }
  var acc = vec3f(0.0);
  let n = min((*R).windPrimCount, WIND_PRIM_CAP);
  for (var i = 0u; i < n; i = i + 1u) {
    let b = i * WIND_PRIM_ROWS;
    acc += windPrimEvalF(p, (*R).windPrims[b], (*R).windPrims[b + 1u],
                         (*R).windPrims[b + 2u]);
  }
  return acc;
}

// THE FIELD. Everything above exists so that this and the per-blade path are
// the same arithmetic. Call this unless you need per-instance decorrelation.
fn windAt(p : vec3f, t : f32, R : ptr<uniform, RenderParams>) -> vec3f {
  let s = windSampleAt(p, t, 0.0, R);
  return windMeanWS(s) + windBandWS(s, s.b1) * WIND_BAND_W1
                       + windBandWS(s, s.b2) * WIND_BAND_W2
                       + windPrimAt(p, R);
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
fn windAtScaledQ(p : vec3<i32>, T : ptr<uniform, TickParams>,
                 scaleQ8 : i32) -> vec3<i32> {
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
fn windDragRampQ(w : vec3<i32>, T : ptr<uniform, TickParams>) -> i32 {
  let mag = max(max(abs(w.x), abs(w.y)), abs(w.z));
  return min(65536, mag / max((*T).windDragRefQ >> 16u, 1));
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

// ---- wind primitives, in integers (research doc §4.3) ----------------------
// The transcription of windPrimEvalF above. Same three shapes, same profiles,
// same constants — and, like windAtQ against windAt, it is transcribed rather
// than shared because the two number systems cannot share code, and the only
// defence against drift is that they sit in one file with this note between
// them. If you change a profile up there, change it here in the same edit.
//
// OVERFLOW, which is the whole difficulty and is bounded by construction:
//   * the Chebyshev reject caps |d| at max(radius, reach) + 1 <= 513, so
//     d2 <= 3 * 513^2 and every dot product stays under 2^27.
//   * radial/axial weights are carried in Q10 (1024 = 1.0), never Q16.16, so
//     `r2 * 1024` cannot leave i32 for any legal radius.
//   * the burst and vortex scale by `strength / radius` BEFORE multiplying by
//     an offset that is itself bounded by the radius. The radius cancels, so
//     the product is bounded by the strength however the two are chosen — that
//     cancellation is why there is no clamp in the inner loop.
//   * strength is capped CPU-side at kWindPrimMaxSpeed (400 cells/s, 2^24.6)
//     and the list at 32, so the accumulated sum cannot leave i32 either.
//
// PRECISION. The burst/vortex per-cell gradient is formed as
// (strength/radius / 64) * (weight / 16) rather than the more obvious
// (.../1024) * weight, which is the same scale with four more bits kept — the
// obvious form silently truncates a weak, wide burst to nothing.
fn windPrimEvalQ(p : vec3<i32>, w0 : vec4<i32>, w1 : vec4<i32>,
                 w2 : vec4<i32>) -> vec3<i32> {
  let rad = max(w2.x, 1);
  let len = max(w2.y, 1);
  let d = p - w0.xyz;
  // Chebyshev reject before anything is squared: this is what bounds every
  // intermediate below, so it must come first and must use the LARGER extent.
  let ext = max(rad, len) + 1;
  if (max(max(abs(d.x), abs(d.y)), abs(d.z)) > ext) { return vec3<i32>(0); }

  let kind = u32(w0.w) & WPRIM_KIND_MASK;
  let dir = w1.xyz;                        // Q16.16 unit axis
  let sQ = w1.w;                           // Q16.16 cells/s, envelope applied
  let d2 = d.x * d.x + d.y * d.y + d.z * d.z;
  let rr = rad * rad;

  if (kind == WPRIM_BURST) {
    if (d2 > rr) { return vec3<i32>(0); }
    let t10 = 1024 - (d2 * 1024) / rr;               // 0..1024
    let perQ = sQ / rad;                             // Q16.16 per cell
    let tapQ = (perQ / 64) * (t10 / 16);             // = perQ * t10 / 1024
    return d * tapQ;                                 // |d| <= rad, so <= sQ
  }

  let ax = (d.x * dir.x + d.y * dir.y + d.z * dir.z) / 65536;   // cells
  let r2 = max(0, d2 - ax * ax);
  if (r2 > rr) { return vec3<i32>(0); }
  let rad10 = 1024 - (r2 * 1024) / rr;

  if (kind == WPRIM_VORTEX) {
    if (abs(ax) > len) { return vec3<i32>(0); }
    let ax10 = 1024 - (abs(ax) * 1024) / len;
    let g10 = (rad10 * ax10) / 1024;
    // Radial offset from the axis, and the tangent 90 degrees round it. Both
    // in whole cells and both bounded by the radius, which is what lets them
    // be multiplied by the per-cell gradient without a clamp.
    let along = vec3<i32>((dir.x * ax) / 65536, (dir.y * ax) / 65536,
                          (dir.z * ax) / 65536);
    let perp = d - along;
    let tang = vec3<i32>((dir.y * perp.z - dir.z * perp.y) / 65536,
                         (dir.z * perp.x - dir.x * perp.z) / 65536,
                         (dir.x * perp.y - dir.y * perp.x) / 65536);
    let sw = w2.z >> 8u;                             // swirl, Q8
    let mix = vec3<i32>((tang.x * sw - perp.x * WPRIM_INFLOW_Q8) / 256,
                        (tang.y * sw - perp.y * WPRIM_INFLOW_Q8) / 256,
                        (tang.z * sw - perp.z * WPRIM_INFLOW_Q8) / 256);
    let perQ = sQ / rad;
    let gQ = (perQ / 64) * (g10 / 16);
    // Axial lift: a share of the full strength, not of the per-cell gradient,
    // because a tornado lofts at its own speed rather than in proportion to
    // how far out you stand.
    let liftQ = (wq(w2.w, sQ) / 1024) * g10;
    return mix * gQ + vec3<i32>(wq(dir.x, liftQ), wq(dir.y, liftQ),
                                wq(dir.z, liftQ));
  }

  // WPRIM_CONE.
  if (ax < 0 || ax > len) { return vec3<i32>(0); }
  let ax10 = 1024 - (ax * 1024) / len;
  let w10 = (rad10 * ax10) / 1024;
  let sw = (sQ / 1024) * w10;
  return vec3<i32>(wq(dir.x, sw), wq(dir.y, sw), wq(dir.z, sw));
}

// The primitive sum at a world cell, Q16.16 cells/s. Zero primitives is one
// compare, which is what makes this hash-neutral until someone places a fan.
fn windPrimAtQ(p : vec3<i32>, T : ptr<uniform, TickParams>) -> vec3<i32> {
  if ((*T).windPrimCount == 0u) { return vec3<i32>(0); }
  if (any(p < (*T).windPrimLo) || any(p > (*T).windPrimHi)) {
    return vec3<i32>(0);
  }
  var acc = vec3<i32>(0);
  let n = min((*T).windPrimCount, WIND_PRIM_CAP);
  for (var i = 0u; i < n; i = i + 1u) {
    let b = i * WIND_PRIM_ROWS;
    acc += windPrimEvalQ(p, (*T).windPrims[b], (*T).windPrims[b + 1u],
                         (*T).windPrims[b + 2u]);
  }
  return acc;
}

// IS THIS CELL INSIDE A PRIMITIVE THAT LICENSES ENTRAINMENT (invariant 4)?
//
// This is the gate that makes settled powder movable, and it is deliberately a
// SEPARATE, cheaper question than "what is the wind here". Entrainment is the
// one rule in the engine that makes resting matter move, and the page table's
// materialization set is only sound because resting matter writes nothing
// (RESEARCH_wind.md §10 — switching it on globally lost 62 voxels to page
// faults). A cell may therefore be entrained only where a primitive DECLARED
// its footprint this tick, because declaring it is what put the chunk in
// `opTargets` and got its page materialized before the dispatch that writes it.
//
// The test is the primitive's own geometry with the profiles left out: being
// inside is a yes/no, and asking the full evaluator would pay for three
// multiplies of a weight nobody reads.
fn windPrimEntrainsQ(p : vec3<i32>, T : ptr<uniform, TickParams>) -> bool {
  if ((*T).windPrimCount == 0u) { return false; }
  if (any(p < (*T).windPrimLo) || any(p > (*T).windPrimHi)) { return false; }
  let n = min((*T).windPrimCount, WIND_PRIM_CAP);
  for (var i = 0u; i < n; i = i + 1u) {
    let b = i * WIND_PRIM_ROWS;
    let w0 = (*T).windPrims[b];
    if (((u32(w0.w) >> WPRIM_F_SHIFT) & WPRIM_F_ENTRAIN) == 0u) { continue; }
    let w2 = (*T).windPrims[b + 2u];
    let rad = max(w2.x, 1);
    let len = max(w2.y, 1);
    let d = p - w0.xyz;
    let ext = max(rad, len) + 1;
    if (max(max(abs(d.x), abs(d.y)), abs(d.z)) > ext) { continue; }
    let d2 = d.x * d.x + d.y * d.y + d.z * d.z;
    let rr = rad * rad;
    let kind = u32(w0.w) & WPRIM_KIND_MASK;
    if (kind == WPRIM_BURST) {
      if (d2 <= rr) { return true; }
      continue;
    }
    let dir = (*T).windPrims[b + 1u].xyz;
    let ax = (d.x * dir.x + d.y * dir.y + d.z * dir.z) / 65536;
    if (max(0, d2 - ax * ax) > rr) { continue; }
    if (kind == WPRIM_VORTEX) {
      if (abs(ax) <= len) { return true; }
      continue;
    }
    if (ax >= 0 && ax <= len) { return true; }
  }
  return false;
}

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
fn windAtQ(p : vec3<i32>, T : ptr<uniform, TickParams>) -> vec3<i32> {
  if ((*T).windMode == WIND_MODE_OFF) { return vec3<i32>(0, 0, 0); }
  let alt = clamp(65536 + (p.y - WINDQ_ALT_REF_Y) * WINDQ_ALT_GAIN,
                  WINDQ_ALT_MIN, WINDQ_ALT_MAX);
  let d = (*T).windDirQ;                 // unit XZ, downwind, Q16.16
  let cw = vec2<i32>(-d.y, d.x);         // 90 degrees to its left
  let spd = wq((*T).windSpeedQ, alt);
  let amp = wq((*T).windGustQ, alt);

  // Band components in the (along, cross, up) frame. Five spatial phases and
  // six clocks, exactly as windSampleAt builds them with ph = 0.
  let tick = (*T).tick;
  let b1a = windSinQ(i32(windPhaseQ(p, d, WINDQ_K_100) +
                         windClockQ(tick, WINDQ_T_100)));
  let b1c = wq(WINDQ_CROSS,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_120) +
                            windClockQ(tick, WINDQ_T_083)) + WINDQ_PH_21));
  let b1u = wq(WINDQ_VERT,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_070) +
                            windClockQ(tick, WINDQ_T_131))));
  let b2a = windSinQ(i32(windPhaseQ(p, d, WINDQ_K_050) +
                         windClockQ(tick, WINDQ_T_173)));
  let b2c = wq(WINDQ_CROSS, windSinQ(i32(windClockQ(tick, WINDQ_T_219))));
  let b2u = wq(WINDQ_VERT,
               windSinQ(i32(windPhaseQ(p, d, WINDQ_K_030) +
                            windClockQ(tick, WINDQ_T_261))));

  // Rotate each band out of that frame into world space and scale by the gust
  // amplitude — windBandWS(), in integers.
  let w1 = vec3<i32>(wq(amp, wq(d.x, b1a) + wq(cw.x, b1c)),
                     wq(amp, b1u),
                     wq(amp, wq(d.y, b1a) + wq(cw.y, b1c)));
  let w2 = vec3<i32>(wq(amp, wq(d.x, b2a) + wq(cw.x, b2c)),
                     wq(amp, b2u),
                     wq(amp, wq(d.y, b2a) + wq(cw.y, b2c)));
  // windMeanWS() + the 0.7/0.3 mix, then the primitive sum. The primitives are
  // ADDED to the ambient field rather than replacing it, which is what makes a
  // fan feel like it is blowing INTO weather instead of switching the weather
  // off inside a box — and it is why a gust bolt fired downwind carries further
  // than one fired upwind with no code saying so.
  return vec3<i32>(wq(d.x, spd), 0, wq(d.y, spd)) +
         vec3<i32>(wq(WINDQ_W1, w1.x), wq(WINDQ_W1, w1.y), wq(WINDQ_W1, w1.z)) +
         vec3<i32>(wq(WINDQ_W2, w2.x), wq(WINDQ_W2, w2.y), wq(WINDQ_W2, w2.z)) +
         windPrimAtQ(p, T);
}

// ============================ THE CURRENT FIELD =============================
// docs/PLAN_water_master.md component 8. DESIGN.md §9c states the invariants.
//
// A CLONE OF THE WIND FIELD, and the word is meant literally: same three-row
// primitive packing, same union-AABB whole-loop reject, same float/integer
// transcription pair sitting adjacent in one file, same `ptr<uniform, T>` rule.
// Water flow is the same KIND of object as wind — a bounded list of parametric
// shapes summed analytically at a sample point, like point lights — and the one
// thing that would have made it harder is inventing a second shape for it.
//
// SUPERPOSITION ONLY. There is no neighbour coupling, no stored field, no
// relaxation pass and nothing to wake. The behaviour that motivated the field —
// a river running into a pool and dissipating outward — is what a point SOURCE
// does for free under superposition (plan §0 correction 4); a real vector
// diffusion would mean stored state, a solver, per-tick cost and a system that
// does not sleep. Superposition of sources, sinks and vortices is a real
// solution of Laplace's equation, not a hack: incompressible irrotational flow
// away from boundaries is approximately what pond water does.
//
// THE FIELD OWNS NO MASS, and that is what makes its accuracy envelope
// acceptable. No no-flow boundary at terrain, no separation, no eddies behind
// obstacles, no turbulence. A wrong current pushes a leaf the wrong way; it
// cannot lose an eighth of water. The ledger (component 3) is the only thing
// that moves mass, and it never reads this.
//
// UNITS. World CELLS PER SECOND, matching windAt() exactly so the two can be
// compared directly by anyone debugging them, and Q16.16 in the integer arm for
// the same reason. Each consumer converts to its own per-tick units at its own
// call site with a const factor.
//
// THE SINK/VORTEX ASYMMETRY IS THE WHOLE LOOK. The sink is 1/r^2 and is only a
// couple of cells wide at any realistic discharge; the vortex is Gamma/2*pi*r
// and reaches far. That is why real whirlpools look enormous while the actual
// suction is a small throat: design the visible danger around the tangential
// term and the lethality around the sink.

// Vortex inflow as a fraction of the tangential swirl — WPRIM_INFLOW's
// argument, at a whirlpool's scale: a vortex that only span would never gather
// anything into itself. A property of the shape, so a constant and not a knob.
const CPRIM_INFLOW : f32 = 0.22;
const CPRIM_INFLOW_Q8 : i32 = 56;   // the same 0.22, Q8, for the integer path

// Exact integer floor(sqrt(v)), v in [0, 2^30). Restoring binary method,
// constant time, no float and no table.
//
// WHY THERE IS A SQUARE ROOT HERE AND NONE IN THE WIND BLOCK. windPrimEvalQ
// gets away without one because every wind profile is quadratic in the distance
// (1 - r^2/R^2), which r^2 already gives. The two profiles this field exists
// for are NOT: Gamma/2*pi*r and 1/r^2 are both about the true radius, and
// faking them with r^2 would make a whirlpool's tangential speed fall off as
// 1/r^2 — which is the sink's profile, i.e. exactly the "reaches far" property
// the vortex is here to provide. So one isqrt is paid, and it is paid ONCE per
// primitive per sample: sink, source and vortex all consume the same radius.
fn curISqrt(v : i32) -> i32 {
  var x = max(v, 0);
  var res = 0;
  var bit = 1 << 30u;
  // Both loops are bounded by the constant above, so this is fixed-cost.
  while (bit > x) { bit = bit >> 2u; }
  while (bit != 0) {
    if (x >= res + bit) {
      x = x - (res + bit);
      res = (res >> 1u) + bit;
    } else {
      res = res >> 1u;
    }
    bit = bit >> 2u;
  }
  return res;
}

// One primitive's contribution at a float sample point, world cells/s.
// TRANSCRIBED from — and by — the integer evaluator currentPrimEvalQ below, the
// same standing obligation windPrimEvalF/Q carry: change a profile in one,
// change it in the other in the same edit. Float here rather than a conversion
// of the integer answer because the renderer samples at fractional positions
// and quantising a whirlpool to whole cells makes its rings visibly staircase.
//
// PACKING (must match CurrentPrimGpu in src/sim/currentprim.h):
//   w0 = (x, y, z, kind | flags << 4)          origin, world cells
//   w1 = (dirX, dirY, dirZ, strengthQ)         unit axis Q16.16, cells/s Q16.16
//   w2 = (radius, reach, swirlQ, riseQ)        cells, cells, Q16.16, Q16.16
//
// `strength` is the speed AT THE CORE RADIUS for every kind, which is what lets
// the four shapes share one cap and one overflow argument: the profile can only
// ever attenuate it.
fn currentPrimEvalF(p : vec3f, w0 : vec4<i32>, w1 : vec4<i32>,
                    w2 : vec4<i32>) -> vec3f {
  let kind = u32(w0.w) & CPRIM_KIND_MASK;
  let pos = vec3f(f32(w0.x), f32(w0.y), f32(w0.z));
  let dir = vec3f(f32(w1.x), f32(w1.y), f32(w1.z)) * (1.0 / 65536.0);
  let s = f32(w1.w) * (1.0 / 65536.0);      // cells/s, envelope already applied
  let rad = max(f32(w2.x), 1.0);
  let len = max(f32(w2.y), 1.0);
  let swirl = f32(w2.z) * (1.0 / 65536.0);
  let rise = f32(w2.w) * (1.0 / 65536.0);
  // THE CORE. r -> 0 is a pole in both profiles, so both are clamped to a
  // throat an eighth of the footprint across. This is not a numerical fudge:
  // a real drain has a physical orifice and the flow inside it is bounded by
  // the discharge, not by 1/r^2. Floor of 1 cell, because a sub-voxel throat
  // is not a thing this engine can draw.
  let core = max(rad * 0.125, 1.0);

  let d = p - pos;
  let d2 = dot(d, d);
  let rr = rad * rad;

  if (kind == CPRIM_SINK || kind == CPRIM_SOURCE) {
    if (d2 > rr) { return vec3f(0.0); }
    let r = sqrt(d2);
    let rc = max(r, core);
    // Soft rim, quadratic, the wind block's idiom — the field must reach zero
    // at its declared footprint or the AABB reject would be a visible edge.
    let edge = 1.0 - d2 / rr;
    let mag = s * (core * core) / (rc * rc) * edge;
    let sgn = select(1.0, -1.0, kind == CPRIM_SINK);
    return (d / rc) * (mag * sgn);
  }

  let ax = dot(d, dir);
  let r2 = max(0.0, d2 - ax * ax);
  if (r2 > rr) { return vec3f(0.0); }
  if (abs(ax) > len) { return vec3f(0.0); }
  let radW = 1.0 - r2 / rr;
  let axW = 1.0 - abs(ax) / len;

  if (kind == CPRIM_VORTEX) {
    let perp = d - dir * ax;             // radial offset from the axis
    let rp = sqrt(r2);
    let rpc = max(rp, core);
    let tang = cross(dir, perp);         // same length as perp, 90 deg round
    // v_theta = Gamma / (2 pi r), written as s * core / r so that `s` means
    // "the tangential speed at the core" for every kind. Gamma is then
    // 2 pi core s, and the CPU is what converts sim.currentVortexGamma into it.
    let vt = s * core / rpc * radW * axW;
    return (tang / rpc) * (vt * swirl) - (perp / rpc) * (vt * CPRIM_INFLOW) +
           dir * (rise * s * radW * axW);
  }

  // CPRIM_STREAM: uniform flow along the axis, tapered to nothing at the rim
  // and at both ends. Symmetric in `ax` (unlike WPRIM_CONE, which is a mouth):
  // a reach of river has no front.
  return dir * (s * radW * axW);
}

// The primitive sum at a point, world cells/s. Zero primitives is one compare;
// a point outside every footprint is four.
// THE UNIFORM ARRIVES BY POINTER — see the WIND UNIFORMS note above
// RenderParams. This function dynamically indexes a uniform array, which is the
// exact shape that cost 220 ms/frame; plan §9 ranks it as risk 5.
fn currentPrimAt(p : vec3f, R : ptr<uniform, RenderParams>) -> vec3f {
  if ((*R).currentPrimCount == 0u) { return vec3f(0.0); }
  let pi = vec3<i32>(floor(p));
  if (any(pi < (*R).currentPrimLo) || any(pi > (*R).currentPrimHi)) {
    return vec3f(0.0);
  }
  var acc = vec3f(0.0);
  let n = min((*R).currentPrimCount, CURRENT_PRIM_CAP);
  for (var i = 0u; i < n; i = i + 1u) {
    let b = i * CURRENT_PRIM_ROWS;
    acc += currentPrimEvalF(p, (*R).currentPrims[b], (*R).currentPrims[b + 1u],
                            (*R).currentPrims[b + 2u]);
  }
  return acc;
}

// THE FIELD, render side. World cells/s at world position `p`.
//
// There is no ambient term to add — unlike wind, which has weather everywhere,
// still water is still. The stream arm is a PRIMITIVE (CPRIM_STREAM, seeded
// from the landform bed gradient on the CPU), not a global, because a river is
// somewhere and a wind is everywhere.
fn currentAt(p : vec3f, R : ptr<uniform, RenderParams>) -> vec3f {
  if ((*R).currentRenderOn == 0u) { return vec3f(0.0); }
  return currentPrimAt(p, R);
}

// CONVERGENCE of the field at a point, per second — negative divergence, so
// positive means "the flow is piling up here". This is what foam rides.
//
// A CENTRAL DIFFERENCE, not a symbolic derivative, and that is a deliberate
// choice rather than laziness: the analytic divergence of the sum would be a
// THIRD transcription of every profile, drifting from the other two with
// nothing checking it, for four evaluations of a function that early-outs to
// one compare outside the AABB. `e` is in world cells.
fn currentConvergeAt(p : vec3f, R : ptr<uniform, RenderParams>) -> f32 {
  if ((*R).currentRenderOn == 0u || (*R).currentPrimCount == 0u) { return 0.0; }
  let e = 1.5;
  let vx1 = currentPrimAt(p + vec3f(e, 0.0, 0.0), R).x;
  let vx0 = currentPrimAt(p - vec3f(e, 0.0, 0.0), R).x;
  let vz1 = currentPrimAt(p + vec3f(0.0, 0.0, e), R).z;
  let vz0 = currentPrimAt(p - vec3f(0.0, 0.0, e), R).z;
  return -((vx1 - vx0) + (vz1 - vz0)) / (2.0 * e);
}

// ==================== THE CURRENT FIELD, IN INTEGERS ========================
// The transcription of currentPrimEvalF above, for the same reason windAtQ is
// the transcription of windAt: everything below is read by sim kernels whose
// output reaches the grid, so rule 1 applies — integer only, no f32 anywhere,
// square root included. f32 `sqrt()` is not bit-identical between vendors and
// the world hash is compared across machines.
//
// OVERFLOW, bounded by construction exactly as windPrimEvalQ's is:
//   * the Chebyshev reject caps |d| at max(radius, reach) + 1 <= 513, so
//     d2 <= 3 * 513^2 < 2^20 and every dot product stays well inside i32.
//   * weights are carried in Q10 (1024 = 1.0), never Q16.16.
//   * `core <= rc` and `core <= rpc` by construction, so the 1/r^2 and 1/r
//     numerators can only ATTENUATE the strength — the radius cancels and
//     there is no clamp in the inner loop.
//   * strength is capped CPU-side at kCurrentPrimMaxSpeed and the list at 32,
//     so the accumulated sum cannot leave i32 either.
//
// PRECISION. Per-cell gradients are formed as (magQ / 64) * (w10 / 16) rather
// than the more obvious (.../1024) * w10 — the same scale with four more bits
// kept, which is what stops a weak, wide current truncating to nothing.
fn currentPrimEvalQ(p : vec3<i32>, w0 : vec4<i32>, w1 : vec4<i32>,
                    w2 : vec4<i32>) -> vec3<i32> {
  let rad = max(w2.x, 1);
  let len = max(w2.y, 1);
  let d = p - w0.xyz;
  // Chebyshev reject before anything is squared: this is what bounds every
  // intermediate below, so it must come first and must use the LARGER extent.
  let ext = max(rad, len) + 1;
  if (max(max(abs(d.x), abs(d.y)), abs(d.z)) > ext) { return vec3<i32>(0); }

  let kind = u32(w0.w) & CPRIM_KIND_MASK;
  let dir = w1.xyz;                        // Q16.16 unit axis
  let sQ = w1.w;                           // Q16.16 cells/s, envelope applied
  let d2 = d.x * d.x + d.y * d.y + d.z * d.z;
  let rr = rad * rad;
  let core = max(rad / 8, 1);              // currentPrimEvalF's rad * 0.125

  if (kind == CPRIM_SINK || kind == CPRIM_SOURCE) {
    if (d2 > rr) { return vec3<i32>(0); }
    let rc = max(curISqrt(d2), core);
    // (core/rc)^2 in Q10. core <= rc, so this is 0..1024 and cannot overflow:
    // core*core*1024 <= 64*64*1024 = 2^22.
    let inv10 = (core * core * 1024) / (rc * rc);
    let edge10 = 1024 - (d2 * 1024) / rr;
    let w10 = (inv10 * edge10) / 1024;
    let magQ = (sQ / 64) * (w10 / 16);     // = sQ * w10 / 1024
    // Unit-ish offset in Q16.16: |dirU| = 65536 * min(r, rc) / rc <= 65536,
    // which is what keeps the wq() product inside i32.
    let dirU = vec3<i32>((d.x * 65536) / rc, (d.y * 65536) / rc,
                         (d.z * 65536) / rc);
    let v = vec3<i32>(wq(dirU.x, magQ), wq(dirU.y, magQ), wq(dirU.z, magQ));
    return select(v, -v, kind == CPRIM_SINK);
  }

  let ax = (d.x * dir.x + d.y * dir.y + d.z * dir.z) / 65536;   // cells
  let r2 = max(0, d2 - ax * ax);
  if (r2 > rr) { return vec3<i32>(0); }
  if (abs(ax) > len) { return vec3<i32>(0); }
  let rad10 = 1024 - (r2 * 1024) / rr;
  let ax10 = 1024 - (abs(ax) * 1024) / len;
  let w10 = (rad10 * ax10) / 1024;

  if (kind == CPRIM_VORTEX) {
    let along = vec3<i32>((dir.x * ax) / 65536, (dir.y * ax) / 65536,
                          (dir.z * ax) / 65536);
    let perp = d - along;                  // radial offset from the axis
    let rpc = max(curISqrt(perp.x * perp.x + perp.y * perp.y + perp.z * perp.z),
                  core);
    let tang = vec3<i32>((dir.y * perp.z - dir.z * perp.y) / 65536,
                         (dir.z * perp.x - dir.x * perp.z) / 65536,
                         (dir.x * perp.y - dir.y * perp.x) / 65536);
    // s * core / rpc — the Gamma/2*pi*r profile. Divide FIRST: core <= rpc, so
    // the quotient can only shrink sQ and nothing here can leave i32.
    let vtQ = (sQ / rpc) * core;
    let magQ = (vtQ / 64) * (w10 / 16);
    let tangU = vec3<i32>((tang.x * 65536) / rpc, (tang.y * 65536) / rpc,
                          (tang.z * 65536) / rpc);
    let perpU = vec3<i32>((perp.x * 65536) / rpc, (perp.y * 65536) / rpc,
                          (perp.z * 65536) / rpc);
    let sw = w2.z >> 8u;                   // swirl, Q8
    let swirlQ = (magQ / 256) * sw;
    let inQ = (magQ / 256) * CPRIM_INFLOW_Q8;
    // Axial lift: a share of the full strength, not of the per-cell gradient —
    // windPrimEvalQ's argument, and the same arithmetic.
    let liftQ = (wq(w2.w, sQ) / 1024) * w10;
    return vec3<i32>(wq(tangU.x, swirlQ) - wq(perpU.x, inQ),
                     wq(tangU.y, swirlQ) - wq(perpU.y, inQ),
                     wq(tangU.z, swirlQ) - wq(perpU.z, inQ)) +
           vec3<i32>(wq(dir.x, liftQ), wq(dir.y, liftQ), wq(dir.z, liftQ));
  }

  // CPRIM_STREAM.
  let sw = (sQ / 1024) * w10;
  return vec3<i32>(wq(dir.x, sw), wq(dir.y, sw), wq(dir.z, sw));
}

// THE FIELD, for the sim. Q16.16 world cells per second at world cell `p`.
//
// Returns the ZERO vector when sim.currentMode is off, which is what makes the
// whole of M4 hash-neutral by construction at the shipping default — but do NOT
// rely on that alone at a call site: a drag term reading zero current still
// drags. Every consumer gates on T.currentMode itself, and says so.
//
// THE AUTHORITY LINE, and it is the one thing about this field that is not a
// straight copy of wind. The seeders (currentprim.cpp) may only put a primitive
// on THIS list if its parameters are a pure function of the tick input stream.
// A drain's sink and vortex are seeded from the MUTATION that cut the hole —
// which rides the mutation queue and is therefore tick-deterministic — and NOT
// from the GPU ledger's WBS_EMIT, which the CPU could only learn from an async
// readback arriving on a schedule set by fence retirement. That is M1's §1.1
// correction 2 restated: "seeded when the CPU got around to noticing" is a
// scheduling-dependent outcome the moment a kernel reads it. Primitives that do
// not clear that bar carry CPRIM_F_SIM clear and reach the renderer only.
fn currentAtQ(p : vec3<i32>, T : ptr<uniform, TickParams>) -> vec3<i32> {
  if ((*T).currentMode == 0u) { return vec3<i32>(0, 0, 0); }
  if ((*T).currentPrimCount == 0u) { return vec3<i32>(0); }
  if (any(p < (*T).currentPrimLo) || any(p > (*T).currentPrimHi)) {
    return vec3<i32>(0);
  }
  var acc = vec3<i32>(0);
  let n = min((*T).currentPrimCount, CURRENT_PRIM_CAP);
  for (var i = 0u; i < n; i = i + 1u) {
    let b = i * CURRENT_PRIM_ROWS;
    let w0 = (*T).currentPrims[b];
    // Only primitives the CPU can defend as a pure function of the tick stream
    // reach a kernel. See the authority note above.
    if (((u32(w0.w) >> CPRIM_F_SHIFT) & CPRIM_F_SIM) == 0u) { continue; }
    acc += currentPrimEvalQ(p, w0, (*T).currentPrims[b + 1u],
                            (*T).currentPrims[b + 2u]);
  }
  return acc;
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
// DEAD SPAWN OPS this tick. The water-body discharge (M3, component 6) fills a
// CPU-RESERVED op block, and every slot in the block is written every tick —
// live while the hole is flowing, dead (mat 0) after it. A dead op still
// occupies a pool slot for one tick, so FA_LIVE counts it; this is what lets a
// conservation gate subtract it and read the real in-flight mass. Without it
// the only honest number would need a full pool scan.
const FA_SPAWNDEAD : u32 = 29u;
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

// ---- VOXEL-KEYED SHADOW CACHE (src/sim/world.h kShadowCacheBuckets) --------
// The identity and packing shared by the two halves of the cache:
// raymarch.wgsl READS a patch's shadow factor and REGISTERS the patch, and
// shadow_resolve.wgsl CASTS the ray and writes the value back. They must agree
// bit for bit about what a patch IS and where it lives, so the derivation lives
// here and in exactly one place.
//
// A PATCH is one face of one voxel, subdivided `subdiv` ways per axis. The
// granularity is a QUALITY knob, not a speed one — see the world.h block for
// why the win saturates long before the patch gets coarse.
//
// FACE ENCODING: axis * 2 + (normal points along +axis). 0..5, three bits.
fn shadowFaceOf(axis : i32, nPositive : bool) -> u32 {
  return u32(axis) * 2u + select(0u, 1u, nPositive);
}
fn shadowFaceNormal(face : u32) -> vec3f {
  let axis = face >> 1u;
  let s = select(-1.0, 1.0, (face & 1u) != 0u);
  var n = vec3f(0.0);
  n[axis] = s;
  return n;
}
// The two in-face axes. Fixed order (a+1, a+2) so the sub-patch indices mean
// the same thing on both sides.
fn shadowFaceTangents(face : u32) -> vec2<u32> {
  let axis = face >> 1u;
  return vec2<u32>((axis + 1u) % 3u, (axis + 2u) % 3u);
}
// The centre of a patch, in world voxel coords — the point the resolve pass
// casts from. Quantising the ray origin to this centre IS the approximation the
// whole cache rests on: every pixel on the patch gets the answer computed here.
fn shadowPatchCentre(cell : vec3<i32>, face : u32, sx : u32, sy : u32,
                     subdiv : u32) -> vec3f {
  let axis = face >> 1u;
  let t = shadowFaceTangents(face);
  let inv = 1.0 / f32(subdiv);
  var p = vec3f(cell);
  p[axis] += select(0.0, 1.0, (face & 1u) != 0u);
  p[t.x] += (f32(sx) + 0.5) * inv;
  p[t.y] += (f32(sy) + 0.5) * inv;
  return p;
}

// ---- the request record ----
// Three WORLD_SHIFT-wide cell fields plus a 3-bit face in one word. The cell is
// stored TOROIDAL — `cell & WORLD_MASK` per axis, the same wrap the slot index
// uses — so it fits, and so that a patch's identity does not depend on where
// the residency window happens to sit. world.h static_asserts that this
// packing fits a u32.
//
// NOT WINDOW-RELATIVE, and this was the shadow that flashed while walking.
// The first version stored `cell - origin*CHUNK`. The window recentres one
// chunk every time the player crosses a chunk boundary (stream.cpp ShiftAxis),
// and on that frame every key in the cache renamed itself: the fragment shader
// looked up patches by their NEW relative coords, found nothing, claimed
// ~190k fresh slots and shaded the entire frame lit — --gate shadow-cache's
// walk arm measured the shift frame at a mean |dL| of 10.13 against 0.6 for
// its neighbours, with 382k lit-hole pixels, and the probe it dumps saw the
// valid-slot count DROP by 69k on that frame (claims steal stale slots). Every
// 1.6 m of walking, a one-frame flash of the whole scene's shadows. The
// toroidal coord is invariant under the shift, and the resolve pass unwraps it
// into whichever window is current (shadowUnwrapCell) — sound because a
// visible patch is by construction inside the window, so its unwrap is unique.
fn shadowPackCell(torCell : vec3<i32>, face : u32) -> u32 {
  let c = vec3<u32>(torCell & vec3<i32>(i32(WORLD_MASK)));
  return c.x | (c.y << WORLD_SHIFT) | (c.z << (WORLD_SHIFT * 2u)) |
         (face << (WORLD_SHIFT * 3u));
}
// The toroidal cell back to a world cell inside the window whose low corner
// (in voxels) is `winLo`: the unique representative of the residue class in
// [winLo, winLo + WORLD_N). The i32 `&` is the right modulus for negative
// window corners too (two's complement, WORLD_N a power of two).
fn shadowUnwrapCell(packedCell : u32, winLo : vec3<i32>) -> vec3<i32> {
  let tor = vec3<i32>(vec3<u32>(packedCell, packedCell >> WORLD_SHIFT,
                                packedCell >> (WORLD_SHIFT * 2u)) &
                      vec3<u32>(u32(WORLD_MASK)));
  return winLo + ((tor - winLo) & vec3<i32>(i32(WORLD_MASK)));
}
fn shadowPackSub(sx : u32, sy : u32) -> u32 { return sx | (sy << 3u); }

// ---- the key, its verifier, and the SET it lives in ----
// A patch's identity is ~36 bits (cell, face, sub) and does not fit one word,
// so it is split across the two words of a slot: a 32-bit hash in the KEY word
// and a second, independent 15-bit hash (the VERIFIER) in the state word. A
// slot is a patch's only if both agree — 47 bits of identity, which at a ~200k
// working set is ~1e-4 false matches per frame, i.e. none. The first version of
// this cache keyed on the 32-bit hash alone and accepted ~5 colliding pairs
// per frame as "self-correcting"; they were not — a pair that shares a key
// alternates ownership frame by frame and the loser's patch flickers.
//
// Key 0 is reserved for "empty slot" — the buffer is zero-filled, and a patch
// that legitimately hashed to 0 would match every untouched slot.
fn shadowPatchKey(packedCell : u32, packedSub : u32) -> u32 {
  let k = pcg(packedCell ^ pcg(packedSub * 0x9E3779B9u + 1u));
  return select(k, 1u, k == 0u);
}
// Never 0, so that a claimed-but-unresolved slot's state word (value 0,
// resolved 0, requested f, valid 0, verifier v) is nonzero even at f = 0 —
// the claim loop in raymarch.wgsl tells "never claimed" from "mid-claim" by
// the state word alone.
fn shadowPatchVerifier(packedCell : u32, packedSub : u32) -> u32 {
  let v = pcg(packedSub ^ pcg(packedCell * 0x85EBCA6Bu + 7u)) & SHADOW_VERIFIER_MASK;
  return select(v, 1u, v == 0u);
}

// SET-ASSOCIATIVE, NOT DIRECT-MAPPED. This is the fix for the "shadow pixels
// that spasm and fly across the ground": the first version mapped a key to one
// bucket and let collisions overwrite. At ~200k patches in 1M buckets that is
// not rare — about 16% of every frame's patches shared a bucket with another
// VISIBLE patch, so ~28k patches per frame took turns evicting each other and
// each showed the miss default (fully lit) on the frames it lost. Worse, the
// steal happened MID-FRAME, so pixels of one patch shaded lit or shadowed by
// GPU fragment order.
//
// Now a key selects a SET of SHADOW_WAYS consecutive slots — 8 slots x 2 words
// = 64 bytes, one cache line, so probing the whole set costs about what one
// probe did. A patch finds its own slot anywhere in the set, or claims a slot
// nobody LIVE holds (raymarch.wgsl shadowCached: live = requested this frame or
// last). A live slot is never stolen, so two patches that share a set coexist
// and nothing alternates. Overflow (nine live patches in one set) is a miss for
// the ninth, and at a 20% load the Poisson tail past 8 is ~1e-5 of patches.
const SHADOW_WAYS : u32 = 8u;
fn shadowSetOf(key : u32) -> u32 {
  // The set index is a slice of the key, which is fine: the verifier hashes
  // the same inputs through a different mix, and the independence of THOSE two
  // is what the 47-bit identity claim rests on.
  return key & (SHADOW_CACHE_BUCKETS - 1u) & ~(SHADOW_WAYS - 1u);
}

// ---- the slot's state word ----
// value 8 | resolvedFrame 4 | requestedFrame 4 | valid 1 | verifier 15.
//
// TWO SEPARATE FRAME FIELDS, and that is load-bearing: registration must stamp
// `requested` WITHOUT disturbing the value other pixels are still reading this
// frame, so one field cannot serve both roles. `requested` is also what makes
// a slot LIVE (see shadowSetOf) and therefore unstealable.
//
// THE VALID BIT is set by nothing but the resolve pass. A freshly claimed slot
// is zero-valued and invalid, and the reader treats invalid as "no opinion"
// (weight 0 in the bilinear blend), never as "black".
//
// resolvedFrame is NOT compared by the reader any more. The first version only
// trusted a value resolved THIS frame and shaded everything else lit, which
// made every patch newly exposed by camera motion sparkle bright for one frame
// along the whole disocclusion fringe. A slot's value is by construction the
// right PATCH's answer (47-bit identity), and it is recast every frame the
// patch is on screen, so the only way it can be stale is that the patch just
// came back into view — and a shadow from a few frames ago is a far smaller
// error than a bright hole. It stays in the word for --render-budget's
// inspection and for the one-frame-old diagnosis it makes possible.
const SHADOW_VERIFIER_SHIFT : u32 = 17u;
const SHADOW_VERIFIER_MASK : u32 = 0x7FFFu;
fn shadowPackState(value : u32, resolvedFrame : u32, requestedFrame : u32,
                   valid : bool, verifier : u32) -> u32 {
  return (value & 0xFFu) | ((resolvedFrame & 15u) << 8u) |
         ((requestedFrame & 15u) << 12u) | select(0u, 0x10000u, valid) |
         ((verifier & SHADOW_VERIFIER_MASK) << SHADOW_VERIFIER_SHIFT);
}
fn shadowStateValue(s : u32) -> f32 { return f32(s & 0xFFu) * (1.0 / 255.0); }
fn shadowStateResolved(s : u32) -> u32 { return (s >> 8u) & 15u; }
fn shadowStateRequested(s : u32) -> u32 { return (s >> 12u) & 15u; }
fn shadowStateValid(s : u32) -> bool { return (s & 0x10000u) != 0u; }
fn shadowStateVerifier(s : u32) -> u32 {
  return (s >> SHADOW_VERIFIER_SHIFT) & SHADOW_VERIFIER_MASK;
}
// Was this slot asked for this frame or last? Modulo-16 arithmetic on the
// 4-bit stamp: a slot last requested exactly 15 or 16 frames ago reads as live
// for one frame and is simply skipped, which costs a probe, not a wrong value.
fn shadowSlotLive(s : u32, curFrame : u32) -> bool {
  return ((curFrame - shadowStateRequested(s)) & 15u) <= 1u;
}

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

// The ONLY way to obtain a WRITABLE word index. Returns PT_NO_WORD for a
// sentinel chunk — which is a BUG at every sim call site, because §3
// guarantees every chunk a kernel may write is materialized before dispatch.
//
// This is the invariant the whole phase rests on, and it is a SHAPE rather
// than a rule to remember: there is no writable accessor that takes a
// sentinel. The write accessor takes a physical word index, and the only way
// to get one is a function that returns a distinguished no-word value.
//
// IT LIVES IN THE WRITE HALF, with `pageFaults` in scope, so that the resolver
// can record WHICH CHUNK refused the write. Nothing read-only ever needed it —
// raymarch.wgsl reads through voxWordAt/voxWordAtEntry — and a fault COUNT with
// no location is most of a day of turning worldgen features off one at a time
// to find out where 58 lost voxels came from. Ask it once, in the one function
// that knows.
// The slot the last resolve looked at, carried from the resolver to voxStore.
// `private` is per-invocation and the two calls are adjacent in the same
// invocation (`voxStore(voxWordIndex(c), w)`), so this is exact.
//
// IT IS RECORDED AT THE STORE, NOT AT THE RESOLVE, and that distinction is the
// whole point. A resolver refusal is not a fault: sim_fluid_seam asks
// voxWordIndex whether a cell is writable and treats PT_NO_WORD as "blocked",
// which is correct behaviour and happens constantly. Reporting the refusal
// location instead of the STORE location named a chunk layer that had nothing
// to do with the 58 lost voxels, which is worse than reporting nothing.
var<private> gPtSlot : u32 = 0xFFFFFFFFu;

fn voxWordIndex(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let slot = chunkIndexOf(s);
  let e = pageTable[slot];
  if ((e & PT_SENTINEL_BIT) != 0u) { gPtSlot = slot; return PT_NO_WORD; }
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
  if ((e & PT_SENTINEL_BIT) != 0u) { gPtSlot = chunkSlot; return PT_NO_WORD; }
  return e * CHUNK_VOL + localIdx;
}


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
    // WHICH MATERIAL WAS LOST, as a 96-bit bitmask across the three spare words
    // `pageFaults` already allocated. The counter alone says "58 voxels went
    // missing somewhere" and sends you turning worldgen features off one at a
    // time; the material names the rule in one run. Ids at or above 96 fold
    // into the top bank, which is flagged rather than hidden.
    atomicMax(&pageFaults[2], w);
    atomicMax(&pageFaults[1], gPtSlot + 1u);
    atomicMax(&pageFaults[3], 0xFFFFFFFFu - gPtSlot);   // == min, reported as one
    return;
  }
  voxels[idx] = w;
}
// >>>PAGE_TABLE_WRITE_END<<<
