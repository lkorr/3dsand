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
// "Any cell in this chunk carries STAIN bits" — bit 31 of the occupancy word
// (packOccStain in common.wgsl). Accumulated as an OR, expressed as an atomic
// max so it needs no separate reduction: any thread that saw stain stores 1.
//
// This is what makes a page reclaimable WITHOUT a readback. It must be counted
// OUTSIDE the `m != MAT_AIR` branch that guards the count/blocker/hash
// accumulation, because stain on an AIR cell is exactly the case that matters:
// an all-air chunk that still carries stain is hashed state and MUST NOT be
// demoted to PT_EMPTY. Folding it into the non-air branch would report those
// chunks as clean and silently drop the stain — the failure this bit exists to
// prevent.
var<workgroup> wgStain : atomic<u32>;
// Sub-chunk occupancy bitmask (common.wgsl SUB-CHUNK OCCUPANCY block):
// [0..1] = TOTAL class, [2..3] = BLOCKER class, laid out exactly as
// subOccIndex expects so the store at the end is a straight copy.
//
// Accumulated per-thread in REGISTERS and OR-ed in once, not per cell: a naive
// atomicOr per non-air voxel would be up to 4,096 workgroup atomics per chunk
// on a loop whose whole job is to be cheap. Four atomics per thread instead.
var<workgroup> wgSub : array<atomic<u32>, 4>;

// The mask store, so the three exits below (EMPTY sentinel, uniform sentinel,
// resident) cannot drift apart on the layout. SUBOCC_WORDS is 2, and this is
// the one place that assumes it.
fn storeSubOcc(slot : u32, t0 : u32, t1 : u32, b0 : u32, b1 : u32) {
  occupancy[subOccIndex(slot, 0u, 0u)] = t0;
  occupancy[subOccIndex(slot, 0u, 1u)] = t1;
  occupancy[subOccIndex(slot, 1u, 0u)] = b0;
  occupancy[subOccIndex(slot, 1u, 1u)] = b1;
}

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
    atomicStore(&wgStain, 0u);
    atomicStore(&wgSub[0], 0u);
    atomicStore(&wgSub[1], 0u);
    atomicStore(&wgSub[2], 0u);
    atomicStore(&wgSub[3], 0u);
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
  var stain = 0u;
  var sm0 = 0u; var sm1 = 0u;   // TOTAL class, bits 0..31 / 32..63
  var sb0 = 0u; var sb1 = 0u;   // BLOCKER class
  if (!sentinel) {
    for (var i = li; i < CHUNK_VOL; i += 64u) {
      let w = voxels[base + i];
      // OUTSIDE the non-air test on purpose: stain on an air cell is the whole
      // point (see the wgStain note above).
      if ((w & STAIN_BITS) != 0u) { stain = 1u; }
      let m = w & 0xFFFu;
      if (m != MAT_AIR) {
        count += 1u;
        // Sub-chunk bit for this cell. Costs one shift-and-or on a path that
        // has already paid for the load and the material fetch.
        let sbit = subOccBitOfLocalIdx(i);
        let sm = 1u << (sbit & 31u);
        if (sbit < 32u) { sm0 |= sm; } else { sm1 |= sm; }
        if (isRayBlocker(materials[m])) {
          block += 1u;
          if (sbit < 32u) { sb0 |= sm; } else { sb1 |= sm; }
        }
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  atomicMax(&wgStain, stain);
  atomicOr(&wgSub[0], sm0);
  atomicOr(&wgSub[1], sm1);
  atomicOr(&wgSub[2], sb0);
  atomicOr(&wgSub[3], sb1);
  workgroupBarrier();

  if (li == 0u) {
    if (sentinel) {
      // synthWord, NOT synthWordAt, and that is correct for JITTER too: this
      // path reads only the MATERIAL, which a JITTER sentinel holds uniformly
      // (it varies the state nibble and nothing else). Occupancy counts
      // non-air cells and ray blockers, both material-only decisions. Do not
      // "fix" this to be positional — it would cost 4,096 hash evaluations to
      // compute a value that cannot change.
      // A sentinel carries NO stain by construction: synthWord builds a word
      // from a material and (for JITTER) a state nibble, and neither form has
      // stain bits. So the stain flag is 0 here, which is also what keeps the
      // demote/rematerialize round trip stable — a chunk demoted BECAUSE it
      // was stainless must not come back claiming stain.
      let m = synthWord(e) & 0xFFFu;
      if (m == MAT_AIR) {
        occupancy[slot] = packOcc(0u, 0u);
        storeSubOcc(slot, 0u, 0u, 0u, 0u);
      } else {
        let isB = isRayBlocker(materials[m]);
        occupancy[slot] = packOcc(CHUNK_VOL, select(0u, CHUNK_VOL, isB));
        // Uniform matter: every block of every class it belongs to is full.
        let bo = select(0u, subOccAllOnes(), isB);
        storeSubOcc(slot, subOccAllOnes(), subOccAllOnes(), bo, bo);
      }
    } else {
      occupancy[slot] = packOccStain(atomicLoad(&wgCount), atomicLoad(&wgBlock),
                                     atomicLoad(&wgStain) != 0u);
      storeSubOcc(slot, atomicLoad(&wgSub[0]), atomicLoad(&wgSub[1]),
                  atomicLoad(&wgSub[2]), atomicLoad(&wgSub[3]));
    }
  }
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgStain, 0u);
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgHash, 0u);
    atomicStore(&wgSub[0], 0u);
    atomicStore(&wgSub[1], 0u);
    atomicStore(&wgSub[2], 0u);
    atomicStore(&wgSub[3], 0u);
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
      if (li == 0u) {
        occupancy[wg.x] = packOcc(0u, 0u);
        storeSubOcc(wg.x, 0u, 0u, 0u, 0u);
      }
      return;
    }
    // UNIFORM: the same 4096 hash evaluations the dense path does, but the
    // word comes from synthWord instead of memory. Spread across all 64
    // threads exactly as the dense loop does — there is no reason to
    // serialize it, and it saves the 16 KiB of traffic, not the ALU.
    //
    // JITTER: identical in shape, but the word varies per cell, so `sv` moves
    // INSIDE the loop and comes from synthWordAt at that cell's world
    // position. The hash still keys on the SLOT base (§4.1, review M3) —
    // only the VALUE is positional, never the key. Splitting the loop rather
    // than branching per iteration keeps the uniform case exactly as cheap as
    // it was.
    let sHashBase = wg.x * CHUNK_VOL;          // SLOT index — the hash key
    var sh = 0u;
    if ((e & PT_JITTER_BIT) != 0u) {
      for (var i = li; i < CHUNK_VOL; i += 64u) {
        let jw = synthWordAt(e, worldCellOfSlotLocal(wg.x, i), T.seed);
        let jv = (jw & 0xFFFFu) | ((jw & STAIN_BITS) >> 8u);
        sh += pcg((sHashBase + i) ^ (jv * 0x9E3779B9u));
      }
    } else {
      let sv = (sw & 0xFFFFu) | ((sw & STAIN_BITS) >> 8u);
      for (var i = li; i < CHUNK_VOL; i += 64u) {
        sh += pcg((sHashBase + i) ^ (sv * 0x9E3779B9u));
      }
    }
    if (T.hashEnable != 0u) { atomicAdd(&wgHash, sh); }
    workgroupBarrier();
    if (li == 0u) {
      let sIsB = isRayBlocker(materials[smat]);
      occupancy[wg.x] = packOcc(CHUNK_VOL, select(0u, CHUNK_VOL, sIsB));
      let sbo = select(0u, subOccAllOnes(), sIsB);
      storeSubOcc(wg.x, subOccAllOnes(), subOccAllOnes(), sbo, sbo);
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
  var stain = 0u;
  var h = 0u;
  var sm0 = 0u; var sm1 = 0u;   // sub-chunk TOTAL class
  var sb0 = 0u; var sb1 = 0u;   // sub-chunk BLOCKER class
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
    // OUTSIDE the non-air branch below, deliberately: an all-air chunk that
    // still carries stain is hashed state and must keep its page (see wgStain).
    if ((w & STAIN_BITS) != 0u) { stain = 1u; }
    let m = v & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      let sbit = subOccBitOfLocalIdx(i);
      let sm = 1u << (sbit & 31u);
      if (sbit < 32u) { sm0 |= sm; } else { sm1 |= sm; }
      if (isRayBlocker(materials[m])) {
        block += 1u;
        if (sbit < 32u) { sb0 |= sm; } else { sb1 |= sm; }
      }
      if (T.hashEnable != 0u) {
        h += pcg((hashBase + i) ^ (v * 0x9E3779B9u));
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  atomicMax(&wgStain, stain);
  atomicOr(&wgSub[0], sm0);
  atomicOr(&wgSub[1], sm1);
  atomicOr(&wgSub[2], sb0);
  atomicOr(&wgSub[3], sb1);
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();

  if (li == 0u) {
    occupancy[wg.x] = packOccStain(atomicLoad(&wgCount), atomicLoad(&wgBlock),
                                   atomicLoad(&wgStain) != 0u);
    storeSubOcc(wg.x, atomicLoad(&wgSub[0]), atomicLoad(&wgSub[1]),
                atomicLoad(&wgSub[2]), atomicLoad(&wgSub[3]));
    if (T.hashEnable != 0u) {
      atomicAdd(&worldHash[0], atomicLoad(&wgHash));
    }
  }
}
