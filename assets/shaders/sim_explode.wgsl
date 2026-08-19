// sim_explode.wgsl — explosion destruction (DESIGN.md §7), two-phase for
// determinism. Each voxel inside the blast ball independently DDA-walks the
// segment from the blast center to itself, summing the hardness of everything
// in between (hard materials shadow soft ones behind them) plus linear
// distance falloff. If the power that survives the walk exceeds the voxel's
// own hardness, the voxel is destroyed; a material-class-dependent fraction of
// destroyed voxels eject as ballistic particles with outward velocity.
//
// Determinism: phase `mark` only READS the grid and records each destroyed
// cell's surviving power in a per-op scratch (each thread writes only its own
// cell — no atomics, no ordering). Phase `apply` consumes the scratch and
// never walks rays, so its voxel writes cannot influence any other thread's
// outcome. A single-phase version raced: one thread zeroing a voxel changed
// another thread's occlusion ray mid-flight. Overlapping same-tick ops dedupe
// in `apply`: the lowest op index that destroyed a cell owns it.
//
// Dispatch (both phases): (EXP_WG * expCount, EXP_WG, EXP_WG) of 4x4x4.

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn  : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;

@group(1) @binding(0) var<storage, read_write> pReadBuf : array<Particle>;
@group(1) @binding(2) var<storage, read_write> counts   : array<atomic<u32>>;
@group(1) @binding(5) var<storage, read>       expOps   : array<ExplosionOp>;
@group(1) @binding(6) var<storage, read_write> expMask  : array<u32>;

const FALLOFF_PER_CELL : i32 = 6;   // power lost per cell of distance (in air)

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

fn hardnessAt(c : vec3<i32>) -> i32 {
  if (!inBounds(c)) { return 100000; }  // residency edge absorbs everything
  let mat = voxMat(voxels[cellIndexW(c)]);
  if (mat == MAT_AIR) { return 0; }
  return i32(materials[mat].hardness);
}

fn markBoth(c : vec3<i32>) {  // callers have bounds-checked c
  let ci = chunkIndexW(c);
  atomicStore(&dirtyIn[ci], 1u);
  atomicStore(&dirtyOut[ci], 1u);
}

fn maskIndex(opIdx : u32, local : vec3<i32>) -> u32 {
  let l = local + vec3<i32>(EXP_R_MAX);
  return opIdx * EXP_MASK_STRIDE + u32((l.z * EXP_BOX + l.y) * EXP_BOX + l.x);
}

@compute @workgroup_size(4, 4, 4)
fn mark(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  let opIdx = wg.x / EXP_WG;
  if (opIdx >= T.expCount) { return; }
  let op = expOps[opIdx];

  let local = vec3<i32>(vec3<u32>(wg.x % EXP_WG, wg.y, wg.z) * 4u + lid) -
              vec3<i32>(EXP_R_MAX);
  let d2 = dot(local, local);
  if (d2 > op.radius * op.radius) { return; }
  let c = vec3<i32>(op.cx, op.cy, op.cz) + local;
  if (!inBounds(c)) { return; }

  // remaining power after occlusion along the center->cell segment + falloff
  let center = vec3<i32>(op.cx, op.cy, op.cz);
  var occlusion = 0;
  let steps = max(max(abs(local.x), abs(local.y)), abs(local.z));
  for (var s = 1; s < steps; s++) {
    let sample = center + (local * s) / steps;
    occlusion += hardnessAt(sample);
    if (occlusion >= op.power) { break; }
  }
  let remaining = op.power - occlusion - i32(isqrt(u32(d2))) * FALLOFF_PER_CELL;

  let mat = voxMat(voxels[cellIndexW(c)]);
  if (mat == MAT_AIR) {
    // shockwave through open space: wake the chunk so settled piles at the
    // cavity edge re-check support this tick
    if (remaining > 0) { markBoth(c); }
    return;
  }
  if (remaining <= i32(materials[mat].hardness)) {
    if (remaining > 0) { markBoth(c); }  // scorched but standing: stays awake
    return;
  }
  expMask[maskIndex(opIdx, local)] = u32(max(remaining, 0)) + 1u;
}

fn destroyedBy(opIdx : u32, c : vec3<i32>) -> u32 {
  let op = expOps[opIdx];
  let local = c - vec3<i32>(op.cx, op.cy, op.cz);
  if (max(max(abs(local.x), abs(local.y)), abs(local.z)) > EXP_R_MAX) { return 0u; }
  return expMask[maskIndex(opIdx, local)];
}

@compute @workgroup_size(4, 4, 4)
fn apply(@builtin(workgroup_id) wg : vec3<u32>,
         @builtin(local_invocation_id) lid : vec3<u32>) {
  let opIdx = wg.x / EXP_WG;
  if (opIdx >= T.expCount) { return; }
  let op = expOps[opIdx];

  let local = vec3<i32>(vec3<u32>(wg.x % EXP_WG, wg.y, wg.z) * 4u + lid) -
              vec3<i32>(EXP_R_MAX);
  let d2 = dot(local, local);
  if (d2 > op.radius * op.radius) { return; }
  let c = vec3<i32>(op.cx, op.cy, op.cz) + local;
  if (!inBounds(c)) { return; }

  let val = expMask[maskIndex(opIdx, local)];
  if (val == 0u) { return; }
  // overlap dedupe: the lowest op index that destroyed this cell owns it
  for (var j = 0u; j < opIdx; j++) {
    if (destroyedBy(j, c) != 0u) { return; }
  }

  let idx = cellIndexW(c);
  let w = voxels[idx];
  voxels[idx] = 0u;
  markBoth(c);

  let m = materials[voxMat(w)];
  var ejectPerMille = 250u;
  if (m.klass == CLASS_LIQUID) { ejectPerMille = 500u; }
  else if (m.klass == CLASS_POWDER) { ejectPerMille = 350u; }
  else if (m.klass == CLASS_GAS) { ejectPerMille = 0u; }

  let rnd = hash3(T.seed ^ 0xB0011u, T.tick, idx);
  if (rnd % 1000u >= ejectPerMille) { return; }

  let slot = atomicAdd(&counts[T.page], 1u);
  if (slot >= PARTICLE_CAP) { return; }  // ring full: voxel just vaporizes

  // outward velocity scaled by surviving power, plus per-cell jitter
  let remaining = i32(val - 1u);
  let dist = max(i32(isqrt(u32(d2))), 1);
  let speed = min(PART_MAX_VEL, PART_ONE + remaining * 2);
  var p : Particle;
  p.px = c.x * PART_ONE + 128;
  p.py = c.y * PART_ONE + 128;
  p.pz = c.z * PART_ONE + 128;
  p.vx = local.x * speed / dist + (i32((rnd >> 8u) & 0x7Fu) - 64);
  p.vy = local.y * speed / dist + (i32((rnd >> 15u) & 0x7Fu) - 32);  // bias up
  p.vz = local.z * speed / dist + (i32((rnd >> 22u) & 0x7Fu) - 64);
  p.payload = w & 0xFFFFu;
  p.flags = PFLAG_ALIVE;
  pReadBuf[slot] = p;
}
