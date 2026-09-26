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
// NOTE: binding 13 (dispatch args) must stay undeclared here — statically
// unused bindings are excluded from the dispatch usage scope, which is what
// makes the same buffer legal as the INDIRECT source in this compute pass.
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
const GAS_SP_HDR      : u32 = 16u; // first record word (words 8..15: sim_gas)
const GAS_SP_STRIDE   : u32 = 8u;  // u32 per record (a 32-byte Particle)
// kGasSpawnPerTick (src/sim/world.h). The BUDGET on edge conversions, charged
// before the write: a refused conversion leaves the voxel exactly where it was
// and it takes today's lateral ladder, so the edge is a rate-limited sink and
// never a hole that loses mass.
//
// AND IT IS SIZED TO BE UNREACHABLE, which is a rule-1 requirement and not a
// throughput one — the long note beside kGasSpawnPerTick in world.h has it:
// the cursor below is a shared atomicAdd, so WHICH voxels lose when it fills
// is scheduling-dependent, and a loser stays in the grid where the world hash
// can see it. `gas-leave` asserts the refusal count is zero.
const GAS_SPAWN_CAP   : u32 = 65536u;
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
const DIRTY_OWN_CHUNK_ONLY : u32 =
    DIRTY_R_REACT | DIRTY_R_STAIN | DIRTY_R_FLOW | DIRTY_R_VISCOUS;

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
    DIRTY_R_MUTATE | DIRTY_R_STAINW | DIRTY_R_REACTW |
    DIRTY_M_DOWN | DIRTY_M_DIAG | DIRTY_M_EQUAL | DIRTY_M_SPLIT |
    DIRTY_M_BRIDGE | DIRTY_M_POWDER | DIRTY_M_GAS |
    DIRTY_M_SOLO;

// Set once per workgroup at the top of main from dirtyIn[ci]; read by BOTH film
// predicates, which is what makes canFlowAnywhere inherit it for free — the
// moving path and the settled path MUST agree or a chunk either pins awake or
// sleeps with work left.
var<private> gFilmLicence : bool = false;

// The acting cell and its physical word index, set once at the top of main.
// tryMove's source is always this cell, and re-resolving it through the page
// table on every move was a second lookup of an answer main already had.
var<private> gSelfCell : vec3<i32> = vec3<i32>(0);
var<private> gSelfIdx : u32 = PT_NO_WORD;

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
const REPOSE_SNAP_TICK_BASE : u32 = NUM_CHUNKS * CHUNK_VOL / 32u;

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
  let idx = cellIndexW(c);
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

  // PASS 0: which ring members are OURS (one thread each, then shared).
  if (li < REPOSE_RING) {
    wgReposeOwned[li] = select(0u, 1u, reposeRingOwned(cx, cy, cz, li));
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
    let slot = reposeRingSlot(cx, cy, cz, k);
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
    reposeSnap[REPOSE_SNAP_TICK_BASE + reposeRingSlot(cx, cy, cz, k)] = stamp;
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
  return r % 3u;
}

// ---- MASS THROUGH A REACTION (docs/PLAN_powder_mass.md §2.5, §10 Q2) --------
// A partial cell that transmutes keeps its eighths when the product can hold
// eighths: 3/8 of snow melts to 3/8 of water, 1/8 of gravel etched by acid is
// 1/8 of sand. Before powder carried mass every product was born full, which
// minted matter from any partial liquid; that is what this retires.
//
// The eighths the cell word `sw` holds: a liquid's fullness, a powder's mass,
// and 8 for everything else (a solid, or a whole cell of anything).
fn cellEighths(sw : u32) -> u32 {
  let sm = materials[voxMat(sw)];
  if (sm.klass == CLASS_LIQUID) { return voxState(sw) + 1u; }
  if (matHasPowderMass(sm)) { return powderMass(sw); }
  return 8u;
}
// The product's state nibble when the cell it replaces held `e` eighths. A
// liquid or powder product keeps the mass; anything else (and anything made
// from a whole cell) is born exactly as productState always made it. Solid
// products from a partial POWDER never get here -- doReactions refuses the
// rule (selfPartialPowder) -- and gas ones are thinned in reactWriteSelf.
fn carriedState(prod : u32, e : u32, r : u32) -> u32 {
  if (e < 8u && prod != MAT_AIR) {
    let pm = materials[prod];
    if (pm.klass == CLASS_LIQUID) { return e - 1u; }
    if (matHasPowderMass(pm)) { return e + 2u; }
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

// Does rain reach this cell? Rain falls at an angle and splashes, so a burning
// trunk is wet down its SIDES, not only on top: the cell is exposed if it sees
// the sky, or if a horizontal face opens onto a cell that does (an air-ish
// side neighbour whose own cell above is not a ray blocker). Every read is at
// Chebyshev distance 1 — the side neighbour and the diagonal above it — which
// is the reach seesSky's note proves is scheduling-free: an actor in this pass
// is >= 3 away and writes reach 1, so it cannot touch a cell within 1 of me.
// Costs nothing while dry: both callers test T.weatherRain first.
fn rainOpen(n : vec3<i32>) -> bool {
  if (!inBounds(n)) { return true; }
  let nm = voxMat(voxWordAt(n));
  return nm == MAT_AIR || !isRayBlocker(materials[nm]);
}
fn rainExposed(c : vec3<i32>) -> bool {
  if (seesSky(c)) { return true; }
  for (var i = 0u; i < 4u; i++) {
    let d = select(vec3<i32>(0, 0, select(-1, 1, i == 3u)),
                   vec3<i32>(select(-1, 1, i == 1u), 0, 0), i < 2u);
    let n = c + d;
    if (rainOpen(n) && rainOpen(n + vec3<i32>(0, 1, 0))) { return true; }
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
  var e = 8u;
  var selfPowder = false;
  if (!synthSelf) {
    let sw = voxWordAt(c);
    e = cellEighths(sw);
    selfPowder = matHasPowderMass(materials[voxMat(sw)]);
  }
  // A partial grain that becomes a GAS (dust flashing to fire) makes one whole
  // gas cell with probability e/8 and otherwise just goes: a dusting burns
  // like 1/8 of a pile, not like a pile. Gas has no eighths to carry, and the
  // roll keeps expected mass exact without one.
  let thinned = selfPowder && e < 8u && prod != 0u &&
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
  if (!open) { markDirtyR(c, DIRTY_R_STAIN); }
  // Nothing open to the sky dries while it rains: that is the rain's surface.
  // (Measured: drying through a storm left a third of a rained-on stone strip
  // CLEAN -- its 1-level wet mark dried between drops.)
  if (open && (T.weatherRain & RAIN_AMOUNT_MASK) != 0u) { return false; }
  if (probe || (hash3(rnd, 0xD41E5u, 0u) % 1000u) >= chance) { return false; }
  let left = voxStainAmt(w) - 1u;
  var nw = w & ~STAIN_BITS;
  if (left > 0u) { nw = nw | packStain(voxStainType(w), left); }
  voxStore(idx, nw);
  markDirtyR(c, DIRTY_R_STAINW);
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
  let stamp = stampFor(T.tick, P.substep);
  var keepAwake = false;
  var covered = false;
  for (var ri = 0u; ri < cmat.reactCount; ri++) {
    let rule = reactions[cmat.reactOffset + ri];
    if ((rule.packed & 3u) != RK_PAIR) { continue; }
    if (!lightMatches(rule, c)) { continue; }
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
fn doReactions(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32,
               m : Material, rnd : u32, synthSelf : bool, coat : u32,
               covered : bool, probe : bool) -> bool {
  var keepAwake = false;
  let stamp = stampFor(T.tick, P.substep);
  // A partial grain cannot become a whole SOLID (a 1/8 seed does not grow a
  // whole sprout): such rules are skipped, not rolled, and do not hold the
  // chunk awake. Eight grains that merge into a full cell react normally.
  let selfPartialPowder = !synthSelf && matHasPowderMass(m) && powderIsPartial(w);

  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    let kind = rule.packed & 3u;
    let dmask = (rule.packed >> 2u) & 7u;
    if (selfPartialPowder && rule.prodSelf != PROD_KEEP && rule.prodSelf != MAT_AIR &&
        materials[rule.prodSelf].klass == CLASS_SOLID) { continue; }

    // Light/phase gate. A rule whose condition is not met is skipped WITHOUT
    // setting keepAwake — that is what lets a lit pond go back to sleep at
    // night instead of spinning on a rule that cannot fire (rule 2). The
    // chunk is re-woken when the phase crosses back, see wakeOnPhaseChange.
    if (!lightMatches(rule, c)) { continue; }

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
      let chance = rainChance(rule, c, scaledChance(rule, c, coat, covered));
      if (chance == 0u) { continue; }
      keepAwake = keepAwake || !lightGated;
      if (!probe && (rr % REACT_CHANCE_DEN) < chance) {
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
          let ni = voxWordIndex(n);  // resolved only for the cell that is written
          voxStore(ni, packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp));
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
          if (rule.prodNbr != PROD_KEEP) {
            if (synthFluid) { flagFluidConsume(n); }
            // For a synthesized neighbour ni is the air cell: a product
            // writes into it (condensed stone, grown plant); prodNbr == 0
            // rewrites air over air, harmless.
            if (rule.prodNbr == 0u) { voxStore(ni, 0u); }
            else {
              // A partial neighbour keeps its eighths (carriedState).
              let ne = select(cellEighths(niw.y), 8u, synthFluid);
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
      if (!faceMatched && coat != 0u && nbrMatches(rule, coat, materials[coat]) &&
          !(rule.prodSelf == PROD_KEEP && rule.prodNbr == coat)) {
        let flame = isFlame(rule.prodNbr);
        var rf = 6u;
        if (flame) { rf = coatReleaseFace(c, dmask, rot, 6u); }
        if ((coatRuleVerdict(flame, rf < 6u) & COAT_VERDICT_MATCH) != 0u) {
          keepAwake = keepAwake || !lightGated;
          if (!probe && (rr % REACT_CHANCE_DEN) < rainChance(rule, c, rule.chance)) {
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
    }
  }
  if (keepAwake) { markDirtyR(c, DIRTY_R_REACT); }
  return false;
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
fn windLateralStart(c : vec3<i32>, base : u32, m : Material,
                    slotIdx : u32) -> u32 {
  return windLateralStartK(c, base, m, slotIdx, P.substep, &T);
}
fn gasIntent(c : vec3<i32>, m : Material, slotIdx : u32, base : u32) -> GasIntent {
  return gasIntentK(c, m, slotIdx, base, P.substep, &T);
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

fn gasLeave(c : vec3<i32>, idx : u32, w : u32, dst : vec3<i32>) -> bool {
  let slot = atomicAdd(&gasSpawn[GAS_SP_COUNT], 1u);
  if (slot >= GAS_SPAWN_CAP) {
    atomicAdd(&gasSpawn[GAS_SP_REFUSED], 1u);
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

// ---- THE GAS MOVEMENT TAIL -------------------------------------------------
// Everything a CLASS_GAS voxel does once reactions and staining are done with
// it. Returns true if the cell is finished (it moved, or it left the window).
//
// A gas that exhausts all fourteen candidates returns false and the caller
// returns anyway: stages 4 and 5 of the old chain (wandering powders and
// saltation) both test CLASS_POWDER, so a gas falling through them was always
// a no-op. Naming that here rather than letting it fall out is the difference
// between "gas is done" and "gas happens to do nothing next".
fn stepGas(c : vec3<i32>, idx : u32, w : u32, m : Material, slotIdx : u32,
           rnd : u32) -> bool {
  let g = gasIntent(c, m, slotIdx, rnd >> 10u);

  // Indices 0..5 need no lateral rotation. Split from the loop below so the
  // common case — a plume with open sky above it, returning at index 0 —
  // never evaluates the wind field for a ring it does not reach.
  for (var i = 0u; i < GAS_LADDER_RING; i++) {
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
    if (T.gasMode != GAS_MODE_OFF && i == 0u && !inBounds(c + d)) {
      atomicAdd(&gasSpawn[GAS_SP_EDGE], 1u);
      markDirtyR(c, DIRTY_M_GASEDGE);
      if (gasLeave(c, idx, w, c + d)) { return true; }
      // Refused: fall through and behave exactly as this voxel does today.
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
  let wv = windAtQ(c, &T);
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
  // moves a surface slowly rather than all at once.
  if (i32((windRnd(slotIdx) >> 10u) & 1023u) >= WIND_ENTRAIN_CHANCE) {
    return false;
  }
  if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w32, m.density, false)) {
    return true;
  }
  return tryMove(c, c + vec3<i32>(d.x, 1, d.y), w32, m.density, false);
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

@compute @workgroup_size(6, 6, 6)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  // one workgroup per compacted dirty chunk (indirect dispatch). The list
  // holds SLOT indices; reconstruct the world chunk from the window origin.
  let ci = dirtyList[wg.x];

  // ---- A SENTINEL CHUNK WHOSE MATERIAL CANNOT ACT IS A WHOLE-CHUNK NO-OP ----
  // Workgroup-uniform (one table load per workgroup), so the early return is
  // uniform too. Two cases, and ONLY these two, are provably inert:
  //   * PT_EMPTY: every cell is air, and main returns on air.
  //   * UNIFORM / JITTER of a material with !matCanAct (a plain solid: no
  //     reaction bucket, no stain, CLASS_SOLID). Every cell is that solid for
  //     the whole dispatch — the table is read-only during it, and a store into
  //     a sentinel is a dropped fault, never a write — so every cell has an
  //     in-chunk face neighbour of the same solid, soloSolid() is false, and
  //     main's `!matCanAct` return is the next thing it does. Nothing is written
  //     and nothing is marked.
  // What this does NOT skip, deliberately: a sentinel of any material that CAN
  // act (sand, water, grass, anything with a rule or a stain). Rules that a
  // NEIGHBOUR chunk's content triggers across the face (a PAIR, a stain, a
  // flow into this chunk) are run by the neighbour's cells, which are in the
  // neighbour's workgroup and untouched by this return.
  let pe = pageEntryOf(ci);
  if ((pe & PT_SENTINEL_BIT) != 0u) {
    let smat = pe & PT_MAT_MASK;
    if (smat == MAT_AIR || !matCanAct(materials[smat])) { return; }
  }

  // Workgroup-uniform, one scalar load, read only by the riser film step: did
  // anything that is not itself a film step happen in this chunk last tick?
  // FILM_LICENCE block — this is what stops a neutral rule from keeping a
  // shoreline puddle awake forever.
  gFilmLicence = (dirtyIn[ci] & FILM_LICENCE) != 0u;
  let wc = slotWorldChunk(ci, T.origin);
  let base = wc * i32(CHUNK);  // world cell of the chunk corner (may be < 0)
  // The color lattice is GLOBAL in WORLD coords: cell ≡ colorPhase (mod 3).
  // Coloring by slot coords would race at the toroidal wrap (world-adjacent
  // cells whose slots are WORLD_N apart would share a color); world coords
  // keep same-color cells >=3 apart in the space movement happens in.
  let bmod = ((base % vec3<i32>(3)) + vec3<i32>(3)) % vec3<i32>(3);
  let start = (vec3<i32>(P.colorPhase) + vec3<i32>(3) - bmod) % vec3<i32>(3);
  let local = start + vec3<i32>(lid) * 3;
  if (local.x >= i32(CHUNK) || local.y >= i32(CHUNK) || local.z >= i32(CHUNK)) { return; }
  let c = base + local;  // world cell this thread acts on

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
  if (!matCanAct(m) && coat == 0u) { return; }
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
    if (m.reactCount > 0u &&
        doReactions(c, idx, slotIdx, wNow, mat, m, rnd, false, coatNow, covered, skip)) {
      return;
    }
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
    if (tryMove(c, c + vec3<i32>(d.x, -1, d.y), w, m.density, false) ||
        tryPowderOnto(c, c + vec3<i32>(d.x, -1, d.y), w, m)) {
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
