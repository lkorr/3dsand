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

struct CellOp {
  cellIdx : u32,
  word    : u32,
};
@group(0) @binding(14) var<storage, read> cellOps : array<CellOp>;

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

fn markBoth(c : vec3<i32>) {
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
          let ci = chunkSlotIndex(n);
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

  let idx = cellIndexW(c);
  if (op.mode == 0u && voxMat(voxels[idx]) != MAT_AIR) { return; }  // paint fills air only

  var mat = op.material;
  if (op.mode == 2u) {
    // melt (laser, PLAN §C1): each cell converts to ITS OWN molten product
    // from the material table — stone becomes lava while the sand next to it
    // becomes molten glass. Air stays air, 255-hardness matter is immune.
    let cur = voxMat(voxels[idx]);
    if (cur == MAT_AIR || materials[cur].hardness >= 255u) { return; }
    mat = materials[cur].molten;
  }

  let rnd = hash3(T.seed ^ 0x5EEDu, T.tick, idx);
  // liquids are born full (their state nibble is fullness); everything else
  // gets a palette variant. STAMP_NEVER = "hasn't acted": falls this tick.
  var state = rnd % 3u;
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) {
    state = LIQ_FULL_STATE;
  }
  voxels[idx] = packVox(mat, state, STAMP_NEVER);
  markBoth(c);
}

// Exact-cell writes (island removal / rubble handoff, DESIGN.md §7). Same
// MutationQueue discipline as the brush: the op stream is the only CPU->grid
// path, so saves/replays/networking capture island events for free.
// Dispatch: ceil(cellCount / 64) workgroups of 64.
@compute @workgroup_size(64)
fn cells(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.cellCount) { return; }
  let op = cellOps[gid.x];
  if (op.cellIdx >= WORLD_N * WORLD_N * WORLD_N) { return; }
  var word = op.word;
  // prefab paint mode: fill air only (flag is spare-bit metadata, never stored)
  if ((word & CELLOP_IF_AIR) != 0u) {
    if (voxMat(voxels[op.cellIdx]) != MAT_AIR) { return; }
    word &= ~CELLOP_IF_AIR;
  }
  voxels[op.cellIdx] = word;

  // cellIdx is a SLOT index: reconstruct the world cell through the window
  let ci = op.cellIdx / CHUNK_VOL;
  let lo = op.cellIdx % CHUNK_VOL;
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let l = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  markBoth(slotToWorldChunk(sc, T.origin) * i32(CHUNK) + l);
}
