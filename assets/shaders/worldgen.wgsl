// worldgen.wgsl — compute procgen (M7), integer-only and seed-deterministic.
// genCell() is a pure function of WORLD coordinates + seed, so a chunk that is
// generated, evicted unmodified, and re-entered regenerates bit-identically.
// The height function is mirrored exactly in C++ (world.cpp TerrainHeight) so
// the CPU can compute spawn points — keep the two in sync, including the
// floor-division fixes for negative coordinates.
//
// Two entries, both one workgroup (64 threads) per chunk:
//   main — full residency window (NUM_CHUNKS workgroups), startup / regen
//   list — T.genCount slot indices from genList, streamed-in chunks
// Each workgroup fills its chunk, computes its occupancy count in-kernel (the
// renderer skips occ==0 chunks, so a late count would flicker the horizon),
// and wakes the chunk once so loose material settles and then sleeps.
//
// Terrain: rolling hills with loose sand caps and snow above the treeline,
// carved by 3D-noise caves below an 8-voxel surface shell, lava pockets at
// depth, sparse seeds greening the surface, and per-256^2-tile ruin POIs.
// The authored origin-area set pieces (water/oil/lava pools, wood platform)
// live at their absolute coordinates and appear when those chunks generate.

@group(0) @binding(0) var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn   : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut  : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read_write> occupancy : array<u32>;
@group(0) @binding(16) var<storage, read> genList : array<u32>;

const M_STONE : u32 = 1u;
const M_WOOD  : u32 = 2u;
const M_SAND  : u32 = 3u;
const M_WATER : u32 = 5u;
const M_OIL   : u32 = 6u;
const M_LAVA  : u32 = 12u;
const M_SNOW  : u32 = 15u;
const M_SEED  : u32 = 18u;

// Floor division / positive modulo: WGSL `/` and `%` truncate toward zero,
// which breaks value-noise lattices at negative coordinates (gx would repeat
// around 0 and fx would go negative).
fn fdiv(a : i32, b : i32) -> i32 {
  var q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) { q -= 1; }
  return q;
}
fn fmodp(a : i32, b : i32) -> i32 {
  let m = a % b;
  return m + select(0, b, m < 0);
}

fn vnoise(x : i32, z : i32, cs : i32, seed : u32) -> i32 {
  let gx = fdiv(x, cs);
  let gz = fdiv(z, cs);
  let fx = fmodp(x, cs);
  let fz = fmodp(z, cs);
  let h00 = i32(hash3(seed, bitcast<u32>(gx),      bitcast<u32>(gz))      & 0xFFu);
  let h10 = i32(hash3(seed, bitcast<u32>(gx + 1),  bitcast<u32>(gz))      & 0xFFu);
  let h01 = i32(hash3(seed, bitcast<u32>(gx),      bitcast<u32>(gz + 1))  & 0xFFu);
  let h11 = i32(hash3(seed, bitcast<u32>(gx + 1),  bitcast<u32>(gz + 1))  & 0xFFu);
  let v0 = h00 * (cs - fx) + h10 * fx;
  let v1 = h01 * (cs - fx) + h11 * fx;
  return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
}

fn baseHeight(x : i32, z : i32, seed : u32) -> i32 {
  return 44 + (vnoise(x, z, 64, seed ^ 1u) * 36) / 255
            + (vnoise(x, z, 16, seed ^ 2u) * 10) / 255;
}

// Caves: COLUMN BANDS carved by 2D noise — for each (x,z) inside a cavern
// mask, one contiguous vertical span is removed. Unlike 3D-threshold carving
// this cannot create free-floating stone blobs (stone above/below a band is
// horizontally connected to full columns at the mask boundary), which matters
// because the island detector would correctly-but-noisily convert generated
// floaters into debris the moment anything moved nearby.
// Returns 0 = solid, 1 = carve to air, 2 = carve to lava (deep cavern floors).
fn caveAt(x : i32, y : i32, z : i32, h : i32, seed : u32) -> i32 {
  // band 1: near-surface caverns following the terrain, floors 14-44 below
  let m1 = vnoise(x, z, 40, seed ^ 5u);
  if (m1 > 150) {
    let f1 = h - 14 - (vnoise(x, z, 32, seed ^ 6u) * 30) / 255;
    let c1 = min(f1 + 3 + (vnoise(x, z, 12, seed ^ 7u) * 8) / 255, h - 10);
    if (y >= f1 && y <= c1) { return 1; }
  }
  // band 2: deep caverns at absolute depth (streamed depth is real terrain)
  let m2 = vnoise(x + 7717, z - 4177, 48, seed ^ 8u);
  if (m2 > 148) {
    let f2 = -24 - (vnoise(x, z, 40, seed ^ 9u) * 40) / 255;
    let c2 = f2 + 4 + (vnoise(x, z, 16, seed ^ 10u) * 12) / 255;
    if (y >= f2 && y <= c2 && y <= h - 10) {
      if (y <= f2 + 1 && m2 > 190) { return 2; }  // lava pools where mask peaks
      return 1;
    }
  }
  return 0;
}

fn genCell(c : vec3<i32>, seed : u32) -> u32 {
  let x = c.x; let y = c.y; let z = c.z;
  var h = baseHeight(x, z, seed);
  var mat = MAT_AIR;
  var fluidTop = -1;     // top of any standing fluid at this column
  var fluid = MAT_AIR;

  // ---- authored origin-area set pieces (absolute world coords) ----
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
  // Lava pool at (48,192)
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
    if (!inPoolFloor && h >= 80 && y > h - 2) {
      mat = M_SNOW;                        // snow caps on the high hills
    } else if (!inPoolFloor && y > h - 4) {
      mat = M_SAND;                        // loose cap — avalanches into repose piles
    } else {
      mat = M_STONE;
    }
    // depth is real now (no bedrock): caves carve the stone body, with lava
    // pooling on deep cavern floors. No caves under the authored pools/rims —
    // a cave breaching a rim column drains the pool through the tunnel system
    // and the world never settles.
    if (mat == M_STONE && !inRim) {
      let cv = caveAt(x, y, z, h, seed);
      if (cv == 1) { mat = MAT_AIR; }
      else if (cv == 2) { mat = M_LAVA; }
    }
  } else if (fluidTop >= 0 && y <= fluidTop) {
    mat = fluid;
  }

  // Sparse seeds on open sandy ground (kept away from pools/rims, snowfields,
  // and the spawn/selftest walk site at (140,140)).
  let sdx = x - 140; let sdz = z - 140;
  if (y == h + 1 && !inRim && h < 80 && sdx * sdx + sdz * sdz > 12 * 12 &&
      hash3(seed ^ 0xBEEFu, bitcast<u32>(x), bitcast<u32>(z)) % 1000u < 4u) {
    mat = M_SEED;
  }

  // Wood platform on pillars near spawn (authored POI)
  let onPillar = (x == 102 || x == 118) && (z == 102 || z == 118) && y <= 76;
  let inSlab = x >= 100 && x <= 120 && z >= 100 && z <= 120 && y >= 76 && y <= 77;
  if ((onPillar && y > h) || inSlab) {
    mat = M_WOOD;
  }

  // Procedural ruin POIs: one hollow stone box per ~5th 256x256 tile, placed
  // by tile hash. Tile (0,0) keeps the authored set pieces instead.
  let tx = fdiv(x, 256); let tz = fdiv(z, 256);
  if (tx != 0 || tz != 0) {
    let rh = hash3(seed ^ 0xA111CEu, bitcast<u32>(tx), bitcast<u32>(tz));
    if (rh % 5u == 0u) {
      let rx = tx * 256 + 28 + i32((rh >> 8u) % 200u);
      let rz = tz * 256 + 28 + i32((rh >> 16u) % 200u);
      // box test in XZ first: baseHeight for the corner only when close
      if (x >= rx && x < rx + 14 && z >= rz && z < rz + 14) {
        let ry = baseHeight(rx + 7, rz + 7, seed);
        if (y >= ry && y < ry + 10) {
          let shellXZ = x == rx || x == rx + 13 || z == rz || z == rz + 13;
          let shellY = y == ry + 9;
          let door = y < ry + 4 && z >= rz + 6 && z <= rz + 8 && x == rx;
          if ((shellXZ || shellY) && !door) { mat = M_STONE; }
          else if (!shellXZ) { mat = select(mat, MAT_AIR, y > ry); }  // hollow
        }
      }
    }
  }

  if (mat == MAT_AIR) { return 0u; }
  let rnd = hash3(seed ^ 0xC0FFEEu,
                  bitcast<u32>(x) ^ (bitcast<u32>(z) << 12u), bitcast<u32>(y));
  // liquids are born full (state nibble = fullness); solids get palette jitter
  var state = rnd % 3u;
  if (mat == M_WATER || mat == M_OIL || mat == M_LAVA) { state = LIQ_FULL_STATE; }
  return packVox(mat, state, 0xFFu);
}

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgBlock : atomic<u32>;

fn genChunk(slot : u32, li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
  }
  workgroupBarrier();

  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  var count = 0u;
  var block = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let l = vec3<i32>(vec3<u32>(i % CHUNK, (i / CHUNK) % CHUNK, i / (CHUNK * CHUNK)));
    let w = genCell(base + l, T.seed);
    voxels[slot * CHUNK_VOL + i] = w;
    let m = w & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      if (isRayBlocker(materials[m])) { block += 1u; }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  workgroupBarrier();

  if (li == 0u) {
    let n = atomicLoad(&wgCount);
    // in-kernel so the renderer never sees a stale 0
    occupancy[slot] = packOcc(n, atomicLoad(&wgBlock));
    if (n > 0u) {          // wake once; loose material settles, then sleeps
      atomicStore(&dirtyIn[slot], 1u);
      atomicStore(&dirtyOut[slot], 1u);
    } else {
      atomicStore(&dirtyIn[slot], 0u);
      atomicStore(&dirtyOut[slot], 0u);
    }
  }
}

// Full residency window: NUM_CHUNKS workgroups.
@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  genChunk(wg.x, li);
}

// Streamed-in chunks: T.genCount slot indices from genList.
@compute @workgroup_size(64)
fn list(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.genCount) { return; }
  genChunk(genList[wg.x], li);
}
