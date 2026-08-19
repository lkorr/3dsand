// common.wgsl — prepended to every shader at load time (see resources.cpp).
// All simulation math is integer-only and stateless: determinism is a day-one
// invariant (DESIGN.md §2/§4). Do not introduce floats, atomics-ordering
// dependence, or stateful RNG into anything that feeds voxel state.

const WORLD_N   : u32 = 256u;   // voxels per axis (must match world.h)
// Physical voxel edge length (must match kVoxelMeters in world.h). Render-only:
// the sim never reads it — voxel state stays integer and scale-free.
const VOXEL_METERS : f32 = 0.125;
const CHUNK     : u32 = 16u;    // voxels per chunk axis
const NCHUNK    : u32 = 16u;    // chunks per axis = WORLD_N / CHUNK
const NUM_CHUNKS : u32 = 4096u; // NCHUNK^3
const CHUNK_VOL : u32 = 4096u;  // CHUNK^3

const MAT_AIR : u32 = 0u;

const CLASS_SOLID  : u32 = 0u;
const CLASS_POWDER : u32 = 1u;
const CLASS_LIQUID : u32 = 2u;
const CLASS_GAS    : u32 = 3u;

// Air participates in density ordering: powders/liquids sink through it,
// gases rise through it.
const AIR_DENSITY : i32 = 10;

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
  _p2 : u32, _p3 : u32, _p4 : u32,
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
  _pad     : u32,
};

struct TickParams {
  tick       : u32,
  seed       : u32,
  opsCount   : u32,
  hashEnable : u32,
  expCount   : u32,  // explosion ops this tick
  page       : u32,  // particle read page (0/1) this tick
  cellCount  : u32,  // exact-cell ops this tick
  _p1        : u32,
};

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
  sunDir     : vec3f,  _p1        : f32,
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
const PART_GRAVITY  : i32 = 22;        // 9.81 m/s^2 in voxels/tick^2, fixed point
const PART_MAX_VEL  : i32 = 1536;      // 6 voxels/tick terminal speed
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

// Chunk-major addressing: each 16^3 chunk is one contiguous 16 KB block, so
// chunk streaming/readback is a single buffer copy per chunk.
fn chunkIndexOf(c : vec3<u32>) -> u32 {
  let ch = c / CHUNK;
  return (ch.z * NCHUNK + ch.y) * NCHUNK + ch.x;
}
fn cellIndex(c : vec3<u32>) -> u32 {
  let lo = c % CHUNK;
  let localIdx = (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  return chunkIndexOf(c) * CHUNK_VOL + localIdx;
}
fn inBounds(c : vec3<i32>) -> bool {
  let n = i32(WORLD_N);
  return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < n && c.y < n && c.z < n;
}

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
