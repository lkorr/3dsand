// selftest_gas.cpp — the gas-particle gates (docs/PLAN_gas_particles.md §4).
//
// WHAT THESE TWO GATES ARE FOR
//
// Stage 1 turned the residency window's edge from a WALL into a SINK. Before
// it, a gas voxel whose buoyancy intent pointed out of the window got the same
// `false` from `tryMove` that a stone wall gives it, fell through to the
// lateral ring, and SHEETED across the entire top chunk plane — up to 1,024
// chunks — until it decayed. After it, the voxel deletes itself and becomes a
// parcel that keeps rising outside.
//
//   `gas-leave`    the sheet is gone, the parcels exist, they are ABOVE the
//                  window, the outer density box sees them, the per-tick
//                  conversion cap was never the bound, and two runs of the
//                  same fixture agree tick for tick.
//   `gas-reenter`  a parcel blown back in becomes a smoke VOXEL again, so it
//                  rejoins the reaction system, and the population drains.
//
// ---- THE ANTI-CIRCULARITY RULE THIS FILE IS BUILT AROUND -------------------
//
// "The top chunk plane has <= 8 awake chunks" is trivially true in a world
// where no smoke ever reached the ceiling — which is also what a broken
// fixture looks like (the puff decayed on the way up, the column was solid,
// the window moved). So the sheet assertion is never made alone: the same run
// must ALSO show `edge hits > 0` and `leave accepted > 0`, i.e. that smoke
// genuinely arrived at the window face and genuinely left through it. A gate
// that can only fail one way is measuring itself
// (memory: gotcha-a-fixture-that-measures-itself, gotcha-circular-probe-assertion).
//
// ---- WHY THE INSTRUMENTATION IS THE SNAPSHOT AND NOT A BLOCKING READ -------
//
// Everything sampled per tick comes off `world.Snap()` — the async ring, one
// tick latent, which carries the whole gas counter block (gasCount,
// gasLeaveAccepted/Refused, gasEdgeHits, gasPoolRefused, gasReentered, gasDied,
// gasAboveWindow) plus `dirtyFlags` and `worldHash`. A blocking readback
// BETWEEN SubmitTick calls stalls the page table's free probe for the life of
// the process and surfaces two dozen gates later in `page-roundtrip`
// (memory: gotcha-midrun-sync-read-stalls-free-probe). The two synchronous
// reads these gates do make — the outer density box and the parcel page — are
// taken at a hard `WaitIdle` + `ProcessEvents` boundary between two segments of
// the run, never with a probe map in flight.
//
// Thresholds live in tests/baseline.json (gasLeave* / gasReenter*), so retuning
// what counts as a sheet costs a JSON edit and no rebuild.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gpu/resources.h"
#include "sim/renderspec.h"
#include "sim/tuning.h"
#include "sim/wind.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t MatId(const std::vector<MaterialDef>& mats, const char* name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (uint32_t)i;
  return 0;
}

// One tick's worth of the gas counter block, as the snapshot delivers it.
// Keyed by the snapshot's OWN tick, because the ring is latent and may skip:
// a series indexed by loop iteration would silently attribute one tick's
// numbers to another.
struct Sample {
  uint32_t live = 0;        // parcels alive after resolve
  uint32_t accepted = 0;    // voxels converted at the window edge this tick
  uint32_t refused = 0;     // conversions refused: the per-tick list was full
  uint32_t poolFull = 0;    // spawns dropped: the parcel pool was full
  uint32_t edge = 0;        // gas voxels whose intent pointed out of the window
  uint32_t above = 0;       // parcels above the window's top face
  uint32_t reentered = 0;   // parcels that became voxels
  uint32_t died = 0;        // decay / outer box / ceiling
  uint32_t topPlane = 0;    // awake chunks in the window's TOP chunk plane
  uint32_t active = 0;      // awake chunks, whole window
  uint32_t hash = 0;        // world hash for this tick
};

// Awake chunks in one chunk-Y plane of the residency window. `dirtyFlags` is
// indexed by SLOT (an identity), so the plane is walked in world chunk coords
// and mapped through World::SlotChunkIndex — never by assuming the slot layout
// (CLAUDE.md: world coords for logic, slot index for memory).
uint32_t PlaneAwake(const std::vector<uint8_t>& dirty, IVec3 wo, int planeCy) {
  if (dirty.size() < kNumChunks) return 0;
  uint32_t n = 0;
  for (int cz = wo.z; cz < wo.z + (int)kNChunk; cz++)
    for (int cx = wo.x; cx < wo.x + (int)kNChunk; cx++)
      if (dirty[World::SlotChunkIndex({cx, planeCy, cz})]) n++;
  return n;
}

// ============================ gas-leave =====================================
//
// THE FIXTURE (plan §4). A 16^3 block of smoke — 4,096 voxels — six chunks
// below the window's top face, with the column above it cleared to air by
// mutation so the plume has open sky to climb and the measurement cannot pick
// up whatever worldgen left in the way. Everything is anchored to
// `world.WindowOrigin()`, never to a literal world position: by the time this
// gate runs the window has walked (memory: gotcha-gate-hardcodes-fixture-site).
//
// 400 ticks, twice, with the SAME fixture. The two runs are compared tick for
// tick on the world hash, and on the gas digest and live count at the end —
// the digest is the only thing that can see a parcel outside the window at all,
// since it touches no voxel and the world hash therefore cannot cover it.
//
// PAGED vs DENSE IS NOT A GATE-SCOPE DIFFERENTIAL. `--residency dense` is a
// process-level flag (World::Residency is fixed at Init); no gate in this
// harness switches it mid-run, and one that tried would be re-pointing every
// other gate's page table underneath it. The dense arm is run by giving the
// binary `--residency dense`, which is how `determinism` gets it too. What is
// asserted here is the twice-run equality, and this comment is the "and say
// so" the plan asks for.
struct LeaveRun {
  std::map<uint32_t, Sample> series;   // by snapshot tick
  uint64_t refused = 0, poolFull = 0;  // summed over every tick the ring gave
  uint64_t accepted = 0, edge = 0;
  uint32_t peakLive = 0;
  uint32_t liveEnd = 0, digest = 0;
  uint32_t coverage = 0;               // ticks the snapshot ring actually spoke
  // Filled by the mid-run probe (run A only): the outer density box folded over
  // everything at or above the window's top face.
  uint32_t outerMax = 0;
  uint64_t outerSum = 0;
  uint32_t aboveProbe = 0, liveProbe = 0;
  // THE DIGEST IS READ AT THE PROBE TICK, NOT AT THE END, and that is the whole
  // difference between a claim and a tautology. By t400 every parcel is dead of
  // its own decay, so the end-of-run digest is 0 in both arms and "0 == 0"
  // proves nothing about the motion out there
  // (memory: gotcha-absolute-zero-is-a-rate-claim). At the probe tick the
  // population is at its most interesting and the sum of its priorities is the
  // only quantity in the engine that can see it — a parcel outside the window
  // touches no voxel, so the world hash cannot.
  uint32_t digestProbe = 0, liveAtProbe = 0;
  // ---- THE SETTLED-TICK SKIP vs PARCELS IN FLIGHT --------------------------
  // DESIGN.md §5 states the rule — "a new GPU-side particle source must land
  // inside that count" — because `Simulation::NoteSnapshot` licenses the CA
  // skip on `activeChunks == 0 && particleCount == 0`, and a gas parcel is a
  // GPU-side dirty-writer whose landing cell the CPU cannot predict.
  //
  // WHEN THIS COUNTER WAS WRITTEN gas was in neither `particleCount` nor
  // `inputsThisTick`, and it was printed rather than asserted because the fix
  // belonged in a file that package did not own. It measured 0 of 400 — which
  // was this fixture never ENTERING the window, not the hole being closed.
  // Naming it is what got it closed: `NoteSnapshot` disqualifies on
  // `gasLive_` now, `NoteGasLive` clears `settledProven_` directly so the fix
  // does not depend on call order, and `gasSpawnsThisTick_` is in
  // `inputsThisTick`.
  //
  // STILL REPORTED RATHER THAN ASSERTED, deliberately. The disqualifier reads
  // the SNAPSHOT's parcel count, which is latent by construction, so a stale
  // zero can in principle still license a skip on the tick a parcel is born —
  // a residual window that is bounded but not provably empty, and a gate that
  // failed on it would be flaky rather than informative. What changed is the
  // MEANING of a non-zero reading: it was an expected condition, and it is a
  // finding now.
  uint32_t skipWithParcels = 0, skipFirstTick = 0, skipParcelsThere = 0;
  uint32_t quietWithParcels = 0;
  // ---- stage 1b: the crossfade's two live wires ---------------------------
  // Both of these are things the render path needs and that NOTHING ELSE in
  // this gate would notice the absence of -- a broken latch or a dead splat
  // leaves every number above untouched and simply turns the crossfade off.
  //
  // renderFlagTicks: ticks on which Simulation::EncodeTick published the
  // "gas may be present" flag (RenderParams bit 3). Zero means the flag never
  // arms and the whole of the crossfade is dead code in every frame.
  //
  // outerInWindow: gasOuter's content INSIDE the window, which only the CA's
  // gasOuterSplat can put there (a parcel only exists outside). Zero means the
  // coarse side has nothing for the voxels to fade INTO, which is exactly the
  // hard edge stage 1b exists to remove.
  uint32_t renderFlagTicks = 0;
  uint64_t outerInWindow = 0;
  uint32_t outerInWindowMax = 0;
};

Status GateGasLeave(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const uint32_t smokeId = MatId(c.mats, "smoke");
  if (!smokeId) {
    detail = "need material smoke";
    return Status::Fail;
  }

  // Thresholds: JSON, not C++ (CLAUDE.md "Authoring cheap-to-verify work").
  const uint32_t sheetMax = (uint32_t)BaselineNumber("gasLeaveSheetMax", 8);
  const uint32_t sheetTick = (uint32_t)BaselineNumber("gasLeaveSheetTick", 150);
  const uint32_t firstTick = (uint32_t)BaselineNumber("gasLeaveFirstTick", 100);
  const uint32_t probeTick = (uint32_t)BaselineNumber("gasLeaveProbeTick", 200);
  const uint32_t splatTick = (uint32_t)BaselineNumber("gasLeaveSplatTick", 20);
  const uint32_t ticks = (uint32_t)BaselineNumber("gasLeaveTicks", 400);

  const IVec3 wo = world.WindowOrigin();
  const int topY = wo.y * (int)kChunk + (int)kWorldN;  // one past the top face
  const int topCy = wo.y + (int)kNChunk - 1;           // the top chunk plane
  // Six chunks below the top face, centred in x/z so the plume leaves through
  // the CEILING rather than out of a side face — the sheet this gate exists to
  // measure is the top plane's.
  const int fy0 = topY - 6 * (int)kChunk;
  const int fx0 = wo.x * (int)kChunk + (int)kWorldN / 2 - 8;
  const int fz0 = wo.z * (int)kChunk + (int)kWorldN / 2 - 8;

  // The shaft: air from the slab's base to the last cell under the top face,
  // four cells of margin around the slab so an early lean does not clip. 24 x
  // 24 x 96 = 55,296 cells, inside kMaxCellOpsPerTick (65,536) with the 4,096
  // smoke ops still to come on the NEXT tick — two ticks rather than one,
  // because ordering WITHIN a tick's op list is not something to lean on.
  std::vector<CellOp> clear;
  clear.reserve(24 * 24 * 96);
  for (int z = fz0 - 4; z < fz0 + 20; z++)
    for (int x = fx0 - 4; x < fx0 + 20; x++)
      for (int y = fy0; y < topY; y++)
        clear.push_back({World::SlotCellIndex({x, y, z}), 0u});
  std::vector<CellOp> puff;
  puff.reserve(4096);
  for (int z = fz0; z < fz0 + 16; z++)
    for (int x = fx0; x < fx0 + 16; x++)
      for (int y = fy0; y < fy0 + 16; y++)
        puff.push_back({World::SlotCellIndex({x, y, z}), smokeId & 0xFFFu});

  auto run = [&](int arm) -> LeaveRun {
    LeaveRun r;
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const std::vector<CellOp> none;
    uint32_t t = 0;
    for (uint32_t i = 1; i <= ticks; i++) {
      const std::vector<CellOp>& ops = i == 1 ? clear : (i == 2 ? puff : none);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, ops, true,
                 {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
      // Snapshot only — no blocking read inside the loop.
      const WorldSnapshot& sn = world.Snap();
      // CaSkipped() is a CPU-side latch read (measurement only, per its
      // declaration), not a readback, so it is free here. Paired with the
      // snapshot's own gas count rather than with `sn.activeChunks` alone,
      // because the claim is about the two together.
      if (sn.valid && sim.CaSkipped() && sn.gasCount > 0) {
        if (!r.skipFirstTick) {
          r.skipFirstTick = sn.tick;
          r.skipParcelsThere = sn.gasCount;
        }
        r.skipWithParcels++;
      }
      if (sn.valid && sn.activeChunks == 0 && sn.gasCount > 0)
        r.quietWithParcels++;
      // A CPU-side latch read, same cost and same class as CaSkipped() above.
      if (sandvox::GasRenderActive()) r.renderFlagTicks++;
      if (sn.valid && r.series.find(sn.tick) == r.series.end()) {
        Sample s;
        s.live = sn.gasCount;
        s.accepted = sn.gasLeaveAccepted;
        s.refused = sn.gasLeaveRefused;
        s.poolFull = sn.gasPoolRefused;
        s.edge = sn.gasEdgeHits;
        s.above = sn.gasAboveWindow;
        s.reentered = sn.gasReentered;
        s.died = sn.gasDied;
        s.topPlane = PlaneAwake(sn.dirtyFlags, wo, topCy);
        s.active = sn.activeChunks;
        s.hash = sn.worldHash;
        r.refused += s.refused;
        r.poolFull += s.poolFull;
        r.accepted += s.accepted;
        r.edge += s.edge;
        r.peakLive = std::max(r.peakLive, s.live);
        r.series[sn.tick] = s;
      }
      // THE MID-RUN PROBE, and it is a SEGMENT BOUNDARY rather than a read in
      // the loop. Everything is drained first, so no free-probe map is in
      // flight when the blocking readbacks below run
      // (memory: gotcha-midrun-sync-read-stalls-free-probe).
      //
      // BOTH arms take it, because one of the four values — the gas digest — is
      // half of the twice-run comparison and a value only one arm measures
      // cannot be compared. The other three are arm 0's alone; a readback
      // cannot change what the sim does, so the control does not need them.
      // ---- THE CA SPLAT PROBE, and it is EARLY on purpose --------------
      // gasOuter's content INSIDE the window can only have been put there by
      // sim_step's gasOuterSplat: a parcel exists only outside. But it has to
      // be read while there IS in-window smoke, and by the main probe at t200
      // this fixture's plume has entirely left through the ceiling -- the
      // first version of this check read 0 there and reported a dead splat on
      // a run in which it had worked perfectly for a hundred ticks. The puff
      // lands at t2 and rises about a voxel a tick from 96 below the face, so
      // t20 is deep inside the window with the whole plume still in it.
      //
      // Arm 0 only, and a segment boundary like the probe below: a readback
      // cannot change what the sim does, so the control does not need it.
      if (i == splatTick && arm == 0) {
        ctx.WaitIdle();
        ctx.ProcessEvents();
        uint32_t bandMax = 0, aboveMax = 0;
        uint64_t bandSum = 0, aboveSum = 0;
        // A difference of two "above Y" folds, because that is the reader that
        // exists. Both Y values are whole multiples of the cell size from the
        // box origin, so no cell is split between the two halves.
        ReadGasOuterAboveSync(ctx, world, fy0, &bandMax, &bandSum);
        ReadGasOuterAboveSync(ctx, world, topY, &aboveMax, &aboveSum);
        r.outerInWindow = bandSum - aboveSum;
        r.outerInWindowMax = bandMax;
      }
      if (i == probeTick) {
        ctx.WaitIdle();
        ctx.ProcessEvents();
        uint32_t gp[kGasSpHdr] = {};
        ReadGasStatsSync(ctx, world, gp);
        r.digestProbe = gp[kGasSpDigest];
        if (arm == 0) {
          ReadGasOuterAboveSync(ctx, world, topY, &r.outerMax, &r.outerSum);
          r.aboveProbe = GasAboveYSync(ctx, world, sim, topY, &r.liveProbe);
        } else {
          r.liveProbe = GasAliveSync(ctx, world, sim);
        }
        r.liveAtProbe = r.liveProbe;
      }
    }
    ctx.WaitIdle();
    ctx.ProcessEvents();
    uint32_t gs[kGasSpHdr] = {};
    ReadGasStatsSync(ctx, world, gs);
    r.digest = gs[kGasSpDigest];
    r.liveEnd = GasAliveSync(ctx, world, sim);
    r.coverage = (uint32_t)r.series.size();
    return r;
  };

  const LeaveRun a = run(0);
  const LeaveRun b = run(1);

  // ---- the assertions ------------------------------------------------------

  // 1. THE SHEET IS GONE, and 1b. it had something to be gone FROM. The second
  //    half is what stops this being a gate that cannot fail: no smoke at the
  //    ceiling means no sheet either way.
  const auto sheetIt = a.series.lower_bound(sheetTick);
  const bool sawSheetTick = sheetIt != a.series.end();
  const uint32_t sheet = sawSheetTick ? sheetIt->second.topPlane : 0u;
  const bool sheetOk = sawSheetTick && sheet <= sheetMax;
  const bool reachedEdge = a.edge > 0 && a.accepted > 0;

  // 2. Parcels exist by `firstTick`.
  uint32_t liveByFirst = 0;
  for (const auto& kv : a.series) {
    if (kv.first > firstTick) break;
    liveByFirst = std::max(liveByFirst, kv.second.live);
  }
  const bool spawned = liveByFirst > 0;

  // 3. MONOTONE NON-INCREASING ONCE EMISSION STOPS. "Emission stops" is read
  //    off the data rather than assumed from a tick number: the last tick that
  //    accepted a conversion. Two ticks of grace after it, because the spawn
  //    list is drained on the NEXT tick (one-tick latent, like every op stream)
  //    and the snapshot ring is latent again on top of that.
  uint32_t lastEmit = 0;
  for (const auto& kv : a.series)
    if (kv.second.accepted > 0) lastEmit = kv.first;
  uint32_t rises = 0, riseAt = 0, risePrev = 0, riseNow = 0;
  {
    bool have = false;
    uint32_t prev = 0;
    for (const auto& kv : a.series) {
      if (kv.first <= lastEmit + 2) continue;
      if (have && kv.second.live > prev) {
        if (!rises) { riseAt = kv.first; risePrev = prev; riseNow = kv.second.live; }
        rises++;
      }
      prev = kv.second.live;
      have = true;
    }
  }
  const bool monotone = rises == 0;

  // 4. There are parcels ABOVE the window at the probe tick, by two independent
  //    readings: the GPU's own per-tick count (kGasSpAbove, off the snapshot)
  //    and a CPU walk of the parcel page. Two derivations of one quantity, not
  //    one derivation checked against itself.
  const auto probeIt = a.series.find(probeTick);
  const uint32_t aboveSnap =
      probeIt != a.series.end() ? probeIt->second.above : 0u;
  const bool aboveOk = a.aboveProbe > 0 || aboveSnap > 0;

  // 5. The outer density box has something in it above the window top.
  const bool outerOk = a.outerSum > 0;

  // 6. THE PER-TICK CAP WAS NEVER THE BOUND. kGasSpawnPerTick refusals are
  //    scheduling-dependent in WHICH voxel gets refused (a shared atomicAdd
  //    cursor), and a refused voxel STAYS IN THE GRID where the world hash can
  //    see it — so a nonzero here is not a slow plume, it is a determinism
  //    hazard. Summed over every tick the snapshot ring delivered; `coverage`
  //    says how many that was, because "0 refusals over 12 of 400 ticks" and
  //    "0 over 400" are different claims (CLAUDE.md rule 6).
  const bool noRefusals = a.refused == 0 && a.poolFull == 0 &&
                          b.refused == 0 && b.poolFull == 0;

  // 7. Twice-run equality: the hash series tick for tick, plus the gas digest
  //    and the surviving population, which are the only things that can see a
  //    parcel outside the window.
  uint32_t divergedAt = 0;
  uint32_t compared = 0;
  for (const auto& kv : a.series) {
    const auto it = b.series.find(kv.first);
    if (it == b.series.end()) continue;
    compared++;
    if (it->second.hash != kv.second.hash && !divergedAt) divergedAt = kv.first;
  }
  // The digest comparison is made where there is a population to digest. A
  // nonzero live count at the probe tick is part of the claim, not a
  // precondition to it: "0 == 0" would pass every arm of every future change.
  const bool gasSame = a.digestProbe == b.digestProbe &&
                       a.liveAtProbe == b.liveAtProbe && a.liveAtProbe > 0 &&
                       a.liveEnd == b.liveEnd;
  const bool stable = divergedAt == 0 && compared > 0 && gasSame;

  // Recorded under `...Observed` keys, NEVER under the threshold's own key: a
  // gate that writes its measurement back over its bound would let
  // --rebaseline quietly ratify a sheet that came back.
  RecordObserved("gasLeave.sheetObserved", (double)sheet);
  RecordObserved("gasLeave.peakParcelsObserved", (double)a.peakLive);
  RecordObserved("gasLeave.convertedObserved", (double)a.accepted);
  RecordObserved("gasLeave.outerSumObserved", (double)a.outerSum);
  RecordObserved("gasLeave.caSkipWithParcelsObserved", (double)a.skipWithParcels);

  // See LeaveRun::skipWithParcels. This is DESIGN.md §5's "a new GPU-side
  // particle source must land inside that count" measured rather than argued
  // about — and measuring it is what got the hole in simulation.cpp closed.
  // Its own printf so it is legible whether the gate passes or fails.
  std::printf(
      "gas-leave: settled-tick skip vs parcels in flight: CA rows SKIPPED on "
      "%u ticks with parcels alive (first t%u, %u parcels), 0 active chunks on "
      "%u such ticks -- gasLive_ is in NoteSnapshot's disqualifier and "
      "gasSpawnsThisTick_ is in inputsThisTick now, so this should read 0; the "
      "residual is a stale-zero snapshot on the tick a parcel is born. "
      "REPORTED, not asserted (a latent count cannot carry a hard assert), but "
      "a non-zero value here is a FINDING.\n",
      a.skipWithParcels, a.skipFirstTick, a.skipParcelsThere,
      a.quietWithParcels);

  // Stage 1b's two live wires (see LeaveRun). Both are hard asserts and not
  // reports: neither is latent, neither is a threshold, and a zero in either
  // means the crossfade is not running at all.
  const bool flagOk = a.renderFlagTicks > 0;
  const bool splatOk = a.outerInWindow > 0;

  detail = Format(
      "top chunk plane t%u: %u awake (max %u) | edge hits %llu, converted %llu "
      "| parcels %u by t%u, peak %u, %u left at t%u | above the window t%u: %u "
      "(GPU) / %u (page walk, of %u live) | gasOuter above the top: max %u, "
      "sum %llu | in-window at t%u: sum %llu max %u (the CA splat) | render "
      "flag on %u/%u ticks | refusals %llu list + %llu pool over %u/%u "
      "snapshot ticks | "
      "twice-run %s over %u hashed ticks, digest at t%u %08x vs %08x over %u "
      "vs %u live parcels, live at t%u %u vs %u | paged "
      "vs dense is a --residency arm, not a gate arm (see the file comment)",
      sheetTick, sheet, sheetMax, (unsigned long long)a.edge,
      (unsigned long long)a.accepted, liveByFirst, firstTick, a.peakLive,
      a.liveEnd, ticks, probeTick, aboveSnap, a.aboveProbe, a.liveProbe,
      a.outerMax, (unsigned long long)a.outerSum,
      splatTick, (unsigned long long)a.outerInWindow, a.outerInWindowMax,
      a.renderFlagTicks, ticks,
      (unsigned long long)(a.refused + b.refused),
      (unsigned long long)(a.poolFull + b.poolFull), a.coverage, ticks,
      stable ? "identical" : "DIVERGED", compared, probeTick, a.digestProbe,
      b.digestProbe, a.liveAtProbe, b.liveAtProbe, ticks, a.liveEnd, b.liveEnd);

  if (!sawSheetTick)
    detail += Format(" -- no snapshot at or after t%u (the ring never spoke)",
                     sheetTick);
  else if (!sheetOk)
    detail += " -- the top chunk plane is still SHEETING";
  if (!reachedEdge)
    detail +=
        " -- no smoke reached the window face at all: the fixture measured "
        "itself, not the sink";
  if (!spawned) detail += Format(" -- no parcels by t%u", firstTick);
  if (!monotone)
    detail += Format(" -- the population GREW after emission stopped (t%u: %u "
                     "-> %u, %u rises; last conversion t%u)",
                     riseAt, risePrev, riseNow, rises, lastEmit);
  if (!aboveOk) detail += " -- nothing is above the window top";
  if (!outerOk) detail += " -- gasOuter is empty above the window top";
  if (!flagOk)
    detail +=
        " -- the gas RENDER flag never armed: RenderParams bit 3 is off on "
        "every tick of a run with a live plume, so the whole crossfade is dead "
        "code (Simulation::EncodeTick / renderspec.h)";
  if (!splatOk)
    detail +=
        Format(" -- gasOuter is EMPTY inside the window at t%u, with the "
               "whole plume still in it: the CA's gasOuterSplat put nothing "
               "there, so the voxels have nothing to crossfade into and the "
               "window face is a hard edge again", splatTick);
  if (!noRefusals)
    detail +=
        " -- spawn refusals are NONZERO: which voxel is refused is decided by "
        "which workgroup arrived first, and a refused voxel stays in the grid";
  if (!stable)
    detail += divergedAt ? Format(" -- twice-run hash diverged at t%u", divergedAt)
                         : " -- the gas digest / population did not reproduce";

  const bool ok = sheetOk && reachedEdge && spawned && monotone && aboveOk &&
                  outerOk && noRefusals && stable && flagOk && splatOk;
  std::printf("gas-leave: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  // Leave the world as it was found: the gates after this one place fixtures on
  // pristine terrain (CLAUDE.md rule 7). The parcel pool drains on its own —
  // every parcel is bounded by its decay and the outer box — but say what is
  // left rather than assuming.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ============================ gas-reenter ===================================
//
// The other direction across the seam. 256 parcels are queued from the CPU two
// cells OUTSIDE the window's low-X face, high in open sky, with a wind blowing
// inward; each must cross the face and reconvert to a `smoke` VOXEL through the
// ordinary atomicMax claim, and the population must then drain to zero.
//
// TWO INDEPENDENT READINGS of the same landing, on purpose: the GPU's own
// `gasReentered` counter, and a CPU census of `smoke` voxels standing in the
// entry box. A counter checked against itself cannot fail
// (memory: gotcha-circular-probe-assertion); the grid is the consumer's side.
//
// The spawns go through `World::QueueGasSpawns` — rule 3: a CPU-authored parcel
// is an INPUT OP on the per-tick stream, on the same footing as a BrushOp, and
// nothing here writes a particle buffer.
Status GateGasReenter(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const uint32_t smokeId = MatId(c.mats, "smoke");
  if (!smokeId) {
    detail = "need material smoke";
    return Status::Fail;
  }
  if (c.mats[smokeId].windResponse == 0) {
    detail = "smoke authors windResponse 0 - nothing can blow a parcel inward";
    return Status::Fail;
  }

  const uint32_t nSpawn = (uint32_t)BaselineNumber("gasReenterParcels", 256);
  // THE WINDOW IS 20 TICKS AND IT IS ALSO THE WHOLE RUN, for a reason worth
  // writing down. The first version ran 40 further "drain" ticks after the
  // deadline and failed itself twice over: the re-entered smoke, which is a
  // rising gas voxel like any other, climbed ~2 cells a tick, left the census
  // box, and then reached the window's own top face and became parcels AGAIN —
  // so "0 smoke voxels" and "83 parcels alive at the end" were both the sink
  // working correctly, measured 60 ticks after the event this gate is about.
  // Measure where the subject is.
  const uint32_t deadline = (uint32_t)BaselineNumber("gasReenterTicks", 20);
  const uint32_t minLand = (uint32_t)BaselineNumber("gasReenterMinLanded", 128);

  const IVec3 wo = world.WindowOrigin();
  const int xIn = wo.x * (int)kChunk;                   // first in-window x
  // Eight chunks below the window's top face: open sky, well clear of both the
  // ceiling sink (a parcel that reached it would leave again) and the ground.
  const int yBase = wo.y * (int)kChunk + (int)kWorldN - 128;
  const int zBase = wo.z * (int)kChunk + (int)kWorldN / 2;

  // The landing box, cleared to air by mutation so re-entry has somewhere to
  // land — `gasResolve` only converts into a cell that reads MAT_AIR, so a
  // fixture that skipped this would be measuring worldgen. 32 x 48 x 32 =
  // 49,152 ops, inside kMaxCellOpsPerTick.
  std::vector<CellOp> clear;
  clear.reserve(32 * 48 * 32);
  for (int z = zBase; z < zBase + 32; z++)
    for (int x = xIn; x < xIn + 32; x++)
      for (int y = yBase; y < yBase + 48; y++)
        clear.push_back({World::SlotCellIndex({x, y, z}), 0u});

  // Wind: hard and steady, blowing +x (dirDeg 90, as the `wind-gas` gate's east
  // arm establishes). weatherAuto off so the field does not evolve under the
  // measurement; gusts down because their vertical component only adds noise to
  // a claim about the X axis.
  const Tuning saved = CurrentTuning();
  {
    Tuning t = saved;
    t.sim.windMode = (int)kWindModeDrift;
    t.wind.weatherAuto = false;
    t.wind.windDirDeg = 90.0f;
    t.wind.windSpeed = 20.0f;
    t.wind.gustStrength = 0.1f;
    SetCurrentTuning(t);
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  const std::vector<CellOp> none;
  uint32_t t = 0;
  // Tick 1 clears the landing box.
  SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, clear, false,
             {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);

  // Tick 2 carries the parcels. 16 x 16 distinct cells at x = xIn - 2: distinct
  // because the identity key is derived from the CELL and the payload, so
  // parcels sharing both roll identically and move as one — which is correct
  // behaviour and useless as a sample.
  std::vector<GasSpawnOp> ops;
  ops.reserve(nSpawn);
  for (uint32_t i = 0; i < nSpawn; i++) {
    const int dy = (int)(i / 16), dz = (int)(i % 16);
    ops.push_back(MakeGasSpawn(xIn - 2, yBase + 4 + dy, zBase + 8 + dz, smokeId));
  }
  const uint32_t accepted = world.QueueGasSpawns(ops.data(), (uint32_t)ops.size());
  // Queued, not yet uploaded: the ops sit on World until the NEXT SubmitTick
  // drains them into gasSpawnOps (one-tick latent, like every op stream). So
  // the queue depth is read twice — full here, and empty after the drain tick
  // below. Reading it once, before the drain, reports 256 "still pending" and
  // reads as a refusal; that cost this gate its first run.
  const uint32_t pendingBefore = world.PendingGasSpawns();

  uint32_t peakLive = 0, landed = 0, landTick = 0, liveZeroTick = 0;
  uint32_t lastLive = 0, pendingAfter = 0;
  for (uint32_t i = 0; i < deadline; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, none, false,
               {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
    if (i == 0) pendingAfter = world.PendingGasSpawns();
    const WorldSnapshot& sn = world.Snap();
    if (!sn.valid) continue;
    peakLive = std::max(peakLive, sn.gasCount);
    if (sn.gasReentered) {
      landed += sn.gasReentered;
      if (!landTick) landTick = sn.tick;
    }
    lastLive = sn.gasCount;
    if (peakLive > 0 && sn.gasCount == 0 && !liveZeroTick) liveZeroTick = sn.tick;
  }
  ctx.WaitIdle();
  ctx.ProcessEvents();
  const uint32_t liveEnd = GasAliveSync(ctx, world, sim);

  // The independent half of the claim: count the smoke VOXELS the landings
  // made, read off the grid rather than off the counter that reported them.
  // A counter checked against itself is circular; the grid is the consumer's
  // side of the same fact.
  uint32_t smokeVox = 0;
  {
    std::vector<uint32_t> cbuf(kChunkVol);
    for (int cz = zBase >> 4; cz <= (zBase + 31) >> 4; cz++)
      for (int cy = (yBase - 16) >> 4; cy <= (yBase + 63) >> 4; cy++)
        for (int cx = xIn >> 4; cx <= (xIn + 47) >> 4; cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "gasReenterVox");
          for (uint32_t k = 0; k < kChunkVol; k++)
            if ((cbuf[k] & 0xFFFu) == smokeId) smokeVox++;
        }
  }

  SetCurrentTuning(saved);

  const bool queued =
      accepted == nSpawn && pendingBefore == nSpawn && pendingAfter == 0;
  const bool crossed = landed >= minLand && landTick > 0 &&
                       landTick <= 1 + deadline;  // tick 1 was the clear
  // The grid must agree with the counter to within decay. Over `deadline` ticks
  // at smoke's authored 9/1000 a landed voxel survives with p ~= 0.84, so a
  // quarter is a floor nothing but a lying counter can go under. `> 0` alone
  // would let one stray voxel ratify 251 claimed landings.
  const bool becameVoxels = smokeVox > 0 && smokeVox * 4 >= landed;
  // "The gas particle count reaches 0" (plan §4): every parcel that entered
  // became a voxel or died, and nothing is still drifting outside. Both halves
  // — it reached zero DURING the window and is zero at the end — because a
  // population that never rose would satisfy the second alone.
  const bool drained = liveEnd == 0 && liveZeroTick > 0 && peakLive > 0;

  detail = Format(
      "%u/%u parcels queued (%u pending before the drain tick, %u after) | %u "
      "reconverted (floor %u), first at t%u (deadline t%u) | %u smoke voxels "
      "standing in the entry box | peak %u parcels alive, first zero t%u, %u "
      "at t%u (last snapshot %u)",
      accepted, nSpawn, pendingBefore, pendingAfter, landed, minLand, landTick,
      1 + deadline, smokeVox, peakLive, liveZeroTick, liveEnd, 1 + deadline,
      lastLive);

  if (!queued) detail += " -- the CPU spawn queue did not accept and drain the ops";
  if (!crossed)
    detail += Format(" -- the wind did not carry %u parcels across the face "
                     "within %u ticks",
                     minLand, deadline);
  if (!becameVoxels)
    detail += Format(" -- the grid does not agree with the counter: %u smoke "
                     "voxels for %u claimed landings",
                     smokeVox, landed);
  if (!drained) detail += " -- the parcel population did not reach 0";

  RecordObserved("gasReenter.landedObserved", (double)landed);
  RecordObserved("gasReenter.firstLandTickObserved", (double)landTick);
  RecordObserved("gasReenter.smokeVoxObserved", (double)smokeVox);

  const bool ok = queued && crossed && becameVoxels && drained;
  std::printf("gas-reenter: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& GasGates() {
  // NO DEPS, deliberately. Both gates run their own SubmitWorldgen and build
  // their own fixture, so nothing here can be silently SKIPPED behind a
  // known-failing dependency (memory: gotcha-spells-gate-skipped-behind-streaming).
  static const std::vector<Gate> g = {
      {"gas-leave", "sim", {}, false, GateGasLeave},
      {"gas-reenter", "sim", {}, false, GateGasReenter},
  };
  return g;
}

}  // namespace selftest
