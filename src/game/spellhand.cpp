#include "game/spellhand.h"

#include <algorithm>
#include <cmath>

#include "game/anim.h"
#include "game/mob.h"
#include "phys/physics.h"

namespace {
constexpr float kArmTickDt = 1.0f / 30.0f;
}  // namespace

void DropCastArm(Mob& body, CastArmState& arm) {
  if (arm.hand >= 0) body.ClearStrikeEffector();
  arm = CastArmState{};
}

// The gesture's frame is rebuilt from the aim EVERY tick, so turning while the
// arm is out swings the whole gesture with the body. Only the start point is
// remembered, in that frame: the gesture begins where the arm actually was, so
// nothing snaps. After the last key the claim fades (exp, 10/s) and the
// animation underneath (the armed clip) takes the arm back.
bool CastArmPose(Mob& body, CastArmState& arm, const CastGestureRun& run,
                 const SpellGestureLib& lib, Vec3 aim, WeaponPose& out) {
  if (arm.hand < 0) return false;
  const Hand h = HandAt(arm.hand);
  const SpellGesture* g = lib.At(run.gesture);
  const int part = body.HandPart(h);
  // The gesture is over and gone, or the hand cannot be posed (no socket, a
  // ruined arm): hand the arm straight back.
  if (g == nullptr || g->keys.empty() || part < 0 || !body.HandUsable(h)) {
    DropCastArm(body, arm);
    return false;
  }
  body.SetStrikeHand(h);
  body.SetStrikeEffector(part, StrikeEffectorMode::Chain);
  Vec3 live;
  float reach = 0;
  if (!body.WeaponArmPose(live, reach) || reach <= 1e-3f) {
    DropCastArm(body, arm);
    return false;
  }
  const float side = HandSideSign(h);
  const Vec3 bodyFwd = QuatRotate(QuatAxisAngle({0, 1, 0}, body.Heading()), Vec3{0, 0, 1});
  const GestureBasis basis = MakeGestureBasis(g->frame, aim, bodyFwd, side);
  if (arm.fresh) {
    arm.from = basis.FromWorld(live) * (1.0f / reach);
    arm.fresh = false;
  }
  if (run.ticks <= g->endTicks) {
    arm.w = 1.0f;
  } else {
    arm.w *= std::exp(-kArmTickDt * 10.0f);
    if (arm.w < 0.02f) {
      DropCastArm(body, arm);
      return false;
    }
  }
  const GestureSample gs =
      SampleGesture(*g, GestureTicksToMs(std::min(run.ticks, g->endTicks)), arm.from);
  Vec3 target = basis.ToWorld(gs.hand) * reach;
  // Short of straight: the two-bone solve is ill-conditioned at full reach.
  const float l = target.len();
  if (l > reach * 0.98f) target = target * (reach * 0.98f / l);
  Vec3 pole = basis.ToWorld(gs.pole);
  pole = pole.len() > 1e-4f ? pole.normalized() : Vec3{0, -1, 0};
  constexpr float kDeg = 3.14159265f / 180.0f;
  out = WeaponPose{};
  out.hand = target;
  out.weight = arm.w;
  out.steerBlade = false;
  // THE ELBOW IS THE GESTURE'S. The rig's own elbow-back pole is antiparallel
  // to a hand straight out in front at shoulder height and flipped the elbow
  // from tick to tick (session.cpp PoseBenchHands says so at length), so every
  // key names where it bulges - down for a push, out to the side for the hand
  // up by the head, back for the arm swung past the hip - with the hinge cone
  // open so the clamp does not refuse the bend it asks for.
  out.usePole = true;
  out.bendPole = pole;
  out.elbowAxisCone = 3.14159265f;
  // The torso joins the throw: + turns the casting side's shoulder back, so
  // the left hand's twist is the mirror. Weight-scaled, as the melee driver's
  // are, so it fades with the claim.
  out.torsoTwist = gs.twistDeg * kDeg * side * arm.w;
  out.torsoPitch = gs.pitchDeg * kDeg * arm.w;
  return true;
}

void SpellReadyClip(Mob& body, Hand h, bool armed, bool& clipOn) {
  const std::string clip = body.HandClip("spell_ready", h);
  if (clip.empty()) {
    clipOn = armed;
    return;
  }
  const bool lost = armed && clipOn && body.ClipWeight(clip) <= 0.0f;
  if (armed != clipOn || lost) {
    if (armed) body.PlayClip(clip);
    else body.StopClip(clip);
  }
  clipOn = armed;
}

bool SpellHandWorld(const MobSystem& mobs, const Physics& phys, uint64_t mobId, Hand h,
                    Vec3& out) {
  const Mob* m = mobs.FindCreature(mobId);
  if (m == nullptr) return false;
  const int part = m->HandPart(h);
  if (part < 0) return false;
  BodyTransform xf;
  const uint64_t body = mobs.LimbBody(mobId, part);
  if (body == 0 || !phys.GetTransform(body, xf)) return false;
  out = xf.pos;
  return true;
}
