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

// ---- ledge-climb-pose ----------------------------------------------------
//
// The ledge climb is a muscle-up (player.h ledgeclimb): the BODY rises on an
// authored curve with slow beats in it, and the pose (pose.cpp, "the ledge
// climb") is keyed on how far it has risen. This drives a real Player up the
// player-ledgegrab gate's plateau — jump, catch, dangle, hold W — and feeds
// the avatar the PoseInputs PlayerAvatar::PreTick would, every tick, so what
// is asserted is the composed thing a player sees:
//   - the rise has its beats: after the pull starts there are at least two
//     separate stretches where the body nearly stops (the stop at the top of
//     the pull, the lag while the knee swings on);
//   - through the pull the hands stay ON the lip (the arms are what move);
//   - while the weight is on the knee, the lead knee is ON the lip;
//   - standing at the end, both feet are on the lip and the hands have come
//     down below the shoulders.
// CPU only: the wall is a kindAt lambda, the body is never stepped by Jolt.
Status GateLedgeClimbPose(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  int defIndex = -1;
  for (size_t i = 0; i < c.mobs.Defs().size(); i++)
    if (c.mobs.Defs()[i].name == kAvatarDefName) defIndex = (int)i;
  if (defIndex < 0) {
    detail = std::string("no \"") + kAvatarDefName + "\" def";
    std::printf("ledge-climb-pose: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const MobDef& def = c.mobs.Defs()[defIndex];
  auto partNamed = [&](const char* n) {
    for (size_t i = 0; i < def.limbs.size(); i++)
      if (def.limbs[i].name == n) return (int)i;
    return -1;
  };
  const int shoulderR = partNamed("armU.R"), wristR = partNamed("hand.R");
  const int wristL = partNamed("hand.L");
  // The LEAD leg is the first leg chain (pose.cpp counts leg ordinals).
  int leadHip = -1, leadKnee = -1, leadAnkle = -1, trailAnkle = -1;
  for (const IkChain& ch : def.skel.chains) {
    if (ch.tag != "leg" || ch.parts.size() < 2) continue;
    if (leadHip < 0) {
      leadHip = ch.parts[0];
      leadKnee = ch.parts[1];
      leadAnkle = ch.effector;
    } else if (trailAnkle < 0) {
      trailAnkle = ch.effector;
    }
  }
  if (shoulderR < 0 || wristR < 0 || wristL < 0 || leadKnee < 0 ||
      trailAnkle < 0) {
    detail = "the avatar def has no arm/leg parts by the expected names";
    std::printf("ledge-climb-pose: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // player-ledgegrab's plateau: launch floor to x<140 (top y=100), wall from
  // x=150 up to y=115, so the lip cell is y=114 and the wall faces -X.
  auto plateauKind = [](IVec3 q) {
    if (q.y < 60) return CellKind::Solid;
    if (q.x < 140) return q.y < 100 ? CellKind::Solid : CellKind::Air;
    if (q.x >= 150) return q.y < 115 ? CellKind::Solid : CellKind::Air;
    return CellKind::Air;
  };
  const Vec3 fwd{1, 0, 0}, right{0, 0, 1};
  const float heading = 1.5707963f;  // facing +X (heading 0 = +Z)
  const float dt = kTickDt;

  PlayerAvatar av;
  av.Init(&c.phys, &c.world, &c.debris, c.mats, &c.mobs);
  av.SetDefs(&c.mobs.Defs(), def.name);
  Player p;
  p.fly = false;
  p.pos = Vec3{132.0f, 100.0f + Player::kHalfY, 200.5f};
  if (!av.Spawn(p, heading)) {
    detail = "PlayerAvatar::Spawn refused the fixture";
    std::printf("ledge-climb-pose: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // ---- run up, jump, catch: player only, no pose yet ----------------------
  int guard = 0;
  {
    TickInput in;
    in.SetHeld(TB_SPRINT, true);
    in.forward = 1.0f;
    while (!(p.grounded && p.pos.x > 135.0f) && ++guard < 600)
      p.Update(dt, in, fwd, right, fwd, plateauKind);
    in.SetPressed(TB_JUMP, true);
    in.SetHeld(TB_JUMP, true);
    p.Update(dt, in, fwd, right, fwd, plateauKind);
    in.SetPressed(TB_JUMP, false);
    guard = 0;
    while (!p.hanging && ++guard < 600)
      p.Update(dt, in, fwd, right, fwd, plateauKind);
  }
  if (!p.hanging) {
    av.Despawn();
    detail = "the fixture jump never caught the lip";
    std::printf("ledge-climb-pose: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }

  // ---- dangle, then climb, posing every tick --------------------------------
  const float lipTop = (float)(p.hangLip.y + 1);
  const float face = (float)p.hangLip.x;  // the wall faces -X: its face is x
  struct Sample {
    float h, feetY;
    Vec3 wristR, wristL, shoulderR, knee, leadAnk, trailAnk;
  };
  std::vector<Sample> climb;
  bool nan = false, climbed = false;
  uint32_t tick = 52000;
  auto toWorld = [&](Vec3 m) {
    const Vec3 pivot{def.worldSize.x * 0.5f, 0, def.worldSize.z * 0.5f};
    const Vec3 o = av.Origin();
    return Vec3{o.x, av.BodyY(), o.z} + pivot +
           QuatRotate(QuatAxisAngle({0, 1, 0}, heading), m - pivot);
  };
  for (int f = 0; f < 60 * 8; f++, tick++) {
    TickInput in;
    in.SetHeld(TB_JUMP, true);
    in.forward = f >= 30 ? 1.0f : 0.0f;  // half a second of dead hang first
    p.Update(dt, in, fwd, right, fwd, plateauKind);

    PoseInputs pin;
    pin.velocity = p.vel;
    const bool climbing = p.mantleFromHang && p.mantleTimer > 0.0f;
    pin.grounded = p.grounded;
    pin.airPoseEligible = false;
    pin.height = PoseInputs::Height::FromDriver;
    pin.tiltFromGround = false;
    pin.hangActive = p.hanging;
    pin.hangLip = p.hangLip;
    pin.hangDir = p.hangDir;
    pin.climbActive = climbing;
    pin.climbRise = climbing ? p.LedgeClimbRise() : 0.0f;
    av.SetDriverPlacement(Vec3{p.pos.x - def.worldSize.x * 0.5f,
                               p.pos.y - Player::kHalfY,
                               p.pos.z - def.worldSize.z * 0.5f},
                          heading);
    av.PosePipeline(pin, dt, c.world, tick);

    const auto& m = av.ModelPose();
    for (const Transform& t : m)
      if (!std::isfinite(t.pos.x + t.pos.y + t.pos.z + t.rot.w)) nan = true;
    if (climbing) {
      Sample s;
      s.h = pin.climbRise;
      s.feetY = p.pos.y - Player::kHalfY;
      s.wristR = toWorld(m[wristR].pos);
      s.wristL = toWorld(m[wristL].pos);
      s.shoulderR = toWorld(m[shoulderR].pos);
      s.knee = toWorld(m[leadKnee].pos);
      s.leadAnk = toWorld(m[leadAnkle].pos);
      s.trailAnk = toWorld(m[trailAnkle].pos);
      climb.push_back(s);
    } else if (!climb.empty()) {
      climbed = p.grounded;
      break;
    }
  }
  av.Despawn();

  // ---- the numbers ---------------------------------------------------------
  // Rows relative to the lip: y above its top, x past its face.
  std::printf("ledge-climb-pose: %zu climb ticks, lip top y=%.0f face x=%.0f\n",
              climb.size(), lipTop, face);
  std::printf("  tick     h  feet | wristR y/x  | shldR y  | knee y/x    | "
              "leadAnk y/x | trailAnk y/x\n");
  for (size_t i = 0; i < climb.size(); i += 3) {
    const Sample& s = climb[i];
    std::printf("  %4zu %5.3f %5.1f | %5.1f %5.1f | %6.1f   | %5.1f %5.1f | "
                "%5.1f %5.1f | %5.1f %5.1f\n",
                i, s.h, s.feetY - lipTop, s.wristR.y - lipTop, s.wristR.x - face,
                s.shoulderR.y - lipTop, s.knee.y - lipTop, s.knee.x - face,
                s.leadAnk.y - lipTop, s.leadAnk.x - face, s.trailAnk.y - lipTop,
                s.trailAnk.x - face);
  }

  bool ok = !nan && climbed && climb.size() > 10;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("ledge-climb-pose: FAILED %s\n", what);
    }
  };
  check(!nan, "the pose stayed finite");
  check(climbed, "the climb finished standing on the lip");

  // The beats: stretches of >= 5 ticks rising under a quarter of the peak,
  // between the pull starting and the stand finishing.
  float peak = 0.0f;
  for (size_t i = 1; i < climb.size(); i++)
    peak = std::max(peak, climb[i].feetY - climb[i - 1].feetY);
  int beats = 0, run = 0;
  for (size_t i = 1; i < climb.size(); i++) {
    const float v = climb[i].feetY - climb[i - 1].feetY;
    const bool mid = climb[i].h > 0.05f && climb[i].h < 0.95f;
    if (mid && v < 0.25f * peak) {
      if (++run == 5) beats++;
    } else {
      run = 0;
    }
  }
  check(beats >= 2, "the rise stops/lags at least twice after the pull");

  // The tolerance is the dead hang's own: the hang IK leaves the WRIST ~3 vox
  // over the lip top (the palm socket is below it), and the climb must not
  // pull the hands further off than that.
  // Hands on the lip through the pull; knee on it while kneeling; feet on it
  // and hands down at the end.
  float worstHand = 0.0f, worstKnee = 1e9f, kneeAlong = 1e9f;
  for (const Sample& s : climb) {
    if (s.h <= ledgeclimb::kHChestOver)
      worstHand = std::max({worstHand, std::fabs(s.wristR.y - lipTop),
                            std::fabs(s.wristL.y - lipTop)});
    if (s.h >= ledgeclimb::kHKneeOn + 0.02f && s.h <= ledgeclimb::kHKneel) {
      worstKnee = std::min(worstKnee, s.knee.y - lipTop);
      kneeAlong = std::min(kneeAlong, s.knee.x - face);
    }
  }
  const float handTol = (float)BaselineNumber("ledgeClimbPose.handTolVox", 4.0);
  check(worstHand <= handTol, "the hands stay on the lip through the pull");
  check(worstKnee > -1.0f && worstKnee < 2.5f && kneeAlong > 0.0f,
        "the lead knee is on the lip while the weight is on it");
  if (!climb.empty()) {
    const Sample& e = climb.back();
    check(std::fabs(e.leadAnk.y - lipTop) < 2.0f &&
              std::fabs(e.trailAnk.y - lipTop) < 2.0f,
          "both feet are on the lip standing");
    check(e.wristR.y < e.shoulderR.y - 2.0f, "the arms are down at the end");
  }
  detail = Format("%zu ticks, %d beats, hands off lip <= %.1f vox, knee %.1f "
                  "above lip %.1f past face",
                  climb.size(), beats, worstHand, worstKnee, kneeAlong);
  std::printf("ledge-climb-pose: %s (%d checks, %s)\n", ok ? "PASS" : "FAIL",
              checks, detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& PoseGates() {
  static const std::vector<Gate> g = {
      {"pose-parity", "mob", {}, false, GatePoseParity},
      {"ledge-climb-pose", "mob", {}, false, GateLedgeClimbPose},
  };
  return g;
}

}  // namespace selftest
