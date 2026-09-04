// selftest_floaters.cpp — the `floaters` gate.
//
// THE INVARIANT: no solid voxel is left stranded in mid-air by the
// grid <-> rigidbody handoff.
//
// WHY IT NEEDS A GATE OF ITS OWN. A CLASS_SOLID voxel never moves in the CA —
// sim_step returns early for it — so island detection is the ONLY mechanism
// that makes solid matter fall, and there is no second line of defence behind
// it. Every component that path declines to convert stays exactly where it is,
// permanently, and nothing else in the suite can see that: `debris` asserts a
// body was MADE, `settle-back` asserts a body CONVERTED. Neither asks the
// complementary question — what did the handoff leave behind?
//
// WHY IT REPORTS CAUSES AND NOT JUST A COUNT. "N things are floating" is the
// exact shape of bare number CLAUDE.md rule 6 says not to bisect: there are
// eight distinct doors matter can leave by, and elimination buys one hypothesis
// per run. DebrisSystem::FloaterProbe records the cause AT THE SITE, and this
// gate prints those counters beside its own sweep — so a red run names its own
// reason instead of starting a hunt.
//
// THREE PASSES, and the second exists because of the first. Pass A proves a
// body with nothing under it refuses to settle; on its own that assertion is
// satisfied just as well by a build that never settles anything at all, which
// is the circular-probe trap this repo has paid for before. Pass B is the
// control: the same block, on the ground, must still settle. Pass C then sweeps
// the world the first two left behind and asserts nothing is hanging in it.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- the sweep -------------------------------------------------------------
//
// Bounded, and it has to be: the CPU chunk mirror only holds what has been
// FETCHED (3x3x3 around the player plus whatever ManageTerrain pulled in around
// bodies), so a window-wide sweep would mostly read absent chunks and report
// confident nonsense about them. The box below is the fixture's own
// neighbourhood, which is exactly the region this gate disturbed and therefore
// the only region it is entitled to draw a conclusion about.
//
// SUPPORT, as this sweep defines it: a 6-connected solid component is supported
// if any of its cells has, directly beneath it, either a solid/powder cell that
// is NOT part of the component, or the bottom face of the box (we cannot see
// below that, so the conservative answer is "supported"). Everything else is
// resting on air.
//
// That is deliberately a weaker question than island detection asks — it says
// nothing about lateral attachment — but it is the right one HERE. A floater is
// a thing with nothing under it, and a test that re-implemented the anchoring
// rules would be asserting that RunIslandDetection agrees with a second copy of
// itself rather than that the world looks right.
struct FloatComp {
  size_t voxels = 0;
  IVec3 lo{}, hi{};
  uint32_t domMat = 0;
};

struct SweepResult {
  uint32_t components = 0;   // unsupported components at or over the size floor
  // Every solid component the flood found, supported or not. THE POSITIVE
  // CONTROL: "0 floaters" is also what a sweep that silently read nothing
  // reports, and those two are indistinguishable from the verdict alone. A
  // fixture that has just built a stone pad must find components; if this is
  // zero the sweep is broken, not the world clean.
  uint32_t componentsTotal = 0;
  uint32_t voxels = 0;       // their total voxel count
  uint32_t cellsScanned = 0;
  uint32_t chunksMissing = 0;
  std::vector<FloatComp> worst;  // largest few, for the detail line
};

SweepResult SweepForFloaters(World& world, const std::vector<MaterialDef>& mats,
                             IVec3 lo, IVec3 hi, size_t minVoxels) {
  SweepResult r;
  const int dx = hi.x - lo.x + 1, dy = hi.y - lo.y + 1, dz = hi.z - lo.z + 1;
  const size_t vol = (size_t)dx * dy * dz;
  auto lidx = [&](int x, int y, int z) {
    return (size_t)((z * dy + y) * dx + x);
  };

  // Class lookup by material id, built from the AUTHORED table rather than
  // borrowed from DebrisSystem, so the gate cannot inherit a mistake from the
  // system it is testing.
  std::vector<uint8_t> klass(mats.size() + 1, 0);
  for (size_t i = 0; i < mats.size(); i++) klass[i] = (uint8_t)mats[i].gpu.klass;

  std::vector<uint16_t> mat(vol, 0);
  std::vector<uint8_t> solid(vol, 0), support(vol, 0);
  for (int z = 0; z < dz; z++)
    for (int y = 0; y < dy; y++) {
      const int wy = lo.y + y, wz = lo.z + z;
      const CachedChunk* cc = nullptr;
      int ccx = INT32_MIN;
      for (int x = 0; x < dx; x++) {
        const int wx = lo.x + x;
        if ((wx >> 4) != ccx) {
          ccx = wx >> 4;
          IVec3 wc{ccx, wy >> 4, wz >> 4};
          cc = world.ChunkInWindow(wc) ? world.Cached(wc) : nullptr;
          if (cc && cc->voxels.size() != kChunkVol) cc = nullptr;
          if (!cc) r.chunksMissing++;
        }
        if (!cc) continue;
        const uint32_t w =
            cc->voxels[((((uint32_t)wz) & 15u) * kChunk + (((uint32_t)wy) & 15u)) *
                           kChunk +
                       (((uint32_t)wx) & 15u)];
        const uint32_t m = w & 0xFFFu;
        r.cellsScanned++;
        if (m == 0 || m >= klass.size()) continue;
        mat[lidx(x, y, z)] = (uint16_t)m;
        solid[lidx(x, y, z)] = klass[m] == CLASS_SOLID ? 1 : 0;
        support[lidx(x, y, z)] =
            (klass[m] == CLASS_SOLID || klass[m] == CLASS_POWDER) ? 1 : 0;
      }
    }

  // 6-connected components over the solids: the same shape of flood
  // RunIslandDetection runs, asking the resting question instead of the
  // anchoring one.
  std::vector<uint8_t> seen(vol, 0);
  std::vector<size_t> stack, cells;
  for (size_t seed = 0; seed < vol; seed++) {
    if (!solid[seed] || seen[seed]) continue;
    cells.clear();
    stack.assign(1, seed);
    seen[seed] = 1;
    bool rests = false;
    while (!stack.empty()) {
      const size_t i = stack.back();
      stack.pop_back();
      cells.push_back(i);
      const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                z = (int)(i / ((size_t)dx * dy));
      if (y == 0) {
        rests = true;  // on the floor of the box: we cannot see below it
      } else if (support[lidx(x, y - 1, z)] && !solid[lidx(x, y - 1, z)]) {
        rests = true;  // powder underneath is real support, and is not in comp
      }
      const int nb[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                            {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (auto& d : nb) {
        const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz)
          continue;
        const size_t ni = lidx(nx, ny, nz);
        if (solid[ni] && !seen[ni]) {
          seen[ni] = 1;
          stack.push_back(ni);
        }
      }
    }
    // "Solid underneath, but belonging to a DIFFERENT component" is only
    // answerable once the component is known, which is why it is a second pass
    // rather than folded into the flood above.
    if (!rests) {
      std::unordered_set<size_t> mine(cells.begin(), cells.end());
      for (size_t i : cells) {
        const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                  z = (int)(i / ((size_t)dx * dy));
        if (y == 0) {
          rests = true;
          break;
        }
        const size_t b = lidx(x, y - 1, z);
        if (support[b] && !mine.count(b)) {
          rests = true;
          break;
        }
      }
    }
    r.componentsTotal++;
    if (rests || cells.size() < minVoxels) continue;

    FloatComp fc;
    fc.voxels = cells.size();
    fc.lo = IVec3{INT32_MAX, INT32_MAX, INT32_MAX};
    fc.hi = IVec3{INT32_MIN, INT32_MIN, INT32_MIN};
    std::unordered_map<uint32_t, uint32_t> hist;
    for (size_t i : cells) {
      const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                z = (int)(i / ((size_t)dx * dy));
      fc.lo.x = std::min(fc.lo.x, lo.x + x);
      fc.hi.x = std::max(fc.hi.x, lo.x + x);
      fc.lo.y = std::min(fc.lo.y, lo.y + y);
      fc.hi.y = std::max(fc.hi.y, lo.y + y);
      fc.lo.z = std::min(fc.lo.z, lo.z + z);
      fc.hi.z = std::max(fc.hi.z, lo.z + z);
      hist[mat[i]]++;
    }
    for (const auto& kv : hist)
      if (kv.second > hist[fc.domMat]) fc.domMat = kv.first;
    r.components++;
    r.voxels += (uint32_t)cells.size();
    r.worst.push_back(fc);
  }
  std::sort(r.worst.begin(), r.worst.end(),
            [](const FloatComp& a, const FloatComp& b) {
              return a.voxels > b.voxels;
            });
  if (r.worst.size() > 3) r.worst.resize(3);
  return r;
}

// One tick of the harness loop, in the shape every world-touching phys gate
// uses it. Local so the gate body reads as the experiment, not the plumbing.
void RunTick(Ctx& c, uint32_t& t, IVec3 playerChunk, uint64_t keepAsleep = 0) {
  std::vector<CellOp> cellOps;
  std::vector<ParticleSpawn> spawns;
  c.debris.PreTick(t + 1, c.world, cellOps, spawns);
  ++t;
  // playerChunk is the FIXTURE's chunk, not a literal. The CPU voxel mirror is
  // 3x3x3 chunks around the player (DESIGN.md section 3), and this gate's sweep
  // reads that mirror — so parking the notional player at a hardcoded {5,*,5}
  // left the mirror 20 chunks away from everything the gate built. Measured at
  // suite scope: "0 cells scanned, 2226 absent".
  SubmitTick(c.ctx, c.world, c.sim, t, kDefaultSeed, {}, {}, cellOps, false,
             playerChunk, true, false, spawns);
  c.ctx.WaitIdle();
  c.ctx.ProcessEvents();
  c.phys.Step(kTickDt);
  c.debris.PostStep();
  // Hold one body asleep, which is the whole precondition of the bug pass A
  // is about. A sleeping Jolt body does not fall — that is what makes "asleep
  // with nothing underneath" a state the world can genuinely sit in — but the
  // harness wakes bodies constantly (ManageTerrain's WakeNear fires whenever a
  // collision surface near them changes, and this fixture is writing a stone
  // pad into the world). Left to itself the block simply woke up, fell the 40
  // voxels to the pad and settled there legitimately, which is what the first
  // run of this gate measured: "air-block settled 1, refused 0" with a clean
  // sweep. Re-deactivating each tick reproduces the real state — a body that
  // stays asleep over a void — rather than a body that falls out of one.
  if (keepAsleep) c.phys.DeactivateBody(keepAsleep);
}

// A 3x3x3 painted stone block — the same fixture settle-back drops.
std::vector<DebrisVoxel> StoneBlock() {
  std::vector<DebrisVoxel> vox;
  for (int z = 0; z < 3; z++)
    for (int y = 0; y < 3; y++)
      for (int x = 0; x < 3; x++)
        vox.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 1u, kMatStone});
  return vox;
}

Status GateFloaters(Ctx& c, std::string& detail) {
  World& world = c.world;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;

  std::string failed;
  auto note = [&failed](bool ok, const char* name) {
    if (!ok) failed += failed.empty() ? name : (std::string(", ") + name);
    return ok;
  };

  debris.Reset();
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  // ---- WHERE THIS GATE STANDS, and why it is not a literal ---------------
  //
  // Anchored to the residency window, exactly as FixtureSite does for the
  // wound gates and for the same reason their comment gives: by the time this
  // runs, `streaming` has walked the window origin ~20 chunks along x. A
  // hardcoded column then lands OUTSIDE the window, where world writes are
  // silently dropped and a body despawns the moment it is created.
  //
  // This gate shipped with a literal (80, 80) and passed under `--gate
  // floaters`, where nothing has moved the window, then failed all three of
  // its assertions under `--selftest` — sweep 0 cells of 2226 absent,
  // ground-block settled 0, air-block refused 0. Not one of those was the
  // subject of the gate: the pad was never written, so there was nothing to
  // stand on, nothing to sweep, and no body left alive to ask about. CLAUDE.md
  // rule 7, and the exact trap selftest.h's ordering note opens with.
  const IVec3 org = world.WindowOrigin();
  const int fx = org.x * (int)kChunk + 200;
  const int fz = org.z * (int)kChunk + 200;
  const int h = World::TerrainHeight(fx, fz, kDefaultSeed);
  const IVec3 fixtureChunk{fx >> 4, h >> 4, fz >> 4};
  // If this is ever false the gate is measuring nothing, and it must say so
  // rather than report a clean sweep of a region that does not exist.
  const bool siteInWindow = world.ChunkInWindow(fixtureChunk);
  uint32_t t = 21000;

  // A flat pad, for the reason settle-back builds one: the control arm has to
  // LAND SQUARE, and procedural terrain at a fixed (x,z) is a slope worldgen is
  // free to move under this gate.
  const int padY = h + 2;
  {
    std::vector<CellOp> pad;
    for (int z = -6; z <= 6; z++)
      for (int x = -6; x <= 6; x++)
        for (int y = padY - 3; y <= padY; y++)
          pad.push_back(
              {World::SlotCellIndex({fx + x, y, fz + z}), (uint32_t)kMatStone});
    SubmitTick(c.ctx, world, c.sim, t, kDefaultSeed, {}, {}, pad, false,
               fixtureChunk, true, false, {});
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
  }

  std::vector<float> dens;
  for (const auto& m : c.mats) dens.push_back((float)m.gpu.density);

  // ---- PASS A: a body with nothing under it must NOT settle ---------------
  //
  // Built ASLEEP on purpose. That is not a contrivance — it is exactly the
  // state worldio recreates a saved body in (Physics::DeactivateBody, so that a
  // settled pile reloads settled), which means a world saved mid-fall arrives
  // with its inactive clock already running. Before the support test this block
  // stamped itself into the sky on the first scan that noticed it, and because
  // no CA cell ever vacated to do it, no support-loss flag could ever fire and
  // nothing would look at it again.
  const uint32_t refusedBefore = debris.Floaters().settleWithoutSupport;
  const uint32_t settledBeforeA = debris.SettledBack();
  const int airY = padY + 40;  // open air all the way down to the pad
  uint64_t airHandle = 0;
  {
    std::vector<DebrisVoxel> vox = StoneBlock();
    airHandle = phys.CreateDebrisBody(vox, {fx, airY, fz}, dens);
    BodyTransform bxf{};
    bxf.pos = Vec3{(float)fx, (float)airY, (float)fz};
    bxf.quat[3] = 1;
    debris.AdoptBody(airHandle, vox, bxf);
    phys.DeactivateBody(airHandle);
  }
  for (int i = 0; i < 200; i++) RunTick(c, t, fixtureChunk, airHandle);

  const uint32_t settledA = debris.SettledBack() - settledBeforeA;
  const uint32_t refusedA =
      debris.Floaters().settleWithoutSupport - refusedBefore;
  const uint32_t bodiesAfterA = debris.BodyCount();
  // It must not have converted, it must still BE a body, and the refusal must
  // be the reason — a block that merely fell out of the window would satisfy
  // the first two on its own.
  const bool passA = note(settledA == 0 && bodiesAfterA >= 1 && refusedA >= 1,
                          "no-settle-in-air");

  // ---- PASS B: the control. On the ground, it must still settle ----------
  //
  // Without this arm pass A is vacuous: "refused to settle in the air" is
  // satisfied perfectly by a build that refuses to settle anything, and a
  // one-armed assertion about a refusal cannot tell the two apart. Same block,
  // same code path; the only variable is what is underneath it.
  debris.Reset();
  const uint32_t settledBeforeB = debris.SettledBack();
  {
    std::vector<DebrisVoxel> vox = StoneBlock();
    const int restY = padY + 1;  // directly on the pad
    uint64_t bh = phys.CreateDebrisBody(vox, {fx, restY, fz}, dens);
    BodyTransform bxf{};
    bxf.pos = Vec3{(float)fx, (float)restY, (float)fz};
    bxf.quat[3] = 1;
    debris.AdoptBody(bh, vox, bxf);
  }
  for (int i = 0; i < 360 && debris.BodyCount() > 0; i++)
    RunTick(c, t, fixtureChunk);
  const uint32_t settledB = debris.SettledBack() - settledBeforeB;
  const bool passB =
      note(settledB >= 1 && debris.BodyCount() == 0, "supported-still-settles");

  // ---- PASS C: sweep the world those two left behind ---------------------
  //
  // The box spans the pad and everything above it, up past where pass A parked
  // its block — so if that block HAD stamped itself into the sky, this sweep is
  // what finds it. The two halves check each other from opposite ends.
  const size_t minVoxels = (size_t)BaselineNumber("floaters.minVoxels", 4);
  const SweepResult sw =
      SweepForFloaters(world, c.mats, {fx - 10, padY - 4, fz - 10},
                       {fx + 10, airY + 8, fz + 10}, minVoxels);
  const uint32_t maxComponents =
      (uint32_t)BaselineNumber("floaters.maxComponents", 0);
  const bool passC = note(sw.components <= maxComponents, "world-sweep-clear") &&
                     note(sw.componentsTotal > 0, "sweep-saw-something") &&
                     note(siteInWindow, "fixture-in-window");

  RecordObserved("floaters.componentsObserved", (double)sw.components);
  RecordObserved("floaters.voxelsObserved", (double)sw.voxels);

  // The probe, cumulative over the whole run. Printed unconditionally, because
  // these are the numbers that make a red verdict diagnosable — and a HEALTHY
  // run wants them too: a rising `deferred*` is the repair working (matter held
  // in the grid on purpose, region re-queued), while a rising `gaveUp` or
  // `in-place` is matter that got away.
  const DebrisSystem::FloaterProbe& fp = debris.Floaters();
  std::string worst;
  for (const FloatComp& f : sw.worst)
    worst += Format("%s%zu vox mat %u at (%d,%d,%d)..(%d,%d,%d)",
                    worst.empty() ? "" : "; ", f.voxels, f.domMat, f.lo.x,
                    f.lo.y, f.lo.z, f.hi.x, f.hi.y, f.hi.z);

  const bool ok = passA && passB && passC;
  detail = Format(
      "at (%d,%d) org (%d,%d) inWindow %d; "
      "sweep %u unsupported of %u comps / %u vox over %u cells (%u absent)"
      "%s%s%s; air-block settled %u (refused %u), ground-block settled %u; "
      "leaks: oversize %u, solid-rubble-in-place %u, stuck-event %u, "
      "defer-gaveup %u, queue-dropped %u; deferrals: cellop %u, spawn-ring %u, "
      "drain-backpressure %u, "
      "oversize %u, spilled %u, settle-unsupported %u; anchors: boundary %u, "
      "unknown %u, oversize-flood %u%s%s",
      fx, fz, org.x, org.z, siteInWindow ? 1 : 0, sw.components,
      sw.componentsTotal, sw.voxels, sw.cellsScanned, sw.chunksMissing,
      worst.empty() ? "" : " [", worst.c_str(), worst.empty() ? "" : "]",
      settledA, refusedA, settledB, fp.oversizeBboxSkipped,
      fp.solidRubbleInPlace, fp.stuckEventDropped, fp.deferGaveUp,
      fp.eventQueueFullDropped, fp.deferredCellOpBudget, fp.deferredSpawnRing,
      fp.drainBackpressure,
      fp.deferredOversize, fp.eventQueueFullSpilled, fp.settleWithoutSupport,
      fp.anchoredByRegionBoundary, fp.anchoredByUnknownChunk,
      fp.anchoredByOversizeFlood, failed.empty() ? "" : "; FAILED: ",
      failed.c_str());
  std::printf("floaters: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  // Leave the world as this gate found it. Every late world-touching gate in
  // this suite regenerates on the way out (CLAUDE.md rule 7), and this one
  // builds a stone pad and drops blocks on it — exactly the kind of leftover a
  // later gate's fixture placement would trip over.
  debris.Reset();
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& FloaterGates() {
  static const std::vector<Gate> g = {
      {"floaters", "phys", {}, false, GateFloaters},
  };
  return g;
}

}  // namespace selftest
