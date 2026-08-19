// sim_step.wgsl — one 3x3x3-color pass of the cellular automaton.
// Threads map to cells of a single color (cell = gid*3 + colorPhase), so any
// two acting cells are >=3 apart on every axis while writes reach <=1 cell:
// destination writes are provably disjoint. No atomics on voxel data, fixed
// pass order => bit-deterministic (DESIGN.md §4).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn  : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(5) var<uniform> P : PassParams;

// Mark the chunk containing c dirty for next tick, plus every neighbor chunk
// c borders (so cross-chunk neighbors re-evaluate; sleeping is per-chunk).
fn markDirty(c : vec3<i32>) {
  let cu = vec3<u32>(c);
  let lo = cu % CHUNK;
  let ch = vec3<i32>(cu / CHUNK);
  var xs = array<i32, 2>(0, 0);
  var ys = array<i32, 2>(0, 0);
  var zs = array<i32, 2>(0, 0);
  if (lo.x == 0u) { xs[1] = -1; } else if (lo.x == CHUNK - 1u) { xs[1] = 1; }
  if (lo.y == 0u) { ys[1] = -1; } else if (lo.y == CHUNK - 1u) { ys[1] = 1; }
  if (lo.z == 0u) { zs[1] = -1; } else if (lo.z == CHUNK - 1u) { zs[1] = 1; }
  let nc = i32(NCHUNK);
  for (var i = 0; i < 2; i++) {
    for (var j = 0; j < 2; j++) {
      for (var k = 0; k < 2; k++) {
        let n = ch + vec3<i32>(xs[i], ys[j], zs[k]);
        if (n.x >= 0 && n.y >= 0 && n.z >= 0 && n.x < nc && n.y < nc && n.z < nc) {
          let ci = u32((n.z * nc + n.y) * nc + n.x);
          atomicStore(&dirtyOut[ci], 1u);
        }
      }
    }
  }
}

// Can a mover of (klass, density) enter the cell holding word tw?
// rising=true for gases (they seek lower density above), false for falling.
fn canDisplace(myDensity : i32, rising : bool, tw : u32) -> bool {
  let tmat = voxMat(tw);
  var td : i32;
  if (tmat == MAT_AIR) {
    td = AIR_DENSITY;
  } else {
    let t = materials[tmat];
    if (t.klass == CLASS_SOLID || t.klass == CLASS_POWDER) { return false; }
    td = t.density;
  }
  if (rising) { return td > myDensity; }
  return td < myDensity;
}

fn tryMove(src : vec3<i32>, dst : vec3<i32>, myWord : u32, myDensity : i32, rising : bool) -> bool {
  if (!inBounds(dst)) { return false; }
  let di = cellIndex(vec3<u32>(dst));
  let tw = voxels[di];
  if (!canDisplace(myDensity, rising, tw)) { return false; }
  let stamp = stampFor(T.tick, P.substep);
  voxels[di] = packVox(voxMat(myWord), voxState(myWord), stamp);
  // displaced fluid (or air) swaps into the source cell, stamped so it does
  // not act again this tick
  voxels[cellIndex(vec3<u32>(src))] = packVox(voxMat(tw), voxState(tw), stamp);
  markDirty(src);
  markDirty(dst);
  return true;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let cell = gid * 3u + P.colorPhase;
  if (cell.x >= WORLD_N || cell.y >= WORLD_N || cell.z >= WORLD_N) { return; }
  if (dirtyIn[chunkIndexOf(cell)] == 0u) { return; }   // sleeping chunk

  let idx = cellIndex(cell);
  let w = voxels[idx];
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return; }
  if (voxStamp(w) == stampFor(T.tick, P.substep)) { return; }  // already acted this substep

  let m = materials[mat];
  if (m.klass == CLASS_SOLID) { return; }

  let rnd = hash3(T.seed, T.tick * 2u + P.substep, idx);
  let c = vec3<i32>(cell);

  if (m.klass == CLASS_GAS && m.decayPerMille > 0u && (rnd % 1000u) < m.decayPerMille) {
    voxels[idx] = 0u;
    markDirty(c);
    return;
  }

  let rising = m.klass == CLASS_GAS;
  let dy = select(-1, 1, rising);

  // 1) straight fall / rise
  if (tryMove(c, c + vec3<i32>(0, dy, 0), w, m.density, rising)) { return; }

  // 2) the four diagonal cells one step down (up for gas), RNG order
  let r = rnd >> 10u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryMove(c, c + vec3<i32>(d.x, dy, d.y), w, m.density, rising)) { return; }
  }

  // 3) liquids and gases also spread laterally, RNG order
  if (m.klass != CLASS_POWDER) {
    let r2 = rnd >> 14u;
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + r2);
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, rising)) { return; }
    }
  }
  // Nothing to do: cell settles. If the whole chunk settles, nothing marks it
  // dirty and it sleeps until a neighbor wakes it.
}
