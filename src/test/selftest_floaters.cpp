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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sim/weather.h"  // SetOverride: tree-fell burns under a clear sky
#include "test/selftest.h"
#include "test/support.h"
#include "test/treefixture.h"

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

  // ---- SIZE HISTOGRAM, unfiltered ------------------------------------------
  //
  // `components` above is gated on `minVoxels`, which is right for the
  // `floaters` gate (it is asking about slabs) and wrong for `tree-fell`, whose
  // whole subject is the LONE VOXEL left hanging where a branch burned out from
  // under it. Those three sizes need completely different fixes — a single is
  // the sub-8 rubble handoff not reaching it, a 2..7 clump is the same door,
  // and a >=8 clump is a rigidbody that was never made — so collapsing them
  // into one number is the bare count CLAUDE.md rule 6 warns about. Counted for
  // EVERY unsupported component regardless of `minVoxels`, so the two gates can
  // ask their own questions of one sweep.
  uint32_t singles = 0;     // exactly 1 voxel: the "specks in the air" symptom
  // Of the singles: how many have NO solid/powder on any face (the CA's
  // soloSolid should have dropped these -- if any exist, its chunk never woke)
  // versus how many touch powder or liquid laterally (soloSolid correctly
  // leaves those to island detection, which then has to arrive).
  uint32_t singlesAllAir = 0;
  uint32_t singlesTouchPowder = 0;
  uint32_t singlesTouchLiquid = 0;
  uint32_t singlesTouchOther = 0;  // gas that is not air (fire, smoke)
  uint32_t smallComps = 0;  // 2..7: below kMinBodyVoxels, should have crumbled
  uint32_t bigComps = 0;    // >= 8: body-worthy, and still in the grid
  uint32_t bigVoxels = 0;   // ...and how much matter that is

  // ---- THE BIGGEST COMPONENT, AND WHY IT WAS LET OFF ----------------------
  //
  // "0 unsupported components" and "the sweep called the severed tree
  // supported" read identically from the verdict, and the `tree-fell` gate hit
  // exactly that: 3300 wood voxels still standing above a fully severed trunk
  // while this sweep reported nothing floating. A bare 0 cannot be bisected, so
  // the sweep records the decision at the point it makes it — which component
  // was largest, whether it rested, and on WHAT.
  size_t biggest = 0;        // its voxel count
  IVec3 biggestLo{}, biggestHi{};
  bool biggestRests = false;
  // 0 none, 1 box floor (cannot see below), 2 powder underneath,
  // 3 a foreign solid underneath, 4 afloat on a denser liquid
  int biggestRestWhy = 0;
  IVec3 biggestRestAt{};     // the cell of this component that was held up
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
  std::vector<int32_t> dens(mats.size() + 1, 0);
  for (size_t i = 0; i < mats.size(); i++) {
    klass[i] = (uint8_t)mats[i].gpu.klass;
    dens[i] = (int32_t)mats[i].gpu.density;
  }

  std::vector<uint16_t> mat(vol, 0);
  std::vector<uint8_t> solid(vol, 0), support(vol, 0);
  // FLOATING IS SUPPORT. A solid over a liquid denser than itself cannot go
  // anywhere: soloSolid's tryMove is refused by canDisplace, and a body dropped
  // onto it would bob. Counting only solid/powder underneath reported six
  // leaves sitting on water as "floating in the air", which is the one thing
  // they were not doing. So a cell over a denser liquid is supported; a cell
  // over a LIGHTER liquid (stone over water) is not, and is expected to sink.
  auto restsOnLiquid = [&](size_t me, size_t below) {
    const uint16_t bm = mat[below];
    if (bm == 0 || klass[bm] != CLASS_LIQUID) return false;
    const uint16_t mm = mat[me];
    return mm != 0 && dens[bm] >= dens[mm];
  };
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
    int restWhy = 0;
    IVec3 restAt{};
    while (!stack.empty()) {
      const size_t i = stack.back();
      stack.pop_back();
      cells.push_back(i);
      const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                z = (int)(i / ((size_t)dx * dy));
      if (y == 0) {
        if (!rests) { restWhy = 1; restAt = {lo.x + x, lo.y + y, lo.z + z}; }
        rests = true;  // on the floor of the box: we cannot see below it
      } else if (support[lidx(x, y - 1, z)] && !solid[lidx(x, y - 1, z)]) {
        if (!rests) { restWhy = 2; restAt = {lo.x + x, lo.y + y, lo.z + z}; }
        rests = true;  // powder underneath is real support, and is not in comp
      } else if (restsOnLiquid(i, lidx(x, y - 1, z))) {
        if (!rests) { restWhy = 4; restAt = {lo.x + x, lo.y + y, lo.z + z}; }
        rests = true;  // afloat
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
          restWhy = 1;
          restAt = {lo.x + x, lo.y + y, lo.z + z};
          break;
        }
        const size_t b = lidx(x, y - 1, z);
        if (support[b] && !mine.count(b)) {
          rests = true;
          restWhy = 3;
          restAt = {lo.x + x, lo.y + y, lo.z + z};
          break;
        }
      }
    }
    if (cells.size() > r.biggest) {
      r.biggest = cells.size();
      r.biggestRests = rests;
      r.biggestRestWhy = restWhy;
      r.biggestRestAt = restAt;
      r.biggestLo = IVec3{INT32_MAX, INT32_MAX, INT32_MAX};
      r.biggestHi = IVec3{INT32_MIN, INT32_MIN, INT32_MIN};
      for (size_t i : cells) {
        const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                  z = (int)(i / ((size_t)dx * dy));
        r.biggestLo.x = std::min(r.biggestLo.x, lo.x + x);
        r.biggestHi.x = std::max(r.biggestHi.x, lo.x + x);
        r.biggestLo.y = std::min(r.biggestLo.y, lo.y + y);
        r.biggestHi.y = std::max(r.biggestHi.y, lo.y + y);
        r.biggestLo.z = std::min(r.biggestLo.z, lo.z + z);
        r.biggestHi.z = std::max(r.biggestHi.z, lo.z + z);
      }
    }
    r.componentsTotal++;
    if (rests) continue;
    // Unfiltered histogram FIRST: `minVoxels` is one gate's question, not the
    // sweep's, and a single voxel must be counted before it is filtered out.
    if (cells.size() == 1) {
      r.singles++;
      const size_t i = cells[0];
      const int x = (int)(i % dx), y = (int)((i / dx) % dy),
                z = (int)(i / ((size_t)dx * dy));
      bool pow = false, liq = false, oth = false;
      const int nb6[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                             {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
      for (auto& d : nb6) {
        const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz)
          continue;
        const uint16_t m = mat[lidx(nx, ny, nz)];
        if (m == 0) continue;
        const uint8_t k = klass[m];
        if (k == CLASS_POWDER) pow = true;
        else if (k == CLASS_LIQUID) liq = true;
        else if (k != CLASS_SOLID) oth = true;
      }
      if (pow) r.singlesTouchPowder++;
      else if (liq) r.singlesTouchLiquid++;
      else if (oth) r.singlesTouchOther++;
      else r.singlesAllAir++;
    }
    else if (cells.size() < 8) r.smallComps++;
    else {
      r.bigComps++;
      r.bigVoxels += (uint32_t)cells.size();
    }
    if (cells.size() < minVoxels) continue;

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
      for (int x = -6; x <= 6; x++) {
        for (int y = padY - 3; y <= padY; y++)
          pad.push_back(
              {World::SlotCellIndex({fx + x, y, fz + z}), (uint32_t)kMatStone});
        // AND CLEAR THE AIR ABOVE IT. The pad is stone up to h+2, but worldgen
        // is free to have grown ground cover from h+1 upward on this column
        // (since the world map's P1 every biome's cover rows come from
        // assets/biomes/*.json and reach h+4 and beyond), and a block set down
        // on top of a fern never rests square, never sleeps, and never
        // settles -- which reads as "supported-still-settles" failing for a
        // reason that has nothing to do with support. The fixture owns its
        // footprint: pad below, open air above, exactly what the control arm
        // assumes.
        for (int y = padY + 1; y <= padY + 12; y++)
          pad.push_back(
              {World::SlotCellIndex({fx + x, y, fz + z}), (uint32_t)kMatAir});
      }
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

// ---------------------------------------------------------------------------
// tree-fell: the census the `floaters` gate cannot run
// ---------------------------------------------------------------------------
//
// WHY A SECOND GATE. `floaters` sweeps its OWN fixture box — a stone pad and
// two 27-voxel blocks — which is the right scope for the question it asks (did
// the settle path stamp something into the sky) and says nothing at all about
// the question the owner actually reports: burn a tree down and single voxels
// hang in the air all over, while sections that should be rigidbodies do not
// fall. The P0-P3 handoff wrote that down explicitly — "the suite's floater
// sweep runs inside the gate's own fixture box, so it says nothing about a
// played-in world; point the sweep at a real world before building P4" — and
// this is that sweep.
//
// WHY A BUILT TREE AND NOT A WORLDGEN ONE. Worldgen's trees are placed by a
// tile hash and their species, height and position move with the seed and with
// the residency window, which by this point in the suite has walked ~20 chunks
// (the `floaters` gate above paid for that lesson with a hardcoded site). A
// gate that first has to HUNT for a tree is a gate whose failures are usually
// about the hunt. So the fixture is authored, to the dimensions the real atlas
// actually reports:
//
//     species     height   reachXZ        (assets/trees/*.svtree header)
//     oak             88        54
//     birch           94        60
//     pine           121        47
//     great_oak      159       115
//     redwood        217        42
//
// The three caps this exercise is about are all in phys/debris.cpp, and every
// one of those trees crosses at least one of them:
//     kMaxRegionCells   80 cells/axis   the scan box cannot CONTAIN a tree
//     kMaxIslandVoxels  32000 voxels    the flood aborts and declares "anchored"
//     DebrisVoxel int8  +-120/axis      no single body can HOLD a tree
// The fixture below is oak-sized on purpose: tall enough (92) to cross the scan
// box on y, cheap enough to sweep and to burn inside a gate's time budget. A
// bigger one would prove nothing the small one does not, and would cost the
// suite a minute.
//
// WHAT IT REPORTS. Two passes, and both report causes rather than a verdict
// alone (CLAUDE.md rule 6): the sweep's size histogram says WHICH of the three
// handoff doors leaked (a single voxel is the sub-8 rubble path not reaching
// it; a >=8 clump is a rigidbody that was never made), and DebrisSystem's
// FloaterProbe says why the scan declined at the site that declined.

// Deterministic dither. Integer, seeded by cell, so the crown's rim is the same
// ragged shape on every run and on every machine — the same property worldgen's
// own trees have, and the reason a support scan near one finds hundreds of
// isolated leaf voxels.
uint32_t DitherHash(int x, int y, int z) {
  uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^
               (uint32_t)z * 0xCB1AB31Fu;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return h;
}

}  // namespace

// Author one tree as exact-cell ops. Trunk, four sloping limbs, and a dithered
// ellipsoid crown — the shape that matters here is not botanical accuracy but
// the two features that drive the symptom: a load-bearing trunk that can be cut
// or burned through, and a crown whose rim is ragged.
//
// AND THEN PRUNED TO ONE 6-CONNECTED COMPONENT, which is not decoration. The
// first version of this fixture dithered the crown rim down to 12% density and
// planted it as written — and the very first run reported 1067 floating single
// voxels before the fire had done any work at all, because a 12%-dense shell IS
// a cloud of isolated voxels. That number was real, but it measured the
// FIXTURE, not the handoff, and a gate whose baseline reading is its own
// construction noise can never tell the two apart afterwards. So the tree is
// flood-filled from its trunk foot and anything the flood does not reach is
// dropped: what gets planted is exactly one component, resting on the ground,
// with nothing in the air. Every floater the passes below find was therefore
// MADE by the fire or the axe, which is the whole claim.
TreeFixture BuildTree(const World& world, IVec3 base, uint32_t wood,
                      uint32_t leaves, std::vector<CellOp>& ops) {
  TreeFixture t;
  t.base = base;
  t.height = 92;
  t.crownR = 26;
  t.lo = IVec3{base.x - t.crownR - 1, base.y, base.z - t.crownR - 1};
  t.hi = IVec3{base.x + t.crownR + 1, base.y + t.height + t.crownR / 2 + 1,
               base.z + t.crownR + 1};

  // Local lattice, so the prune below is an array walk rather than a hash.
  const int LX = t.hi.x - t.lo.x + 1, LY = t.hi.y - t.lo.y + 1,
            LZ = t.hi.z - t.lo.z + 1;
  std::vector<uint16_t> cell((size_t)LX * LY * LZ, 0);
  auto at = [&](int x, int y, int z) {
    return (size_t)(((z - t.lo.z) * LY + (y - t.lo.y)) * LX + (x - t.lo.x));
  };
  auto inBox = [&](int x, int y, int z) {
    return x >= t.lo.x && x <= t.hi.x && y >= t.lo.y && y <= t.hi.y &&
           z >= t.lo.z && z <= t.hi.z;
  };
  auto put = [&](int x, int y, int z, uint32_t mat) {
    if (inBox(x, y, z)) cell[at(x, y, z)] = (uint16_t)mat;
  };

  // ---- trunk: a radius-3 column ----
  for (int y = 0; y <= t.height; y++)
    for (int dz = -3; dz <= 3; dz++)
      for (int dx = -3; dx <= 3; dx++)
        if (dx * dx + dz * dz <= 9)
          put(base.x + dx, base.y + y, base.z + dz, wood);

  // ---- four limbs, sloping up and out from the upper trunk ----
  const int dir[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (int L = 0; L < 4; L++) {
    const int y0 = t.height - 34 + L * 7;
    for (int s = 0; s <= 22; s++) {
      const int cx = base.x + dir[L][0] * s;
      const int cz = base.z + dir[L][1] * s;
      const int cy = base.y + y0 + s / 2;
      for (int dy = -1; dy <= 1; dy++)
        for (int dz = -1; dz <= 1; dz++)
          for (int dx = -1; dx <= 1; dx++)
            put(cx + dx, cy + dy, cz + dz, wood);
    }
  }

  // ---- crown: a dithered ellipsoid, thinning toward the rim ----
  const int cy = base.y + t.height - 6;
  const int R = t.crownR;
  for (int dy = -R / 2; dy <= R / 2 + 4; dy++)
    for (int dz = -R; dz <= R; dz++)
      for (int dx = -R; dx <= R; dx++) {
        const float rr = (float)(dx * dx + dz * dz) / (float)(R * R) +
                         (float)(dy * dy) / (float)((R / 2 + 4) * (R / 2 + 4));
        if (rr > 1.0f) continue;
        // 92% dense at the core, 35% at the rim. Ragged enough that burning
        // through a limb strands what it was holding; dense enough that the
        // prune below leaves a crown rather than a stick.
        const uint32_t keep = (uint32_t)(35.0f + 57.0f * (1.0f - rr));
        if (DitherHash(base.x + dx, cy + dy, base.z + dz) % 100u >= keep)
          continue;
        if (!cell[at(base.x + dx, cy + dy, base.z + dz)])
          put(base.x + dx, cy + dy, base.z + dz, leaves);
      }

  // ---- prune to the component that reaches the ground ----
  std::vector<uint8_t> keep(cell.size(), 0);
  std::vector<size_t> stack;
  const size_t root = at(base.x, base.y, base.z);
  if (cell[root]) {
    keep[root] = 1;
    stack.push_back(root);
  }
  while (!stack.empty()) {
    const size_t i = stack.back();
    stack.pop_back();
    const int x = (int)(i % LX), y = (int)((i / LX) % LY),
              z = (int)(i / ((size_t)LX * LY));
    const int nb[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                          {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (auto& d : nb) {
      const int nx = x + d[0], ny = y + d[1], nz = z + d[2];
      if (nx < 0 || ny < 0 || nz < 0 || nx >= LX || ny >= LY || nz >= LZ)
        continue;
      const size_t ni = (size_t)((nz * LY + ny) * LX + nx);
      if (cell[ni] && !keep[ni]) {
        keep[ni] = 1;
        stack.push_back(ni);
      }
    }
  }

  for (size_t i = 0; i < cell.size(); i++) {
    if (!keep[i]) continue;
    const int x = t.lo.x + (int)(i % LX), y = t.lo.y + (int)((i / LX) % LY),
              z = t.lo.z + (int)(i / ((size_t)LX * LY));
    const IVec3 cc{x, y, z};
    if (!world.CellInWindow(cc)) continue;
    if (ops.size() >= kMaxCellOpsPerTick) break;
    // Palette jitter by the same `% 3` convention every other writer uses, so
    // the fixture does not read as one flat slab of colour.
    ops.push_back({World::SlotCellIndex(cc),
                   PackVoxNew(cell[i], DitherHash(x, y, z) % 3u)});
    if (cell[i] == wood) t.woodCells++;
    else t.leafCells++;
  }
  return t;
}

namespace {

Status GateTreeFell(Ctx& c, std::string& detail) {
  World& world = c.world;
  DebrisSystem& debris = c.debris;
  // THE SKY IS PINNED CLEAR (suite triage 2026-10-01). Since 9a0a702
  // (2026-09-23, "rain touches the sim") the weather schedule rains on the
  // world at its tick: `rain` douses embers back to wood and `rainDamped` cuts
  // every combustion ignition by max(rain, ground wetness). This gate's ticks
  // (~94,000) fall in a rainy stretch of the weather.autoCycle schedule, so
  // the tree stopped burning (wood 3677 -> 2820..3103, under half needed) and
  // `tree-actually-burned` failed. The subject here is the floater handoff
  // after a burn, not rain on fire (rain-fire / rain-lean own that), so the
  // fixture burns in dry weather. Measured with the pin: wood 3677 -> 260.
  struct ClearSky {
    std::string prev = weather::Override();
    ClearSky() { weather::SetOverride("clear"); }
    ~ClearSky() { weather::SetOverride(prev); }
  } clearSky;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mWood = matId("wood"), mLeaves = matId("leaves"),
                 mFire = matId("fire");
  if (!mWood || !mLeaves || !mFire) {
    detail = "wood / leaves / fire missing from materials.json";
    return Status::Fail;
  }

  std::string failed;
  auto note = [&failed](bool ok, const char* name) {
    if (!ok) failed += failed.empty() ? name : (std::string(", ") + name);
    return ok;
  };

  // The CENTRE of the window, like corpse-burn and for the same reason: this
  // fixture is 92 voxels tall and 54 across, and an inset chosen for a 3-voxel
  // block puts half a tree outside the window where writes are dropped.
  const IVec3 org = world.WindowOrigin();
  const int fx = org.x * (int)kChunk + (int)(kWorldN / 2);
  const int fz = org.z * (int)kChunk + (int)(kWorldN / 2);
  const int groundY = World::TerrainHeight(fx, fz, kDefaultSeed);
  const IVec3 fixtureChunk{fx >> 4, groundY >> 4, fz >> 4};
  const bool siteInWindow = world.ChunkInWindow(fixtureChunk);

  // THE GROUND THIS TREE STANDS ON, measured rather than assumed. The `floaters`
  // gate builds a flat pad and says why: "procedural terrain at a fixed (x,z) is
  // a slope worldgen is free to move under this gate". This one plants a 92-tall
  // tree over a 59-cell footprint, so the relief across that footprint decides
  // whether a cut through the trunk actually isolates anything — a shoulder of
  // terrain rising past the cut plane re-attaches the tree to the world through
  // rock, and the severed tree is then genuinely, correctly, still supported.
  int terrainMin = INT32_MAX, terrainMax = INT32_MIN;
  for (int dz = -30; dz <= 30; dz++)
    for (int dx = -30; dx <= 30; dx++) {
      const int th = World::TerrainHeight(fx + dx, fz + dz, kDefaultSeed);
      terrainMin = std::min(terrainMin, th);
      terrainMax = std::max(terrainMax, th);
    }

  uint32_t tick = 91000;

  // One tick, with the whole handoff wired: the GPU support-loss flags become
  // island-check events (QueueSupportEvents), the debris system drains them
  // (PreTick), and the ops it produces are submitted. The `floaters` gate above
  // does NOT call QueueSupportEvents — it has no CA activity to flag — so this
  // is the first gate in the suite that exercises the flag -> event -> island
  // path end to end.
  //
  // `fetch` walks the tree's chunk box asking for mirror copies. The CPU mirror
  // is 3x3x3 around the player plus whatever is requested, and this gate's
  // sweep reads that mirror, so without this it would confidently report a
  // clean world it never looked at (World::kFetchPerTick caps it at 64/tick,
  // which is why the loops below run for a few ticks before measuring).
  double joltMsTotal = 0, joltWorstMs = 0;
  uint32_t joltTicks = 0;
  IVec3 fetchLo{}, fetchHi{};
  auto runTick = [&](const std::vector<CellOp>& extra, bool fetch) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(tick + 1, world, cellOps, spawns);
    cellOps.insert(cellOps.end(), extra.begin(), extra.end());
    if (fetch)
      for (int cz = fetchLo.z >> 4; cz <= (fetchHi.z >> 4); cz++)
        for (int cy = fetchLo.y >> 4; cy <= (fetchHi.y >> 4); cy++)
          for (int cx = fetchLo.x >> 4; cx <= (fetchHi.x >> 4); cx++)
            if (world.ChunkInWindow({cx, cy, cz}))
              world.RequestChunkFetch({cx, cy, cz});
    ++tick;
    SubmitTick(c.ctx, world, c.sim, tick, kDefaultSeed, {}, {}, cellOps, false,
               fixtureChunk, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    {
      // Jolt's own step, timed here because it is the one phase of the hitch
      // DebrisSystem cannot see from the inside (hypothesis 3: a compound of
      // thousands of boxes resting on triangle-mesh patches).
      const auto t0 = std::chrono::steady_clock::now();
      c.phys.Step(kTickDt);
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count();
      joltMsTotal += ms;
      joltTicks++;
      if (ms > joltWorstMs) joltWorstMs = ms;
    }
    debris.PostStep();
  };

  // Count one material over the swept box, straight off the mirror. The
  // positive control for both passes: "no floaters" and "the tree was never
  // written" are the same reading otherwise.
  auto countMat = [&](uint32_t want, IVec3 lo, IVec3 hi) {
    uint32_t n = 0;
    for (int z = lo.z; z <= hi.z; z++)
      for (int y = lo.y; y <= hi.y; y++)
        for (int x = lo.x; x <= hi.x; x++) {
          const IVec3 wc{x >> 4, y >> 4, z >> 4};
          if (!world.ChunkInWindow(wc)) continue;
          const CachedChunk* cc = world.Cached(wc);
          if (!cc || cc->voxels.size() != kChunkVol) continue;
          const uint32_t w =
              cc->voxels[((((uint32_t)z) & 15u) * kChunk + (((uint32_t)y) & 15u)) *
                             kChunk +
                         (((uint32_t)x) & 15u)];
          if ((w & 0xFFFu) == want) n++;
        }
    return n;
  };

  // The fixture, planted and given a few ticks to land in the mirror.
  //
  // LEVELLED FIRST, and the run that proved it necessary is worth recording.
  // Planted straight onto worldgen, the gate reported 3300 wood voxels still
  // standing above a fully severed trunk while the sweep found NOTHING
  // unsupported — which reads like a broken sweep and is not one. The relief
  // across this footprint is 162..211 with the trunk foot at 176: the hillside
  // rises 22 voxels PAST the cut plane, so the tree above the cut was still
  // 6-connected to the world through rock and was correctly judged supported.
  // The sweep was right and the fixture was standing in a hill.
  //
  // So a disc is levelled to the trunk's own ground height before anything is
  // planted — dug out where the hill is higher, filled where it falls away.
  // Radius 12 is chosen from the geometry rather than by taste: above the cut
  // the only part of the tree below y+35 (the measured relief ceiling) is the
  // radius-3 trunk, so a 12-cell clearance puts every remaining terrain cell
  // eight cells from anything the tree owns. Same reason `floaters` builds a
  // pad, and its comment says it in one line: procedural terrain at a fixed
  // (x,z) is a slope worldgen is free to move under a gate.
  // ---- the site: a bare plateau, exactly as wide as the sweep --------------
  //
  // Every relaxation of this was paid for by a run, so all three reasons are
  // written down rather than compressed into "prepare the site":
  //
  //   1. LEVELLED AT ALL. Planted straight onto worldgen, the gate reported
  //      3300 wood voxels still standing above a fully severed trunk while the
  //      sweep found nothing unsupported. That reads like a broken sweep and is
  //      not one: relief across this footprint is 162..211 with the trunk foot
  //      at 176, so the hill rises 22 voxels PAST the cut plane and the tree
  //      above the cut was still 6-connected to the world through rock.
  //   2. CLEARED TO THE CEILING, not down to TerrainHeight. Digging only to the
  //      heightfield left a 118-voxel floater in the "clean" fixture: worldgen
  //      puts TREES on the hill, and cutting the ground from under one leaves
  //      its crown in the air. Correct engine behaviour and exactly the bug
  //      under test — but arriving from the scenery instead of the subject.
  //   3. AS WIDE AS THE SWEEP BOX. A radius-12 clearing still left 6 floaters,
  //      because the sweep reaches +-30 and the surrounding forest's crowns are
  //      dithered down to isolated rim voxels BY DESIGN (DESIGN.md section 7
  //      says so in as many words). Those are real floaters and none of them is
  //      this gate's. The box is cleared to its own edge so that what remains
  //      inside it is the fixture and nothing else.
  //
  // `phase` splits the work across ticks: 61x61 columns 135 cells tall is 500k
  // exact-cell ops and kMaxCellOpsPerTick is 65536.
  constexpr int kSiteR = 30, kSiteTop = 135, kSiteBand = 15;
  constexpr int kSitePhases = kSiteTop / kSiteBand + 1;  // + the fill pass
  auto levelSite = [&](std::vector<CellOp>& ops, int phase) {
    for (int dz = -kSiteR; dz <= kSiteR; dz++)
      for (int dx = -kSiteR; dx <= kSiteR; dx++) {
        const int th = World::TerrainHeight(fx + dx, fz + dz, kDefaultSeed);
        if (phase + 1 < kSitePhases) {
          const int y0 = groundY + 1 + phase * kSiteBand;
          for (int y = y0; y < y0 + kSiteBand; y++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (world.CellInWindow(cc) && ops.size() < kMaxCellOpsPerTick)
              ops.push_back({World::SlotCellIndex(cc), 0u});
          }
        } else {
          // fill the hollow up to the trunk's ground, so the stump has ground
          for (int y = th + 1; y <= groundY; y++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (world.CellInWindow(cc) && ops.size() < kMaxCellOpsPerTick)
              ops.push_back(
                  {World::SlotCellIndex(cc),
                   PackVoxNew(kMatStone, DitherHash(cc.x, y, cc.z) % 3u)});
          }
        }
      }
  };

  auto plant = [&]() -> TreeFixture {
    debris.Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    {
      // Its own ticks, ahead of the tree: the terraform raises support-loss
      // flags of its own, and letting them settle before the subject exists
      // keeps the probe counters below about the TREE.
      fetchLo = IVec3{fx - kSiteR, groundY - 6, fz - kSiteR};
      fetchHi = IVec3{fx + kSiteR, groundY + kSiteTop, fz + kSiteR};
      for (int phase = 0; phase < kSitePhases; phase++) {
        std::vector<CellOp> level;
        levelSite(level, phase);
        runTick(level, true);
      }
      for (int i = 0; i < 80; i++) runTick({}, true);
    }
    std::vector<CellOp> build;
    const TreeFixture t =
        BuildTree(world, {fx, groundY + 1, fz}, mWood, mLeaves, build);
    fetchLo = IVec3{t.lo.x - 2, t.lo.y - 4, t.lo.z - 2};
    fetchHi = IVec3{t.hi.x + 2, t.hi.y + 2, t.hi.z + 2};
    runTick(build, true);
    for (int i = 0; i < 14; i++) runTick({}, true);  // mirror catches up
    return t;
  };

  // ---- PASS A: BURN IT DOWN ------------------------------------------------
  //
  // The owner's report, reproduced: engulf the tree, let the fire eat it, and
  // ask what the handoff left hanging.
  //
  // ENGULFED, not lit at the foot, and the first run is why. A fire at the
  // trunk base for 40 ticks moved 5% of the wood and never touched a leaf —
  // ignition is a 1/8 per-neighbour roll and the flame was deliberately
  // weakened (`combustion.flamePct`), so a base fire takes thousands of ticks
  // to climb 92 voxels and the gate would have been measuring the climb. The
  // subject is the AFTERMATH of a burnt tree, so the fixture starts from a tree
  // that is already alight everywhere: a fire column around the trunk and a
  // scatter through the crown, held for 60 ticks and then never renewed. After
  // that it feeds on the tree alone, which is what makes the quiet period below
  // a real measurement instead of a reading taken inside a pyre.
  const TreeFixture treeA = plant();
  const uint32_t woodBeforeA = countMat(mWood, treeA.lo, treeA.hi);
  const uint32_t leafBeforeA = countMat(mLeaves, treeA.lo, treeA.hi);
  // THE FIXTURE'S OWN PRECONDITION. BuildTree prunes to one ground-connected
  // component, so a freshly planted tree must float nothing at all. If this is
  // not zero, every number the passes below report is construction noise and
  // the gate is measuring itself (which is exactly what its first run did).
  const SweepResult swPre =
      SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
  debris.ResetFloaterProbe();
  for (int i = 0; i < 640; i++) {
    std::vector<CellOp> fire;
    if (i < 60) {
      // a column of flame hugging the trunk, all the way up
      for (int y = 0; y <= treeA.height; y++)
        for (int dz = -6; dz <= 6; dz++)
          for (int dx = -6; dx <= 6; dx++) {
            if (dx * dx + dz * dz > 36) continue;
            const IVec3 cc{fx + dx, groundY + 1 + y, fz + dz};
            if (!world.CellInWindow(cc)) continue;
            fire.push_back({World::SlotCellIndex(cc),
                            PackVoxNew(mFire, 7u) | kCellOpIfAir});
          }
      // ...and a scatter through the crown, so the canopy burns with it
      const int cyA = groundY + 1 + treeA.height - 6, R = treeA.crownR;
      for (int dy = -R / 2; dy <= R / 2 + 4; dy++)
        for (int dz = -R; dz <= R; dz++)
          for (int dx = -R; dx <= R; dx++) {
            if (dx * dx + dz * dz > R * R) continue;
            if (DitherHash(fx + dx, cyA + dy + i, fz + dz) % 5u) continue;
            const IVec3 cc{fx + dx, cyA + dy, fz + dz};
            if (!world.CellInWindow(cc)) continue;
            if (fire.size() >= kMaxCellOpsPerTick) break;
            fire.push_back({World::SlotCellIndex(cc),
                            PackVoxNew(mFire, 7u) | kCellOpIfAir});
          }
    }
    runTick(fire, true);
  }
  // ---- PASS A0: THE NATURAL BURN-OUT, sampled ------------------------------
  //
  // What the owner actually stands in front of. No quench: the crown goes on
  // smouldering, and every 100 ticks the box is swept for floating components
  // AND counted for cells still hot. The pair is what separates "the handoff
  // strands clumps" from "the fire is still making them": residue that tracks
  // the hot count is the burn, residue that stays up while the hot count falls
  // to nothing is the machinery. Reported as a series, because a single
  // sample of a smouldering tree is a number with no cause attached.
  std::string burnSeries;
  uint32_t residueAtBurnEnd = 0, hotAtBurnEnd = 0;
  {
    std::vector<uint8_t> hotTag(c.mats.size(), 0);
    for (size_t i = 0; i < c.mats.size(); i++)
      for (const std::string& t : c.mats[i].tags)
        if (t == "hot") hotTag[i] = 1;
    auto countHot = [&]() {
      uint32_t n = 0;
      for (int z = fetchLo.z; z <= fetchHi.z; z++)
        for (int y = fetchLo.y; y <= fetchHi.y; y++)
          for (int x = fetchLo.x; x <= fetchHi.x; x++) {
            const IVec3 wc{x >> 4, y >> 4, z >> 4};
            if (!world.ChunkInWindow(wc)) continue;
            const CachedChunk* cc = world.Cached(wc);
            if (!cc || cc->voxels.size() != kChunkVol) continue;
            const uint32_t m =
                cc->voxels[((((uint32_t)z) & 15u) * kChunk + (((uint32_t)y) & 15u)) *
                               kChunk +
                           (((uint32_t)x) & 15u)] & 0xFFFu;
            if (m < hotTag.size() && hotTag[m]) n++;
          }
      return n;
    };
    for (int t = 100; t <= 1500; t += 100) {
      for (int i = 0; i < 100; i++) runTick({}, true);
      const SweepResult s = SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
      residueAtBurnEnd = s.singles + s.smallComps + s.bigComps;
      hotAtBurnEnd = countHot();
      if (t % 300 == 0)
        burnSeries += Format("%s+%d:%u/%uhot", burnSeries.empty() ? "" : " ",
                             t, residueAtBurnEnd, hotAtBurnEnd);
    }
  }

  // ---- QUENCH: the fire is now OUT, by fiat ---------------------------------
  //
  // Without this the latency numbers below measure nothing. A burnt tree
  // smoulders for thousands of ticks (leaf_burning -> ember -> ash is a slow
  // chain, and bodies burn on their own clock), so every sweep during the
  // "quiet" period was taken while new floaters were still being MADE, and the
  // residue read 22, 11, 30 at +300/+900/+1800 -- non-monotonic, never clean,
  // and saying nothing about how fast the handoff clears what it is handed.
  //
  // So every hot cell in the box is finished off at once: hot gases (fire) to
  // air, hot solids (ember, burning leaves) to their burnt-out powder. Ash is a
  // powder and falls, which is exactly what the end of a real fire does to
  // whatever the embers were holding up -- and it is the last support-loss
  // flag this fixture will ever raise. From this tick the clock measures ONE
  // thing: how long the machinery takes to clear a known, fixed set of
  // floaters.
  {
    const uint32_t mAsh = matId("ash");
    std::vector<uint8_t> hot(c.mats.size(), 0), klass(c.mats.size(), 0);
    for (size_t i = 0; i < c.mats.size(); i++) {
      klass[i] = (uint8_t)c.mats[i].gpu.klass;
      for (const std::string& t : c.mats[i].tags)
        if (t == "hot") hot[i] = 1;
    }
    std::vector<CellOp> quench;
    for (int z = fetchLo.z; z <= fetchHi.z; z++)
      for (int y = fetchLo.y; y <= fetchHi.y; y++)
        for (int x = fetchLo.x; x <= fetchHi.x; x++) {
          const IVec3 wc{x >> 4, y >> 4, z >> 4};
          if (!world.ChunkInWindow(wc)) continue;
          const CachedChunk* cc = world.Cached(wc);
          if (!cc || cc->voxels.size() != kChunkVol) continue;
          const uint32_t w =
              cc->voxels[((((uint32_t)z) & 15u) * kChunk + (((uint32_t)y) & 15u)) *
                             kChunk +
                         (((uint32_t)x) & 15u)];
          const uint32_t m = w & 0xFFFu;
          if (m == 0 || m >= hot.size() || !hot[m]) continue;
          if (quench.size() >= kMaxCellOpsPerTick) break;
          const bool solidHot = klass[m] == CLASS_SOLID || klass[m] == CLASS_POWDER;
          quench.push_back({World::SlotCellIndex({x, y, z}),
                            solidHot ? PackVoxNew(mAsh, DitherHash(x, y, z) % 3u)
                                     : 0u});
        }
    runTick(quench, true);
    // AND THE BODIES. A quench of the grid alone left 68 alight rigidbodies on
    // the ground, and BurnBodies emits real fire back into the grid, so the
    // "quiet" period re-lit the tree and the post-quench residue ROSE
    // (31, 37, 29 with zero hot grid cells). Their matter is taken out of the
    // world here: this clock is for the grid handoff, and a burning corpse
    // pile is a different fixture's subject.
    for (uint32_t i = debris.BodyCount(); i-- > 0;)
      debris.DestroyBody(debris.BodyHandle(i));
  }
  // Let the queue drain with nothing new arriving. This is the quiet period a
  // player stands in afterwards, and what they see then is the report.
  for (int i = 0; i < 400; i++) runTick({}, true);

  const uint32_t woodAfterA = countMat(mWood, treeA.lo, treeA.hi);
  const uint32_t leafAfterA = countMat(mLeaves, treeA.lo, treeA.hi);
  const SweepResult swA =
      SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
  const DebrisSystem::FloaterProbe fpA = debris.Floaters();
  const uint32_t bodiesA = debris.BodyCount();

  // ---- PASS A1: HOW LONG THE RESIDUE HANGS THERE ---------------------------
  //
  // The owner's second report, made measurable: the 2..8-voxel clumps left by a
  // burnt tree "DO all eventually fall after a minute or two, but that's a
  // minute or two of hanging in the air". So this is a LATENCY defect, not a
  // correctness one — the handoff arrives, far too late — and a gate that only
  // sweeps once cannot tell the two apart. It reports the same number for a
  // world that will never clean up and a world that cleans up in an hour.
  //
  // So: keep sweeping, and record the tick the world first comes back clean.
  // The sweep above (`swA`, at a FIXED 400 ticks after the fire) keeps owning
  // the `maxSingles` / `maxClumps` thresholds, so those assertions still mean
  // what they meant; this adds the axis they cannot express.
  //
  // 1800 ticks is 60 s at the 30 Hz sim rate, which is the upper end of what
  // the report describes. A run that has not converged by then reports -1
  // rather than a bigger number, because "still dirty at a minute" is the
  // finding and the exact tick past that is not worth the wall clock.
  int cleanTick = -1;
  uint32_t residueAt300 = 0, residueAt900 = 0, residueAt1800 = 0;
  {
    const int kStride = 100, kMaxWait = 1800;
    for (int t = kStride; t <= kMaxWait; t += kStride) {
      for (int i = 0; i < kStride; i++) runTick({}, true);
      const SweepResult s = SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
      const uint32_t residue = s.singles + s.smallComps + s.bigComps;
      if (t <= 300) residueAt300 = residue;
      if (t <= 900) residueAt900 = residue;
      residueAt1800 = residue;
      if (residue == 0 && cleanTick < 0) {
        cleanTick = t;
        break;
      }
    }
  }

  // ---- PASS A2: THE FORCED RESCAN -----------------------------------------
  //
  // The one measurement that splits the remaining hypotheses in a single run
  // instead of one per elimination (CLAUDE.md rule 6). Everything above depends
  // on island detection being SUMMONED to the right place: the GPU support flag
  // is the only trigger, it is rate-limited per chunk and drained a few a tick,
  // and it stops firing the moment the CA goes quiet. So "the scan declined
  // this component" and "no scan was ever run here" produce exactly the same
  // world, and no counter can tell them apart — a decline is recorded at the
  // site that makes it, and a scan that never happened records nothing at all.
  //
  // So: tile the tree's box with explicit destruction events, which is the same
  // door the brush and the grenade use, and sweep again. If the residue clears,
  // the anchoring rules were fine and the TRIGGER missed it. If it survives a
  // scan aimed squarely at it, the rules declined it and the anchor counters
  // say which one did.
  for (int by = treeA.lo.y; by <= treeA.hi.y; by += 30)
    for (int bz = treeA.lo.z; bz <= treeA.hi.z; bz += 30)
      for (int bx = treeA.lo.x; bx <= treeA.hi.x; bx += 30)
        debris.AddDestructionEvent(tick, {bx, by, bz},
                                   {bx + 29, by + 29, bz + 29}, 4);
  const uint32_t rescanFrom = tick;
  for (int i = 0; i < 400; i++) {
    runTick({}, true);
    // keep the tiling topped up: the queue holds 64 and this is ~24 events
    if ((i % 60) == 30)
      for (int by = treeA.lo.y; by <= treeA.hi.y; by += 30)
        for (int bz = treeA.lo.z; bz <= treeA.hi.z; bz += 30)
          for (int bx = treeA.lo.x; bx <= treeA.hi.x; bx += 30)
            debris.AddDestructionEvent(tick, {bx, by, bz},
                                       {bx + 29, by + 29, bz + 29}, 4);
  }
  (void)rescanFrom;
  const SweepResult swA2 =
      SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);

  // It must have BURNED, or every number after it is about a tree that just
  // stood there. Half the wood gone is a low bar deliberately: this gate is not
  // a combustion test, it only needs the fire to have done real work.
  const bool burned = note(woodBeforeA > 2000 && woodAfterA * 2 < woodBeforeA,
                           "tree-actually-burned");
  const bool cleanStart =
      note(swPre.singles + swPre.smallComps + swPre.bigComps == 0,
           "fixture-starts-clean");
  const bool sweepReal =
      note(swA.componentsTotal > 0 && swA.cellsScanned > 0, "sweep-saw-something");
  const bool inWin = note(siteInWindow, "fixture-in-window");
  const bool noSingles =
      note(swA.singles <= (uint32_t)BaselineNumber("treeFell.maxSingles", 0),
           "burn-leaves-no-singles");
  const bool noClumps =
      note(swA.smallComps + swA.bigComps <=
               (uint32_t)BaselineNumber("treeFell.maxClumps", 0),
           "burn-leaves-no-clumps");

  // ---- PASS B: CUT THE TRUNK ----------------------------------------------
  //
  // A fresh tree, a three-voxel slab erased out of the trunk ten cells up, and
  // nothing else. Everything above the cut is one 6-connected component resting
  // on air, so the correct outcome is unambiguous: it becomes a rigidbody and
  // falls over. What it does today is the subject.
  const TreeFixture treeB = plant();
  const uint32_t woodBeforeB = countMat(mWood, treeB.lo, treeB.hi);
  debris.ResetFloaterProbe();
  // ---- THE HITCH, MEASURED ------------------------------------------------
  //
  // The owner's report is "2 fps for five seconds when a tree enters the
  // rigidbody system", and this is the cheapest reproduction of it in the
  // repo: one 28k-voxel body, born in one tick, falling for 300. The phase
  // clock goes on HERE rather than around the whole gate so the numbers are
  // about the cut and not about the 100-tick terraform that precedes it.
  debris.SetProfiling(true);
  debris.ResetProfile();
  joltMsTotal = joltWorstMs = 0;
  joltTicks = 0;
  {
    std::vector<CellOp> cut;
    const int cutY = groundY + 11;
    for (int y = cutY; y < cutY + 3; y++)
      for (int dz = -4; dz <= 4; dz++)
        for (int dx = -4; dx <= 4; dx++) {
          const IVec3 cc{fx + dx, y, fz + dz};
          if (!world.CellInWindow(cc)) continue;
          cut.push_back({World::SlotCellIndex(cc), 0u});
        }
    runTick(cut, true);
  }
  // The cut also goes in through the CPU door the brush uses, because the GPU
  // support flag is a BACKSTOP (a 45-tick cooldown behind an async readback)
  // and a player's axe does not wait for it. Both doors, like the real game.
  debris.AddDestructionEvent(tick, {fx - 5, groundY + 10, fz - 5},
                             {fx + 5, groundY + 15, fz + 5});
  // The felled tree is an ASSEMBLY of shards (PLAN §5), so the claim is on
  // the matter in bodies at once, not on the largest single body.
  uint32_t maxBodyVox = 0, bodiesMadeB = 0, sumBodyVox = 0;
  for (int i = 0; i < 300; i++) {
    runTick({}, true);
    bodiesMadeB = std::max(bodiesMadeB, debris.BodyCount());
    uint32_t sum = 0;
    for (uint32_t b = 0; b < debris.BodyCount(); b++) {
      maxBodyVox = std::max(maxBodyVox, debris.BodyVoxelCount(b));
      sum += debris.BodyVoxelCount(b);
    }
    sumBodyVox = std::max(sumBodyVox, sum);
  }
  const std::string cutProfile = debris.ProfileReport();
  debris.SetProfiling(false);
  const uint32_t woodAfterB = countMat(mWood, treeB.lo, treeB.hi);
  // The mirror's own answer to "is the tree still up there", independent of the
  // sweep's connectivity reasoning. Two numbers derived different ways: if the
  // wood is still above the cut and the sweep calls nothing unsupported, the
  // disagreement is the finding (and on the first run it was — see the note on
  // `standingAbove` in the detail line).
  const uint32_t standingAbove =
      countMat(mWood, {treeB.lo.x, groundY + 14, treeB.lo.z}, treeB.hi);
  const SweepResult swB = SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
  const DebrisSystem::FloaterProbe fpB = debris.Floaters();

  // THE ASSERTION THIS GATE EXISTS FOR. The trunk above the cut is ~2000 wood
  // voxels with nothing under it; it must leave the grid as a body. The floor
  // is a fraction of what was standing rather than a literal, so re-shaping the
  // fixture does not silently re-tune the claim.
  const uint32_t felledFloor =
      (uint32_t)(woodBeforeB * BaselineNumber("treeFell.felledFraction", 0.30));
  const bool felled =
      note(sumBodyVox >= felledFloor, "cut-trunk-fells-the-tree");
  // ...and the complementary half, which is what the owner actually sees: the
  // severed tree must not still be STANDING in the grid.
  const bool leftStanding =
      note(swB.bigVoxels <= (uint32_t)BaselineNumber("treeFell.maxStandingVoxels",
                                                     200),
           "cut-leaves-nothing-standing");

  RecordObserved("treeFell.cleanTick", (double)cleanTick);
  RecordObserved("treeFell.naturalResidue", (double)residueAtBurnEnd);
  RecordObserved("treeFell.naturalHot", (double)hotAtBurnEnd);
  RecordObserved("treeFell.residueAt300", (double)residueAt300);
  RecordObserved("treeFell.singlesObserved", (double)swA.singles);
  RecordObserved("treeFell.clumpsObserved",
                 (double)(swA.smallComps + swA.bigComps));
  RecordObserved("treeFell.burnFloatVoxels", (double)swA.voxels);
  RecordObserved("treeFell.felledBodyVoxels", (double)sumBodyVox);
  RecordObserved("treeFell.standingVoxels", (double)swB.bigVoxels);
  // The hitch, as three numbers in build/last_run.json: observed only, never
  // asserted -- a wall-clock threshold in a gate would be a machine test.
  RecordObserved("treeFell.cutDebrisMsTotal",
                 debris.Profile().tickUsTotal / 1000.0);
  RecordObserved("treeFell.cutDebrisWorstMs",
                 debris.Profile().worstTickUs / 1000.0);
  RecordObserved("treeFell.cutChunksNeededMax",
                 (double)debris.Profile().maxNeededOneTick);

  std::string worst;
  for (const FloatComp& f : swA.worst)
    worst += Format("%s%zu vox mat %u at (%d,%d,%d)", worst.empty() ? "" : "; ",
                    f.voxels, f.domMat, f.lo.x, f.lo.y, f.lo.z);

  const bool ok = burned && cleanStart && sweepReal && inWin && noSingles &&
                  noClumps && felled && leftStanding;
  detail = Format(
      "at (%d,%d) ground %d (relief %d..%d) inWindow %d; planted clean %u/%u/%u; "
      "BURN: wood %u->%u leaves %u->%u, %u bodies; floating singles %u "
      "(all-air %u, touch powder %u, liquid %u, hot-gas %u), "
      "2..7 %u, >=8 %u (%u vox) of %u comps over %u cells (%u absent)%s%s%s; "
      "NATURAL burn-out [%s] (%u floating / %u hot at +1500); after QUENCH residue "
      "%u @+300, %u @+900, %u @+1800, CLEAN at +%d ticks; "
      "after a FORCED rescan %u/%u/%u (%u vox); "
      "probe leaks oversize %u, in-place %u, stuck-event %u, defer-gaveup %u "
      "(waiting on a fetch %u [waits: no-cache %u no-voxels %u stale %u; last gave up "
      "on chunk (%d,%d,%d) inWindow %u cached %u for seed (%d,%d,%d)..(%d,%d,%d)], "
      "unfetched-held %u, body-cap %u), "
      "queue-dropped %u; stuck-requeued %u, cooldown-held %u/rearmed %u; "
      "deferrals cellop %u, spawn-ring %u, oversize %u, spilled %u, "
      "settle-unsupported %u, backpressure %u; "
      "anchors boundary %u, unknown %u, oversize-flood %u; "
      "SMALL comps anchored boundary %u / unknown %u / powder %u, freed %u; "
      "%u scans visited %llu of %llu cells | "
      "CUT: wood %u->%u (%u still above the cut), bodies %u, largest body %u "
      "vox (floor %u), unsupported %u/%u/%u of %u comps over %u cells "
      "(%u absent) = %u vox; biggest comp %zu vox (%d,%d,%d)..(%d,%d,%d) "
      "rests %d why %d at (%d,%d,%d); probe stuck-event %u, requeued %u, "
      "anchors boundary %u, unknown %u, oversize-flood %u | "
      "COST over the 300 cut ticks: %s; jolt step %.1f ms over %u ticks "
      "(%.2f/tick, worst %.1f)%s%s",
      fx, fz, groundY, terrainMin, terrainMax, siteInWindow ? 1 : 0,
      swPre.singles, swPre.smallComps,
      swPre.bigComps, woodBeforeA, woodAfterA, leafBeforeA,
      leafAfterA, bodiesA, swA.singles, swA.singlesAllAir,
      swA.singlesTouchPowder, swA.singlesTouchLiquid, swA.singlesTouchOther,
      swA.smallComps, swA.bigComps,
      swA.bigVoxels, swA.componentsTotal, swA.cellsScanned, swA.chunksMissing,
      worst.empty() ? "" : " [", worst.c_str(), worst.empty() ? "" : "]",
      burnSeries.c_str(), residueAtBurnEnd, hotAtBurnEnd,
      residueAt300, residueAt900, residueAt1800, cleanTick,
      swA2.singles, swA2.smallComps, swA2.bigComps, swA2.bigVoxels,
      fpA.oversizeBboxSkipped, fpA.solidRubbleInPlace, fpA.stuckEventDropped,
      fpA.deferGaveUp, fpA.deferGaveUpFetch, fpA.fetchWaitNoCache,
      fpA.fetchWaitNoVoxels, fpA.fetchWaitStale, fpA.gaveUpChunk.x,
      fpA.gaveUpChunk.y, fpA.gaveUpChunk.z, fpA.gaveUpChunkInWindow,
      fpA.gaveUpChunkCached, fpA.gaveUpSeedLo.x, fpA.gaveUpSeedLo.y,
      fpA.gaveUpSeedLo.z, fpA.gaveUpSeedHi.x, fpA.gaveUpSeedHi.y,
      fpA.gaveUpSeedHi.z, fpA.deferredUnfetched,
      fpA.deferredBodyCap, fpA.eventQueueFullDropped, fpA.stuckEventRequeued,
      fpA.supportLateHeld, fpA.supportLateRearmed,
      fpA.deferredCellOpBudget, fpA.deferredSpawnRing, fpA.deferredOversize,
      fpA.eventQueueFullSpilled, fpA.settleWithoutSupport,
      fpA.drainBackpressure,
      fpA.anchoredByRegionBoundary,
      fpA.anchoredByUnknownChunk, fpA.anchoredByOversizeFlood,
      fpA.smallAnchoredBoundary, fpA.smallAnchoredUnknown,
      fpA.smallAnchoredPowder, fpA.smallUnanchored, fpA.scans,
      (unsigned long long)fpA.scanCellsVisited,
      (unsigned long long)fpA.scanCellsCovered, woodBeforeB,
      woodAfterB, standingAbove, bodiesMadeB, maxBodyVox, felledFloor,
      swB.singles, swB.smallComps, swB.bigComps, swB.componentsTotal,
      swB.cellsScanned, swB.chunksMissing, swB.bigVoxels, swB.biggest,
      swB.biggestLo.x, swB.biggestLo.y, swB.biggestLo.z, swB.biggestHi.x,
      swB.biggestHi.y, swB.biggestHi.z, swB.biggestRests ? 1 : 0,
      swB.biggestRestWhy, swB.biggestRestAt.x, swB.biggestRestAt.y,
      swB.biggestRestAt.z,
      fpB.stuckEventDropped, fpB.stuckEventRequeued,
      fpB.anchoredByRegionBoundary,
      fpB.anchoredByUnknownChunk, fpB.anchoredByOversizeFlood,
      cutProfile.c_str(), joltMsTotal, joltTicks,
      joltMsTotal / (joltTicks ? joltTicks : 1), joltWorstMs,
      failed.empty() ? "" : "; FAILED: ", failed.c_str());
  std::printf("tree-fell: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  // Leave the world as this gate found it, like every late world-touching gate.
  debris.Reset();
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// cactus-fell: the rigidbody handoff on a thing every cap already fits
// ---------------------------------------------------------------------------
//
// WHY. `tree-fell` is red because a tree crosses kMaxRegionCells, and that is
// a known, planned, structural limit (docs/PLAN_rigidbody_islands.md). The
// owner reports the same symptom on a SAGUARO: sever the bole and the top
// stands in the air. A saguaro is 30..50 voxels tall, its arms reach a few
// voxels, it is one 6-connected component, and it stands on SAND. It fits the
// scan box, the flood cap and the int8 body lattice with room to spare, so if
// it does not fall the reason is not a cap — it is somewhere in the flag ->
// event -> fetch -> scan -> body chain, and this gate is built to say where.
//
// THREE ARMS on one fixture, so one build attributes rather than eliminates:
//   1. `game`   sand bed, the cut goes in as exact-cell ops ONLY (so the GPU
//               support-loss flag is the only summons), and the mirror is NOT
//               force-fetched afterwards: the debris system's own fetch
//               requests (EventReady, ManageTerrain) are all it gets, which is
//               what a player standing in the desert gives it.
//   2. `cpu`    sand bed, plus the brush's CPU door (AddDestructionEvent) and
//               forced fetches — the `tree-fell` cut recipe, on a cactus.
//   3. `stone`  the same cut on a stone pad: the control the tree gate ran.
// The claim per arm is the same: after 300 ticks a body at least half the
// severed top's size exists (or existed), and the mirror holds nothing of the
// cactus material above the cut plane that is still unsupported.

struct CactusFixture {
  IVec3 base{};            // bole foot, world cells (base.y is the first cactus cell)
  int height = 0;
  int radius = 0;
  IVec3 lo{}, hi{};
  uint32_t cells = 0;
  uint32_t aboveCut = 0;   // cells strictly above the cut plane, as written
};

// One saguaro: a ribbed bole of radius r and height h, one arm leaving at 45%
// height, reaching 3r out along +x and rising to 85% height. Same geometry as
// worldgen's cactusCell for species 0, minus the bloom (passable matter is not
// what this gate is about).
CactusFixture BuildCactus(const World& world, IVec3 base, uint32_t flesh,
                          uint32_t rib, int cutY, std::vector<CellOp>& ops) {
  CactusFixture f;
  f.base = base;
  f.height = 40;
  f.radius = 3;
  const int r = f.radius, h = f.height;
  const int attach = h * 45 / 100, ex = r * 3, ar = 2;
  const int riseTop = attach + (h - attach) * 85 / 100;
  f.lo = IVec3{base.x - r - 1, base.y, base.z - r - 1};
  f.hi = IVec3{base.x + ex + ar + 1, base.y + h, base.z + r + 1};
  for (int z = f.lo.z; z <= f.hi.z; z++)
    for (int y = f.lo.y; y <= f.hi.y; y++)
      for (int x = f.lo.x; x <= f.hi.x; x++) {
        const int dx = x - base.x, dy = y - base.y, dz = z - base.z;
        uint32_t m = 0;
        const int d2 = dx * dx + dz * dz;
        if (dy >= 0 && dy <= h && d2 <= r * r) {
          const bool rim = d2 * 4 >= r * r * 3;
          const int flute = (std::abs(dx) * 7 + std::abs(dz) * 11) % 5;
          m = (rim || flute == 0) ? rib : flesh;
        } else if (dx >= 0 && dx <= ex &&
                   (dy - attach) * (dy - attach) + dz * dz <= ar * ar) {
          m = rib;  // the arm's horizontal run, +x from the bole
        } else if (dy >= attach && dy <= riseTop &&
                   (dx - ex) * (dx - ex) + dz * dz <= ar * ar) {
          m = rib;  // the arm's rise
        }
        if (!m) continue;
        const IVec3 cc{x, y, z};
        if (!world.CellInWindow(cc)) continue;
        ops.push_back({World::SlotCellIndex(cc),
                       PackVoxNew(m, DitherHash(x, y, z) % 3u)});
        f.cells++;
        if (y > cutY) f.aboveCut++;
      }
  return f;
}

Status GateCactusFell(Ctx& c, std::string& detail) {
  World& world = c.world;
  DebrisSystem& debris = c.debris;

  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  const uint32_t mFlesh = matId("cactus_flesh"), mRib = matId("cactus_rib"),
                 mSand = matId("sand");
  if (!mFlesh || !mRib || !mSand) {
    detail = "cactus_flesh / cactus_rib / sand missing from materials.json";
    return Status::Fail;
  }

  std::string failed;
  auto note = [&failed](bool ok, const char* name) {
    if (!ok) failed += failed.empty() ? name : (std::string(", ") + name);
    return ok;
  };

  const IVec3 org = world.WindowOrigin();
  const int fx = org.x * (int)kChunk + (int)(kWorldN / 2);
  const int fz = org.z * (int)kChunk + (int)(kWorldN / 2);
  const int groundY = World::TerrainHeight(fx, fz, kDefaultSeed);
  const IVec3 fixtureChunk{fx >> 4, groundY >> 4, fz >> 4};
  const bool siteInWindow = world.ChunkInWindow(fixtureChunk);

  // Past whatever the suite has already submitted: the mirror ignores a fetch
  // whose tick is older than the copy it holds (World::EncodeReadbacks), so a
  // gate that rewinds the clock reads a world it never wrote.
  uint32_t tick = std::max(93000u, world.Snap().tick + 1000u);
  IVec3 fetchLo{}, fetchHi{};
  auto runTick = [&](const std::vector<CellOp>& extra, bool fetch) {
    std::vector<CellOp> cellOps;
    std::vector<ParticleSpawn> spawns;
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(tick + 1, world, cellOps, spawns);
    cellOps.insert(cellOps.end(), extra.begin(), extra.end());
    if (fetch)
      for (int cz = fetchLo.z >> 4; cz <= (fetchHi.z >> 4); cz++)
        for (int cy = fetchLo.y >> 4; cy <= (fetchHi.y >> 4); cy++)
          for (int cx = fetchLo.x >> 4; cx <= (fetchHi.x >> 4); cx++)
            if (world.ChunkInWindow({cx, cy, cz}))
              world.RequestChunkFetch({cx, cy, cz});
    ++tick;
    SubmitTick(c.ctx, world, c.sim, tick, kDefaultSeed, {}, {}, cellOps, false,
               fixtureChunk, true, false, spawns);
    c.ctx.WaitIdle();
    c.ctx.ProcessEvents();
    c.phys.Step(kTickDt);
    debris.PostStep();
  };
  auto countMat = [&](uint32_t want, IVec3 lo, IVec3 hi) {
    uint32_t n = 0;
    for (int z = lo.z; z <= hi.z; z++)
      for (int y = lo.y; y <= hi.y; y++)
        for (int x = lo.x; x <= hi.x; x++) {
          const IVec3 wc{x >> 4, y >> 4, z >> 4};
          if (!world.ChunkInWindow(wc)) continue;
          const CachedChunk* cc = world.Cached(wc);
          if (!cc || cc->voxels.size() != kChunkVol) continue;
          const uint32_t w =
              cc->voxels[((((uint32_t)z) & 15u) * kChunk + (((uint32_t)y) & 15u)) *
                             kChunk +
                         (((uint32_t)x) & 15u)];
          if ((w & 0xFFFu) == want) n++;
        }
    return n;
  };

  // Site: clear 60 up, fill to groundY with stone, then (sand arms) a 4-deep
  // sand bed on top. The cactus foot is the first cell above the bed.
  constexpr int kSiteR = 20, kSiteTop = 60;
  auto plant = [&](bool sandBed, int& cutY) -> CactusFixture {
    debris.Reset();
    SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    fetchLo = IVec3{fx - kSiteR, groundY - 6, fz - kSiteR};
    fetchHi = IVec3{fx + kSiteR, groundY + kSiteTop, fz + kSiteR};
    for (int band = 0; band < kSiteTop; band += 15) {
      std::vector<CellOp> ops;
      for (int dz = -kSiteR; dz <= kSiteR; dz++)
        for (int dx = -kSiteR; dx <= kSiteR; dx++)
          for (int y = groundY + 1 + band; y < groundY + 1 + band + 15; y++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (world.CellInWindow(cc) && ops.size() < kMaxCellOpsPerTick)
              ops.push_back({World::SlotCellIndex(cc), 0u});
          }
      runTick(ops, true);
    }
    {
      std::vector<CellOp> ops;
      for (int dz = -kSiteR; dz <= kSiteR; dz++)
        for (int dx = -kSiteR; dx <= kSiteR; dx++) {
          const int th = World::TerrainHeight(fx + dx, fz + dz, kDefaultSeed);
          for (int y = th + 1; y <= groundY; y++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (world.CellInWindow(cc) && ops.size() < kMaxCellOpsPerTick)
              ops.push_back({World::SlotCellIndex(cc),
                             PackVoxNew(kMatStone, DitherHash(cc.x, y, cc.z) % 3u)});
          }
        }
      runTick(ops, true);
    }
    const int bedTop = sandBed ? groundY + 4 : groundY;
    if (sandBed) {
      std::vector<CellOp> ops;
      for (int dz = -kSiteR; dz <= kSiteR; dz++)
        for (int dx = -kSiteR; dx <= kSiteR; dx++)
          for (int y = groundY + 1; y <= bedTop; y++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (world.CellInWindow(cc) && ops.size() < kMaxCellOpsPerTick)
              ops.push_back({World::SlotCellIndex(cc),
                             PackVoxNew(mSand, DitherHash(cc.x, y, cc.z) % 3u)});
          }
      runTick(ops, true);
    }
    for (int i = 0; i < 40; i++) runTick({}, true);  // sand settles, mirror lands
    std::vector<CellOp> build;
    cutY = bedTop + 1 + 8;  // cut plane: 8 cells up the bole
    const CactusFixture f =
        BuildCactus(world, {fx, bedTop + 1, fz}, mFlesh, mRib, cutY + 2, build);
    runTick(build, true);
    for (int i = 0; i < 14; i++) runTick({}, true);
    return f;
  };

  struct ArmResult {
    const char* name;
    uint32_t before = 0, aboveBefore = 0, aboveAfter = 0, afterTotal = 0;
    uint32_t bodies = 0, maxBody = 0;
    int firstBodyTick = -1;
    float bodyDrop = 0;  // how far the largest body's origin fell, cells
    float dropAfterFetch = 0;  // ...and after 60 more ticks with the mirror forced fresh
    uint32_t vacateTick = 0;   // sim tick the body left the grid
    uint32_t colliderBuilt = 0, cacheVersion = 0;  // for the chunk holding the body
    uint32_t terrainBuilds = 0, terrainSame = 0, terrainDeferred = 0,
            terrainGatherDeferred = 0;
    SweepResult sweep;
    DebrisSystem::FloaterProbe probe;
    bool felled = false, cleared = false;
  };
  auto runArm = [&](const char* name, bool sandBed, bool cpuDoor,
                    bool forceFetch) -> ArmResult {
    ArmResult r;
    r.name = name;
    int cutY = 0;
    const CactusFixture f = plant(sandBed, cutY);
    // ---- THE MIRROR PROLOGUE: pin the ENTRY STATE, not the subject ---------
    //
    // SUBJECT ISOLATION applied to entry state, which is the ca-skip pin's
    // principle aimed at what this arm STARTS from rather than at a mode.
    //
    // The subject is FELLING: the severed top becomes a body and the body
    // falls. `game-arm-body-falls` is the assertion that exists for it, and the
    // quantity it reads was not a statement about felling at all — it was a
    // statement about what the PREVIOUS GATE left in the mirror cache. Measured
    // on one tree, one binary, the same fixture: **13.2 cells standalone
    // against 1.8 inside `--selftest`**, against a floor of 2.0 that the gate's
    // own note says should be cleared by ~11. That spread is not felling
    // physics moving; it is the gate reading a different world in.
    //
    // WHY IT CAN DRIFT AT ALL is already written down twenty lines below: the
    // collider around the body is meshed from a mirror copy, and
    // `ManageTerrain` re-fetches a cached chunk only while the snapshot still
    // shows it dirty and never inside 8 ticks of the last request. `plant()`
    // force-fetches throughout, but `RequestChunkFetch` is a REQUEST — it is
    // served through the snapshot ring, and at suite scope that ring is
    // contended by every gate before this one, so a request can be declined and
    // the arm enters its measured window holding a stale chunk.
    //
    // So: the SAME CALL the epilogue uses (`runTick({}, true)` over the
    // fixture's chunk box), for the same number of ticks the epilogue has
    // already proved is enough for the mirror to actually land, run BEFORE the
    // cut goes in. After it, every arm starts from a mirror that is fresh over
    // this fixture wherever the gate sits in `kOrder`.
    //
    // IT DOES NOT TOUCH THE SUBJECT, and that is the line not to cross. The
    // game arm's defining property is that it gets NO forced fetches once the
    // cut is in — `runTick({}, forceFetch)` with `forceFetch == false` for all
    // 300 ticks of the fall, which is what a player standing in the desert
    // gives the debris system. That is unchanged, and the standalone numbers
    // confirm it: the game arm falls 13.2 cells with the prologue and 13.2
    // without it. The floor stays 2.0.
    //
    // WHAT IT FIXED, AND WHAT IT DID NOT — recorded because the second half is
    // the whole diagnosis and the next person should not re-derive it. At suite
    // scope the prologue moved the CPU arm 2.0 -> 18.4 cells and the STONE arm
    // 1.3 -> 11.6, i.e. both now behave like their standalone selves, which is
    // exactly what pinning entry state is supposed to buy. The GAME arm did
    // NOT move into range (1.8 -> 1.1), and the reason is on the same printed
    // line: its collider for the body's chunk is built from **mirror v0 vs
    // cache v0** while the other two read v96272 and v96773. v0 is "never
    // built" — so for the game arm the collider was not rebuilt after the cut
    // AT ALL, and no amount of mirror freshness before the cut can change that.
    //
    // That rules the mirror cache OUT as the coupling. `ManageTerrain` rebuilds
    // a chunk's collider only while the snapshot still shows it DIRTY, and the
    // prologue deliberately leaves the world settled, so on the game arm —
    // which gets no forced fetches afterwards either — nothing ever re-meshes
    // the chunk the severed top is standing in. The body rests on geometry that
    // still holds it. The epilogue agrees: all three arms fall a further 0.0
    // cells under forced fetches, so each has genuinely come to rest.
    //
    // The remaining question is therefore about the debris system's own
    // collider-build path (NoteGridWrite / pendingVacate_ / ManageTerrain's
    // dirty-and-8-tick gate), not about this fixture, and it belongs to whoever
    // owns that overlay. The prologue stays because it is correct on its own
    // terms and because it is what produced the v0 evidence.
    constexpr int kMirrorPrologueTicks = 70;
    for (int i = 0; i < kMirrorPrologueTicks; i++) runTick({}, true);
    const IVec3 aboveLo{f.lo.x, cutY + 3, f.lo.z};
    r.before = countMat(mFlesh, f.lo, f.hi) + countMat(mRib, f.lo, f.hi);
    r.aboveBefore = countMat(mFlesh, aboveLo, f.hi) + countMat(mRib, aboveLo, f.hi);
    debris.ResetFloaterProbe();
    {
      std::vector<CellOp> cut;
      for (int y = cutY; y < cutY + 3; y++)
        for (int dz = -5; dz <= 5; dz++)
          for (int dx = -5; dx <= 5; dx++) {
            const IVec3 cc{fx + dx, y, fz + dz};
            if (!world.CellInWindow(cc)) continue;
            cut.push_back({World::SlotCellIndex(cc), 0u});
          }
      runTick(cut, forceFetch);
    }
    if (cpuDoor)
      debris.AddDestructionEvent(tick, {fx - 5, cutY, fz - 5},
                                 {fx + 5, cutY + 2, fz + 5});
    float bodyY0 = 0;
    uint32_t bestIdx = 0;
    for (int i = 0; i < 300; i++) {
      runTick({}, forceFetch);
      r.bodies = std::max(r.bodies, debris.BodyCount());
      for (uint32_t b = 0; b < debris.BodyCount(); b++) {
        const uint32_t n = debris.BodyVoxelCount(b);
        if (n > r.maxBody) {
          r.maxBody = n;
          bestIdx = b;
          bodyY0 = debris.BodyPosition(b).y;
          if (r.firstBodyTick < 0) {
            r.firstBodyTick = i;
            r.vacateTick = tick;
          }
        }
      }
      if (r.maxBody && bestIdx < debris.BodyCount() &&
          debris.BodyVoxelCount(bestIdx) == r.maxBody)
        r.bodyDrop = bodyY0 - debris.BodyPosition(bestIdx).y;
    }
    if (r.maxBody && bestIdx < debris.BodyCount()) {
      const Vec3 bp = debris.BodyPosition(bestIdx);
      const IVec3 wc{ifloor(bp.x + 4.0f) >> 4, ifloor(bp.y + 8.0f) >> 4,
                     ifloor(bp.z + 4.0f) >> 4};
      r.colliderBuilt = debris.TerrainBuiltVersion(wc);
      const CachedChunk* cc = world.Cached(wc);
      r.cacheVersion = cc ? cc->version : 0;
    }
    r.terrainBuilds = debris.Settle().terrainBuilds;
    r.terrainSame = debris.Settle().terrainSame;
    r.terrainDeferred = debris.Settle().terrainDeferred;
    r.terrainGatherDeferred = debris.Settle().terrainGatherDeferred;
    {
      const float y1 = (r.maxBody && bestIdx < debris.BodyCount())
                           ? debris.BodyPosition(bestIdx).y
                           : 0.0f;
      for (int i = 0; i < 70; i++) runTick({}, true);  // mirror forced fresh
      if (r.maxBody && bestIdx < debris.BodyCount() &&
          debris.BodyVoxelCount(bestIdx) == r.maxBody)
        r.dropAfterFetch = y1 - debris.BodyPosition(bestIdx).y;
    }
    r.aboveAfter = countMat(mFlesh, aboveLo, f.hi) + countMat(mRib, aboveLo, f.hi);
    r.afterTotal = countMat(mFlesh, f.lo, f.hi) + countMat(mRib, f.lo, f.hi);
    r.sweep = SweepForFloaters(world, c.mats, fetchLo, fetchHi, 1);
    r.probe = debris.Floaters();
    r.felled = r.maxBody * 2 >= r.aboveBefore;
    r.cleared = r.sweep.bigVoxels == 0;
    return r;
  };

  const ArmResult game = runArm("game", true, false, false);
  const ArmResult cpu = runArm("cpu", true, true, true);
  const ArmResult stone = runArm("stone", false, true, true);

  note(siteInWindow, "fixture-in-window");
  note(game.before > 200, "cactus-was-written");
  note(game.felled, "game-arm-fells-the-top");
  // THE ASSERTION THIS GATE EXISTS FOR. Making the body is not felling the
  // cactus: the body has to FALL. In the game arm it is made within three
  // ticks and then stands in the air for the rest of the run, because the
  // terrain collider around it was built from a mirror copy that still held
  // the severed top (ManageTerrain re-fetches a cached chunk only while the
  // snapshot shows it dirty, and never inside 8 ticks of the last request; the
  // vacated chunk is asleep again before that window opens). The forced-fetch
  // epilogue proves it: the same body drops a dozen cells the moment the
  // mirror is refreshed.
  note(game.bodyDrop >= (float)BaselineNumber("cactusFell.minDropCells", 2.0),
       "game-arm-body-falls");
  RecordObserved("cactusFell.gameBodyDrop", (double)game.bodyDrop);
  RecordObserved("cactusFell.gameDropAfterFetch", (double)game.dropAfterFetch);
  note(game.cleared, "game-arm-leaves-nothing-floating");
  note(cpu.felled, "cpu-arm-fells-the-top");
  note(stone.felled, "stone-arm-fells-the-top");
  RecordObserved("cactusFell.gameBodyVoxels", (double)game.maxBody);
  RecordObserved("cactusFell.cpuBodyVoxels", (double)cpu.maxBody);
  RecordObserved("cactusFell.stoneBodyVoxels", (double)stone.maxBody);

  auto armLine = [&](const ArmResult& r) {
    const DebrisSystem::FloaterProbe& p = r.probe;
    return Format(
        "[%s: cactus %u cells, %u above the cut -> %u above / %u total after; "
        "bodies %u, largest %u vox (first at +%d, fell %.1f cells); sweep "
        "unsupported %u/%u/%u (%u vox, %u chunks absent), biggest %zu at "
        "(%d,%d,%d) rests %d why %d; probe scans %u visited %llu, anchors "
        "boundary %u / unknown %u / oversize %u, small powder-anchored %u, "
        "freed %u, stuck dropped %u requeued %u, cooldown held %u rearmed %u, "
        "spilled %u, defer cellop %u ring %u oversize %u, in-place %u; "
        "COLLIDER for the body chunk built from mirror v%u vs cache v%u, "
        "cells vacated at tick %u; terrain builds %u same %u deferred %u "
        "(gather-deferred %u); "
        "after 70 ticks of forced fetches the body fell a further %.1f cells]",
        r.name, r.before, r.aboveBefore, r.aboveAfter, r.afterTotal, r.bodies,
        r.maxBody, r.firstBodyTick, r.bodyDrop, r.sweep.singles,
        r.sweep.smallComps, r.sweep.bigComps, r.sweep.bigVoxels,
        r.sweep.chunksMissing, r.sweep.biggest, r.sweep.biggestLo.x,
        r.sweep.biggestLo.y, r.sweep.biggestLo.z, r.sweep.biggestRests ? 1 : 0,
        r.sweep.biggestRestWhy, p.scans, (unsigned long long)p.scanCellsVisited,
        p.anchoredByRegionBoundary, p.anchoredByUnknownChunk,
        p.anchoredByOversizeFlood, p.smallAnchoredPowder, p.smallUnanchored,
        p.stuckEventDropped, p.stuckEventRequeued, p.supportLateHeld,
        p.supportLateRearmed, p.eventQueueFullSpilled, p.deferredCellOpBudget,
        p.deferredSpawnRing, p.deferredOversize, p.solidRubbleInPlace,
        r.colliderBuilt, r.cacheVersion, r.vacateTick, r.terrainBuilds,
        r.terrainSame, r.terrainDeferred, r.terrainGatherDeferred,
        r.dropAfterFetch);
  };
  const bool ok = failed.empty();
  detail = Format("at (%d,%d) ground %d inWindow %d %s %s %s%s%s", fx, fz,
                  groundY, siteInWindow ? 1 : 0, armLine(game).c_str(),
                  armLine(cpu).c_str(), armLine(stone).c_str(),
                  failed.empty() ? "" : "; FAILED: ", failed.c_str());
  std::printf("cactus-fell: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());

  debris.Reset();
  SubmitWorldgen(c.ctx, world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& FloaterGates() {
  static const std::vector<Gate> g = {
      {"floaters", "phys", {}, false, GateFloaters},
      {"tree-fell", "phys", {}, false, GateTreeFell},
      {"cactus-fell", "phys", {}, false, GateCactusFell},
  };
  return g;
}

}  // namespace selftest
