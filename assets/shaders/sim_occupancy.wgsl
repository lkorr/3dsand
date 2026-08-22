// sim_occupancy.wgsl — per-chunk occupancy words (renderer empty-space
// skipping) + optional whole-world hash (determinism check / desync detector).
// Each occupancy word packs two counts (packOcc in common.wgsl): low 16 bits =
// non-air voxels, high 16 = ray blockers (solid/powder/opaque-liquid). Shadow
// rays chunk-skip on the blocker count, so gas plumes don't defeat the skip.
// The hash is a wrapping sum of per-cell hashes: commutative, so the atomic
// accumulation order cannot affect the result — deterministic by construction.
//
// Dispatch: (NUM_CHUNKS, 1, 1) workgroups; each workgroup reduces one chunk.

@group(0) @binding(0) var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read_write> occupancy : array<u32>;
@group(0) @binding(8) var<storage, read_write> worldHash : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgBlock : atomic<u32>;
var<workgroup> wgHash  : atomic<u32>;

// mainDirty: occupancy update over only the chunks written this tick
// (indirect over the compacted dirtyOut list) — the per-tick sim cost stays
// proportional to activity, not world size (DESIGN.md §11). No hash: the
// whole-world hash needs every chunk, so hash ticks use the full pass below.
@compute @workgroup_size(64)
fn mainDirty(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
  }
  workgroupBarrier();

  // This entry point computes NO hash, so it needs only the LOAD base — the
  // page. Stated because the full pass below deliberately splits two bases and
  // nobody should "fix" this one symmetrically (§4.1).
  let slot = dirtyList[wg.x];
  let e = pageTable[slot];

  // The sentinel branch here is MANDATORY, not a belt. Under the corrected
  // materialization rule (§3.2a) a dirty EMPTY chunk with no non-empty
  // neighbour is deliberately NOT materialized, and a daylight wake-all makes
  // every chunk dirty — so `mainDirty` sees sentinels routinely, by design.
  // It must NOT increment pageFaults: a sentinel here is expected, not a
  // fault, and a counter that fires in normal operation is one nobody looks at.
  //
  // No early `return` here, and that is deliberate: `sentinel` is uniform
  // across the workgroup (every thread read the same table entry), but Tint
  // cannot prove it — dirtyList is a read_write storage buffer, so the value
  // is "possibly non-uniform" to the analysis and a barrier after a return
  // guarded by it is rejected. Gating the LOOP instead keeps every path to
  // the barrier uniform and costs nothing: a sentinel chunk runs zero
  // iterations.
  let sentinel = (e & PT_SENTINEL_BIT) != 0u;
  let base = select(e * CHUNK_VOL, 0u, sentinel);
  var count = 0u;
  var block = 0u;
  if (!sentinel) {
    for (var i = li; i < CHUNK_VOL; i += 64u) {
      let m = voxels[base + i] & 0xFFFu;
      if (m != MAT_AIR) {
        count += 1u;
        if (isRayBlocker(materials[m])) { block += 1u; }
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  workgroupBarrier();

  if (li == 0u) {
    if (sentinel) {
      let m = synthWord(e) & 0xFFFu;
      if (m == MAT_AIR) {
        occupancy[slot] = packOcc(0u, 0u);
      } else {
        occupancy[slot] = packOcc(CHUNK_VOL,
            select(0u, CHUNK_VOL, isRayBlocker(materials[m])));
      }
    } else {
      occupancy[slot] = packOcc(atomicLoad(&wgCount), atomicLoad(&wgBlock));
    }
  }
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgHash, 0u);
  }
  workgroupBarrier();

  // ---- the analytic sentinel path (§4.1) ----
  // Taken AFTER the li == 0 prologue and its workgroupBarrier above, because
  // wgHash must be zeroed before any thread can add to it.
  let e = pageTable[wg.x];
  if ((e & PT_SENTINEL_BIT) != 0u) {
    let sw = synthWord(e);
    let smat = sw & 0xFFFu;
    if (smat == MAT_AIR) {
      // EXACT, and with no arithmetic at all: the dense loop's
      // `if (m != MAT_AIR)` guard means an all-air chunk contributes 0 to
      // count, blockers AND hash. The analytic path for EMPTY is: skip.
      if (li == 0u) { occupancy[wg.x] = packOcc(0u, 0u); }
      return;
    }
    // UNIFORM: the same 4096 hash evaluations the dense path does, but the
    // word comes from synthWord instead of memory. Spread across all 64
    // threads exactly as the dense loop does — there is no reason to
    // serialize it, and it saves the 16 KiB of traffic, not the ALU.
    let sv = (sw & 0xFFFFu) | ((sw & STAIN_BITS) >> 8u);
    let sHashBase = wg.x * CHUNK_VOL;          // SLOT index — the hash key
    var sh = 0u;
    for (var i = li; i < CHUNK_VOL; i += 64u) {
      sh += pcg((sHashBase + i) ^ (sv * 0x9E3779B9u));
    }
    if (T.hashEnable != 0u) { atomicAdd(&wgHash, sh); }
    workgroupBarrier();
    if (li == 0u) {
      occupancy[wg.x] = packOcc(CHUNK_VOL,
          select(0u, CHUNK_VOL, isRayBlocker(materials[smat])));
      if (T.hashEnable != 0u) {
        atomicAdd(&worldHash[0], atomicLoad(&wgHash));
      }
    }
    return;
  }

  // ---- resident: TWO DIFFERENT BASES, and conflating them is a silent
  // desync. Today's code used one `base` for both purposes because under a
  // dense layout page index IS slot index. Paging splits them, and only the
  // LOAD follows the page; the hash must keep keying on the SLOT or every
  // non-identity page assignment changes the world hash (§4.1, review M3).
  let loadBase = e * CHUNK_VOL;      // PAGE index — where the words are
  let hashBase = wg.x * CHUNK_VOL;   // SLOT index — what the hash keys on
  var count = 0u;
  var block = 0u;
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    // What counts as "state identity" for the determinism hash: material +
    // state nibble (bits 0..15) and the STAIN layer (bits 24..30).
    //
    // The tick-stamp byte (16..23) is deliberately excluded — it is scheduling
    // bookkeeping ("has this voxel acted this substep"), not world state, and
    // it legitimately differs between two runs that reached the same world.
    //
    // The stain bits ARE state: a sim kernel writes them, they persist, and
    // they change what the world looks like. Leaving them out of the hash
    // would mean --selftest could not tell a correctly-stained world from one
    // where staining diverged across vendors — exactly the hole the hash
    // exists to close (CLAUDE.md rule 1). Bit 31 stays out: it is the
    // transient CELLOP_IF_AIR message flag and is never stored.
    let w = voxels[loadBase + i];
    let v = (w & 0xFFFFu) | ((w & STAIN_BITS) >> 8u);
    let m = v & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      if (isRayBlocker(materials[m])) { block += 1u; }
      if (T.hashEnable != 0u) {
        h += pcg((hashBase + i) ^ (v * 0x9E3779B9u));
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();

  if (li == 0u) {
    occupancy[wg.x] = packOcc(atomicLoad(&wgCount), atomicLoad(&wgBlock));
    if (T.hashEnable != 0u) {
      atomicAdd(&worldHash[0], atomicLoad(&wgHash));
    }
  }
}
