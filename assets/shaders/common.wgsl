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

struct Material {
  klass       : u32,
  density     : i32,
  color0      : u32,   // RGBA8 palette variants
  color1      : u32,
  color2      : u32,
  emission    : u32,   // 0..255 glow strength (render + media)
  flags       : u32,   // MATF_*
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
// Does this material stain what it touches at all? One comparison, so the sim
// can reject the overwhelmingly common "no" before doing any other work.
fn matStains(m : Material) -> bool { return (m.stainPack & 0x7u) != 0u; }

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
  _p3 : u32, _p4 : u32,
};

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
fn stampFor(tick : u32, substep : u32) -> u32 {
  return (tick * 2u + substep) & 0xFFu;
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
  _pdn0      : u32,
  _pdn1      : u32,
  _pdn2      : u32,
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

// Direct light: sun by day, moon by night. Returns colour x intensity;
// callers multiply by their own N.L / shadow terms.
fn keyLightColorP(R : RenderParams) -> vec3f {
  let sunCol = sunTransmittance(airMass(R.sunDir.y)) * TUNE_SUN_COLOR *
               TUNE_SUN_INTENSITY;
  let moonUp = smoothstep(-0.10, 0.18, R.moonDir.y);
  let moonCol = TUNE_MOON_LIGHT_COLOR * TUNE_MOON_LIGHT_INTENSITY * moonUp *
                (0.15 + 1.70 * R.moonPhase * R.moonPhase);
  return mix(moonCol, sunCol, R.sunUp);
}

// Direction of the key light. A hard switch at sunUp = 0.5 rather than a
// blend: a lerp between two directions would swing shadows wildly through
// twilight, and at the crossover both lights are dim enough to hide the swap.
fn keyLightDirP(R : RenderParams) -> vec3f {
  return normalize(mix(R.moonDir, R.sunDir, step(0.5, R.sunUp)));
}

// Two-tone hemisphere ambient (cool sky above, warm bounce below), scaled to
// a dim blue moon/starlight version at night.
fn ambientAtP(n : vec3f, R : RenderParams) -> vec3f {
  let base = mix(TUNE_AMB_GROUND, TUNE_AMB_SKY, n.y * 0.5 + 0.5);
  let nightAmb = mix(TUNE_NIGHT_AMB_GROUND, TUNE_NIGHT_AMB_SKY, n.y * 0.5 + 0.5);
  let moonUp = smoothstep(-0.10, 0.18, R.moonDir.y);
  let moonAmt = moonUp * (0.30 + 1.40 * R.moonPhase * R.moonPhase);
  return mix(nightAmb * (0.45 + moonAmt), base, R.sunUp);
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
  // fixed day-blue tint here was a second source of midnight glow)
  let moonUp = smoothstep(-0.10, 0.18, R.moonDir.y);
  let fogTint = vec3f(0.55, 0.65, 0.85) *
                mix(0.015 + 0.05 * moonUp * R.moonPhase, 1.0, R.sunUp);
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

struct Particle {
  px : i32, py : i32, pz : i32,   // position, fixed 24.8 voxels
  vx : i32, vy : i32, vz : i32,   // velocity, fixed 24.8 voxels/tick
  payload : u32,                  // bits 0..11 material, 12..15 state
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
fn occTotal(occ : u32) -> u32 { return occ & 0xFFFFu; }
fn occBlockers(occ : u32) -> u32 { return occ >> 16u; }
fn packOcc(total : u32, blockers : u32) -> u32 { return total | (blockers << 16u); }

// Voxel word: bits 0..11 material, 12..15 state, 16..23 tick-stamp,
//             24..27 stain amount, 28..30 stain type, 31 reserved.
fn voxMat(w : u32) -> u32 { return w & 0xFFFu; }
fn voxState(w : u32) -> u32 { return (w >> 12u) & 0xFu; }
fn voxStamp(w : u32) -> u32 { return (w >> 16u) & 0xFFu; }
fn packVox(mat : u32, state : u32, stamp : u32) -> u32 {
  return (mat & 0xFFFu) | ((state & 0xFu) << 12u) | ((stamp & 0xFFu) << 16u);
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
