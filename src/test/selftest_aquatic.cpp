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

  // ---- THE SITE: dry ground, chosen off the GPU's own voxels --------------
  // corpse-float's lesson: the harness map's fixture lake relevels a basin
  // stamped inside it, and high ground freezes. The first candidate with no
  // liquid near it wins; every rejection is in the detail.
  constexpr int R = 34, kDepth = 16;
  IVec3 site{};
  int hMin = 0, hMax = 0;
  std::string siteWhy;
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
  if (!sited) {
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
  Vec3 cStart{}, cEnd{};
  if (const Mob* m = mobs.FindMobById(shark2)) cStart = CentreOf(*m);
  const int kMedium = (int)BaselineNumber("aquaticMediumTicks", 360);
  for (int i = 0; i < kMedium; i++) {
    support::RunTicks(rig, 1);
    if (const ai::Brain* b = mobs.MobBrain(shark2); b && b->hasTarget) targeted++;
  }
  if (const Mob* m = mobs.FindMobById(shark2)) cEnd = CentreOf(*m);
  const float patrol = (cEnd - cStart).len();
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
      "(%.1f hp to asphyxia)",
      siteWhy.c_str(), aOk ? "PASS" : "FAIL", swimShare * 100.0f, outOfWater, worstLow,
      worstHigh, sweptAtFirstPass, passes, peakV, cruise > 0 ? peakV / cruise : 0.0f,
      bites, wentPast, lostA, limbsA, hpLostA, victimEnd.c_str(), bOk ? "PASS" : "FAIL",
      nips, swimmingB,
      lostB, limbsB, hpLostB, perBitePiranha, perBiteShark, cOk ? "PASS" : "FAIL",
      targeted, kMedium, patrol, dOk ? "PASS" : "FAIL", maxRollLedge, flopsLedge,
      backInAt, maxRollBank, flopsBank, deadAt, cause, asphyxia);
  return aOk && bOk && cOk && dOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& AquaticGates() {
  static const std::vector<Gate> g = {
      // The swimmers: a shark's hunt, a piranha shoal, the medium, stranding.
      {"aquatic", "mob", {}, false, GateAquatic, false},
  };
  return g;
}

}  // namespace selftest
