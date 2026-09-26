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
//   R — RELEVEL (W1, docs/PLAN_water_relevel.md §3.10). A 9x9x6 crater bored
//       into the lake's own floor that goes NOWHERE: nothing drains, the water
//       above falls in, and what is left is a surface DEPRESSION with water
//       under every cell of it — the one state §1 proves the reach-1 CA cannot
//       undo, because its equalize branch fires at 2 eighths and a ramp of 1
//       eighth per 2 cells is a stable fixed point. Asserts the free-surface
//       spread IN EIGHTHS (whole voxels would read flat while a forty-eighth
//       cone stands), the §3.5 identity with the credit term, that the body
//       goes back to sleep, and that the relevel does not feed the excite
//       detector.
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
#include <cstdlib>
#include <string>
#include <vector>

#include "sim/waterbody.h"
#include "sim/currentprim.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// The harness lake's basin id: the harness map's `harness_lake` water site
// (sim/worlddefaults.h), which the registry names by its site index. 0 = the
// loaded map has no such site, which Find/Basin answer with null.
uint32_t LakeId() {
  const int i = World::WaterSiteIndex(kHarnessLakeSite);
  return i >= 0 ? WaterSiteBasinId(i) : 0u;
}

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
  // W1: the same spread in EIGHTHS (8*y + fullness), which is the unit the
  // relevel works in and the only one that can see the thing it fixes. A cone
  // the CA has flattened to its own fixed point — one eighth per two cells —
  // is ZERO voxels deep over any two adjacent columns and forty eighths deep
  // across the lake, so a spread measured in whole Y says "flat" about exactly
  // the surface this whole package exists to flatten.
  int surfaceMinE = 0;
  int surfaceMaxE = 0;
  // THE TOPMOST free surface per column, which is a DIFFERENT SET from the one
  // above and is the one the relevel works on. `surfaceCells` counts every
  // water cell with nothing of ours over it, so a column holding water in a
  // sealed pocket UNDER an air gap contributes twice — and the deeper of the
  // two is not a lake surface at all, it is a puddle in a hole. Measured on the
  // first pass R: a 9x9x6 crater left 45 such pockets and the spread read 190
  // eighths (24 voxels) while the lake's actual surface was flat, which is a
  // gate accusing a kernel of doing nothing when doing nothing was right.
  // `wbSurface` walks DOWN from level+1 and takes the FIRST hit; so does this.
  uint32_t topCells = 0;
  int topMinE = 0;
  int topMaxE = 0;
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
// H1DIAG instrumentation (permanent, see the block in pass H): `noDisc`
// drops the disc filter so a sweep can answer "is any of the missing water
// simply OUTSIDE the lake's disc and therefore invisible to this sweep?" — the
// chamber is a 29x29 square around the lake centre and its corners sit at
// d2 = 2*14^2 = 392, so a small enough basin would clip them.
VoxelTruth SweepBasin(Ctx& c, const WaterBasin& b, const WaterBodyDesc& d,
                      uint32_t matId, int yLoOverride = 0,
                      int yHiOverride = -1, bool noDisc = false) {
  VoxelTruth t;
  World& world = c.world;
  std::vector<uint32_t> chunk(kChunkVol);
  t.surfaceMinY = 1 << 30;
  t.surfaceMaxY = -(1 << 30);
  t.surfaceMinE = 1 << 30;
  t.surfaceMaxE = -(1 << 30);
  t.topMinE = 1 << 30;
  t.topMaxE = -(1 << 30);

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
              if (!noDisc && dx * dx + dz * dz > b.discD2Max) continue;
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
          const int e8 = y * 8 + (int)e;
          t.surfaceMinE = std::min(t.surfaceMinE, e8);
          t.surfaceMaxE = std::max(t.surfaceMaxE, e8);
        }
      }
    }
  }
  // THE TOPMOST surface, one per column, walking DOWN — `wbSurface`'s own walk,
  // bounded the way `wbBandFloor` bounds it: never below the basin floor, so a
  // pocket the shader cannot see is not a column the gate judges it on.
  const int yTopLo = std::max(y0, b.floorY + 1);
  for (int z = d.lo.z; z <= d.hi.z; z++) {
    for (int x = d.lo.x; x <= d.hi.x; x++) {
      for (int y = y1; y >= yTopLo; y--) {
        const uint8_t e = at(x, y, z);
        if (e == 0) continue;
        if (at(x, y + 1, z) != 0) break;   // our own liquid above: not a surface
        t.topCells++;
        const int e8 = y * 8 + (int)e;
        t.topMinE = std::min(t.topMinE, e8);
        t.topMaxE = std::max(t.topMaxE, e8);
        break;
      }
    }
  }
  if (t.surfaceCells == 0) {
    t.surfaceMinY = 0; t.surfaceMaxY = 0;
    t.surfaceMinE = 0; t.surfaceMaxE = 0;
  }
  if (t.topCells == 0) { t.topMinE = 0; t.topMaxE = 0; }
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
  // W1 (docs/PLAN_water_relevel.md §3.2/§3.3): the relevel block. POSITIONAL,
  // like everything above it — a name dropped here shifts every one after it,
  // which `check_invariants.py`'s `waterledger` check is what catches.
  WBS_RVCOUNT_W, WBS_RVSUM_W, WBS_RVMEAN_W, WBS_RVTAKECUT_W, WBS_RVTAKEFRAC_W,
  WBS_RVGIVECUT_W, WBS_RVGIVEFRAC_W, WBS_RVGIVEN_W, WBS_RVTAKEN_W,
  WBS_RVCREDIT_W, WBS_RVCAPPED_W, WBS_RVBASE_W,
  // The CUMULATIVE totals. RVGIVEN/RVTAKEN are a per-TICK report the ledger
  // zeroes as it banks them, so reading them at the end of a window says
  // nothing about the window — measured once as "given 0 / taken 0" over
  // ninety ticks in which the relevel had genuinely been working.
  WBS_RVGIVENT_W, WBS_RVTAKENT_W,
  // W-D (§8.4): the free-surface CELL COUNT the adoption reduce measured, and
  // the only thing discovery's size gate is allowed to read. Distinct from
  // WBS_AREA, which at adoption is the CPU's ANALYTIC seed — for a probe disc
  // that number is the area of a cylinder the CPU drew around some evidence,
  // and gating adoption on it would be the candidate's own guess deciding
  // whether the candidate is real.
  WBS_RAREA_W,
  // W2 (§4.3): the surface-momentum SLEEP. The pipes themselves are per COLUMN
  // and live in `world.waterFlux`; these four are the per-body half — this
  // tick's max and sum of |q| (Q8), the consecutive-calm counter, and the
  // published flux-asleep flag pass S asserts on.
  WBS_WVMAX_W, WBS_WVSUM_W, WBS_WVCALM_W, WBS_WVASLEEP_W,
};
// M5 — the SWEEP block's word map, which lives past the end of the ledger in
// the same buffer (world.h's kWaterCurveBase). Must match the SW_* block in
// assets/shaders/common.wgsl.
enum : uint32_t {
  SW_FLOORY = 0, SW_SPANY, SW_TRUNC, SW_SPILLY, SW_SPLITY, SW_COMPS, SW_MAPY,
  SW_MAPGEN, SW_SPILLYN, SW_SPLITYN,
};
constexpr int32_t kSplitNone = -0x40000000;
// ---- the FA_* words this gate reads ---------------------------------------
// C++ has no view of the shaders' `const FA_* : u32`, so the subscripts are
// spelled out once here rather than as bare numbers at the eleven use sites.
// The authoritative occupancy ledger for the 40-word map is the comment block
// above `FA_LIVE` in assets/shaders/common.wgsl; these must agree with it.
//
// [34..39] are THE SEAM'S MASS BOOKS: cumulative, deliberately skipped by the
// per-tick clear at the head of sim_fluid_seam.wgsl, so a window's total is
// (after - before). Everything else in the map is one tick.
constexpr uint32_t kFaLive = 7;         // FA_LIVE
constexpr uint32_t kFaSpawnDead = 29;   // FA_SPAWNDEAD
constexpr uint32_t kFaKillHard = 34;    // FA_KILLHARD    (sim_fluid.wgsl)
constexpr uint32_t kFaSettleKill = 35;  // FA_SETTLEKILL  (sim_fluid_seam.wgsl)
constexpr uint32_t kFaExcitedCum = 36;  // FA_EXCITEDCUM  (sim_fluid_seam.wgsl)
constexpr uint32_t kFaCalmSubm = 37;    // FA_CALMSUBM    (sim_fluid.wgsl)
constexpr uint32_t kFaSpawnLive = 38;   // FA_SPAWNLIVE   (sim_fluid_seam.wgsl)
constexpr uint32_t kFaSetWrote = 39;    // FA_SETWROTE    (sim_fluid_seam.wgsl)
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
// THE PASS-R FIXTURE (W1). A 9x9x6 box of the lake's own floor, removed and
// going NOWHERE — stone on every side, so nothing drains and the only thing
// that changes is the shape of the surface. That is the disturbance the
// relevel exists for and the one the CA provably cannot undo; a shaft into a
// chamber would measure the drain again, which pass H already does.
// SIZED AGAINST THE CA'S OWN FIXED POINT, not against the plan's 9x9x6 — and
// the difference is a measurement, not taste. A crater of half-width r and
// depth D voxels spreads into a cone the CA stops flattening at 1 eighth per 2
// cells, i.e. radius R = (48*D*r^2)^(1/3) and a centre depth of R/2 eighths.
// At the plan's 4 and 6 that is a 9-eighth residue in theory and NOTHING in
// practice: measured, the CA and the MPM closed it inside 90 ticks and the
// control arm read the same flat surface as the live one, so the pass was a
// green light about nothing. At 8 and 6 the arithmetic says ~14 eighths, seven
// times the budget, and the control arm measured 66 — the CA had not started.
//
// 16 and 8 was tried and REJECTED, and the reason is worth keeping: a 33x33x8
// pit is violent enough that the MPM and the evaporation rule ate 54,091
// eighths of the lake WITH THE FEATURE OFF and left 64 chunks awake. A fixture
// whose control arm fails the conservation and sleep budgets cannot attribute
// anything to the rule under test.
constexpr int kCraterHalf = 8;    // half-extent, so 17x17
constexpr int kCraterDepth = 6;   // voxels of floor removed
enum : int32_t {
  WB_CANDIDATE = 0, WB_MEASURING = 1, WB_ADOPTED = 2, WB_RELEASING = 3,
  // W-D: the sticky refusal. A discovered probe the GPU measured and did not
  // believe in — four ledger loads a tick until new evidence arrives.
  WB_REFUSED = 4,
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
    case WB_REFUSED: return "refused";
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
  // A PROGRESS MARKER PER PASS, on stderr and behind an env var. This gate is
  // ten passes long and prints ONE line, at the end — so a pass that dies hard
  // leaves a log that names no pass at all, and every candidate has to be
  // eliminated by a rebuild. `SANDVOX_SELFTEST_TRACE=1` costs nothing when
  // unset and answers the question in one run when it is. stderr rather than
  // stdout so it cannot reorder against the gate's own reported line.
  const bool trace = std::getenv("SANDVOX_SELFTEST_TRACE") != nullptr;
  auto mark = [&](const char* what) {
    if (trace) { std::fprintf(stderr, "waterbody: %s\n", what); }
  };

  mark("pass K");
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
    // The pre-P-F tarn's numbers (depth 26, rim 3, berm 5), as literals: this
    // arm is the parabola's own round-trip, kept beside the profiled bowl the
    // world makes now so the inversion's off-by-one is still pinned somewhere.
    b.centreDepth = 26;
    b.rimDepth = 3;
    b.floorY = b.surfY - b.centreDepth;
    b.spillY = b.surfY + 5;
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

  mark("pass K2");
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
      // P-F: the geometry is the tarn's own preset's, carried in the disc,
      // and the curve inverts the mirrored bowl (World::BowlDepth) by
      // bisection -- the Profiled kind the registry builds for every pond.
      b.centreDepth = found.depth;
      b.rimDepth = found.rimDepth;
      b.floorY = found.surf - b.centreDepth;
      b.spillY = found.surf + found.bermH;
      b.kind = WaterBasinKind::Profiled;
      b.preset = found.preset;
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

  mark("setup");
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
  const WaterBasin* lake = wb.Basin(LakeId());
  const WaterBodyDesc* desc = wb.Find(LakeId());
  if (!lake || !desc) {
    SetCurrentTuning(base);
    SetHarnessSnapshotDrain(hadDrain);
    detail = "the harness lake (water site harness_lake) is not in the registry";
    std::printf("waterbody: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  mark("pass G");
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
    // The bound lived in `sim.waterBodySpreadExit` until 2026-09-24, a knob
    // nothing but this gate read: spread-based adopt/release was planned and
    // never built, so the number is a gate threshold and lives in baseline.
    const int spread = v.surfaceMaxY - v.surfaceMinY;
    const int spreadMax = (int)BaselineNumber("waterbodySpreadMax", 4);
    if (spread > spreadMax)
      fail(Format("%s: a settled surface spans %d voxels, over the bound of "
                  "%d — the level model does not describe it",
                  what, spread, spreadMax));
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

  mark("pass E");
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

  mark("pass C");
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
    WaterBodyState last = WaterBodies().Find(LakeId())
                              ? WaterBodies().Find(LakeId())->state
                              : WaterBodyState::Candidate;
    for (uint32_t i = 0; i < 200; i++) {
      tick = RunQuietTicks(c, tick, 1);
      const WaterBodyDesc* d = WaterBodies().Find(LakeId());
      if (!d) { fail("the harness lake vanished from the registry mid-run"); break; }
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
  const WaterBodyDesc* fin = WaterBodies().Find(LakeId());
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


  mark("pass A+X");
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


  mark("pass H");
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
    // W3, and it is the SAME DISCIPLINE passes R/N/S state in their own words:
    // this pass measures whether the DISCHARGE LAW is mass-exact, so the
    // fixture must contain nothing else that moves settled water. The MPM seam
    // is already disarmed per arm above; the surface-momentum layer became a
    // second mover the day `sim.waveMode` shipped at 1, and an arm that
    // inherited it would be reporting the sum of two rules under one identity.
    //
    // MEASURED, the run that flipped the default: H1's ledger-only identity
    // went from +0 to +1225 eighths against a 256-eighth strict slack with
    // nothing else changed. The wave's OWN conservation is asserted exactly, on
    // its own fixture, by passes S and T — it is not going untested here, it is
    // going untested HERE.
    ht.sim.waveMode = 0;
    SetCurrentTuning(ht);
    tick = RunQuietTicks(c, tick, 130);

    const WaterBodyDesc* hd = WaterBodies().Find(LakeId());
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
    // ===== H1DIAG: the residual's decomposition, permanently ================
    // A bare residual is not a measurement (CLAUDE.md rule 6). These sweeps and
    // the seam's cumulative mass books are what turned a flat "-73,287 eighths"
    // into "g2p is deleting particles that end up inside submerged liquid" in
    // four runs instead of fourteen, and they print on every run — PASS or
    // FAIL — because the run where you wish you had them is the one where the
    // number has already moved.
    //
    // Three extra sweeps split the box into the two halves the
    // identity is really about, because the residual algebraically reduces to
    //
    //     err = (particles actually spawned) - (eighths the ledger debited)
    //           - (anything else that moved water out of the box)
    //
    // and those two families need different owners. The LAKE half above the
    // chamber roof should fall by exactly `shaved` = drained - debit; the
    // CHAMBER half should rise by exactly what landed. `noDisc` is the third:
    // the sweep clips to the lake's disc, and the chamber is a SQUARE.
    const int lakeLo = chTop + 1;    // shaft mouth upward: the lake's own water
    const VoxelTruth h0L =
        SweepBasin(c, lakeGeo, lakeDesc, matId, lakeLo, boxHi);
    const VoxelTruth h0N =
        SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi, true);
    uint32_t fa0[kFluidArgsWords] = {};
    ReadFluidArgsSync(c.ctx, world, fa0);
    // =======================================================================
    uint32_t pfBefore[4] = {0, 0, 0, 0};
    ReadPageFaultsSync(c.ctx, world, pfBefore);

    SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, punch, false,
               c.world.WindowOrigin(), true, false);
    c.ctx.ProcessEvents();
    tick++;

    uint64_t seen = 0, cand = 0;
    uint32_t samples = 0;
    tick = RunQuietTicks(c, tick, kDrainWindow, &seen, &cand, &samples);
    // ===== H1DIAG: the mid-window split =====================================
    // A MID-WINDOW sample, taken at the end of the draining ticks and before
    // the settle. It separates "the loss accrues WITH the drain" (a per-eighth
    // rule: a spawn that never happened, a settle that rounds down) from "the
    // loss accrues while the pool SETTLES" (evaporation, the seam churning a
    // surface). All reads, no writes, so the world the settle ticks see is the
    // one they would have seen anyway.
    const VoxelTruth hM = SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi);
    const VoxelTruth hML =
        SweepBasin(c, lakeGeo, lakeDesc, matId, lakeLo, boxHi);
    const LedgerView lvM = ReadLedger(c);
    uint32_t faM[kFluidArgsWords] = {};
    ReadFluidArgsSync(c.ctx, world, faM);
    const int64_t inFlightM = (int64_t)faM[kFaLive] -
                              (int64_t)std::min(faM[kFaSpawnDead], faM[kFaLive]);
    // =======================================================================
    // SETTLE, with the hole still open: the jet is still in flight and the
    // ledger still owes a debit the shave has not taken. Measuring before this
    // would charge the difference to the feature.
    tick = RunQuietTicks(c, tick, 90);

    const VoxelTruth h1 = SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi);
    // ===== H1DIAG: the same three sweeps, after ============================
    const VoxelTruth h1L =
        SweepBasin(c, lakeGeo, lakeDesc, matId, lakeLo, boxHi);
    const VoxelTruth h1N =
        SweepBasin(c, lakeGeo, lakeDesc, matId, boxLo, boxHi, true);
    // =======================================================================
    const LedgerView lv = ReadLedger(c);
    uint32_t fa[kFluidArgsWords] = {};   // ReadFluidArgsSync fills the whole map
    ReadFluidArgsSync(c.ctx, world, fa);
    // ONE EIGHTH PER PARTICLE (every seam-born particle carries fullness 1),
    // minus the dead tail of this tick's reserved discharge block.
    const int64_t inFlight =
        (int64_t)fa[kFaLive] -
        (int64_t)std::min(fa[kFaSpawnDead], fa[kFaLive]);
    const int64_t debitNow = lv.At(hSlot, WBS_DEBIT);
    const int64_t drainedNow = lv.At(hSlot, WBS_DRAINED);
    // THE BANKED KILL IS A TERM, NOT A SLACK. g2p deletes a particle that finds
    // itself inside HARD solid — rock closed over it, or the window moved — and
    // there is no legal cell to hand its eighths back to, so that mass really
    // does leave the world. It is the only unaccounted deletion left in the
    // engine and FA_KILLHARD banks it (sim_fluid.wgsl), which lets this
    // identity stay STRICT: the alternative is widening the slack until the
    // kill fits, and a slack wide enough to hide a legitimate deletion is wide
    // enough to hide the next leak. Measured in this fixture the jet never
    // touches rock, so the term is 0 and costs nothing to carry — which is
    // exactly the condition under which you should carry it.
    const int64_t killedHard =
        (int64_t)fa[kFaKillHard] - (int64_t)fa0[kFaKillHard];
    const int64_t err = (int64_t)h1.eighths + inFlight + killedHard - debitNow -
                        (int64_t)h0.eighths;
    uint32_t pfAfter[4] = {0, 0, 0, 0};
    ReadPageFaultsSync(c.ctx, world, pfAfter);

    // ===== H1DIAG: the residual, decomposed. PRINTED EVERY RUN ==============
    //
    // THE DECOMPOSITION. Let E = cumulative emitted (WBS_DRAINED), D = the
    // outstanding debit, S = cumulative eighths the shave actually removed.
    // The ledger keeps D = E - S by construction, so S = E - D. Then over the
    // two halves of the box:
    //
    //   LAKE  (above the chamber roof): should fall by exactly S.
    //         lakeLeak = (h1L - h0L) + S
    //   CHAMB (the rest of the box):    should gain what landed, and the rest
    //         is still particles (or banked in FA_KILLHARD).
    //         chLeak   = (h1 - h1L) - (h0 - h0L) + inFlight + killedHard - E
    //
    //   err == lakeLeak + chLeak, identically. A negative lakeLeak means the
    //   LAKE lost water nobody debited (evaporation, the seam, a shave that
    //   over-took); a negative chLeak means the jet's eighths never arrived (a
    //   spawn refused, a settle that rounds down, a particle that died).
    {
      const int64_t S = drainedNow - debitNow;
      const int64_t lake0 = (int64_t)h0L.eighths, lake1 = (int64_t)h1L.eighths;
      const int64_t ch0 = (int64_t)h0.eighths - lake0;
      const int64_t ch1 = (int64_t)h1.eighths - lake1;
      const int64_t lakeLeak = (lake1 - lake0) + S;
      const int64_t chLeak =
          (ch1 - ch0) + inFlight + killedHard - drainedNow;
      // THE SEAM'S OWN BOOKS, independent of the ledger: in == out + liveDelta
      // or mass appeared or vanished inside the MPM seam itself. FA_CALMSUBM is
      // on NEITHER side — a calmed particle is still alive and still counted in
      // liveDelta, so adding it would double-count. That it is not a term here
      // is the whole content of the H1 fix: it used to be a KILL, which put it
      // on `out` while its mass went nowhere.
      const int64_t booksIn0 =
          (int64_t)fa[kFaSpawnLive] - (int64_t)fa0[kFaSpawnLive];
      const int64_t booksIn1 =
          (int64_t)fa[kFaExcitedCum] - (int64_t)fa0[kFaExcitedCum];
      const int64_t booksOut1 =
          (int64_t)fa[kFaSettleKill] - (int64_t)fa0[kFaSettleKill];
      const int64_t liveDelta = (int64_t)fa[kFaLive] - (int64_t)fa0[kFaLive];
      const int64_t lakeM = (int64_t)hML.eighths;
      const int64_t SM = lvM.At(hSlot, WBS_DRAINED) - lvM.At(hSlot, WBS_DEBIT);
      const int64_t lakeLeakM = (lakeM - lake0) + SM;
      const int64_t chLeakM = ((int64_t)hM.eighths - lakeM - ch0) + inFlightM -
                              lvM.At(hSlot, WBS_DRAINED);
      std::fprintf(
          stderr,
          "H1DIAG[%s] err %+lld = lakeLeak %+lld + chLeak %+lld\n"
          "  lake  %lld -> %lld (%+lld), shaved S=%lld (drained %lld - debit "
          "%lld)\n"
          "  chamb %lld -> %lld (%+lld), inFlight %lld (live %u dead %u), "
          "banked hard-solid kill %lld\n"
          "  MID-WINDOW (end of drain, before settle): err %+lld = lakeLeak "
          "%+lld + chLeak %+lld ; drained %lld debit %lld inFlight %lld\n"
          "  disc filter: box %llu -> %llu vs NO-DISC %llu -> %llu (d0 %+lld d1 "
          "%+lld)\n"
          "  ledger: volume %d area %d seen %d capped %d level %d floorY %d "
          "surfY %d discD2Max %lld holeArea %d jetv %d\n"
          "  relevel: credit %d givenT %d takenT %d rvcapped %d\n"
          "  faults: %u %u 0x%08x 0x%08x\n"
          "  fluidArgs delta: live %+lld dead %+lld emitted %+lld settled %+lld "
          "excited %+lld refused %+lld consumed %+lld clamped %+lld "
          "setrefused %+lld setunstable %+lld spawndead %+lld\n"
          "  SEAM BOOKS (cumulative over the window, FA_* words 34..39):\n"
          "    in : FA_SPAWNLIVE %lld + FA_EXCITEDCUM %lld = %lld\n"
          "    out: FA_KILLHARD %lld + FA_SETTLEKILL %lld = %lld\n"
          "    FA_SETWROTE %lld net eighths into voxels; FA_CALMSUBM %lld "
          "calm events (NOT a transfer, on neither side)\n"
          "    in - out - liveDelta = %lld  (0 means the seam's books close)\n",
          arm.name, (long long)err, (long long)lakeLeak, (long long)chLeak,
          (long long)lake0, (long long)lake1, (long long)(lake1 - lake0),
          (long long)S, (long long)drainedNow, (long long)debitNow,
          (long long)ch0, (long long)ch1, (long long)(ch1 - ch0),
          (long long)inFlight, fa[kFaLive], fa[kFaSpawnDead],
          (long long)killedHard,
          (long long)(lakeLeakM + chLeakM), (long long)lakeLeakM,
          (long long)chLeakM, (long long)lvM.At(hSlot, WBS_DRAINED),
          (long long)lvM.At(hSlot, WBS_DEBIT), (long long)inFlightM,
          (unsigned long long)h0.eighths, (unsigned long long)h1.eighths,
          (unsigned long long)h0N.eighths, (unsigned long long)h1N.eighths,
          (long long)((int64_t)h0N.eighths - (int64_t)h0.eighths),
          (long long)((int64_t)h1N.eighths - (int64_t)h1.eighths),
          lv.At(hSlot, WBS_VOLUME), lv.At(hSlot, WBS_AREA),
          lv.At(hSlot, WBS_SEEN), lv.At(hSlot, WBS_CAPPED),
          lv.At(hSlot, WBS_LEVEL), lakeGeo.floorY, lakeGeo.surfY,
          (long long)lakeGeo.discD2Max, lv.At(hSlot, WBS_HOLEAREA),
          lv.At(hSlot, WBS_JETV), lv.At(hSlot, WBS_RVCREDIT_W),
          lv.At(hSlot, WBS_RVGIVENT_W), lv.At(hSlot, WBS_RVTAKENT_W),
          lv.At(hSlot, WBS_RVCAPPED_W), pfAfter[0] - pfBefore[0],
          pfAfter[1] - pfBefore[1], pfAfter[2], pfAfter[3],
          (long long)fa[kFaLive] - (long long)fa0[kFaLive],
          (long long)fa[8] - (long long)fa0[8],
          (long long)fa[9] - (long long)fa0[9],
          (long long)fa[10] - (long long)fa0[10],
          (long long)fa[11] - (long long)fa0[11],
          (long long)fa[12] - (long long)fa0[12],
          (long long)fa[16] - (long long)fa0[16],
          (long long)fa[18] - (long long)fa0[18],
          (long long)fa[25] - (long long)fa0[25],
          (long long)fa[26] - (long long)fa0[26],
          (long long)fa[kFaSpawnDead] - (long long)fa0[kFaSpawnDead],
          (long long)booksIn0, (long long)booksIn1,
          (long long)(booksIn0 + booksIn1), (long long)killedHard,
          (long long)booksOut1, (long long)(killedHard + booksOut1),
          (long long)fa[kFaSetWrote] - (long long)fa0[kFaSetWrote],
          (long long)fa[kFaCalmSubm] - (long long)fa0[kFaCalmSubm],
          (long long)(booksIn0 + booksIn1 - killedHard - booksOut1 -
                      liveDelta));
      std::fflush(stderr);
    }
    // ===== end H1DIAG ======================================================

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
          lv.At(hSlot, WBS_JETV), (long long)inFlight, fa[kFaLive],
          fa[kFaSpawnDead]);
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
    // WHAT THE SLACK IS NOT FOR: a deletion the engine knows it performed. The
    // residual sat at -73,287 for a while because g2p was annihilating
    // particles that ended up inside submerged liquid; the fix was to stop
    // doing that (they go calm now), and the one deletion that REMAINS legal
    // — a particle inside hard solid — is banked in FA_KILLHARD and added
    // back above rather than absorbed here. A slack wide enough to cover a
    // known deletion is wide enough to cover the next unknown one.
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
          "banked hard-solid kill %lld, ledger drained %lld, outstanding debit "
          "%lld, capped %d, level %d, hole area %d, %u page faults — the "
          "H1DIAG lines above decompose this into lake / chamber and the seam's "
          "mass books",
          arm.name, lakeDesc.basinId, hSlot, (long long)err,
          (unsigned long long)h0.eighths, (unsigned long long)h1.eighths,
          (long long)((int64_t)h1.eighths - (int64_t)h0.eighths),
          (long long)inFlight, fa[kFaLive], fa[kFaSpawnDead],
          (long long)killedHard, (long long)drainedNow,
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


  mark("pass R");
  // ========================================================== pass R (W1)
  //
  // RELEVEL. docs/PLAN_water_relevel.md §3.10, and it is the acceptance of the
  // owner complaint the whole package exists for: blow a hole under a pond and
  // the dent it leaves takes ten minutes to go away.
  //
  // THE FIXTURE IS DELIBERATELY NOT A DRAIN. A crater is bored into the lake's
  // floor and it goes NOWHERE — sealed stone on every side, and
  // `sim.drainMaxEighthsPerTick` is 0 for this pass so the discharge law cannot
  // fire even if the hole detector sees the transient. Nothing leaves the lake;
  // the water above falls in, and what is left is a surface DEPRESSION with
  // water under every cell of it. That is the one state §1 proves the reach-1
  // CA cannot undo, because its equalize branch fires at 2 eighths and a ramp of
  // 1 eighth per 2 cells is a stable fixed point.
  //
  // Setting the drain knob to 0 is also the test of §3.6's decoupling: with no
  // drain armed, the ONLY thing that can declare this body's footprint to the
  // page table is `relevelMax > 0` in the hot latch. Before that change the
  // relevel would write into JITTER sentinels here and the page-fault assertion
  // below would say so.
  //
  // TWO ARMS, AND THE CONTROL IS THE POINT. `sim.waterRelevelMax = 0` is an
  // exact identity: the passes record and move nothing. So arm 1 measures what
  // the CA alone does with the crater and arm 2 measures what the CA plus the
  // relevel does, on the same fixture from the same worldgen at the same ticks.
  // Without the control, "spread 3 eighths" is a bare count that cannot tell a
  // working relevel from a crater the CA would have closed anyway — CLAUDE.md
  // rule 6, and the reason pass A carries its own quiet window.
  //
  // WHAT IS ASSERTED, and each fails differently:
  //
  //   * SPREAD, IN EIGHTHS, over the TOPMOST free surface of each column. Both
  //     halves of that are load-bearing. Whole voxels would read flat while a
  //     forty-eighth cone still stood, which is the bug itself; and counting
  //     every free surface rather than the topmost counts water sealed in a
  //     pocket under an air gap, which is not a lake surface and which
  //     `wbSurface` correctly never looks at.
  //   * THE FEATURE DID SOMETHING: arm 2's spread must beat arm 1's.
  //   * THE IDENTITY of §3.5 with the credit term, on arm 2.
  //   * AWAKE AT REST, and the excite-candidate bound of §3.8.
  //
  // `RVCAPPED` and the cumulative given/taken are RECORDED and not asserted,
  // which is rule 6 built in before it is needed: "the surface did not flatten"
  // is several different bugs and only those numbers tell them apart.
  std::string relevelNote;
  int64_t rvSpreadOff = 0, rvSpreadOn = 0, rvSpread0 = 0;
  int64_t rvErr = 0, rvCredit = 0, rvGiven = 0, rvTaken = 0, rvCapped = 0;
  int64_t rvDrained = 0, rvDebit = 0, rvInFlight = 0;
  int rvFlatTick = -1;
  int32_t rvMean = 0, rvLevel = 0, rvCount = 0;
  uint32_t rvAwake = 0, rvTopCells = 0;
  double rvCandPerTick = 0.0;
  //
  // NOT GUARDED ON `ok`, and that is deliberate rather than sloppy. Passes H and
  // B are, because they build on the world the passes before them left; this one
  // rebuilds the world itself (Reset + worldgen + 130 quiet ticks) and shares
  // nothing with them but the lake's geometry. CLAUDE.md's memory note on the
  // spells gate is the reason to care: a pass that depends on a failing pass is
  // a SILENT SKIP, and it costs exactly when it matters — pass H1 carries an
  // inherited conservation failure on this branch, so a guarded pass R would
  // have shipped never having run once.
  {
    const uint32_t rvTicks =
        (uint32_t)BaselineNumber("waterbodyRelevelTicks", 90.0);
    const double spreadMax = BaselineNumber("waterbodyRelevelSpread", 2.0);
    const uint32_t rvStep =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyRelevelStep", 15.0));
    const uint32_t rvSettle = (uint32_t)std::max(
        1.0, BaselineNumber("waterbodyRelevelSettleTicks", 60.0));
    int64_t armErr[2] = {0, 0};
    int32_t armCount[2] = {0, 0};
    uint32_t armAwake[2] = {0, 0}, armTop[2] = {0, 0};
    double armCand[2] = {0.0, 0.0};
    const int rBoxLo = lakeGeo.floorY - kCraterDepth;
    const int rBoxHi = lakeGeo.surfY;

    // ONE ARM. Returns the topmost-surface spread in eighths, or -1 if the arm
    // could not run at all. Everything else it learned goes into the outer
    // variables, which the second (live) arm is the last to write.
    auto relevelArm = [&](int rate) -> int64_t {
      mark("pass R: arm begin");
      WaterBodies().Reset();
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      Tuning rt = t;
      rt.sim.waterBodyTestDrain = 0;      // the CRATER is the disturbance
      rt.sim.drainMaxEighthsPerTick = 0;  // ...and it is NOT a drain
      // ---- AND THE MPM IS OFF, which is pass H1's discipline reused --------
      //
      // This pass is about a rule that moves SETTLED water between columns, so
      // the fixture must contain nothing else that moves settled water. With
      // the excite seam live, the crater is a particle event: measured, the
      // solver and the evaporation rule between them took 54,091 eighths out
      // of the lake with the relevel DISARMED, left 64 chunks awake past the
      // budget, and put an MPM block in a third of the surface chunks — which
      // the measure then correctly skips, so the mean was computed over 9,613
      // of 14,493 columns. Every number the pass reports was then a statement
      // about the solver. Pass H measures the seam; this measures the rule.
      rt.sim.fluidExciteMode = 0;
      rt.sim.drainExciteRadius = 0;
      rt.sim.fluidSplashRate = 0.0f;
      rt.sim.waterRelevelMax = rate;
      SetCurrentTuning(rt);
      tick = RunQuietTicks(c, tick, 130);

      const WaterBodyDesc* rd = WaterBodies().Find(LakeId());
      if (!rd || rd->gpuSlot >= kWaterBodyCap) {
        fail(Format("pass R (rate %d): the authored lake is not proposed", rate));
        return -1;
      }
      const uint32_t rSlot = rd->gpuSlot;
      {
        const LedgerView lv0 = ReadLedger(c);
        if (lv0.At(rSlot, WBS_STATE) != WB_ADOPTED) {
          fail(Format("pass R (rate %d): the lake is %s, not adopted, before "
                      "the crater",
                      rate, LedgerStateName(lv0.At(rSlot, WBS_STATE))));
          return -1;
        }
      }

      // A NARROW box for the per-sample "is it flat yet" curve. The dent is
      // local; sweeping the whole lake six times to watch it close would cost
      // more gate seconds than the drain does and answer the same question.
      // The assertion at the end is over the WHOLE lake.
      WaterBodyDesc nearDesc = lakeDesc;
      nearDesc.lo.x = std::max(lakeDesc.lo.x, lakeGeo.cx - kCraterHalf - 24);
      nearDesc.hi.x = std::min(lakeDesc.hi.x, lakeGeo.cx + kCraterHalf + 24);
      nearDesc.lo.z = std::max(lakeDesc.lo.z, lakeGeo.cz - kCraterHalf - 24);
      nearDesc.hi.z = std::min(lakeDesc.hi.z, lakeGeo.cz + kCraterHalf + 24);

      const VoxelTruth r0 =
          SweepBasin(c, lakeGeo, lakeDesc, matId, rBoxLo, rBoxHi);
      rvSpread0 = (int64_t)r0.topMaxE - (int64_t)r0.topMinE;
      uint32_t rPf0[4] = {0, 0, 0, 0};
      ReadPageFaultsSync(c.ctx, world, rPf0);

      std::vector<CellOp> crater;
      for (int y = lakeGeo.floorY - kCraterDepth + 1; y <= lakeGeo.floorY; y++)
        for (int z = lakeGeo.cz - kCraterHalf; z <= lakeGeo.cz + kCraterHalf;
             z++)
          for (int x = lakeGeo.cx - kCraterHalf; x <= lakeGeo.cx + kCraterHalf;
               x++)
            crater.push_back({World::SlotCellIndex({x, y, z}), 0u});
      SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, crater,
                 false, c.world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();
      tick++;

      uint64_t rSeen = 0, rCand = 0;
      uint32_t rSamples = 0;
      int flatAt = -1;
      for (uint32_t done = 0; done < rvTicks; done += rvStep) {
        const uint32_t n = std::min(rvStep, rvTicks - done);
        tick = RunQuietTicks(c, tick, n, &rSeen, &rCand, &rSamples);
        const VoxelTruth s =
            SweepBasin(c, lakeGeo, nearDesc, matId, rBoxLo, rBoxHi);
        const int64_t sp = (int64_t)s.topMaxE - (int64_t)s.topMinE;
        if (flatAt < 0 && s.read && s.topCells > 0 && (double)sp <= spreadMax)
          flatAt = (int)(done + n);
      }
      // SETTLE with nothing happening, so the ledger has consumed the last
      // apply's report and the MPM has put its particles back. Pass H takes the
      // same window for the same reason: measured in between, `credit` is short
      // by one tick of moves and `inFlight` is a churning pool.
      tick = RunQuietTicks(c, tick, rvSettle);
      const VoxelTruth r1 =
          SweepBasin(c, lakeGeo, lakeDesc, matId, rBoxLo, rBoxHi);
      const int64_t spread = (int64_t)r1.topMaxE - (int64_t)r1.topMinE;
      const LedgerView lv = ReadLedger(c);
      // kFluidArgsWords, NOT a literal 32. ReadFluidArgsSync writes the WHOLE
      // FA_* map (40 words since the gas package widened it) and a 32-word
      // array is a 32-byte stack overrun that /GS catches in the epilogue —
      // no crash.log, no SEH filter, no output past the last flush. Cost four
      // runs and the SANDVOX_SELFTEST_TRACE markers above to find.
      uint32_t rFa[kFluidArgsWords] = {};
      ReadFluidArgsSync(c.ctx, world, rFa);
      uint32_t rPf1[4] = {0, 0, 0, 0};
      ReadPageFaultsSync(c.ctx, world, rPf1);
      const uint32_t awake = ReadActiveChunksSync(c.ctx, world, c.sim);

      if (!r1.read || r1.topCells == 0) {
        fail(Format("pass R (rate %d): the final sweep found no free surface "
                    "at all", rate));
        return -1;
      }
      if (rPf1[0] != rPf0[0]) {
        fail(Format("pass R (rate %d): %u page faults during the relevel (lost "
                    "word 0x%08x, refusing chunks %u..%u) — a relevel wrote "
                    "into a sentinel chunk, which is what §3.6's hot-latch "
                    "decoupling exists to prevent",
                    rate, rPf1[0] - rPf0[0], rPf1[2],
                    rPf1[1] ? rPf1[1] - 1u : 0u,
                    rPf1[3] ? 0xFFFFFFFFu - rPf1[3] : 0u));
      }

      // BOTH ARMS REPORT THE CONSERVATION SUM, and that is rule 6 rather than
      // completeness for its own sake: this box contains a churning MPM pool
      // and a freshly exposed surface the evaporation rule acts on, so "the
      // relevel lost 113,000 eighths" is only a statement about the relevel if
      // the SAME fixture without it lost something different.
      {
        const int64_t inF =
            (int64_t)rFa[7] - (int64_t)std::min(rFa[29], rFa[7]);
        const int64_t err =
            (int64_t)r1.eighths + inF + lv.At(rSlot, WBS_DRAINED) -
            lv.At(rSlot, WBS_DEBIT) +
            (lv.At(rSlot, WBS_RVCREDIT_W) + lv.At(rSlot, WBS_RVGIVEN_W) -
             lv.At(rSlot, WBS_RVTAKEN_W)) -
            (int64_t)r0.eighths;
        armErr[rate > 0 ? 1 : 0] = err;
        armCount[rate > 0 ? 1 : 0] = lv.At(rSlot, WBS_RVCOUNT_W);
        armAwake[rate > 0 ? 1 : 0] = awake;
        armTop[rate > 0 ? 1 : 0] = r1.topCells;
        armCand[rate > 0 ? 1 : 0] =
            rvTicks > 0 ? (double)rCand / (double)rvTicks : 0.0;
      }
      if (rate > 0) {
        rvFlatTick = flatAt;
        rvCredit = lv.At(rSlot, WBS_RVCREDIT_W);
        rvGiven = lv.At(rSlot, WBS_RVGIVENT_W);
        rvTaken = lv.At(rSlot, WBS_RVTAKENT_W);
        rvCapped = lv.At(rSlot, WBS_RVCAPPED_W);
        rvDrained = lv.At(rSlot, WBS_DRAINED);
        rvDebit = lv.At(rSlot, WBS_DEBIT);
        rvMean = lv.At(rSlot, WBS_RVMEAN_W);
        rvCount = lv.At(rSlot, WBS_RVCOUNT_W);
        rvLevel = lv.At(rSlot, WBS_LEVEL);
        rvTopCells = r1.topCells;
        rvAwake = awake;
        rvInFlight = (int64_t)rFa[7] - (int64_t)std::min(rFa[29], rFa[7]);
        rvCandPerTick =
            rvTicks > 0 ? (double)rCand / (double)rvTicks : 0.0;
        // THE IDENTITY OF §3.5, every term measured and none inferred. The
        // unbanked half of the current tick is folded in with the credit,
        // because the ledger banks it NEXT tick and a sum taken in between is
        // short by one tick of moves.
        rvErr = (int64_t)r1.eighths + rvInFlight + rvDrained - rvDebit +
                (rvCredit + lv.At(rSlot, WBS_RVGIVEN_W) -
                 lv.At(rSlot, WBS_RVTAKEN_W)) -
                (int64_t)r0.eighths;
        const int64_t rSlack =
            (int64_t)BaselineNumber("waterbodyRelevelSlackEighths", 4096.0);
        if (rvErr < -rSlack || rvErr > rSlack) {
          fail(Format(
              "CONSERVATION (pass R): basin %u slot %u is off by %+lld eighths "
              "over %u relevel ticks. box %llu -> %llu (%+lld), in flight "
              "%lld, drained %lld, debit %lld, credit %lld (%lld given / %lld "
              "taken cumulative), capped %lld",
              lakeDesc.basinId, rSlot, (long long)rvErr, rvTicks,
              (unsigned long long)r0.eighths, (unsigned long long)r1.eighths,
              (long long)((int64_t)r1.eighths - (int64_t)r0.eighths),
              (long long)rvInFlight, (long long)rvDrained, (long long)rvDebit,
              (long long)rvCredit, (long long)rvGiven, (long long)rvTaken,
              (long long)rvCapped));
        }
        if ((double)awake > awakeMax) {
          fail(Format("pass R: %u chunks still awake %u ticks after the "
                      "relevel window, over the budget of %.0f — a relevelled "
                      "lake does not settle",
                      awake, rvSettle, awakeMax));
        }
        const double rCandMax =
            BaselineNumber("waterbodyDrainExciteCandPerTickMax", 3000.0);
        if (rvCandPerTick > rCandMax) {
          fail(Format(
              "the relevel feeds the excite detector: %llu candidates over %u "
              "ticks (%.1f/tick) against %llu cells seen, budget %.0f/tick — "
              "plan section 3.8 says a relevel cannot MANUFACTURE the seam's "
              "two-cell step, so this is either the crater wall (correct) or "
              "that paragraph is wrong",
              (unsigned long long)rCand, rvTicks, rvCandPerTick,
              (unsigned long long)rSeen, rCandMax));
        }
      }
      return spread;
    };

    // THE CONTROL FIRST, so the live arm is the one that leaves the world.
    mark("pass R: control arm (rate 0)");
    rvSpreadOff = relevelArm(0);
    mark("pass R: live arm");
    rvSpreadOn = relevelArm(t.sim.waterRelevelMax);

    if (rvSpreadOn >= 0 && rvSpreadOff >= 0) {
      if ((double)rvSpreadOn > spreadMax) {
        fail(Format(
            "RELEVEL (pass R): the surface is still %lld eighths from flat "
            "after %u ticks + %u settling, over the budget of %.0f. The CA "
            "alone left %lld on the same fixture and the lake was %lld at "
            "rest; the ledger moved %lld given / %lld taken with %lld capped "
            "and a credit of %lld, mean %d over %d measured columns, level %d, "
            "%u surface columns",
            (long long)rvSpreadOn, rvTicks, rvSettle, spreadMax,
            (long long)rvSpreadOff, (long long)rvSpread0, (long long)rvGiven,
            (long long)rvTaken, (long long)rvCapped, (long long)rvCredit,
            rvMean, rvCount, rvLevel, rvTopCells));
      } else if (rvSpreadOn >= rvSpreadOff) {
        // The budget was met with the feature OFF, so the fixture proves
        // nothing about the feature. That is a FIXTURE bug and it says so,
        // rather than passing green on a crater the CA would have closed.
        fail(Format(
            "pass R proves nothing: the CA alone closed the crater to %lld "
            "eighths and the relevel to %lld, both inside the %.0f budget. "
            "Deepen or widen the crater (kCraterHalf/kCraterDepth) until the "
            "control arm fails, or the pass is a green light about nothing",
            (long long)rvSpreadOff, (long long)rvSpreadOn, spreadMax));
      }
    }

    RecordObserved("waterbodyRelevelSpreadRest", (double)rvSpread0);
    RecordObserved("waterbodyRelevelSpreadOff", (double)rvSpreadOff);
    RecordObserved("waterbodyRelevelSpreadOn", (double)rvSpreadOn);
    RecordObserved("waterbodyRelevelGiven", (double)rvGiven);
    RecordObserved("waterbodyRelevelTaken", (double)rvTaken);
    RecordObserved("waterbodyRelevelCredit", (double)rvCredit);
    RecordObserved("waterbodyRelevelCapped", (double)rvCapped);
    RecordObserved("waterbodyRelevelConsErr", (double)rvErr);
    RecordObserved("waterbodyRelevelAwake", (double)rvAwake);
    RecordObserved("waterbodyRelevelCandPerTick", rvCandPerTick);
    RecordObserved("waterbodyRelevelFlatTick",
                   (double)(rvFlatTick < 0 ? -1 : rvFlatTick));
    // ONE LINE, and it names every term (CLAUDE.md rule 6): a failure has to
    // say WHICH of "the cutoffs asked for nothing", "the cells refused", "the
    // credit ran away" and "it simply needs longer" it is.
    const std::string flatStr =
        rvFlatTick < 0 ? std::string("never")
                       : std::to_string(rvFlatTick) + " ticks";
    relevelNote = Format(
        "RELEVEL(crater %dx%dx%d, %u ticks + %u settle) spread at rest %lld, "
        "CA alone %lld, relevel %lld eighths (budget %.0f), flat at %s | "
        "credit %lld, given %lld / taken %lld cumulative, capped %lld, mean %d "
        "over %d columns, level %d, %u surface columns | identity %+lld "
        "eighths (in flight %lld, drained %lld, debit %lld), %u awake, %.1f "
        "excite cand/tick | CONTROL arm: identity %+lld, %d measured columns, "
        "%u surface columns, %u awake, %.1f cand/tick (live arm measured %d "
        "columns)",
        kCraterHalf * 2 + 1, kCraterHalf * 2 + 1, kCraterDepth, rvTicks,
        rvSettle, (long long)rvSpread0, (long long)rvSpreadOff,
        (long long)rvSpreadOn, spreadMax, flatStr.c_str(), (long long)rvCredit,
        (long long)rvGiven, (long long)rvTaken, (long long)rvCapped, rvMean,
        rvCount, rvLevel, rvTopCells, (long long)rvErr, (long long)rvInFlight,
        (long long)rvDrained, (long long)rvDebit, rvAwake, rvCandPerTick,
        (long long)armErr[0], armCount[0], armTop[0], armAwake[0], armCand[0],
        armCount[1]);

    // Leave the world settled and pristine for the passes that hash it.
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  mark("pass N");
  // ========================================================== pass N (W-D)
  //
  // DISCOVERY. docs/PLAN_water_relevel.md §8.5, and it is the acceptance of the
  // owner decision that lifted §6's old first exclusion: a body of water the
  // PLAYER made must become a real body and get W1's relevel, while a puddle
  // stays CA and costs nothing.
  //
  // FOUR ARMS, and each fails differently:
  //
  //   1. THE POSITIVE. A pit dug OUTSIDE the harness lake's disc with CellOps
  //      and filled past `sim.waterDiscoverMinEighths`, also with CellOps. A
  //      probe must appear in the registry, take a GPU slot, and the ledger must
  //      read WB_ADOPTED with a measured volume close to what was poured. This
  //      is the whole feature in one assertion.
  //   2. W1 ON THE CREATED BODY, which is the owner's ACTUAL ask rather than
  //      discovery for its own sake. Pass R's crater, bored into the DISCOVERED
  //      pool, and pass R's spread bound asserted on the surface it leaves.
  //   3. THE NEGATIVE, and without it arm 1 is a green light about nothing: a
  //      second pit filled to HALF the threshold must raise NO probe and put
  //      nothing in the ledger. That is the half of §8 that says puddles cost
  //      nothing, and it is also the reachability proof `--sweep` cannot give —
  //      the sweep script places no liquid at all, so every arm of it reports
  //      one hash whatever this knob is set to (W1 learned the same lesson).
  //   4. THE ROUND-TRIP. The registry is the one thing in the water system that
  //      is NOT derivable from (seed, window, voxels), so it is saved with the
  //      world. Save the block, clear the registry, watch the descriptor go,
  //      reload, and require the GPU to re-adopt by RE-MEASURING the same water.
  //
  // NOT GUARDED ON `ok`, for pass R's reason: this pass rebuilds the world
  // itself and shares nothing with the passes before it, and pass H1 carries an
  // inherited conservation failure on this branch — a guarded pass N would ship
  // never having run once.
  //
  // THE FIXTURE IS NOT A DRAIN and the MPM is off, both exactly as pass R has
  // them. This measures a rule about which columns a body OWNS and then a rule
  // that moves settled water between them; anything else in the fixture that
  // moves settled water makes every number a statement about the solver.
  std::string discoverNote;
  {
    const uint32_t nSettle =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyDiscoverTicks", 200.0));
    const int nHalf = (int)BaselineNumber("waterbodyDiscoverPitHalf", 16.0);
    const int nDepth = (int)BaselineNumber("waterbodyDiscoverPitDepth", 14.0);
    const int nCraterHalf =
        (int)BaselineNumber("waterbodyDiscoverCraterHalf", 4.0);
    const int nCraterDepth =
        (int)BaselineNumber("waterbodyDiscoverCraterDepth", 4.0);
    const double nVolTol =
        BaselineNumber("waterbodyDiscoverVolTolPct", 25.0);
    // ITS OWN KEY, and that is the whole point of the line. Arm 2 used to read
    // pass R's `waterbodyRelevelSpread` and landed exactly on it (2 == 2), so
    // the two arms were one budget apart from failing together: tightening R's
    // bound, or a change that cost the created pool one eighth, would have
    // failed a pass about discovery for a reason about the harness lake. Same
    // value, separate knob — this is not extra margin.
    const double nSpreadMax = BaselineNumber("waterbodyDiscoverSpread", 2.0);
    const uint32_t nRelevelTicks =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyDiscoverRelevelTicks",
                                               120.0));

    // ---- the world, and the tuning the whole pass runs under -------------
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    Tuning nt = t;
    nt.sim.waterBodyTestDrain = 0;
    nt.sim.drainMaxEighthsPerTick = 0;   // a pit is not a drain
    nt.sim.fluidExciteMode = 0;          // pass R's discipline, same reason
    nt.sim.drainExciteRadius = 0;
    nt.sim.fluidSplashRate = 0.0f;
    nt.sim.waterRelevelMax = t.sim.waterRelevelMax;
    SetCurrentTuning(nt);
    tick = RunQuietTicks(c, tick, 130);

    // ---- WHERE. Outside the lake's disc, inside the window ---------------
    // Far enough west that the probe's padded disc cannot share a CHUNK with
    // the lake's footprint — a straddle refuses the newcomer (waterbody.cpp's
    // Relabel), so a pit on the bank would test the refusal rather than the
    // feature. Derived from the lake's own geometry rather than written as a
    // literal, because the harness pool has moved once already and a fixture
    // that hardcodes a site is the gotcha this repo has a memory note about.
    const int nx = lakeGeo.cx - lakeGeo.radius - 150;
    const int nz = lakeGeo.cz;
    bool nPlaced = true;
    int gyMin = 1 << 30, gyMax = -(1 << 30);
    for (int z = nz - nHalf; z <= nz + nHalf && nPlaced; z++) {
      for (int x = nx - nHalf; x <= nx + nHalf; x++) {
        const int h = World::TerrainHeight(x, z, kDefaultSeed);
        gyMin = std::min(gyMin, h);
        gyMax = std::max(gyMax, h);
      }
    }
    if (!world.ChunkInWindow({(nx - nHalf - 40) >> 4,
                              (gyMin - nDepth - 8) >> 4,
                              (nz - nHalf - 40) >> 4}) ||
        !world.ChunkInWindow({(nx + nHalf + 40) >> 4, (gyMax + 2) >> 4,
                              (nz + nHalf + 40) >> 4})) {
      fail(Format("pass N: the pit site (%d,%d) y%d..%d is not resident — the "
                  "window moved under the fixture",
                  nx, nz, gyMin - nDepth, gyMax));
      nPlaced = false;
    }

    // ---- ARM 1: dig, fill, and require a body -----------------------------
    // TWO TICKS, not one. The carve writes AIR over cells the fill then writes
    // WATER into, and two ops aimed at one cell in one tick is a mutation
    // ordering question this fixture has no business asking.
    //
    // The fill stops one voxel BELOW the lowest rim in the footprint, so the
    // pool is contained by terrain on every column whatever the ground does —
    // a fixture that filled to the highest rim would be testing a spill.
    uint32_t nSlot = kNoGpuSlot;
    uint32_t nProbeBasin = 0;
    int64_t nPoured = 0;
    int32_t nState = -1, nVolume = 0, nRArea = 0, nLevel = 0;
    int32_t nAdoptTick = -1;
    size_t nRegistry = 0;
    const int nWaterTop = gyMin - 2;
    const int nWaterBot = gyMin - nDepth;
    if (nPlaced) {
      std::vector<CellOp> carve, fill;
      for (int y = nWaterBot; y <= gyMax; y++)
        for (int z = nz - nHalf; z <= nz + nHalf; z++)
          for (int x = nx - nHalf; x <= nx + nHalf; x++)
            carve.push_back({World::SlotCellIndex({x, y, z}), 0u});
      for (int y = nWaterBot; y <= nWaterTop; y++)
        for (int z = nz - nHalf; z <= nz + nHalf; z++)
          for (int x = nx - nHalf; x <= nx + nHalf; x++) {
            fill.push_back({World::SlotCellIndex({x, y, z}),
                            PackVoxNew(matId, 7u)});
            nPoured += 8;
          }
      if (carve.size() > kMaxCellOpsPerTick ||
          fill.size() > kMaxCellOpsPerTick) {
        fail(Format("pass N: the pit needs %llu carve / %llu fill ops, over the "
                    "%u-op per-tick budget — shrink "
                    "waterbodyDiscoverPitHalf/Depth",
                    (unsigned long long)carve.size(),
                    (unsigned long long)fill.size(), kMaxCellOpsPerTick));
        nPlaced = false;
      } else {
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, carve,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, fill,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;
        tick = RunQuietTicks(c, tick, nSettle);

        nRegistry = WaterBodies().Discovered().size();
        if (nRegistry != 1) {
          fail(Format(
              "pass N arm 1: %llu probes in the registry, expected 1, after "
              "pouring %lld eighths at (%d,%d) against a threshold of %d — the "
              "evidence never reached the promotion scan, or the site clashed "
              "with the lake",
              (unsigned long long)nRegistry, (long long)nPoured, nx, nz,
              nt.sim.waterDiscoverMinEighths));
        } else {
          nProbeBasin = WaterBodies().Discovered()[0].basinId;
          const WaterBodyDesc* nd = WaterBodies().Find(nProbeBasin);
          if (!nd || nd->gpuSlot >= kWaterBodyCap) {
            fail(Format("pass N arm 1: the probe (basin %08x) took no GPU slot "
                        "— the CPU ladder refused it (%d)",
                        nProbeBasin, nd ? (int)nd->refusal : -1));
          } else {
            nSlot = nd->gpuSlot;
            const LedgerView lv = ReadLedger(c);
            nState = lv.At(nSlot, WBS_STATE);
            nVolume = lv.At(nSlot, WBS_VOLUME);
            nRArea = lv.At(nSlot, WBS_RAREA_W);
            nLevel = lv.At(nSlot, WBS_LEVEL);
            nAdoptTick = lv.At(nSlot, WBS_ADOPTTICK);
            if (nState != WB_ADOPTED) {
              fail(Format(
                  "pass N arm 1: the created body is %s, not adopted, %u ticks "
                  "after %lld eighths were poured into it (measured volume %d, "
                  "measured surface %d cells against a floor of %d, level %d)",
                  LedgerStateName(nState), nSettle, (long long)nPoured, nVolume,
                  nRArea, nt.sim.waterAdoptMinArea, nLevel));
            } else {
              // WITHIN TOLERANCE, not exact, and the tolerance is the honest
              // part: the CA settles the pour, the top layer levels out and the
              // evaporation rule acts on a freshly exposed 33x33 surface. What
              // would be a BUG is a body measuring a different pool — half the
              // water, or the lake next door.
              const double err =
                  nPoured == 0 ? 100.0
                               : 100.0 * ((double)nVolume - (double)nPoured) /
                                     (double)nPoured;
              if (std::abs(err) > nVolTol)
                fail(Format("pass N arm 1: the created body measures %d eighths "
                            "against %lld poured (%+.2f%%, tolerance %.2f%%) — "
                            "the probe disc is not over the water that was made",
                            nVolume, (long long)nPoured, err, nVolTol));
            }
          }
        }
      }
    }

    // ---- ARM 3: the negative. Half the threshold raises nothing ----------
    // The pit is real and the water is real; only the QUANTITY is below the
    // bar. That is the distinction the feature is supposed to make, and the
    // alternative arm — pouring nothing — would pass against a build where
    // discovery had been deleted.
    size_t nRegistry2 = nRegistry;
    int64_t nPoured2 = 0;
    bool nGhost = false;
    if (nPlaced) {
      // A pit of `half` giving just under half the threshold in eighths, so the
      // count is derived from the knob rather than hoped to be under it.
      const int64_t wantVox = std::max<int64_t>(
          1, (int64_t)nt.sim.waterDiscoverMinEighths / 16);   // half, /8 per vox
      int side = 1;
      while ((int64_t)(side + 1) * (side + 1) * 2 <= wantVox) side++;
      const int px = nx, pz2 = nz - nHalf - 120;
      int pgy = 1 << 30;
      for (int z = pz2; z < pz2 + side; z++)
        for (int x = px; x < px + side; x++)
          pgy = std::min(pgy, World::TerrainHeight(x, z, kDefaultSeed));
      if (world.ChunkInWindow({(px - 8) >> 4, (pgy - 8) >> 4, (pz2 - 8) >> 4}) &&
          world.ChunkInWindow({(px + side + 8) >> 4, (pgy + 2) >> 4,
                               (pz2 + side + 8) >> 4})) {
        std::vector<CellOp> carve2, fill2;
        for (int y = pgy - 3; y <= pgy; y++)
          for (int z = pz2; z < pz2 + side; z++)
            for (int x = px; x < px + side; x++)
              carve2.push_back({World::SlotCellIndex({x, y, z}), 0u});
        for (int y = pgy - 3; y <= pgy - 2; y++)
          for (int z = pz2; z < pz2 + side; z++)
            for (int x = px; x < px + side; x++) {
              fill2.push_back({World::SlotCellIndex({x, y, z}),
                               PackVoxNew(matId, 7u)});
              nPoured2 += 8;
            }
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, carve2,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, fill2,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;
        tick = RunQuietTicks(c, tick, 60);
        nRegistry2 = WaterBodies().Discovered().size();
        if (nPoured2 >= nt.sim.waterDiscoverMinEighths) {
          fail(Format("pass N arm 3 proves nothing: the 'half' pit poured %lld "
                      "eighths against a threshold of %d — it is over the bar",
                      (long long)nPoured2, nt.sim.waterDiscoverMinEighths));
        } else if (nRegistry2 != nRegistry) {
          fail(Format(
              "pass N arm 3: %llu probes in the registry after a puddle of %lld "
              "eighths (threshold %d), was %llu — a puddle raised a body",
              (unsigned long long)nRegistry2, (long long)nPoured2,
              nt.sim.waterDiscoverMinEighths,
              (unsigned long long)nRegistry));
        }
        // AND NO LEDGER SLOT WENT LIVE. The registry count alone would miss a
        // probe that was raised and then dropped inside the window; every slot
        // past the ones we know about must be untouched.
        const LedgerView lv2 = ReadLedger(c);
        for (uint32_t s = 0; s < kWaterBodyCap; s++) {
          if (s == nSlot) continue;
          const int32_t st2 = lv2.At(s, WBS_STATE);
          const WaterBodyDesc* own = nullptr;
          for (const WaterBodyDesc& bd : WaterBodies().Bodies())
            if (bd.gpuSlot == s) own = &bd;
          // CHILDREN TOO. `Bodies()` is parallel to `basins_` and the M5 split
          // children are deliberately kept out of it (waterbody.h says why), so
          // a check that walked only `Bodies()` calls every split child a ghost
          // — and a body someone has dug a crater into always has one. Measured
          // as a false accusation on the first run of this pass.
          for (const WaterBodyDesc& bd : WaterBodies().Children())
            if (bd.gpuSlot == s) own = &bd;
          if (st2 != WB_CANDIDATE && own == nullptr) {
            nGhost = true;
            fail(Format("pass N arm 3: ledger slot %u is %s with no descriptor "
                        "behind it — a puddle took a slot",
                        s, LedgerStateName(st2)));
            break;
          }
        }
      }
    }

    // ---- ARM 4: the save/reload round-trip --------------------------------
    int32_t nStateAfter = -1;
    int32_t nVolAfter = 0;
    size_t nRegistry3 = 0;
    bool nCleared = false;
    if (nSlot < kWaterBodyCap && nState == WB_ADOPTED) {
      std::vector<uint8_t> blob;
      WaterBodies().SaveState(blob);
      const size_t want = WaterBodies().Discovered().size();
      WaterBodies().ClearDiscovered();
      // Four ticks so the CPU withdraws the descriptor and the ledger clears
      // the slot it held (wbLedger's `(flags & WBF_PROPOSE) == 0` branch).
      tick = RunQuietTicks(c, tick, 4);
      nCleared = WaterBodies().Discovered().empty();
      if (!nCleared)
        fail("pass N arm 4: ClearDiscovered left entries behind, so the reload "
             "would be testing nothing");
      if (!WaterBodies().LoadState(blob.data(), blob.size(),
                                   WaterBodySystem::kSaveVersion)) {
        fail("pass N arm 4: the 'WTRB' block did not load back");
      } else {
        // LONG ENOUGH TO RE-ADOPT BY MEASURING, which is the point: nothing
        // about the body's state was saved, so the GPU has to run the whole
        // Candidate -> Measuring -> Adopted ladder again over the restored
        // water. A round trip that restored a LEDGER would pass this in one
        // tick and would be the carried-descriptor bug.
        tick = RunQuietTicks(c, tick, 90);
        nRegistry3 = WaterBodies().Discovered().size();
        const WaterBodyDesc* rd2 =
            nRegistry3 > 0
                ? WaterBodies().Find(WaterBodies().Discovered()[0].basinId)
                : nullptr;
        if (nRegistry3 != want || !rd2 || rd2->gpuSlot >= kWaterBodyCap) {
          fail(Format("pass N arm 4: %llu probes restored (wanted %llu), slot "
                      "%u",
                      (unsigned long long)nRegistry3,
                      (unsigned long long)want,
                      rd2 ? rd2->gpuSlot : kNoGpuSlot));
        } else {
          const LedgerView lv3 = ReadLedger(c);
          nStateAfter = lv3.At(rd2->gpuSlot, WBS_STATE);
          nVolAfter = lv3.At(rd2->gpuSlot, WBS_VOLUME);
          // ATTRIBUTION, NOT A COUNT (CLAUDE.md rule 6). "Not adopted" is four
          // different bugs — the reduce found nothing, it found a pool under
          // the volume floor, it found a film under the area floor, or a live
          // SPLIT MAP handed this component to somebody else — and only these
          // numbers tell them apart. The refusal path deliberately keeps
          // RSUM/RAREA/LEVEL for exactly this read.
          if (nStateAfter != WB_ADOPTED)
            fail(Format(
                "pass N arm 4: the restored probe is %s, not adopted — the "
                "registry round-tripped but the GPU did not re-measure the "
                "water behind it. slot %u: reduce sum %d (volume floor %d), "
                "measured surface %d cells (floor %d), level %d (basin floor "
                "%d, seed %d), quiet %d, %llu listed chunks | sweep: %d "
                "components mapped at y=%d, spill %d, split %d",
                LedgerStateName(nStateAfter), rd2->gpuSlot,
                lv3.At(rd2->gpuSlot, WBS_RSUM), nt.sim.waterBodyMinVolume,
                lv3.At(rd2->gpuSlot, WBS_RAREA_W), nt.sim.waterAdoptMinArea,
                lv3.At(rd2->gpuSlot, WBS_LEVEL),
                WaterBodies().Basin(rd2->basinId)
                    ? WaterBodies().Basin(rd2->basinId)->floorY
                    : 0,
                WaterBodies().Basin(rd2->basinId)
                    ? WaterBodies().Basin(rd2->basinId)->surfY
                    : 0,
                lv3.At(rd2->gpuSlot, WBS_QUIET),
                (unsigned long long)rd2->chunks.size(),
                lv3.Sw(rd2->gpuSlot, SW_COMPS), lv3.Sw(rd2->gpuSlot, SW_MAPY),
                lv3.Sw(rd2->gpuSlot, SW_SPILLY),
                lv3.Sw(rd2->gpuSlot, SW_SPLITY)));
        }
      }
    }

    // ---- ARM 2: pass R's crater, in the DISCOVERED body ------------------
    //
    // LAST, not second as §8.5 lists it, and the reordering is a finding rather
    // than a convenience. Boring the crater lands a mutation in a chunk the
    // probe LABELLED, which sets the basin's curve-dirty latch for 900 ticks —
    // and a curve-dirty basin is what arms M5's sweep. The sweep then publishes
    // a split map at the body's live level, this pool's disc holds a second
    // open region (the pit's water reached 1,107 columns against the 1,089 that
    // were dug, so it found a way out sideways), and the parent is handed
    // component 0 while the child adopts the water.
    //
    // That is M5 behaving as designed for an ADOPTED body, and it made arm 4 —
    // run in between — report a refusal with `reduce sum 0, 2 components mapped
    // at y=198`. But arm 4 is a statement about the REGISTRY round-tripping,
    // and a real load does not reproduce that situation at all: LoadWorld
    // restores the grid and the ledger buffer comes back zeroed, so no stale
    // split map survives for a re-adopting body to lose to. Running the round
    // trip on a body nobody has dug into tests what §8.5 asks; running it after
    // the crater tested M5's split under a ladder restart, which is a real
    // question and not this package's.
    int64_t nSpread = -1;
    uint32_t nPf = 0;
    if (nSlot < kWaterBodyCap && nState == WB_ADOPTED) {
      const WaterBasin* pb = WaterBodies().Basin(nProbeBasin);
      const WaterBodyDesc* pd = WaterBodies().Find(nProbeBasin);
      if (!pb || !pd) {
        fail("pass N arm 2: the probe left the registry between the arms");
      } else {
        const WaterBasin pbCopy = *pb;
        const WaterBodyDesc pdCopy = *pd;
        uint32_t pf0[4] = {0, 0, 0, 0};
        ReadPageFaultsSync(c.ctx, world, pf0);
        std::vector<CellOp> crater;
        for (int y = nWaterBot - nCraterDepth; y < nWaterBot; y++)
          for (int z = nz - nCraterHalf; z <= nz + nCraterHalf; z++)
            for (int x = nx - nCraterHalf; x <= nx + nCraterHalf; x++)
              crater.push_back({World::SlotCellIndex({x, y, z}), 0u});
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, crater,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;
        tick = RunQuietTicks(c, tick, nRelevelTicks);
        const VoxelTruth nv = SweepBasin(c, pbCopy, pdCopy, matId,
                                         nWaterBot - nCraterDepth, gyMax);
        uint32_t pf1[4] = {0, 0, 0, 0};
        ReadPageFaultsSync(c.ctx, world, pf1);
        nPf = pf1[0] - pf0[0];
        if (!nv.read || nv.topCells == 0) {
          fail("pass N arm 2: the sweep found no free surface in the created "
               "body after the crater");
        } else {
          nSpread = (int64_t)nv.topMaxE - (int64_t)nv.topMinE;
          if ((double)nSpread > nSpreadMax)
            fail(Format(
                "pass N arm 2: the CREATED body's surface is still %lld eighths "
                "from flat %u ticks after a %dx%dx%d crater, over pass R's "
                "budget of %.0f — W1 does not reach a discovered body, which is "
                "the owner's actual ask",
                (long long)nSpread, nRelevelTicks, nCraterHalf * 2 + 1,
                nCraterHalf * 2 + 1, nCraterDepth, nSpreadMax));
        }
        if (nPf != 0)
          fail(Format("pass N arm 2: %u page faults (lost word 0x%08x) — a "
                      "relevel wrote into a sentinel chunk, so a DISCOVERED "
                      "body's footprint is not being declared to the page table",
                      nPf, pf1[2]));
      }
    }

    RecordObserved("waterbodyDiscoverProbes", (double)nRegistry);
    RecordObserved("waterbodyDiscoverVolume", (double)nVolume);
    RecordObserved("waterbodyDiscoverPoured", (double)nPoured);
    RecordObserved("waterbodyDiscoverArea", (double)nRArea);
    RecordObserved("waterbodyDiscoverAdoptTick", (double)nAdoptTick);
    // `...SpreadOn`, not `...Spread`: the THRESHOLD now owns that name, and a
    // recorded value sharing a key with a budget is a --rebaseline that quietly
    // overwrites the budget with whatever the last run measured.
    RecordObserved("waterbodyDiscoverSpreadOn", (double)nSpread);
    // ONE LINE, and it names every term §8.5 asks for plus the ones a failure
    // needs to tell itself apart (CLAUDE.md rule 6): "no body appeared" is a
    // different bug from "a body appeared and measured the wrong pool" and from
    // "a body appeared and the relevel did not reach it".
    int nRefused = 0;
    {
      const LedgerView lvF = ReadLedger(c);
      for (uint32_t s = 0; s < kWaterBodyCap; s++)
        if (lvF.At(s, WBS_STATE) == WB_REFUSED) nRefused++;
    }
    discoverNote = Format(
        "DISCOVERY(pit %dx%dx%d at %d,%d y%d..%d) bodies %llu / adopted-tick %d "
        "/ refused %d | poured %lld -> measured %d eighths, surface %d cells "
        "(floor %d), level %d, state %s, slot %u | crater %dx%dx%d spread %lld "
        "eighths (budget %.0f), %u page faults | NEGATIVE arm: %lld eighths "
        "(half of %d) left %llu probes%s | ROUND TRIP: cleared %s, %llu "
        "restored, %s, volume %d -> %d | %u evictions, %llu evidence cells",
        nHalf * 2 + 1, nHalf * 2 + 1, nDepth - 1, nx, nz, nWaterBot, nWaterTop,
        (unsigned long long)nRegistry, nAdoptTick, nRefused, (long long)nPoured,
        nVolume, nRArea, nt.sim.waterAdoptMinArea, nLevel,
        LedgerStateName(nState), nSlot, nCraterHalf * 2 + 1,
        nCraterHalf * 2 + 1, nCraterDepth, (long long)nSpread, nSpreadMax, nPf,
        (long long)nPoured2, nt.sim.waterDiscoverMinEighths,
        (unsigned long long)nRegistry2, nGhost ? " + a ghost slot" : "",
        nCleared ? "yes" : "no", (unsigned long long)nRegistry3,
        LedgerStateName(nStateAfter), nVolume, nVolAfter,
        WaterBodies().DiscoverEvictions(),
        (unsigned long long)WaterBodies().Evidence().size());

    // Leave the world and the registry pristine for the passes that hash it.
    // The probe MUST go: pass D compares a mode-0 script against a mode-1 one
    // and a surviving discovered body would make the mode-1 arm describe a
    // different world.
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  mark("pass S");
  // ========================================================== pass S (W2)
  //
  // SLOSH. docs/PLAN_water_relevel.md §4.5. W1 taught a disturbed pond to find
  // its level; this is the acceptance that it finds it the way water does —
  // the columns beside a fresh crater accelerate into it, arrive carrying
  // momentum, overshoot, and ring out until the damping eats them.
  //
  // THE FIXTURE IS PASS R'S, DELIBERATELY. Same lake, same crater, same
  // no-drain / no-MPM discipline, so the only difference between the two passes
  // is `sim.waveMode` — which means every number here is comparable to a number
  // pass R already printed, and a regression in the shared half shows up in
  // both rather than in neither.
  //
  // WHAT IS ASSERTED, and each fails differently:
  //
  //   * THE MASS IDENTITY of §3.5, with the credit term, exactly as pass R
  //     states it. This is the one that matters most: a pipe layer is a
  //     transfer between two columns, and the entire argument for owned
  //     outflows over shared signed faces is that each transfer lives in ONE
  //     word both ends read. If that argument is wrong, water is created or
  //     destroyed at the seam and this line is what says so.
  //   * DISSIPATION. Sigma|q| over the body must RISE (the crater accelerates
  //     the rim) and then FALL and stay fallen. Not created, ever.
  //   * SLEEP. The body must publish flux-asleep inside the window, or the
  //     feature has no idle cost story and rule 2 is broken.
  //   * FLATNESS at the end, on pass R's own budget: a ring that never settles
  //     level is not water.
  //
  // WHY THE DISSIPATION TEST IS BINNED rather than a raw per-tick
  // non-increase. Sigma|q| is an integer sum over ~14,000 dithered columns and
  // a ring REFLECTS off the bank: it focuses at the centre, which concentrates
  // the same momentum into fewer columns and can lift the sum for a tick or
  // two. A strict per-tick monotone assertion would be a knife edge measuring
  // the bank's shape, not the physics. Binned maxima with a stated tolerance
  // say the thing that is actually true — "after its peak it goes down and
  // stays down" — and the decay floor at the end is what makes it a claim
  // rather than a shrug.
  //
  // NOT GUARDED ON `ok`, for pass R's and pass N's reason: it rebuilds the
  // world itself, and pass H1 carries an inherited conservation failure on this
  // branch that would otherwise turn this into a silent skip.
  std::string sloshNote;
  {
    const uint32_t swTicks =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyWaveTicks", 120.0));
    const uint32_t swSettle = (uint32_t)std::max(
        1.0, BaselineNumber("waterbodyWaveSettleTicks", 60.0));
    const uint32_t swSleepMax = (uint32_t)std::max(
        1.0, BaselineNumber("waterbodyWaveSleepTicks", 150.0));
    const uint32_t swBin =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyWaveBinTicks", 10.0));
    const double swSpreadMax = BaselineNumber("waterbodyWaveSpread", 2.0);
    const double swRiseTolPct = BaselineNumber("waterbodyWaveRiseTolPct", 10.0);
    const double swDecayFrac = BaselineNumber("waterbodyWaveDecayFrac", 0.25);
    const int64_t swSlack =
        (int64_t)BaselineNumber("waterbodyWaveSlackEighths", 64.0);

    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    Tuning st2 = t;
    st2.sim.waterBodyTestDrain = 0;      // the CRATER is the disturbance
    st2.sim.drainMaxEighthsPerTick = 0;  // ...and it is NOT a drain
    st2.sim.fluidExciteMode = 0;         // pass R's discipline, same reason
    st2.sim.drainExciteRadius = 0;
    st2.sim.fluidSplashRate = 0.0f;
    // ARMED EXPLICITLY, both of them. `sim.waterBodyMode` is 0 in the shipped
    // tuning and `sim.waveMode` is 0 as W2 ships; a pass that inherited either
    // would be a green light about a feature that never ran.
    st2.sim.waterBodyMode = 1;
    st2.sim.waterRelevelMax =
        t.sim.waterRelevelMax > 0 ? t.sim.waterRelevelMax : 4;
    st2.sim.waveMode = 1;
    SetCurrentTuning(st2);
    tick = RunQuietTicks(c, tick, 130);

    const int swBoxLo = lakeGeo.floorY - kCraterDepth;
    const int swBoxHi = lakeGeo.surfY;
    int64_t swSpread = -1, swErr = 0, swCredit = 0, swGiven = 0, swTaken = 0;
    int64_t swCapped = 0, swPeak = 0, swLast = 0, swInFlight = 0;
    int swPeakBin = -1, swSleepTick = -1, swBadBin = -1;
    uint32_t swAwake = 0, swPf = 0;
    std::vector<int64_t> swBins;

    const WaterBodyDesc* sd = WaterBodies().Find(LakeId());
    if (!sd || sd->gpuSlot >= kWaterBodyCap) {
      fail("pass S: the authored lake is not proposed");
    } else {
      const uint32_t sSlot = sd->gpuSlot;
      const LedgerView lv0 = ReadLedger(c);
      if (lv0.At(sSlot, WBS_STATE) != WB_ADOPTED) {
        fail(Format("pass S: the lake is %s, not adopted, before the crater",
                    LedgerStateName(lv0.At(sSlot, WBS_STATE))));
      } else {
        const VoxelTruth s0 =
            SweepBasin(c, lakeGeo, lakeDesc, matId, swBoxLo, swBoxHi);
        uint32_t sPf0[4] = {0, 0, 0, 0};
        ReadPageFaultsSync(c.ctx, world, sPf0);

        std::vector<CellOp> crater;
        for (int y = lakeGeo.floorY - kCraterDepth + 1; y <= lakeGeo.floorY; y++)
          for (int z = lakeGeo.cz - kCraterHalf; z <= lakeGeo.cz + kCraterHalf;
               z++)
            for (int x = lakeGeo.cx - kCraterHalf; x <= lakeGeo.cx + kCraterHalf;
                 x++)
              crater.push_back({World::SlotCellIndex({x, y, z}), 0u});
        SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, crater,
                   false, c.world.WindowOrigin(), true, false);
        c.ctx.ProcessEvents();
        tick++;

        // ---- THE CURVE. One tick, one ledger read, one sample. -----------
        //
        // A per-tick readback is expensive and this is the one place that
        // earns it: the claim is about the SHAPE of Sigma|q| over time, and a
        // shape sampled every fifteen ticks (pass R's cadence) cannot tell a
        // ring that rose and fell from one that rose and stayed up. The window
        // is bounded by the sleep budget, and the loop STOPS the moment the
        // body reports asleep — which on a working build is most of it.
        int64_t binMax = 0;
        uint32_t inBin = 0;
        for (uint32_t i = 0; i < swSleepMax; i++) {
          tick = RunQuietTicks(c, tick, 1);
          const LedgerView lvi = ReadLedger(c);
          const int64_t qs = lvi.At(sSlot, WBS_WVSUM_W);
          swLast = qs;
          if (qs > swPeak) swPeak = qs;
          binMax = std::max(binMax, qs);
          if (++inBin >= swBin) {
            swBins.push_back(binMax);
            binMax = 0;
            inBin = 0;
          }
          if (swSleepTick < 0 && lvi.At(sSlot, WBS_WVASLEEP_W) != 0)
            swSleepTick = (int)i + 1;
          if (swSleepTick >= 0 && i + 1 >= swTicks) break;
        }
        if (inBin > 0) swBins.push_back(binMax);

        tick = RunQuietTicks(c, tick, swSettle);
        const VoxelTruth s1 =
            SweepBasin(c, lakeGeo, lakeDesc, matId, swBoxLo, swBoxHi);
        const LedgerView lv1 = ReadLedger(c);
        uint32_t sFa[kFluidArgsWords] = {};
        ReadFluidArgsSync(c.ctx, world, sFa);
        uint32_t sPf1[4] = {0, 0, 0, 0};
        ReadPageFaultsSync(c.ctx, world, sPf1);
        swAwake = ReadActiveChunksSync(c.ctx, world, c.sim);
        swPf = sPf1[0] - sPf0[0];
        swCredit = lv1.At(sSlot, WBS_RVCREDIT_W);
        swGiven = lv1.At(sSlot, WBS_RVGIVENT_W);
        swTaken = lv1.At(sSlot, WBS_RVTAKENT_W);
        swCapped = lv1.At(sSlot, WBS_RVCAPPED_W);
        swInFlight = (int64_t)sFa[7] - (int64_t)std::min(sFa[29], sFa[7]);

        if (!s1.read || s1.topCells == 0) {
          fail("pass S: the final sweep found no free surface at all");
        } else {
          swSpread = (int64_t)s1.topMaxE - (int64_t)s1.topMinE;
          // ---- (a) THE MASS IDENTITY, with the credit term ---------------
          swErr = (int64_t)s1.eighths + swInFlight +
                  lv1.At(sSlot, WBS_DRAINED) - lv1.At(sSlot, WBS_DEBIT) +
                  (swCredit + lv1.At(sSlot, WBS_RVGIVEN_W) -
                   lv1.At(sSlot, WBS_RVTAKEN_W)) -
                  (int64_t)s0.eighths;
          if (swErr < -swSlack || swErr > swSlack) {
            fail(Format(
                "CONSERVATION (pass S): the sloshing lake is off by %+lld "
                "eighths. box %llu -> %llu (%+lld), in flight %lld, credit "
                "%lld (%lld given / %lld taken cumulative), capped %lld. A "
                "pipe transfer lives in ONE word both ends read, so a non-zero "
                "number here means the giver and the receiver disagreed about "
                "whether a pipe existed (plan §4.2)",
                (long long)swErr, (unsigned long long)s0.eighths,
                (unsigned long long)s1.eighths,
                (long long)((int64_t)s1.eighths - (int64_t)s0.eighths),
                (long long)swInFlight, (long long)swCredit, (long long)swGiven,
                (long long)swTaken, (long long)swCapped));
          }
          if (swPf != 0) {
            fail(Format("pass S: %u page faults (lost word 0x%08x) — the wave "
                        "apply wrote into a sentinel chunk",
                        swPf, sPf1[2]));
          }
          // ---- (b) DISSIPATIVE: rises, then falls and stays down ---------
          for (size_t i = 0; i < swBins.size(); i++)
            if (swPeakBin < 0 || swBins[i] > swBins[(size_t)swPeakBin])
              swPeakBin = (int)i;
          if (swPeak <= 0) {
            fail("pass S proves nothing: Sigma|q| was 0 for the whole window, "
                 "so no pipe ever carried anything and sim.waveMode reached no "
                 "kernel. Check the arm (TickParams::waveMode) and the "
                 "Cond::WaterWave row before believing any other number here");
          } else {
            for (size_t i = (size_t)swPeakBin + 1; i < swBins.size(); i++) {
              const double cap =
                  (double)swBins[i - 1] * (1.0 + swRiseTolPct / 100.0);
              if ((double)swBins[i] > cap) { swBadBin = (int)i; break; }
            }
            if (swBadBin >= 0) {
              fail(Format(
                  "pass S: Sigma|q| ROSE after its peak — bin %d is %lld "
                  "against bin %d's %lld (tolerance %.0f%%), peak %lld at bin "
                  "%d of %llu. A heightfield that gains momentum after the "
                  "disturbance stopped is creating energy, which is the "
                  "seam's 'settle and wake must be strictly dissipative' "
                  "lesson applied to pipes",
                  swBadBin, (long long)swBins[(size_t)swBadBin], swBadBin - 1,
                  (long long)swBins[(size_t)swBadBin - 1], swRiseTolPct,
                  (long long)swPeak, swPeakBin,
                  (unsigned long long)swBins.size()));
            }
            if (!swBins.empty() &&
                (double)swBins.back() > swDecayFrac * (double)swPeak) {
              fail(Format(
                  "pass S: the ring did not die — the last bin is still %lld "
                  "against a peak of %lld (budget %.2f of peak) after %u "
                  "ticks. sim.waveDamping is %.2f/s, which should e-fold in "
                  "%.1f ticks",
                  (long long)swBins.back(), (long long)swPeak, swDecayFrac,
                  swSleepMax, (double)st2.sim.waveDamping,
                  st2.sim.waveDamping > 0.0f
                      ? 30.0 / (double)st2.sim.waveDamping
                      : 0.0));
            }
            // `swPeakBin == 0` is NOT asserted. A crater this size accelerates
            // the rim inside the first bin, so "the peak is in bin 0" is the
            // normal shape here rather than evidence of a wave that never
            // rose — and the thing that WOULD catch that is the `swPeak <= 0`
            // branch above, which is exact. The bin index is on the printed
            // line so a change in the shape is visible without being a budget.
          }
          // ---- (c) SLEEP -------------------------------------------------
          if (swSleepTick < 0) {
            fail(Format(
                "pass S: the body never published flux-asleep in %u ticks "
                "(Sigma|q| ended at %lld against a peak of %lld, sleep "
                "epsilon %d Q8, settle window %d ticks). A wave layer that "
                "cannot sleep has no idle-cost story at all (rule 2)",
                swSleepMax, (long long)swLast, (long long)swPeak,
                st2.sim.waveSleepEps, st2.sim.fluidSettleTicks));
          }
          // ---- (d) FLAT AT THE END ---------------------------------------
          if ((double)swSpread > swSpreadMax) {
            fail(Format(
                "pass S: the sloshing surface is still %lld eighths from flat "
                "after %u ticks + %u settling, over the budget of %.0f. The "
                "ledger moved %lld given / %lld taken with %lld capped and a "
                "credit of %lld",
                (long long)swSpread, swSleepMax, swSettle, swSpreadMax,
                (long long)swGiven, (long long)swTaken, (long long)swCapped,
                (long long)swCredit));
          }
          if ((double)swAwake > awakeMax) {
            fail(Format("pass S: %u chunks still awake %u ticks after the "
                        "ring died, over the budget of %.0f",
                        swAwake, swSettle, awakeMax));
          }
        }
      }
    }

    RecordObserved("waterbodyWavePeak", (double)swPeak);
    RecordObserved("waterbodyWavePeakBin", (double)swPeakBin);
    RecordObserved("waterbodyWaveSleepAt", (double)swSleepTick);
    RecordObserved("waterbodyWaveSpreadOn", (double)swSpread);
    RecordObserved("waterbodyWaveCredit", (double)swCredit);
    RecordObserved("waterbodyWaveGiven", (double)swGiven);
    RecordObserved("waterbodyWaveTaken", (double)swTaken);
    RecordObserved("waterbodyWaveCapped", (double)swCapped);
    RecordObserved("waterbodyWaveConsErr", (double)swErr);
    RecordObserved("waterbodyWaveAwake", (double)swAwake);
    // ONE LINE, EVERY TERM (CLAUDE.md rule 6). "The pond did not slosh" is at
    // least five different bugs — the arm never reached the kernel (peak 0),
    // the pipes saturated and behaved as a relevel (peak high, bins flat), the
    // ring never died (last bin high), the body never slept (asleep -1), or
    // the transfer leaked (identity non-zero) — and only these numbers
    // together tell them apart.
    std::string binStr;
    for (size_t i = 0; i < swBins.size() && i < 16; i++) {
      if (i) binStr += ",";
      binStr += std::to_string((long long)swBins[i]);
    }
    sloshNote = Format(
        "SLOSH(crater %dx%dx%d, waveMode 1, g %.0f vox/s² cap %d vox, damp "
        "%.2f/s, eps %d) Sigma|q| peak %lld Q8 at bin %d of %llu (%u ticks "
        "each) -> last %lld | bins [%s] | asleep at %s (budget %u) | spread "
        "%lld eighths (budget %.0f) | identity %+lld (in flight %lld, credit "
        "%lld, %lld given / %lld taken cumulative, capped %lld) | %u awake, "
        "%u page faults",
        kCraterHalf * 2 + 1, kCraterHalf * 2 + 1, kCraterDepth,
        (double)st2.sim.waveGravity, st2.sim.waveDepthCap,
        (double)st2.sim.waveDamping, st2.sim.waveSleepEps, (long long)swPeak,
        swPeakBin, (unsigned long long)swBins.size(), swBin, (long long)swLast,
        binStr.c_str(),
        swSleepTick < 0 ? std::string("never")
                        : std::to_string(swSleepTick) + " ticks",
        swSleepMax, (long long)swSpread, swSpreadMax, (long long)swErr,
        (long long)swInFlight, (long long)swCredit, (long long)swGiven,
        (long long)swTaken, (long long)swCapped, swAwake, swPf);

    // Leave the world settled and pristine for the passes that hash it.
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  mark("pass T");
  // ========================================================== pass T (W3)
  //
  // WHAT DISTURBS THE SURFACE. docs/PLAN_water_relevel.md §5. Pass S proved the
  // pipes carry a ring the HEIGHTFIELD started; these are the three sources that
  // start one from OUTSIDE it — a blast, a swimmer, and a live discharge — and
  // each is behind its own knob that must be an exact identity at 0.
  //
  // THE SHAPE OF EVERY ARM IS THE SAME, and it is the minimum that proves
  // anything: run the fixture with the knob OFF and with it ON, and require the
  // measured effect to be exactly zero in the first and non-zero in the second.
  // Either half alone is a green light about nothing — an "on" arm with no
  // control cannot tell the feature from the crater the CA would have closed
  // anyway (pass R's lesson), and an "off" arm with no treatment cannot tell an
  // identity from a knob that reaches no kernel at all.
  //
  // THE IMPULSE ARMS GO THROUGH THE GAME'S OWN EMITTERS, not through
  // SpawnImpulse directly: WaterBodyNoteBlast and WaterBodyNoteSwimmer are what
  // SubmitTick and main.cpp's frame loop call, so the arithmetic under test is
  // the arithmetic the game runs rather than a copy of it (the
  // gate-hardcodes-the-cast gotcha, applied to a conversion).
  //
  // NOT A REAL EXPLOSION, deliberately. Detonating over the lake would carve
  // voxels, and then the mass identity below would be measuring sim_explode
  // rather than the pipes. The door is the same either way — SubmitTick turns
  // every ExplosionOp in the engine into exactly this call — so handing the
  // emitter an ExplosionOp and letting the impulse ride the tick stream tests
  // the whole path from the event to the pipe with nothing else moving.
  std::string w3Note;
  {
    const uint32_t w3Ticks =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyW3Ticks", 60.0));
    const uint32_t w3Settle =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyW3SettleTicks", 60.0));
    const uint32_t w3Bin =
        (uint32_t)std::max(1.0, BaselineNumber("waterbodyW3BinTicks", 10.0));
    const double w3RiseTolPct = BaselineNumber("waterbodyW3RiseTolPct", 10.0);
    const int64_t w3Slack =
        (int64_t)BaselineNumber("waterbodyW3SlackEighths", 64.0);
    // The blast the emitter is handed. Radius is the lake's own scale rather
    // than a literal: a 20-cell disc on a r68 pool is a stone in a pond, which
    // is what this is meant to look like.
    const int w3BlastR = std::min(kMaxExplosionRadius, 20);
    const int w3SwimQ = (int)BaselineNumber("waterbodyW3SwimKnob", 4096.0);
    const int w3SinkQ = (int)BaselineNumber("waterbodyW3SinkKnob", 4096.0);

    // One world per arm, built the way pass S builds its: same lake, no test
    // tap, no MPM, the relevel and the wave both armed explicitly (the shipped
    // tuning arms neither, and a pass that inherited either would be a green
    // light about a feature that never ran).
    auto armTuning = [&](int blast, int swim, int sink, int drainMax) {
      Tuning w = t;
      w.sim.waterBodyTestDrain = 0;
      w.sim.drainMaxEighthsPerTick = drainMax;
      w.sim.fluidExciteMode = 0;
      w.sim.drainExciteRadius = 0;
      w.sim.fluidSplashRate = 0.0f;
      w.sim.waterBodyMode = 1;
      w.sim.waterRelevelMax =
          t.sim.waterRelevelMax > 0 ? t.sim.waterRelevelMax : 4;
      w.sim.waveMode = 1;
      w.sim.waveBlastImpulse = blast;
      w.sim.waveSwimWake = swim;
      w.sim.waveDrainSink = sink;
      return w;
    };

    // ---- ARM 1+2: the two CPU-door sources, four runs ---------------------
    //
    // Each run: rebuild, settle, snapshot the box, fire the source ONCE, sample
    // Sigma|q| per tick, settle, re-measure. The two OFF runs are the controls
    // and their `peak` must be exactly 0 — not "small", zero: at knob 0 the
    // emitter queues no record at all, so TickParams::waterImpulseCount stays 0
    // and wbFlux's loop does not execute. Anything else means the identity is
    // not an identity.
    struct ImpArm {
      const char* name;
      bool swim;        // false = blast
      int knob;
    };
    const ImpArm w3Arms[4] = {
        {"blast off", false, 0},
        {"blast on", false, t.sim.waveBlastImpulse > 0 ? t.sim.waveBlastImpulse
                                                       : 3072},
        {"wake off", true, 0},
        {"wake on", true, w3SwimQ},
    };
    int64_t w3Peak[4] = {0, 0, 0, 0};
    int64_t w3Err[4] = {0, 0, 0, 0};
    int w3BadBin[4] = {-1, -1, -1, -1};
    uint32_t w3Queued[4] = {0, 0, 0, 0};
    uint32_t w3Shipped[4] = {0, 0, 0, 0};
    bool w3Wrote[4] = {false, false, false, false};
    uint32_t w3Pf[4] = {0, 0, 0, 0};
    std::string w3Bins[4];
    for (int a = 0; a < 4; a++) {
      const ImpArm& arm = w3Arms[a];
      WaterBodies().Reset();
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      SetCurrentTuning(armTuning(arm.swim ? 0 : arm.knob,
                                 arm.swim ? arm.knob : 0, 0, 0));
      tick = RunQuietTicks(c, tick, 130);
      const WaterBodyDesc* wd = WaterBodies().Find(LakeId());
      if (!wd || wd->gpuSlot >= kWaterBodyCap) {
        fail(Format("pass T (%s): the authored lake is not proposed", arm.name));
        continue;
      }
      const uint32_t wSlot = wd->gpuSlot;
      {
        const LedgerView lv = ReadLedger(c);
        if (lv.At(wSlot, WBS_STATE) != WB_ADOPTED) {
          fail(Format("pass T (%s): the lake is %s, not adopted, before the "
                      "impulse",
                      arm.name, LedgerStateName(lv.At(wSlot, WBS_STATE))));
          continue;
        }
      }
      const int wBoxLo = lakeGeo.floorY - kCraterDepth;
      const int wBoxHi = lakeGeo.surfY;
      const VoxelTruth v0 =
          SweepBasin(c, lakeGeo, lakeDesc, matId, wBoxLo, wBoxHi);
      uint32_t wPf0[4] = {0, 0, 0, 0};
      ReadPageFaultsSync(c.ctx, world, wPf0);

      // THE EVENT. Queued on the CPU exactly as the game queues it, then the
      // very next SubmitTick consumes it (WaterBodySystem::Tick) and ships it
      // in TickParams. Nothing is written to the world.
      if (arm.swim) {
        // One voxel per tick due east, fully submerged: a swimmer at a
        // plausible speed rather than a maximum.
        WaterBodyNoteSwimmer(WaterBodies(), lakeGeo.cx, lakeGeo.cz, 1.0f, 0.0f,
                             1.0f, arm.knob);
      } else {
        const ExplosionOp w3Exp{lakeGeo.cx, lakeGeo.surfY, lakeGeo.cz, w3BlastR,
                                256, 0, 0, 0};
        WaterBodyNoteBlast(WaterBodies(), w3Exp, arm.knob);
      }
      w3Queued[a] = (uint32_t)WaterBodies().PendingImpulses().size();

      int64_t binMax = 0;
      uint32_t inBin = 0;
      std::vector<int64_t> bins;
      for (uint32_t i = 0; i < w3Ticks; i++) {
        tick = RunQuietTicks(c, tick, 1);
        // THREE STAGES, THREE NUMBERS (CLAUDE.md rule 6). A bare "Sigma|q| was
        // 0" cost two runs: the record was queued, the uniform carried it, and
        // the tick it landed on had no measured heightfield under it. So the
        // SHIPPED count is recorded separately from the QUEUED count and from
        // the flux, and the three together say which of the three stages
        // dropped it. `Gpu().impulseCount` is what the last Tick() put in
        // TickParams, and it survives until the next one.
        w3Shipped[a] += WaterBodies().Gpu().impulseCount;
        w3Wrote[a] = w3Wrote[a] || WaterBodies().Gpu().writesThisTick;
        const LedgerView lvi = ReadLedger(c);
        const int64_t qs = lvi.At(wSlot, WBS_WVSUM_W);
        if (qs > w3Peak[a]) w3Peak[a] = qs;
        binMax = std::max(binMax, qs);
        if (++inBin >= w3Bin) {
          bins.push_back(binMax);
          binMax = 0;
          inBin = 0;
        }
      }
      if (inBin > 0) bins.push_back(binMax);
      tick = RunQuietTicks(c, tick, w3Settle);
      const VoxelTruth v1 =
          SweepBasin(c, lakeGeo, lakeDesc, matId, wBoxLo, wBoxHi);
      const LedgerView lv1 = ReadLedger(c);
      uint32_t wFa[kFluidArgsWords] = {};
      ReadFluidArgsSync(c.ctx, world, wFa);
      uint32_t wPf1[4] = {0, 0, 0, 0};
      ReadPageFaultsSync(c.ctx, world, wPf1);
      w3Pf[a] = wPf1[0] - wPf0[0];
      const int64_t inFlight =
          (int64_t)wFa[7] - (int64_t)std::min(wFa[29], wFa[7]);
      // Pass S's identity, term for term. An impulse is added BEFORE the
      // outflow clamp, so it can change WHERE water is and never HOW MUCH —
      // and this is the line that says so.
      w3Err[a] = (int64_t)v1.eighths + inFlight + lv1.At(wSlot, WBS_DRAINED) -
                 lv1.At(wSlot, WBS_DEBIT) +
                 (lv1.At(wSlot, WBS_RVCREDIT_W) + lv1.At(wSlot, WBS_RVGIVEN_W) -
                  lv1.At(wSlot, WBS_RVTAKEN_W)) -
                 (int64_t)v0.eighths;
      // Dissipation, binned for pass S's stated reason (a reflected ring
      // focuses and can lift the sum for a tick).
      int peakBin = -1;
      for (size_t i = 0; i < bins.size(); i++)
        if (peakBin < 0 || bins[i] > bins[(size_t)peakBin]) peakBin = (int)i;
      for (size_t i = (size_t)std::max(peakBin, 0) + 1;
           peakBin >= 0 && i < bins.size(); i++) {
        if ((double)bins[i] >
            (double)bins[i - 1] * (1.0 + w3RiseTolPct / 100.0)) {
          w3BadBin[a] = (int)i;
          break;
        }
      }
      for (size_t i = 0; i < bins.size() && i < 12; i++) {
        if (i) w3Bins[a] += ",";
        w3Bins[a] += std::to_string((long long)bins[i]);
      }
    }
    for (int a = 0; a < 4; a++) {
      const ImpArm& arm = w3Arms[a];
      const bool on = arm.knob > 0;
      if (!on) {
        // THE IDENTITY. Exactly zero, both halves: nothing queued and no pipe
        // ever moved.
        if (w3Queued[a] != 0 || w3Shipped[a] != 0)
          fail(Format("pass T (%s): the emitter queued %u impulses at knob 0 "
                      "and shipped %u — the off switch is not an identity, it "
                      "is a cheap path",
                      arm.name, w3Queued[a], w3Shipped[a]));
        if (w3Peak[a] != 0)
          fail(Format(
              "pass T (%s): Sigma|q| reached %lld with the knob at 0. Nothing "
              "in this fixture disturbs the lake but the impulse, so a non-zero "
              "peak means the control arm is not a control",
              arm.name, (long long)w3Peak[a]));
      } else {
        if (w3Queued[a] != 1)
          fail(Format("pass T (%s): the emitter queued %u impulses, expected 1 "
                      "— the record was refused before it reached TickParams",
                      arm.name, w3Queued[a]));
        // EXACTLY ONE TICK'S WORTH. An impulse is one push; a shipment counted
        // over sixty ticks that is not 1 means the record was re-sent, which
        // would make a splash a standing force.
        if (w3Shipped[a] != 1)
          fail(Format("pass T (%s): the impulse was shipped on %u ticks, "
                      "expected exactly 1 — an impulse is ONE tick's push, and "
                      "a record re-sent every tick is a standing force with a "
                      "knob on it",
                      arm.name, w3Shipped[a]));
        if (w3Peak[a] <= 0)
          fail(Format(
              "pass T (%s): Sigma|q| was 0 for the whole window at knob %d "
              "(%u queued, %u shipped in TickParams, footprint declared: %s). "
              "Those three numbers name the stage: 0 queued is the emitter's "
              "refusal path, 0 shipped is BuildImpulses or the mode-0 clear, "
              "shipped-but-no-flux with no footprint is the hot latch, and "
              "shipped-with-a-footprint is wbFlux itself (wvEligible's stamp, "
              "or wvBodyAwake)",
              arm.name, arm.knob, w3Queued[a], w3Shipped[a],
              w3Wrote[a] ? "yes" : "no"));
        if (w3BadBin[a] >= 0)
          fail(Format("pass T (%s): Sigma|q| ROSE after its peak at bin %d "
                      "[%s]. An impulse is one tick's push; a heightfield that "
                      "keeps gaining momentum after it is creating energy",
                      arm.name, w3BadBin[a], w3Bins[a].c_str()));
      }
      if (w3Err[a] < -w3Slack || w3Err[a] > w3Slack)
        fail(Format("CONSERVATION (pass T, %s): the lake is off by %+lld "
                    "eighths. An impulse is added BEFORE the outflow clamp, so "
                    "it can move water sideways and never create it",
                    arm.name, (long long)w3Err[a]));
      if (w3Pf[a] != 0)
        fail(Format("pass T (%s): %u page faults — an impulse pushed a write "
                    "into a sentinel chunk",
                    arm.name, w3Pf[a]));
    }

    // ---- ARM 3: the drain sink -------------------------------------------
    //
    // A DIFFERENTIAL AND NOT AN IDENTITY, because the fixture itself disturbs
    // the surface: a discharge shaves the lake whether or not the sink is on,
    // so Sigma|q| is non-zero in both arms and the claim has to be that the
    // sink adds to it. What IS exact is the other half — the sink is generated
    // GPU-side from WBS_EMIT, so an arm where the fixture never discharged
    // proves nothing at all and says so.
    //
    // PASS H'S PUNCH, cell for cell (the same shaft, the same sealed chamber,
    // the same constants). A second geometry for the same question is a second
    // thing to keep in step, and this one is already known to make a hole the
    // detector finds.
    int64_t w3SinkQsum[2] = {0, 0};
    int64_t w3SinkEmit[2] = {0, 0};
    for (int a = 0; a < 2; a++) {
      const int knob = a == 0 ? 0 : w3SinkQ;
      WaterBodies().Reset();
      SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
      SetCurrentTuning(armTuning(0, 0, knob, 512));
      tick = RunQuietTicks(c, tick, 130);
      const WaterBodyDesc* wd = WaterBodies().Find(LakeId());
      if (!wd || wd->gpuSlot >= kWaterBodyCap) {
        fail(Format("pass T (sink %d): the authored lake is not proposed", knob));
        continue;
      }
      const uint32_t wSlot = wd->gpuSlot;
      const int chTop = lakeGeo.floorY - kShaftDepth;
      const int chBot = chTop - kChamberH;
      std::vector<CellOp> punch;
      for (int y = chBot; y <= lakeGeo.floorY; y++) {
        const bool inShaft = y > chTop;
        const int half = inShaft ? kShaftR : kChamberR;
        for (int z = lakeGeo.cz - half; z <= lakeGeo.cz + half; z++)
          for (int x = lakeGeo.cx - half; x <= lakeGeo.cx + half; x++) {
            const bool wall =
                !inShaft && (y == chBot ||
                             std::abs(x - lakeGeo.cx) == kChamberR ||
                             std::abs(z - lakeGeo.cz) == kChamberR);
            punch.push_back({World::SlotCellIndex({x, y, z}),
                             wall ? (uint32_t)kMatStone : 0u});
          }
      }
      SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, punch, false,
                 c.world.WindowOrigin(), true, false);
      c.ctx.ProcessEvents();
      tick++;
      for (uint32_t i = 0; i < w3Ticks; i++) {
        tick = RunQuietTicks(c, tick, 1);
        const LedgerView lvi = ReadLedger(c);
        w3SinkQsum[a] += lvi.At(wSlot, WBS_WVSUM_W);
        w3SinkEmit[a] += lvi.At(wSlot, WBS_EMIT);
      }
    }
    for (int a = 0; a < 2; a++) {
      if (w3SinkEmit[a] <= 0)
        fail(Format(
            "pass T (sink %s) proves nothing: the punched shaft never "
            "discharged a single eighth over %u ticks, so WBS_EMIT was 0 and "
            "the sink branch was never reached whatever the knob said",
            a == 0 ? "off" : "on", w3Ticks));
    }
    if (w3SinkEmit[0] > 0 && w3SinkEmit[1] > 0 &&
        w3SinkQsum[1] <= w3SinkQsum[0])
      fail(Format(
          "pass T (sink): Sigma|q| over the drain window is %lld with the sink "
          "at %d against %lld with it at 0 — the knob reached no kernel. The "
          "sink is a TUNE_ const (TUNE_WAVE_DRAIN_SINK), so a stale pipeline "
          "cache or a missing tuning_params.def row are the two candidates "
          "before the kernel itself",
          (long long)w3SinkQsum[1], w3SinkQ, (long long)w3SinkQsum[0]));

    RecordObserved("waterbodyW3BlastPeak", (double)w3Peak[1]);
    RecordObserved("waterbodyW3WakePeak", (double)w3Peak[3]);
    RecordObserved("waterbodyW3SinkQOn", (double)w3SinkQsum[1]);
    RecordObserved("waterbodyW3SinkQOff", (double)w3SinkQsum[0]);
    RecordObserved("waterbodyW3SinkEmit", (double)w3SinkEmit[1]);
    // ONE LINE, EVERY TERM (rule 6): "W3 did nothing" is four different bugs —
    // the emitter refused the record, the uniform never carried it, the kernel
    // never read it, or the body was asleep and stayed asleep.
    w3Note = Format(
        "W3(blast r%d, wake 1 vox/tick, sink knob %d) blast Sigma|q| peak "
        "%lld on / %lld off (%u queued / %u shipped on) | wake peak %lld on / "
        "%lld off (%u queued / %u shipped on) | sink Sigma|q| %lld on / %lld "
        "off over %u ticks, emitted %lld eighths | identities "
        "%+lld/%+lld/%+lld/%+lld eighths | %u/%u/%u/%u page faults",
        w3BlastR, w3SinkQ, (long long)w3Peak[1], (long long)w3Peak[0],
        w3Queued[1], w3Shipped[1], (long long)w3Peak[3], (long long)w3Peak[2],
        w3Queued[3], w3Shipped[3],
        (long long)w3SinkQsum[1], (long long)w3SinkQsum[0], w3Ticks,
        (long long)w3SinkEmit[1], (long long)w3Err[0], (long long)w3Err[1],
        (long long)w3Err[2], (long long)w3Err[3], w3Pf[0], w3Pf[1], w3Pf[2],
        w3Pf[3]);

    // Leave the world settled and pristine for the passes that hash it.
    SetCurrentTuning(t);
    WaterBodies().Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    tick = RunQuietTicks(c, tick, 60);
  }

  mark("pass B");
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

    const WaterBodyDesc* pd = WaterBodies().Find(LakeId());
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
        fail(Format("pass B: %u descriptors are adopted over the harness lake, not 2. "
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
            "holds %lld = %lld eighths, but the voxels of the harness lake sum to %lld "
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

  mark("pass F");
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

  mark("pass D");
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
  detail += " | " + relevelNote;
  detail += " | " + discoverNote;
  detail += " | " + sloshNote;
  detail += " | " + w3Note;
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
