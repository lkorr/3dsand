// selftest_wind.cpp — the wind gate (docs/RESEARCH_wind.md phases 3 and 4).
//
// WHAT IS ACTUALLY UNDER TEST, and why it needs a differential rather than a
// threshold. Wind is a field: every consumer samples the same function, and the
// function drifts on purpose. So "the smoke moved" proves nothing (smoke moves
// anyway — the CA's direction rotation is random) and "the smoke moved 4.2
// cells" is a number that changes the day anyone retunes a gust. The claims
// that ARE falsifiable are about sign and about invariance:
//
//   * turn the direction knob 180 degrees and everything the field touches must
//     move the other way, from the same start, in the same chamber, on the same
//     ticks. One field sampled by everything is the whole design — this is what
//     would break the day a consumer grew its own copy.
//   * with the gate off, none of it happens at all, bit for bit. That is the
//     argument for shipping phases 3 and 4 while the pinned hash stays where it
//     is, and it is worth a measurement rather than an assertion.
//   * a SETTLED grain is not moved by the drift bias, however hard it blows —
//     invariant 4, and the one property separating "wind steers what is already
//     falling" from "wind is a second gravity". Asserted as BITWISE equality of
//     the whole sand bed against the gate-off run, not as a tolerance.
//   * and the same script run twice with wind on gives the same world hash,
//     which is the twice-run equality phase 3's acceptance asks for.
//
// ---- the entrainment arms are OPT-IN, and this is the reason ---------------
//
// `SANDVOX_WIND_ENTRAIN=1` adds two windMode-2 arms which do pass: the bed
// creeps ~22 cells downwind, mass is conserved in the chamber, and the two runs
// agree bit for bit. They are not in the default suite because windMode 2 trips
// the suite's page-fault counter — 62 faults over the two arms, at the same
// ticks in both, so deterministic rather than a race — and a red suite is worse
// than an untested opt-in.
//
// THE DIAGNOSIS, because "known failure" without one is just a shrug. The page
// table materializes [ (cpuDirty n hasMatter) u N26(...) ] u N26(opTargets),
// and cpuDirty is TIGHTENED against a lagging snapshot (PLAN_page_table.md
// §3.2). Under every pre-wind rule that tightening is sound: a chunk of settled
// powder writes nothing, so dropping it — and letting its empty sky neighbour's
// page retire — costs nothing. Entrainment is the first rule in the engine that
// makes SETTLED matter move, so a grain now steps into a neighbour the CPU was
// told would never be written, and the write is a lost voxel.
//
// The fix is the same missing piece that makes windMode 2 rule-2 unclean:
// phase 2's wind primitives put the wind's footprint on the mutation path,
// where it becomes an opTarget the CPU knows about — one mechanism, two
// symptoms, which is a good sign that it is the right mechanism. Until then
// mode 2 is a thing to look at rather than a thing to ship, exactly as
// kWindModeEntrain in world.h says.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sim/wind.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

struct WindArm {
  const char* label;
  int mode;         // sim.windMode
  float dirDeg;     // wind.windDirDeg — 0 = +Z, 90 = +X, 270 = -X
  float gasScale = 1.0f;    // sim.windGasScale, the dev CA-tier multiplier
};

struct WindResult {
  uint32_t hash = 0;
  uint32_t smokeCount = 0;
  double smokeX = 0.0;      // x centroid of surviving smoke, world cells
  uint32_t sandCount = 0;
  double sandX = 0.0;
  int sandMaxX = 0;         // furthest-downwind grain
  std::vector<uint32_t> sandCells;   // slot cell indices, chunk-major order
};

Status GateWind(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t smokeId = 0, sandId = 0, stoneId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "smoke") smokeId = (uint32_t)i;
    else if (c.mats[i].name == "sand") sandId = (uint32_t)i;
    else if (c.mats[i].name == "stone") stoneId = (uint32_t)i;
  }
  if (!smokeId || !sandId || !stoneId) {
    detail = "need materials smoke, sand and stone";
    return Status::Fail;
  }
  // The authored/derived coupling every measurement below depends on. Asserted
  // rather than assumed: with `"wind": {"response": 0}` on smoke, every number
  // here goes quietly to zero and the gate would report a broken field instead
  // of a retuned material.
  const uint32_t smokeResp = c.mats[smokeId].windResponse;
  const uint32_t sandFric = c.mats[sandId].windFriction;
  if (smokeResp == 0) {
    detail = "smoke authors windResponse 0 — nothing here can measure anything";
    return Status::Fail;
  }

  // ---- the chamber --------------------------------------------------------
  // A long, low sealed box: 48 cells along x (the wind axis), 6 in z, 10 tall.
  // Stone shell, air inside, a one-voxel sand bed on the floor in the middle
  // and a smoke blob in the air above it. Anchored to the residency window,
  // never to a literal world position (CLAUDE.md) — a gate that ran after
  // `streaming` would otherwise build into space.
  const IVec3 wo = world.WindowOrigin();
  const int bx = wo.x * (int)kChunk + 64;
  const int by = wo.y * (int)kChunk + 112;
  const int bz = wo.z * (int)kChunk + 64;
  const int kW = 48, kD = 6, kH = 10;
  const int x0 = bx, x1 = bx + kW - 1;
  const int z0 = bz, z1 = bz + kD - 1;
  const int yF = by, yT = by + kH + 1;     // stone floor and ceiling
  // The bed sits DIRECTLY on the floor, which is what makes it settled the
  // instant it is painted: no slump to wait out, and not one down-diagonal it
  // could take even with the bias fully engaged, because every one of them
  // lands in stone. Without that the "settled matter does not move" assertion
  // would be measuring a race against gravity.
  const int sx0 = bx + 14, sx1 = sx0 + 11;
  const int mx0 = bx + 20, mx1 = mx0 + 5;      // the smoke blob
  const int my0 = yF + 4, my1 = yF + 6;
  const int kTicks = 160;

  std::vector<CellOp> build;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = yF; y <= yT; y++) {
        const bool shell = y == yF || y == yT || x < x0 || x > x1 ||
                           z < z0 || z > z1;
        build.push_back({World::SlotCellIndex({x, y, z}),
                         shell ? (stoneId & 0xFFFu) : 0u});
      }
  std::vector<CellOp> seed;
  for (int z = z0; z <= z1; z++) {
    for (int x = sx0; x <= sx1; x++)
      seed.push_back({World::SlotCellIndex({x, yF + 1, z}), sandId & 0xFFFu});
    for (int y = my0; y <= my1; y++)
      for (int x = mx0; x <= mx1; x++)
        seed.push_back({World::SlotCellIndex({x, y, z}), smokeId & 0xFFFu});
  }
  // THE WAKE OP — the one piece of scaffolding here, and it stands in for
  // something real. The ambient field is forbidden to wake a chunk (invariant
  // 3), so a settled bed in an open world SLEEPS and no wind rule ever runs on
  // it, correctly. Phase 2 supplies the missing half: a primitive dirty-marks
  // its own bounded footprint through the mutation path. Until it exists, one
  // write per chamber chunk per tick is that footprint.
  //
  // Parked just under the ceiling and flagged kCellOpIfAir. Both halves are
  // needed and the first was learned the hard way: one cell above the bed, this
  // op silently ERASED the first grain saltation lifted into it, and the gate
  // read as a mass leak in the CA.
  std::vector<CellOp> wake;
  for (int cx = x0 >> 4; cx <= (x1 >> 4); cx++)
    wake.push_back({World::SlotCellIndex({cx * 16 + 8, yT - 1, z0}),
                    kCellOpIfAir});

  auto run = [&](const WindArm& arm) -> WindResult {
    Tuning t = CurrentTuning();
    t.sim.windMode = arm.mode;
    // Pinned weather. An evolving field makes two runs incomparable, which is
    // the entire reason weatherAuto exists as a switch (sim/wind.h).
    t.wind.weatherAuto = false;
    t.wind.windDirDeg = arm.dirDeg;
    t.wind.windSpeed = 20.0f;   // a storm: over sand's entrainment threshold
    t.wind.gustStrength = 0.4f;
    t.sim.windGasScale = arm.gasScale;
    const Tuning saved = CurrentTuning();
    SetCurrentTuning(t);

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    uint32_t tick = 40000;
    for (int i = 0; i < kTicks; i++) {
      const std::vector<CellOp>& ops = i == 0 ? build : (i == 1 ? seed : wake);
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, ops, false,
                 {wo.x + 4, wo.y + 7, wo.z + 4}, true, false);
    }
    ctx.WaitIdle();

    WindResult r;
    r.hash = HashWorldNow(ctx, world, sim, kDefaultSeed);
    double sxSum = 0.0, mxSum = 0.0;
    r.sandMaxX = x0 - 1;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = yF >> 4; cy <= (yT >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          const uint32_t slot = World::SlotChunkIndex({cx, cy, cz});
          ReadVoxelsSync(ctx, world, slot, 1, cbuf.data(), "windVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            const uint32_t mat = cbuf[k] & 0xFFFu;
            if (mat != smokeId && mat != sandId) continue;
            const int x = (int)(k % 16) + cx * 16;
            if (mat == smokeId) { r.smokeCount++; mxSum += x; continue; }
            r.sandCount++;
            sxSum += x;
            if (x > r.sandMaxX) r.sandMaxX = x;
            r.sandCells.push_back(slot * kChunkVol + k);
          }
        }
    if (r.smokeCount) r.smokeX = mxSum / r.smokeCount;
    if (r.sandCount) r.sandX = sxSum / r.sandCount;
    SetCurrentTuning(saved);
    return r;
  };

  const WindResult off = run({"off", (int)kWindModeOff, 90.0f});
  const WindResult east = run({"drift +x", (int)kWindModeDrift, 90.0f});
  const WindResult west = run({"drift -x", (int)kWindModeDrift, 270.0f});
  const WindResult twice = run({"drift +x (repeat)", (int)kWindModeDrift, 90.0f});
  // The dev force multiplier, CA tier. Same wind, same script, 8x the bias.
  const WindResult hard = run({"drift +x x8", (int)kWindModeDrift, 90.0f, 8.0f});

  // 1. The gate is real: turning it on has to CHANGE something, or every other
  //    assertion here is satisfied vacuously by a field that returns zero.
  const bool live = east.hash != off.hash;
  // 2. One field, sampled by everything: reversing the direction reverses the
  //    displacement. Measured against the gate-off run so the CA's own random
  //    spread cancels out of both sides. A cell is the floor — the observed
  //    figures are tens of cells, so this fails loudly rather than marginally.
  const double dEast = east.smokeX - off.smokeX;
  const double dWest = west.smokeX - off.smokeX;
  const bool drifts = dEast > 1.0 && dWest < -1.0;
  // 3. Invariant 4, BITWISE: the drift bias must not move settled matter.
  const bool bedHeld = east.sandCells == off.sandCells &&
                       west.sandCells == off.sandCells;
  // 4. Twice-run equality with wind on — phase 3's stated acceptance.
  const bool stable = twice.hash == east.hash &&
                      twice.sandCells == east.sandCells;
  // 5. Mass. Sand is inert here (no reaction consumes it, the chamber is
  //    sealed), so a grain count that moved is a real bug however good the
  //    centroids look. This is the assertion that caught the wake op erasing a
  //    saltating grain.
  const bool massOk = east.sandCount == off.sandCount &&
                      west.sandCount == off.sandCount;
  // 6. The dev force multiplier, REPORTED AND NOT VOTED ON. The arm runs and
  //    its number goes in the detail line beside the 1x number, which is where
  //    anyone comparing them would look; it is deliberately not part of the
  //    pass condition. Two reasons, and the first is enough: this threshold has
  //    never been measured, and an assertion nobody has watched pass is a
  //    coin-flip that turns the suite red on someone else's commit. The second
  //    is that the bias saturates at certainty, so the knob-to-distance
  //    relationship is not linear and any factor written here would rot.
  //
  //    The bed equality IS worth an eye even so — a multiplier that quietly
  //    turned the drift bias into entrainment would be a far worse bug than one
  //    that did nothing — so it is printed rather than dropped.
  const double dHard = hard.smokeX - off.smokeX;
  const bool hardBedHeld = hard.sandCells == off.sandCells &&
                           hard.sandCount == off.sandCount;

  // ---- the opt-in entrainment arms (see the header) -----------------------
  bool creeps = true;
  bool entrainStable = true;
  double dune = 0.0;
  int duneMax = 0;
  const bool ranEntrain = getenv("SANDVOX_WIND_ENTRAIN") != nullptr;
  if (ranEntrain) {
    const WindResult a = run({"entrain +x", (int)kWindModeEntrain, 90.0f});
    const WindResult b = run({"entrain +x (repeat)", (int)kWindModeEntrain, 90.0f});
    dune = a.sandX - off.sandX;
    duneMax = a.sandMaxX - off.sandMaxX;
    creeps = dune > 0.5 && duneMax > 0 && a.sandCount == off.sandCount;
    entrainStable = b.hash == a.hash && b.sandCells == a.sandCells;
  }

  detail = Format(
      "smoke resp %u / sand friction %u | smoke drifts %+.2f / %+.2f cells "
      "against mode 0 (x=%.2f); %+.2f at gasScale 8x (reported, not asserted) "
      "| settled bed %s under drift, %s at 8x | grains %u/%u/%u "
      "| hash off %08x, on %08x, repeat %s | entrain %s",
      smokeResp, sandFric, dEast, dWest, off.smokeX, dHard,
      bedHeld ? "unmoved bitwise" : "MOVED",
      hardBedHeld ? "unmoved" : "MOVED", off.sandCount, east.sandCount,
      west.sandCount, off.hash, east.hash, stable ? "identical" : "DIFFERS",
      ranEntrain
          ? Format("bed creeps %+.2f (maxX %+d), repeat %s", dune, duneMax,
                   entrainStable ? "identical" : "DIFFERS").c_str()
          : "SKIPPED (windMode 2 trips the page-fault counter — see the file "
            "header; SANDVOX_WIND_ENTRAIN=1 to run it)");

  if (!live) detail += " -- windMode 1 changed nothing";
  if (!drifts) detail += " -- smoke did not follow the direction knob";
  if (!bedHeld) detail += " -- drift bias moved SETTLED powder (invariant 4)";
  if (!stable) detail += " -- twice-run equality failed with wind on";
  if (!massOk) detail += " -- grain count changed";
  if (!creeps) detail += " -- entrainment did not lift the bed";
  if (!entrainStable) detail += " -- entrainment was not reproducible";

  const bool ok = live && drifts && bedHeld && stable && massOk && creeps &&
                  entrainStable;
  std::printf("wind: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ============================ THE GAS VERTICAL MODEL =========================
// The `wind` gate above builds a chamber with a CEILING, so its smoke reaches
// the roof in a few ticks and everything it measures after that is lateral
// spread through the fallback chain. That is why it stayed green through the
// entire period in which a freely-rising plume did not lean at all: a gas with
// open sky above it took the unconditional step-1 rise every tick and never
// reached a line of wind code. This gate is the one that can see that, and it
// exists because the bug it covers shipped green once already.
//
// OPEN SHAFT, no roof. Two things are asserted, and they are exactly the two
// design decisions in the model (sim_step.wgsl, THE GAS VERTICAL MODEL):
//
//   1. THE PLUME LEANS. Horizontal displacement against the wind-off run must
//      be real and must follow the direction knob. This is the fix.
//   2. THE CLIMB IS NOT PAID FOR. The lean is spent on an UP-diagonal, so a
//      leaning plume must rise as fast as a still one. Had the horizontal share
//      been taken out of the rise instead, this is the assertion that would
//      fail - which is the whole reason it is written as a floor on height and
//      not as a comment.
struct GasResult {
  uint32_t count = 0;
  double x = 0.0, y = 0.0;   // smoke centroid, world cells
};

Status GateWindGas(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t smokeId = 0, stoneId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "smoke") smokeId = (uint32_t)i;
    else if (c.mats[i].name == "stone") stoneId = (uint32_t)i;
  }
  if (!smokeId || !stoneId) {
    detail = "need materials smoke and stone";
    return Status::Fail;
  }
  if (c.mats[smokeId].windResponse == 0) {
    detail = "smoke authors windResponse 0 - nothing here can measure anything";
    return Status::Fail;
  }

  // A clear box: floor and side walls, OPEN AT THE TOP. Sized so a plume
  // leaning at the saturated 45 degrees stays inside it for the whole run --
  // a gas takes one move per SUBSTEP, so ~2 cells of rise a tick, and the lean
  // can match that cell for cell.
  const IVec3 wo = world.WindowOrigin();
  const int bx = wo.x * (int)kChunk + 32;
  const int by = wo.y * (int)kChunk + 96;
  const int bz = wo.z * (int)kChunk + 64;
  const int kW = 60, kD = 6, kH = 44, kTicks = 18;
  const int x0 = bx, x1 = bx + kW - 1;
  const int z0 = bz, z1 = bz + kD - 1;
  const int yF = by, yT = by + kH;

  std::vector<CellOp> build;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = yF; y <= yT; y++) {
        // Floor and side walls; no roof, and the interior is cleared to air so
        // the measurement cannot pick up whatever worldgen left here.
        const bool wall = y == yF || x < x0 || x > x1 || z < z0 || z > z1;
        build.push_back({World::SlotCellIndex({x, y, z}),
                         wall ? (stoneId & 0xFFFu) : 0u});
      }
  // The puff: low and upwind, so it has the whole shaft to climb and the whole
  // width to lean across before anything can clip it.
  std::vector<CellOp> seed;
  for (int z = z0 + 1; z <= z1 - 1; z++)
    for (int y = yF + 2; y <= yF + 4; y++)
      for (int x = bx + 24; x <= bx + 29; x++)
        seed.push_back({World::SlotCellIndex({x, y, z}), smokeId & 0xFFFu});

  auto run = [&](int mode, float dirDeg) -> GasResult {
    Tuning t = CurrentTuning();
    t.sim.windMode = mode;
    t.wind.weatherAuto = false;   // an evolving field makes arms incomparable
    t.wind.windDirDeg = dirDeg;
    t.wind.windSpeed = 20.0f;     // past sim.windDriftSpeed: the lean saturates
    // GUSTS TURNED DOWN, and not for convenience. The gust bands carry a
    // vertical component (WINDQ_VERT, 0.18), and the model is ASYMMETRIC about
    // it by design: `rise` is already at certainty in calm air, so an updraft
    // cannot add height, while a downdraft subtracts it. A gusty field
    // therefore lowers a plume slightly no matter how the horizontal share is
    // spent -- at gustStrength 0.4 it costs ~2 cells over this run, which is
    // the same order as the thing being measured. Quieting the gusts leaves
    // the height difference reading ONE decision (up-diagonal vs flat step),
    // which is what this assertion is for; the alternative costs ~half the
    // climb, so the slack below is still nowhere near tight.
    t.wind.gustStrength = 0.1f;
    const Tuning saved = CurrentTuning();
    SetCurrentTuning(t);

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    const std::vector<CellOp> none;
    uint32_t tick = 40000;
    for (int i = 0; i < kTicks; i++) {
      const std::vector<CellOp>& ops = i == 0 ? build : (i == 1 ? seed : none);
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, ops, false,
                 {wo.x + 2, wo.y + 6, wo.z + 4}, true, false);
    }
    ctx.WaitIdle();

    GasResult r;
    double xs = 0.0, ys = 0.0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = yF >> 4; cy <= (yT >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          const uint32_t slot = World::SlotChunkIndex({cx, cy, cz});
          ReadVoxelsSync(ctx, world, slot, 1, cbuf.data(), "gasVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != smokeId) continue;
            r.count++;
            xs += (double)((int)(k % 16) + cx * 16);
            ys += (double)((int)((k / 16) % 16) + cy * 16);
          }
        }
    if (r.count) { r.x = xs / r.count; r.y = ys / r.count; }
    SetCurrentTuning(saved);
    return r;
  };

  const GasResult off  = run((int)kWindModeOff,   90.0f);
  const GasResult east = run((int)kWindModeDrift, 90.0f);
  const GasResult west = run((int)kWindModeDrift, 270.0f);

  const double dEast = east.x - off.x;
  const double dWest = west.x - off.x;
  // 1. The plume leans, and it leans the way the knob points. A cell is the
  //    floor; the observed figures are many cells, so this fails loudly.
  const bool leans = dEast > 1.0 && dWest < -1.0;
  // 2. The lean was not bought out of the climb. Slack of two cells absorbs the
  //    RNG spread and the gust field's own vertical component (WINDQ_VERT is
  //    0.18, so a downward gust legitimately costs a little height); paying for
  //    the drift out of the rise rate would cost far more than that, since at
  //    this wind the lean is saturated and EVERY rise would have become a
  //    sideways step.
  const bool climbs = east.y > off.y - 2.0 && west.y > off.y - 2.0;
  // 3. Mass. Smoke is not inert (it thins out), so this is a both-arms
  //    comparison rather than a fixed count: the wind must not create or eat
  //    smoke relative to the still run.
  const bool massOk = east.count > 0 && off.count > 0 &&
                      east.count * 2 > off.count && off.count * 2 > east.count;

  detail = Format(
      "plume leans %+.2f / %+.2f cells against mode 0 | climb %.2f vs %.2f "
      "still (the lean is spent on an up-diagonal, so it must not cost height) "
      "| smoke %u/%u/%u",
      dEast, dWest, east.y, off.y, off.count, east.count, west.count);
  if (!leans) detail += " -- a FREELY RISING plume did not follow the wind";
  if (!climbs) detail += " -- the lean was paid for out of the climb rate";
  if (!massOk) detail += " -- smoke count diverged from the still run";

  const bool ok = leans && climbs && massOk;
  std::printf("wind-gas: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WindGates() {
  static const std::vector<Gate> g = {
      {"wind", "sim", {}, false, GateWind},
      {"wind-gas", "sim", {}, false, GateWindGas},
  };
  return g;
}

}  // namespace selftest
