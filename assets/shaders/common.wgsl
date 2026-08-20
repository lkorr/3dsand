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
  _p3 : u32, _p4 : u32,
};

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
  chance   : u32,  // per-mille per 30 Hz tick
  prodSelf : u32,  // what self becomes (PROD_KEEP = unchanged, 0 = air)
  prodNbr  : u32,  // pair: neighbor product; emit: emitted material
  // Light/day-phase gate: bits 0..7 RCOND_*, bits 8..15 min daylight 0..255.
  // 0 = unconditional (every rule that predates the day/night cycle).
  cond     : u32,
};
// Reaction light conditions — must match kCond* in src/sim/materials.h.
const RCOND_SKY   : u32 = 1u;  // cell must have open sky above it
const RCOND_DAY   : u32 = 2u;  // only while the sun is up
const RCOND_NIGHT : u32 = 4u;  // only while the sun is down

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
  _pdn       : f32,
};

// Reversed-Z depth (clear 0, compare GreaterEqual): depth = KNEAR / viewZ.
// Shared by the raymarcher (frag_depth) and every raster pipeline so raster
// geometry (particles, debris bodies, sprites) composites exactly against the
// raymarched terrain. KNEAR is in voxels.
const KNEAR : f32 = 0.4;

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
fn isRayBlocker(m : Material) -> bool {
  return m.klass == CLASS_SOLID || m.klass == CLASS_POWDER ||
         (m.klass == CLASS_LIQUID && (m.flags & MATF_OPAQUE) != 0u);
}
fn occTotal(occ : u32) -> u32 { return occ & 0xFFFFu; }
fn occBlockers(occ : u32) -> u32 { return occ >> 16u; }
fn packOcc(total : u32, blockers : u32) -> u32 { return total | (blockers << 16u); }

// Voxel word: bits 0..11 material, 12..15 state, 16..23 tick-stamp, 24..31 spare.
fn voxMat(w : u32) -> u32 { return w & 0xFFFu; }
fn voxState(w : u32) -> u32 { return (w >> 12u) & 0xFu; }
fn voxStamp(w : u32) -> u32 { return (w >> 16u) & 0xFFu; }
fn packVox(mat : u32, state : u32, stamp : u32) -> u32 {
  return (mat & 0xFFFu) | ((state & 0xFu) << 12u) | ((stamp & 0xFFu) << 16u);
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
