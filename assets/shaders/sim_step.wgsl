// sim_step.wgsl — one 3x3x3-color pass of the cellular automaton.
// Dispatched INDIRECTLY over the compacted dirty-chunk list (sim_compact.wgsl):
// one workgroup per dirty chunk, threads map to that chunk's cells of the
// current color (local = colorPhase + lid*3). Any two acting cells are >=3
// apart on every axis while writes reach <=1 cell: destination writes are
// provably disjoint. No atomics on voxel data, fixed pass order =>
// bit-deterministic (DESIGN.md §4). The dirty-list ORDER is scheduling-
// dependent, but each dirty chunk appears exactly once and writes are
// disjoint, so processing order cannot affect sim state.
//
// Each acting cell: reaction scan (substep 0 only, DESIGN.md §6 rules compiled
// from reactions.json) -> movement (powder/gas rules, mass-conserving fullness
// flow for liquids, viscosity gate, critter wander).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
// THIS tick's dirty set, i.e. the reason bits every rule ORed in LAST tick. Read
// only, and stable for the whole 54-iteration colour loop: mutate/explode write
// it before sim_compact builds the dispatch list and nothing touches it after.
// Same read, and the same "was this chunk actually disturbed" question, as
// sim_waterbody's quiescence pass (pass_table.def's R(DirtyIn) note). One scalar
// load per workgroup — see FILM_LICENCE below for the only thing that uses it.
@group(0) @binding(1) var<storage, read>       dirtyIn  : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(5) var<uniform> P : PassParams;
@group(0) @binding(11) var<storage, read>      reactions : array<Reaction>;
// ---- MLS-MPM excited-fluid coupling (sim_fluid_seam.wgsl; DESIGN.md §5) ----
// The solver's block map + node grid (LAST tick's final substep) and the
// seam's per-cell intent/flags scratch. Read here so authored PAIR rules see
// excited liquid as a real neighbour (fluidOccMat below); the one write is
// the consume flag when such a rule fires. A world with no fluid has an
// all-zero block map and none of this costs more than one load per air
// neighbour of a reacting cell — and the pinned hash is the gate on "no
// fluid means no behaviour change".
@group(0) @binding(20) var<storage, read> fluidBlockMapS : array<u32>;
@group(0) @binding(21) var<storage, read> fluidGridS : array<i32>;
@group(0) @binding(22) var<storage, read_write> fluidCellScratch : array<atomic<u32>>;
// read_write to match the shared layout entry (sim_compact writes it);
// this shader only reads.
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
// Binding 13 is argsStage, NOT the indirect buffer (copy_argsStage_dispatchArgs
// copies it into the never-bound dispatchArgs), so `main` reading the dirty
// count from it (declared below, beside the air mask) is legal. The old note
// here dated from Dawn, where the two were one buffer.
// Per-chunk support-loss flags: set when a supporting voxel (solid/powder)
// vacates or transforms next to a solid. Side-channel only — read back by the
// CPU to queue island checks (debris.cpp), never fed back into voxel state,
// so determinism is unaffected (all writers store the same value 1).
@group(0) @binding(15) var<storage, read_write> supportOut : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Every
// shader that declares `read_write> voxels` must define this: gPtKernel's
// initializer references it, so omitting it is a compile error rather than
// a fault that reports as "unknown".
const PT_KERNEL : u32 = PT_K_STEP;
@group(0) @binding(23) var<storage, read_write> actVoxViz  : array<atomic<u32>>;
// ---- THE WINDOW EDGE'S OUTBOX (docs/PLAN_gas_particles.md §2.3) -----------
// An 8-word header followed by kGasSpawnPerTick Particle-shaped records. A gas
// voxel whose intended move leaves the residency window appends itself here
// and deletes itself from the grid; sim_gas.wgsl's spawn pass turns the list
// into gas particles LATER IN THIS SAME TICK. Cleared by a whole-buffer fill
// before the CA runs, so the header words double as this tick's gas counters
// (they are read back with the snapshot ring; see GAS_SP_* below).
@group(0) @binding(33) var<storage, read_write> gasSpawn : array<atomic<u32>>;
// ---- THE OUTER GAS DENSITY BOX (docs/PLAN_gas_particles.md §2.5, stage 1b) -
// RENDER-ONLY DERIVED DATA, and the CA writes it for one reason: gas inside
// the window is a voxel and gas outside it is a parcel, the two are drawn from
// different representations at 64x different resolution, and until stage 1b
// they met at the window face with NO overlap — a hard edge in the sky where
// crisp voxel smoke became soft 0.8 m cells. The renderer crossfades between
// them across the outer half of the window now, and it can only do that if the
// coarse box holds the in-window plume as well as the parcels.
//
// The sim never reads this buffer, the world hash never covers it, and it is
// cleared by fill_gasOuter before the CA runs. So the splat below cannot move
// a voxel or a hash — it is exactly as observable as `actVoxViz` at 23.
@group(0) @binding(34) var<storage, read_write> gasOuter : array<atomic<u32>>;

// gasSpawn's header. Words 0..2 are written HERE (the CA is the only producer
// of edge conversions); words 3..7 are written by sim_gas.wgsl. Must agree with
// the GAS_SP_* block in sim_gas.wgsl and with kGasSp* in src/sim/world.h —
// check_invariants.py compares the three.
const GAS_SP_COUNT    : u32 = 0u;  // gasLeave append cursor (may exceed the cap)
const GAS_SP_REFUSED  : u32 = 1u;  // gasLeave refused: the per-tick list was full
const GAS_SP_EDGE     : u32 = 2u;  // gas voxels whose intent pointed out of the window
// Words 9..11: the DETERMINISTIC LEAVE BUDGET (2026-10-03, see THE EDGE'S
// BUDGET below). 9 is written by sim_gas's gasLeavePrep before the CA, 10 by
// camask on substep 0, 11 only on a broken invariant.
const GAS_SP_BUDGET   : u32 = 9u;  // conversions this tick may make, whole window
const GAS_SP_EDGECH   : u32 = 10u; // dirty chunks touching the residency edge
const GAS_SP_OVERRUN  : u32 = 11u; // an accepted conversion found the list full: a BUG
// Words 12..15: THE CA'S COST ATTRIBUTION (2026-10-03, fire-gpu). Diagnostic
// counters, never read by the sim and never hashed: what the colour rows ran
// this tick, by kind, and how many awake chunks hold gas and nothing else.
// world.h kGasSpCa* (check_invariants.py compares).
const GAS_SP_CA_GAS   : u32 = 12u; // gas cells the colour rows ran (both substeps)
const GAS_SP_CA_OTHER : u32 = 13u; // non-gas, non-inert cells they ran (both substeps)
const GAS_SP_GASONLY_CELLS : u32 = 14u; // gas cells in gas-only chunks (substep 0)
const GAS_SP_GASONLY  : u32 = 15u; // lo16 gas-only chunks, hi16 matterless chunks (substep 0)
const GAS_SP_HDR      : u32 = 16u; // first record word (words 8..15: sim_gas)
const GAS_SP_STRIDE   : u32 = 8u;  // u32 per record (a 32-byte Particle)
// kGasSpawnPerTick (src/sim/world.h). The size of the record list and the
// ceiling on GAS_SP_BUDGET. A refused conversion leaves the voxel exactly
// where it was and it takes the lateral ladder, so the edge is a rate-limited
// sink and never a hole that loses mass.
const GAS_SPAWN_CAP   : u32 = 65536u;
// The per-chunk budget words (world.h kGasSpChunkBase): one u32 per SLOT
// behind the record list. Bit 31 = "this chunk was counted into GAS_SP_EDGECH
// this tick" (camask, substep 0), bits 0..30 = conversions it has made so far
// this tick. Initialised by camask for every chunk on the dirty list, and read
// only for chunks on that same list, so it needs no clear.
const GAS_SP_CHUNK0   : u32 = 524304u; // GAS_SP_HDR + GAS_SPAWN_CAP * GAS_SP_STRIDE
const GAS_LV_COUNTED  : u32 = 0x80000000u;

// ---- THE EDGE'S BUDGET, AND WHY IT IS NOT A CURSOR (2026-10-03) -----------
// Until this date a conversion charged a shared atomicAdd cursor against
// GAS_SPAWN_CAP, so WHICH voxels were refused when the list filled was decided
// by which workgroup got there first -- and a refused voxel stays in the grid,
// where the world hash sees it. A rule-1 hole, held off by sizing the cap out
// of reach. It is closed now by making every refusal a pure function of the
// world:
//
//  1. THE TICK'S BUDGET is fixed before the CA (sim_gas gasLeavePrep): the
//     record list's cap, less what the parcel POOL cannot take (live parcels +
//     this tick's CPU spawns), so gasSpawnStep can never refuse one either.
//  2. IT IS SPLIT EVENLY across the dirty chunks that touch the residency edge
//     (camask counts them on substep 0 -- a count is order-free). A chunk's
//     share is budget / edgeChunks, so the shares can never sum past the cap.
//  3. WITHIN A CHUNK, across the 54 dispatches, the share is spent in dispatch
//     order (the per-chunk word); within ONE dispatch a chunk belongs to ONE
//     workgroup, which collects its leavers (gasLeaveDefer), and if there are
//     more than its share left it accepts the lowest-ranked by a per-cell hash
//     -- a total order on the cells, not on the threads.
//
// The deferral is free of consequence: a leaver has done nothing yet when it
// defers (its primary tryMove failed out of window, which writes nothing),
// and what it does after the workgroup barrier -- convert, or take the rest
// of the ladder -- reads and writes only within reach 1, which no other
// thread of this colour can touch. So the only thing that moved is WHEN in
// the dispatch the cell finishes, never what it does.
const GAS_LV_MAX      : u32 = 304u;   // CA_PACK_MAX * 152 boundary sites of one colour
const GAS_LV_SALT     : u32 = 0x6A5C11F7u;
// The outer density box's shape. MIRRORED FROM world.h's kGasOuterN /
// kGasOuterShift, not imported through common.wgsl: a constant only a few
// shaders read is declared in each of them, because a common.wgsl edit misses
// the SPIR-V cache for EVERY shader and pays the worldgen far-cascade compile
// (CLAUDE.md). check_invariants.py's `pairs` table is what holds the three
// copies (here, sim_gas.wgsl, raymarch.wgsl) in step with world.h — the
// splatter and the sampler disagreeing about cell size draws a plume in the
// wrong place, silently.
const GAS_OUTER_N     : u32 = 128u;
const GAS_OUTER_SHIFT : u32 = 3u;
const GAS_OUTER_MAX   : u32 = 60000u;
// sim.gasMode, on the tick stream (TickParams). 0 makes the residency edge a
// WALL again: the branch below never runs, no record is appended, and the CPU
// reads the same value to leave every gas pass unrecorded — so `gasMode = 0`
// is an off switch that costs nothing, not a cheap path. Same shape as
// windMode and waterBodyMode.
const GAS_MODE_OFF    : u32 = 0u;
// PFLAG_GAS is in common.wgsl now, beside the other PFLAG_* bits.

// ---- THE SOLUTE LAYER (docs/PLAN_solutes.md; world.h kSol*; bindings 40..43)
// The CA is where dissolved mass MOVES: every liquid move carries it
// (advection, tryMove / transferLiquid), a powder named as a solute's `from`
// dissolves into a solvent it touches, a solvent the CA turns into something
// else pushes its mass into a neighbour or leaves it as powder (evaporation
// concentrates brine and finally precipitates salt), and a solvent
// concentrated past a `converts` row becomes that material. Diffusion, the
// dilution floor and the pages themselves are sim_solute.wgsl's.
//
// solTable is read_write only because the layout entry is: the CA never
// writes it (a page is solAlloc's, handed back by solCompact). Every write of
// a cell's mass goes through solStoreAt, which refuses and COUNTS a write into
// a chunk that is not a page -- the allocator guarantees one to every chunk
// this tick can write, so the count is a bug detector, not a code path.
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

// ---- THE TEMPERATURE LAYER (sim_heat.wgsl, src/sim/heat.h) ------------------
// The CA READS the local excess X of a cell's block (heatPool, written by last
// tick's heatRelax, so stable for the whole colour loop) and the climate
// (heatParams); it WRITES only two things into heatMeta, both order-free: the
// caMask rows raise a chunk's "an emitter is here" flag (an OR), and a fired
// thermal transition bumps its kind's counter (an ADD). A thermal transition
// is run by heatReact, below, after the material's reaction bucket.
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

// ---- THE CHARGE FIELD (sim_elec.wgsl, src/sim/elec.h) ----------------------
// The CA may READ the settled potential P (elecAt below: half 0 of a chunk's
// page, written by last tick's elecSettle, so stable for the whole colour
// loop). It WRITES one thing, order-free: the caMask rows set a chunk's bit of
// the want bitset (an OR) when it holds a SOURCE material and has no page --
// the doorbell that pages it after the CA (elecAlloc's head).
@group(0) @binding(55) var<storage, read> elecPool : array<u32>;
@group(0) @binding(56) var<storage, read_write> elecMeta : array<atomic<u32>>;

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

// ---- E2: WHAT CHARGE DOES IN THE CA (docs/PLAN_electricity.md section 2) -----
// Three effects, all reading the cell's OWN settled P (last tick's field,
// stable for the whole colour loop) and writing only the cell or one face:
//   1. THE VIRTUAL ELECTRIC NEIGHBOUR (doReactions, RK_PAIR): a cell holding
//      P >= EP_REACT_MIN satisfies its rules whose neighbour is tag:electric
//      with no spark beside it -- after the six faces and the coat, one roll
//      per rule per tick as for any partner, at the authored chance ramped by
//      P / EP_REACT_FULL. The neighbour-side product goes into an open (air)
//      face, else it is lost (the bench's Deposit). This is what electrolyses a
//      charged pool from inside, and at every pool-top height (the old y = 2
//      mod 3 miss was a spark rising before the pool cell's phase looked).
//   2. OHMIC IGNITION / CHAR (elecReact): a conductor with electric.ignite /
//      char heats by P x resist; with an air face it becomes ignite.into, with
//      none its char product, scaled by air faces like heatReact.
//   3. CRACKLE (elecReact): a charged conductor with an air face throws a
//      spark (P >= EP_CRACKLE_LO_P) or an arc (>= EP_CRACKLE_HI_P) into it.
//      BOUNDED: each threshold is held above the emitted material's own source
//      (Simulation::PrepareElec, ElecCrackleThreshold), and the field is a MAX
//      (never a sum), so the P a crackle can give back to any conductor is at
//      most source - 1 < the threshold that emitted it. A crackle can never
//      sustain the crackling; it dies with the real source's charge, which
//      falls by sim.elecDecay a tick.
// Rule 1: integer, keyed on the SLOT index (its own salt), reads at distance
// <= 1 plus the cell's own P. Rule 2: everything here consumes the cell or
// costs the field nothing; a cell holds its chunk awake only while a roll is
// possible, and P (with no source) falls to 0 in P / decay ticks.
@group(0) @binding(57) var<storage, read> elecParams : array<u32>;
const EP_MODE : u32 = 0u;
const EP_WET : u32 = 16u;
const EP_MAT : u32 = 32u;
const EP_MAT_STRIDE : u32 = 4u;
const EP_REACT_MIN : u32 = 4u;
const EP_REACT_FULL : u32 = 5u;
const EP_IGNITE_Q : u32 = 6u;
const EP_CRACKLE_Q : u32 = 7u;
const EP_CRACKLE_LO_P : u32 = 8u;
const EP_CRACKLE_LO_MAT : u32 = 9u;
const EP_CRACKLE_HI_P : u32 = 10u;
const EP_CRACKLE_HI_MAT : u32 = 11u;
const EP_ELEC_TAG : u32 = 12u;
const ELEC_P_UNREAD : u32 = 0xFFFFFFFFu;
const ELEC_IGNITE_SALT : u32 = 0x5E1EC7A1u;
const ELEC_CRACKLE_SALT : u32 = 0x3C4A7B1Fu;
// The most a cell crackles a tick: 100 per-mille (in 1/REACT_CHANCE_DEN).
const ELEC_CRACKLE_CAP : u32 = 200000u;

// The resist the field used for the cell holding w at world cell c (the
// shared rule, MIRROR elec elecResistRaw: material, wet coat, brine), 0 for an
// insulator.
fn elecResistOf(w : u32, c : vec3<i32>) -> u32 {
  return elecResistRaw(w, voxSlotOfCell(c), solLocalOf(c));
}
// The P a cell can hold: only a conductor (or a wet cell) is ever charged
// short of being a source, so everything else skips the field read.
fn elecCellCharge(c : vec3<i32>, w : u32, m : Material) -> u32 {
  if (elecParams[EP_MODE] == 0u) { return 0u; }
  if ((m._r2 & ELEC_R2_CONDUCTS) == 0u && !voxStained(w)) { return 0u; }
  return elecAtCell(c);
}
// Is this PAIR rule's neighbour a discharge (tag:electric, any material of
// it, a gas may be one)? Then charge at the cell can stand in for it.
fn elecRuleWantsDischarge(rule : Reaction) -> bool {
  let tag = elecParams[EP_ELEC_TAG];
  if (tag == 0u || rule.nbrMat != NBR_ANY || (rule.nbrTags & tag) == 0u) { return false; }
  return rule.nbrClass == 0u || ((1u << CLASS_GAS) & rule.nbrClass) != 0u;
}
// The authored chance (already weather-scaled), ramped by P up to
// EP_REACT_FULL. base <= REACT_CHANCE_DEN (2e6) and q < 1024: no overflow.
fn elecRampChance(base : u32, p : u32) -> u32 {
  let full = max(elecParams[EP_REACT_FULL], 1u);
  if (p >= full) { return base; }
  let q = (p << 10u) / full;
  return (base * q) >> 10u;
}
// The neighbour-side product of a rule fired through the virtual neighbour:
// into the first AIR face (RNG-rotated, the rule's direction mask), or lost.
fn elecDeposit(c : vec3<i32>, dmask : u32, rot : u32, prod : u32, rr : u32, stamp : u32) {
  if (prod == PROD_KEEP || prod == MAT_AIR) { return; }
  for (var i = 0u; i < 6u; i++) {
    let di = (i + rot) % 6u;
    if ((faceDirBit(di) & dmask) == 0u) { continue; }
    let n = c + faceDir(di);
    if (!inBounds(n) || voxMat(voxWordAt(n)) != MAT_AIR) { continue; }
    let ni = voxWordIndex(n);
    voxStore(ni, packVox(prod, productState(prod, rr >> 4u), stamp));
    if (solIsLiquidMat(prod)) { solClearStale(n); }
    markVoxActive(ni);
    markDirtyR(n, DIRTY_R_REACTW);
    return;
  }
}

fn markVoxActive(idx : u32) {
  if (T.vizActive != 0u && idx != PT_NO_WORD) {
    atomicOr(&actVoxViz[idx >> 5u], 1u << (idx & 31u));
  }
}

// Unloaded space is solid and inert (DESIGN.md §3): the sim's world edge is
// the residency window, not a fixed cube.
fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

// flagSupportLoss now lives in common.wgsl (the SUPPORT_LOSS block): the
// MutationQueue and blast kernels raise it too, and two copies of the rule
// was the bug. `supportOut` is still declared above, which is the predicate
// LoadShader strips the block on.


// Mark the chunk containing world cell c dirty for next tick, plus every
// neighbor chunk c borders (cross-chunk neighbors re-evaluate; sleeping is
// per-chunk). Chunks outside the residency window don't exist to mark.
//
// `reason` is one of common.wgsl's DIRTY_R_* bits and is ORed in rather than
// stored, so a chunk's flag names every rule that asked for it this tick. See
// the block there: the plain markDirty() below keeps the old call shape for
// the write paths, which are the overwhelming majority of the call sites and
// the only ones that also markVoxActive.
//
// NON-WRITE REASONS MARK ONLY THE OWN CHUNK (2026-09-23). REACT (matched, did
// not fire), STAIN (unsaturated surface in reach), VISCOUS (off-tick with
// somewhere to go) and the retired FLOW say "THIS cell still has work", and
// that work is done by this cell, in this chunk. Nothing was written, so no
// neighbour's view changed and there is nothing for it to re-evaluate; a
// neighbour chunk that has work of its own marks itself, and the moment this
// cell's work IS a write, that write fans out as before. Fanning these out
// used to hold up to 7 innocent chunks awake beside every idle reactor. None
// of the four is in FILM_LICENCE, and the repose snapshot is keyed on the
// dispatch list rather than on who marked it, so neither depends on the fan.
// DIRTY_R_SOLUTE joins them for the CA's one non-write use of it: a dissolve
// that found no page this tick asks for one (SOLM_REQ_FLAG) and keeps only its
// OWN chunk awake to take it next tick.
const DIRTY_OWN_CHUNK_ONLY : u32 =
    DIRTY_R_REACT | DIRTY_R_STAIN | DIRTY_R_FLOW | DIRTY_R_VISCOUS | DIRTY_R_SOLUTE |
    DIRTY_R_DRY;

fn markDirtyR(c : vec3<i32>, reason : u32) {
  if ((reason & ~DIRTY_OWN_CHUNK_ONLY) == 0u) {
    let own = dirtyFanSlot(c, T.origin, 0u);  // k = 0: c's own chunk
    if (own != SLOT_NONE) { atomicOr(&dirtyOut[own], reason); }
    return;
  }
  // The fan-out rule is common.wgsl's dirtyFanSlot: only the distinct chunks
  // c borders, so an interior cell pays one atomic instead of eight.
  for (var k = 0u; k < 8u; k++) {
    let ns = dirtyFanSlot(c, T.origin, k);
    if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], reason); }
  }
}

fn markDirty(c : vec3<i32>) { markDirtyR(c, DIRTY_R_WRITE); }

// ---- WHICH STAGE of stepLiquid moved the mass, and was the mover SUBMERGED --
//
// DIRTY_R_MOVE established that a pond's chunks stay awake because water is
// genuinely still being written, every tick, forever. It cannot say WHICH of
// the seven liquid moves does it, and those have completely different
// termination arguments: descent strictly decreases SUM(f*y), equalize / split
// / bridge strictly decrease SUM(f*f), and the two film rules are NEUTRAL in
// both. Only a neutral move can cycle, so naming the stage names the bug.
//
// SUBMERGED is the owner's invariant, measured rather than assumed: a cell with
// the same liquid directly above it should be FULL, so it should never be the
// SOURCE of a lateral transfer at all. If the lateral stages come back firing
// on submerged cells, the interior of every pond is levelling itself sideways
// forever and stage 3 has to become a free-surface stage.
//
// Local to this file rather than common.wgsl on purpose: common.wgsl is
// prepended to every shader, so editing it invalidates the whole SPIR-V cache
// and costs the ~9-minute worldgen far-cascade recompile. Diagnostic only —
// nothing branches on these.
const DIRTY_M_DOWN      : u32 = 4096u;    // stage 1: straight down
const DIRTY_M_DIAG      : u32 = 8192u;    // stage 2: axis/corner down-diagonals
const DIRTY_M_EQUAL     : u32 = 16384u;   // stage 3: same-liquid equalize
const DIRTY_M_SPLIT     : u32 = 32768u;   // stage 3: halve into air
const DIRTY_M_FILM      : u32 = 65536u;   // stage 3: film step off a RISER
const DIRTY_M_DISPLACE  : u32 = 131072u;  // stage 3: displace a lighter fluid
const DIRTY_M_BRIDGE    : u32 = 262144u;  // stage 4: level two neighbours
const DIRTY_M_SUBMERGED : u32 = 524288u;  // the mover had its own liquid above
// Bit 20 WAS DIRTY_M_SPILL — the submerged whole-cell spill, retired, and the
// block in stepLiquid that says why it is not a rule is still there. It now
// carries the OTHER film branch, because "film" naming two rules with two
// completely different termination arguments is exactly the blank CLAUDE.md
// rule 6 is about: one of them is provably finite and the other was the
// never-sleeping shoreline. `film` is the riser step, `film-press` is the
// pressed one, and a histogram that says which is a diagnosis instead of a
// direction to go looking.
const DIRTY_M_FILMPRESS : u32 = 1048576u; // stage 3: film step under PRESSURE
// The non-liquid movers, so "MOVE with no liquid stage" stops being a blank.
const DIRTY_M_POWDER    : u32 = 2097152u;
const DIRTY_M_GAS       : u32 = 4194304u;
const DIRTY_M_SOLO      : u32 = 8388608u;  // a one-voxel solid island falling

// ---- WHY A GAS CHUNK IS AWAKE (docs/PLAN_gas_particles.md P0) -------------
//
// DIRTY_M_GAS existed before this and was a BLANK for the case it is most
// wanted in. It was set at exactly two of the gas ladder's five stages — the
// bare rise and the four up-diagonals — and a plume in calm air never reaches
// either: gasIntent returns straight up, the tryMove in front of the chain
// succeeds, and the cell returns having marked only DIRTY_R_MOVE. So the
// `sleep` gate's "gas" column read 0 for a world full of rising smoke, and the
// top-plane SHEET — which is stage 3, the flat lateral ring, and also never
// marked — was invisible in the histogram that exists to name it.
//
// Now every successful gas move marks, split by the axis it moved on:
//
//   DIRTY_M_GAS     the move changed the parcel's HEIGHT (rise, sink, or an
//                   up-diagonal). Progress: a gas that keeps rising leaves
//                   through the ceiling or decays, so this terminates.
//   DIRTY_M_GASLAT  the move was purely horizontal. This is the sheet, and it
//                   is NEUTRAL — a gas spreading flat under a lid can do it
//                   forever, which is exactly the shape of the never-sleeping
//                   chunk this plan exists to delete.
//   DIRTY_M_GASEDGE the parcel's PRIMARY intent pointed out of the residency
//                   window. Whether it converted to a particle or was refused
//                   by the per-tick budget, this names the cells at the sink.
//
// Only DIRTY_M_GAS is in FILM_LICENCE, and the split above is why: the licence
// means "something that decreased a Lyapunov function happened here", and a
// flat gas spread decreases nothing. GASLAT and GASEDGE are diagnostics only —
// adding them to the licence would let a sheet license a film step, which is
// the precise defect the licence was written to remove.
const DIRTY_M_GASLAT    : u32 = 16777216u;  // gas moved HORIZONTALLY (the sheet)
const DIRTY_M_GASEDGE   : u32 = 33554432u;  // gas wanted out of the window

// ---- THE FILM LICENCE: a neutral rule may not license ITSELF ---------------
//
// THE BUG, measured at the authored home_lake on 2026-09-08. The owner, from
// the live game: F6 lights a handful of chunks permanently and the active-voxel
// overlay inside them is empty. `sleep` printed
//
//   awake by reason (14 chunks): MOVE 14 film-press 14
//   awake chunks over 20 more ticks: 47 words changed in 14 chunks
//     (material 10, fullness 0, stain 0, STAMP-ONLY 37)
//
// and then, because a count is not a measurement (CLAUDE.md rule 6), the plan
// view the same gate now dumps — the movers' own level beside the level under
// them:
//
//     .1122   88888
//     .1.22   88888
//     .1122   88888   <- the cell's row
//     .1112   88888
//     ..112   88888
//
// FULL WATER UNDERNEATH. This is not a puddle in a hollow and it is not the
// 2-wide gutter the riser step cannot resolve — it is the TOP LAYER OF THE
// LAKE, a patchwork of one- and two-eighth cells with holes in it, endlessly
// rearranging itself over a surface it can never finish levelling. 37 of 47
// changed words came back to the value they started with: a shuffle, not work.
//
// WHICH RULE. `film-press`, and the bit that says so was split out of `film`
// for this: the two film branches have completely different termination
// arguments and a histogram that folds them together names neither. The first
// repair went to the RISER branch on an inference from `fullness 0` — every
// filmPressed advance is supposed to be followed by the split that justifies
// it, and a split moves fullness, so nothing splitting looked like nothing
// pressing. Wrong, and wrong in the informative direction: the splits are not
// happening, and THAT IS THE BUG.
//
// WHY THE PROOF FAILS. filmPressed's argument (see its block) is that the cell
// the film VACATES is a face neighbour of the cell that pressed it, which holds
// >= LIQ_SPLIT_MIN and CAN therefore split into the hole — strictly decreasing
// SUM(f*f). "Can" is not "does". On an open surface there are films on every
// side of that hole, and one of THEM advances into it first; the pressing cell
// still holds its two eighths, nothing split, SUM(f*f) is unchanged, and the
// configuration has merely rotated. A potential argument that assumes the hole
// waits its turn is not a termination argument on a crowd.
//
// The riser branch (filmStepAllowed) has the same shape of hole in it, admitted
// in its own block: two risers facing each other exactly 3 apart is a 2-cycle
// no reach-1 predicate can break, because a terrace tread's inner cell and a
// 2-wide rimmed gutter's cell have byte-identical 3x3x3 neighbourhoods and the
// one read that separates them, `c + 2d`, is racy by construction — the colour
// lattice keeps acting cells >= 3 apart and each writes within 1, so a cell
// exactly 2 away is exactly the cell another thread may be writing this pass.
//
// SO GATE BOTH ON PROGRESS INSTEAD OF ON GEOMETRY. Both film moves are NEUTRAL
// in both of this file's Lyapunov functions (same SUM(f*y), same SUM(f*f)) —
// they only relocate a film. Every OTHER liquid rule strictly decreases one of
// them. So the whole defect is that a neutral move can be its own cause, and
// the repair is to say it cannot: a film may only step in a chunk where
// something that DID decrease a Lyapunov function happened last tick.
//
// TERMINATION, and it is the whole point. Every bit below is either a move that
// strictly decreases a bounded integer (descent SUM(f*y); equalize / split /
// bridge SUM(f*f); powder and gas their own) or an EXTERNAL input (a mutation,
// the seam, a particle landing, a reaction firing). None of them can be caused
// by a film step. So in a world with no new input the licence is granted on
// finitely many ticks, hence finitely many film steps happen, hence the chunk
// sleeps. The rules keep every case they were written for — while a pour, a
// spill or a dome is actually draining the chunk is full of descents and
// splits, the licence is on every tick, and the films step exactly as before —
// and lose only the case they never should have had: being the last thing
// awake, moving a third of a voxel per tick, forever, with nothing to show.
//
// WHAT IT COSTS, measured rather than assumed: `ca-slope` is unchanged to the
// digit (96.9% of the pour in the basin, 0 eighths left on the ramp, quiet from
// tick 90), and so are ca-level-one and ca-level. A drain is a chunk full of
// progress; that is exactly when the licence is granted.
//
// WHAT IS DELIBERATELY *NOT* IN THE SET, because it is the trap: DIRTY_R_FLOW
// and DIRTY_R_VISCOUS. Those are the settled path's "this cell HAS an option"
// marks, not "this cell DID something", and canFlowAnywhere reports the riser
// step as an option — so licensing on either would let the oscillation license
// itself one tick later and buy nothing but a longer period. DIRTY_R_MOVE is
// out for the same reason (a film step is a move). DIRTY_R_REACT and
// DIRTY_R_STAIN are the matched-but-did-not-fire / unsaturated-neighbour idle
// marks; their WROTE counterparts are what count.
//
// ---- AND DIRTY_M_DISPLACE WAS IN THE SET AND SHOULD NEVER HAVE BEEN ---------
//
// Removed 2026-09-16, on the owner's report of a desert pond whose shore never
// settles: "water voxels just swap back and forth really really fast endlessly,
// keeping the simulation alive in all of those chunks."
//
// THE MEASUREMENT, from `--gate pond-shore` pass C (the owner's own shore, one
// splash, day phase pinned to noon), against passes A and B at the same site
// which both go quiet:
//
//   5 of 507 shore chunks awake at tick 300 (quiet from -1)
//     by reason: MOVE 2 displace 2
//     2 words changed / 1 BACK where they started
//     first at (-2344,112,1670) water<->steam
//       y+0  ~~~~~~~   ~~~~~^~     <- the flat pond SURFACE
//
// A water cell and a steam cell trading places, forever. `displace` is stage 3's
// lighter-fluid branch, and read what that move is: a LATERAL, WHOLE-CELL swap
// of two fluids. Both cells keep their y, so SUM(f*y) is unchanged. Both keep
// their fullness, so SUM(f*f) is unchanged. IT IS NEUTRAL IN BOTH — which is the
// exact property this whole block exists to say a move may not have while also
// being its own cause.
//
// So the set contained a member that satisfies neither half of its own
// definition: `displace` is not a strict decrease of a bounded integer, and it
// is not an external input. Two consequences, and the second is the one that
// cost the owner a pond:
//   * it licensed ITSELF, so once a shore started swapping it never stopped;
//   * it licensed the FILM steps, so the 2-cycle `ca-gutter` proves cannot
//     terminate on its own got a standing permit from an unrelated rule.
//
// WHAT STARTS IT is daylight, and that is why no existing gate could see it.
// reactions.json authors evaporation as water -> steam `when: "day"` with
// `minCount: 4` non-water faces; `ca-gutter` and `ca-slope` both PIN THE PHASE
// TO A DIM DAWN and say why ("freezing and evaporation are authored mass sinks
// and would make the audit inexact"). Correct for an audit, and it means every
// liquid gate in the engine ran with the one rule that seeds the churn switched
// off. `pond-shore` pass C pins noon instead.
//
// The branch itself is licensed now too (see stepLiquid stage 3) — removing the
// bit alone would stop it licensing the films while leaving it free to license
// itself through some other chunk's genuine work.
const FILM_LICENCE : u32 =
    DIRTY_R_WRITE | DIRTY_R_SEAM | DIRTY_R_PARTICLE | DIRTY_R_WATERBODY |
    DIRTY_R_MUTATE | DIRTY_R_STAINW | DIRTY_R_DRYW | DIRTY_R_REACTW |
    DIRTY_M_DOWN | DIRTY_M_DIAG | DIRTY_M_EQUAL | DIRTY_M_SPLIT |
    DIRTY_M_BRIDGE | DIRTY_M_POWDER | DIRTY_M_GAS |
    DIRTY_M_SOLO;

// Set once per workgroup at the top of main from dirtyIn[ci]; read by BOTH film
// predicates, which is what makes canFlowAnywhere inherit it for free — the
// moving path and the settled path MUST agree or a chunk either pins awake or
// sleeps with work left.
var<private> gFilmLicence : bool = false;
// ---- THIN SMOKE DISSIPATES (2026-10-03, fire-gpu; sim.gasThinDecayMul) -----
// A buoyant, cold gas voxel with at most sim.gasThinNeighbors gas voxels on
// its six faces is THIN, and its fade-to-air rules (DECAY rules whose product
// is air: smoke's 9, black smoke's 6 per mille) roll at gasThinDecayMul times
// their authored chance. The dense core of a plume is untouched; the haze it
// sheds -- most of a big fire's awake chunks, and most of what the raymarch
// wades through -- fades sooner. Measured on --perf village-fire at x3: gas
// cells run -32%, awake chunks -8%, CA -0.9 ms, raymarch -1.3 ms, frame p50
// -2.8 ms (DESIGN.md, "The fire's CA, attributed").
//
// AT x1 (THE DEFAULT) THIS IS COMPILED OUT: GAS_THIN_ON is a const, so the
// world, its hash and the shader's register count are today's.
//
// THE NEIGHBOUR COUNT READS THE AIR MASK, NOT THE VOXELS: camask's gas plane
// for this chunk (gas at the start of this substep -- substep 0, which is when
// reactions roll), one word per face. A face in ANOTHER chunk counts as gas,
// so a cell on a chunk face is judged on its in-chunk faces only and can only
// be called thin less often, never more. Deterministic (rule 1): the mask is
// a pure function of the voxels at the substep's start, whatever the schedule.
// Not hot (a flame is a gas: fire must not burn out faster at its fringe) and
// not heavy (a pooled heavy gas is a hazard someone placed).
const GAS_THIN_ON : bool = TUNE_GAS_THIN_DECAY_MUL > 1u;
var<private> gThinMul : u32 = 1u;
fn caMaskGasAt(ci : u32, l : vec3<i32>) -> bool {
  let i = u32(l.x) + 16u * u32(l.y) + 256u * u32(l.z);
  return ((caMask[ci * CA_MASK_STRIDE + CA_MASK_WORDS + (i >> 5u)] >> (i & 31u)) & 1u) != 0u;
}
fn gasThinMul(ci : u32, local : vec3<i32>, m : Material) -> u32 {
  if ((m.flags & MATF_HEAVY_GAS) != 0u) { return 1u; }
  if (((m._r2 >> HEAT_R2_EMIT_SHIFT) & 0xFFu) != 0u) { return 1u; }
  var n = 0u;
  for (var f = 0u; f < 6u; f++) {
    let l = local + faceDir(f);
    if (any(l < vec3<i32>(0)) || any(l >= vec3<i32>(i32(CHUNK)))) { n++; continue; }
    if (caMaskGasAt(ci, l)) { n++; }
  }
  return select(1u, TUNE_GAS_THIN_DECAY_MUL, n <= TUNE_GAS_THIN_NEIGHBORS);
}

// The acting cell and its physical word index, set once at the top of main.
// tryMove's source is always this cell, and re-resolving it through the page
// table on every move was a second lookup of an answer main already had.
var<private> gSelfCell : vec3<i32> = vec3<i32>(0);
var<private> gSelfIdx : u32 = PT_NO_WORD;

// ---- REACTIONS IN A TICKET (docs/PLAN_chunk_tickets.md §2.5, P3) ----------
// Set per cell at the top of main: is the chunk this thread acts on a TICKET
// slot (resident outside the window)? In a ticket only DECAY rules run. Decay
// and movement TERMINATE — bounded by the matter present — while emit and pair
// rules PROPAGATE, bounded only by fuel, and fuel runs past a ticket's edge:
// fire spreading in a ticket would either grow the ticket without bound (rule
// 2) or stop at a straight chunk-aligned wall. So an ember in a ticket burns
// out to ash and does NOT light the wood beside it, smoke fades, and a ticket
// holding only pair/emit-reactive matter sleeps and releases. A skipped rule
// does not set keepAwake (it cannot fire here, so it is no reason to wake).
// One compare against the slot index (ci >= NUM_CHUNKS), uniform per chunk.
var<private> gInTicket : bool = false;

// Is there more of this same liquid directly ABOVE c? Out of window reads as
// "no": the residency edge is solid and inert, so a cell at the top of the
// window counts as a free surface, which is the conservative direction — it may
// still move, it is not frozen.
fn liquidSubmerged(c : vec3<i32>, mat : u32) -> bool {
  let a = c + vec3<i32>(0, 1, 0);
  if (!inBounds(a)) { return false; }
  return voxMat(voxWordAt(a)) == mat;
}

// Can a mover of (klass, density) enter the cell holding word tw?
// rising=true for gases (they seek lower density above), false for falling.
// Is this cell a component of ONE — no solid and no powder on any of its six
// faces? See the long note at the call site for why this is the whole island
// test for the degenerate case, and why six distance-1 reads are inside the
// colour lattice's guarantee when a longer walk would not be.
//
// PROBES DOWNWARD FIRST, and that ordering is the cost argument rather than a
// style choice: this runs for every awake solid cell, and essentially every
// solid cell in a terrain chunk has ground directly beneath it, so the common
// case exits after ONE extra load instead of six.
fn soloSolid(c : vec3<i32>, selfPassable : bool) -> bool {
  // below, the four laterals, then above: cheapest rejection first
  var off = array<vec3<i32>, 6>(
      vec3<i32>(0, -1, 0), vec3<i32>(1, 0, 0), vec3<i32>(-1, 0, 0),
      vec3<i32>(0, 0, 1), vec3<i32>(0, 0, -1), vec3<i32>(0, 1, 0));
  for (var i = 0u; i < 6u; i = i + 1u) {
    let n = c + off[i];
    // Cannot see past the residency window, so assume attached: the same
    // direction island detection takes when a chunk is unfetched.
    if (!inBounds(n)) { return false; }
    let nm = voxMat(voxWordAt(n));
    if (nm == MAT_AIR) { continue; }
    let k = materials[nm].klass;
    // PASSABLE VEGETATION HOLDS NOTHING UP (see tryCrush): a chip resting on,
    // or leaning on, a flower is still a component of one. A plant's own
    // attachment to plants is untouched (selfPassable), so a vine or a grass
    // head is still held by the stem it grows from.
    if (k == CLASS_SOLID &&
        (selfPassable || (materials[nm].flags & MATF_PASSABLE) == 0u)) {
      return false;
    }
    // Powder only counts BELOW (i == 0): sand beside a voxel does not hold it
    // up, and RunIslandDetection's anchor rule already says the same (resting
    // ON powder anchors; powder alongside does not). Counting every face let a
    // lone leaf with ash beside it refuse to fall, while the ash's own flag
    // logic never summoned a scan for it -- see flagSupportLoss.
    if (k == CLASS_POWDER && i == 0u) { return false; }
  }
  return true;
}

fn canDisplace(myDensity : i32, rising : bool, tw : u32) -> bool {
  let tmat = voxMat(tw);
  var td : i32;
  if (tmat == MAT_AIR) {
    td = AIR_DENSITY;
  } else {
    let t = materials[tmat];
    if (t.klass == CLASS_SOLID || t.klass == CLASS_POWDER) { return false; }
    td = t.density;
  }
  if (rising) { return td > myDensity; }
  return td < myDensity;
}

// ---- ADVECTION: dissolved mass rides the liquid that carries it -------------
// Each of these runs on cells the acting thread is ALREADY writing (tryMove's
// and transferLiquid's src/dst, a reaction's self/neighbour), so they inherit
// the colour lattice's disjointness: no other thread of this pass touches
// either cell's mass. (Two cells share a pool word, hence solStoreAt's atomics.)

// Mass that must go because the thing carrying it is gone, counted so the
// ledger still balances (sim_solute.wgsl SOLM_DISCARDED).
fn solDiscard(units : u32) {
  if (units != 0u) { atomicAdd(&solMeta[SOLM_DISCARDED], units); }
}

// tryMove's half. The two cells trade places and each one's mass goes WITH its
// matter: a sinking grain of sand lifts the brine it displaces, brine it falls
// through keeps its salt, a bubble rising through a pond leaves the pond's
// salt behind. A cell that is not a liquid carries nothing -- a value found on
// one is mass a destroyed solvent left behind, and is discarded here.
fn solSwap(src : vec3<i32>, dst : vec3<i32>, srcWord : u32, dstWord : u32) {
  let sl = solIsLiquidMat(voxMat(srcWord));
  let dl = solIsLiquidMat(voxMat(dstWord));
  if (!sl && !dl) { return; }
  let rs = solValueAt(src);
  let rd = solValueAt(dst);
  if (rs == 0u && rd == 0u) { return; }
  let vs = select(0u, rs, sl);
  let vd = select(0u, rd, dl);
  solDiscard(solMassOf(rs ^ vs) + solMassOf(rd ^ vd));
  // After the swap `dst` holds the source's matter and `src` the target's.
  _ = solStoreAt(dst, vs);
  _ = solStoreAt(src, vd);
}

// transferLiquid's half: t of the source's sf eighths move onto the target, and
// the same FRACTION of its mass goes with them -- concentration is unchanged
// on both sides, which is what a liquid moving as a body does. The last eighth
// takes all of it. Integer floor, so a source keeps the remainder; nothing is
// created and nothing is lost. A target of a DIFFERENT species keeps its own
// (immiscible, PLAN_solutes §2.3) and REFUSES the arriving mass, which stays
// in the source; only a source that left entirely has it discarded, counted.
fn solTransfer(src : vec3<i32>, dst : vec3<i32>, sf : u32, df : u32, t : u32) {
  let rs = solValueAt(src);
  let rd = solValueAt(dst);
  if (rs == 0u && rd == 0u) { return; }
  // df == 0: the target is AIR, and a value on it is stale.
  var vd = rd;
  if (df == 0u) {
    solDiscard(solMassOf(rd));
    vd = 0u;
  }
  let ms = solMassOf(rs);
  let ss = solSpeciesOf(rs);
  var moved = ms;
  if (t < sf) { moved = (ms * t) / sf; }
  var nd = vd;
  if (moved != 0u) {
    let sd = solSpeciesOf(vd);
    if (sd != 0u && sd != ss) {
      // Immiscible: the arriving mass is REFUSED, not destroyed -- it stays
      // in what is left of the source (whose concentration rises; capped at
      // 255 already, since it is the source's own mass). Only when the whole
      // source left (t == sf: it becomes air) is there nowhere for it to
      // stay, and it is discarded, counted. Destroying it every transfer
      // would let brine levelling into sugar water delete the salt.
      if (t < sf) { moved = 0u; } else { solDiscard(moved); }
    } else {
      let tot = solMassOf(vd) + moved;
      nd = solPack(ss, min(tot, 255u));
      if (tot > 255u) { solDiscard(tot - 255u); }
    }
  }
  _ = solStoreAt(dst, nd);
  _ = solStoreAt(src, solPack(ss, ms - moved));
}

// A value on a cell that is about to become something new from nothing (an
// emission into air). Only a stale value can be there; drop it so the new
// matter does not inherit a destroyed solvent's mass.
fn solClearStale(c : vec3<i32>) {
  let v = solValueAt(c);
  if (v == 0u) { return; }
  solDiscard(solMassOf(v));
  _ = solStoreAt(c, 0u);
}

// ---- THE PHASE CHANGE: a solvent the CA turns into something else ------------
// Cell `c` carried value `v` (non-zero) and is being rewritten to `prod`. What
// happens to the mass is the physics of evaporation and boiling:
//   * `prod` is itself a solvent of the species (water -> enchanted water):
//     the mass stays where it is, untouched;
//   * otherwise, if `push`, it moves into face neighbours that are solvents of
//     the species with room under saturation -- a pond that evaporates from the
//     top CONCENTRATES rather than losing its salt;
//   * what cannot move PRECIPITATES: the cell becomes the species'
//     `precipitatesTo` powder with one eighth per yieldPerVoxel/8 units (so the
//     salt that went in comes out as the same amount of salt). A remainder
//     under one eighth, or a species with no precipitate, is discarded and
//     counted.
// Returns (material, eighths): 0 eighths means "write `prod` as the caller
// would have"; otherwise write the precipitate with that many eighths.
// `push` only for the ACTING cell: its neighbours are within the lattice's
// write reach; a rule's NEIGHBOUR is not, so its mass cannot move again.
fn solOnReplace(c : vec3<i32>, v : u32, prod : u32, push : bool, rot : u32) -> vec2<u32> {
  let s = solSpeciesOf(v);
  var m = solMassOf(v);
  if (solIsLiquidMat(prod) && solIsSolvent(prod, s)) { return vec2<u32>(prod, 0u); }
  if (push) {
    for (var i = 0u; i < 6u && m > 0u; i = i + 1u) {
      let n = c + faceDir((i + rot) % 6u);
      if (!inBounds(n)) { continue; }
      let nw = voxWordAt(n);
      let nmat = voxMat(nw);
      if (!solIsLiquidMat(nmat) || !solIsSolvent(nmat, s) || !solWritable(n)) { continue; }
      let nv = solValueAt(n);
      if (solSpeciesOf(nv) != 0u && solSpeciesOf(nv) != s) { continue; }
      let cap = solCap(s, voxState(nw) + 1u);
      let have = solMassOf(nv);
      if (have >= cap) { continue; }
      let k = min(m, cap - have);
      _ = solStoreAt(n, solPack(s, have + k));
      m -= k;
      markDirty(n);
    }
  }
  _ = solStoreAt(c, 0u);
  var out = vec2<u32>(prod, 0u);
  if (m != 0u) {
    let y8 = solYield8(s);
    let p = solPrecipitate(s);
    var e = min(m / y8, POWDER_FULL);
    // A precipitate that cannot hold eighths is whole cells or nothing.
    if (p != MAT_AIR && !matHasPowderMass(materials[p]) && e < POWDER_FULL) { e = 0u; }
    if (e != 0u && p != MAT_AIR) {
      out = vec2<u32>(p, e);
      atomicAdd(&solMeta[SOLM_PRECIP], e * y8);
      m -= e * y8;
    }
    // What is left is less than an eighth of a grain (or has no precipitate):
    // it cannot leave as powder, so it stays in the liquid -- the brine beside
    // this cell takes it past saturation (a supersaturated film, which is what
    // a drying pan really holds). Only mass with no liquid of its kind to go
    // to is discarded, counted.
    if (push) {
      for (var i = 0u; i < 6u && m > 0u; i = i + 1u) {
        let n = c + faceDir((i + rot) % 6u);
        if (!inBounds(n)) { continue; }
        let nw = voxWordAt(n);
        let nmat = voxMat(nw);
        if (!solIsLiquidMat(nmat) || !solIsSolvent(nmat, s) || !solWritable(n)) { continue; }
        let nv = solValueAt(n);
        if (solSpeciesOf(nv) != 0u && solSpeciesOf(nv) != s) { continue; }
        let have = solMassOf(nv);
        let k = min(m, 255u - have);
        if (k == 0u) { continue; }
        _ = solStoreAt(n, solPack(s, have + k));
        m -= k;
        markDirty(n);
      }
    }
    solDiscard(m);
  }
  return out;
}

// The state nibble for a precipitate of `e` eighths at cell c.
fn solPrecipState(p : u32, e : u32, c : vec3<i32>) -> u32 {
  if (matHasPowderMass(materials[p])) { return powderStateFor(e, c, ptSeed()); }
  return 0u;
}

// ---- DISSOLVING: a powder named as a solute's `from` meets a solvent ----------
// The ACTING cell is the powder (salt, fairy dust). Faces are tried from a
// rotated start; the first that is a solvent of its species, holding no other
// species and with room for at least one eighth under saturation, takes up to
// the whole cell at dissolveChance per mille per tick. The powder loses those
// eighths (an eighth is yieldPerVoxel/8 units: the contract's unit bridge) and
// the solvent gains them, so the ledger moves the same integer both ways.
//
// A solvent in a chunk with no page cannot take mass THIS tick: the powder
// asks for one (SOLM_REQ_FLAG, read by next tick's solWant) and keeps its own
// chunk awake. Whether a chunk has a page is itself deterministic, so the
// one-tick deferral is too.
//
// Returns true when the cell dissolved (fully or in part): it has acted.
fn solTryDissolve(c : vec3<i32>, idx : u32, w : u32, mat : u32, m : Material,
                  s : u32, slotIdx : u32, probe : bool) -> bool {
  let rr = hash3(T.seed ^ 0x501D155Du, T.tick, slotIdx);
  let rot = (rr >> 12u) % 6u;
  let pm = select(POWDER_FULL, powderMass(w), matHasPowderMass(m));
  let y8 = solYield8(s);
  for (var i = 0u; i < 6u; i = i + 1u) {
    let n = c + faceDir((i + rot) % 6u);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    let nmat = voxMat(nw);
    if (!solIsLiquidMat(nmat) || !solIsSolvent(nmat, s)) { continue; }
    let nv = solValueAt(n);
    if (solSpeciesOf(nv) != 0u && solSpeciesOf(nv) != s) { continue; }
    let cap = solCap(s, voxState(nw) + 1u);
    let have = solMassOf(nv);
    if (have + y8 > cap) { continue; }   // saturated for a whole eighth
    let k = min(pm, (cap - have) / y8);
    // A powder with no eighths dissolves whole or not at all.
    if (!matHasPowderMass(m) && k < POWDER_FULL) { continue; }
    // Stamped this substep already: only say there is work here.
    if (probe) {
      markDirtyR(c, DIRTY_R_REACT);
      return false;
    }
    // No page there yet: ask for one BEFORE rolling. Rolling first made the
    // dissolve need two successes in a row (one to ask, one on the tick the
    // page exists -- an all-zero page is handed back at the end of the tick
    // it was made), and a lone grain in a clean pond measured 0 dissolved in
    // 1,500 ticks.
    if (!solWritable(n)) {
      atomicStore(&solMeta[SOLM_REQ_FLAG + voxSlotOfCell(c)], 1u);
      markDirtyR(c, DIRTY_R_SOLUTE);
      return false;
    }
    // A solvent contact exists: this cell has work until it fires.
    if ((rr % REACT_CHANCE_DEN) >= solDissolveChance(s)) {
      markDirtyR(c, DIRTY_R_REACT);
      return false;
    }
    let stamp = stampFor(T.tick, P.substep);
    _ = solStoreAt(n, solPack(s, have + k * y8));
    atomicAdd(&solMeta[SOLM_DISSOLVED], k * y8);
    if (k >= pm) {
      voxStore(idx, 0u);
      flagSupportLoss(c, m.klass, MAT_AIR);
    } else {
      voxStore(idx, packVoxKeepStain(mat, powderStateFor(pm - k, c, ptSeed()), stamp, w));
    }
    markVoxActive(idx);
    markDirty(c);
    markDirty(n);
    return true;
  }
  return false;
}

// ---- DENSITY: brine sinks (PLAN_solutes §4.2) -------------------------------
// The solute's share of a FULL cell's density: densityPerUnit * c >> 8, with
// c = mass for a full cell. 0 for no solute or a species that weighs nothing.
fn solDensityOf(v : u32) -> i32 {
  if (v == 0u) { return 0; }
  return (solDensityPerUnit(solSpeciesOf(v)) * i32(solMassOf(v))) >> 8u;
}
// Mark every chunk the cell c borders with DIRTY_R_SOLUTE. markDirtyR treats
// that bit as own-chunk-only (the CA's waiting dissolve), but a solute SWAP
// across a chunk face changes both chunks' state and both must look again.
fn solMarkFan(c : vec3<i32>) {
  for (var k = 0u; k < 8u; k++) {
    let ns = dirtyFanSlot(c, T.origin, k);
    if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], DIRTY_R_SOLUTE); }
  }
}
// The acting FULL cell c (word w, liquid mat) trades its solute with the FULL
// cell of the same liquid directly below when its solute makes it strictly
// denser. `apply` false is canFlowAnywhere's read-only mirror (they must
// agree, or a stratifying pond pins awake or sleeps with work left).
// Different species never trade (immiscible). Both cells are within the
// lattice's write reach of c, and the voxels do not change.
fn solTrySink(c : vec3<i32>, w : u32, mat : u32, apply : bool) -> bool {
  if (voxState(w) + 1u != 8u) { return false; }
  let b = c + vec3<i32>(0, -1, 0);
  if (!inBounds(b)) { return false; }
  let bw = voxWordAt(b);
  if (voxMat(bw) != mat || voxState(bw) + 1u != 8u) { return false; }
  let v = solValueAt(c);
  if (v == 0u) { return false; }
  let vb = solValueAt(b);
  if (vb != 0u && solSpeciesOf(vb) != solSpeciesOf(v)) { return false; }
  if (solDensityOf(v) <= solDensityOf(vb)) { return false; }
  if (!apply) { return true; }
  if (!solWritable(c) || !solWritable(b)) { return false; }
  _ = solStoreAt(b, v);
  _ = solStoreAt(c, vb);
  solMarkFan(c);
  solMarkFan(b);
  return true;
}

// ---- CONVERTING: a solvent concentrated past a `converts` row --------------
// solutes.json `converts: [{solvent, into, cMin}]`: once the ACTING liquid cell
// is that solvent at concentration >= cMin (c = mass * 8 / fullness), it
// BECOMES `into` at the same fullness and the mass is spent in it (counted as
// SOLM_CONVERTED) -- fairy dust dissolved in water to 48 is enchanted water.
// This is the concentration-driven replacement of a plain pair rule: a pinch of
// dust in a lake disperses below cMin and converts nothing.
fn solTryConvert(c : vec3<i32>, idx : u32, w : u32, mat : u32) -> bool {
  let v = solValueAt(c);
  if (v == 0u) { return false; }
  let s = solSpeciesOf(v);
  let f = voxState(w) + 1u;
  let conc = (solMassOf(v) * 8u) / f;
  for (var k = 0u; k < solConvertCount(s); k = k + 1u) {
    let row = solConvertRow(s, k);
    if ((row & 0xFFFu) != mat || conc < (row >> 24u)) { continue; }
    let into = (row >> 12u) & 0xFFFu;
    voxStore(idx, packVoxKeepStain(into, voxState(w), stampFor(T.tick, P.substep), w));
    _ = solStoreAt(c, 0u);
    atomicAdd(&solMeta[SOLM_CONVERTED], solMassOf(v));
    markVoxActive(idx);
    markDirty(c);
    return true;
  }
  return false;
}

fn tryMove(src : vec3<i32>, dst : vec3<i32>, myWord : u32, myDensity : i32, rising : bool) -> bool {
  if (!inBounds(dst)) { return false; }
  // One table resolution for the target's index AND word.
  let dt = voxIndexAndWord(dst);
  let di = dt.x;
  let tw = dt.y;
  if (!canDisplace(myDensity, rising, tw)) { return false; }
  let stamp = stampFor(T.tick, P.substep);
  // Stain travels WITH the voxel, not with the cell: a stained pebble that
  // falls is still stained, and the air it left behind is not. Both sides of
  // the swap therefore carry their own source word's stain bits.
  voxStore(di, packVoxKeepStain(voxMat(myWord), voxState(myWord), stamp, myWord));
  markVoxActive(di);
  // displaced fluid (or air) swaps into the source cell, stamped so it does
  // not act again this tick. Every caller's `src` is the acting cell, whose
  // index main already resolved (gSelf*); anything else resolves here. So
  // does a sentinel source, so a fault record names the right chunk.
  var si = gSelfIdx;
  if (any(src != gSelfCell) || si == PT_NO_WORD) { si = voxWordIndex(src); }
  voxStore(si, packVoxKeepStain(voxMat(tw), voxState(tw), stamp, tw));
  markVoxActive(si);
  solSwap(src, dst, myWord, tw);
  markDirtyR(src, DIRTY_R_MOVE);
  markDirtyR(dst, DIRTY_R_MOVE);
  // a powder sliding out from under a solid may leave it floating
  let myKlass = materials[voxMat(myWord)].klass;
  if (myKlass == CLASS_POWDER) { flagSupportLoss(src, myKlass, voxMat(tw)); }
  return true;
}

// A FALLING GRAIN OR LOOSE CHIP CRUSHES THE PLANT UNDER IT (2026-09-25).
// Passable vegetation (materials.json "passable": grass tufts, flowers,
// bramble, ...) is a full SOLID cell drawn as a small micro-model, so matter
// that came down on one rested a whole voxel up, floating over a plant a
// quarter its height (the owner's report, flesh thrown off a carved body).
// A body walks through these cells; what falls should not stand on them.
//
// So a straight fall into a passable cell takes it and leaves AIR behind: the
// plant is destroyed, not displaced upward (a swap would lift a flower onto
// the grain that crushed it). Straight down only, and only for a mover that is
// not itself passable -- a liquid never reaches here (reeds and kelp stand in
// water) and plants do not crush plants. Write reach is the same 1 cell as
// tryMove's, and the outcome depends on nothing a same-colour neighbour
// writes. Plants are not conserved matter, so dropping one conserves nothing
// that anything counts. Subcritical: each crush consumes a plant cell and the
// mover lands one cell lower, so it cannot recur without new matter falling.
fn tryCrush(src : vec3<i32>, dst : vec3<i32>, myWord : u32) -> bool {
  if (!inBounds(dst)) { return false; }
  let dt = voxIndexAndWord(dst);
  let tmat = voxMat(dt.y);
  if (tmat == MAT_AIR || (materials[tmat].flags & MATF_PASSABLE) == 0u) { return false; }
  let myMat = voxMat(myWord);
  if ((materials[myMat].flags & MATF_PASSABLE) != 0u) { return false; }
  let stamp = stampFor(T.tick, P.substep);
  voxStore(dt.x, packVoxKeepStain(myMat, voxState(myWord), stamp, myWord));
  markVoxActive(dt.x);
  var si = gSelfIdx;
  if (any(src != gSelfCell) || si == PT_NO_WORD) { si = voxWordIndex(src); }
  voxStore(si, packVox(MAT_AIR, 0u, stamp));
  markVoxActive(si);
  markDirtyR(src, DIRTY_R_MOVE);
  markDirtyR(dst, DIRTY_R_MOVE);
  let myKlass = materials[myMat].klass;
  if (myKlass == CLASS_POWDER) { flagSupportLoss(src, myKlass, MAT_AIR); }
  return true;
}

// ===================== POWDER MASS: MERGE AND SINK ==========================
// docs/PLAN_powder_mass.md §2. A powder cell carries 1..8 eighths (common.wgsl
// POWDER MASS), so a grain landing on a PARTIAL powder cell is no longer
// simply blocked by it. Two moves, both onto a partial powder `dst` at reach
// 1, both written by this one thread under the colour lattice exactly as
// transferLiquid writes a liquid's two cells (no claim, no second phase):
//
//   MERGE  dst is the SAME powder. Transfer t = min(f, 8 - nf) eighths down;
//          the source keeps the rest, or becomes air. Grains fill the cell
//          below before they stack a new one, which is what turns a dusting
//          into a surface instead of a column of 1/8 cubes.
//   SINK   dst is a DIFFERENT powder lighter in MASS than me (nf < f): the
//          two words swap whole, stains travelling with their grains. Without
//          it a full cell of sand rests on a 1/8 film of dust as if the film
//          were a whole voxel, floating 7/8 of a cell over it.
//
// TERMINATION (rule 2). Let E = sum over eighths of their height. A merge
// moves t >= 1 eighths down one level (dE = -t); a sink moves f eighths down
// and nf < f up (dE = nf - f < 0). Every move strictly lowers E and no move is
// neutral, so neither can recur without new matter arriving, and a settled
// partial writes nothing -- it sleeps like any grain. Deliberately there is NO
// lateral levelling of powder mass: that is the liquid ladder, it would make
// sand flow like water, and it carries the CA levelling limit with it.
//
// Mites (MATF_WANDER) are not grains and take no part (matHasPowderMass).
fn tryPowderOnto(src : vec3<i32>, dst : vec3<i32>, myWord : u32, m : Material) -> bool {
  if (!matHasPowderMass(m) || !inBounds(dst)) { return false; }
  let dv = voxIndexAndWord(dst);
  let tw = dv.y;
  let tmat = voxMat(tw);
  if (tmat == MAT_AIR || !powderIsPartial(tw)) { return false; }
  let tm = materials[tmat];
  if (!matHasPowderMass(tm)) { return false; }
  let myMat = voxMat(myWord);
  let f = powderMass(myWord);
  let nf = powderMass(tw);
  let stamp = stampFor(T.tick, P.substep);
  var si = gSelfIdx;
  if (any(src != gSelfCell) || si == PT_NO_WORD) { si = voxWordIndex(src); }
  var srcNow = tmat;   // what the source cell holds afterwards
  if (tmat == myMat) {
    let t = min(f, POWDER_FULL - nf);
    if (t >= f) { srcNow = MAT_AIR; }
    // The destination keeps its own stain (the grains already there), as
    // transferLiquid's destination does; a source that empties goes to clean
    // air -- its stain left with its grains.
    voxStore(dv.x, packVoxKeepStain(myMat, powderStateFor(nf + t, dst, ptSeed()), stamp, tw));
    if (t >= f) { voxStore(si, 0u); }
    else { voxStore(si, packVoxKeepStain(myMat, powderStateFor(f - t, src, ptSeed()), stamp, myWord)); }
  } else {
    if (nf >= f) { return false; }
    voxStore(dv.x, packVoxKeepStain(myMat, voxState(myWord), stamp, myWord));
    voxStore(si, packVoxKeepStain(tmat, voxState(tw), stamp, tw));
  }
  markVoxActive(dv.x);
  markVoxActive(si);
  markDirtyR(src, DIRTY_R_MOVE);
  markDirtyR(dst, DIRTY_R_MOVE);
  // The source cell got lighter (or emptied): whatever rested on it may have
  // lost its support, exactly as when tryMove slides a grain out from under.
  flagSupportLoss(src, CLASS_POWDER, srcNow);
  return true;
}


// ================== SUB-VOXEL REPOSE (docs/PLAN_powder_mass.md P5) ===========
// The repose tiers above decide in WHOLE cells, so a pile's surface is a
// staircase of whole voxels. With powder mass in eighths (POWDER MASS), the
// same rule can be stated on COLUMN-TOP HEIGHTS IN EIGHTHS, and then a slope
// rests at the same angle with eighth-voxel steps in it:
//
//   A resting surface cell c (mass f, top at 8y + f) looks at each axis
//   neighbour column's top at its own level or one below -- a partial of the
//   same powder beside it (8y + mL), or a partial of it one down (8y - 8 +
//   mB), or open air standing on a solid / full cell (8y). If the drop
//   D = (8y + f) - top exceeds the material's threshold T (eighths of rise per
//   cell of run, reposeEighths), c sheds k = ceil((D - T) / 2) eighths into
//   that neighbour's top (capped by c's mass and the room there).
//
// T at 45 degrees is 8, so for WHOLE cells (D a multiple of 8) the rule moves
// at D = 16 and holds at D = 8 -- exactly the 1:1 tier. It only ever adds
// moves between whole-cell rest states, it never contradicts one.
//
// TERMINATION. The moved grains leave heights top-k+1..top and land at
// ntop+1..ntop+k with ntop + 2k <= top (k <= D/2, since T >= 3), so every
// moved grain ends strictly lower: E = sum of grain heights strictly falls, no
// move is neutral, and a surface with every D <= T writes nothing and sleeps.
//
// WRITE REACH 1: c, its axis neighbour, or that neighbour's cell below (a
// reach-1 diagonal) -- the same boxes the diagonal move uses. All reads are
// reach 1 and live, so no snapshot is needed.
//
// WORLDGEN IS A FIXED POINT OF THIS (worldgen.wgsl looseStep, same T):
// generated loose tops are 4/8 on an upper step edge, 6/8 on a staircase
// (T >= 8; for T < 8 a staircase top is the firm cover), whole on flats and
// feet, so every upper/lower pair of columns has D <= T and a generated dune
// does not move on tick 1.
// The material's threshold, eighths of rise per cell: reposeEighthsOf in
// common.wgsl, because worldgen.wgsl lays loose cover against the SAME number
// (a generated dune is a fixed point of this rule only if both read it).
fn reposeEighths(m : Material) -> u32 { return reposeEighthsOf(m.repose); }

// Is the diagonal step from a surface cell of mass `f` onto a partial of mass
// `nf` one level down steep enough to take? D = f + 8 - nf against T. The
// diagonal MERGE in stage 2 asks this when fine repose is on; without it a
// 4/8 lip would pour into the 6/8 stair below it and flatten a generated dune.
fn fineDiagSteep(f : u32, nf : u32, m : Material) -> bool {
  return i32(f) + 8 - i32(nf) > i32(reposeEighths(m));
}

// A SURFACE cell: nothing rests on it (no solid, no powder above). Both
// sub-voxel moves are surface moves: a cell with a plant or a cactus standing
// on it keeps its grains, which is also what keeps a generated step whose
// upper column carries a plant (and so stayed whole) at rest beside a 6/8
// stair.
fn powderSurface(c : vec3<i32>) -> bool {
  let up = c + vec3<i32>(0, 1, 0);
  if (!inBounds(up)) { return true; }
  let um = voxMat(voxWordAt(up));
  if (um == MAT_AIR) { return true; }
  let uk = materials[um].klass;
  return uk != CLASS_SOLID && uk != CLASS_POWDER;
}

fn tryFineRepose(c : vec3<i32>, w : u32, m : Material, r : u32) -> bool {
  if (!matHasPowderMass(m) || !powderSurface(c)) { return false; }
  let myMat = voxMat(w);
  let f = powderMass(w);
  let t = i32(reposeEighths(m));
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    let lat = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(lat)) { continue; }
    let lw = voxWordAt(lat);
    let lm = voxMat(lw);
    var tgt = lat;
    var tw = 0u;       // the target's word before (0 = air)
    var tMass = 0u;
    var top = 0;       // neighbour column top, eighths above 8y
    if (lm == myMat && powderIsPartial(lw)) {
      tw = lw;
      tMass = powderMass(lw);
      top = i32(tMass);
    } else if (lm == MAT_AIR) {
      let lb = lat + vec3<i32>(0, -1, 0);
      if (!inBounds(lb)) { continue; }
      let bw = voxWordAt(lb);
      let bm = voxMat(bw);
      if (bm == myMat && powderIsPartial(bw)) {
        tgt = lb;
        tw = bw;
        tMass = powderMass(bw);
        top = i32(tMass) - 8;
      } else if (bm != MAT_AIR &&
                 (materials[bm].klass == CLASS_SOLID ||
                  (materials[bm].klass == CLASS_POWDER && !powderIsPartial(bw)))) {
        top = 0;   // air on a whole cell: new grains stand on it
      } else {
        continue;  // a drop, a liquid, a thinner foreign powder: not this rule
      }
    } else {
      continue;
    }
    let drop = i32(f) - top;
    if (drop <= t) { continue; }
    let k = min(min(u32((drop - t + 1) / 2), f), POWDER_FULL - tMass);
    if (k == 0u) { continue; }
    let stamp = stampFor(T.tick, P.substep);
    let tv = voxIndexAndWord(tgt);
    voxStore(tv.x, packVoxKeepStain(myMat, powderStateFor(tMass + k, tgt, ptSeed()),
                                    stamp, tw));
    var si = gSelfIdx;
    if (si == PT_NO_WORD) { si = voxWordIndex(c); }
    var srcNow = myMat;
    if (k >= f) {
      voxStore(si, 0u);
      srcNow = MAT_AIR;
    } else {
      voxStore(si, packVoxKeepStain(myMat, powderStateFor(f - k, c, ptSeed()), stamp, w));
    }
    markVoxActive(tv.x);
    markVoxActive(si);
    markDirtyR(c, DIRTY_R_MOVE);
    markDirtyR(tgt, DIRTY_R_MOVE);
    flagSupportLoss(c, CLASS_POWDER, srcNow);
    return true;
  }
  return false;
}

// ======================= ANGLE OF REPOSE (POWDERS) =========================
//
// WHY EVERY POWDER USED TO PILE AT EXACTLY 45 DEGREES. The chain in main() is
// "straight down, else one of the four cells one step down and one step
// across". One down, one across -- so a pile grows until its face drops one
// voxel per voxel of run, and then every grain on the face fails both stages
// and stops. That is 45 degrees, for sand, for gravel, for snow, for dust,
// with no authored say in it. Real powders rest anywhere from about 18 degrees
// (fine dry dust) to 70 and past it (damp cohesive snow, clay), and the
// difference between a dune and a drift is most of what a pile LOOKS like.
//
// THE ANGLE IS A RUN:RISE RATIO, because a lattice CA has no other way to hold
// one: movement is whole cells, so the expressible angles are the ones whose
// tangent is a ratio of small integers. Five TIERS, in two families either side
// of the rule that was already here:
//
//   REPOSE_3_1  3:1  18.43 deg  FLOWY  a resting grain SLIDES toward a drop 3 out
//   REPOSE_2_1  2:1  26.57 deg  FLOWY  ... a drop 2 out
//   REPOSE_1_1  1:1  45.00 deg         the bare down-diagonal: unchanged, and free
//   REPOSE_1_2  1:2  63.43 deg  STEEP  the down-diagonal must drop >= 2
//   REPOSE_1_3  1:3  71.57 deg  STEEP  ... >= 3
//
// The codes are the kRepose* values in src/sim/materials.h, emitted into the
// prelude by ShaderConstantPrelude() so the two sides cannot drift; THEIR ORDER
// IS LOAD-BEARING, because `code < REPOSE_1_2` is how the diagonal gate below
// says "not a steep tier" in one compare, and REPOSE_1_1 is 0 so a material
// which authors no `repose` -- and every zeroed slot in the 4096-entry table --
// reads as the old rule.
//
// FLOWY: A 2:1 FACE IS ALREADY STABLE UNDER THE OLD RULES; nothing ever BUILT
// one. Check it: on a 2:1 staircase the cell one down and one across from a
// surface grain is the top of the next run, i.e. FILLED, so stage 2 refuses and
// the grain rests. The reason piles came out at 45 is not that flatter faces
// collapse -- it is that grains STOP as soon as they cannot descend, which is
// one cell too early. So the flowy half is one extra stage: let a grain that
// has already failed the straight fall AND all four down-diagonals take ONE
// LATERAL STEP toward a drop it can see within its tier's run. It cannot fire
// for a grain that could have descended, because descent is tried first and
// returns.
//
// STEEP: the mirror image. A 1:2 grain takes the down-diagonal only if the
// target has another open cell under it -- i.e. only if it would fall at least
// 2 -- so a one-voxel step never spreads and the face grows one run per two
// drops. The cohesive look that falls out of it (a small bump on a snow drift
// simply stays) is the intent, not a defect.
//
// ---- THE READ REACH, AND WHY THERE IS A SNAPSHOT ------------------------
//
// This is the part that had to be got right rather than assumed.
//
// The 3x3x3 colour lattice (see the header of this file, and pass_table.def)
// guarantees that within one dispatch any two acting cells are >=3 apart on
// every axis, and that writes reach <=1 cell. That makes WRITES provably
// disjoint. It says nothing about READS -- and the two facts together have a
// sharp consequence that is easy to miss: the 3x3x3 write boxes of same-colour
// cells, spaced 3 apart, TILE SPACE EXACTLY. Every cell in the world is inside
// exactly one acting cell's write box. So:
//
//   * a read at distance <=1 is inside MY OWN box and only I can write it --
//     which is precisely the argument soloSolid() makes for its six probes;
//   * a read at distance >=2 is inside SOMEBODY ELSE'S box, and whether I see
//     the pre-pass word or their write is a function of GPU scheduling. That is
//     a determinism break (rule 1), not merely a race. There is no
//     probably-fine band between the two.
//
// And repose cannot be done at reach 1: a 2:1 face and a 1:1 face are LOCALLY
// IDENTICAL at a surface grain (uphill lateral filled, downhill lateral air,
// downhill diagonal filled, cell below filled) -- the difference is at distance
// 2. That is a fact about the lattice, not about this rule.
//
// So every probe here reads the REPOSE OCCUPANCY SNAPSHOT (world.h's
// kReposeSnap* block): one bit per voxel, "a powder could drop into this cell",
// taken by the `reposesnap` prepass at TICK START, before any CA substep of
// this tick has written anything. A snapshot bit is the same in every run of
// the same tick by construction, so the probe is deterministic however the
// workgroups are scheduled.
//
// IT IS DELIBERATELY STALE, and the staleness is the cheap half of the design:
// a drop that fills in mid-tick still reads open, the grain slides toward it,
// the ordinary tryMove refuses (the destination is a powder, canDisplace says
// no), and next tick's snapshot says so. Nothing is created or destroyed, the
// error is bounded by one tick, and -- the only property rule 1 asks for -- it
// is identical in every run.
//
// AN EARLIER DESIGN READ THE TICK STAMP OF AN AIR CELL to prove no acting cell
// owned the probe's write box. The ownership argument was sound; the BIT was
// not. The stamp is explicitly not representation-invariant (sim_occupancy.wgsl
// says it "legitimately differs between two runs that reached the same world"),
// world.h strips it on save, and pagetable.cpp's kAirDemoteMask demotes an
// all-air chunk to PT_EMPTY ignoring stamps -- after which every cell reads
// STAMP_NEVER. With a 7-long stamp cycle a stale stamp on vacated air aliases
// the current substep one time in seven, so the guard would allow in one run
// and refuse in another, and a refused slide marks nothing dirty: if it landed
// on a grain's last attempt the chunk slept and the pile stayed one cell
// steeper, invisibly to both the hash and the twice-run comparison. NO CA PATH
// READS A TICK STAMP ON AN AIR CELL. main()'s own gate reads it only after
// returning on MAT_AIR.

// One bit per voxel, indexed by the SLOT cell index, plus one tick stamp per
// slot past the end. Both halves derive from constants the prelude already
// emits, so this needs no new world constant.
@group(0) @binding(35) var<storage, read_write> reposeSnap : array<u32>;
// NUM_SLOTS, not NUM_CHUNKS: a chunk TICKET's slots are dispatched by this
// same prepass and probed by the same CA (docs/PLAN_chunk_tickets.md), so they
// need bits and stamps of their own (world.h kReposeSnapBitWords).
const REPOSE_SNAP_TICK_BASE : u32 = NUM_SLOTS * CHUNK_VOL / 32u;

// Would a powder be able to drop into a cell holding this word? The snapshot's
// predicate, in one place so the prepass and the assert in the gate agree.
//
// AN OVER-APPROXIMATION OF canDisplace ON PURPOSE: it asks only "not solid,
// not powder" and ignores density, so ONE bit serves every powder instead of a
// bitfield per density class. Two consequences, both intended and both worth
// stating because they are visible in the world:
//
//   * A LIQUID READS AS OPEN. A flowy grain therefore slides sideways THROUGH
//     water toward a drop on the far side of it, rather than treating the pond
//     as a wall -- which is right (a sinking grain does drift toward a hole)
//     and is bounded by the fact that only the PROBE is liberal: the move
//     itself still goes through tryMove, and canDisplace refuses any target
//     denser than the grain. Sand slides through water; it does not slide
//     through lava.
//   * A cell too dense to enter reads as open, so the grain slides one cell
//     toward a drop it then cannot take and re-evaluates next tick. That is
//     the same harmless idling the one-tick staleness already produces.
fn reposeSnapOpenMat(m : u32) -> bool {
  if (m == MAT_AIR) { return true; }
  let k = materials[m].klass;
  return k != CLASS_SOLID && k != CLASS_POWDER;
}
fn reposeSnapOpenWord(w : u32) -> bool { return reposeSnapOpenMat(voxMat(w)); }

// Is the snapshot for this cell's SLOT valid this tick?
//
// Per-slot, and required rather than belt-and-braces: the residency window is
// toroidal, so a slot is reused by a new world chunk as the window walks, and a
// probe that read the previous occupant's bits would be reading another place
// in the world entirely. The stamp is the SNAP EPOCH (world.h), a monotonic
// never-reset counter rather than the tick -- the tick rewinds on an F7 regen
// and on every selftest arm that replays a fixed window, and a slot still
// carrying stamp k would then skip its refill at tick k and hand this kernel
// the PREVIOUS WORLD's bits. The epoch starts at 1, so the zeroed allocation
// cold-starts as "no snapshot anywhere" and every probe refuses until the
// prepass has run -- which is also what makes a window shift need no
// invalidation pass.
fn reposeSnapStamp() -> u32 { return T.snapEpoch; }

// The probe. REFUSES (returns false, "no drop here") when the slot has no
// snapshot for this tick, which is the conservative direction: refusing only
// ever leaves a grain where it is, and where it is, is a settled state.
fn reposeSnapOpen(c : vec3<i32>) -> bool {
  if (!inBounds(c)) { return false; }
  var idx = cellIndexW(c);
  // A TICKET cell (resident outside the window) is snapshotted at its TICKET
  // slot, not at the window slot its coordinate aliases 512 cells away: the
  // alias holds another place in the world, and reading it handed ticket
  // grains the window's bits whenever that slot happened to be stamped.
  if (TICKET_PROBE && !inWindow(c, T.origin)) {
    let lo = vec3<u32>(c & vec3<i32>(CHUNK_MASK));
    idx = voxSlotOfCell(c) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  }
  let slot = idx / CHUNK_VOL;
  if (reposeSnap[REPOSE_SNAP_TICK_BASE + slot] != reposeSnapStamp()) { return false; }
  return (reposeSnap[idx >> 5u] & (1u << (idx & 31u))) != 0u;
}

// ---- the prepass ---------------------------------------------------------
//
// One workgroup per chunk on the SAME compacted dirty list the CA is about to
// dispatch over, so a settled world dispatches nothing here and pays nothing
// (rule 2). Each workgroup fills its own chunk AND THE NINE NEIGHBOURS A PROBE
// CAN REACH (reposeRingOffset above): a resting grain's probes reach 3 cells
// and cross into a neighbour chunk that may be asleep, and a boundary artifact
// every 16 cells is not acceptable.
//
// EACH RING SLOT HAS EXACTLY ONE OWNER, decided from this tick's dirty flags
// (reposeRingOwned below). It used to be "every dirty chunk that touches a
// slot fills it, and a stamp test skips the ones already done" -- but a stamp
// is only published when a workgroup FINISHES, and with thousands of
// workgroups in flight at once almost none of them saw one. Measured
// 2026-09-22 on a forest fire (~5,000 dirty chunks, a solid block of smoke):
// 5.1 ms/tick, i.e. nearly every slot filled by all of its up-to-ten
// neighbours. Ownership is a pure function of dirtyIn, so which workgroup
// does the work is no longer scheduling-dependent either; the bits written
// were always a pure function of pre-CA voxel state.
//
// A SENTINEL CHUNK COSTS 128 STORES AND NO VOXEL READS. Its material is
// uniform, so its occupancy bit is uniform, so the whole 4096-cell bitfield is
// either all zeros or all ones. That matters: about half a typical window is
// PT_EMPTY sky and much of the rest is JITTER stone, and a per-cell
// synthWordAt() on a JITTER chunk pays two PCG rounds per cell for an answer
// that cannot vary.
// THE RING, and it is TEN chunks rather than the 3x3x3 block of 27.
//
// A grain acts only if its own chunk is on the dirty list, and its probes reach
// at most 3 cells, so the chunks a probe can land in are its own plus the
// neighbours those 3 cells can cross into. Two facts about the probe OFFSETS --
// (+-2,-1,0) (+-3,-1,0) (0,-1,+-2) (0,-1,+-3) for the flowy tiers and
// (+-1,-2,0) (+-1,-3,0) (0,-2,+-1) (0,-3,+-1) for the steep ones -- cut that
// from 27 to 10, and both are one line:
//
//   * EVERY PROBE HAS dy <= -1. Nothing here looks up, so the +y layer of the
//     block (9 chunks) can never be read. Falling and sliding are downhill
//     questions; that is not going to change by accident.
//   * NO PROBE OFFSETS BOTH LATERAL AXES. Every offset above has x or z zero,
//     so the four (+-1, .., +-1) corners of each layer cannot be reached.
//
// 27 -> 10 is 2.7x off the prepass, which is the whole of its cost. THE PRICE
// IS THAT THIS TABLE IS COUPLED TO THOSE OFFSETS: add a probe that looks up, or
// one that steps diagonally, and it must grow in the same commit or the new
// probe silently reads an unsnapshotted slot and REFUSES -- which does not
// corrupt anything (a refusal only ever leaves a grain where it is) but would
// quietly make piles steeper near one chunk boundary in sixteen.
const REPOSE_RING : u32 = 10u;
fn reposeRingOffset(k : u32) -> vec3<i32> {
  // Layer y = 0 (five), then layer y = -1 (five). Own chunk first, so the
  // common case is the first iteration.
  switch (k) {
    case 0u:  { return vec3<i32>( 0,  0,  0); }
    case 1u:  { return vec3<i32>( 1,  0,  0); }
    case 2u:  { return vec3<i32>(-1,  0,  0); }
    case 3u:  { return vec3<i32>( 0,  0,  1); }
    case 4u:  { return vec3<i32>( 0,  0, -1); }
    case 5u:  { return vec3<i32>( 0, -1,  0); }
    case 6u:  { return vec3<i32>( 1, -1,  0); }
    case 7u:  { return vec3<i32>(-1, -1,  0); }
    case 8u:  { return vec3<i32>( 0, -1,  1); }
    default:  { return vec3<i32>( 0, -1, -1); }
  }
}
// The ring member's slot, toroidally: the window wraps, so a member off one
// face is the slot on the opposite one -- the same modulo chunkIndexOf does,
// in chunk units.
fn reposeRingSlot(cx : u32, cy : u32, cz : u32, k : u32) -> u32 {
  let o = reposeRingOffset(k);
  let nx = u32((i32(cx) + o.x + i32(NCHUNK)) % i32(NCHUNK));
  let ny = u32((i32(cy) + o.y + i32(NCHUNK)) % i32(NCHUNK));
  let nz = u32((i32(cz) + o.z + i32(NCHUNK)) % i32(NCHUNK));
  return (nz * NCHUNK + ny) * NCHUNK + nx;
}

// Does the dirty chunk at (cx,cy,cz) own ring member k? The member's slot s is
// in the ring of every dirty centre s - offset(j); the owner is the one with
// the SMALLEST j. Offset 0 is j = 0, so a dirty chunk always owns itself, and
// every slot some dirty chunk's ring reaches has exactly one owner.
fn reposeRingOwned(cx : u32, cy : u32, cz : u32, k : u32) -> bool {
  let o = reposeRingOffset(k);
  let sx = i32(cx) + o.x;
  let sy = i32(cy) + o.y;
  let sz = i32(cz) + o.z;
  for (var j = 0u; j < k; j++) {
    let oj = reposeRingOffset(j);
    let nx = u32((sx - oj.x + 2 * i32(NCHUNK)) % i32(NCHUNK));
    let ny = u32((sy - oj.y + 2 * i32(NCHUNK)) % i32(NCHUNK));
    let nz = u32((sz - oj.z + 2 * i32(NCHUNK)) % i32(NCHUNK));
    if (dirtyIn[(nz * NCHUNK + ny) * NCHUNK + nx] != 0u) { return false; }
  }
  return true;
}

// THE RING OF A TICKET CENTRE (chunk tickets, docs/PLAN_chunk_tickets.md).
// The two functions above decode a centre as WINDOW slot coordinates and wrap
// toroidally, which is meaningless for a ticket slot (>= NUM_CHUNKS): a ticket
// centre's ring used to land on arbitrary window slots, stamping them and
// leaving the ticket's own chunks unsnapshotted. A ticket ring is resolved in
// WORLD chunks through the ticket table instead. The active interior is box
// offset 1..3 and every ring offset is within one chunk, so a ticket ring
// never leaves its own box; a window centre's toroidal ring never reaches a
// ticket slot. Ownership is the window rule (the smallest j whose centre is a
// DISPATCHED dirty chunk owns the member), where "dispatched" for a ticket slot
// also means ACTIVE: a dirty shell chunk is never on the list (sim_compact.wgsl)
// and must not claim a member it will not fill.
fn reposeRingSlotT(cwc : vec3<i32>, k : u32) -> u32 {
  return ticketSlotOf(cwc + reposeRingOffset(k));
}
fn reposeRingOwnedT(cwc : vec3<i32>, k : u32) -> bool {
  let m = cwc + reposeRingOffset(k);
  if (ticketSlotOf(m) == SLOT_NONE) { return false; }
  for (var j = 0u; j < k; j++) {
    let s = ticketSlotOf(m - reposeRingOffset(j));
    if (s != SLOT_NONE && dirtyIn[s] != 0u && ticketSlotActive(s)) { return false; }
  }
  return true;
}

var<workgroup> wgReposeOwned : array<u32, REPOSE_RING>;
var<workgroup> wgReposeEntry : u32;
var<workgroup> wgReposeFlag : array<u32, 4224>;   // CHUNK_VOL + CHUNK_VOL / 32

@compute @workgroup_size(64)
fn reposesnap(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  let stamp = reposeSnapStamp();
  let cz = centre / (NCHUNK * NCHUNK);
  let cy = (centre / NCHUNK) % NCHUNK;
  let cx = centre % NCHUNK;
  let wpc = CHUNK_VOL / 32u;   // 128 bitfield words per chunk
  // A ticket centre resolves its ring in world chunks (reposeRingSlotT).
  let isTicket = TICKET_PROBE && centre >= NUM_CHUNKS;
  let cwc = slotWorldChunk(centre, T.origin);

  // PASS 0: which ring members are OURS (one thread each, then shared).
  // Member 0 -- this chunk itself, always its own owner -- is published by
  // camask (substep 0) from the voxels it reads anyway, so it is never ours
  // here (THE CHUNK'S OWN REPOSE SNAPSHOT, 2026-10-03).
  if (li < REPOSE_RING) {
    var owned = false;
    if (li == 0u) { owned = false; }
    else if (isTicket) { owned = reposeRingOwnedT(cwc, li); }
    else { owned = reposeRingOwned(cx, cy, cz, li); }
    wgReposeOwned[li] = select(0u, 1u, owned);
  }
  workgroupBarrier();

  // PASS 1: the bits, one ring member at a time. The loop is UNIFORM (every
  // branch that holds a barrier tests a workgroupUniformLoad), so a member is
  // read COALESCED -- thread v loads voxel v into a padded flag array -- and
  // then packed thread-per-word out of workgroup memory. The previous shape,
  // thread-per-word straight from the page, strode 128 B between lanes on
  // every load; measured on a forest fire it was 2.6 ms/tick of a pass whose
  // whole input is one read of each chunk.
  for (var k = 0u; k < REPOSE_RING; k++) {
    // Another dirty chunk owns this member and fills it (reposeRingOwned).
    if (workgroupUniformLoad(&wgReposeOwned[k]) == 0u) { continue; }
    var slot = reposeRingSlot(cx, cy, cz, k);
    if (isTicket) { slot = reposeRingSlotT(cwc, k); }
    let wordBase = slot * wpc;
    if (li == 0u) { wgReposeEntry = pageTable[slot]; }
    let e = workgroupUniformLoad(&wgReposeEntry);
    if ((e & PT_SENTINEL_BIT) != 0u) {
      // Uniform by definition: EMPTY, UNIFORM(mat) and JITTER(mat) all hold ONE
      // material, and JITTER varies only the palette nibble -- which this
      // predicate does not look at. So the whole 4096-cell bitfield is all
      // zeros or all ones and synthWordAt() is never called, which matters:
      // about half a window is PT_EMPTY sky, much of the rest is JITTER stone,
      // and a per-cell synth pays two PCG rounds for an answer that cannot vary.
      var bits = 0u;
      if (reposeSnapOpenMat(e & PT_MAT_MASK)) { bits = 0xFFFFFFFFu; }
      for (var i = li; i < wpc; i += 64u) { reposeSnap[wordBase + i] = bits; }
      continue;
    }
    let pageBase = e * CHUNK_VOL;
    for (var v = li; v < CHUNK_VOL; v += 64u) {
      // +1 word of padding per 32 so the packing reads below (lane t at
      // t*33 + b) fall in 32 different banks.
      wgReposeFlag[v + (v >> 5u)] = select(0u, 1u, reposeSnapOpenWord(voxels[pageBase + v]));
    }
    workgroupBarrier();
    for (var i = li; i < wpc; i += 64u) {
      var bits = 0u;
      for (var b = 0u; b < 32u; b++) { bits |= wgReposeFlag[i * 33u + b] << b; }
      reposeSnap[wordBase + i] = bits;
    }
    // The next member overwrites the flags this one's packing is reading.
    workgroupBarrier();
  }

  // NO BARRIER before publishing: nothing in this pass reads a stamp any
  // more (ownership replaced the stamp skip), and the CA that does read them
  // runs behind the pass-table barrier on W(ReposeSnap).

  // PASS 2: publish the members this workgroup owns and filled.
  for (var k = li; k < REPOSE_RING; k += 64u) {
    if (wgReposeOwned[k] == 0u) { continue; }
    var slot = reposeRingSlot(cx, cy, cz, k);
    if (isTicket) { slot = reposeRingSlotT(cwc, k); }
    reposeSnap[REPOSE_SNAP_TICK_BASE + slot] = stamp;
  }
}

// Which tier THIS GRAIN uses, for a material whose authored angle sits between
// two tiers.
//
// POSITION-KEYED, AND THE MISSING TICK IS THE POINT. Keying on the world
// position and not on the tick means a grain that has not moved makes the same
// decision on every tick of its life: it fails the same stage it failed last
// tick, nothing is written, nothing marks the chunk dirty, and the pile SLEEPS
// (CLAUDE.md rule 2). Key it on the tick instead and a settled dune re-rolls a
// share of its grains every tick, a few of them find a move, and those chunks
// never sleep again. A grain re-rolls when it changes cells, which is intended:
// it makes the mixture a property of the PILE rather than a permanent label on
// one grain.
//
// WORLD POSITION, NOT THE SLOT INDEX, and the same mixing synthJitterState
// uses. A slot index is WINDOW-RELATIVE and renames on a streaming shift, so a
// settled pile would re-roll its whole mixture the moment the player walked far
// enough -- a pile silently changing shape with nothing touching it.
fn matReposeCode(m : Material, c : vec3<i32>) -> u32 {
  let a = (m.repose >> MAT_REPOSE_A_SHIFT) & MAT_REPOSE_A_MASK;
  let blend = (m.repose >> MAT_REPOSE_BLEND_SHIFT) & MAT_REPOSE_BLEND_MASK;
  // Pure tier -- which includes every 45-degree material, whose whole word is 0.
  if (blend == 0u) { return a; }
  let b = (m.repose >> MAT_REPOSE_B_SHIFT) & MAT_REPOSE_B_MASK;
  let h = hash3(T.seed ^ REPOSE_BLEND_SALT,
                bitcast<u32>(c.x) ^ (bitcast<u32>(c.z) << 12u),
                bitcast<u32>(c.y));
  if ((h & 0xFFu) < blend) { return b; }
  return a;
}
// Salt for the blend hash. Any constant works; a distinct one keeps the blend
// stream from correlating with the movement RNG, whose key is
// (seed, tick*2+substep, slot), and with synthJitterState's 0xC0FFEE.
const REPOSE_BLEND_SALT : u32 = 0x9E3779B9u;

// STEEP TIERS: may this grain take the down-diagonal in direction d?
//
// Returns true on the first compare for every non-steep material, which is what
// keeps 45 degrees free.
fn reposeDiagAllowed(c : vec3<i32>, d : vec2<i32>, code : u32) -> bool {
  if (code < REPOSE_1_2) { return true; }
  if (!reposeSnapOpen(c + vec3<i32>(d.x, -2, d.y))) { return false; }
  if (code != REPOSE_1_3) { return true; }
  return reposeSnapOpen(c + vec3<i32>(d.x, -3, d.y));
}

// Move t eighths of liquid `mat` from src (fullness sf) onto dst (fullness df,
// 0 = air). Mass-conserving: src empties to air when it gives everything.
fn transferLiquid(src : vec3<i32>, dst : vec3<i32>, mat : u32,
                  sf : u32, df : u32, t : u32) {
  let stamp = stampFor(T.tick, P.substep);
  // One table resolution per cell for index AND word (they used to be four).
  let sv = voxIndexAndWord(src);
  let dv = voxIndexAndWord(dst);
  let si = sv.x;
  let di = dv.x;
  // Stain and flowing liquid: the DESTINATION keeps its own stain, and the
  // source keeps its own. A liquid moving through a cell does not pick the
  // cell's stain up and carry it downstream — stain marks the SURFACE that was
  // soaked, and it stays on that surface until the stain rule itself changes
  // it. (A stained liquid voxel is a normal thing to have: blood that has
  // pooled on stained ground reads stained through, which is right.)
  //
  // A source cell that empties completely goes to 0 — full air, no stain. That
  // is deliberate: the stain belonged to the liquid that just left, and an
  // empty cell of air has no surface to hold it.
  let sw = sv.y;
  let dw = dv.y;
  if (t >= sf) { voxStore(si, 0u); }
  else { voxStore(si, packVoxKeepStain(mat, sf - t - 1u, stamp, sw)); }
  markVoxActive(si);
  voxStore(di, packVoxKeepStain(mat, df + t - 1u, stamp, dw));
  markVoxActive(di);
  solTransfer(src, dst, sf, df, t);
  markDirtyR(src, DIRTY_R_MOVE);
  markDirtyR(dst, DIRTY_R_MOVE);
}

// faceDir and faceDirBit (the six face directions with their RDIR_* bits)
// live in common.wgsl: the seam's particleTick walks a particle's reaction
// bucket with the same direction mask doReactions honours (W1-B1).

// State nibble for a freshly created voxel: liquids are born full (their
// nibble is fullness, not a palette variant).
fn productState(mat : u32, r : u32) -> u32 {
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) { return LIQ_FULL_STATE; }
  // A powder CREATED from something that was not powder (ash off a burning
  // solid, a fountain's emission, a product in an empty face) is born as a
  // CRUMBLE -- see POWDER ENTERS THE WORLD AS GRAINS in common.wgsl.
  if (mat != MAT_AIR && matHasPowderMass(materials[mat])) { return powderCrumbleState(r); }
  return r % 3u;
}

// ---- MASS THROUGH A REACTION (docs/PLAN_powder_mass.md §2.5, §10 Q2) --------
// A partial cell that transmutes keeps its eighths when the product can hold
// eighths: 3/8 of snow melts to 3/8 of water, 1/8 of gravel etched by acid is
// 1/8 of sand. Before powder carried mass every product was born full, which
// minted matter from any partial liquid; that is what this retires.
//
// The eighths the cell word `sw` holds if it is MATTER THAT HAS EIGHTHS: a
// liquid's fullness or a powder's mass (1..8). 0 for everything else -- a
// solid, a gas, air -- which is "no mass to carry".
fn cellEighths(sw : u32) -> u32 {
  let sm = materials[voxMat(sw)];
  if (sm.klass == CLASS_LIQUID) { return voxState(sw) + 1u; }
  if (matHasPowderMass(sm)) { return powderMass(sw); }
  return 0u;
}
// The product's state nibble when the cell it replaces held `e` eighths
// (cellEighths). A powder or liquid source hands its eighths on: 3/8 of snow
// melts to 3/8 of water, a whole cell of gravel etched by acid is a whole
// cell of sand. A source with no eighths (e == 0: wood burning to ash) gets
// productState -- a crumble for a powder product. Solid products from a
// partial POWDER never get here (doReactions refuses the rule) and gas ones
// are thinned in reactWriteSelf.
fn carriedState(prod : u32, e : u32, r : u32) -> u32 {
  if (e != 0u && prod != MAT_AIR) {
    let pm = materials[prod];
    if (pm.klass == CLASS_LIQUID) { return min(e, 8u) - 1u; }
    if (matHasPowderMass(pm)) { return select(e + 2u, r % 3u, e >= POWDER_FULL); }
  }
  return productState(prod, r);
}

// ---- sky exposure (daylight-gated reactions) --------------------------------
// "Is this cell exposed to the sky?" — answered by looking at the ONE cell
// directly above it, and nothing further.
//
// ---- WHY THIS IS NOT A COLUMN WALK ----
// The obvious implementation is to march up until something blocks, so that a
// pond in a cave is correctly "indoors". That was tried here and it BREAKS
// DETERMINISM, for a reason worth recording because it is easy to talk
// yourself out of:
//
// The 3x3x3 color lattice guarantees that two cells acting in the same pass
// are >=3 apart and that WRITES reach <=1 cell, so writes never collide. It
// guarantees nothing about READS. A 48-cell probe column crosses dozens of
// cells that other threads in this very pass are legally writing, so whether
// the probe sees a cell before or after its update depends on scheduling —
// exactly the "no scheduling-dependent outcomes" ban (CLAUDE.md rule 1). It
// reproduced as a hash divergence at tick 1, with water freezing under
// different roofs on each run.
//
// A one-cell look-up stays inside the guarantee: the cell above is either
// >=3 away (so it is not acting this pass) or is the mover that is writing
// into this very cell, which the stamp check already serializes.
//
// The cost of the honest version is that "sky" means "nothing directly on top
// of me" rather than "open to the heavens", so water in a lit cave evaporates
// too. That is a content inaccuracy, not a correctness one, and the trade is
// forced: the deterministic alternative is a separate mark/apply pass over a
// sky-exposure buffer (the pattern sim_explode.wgsl uses), which is the right
// answer if this ever needs to tell a cave from a meadow.
fn seesSky(c : vec3<i32>) -> bool {
  let n = c + vec3<i32>(0, 1, 0);
  // Left the window going up = open sky above.
  if (!inBounds(n)) { return true; }
  let nmat = voxMat(voxWordAt(n));
  if (nmat == MAT_AIR) { return true; }
  // Translucent things (water, glass, smoke, steam) let light through; only a
  // ray blocker really shades the cell below it.
  return !isRayBlocker(materials[nmat]);
}

// Does rain reach this cell? THE RAIN EXPOSURE MAP (sim_rain_expo.wgsl,
// built this tick before the CA from the grid as the tick's mutations left it,
// read-only here): the cell is exposed if it is not below the first ray
// blocker on its own fall line -- along this tick's slope, so a roof keeps its
// floor dry however far up it is, and a windward door lets rain in while the
// lee side stays dry. The map is a SNAPSHOT, which is what makes the long read
// legal here where seesSky's 48-cell column was not: nothing in this pass
// writes it.
//
// Rain splashes, so a burning trunk is wet down its SIDES, not only on top: a
// horizontal face opening onto an air-ish neighbour that the map calls exposed
// counts too. (It used to be any neighbour whose own cell above was clear,
// which made every air cell beside an indoor wall rain-open.) The neighbour
// read is at Chebyshev distance 1, the reach seesSky's note proves is
// scheduling-free.
// Costs nothing while dry: both callers test T.weatherRain first, and the map
// is built on exactly the ticks where that test can pass (rain or wetness).
@group(0) @binding(45) var<storage, read> rainExpo : array<u32>;
// The wind-draft shelter volume (sim_draft.wgsl): windAtQ reads its transfer
// field through common.wgsl's WIND DRAFTS block, so the drift bias and the
// entrainment test feel walls. Built by the draft rows before the CA.
@group(0) @binding(46) var<storage, read> draftField : array<u32>;
// THE AIR MASK (pass_table.def caMask): one bit per cell of every dirty chunk,
// CA_MASK_WORDS words per SLOT, bit (local & 31) of word (local >> 5) where
// local = x + 16y + 256z. Set = the cell held matter when this gravity substep
// began. Written by `camask` below, read by `main`.
@group(0) @binding(48) var<storage, read_write> caMask : array<u32>;
const CA_MASK_WORDS : u32 = CHUNK_VOL / 32u;
// Three bit planes per slot, CA_MASK_STRIDE words apart: [0] matter, [1] gas,
// [2] inert solid (CLASS_SOLID, !matCanAct). Planes 1-2 only ORDER the work
// (main's gather sorts by them so a warp runs one path); plane 0 decides it.
const CA_MASK_STRIDE : u32 = 3u * CA_MASK_WORDS;
// ---- THE COLOUR WORK LISTS (2026-10-03, ca-chunk-overhead) -----------------
// The tail of the same buffer, past the NUM_SLOTS mask records. Rebuilt before
// each gravity substep: `camask` writes, per DIRTY-LIST POSITION, which of the
// 27 colours the chunk has any work in (CA_WORK_COL0); `calist` turns those
// into one chunk list per colour (CA_WORK_LIST0, NUM_SLOTS entries each) with
// its count (CA_WORK_N0) and the colour's indirect args (caArgs, binding 54).
// The colour rows walk their colour's list instead of the whole dirty list, so
// a chunk with nothing of colour k -- every colour, for an awake chunk that
// holds no matter -- is not dispatched for it at all. src/sim/simulation.cpp
// sizes the buffer from the same numbers (kCaPoolCap is CA_POOL_CAP).
//
// THE SPARSE HALF (same date): a (chunk, colour) holding at most CA_SPARSE_T
// cells -- and in a chunk whose cells may run on their own, see camask -- is
// NOT put on the chunk list. camask writes its cells straight into colour k's
// two CELL POOLS (CA_POOL0 + (2k + kind) * CA_POOL_CAP; kind 0 = gas, 1 =
// everything else), one u32 each, (slot << 13) | local. The colour row then
// runs those 128 to a workgroup, so ~2,900 smoke chunks holding a few cells a
// colour stop costing a workgroup's fixed cost apiece.
// A POOL IS BOUNDED, AND FULL IS NOT AN ERROR: a (chunk, colour) whose
// reservation does not fit in either pool goes on the dense list instead, and
// whatever it reserved in the other pool is filled with CA_POOL_SKIP entries
// that `main` passes over. Which (chunk, colour) overflows is arrival order --
// and irrelevant, since either way the same cells run once each (THE COLOUR
// ROWS' lattice argument). Per colour k, the header at CA_WORK_N0 + 4k is
// (dense chunks, sparse gas entries, sparse other entries, dense workgroups),
// written by calist.
const CA_WORK_COL0 : u32 = NUM_SLOTS * CA_MASK_STRIDE;
const CA_WORK_N0 : u32 = CA_WORK_COL0 + NUM_SLOTS;
const CA_WORK_LIST0 : u32 = CA_WORK_N0 + 128u;
const CA_SPARSE_T : u32 = 216u;
const CA_POOL_CAP : u32 = 65536u;
const CA_POOL0 : u32 = CA_WORK_LIST0 + 27u * NUM_SLOTS;
const CA_POOL_SKIP : u32 = 0xFFFFFFFFu;
const CA_ALL_COLOURS : u32 = 0x7FFFFFFu;   // 27 bits
// caArgs words: 27 indirect records of 4, then the two sparse cursors of each
// colour (camask reserves pool ranges from them; calist reads and re-zeroes).
const CA_CUR_G : u32 = 128u;
const CA_CUR_R : u32 = 160u;
// The dirty-chunk count `compact` appended (args[0]): `calist` reads it to
// know how many positions `camask` wrote.
@group(0) @binding(13) var<storage, read> args : array<u32>;
// The colour rows' indirect args, 27 records of (x, 1, 1, chunks): written by
// `calist`, consumed by the recorder as the dispatch of colour k (pass_table
// IND_CAARGS, offset 16 k). Never read by `main`, which takes its counts from
// CA_WORK_N0 -- this buffer is the indirect-command source in that dispatch.
// Words CA_CUR_G / CA_CUR_R + k are colour k's sparse pool cursors.
@group(0) @binding(54) var<storage, read_write> caArgs : array<atomic<u32>>;
// THE AMBIENT WIND, CACHED PER 4^3 BLOCK (2026-10-01): windAmbQ at the block's
// centre, 64 blocks x 3 words per SLOT, written by `camask` for every dirty
// chunk before each substep and read by caWindAt. Measured on the village
// fire: windAtQ evaluated per gas cell per substep (1-3 times: the intent and
// the two lateral rotations) was ~40% of the CA -- terrain ramp, six integer
// sine bands, the draft shelter -- for a field that is smooth at the draft
// volume's own 0.4 m cell. One evaluation per block instead of per cell is
// the per-chunk cache windAtQ's COST note asked for, at the draft resolution
// so a doorway's shelter keeps its shape. The primitives stay per cell.
@group(0) @binding(49) var<storage, read_write> caWind : array<i32>;
const CA_WIND_BLOCKS : u32 = 64u;   // (CHUNK / 4)^3
// sim_rain_expo.wgsl's lattice, byte for byte (and src/sim/rainexpo.h).
const RX_TEX_SHIFT : u32 = 2u;
const RX_OPEN : i32 = -2147483647 - 1;
const RX_MARGIN : i32 = 1;                  // rainlat::kExpoMargin
fn rxFloorDiv(a : i32, b : i32) -> i32 {
  return select(-((-a + b - 1) / b), a / b, a >= 0);
}
fn rxDrift(n : i32, y : i32) -> i32 { return rxFloorDiv(n * y + 8, 16); }
fn rainMapExposed(c : vec3<i32>) -> bool {
  let n = vec2<i32>(T.rainSlopeQx >> 12u, T.rainSlopeQz >> 12u);
  let b = T.origin * i32(CHUNK);
  let yb = b.y;
  let yt = b.y + i32(WORLD_N) - 1;
  let d0 = vec2<i32>(rxDrift(n.x, yb), rxDrift(n.y, yb));
  let d1 = vec2<i32>(rxDrift(n.x, yt), rxDrift(n.y, yt));
  let lo = (b.xz + min(d0, d1)) >> vec2<u32>(RX_TEX_SHIFT);
  let hi = (b.xz + vec2<i32>(i32(WORLD_N) - 1) + max(d0, d1)) >> vec2<u32>(RX_TEX_SHIFT);
  let key = c.xz + vec2<i32>(rxDrift(n.x, c.y), rxDrift(n.y, c.y));
  let t = (key >> vec2<u32>(RX_TEX_SHIFT)) - lo;
  let ext = hi - lo + 1;
  if (any(t < vec2<i32>(0)) || any(t >= ext)) { return true; }
  let h = bitcast<i32>(rainExpo[u32(t.y * ext.x + t.x)]);
  return h == RX_OPEN || c.y >= h - RX_MARGIN;
}
// The rain's SURFACE, leniently: exposed by any of the 3x3 texels round the
// cell's own. stainDry asks "is this wet cell one the rain keeps wet" -- and
// the ground sampler wet it along its OWN fall line (sim_mutate rainFall is
// exact per line), where the map answers for a texel's representative line,
// up to two keys off. On a hillside riser the representative line can meet
// the tread above first and call the riser covered; the cell then dried and
// held its chunk awake while the rain re-wet it, every tick of the storm.
// Leniency here is harmless: a covered cell judged exposed only waits for the
// rain to stop before it dries.
fn rainMapExposedNear(c : vec3<i32>) -> bool {
  for (var i = 0; i < 9; i++) {
    let d = vec3<i32>((i % 3 - 1) * 4, 0, (i / 3 - 1) * 4);
    if (rainMapExposed(c + d)) { return true; }
  }
  return false;
}
fn rainOpen(n : vec3<i32>) -> bool {
  if (!inBounds(n)) { return true; }
  let nm = voxMat(voxWordAt(n));
  return nm == MAT_AIR || !isRayBlocker(materials[nm]);
}
fn rainExposed(c : vec3<i32>) -> bool {
  if (rainMapExposed(c)) { return true; }
  for (var i = 0u; i < 4u; i++) {
    let d = select(vec3<i32>(0, 0, select(-1, 1, i == 3u)),
                   vec3<i32>(select(-1, 1, i == 1u), 0, 0), i < 2u);
    let n = c + d;
    if (rainOpen(n) && rainMapExposed(n)) { return true; }
  }
  return false;
}

// A rule's chance under this tick's weather — rain douses (RCOND_RAIN) and
// damps ignition (RCOND_RAINDAMP). The arithmetic is common.wgsl's
// reactWeatherChance, the one definition the gas parcels and (by mirror,
// materials.h) the body burners also use; what is this kernel's own is only
// the exposure probe, paid only when that arithmetic would read it (a
// RAINDAMP rule while anything is wet). A RAIN rule has already passed
// lightMatches (rain > 0, exposed).
fn rainChance(rule : Reaction, c : vec3<i32>, chance : u32) -> u32 {
  let exposed = reactWantsExposure(rule.cond, T.weatherRain) && rainExposed(c);
  return reactWeatherChance(rule.cond, chance, T.weatherRain, exposed);
}

// Does the cell's light environment satisfy this rule's condition?
// Encoded in Reaction.cond (see materials.h ReactionGpu.cond):
//   bit0 RCOND_SKY   — requires open sky above
//   bit1 RCOND_DAY   — requires daytime
//   bit2 RCOND_NIGHT — requires night
//   bit3 RCOND_RAIN  — a douse: only while it rains, only where it lands
// `minLight` (bits 8..15) is a daylight-strength floor, so "only near noon"
// rules are expressible without a second condition bit.
// The tick-only half is common.wgsl's reactPhaseOpen; the two probes are this
// kernel's, and run last because they are the only expensive tests.
fn lightMatches(rule : Reaction, c : vec3<i32>) -> bool {
  let cond = rule.cond & 0xFFu;
  if (cond == 0u) { return true; }  // unconditional: the common case, free
  if (!reactPhaseOpen(rule.cond, daylightStrength(T.dayPhase), T.weatherRain)) {
    return false;
  }
  if ((cond & RCOND_RAIN) != 0u && !rainExposed(c)) { return false; }
  if ((cond & RCOND_SKY) != 0u && !seesSky(c)) { return false; }
  return true;
}

// Excited-fluid occupancy of an AIR cell: the material id the seam's intent
// carries, or 0 when no meaningful fluid mass is there. One tick latent by
// design (the seam wrote both after last tick's substeps), deterministic, and
// gated on the block map so a fluid-free world pays one zero-load.
fn fluidOccMat(n : vec3<i32>) -> u32 {
  let wc = worldChunkOf(n);
  let wsl = chunkSlotOf(wc, T.origin);
  if (wsl == SLOT_NONE) { return 0u; }
  let bm = fluidBlockMapS[wsl];
  if (bm == 0u) { return 0u; }
  let lo = vec3<u32>(n & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  if (fluidGridS[ci * FLUID_GW] < 1024) { return 0u; }  // < 1 particle mass
  return atomicLoad(&fluidCellScratch[ci * 2u]) >> 16u;
}

// ---- EXCITED FLUID RUNS ITS OWN RULES (rule-unification W1-B1) ------------
// Before this, MPM particles were only ever the NEIGHBOUR side of a reaction
// (fluidOccMat, in the PAIR scan below): stone next to excited water could
// react with it, but the water's own rules never ran, because the CA only acts
// on voxels and an excited cell is air in the grid. So splashing acid (every
// acid rule has acid as `self`, and seamLiquid admits acid) corroded nothing,
// and excited water beside `tag:hot` never became steam.
//
// THE DESIGN: the air cell is the actor. The same fluidOccMat synthesis that
// makes an excited cell a neighbour makes it a SELF: an air cell whose intent
// names a fluid material runs that material's bucket through the unchanged
// doReactions, with `synthSelf` set so that a self product is written into
// the air cell and the cell's particle bin is consumed (reactWriteSelf).
// Chosen over the alternatives because it adds NO new pass, NO new buffer and
// NO new consumption path:
//   * per-particle evaluation in the seam would need a voxel write from a
//     particle thread — two particles beside one stone race, which is exactly
//     the mark/apply problem the colour lattice already solves here;
//   * a separate mark/apply pair would duplicate doReactions' rule walk,
//     which is the drift this program exists to remove.
//
// DETERMINISM (rule 1). The actor is the cell `c`, acting in its own colour
// pass: every write is `c` itself or a face neighbour, reach <= 1, inside the
// lattice guarantee. The intent it reads was written by last tick's
// particleTick (atomicMax — order-free) and is stable for this whole dispatch;
// the consume flag is an atomicOr, idempotent. RNG keys on the SLOT index as
// every other rule does.
//
// SLEEP (rule 2). A synthesized self holds its chunk awake by the ordinary
// keepAwake rule while a rule matches; every such rule either consumes the
// finite bin (self product) or transforms a finite neighbour, and the
// particles themselves settle. What WAKES a chunk the CA had let sleep is
// particleTick (sim_fluid_seam.wgsl), which marks DIRTY_R_REACT on a
// particle's chunk only when one of its UNGATED rules has a partner beside it.
fn excitedReact(c : vec3<i32>, idx : u32, slotIdx : u32) {
  let fm = fluidOccMat(c);
  if (fm == 0u) { return; }
  let m = materials[fm];
  if (m.reactCount == 0u) { return; }
  let rnd = hash3(T.seed, T.tick * 2u + P.substep, slotIdx);
  _ = doReactions(c, idx, slotIdx, 0u, fm, m, rnd, true, 0u, false, false);
}

// A rule fired against synthesized excited fluid and takes the neighbour:
// flag the cell so the seam's consumeApply kills its particle bin this tick.
// atomicOr — order-free, idempotent.
fn flagFluidConsume(n : vec3<i32>) {
  let wc = worldChunkOf(n);
  let wsl = chunkSlotOf(wc, T.origin);
  if (wsl == SLOT_NONE) { return; }
  let bm = fluidBlockMapS[wsl];
  if (bm == 0u) { return; }
  let lo = vec3<u32>(n & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  atomicOr(&fluidCellScratch[ci * 2u + 1u], 1u);
}

// A rule rewrote SELF. The one place the three rule kinds commit that, so the
// synthesized-self case below cannot be forgotten in one of them.
//
// SYNTHESIZED SELF (excited fluid running its own rules — see excitedReact).
// Self is then an AIR cell holding MPM particles: the product is written into
// that air cell exactly as a PAIR writes into a synthesized neighbour, and the
// cell's particle bin is flagged for consumeApply, which kills it in the seam
// front half later this same tick. "Becomes air" writes nothing — the cell is
// already air; the flag is the whole of it. Consumption granularity is the
// voxel-eighth bin, the same contract the neighbour side has always had.
fn reactWriteSelf(c : vec3<i32>, idx : u32, synthSelf : bool, klass : u32,
                  prod : u32, rnd : u32, stamp : u32) {
  markDirtyR(c, DIRTY_R_REACTW);
  if (synthSelf) {
    flagFluidConsume(c);
    if (prod == 0u) { return; }
  }
  // Self's eighths (8 for a synthesized self: that cell is air). Read before
  // the store below; nothing on the reaction paths writes self before this.
  var e = 0u;
  var selfPowder = false;
  // The solute this cell carries, if it is a liquid (solOnReplace below).
  var sv = 0u;
  if (!synthSelf) {
    let sw = voxWordAt(c);
    e = cellEighths(sw);
    selfPowder = matHasPowderMass(materials[voxMat(sw)]);
    sv = solCarried(c, sw);
    reactLiquidEaten(sw, prod);  // the ledger's CA half (see its header)
  }
  // A solvent turning into something else: its mass moves on or precipitates
  // (evaporating brine concentrates, then leaves salt).
  if (sv != 0u) {
    let rep = solOnReplace(c, sv, prod, true, (rnd >> 7u) % 6u);
    if (rep.y != 0u) {
      voxStore(idx, packVox(rep.x, solPrecipState(rep.x, rep.y, c), stamp));
      markVoxActive(idx);
      flagSupportLoss(c, klass, rep.x);
      return;
    }
  }
  // A partial grain that becomes a GAS (dust flashing to fire) makes one whole
  // gas cell with probability e/8 and otherwise just goes: a dusting burns
  // like 1/8 of a pile, not like a pile. Gas has no eighths to carry, and the
  // roll keeps expected mass exact without one.
  let thinned = selfPowder && e < POWDER_FULL && prod != 0u &&
                materials[prod].klass == CLASS_GAS && ((rnd >> 5u) & 7u) >= e;
  if (prod == 0u || thinned) { voxStore(idx, 0u); }
  else { voxStore(idx, packVox(prod, carriedState(prod, e, rnd), stamp)); }
  markVoxActive(idx);
  flagSupportLoss(c, klass, prod);  // ember->ash drops the wood above
}

// nbrMatches lives in common.wgsl (W1-B1): the seam's particleTick asks the
// same question of a particle's neighbours, and two copies of a neighbour
// predicate are two answers to "does this rule apply here".

// Scales a rule's chance by how many of the 6 face neighbours match its
// neighbour predicate. Returns the effective chance in units of
// 1/REACT_CHANCE_DEN; 0 means the rule cannot fire at all this tick.
//
// This is what makes a rule spread from a FRONTIER instead of nucleating
// uniformly. Water freezing is the motivating case: scaled by the count of
// non-water neighbours, a pond's banks and surface freeze first and the ice
// creeps inward, because every new ice voxel raises its liquid neighbours'
// odds. Deep water is surrounded by water, counts 0, and cannot freeze until
// the front reaches it.
//
// `minCount` generalizes the count-0 gate into a count-<N gate, which is what
// evaporation needs: it counts NON-water neighbours too, but demands at least
// 4 of them, so a lone droplet (6 non-water) boils off fast, a rim cell (4-5)
// goes slowly, and the flat surface of a pond (1 non-water — just the air
// above) is immune. Without the floor, a pond surface would fire at the full
// base chance, which is exactly the "way too much steam" failure.
//
// The return is in a FINER denominator than the authored per-mille, because
// the interesting rules are authored at chance 1-2: computing
// `(chance * q) / 4` per-mille would truncate 1.5x and 2.75x onto the same
// integer, collapsing a 6-step ramp to 4 steps. Scaling the numerator instead
// keeps every step distinct at chance 1.
//
// Determinism (rule 1): this READS a 1-cell neighbourhood and writes nothing,
// so it stays inside the colour lattice's guarantee — the lattice bounds
// WRITES to 1 cell, and no cell within 1 of an acting cell is itself acting.
// All integer: the multiplier is quarters, biased by 1.0x, and the whole
// expression is done in u32 with the divide last so it rounds identically on
// every vendor. A float here would be a determinism bug.
//
// THE COAT (see "A COAT IS A CO-LOCATED VIRTUAL NEIGHBOUR" below): `coat` is
// the material of the cell's own coat, 0 for none. It counts as ONE more
// neighbour, for a DIRECT ramp only and only when it matches, and the count is
// capped at 6: a coat is not a seventh face, and an INVERTED ramp counts what
// the cell is exposed to, which a film on it does not add to. `covered` hides
// the six faces (a quenching coat is reacting; see coatReact), leaving the coat.
fn scaledChance(rule : Reaction, c : vec3<i32>, coat : u32, covered : bool) -> u32 {
  if ((rule.cond & RSCALE_ON) == 0u) { return rule.chance; }
  let invert = (rule.cond & RSCALE_INVERT) != 0u;

  let coatHit = !invert && coat != 0u && nbrMatches(rule, coat, materials[coat]);
  var count = 0u;  // matching FACES; the coat is added by coatRampCount below
  for (var i = 0u; i < 6u && !covered; i++) {
    let n = c + faceDir(i);
    // Out-of-window space is solid and inert, and reads as "not the counted
    // material" — which is right for ice: the residency edge acts like a bank
    // rather than like more water.
    var hit = false;
    if (inBounds(n)) {
      let nmat = voxMat(voxWordAt(n));
      // MAT_AIR has no Material entry worth matching on tags/class, so an
      // air neighbour only counts via an exact nbrMat == 0 predicate —
      // UNLESS it holds excited fluid, which counts as the fluid's material
      // (the same synthesis doReactions' PAIR scan makes). Without it an
      // excited water cell running its OWN evaporation rule (the synthSelf
      // path) would count its excited neighbours as air and read every cell
      // of a splash as a lone droplet; and a voxel pond beside a splash read
      // the splash as open air too (DESIGN.md §5's old "frontier scaling
      // sees excited fluid as air" limit). One zero-load in a fluid-free
      // chunk.
      if (nmat == MAT_AIR) {
        let fm = fluidOccMat(n);
        if (fm != 0u) { hit = nbrMatches(rule, fm, materials[fm]); }
        else { hit = rule.nbrMat == MAT_AIR; }
      }
      else { hit = nbrMatches(rule, nmat, materials[nmat]); }
    }
    if (hit != invert) { count++; }
  }
  // The coat as one more neighbour, capped at 6 (rule 4). The grid is at
  // world pitch, so clause 4a's widening is off (pitchFine false): a body
  // calls the same function with its lattice pitch (src/sim/coatrule.h).
  count = coatRampCount(count, coatHit, false);
  // Hard gate: below the minimum count there is no frontier, so no reaction.
  // minCount defaults to 1 (any matching neighbour will do), which is the
  // freezing case. Evaporation raises it so that a water voxel with a couple
  // of watery neighbours still counts as "part of the pond" and is immune,
  // while an exposed droplet is not.
  let minCount = ((rule.cond >> RSCALE_MIN_SHIFT) & RSCALE_MIN_MASK) + 1u;
  if (count < minCount) { return 0u; }

  // chance * lerp(1.0x, maxMul, (count-1)/5), integer throughout, evaluated
  // with the single divide LAST so nothing is truncated mid-ramp.
  //   scaled = chance * SCALE * (4 + span*(count-1)/5) / 4
  // Numerator first, then one divide by (RSCALE_MUL_UNIT * 5) = 20, which
  // divides REACT_CHANCE_SCALE exactly — so every one of the 6 steps lands on
  // a distinct integer even at chance 1.
  let maxQ = ((rule.cond >> RSCALE_MUL_SHIFT) & RSCALE_MUL_MASK) + RSCALE_MUL_UNIT;
  let span = maxQ - RSCALE_MUL_UNIT;  // quarters above 1.0x
  let num = rule.chance * (RSCALE_MUL_UNIT * 5u + span * (count - 1u));
  let scaled = num / (RSCALE_MUL_UNIT * 5u);
  return min(scaled, REACT_CHANCE_DEN);
}

// ============ A COAT IS A CO-LOCATED VIRTUAL NEIGHBOUR (DESIGN.md §6) ========
// rule-unification W2-J1, 2026-09-24. Before this the grid ignored its own
// stains in reactions -- `doStaining` wrote them and nothing read them -- so
// wet grass burnt exactly like dry and oiled dirt was not flammable, while a
// BODY wearing the same substances (mob.cpp BurnOneLimb: wet douses and blocks
// the burn, oil flashes, acid eats) reacted to them. This is the grid half of
// the one rule DESIGN.md writes down for both; W2-J2 makes the body match it.
//
// THE RULE, in the order the kernel runs it (the spec lives in DESIGN.md §6,
// "A coat is a co-located virtual neighbour"; this is its implementation):
//
//   0. WHICH STAINS ARE COATS. A cell's stain is a coat -- a substance, not a
//      look -- iff its substrate ABSORBS (`absorb.capacity` > 0). Absorbent
//      ground only ever takes a level that the liquid PAID for (stainStep sets
//      STAIN_SPEND; doStaining debits an eighth, the MPM seam spends mass),
//      while a capacity-0 surface takes a free one-level mark (stone under a
//      pond). A free mark that could react would be matter from nothing: the
//      pond re-marks stone for free each time a reaction spends it, so wet
//      stone between a pond and lava would never dry and never sleep. The coat
//      MATERIAL is the stain type's, read out of the stain palette's spare
//      `_r3` (Simulation::UploadTables). A `bodyOnly` stain has no ground type
//      bits and so is never here.
//   1. THE COAT'S RULES FIRST (coatReact). The coat material's PAIR rules, in
//      file order, with the coat as `self` standing where the cell stands: its
//      partners are the cell's six faces (rule direction mask, RNG-rotated,
//      excited fluid synthesized as in doReactions) and then the cell's OWN
//      material. Decay and emit rules of the coat material do NOT run: a
//      coat's lifetime on the ground is its stain's (monotone, see doStaining),
//      and water's evaporation or blood's drying run by the coat would make
//      every stained chunk a clock.
//   2. A RULE FIRED THROUGH THE COAT SPENDS ONE LEVEL of it (amount - 1; at 0
//      the cell is clean) instead of rewriting the coat's side. The coat side's
//      PRODUCT is released only if it is a FLAME (a hot gas, MATF_FLAME), into
//      an open face -- air first, else a gas cell -- of the cell; a flame that
//      has no open face makes the rule not a match at all. Any other product is
//      simply not created: a level is at most an eighth of a liquid cell. The
//      PARTNER's side is ordinary: a face partner takes `neighborBecomes`; the
//      cell itself taking `neighborBecomes` is rewritten and born clean (the
//      coat goes with the voxel).
//      2c. A FLAME A COAT MAKES IS MADE ON ITS WEARER (2026-09-24, the owner's
//      "oil should catch fire easily in all scenarios"): when the coat rule
//      that fired released a flame and did not itself rewrite the cell, the
//      cell CATCHES if it can -- it becomes its catch form (the burning
//      product of its own heat rules, derived at load into `_r2`) -- and the
//      coat goes with it. Oiled grass beside an ember goes up at oil's 700
//      per mille as well as grass's own 220 (both before combustion.spreadPct);
//      oiled dirt, which cannot catch, is unchanged.
//   3. A QUENCHING COAT COVERS THE CELL. If any coat rule MATCHED this tick
//      (partner found, whether or not it rolled) and its coat-side product is
//      not a flame, the cell is COVERED: its own rules below see its coat and
//      nothing else -- no face partners, no emit, faces uncounted in ramps.
//      Wet grass beside an ember is covered while the water boils off, so it
//      does not catch until it is dry; an oil coat's rule makes a flame, so it
//      covers nothing, and the grass under it rolls its own ignition as well
//      (rule 5) while the flashes throw flame at its neighbours. A wet ember
//      is covered too, so it
//      stops emitting flame, and its own douse (`tag:extinguisher`) still runs
//      against the coat.
//   4. THE CELL'S OWN RULES SEE THE COAT (doReactions): after the six faces, a
//      PAIR rule may take the coat as its partner (one roll per rule per tick,
//      as for a face); a direct neighbour-count ramp counts it once (capped at
//      6). Fired through the coat, rule 2 applies to the coat side.
//   5. TWO REACTANTS, ONE RULE EACH. The coat and the cell each fire at most
//      one rule per tick, the coat's first; the cell's rules then see the coat
//      as it is after (thinner, or gone). So an oiled grass cell rolls its own
//      ignition at the dry rate AND flashes its oil -- one rule per cell for
//      both would make oil DELAY grass catching, the opposite of fuel.
//
// A coat is seen ONLY by the cell wearing it. A neighbour cell's rules see the
// cell's material, never its coat -- so `lava + tag:organic -> fire` still
// burns wet grass, exactly as a body's inbound pass (BurnOneLimb section 2)
// burns a wet limb.
//
// DETERMINISM (rule 1). Every write is the cell itself or a face neighbour
// (the partner, and the flame's open face, never the same cell when the
// partner is rewritten), inside the colour lattice's reach-1 guarantee; every
// read is at distance <= 1. The coat's rolls are their own stream
// (COAT_ROLL_SALT), keyed on the SLOT index like every other roll.
//
// SLEEP AND TERMINATION (rule 2). The stain rule's old argument was "stain only
// increases". It no longer does, so the argument is now MASS: every level a
// coat reaction spends was paid for with liquid (rule 0), a firing spends
// exactly one, and nothing a coat releases is liquid or solid -- only a flame,
// which the ordinary fire chain already bounds. A cycle of re-wet and boil
// therefore drains the liquid doing the wetting and ends. A matched coat rule
// holds its chunk awake exactly as doReactions' rules do (and a light-gated
// one does not); a coat with no partner costs one palette load and a bucket
// walk in an AWAKE chunk and never marks anything, so a rained-on meadow
// sleeps. A flame with no open face is not a match, so it cannot pin a buried
// oiled cell beside lava awake.
// coatReact's verdict bits (.x of its return; .y is the cell's word after).
const COAT_COVERED : u32 = 1u;  // a quenching coat rule matched: rule 3
const COAT_SPENT   : u32 = 2u;  // a coat rule fired: a level spent, same material
const COAT_GONE    : u32 = 4u;  // a coat rule rewrote the cell itself
const COAT_ROLL_SALT : u32 = 0xC0A7F11Au;
// materials.h kMatFlagFlame, mirrored (check_invariants `coatflame`). Declared
// here, not in common.wgsl: this kernel is its only reader.
const MATF_FLAME : u32 = 64u;

// ---- THE COAT RULE'S ARITHMETIC, shared with the body (W2-J2) --------------
// Token for token the MIRROR block of the same tag in src/sim/coatrule.h,
// which MobSystem::BurnOneLimb calls for every body; scripts/
// check_invariants.py `coatrule` compares the two streams and the constants.
// Declared here, not in common.wgsl: this kernel is the only WGSL reader.
const COAT_VERDICT_MATCH : u32 = 1u;
const COAT_VERDICT_COVERS : u32 = 2u;
// MIRROR-BEGIN coatrule
fn coatRuleVerdict(flame : bool, openFace : bool) -> u32 {
  if (flame && !openFace) { return 0u; }
  if (flame) { return COAT_VERDICT_MATCH; }
  return COAT_VERDICT_MATCH | COAT_VERDICT_COVERS;
}
fn coatLevelsAfter(amt : u32, price : u32) -> u32 {
  if (amt > price) { return amt - price; }
  return 0u;
}
fn coatRampCount(faces : u32, coatHit : bool, pitchFine : bool) -> u32 {
  if (!coatHit) { return min(faces, 6u); }
  var n = min(faces + 1u, 6u);
  if (pitchFine) { n = max(n, 5u); }
  return n;
}
// MIRROR-END coatrule

// The material of the cell's COAT, or 0 if its stain is not one (rule 0).
fn coatMatOf(w : u32, sub : Material) -> u32 {
  if (!voxStained(w) || !matAbsorbs(sub)) { return 0u; }
  return materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3 & 0xFFFu;
}

// Bits 29 and 30 ("dry", "DRY-WROTE"): a WET STAIN DRYING (stainDry below).
// They used to share DIRTY_R_STAIN / DIRTY_R_STAINW with doStaining, and that
// made two different things one bit: a liquid soaking into the ground (which
// SPENDS liquid -- a body's volume moves) and a bank the liquid has LEFT
// drying (which touches no liquid at all: stainDry refuses any cell with its
// wetter on a face). sim_waterbody.wgsl wbQuiet must tell them apart -- a
// drained pit's banks dry for ~1000 ticks and, read as one bit, held every
// created body CANDIDATE that long (`--gate waterbody` pass N). Same sleep and
// licence semantics as the pair they replace: DRY is an own-chunk idle mark
// (not in FILM_LICENCE), DRYW is a write (fans out, in FILM_LICENCE). Declared
// here and in sim_waterbody.wgsl, its one reader; check_invariants.py `drybits`
// pins both to world.h kDirtyReasonName.
const DIRTY_R_DRY : u32 = 536870912u;
const DIRTY_R_DRYW : u32 = 1073741824u;

// ---- A WET STAIN DRIES (2026-09-25) -----------------------------------------
// A stain type whose material authors `stain.dries` (water) comes off the
// ground on its own: `dries` per mille per tick, one level at a time, the
// chance packed into the stain palette entry's `_r3` bits 12..21 beside the
// coat material (Simulation::UploadTables; common.wgsl stainDryChance). Never
// while the cell touches the liquid that made it -- a pond's banks and bed stay
// wet, and that is what lets them sleep.
//
// SLEEP (rule 2). A drying cell holds its chunk awake ONLY if its top face is
// covered. A cell open above is a top surface, and top surfaces are dried by
// sim_mutate.wgsl's rainFall sampler whether or not their chunk is awake -- so
// a rained-on meadow does not keep every surface chunk awake for the half
// minute it takes to dry; it dries asleep, a column sample at a time. A wet
// wall or cave floor (a flow went past) is not sampled, so it holds its chunk
// until dry: bounded, at most 15 levels at `dries`, and only liquid that
// moved there -- which is activity -- makes one.
//
// Returns true when it WROTE the cell (the caller ends the cell's substep: the
// word it holds is stale).
fn stainDry(c : vec3<i32>, idx : u32, w : u32, m : Material, rnd : u32, probe : bool) -> bool {
  if (m.klass != CLASS_SOLID && m.klass != CLASS_POWDER) { return false; }
  let pal = materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3;
  let chance = stainDryChance(pal);
  if (chance == 0u) { return false; }
  let wetter = pal & 0xFFFu;
  for (var i = 0u; i < 6u; i++) {
    let n = c + faceDir(i);
    if (inBounds(n) && voxMat(voxWordAt(n)) == wetter) { return false; }
  }
  let up = c + vec3<i32>(0, 1, 0);
  var open = true;
  if (inBounds(up)) {
    let um = voxMat(voxWordAt(up));
    open = um == MAT_AIR || materials[um].klass == CLASS_GAS;
  }
  // SLANTED RAIN'S SURFACES (2026-09-30). While it rains along a slope the
  // rain lands on faces with something over them too -- a windward wall, the
  // floor inside a windward door -- and each of those is, by construction, the
  // first blocker on its fall line: exposed by the rain exposure map (read
// leniently, rainMapExposedNear says why). It is the
  // rain's surface exactly as a top face is, so it neither dries nor holds its
  // chunk awake while the rain lasts (measured: without this, a storm's lean
  // wetted every hillside riser and tree flank in the window and the CA went
  // 1.3 -> 9.1 ms a tick, every one of those cells re-wetted as it dried). The
  // map exists on every raining tick the CA runs (C_RAINEXPO), which is the
  // only tick this reads it on. After the rain those faces dry here, awake,
  // bounded by their 15 levels -- the same standing a wall a flow wetted has.
  let raining = (T.weatherRain & RAIN_AMOUNT_MASK) != 0u;
  if (!open && raining && rainMapExposedNear(c)) { open = true; }
  if (!open) { markDirtyR(c, DIRTY_R_DRY); }
  // Nothing open to the sky dries while it rains: that is the rain's surface.
  // (Measured: drying through a storm left a third of a rained-on stone strip
  // CLEAN -- its 1-level wet mark dried between drops.)
  if (open && raining) { return false; }
  if (probe || (hash3(rnd, 0xD41E5u, 0u) % 1000u) >= chance) { return false; }
  let left = voxStainAmt(w) - 1u;
  var nw = w & ~STAIN_BITS;
  if (left > 0u) { nw = nw | packStain(voxStainType(w), left); }
  voxStore(idx, nw);
  markDirtyR(c, DIRTY_R_DRYW);
  return true;
}

fn isFlame(prod : u32) -> bool {
  return prod != PROD_KEEP && prod != MAT_AIR &&
         (materials[prod].flags & MATF_FLAME) != 0u;
}

// Where a released flame goes (rule 2): the first face, in the rule's
// direction mask and RNG rotation, that is air, else the first that is a gas.
// `avoid` is a face that must not be used (the partner being rewritten), 6u
// for none. 6u = no open face.
fn coatReleaseFace(c : vec3<i32>, dmask : u32, rot : u32, avoid : u32) -> u32 {
  var gasFace = 6u;
  for (var i = 0u; i < 6u; i++) {
    let di = (i + rot) % 6u;
    if ((faceDirBit(di) & dmask) == 0u || di == avoid) { continue; }
    let n = c + faceDir(di);
    if (!inBounds(n)) { continue; }
    let nm = voxMat(voxWordAt(n));
    if (nm == MAT_AIR) { return di; }
    if (gasFace == 6u && materials[nm].klass == CLASS_GAS) { gasFace = di; }
  }
  return gasFace;
}

fn coatRelease(c : vec3<i32>, face : u32, prod : u32, rr : u32, stamp : u32) {
  let n = c + faceDir(face);
  let ni = voxWordIndex(n);
  voxStore(ni, packVox(prod, productState(prod, rr >> 4u), stamp));
  markVoxActive(ni);
  markDirtyR(n, DIRTY_R_REACTW);
}

// Spend one level of the cell's coat (rule 2). The rest of the word is kept.
// The caller (main) ends the cell's substep after a spend, so the movement code
// cannot move it with the stale word and put the level back -- which is why
// the word is NOT stamped: a spend is not a move, and a live stamp left on a
// cell that then sits still ALIASES the current one every STAMP_CYCLE ticks
// (common.wgsl), skipping the cell before it can mark keepAwake. Measured in
// `stain-react`: the last oiled cells beside a lava channel stamped by their
// own flashes let the chunk fall asleep on such a tick with 2 of 72 levels
// unburnt, heat still there, forever. STAMP_NEVER never aliases. (Since W2-R
// main's stamp-skipped cells probe their rules and mark keepAwake anyway, so
// this is no longer load-bearing; kept, because a spend is not a move.)
fn coatSpend(c : vec3<i32>, idx : u32, w : u32, stamp : u32) -> u32 {
  _ = stamp;
  let left = coatLevelsAfter(voxStainAmt(w), 1u);
  var nw = packVox(voxMat(w), voxState(w), STAMP_NEVER);
  if (left > 0u) { nw = nw | packStain(voxStainType(w), left); }
  voxStore(idx, nw);
  markVoxActive(idx);
  markDirtyR(c, DIRTY_R_REACTW);
  return nw;
}

// Rules 1-3: the coat material `cm`'s pair rules, run with the coat as self.
fn coatReact(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32,
             m : Material, cm : u32, rnd : u32, probe : bool) -> vec2<u32> {
  let cmat = materials[cm];
  // A coat's rules are PAIR rules (rules 1-3): none of them runs in a ticket
  // (gInTicket). Nothing covered, nothing spent, no reason to wake.
  if (gInTicket) { return vec2<u32>(0u, w); }
  let stamp = stampFor(T.tick, P.substep);
  var keepAwake = false;
  var covered = false;
  for (var ri = 0u; ri < cmat.reactCount; ri++) {
    let rule = reactions[cmat.reactOffset + ri];
    if ((rule.packed & 3u) != RK_PAIR) { continue; }
    if (!lightMatches(rule, c)) { continue; }
    // A coat carries no solute: a concentration-conditioned rule never fires
    // through one (word 0 reads as "not a liquid").
    if (cmat.klass == CLASS_LIQUID && !solRuleAllows(cmat.reactOffset + ri, c, 0u)) { continue; }
    let rr = hash3(rnd ^ COAT_ROLL_SALT, ri, slotIdx);  // SLOT index
    let rot = rr >> 12u;
    let dmask = (rule.packed >> 2u) & 7u;
    // Partner: the cell's own material FIRST (pk == 6u; clause 1a -- the coat
    // touches what it is ON before what is beside it), then the six faces.
    // (W2-J2 moved the cell ahead of the faces for the body's sake, where a
    // coat on skin must bite the skin, not the flesh beside it. On the grid
    // the order is not observable today: no ground coat material has a rule
    // that rewrites its partner, so which matching partner is taken changes
    // no write -- and the rolls are per rule, not per partner.)
    var pk = 7u;
    var pmat = 0u;
    var pni = 0u;
    var psyn = false;
    for (var k = 0u; k < 7u; k++) {
      var di = 6u;
      var nm = mat;
      var nidx = idx;
      var syn = false;
      if (k > 0u) {
        di = (k - 1u + rot) % 6u;
        if ((faceDirBit(di) & dmask) == 0u) { continue; }
        let n = c + faceDir(di);
        if (!inBounds(n)) { continue; }
        let niw = voxIndexAndWord(n);
        nidx = niw.x;
        nm = voxMat(niw.y);
        if (nm == MAT_AIR) {
          nm = fluidOccMat(n);
          if (nm == 0u) { continue; }
          syn = true;
        }
      }
      if (!nbrMatches(rule, nm, materials[nm])) { continue; }
      // The no-op skip doReactions makes (a rule that would turn its partner
      // into what it is and keep self changes nothing and is not a match).
      if (!syn && rule.prodSelf == PROD_KEEP && rule.prodNbr == nm) { continue; }
      pk = di;
      pmat = nm;
      pni = nidx;
      psyn = syn;
      break;
    }
    if (pk == 7u) { continue; }
    let flame = isFlame(rule.prodSelf);
    var rf = 6u;
    if (flame) {
      let avoid = select(6u, pk, rule.prodNbr != PROD_KEEP);
      rf = coatReleaseFace(c, dmask, rot, avoid);
    }
    // Rules 2-3 (coatRuleVerdict, mirrored in src/sim/coatrule.h): a flame
    // with nowhere to go is not a match; a non-flame match covers the cell.
    let verdict = coatRuleVerdict(flame, rf < 6u);
    if ((verdict & COAT_VERDICT_MATCH) == 0u) { continue; }
    keepAwake = keepAwake || (rule.cond & RCOND_GATES) == 0u;
    covered = covered || (verdict & COAT_VERDICT_COVERS) != 0u;
    if (probe || (rr % REACT_CHANCE_DEN) >= rainChance(rule, c, rule.chance)) { continue; }
    // ---- FIRED ----
    if (rf < 6u) { coatRelease(c, rf, rule.prodSelf, rr, stamp); }
    if (rule.prodNbr != PROD_KEEP) {
      if (pk == 6u) {
        // The coat's rule rewrites the cell under it: the coat goes with it.
        reactWriteSelf(c, idx, false, m.klass, rule.prodNbr, rnd, stamp);
        return vec2<u32>(COAT_GONE, 0u);
      }
      let n = c + faceDir(pk);
      if (psyn) { flagFluidConsume(n); }
      if (rule.prodNbr == 0u) { voxStore(pni, 0u); }
      else { voxStore(pni, packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp)); }
      markVoxActive(pni);
      markDirtyR(n, DIRTY_R_REACTW);
      if (!psyn) { flagSupportLoss(n, materials[pmat].klass, rule.prodNbr); }
    }
    // Clause 2c: the flame was made ON the cell, so a cell that can catch
    // CATCHES -- it takes its catch form (its own heat rules' burning product,
    // derived at load: sim/bodyreact.h CatchFormTable, uploaded in the
    // material's spare `_r2`; 0 = it does not catch). No roll of its own: the
    // coat rule's chance is how easily an oiled thing catches. That is the
    // cell's one rewrite this tick (rule 5), and the grid's coat is HELD, so
    // it goes with the cell exactly as a coat rule rewriting its wearer does.
    if (rf < 6u) {
      let catchMat = m._r2 & 0xFFFu;
      if (catchMat != 0u && catchMat != mat) {
        reactWriteSelf(c, idx, false, m.klass, catchMat, rnd, stamp);
        return vec2<u32>(COAT_GONE, 0u);
      }
    }
    let nw = coatSpend(c, idx, w, stamp);
    if (keepAwake) { markDirtyR(c, DIRTY_R_REACT); }
    return vec2<u32>(select(0u, COAT_COVERED, covered) | COAT_SPENT, nw);
  }
  if (keepAwake) { markDirtyR(c, DIRTY_R_REACT); }
  return vec2<u32>(select(0u, COAT_COVERED, covered), w);
}

// Runs the cell's reaction bucket. At most one rule fires per tick. Returns
// true if SELF changed material (caller then skips movement this substep).
// Matching-but-unfired rules mark the chunk dirty so reactive neighborhoods
// stay awake until they resolve — sleeping stays activity-bounded because
// every chain (fire, growth, decay) terminates by transforming its inputs.
// TWO BASES (§4.1), exactly as in main: `idx` is the PHYSICAL word index and is
// only ever a memory address for voxStore; `slotIdx` is the SLOT cell index and
// is the only thing that may key the RNG. Passing `idx` to hash3 here made every
// reaction roll a function of ALLOCATION HISTORY, so a paged run diverged from a
// dense one with no other symptom — silently, because under the identity map
// (dense) the two are equal. Found as a deterministic, reproducible lava/stone
// swap at slot 9450 t43 in --vk-smoke-loud --residency paged.
//
// `synthSelf`: self is not a voxel but EXCITED FLUID of material `mat` in the
// air cell `c` (excitedReact). Every rule runs unchanged; only committing a
// self product differs, and reactWriteSelf owns that difference.
//
// `coat` / `covered`: the cell's own coat material (0 = none) and whether a
// quenching coat is reacting this tick (coatReact) -- rules 3 and 4 of "A COAT
// IS A CO-LOCATED VIRTUAL NEIGHBOUR" above. The coat is a PAIR partner after
// the six faces; covered hides the faces from pairs, emits and ramps. A rule
// fired through the coat also returns true: the word changed (a level spent)
// even where the material did not, and the caller must not move the stale one.
// ---- REACTION EFFECTS: "this rule fired HERE" (docs/PLAN_alchemy_chemistry.md A)
//
// A rule authored with `"effects"` in reactions.json (sodium + water ->
// explode) carries a 5-bit fx id in Reaction.cond bits 24..28
// (materials.h kCondFxShift). The CA cannot run an explosion -- a blast is
// an ExplosionOp through sim_explode.wgsl, authored on the CPU and pushed
// through the op queue like a grenade (rule 3) -- so all it does is REPORT:
// it records the firing into the pageFaults record's per-tick block
// (world.h kPageFaultReactFx*), the snapshot ring carries that back at the
// fixed World::kSnapshotLatency, and game/session.cpp turns the survivors
// into ExplosionOps. The rule's own products are written here as always.
//
// ORDER-FREE, which is the whole design (rule 1). A busy tick (a lump of
// sodium in a lake) fires hundreds of these; the record keeps RFX_SLOTS of
// them, and WHICH ones must not depend on thread scheduling. So there is no
// append cursor: each firing picks a slot by hash3 of (seed, tick, cell) and
// atomicMax's a key that is unique per (fx id, cell) into it. The survivors
// are the per-slot maxima of the SET of firings -- the same set, the same
// maxima, on every machine. The key's cell half is scrambled by an odd
// multiply (a bijection mod 2^27, world.h kReactFxScramble) so a slot's winner
// is not always the cell nearest the window's far corner. The count and the
// origin/tick words are an atomicAdd and identical stores: order-free too.
//
// Out-of-window cells (a ticket chunk; TICKET_SLOTS) are not reported: the
// key's slot coordinates decode to a WINDOW cell. Their products still apply; only the
// effect is skipped, and a ticket chunk is a peer's, whose own window
// reports it.
//
// Constants mirror world.h (check_invariants `pairs`); declared HERE, not in
// common.wgsl, because only this shader writes the record (CLAUDE.md: a
// common.wgsl edit misses the SPIR-V cache for every shader).
const RFX_FIRES     : u32 = 40u;   // kPageFaultReactFxFires
const RFX_ORIGIN    : u32 = 41u;   // kPageFaultReactFxOrigin (41..43)
const RFX_TICK      : u32 = 44u;   // kPageFaultReactFxTick
const RFX_SLOT0     : u32 = 48u;   // kPageFaultReactFxSlot0
const RFX_SLOTS     : u32 = 16u;   // kPageFaultReactFxSlots
const RFX_COND_SHIFT: u32 = 24u;   // kCondFxShift
const RFX_COND_MASK : u32 = 31u;   // kCondFxMask
const RFX_CELL_BITS : u32 = 27u;   // kReactFxCellBits
const RFX_SCRAMBLE  : u32 = 0x0B5AD4EBu;  // kReactFxScramble
const RFX_LIQ_EATEN : u32 = 45u;   // kPageFaultReactLiquidEaten

// ---- THE CA HALF OF THE REACTION LEDGER -------------------------------------
// A reaction that rewrites a SETTLED liquid voxel to another material removes
// that voxel's eighths from the liquid books. When the liquid is EXCITED the
// seam counts it (FA_CONSUMED, via flagFluidConsume -> consumeApply); a voxel
// never crosses the seam, so before this word nothing counted it and a
// reaction gate's ledger (`fluid-react`: plants growing into the water they
// drink) could only be asserted to within a tolerance. Called with the word
// the cell held BEFORE the write and the material written over it; a write of
// the same material (a no-op rewrite) eats nothing. A liquid PRODUCT carries
// the eighths on (carriedState) -- they are still counted here, because the
// question this word answers is "how much of the liquid that was there is
// gone", per material, and a gate that wants mass across a liquid->liquid
// transform adds the product back itself.
// An order-free atomicAdd into the per-tick reaction record (world.h [45]);
// nothing in the sim reads it (rule 1).
fn reactLiquidEaten(oldWord : u32, prod : u32) {
  let om = voxMat(oldWord);
  if (om == MAT_AIR || om == prod) { return; }
  if (materials[om].klass != CLASS_LIQUID) { return; }
  atomicAdd(&pageFaults[RFX_LIQ_EATEN], voxState(oldWord) + 1u);
}

fn reactFxNote(rule : Reaction, c : vec3<i32>) {
  let fx = (rule.cond >> RFX_COND_SHIFT) & RFX_COND_MASK;
  if (fx == 0u) { return; }
  if (!inWindow(c, T.origin)) { return; }
  // The cell's SLOT coordinates (world coords wrapped into the toroidal
  // window), never window-relative ones: the slot hash and the key must not
  // depend on where the window sits (CLAUDE.md: an identity is the slot
  // index), or the same firings would pick different winners -- different
  // ExplosionOps -- for a player standing elsewhere. Unique within the
  // window, so the CPU decodes it back with the origin (ReactFxDecodeCell).
  let wr = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let lin = (wr.z * WORLD_N + wr.y) * WORLD_N + wr.x;
  atomicAdd(&pageFaults[RFX_FIRES], 1u);
  atomicStore(&pageFaults[RFX_ORIGIN + 0u], bitcast<u32>(T.origin.x));
  atomicStore(&pageFaults[RFX_ORIGIN + 1u], bitcast<u32>(T.origin.y));
  atomicStore(&pageFaults[RFX_ORIGIN + 2u], bitcast<u32>(T.origin.z));
  atomicStore(&pageFaults[RFX_TICK], T.tick + 1u);
  let slot = hash3(T.seed ^ 0xFE7C0DEu, T.tick, lin) % RFX_SLOTS;
  let key = (fx << RFX_CELL_BITS) | ((lin * RFX_SCRAMBLE) & ((1u << RFX_CELL_BITS) - 1u));
  atomicMax(&pageFaults[RFX_SLOT0 + slot], key);
}

fn doReactions(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32,
               m : Material, rnd : u32, synthSelf : bool, coat : u32,
               covered : bool, probe : bool) -> bool {
  var keepAwake = false;
  let stamp = stampFor(T.tick, P.substep);
  // A partial grain cannot become a whole SOLID (a 1/8 seed does not grow a
  // whole sprout): such rules are skipped, not rolled, and do not hold the
  // chunk awake. Eight grains that merge into a full cell react normally.
  let selfPartialPowder = !synthSelf && matHasPowderMass(m) && powderIsPartial(w);
  // The cell's charge (E2, THE CHARGE AS PARTNER below), read once, on the
  // first rule that wants a discharge and found none at a face.
  var eP = ELEC_P_UNREAD;

  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    let kind = rule.packed & 3u;
    let dmask = (rule.packed >> 2u) & 7u;
    // In a ticket only DECAY runs (gInTicket, above): skipped before the roll
    // and before keepAwake, exactly as a light-gated rule out of its phase is.
    if (gInTicket && kind != RK_DECAY) { continue; }
    if (selfPartialPowder && rule.prodSelf != PROD_KEEP && rule.prodSelf != MAT_AIR &&
        materials[rule.prodSelf].klass == CLASS_SOLID) { continue; }

    // Light/phase gate. A rule whose condition is not met is skipped WITHOUT
    // setting keepAwake — that is what lets a lit pond go back to sleep at
    // night instead of spinning on a rule that cannot fire (rule 2). The
    // chunk is re-woken when the phase crosses back, see wakeOnPhaseChange.
    if (!lightMatches(rule, c)) { continue; }
    // A concentration condition (reactions.json "solute"/"cMin"; the solute
    // layer's rule side array): brine electrolysis needs the salt, not just
    // the water. One side-array word per rule tried; 0 for nearly all. Only a
    // LIQUID self can carry the condition (materials.cpp refuses it on any
    // other), so fire, smoke and stone never pay the side-array read.
    if (m.klass == CLASS_LIQUID && !solRuleAllows(m.reactOffset + ri, c, w)) { continue; }

    // Drawn AFTER the gate: hash3 is stateless and keyed on (rnd, ri, slot),
    // so a skipped rule consumes nothing and every later draw is unchanged.
    let rr = hash3(rnd, ri, slotIdx);  // SLOT index: never the page index
    let rot = rr >> 12u;

    // A light-gated rule does not hold its chunk awake even when it MATCHES.
    //
    // The unconditional rules use keepAwake to mean "this neighbourhood is
    // reactive, re-examine it next tick", which is right for chains that
    // resolve on their own (fire burns out, growth terminates). A light-gated
    // rule has no such terminus: it stays matched for as long as the sun is in
    // the right part of the sky, which is thousands of ticks. Letting it set
    // keepAwake pins every affected chunk awake for half of every in-game day
    // — measured at 292/32768 chunks still active from ONE such rule, against
    // a budget of 32 (CLAUDE.md rule 2).
    //
    // Instead the day phase itself is the wake signal: Simulation::
    // EncodeWakeAll re-dirties the world on the tick daylight switches on or
    // off, which is a handful of ticks per in-game day. Between those
    // boundaries a chunk with only light-gated work sleeps, and the rules
    // still fire on the ticks it is awake for other reasons.
    // RCOND_GATES, not the whole byte: RAINDAMP only rescales an ordinary
    // ignition rule, and counting it as a gate would let a fire front fall
    // asleep mid-spread. RAIN is a gate — a douse on something that does not
    // resolve by itself must not pin its chunk awake for a whole storm.
    let lightGated = (rule.cond & RCOND_GATES) != 0u;

    if (kind == RK_DECAY) {
      // Neighbour-count scaling (frontier rules — see scaledChance). Returns
      // rule.chance untouched for the ordinary unscaled case; 0 means the cell
      // has no qualifying neighbours and the rule is inert here this tick.
      var chance = rainChance(rule, c, scaledChance(rule, c, coat, covered));
      // THIN SMOKE DISSIPATES (sim.gasThinDecayMul, see gasThinMul): the
      // gas's own fade-to-air rule, scaled -- same roll, so x1 is today.
      if (GAS_THIN_ON && rule.prodSelf == MAT_AIR) {
        chance = min(chance * gThinMul, REACT_CHANCE_DEN);
      }
      if (chance == 0u) { continue; }
      keepAwake = keepAwake || !lightGated;
      if (!probe && (rr % REACT_CHANCE_DEN) < chance) {
        reactFxNote(rule, c);
        reactWriteSelf(c, idx, synthSelf, m.klass, rule.prodSelf, rnd, stamp);
        return true;
      }
    } else if (kind == RK_EMIT) {
      // first air cell among allowed dirs (RNG-rotated scan). A covered cell
      // has no open face: the coat is between it and the air.
      for (var i = 0u; i < 6u && !covered; i++) {
        let di = (i + rot) % 6u;
        if ((faceDirBit(di) & dmask) == 0u) { continue; }
        let n = c + faceDir(di);
        if (!inBounds(n)) { continue; }
        if (voxMat(voxWordAt(n)) != MAT_AIR) { continue; }
        keepAwake = keepAwake || !lightGated;
        if (!probe && (rr % REACT_CHANCE_DEN) < rainChance(rule, c, rule.chance)) {
          reactFxNote(rule, c);
          let ni = voxWordIndex(n);  // resolved only for the cell that is written
          voxStore(ni, packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp));
          // Only a LIQUID product can carry a value, so only one can inherit a
          // stale one (solCarried): smoke and fire emits skip the table read.
          if (solIsLiquidMat(rule.prodNbr)) { solClearStale(n); }
          markVoxActive(ni);
          markDirtyR(n, DIRTY_R_REACTW);
          markDirtyR(c, DIRTY_R_REACTW);
          if (rule.prodSelf != PROD_KEEP) {
            reactWriteSelf(c, idx, synthSelf, m.klass, rule.prodSelf, rnd, stamp);
            return true;
          }
          return false;  // emitted; self unchanged, may still move
        }
        break;  // one roll per rule per tick
      }
    } else {  // RK_PAIR
      var faceMatched = false;
      for (var i = 0u; i < 6u && !covered; i++) {
        let di = (i + rot) % 6u;
        if ((faceDirBit(di) & dmask) == 0u) { continue; }
        let n = c + faceDir(di);
        if (!inBounds(n)) { continue; }
        let niw = voxIndexAndWord(n);  // one table resolution for index + word
        let ni = niw.x;
        var nmat = voxMat(niw.y);
        // Excited-fluid synthesis: an air cell holding MPM particles reads
        // as a liquid neighbour of the particles' material, so every
        // authored PAIR rule works against excited water exactly as against
        // a fullness voxel. Consumption crosses the seam through the flag —
        // the particles die in consumeApply this same tick (plan §6.2).
        var synthFluid = false;
        if (nmat == MAT_AIR) {
          nmat = fluidOccMat(n);
          if (nmat == 0u) { continue; }
          synthFluid = true;
        }
        if (!nbrMatches(rule, nmat, materials[nmat])) { continue; }
        // A PAIR that would turn this neighbour into WHAT IT ALREADY IS, and
        // leave self alone, is a no-op, and it is not a match. The case that
        // found it: grass spreads onto `tag:soil`, and grass carries `soil`, so
        // every grass cell matched its grass neighbours and rewrote grass over
        // grass at 3 per mille. Each such write marked REACTW, which is in
        // FILM_LICENCE and re-dirties the chunk, so a lawn in daylight never
        // slept. Skipped here, before keepAwake and before the roll, so the
        // scan goes on to a neighbour the rule can actually change (dirt,
        // sand). A synthesized fluid neighbour is never a no-op — its product
        // is written into the AIR cell — so it is excluded.
        if (!synthFluid && rule.prodSelf == PROD_KEEP && rule.prodNbr == nmat) {
          continue;
        }
        keepAwake = keepAwake || !lightGated;
        faceMatched = true;
        // Weather scale here, after the neighbour matched, so a dry-sky tick
        // and a wood cell with nothing hot beside it never pay the probe.
        if (!probe && (rr % REACT_CHANCE_DEN) < rainChance(rule, c, rule.chance)) {
          reactFxNote(rule, c);
          if (rule.prodNbr != PROD_KEEP) {
            if (synthFluid) { flagFluidConsume(n); }
            // For a synthesized neighbour ni is the air cell: a product
            // writes into it (condensed stone, grown plant); prodNbr == 0
            // rewrites air over air, harmless.
            //
            // A solvent neighbour being replaced: its mass precipitates in
            // place (it cannot move on -- a neighbour's neighbour is past the
            // lattice's write reach). Boiling brine leaves its salt.
            var nsv = 0u;
            if (!synthFluid) {
              nsv = solCarried(n, niw.y);
              reactLiquidEaten(niw.y, rule.prodNbr);  // settled neighbour eaten
            }
            var rep = vec2<u32>(rule.prodNbr, 0u);
            if (nsv != 0u) { rep = solOnReplace(n, nsv, rule.prodNbr, false, 0u); }
            if (rep.y != 0u) {
              voxStore(ni, packVox(rep.x, solPrecipState(rep.x, rep.y, n), stamp));
            } else if (rule.prodNbr == 0u) { voxStore(ni, 0u); }
            else {
              // A partial neighbour keeps its eighths (carriedState).
              let ne = select(cellEighths(niw.y), 0u, synthFluid);
              voxStore(ni, packVox(rule.prodNbr, carriedState(rule.prodNbr, ne, rr >> 4u), stamp));
            }
            markVoxActive(ni);
            markDirtyR(n, DIRTY_R_REACTW);
            if (!synthFluid) {
              flagSupportLoss(n, materials[nmat].klass, rule.prodNbr);
            }
          }
          if (rule.prodSelf != PROD_KEEP) {
            reactWriteSelf(c, idx, synthSelf, m.klass, rule.prodSelf, rnd, stamp);
            return true;
          }
          markDirtyR(c, DIRTY_R_REACTW);
          return false;  // neighbor transformed; self may still move
        }
        break;  // one roll per rule per tick
      }
      // ---- THE COAT AS PARTNER (rule 4), after the faces ----------------
      // Only if no face matched: one roll per rule per tick, whichever
      // partner it is. No direction mask -- the coat is not in a direction.
      // The no-op skip applies as for a face. Fired, the coat side follows
      // rule 2: one level spent, a flame product released into an open face
      // (none open = not a match), any other product not created. A self
      // product rewrites the cell, and the coat goes with it.
      var partnered = faceMatched;
      if (!faceMatched && coat != 0u && nbrMatches(rule, coat, materials[coat]) &&
          !(rule.prodSelf == PROD_KEEP && rule.prodNbr == coat)) {
        let flame = isFlame(rule.prodNbr);
        var rf = 6u;
        if (flame) { rf = coatReleaseFace(c, dmask, rot, 6u); }
        if ((coatRuleVerdict(flame, rf < 6u) & COAT_VERDICT_MATCH) != 0u) {
          partnered = true;
          keepAwake = keepAwake || !lightGated;
          if (!probe && (rr % REACT_CHANCE_DEN) < rainChance(rule, c, rule.chance)) {
            reactFxNote(rule, c);
            if (rf < 6u) { coatRelease(c, rf, rule.prodNbr, rr, stamp); }
            if (rule.prodSelf != PROD_KEEP) {
              reactWriteSelf(c, idx, synthSelf, m.klass, rule.prodSelf, rnd, stamp);
            } else {
              _ = coatSpend(c, idx, w, stamp);
            }
            return true;
          }
        }
      }
      // ---- THE CHARGE AS PARTNER (E2), after the faces and the coat --------
      // A rule wanting a discharge (tag:electric) whose partner was not found
      // at a face or in the coat: the cell's OWN charge stands in for it. One
      // roll per rule per tick, as for any partner; the chance ramps with P.
      // The discharge side's product goes into an open face or is lost; the
      // self product is written as for any pair. Not for a synthesized self
      // (air holds no charge) and not for a covered cell (rule 3: it sees its
      // coat and nothing else). Only conductors (or wet cells) read the field.
      if (!partnered && !synthSelf && !covered && elecRuleWantsDischarge(rule)) {
        if (eP == ELEC_P_UNREAD) { eP = elecCellCharge(c, w, m); }
        if (eP >= elecParams[EP_REACT_MIN]) {
          keepAwake = keepAwake || !lightGated;
          if (!probe && (rr % REACT_CHANCE_DEN) <
                            elecRampChance(rainChance(rule, c, rule.chance), eP)) {
            reactFxNote(rule, c);
            elecDeposit(c, dmask, rot, rule.prodNbr, rr, stamp);
            if (rule.prodSelf != PROD_KEEP) {
              reactWriteSelf(c, idx, synthSelf, m.klass, rule.prodSelf, rnd, stamp);
              return true;
            }
            markDirtyR(c, DIRTY_R_REACTW);
            return false;
          }
        }
      }
    }
  }
  // ---- THERMAL TRANSITIONS (materials.json "thermal"), after the bucket ----
  // Not for excited fluid (in motion; the seam owns it) and not in a ticket
  // (no heat there). A transition that CAN fire holds the chunk awake: it
  // consumes its input (snow -> water, wood -> ember, water -> ice), so the
  // hold is bounded by the matter present -- the finite-process exception the
  // light-gate note allows. One that cannot is inert and holds nothing.
  if (!synthSelf && !gInTicket && ((m._r2 >> HEAT_R2_TRANS_SHIFT) & 3u) != 0u) {
    let hr = heatReact(c, idx, slotIdx, w, mat, m, rnd, probe, stamp);
    if (hr == 2u) { return true; }
    if (hr == 1u) { keepAwake = true; }
  }
  // ---- CHARGE: OHMIC IGNITION AND CRACKLE (E2), after the bucket ----------
  // Conductors only (the rest hold no P short of being a source). Same
  // contract as heatReact: 2 = self rewritten, 1 = could fire (hold awake).
  if (!synthSelf && !gInTicket && (m._r2 & ELEC_R2_CONDUCTS) != 0u) {
    let er = elecReact(c, idx, slotIdx, w, mat, m, rnd, probe, stamp);
    if (er == 2u) { return true; }
    if (er == 1u) { keepAwake = true; }
  }
  if (keepAwake) { markDirtyR(c, DIRTY_R_REACT); }
  return false;
}

// ---- E2: OHMIC IGNITION / CHAR AND CRACKLE ----------------------------------
// (see "E2: WHAT CHARGE DOES IN THE CA" by the elec bindings). For a
// conductor with charge P at its own cell:
//   IGNITE / CHAR. E = P x resist (the cell's own, wet rule included: a wet
//   plank conducts better and heats LESS). chance = min(cap, E x q / 16) with
//   cap the material's electric.ignite.chance and q sim.elecIgniteGain (x32),
//   then x (air faces + 2) / 8 as heatReact's frontier: a cell with an air
//   face becomes ignite.into, a buried one its char product at 2/8 of the
//   rate. Copper's resist 1 makes E tiny: it never heats.
//   CRACKLE. With an air face, P at or over a tier's threshold throws that
//   tier's material (spark, or arc for the strongly charged) into one air face
//   (RNG-rotated) at chance (P - threshold) x q / 16, capped at
//   ELEC_CRACKLE_CAP. The thresholds sit above the emitted material's own
//   source, which is what bounds it (the header block above).
// Returns 0 inert, 1 could fire (hold the chunk awake), 2 self rewritten.
fn elecReact(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32, m : Material,
             rnd : u32, probe : bool, stamp : u32) -> u32 {
  if (elecParams[EP_MODE] == 0u) { return 0u; }
  let p = elecAtCell(c);
  if (p == 0u) { return 0u; }
  // The air faces: how many, for the frontier scaling and the char/ignite
  // choice. Distance-1 reads only.
  var air = 0u;
  for (var f = 0u; f < 6u; f++) {
    let n = c + faceDir(f);
    if (inBounds(n) && voxMat(voxWordAt(n)) == MAT_AIR) { air++; }
  }
  var awake = 0u;
  let at = EP_MAT + (mat & 0xFFFu) * EP_MAT_STRIDE;
  let w1 = elecParams[at + 1u];
  let cap = elecParams[at + 2u];
  let prod = select((w1 >> 12u) & 0xFFFu, w1 & 0xFFFu, air != 0u);
  let q = elecParams[EP_IGNITE_Q];
  let res = elecResistOf(w, c);
  if (prod != 0u && cap != 0u && q != 0u && res != 0u) {
    let e = p * res;                 // <= 65535 * 254
    let lim = (cap << 4u) / q;       // cap <= 2e6
    var ch = select(cap, (e * q) >> 4u, e < lim);
    ch = ch * (air + 2u) / 8u;
    if (ch != 0u) {
      awake = 1u;
      if (!probe) {
        let rr = hash3(rnd ^ ELEC_IGNITE_SALT, mat, slotIdx);  // SLOT index keys the dice
        if ((rr % REACT_CHANCE_DEN) < ch) {
          reactWriteSelf(c, idx, false, m.klass, prod, rnd, stamp);
          return 2u;
        }
      }
    }
  }
  if (air == 0u) { return awake; }
  var cm = 0u;
  var thr = 0u;
  if (p >= elecParams[EP_CRACKLE_HI_P]) {
    cm = elecParams[EP_CRACKLE_HI_MAT];
    thr = elecParams[EP_CRACKLE_HI_P];
  } else if (p >= elecParams[EP_CRACKLE_LO_P]) {
    cm = elecParams[EP_CRACKLE_LO_MAT];
    thr = elecParams[EP_CRACKLE_LO_P];
  }
  if (cm == 0u) { return awake; }
  let ch = min(((p - thr) * elecParams[EP_CRACKLE_Q]) >> 4u, ELEC_CRACKLE_CAP);
  if (ch == 0u) { return awake; }
  awake = 1u;
  if (probe) { return awake; }
  let rr = hash3(rnd ^ ELEC_CRACKLE_SALT, mat, slotIdx);
  if ((rr % REACT_CHANCE_DEN) >= ch) { return awake; }
  let rot = rr >> 12u;
  for (var i = 0u; i < 6u; i++) {
    let n = c + faceDir((i + rot) % 6u);
    if (!inBounds(n) || voxMat(voxWordAt(n)) != MAT_AIR) { continue; }
    let ni = voxWordIndex(n);
    voxStore(ni, packVox(cm, productState(cm, rr >> 4u), stamp));
    markVoxActive(ni);
    markDirtyR(n, DIRTY_R_REACTW);
    markDirtyR(c, DIRTY_R_REACTW);
    break;
  }
  return awake;
}

// Is ground cell n below water's freezing point (T = ambient + X < 0)? The
// staining rule's absorption skips frozen ground (doStaining). 0 is the
// freeze threshold the climate checks hold every biome to (heat units, 0 =
// water freezes); the layer off answers "no".
fn heatGroundFrozen(n : vec3<i32>) -> bool {
  if (heatParams[HP_MODE] == 0u || !inWindow(n, T.origin)) { return false; }
  return heatAmbient(n) + i32(heatX(n)) < 0;
}

// ---- THE THERMAL TRANSITIONS (docs/PLAN_temperature.md §6) -----------------
// Up to two per material, packed by heat.cpp PackHeatMaterial into heatParams
// at HP_MAT + mat * 8: [w0 thresholds|kind|scale|surface, w1 chance, w2
// product|partial product]. The cell's actual temperature is T = ambient + X.
//   melt / ignite fire when T > threshold, freeze when T < threshold; the
//   chance ramps linearly from 0 at the threshold to `chance` at `full`.
//   FRONTIER: a melt or a freeze needs a face that is not this material (a
//   bank melts from its surface in, a pond skins from its banks); an ignition
//   needs an AIR face. More such faces, faster: x (count + 2) / 8.
//   SURFACE (freeze): only with air directly above -- once a column's top is
//   ice the water under it has ice above and can never freeze, so ice is at
//   most one cell thick per column: a lake skins over, never freezes solid.
// Reads at distance 1 only (the lattice's read bound). Returns 0 inert,
// 1 could fire (keep awake), 2 fired (self rewritten).
const HEAT_ROLL_SALT : u32 = 0x7E3A1D5u;
const HM_FIRE_LOG : u32 = 64u;
const HEAT_FIRE_LOG_MAX : u32 = 8u;
fn heatReact(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32, m : Material,
             rnd : u32, probe : bool, stamp : u32) -> u32 {
  if (heatParams[HP_MODE] == 0u) { return 0u; }
  if (!inWindow(c, T.origin)) { return 0u; }
  let t = heatAmbient(c) + i32(heatX(c));
  let n = min((m._r2 >> HEAT_R2_TRANS_SHIFT) & 3u, 2u);
  var awake = 0u;
  for (var k = 0u; k < n; k++) {
    let at = HP_MAT + mat * HP_MAT_STRIDE + 3u * k;
    let w0 = heatParams[at];
    let kind = (w0 >> 20u) & 3u;
    let thr = i32(w0 & 1023u) - HEAT_THR_BIAS;
    let full = i32((w0 >> 10u) & 1023u) - HEAT_THR_BIAS;
    var num = 0;
    var den = 1;
    if (kind == HEAT_KIND_FREEZE) {
      if (t >= thr) { continue; }
      num = min(thr - t, thr - full);
      den = max(thr - full, 1);
    } else {
      if (t <= thr) { continue; }
      num = min(t - thr, full - thr);
      den = max(full - thr, 1);
    }
    if (((w0 >> 24u) & 1u) != 0u) {
      let up = c + vec3<i32>(0, 1, 0);
      if (!inBounds(up) || voxMat(voxWordAt(up)) != MAT_AIR) { continue; }
    }
    let air = ((w0 >> 22u) & 3u) == HEAT_SCALE_AIR;
    var faces = 0u;
    for (var f = 0u; f < 6u; f++) {
      let nb = c + faceDir(f);
      if (!inBounds(nb)) { continue; }
      let nm = voxMat(voxWordAt(nb));
      if (select(nm != mat, nm == MAT_AIR, air)) { faces++; }
    }
    if (faces == 0u) { continue; }
    let chance = (heatParams[at + 1u] / u32(den)) * u32(num) * (faces + 2u) / 8u;
    if (chance == 0u) { continue; }
    awake = 1u;
    if (probe) { continue; }
    let rr = hash3(rnd ^ HEAT_ROLL_SALT, k, slotIdx);  // SLOT index keys the dice
    if ((rr % REACT_CHANCE_DEN) >= chance) { continue; }
    let w2 = heatParams[at + 2u];
    var prod = w2 & 0xFFFu;
    let alt = (w2 >> 12u) & 0xFFFu;
    if (alt != 0u && cellEighths(w) != 0u && cellEighths(w) < 8u) { prod = alt; }
    atomicAdd(&heatMeta[HM_MELTS + kind - 1u], 1u);
    // The firing log (heat.h kHmFireLog): diagnostic, never read by the sim.
    let fl = atomicAdd(&heatMeta[HM_FIRE_LOG], 1u);
    if (fl < HEAT_FIRE_LOG_MAX) {
      let fo = HM_FIRE_LOG + 1u + 4u * fl;
      atomicStore(&heatMeta[fo], bitcast<u32>(c.x));
      atomicStore(&heatMeta[fo + 1u], bitcast<u32>(c.y));
      atomicStore(&heatMeta[fo + 2u], bitcast<u32>(c.z));
      atomicStore(&heatMeta[fo + 3u], kind);
    }
    reactWriteSelf(c, idx, false, m.klass, prod, rnd, stamp);
    return 2u;
  }
  return awake;
}

// ---- staining (DESIGN.md §6) ------------------------------------------------
// A staining liquid marks the voxels it touches. The mark lives in the voxel
// word's spare bits (STAIN_* in common.wgsl) as a 3-bit type + 4-bit amount:
// no side buffer, no struct growth, and it survives movement because every sim
// write carries the stain across (packVoxKeepStain).
//
// Authored per material, not per material PAIR — "blood stains what it touches"
// is one line in materials.json and applies to every surface in the game,
// present and future, which is the same anti-N×M argument tags exist for
// (CLAUDE.md conventions). Rules:
//
//   * A staining liquid rolls once per tick against `chance` (per-mille).
//   * On success it stains ONE face neighbour — chosen by an RNG rotation, so
//     which one is deterministic but not biased toward an axis.
//   * The stain ADDS to what the neighbour already carries OF ITS OWN TYPE,
//     saturating at the ceiling. Repeated contact deepens a stain rather than
//     resetting it. A FOREIGN stain is never painted over (only a washer may
//     touch it, and only downward) — see `canStain` below.
//   * Having stained, it may CONSUME the voxel (per-mille `consume`), which
//     deletes it to air and lets the liquid flow into the hole.
//
// ---- DETERMINISM (rule 1) ----
// Write reach is exactly 1 cell (a face neighbour), which is what the 3x3x3
// colour lattice bounds; two cells acting in the same pass are >=3 apart, so
// no two stainers can write the same neighbour. All integer, all from
// hash3(seed, tick, cell) — no scheduling, no atomics, no float.
//
// ---- SLEEP (rule 2) ----
// This is the subtle half, and it is why the rule tracks `progress`.
// A pool of blood sitting on stone is a PERMANENT condition: the liquid is
// there, the stone is there, and a rule that says "keep this chunk awake while
// I am touching something stainable" would pin every blood-soaked chunk awake
// forever — the exact failure the light-gated rules hit (see the keepAwake note
// in doReactions, and gotcha: light-gated rules never sleep).
//
// So the chunk is kept awake ONLY while there is work left to do: a neighbour
// that is not yet stained to the full amount this material applies. Once every
// touching surface has taken all the stain it can, nothing marks the chunk and
// the pool settles and sleeps. That termination is what makes the rule
// decisively subcritical: the reachable surface is finite, each cell's stain
// is bounded by STAIN_AMT_MAX, and stain only ever increases.
//
// ---- absorption and washing (DESIGN.md §6) ----------------------------------
// Two behaviours layered on the rule above, both authored as data:
//
//   * ABSORPTION. A substrate declares `absorb: {capacity: N}` — how deep a
//     stain it will hold. A staining liquid soaking into UNSATURATED ground
//     SPENDS ITSELF doing it: one eighth of the source cell's fullness per
//     successful contact. So a puddle on dry grass drains away into the grass,
//     and only once the ground under it is saturated does the water stop
//     vanishing and start to persist as a pool on top. That "spend a unit of
//     mass" step is the whole feature — without it the puddle stains the ground
//     and then sits there full forever, which is the current behaviour and is
//     not absorption at all.
//
//     The capacity comes off the NEIGHBOUR (the ground), and the per-contact
//     step off the STAINER (the liquid): how deep the ground can get wet is a
//     property of the ground, how fast it soaks is a property of the liquid.
//     Capacity 0 (every material that predates this, all stone) means the
//     liquid never soaks in and pools immediately.
//
//   * WASHING, and the newest stain winning (2026-09-25). A liquid with
//     `stain: {washes: true}` rinses a FOREIGN stain out in one contact and
//     leaves the cell wet at one level; any other staining liquid REPLACES a
//     foreign stain the same way (oil over blood is oil). Either costs the
//     liquid an eighth, on stone too. The wet then DRIES (stainDry), so the
//     blood is actually gone and not merely relabelled.
//
// ---- SLEEP (rule 2) ----
// Both additions preserve the termination argument, and that is the thing to
// check when editing this. Every one of these is a monotone step toward a
// bounded fixed point: stain rises only to min(capacity, addAmt); a replaced
// stain costs the replacing liquid an eighth; absorbed fullness falls only to
// 0 (the cell dies).
// Nothing here ever increases the work remaining, so `progress` goes false and
// stays false, and a saturated puddle on saturated ground sleeps. A rule that
// could both wet and dry the same cell would NOT terminate — which is why
// stainDry never touches a cell while the liquid that makes its stain is
// against it: the wetting and the drying are never both live on one cell.
//
// Returns whether the caller should keep the cell awake.
fn doStaining(c : vec3<i32>, idx : u32, selfWord : u32, m : Material,
               rnd : u32, probe : bool) -> bool {
  let stainType = matStainType(m);
  let addAmt = matStainAmount(m);
  let washes = matWashes(m);
  let stamp = stampFor(T.tick, P.substep);

  // Rotate the scan so the stained neighbour is not biased toward -Y. One roll
  // decides WHETHER we stain this tick; the rotation decides WHICH neighbour.
  let rot = rnd >> 7u;
  let fires = !probe && stainFires(m, rnd);

  // This cell's own word, for the absorption debit below. Passed in from main
  // rather than re-read: doReactions returns true (and main returns) whenever
  // it rewrites SELF, so reaching here means the word main loaded is current.
  let selfMat = voxMat(selfWord);
  let selfIsLiquid = materials[selfMat].klass == CLASS_LIQUID;

  var progress = false;  // is there still unstained surface in reach?
  for (var i = 0u; i < 6u; i++) {
    let di = (i + rot) % 6u;
    let n = c + faceDir(di);
    if (!inBounds(n)) { continue; }
    // TWO BASES, and conflating them is a silent desync (§4.1's rule applied
    // to an RNG key rather than to the hash): `niSlot` is the SLOT cell index
    // and is what the consumption roll below hashes on, so the RNG stream is a
    // property of WHERE the cell is in the world and not of which page happens
    // to hold it. `ni` is the physical word index and is only ever a memory
    // address. Feeding a page index into hash3 would make the sim's random
    // stream depend on allocation history — the world would still be
    // self-consistent and would still diverge from a dense run.
    let niSlot = cellIndexW(n);
    let niw = voxIndexAndWord(n);  // one table resolution for index + word
    let ni = niw.x;
    let nw = niw.y;
    let nmat = voxMat(nw);
    if (nmat == MAT_AIR) { continue; }

    // THE DECISION IS common.wgsl's stainStep — the same function the MPM
    // seam's stainApply calls, so the two stainers cannot drift again. What
    // it decides (see its header): which surfaces take a stain (solids and
    // powders, never liquids or gases), the strict order that terminates
    // (a foreign stain is only ever WASHED down, by a washer; our own climbs
    // to min(amount, max(capacity, 1))), the depth step (one level per
    // contact on absorbent ground, the whole ceiling at once on stone), and
    // whether the contact must be PAID for. What stays here is this kernel's
    // own half: the roll, the write, the spend in fullness eighths, and the
    // consumption of the marked voxel.
    let d = stainStep(stainType, addAmt, washes, nw, materials[nmat]);
    if ((d.y & STAIN_WORK) == 0u) { continue; }
    // FROZEN GROUND DOES NOT SOAK (the temperature layer). A liquid absorbing
    // into ground below water's freezing point is skipped -- that neighbour is
    // inert for this rule, like stone. Without this a tundra tarn, born with
    // ice on its top cell, drank ~3 cells of depth into its mud bed in its
    // first hundred ticks; the air that opened under the lid was a fresh
    // surface, the CA froze it, and the lake stacked lids (1,538 freezes in a
    // fresh tundra window, heat-ambient part D). Read at distance 1 (the
    // ground cell), off the field last tick's heatRelax left: deterministic.
    if ((d.y & STAIN_SPEND) != 0u && selfIsLiquid && heatGroundFrozen(n)) { continue; }
    progress = true;
    if (!fires) { break; }  // work remains, but not this tick

    voxStore(ni, d.x);
    markVoxActive(ni);
    markDirtyR(n, DIRTY_R_STAINW);
    markDirtyR(c, DIRTY_R_STAINW);
    // ---- absorption: the liquid SPENDS itself soaking in ----
    // Only when the ground actually declared a capacity and the contact
    // deepened the stain (STAIN_SPEND), and only for a liquid that has mass to
    // give. One eighth per contact, and the cell dies when it gives its last —
    // mass-conserving in the same units stepLiquid speaks. On absorbent ground
    // the stain climbs one level per contact in lockstep with this eighth, so
    // the ground visibly darkens as it drinks and the depth reached is paid
    // for in real mass.
    //
    // This writes SELF, which the stain rule otherwise never does. It is safe
    // for the same reason the neighbour write is: reach is still <= 1 cell, so
    // the colour lattice still guarantees no other thread touches either cell
    // this pass. The stamp is set so the movement code below cannot ALSO move
    // this cell in the same substep and double-spend the eighth.
    if ((d.y & STAIN_SPEND) != 0u && selfIsLiquid) {
      let sf = voxState(selfWord) + 1u;  // fullness 1..8
      if (sf <= 1u) {
        voxStore(idx, 0u);               // last eighth soaked in — gone
      } else {
        voxStore(idx, packVoxKeepStain(selfMat, sf - 2u, stamp, selfWord));
      }
      markVoxActive(idx);
    }

    // ---- washing: a rinse eats nothing ----
    // (It is paid for above like any replacement: water rinsing blood out and
    // blood re-staining the rinsed cell is a cycle only mass can end.)
    if ((d.y & STAIN_WASH) != 0u) { break; }

    // Consumption: the stain eats the voxel it just marked. Rolled from a
    // DIFFERENT slice of the hash than the stain roll, so the two are
    // independent — reusing the same bits would correlate "stained" with
    // "consumed" and every stain would either always or never eat.
    if (stainConsumes(m, hash3(rnd, 0x51A17u, niSlot))) {
      voxStore(ni, 0u);
      markVoxActive(ni);
      // The voxel that vanished may have been holding a solid up.
      flagSupportLoss(n, materials[nmat].klass, MAT_AIR);
    }
    break;  // one neighbour per tick — bounds the rule's rate (rule 2)
  }
  return progress;
}

// ============================== LIQUID DESCENT ===============================
// PLAN_fluid_overhaul.md §1.1 defect 3, and the single largest of the four.
//
// A liquid's descent set is the 9 cells of the layer below that the colour
// lattice's 1-cell write reach allows: straight down, the 4 axis diagonals and
// the 4 CORNERS. Before this the CA used 5 of those 9, and used them only as
// WHOLE-CELL moves, which meant `canDisplace` refused any target already
// holding the same liquid (equal density is not "lighter"). A partly filled
// lower step was therefore an impassable wall to the water standing above it.
//
// DETERMINISM (rule 1). Corners cost nothing new: two acting cells in one pass
// are >= 3 apart on EVERY axis, so their 3x3x3 write neighbourhoods are
// disjoint whether a move is axis-aligned or not. Every read below is a
// neighbour at Chebyshev distance 1, which is inside the guarantee (a read at
// distance 2 is NOT — see the column-walk note on seesSky).
//
// TERMINATION (rule 2). Every descent moves >= 1 eighth exactly one level down,
// so it strictly decreases the world's gravitational potential SUM(f * y),
// which is a bounded integer. Descents alone can therefore never keep a chunk
// awake forever; only the lateral rules need their own argument.

// The four CORNER lateral directions — the diagonal complement of lateralDir().
fn cornerDir(i : u32) -> vec2<i32> {
  switch (i & 3u) {
    case 0u: { return vec2<i32>( 1,  1); }
    case 1u: { return vec2<i32>(-1,  1); }
    case 2u: { return vec2<i32>(-1, -1); }
    default: { return vec2<i32>( 1, -1); }
  }
}

// Is `c` a WALL as far as a liquid is concerned? Solids and powders are; air,
// gases and other liquids are not; unloaded space outside the residency window
// is (DESIGN.md §3 — the sim's world edge is the window, and it is solid).
fn liquidWall(c : vec3<i32>) -> bool {
  if (!inBounds(c)) { return true; }
  let wm = voxMat(voxWordAt(c));
  if (wm == MAT_AIR) { return false; }
  let k = materials[wm].klass;
  return k == CLASS_SOLID || k == CLASS_POWDER;
}

// A corner descent must not squeeze through a SEALED DIAGONAL CRACK. Two walls
// that meet at a corner leave a diagonal seam with no volume; letting water
// through it would drain any box whose walls join, which is most of them. So a
// corner counts as a path only when at least one of the two axis descents that
// flank it is itself passable — the water goes AROUND the corner, never
// through it.
fn cornerDescentOpen(c : vec3<i32>, d : vec2<i32>) -> bool {
  return !liquidWall(c + vec3<i32>(d.x, -1, 0)) ||
         !liquidWall(c + vec3<i32>(0, -1, d.y));
}

// Move mass from `c` into `n`, one level below it. A same-liquid target takes a
// PARTIAL transfer — exactly what stage 1 always did for the cell directly
// below — and everything else goes through the whole-cell tryMove. Returns
// whether anything moved. Mass-exact in both arms (transferLiquid and tryMove
// are the only writers).
fn tryDescend(c : vec3<i32>, n : vec3<i32>, w : u32, mat : u32, f : u32,
              dens : i32) -> bool {
  if (!inBounds(n)) { return false; }
  let nw = voxWordAt(n);
  if (voxMat(nw) == mat) {
    let nf = voxState(nw) + 1u;
    if (nf >= 8u) { return false; }
    transferLiquid(c, n, mat, f, nf, min(f, 8u - nf));
    return true;
  }
  return tryMove(c, n, w, dens, false);
}

// Read-only mirror of tryDescend, for canFlowAnywhere. These two MUST agree:
// a predicate broader than the rule pins chunks awake forever (rule 2), one
// narrower lets a cell sleep with work left.
fn canDescend(c : vec3<i32>, n : vec3<i32>, mat : u32, dens : i32) -> bool {
  if (!inBounds(n)) { return false; }
  let nw = voxWordAt(n);
  if (voxMat(nw) == mat) { return voxState(nw) + 1u < 8u; }
  return canDisplace(dens, false, nw);
}

// ---- the last eighth: PLAN §1.1 defect 1, and its termination argument ------
//
// Lateral spread into air is repeated halving (8 -> 4 -> 2 -> 1) and stopped
// dead at `f >= 2`, so every blob decayed into fullness-1 films that could
// never move again. On the hill's stepped ramp that is fatal: a 2-cell tread's
// INNER cell has terrain below it and terrain on both down-diagonals, so its
// only exit is one lateral step to the tread's lip — which the halving rule
// refuses to make once the cell is down to its last eighth.
//
// WHY THIS IS NOT SIMPLY "LET FULLNESS 1 SPREAD". A lone eighth allowed to
// move sideways into air on FLAT ground is an unbounded random walk: it never
// finds a resting state, its chunk never sleeps, and rule 2 is gone. There is
// no reach-1 rule that can tell "one cell from the lip of a tread" from "one
// cell from the middle of a floor" — that information is two cells away, and a
// two-cell read is scheduling-dependent (the seesSky note). So the move has to
// be justified by something the cell can actually see.
//
// WHAT IT CAN SEE: the STEP BEHIND IT. The condition is
//
//     the cell opposite the move is a wall, and the cell above THAT is not
//
// i.e. "I am standing at the foot of a riser exactly one voxel proud of my own
// level" — which is precisely a terrace tread, and precisely where the water
// wants to go on. The move is away from the riser, which on a terrace is
// downhill.
//
// TERMINATION. After the step, the cell behind the film is the one it just
// vacated: AIR, not a wall, so the same move cannot repeat. On flat open ground
// there is no riser and the film never moves at all. Films therefore make at
// most one step per riser they are standing against, and the world still
// settles.
//
// THE ONE GEOMETRY THIS DOES NOT TERMINATE ON, stated because it is the honest
// limit of the rule: two risers facing each other exactly 3 apart (walls, two
// air cells between, both risers exactly one voxel tall) is symmetric, so a
// film in it steps back and forth forever. No reach-1 predicate can break that
// tie — the two cells have byte-identical neighbourhoods — and the `sleep` gate
// is the arbiter for whether the generated world contains it. The "one voxel
// tall" clause is what keeps the far more common case (a 2-wide slot between
// two ordinary walls) out of the rule entirely.
//
// THE `sleep` GATE FOUND ONE (2026-09-08, the authored home_lake: 8 chunks
// awake forever, `MOVE 8 film 8`, 21 of 27 changed words back where they
// started). The termination argument above is therefore FALSE as written, and
// what repairs it is not a better predicate — see the FILM_LICENCE block near
// the top of this file for why no reach-1 predicate exists — but a licence:
// this rule may only fire in a chunk where something that DOES decrease a
// Lyapunov function happened last tick, so it can no longer be its own cause.
// ---- the MINIMUM FILM, and why the halving needed a floor -------------------
//
// The rule above says a lone eighth may not wander. This one says how the
// halving is allowed to GET to a lone eighth, and the answer used to be "all
// the way": `f >= 2` let any cell split f/2 into an empty neighbour with no
// floor at all, so one placed water voxel ran 8 -> (4,4) -> (2,2,2,2) ->
// (1,1,1,1,1,1,1,1) and came to rest as EIGHT cells one eighth deep. The mass
// is exactly right and the shape is absurd: eight times the footprint the
// player placed, an eighth as tall, and since b799a58 draws liquid at
// fullness-proportional height that reads on screen as a splash rather than a
// voxel of water. It is also the worst possible input to the MPM side — a
// 1-eighth film gathers rho far below rest inside the solver's 3-cell support,
// so its EOS pressure is zero and excite converts it into particles that
// cannot move (the thin-film gap, RESEARCH_water_architecture.md).
//
// The floor is stated on BOTH halves, not just the source: a split is allowed
// only when `f >= 2 * minFilm`, so the cell keeps `f - f/2 >= minFilm` and the
// neighbour receives `f/2 >= minFilm`. That makes the equilibrium film
// minFilm..2*minFilm-1 eighths and shrinks a placement's footprint by minFilm.
//
// WHAT IT DOES NOT TOUCH, deliberately: the same-liquid EQUALIZE branch. That
// one moves mass between two cells that already hold water, which is what
// levels a pond, and gating it on a film thickness would leave real pools
// permanently stepped. Only the leading edge advancing into AIR is floored —
// and descent (stages 1 and 2) is untouched too, so water still runs downhill
// at any fullness and a spill on a slope behaves exactly as before.
//
// minFilm 1 reproduces the old `f >= 2` bit-for-bit, which is what makes this
// an A/B rather than a one-way change.
//
// ---- AND THE OWNER PUT IT BACK TO 1 (2026-08-25) ----------------------------
// The paragraph above is still an accurate description of what the floor does;
// the taste call it rests on was overruled, and the reasoning is worth keeping
// because the two halves of this file now pull in opposite directions.
//
// The ask: "if I use the smallest brush and place water on a flat plane I want
// it to keep spreading until every single voxel is the smallest height
// possible." That is minFilm 1 by definition — the flattest state a lattice
// quantised in eighths can represent is every wetted cell holding exactly one
// eighth. At minFilm 2 the same water rests two to three eighths deep over half
// the footprint, which is a lower, wider version of the same mound.
//
// It is a knob, not a decision: minFilm 2 is one edit away and everything below
// (filmPressed included) is written against LIQ_SPLIT_MIN rather than against a
// literal, so both settings behave consistently. What DOES change with it is the
// thin-film handoff to the solver — a 1-eighth film gathers rho far below rest
// inside the MPM's 3-cell support (RESEARCH_water_architecture.md), so the
// thinner the resting film, the more water the CA owns outright. That is the
// real cost of 1, and it is the reason the excite seam grew a surface-step
// trigger in the same change: disturbed water goes to the solver on purpose
// instead of being left to the CA by accident.
const LIQ_MIN_FILM : u32 = clamp(TUNE_LIQUID_MIN_FILM, 1u, 4u);
const LIQ_SPLIT_MIN : u32 = 2u * LIQ_MIN_FILM;

fn filmStepAllowed(c : vec3<i32>, d : vec2<i32>) -> bool {
  // The licence comes FIRST: it is a workgroup-uniform bool and the two loads
  // below are not. See the FILM_LICENCE block — the geometry test is unchanged
  // and still cannot tell a tread from a gutter; what changed is that a rule
  // neutral in both Lyapunov functions is no longer allowed to be its own cause.
  if (!gFilmLicence) { return false; }
  let back = c - vec3<i32>(d.x, 0, d.y);
  return liquidWall(back) && !liquidWall(back + vec3<i32>(0, 1, 0));
}

// ---- the PRESSED FILM: why a puddle went DOMED instead of flat --------------
//
// The rule above frees a film standing against a riser. This one frees the rim
// of a puddle, and it is what makes the resting shape actually LEVEL.
//
// THE DEFECT. Lateral spread into air is halving, and the same-liquid EQUALIZE
// branch only fires at a difference of TUNE_LIQUID_EQUALIZE (2). Put those
// together and a slope of exactly ONE eighth per cell is a STABLE state: no
// adjacent pair differs by 2 so nothing equalizes, and only the RIM touches air
// so nothing splits. A blob dropped on flat ground therefore relaxes into a
// CONE — 8 in the middle, 7 around that, ... 1 at the rim — and stops there
// forever. Dropped on a pond (where descent is refused because the water below
// is already full) that cone is a mound of water sitting proud of the surface
// that never disperses, which is exactly what the owner reported. It was
// invisible while liquid drew as full cubes and became obvious at b799a58,
// which draws a partial cell at fullness height.
//
// PLAN_fluid_overhaul.md §1.1 defect 2 names `liquidEqualize = 2` for this, and
// the long note in stepLiquid explains why lowering it to 1 cannot work: a
// difference of 1 transfers `(f - nf) / 2u` == 0 eighths, and forcing the odd
// eighth across instead is flat in the diffusion's own Lyapunov function, so the
// pair trades it back and forth forever. That analysis is right about the PAIR
// and wrong about the CHAIN — (8,7,6,5,4,3,2,1) has no unstable pair in it and
// is still a dome. The dome is not held up by the equalize threshold; it is held
// up by the RIM, which cannot move at all: a lone eighth may not split (that
// would leave nothing behind) and has no neighbour two lower to equalize with,
// so the footprint can never grow and the cone behind it has nowhere to go.
// Free the rim and the whole thing unwinds from the outside in.
//
// THE RULE. A cell too thin to split moves its WHOLE content one step into air
// when some OTHER lateral neighbour is thick enough to split — "there is water
// pressing behind me and there is room in front of me".
//
// TERMINATION (rule 2), and the gate is chosen for this and not for taste.
// SUM(f*f) is the lateral rules' Lyapunov function: splitting strictly
// decreases it (2 -> (1,1) is 4 -> 2), equalizing strictly decreases it, and
// descent strictly decreases SUM(f*y) instead. This move is NEUTRAL in both — it
// only relocates a film — so on its own it could cycle forever, which is the
// exact objection that kept the last eighth pinned in place.
//
// The gate is what forbids the cycle. The cell the film VACATES becomes air, and
// it is a face neighbour of the cell that justified the move, which by the gate
// holds >= LIQ_SPLIT_MIN. That cell can therefore split into the hole, and
// splitting strictly decreases SUM(f*f). So: SUM(f*f) is non-increasing and
// bounded below, hence eventually constant; over any stretch where it is
// constant no split and no equalize happens; but every advance in that stretch
// hands the neighbour behind it a split it can take. Contradiction — there are
// only finitely many advances. A puddle whose cells all hold the same amount has
// no cell at or over the split floor at all, so it makes no advance, finds
// nothing else to do, and sleeps. That level state is what this exists to reach.
//
// `nf >= LIQ_SPLIT_MIN` and not the more natural `nf > f` IS that argument: a
// neighbour merely thicker than the film can still be too thin to split into the
// hole the film leaves, and at minFilm > 1 that gap is where the proof (and the
// settling) breaks.
//
// AND THE PROOF IS STILL NOT ENOUGH, measured 2026-09-08 (FILM_LICENCE, near the
// top of this file). "That cell CAN therefore split into the hole" quantifies
// over an OPPORTUNITY. On a lake surface — films on every side of every hole —
// some other film advances into the hole first, the pressing cell keeps its two
// eighths, nothing splits, SUM(f*f) does not move and the configuration has
// merely rotated. Fourteen chunks of the authored home_lake did that forever.
// The licence at the top of this function is what closes it: no split anywhere
// in the chunk last tick means no advance this tick.
fn filmPressed(c : vec3<i32>, mat : u32) -> bool {
  // THE LICENCE FIRST, and the paragraph above is why it is needed: "that cell
  // CAN therefore split into the hole" is a statement about what is available,
  // not about what happens, and on an open water surface another film reaches
  // the hole first. Measured at the home_lake — 14 chunks awake forever, marked
  // `film-press`, 37 of 47 changed words back where they started, and not one
  // split in 20 ticks. See the FILM_LICENCE block: a move neutral in both
  // Lyapunov functions may not be its own cause.
  if (!gFilmLicence) { return false; }
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    if (voxMat(nw) != mat) { continue; }
    if (voxState(nw) + 1u >= LIQ_SPLIT_MIN) { return true; }
  }
  return false;
}

// ---- the BRIDGED EQUALIZE: reaching past a cell that cannot itself move -----
//
// filmPressed frees the rim; this is what drains the CORE behind it, and the two
// together are what actually lower a mound rather than merely widening it.
//
// WHAT IS LEFT AFTER filmPressed, measured on the ca-level gate: 216 eighths
// dropped on a flat floor rest over 113 cells with the profile
// 1:59 2:23 3:17 4:10 5:4 — an apron of single eighths around a core still five
// eighths deep. The apron is the trap. A cell holding 1 with 1s around it cannot
// split (nothing to halve), cannot equalize (its neighbours are equal) and is
// not pressed (no neighbour over the split floor), so once the frontier has run
// one cell ahead of the core the core is SEALED OFF from the only thing that was
// draining it. Every pairwise rule is blind here: adjacent cells differ by
// exactly one eighth all the way down the slope, which is the integer
// equilibrium of a PAIR and nothing like the equilibrium of a surface.
//
// THE MOVE. A cell may transfer between TWO OF ITS OWN LATERAL NEIGHBOURS. Both
// are one cell away, so the write reach is unchanged and the colour lattice's
// disjointness argument is untouched — this is the same licence tryMove has
// always used, spent on a pair of neighbours instead of on self and one
// neighbour. What it buys is a look at a distance the direct rules do not have:
// two cells that straddle a mediator are 2 apart (opposite laterals) or diagonal
// (perpendicular ones), and on a slope of one eighth per cell they differ by
// exactly 2 — which is the ordinary equalize threshold. The dome is therefore
// unstable again, from the inside.
//
// It is physically the right picture as well as a convenient one: the mediator
// is WATER. Pressure crosses a connected body of water; it does not have to be
// carried cell by cell. Requiring the middle cell to hold this same liquid is
// what keeps that true, and it also means the diagonal case needs no
// crack-check — the path between two perpendicular neighbours runs through the
// mediator, which is water by construction (compare cornerDescentOpen, which
// exists precisely because a corner descent has no such guarantee).
//
// TERMINATION (rule 2) IS FREE, which is the reason this rule is worth having
// and the diff-1 rules are not. It is an ordinary equalize — it moves
// (fa - fb) / 2 eighths from the fuller cell to the emptier one across a gap of
// at least TUNE_LIQUID_EQUALIZE — so it strictly decreases SUM(f*f) exactly as
// the direct branch does. No new Lyapunov argument, no new risk: the same
// bounded integer that already forbids the lateral rules from cycling forbids
// this one.
//
// THE REMAINING LIMIT, since this is the last rung reach-1 can climb. The joint
// fixpoint is now "no two cells within a mediated hop differ by 2", i.e. a
// surface may still slope by one eighth per TWO cells instead of per cell — half
// the dome, not no dome. Going further needs a look 4 cells wide, and a mediator
// can only bridge cells that are both inside ITS write reach, so 2 is the end of
// the line for this shape of rule. A genuinely level surface needs what a level
// surface physically is: a global pressure solve (the MPM owns that) or a
// mark/apply pass over a flux field (docs/RESEARCH_water_architecture.md option
// B). Both are architecture, not a rule tweak.
//
// `apply` rather than a mirrored read-only twin, deliberately: the settled path
// and the moving path MUST agree or a chunk either pins awake forever or sleeps
// with work left (see canFlowAnywhere), and the cheapest way to guarantee that
// is to have one function and one scan order. The scan is also NOT rng-rotated,
// unlike every other lateral scan here: the read-only mirror makes no roll, so
// rotating would let the two disagree about WHICH pair is available.
fn bridgeLevel(c : vec3<i32>, mat : u32, apply : bool) -> bool {
  // The four laterals' fullness, 0 meaning "not this liquid" (air, a wall,
  // another material, out of window). Gathered before any write, so the pair the
  // scan picks is a function of pre-move state.
  var fl = array<u32, 4>(0u, 0u, 0u, 0u);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    if (voxMat(nw) == mat) { fl[i] = voxState(nw) + 1u; }
  }
  for (var ia = 0u; ia < 4u; ia++) {
    if (fl[ia] == 0u) { continue; }
    for (var ib = 0u; ib < 4u; ib++) {
      if (ib == ia || fl[ib] == 0u) { continue; }
      if (fl[ib] + TUNE_LIQUID_EQUALIZE > fl[ia]) { continue; }
      if (!apply) { return true; }
      let da = lateralDir(ia);
      let db = lateralDir(ib);
      transferLiquid(c + vec3<i32>(da.x, 0, da.y), c + vec3<i32>(db.x, 0, db.y),
                     mat, fl[ia], fl[ib], (fl[ia] - fl[ib]) / 2u);
      return true;
    }
  }
  return false;
}

// Would stepLiquid() find anything to do for this cell? PURE READ — it makes
// no writes at all, and every read is a face/diagonal neighbour (reach 1), so
// it stays inside the colour lattice's guarantee exactly like the reaction
// neighbour scans do.
//
// One caller: a viscous liquid on an off-tick deciding whether it is worth
// staying awake for (the moveEvery gate in main). It used to be asked after a
// failed stepLiquid too, where it was provably false — see the liquid branch
// of main. The conditions below MIRROR stepLiquid's stages one for
// one; drift in the loose direction pins chunks awake forever (rule 2), drift
// in the tight direction lets a cell sleep with work left, so keep them in
// step. Same shape as the powder path's "nothing to do: cell settles".
fn canFlowAnywhere(c : vec3<i32>, w : u32, mat : u32, m : Material) -> bool {
  let f = voxState(w) + 1u;
  // Mirrors stepLiquid's hydrostatic gate one for one -- see the long block
  // there. Drift in the loose direction pins chunks awake forever, drift in the
  // tight direction lets a cell sleep with work left, and this predicate is
  // exactly where the never-sleeping pond was decided.
  let submerged = liquidSubmerged(c, mat);

  // 1) down: a partial same-liquid cell to top up, or anything displaceable.
  if (canDescend(c, c + vec3<i32>(0, -1, 0), mat, m.density)) { return true; }
  // 1b) brine above lighter liquid of its own kind (solTrySink, read only).
  if (solTrySink(c, w, mat, false)) { return true; }

  // 2a) the four axis down-diagonals.
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    if (canDescend(c, c + vec3<i32>(d.x, -1, d.y), mat, m.density)) { return true; }
  }
  // 2b) the four corner down-diagonals, path-gated exactly as stepLiquid does.
  for (var i = 0u; i < 4u; i++) {
    let d = cornerDir(i);
    if (!cornerDescentOpen(c, d)) { continue; }
    if (canDescend(c, c + vec3<i32>(d.x, -1, d.y), mat, m.density)) { return true; }
  }

  // 3) laterals: equalize into a same-liquid neighbour holding >= 2 less,
  //    split into air, step a film off a riser (only under FILM_LICENCE — and
  //    filmStepAllowed tests it, so this mirror inherits it for free, which is
  //    the whole reason the licence lives in the shared predicate) or out from
  //    under the water pressing on it, or displace something lighter.
  // Hoisted out of the direction loop: it does not depend on `d`, and it is only
  // ever asked of a cell too thin to split.
  let pressed = f < LIQ_SPLIT_MIN && filmPressed(c, mat);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    let nmat = voxMat(nw);
    if (nmat == mat) {
      if (submerged) { continue; }
      if (voxState(nw) + 1u + TUNE_LIQUID_EQUALIZE <= f) { return true; }
    } else if (nmat == MAT_AIR) {
      if (f >= LIQ_SPLIT_MIN) { return true; }
      if ((pressed || filmStepAllowed(c, d)) &&
          canDisplace(m.density, false, nw)) { return true; }
    } else if (gFilmLicence && materials[nmat].klass == CLASS_GAS &&
               canDisplace(m.density, false, nw)) {
      // Mirrors stepLiquid's licensed lateral displace (gas targets only). The two MUST agree —
      // loose here pins a chunk awake forever, tight lets a cell sleep with work
      // left — and the licence is a workgroup-uniform bool, so testing it first
      // keeps the non-uniform load off the common path exactly as the film
      // predicates do.
      return true;
    }
  }
  // 4) last resort: level two of the neighbours THROUGH this cell. Same scan and
  //    same predicate as the moving path — one function, so they cannot drift.
  return !submerged && bridgeLevel(c, mat, false);
}

// Mass-conserving liquid flow (fullness in eighths, DESIGN.md §4).
// Returns whether the cell moved any mass this substep; the caller uses that to
// decide whether a settled cell still has a reason to stay awake (defect 4).
fn stepLiquid(c : vec3<i32>, idx : u32, w : u32, mat : u32, m : Material, rnd : u32) -> bool {
  let f = voxState(w) + 1u;  // fullness 1..8
  // Diagnostic only (the DIRTY_M_* block above): tagged onto every successful
  // move so ONE run says which stage keeps a settled pond awake, and whether
  // the mover was under its own liquid at the time.
  // ---- THE HYDROSTATIC INVARIANT: A SUBMERGED CELL IS A FULL CELL ---------
  //
  // An eighth is a SURFACE thing. Anything with this same liquid standing on
  // top of it is full, and that is a statement about where lateral flow is
  // allowed to come FROM -- which is why it is also the reason a settled pond
  // could not sleep.
  //
  // MEASURED, by the DIRTY_M_* histogram this same block feeds: at the
  // authored home_lake the chunks that never slept were marked
  //   equalize 854 | bridge 912 | SUBMERGED 1180
  // i.e. the pond was levelling its own INTERIOR sideways, an eighth at a
  // time, forever. Both of those rules are same-liquid levelling -- they exist
  // to flatten a free surface -- and running them at depth is what created the
  // partial submerged cells that then gave them more work to do.
  //
  // WHAT CHANGES, and why each one keeps its termination argument:
  //   * EQUALIZE (stage 3, same liquid) and BRIDGE (stage 4) are refused for a
  //     submerged cell. Under the invariant they were unreachable anyway (two
  //     full cells cannot differ by TUNE_LIQUID_EQUALIZE); the gate is what
  //     stops a transient violation from feeding itself.
  //   * SPLIT into air (stage 3) HALVES, which leaves the source submerged and
  //     half empty -- the invariant broken by the very rule that drains a
  //     breached wall. A submerged cell gives EVERYTHING instead, so the source
  //     lands on AIR rather than on a partial cell. It is also the better
  //     picture: pressure pushes a whole cell of water out of the hole and the
  //     column above collapses into the gap, instead of the wall weeping.
  //
  // TERMINATION (rule 2) for the spill, which is the only genuinely new move.
  // It is NEUTRAL in both of this file's Lyapunov functions -- same y, and
  // (8,0) -> (0,8) has the same SUM(f*f) -- so it needs its own argument, and
  // it gets one free from the gate. The source is SUBMERGED by definition, so
  // once it empties there is liquid directly above an AIR cell, and stage 1
  // descent moves that liquid down: SUM(f*y) strictly decreases, and it is a
  // bounded integer. Every spill is therefore followed by a strict decrease, so
  // there can only be finitely many of them. A cell that is not submerged
  // cannot spill at all, which leaves the old halving (and its own proof) in
  // charge of every free-surface case.
  //
  // NOT CHANGED, because they already preserve the invariant by construction:
  // the film step and the displace-a-lighter-fluid branch are WHOLE-CELL moves,
  // so their source ends as air or as the other fluid, never as a partial cell
  // with water standing on it.
  let submerged = liquidSubmerged(c, mat);
  let sub = select(0u, DIRTY_M_SUBMERGED, submerged);

  // 1) straight down: top a partial same-liquid cell up, else move/swap whole.
  if (tryDescend(c, c + vec3<i32>(0, -1, 0), w, mat, f, m.density)) {
    markDirtyR(c, DIRTY_M_DOWN | sub);
    return true;
  }

  // 1b) BRINE SINKS (docs/PLAN_solutes.md §4.2): a full cell whose dissolved
  //     mass makes it denser than the full cell of the same liquid below it
  //     trades its SOLUTE with that cell. The voxels are the same liquid at the
  //     same fullness, so only the mass moves -- a halocline out of one
  //     comparison. Strictly decreases SUM(solute density * y) (a bounded
  //     integer), so it terminates on its own; not a film licence.
  if (solTrySink(c, w, mat, true)) { return true; }

  // 2a) the four AXIS down-diagonals, RNG order.
  let r = rnd >> 10u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryDescend(c, c + vec3<i32>(d.x, -1, d.y), w, mat, f, m.density)) {
      markDirtyR(c, DIRTY_M_DIAG | sub);
      return true;
    }
  }
  // 2b) the four CORNER down-diagonals, RNG order, after the axis ones (the
  //     shorter path wins) and only where the corner is a real path and not a
  //     sealed diagonal crack.
  for (var i = 0u; i < 4u; i++) {
    let d = cornerDir(i + r);
    if (!cornerDescentOpen(c, d)) { continue; }
    if (tryDescend(c, c + vec3<i32>(d.x, -1, d.y), w, mat, f, m.density)) {
      markDirtyR(c, DIRTY_M_DIAG | sub);
      return true;
    }
  }

  // 3) lateral: equalize into a same-liquid neighbor holding at least the
  //    equalize threshold less, split into air, step the LAST eighth off a
  //    riser (filmStepAllowed), or whole-cell displace a lighter fluid.
  //    RNG order.
  //
  //    On TUNE_LIQUID_EQUALIZE, which PLAN §1.1 names as defect 2: it stays 2,
  //    and the reason is worth stating because "just lower it to 1" is the
  //    obvious move and it does not work. `(f - nf) / 2u` is 0 when the
  //    difference is exactly 1, so a threshold of 1 makes a cell permanently
  //    "unstable", transfers nothing, and re-dirties its chunk forever. Forcing
  //    the odd eighth across instead turns (k+1, k) into (k, k+1), which has
  //    the SAME sum of squares — the diffusion's own Lyapunov function is flat
  //    on that move — so the pair trades it back and forth and never rests. The
  //    only tie-breaks available at reach 1 are position (a canonical direction
  //    ratchets mass toward +x/+z and makes an ASCENDING wedge a stable state,
  //    strictly worse) or the RNG (which oscillates). At eighth resolution
  //    (k, k+1) IS the integer equilibrium of two same-height cells, so there
  //    is nothing to fix there. The stable "1-eighth staircase" the plan is
  //    actually complaining about is between cells at DIFFERENT heights, and
  //    stage 2's new partial descent dissolves it outright: a cell one level
  //    down that is not full now receives, with no threshold at all, because
  //    moving mass downhill always strictly decreases SUM(f * y) and so can
  //    never oscillate.
  let r2 = rnd >> 14u;
  // See filmPressed: a film under pressure may advance even though it is too
  // thin to split. Hoisted for the same reason the mirror hoists it.
  let pressed = f < LIQ_SPLIT_MIN && filmPressed(c, mat);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r2);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt((n));
    let nmat = voxMat(nw);
    if (nmat == mat) {
      // Free-surface stage: a submerged cell does not level sideways.
      if (submerged) { continue; }
      let nf = voxState(nw) + 1u;
      if (nf + TUNE_LIQUID_EQUALIZE <= f) {
        transferLiquid(c, n, mat, f, nf, (f - nf) / 2u);
        markDirtyR(c, DIRTY_M_EQUAL | sub);
        return true;
      }
    } else if (nmat == MAT_AIR) {
      // THE SPILL, AND WHY IT IS NOT HERE.
      //
      // A submerged cell HALVING into a lateral void leaves the source
      // submerged and half empty — the invariant broken by the very rule that
      // drains a breached wall — so the obvious repair is to let a submerged
      // cell give its WHOLE content instead, landing the source on air rather
      // than on a partial cell. It was written, and it was MEASURED, and it
      // costs rule 2: `ca-level` (216 eighths dropped on flat ground) went from
      // "0 of 8 box chunks awake, quiet from tick 30" to "2 awake, quiet from
      // -1" — never quiet at all, on the flattest fixture there is.
      //
      // That is the predictable price of the only move in this file that is
      // NEUTRAL in BOTH Lyapunov functions: same y, and (8,0) -> (0,8) has the
      // same SUM(f*f). Its termination argument leaned on the descent that
      // refills the emptied source, and a descent that only USUALLY follows is
      // not a proof. A rule that cannot show it terminates does not belong in
      // the CA, however good the picture.
      //
      // So the halving below stays, and with it a TRANSIENT violation at a
      // breach: the source drops to f/2 with water still standing on it, and
      // the descent above refills it over the next few ticks. A settling
      // artefact at a hole is a different thing from the permanent state of
      // every pond INTERIOR, which is what the two gates above and below
      // actually remove.
      // Split only if BOTH halves clear the film floor — see LIQ_MIN_FILM.
      if (f >= LIQ_SPLIT_MIN) {
        transferLiquid(c, n, mat, f, 0u, f / 2u);
        markDirtyR(c, DIRTY_M_SPLIT | sub);
        return true;
      }
      // Too thin to split: the whole film steps. Two justifications, both
      // reach-1 and both with their own termination argument (see the blocks
      // on filmStepAllowed and filmPressed):
      //   * it is standing against a one-voxel riser — the terrace tread case,
      //     and the cell it vacates is air so the move cannot repeat UNLESS a
      //     second riser faces the first, which is why that branch now also
      //     needs the chunk to have done something else last tick
      //     (FILM_LICENCE); or
      //   * water thick enough to split is pressing on it from behind, so
      //     advancing hands that neighbour a split and the puddle levels.
      // The second is what dissolves a dome; without it the rim is frozen and
      // the cone behind it is a stable resting shape.
      // The two branches are marked SEPARATELY: they have different termination
      // arguments and only one of them was the shoreline that never slept, so a
      // histogram that folds them together cannot name the bug.
      let riser = filmStepAllowed(c, d);
      if ((pressed || riser) && tryMove(c, n, w, m.density, false)) {
        markDirtyR(c, select(0u, DIRTY_M_FILM, riser) |
                      select(0u, DIRTY_M_FILMPRESS, pressed) | sub);
        return true;
      }
    } else if (gFilmLicence && materials[nmat].klass == CLASS_GAS &&
               tryMove(c, n, w, m.density, false)) {
      // GAS TARGETS ONLY: A LIQUID NEVER SWAPS SIDEWAYS WITH ANOTHER LIQUID
      // (2026-09-24, `--gate oil-slick`). Water beside oil at the same level
      // traded whole cells with it, and a sideways trade has no direction to
      // it, so every oil cell took a random walk through the water's surface
      // layer: a disturbed slick diffused into a checkerboard of lone cells
      // (the owner's report; measured 16 pieces from one slab, 31% isolated).
      // Immiscible liquids separate VERTICALLY, and stages 1-2 already do that
      // (water descending through oil strictly lowers SUM(f*y)); a same-level
      // swap adds only mixing. A gas beside a liquid still yields, as air does.
      //
      // THE LATERAL DISPLACE IS A NEUTRAL MOVE AND NEEDS THE SAME LICENCE THE
      // FILM STEPS DO. See the FILM_LICENCE block: this swaps two whole fluid
      // cells at the SAME level, so SUM(f*y) is unchanged and SUM(f*f) is
      // unchanged, and a move neutral in both may not be its own cause. It was
      // ungated, and DIRTY_M_DISPLACE was in the licence set, so it was its own
      // cause twice over — `--gate pond-shore` pass C caught a water cell and a
      // steam cell trading places at a pond surface forever.
      //
      // NOT A DOWNWARD DISPLACE, which is the reason this costs nothing real:
      // stages 1 and 2 above go through tryDescend and mark DOWN / DIAG. Water
      // falling through smoke, oil rising through water, a liquid dropping into
      // a gas pocket — all of those strictly decrease SUM(f*y) and never reach
      // here. What reaches here is a same-level sideways swap, which has no
      // driving force behind it in the first place: a liquid at rest beside a
      // gas at the same height has no reason to trade with it, and now it only
      // does so while the chunk is genuinely doing something.
      //
      // The licence does NOT stop a real flow. While a pour, a breach or a
      // drain is running the chunk is full of descents and splits, so the
      // licence is granted every tick and this behaves exactly as before; what
      // it loses is the case where two settled fluids are the last thing awake.
      markDirtyR(c, DIRTY_M_DISPLACE | sub);
      return true;
    }
  }

  // 4) nothing this cell can do with its OWN mass — but it may still be the
  //    bridge two of its neighbours need. See the bridgeLevel block: this is
  //    what drains a mound whose rim has already run away from it, and it is an
  //    ordinary equalize (SUM(f*f) strictly down), just reaching one cell
  //    further.
  if (!submerged && bridgeLevel(c, mat, true)) {
    markDirtyR(c, DIRTY_M_BRIDGE | sub);
    return true;
  }
  return false;  // settled — the caller decides whether to stay awake
}

// ============================== WIND IN THE CA ==============================
// docs/RESEARCH_wind.md §4.5, phase 4. This is the only part of the wind system
// that writes voxels, and therefore the only part that can move the world hash.
// It is behind T.windMode, which ships at 0 (kWindMode* in world.h).
//
// TWO MECHANISMS, and the split is Bagnold's: matter already in motion is
// steered, matter at rest has to be picked up, and those need different
// thresholds. The hysteresis that separates them is not written anywhere — it
// falls out of the sleep machinery, because "already moving" and "settled" are
// states this kernel already distinguishes by which stage a cell reaches.
//
//   * DRIFT BIAS reorders the direction candidates a MOVING voxel was going to
//     try anyway. It cannot make a voxel move that would not have moved, it adds
//     no write, and its reach is still <= 1 cell — so it changes what the world
//     does without changing anything about how the world sleeps (invariant 3:
//     the ambient field never wakes a chunk).
//     Instantaneous advection, not acceleration, is the CORRECT model for a
//     gas: a parcel of smoke has no inertia worth modelling at this scale, it
//     goes where the air goes. The particle tier (sim_particle.wgsl) is where
//     momentum lives, and §4.6 is explicit that the two tiers divide that way.
//
//   * ENTRAINMENT unlocks a move for a SETTLED grain when the wind on one axis
//     beats that material's authored friction. That is saltation, and it is
//     what makes a fan blow a sand pile flat. It needs a LICENCE, because it is
//     the one mechanism here that is not rule-2 clean on its own and the one
//     rule in the engine that makes resting matter move — which is a property
//     the page table's materialization set quietly depends on NOT happening
//     (RESEARCH_wind.md §10). The licence is a wind primitive's declared,
//     budgeted, CPU-known footprint; the global WIND_MODE_ENTRAIN is the same
//     rule with the licence removed, and it ships off. See the call site.
//
// LIQUIDS ARE DELIBERATELY EXCLUDED from both. They move by mass transfer
// through stepLiquid, not by the direction rotation these hook into, and what
// wind does to standing water is drive surface waves and spray — a different
// phenomenon, owned by the MPM node force (sim_fluid.wgsl) where a wave can
// actually exist. Biasing eighths downwind would just make a pond flow uphill.

// Wind speed at which the drift bias reaches its cap, Q16.16 world cells/s
// (windAtQ's unit). Authored in m/s; converted at const-eval, so the kernel is
// integer (the sim_fluid.wgsl discipline).
// ---- SALTATION's two constants, which stay HERE ---------------------------
// windEntrain is the only reader and it is in this file: a constant only one
// shader reads is declared next to its consumer (CLAUDE.md), and adding it to
// common.wgsl would re-key every shader in the engine for nothing.
//
// Per-axis wind that just lifts a settled grain of windFriction 1, in the same
// units WIND_DRIFT_REF speaks — including the same `/ VOXEL_METERS`, for the
// same reason.
const WIND_ENTRAIN_REF : i32 = i32(round(
    clamp(TUNE_WIND_ENTRAIN_SPEED, 0.5, 200.0) * 65536.0 / VOXEL_METERS));
// ...and how often a grain over that threshold actually hops, in 1024ths per
// TICK (the entrainment stage runs on substep 0 only, so this is per tick with
// no substep division to keep in step). A rate, not a certainty: this is the
// bound that makes a dune creep instead of detonate (rule 2).
const WIND_ENTRAIN_CHANCE : i32 =
    i32(round(clamp(TUNE_WIND_ENTRAIN_RATE, 0.0, 30.0) * 1024.0 / 30.0));
// ...scaled by a POWER of the excess (2026-09-30, docs/RESEARCH_wind.md
// §13.5): chance = rate x min(excess / (threshold x span), 1)^power. Sand flux
// grows roughly with the cube of the excess over the threshold; a flat rate
// past a step made a wind 1% over the line move a dune as fast as a gale.
const WIND_ENTRAIN_POWER : i32 = clamp(i32(round(TUNE_WIND_ENTRAIN_POWER)), 1, 6);
const WIND_ENTRAIN_SPAN_Q10 : i32 =
    i32(round(clamp(TUNE_WIND_ENTRAIN_SPAN, 0.1, 10.0) * 1024.0));

// ---- THE GAS MOTION MODEL LIVES IN common.wgsl -----------------------------
// gasRndK / windLateralCode / windLateralStartK / windAxisFrac / gasLateralRot
// / GasIntent / gasIntentK / gasLadderStep, plus WIND_DRIFT_REF, WIND_DRIFT_CAP
// and WIND_RNG_SALT, moved there when the gas particle kernel landed: the CA
// moves a gas VOXEL and sim_gas.wgsl moves a gas PARCEL, and the plan's
// requirement is that they make the SAME move. See the block there.
//
// What stays here is the CA's call shape. The shared functions take the
// identity key and the substep as arguments because a parcel has neither a
// slot index nor a PassParams; these three wrappers supply the CA's, so every
// call site in this file reads exactly as it did before the move.
fn windRndS(slotIdx : u32, stream : u32) -> u32 {
  return gasRndK(slotIdx, stream, P.substep, &T);
}
fn windRnd(slotIdx : u32) -> u32 { return windRndS(slotIdx, 0u); }
// The wind at an ACTING cell: its 4^3 block's cached ambient (camask) plus
// the primitives, exact. `c` must be in a dirty chunk -- every caller passes
// the cell the colour thread is running, so it is.
fn caWindAt(c : vec3<i32>) -> vec3<i32> {
  if (T.windMode == WIND_MODE_OFF) { return vec3<i32>(0); }
  let lo = (c & vec3<i32>(CHUNK_MASK)) >> vec3<u32>(2u);
  let o = (voxSlotOfCell(c) * CA_WIND_BLOCKS + u32(lo.x + 4 * lo.y + 16 * lo.z)) * 3u;
  return vec3<i32>(caWind[o], caWind[o + 1u], caWind[o + 2u]) + windPrimAtQ(c, &T);
}
fn windLateralStart(c : vec3<i32>, base : u32, m : Material,
                    slotIdx : u32) -> u32 {
  if (T.windMode == WIND_MODE_OFF || matWindResponse(m) == 0u) { return base; }
  return windLateralStartW(caWindAt(c), base, m, slotIdx, P.substep, &T);
}
fn gasIntent(c : vec3<i32>, m : Material, slotIdx : u32, base : u32) -> GasIntent {
  if (T.windMode == WIND_MODE_OFF || matWindResponse(m) == 0u) {
    return gasIntentK(c, m, slotIdx, base, P.substep, &T);   // calm: no wind read
  }
  return gasIntentW(caWindAt(c), m, slotIdx, base, P.substep, &T);
}

// ---- THE WINDOW EDGE IS A SINK (docs/PLAN_gas_particles.md §2.3) -----------
//
// `tryMove` returns false out of window, and until now a gas voxel could not
// tell that refusal apart from a solid wall: it fell through to the lateral
// ring and SHEETED across the whole top chunk plane — up to 1,024 chunks held
// awake by smoke pressed against a lid that is not a lid, it is the edge of
// what happens to be resident. That sheet is the "chunk outline in the sky"
// and it is most of a big fire's CA cost.
//
// So the two refusals are split. A gas whose intent points OUT of the window
// leaves: the cell becomes air and a record goes to gasSpawn, which sim_gas
// turns into a gas PARTICLE later this same tick. Outside the window it keeps
// rising and drifting under the same model, bounded by its authored decay and
// an outer box, and it comes back as a voxel if it drifts back in.
//
// REACH 0. This writes ONE word — its own cell. The destination is outside the
// residency window and was never writable from here, so nothing about the
// colour lattice's write-disjointness argument changes (rule 1).
//
// BUDGET CHARGED BEFORE THE WRITE, the engine's standing convention: the slot
// is reserved first and the voxel is deleted only if it was granted. A refusal
// therefore leaves the voxel exactly where it was, taking today's ladder — the
// edge degrades into the old behaviour under load instead of losing mass.
//
// SPAWN-LIST ORDER IS SCHEDULING-DEPENDENT and nothing keys on it: each record
// is a complete particle state, and the particle system's rule is that
// behaviour is derived from state, never from a buffer slot.
// ---- THE IN-WINDOW SPLAT (stage 1b) ----------------------------------------
// Every gas voxel in an awake chunk adds ONE to the 0.8 m cell it sits in, so
// the coarse box holds the whole plume and not just the part of it that has
// already left. The renderer needs that overlap to crossfade: a voxel fading
// out at the window face has to be fading INTO something.
//
// IDENTICAL CELL MAPPING to sim_gas.wgsl's gasOuterOrigin/gasOuterCell and to
// raymarch.wgsl's gasOuterOriginVox — that agreement IS the interface, and the
// constants it is built from are pinned by check_invariants.py. T.origin is in
// CHUNK units; the box is two window edges centred on the window, so its min
// corner is half a window below the window's, which is a multiple of the cell
// size and needs no rounding.
//
// ONCE PER VOXEL PER TICK. The CA runs 2 gravity substeps x 27 colour phases;
// the stamp gate at the top of stepCell lets a voxel act at most once per
// SUBSTEP, so `P.substep == 0u` at the call site is what makes this once per
// tick. Called BEFORE the voxel moves, so a voxel that leaves the window this
// tick is splatted at the cell it left from and sim_gas splats the parcel it
// became at the cell it arrived at — one contribution each, from two
// populations that do not overlap.
//
// RENDER-ONLY: no voxel is written, nothing is hashed, and the load-then-add
// is a benign race for the same reason it is in sim_gas.wgsl. See
// GAS_OUTER_MAX there for what the guard buys.
//
// KNOWN LIMITATION, stated rather than solved: gas in a SLEEPING chunk is not
// visited by the CA and so is not splatted. The renderer's fade is gated on
// the coarse cell being non-empty for exactly that reason — a voxel never
// fades out into a cell with nothing in it — so a settled plume in a sleeping
// chunk keeps its full voxel opacity instead of vanishing.
fn gasOuterSplat(c : vec3<i32>) {
  let d = (c - (T.origin * i32(CHUNK) - vec3<i32>(i32(WORLD_N) / 2)))
          >> vec3<u32>(GAS_OUTER_SHIFT);
  let n = i32(GAS_OUTER_N);
  if (d.x < 0 || d.y < 0 || d.z < 0 || d.x >= n || d.y >= n || d.z >= n) { return; }
  let li = (u32(d.z) * GAS_OUTER_N + u32(d.y)) * GAS_OUTER_N + u32(d.x);
  let word = li >> 1u;
  let sh = 16u * (li & 1u);
  if (((atomicLoad(&gasOuter[word]) >> sh) & 0xFFFFu) >= GAS_OUTER_MAX) { return; }
  atomicAdd(&gasOuter[word], 1u << sh);
}

// THE CONVERSION ITSELF: append the record, delete the voxel. Called only from
// main's leave-resolve tail with a decision already made (THE EDGE'S BUDGET),
// so the slot check below is an INVARIANT, not a budget: the shares sum to at
// most GAS_SP_BUDGET <= GAS_SPAWN_CAP. If it ever fires the voxel stays put
// and GAS_SP_OVERRUN says the accounting is broken.
fn gasConvert(c : vec3<i32>, idx : u32, w : u32, dst : vec3<i32>) -> bool {
  let slot = atomicAdd(&gasSpawn[GAS_SP_COUNT], 1u);
  if (slot >= GAS_SPAWN_CAP) {
    atomicAdd(&gasSpawn[GAS_SP_OVERRUN], 1u);
    return false;
  }
  // Position is 24.8 with a ZERO FRACTION: a gas particle lives ON a cell and
  // moves in whole cells, exactly as the voxel did. Velocity words stay zero,
  // which is what keeps particlePriority a pure function of the visible state.
  let b = GAS_SP_HDR + slot * GAS_SP_STRIDE;
  atomicStore(&gasSpawn[b + 0u], bitcast<u32>(dst.x << 8u));
  atomicStore(&gasSpawn[b + 1u], bitcast<u32>(dst.y << 8u));
  atomicStore(&gasSpawn[b + 2u], bitcast<u32>(dst.z << 8u));
  atomicStore(&gasSpawn[b + 3u], 0u);
  atomicStore(&gasSpawn[b + 4u], 0u);
  atomicStore(&gasSpawn[b + 5u], 0u);
  // Material AND state nibble travel: a gas's nibble is its palette variant,
  // and dropping it would make every plume that leaves the window change shade
  // at the seam.
  atomicStore(&gasSpawn[b + 6u], voxMat(w) | (voxState(w) << 12u));
  atomicStore(&gasSpawn[b + 7u], PFLAG_ALIVE | PFLAG_GAS);
  voxStore(idx, 0u);
  markVoxActive(idx);
  markDirty(c);
  return true;
}

// The workgroup's leavers for THIS dispatch (THE EDGE'S BUDGET, step 3). An
// entry is main's gather entry (chunk-in-group << 12 | local index) with the
// primary direction coded into bits 16..20, plus the word the cell acted with.
// GAS_LV_MAX is structural: a leaver's step goes out of residency, so it sits
// on its chunk's boundary, and one colour has at most 152 boundary sites in a
// chunk (6^3 sites less the 4^3 interior ones) -- times CA_PACK_MAX chunks.
// The chunk-in-group index is ONE bit below (`& 1u`) and the per-chunk arrays
// are 2 long: both are CA_PACK_MAX, and a wider pack must widen them.
const_assert CA_PACK_MAX == 2u;
const_assert GAS_LV_MAX == 152u * CA_PACK_MAX;
var<workgroup> wgLvCell : array<u32, 304>;
var<workgroup> wgLvWord : array<u32, 304>;
var<workgroup> wgLvN : atomic<u32>;
var<workgroup> wgLvCnt : array<atomic<u32>, 2>;   // CA_PACK_MAX: leavers per chunk
var<workgroup> wgLvAllow : array<u32, 2>;         // CA_PACK_MAX: share left per chunk
// main's gather entry of the cell this thread is running (set per cell).
var<private> gCaEntry : u32 = 0u;

// Record this cell as a leaver and finish it for now; main decides after the
// barrier. False only if the list is full, which GAS_LV_MAX makes impossible.
fn gasLeaveDefer(w : u32, d : vec3<i32>) -> bool {
  let k = atomicAdd(&wgLvN, 1u);
  if (k >= GAS_LV_MAX) { return false; }
  let dc = u32((d.x + 1) + 3 * (d.y + 1) + 9 * (d.z + 1));
  wgLvCell[k] = (gCaEntry & 0xFFFFu) | (dc << 16u);
  wgLvWord[k] = w;
  atomicAdd(&wgLvCnt[(gCaEntry >> 12u) & 1u], 1u);
  return true;
}

// This chunk's share of the tick's budget that is still unspent, read ONCE per
// dispatch at the top of main (nothing else touches the chunk's word during
// the dispatch: a chunk is in exactly one workgroup).
fn gasLeaveAllow(ci : u32) -> u32 {
  let word = atomicLoad(&gasSpawn[GAS_SP_CHUNK0 + ci]);
  if ((word & GAS_LV_COUNTED) == 0u) { return 0u; }
  let share = atomicLoad(&gasSpawn[GAS_SP_BUDGET]) /
              max(atomicLoad(&gasSpawn[GAS_SP_EDGECH]), 1u);
  let used = word & ~GAS_LV_COUNTED;
  return share - min(used, share);
}

// The rank key: a hash of the cell, the tick and the substep, so which of a
// chunk's leavers lose when it is over its share is a pure function of the
// cells and is not always the same corner of the chunk.
fn gasLeaveKey(local : u32) -> u32 {
  return hash3(T.seed ^ GAS_LV_SALT, T.tick * 2u + P.substep, local);
}

// THE CHUNK BUDGET'S DOORBELL (camask, substep 0, one thread per dirty chunk):
// initialise the chunk's budget word and, if any of its 26 neighbours is not
// resident -- the only way a primary step (a unit step) can leave -- count it
// into GAS_SP_EDGECH. Every dirty chunk is initialised, edge or not, because
// the CA reads the word of every chunk it runs.
fn caChunkAtEdge(ci : u32) -> bool {
  let wc = slotWorldChunk(ci, T.origin);
  var edge = false;
  for (var dz = -1; dz <= 1; dz++) {
    for (var dy = -1; dy <= 1; dy++) {
      for (var dx = -1; dx <= 1; dx++) {
        if (!chunkResident(wc + vec3<i32>(dx, dy, dz), T.origin)) { edge = true; }
      }
    }
  }
  return edge;
}
fn gasLeaveCount(ci : u32) {
  let edge = caChunkAtEdge(ci);
  atomicStore(&gasSpawn[GAS_SP_CHUNK0 + ci], select(0u, GAS_LV_COUNTED, edge));
  if (edge) { atomicAdd(&gasSpawn[GAS_SP_EDGECH], 1u); }
}

// ---- THE GAS MOVEMENT TAIL -------------------------------------------------
// Everything a CLASS_GAS voxel does once reactions and staining are done with
// it. Returns true if the cell is finished (it moved, or it left the window).
//
// A gas that exhausts all fourteen candidates returns false and the caller
// returns anyway: stages 4 and 5 of the old chain (wandering powders and
// saltation) both test CLASS_POWDER, so a gas falling through them was always
// a no-op. Naming that here rather than letting it fall out is the difference
// between "gas is done" and "gas happens to do nothing next".
// ---- A HEAVY GAS SINKS AND CREEPS (materials.h kMatFlagHeavyGas) -----------
// Chlorine is 2.5x denser than air: released, it rolls DOWN and spreads along
// the ground, pooling in hollows, instead of rising like smoke. The CA's gas
// ladder is all buoyancy (every fallback past the flat ring goes UP), so a
// heavy gas takes its own short ladder: the primary is straight down half the
// time and a flat step the other half, then the four flat laterals, and
// NEVER an upward candidate -- a pooled cloud boxed in on all sides simply
// waits. Wind is ignored (it only ever lifts or leans a buoyant parcel).
//
// It moves only into AIR or a LIGHTER gas: a heavy gas does not sink into a
// pond or displace a solid. The pre-check is here because tryMove's density
// test is a buoyancy test (rising = "the target is denser than me"), which
// would let a gas moving DOWN swap under water. With the target vetted,
// tryMove is called with density -1 so its own test always passes.
//
// Rule 2: a pool of heavy gas on flat ground random-walks until its authored
// decay fades it (chlorine: reactions.json), so the chunk sleeps within that
// lifetime; the walk itself creates nothing. Rule 1: every roll is from
// `rnd`, the cell's own hash3 stream. MATF_HEAVY_GAS and the lift floor
// (1 m/s of heat updraft, Q16.16 cells/s) live in common.wgsl since sim_gas's
// parcels obey the same rule (gasHeavyStep).

fn heavyGasTarget(n : vec3<i32>, myDensity : i32) -> bool {
  if (!inBounds(n)) { return false; }
  let tm = voxMat(voxWordAt(n));
  if (tm == MAT_AIR) { return true; }
  let t = materials[tm];
  return t.klass == CLASS_GAS && t.density < myDensity;
}

fn stepHeavyGas(c : vec3<i32>, w : u32, m : Material, rnd : u32) -> bool {
  // HEAT LIFTS A HEAVY GAS (wind phase 5): a heavy gas never rises on its
  // own, but the updraft over a fire or a lava pool carries it -- hot choke
  // damp goes up the column like the smoke beside it. It reads the heat's
  // LIFT alone (common.wgsl windHeatUpQ), not the whole field: a storm's gust
  // bands have a vertical component of a few m/s, and a cellar of chlorine
  // must not boil out of its hollow in a gale. Only a lift past
  // HEAVY_LIFT_FLOOR counts, so faint warmth does not make it seep; the
  // chance ramps from 0 there to certainty a drift-saturation
  // (sim.windDriftSpeed) above it, scaled by the material's wind response.
  // One cell up into air or a lighter gas: reach 1, the ordinary tryMove.
  // Costs two shared loads per heavy-gas cell where no heat page exists.
  if (T.windMode != WIND_MODE_OFF && matWindResponse(m) != 0u) {
    let wy = windHeatUpQ(c, &T);
    if (wy > HEAVY_LIFT_FLOOR) {
      let p = min(windAxisFrac(wy - HEAVY_LIFT_FLOOR, i32(matWindResponse(m)), &T), 1024);
      let up = c + vec3<i32>(0, 1, 0);
      if (i32((rnd >> 22u) & 1023u) < p && heavyGasTarget(up, m.density) &&
          tryMove(c, up, w, -1, true)) {
        markDirtyR(c, DIRTY_M_GAS);
        return true;
      }
    }
  }
  let rot = rnd >> 12u;
  var cand : array<vec3<i32>, 5>;
  let lat0 = lateralDir(rot);
  if (((rnd >> 9u) & 1u) == 0u) {
    cand[0] = vec3<i32>(0, -1, 0);
  } else {
    cand[0] = vec3<i32>(lat0.x, 0, lat0.y);
  }
  for (var i = 1u; i < 5u; i++) {
    let d = lateralDir(rot + i);
    cand[i] = vec3<i32>(d.x, 0, d.y);
  }
  for (var i = 0u; i < 5u; i++) {
    let n = c + cand[i];
    if (!heavyGasTarget(n, m.density)) { continue; }
    if (tryMove(c, n, w, -1, true)) {
      markDirtyR(c, select(DIRTY_M_GASLAT, DIRTY_M_GAS, cand[i].y != 0));
      return true;
    }
  }
  return false;
}

fn stepGas(c : vec3<i32>, idx : u32, w : u32, m : Material, slotIdx : u32,
           rnd : u32) -> bool {
  if ((m.flags & MATF_HEAVY_GAS) != 0u) { return stepHeavyGas(c, w, m, rnd); }
  return stepGasFrom(c, idx, w, m, slotIdx, rnd, 0u);
}

// The buoyant ladder from candidate `i0` on. main's leave-resolve tail calls
// it with i0 = 1 for a leaver that was REFUSED: candidate 0 was the step out
// of the window, which wrote nothing, so resuming at 1 is exactly the voxel
// "falling through and behaving as it does at a wall".
fn stepGasFrom(c : vec3<i32>, idx : u32, w : u32, m : Material, slotIdx : u32,
               rnd : u32, i0 : u32) -> bool {
  let g = gasIntent(c, m, slotIdx, rnd >> 10u);

  // Indices 0..5 need no lateral rotation. Split from the loop below so the
  // common case — a plume with open sky above it, returning at index 0 —
  // never evaluates the wind field for a ring it does not reach.
  for (var i = i0; i < GAS_LADDER_RING; i++) {
    let s = gasLadderStep(g, 0u, 0u, i);
    if (s.w == 0) { continue; }
    let d = s.xyz;
    if (tryMove(c, c + d, w, m.density, true)) {
      // Height changed => progress (it will leave or decay); flat => the sheet.
      markDirtyR(c, select(DIRTY_M_GASLAT, DIRTY_M_GAS, d.y != 0));
      return true;
    }
    // Only the PRIMARY intent converts, and that is the whole rule: it is the
    // move the parcel actually wanted, it is what a plume at the top face
    // makes on every single tick, and confining the sink to it keeps a gas
    // that merely BRUSHES the edge on a fallback candidate inside the world.
    // The conversion is DECIDED LATER, by main, after every cell of this
    // colour in this workgroup has run (THE EDGE'S BUDGET).
    if (T.gasMode != GAS_MODE_OFF && i == 0u && !inBounds(c + d)) {
      atomicAdd(&gasSpawn[GAS_SP_EDGE], 1u);
      markDirtyR(c, DIRTY_M_GASEDGE);
      if (gasLeaveDefer(w, d)) { return true; }
      // Unreachable (GAS_LV_MAX is structural). Refuse in place, loudly.
      atomicAdd(&gasSpawn[GAS_SP_OVERRUN], 1u);
      atomicAdd(&gasSpawn[GAS_SP_REFUSED], 1u);
    }
  }

  let rUp  = windLateralStart(c, rnd >> 10u, m, slotIdx);
  let rLat = windLateralStart(c, rnd >> 14u, m, slotIdx);
  for (var i = GAS_LADDER_RING; i < GAS_LADDER_N; i++) {
    let s = gasLadderStep(g, rUp, rLat, i);
    let d = s.xyz;
    if (tryMove(c, c + d, w, m.density, true)) {
      markDirtyR(c, select(DIRTY_M_GASLAT, DIRTY_M_GAS, d.y != 0));
      return true;
    }
  }
  return false;
}

// Saltation: a settled grain of powder pulled loose by a wind that beats its
// authored friction. Returns true if it moved (the caller is then done with
// this cell, exactly as a successful tryMove leaves it).
//
// The threshold is PER AXIS, not on the magnitude, because that is what makes
// friction behave like friction: a wind blowing diagonally has to beat the
// threshold on the axis it is trying to push along, so a grain does not creep
// sideways off a component too weak to move it.
//
// Two candidates, in the order a real grain takes them: slide along the surface
// first, and only if that is blocked, hop UP and over the obstruction. Both are
// ordinary tryMoves at reach 1.
fn windEntrain(c : vec3<i32>, w32 : u32, m : Material, slotIdx : u32) -> bool {
  let resp = i32(matWindResponse(m));
  if (resp == 0) { return false; }
  let thresh = i32(matWindFriction(m)) * WIND_ENTRAIN_REF;
  // Raw field, NOT windAtScaledQ and not the gas multiplier either. Both dev
  // sliders scale a tier's RESPONSE to the wind; this test is a property of
  // the wind itself — whether it beats a material's authored friction — and
  // running a debug multiplier through a physical threshold would make the
  // slider silently retune every material's saltation point. The knob for
  // that is sim.windEntrainSpeed, which is what it is for.
  let wv = caWindAt(c);
  var d = vec2<i32>(0, 0);
  // Only the horizontal axes are tested. A vertical component lifts nothing on
  // its own — a grain needs somewhere lateral to go, and an updraft that
  // levitated powder in place would be a fountain, not saltation. It reaches
  // the grain anyway, through the up-diagonal candidate below.
  if (abs(wv.x) > thresh) { d.x = select(-1, 1, wv.x > 0); }
  if (abs(wv.z) > thresh) { d.y = select(-1, 1, wv.z > 0); }
  if (d.x == 0 && d.y == 0) { return false; }
  // The rate gate. Drawn AFTER the threshold test so a becalmed dune costs a
  // compare, and drawn at all so that a wind sitting just over the threshold
  // moves a surface slowly rather than all at once. The chance rises with a
  // power of the excess on the stronger axis, in Q10; a zero threshold
  // (friction 0) is always "far past" it.
  let ex = max(abs(wv.x), abs(wv.z)) - thresh;
  var x10 = 1024;
  if (thresh > 0) {
    x10 = min(((ex / max(thresh >> 10u, 1)) * 1024) / WIND_ENTRAIN_SPAN_Q10, 1024);
  }
  var pw = x10;
  for (var k = 1; k < WIND_ENTRAIN_POWER; k++) { pw = (pw * x10) >> 10u; }
  if (i32((windRnd(slotIdx) >> 10u) & 1023u) >= (WIND_ENTRAIN_CHANCE * pw) >> 10u) {
    return false;
  }
  // A GRAIN, not a cell (POWDER ENTERS THE WORLD AS GRAINS): a wind that
  // beats the friction lifts ONE EIGHTH off the surface into open air, and
  // the rest of the cell stays. Matter is conserved exactly, a dune smokes
  // grains instead of launching bricks, and each grain then falls and merges
  // like any other. Into anything but air (a lighter fluid) it is the old
  // whole-cell move. Mites are not grains and move whole.
  if (windGrain(c, c + vec3<i32>(d.x, 0, d.y), w32, m) ||
      tryMove(c, c + vec3<i32>(d.x, 0, d.y), w32, m.density, false)) {
    return true;
  }
  return windGrain(c, c + vec3<i32>(d.x, 1, d.y), w32, m) ||
         tryMove(c, c + vec3<i32>(d.x, 1, d.y), w32, m.density, false);
}

// One eighth from c into the AIR cell dst (reach 1). A one-eighth cell has
// nothing left to split and simply moves (tryMove), as before.
fn windGrain(c : vec3<i32>, dst : vec3<i32>, w : u32, m : Material) -> bool {
  if (!matHasPowderMass(m) || !inBounds(dst)) { return false; }
  let f = powderMass(w);
  if (f <= 1u) { return false; }
  let dv = voxIndexAndWord(dst);
  if (voxMat(dv.y) != MAT_AIR) { return false; }
  let myMat = voxMat(w);
  let stamp = stampFor(T.tick, P.substep);
  voxStore(dv.x, packVox(myMat, POWDER_GRAIN_STATE, stamp));
  var si = gSelfIdx;
  if (any(c != gSelfCell) || si == PT_NO_WORD) { si = voxWordIndex(c); }
  voxStore(si, packVoxKeepStain(myMat, powderStateFor(f - 1u, c, ptSeed()), stamp, w));
  markVoxActive(dv.x);
  markVoxActive(si);
  markDirtyR(c, DIRTY_R_MOVE);
  markDirtyR(dst, DIRTY_R_MOVE);
  return true;
}

// ---- THE STAMP GATE MUST NOT EAT A KEEP-AWAKE MARK (rule-unification W2-R) --
//
// main's substep gate skips a cell whose stamp equals stampFor(tick, substep).
// It means "this word was written earlier in THIS substep" (a mover landed here
// from an earlier colour pass, a product was written here), and for that it is
// exact. But the field is 3 bits cycling 1..7 (common.wgsl STAMP_CYCLE), so a
// cell that moved and then SITS STILL keeps a stale stamp that equals the
// current code once every 7 ticks: at substep 0, 7 ticks after a substep-0
// write, 4 after a substep-1 one. Movement loses nothing to that (the other
// substep of the same tick moves the cell). Reactions and staining do: they
// run on substep 0 only, and their keepAwake mark (DIRTY_R_REACT / _STAIN) is
// how "matched but did not fire" holds the chunk awake. On the alias tick the
// cell never got there, and if it was the only mark in its chunk the chunk
// slept with the reaction pending -- for good, since nothing else would wake
// it. `stamp-sleep` measured it: 7 of 8 acid voxels dropped onto iron slept on
// exactly the alias tick and never ate it; W2-J1 met it as 2 of 72 oil levels
// left unburnt beside live lava (its coatSpend STAMP_NEVER workaround).
//
// So a skipped cell, on substep 0, still runs its rules and its staining, with
// `probe` set (main's `skip`): every predicate, every keepAwake, no roll, no
// write, then it returns before the movement code. The only thing it can do
// is OR a reason bit into its OWN chunk. It goes through main's ONE call site
// of each evaluator rather than a helper of its own: a second call site
// inlined a second copy of doReactions into main and cost the forest fire
// 7% of CA time (27.2 -> 29.2 ms/frame, active chunks unchanged).
//   * Move once per substep / react once per tick: untouched. The cell still
//     returns; nothing fires twice.
//   * Genuinely-acted cells: every write that leaves a live stamp (tryMove,
//     transferLiquid, a product, absorption, coatRelease) already marks the
//     written cell's chunk for next tick, so the probe cannot change which
//     chunks are awake -- only add a reason bit to one that already is. Where
//     it changes membership is exactly the alias.
//   * Rule 1: reads at reach 1 (the same reads the unskipped evaluation makes,
//     in the same colour pass), writes nothing but an order-free atomicOr.
//   * Rule 2: marks only what the unaliased tick would have marked, so it
//     keeps nothing awake that the fix-free kernel would let sleep on the
//     other six ticks.
// Cost: a skipped cell on substep 0 pays its rule walk twice in the tick it
// arrived (once where it came from, once here); a settled cell 1 tick in 7.
// Measured on `--perf forestfire` (smoke-heavy, ~4,050 awake chunks, one run
// each side): CA 27.2 -> 27.9 ms/frame, frame p50 37.5 -> 38.0 ms, awake
// chunks +0.3%.

// ---- camask: the air mask for one dirty chunk (pass_table.def caMask) ------
// One workgroup per dirty chunk, recorded before EACH gravity substep's 27
// colours. Thread t reads cells t, t+256, ... COALESCED and ORs its bit into a
// workgroup word -- an OR is order-independent, so the mask is a pure function
// of the voxels (rule 1) whatever the schedule. A sentinel page is uniform:
// all air or all matter, no voxel read.
var<workgroup> wgCaMask : array<atomic<u32>, 384>;   // 3 planes x CA_MASK_WORDS
var<workgroup> wgCaEmit : atomic<u32>;
var<workgroup> wgCaCol : atomic<u32>;   // the 27-colour work word (CA_WORK_COL0)
// The repose snapshot's BLOCKERS (solid or powder) of this chunk, one bit per
// cell: substep 0 publishes ~this as the chunk's own reposeSnap record (see
// "THE CHUNK'S OWN REPOSE SNAPSHOT" below).
var<workgroup> wgCaBlk : array<atomic<u32>, 128>;
// The SPARSE colours (THE COLOUR WORK LISTS): this chunk's cell count per
// (colour, kind), the colours whose cells go to the pool, each such colour's
// reserved pool range (gas, rest) and the scatter cursors into it.
var<workgroup> wgColCnt : array<atomic<u32>, 81>;
var<workgroup> wgCaSparse : atomic<u32>;
var<workgroup> wgColBase : array<u32, 54>;
var<workgroup> wgColCur : array<atomic<u32>, 54>;
var<workgroup> wgCaEdge : u32;
var<workgroup> wgCaSpCnt : array<atomic<u32>, 2>;   // diagnostic: sparse gas / other cells
// The cost attribution (GAS_SP_GASONLY*): matter / gas cell counts of this chunk.
var<workgroup> wgCaPop : array<atomic<u32>, 2>;

@compute @workgroup_size(256)
fn camask(@builtin(workgroup_id) wg : vec3<u32>,
          @builtin(local_invocation_index) li : u32) {
  let ci = dirtyList[wg.x];
  let e = pageEntryOf(ci);
  // No early return for a sentinel: the barriers below need uniform control
  // flow, and a storage load is not uniform to the compiler.
  let sentinel = (e & PT_SENTINEL_BIT) != 0u;
  for (var k = li; k < 3u * CA_MASK_WORDS; k += 256u) { atomicStore(&wgCaMask[k], 0u); }
  if (li < CA_MASK_WORDS) { atomicStore(&wgCaBlk[li], 0u); }
  if (li < 81u) { atomicStore(&wgColCnt[li], 0u); }
  if (li < 54u) { atomicStore(&wgColCur[li], 0u); }
  if (li == 0u) {
    atomicStore(&wgCaEmit, 0u);
    atomicStore(&wgCaCol, 0u);
    atomicStore(&wgCaSparse, 0u);
    // A chunk a gas voxel can LEAVE the window from keeps its cells together
    // in one workgroup: the leave tail ranks a chunk's leavers against its
    // share inside the workgroup (THE EDGE'S BUDGET), so it never goes sparse.
    wgCaEdge = select(0u, 1u, T.gasMode != GAS_MODE_OFF && caChunkAtEdge(ci));
  }
  if (li < 2u) { atomicStore(&wgCaPop[li], 0u); atomicStore(&wgCaSpCnt[li], 0u); }
  workgroupBarrier();
  // THE COLOURS OF THIS THREAD'S CELLS. Thread li reads cells li + 256 m:
  // x = li & 15 and y = li >> 4 are fixed, z = m. A cell's colour is its WORLD
  // coordinate mod 3 per axis (the global lattice, caRow's `start`),
  // k = cx + 3 cy + 9 cz -- the passUBO slice order (simulation.cpp:
  // colorPhase = (k % 3, k / 3 % 3, k / 9)). So only cz varies down the loop.
  let wb = slotWorldChunk(ci, T.origin) * i32(CHUNK);
  let bm = ((wb % vec3<i32>(3)) + vec3<i32>(3)) % vec3<i32>(3);
  let cxy = u32((bm.x + i32(li & 15u)) % 3) + 3u * u32((bm.y + i32(li >> 4u)) % 3);
  let cz0 = u32(bm.z);
  if (!sentinel) {
    let base = e * CHUNK_VOL;
    // Per cz: gas | other << 8 | inert << 16 (at most 6 cells of one cz each).
    var cnt0 = 0u;
    var cnt1 = 0u;
    var cnt2 = 0u;
    var cz = cz0;
    for (var i = li; i < CHUNK_VOL; i += 256u) {
      let mat = voxMat(voxels[base + i]);
      if (mat != MAT_AIR) {
        let bit = 1u << (i & 31u);
        atomicOr(&wgCaMask[i >> 5u], bit);
        let m = materials[mat];
        if (((m._r2 >> HEAT_R2_EMIT_SHIFT) & 0xFFu) != 0u) { atomicOr(&wgCaEmit, 1u); }
        if ((m._r2 & ELEC_R2_SOURCE) != 0u) { atomicOr(&wgCaEmit, 2u); }
        if (m.klass == CLASS_SOLID || m.klass == CLASS_POWDER) {
          atomicOr(&wgCaBlk[i >> 5u], bit);
        }
        var inc = 1u << 8u;
        if (m.klass == CLASS_GAS) {
          atomicOr(&wgCaMask[CA_MASK_WORDS + (i >> 5u)], bit);
          inc = 1u;
        } else if (m.klass == CLASS_SOLID && !matCanAct(m)) {
          atomicOr(&wgCaMask[2u * CA_MASK_WORDS + (i >> 5u)], bit);
          inc = 1u << 16u;
        }
        if (cz == 0u) { cnt0 += inc; } else if (cz == 1u) { cnt1 += inc; } else { cnt2 += inc; }
      }
      cz = select(cz + 1u, 0u, cz == 2u);
    }
    for (var z = 0u; z < 3u; z++) {
      let c = select(select(cnt2, cnt1, z == 1u), cnt0, z == 0u);
      if (c == 0u) { continue; }
      let k3 = (cxy + 9u * z) * 3u;
      if ((c & 0xFFu) != 0u) { atomicAdd(&wgColCnt[k3], c & 0xFFu); }
      if (((c >> 8u) & 0xFFu) != 0u) { atomicAdd(&wgColCnt[k3 + 1u], (c >> 8u) & 0xFFu); }
      if ((c >> 16u) != 0u) { atomicAdd(&wgColCnt[k3 + 2u], c >> 16u); }
    }
  }
  workgroupBarrier();
  // DENSE OR SPARSE, per colour (one thread each). A colour with at most
  // CA_SPARSE_T cells goes to the pool when its cells may run apart from the
  // rest of the chunk: not a sentinel (every cell or none), not substep 0 of
  // an MPM chunk (air runs there too), not an edge chunk (above). Otherwise a
  // colour with any cell puts the chunk on that colour's dense list.
  if (li < 27u) {
    let g = atomicLoad(&wgColCnt[li * 3u]);
    let o = atomicLoad(&wgColCnt[li * 3u + 1u]);
    let r = o + atomicLoad(&wgColCnt[li * 3u + 2u]);
    let tot = g + r;
    if (tot != 0u) {
      let mpm = P.substep == 0u && fluidBlockMapS[ci] != 0u;
      var sparse = !sentinel && !mpm && wgCaEdge == 0u && tot <= CA_SPARSE_T;
      if (sparse) {
        // Reserve both kinds; if either does not fit, neither is used.
        var bg = 0u;
        var br = 0u;
        if (g != 0u) { bg = atomicAdd(&caArgs[CA_CUR_G + li], g); }
        if (r != 0u) { br = atomicAdd(&caArgs[CA_CUR_R + li], r); }
        let fits = (g == 0u || bg + g <= CA_POOL_CAP) && (r == 0u || br + r <= CA_POOL_CAP);
        if (fits) {
          wgColBase[li * 2u] = bg;
          wgColBase[li * 2u + 1u] = br;
        } else {
          // What did land inside a pool is counted by calist, so it must not
          // be left holding a stale entry from an earlier substep.
          let pg = CA_POOL0 + 2u * li * CA_POOL_CAP;
          if (g != 0u) {
            for (var j = bg; j < min(bg + g, CA_POOL_CAP); j++) { caMask[pg + j] = CA_POOL_SKIP; }
          }
          if (r != 0u) {
            for (var j = br; j < min(br + r, CA_POOL_CAP); j++) {
              caMask[pg + CA_POOL_CAP + j] = CA_POOL_SKIP;
            }
          }
          sparse = false;
        }
      }
      if (sparse) {
        atomicOr(&wgCaSparse, 1u << li);
        if (g != 0u) { atomicAdd(&wgCaSpCnt[0], g); }
        if (o != 0u) { atomicAdd(&wgCaSpCnt[1], o); }
      } else {
        atomicOr(&wgCaCol, 1u << li);
      }
    }
  }
  workgroupBarrier();
  // THE CHUNK'S COLOUR WORD (CA_WORK_COL0, read by `calist`): the colours
  // whose DENSE list the chunk goes on -- exactly those for which `main`'s
  // caRow would gather at least one cell of it and that did not go sparse,
  // so leaving it off colour k's list skips nothing:
  //   * a sentinel of air or of a material that cannot act: none (caRow
  //     returns before looking at the mask);
  //   * any other sentinel: all 27 (its mask is all ones);
  //   * substep 0 in a chunk with an MPM block: all 27 (caRow takes every
  //     site of the colour there, air included, for excitedReact);
  //   * otherwise the colours of the cells the matter plane holds, less the
  //     sparse ones.
  if (li == 0u) {
    var cols = atomicLoad(&wgCaCol);
    if (sentinel) {
      let smat = e & PT_MAT_MASK;
      cols = select(CA_ALL_COLOURS, 0u, smat == MAT_AIR || !matCanAct(materials[smat]));
    } else if (P.substep == 0u && fluidBlockMapS[ci] != 0u) {
      cols = CA_ALL_COLOURS;
    }
    caMask[CA_WORK_COL0 + wg.x] = cols;
    if (T.gasMode != GAS_MODE_OFF) {   // THE CA'S COST ATTRIBUTION, diagnostic
      let sg = atomicLoad(&wgCaSpCnt[0]);
      let so = atomicLoad(&wgCaSpCnt[1]);
      if (sg != 0u) { atomicAdd(&gasSpawn[GAS_SP_CA_GAS], sg); }
      if (so != 0u) { atomicAdd(&gasSpawn[GAS_SP_CA_OTHER], so); }
    }
  }
  // THE CHUNK'S OWN REPOSE SNAPSHOT (2026-10-03, ca-chunk-overhead). The
  // reposeSnap prepass fills, for every dirty chunk, the open bits of its
  // ring -- the chunk itself and nine neighbours -- and the chunk ITSELF is
  // always its own owner (ring offset 0), so it re-read all 4,096 voxels of
  // every dirty chunk that this pass has just read. Substep 0 publishes that
  // record here instead, from the same voxels (no row between the two writes
  // a voxel; reposeSnap now runs after caMask) with the same predicate,
  // reposeSnapOpenMat: open = not solid and not powder. reposesnap skips ring
  // member 0. Written whether or not the prepass is recorded this tick: a
  // valid snapshot nobody reads costs 512 bytes.
  if (P.substep == 0u && li < CA_MASK_WORDS) {
    var open = ~atomicLoad(&wgCaBlk[li]);
    if (sentinel) { open = select(0u, 0xFFFFFFFFu, reposeSnapOpenMat(e & PT_MAT_MASK)); }
    reposeSnap[ci * CA_MASK_WORDS + li] = open;
    if (li == 0u) { reposeSnap[REPOSE_SNAP_TICK_BASE + ci] = reposeSnapStamp(); }
  }
  // THE SCATTER: each sparse cell into the range its colour reserved in the
  // pool of its kind (gas, or the rest). Re-read from the mask planes
  // just built, not the voxels. The order within a range is the shared
  // cursors' arrival order -- scheduling-dependent and irrelevant, for the
  // colour lattice's reason (THE COLOUR ROWS below).
  let sparse = atomicLoad(&wgCaSparse);
  if (sparse != 0u && !sentinel) {
    var cz = cz0;
    for (var i = li; i < CHUNK_VOL; i += 256u) {
      let w5 = i >> 5u;
      let bit = 1u << (i & 31u);
      let k = cxy + 9u * cz;
      cz = select(cz + 1u, 0u, cz == 2u);
      if (((sparse >> k) & 1u) == 0u || (atomicLoad(&wgCaMask[w5]) & bit) == 0u) { continue; }
      let kind = select(1u, 0u, (atomicLoad(&wgCaMask[CA_MASK_WORDS + w5]) & bit) != 0u);
      let q = k * 2u + kind;
      caMask[CA_POOL0 + q * CA_POOL_CAP + wgColBase[q] + atomicAdd(&wgColCur[q], 1u)] =
          (ci << 13u) | i;
    }
  }
  // THE TEMPERATURE LAYER'S DOORBELL: this chunk holds something hot, so
  // heatWant (after the CA) pages it and its 3x3x3. Window slots only -- heat
  // does not run in a ticket. A sentinel chunk's one material is tested here.
  // THE EDGE'S BUDGET (gasLeaveCount): substep 0 only -- both substeps run
  // the same dirty list -- and before the CA's first colour by pass order.
  if (li == 0u && P.substep == 0u && T.gasMode != GAS_MODE_OFF) { gasLeaveCount(ci); }
  if (li == 0u && ci < NUM_CHUNKS) {
    // wgCaEmit: bit 0 = a heat emitter, bit 1 = an electric source.
    let emit = atomicLoad(&wgCaEmit);
    var hot = (emit & 1u) != 0u;
    var charged = (emit & 2u) != 0u;
    if (sentinel) {
      let sm = materials[e & PT_MAT_MASK];
      hot = ((sm._r2 >> HEAT_R2_EMIT_SHIFT) & 0xFFu) != 0u;
      charged = (sm._r2 & ELEC_R2_SOURCE) != 0u;
    }
    if (hot) { atomicOr(&heatMeta[HM_FLAGS + ci], HF_EMIT); }
    // THE CHARGE FIELD'S DOORBELL: a source here and no page yet -- elecAlloc
    // pages it after the CA (src/sim/elec.h). Window slots only.
    // EM_DOORBELLS counts the rings (wave 2): the CPU reads it off the
    // snapshot, and a ring it has not seen keeps the elec rows recording
    // (World::ElecMayBeLive) for a source that came from no op.
    if (charged && (atomicLoad(&elecMeta[EM_ENTRY + ci]) & ELEC_ENTRY_HAS) == 0u) {
      atomicOr(&elecMeta[EM_WANT + (ci >> 5u)], 1u << (ci & 31u));
      atomicAdd(&elecMeta[EM_DOORBELLS], 1u);
    }
  }
  for (var k = li; k < 3u * CA_MASK_WORDS; k += 256u) {
    var word = atomicLoad(&wgCaMask[k]);
    if (sentinel) {
      let smat = e & PT_MAT_MASK;
      let sm = materials[smat];
      var on = smat != MAT_AIR;
      if (k >= CA_MASK_WORDS) {
        on = on && select(sm.klass == CLASS_SOLID && !matCanAct(sm), sm.klass == CLASS_GAS,
                          k < 2u * CA_MASK_WORDS);
      }
      word = select(0u, 0xFFFFFFFFu, on);
    }
    caMask[ci * CA_MASK_STRIDE + k] = word;
    if (k < 2u * CA_MASK_WORDS && word != 0u) {
      atomicAdd(&wgCaPop[k / CA_MASK_WORDS], countOneBits(word));
    }
  }
  // THE CA'S COST ATTRIBUTION (GAS_SP_GASONLY*), substep 0: is this awake
  // chunk gas and nothing else, or nothing at all? Diagnostic only.
  workgroupBarrier();
  if (li == 0u && P.substep == 0u && T.gasMode != GAS_MODE_OFF) {
    let nm = atomicLoad(&wgCaPop[0]);
    let ng = atomicLoad(&wgCaPop[1]);
    if (nm == 0u) {
      atomicAdd(&gasSpawn[GAS_SP_GASONLY], 0x10000u);
    } else if (ng == nm) {
      atomicAdd(&gasSpawn[GAS_SP_GASONLY], 1u);
      atomicAdd(&gasSpawn[GAS_SP_GASONLY_CELLS], ng);
    }
  }
  // The wind cache (caWind above): one block per thread, the block's centre.
  // SUBSTEP 0 ONLY (2026-10-02, wind phase 5): the field is a function of the
  // tick (TickParams), the draft volume (built before the CA) and the heat
  // pool (written after it) -- nothing the CA's first substep changes -- and
  // both camask rows walk the same dirty list, so substep 1's cache would be
  // the same words written again. caMask1 is recorded at substep 1's pass
  // slice (pass_table.def DYN_CA1) to say which one it is. Halves the cache's
  // cost, which the heat term made heavier (village fire: caMask 1.09 ->
  // 1.36 ms with the heat lookups, both substeps).
  if (P.substep == 0u && T.windMode != WIND_MODE_OFF && li < CA_WIND_BLOCKS) {
    let b = vec3<i32>(i32(li & 3u), i32((li >> 2u) & 3u), i32(li >> 4u));
    let w = windAmbQ(slotWorldChunk(ci, T.origin) * i32(CHUNK) + b * 4 + vec3<i32>(2), &T);
    let o = (ci * CA_WIND_BLOCKS + li) * 3u;
    caWind[o] = w.x;
    caWind[o + 1u] = w.y;
    caWind[o + 2u] = w.z;
  }
}

// ---- calist: one chunk list per colour (pass_table.def caList) -------------
// (2026-10-03, ca-chunk-overhead.) Recorded after each caMask row, one
// workgroup per COLOUR. It turns camask's per-chunk colour words into colour
// k's list of chunks with work in k, its count, and the indirect args of
// colour k's dispatch: ceil(count / pack) workgroups, so no workgroup of a
// colour row starts past the end of its list either.
//
// WHY: `--perf village-fire` keeps ~300 awake chunks that hold NO matter
// (dirty-reasons' EMPTY fold: a neighbour's MOVE / gas write fanned out across
// the face, or the chunk's own last gas voxel leaving) and ~2,900 smoke chunks
// holding 11..65 cells, which have nothing at all in many of the 27 colours.
// Each such (chunk, colour) cost a full gather -- 36 mask rows, three
// barriers -- and the dispatch was always sized for the WHOLE dirty list
// (pack 2 left half the workgroups to return at once). The dirty flags are
// untouched: the page table's materialisation ring and the heat layer's mark
// licence read them, so a chunk is still awake; it is only not DISPATCHED for
// a colour it has nothing in.
//
// EXACT (rule 1): a chunk is left off colour k's list only when caRow would
// gather no cell of it for k (camask's colour word, above), and a workgroup's
// effect is a pure function of the cells it runs -- the colour lattice makes
// which workgroup runs a cell irrelevant, the same argument that lets the
// dirty list's own order be scheduling-dependent. The list here is not even
// that: a blocked prefix sum, in dirty-list order.
var<workgroup> wgCaScan : array<u32, 256>;

@compute @workgroup_size(256)
fn calist(@builtin(workgroup_id) wg : vec3<u32>,
          @builtin(local_invocation_index) li : u32) {
  let k = wg.x;   // the colour, 0..26
  let n = args[0];
  let per = (n + 255u) / 256u;
  let lo = min(li * per, n);
  let hi = min(lo + per, n);
  var cnt = 0u;
  for (var p = lo; p < hi; p++) { cnt += (caMask[CA_WORK_COL0 + p] >> k) & 1u; }
  // Inclusive scan of the 256 per-thread counts (Hillis-Steele).
  wgCaScan[li] = cnt;
  workgroupBarrier();
  for (var s = 1u; s < 256u; s = s << 1u) {
    var v = wgCaScan[li];
    if (li >= s) { v += wgCaScan[li - s]; }
    workgroupBarrier();
    wgCaScan[li] = v;
    workgroupBarrier();
  }
  let incl = wgCaScan[li];
  var at = CA_WORK_LIST0 + k * NUM_SLOTS + incl - cnt;
  for (var p = lo; p < hi; p++) {
    if (((caMask[CA_WORK_COL0 + p] >> k) & 1u) != 0u) {
      caMask[at] = dirtyList[p];
      at += 1u;
    }
  }
  if (li == 255u) {
    // The sparse cells camask scattered into this colour's pool, and the
    // cursors back to zero for the next substep's camask (nothing else reads
    // them: colour k's cursors are this workgroup's alone).
    // A cursor past CA_POOL_CAP is reservations that did not fit (camask
    // sent those cells dense); the pool holds the first CA_POOL_CAP.
    let sg = min(atomicLoad(&caArgs[CA_CUR_G + k]), CA_POOL_CAP);
    let sr = min(atomicLoad(&caArgs[CA_CUR_R + k]), CA_POOL_CAP);
    atomicStore(&caArgs[CA_CUR_G + k], 0u);
    atomicStore(&caArgs[CA_CUR_R + k], 0u);
    let pack = caPack(incl);
    let dwg = (incl + pack - 1u) / pack;
    caMask[CA_WORK_N0 + 4u * k] = incl;
    caMask[CA_WORK_N0 + 4u * k + 1u] = sg;
    caMask[CA_WORK_N0 + 4u * k + 2u] = sr;
    caMask[CA_WORK_N0 + 4u * k + 3u] = dwg;
    atomicStore(&caArgs[k * 4u], dwg + (sg + CA_WG - 1u) / CA_WG + (sr + CA_WG - 1u) / CA_WG);
    atomicStore(&caArgs[k * 4u + 1u], 1u);
    atomicStore(&caArgs[k * 4u + 2u], 1u);
    atomicStore(&caArgs[k * 4u + 3u], incl);
  }
}

// ---- THE COLOUR ROWS: GATHERED, SORTED CELLS, NOT ONE THREAD PER SITE -----
//
// (2026-10-01, the oil village fire: three houses burning, ~3,900 awake
// chunks of smoke, CA 19 ms a tick.) The colour rows used to launch one 6x6x6
// workgroup per dirty chunk per colour and give each thread one lattice site.
// A smoke chunk holds ~300 non-air cells, so a colour visited ~11 cells a
// chunk with 216 threads, and the few that worked were every kind at once --
// a gas, a burning plank, an inert stone -- so each warp ran stepGas, the
// reaction tail and the solid early-out one after another.
//
// Now a workgroup takes caPack(n) consecutive entries of the dirty list,
// GATHERS the sites of this colour that the air mask says hold matter, SORTS
// them by kind (gas | other | inert solid; camask's planes 1-2) and works
// through the list CA_WG at a time, so a warp runs one path. Measured on
// --perf village-fire, hash unmoved at every step: gather alone 0%, the sort
// -17% (13.2 -> 10.9 ms), then the pack factor: 1 chunk a group 12.2 ms,
// 2 10.45, 4 10.6, 8 10.9, 16 11.4. A fourth kind (liquids apart) measured
// no better on this scene. The same cells run, through the same caCell, with
// the same RNG keys; only which THREAD runs a cell changed, and the colour
// lattice is what makes that irrelevant (same-colour cells are >= 3 apart and
// write <= 1 away, so no two of them can touch the same word). The list order
// is scheduling-dependent (shared atomicAdds), which is just as irrelevant,
// for the same reason the dirty list's own order is (sim_compact.wgsl).
//
// Below CA_PACK_STEP listed chunks it is one chunk a workgroup, so a small
// world keeps its parallelism. Since 2026-10-03 the entries are those of THIS
// COLOUR's list (calist), not the dirty list, and the dispatch is sized to
// ceil(count / pack) -- no workgroup starts past the end.
const CA_WG : u32 = 128u;
const CA_PACK_MAX : u32 = 2u;
const CA_PACK_STEP : u32 = 512u;
fn caPack(n : u32) -> u32 { return clamp(n / CA_PACK_STEP, 1u, CA_PACK_MAX); }
// Where this dispatch's chunk list starts in caMask (CA_WORK_LIST0 + colour).
var<private> gCaList : u32 = 0u;
// One entry per gathered cell: slot << 13 | chunk-in-group (1 bit) << 12 |
// local index -- a pool entry has the same shape with chunk-in-group 0.
var<workgroup> wgCaList : array<u32, 432>;   // CA_PACK_MAX * 216
var<workgroup> wgCaCount : atomic<u32>;
// Per segment (gas, other, inert): the count, then the write cursor.
var<workgroup> wgCaSeg : array<atomic<u32>, 3>;

// One lattice row of one chunk of the group: its sites of this colour split
// into (gas, other, inert) bit sets, or zeros if the row is off the chunk.
struct CaRow { ci : u32, rowBase : u32, b : vec3<u32> }
fn caRow(first : u32, total : u32, t : u32) -> CaRow {
  var o : CaRow;
  o.b = vec3<u32>(0u);
  let j = t / 36u;
  let r = t % 36u;
  if (first + j >= total) { return o; }
  let ci = caMask[gCaList + first + j];
  o.ci = ci;
  let pe = pageEntryOf(ci);
  if ((pe & PT_SENTINEL_BIT) != 0u) {
    let smat = pe & PT_MAT_MASK;
    if (smat == MAT_AIR || !matCanAct(materials[smat])) { return o; }
  }
  let base = slotWorldChunk(ci, T.origin) * i32(CHUNK);
  // The color lattice is GLOBAL in WORLD coords: cell ≡ colorPhase (mod 3).
  // Coloring by slot coords would race at the toroidal wrap (world-adjacent
  // cells whose slots are WORLD_N apart would share a color); world coords
  // keep same-color cells >=3 apart in the space movement happens in.
  let bmod = ((base % vec3<i32>(3)) + vec3<i32>(3)) % vec3<i32>(3);
  let start = (vec3<i32>(P.colorPhase) + vec3<i32>(3) - bmod) % vec3<i32>(3);
  let y = start.y + 3 * i32(r % 6u);
  let z = start.z + 3 * i32(r / 6u);
  if (y >= i32(CHUNK) || z >= i32(CHUNK)) { return o; }
  let wi = ci * CA_MASK_STRIDE + u32(y >> 1) + 8u * u32(z);
  let sh = 16u * u32(y & 1);
  var bits = caRowBits(start.x);
  if (P.substep != 0u || fluidBlockMapS[ci] == 0u) {
    bits &= (caMask[wi] >> sh) & 0xFFFFu;
  }
  let gas = bits & (caMask[wi + CA_MASK_WORDS] >> sh);
  let inert = bits & ~gas & (caMask[wi + 2u * CA_MASK_WORDS] >> sh);
  o.b = vec3<u32>(gas, bits & ~gas & ~inert, inert);
  o.rowBase = (j << 12u) | (u32(y) * CHUNK + u32(z) * CHUNK * CHUNK);
  return o;
}

// x positions of one colour in a 16-wide row: bits sx, sx+3, ... (0x9249 = 0,3,..,15).
fn caRowBits(sx : i32) -> u32 { return (0x9249u << u32(sx)) & 0xFFFFu; }

@compute @workgroup_size(128)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  // This colour's work (calist): the workgroups below `dwg` take chunks of its
  // DENSE list, pack at a time, and gather; the ones above take 128 cells of
  // its sparse POOL (gas cells, then the rest). Same caCell either way.
  let kc = P.colorPhase.x + 3u * P.colorPhase.y + 9u * P.colorPhase.z;
  gCaList = CA_WORK_LIST0 + kc * NUM_SLOTS;
  let hdr = CA_WORK_N0 + 4u * kc;
  let total = caMask[hdr];
  let dwg = caMask[hdr + 3u];
  let dense = wg.x < dwg;
  let pack = caPack(total);
  let first = wg.x * pack;
  if (li < 3u) { atomicStore(&wgCaSeg[li], 0u); }
  // THE EDGE'S BUDGET: this dispatch's leaver list starts empty, and each
  // chunk's unspent share is read once, before any cell can spend it. (A
  // sparse workgroup holds no edge chunk's cells -- camask -- so has none.)
  if (T.gasMode != GAS_MODE_OFF) {
    if (li == 0u) { atomicStore(&wgLvN, 0u); }
    if (li < CA_PACK_MAX) {
      atomicStore(&wgLvCnt[li], 0u);
      var allow = 0u;
      if (dense && li < pack && first + li < total) {
        allow = gasLeaveAllow(caMask[gCaList + first + li]);
      }
      wgLvAllow[li] = allow;
    }
  }
  workgroupBarrier();
  // PASS 1: how many of each kind. The list is then laid out gas | other |
  // inert, so a warp runs ONE path (stepGas, the reaction/liquid/powder tail,
  // or the inert solid's early return) instead of all three in turn. Which
  // thread runs a cell cannot change what the cell does (see above), so the
  // sort is free of consequence -- only of cost.
  var rw : CaRow;
  rw.b = vec3<u32>(0u);
  if (dense && li < pack * 36u) { rw = caRow(first, total, li); }
  {
    if (rw.b.x != 0u) { atomicAdd(&wgCaSeg[0], countOneBits(rw.b.x)); }
    if (rw.b.y != 0u) { atomicAdd(&wgCaSeg[1], countOneBits(rw.b.y)); }
    if (rw.b.z != 0u) { atomicAdd(&wgCaSeg[2], countOneBits(rw.b.z)); }
  }
  workgroupBarrier();
  if (li == 0u) {
    let ng = atomicLoad(&wgCaSeg[0]);
    let no = atomicLoad(&wgCaSeg[1]);
    let ni = atomicLoad(&wgCaSeg[2]);
    atomicStore(&wgCaCount, ng + no + ni);
    if (T.gasMode != GAS_MODE_OFF) {   // THE CA'S COST ATTRIBUTION, diagnostic
      if (ng != 0u) { atomicAdd(&gasSpawn[GAS_SP_CA_GAS], ng); }
      if (no != 0u) { atomicAdd(&gasSpawn[GAS_SP_CA_OTHER], no); }
    }
    atomicStore(&wgCaSeg[0], 0u);
    atomicStore(&wgCaSeg[1], ng);
    atomicStore(&wgCaSeg[2], ng + no);
  }
  workgroupBarrier();

  // ---- PASS 2, GATHER: up to 36 lattice rows a chunk, one mask word each --
  // Row r of chunk j: y = sy + 3*(r % 6), z = sz + 3*(r / 6). A row past the
  // chunk is skipped. In a chunk with an MPM block on substep 0 every site of
  // the colour is taken, air included: excitedReact runs on air there.
  // A sentinel chunk whose material cannot act is skipped whole (caCell would
  // return on every one of its cells).
  {
    for (var k = 0u; k < 3u; k++) {
      var bits = rw.b[k];
      if (bits == 0u) { continue; }
      var at = atomicAdd(&wgCaSeg[k], countOneBits(bits));
      let rb = (rw.ci << 13u) | rw.rowBase;
      while (bits != 0u) {
        let x = firstTrailingBit(bits);
        bits &= bits - 1u;
        wgCaList[at] = rb | x;
        at += 1u;
      }
    }
  }
  // ---- ...OR TAKE 128 CELLS OF THE POOL (THE COLOUR WORK LISTS) -----------
  // Gas workgroups first, then the rest's, so a warp runs one kind. An entry
  // may be CA_POOL_SKIP (a reservation that overflowed the other pool, see
  // camask); the work loop passes over it.
  if (!dense) {
    let sw = wg.x - dwg;
    let sg = caMask[hdr + 1u];
    let gw = (sg + CA_WG - 1u) / CA_WG;
    var q = 2u * kc;
    var j0 = sw * CA_WG;
    var nk = sg;
    if (sw >= gw) {
      q += 1u;
      j0 = (sw - gw) * CA_WG;
      nk = caMask[hdr + 2u];
    }
    let cnt = min(CA_WG, nk - min(j0, nk));
    if (li < cnt) { wgCaList[li] = caMask[CA_POOL0 + q * CA_POOL_CAP + j0 + li]; }
    if (li == 0u) { atomicStore(&wgCaCount, cnt); }
  }
  workgroupBarrier();

  // ---- WORK: the gathered cells, CA_WG at a time --------------------------
  // An entry is (slot << 13) | (chunk-in-group << 12) | local; gCaEntry keeps
  // the low 13 bits, the shape the leave deferral records.
  let n = atomicLoad(&wgCaCount);
  for (var i = li; i < n; i += CA_WG) {
    let e = wgCaList[i];
    if (e == CA_POOL_SKIP) { continue; }
    let lm = e & 0xFFFu;
    gCaEntry = e & 0x1FFFu;
    caCell(e >> 13u, vec3<i32>(i32(lm & 15u), i32((lm >> 4u) & 15u), i32(lm >> 8u)));
  }

  // ---- THE LEAVE-RESOLVE TAIL (THE EDGE'S BUDGET, step 3) -----------------
  // Every cell of this colour in this workgroup has run; the ones whose
  // primary step left the residency window are waiting in wgLvCell. Each is
  // accepted if its chunk has share left for it -- all of them when the
  // chunk's leavers fit, otherwise the lowest `allow` by gasLeaveKey -- and
  // then either converts or resumes its ladder at candidate 1. T.gasMode is a
  // uniform, so the barrier is in uniform control flow.
  if (T.gasMode != GAS_MODE_OFF) {
    workgroupBarrier();
    let nl = min(atomicLoad(&wgLvN), GAS_LV_MAX);
    for (var k = li; k < nl; k += CA_WG) {
      let ent = wgLvCell[k];
      let j = (ent >> 12u) & 1u;
      let lm = ent & 0xFFFu;
      let allow = wgLvAllow[j];
      var ok = allow > 0u;
      if (ok && atomicLoad(&wgLvCnt[j]) > allow) {
        // Over the share: rank this leaver among its chunk's by (key, local).
        // Only on the overflow tick of an overflowing chunk; <= 152 entries.
        let key = gasLeaveKey(lm);
        var rank = 0u;
        for (var q = 0u; q < nl; q++) {
          let o = wgLvCell[q];
          if (((o >> 12u) & 1u) != j || q == k) { continue; }
          let olm = o & 0xFFFu;
          let okey = gasLeaveKey(olm);
          if (okey < key || (okey == key && olm < lm)) { rank++; }
        }
        ok = rank < allow;
      }
      let ci = caMask[gCaList + first + j];
      let c = slotWorldChunk(ci, T.origin) * i32(CHUNK) +
              vec3<i32>(i32(lm & 15u), i32((lm >> 4u) & 15u), i32(lm >> 8u));
      // caCell's per-cell context, exactly as it was when this cell deferred.
      gFilmLicence = (dirtyIn[ci] & FILM_LICENCE) != 0u;
      gInTicket = ci >= NUM_CHUNKS;
      gSelfCell = c;
      gSelfIdx = PT_NO_WORD;
      gCaEntry = ent & 0xFFFFu;
      let idx = voxWordIndex(c);
      let w = wgLvWord[k];
      let dc = i32(ent >> 16u);
      let d = vec3<i32>(dc % 3 - 1, (dc / 3) % 3 - 1, dc / 9 - 1);
      if (ok && gasConvert(c, idx, w, c + d)) {
        atomicAdd(&gasSpawn[GAS_SP_CHUNK0 + ci], 1u);
        continue;
      }
      // Refused (or the invariant broke, which gasConvert has counted): the
      // voxel stays and takes the rest of its ladder, as it would at a wall.
      if (!ok) { atomicAdd(&gasSpawn[GAS_SP_REFUSED], 1u); }
      let slotIdx = cellIndexW(c);
      stepGasFrom(c, idx, w, materials[voxMat(w)], slotIdx,
                  hash3(T.seed, T.tick * 2u + P.substep, slotIdx), 1u);
    }
  }
}

// ONE CELL of one colour (was main's body, one thread per lattice site).
// `ci` is the chunk's slot, `local` the cell within it.
fn caCell(ci : u32, local : vec3<i32>) {
  // ---- A SENTINEL CHUNK WHOSE MATERIAL CANNOT ACT IS A NO-OP ----
  // (main's gather already skips such a chunk; this is the per-cell guard.)
  //   * PT_EMPTY: every cell is air, and main returns on air.
  //   * UNIFORM / JITTER of a material with !matCanAct (a plain solid: no
  //     reaction bucket, no stain, CLASS_SOLID). Every cell is that solid for
  //     the whole dispatch — the table is read-only during it, and a store into
  //     a sentinel is a dropped fault, never a write — so every cell has an
  //     in-chunk face neighbour of the same solid, soloSolid() is false, and
  //     the `!matCanAct` return is the next thing it does. Nothing is written
  //     and nothing is marked.
  // What this does NOT skip, deliberately: a sentinel of any material that CAN
  // act (sand, water, grass, anything with a rule or a stain). Rules that a
  // NEIGHBOUR chunk's content triggers across the face (a PAIR, a stain, a
  // flow into this chunk) are run by the neighbour's cells.
  let pe = pageEntryOf(ci);
  if ((pe & PT_SENTINEL_BIT) != 0u) {
    let smat = pe & PT_MAT_MASK;
    if (smat == MAT_AIR || !matCanAct(materials[smat])) { return; }
  }

  // One scalar load, read only by the riser film step: did anything that is
  // not itself a film step happen in this chunk last tick? FILM_LICENCE block —
  // this is what stops a neutral rule from keeping a shoreline puddle awake
  // forever. Set PER CELL now (a thread runs cells of several chunks).
  gFilmLicence = (dirtyIn[ci] & FILM_LICENCE) != 0u;
  gInTicket = ci >= NUM_CHUNKS;
  let wc = slotWorldChunk(ci, T.origin);
  let base = wc * i32(CHUNK);  // world cell of the chunk corner (may be < 0)
  let c = base + local;  // world cell this thread acts on
  // The previous cell this thread ran must not leak its self-cell cache into
  // this one (tryMove re-resolves when src != gSelfCell or the index is unset).
  gSelfCell = c;
  gSelfIdx = PT_NO_WORD;
  gThinMul = 1u;

  // TWO BASES (§4.1). `slotIdx` is the SLOT cell index and keys the per-cell
  // RNG below; `idx` is the physical word index and is only a memory address.
  // Under the identity map they are equal, which is why commit 1 could
  // introduce the split while it cannot differ — but keying the RNG on a page
  // index would make every cell's random stream a function of allocation
  // history, and a paged run would diverge from a dense one with no other
  // symptom.
  let slotIdx = cellIndexW(c);
  let here = voxIndexAndWord(c);  // one table resolution for index + word
  let idx = here.x;
  let w = here.y;
  let mat = voxMat(w);
  if (mat == MAT_AIR) {
    // An air cell may hold EXCITED FLUID, which runs its own rules here
    // (excitedReact). Reactions roll on substep 0 only, as for voxels, and
    // the block-map load is workgroup-uniform (every thread is in chunk ci),
    // so a fluid-free chunk pays one broadcast load per air cell.
    if (P.substep == 0u && fluidBlockMapS[ci] != 0u) {
      excitedReact(c, idx, slotIdx);
    }
    return;
  }
  gSelfCell = c;
  gSelfIdx = idx;
  // Already acted this substep -- OR a stale stamp that ALIASES this one ("THE
  // STAMP GATE MUST NOT EAT A KEEP-AWAKE MARK" above): the gate cannot tell
  // which, so on substep 0 a skipped cell runs its rules as a PROBE (match,
  // mark, never fire) and returns after staining; on substep 1 it just returns.
  let skip = voxStamp(w) == stampFor(T.tick, P.substep);
  if (skip && P.substep != 0u) { return; }

  let m = materials[mat];
  // ---- THE SOLUTE LAYER'S TWO SELF-RULES (substep 0, before the authored
  // rules): a solute powder touching a solvent dissolves into it, and a solvent
  // concentrated past a `converts` row becomes that material. Ahead of the
  // reaction scan so the concentration-driven path owns these pairs; a cell
  // that dissolved or converted has acted this tick. Ahead of the stain-dry
  // step too: a grain resting in water is WET (stained), and stainDry returns
  // early for a wet cell beside water -- measured, a grain at rest on a pond
  // floor never dissolved at all.
  //
  // A STAMPED cell (skip) still PROBES: a resting grain's stamp matches the
  // tick's substep-0 stamp one tick in seven, and a tick on which nothing in
  // the chunk marks dirty is a tick the CPU can prove the world settled and
  // stop running the CA -- measured, a grain on a pond floor went to sleep
  // after 22 ticks and never dissolved. doReactions probes for the same reason.
  // Only a POWDER dissolves (solutes.cpp refuses any other `from`) and only a
  // LIQUID converts: gases and solids -- most of a burning forest's awake
  // cells -- skip the species-table read entirely (measured: +9% CA in the
  // --perf forestfire scene before this gate).
  if (P.substep == 0u && (m.klass == CLASS_POWDER || m.klass == CLASS_LIQUID)) {
    let sp = solFromSpecies(mat);
    if (sp != 0u && TUNE_SOLUTE_MODE != 0u &&
        solTryDissolve(c, idx, w, mat, m, sp, slotIdx, skip)) { return; }
    if (!skip && m.klass == CLASS_LIQUID && solTryConvert(c, idx, w, mat)) { return; }
  }

  // ---- A SOLID WITH NOTHING TOUCHING IT IS A ONE-VOXEL ISLAND -------------
  //
  // A CLASS_SOLID voxel never moves in the CA, so island detection is the only
  // thing that can make solid matter fall — and island detection is a CPU
  // machine: a GPU support-loss flag, a per-chunk cooldown, an async readback,
  // a queue drained a few entries a tick, a bounded region scan, a connectivity
  // flood, an anchor test. Every one of those is the right tool for deciding
  // whether a LEDGE is still attached to a cliff. All of it is absurd for a
  // single voxel, and measurably it does not arrive: burning one tree in the
  // `tree-fell` gate left 38 lone voxels hanging with every leak counter at
  // zero, `eventQueueFullSpilled` at 1113 and the sub-8 rubble handoff reached
  // 401 times against 11105 small components anchored at a scan-box boundary.
  // A firehose into a straw.
  //
  // But "is this a one-voxel island" needs no connectivity analysis at all. A
  // solid with no solid or powder among its six faces IS a component of one, by
  // definition, decided from information the cell already has in hand. So it
  // falls, here, like the powder it has become — no flag, no queue, no scan,
  // and nothing to congest. The expensive machine keeps the job it is good at.
  //
  // WITHIN THE LATTICE'S READ BOUND, which is the constraint that decides
  // whether this is legal at all. The 3x3x3 colour lattice keeps acting cells
  // >=3 apart and bounds WRITES to <=1 cell — it promises nothing about reads,
  // and a `seesSky` that walked 48 cells up once broke determinism at tick 1
  // for exactly that reason. This reads six cells at distance 1: an acting cell
  // may write at distance <=1 from itself, so a cell one step from me can only
  // be written by an acting cell within two steps of me, and I am the only one
  // there. No race, whatever the schedule.
  //
  // AN UNSEEN NEIGHBOUR COUNTS AS ATTACHED, the same conservative direction
  // RunIslandDetection's `solidOutside` takes at the residency edge: refuse to
  // move matter on a guess.
  if (!skip && m.klass == CLASS_SOLID &&
      soloSolid(c, (m.flags & MATF_PASSABLE) != 0u)) {
    // Straight down, and only down. Displacement rules still apply, so a chip
    // resting on lava it cannot sink into simply stays — which is support, and
    // reads as such. Failure does NOT markDirty: nothing here may keep a chunk
    // awake forever (CLAUDE.md rule 2), and any change around it re-dirties the
    // chunk through the ordinary paths.
    if (tryMove(c, c + vec3<i32>(0, -1, 0), w, m.density, false) ||
        tryCrush(c, c + vec3<i32>(0, -1, 0), w)) {
      markDirtyR(c, DIRTY_M_SOLO);
      return;
    }
  }

  // Provably inert cell: no reaction bucket, no staining, and a SOLID class, so
  // every branch below is skipped and nothing is written. Structurally a no-op
  // — the three tests it folds are each still there underneath — but it is the
  // SAME predicate worldgen's streaming wake refuses on (matCanAct, common.wgsl),
  // and having one function stand for "this cell cannot act" is what keeps the
  // two from drifting. If a future rule lets a plain solid do something, this
  // return is where it breaks first, loudly, in the world hash.
  //
  // ...EXCEPT a cell wearing a COAT (a stain on absorbent ground, coatMatOf):
  // the coat's own rules act from here even when the substrate has none (an
  // absorbent solid with an empty bucket). One stain-bits test for every cell
  // that reaches here; the palette load only for a stained absorbent one.
  let coat = coatMatOf(w, m);
  // A wet stain dries (stainDry), on any surface, absorbent or not -- so this
  // is tested before the inert return. One stain-bits test (already paid by
  // coatMatOf) for every cell; the palette load only for a stained one.
  if (P.substep == 0u && voxStained(w) &&
      stainDry(c, idx, w, m, hash3(T.seed, T.tick * 2u, slotIdx), skip)) {
    return;
  }
  // ...AND EXCEPT A CONDUCTOR (E2): an inert copper bar or iron plate holding
  // charge crackles (elecReact). One `_r2` bit test; the field is read on
  // substep 0 only, and only by a conductor.
  let conducts = (m._r2 & ELEC_R2_CONDUCTS) != 0u;
  if (!matCanAct(m) && coat == 0u && !conducts) { return; }
  let rnd = hash3(T.seed, T.tick * 2u + P.substep, slotIdx);

  // Reactions roll once per tick (substep 0 of the two gravity substeps).
  // The COAT first (rules 1-3 of "A COAT IS A CO-LOCATED VIRTUAL NEIGHBOUR"),
  // then the cell's own bucket: TWO reactants, one rule each per tick. A coat
  // rule that rewrote the cell ends the cell's substep as any self rewrite
  // does. One that spent a level has written (and stamped) the word, so the
  // cell's own rules run against the word as it now is -- the coat thinner, or
  // gone -- and the cell does not move this substep (its stamp is this
  // substep's: moving it would carry a stale word). One that matched and
  // quenches covers the cell for its own rules.
  if (P.substep == 0u) {
    var covered = false;
    var spent = false;
    var wNow = w;
    var coatNow = coat;
    if (coat != 0u) {
      let cr = coatReact(c, idx, slotIdx, w, mat, m, coat, rnd, skip);
      if ((cr.x & COAT_GONE) != 0u) { return; }
      covered = (cr.x & COAT_COVERED) != 0u;
      spent = (cr.x & COAT_SPENT) != 0u;
      wNow = cr.y;
      if (!voxStained(wNow)) { coatNow = 0u; }
    }
    gThinMul = 1u;
    if (GAS_THIN_ON && m.klass == CLASS_GAS && m.reactCount > 0u) {
      gThinMul = gasThinMul(ci, local, m);
    }
    if (m.reactCount > 0u &&
        doReactions(c, idx, slotIdx, wNow, mat, m, rnd, false, coatNow, covered, skip)) {
      return;
    }
    // A conductor with no reaction bucket (copper, iron, steel) still takes
    // E2's after-bucket step; one with a bucket took it in doReactions.
    if (m.reactCount == 0u && conducts && !gInTicket && !spent) {
      let er = elecReact(c, idx, slotIdx, wNow, mat, m, rnd, skip, stampFor(T.tick, P.substep));
      if (er == 2u) { return; }
      if (er == 1u) { markDirtyR(c, DIRTY_R_REACT); }
    }
    gThinMul = 1u;
    if (spent) { return; }
  }

  // Staining, same once-per-tick budget as reactions. Gated on the material
  // flag first so the ~all materials that do not stain pay one comparison.
  // Keeps the chunk awake only while unstained surface remains in reach — see
  // the sleep note on doStaining.
  if (P.substep == 0u && matStains(m)) {
    if (doStaining(c, idx, w, m, rnd, skip)) { markDirtyR(c, DIRTY_R_STAIN); }
    // Absorption can have emptied this cell (the liquid soaked away) or docked
    // its fullness and stamped it. Re-read before the movement code below acts
    // on a stale word: moving an already-spent eighth would create mass.
    let after = voxWordAt(c);
    if (voxMat(after) == MAT_AIR) { return; }
    if (voxStamp(after) == stampFor(T.tick, P.substep)) { return; }
  }

  // A stamp-skipped cell has said whether it has matched work; it does not move.
  if (skip || m.klass == CLASS_SOLID) { return; }

  // Viscosity: thick liquids (lava, molten glass, blood) only move on their
  // tick. On an off-tick the cell stays awake for the tick it MAY move on —
  // but only if it has somewhere to go.
  //
  // The unconditional markDirty this replaces meant a settled pool of any
  // viscous liquid re-dirtied its chunk on every off-tick, forever: those
  // chunks could never sleep, at any pool size, for the rest of the session
  // (CLAUDE.md rule 2). It went unnoticed because the sleep selftest only ever
  // settled water and powders — moveEvery is 1 for both, so they take the fast
  // path below and this branch never ran in the test.
  //
  // `canFlowAnywhere` is the same predicate stepLiquid() uses to decide it has
  // work, evaluated read-only: if it is false the cell would do nothing on its
  // move tick either, so there is nothing to stay awake for. A pool whose
  // surface is flat and whose floor is solid therefore sleeps, and any change
  // around it (a wall broken, liquid added) marks it dirty through the normal
  // paths and wakes it back up.
  if (m.moveEvery > 1u && (T.tick % m.moveEvery) != 0u) {
    if (canFlowAnywhere(c, w, mat, m)) { markDirtyR(c, DIRTY_R_VISCOUS); }
    return;
  }

  if (m.klass == CLASS_LIQUID) {
    // `w` is still this cell's word here: doReactions returns true whenever it
    // writes SELF (every other write it makes is a face neighbour), and the
    // staining block above returns if absorption wrote self (it stamps it).
    //
    // NO canFlowAnywhere AFTER A FAILED stepLiquid (DIRTY_R_FLOW, retired
    // 2026-09-23). It used to be asked here as PLAN §1.1 defect 4's "settled
    // but not stable" mark, and it could never answer true: stepLiquid writes
    // nothing on any path that returns false, every read both functions make
    // is at reach 1 (which no other thread writes this pass), and the mirror
    // tests the same stages against the same word, licence and submerged
    // flag. Measured-by-proof dead, so deleted. A cell whose exit a neighbour
    // took this tick is woken by that neighbour's write, which fans out.
    stepLiquid(c, idx, w, mat, m, rnd);
    return;
  }

  // ---- GASES: the whole tail, in stepGas ----------------------------------
  // Stages 0..3 of the old chain used to be written out here with `dy` and
  // `rising` shared between gas and powder. They are one function now for one
  // reason: sim_gas.wgsl's particle kernel must take the moves a voxel would
  // (PLAN_gas_particles.md §2.2), and it can only do that against a ladder
  // that EXISTS as a callable thing. What the powder path keeps is stages
  // 1, 2, 4 and 5 with dy = -1, which is all it ever used.
  if (m.klass == CLASS_GAS) {
    // The render-only density splat, ONCE PER TICK (substep 0) and BEFORE the
    // move, so this voxel contributes to the cell it is in rather than to the
    // one it is about to be in. Gated on gasMode for the reason the sink is:
    // with GAS_MODE_OFF no gas row is recorded at all, fill_gasOuter never
    // clears the box, and a splat into an uncleared box would accumulate
    // forever. See gasOuterSplat.
    if (T.gasMode != GAS_MODE_OFF && P.substep == 0u) { gasOuterSplat(c); }
    // A DISCHARGE DOES NOT DRIFT (E2, 2026-10-03). A gas that is an electric
    // SOURCE (spark, arc, lightning: `_r2` bit 25) lives a tick or two where
    // it struck and never moves. Two reasons, one physical: a spark does not
    // float. The other is the pool-top parity miss (selftest_chem.cpp "WHY
    // THREE"): a spark laid on a pool whose top is y = 2 (mod 3) used to rise
    // in its own phase -- which runs before the pool cell's -- so the pool
    // never saw it at a face, AND the charge field (run after the CA) found it
    // a cell clear of the pool, so the pool never charged either. Held still,
    // it is a face partner for its whole life and seeds the pool's charge.
    // Its decay rule (doReactions above) still runs and holds the chunk awake.
    if ((m._r2 & ELEC_R2_SOURCE) != 0u) { return; }
    stepGas(c, idx, w, m, slotIdx, rnd);
    return;
  }

  // Diagnostic (DIRTY_M_*): which CLASS moved, so "MOVE with no liquid stage"
  // names the mover instead of being a blank. Only powders reach here — solids
  // and liquids returned above, gases into stepGas.
  let cls = DIRTY_M_POWDER;

  // 1) straight fall -- through air or a lighter fluid, or onto a plant it
  //    crushes (tryCrush)
  if (tryMove(c, c + vec3<i32>(0, -1, 0), w, m.density, false) ||
      tryPowderOnto(c, c + vec3<i32>(0, -1, 0), w, m) ||
      tryCrush(c, c + vec3<i32>(0, -1, 0), w)) {
    markDirtyR(c, cls);
    return;
  }

  // ANGLE OF REPOSE: which run:rise tier THIS grain uses (see the block above
  // tryMove). REPOSE_1_1 for every powder that authors no `repose`, and that
  // value makes BOTH the diagonal gate in stage 2 and the slide stage 2b
  // no-ops, on their first compare, with no hash and no extra load. That is
  // what makes "absent repose == the old sim, bit for bit" structural rather
  // than a claim. No class test: only powders reach here now that gases have
  // their own ladder in stepGas.
  var reposeCode = REPOSE_1_1;
  if (m.repose != 0u) { reposeCode = matReposeCode(m, c); }

  // 2) the four diagonal cells one step down, RNG order — started
  //    DOWNWIND with a probability set by the wind and the material's authored
  //    response (windLateralStart; no-op when the gate is off). The rotation
  //    itself is unchanged: this only decides where it begins.
  //
  //    A STEEP tier (1:2, 1:3) gates this stage: the diagonal is taken only if
  //    the grain would drop at least M, read from the tick-start snapshot. Every
  //    other material's gate returns true on one compare.
  let r = windLateralStart(c, rnd >> 10u, m, slotIdx);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (!reposeDiagAllowed(c, d, reposeCode)) { continue; }
    // Onto a partial powder diagonally too (merge/sink): grains settle into
    // the hollow beside them. Same gate, same reach as the diagonal move.
    let dg = c + vec3<i32>(d.x, -1, d.y);
    if (tryMove(c, dg, w, m.density, false)) {
      markDirtyR(c, cls);
      return;
    }
    // Onto a partial below-diagonal: with fine repose on, only when the
    // column-top drop is past the material's repose (fineDiagSteep) -- a
    // generated 4/8 lip must not pour into the 6/8 stair under it.
    var diagOk = true;
    if (TUNE_POWDER_FINE_REPOSE != 0u && inBounds(dg)) {
      let dw = voxWordAt(dg);
      if (voxMat(dw) != MAT_AIR && powderIsPartial(dw)) {
        diagOk = fineDiagSteep(powderMass(w), powderMass(dw), m) && powderSurface(c);
      }
    }
    if (diagOk && tryPowderOnto(c, dg, w, m)) {
      markDirtyR(c, cls);
      return;
    }
  }

  // 2b) FLOWY TIERS ONLY: a resting grain takes one lateral step toward a drop
  //     within its tier's run. See the block above tryMove for the tier table,
  //     the position-keyed blend and the snapshot; this is only the wiring.
  //
  //     GATED ON THE GRAIN'S OWN CODE, not merely on `repose != 0`: a material
  //     authored between 27 and 44 degrees mixes 2:1 and 1:1 per grain, and a
  //     grain whose roll came up 1:1 must behave like a 45-degree grain -- which
  //     is the entire content of the blend. Gating on the material instead made
  //     every blended powder behave as pure 2:1 and the blend inert.
  //
  //     CANNOT FIRE FOR A GRAIN THAT COULD HAVE DESCENDED: stages 1 and 2
  //     return on success, so reaching here means straight-down and all four
  //     down-diagonals were refused. The lateral move goes through tryMove, so
  //     write reach stays 1 and flagSupportLoss still runs for the cell this
  //     grain vacates, exactly as it does for a diagonal.
  //
  //     NEAREST DROP FIRST, in two passes, and that ordering is the TERMINATION
  //     argument rather than a preference. A slide is the one move in the chain
  //     that does not lower the grain, so an unbounded run of slides would keep
  //     a chunk awake forever (rule 2). It cannot happen: a slide toward a drop
  //     k cells out leaves the grain with that same drop k-1 out, and
  //       k-1 == 1  -> stage 2 takes it next tick and the grain FALLS;
  //       k-1 == 2  -> pass 1 fires again, and it cannot instead slide BACK,
  //                    because the cell 2 out backwards is this grain's old
  //                    down-diagonal, which was filled (that is why it is here).
  //     So every slide is followed by another slide strictly closer to the drop,
  //     or by a fall, within at most two steps. Searching 3-out before 2-out
  //     would break exactly this: a grain between two drops at 3 and 2 could
  //     take the far one and then come back.
  //
  //     NOT WHILE FLOATING. That argument assumes a "drop" is somewhere the
  //     grain can fall INTO, but the snapshot bit is density-blind: it counts
  //     every liquid as open (reposeSnapOpenMat). A grain lighter than the
  //     liquid it rests on (ash, seed, snow on water) sees a drop in every
  //     direction, slides, and sees one again -- forever, and its chunk never
  //     sleeps (rule 2). Stage 1 just refused the cell below, so if that cell
  //     is not solid/powder it is a liquid this grain cannot enter: it is
  //     floating, and a floating grain has already reached its rest. Distance
  //     1, so the live read is legal. A grain on a shore may still take ONE
  //     slide onto the surface; once there it floats and stops. Below the
  //     window is unreadable and reads as not floating (the old behaviour).
  let below = c + vec3<i32>(0, -1, 0);
  var floating = false;
  if (inBounds(below)) {
    let bm = voxMat(voxWordAt(below));
    let bk = materials[bm].klass;
    floating = bm != MAT_AIR && bk != CLASS_SOLID && bk != CLASS_POWDER;
  }
  if (!floating && (reposeCode == REPOSE_2_1 || reposeCode == REPOSE_3_1)) {
    // Same RNG-rotated direction order the diagonal loop just used, so a slide
    // does not get its own symmetry-breaking and downwind still leads.
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + r);
      // Cheapest rejection first, and a LIVE read on purpose: the lateral cell
      // is at distance 1, inside my own write box, so reading it live is legal
      // and it must agree with the tryMove that follows. In a packed pile three
      // of four laterals are filled and those directions never pay a probe.
      let lat = c + vec3<i32>(d.x, 0, d.y);
      if (!inBounds(lat) || !canDisplace(m.density, false, voxWordAt(lat))) { continue; }
      if (!reposeSnapOpen(c + vec3<i32>(d.x * 2, -1, d.y * 2))) { continue; }
      if (tryMove(c, lat, w, m.density, false)) {
        markDirtyR(c, cls);
        return;
      }
    }
    // 3:1 only: a drop three out. Reaching this loop means pass 1 found no drop
    // two out in ANY direction, which is what makes this the far search and not
    // a competing one.
    if (reposeCode == REPOSE_3_1) {
      for (var i = 0u; i < 4u; i++) {
        let d = lateralDir(i + r);
        let lat = c + vec3<i32>(d.x, 0, d.y);
        if (!inBounds(lat) || !canDisplace(m.density, false, voxWordAt(lat))) { continue; }
        if (!reposeSnapOpen(c + vec3<i32>(d.x * 3, -1, d.y * 3))) { continue; }
        if (tryMove(c, lat, w, m.density, false)) {
          markDirtyR(c, cls);
          return;
        }
      }
    }
  }
  // 2c) SUB-VOXEL REPOSE: shed eighths toward a neighbour column whose top is
  //     past this material's repose (tryFineRepose). Only a grain that could
  //     not descend reaches here, like the slide above.
  if (TUNE_POWDER_FINE_REPOSE != 0u && tryFineRepose(c, w, m, r)) {
    markDirtyR(c, cls);
    return;
  }

  // 4) wandering powders (mites): scuttle laterally, occasionally hop up.
  if (m.klass == CLASS_POWDER && (m.flags & MATF_WANDER) != 0u) {
    let r3 = rnd >> 18u;
    if ((r3 & TUNE_WANDER_HOP_MASK) == 0u &&
        tryMove(c, c + vec3<i32>(0, 1, 0), w, m.density, false)) { return; }
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + (r3 >> 3u));
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, false)) { return; }
    }
  }

  // 5) entrainment: this grain has now failed every move its own weight can
  //    justify, which is exactly the definition of SETTLED — so this is the one
  //    place a wind is allowed to pick it up (invariant 4). Substep 0 only, the
  //    same once-per-tick budget reactions and staining take, which is also
  //    what lets WIND_ENTRAIN_CHANCE be a plain per-tick rate.
  //
  //    Reaching here at all means the chunk was already awake. The ambient
  //    field cannot wake it (invariant 3) — but a grain that DOES hop marks its
  //    chunk through the ordinary tryMove path and so keeps it awake, which is
  //    why the LICENCE below is required rather than assumed.
  //
  //    TWO WAYS TO HOLD THAT LICENCE, and the difference between them is the
  //    whole of RESEARCH_wind.md §10:
  //
  //      * inside a wind PRIMITIVE that declared kWindPrimEntrain. Its
  //        footprint was dirty-marked through the mutation path this tick, so
  //        those chunks are CPU-known, materialized with their 26-ring, and
  //        charged against a per-tick budget BEFORE the dispatch that writes
  //        them. This is the shipping path, and it is the one that makes a fan
  //        blow a sand pile flat without losing grains.
  //      * sim.windMode >= 2, which is the same rule with no footprint and no
  //        budget: ANY settled powder anywhere may be pulled loose by the
  //        ambient field. It is not rule-2 clean and it is not page-table safe
  //        (62 reproducible faults over two 160-tick runs), so it ships off and
  //        exists to be looked at. See kWindModeEntrain in world.h.
  //
  //    Order matters for cost, not for correctness: the mode compare is free
  //    and the footprint test walks the primitive list, so the cheap one goes
  //    first and a world with no primitives never reaches the loop.
  if (T.windMode >= WIND_MODE_DRIFT && P.substep == 0u &&
      m.klass == CLASS_POWDER &&
      (T.windMode >= WIND_MODE_ENTRAIN || windPrimEntrainsQ(c, &T))) {
    if (windEntrain(c, w, m, slotIdx)) { return; }
  }
  // Nothing to do: cell settles. If the whole chunk settles, nothing marks it
  // dirty and it sleeps until a neighbor wakes it.
}
