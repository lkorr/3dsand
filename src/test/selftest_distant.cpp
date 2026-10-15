// =============================================================================
// distant-walk / distant-sleep -- the distant tier WALKS and SLEEPS
// (docs/PLAN_distant_npcs.md package D1, §4; game/distant.h, distant.cpp).
// =============================================================================
//
// distant-walk -- every claim the plan's §4 row makes, one arm each:
//
//   W. THE LONG WALK, ON `peaks`. A distant human with a Goto 1,150 voxels
//      across the tall-terrain fixture (the map `altitude` loads), the line
//      chosen as the gentlest of 16 headings that still climbs or falls
//      >= 40 voxels: it walks >= distantWalkMinVox, makes >= 80% of it as
//      progress, and at EVERY tick its feet are on DistantGround (y ==
//      TerrainTop + 1 at its column, unless afloat). CPU only: the window and
//      the GPU stay on the harness map, so DIRECT PHASE CALLS ON PURPOSE
//      (W2-O): this arm drives MobSystem::TickDistant alone -- the walk is
//      over ground no window holds, which is the point of the tier.
//   A. ANCHORED. Demoted 3 m up (ANCHORED, D0's rule) with a goal 200 voxels
//      away: 200 ticks later its origin is bit-identical (it may turn).
//   X. THE MID-AIR LANDING (§2.4). Spawned 2 m up, demoted while falling: its
//      first distant step puts it on the ground, billing ApplyFallDamage's
//      own formula (impact from the drop and vel.y) spread over its limbs as
//      SpendHp spreads it; at a splat speed it dies of "fall".
//   P. THE PROMOTION LINE AND THE SNAP (real tick). A walker outside the +X
//      face with a goal deep inside the window and NO promotion (no parking):
//      it crosses the line by at most distantLineHoldMaxVox and is held there
//      (lineHolds > 0). A stone bump is laid where it waits -- ground
//      TerrainHeight does not know -- and promotion is switched on: it comes
//      back under its own id ON the bump (snapped up, error <=
//      distantPromoteMaxDropVox), with every limb's hp unchanged over 30
//      ticks and no fall billed.
//   C. THE CRATER AND THE EVICTION DRAIN (§2.4 item 3, audit A3). A crater is
//      dug in the window's -X plane, marked modified, and the window SHIFTS
//      one chunk +X (Stream::Update, DIRECT ON PURPOSE: the rig pins the
//      window) so the crater's chunk is evicted -- its readback still in
//      flight. A distant walker crosses it twice from the same state: run 1
//      reads the store while the eviction is PENDING (the drain completes
//      it), run 2 after, with the ground cache cleared. The two traces are
//      identical, and both walk DOWN into the crater (>= 3 voxels below the
//      untouched ground): the edit is real at distance.
//   S. WATER (harness map, the home lake). A shark anchored at the shore never
//      takes a step out of the water (waterRefusals counted, every tick's
//      column deep enough); a human walking across the lake swims (swim
//      steps > 0) at no more than its water speed.
//   D. TWICE-RUN DIGESTS: W and C each run twice from one saved state; the
//      per-tick traces (origin + heading bits) are equal.
//
// distant-sleep:
//   1. 200 idle distant duelists (one faction: nobody hostile to anybody) on
//      a grid outside the window: awake count 0 within distantSleepMaxTicks.
//   2. Asleep, the tier costs < distantIdleMaxUs a tick (TickDistant's own
//      wall clock, a measurement, never an input), and nobody steps.
//   3. A walker crossing the grid wakes EXACTLY the sleepers whose cell is
//      within one cell of a cell it stood in -- not one more, not one fewer.
//
// LEAVES NOTHING: tuning and the map restored, mobs / debris reset, park
// function removed, id counter put back, store cleared, the window shifted
// back and worldgen regenerated at home, ground caches dropped.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "game/distant.h"
#include "game/mob.h"
#include "game/persist.h"
#include "sim/biomes.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

// FNV-1a 64 over raw bytes: the trace digests.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
};

// The tier switched on with the gate's own numbers, whatever tuning.json says.
void SetTier(bool on) {
  Tuning t = CurrentTuning();
  t.distant.enabled = on;
  t.distant.radius = 1280.0f;
  t.distant.hysteresis = 256.0f;
  t.distant.idleStride = 4;
  t.distant.surfaceSnapMaxVox = 24.0f;
  t.distant.promoteMaxDropVox = 48.0f;
  SetCurrentTuning(t);
}

// Everything a gate in this file needs to build distant creatures.
struct Fix {
  Ctx& c;
  int human = -1;
  IVec3 home{};
  Vec3 wlo{};
  int baseX = 0, baseZ = 0;   // the window centre column (the spawn spot)

  explicit Fix(Ctx& ctx) : c(ctx) {
    human = c.mobs.FindDef("human");
    home = c.world.WindowOrigin();
    wlo = Vec3{(float)(home.x * (int)kChunk), (float)(home.y * (int)kChunk),
               (float)(home.z * (int)kChunk)};
    baseX = (int)wlo.x + (int)kWorldN / 2;
    baseZ = (int)wlo.z + (int)kWorldN / 2;
  }
  Vec3 Size(int def) const { return c.mobs.Defs()[(size_t)def].worldSize; }
  // A creature of `def` on profile `prof`, demoted where it was spawned
  // (`yUp` voxels over the ground at the window centre) through the park
  // rule's own demotion (MobSystem::DemoteForTest), then -- if `to` is given
  // -- moved to stand at column (to.x, to.z) on its ground (`to.y` voxels
  // over it). 0 when anything refused.
  uint64_t Make(int def, const char* prof, int yUp = 0, const Vec3* to = nullptr,
                int ticksFirst = 0, support::TickCursor* rt = nullptr) {
    const int gx = baseX, gz = baseZ;
    const uint64_t id =
        c.mobs.Spawn(def, {gx, World::TerrainHeight(gx, gz, kDefaultSeed) + 1 + yUp, gz});
    if (id == 0) return 0;
    if (prof != nullptr && prof[0] != 0) c.mobs.SetMobBehavior(id, prof);
    for (int k = 0; k < ticksFirst && rt != nullptr; k++) (*rt)();
    if (!c.mobs.DemoteForTest(id)) return 0;
    if (to != nullptr) Place(id, to->x, to->z, to->y);
    return id;
  }
  // Stand distant creature `id` with its footprint centre at column (x, z),
  // `up` voxels over the ground there (TerrainTop + 1 + up).
  void Place(uint64_t id, float x, float z, float up = 0.0f) {
    DistantMobs& tier = c.mobs.DistantMut();
    const DistantMob* d = tier.Find(id);
    if (d == nullptr) return;
    const Vec3 ws = d->worldSize;
    const int cx = ifloor(x), cz = ifloor(z);
    const float gy = (float)(c.mobs.DistantGroundMut().TerrainTop(cx, cz) + 1) + up;
    tier.Relocate(id, Vec3{(float)cx + 0.5f - ws.x * 0.5f, gy, (float)cz + 0.5f - ws.z * 0.5f});
  }
  void Goto(uint64_t id, Vec3 goal, float speed = 1.0f) {
    ai::Brain* b = c.mobs.BrainAnyTierMut(id);
    if (b == nullptr) return;
    b->routine = ai::Routine{};
    b->routine.active = true;
    b->routine.verb = ai::Intent::Goto;
    b->routine.goal = goal;
    b->routine.final = true;
    b->routine.arriveRadius = 4.0f;
    b->routine.speed = speed;
  }
};

// One distant creature's state, saved and put back whole (Erase + Insert:
// the hash files it afresh) -- the twice-run arms start both runs from it.
DistantMob Save(MobSystem& mobs, uint64_t id) {
  const DistantMob* d = mobs.Distant().Find(id);
  return d != nullptr ? *d : DistantMob{};
}
void Restore(MobSystem& mobs, const DistantMob& saved) {
  DistantMobs& tier = mobs.DistantMut();
  tier.Erase(saved.id);
  DistantMob copy = saved;
  tier.Insert(std::move(copy));
}

// ---- distant-walk ----------------------------------------------------------------
Status GateDistantWalk(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  EntityParking parking;
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) fails.push_back(why);
    return ok;
  };
  std::string laps;
  auto lapT0 = std::chrono::steady_clock::now();
  auto lap = [&](const char* arm) {
    const auto now = std::chrono::steady_clock::now();
    laps += Format("%s%s %lld", laps.empty() ? "" : " ", arm,
                   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - lapT0)
                       .count());
    lapT0 = now;
  };
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetItems(&c.items);
  c.mobs.SetParkFn(nullptr);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  SetTier(true);
  c.mobs.SetDistantStream(&c.stream);
  Fix fx(c);
  const IVec3 home = fx.home;
  const Vec3 wlo = fx.wlo;
  const int n = (int)kNChunk;
  if (fx.human < 0) {
    detail = "no 'human' def";
    SetCurrentTuning(tuneWas);
    return Status::Fail;
  }
  DistantGround& ground = c.mobs.DistantGroundMut();
  uint32_t t = 9400;
  IVec3 pc{home.x + n / 2, World::TerrainHeight(fx.baseX, fx.baseZ, kDefaultSeed) >> 4,
           home.z + n / 2};
  support::TickCursor realTick{c, t, pc};
  auto direct = [&]() {
    // DIRECT PHASE CALLS ON PURPOSE (W2-O): the arms that walk the tier over
    // ground no window holds (or a window the gate has just shifted) step the
    // distant tier alone; see the file header.
    ++t;
    c.mobs.TickDistant(t, c.world, nullptr);
  };
  // ...and a twice-run arm replays the SAME tick numbers (every clock a brain
  // keeps is absolute), on its own counter; the gate's `t` jumps past both.
  auto directAt = [&](uint32_t tk) { c.mobs.TickDistant(tk, c.world, nullptr); };
  const DistantMobs& tier = c.mobs.Distant();
  const float minWalk = (float)BaselineNumber("distantWalkMinVox", 1000);
  const float maxHold = (float)BaselineNumber("distantLineHoldMaxVox", 8);
  const float maxSnap = (float)BaselineNumber("distantPromoteMaxDropVox", 48);
  const Vec3 hs = fx.Size(fx.human);

  // ======== W: the long walk on `peaks` =======================================
  float wWalked = 0, wProgress = 0, wMinY = 1e9f, wMaxY = -1e9f;
  int wOffGround = 0, wTicks = 0, wHeadingDeg = -1, wRange = 0;
  uint64_t wDigest[2] = {0, 0};
  uint32_t wReplans = 0;
  uint64_t wClimb = 0, wDrop = 0;
  bool wLoaded = false;
  std::string wWhy;
  {
    const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
    biomes::BiomeSet set;
    std::string log;
    worldmap::WorldMapData pk;
    if (biomes::LoadBiomeSet(AssetDir(), c.mats, set, log) &&
        worldmap::LoadWorldMap(AssetDir(), "peaks", set, c.mats.size(), kDefaultSeed, pk, log)) {
      wLoaded = true;
      // A walker made at home (the window's terrain), then carried to peaks.
      const uint64_t id = fx.Make(fx.human, "duelist");
      worldmap::SetCurrentWorldMap(pk);
      const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
      const float sx = (float)m.spawnX + 0.5f, sz = (float)m.spawnZ + 0.5f;
      // THE LINE: the gentlest of 16 headings (the steepest 2-voxel step along
      // it) that still climbs or falls 40 voxels -- varied ground the body's
      // own budgets can walk.
      const float kLen = 1150.0f;
      int bestDir = -1, bestStep = 1 << 30, bestRange = 0;
      for (int k = 0; k < 16; k++) {
        const float a = (float)k * (6.2831853f / 16.0f);
        int prev = ground.TerrainTop(ifloor(sx), ifloor(sz)), worst = 0, lo = prev, hi = prev;
        for (float s = 2.0f; s <= kLen; s += 2.0f) {
          const int hh = ground.TerrainTop(ifloor(sx + std::sin(a) * s), ifloor(sz + std::cos(a) * s));
          worst = std::max(worst, std::abs(hh - prev));
          lo = std::min(lo, hh);
          hi = std::max(hi, hh);
          prev = hh;
        }
        const bool varied = hi - lo >= 40;
        const bool better = bestDir < 0 || (varied && (bestRange < 40 || worst < bestStep)) ||
                            (!varied && bestRange < 40 && worst < bestStep);
        if (better) {
          bestDir = k;
          bestStep = worst;
          bestRange = hi - lo;
        }
      }
      wHeadingDeg = bestDir * 360 / 16;
      wRange = bestRange;
      std::printf("distant-walk: W line heading %d deg, steepest 2-voxel step %d, range %d\n",
                  wHeadingDeg, bestStep, bestRange);
      const float a = (float)bestDir * (6.2831853f / 16.0f);
      const Vec3 goal{sx + std::sin(a) * kLen, 0.0f, sz + std::cos(a) * kLen};
      fx.Place(id, sx, sz);
      fx.Goto(id, goal, 1.0f);
      const DistantMob start = Save(c.mobs, id);
      const DistantMobs::Stats s0 = tier.GetStats();
      const uint32_t tBase = t + 1;
      uint32_t tEnd = tBase;
      for (int run = 0; run < 2; run++) {
        if (run == 1) Restore(c.mobs, start);
        Digest dg;
        int ticks = 0;
        for (; ticks < 4000; ticks++) {
          directAt(tBase + (uint32_t)ticks);
          tEnd = std::max(tEnd, tBase + (uint32_t)ticks);
          const DistantMob* d = tier.Find(id);
          if (d == nullptr) break;
          dg.F(d->origin.x);
          dg.F(d->origin.y);
          dg.F(d->origin.z);
          dg.F(d->steer.heading);
          if (run == 0 && ticks % 150 == 0) {
            const Vec3 cc = d->Centre();
            std::printf("distant-walk: W t%4d at (%.1f, %.1f, %.1f) goal dist %.0f heading %.2f "
                        "desired %.2f intent %s path %zu/%zu valid %d navFailed %d walked %.0f "
                        "swim %d asleep %d steps %u\n",
                        ticks, (double)cc.x, (double)d->origin.y, (double)cc.z,
                        (double)std::sqrt((goal.x - cc.x) * (goal.x - cc.x) +
                                          (goal.z - cc.z) * (goal.z - cc.z)),
                        (double)d->steer.heading, (double)d->steer.desiredHeading,
                        ai::IntentName(d->brain.intent), d->brain.path.cursor,
                        d->brain.path.pts.size(), d->brain.path.valid ? 1 : 0,
                        d->brain.navFailed ? 1 : 0, (double)d->walked, d->swimming ? 1 : 0,
                        d->asleep ? 1 : 0, d->steps);
          }
          if (run == 0) {
            const Vec3 cc = d->Centre();
            const float gy = (float)(ground.TerrainTop(ifloor(cc.x), ifloor(cc.z)) + 1);
            if (!d->swimming && d->origin.y != gy) {
              wOffGround++;
              if (wWhy.empty())
                wWhy = Format("tick %d: feet y %.2f, ground %.0f at (%d,%d)", ticks, (double)d->origin.y,
                              (double)gy, ifloor(cc.x), ifloor(cc.z));
            }
            wMinY = std::min(wMinY, d->origin.y);
            wMaxY = std::max(wMaxY, d->origin.y);
          }
          if (d->walked >= kLen - 30.0f || (d->brain.routine.arrived)) break;
        }
        wDigest[run] = dg.h;
        if (run == 0) {
          const DistantMob* d = tier.Find(id);
          if (d != nullptr) {
            wWalked = d->walked;
            const float dx = d->origin.x - start.origin.x, dz = d->origin.z - start.origin.z;
            wProgress = (dx * std::sin(a) + dz * std::cos(a));
            wReplans = d->brain.replans;
          }
          wTicks = ticks;
          const DistantMobs::Stats s1 = tier.GetStats();
          wClimb = s1.climbRefusals - s0.climbRefusals;
          wDrop = s1.dropRefusals - s0.dropRefusals;
        }
      }
      t = tEnd;
      c.mobs.DistantMut().Erase(id);
      worldmap::SetCurrentWorldMap(real);
    } else {
      wWhy = "the peaks map did not load: " + log;
    }
  }
  const bool okW = check(wLoaded && wWalked >= minWalk && wProgress >= 0.8f * minWalk &&
                             wOffGround == 0 && wRange >= 40,
                         Format("W: walked %.0f (want >= %.0f), progress %.0f, off-ground %d, "
                                "range %d%s%s",
                                (double)wWalked, (double)minWalk, (double)wProgress, wOffGround,
                                wRange, wWhy.empty() ? "" : ", first: ", wWhy.c_str()));
  const bool okDW = check(wDigest[0] == wDigest[1] && wDigest[0] != 0,
                          Format("D(W): trace digests %016llx / %016llx",
                                 (unsigned long long)wDigest[0], (unsigned long long)wDigest[1]));
  lap("W");

  // ======== A: anchored ======================================================
  bool aSame = false, aAnchored = false;
  float aTurn = 0.0f;
  {
    const uint64_t id = fx.Make(fx.human, "duelist", 31);
    const DistantMob* d = tier.Find(id);
    aAnchored = d != nullptr && d->groundMode == DistantGroundMode::Anchored;
    if (d != nullptr) {
      // Out of the window, still 3 m up (its mode was decided at demotion).
      fx.Place(id, wlo.x - 120.0f, wlo.z + 200.0f, 31.0f);
      fx.Goto(id, Vec3{wlo.x - 320.0f, 0.0f, wlo.z + 200.0f});
      const DistantMob before = Save(c.mobs, id);
      for (int k = 0; k < 200; k++) direct();
      if (const DistantMob* e = tier.Find(id)) {
        aSame = std::memcmp(&e->origin, &before.origin, sizeof(Vec3)) == 0;
        aTurn = std::fabs(std::remainder(e->steer.heading - before.steer.heading, 6.2831853f));
      }
      c.mobs.DistantMut().Erase(id);
    }
  }
  const bool okA = check(aAnchored && aSame,
                         Format("A: anchored %d, origin unchanged over 200 ticks %d", aAnchored, aSame));
  lap("A");

  // ======== X: the mid-air landing ===========================================
  int xHurtOk = 0, xDeadOk = 0, xSoftOk = 0;
  float xLost = 0, xWant = 0;
  std::string xWhy;
  {
    const auto& pt = CurrentTuning().player;
    const float g = MetresToCells(CurrentTuning().physics.gravity);
    // Three demotions in the air, each given its vertical speed before the
    // first step: 0 (a short soft drop), 12 m/s, 40 m/s (past the splat).
    const float speeds[3] = {0.0f, -120.0f, -400.0f};
    for (int k = 0; k < 3; k++) {
      // 8 real ticks: a fresh spawn's gravity WAITS on a column the CPU mirror
      // has not delivered (MobSystem::UpdateFall's groundUnknown), ~K ticks,
      // and from 2 m up it is still falling after 8.
      const uint64_t id = fx.Make(fx.human, "duelist", 20, nullptr, 8, &realTick);
      DistantMob* d = c.mobs.DistantMut().Find(id);
      if (d == nullptr || !d->landPending) {
        if (xWhy.empty()) xWhy = Format("case %d: not demoted in the air", k);
        if (d != nullptr) c.mobs.DistantMut().Erase(id);
        continue;
      }
      // Off the window, keeping its height over the ground, so nothing the
      // window holds is under it.
      const float up = d->origin.y - (float)(ground.TerrainTop(ifloor(d->Centre().x),
                                                                ifloor(d->Centre().z)) + 1);
      fx.Place(id, wlo.x - 150.0f - 40.0f * (float)k, wlo.z + 300.0f, up);
      d = c.mobs.DistantMut().Find(id);
      d->vel.y = speeds[k];
      float before = 0;
      for (size_t i = 0; i < d->limbHp.size(); i++)
        if ((d->limbRole[i].flags & kDistantLimbAttached) && !(d->limbRole[i].flags & kDistantLimbBloodless))
          before += d->limbHp[i];
      const Vec3 cc = d->Centre();
      const float gy = (float)(ground.TerrainTop(ifloor(cc.x), ifloor(cc.z)) + 1);
      const float drop = std::max(0.0f, d->origin.y - gy);
      const float impactMs = std::sqrt(speeds[k] * speeds[k] + 2.0f * g * drop) * kVoxelMeters;
      direct();
      d = c.mobs.DistantMut().Find(id);
      if (d == nullptr) continue;
      float after = 0;
      for (size_t i = 0; i < d->limbHp.size(); i++)
        if ((d->limbRole[i].flags & kDistantLimbAttached) && !(d->limbRole[i].flags & kDistantLimbBloodless))
          after += d->limbHp[i];
      const bool onGround = d->origin.y == gy && !d->landPending;
      const float excess = impactMs - pt.fallDamageSpeed;
      const float bill = impactMs < pt.fallDamageSpeed ? 0.0f : (float)std::lround(excess * excess * pt.fallDamageScale);
      const bool lethal = impactMs >= pt.fallSplatSpeed || bill >= std::floor(before);
      if (k == 0) {
        xSoftOk = onGround && !d->dead && (bill == 0.0f ? after == before : true);
      } else if (!lethal) {
        xLost = before - after;
        xWant = bill;
        xHurtOk = onGround && !d->dead && std::fabs(xLost - bill) <= 1.0f && bill > 0.0f;
      } else {
        xDeadOk = onGround && d->dead && d->cause == "fall";
      }
      if (xWhy.empty() && !(k == 0 ? xSoftOk : (!lethal ? xHurtOk : xDeadOk)))
        xWhy = Format("case %d: impact %.1f m/s, on ground %d, dead %d, lost %.1f (bill %.0f)", k,
                      (double)impactMs, onGround, d->dead, (double)(before - after), (double)bill);
      c.mobs.DistantMut().Erase(id);
    }
  }
  // Case 1 (12 m/s) must be a hurt or (if the tuning splats it) a death; the
  // claim is that the near tier's formula decided which.
  const bool okX = check(xSoftOk && (xHurtOk || xDeadOk) && xDeadOk,
                         Format("X: soft %d, hurt %d (lost %.1f, bill %.0f), splat %d%s%s", xSoftOk,
                                xHurtOk, (double)xLost, (double)xWant, xDeadOk,
                                xWhy.empty() ? "" : ", first: ", xWhy.c_str()));
  lap("X");

  // ======== P: the promotion line, then the snap ==============================
  float pDepthMax = -1.0f, pErr = -1.0f, pHpDiff = -1.0f;
  bool pReached = false, pPromoted = false, pUp = false;
  uint64_t pHolds = 0;
  uint32_t pFalls = 0;
  int pTicks = 0;
  {
    const int m = EntityParking::kUnparkMarginChunks;
    const float lineX = (float)((home.x + n - m) * (int)kChunk);   // the +X promotion face
    const float lo[3] = {(float)((home.x + m) * (int)kChunk), (float)((home.y + m) * (int)kChunk),
                         (float)((home.z + m) * (int)kChunk)};
    const float hi[3] = {lineX, (float)((home.y + n - m) * (int)kChunk),
                         (float)((home.z + n - m) * (int)kChunk)};
    auto depthOf = [&](Vec3 p) {
      if (!(p.x >= lo[0] && p.x < hi[0] && p.y >= lo[1] && p.y < hi[1] && p.z >= lo[2] && p.z < hi[2]))
        return -1.0f;
      const float v[3] = {p.x, p.y, p.z};
      float dd = 1e9f;
      for (int a = 0; a < 3; a++) dd = std::min(dd, std::min(v[a] - lo[a], hi[a] - v[a]));
      return dd;
    };
    const Vec3 at{wlo.x + (float)kWorldN + 24.0f, 0.0f, (float)fx.baseZ};
    const uint64_t id = fx.Make(fx.human, "duelist", 0, &at);
    fx.Goto(id, Vec3{(float)fx.baseX, 0.0f, (float)fx.baseZ});
    const DistantMobs::Stats s0 = tier.GetStats();
    for (int k = 0; k < 150; k++) {
      realTick();
      const DistantMob* d = tier.Find(id);
      if (d == nullptr) break;
      const float dd = depthOf(d->origin);
      if (dd >= 0.0f) {
        pReached = true;
        pDepthMax = std::max(pDepthMax, dd);
      }
    }
    pHolds = tier.GetStats().lineHolds - s0.lineHolds;
    // THE BUMP: stone where it waits, painted into air only -- ground the
    // real window has and TerrainHeight does not.
    const DistantMob* d = tier.Find(id);
    if (d != nullptr) {
      const Vec3 cc = d->Centre();
      const float distantY = d->origin.y;
      support::TickOps bump;
      bump.ops.push_back(BrushOp{ifloor(cc.x), ifloor(distantY), ifloor(cc.z), 3, kMatStone, 0});
      realTick(bump);
      for (int k = 0; k < 4; k++) realTick();
      // PROMOTION ON: the tick's own door (phase H Unpark), as the game's.
      parking.Bind(c.mobs, c.stream.Store(), true);
      realTick.Rig().Authority().parking = &parking;
      const float yBefore = tier.Find(id) != nullptr ? tier.Find(id)->origin.y : distantY;
      std::vector<float> hpBefore;
      if (const DistantMob* e = tier.Find(id)) hpBefore = e->limbHp;
      for (int k = 0; k < 90 && c.mobs.FindMobById(id) == nullptr; k++, pTicks++) realTick();
      if (const Mob* mb = c.mobs.FindMobById(id)) {
        pPromoted = true;
        pErr = std::fabs(mb->Origin().y - yBefore);
        pUp = mb->Origin().y > yBefore;
        for (int k = 0; k < 30; k++) realTick();
        if (const Mob* m2 = c.mobs.FindMobById(id)) {
          float diff = 0;
          for (size_t li = 0; li < hpBefore.size() && li < c.mobs.Defs()[(size_t)fx.human].limbs.size(); li++)
            diff = std::max(diff, std::fabs(c.mobs.LimbHp(id, (int)li) - hpBefore[li]));
          pHpDiff = diff;
          pFalls = m2->FallsBilled();
        }
      }
      realTick.Rig().Authority().parking = nullptr;
      c.mobs.SetParkFn(nullptr);
    }
  }
  const bool okP = check(pReached && pDepthMax <= maxHold && pHolds > 0 && pPromoted &&
                             pErr <= maxSnap && pUp && pHpDiff == 0.0f && pFalls == 0,
                         Format("P: reached the line %d, deepest %.2f (max %.0f), holds %llu, "
                                "promoted %d after %d ticks, snap %.2f up %d (max %.0f), hp drift "
                                "%.3f, falls %u",
                                pReached, (double)pDepthMax, (double)maxHold,
                                (unsigned long long)pHolds, pPromoted, pTicks, (double)pErr, pUp,
                                (double)maxSnap, (double)pHpDiff, pFalls));
  c.mobs.Reset();
  c.mobs.SetDistantStream(&c.stream);
  lap("P");

  // ======== C: the crater and the eviction drain ===============================
  uint64_t cDigest[2] = {0, 0};
  uint64_t cDrains = 0;
  bool cShifted = false, cPending = false;
  float cDip = 0.0f;
  std::string cWhy;
  {
    const IVec3 wo = c.world.WindowOrigin();
    // The middle of a chunk on both axes, so the bowl (radius 6) is one
    // chunk column: x in the window's -X plane, z mid-window.
    const int x0 = wo.x * (int)kChunk + 8, z0 = ((fx.baseZ >> 4) << 4) + 8;
    const int h0 = World::TerrainHeight(x0, z0, kDefaultSeed);
    support::TickOps dig;
    dig.ops.push_back(BrushOp{x0, h0, z0, 6, kMatAir, 1});
    realTick(dig);
    for (int k = 0; k < 3; k++) realTick();
    // Modified, explicitly (the worldio gates' idiom): the eviction keeps it.
    c.stream.MarkModifiedBox({x0 - 8, h0 - 10, z0 - 8}, {x0 + 7, h0 + 8, z0 + 7});
    for (int k = 0; k < 4; k++) realTick();
    // ONE +X SHIFT (DIRECT ON PURPOSE: the rig pins the window): the interest
    // three chunks past centre, the -X plane -- the crater's -- leaves.
    const IVec3 craterChunk{wo.x, h0 >> 4, z0 >> 4};
    c.stream.Update(IVec3{wo.x + n / 2 + 3, wo.y + n / 2, wo.z + n / 2}, ++t);
    cShifted = c.world.WindowOrigin().x == wo.x + 1;
    cPending = c.stream.EvictionPending(craterChunk) ||
               c.stream.EvictionPending({craterChunk.x, craterChunk.y - 1, craterChunk.z});
    // The walker: in the evicted plane (outside the window now), crossing
    // the crater along z.
    const Vec3 at{(float)x0 + 0.5f, 0.0f, (float)z0 - 30.0f};
    const uint64_t id = fx.Make(fx.human, "duelist", 0, &at);
    fx.Goto(id, Vec3{(float)x0 + 0.5f, 0.0f, (float)z0 + 30.0f});
    const DistantMob start = Save(c.mobs, id);
    const float untouched = (float)(h0 + 1);
    float minY[2] = {1e9f, 1e9f};
    const uint32_t tBase = t + 1;
    for (int run = 0; run < 2; run++) {
      if (run == 1) {
        Restore(c.mobs, start);
        c.mobs.DistantGroundMut().InvalidateAll();
      }
      const uint64_t dr0 = ground.GetStats().drains;
      Digest dg;
      for (int k = 0; k < 160; k++) {
        directAt(tBase + (uint32_t)k);
        const DistantMob* d = tier.Find(id);
        if (d == nullptr) break;
        dg.F(d->origin.x);
        dg.F(d->origin.y);
        dg.F(d->origin.z);
        dg.F(d->steer.heading);
        if (std::fabs(d->Centre().z - (float)z0) < 3.0f) minY[run] = std::min(minY[run], d->origin.y);
      }
      cDigest[run] = dg.h;
      if (run == 0) cDrains = ground.GetStats().drains - dr0;
    }
    t = tBase + 160;
    cDip = untouched - std::min(minY[0], minY[1]);
    if (minY[0] != minY[1]) cWhy = Format("min y %.1f / %.1f", (double)minY[0], (double)minY[1]);
    c.mobs.DistantMut().Erase(id);
    // The window and the world back where they were.
    c.stream.OnRegen();
    c.world.SetWindowOrigin(home);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  }
  const bool okC = check(cShifted && cDigest[0] == cDigest[1] && cDigest[0] != 0 && cDip >= 3.0f &&
                             (!cPending || cDrains >= 1),
                         Format("C: shifted %d, pending at first read %d, drains %llu, dip %.1f "
                                "(want >= 3), digests %016llx / %016llx%s%s",
                                cShifted, cPending, (unsigned long long)cDrains, (double)cDip,
                                (unsigned long long)cDigest[0], (unsigned long long)cDigest[1],
                                cWhy.empty() ? "" : ", ", cWhy.c_str()));
  lap("C");

  // ======== S: water ===========================================================
  int sShark = -1, sDry = 0, sTicks = 0;
  uint64_t sRefused = 0, sSwim = 0;
  float sSharkWalked = 0, sSwimSpeedMax = 0, sSwimSpeedCap = 0, sDepth = 0;
  std::string sWhy;
  {
    sShark = c.mobs.FindDef("shark");
    // The home lake (harness map site `home_lake`, an authored lake at
    // (1240, 900)): its deepest column on a scan, then the axis (x or z)
    // with the widest water through it -- the crossing.
    int lx = 0, lz = 0, best = 0;
    for (int z = 900 - 240; z <= 900 + 240; z += 4)
      for (int x = 1240 - 240; x <= 1240 + 240; x += 4) {
        const int wt = ground.WaterTop(x, z);
        if (wt == DistantGround::kNoWater) continue;
        const int dep = wt - (ground.TerrainTop(x, z) + 1);
        if (dep > best) {
          best = dep;
          lx = x;
          lz = z;
        }
      }
    sDepth = (float)best;
    auto wetAt = [&](int x, int z) {
      const int wt = ground.WaterTop(x, z);
      return wt != DistantGround::kNoWater && wt > ground.TerrainTop(x, z) + 1;
    };
    // Water extent from the deepest column along each axis.
    auto extent = [&](int dx, int dz) {
      int k = 0;
      while (k < 600 && wetAt(lx + dx * (k + 1), lz + dz * (k + 1))) k++;
      return k;
    };
    const int ePX = extent(1, 0), eNX = extent(-1, 0), ePZ = extent(0, 1), eNZ = extent(0, -1);
    const bool alongX = ePX + eNX >= ePZ + eNZ;
    const int eHi = alongX ? ePX : ePZ, eLo = alongX ? eNX : eNZ;
    auto at2 = [&](int k) {   // k voxels along the crossing axis from the deepest
      return alongX ? Vec3{(float)(lx + k) + 0.5f, 0.0f, (float)lz + 0.5f}
                    : Vec3{(float)lx + 0.5f, 0.0f, (float)(lz + k) + 0.5f};
    };
    std::printf("distant-walk: S deepest (%d,%d) depth %d; water +x %d -x %d +z %d -z %d; crossing "
                "along %s\n",
                lx, lz, best, ePX, eNX, ePZ, eNZ, alongX ? "x" : "z");
    if (best < 20 || sShark < 0 || eHi + eLo < 16) {
      sWhy = Format("no lake (deepest %d, span %d) or no shark def (%d)", best, eHi + eLo, sShark);
    } else {
      // ---- the shark, wandering round an anchor ON THE SHORE: half its legs
      // point at land, and it must take none of them.
      const Vec3 at = at2(eHi - 8);
      const uint64_t id = fx.Make(sShark, "shark", 0, &at);
      if (DistantMob* d = c.mobs.DistantMut().Find(id)) {
        const int qx = ifloor(d->Centre().x), qz = ifloor(d->Centre().z);
        const int wt = ground.WaterTop(qx, qz);
        int bed = 0;
        ground.ProbeGround(qx, qz, wt, bed);
        // In the water, mid-depth.
        c.mobs.DistantMut().Relocate(
            id, Vec3{d->origin.x, std::max((float)bed, (float)wt - d->worldSize.y - 2.0f), d->origin.z});
        ai::Brain* b = c.mobs.BrainAnyTierMut(id);
        b->wanderAnchorSet = true;
        const Vec3 shore = at2(eHi + 1);
        b->wanderAnchor = Vec3{shore.x, (float)wt, shore.z};
        const DistantMobs::Stats s0 = tier.GetStats();
        for (int k = 0; k < 600; k++, sTicks++) {
          direct();
          const DistantMob* e = tier.Find(id);
          if (e == nullptr) break;
          const int ex = ifloor(e->Centre().x), ez = ifloor(e->Centre().z);
          if (!wetAt(ex, ez)) sDry++;
          // Keep it awake (a resting fish sleeps; this arm is about its legs).
          if (e->asleep) {
            c.mobs.DistantMut().Find(id)->asleep = false;
            c.mobs.DistantMut().MarkAwakeHint();
          }
        }
        if (const DistantMob* e = tier.Find(id)) sSharkWalked = e->walked;
        sRefused = tier.GetStats().waterRefusals - s0.waterRefusals;
      }
      c.mobs.DistantMut().Erase(id);
      // ---- a human in the lake: it swims, at its water speed. Started in
      // the deepest column (on its bed, as a demotion over deep water would
      // leave it) with a goal past the far shore: it must come up and swim.
      // (Not walked in from a bank: this lake's banks stand below its
      // surface on the side the scan finds -- TerrainColumn / WaterColumnAt
      // disagree there, the plan's shoreline risk -- and a bank a body cannot
      // step from is a wall, so it would walk round.)
      const Vec3 hat = at2(0);
      const Vec3 hgoal = at2(eHi + 20);
      const uint64_t hid = fx.Make(fx.human, "duelist", 0, &hat);
      fx.Goto(hid, hgoal);
      const DistantMobs::Stats s1 = tier.GetStats();
      const float swimCap = c.mobs.Defs()[(size_t)fx.human].speed * CurrentTuning().player.liquidSpeedScale;
      sSwimSpeedCap = swimCap;
      Vec3 prev = tier.Find(hid) != nullptr ? tier.Find(hid)->origin : Vec3{};
      uint32_t prevTick = t;
      bool prevSwim = false;
      std::string exitWhy = "ran out of ticks";
      for (int k = 0; k < 1500; k++) {
        direct();
        const DistantMob* e = tier.Find(hid);
        if (e == nullptr) {
          exitWhy = "gone from the tier";
          break;
        }
        if (k % 30 == 0) {
          const Vec3 ec = e->Centre();
          const int wt = ground.WaterTop(ifloor(ec.x), ifloor(ec.z));
          std::printf("distant-walk: S t%4d human at (%.1f, %.1f, %.1f) water %d bed %d swim %d "
                      "intent %s walked %.0f asleep %d\n",
                      k, (double)ec.x, (double)e->origin.y, (double)ec.z,
                      wt == DistantGround::kNoWater ? -99999 : wt,
                      ground.TerrainTop(ifloor(ec.x), ifloor(ec.z)) + 1, e->swimming ? 1 : 0,
                      ai::IntentName(e->brain.intent), (double)e->walked, e->asleep ? 1 : 0);
        }
        if (e->origin.x != prev.x || e->origin.z != prev.z) {
          // A step that began AND ended afloat (one that waded in started at
          // walking pace -- the speed is the step's start state's).
          const bool both = e->swimming && prevSwim;
          prevSwim = e->swimming;
          if (both) {
            const float dx = e->origin.x - prev.x, dz = e->origin.z - prev.z;
            const float v = std::sqrt(dx * dx + dz * dz) / ((float)(t - prevTick) / 30.0f);
            sSwimSpeedMax = std::max(sSwimSpeedMax, v);
          }
          prev = e->origin;
          prevTick = t;
        }
        if (e->brain.routine.arrived) {
          exitWhy = Format("arrived after %d ticks", k);
          break;
        }
      }
      std::printf("distant-walk: S human loop ended: %s\n", exitWhy.c_str());
      sSwim = tier.GetStats().swimSteps - s1.swimSteps;
      c.mobs.DistantMut().Erase(hid);
    }
  }
  const bool okS = check(sWhy.empty() && sDry == 0 && sSharkWalked > 0.0f && sSwim > 0 &&
                             sSwimSpeedMax <= sSwimSpeedCap * 1.05f + 0.01f,
                         Format("S: lake depth %.0f; shark walked %.0f in %d ticks, dry ticks %d, "
                                "water refusals %llu; human swim steps %llu, swim speed max %.1f "
                                "(cap %.1f)%s%s",
                                (double)sDepth, (double)sSharkWalked, sTicks, sDry,
                                (unsigned long long)sRefused, (unsigned long long)sSwim,
                                (double)sSwimSpeedMax, (double)sSwimSpeedCap,
                                sWhy.empty() ? "" : ", ", sWhy.c_str()));
  const bool okDC = check(cDigest[0] == cDigest[1], "D(C): crater digests differ");
  lap("S");

  // ---- the record ----
  RecordObserved("distantWalk.walkedVox", (double)wWalked);
  RecordObserved("distantWalk.progressVox", (double)wProgress);
  RecordObserved("distantWalk.ticks", (double)wTicks);
  RecordObserved("distantWalk.yRange", (double)(wMaxY - wMinY));
  RecordObserved("distantWalk.climbRefusals", (double)wClimb);
  RecordObserved("distantWalk.dropRefusals", (double)wDrop);
  RecordObserved("distantWalk.replans", (double)wReplans);
  RecordObserved("distantWalk.lineDepthMax", (double)pDepthMax);
  RecordObserved("distantWalk.promoteSnapErr", (double)pErr);
  RecordObserved("distantWalk.craterDip", (double)cDip);
  RecordObserved("distantWalk.craterDrains", (double)cDrains);
  RecordObserved("distantWalk.landingHpLost", (double)xLost);
  RecordObserved("distantWalk.sharkWaterRefusals", (double)sRefused);
  RecordObserved("distantWalk.swimSteps", (double)sSwim);
  const bool ok = okW && okDW && okA && okX && okP && okC && okS && okDC;
  detail = Format(
      "W %s: %.0f vox walked (%.0f progress, heading %d deg, y %.0f..%.0f, %d ticks, %llu climb + "
      "%llu drop refusals, %u replans) | D %s | A %s (turned %.2f rad) | X %s | P %s: deepest "
      "%.2f, %llu holds, snap %.2f | C %s: dip %.1f, %llu drains | S %s",
      okW ? "ok" : "FAIL", (double)wWalked, (double)wProgress, wHeadingDeg, (double)wMinY,
      (double)wMaxY, wTicks, (unsigned long long)wClimb, (unsigned long long)wDrop, wReplans,
      okDW && okDC ? "ok" : "FAIL", okA ? "ok" : "FAIL", (double)aTurn, okX ? "ok" : "FAIL",
      okP ? "ok" : "FAIL", (double)pDepthMax, (unsigned long long)pHolds, (double)pErr,
      okC ? "ok" : "FAIL", (double)cDip, (unsigned long long)cDrains, okS ? "ok" : "FAIL");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];
  detail += " | ms: " + laps;

  // ---- LEAVES NOTHING ----
  realTick.Rig().Authority().parking = nullptr;
  c.mobs.SetParkFn(nullptr);
  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.ClearPlayerActors();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  c.stream.OnRegen();
  c.world.SetWindowOrigin(home);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return ok ? Status::Pass : Status::Fail;
}

// ---- distant-sleep --------------------------------------------------------------
Status GateDistantSleep(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) fails.push_back(why);
    return ok;
  };
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetItems(&c.items);
  c.mobs.SetParkFn(nullptr);
  SetTier(true);
  c.mobs.SetDistantStream(&c.stream);
  Fix fx(c);
  if (fx.human < 0) {
    detail = "no 'human' def";
    SetCurrentTuning(tuneWas);
    return Status::Fail;
  }
  const IVec3 home = fx.home;
  const Vec3 wlo = fx.wlo;
  const int n = (int)kNChunk;
  uint32_t t = 9800;
  IVec3 pc{home.x + n / 2, World::TerrainHeight(fx.baseX, fx.baseZ, kDefaultSeed) >> 4,
           home.z + n / 2};
  support::TickCursor realTick{c, t, pc};
  const DistantMobs& tier = c.mobs.Distant();
  const double maxTicks = BaselineNumber("distantSleepMaxTicks", 30);
  const double maxUs = BaselineNumber("distantIdleMaxUs", 25);

  // ---- 1. 200 idle duelists on a grid outside the window ----
  // Pitch: far enough apart that a walker passing between two columns is
  // outside every sleeper's personal space (LocomotionDef::spacingMul), so
  // nobody it wakes is shoved into a new cell -- the wake set is then
  // exactly the cells it reached, which is the claim in 3.
  const float pitch = 40.0f;
  const int cols = 20, rows = 10;
  const float gx0 = wlo.x + (float)kWorldN + 96.0f, gz0 = wlo.z + 32.0f;
  std::vector<uint64_t> ids;
  int made = 0;
  for (int r = 0; r < rows; r++)
    for (int q = 0; q < cols; q++) {
      const Vec3 at{gx0 + pitch * (float)q, 0.0f, gz0 + pitch * (float)r};
      const uint64_t id = fx.Make(fx.human, "duelist", 0, &at);
      if (id != 0) {
        ids.push_back(id);
        made++;
      }
    }
  int asleepBy = -1;
  for (int k = 0; k < (int)maxTicks + 10 && asleepBy < 0; k++) {
    realTick();
    if (tier.AwakeCount() == 0) asleepBy = k + 1;
  }
  const bool ok1 = check(made == cols * rows && asleepBy >= 1 && asleepBy <= (int)maxTicks,
                         Format("1: %d/%d made, all asleep after %d ticks (max %.0f), %zu awake",
                                made, cols * rows, asleepBy, maxTicks, tier.AwakeCount()));

  // ---- 2. asleep costs nothing ----
  const DistantMobs::Stats s0 = tier.GetStats();
  for (int k = 0; k < 120; k++) realTick();
  const DistantMobs::Stats s1 = tier.GetStats();
  const uint64_t calls = s1.tickCallsAll - s0.tickCallsAll;
  const double usPerTick = calls > 0 ? (double)(s1.tickNs - s0.tickNs) / 1000.0 / (double)calls : -1.0;
  const uint64_t idleSteps = s1.steps - s0.steps;
  const bool ok2 = check(calls == 120 && usPerTick >= 0.0 && usPerTick <= maxUs && idleSteps == 0,
                         Format("2: %.3f us/tick over %llu ticks (max %.0f), %llu steps",
                                usPerTick, (unsigned long long)calls, maxUs,
                                (unsigned long long)idleSteps));
  // ...and what one awake step cost, while they were deciding to sleep.
  const DistantMobs::Stats& rt = tier.GetStats();
  const double usPerStep = rt.steps > 0 ? (double)rt.tickNs / 1000.0 / (double)rt.steps : 0.0;

  // ---- 3. a walker wakes exactly its neighbourhood ----
  for (const uint64_t id : ids)
    if (DistantMob* d = c.mobs.DistantMut().Find(id)) d->wakes = 0;
  // Between columns 9 and 10 (x half a pitch off both), walking the grid's
  // z extent and out the far side.
  const float wx = gx0 + pitch * 9.5f;
  const Vec3 wat{wx, 0.0f, gz0 - 60.0f};
  std::set<std::pair<int, int>> visited;
  const uint64_t walker = fx.Make(fx.human, "duelist", 0, &wat);
  fx.Goto(walker, Vec3{wx, 0.0f, gz0 + pitch * (float)rows + 60.0f});
  int wTicks = 0;
  for (; wTicks < 1500; wTicks++) {
    if (const DistantMob* w = tier.Find(walker)) visited.insert({w->cellX, w->cellZ});
    realTick();
    const DistantMob* w = tier.Find(walker);
    if (w == nullptr || w->brain.routine.arrived) break;
  }
  if (const DistantMob* w = tier.Find(walker)) visited.insert({w->cellX, w->cellZ});
  int want = 0, got = 0, wrong = 0;
  std::string why3;
  for (const uint64_t id : ids) {
    const DistantMob* d = tier.Find(id);
    if (d == nullptr) continue;
    bool near = false;
    for (const auto& v : visited)
      near |= std::abs(v.first - d->cellX) <= 1 && std::abs(v.second - d->cellZ) <= 1;
    const bool woke = d->wakes > 0;
    want += near ? 1 : 0;
    got += woke ? 1 : 0;
    if (near != woke) {
      wrong++;
      if (why3.empty())
        why3 = Format("sleeper %llu at cell (%d,%d): %s", (unsigned long long)id, d->cellX, d->cellZ,
                      near ? "in reach, never woke" : "woke out of reach");
    }
  }
  const bool ok3 = check(want > 0 && wrong == 0 && visited.size() >= 4,
                         Format("3: walker crossed %zu cells in %d ticks, %d sleepers in reach, %d "
                                "woke, %d wrong%s%s",
                                visited.size(), wTicks, want, got, wrong,
                                why3.empty() ? "" : ", first: ", why3.c_str()));

  RecordObserved("distantSleep.asleepAfterTicks", (double)asleepBy);
  RecordObserved("distantSleep.idleUsPerTick", usPerTick);
  RecordObserved("distantSleep.awakeUsPerStep", usPerStep);
  RecordObserved("distantSleep.woken", (double)got);
  const bool ok = ok1 && ok2 && ok3;
  detail = Format("1 %s: %d asleep after %d ticks | 2 %s: %.3f us/tick asleep (%.1f us per awake "
                  "step) | 3 %s: %d/%d woken exactly",
                  ok1 ? "ok" : "FAIL", made, asleepBy, ok2 ? "ok" : "FAIL", usPerTick, usPerStep,
                  ok3 ? "ok" : "FAIL", got, want);
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];

  c.mobs.Reset();
  c.debris.Reset();
  c.mobs.DistantGroundMut().InvalidateAll();
  SetCurrentTuning(tuneWas);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DistantGates() {
  static const std::vector<Gate> g = {
      // The distant tier walks (docs/PLAN_distant_npcs.md D1, §4).
      {"distant-walk", "mob", {}, false, GateDistantWalk, /*needsRender=*/false},
      {"distant-sleep", "mob", {}, false, GateDistantSleep, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
