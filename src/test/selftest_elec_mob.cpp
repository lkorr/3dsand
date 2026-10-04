// selftest_elec_mob.cpp — SHOCKS REACH BODIES (docs/PLAN_electricity.md
// section 4, package E4; src/game/mob_shock.cpp, sim_elec.wgsl elecQuery).
//
//   elec-water-mob  A. A stone basin of water on a stone pad, a LIGHTNING cell
//                      re-laid at the pond's corner every tick of the hold. A
//                      human standing IN the pond must take Electric hp and be
//                      stunned; a human on the dry pad ten cells off the
//                      basin wall must take none (nothing conducts to it).
//                   B. Three copper_bar plates, each fed by its own spark in a
//                      stone pocket. On them: a human SOAKED in water (every
//                      limb, SoakLimb), a dry one, and a dry one in iron
//                      sabatons (wave 2: the metal where the current runs;
//                      it was a cuirass). Wet must take at least elecMob.wetRatioMin x
//                      the dry one's Electric hp, the armoured one at least
//                      elecMob.armourRatioMin x.
//   elec-stun       A. A zombie (AI on) beside a training dummy on a copper
//                      plate (arc-fed since wave 2), the stun floor lifted to elecStun.stunTicks so
//                      the window covers several of the zombie's attack
//                      cadences. It must attack BEFORE the shock (a fixture
//                      that cannot fail measures nothing), issue no attack and
//                      hold still while stunned (the AI's refused attacks are
//                      counted: ShockAttacksDropped), twitch, never ragdoll,
//                      and attack again within elecStun.recoverTicksMax of the
//                      stun ending.
//                   B. A dummy on a plate fed by LIGHTNING: lightning-class
//                      charge must knock it down (StartRagdoll "shock").
//   elec-player-stun  THE PLAYER (TickAuthority's SHOCKED block, session.cpp):
//                   the rig's session gets a real walking avatar (fly off) on a
//                   spark-fed copper_bar plate, the stun floor lifted to
//                   elecPlayerStun.stunTicks. The avatar must take Electric hp
//                   and be stunned; on every stunned tick the gate presses
//                   FORWARD and the player must not move more than
//                   elecPlayerStun.stillVoxelsMax (the command is zeroed); once
//                   the stun is over, the same press must walk at least
//                   elecPlayerStun.walkVoxelsMin within walkTicks (the control:
//                   a fixture whose player cannot walk proves nothing).
//   elec-replay     Plate + spark + a wet and a dry human under the op
//                   recorder (oprecord.h, what SANDVOX_RECORD_OPS records):
//                   the replay of the record must reproduce every world-hash
//                   probe (the shock's consequences reach the world only as
//                   ops, and those are recorded), and a second LIVE run must
//                   reproduce every mob probe -- total hp, Electric hp, the
//                   stun, the shock record -- bit for bit (the body query is
//                   answered at the fixed snapshot latency, so what a body
//                   takes is a pure function of the tick).
//
// Ticks THE tick (support::TickRig -> TickAuthority): the shock pass lives in
// MobSystem::PreTick and its query rides SubmitTick. Every fixture is built on
// its own stone pad over the harness terrain (FixtureYOver, never an absolute
// Y), walls one tick and contents the next (the first op on a cell wins), and
// the world is regenerated on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "sim/oprecord.h"
#include "sim/tuning.h"
#include "sim/voxload.h"   // kBodyStainAmtMax: a full coat
#include "sim/weather.h"   // the dry sky elec-body-matter pins
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

void Put(std::vector<CellOp>& ops, int x, int y, int z, uint32_t word) {
  ops.push_back({World::SlotCellIndex({x, y, z}), word});
}
// A source pocket: stone on all 26 neighbours of (x, y, z) but the `open` one
// (the conductor it feeds), so a GAS source (spark, arc, lightning) cannot
// drift off. Pushed AFTER the conductor in the same tick, so the conductor's
// cells keep their own op (first op wins).
void Pocket(std::vector<CellOp>& ops, uint32_t stone, int x, int y, int z, int ox, int oy, int oz) {
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        if ((dx == 0 && dy == 0 && dz == 0) || (dx == ox && dy == oy && dz == oz)) continue;
        Put(ops, x + dx, y + dy, z + dz, stone);
      }
}

void Regenerate(Ctx& c) {
  c.mobs.Reset();
  c.debris.Reset();
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
}

// ---- the pad ----------------------------------------------------------------
// A stone pad over the terrain, `hx` x `hz` either side of (x, z) at window
// inset `inset`: stone from below the ground up to y - 1, air from y up (y is
// where feet stand).
struct Fix {
  int x = 0, y = 0, z = 0;
  IVec3 chunk{};
  std::vector<CellOp> pad, build, fill, feed;
};
void PadOps(Ctx& c, Fix& f, int inset, int hx, int hz, uint32_t stone) {
  const IVec3 o = c.world.WindowOrigin();
  f.x = o.x * (int)kChunk + inset;
  f.z = o.z * (int)kChunk + inset;
  f.y = FixtureYOver(f.x - hx - 2, f.z - hz - 2, f.x + hx + 2, f.z + hz + 2, kDefaultSeed, 3);
  for (int zz = f.z - hz; zz <= f.z + hz; zz++)
    for (int xx = f.x - hx; xx <= f.x + hx; xx++) {
      for (int yy = World::TerrainHeight(xx, zz, kDefaultSeed) - 2; yy < f.y; yy++)
        Put(f.pad, xx, yy, zz, stone);
      for (int yy = f.y; yy <= f.y + 26; yy++) Put(f.pad, xx, yy, zz, 0u);
    }
  f.chunk = {f.x >> 4, f.y >> 4, f.z >> 4};
}

// One tick of THE tick with the gate's cell ops pushed first.
void Tick(support::TickRig& rig, const std::vector<CellOp>& cells = {}) {
  support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) { o.cells = cells; });
}
// Pad, build, fill, then the settle the snapshot mirror needs to publish it.
void BuildFix(support::TickRig& rig, const Fix& f) {
  Tick(rig, f.pad);
  Tick(rig, f.build);
  Tick(rig, f.fill);
  for (int i = 0; i < 8; i++) Tick(rig);
}

// A human with its collider CENTRED on (cx, cz), feet at y.
uint64_t SpawnAt(Ctx& c, int def, int cx, int y, int cz, const char* behaviour) {
  const Vec3 ws = c.mobs.Defs()[def].worldSize;
  const uint64_t id = c.mobs.Spawn(def, {cx - (int)(ws.x * 0.5f), y, cz - (int)(ws.z * 0.5f)});
  if (id && behaviour) c.mobs.SetMobBehavior(id, behaviour);
  return id;
}

float ElecHp(Ctx& c, uint64_t id) {
  const Mob* m = c.mobs.FindMobById(id);
  return m ? m->HpLostBy(DamageCause::Electric) : 0.0f;
}
Mob::ShockRecord ShockOf(Ctx& c, uint64_t id) {
  const Mob* m = c.mobs.FindMobById(id);
  return m ? m->Shock() : Mob::ShockRecord{};
}
std::string RecNote(const Mob::ShockRecord& r) {
  return Format("%u shocked ticks, %u limb hits, P max %u (eff %.0f), wet %.2f%s, %.1f hp, "
                "stun %u ticks, %u ragdolls, %u lit, %u twitches",
                r.ticks, r.limbHits, r.maxP, r.maxEffP, r.wetMax, r.armour ? ", ARMOUR" : "",
                r.hp, r.stunTicks, r.ragdolls, r.ignited, r.twitches);
}

// ============================================================================
// elec-water-mob
// ============================================================================
struct PondOut {
  bool spawned = false;
  float wetHp = 0, dryHp = 0;
  Mob::ShockRecord wet, dry;
  bool wetEverStunned = false, dryEverStunned = false;
  bool wetAlive = true;
  uint32_t hitsCharged = 0;
};

PondOut RunPond(Ctx& c, int hd, int hold, int observe) {
  PondOut r;
  Regenerate(c);
  const uint32_t mStone = MatId(c, "stone"), mWater = MatId(c, "water"),
                 mLight = MatId(c, "lightning");
  Fix f;
  PadOps(c, f, 200, 22, 10, mStone);
  // The basin: interior x in [x-18, x-4], z in [z-6, z+6], walls a cell
  // outside it from y to y+3; water three deep.
  const int ix0 = f.x - 18, ix1 = f.x - 4, iz0 = f.z - 6, iz1 = f.z + 6;
  for (int zz = iz0 - 1; zz <= iz1 + 1; zz++)
    for (int xx = ix0 - 1; xx <= ix1 + 1; xx++) {
      const bool wall = xx < ix0 || xx > ix1 || zz < iz0 || zz > iz1;
      if (!wall) continue;
      for (int yy = f.y; yy <= f.y + 3; yy++) Put(f.build, xx, yy, zz, mStone);
    }
  for (int zz = iz0; zz <= iz1; zz++)
    for (int xx = ix0; xx <= ix1; xx++)
      for (int yy = f.y; yy <= f.y + 2; yy++) Put(f.fill, xx, yy, zz, PackVoxNew(mWater, 7));
  // The bolt's foot: a lightning cell in the pond's bottom corner, re-laid
  // every tick of the hold (it decays in a tick or two).
  Put(f.feed, ix0 + 1, f.y, iz1 - 1, mLight);
  uint32_t t = 83000;
  support::TickRig rig(c, t, f.chunk);
  BuildFix(rig, f);
  const uint64_t wet = SpawnAt(c, hd, (ix0 + ix1) / 2, f.y, f.z, "dummy");
  const uint64_t dry = SpawnAt(c, hd, f.x + 12, f.y, f.z, "dummy");
  r.spawned = wet != 0 && dry != 0;
  if (!r.spawned) return r;
  for (int i = 0; i < 12; i++) Tick(rig);
  const uint64_t charged0 = c.world.ElecQueryCounters().charged;
  for (int i = 0; i < hold + observe; i++) {
    Tick(rig, i < hold ? f.feed : std::vector<CellOp>{});
    if (const Mob* m = c.mobs.FindMobById(wet)) r.wetEverStunned |= m->Stunned(rig.tick);
    if (const Mob* m = c.mobs.FindMobById(dry)) r.dryEverStunned |= m->Stunned(rig.tick);
  }
  r.wetHp = ElecHp(c, wet);
  r.dryHp = ElecHp(c, dry);
  r.wet = ShockOf(c, wet);
  r.dry = ShockOf(c, dry);
  r.wetAlive = c.mobs.IsAlive(wet);
  r.hitsCharged = (uint32_t)(c.world.ElecQueryCounters().charged - charged0);
  return r;
}

struct PlateOut {
  bool spawned = false, armoured = false;
  float hp[3] = {0, 0, 0};     // wet, dry, armour
  Mob::ShockRecord rec[3];
  float soak = 0;              // the wet human's water fraction after the soak
};

// Three plates along x, each 7 wide (z from z-6 to z+3), fed by a spark in a
// pocket at its -z end; the humans stand at the plates' centre rows.
PlateOut RunPlates(Ctx& c, int hd, int hold, int observe) {
  PlateOut r;
  Regenerate(c);
  const uint32_t mStone = MatId(c, "stone"), mCopper = MatId(c, "copper_bar"),
                 mSpark = MatId(c, "spark"), mWater = MatId(c, "water");
  Fix f;
  PadOps(c, f, 200, 22, 10, mStone);
  const int px[3] = {f.x - 14, f.x, f.x + 14};
  for (int k = 0; k < 3; k++) {
    for (int zz = f.z - 6; zz <= f.z + 3; zz++)
      for (int xx = px[k] - 3; xx <= px[k] + 3; xx++) Put(f.build, xx, f.y - 1, zz, mCopper);
  }
  for (int k = 0; k < 3; k++) {
    Pocket(f.build, mStone, px[k], f.y - 1, f.z - 7, 0, 0, 1);
    Put(f.feed, px[k], f.y - 1, f.z - 7, mSpark);
  }
  uint32_t t = 84000;
  support::TickRig rig(c, t, f.chunk);
  BuildFix(rig, f);
  uint64_t id[3];
  for (int k = 0; k < 3; k++) id[k] = SpawnAt(c, hd, px[k], f.y, f.z, "dummy");
  r.spawned = id[0] && id[1] && id[2];
  if (!r.spawned) return r;
  // The armour: IRON SABATONS on the third (skipped, and said so, if the
  // item library has none). Since wave 2 (package B) metal armour adds to a
  // shock only when it is IN the current's path -- the body is solved as
  // matter, and a worn conductor counts when it CARRIES charge -- so the
  // metal goes where the plate is: on the feet. (Wave 1 used a cuirass and
  // multiplied by shockArmourGain whenever any worn shell held a conductor.)
  // Original note: the iron cuirass on the third (skipped, and said so, if the
  // item library has none).
  const int cuirassIdx = c.items.Find("iron_sabatons");
  if (const ItemDef* cuirass = cuirassIdx >= 0 ? c.items.At(cuirassIdx) : nullptr) {
    Mob* m = c.mobs.FindMobById(id[2]);
    int home = -1;
    for (int sl = 0; sl < kEquipSlotCount && home < 0; sl++)
      if (EquipSlotAccepts(sl, cuirass->kind)) home = sl;
    r.armoured = m && home >= 0 && m->WearItem(cuirass, home);
  }
  for (int i = 0; i < 12; i++) Tick(rig);
  // The soak: every base limb of the first under a full water coat.
  if (Mob* m = c.mobs.FindMobById(id[0])) {
    for (int li = 0; li < m->AppendedBase(); li++)
      if (c.mobs.LimbBody(id[0], li)) c.mobs.SoakLimb(id[0], li, mWater, kBodyStainAmtMax, rig.tick);
  }
  for (int i = 0; i < hold + observe; i++) Tick(rig, i < hold ? f.feed : std::vector<CellOp>{});
  for (int k = 0; k < 3; k++) {
    r.hp[k] = ElecHp(c, id[k]);
    r.rec[k] = ShockOf(c, id[k]);
  }
  // How wet the shock found the soaked one: the conducting coat of its
  // wettest touching limb, as ApplyShocks measured it.
  r.soak = r.rec[0].wetMax;
  return r;
}

Status GateElecWaterMob(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int hd = c.mobs.FindDef("human");
  for (const char* m : {"stone", "water", "lightning", "copper_bar", "spark"})
    if (MatId(c, m) == 0) {
      detail = Format("missing material %s", m);
      return Status::Fail;
    }
  if (hd < 0) {
    detail = "no `human` def";
    return Status::Skip;
  }
  const int hold = (int)BaselineNumber("elecMob.pondHoldTicks", 20);
  const int observe = (int)BaselineNumber("elecMob.pondObserveTicks", 20);
  const double pondHpMin = BaselineNumber("elecMob.pondHpMin", 20.0);
  const int plateHold = (int)BaselineNumber("elecMob.plateHoldTicks", 10);
  const int plateObserve = (int)BaselineNumber("elecMob.plateObserveTicks", 30);
  const double wetRatioMin = BaselineNumber("elecMob.wetRatioMin", 1.5);
  const double armourRatioMin = BaselineNumber("elecMob.armourRatioMin", 1.2);
  const double dryHpMin = BaselineNumber("elecMob.plateDryHpMin", 0.5);

  const PondOut a = RunPond(c, hd, hold, observe);
  const PlateOut b = RunPlates(c, hd, plateHold, plateObserve);
  Regenerate(c);

  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  check(a.spawned && b.spawned, "a fixture human did not spawn");
  check(a.wetHp >= pondHpMin, Format("A: the human in the pond took %.1f Electric hp (min %.0f)",
                                     a.wetHp, pondHpMin));
  check(a.wet.stunTicks > 0, "A: the human in the pond was never stunned");
  check(a.dryHp == 0.0f && a.dry.ticks == 0 && !a.dryEverStunned,
        Format("A: the human on the dry pad took %.2f Electric hp over %u ticks", a.dryHp,
               a.dry.ticks));
  check(b.hp[1] >= dryHpMin, Format("B: the dry human on the plate took %.2f Electric hp (min %.2f)",
                                    b.hp[1], dryHpMin));
  check(b.soak > 0.5f, Format("B: the soak left a water coat of %.2f", b.soak));
  check(b.hp[0] >= wetRatioMin * b.hp[1],
        Format("B: wet %.2f vs dry %.2f Electric hp (min ratio %.2f)", b.hp[0], b.hp[1],
               wetRatioMin));
  if (b.armoured)
    check(b.hp[2] >= armourRatioMin * b.hp[1] && b.rec[2].armour,
          Format("B: iron sabatons %.2f vs bare %.2f Electric hp (min ratio %.2f)", b.hp[2], b.hp[1],
                 armourRatioMin));
  for (int k = 0; k < 3; k++)
    check(k == 2 && !b.armoured ? true : b.rec[k].stunTicks > 0,
          Format("B: plate human %d never stunned", k));

  RecordObserved("elecMob.pondWetHp", a.wetHp);
  RecordObserved("elecMob.plateWetHp", b.hp[0]);
  RecordObserved("elecMob.plateDryHp", b.hp[1]);
  RecordObserved("elecMob.plateArmourHp", b.hp[2]);
  const World::ElecQueryStats qs = c.world.ElecQueryCounters();
  detail = Format(
      "A pond + lightning: IN the water %.1f Electric hp (%s)%s; on the dry pad %.2f hp (%s) | "
      "B plates + spark: wet (coat %.2f) %.2f hp [%s], dry %.2f hp [%s], %s %.2f hp [%s] | "
      "body query: %llu boxes asked, %llu refused, %llu answered, %llu charged%s%s",
      a.wetHp, RecNote(a.wet).c_str(), a.wetAlive ? "" : ", DIED", a.dryHp,
      RecNote(a.dry).c_str(), b.soak, b.hp[0], RecNote(b.rec[0]).c_str(), b.hp[1],
      RecNote(b.rec[1]).c_str(), b.armoured ? "iron sabatons" : "(no sabatons item)", b.hp[2],
      RecNote(b.rec[2]).c_str(), (unsigned long long)qs.asked, (unsigned long long)qs.refused,
      (unsigned long long)qs.hits, (unsigned long long)qs.charged,
      fails.empty() ? "" : " | FAILED: ", fails.c_str());
  std::printf("elec-water-mob: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-stun
// ============================================================================
Status GateElecStun(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int hd = c.mobs.FindDef("human");
  if (hd < 0) {
    detail = "no `human` def";
    return Status::Skip;
  }
  const uint32_t mStone = MatId(c, "stone"), mCopper = MatId(c, "copper_bar"),
                 mSpark = MatId(c, "spark"), mLight = MatId(c, "lightning");
  const int stunTicks = (int)BaselineNumber("elecStun.stunTicks", 150);
  const int preTicks = (int)BaselineNumber("elecStun.preTicks", 150);
  const int recoverMax = (int)BaselineNumber("elecStun.recoverTicksMax", 150);
  const double stillMax = BaselineNumber("elecStun.stillVoxelsMax", 1.0);
  const int droppedMin = (int)BaselineNumber("elecStun.droppedMin", 1);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };

  // ---- A. the zombie ------------------------------------------------------
  const Tuning saved = CurrentTuning();
  {
    Tuning tt = saved;
    tt.gore.shockStunMinTicks = stunTicks;
    tt.gore.shockStunMaxTicks = std::max(tt.gore.shockStunMaxTicks, stunTicks);
    SetCurrentTuning(tt);
  }
  Regenerate(c);
  Fix f;
  PadOps(c, f, 200, 22, 10, mStone);
  for (int zz = f.z - 6; zz <= f.z + 6; zz++)
    for (int xx = f.x - 12; xx <= f.x + 12; xx++) Put(f.build, xx, f.y - 1, zz, mCopper);
  Pocket(f.build, mStone, f.x - 13, f.y - 1, f.z, 1, 0, 0);
  // An ARC, not a spark, since wave 2: package A's spreading loss (a copper
  // SHEET has four conducting neighbours a cell) leaves a spark-fed plate at
  // P ~50 under the zombie, below gore.shockMinP 60 -- the stun this gate is
  // about would never start. An arc there still stuns without knocking down.
  Put(f.feed, f.x - 13, f.y - 1, f.z, MatId(c, "arc"));
  uint32_t t = 85000;
  int preAttacks = 0, stunAttacks = 0, postAttacks = 0, stunned = 0, strokeWhileStunned = 0;
  int recoverAt = -1, stunEndTick = -1;
  float moved = 0.0f;
  bool ragdolledA = false;
  uint64_t zombie = 0, dummy = 0, dropped = 0;
  Mob::ShockRecord zrec;
  {
    support::TickRig rig(c, t, f.chunk);
    BuildFix(rig, f);
    zombie = SpawnAt(c, hd, f.x - 4, f.y, f.z, "zombie");
    dummy = SpawnAt(c, hd, f.x + 4, f.y, f.z, "training_dummy");
    auto attacksBy = [&](uint64_t id) {
      int n = 0;
      for (const ai::AttackRequest& a : c.mobs.AttackRequests()) n += a.mobId == id;
      return n;
    };
    for (int i = 0; i < preTicks; i++) {
      c.mobs.ClearAttackRequests();
      Tick(rig);
      preAttacks += attacksBy(zombie);
    }
    const uint64_t dropped0 = c.mobs.ShockAttacksDropped();
    Vec3 anchor{};
    bool haveAnchor = false;
    // The pulse (three ticks of spark), then watch until the stun has been
    // over for recoverMax ticks.
    const int budget = 3 + stunTicks + 60 + recoverMax;
    for (int i = 0; i < budget; i++) {
      c.mobs.ClearAttackRequests();
      Tick(rig, i < 3 ? f.feed : std::vector<CellOp>{});
      Mob* z = c.mobs.FindMobById(zombie);
      if (!z || !c.mobs.IsAlive(zombie)) break;
      ragdolledA |= z->Ragdolled();
      const int att = attacksBy(zombie);
      if (z->Stunned(rig.tick)) {
        stunned++;
        stunAttacks += att;
        // The first stunned tick may still be finishing a stroke DecideIntent
        // dropped that very tick; every one after must have none.
        if (stunned > 1 && z->Stroke().Active()) strokeWhileStunned++;
        if (!haveAnchor) {
          anchor = z->Origin();
          haveAnchor = true;
        } else {
          const Vec3 d = z->Origin() - anchor;
          moved = std::max(moved, std::sqrt(d.x * d.x + d.z * d.z));
        }
      } else if (stunned > 0) {
        if (stunEndTick < 0) stunEndTick = i;
        postAttacks += att;
        if (att > 0 && recoverAt < 0) recoverAt = i - stunEndTick;
        if (recoverAt >= 0) break;
      }
    }
    dropped = c.mobs.ShockAttacksDropped() - dropped0;
    zrec = ShockOf(c, zombie);
  }
  SetCurrentTuning(saved);
  check(preAttacks > 0, Format("A: the zombie never attacked before the shock (%d ticks): the "
                               "fixture cannot tell a stun from a calm zombie",
                               preTicks));
  check(stunned >= stunTicks, Format("A: stunned %d ticks (want >= %d)", stunned, stunTicks));
  check(stunAttacks == 0, Format("A: %d attack requests while stunned", stunAttacks));
  check((int)dropped >= droppedMin,
        Format("A: the AI asked to attack %llu times while stunned (min %d): the window "
               "never tested the refusal",
               (unsigned long long)dropped, droppedMin));
  check(strokeWhileStunned == 0, Format("A: a stroke in flight on %d stunned ticks", strokeWhileStunned));
  check(moved <= stillMax, Format("A: moved %.2f voxels while stunned (max %.2f)", moved, stillMax));
  check(zrec.twitches > 0, "A: no twitch while stunned");
  check(!ragdolledA, "A: a spark knocked the zombie down");
  check(recoverAt >= 0 && recoverAt <= recoverMax,
        Format("A: attacked again %d ticks after the stun (max %d)", recoverAt, recoverMax));

  // ---- B. lightning-class: the knock-down -----------------------------------
  Regenerate(c);
  Fix g;
  PadOps(c, g, 200, 22, 10, mStone);
  for (int zz = g.z - 6; zz <= g.z + 6; zz++)
    for (int xx = g.x - 6; xx <= g.x + 6; xx++) Put(g.build, xx, g.y - 1, zz, mCopper);
  Pocket(g.build, mStone, g.x - 7, g.y - 1, g.z, 1, 0, 0);
  Put(g.feed, g.x - 7, g.y - 1, g.z, mLight);
  bool knocked = false;
  Mob::ShockRecord brec;
  {
    support::TickRig rig(c, 86000u, g.chunk);
    BuildFix(rig, g);
    const uint64_t victim = SpawnAt(c, hd, g.x, g.y, g.z, "dummy");
    for (int i = 0; i < 12; i++) Tick(rig);
    for (int i = 0; i < 24; i++) {
      Tick(rig, i < 4 ? g.feed : std::vector<CellOp>{});
      if (const Mob* m = c.mobs.FindMobById(victim)) knocked |= m->Ragdolled() && m->Shock().ragdolls > 0;
    }
    brec = ShockOf(c, victim);
  }
  Regenerate(c);
  check(brec.ragdolls > 0 && knocked,
        Format("B: lightning-class charge (eff P %.0f, ragdoll at %d) did not knock the body "
               "down",
               brec.maxEffP, CurrentTuning().gore.shockRagdollP));

  RecordObserved("elecStun.stunnedTicks", stunned);
  RecordObserved("elecStun.recoverTicks", recoverAt);
  detail = Format(
      "A zombie on a spark-fed plate: %d attacks in %d ticks before; stunned %d ticks (floor %d) "
      "with %d attacks, %llu refused by the stun, %d stroke ticks, moved %.2f vox, %u twitches, "
      "%s; attacked again %d ticks after (%d after in all) [%s] | B lightning-fed plate: [%s]%s%s",
      preAttacks, preTicks, stunned, stunTicks, stunAttacks, (unsigned long long)dropped,
      strokeWhileStunned, moved, zrec.twitches, ragdolledA ? "KNOCKED DOWN" : "never down",
      recoverAt, postAttacks, RecNote(zrec).c_str(), RecNote(brec).c_str(),
      fails.empty() ? "" : " | FAILED: ", fails.c_str());
  std::printf("elec-stun: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-replay
// ============================================================================
struct ReplayScene {
  Fix f;
  int px = 0;
};
constexpr int kReplayTicks = 48, kReplayProbe = 6;

void SceneOps(Ctx& c, ReplayScene& s) {
  const uint32_t mStone = MatId(c, "stone"), mCopper = MatId(c, "copper_bar"),
                 mSpark = MatId(c, "spark");
  PadOps(c, s.f, 200, 14, 10, mStone);
  for (int zz = s.f.z - 6; zz <= s.f.z + 3; zz++)
    for (int xx = s.f.x - 10; xx <= s.f.x + 10; xx++) Put(s.f.build, xx, s.f.y - 1, zz, mCopper);
  Pocket(s.f.build, mStone, s.f.x, s.f.y - 1, s.f.z - 7, 0, 0, 1);
  Put(s.f.feed, s.f.x, s.f.y - 1, s.f.z - 7, mSpark);
}

struct LiveTrace {
  std::vector<uint32_t> hashes;
  std::vector<float> hp;       // per probe: wet total, wet electric, dry total, dry electric
  std::vector<uint32_t> stun;  // per probe: wet, dry stun ticks left
  Mob::ShockRecord rec[2];
  bool spawned = false;
};

LiveTrace RunReplayScene(Ctx& c, int hd, uint64_t idBase) {
  LiveTrace r;
  ReplayScene s;
  SceneOps(c, s);
  const uint32_t mWater = MatId(c, "water");
  support::TickRig rig(c, 87000u, s.f.chunk);
  BuildFix(rig, s.f);
  c.mobs.SetNextIdCounter(idBase);
  const uint64_t id[2] = {SpawnAt(c, hd, s.f.x - 5, s.f.y, s.f.z, "dummy"),
                          SpawnAt(c, hd, s.f.x + 5, s.f.y, s.f.z, "dummy")};
  r.spawned = id[0] && id[1];
  if (!r.spawned) return r;
  for (int i = 0; i < 6; i++) Tick(rig);
  if (Mob* m = c.mobs.FindMobById(id[0]))
    for (int li = 0; li < m->AppendedBase(); li++)
      if (c.mobs.LimbBody(id[0], li)) c.mobs.SoakLimb(id[0], li, mWater, kBodyStainAmtMax, rig.tick);
  for (int i = 0; i < kReplayTicks; i++) {
    Tick(rig, i < 8 ? s.f.feed : std::vector<CellOp>{});
    if ((i + 1) % kReplayProbe != 0) continue;
    r.hashes.push_back(HashWorldNow(c.ctx, c.world, c.sim, kDefaultSeed));
    for (int k = 0; k < 2; k++) {
      const Mob* m = c.mobs.FindMobById(id[k]);
      r.hp.push_back(m ? m->TotalHp() : -1.0f);
      r.hp.push_back(m ? m->HpLostBy(DamageCause::Electric) : -1.0f);
      r.stun.push_back(m ? m->StunTicksLeft(rig.tick) : 0xFFFFFFFFu);
    }
  }
  for (int k = 0; k < 2; k++) r.rec[k] = ShockOf(c, id[k]);
  return r;
}

Status GateElecReplay(Ctx& c, std::string& detail) {
  namespace ops = sandvox::opstream;
  IdCounterScope ids(c.mobs);
  const uint64_t idBase = c.mobs.NextIdCounter();
  const int hd = c.mobs.FindDef("human");
  if (hd < 0) {
    detail = "no `human` def";
    return Status::Skip;
  }
  const std::string path = "build/elec_replay.svops";
  std::string err;
  // ---- pass A: live, recorded ------------------------------------------------
  Regenerate(c);
  if (ops::Recording())
    std::printf("elec-replay: taking over the recorder (a SANDVOX_RECORD_OPS recording ends here)\n");
  if (!ops::StartRecording(path, kDefaultSeed, c.mats, err)) {
    detail = "cannot record: " + err;
    return Status::Fail;
  }
  const LiveTrace a = RunReplayScene(c, hd, idBase);
  const uint32_t frames = ops::RecordedFrames();
  ops::StopRecording();

  // ---- pass B: the record, replayed ------------------------------------------
  std::vector<uint32_t> hashB;
  uint32_t paramMiss = 0;
  {
    ops::Log log;
    if (!log.Load(path, c.mats, err)) {
      detail = "record refused on load: " + err;
      return Status::Fail;
    }
    struct ReplayArm {
      explicit ReplayArm(const ops::Log* l) {
        ops::ResetReplayStats();
        ops::SetReplay(l);
      }
      ~ReplayArm() { ops::SetReplay(nullptr); }
    } arm(&log);
    Regenerate(c);
    // The probes fall on the same ticks as pass A's: the scene's last
    // kReplayTicks frames, every kReplayProbe-th.
    const size_t first = log.frames.size() >= (size_t)kReplayTicks
                             ? log.frames.size() - (size_t)kReplayTicks
                             : 0;
    for (size_t fi = 0; fi < log.frames.size(); fi++) {
      const ops::Frame& fr = log.frames[fi];
      ops::InjectTicketRequestsIfReplaying(fr.in.tick, c.stream.TicketSet());
      c.stream.TicketTick(fr.in.tick);
      ops::ReplaceChunksIfReplaying(fr.in.tick, c.stream);
      SubmitTick(c.ctx, c.world, c.sim, fr.in.tick, fr.in.seed, fr.ops, fr.exps, fr.cells,
                 fr.in.hashEnable != 0,
                 {fr.in.playerChunk[0], fr.in.playerChunk[1], fr.in.playerChunk[2]},
                 fr.in.wantReadback != 0, fr.in.particlesActive != 0, fr.spawns,
                 fr.in.farCount, fr.fluid, fr.in.fluidLive, fr.in.vizActive != 0);
      c.ctx.WaitIdle();
      if (fi >= first && (fi - first + 1) % kReplayProbe == 0)
        hashB.push_back(HashWorldNow(c.ctx, c.world, c.sim, kDefaultSeed));
    }
    paramMiss = ops::ReplayParamMismatches();
  }

  // ---- pass C: live again, unrecorded ------------------------------------------
  Regenerate(c);
  const LiveTrace cc = RunReplayScene(c, hd, idBase);
  Regenerate(c);

  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  check(a.spawned && cc.spawned, "the scene's humans did not spawn");
  check(a.rec[0].hp > 0.0f && a.rec[1].hp > 0.0f,
        Format("the scene shocked nobody (wet %.2f, dry %.2f Electric hp): the replay would "
               "prove nothing",
               a.rec[0].hp, a.rec[1].hp));
  size_t worldDiff = a.hashes.size();
  for (size_t i = 0; i < a.hashes.size() && i < hashB.size(); i++)
    if (a.hashes[i] != hashB[i]) {
      worldDiff = i;
      break;
    }
  check(a.hashes.size() == hashB.size() && worldDiff == a.hashes.size(),
        Format("replay diverged at probe %zu of %zu (%08x vs %08x)", worldDiff, a.hashes.size(),
               worldDiff < a.hashes.size() ? a.hashes[worldDiff] : 0u,
               worldDiff < hashB.size() ? hashB[worldDiff] : 0u));
  check(paramMiss == 0, Format("%u TickParams words rebuilt differently on replay", paramMiss));
  const bool sameHash = a.hashes == cc.hashes;
  const bool sameHp = a.hp == cc.hp && a.stun == cc.stun;
  const bool sameRec = a.rec[0].hp == cc.rec[0].hp && a.rec[1].hp == cc.rec[1].hp &&
                       a.rec[0].ticks == cc.rec[0].ticks && a.rec[1].ticks == cc.rec[1].ticks &&
                       a.rec[0].stunTicks == cc.rec[0].stunTicks &&
                       a.rec[1].stunTicks == cc.rec[1].stunTicks;
  check(sameHash, "a second live run moved a world-hash probe");
  check(sameHp && sameRec, Format("a second live run moved the mobs: wet %.3f vs %.3f, dry %.3f vs "
                                  "%.3f Electric hp",
                                  a.rec[0].hp, cc.rec[0].hp, a.rec[1].hp, cc.rec[1].hp));
  detail = Format(
      "%u frames recorded, %zu world probes replayed %s, %u TickParams mismatches; live twice: "
      "world %s, mob hp/stun %s (wet [%s], dry [%s])%s%s",
      frames, hashB.size(), worldDiff == a.hashes.size() ? "identical" : "DIVERGED", paramMiss,
      sameHash ? "identical" : "DIFFERS", sameHp && sameRec ? "identical" : "DIFFERS",
      RecNote(a.rec[0]).c_str(), RecNote(a.rec[1]).c_str(), fails.empty() ? "" : " | FAILED: ",
      fails.c_str());
  std::printf("elec-replay: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-player-stun
// ============================================================================
Status GateElecPlayerStun(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  if (c.mobs.FindDef(kAvatarDefName) < 0) {
    detail = Format("no `%s` def", kAvatarDefName);
    return Status::Skip;
  }
  const uint32_t mStone = MatId(c, "stone"), mCopper = MatId(c, "copper_bar"),
                 mSpark = MatId(c, "spark");
  const int stunTicks = (int)BaselineNumber("elecPlayerStun.stunTicks", 40);
  const int feedTicks = (int)BaselineNumber("elecPlayerStun.feedTicks", 4);
  const double stillMax = BaselineNumber("elecPlayerStun.stillVoxelsMax", 0.5);
  const int walkTicks = (int)BaselineNumber("elecPlayerStun.walkTicks", 40);
  const double walkMin = BaselineNumber("elecPlayerStun.walkVoxelsMin", 4.0);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  const Tuning saved = CurrentTuning();
  {
    Tuning tt = saved;
    tt.gore.shockStunMinTicks = stunTicks;
    tt.gore.shockStunMaxTicks = std::max(tt.gore.shockStunMaxTicks, stunTicks);
    SetCurrentTuning(tt);
  }
  Regenerate(c);
  Fix f;
  PadOps(c, f, 200, 22, 12, mStone);
  // The plate: copper under the feet, x +-6, z from z-6 to z+2 (the player
  // walks +z off its end once the stun is over, onto the stone pad).
  for (int zz = f.z - 6; zz <= f.z + 2; zz++)
    for (int xx = f.x - 6; xx <= f.x + 6; xx++) Put(f.build, xx, f.y - 1, zz, mCopper);
  Pocket(f.build, mStone, f.x, f.y - 1, f.z - 7, 0, 0, 1);
  Put(f.feed, f.x, f.y - 1, f.z - 7, mSpark);
  float elecHp = 0.0f;
  int stunned = 0, walkStart = -1;
  float stillMoved = 0.0f, walked = 0.0f;
  bool spawned = false, everStunned = false;
  Mob::ShockRecord rec;
  {
    support::TickRig rig(c, 87000u, f.chunk);
    BuildFix(rig, f);
    // THE BODY: the rig's session leaves fly mode, so phase I spawns the
    // avatar where the player stands (session.cpp `wantAvatar`), and the
    // shock pass asks about its limbs like any body (avatars first).
    PlayerSession& s = rig.Session();
    s.avatar.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
    s.avatar.SetDefs(&c.mobs.Defs(), kAvatarDefName);
    c.mobs.SetAvatar(&s.avatar);
    s.player.fly = false;
    s.player.grounded = true;
    s.player.vel = Vec3{0, 0, 0};
    s.player.pos = Vec3{(float)f.x + 0.5f, (float)f.y + Player::kHalfY + 0.05f, (float)f.z + 0.5f};
    for (int i = 0; i < 20; i++) Tick(rig);
    spawned = s.avatar.Spawned();
    if (spawned) {
      Vec3 anchor{};
      bool haveAnchor = false;
      Vec3 walk0{};
      // The pulse, then: press FORWARD on every tick the body is stunned
      // (the tick is told before it runs: Stunned(t) for the t about to
      // run), and keep pressing for walkTicks once the stun is over.
      const int budget = feedTicks + 30 + 2 * stunTicks + walkTicks;
      for (int i = 0; i < budget; i++) {
        const uint32_t next = rig.tick + 1;
        const bool stun = s.avatar.Stunned(next);
        const bool press = stun || walkStart >= 0;
        support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
          if (i < feedTicks) o.cells = f.feed;
          if (press) o.input.forward = 1.0f;
        });
        if (stun) {
          everStunned = true;
          stunned++;
          if (!haveAnchor) {
            anchor = s.player.pos;
            haveAnchor = true;
          } else {
            const Vec3 d = s.player.pos - anchor;
            stillMoved = std::max(stillMoved, std::sqrt(d.x * d.x + d.z * d.z));
          }
        } else if (everStunned && walkStart < 0) {
          walkStart = i;
          walk0 = s.player.pos;
        }
        if (walkStart >= 0) {
          const Vec3 d = s.player.pos - walk0;
          walked = std::max(walked, std::sqrt(d.x * d.x + d.z * d.z));
          if (i - walkStart >= walkTicks) break;
        }
      }
      elecHp = s.avatar.HpLostBy(DamageCause::Electric);
      rec = s.avatar.Shock();
    }
    // The avatar's limbs are Jolt bodies the rig's session owns: gone before
    // the rig is.
    c.mobs.SetAvatar(nullptr);
    if (s.avatar.Spawned()) s.avatar.Despawn();
  }
  SetCurrentTuning(saved);
  Regenerate(c);
  check(spawned, "the avatar never spawned (fly off, def set)");
  check(elecHp > 0.0f, "no Electric hp on the player's body");
  check(stunned >= stunTicks / 2,
        Format("stunned %d ticks (want >= %d of the %d-tick floor)", stunned, stunTicks / 2,
               stunTicks));
  check(stillMoved <= stillMax,
        Format("moved %.2f voxels with FORWARD held while stunned (max %.2f): the command was "
               "not zeroed",
               stillMoved, stillMax));
  check(walkStart >= 0 && walked >= walkMin,
        Format("after the stun, FORWARD walked %.2f voxels in %d ticks (min %.2f): the control "
               "arm cannot walk, so the stunned stillness proves nothing",
               walked, walkTicks, walkMin));
  detail = Format("player on a spark-fed copper plate (%d ticks): %.2f Electric hp, stunned %d "
                  "ticks (floor %d), moved %.2f voxels with FORWARD held while stunned (max "
                  "%.2f), then walked %.2f in %d ticks (min %.2f); shock [%s]%s%s",
                  feedTicks, elecHp, stunned, stunTicks, stillMoved, stillMax, walked, walkTicks,
                  walkMin, RecNote(rec).c_str(), fails.empty() ? "" : " | FAILED: ",
                  fails.c_str());
  std::printf("elec-player-stun: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-crowd (wave 2, package B: bug 2)
// ============================================================================
// THE WHOLE CROWD IS ASKED. Wave 1 queued one box per LIMB against a 128-box
// cap in spawn order, so a crowd past ~8 humans was never shocked. Now one box
// per body, sized from the creature cap: MaxLiveMobs() humans on a pad in rows
// of eight, the LAST rows (the bodies spawned last, which wave 1 refused)
// standing in a lightning-fed basin. Every one in the water must take Electric
// hp; every one on the dry pad none; nothing refused.
Status GateElecCrowd(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int hd = c.mobs.FindDef("human");
  if (hd < 0) {
    detail = "no `human` def";
    return Status::Skip;
  }
  const uint32_t mStone = MatId(c, "stone"), mWater = MatId(c, "water"),
                 mLight = MatId(c, "lightning");
  const int hold = (int)BaselineNumber("elecCrowd.holdTicks", 16);
  const int observe = (int)BaselineNumber("elecCrowd.observeTicks", 12);
  const int n = (int)MobSystem::MaxLiveMobs();
  const int cols = 8, rows = (n + cols - 1) / cols;
  const int wetRows = rows >= 4 ? 2 : 1;
  Regenerate(c);
  Fix f;
  const int dz = 6, wetGap = 6;
  const int depth = rows * dz + wetGap + 8;
  PadOps(c, f, 200, 24, depth / 2 + 4, mStone);
  auto colX = [&](int col) { return f.x - 18 + col * 5; };
  const int z0 = f.z - depth / 2 + 2;
  auto rowZ = [&](int row) { return z0 + row * dz + (row >= rows - wetRows ? wetGap : 0); };
  // The basin round the wet rows: water three deep, walls a cell outside.
  const int bz0 = rowZ(rows - wetRows) - 3, bz1 = rowZ(rows - 1) + 3;
  const int bx0 = f.x - 22, bx1 = f.x + 21;
  for (int zz = bz0 - 1; zz <= bz1 + 1; zz++)
    for (int xx = bx0 - 1; xx <= bx1 + 1; xx++) {
      const bool wall = xx < bx0 || xx > bx1 || zz < bz0 || zz > bz1;
      for (int yy = f.y; yy <= f.y + 3; yy++)
        if (wall) Put(f.build, xx, yy, zz, mStone);
        else if (yy <= f.y + 2) Put(f.fill, xx, yy, zz, PackVoxNew(mWater, 7));
    }
  // Bolts' feet along the basin floor, every eight cells (the charge reaches
  // every wet body, whatever spreading loss the field grows).
  for (int xx = bx0 + 1; xx <= bx1; xx += 8) Put(f.feed, xx, f.y, bz0, mLight);
  uint32_t t = 88000;
  support::TickRig rig(c, t, f.chunk);
  BuildFix(rig, f);
  std::vector<uint64_t> id;
  std::vector<bool> wetBody;
  for (int k = 0; k < n; k++) {
    const int row = k / cols, col = k % cols;
    const uint64_t m = SpawnAt(c, hd, colX(col), f.y, rowZ(row), "dummy");
    if (m) {
      id.push_back(m);
      wetBody.push_back(row >= rows - wetRows);
    }
  }
  for (int i = 0; i < 12; i++) Tick(rig);
  const World::ElecQueryStats q0 = c.world.ElecQueryCounters();
  const MobSystem::ShockCounters s0 = c.mobs.ShockStats();
  for (int i = 0; i < hold + observe; i++) Tick(rig, i < hold ? f.feed : std::vector<CellOp>{});
  const World::ElecQueryStats q1 = c.world.ElecQueryCounters();
  const MobSystem::ShockCounters s1 = c.mobs.ShockStats();
  int wetN = 0, wetHit = 0, dryN = 0, dryHit = 0, lastWetMiss = -1;
  float wetHpMin = 1e30f;
  // ATTRIBUTION for a wet body that took nothing: did the field reach its
  // box (raw box P), and did any of its cells charge?
  int wetBoxP = 0;
  std::string misses;
  for (size_t k = 0; k < id.size(); k++) {
    const Mob::ShockRecord r = ShockOf(c, id[k]);
    const float hp = ElecHp(c, id[k]);
    if (wetBody[k]) {
      wetN++;
      if (r.maxP > 0) wetBoxP++;
      if (hp > 0.0f) wetHit++;
      else {
        lastWetMiss = (int)k;
        if (misses.size() < 400)
          misses += Format("%s#%zu box P %u cells %u peak %u", misses.empty() ? "" : ", ", k,
                           r.maxP, r.cellsPeak, r.peakCellP);
      }
      wetHpMin = std::min(wetHpMin, hp);
    } else {
      dryN++;
      if (hp > 0.0f || r.ticks > 0) dryHit++;
    }
  }
  Regenerate(c);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  check((int)id.size() == n, Format("%zu of %d humans spawned", id.size(), n));
  check(wetN > 0 && wetHit == wetN,
        Format("%d of %d humans in the charged water were shocked, %d saw charge in their box "
               "(spawn index %d missed; misses: %s)",
               wetHit, wetN, wetBoxP, lastWetMiss, misses.c_str()));
  check(dryHit == 0, Format("%d of %d humans on the dry pad were shocked", dryHit, dryN));
  check(q1.refused == q0.refused && s1.refused == s0.refused,
        Format("%llu bodies refused by the query (%llu for the grid budget)",
               (unsigned long long)(q1.refused - q0.refused),
               (unsigned long long)(q1.gridRefused - q0.gridRefused)));
  const uint64_t calls = s1.applyCalls - s0.applyCalls;
  const double usPerTick =
      calls ? (double)(s1.applyNanos - s0.applyNanos) / 1000.0 / (double)calls : 0.0;
  const double refreshUs =
      calls ? (double)(s1.refreshNanos - s0.refreshNanos) / 1000.0 / (double)calls : 0.0;
  RecordObserved("elecCrowd.applyUsPerTick", usPerTick);
  detail = Format(
      "%zu humans (cap %d): %d/%d in the lightning-fed basin shocked (least %.1f Electric hp), "
      "%d/%d on the dry pad | query: %llu boxes asked, %llu refused | conduction: %llu body-ticks "
      "(%llu linked), %llu cells, slot bins %llu built / %llu re-accumulated / %llu reused, ohmic %llu cells / %llu "
      "voxels (%llu refused), crackle %llu ops (%llu refused); ApplyShocks %.0f us a tick (%.0f of it re-binning "
      "the slots)%s%s",
      id.size(), n, wetHit, wetN, wetN ? wetHpMin : 0.0f, dryHit, dryN,
      (unsigned long long)(q1.asked - q0.asked), (unsigned long long)(q1.refused - q0.refused),
      (unsigned long long)(s1.bodiesSolved - s0.bodiesSolved),
      (unsigned long long)(s1.linked - s0.linked), (unsigned long long)(s1.cellsSolved - s0.cellsSolved),
      (unsigned long long)(s1.cacheBuilds - s0.cacheBuilds),
      (unsigned long long)(s1.cacheAccums - s0.cacheAccums),
      (unsigned long long)(s1.cacheHits - s0.cacheHits),
      (unsigned long long)(s1.ohmicCells - s0.ohmicCells),
      (unsigned long long)(s1.ohmicVoxels - s0.ohmicVoxels),
      (unsigned long long)(s1.ohmicRefused - s0.ohmicRefused),
      (unsigned long long)(s1.crackleOps - s0.crackleOps),
      (unsigned long long)(s1.crackleRefused - s0.crackleRefused), usPerTick, refreshUs,
      fails.empty() ? "" : " | FAILED: ", fails.c_str());
  std::printf("elec-crowd: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

// ============================================================================
// elec-body-matter (wave 2, package B: the material decides)
// ============================================================================
// Six bodies, each on its own arc-fed copper plate, the same charge under every
// one; what each takes is its matter's:
//   barefoot human          skin and flesh conduct and FEEL: Electric hp, stun
//   human in leather shoes  the sole is an insulator between foot and plate:
//                           nothing (the worn shell covers the foot's cells)
//   human in iron sabatons  iron conducts into the foot: Electric hp
//   sylvan (dryad), on a LIGHTNING-fed plate (dry wood, resist 1,500, takes
//                           nothing from an arc-fed one, world or body)
//                           wood carries the charge and feels none of it: no
//                           Electric hp, no stun -- and it CHARS / ignites (the
//                           ohmic rule on its body cells, wood's own data)
//   android (courier), dry  reported (its panels are an insulator; whatever
//                           its exposed frame does is what it does)
//   android, soaked         the wet rule on its panels lets the plate in; its
//                           circuitry feels it: Electric hp
Status GateElecBodyMatter(Ctx& c, std::string& detail) {
  IdCounterScope ids(c.mobs);
  const int hd = c.mobs.FindDef("human");
  const int sy = c.mobs.FindDef("dryad");
  const int an = c.mobs.FindDef("courier");
  if (hd < 0) {
    detail = "no `human` def";
    return Status::Skip;
  }
  const uint32_t mStone = MatId(c, "stone"), mCopper = MatId(c, "copper_bar"),
                 mArc = MatId(c, "arc"), mWater = MatId(c, "water"),
                 mLight = MatId(c, "lightning");
  const int hold = (int)BaselineNumber("elecMatter.holdTicks", 16);
  const int observe = (int)BaselineNumber("elecMatter.observeTicks", 16);
  // A DRY day: the scheduled sky can rain on the harness, and rain is a
  // conducting coat -- the wet rule would (rightly) let a soaked leather
  // sole conduct, which is not what this fixture measures. Restored on the
  // way out.
  const std::string weatherWas = weather::Override();
  weather::SetOverride("clear");
  Regenerate(c);
  Fix f;
  PadOps(c, f, 200, 48, 10, mStone);
  constexpr int kBodies = 6;
  int px[kBodies];
  for (int k = 0; k < kBodies; k++) {
    px[k] = f.x - 40 + k * 16;
    for (int zz = f.z - 6; zz <= f.z + 3; zz++)
      for (int xx = px[k] - 3; xx <= px[k] + 3; xx++) Put(f.build, xx, f.y - 1, zz, mCopper);
    Pocket(f.build, mStone, px[k], f.y - 1, f.z - 7, 0, 0, 1);
    // The sylvan's plate is fed by LIGHTNING: dry wood (resist 1,500) takes
    // nothing from an arc-fed plate's ~1,200 -- in the world as on a body.
    Put(f.feed, px[k], f.y - 1, f.z - 7, k == 3 ? mLight : mArc);
  }
  uint32_t t = 89000;
  support::TickRig rig(c, t, f.chunk);
  BuildFix(rig, f);
  const int defOf[kBodies] = {hd, hd, hd, sy, an, an};
  const char* name[kBodies] = {"barefoot human", "human in leather shoes", "human in iron sabatons",
                               "sylvan", "android (dry)", "android (soaked)"};
  uint64_t id[kBodies] = {};
  for (int k = 0; k < kBodies; k++)
    if (defOf[k] >= 0) id[k] = SpawnAt(c, defOf[k], px[k], f.y, f.z, "dummy");
  auto wear = [&](uint64_t mid, const char* item) {
    const int idx = c.items.Find(item);
    const ItemDef* it = idx >= 0 ? c.items.At(idx) : nullptr;
    Mob* m = mid ? c.mobs.FindMobById(mid) : nullptr;
    if (!it || !m) return false;
    for (int sl = 0; sl < kEquipSlotCount; sl++)
      if (EquipSlotAccepts(sl, it->kind) && m->WearItem(it, sl)) return true;
    return false;
  };
  const bool shoes = wear(id[1], "shoes");
  const bool sabatons = wear(id[2], "iron_sabatons");
  for (int i = 0; i < 12; i++) Tick(rig);
  if (Mob* m = id[5] ? c.mobs.FindMobById(id[5]) : nullptr)
    for (int li = 0; li < m->AppendedBase(); li++)
      if (c.mobs.LimbBody(id[5], li)) c.mobs.SoakLimb(id[5], li, mWater, kBodyStainAmtMax, rig.tick);
  bool stunned[kBodies] = {};
  for (int i = 0; i < hold + observe; i++) {
    Tick(rig, i < hold ? f.feed : std::vector<CellOp>{});
    for (int k = 0; k < kBodies; k++)
      if (const Mob* m = id[k] ? c.mobs.FindMobById(id[k]) : nullptr)
        stunned[k] = stunned[k] || m->Stunned(rig.tick);
  }
  float hp[kBodies];
  Mob::ShockRecord rec[kBodies];
  std::vector<std::string> slotNames[kBodies];
  for (int k = 0; k < kBodies; k++) {
    hp[k] = id[k] ? ElecHp(c, id[k]) : 0.0f;
    rec[k] = id[k] ? ShockOf(c, id[k]) : Mob::ShockRecord{};
    if (const Mob* m = id[k] ? c.mobs.FindMobById(id[k]) : nullptr)
      for (int sl = 0; sl < 63; sl++) slotNames[k].push_back(m->SlotName(sl));
    slotNames[k].push_back("#63+");
  }
  Regenerate(c);
  std::string fails;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok) fails += (fails.empty() ? "" : "; ") + what;
  };
  check(id[0] != 0, "the barefoot human did not spawn");
  check(hp[0] > 0.0f && stunned[0], Format("barefoot: %.2f Electric hp, %s", hp[0],
                                           stunned[0] ? "stunned" : "never stunned"));
  if (shoes)
    check(hp[1] == 0.0f && !stunned[1],
          Format("leather shoes: %.2f Electric hp (the sole should insulate)", hp[1]));
  if (sabatons)
    check(hp[2] > 0.0f, Format("iron sabatons: %.2f Electric hp (iron should conduct)", hp[2]));
  if (id[3]) {
    check(hp[3] == 0.0f && !stunned[3],
          Format("sylvan: %.2f Electric hp%s (wood feels nothing)", hp[3],
                 stunned[3] ? ", stunned" : ""));
    check(rec[3].ohmicVoxels > 0 && rec[3].cellsPeak > 0,
          Format("sylvan: %u charged cells, %u voxels charred / lit (wood should char)",
                 rec[3].cellsPeak, rec[3].ohmicVoxels));
  }
  if (id[5]) check(hp[5] > 0.0f, Format("soaked android: %.2f Electric hp (its circuitry feels)", hp[5]));
  std::string rows;
  for (int k = 0; k < kBodies; k++) {
    if (!id[k]) {
      rows += Format("%s%s: (no def)", k ? " | " : "", name[k]);
      continue;
    }
    // The slots that took the world's charge directly, by name (attribution).
    std::string seeded;
    for (int sl = 0; sl < 64; sl++)
      if (rec[k].seededSlots & (1ull << sl))
        seeded += (seeded.empty() ? "" : ",") + slotNames[k][std::min<size_t>(sl, slotNames[k].size() - 1)];
    rows += Format("%s%s%s: %.2f hp%s, cells %u (P peak %u), ohmic %u cells / %u voxels, %u "
                   "crackles%s, seeded [%s], %u covered",
                   k ? " | " : "", name[k],
                   (k == 1 && !shoes) || (k == 2 && !sabatons) ? " (ITEM NOT WORN)" : "", hp[k],
                   stunned[k] ? " stunned" : "", rec[k].cellsPeak, rec[k].peakCellP, rec[k].ohmic,
                   rec[k].ohmicVoxels, rec[k].crackles, rec[k].armour ? ", armour" : "",
                   seeded.c_str(), rec[k].covered);
  }
  weather::SetOverride(weatherWas);
  RecordObserved("elecMatter.barefootHp", hp[0]);
  RecordObserved("elecMatter.sabatonHp", hp[2]);
  RecordObserved("elecMatter.sylvanOhmicVoxels", rec[3].ohmicVoxels);
  RecordObserved("elecMatter.androidSoakedHp", hp[5]);
  detail = rows + (fails.empty() ? "" : " | FAILED: ") + fails;
  std::printf("elec-body-matter: %s (%s)\n", fails.empty() ? "PASS" : "FAIL", detail.c_str());
  return fails.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& ElecMobGates() {
  static const std::vector<Gate> g = {
      {"elec-water-mob", "mob", {}, false, GateElecWaterMob},
      {"elec-stun", "mob", {}, false, GateElecStun},
      {"elec-player-stun", "mob", {}, false, GateElecPlayerStun},
      {"elec-replay", "mob", {}, false, GateElecReplay},
      {"elec-crowd", "mob", {}, false, GateElecCrowd},
      {"elec-body-matter", "mob", {}, false, GateElecBodyMatter},
  };
  return g;
}

}  // namespace selftest
