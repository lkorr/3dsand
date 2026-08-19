// sim_mutate.wgsl — applies the CPU MutationQueue (brush paints/erases) to the
// grid. Runs before the CA passes each tick. Every world write flows through
// this path (DESIGN.md §2: the MutationQueue is also the future save/replay/
// network format).
//
// Dispatch: (4 * opsCount, 4, 4) workgroups of 4x4x4 threads — each op gets a
// 16^3 thread box centered on it (max brush radius 7).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn  : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(6) var<storage, read> ops : array<BrushOp>;

fn markBoth(c : vec3<i32>) {
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
          atomicStore(&dirtyIn[ci], 1u);   // simulate this tick
          atomicStore(&dirtyOut[ci], 1u);  // and re-check next tick
        }
      }
    }
  }
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  let opIdx = wg.x / 4u;
  if (opIdx >= T.opsCount) { return; }
  let op = ops[opIdx];

  let local = vec3<i32>(vec3<u32>((wg.x % 4u), wg.y, wg.z) * 4u + lid) - vec3<i32>(8, 8, 8);
  if (dot(local, local) > op.radius * op.radius) { return; }
  let c = vec3<i32>(op.cx, op.cy, op.cz) + local;
  if (!inBounds(c)) { return; }

  let idx = cellIndex(vec3<u32>(c));
  if (op.mode == 0u && voxMat(voxels[idx]) != MAT_AIR) { return; }  // paint fills air only

  let rnd = hash3(T.seed ^ 0x5EEDu, T.tick, idx);
  // liquids are born full (their state nibble is fullness); everything else
  // gets a palette variant. stamp 0xFF = "hasn't acted": falls this tick.
  var state = rnd % 3u;
  if (op.material != MAT_AIR && materials[op.material].klass == CLASS_LIQUID) {
    state = LIQ_FULL_STATE;
  }
  voxels[idx] = packVox(op.material, state, 0xFFu);
  markBoth(c);
}
