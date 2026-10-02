// sim_heat.wgsl -- THE TEMPERATURE LAYER: pages, sources, targets, relaxation.
//
// docs/PLAN_temperature.md is the design of record; DESIGN.md "Heat" the
// summary; src/sim/heat.h the layout (every constant below is mirrored from
// it and held there by scripts/check_invariants.py `heat`).
//
// WHAT IS STORED. One page per WINDOW chunk that is near something hot: for
// each of its 8^3 blocks (2^3 voxels) the local excess X (u8), its target X*,
// and the block's sources (hottest emitter E, emitting cells n, inertia k).
// Temperature is T = ambient(column, day) + X; ambient is never stored.
//
// WHAT RUNS, after the CA every CA-active tick (pass_table.def "heat"):
//   heatBegin   1 thread: reset list counts, day flip, shift / pend args,
//               the F1 probe
//   heatShift   (before the CA) release every page whose owner left the
//               window and re-target the kept edge; at dawn, lower X by the
//               ambient step so T stays under the ceiling
//   heatPend    (before the CA) last tick's still-relaxing chunks back onto
//               the relax list
//   heatWant    per dirty chunk: list paged chunks, want pages for N27 of an
//               emitting chunk
//   heatArgs    1 thread: indirect args from the counts (recorded 4x)
//   heatAlloc   ONE workgroup: page the wanted chunks in SLOT ORDER (prefix
//               sum), so who gets a page under exhaustion is deterministic
//   heatSrc     per source-list chunk: re-read the voxels into E / n / k;
//               a change queues a target recompute over N27
//   heatTent    3 dispatches (x, y, z): the separable tent filter -- the
//               coverage-weighted mean of emitters within R blocks -> X*
//   heatRelax   per relax-list chunk: X steps toward X* (inertia k); wakes
//               the CA only where the walk can change a cell, else pends the
//               chunk for next tick; frees an all-zero page
//
// DETERMINISM (rule 1). Integer only. Every pass writes only its OWN chunk's
// page; the passes that read a neighbour's page read words the PREVIOUS
// dispatch wrote. Atomics only touch flags (an OR: order-free set semantics),
// list cursors and counters (which list position a chunk got is unobservable:
// every row treats its chunks independently) and the free stack (which PAGE a
// slot got is unobservable: a page is zero when handed out but for the top
// nibble of plane 0, which heatSrc rewrites before anything reads it). The
// allocation RANK is a prefix sum over a slot-ordered bitset; the pend list is
// a bitset too (a set, consumed whole).
//
// COST (rule 2). A settled world records none of this (C_CAACTIVE). An active
// world with no heat: heatWant returns after two loads per dirty chunk, and
// every other row dispatches zero groups.

@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
// heatTent's axis (colorPhase.x of the DYN_CA slice it was recorded with).
@group(0) @binding(5) var<uniform> P : PassParams;
@group(0) @binding(12) var<storage, read> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(50) var<storage, read_write> heatPool : array<u32>;
@group(0) @binding(51) var<storage, read_write> heatMeta : array<atomic<u32>>;
@group(0) @binding(52) var<storage, read> heatParams : array<u32>;

// MIRROR-BEGIN heat
// ---- THE TEMPERATURE LAYER'S SHARED ACCESSORS (src/sim/heat.h) --------------
// ONE block, pasted verbatim into sim_heat.wgsl and sim_step.wgsl and held
// identical by scripts/check_invariants.py `heat`, which also checks every
// constant against heat.h. Not in common.wgsl: an edit there misses the SPIR-V
// cache of every shader, and only these two agree on the layout.
const HEAT_BLOCKS : u32 = 512u;
const HEAT_PAGE_WORDS : u32 = 1536u;
const HEAT_POOL_PAGES : u32 = 5120u;
const HEAT_ENTRY_HAS : u32 = 0x80000000u;
const HEAT_ENTRY_PAGE : u32 = 0x00FFFFFFu;
const HM_MELTS : u32 = 18u;
const HM_ENTRY : u32 = 128u;
const HM_FLAGS : u32 = 32896u;
const HF_EMIT : u32 = 1u;
const HP_MODE : u32 = 0u;
const HP_SNOWLINE_Y : u32 = 2u;
const HP_SNOW_BASE : u32 = 3u;
const HP_SNOW_SWING : u32 = 4u;
const HP_BIOME : u32 = 32u;
const HP_MAT : u32 = 160u;
const HP_MAT_STRIDE : u32 = 8u;
const HP_COL : u32 = 32928u;
const HEAT_THR_BIAS : i32 = 512;
const HEAT_KIND_FREEZE : u32 = 3u;
const HEAT_SCALE_AIR : u32 = 2u;
const HEAT_R2_EMIT_SHIFT : u32 = 12u;
const HEAT_R2_INERTIA_SHIFT : u32 = 20u;
const HEAT_R2_TRANS_SHIFT : u32 = 23u;

// The block (2^3 voxels) of world cell c inside its chunk, 0..511.
fn heatBlockOf(c : vec3<i32>) -> u32 {
  let b = vec3<u32>(c & vec3<i32>(CHUNK_MASK)) >> vec3<u32>(1u);
  return (b.z * 8u + b.y) * 8u + b.x;
}
// The AMBIENT at world cell c, heat units (0 = water freezes): the climate of
// the biome of c's column (heatParams' window column table, filled by the CPU
// from the same seeded map read worldgen uses), or the snowline's at and above
// the snowline; base + swing by day, base - swing by night. Two-level ON
// PURPOSE: it changes only on the tick daylight switches, which is the tick
// SubmitTick already wakes the whole world on (docs/PLAN_temperature.md §5).
fn heatAmbient(c : vec3<i32>) -> i32 {
  var base : i32;
  var swing : i32;
  if (c.y >= bitcast<i32>(heatParams[HP_SNOWLINE_Y])) {
    base = bitcast<i32>(heatParams[HP_SNOW_BASE]);
    swing = bitcast<i32>(heatParams[HP_SNOW_SWING]);
  } else {
    let col = u32(c.x & i32(WORLD_MASK)) + u32(c.z & i32(WORLD_MASK)) * WORLD_N;
    let biome = (heatParams[HP_COL + (col >> 2u)] >> ((col & 3u) * 8u)) & 0xFFu;
    base = bitcast<i32>(heatParams[HP_BIOME + 2u * biome]);
    swing = bitcast<i32>(heatParams[HP_BIOME + 2u * biome + 1u]);
  }
  return select(base - swing, base + swing, isDaytime(T.dayPhase));
}
// The local excess X at world cell c: 0 outside the window (tickets carry no
// heat), in a chunk with no page, or with the layer off.
fn heatX(c : vec3<i32>) -> u32 {
  let wc = worldChunkOf(c);
  if (!chunkInWindow(wc, T.origin)) { return 0u; }
  let e = atomicLoad(&heatMeta[HM_ENTRY + chunkSlotIndex(wc)]);
  if ((e & HEAT_ENTRY_HAS) == 0u) { return 0u; }
  return heatPool[(e & HEAT_ENTRY_PAGE) * HEAT_PAGE_WORDS + heatBlockOf(c)] & 0xFFu;
}
// MIRROR-END heat

const HM_WANT_COUNT : u32 = 0u;
const HM_FREE_TOP : u32 = 1u;
const HM_NEXT_FRESH : u32 = 2u;
const HM_SRC_COUNT : u32 = 3u;
const HM_RECOMP_COUNT : u32 = 4u;
const HM_RELAX_COUNT : u32 = 5u;
const HM_LAST_DAY : u32 = 6u;
const HM_DAY_FLIP : u32 = 7u;
const HM_ORIGIN : u32 = 8u;
const HM_ORIGIN_SET : u32 = 11u;
const HM_PEND_COUNT : u32 = 12u;
const HM_SHIFT_WHY : u32 = 13u;
const HM_PAGES_PEAK : u32 = 16u;
const HM_REFUSED : u32 = 17u;
const HM_SRC_PEAK : u32 = 21u;
const HM_RECOMP_PEAK : u32 = 22u;
const HM_RELAX_PEAK : u32 = 23u;
const HM_RELAX_TICKS : u32 = 24u;
const HM_FREES : u32 = 25u;
const HM_ALLOCS : u32 = 26u;
const HM_RELEASED : u32 = 27u;
const HM_LAST_RELAX : u32 = 28u;
const HM_LAST_RECOMP : u32 = 29u;
const HM_PROBE_X : u32 = 30u;
const HM_PROBE_E : u32 = 31u;
const HEAT_PROBE_TAG_SHIFT : u32 = 17u;
const HM_ARGS : u32 = 32u;
const HM_SUMMARY : u32 = 65664u;
const HM_OWNER : u32 = 98432u;
const HM_WANT : u32 = 131200u;
const HM_PEND : u32 = 132224u;
const HM_SRC_LIST : u32 = 133248u;
const HM_RECOMP_LIST : u32 = 166016u;
const HM_RELAX_LIST : u32 = 198784u;
const HM_STACK : u32 = 231552u;
const HF_SRC : u32 = 2u;
const HF_RECOMP : u32 = 4u;
const HF_RELAX : u32 = 8u;
const HF_NEW : u32 = 16u;
const HS_SOURCE : u32 = 1u;
const HS_NONZERO : u32 = 2u;
const HS_TRIG_ABOVE_SHIFT : u32 = 2u;
const HS_TRIG_BELOW_SHIFT : u32 = 12u;
const HS_TRIG_MASK : u32 = 1023u;
const HEAT_BLOCK_TRANS : u32 = 0x80000000u;
const HEAT_KIND_MELT : u32 = 1u;
const HEAT_KIND_IGNITE : u32 = 2u;
const HEAT_SHIFT_RELEASE : u32 = 1u;
const HEAT_SHIFT_DAWN : u32 = 2u;
const HP_RADIUS : u32 = 1u;
const HP_PROBE : u32 = 5u;
const HP_PROBE_ON : u32 = 8u;
const HP_GAIN : u32 = 9u;
const HEAT_ARG_ALLOC : u32 = 0u;
const HEAT_ARG_SRC : u32 = 1u;
const HEAT_ARG_RECOMP : u32 = 2u;
const HEAT_ARG_RELAX : u32 = 3u;
const HEAT_ARG_SHIFT : u32 = 4u;
const HEAT_ARG_PEND : u32 = 5u;
const HEAT_RADIUS_MAX : u32 = 8u;
// Bit 28 of the dirty word (world.h kDirtyReasonName "heat"): a chunk whose
// local temperature moved this tick. Not in sim_step's FILM_LICENCE: heat
// moving is not liquid progress.
const DIRTY_R_HEAT : u32 = 268435456u;
// Fixed point of the tent filter: emitter sum x64, coverage x4096.
const HEAT_SE_ONE : u32 = 64u;
const HEAT_SF_ONE : u32 = 4096u;

fn heatMode() -> bool { return heatParams[HP_MODE] != 0u; }
fn heatRadius() -> i32 { return i32(clamp(heatParams[HP_RADIUS], 1u, HEAT_RADIUS_MAX)); }
fn heatEntry(slot : u32) -> u32 { return atomicLoad(&heatMeta[HM_ENTRY + slot]); }
fn heatPageBase(e : u32) -> u32 { return (e & HEAT_ENTRY_PAGE) * HEAT_PAGE_WORDS; }
// The window slot of world chunk wc, or SLOT_NONE outside the window. Heat is
// window-only: a ticket chunk and anything past the edge read as cold.
fn heatSlotOf(wc : vec3<i32>) -> u32 {
  if (!chunkInWindow(wc, T.origin)) { return SLOT_NONE; }
  return chunkSlotIndex(wc);
}
fn heatOwnerKey(wc : vec3<i32>) -> u32 {
  let u = vec3<u32>(wc & vec3<i32>(1023));
  return u.x | (u.y << 10u) | (u.z << 20u);
}
// Put `slot` on a work list once: the first setter of the flag appends.
fn heatListAdd(slot : u32, flag : u32, countAt : u32, listAt : u32) {
  let old = atomicOr(&heatMeta[HM_FLAGS + slot], flag);
  if ((old & flag) != 0u) { return; }
  let at = atomicAdd(&heatMeta[countAt], 1u);
  atomicStore(&heatMeta[listAt + at], slot);
}
fn heatWantSlot(slot : u32) {
  let old = atomicOr(&heatMeta[HM_WANT + (slot >> 5u)], 1u << (slot & 31u));
  if ((old & (1u << (slot & 31u))) == 0u) { atomicAdd(&heatMeta[HM_WANT_COUNT], 1u); }
}
fn heatSetArgs(rec : u32, x : u32) {
  let o = HM_ARGS + rec * 4u;
  atomicStore(&heatMeta[o], x);
  atomicStore(&heatMeta[o + 1u], 1u);
  atomicStore(&heatMeta[o + 2u], 1u);
}
// Neighbour k (0..26) of world chunk wc.
fn heatN27(wc : vec3<i32>, k : u32) -> vec3<i32> {
  return wc + vec3<i32>(i32(k % 3u) - 1, i32((k / 3u) % 3u) - 1, i32(k / 9u) - 1);
}

// ============================================================================
// heatBegin: one thread, before the CA.
@compute @workgroup_size(1)
fn heatBegin() {
  atomicStore(&heatMeta[HM_WANT_COUNT], 0u);
  atomicStore(&heatMeta[HM_SRC_COUNT], 0u);
  atomicStore(&heatMeta[HM_RECOMP_COUNT], 0u);
  atomicStore(&heatMeta[HM_RELAX_COUNT], 0u);
  // The day flip: a recompute of every paged chunk the CA has awake (all of
  // them, on the wake-all tick the flip falls on), because X* = coverage x
  // (E - ambient) moves with the ambient.
  let day = select(1u, 2u, isDaytime(T.dayPhase));
  let last = atomicExchange(&heatMeta[HM_LAST_DAY], day);
  let flip = last != day;
  atomicStore(&heatMeta[HM_DAY_FLIP], select(0u, 1u, flip));
  // The window moved (or the layer was switched off with pages out): release
  // what no longer belongs. 128 groups of 256 = every window slot.
  let o = T.origin;
  let known = atomicLoad(&heatMeta[HM_ORIGIN_SET]) != 0u;
  let moved = !known || bitcast<i32>(atomicLoad(&heatMeta[HM_ORIGIN])) != o.x ||
              bitcast<i32>(atomicLoad(&heatMeta[HM_ORIGIN + 1u])) != o.y ||
              bitcast<i32>(atomicLoad(&heatMeta[HM_ORIGIN + 2u])) != o.z;
  let inUse = atomicLoad(&heatMeta[HM_NEXT_FRESH]) - atomicLoad(&heatMeta[HM_FREE_TOP]);
  var why = 0u;
  if (inUse > 0u && (moved || !heatMode())) { why |= HEAT_SHIFT_RELEASE; }
  // DAWN (night -> day, with the layer on and a known previous phase): the
  // ambient steps UP by 2 x swing while every stored X still holds the
  // night's excess, so T = ambient + X would jump past the hottest source in
  // reach (a lava pool's night X of ~226 plus a desert day of 44 is 270 > its
  // 230) until the relaxation caught up -- long enough for embers to
  // heat-ignite wood at a desert dawn. heatShift lowers every kept page's X
  // and X* by the step first, so T is continuous and the ceiling holds.
  if (inUse > 0u && flip && last == 1u && day == 2u && heatMode()) { why |= HEAT_SHIFT_DAWN; }
  atomicStore(&heatMeta[HM_SHIFT_WHY], why);
  heatSetArgs(HEAT_ARG_SHIFT, select(0u, NUM_CHUNKS / 256u, why != 0u));
  atomicStore(&heatMeta[HM_ORIGIN], bitcast<u32>(o.x));
  atomicStore(&heatMeta[HM_ORIGIN + 1u], bitcast<u32>(o.y));
  atomicStore(&heatMeta[HM_ORIGIN + 2u], bitcast<u32>(o.z));
  atomicStore(&heatMeta[HM_ORIGIN_SET], 1u);
  // The pend list (heatRelax's still-relaxing chunks from last tick): four
  // groups of 256 read the 1,024-word bitset, zero groups when it is empty.
  let pend = atomicExchange(&heatMeta[HM_PEND_COUNT], 0u);
  heatSetArgs(HEAT_ARG_PEND, select(0u, (NUM_CHUNKS / 32u) / 256u, pend != 0u && heatMode()));
  // THE F1 PROBE, every CA-active tick: the block under the probe cell as
  // last tick's heatRelax left it, or "no page" -- so the readout follows the
  // player out of a warm chunk instead of keeping its last hot value. The
  // block tag lets the CPU tell this reading from a stale one.
  if (heatParams[HP_PROBE_ON] != 0u) {
    let pc = vec3<i32>(bitcast<i32>(heatParams[HP_PROBE]), bitcast<i32>(heatParams[HP_PROBE + 1u]),
                       bitcast<i32>(heatParams[HP_PROBE + 2u]));
    let tag = (u32(pc.x >> 1) & 31u) | ((u32(pc.y >> 1) & 31u) << 5u) |
              ((u32(pc.z >> 1) & 31u) << 10u);
    var px = 0u;
    var pe = 0u;
    let q = heatSlotOf(worldChunkOf(pc));
    if (q != SLOT_NONE) {
      let pq = heatEntry(q);
      if ((pq & HEAT_ENTRY_HAS) != 0u) {
        let w = heatPool[heatPageBase(pq) + heatBlockOf(pc)];
        px = (w & 0xFFFFu) | 0x10000u;
        pe = w >> 16u;
      }
    }
    atomicStore(&heatMeta[HM_PROBE_X], px | (tag << HEAT_PROBE_TAG_SHIFT));
    atomicStore(&heatMeta[HM_PROBE_E], pe);
  }
}

// The day - night step of the ambient at world cell c (2 x swing of its
// column's climate, or the snowline's): what heatShift's dawn takes off X.
fn heatAmbientStep(c : vec3<i32>) -> i32 {
  if (c.y >= bitcast<i32>(heatParams[HP_SNOWLINE_Y])) {
    return 2 * bitcast<i32>(heatParams[HP_SNOW_SWING]);
  }
  let col = u32(c.x & i32(WORLD_MASK)) + u32(c.z & i32(WORLD_MASK)) * WORLD_N;
  let biome = (heatParams[HP_COL + (col >> 2u)] >> ((col & 3u) * 8u)) & 0xFFu;
  return 2 * bitcast<i32>(heatParams[HP_BIOME + 2u * biome + 1u]);
}

// ============================================================================
// heatShift: one thread per window slot, recorded indirect on heatBegin's
// record (zero groups unless the origin moved, the layer was turned off, or
// it is dawn). RELEASE: a page whose owner is no longer the slot's occupant --
// or every page, with the layer off -- is zeroed and pushed. Heat is
// ephemeral: a chunk that leaves and comes back starts at ambient (PLAN §9).
// A KEPT page on the window's edge after a move is queued for a target
// recompute: the chunks beyond the edge now read as cold, and a target built
// from them would otherwise outlive them (an edge chunk beside a fire that
// has left the window, or burnt out in a ticket, kept its warmth for good).
// DAWN: a kept page's X and X* drop by the block's ambient step (see
// heatBegin), clamped at 0.
@compute @workgroup_size(256)
fn heatShift(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.x;
  if (slot >= NUM_CHUNKS) { return; }
  let e = heatEntry(slot);
  if ((e & HEAT_ENTRY_HAS) == 0u) { return; }
  let why = atomicLoad(&heatMeta[HM_SHIFT_WHY]);
  let base = heatPageBase(e);
  let wc = slotWorldChunk(slot, T.origin);
  let owner = atomicLoad(&heatMeta[HM_OWNER + slot]);
  if ((why & HEAT_SHIFT_RELEASE) != 0u && (!heatMode() || owner != heatOwnerKey(wc))) {
    for (var i = 0u; i < HEAT_PAGE_WORDS; i++) { heatPool[base + i] = 0u; }
    let at = atomicAdd(&heatMeta[HM_FREE_TOP], 1u);
    atomicStore(&heatMeta[HM_STACK + at], e & HEAT_ENTRY_PAGE);
    atomicStore(&heatMeta[HM_ENTRY + slot], 0u);
    atomicStore(&heatMeta[HM_FLAGS + slot], 0u);
    atomicStore(&heatMeta[HM_SUMMARY + slot], 0u);
    atomicAdd(&heatMeta[HM_RELEASED], 1u);
    return;
  }
  if ((why & HEAT_SHIFT_DAWN) != 0u) {
    let corner = wc * i32(CHUNK);
    for (var b = 0u; b < HEAT_BLOCKS; b++) {
      let bp = vec3<i32>(i32(b & 7u), i32((b >> 3u) & 7u), i32(b >> 6u)) * 2;
      let step = heatAmbientStep(corner + bp);
      let w = heatPool[base + b];
      let x = max(i32(w & 0xFFu) - step, 0);
      let xs = max(i32((w >> 8u) & 0xFFu) - step, 0);
      heatPool[base + b] = (w & 0xFFFF0000u) | u32(x) | (u32(xs) << 8u);
    }
  }
  if ((why & HEAT_SHIFT_RELEASE) != 0u) {
    var edge = false;
    for (var k = 0u; k < 27u; k++) {
      if (!chunkInWindow(heatN27(wc, k), T.origin)) { edge = true; }
    }
    if (edge) { heatListAdd(slot, HF_RECOMP, HM_RECOMP_COUNT, HM_RECOMP_LIST); }
  }
}

// ============================================================================
// heatPend: four groups of 256 over the pend bitset, recorded indirect on
// heatBegin's record (zero groups when last tick's heatRelax left nothing
// pending). Every still-paged chunk whose bit is set goes back on the relax
// list, so its X keeps walking to X* this tick WITHOUT the CA running over it
// (heatRelax wakes the CA only where the walk can change a cell). Consumes
// the bitset.
@compute @workgroup_size(256)
fn heatPend(@builtin(global_invocation_id) gid : vec3<u32>) {
  let wi = gid.x;
  if (wi >= NUM_CHUNKS / 32u) { return; }
  var bits = atomicExchange(&heatMeta[HM_PEND + wi], 0u);
  while (bits != 0u) {
    let b = firstTrailingBit(bits);
    bits &= bits - 1u;
    let slot = wi * 32u + b;
    if ((heatEntry(slot) & HEAT_ENTRY_HAS) != 0u) {
      heatListAdd(slot, HF_RELAX, HM_RELAX_COUNT, HM_RELAX_LIST);
    }
  }
}

// ============================================================================
// heatWant: one group per DIRTY chunk (the CA's list), after the CA.
//
// A paged chunk goes on the source list (its voxels may have changed). A chunk
// that holds an emitter -- caMask raised HF_EMIT this tick, or its last source
// scan found one -- wants a page for every unpaged WINDOW chunk of its 3x3x3:
// the tent reaches at most one chunk (heat.h kHeatRadiusMax), and an unpaged
// chunk reads as zero, which would break the separable passes' chain. Every
// chunk wanted here is in N26 of a dirty chunk, which is what lets heatRelax
// mark it without escaping the CPU page-table mirror's dirty bound.
var<workgroup> wgWantNeed : u32;

@compute @workgroup_size(32)
fn heatWant(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  let slot = dirtyList[wg.x];
  if (li == 0u) {
    wgWantNeed = 0u;
    if (slot < NUM_CHUNKS) {
      let f = atomicAnd(&heatMeta[HM_FLAGS + slot], ~HF_EMIT);
      if (heatMode()) {
        let paged = (heatEntry(slot) & HEAT_ENTRY_HAS) != 0u;
        if (paged) { heatListAdd(slot, HF_SRC, HM_SRC_COUNT, HM_SRC_LIST); }
        let src = paged && (atomicLoad(&heatMeta[HM_SUMMARY + slot]) & HS_SOURCE) != 0u;
        if ((f & HF_EMIT) != 0u || src) { wgWantNeed = 1u; }
      }
    }
  }
  workgroupBarrier();
  let need = workgroupUniformLoad(&wgWantNeed);
  if (need == 0u || li >= 27u) { return; }
  let q = heatSlotOf(heatN27(slotWorldChunk(slot, T.origin), li));
  if (q == SLOT_NONE) { return; }
  if ((heatEntry(q) & HEAT_ENTRY_HAS) == 0u) { heatWantSlot(q); }
}

// ============================================================================
// heatArgs: one thread. The indirect args of every list from its count,
// recorded after each phase that grows a list (alloc, src, recomp, relax).
@compute @workgroup_size(1)
fn heatArgs() {
  heatSetArgs(HEAT_ARG_ALLOC, select(0u, 1u, atomicLoad(&heatMeta[HM_WANT_COUNT]) != 0u));
  let ns = atomicLoad(&heatMeta[HM_SRC_COUNT]);
  let nr = atomicLoad(&heatMeta[HM_RECOMP_COUNT]);
  let nx = atomicLoad(&heatMeta[HM_RELAX_COUNT]);
  heatSetArgs(HEAT_ARG_SRC, ns);
  heatSetArgs(HEAT_ARG_RECOMP, nr);
  heatSetArgs(HEAT_ARG_RELAX, nx);
  atomicMax(&heatMeta[HM_SRC_PEAK], ns);
  atomicMax(&heatMeta[HM_RECOMP_PEAK], nr);
  atomicMax(&heatMeta[HM_RELAX_PEAK], nx);
  atomicStore(&heatMeta[HM_LAST_RELAX], nx);
  atomicStore(&heatMeta[HM_LAST_RECOMP], nr);
}

// ============================================================================
// heatAlloc: ONE workgroup. Rank every wanted slot by SLOT ORDER (a prefix sum
// over the bitset) and give rank r the r-th page available: the free stack
// from its top, then never-used pages from nextFresh. A rank past the pool is
// REFUSED and counted (heat-bound fails on any): the chunk stays a cold gap
// and asks again the next tick it qualifies. Which slots win is therefore a
// pure function of the want SET -- never of which thread got there first.
const ALLOC_WG : u32 = 256u;
const WANT_WORDS : u32 = NUM_CHUNKS / 32u;
const WANT_PER_THREAD : u32 = WANT_WORDS / ALLOC_WG;
var<workgroup> wgScan : array<u32, 256>;

@compute @workgroup_size(256)
fn heatAlloc(@builtin(local_invocation_index) li : u32) {
  let w0 = li * WANT_PER_THREAD;
  var cnt = 0u;
  for (var k = 0u; k < WANT_PER_THREAD; k++) {
    cnt += countOneBits(atomicLoad(&heatMeta[HM_WANT + w0 + k]));
  }
  wgScan[li] = cnt;
  workgroupBarrier();
  // Hillis-Steele inclusive scan; every step is a pure function of the counts.
  for (var off = 1u; off < ALLOC_WG; off <<= 1u) {
    var v = wgScan[li];
    if (li >= off) { v += wgScan[li - off]; }
    workgroupBarrier();
    wgScan[li] = v;
    workgroupBarrier();
  }
  let total = wgScan[ALLOC_WG - 1u];
  var rank = wgScan[li] - cnt;
  let top = atomicLoad(&heatMeta[HM_FREE_TOP]);
  let fresh = atomicLoad(&heatMeta[HM_NEXT_FRESH]);
  let avail = top + (HEAT_POOL_PAGES - fresh);
  for (var k = 0u; k < WANT_PER_THREAD; k++) {
    var bits = atomicLoad(&heatMeta[HM_WANT + w0 + k]);
    atomicStore(&heatMeta[HM_WANT + w0 + k], 0u);
    while (bits != 0u) {
      let b = firstTrailingBit(bits);
      bits &= bits - 1u;
      let slot = (w0 + k) * 32u + b;
      let r = rank;
      rank += 1u;
      if (r >= avail) { continue; }
      var page : u32;
      if (r < top) { page = atomicLoad(&heatMeta[HM_STACK + top - 1u - r]); }
      else { page = fresh + (r - top); }
      atomicStore(&heatMeta[HM_ENTRY + slot], HEAT_ENTRY_HAS | page);
      atomicStore(&heatMeta[HM_OWNER + slot], heatOwnerKey(slotWorldChunk(slot, T.origin)));
      atomicStore(&heatMeta[HM_SUMMARY + slot], 0u);
      atomicOr(&heatMeta[HM_FLAGS + slot], HF_NEW);
      heatListAdd(slot, HF_SRC, HM_SRC_COUNT, HM_SRC_LIST);
    }
  }
  workgroupBarrier();
  if (li == 0u) {
    let taken = min(total, avail);
    let fromStack = min(taken, top);
    atomicStore(&heatMeta[HM_FREE_TOP], top - fromStack);
    atomicStore(&heatMeta[HM_NEXT_FRESH], fresh + (taken - fromStack));
    atomicAdd(&heatMeta[HM_REFUSED], total - taken);
    atomicAdd(&heatMeta[HM_ALLOCS], taken);
    atomicMax(&heatMeta[HM_PAGES_PEAK], fresh + (taken - fromStack) - (top - fromStack));
  }
}

// ============================================================================
// heatSrc: one group per source-list chunk. Re-read the chunk's 4,096 voxels
// into per-block sources: E the hottest `thermal.emit` among the block's 8
// cells, n how many cells emit, k the slowest `thermal.inertia` (air 1), and
// whether any cell carries a thermal transition (HEAT_BLOCK_TRANS). A block
// whose sources changed -- or a page that is new -- queues a target
// recompute of the WHOLE 3x3x3 around it (a source reaches at most one chunk);
// the day flip queues only the chunk itself (X* moves with its own ambient).
// The chunk's summary gets its TRIGGERS: the lowest melt / ignite threshold
// and the highest freeze threshold among its cells (heat.h kHsTrig*), which
// is what heatRelax compares a moving block's temperature range against.
var<workgroup> wgSrcChanged : atomic<u32>;
var<workgroup> wgSrcAny : atomic<u32>;
var<workgroup> wgSrcAbove : atomic<u32>;
var<workgroup> wgSrcBelow : atomic<u32>;
var<workgroup> wgSrcMode : u32;

@compute @workgroup_size(128)
fn heatSrc(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let slot = atomicLoad(&heatMeta[HM_SRC_LIST + wg.x]);
  if (li == 0u) {
    atomicStore(&wgSrcChanged, 0u);
    atomicStore(&wgSrcAny, 0u);
    atomicStore(&wgSrcAbove, HS_TRIG_MASK);
    atomicStore(&wgSrcBelow, 0u);
  }
  workgroupBarrier();
  let e = heatEntry(slot);
  let base = heatPageBase(e);
  let wc = slotWorldChunk(slot, T.origin);
  let corner = wc * i32(CHUNK);
  var above = HS_TRIG_MASK;
  var below = 0u;
  for (var i = 0u; i < 4u; i++) {
    let b = li * 4u + i;
    let bp = vec3<i32>(i32(b & 7u), i32((b >> 3u) & 7u), i32(b >> 6u)) * 2;
    var eMax = 0u;
    var n = 0u;
    var k = 0u;
    var trans = 0u;
    for (var j = 0u; j < 8u; j++) {
      let c = corner + bp + vec3<i32>(i32(j & 1u), i32((j >> 1u) & 1u), i32(j >> 2u));
      let mat = voxMat(voxWordAt(c));
      var kk = 1u;
      if (mat != MAT_AIR) {
        let r2 = materials[mat]._r2;
        let em = (r2 >> HEAT_R2_EMIT_SHIFT) & 0xFFu;
        if (em != 0u) { n += 1u; eMax = max(eMax, em); }
        kk = (r2 >> HEAT_R2_INERTIA_SHIFT) & 7u;
        let nt = min((r2 >> HEAT_R2_TRANS_SHIFT) & 3u, 2u);
        for (var t = 0u; t < nt; t++) {
          let w0 = heatParams[HP_MAT + mat * HP_MAT_STRIDE + 3u * t];
          // The threshold is stored + HEAT_THR_BIAS already (heat.cpp
          // PackHeatMaterial), which is the summary's encoding.
          let thr = w0 & HS_TRIG_MASK;
          if (((w0 >> 20u) & 3u) == HEAT_KIND_FREEZE) { below = max(below, thr); }
          else { above = min(above, thr); }
          trans = 1u;
        }
      }
      k = max(k, kk);
    }
    let hi = eMax | (n << 8u) | (k << 12u) | (trans << 15u);
    let old = heatPool[base + b];
    if ((old >> 16u) != hi) {
      heatPool[base + b] = (old & 0xFFFFu) | (hi << 16u);
      atomicOr(&wgSrcChanged, 1u);
    }
    if (eMax != 0u) { atomicOr(&wgSrcAny, 1u); }
  }
  if (above != HS_TRIG_MASK) { atomicMin(&wgSrcAbove, above); }
  if (below != 0u) { atomicMax(&wgSrcBelow, below); }
  workgroupBarrier();
  if (li == 0u) {
    let f = atomicAnd(&heatMeta[HM_FLAGS + slot], ~HF_NEW);
    let any = atomicLoad(&wgSrcAny);
    let sum = atomicLoad(&heatMeta[HM_SUMMARY + slot]);
    var trig = atomicLoad(&wgSrcAbove);
    if (trig == HS_TRIG_MASK) { trig = 0u; }   // no melt / ignite here: none
    atomicStore(&heatMeta[HM_SUMMARY + slot],
                (sum & HS_NONZERO) | select(0u, HS_SOURCE, any != 0u) |
                (trig << HS_TRIG_ABOVE_SHIFT) | (atomicLoad(&wgSrcBelow) << HS_TRIG_BELOW_SHIFT));
    var mode = 0u;
    if (atomicLoad(&wgSrcChanged) != 0u || (f & HF_NEW) != 0u) { mode = 2u; }
    else if (atomicLoad(&heatMeta[HM_DAY_FLIP]) != 0u) { mode = 1u; }
    wgSrcMode = mode;
    heatListAdd(slot, HF_RELAX, HM_RELAX_COUNT, HM_RELAX_LIST);
  }
  workgroupBarrier();
  let mode = workgroupUniformLoad(&wgSrcMode);
  if (mode == 1u && li == 0u) {
    heatListAdd(slot, HF_RECOMP, HM_RECOMP_COUNT, HM_RECOMP_LIST);
  }
  if (mode == 2u && li < 27u) {
    let q = heatSlotOf(heatN27(wc, li));
    if (q != SLOT_NONE && (heatEntry(q) & HEAT_ENTRY_HAS) != 0u) {
      heatListAdd(q, HF_RECOMP, HM_RECOMP_COUNT, HM_RECOMP_LIST);
    }
  }
}

// ============================================================================
// heatTent: one group per recompute-list chunk, THREE dispatches (axis =
// colorPhase.x: 0 x, 1 y, 2 z) with the recorder's barrier between them -- a
// pass reads its NEIGHBOURS' output of the previous pass, so two axes can never
// share a dispatch.
//
// The filter is a separable TENT of half-width R blocks, weights R+1-|d|,
// normalised to 1 per axis: per block it carries the COVERAGE-WEIGHTED
// emitter sum sE (each emitting cell counts E x 1/8 of its block) and the
// coverage sf (1/8 per emitting cell), so after three passes
//   sE = sum over sources of w * E * n/8,   sf = sum of w * n/8,   sum(w) = 1.
// sE / sf is the coverage-weighted MEAN temperature of the emitters in reach;
// sf is how much of the surroundings they fill. The target mixes that mean
// with the ambient by an effective coverage that saturates:
//   c = min(1, G x sf)      (G = sim.heatGain)
//   T* = A + c x (sE/sf - A),   X* = T* - A   (clamped >= 0).
// T* is a CONVEX combination of the ambient and a mean of emitters, so it can
// never exceed the hottest source in reach or the ambient -- the ceiling, by
// construction. More sources, more coverage: several sources ADD until the
// coverage saturates. Beyond R blocks a source contributes exactly nothing --
// the radius, by construction. G is what lets a big source (a lava pool's
// face, a wall of flame) reach its own temperature a few blocks out while a
// single burning block barely warms the next one: a lone block covers ~2% of
// its neighbour's surroundings, a pool's face ~15-40%.
//
// Plane 0 (x input) holds E and n; planes 1 and 2 hold the x and y passes'
// sE (x64, bits 0..15) and sf (x4096, bits 16..31). A neighbour chunk outside
// the window or without a page reads as zero, which is exact: every chunk
// within reach of a source is paged (heatWant), so an unpaged chunk carries
// no source and no partial sum.
@compute @workgroup_size(128)
fn heatTent(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  let slot = atomicLoad(&heatMeta[HM_RECOMP_LIST + wg.x]);
  let axis = P.colorPhase.x;
  let e = heatEntry(slot);
  let base = heatPageBase(e);
  let wc = slotWorldChunk(slot, T.origin);
  let R = heatRadius();
  let norm = u32((R + 1) * (R + 1));
  var dir = vec3<i32>(0);
  dir[axis] = 1;
  // The two neighbour pages along this axis (-1, +1).
  let qm = heatSlotOf(wc - dir);
  let qp = heatSlotOf(wc + dir);
  var em = 0u;
  var ep = 0u;
  if (qm != SLOT_NONE) { em = heatEntry(qm); }
  if (qp != SLOT_NONE) { ep = heatEntry(qp); }
  let inPlane = axis * HEAT_BLOCKS;
  for (var i = 0u; i < 4u; i++) {
    let b = li * 4u + i;
    let bp = vec3<i32>(i32(b & 7u), i32((b >> 3u) & 7u), i32(b >> 6u));
    var sE = 0u;
    var sF = 0u;
    for (var d = -R; d <= R; d++) {
      let q = bp + dir * d;
      let s = q[axis];
      var pe = e;
      if (s < 0) { pe = em; } else if (s > 7) { pe = ep; }
      if ((pe & HEAT_ENTRY_HAS) == 0u) { continue; }
      let ql = q - dir * (select(0, -8, s < 0) + select(0, 8, s > 7));
      let qb = u32((ql.z * 8 + ql.y) * 8 + ql.x);
      let w = heatPool[heatPageBase(pe) + inPlane + qb];
      let wt = u32(R + 1 - abs(d));
      if (axis == 0u) {
        let ee = (w >> 16u) & 0xFFu;
        let nn = (w >> 24u) & 0xFu;
        sE += wt * ee * nn * (HEAT_SE_ONE / 8u);
        sF += wt * nn * (HEAT_SF_ONE / 8u);
      } else {
        sE += wt * (w & 0xFFFFu);
        sF += wt * (w >> 16u);
      }
    }
    sE = min(sE / norm, 0xFFFFu);
    sF = min(sF / norm, HEAT_SF_ONE);
    if (axis < 2u) {
      heatPool[base + (axis + 1u) * HEAT_BLOCKS + b] = sE | (sF << 16u);
    } else {
      // X* = min(1, G sf) x (mean E - A), clamped to 0..255. sE is x64 and sF
      // x4096, so sE * 64 / sF is the mean emitter temperature.
      let c = wc * i32(CHUNK) + bp * 2;
      let a = heatAmbient(c);
      var xs = 0;
      if (sF != 0u) {
        let gap = (i32(sE) * i32(HEAT_SF_ONE / HEAT_SE_ONE)) / i32(sF) - a;
        let eff = i32(min(sF * clamp(heatParams[HP_GAIN], 1u, 64u), HEAT_SF_ONE));
        xs = clamp(gap * eff / i32(HEAT_SF_ONE), 0, 255);
      }
      let old = heatPool[base + b];
      heatPool[base + b] = (old & 0xFFFF00FFu) | (u32(xs) << 8u);
    }
  }
  if (axis == 2u && li == 0u) {
    heatListAdd(slot, HF_RELAX, HM_RELAX_COUNT, HM_RELAX_LIST);
  }
}

// ============================================================================
// heatRelax: one group per relax-list chunk (every chunk whose sources were
// re-read, whose target was recomputed, or that was still relaxing last tick
// -- the pend list). X steps toward X* by max(1, |X* - X| >> k): gradual (k is
// the block's inertia -- water slow, air fast), EXACT at the end (the step is
// never 0 while they differ), and never past X* (the step is at most the
// gap). So X never exceeds the largest target the block has had, and the
// target never exceeds the hottest source in reach: the ceiling holds for the
// ACTUAL temperature, not only the target.
//
// WAKING THE CA. A moving field matters to the CA only where it can change a
// cell: a block holding a transition-bearing cell (HEAT_BLOCK_TRANS) whose
// temperature range [T, T*] -- everything the walk can still visit -- reaches
// the chunk's melt / ignite trigger or its freeze trigger (the summary, from
// heatSrc). Only such a chunk is marked dirty (DIRTY_R_HEAT) so the CA
// re-reads it next tick. Every other moving chunk -- the halo of air, stone
// and smoke round a fire, foliage warming to 60 beside flames that cannot
// light it -- goes on the PEND list instead: next tick heatPend puts it back
// on this list and it keeps walking WITHOUT the CA running over it. The heat
// never stops short of its target, so a stored X never outlives what made it.
//
// The mark is allowed only if some chunk of its 3x3x3 is dirty this tick: the
// CPU's page-table mirror bounds next tick's dirty set by the one-ring of this
// tick's (docs/PLAN_page_table.md §3.2), and a mark outside it would run the
// CA beside unmaterialised pages. A chunk that needs the CA but may not mark
// stays pending (its field still walks); the CA sees it the next time
// anything wakes the neighbourhood -- a lag, never a phantom.
//
// A chunk whose page is entirely zero (no source, no excess, no partial sum)
// and with no SOURCE in its 3x3x3 is freed (the page is already zero, so it
// goes straight onto the stack). The N27 test is what keeps a source's
// neighbours paged even where its reach ends inside them, or they would be
// freed and re-wanted every tick.
var<workgroup> wgRelaxMoved : atomic<u32>;
var<workgroup> wgRelaxNonzero : atomic<u32>;
var<workgroup> wgRelaxKeep : atomic<u32>;
var<workgroup> wgRelaxWake : atomic<u32>;

// The coldest and hottest ambient over the four columns and two rows of the
// block at chunk-local block corner c (a block can straddle a biome border or
// the snowline; the CA reads the ambient per cell).
fn heatBlockAmbientRange(c : vec3<i32>) -> vec2<i32> {
  var lo = 1 << 20;
  var hi = -(1 << 20);
  for (var j = 0u; j < 8u; j++) {
    let a = heatAmbient(c + vec3<i32>(i32(j & 1u), i32((j >> 1u) & 1u), i32(j >> 2u)));
    lo = min(lo, a);
    hi = max(hi, a);
  }
  return vec2<i32>(lo, hi);
}

@compute @workgroup_size(128)
fn heatRelax(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let slot = atomicLoad(&heatMeta[HM_RELAX_LIST + wg.x]);
  if (li == 0u) {
    atomicStore(&wgRelaxMoved, 0u);
    atomicStore(&wgRelaxNonzero, 0u);
    atomicStore(&wgRelaxKeep, 0u);
    atomicStore(&wgRelaxWake, 0u);
  }
  workgroupBarrier();
  let e = heatEntry(slot);
  let base = heatPageBase(e);
  let wc = slotWorldChunk(slot, T.origin);
  let sum = atomicLoad(&heatMeta[HM_SUMMARY + slot]);
  let trigAbove = (sum >> HS_TRIG_ABOVE_SHIFT) & HS_TRIG_MASK;
  let trigBelow = (sum >> HS_TRIG_BELOW_SHIFT) & HS_TRIG_MASK;
  for (var i = 0u; i < 4u; i++) {
    let b = li * 4u + i;
    let w = heatPool[base + b];
    let x = i32(w & 0xFFu);
    let xs = i32((w >> 8u) & 0xFFu);
    let k = (w >> 28u) & 7u;
    var nx = x;
    if (x != xs) {
      let d = xs - x;
      let step = max(1, abs(d) >> k);
      nx = x + select(-step, step, d > 0);
      heatPool[base + b] = (w & 0xFFFFFF00u) | u32(nx);
      atomicOr(&wgRelaxMoved, 1u);
      // Can this walk change a cell? Only a block with a transition-bearing
      // cell, and only if [T, T*] reaches a trigger (both stored + bias).
      if ((w & HEAT_BLOCK_TRANS) != 0u && (trigAbove != 0u || trigBelow != 0u)) {
        let bp = vec3<i32>(i32(b & 7u), i32((b >> 3u) & 7u), i32(b >> 6u)) * 2;
        let ar = heatBlockAmbientRange(wc * i32(CHUNK) + bp);
        let tHi = ar.y + max(nx, xs) + HEAT_THR_BIAS;
        let tLo = ar.x + min(nx, xs) + HEAT_THR_BIAS;
        if ((trigAbove != 0u && tHi > i32(trigAbove)) || (trigBelow != 0u && tLo < i32(trigBelow))) {
          atomicOr(&wgRelaxWake, 1u);
        }
      }
    }
    // Every byte but X (rewritten above) and the top nibble (inertia and the
    // transition bit: set on every block heatSrc has scanned, so they say
    // nothing about heat): X*, E and n.
    if ((w & 0x0FFFFF00u) != 0u || nx != 0 ||
        heatPool[base + HEAT_BLOCKS + b] != 0u || heatPool[base + 2u * HEAT_BLOCKS + b] != 0u) {
      atomicOr(&wgRelaxNonzero, 1u);
    }
  }
  // N27: is any neighbour dirty this tick (may we mark?) / holding a source
  // (may we free?).
  if (li < 27u) {
    let q = heatSlotOf(heatN27(wc, li));
    if (q != SLOT_NONE) {
      var bits = 0u;
      if (dirtyIn[q] != 0u) { bits |= 1u; }
      if ((atomicLoad(&heatMeta[HM_SUMMARY + q]) & HS_SOURCE) != 0u) { bits |= 2u; }
      if (bits != 0u) { atomicOr(&wgRelaxKeep, bits); }
    }
  }
  workgroupBarrier();
  if (li != 0u) { return; }
  atomicAnd(&heatMeta[HM_FLAGS + slot], ~(HF_SRC | HF_RECOMP | HF_RELAX));
  let moved = atomicLoad(&wgRelaxMoved) != 0u;
  let nonzero = atomicLoad(&wgRelaxNonzero) != 0u;
  let keep = atomicLoad(&wgRelaxKeep);
  let marked = moved && atomicLoad(&wgRelaxWake) != 0u && (keep & 1u) != 0u;
  if (marked) {
    atomicOr(&dirtyOut[slot], DIRTY_R_HEAT);
  }
  if (moved) { atomicAdd(&heatMeta[HM_RELAX_TICKS], 1u); }
  if (!nonzero && (keep & 2u) == 0u) {
    // Free: the page is all zero, so it goes back as it is.
    let at = atomicAdd(&heatMeta[HM_FREE_TOP], 1u);
    atomicStore(&heatMeta[HM_STACK + at], e & HEAT_ENTRY_PAGE);
    atomicStore(&heatMeta[HM_ENTRY + slot], 0u);
    atomicStore(&heatMeta[HM_SUMMARY + slot], 0u);
    atomicStore(&heatMeta[HM_FLAGS + slot], 0u);
    atomicAdd(&heatMeta[HM_FREES], 1u);
    return;
  }
  atomicStore(&heatMeta[HM_SUMMARY + slot],
              (sum & ~HS_NONZERO) | select(0u, HS_NONZERO, nonzero));
  // Still walking and not handed to the CA: walk again next tick, alone. (A
  // marked chunk is dirty next tick, so heatWant lists it anyway.)
  if (moved && !marked) {
    let bit = 1u << (slot & 31u);
    let old = atomicOr(&heatMeta[HM_PEND + (slot >> 5u)], bit);
    if ((old & bit) == 0u) { atomicAdd(&heatMeta[HM_PEND_COUNT], 1u); }
  }
}
