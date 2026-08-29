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
//   H — THE REAL DRAIN (M3). A 7x7 shaft through the lake floor into a sealed
//       chamber, the discharge law running for 90 ticks, and the same
//       conservation discipline over a box that contains both.
//   B — SPLIT SCHEDULING (M5). A stone partition raised across the lake, the
//       lake drained past its top, and then: the sweep's split elevation
//       against the partition's known top, exactly two adopted descriptors
//       over one basin, their held volumes summing EXACTLY to the basin's
//       voxels, and the measured area(y) against a hand-computed lattice count
//       both above and through the wall. That last pair is what separates "the
//       sweep ran" from "the sweep saw the terrain the player shaped", and it
//       is also pass G extended to a RE-DERIVED basin.
//   F — DETERMINISM, MID-DRAIN (M5). The same script twice from the same fresh
//       worldgen at the same tick numbers, hashed mid-drain and again after.
//       This is the gate on M5's schedule: the container re-derive is the first
//       work in this subsystem spread over ticks, and a schedule is exactly the
//       thing that can be written two ways that look identical and are not.
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
#include "sim/currentprim.h"
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
// `yLo`/`yHi` override the descriptor's own water AABB. Pass H needs that: the
// conservation box for a REAL drain has to contain the shaft and the chamber
// the jet lands in, or the water that left the lake correctly reads as a leak.
VoxelTruth SweepBasin(Ctx& c, const WaterBasin& b, const WaterBodyDesc& d,
                      uint32_t matId, int yLoOverride = 0,
                      int yHiOverride = -1) {
  VoxelTruth t;
  World& world = c.world;
  std::vector<uint32_t> chunk(kChunkVol);
  t.surfaceMinY = 1 << 30;
  t.surfaceMaxY = -(1 << 30);

  // One cell of headroom above the fill level, because "is this cell the free
  // surface" is a question about the cell ABOVE it. Without the extra layer the
  // topmost water would be classified by reading a chunk that was never fetched
  // and every surface cell would be misjudged at once.
  const int y0 = yHiOverride >= yLoOverride ? yLoOverride : d.lo.y;
  const int y1 = yHiOverride >= yLoOverride ? yHiOverride : d.hi.y;
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
  // M3, components 6 + 7 (the hole record and the published discharge). The
  // `_W` suffix is only to keep two of them from colliding with this file's
  // own locals; the word map itself lives in common.wgsl now, because from M3
  // the excite/settle seam reads it too.
  WBS_HOLEKEY_W, WBS_HOLEAREA, WBS_HOLEKEYN_W, WBS_HOLEAREAN_W, WBS_HOLETTL_W,
  WBS_EMIT, WBS_JETV, WBS_EXSHELL,
  // M5, component 10: the re-audit arm, its attribution tick, and the level the
  // ledger resolved for this tick's sweep.
  WBS_REAUDIT_W, WBS_AUDITTICK_W, WBS_SWEEPY_W,
};
// M5 — the SWEEP block's word map, which lives past the end of the ledger in
// the same buffer (world.h's kWaterCurveBase). Must match the SW_* block in
// assets/shaders/common.wgsl.
enum : uint32_t {
  SW_FLOORY = 0, SW_SPANY, SW_TRUNC, SW_SPILLY, SW_SPLITY, SW_COMPS, SW_MAPY,
  SW_MAPGEN, SW_SPILLYN, SW_SPLITYN,
};
constexpr int32_t kSplitNone = -0x40000000;
// THE PASS-H FIXTURE. A 5x5 shaft through the lake floor into a sealed
// 25x25x16 chamber — the same puncture `--fluid-bench wp5` uses, and for the
// same reason: a shaft on its own fills in three ticks and the hole stops
// being a hole, which would make the pass a measurement of a puddle.
constexpr int kShaftDepth = 6;
constexpr int kChamberH = 18;
constexpr int kChamberR = 14;      // half-extent, so 29x29 in plan
constexpr int kShaftR = 3;         // half-extent, so a 7x7 orifice
// 7x7 rather than 5x5 so the analytic Q (Cd*A*sqrt(2gh) = 0.6*49*4*8 = 941
// eighths/tick at the capped head) EXCEEDS sim.drainMaxEighthsPerTick. Plan
// section 6 trap 2 is about what happens when that bound binds, and a fixture
// that never reaches it would not test the thing.
constexpr uint32_t kDrainWindow = 90;
enum : int32_t {
  WB_CANDIDATE = 0, WB_MEASURING = 1, WB_ADOPTED = 2, WB_RELEASING = 3,
};

struct LedgerView {
  // THE WHOLE BUFFER — ledger AND sweep block. Sized from
  // kWaterBodyStateTotalWords and not from the ledger's own extent: the first
  // version of M5 sized this at the ledger's extent while
  // ReadWaterLedgerSync had already been widened to the full buffer, and the
  // overflow took the process out with an empty crash.log and no gate output at
  // all. One constant, one owner.
  std::vector<int32_t> w;
  int32_t At(uint32_t slot, uint32_t f) const {
    const size_t i = (size_t)slot * kWaterBodyStateWords + f;
    return i < w.size() ? w[i] : 0;
  }
  // M5: a word of the per-body SWEEP block.
  int32_t Sw(uint32_t slot, uint32_t f) const {
    const size_t i = (size_t)kWaterCurveBase + (size_t)slot * kWaterCurveWords + f;
    return i < w.size() ? w[i] : 0;
  }
  // M5: area(y) — the measured container curve, cells at world height `y`.
  int32_t Area(uint32_t slot, int floorY, int y) const {
    const int i = y - floorY - 1;
    if (i < 0 || i >= (int)kWaterCurveMaxY) return -1;
    return Sw(slot, kWaterSweepHeaderWords + (uint32_t)i);
  }
};

LedgerView ReadLedger(Ctx& c) {
  LedgerView v;
  v.w.assign((size_t)kWaterBodyStateTotalWords, 0);
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
                       uint64_t* exciteCandid = nullptr,
                       uint32_t* samples = nullptr) {
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
      if (samples) (*samples)++;
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
  uint32_t drainTicks = 0, drainPf = 0, awakeAfterDrainOut = 0;
  uint64_t quietSeen = 0, quietCandid = 0, drainSeen = 0, drainCandid = 0;
  uint32_t quietSamples = 0, drainSamples = 0;
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
    tick = RunQuietTicks(c, tick, drainTicks, &quietSeen, &quietCandid,
                         &quietSamples);

    // ---- A + X, draining arm ---------------------------------------------
    Tuning dr = t;
    dr.sim.waterBodyTestDrain =
        (int)std::min<uint64_t>(std::max<uint32_t>(lakeArea, 1u), 1u << 20);
    SetCurrentTuning(dr);
    // Measured HERE, after the control window and with the tap still shut, so
    // the opening balance is the world the drain actually starts from.
    const VoxelTruth before = SweepBasin(c, *lake, *desc, matId);
    consV0 = (int64_t)before.eighths;
    tick = RunQuietTicks(c, tick, drainTicks, &drainSeen, &drainCandid,
                         &drainSamples);
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

    // LEAVE THE WORLD SETTLED. The drain woke every surface chunk of the lake
    // and the CA is mid-relevel behind it; handing that to pass D's hash
    // identity is what made the FIRST of its arms disagree with the other two
    // (401bbd76 against af008434 twice). The pass that perturbs the world is
    // the pass that owes the cleanup — CLAUDE.md rule 7's "gates share one
    // World", applied inside a gate.
    tick = RunQuietTicks(c, tick, 60);
    const uint32_t awakeAfterDrain = ReadActiveChunksSync(c.ctx, world, c.sim);
    RecordObserved("waterbodyAwakeAfterDrain", (double)awakeAfterDrain);
    awakeAfterDrainOut = awakeAfterDrain;
    if ((double)awakeAfterDrain > awakeMax)
      fail(Format("%u chunks still awake 60 ticks after the drain stopped, "
                  "over the budget of %.0f — a drained lake does not settle",
                  awakeAfterDrain, awakeMax));

    RecordObserved("waterbodyDrainedEighths", (double)consDrained);
    RecordObserved("waterbodyConsErrEighths", (double)consErr);
    RecordObserved("waterbodyLevelDrop", (double)(levelBefore - levelAfter));
    RecordObserved("waterbodyExciteCandidQuiet", (double)quietCandid);
    RecordObserved("waterbodyExciteCandidDrain", (double)drainCandid);
    RecordObserved("waterbodyExciteSeenDrain", (double)drainSeen);
    // A zero candidate count means one of two very different things: the shave
    // creates no air-below (the result plan §9 hopes for), or the detector
    // never ran. `seen` is what tells them apart -- it counts cells the
    // detector LOOKED AT -- and a run that cannot tell them apart has measured
    // nothing.
    if (drainSeen == 0 && drainSamples > 0)
      notes += Format(" -- note: exciteDetect saw 0 settled liquid cells over "
                      "%u draining snapshots at sim.fluidExciteMode %d, so the "
                      "candidate count is a statement about the DETECTOR, not "
                      "about the shave", drainSamples, t.sim.fluidExciteMode);

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


  // =========================================================== pass H (M3)
  //
  // THE REAL DRAIN. Components 6 and 7: a hole is punched in the lake floor,
  // the discharge law computes Q = Cd*A*sqrt(2gh) from the head the GPU ledger
  // owns, that ONE evaluation of `h` produces both the jet's momentum and the
  // ledger's debit, and the water arrives in a sealed chamber as MPM particles.
  // Everything pass A asserts about the test tap, this asserts about a feature.
  //
  // THE IDENTITY IS DIFFERENT, and the difference is the milestone. Pass A's
  // sum is about the LAKE, so `drained` is a term: eighths that left the body
  // forever. Here the water does not leave the WORLD, it leaves the lake and
  // lands 30 voxels lower, and some of it is in flight as particles when the
  // window closes. So the box is drawn around the lake AND the chamber, and:
  //
  //     boxVoxelEighths(t) + inFlightMpm(t) - debit(t)  ==  boxVoxelEighths(0)
  //
  // Every term is measured, none inferred. `debit` is the ledger's own
  // legitimate divergence (eighths owed but not yet shaved) and it is the same
  // stored field pass A uses; `inFlightMpm` is the live particle count minus
  // the dead tail of the reserved op block (FA_SPAWNDEAD), because a particle
  // carries exactly one eighth and a dead slot carries none.
  //
  // TWO ARMS, and the strict one is first. H1 turns the excite seam and the
  // splash coupling OFF: nothing but the discharge and the shave can move an
  // eighth, so the identity is an EQUALITY and any drift is a mass pump. H2 is
  // the shipped configuration with component 7's shell live, where the splash
  // coupling genuinely converts a little water into stain-carrying micro
  // droplets the voxel sweep cannot see; that arm is bounded by a baseline
  // number and REPORTED, exactly as M2 bounded its excite slack.
  struct DrainArm {
    const char* name;
    int exciteMode;
    int shellRadius;
    float splash;
    bool strict;
  };
  const DrainArm arms[2] = {
      {"H1 ledger-only", 0, 0, 0.0f, true},
      {"H2 shipped", t.sim.fluidExciteMode, t.sim.drainExciteRadius,
       t.sim.fluidSplashRate, false},
  };
  // BY VALUE. `lake` and `desc` point into WaterBodySystem's own vectors, and
  // this pass calls Reset() between arms — a descriptor is a description of a
  // world, and the world is rebuilt here. Holding the pointers across that is a
  // use-after-free whose symptom would be a plausible-looking wrong basin.
  const WaterBasin lakeGeo = *lake;
  const WaterBodyDesc lakeDesc = *desc;
  std::string holeNote = "pass H did not run (an earlier pass failed)";
  int64_t shellCells = 0, hEmit1 = 0, hErr1 = 0;
  for (int ai = 0; ai < 2 && ok; ai++) {
    const DrainArm& arm = arms[ai];
    // A FRESH WORLD PER ARM. The previous arm carved a chamber and filled it;
    // starting the second on that is the same "the pass that perturbs the world
    // owes the cleanup" hazard pass D's third arm exists to catch, except here
    // it would silently change the head rather than the hash.
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    Tuning ht = t;
    ht.sim.fluidExciteMode = arm.exciteMode;
    ht.sim.drainExciteRadius = arm.shellRadius;
    ht.sim.fluidSplashRate = arm.splash;
    ht.sim.waterBodyTestDrain = 0;   // the DISCHARGE is the source now
    SetCurrentTuning(ht);
    tick = RunQuietTicks(c, tick, 130);

    const WaterBodyDesc* hd = WaterBodies().Find(1);
    if (!hd || hd->gpuSlot >= kWaterBodyCap) {
      fail(Format("pass %s: the authored lake is not proposed", arm.name));
      break;
    }
    const uint32_t hSlot = hd->gpuSlot;
    {
      const LedgerView lv0 = ReadLedger(c);
      if (lv0.At(hSlot, WBS_STATE) != WB_ADOPTED) {
        fail(Format("pass %s: the lake is %s, not adopted, before the punch",
                    arm.name, LedgerStateName(lv0.At(hSlot, WBS_STATE))));
        break;
      }
    }

    // ---- the punch, and the chamber it drains into ----------------------
    const int hFloorY = lakeGeo.floorY;
    const int chTop = hFloorY - kShaftDepth;         // chamber roof
    const int chBot = chTop - kChamberH;             // chamber floor
    std::vector<CellOp> punch;
    for (int y = chBot; y <= hFloorY; y++) {
      const bool inShaft = y > chTop;
      const int half = inShaft ? kShaftR : kChamberR;
      for (int z = lakeGeo.cz - half; z <= lakeGeo.cz + half; z++)
        for (int x = lakeGeo.cx - half; x <= lakeGeo.cx + half; x++) {
          const bool wall = !inShaft && (y == chBot ||
                                         std::abs(x - lakeGeo.cx) == kChamberR ||
                                         std::abs(z - lakeGeo.cz) == kChamberR);
          punch.push_back({World::SlotCellIndex({x, y, z}),
                           wall ? (uint32_t)kMatStone : 0u});
        }
    }
    // The box every conservation number below is measured over. It contains
    // the lake, the shaft and the chamber, so water that legitimately LEFT the
    // lake is still inside the sum.
    const int boxLo = chBot, boxHi = lakeGeo.surfY;
    const VoxelTruth h0 = SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi);
    uint32_t pfBefore[4] = {0, 0, 0, 0};
    ReadPageFaultsSync(c.ctx, world, pfBefore);

    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, punch, false,
               c.world.WindowOrigin(), true, false);
    c.ctx.ProcessEvents();
    tick++;

    uint64_t seen = 0, cand = 0;
    uint32_t samples = 0;
    tick = RunQuietTicks(c, tick, kDrainWindow, &seen, &cand, &samples);
    // SETTLE, with the hole still open: the jet is still in flight and the
    // ledger still owes a debit the shave has not taken. Measuring before this
    // would charge the difference to the feature.
    tick = RunQuietTicks(c, tick, 90);

    const VoxelTruth h1 = SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi);
    const LedgerView lv = ReadLedger(c);
    uint32_t fa[32] = {};
    ReadFluidArgsSync(c.ctx, world, fa);
    // ONE EIGHTH PER PARTICLE (every seam-born particle carries fullness 1),
    // minus the dead tail of this tick's reserved discharge block.
    const int64_t inFlight = (int64_t)fa[7] - (int64_t)std::min(fa[29], fa[7]);
    const int64_t debitNow = lv.At(hSlot, WBS_DEBIT);
    const int64_t drainedNow = lv.At(hSlot, WBS_DRAINED);
    const int64_t err =
        (int64_t)h1.eighths + inFlight - debitNow - (int64_t)h0.eighths;
    uint32_t pfAfter[4] = {0, 0, 0, 0};
    ReadPageFaultsSync(c.ctx, world, pfAfter);

    if (ai == 0) {
      hEmit1 = drainedNow;
      hErr1 = err;
      RecordObserved("waterbodyDrainH1Eighths", (double)drainedNow);
      RecordObserved("waterbodyDrainH1ErrEighths", (double)err);
    } else {
      shellCells = lv.At(hSlot, WBS_EXSHELL);
      RecordObserved("waterbodyShellCells", (double)shellCells);
      RecordObserved("waterbodyDrainH2Eighths", (double)drainedNow);
      RecordObserved("waterbodyDrainH2ErrEighths", (double)err);
      RecordObserved("waterbodyDrainExciteCandPerTick",
                     (double)cand / (double)kDrainWindow);
      holeNote = Format(
          "DRAIN(real hole, %u ticks) H1 %lld eighths err %+lld | H2 %lld "
          "eighths err %+lld, shell %lld cells @r%d, excite %llu cand / %llu "
          "seen over %u snaps (%.1f cand/tick), level %d, hole area %d, jet "
          "v %d Q16.16, in flight %lld, %u live / %u dead ops",
          kDrainWindow, (long long)hEmit1, (long long)hErr1,
          (long long)drainedNow, (long long)err, (long long)shellCells,
          arm.shellRadius, (unsigned long long)cand, (unsigned long long)seen,
          samples, (double)cand / (double)kDrainWindow,
          lv.At(hSlot, WBS_LEVEL), lv.At(hSlot, WBS_HOLEAREA),
          lv.At(hSlot, WBS_JETV), (long long)inFlight, fa[7], fa[29]);
    }

    // WHY NEITHER ARM IS A STRICT EQUALITY, and it is worth being exact about
    // because pass A's IS one. Pass A's identity is about the LAKE and its
    // ledger: the only movers are the shave and the tap, both of which report
    // what they granted, so it closes at +0 and any drift is a mass pump.
    //
    // This identity is about a BOX containing a violent, churning MPM pool, and
    // the box has downstream physics in it that the water-body system does not
    // own and must not pretend to: the CA's thin-film handling of water sheeting
    // down a shaft wall, the sun/water evaporation rule on any cell that ends up
    // exposed, and the wake trigger converting CA water near the jet. Measured
    // at -37 eighths against 35,381 drained and 40,342 in flight (0.09%), all of
    // it downstream of the ledger — `capped` is 0, so the shave was never short
    // and the debit followed what it granted, every tick.
    //
    // So the bound is small and it lives in JSON: it is an assertion that the
    // discharge is not a PUMP, not a claim that a churning pool is lossless.
    const int64_t slack = (int64_t)BaselineNumber(
        arm.strict ? "waterbodyDrainSlackStrictEighths"
                   : "waterbodyDrainSlackEighths",
        arm.strict ? 256.0 : 4096.0);
    if (err < -slack || err > slack)
      fail(Format(
          "CONSERVATION (pass %s): basin %u slot %u is off by %+lld eighths. "
          "box %llu -> %llu (%+lld), in flight %lld (live %u, dead ops %u), "
          "ledger drained %lld, outstanding debit %lld, capped %d, level %d, "
          "hole area %d, %u page faults",
          arm.name, lakeDesc.basinId, hSlot, (long long)err,
          (unsigned long long)h0.eighths, (unsigned long long)h1.eighths,
          (long long)((int64_t)h1.eighths - (int64_t)h0.eighths),
          (long long)inFlight, fa[7], fa[29], (long long)drainedNow,
          (long long)debitNow, lv.At(hSlot, WBS_CAPPED),
          lv.At(hSlot, WBS_LEVEL), lv.At(hSlot, WBS_HOLEAREA),
          pfAfter[0] - pfBefore[0]));
    // A conserving drain that never drained is a green light meaning nothing —
    // the same guard pass A carries, and it is what would catch a hole detector
    // that never fired or an op block that was never reserved.
    if (drainedNow <= 0)
      fail(Format("pass %s: the discharge emitted 0 eighths — the hole was "
                  "never detected (hole key %d, area %d, ttl %d, level %d, "
                  "floor %d)",
                  arm.name, lv.At(hSlot, WBS_HOLEKEY_W),
                  lv.At(hSlot, WBS_HOLEAREA), lv.At(hSlot, WBS_HOLETTL_W),
                  lv.At(hSlot, WBS_LEVEL), hFloorY));
    if (pfAfter[0] != pfBefore[0])
      fail(Format("pass %s: %u page faults during the drain (lost word "
                  "0x%08x) — the shave wrote into a sentinel chunk",
                  arm.name, pfAfter[0] - pfBefore[0], pfAfter[2]));
    // COMPONENT 7's BUDGET, plan section 9 item 2. The shell mitigation was
    // UNMEASURED in the plan; this is the measurement, and it is asserted
    // rather than only printed so a future radius change cannot quietly
    // reintroduce the solid ball's ~33,000 particles against a ~40,000
    // envelope.
    if (ai == 1) {
      const double shellMax = BaselineNumber("waterbodyShellCellMax", 20000.0);
      if ((double)shellCells > shellMax)
        fail(Format("component 7's shell converted %lld cells at radius %d, "
                    "over the budget of %.0f — that is the solid-ball cost "
                    "plan section 9 item 2 says must not be paid",
                    (long long)shellCells, arm.shellRadius, shellMax));
      // Plan section 9 item 1, REOPENED by M3: a real jet at the throat can
      // feed the excite detector the way WP5's draining CA did (169,616
      // candidates / 400 ticks on worldlake). Both halves are reported because
      // a bare 0 is unattributable — `seen` is what separates "the mechanism is
      // not there" from "the detector never ran".
      const double perTickH = (double)cand / (double)kDrainWindow;
      const double candMaxH =
          BaselineNumber("waterbodyDrainExciteCandPerTickMax", 3000.0);
      if (perTickH > candMaxH)
        fail(Format("the real drain feeds the excite detector: %llu candidates "
                    "over %u ticks (%.1f/tick) against %llu cells seen, budget "
                    "%.0f/tick — this is plan section 9's ranked-first risk, "
                    "reopened at the throat",
                    (unsigned long long)cand, kDrainWindow, perTickH,
                    (unsigned long long)seen, candMaxH));
    }
    // Leave the world settled and pristine for pass D, which hashes it.
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }


  // ========================================================== pass B (M5)
  //
  // SPLIT SCHEDULING, and with it the whole of component 2's case-2 sweep and
  // component 10's discovery. Plan section 7's row for this pass:
  //
  //   > A pond with a known interior high point drains past a partition
  //   > elevation; exactly two descriptors appear at the predicted level,
  //   > volumes summing to the parent's.
  //
  // THE FIXTURE. A stone wall is raised across the authored lake from its floor
  // to a known top, submerged and therefore invisible to the level model — one
  // pool, one descriptor, one surface. Then the test tap drains the lake past
  // the wall's top. From that moment the basin is physically two puddles, and
  // before M5 the level model could not see it: the shave went on taking an
  // eighth off both surfaces at one level while the hole was in only one of
  // them, so a puddle with nothing draining it went on descending.
  //
  // WHAT IS BEING ASSERTED, and each of these fails differently:
  //
  //   * SW_SPLITY == the wall's top. Output 3 of the sweep, i.e. the merge tree
  //     read downward. A wrong value here means the level scan or the label
  //     propagation is off, and it is the one number nothing else checks.
  //   * TWO adopted descriptors over one basin. Output 4 reaching the ladder.
  //   * held(parent) + held(child) == the CPU's own sweep of the lake's voxels.
  //     THE MASS STATEMENT, and the reason a split needs no new arithmetic:
  //     both bodies measured their own cells with the existing adoption reduce,
  //     so the sum is exact by measurement rather than by a division that could
  //     round. This is also pass G extended to a re-derived basin — a recompute
  //     from voxels, asserted against the live descriptors.
  //   * area(y) against a hand-computed lattice count, ABOVE and BELOW the wall
  //     top. Output 1, verified against a known bowl: above the wall the count
  //     is the plain disc, below it the disc minus the wall's cross-section.
  //     Two different numbers from one table is what separates "the sweep ran"
  //     from "the sweep saw the terrain".
  //   * SW_SPILLY == WB_HOLE_NONE. Output 2: an intact rim does not leak. A
  //     probe that wandered inside the disc would report the pool floor here.
  int32_t splitY = kSplitNone, splitComps = 0, splitSpill = 0;
  int64_t heldParent = 0, heldChild = 0, splitVox = 0;
  int32_t areaAbove = 0, areaBelow = 0;
  int64_t wantAbove = 0, wantBelow = 0;
  uint32_t splitSlots = 0;
  int wallTopY = 0;
  std::string splitNote = "pass B did not run (an earlier pass failed)";
  if (ok) {
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    SetCurrentTuning(t);
    tick = RunQuietTicks(c, tick, 130);

    const WaterBodyDesc* pd = WaterBodies().Find(1);
    // BY VALUE, IMMEDIATELY. `pd` points into WaterBodySystem's own vector and
    // every RunQuietTicks below re-runs Classify — the same use-after-free
    // pass H's `lakeGeo`/`lakeDesc` copies exist to avoid, and here it would
    // have surfaced as a plausible-looking wrong slot rather than a crash.
    const uint32_t pSlot = pd ? pd->gpuSlot : kNoGpuSlot;
    if (!pd || pSlot >= kWaterBodyCap) {
      fail("pass B: the authored lake is not proposed");
    } else {
      // THE WALL. Nine cells thick, and the thickness is arithmetic rather
      // than taste: the split grid is kWaterSplitGrid across the basin's
      // column AABB, so one grid cell spans ceil(137/48) = 3 columns, and a
      // grid cell counts as OPEN if ANY of its columns is. A partition
      // narrower than 2*step-1 = 5 columns can therefore straddle every grid
      // column it touches and be missed — the LIBERAL direction, which
      // under-splits and is safe (world.h's kWaterSplitGrid note). Nine
      // guarantees at least two fully-blocked grid columns whatever the
      // alignment, which is what makes this a test of the labelling rather
      // than of the alignment.
      const int wallHalf = 4;
      const int wallH = 20;
      wallTopY = lakeGeo.floorY + wallH;
      std::vector<CellOp> wall;
      for (int y = lakeGeo.floorY + 1; y <= wallTopY; y++)
        for (int z = lakeGeo.cz - lakeGeo.radius; z <= lakeGeo.cz + lakeGeo.radius; z++)
          for (int x = lakeGeo.cx - wallHalf; x <= lakeGeo.cx + wallHalf; x++) {
            const int64_t dx = x - lakeGeo.cx, dz = z - lakeGeo.cz;
            if (dx * dx + dz * dz > lakeGeo.discD2Max) continue;
            wall.push_back({World::SlotCellIndex({x, y, z}),
                            (uint32_t)kMatStone});
          }
      // The hand-computed expectations for output 1, by an independent lattice
      // walk. Not `pi r^2` and not a copy of the curve builder: two
      // implementations of one count is the whole point of a verification.
      for (int z = lakeGeo.cz - lakeGeo.radius; z <= lakeGeo.cz + lakeGeo.radius; z++)
        for (int x = lakeGeo.cx - lakeGeo.radius; x <= lakeGeo.cx + lakeGeo.radius; x++) {
          const int64_t dx = x - lakeGeo.cx, dz = z - lakeGeo.cz;
          if (dx * dx + dz * dz > lakeGeo.discD2Max) continue;
          wantAbove++;
          if (std::abs(x - lakeGeo.cx) > wallHalf) wantBelow++;
        }

      // ---- THE ORDER IS THE FIXTURE, and the first version had it backwards.
      //
      // DRAIN FIRST, THEN RAISE THE WALL. Building a submerged partition and
      // draining past it looks like the more natural script and it does not
      // work: the side with the drain descends, the other side SPILLS OVER the
      // partition into it, and the two settle with the far pool sitting exactly
      // AT the partition's top — permanently trickling, never quiet, so the
      // second descriptor never clears its quiescence window and the pass
      // measures one body forever. Measured as `child ... quiet 0` after 368
      // settling ticks in a world the terrain gate proves reaches zero awake
      // chunks at 120.
      //
      // Draining first and then raising the wall THROUGH the free surface
      // leaves two pools at the same level with a partition standing two
      // voxels proud of both. That is the configuration a draining lake
      // actually reaches on its own — plan section 2: "any bowl with an uneven
      // floor becomes two puddles as the level falls past the high point
      // between them" — reached by construction instead of by a race.
      Tuning bt = t;
      bt.sim.waterBodyTestDrain = (int)std::max<int64_t>(wantAbove / 8, 1);
      SetCurrentTuning(bt);
      // 400 ticks at an eighth of an eighth-step is ~6 voxels down, which puts
      // the free surface two voxels under the partition's top. `steps` stays 0
      // at this rate so the ledger's outstanding debit is bounded by `area`
      // every tick and nothing accumulates — a body carrying a large debit when
      // its footprint shrinks pays all of it out of the part it kept, which is
      // the milestone's one named leftover (plan section 1.5).
      tick = RunQuietTicks(c, tick, 400);
      SetCurrentTuning(t);
      tick = RunQuietTicks(c, tick, 60);

      // NOW the partition, through the free surface. This is also what ARMS the
      // re-derive: a mutation in a chunk the registry labelled is component
      // 10's whole detection.
      SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, wall, false,
                 c.world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();
      tick++;
      // One full sweep cycle plus room for the ladder: the cycle is 2 * span
      // scheduled steps at kWaterSweepPeriod ticks each (odd steps walk the
      // cursor down the Y span and build the table, even steps refresh the
      // split map at the LIVE level), then the second descriptor needs its
      // quiescence window and one measuring tick. This is the honest cost of a
      // re-derive and it is quoted rather than rounded up.
      const int span = std::clamp(lakeGeo.spillY - lakeGeo.floorY, 1,
                                  (int)kWaterCurveMaxY);
      const uint32_t cycleTicks = 2u * (uint32_t)span * kWaterSweepPeriod;
      tick = RunQuietTicks(c, tick, 2u * cycleTicks + 200);

      const LedgerView lv = ReadLedger(c);
      splitY = lv.Sw(pSlot, SW_SPLITY);
      splitComps = lv.Sw(pSlot, SW_COMPS);
      splitSpill = lv.Sw(pSlot, SW_SPILLY);
      areaAbove = lv.Area(pSlot, lakeGeo.floorY, wallTopY + 2);
      areaBelow = lv.Area(pSlot, lakeGeo.floorY, wallTopY - 2);

      const WaterBodyDesc* cd = WaterBodies().FindChild(1);
      const uint32_t cSlot = cd ? cd->gpuSlot : kNoGpuSlot;
      if (lv.At(pSlot, WBS_STATE) == WB_ADOPTED) splitSlots++;
      if (cSlot < kWaterBodyCap && lv.At(cSlot, WBS_STATE) == WB_ADOPTED)
        splitSlots++;
      // held = VOLUME - DRAINED is the water this body still owns; the voxels
      // it is standing on are `held + debit`, because a debit is water already
      // accounted gone that no shave has taken off the cells yet (plan section
      // 3.3's legitimate divergence). Summing that form is what makes the
      // comparison against a raw voxel sweep exact rather than approximate.
      // held = VOLUME - DRAINED, and after the settle below both debits are
      // zero, so `held` IS the water the body is standing on. The debit term is
      // not added: the re-audit already folded it in (see the WBS_REAUDIT
      // consume in sim_waterbody.wgsl), and adding it again is how the first
      // version of this pass reported a basin holding a third more than it did.
      heldParent = (int64_t)lv.At(pSlot, WBS_VOLUME) - lv.At(pSlot, WBS_DRAINED);
      if (cSlot < kWaterBodyCap)
        heldChild = (int64_t)lv.At(cSlot, WBS_VOLUME) - lv.At(cSlot, WBS_DRAINED);
      // The split map's own histogram, decoded straight out of the buffer the
      // gate already read. Attribution before it is needed (plan section 7): a
      // wrong `held` sum is either "the labelling put the cells in the wrong
      // component" or "the ledger arithmetic is off", and these four counts are
      // what separate them.
      uint32_t comph[4] = {0, 0, 0, 0};
      for (uint32_t g = 0; g < kWaterSplitCells; g++) {
        const int32_t w =
            lv.Sw(pSlot, kWaterSweepHeaderWords + kWaterCurveMaxY + (g >> 4));
        comph[((uint32_t)w >> ((g & 15u) * 2u)) & 3u]++;
      }
      const VoxelTruth bt2 = SweepBasin(c, lakeGeo, lakeDesc, matId);
      splitVox = (int64_t)bt2.eighths;

      // ---- the assertions ------------------------------------------------
      if (splitY != wallTopY)
        fail(Format("pass B: the sweep put the split elevation at %d, but the "
                    "partition's top is at y=%d (floor %d, %d components at "
                    "the live level)",
                    splitY, wallTopY, lakeGeo.floorY, splitComps));
      if (splitComps < 2)
        fail(Format("pass B: the basin still reads as %d component(s) at the "
                    "live level %d, below the partition top %d — the split map "
                    "never reached the footprint test",
                    splitComps, lv.At(pSlot, WBS_LEVEL), wallTopY));
      // ATTRIBUTION, not a bare count (CLAUDE.md rule 6). "1 descriptor, not
      // 2" is true of a child that was never proposed, one the ladder refused
      // for volume, one still counting quiet ticks and one whose footprint test
      // answered "I own nothing" — four different fixes. Every word that
      // separates them is printed.
      if (splitSlots != 2)
        fail(Format("pass B: %u descriptors are adopted over basin 1, not 2. "
                    "parent slot %u is %s; child slot %u is %s (quiet %d, "
                    "reduce sum %d, volume %d, level %d, min volume %d, %u "
                    "chunks listed)",
                    splitSlots, pSlot, LedgerStateName(lv.At(pSlot, WBS_STATE)),
                    cSlot,
                    cSlot < kWaterBodyCap
                        ? LedgerStateName(lv.At(cSlot, WBS_STATE))
                        : "unproposed",
                    lv.At(cSlot, WBS_QUIET), lv.At(cSlot, WBS_RSUM),
                    lv.At(cSlot, WBS_VOLUME), lv.At(cSlot, WBS_LEVEL),
                    t.sim.waterBodyMinVolume,
                    cd ? (unsigned)cd->chunks.size() : 0u));
      if (splitSlots == 2 &&
          (lv.At(pSlot, WBS_DEBIT) != 0 || lv.At(cSlot, WBS_DEBIT) != 0))
        fail(Format("pass B: the ledgers did not settle — parent debit %d, "
                    "child debit %d after the drain stopped, so `held` is not "
                    "yet the water on the cells",
                    lv.At(pSlot, WBS_DEBIT), lv.At(cSlot, WBS_DEBIT)));
      if (splitSlots == 2 && heldParent + heldChild != splitVox)
        fail(Format(
            "pass B: the split is not mass-exact. parent holds %lld + child "
            "holds %lld = %lld eighths, but the voxels of basin 1 sum to %lld "
            "(%+lld). parent volume %d drained %d debit %d, child volume %d "
            "drained %d debit %d",
            (long long)heldParent, (long long)heldChild,
            (long long)(heldParent + heldChild), (long long)splitVox,
            (long long)(heldParent + heldChild - splitVox),
            lv.At(pSlot, WBS_VOLUME), lv.At(pSlot, WBS_DRAINED),
            lv.At(pSlot, WBS_DEBIT), lv.At(cSlot, WBS_VOLUME),
            lv.At(cSlot, WBS_DRAINED), lv.At(cSlot, WBS_DEBIT)));
      if ((int64_t)areaAbove != wantAbove)
        fail(Format("pass B: the measured curve says %d cells at y=%d (above "
                    "the partition), a lattice walk of the same disc says %lld",
                    areaAbove, wallTopY + 2, (long long)wantAbove));
      if ((int64_t)areaBelow != wantBelow)
        fail(Format("pass B: the measured curve says %d cells at y=%d (through "
                    "the partition), the disc minus the wall is %lld — the "
                    "sweep is not seeing the terrain the player shaped",
                    areaBelow, wallTopY - 2, (long long)wantBelow));
      if (splitSpill != 0x7FFFFFFF)
        fail(Format("pass B: the sweep reports the lake spilling at y=%d, but "
                    "its rim is intact — the spill probe is reading inside the "
                    "disc", splitSpill));
      splitNote = Format(
          "SPLIT: wall top y=%d, sweep split y=%d, %d components, %u adopted "
          "descriptors, held %lld + %lld = %lld vs %lld voxel eighths (%+lld), "
          "map %u/%u/%u/%u cells by component, area(y=%d) %d/%lld above and "
          "(y=%d) %d/%lld through the wall, spill %s",
          wallTopY, splitY, splitComps, splitSlots, (long long)heldParent,
          (long long)heldChild, (long long)(heldParent + heldChild),
          (long long)splitVox,
          (long long)(heldParent + heldChild - splitVox), comph[0], comph[1],
          comph[2], comph[3], wallTopY + 2,
          areaAbove, (long long)wantAbove, wallTopY - 2, areaBelow,
          (long long)wantBelow,
          splitSpill == 0x7FFFFFFF ? "none (rim intact)" : "FOUND");
    }
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  // ========================================================== pass F (M5)
  //
  // DETERMINISM, MID-DRAIN. Plan section 7's row: "same seed, two runs, drain in
  // progress at the compare tick", catching "a re-derive scheduled on CPU
  // convenience (discipline 3.4)".
  //
  // WHY THIS PASS COULD NOT EXIST BEFORE M5, and why it has to exist now. Until
  // this milestone nothing in the water system was spread over ticks: the ledger
  // ran every tick, the shave ran every tick, the reduce ran once. M5 introduces
  // the first SCHEDULED work in the subsystem — a container re-derive walked one
  // level at a time — and a schedule is exactly the thing that can be written
  // two ways that look identical and are not. `basinId % N == tick % N` is
  // reproducible; "the dirty one I noticed first" is not, and neither is
  // "whichever readback had arrived".
  //
  // TWO ARMS AND TWO CHECKPOINTS. Both arms run the SAME script from the SAME
  // fresh worldgen at the SAME tick numbers — the tick base is fixed rather
  // than carried from the passes above, because hash3 keys on the tick and a
  // second arm running at t+900 is a different world by construction, not a
  // determinism failure. The world is hashed twice: once mid-drain with the jet
  // in flight and the sweep mid-cycle, once after. Two checkpoints rather than
  // one because a single end-of-run comparison cannot say WHEN two runs
  // diverged, and this pass exists to point at a schedule.
  //
  // TWO ARMS ARE ENOUGH HERE, and it is worth saying why given that passes D
  // and S both need three. Those two compare a feature ON against the feature
  // OFF, so a third arm is what separates "the feature moved the world" from
  // "arm 1 inherited state arm 2 did not". Here both arms are the SAME
  // configuration; there is no ON/OFF question, only "does this script produce
  // one world or two", and each arm rebuilds the world itself.
  uint32_t detMid[2] = {0, 0}, detEnd[2] = {0, 0};
  {
    const uint32_t kBase = 90000;
    for (int a = 0; a < 2; a++) {
      SetCurrentTuning(t);
      WaterBodies().Reset();
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      uint32_t k = RunQuietTicks(c, kBase, 130);
      // The same 7x7 shaft pass H bores, which is what ARMS the whole thing:
      // it opens a real hole for the discharge AND marks the basin's curve
      // dirty, so both the drain and the scheduled re-derive are live across
      // the compare.
      std::vector<CellOp> punch;
      const int chTop = lakeGeo.floorY - kShaftDepth;
      const int chBot = chTop - kChamberH;
      for (int y = chBot; y <= lakeGeo.floorY; y++) {
        const bool inShaft = y > chTop;
        const int half = inShaft ? kShaftR : kChamberR;
        for (int z = lakeGeo.cz - half; z <= lakeGeo.cz + half; z++)
          for (int x = lakeGeo.cx - half; x <= lakeGeo.cx + half; x++) {
            const bool wall = !inShaft && (y == chBot ||
                                           std::abs(x - lakeGeo.cx) == kChamberR ||
                                           std::abs(z - lakeGeo.cz) == kChamberR);
            punch.push_back({World::SlotCellIndex({x, y, z}),
                             wall ? (uint32_t)kMatStone : 0u});
          }
      }
      SubmitTick(c.ctx, c.world, c.sim, k, kDefaultSeed, {}, {}, punch, false,
                 c.world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();
      k++;
      k = RunQuietTicks(c, k, 45);
      detMid[a] = HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);
      k = RunQuietTicks(c, k, 45);
      detEnd[a] = HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);
    }
    if (detMid[0] != detMid[1])
      fail(Format(
          "DETERMINISM (pass F): two identical runs disagree MID-DRAIN — "
          "%08x against %08x, 45 ticks after the punch. Something in the water "
          "system is scheduled on CPU convenience rather than on the tick "
          "(plan discipline 3.4); the re-derive schedule is the first suspect",
          detMid[0], detMid[1]));
    else if (detEnd[0] != detEnd[1])
      fail(Format(
          "DETERMINISM (pass F): two identical runs agree mid-drain (%08x) and "
          "disagree 45 ticks later — %08x against %08x. The divergence is in "
          "the second half of the window, after the first sweep cycle closed",
          detMid[0], detEnd[0], detEnd[1]));
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  // ------------------------------------------------------------------ pass D
  // THE OFF SWITCH, and it is the whole argument for landing M1 and M2 without
  // a rebaseline. An identical 40-tick mutation script from an identical world
  // must hash the same at sim.waterBodyMode 0 and 1.
  //
  // THREE ARMS, NOT TWO, and the third one is the whole point. This pass runs
  // LAST, after pass A has drained a lake and left the world in a state the
  // preceding passes did not. CLAUDE.md rule 7 is about gates sharing one
  // World; the same hazard exists WITHIN a gate, and a two-arm comparison
  // cannot tell "mode 1 changed the world" from "arm 1 inherited something arm
  // 2 did not". Running mode 0 again at the end separates them in one
  // invocation:
  //
  //     arm1 != arm3  ->  the world entering this pass was not clean. The
  //                       finding is about pass ordering, not the off switch.
  //     arm1 == arm3 != arm2  ->  the off switch is genuinely broken, which is
  //                       a rebaseline-blocking regression.
  //
  // That is the "add attribution to the reporter rather than A/B-eliminate"
  // rule applied to a gate: one run prints which of the two it is.
  uint32_t hashOff = 0, hashOn = 0, hashOff2 = 0;
  {
    auto arm = [&](int mode) {
      Tuning a = base;
      a.sim.waterBodyMode = mode;
      SetCurrentTuning(a);
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      for (uint32_t i = 0, k = 1; i < 40; i++, k++) {
        SubmitTick(c.ctx, c.world, c.sim, k, kDefaultSeed,
                   SelftestOps(k, kDefaultSeed), {}, {}, false,
                   world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();   // SubmitTick owns the page flip
      }
      return HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);
    };
    hashOff = arm(0);
    hashOn = arm(1);
    hashOff2 = arm(0);
    if (hashOff != hashOff2) {
      fail(Format(
          "pass D is not measuring the off switch: the SAME mode-0 script "
          "hashed %08x the first time and %08x the third, so the world "
          "entering this pass carries state from an earlier pass (mode 1 "
          "hashed %08x in between). Fix the ordering, not the feature",
          hashOff, hashOff2, hashOn));
    } else if (hashOff != hashOn) {
      fail(Format("the off switch is not an identity: mode 0 hashes %08x "
                  "(twice), mode 1 hashes %08x",
                  hashOff, hashOn));
    }
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
        "CONSERVATION %+lld eighths | excite drain %llu cand / %llu seen over "
        "%u snaps, quiet %llu cand / %llu seen over %u snaps (%.1f cand/tick) "
        "| %u page faults",
        drainTicks, (int)std::max<uint32_t>(lakeArea, 1u), (long long)consV0,
        (long long)consV1, (long long)(consV1 - consV0), (long long)consDrained,
        (long long)consDebit, (long long)consCapped, levelBefore, levelAfter,
        lakeArea, (long long)consErr, (unsigned long long)drainCandid,
        (unsigned long long)drainSeen, drainSamples,
        (unsigned long long)quietCandid, (unsigned long long)quietSeen,
        quietSamples,
        drainTicks ? (double)drainCandid / (double)drainTicks : 0.0, drainPf);
    drainNote += Format(", %u awake 60 ticks later", awakeAfterDrainOut);
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
      (unsigned long long)lakeDesc.volumeEighths,
      (unsigned long long)truth.eighths, volErrPct, lakeDesc.surfaceArea,
      truth.surfaceCells, areaErrPct, truth.surfaceMaxY - truth.surfaceMinY,
      bowlNote.c_str(), drainNote.c_str(),
      truth.chunks + bowlTruth.chunks, awake, flips, hashOff, hashOn);
  detail += Format(" (mode 0 again %08x)", hashOff2);
  detail += " | " + holeNote;
  detail += " | " + splitNote;
  detail += Format(
      " | DETERMINISM mid-drain %08x/%08x, end %08x/%08x",
      detMid[0], detMid[1], detEnd[0], detEnd[1]);
  detail += notes;
  std::printf("waterbody: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace


// ============================================================================
// `--gate current` - the acceptance gate for docs/PLAN_water_master.md M4,
// component 8 (the current field).
//
// TWO PASSES, and they answer the two questions that are genuinely separate:
//
//   P - THE PROFILES ARE THE PROFILES. Pure arithmetic against CurrentAtCpu,
//       no GPU and no world. The whole design rests on one asymmetry - a
//       vortex that falls off as Gamma/2*pi*r and REACHES, against a sink that
//       falls off as 1/r^2 and does not - because that asymmetry is why real
//       whirlpools look enormous while the actual suction is a small throat.
//       If the two profiles were swapped, or if either had silently become the
//       other under an integer truncation, the field would still look busy in
//       a screenshot and would be wrong in the one way that matters. This pass
//       also asserts the two things a pure function must do: exactly zero
//       outside the union AABB, and exactly zero once the decay window closes
//       (a funnel standing open in still water is plan component 8's named
//       failure).
//
//   S - THE SIM ARM'S OFF SWITCH, in THREE arms, for pass D's reason: two arms
//       cannot tell "mode 1 changed the world" from "arm 1 inherited something
//       arm 2 did not". Mode 0 / mode 1 / mode 0 over an identical fluid pour
//       with an identical whirlpool standing in it.
//
//       Unlike pass D, arm 2 is REQUIRED TO DIFFER. Pass D proves an off
//       switch; this proves an off switch AND that the knob reaches the kernel,
//       which is the half `--sweep` cannot establish in a world with no
//       primitives in it (M1's note on "ALL HASHES IDENTICAL" is about exactly
//       that ambiguity). arm1 == arm3 != arm2 is the only passing shape.
Status GateCurrent(Ctx& c, std::string& detail) {
  World& world = c.world;
  const Tuning base = CurrentTuning();
  std::vector<std::string> fails;
  auto fail = [&](const std::string& m) { fails.push_back(m); };

  // ------------------------------------------------------------------ pass P
  // A whirlpool and a drain throat, placed nowhere in particular: this pass
  // never touches a voxel, so the world is irrelevant to it.
  const int kR = 64;            // vortex radius, cells
  const int kCx = 200, kCy = 100, kCz = 200;
  double vortRatio = 0.0, sinkRatio = 0.0, tangCos = 1.0;
  {
    CurrentPrims().Clear();
    CurrentPrim v;
    v.kind = kCurrentPrimVortex;
    v.flags = kCurrentPrimSim;
    v.x = kCx; v.y = kCy; v.z = kCz;
    v.radius = kR;
    v.reach = kR;
    v.swirlQ = 1 << 16;
    v.decayTicks = kCurrentPrimForever;
    v.spawnTick = 0;
    v.seenTick = 0;
    v.ownerId = 0x7E57u;
    CurrentPrimAim(v, Vec3{0.0f, -1.0f, 0.0f}, 4.0f);
    if (!CurrentPrims().Spawn(v)) fail("pass P: the vortex would not spawn");
    // Past the attack ramp, so the envelope is 1 and the profile is the
    // profile rather than a sixth of it.
    CurrentPrims().Tick(64);

    auto speedAt = [&](float dx) {
      const Vec3 f = CurrentAtCpu(Vec3{(float)kCx + dx, (float)kCy, (float)kCz});
      return (double)std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    };
    // THE VORTEX FALLS OFF AS 1/r. Sampled at r and 2r on the mid-plane, where
    // the axial weight is identical, so the only thing that differs is the
    // radius. The radial weight (1 - r^2/R^2) is divided out by comparing
    // against the closed form rather than against a bare 2.
    const float r1 = 12.0f, r2 = 24.0f;
    const double s1 = speedAt(r1), s2 = speedAt(r2);
    const double w1 = 1.0 - (double)(r1 * r1) / (double)(kR * kR);
    const double w2 = 1.0 - (double)(r2 * r2) / (double)(kR * kR);
    const double want = (double)(r2 / r1) * (w1 / w2);
    vortRatio = s2 > 0.0 ? s1 / s2 : 0.0;
    if (s2 <= 0.0 || std::abs(vortRatio - want) > 0.06 * want) {
      fail(Format("pass P: the vortex is not a 1/r field. speed(%.0f)/"
                  "speed(%.0f) = %.3f, the Gamma/2*pi*r form wants %.3f",
                  r1, r2, vortRatio, want));
    }
    // AND IT IS TANGENTIAL. A vortex whose velocity points along the radius is
    // a sink wearing a vortex's name, and every consumer would still work.
    Vec3 swirlA{0.0f, 0.0f, 0.0f};
    {
      const Vec3 f =
          CurrentAtCpu(Vec3{(float)kCx + r1, (float)kCy, (float)kCz});
      swirlA = f;
      const double m = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
      tangCos = m > 0.0 ? std::abs((double)f.x / m) : 1.0;   // radial share
      if (m <= 0.0 || tangCos > 0.30)
        fail(Format("pass P: the vortex is not tangential - the radial share "
                    "of its velocity at r=%.0f is %.2f", (double)r1, tangCos));
    }
    // CHIRALITY IS REAL. The same primitive with the opposite swirl must give
    // the opposite tangential direction, or every drain in the world spins the
    // same way whatever the hash says.
    {
      CurrentPrims().Clear();
      CurrentPrim w = v;
      w.swirlQ = -(1 << 16);
      CurrentPrims().Spawn(w);
      CurrentPrims().Tick(64);
      const Vec3 b =
          CurrentAtCpu(Vec3{(float)kCx + r1, (float)kCy, (float)kCz});
      if (!(swirlA.z * b.z < 0.0f))
        fail("pass P: reversing swirl did not reverse the tangential flow - "
             "chirality is not reaching the field");
    }

    // THE SINK FALLS OFF AS 1/r^2, AND POINTS IN. Same two radii, same radial
    // weight division, so this compares directly against the vortex number
    // above: 1/r^2 against 1/r is the asymmetry the whole look rests on.
    CurrentPrims().Clear();
    CurrentPrim k;
    k.kind = kCurrentPrimSink;
    k.flags = kCurrentPrimSim;
    k.x = kCx; k.y = kCy; k.z = kCz;
    k.radius = kR;
    k.reach = kR;
    k.decayTicks = kCurrentPrimForever;
    k.spawnTick = 0;
    k.seenTick = 0;
    k.ownerId = 0x7E58u;
    CurrentPrimAim(k, Vec3{0.0f, -1.0f, 0.0f}, 4.0f);
    CurrentPrims().Spawn(k);
    CurrentPrims().Tick(64);
    const double k1 = speedAt(r1), k2 = speedAt(r2);
    const double kwant = (double)(r2 * r2) / (double)(r1 * r1) * (w1 / w2);
    sinkRatio = k2 > 0.0 ? k1 / k2 : 0.0;
    if (k2 <= 0.0 || std::abs(sinkRatio - kwant) > 0.06 * kwant) {
      fail(Format("pass P: the sink is not a 1/r^2 field. speed(%.0f)/"
                  "speed(%.0f) = %.3f, wanted %.3f",
                  (double)r1, (double)r2, sinkRatio, kwant));
    }
    {
      const Vec3 f =
          CurrentAtCpu(Vec3{(float)kCx + r1, (float)kCy, (float)kCz});
      if (!(f.x < 0.0f))
        fail("pass P: the sink does not point INWARD - it is a source");
    }
    // ZERO OUTSIDE THE UNION AABB. Not "small": the reject is an early-out and
    // a field that leaks past its own declared box would make every consumer's
    // cost unbounded and the arrow overlay a lie.
    {
      const Vec3 f = CurrentAtCpu(
          Vec3{(float)(kCx + kR + 8), (float)kCy, (float)kCz});
      if (f.x != 0.0f || f.y != 0.0f || f.z != 0.0f)
        fail("pass P: the field is non-zero outside its own union AABB");
    }
    // GAMMA DECAYS WHEN FLOW STOPS. Component 8 names the alternative outright
    // - a funnel standing open in still water - so this asserts the envelope
    // reaches exactly zero and that Tick() then drops the primitive entirely.
    {
      CurrentPrims().Clear();
      CurrentPrim d = v;
      d.decayTicks = 30;
      d.spawnTick = 0;
      d.seenTick = 0;
      CurrentPrims().Spawn(d);
      CurrentPrims().Tick(29);
      const Vec3 mid =
          CurrentAtCpu(Vec3{(float)kCx + r1, (float)kCy, (float)kCz});
      const double mm = std::sqrt(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z);
      CurrentPrims().Tick(30);
      const Vec3 dead =
          CurrentAtCpu(Vec3{(float)kCx + r1, (float)kCy, (float)kCz});
      if (!(mm > 0.0))
        fail("pass P: the vortex was already dead one tick before its decay "
             "window closed - the envelope is not a ramp");
      if (CurrentPrims().Count() != 0 || dead.x != 0.0f || dead.z != 0.0f)
        fail(Format("pass P: a vortex survived its decay window (%u primitives "
                    "still live) - a funnel would stand open in still water",
                    CurrentPrims().Count()));
    }
    CurrentPrims().Clear();
  }

  // ------------------------------------------------------------------ pass S
  // THE SIM ARM, three arms. An identical pour into an identical stone basin
  // with an identical whirlpool standing in it; only sim.currentMode differs.
  uint32_t hOff = 0, hOn = 0, hOff2 = 0;
  uint32_t pouredParts = 0;
  {
    uint32_t waterId = 0;
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (waterId == 0) {
      detail = "no 'water' material";
      return Status::Fail;
    }
    const IVec3 o = world.WindowOrigin();
    const int px = o.x * (int)kChunk + 100, pz = o.z * (int)kChunk + 100;
    const int py = World::TerrainHeight(px, pz, kDefaultSeed) + 4;
    const int Rb = 5, Hb = 12;

    auto arm = [&](int mode) {
      Tuning a = base;
      a.sim.currentMode = mode;
      // The seeders must not add anything of their own, or the three arms
      // would differ by which streams the window happened to hold.
      a.sim.currentStreamScale = 0.0f;
      SetCurrentTuning(a);
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      c.ctx.WaitIdle();

      CurrentPrims().Clear();
      CurrentPrim v;
      v.kind = kCurrentPrimVortex;
      v.flags = kCurrentPrimSim;
      v.x = px; v.y = py + 4; v.z = pz;
      v.radius = 24;
      v.reach = 24;
      v.swirlQ = 1 << 16;
      v.decayTicks = kCurrentPrimForever;
      v.spawnTick = 0;
      v.seenTick = 0;
      v.ownerId = 0x7E59u;
      CurrentPrimAim(v, Vec3{0.0f, -1.0f, 0.0f}, 8.0f);
      CurrentPrims().Spawn(v);

      std::vector<CellOp> basin;
      for (int z = -Rb - 1; z <= Rb + 1; z++)
        for (int x = -Rb - 1; x <= Rb + 1; x++) {
          basin.push_back({World::SlotCellIndex({px + x, py - 1, pz + z}),
                           (uint32_t)kMatStone});
          const bool rim = (x < -Rb || x > Rb || z < -Rb || z > Rb);
          for (int y = 0; y < Hb; y++)
            basin.push_back({World::SlotCellIndex({px + x, py + y, pz + z}),
                             rim ? (uint32_t)kMatStone : 0u});
        }
      // A deterministic pour: a pure function of the index, so all three arms
      // see byte-identical ops (the twice-run comparison's precondition).
      std::vector<FluidSpawnOp> pour;
      for (int cz = -3; cz < 3; cz++)
        for (int cy = 0; cy < 3; cy++)
          for (int cx = -3; cx < 3; cx++)
            for (int s = 0; s < 4; s++) {
              const uint32_t h =
                  ((uint32_t)pour.size() * 6271u + 12345u) * 747796405u +
                  2891336453u;
              FluidSpawnOp op{};
              op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                      (int32_t)(h % 8192u) - 4096;
              op.py = ((py + 6 + cy) << 16) + 32768;
              op.pz = ((pz + cz) << 16) + ((s & 2) ? 49152 : 16384) +
                      (int32_t)((h >> 13) % 8192u) - 4096;
              op.mat = waterId;
              pour.push_back(op);
            }
      pouredParts = (uint32_t)pour.size();
      uint32_t live = 0;
      for (uint32_t i = 0, k = 40000; i < 70; i++, k++) {
        std::vector<FluidSpawnOp> fs;
        if (i == 1) fs = pour;
        SubmitTick(c.ctx, c.world, c.sim, k, kDefaultSeed, {}, {},
                   i == 0 ? basin : std::vector<CellOp>{}, false,
                   world.WindowOrigin(), false, false, {}, 0, fs, live);
        live += (uint32_t)fs.size();
        c.ctx.ProcessEvents();
      }
      return HashWorldNow(c.ctx, world, c.sim, kDefaultSeed);
    };
    hOff = arm(0);
    hOn = arm(1);
    hOff2 = arm(0);
    if (hOff != hOff2) {
      fail(Format("pass S is not measuring the off switch: the SAME mode-0 "
                  "script hashed %08x the first time and %08x the third, so "
                  "the fixture carries state between arms (mode 1 hashed %08x "
                  "in between). Fix the fixture, not the feature",
                  hOff, hOff2, hOn));
    } else if (hOff == hOn) {
      fail(Format("sim.currentMode does not reach the kernel: mode 0 and mode "
                  "1 both hash %08x over a %u-particle pour standing inside a "
                  "vortex. The off switch is vacuous and so is any claim about "
                  "it", hOff, pouredParts));
    }
  }

  // Leave the world and the tuning the way the ordering in selftest.h assumes.
  CurrentPrims().Clear();
  SetCurrentTuning(base);
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "profiles: vortex 1/r ratio %.2f, sink 1/r^2 ratio %.2f, radial share of "
      "the swirl %.2f; sim arm mode0/mode1/mode0 = %08x / %08x / %08x over a "
      "%u-particle pour",
      vortRatio, sinkRatio, tangCos, hOff, hOn, hOff2, pouredParts);
  if (!fails.empty()) {
    std::string all;
    for (size_t i = 0; i < fails.size(); i++) {
      if (i) all += "; ";
      all += fails[i];
    }
    detail = all + " | " + detail;
    return Status::Fail;
  }
  return Status::Pass;
}

const std::vector<Gate>& WaterGates() {
  static const std::vector<Gate> g = {
      // Depends on `terrain`: that gate is what establishes pristine worldgen at
      // the origin and asserts the CPU height mirror against the GPU's voxels,
      // which is the property this gate's whole analytic basin registry rests
      // on. Running standalone without it would test a curve against a world
      // nobody had checked.
      {"waterbody", "sim", {"terrain"}, false, GateWaterBody},
      // The current field (M4 component 8). Depends on nothing another
      // gate leaves behind: pass P is pure arithmetic and pass S
      // rebuilds the world itself for each of its three arms.
      {"current", "sim", {}, false, GateCurrent},
  };
  return g;
}

}  // namespace selftest
