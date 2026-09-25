// selftest_pose.cpp — pose-parity: ONE pose pipeline, whoever drives the body.
//
// WHY THIS EXISTS (rule-unification W2-L). NPCs and the player used to pose
// through two copies of the same pipeline — MobSystem::UpdateAnimation /
// UpdateGait and PlayerAvatar::UpdateAnimation / UpdateGait — kept in step by
// hand and drifting (a per-call velocity blend against a half-life, a free
// oscillator against a foot-synced stride clock, an air pose only one of them
// had). They are one function now, Mob::PosePipeline (game/pose.h), and this
// gate is what keeps them one: the SAME PoseInputs, fed tick by tick to an NPC
// spawned by MobSystem and to a PlayerAvatar, on the same def at the same spot,
// must produce the same pose — every part's model-space position and rotation,
// the drawn height and the drawn tilt — within a tolerance, over a walk, a
// run, a jump and its landing.
//
// WHAT WOULD MAKE IT FAIL. Anything that lets the DRIVER'S IDENTITY back into
// the pose: a virtual override the pipeline calls, spawn-time state one driver
// seeds and the other does not (the avatar marks its leg feet valid at Spawn,
// an NPC's BuildRig does not — the pipeline has to absorb that), or a stage
// guarded by "is this the player". Two arms, one per driver's usual inputs
// (the player's driver-owned height; the NPC's feet-derived height and ground
// tilt), so neither height mode is exempt.
//
// CPU ONLY after the fixture: the bodies are never stepped by Jolt or by either
// driver's own PreTick — only PosePipeline runs — so both can stand on the
// same column and read the same ground. The ground is real terrain, fetched
// into the chunk cache before the first posed tick.
//
// THE FIXTURE CANNOT PASS BY STANDING STILL: it asserts the walk stepped, the
// stride clock locked, the air pose took the body and the pose left rest.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/mob.h"
#include "game/player.h"
#include "sim/scale.h"
#include "sim/tuning.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {

namespace {

// The residency window's centre, in voxels (selftest_combat.cpp's
// FixtureCentre: never an absolute coordinate).
IVec3 WindowCentre(const World& world) {
  const IVec3 o = world.WindowOrigin();
  return IVec3{(o.x + (int)kNChunk / 2) * (int)kChunk, 0,
               (o.z + (int)kNChunk / 2) * (int)kChunk};
}

struct ArmResult {
  bool ok = true;
  float worstPos = 0, worstRot = 0, worstBodyY = 0, worstUp = 0;
  int worstTick = -1;
  const char* worstPhase = "";
  int footfalls = 0;
  float strideRate = 0;
  float peakAir = 0;
  float poseTravel = 0;   // how far the pose moved from its first posed tick
  std::string why;
};

// The phase of the script at tick `t`.
const char* PhaseAt(int t) {
  return t < 60 ? "walk" : t < 120 ? "run" : t < 150 ? "air" : "land";
}

ArmResult RunArm(Ctx& c, int defIndex, IVec3 spot, bool driverHeight) {
  ArmResult r;
  const MobDef& def = c.mobs.Defs()[defIndex];

  // ---- the NPC, spawned the way every NPC is ------------------------------
  const uint64_t npcId = c.mobs.Spawn(defIndex, spot);
  Mob* npc = npcId != 0 ? c.mobs.FindMobById(npcId) : nullptr;
  if (npc == nullptr) {
    r.ok = false;
    r.why = "MobSystem::Spawn refused the fixture";
    return r;
  }
  // ---- the avatar, spawned the way the player's is ------------------------
  PlayerAvatar av;
  av.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av.SetDefs(&c.mobs.Defs(), def.name);
  Player pl;
  pl.fly = false;
  pl.pos = Vec3{(float)spot.x + def.worldSize.x * 0.5f,
                (float)spot.y + Player::kHalfY,
                (float)spot.z + def.worldSize.z * 0.5f};
  if (!av.Spawn(pl, 0.0f)) {
    r.ok = false;
    r.why = "PlayerAvatar::Spawn refused the fixture";
    return r;
  }

  const float tolPos = (float)BaselineNumber("poseParity.tolVox", 0.01);
  const float tolRot = (float)BaselineNumber("poseParity.tolRad", 0.002);
  const float walk = def.speed * 0.55f;
  const float run = def.speed;
  const float g = MetresToCells(CurrentTuning().physics.gravity);
  const float jumpV = MetresPerSecToCells(5.25f);

  Vec3 origin{(float)spot.x, (float)spot.y, (float)spot.z};
  const float heading = 0.0f;  // +Z
  float vy = 0.0f;
  std::vector<Transform> first;
  uint32_t tick = 41000;
  for (int t = 0; t < 180; t++, tick++) {
    const char* phase = PhaseAt(t);
    PoseInputs in;
    float speed = t < 60 ? walk : t < 150 ? run : run * std::max(0.0f, 1.0f - (t - 150) / 15.0f);
    in.grounded = true;
    if (t >= 120 && t < 150) {
      // A JUMP: launched at the avatar's rise speed, ballistic from there.
      if (t == 120) vy = jumpV;
      vy -= g * kTickDt;
      origin.y += vy * kTickDt;
      in.grounded = false;
      in.airPoseEligible = true;
    } else {
      vy = 0.0f;
    }
    origin.z += speed * kTickDt;
    // Standing on whatever the terrain says is under the body, read ONCE for
    // both — the claim is that the same inputs give the same pose, not that
    // two probes agree. During the jump the body is wherever the arc put it;
    // the landing snaps back to the ground.
    if (in.grounded) {
      int gy = 0;
      if (npc->GroundHeightAt(c.world, ifloor(origin.x + def.worldSize.x * 0.5f),
                              ifloor(origin.z + def.worldSize.z * 0.5f),
                              ifloor(origin.y) + 4, gy))
        origin.y = (float)gy;
    }
    in.velocity = Vec3{0.0f, vy, speed};
    if (driverHeight) {
      in.height = PoseInputs::Height::FromDriver;
      in.tiltFromGround = false;
      in.haveLook = true;
      in.lookYaw = 0.35f * std::sin((float)t * 0.07f);
      in.lookPitch = 0.10f;
      in.lookSpineShare = CurrentTuning().avatar.headLookSpine;
    } else {
      in.height = PoseInputs::Height::FromFeet;
      in.tiltFromGround = true;
      in.chaseWant = t < 120;
    }
    npc->SetDriverPlacement(origin, heading);
    av.SetDriverPlacement(origin, heading);
    npc->PosePipeline(in, kTickDt, c.world, tick);
    av.PosePipeline(in, kTickDt, c.world, tick);

    const auto& a = npc->ModelPose();
    const auto& b = av.ModelPose();
    if (a.size() != b.size() || a.empty()) {
      r.ok = false;
      r.why = "the two rigs posed a different number of parts";
      break;
    }
    float dp = 0, dr = 0;
    for (size_t i = 0; i < a.size(); i++) {
      dp = std::max(dp, (a[i].pos - b[i].pos).len());
      // The relative rotation's angle, in the atan2 form: `2 acos(|dot|)` has
      // no precision near identity in float (it reads 0.001 rad for two
      // bit-identical quaternions), which would hide a real small difference.
      const Quat& qa = a[i].rot;
      const Quat& qb = b[i].rot;
      const Quat dq = QuatMul(Quat{-qa.x, -qa.y, -qa.z, qa.w}, qb);
      const float s = std::sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
      dr = std::max(dr, 2.0f * std::atan2(s, std::fabs(dq.w)));
    }
    const float dy = std::fabs(npc->BodyY() - av.BodyY());
    const float du = (npc->BodyUp() - av.BodyUp()).len();
    if (dp > r.worstPos || dr > r.worstRot || dy > r.worstBodyY || du > r.worstUp) {
      r.worstTick = t;
      r.worstPhase = phase;
    }
    r.worstPos = std::max(r.worstPos, dp);
    r.worstRot = std::max(r.worstRot, dr);
    r.worstBodyY = std::max(r.worstBodyY, dy);
    r.worstUp = std::max(r.worstUp, du);
    if (first.empty()) first = a;
    for (size_t i = 0; i < a.size() && i < first.size(); i++)
      r.poseTravel = std::max(r.poseTravel, (a[i].pos - first[i].pos).len());
    r.peakAir = std::max(r.peakAir, std::min(npc->AirPoseWeight(), av.AirPoseWeight()));
    if (t == 119) r.strideRate = std::min(npc->StrideRate(), av.StrideRate());
    r.footfalls += (int)npc->Footfalls().size();
    npc->ClearFootfalls();
    av.ClearFootfalls();
  }
  if (r.worstPos > tolPos || r.worstRot > tolRot || r.worstBodyY > tolPos ||
      r.worstUp > tolRot)
    r.ok = false;
  av.Despawn();
  return r;
}

Status GatePoseParity(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == kAvatarDefName) defIndex = (int)i;
  if (defIndex < 0) {
    detail = std::string("no \"") + kAvatarDefName + "\" def";
    std::printf("pose-parity: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  // ---- the stage: pristine terrain, the ground fetched into the cache -----
  c.debris.Reset();
  c.mobs.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  const IVec3 centre = WindowCentre(c.world);
  const IVec3 spot{centre.x, World::TerrainHeight(centre.x, centre.z, kDefaultSeed),
                   centre.z};
  // The script walks ~150 voxels along +Z. Every chunk it can probe is asked
  // for up front and a few empty ticks carry them (World::kFetchPerTick each).
  uint32_t tick = 40000;
  const IVec3 pc{spot.x >> 4, spot.y >> 4, spot.z >> 4};
  for (int round = 0; round < 3; round++) {
    for (int cz = (spot.z >> 4) - 1; cz <= ((spot.z + 180) >> 4); cz++)
      for (int cx = (spot.x >> 4) - 1; cx <= (spot.x >> 4) + 1; cx++)
        for (int cy = (spot.y >> 4) - 2; cy <= (spot.y >> 4) + 1; cy++)
          c.world.RequestChunkFetch({cx, cy, cz});
    for (int i = 0; i < 4; i++) {
      ++tick;
      SubmitTick(c.ctx, c.world, c.sim, tick, kDefaultSeed, {}, {}, {}, false,
                 pc, true, false);
      c.ctx.WaitIdle();
      c.ctx.ProcessEvents();
    }
  }

  const ArmResult drv = RunArm(c, defIndex, spot, /*driverHeight=*/true);
  c.mobs.Reset();
  c.debris.Reset();
  const ArmResult ft = RunArm(c, defIndex, spot, /*driverHeight=*/false);

  c.mobs.Reset();
  c.debris.Reset();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  bool ok = drv.ok && ft.ok;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("pose-parity: FAILED %s\n", what);
    }
  };
  for (const ArmResult* a : {&drv, &ft}) {
    const char* arm = a == &drv ? "driver-height" : "feet-height";
    std::printf(
        "pose-parity %-13s worst part %.4f vox / %.4f rad, bodyY %.4f, up "
        "%.4f (tick %d, %s) | footfalls %d, stride %.2f Hz at the run's end, "
        "peak air %.2f, pose travel %.1f vox%s%s\n",
        arm, a->worstPos, a->worstRot, a->worstBodyY, a->worstUp, a->worstTick,
        a->worstPhase, a->footfalls, a->strideRate, a->peakAir, a->poseTravel,
        a->why.empty() ? "" : " | ", a->why.c_str());
    // THE FIXTURE MUST HAVE EXERCISED WHAT IT CLAIMS TO COMPARE.
    check(a->why.empty(), "the fixture spawned both bodies");
    check(a->footfalls >= 6, "the walk and run stepped (footfalls)");
    check(a->strideRate > 0.2f, "the stride clock locked onto the feet");
    check(a->peakAir > 0.8f, "the jump put both bodies in the air pose");
    check(a->poseTravel > 1.0f, "the pose left its first tick");
  }
  RecordObserved("poseParity.observedWorstVox", std::max(drv.worstPos, ft.worstPos));
  RecordObserved("poseParity.observedWorstRad", std::max(drv.worstRot, ft.worstRot));
  detail = Format("driver-height worst %.4f vox %.4f rad, feet-height worst "
                  "%.4f vox %.4f rad over walk/run/air/land",
                  drv.worstPos, drv.worstRot, ft.worstPos, ft.worstRot);
  std::printf("pose-parity: %s (%d checks, %s)\n", ok ? "PASS" : "FAIL",
              checks, detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& PoseGates() {
  static const std::vector<Gate> g = {
      {"pose-parity", "mob", {}, false, GatePoseParity},
  };
  return g;
}

}  // namespace selftest
