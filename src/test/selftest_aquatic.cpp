// ============================================================================
// THE SWIMMERS (2026-10-10; DESIGN.md "Swimmers"): gate `aquatic`.
//
// One stamped pool -- a stone basin dug where the harness ground is dry and
// low, 16 voxels of water -- and four claims, each on fresh creatures in it:
//
//   A. THE HUNT. A shark and a man standing chest-deep in the middle (a
//      statue: `dummy`). The shark must SWIM (the water carries it every tick
//      after it is in, and it never leaves the water or goes through the bed),
//      ORBIT before it commits (it sweeps at least half a lap round him before
//      its first pass), CHARGE (several passes, each one faster than it
//      cruises: the run reaches at least `aquaticRunFrac` of its cruise), BITE
//      FROM THE RUN (bites thrown, and the man loses a real chunk of himself
//      across more than one limb -- a mouthful), and GO PAST (after each bite
//      it is at least 10 voxels away from him again within 75 ticks: it went
//      through, it did not stop on top of him).
//   B. THE SHOAL. Six piranha and a fresh man. They must dart in and NIP:
//      many bites, the man hurt -- and each bite SMALL: voxels lost per bite
//      at most a small fraction of the shark's (the two creatures differ by
//      their jaws' numbers, not by code).
//   C. THE MEDIUM. A man standing DRY on the bank beside the pool, the shark
//      in it: it never takes him as a target (Perception::minImmersion) and
//      goes on patrolling.
//   D. OUT OF THE WATER. A piranha on a stone ledge one voxel over the water
//      must lie on its flank, flop, and get itself back in (it swims again);
//      one on the dry bank out of reach of any water must suffocate --
//      hp lost to DamageCause::Asphyxia, and dead inside the time its own
//      numbers allow.
//   E. CARRION. A dead man and a fresh shoal: they target the corpse and
//      nip the meat off it (Perception::carrion).
//
// One line per part in the detail names what was measured (rule 6). Thresholds
// in tests/baseline.json (aquatic*). Ticks the world and regenerates it on the
// way out, as corpse-float does.
// ============================================================================
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/mob.h"
#include "gpu/rhi.h"
#include "sim/tuning.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t MatIdOf(Ctx& c, const char* n) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == n) return (uint32_t)i;
  return 0;
}

Vec3 CentreOf(const Mob& m) { return m.Origin() + m.Def()->worldSize * 0.5f; }

// THE SITE: dry ground, chosen off the GPU's own voxels, for a pool of
// half-width `R` dug `kDepth` below the lowest ground (gates `aquatic` and
// `npc-swim`). corpse-float's lesson: the harness map's fixture lake relevels
// a basin stamped inside it, and high ground freezes. The first candidate with
// no liquid near it wins; every rejection is in `siteWhy`.
bool PickDrySite(Ctx& c, int R, int kDepth, IVec3& site, int& hMin, int& hMax,
                 std::string& siteWhy) {
  bool sited = false;
  {
    std::vector<uint32_t> ch(kChunkVol, 0);
    const IVec3 org = c.world.WindowOrigin();
    for (int inset : {250, 200, 300, 150, 350, 100}) {
      const int x0 = org.x * (int)kChunk + inset, z0 = org.z * (int)kChunk + inset;
      int h = INT_MIN, lo = INT_MAX;
      for (int x = x0 - R; x <= x0 + R; x++)
        for (int z = z0 - R; z <= z0 + R; z++) {
          const int th = World::TerrainHeight(x, z, kDefaultSeed);
          h = std::max(h, th);
          lo = std::min(lo, th);
        }
      if (h - lo > 48) {   // the pool is dug below the LOWEST ground anyway
        siteWhy += Format("%d: relief %d; ", inset, h - lo);
        continue;
      }
      uint32_t wet = 0;
      for (int cy = (lo - kDepth - 4) >> 4; cy <= (h + 24) >> 4; cy++)
        for (int cz = (z0 - R - 6) >> 4; cz <= (z0 + R + 6) >> 4; cz++)
          for (int cx = (x0 - R - 6) >> 4; cx <= (x0 + R + 6) >> 4; cx++) {
            if (!c.world.ChunkInWindow({cx, cy, cz})) continue;
            ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({cx, cy, cz}), 1,
                           ch.data(), "aquaticSite");
            for (uint32_t w : ch) {
              const uint32_t mm = w & 0xFFFu;
              if (mm && mm < c.mats.size() && c.mats[mm].gpu.klass == CLASS_LIQUID)
                wet++;
            }
          }
      if (wet > 0) {
        siteWhy += Format("%d: %u liquid cells near; ", inset, wet);
        continue;
      }
      site = IVec3{x0, h, z0};
      hMin = lo;
      hMax = h;
      siteWhy += Format("%d: chosen", inset);
      sited = true;
      break;
    }
  }
  return sited;
}

Status GateAquatic(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  const int dShark = mobs.FindDef("shark"), dPiranha = mobs.FindDef("piranha"),
            dHuman = mobs.FindDef("human");
  if (!mStone || !mWater || dShark < 0 || dPiranha < 0 || dHuman < 0) {
    detail = "missing stone / water / shark / piranha / human";
    return Status::Fail;
  }
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  mobs.Reset();
  c.debris.Reset();

  // ---- THE SITE: dry ground, chosen off the GPU's own voxels (PickDrySite).
  constexpr int R = 34, kDepth = 16;
  IVec3 site{};
  int hMin = 0, hMax = 0;
  std::string siteWhy;
  if (!PickDrySite(c, R, kDepth, site, hMin, hMax, siteWhy)) {
    detail = "no dry, flat site: " + siteWhy;
    return Status::Fail;
  }
  // Dug below the lowest ground; walls to above the highest; air over it.
  const int bedY = hMin - kDepth - 1;      // the floor's top stone layer
  const int waterTop = bedY + kDepth;      // the topmost water layer
  const int wallTop = hMax + 2;
  // THE LEDGE (part D): a stone shelf inside the pool, its top one voxel over
  // the waterline, against the +x wall.
  const int ledgeX0 = site.x + R - 9, ledgeX1 = site.x + R - 2;
  auto inLedge = [&](int x, int z) {
    return x >= ledgeX0 && x <= ledgeX1 && std::abs(z - site.z) <= 4;
  };
  std::vector<CellOp> pool;
  for (int x = site.x - R; x <= site.x + R; x++)
    for (int z = site.z - R; z <= site.z + R; z++)
      for (int y = bedY - 1; y <= wallTop + 14; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = std::abs(x - site.x) >= R - 1 || std::abs(z - site.z) >= R - 1;
        uint32_t w = PackVoxNew(0u, 0u);
        if (y <= bedY || (wall && y <= wallTop)) w = PackVoxNew(mStone, 0u);
        else if (inLedge(x, z) && y <= waterTop + 1) w = PackVoxNew(mStone, 0u);
        else if (y <= waterTop && !wall) w = (mWater & 0xFFFu) | (7u << 12);
        pool.push_back({World::SlotCellIndex(cc), w});
      }
  // ...and a dry pad on the bank for part C's man and part D's dry fish:
  // level stone, air over it, outside the -x wall.
  const int padX = site.x - R - 10, padZ = site.z;
  for (int x = padX - 5; x <= padX + 5; x++)
    for (int z = padZ - 5; z <= padZ + 5; z++)
      for (int y = hMin - 2; y <= hMax + 24; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        pool.push_back({World::SlotCellIndex(cc),
                        PackVoxNew(y <= hMax ? mStone : 0u, 0u)});
      }
  support::TickRig rig(c, 74000u, IVec3{site.x >> 4, waterTop >> 4, site.z >> 4});
  for (size_t off = 0; off < pool.size(); off += 60000) {
    const size_t n = std::min<size_t>(60000, pool.size() - off);
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells.assign(pool.begin() + (ptrdiff_t)off, pool.begin() + (ptrdiff_t)(off + n));
    });
  }
  support::RunTicks(rig, 30);

  const Vec3 poolC{(float)site.x + 0.5f, (float)bedY + 1.0f, (float)site.z + 0.5f};
  auto spawnVictim = [&](Vec3 at) -> uint64_t {
    const Vec3 vs = mobs.Defs()[dHuman].worldSize;
    const uint64_t id = mobs.Spawn(dHuman, {ifloor(at.x - vs.x * 0.5f), ifloor(at.y),
                                            ifloor(at.z - vs.z * 0.5f)});
    if (id) mobs.SetMobBehavior(id, "dummy");
    return id;
  };
  auto spawnAt = [&](int def, Vec3 centre) -> uint64_t {
    const Vec3 ws = mobs.Defs()[def].worldSize;
    return mobs.Spawn(def, {ifloor(centre.x - ws.x * 0.5f), ifloor(centre.y - ws.y * 0.5f),
                            ifloor(centre.z - ws.z * 0.5f)});
  };
  auto voxelsOf = [&](uint64_t id) {
    uint32_t n = 0;
    if (const Mob* m = mobs.FindMobById(id))
      for (int li = 0; li < m->LimbCount(); li++) n += mobs.LimbArtVoxelCount(id, li);
    return n;
  };
  auto limbLoss = [&](uint64_t id, const std::vector<uint32_t>& v0, int& limbs) {
    uint32_t lost = 0;
    limbs = 0;
    if (const Mob* m = mobs.FindMobById(id))
      for (int li = 0; li < m->LimbCount() && li < (int)v0.size(); li++) {
        const uint32_t now = mobs.LimbArtVoxelCount(id, li);
        if (now < v0[(size_t)li]) {
          lost += v0[(size_t)li] - now;
          limbs++;
        }
      }
    return lost;
  };
  auto limbCounts = [&](uint64_t id) {
    std::vector<uint32_t> v;
    if (const Mob* m = mobs.FindMobById(id))
      for (int li = 0; li < m->LimbCount(); li++) v.push_back(mobs.LimbArtVoxelCount(id, li));
    return v;
  };

  // ======== A. THE HUNT ======================================================
  const int kHunt = (int)BaselineNumber("aquaticHuntTicks", 1500);
  const float runFracMin = (float)BaselineNumber("aquaticRunFrac", 1.7);
  const float lossMin = (float)BaselineNumber("aquaticSharkVoxelsMin", 60);
  const float hpMin = (float)BaselineNumber("aquaticSharkHpMin", 150);
  uint64_t victim = spawnVictim(poolC);
  const uint64_t shark = spawnAt(dShark, poolC + Vec3{22.0f, 7.0f, 0.0f});
  const std::vector<uint32_t> v0A = limbCounts(victim);
  const float hp0A = mobs.TotalHp(victim);
  int swimTicks = 0, considered = 0, outOfWater = 0;
  float peakV = 0.0f, sweptAtFirstPass = -1.0f;
  uint32_t passes = 0, bites = 0;
  std::vector<int> biteTicks;
  int wentPast = 0;
  float worstLow = 1e9f, worstHigh = -1e9f;
  {
    uint32_t bitesSeen = 0;
    for (int i = 0; i < kHunt; i++) {
      support::RunTicks(rig, 1);
      const Mob* m = mobs.FindMobById(shark);
      const ai::Brain* b = mobs.MobBrain(shark);
      if (m == nullptr || b == nullptr) break;
      const Vec3 cS = CentreOf(*m);
      if (i >= 30) {
        considered++;
        if (m->Swimming()) swimTicks++;
        // Never out of the water's slab: the bed and the surface (the back may
        // break it, by `breach` on a run).
        worstLow = std::min(worstLow, cS.y - (float)(bedY + 1));
        worstHigh = std::max(worstHigh, cS.y - (float)(waterTop + 1));
        if (cS.y < (float)bedY + 1.0f || cS.y > (float)waterTop + 2.0f) outOfWater++;
      }
      if (b->passes > passes) {
        if (passes == 0) sweptAtFirstPass = b->orbitSwept;
        passes = b->passes;
      }
      if (b->passPhase >= 2) peakV = std::max(peakV, m->SwimVelocity().len());
      if (b->passBites > bitesSeen) {
        bitesSeen = b->passBites;
        biteTicks.push_back(i);
      }
      // GONE PAST: the first tick after each bite at which it is 10+ away.
      for (size_t k = 0; k < biteTicks.size(); k++)
        if (biteTicks[k] >= 0 && i - biteTicks[k] <= 75) {
          if (const Mob* vm = mobs.FindMobById(victim))
            if ((cS - CentreOf(*vm)).len() >= 10.0f) {
              wentPast++;
              biteTicks[k] = -1000000;
            }
        }
    }
    if (const ai::Brain* b = mobs.MobBrain(shark)) bites = b->passBites;
  }
  int limbsA = 0;
  const uint32_t lostA = limbLoss(victim, v0A, limbsA);
  const float hpLostA = hp0A - mobs.TotalHp(victim);
  const float cruise = mobs.Defs()[dShark].speed;
  const float swimShare = considered > 0 ? (float)swimTicks / (float)considered : 0.0f;
  const bool aSwim = swimShare >= 0.98f && outOfWater == 0;
  const bool aOrbit = sweptAtFirstPass >= 3.14159f;
  // Three passes -- or fewer if the man did not survive them.
  const bool victimDead = !mobs.IsAlive(victim);
  std::string victimEnd = victimDead ? "dead" : "alive";
  if (victimDead)
    if (const char* why = mobs.DeathCause(victim); why != nullptr && why[0] != 0)
      victimEnd = std::string("dead: ") + why;
  const bool aCharge = (passes >= 3 || victimDead) && peakV >= runFracMin * cruise;
  const bool aBite = (bites >= 2 || victimDead) && bites >= 1 &&
                     (float)lostA >= lossMin && limbsA >= 2 && hpLostA >= hpMin;
  const bool aPast = bites > 0 && wentPast >= (int)bites;
  const bool aOk = aSwim && aOrbit && aCharge && aBite && aPast;
  const float perBiteShark = bites > 0 ? (float)lostA / (float)bites : 0.0f;
  RecordObserved("aquaticSharkVoxels", (double)lostA);
  RecordObserved("aquaticSharkPasses", (double)passes);

  // ======== B. THE SHOAL =====================================================
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  const int kShoal = (int)BaselineNumber("aquaticShoalTicks", 900);
  victim = spawnVictim(poolC);
  std::vector<uint64_t> shoal;
  for (int k = 0; k < 6; k++) {
    const float a = 0.6f + (float)k * 1.047f;
    shoal.push_back(spawnAt(dPiranha, poolC + Vec3{std::cos(a) * 11.0f, 5.0f + (float)(k % 3),
                                                   std::sin(a) * 11.0f}));
  }
  const std::vector<uint32_t> v0B = limbCounts(victim);
  const float hp0B = mobs.TotalHp(victim);
  support::RunTicks(rig, kShoal);
  uint32_t nips = 0;
  int swimmingB = 0;
  for (uint64_t id : shoal) {
    if (const ai::Brain* b = mobs.MobBrain(id)) nips += b->passBites;
    if (const Mob* m = mobs.FindMobById(id); m && m->Swimming()) swimmingB++;
  }
  int limbsB = 0;
  const uint32_t lostB = limbLoss(victim, v0B, limbsB);
  const float hpLostB = hp0B - mobs.TotalHp(victim);
  const float perBitePiranha = nips > 0 ? (float)lostB / (float)nips : 0.0f;
  const float ratioMax = (float)BaselineNumber("aquaticNipRatioMax", 0.15);
  const bool bOk = nips >= (uint32_t)BaselineNumber("aquaticNipsMin", 15) &&
                   hpLostB >= (float)BaselineNumber("aquaticShoalHpMin", 30) &&
                   swimmingB == 6 && perBiteShark > 0.0f &&
                   perBitePiranha <= ratioMax * perBiteShark;
  RecordObserved("aquaticNips", (double)nips);

  // ======== C. THE MEDIUM ====================================================
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  const Vec3 padTop{(float)padX + 0.5f, (float)hMax + 1.0f, (float)padZ + 0.5f};
  const uint64_t dry = spawnVictim(padTop);
  const uint64_t shark2 = spawnAt(dShark, poolC + Vec3{-10.0f, 7.0f, 0.0f});
  int targeted = 0;
  // PATROLLED = the path swum, not start-to-end: a figure-eight wander leg
  // (movement.wander.figure8) is a closed loop that ends where it began.
  Vec3 cPrev{};
  float patrol = 0.0f;
  if (const Mob* m = mobs.FindMobById(shark2)) cPrev = CentreOf(*m);
  const int kMedium = (int)BaselineNumber("aquaticMediumTicks", 360);
  for (int i = 0; i < kMedium; i++) {
    support::RunTicks(rig, 1);
    if (const ai::Brain* b = mobs.MobBrain(shark2); b && b->hasTarget) targeted++;
    if (const Mob* m = mobs.FindMobById(shark2)) {
      const Vec3 cNow = CentreOf(*m);
      patrol += (cNow - cPrev).len();
      cPrev = cNow;
    }
  }
  const bool cOk = dry != 0 && targeted == 0 && patrol >= 6.0f;

  // ======== D. OUT OF THE WATER ==============================================
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  // On the ledge (one over the waterline, water on three sides of it) and on
  // the bank pad (no water within its reach).
  const Vec3 ledgeTop{(float)(ledgeX0 + ledgeX1) * 0.5f, (float)waterTop + 3.0f,
                      (float)site.z + 0.5f};
  const uint64_t onLedge = spawnAt(dPiranha, ledgeTop);
  const uint64_t onBank = spawnAt(dPiranha, padTop + Vec3{0.0f, 2.0f, 0.0f});
  const int kStrand = (int)BaselineNumber("aquaticStrandTicks", 900);
  float maxRollLedge = 0.0f, maxRollBank = 0.0f;
  int backInAt = -1;
  uint32_t flopsLedge = 0, flopsBank = 0;
  int deadAt = -1;
  for (int i = 0; i < kStrand; i++) {
    support::RunTicks(rig, 1);
    if (const Mob* m = mobs.FindMobById(onLedge)) {
      if (!m->Swimming()) maxRollLedge = std::max(maxRollLedge, std::fabs(m->SwimRoll()));
      flopsLedge = m->Flops();
      if (backInAt < 0 && i > 5 && m->Swimming()) backInAt = i;
    }
    if (const Mob* m = mobs.FindMobById(onBank)) {
      maxRollBank = std::max(maxRollBank, std::fabs(m->SwimRoll()));
      flopsBank = std::max(flopsBank, m->Flops());
      if (deadAt < 0 && !m->Alive()) deadAt = i;
    }
  }
  float asphyxia = 0.0f;
  const char* cause = "";
  if (const Mob* m = mobs.FindMobById(onBank)) {
    asphyxia = m->HpLostBy(DamageCause::Asphyxia);
    if (m->DeathCause() != nullptr) cause = m->DeathCause();
  }
  const bool dOk = backInAt >= 0 && flopsLedge >= 1 && maxRollLedge >= 1.0f &&
                   maxRollBank >= 1.0f && flopsBank >= 2 && deadAt >= 0 &&
                   asphyxia > 0.0f;

  // ======== E. CARRION =======================================================
  // A dead man in the pool and a fresh shoal (ai::Perception::carrion). They
  // must take him as a target although he is dead, and NIP the meat off him:
  // the corpse loses art voxels and its meat-to-bone ratio falls. The release
  // at the skeleton is the same Perceive test with the ratio under the
  // profile's `carrion`, and is not run to the end here (a whole man is a
  // long meal).
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  const uint64_t corpse = spawnVictim(poolC);
  if (Mob* m = mobs.FindMobById(corpse)) m->Die();
  support::RunTicks(rig, 20);
  // THE CONTROL ARM: the same corpse alone for 300 ticks -- what the water
  // and the float do to him with no fish (its path, beside the shoal's).
  float alonePath = 0.0f;
  {
    Vec3 prev{};
    bool had = false;
    for (int i = 0; i < 300; i++) {
      support::RunTicks(rig, 1);
      Vec3 p{};
      if (const Mob* m = mobs.FindMobById(corpse); m && m->LimbCentreWorld(m->Def()->rootLimb, p)) {
        if (had) alonePath += (p - prev).len();
        prev = p;
        had = true;
      }
    }
  }
  shoal.clear();
  for (int k = 0; k < 6; k++) {
    const float a = 0.3f + (float)k * 1.047f;
    shoal.push_back(spawnAt(dPiranha, poolC + Vec3{std::cos(a) * 11.0f, 5.0f + (float)(k % 3),
                                                   std::sin(a) * 11.0f}));
  }
  const std::vector<uint32_t> v0E = limbCounts(corpse);
  const int kCarrion = (int)BaselineNumber("aquaticCarrionTicks", 1200);
  float ratio0 = -1.0f, ratioEnd = -1.0f;
  uint32_t onCorpse = 0;
  // STRIPPED: the first tick the meat ratio is at or under what the profile
  // eats down to; RELEASED: the first tick after that no fish holds him.
  const ai::Profile* prP = mobs.Behaviors().At(mobs.Behaviors().Find("piranha"));
  const float eatsTo = prP != nullptr ? prP->perception.carrion : 0.0f;
  int strippedAt = -1, releasedAt = -1;
  uint32_t meat0 = 0, meatEnd = 0;
  // WHERE THE NIPS WENT (rule 6): fish-ticks in a cut at the corpse, bodies
  // those cuts touched, how near the fish was, where the corpse floats.
  uint32_t cutTicksE = 0, bodiesE = 0;
  float nearSum = 0.0f, corpseY = 0.0f;
  // HOW FAR THE FEEDING SHOVES HIM: the pelvis's path and its fastest tick.
  float corpsePath = 0.0f, corpseTop = 0.0f;
  Vec3 corpsePrev{};
  bool corpseHad = false;
  std::vector<int> lastHit(shoal.size(), 0);
  for (int i = 0; i < kCarrion; i++) {
    support::RunTicks(rig, 1);
    Vec3 cc{};
    if (const Mob* m = mobs.FindMobById(corpse))
      if (m->LimbCentreWorld(m->Def()->rootLimb, cc)) {
        corpseY = cc.y;
        if (corpseHad) {
          const float step = (cc - corpsePrev).len();
          corpsePath += step;
          corpseTop = std::max(corpseTop, step);
        }
        corpsePrev = cc;
        corpseHad = true;
      }
    for (size_t k = 0; k < shoal.size(); k++) {
      const NpcStroke* st = mobs.MobStroke(shoal[k]);
      const Mob* f = mobs.FindMobById(shoal[k]);
      if (st == nullptr || f == nullptr) continue;
      if (st->phase == NpcStroke::Phase::Cut && st->targetId == corpse) {
        cutTicksE++;
        nearSum += (CentreOf(*f) - cc).len();
        if (st->bodiesHit > lastHit[k]) bodiesE += (uint32_t)(st->bodiesHit - lastHit[k]);
      }
      lastHit[k] = st->bodiesHit;
    }
    int holding = 0;
    for (uint64_t id : shoal)
      if (const ai::Brain* b = mobs.MobBrain(id); b && b->hasTarget && b->targetId == corpse)
        holding++;
    onCorpse += (uint32_t)holding;
    if (const Mob* m = mobs.FindMobById(corpse)) {
      if (ratio0 < 0.0f) {
        ratio0 = m->FleshRatio();
        meat0 = m->CarrionCells();
      }
      ratioEnd = m->FleshRatio();
      meatEnd = m->CarrionCells();
      if (strippedAt < 0 && ratioEnd >= 0.0f && ratioEnd <= eatsTo) strippedAt = i;
    }
    if (strippedAt >= 0 && releasedAt < 0 && holding == 0) {
      releasedAt = i;
      break;
    }
  }
  uint32_t nipsE = 0;
  for (uint64_t id : shoal)
    if (const ai::Brain* b = mobs.MobBrain(id)) nipsE += b->passBites;
  int limbsE = 0;
  const uint32_t lostE = limbLoss(corpse, v0E, limbsE);
  const bool eOk = corpse != 0 && !mobs.IsAlive(corpse) &&
                   nipsE >= (uint32_t)BaselineNumber("aquaticCarrionNipsMin", 20) &&
                   lostE >= (uint32_t)BaselineNumber("aquaticCarrionVoxelsMin", 60) &&
                   ratio0 > 0.0f && ratioEnd < ratio0;
  RecordObserved("aquaticCarrionVoxels", (double)lostE);

  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "site %s | A hunt %s: swam %.0f%% (out of the slab %d ticks, low %+.1f high %+.1f), "
      "swept %.2f rad before pass 1, %u passes (peak %.1f = %.2fx cruise), %u bites, "
      "%d went past, man lost %u voxels over %d limbs and %.0f hp (%s) | B shoal %s: %u "
      "nips, %d/6 swimming, man lost %u voxels over %d limbs and %.0f hp, %.2f voxels a "
      "nip against the shark's %.1f a bite | C medium %s: shark targeted the dry man "
      "%d/%d ticks, patrolled %.1f | D stranded %s: ledge fish rolled %.2f, %u flops, back "
      "in the water at tick %d; bank fish rolled %.2f, %u flops, dead at tick %d of %s "
      "(%.1f hp to asphyxia) | E carrion %s: %u nips on a corpse, %u fish-ticks on it, "
      "lost %u voxels over %d limbs, meat/bone %.2f -> %.3f (eats to %.3f: stripped at "
      "tick %d, let go at %d) [%u cut fish-ticks at it, %u bodies touched, %.1f from its "
      "pelvis on average; pelvis y %+.1f over the water's top; meat cells %u -> %u, bone and the rest %.0f -> %.0f; pelvis swam %.0f voxels, fastest %.2f a tick; alone, %.1f voxels in 300 ticks]",
      siteWhy.c_str(), aOk ? "PASS" : "FAIL", swimShare * 100.0f, outOfWater, worstLow,
      worstHigh, sweptAtFirstPass, passes, peakV, cruise > 0 ? peakV / cruise : 0.0f,
      bites, wentPast, lostA, limbsA, hpLostA, victimEnd.c_str(), bOk ? "PASS" : "FAIL",
      nips, swimmingB,
      lostB, limbsB, hpLostB, perBitePiranha, perBiteShark, cOk ? "PASS" : "FAIL",
      targeted, kMedium, patrol, dOk ? "PASS" : "FAIL", maxRollLedge, flopsLedge,
      backInAt, maxRollBank, flopsBank, deadAt, cause, asphyxia, eOk ? "PASS" : "FAIL",
      nipsE, onCorpse, lostE, limbsE, ratio0, ratioEnd, eatsTo, strippedAt, releasedAt,
      cutTicksE, bodiesE, cutTicksE ? nearSum / (float)cutTicksE : 0.0f,
      corpseY - (float)(waterTop + 1), meat0, meatEnd,
      ratio0 > 0.0f ? (float)meat0 / ratio0 : 0.0f,
      ratioEnd > 0.0f ? (float)meatEnd / ratioEnd : 0.0f, corpsePath, corpseTop, alonePath);
  return aOk && bOk && cOk && dOk && eOk ? Status::Pass : Status::Fail;
}


// ============================================================================
// A WALKER SWIMS (2026-10-11; game/mob_afloat.cpp): gate `npc-swim`.
//
// One stamped pool too deep to stand in (kDepth over a human's 0.92 standing
// height share) with a LOW BANK on its +x side -- stone one voxel over the
// waterline, a dry pad beyond it -- and three claims, each on a fresh human:
//
//   A. IT FLOATS. A human with a statue's profile (`dummy`) put on the bed must
//      come up and tread: the water carries it (Mob::swimming_), its soles
//      settle within npcSwimFloatTol of the float height (kAfloatShare under
//      the waterline), and the pose is the swim family with the tread leading.
//   B. IT SWIMS OUT. A zombie in the middle with the player's point on the pad
//      must swim to the bank (the crawl leading the mix on the way) and climb
//      out onto it: dry, standing at the bank's height, inside npcSwimOutTicks.
//   C. IT DIVES. A zombie with the player's point on the bed must go under
//      for it -- its whole body below the waterline -- with the stroke
//      pitched along the dive (PoseDrive::swimPitch past npcSwimPitchMin).
//
// One line per part in the detail names what was measured (rule 6).
// Thresholds in tests/baseline.json (npcSwim*). Ticks the world and
// regenerates it on the way out, as `aquatic` does.
// ============================================================================
Status GateNpcSwim(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  const int dHuman = mobs.FindDef("human");
  if (!mStone || !mWater || dHuman < 0) {
    detail = "missing stone / water / human";
    return Status::Fail;
  }
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  mobs.Reset();
  c.debris.Reset();

  constexpr int R = 22, kDepth = 28, kBankHalf = 7, kPadLen = 16;
  IVec3 site{};
  int hMin = 0, hMax = 0;
  std::string siteWhy;
  if (!PickDrySite(c, R, kDepth, site, hMin, hMax, siteWhy)) {
    detail = "no dry, flat site: " + siteWhy;
    return Status::Fail;
  }
  const int bedY = hMin - kDepth - 1;      // the floor's top stone layer
  const int waterTop = bedY + kDepth;      // the topmost water layer
  const int surfY = waterTop + 1;          // the first air cell: the waterline
  const int wallTop = hMax + 2;
  // THE BANK: the +x wall's middle stretch topped one voxel over the water,
  // so a body standing on it has its soles at surfY + 1.
  auto inBank = [&](int x, int z) {
    return x >= site.x + R - 1 && std::abs(z - site.z) <= kBankHalf;
  };
  std::vector<CellOp> pool;
  for (int x = site.x - R; x <= site.x + R + kPadLen; x++)
    for (int z = site.z - R; z <= site.z + R; z++) {
      const bool inPool = x <= site.x + R;
      if (!inPool && std::abs(z - site.z) > kBankHalf) continue;
      for (int y = bedY - 1; y <= wallTop + 30; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = inPool && (std::abs(x - site.x) >= R - 1 ||
                                     std::abs(z - site.z) >= R - 1);
        uint32_t w = PackVoxNew(0u, 0u);
        if (inBank(x, z)) {
          if (y <= surfY) w = PackVoxNew(mStone, 0u);
        } else if (y <= bedY || (wall && y <= wallTop)) {
          w = PackVoxNew(mStone, 0u);
        } else if (y <= waterTop && inPool && !wall) {
          w = (mWater & 0xFFFu) | (7u << 12);
        }
        pool.push_back({World::SlotCellIndex(cc), w});
      }
    }
  support::TickRig rig(c, 75000u, IVec3{site.x >> 4, waterTop >> 4, site.z >> 4});
  for (size_t off = 0; off < pool.size(); off += 60000) {
    const size_t n = std::min<size_t>(60000, pool.size() - off);
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells.assign(pool.begin() + (ptrdiff_t)off, pool.begin() + (ptrdiff_t)(off + n));
    });
  }
  support::RunTicks(rig, 30);

  const Vec3 hs = mobs.Defs()[dHuman].worldSize;
  const float H = std::max(hs.y, 1.0f);
  const float floatY = (float)surfY - 0.72f * H;   // mob_afloat.cpp kAfloatShare
  auto spawnOnBed = [&](float x, float z, const char* profile) -> uint64_t {
    const uint64_t id = mobs.Spawn(dHuman, {ifloor(x - hs.x * 0.5f), bedY + 1,
                                            ifloor(z - hs.z * 0.5f)});
    if (id) mobs.SetMobBehavior(id, profile);
    return id;
  };
  // The player's point, far out of the way, for the part that wants none.
  auto parkPlayer = [&]() {
    mobs.SetPlayerActor(Vec3{(float)site.x - 400.0f, (float)hMax + 400.0f,
                             (float)site.z},
                        3.0f, 17.0f, false);
  };

  // ======== A. IT FLOATS =====================================================
  const float floatTol = (float)BaselineNumber("npcSwimFloatTol", 2.5);
  parkPlayer();
  const uint64_t a = spawnOnBed((float)site.x - 6.0f, (float)site.z, "dummy");
  support::RunTicks(rig, (int)BaselineNumber("npcSwimFloatTicks", 120));
  bool aSwim = false, aPose = false;
  float aY = 0.0f, aTread = 0.0f;
  if (const Mob* m = mobs.FindMobById(a)) {
    aSwim = m->Swimming();
    aY = m->Origin().y;
    aPose = m->Pose().swimOn;
    aTread = m->Pose().swimW[0];
  }
  const bool aOk = a != 0 && aSwim && std::fabs(aY - floatY) <= floatTol && aPose &&
                   aTread >= 0.5f;

  // ======== B. IT SWIMS OUT ==================================================
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  const int kOut = (int)BaselineNumber("npcSwimOutTicks", 900);
  const Vec3 padPoint{(float)(site.x + R + kPadLen / 2), (float)(surfY + 1) + 8.0f,
                      (float)site.z + 0.5f};
  mobs.SetPlayerActor(padPoint, 3.0f, 17.0f, true);
  const uint64_t b = spawnOnBed((float)site.x + 4.0f, (float)site.z, "zombie");
  int swamB = 0, outAt = -1, targetedB = 0;
  float crawlPeak = 0.0f, bestX = -1e9f;
  for (int i = 0; i < kOut && outAt < 0; i++) {
    support::RunTicks(rig, 1);
    const Mob* m = mobs.FindMobById(b);
    if (m == nullptr) break;
    bestX = std::max(bestX, m->Origin().x + hs.x * 0.5f);
    if (const ai::Brain* br = mobs.MobBrain(b); br != nullptr && br->hasTarget)
      targetedB++;
    if (m->Swimming()) {
      swamB++;
      crawlPeak = std::max(crawlPeak, m->Pose().swimW[1]);
    } else if (swamB > 0 && m->Origin().x + hs.x * 0.5f >= (float)(site.x + R - 1) &&
               m->Origin().y >= (float)(surfY + 1) - 0.5f) {
      outAt = i;
    }
  }
  const bool bOk = b != 0 && swamB > 0 && crawlPeak >= 0.5f && outAt >= 0;

  // ======== C. IT DIVES ======================================================
  mobs.Reset();
  c.debris.Reset();
  support::RunTicks(rig, 2);
  const int kDive = (int)BaselineNumber("npcSwimDiveTicks", 300);
  const float pitchMin = (float)BaselineNumber("npcSwimPitchMin", 0.3);
  const Vec3 bedPoint{(float)site.x + 8.0f, (float)(bedY + 1) + 8.0f, (float)site.z + 0.5f};
  mobs.SetPlayerActor(bedPoint, 3.0f, 17.0f, true);
  const uint64_t d = spawnOnBed((float)site.x - 8.0f, (float)site.z, "zombie");
  int swamC = 0, underC = 0;
  float headLow = 1e9f, pitchPeak = 0.0f;
  for (int i = 0; i < kDive; i++) {
    support::RunTicks(rig, 1);
    const Mob* m = mobs.FindMobById(d);
    if (m == nullptr) break;
    if (!m->Swimming()) continue;
    swamC++;
    const float top = m->Origin().y + H;
    headLow = std::min(headLow, top - (float)surfY);
    if (top < (float)surfY) underC++;
    pitchPeak = std::max(pitchPeak, std::fabs(m->Pose().swimPitch));
  }
  const bool cOk = d != 0 && swamC > 0 && underC > 0 && pitchPeak >= pitchMin;
  RecordObserved("npcSwimOutTick", (double)outAt);

  mobs.Reset();
  c.debris.Reset();
  parkPlayer();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = Format(
      "site %s, H %.1f, bed %d, waterline %d | A floats %s: swimming %d, soles %.2f "
      "vs float %.2f (tol %.1f), swim pose %d, tread %.2f | B swims out %s: %d ticks "
      "swimming (%d with a target), crawl peak %.2f, out on the bank at tick %d of %d "
      "(got to x %+.1f of the bank) | C dives %s: %d ticks swimming, %d with the head under (lowest %+.1f "
      "vs the waterline), stroke pitch peak %.2f rad (min %.2f)",
      siteWhy.c_str(), H, bedY, surfY, aOk ? "PASS" : "FAIL", (int)aSwim, aY, floatY,
      floatTol, (int)aPose, aTread, bOk ? "PASS" : "FAIL", swamB, targetedB, crawlPeak, outAt, kOut,
      bestX - (float)(site.x + R - 1), cOk ? "PASS" : "FAIL", swamC, underC, headLow,
      pitchPeak, pitchMin);
  return aOk && bOk && cOk ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// A STUMP UNDER WATER NEVER STOPS (2026-10-11, the owner's shark bite): gate
// `stump-underwater`.
//
// "It bled at the right rate for three seconds and then stopped completely."
// A man in a pool with a leg severed, and the same man on the dry bank beside
// it: the blood the creature emits (its drip ops, the mob phase's own span of
// the batch) is counted per 60-tick window. Claims:
//   * NEVER STOPS: under water, every window after the first has drips while
//     the man is alive (gore.stumpBleedsOpen).
//   * 2x: the wet total over the run is gore.underwaterBleedRate times the
//     dry total, within stumpWetRatioTol.
// Each window's count is in the detail (rule 6); SANDVOX_BLEED_TRACE=1 names
// the decision behind each wound every 15 ticks; SANDVOX_STUMP_PROBE=1 prints,
// per window, where the drips were painted and where the dissolved blood sits
// (cells, mass, rms radius about the wound).
// ---------------------------------------------------------------------------
Status GateStumpUnderwater(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  MobSystem& mobs = c.mobs;
  const uint32_t mStone = MatIdOf(c, "stone"), mWater = MatIdOf(c, "water");
  const int dHuman = mobs.FindDef("human");
  if (!mStone || !mWater || dHuman < 0) {
    detail = "missing stone / water / human";
    return Status::Fail;
  }
  const uint32_t bleedMat = mobs.Defs()[dHuman].bleedMat;
  int legIdx = -1;
  for (size_t li = 0; li < mobs.Defs()[dHuman].limbs.size(); li++)
    if (mobs.Defs()[dHuman].limbs[li].name == "legU.L") legIdx = (int)li;
  if (legIdx < 0 || bleedMat == 0) {
    detail = "human has no legU.L or does not bleed";
    return Status::Fail;
  }
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  mobs.Reset();
  c.debris.Reset();

  constexpr int R = 12, kDepth = 28, kPadLen = 14, kPadHalf = 6;
  IVec3 site{};
  int hMin = 0, hMax = 0;
  std::string siteWhy;
  if (!PickDrySite(c, R + kPadLen, kDepth, site, hMin, hMax, siteWhy)) {
    detail = "no dry, flat site: " + siteWhy;
    return Status::Fail;
  }
  const int bedY = hMin - kDepth - 1;
  const int waterTop = bedY + kDepth;
  const int surfY = waterTop + 1;
  const int wallTop = hMax + 2;
  // The pool, and a dry stone pad off its +x wall topped at the waterline.
  std::vector<CellOp> pool;
  for (int x = site.x - R; x <= site.x + R + kPadLen; x++)
    for (int z = site.z - R; z <= site.z + R; z++) {
      const bool inPool = x <= site.x + R;
      if (!inPool && std::abs(z - site.z) > kPadHalf) continue;
      for (int y = bedY - 1; y <= wallTop + 30; y++) {
        const IVec3 cc{x, y, z};
        if (!c.world.CellInWindow(cc)) continue;
        const bool wall = inPool && (std::abs(x - site.x) >= R - 1 ||
                                     std::abs(z - site.z) >= R - 1);
        uint32_t w = PackVoxNew(0u, 0u);
        if (!inPool) {
          if (y <= surfY) w = PackVoxNew(mStone, 0u);
        } else if (y <= bedY || (wall && y <= wallTop)) {
          w = PackVoxNew(mStone, 0u);
        } else if (y <= waterTop && !wall) {
          w = (mWater & 0xFFFu) | (7u << 12);
        }
        pool.push_back({World::SlotCellIndex(cc), w});
      }
    }
  support::TickRig rig(c, 76000u, IVec3{site.x >> 4, waterTop >> 4, site.z >> 4});
  for (size_t off = 0; off < pool.size(); off += 60000) {
    const size_t n = std::min<size_t>(60000, pool.size() - off);
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
      o.cells.assign(pool.begin() + (ptrdiff_t)off, pool.begin() + (ptrdiff_t)(off + n));
    });
  }
  support::RunTicks(rig, 30);
  mobs.SetPlayerActor(Vec3{(float)site.x - 400.0f, (float)hMax + 400.0f, (float)site.z},
                      3.0f, 17.0f, false);

  const Vec3 hs = mobs.Defs()[dHuman].worldSize;
  const int kTicks = (int)BaselineNumber("stumpWetTicks", 600);
  constexpr int kWin = 60;
  // The solute ledger's monotonic "units dissolved" word: blood that REACHED
  // the water (a drip that painted onto a standing blood voxel took nothing).
  auto dissolved = [&]() {
    c.ctx.WaitIdle();
    uint32_t v = 0;
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solMeta,
                          (uint64_t)kSolMDissolved * 4, &v, 4, "stumpDissolved");
    return v;
  };
  struct Arm {
    std::vector<uint32_t> win, mixed;
    uint32_t total = 0;
    int deathTick = -1;
    bool spawned = false;
  };
  auto run = [&](Vec3 feet) {
    Arm a;
    mobs.Reset();
    c.debris.Reset();
    support::RunTicks(rig, 2);
    const uint64_t id = mobs.Spawn(dHuman, {ifloor(feet.x - hs.x * 0.5f), ifloor(feet.y),
                                            ifloor(feet.z - hs.z * 0.5f)});
    if (!id) return a;
    a.spawned = true;
    mobs.SetMobBehavior(id, "dummy");
    support::RunTicks(rig, 30);   // settle (a floater rises to its float height)
    mobs.Sever(id, legIdx);
    a.win.assign((size_t)(kTicks / kWin), 0u);
    a.mixed.assign((size_t)(kTicks / kWin), 0u);
    uint32_t d0 = dissolved();
    // SANDVOX_STUMP_PROBE=1 (rule 6): per window, WHERE the drips were painted
    // (centroid + bounding box) against the creature's root, and where the
    // dissolved blood sits (mass centroid, rms radius about the drip centroid,
    // cells carrying any). "The cloud appears round the body, not at the
    // wound" is then either the drip position or something after it.
    static const bool kProbe = std::getenv("SANDVOX_STUMP_PROBE") != nullptr;
    double px = 0, py = 0, pz = 0;
    int pn = 0, bx0 = INT_MAX, by0 = INT_MAX, bz0 = INT_MAX, bx1 = INT_MIN, by1 = INT_MIN,
        bz1 = INT_MIN;
    for (int i = 0; i < kTicks; i++) {
      support::RunTicks(rig, 1);
      const OpBatch& tb = rig.LastBatch();
      const auto& mp = rig.MobPhase();
      for (size_t k = mp.ops0; k < mp.ops1 && k < tb.ops.size(); k++)
        if (tb.ops[k].material == bleedMat) {
          a.total++;
          if ((size_t)(i / kWin) < a.win.size()) a.win[(size_t)(i / kWin)]++;
          if (kProbe) {
            const BrushOp& o = tb.ops[k];
            px += o.x; py += o.y; pz += o.z; pn++;
            bx0 = std::min(bx0, o.x); by0 = std::min(by0, o.y); bz0 = std::min(bz0, o.z);
            bx1 = std::max(bx1, o.x); by1 = std::max(by1, o.y); bz1 = std::max(bz1, o.z);
          }
        }
      if (kProbe && (i + 1) % kWin == 0 && mobs.IsAlive(id)) {
        c.ctx.WaitIdle();
        std::vector<uint32_t> tab(kNumSlots), pool((size_t)kSolutePoolPages * kSolWordsPerPage);
        rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solTable, 0, tab.data(),
                              tab.size() * 4, "probeSolTable");
        rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.solPool, 0, pool.data(),
                              pool.size() * 4, "probeSolPool");
        const Vec3 dc = pn ? Vec3{(float)(px / pn), (float)(py / pn), (float)(pz / pn)}
                           : Vec3{0, 0, 0};
        double mx = 0, my = 0, mz = 0, mm = 0, r2 = 0;
        int cells = 0;
        for (int x = site.x - R; x <= site.x + R; x++)
          for (int z = site.z - R; z <= site.z + R; z++)
            for (int y = bedY; y <= waterTop; y++) {
              const IVec3 cc{x, y, z};
              if (!c.world.CellInWindow(cc)) continue;
              const uint32_t ci = World::SlotCellIndex(cc);
              const uint32_t slot = ci / kChunkVol, local = ci % kChunkVol;
              const uint32_t e = tab[slot];
              uint32_t v = 0;
              if (e & kSolUniformBit) v = e & 0xFFFFu;
              else if (e != 0u)
                v = (pool[(size_t)(e & kSolPageMask) * kSolWordsPerPage + (local >> 1)] >>
                     ((local & 1u) * 16u)) & 0xFFFFu;
              const double m = (double)(v & 0xFFu);
              if (m == 0) continue;
              cells++;
              mm += m; mx += m * x; my += m * y; mz += m * z;
              const double dx = x + 0.5 - dc.x, dy = y + 0.5 - dc.y, dz = z + 0.5 - dc.z;
              r2 += m * (dx * dx + dy * dy + dz * dz);
            }
        const Vec3 root = mobs.MobRootPos(id);
        std::printf("stump probe t%d: drips %d centroid (%.1f %.1f %.1f) box x%d..%d "
                    "y%d..%d z%d..%d | root (%.1f %.1f %.1f) | solute %d cells mass %.0f "
                    "centroid (%.1f %.1f %.1f) rms about drips %.1f\n",
                    i + 1, pn, dc.x, dc.y, dc.z, bx0, bx1, by0, by1, bz0, bz1, root.x,
                    root.y, root.z, cells, mm, mm ? mx / mm : 0, mm ? my / mm : 0,
                    mm ? mz / mm : 0, mm ? std::sqrt(r2 / mm) : 0);
        px = py = pz = 0;
        pn = 0;
        bx0 = by0 = bz0 = INT_MAX;
        bx1 = by1 = bz1 = INT_MIN;
      }
      if ((i + 1) % kWin == 0 && (size_t)(i / kWin) < a.mixed.size()) {
        const uint32_t d1 = dissolved();
        a.mixed[(size_t)(i / kWin)] = d1 - d0;
        d0 = d1;
      }
      if (!mobs.IsAlive(id)) {
        a.deathTick = i;
        break;
      }
    }
    return a;
  };
  const Arm wet = run(Vec3{(float)site.x + 0.5f, (float)(bedY + 1), (float)site.z + 0.5f});
  const Arm dry = run(Vec3{(float)(site.x + R + kPadLen / 2) + 0.5f, (float)(surfY + 1),
                           (float)site.z + 0.5f});

  mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  auto list = [&](const std::vector<uint32_t>& v) {
    std::string s;
    for (uint32_t n : v) s += Format("%u ", n);
    return s;
  };
  // NEVER STOPS: every window after the first (the sever's) has blood while
  // the man lives -- a window cut short by death is not counted.
  bool never = wet.spawned && !wet.win.empty();
  for (size_t w = 1; w < wet.win.size(); w++) {
    if (wet.deathTick >= 0 && (int)((w + 1) * kWin) > wet.deathTick) break;
    if (wet.win[w] == 0) never = false;
  }
  // ...AND IT REACHES THE WATER: blood dissolves in every such window. The
  // emitted count alone passed while the owner watched it stop: the drips
  // were painting onto a blood voxel stuck at the wound, which takes nothing.
  bool reaches = wet.spawned && !wet.mixed.empty();
  for (size_t w = 1; w < wet.mixed.size(); w++) {
    if (wet.deathTick >= 0 && (int)((w + 1) * kWin) > wet.deathTick) break;
    if (wet.mixed[w] == 0) reaches = false;
  }
  const float want = CurrentTuning().gore.underwaterBleedRate;
  const float tol = (float)BaselineNumber("stumpWetRatioTol", 0.35);
  const float ratio = dry.total > 0 ? (float)wet.total / (float)dry.total : 0.0f;
  const bool faster = dry.spawned && dry.total > 0 && std::fabs(ratio - want) <= tol;
  detail = Format(
      "wet stump %s: %u drips, per %d ticks [%s], blood units dissolved [%s] %s, "
      "died %d | dry stump: %u drips [%s], died %d | ratio %.2f (want %.2f +- "
      "%.2f) %s",
      never ? "NEVER STOPS" : "STOPPED", wet.total, kWin, list(wet.win).c_str(),
      list(wet.mixed).c_str(), reaches ? "REACHES THE WATER" : "NOT REACHING THE WATER",
      wet.deathTick, dry.total, list(dry.win).c_str(), dry.deathTick, (double)ratio,
      (double)want, (double)tol, faster ? "ok" : "FAIL");
  return never && reaches && faster ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& AquaticGates() {
  static const std::vector<Gate> g = {
      // The swimmers: a shark's hunt, a piranha shoal, the medium, stranding.
      {"aquatic", "mob", {}, false, GateAquatic, false},
      // A walker swims: floats and treads, swims out over a low bank, dives.
      {"npc-swim", "mob", {}, false, GateNpcSwim, false},
      // An amputation under water bleeds on, at underwaterBleedRate x dry.
      {"stump-underwater", "mob", {}, false, GateStumpUnderwater, false},
  };
  return g;
}

}  // namespace selftest
