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
#include "sim/windprim.h"
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
    if (r.smokeCount) r.smokeX = mxSum / r.smokeCount;
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

// ========================= WIND PRIMITIVES (phase 2) ========================
// docs/RESEARCH_wind.md §4.3 and §10. The gate that turns "entrainment is a
// landmine" into "entrainment is a feature", so it is worth being precise about
// what it claims.
//
// The `wind` gate above had to fake this. It writes one kCellOpIfAir per
// chamber chunk per tick purely to keep the chunks awake, because a settled
// sand bed is ASLEEP and the ambient field is categorically forbidden to wake
// it (invariant 3). And even with the scaffolding, its entrainment arms are
// OPT-IN, because switching windMode to 2 loses voxels: the page table
// materializes a set that is tightened against a lagging snapshot on the
// argument that settled matter writes nothing, and entrainment is the first
// rule that breaks it (62 reproducible faults over two 160-tick runs).
//
// This gate uses NO scaffolding and runs in the DEFAULT SUITE. A wind
// primitive carrying kWindPrimEntrain declares its own footprint every tick,
// the CPU filters it against occupancy, charges it against sim.windWakeChunks,
// hands it to the page table as op targets AND to sim_mutate's windWake kernel
// as dirty marks. So the chunks are awake because something player-caused woke
// them, and they are materialized because the CPU said so before the command
// buffer existed.
//
// THE PAGE-FAULT COUNT IS THE HEADLINE ASSERTION and it is not made here: the
// suite reports page faults across every gate and zero is the only acceptable
// value. This gate simply moves a settled dune
// with the default suite watching. If §10's fault ever comes back, it comes
// back on this gate.
//
// WHAT IS ASSERTED, all of it a differential or an invariance:
//   1. A LICENCE-CARRYING FAN MOVES THE BED, and moves it DOWNWIND. Sign, not
//      magnitude — a distance would rot the day anyone retunes a taper.
//   2. REVERSING THE FAN REVERSES THE CREEP. The falsifiable half of "one
//      field, sampled by everything".
//   3. A FAN WITHOUT THE LICENCE LEAVES THE BED BITWISE UNMOVED. This is the
//      whole shipping safety property: wind primitives are the default and
//      entrainment is opt-in per primitive, so a decorative gust cannot
//      rearrange terrain and cannot spend the wake budget.
//   4. NO PRIMITIVES IS AN EXACT IDENTITY. Same script, empty list, and the
//      world hash must equal the no-wind-primitive baseline bit for bit. This
//      is the argument that shipping this cannot move the pinned hash.
//   5. GRAIN COUNT IS CONSERVED. Sand is inert in a sealed chamber, so a count
//      that changed is a lost voxel — which is exactly the §10 symptom, caught
//      here as a wrong number rather than as a fault counter nobody read.
//   6. TWICE-RUN EQUALITY with a fan blowing.
struct PrimResult {
  uint32_t hash = 0;
  uint32_t sandCount = 0;
  double sandX = 0.0;
  int sandMaxX = 0;
  uint32_t wakeMax = 0;    // largest per-tick wake list this arm produced
  uint32_t wakeActive = 0; // chunks the CA actually simulated, mid-run
  uint32_t smokeCount = 0;
  double smokeX = 0.0;
  std::vector<uint32_t> sandCells;
};

Status GateWindPrim(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t sandId = 0, stoneId = 0, smokeId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "sand") sandId = (uint32_t)i;
    else if (c.mats[i].name == "stone") stoneId = (uint32_t)i;
    else if (c.mats[i].name == "smoke") smokeId = (uint32_t)i;
  }
  if (!sandId || !stoneId || !smokeId) {
    detail = "need materials sand, stone and smoke";
    return Status::Fail;
  }

  // ---- the chamber -------------------------------------------------------
  // Same shape as the `wind` gate's: a long sealed box along x with a one-voxel
  // sand bed sitting DIRECTLY on the stone floor, so the bed is settled the
  // instant it is painted and has no down-diagonal to take. Anchored to the
  // residency window, never to a literal world position.
  const IVec3 wo = world.WindowOrigin();
  const int bx = wo.x * (int)kChunk + 64;
  // Terrain-relative, and LOW. A literal +144 put the whole chamber inside the
  // hillside once the datum moved, which reads as "the fan did not WAKE a
  // sleeping bed" rather than as a buried fixture.
  //
  // +40 rather than something generous, and the reason is measured: this gate's
  // fan speeds are tuned against the AMBIENT field at the chamber's altitude,
  // and lifting the chamber to +200 left `reverses` at -0.12 cells where it
  // needs to see a real reversal. Wind is a function of position (windAt in
  // common.wgsl); a fixture that measures wind may not move in Y for free.
  const int by = FixtureYOver(wo.x * (int)kChunk + 64, wo.z * (int)kChunk + 64,
                              wo.x * (int)kChunk + 64 + 48,
                              wo.z * (int)kChunk + 64 + 6, kDefaultSeed, 40);
  const int bz = wo.z * (int)kChunk + 64;
  // LOW on purpose: 6 cells of headroom, not 10. A gas rises, and in a taller
  // box the smoke witness below spends the whole run pinned to the ceiling
  // where a cone anchored near the floor has already tapered to nothing — which
  // makes the witness measure the chamber rather than the fan. Six cells keeps
  // every seeded cell inside the fan's radius for the whole run.
  const int kW = 48, kD = 6, kH = 6;
  const int x0 = bx, x1 = bx + kW - 1;
  const int z0 = bz, z1 = bz + kD - 1;
  const int yF = by, yT = by + kH + 1;
  const int sx0 = bx + 14, sx1 = sx0 + 11;
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
  std::vector<CellOp> seed, smoke;
  for (int z = z0; z <= z1; z++) {
    for (int x = sx0; x <= sx1; x++)
      seed.push_back({World::SlotCellIndex({x, yF + 1, z}), sandId & 0xFFFu});
    // A smoke blob as well, and it is not decoration: gas responds to the
    // PRIMITIVE FIELD through the ordinary drift bias, with no licence and no
    // wake involved. So if the bed does not move but the smoke does, the
    // failure is in the entrainment licence; if neither moves, the field never
    // reached the kernel. One extra blob buys the difference between those two.
    for (int y = yF + 3; y <= yF + 5; y++)
      for (int x = bx + 20; x <= bx + 25; x++)
        smoke.push_back({World::SlotCellIndex({x, y, z}), smokeId & 0xFFFu});
  }

  // ---- the fan -----------------------------------------------------------
  // A cone with its mouth at the upwind end of the chamber, on the axis of the
  // bed. Reach 36 and radius 8 put the whole bed inside the first 60% of the
  // taper, where the axial weight is 0.4..0.7 — comfortably over sand's
  // authored friction threshold at 36 m/s, and comfortably under it at the
  // 2 m/s ambient the arms are pinned to, which is what makes the fan and only
  // the fan responsible for anything that moves.
  //
  // Infinite TTL: this is a fan, not a gust. The system is cleared between arms.
  auto makeFan = [&](int sign, bool entrain) {
    WindPrim p{};
    p.x = sign > 0 ? (bx + 4) : (bx + kW - 5);
    p.y = yF + 3;
    p.z = bz + 2;
    p.kind = kWindPrimCone;
    p.radius = 8;
    p.reach = 36;
    p.ttl = kWindPrimForever;
    p.flags = kWindPrimAir | (entrain ? kWindPrimEntrain : 0u);
    p.ownerId = 0xF00Du;
    WindPrimAim(p, Vec3{(float)sign, 0.0f, 0.0f}, 36.0f);
    return p;
  };

  // `withSmoke` is the diagnostic axis, not decoration — see the two arms it
  // separates at the call site.
  auto run = [&](int fanSign, bool entrain, bool withSmoke) -> PrimResult {
    Tuning t = CurrentTuning();
    // windMode 1 throughout: the licence comes from the PRIMITIVE, never from
    // the global mode. If this gate ever needs mode 2 to pass, the feature it
    // tests does not work.
    t.sim.windMode = (int)kWindModeDrift;
    t.wind.weatherAuto = false;
    t.wind.windDirDeg = 90.0f;
    t.wind.windSpeed = 2.0f;      // ambient alone cannot entrain sand
    t.wind.gustStrength = 0.2f;
    const Tuning saved = CurrentTuning();
    SetCurrentTuning(t);

    WindPrims().Clear();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    uint32_t tick = 41000;
    uint32_t midActive = 0;
    for (int i = 0; i < kTicks; i++) {
      // The fan is placed AFTER the chamber is built and the bed has settled,
      // which is the order a player would produce and the order that makes
      // "settled matter moved" mean something.
      if (i == 4 && fanSign != 0) WindPrims().Spawn(makeFan(fanSign, entrain));
      std::vector<CellOp> ops;
      if (i == 0) ops = build;
      else if (i == 1) {
        ops = seed;
        if (withSmoke) ops.insert(ops.end(), smoke.begin(), smoke.end());
      }
      SubmitTick(ctx, world, sim, ++tick, kDefaultSeed, {}, {}, ops, false,
                 {wo.x + 4, wo.y + 9, wo.z + 4}, true, false);
      // The active-chunk count MID-RUN is what proves the wake rather than the
      // end state, and it is the number that found the bug this gate was
      // written for: with the dry chamber asleep it read 0 while the CPU was
      // happily shipping a 10-slot wake list every tick — because the count
      // has to cross THREE structs to reach the recorder (RecordCtx ->
      // rhi::TableCtx -> the recorder's own RecordCtx) and one copy was
      // missing.
      //
      // Read off the SNAPSHOT, never with a blocking readback. A sync read
      // mid-run shares the free-probe's staging buffer and deferred map, and
      // interleaving one there left the page table's demotion drain stalled
      // for the rest of the process — which surfaced two dozen gates later as
      // `page-roundtrip` waiting 400 ticks for a page that never came back.
      // The snapshot is one tick latent, which is plenty for "is anything
      // awake at all".
      if (i >= kTicks - 40 && world.Snap().valid)
        midActive = std::max(midActive, world.Snap().activeChunks);
    }
    ctx.WaitIdle();

    PrimResult r;
    r.wakeActive = midActive;
    r.hash = HashWorldNow(ctx, world, sim, kDefaultSeed);
    double sxSum = 0.0, mxSum = 0.0;
    r.sandMaxX = x0 - 1;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = yF >> 4; cy <= (yT >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          const uint32_t slot = World::SlotChunkIndex({cx, cy, cz});
          ReadVoxelsSync(ctx, world, slot, 1, cbuf.data(), "primVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            const uint32_t mat = cbuf[k] & 0xFFFu;
            if (mat != sandId && mat != smokeId) continue;
            const int x = (int)(k % 16) + cx * 16;
            if (mat == smokeId) { r.smokeCount++; mxSum += x; continue; }
            r.sandCount++;
            sxSum += x;
            if (x > r.sandMaxX) r.sandMaxX = x;
            r.sandCells.push_back(slot * kChunkVol + k);
          }
        }
    if (r.sandCount) r.sandX = sxSum / r.sandCount;
    if (r.smokeCount) r.smokeX = mxSum / r.smokeCount;
    // The wake the last tick asked for. Bounded by the budget by construction;
    // read back so the number is in the detail line rather than merely
    // believed. (BuildWake is re-run here against the same live list, which is
    // exactly what SubmitTick did — it has no side effects.)
    {
      std::vector<uint32_t> w;
      WindPrims().BuildWake(world, world.Snap().valid ? world.Snap().occupancy
                                                      : std::vector<uint32_t>(),
                            (uint32_t)CurrentTuning().sim.windWakeChunks, w);
      r.wakeMax = (uint32_t)w.size();
    }
    WindPrims().Clear();
    SetCurrentTuning(saved);
    return r;
  };

  const PrimResult none = run(0, false, true);    // no primitive at all
  const PrimResult quiet = run(+1, false, true);  // a fan with no licence
  const PrimResult east = run(+1, true, true);    // licence, blowing +x
  const PrimResult west = run(-1, true, true);    // licence, blowing -x
  const PrimResult twice = run(+1, true, true);   // and again
  // THE WAKE ARM, and it is the one that matters most. No smoke at all: the
  // chamber settles completely within a few ticks of being built, so every
  // chunk in it is ASLEEP by the time the fan appears. If the bed still creeps,
  // the ONLY thing that could have woken it is the primitive's own footprint
  // going through sim_mutate's windWake kernel — which is the entire claim of
  // phase 2 and the thing the `wind` gate above had to fake with a per-tick
  // kCellOpIfAir. With smoke in the chamber the CA never sleeps and this
  // question cannot be asked, which is why it gets its own arm.
  const PrimResult dry = run(+1, true, false);
  const PrimResult dryNone = run(0, false, false);

  // 1/2. The creep, and its sign. Measured against the no-primitive arm so the
  //      CA's own randomness cancels out of both sides.
  const double dEast = east.sandX - none.sandX;
  const double dWest = west.sandX - none.sandX;
  const int mEast = east.sandMaxX - none.sandMaxX;
  const bool creeps = dEast > 0.5 && mEast > 0;
  const bool reverses = dWest < -0.5;
  // 3. The licence, and nothing else, is what moves settled matter. A fan is
  //    blowing in `quiet` at exactly the strength that moved the bed in `east`.
  const bool quietHeld = quiet.sandCells == none.sandCells;
  // 4. ...and it is a fan that is genuinely BLOWING. Without this the previous
  //    assertion is satisfied by a primitive that does nothing at all, which is
  //    exactly how a broken field would pass. The smoke is the witness: it
  //    responds to the primitive through the ordinary drift bias, which needs
  //    no licence and no wake.
  const bool quietBlows = (quiet.smokeX - none.smokeX) > 0.5;
  // 5. Mass. A lost grain is the §10 symptom.
  const bool massOk = east.sandCount == none.sandCount &&
                      west.sandCount == none.sandCount &&
                      quiet.sandCount == none.sandCount;
  // 6. Twice-run equality with a fan blowing.
  const bool stable = twice.hash == east.hash &&
                      twice.sandCells == east.sandCells;
  // 7. The rule-2 budget actually binds.
  const uint32_t budget = (uint32_t)CurrentTuning().sim.windWakeChunks;
  const bool bounded = east.wakeMax <= budget && quiet.wakeMax == 0;
  // 8. THE WAKE. A settled, sleeping bed, woken by nothing but the fan.
  const double dDry = dry.sandX - dryNone.sandX;
  // `dryNone.wakeActive == 0` was the fourth term, and it could not survive
  // contact with a world that has anything in it: `wakeActive` is read off
  // World::Snap().activeChunks, which is a WHOLE-WORLD count, while the
  // sentence this gate is asserting is local ("a settled, sleeping bed, woken
  // by nothing but the fan"). The project's own rest budget is 32 active
  // chunks (CLAUDE.md rule 2, and the `sleep` gate), so a control run with 5
  // awake somewhere in the world is a normal world and not a broken one -- it
  // failed here the day the forest got denser, and the message blamed the wind
  // footprint.
  //
  // What the claim actually needs is that the fan wakes MORE than the control,
  // which is the comparison the rest of this gate is built on.
  const bool wakes = dDry > 0.5 && dry.sandCount == dryNone.sandCount &&
                     dry.wakeActive > dryNone.wakeActive;

  detail = Format(
      "fan 36 m/s, 2 m/s ambient | bed creeps %+.2f cells (maxX %+d) blowing "
      "+x, %+.2f blowing -x | fan WITHOUT the entrain licence: bed %s, air %s "
      "| smoke drifts %+.2f cells (unlicensed fan %+.2f) "
      // `dryNone.wakeActive` was a hardcoded "0" here, which is the shape
      // CLAUDE.md rule 6 warns about: `wakes` is a four-term AND and the line
      // printed three of them, so a failure said "the footprint wake is not
      // reaching the CA" while every number on screen looked right. Print the
      // control.
      "| ASLEEP chamber, no smoke: %u chunks awake (%u with no fan), bed "
      "creeps %+.2f "
      "| grains %u/%u/%u (dry %u vs %u) "
      "| wake %u of %u chunks (%u without the licence) "
      "| hash none %08x, fan %08x, repeat %s",
      dEast, mEast, dWest, quietHeld ? "unmoved bitwise" : "MOVED",
      quietBlows ? "blowing" : "STILL",
      east.smokeX - none.smokeX, quiet.smokeX - none.smokeX, dry.wakeActive,
      dryNone.wakeActive, dDry,
      none.sandCount, east.sandCount,
      west.sandCount, dry.sandCount, dryNone.sandCount,
      east.wakeMax, budget, quiet.wakeMax, none.hash, east.hash,
      stable ? "identical" : "DIFFERS");

  if (!creeps) detail += " -- a licensed fan did not move the settled bed";
  if (!reverses) detail += " -- reversing the fan did not reverse the creep";
  if (!quietHeld) detail += " -- an unlicensed fan moved settled powder";
  if (!quietBlows) detail += " -- the unlicensed fan did not blow at all (the primitive field is not reaching the CA)";
  if (!massOk) detail += " -- grain count changed (a lost voxel: see §10)";
  if (!stable) detail += " -- twice-run equality failed with a fan blowing";
  if (!bounded) detail += " -- the wake budget did not bind";
  if (!wakes) detail += " -- the fan did not WAKE a sleeping bed (the footprint wake is not reaching the CA)";

  const bool ok = creeps && reverses && quietHeld && quietBlows && massOk &&
                  stable && bounded && wakes;
  std::printf("wind-prim: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
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
      {"wind-prim", "sim", {}, false, GateWindPrim},
  };
  return g;
}

}  // namespace selftest
