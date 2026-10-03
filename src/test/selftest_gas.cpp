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
#include "sim/farplumes.h"
#include "sim/renderspec.h"
#include "sim/tuning.h"
#include "sim/wind.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

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

  // 6. THE LEAVE BUDGET WAS NEVER THE BOUND for a single plume. Since
  //    2026-10-03 a refusal is no longer a determinism hazard -- which voxel
  //    is refused is a function of the world (sim_step.wgsl THE EDGE'S
  //    BUDGET; `gas-leave-overflow` forces it) -- so a nonzero here is a
  //    THROUGHPUT finding: one 16^3 puff should not exhaust a 65,536 budget
  //    split across the few edge chunks this fixture wakes. Summed over every
  //    tick the snapshot ring delivered; `coverage` says how many that was,
  //    because "0 refusals over 12 of 400 ticks" and "0 over 400" are
  //    different claims (CLAUDE.md rule 6).
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
        " -- leave / pool refusals are NONZERO for one plume: the per-chunk "
        "share of the leave budget bound (deterministically) where it should "
        "not have";
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

// ========================= gas-leave-overflow ===============================
//
// THE RULE-1 HOLE THE EDGE HAD, PROVEN CLOSED RATHER THAN MASKED (2026-10-03).
// Until this date a window-edge conversion charged a shared atomicAdd cursor,
// so when the per-tick list filled, WHICH voxels were refused was decided by
// which workgroup arrived first -- and a refused voxel stays in the grid,
// where the world hash sees it. The engine held that off by sizing the list
// (65,536) out of reach, and `gas-leave` asserts it is never reached. That
// proves the hole is not ENTERED; it says nothing about whether it is there.
//
// This gate enters it on purpose. World::SetGasLeaveCapForTest shrinks the
// tick's leave budget (sim_gas.wgsl gasLeavePrep) to a few dozen, a 16^3 puff
// of smoke four cells under the window's top face meets the ceiling within a
// few ticks, and most of every tick's leavers are refused. Two runs of the
// same fixture must then agree tick for tick on the world hash AND on the
// per-tick accepted / refused / budget / edge-chunk counts, and on the parcel
// digest at a probe tick -- the only quantity that can see which voxels left.
//
// WHAT THIS CAN AND CANNOT PROVE. A twice-run comparison on one GPU in one
// process is evidence, not proof: the old cursor would often have produced
// the same arrival order twice too. The proof is structural (sim_step.wgsl
// THE EDGE'S BUDGET: every refusal is a function of the budget, the chunk's
// edge-chunk share and a per-cell hash rank, none of which is an arrival
// order). What the gate pins is that the structure is the one RUNNING: the
// overflow genuinely happens (refused > 0), it is genuinely partial
// (accepted > 0 -- a share of zero would refuse everything deterministically
// and prove nothing), no accepted conversion ever found the list full
// (kGasSpOverrun == 0), no tick accepted more than its budget, and the pool
// never refused a parcel (the budget leaves it room by construction).
//
// Ticks THE tick (support::TickCursor). Thresholds in tests/baseline.json
// (gasOverflow*).
Status GateGasLeaveOverflow(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const uint32_t smokeId = MatId(c.mats, "smoke");
  if (!smokeId) {
    detail = "need material smoke";
    return Status::Fail;
  }
  const uint32_t cap = (uint32_t)BaselineNumber("gasOverflowCap", 64);
  const uint32_t ticks = (uint32_t)BaselineNumber("gasOverflowTicks", 60);
  const uint32_t probeTick = (uint32_t)BaselineNumber("gasOverflowProbeTick", 30);

  const IVec3 wo = world.WindowOrigin();
  const int topY = wo.y * (int)kChunk + (int)kWorldN;  // one past the top face
  // The puff's top is four cells under the face, centred in x/z so it leaves
  // through the CEILING. A shaft of air from its base to the face, four cells
  // of margin, so nothing worldgen left there decides where it goes.
  const int fy0 = topY - 20;
  const int fx0 = wo.x * (int)kChunk + (int)kWorldN / 2 - 8;
  const int fz0 = wo.z * (int)kChunk + (int)kWorldN / 2 - 8;
  std::vector<CellOp> clear, puff;
  for (int z = fz0 - 4; z < fz0 + 20; z++)
    for (int x = fx0 - 4; x < fx0 + 20; x++)
      for (int y = fy0; y < topY; y++)
        clear.push_back({World::SlotCellIndex({x, y, z}), 0u});
  for (int z = fz0; z < fz0 + 16; z++)
    for (int x = fx0; x < fx0 + 16; x++)
      for (int y = fy0; y < fy0 + 16; y++)
        puff.push_back({World::SlotCellIndex({x, y, z}), smokeId & 0xFFFu});
  const IVec3 fixtureChunk{fx0 >> 4, fy0 >> 4, fz0 >> 4};

  struct Tick {
    uint32_t hash = 0, accepted = 0, refused = 0, budget = 0, edgeCh = 0;
    uint32_t overrun = 0, poolFull = 0, live = 0;
    bool operator==(const Tick& o) const {
      return hash == o.hash && accepted == o.accepted && refused == o.refused &&
             budget == o.budget && edgeCh == o.edgeCh && overrun == o.overrun &&
             poolFull == o.poolFull && live == o.live;
    }
  };
  struct Run {
    std::map<uint32_t, Tick> series;
    uint64_t accepted = 0, refused = 0, overrun = 0, poolFull = 0;
    uint32_t overBudgetTicks = 0, overCapTicks = 0, refusingTicks = 0;
    uint32_t maxEdgeCh = 0;
    uint32_t digest = 0, live = 0;
  };

  auto run = [&]() -> Run {
    Run r;
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    world.SetGasLeaveCapForTest(cap);
    uint32_t t = 0;
    support::TickCursor tick{c, t, fixtureChunk};
    for (uint32_t i = 1; i <= ticks; i++) {
      tick(std::vector<BrushOp>{},
           i == 1 ? clear : (i == 2 ? puff : std::vector<CellOp>{}));
      const WorldSnapshot& sn = world.Snap();
      if (sn.valid && r.series.find(sn.tick) == r.series.end()) {
        Tick s;
        s.hash = sn.worldHash;
        s.accepted = sn.gasLeaveAccepted;
        s.refused = sn.gasLeaveRefused;
        s.budget = sn.gasLeaveBudget;
        s.edgeCh = sn.gasLeaveEdgeChunks;
        s.overrun = sn.gasLeaveOverrun;
        s.poolFull = sn.gasPoolRefused;
        s.live = sn.gasCount;
        r.series[sn.tick] = s;
        r.accepted += s.accepted;
        r.refused += s.refused;
        r.overrun += s.overrun;
        r.poolFull += s.poolFull;
        if (s.accepted > s.budget) r.overBudgetTicks++;
        if (s.budget > cap) r.overCapTicks++;
        if (s.refused > 0) r.refusingTicks++;
        r.maxEdgeCh = std::max(r.maxEdgeCh, s.edgeCh);
      }
      if (i == probeTick) {
        // A segment boundary, not a read inside the tick loop (the rig has
        // already drained the tick; see gas-leave's probe).
        ctx.WaitIdle();
        ctx.ProcessEvents();
        uint32_t gp[kGasSpHdr] = {};
        ReadGasStatsSync(ctx, world, gp);
        r.digest = gp[kGasSpDigest];
        r.live = GasAliveSync(ctx, world, sim);
      }
    }
    world.SetGasLeaveCapForTest(0);
    return r;
  };

  const Run a = run();
  const Run b = run();

  uint32_t compared = 0, divergedAt = 0;
  for (const auto& kv : a.series) {
    const auto it = b.series.find(kv.first);
    if (it == b.series.end()) continue;
    compared++;
    if (!(it->second == kv.second) && !divergedAt) divergedAt = kv.first;
  }
  const bool forced = a.refused > 0 && b.refused > 0;
  const bool partial = a.accepted > 0;
  const bool clean = a.overrun == 0 && b.overrun == 0 && a.poolFull == 0 &&
                     b.poolFull == 0 && a.overBudgetTicks == 0 &&
                     b.overBudgetTicks == 0 && a.overCapTicks == 0 &&
                     b.overCapTicks == 0;
  const bool same = compared > 0 && divergedAt == 0 && a.digest == b.digest &&
                    a.live == b.live && a.live > 0;

  RecordObserved("gasOverflow.refusedObserved", (double)a.refused);
  RecordObserved("gasOverflow.acceptedObserved", (double)a.accepted);

  detail = Format(
      "leave budget forced to %u: converted %llu, REFUSED %llu on %u ticks "
      "(edge chunks up to %u) | overrun %llu/%llu, pool refusals %llu/%llu, "
      "ticks over budget %u/%u | twice-run %s over %u snapshot ticks, digest "
      "at t%u %08x vs %08x over %u vs %u live parcels",
      cap, (unsigned long long)a.accepted, (unsigned long long)a.refused,
      a.refusingTicks, a.maxEdgeCh, (unsigned long long)a.overrun,
      (unsigned long long)b.overrun, (unsigned long long)a.poolFull,
      (unsigned long long)b.poolFull, a.overBudgetTicks, b.overBudgetTicks,
      same ? "identical" : "DIVERGED", compared, probeTick, a.digest, b.digest,
      a.live, b.live);
  if (!forced)
    detail += " -- the overflow was never ENTERED: nothing was refused, so "
              "this run says nothing about how refusals are chosen";
  if (!partial)
    detail += " -- nothing was accepted: a share of zero refuses everything "
              "deterministically and proves nothing about the ranking";
  if (!clean)
    detail += " -- the budget accounting is broken (an overrun, a pool "
              "refusal, or a tick that converted more than its budget)";
  if (!same)
    detail += divergedAt
                  ? Format(" -- twice-run DIVERGED at t%u (hash or per-tick "
                           "leave counts): a refusal depended on scheduling",
                           divergedAt)
                  : " -- the parcel digest / population did not reproduce";

  const bool ok = forced && partial && clean && same;
  std::printf("gas-leave-overflow: %s (%s)\n", ok ? "PASS" : "FAIL",
              detail.c_str());

  world.SetGasLeaveCapForTest(0);
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
    // Drafts off: this gate measures the wind COUPLING inside a chamber, and the
    // draft volume (correctly) stills the air in a chamber. Shelter is `drafts`.
    t.sim.draftMode = 0;
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

// =========================== gas-farplume ===================================
//
// THE SUBJECT. A chunk that leaves the residency window mid-burn is FROZEN: its
// embers were downsampled into the far cascade by the last `fardown` and stay
// there, visibly orange, for the rest of the session — while its smoke dies
// within about four seconds, because a smoke parcel is only ever born at the
// window face by the running CA and then decays at the material's own authored
// rate. `gasFarPlume` (sim_gas.wgsl) synthesizes that missing smoke into the
// render-only density box from a CPU-built emitter list (world.h
// kGasFarEmitMax, src/sim/farplumes.h).
//
// HOW THE FIXTURE GETS AN EVICTED BURNING CHUNK WITHOUT EVICTING ONE. Forcing a
// real window shift mid-suite would move the origin out from under every gate
// that follows, which is the same reason `far-persist` (selftest_render.cpp)
// feeds FarEdits by hand: the words come off the GPU and are handed to the
// index at a coordinate half a window away, which is exactly what the eviction
// path hands it. What is under test is the index -> list -> splat chain, and
// that chain does not care which door the words came through.
//
// THE FOUR CLAIMS, and why none of them can pass on its own:
//
//   A. THE PLUME EXISTS AND IS WHERE THE FIRE IS. Density over the emitter's
//      column, and ZERO over a control column the same size elsewhere in the
//      box. A whole-box sum would pass on a plume drawn in the wrong place
//      (memory: gotcha-a-count-is-not-a-shape).
//   B. THE WORLD DID NOT MOVE. The same fixture, the same ticks, with the
//      emitter list EMPTY — and the per-tick world hash series must be
//      IDENTICAL. This is the claim the whole feature rests on: gasOuter is
//      render-only, so a frozen fire may not be able to change one voxel. It is
//      a differential rather than a pinned number, so it keeps meaning after
//      every future rebaseline.
//   C. IT BLOWS DOWNWIND. With a hard east wind the mass in the +x half of the
//      column box must exceed the -x half. Without this the drift wire could be
//      dead and A would still pass.
//   D. OFF MEANS NO ROW, OBSERVED. With sim.gasMode 0 (the parcel pipeline's
//      pre-existing exact off switch) AND render.farPlumeStrength 0, not one
//      gas row is recorded — including the box's per-tick CLEAR. So the box
//      still holds the previous arm's plume, byte for byte, after four more
//      ticks. A stale box is the direct, positive observation of "the row did
//      not run"; asserting zero density would have been satisfied by a row that
//      ran and added nothing. GasRenderActive() must be false in that arm, or
//      the renderer would be sampling a box nobody cleared.
//
// D also proves the NEW condition is load-bearing rather than cosmetic: the
// clear used to be on C_GAS, and at gasMode 0 with emitters live it would leave
// the box uncleared while the splat kept adding to it — a plume that
// accumulates forever. C_GASOUT is the union that fixes it, and arm A runs with
// gasMode 0 for exactly that reason.
struct PlumeRun {
  std::map<uint32_t, uint32_t> hash;   // world hash by snapshot tick
  uint32_t colMax = 0, ctrlMax = 0;
  uint64_t colSum = 0, ctrlSum = 0;
  uint64_t loSum = 0, hiSum = 0;       // the column box's -x / +x halves
  // The LONG-RANGE box over the SAME column. Arm E (the crossfade shell): this
  // fixture stands 376 voxels from the window centre in the max norm, which is
  // inside world.h kGasFarBlendVox of the fine box's face, so the emitter must
  // be in BOTH lists and this must be non-zero.
  uint32_t wideMax = 0;
  uint64_t wideSum = 0, wideCtrlSum = 0;
  // Arm F only: the FARTHEST burning chunk's own column, read in both boxes.
  // It is the one the nearest-first fine cap must have rejected, so the fine
  // box must be empty over it and the wide box must not be.
  uint64_t farColFineSum = 0, farColWideSum = 0;
  uint32_t renderFlagTicks = 0;
};

Status GateGasFarPlume(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  if (!world.farPlumes) {
    detail = "Stream never published world.farPlumes - the index is not wired";
    return Status::Fail;
  }
  FarPlumes& plumes = *world.farPlumes;
  // NO CAMERA: a gate that ran TickAuthority before this one left the
  // selftest player's eye on the shared index (FarPlumes::ClearEye). The
  // fixture's weights are measured from the window centre.
  plumes.ClearEye();

  // WHAT COUNTS AS FROZEN FIRE IS DATA, and the fixture proves it rather than
  // assuming it: the material is the first NON-GAS one the compiled reaction
  // table says makes smoke (materials.h SmokeSourceTable: its own rules decay
  // to or emit fire/smoke -- the near-field fact, rule-unification W1-B1).
  // Non-gas because the fixture paints a whole chunk of it and a gas would
  // drift off before the harvest. Naming `ember` here would have made the gate
  // pass over a hardcoded id and say nothing about the rule (memory:
  // gotcha-gate-hardcodes-asset-cast).
  //
  // NEAR AND FAR AGREE: the index must hold exactly that table for every id.
  // The old rule (tagged hot + emissive) put lava in it, so a lava lake
  // plumed at distance and never up close; that is the disagreement this
  // loop now refuses.
  const std::vector<uint8_t> smoky = SmokeSourceTable(c.mats, c.reactions);
  uint32_t hotId = 0;
  for (uint32_t i = 1; i < (uint32_t)c.mats.size(); i++) {
    if (FarPlumes::SmokeSource(i) != (smoky[i] != 0)) {
      detail = Format("material %s: the far plume index says %s but its own "
                      "reaction rules say %s - near and far disagree about "
                      "whether it is on fire (Simulation::UploadTables did not "
                      "latch the table, or FarPlumes grew a second rule)",
                      c.mats[i].name.c_str(),
                      FarPlumes::SmokeSource(i) ? "smoke" : "no smoke",
                      smoky[i] ? "smoke" : "no smoke");
      return Status::Fail;
    }
    if (!hotId && smoky[i] && c.mats[i].gpu.klass != CLASS_GAS) hotId = i;
  }
  if (!hotId) {
    detail = "no non-gas material has a rule that decays to or emits fire or "
             "smoke - nothing in this table could ever freeze as a distant fire";
    return Status::Fail;
  }

  const uint32_t ticks = (uint32_t)BaselineNumber("gasFarPlumeTicks", 12);
  const uint32_t minMax = (uint32_t)BaselineNumber("gasFarPlumeMinCellMax", 40);
  const uint64_t minSum =
      (uint64_t)BaselineNumber("gasFarPlumeMinColumnSum", 2000);

  const IVec3 wo = world.WindowOrigin();
  // The paint site: a whole chunk, mid-window in y/z and well inside x, cleared
  // to the hot material by mutation. A WHOLE chunk so all four of its column
  // footprints saturate — the strength the shader normalises by is the column's
  // hot-voxel count, and a fixture at 1/8 strength would be testing the taper
  // as well as the wiring.
  const IVec3 wcPaint{wo.x + 20, wo.y + 20, wo.z + 16};
  std::vector<CellOp> paint;
  paint.reserve(kChunkVol);
  for (int z = 0; z < (int)kChunk; z++)
    for (int x = 0; x < (int)kChunk; x++)
      for (int y = 0; y < (int)kChunk; y++)
        paint.push_back({World::SlotCellIndex({wcPaint.x * (int)kChunk + x,
                                               wcPaint.y * (int)kChunk + y,
                                               wcPaint.z * (int)kChunk + z}),
                         hotId & 0xFFFu});

  // WHERE THE FROZEN FIRE LIVES: eight chunks BEYOND the window's low-x face,
  // which is 128 voxels out — outside the window (so Build keeps it) and well
  // inside the density box's 256-voxel skirt (so it is in range). Anchored to
  // WindowOrigin, never to a literal (memory: gotcha-gate-hardcodes-fixture-site).
  const IVec3 wcOut{wo.x - 8, wo.y + 20, wo.z + 16};
  if (world.ChunkInWindow(wcOut)) {
    detail = "the fixture's frozen-fire chunk is INSIDE the window - it would "
             "be dropped as a live fire and the gate would measure nothing";
    return Status::Fail;
  }
  // ...and it must land inside the fine/wide CROSSFADE SHELL (world.h
  // kGasFarBlendVox), which since 2026-09-19 is the WHOLE band an emitter can
  // be in the fine list: arm E below asserts this emitter reaches BOTH lists,
  // and that claim is only meaningful if the site is in the shell. Derived
  // from the site and the constant, never compared against a literal distance
  // (memory: gotcha-gate-hardcodes-fixture-site).
  const int ctrX = wo.x * (int)kChunk + (int)kWorldN / 2;
  const int ctrY = wo.y * (int)kChunk + (int)kWorldN / 2;
  const int ctrZ = wo.z * (int)kChunk + (int)kWorldN / 2;
  const int fixMx = std::max(
      std::abs(wcOut.x * (int)kChunk + (int)kChunk / 2 - ctrX),
      std::max(std::abs(wcOut.y * (int)kChunk + (int)kChunk / 2 - ctrY),
               std::abs(wcOut.z * (int)kChunk + (int)kChunk / 2 - ctrZ)));
  if (fixMx <= (int)kWorldN - kGasFarBlendVox || fixMx >= (int)kWorldN) {
    detail = Format("the fixture sits %d voxels from the window centre, which "
                    "is outside the fine/wide crossfade shell (%d..%d) - arm E "
                    "would be asserting nothing",
                    fixMx, (int)kWorldN - kGasFarBlendVox, (int)kWorldN);
    return Status::Fail;
  }

  // ---- harvest the words the eviction path would have handed the index ----
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  SubmitTick(ctx, world, sim, 1, kDefaultSeed, {}, {}, paint, false,
             {wo.x + 16, wo.y + 16, wo.z + 16}, false, false);
  ctx.WaitIdle();
  std::vector<uint32_t> words(kChunkVol);
  ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wcPaint), 1, words.data(),
                 "farPlumePaint");
  uint32_t painted = 0;
  for (uint32_t i = 0; i < kChunkVol; i++)
    if ((words[i] & 0xFFFu) == hotId) painted++;
  // Regenerate at once: the point of the fixture is the INDEX, and leaving
  // 4,096 embers burning in the window would put real smoke in the same box
  // this gate reads (and leave the world on fire for the gates after it).
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // The probe boxes, in world voxels. The column reaches from the fire to the
  // plume's top; the control is the same shape, a quarter of the box away in z,
  // and must read exactly zero.
  const int px = wcOut.x * (int)kChunk + (int)kChunk / 2;
  const int pyBase = wcOut.y * (int)kChunk;
  const int pz = wcOut.z * (int)kChunk + (int)kChunk / 2;
  const int reach = (int)(CurrentTuning().render.farPlumeHeight / kVoxelMeters);
  const int half = 48;   // covers the spread AND the wind tilt at 20 m/s
  const IVec3 colLo{px - half, pyBase, pz - half};
  const IVec3 colHi{px + half, pyBase + reach, pz + half};
  const IVec3 ctlLo{px - half, pyBase, pz + 256 - half};
  const IVec3 ctlHi{px + half, pyBase + reach, pz + 256 + half};

  const Tuning saved = CurrentTuning();

  // `withEmitters` is the ONLY thing that differs between arms A and B, which
  // is what makes the hash comparison a claim about this feature and not about
  // the fixture.
  // THE CAP ARM's fixture (arm F). `capFill` is a block of burning chunks big
  // enough that its emitters overflow the fine section on their own, and
  // `wcFar` is one more chunk placed unambiguously FARTHER from the window
  // centre than any of them -- so the nearest-first cap is guaranteed to
  // reject it and the promotion is guaranteed to be what draws it. Both are
  // anchored to WindowOrigin and sized from kGasFarEmitMax, never literals.
  std::vector<IVec3> capFill;
  for (int dy = 0; dy < 4; dy++)
    for (int dz = 0; dz < 20; dz++)
      capFill.push_back({wo.x - 8, wo.y + 18 + dy, wo.z + 4 + dz});
  const IVec3 wcFar{wo.x - 8, wo.y + 20, wo.z + 40};
  // Its own probe column, far enough from the filler block that no filler
  // plume can reach it: 17 chunks of z is 272 voxels and a plume is ~40 wide.
  const int fx = wcFar.x * (int)kChunk + (int)kChunk / 2;
  const int fz = wcFar.z * (int)kChunk + (int)kChunk / 2;
  const IVec3 farLo{fx - half, wcFar.y * (int)kChunk, fz - half};
  const IVec3 farHi{fx + half, wcFar.y * (int)kChunk + reach, fz + half};

  // mode 0 = the one-chunk fixture, 1 = no emitters at all, 2 = the cap arm.
  auto run = [&](int mode, bool windEast) -> PlumeRun {
    PlumeRun r;
    {
      Tuning t = saved;
      // sim.gasMode 0: the parcel pipeline is OFF for the whole gate. Two
      // reasons, both load-bearing. It makes arm D's stale box reachable (with
      // parcels armed the CA would clear the box every tick whatever the
      // emitters do), and it makes arm A a test of C_GASOUT: the clear is
      // running on the far-emitter half of the union alone.
      t.sim.gasMode = (int)kGasModeWall;
      t.render.farPlumeStrength = 1.0f;
      t.sim.windMode = windEast ? (int)kWindModeDrift : (int)kWindModeOff;
      // Drafts off: this gate measures the wind COUPLING inside a chamber, and the
      // draft volume (correctly) stills the air in a chamber. Shelter is `drafts`.
      t.sim.draftMode = 0;
      t.wind.weatherAuto = false;
      t.wind.windDirDeg = 90.0f;     // +x, as the `wind-gas` gate establishes
      t.wind.windSpeed = 20.0f;
      t.wind.gustStrength = 0.1f;
      SetCurrentTuning(t);
    }
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    plumes.Clear();
    if (mode == 0) plumes.NoteChunk(wcOut, words.data());
    if (mode == 2) {
      for (const IVec3& wc : capFill) plumes.NoteChunk(wc, words.data());
      plumes.NoteChunk(wcFar, words.data());
    }

    for (uint32_t i = 1; i <= ticks; i++) {
      SubmitTick(ctx, world, sim, i, kDefaultSeed, {}, {}, {}, true,
                 {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
      if (sandvox::GasRenderActive()) r.renderFlagTicks++;
      const WorldSnapshot& sn = world.Snap();
      if (sn.valid && r.hash.find(sn.tick) == r.hash.end())
        r.hash[sn.tick] = sn.worldHash;
    }
    ctx.WaitIdle();
    ctx.ProcessEvents();
    ReadGasOuterBoxSync(ctx, world, colLo, colHi, &r.colMax, &r.colSum);
    ReadGasOuterBoxSync(ctx, world, ctlLo, ctlHi, &r.ctrlMax, &r.ctrlSum);
    ReadGasOuterBoxSync(ctx, world, {colLo.x, colLo.y, colLo.z},
                        {px - 1, colHi.y, colHi.z}, nullptr, &r.loSum);
    ReadGasOuterBoxSync(ctx, world, {px + 1, colLo.y, colLo.z}, colHi,
                        nullptr, &r.hiSum);
    ReadGasFarOuterBoxSync(ctx, world, colLo, colHi, &r.wideMax, &r.wideSum);
    ReadGasFarOuterBoxSync(ctx, world, ctlLo, ctlHi, nullptr, &r.wideCtrlSum);
    if (mode == 2) {
      ReadGasOuterBoxSync(ctx, world, farLo, farHi, nullptr, &r.farColFineSum);
      ReadGasFarOuterBoxSync(ctx, world, farLo, farHi, nullptr, &r.farColWideSum);
    }
    return r;
  };

  const PlumeRun a = run(0, false);
  const uint32_t emitters = plumes.Count();
  const uint64_t refusedChunks = plumes.RefusedChunks();
  // Latched HERE, not read at format time: arm F leaves 81 chunks in the
  // index and this phrase is about arm A's one-chunk fixture.
  const size_t indexChunks = plumes.Chunks();

  // ---- arm D: the off switch, measured as a box nobody cleared ------------
  // Continues from arm A WITHOUT resetting anything, so the box still holds
  // arm A's plume. With gasMode 0 already and farPlumeStrength now 0, the
  // emitter count goes to zero, C_GASOUT is false, and the clear is not
  // recorded — so four more ticks must leave the box exactly as it was.
  {
    Tuning t = CurrentTuning();
    t.render.farPlumeStrength = 0.0f;
    SetCurrentTuning(t);
  }
  uint32_t offFlagTicks = 0;
  for (uint32_t i = 1; i <= 4; i++) {
    SubmitTick(ctx, world, sim, ticks + i, kDefaultSeed, {}, {}, {}, false,
               {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
    if (sandvox::GasRenderActive()) offFlagTicks++;
  }
  ctx.WaitIdle();
  ctx.ProcessEvents();
  uint32_t offMax = 0;
  uint64_t offSum = 0;
  ReadGasOuterBoxSync(ctx, world, colLo, colHi, &offMax, &offSum);
  const uint32_t offCount = sim.FarPlumeCount();

  const PlumeRun b = run(1, false);
  const PlumeRun w = run(0, true);
  // ---- arm F: the fine cap OVERFLOWS, and nothing goes silent -------------
  // 81 whole burning chunks -> ~324 column emitters against a 256-record fine
  // section. Before 2026-09-19 the surplus was simply deleted, which is how a
  // handful of burning trees could switch off the smoke of every other fire in
  // the world. It is now PROMOTED to the wide list, so the farthest chunk --
  // the one the nearest-first cap is guaranteed to have rejected -- must have
  // an empty fine column and a non-empty wide one.
  const PlumeRun f = run(2, false);
  const uint32_t capFine = plumes.Count();
  const uint32_t capWide = plumes.CountWide();
  const uint32_t capPromoted = plumes.FinePromoted();
  const uint32_t capBucket = plumes.WideBucketScale();
  const uint64_t capRefused = plumes.RefusedChunks();
  SetCurrentTuning(saved);

  // ---- the assertions -----------------------------------------------------
  // NOT `== kChunkVol`, and the difference is the CA: mutateCells lands the
  // paint and the colour loop runs behind it in the SAME tick, so a handful of
  // embers have already reacted by the time the words are read (measured
  // 4,082 of 4,096). What the fixture needs is a near-saturated column, which
  // is what 7/8 asserts; an exact count would be a gate re-pinned to
  // reactions.json.
  const bool noted = painted * 8 >= kChunkVol * 7 && emitters > 0;
  const bool drawn = a.colMax >= minMax && a.colSum >= minSum;
  const bool placed = a.ctrlSum == 0;
  const bool flagged = a.renderFlagTicks == ticks;
  const bool quiet = b.colSum == 0 && b.ctrlSum == 0 && b.renderFlagTicks == 0;
  // ARM E, THE CROSSFADE SHELL. The fixture is inside kGasFarBlendVox of the
  // fine box's face (checked above), so the CPU must have put it in BOTH lists
  // with complementary weights and the LONG-RANGE box must hold the fading-in
  // twin of the plume arm A just measured in the near box. Non-zero, not a
  // floor: the weight at this distance is a smoothstep of the site and pinning
  // a number would re-pin the gate every time the shell is retuned.
  //
  // PAIRED WITH THE CONTROL COLUMN, not with the no-emitter arm — and that is
  // not a convenience, it is arm D's rule applied to the other box. With no
  // emitters there is no wide row, so nothing CLEARS the wide box and arm B
  // reads arm A's plume still sitting in it; "empty with no emitters" is a
  // claim the feature is designed to make false. What makes this arm A's own
  // plume rather than anyone's leftovers is that it is over the FIRE and the
  // control column a quarter of the box away is exactly zero.
  const bool faded = a.wideSum > 0 && a.wideCtrlSum == 0;
  // ARM F. Three claims that only mean something together: the cap really was
  // the bound (a fixture that fits proves nothing), the surplus was PROMOTED
  // rather than dropped, and the promotion is visible as DENSITY -- over the
  // farthest chunk specifically, which is the one that cannot have been kept.
  // The empty FINE column beside the full wide one is what distinguishes
  // "promoted" from "it was in the fine list all along".
  const bool capBound =
      capFine == kGasFarEmitMax && capPromoted > 0 && capRefused == 0;
  const bool capDrawn =
      f.farColWideSum > 0 && f.farColFineSum == 0 && capWide > 0;

  uint32_t compared = 0, divergedAt = 0;
  for (const auto& kv : a.hash) {
    const auto it = b.hash.find(kv.first);
    if (it == b.hash.end()) continue;
    compared++;
    if (it->second != kv.second && !divergedAt) divergedAt = kv.first;
  }
  const bool inert = compared > 0 && divergedAt == 0;

  // The drift: mass on the downwind side of the column box beats the upwind
  // side. A ratio rather than "> 0", so a symmetric spread cannot ratify it.
  const bool drifted = w.hiSum > w.loSum * 2 && w.hiSum > 0;

  // THE FLAG IS THE PROOF THAT THE CONDITION WAS FALSE, and it is the right
  // one to use. Simulation::FarPlumeCount() is the count standing in the
  // BUFFER — the emitter list is uploaded only when it changes, so it keeps
  // reading 4 here and always will; what went to zero is the count the
  // RECORDER saw (cx.gasFarEmitCount, gated on render.farPlumeStrength). The
  // render flag is that same value ORed into SetGasRenderActive, so 0 ticks of
  // flag with gasMode already off is "the recorder saw zero emitters",
  // observed. The unchanged box is the other half: nothing cleared it.
  const bool offExact =
      offSum == a.colSum && offMax == a.colMax && offFlagTicks == 0;

  RecordObserved("gasFarPlume.emittersObserved", (double)emitters);
  RecordObserved("gasFarPlume.columnSumObserved", (double)a.colSum);
  RecordObserved("gasFarPlume.columnMaxObserved", (double)a.colMax);
  RecordObserved("gasFarPlume.driftRatioObserved",
                 w.loSum ? (double)w.hiSum / (double)w.loSum : (double)w.hiSum);

  detail = Format(
      "material %s (id %u) | %u/%u voxels painted -> %u emitters in range "
      "(index holds %zu chunks, %llu refused) | column box: max %u (floor %u), "
      "sum %llu (floor %llu) | control box the same size: max %u, sum %llu | "
      "CROSSFADE SHELL at %d vox (shell %d..%d): wide box over the same column "
      "max %u, sum %llu (the fading-in twin), control column %llu | "
      "CAP arm: %zu chunks -> fine %u/%u (%u promoted to wide), wide %u at "
      "bucket x%u, %llu refused; the FARTHEST chunk column: fine %llu (must be "
      "0), wide %llu | "
      "render flag %u/%u ticks | NO-EMITTER arm: column %llu, control %llu, "
      "flag %u ticks, world hash identical over %u ticks (%s) | wind arm: "
      "downwind %llu vs upwind %llu | OFF arm: column %llu/max %u (was "
      "%llu/%u), %u still in the buffer, flag %u/4 ticks",
      c.mats[hotId].name.c_str(), hotId, painted, kChunkVol, emitters,
      indexChunks, (unsigned long long)refusedChunks, a.colMax, minMax,
      (unsigned long long)a.colSum, (unsigned long long)minSum, a.ctrlMax,
      (unsigned long long)a.ctrlSum, fixMx, (int)kWorldN - kGasFarBlendVox,
      (int)kWorldN, a.wideMax, (unsigned long long)a.wideSum,
      (unsigned long long)a.wideCtrlSum, capFill.size() + 1, capFine,
      kGasFarEmitMax, capPromoted, capWide, capBucket,
      (unsigned long long)capRefused, (unsigned long long)f.farColFineSum,
      (unsigned long long)f.farColWideSum, a.renderFlagTicks, ticks,
      (unsigned long long)b.colSum, (unsigned long long)b.ctrlSum,
      b.renderFlagTicks, compared, inert ? "INERT" : "MOVED",
      (unsigned long long)w.hiSum, (unsigned long long)w.loSum,
      (unsigned long long)offSum, offMax, (unsigned long long)a.colSum,
      a.colMax, offCount, offFlagTicks);

  if (!noted)
    detail += " -- the fixture never reached the index: the paint or the "
              "hot-material rule did not take";
  if (!drawn)
    detail += " -- NO PLUME over the frozen fire: the emitter list reached the "
              "GPU but gasFarPlume put nothing in the density box";
  if (!placed)
    detail += " -- the control column is NOT empty: whatever is in the box is "
              "not this emitter's plume";
  if (!flagged)
    detail += " -- the gas RENDER flag did not arm on every tick, so the "
              "raymarcher skips the box the splat just filled";
  if (!quiet)
    detail += " -- the box is not empty with NO emitters: something else is "
              "writing it and arm A measured that instead";
  if (!inert)
    detail += divergedAt
                  ? Format(" -- THE WORLD MOVED: the hash diverges at t%u "
                           "between the plume and no-plume arms, so this "
                           "render-only feature is writing hashed state",
                           divergedAt)
                  : " -- the snapshot ring never delivered a comparable tick";
  if (!drifted)
    detail += " -- the plume does not lean downwind in a 20 m/s east wind: the "
              "wind wire in gasFarPlume is dead";
  if (!faded)
    detail += a.wideSum == 0
                  ? " -- the LONG-RANGE box is EMPTY over a fire inside the "
                    "crossfade shell: the fine/wide handover is a hard split "
                    "again and the coarse plume will pop in at the fine box's "
                    "face"
                  : " -- the wide box's CONTROL column is not zero, so the "
                    "fading-in twin is not where the fire is";
  if (!offExact)
    detail += " -- at render.farPlumeStrength 0 with sim.gasMode 0 the density "
              "box CHANGED, so a gas row is still being recorded in a world "
              "where every gas condition is false";

  if (!capBound)
    detail += " -- the cap arm did not actually overflow the fine section (or "
              "the INDEX refused chunks first), so it asserts nothing about "
              "what happens when it does";
  if (!capDrawn)
    detail += f.farColWideSum == 0
                  ? " -- THE FARTHEST BURNING CHUNK HAS NO SMOKE: the fine cap "
                    "dropped it instead of promoting it to the wide list, "
                    "which is the bug where a big fire silences every other "
                    "fire in the world"
                  : " -- the farthest chunk is in the FINE box, so the cap did "
                    "not reject it and the promotion is untested";
  const bool ok = noted && drawn && placed && flagged && quiet && inert &&
                  drifted && offExact && faded && capBound && capDrawn;
  std::printf("gas-farplume: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  // Leave the world as it was found (rule 7), index included: an emitter left
  // in it would draw a plume across every gate after this one.
  plumes.Clear();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}


// =========================== gas-farplume2 ==================================
//
// THE SUBJECT, and it is the half that answers the actual complaint. `gasOuter`
// spans exactly two window edges — ±51.2 m — which is a sliver of what the
// cascade DRAWS. A fire 200 m out is plainly visible as frozen orange in the
// far field and, before this, had nowhere at all to put its smoke: it was not
// in the near box, so package A could not help it. The LONG-RANGE box (world.h
// kGasFarOuterN) is 128 cells of 64 x 8 x 64 voxels — 51.2 m across, 6.4 m tall, which
// is exactly far cascade level 4's box edge, so the plume LOD boundary and the
// terrain LOD boundary are the same distance instead of two visible rings.
//
// THE FIXTURE is `gas-farplume`'s, moved out. The same harvested ember chunk is
// handed to the index at a coordinate ~200 m from the window centre instead of
// ~13 m outside it, which is the one thing that decides which of the two
// emitter lists it lands in. The lists are no longer disjoint EVERYWHERE --
// since world.h kGasFarBlendVox became the whole fine band, everything from
// the window face to the fine box's face is in both with complementary
// weights, and `gas-farplume` arm E asserts exactly that. They are still
// disjoint out HERE, past the shell, which is what claim B below reads.
//
// FOUR CLAIMS, and the first two are a PAIR — neither means anything alone:
//
//   A. THE FAR PLUME EXISTS. Density in the long-range box over the frozen
//      fire's column, and exactly ZERO over a control column the same size.
//   B. IT IS IN THE RIGHT BOX. The near box must be EMPTY over the same site,
//      and the fine emitter count must be 0 while the wide one is not. PAST
//      THE CROSSFADE SHELL the two lists are still strictly disjoint, and this
//      asserts it rather than assuming it -- a bug that put every emitter in
//      both lists at every distance would pass A, and would double-brighten
//      every distant fire. What stops the shell from being that bug is that
//      its two weights sum to 255, which is a property of the data here too.
//   C. THE WORLD DID NOT MOVE. The same fixture with the wide list EMPTY, and
//      the per-tick world hash series must be IDENTICAL — the render-only
//      claim, repeated for the long-range half, as a differential rather than
//      against a pinned number.
//   D. THE RANGE KNOB IS AN EXACT OFF SWITCH, observed as a box nobody
//      cleared. At render.farPlumeRange 0 the wide list is empty, so its splat
//      row AND its clear are both unrecorded and the box still holds arm A's
//      plume byte for byte four ticks later. Asserting zero density there
//      would have been satisfied by a row that ran and added nothing.
//
// The FAR render flag (RenderParams bit 5) is checked in every arm: it is a
// separate bit from the near one on purpose, and a gate that let them share
// would not notice the near box's flag silently arming a second 4 MiB volume
// walk on every terrain pixel.
Status GateGasFarPlume2(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  if (!world.farPlumes) {
    detail = "Stream never published world.farPlumes - the index is not wired";
    return Status::Fail;
  }
  FarPlumes& plumes = *world.farPlumes;
  // NO CAMERA: a gate that ran TickAuthority before this one left the
  // selftest player's eye on the shared index (FarPlumes::ClearEye). The
  // fixture's weights are measured from the window centre.
  plumes.ClearEye();

  // The same fixture material gas-farplume derives: the first non-gas smoke
  // source in the compiled reaction table (materials.h SmokeSourceTable).
  const std::vector<uint8_t> smoky = SmokeSourceTable(c.mats, c.reactions);
  uint32_t hotId = 0;
  for (uint32_t i = 1; i < (uint32_t)c.mats.size() && !hotId; i++)
    if (smoky[i] && c.mats[i].gpu.klass != CLASS_GAS) hotId = i;
  if (!hotId) {
    detail = "no non-gas material has a rule that decays to or emits fire or "
             "smoke";
    return Status::Fail;
  }

  const uint32_t ticks = (uint32_t)BaselineNumber("gasFarPlume2Ticks", 12);
  const uint32_t minMax = (uint32_t)BaselineNumber("gasFarPlume2MinCellMax", 40);
  const uint64_t minSum =
      (uint64_t)BaselineNumber("gasFarPlume2MinColumnSum", 800);
  // How far out the frozen fire sits, in CHUNKS from the window's low-x face.
  // 128 chunks = 2,048 voxels = 204.8 m, which is half the long-range box's
  // reach: comfortably past the near box (51.2 m) and comfortably inside the
  // far one, so neither bound is what the gate is measuring.
  const int outChunks = (int)BaselineNumber("gasFarPlume2ChunksOut", 128);

  const IVec3 wo = world.WindowOrigin();
  const IVec3 wcPaint{wo.x + 20, wo.y + 20, wo.z + 16};
  std::vector<CellOp> paint;
  paint.reserve(kChunkVol);
  for (int z = 0; z < (int)kChunk; z++)
    for (int x = 0; x < (int)kChunk; x++)
      for (int y = 0; y < (int)kChunk; y++)
        paint.push_back({World::SlotCellIndex({wcPaint.x * (int)kChunk + x,
                                               wcPaint.y * (int)kChunk + y,
                                               wcPaint.z * (int)kChunk + z}),
                         hotId & 0xFFFu});

  // The frozen fire's home, anchored to WindowOrigin like every other fixture
  // here. Far enough out that it CANNOT be in the fine list, which the gate
  // then asserts rather than trusting.
  const IVec3 wcOut{wo.x - outChunks, wo.y + 20, wo.z + 16};
  const int cx = wo.x * (int)kChunk + (int)kWorldN / 2;
  const int exv = wcOut.x * (int)kChunk + (int)kChunk / 2;
  if (std::abs(exv - cx) <= (int)kWorldN) {
    detail = Format("the fixture site is only %d voxels from the window centre "
                    "- inside the NEAR box's half-extent (%u), so it would land "
                    "in the fine list and this gate would re-test gas-farplume",
                    std::abs(exv - cx), kWorldN);
    return Status::Fail;
  }

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  SubmitTick(ctx, world, sim, 1, kDefaultSeed, {}, {}, paint, false,
             {wo.x + 16, wo.y + 16, wo.z + 16}, false, false);
  ctx.WaitIdle();
  std::vector<uint32_t> words(kChunkVol);
  ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wcPaint), 1, words.data(),
                 "farPlume2Paint");
  uint32_t painted = 0;
  for (uint32_t i = 0; i < kChunkVol; i++)
    if ((words[i] & 0xFFFu) == hotId) painted++;
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // ---- arm E's fixture: a TREE-SIZED fire, not a solid chunk of ember -----
  // Every other arm here paints a WHOLE chunk, which is ~4,000 hot voxels and
  // four saturated columns. A burning tree is one or two orders of magnitude
  // less than that, and the wide splat's mass is LINEAR in the column count it
  // aggregates -- so the whole-chunk fixture sat comfortably above the
  // renderer's erosion threshold while a tree sat under it and was carved to
  // nothing. That is the "the big fire smokes at distance and the small trees
  // do not" report, and no arm could see it. `sparseDiv` is how much of the
  // chunk stays alight.
  const int sparseDiv = (int)BaselineNumber("gasFarPlume2SparseDiv", 16);
  std::vector<uint32_t> sparse(words);
  uint32_t sparseHot = 0;
  {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kChunkVol; i++) {
      if ((sparse[i] & 0xFFFu) != hotId) continue;
      if ((seen++ % (uint32_t)sparseDiv) == 0) { sparseHot++; continue; }
      sparse[i] = 0u;   // air
    }
  }

  // The probe boxes. The coarse cell is 6.4 m and a WIDE emitter's column is
  // stretched by how big the fire is, so the box is generous in every axis;
  // the control is the same shape a quarter of the far box away in z.
  const int pyBase = wcOut.y * (int)kChunk;
  const int pz = wcOut.z * (int)kChunk + (int)kChunk / 2;
  const int reach =
      (int)(CurrentTuning().render.farPlumeHeight * 4.0f / kVoxelMeters);
  const int half = 192;
  const IVec3 colLo{exv - half, pyBase, pz - half};
  const IVec3 colHi{exv + half, pyBase + reach, pz + half};
  const IVec3 ctlLo{exv - half, pyBase, pz + 2048 - half};
  const IVec3 ctlHi{exv + half, pyBase + reach, pz + 2048 + half};

  const Tuning saved = CurrentTuning();

  // ...and its own site, far enough from arm A's that the two plumes cannot
  // overlap: 64 chunks of z is 1,024 voxels against a wide column a few cells
  // across.
  const IVec3 wcSparse{wcOut.x, wcOut.y, wcOut.z + 64};
  const int spz = wcSparse.z * (int)kChunk + (int)kChunk / 2;
  const IVec3 spLo{exv - half, pyBase, spz - half};
  const IVec3 spHi{exv + half, pyBase + reach, spz + half};

  struct Run {
    std::map<uint32_t, uint32_t> hash;
    uint32_t wideMax = 0, ctrlMax = 0, fineMax = 0;
    uint32_t spMax = 0;
    uint64_t spSum = 0;
    uint64_t wideSum = 0, ctrlSum = 0, fineSum = 0;
    uint32_t farFlagTicks = 0, nearFlagTicks = 0;
    uint32_t emittersFine = 0, emittersWide = 0;
  };

  // mode 0 = the whole-chunk fixture, 1 = no emitters, 2 = the tree-sized one.
  auto run = [&](int mode) -> Run {
    Run r;
    {
      Tuning t = saved;
      // The parcel pipeline OFF for the whole gate, for gas-farplume's two
      // reasons: it makes arm D's stale box reachable, and it makes arm A a
      // test of the wide row's own condition rather than of the parcel latch.
      t.sim.gasMode = (int)kGasModeWall;
      t.render.farPlumeStrength = 1.0f;
      t.sim.windMode = (int)kWindModeOff;
      SetCurrentTuning(t);
    }
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    plumes.Clear();
    if (mode == 0) plumes.NoteChunk(wcOut, words.data());
    if (mode == 2) plumes.NoteChunk(wcSparse, sparse.data());

    for (uint32_t i = 1; i <= ticks; i++) {
      SubmitTick(ctx, world, sim, i, kDefaultSeed, {}, {}, {}, true,
                 {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
      if (sandvox::GasFarRenderActive()) r.farFlagTicks++;
      if (sandvox::GasRenderActive()) r.nearFlagTicks++;
      const WorldSnapshot& sn = world.Snap();
      if (sn.valid && r.hash.find(sn.tick) == r.hash.end())
        r.hash[sn.tick] = sn.worldHash;
    }
    r.emittersFine = sim.FarPlumeCount();
    r.emittersWide = sim.FarPlumeWideCount();
    ctx.WaitIdle();
    ctx.ProcessEvents();
    ReadGasFarOuterBoxSync(ctx, world, colLo, colHi, &r.wideMax, &r.wideSum);
    ReadGasFarOuterBoxSync(ctx, world, ctlLo, ctlHi, &r.ctrlMax, &r.ctrlSum);
    // THE NEAR BOX OVER THE SAME SITE. It must be empty, and that is claim B:
    // this fixture is far PAST the crossfade shell (world.h kGasFarBlendVox),
    // where the two lists are still strictly disjoint, so a fire this far out
    // must be drawn by exactly one of them.
    ReadGasOuterBoxSync(ctx, world, colLo, colHi, &r.fineMax, &r.fineSum);
    if (mode == 2) ReadGasFarOuterBoxSync(ctx, world, spLo, spHi, &r.spMax,
                                          &r.spSum);
    return r;
  };

  const Run a = run(0);

  // ---- arm D: render.farPlumeRange 0, measured as a box nobody cleared ----
  {
    Tuning t = CurrentTuning();
    t.render.farPlumeRange = 0.0f;
    SetCurrentTuning(t);
  }
  uint32_t offFlagTicks = 0;
  for (uint32_t i = 1; i <= 4; i++) {
    SubmitTick(ctx, world, sim, ticks + i, kDefaultSeed, {}, {}, {}, false,
               {wo.x + 16, wo.y + 16, wo.z + 16}, true, false);
    if (sandvox::GasFarRenderActive()) offFlagTicks++;
  }
  ctx.WaitIdle();
  ctx.ProcessEvents();
  uint32_t offMax = 0;
  uint64_t offSum = 0;
  ReadGasFarOuterBoxSync(ctx, world, colLo, colHi, &offMax, &offSum);
  const uint32_t offWide = sim.FarPlumeWideCount();

  const Run b = run(1);
  // ---- arm E: a TREE-SIZED fire at the same distance ----------------------
  // The wide splat's mass is LINEAR in the columns it aggregates and the
  // renderer erodes anything under GAS_ERODE x its core threshold to nothing,
  // so "does a whole chunk of ember show up" and "does a tree show up" are
  // different questions. Only the second one was ever asked by a player.
  const Run e = run(2);
  SetCurrentTuning(saved);

  // ---- the assertions -----------------------------------------------------
  const bool noted = painted * 8 >= kChunkVol * 7 && a.emittersWide > 0;
  const bool drawn = a.wideMax >= minMax && a.wideSum >= minSum;
  const bool placed = a.ctrlSum == 0;
  // Claim B: the fire is in the WIDE list and only there, and the near box
  // holds nothing over it.
  const bool disjoint = a.emittersFine == 0 && a.fineSum == 0;
  // ARM E. The floor is the renderer's own erasure threshold for THIS box:
  // gasErode carves everything below GAS_ERODE x GAS_CORE_COUNT_WIDE to
  // exactly zero, so a peak cell under it is a plume the player cannot see
  // however much the box says is in it. In baseline.json rather than here
  // because it mirrors two WGSL constants and a tuned threshold should not
  // cost a rebuild; check_invariants.py pins the WGSL half.
  const uint32_t spFloor =
      (uint32_t)BaselineNumber("gasFarPlume2SparseMinCellMax", 6);
  const bool sparseDrawn = e.spMax >= spFloor && e.spSum > 0;
  const bool flagged = a.farFlagTicks == ticks;
  // THE NO-EMITTER ARM'S BOX IS NOT EMPTY, AND THAT IS THE POINT. The
  // long-range box is cleared only on ticks its own row is recorded, and with
  // no wide emitter there is no row -- so twelve ticks later it still holds
  // arm A's plume, byte for byte. Asserting `== 0` here was this gate's first
  // failure and it was the gate being wrong, not the engine: it would have
  // required the clear to run in a world where the whole feature is off, which
  // is exactly the per-tick cost rule 2 forbids. What IS asserted is that
  // nothing arms, nothing is recorded, and the box is untouched.
  const bool quiet = b.farFlagTicks == 0 && b.emittersWide == 0 &&
                     b.wideSum == a.wideSum && b.wideMax == a.wideMax;

  uint32_t compared = 0, divergedAt = 0;
  for (const auto& kv : a.hash) {
    const auto it = b.hash.find(kv.first);
    if (it == b.hash.end()) continue;
    compared++;
    if (it->second != kv.second && !divergedAt) divergedAt = kv.first;
  }
  const bool inert = compared > 0 && divergedAt == 0;
  const bool offExact = offSum == a.wideSum && offMax == a.wideMax &&
                        offWide == 0 && offFlagTicks == 0;

  RecordObserved("gasFarPlume2.wideEmittersObserved", (double)a.emittersWide);
  RecordObserved("gasFarPlume2.columnSumObserved", (double)a.wideSum);
  RecordObserved("gasFarPlume2.columnMaxObserved", (double)a.wideMax);

  detail = Format(
      "material %s (id %u) | %u/%u painted, frozen fire %d chunks (%.1f m) out "
      "-> %u WIDE emitters and %u fine | TREE arm: %u hot voxels (1/%d of the "
      "chunk) -> wide column peak %u (erosion floor %u), sum %llu | "
      "long-range box over its column: max %u "
      "(floor %u), sum %llu (floor %llu) | control box the same size: max %u, "
      "sum %llu | the NEAR box over the same column: max %u, sum %llu (must be "
      "0 - the lists are disjoint) | far render flag %u/%u ticks, near flag "
      "%u/%u | NO-EMITTER arm: column %llu (unchanged), control %llu, flag %u "
      "ticks, world "
      "hash identical over %u ticks (%s; the box is untouched, as it must be - "
      "no emitters means no clear) | RANGE-0 arm: column %llu/max %u (was "
      "%llu/%u), %u wide emitters, flag %u/4 ticks",
      c.mats[hotId].name.c_str(), hotId, painted, kChunkVol, outChunks,
      (double)outChunks * kChunk * kVoxelMeters, a.emittersWide, a.emittersFine,
      sparseHot, sparseDiv, e.spMax, spFloor, (unsigned long long)e.spSum,
      a.wideMax, minMax, (unsigned long long)a.wideSum,
      (unsigned long long)minSum, a.ctrlMax, (unsigned long long)a.ctrlSum,
      a.fineMax, (unsigned long long)a.fineSum, a.farFlagTicks, ticks,
      a.nearFlagTicks, ticks, (unsigned long long)b.wideSum,
      (unsigned long long)b.ctrlSum, b.farFlagTicks, compared,
      inert ? "INERT" : "MOVED", (unsigned long long)offSum, offMax,
      (unsigned long long)a.wideSum, a.wideMax, offWide, offFlagTicks);

  if (!noted)
    detail += " -- the fixture never reached the WIDE list: either the paint "
              "did not take or Build put this fire in the wrong band";
  if (!drawn)
    detail += " -- NO PLUME in the long-range box over the frozen fire: the "
              "wide emitter list reached the GPU but gasFarPlumeWide put "
              "nothing in it";
  if (!placed)
    detail += " -- the control column is NOT empty: whatever is in the box is "
              "not this emitter's plume";
  if (!disjoint)
    detail += " -- THE LISTS ARE NOT DISJOINT: this fire is in the fine list "
              "too, or the near box holds density over it, so the same plume "
              "is being drawn twice and the boxes would double-brighten";
  if (!flagged)
    detail += " -- the LONG-RANGE render flag (bit 5) did not arm on every "
              "tick, so the raymarcher skips the box the splat just filled";
  if (!quiet)
    detail += " -- with NO emitters the long-range box was still armed, "
              "recorded or WRITTEN: nothing may touch it in a world with no "
              "distant fire in it";
  if (!inert)
    detail += divergedAt
                  ? Format(" -- THE WORLD MOVED: the hash diverges at t%u "
                           "between the plume and no-plume arms",
                           divergedAt)
                  : " -- the snapshot ring never delivered a comparable tick";
  if (!offExact)
    detail += " -- at render.farPlumeRange 0 the long-range box CHANGED, so a "
              "row is still being recorded for a band with no emitters in it";

  if (!sparseDrawn)
    detail += Format(" -- A TREE-SIZED FIRE IS INVISIBLE AT DISTANCE: peak %u "
                     "in the long-range box against the %u the renderer erodes "
                     "away, so only big fires get distant smoke",
                     e.spMax, spFloor);
  const bool ok = noted && drawn && placed && disjoint && flagged && quiet &&
                  inert && offExact && sparseDrawn;
  std::printf("gas-farplume2: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  plumes.Clear();
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
      {"gas-leave-overflow", "sim", {}, false, GateGasLeaveOverflow},
      {"gas-farplume", "sim", {}, false, GateGasFarPlume},
      {"gas-farplume2", "sim", {}, false, GateGasFarPlume2},
  };
  return g;
}

}  // namespace selftest
