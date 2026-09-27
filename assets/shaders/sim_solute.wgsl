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
const SOL_POOL_PAGES : u32 = 4096u;
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
// the count is what says so (Simulation::CheckSoluteFaults aborts on it). A
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
// (Simulation::CheckSoluteFaults): which slots lost the race would be
// scheduling-dependent, and a slot without its page would drop writes.
// ============================================================================
var<workgroup> allocPage : u32;
var<workgroup> allocFill : u32;

@compute @workgroup_size(64)
fn solAlloc(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    let slot = atomicLoad(&solMeta[SOLM_WANT_LIST + wg.x]);
    atomicStore(&solMeta[SOLM_WANT_FLAG + slot], 0u);
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
        atomicMax(&solMeta[SOLM_HIGH_WATER], SOL_POOL_PAGES - (old - 1u));
      }
    }
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
// ============================================================================
var<workgroup> difWork : atomic<u32>;
var<workgroup> difCross : atomic<u32>;
var<workgroup> difBack : atomic<u32>;

// 0 = nothing to do, 1 = work pending (d != 0) but nothing written, 3 = wrote.
// `k` is the stride: cA + k * step == cB.
fn solPairExchange(cA : vec3<i32>, cB : vec3<i32>, step : vec3<i32>, k : u32,
                   write : bool) -> u32 {
  if (!cellResident(cB, T.origin)) { return 0u; }
  let wA = voxWordAt(cA);
  let wB = voxWordAt(cB);
  let mA = voxMat(wA);
  let mB = voxMat(wB);
  if (!solIsLiquidMat(mA) || !solIsLiquidMat(mB)) { return 0u; }
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
    }
    // The face this chunk is the PARTNER of: parity-1 pairs owned by the -axis
    // neighbour (their upper cells are this chunk's first k layers). Read
    // only; a pending one wakes its owner.
    if (parity == 1u) {
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
//      nothing and a body held above the floor conserves exactly;
//   3. the chunk's aggregate (species, total mass) is republished;
//   4. a chunk whose 4096 values are all equal becomes SOL_UNIFORM (or
//      SOL_EMPTY when the value is 0) and its page goes back on the stack.
// Every discarded unit is counted (SOLM_DISCARDED), so the gates balance the
// ledger rather than trusting it.
// ============================================================================
var<workgroup> cmpSlot : u32;
var<workgroup> cmpEntry : u32;
var<workgroup> cmpMixed : atomic<u32>;
var<workgroup> cmpMass : atomic<u32>;
var<workgroup> cmpSpecies : atomic<u32>;

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
    atomicStore(&cmpMixed, 0u);
    atomicStore(&cmpMass, 0u);
    atomicStore(&cmpSpecies, 0u);
  }
  workgroupBarrier();
  let slot = workgroupUniformLoad(&cmpSlot);
  let e = workgroupUniformLoad(&cmpEntry);
  if ((e & SOL_PAGE_BIT) == 0u) {
    // A sentinel: nothing to clean or hand back, only the aggregate.
    if (li == 0u) {
      var agg = 0u;
      if (e != 0u) { agg = (solSpeciesOf(e & 0xFFFFu) << 24u) | (solMassOf(e & 0xFFFFu) * CHUNK_VOL); }
      atomicStore(&solMeta[SOLM_AGG + slot], agg);
    }
    return;
  }
  let page = e & SOL_PAGE_MASK;
  let pbase = page * SOL_WORDS_PER_PAGE;
  let stalled = atomicLoad(&solMeta[SOLM_STALL + slot]) >= SOL_STALL_TICKS;
  let cbase = slotWorldChunk(slot, T.origin) * i32(CHUNK);
  var mass = 0u;
  var species = 0u;
  var discarded = 0u;
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
        if (!solIsLiquidMat(m) || !solIsSolvent(m, s)) {
          discarded += solMassOf(v);
          v = 0u;
        } else if (stalled && solMassOf(v) * 8u < solFloor(s) * (voxState(vw) + 1u)) {
          discarded += solMassOf(v);
          v = 0u;
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
  if (op.cellIdx >= WORLD_N * WORLD_N * WORLD_N) { return; }
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
