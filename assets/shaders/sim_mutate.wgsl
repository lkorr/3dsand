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
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

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

  // TWO BASES (§4.1): slotIdx keys the palette-variant RNG, idx addresses
  // memory. See the note in sim_step:main.
  let slotIdx = cellIndexW(c);
  let idx = voxWordIndex(c);
  if (op.mode == 0u && voxMat(voxWordAt(c)) != MAT_AIR) { return; }  // paint fills air only

  var mat = op.material;
  if (op.mode == 2u) {
    // melt (laser, PLAN §C1): each cell converts to ITS OWN molten product
    // from the material table — stone becomes lava while the sand next to it
    // becomes molten glass. Air stays air, 255-hardness matter is immune.
    let cur = voxMat(voxWordAt(c));
    if (cur == MAT_AIR || materials[cur].hardness >= 255u) { return; }
    mat = materials[cur].molten;
  }

  let rnd = hash3(T.seed ^ 0x5EEDu, T.tick, slotIdx);
  // liquids are born full (their state nibble is fullness); everything else
  // gets a palette variant. STAMP_NEVER = "hasn't acted": falls this tick.
  var state = rnd % 3u;
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) {
    state = LIQ_FULL_STATE;
  }
  voxStore(idx, packVox(mat, state, STAMP_NEVER));
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
  // cellIdx is a SLOT index, so under paging it is NOT a physical word index
  // (§5.5). Decompose it — which this entry point already did below, to
  // reconstruct the world cell for markBoth — and translate through the table.
  // The same decomposition the CPU uses to build the materialization set is
  // the one the shader uses to index the table, which is the point.
  let ci = op.cellIdx / CHUNK_VOL;
  let lo = op.cellIdx % CHUNK_VOL;
  let wordIdx = voxWordInChunk(ci, lo);
  // prefab paint mode: fill air only (flag is spare-bit metadata, never stored)
  //
  // Reading a sentinel HERE is legal and correct: a paint-into-air op against
  // an EMPTY chunk should see air and proceed. That it can proceed is the
  // CPU's obligation — §3.3 materializes every op target unfiltered, precisely
  // so a brush into open sky is not silently a no-op.
  if ((word & CELLOP_IF_AIR) != 0u) {
    if (voxMat(voxWordInChunkAt(ci, lo)) != MAT_AIR) { return; }
    word &= ~CELLOP_IF_AIR;
  }
  voxStore(wordIdx, word);

  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let l = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  markBoth(slotToWorldChunk(sc, T.origin) * i32(CHUNK) + l);
}

// ---- WIND PRIMITIVE FOOTPRINT WAKE (docs/RESEARCH_wind.md §4.3, §10) -------
//
// The one thing in the engine that dirty-marks a chunk WITHOUT writing a voxel,
// and the reason phase 2 had to land before entrainment could be switched on.
//
// A settled sand dune is asleep. Its chunk carries no dirty flag, it is not in
// the compacted dispatch list, and no CA invocation ever visits it — so a fan
// pointed at it would do nothing at all, however hard it blew. Something has to
// wake the footprint, and the ambient field is categorically not allowed to
// (invariant 3: an exposed dune under a steady breeze would re-mark its own
// chunks for as long as the weather held, which is rule 2 with the sign
// flipped).
//
// A PRIMITIVE can, because it is bounded and player-caused. The CPU resolves
// the footprint (WindPrimSystem::BuildWake), filters it against the snapshot's
// occupancy so a cube of sky costs nothing, charges it against a per-tick chunk
// budget, and ships the surviving SLOT indices in TickParams. This kernel is
// the last step: set the flag.
//
// WHY THAT ORDER IS THE WHOLE POINT. The same CPU pass that fills this list
// also declares those chunks to the page table as op targets, so they are
// materialized WITH THEIR 26-RING before the command buffer is built. The page
// table's materialization set is tightened against a lagging snapshot on the
// argument that settled matter writes nothing — entrainment breaks that
// argument, and this is what repairs it: by the time a grain steps into a
// neighbouring chunk, the CPU had already said that chunk could be written.
// Without it the write lands on a sentinel and the voxel is simply lost (62
// reproducible faults over two 160-tick runs; §10).
//
// Both flags, exactly as markBoth sets them: dirtyIn so the chunk simulates
// THIS tick (the compaction runs after this kernel), dirtyOut so it is
// re-checked next tick even if nothing moved. Idempotent stores, never atomic
// arithmetic, so the order two invocations land in cannot matter.
//
// Dispatch: ceil(windWakeCount / 64) workgroups of 64. Zero primitives with the
// entrainment licence means zero count means the row is not recorded at all.
@compute @workgroup_size(64)
fn windWake(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.windWakeCount) { return; }
  // The list is four slots to a std140 row (world.h TickParams).
  let slot = T.windWake[gid.x / 4u][gid.x % 4u];
  if (slot >= NCHUNK * NCHUNK * NCHUNK) { return; }
  atomicStore(&dirtyIn[slot], 1u);
  atomicStore(&dirtyOut[slot], 1u);
}
