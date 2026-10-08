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
//               ambient step so T stays under the ceiling; when a live
//               knob moved (the knob epoch), re-target every kept page
//   heatPend    (before the CA) last tick's still-relaxing chunks back onto
//               the relax list
//   heatWant    per dirty chunk: list paged chunks, want pages for N27 of an
//               emitting chunk
//   heatArgs    1 thread: indirect args from the counts (recorded 4x)
//   heatAlloc   ONE workgroup: page the wanted chunks in SLOT ORDER (prefix
//               sum), so who gets a page under exhaustion is deterministic
//   (caMask)    sim_step.wgsl, substep 0, BEFORE the CA: a paged dirty
//               chunk's E / n / k / triggers come from the voxels caMask
//               already reads (the source fold); a change marks N27 DUE
//   heatSrc     per NEW page (heatAlloc's): read its voxels into E / n / k;
//               marks N27 due -- the only chunks caMask did not read
//   heatDue     one thread a slot: due chunks whose PHASE is this tick
//               (world chunk x mod sim.heatTentEvery) -> the recompute list
//   heatTent    3 dispatches (x, z, y): the separable tent filter -- the
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
// heat.h kHmDueLeft: due bits heatDue left set this tick (off their phase).
// With HM_PEND_COUNT it is the CPU's "the field is still moving" input to
// the settled-tick proof (Simulation::NoteHeatPending).
const HM_DUE_LEFT : u32 = 15u;
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
const HM_DRAFT_HEAT_CLOCK : u32 = 56u;
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
const HF_URGENT : u32 = 32u;
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
const HEAT_SHIFT_RETARGET : u32 = 4u;
const HM_KNOB_EPOCH : u32 = 14u;
const HP_KNOB_EPOCH : u32 = 13u;
const HP_RADIUS : u32 = 1u;
const HP_PROBE : u32 = 5u;
const HP_PROBE_ON : u32 = 8u;
const HP_GAIN : u32 = 9u;
const HP_UP_GAIN : u32 = 10u;
const HP_SIDE_GAIN : u32 = 11u;
const HP_DOWN_GAIN : u32 = 12u;
const HEAT_GAIN_ONE : u32 = 16u;
const HEAT_ARG_ALLOC : u32 = 0u;
const HEAT_ARG_SRC : u32 = 1u;
const HEAT_ARG_RECOMP : u32 = 2u;
const HEAT_ARG_RELAX : u32 = 3u;
const HEAT_ARG_SHIFT : u32 = 4u;
const HEAT_ARG_PEND : u32 = 5u;
const HEAT_RADIUS_MAX : u32 = 8u;
const HM_DUE : u32 = 236672u;
const HP_TENT_EVERY : u32 = 14u;
const HEAT_TENT_EVERY_MAX : u32 = 8u;
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
// Mark `slot` DUE for a target recompute (heat.h kHmDue): heatDue moves it
// onto the recompute list on its phase tick. An OR -- order-free.
fn heatDueSet(slot : u32) {
  atomicOr(&heatMeta[HM_DUE + (slot >> 5u)], 1u << (slot & 31u));
}
// Due AND URGENT (heat.h kHfUrgent): heatDue recomputes it THIS tick as well
// as on its phase, if its summary holds a trigger. Three causes -- a page
// handed out this tick, a chunk whose FIRST emitter appeared (its summary's
// source bit went 0 -> 1, sim_step.wgsl camask), the day flip -- because each
// is a target going from nothing to something, and a chunk can mark itself for
// the CA only while a chunk of its 3x3x3 is dirty (heatRelax): a target three
// ticks late can miss the dirty window of the edit that caused it and leave
// snow beside fresh lava unmelted until something else wakes it (heat-melt
// did exactly that). Only a chunk with a trigger can lose a wake that way, so
// only such a chunk jumps the queue; the air, smoke and stone round a fire wait
// for their phase, and so does a flicker of a standing fire. The decision is
// heatDue's because the summary is stable there (camask and heatSrc are still
// writing summaries while they raise the flag).
fn heatDueUrgent(slot : u32) {
  heatDueSet(slot);
  atomicOr(&heatMeta[HM_FLAGS + slot], HF_URGENT);
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
  atomicStore(&heatMeta[HM_DUE_LEFT], 0u);
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
  // A LIVE KNOB MOVED (the F1 Temperature sliders: radius, gain, the
  // direction gains, the snowline): every kept page re-targets. A target is
  // rebuilt only when its sources change, so without this a settled field
  // would keep the old knobs' heat until something near it moved. The CPU
  // bumps the epoch on the tick it uploads the change and wakes the world on
  // that tick, so this runs, and every chunk may mark for the CA.
  let epoch = heatParams[HP_KNOB_EPOCH];
  let lastEpoch = atomicExchange(&heatMeta[HM_KNOB_EPOCH], epoch);
  if (inUse > 0u && lastEpoch != epoch && heatMode()) { why |= HEAT_SHIFT_RETARGET; }
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
// record (zero groups unless the origin moved, the layer was turned off, it
// is dawn, or a live knob moved). RELEASE: a page whose owner is no longer the slot's occupant --
// or every page, with the layer off -- is zeroed and pushed. Heat is
// ephemeral: a chunk that leaves and comes back starts at ambient (PLAN §9).
// A KEPT page on the window's edge after a move is queued for a target
// recompute: the chunks beyond the edge now read as cold, and a target built
// from them would otherwise outlive them (an edge chunk beside a fire that
// has left the window, or burnt out in a ticket, kept its warmth for good).
// DAWN: a kept page's X and X* drop by the block's ambient step (see
// heatBegin), clamped at 0. RETARGET: every kept page is queued for a target
// recompute (one pass over the live pages per knob change, nothing after).
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
  // RETARGET (a live knob moved): every kept page, once, THIS tick -- not
  // through the due set's cadence (heat-live holds a re-target to 2 ticks).
  // The x / z / y passes read only paged neighbours, all of which are on the
  // list too, so the new targets are exactly what a fresh field under the new
  // knobs would hold. The window EDGE below goes through the due set:
  // x-phased, so exact on its phase tick (heatDue).
  if ((why & HEAT_SHIFT_RETARGET) != 0u) {
    heatListAdd(slot, HF_RECOMP, HM_RECOMP_COUNT, HM_RECOMP_LIST);
    return;
  }
  if ((why & HEAT_SHIFT_RELEASE) != 0u) {
    var edge = false;
    for (var k = 0u; k < 27u; k++) {
      if (!chunkInWindow(heatN27(wc, k), T.origin)) { edge = true; }
    }
    if (edge) { heatDueSet(slot); }
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
// A paged chunk's sources were already re-read by caMask (sim_step.wgsl, the
// source fold, substep 0) -- before the CA, so a change the CA makes this tick
// is read next tick, when the chunk it wrote is dirty again. A chunk that
// holds an emitter -- caMask raised HF_EMIT this tick, or its last source
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
// heatSrc: one group per source-list chunk -- since the source fold
// (2026-10-06) only the pages heatAlloc handed out THIS tick: caMask reads the
// sources of every paged DIRTY chunk (sim_step.wgsl camask) and a fresh page's
// chunk may not be dirty, so nobody else has read it. Zero groups in a steady
// fire. Re-read the chunk's 4,096 voxels into per-block sources (camask's
// fold computes exactly these words; keep the two in step): E the hottest `thermal.emit` among the block's 8
// cells, n how many cells emit, k the slowest `thermal.inertia` (air 1), and
// whether any cell carries a thermal transition (HEAT_BLOCK_TRANS). A block
// whose sources changed -- or a page that is new -- marks the WHOLE 3x3x3
// around it DUE (a source reaches at most one chunk); the day flip marks only
// the chunk itself (X* moves with its own ambient). heatDue turns due into
// the recompute list on each chunk's phase.
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
  // Every chunk here is a page handed out this tick (mode 2): URGENT.
  if (mode == 1u && li == 0u) { heatDueUrgent(slot); }
  if (mode == 2u && li < 27u) {
    let q = heatSlotOf(heatN27(wc, li));
    if (q != SLOT_NONE && (heatEntry(q) & HEAT_ENTRY_HAS) != 0u) { heatDueUrgent(q); }
  }
}

// ============================================================================
// heatDue: over the due bitset (heat.h kHmDue), after the source passes and
// before heatTent. A due chunk whose PHASE is this tick --
// (world chunk x) mod sim.heatTentEvery == tick mod sim.heatTentEvery -- goes
// on the recompute list and its bit clears; any other keeps its bit for its
// phase. A slot with no page any more (freed, released) just clears.
//
// WHY THE PHASE IS X ALONE, AND WHY THAT IS EXACT. heatTent's x pass reads the
// x neighbours' plane 0 (the SOURCES, never deferred); its z pass reads the z
// neighbours' plane 1 and its y pass the y neighbours' plane 2 -- chunks with
// the chunk's own x, so its own phase. A partial sum a recompute reads is
// therefore either rewritten earlier in the SAME tick (that neighbour was due
// too: every source change marks its whole 3x3x3, which holds every chunk
// whose planes read that source) or not stale at all. So every target is
// exactly the every-tick target, built from sources at most
// sim.heatTentEvery - 1 ticks old -- and once the sources stop moving, the
// field is the every-tick field after one cadence. (A per-chunk hash phase
// would read a neighbour's partial sum from before that neighbour's own
// recompute, and could keep a gone source's heat for good.)
//
// The relaxation is what hides the lag: X walks to X* by |gap| >> k a tick
// (k >= 1, solids 3), so a target a few ticks late moves T by a few percent of
// the gap. What is NOT deferred: the sources, the summary's triggers and the
// CA wake (caMask / heatSrc / heatRelax every tick), a live-knob re-target
// (heatShift puts every page on the list directly) and an URGENT chunk with a
// trigger (heatDueUrgent: a new page, a source appearing, the day flip). An
// urgent recompute off its phase keeps the due bit, so the phase tick
// recomputes it again from same-tick partial sums: it may read a neighbour's
// stale partial for <= N - 1 ticks, and is exact from its phase on.
//
// ONE THREAD PER SLOT (128 groups of 256): a set bit's list append is two
// dependent atomics, and a thread walking a word's 32 bits serialised them
// (one workgroup, measured 87 us/frame on --perf village-fire).
@compute @workgroup_size(256)
fn heatDue(@builtin(global_invocation_id) gid : vec3<u32>,
           @builtin(num_workgroups) nwg : vec3<u32>) {
  let n = clamp(heatParams[HP_TENT_EVERY], 1u, HEAT_TENT_EVERY_MAX);
  let ph = T.tick % n;
  for (var slot = gid.x; slot < NUM_CHUNKS; slot += nwg.x * 256u) {
    let bit = 1u << (slot & 31u);
    if ((atomicLoad(&heatMeta[HM_DUE + (slot >> 5u)]) & bit) == 0u) { continue; }
    if ((heatEntry(slot) & HEAT_ENTRY_HAS) != 0u) {
      let f = atomicAnd(&heatMeta[HM_FLAGS + slot], ~HF_URGENT);
      let x = slotWorldChunk(slot, T.origin).x;
      let onPhase = u32(((x % i32(n)) + i32(n)) % i32(n)) == ph;
      let trig = (atomicLoad(&heatMeta[HM_SUMMARY + slot]) >> HS_TRIG_ABOVE_SHIFT) &
                 ((HS_TRIG_MASK << (HS_TRIG_BELOW_SHIFT - HS_TRIG_ABOVE_SHIFT)) | HS_TRIG_MASK);
      if (onPhase || ((f & HF_URGENT) != 0u && trig != 0u)) {
        heatListAdd(slot, HF_RECOMP, HM_RECOMP_COUNT, HM_RECOMP_LIST);
      }
      if (!onPhase) {
        // Still owed on a later phase: counted, so a world the CPU would
        // otherwise prove settled keeps ticking until the recompute is made
        // (heat.h kHmDueLeft).
        atomicAdd(&heatMeta[HM_DUE_LEFT], 1u);
        continue;
      }
    }
    atomicAnd(&heatMeta[HM_DUE + (slot >> 5u)], ~bit);
  }
}

// ============================================================================
// heatTent: one group per recompute-list chunk, THREE dispatches (pass =
// colorPhase.x: 0, 1, 2 filter along x, z, y -- Y LAST, see "heat rises")
// with the recorder's barrier between them -- a pass reads its NEIGHBOURS'
// output of the previous pass, so two axes can never share a dispatch.
//
// The filter is a separable TENT of half-width R blocks, weights R+1-|d|,
// normalised to 1 per axis: per block it carries the COVERAGE-WEIGHTED
// emitter sum sE (each emitting cell counts E x 1/8 of its block) and the
// coverage sf (1/8 per emitting cell), so after three passes
//   sE = sum over sources of w * g * E * n/8,   sf = sum of w * g * n/8.
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
// HEAT RISES (2026-10-02, owner: "a campfire must not scorch the bush beside
// it, but should ignite one directly above it"). g is a DIRECTION gain, one
// factor per axis: the y pass gives sim.heatUpGain to a source BELOW the
// block, sim.heatDownGain to one above and 1 to one level with it; the x and
// z passes each give sim.heatSideGain to a source off the block's own column
// and 1 on it. Straight up is the up gain, straight across the side gain, a
// diagonal the product (up-and-across = up x side). A product of per-axis
// factors has no classification boundary -- no "which direction dominates"
// test, so no cone edge in the field: the gain changes only where an axis
// offset leaves zero, which is the lattice the tent itself steps on. The gain
// weights the coverage AND the emitter sum alike, so it moves how close a
// block gets to the mean source temperature, never the mean: the ceiling
// holds at any gain. Y runs LAST because the up gain is large (a campfire
// fills ~2% of a block's surroundings, so reaching the flame's temperature
// three cells above it takes ~20x): the x and z passes, with side <= 1, stay
// inside the 16-bit planes, and the y pass's sums never leave registers.
//
// Plane 0 (the first pass's input) holds E and n; planes 1 and 2 hold the x
// and z passes' sE (x64, bits 0..15) and sf (x4096, bits 16..31). A
// neighbour chunk outside the window or without a page reads as zero, which
// is exact: every chunk within reach of a source is paged (heatWant), so an
// unpaged chunk carries no source and no partial sum.
@compute @workgroup_size(128)
fn heatTent(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  let slot = atomicLoad(&heatMeta[HM_RECOMP_LIST + wg.x]);
  let tpass = P.colorPhase.x;
  // Pass 0 -> x, 1 -> z, 2 -> y.
  let axis = select(select(1u, 2u, tpass == 1u), 0u, tpass == 0u);
  let e = heatEntry(slot);
  let base = heatPageBase(e);
  let wc = slotWorldChunk(slot, T.origin);
  let R = heatRadius();
  // Every tap's weight is (R+1-|d|) x a gain in HEAT_GAIN_ONE units.
  let norm = u32((R + 1) * (R + 1)) * HEAT_GAIN_ONE;
  let gSide = min(heatParams[HP_SIDE_GAIN], HEAT_GAIN_ONE);
  let gUp = min(heatParams[HP_UP_GAIN], 32u * HEAT_GAIN_ONE);
  let gDown = min(heatParams[HP_DOWN_GAIN], 32u * HEAT_GAIN_ONE);
  var dir = vec3<i32>(0);
  dir[axis] = 1;
  // The two neighbour pages along this axis (-1, +1).
  let qm = heatSlotOf(wc - dir);
  let qp = heatSlotOf(wc + dir);
  var em = 0u;
  var ep = 0u;
  if (qm != SLOT_NONE) { em = heatEntry(qm); }
  if (qp != SLOT_NONE) { ep = heatEntry(qp); }
  let inPlane = tpass * HEAT_BLOCKS;
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
      // The direction gain of a source at offset d (source - block) on this
      // axis: d < 0 on y is a source BELOW the block (heat rises to it).
      var g = HEAT_GAIN_ONE;
      if (d != 0) {
        if (axis != 1u) { g = gSide; } else { g = select(gDown, gUp, d < 0); }
      }
      let wt = u32(R + 1 - abs(d)) * g;
      if (tpass == 0u) {
        let ee = (w >> 16u) & 0xFFu;
        let nn = (w >> 24u) & 0xFu;
        sE += wt * ee * nn * (HEAT_SE_ONE / 8u);
        sF += wt * nn * (HEAT_SF_ONE / 8u);
      } else {
        sE += wt * (w & 0xFFFFu);
        sF += wt * (w >> 16u);
      }
    }
    if (tpass < 2u) {
      // Side <= 1, so neither sum can tpass its 16 bits (a full surround is
      // sE 255 x 64, sf 4096); the min is a guard, never a clamp.
      let oE = min(sE / norm, 0xFFFFu);
      let oF = min(sF / norm, 0xFFFFu);
      heatPool[base + (tpass + 1u) * HEAT_BLOCKS + b] = oE | (oF << 16u);
    } else {
      // X* = min(1, G sf) x (mean E - A), clamped to 0..255. The mean is
      // 64 sE / sF on the RAW sums (the gains and the norm cancel), floored
      // exactly in three steps so no product leaves u32 (sF < 2^28 at
      // R 8 and up 32): a floor of a weighted mean of integers never exceeds
      // the largest of them.
      let c = wc * i32(CHUNK) + bp * 2;
      let a = heatAmbient(c);
      var xs = 0;
      if (sF != 0u) {
        let r1 = sE % sF;
        let r2 = (r1 * 8u) % sF;
        let mean = (sE / sF) * 64u + ((r1 * 8u) / sF) * 8u + (r2 * 8u) / sF;
        let gap = i32(mean) - a;
        let eff = i32(min((sF / norm) * clamp(heatParams[HP_GAIN], 1u, 64u), HEAT_SF_ONE));
        xs = clamp(gap * eff / i32(HEAT_SF_ONE), 0, 255);
      }
      let old = heatPool[base + b];
      heatPool[base + b] = (old & 0xFFFF00FFu) | (u32(xs) << 8u);
    }
  }
  if (tpass == 2u && li == 0u) {
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
// never stops short of its target, so a stored X never outlives what made it
// -- including in a world otherwise asleep: every heat row is C_CAACTIVE, so
// the CPU's settled-tick proof would stop the walk, and a non-zero
// HM_PEND_COUNT (or HM_DUE_LEFT) in the snapshot withholds that proof until
// the list is empty (Simulation::NoteHeatPending, 2026-10-07).
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

// World chunk wc is in the draft box (T.draftOrigin; DRAFT_N* cells of 4
// voxels, so a chunk is 4 cells) or up to two chunks under it. False with the
// volume off.
fn heatInDraftReach(wc : vec3<i32>) -> bool {
  if (T.draftMode == 0u) { return false; }
  let v = wc - (T.draftOrigin >> vec3<u32>(CHUNK_SHIFT));
  return v.x >= 0 && v.z >= 0 && v.y >= -2 &&
         v.x < DRAFT_NX / 4 && v.y < DRAFT_NY / 4 && v.z < DRAFT_NZ / 4;
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
  let freeing = !nonzero && (keep & 2u) == 0u;
  // The draft volume's heat clock (heat.h kHmDraftHeatClock): this chunk's X
  // moved or its page goes back, and it lies in the draft box or within the
  // updraft's two-chunk look-down under it.
  if ((moved || freeing) && heatInDraftReach(wc)) {
    atomicAdd(&heatMeta[HM_DRAFT_HEAT_CLOCK], 1u);
  }
  if (freeing) {
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
