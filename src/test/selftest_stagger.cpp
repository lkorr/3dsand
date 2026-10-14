// =============================================================================
// stagger — a blow you reel back from (mob.h Mob::NoteStaggerHp,
// MobSystem::StartStagger / StaggerDrive, Tuning::CombatFx stagger*)
// =============================================================================
//
// One walking human (no behaviour profile: the legacy wander drives it straight
// ahead at full speed, which is the "someone running at you" the owner asked
// about) and hp put straight onto its head and torso through MobSystem::Damage
// -- the door a mace and an arrow's flat hp share. Claims, in order:
//
//   A. A SCRATCH DOES NOT STAGGER: a head blow under staggerHeadFrac, and a big
//      head charge that is not a blow (Burn), both leave it walking.
//   B. A TRUNK BLOW STAGGERS IT LIGHTLY: the stagger_chest clip plays, it gives
//      ground for an instant, then walks at about staggerChestMoveScale of its
//      pace, and is back to full pace by the end of staggerChestMs.
//   C. A HEAD BLOW STAGGERS IT HARD: stagger_head plays, it steps BACK (against
//      its facing), then stands -- no walk, no turn -- until staggerHeadMs.
//   D. A HEAD BLOW UPGRADES A LIVE TRUNK STAGGER, and the guard keeps a second
//      trunk blow from re-starting one straight after.
//
// Everything is ratios of the creature's own measured pace, so the gate does
// not care what speed the human is authored at.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/mob.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

using namespace sandvox;

namespace selftest {
namespace {

Status GateStagger(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("stagger: FAILED %s\n", what);
    }
  };
  auto skip = [&](const std::string& why) {
    detail = why;
    std::printf("stagger: SKIP (%s)\n", why.c_str());
    c.mobs.Reset();
    c.debris.Reset();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return Status::Skip;
  };

  // The avatar's def: the human every humanoid inherits, and the rig the
  // stagger clips were solved on.
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == kAvatarDefName) defIndex = (int)i;
  if (defIndex < 0) return skip("no human def");

  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  // The window's middle, on the surface: room to walk ~3 m every way.
  const IVec3 o = c.world.WindowOrigin();
  const int cx = (o.x + (int)kNChunk / 2) * (int)kChunk;
  const int cz = (o.z + (int)kNChunk / 2) * (int)kChunk;
  const int cy = World::TerrainHeight(cx, cz, kDefaultSeed);

  support::TickRig rig(c, 33000, IVec3{cx >> 4, cy >> 4, cz >> 4});
  auto tick = [&](int n) { support::RunTicks(rig, n); };
  const float dt = kTickDt;

  const Tuning::CombatFx& fx = CurrentTuning().combatfx;
  auto ticksOf = [&](float ms) { return (int)std::ceil(ms * 0.001f / dt); };

  // A FRESH WALKER for each arm, so no arm inherits another's guard window.
  struct Walker {
    uint64_t id = 0;
    uint64_t head = 0, torso = 0;
  };
  auto spawn = [&](std::string& why) {
    Walker w;
    c.mobs.Reset();
    w.id = c.mobs.Spawn(defIndex, IVec3{cx, cy + 1, cz});
    if (w.id == 0) {
      why = "Spawn refused";
      return w;
    }
    // NO PROFILE: the legacy wander, which drives forward at full speed.
    c.mobs.SetMobBehavior(w.id, "");
    c.mobs.SetHeading(w.id, 0.0f);
    const MobDef& def = c.mobs.Defs()[defIndex];
    for (size_t i = 0; i < def.limbs.size(); i++) {
      if (def.limbs[i].tag == "head" && !w.head) w.head = c.mobs.LimbBody(w.id, (int)i);
      if (def.limbs[i].name == "torso") w.torso = c.mobs.LimbBody(w.id, (int)i);
    }
    if (!w.head || !w.torso) why = "no head / torso collider";
    tick(40);   // on its feet and walking
    return w;
  };
  auto mob = [&](const Walker& w) { return c.mobs.FindMobById(w.id); };
  auto origin = [&](const Walker& w) { return c.mobs.MobOrigin(w.id); };
  auto flat = [](Vec3 v) { return std::sqrt(v.x * v.x + v.z * v.z); };
  // Its pace over `n` ticks, voxels per tick in the plane.
  auto pace = [&](const Walker& w, int n) {
    const Vec3 a = origin(w);
    tick(n);
    return flat(origin(w) - a) / (float)n;
  };
  auto hit = [&](const Walker& w, uint64_t body, float hp, DamageCause cause) {
    DamageCtx ctx;
    ctx.cause = cause;
    return c.mobs.Damage(body, hp, origin(w), 0.0f, ctx);
  };
  const MobDef& def = c.mobs.Defs()[defIndex];
  float headHp = 0, torsoHp = 0;
  for (const MobLimbDef& l : def.limbs) {
    if (l.tag == "head") headHp = l.hp;
    if (l.name == "torso") torsoHp = l.hp;
  }
  if (!(headHp > 0) || !(torsoHp > 0)) return skip("head / torso has no hp");
  using SK = Mob::StaggerKind;

  std::string why;
  // ---- A. NOT EVERY BLOW --------------------------------------------------
  float pace0 = 0;
  {
    Walker w = spawn(why);
    if (!why.empty()) return skip(why);
    pace0 = pace(w, 15);
    check(pace0 > 0.05f, "the fixture walks (legacy wander)");
    hit(w, w.head, headHp * fx.staggerHeadFrac * 0.4f, DamageCause::Blunt);
    hit(w, w.head, headHp * 0.3f, DamageCause::Burn);
    tick(3);
    check(mob(w) && mob(w)->StaggerNow() == SK::None,
          "a scratch, and a burn, do not stagger");
    check(mob(w) && mob(w)->ClipWeight("stagger_head") == 0.0f,
          "...and play no clip");
  }

  // ---- B. THE TRUNK -------------------------------------------------------
  float chestSlow = 0, chestBack = 0;
  {
    Walker w = spawn(why);
    if (!why.empty()) return skip(why);
    const float p0 = pace(w, 15);
    const Vec3 fwd = mob(w)->Facing();
    const Vec3 a = origin(w);
    check(hit(w, w.torso, torsoHp * fx.staggerChestFrac * 1.5f, DamageCause::Pierce),
          "the torso takes the blow");
    tick(2);
    check(mob(w)->StaggerNow() == SK::Chest, "a trunk blow staggers (Chest)");
    check(mob(w)->ClipWeight("stagger_chest") > 0.0f, "stagger_chest plays");
    // The backstep: half of staggerBackstepMs. Net travel along the facing
    // over it must be backwards or at least not a stride forwards.
    const int step = ticksOf(fx.staggerBackstepMs * 0.5f);
    tick(std::max(0, step - 2));
    const Vec3 d = origin(w) - a;
    chestBack = d.x * fwd.x + d.z * fwd.z;
    check(chestBack < p0 * 2.0f, "it gives ground instead of striding on");
    // The slowed walk: the first half of what is left.
    const int total = ticksOf(fx.staggerChestMs);
    const int slow = std::max(4, (total - step) / 2);
    chestSlow = pace(w, slow) / std::max(p0, 1e-4f);
    check(chestSlow < fx.staggerChestMoveScale + 0.2f,
          "it walks at about staggerChestMoveScale while hunched");
    check(chestSlow > 0.02f || fx.staggerChestMoveScale == 0.0f,
          "...but it does not stop dead (a trunk hit only slows)");
    tick(std::max(0, total - step - slow) + 2);
    check(mob(w)->StaggerNow() == SK::None, "the trunk stagger ends on time");
    const float after = pace(w, 12) / std::max(p0, 1e-4f);
    check(after > 0.75f, "and it catches up: full pace again");
    RecordObserved("stagger.observedChestSlow", chestSlow);
  }

  // ---- C. THE HEAD --------------------------------------------------------
  float headBack = 0, headStill = 0, headTurn = 0;
  {
    Walker w = spawn(why);
    if (!why.empty()) return skip(why);
    const float p0 = pace(w, 15);
    const Vec3 fwd = mob(w)->Facing();
    const Vec3 a = origin(w);
    check(hit(w, w.head, headHp * fx.staggerHeadFrac * 1.5f, DamageCause::Blunt),
          "the head takes the blow");
    tick(2);
    check(mob(w)->StaggerNow() == SK::Head, "a head blow staggers (Head)");
    check(mob(w)->ClipWeight("stagger_head") > 0.0f, "stagger_head plays");
    check(mob(w)->StaggerBlocksAttack(), "a staggered body may not attack");
    const int step = ticksOf(fx.staggerBackstepMs);
    tick(step + 2);
    const Vec3 d = origin(w) - a;
    headBack = d.x * fwd.x + d.z * fwd.z;
    // Momentum: the tick of the blow and the one after still walked forward.
    check(headBack < p0 * 1.0f - 0.2f, "it steps BACK, against its facing");
    const float h0 = c.mobs.MobHeading(w.id);
    const int hold = std::max(4, ticksOf(fx.staggerHeadMs) - step - 8);
    headStill = pace(w, hold) / std::max(p0, 1e-4f);
    headTurn = std::fabs(std::remainder(c.mobs.MobHeading(w.id) - h0, 6.2831853f));
    check(headStill < 0.15f, "then it stands, holding its head");
    check(headTurn < 0.05f, "...without turning");
    tick(12);
    check(mob(w)->StaggerNow() == SK::None, "the head stagger ends");
    RecordObserved("stagger.observedHeadBackVox", headBack);
  }

  // ---- D. UPGRADE, AND THE GUARD ------------------------------------------
  {
    Walker w = spawn(why);
    if (!why.empty()) return skip(why);
    hit(w, w.torso, torsoHp * fx.staggerChestFrac * 1.5f, DamageCause::Blade);
    tick(3);
    check(mob(w)->StaggerNow() == SK::Chest, "upgrade arm: chest first");
    hit(w, w.head, headHp * fx.staggerHeadFrac * 1.5f, DamageCause::Pierce);
    tick(2);
    check(mob(w)->StaggerNow() == SK::Head, "a head blow upgrades a trunk stagger");
    check(mob(w)->ClipWeight("stagger_head") > 0.0f, "...to the head clip");
    tick(ticksOf(fx.staggerHeadMs) + 1);
    check(mob(w)->StaggerNow() == SK::None, "upgrade arm: over");
    if (fx.staggerImmuneMs > 2.0f * dt * 1000.0f) {
      const uint32_t before = mob(w)->StaggersChest();
      hit(w, w.torso, torsoHp * fx.staggerChestFrac * 1.5f, DamageCause::Blade);
      tick(2);
      check(mob(w)->StaggersChest() == before &&
                mob(w)->StaggerNow() == SK::None,
            "the guard: no fresh trunk stagger straight after one ends");
    }
  }

  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  detail = Format("pace %.2f vox/t; chest slow %.2f, back %.2f; head back %.2f, "
                  "still %.2f, turn %.3f",
                  pace0, chestSlow, chestBack, headBack, headStill, headTurn);
  std::printf("stagger: %s (%d checks, %s)\n", ok ? "PASS" : "FAIL", checks,
              detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& StaggerGates() {
  static const std::vector<Gate> g = {
      // A head / trunk blow on a walking human: the clip, the backstep, the
      // slowed walk, the stand, the upgrade and the guard.
      {"stagger", "mob", {}, false, GateStagger, false},
  };
  return g;
}

}  // namespace selftest
