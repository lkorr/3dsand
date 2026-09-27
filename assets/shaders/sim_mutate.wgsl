// sim_mutate.wgsl — applies the CPU MutationQueue (brush paints/erases) to the
// grid. Runs before the CA passes each tick. Every world write flows through
// this path (DESIGN.md §2: the MutationQueue is also the future save/replay/
// network format).
//
// Dispatch: (4 * opsCount, 4, 4) workgroups of 4x4x4 threads — each op gets a
// 16^3 thread box centered on it (max brush radius 7).
//
// ---- TWO OPS ON ONE CELL IN ONE TICK: THE WINNER IS DEFINED ---------------
//
// Until docs/PLAN_multiplayer_now.md N3 it was not. Both entry points below
// were plain voxStores over one dispatch with no ordering between
// invocations, so a cell covered by two brush ops of the same tick took
// whichever write the driver happened to schedule last — the exact class of
// scheduling-dependent outcome CLAUDE.md rule 1 forbids, sitting on the one
// path every CPU mutation in the engine goes through.
//
// THE RULE, and it is `sim_explode.wgsl`'s rule word for word: the LOWEST op
// index that covers the cell AND would write it owns the cell. A thread for op
// i returns as soon as it finds a j < i that qualifies. Op index is push
// order, push order is deterministic (a fixed sequence in the CPU tick body),
// so the outcome is a pure function of the op list.
//
// "WOULD WRITE IT" is the full predicate, not just the sphere: op j's mode and
// its transmute from-filter are re-evaluated against the same occupant this
// thread read. A paint-into-air op that lands on stone does NOT get to shadow
// a later overwrite there, because it was never going to write anything.
// `opWouldWrite` below is that predicate and `main` must keep agreeing with
// it — the two are deliberately adjacent for that reason.
//
// WHAT THIS DOES NOT FIX, stated so nobody reads more into it than is there:
// each thread evaluates the predicate against ONE read of the occupant, and a
// lower op's store can still land between that read and this thread's
// decision. Closing that last window needs the two-phase mark/apply
// sim_explode uses (a per-op scratch buffer), which is a binding, a pass-table
// row and a buffer — a separate change. What is closed here is the common
// case: for overlaps where the ops disagree about a cell (paint vs melt, two
// materials), exactly one invocation now writes it.
//
// CELL OPS (the `cells` entry) dedupe on the CPU instead — an 8-byte op has no
// room for a predicate a shader could re-derive, and the choke point can sort.
// See sim/oprecord.h's CanonicalizeCells.

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
// Per-chunk support-loss flags. The MutationQueue can take a supporting voxel
// out of the grid (a brush erase, a laser melt, an island removal) and solids
// never fall in the CA, so without this the matter above simply hangs there —
// island detection is the only thing that can drop it, and this flag is what
// summons island detection. Side channel: see common.wgsl's SUPPORT_LOSS block.
@group(0) @binding(15) var<storage, read_write> supportOut : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Every
// shader that declares `read_write> voxels` must define this: gPtKernel's
// initializer references it, so omitting it is a compile error rather than
// a fault that reports as "unknown".
const PT_KERNEL : u32 = PT_K_MUTATE;

// THE SOLUTE LAYER (docs/PLAN_solutes.md; world.h kSol*). Bound here for the
// solute POUR (world.h CellOpSolute): `cells` asks the allocator for pages
// under a pour op, and `solPour` lays the pour's mass into the solvent there.
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

fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

// c's own chunk plus every chunk it borders (common.wgsl's dirtyFanSlot).
fn markBoth(c : vec3<i32>) {
  for (var k = 0u; k < 8u; k++) {
    let ci = dirtyFanSlot(c, T.origin, k);
    if (ci != SLOT_NONE) {
      atomicOr(&dirtyIn[ci], DIRTY_R_MUTATE);   // simulate this tick
      atomicOr(&dirtyOut[ci], DIRTY_R_MUTATE);  // and re-check next tick
    }
  }
}

// Would op `o` write world cell `c`, given the occupant material `prevMat`?
// EXACTLY the gate sequence `main` applies to its own op, restated as a
// predicate so the overlap dedupe can ask it about an EARLIER op. Any change
// to main's gates has to land here in the same edit or the two disagree and
// the dedupe starts shadowing writes that would never have happened.
fn opWouldWrite(o : BrushOp, c : vec3<i32>, prevMat : u32) -> bool {
  let d = c - vec3<i32>(o.cx, o.cy, o.cz);
  if (dot(d, d) > o.radius * o.radius) { return false; }
  if (o.mode == 0u) { return prevMat == MAT_AIR; }  // paint fills air only
  // the spell transmute's from-filter (see the note in main)
  if (o._p0 != 0u && prevMat != o._p0) { return false; }
  if ((o._p1 & 1u) != 0u && prevMat == MAT_AIR) { return false; }
  if (o.mode == 2u) {  // melt: air and 255-hardness matter are immune
    return prevMat != MAT_AIR && materials[prevMat].hardness < 255u;
  }
  return true;
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
  // Read the OCCUPANT ONCE, ahead of every branch that wants it: the paint
  // mode's air test, the melt mode's source material, and the support-loss
  // flag at the bottom, which needs what was here BEFORE the store. Index and
  // word come from one page-table resolution.
  let iw = voxIndexAndWord(c);
  let idx = iw.x;
  let prevMat = voxMat(iw.y);
  // OVERLAP DEDUPE (see the header): the lowest op index that covers this cell
  // and would write it owns it. One read of the occupant feeds every decision
  // in this thread, so the answer is a function of the op list and that read.
  // opsCount <= 64 and the loop only runs for ops after the first, so a tick
  // with one brush op pays a single compare.
  for (var j = 0u; j < opIdx; j++) {
    if (opWouldWrite(ops[j], c, prevMat)) { return; }
  }
  if (op.mode == 0u && prevMat != MAT_AIR) { return; }  // paint fills air only
  // A spell's transmute (game/spell.cpp Convert) is an overwrite with a FROM
  // filter: _p0 names the only material it may replace (0 = any), and bit 0 of
  // _p1 says the wildcard matches matter, not the void. Both are zero for every
  // other producer, so the brush and the laser are unchanged.
  if (op.mode != 0u && op._p0 != 0u && prevMat != op._p0) { return; }
  if (op.mode != 0u && (op._p1 & 1u) != 0u && prevMat == MAT_AIR) { return; }

  var mat = op.material;
  if (op.mode == 2u) {
    // melt (laser, PLAN §C1): each cell converts to ITS OWN molten product
    // from the material table — stone becomes lava while the sand next to it
    // becomes molten glass. Air stays air, 255-hardness matter is immune.
    if (prevMat == MAT_AIR || materials[prevMat].hardness >= 255u) { return; }
    mat = materials[prevMat].molten;
  }

  let rnd = hash3(T.seed ^ 0x5EEDu, T.tick, slotIdx);
  // liquids are born full (their state nibble is fullness); everything else
  // gets a palette variant. STAMP_NEVER = "hasn't acted": falls this tick.
  var state = rnd % 3u;
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) {
    state = LIQ_FULL_STATE;
  }
  // A powder brush with a GRAIN size (world.h BrushOp.pad1 bits 4..7, 1..7
  // eighths; 0 = whole cells) paints partial cells: a fine brush lays a
  // dusting instead of a block (common.wgsl POWDER MASS).
  // 15 = MIXED: each painted cell a crumble (POWDER ENTERS THE WORLD AS
  // GRAINS), the brush's default.
  let grain = (op._p1 >> 4u) & 0xFu;
  if (mat != MAT_AIR && matHasPowderMass(materials[mat])) {
    if (grain >= 1u && grain < POWDER_FULL) { state = grain + 2u; }
    else if (grain == 15u) { state = powderCrumbleState(rnd >> 2u); }
  }
  voxStore(idx, packVox(mat, state, STAMP_NEVER));
  markBoth(c);
  // An erase (mode 1 writing air) or a melt (solid -> lava) can be the thing a
  // ledge was standing on. Air-into-air and paint-into-air cost one compare:
  // prevMat is MAT_AIR, whose class is neither SOLID nor POWDER, so the call
  // returns on its first line.
  flagSupportLoss(c, materials[prevMat].klass, mat);
}

// Exact-cell writes (island removal / rubble handoff, DESIGN.md §7). Same
// MutationQueue discipline as the brush: the op stream is the only CPU->grid
// path, so saves/replays/networking capture island events for free.
// Dispatch: ceil(cellCount / 64) workgroups of 64.
// The scoop ledger's words in the page-fault record (world.h
// kPageFaultScoop*). Declared here, next to their only writer, not in
// common.wgsl: a constant one shader reads belongs to that shader.
const SCOOP_EIGHTHS_WORD : u32 = 36u;
const SCOOP_APPLIED_WORD : u32 = 37u;
const SCOOP_REFUSED_WORD : u32 = 38u;

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
  // Index and occupant from ONE table resolution (they used to be two).
  let iw = voxIndexAndWordInChunk(ci, lo);
  let wordIdx = iw.x;
  // prefab paint mode: fill air only (flag is spare-bit metadata, never stored)
  //
  // Reading a sentinel HERE is legal and correct: a paint-into-air op against
  // an EMPTY chunk should see air and proceed. That it can proceed is the
  // CPU's obligation — §3.3 materializes every op target unfiltered, precisely
  // so a brush into open sky is not silently a no-op.
  // Hoisted out of the fill-air-only branch below, because the support flag
  // needs it on EVERY path: an exact-cell op is how island removal and the
  // rubble handoff take matter out of the grid, and those are precisely the
  // writes that can leave something above them unsupported.
  let prevMat = voxMat(iw.y);
  // A SOLUTE POUR (world.h CellOpSolute) writes no voxel here. It wakes its
  // chunk and asks the solute allocator for pages round it (the request flag
  // solWant reads for a dirty chunk -- this one, now), so that `solPour`,
  // after solAlloc, finds every cell it may deposit into paged. Tested FIRST:
  // its word is also an IF_AIR word on AIR, which below would read as a
  // conditional clear of the material in bits 12..23.
  if (cellOpIsSolutePour(word)) {
    let lp = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
    let wcell = slotWorldChunk(ci, T.origin) * i32(CHUNK) + lp;
    markBoth(wcell);
    atomicStore(&solMeta[SOLM_REQ_FLAG + ci], 1u);
    // ...and for the chunks ABOVE and BELOW it. solPour's surface search
    // climbs out of the ground and falls up to SOL_POUR_DOWN cells, so it can
    // deposit into the chunk above or below this one; that chunk, if the CA
    // is already moving its liquid THIS tick, carries the mass on into ITS
    // neighbours -- which must be paged too, or the store is refused (a
    // fault: mass lost, or duplicated by half a swap). Waking them and
    // raising their request flags pages their 3x3x3 in this tick's solWant.
    for (var dy = -1; dy <= 1; dy += 2) {
      let c2 = wcell + vec3<i32>(0, dy * i32(CHUNK), 0);
      if (!inBounds(c2)) { continue; }
      markBoth(c2);
      atomicStore(&solMeta[SOLM_REQ_FLAG + voxSlotOfCell(c2)], 1u);
    }
    return;
  }
  if ((word & CELLOP_IF_AIR) != 0u) {
    if ((word & 0xFFFu) == MAT_AIR) {
      // CONDITIONAL CLEAR (world.h CellOpClearIfMat): the flag on an AIR word
      // means "empty this cell only if it still holds the material in bits
      // 12..23". A vessel's scoop is decided off last tick's snapshot and
      // liquid moves, so whatever flowed in since is left alone.
      if (prevMat != ((word >> 12u) & 0xFFFu)) {
        atomicAdd(&pageFaults[SCOOP_REFUSED_WORD], 1u);
        return;
      }
      // THE LEDGER: what this clear really took, in eighths -- a liquid's
      // fullness as it is NOW, not as the (older) snapshot the CPU scooped
      // from said it was. Summed with atomicAdd, which is order-independent,
      // so the total is a pure function of the tick's ops. The vessel is
      // credited from this number, never from its own guess.
      let prevWord = voxWordInChunkAt(ci, lo);
      var units = 8u;
      if (materials[prevMat].klass == CLASS_LIQUID) { units = ((prevWord >> 12u) & 7u) + 1u; }
      // A powder is paid its MASS (common.wgsl POWDER MASS); container.cpp
      // ContainerCellUnits is the CPU twin and must agree.
      if (matHasPowderMass(materials[prevMat])) { units = powderMass(prevWord); }
      atomicAdd(&pageFaults[SCOOP_EIGHTHS_WORD], units);
      atomicAdd(&pageFaults[SCOOP_APPLIED_WORD], 1u);
      word = 0u;
    } else {
      if (prevMat != MAT_AIR) { return; }
      word &= ~CELLOP_IF_AIR;
    }
  }
  voxStore(wordIdx, word);

  let l = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  let wc = slotWorldChunk(ci, T.origin) * i32(CHUNK) + l;
  markBoth(wc);
  // Island removal and the rubble handoff write air here; settle-back writes a
  // SOLID, which flagSupportLoss rejects on its second line (a solid still
  // supports). Body-burn escapes are CELLOP_IF_AIR ops that only land on air,
  // so prevMat is MAT_AIR and they never reach the neighbour walk — which is
  // what keeps DESIGN.md §7's "burn ops must not starve island detection"
  // property intact without a special case here.
  flagSupportLoss(wc, materials[prevMat].klass, voxMat(word));
}

// ---- THE SOLUTE POUR (docs/PLAN_alchemy_chemistry.md contract 2.5) --------
//
// A vessel pouring a SOLUTION (a flask of brine) sends its dissolved share as
// CellOpSolute ops (world.h): "this many units of species S into the liquid
// at or under this cell". The CPU aims them where the stream lands and sends
// them when it lands (game/container.h ContainerSolutePour); this kernel
// finds the solvent and lays the mass into it. Recorded after solAlloc (the
// op's chunk asked for pages in `cells` above, so N27 of it is paged) and
// after solScoop (which empties AIR cells' stale mass; this pass writes only
// LIQUID cells' mass and AIR cells' voxels, and its RW(Voxels) use orders it
// after solScoop's reads), before the CA.
//
// ONE INVOCATION, THE OPS IN PUSH ORDER. SubmitTick moves every pour to the
// tail of the cell-op stream, so the thread walks back from the end while the
// ops are pours and then applies them forward. Two pours onto one cell, or
// two whose patches overlap, are applied one after the other -- no atomics
// deciding who got the room, so the outcome is a function of the op list
// (rule 1). A tick carries a handful of pours (the CPU caps them), so a
// single thread is the cheap answer, not a compromise.
//
// PER OP:
//   1. FIND THE SURFACE: out of the ground if the cell is in it (a pour point
//      on a surface floors into it; up to SOL_POUR_CLIMB), then down through
//      air and gas (up to SOL_POUR_DOWN) to the first thing that is not.
//   2. INTO SOLUTION: if that is a liquid SOLVENT of the species, the mass
//      goes into it and the cells round it (a 5 x 5 patch, SOL_POUR_LAYERS
//      deep: the surface a stream lands on), each up to the species
//      saturation for its fullness -- the same cap a dissolving grain meets
//      (solCap). A cell carrying another species is skipped (two species do
//      not share a cell). The total put in is WHOLE EIGHTHS unless it is all
//      of it, so what is left over is whole eighths too.
//   3. THE REST PRECIPITATES: what found no room -- dry ground, a saturated
//      puddle, oil -- is laid down as the species powder (`from`, the powder
//      it dissolved from) in the first AIR cell at the surface, one cell per
//      eight eighths, as a partial cell where it is less. That is what the
//      old CPU fallback did, minus the flight: the salt a dry basin gets is a
//      salt crust, and it dissolves again when water arrives (sim_step
//      solTryDissolve). Nothing is rounded away: a sub-eighth remainder or a
//      pour into solid rock with no air in reach is COUNTED lost
//      (SOLM_POUR_LOST), which the solute-pour gate asserts stays zero.
// Every unit is counted: SOLM_POURED (into solution) + SOLM_POUR_POWDER (as
// powder) + SOLM_POUR_LOST == every unit the ops carried.
const SOLM_POUR_POWDER : u32 = 14u;   // world.h kSolMPourPowder
const SOLM_POUR_LOST : u32 = 15u;     // world.h kSolMPourLost
const SOL_POUR_CLIMB : i32 = 3;
const SOL_POUR_DOWN : i32 = 16;
const SOL_POUR_RADIUS : i32 = 2;
const SOL_POUR_LAYERS : i32 = 3;
// world.h kMaxSolutePourOpsPerTick (check_invariants.py); SubmitTick has
// already clamped the stream to it and counted what it refused.
const SOL_POUR_MAX_OPS : u32 = 256u;

fn cellOpIsSolutePour(w : u32) -> bool {
  return (w & CELLOP_IF_AIR) != 0u && (w & 0xFFFu) == MAT_AIR && ((w >> 24u) & 0x7Fu) != 0u;
}
fn solPourOpen(m : u32) -> bool {
  return m == MAT_AIR || materials[m].klass == CLASS_GAS;
}
// Room for species `s` in the cell at `c`, or 0 when it cannot take any (not
// a solvent of s, carries another species, not paged).
fn solPourRoom(c : vec3<i32>, s : u32) -> u32 {
  if (!inBounds(c)) { return 0u; }
  let w = voxWordAt(c);
  let m = voxMat(w);
  if (!solIsLiquidMat(m) || !solIsSolvent(m, s) || !solWritable(c)) { return 0u; }
  let v = solValueAt(c);
  if (v != 0u && solSpeciesOf(v) != s) { return 0u; }
  let cap = min(solCap(s, voxState(w) + 1u), 255u);
  return cap - min(cap, solMassOf(v));
}
// The k-th cell of the patch under surface cell p: the centre first, then
// the rest of its layer, then the layers below.
fn solPourPatch(p : vec3<i32>, k : i32) -> vec3<i32> {
  let side = 2 * SOL_POUR_RADIUS + 1;
  let per = side * side;
  let dy = k / per;
  let r = k % per;
  var q = r;
  if (r == 0) { q = per / 2; } else if (r <= per / 2) { q = r - 1; }
  return p + vec3<i32>(q % side - SOL_POUR_RADIUS, -dy, q / side - SOL_POUR_RADIUS);
}

fn solPourOne(op : CellOp) {
  let s = (op.word >> 24u) & 0x7Fu;
  let units = (op.word >> 12u) & 0xFFFu;
  if (units == 0u) { return; }
  let ci = op.cellIdx / CHUNK_VOL;
  let lo = op.cellIdx % CHUNK_VOL;
  let lp = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  var p = slotWorldChunk(ci, T.origin) * i32(CHUNK) + lp;
  if (!inBounds(p)) {
    atomicAdd(&solMeta[SOLM_POUR_LOST], units);
    return;
  }
  // 1. The surface.
  for (var k = 0; k < SOL_POUR_CLIMB; k++) {
    let m = voxMat(voxWordAt(p));
    if (solPourOpen(m) || solIsLiquidMat(m)) { break; }
    let up = p + vec3<i32>(0, 1, 0);
    if (!inBounds(up)) { break; }
    p = up;
  }
  for (var k = 0; k < SOL_POUR_DOWN; k++) {
    if (!solPourOpen(voxMat(voxWordAt(p)))) { break; }
    let dn = p - vec3<i32>(0, 1, 0);
    if (!inBounds(dn)) { break; }
    p = dn;
  }
  var left = units;
  let y8 = solYield8(s);
  // The pour's reach, in chunks: its own and the ones above and below,
  // which `cells` woke and asked pages round (so whatever the CA does with
  // the mass this tick lands in a paged chunk). A deeper fall -- 16 cells
  // of air from the bottom of the op's chunk -- lands past it and
  // precipitates instead of dissolving into a chunk nobody paged round.
  let opCy = worldChunkOf(slotWorldChunk(ci, T.origin) * i32(CHUNK) + lp).y;
  // 2. Into solution.
  let side = 2 * SOL_POUR_RADIUS + 1;
  let n = side * side * SOL_POUR_LAYERS;
  let pm = voxMat(voxWordAt(p));
  if (solIsLiquidMat(pm) && solIsSolvent(pm, s)) {
    var room = 0u;
    for (var k = 0; k < n; k++) {
      let c = solPourPatch(p, k);
      if (abs(worldChunkOf(c).y - opCy) > 1) { continue; }
      room += solPourRoom(c, s);
    }
    var put = min(left, room);
    if (put < left) { put = (put / y8) * y8; }
    var given = 0u;
    for (var k = 0; k < n && given < put; k++) {
      let c = solPourPatch(p, k);
      if (abs(worldChunkOf(c).y - opCy) > 1) { continue; }
      let r = solPourRoom(c, s);
      if (r == 0u) { continue; }
      let g = min(r, put - given);
      _ = solStoreAt(c, solPack(s, solMassOf(solValueAt(c)) + g));
      given += g;
      // Awake next tick: the diffusion spreads it, the CA carries it.
      let own = dirtyFanSlot(c, T.origin, 0u);
      if (own != SLOT_NONE) { atomicOr(&dirtyOut[own], DIRTY_R_SOLUTE); }
    }
    if (given != 0u) { atomicAdd(&solMeta[SOLM_POURED], given); }
    left -= given;
  }
  if (left == 0u) { return; }
  // 3. The rest precipitates, as the powder it dissolved from.
  let powder = solSpec[SOLS_BASE + s * SOLS_STRIDE + 15u] & 0xFFFu;
  var eighths = left / y8;
  var lost = left % y8;
  if (powder == MAT_AIR || !matHasPowderMass(materials[powder])) {
    atomicAdd(&solMeta[SOLM_POUR_LOST], left);
    return;
  }
  var q = p;
  var tries = SOL_POUR_CLIMB + 4;
  loop {
    if (eighths == 0u || tries <= 0) { break; }
    tries -= 1;
    let idx = voxWordIndex(q);
    if (idx != PT_NO_WORD && voxMat(voxWordAt(q)) == MAT_AIR) {
      let e = min(eighths, POWDER_FULL);
      var state = e + 2u;
      if (e == POWDER_FULL) { state = hash3(T.seed ^ 0x5017B0u, T.tick, cellIndexW(q)) % 3u; }
      voxStore(idx, packVox(powder, state, STAMP_NEVER));
      // Next tick, not this one: the CA's list is already compacted.
      for (var f = 0u; f < 8u; f++) {
        let fs = dirtyFanSlot(q, T.origin, f);
        if (fs != SLOT_NONE) { atomicOr(&dirtyOut[fs], DIRTY_R_MUTATE); }
      }
      atomicAdd(&solMeta[SOLM_POUR_POWDER], e * y8);
      eighths -= e;
    }
    let up = q + vec3<i32>(0, 1, 0);
    if (!inBounds(up)) { break; }
    q = up;
  }
  lost += eighths * y8;
  if (lost != 0u) { atomicAdd(&solMeta[SOLM_POUR_LOST], lost); }
}

@compute @workgroup_size(1)
fn solPour() {
  var first = T.cellCount;
  loop {
    if (first == 0u || T.cellCount - first >= SOL_POUR_MAX_OPS) { break; }
    if (!cellOpIsSolutePour(cellOps[first - 1u].word)) { break; }
    first -= 1u;
  }
  for (var i = first; i < T.cellCount; i++) { solPourOne(cellOps[i]); }
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
  if (slot >= NUM_SLOTS) { return; }
  atomicOr(&dirtyIn[slot], DIRTY_R_MUTATE);
  atomicOr(&dirtyOut[slot], DIRTY_R_MUTATE);
}

// ---- RAIN ON THE GROUND (2026-09-25) ----------------------------------------
//
// Rain does not delete stains. It STAINS THE GROUND WITH WATER, through the
// same common.wgsl stainStep a pouring liquid uses -- and the newest stain wins,
// so a drop landing on blood-soaked ground replaces the blood with wet, and the
// wet then dries (a stain type's `dries`). The rain material is data: the one
// that authors `"stain": {"rain": true}` (water), published in stain palette
// entry 0's `_r3` (materials.h kStainPal*).
//
// THE COST IS FIXED, NOT THE WORLD'S (rule 2). A storm falls on every surface
// of the window, and a kernel that woke every surface chunk for it would keep
// the whole visible world awake for as long as it rained. So this is a SAMPLER:
// one thread per RAIN_TILE x RAIN_TILE tile of columns, each tick landing on
// one column of its tile (a hash of seed, tick and tile), walking down past
// sky -- a whole chunk at a time over an EMPTY sentinel -- to the first thing
// that is not air or gas, and acting on that one cell:
//   * while it rains (T.weatherRain's amount, as that chance out of 255): wet
//     it. A cell that was dry or wore another stain dirty-marks its chunk --
//     the chunk has changed what it IS (modified tracking; its coat rules must
//     see the water) -- but re-wetting what is already wet marks nothing, so a
//     steady storm on a wet meadow keeps nothing awake;
//   * while it does not: a wet top surface loses a level, at its type's `dries`
//     scaled up by the tile area (a column is visited once per RAIN_TILE^2
//     ticks on average), and never while the liquid that made it touches it.
//     This is why sim_step's stainDry does not hold a chunk awake for a cell
//     open above: the top surfaces dry here, asleep.
// A liquid on top (a pond) takes the rain and nothing is stained; a solid
// SENTINEL on top (a uniform chunk whose top face is the surface) has no page
// to write, so the drop is dropped rather than faulted.
//
// DETERMINISM (rule 1). Every tile is one thread and owns its columns, so no
// two threads write one cell; each writes only the STAIN bits of its cell, and
// what it reads of a neighbour is its material, which nothing here changes.
// The column and the rolls are hash3 of (seed, tick, tile); the rain word is
// TickParams, which ops-replay records.
const RAIN_TILE : u32 = 8u;            // pass_table.cpp kRainTile
const RAIN_TILES : u32 = WORLD_N / RAIN_TILE;
const RAIN_SALT : u32 = 0x5A1D0F00u;

@compute @workgroup_size(64)
fn rainFall(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= RAIN_TILES * RAIN_TILES) { return; }
  let h = hash3(T.seed ^ RAIN_SALT, T.tick, gid.x);
  let base = T.origin * i32(CHUNK);
  let x = base.x + i32((gid.x % RAIN_TILES) * RAIN_TILE + (h % RAIN_TILE));
  let z = base.z + i32((gid.x / RAIN_TILES) * RAIN_TILE + ((h / RAIN_TILE) % RAIN_TILE));
  let rain = T.weatherRain & RAIN_AMOUNT_MASK;
  let roll = hash3(h, 0x7A11u, T.tick);
  // A dry sky dries; the drying chance is read off the cell's stain below.
  // Rolled FIRST while it rains, so a drizzle's idle threads skip the walk.
  if (rain != 0u && (roll % 255u) >= rain) { return; }

  // Down the column to the first surface.
  var y = base.y + i32(WORLD_N) - 1;
  var idx = PT_NO_WORD;
  var w = 0u;
  loop {
    if (y < base.y) { return; }
    let e = pageEntryOf(voxSlotOfCell(vec3<i32>(x, y, z)));
    if ((e & PT_SENTINEL_BIT) != 0u) {
      let sm = e & PT_MAT_MASK;
      if (sm != MAT_AIR && materials[sm].klass != CLASS_GAS) { return; }
      y = ((y >> CHUNK_SHIFT) << CHUNK_SHIFT) - 1;  // the whole chunk is sky
      continue;
    }
    let lo = vec3<u32>(vec3<i32>(x, y, z) & vec3<i32>(CHUNK_MASK));
    let i = e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
    let cw = voxels[i];
    let cm = voxMat(cw);
    if (cm == MAT_AIR || materials[cm].klass == CLASS_GAS) { y -= 1; continue; }
    idx = i;
    w = cw;
    break;
  }
  let c = vec3<i32>(x, y, z);
  let m = materials[voxMat(w)];
  if (m.klass != CLASS_SOLID && m.klass != CLASS_POWDER) { return; }

  if (rain != 0u) {
    let rm = materials[STAIN_PALETTE_BASE]._r3 & 0xFFFu;
    if (rm == 0u) { return; }
    let r = materials[rm];
    let d = stainStep(matStainType(r), matStainAmount(r), matWashes(r), w, m);
    if ((d.y & STAIN_WORK) == 0u) { return; }
    voxStore(idx, d.x);
    if (!voxStained(w) || voxStainType(w) != matStainType(r)) {
      let own = dirtyFanSlot(c, T.origin, 0u);
      if (own != SLOT_NONE) { atomicOr(&dirtyOut[own], DIRTY_R_STAINW); }
    }
    return;
  }

  if (!voxStained(w)) { return; }
  let pal = materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3;
  let chance = min(stainDryChance(pal) * RAIN_TILE * RAIN_TILE, 1000u);
  if (chance == 0u || (roll % 1000u) >= chance) { return; }
  let wetter = pal & 0xFFFu;
  for (var k = 0u; k < 6u; k++) {
    let n = c + faceDir(k);
    if (inBounds(n) && voxMat(voxWordAt(n)) == wetter) { return; }
  }
  let left = voxStainAmt(w) - 1u;
  var nw = w & ~STAIN_BITS;
  if (left > 0u) { nw = nw | packStain(voxStainType(w), left); }
  voxStore(idx, nw);
}
