// sim_particle.wgsl — ballistic voxels-in-flight (DESIGN.md §5).
// Runs after the CA color passes each tick, double-buffered:
//   args1    -> integrate (read page, indirect) -> args2 -> resolve (write page)
// integrate: gravity + fixed-point flight, sampling the grid every <=half
// voxel; a particle that would enter a blocking cell proposes a reinsertion at
// the last empty cell by atomicMax-ing a state-derived priority into a claim
// hash. resolve: claim winners write themselves back into the grid; losers
// (two particles wanting one cell, or a hash collision) rest one tick and
// retry. Everything is integer and keyed on particle state + tick, never on
// buffer slot order, so the grid stays bit-deterministic (DESIGN.md §2/§4).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;

@group(1) @binding(0) var<storage, read_write> pRead  : array<Particle>;
@group(1) @binding(1) var<storage, read_write> pWrite : array<Particle>;
@group(1) @binding(2) var<storage, read_write> counts : array<atomic<u32>>;
@group(1) @binding(3) var<storage, read_write> claim  : array<atomic<u32>>;
@group(1) @binding(4) var<storage, read_write> pArgs  : array<u32>;

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

// Blocks flight: solids, powders and liquids (splash = plop onto the surface).
fn blocksParticle(c : vec3<i32>) -> bool {
  let w = voxels[cellIndexW(c)];
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return false; }
  return materials[mat].klass != CLASS_GAS;
}

// Next-tick dirty mark incl. boundary neighbors (particles run post-CA, so
// their writes are next tick's business).
fn markDirtyNext(c : vec3<i32>) {
  let lo = c & vec3<i32>(CHUNK_MASK);
  let ch = worldChunkOf(c);
  var xs = array<i32, 2>(0, 0);
  var ys = array<i32, 2>(0, 0);
  var zs = array<i32, 2>(0, 0);
  if (lo.x == 0) { xs[1] = -1; } else if (lo.x == CHUNK_MASK) { xs[1] = 1; }
  if (lo.y == 0) { ys[1] = -1; } else if (lo.y == CHUNK_MASK) { ys[1] = 1; }
  if (lo.z == 0) { zs[1] = -1; } else if (lo.z == CHUNK_MASK) { zs[1] = 1; }
  for (var i = 0; i < 2; i++) {
    for (var j = 0; j < 2; j++) {
      for (var k = 0; k < 2; k++) {
        let n = ch + vec3<i32>(xs[i], ys[j], zs[k]);
        if (chunkInWindow(n, T.origin)) {
          atomicStore(&dirtyOut[chunkSlotIndex(n)], 1u);
        }
      }
    }
  }
}

fn liveCount(page : u32) -> u32 {
  return min(atomicLoad(&counts[page]), PARTICLE_CAP);
}

// pArgs layout: [0..3] indirect draw {36, instances, 0, 0},
//               [4..6] indirect dispatch {groups, 1, 1}
@compute @workgroup_size(1)
fn args1() {
  let n = liveCount(T.page);
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

@compute @workgroup_size(1)
fn args2() {
  let n = liveCount(1u - T.page);
  pArgs[0] = 36u;
  pArgs[1] = n;
  pArgs[2] = 0u;
  pArgs[3] = 0u;
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

fn append(p : Particle) {
  let slot = atomicAdd(&counts[1u - T.page], 1u);
  if (slot < PARTICLE_CAP) { pWrite[slot] = p; }
}

@compute @workgroup_size(64)
fn integrate(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(T.page)) { return; }
  var p = pRead[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }

  let startCell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  if (!inBounds(startCell)) { return; }  // fell out of the world: gone

  // buried (CA moved material onto us): rise one voxel per tick until free
  if (blocksParticle(startCell)) {
    p.py += PART_ONE;
    p.vx = 0; p.vy = 0; p.vz = 0;
    append(p);
    return;
  }

  // gravity + clamp
  p.vy -= PART_GRAVITY;
  p.vx = clamp(p.vx, -PART_MAX_VEL, PART_MAX_VEL);
  p.vy = clamp(p.vy, -PART_MAX_VEL, PART_MAX_VEL);
  p.vz = clamp(p.vz, -PART_MAX_VEL, PART_MAX_VEL);

  // sample the flight path every <= half voxel
  let maxc = max(max(abs(p.vx), abs(p.vy)), abs(p.vz));
  let n = max(1, (maxc + 127) / 128);
  var lastAir = vec3<i32>(p.px, p.py, p.pz);
  for (var k = 1; k <= n; k++) {
    let sx = p.px + p.vx * k / n;
    let sy = p.py + p.vy * k / n;
    let sz = p.pz + p.vz * k / n;
    let cell = vec3<i32>(sx >> 8u, sy >> 8u, sz >> 8u);
    if (!inBounds(cell)) { return; }  // left the world: particle dies
    if (blocksParticle(cell)) {
      // propose reinsertion at the last empty position
      p.px = lastAir.x; p.py = lastAir.y; p.pz = lastAir.z;
      p.flags |= PFLAG_PENDING;
      let tgt = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
      atomicMax(&claim[claimSlot(cellIndexW(tgt))], particlePriority(p));
      append(p);
      return;
    }
    lastAir = vec3<i32>(sx, sy, sz);
  }
  p.px += p.vx;
  p.py += p.vy;
  p.pz += p.vz;
  append(p);
}

@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(1u - T.page)) { return; }
  var p = pWrite[gid.x];
  if ((p.flags & (PFLAG_ALIVE | PFLAG_PENDING)) != (PFLAG_ALIVE | PFLAG_PENDING)) {
    return;
  }

  let cell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  let tgt = cellIndexW(cell);
  let won = atomicLoad(&claim[claimSlot(tgt)]) == particlePriority(p);

  if (won && voxMat(voxels[tgt]) == MAT_AIR) {
    // rejoin the grid; stamp 0xFF = "hasn't acted", falls next tick
    voxels[tgt] = packVox(p.payload & 0xFFFu, (p.payload >> 12u) & 0xFu, 0xFFu);
    markDirtyNext(cell);
    p.flags = 0u;  // dead
  } else {
    // lost the claim (or the cell got taken): rest, retry next tick
    p.flags = PFLAG_ALIVE;
    p.vx = 0; p.vy = 0; p.vz = 0;
  }
  pWrite[gid.x] = p;
}
