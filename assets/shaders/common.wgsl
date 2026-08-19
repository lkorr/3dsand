// common.wgsl — prepended to every shader at load time (see resources.cpp).
// All simulation math is integer-only and stateless: determinism is a day-one
// invariant (DESIGN.md §2/§4). Do not introduce floats, atomics-ordering
// dependence, or stateful RNG into anything that feeds voxel state.

const WORLD_N   : u32 = 256u;   // voxels per axis (must match world.h)
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

struct Material {
  klass         : u32,
  density       : i32,
  color0        : u32,   // RGBA8 palette variants
  color1        : u32,
  color2        : u32,
  decayPerMille : u32,   // gases: chance/tick to vanish
  flags         : u32,
  _pad          : u32,
};

struct TickParams {
  tick       : u32,
  seed       : u32,
  opsCount   : u32,
  hashEnable : u32,
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
