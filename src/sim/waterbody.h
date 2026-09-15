#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// waterbody.h — COMPONENT 1 (body identity) + COMPONENT 5's STRUCTURE
// (jurisdiction), the M1 milestone of docs/PLAN_water_master.md.
//
// THE ONE-PARAGRAPH VERSION. A lake is a name plus a small record of
// aggregates: which basin it is, where its free surface sits, how many cells
// that surface has, how much water it holds in EIGHTHS, and a ledger of what
// has been taken out of it but not yet taken off the top. Once a still body of
// water has a name, draining it is arithmetic on that record instead of
// pressure propagating cell by cell through 87,000 voxels — which is the whole
// architectural bet of the master plan. This file is the name and the record.
// It is deliberately the ONLY part of that plan that changes nothing.
//
// ---- WHAT M1 IS, AND WHAT IT IS NOT ---------------------------------------
//
// M1 has NO BEHAVIOUR. Nothing here writes a voxel, dispatches a kernel,
// touches TickParams or reaches the GPU at all. `sim.waterBodyMode` is 0 by
// default and even at 1 the only observable is the dev overlay and the gate.
// That is the point: components 2-7 are all impossible without a name for
// "this lake", and landing the name alone means the world hash provably cannot
// move (`--sweep sim.waterBodyMode=0,1` gives one hash) while the descriptor,
// the labelling, the straddle refusal and the jurisdiction ladder are all under
// test.
//
// The pieces M1 does NOT have, and where they land:
//
//   * The GPU reduce. Component 1 specifies adoption reading the voxel sum into
//     the ledger through a reduce over the body's chunks. That buffer, its
//     bindings and its pass_table rows arrive with M2, when the ledger has
//     something to be exact ABOUT. Until then the descriptor's volume and area
//     are the ANALYTIC prediction (below), and `--gate waterbody` pass G is
//     what measures the prediction against real voxels.
//   * The drain ledger, the surface shave, the discharge law. M2/M3.
//
// ---- DERIVED DATA, ONE OWNER (plan §3.1) ----------------------------------
//
// > Voxels are authoritative. Every descriptor, table and label here is
// > derived: reconstructible from the voxels, disposable, never saved, never
// > hashed.
//
// The same doctrine the page table lives under, and for the same reason. A
// pooled body's interior voxels continue to exist and continue to BE the mass;
// this is a cache of aggregates over them. So any gate may recompute a
// descriptor from scratch and assert equality with the live one, which is the
// primary test hook and the reason pass G exists.
//
// ---- THE BASIN IS ANALYTIC, AND THAT IS NOT A SHORTCUT ---------------------
//
// Terrain overhaul package C made a tarn's bowl REPLACE the ground rather than
// min() into it (worldgen.wgsl, the `h = L.pw.x` block), so `pondAt` is an
// exact integer parabola with known parameters and the three authored pools are
// exact flat-floored cylinders. That makes area-per-height a CLOSED FORM for
// every body worldgen creates — no flood fill, no measurement pass, no storage
// beyond a tile coordinate. Player-dug basins need the height-ordered
// union-find sweep of component 10 and are simply NOT ADOPTED until it exists;
// the master plan names that as a legitimate v1 and it is the one taken here.
//
// The CPU half of `pondAt` already exists and is token-compared against the
// shader by scripts/check_invariants.py (world.cpp's MIRROR-BEGIN height
// block). This file consumes it through World::TerrainColumn /
// World::PondNearColumn rather than mirroring it a fourth time — world.cpp's
// deleted `surfHeightAt` is that file's own evidence for how a fourth copy
// ends.
//
// ---- WHY THE TABLE IS A SCHEDULE AND NOT AN AUTHORITY (plan §3.2) ----------
//
// The area curve here is a PREDICTION. From M2 the surface shave will count the
// cells it actually removed and debit THAT, so an inaccurate curve costs a
// slightly off-pace surface descent and never a mass error. Building it the
// other way round — deriving the debit from the curve — makes every inaccuracy
// in this file a mass leak that no conservation gate can attribute. Nothing in
// this header may become the source of a mass number.
//
// ---- THE M1 HAZARD, AND HOW M2 CLOSED IT ----------------------------------
//
// M1 recorded this and it is worth keeping the whole shape of, because the fix
// is the reason this file looks the way it does now.
//
// The M1 jurisdiction ladder's quiescence term read `World::Snap()` — the ASYNC
// readback, which arrives on a schedule set by fence retirement rather than by
// tick number. That was harmless while the verdict was advisory and no kernel
// read it. The moment adoption gates a shave it stops being harmless: "adopted
// when the CPU got around to noticing" is a scheduling-dependent outcome and
// breaks rule 1 through the back door, and no determinism gate that runs twice
// in one process with the same fence cadence would catch it.
//
// M2's answer is a SPLIT OF AUTHORITY, not a cleverer CPU test:
//
//   * The CPU decides what it can decide from pure functions of (seed, window,
//     tuning) — basin geometry, chunk labelling, straddles, residency, the
//     spill elevation, the volume thresholds and their hysteresis. Every one of
//     those is reproducible from the tick alone. It PROPOSES bodies.
//   * The GPU decides everything that depends on what the world is DOING.
//     Quiescence is measured from `dirtyIn` and the MPM block map — the hashed
//     world's own state — and the Candidate -> Measuring -> Adopted ladder,
//     the level, the area and the whole ledger live in `waterBodyState`. See
//     assets/shaders/sim_waterbody.wgsl.
//
// So `WaterBodyState::Proposed` below does NOT mean "this lake is governed". It
// means "the CPU has no objection". Nothing in this file reads a snapshot, and
// nothing in this file may start to: the payload it builds gates a voxel write.

namespace sandvox {

// `kWaterBodyCap` — the ceiling on live descriptors, and the bound
// `sim.waterBodyMaxCount` clamps against — lives in sim/world.h, next to
// kWindPrimCap and for the same reason: from M2 it sizes a TickParams array and
// a per-body state buffer, so it is a GPU layout, and world.h is where a layout
// a shader must agree with is stated.

// "This body has no slot in the GPU ledger this tick." A body is unproposed,
// refused, or lost the chunk budget.
constexpr uint32_t kNoGpuSlot = 0xFFFFFFFFu;

// Ticks a world mutation keeps a governed body's footprint declared to the page
// table — see `drainHotUntil_` below for the whole argument. 900 = 30 s.
constexpr uint32_t kWaterDrainHotTicks = 900;
// Extra ticks the FOOTPRINT stays declared after the hot window closes, so a
// debit the ledger granted on its last armed tick can still be shaved into
// declared chunks. See BuildGpu.
constexpr uint32_t kWaterDrainSettleTicks = 64;

// Ticks a W3 IMPULSE keeps a governed body's footprint declared to the page
// table (PLAN_water_relevel.md §5). Its own number rather than the drain's
// because the two events have different lifetimes: a dig starts a discharge
// that runs for minutes, a splash starts a ring that pass S measured asleep
// after 112 ticks. 240 is eight seconds — twice the measured life of a ring,
// and a thirtieth of what a swimmer would otherwise cost a lake every time
// they crossed it.
constexpr uint32_t kWaterImpulseHotTicks = 240;

// How a basin's floor is shaped. Two kinds cover every body worldgen makes,
// and both are closed forms; a third kind is what component 10's union-find
// sweep will produce for dug terrain.
enum class WaterBasinKind : uint32_t {
  FlatDisc = 0,      // the three authored pools: floor is one Y, walls vertical
  ParabolicBowl = 1, // the pre-P-F tarn: an integer parabola (kept for the gate's round-trip arm)
  Profiled = 2,      // a P-F tarn or authored lake: the preset's sampled profile,
                     // World::BowlDepth(preset, r, d2), inverted by bisection
};

// THE CONTAINER. A pure function of (seed, tuning) — nothing here is measured
// and nothing is stored per voxel.
struct WaterBasin {
  // Stable across ticks and across a window shift, because it is derived from
  // WHERE the basin is rather than from the order it was discovered in. Tarns
  // key off their pond tile; the authored pools take fixed low ids. A descriptor
  // that changed identity when the player walked away would re-adopt itself
  // every window move and flap through the seam that plan §5 calls a mass-loss
  // machine.
  uint32_t id = 0;
  int cx = 0, cz = 0;        // disc centre, world cells
  int radius = 0;            // disc radius, world cells
  // Inclusive squared-radius bound: a column is inside iff dx*dx+dz*dz <= this.
  // Carried rather than derived because the two kinds disagree by one — pondAt
  // rejects `d2 > r*r` while the authored pools test `d2 < r*r` — and a
  // one-cell ring is 2*pi*r cells of silent disagreement at r=68.
  int discD2Max = 0;
  int surfY = 0;             // the free surface worldgen authored (fill level)
  int floorY = 0;            // the DEEPEST floor cell's Y (bowl centre)
  int rimDepth = 0;          // depth below surfY at the rim (0 for FlatDisc)
  int centreDepth = 0;       // depth below surfY at the centre
  // The elevation at which water leaves. For a tarn this is the berm core,
  // `surfY + pondBerm` — the wall containment structurally rests on. For an
  // authored pool it is the rim lift. Above it the area table is meaningless
  // because the body is no longer a body, it is a flow.
  int spillY = 0;
  WaterBasinKind kind = WaterBasinKind::FlatDisc;
  uint32_t preset = 0;       // Profiled: the water preset (1-based) the bowl wears
  // The material by NAME, resolved at the consumer's boundary (design guideline
  // #4: author content by name, resolve at load). Storing an id here would bake
  // a materials.json ordering into a derived cache.
  std::string matName;
};

// Component 5's ladder, CPU HALF. Candidate -> Proposed needs every
// deterministic enter test to hold; Proposed -> Releasing needs an EXIT test to
// fail, and the exit thresholds sit meaningfully clear of the enter ones.
// Hysteresis is non-negotiable: a body parked on a boundary flips
// representation every tick and every flip is a seam crossing where mass can be
// lost.
//
// PROPOSED IS NOT ADOPTED. The GPU runs its own ladder on top of this one
// (WB_CANDIDATE/WB_MEASURING/WB_ADOPTED in sim_waterbody.wgsl) and it is the
// one that governs water. This enum is what the CPU is entitled to have an
// opinion about — see the authority split in this file's header.
enum class WaterBodyState : uint32_t {
  Candidate = 0,
  Proposed = 1,
  Releasing = 2,
};

// Why a candidate was refused. Reported rather than implied — CLAUDE.md rule 6:
// a bare count is not a measurement, and "3 bodies, 0 adopted" with no reason
// attached is the failure mode that costs a dozen elimination runs.
enum class WaterBodyRefusal : uint32_t {
  None = 0,
  TooSmall,      // volume below sim.waterBodyMinVolume
  Overflowing,   // level at or above the spill elevation
  Straddle,      // a chunk holds two basins (see the note on ChunkBody)
  Spread,        // surface height spread over sim.waterBodySpreadEnter
  AtCap,         // sim.waterBodyMaxCount bodies already proposed
  OutOfWindow,   // the basin is not fully resident
  // kWaterChunkCap is the rule-2 bound on the whole subsystem's dispatch, and
  // rule 2 charges a budget BEFORE emission. A body whose footprint does not
  // fit in what is left is not proposed at all, rather than proposed with a
  // truncated chunk list — a half-listed body would shave part of its surface
  // and report a partial `seen`, which is a wrong AREA, which is a drain that
  // paces wrong. Refusing costs only performance: unproposed means "simulated
  // the way it is today".
  NoChunkBudget,
};

// There is deliberately no `NotQuiet` here any more. Quiescence is measured on
// the GPU from the hashed world (sim_waterbody.wgsl's wbQuiet) because the CPU
// could only ever learn it from the async snapshot — see this file's header.

// THE DESCRIPTOR. Component 1's field list, plus the attribution words plan §7
// asks for BEFORE they are needed.
struct WaterBodyDesc {
  uint32_t basinId = 0;
  int32_t level = 0;             // world Y of the free surface
  uint32_t surfaceArea = 0;      // basin cells at `level` (component 2)
  uint64_t volumeEighths = 0;    // total, for conservation gates
  // ---- the ledger (component 3; INERT at M1, every field zero) ----
  // Present from M1 so the shape of the record is settled before anything
  // writes it, and so pass G has something to assert equality on.
  int32_t debitEighths = 0;      // removed but not yet shaved off the surface
  int32_t shavedEighths = 0;     // what last tick's shave actually removed
  uint32_t remainder = 0;        // leftover below one full eighth-step
  WaterBodyState state = WaterBodyState::Candidate;
  WaterBodyRefusal refusal = WaterBodyRefusal::None;
  // Sticky, and separate from `refusal` on purpose. Straddling is a property of
  // the LABELLING, discovered once per relabel; `refusal` is this tick's verdict
  // and is rewritten every tick. Folding the two would let a body that stopped
  // being refused for some other reason silently forget it shares a chunk.
  bool straddle = false;
  // The basin liquid's material id, resolved from `WaterBasin::matName` at
  // proposal time. The shave compares every cell against it, so it is the one
  // place a name becomes a number (design guideline #4: author by name, resolve
  // at the consumer's boundary).
  uint32_t matId = 0;
  // ---- M5: the split child (component 10) --------------------------------
  //
  // A basin someone has DUG IN proposes a second body over the same disc,
  // flagged WBF_CHILD with component index 1. It owns nothing until the GPU's
  // sweep finds the basin's wet region disconnected at the live level and
  // publishes a two-component map; until then its reduce measures zero, the
  // ladder refuses it for volume, and it costs one GPU slot and one copy of the
  // chunk list. So a lake nobody has touched proposes no child at all and pays
  // exactly what it paid at M4.
  //
  // WHY THE CPU PROPOSES A BODY IT CANNOT SEE THE NEED FOR. The split is a GPU
  // fact — it comes from a level and a voxel scan the GPU owns — and the CPU
  // could only learn it through a readback, which is §1.1 correction 2 all over
  // again. So the authority split M2 established is reused verbatim: the CPU
  // proposes on tick-deterministic inputs (was this basin dug in?), the GPU
  // disposes (is it actually two pools?).
  uint32_t childOf = kNoGpuSlot;   // parent GPU slot, for a child descriptor
  uint32_t component = 0;          // which component of the split map is mine
  bool isChild = false;
  // Set when a mutation lands in a chunk this basin labelled: the cheap
  // detection of the POSSIBILITY that the container changed (plan component
  // 10). Sticky for kWaterDrainHotTicks, like the drain latch and for the same
  // reason — a dig is one tick and its consequences are minutes.
  bool curveDirty = false;
  // Slot in the GPU ledger, or kNoGpuSlot. Assigned in proposal order and
  // STABLE across ticks for as long as the body stays proposed, because the
  // ledger it indexes is carried state: a body that changed slots would inherit
  // another body's level and debit.
  uint32_t gpuSlot = kNoGpuSlot;
  // Chunk SLOT indices this body's footprint covers. The dispatch list M2's
  // reduce will run over, and the set the quiescence test reads dirty flags for.
  std::vector<uint32_t> chunks;
  // Inclusive world-cell AABB of the basin's water. The bound on everything.
  IVec3 lo{0, 0, 0}, hi{-1, -1, -1};
};

// THE HOLE HINT (component 8's drain seeder, docs/PLAN_water_master.md M4).
//
// Where the MUTATION that armed this body's drain window happened, and on which
// tick. It is deliberately NOT the ledger's hole (`WBS_HOLEKEY`): that is a GPU
// fact derived from a level the GPU owns, and the only way the CPU could learn
// it is a readback arriving on a schedule set by fence retirement — which is
// M1's §1.1 correction 2, and would put scheduling inside the input of a field
// a sim kernel reads.
//
// What the CPU knows on the TICK INPUT STREAM is the dig, and a dig is where
// the hole is. So this is exact in the case that matters (a player boring a
// shaft), approximate in the case that does not (which of several digs the
// ledger picked as deepest), and free.
struct WaterBodyHole {
  bool valid = false;
  uint32_t basinId = 0;
  int32_t x = 0, y = 0, z = 0;
  uint32_t tick = 0;   // the tick the mutation arrived on
};

// ============================================================================
// W-D — DISCOVERY: a body the PLAYER made (docs/PLAN_water_relevel.md §8).
//
// M1's registry knows two kinds of container and both are closed forms of
// (seed, tuning). A basin the player DUG and FILLED is neither, so until now it
// stayed CA forever — §6's old first exclusion, lifted by owner decision on
// 2026-09-14 because the thing the owner actually wants ("blow a hole, watch it
// settle") should work on a pool they made.
//
// THE CONSTRAINT THAT SHAPES ALL OF IT (§8.2). The CPU may not look at voxels:
// the mirror is 3x3x3 and a readback on the frame path puts fence retirement in
// a voxel write's control path, which is this file's header note from M1. So
// discovery takes the SAME authority split M2 took:
//
//   * The CPU PROPOSES from the tick stream only. Every liquid-placing brush
//     op and cell op rides the MutationQueue, so "how many eighths of which
//     liquid were placed, where" is an EXACT function of the tick's op list.
//     A replay reproduces it; the twice-run gate compares it by construction.
//   * The GPU DISPOSES. The existing WB_MEASURING reduce measures the real
//     water and the ledger adopts or parks the probe in WB_REFUSED. The CPU
//     never learns the verdict and does not need to — a refused probe's
//     steady-state cost is four ledger loads a tick.
//
// The evidence is therefore a HEURISTIC and is allowed to be wrong in both
// directions. It over-counts (a `kCellOpIfAir` op the grid refused still
// counts; the CPU cannot know) and it under-counts (water that ran laterally
// out of every probe disc is missed, §8.3's stated v1 limit — that water stays
// CA, which is today's behaviour and not a regression). Neither costs an
// eighth: evidence decides only whether a DISC IS PROPOSED.
//
// WHY THE REGISTRY IS SAVED AND NOTHING ELSE HERE IS. Every other record in
// this file is derived data — reconstructible from (seed, window, voxels),
// disposable, never saved. A discovered entry is not: it is the residue of
// player action, so it is AUTHORED-EQUIVALENT TRUTH in guideline #3's sense and
// it goes in the world save (the 'WTRB' section, src/game/persist.cpp). What is
// saved is only the disc and its evidence total — the ledger, the curve and the
// level stay derived, and the GPU re-adopts a restored entry by re-measuring
// the restored voxels exactly as it adopted it the first time.
// ============================================================================

// ONE PROBE DISC. A few integers, which is the whole of what discovery stores.
struct WaterDiscovered {
  // kWaterDiscoverIdBase | slot. Assigned from a FREE-SLOT search rather than
  // from the position in the vector, because a basin id is an IDENTITY (the
  // hole hints, the curve-dirty latch and the carried GPU ledger slot are all
  // keyed by it) and evicting a neighbour must not renumber anybody.
  uint32_t basinId = 0;
  int cx = 0, cz = 0;            // disc centre, world cells
  int radius = 0;                // bounding circle of the evidence + pad
  int floorY = 0;                // lowest evidence Y - kWaterDiscoverFloorPad
  int topY = 0;                  // highest evidence Y; seeds the fill level
  uint32_t matId = 0;            // the liquid the evidence was made of
  // Cumulative eighths of that liquid placed inside this disc. The eviction
  // key, and the only thing that ranks one probe against another.
  uint64_t evidenceEighths = 0;
  uint32_t lastEvidenceTick = 0;
  // THE ONE TICK WBF_REPROBE IS SENT. Not a latch: a probe re-enters the ladder
  // from WB_REFUSED on the tick the flag arrives, and a flag held for several
  // ticks would reset it to WB_CANDIDATE on every one of them so it could never
  // accumulate the quiet ticks adoption needs. 0xFFFFFFFF = never.
  uint32_t reprobeTick = 0xFFFFFFFFu;
  // Non-zero once the entry has lost the cap and is on its way out: proposed
  // with WBF_RELEASE until this tick, then dropped. NEVER drop a descriptor
  // cold — the slot would be reused against a ledger carrying the old body's
  // state, which is the carried-descriptor bug class the drain pass documents.
  uint32_t releaseUntil = 0;
};

// ONE COARSE EVIDENCE CELL: kWaterEvidenceGrid voxels square, per liquid
// material. The accumulator the threshold is crossed in, and it is deliberately
// NOT per-voxel: a set keyed by cell is O(pond) to grow and O(1) to test, and
// the disc a cluster of them bounds is the only geometry discovery ever needs.
struct WaterEvidenceCell {
  int gx = 0, gz = 0;            // floor-divided grid coords
  uint32_t matId = 0;
  uint64_t eighths = 0;
  // The exact voxel AABB the evidence landed in, so the bounding circle is the
  // circle around the WATER and not around the grid.
  int loX = 0, hiX = 0, loZ = 0, hiZ = 0, loY = 0, hiY = 0;
  uint32_t lastTick = 0;
};

// ---- component 2, the analytic half ---------------------------------------
//
// THE CONTAINER CURVE: cell count at each height, its prefix sum, and a binary
// search of that prefix sum.
//
//     area(y)       = number of BASIN CELLS at height y
//     volume(level) = sum of area(y) for y <= level, in EIGHTHS
//     level(volume) = binary search over the prefix sum
//
// COUNT CELLS, NOT COLUMNS. Counting columns silently reimplements the
// single-span-per-column assumption that got heightfields rejected
// (RESEARCH_water_architecture.md §4.1.1) — no caves, no overhangs, no flooded
// tunnel under the lake. For the two closed-form kinds a level's cell set
// happens to BE a disc; the interface is the cell count, and component 10's
// union-find sweep will fill exactly the same table for a basin the player dug.
//
// WHY A BOWL NEEDS THIS AT ALL. It narrows as it empties. At the shipped
// defaults (`pondDepth` 26, `pondDepthRim` 3) the area at the floor is a small
// fraction of the area at the rim, so a fixed-area assumption drifts badly and
// in the direction of draining too slowly at the end — the drain visibly
// stalls. One entry per Y, ~26 for a default pond, is the whole cost.
struct WaterBasinCurve {
  int floorY = 0;                  // the table covers (floorY, floorY + n]
  std::vector<uint32_t> area;      // area[i] = cells at y = floorY + 1 + i
  std::vector<uint64_t> prefix;    // prefix[i] = eighths held up to that level
};

// Evaluate the closed form into a table. Called once per basin at registry
// build, never per tick.
WaterBasinCurve WaterBasinBuildCurve(const WaterBasin& b);

// Basin cells at world height `y`, 0 outside the table. Exact integer lattice
// count — no floating point anywhere, so this is reproducible bit for bit.
uint32_t WaterBasinAreaAt(const WaterBasinCurve& c, int y);

// Volume in EIGHTHS at a given free-surface level. Eighths because that is the
// unit of the CA's state nibble (LIQ_FULL_STATE, common.wgsl) and therefore of
// the entire ledger — integer against integer, no scaling and no rounding.
uint64_t WaterBasinVolumeEighths(const WaterBasinCurve& c, int level);

// The inverse: the level a volume of eighths fills to, plus the leftover that
// does not complete a step. `remainder` is plan §3.3's LEGITIMATE DIVERGENCE
// and it is an output, never implied — a conservation gate that forgets it
// reports a leak that does not exist.
int WaterBasinLevelFor(const WaterBasinCurve& c, uint64_t eighths,
                       uint32_t* remainder);

// THE CPU->GPU PAYLOAD, rebuilt every tick. Everything in it is a pure function
// of (seed, window origin, tuning, tick) — that is the whole property M2 turns
// on, because this payload gates a voxel write and rule 1 does not care how
// convenient the alternative would have been.
//
// The layout is decoded by sim_waterbody.wgsl and by nothing else.
struct WaterBodyGpu {
  // kWaterBodyScalars i32, copied straight into TickParams::waterBodies. Two
  // vec4 rows per body:
  //   row0 = (cx, cz, discD2Max, matId)
  //   row1 = (floorY, seedLevel, seedArea, flags)
  // `seedArea` is the analytic surface-cell count and it SEEDS THE PACE ONLY:
  // the first shave measures the real surface and overwrites it, and the debit
  // is always what was granted. Plan §3.2 — a schedule, not an authority.
  std::vector<int32_t> bodies;
  // <= kWaterChunkCap entries of (gpuSlot << 16) | chunkSlot, in body order.
  // The dispatch extent of all three chunk-shaped passes.
  std::vector<uint32_t> chunks;
  uint32_t bodyCount = 0;
  // A shave could fire this tick, so the caller must (a) declare these chunks
  // as page-table op targets before the command buffer exists and (b) treat the
  // tick as one with chunk-dirtying inputs.
  //
  // Both halves matter and both are the wind-primitive wake's lesson repeated:
  // `cpuDirty` is tightened against a lagging snapshot and that tightening is
  // only sound because SETTLED MATTER WRITES NOTHING. The shave is the second
  // rule in this engine to make resting voxels move, so its chunks have to be
  // declared the way entrainment's are, or the writes land in JITTER sentinels
  // and voxStore counts page faults instead of draining a lake.
  bool writesThisTick = false;
  // ---- M5: THE SCHEDULED RE-DERIVE (components 2 case 2 + 10) ------------
  //
  // Which GPU slot's container curve is being re-derived this tick, and at
  // which world Y. `kWaterBodyCap` means NOTHING IS SCHEDULED, which is the
  // state of every basin nobody has dug into, and it is what makes the two
  // sweep pass rows unrecorded (C_WATERSWEEP).
  //
  // BOTH ARE PURE FUNCTIONS OF THE TICK, and that is the entire rule-1 argument
  // for this milestone. The body is picked by `slot % kWaterSweepPeriod ==
  // tick % kWaterSweepPeriod` and the level by a cursor derived from
  // `tick / kWaterSweepPeriod` — never "the one the CPU noticed had changed",
  // never "the one whose readback arrived". `--gate waterbody` pass F is the
  // gate on exactly this, and plan §3.4 is blunt about the alternative: a
  // re-derive scheduled on CPU convenience is a scheduling-dependent outcome
  // and breaks rule 1 through the back door.
  uint32_t sweepSlot = kWaterBodyCap;
  int32_t sweepLevel = 0;
  // COULD A HOLE BE DRAINING THIS TICK — the same mutation latch, exposed
  // separately because it gates a different thing: whether the CPU reserves the
  // discharge's spawn-op block at all.
  //
  // It has to be gated, and the measurement is why. The block is filled every
  // tick it exists, live ops or dead, so a standing reservation keeps
  // `fluidSpawnCount` non-zero forever — which keeps the whole PT_FLUIDSEAM
  // table recorded, keeps the lab's "idle ticks before the plug" at 0 and cost
  // 5.18 -> 6.69 ms p50 on `pond68` and 2.14 -> 3.64 on `worldlake` for a lake
  // with no hole in it. That is rule 2 with the sign flipped: cost that scales
  // with the world containing a lake rather than with anything happening to it.
  bool drainArmed = false;
  // ---- W3: THE IMPULSE BLOCK (PLAN_water_relevel.md §5) ------------------
  //
  // <= kWaterImpulseCap records x kWaterImpulseWords i32, copied straight into
  // TickParams::waterImpulses. Resolved from the pending queue by Tick(), which
  // also CLEARS the queue: an impulse is one tick's push and nothing about it
  // persists, so a caller that stops pushing stops disturbing the water with no
  // expiry rule to get wrong.
  //
  // Empty is the shipping state and an exact identity — wbFlux's loop runs zero
  // times — which is what makes each of §5's three knobs an identity at 0: at 0
  // the emitter refuses to build the record in the first place.
  std::vector<int32_t> impulses;
  uint32_t impulseCount = 0;
};

// ---- W3: one impulse, the CPU's form (PLAN_water_relevel.md §5) ------------
//
// ONE record for the blast AND the swimmer, because they are the same
// statement: "at this column, for one tick, push the surface this way this
// hard". Two record types would be two things to keep in step for no gain.
//
// `strength` is Q8 pipe flux — the unit `waterFlux`'s pipes already carry — so
// nothing in the kernel has to convert anything and no float reaches the sim.
// The falloff is linear to zero at `radius`, computed GPU-side from the column's
// own distance, so a record is six integers and not a field.
//
// `dir` (Q8) is the direction the push points. (0,0) means RADIAL OUTWARD from
// the centre, which is what a blast is; anything else is a directional shove,
// which is what a wake is. The emitters below never build a (0,0) directional
// record — a swimmer whose velocity rounds to zero in Q8 is not moving and gets
// no record at all — so the overload is never ambiguous.
struct WaterImpulse {
  int32_t x = 0, z = 0;       // world column
  int32_t radius = 0;         // world cells; <= 0 is refused
  int32_t strength = 0;       // Q8 flux at the centre; <= 0 is refused
  int32_t dirX = 0, dirZ = 0; // Q8 direction, (0,0) = radial outward
};

// ---- the system ------------------------------------------------------------
//
// Deterministic by construction in the same sense WindPrimSystem is: the basin
// registry is a pure function of (seed, window origin, tuning), the labelling is
// a pure function of the registry, and the ladder advances by tick comparison.
// The one input that is NOT tick-deterministic is the snapshot's quiescence
// term — see the M2 hazard note at the top of this file.
class WaterBodySystem {
 public:
  // Drop everything. Called on worldgen/load, because a descriptor is a
  // description of a world that no longer exists (World::InvalidateSnapshot's
  // argument, and the same trap: a stale one reads as fresh).
  void Reset();

  // Re-derive the basin registry for the window at `origin`. Cheap — a scan of
  // the pond tiles the window touches, ~4 hashes a tile — and idempotent, so a
  // caller may run it whenever the window moves without checking whether it did.
  void RebuildBasins(const World& world, uint32_t seed);

  // One tick: label chunks, refuse straddles, run the CPU half of the
  // jurisdiction ladder, refresh the descriptors and rebuild the GPU payload. A
  // no-op that touches nothing when `mode` is 0, which is what makes the off
  // switch free rather than merely correct.
  //
  // `testDrain` is sim.waterBodyTestDrain, and it is read here for one reason:
  // it is what tells the CPU whether a shave can fire this tick, which is what
  // decides whether the footprint has to be declared to the page table. It is
  // NOT part of any decision about which bodies exist.
  //
  // `editCell` is the world cell of that mutation (meaningless when
  // `worldEdited` is false). It is recorded per body as the hole hint component
  // 8's drain seeder reads — see WaterBodyHole above. Defaulted so a caller
  // that has no cell to offer still compiles into the M3 behaviour exactly.
  void Tick(const World& world, uint32_t seed, uint32_t tick, int mode,
            int testDrain, int drainMax, int relevelMax, bool worldEdited,
            IVec3 editCell = IVec3{0, 0, 0});

  // ---- W-D: EVIDENCE ACCOUNTING (§8.2/§8.3) ------------------------------
  //
  // Called once per tick from SubmitTick — the ONE function the game frame loop,
  // every gate and both smoke harnesses hand their op lists to — BEFORE Tick()
  // below, so a threshold crossed by this tick's ops raises a probe on this
  // tick. That call site is the whole determinism argument in one sentence: the
  // player's brush arrives as `ops`, a gate's CellOps and every in-game system
  // that writes a voxel (mobs, debris, prefabs, tree felling, spells, the world
  // edit layer) arrive as `cells`, and there is no other door into the
  // MutationQueue. Nothing here reads a voxel, a snapshot or a clock.
  //
  // `mode` is sim.waterBodyMode and `minEighths` is sim.waterDiscoverMinEighths;
  // either at 0 is an immediate return that touches nothing, which is what makes
  // the off switch an exact identity rather than merely a cheap path.
  //
  // A CELL OP carries a SLOT-linear index, so `world` is needed to turn it back
  // into a world cell (SlotToWorldChunk — a slot index is a memory address, not
  // an identity, and reading it as one would put the evidence in another
  // chunk's pond).
  void NoteMutations(const World& world, uint32_t tick, int mode,
                     int minEighths, const CellOp* cells, uint32_t cellCount,
                     const BrushOp* ops, uint32_t opCount);

  // ---- W3: THE IMPULSE DOOR (§5) ----------------------------------------
  //
  // Queue one impulse for the NEXT Tick(). Returns false — and changes nothing
  // — when the queue is full or the record is degenerate (non-positive radius
  // or strength): budgets are charged BEFORE emission (CLAUDE.md rule 2), so a
  // caller learns its splash did not happen rather than silently displacing
  // someone else's.
  //
  // DETERMINISTIC BY CONSTRUCTION, in WindPrimSystem::Spawn's exact sense: the
  // contents are a pure function of the ops and the tick, the order is insertion
  // order, and there is no expiry to get wrong because Tick() consumes the whole
  // queue. What it is NOT is a place to put anything derived from a readback or
  // a wall clock — an impulse decides voxel writes, so it is tick-stream state
  // and the twice-run comparison covers it.
  bool SpawnImpulse(const WaterImpulse& im);
  // The queue as it stands, before Tick() consumes it. For the gate and the
  // overlay; "the wake did nothing" and "the wake was never queued" are
  // different bugs (CLAUDE.md rule 6).
  const std::vector<WaterImpulse>& PendingImpulses() const { return pendingImp_; }
  uint32_t ImpulsesRefused() const { return impRefused_; }

  const std::vector<WaterDiscovered>& Discovered() const { return discovered_; }
  // Evidence cells currently tracked. For the overlay and the gate: "no body
  // appeared" with no reason attached is the bare count rule 6 warns about.
  const std::vector<WaterEvidenceCell>& Evidence() const { return evidence_; }
  // Probes the cap made room for by evicting, cumulative. Reported, not
  // asserted.
  uint32_t DiscoverEvictions() const { return discoverEvictions_; }

  // ---- W-D: PERSISTENCE ('WTRB', src/game/persist.cpp) -------------------
  //
  // The probe registry is the one thing in this file that is NOT derivable from
  // (seed, window, voxels), so it is the one thing that is saved. Restored
  // entries are re-proposed on the next tick and the GPU re-adopts them by
  // re-measuring the voxels the chunk store brought back — no ledger, level,
  // curve or adoption verdict crosses the save boundary, because all of those
  // are descriptions of a world that the loader has just replaced.
  static constexpr uint32_t kSaveVersion = 1;
  void SaveState(std::vector<uint8_t>& out) const;
  bool LoadState(const uint8_t* data, size_t len, uint32_t version);
  // Runs on EVERY load, section present or not: an older save without a 'WTRB'
  // block must not inherit the previous world's probes.
  void ClearDiscovered();

  // The live hole hint for a basin, or an invalid record. Expires with the same
  // hot window the drain latch uses, so a swirl cannot outlive the dig that
  // caused it by more than the drain could have.
  WaterBodyHole HoleHint(uint32_t basinId) const;

  const std::vector<WaterBasin>& Basins() const { return basins_; }
  const std::vector<WaterBodyDesc>& Bodies() const { return bodies_; }
  // M5: the split children proposed this tick, one per dug basin at most. Kept
  // OUT of `bodies_` rather than appended to it because `bodies_` is parallel
  // to `basins_` and `curves_`, and three vectors that are parallel except
  // sometimes is the kind of invariant that holds until the day it does not.
  const std::vector<WaterBodyDesc>& Children() const { return children_; }
  // The child descriptor for a basin, or nullptr. For the gate and the overlay.
  const WaterBodyDesc* FindChild(uint32_t basinId) const;
  // Has a mutation landed in this basin's labelled chunks recently enough that
  // its container curve is being re-derived? Component 10's cheap detection,
  // exposed so `--gate waterbody` can assert the schedule rather than infer it.
  bool CurveDirty(uint32_t basinId) const;

  // Chunk SLOT index -> body index + 1, or 0 for "no body". Sparse aux layer
  // keyed by chunk (design guideline #2) rather than four bits stolen from the
  // voxel word: 32,768 * 4 B = 128 KiB of derived data, not hashed, not saved,
  // rebuilt on load, exactly like the page table.
  //
  // THE STRADDLE CASE. A chunk can hold water from two basins at two different
  // levels, and a shave keyed off the chunk's body would then shave one of them
  // at the wrong level. Both bodies are REFUSED when that happens. Falling back
  // to the CA is a safe degradation, the detection is two lines, and straddles
  // are rare because basins are separated by terrain above the water line.
  // Per-cell labelling to "fix" this buys a rare case at the price of the aux
  // layer's whole cost model.
  const std::vector<uint32_t>& ChunkBody() const { return chunkBody_; }

  // Bodies the CPU has no objection to. NOT the number of governed lakes —
  // that is a GPU fact, and `--gate waterbody` reads it back to report it.
  uint32_t ProposedCount() const;
  const WaterBodyGpu& Gpu() const { return gpu_; }
  uint32_t StraddleRefusals() const { return straddles_; }
  // Basins the registry knows about but could not turn into a candidate this
  // tick because the window does not contain them. Reported for the overlay:
  // "0 bodies" with no reason is the bare count rule 6 warns about.
  uint32_t OutOfWindow() const { return outOfWindow_; }
  int Mode() const { return mode_; }

  // Descriptor / geometry / curve for a basin id, or nullptr. For the gate and
  // the overlay. Keyed by ID rather than by index because an index is a fact
  // about this tick's registry and an id is a fact about the world.
  const WaterBodyDesc* Find(uint32_t basinId) const;
  const WaterBasin* Basin(uint32_t basinId) const;
  const WaterBasinCurve* Curve(uint32_t basinId) const;

  // The discovered entry for a basin id, or nullptr. For the gate and overlay.
  const WaterDiscovered* DiscoveredFor(uint32_t basinId) const;

 private:
  void Relabel(const World& world);
  void Classify(const World& world, uint32_t tick);
  void BuildGpu(uint32_t tick, int testDrain, int drainMax, int relevelMax);
  // W-D: fold one liquid placement into the evidence grid. Returns the cell it
  // landed in, creating or evicting as the cap requires.
  void AddEvidence(uint32_t matId, int x, int y, int z, uint64_t eighths,
                   uint32_t tick);
  // W-D: scan the evidence clusters and raise / grow probe discs. Pure function
  // of (evidence_, discovered_, basins_, window, tick, threshold), and run only
  // on ticks where one of the first two moved — so a world nobody is pouring
  // into pays one boolean test.
  void PromoteEvidence(const World& world, uint32_t tick, int minEighths);
  // W-D: the free slot a new probe takes, or kWaterDiscoveredCap if the registry
  // is full of live entries.
  uint32_t FreeDiscoverSlot() const;
  // W3: resolve the pending queue into gpu_.impulses and clear it. Called from
  // Tick() after BuildGpu, so a caller cannot forget it and cannot run it twice.
  void BuildImpulses();

  std::vector<WaterBasin> basins_;
  std::vector<WaterBasinCurve> curves_;   // parallel to basins_
  std::vector<WaterBodyDesc> bodies_;     // parallel to basins_
  std::vector<WaterBodyDesc> children_;   // M5: split children, NOT parallel
  std::vector<uint32_t> chunkBody_;
  // Keyed by basinId, NOT parallel to bodies_ — a window rebuild renumbers the
  // descriptors and a hint that followed an index would jump between lakes.
  // Kept OUT of WaterBodyDesc on purpose: the descriptor is what gate pass G
  // recomputes from voxels and asserts equality on, and a field recording
  // "which tick a dig happened" is not recomputable from a world state.
  std::vector<WaterBodyHole> holeHints_;
  WaterBodyGpu gpu_;
  IVec3 builtOrigin_{1 << 30, 1 << 30, 1 << 30};
  uint32_t builtSeed_ = 0;
  uint32_t straddles_ = 0;
  uint32_t outOfWindow_ = 0;
  int mode_ = 0;
  // M5, component 10's CHEAP DETECTION. Keyed by basin id (not by index — a
  // window rebuild renumbers the descriptors and a flag that followed an index
  // would re-derive the wrong lake), value = the tick the dig is forgotten on.
  // A basin whose curve is dirty proposes a split child and takes a slot in the
  // sweep schedule; every other basin does neither, which is where the zero
  // idle cost comes from.
  std::vector<std::pair<uint32_t, uint32_t>> curveDirtyUntil_;
  // THE DRAIN HOT WINDOW (M3). A discharge makes the shave fire, the shave
  // writes RESTING voxels, and a write into a chunk the page table was never
  // told about is a lost eighth reported as a page fault. So the footprint has
  // to be declared as an op target BEFORE the command buffer exists — and the
  // CPU cannot ask whether a hole exists, because a hole is a GPU fact derived
  // from a level the GPU owns.
  //
  // What the CPU CAN see is the thing that MAKES holes: a mutation. Holes
  // appear when someone digs or explodes, and every one of those arrives
  // through the mutation queue on the tick input stream. So any edit at all
  // opens a window during which a governed body declares its footprint, and
  // outside that window a still lake declares nothing and materializes nothing
  // — M2's zero-idle-cost property, kept.
  //
  // It is a LATCH, not a per-tick test, for the reason the frame-local-value
  // gotcha records: the drain outlives the dig by minutes, and a footprint
  // declared only on the tick of the dig would fault on every tick after it.
  // 900 ticks is 30 s, comfortably longer than a bounded drain and bounded by
  // construction (rule 2) rather than by hoping the world goes quiet.
  //
  // KNOWN M3 COST, named rather than discovered: inside the window a governed
  // body materializes its whole footprint, not just the two Y layers the shave
  // can write. Narrowing it to the band needs the CPU to know the live level,
  // which is exactly what M2 moved onto the GPU. See PLAN_water_master.md
  // §1.3's open items.
  uint32_t drainHotUntil_ = 0;
  // ---- W-D ----------------------------------------------------------------
  std::vector<WaterDiscovered> discovered_;
  std::vector<WaterEvidenceCell> evidence_;
  // WHY A GENERATION COUNTER AND NOT A REBUILD EVERY TICK. `basins_` is rebuilt
  // only when the window or the seed moves, because labelling is
  // O(basins x footprint chunks) and paying it per tick would be rule 2 with the
  // sign flipped. A probe raised mid-run has to get into `basins_` all the same,
  // so it bumps this and Tick() treats it exactly like a window move. Discovery
  // events are rare by construction (a threshold crossing, an eviction, a
  // release expiry), so this is the cheap half of the trade and not a loophole
  // in it.
  uint32_t discoveredGen_ = 0;
  uint32_t builtDiscoveredGen_ = 0xFFFFFFFFu;
  // Set when the evidence or the registry moved; cleared by PromoteEvidence.
  // The clustering scan is O(cells^2) at kWaterEvidenceCap = 192, which is
  // nothing on the ticks it runs and would be a standing cost on the ticks it
  // does not.
  bool evidenceDirty_ = false;
  uint32_t discoverEvictions_ = 0;
  // ---- W3 (§5) ------------------------------------------------------------
  // Queued this tick, consumed and cleared by Tick(). Insertion order, no
  // expiry, no persistence: a disturbance is one tick's push.
  // `pendingImp_` takes this tick's arrivals; `armedImp_` is what Tick() ships
  // — LAST tick's, because an impulse is spent against the column heights the
  // previous tick measured (see BuildImpulses). One tick latent, and a record
  // is shipped exactly once.
  std::vector<WaterImpulse> pendingImp_;
  std::vector<WaterImpulse> armedImp_;
  uint32_t impRefused_ = 0;
};

// ---- W3: the two EMITTERS, so the conversion lives in ONE place -----------
//
// Both turn a game-side event into the WaterImpulse above, and both are here
// rather than at their call sites for the reason every "two places must agree"
// note in this repo gives: the blast is emitted from SubmitTick (which sees
// every explosion in the engine, game and harness alike) and the wake from the
// player and mob updates, and a gate that built its own record by hand would be
// testing arithmetic the game does not run.
//
// `knob` is sim.waveBlastImpulse / sim.waveSwimWake. At 0 both return false
// having queued nothing, which is the documented identity: no record means
// `waterImpulseCount` stays 0 and `wbFlux` is bit-identical.
bool WaterBodyNoteBlast(WaterBodySystem& wb, const ExplosionOp& e, int knob);
// `velVoxPerTick` is the swimmer's horizontal velocity in VOXELS PER TICK, and
// `submersion` is the 0..1 fraction of the body under the surface that
// player.cpp already computes for drag and thrust. Below a quarter submerged
// there is no wake: a wader's ankles are not a swimmer.
//
// The float->integer transcription happens HERE, on the CPU, exactly as
// WindPrimAim's does — the sim never sees an f32 (rule 1).
bool WaterBodyNoteSwimmer(WaterBodySystem& wb, int cellX, int cellZ,
                          float velX, float velZ, float submersion, int knob);

// THE live system, a global for exactly the reason WindPrims() is one: it has
// to reach the frame loop, every selftest gate and both smoke harnesses, and
// any one of those that did not get it passed would describe a world with no
// lakes in it. SubmitTick advances it — once per sim tick, from the one place
// the game and the harnesses share — so nothing can advance it twice or skip it.
//
// Every path leaves it EMPTY at `sim.waterBodyMode` 0, and empty is an exact
// identity all the way down: the pinned world hash is untouched by the
// existence of this file.
WaterBodySystem& WaterBodies();

}  // namespace sandvox
