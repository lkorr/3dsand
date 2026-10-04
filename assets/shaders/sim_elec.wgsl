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
// THE UPDATE: P' = max(seed, max_nb6(P_nb - enter(cell, P_nb)), P - decay(P))
// >= 0 in a conducting cell, enter = resist + the SPREADING LOSS (elecEnter;
// elec.h); an insulator holds its seed (0 unless it is a source).
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
// COST (rule 2). A settled world records none of this (C_CAACTIVE), and since
// wave 2 neither does an active world that cannot hold charge (C_ELEC: World::
// ElecMayBeLive -- a source op in the last K + 2 ticks, pages in use or a
// doorbell ring in the published snapshot). Rounds after a chunk's first in a
// tick read its CELL CACHE (elec.h kElecCacheWords) instead of the voxels.

@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
// The solute layer (wave 2: brine conducts -- MIRROR elec, elecResistRaw).
// Read only in practice; declared as the solute block pastes it.
@group(0) @binding(40) var<storage, read_write> solTable : array<u32>;
@group(0) @binding(41) var<storage, read_write> solPool : array<atomic<u32>>;
@group(0) @binding(42) var<storage, read_write> solMeta : array<atomic<u32>>;
@group(0) @binding(43) var<storage, read> solSpec : array<u32>;
@group(0) @binding(55) var<storage, read_write> elecPool : array<u32>;
@group(0) @binding(56) var<storage, read_write> elecMeta : array<atomic<u32>>;
@group(0) @binding(57) var<storage, read> elecParams : array<u32>;

// MIRROR-BEGIN solute
// ---- THE SOLUTE LAYER'S SHARED ACCESSORS (docs/PLAN_solutes.md) -------------
// ONE block, pasted verbatim into every shader that touches dissolved mass
// (sim_step.wgsl, sim_solute.wgsl, sim_mutate.wgsl, sim_fluid_seam.wgsl) and
// held identical by scripts/check_invariants.py `solute`. Not in common.wgsl on
// purpose: common.wgsl is prepended to every shader, so an edit there misses the
// SPIR-V cache for all of them (CLAUDE.md, "What needs a rebuild"); this block
// only reaches the four modules that bind the layer.
//
// The layout is world.h's kSol* block, mirrored constant for constant:
//   solTable[slot]  0 = SOL_EMPTY | SOL_UNIFORM_BIT | value16 | SOL_PAGE_BIT | page
//   solPool         16-bit cells (species << 8 | mass), two per word, the EVEN
//                   chunk-local index in the low half
//   solMeta         SOLM_* header, free stack, want list/flags, request flags,
//                   stall counters, per-chunk aggregates
//   solSpec         species params (SOLS_*) + one word per material
//
// POOL WORDS ARE WRITTEN WITH TWO ATOMICS (and, then or) because two cells share
// a word and the colour lattice lets two acting threads write neighbouring
// cells: a plain read-modify-write of the word would lose one of them. Atomic
// ops on DISJOINT bits commute, so the result does not depend on their order.
const SOL_UNIFORM_BIT : u32 = 0x80000000u;
const SOL_PAGE_BIT : u32 = 0x40000000u;
const SOL_PAGE_MASK : u32 = 0x3FFFFFFFu;
const SOL_WORDS_PER_PAGE : u32 = 2048u;
const SOL_POOL_PAGES : u32 = 4352u;
const SOLM_FREE : u32 = 0u;
const SOLM_WANT_COUNT : u32 = 1u;
const SOLM_FAULTS : u32 = 2u;
const SOLM_EXHAUSTED : u32 = 3u;
const SOLM_HIGH_WATER : u32 = 4u;
const SOLM_DISSOLVED : u32 = 5u;
const SOLM_PRECIP : u32 = 6u;
const SOLM_DISCARDED : u32 = 7u;
const SOLM_CONVERTED : u32 = 8u;
const SOLM_FAULT_SLOT : u32 = 9u;
const SOLM_FAULT_TICK : u32 = 10u;
const SOLM_POURED : u32 = 11u;
const SOLM_SCOOPED : u32 = 12u;
const SOLM_SEAM_REFUSED : u32 = 13u;
const SOLM_ARGS : u32 = 16u;
const SOLM_STACK : u32 = 64u;
const SOLM_WANT_LIST : u32 = SOLM_STACK + SOL_POOL_PAGES;
const SOLM_WANT_FLAG : u32 = SOLM_WANT_LIST + NUM_SLOTS;
const SOLM_REQ_FLAG : u32 = SOLM_WANT_FLAG + NUM_SLOTS;
const SOLM_STALL : u32 = SOLM_REQ_FLAG + NUM_SLOTS;
const SOLM_AGG : u32 = SOLM_STALL + NUM_SLOTS;
const SOLS_BASE : u32 = 16u;
const SOLS_STRIDE : u32 = 16u;
const SOLS_MAT_BASE : u32 = 4112u;
const SOLS_RULE_BASE : u32 = 8208u;
// Bit 26 of the dirty word (world.h kDirtyReasonName "solute"): dissolved mass
// still moving. Deliberately NOT in sim_step's FILM_LICENCE.
const DIRTY_R_SOLUTE : u32 = 67108864u;
// Bit 27 ("solute-back"): a chunk's diffusion found work across its -axis face
// in a pair its -axis neighbour OWNS. The owner, woken by it, keeps itself
// awake for a whole phase cycle (it cannot know which of the 24 phases the
// pair lives in, and waking it for one tick lands on the wrong one).
const DIRTY_R_SOLBACK : u32 = 134217728u;
// A chunk whose diffusion found nothing to do for this many CONSECUTIVE
// dispatches has seen every axis, parity and stride (3 x 2 x 4 = 24 phases,
// eight ticks) idle: it is STALLED, the dilution floor applies and it stops
// keeping itself awake.
const SOL_STALL_TICKS : u32 = 24u;

fn solSpeciesOf(v : u32) -> u32 { return v >> 8u; }
fn solMassOf(v : u32) -> u32 { return v & 0xFFu; }
fn solPack(species : u32, mass : u32) -> u32 {
  if (mass == 0u || species == 0u) { return 0u; }
  return (species << 8u) | min(mass, 255u);
}
fn solLocalOf(c : vec3<i32>) -> u32 {
  let lo = vec3<u32>(c & vec3<i32>(CHUNK_MASK));
  return (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
}
// The cell value a table entry implies for chunk-local cell `local`.
fn solCellValue(e : u32, local : u32) -> u32 {
  if (e == 0u) { return 0u; }
  if ((e & SOL_UNIFORM_BIT) != 0u) { return e & 0xFFFFu; }
  let wv = atomicLoad(&solPool[(e & SOL_PAGE_MASK) * SOL_WORDS_PER_PAGE + (local >> 1u)]);
  return (wv >> ((local & 1u) * 16u)) & 0xFFFFu;
}
// The STORED value at world cell c. A caller that wants the value a cell
// carries must also ask whether the cell is a liquid (solCarried): a value
// left behind on a cell something turned into air is stale until solCompact
// clears it, and must read as nothing.
fn solValueAt(c : vec3<i32>) -> u32 {
  return solCellValue(solTable[voxSlotOfCell(c)], solLocalOf(c));
}
fn solWritable(c : vec3<i32>) -> bool {
  return (solTable[voxSlotOfCell(c)] & SOL_PAGE_BIT) != 0u;
}
// A write into a chunk with no page is REFUSED and counted -- the allocator
// promises a page to every chunk a tick can write, so a refusal is a bug, and
// the count is what says so (SubmitTick aborts on it: support.cpp). A
// write of the value the sentinel already implies is not a write at all.
fn solFault(slot : u32) {
  let prev = atomicAdd(&solMeta[SOLM_FAULTS], 1u);
  if (prev == 0u) {
    atomicStore(&solMeta[SOLM_FAULT_SLOT], slot + 1u);
    atomicStore(&solMeta[SOLM_FAULT_TICK], T.tick);
  }
}
fn solStoreAt(c : vec3<i32>, v : u32) -> bool {
  let slot = voxSlotOfCell(c);
  let e = solTable[slot];
  let local = solLocalOf(c);
  if ((e & SOL_PAGE_BIT) == 0u) {
    if (solCellValue(e, local) == v) { return true; }
    solFault(slot);
    return false;
  }
  let wi = (e & SOL_PAGE_MASK) * SOL_WORDS_PER_PAGE + (local >> 1u);
  let sh = (local & 1u) * 16u;
  let old = (atomicLoad(&solPool[wi]) >> sh) & 0xFFFFu;
  if (old == v) { return true; }
  atomicAnd(&solPool[wi], ~(0xFFFFu << sh));
  atomicOr(&solPool[wi], (v & 0xFFFFu) << sh);
  return true;
}
// ---- the species table (world.h kSolSpec*, Simulation::UploadSolutes) ----
fn solYield8(s : u32) -> u32 { return max(solSpec[SOLS_BASE + s * SOLS_STRIDE], 1u); }
fn solSaturation(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 1u]; }
fn solDissolveChance(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 2u]; }
fn solDiffusivity(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 3u]; }
fn solFloor(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 4u]; }
fn solDensityPerUnit(s : u32) -> i32 { return bitcast<i32>(solSpec[SOLS_BASE + s * SOLS_STRIDE + 5u]); }
fn solPrecipitate(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 9u]; }
fn solConvertCount(s : u32) -> u32 { return min(solSpec[SOLS_BASE + s * SOLS_STRIDE + 10u], 4u); }
fn solConvertRow(s : u32, k : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 11u + k]; }
fn solMatWord(m : u32) -> u32 { return solSpec[SOLS_MAT_BASE + (m & 0xFFFu)]; }
// The species powder `m` dissolves AS, or 0.
fn solFromSpecies(m : u32) -> u32 { return solMatWord(m) & 0xFFu; }
// Is liquid `m` a solvent of species `s`?
fn solIsSolvent(m : u32, s : u32) -> bool {
  if (s == 0u || s > 24u) { return false; }
  return (solMatWord(m) & (1u << (7u + s))) != 0u;
}
// The most mass a cell of `f` eighths of a solvent holds of species `s`.
fn solCap(s : u32, f : u32) -> u32 { return (solSaturation(s) * f) / 8u; }
fn solIsLiquidMat(m : u32) -> bool {
  return m != MAT_AIR && materials[m].klass == CLASS_LIQUID;
}
// The value cell c CARRIES given its word `w`: the stored value if the cell is
// a liquid, else nothing (see solValueAt).
fn solCarried(c : vec3<i32>, w : u32) -> u32 {
  if (!solIsLiquidMat(voxMat(w))) { return 0u; }
  return solValueAt(c);
}
// A CONCENTRATION CONDITION on a reaction rule (PLAN_solutes §4.1; the side
// array at SOLS_RULE_BASE, indexed by the rule's GPU index): may rule `ri`
// fire for the self cell c whose word is w? An unconditioned rule (word 0)
// always may; a conditioned one needs a liquid self carrying that species at
// a concentration (mass * 8 / fullness) inside [cMin, cMax].
fn solRuleAllows(ri : u32, c : vec3<i32>, w : u32) -> bool {
  let cond = solSpec[SOLS_RULE_BASE + ri];
  if (cond == 0u) { return true; }
  if (!solIsLiquidMat(voxMat(w))) { return false; }
  let v = solValueAt(c);
  if (solSpeciesOf(v) != (cond & 0xFFu)) { return false; }
  let conc = (solMassOf(v) * 8u) / (voxState(w) + 1u);
  return conc >= ((cond >> 8u) & 0xFFu) && conc <= ((cond >> 16u) & 0xFFu);
}
// MIRROR-END solute

// MIRROR-BEGIN elec
// ---- THE CHARGE FIELD'S SHARED ACCESSORS (src/sim/elec.h) -------------------
// ONE block, pasted verbatim into sim_elec.wgsl and sim_step.wgsl and held
// identical by scripts/check_invariants.py `elec`, which also checks every
// constant against elec.h. Not in common.wgsl: an edit there misses the SPIR-V
// cache of every shader, and only these agree on the layout. A shader that
// pastes it binds elecPool (55), elecMeta (56, atomic) and elecParams (57),
// the solute layer (40..43, its MIRROR block) and declares EP_MAT,
// EP_MAT_STRIDE and EP_WET.
//
// READING P (packages E2 / E4): elecAt(slot, local) is the SETTLED field --
// half 0, what elecSettle left at the end of the LAST tick. The elec rows run
// after the CA, so inside the CA it is stable for the whole tick (as heatPool
// is). `slot` is a WINDOW slot (tickets carry no charge and read 0), `local`
// the chunk-local cell index (z * 256 + y * 16 + x).
//
// THE OWNER CHECK (wave 2, 2026-10-04): a page belongs to the world chunk its
// owner word names. When the window moves, a slot changes chunk BEFORE the elec
// head re-keys its page (after the CA), and on a tick the elec rows skip
// (World::ElecMayBeLive false) not at all -- so a reader that trusted the entry
// alone saw the departed chunk's charge in the arrived one for a tick (the
// phantom charge). A slot whose owner is not its resident chunk reads 0.
const ELEC_POOL_PAGES : u32 = 2048u;
const ELEC_PAGE_WORDS : u32 = 4096u;
const ELEC_HALF_WORDS : u32 = 2048u;
const ELEC_ENTRY_HAS : u32 = 0x80000000u;
const ELEC_ENTRY_PAGE : u32 = 0x00FFFFFFu;
const ELEC_RES_MASK : u32 = 0xFFFu;
const EM_ENTRY : u32 = 64u;
const EM_OWNER : u32 = 32832u;
const EM_WANT : u32 = 65600u;
const EM_DOORBELLS : u32 = 7u;
const ELEC_R2_SOURCE : u32 = 33554432u;
const ELEC_R2_CONDUCTS : u32 = 67108864u;

// The owner word of world chunk wc (elec.h ElecOwnerKey): 10 bits an axis, +1
// so a zero word is never an owner.
fn elecOwnerKey(wc : vec3<i32>) -> u32 {
  let u = vec3<u32>(wc & vec3<i32>(1023));
  return (u.x | (u.y << 10u) | (u.z << 20u)) + 1u;
}
fn elecAt(slot : u32, local : u32) -> u32 {
  if (slot >= NUM_CHUNKS) { return 0u; }
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  if ((e & ELEC_ENTRY_HAS) == 0u) { return 0u; }
  if (atomicLoad(&elecMeta[EM_OWNER + slot]) != elecOwnerKey(slotWorldChunk(slot, T.origin))) {
    return 0u;
  }
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

// ---- THE RESIST RULE: one copy for the field (sim_elec) and the CA (E2) ----
// Both used to carry their own; brine made them three-part, so it lives here.
fn elecMatWord(m : u32) -> u32 { return elecParams[EP_MAT + (m & 0xFFFu) * EP_MAT_STRIDE]; }
// The resist of the cell holding word w from its MATERIAL and its COAT alone,
// 0 = insulator. THE WET RULE: a cell under a coat whose material conducts
// (water, blood, brine's water...) conducts through the film --
// min(own, wet[stain amount]) -- so a wet plank or wet stone carries what a
// dry one does not. Whether a cell conducts at all is decided here (brine
// below only lowers a conductor's resist), so the field's neighbour count uses
// this, the cheap half.
fn elecResistBase(w : u32) -> u32 {
  var r = elecMatWord(voxMat(w)) & ELEC_RES_MASK;
  if (voxStained(w)) {
    let coat = materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3 & 0xFFFu;
    if ((elecMatWord(coat) & ELEC_RES_MASK) != 0u) {
      let wr = elecParams[EP_WET + voxStainAmt(w)];
      r = select(wr, min(r, wr), r != 0u);
    }
  }
  return r;
}
// ...and BRINE (wave 2): a conducting LIQUID carrying a dissolved electrolyte
// (the species' `from` material has electric.dissolved, its word 3: salt) at
// concentration c conducts as dissolved + (own - dissolved) x (sat - c) / sat
// -- fresh water at c = 0, the electrolyte's own resist at saturation. The
// concentration is the solute layer's (mass x 8 / fullness, solRuleAllows').
// `slot` / `local` locate the cell in the solute table (a window slot).
fn elecResistRaw(w : u32, slot : u32, local : u32) -> u32 {
  let r = elecResistBase(w);
  if (r <= 1u || slot >= NUM_CHUNKS) { return r; }
  if (materials[voxMat(w)].klass != CLASS_LIQUID) { return r; }
  let v = solCellValue(solTable[slot], local);
  let s = solSpeciesOf(v);
  if (s == 0u) { return r; }
  let fromMat = solSpec[SOLS_BASE + s * SOLS_STRIDE + 15u] & 0xFFFu;
  let re = elecParams[EP_MAT + fromMat * EP_MAT_STRIDE + 3u] & ELEC_RES_MASK;
  if (re == 0u || re >= r) { return r; }
  let sat = max(solSaturation(s), 1u);
  let conc = min((solMassOf(v) * 8u) / (voxState(w) + 1u), sat);
  return re + ((r - re) * (sat - conc)) / sat;
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
const EM_LIST0 : u32 = 66624u;
const EM_LIST1 : u32 = 68672u;
const EM_STACK : u32 = 70720u;
const ELEC_ARG_ROUND : u32 = 0u;
const ELEC_ARG_PURGE : u32 = 1u;
const ELEC_ROUNDS_MAX : u32 = 8u;
const ELEC_RES_INS : u32 = 4095u;
const ELEC_CACHE_WORDS : u32 = 2048u;
const ELEC_CACHE_BASE : u32 = 8388608u;
const ELEC_STAMP_BASE : u32 = 12582912u;
const EP_MODE : u32 = 0u;
const EP_ROUNDS : u32 = 1u;
const EP_DECAY : u32 = 2u;
const EP_ITER_CAP : u32 = 3u;
const EP_DECAY_SHIFT : u32 = 13u;
const EP_SPREAD_Q : u32 = 14u;
const EP_SPREAD_FREE : u32 = 15u;
const EP_WET : u32 = 16u;
const EP_MAT : u32 = 32u;
const EP_MAT_STRIDE : u32 = 4u;
// Bit 31 of the dirty word (world.h kDirtyReasonName "elec"): the chunk holds
// charge. Not in sim_step's FILM_LICENCE: charge is not liquid progress.
const DIRTY_R_ELEC : u32 = 2147483648u;

fn elecMode() -> bool { return elecParams[EP_MODE] != 0u; }
fn elecRounds() -> u32 { return clamp(elecParams[EP_ROUNDS], 1u, ELEC_ROUNDS_MAX); }
fn elecList(which : u32) -> u32 { return select(EM_LIST0, EM_LIST1, which != 0u); }
fn elecSetArgs(rec : u32, x : u32) {
  let o = EM_ARGS + rec * 4u;
  atomicStore(&elecMeta[o], x);
  atomicStore(&elecMeta[o + 1u], 1u);
  atomicStore(&elecMeta[o + 2u], 1u);
}
fn elecSub(a : u32, b : u32) -> u32 { return select(0u, a - b, a > b); }
// THE DECAY of a stored value (round 0 only; `floor` is 0 in later rounds, and
// so is the whole decay): max(floor, p >> shift), shift 0 = linear only.
// Proportional above floor << shift, so lightning's 30,000 is gone in ~55
// ticks, not 3,750. Applied to the OWN value and the HALO alike -- the whole
// stored field decays before the rounds relax it (DESIGN.md: a self-only decay
// lets two chunks hold each other up across their face).
fn elecDecayed(p : u32, floor : u32, shift : u32) -> u32 {
  if (floor == 0u) { return p; }
  var d = floor;
  if (shift != 0u) { d = max(d, p >> shift); }
  return elecSub(p, d);
}

// The resist of the cell holding word w as the FIELD sees it: the shared rule
// (MIRROR elec, elecResistRaw: material, wet coat, brine), 1..4094, or
// ELEC_RES_INS for an insulator.
fn elecCellResist(w : u32, slot : u32, local : u32) -> u32 {
  let r = elecResistRaw(w, slot, local);
  return select(r, ELEC_RES_INS, r == 0u);
}
fn elecCellSource(w : u32) -> u32 { return elecMatWord(voxMat(w)) >> 16u; }
// The word of chunk-local cell i of a chunk whose page-table entry is pe. A
// sentinel chunk is one material and carries no stain, so its word is the
// material id (the JITTER palette variant is irrelevant here).
fn elecWordIn(pe : u32, i : u32) -> u32 {
  if ((pe & PT_SENTINEL_BIT) != 0u) { return pe & PT_MAT_MASK; }
  return voxels[pe * CHUNK_VOL + i];
}

// THE ENTRY COST (wave 2, src/sim/elec.h): what a cell whose shared-memory
// word is c (P | resist << 16 | conducting neighbours << 28) takes off a
// potential `prev` arriving from a neighbour -- its resist, plus the SPREADING
// LOSS: prev x spreadQ(n) / 4096, spreadQ(n) = (n - free) x k past the free
// neighbours (a wire's two cost nothing), held under 4096 so the result never
// falls as prev rises (monotone: the relaxation still only climbs to a least
// fixpoint, whatever the order). Integer, no overflow (65535 x 4095 < 2^32).
fn elecEnter(prev : u32, c : u32, k : u32, free : u32) -> u32 {
  let res = (c >> 16u) & ELEC_RES_MASK;
  let n = c >> 28u;
  var q = 0u;
  if (n > free) { q = min((n - free) * k, 4095u); }
  return elecSub(prev, res + ((prev * q) >> 12u));
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
// The head's re-keyed pages (base word of half 0), zeroed by the whole group.
var<workgroup> wgRekey : array<u32, 2048>;
var<workgroup> wgRekeyN : atomic<u32>;
var<workgroup> wgRekeyCount : u32;

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
  // it if nothing re-charges it. WORKGROUP-PARALLEL (wave 2): each thread finds
  // the re-keyed pages among its share of the list, then all 256 zero each one
  // -- one thread zeroing 16 KiB serially was the head's long pole on a window
  // shift. Only half 0: half 1 is zero on every listed page (elecSettle zeroes
  // it, and a page fresh off the stack is all zero). Which thread found which
  // page, and in what order they are zeroed, is unobservable.
  if (li == 0u) { atomicStore(&wgRekeyN, 0u); }
  workgroupBarrier();
  if (phase == 0u) {
    let n = atomicLoad(&elecMeta[EM_COUNT0 + cur]);
    for (var i = li; i < n; i += ALLOC_WG) {
      let slot = atomicLoad(&elecMeta[elecList(cur) + i]);
      let key = elecOwnerKey(slotWorldChunk(slot, T.origin));
      if (atomicLoad(&elecMeta[EM_OWNER + slot]) != key) {
        let at = atomicAdd(&wgRekeyN, 1u);
        wgRekey[at] = (atomicLoad(&elecMeta[EM_ENTRY + slot]) & ELEC_ENTRY_PAGE) * ELEC_PAGE_WORDS;
        atomicStore(&elecMeta[EM_OWNER + slot], key);
        atomicAdd(&elecMeta[EM_REKEYED], 1u);
      }
    }
  }
  workgroupBarrier();
  if (li == 0u) { wgRekeyCount = atomicLoad(&wgRekeyN); }
  let nRekey = workgroupUniformLoad(&wgRekeyCount);
  for (var k = 0u; k < nRekey; k++) {
    let base = wgRekey[k];
    for (var j = li; j < ELEC_HALF_WORDS; j += ALLOC_WG) { elecPool[base + j] = 0u; }
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
// Shared memory holds the chunk: wgP[i] = P (bits 0..15) | resist (16..27) |
// conducting face neighbours (28..30), and the six faces' halo (the neighbour
// cell across each face, its previous round's P, 0 if unpaged or out of the
// window). A chunk with no page in a neighbour reads 0 there: charge enters a
// new chunk only once elecAlloc pages it, which is what the face wants below
// ask for.
var<workgroup> wgP : array<u32, 4096>;
var<workgroup> wgHalo : array<u32, 1536>;
var<workgroup> wgChanged : atomic<u32>;
var<workgroup> wgFaces : atomic<u32>;
var<workgroup> wgMax : atomic<u32>;
var<workgroup> wgCondMax : atomic<u32>;
var<workgroup> wgGo : u32;
var<workgroup> wgCap : u32;
var<workgroup> wgBuilt : u32;

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
// the high one's. Returns whether any cell rose. k / free: the spreading loss
// (elecEnter).
fn elecSweep(axis : u32, li : u32, k : u32, free : u32) -> bool {
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
    var p = c & 0xFFFFu;
    if (((c >> 16u) & ELEC_RES_MASK) != ELEC_RES_INS) {
      let cand = elecEnter(prev, c, k, free);
      if (cand > p) {
        p = cand;
        wgP[idx] = (c & 0xFFFF0000u) | p;
        changed = true;
      }
    }
    prev = p;
  }
  prev = wgHalo[(axis * 2u + 1u) * 256u + li];
  for (var t = 0u; t < 16u; t++) {
    let idx = base + (15u - t) * stride;
    let c = wgP[idx];
    var p = c & 0xFFFFu;
    if (((c >> 16u) & ELEC_RES_MASK) != ELEC_RES_INS) {
      let cand = elecEnter(prev, c, k, free);
      if (cand > p) {
        p = cand;
        wgP[idx] = (c & 0xFFFF0000u) | p;
        changed = true;
      }
    }
    prev = p;
  }
  return changed;
}

// One cell's CACHE word (elec.h kElecCacheWords): its resist (12 bits, from
// wgP's build-time staging: resist | source << 15), its conducting face
// neighbours (inside the chunk from the staging, across a face from the
// neighbour chunk's voxel -- the cheap half of the rule, elecResistBase:
// whether a cell conducts never depends on its brine), and the source bit.
fn elecCacheCell(i : u32, wc : vec3<i32>) -> u32 {
  let st = wgP[i];
  let res = st & ELEC_RES_MASK;
  let lo = vec3<i32>(i32(i & 15u), i32((i >> 4u) & 15u), i32(i >> 8u));
  var n = 0u;
  for (var f = 0u; f < 6u; f++) {
    let d = elecFaceDir(f);
    let q = lo + d;
    if (all(q >= vec3<i32>(0)) && all(q < vec3<i32>(16))) {
      let ni = u32((q.z * 16 + q.y) * 16 + q.x);
      if ((wgP[ni] & ELEC_RES_MASK) != ELEC_RES_INS) { n++; }
    } else {
      let nwc = wc + d;
      if (chunkInWindow(nwc, T.origin) && elecResistBase(voxWordAt(wc * i32(CHUNK) + q)) != 0u) {
        n++;
      }
    }
  }
  return res | (n << 12u) | (st & 0x8000u);
}

@compute @workgroup_size(256)
fn elecRound(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let cur = atomicLoad(&elecMeta[EM_CUR]) & 1u;
  let slot = atomicLoad(&elecMeta[elecList(cur) + wg.x]);
  let e = atomicLoad(&elecMeta[EM_ENTRY + slot]);
  let pageIdx = e & ELEC_ENTRY_PAGE;
  let page = pageIdx * ELEC_PAGE_WORDS;
  let cacheAt = ELEC_CACHE_BASE + pageIdx * ELEC_CACHE_WORDS;
  let stampAt = ELEC_STAMP_BASE + pageIdx;
  let r = atomicLoad(&elecMeta[EM_PHASE]) - 1u;   // the head alloc made it 1
  let src = page + (r & 1u) * ELEC_HALF_WORDS;
  let dst = page + (1u - (r & 1u)) * ELEC_HALF_WORDS;
  let decay = select(0u, max(elecParams[EP_DECAY], 1u), r == 0u);
  let decayShift = min(elecParams[EP_DECAY_SHIFT], 15u);
  let spreadK = min(elecParams[EP_SPREAD_Q], 4095u);
  let spreadFree = min(elecParams[EP_SPREAD_FREE], 6u);
  let mode = elecMode();
  let wc = slotWorldChunk(slot, T.origin);
  let pe = pageEntryOf(slot);
  if (li == 0u) {
    atomicStore(&wgChanged, 0u);
    atomicStore(&wgFaces, 0u);
    atomicStore(&wgMax, 0u);
    atomicStore(&wgCondMax, 0u);
    wgCap = clamp(elecParams[EP_ITER_CAP], 1u, 64u);
    wgBuilt = select(0u, 1u, elecPool[stampAt] == T.tick + 1u);
  }
  let built = workgroupUniformLoad(&wgBuilt) != 0u;
  // THE CELL CACHE (wave 2, elec.h kElecCacheWords): resist, neighbour count
  // and the source bit of every cell, built by this page's FIRST round of the
  // tick (the voxels do not change between the elec rows, so it is exact) and
  // read by every later one -- which then touch no voxel, material, stain or
  // solute word, and scan no face. Each thread owns words li + 256 k.
  var cw : array<u32, 8>;
  if (!built) {
    // Staging: resist | source << 15 per cell, so the neighbour count can
    // read the whole chunk's.
    for (var i = li; i < CHUNK_VOL; i += 256u) {
      let w = elecWordIn(pe, i);
      let res = elecCellResist(w, slot, i);
      wgP[i] = res | select(0u, 0x8000u, elecCellSource(w) != 0u);
    }
    workgroupBarrier();
    for (var k = 0u; k < 8u; k++) {
      let j = li + k * 256u;
      cw[k] = elecCacheCell(2u * j, wc) | (elecCacheCell(2u * j + 1u, wc) << 16u);
      elecPool[cacheAt + j] = cw[k];
    }
    workgroupBarrier();
    if (li == 0u) { elecPool[stampAt] = T.tick + 1u; }
  } else {
    for (var k = 0u; k < 8u; k++) { cw[k] = elecPool[cacheAt + li + k * 256u]; }
  }
  // THE CELLS: seed, resist, neighbours, and last round's P (this tick's
  // decay in round 0). A source re-reads its voxel for the seed (rare).
  for (var k = 0u; k < 8u; k++) {
    let j = li + k * 256u;
    let olds = elecPool[src + j];
    for (var h = 0u; h < 2u; h++) {
      let i = 2u * j + h;
      let cc = (cw[k] >> (h * 16u)) & 0xFFFFu;
      var res = cc & ELEC_RES_MASK;
      var seed = 0u;
      if ((cc & 0x8000u) != 0u) { seed = elecCellSource(elecWordIn(pe, i)); }
      if (!mode) {
        res = ELEC_RES_INS;
        seed = 0u;
      }
      var p = seed;
      if (res != ELEC_RES_INS) {
        let old = (olds >> (h * 16u)) & 0xFFFFu;
        p = max(p, elecDecayed(old, decay, decayShift));
      }
      wgP[i] = p | (res << 16u) | (((cc >> 12u) & 7u) << 28u);
    }
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
          v = elecDecayed(old, decay, decayShift);
        }
      }
    }
    wgHalo[k] = v;
  }
  workgroupBarrier();
  let cap = workgroupUniformLoad(&wgCap);
  // THE FIXPOINT: x, y, z sweeps until a whole iteration raises nothing.
  for (var it = 0u; it < cap; it++) {
    var ch = elecSweep(0u, li, spreadK, spreadFree);
    workgroupBarrier();
    ch = elecSweep(1u, li, spreadK, spreadFree) || ch;
    workgroupBarrier();
    ch = elecSweep(2u, li, spreadK, spreadFree) || ch;
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
    if (((c0 >> 16u) & ELEC_RES_MASK) != ELEC_RES_INS) { cmax = max(cmax, lo); }
    if (((c1 >> 16u) & ELEC_RES_MASK) != ELEC_RES_INS) { cmax = max(cmax, hi); }
  }
  if (lmax != 0u) { atomicMax(&wgMax, lmax); }
  if (cmax != 0u) { atomicMax(&wgCondMax, cmax); }
  // THE FACE WANTS: an unpaged window neighbour whose cell across a face can
  // take charge from this side (it conducts, and P here exceeds its resist).
  // Only those -- a chunk of air or stone beside a charged one is never paged.
  // (Its spreading loss is not counted here: the neighbour's neighbours are
  // unknown, and over-asking only pages a chunk the round then frees.)
  if (mode) {
    for (var f = 0u; f < 6u; f++) {
      let own = elecFaceCell(f, li & 15u, li >> 4u, true);
      let p = wgP[own] & 0xFFFFu;
      if (p <= 1u) { continue; }
      let d = elecFaceDir(f);
      let nwc = wc + d;
      if (!chunkInWindow(nwc, T.origin)) { continue; }
      let nslot = chunkSlotIndex(nwc);
      if ((atomicLoad(&elecMeta[EM_ENTRY + nslot]) & ELEC_ENTRY_HAS) != 0u) { continue; }
      let lo = vec3<i32>(i32(own & 15u), i32((own >> 4u) & 15u), i32(own >> 8u));
      let nl = elecFaceCell(f, li & 15u, li >> 4u, false);
      let nres = elecCellResist(voxWordAt(wc * i32(CHUNK) + lo + d), nslot, nl);
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

// ============================================================================
// elecQuery: THE BODY QUERY (package E4, docs/PLAN_electricity.md section 4;
// wave 2 package B, docs/PLAN_electricity_wave2.md; src/sim/elec.h
// kElecQuery*). One workgroup per box the CPU asked about this tick
// (MobSystem::QueueShockQueries: ONE box per body -- its limbs, shells and held
// items -- dilated a cell so a foot ON a charged plate counts). It reads the
// SETTLED field -- half 0, after this tick's settle / tail / purge -- and
// writes four words per box into elecMeta's tail: the max P, the cells with
// P > 0, their P summed and the cells scanned. Then the box's GRID, when the
// CPU gave it one (box word 6 = its word offset in the grid area): every cell
// as a u16 -- P clamped to ELEC_QUERY_GRID_PMAX, ELEC_QUERY_GRID_AIR when the
// cell is air (the body's crackle and ignition frontier) -- two cells a word,
// x fastest. The CPU conducts the charge through the body's own materials from
// it (mob_shock.cpp). The snapshot ring carries everything to the CPU at the
// fixed latency (World::kSnapshotLatency); the tag that says WHOSE box it was
// never leaves the CPU (the readback slot holds it).
//
// DETERMINISM (rule 1): max and sum are order-independent; each grid word is
// written by exactly one thread (it owns both of its cells), and each group
// writes only its own box's words. Read-only on the field and the voxels.
// COST (rule 2): recorded only on a tick with boxes (C_ELECQUERY), and the CPU
// queues boxes only while the field can hold charge (World::ElecMayBeLive: a
// source op in the last K + 2 ticks, or pages in use in the published
// snapshot). A group whose field has no page in use writes zeros after one
// load (its grid is left unwritten: the CPU never reads the grid of a box
// whose max P is 0 unless a charged body touches it, and then reads "no P").
const EM_QUERY : u32 = 72768u;
const EM_QUERY_GRID : u32 = 73088u;
const EP_QUERY : u32 = 16416u;
const EP_QUERY_BOXES : u32 = 16420u;
const ELEC_QUERY_MAX : u32 = 80u;
const ELEC_QUERY_BOX_WORDS : u32 = 8u;
const ELEC_QUERY_RES_WORDS : u32 = 4u;
const ELEC_QUERY_AXIS_MAX : u32 = 32u;
const ELEC_QUERY_GRID_WORDS : u32 = 49152u;
const ELEC_QUERY_GRID_PMAX : u32 = 32767u;
const ELEC_QUERY_GRID_AIR : u32 = 32768u;
const ELEC_QUERY_NO_GRID : u32 = 0xFFFFFFFFu;

var<workgroup> wgqMax : atomic<u32>;
var<workgroup> wgqCharged : atomic<u32>;
var<workgroup> wgqSum : atomic<u32>;
var<workgroup> wgqLive : u32;

// One grid cell: P clamped, | air.
fn elecQueryCell(c : vec3<i32>) -> u32 {
  if (!chunkInWindow(worldChunkOf(c), T.origin)) { return 0u; }
  var v = min(elecAtCell(c), ELEC_QUERY_GRID_PMAX);
  if (voxMat(voxWordAt(c)) == MAT_AIR) { v = v | ELEC_QUERY_GRID_AIR; }
  return v;
}

@compute @workgroup_size(64)
fn elecQuery(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let q = wg.x;
  if (q >= min(elecParams[EP_QUERY], ELEC_QUERY_MAX)) { return; }
  let b = EP_QUERY_BOXES + q * ELEC_QUERY_BOX_WORDS;
  let lo = vec3<i32>(bitcast<i32>(elecParams[b]), bitcast<i32>(elecParams[b + 1u]),
                     bitcast<i32>(elecParams[b + 2u]));
  let hi = vec3<i32>(bitcast<i32>(elecParams[b + 3u]), bitcast<i32>(elecParams[b + 4u]),
                     bitcast<i32>(elecParams[b + 5u]));
  let gridOff = elecParams[b + 6u];
  let dims = vec3<u32>(clamp(hi - lo + vec3<i32>(1), vec3<i32>(0),
                             vec3<i32>(i32(ELEC_QUERY_AXIS_MAX))));
  let total = dims.x * dims.y * dims.z;
  if (li == 0u) {
    atomicStore(&wgqMax, 0u);
    atomicStore(&wgqCharged, 0u);
    atomicStore(&wgqSum, 0u);
    // Pages in use (handed out and not back on the stack): none = no charge
    // anywhere, and the box is answered without a scan.
    let fresh = atomicLoad(&elecMeta[EM_NEXT_FRESH]);
    let top = atomicLoad(&elecMeta[EM_FREE_TOP]);
    wgqLive = select(0u, 1u, fresh > top);
  }
  let live = workgroupUniformLoad(&wgqLive);
  if (live != 0u) {
    for (var i = li; i < total; i += 64u) {
      let c = lo + vec3<i32>(vec3<u32>(i % dims.x, (i / dims.x) % dims.y, i / (dims.x * dims.y)));
      let p = elecAtCell(c);
      if (p != 0u) {
        atomicMax(&wgqMax, p);
        atomicAdd(&wgqCharged, 1u);
        atomicAdd(&wgqSum, p);
      }
    }
    // THE GRID: one thread a word, both of its cells.
    let words = (total + 1u) / 2u;
    if (gridOff != ELEC_QUERY_NO_GRID && gridOff + words <= ELEC_QUERY_GRID_WORDS) {
      for (var j = li; j < words; j += 64u) {
        var w = 0u;
        for (var h = 0u; h < 2u; h++) {
          let i = j * 2u + h;
          if (i >= total) { break; }
          let c = lo + vec3<i32>(vec3<u32>(i % dims.x, (i / dims.x) % dims.y, i / (dims.x * dims.y)));
          w = w | (elecQueryCell(c) << (h * 16u));
        }
        atomicStore(&elecMeta[EM_QUERY_GRID + gridOff + j], w);
      }
    }
  }
  workgroupBarrier();
  if (li != 0u) { return; }
  let o = EM_QUERY + q * ELEC_QUERY_RES_WORDS;
  atomicStore(&elecMeta[o], atomicLoad(&wgqMax));
  atomicStore(&elecMeta[o + 1u], atomicLoad(&wgqCharged));
  atomicStore(&elecMeta[o + 2u], atomicLoad(&wgqSum));
  atomicStore(&elecMeta[o + 3u], total);
}
