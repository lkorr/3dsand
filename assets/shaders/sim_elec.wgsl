// sim_elec.wgsl -- THE CHARGE FIELD: pages, rounds, settle (package E1).
//
// docs/PLAN_electricity.md section 1 is the design of record; DESIGN.md
// "Electricity -- charge field" the summary; src/sim/elec.h the layout (every
// constant below is mirrored from it and held there by
// scripts/check_invariants.py `elec`).
//
// WHAT IS STORED. One page per WINDOW chunk that holds charge: each of its
// 4,096 cells' potential P as a u16, in two halves (half 0 = the settled field
// every reader sees, half 1 = round scratch). A chunk with no page reads 0.
//
// WHAT RUNS, after the CA and the heat rows every CA-active tick
// (pass_table.def "elec"):
//   elecAlloc   ONE workgroup, called 1 + sim.elecRounds times (elecMeta's
//               PHASE word says which): page the wanted chunks in SLOT ORDER
//               (prefix sum over the want bitset), so who gets a page under
//               exhaustion is deterministic; the head re-keys pages whose slot
//               changed owner; the tail flips the lists or purges
//   elecRound   one group per live chunk, sim.elecRounds times: relax the
//               chunk to its fixpoint in shared memory; want the unpaged
//               neighbours the charge can enter
//   elecSettle  one group per live chunk: the last round into half 0; keep +
//               mark, or free an all-zero page
//   elecPurge   one group per chunk of a field nothing kept awake (rare)
//
// THE UPDATE: P' = max(seed, max_nb6(P_nb - resist(cell)), P - decay) >= 0 in
// a conducting cell; an insulator holds its seed (0 unless it is a source).
// The decay is taken once a tick, in round 0, off every stored value the
// round reads (its own and its halo). Within a round the chunk iterates AXIS
// SWEEPS -- each of 256 threads owns one line of 16 cells along x, then y,
// then z, and walks it forward and back -- until nothing changes (or the
// iteration cap). Across rounds the halo is the neighbours' previous round
// (Jacobi): a round reads half (r & 1) of every page and writes the other.
//
// DETERMINISM (rule 1). Integer only. A sweep thread writes only its own
// line; a round writes only its own page's scratch half and reads only the
// half the previous dispatch wrote. Atomics touch only counters, the want
// bitset (an OR), list cursors (which position a chunk got is unobservable:
// every group treats its chunk alone) and the free stack (which PAGE a slot
// got is unobservable: a free page is all zero). The allocation RANK is a
// prefix sum over the slot-ordered want bitset.
//
// COST (rule 2). A settled world records none of this (C_CAACTIVE). An active
// world with no charge: each elecAlloc reads 4 words a thread and dispatches
// zero rounds.

@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(55) var<storage, read_write> elecPool : array<u32>;
@group(0) @binding(56) var<storage, read_write> elecMeta : array<atomic<u32>>;
@group(0) @binding(57) var<storage, read> elecParams : array<u32>;

// MIRROR-BEGIN elec
// ---- THE CHARGE FIELD'S SHARED ACCESSORS (src/sim/elec.h) -------------------
// ONE block, pasted verbatim into sim_elec.wgsl and sim_step.wgsl and held
// identical by scripts/check_invariants.py `elec`, which also checks every
// constant against elec.h. Not in common.wgsl: an edit there misses the SPIR-V
// cache of every shader, and only these agree on the layout. A shader that
// pastes it binds elecPool (55) and elecMeta (56, atomic).
//
// READING P (packages E2 / E4): elecAt(slot, local) is the SETTLED field --
// half 0, what elecSettle left at the end of the LAST tick. The elec rows run
// after the CA, so inside the CA it is stable for the whole tick (as heatPool
// is). `slot` is a WINDOW slot (tickets carry no charge and read 0), `local`
// the chunk-local cell index (z * 256 + y * 16 + x).
const ELEC_POOL_PAGES : u32 = 2048u;
const ELEC_PAGE_WORDS : u32 = 4096u;
const ELEC_HALF_WORDS : u32 = 2048u;
const ELEC_ENTRY_HAS : u32 = 0x80000000u;
const ELEC_ENTRY_PAGE : u32 = 0x00FFFFFFu;
const EM_ENTRY : u32 = 64u;
const EM_WANT : u32 = 65600u;
const ELEC_R2_SOURCE : u32 = 33554432u;
const ELEC_R2_CONDUCTS : u32 = 67108864u;

fn elecAt(slot : u32, local : u32) -> u32 {
  if (slot >= NUM_CHUNKS) { return 0u; }
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  if ((e & ELEC_ENTRY_HAS) == 0u) { return 0u; }
  let w = elecPool[(e & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS + (local >> 1u)];
  return (w >> ((local & 1u) * 16u)) & 0xFFFFu;
}
// P at WORLD cell c: 0 outside the window.
fn elecAtCell(c : vec3<i32>) -> u32 {
  let wc = worldChunkOf(c);
  if (!chunkInWindow(wc, T.origin)) { return 0u; }
  let lo = vec3<u32>(c & vec3<i32>(CHUNK_MASK));
  return elecAt(chunkSlotIndex(wc), (lo.z * CHUNK + lo.y) * CHUNK + lo.x);
}
// MIRROR-END elec

const EM_CUR : u32 = 0u;
const EM_FREE_TOP : u32 = 1u;
const EM_NEXT_FRESH : u32 = 2u;
const EM_PHASE : u32 = 3u;
const EM_MARKED : u32 = 4u;
const EM_COUNT0 : u32 = 5u;
const EM_PAGES_PEAK : u32 = 16u;
const EM_REFUSED : u32 = 17u;
const EM_ALLOCS : u32 = 18u;
const EM_FREES : u32 = 19u;
const EM_PURGES : u32 = 20u;
const EM_STRANDED : u32 = 21u;
const EM_LIVE_PEAK : u32 = 22u;
const EM_REKEYED : u32 = 23u;
const EM_P_PEAK : u32 = 24u;
const EM_ROUND_CHUNKS : u32 = 25u;
const EM_COND_PEAK : u32 = 26u;
const EM_ROUND_HIST : u32 = 27u;
const EM_ARGS : u32 = 32u;
const EM_OWNER : u32 = 32832u;
const EM_LIST0 : u32 = 66624u;
const EM_LIST1 : u32 = 68672u;
const EM_STACK : u32 = 70720u;
const ELEC_ARG_ROUND : u32 = 0u;
const ELEC_ARG_PURGE : u32 = 1u;
const ELEC_ROUNDS_MAX : u32 = 8u;
const ELEC_RES_INS : u32 = 255u;
const EP_MODE : u32 = 0u;
const EP_ROUNDS : u32 = 1u;
const EP_DECAY : u32 = 2u;
const EP_ITER_CAP : u32 = 3u;
const EP_WET : u32 = 16u;
const EP_MAT : u32 = 32u;
const EP_MAT_STRIDE : u32 = 4u;
// Bit 31 of the dirty word (world.h kDirtyReasonName "elec"): the chunk holds
// charge. Not in sim_step's FILM_LICENCE: charge is not liquid progress.
const DIRTY_R_ELEC : u32 = 2147483648u;

fn elecMode() -> bool { return elecParams[EP_MODE] != 0u; }
fn elecRounds() -> u32 { return clamp(elecParams[EP_ROUNDS], 1u, ELEC_ROUNDS_MAX); }
fn elecList(which : u32) -> u32 { return select(EM_LIST0, EM_LIST1, which != 0u); }
fn elecMatWord(m : u32) -> u32 { return elecParams[EP_MAT + (m & 0xFFFu) * EP_MAT_STRIDE]; }
fn elecOwnerKey(wc : vec3<i32>) -> u32 {
  let u = vec3<u32>(wc & vec3<i32>(1023));
  return (u.x | (u.y << 10u) | (u.z << 20u)) + 1u;
}
fn elecSetArgs(rec : u32, x : u32) {
  let o = EM_ARGS + rec * 4u;
  atomicStore(&elecMeta[o], x);
  atomicStore(&elecMeta[o + 1u], 1u);
  atomicStore(&elecMeta[o + 2u], 1u);
}
fn elecSub(a : u32, b : u32) -> u32 { return select(0u, a - b, a > b); }

// The resist of the cell holding word w: 1..254, or ELEC_RES_INS. THE WET
// RULE: a cell under a coat whose material conducts (water, blood, brine's
// water...) conducts through the film -- min(own, wet[stain amount]) -- so a
// wet plank or wet stone carries what a dry one does not.
fn elecCellResist(w : u32) -> u32 {
  let m = voxMat(w);
  if (m == MAT_AIR) { return ELEC_RES_INS; }
  var r = elecMatWord(m) & 0xFFu;
  if (r == 0u) { r = ELEC_RES_INS; }
  if (voxStained(w)) {
    let coat = materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3 & 0xFFFu;
    if ((elecMatWord(coat) & 0xFFu) != 0u) { r = min(r, elecParams[EP_WET + voxStainAmt(w)]); }
  }
  return r;
}
fn elecCellSource(w : u32) -> u32 { return elecMatWord(voxMat(w)) >> 16u; }
// The word of chunk-local cell i of a chunk whose page-table entry is pe. A
// sentinel chunk is one material and carries no stain, so its word is the
// material id (the JITTER palette variant is irrelevant here).
fn elecWordIn(pe : u32, i : u32) -> u32 {
  if ((pe & PT_SENTINEL_BIT) != 0u) { return pe & PT_MAT_MASK; }
  return voxels[pe * CHUNK_VOL + i];
}

// ============================================================================
// elecAlloc: ONE workgroup. Which call this is comes from EM_PHASE:
//   0              the HEAD: re-key the live list's pages, then page the
//                  doorbell's wants (caMask: a source in a dirty chunk) onto
//                  this tick's list
//   1..rounds-1    between rounds: page the last round's wants onto it
//   rounds         the TAIL: page the last round's wants onto NEXT tick's list
//                  and flip -- or, when elecSettle could mark no chunk dirty,
//                  discard the wants and arm elecPurge over the whole field
// Rank every wanted slot by SLOT ORDER and give rank r the r-th page
// available: the free stack from its top, then never-used pages. A rank past
// the pool is REFUSED and counted, never an abort.
const ALLOC_WG : u32 = 256u;
const WANT_WORDS : u32 = NUM_CHUNKS / 32u;
const WANT_PER_THREAD : u32 = WANT_WORDS / ALLOC_WG;
var<workgroup> wgScan : array<u32, 256>;

@compute @workgroup_size(256)
fn elecAlloc(@builtin(local_invocation_index) li : u32) {
  let phase = atomicLoad(&elecMeta[EM_PHASE]);
  let rounds = elecRounds();
  let cur = atomicLoad(&elecMeta[EM_CUR]) & 1u;
  let nxt = 1u - cur;
  let tail = phase >= rounds;
  let dest = select(cur, nxt, tail);
  // RE-KEY (the head): a slot whose resident chunk changed (the window moved)
  // must not read the departed chunk's charge. Zero the page; elecSettle frees
  // it if nothing re-charges it.
  if (phase == 0u) {
    let n = atomicLoad(&elecMeta[EM_COUNT0 + cur]);
    for (var i = li; i < n; i += ALLOC_WG) {
      let slot = atomicLoad(&elecMeta[elecList(cur) + i]);
      let key = elecOwnerKey(slotWorldChunk(slot, T.origin));
      if (atomicLoad(&elecMeta[EM_OWNER + slot]) != key) {
        let base = (atomicLoad(&elecMeta[EM_ENTRY + slot]) & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS;
        for (var k = 0u; k < ELEC_PAGE_WORDS; k++) { elecPool[base + k] = 0u; }
        atomicStore(&elecMeta[EM_OWNER + slot], key);
        atomicAdd(&elecMeta[EM_REKEYED], 1u);
      }
    }
  }
  // Nothing is paged with the layer off, or at the tail of a tick whose
  // charge the CA could not be kept running over (see elecPurge).
  let purge = tail && atomicLoad(&elecMeta[EM_MARKED]) == 0u;
  let allocOn = elecMode() && !purge;
  let w0 = li * WANT_PER_THREAD;
  var cnt = 0u;
  for (var k = 0u; k < WANT_PER_THREAD; k++) {
    cnt += countOneBits(atomicLoad(&elecMeta[EM_WANT + w0 + k]));
  }
  wgScan[li] = cnt;
  workgroupBarrier();
  for (var off = 1u; off < ALLOC_WG; off <<= 1u) {
    var v = wgScan[li];
    if (li >= off) { v += wgScan[li - off]; }
    workgroupBarrier();
    wgScan[li] = v;
    workgroupBarrier();
  }
  let total = wgScan[ALLOC_WG - 1u];
  var rank = wgScan[li] - cnt;
  let top = atomicLoad(&elecMeta[EM_FREE_TOP]);
  let fresh = atomicLoad(&elecMeta[EM_NEXT_FRESH]);
  let avail = select(0u, top + (ELEC_POOL_PAGES - fresh), allocOn);
  let destCount = atomicLoad(&elecMeta[EM_COUNT0 + dest]);
  let destBase = elecList(dest);
  for (var k = 0u; k < WANT_PER_THREAD; k++) {
    var bits = atomicLoad(&elecMeta[EM_WANT + w0 + k]);
    if (bits == 0u) { continue; }
    atomicStore(&elecMeta[EM_WANT + w0 + k], 0u);
    while (bits != 0u) {
      let b = firstTrailingBit(bits);
      bits &= bits - 1u;
      let slot = (w0 + k) * 32u + b;
      let r = rank;
      rank += 1u;
      if (r >= avail) { continue; }
      var page : u32;
      if (r < top) { page = atomicLoad(&elecMeta[EM_STACK + top - 1u - r]); }
      else { page = fresh + (r - top); }
      atomicStore(&elecMeta[EM_ENTRY + slot], ELEC_ENTRY_HAS | page);
      atomicStore(&elecMeta[EM_OWNER + slot], elecOwnerKey(slotWorldChunk(slot, T.origin)));
      atomicStore(&elecMeta[destBase + destCount + r], slot);
    }
  }
  workgroupBarrier();
  if (li != 0u) { return; }
  let taken = min(total, avail);
  let fromStack = min(taken, top);
  atomicStore(&elecMeta[EM_FREE_TOP], top - fromStack);
  atomicStore(&elecMeta[EM_NEXT_FRESH], fresh + (taken - fromStack));
  if (allocOn) {
    atomicAdd(&elecMeta[EM_REFUSED], total - taken);
    atomicAdd(&elecMeta[EM_ALLOCS], taken);
  }
  atomicMax(&elecMeta[EM_PAGES_PEAK], fresh + (taken - fromStack) - (top - fromStack));
  let n = destCount + taken;
  if (!tail) {
    atomicStore(&elecMeta[EM_COUNT0 + dest], n);
    atomicStore(&elecMeta[EM_PHASE], phase + 1u);
    elecSetArgs(ELEC_ARG_ROUND, n);
    atomicMax(&elecMeta[EM_LIVE_PEAK], n);
    return;
  }
  // THE TAIL. Purge: the next list's chunks are zeroed and freed by elecPurge
  // (args record 1); either way next tick starts from the next list.
  if (purge) {
    elecSetArgs(ELEC_ARG_PURGE, n);
    atomicStore(&elecMeta[EM_COUNT0 + nxt], 0u);
    if (n != 0u) { atomicAdd(&elecMeta[EM_PURGES], 1u); }
  } else {
    elecSetArgs(ELEC_ARG_PURGE, 0u);
    atomicStore(&elecMeta[EM_COUNT0 + nxt], n);
  }
  atomicStore(&elecMeta[EM_COUNT0 + cur], 0u);
  atomicStore(&elecMeta[EM_CUR], nxt);
  atomicStore(&elecMeta[EM_PHASE], 0u);
  atomicStore(&elecMeta[EM_MARKED], 0u);
  elecSetArgs(ELEC_ARG_ROUND, 0u);
}

// ============================================================================
// elecRound: one group per chunk of this tick's live list.
//
// Shared memory holds the chunk: wgP[i] = P (bits 0..15) | resist (16..23),
// and the six faces' halo (the neighbour cell across each face, its previous
// round's P, 0 if unpaged or out of the window). A chunk with no page in a
// neighbour reads 0 there: charge enters a new chunk only once elecAlloc pages
// it, which is what the face wants below ask for.
var<workgroup> wgP : array<u32, 4096>;
var<workgroup> wgHalo : array<u32, 1536>;
var<workgroup> wgChanged : atomic<u32>;
var<workgroup> wgFaces : atomic<u32>;
var<workgroup> wgMax : atomic<u32>;
var<workgroup> wgCondMax : atomic<u32>;
var<workgroup> wgGo : u32;
var<workgroup> wgCap : u32;

// The neighbour chunk across face f (axis f >> 1, + side when f is odd).
fn elecFaceDir(f : u32) -> vec3<i32> {
  let s = select(-1, 1, (f & 1u) != 0u);
  let a = f >> 1u;
  return vec3<i32>(select(0, s, a == 0u), select(0, s, a == 1u), select(0, s, a == 2u));
}
// Chunk-local index of face cell (a, b) on face f, on THIS side (own = true)
// or the neighbour's mirrored side. Face axis x: (y = a, z = b); y: (x = a,
// z = b); z: (x = a, y = b) -- the same (a, b) the sweep thread li = a + 16 b
// owns.
fn elecFaceCell(f : u32, a : u32, b : u32, own : bool) -> u32 {
  let hi = (f & 1u) != 0u;
  let edge = select(select(15u, 0u, hi), select(0u, 15u, hi), own);
  let ax = f >> 1u;
  if (ax == 0u) { return (b * 16u + a) * 16u + edge; }
  if (ax == 1u) { return (b * 16u + edge) * 16u + a; }
  return (edge * 16u + b) * 16u + a;
}
// One axis sweep: thread li owns the line of 16 cells along `axis` through
// (a, b) = (li & 15, li >> 4), forward from the low face's halo and back from
// the high one's. Returns whether any cell rose.
fn elecSweep(axis : u32, li : u32) -> bool {
  var base : u32;
  var stride : u32;
  if (axis == 0u) { base = li * 16u; stride = 1u; }
  else if (axis == 1u) { base = (li >> 4u) * 256u + (li & 15u); stride = 16u; }
  else { base = li; stride = 256u; }
  var changed = false;
  var prev = wgHalo[(axis * 2u) * 256u + li];
  for (var t = 0u; t < 16u; t++) {
    let idx = base + t * stride;
    let c = wgP[idx];
    let res = c >> 16u;
    var p = c & 0xFFFFu;
    if (res != ELEC_RES_INS && prev > res && prev - res > p) {
      p = prev - res;
      wgP[idx] = p | (res << 16u);
      changed = true;
    }
    prev = p;
  }
  prev = wgHalo[(axis * 2u + 1u) * 256u + li];
  for (var t = 0u; t < 16u; t++) {
    let idx = base + (15u - t) * stride;
    let c = wgP[idx];
    let res = c >> 16u;
    var p = c & 0xFFFFu;
    if (res != ELEC_RES_INS && prev > res && prev - res > p) {
      p = prev - res;
      wgP[idx] = p | (res << 16u);
      changed = true;
    }
    prev = p;
  }
  return changed;
}

@compute @workgroup_size(256)
fn elecRound(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let cur = atomicLoad(&elecMeta[EM_CUR]) & 1u;
  let slot = atomicLoad(&elecMeta[elecList(cur) + wg.x]);
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  let page = (e & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS;
  let r = atomicLoad(&elecMeta[EM_PHASE]) - 1u;   // the head alloc made it 1
  let src = page + (r & 1u) * ELEC_HALF_WORDS;
  let dst = page + (1u - (r & 1u)) * ELEC_HALF_WORDS;
  let decay = select(0u, elecParams[EP_DECAY], r == 0u);
  let mode = elecMode();
  let wc = slotWorldChunk(slot, T.origin);
  let pe = pageEntryOf(slot);
  if (li == 0u) {
    atomicStore(&wgChanged, 0u);
    atomicStore(&wgFaces, 0u);
    atomicStore(&wgMax, 0u);
    atomicStore(&wgCondMax, 0u);
    wgCap = clamp(elecParams[EP_ITER_CAP], 1u, 64u);
  }
  // THE CELLS: seed, resist, and last round's P (this tick's decay in round 0).
  for (var i = li; i < CHUNK_VOL; i += 256u) {
    let w = elecWordIn(pe, i);
    var res = ELEC_RES_INS;
    var seed = 0u;
    if (mode) {
      res = elecCellResist(w);
      seed = elecCellSource(w);
    }
    var p = seed;
    if (res != ELEC_RES_INS) {
      let old = (elecPool[src + (i >> 1u)] >> ((i & 1u) * 16u)) & 0xFFFFu;
      p = max(p, elecSub(old, decay));
    }
    wgP[i] = p | (res << 16u);
  }
  // THE HALO: the neighbour cell across each face, as its previous round left
  // it (the same decay in round 0).
  for (var k = li; k < 1536u; k += 256u) {
    let f = k >> 8u;
    var v = 0u;
    if (mode) {
      let nwc = wc + elecFaceDir(f);
      if (chunkInWindow(nwc, T.origin)) {
        let ne = atomicLoad(&elecMeta[EM_ENTRY + chunkSlotIndex(nwc)]);
        if ((ne & ELEC_ENTRY_HAS) != 0u) {
          let nl = elecFaceCell(f, k & 15u, (k >> 4u) & 15u, false);
          let nsrc = (ne & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS + (r & 1u) * ELEC_HALF_WORDS;
          let old = (elecPool[nsrc + (nl >> 1u)] >> ((nl & 1u) * 16u)) & 0xFFFFu;
          v = elecSub(old, decay);
        }
      }
    }
    wgHalo[k] = v;
  }
  workgroupBarrier();
  let cap = workgroupUniformLoad(&wgCap);
  // THE FIXPOINT: x, y, z sweeps until a whole iteration raises nothing.
  for (var it = 0u; it < cap; it++) {
    var ch = elecSweep(0u, li);
    workgroupBarrier();
    ch = elecSweep(1u, li) || ch;
    workgroupBarrier();
    ch = elecSweep(2u, li) || ch;
    if (ch) { atomicAdd(&wgChanged, 1u); }
    workgroupBarrier();
    if (li == 0u) {
      wgGo = atomicLoad(&wgChanged);
      atomicStore(&wgChanged, 0u);
    }
    if (workgroupUniformLoad(&wgGo) == 0u) { break; }
  }
  // OUT: this round's half, and the peak.
  var lmax = 0u;
  var cmax = 0u;
  for (var j = li; j < ELEC_HALF_WORDS; j += 256u) {
    let c0 = wgP[2u * j];
    let c1 = wgP[2u * j + 1u];
    let lo = c0 & 0xFFFFu;
    let hi = c1 & 0xFFFFu;
    elecPool[dst + j] = lo | (hi << 16u);
    lmax = max(lmax, max(lo, hi));
    if ((c0 >> 16u) != ELEC_RES_INS) { cmax = max(cmax, lo); }
    if ((c1 >> 16u) != ELEC_RES_INS) { cmax = max(cmax, hi); }
  }
  if (lmax != 0u) { atomicMax(&wgMax, lmax); }
  if (cmax != 0u) { atomicMax(&wgCondMax, cmax); }
  // THE FACE WANTS: an unpaged window neighbour whose cell across a face can
  // take charge from this side (it conducts, and P here exceeds its resist).
  // Only those -- a chunk of air or stone beside a charged one is never paged.
  if (mode) {
    for (var f = 0u; f < 6u; f++) {
      let own = elecFaceCell(f, li & 15u, li >> 4u, true);
      let p = wgP[own] & 0xFFFFu;
      if (p <= 1u) { continue; }
      let d = elecFaceDir(f);
      let nwc = wc + d;
      if (!chunkInWindow(nwc, T.origin)) { continue; }
      if ((atomicLoad(&elecMeta[EM_ENTRY + chunkSlotIndex(nwc)]) & ELEC_ENTRY_HAS) != 0u) { continue; }
      let lo = vec3<i32>(i32(own & 15u), i32((own >> 4u) & 15u), i32(own >> 8u));
      let nres = elecCellResist(voxWordAt(wc * i32(CHUNK) + lo + d));
      if (nres != ELEC_RES_INS && p > nres) { atomicOr(&wgFaces, 1u << f); }
    }
  }
  workgroupBarrier();
  if (li != 0u) { return; }
  let faces = atomicLoad(&wgFaces);
  for (var f = 0u; f < 6u; f++) {
    if ((faces & (1u << f)) == 0u) { continue; }
    let q = chunkSlotIndex(wc + elecFaceDir(f));
    atomicOr(&elecMeta[EM_WANT + (q >> 5u)], 1u << (q & 31u));
  }
  atomicMax(&elecMeta[EM_P_PEAK], atomicLoad(&wgMax));
  atomicMax(&elecMeta[EM_COND_PEAK], atomicLoad(&wgCondMax));
  atomicAdd(&elecMeta[EM_ROUND_CHUNKS], 1u);
  atomicAdd(&elecMeta[EM_ROUND_HIST + min(r, 3u)], 1u);
}

// ============================================================================
// elecSettle: one group per chunk of this tick's live list, after the last
// round. The last round's half becomes half 0 (what every reader sees) and
// half 1 is zeroed. A chunk with charge goes on next tick's list, and is
// MARKED for the CA when a chunk of its 3x3x3 is dirty this tick -- the CPU
// page-table mirror's one-ring bound (the heat layer's rule): it is what keeps
// the CA, and with it every elec row, running while charge exists. An all-zero
// page goes back on the free stack.
var<workgroup> wgNonzero : atomic<u32>;
var<workgroup> wgDirtyNb : atomic<u32>;

@compute @workgroup_size(256)
fn elecSettle(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let cur = atomicLoad(&elecMeta[EM_CUR]) & 1u;
  let nxt = 1u - cur;
  let slot = atomicLoad(&elecMeta[elecList(cur) + wg.x]);
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  let page = (e & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS;
  let last = elecRounds() & 1u;   // round r wrote half 1 - (r & 1)
  if (li == 0u) {
    atomicStore(&wgNonzero, 0u);
    atomicStore(&wgDirtyNb, 0u);
  }
  workgroupBarrier();
  var nz = 0u;
  for (var j = li; j < ELEC_HALF_WORDS; j += 256u) {
    let v = elecPool[page + last * ELEC_HALF_WORDS + j];
    if (last != 0u) { elecPool[page + j] = v; }
    elecPool[page + ELEC_HALF_WORDS + j] = 0u;
    nz |= v;
  }
  if (nz != 0u) { atomicOr(&wgNonzero, 1u); }
  if (li < 27u) {
    let wc = slotWorldChunk(slot, T.origin) +
             vec3<i32>(i32(li % 3u) - 1, i32((li / 3u) % 3u) - 1, i32(li / 9u) - 1);
    if (chunkInWindow(wc, T.origin) && dirtyIn[chunkSlotIndex(wc)] != 0u) {
      atomicOr(&wgDirtyNb, 1u);
    }
  }
  workgroupBarrier();
  if (li != 0u) { return; }
  if (atomicLoad(&wgNonzero) != 0u) {
    let at = atomicAdd(&elecMeta[EM_COUNT0 + nxt], 1u);
    atomicStore(&elecMeta[elecList(nxt) + at], slot);
    if (atomicLoad(&wgDirtyNb) != 0u) {
      atomicOr(&dirtyOut[slot], DIRTY_R_ELEC);
      atomicAdd(&elecMeta[EM_MARKED], 1u);
    } else {
      atomicAdd(&elecMeta[EM_STRANDED], 1u);
    }
    return;
  }
  let at = atomicAdd(&elecMeta[EM_FREE_TOP], 1u);
  atomicStore(&elecMeta[EM_STACK + at], e & ELEC_ENTRY_PAGE);
  atomicStore(&elecMeta[EM_ENTRY + slot], 0u);
  atomicStore(&elecMeta[EM_OWNER + slot], 0u);
  atomicAdd(&elecMeta[EM_FREES], 1u);
}

// ============================================================================
// elecPurge: one group per chunk of a field the tail found NOTHING could keep
// awake (no chunk of it within a ring of this tick's dirty set). Charge the CA
// does not run over would go on evolving for exactly as many ticks as the CPU
// takes to prove the world settled -- a readback-timing outcome, which rule 1
// forbids. Zeroing it here makes that case a pure function of the tick. The
// tail has already flipped the lists, so the purged chunks are the CURRENT
// list's first (args record 1) entries; its count is already 0. Half 1 is
// zero (elecSettle), so only half 0 is cleared.
@compute @workgroup_size(256)
fn elecPurge(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let cur = atomicLoad(&elecMeta[EM_CUR]) & 1u;
  let slot = atomicLoad(&elecMeta[elecList(cur) + wg.x]);
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  let page = (e & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS;
  for (var j = li; j < ELEC_HALF_WORDS; j += 256u) { elecPool[page + j] = 0u; }
  if (li != 0u) { return; }
  let at = atomicAdd(&elecMeta[EM_FREE_TOP], 1u);
  atomicStore(&elecMeta[EM_STACK + at], e & ELEC_ENTRY_PAGE);
  atomicStore(&elecMeta[EM_ENTRY + slot], 0u);
  atomicStore(&elecMeta[EM_OWNER + slot], 0u);
  atomicAdd(&elecMeta[EM_FREES], 1u);
}
