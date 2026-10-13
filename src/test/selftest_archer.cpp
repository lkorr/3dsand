// selftest_archer.cpp -- NPC ARCHERS (docs/PLAN_projectiles.md package E;
// game/mob_archery.h, ai_behavior.h RangedTuning).
//
//   archer  Two humans on a stone strip, through THE tick (support::TickRig):
//           an `archer` (behaviors.json; armed by ArmArcher -- the bow in its
//           right fist, the gate's sword as its SIDEARM in the sheath, a
//           quiver) and a `training_dummy` of another faction to shoot at.
//
//           A. DRAW, LOOSE, HIT. Target `archer.gap` voxels off: the AI asks
//              (the `shoot` intent), the owner draws -- the player's bow_draw
//              clip on, Mob::BowDrawn the whole draw, which lasts at least the
//              bow's drawTicks, the body holding FaceTarget -- and looses ONE
//              arrow at the bow's full draw speed through the SAME
//              ArrowFlights the player's bow uses; it gets INTO the target
//              (an arrow Hit on the dummy that is not a glance) within
//              `archer.hitTicks`. One arrow left the archer's kit per loose.
//           B. BACK OFF. Target `archer.closeGap` voxels off (inside the
//              profile's minRange): over `archer.backoffTicks` the archer
//              gives ground (its distance grows by at least
//              `archer.backoffMin`), never draws while the target is inside
//              minRange, and never swings (its rules scale `attack` to 0
//              while it has arrows and is not cornered).
//           C. OUT OF ARROWS. Two arrows: exactly two loosed, then nothing
//              more is drawn or loosed for `archer.dryTicks`; the kit holds no
//              arrow, the bow went into the bag and the sheathed sword came
//              into the hand (the Sidearm event), and -- the target alive --
//              the archer closes on it (the melee band) by at least
//              `archer.closeMin`.
//           D. REPLAY. A run again on a fresh world: every archery event, every
//              arrow event and the dummy's hp, bit for bit, identical.
//
// Thresholds in tests/baseline.json (`archer.*`). Every scene regenerates the
// world, rewinds the creature ids and Jolt's body id history (ids are
// simulation input) and pins a clear dawn sky, so a scene is a function of
// its inputs alone; the world is regenerated on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "game/ai_behavior.h"
#include "game/arrows.h"
#include "game/equipment.h"
#include "game/mob.h"
#include "game/mob_archery.h"
#include "game/session.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/worldgen_run.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

uint32_t MatId(const Ctx& c, const char* name) {
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == name) return (uint32_t)i;
  return 0;
}

// A FRESH PROCESS'S STATE for one scene: every body torn down, Jolt's id
// history and the creature ids rewound, a clear dawn sky, worldgen again.
void Fresh(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.phys.ResetBirthOrdinals();
  c.phys.ResetBodyIdHistory();
  c.mobs.SetNextIdCounter(1);
  c.mobs.SetWeatherRain(0u);
  c.mobs.SetRainSlope(0, 0);
  c.mobs.SetDayPhase(0u);
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// A flat STONE STRIP along +x, clear air above it: nothing between the two
// bodies but the shot, and room behind the archer to give ground.
struct Strip {
  int x = 0, y = 0, z = 0;   // the archer's column; the target stands at +x
  IVec3 chunk{};
  std::vector<CellOp> cells;
};

Strip BuildStrip(Ctx& c, int behind, int ahead, int halfZ) {
  Strip s;
  const IVec3 o = c.world.WindowOrigin();
  s.x = o.x * (int)kChunk + 200;
  s.z = o.z * (int)kChunk + 200;
  s.y = FixtureYOver(s.x - behind, s.z - halfZ, s.x + ahead, s.z + halfZ, kDefaultSeed, 1);
  const uint32_t stone = PackVoxNew(MatId(c, "stone"), 0);
  for (int zz = s.z - halfZ; zz <= s.z + halfZ; zz++)
    for (int xx = s.x - behind; xx <= s.x + ahead; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < s.y; yy++)
        s.cells.push_back({World::SlotCellIndex({xx, yy, zz}), stone});
      for (int yy = s.y; yy <= s.y + 26; yy++)
        s.cells.push_back({World::SlotCellIndex({xx, yy, zz}), 0u});
    }
  s.chunk = {s.x >> 4, s.y >> 4, s.z >> 4};
  return s;
}

void Lay(support::TickRig& rig, const Strip& s) {
  const size_t step = kMaxCellOpsPerTick / 2;
  for (size_t i = 0; i < s.cells.size(); i += step) {
    const std::vector<CellOp> part(s.cells.begin() + (ptrdiff_t)i,
                                   s.cells.begin() + (ptrdiff_t)std::min(s.cells.size(), i + step));
    support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = part; });
  }
}

// One scene: the strip, an archer at its column (facing +x), the dummy `gap`
// voxels on (centre to centre), `arrows` in the archer's kit.
struct Scene {
  std::unique_ptr<support::TickRig> rig;
  Strip strip;
  uint64_t archer = 0, target = 0;
  std::string err;
};

Scene Stage(Ctx& c, uint32_t clock, float gap, int arrows) {
  Scene s;
  Fresh(c);
  s.strip = BuildStrip(c, 110, 90, 12);
  s.rig = std::make_unique<support::TickRig>(c, clock, s.strip.chunk);
  Lay(*s.rig, s.strip);
  const int hd = c.mobs.FindDef("human");
  const ItemDef* sword = c.items.Named("sword");
  if (hd < 0 || sword == nullptr) {
    s.err = "no human def or no sword item";
    return s;
  }
  const Vec3 ws = c.mobs.Defs()[(size_t)hd].worldSize;
  auto spawnAt = [&](float cx) {
    return c.mobs.Spawn(hd, IVec3{(int)std::floor(cx - ws.x * 0.5f), s.strip.y,
                                  (int)std::floor((float)s.strip.z + 0.5f - ws.z * 0.5f)});
  };
  s.target = spawnAt((float)s.strip.x + 0.5f + gap);
  s.archer = spawnAt((float)s.strip.x + 0.5f);
  if (s.target == 0 || s.archer == 0) {
    s.err = "the two humans did not spawn";
    return s;
  }
  c.mobs.SetMobBehavior(s.target, "training_dummy");
  c.mobs.SetHeading(s.target, -1.5707963f);   // facing the archer (-x)
  c.mobs.SetMobBehavior(s.archer, "archer");
  c.mobs.SetHeading(s.archer, 1.5707963f);    // facing the dummy (+x)
  // The spawn panel's path: the picked weapon in the fist, then the profile
  // arms it (the sword becomes the sidearm) -- ArmArcher.
  c.mobs.EquipItem(s.archer, sword);
  std::string note;
  if (ArmArcher(c.mobs, s.archer, c.items, &note) < 0) {
    s.err = "ArmArcher refused: " + note;
    return s;
  }
  // The scene's own count of arrows.
  if (Mob* m = c.mobs.FindMobById(s.archer)) {
    const ItemDef* bow = HeldBow(*m, c.items);
    if (bow == nullptr) {
      s.err = "the archer holds no bow after ArmArcher";
      return s;
    }
    Kit& kit = m->KitMut();
    while (KitTakeAmmo(kit, bow->bow.ammo)) {
    }
    if (arrows > 0) m->AddCarried(StackOf(c.items, c.items.Find(bow->bow.ammo), arrows));
  }
  return s;
}

float Gap(Ctx& c, const Scene& s) {
  Vec3 a{}, b{}, lo{}, hi{};
  if (c.mobs.MobBodyBox(s.archer, lo, hi)) a = (lo + hi) * 0.5f;
  if (c.mobs.MobBodyBox(s.target, lo, hi)) b = (lo + hi) * 0.5f;
  return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

uint32_t Bits(float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  return u;
}

// Every fact a scene produced, one line each, the bits of every float: the
// replay digest is these lines, and a mismatch names the first that differs.
std::vector<std::string> Facts(Ctx& c, Scene& s) {
  std::vector<std::string> f;
  for (const ArcheryEvent& e : s.rig->Authority().archers.events)
    f.push_back(Format("archery kind %d tick %u mob %llu tgt %llu why '%s' from %08x,%08x,%08x "
                       "dir %08x,%08x,%08x speed %08x seed %08x drawn %d item %s",
                       (int)e.kind, e.tick, (unsigned long long)e.mobId,
                       (unsigned long long)e.targetId, e.why, Bits(e.from.x), Bits(e.from.y),
                       Bits(e.from.z), Bits(e.dir.x), Bits(e.dir.y), Bits(e.dir.z),
                       Bits(e.speed), e.seed, e.drawn, e.item.c_str()));
  for (const ArrowEvent& e : s.rig->Authority().arrows.Events())
    f.push_back(Format("arrow kind %d tick %u seed %08x mob %llu limb %d slot %d at "
                       "%08x,%08x,%08x outcome %s cells %u hp %08x",
                       (int)e.kind, e.tick, e.seed, (unsigned long long)e.mobId, e.limb, e.slot,
                       Bits(e.at.x), Bits(e.at.y), Bits(e.at.z), PierceOutcomeName(e.r.outcome),
                       e.r.cells, Bits(e.r.hpCharged)));
  f.push_back(Format("target hp %08x archer at %08x,%08x,%08x",
                     Bits(c.mobs.TotalHp(s.target)), Bits(c.mobs.MobOrigin(s.archer).x),
                     Bits(c.mobs.MobOrigin(s.archer).y), Bits(c.mobs.MobOrigin(s.archer).z)));
  return f;
}

}  // namespace

Status GateArcher(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  std::string fails, lines;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) {
      fails += (fails.empty() ? "" : "; ") + what;
      std::printf("archer: FAILED %s\n", what.c_str());
    }
  };
  const ItemDef* bow = c.items.Named("bow");
  const int pi = c.mobs.Behaviors().Find("archer");
  const ai::Profile* prof = c.mobs.Behaviors().At(pi);
  if (bow == nullptr || !bow->IsBow() || prof == nullptr || prof->ranged.maxRange <= 0.0f) {
    detail = "no bow item, or no `archer` profile with a ranged block in behaviors.json";
    std::printf("archer: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const ai::RangedTuning& rt = prof->ranged;
  const float gap = (float)BaselineNumber("archer.gap", 60.0);
  const int hitTicks = (int)BaselineNumber("archer.hitTicks", 150.0);
  const float closeGap = (float)BaselineNumber("archer.closeGap", 9.0);
  const int backoffTicks = (int)BaselineNumber("archer.backoffTicks", 60.0);
  const float backoffMin = (float)BaselineNumber("archer.backoffMin", 10.0);
  const int dryTicks = (int)BaselineNumber("archer.dryTicks", 150.0);
  const float closeMin = (float)BaselineNumber("archer.closeMin", 10.0);
  const uint32_t clock = 99000u;

  // ======== A. DRAW, LOOSE, HIT (and the facts for D) ==============================
  struct RunA {
    int drawTick = -1, looseTick = -1, hitTick = -1;
    int drawnTicks = 0;          // ticks the body was BowDrawn before the loose
    float clipMax = 0.0f;        // the bow_draw clip's weight at its strongest
    bool facedThrough = true;    // FaceTarget every drawn tick
    float looseSpeed = 0.0f;
    std::string hitOutcome = "none";
    int kitBefore = 0, kitAfter = 0, loosed = 0, embedded = 0, exited = 0;
    std::string sheath, hand;
    std::vector<std::string> facts;
    std::string err;
  };
  auto runA = [&]() {
    RunA r;
    Scene s = Stage(c, clock, gap, 6);
    if (!s.err.empty()) {
      r.err = s.err;
      return r;
    }
    Mob* m0 = c.mobs.FindMobById(s.archer);
    r.kitBefore = m0 ? KitAmmoCount(m0->GetKit(), bow->bow.ammo) : 0;
    r.sheath = m0 ? m0->GetKit().equip.slots[(int)EquipSlotId::Sheath].name : "";
    r.hand = m0 ? m0->HeldItem(Hand::Right) : "";
    const std::string clip = m0 ? m0->HandClip("bow_draw", Hand::Right) : "";
    const uint32_t t0 = s.rig->tick;
    // The whole window, not just to the first hit: the replay (D) then
    // covers a few shots, the flights after them and what they did.
    for (int i = 0; i < hitTicks; i++) {
      support::RunTicks(*s.rig, 1);
      Mob* m = c.mobs.FindMobById(s.archer);
      if (m == nullptr) break;
      const int now = (int)(s.rig->tick - t0);
      if (r.looseTick < 0 && m->BowDrawn()) {
        r.drawnTicks++;
        if (!clip.empty()) r.clipMax = std::max(r.clipMax, m->ClipWeight(clip));
        // From the draw's second tick: on its first the brain's intent is
        // still the `shoot` that asked for it (the draw starts after PreTick).
        const ai::Brain* br = c.mobs.MobBrain(s.archer);
        if (r.drawnTicks > 1 && (br == nullptr || br->intent != ai::Intent::FaceTarget))
          r.facedThrough = false;
      }
      for (const ArcheryEvent& e : s.rig->Authority().archers.events) {
        if (e.kind == ArcheryEvent::Kind::Draw && r.drawTick < 0)
          r.drawTick = (int)(e.tick - t0);
        if (e.kind == ArcheryEvent::Kind::Loose && r.looseTick < 0) {
          r.looseTick = (int)(e.tick - t0);
          r.looseSpeed = e.speed;
        }
      }
      for (const ArrowEvent& e : s.rig->Authority().arrows.Events())
        if (e.kind == ArrowEvent::Kind::Hit && e.mobId == s.target && e.r.hit &&
            !e.r.missed && e.r.outcome != PierceOutcome::Glanced && r.hitTick < 0) {
          r.hitTick = now;
          // WHERE it went in and how far, not just how it ended: an arrow
          // through a forearm and one through a chest are both "exited".
          const auto& L = c.mobs.Defs()[(size_t)c.mobs.FindDef("human")].limbs;
          std::string mats;
          for (const PierceMatCells& m : e.r.byMat)
            mats += Format("%s%s %u", mats.empty() ? "" : ", ",
                           m.mat < c.mats.size() ? c.mats[m.mat].name.c_str() : "?",
                           m.cells);
          r.hitOutcome = Format("%s, %s %.1f cm, %.1f J in, cells: %s",
                                PierceOutcomeName(e.r.outcome),
                                e.limb >= 0 && e.limb < (int)L.size()
                                    ? L[(size_t)e.limb].name.c_str()
                                    : "?",
                                e.r.depth * kVoxelMeters * 100.0f, e.r.energyIn,
                                mats.empty() ? "none" : mats.c_str());
        }
    }
    // A few ticks more so the last flight finishes its story.
    support::RunTicks(*s.rig, 8);
    for (const ArcheryEvent& e : s.rig->Authority().archers.events)
      if (e.kind == ArcheryEvent::Kind::Loose) r.loosed++;
    for (const ArrowEvent& e : s.rig->Authority().arrows.Events()) {
      if (e.kind == ArrowEvent::Kind::Embedded && e.mobId == s.target) r.embedded++;
      if (e.kind == ArrowEvent::Kind::Hit && e.mobId == s.target &&
          e.r.outcome == PierceOutcome::Exited && !e.r.missed)
        r.exited++;
    }
    if (Mob* m = c.mobs.FindMobById(s.archer)) r.kitAfter = KitAmmoCount(m->GetKit(), bow->bow.ammo);
    r.facts = Facts(c, s);
    return r;
  };
  const RunA a1 = runA();
  check(a1.err.empty(), "A: the scene stages (" + a1.err + ")");
  if (a1.err.empty()) {
    check(a1.hand == bow->name && a1.sheath == "sword",
          Format("A: ArmArcher put the bow in the right fist and the sword in the sheath "
                 "(fist '%s', sheath '%s')",
                 a1.hand.c_str(), a1.sheath.c_str()));
    check(a1.drawTick >= 0, "A: the archer drew (a Draw event)");
    check(a1.looseTick >= 0, "A: the archer loosed (a Loose event)");
    check(a1.drawnTicks >= bow->bow.drawTicks,
          Format("A: the string was back at least the bow's draw, %d ticks (%d)",
                 bow->bow.drawTicks, a1.drawnTicks));
    check(a1.clipMax >= 0.5f, Format("A: the bow_draw clip played (weight %.2f)", a1.clipMax));
    check(a1.facedThrough, "A: the archer held FaceTarget through the draw");
    check(std::fabs(a1.looseSpeed - bow->bow.drawSpeed) <= 0.01f * bow->bow.drawSpeed,
          Format("A: a full draw looses at the bow's draw speed (%.1f of %.1f vox/s)",
                 a1.looseSpeed, bow->bow.drawSpeed));
    check(a1.hitTick >= 0,
          Format("A: an arrow got INTO the dummy within %d ticks (outcome %s)", hitTicks,
                 a1.hitOutcome.c_str()));
    check(a1.kitAfter == a1.kitBefore - a1.loosed,
          Format("A: one arrow left the kit per loose (%d -> %d, %d loosed)", a1.kitBefore,
                 a1.kitAfter, a1.loosed));
  }
  lines += Format("A: gap %.0f: drew +%d, loosed +%d at %.0f vox/s after %d ticks drawn "
                  "(clip %.2f), into the dummy +%d (%s); %d loosed in %d ticks, %d embedded, "
                  "%d out the far side of a limb, kit %d -> %d | ",
                  gap, a1.drawTick, a1.looseTick, a1.looseSpeed, a1.drawnTicks, a1.clipMax,
                  a1.hitTick, a1.hitOutcome.c_str(), a1.loosed, hitTicks, a1.embedded,
                  a1.exited, a1.kitBefore, a1.kitAfter);
  RecordObserved("archer.hitTick", (double)a1.hitTick);

  // ======== B. BACK OFF ===============================================================
  {
    Scene s = Stage(c, clock + 5000u, closeGap, 6);
    check(s.err.empty(), "B: the scene stages (" + s.err + ")");
    if (s.err.empty()) {
      const float g0 = Gap(c, s);
      int drewInside = 0, cornered = 0;
      for (int i = 0; i < backoffTicks; i++) {
        support::RunTicks(*s.rig, 1);
        const ai::Brain* br = c.mobs.MobBrain(s.archer);
        if (br != nullptr && br->facts[(int)ai::Fact::Cornered] > 0.0f) cornered++;
        const Mob* m = c.mobs.FindMobById(s.archer);
        if (m != nullptr && m->BowDrawn() && Gap(c, s) < rt.minRange) drewInside++;
      }
      const float g1 = Gap(c, s);
      const ai::Brain* br = c.mobs.MobBrain(s.archer);
      const uint32_t swings = br ? br->attacksIssued : 0u;
      lines += Format("B: dummy at %.1f (minRange %.0f): after %d ticks %.1f (+%.1f), drew "
                      "inside minRange %d ticks, swings %u, cornered %d ticks | ",
                      g0, rt.minRange, backoffTicks, g1, g1 - g0, drewInside, swings, cornered);
      RecordObserved("archer.backoff", (double)(g1 - g0));
      check(g0 < rt.minRange, Format("B: the dummy starts inside minRange (%.1f)", g0));
      check(g1 - g0 >= backoffMin,
            Format("B: the archer gave ground, %.1f voxels (min %.1f)", g1 - g0, backoffMin));
      check(drewInside == 0, Format("B: no draw inside minRange (%d ticks)", drewInside));
      check(swings == 0, Format("B: no swing while it had arrows (%u)", swings));
    }
  }

  // ======== C. OUT OF ARROWS ==========================================================
  {
    Scene s = Stage(c, clock + 10000u, gap, 2);
    check(s.err.empty(), "C: the scene stages (" + s.err + ")");
    if (s.err.empty()) {
      const uint32_t t0 = s.rig->tick;
      // Until the second loose (bounded), then dryTicks more.
      int lastLoose = -1, loosed = 0;
      for (int i = 0; i < 4 * hitTicks && loosed < 2; i++) {
        support::RunTicks(*s.rig, 1);
        loosed = 0;
        for (const ArcheryEvent& e : s.rig->Authority().archers.events)
          if (e.kind == ArcheryEvent::Kind::Loose) {
            loosed++;
            lastLoose = (int)(e.tick - t0);
          }
      }
      const float gDry = Gap(c, s);
      const ai::Brain* br0 = c.mobs.MobBrain(s.archer);
      const uint32_t shots0 = br0 ? br0->shotsIssued : 0u;
      support::RunTicks(*s.rig, dryTicks);
      int loosedAll = 0, drawsAfter = 0, sidearm = 0;
      std::string drawn;
      for (const ArcheryEvent& e : s.rig->Authority().archers.events) {
        if (e.kind == ArcheryEvent::Kind::Loose) loosedAll++;
        if (e.kind == ArcheryEvent::Kind::Draw && (int)(e.tick - t0) > lastLoose) drawsAfter++;
        if (e.kind == ArcheryEvent::Kind::Sidearm) {
          sidearm++;
          drawn = e.item;
        }
      }
      const Mob* m = c.mobs.FindMobById(s.archer);
      const ai::Brain* br = c.mobs.MobBrain(s.archer);
      const uint32_t shots1 = br ? br->shotsIssued : 0u;
      const int left = m ? KitAmmoCount(m->GetKit(), bow->bow.ammo) : -1;
      const std::string fist = m ? m->HeldItem(Hand::Right) : "";
      bool bowInBag = false;
      if (m)
        for (const ItemStack& st : m->GetKit().bag.slots)
          if (st.name == bow->name) bowInBag = true;
      const bool targetAlive = c.mobs.IsAlive(s.target);
      const float gEnd = Gap(c, s);
      lines += Format("C: 2 arrows: loosed %d (last +%d), %d more draws and %u more shot "
                      "requests over %d ticks, kit %d, fist '%s', bow in bag %d, sidearm %d "
                      "('%s'), dummy %s, gap %.1f -> %.1f",
                      loosedAll, lastLoose, drawsAfter, shots1 - shots0, dryTicks, left,
                      fist.c_str(), (int)bowInBag, sidearm, drawn.c_str(),
                      targetAlive ? "alive" : "DEAD", gDry, gEnd);
      check(loosedAll == 2, Format("C: exactly two arrows loosed (%d)", loosedAll));
      check(drawsAfter == 0 && shots1 == shots0,
            Format("C: nothing drawn or asked for at 0 ammo (%d draws, %u requests)", drawsAfter,
                   shots1 - shots0));
      check(left == 0, Format("C: no arrow left in the kit (%d)", left));
      check(sidearm == 1 && fist == "sword" && bowInBag,
            Format("C: the bow went into the bag and the sheathed sword came out (sidearm %d, "
                   "fist '%s', bow in bag %d)",
                   sidearm, fist.c_str(), (int)bowInBag));
      if (targetAlive)
        check(gDry - gEnd >= closeMin,
              Format("C: out of arrows it closes in to fight (%.1f -> %.1f, min %.1f)", gDry,
                     gEnd, closeMin));
    }
  }

  // ======== D. REPLAY =================================================================
  {
    const RunA a2 = runA();
    size_t k = 0;
    while (k < a1.facts.size() && k < a2.facts.size() && a1.facts[k] == a2.facts[k]) k++;
    const bool same = a2.err.empty() && a1.facts == a2.facts && !a1.facts.empty();
    lines += Format(" | D: replay %s (%zu facts)", same ? "IDENTICAL" : "DIFFERS",
                    a1.facts.size());
    check(same, Format("D: the scene did not replay bit-identically: first difference at fact "
                       "%zu of %zu/%zu: [%s] vs [%s]",
                       k, a1.facts.size(), a2.facts.size(),
                       k < a1.facts.size() ? a1.facts[k].c_str() : "-",
                       k < a2.facts.size() ? a2.facts[k].c_str() : "-"));
  }

  // Pristine ground and no bodies for whoever is next.
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  detail = lines;
  if (!fails.empty()) detail += " | FAILED: " + fails;
  std::printf("archer: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace selftest
