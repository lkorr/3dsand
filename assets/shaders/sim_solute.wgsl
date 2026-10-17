// sim_solute.wgsl -- THE SOLUTE LAYER: allocation, diffusion, compaction, hash.
//
// docs/PLAN_solutes.md is the design of record; DESIGN.md "Solutes" is the
// summary; world.h's kSol* block is the layout. Dissolved matter is MASS per
// liquid cell in a sparse aux layer beside the voxel pool: the CA
// (sim_step.wgsl) moves it with every liquid move and dissolves / precipitates
// / converts it; this file gives the layer its pages (solWant, solArgs,
// solAlloc -- before the CA), spreads it (solDiffuse), cleans and compacts it
// (solCompact -- after the CA) and hashes it (solHash -- hash ticks).
//
// Integer-only (rule 1). The only atomics whose ORDER could matter are the free
// stack's and the want list's, and both only choose memory addresses (which
// page, which list position) -- never a cell's value, which is all that is
// hashed, saved or read by a rule.

@group(0) @binding(0) var<storage, read> voxels : array<u32>;
// solDiffuse reads its own chunk's reasons (DIRTY_R_SOLBACK) off this tick's set.
@group(0) @binding(1) var<storage, read> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
// solDiffuse's axis (colorPhase.x of the DYN_CA slice it was recorded with).
@group(0) @binding(5) var<uniform> P : PassParams;
@group(0) @binding(8) var<storage, read_write> worldHash : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(40) var<storage, read_write> solTable : array<u32>;
@group(0) @binding(41) var<storage, read_write> solPool : array<atomic<u32>>;
@group(0) @binding(42) var<storage, read_write> solMeta : array<atomic<u32>>;
@group(0) @binding(43) var<storage, read> solSpec : array<u32>;

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
// The first fault's detail (world.h kSolMFaultCell/Entry/Value).
const SOLM_FAULT_CELL : u32 = 28u;
const SOLM_FAULT_ENTRY : u32 = 29u;
const SOLM_FAULT_VALUE : u32 = 30u;
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
// The first fault also records WHO and WHAT (SubmitTick prints it): the
// cell, the writing module (SOL_WRITER, declared beside each copy of this
// block), the table entry it met and the value it tried to store -- a bare
// slot named the chunk but not the writer, and the writers are many.
fn solFault(slot : u32, c : vec3<i32>, e : u32, v : u32) {
  let prev = atomicAdd(&solMeta[SOLM_FAULTS], 1u);
  if (prev == 0u) {
    atomicStore(&solMeta[SOLM_FAULT_SLOT], slot + 1u);
    atomicStore(&solMeta[SOLM_FAULT_TICK], T.tick);
    atomicStore(&solMeta[SOLM_FAULT_CELL], solLocalOf(c) | (SOL_WRITER << 16u));
    atomicStore(&solMeta[SOLM_FAULT_ENTRY], e);
    atomicStore(&solMeta[SOLM_FAULT_VALUE], v);
  }
}
fn solStoreAt(c : vec3<i32>, v : u32) -> bool {
  let slot = voxSlotOfCell(c);
  let e = solTable[slot];
  let local = solLocalOf(c);
  if ((e & SOL_PAGE_BIT) == 0u) {
    if (solCellValue(e, local) == v) { return true; }
    solFault(slot, c, e, v);
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
fn solFloor(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 4u] & 0xFFFFu; }
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
// This module's id in a solute store fault record (solFault): 1 sim_step,
// 2 sim_solute, 3 sim_mutate, 4 sim_elec, 5 sim_fluid_seam.
const SOL_WRITER : u32 = 2u;

// ============================================================================
// solWant: which chunks must be pages before the CA runs.
//
// One group per DIRTY chunk D (the CA's own compacted list). If D asked for a
// page last tick (a dissolve that found none -- SOLM_REQ_FLAG) or any chunk of
// N27(D) carries solute, every resident chunk of N27(D) goes on the want list.
// That is the whole write reach of the tick: the CA and the diffusion act only
// in dirty chunks and write at most one cell away, and they write solute only
// where solute already is (or where a dissolve asked for it).
//
// The first marker of a slot appends it (atomicOr on its flag returns 0 exactly
// once), so the list holds each slot once and its ORDER is scheduling-
// dependent. Nothing downstream depends on the order: solAlloc gives each
// listed slot the same answer whichever group it lands in.
// ============================================================================
var<workgroup> wantAny : atomic<u32>;

fn solNeighbourSlot(wc : vec3<i32>, li : u32) -> u32 {
  let d = vec3<i32>(i32(li % 3u) - 1, i32((li / 3u) % 3u) - 1, i32(li / 9u) - 1);
  return chunkSlotOf(wc + d, T.origin);
}

@compute @workgroup_size(32)
fn solWant(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let ci = dirtyList[wg.x];
  if (li == 0u) { atomicStore(&wantAny, 0u); }
  workgroupBarrier();
  let wc = slotWorldChunk(ci, T.origin);
  var sl = SLOT_NONE;
  if (li < 27u) {
    sl = solNeighbourSlot(wc, li);
    if (sl != SLOT_NONE && solTable[sl] != 0u) { atomicOr(&wantAny, 1u); }
  }
  // Only D's own group ever touches D's request flag in this pass; the CA
  // sets it LATER in the tick, for the NEXT solWant.
  if (li == 0u && atomicLoad(&solMeta[SOLM_REQ_FLAG + ci]) != 0u) {
    atomicStore(&solMeta[SOLM_REQ_FLAG + ci], 0u);
    atomicOr(&wantAny, 1u);
  }
  workgroupBarrier();
  if (atomicLoad(&wantAny) == 0u || sl == SLOT_NONE) { return; }
  if (atomicOr(&solMeta[SOLM_WANT_FLAG + sl], 1u) == 0u) {
    let at = atomicAdd(&solMeta[SOLM_WANT_COUNT], 1u);
    atomicStore(&solMeta[SOLM_WANT_LIST + at], sl);
  }
}

// solArgs: the want count becomes solAlloc's (and solCompact's) indirect
// args -- one group per listed slot -- and the cursor starts again at 0 for
// next tick. The list itself stays readable until the next solWant.
@compute @workgroup_size(1)
fn solArgs() {
  let n = atomicLoad(&solMeta[SOLM_WANT_COUNT]);
  atomicStore(&solMeta[SOLM_ARGS], n);
  atomicStore(&solMeta[SOLM_ARGS + 1u], 1u);
  atomicStore(&solMeta[SOLM_ARGS + 2u], 1u);
  atomicStore(&solMeta[SOLM_WANT_COUNT], 0u);
}

// ============================================================================
// solAlloc: give every listed slot a page, filled from the sentinel it had.
//
// A slot that already holds a page keeps it. Otherwise one page comes off the
// free stack (atomicSub on its depth: concurrent pops get distinct indices)
// and all 2048 words are written with the value the sentinel implied -- zero
// for EMPTY, the repeated cell value for UNIFORM -- so no reader can ever tell
// the page from the sentinel it replaced. An empty stack is POOL EXHAUSTION:
// counted, never silently absorbed, and fatal on the CPU
// (SubmitTick, support.cpp): which slots lost the race would be
// scheduling-dependent, and a slot without its page would drop writes.
//
// A page made from an EMPTY entry leaves SOL_WANT_FRESH in the slot's want
// flag (solWant's dedupe word, which this pass zeroed anyway): "this chunk
// carried nothing when the tick began". solCompact reads it and zeroes it --
// same list, same tick -- so the next solWant still finds every flag 0. See
// solCompact's THE STALL CLOCK STARTS WHEN MASS ARRIVES for what it decides.
// ============================================================================
var<workgroup> allocPage : u32;
var<workgroup> allocFill : u32;
const SOL_WANT_FRESH : u32 = 2u;

@compute @workgroup_size(64)
fn solAlloc(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    let slot = atomicLoad(&solMeta[SOLM_WANT_LIST + wg.x]);
    var flag = 0u;
    allocPage = 0xFFFFFFFFu;
    allocFill = 0u;
    let e = solTable[slot];
    if ((e & SOL_PAGE_BIT) == 0u) {
      let old = atomicSub(&solMeta[SOLM_FREE], 1u);
      if (old == 0u || old > SOL_POOL_PAGES) {
        atomicAdd(&solMeta[SOLM_FREE], 1u);
        atomicAdd(&solMeta[SOLM_EXHAUSTED], 1u);
      } else {
        let page = atomicLoad(&solMeta[SOLM_STACK + old - 1u]);
        solTable[slot] = SOL_PAGE_BIT | page;
        allocPage = page;
        var v = 0u;
        if ((e & SOL_UNIFORM_BIT) != 0u) { v = e & 0xFFFFu; }
        allocFill = v | (v << 16u);
        if (e == 0u) { flag = SOL_WANT_FRESH; }
        atomicMax(&solMeta[SOLM_HIGH_WATER], SOL_POOL_PAGES - (old - 1u));
      }
    }
    atomicStore(&solMeta[SOLM_WANT_FLAG + slot], flag);
  }
  workgroupBarrier();
  let page = workgroupUniformLoad(&allocPage);
  if (page == 0xFFFFFFFFu) { return; }
  let fill = workgroupUniformLoad(&allocFill);
  let base = page * SOL_WORDS_PER_PAGE;
  for (var i = li; i < SOL_WORDS_PER_PAGE; i = i + 64u) {
    atomicStore(&solPool[base + i], fill);
  }
}

// ============================================================================
// THE HELD FLOW CARRIES WHAT IS DISSOLVED IN IT (ticket 2026-10-16_11-11-51;
// docs/PLAN_solutes.md §3.5, DESIGN.md §5b.13).
//
// A map river, a waterfall's crest and curtain and an adopted stream are HELD
// water (sim_step.wgsl HELD WATER): real voxels at rest, which the CA never
// moves. The CA is what carries solute (every liquid move takes its mass
// along), so mass dissolved in a held river only DIFFUSED -- blood poured at
// the top of a waterfall stayed at the top. Here the pair exchange learns
// which way the held water is going and moves mass that way.
//
// THE POTENTIAL. Every liquid cell gets a place in one total preorder:
//   * a cell of a column with no held target for its liquid (STILL_UNHELD:
//     no record, another liquid, the reservoir off) is LOWEST of all;
//   * a held cell ranks by (T8, r, h), lexicographically: T8 its column's
//     target; r its column's DROP DISTANCE (solDropDist) -- how many columns
//     along an axis, through held columns at its own T8, to one that stands
//     SOL_FALL_DROP8 or more eighths under it: 1 for a FALL column (the
//     curtain's outer strip against the reach below, a stream's step), 2..
//     SOL_DROP_REACH for the inner curtain strip, the crest and the last
//     voxels of the reach above it, SOL_DROP_REACH + 1 for every other
//     column (a lake, the sea, the middle of a reach); h its own y in a fall
//     column and 0 in any other.
// A pair whose two cells rank EQUAL exchanges by plain diffusion, as it always
// did (a lake, a flat reach, a vertical pair outside a fall). A pair whose
// cells rank differently moves mass ONE WAY, from the higher to the lower:
//   * both held -- CARRIED (solCarry): SOL_HELD_CARRY / 256 of the upper
//     cell's mass a visit, whatever the lower holds, up to the lower cell's
//     saturation. Laterally that is down the surface (a ramp, a rapid: the
//     smaller T8) and, at one T8, toward the drop (the approach, over the
//     crest, across the curtain: the smaller r); vertically, in a fall column
//     only, it is DOWN the curtain to its foot, where the reach below (a
//     smaller T8) takes it on sideways;
//   * held over unheld -- the plain diffusive amount, but only if it runs
//     from the held cell into the unheld one.
// All four strides take part (a stride pair is still connected only through
// its solvent), so a plume crosses a terrace of equal T8 by the long pairs.
//
// TERMINATION (rule 2). Let PHI = SUM over cells of mass x rank (the cell's
// index in the preorder) and D = SUM mass^2 / fullness. A carry or a one-way
// diffusion moves at least one unit to a strictly lower rank: PHI falls by
// at least one. An equal-rank exchange leaves PHI alone and strictly lowers
// D (the half-down rounding, solDiffuse's argument). The fade and the floor
// only remove mass. So (PHI, D) falls lexicographically with every write and
// is bounded below: nothing can cycle, and when every unit stands where no
// lower-ranked neighbour has room -- the foot of the river in the window, a
// lake at the end of it, unheld water -- only d == 0 diffusion is left and
// the chunks stall and sleep as before. WHY UNHELD IS LOWEST and not just
// "level": an unheld pocket dug beside a curtain touches it at many heights;
// plain diffusion through the pocket would lift what the curtain carried down
// and the pair of them would run for ever. One way into it, it cannot.
// The window edge is not special: a pair whose partner is not resident is no
// pair, so mass reaching the downstream face stops there (bounded by the
// saturation cap) and stalls like any other.
//
// RULE 1. Pure reads of the still table (written by worldgen `cols` and
// sim_stream before this pass; pass_table.def orders it), integer arithmetic,
// the rounding byte a counter hash of (seed, tick, the owner cell), the same
// single-owner pairs as the diffusion.
//
// STILL_WORDS / stillIndex / stillTag / STILL_UNHELD / stillLevel8 below are
// sim_step.wgsl's, copied TEXT-IDENTICAL (check_invariants.py `stilllevel`
// fails if they drift): the carry must agree with the held rule about which
// columns are held and where their surfaces stand.
// ============================================================================
@group(0) @binding(58) var<storage, read> stillWater : array<u32>;
const STILL_WORDS : u32 = 4u;
fn stillIndex(x : i32, z : i32) -> u32 {
  return (bitcast<u32>(z & WORLD_MASK) * WORLD_N + bitcast<u32>(x & WORLD_MASK)) * STILL_WORDS;
}
fn stillTag(x : i32, z : i32) -> u32 {
  let tx = bitcast<u32>(x >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  let tz = bitcast<u32>(z >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  return 1u | (tx << 1u) | (tz << 16u);
}
const STILL_UNHELD : i32 = -1073741824;
fn stillLevel8(x : i32, z : i32, mat : u32) -> i32 {
  if (T.stillWaterRelax == 0u) { return STILL_UNHELD; }
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z)) { return STILL_UNHELD; }
  let v = stillWater[si + 1u];
  if (v != 0u) {
    if ((v >> 20u) != mat) { return STILL_UNHELD; }
    return bitcast<i32>(v << 12u) >> 12u;
  }
  let d = stillWater[si + 2u];
  if (d == 0u || (stillWater[si + 3u] & 0xFFFu) != mat) { return STILL_UNHELD; }
  return bitcast<i32>(d << 12u) >> 12u;
}

// The share of the upper cell's mass a carried pair moves per visit, of 256.
// Every pair is visited once in 8 ticks per stride (24 phases), so at 96 a
// unit at the top of a fall is most of the way down a 20-voxel curtain in a
// few seconds, and a plume runs down a ramp as a smear, not a slug.
const SOL_HELD_CARRY : u32 = 96u;
// A DROP: a held column of the same liquid at least this many eighths (one
// voxel) under a column's target. Not "any lower column": a ramp falls an
// eighth every few columns, and every ramp column a fall column would sink
// everything dissolved in a river onto its bed (a ramp's carry is its T8).
const SOL_FALL_DROP8 : i32 = 8;
// How far along an axis a column looks for a drop: RIVER_CREST +
// RIVER_CURTAIN (worldgen.wgsl), so a waterfall's crest and both curtain
// strips are graded toward it, and the stride pairs (up to 8) reach on from
// there into the flat approach. Each extra column is four still-table lookups
// for every pair with mass at one T8 -- a blood cloud in a lake pays them.
const SOL_DROP_REACH : i32 = 4;
// The column's drop distance (THE POTENTIAL, above), reach + 1 when there is
// none within `reach` (SOL_DROP_REACH; 1 when only "is it a fall column"
// matters, a quarter of the lookups). A ray stops at the first column that is not held
// at exactly this target -- a bank, another liquid, a ramp's step (the T8
// order already carries across that one).
fn solDropDist(x : i32, z : i32, mat : u32, t8 : i32, reach : i32) -> i32 {
  var best = reach + 1;
  for (var a = 0; a < 4; a++) {
    let dx = select(0, select(-1, 1, a == 0), a < 2);
    let dz = select(0, select(-1, 1, a == 2), a >= 2);
    for (var j = 1; j < best; j++) {
      let t = stillLevel8(x + dx * j, z + dz * j, mat);
      if (t == STILL_UNHELD) { break; }
      if (t <= t8 - SOL_FALL_DROP8) { best = j; break; }
      if (t != t8) { break; }
    }
  }
  return best;
}
// Where the pair's two liquid cells stand on the potential above: 0 = level
// (plain diffusion), +-1 = held over unheld (one-way diffusion), +-2 = both
// held, carried. Positive: A ranks higher, mass runs A -> B. cB is cA plus a
// positive axis step, so a vertical pair's B is the UPPER cell.
fn solHeldOrder(cA : vec3<i32>, mA : u32, cB : vec3<i32>, mB : u32) -> i32 {
  let tA = stillLevel8(cA.x, cA.z, mA);
  let tB = stillLevel8(cB.x, cB.z, mB);
  let hA = tA != STILL_UNHELD;
  let hB = tB != STILL_UNHELD;
  if (!hA && !hB) { return 0; }
  if (!hB) { return 1; }
  if (!hA) { return -1; }
  if (tA != tB) { return select(-2, 2, tA > tB); }
  if (cA.x == cB.x && cA.z == cB.z) {
    // One column (one record, so one liquid): down, in a fall column.
    return select(0, -2, solDropDist(cA.x, cA.z, mA, tA, 1) == 1);
  }
  let rA = solDropDist(cA.x, cA.z, mA, tA, SOL_DROP_REACH);
  // Side by side at one T8 and one y: the nearer the drop ranks lower; two
  // at the same distance are level (two fall cells at one y included).
  let rB = solDropDist(cB.x, cB.z, mB, tB, SOL_DROP_REACH);
  if (rA == rB) { return 0; }
  return select(-2, 2, rA > rB);
}

// A CARRIED pair (both held, A ranks over B when aToB): 0 = nothing to do,
// 1 = work pending, 3 = wrote -- solPairExchange's convention. "Nothing to
// do" is EXACT: the upper cell is empty or the lower one is at its
// saturation, which no later visit of this pair can change by itself.
fn solCarry(cA : vec3<i32>, cB : vec3<i32>, step : vec3<i32>, k : u32, write : bool,
            s : u32, aToB : bool, fA : u32, fB : u32, xA : u32, xB : u32) -> u32 {
  let xh = select(xB, xA, aToB);
  let xl = select(xA, xB, aToB);
  if (xh == 0u) { return 0u; }
  let cap = min(solCap(s, select(fA, fB, aToB)), 255u);
  if (xl >= cap) { return 0u; }
  for (var i = 1u; i < k; i = i + 1u) {
    if (!solIsSolvent(voxMat(voxWordAt(cA + step * i32(i))), s)) { return 0u; }
  }
  if (!write) { return 1u; }
  // Stochastic rounding, as the diffusion's: a single unit still moves (with
  // chance SOL_HELD_CARRY / 256 a visit) instead of stalling the tail.
  let rnd = hash3(T.seed ^ 0xCA221EDu, T.tick, cellIndexW(cA)) & 255u;
  let q = min(min((xh * SOL_HELD_CARRY + rnd) >> 8u, xh), cap - xl);
  if (q == 0u) { return 1u; }
  if (!solWritable(cA) || !solWritable(cB)) { return 1u; }
  if (aToB) {
    _ = solStoreAt(cA, solPack(s, xA - q));
    _ = solStoreAt(cB, solPack(s, xB + q));
  } else {
    _ = solStoreAt(cA, solPack(s, xA + q));
    _ = solStoreAt(cB, solPack(s, xB - q));
  }
  return 3u;
}

// THE STEP DOWN. A held surface that falls a whole cell from one column to the
// next ON A SILL -- a waterfall's crest (one cell of water on the stone, the
// curtain's top a cell lower: worldgen THE BRINK), the step of an adopted
// stream one cell deep -- has no axis-aligned liquid pair across it: beside
// the wet cell is air, and under it is the sill. Mass there would stall at the
// lip for ever. So a stride-1 LATERAL pair with one cell wet (hi) and one dry
// carries from hi into the cell UNDER the dry one (lo), when
//   * lo is the same liquid and hi outranks it (a carry, THE POTENTIAL),
//   * the cell under hi is NOT liquid (the sill).
// SINGLE OWNER, still. lo is the partner cell of the pair one row down,
// (hi - y, lo) along the same axis -- and that pair is inert, because its
// other cell (under hi) is not liquid: it may only try a step itself, from lo
// into the cell under hi's under-cell, which needs lo's column to outrank
// hi's -- the opposite of ours, so at most one of them fires. The row above
// may step INTO hi only from the dry cell's column over it, which needs that
// column to outrank hi's: again the opposite. Values are read only after the
// order test, so a pair that will not fire reads nothing another thread
// writes. A lo in another chunk (the row below crosses a chunk face) is woken.
// The move is a carry to a strictly lower rank: the termination argument
// above covers it unchanged.
fn solStepCarry(cA : vec3<i32>, cB : vec3<i32>, wA : u32, wB : u32, aWet : bool, write : bool) -> u32 {
  let hi = select(cB, cA, aWet);
  let dry = select(cA, cB, aWet);
  let mh = voxMat(select(wB, wA, aWet));
  let down = vec3<i32>(0, 1, 0);
  let lo = dry - down;
  if (!cellResident(lo, T.origin)) { return 0u; }
  let wl = voxWordAt(lo);
  if (voxMat(wl) != mh) { return 0u; }
  let under = hi - down;
  if (cellResident(under, T.origin) && solIsLiquidMat(voxMat(voxWordAt(under)))) { return 0u; }
  if (solHeldOrder(hi, mh, lo, mh) != 2) { return 0u; }
  let vh = solValueAt(hi);
  if (vh == 0u) { return 0u; }
  let vl = solValueAt(lo);
  let sh = solSpeciesOf(vh);
  let sl = solSpeciesOf(vl);
  if (sl != 0u && sl != sh) { return 0u; }
  if (!solIsSolvent(mh, sh)) { return 0u; }
  let r = solCarry(hi, lo, vec3<i32>(0), 1u, write, sh, true, voxState(select(wB, wA, aWet)) + 1u,
                   voxState(wl) + 1u, solMassOf(vh), solMassOf(vl));
  if (r == 3u) {
    let ls = chunkSlotOf(worldChunkOf(lo), T.origin);
    if (ls != SLOT_NONE) { atomicOr(&dirtyOut[ls], DIRTY_R_SOLUTE); }
  }
  return r;
}

// ============================================================================
// solDiffuse: PAIR-EXCHANGE diffusion (PLAN_solutes §3.3), at FOUR STRIDES.
//
// Three dispatches per tick, one per axis (DYN_CA hands iteration k the passUBO
// slice whose colorPhase.x is k). Along the axis a pair is (a, a + K) for a
// stride K = 1, 2, 4 or 8 and a parity p: the owner's coordinate is
//     a = 2K * (q / K) + (q % K) + p * K,   q = 0..7,
// which tiles a 16-cell line into 8 disjoint pairs; p = 0 pairs lie inside the
// chunk and p = 1 pairs straddle its +axis face. The tick picks (p, K): parity
// is tick & 1, stride 1 << ((tick >> 1) & 3), so 8 ticks visit all 24 phases.
// The LOWER cell owns the pair and writes both halves, pairs are disjoint, and
// the same integer leaves one cell and enters the other: exactly
// conservative, nothing scheduling-dependent in it.
//
//   d = (mA * fB - mB * fA) / (fA + fB)     the amount that equalises c = m*8/f
//   q = (|d| * rate + rnd8) >> 8            stochastic rounding (0 <= q <= |d|)
//
// WHY STRIDES. Integer pair exchange has a fixpoint at |mA - mB| <= 1 per PAIR,
// which is not "uniform": a ramp that climbs one unit a cell (0, 1, 2, ... 12
// across a pond) has d == 0 everywhere and is a perfectly stable resting state.
// Measured, the first run of `solute`: concentration 0..13 across a 12-cell
// pond, asleep. A stride-K pair sees K steps of that ramp at once, so the
// fixpoint over all four strides is a slope under 1/8 of a unit a cell -- and
// the long strides also spread a plume in O(L) dispatches rather than O(L^2).
// A stride pair only exchanges THROUGH its solvent: every cell between the two
// ends must be a solvent of the species, so mass never jumps a wall.
//
// Stochastic rounding keeps a small rate from stalling (plain truncation makes
// any |d| < 256/rate a fixpoint); the rounding byte is a counter hash of
// (seed, tick, the owner's SLOT cell index), as deterministic as the CA.
//
// THE d == 0 FIXPOINT IS LOAD-BEARING: it neither writes nor counts as work, so
// a mixed body finds nothing and sleeps. A chunk keeps itself awake only while
// some pair touching it has d != 0, plus SOL_STALL_TICKS idle dispatches (all
// 24 phases) to be sure.
//
// A p = 1 pair whose OWNER is in the -axis neighbour is checked here too (read
// only): if it has work the owner is marked dirty -- the owner may be asleep,
// and a gradient across the face would otherwise never move.
//
// In HELD water a pair across the held flow's potential is not diffused but
// CARRIED one way (THE HELD FLOW CARRIES WHAT IS DISSOLVED IN IT, above):
// same pairs, same owner, same work / no-work convention, so the stall clock,
// the back-check and the wake-ups below need nothing new.
// ============================================================================
var<workgroup> difWork : atomic<u32>;
var<workgroup> difCross : atomic<u32>;
var<workgroup> difBack : atomic<u32>;
var<workgroup> difFront : atomic<u32>;

// 0 = nothing to do, 1 = work pending (d != 0) but nothing written, 3 = wrote.
// `k` is the stride: cA + k * step == cB.
fn solPairExchange(cA : vec3<i32>, cB : vec3<i32>, step : vec3<i32>, k : u32,
                   write : bool) -> u32 {
  if (!cellResident(cB, T.origin)) { return 0u; }
  let wA = voxWordAt(cA);
  let wB = voxWordAt(cB);
  let mA = voxMat(wA);
  let mB = voxMat(wB);
  let lA = solIsLiquidMat(mA);
  let lB = solIsLiquidMat(mB);
  if (!lA || !lB) {
    // One wet, one dry, side by side: perhaps a STEP DOWN (solStepCarry).
    if (k == 1u && step.y == 0 && lA != lB) { return solStepCarry(cA, cB, wA, wB, lA, write); }
    return 0u;
  }
  let vA = solValueAt(cA);
  let vB = solValueAt(cB);
  if (vA == 0u && vB == 0u) { return 0u; }
  let sA = solSpeciesOf(vA);
  let sB = solSpeciesOf(vB);
  // Two species do not mix in one cell (PLAN_solutes §2.3: immiscible is the
  // safe, deterministic default).
  if (sA != 0u && sB != 0u && sA != sB) { return 0u; }
  let s = max(sA, sB);
  if (!solIsSolvent(mA, s) || !solIsSolvent(mB, s)) { return 0u; }
  let fA = voxState(wA) + 1u;
  let fB = voxState(wB) + 1u;
  let xA = solMassOf(vA);
  let xB = solMassOf(vB);
  // THE HELD FLOW (above): a pair across its potential is carried, or
  // diffuses one way only.
  let ord = solHeldOrder(cA, mA, cB, mB);
  if (ord == 2 || ord == -2) {
    return solCarry(cA, cB, step, k, write, s, ord > 0, fA, fB, xA, xB);
  }
  // The equalising amount, ROUNDED HALF DOWN rather than truncated: with
  // truncation a one-unit film over a clean full cell (8 : 0 in concentration)
  // is a fixpoint -- measured, the last unit of a pinch in a pond sat on the
  // surface for the rest of the run. Half-down still leaves |remaining| <= 1/2
  // after a move, which is exactly the termination argument (the potential
  // SUM(m*m/f) strictly decreases), and ties -- two full cells one unit apart
  // -- stay put, so it cannot oscillate. For equal fullness it is identical
  // to (dm)/2 truncated.
  let num = i32(xA * fB) - i32(xB * fA);
  let den = i32(fA + fB);
  var d = (2 * abs(num) + den - 1) / (2 * den);
  if (num < 0) { d = -d; }
  if (d == 0) { return 0u; }
  // Held over unheld: only down the potential, out of the held cell.
  if (ord != 0 && (d > 0) != (ord > 0)) { return 0u; }
  // A stride pair is connected only through its solvent.
  for (var i = 1u; i < k; i = i + 1u) {
    if (!solIsSolvent(voxMat(voxWordAt(cA + step * i32(i))), s)) { return 0u; }
  }
  if (!write) { return 1u; }
  let mag = u32(abs(d));
  let rate = (solDiffusivity(s) * TUNE_SOLUTE_DIFFUSION) / 100u;
  let rnd = hash3(T.seed ^ 0xD1FF5A17u, T.tick, cellIndexW(cA)) & 255u;
  var q = min((mag * rate + rnd) >> 8u, mag);
  if (d > 0) { q = min(q, 255u - xB); } else { q = min(q, 255u - xA); }
  if (q == 0u) { return 1u; }
  if (!solWritable(cA) || !solWritable(cB)) { return 1u; }
  if (d > 0) {
    _ = solStoreAt(cA, solPack(s, xA - q));
    _ = solStoreAt(cB, solPack(s, xB + q));
  } else {
    _ = solStoreAt(cA, solPack(s, xA + q));
    _ = solStoreAt(cB, solPack(s, xB - q));
  }
  return 3u;
}

fn solAxisCell(axis : u32, a : u32, u : u32, v : u32) -> vec3<i32> {
  if (axis == 1u) { return vec3<i32>(vec3<u32>(u, a, v)); }
  if (axis == 2u) { return vec3<i32>(vec3<u32>(u, v, a)); }
  return vec3<i32>(vec3<u32>(a, u, v));
}

@compute @workgroup_size(256)
fn solDiffuse(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let ci = dirtyList[wg.x];
  if (li == 0u) {
    atomicStore(&difWork, 0u);
    atomicStore(&difCross, 0u);
    atomicStore(&difBack, 0u);
    atomicStore(&difFront, 0u);
  }
  // WOKEN BY A NEIGHBOUR'S BACK-CHECK (DIRTY_R_SOLBACK): this chunk owns a
  // pending pair across its +axis face in a phase nobody can name from here,
  // so it restarts its stall clock and stays awake a whole cycle (24 phases).
  // Waking it for ONE tick was measured to land it two ticks later -- on the
  // NEXT stride, every time -- and the gradient never moved. Its own counter,
  // written only by its own group, once a tick (the first axis dispatch).
  if (li == 0u && P.colorPhase.x == 0u && (dirtyIn[ci] & DIRTY_R_SOLBACK) != 0u) {
    atomicStore(&solMeta[SOLM_STALL + ci], 0u);
  }
  workgroupBarrier();
  let e = solTable[ci];
  let wc = slotWorldChunk(ci, T.origin);
  let base = wc * i32(CHUNK);
  let axis = P.colorPhase.x;
  let parity = T.tick & 1u;
  let k = 1u << ((T.tick >> 1u) & 3u);
  let step = axisVecI(i32(axis), 1);
  let reach = step * i32(k);
  if (e != 0u) {
    var work = 0u;
    for (var j = 0u; j < 8u; j = j + 1u) {
      let pi = li + j * 256u;              // pair index, 0..2047
      let q = pi & 7u;
      let a = 2u * k * (q / k) + (q % k) + parity * k;
      let cA = base + solAxisCell(axis, a, (pi >> 3u) & 15u, pi >> 7u);
      let r = solPairExchange(cA, cA + reach, step, k, true);
      work = work | r;
      if (r == 3u && a + k >= CHUNK) { atomicOr(&difCross, 1u); }
      // A face pair with work restarts the PARTNER's stall clock (next tick,
      // through DIRTY_R_SOLBACK): the partner no longer reads this pair itself
      // while this group is writing it -- see the back-check below.
      if (r != 0u && a + k >= CHUNK) { atomicOr(&difFront, 1u); }
    }
    // The face this chunk is the PARTNER of: parity-1 pairs owned by the -axis
    // neighbour (their upper cells are this chunk's first k layers). Read
    // only; a pending one wakes its owner.
    //
    // ONLY WHILE THE OWNER IS NOT WRITING THEM IN THIS SAME DISPATCH (rule 1).
    // An owner that is on this tick's dirty list and carries solute evaluates
    // and WRITES exactly these pairs, from another workgroup, concurrently: a
    // read here then sees the pair before or after the exchange depending on
    // scheduling, and its answer (work / no work) decided this chunk's stall
    // counter -- so whether the dilution floor discarded a unit in solCompact.
    // Measured 2026-09-27: `determinism` diverged between its two runs at tick
    // 85 (parity 1, stride 4) on exactly one slot, stall 78 vs 0, 2 units
    // floored in one run and kept in the other. A running owner needs no
    // wake-up, and it restarts this chunk's clock itself (difFront above);
    // an owner that is asleep, or carries nothing, writes nothing here, so
    // the read is stable and the back-check stays as it was.
    let os = chunkSlotOf(wc - step, T.origin);
    let ownerWrites = os != SLOT_NONE && dirtyIn[os] != 0u && solTable[os] != 0u;
    if (parity == 1u && !ownerWrites) {
      for (var j = 0u; j < k; j = j + 1u) {
        let cB = base + solAxisCell(axis, j, li & 15u, li >> 4u);
        let cA = cB - reach;
        if (cellResident(cA, T.origin) && solPairExchange(cA, cB, step, k, false) != 0u) {
          atomicOr(&difBack, 1u);
          work = work | 1u;
        }
      }
    }
    if (work != 0u) { atomicOr(&difWork, 1u); }
  }
  workgroupBarrier();
  if (li != 0u || e == 0u) { return; }
  let st = SOLM_STALL + ci;
  if (atomicLoad(&difWork) != 0u) {
    atomicStore(&solMeta[st], 0u);
    atomicOr(&dirtyOut[ci], DIRTY_R_SOLUTE);
  } else {
    let s0 = atomicLoad(&solMeta[st]);
    atomicStore(&solMeta[st], min(s0 + 1u, 255u));
    // Stay awake until a full cycle of phases has been idle.
    if (s0 + 1u < SOL_STALL_TICKS) { atomicOr(&dirtyOut[ci], DIRTY_R_SOLUTE); }
  }
  if (atomicLoad(&difCross) != 0u) {
    let ps = chunkSlotOf(wc + step, T.origin);
    if (ps != SLOT_NONE) { atomicOr(&dirtyOut[ps], DIRTY_R_SOLUTE); }
  }
  if (atomicLoad(&difBack) != 0u) {
    let os = chunkSlotOf(wc - step, T.origin);
    if (os != SLOT_NONE) { atomicOr(&dirtyOut[os], DIRTY_R_SOLBACK); }
  }
  if (atomicLoad(&difFront) != 0u) {
    let ps = chunkSlotOf(wc + step, T.origin);
    if (ps != SLOT_NONE) { atomicOr(&dirtyOut[ps], DIRTY_R_SOLBACK); }
  }
}

// ============================================================================
// solCompact: clean, floor, aggregate and hand back (PLAN_solutes §3.3.1).
//
// One group per WANT-LIST slot: every chunk that could have been written this
// tick is on it (solWant put every writable chunk there), so a page that turns
// uniform is returned the same tick and a settled lake is table entries.
//
// Per cell, in order:
//   1. mass on a cell that is no longer a solvent of its species (a reaction or
//      an explosion replaced the liquid without the CA's help) is DISCARDED --
//      the matter went with the liquid that carried it;
//   2. in a STALLED chunk, mass below the species' dilution floor
//      (c = m * 8 / f < floor) is DISCARDED. Deliberately non-conservative
//      (§3.3.1): integer diffusion quantizes to a halt below one unit, and
//      without this a creature's worth of blood in a lake would freeze in
//      blotches forever. Only STALLED chunks, so a plume still spreading loses
//      nothing and a body held above the floor conserves exactly. Only
//      CONSERVED species: a fading one's floor is a per-cell erosion (3);
//   3. a species with a halfLife (solutes.json; blood) FADES: each cell loses
//      mass * decayQ / 65536 a tick, stochastically rounded, wherever it is --
//      spread or not, stalled or not. Also discarded and counted. A chunk
//      still holding fading mass keeps itself awake (DIRTY_R_SOLUTE) so the
//      fade runs to the end: bounded, since the mass only goes down. A cell
//      under the floor also loses one unit with SOL_FLOOR_FADE / 65536 a tick,
//      per cell, never chunk by chunk -- rule 2's cut drew a cube;
//   4. the chunk's aggregate (species, total mass) is republished;
//   5. a chunk whose 4096 values are all equal becomes SOL_UNIFORM (or
//      SOL_EMPTY when the value is 0) and its page goes back on the stack.
// Every discarded unit is counted (SOLM_DISCARDED), so the gates balance the
// ledger rather than trusting it.
//
// THE STALL CLOCK STARTS WHEN MASS ARRIVES, not when it leaves (ticket
// 2026-10-14_23-01-29, the 5x5 cube of awake chunks over a lake). The clock
// used to be zeroed whenever a chunk compacted back to EMPTY, so the dilution
// floor could not fire on mass arriving into a chunk a finished plume had
// stalled. But every DIRTY chunk in N27 of a carrier is on the want list, is
// handed a fresh all-zero page by solAlloc, and solDiffuse reads that page as
// "carries solute": no work, clock 0 -> 3 over the three axes, still under
// SOL_STALL_TICKS, so it marks itself DIRTY_R_SOLUTE -- and the compaction
// then found it EMPTY and zeroed the clock again. A self-sustaining loop: the
// sky, the rock and the clean water round a blood cloud stayed awake (and
// churned 27 pages each a tick) for as long as the cloud lasted, and all
// went to sleep on the tick its last unit faded. Measured, --shot-aquatic
// piranha*6+human:dead: 191 EMPTY chunks awake, "solute" their only reason.
// Now a page solAlloc made from EMPTY (SOL_WANT_FRESH) is not "stalled"
// whatever its clock says -- the clock describes earlier mass, so nothing it
// receives this tick is floored -- and the clock restarts only if mass did
// arrive. A chunk that stays empty keeps counting, and after one cycle of
// phases stops marking itself, like any other idle chunk.
// ============================================================================
var<workgroup> cmpSlot : u32;
var<workgroup> cmpEntry : u32;
var<workgroup> cmpFresh : u32;
var<workgroup> cmpMixed : atomic<u32>;
var<workgroup> cmpMass : atomic<u32>;
var<workgroup> cmpSpecies : atomic<u32>;
var<workgroup> cmpFading : atomic<u32>;

// The species' FADE: per-tick loss fraction x 65536, the high half of the
// floor word (Simulation::UploadSolutes; solutes.h decayQ). 0 = conserved.
// Declared here, beside its only reader, not in the mirrored block.
fn solDecayQ(s : u32) -> u32 { return solSpec[SOLS_BASE + s * SOLS_STRIDE + 4u] >> 16u; }
// A fading species' sub-floor erosion: the chance x 65536 a tick that a cell
// under the floor loses one unit (256 = 1/256: ~8.5 s a unit at 30 Hz). It
// must stay well under the diffusion's own pace: at 2048 (1/32) it ate a
// bleeding stump's halo as fast as the pair exchange pushed it out, and the
// cloud sat round the body as ~120 cells of speckle instead of spreading
// (SANDVOX_STUMP_PROBE=1 --gate stump-underwater, 2026-10-11: 0 -> ~3000
// cells, 128 -> ~1700, 256 -> ~1000-1700, 512 -> ~450, 2048 -> ~120). The
// floor of it is the clear-up time: solute-blood's cloud is gone at tick
// 2520 at 256, 4740 at 128 (over the gate's 4500).
const SOL_FLOOR_FADE : u32 = 256u;

fn solCellOfLocal(base : vec3<i32>, local : u32) -> vec3<i32> {
  return base + vec3<i32>(i32(local & 15u), i32((local >> 4u) & 15u), i32(local >> 8u));
}

@compute @workgroup_size(256)
fn solCompact(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    let slot = atomicLoad(&solMeta[SOLM_WANT_LIST + wg.x]);
    cmpSlot = slot;
    cmpEntry = solTable[slot];
    // solAlloc's mark, read and cleared by the one group that owns the slot
    // (the list holds it once): next tick's solWant dedupes on a zero word.
    cmpFresh = atomicLoad(&solMeta[SOLM_WANT_FLAG + slot]);
    atomicStore(&solMeta[SOLM_WANT_FLAG + slot], 0u);
    atomicStore(&cmpMixed, 0u);
    atomicStore(&cmpMass, 0u);
    atomicStore(&cmpSpecies, 0u);
    atomicStore(&cmpFading, 0u);
  }
  workgroupBarrier();
  let slot = workgroupUniformLoad(&cmpSlot);
  let e = workgroupUniformLoad(&cmpEntry);
  let fresh = workgroupUniformLoad(&cmpFresh) == SOL_WANT_FRESH;
  if ((e & SOL_PAGE_BIT) == 0u) {
    // A sentinel: nothing to clean or hand back, only the aggregate. (A want
    // slot is a sentinel here only when solAlloc found the pool exhausted,
    // which SubmitTick treats as fatal.)
    if (li == 0u) {
      var agg = 0u;
      if (e != 0u) { agg = (solSpeciesOf(e & 0xFFFFu) << 24u) | (solMassOf(e & 0xFFFFu) * CHUNK_VOL); }
      atomicStore(&solMeta[SOLM_AGG + slot], agg);
      if (e == 0u) { atomicStore(&solMeta[SOLM_STALL + slot], 0u); }
    }
    return;
  }
  let page = e & SOL_PAGE_MASK;
  let pbase = page * SOL_WORDS_PER_PAGE;
  // A FRESH page's clock belongs to whatever left it, not to what arrived
  // (THE STALL CLOCK STARTS WHEN MASS ARRIVES, above): never stalled.
  let stalled = !fresh && atomicLoad(&solMeta[SOLM_STALL + slot]) >= SOL_STALL_TICKS;
  let cbase = slotWorldChunk(slot, T.origin) * i32(CHUNK);
  var mass = 0u;
  var species = 0u;
  var discarded = 0u;
  var fading = false;
  for (var j = 0u; j < 8u; j = j + 1u) {
    let wi = li + j * 256u;
    let w = atomicLoad(&solPool[pbase + wi]);
    var nw = 0u;
    for (var h = 0u; h < 2u; h = h + 1u) {
      var v = (w >> (h * 16u)) & 0xFFFFu;
      if (v != 0u) {
        let c = solCellOfLocal(cbase, wi * 2u + h);
        let vw = voxWordAt(c);
        let m = voxMat(vw);
        let s = solSpeciesOf(v);
        let dq = solDecayQ(s);
        if (!solIsLiquidMat(m) || !solIsSolvent(m, s)) {
          discarded += solMassOf(v);
          v = 0u;
        } else if (dq == 0u && stalled && solMassOf(v) * 8u < solFloor(s) * (voxState(vw) + 1u)) {
          discarded += solMassOf(v);
          v = 0u;
        }
        // 3. the fade. Keyed by the SLOT cell index (an identity, never the
        // page), so it is as deterministic as the diffusion's rounding.
        if (v != 0u && dq != 0u) {
          let mm = solMassOf(v);
          let hv = hash3(T.seed ^ 0xFADEB10Du, T.tick, slot * CHUNK_VOL + wi * 2u + h);
          var loss = min((mm * dq + (hv & 0xFFFFu)) >> 16u, mm);
          // A FADING species' floor is a per-cell fade, not rule 2's cut. The
          // cut zeroes every sub-floor cell of a chunk on the tick its stall
          // clock runs out, and the blood column the renderer integrates made
          // that a 16^3 cube of cloud blinking out (owner, 2026-10-11). Here a
          // cell under the floor loses one more unit with SOL_FLOOR_FADE/65536
          // a tick wherever it is, stalled or not: whether a cell erodes
          // depends on its own concentration only, so the thin rim of a cloud
          // thins away smoothly across chunk faces. Still bounded: the mass
          // only goes down.
          if (mm - loss != 0u && (mm - loss) * 8u < solFloor(s) * (voxState(vw) + 1u) &&
              (hv >> 16u) < SOL_FLOOR_FADE) {
            loss += 1u;
          }
          discarded += loss;
          v = solPack(s, mm - loss);
          if (v != 0u) { fading = true; }
        }
      }
      mass += solMassOf(v);
      species = max(species, solSpeciesOf(v));
      nw = nw | (v << (h * 16u));
    }
    if (nw != w) { atomicStore(&solPool[pbase + wi], nw); }
  }
  if (discarded != 0u) { atomicAdd(&solMeta[SOLM_DISCARDED], discarded); }
  if (mass != 0u) { atomicAdd(&cmpMass, mass); }
  if (species != 0u) { atomicMax(&cmpSpecies, species); }
  if (fading) { atomicOr(&cmpFading, 1u); }
  storageBarrier();
  workgroupBarrier();
  // Uniform iff every word equals word 0 and word 0's halves agree.
  let w0 = atomicLoad(&solPool[pbase]);
  var mixed = (w0 & 0xFFFFu) != (w0 >> 16u);
  for (var j = 0u; j < 8u; j = j + 1u) {
    if (atomicLoad(&solPool[pbase + li + j * 256u]) != w0) { mixed = true; }
  }
  if (mixed) { atomicOr(&cmpMixed, 1u); }
  workgroupBarrier();
  if (li != 0u) { return; }
  let total = atomicLoad(&cmpMass);
  atomicStore(&solMeta[SOLM_AGG + slot],
              (atomicLoad(&cmpSpecies) << 24u) | min(total, 0xFFFFFFu));
  // MASS ARRIVED IN A CHUNK THAT CARRIED NONE: its stall clock starts now.
  // The count a finished plume left (e.g. 78) must not be what the dilution
  // floor reads on the new mass's next compaction -- before it has diffused
  // at all. (This used to be done the other way round, by zeroing the clock
  // whenever a chunk went EMPTY, and that is what made an empty neighbour of
  // a carrier keep itself awake forever: the header above.) Safe here:
  // solDiffuse (the only other writer during a tick) ran earlier in this
  // tick, and this group owns slot.
  if (fresh && total != 0u) { atomicStore(&solMeta[SOLM_STALL + slot], 0u); }
  // Fading mass left: awake next tick, so this pass runs on it again.
  if (atomicLoad(&cmpFading) != 0u) { atomicOr(&dirtyOut[slot], DIRTY_R_SOLUTE); }
  if (atomicLoad(&cmpMixed) != 0u) { return; }
  let v0 = w0 & 0xFFFFu;
  var ne = 0u;
  if (v0 != 0u) { ne = SOL_UNIFORM_BIT | v0; }
  solTable[slot] = ne;
  let at = atomicAdd(&solMeta[SOLM_FREE], 1u);
  atomicStore(&solMeta[SOLM_STACK + at], page);
}

// ============================================================================
// solScoop: THE WORLD HALF OF A VESSEL'S SCOOP (docs/PLAN_alchemy_chemistry.md
// contract 2.5). A vessel scoops through CONDITIONAL CLEARS (world.h
// CellOpClearIfMat), which sim_mutate.wgsl `cells` applies to the VOXEL and
// credits to the eighths ledger. The mass the cleared liquid carried is still
// on the (now air) cell. This pass, recorded after solAlloc -- the clear
// dirtied the chunk and the chunk carries solute, so solWant listed it and it
// is a page -- walks the same op list, takes the mass off every cell a clear
// emptied and adds it to that species' scoop ledger word. TickAuthority pays
// it to the vessel as a DISSOLVED portion, exactly as the eighths ledger pays
// the water. Without it the mass would be stale on an air cell and solCompact
// would discard it: the salt of a flask of brine would vanish.
//
// "A clear emptied it": the op is a conditional clear (CELLOP_IF_AIR on an air
// word) and the cell is air now. A REFUSED clear left its occupant (something
// else flowed in), which is not air unless it was air already -- and an air
// cell carries no mass after compaction, so reading one costs nothing wrong.
// Cell ops are deduplicated keep-first on the CPU (sim/oprecord.h), so no
// cell is credited twice. atomicAdd sums: order-independent (rule 1).
// ============================================================================
struct SolCellOp {
  cellIdx : u32,
  word    : u32,
};
@group(0) @binding(14) var<storage, read> cellOps : array<SolCellOp>;
// world.h kSolMScoopBySpecies / kSolScoopSpecies. Declared here, beside their
// only writer (CLAUDE.md: a constant one shader reads lives in that shader).
const SOLM_SCOOP_BY_SPECIES : u32 = 20u;
const SOL_SCOOP_SPECIES : u32 = 8u;

@compute @workgroup_size(64)
fn solScoop(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.cellCount) { return; }
  let op = cellOps[gid.x];
  // The slot space (chunk tickets), as sim_mutate.wgsl's `cells`.
  if (op.cellIdx >= NUM_SLOTS * CHUNK_VOL) { return; }
  if ((op.word & CELLOP_IF_AIR) == 0u || (op.word & 0xFFFu) != MAT_AIR) { return; }
  // A SOLUTE POUR (world.h CellOpSolute) shares the IF_AIR-on-AIR spelling
  // with a clear; its species in bits 24..30 is what tells them apart (a
  // clear's are always 0). sim_mutate.wgsl solPour is its reader.
  if (((op.word >> 24u) & 0x7Fu) != 0u) { return; }
  let tmat = (op.word >> 12u) & 0xFFFu;
  if (!solIsLiquidMat(tmat)) { return; }
  let ci = op.cellIdx / CHUNK_VOL;
  let lo = op.cellIdx % CHUNK_VOL;
  let l = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  let c = slotWorldChunk(ci, T.origin) * i32(CHUNK) + l;
  if (voxMat(voxWordAt(c)) != MAT_AIR) { return; }
  let v = solValueAt(c);
  if (v == 0u) { return; }
  let s = solSpeciesOf(v);
  // A species past the ledger's width is not credited: the mass stays and
  // solCompact discards it (counted), the same fate as before this pass.
  if (s == 0u || s > SOL_SCOOP_SPECIES) { return; }
  if (!solWritable(c)) { return; }
  _ = solStoreAt(c, 0u);
  atomicAdd(&solMeta[SOLM_SCOOP_BY_SPECIES + s - 1u], solMassOf(v));
  atomicAdd(&solMeta[SOLM_SCOOPED], solMassOf(v));
}

// ============================================================================
// solHash:the layer's share of the world hash. One group per slot; an EMPTY
// slot returns at once (almost all of them). Keyed by the SLOT cell index and
// the value, never by page index, and added into the same wrapping sum as the
// voxels -- commutative, so the atomic order cannot matter. A UNIFORM sentinel
// hashes its 4096 cells exactly as the page it stands for would, so the hash
// does not depend on whether compaction has run.
// ============================================================================
@compute @workgroup_size(64)
fn solHash(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let slot = wg.x;
  let e = solTable[slot];
  if (e == 0u) { return; }
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i = i + 64u) {
    let v = solCellValue(e, i);
    if (v != 0u) {
      h += pcg((slot * CHUNK_VOL + i) ^ (v * 0x85EBCA6Bu) ^ 0x5017E5EDu);
    }
  }
  if (h != 0u) { atomicAdd(&worldHash[0], h); }
}

// ============================================================================
// solEvict: a window shift is about to give these slots to OTHER world chunks.
//
// Recorded in its own command buffer between ticks (Simulation::
// EncodeSoluteEvict, called by Stream::ShiftAxis before the plane is
// refilled), over the slot list the CPU wrote into solMeta[SOLM_EVICT_LIST..].
// Per slot that carries solute: its entry (and, for a page, a copy of the
// page) goes to the eviction staging so the CPU can keep it with the chunk;
// the page goes back on the stack and the slot becomes EMPTY with its flags,
// stall counter and aggregate cleared. A slot that carries nothing costs one
// table read. The staging holds SOL_EVICT_RECORDS records; one that does not
// fit is COUNTED (stage[1]) -- its mass is gone from the world, and the CPU
// reports how many rather than it silently vanishing.
// ============================================================================
const SOLM_EVICT_LIST : u32 = SOLM_AGG + NUM_SLOTS;
const SOL_REC_WORDS : u32 = 2050u;
const SOL_EVICT_RECORDS : u32 = 256u;
const SOL_STAGE_HDR : u32 = 16u;
@group(0) @binding(44) var<storage, read_write> solStage : array<atomic<u32>>;

var<workgroup> evRec : u32;
var<workgroup> evEntry : u32;

@compute @workgroup_size(64)
fn solEvict(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    let slot = atomicLoad(&solMeta[SOLM_EVICT_LIST + wg.x]);
    let e = solTable[slot];
    evRec = 0xFFFFFFFFu;
    evEntry = e;
    if (e != 0u) {
      let r = atomicAdd(&solStage[0], 1u);
      if (r < SOL_EVICT_RECORDS) {
        evRec = r;
        // The slot, with the chunk's diffusion stall counter in the top byte
        // (the dilution floor's clock travels with the chunk).
        let st = min(atomicLoad(&solMeta[SOLM_STALL + slot]), 255u);
        atomicStore(&solStage[SOL_STAGE_HDR + r * SOL_REC_WORDS], slot | (st << 24u));
        atomicStore(&solStage[SOL_STAGE_HDR + r * SOL_REC_WORDS + 1u], e);
      } else {
        atomicAdd(&solStage[1], 1u);
      }
      if ((e & SOL_PAGE_BIT) != 0u) {
        // Safe to push before the copy below: nothing pops in this pass.
        let at = atomicAdd(&solMeta[SOLM_FREE], 1u);
        atomicStore(&solMeta[SOLM_STACK + at], e & SOL_PAGE_MASK);
      }
      solTable[slot] = 0u;
    }
    atomicStore(&solMeta[SOLM_WANT_FLAG + slot], 0u);
    atomicStore(&solMeta[SOLM_REQ_FLAG + slot], 0u);
    atomicStore(&solMeta[SOLM_STALL + slot], 0u);
    atomicStore(&solMeta[SOLM_AGG + slot], 0u);
  }
  workgroupBarrier();
  let rec = workgroupUniformLoad(&evRec);
  let e = workgroupUniformLoad(&evEntry);
  if (rec == 0xFFFFFFFFu || (e & SOL_PAGE_BIT) == 0u) { return; }
  let src = (e & SOL_PAGE_MASK) * SOL_WORDS_PER_PAGE;
  let dst = SOL_STAGE_HDR + rec * SOL_REC_WORDS + 2u;
  for (var i = li; i < SOL_WORDS_PER_PAGE; i = i + 64u) {
    atomicStore(&solStage[dst + i], atomicLoad(&solPool[src + i]));
  }
}

// ============================================================================
// solRestore: chunks entering the window (or a loaded world) get back the
// solute the CPU kept for them. The CPU wrote `count` records into the staging
// in the eviction layout, each naming the SLOT the chunk now occupies; a
// sentinel record installs its entry, a page record pops a page and copies the
// words in. Its own command buffer between ticks, like solEvict, so no tick
// can observe a half-restored chunk. Exhaustion counts exactly as solAlloc's.
// ============================================================================
var<workgroup> rsPage : u32;

@compute @workgroup_size(64)
fn solRestore(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let base = SOL_STAGE_HDR + wg.x * SOL_REC_WORDS;
  if (li == 0u) {
    let head = atomicLoad(&solStage[base]);
    let slot = head & 0xFFFFFFu;
    let e = atomicLoad(&solStage[base + 1u]);
    rsPage = 0xFFFFFFFFu;
    if ((e & SOL_PAGE_BIT) == 0u) {
      solTable[slot] = e & (SOL_UNIFORM_BIT | 0xFFFFu);
    } else {
      let old = atomicSub(&solMeta[SOLM_FREE], 1u);
      if (old == 0u || old > SOL_POOL_PAGES) {
        atomicAdd(&solMeta[SOLM_FREE], 1u);
        atomicAdd(&solMeta[SOLM_EXHAUSTED], 1u);
        solTable[slot] = 0u;
      } else {
        let page = atomicLoad(&solMeta[SOLM_STACK + old - 1u]);
        solTable[slot] = SOL_PAGE_BIT | page;
        rsPage = page;
        atomicMax(&solMeta[SOLM_HIGH_WATER], SOL_POOL_PAGES - (old - 1u));
      }
    }
    atomicStore(&solMeta[SOLM_STALL + slot], head >> 24u);
  }
  workgroupBarrier();
  let page = workgroupUniformLoad(&rsPage);
  if (page == 0xFFFFFFFFu) { return; }
  let dst = page * SOL_WORDS_PER_PAGE;
  for (var i = li; i < SOL_WORDS_PER_PAGE; i = i + 64u) {
    atomicStore(&solPool[dst + i], atomicLoad(&solStage[base + 2u + i]));
  }
}
