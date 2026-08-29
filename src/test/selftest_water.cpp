// selftest_water.cpp — `--gate waterbody`, the acceptance gate for
// docs/PLAN_water_master.md M1 and M2.
//
// THE PASSES, and what each one is the only thing that can catch:
//
//   A — CONSERVATION, and it is the primary pass. Across a real drain,
//
//           voxelEighths(t) + drained(t) - debit(t)  ==  voxelEighths(0)
//
//       as INTEGER equality. Every mistake in the drain ledger and the surface
//       shave breaks it, and when it breaks it names the body, the term and the
//       delta rather than reporting "mass changed by 37" (CLAUDE.md rule 6).
//       Two arms: A1 with the MPM excite seam off, which is the exact statement
//       about the ledger alone; A2 at the SHIPPED configuration, which is where
//       plan §9's ranked-first risk gets measured (below).
//   X — THE EXCITE CANDIDATE COUNT, which is not an assertion so much as the
//       one number this milestone was told to produce. WP5 measured 169,616
//       excite candidates over 400 ticks on `worldlake` from a DRAINING CA
//       leaving transient gaps under cells — enough to convert the whole
//       262,144-particle pool. The shave removes from the TOP so it should
//       create no air-below, but the CA re-levelling behind it might. The gate
//       runs a QUIET window and a DRAINING window of equal length and reports
//       both, because "the drain produced N candidates" is only meaningful
//       against what the same world produces doing nothing.
//   D — the off switch is bit-identical. Two runs of the same 40-tick script,
//       one at sim.waterBodyMode 0 and one at 1, MUST hash the same. This is
//       what let M1 and M2 land without a rebaseline, and it is kept forever.
//   G — the descriptor recomputes. The analytic container curve measured
//       against the real voxels, reported as a number rather than merely
//       bounded. From M2 that curve is only a SEED and a SCHEDULE, so this is
//       no longer load-bearing for mass — which is itself the thing pass A
//       proves.
//   E — a labelled body still sleeps, and a labelled body materializes no
//       pages. If merely NAMING a lake costs anything at rest, the feature is a
//       regression regardless of what it enables.
//   C — hysteresis does not flap. A body parked exactly on the size threshold
//       for 200 ticks may change state at most once.
//   K — the curve inverts. level(volume(y)) == y for every y in the table, on
//       the real parabola. Pure arithmetic, no GPU.
//
// STILL NOT HERE: pass B (split scheduling) needs component 10's union-find
// sweep, and pass F (determinism mid-drain) needs a second in-process world to
// compare against — `--gate determinism` covers the shipped configuration, and
// at sim.waterBodyMode 0 that is the whole world. Both land with the milestone
// that gives them a subject rather than being asserted against zero now.
//
// EVERY PASS RUNS UNDER `--gate waterbody` ALONE. No second invocation, no
// manual read of terminal output, no separate smoke pass — CLAUDE.md's
// "authoring cheap-to-verify work". Every threshold lives in
// tests/baseline.json and every measurement goes through RecordObserved, so
// retuning one costs no rebuild.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sim/waterbody.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// What a sweep of the real voxels says about a basin. The RECOMPUTE half of
// pass G: derived from the voxels alone, with no reference to the descriptor it
// is about to be compared against.
struct VoxelTruth {
  bool read = false;
  uint64_t eighths = 0;      // total liquid eighths of the basin's material
  uint32_t cells = 0;        // cells holding any of it
  uint32_t surfaceCells = 0; // free-surface cells (nothing of ours above)
  int surfaceMinY = 0;       // spread = max - min over the free surface
  int surfaceMaxY = 0;
  uint32_t chunks = 0;       // chunks actually read
};

// Sweep the basin's water AABB, chunk by chunk, through the CPU seam.
//
// ReadVoxelsSync and not a raw subscript: the voxel buffer is a PAGE POOL, and
// a slot's words are wherever its page is (or nowhere at all, for a sentinel —
// which ReadVoxelsSync synthesizes). This is the fifth site named in
// PLAN_page_table.md §2.1a and the gate has no business being the sixth.
VoxelTruth SweepBasin(Ctx& c, const WaterBasin& b, const WaterBodyDesc& d,
                      uint32_t matId) {
  VoxelTruth t;
  World& world = c.world;
  std::vector<uint32_t> chunk(kChunkVol);
  t.surfaceMinY = 1 << 30;
  t.surfaceMaxY = -(1 << 30);

  // One cell of headroom above the fill level, because "is this cell the free
  // surface" is a question about the cell ABOVE it. Without the extra layer the
  // topmost water would be classified by reading a chunk that was never fetched
  // and every surface cell would be misjudged at once.
  const int y0 = d.lo.y, y1 = d.hi.y;
  std::vector<uint8_t> full;   // per (x,z,y) of the AABB: eighths, 0 = not ours
  const int nx = d.hi.x - d.lo.x + 1;
  const int nz = d.hi.z - d.lo.z + 1;
  const int ny = y1 - y0 + 2;
  full.assign((size_t)nx * nz * ny, 0u);
  auto at = [&](int x, int y, int z) -> uint8_t& {
    return full[(size_t)((y - y0) * nz + (z - d.lo.z)) * nx + (x - d.lo.x)];
  };

  for (int cy = y0 >> 4; cy <= ((y1 + 1) >> 4); cy++) {
    for (int cz = d.lo.z >> 4; cz <= (d.hi.z >> 4); cz++) {
      for (int cx = d.lo.x >> 4; cx <= (d.hi.x >> 4); cx++) {
        const IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) return t;   // t.read stays false
        ReadVoxelsSync(c.ctx, world, World::SlotChunkIndex(wc), 1, chunk.data(),
                       "waterbodySweep");
        t.chunks++;
        for (int ly = 0; ly < 16; ly++) {
          const int y = cy * 16 + ly;
          if (y < y0 || y > y1 + 1) continue;
          for (int lz = 0; lz < 16; lz++) {
            const int z = cz * 16 + lz;
            if (z < d.lo.z || z > d.hi.z) continue;
            for (int lx = 0; lx < 16; lx++) {
              const int x = cx * 16 + lx;
              if (x < d.lo.x || x > d.hi.x) continue;
              const int64_t dx = x - b.cx, dz = z - b.cz;
              if (dx * dx + dz * dz > b.discD2Max) continue;
              const uint32_t w = chunk[(size_t)(lz * 16 + ly) * 16 + lx];
              if ((w & 0xFFFu) != matId) continue;
              at(x, y, z) = (uint8_t)(((w >> 12) & 0xFu) + 1u);
            }
          }
        }
      }
    }
  }

  for (int y = y0; y <= y1; y++) {
    for (int z = d.lo.z; z <= d.hi.z; z++) {
      for (int x = d.lo.x; x <= d.hi.x; x++) {
        const uint8_t e = at(x, y, z);
        if (e == 0) continue;
        t.eighths += e;
        t.cells++;
        // FREE SURFACE: nothing of ours directly above. That is the exact
        // predicate component 4's shave will key off, so measuring it here is
        // measuring the thing, not a proxy for it.
        if (at(x, y + 1, z) == 0) {
          t.surfaceCells++;
          t.surfaceMinY = std::min(t.surfaceMinY, y);
          t.surfaceMaxY = std::max(t.surfaceMaxY, y);
        }
      }
    }
  }
  if (t.surfaceCells == 0) { t.surfaceMinY = 0; t.surfaceMaxY = 0; }
  t.read = true;
  return t;
}

// ---- the GPU ledger, as the gate sees it -----------------------------------
//
// Must match the WBS_* / WB_* block in assets/shaders/sim_waterbody.wgsl. This
// is the one place in C++ that decodes it, and it exists because M2 moved the
// level, the debit and the adoption verdict ONTO THE GPU on purpose: the ledger
// debits by what the shave atomically reported, and reading that back to decide
// anything would put fence retirement inside a voxel write's control path.
// A gate may read it. Nothing on the frame path may.
enum : uint32_t {
  WBS_STATE = 0, WBS_LEVEL, WBS_AREA, WBS_DEBIT, WBS_SHAVED, WBS_SEEN,
  WBS_ATLEVEL, WBS_STEPS, WBS_FRAC, WBS_DRAINED, WBS_VOLUME, WBS_QUIET,
  WBS_RSUM, WBS_RDIRTY, WBS_CAPPED, WBS_ADOPTTICK,
};
enum : int32_t {
  WB_CANDIDATE = 0, WB_MEASURING = 1, WB_ADOPTED = 2, WB_RELEASING = 3,
};

struct LedgerView {
  std::vector<int32_t> w;
  int32_t At(uint32_t slot, uint32_t f) const {
    const size_t i = (size_t)slot * kWaterBodyStateWords + f;
    return i < w.size() ? w[i] : 0;
  }
};

LedgerView ReadLedger(Ctx& c) {
  LedgerView v;
  v.w.assign((size_t)kWaterBodyCap * kWaterBodyStateWords, 0);
  ReadWaterLedgerSync(c.ctx, c.world, v.w.data());
  return v;
}

const char* LedgerStateName(int32_t st) {
  switch (st) {
    case WB_CANDIDATE: return "candidate";
    case WB_MEASURING: return "measuring";
    case WB_ADOPTED: return "adopted";
    case WB_RELEASING: return "releasing";
    default: return "?";
  }
}

// Advance the sim `n` ticks with no ops at all. The harness snapshot drain is
// what makes World::Snap() arrive under a headless run, and the jurisdiction
// ladder's quiescence term reads it — without the drain nothing is ever quiet
// and pass C would be testing a body that can never be adopted.
// `excite` (optional) accumulates the WP5 reach probe across the window. It is
// read from World::Snap(), NEVER from a blocking readback: a blocking read
// inside a tick loop dilates the page-table snapshot cadence, and that has
// already once produced 217 page faults with nothing to do with the code under
// test. The snapshot is one tick latent and can skip, so this is a LOWER BOUND
// on the candidate count and is reported as one.
uint32_t RunQuietTicks(Ctx& c, uint32_t first, uint32_t n,
                       uint64_t* exciteSeen = nullptr,
                       uint64_t* exciteCandid = nullptr) {
  uint32_t lastSnap = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) {
    // NO FlipPage HERE. SubmitTick already flips (support.cpp), and a second
    // flip makes the read and write dirty pages the SAME buffer every tick, so
    // the CA re-reads what it just wrote and the world never settles. Measured
    // as 1,872 chunks permanently awake in a world the `terrain` gate proves
    // reaches zero at 120 ticks — a false accusation aimed at whatever feature
    // the loop happened to be testing.
    SubmitTick(c.ctx, c.world, c.sim, first + i, kDefaultSeed, {}, {}, {},
               false, c.world.WindowOrigin(), true, false);
    c.ctx.ProcessEvents();
    const WorldSnapshot& sn = c.world.Snap();
    if ((exciteSeen || exciteCandid) && sn.valid && sn.tick != lastSnap) {
      lastSnap = sn.tick;
      if (exciteSeen) *exciteSeen += sn.fluidExciteSeen;
      if (exciteCandid) *exciteCandid += sn.fluidExciteCandidates;
    }
  }
  return first + n;
}

Status GateWaterBody(Ctx& c, std::string& detail) {
  World& world = c.world;
  Tuning base = CurrentTuning();
  const bool hadDrain = HarnessSnapshotDrain();
  SetHarnessSnapshotDrain(true);

  bool ok = true;
  std::string notes;
  auto fail = [&](const std::string& why) { ok = false; notes += " -- " + why; };

  // ------------------------------------------------------------------ pass K
  // The container curve inverts. Pure arithmetic on the SHIPPED parabola, so it
  // runs before any GPU work and cannot be poisoned by a scene that failed to
  // build. `level(volume(y)) == y` for every y the table covers is the property
  // that catches an off-by-one in crossSectionD2's algebraic inversion of
  // pondAt, and an off-by-one there is a mass error the moment M2's ledger
  // starts spending the table.
  uint32_t curveLevels = 0;
  {
    WaterBasin b;
    b.cx = 0; b.cz = 0;
    b.radius = 48;
    b.discD2Max = 48 * 48;
    b.surfY = 1000;
    b.centreDepth = base.worldgen.pondDepth;
    b.rimDepth = base.worldgen.pondDepthRim;
    b.floorY = b.surfY - b.centreDepth;
    b.spillY = b.surfY + base.worldgen.pondBerm;
    b.kind = WaterBasinKind::ParabolicBowl;
    const WaterBasinCurve cur = WaterBasinBuildCurve(b);
    curveLevels = (uint32_t)cur.area.size();
    if (curveLevels == 0) fail("the container curve is empty");
    uint64_t prev = 0;
    for (uint32_t i = 0; i < curveLevels; i++) {
      const int y = cur.floorY + 1 + (int)i;
      // A bowl NARROWS as it empties, so area must never decrease going up.
      // A curve that dipped would drain in reverse somewhere.
      if (i > 0 && cur.area[i] < cur.area[i - 1]) {
        fail(Format("the curve is not monotone at y=%d (%u after %u)", y,
                    cur.area[i], cur.area[i - 1]));
        break;
      }
      if (cur.prefix[i] <= prev && cur.area[i] > 0) {
        fail("the prefix sum did not advance over a non-empty level");
        break;
      }
      prev = cur.prefix[i];
      uint32_t rem = 0;
      const uint64_t v = WaterBasinVolumeEighths(cur, y);
      const int back = WaterBasinLevelFor(cur, v, &rem);
      if (back != y || rem != 0) {
        fail(Format("curve round-trip failed at y=%d: volume %llu -> level %d "
                    "rem %u", y, (unsigned long long)v, back, rem));
        break;
      }
    }
  }

  // ----------------------------------------------------------------- pass K2
  // THE PARABOLA AGAINST THE WORLD, by a second independent path.
  //
  // Why this pass exists at all: `pondInfo`'s keep-out box is the 768-voxel
  // square around the origin, which is EXACTLY the residency window the harness
  // runs in (world.cpp says so in as many words). So no tarn is ever resident
  // here, pass G below can only ever sweep the authored lake, and the authored
  // lake is a flat-floored cylinder — the DEGENERATE curve, one number repeated.
  // The bowl arm, where crossSectionD2 algebraically inverts pondAt's integer
  // parabola, would ship with nothing but its own round-trip behind it, and an
  // off-by-one there is a mass error in every natural pond in the world.
  //
  // Moving the window to a real tarn is what `voxregion` does and why that gate
  // must run LAST (CLAUDE.md rule 7); this gate runs second and must not.
  //
  // So: check the curve against the COLUMN function instead. Sum (surf - floor)
  // over every column of a real tarn using World::TerrainHeight — the height
  // contract, which the `terrain` gate proves per-voxel against the GPU in its
  // pass C — and compare that cell count against the curve's own volume. Two
  // independent traversals of the same bowl, one per COLUMN and one per LEVEL,
  // which is precisely the axis an inversion bug lies along. It is transitively
  // grounded in real voxels by the gate that ran immediately before this one.
  uint32_t tarnR = 0;
  int64_t tarnCurveCells = 0, tarnColumnCells = 0;
  {
    // Find a real tarn by walking pond tiles outward from the origin tile until
    // one rolls present. Bounded and cheap (~4 hashes a tile); the keep-out puts
    // the nearest a few tiles away.
    const int tile = World::PondTileSize();
    World::PondDisc found;
    for (int ring = 1; ring <= 10 && !found.present; ring++) {
      for (int tz = -ring; tz <= ring && !found.present; tz++) {
        for (int tx = -ring; tx <= ring && !found.present; tx++) {
          if (std::max(std::abs(tx), std::abs(tz)) != ring) continue;
          const World::PondDisc d = World::PondTile(tx, tz, kDefaultSeed);
          if (d.present) found = d;
        }
      }
    }
    if (!found.present || tile <= 0) {
      // Not a failure of this code: a seed may simply roll no tarn nearby, and
      // saying so beats inventing a green light.
      notes += " -- note: no tarn within 10 pond tiles of the origin, so the "
               "bowl arm was checked by round-trip only";
    } else {
      WaterBasin b;
      b.cx = found.cx; b.cz = found.cz;
      b.radius = found.r;
      b.discD2Max = found.r * found.r;
      b.surfY = found.surf;
      b.centreDepth = base.worldgen.pondDepth;
      b.rimDepth = base.worldgen.pondDepthRim;
      b.floorY = found.surf - b.centreDepth;
      b.spillY = found.surf + base.worldgen.pondBerm;
      b.kind = WaterBasinKind::ParabolicBowl;
      tarnR = (uint32_t)found.r;
      const WaterBasinCurve cur = WaterBasinBuildCurve(b);
      tarnCurveCells = (int64_t)(WaterBasinVolumeEighths(cur, b.surfY) / 8u);
      for (int z = b.cz - b.radius; z <= b.cz + b.radius; z++) {
        for (int x = b.cx - b.radius; x <= b.cx + b.radius; x++) {
          const int64_t dx = x - b.cx, dz = z - b.cz;
          if (dx * dx + dz * dz > b.discD2Max) continue;
          // Inside a disc the bowl REPLACES the ground, so TerrainHeight IS the
          // bowl floor and worldgen fills (floor, surf] with water.
          const int floorY = World::TerrainHeight(x, z, kDefaultSeed);
          if (floorY < b.surfY) tarnColumnCells += b.surfY - floorY;
        }
      }
      const double err =
          tarnColumnCells == 0
              ? 100.0
              : 100.0 * (double)(tarnCurveCells - tarnColumnCells) /
                    (double)tarnColumnCells;
      RecordObserved("waterbodyBowlColumnErrPct", err);
      const double tol = BaselineNumber("waterbodyVolTolPct", 5.0);
      if (std::abs(err) > tol)
        fail(Format("the bowl curve is %+.2f%% off the column walk (tolerance "
                    "%.2f%%): %lld cells by level vs %lld by column, tarn r%d",
                    err, tol, (long long)tarnCurveCells,
                    (long long)tarnColumnCells, found.r));
    }
  }

  // ------------------------------------------------------------------ setup
  // Pristine worldgen, then let it settle. 130 ticks, and the number is not
  // arbitrary: the `terrain` gate's pass D measures this same fresh world
  // reaching ZERO awake chunks at 120. Measured here at 60 it was still 1,872,
  // which pass E below would report as this feature keeping the world awake —
  // a false accusation against a subsystem that writes nothing at all.
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  Tuning t = base;
  t.sim.waterBodyMode = 1;
  // The shipped quiet window is 30 ticks (one second) and the gate does not
  // argue with it — it shortens it, because what this gate is testing is the
  // ledger and not how long the CPU is willing to wait. 8 leaves the world's
  // own ~120-tick settle as the thing that gates adoption, which is the honest
  // dependency, and it keeps the gate's tick budget for the drain.
  t.sim.waterBodyQuietTicks = 8;
  SetCurrentTuning(t);
  uint32_t tick = RunQuietTicks(c, 1, 130);

  const WaterBodySystem& wb = WaterBodies();
  const WaterBasin* lake = wb.Basin(1);
  const WaterBodyDesc* desc = wb.Find(1);
  if (!lake || !desc) {
    SetCurrentTuning(base);
    SetHarnessSnapshotDrain(hadDrain);
    detail = "the authored lake (basin 1) is not in the registry";
    std::printf("waterbody: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // ------------------------------------------------------------------ pass G
  // The descriptor against the voxels. The analytic curve is a PREDICTION
  // (plan §3.2: a schedule, not an authority) and this is the measurement of
  // how good a prediction it is. Reported as a percentage rather than only
  // bounded, because that number is what M2's ledger inherits.
  //
  // TWO BASINS, and the second is the one that matters. The authored lake is a
  // flat-floored cylinder — the DEGENERATE case, where the curve is one number
  // repeated. The parabolic bowl is where crossSectionD2 actually inverts
  // pondAt's integer arithmetic, and an off-by-one there would be invisible in
  // the cylinder and a mass error in every natural pond in the world. So the
  // largest tarn in the window is swept too, when the window holds one.
  double volErrPct = 0.0, areaErrPct = 0.0;
  VoxelTruth truth;
  double bowlVolErrPct = 0.0, bowlAreaErrPct = 0.0;
  VoxelTruth bowlTruth;
  const WaterBasin* bowl = nullptr;
  const WaterBodyDesc* bowlDesc = nullptr;
  // Counted HERE, while the registry is alive: the gate resets the system on
  // its way out and a count read after that is a count of nothing.
  const uint32_t basinCount = (uint32_t)wb.Basins().size();
  uint32_t bowlCount = 0;
  for (const WaterBasin& b : wb.Basins())
    if (b.kind == WaterBasinKind::ParabolicBowl) bowlCount++;
  std::string bowlNote = "no tarn in the window";

  auto matIdOf = [&](const std::string& name, uint32_t& out) {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == name) { out = (uint32_t)i; return true; }
    return false;
  };
  // Compare one basin's descriptor against one sweep, name the failure, and
  // report the delta as a percentage whether it passed or not.
  auto compare = [&](const char* what, const WaterBasin& b,
                     const WaterBodyDesc& d, const VoxelTruth& v,
                     double& volErr, double& areaErr) {
    if (!v.read) {
      fail(Format("the %s is not fully resident in the window", what));
      return;
    }
    if (v.eighths == 0) {
      fail(Format("the %s holds no water at all", what));
      return;
    }
    volErr = 100.0 * ((double)d.volumeEighths - (double)v.eighths) /
             (double)v.eighths;
    areaErr = v.surfaceCells == 0
                  ? 100.0
                  : 100.0 * ((double)d.surfaceArea - (double)v.surfaceCells) /
                        (double)v.surfaceCells;
    // Thresholds in JSON, never in C++ (CLAUDE.md): the closed form is exact
    // arithmetic but the WORLD is not obliged to agree with it — pond life
    // replaces water cells with kelp and reeds, a ruin can intrude — so this is
    // a tolerance and tolerances get retuned. An absent key must still work, so
    // the fallback is generous and the measured value is always printed.
    const double volTol = BaselineNumber("waterbodyVolTolPct", 5.0);
    const double areaTol = BaselineNumber("waterbodyAreaTolPct", 5.0);
    if (std::abs(volErr) > volTol)
      fail(Format("%s: analytic volume is %+.2f%% off the voxels (tolerance "
                  "%.2f%%): %llu predicted vs %llu real eighths",
                  what, volErr, volTol, (unsigned long long)d.volumeEighths,
                  (unsigned long long)v.eighths));
    if (std::abs(areaErr) > areaTol)
      fail(Format("%s: analytic surface area is %+.2f%% off (tolerance %.2f%%):"
                  " %u predicted vs %u real cells",
                  what, areaErr, areaTol, d.surfaceArea, v.surfaceCells));
    // The spread test component 5 leans on, measured rather than assumed. At M1
    // only closed analytic basins are registered and a stream has no basin, so
    // the model's error term SHOULD be zero here; this is what turns that
    // structural argument into a number.
    const int spread = v.surfaceMaxY - v.surfaceMinY;
    if (spread > t.sim.waterBodySpreadExit)
      fail(Format("%s: a settled surface spans %d voxels, over the release "
                  "threshold of %d — the level model does not describe it",
                  what, spread, t.sim.waterBodySpreadExit));
    (void)b;
  };

  uint32_t matId = 0;
  if (!matIdOf(lake->matName, matId)) {
    fail(Format("no material named '%s'", lake->matName.c_str()));
  } else {
    truth = SweepBasin(c, *lake, *desc, matId);
    compare("authored lake", *lake, *desc, truth, volErrPct, areaErrPct);
    RecordObserved("waterbodyVolErrPct", volErrPct);
    RecordObserved("waterbodyAreaErrPct", areaErrPct);
    RecordObserved("waterbodySurfaceSpread",
                   (double)(truth.surfaceMaxY - truth.surfaceMinY));
  }

  // The largest ADOPTABLE bowl. Largest because the relative cost of a single
  // miscounted ring falls with radius, so a big tarn is the sharper instrument;
  // adoptable because a body the ladder refused has a descriptor nobody would
  // ever have spent.
  for (const WaterBasin& b : wb.Basins()) {
    if (b.kind != WaterBasinKind::ParabolicBowl) continue;
    const WaterBodyDesc* d = wb.Find(b.id);
    if (!d || d->chunks.empty()) continue;
    if (!bowlDesc || d->volumeEighths > bowlDesc->volumeEighths) {
      bowl = &b;
      bowlDesc = d;
    }
  }
  if (bowl && bowlDesc) {
    uint32_t bowlMat = 0;
    if (!matIdOf(bowl->matName, bowlMat)) {
      fail(Format("no material named '%s'", bowl->matName.c_str()));
    } else {
      bowlTruth = SweepBasin(c, *bowl, *bowlDesc, bowlMat);
      if (!bowlTruth.read) {
        // A tarn that straddles the window edge is not a failure of anything —
        // it is a basin this window does not contain, and component 5 refuses
        // it for exactly that reason. Say so rather than reporting a fake error.
        bowlNote = "the window's tarn is not fully resident (not swept)";
      } else {
        compare("tarn", *bowl, *bowlDesc, bowlTruth, bowlVolErrPct,
                bowlAreaErrPct);
        RecordObserved("waterbodyBowlVolErrPct", bowlVolErrPct);
        RecordObserved("waterbodyBowlAreaErrPct", bowlAreaErrPct);
        bowlNote = Format(
            "tarn r%d %llu/%llu eighths (%+.2f%%), surface %u/%u cells "
            "(%+.2f%%), spread %d vox",
            bowl->radius, (unsigned long long)bowlDesc->volumeEighths,
            (unsigned long long)bowlTruth.eighths, bowlVolErrPct,
            bowlDesc->surfaceArea, bowlTruth.surfaceCells, bowlAreaErrPct,
            bowlTruth.surfaceMaxY - bowlTruth.surfaceMinY);
      }
    }
  }

  // ------------------------------------------------------------------ pass E
  // A labelled body still sleeps. Rule 2 is not suspended for a feature that
  // has not started costing anything yet: if merely NAMING a lake keeps its
  // chunks awake, the naming is already a regression.
  const uint32_t awake = ReadActiveChunksSync(c.ctx, world, c.sim);
  RecordObserved("waterbodyAwakeChunks", (double)awake);
  const double awakeMax = BaselineNumber("waterbodyAwakeMax", 32.0);
  if ((double)awake > awakeMax)
    fail(Format("%u chunks awake with the feature on, over the budget of %.0f",
                awake, awakeMax));
  // The other half of "idle cost is zero", and it is a residency claim rather
  // than an activity one: a GOVERNED lake that is not draining declares no page
  // targets, so it materializes nothing. `writesThisTick` is what enforces it
  // and this is what checks the enforcement — a lake sitting still for 130
  // ticks must not have moved the fault counter, because it must not have
  // written a voxel.
  uint32_t pf[4] = {0, 0, 0, 0};
  ReadPageFaultsSync(c.ctx, world, pf);
  if (pf[0] != 0)
    fail(Format("%u page faults before any drain (lost word 0x%08x) — a "
                "labelled lake wrote a voxel it should not have",
                pf[0], pf[2]));

  // ------------------------------------------------------------------ pass C
  // Hysteresis. The body is parked EXACTLY on the enter threshold — the single
  // configuration that makes a naive classifier oscillate — and watched for 200
  // ticks. At most one transition. Two or more means the enter and exit tests
  // are reachable from each other in one tick, and plan §5 is blunt about what
  // that costs: every flip is a seam crossing where mass can be lost.
  uint32_t flips = 0;
  {
    Tuning h = t;
    h.sim.waterBodyMinVolume =
        (int)std::min<uint64_t>(desc->volumeEighths, 0x7FFFFFFFull);
    h.sim.waterBodyExitVolume = h.sim.waterBodyMinVolume / 2;
    h.sim.waterBodyQuietTicks = 4;
    SetCurrentTuning(h);
    WaterBodyState last = WaterBodies().Find(1)
                              ? WaterBodies().Find(1)->state
                              : WaterBodyState::Candidate;
    for (uint32_t i = 0; i < 200; i++) {
      tick = RunQuietTicks(c, tick, 1);
      const WaterBodyDesc* d = WaterBodies().Find(1);
      if (!d) { fail("basin 1 vanished from the registry mid-run"); break; }
      if (d->state != last) { flips++; last = d->state; }
    }
    if (flips > 1)
      fail(Format("the descriptor changed state %u times in 200 ticks parked "
                  "on the threshold", flips));
    SetCurrentTuning(t);
    tick = RunQuietTicks(c, tick, 12);   // let the ladder re-settle after the poke
  }

  // The ladder must actually REACH adopted somewhere in this gate, or every
  // assertion above is about a body the classifier refused and the pass is
  // vacuous. Reported with the refusal reason, never as a bare count.
  const WaterBodyDesc* fin = WaterBodies().Find(1);
  const char* why = "none";
  if (fin) {
    switch (fin->refusal) {
      case WaterBodyRefusal::TooSmall: why = "too small"; break;
      case WaterBodyRefusal::Overflowing: why = "over its spill"; break;
      case WaterBodyRefusal::Straddle: why = "straddling chunks"; break;
      case WaterBodyRefusal::Spread: why = "surface spread"; break;
      case WaterBodyRefusal::AtCap: why = "at the body cap"; break;
      case WaterBodyRefusal::OutOfWindow: why = "outside the window"; break;
      case WaterBodyRefusal::NoChunkBudget: why = "no chunk budget"; break;
      default: break;
    }
  }
  if (!fin || fin->state != WaterBodyState::Proposed)
    fail(Format("the CPU never proposed the authored lake (refused: %s)", why));
  const uint32_t proposedNow = WaterBodies().ProposedCount();

  // ADOPTION IS A GPU FACT NOW, so it is read back rather than inferred. This
  // is also the M1 hazard's closing statement: the verdict below was computed
  // from `dirtyIn` and the MPM block map on the tick, not from whenever a fence
  // retired, and everything after this point depends on it.
  const uint32_t lakeSlot = fin ? fin->gpuSlot : kNoGpuSlot;
  int32_t lakeState = -1, lakeLevel = 0, lakeArea = 0, lakeVolume = 0,
          lakeQuiet = 0;
  if (lakeSlot < kWaterBodyCap) {
    const LedgerView lv = ReadLedger(c);
    lakeState = lv.At(lakeSlot, WBS_STATE);
    lakeLevel = lv.At(lakeSlot, WBS_LEVEL);
    lakeArea = lv.At(lakeSlot, WBS_AREA);
    lakeVolume = lv.At(lakeSlot, WBS_VOLUME);
    lakeQuiet = lv.At(lakeSlot, WBS_QUIET);
  }
  if (lakeState != WB_ADOPTED)
    fail(Format("the GPU ladder never adopted the authored lake: slot %u is "
                "%s after %u quiet ticks (min volume %d eighths)",
                lakeSlot, LedgerStateName(lakeState), (unsigned)lakeQuiet,
                t.sim.waterBodyMinVolume));

  // THE ADOPTION REDUCE, against the sweep pass G already did. This is the
  // number component 1 deferred and M2 owes: the ledger's opening balance is
  // read from the voxels by a GPU pass, and if that pass disagrees with a CPU
  // walk of the same cells then every conservation statement below is about the
  // wrong lake. Exact equality, not a tolerance — both sides count the same
  // eighths of the same material in the same disc.
  double reduceErrPct = 0.0;
  if (truth.read && truth.eighths > 0) {
    reduceErrPct = 100.0 * ((double)lakeVolume - (double)truth.eighths) /
                   (double)truth.eighths;
    RecordObserved("waterbodyReduceErrPct", reduceErrPct);
    if ((uint64_t)std::max(lakeVolume, 0) != truth.eighths)
      fail(Format("the GPU adoption reduce read %d eighths where the CPU sweep "
                  "of the same cells found %llu (%+.4f%%)",
                  lakeVolume, (unsigned long long)truth.eighths,
                  reduceErrPct));
  }


  // ------------------------------------------------------------- pass A + X
  //
  // CONSERVATION ACROSS A REAL DRAIN, and the excite-candidate measurement that
  // rides in the same window. This is the primary pass of the milestone: it is
  // the only thing that can tell a working ledger from a mass pump, and every
  // discipline in plan §3 exists to make it hold.
  //
  //     voxelEighths(t) + drained(t) - debit(t)  ==  voxelEighths(0)
  //
  // `drained` is what left the body forever, `debit` is what has been taken
  // from the ledger but is still in the voxels because no shave has removed it
  // yet. That second term is plan §3.3's LEGITIMATE divergence and it is a
  // stored field, never implied: a gate that forgot it would report a leak of
  // up to one whole eighth-step that does not exist.
  //
  // THE DRAIN SOURCE IS THE TEST TAP. M2 has no discharge law, so the tap is
  // sized at one eighth per surface cell per tick — exactly one eighth-step,
  // which is the rate that exercises `steps` and leaves `frac` at zero on the
  // clean ticks and nonzero the moment the measured area diverges from the
  // analytic seed. That divergence is the point: it is what proves the area
  // table is a SCHEDULE and not an authority.
  int64_t consV0 = 0, consV1 = 0;
  int64_t consDrained = 0, consDebit = 0, consCapped = 0, consErr = 0;
  int32_t levelBefore = lakeLevel, levelAfter = lakeLevel;
  uint32_t drainTicks = 0, drainPf = 0;
  uint64_t quietSeen = 0, quietCandid = 0, drainSeen = 0, drainCandid = 0;
  int64_t exciteSlack = 0;
  bool ranDrain = false;
  if (ok && lakeSlot < kWaterBodyCap && truth.read && truth.eighths > 0) {
    drainTicks = (uint32_t)BaselineNumber("waterbodyDrainTicks", 60.0);

    // ---- X, control arm ---------------------------------------------------
    // The SAME world, the SAME length, doing nothing. Without it "the drain
    // produced N candidates" is a bare count, and CLAUDE.md rule 6 is explicit
    // about what a bare count costs. The excite seam is at its SHIPPED setting
    // for both arms — measuring the risk in a configuration nobody ships would
    // measure nothing.
    tick = RunQuietTicks(c, tick, drainTicks, &quietSeen, &quietCandid);

    // ---- A + X, draining arm ---------------------------------------------
    Tuning dr = t;
    dr.sim.waterBodyTestDrain =
        (int)std::min<uint64_t>(std::max<uint32_t>(lakeArea, 1u), 1u << 20);
    SetCurrentTuning(dr);
    // Measured HERE, after the control window and with the tap still shut, so
    // the opening balance is the world the drain actually starts from.
    const VoxelTruth before = SweepBasin(c, *lake, *desc, matId);
    consV0 = (int64_t)before.eighths;
    tick = RunQuietTicks(c, tick, drainTicks, &drainSeen, &drainCandid);
    SetCurrentTuning(t);
    // One settling tick with the tap shut, so the last shave's report has been
    // consumed by the ledger. Without it `debit` still carries eighths the
    // shave has already taken out of the voxels and the identity is off by one
    // tick's worth — which would look exactly like a leak.
    tick = RunQuietTicks(c, tick, 1);

    const VoxelTruth after = SweepBasin(c, *lake, *desc, matId);
    consV1 = (int64_t)after.eighths;
    const LedgerView lv = ReadLedger(c);
    consDrained = lv.At(lakeSlot, WBS_DRAINED);
    consDebit = lv.At(lakeSlot, WBS_DEBIT);
    consCapped = lv.At(lakeSlot, WBS_CAPPED);
    levelAfter = lv.At(lakeSlot, WBS_LEVEL);
    ranDrain = true;

    // THE IDENTITY. Integer, and reported term by term when it fails — plan §7:
    // "which body, which term, and by how much", never "mass changed by 37".
    consErr = consV1 + consDrained - consDebit - consV0;

    uint32_t pf2[4] = {0, 0, 0, 0};
    ReadPageFaultsSync(c.ctx, world, pf2);
    drainPf = pf2[0];

    // The excite seam converts settled voxels into particles, which the voxel
    // sweep cannot see. At the shipped sim.fluidExciteMode that is a REAL term
    // of the conservation sum and M2 has no exact hook for it (component 7 is
    // M3), so it is bounded and reported rather than asserted to zero — and the
    // bound is a baseline number, which is what makes it retunable without a
    // rebuild. With the seam off it is exactly 0 and this is a strict equality.
    exciteSlack = (int64_t)BaselineNumber("waterbodyExciteSlackEighths", 0.0);
    const int64_t slack = t.sim.fluidExciteMode != 0 ? exciteSlack : 0;

    if (consErr < -slack || consErr > slack) {
      fail(Format(
          "CONSERVATION: basin %u (slot %u) is off by %+lld eighths over %u "
          "draining ticks. voxels %lld -> %lld (%+lld), ledger drained %lld, "
          "outstanding debit %lld, shave short by %lld, level %d -> %d, "
          "page faults %u, excite candidates %llu",
          desc->basinId, lakeSlot, (long long)consErr, drainTicks,
          (long long)consV0, (long long)consV1, (long long)(consV1 - consV0),
          (long long)consDrained, (long long)consDebit, (long long)consCapped,
          levelBefore, levelAfter, drainPf, (unsigned long long)drainCandid));
    }
    // A conserving drain that drained nothing conserves trivially. These two
    // are what stop pass A being a green light that means nothing.
    if (consDrained <= 0)
      fail("the ledger drained 0 eighths — the test tap never reached it");
    if (consV1 >= consV0)
      fail(Format("the voxels did not lose any water: %lld -> %lld eighths",
                  (long long)consV0, (long long)consV1));
    if (drainPf != 0)
      fail(Format("%u page faults during the drain (lost word 0x%08x, refusing "
                  "chunks %u..%u) — the shave wrote into a sentinel chunk",
                  drainPf, pf2[2], pf2[1] ? pf2[1] - 1u : 0u,
                  pf2[3] ? 0xFFFFFFFFu - pf2[3] : 0u));

    RecordObserved("waterbodyDrainedEighths", (double)consDrained);
    RecordObserved("waterbodyConsErrEighths", (double)consErr);
    RecordObserved("waterbodyLevelDrop", (double)(levelBefore - levelAfter));
    RecordObserved("waterbodyExciteCandidQuiet", (double)quietCandid);
    RecordObserved("waterbodyExciteCandidDrain", (double)drainCandid);

    // ---- X, the verdict ---------------------------------------------------
    // Plan §9 ranks this the most likely way the whole feature fails: WP5 saw a
    // draining CA leave transient gaps under cells and produce 169,616 excite
    // candidates in 400 ticks, converting the entire particle pool. The shave
    // takes from the TOP, so the mechanism should not be there — but the CA
    // re-levelling behind it runs on the chunks the shave woke, and that is the
    // part nobody can reason their way to. The bound is per tick and lives in
    // JSON so it can be moved with evidence rather than with a rebuild.
    const double perTick =
        drainTicks > 0 ? (double)drainCandid / (double)drainTicks : 0.0;
    RecordObserved("waterbodyExciteCandidPerTick", perTick);
    const double candMax = BaselineNumber("waterbodyExciteCandidPerTickMax", 400.0);
    if (perTick > candMax)
      fail(Format(
          "the surface shave feeds the excite detector: %llu candidates over "
          "%u draining ticks (%.1f/tick) against %llu over the same %u quiet "
          "ticks, budget %.0f/tick — this is plan §9's ranked-first risk",
          (unsigned long long)drainCandid, drainTicks, perTick,
          (unsigned long long)quietCandid, drainTicks, candMax));
  }

  // ------------------------------------------------------------------ pass D
  // THE OFF SWITCH, and it is the whole argument for landing M1 on its own.
  // Two runs of an identical script from an identical world, one at mode 0 and
  // one at mode 1, must hash the same. Not "should" — this milestone's entire
  // claim is that it cannot move the world, and a claim that costs one
  // in-process differential to check has no excuse to be an assertion.
  uint32_t hashOff = 0, hashOn = 0;
  {
    Tuning off = base;
    off.sim.waterBodyMode = 0;
    SetCurrentTuning(off);
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    uint32_t k = 1;
    for (uint32_t i = 0; i < 40; i++) {
      SubmitTick(c.ctx, c.world, c.sim, k, kDefaultSeed,
                 SelftestOps(k, kDefaultSeed), {}, {}, false,
                 world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();   // SubmitTick owns the page flip - see above
      k++;
    }
    hashOff = HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);

    Tuning on = base;
    on.sim.waterBodyMode = 1;
    SetCurrentTuning(on);
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    k = 1;
    for (uint32_t i = 0; i < 40; i++) {
      SubmitTick(c.ctx, c.world, c.sim, k, kDefaultSeed,
                 SelftestOps(k, kDefaultSeed), {}, {}, false,
                 world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();   // SubmitTick owns the page flip - see above
      k++;
    }
    hashOn = HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);
    if (hashOff != hashOn)
      fail(Format("the off switch is not an identity: mode 0 hashes %08x, "
                  "mode 1 hashes %08x",
                  hashOff, hashOn));
  }

  // Leave the world the way the ordering in selftest.h assumes it: pristine
  // worldgen at the origin, the shipped tuning, the drain as it was found.
  SetCurrentTuning(base);
  SetHarnessSnapshotDrain(hadDrain);
  WaterBodies().Reset();
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);

  std::string drainNote = "no drain run (an earlier pass failed)";
  if (ranDrain) {
    drainNote = Format(
        "DRAIN %u ticks @ %d eighths/tick: voxels %lld -> %lld (%+lld), "
        "drained %lld, debit %lld, capped %lld, level %d -> %d, area %d, "
        "CONSERVATION %+lld eighths | excite candidates %llu drain vs %llu "
        "quiet (%.1f/tick) | %u page faults",
        drainTicks, (int)std::max<uint32_t>(lakeArea, 1u), (long long)consV0,
        (long long)consV1, (long long)(consV1 - consV0), (long long)consDrained,
        (long long)consDebit, (long long)consCapped, levelBefore, levelAfter,
        lakeArea, (long long)consErr, (unsigned long long)drainCandid,
        (unsigned long long)quietCandid,
        drainTicks ? (double)drainCandid / (double)drainTicks : 0.0, drainPf);
  }
  detail = Format(
      "curve %u levels, round-trips | bowl r%u %lld/%lld cells level/column "
      "walk | %u basins (%u bowls), %u proposed | GPU %s slot %u, volume %d "
      "(reduce %+.4f%% vs sweep), level %d, area %d | lake %llu/%llu "
      "eighths analytic/real (%+.2f%%), surface %u/%u cells (%+.2f%%), spread "
      "%d vox | %s | %s | %u chunks swept | %u awake | %u state flips in 200 "
      "ticks | mode 0/1 hash %08x/%08x",
      curveLevels, tarnR, (long long)tarnCurveCells,
      (long long)tarnColumnCells, basinCount, bowlCount, proposedNow,
      LedgerStateName(lakeState), lakeSlot, lakeVolume, reduceErrPct,
      levelBefore, lakeArea,
      (unsigned long long)desc->volumeEighths,
      (unsigned long long)truth.eighths, volErrPct, desc->surfaceArea,
      truth.surfaceCells, areaErrPct, truth.surfaceMaxY - truth.surfaceMinY,
      bowlNote.c_str(), drainNote.c_str(),
      truth.chunks + bowlTruth.chunks, awake, flips, hashOff, hashOn);
  detail += notes;
  std::printf("waterbody: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WaterGates() {
  static const std::vector<Gate> g = {
      // Depends on `terrain`: that gate is what establishes pristine worldgen at
      // the origin and asserts the CPU height mirror against the GPU's voxels,
      // which is the property this gate's whole analytic basin registry rests
      // on. Running standalone without it would test a curve against a world
      // nobody had checked.
      {"waterbody", "sim", {"terrain"}, false, GateWaterBody},
  };
  return g;
}

}  // namespace selftest
