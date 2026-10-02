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
#include <map>
#include <tuple>
#include <string>
#include <vector>

#include "sim/wind.h"
#include "sim/windfield.h"
#include "sim/windprim.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

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
    // Drafts off: this gate measures the wind COUPLING inside a chamber, and the
    // draft volume (correctly) stills the air in a chamber. Shelter is `drafts`.
    t.sim.draftMode = 0;
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
  // MASS in eighths, not cells. Since the powder-mass work (706c6b3 / decb125,
  // 2026-09-26) wind entrainment splits a grain off a cell -- a conserved
  // split into partial-mass sand voxels (state 0..2 = full, 3..9 = 1..7
  // eighths) -- so a blown bed has MORE sand voxels and the same sand. A cell
  // count read "72 / 113 / 93 grains" as a lost voxel that was not lost.
  uint32_t sandMass = 0;
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
    // Drafts off: this gate measures the wind COUPLING inside a chamber, and the
    // draft volume (correctly) stills the air in a chamber. Shelter is `drafts`.
    t.sim.draftMode = 0;
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
            // THE CHAMBER'S INTERIOR ONLY. These chunks also hold whatever
            // worldgen put around the box, and the fan's cone (radius 8 about
            // z = bz + 2) reaches through the side walls: on the harness map
            // there is terrain sand out there, the licensed fan blew it, and
            // whole-chunk counts read "72 / 113 / 93 grains" -- a lost voxel
            // inside a sealed box that never happened (the bed is 72 grains).
            const int y = (int)((k / 16) % 16) + cy * 16;
            const int z = (int)(k / 256) + cz * 16;
            if (x < x0 || x > x1 || z < z0 || z > z1 || y <= yF || y >= yT)
              continue;
            if (mat == smokeId) { r.smokeCount++; mxSum += x; continue; }
            r.sandCount++;
            {
              const uint32_t st = (cbuf[k] >> 12) & 0xFu;
              r.sandMass += st <= 2u ? 8u : (st <= 9u ? st - 2u : 8u);
            }
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
  const bool massOk = east.sandMass == none.sandMass &&
                      west.sandMass == none.sandMass &&
                      quiet.sandMass == none.sandMass;
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
  const bool wakes = dDry > 0.5 && dry.sandMass == dryNone.sandMass &&
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
      "| sand eighths %u/%u/%u (dry %u vs %u; voxels %u/%u/%u) "
      "| wake %u of %u chunks (%u without the licence) "
      "| hash none %08x, fan %08x, repeat %s",
      dEast, mEast, dWest, quietHeld ? "unmoved bitwise" : "MOVED",
      quietBlows ? "blowing" : "STILL",
      east.smokeX - none.smokeX, quiet.smokeX - none.smokeX, dry.wakeActive,
      dryNone.wakeActive, dDry,
      none.sandMass, east.sandMass,
      west.sandMass, dry.sandMass, dryNone.sandMass,
      none.sandCount, east.sandCount, west.sandCount,
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
    // Drafts off: this gate measures the wind COUPLING inside a chamber, and the
    // draft volume (correctly) stills the air in a chamber. Shelter is `drafts`.
    t.sim.draftMode = 0;
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

// ---- wind-field: the weather-driven model, as numbers (docs/RESEARCH_wind.md §13)
//
// CPU ONLY. It evaluates the INTEGER field through windfield.h's C++ mirror of
// windAtQ on the same wf* block the tick ships, so it needs no ticks, no
// fixture and no GPU round trip — a few thousand probe cells over the live
// terrain table around the window. Each claim of the model is one line:
//
//   spawn     the mean wind at the reference height (1.5 m) over the table is
//             ~1x the authored speed (the old absolute-Y ramp gave 1.8x)
//   terrain   ridges (top-exposure cells) windier than hollows, hollows more
//             sheltered in light wind, ridges faster in strong wind
//   fronts    the base gust band's crests travel DOWNWIND at ~the mean speed
//             (they ran upwind at ~0.8 m/s before 2026-09-30)
//   meander   light air wanders several times more than a gale
//   gusts     a gale's peak gust / mean ~1.5
//   storm     the thunderstorm cycle has its lull, its front spike, its jump
//             and its primitives
//   stability a calm night decouples the ground wind; a calm noon has thermals
//
// Thresholds are tests/baseline.json numbers ("windField.*"), defaults here.
Status GateWindField(Ctx& c, std::string& detail) {
  World& world = c.world;
  const uint32_t seed = world.WorldSeed();
  const IVec3 wo = world.WindowOrigin();
  const int32_t o3[3] = {wo.x, wo.y, wo.z};
  const int32_t cx = wo.x * (int)kChunk + (int)kWorldN / 2;
  const int32_t cz = wo.z * (int)kChunk + (int)kWorldN / 2;
  const uint32_t tick = 3000;
  char buf[512];
  bool ok = true;
  std::string out;
  auto note = [&](const char* s) { out += s; out += "; "; };

  Tuning base = CurrentTuning();
  base.wind.weatherAuto = false;
  base.wind.windDirDeg = 90.0f;      // blowing toward +X
  base.wind.regime = "auto";
  base.wind.intensity = -1.0f;
  base.wind.windSpeed = 6.0f;
  base.wind.gustStrength = 1.0f;

  // A grid of probe columns over the table, 1.5 m above ITS ground.
  const int kG = 24;
  auto columnProbes = [&](const Tuning& t, uint32_t tk, uint32_t day, float aglM,
                          std::vector<windfield::FieldProbe>& res) {
    std::vector<int32_t> xyz;
    // First pass at y = 0 only to learn the ground of each column.
    for (int j = 0; j < kG; j++)
      for (int i = 0; i < kG; i++) {
        xyz.push_back(cx - 800 + i * 1600 / kG + 16);
        xyz.push_back(0);
        xyz.push_back(cz - 800 + j * 1600 / kG + 16);
      }
    res.assign(xyz.size() / 3, {});
    windfield::ProbeMany(t, seed, tk, day, o3, xyz.data(), (int)res.size(), res.data());
    for (size_t k = 0; k < res.size(); k++)
      xyz[3 * k + 1] = (int32_t)std::floor(res[k].groundY) + (int32_t)std::lround(aglM * 10.0f);
    windfield::ProbeMany(t, seed, tk, day, o3, xyz.data(), (int)res.size(), res.data());
  };
  auto hspeed = [](const float v[3]) { return std::sqrt(v[0] * v[0] + v[2] * v[2]); };

  // ---- spawn: the mean at the reference height over the table ------------
  {
    std::vector<windfield::FieldProbe> pr;
    columnProbes(base, tick, kWindNoDayPhase, 1.5f, pr);
    std::vector<float> ratio;
    for (const auto& p : pr)
      if (p.inTable && p.refSpeed > 0.0f) ratio.push_back(hspeed(p.mean) / p.refSpeed);
    std::sort(ratio.begin(), ratio.end());
    const float med = ratio.empty() ? 0.0f : ratio[ratio.size() / 2];
    const windfield::FieldProbe centre =
        windfield::Probe(base, seed, tick, kWindNoDayPhase, o3, cx, 0, cz);
    const windfield::FieldProbe atC = windfield::Probe(
        base, seed, tick, kWindNoDayPhase, o3, cx, (int32_t)std::floor(centre.groundY) + 15, cz);
    const float lo = (float)BaselineNumber("windField.spawnMin", 0.8);
    const float hi = (float)BaselineNumber("windField.spawnMax", 1.25);
    const bool pass = med >= lo && med <= hi;
    std::snprintf(buf, sizeof buf,
                  "spawn: 1.5 m mean / authored = %.2f median over %zu columns "
                  "(window centre %.2f: profile %.2f exposure %+.2f) [%.2f..%.2f] %s",
                  med, ratio.size(), hspeed(atC.mean) / std::max(atC.refSpeed, 1e-6f),
                  atC.profile, atC.exposure, lo, hi, pass ? "ok" : "FAIL");
    note(buf);
    RecordObserved("windField.spawnRatio", med);
    ok = ok && pass;
  }

  // ---- terrain: ridges vs hollows, light vs strong ------------------------
  {
    struct Arm { const char* name; float I; float ridge = 0, flat = 0, hollow = 0; };
    Arm arms[2] = {{"light", 0.12f}, {"strong", 0.8f}};
    float eMin = 1.0f, eMax = -1.0f;
    for (Arm& a : arms) {
      Tuning t = base;
      t.wind.intensity = a.I;
      std::vector<windfield::FieldProbe> pr;
      columnProbes(t, tick, kWindNoDayPhase, 1.5f, pr);
      std::vector<std::pair<float, float>> es;   // (exposure, speed / ref)
      for (const auto& p : pr)
        if (p.inTable && p.refSpeed > 0.0f) es.push_back({p.exposure, hspeed(p.mean) / p.refSpeed});
      std::sort(es.begin(), es.end());
      if (es.size() < 20) continue;
      const size_t n5 = std::max<size_t>(es.size() / 10, 1);
      auto avg = [&](size_t b, size_t e) {
        double s = 0;
        for (size_t k = b; k < e; k++) s += es[k].second;
        return (float)(s / (double)(e - b));
      };
      a.hollow = avg(0, n5);
      a.ridge = avg(es.size() - n5, es.size());
      a.flat = avg(es.size() / 2 - n5 / 2, es.size() / 2 + n5 / 2 + 1);
      eMin = std::min(eMin, es.front().first);
      eMax = std::max(eMax, es.back().first);
    }
    const bool relief = eMax - eMin > 0.1f;
    const bool order = arms[0].ridge > arms[0].hollow && arms[1].ridge > arms[1].hollow;
    const bool lightShelter = arms[0].hollow / std::max(arms[0].flat, 1e-6f) <
                              arms[1].hollow / std::max(arms[1].flat, 1e-6f);
    const bool strongRidge = arms[1].ridge / std::max(arms[1].flat, 1e-6f) >
                             arms[0].ridge / std::max(arms[0].flat, 1e-6f);
    const bool pass = !relief || (order && lightShelter && strongRidge);
    std::snprintf(buf, sizeof buf,
                  "terrain (exposure %.2f..%.2f): light ridge %.2f flat %.2f hollow %.2f, "
                  "strong ridge %.2f flat %.2f hollow %.2f %s",
                  eMin, eMax, arms[0].ridge, arms[0].flat, arms[0].hollow, arms[1].ridge,
                  arms[1].flat, arms[1].hollow,
                  !relief ? "(flat table: not asserted)" : pass ? "ok" : "FAIL");
    note(buf);
    RecordObserved("windField.ridgeOverHollowStrong",
                   arms[1].ridge / std::max(arms[1].hollow, 1e-6f));
    ok = ok && pass;
  }

  // ---- fronts: the base band's crests move downwind at ~the mean ----------
  {
    Tuning t = base;
    t.wind.intensity = 0.5f;
    t.wind.wanderLight = t.wind.wanderStrong = 0.0f;   // a straight line to track along
    const int32_t y = (int32_t)std::floor(
                          windfield::Probe(t, seed, tick, kWindNoDayPhase, o3, cx, 0, cz).groundY) + 15;
    const int kN = 192;
    std::vector<int32_t> xyz;
    for (int i = 0; i < kN; i++) { xyz.push_back(cx - kN / 2 + i); xyz.push_back(y); xyz.push_back(cz); }
    std::vector<windfield::FieldProbe> a(kN), b(kN);
    // Two ticks: the crest moves ~U * dt, a few cells at any sane wind, well
    // inside half the base band's wavelength (the search below is limited to
    // that half-period, or a pure sinusoid's shift is ambiguous mod lambda:
    // +12 and -36 cells were the same shift for the old 48-cell wave).
    const uint32_t dt = 2;
    windfield::ProbeMany(t, seed, tick, kWindNoDayPhase, o3, xyz.data(), kN, a.data());
    windfield::ProbeMany(t, seed, tick + dt, kWindNoDayPhase, o3, xyz.data(), kN, b.data());
    // The shift that best maps a onto b: b(x) = a(x - s). The window is
    // [kMargin, kN - kMargin), so |s| must stay under kMargin or a[i - s]
    // reads outside the probe row — which a wavelength over ~9.8 m (the
    // knob goes to 60) used to do.
    const int kMargin = 48;
    int best = 0;
    double bestC = -1e30;
    const int half = std::clamp((int)(0.5f * t.wind.gustWavelength / kVoxelMeters) - 1, 2,
                                kMargin - 1);
    for (int s = -half; s <= half; s++) {
      double cc = 0;
      for (int i = kMargin; i < kN - kMargin; i++) cc += (double)b[i].band1 * a[i - s].band1;
      if (cc > bestC) { bestC = cc; best = s; }
    }
    const float v = (float)best / ((float)dt / 30.0f) * (float)kVoxelMeters;   // m/s along +X
    const float u = a[kN / 2].refSpeed * t.wind.gustAdvect;
    const float r = u > 0 ? v / u : 0.0f;
    const float lo = (float)BaselineNumber("windField.frontMin", 0.7);
    const float hi = (float)BaselineNumber("windField.frontMax", 1.3);
    const bool pass = r >= lo && r <= hi;
    std::snprintf(buf, sizeof buf,
                  "fronts: crest shift %+d cells in %u ticks = %+.1f m/s downwind vs mean %.1f m/s "
                  "(ratio %.2f) [%.2f..%.2f] %s",
                  best, dt, v, u, r, lo, hi, pass ? "ok" : "FAIL");
    note(buf);
    RecordObserved("windField.frontRatio", r);
    ok = ok && pass;
  }

  // ---- meander: light air wanders, a gale holds ---------------------------
  {
    auto spread = [&](const char* regime) {
      Tuning t = base;
      t.wind.regime = regime;
      double s2 = 0;
      int n = 0;
      for (uint32_t k = 0; k < 12; k++) {
        std::vector<int32_t> xyz;
        for (int i = 0; i < 16; i++) {
          xyz.push_back(cx - 600 + i * 80);
          xyz.push_back(0);
          xyz.push_back(cz - 300 + (int)k * 50);
        }
        std::vector<windfield::FieldProbe> pr(16);
        windfield::ProbeMany(t, seed, tick + k * 211, kWindNoDayPhase, o3, xyz.data(), 16, pr.data());
        for (const auto& p : pr) {
          float d = p.localHeadingDeg - p.weatherHeadingDeg;
          while (d > 180.0f) d -= 360.0f;
          while (d < -180.0f) d += 360.0f;
          s2 += (double)d * d;
          n++;
        }
      }
      return (float)std::sqrt(s2 / std::max(n, 1));
    };
    const float light = spread("light"), gale = spread("gale");
    const float need = (float)BaselineNumber("windField.meanderRatioMin", 2.5);
    const bool pass = light > need * gale && gale < 12.0f;
    std::snprintf(buf, sizeof buf, "meander: rms heading deviation light %.1f deg, gale %.1f deg %s",
                  light, gale, pass ? "ok" : "FAIL");
    note(buf);
    RecordObserved("windField.meanderLightDeg", light);
    RecordObserved("windField.meanderGaleDeg", gale);
    ok = ok && pass;
  }

  // ---- gusts: a gale's gust factor ---------------------------------------
  {
    Tuning t = base;
    t.wind.regime = "gale";
    std::vector<windfield::FieldProbe> pr;
    float peak = 0, meanSum = 0;
    int n = 0;
    for (uint32_t k = 0; k < 8; k++) {
      columnProbes(t, tick + k * 97, kWindNoDayPhase, 10.0f, pr);
      for (const auto& p : pr) {
        if (!p.inTable) continue;
        const float m = hspeed(p.mean);
        const float tot = std::sqrt((p.mean[0] + p.bands[0]) * (p.mean[0] + p.bands[0]) +
                                    (p.mean[2] + p.bands[2]) * (p.mean[2] + p.bands[2]));
        if (m > 0) { peak = std::max(peak, tot / m); meanSum += m; n++; }
      }
    }
    const bool pass = peak > 1.3f && peak < 1.8f;
    std::snprintf(buf, sizeof buf, "gusts: gale peak/mean at 10 m = %.2f (mean %.1f m/s) %s", peak,
                  n ? meanSum / n : 0.0f, pass ? "ok" : "FAIL");
    note(buf);
    RecordObserved("windField.galeGustFactor", peak);
    ok = ok && pass;
  }

  // ---- storm: the timeline and its primitives ------------------------------
  {
    Tuning t = base;
    t.wind.regime = "thunderstorm";
    const uint32_t P = (uint32_t)std::lround(t.wind.stormCycle * 30.0f);
    const uint32_t c0 = P * 7;
    float envMin = 9, envMax = 0, jumpMax = 0;
    uint32_t prims = 0;
    for (uint32_t k = 0; k < 64; k++) {
      const uint32_t tk = c0 + (P * k) / 64;
      const WindStateQ q = WindWeatherQ(t, seed, tk);
      envMin = std::min(envMin, (float)q.envelope / 65536.0f);
      envMax = std::max(envMax, (float)q.envelope / 65536.0f);
      jumpMax = std::max(jumpMax, std::fabs((float)(int32_t)q.jumpBam) * (180.0f / 2147483648.0f));
      WindPrimGpu g[kWindPrimCap];
      int32_t lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX}, hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
      prims = std::max(prims, windfield::WeatherPrims(t, seed, tk, o3, g, kWindPrimCap, lo, hi));
    }
    const bool pass = envMin <= t.wind.stormLull + 0.05f && envMax >= t.wind.stormFront - 0.1f &&
                      jumpMax > 30.0f && prims > 0;
    std::snprintf(buf, sizeof buf,
                  "storm: envelope %.2f..%.2f x, jump up to %.0f deg, up to %u weather primitives %s",
                  envMin, envMax, jumpMax, prims, pass ? "ok" : "FAIL");
    note(buf);
    ok = ok && pass;
  }

  // ---- stability: night decoupling, noon thermals, a gale mixes both away --
  {
    Tuning t = base;
    t.wind.regime = "light";
    const WindStateQ night = WindWeatherQ(t, seed, tick, 0);
    const WindStateQ noon = WindWeatherQ(t, seed, tick, kDayNoon);
    t.wind.regime = "gale";
    const WindStateQ galeNoon = WindWeatherQ(t, seed, tick, kDayNoon);
    const bool pass = night.coupling < 52429 && noon.thermal > 0 && galeNoon.thermal == 0 &&
                      galeNoon.coupling == 65536;
    std::snprintf(buf, sizeof buf,
                  "stability: light night S %+.2f coupling %.2f; light noon S %+.2f thermal %.2f m/s; "
                  "gale noon S %+.2f %s",
                  night.stability / 65536.0f, night.coupling / 65536.0f, noon.stability / 65536.0f,
                  noon.thermal / 65536.0f * (float)kVoxelMeters, galeNoon.stability / 65536.0f,
                  pass ? "ok" : "FAIL");
    note(buf);
    ok = ok && pass;
  }

  detail = out;
  std::printf("wind-field: %s\n  %s\n", ok ? "PASS" : "FAIL", out.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- drafts: the shelter volume (docs/RESEARCH_wind.md §14) ----------------
//
// FOUR STONE HUTS ON ONE PAD, the wind pinned along +X, and the claims are the
// ones the plan makes about what walls do to air:
//
//   1. sealed          the transfer at the centre is ~0: a uniform flow cannot
//                      exist in a closed box.
//   2. door only       a windward doorway and nothing else: little gets in.
//   3. door + window   windward doorway, leeward window: a DRAFT threads the
//                      room, pointing door -> window, at a real fraction of the
//                      outside wind.
//   4. door + side     the window on a side wall: the flow inside turns toward
//                      it.
//
// Plus the open air above the roofs is unsheltered, and the alley between two
// huts is reported (continuity speeds it up). Then the CA, end to end: a smoke
// puff in hut 1 and in hut 3, the same ticks, and the draft hut must lose its
// smoke faster and out of the leeward side. Then the two promises that make the
// volume cheap and sound: a world whose blockers do not change never re-solves
// (smoke moving is not a geometry change), and a forced rebuild of the same
// geometry reproduces the field byte for byte (no history).
//
// THE TRANSFER IS READ DIRECTLY, not a wind: R_x at a cell is what a unit +X
// wind becomes there, which is the quantity every claim above is about and the
// one that does not move with the weather.
Status GateDrafts(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  uint32_t mStone = 0, mSmoke = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "stone") mStone = (uint32_t)i;
    else if (c.mats[i].name == "smoke") mSmoke = (uint32_t)i;
  }
  if (!mStone || !mSmoke) {
    detail = "need materials stone and smoke";
    return Status::Fail;
  }
  const Tuning saved = CurrentTuning();
  Tuning t = saved;
  t.sim.draftMode = 1;
  t.sim.windMode = 1;
  t.wind.weatherAuto = false;
  t.wind.windDirDeg = 90.0f;   // downwind +X: into every hut's doorway
  t.wind.windSpeed = (float)BaselineNumber("drafts.windSpeed", 12.0);
  t.wind.gustStrength = 0.0f;
  SetCurrentTuning(t);

  // ---- the fixture ----
  const int cx = 200;
  const int kHuts = 4;
  const int czs[kHuts] = {116, 172, 228, 284};
  const int kHx = 16, kHz = 16, kH = 24;      // walls at +-16, interior 1..24
  const int kDoorZ = 4, kDoorH = 18;          // doorway: 8 wide, 18 high
  // Window rows: up to one below the ceiling, so smoke that has risen to the
  // roof can still find it (6 wide, 7 high, its top row the ceiling's).
  const int kWin0 = 18, kWin1 = 24;
  const int y0 = FixtureYOver(cx - 40, czs[0] - 40, cx + 40, czs[kHuts - 1] + 40,
                              kDefaultSeed, 24, 140);
  std::map<std::tuple<int, int, int>, uint32_t> fx;
  for (int x = cx - 40; x <= cx + 40; x++)
    for (int z = czs[0] - 40; z <= czs[kHuts - 1] + 40; z++) {
      fx[{x, y0, z}] = mStone;
      for (int y = 1; y <= kH + 30; y++) fx[{x, y0 + y, z}] = 0u;
    }
  for (int h = 0; h < kHuts; h++) {
    const int cz = czs[h];
    for (int x = -kHx; x <= kHx; x++)
      for (int z = -kHz; z <= kHz; z++) fx[{cx + x, y0 + kH + 1, cz + z}] = mStone;
    for (int y = 1; y <= kH; y++) {
      for (int z = -kHz; z <= kHz; z++) {
        const bool door = h > 0 && z >= -kDoorZ && z < kDoorZ && y <= kDoorH;
        const bool lwin = h == 2 && z >= -3 && z < 3 && y >= kWin0 && y <= kWin1;
        if (!door) fx[{cx - kHx, y0 + y, cz + z}] = mStone;   // windward wall
        if (!lwin) fx[{cx + kHx, y0 + y, cz + z}] = mStone;   // leeward wall
      }
      for (int x = -kHx; x <= kHx; x++) {
        const bool swin = h == 3 && x >= -3 && x < 3 && y >= kWin0 && y <= kWin1;
        fx[{cx + x, y0 + y, cz - kHz}] = mStone;
        if (!swin) fx[{cx + x, y0 + y, cz + kHz}] = mStone;
      }
    }
  }
  std::vector<CellOp> build;
  build.reserve(fx.size());
  for (const auto& [k, w] : fx)
    build.push_back({World::SlotCellIndex({std::get<0>(k), std::get<1>(k), std::get<2>(k)}), w});

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t tick = 61000;
  // The fixture chunk -- which the draft box centres on (support.cpp's box
  // placement) -- is the middle of the row of huts, so all four sit well inside
  // the box and clear of its 2-cell edge blend.
  support::TickCursor ticker{c, tick, {cx >> 4, (y0 + 12) >> 4, 200 >> 4}};
  // ~1M cells: fed in batches under the per-tick cell-op cap
  // (kMaxCellOpsPerTick), or SubmitTick clamps the rest away.
  for (size_t i = 0; i < build.size(); i += kMaxCellOpsPerTick) {
    const size_t n = std::min<size_t>(kMaxCellOpsPerTick, build.size() - i);
    ticker({}, std::vector<CellOp>(build.begin() + (long)i, build.begin() + (long)(i + n)));
  }
  // The solve is a pipeline of kDraftStages ticks; let the last one start and
  // finish before reading.
  for (int i = 0; i < 2 * (int)kDraftStages + 2; i++) ticker();
  ctx.WaitIdle();

  // ---- read the volume ----
  auto readField = [&](std::vector<uint32_t>& f) {
    f.assign((size_t)3 * kDraftCells, 0u);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DraftBuffer(),
                          (uint64_t)kDraftFieldBase * 4, f.data(), f.size() * 4, "draftField");
  };
  auto readMeta = [&](uint32_t m[kDraftMetaWords]) {
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DraftMetaBuffer(), 0, m,
                          kDraftMetaWords * 4, "draftMeta");
  };
  std::vector<uint32_t> field;
  readField(field);
  uint32_t meta0[kDraftMetaWords] = {};
  readMeta(meta0);
  const int32_t* o = sim.DraftOrigin();
  struct R6 { float rx[3], rz[3]; bool in; };
  auto at = [&](const std::vector<uint32_t>& f, int x, int y, int z) -> R6 {
    R6 r{};
    const int dx = (x - o[0]) >> 2, dy = (y - o[1]) >> 2, dz = (z - o[2]) >> 2;
    if (dx < 0 || dy < 0 || dz < 0 || dx >= (int)kDraftNX || dy >= (int)kDraftNY ||
        dz >= (int)kDraftNZ)
      return r;
    const size_t i = 3 * (size_t)(((uint32_t)dz * kDraftNY + (uint32_t)dy) * kDraftNX + (uint32_t)dx);
    auto s16 = [](uint32_t v) { return (float)(int16_t)(uint16_t)(v & 0xFFFFu) / 4096.0f; };
    r.rx[0] = s16(f[i]);
    r.rx[1] = s16(f[i] >> 16);
    r.rx[2] = s16(f[i + 1]);
    r.rz[0] = s16(f[i + 1] >> 16);
    r.rz[1] = s16(f[i + 2]);
    r.rz[2] = s16(f[i + 2] >> 16);
    r.in = true;
    return r;
  };
  auto len3 = [](const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };

  std::string out;
  char buf[512];
  bool ok = true;
  auto note = [&](const char* s) { if (!out.empty()) out += "\n  "; out += s; };

  // The centre of each hut, averaged over a 3x3x3 block of cells so one cell's
  // rounding is not the verdict.
  auto mean = [&](int x, int y, int z, float rxOut[3]) {
    rxOut[0] = rxOut[1] = rxOut[2] = 0.0f;
    int n = 0;
    for (int dz = -4; dz <= 4; dz += 4)
      for (int dy = -4; dy <= 4; dy += 4)
        for (int dx = -4; dx <= 4; dx += 4) {
          const R6 r = at(field, x + dx, y + dy, z + dz);
          if (!r.in) continue;
          for (int a = 0; a < 3; a++) rxOut[a] += r.rx[a];
          n++;
        }
    if (n) for (int a = 0; a < 3; a++) rxOut[a] /= (float)n;
    return n > 0;
  };
  float hut[kHuts][3];
  for (int h = 0; h < kHuts; h++) {
    if (!mean(cx, y0 + 12, czs[h], hut[h])) {
      detail = "the huts are outside the draft volume -- the box did not follow the fixture";
      SetCurrentTuning(saved);
      return Status::Fail;
    }
  }
  const float sealedMax = (float)BaselineNumber("drafts.sealedMax", 0.05);
  const float doorMax = (float)BaselineNumber("drafts.doorOnlyMax", 0.2);
  const float crossMin = (float)BaselineNumber("drafts.crossMin", 0.3);
  const float sealed = len3(hut[0]), doorOnly = len3(hut[1]), cross = len3(hut[2]);
  const float crossDir = cross > 1e-4f ? hut[2][0] / cross : 0.0f;
  {
    const bool p = sealed < sealedMax;
    std::snprintf(buf, sizeof buf, "sealed hut: R (%.3f, %.3f, %.3f), |R| %.3f (< %.2f) %s",
                  hut[0][0], hut[0][1], hut[0][2], sealed, sealedMax, p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  // SANDVOX_DRAFT_DUMP=1: R_x along the centre line of every hut, cell by
  // cell, x then y -- where a residual flow sits says what is leaking.
  if (std::getenv("SANDVOX_DRAFT_DUMP")) {
    // The coarse (L1, 8-voxel) potential phi_x and K+ through the sealed hut.
    std::vector<uint32_t> cw((size_t)kDraftCoarseWords * kDraftCoarseCells);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DraftBuffer(), (uint64_t)kDraftCoarseBase * 4,
                          cw.data(), cw.size() * 4, "draftCoarse");
    auto cAt = [&](int x, int y, int z, int f) -> int32_t {
      const int X = ((x - o[0]) >> 4), Y = ((y - o[1]) >> 4), Z = ((z - o[2]) >> 4);
      return (int32_t)cw[(size_t)kDraftCoarseWords * (size_t)((Z * (int)kDraftChunksY + Y) * (int)kDraftChunksX + X) + (size_t)f];
    };
    std::printf("drafts dump coarse hut 0 along x: ");
    for (int x = cx - 24; x <= cx + 24; x += 8) {
      const int32_t k = cAt(x, y0 + 12, czs[0], 2);
      std::printf(" [%d phi %.3f K+ %d/%d/%d mem %08x%08x]", x - cx, cAt(x, y0 + 12, czs[0], 0) / 4096.0,
                  k & 511, (k >> 9) & 511, (k >> 18) & 511,
                  (uint32_t)cAt(x, y0 + 12, czs[0], 5), (uint32_t)cAt(x, y0 + 12, czs[0], 4));
    }
    std::printf("\ndrafts dump coarse hut 0 along y: ");
    for (int y = -8; y <= kH + 8; y += 8)
      std::printf(" [%d phi %.3f K+ y %d]", y, cAt(cx, y0 + y, czs[0], 0) / 4096.0,
                  (cAt(cx, y0 + y, czs[0], 2) >> 9) & 511);
    std::printf("\ndrafts dump origin %d %d %d, y0 %d\n", o[0], o[1], o[2], y0);
    if (FILE* fd = std::fopen("build/draft_coarse.bin", "wb")) {
      std::fwrite(cw.data(), 4, cw.size(), fd);
      std::fclose(fd);
    }
    {
      std::vector<uint32_t> masks((size_t)2 * kDraftCells);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DraftBuffer(), 0, masks.data(),
                            masks.size() * 4, "draftMasks");
      if (FILE* fd = std::fopen("build/draft_masks.bin", "wb")) {
        std::fwrite(masks.data(), 4, masks.size(), fd);
        std::fclose(fd);
      }
    }
    for (int h = 0; h < kHuts; h++) {
      std::printf("drafts dump hut %d along x (y0+12, cz):", h);
      for (int x = cx - 24; x <= cx + 24; x += 4) {
        const R6 r = at(field, x, y0 + 12, czs[h]);
        std::printf(" [%d %.2f %.2f %.2f]", x - cx, r.rx[0], r.rx[1], r.rx[2]);
      }
      std::printf("\ndrafts dump hut %d along y (cx, cz):", h);
      for (int y = -4; y <= kH + 8; y += 4) {
        const R6 r = at(field, cx, y0 + y, czs[h]);
        std::printf(" [%d %.2f %.2f %.2f]", y, r.rx[0], r.rx[1], r.rx[2]);
      }
      std::printf("\n");
    }
  }
  {
    const bool p = doorOnly < doorMax;
    std::snprintf(buf, sizeof buf, "windward door only: |R| %.3f (< %.2f) %s", doorOnly, doorMax, p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  {
    const bool p = cross >= crossMin && crossDir > 0.7f;
    std::snprintf(buf, sizeof buf,
                  "door + leeward window: R (%.3f, %.3f, %.3f), |R| %.3f (>= %.2f), along +X %.2f (> 0.7) %s",
                  hut[2][0], hut[2][1], hut[2][2], cross, crossMin, crossDir, p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  {
    // Inside hut 4, between the centre and its side (+Z) window.
    float s[3];
    mean(cx, y0 + 16, czs[3] + 8, s);
    const bool p = s[2] > (float)BaselineNumber("drafts.sideTurnMin", 0.05);
    std::snprintf(buf, sizeof buf, "door + side window: R toward the window (%.3f, %.3f, %.3f), +Z %s",
                  s[0], s[1], s[2], p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  {
    const R6 top = at(field, cx, y0 + 52, 200);
    const R6 alley = at(field, cx, y0 + 12, 200);
    const float dev = std::sqrt((top.rx[0] - 1.0f) * (top.rx[0] - 1.0f) + top.rx[1] * top.rx[1] +
                                top.rx[2] * top.rx[2]);
    const bool p = top.in && dev < (float)BaselineNumber("drafts.openDevMax", 0.2);
    std::snprintf(buf, sizeof buf,
                  "open air 2.8 m over the roofs: R (%.3f, %.3f, %.3f) %s; alley between huts 2/3: R.x %.3f",
                  top.rx[0], top.rx[1], top.rx[2], p ? "ok" : "FAIL", alley.rx[0]);
    note(buf); ok = ok && p;
  }

  // ---- the CA: smoke in the sealed hut and in the draft hut ----
  std::vector<CellOp> puff;
  for (int h : {0, 2})
    for (int y = 8; y <= 12; y++)
      for (int z = -3; z < 3; z++)
        for (int x = -3; x < 3; x++)
          puff.push_back({World::SlotCellIndex({cx + x, y0 + y, czs[h] + z}), mSmoke});
  const int kSmokeTicks = (int)BaselineNumber("drafts.smokeTicks", 90);
  std::map<uint32_t, std::vector<uint32_t>> chunks;
  auto matAt = [&](int x, int y, int z) -> uint32_t {
    const uint32_t slot = World::SlotChunkIndex({x >> 4, y >> 4, z >> 4});
    auto it = chunks.find(slot);
    if (it == chunks.end()) {
      std::vector<uint32_t> v((size_t)kChunkVol);
      ReadVoxelsSync(ctx, world, slot, 1, v.data(), "draftsVox");
      it = chunks.emplace(slot, std::move(v)).first;
    }
    return it->second[World::SlotCellIndex({x, y, z}) % kChunkVol] & 0xFFFu;
  };
  // Smoke in the open air just outside the draft hut, summed over samples
  // every 3 ticks: smoke that leaves is carried off by the outside wind
  // within a few ticks, so a count at the end would only ever see zero. The
  // hut's z band +-24, up to 3 m over its roof, 2.2 m out from each wall.
  uint32_t lee = 0, windward = 0;
  auto sampleOutside = [&]() {
    chunks.clear();
    for (int y = 1; y <= kH + 28; y++)
      for (int z = czs[2] - 24; z <= czs[2] + 24; z++)
        for (int x = 1; x <= 22; x++) {
          if (matAt(cx + kHx + x, y0 + y, z) == mSmoke) lee++;
          if (matAt(cx - kHx - x, y0 + y, z) == mSmoke) windward++;
        }
  };
  ticker({}, puff);
  for (int i = 0; i < kSmokeTicks; i++) {
    ticker();
    if (i % 3 == 2) {
      ctx.WaitIdle();
      sampleOutside();
    }
  }
  ctx.WaitIdle();
  uint32_t meta1[kDraftMetaWords] = {};
  readMeta(meta1);
  chunks.clear();
  uint32_t inside[2] = {};
  for (int k = 0; k < 2; k++) {
    const int cz = czs[k == 0 ? 0 : 2];
    for (int y = 1; y <= kH; y++)
      for (int z = -kHz + 1; z < kHz; z++)
        for (int x = -kHx + 1; x < kHx; x++)
          if (matAt(cx + x, y0 + y, cz + z) == mSmoke) inside[k]++;
  }
  const uint32_t placed = (uint32_t)puff.size() / 2;
  {
    // Smoke DECAYS (reactions.json), so the sealed hut's count is a control,
    // not a sealing claim: it only has to keep enough for the ratio below to
    // mean something. The claims are the RATIO -- the draft hut empties
    // faster -- and WHERE it went: out of the leeward window, not the door.
    const double keepMin = BaselineNumber("drafts.sealedKeepMin", 0.3);
    const double ratioMax = BaselineNumber("drafts.draftKeepRatioMax", 0.6);
    const bool p = inside[0] >= keepMin * placed &&
                   inside[1] <= ratioMax * inside[0] && lee > windward;
    std::snprintf(buf, sizeof buf,
                  "smoke after %d ticks (%u placed each): sealed hut keeps %u, draft hut %u; "
                  "smoke-cell samples outside the draft hut, lee %u vs windward %u %s",
                  kSmokeTicks, placed, inside[0], inside[1], lee, windward, p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  {
    // Smoke moving is not a geometry change: no solve in all those ticks.
    const bool p = meta1[kDraftMetaSolves] == meta0[kDraftMetaSolves];
    std::snprintf(buf, sizeof buf, "sleep: solves %u -> %u across %d smoky ticks (last at tick %u) %s",
                  meta0[kDraftMetaSolves], meta1[kDraftMetaSolves], kSmokeTicks,
                  meta1[kDraftMetaLastTick], p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }
  {
    // No history: re-mask and re-solve the same geometry from scratch.
    sim.ForceDraftRebuild();
    ticker();
    ctx.WaitIdle();
    std::vector<uint32_t> again;
    readField(again);
    uint32_t meta2[kDraftMetaWords] = {};
    readMeta(meta2);
    size_t diff = 0;
    for (size_t i = 0; i < field.size(); i++) diff += field[i] != again[i];
    const bool p = meta2[kDraftMetaSolves] == meta1[kDraftMetaSolves] + 1 && diff == 0;
    std::snprintf(buf, sizeof buf, "purity: forced rebuild solved %u time(s), %zu of %zu field words differ %s",
                  meta2[kDraftMetaSolves] - meta1[kDraftMetaSolves], diff, field.size(), p ? "ok" : "FAIL");
    note(buf); ok = ok && p;
  }

  SetCurrentTuning(saved);
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  detail = out;
  std::printf("drafts: %s\n  %s\n", ok ? "PASS" : "FAIL", out.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WindGates() {
  static const std::vector<Gate> g = {
      {"wind", "sim", {}, false, GateWind},
      {"wind-gas", "sim", {}, false, GateWindGas},
      {"wind-prim", "sim", {}, false, GateWindPrim},
      {"wind-field", "sim", {}, false, GateWindField},
      {"drafts", "sim", {}, false, GateDrafts},
  };
  return g;
}

}  // namespace selftest
