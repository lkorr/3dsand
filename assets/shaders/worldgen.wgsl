// worldgen.wgsl — startup test world, integer-only and seed-deterministic.
// The height() function is mirrored exactly in C++ (world.cpp) so the CPU can
// compute the spawn point — keep the two in sync.
//
// Layout: rolling stone hills with a sand cap, a rimmed water pool, a rimmed
// oil pond, and a wood platform on pillars. Material IDs are fixed by
// materials.json order and mirrored in world.h.

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn  : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(4) var<uniform> T : TickParams;

const M_STONE : u32 = 1u;
const M_WOOD  : u32 = 2u;
const M_SAND  : u32 = 3u;
const M_GRAVEL: u32 = 4u;
const M_WATER : u32 = 5u;
const M_OIL   : u32 = 6u;
const M_LAVA  : u32 = 12u;
const M_SNOW  : u32 = 15u;
const M_SEED  : u32 = 18u;

fn vnoise(x : i32, z : i32, cs : i32, seed : u32) -> i32 {
  let gx = x / cs;
  let gz = z / cs;
  let fx = x % cs;
  let fz = z % cs;
  let h00 = i32(hash3(seed, u32(gx),     u32(gz))     & 0xFFu);
  let h10 = i32(hash3(seed, u32(gx + 1), u32(gz))     & 0xFFu);
  let h01 = i32(hash3(seed, u32(gx),     u32(gz + 1)) & 0xFFu);
  let h11 = i32(hash3(seed, u32(gx + 1), u32(gz + 1)) & 0xFFu);
  let v0 = h00 * (cs - fx) + h10 * fx;
  let v1 = h01 * (cs - fx) + h11 * fx;
  return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
}

fn baseHeight(x : i32, z : i32, seed : u32) -> i32 {
  return 44 + (vnoise(x, z, 64, seed ^ 1u) * 36) / 255
            + (vnoise(x, z, 16, seed ^ 2u) * 10) / 255;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= WORLD_N || gid.y >= WORLD_N || gid.z >= WORLD_N) { return; }
  let x = i32(gid.x);
  let y = i32(gid.y);
  let z = i32(gid.z);

  var h = baseHeight(x, z, T.seed);
  var mat = MAT_AIR;
  var fluidTop = -1;     // top of any standing fluid at this column
  var fluid = MAT_AIR;

  // Water pool at (176,176)
  let pdx = x - 176; let pdz = z - 176;
  let pd2 = pdx * pdx + pdz * pdz;
  if (pd2 < 34 * 34) {
    h = 40;
    fluid = M_WATER; fluidTop = 52;
  } else if (pd2 < 40 * 40) {
    h = max(h, 54);    // containment rim
  }

  // Oil pond at (64,72)
  let odx = x - 64; let odz = z - 72;
  let od2 = odx * odx + odz * odz;
  if (od2 < 16 * 16) {
    h = 46;
    fluid = M_OIL; fluidTop = 52;
  } else if (od2 < 21 * 21) {
    h = max(h, 54);
  }

  // Lava pool at (48,192) — stone-rimmed, feeds the melt/burn reaction chains
  let ldx = x - 48; let ldz = z - 192;
  let ld2 = ldx * ldx + ldz * ldz;
  if (ld2 < 12 * 12) {
    h = 42;
    fluid = M_LAVA; fluidTop = 50;
  } else if (ld2 < 17 * 17) {
    h = max(h, 52);
  }

  let inPoolFloor = pd2 < 34 * 34 || od2 < 16 * 16 || ld2 < 12 * 12;
  let inRim = pd2 < 40 * 40 || od2 < 21 * 21 || ld2 < 17 * 17;

  if (y <= h) {
    if (y < 3) {
      mat = M_STONE;                       // bedrock floor
    } else if (!inPoolFloor && h >= 80 && y > h - 2) {
      mat = M_SNOW;                        // snow caps on the high hills
    } else if (!inPoolFloor && y > h - 4) {
      mat = M_SAND;                        // loose cap — avalanches into repose piles
    } else {
      mat = M_STONE;
    }
  } else if (fluidTop >= 0 && y <= fluidTop) {
    mat = fluid;
  }

  // Sparse seeds on open sandy ground: they fall one cell, germinate, and the
  // world greens itself over the first minutes. Kept away from pools/rims,
  // snowfields, and the spawn/selftest walk site at (140,140).
  let sdx = x - 140; let sdz = z - 140;
  if (y == h + 1 && !inRim && h < 80 && sdx * sdx + sdz * sdz > 12 * 12 &&
      hash3(T.seed ^ 0xBEEFu, u32(x), u32(z)) % 1000u < 4u) {
    mat = M_SEED;
  }

  // Wood platform on pillars near spawn
  let onPillar = (x == 102 || x == 118) && (z == 102 || z == 118) && y <= 76;
  let inSlab = x >= 100 && x <= 120 && z >= 100 && z <= 120 && y >= 76 && y <= 77;
  if ((onPillar && y > h) || inSlab) {
    mat = M_WOOD;
  }

  let idx = cellIndex(gid);
  if (mat == MAT_AIR) {
    voxels[idx] = 0u;
    return;
  }
  let rnd = hash3(T.seed ^ 0xC0FFEEu, gid.x ^ (gid.z << 12u), gid.y);
  // liquids are born full (state nibble = fullness); solids get palette jitter
  var state = rnd % 3u;
  if (mat == M_WATER || mat == M_OIL || mat == M_LAVA) { state = LIQ_FULL_STATE; }
  voxels[idx] = packVox(mat, state, 0xFFu);

  let ci = chunkIndexOf(gid);
  atomicStore(&dirtyIn[ci], 1u);
  atomicStore(&dirtyOut[ci], 1u);
}
