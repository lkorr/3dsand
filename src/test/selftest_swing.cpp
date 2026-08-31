// selftest_swing.cpp — the mouse-directed swing's CONTROL LAW (game/melee.h).
//
// CPU-ONLY, ASSET-FREE, MILLISECONDS. MeleeState is a small float state machine
// with no world, no GPU and no content behind it, so the gate that covers it can
// be run on its own as the whole verification loop for a feel change:
//
//     ./build/Release/sandvox.exe --selftest --gate swing
//
// WHAT IT IS FOR. The swing's input mapping has been rewritten once already,
// and the rewrite was forced by a bug that no gate could have caught because
// nothing measured the mapping at all: mouse VELOCITY drove a lean off a fixed
// guard pose, so clicking teleported the arm to that pose and the blade then
// sat wherever the *current* mouse speed put it — saturating its clamp on any
// real flick, and falling back the instant the mouse stopped. The player's
// report was "the moment I click, the arm shoots to the top right".
//
// The four properties below are exactly the four ways that failed, phrased so
// that the old law fails each of them and the new one passes:
//
//   1. TAKE-OVER IS NOT A TELEPORT. The first driven tick asks for the pose the
//      arm is ALREADY in. Checked against a CONTROL (the same click with no arm
//      reported) so it cannot pass by the seed and the fallback coinciding.
//   2. THE MOUSE IS A DISPLACEMENT. Travel is proportional to pixels MOVED and
//      independent of how fast they were delivered — 300 px in one tick and
//      300 px spread over thirty land in the same place.
//   3. THE BLADE STAYS PUT. With the mouse still, the hand does not drift back
//      toward anything. This is the one the velocity law could never satisfy.
//   4. THE CLAMP BANKS NOTHING. Pushing past the arm's reach must not store
//      travel that has to be wound back before the arm moves again: one tick of
//      inward mouse after a long outward push moves the hand inward by a full
//      tick's worth.
//
// Reach and the seed both come from the RIG in the live game (Mob::
// WeaponArmPose), so the fixture here feeds SetArm directly — that keeps the
// gate about the control law and not about whatever human.json's arm is this
// week.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/melee.h"
#include "test/selftest.h"

namespace selftest {
namespace {

// The camera basis the game hands in: +X right, +Y up, +Z forward. Deliberately
// axis-aligned so a failure prints a number that can be read by eye.
const Vec3 kRight{1, 0, 0}, kUp{0, 1, 0}, kFwd{0, 0, 1};

// A shoulder-relative hand pose that is nothing like the guard fallback: down
// and slightly back, which is roughly where a walk cycle leaves an arm. If this
// ever coincided with `guard*` the take-over check would be vacuous, so the
// gate asserts they differ before relying on it.
const Vec3 kRestHand{0.4f, -3.0f, -0.3f};
const float kRestReach = 6.0f;

const float kDt = 1.0f / 60.0f;

// A per-tick mouse delta that is FAST for the pose and SLOW for the commit
// test: the mapping checks want as much travel as they can get, and every one
// of them is invalidated if a cut fires in the middle. 10 px/tick is 600 px/s
// against a 900 px/s commit threshold.
const float kNoCommitPx = 10.0f;

// One tick with a given per-tick mouse delta, button held.
void Step(MeleeState& m, float dx, float dy, bool held = true) {
  m.AddMouse(dx, dy);
  m.Update(kDt, held, true, kRight, kUp, kFwd);
}

MeleeState MakeSeeded() {
  MeleeState m;
  m.SetArm(kRestHand, kRestReach);
  return m;
}

float Dist(const Vec3& a, const Vec3& b) { return (a - b).len(); }

Status GateSwing(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("swing: FAILED %s\n", what);
    }
  };

  const MeleeTuning t;   // defaults; the gate asserts on shape, not on values

  // ---- 1. take-over is not a teleport --------------------------------------
  {
    const Vec3 guard =
        kFwd * t.guardForward + kUp * t.guardUp + kRight * t.guardSide;
    check(Dist(guard, kRestHand) > 1.0f,
          "the fixture's rest hand differs from the guard fallback (else the "
          "take-over check below proves nothing)");

    MeleeState m = MakeSeeded();
    Step(m, 0, 0);   // the click, with a still mouse
    check(m.Phase() == SwingPhase::Guard, "one held tick leaves Idle");
    check(Dist(m.HandOffset(), kRestHand) < 0.01f,
          "the first driven tick asks for the arm's CURRENT pose");
    check(m.PoseWeight() > 0.99f,
          "the arm is claimed in full on the take-over tick (it can be, "
          "because nothing moved)");

    // THE CONTROL. Same click, no arm reported: the hand starts at the guard
    // fallback instead. Without this, check 2 above would also pass on a build
    // that ignored SetArm and happened to guard where the fixture rests.
    MeleeState blind;
    blind.ClearArm();
    blind.AddMouse(0, 0);
    blind.Update(kDt, true, true, kRight, kUp, kFwd);
    check(Dist(blind.HandOffset(), guard) < 0.01f,
          "with no arm reported the seed is the guard fallback");
    check(Dist(blind.HandOffset(), m.HandOffset()) > 1.0f,
          "the seeded and unseeded take-overs land in DIFFERENT places");
  }

  // ---- 2. the mouse is a displacement, not a rate ---------------------------
  {
    // The SAME total travel, delivered in one tick and in nine. A velocity law
    // puts these in wildly different places (and the fast one on its clamp); an
    // integrator puts them in the same place.
    //
    // The total is kept well inside the arm's reach on purpose: the reach clamp
    // is path-dependent (nine small steps slide along the sphere where one big
    // step projects radially onto it), so a comparison that saturated would be
    // measuring the clamp rather than the mapping. Property 4 below is where
    // the clamp is measured, deliberately on its own.
    // 600 px/s against 300 px/s, both under the commit threshold — a genuine
    // rate difference that is still a guard rather than a cut. (A single-tick
    // delivery of the same 90 px would be 5400 px/s, which SHOULD commit, and
    // then the comparison would be against a swing arc.)
    const float kPx = 9.0f * kNoCommitPx;

    MeleeState fast = MakeSeeded();
    Step(fast, 0, 0);
    for (int i = 0; i < 9; i++) Step(fast, kNoCommitPx, 0);

    MeleeState slow = MakeSeeded();
    Step(slow, 0, 0);
    for (int i = 0; i < 18; i++) Step(slow, kNoCommitPx * 0.5f, 0);

    check(Dist(fast.HandOffset(), slow.HandOffset()) < 0.01f,
          "the same pixels land in the same place fast or slow");
    check(fast.Phase() != SwingPhase::Slash &&
              slow.Phase() != SwingPhase::Slash,
          "neither arm of the rate test committed a cut (which would make it "
          "measure the arc instead of the mapping)");

    // ...and the place is the seed plus the mapped travel: +dx is screen-right
    // and screen +y is DOWN, so -dy must be up.
    const Vec3 wantRight = kRestHand + kRight * (kPx * t.moveGainX);
    check(Dist(slow.HandOffset(), wantRight) < 0.01f,
          "rightward mouse moves the hand right, by pixels x gain");

    MeleeState upward = MakeSeeded();
    Step(upward, 0, 0);
    for (int i = 0; i < 9; i++) Step(upward, 0, -kNoCommitPx);
    const Vec3 wantUp = kRestHand + kUp * (kPx * t.moveGainY);
    check(Dist(upward.HandOffset(), wantUp) < 0.01f,
          "screen-up mouse (negative dy) raises the hand");
    check(upward.HandOffset().y > kRestHand.y + 0.5f,
          "raising the blade does not need the whole mousepad");
  }

  // ---- 3. the blade stays where it was put ---------------------------------
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    for (int i = 0; i < 20; i++) Step(m, 6.0f, -6.0f);
    const Vec3 parked = m.HandOffset();
    check(Dist(parked, kRestHand) > 0.5f, "the push actually moved the hand");
    // Two full seconds of a perfectly still mouse.
    for (int i = 0; i < 120; i++) Step(m, 0, 0);
    check(Dist(m.HandOffset(), parked) < 0.01f,
          "a still mouse leaves the hand exactly where it was put");
    check(m.Phase() == SwingPhase::Guard,
          "a still mouse does not commit a cut");
  }

  // ---- 4. the reach clamp banks nothing ------------------------------------
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    // Push outward for long enough that an unclamped integrator would be metres
    // away — at a speed that never commits, so what is measured is the hand and
    // not the arc a cut would add on top of it.
    for (int i = 0; i < 400; i++) Step(m, kNoCommitPx, 0);
    check(m.Phase() != SwingPhase::Slash, "the sustained push did not commit");
    const float reach = kRestReach * t.reachFraction;
    const float d = m.HandOffset().len();
    check(d <= reach + 0.01f, "the hand never leaves the arm's reach");
    check(d > reach - 0.01f, "a sustained push holds the hand ON the limit");

    // The banking test: one tick of INWARD mouse must move the hand by a full
    // tick's travel. If the clamp had let handCam_ grow unbounded, this tick
    // would move it by nothing at all.
    const Vec3 before = m.HandOffset();
    Step(m, -kNoCommitPx, 0);
    const float moved = Dist(m.HandOffset(), before);
    const float want = kNoCommitPx * t.moveGainX;
    check(moved > want * 0.5f,
          "one inward tick after a long outward push moves the hand at once");
  }

  // ---- 5. releasing hands the arm back, a combination does not -------------
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    for (int i = 0; i < 10; i++) Step(m, 5.0f, 0);
    const Vec3 held = m.HandOffset();
    // Release: the weight fades over the recover rather than being dropped, and
    // the hand does NOT spring back to a pose while it fades.
    m.Update(kDt, false, true, kRight, kUp, kFwd);
    check(m.Phase() == SwingPhase::Recover, "releasing enters recover");
    check(Dist(m.HandOffset(), held) < 0.01f,
          "the hand does not spring to a guard pose on release");
    // A tick or two in, not on the entry tick: the fade is a ramp over
    // recoverTime and it is still 1.0 at t = 0 by construction.
    m.Update(kDt, false, true, kRight, kUp, kFwd);
    m.Update(kDt, false, true, kRight, kUp, kFwd);
    check(m.PoseWeight() < 1.0f, "the releasing recover starts handing back");
    float w = m.PoseWeight();
    for (int i = 0; i < 60; i++) {
      m.Update(kDt, false, true, kRight, kUp, kFwd);
      const float now = m.PoseWeight();
      if (now > w + 1e-4f) {
        check(false, "the hand-back weight only ever falls");
        break;
      }
      w = now;
    }
    check(m.Phase() == SwingPhase::Idle, "the recover ends at Idle");
    check(m.PoseWeight() <= 0.001f, "the arm is fully handed back at Idle");
  }

  // ---- 6. a committed cut still fires, and still ends where the mouse did ---
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    // Fast enough to clear commitSpeed, in a direction with both axes so the
    // "a diagonal flick is a diagonal cut" property is what is measured.
    for (int i = 0; i < 6; i++) Step(m, 40.0f, -40.0f);
    check(m.Phase() == SwingPhase::Slash, "a fast flick commits a cut");
    const Vec3 cut = m.CutDir();
    check(cut.x > 0.3f && cut.y > 0.3f,
          "the cut goes the way the mouse went (right and up)");
    // Let the slash and its follow-through run out with the mouse now still.
    for (int i = 0; i < 40; i++) Step(m, 0, 0);
    check(m.Phase() == SwingPhase::Guard,
          "the cut returns to guard while the button is held");
    // The follow-through unwinds ONTO the steered position: up and to the
    // right of where the hand started, not back at the seed.
    check(m.HandOffset().y > kRestHand.y + 0.2f &&
              m.HandOffset().x > kRestHand.x + 0.2f,
          "the cut ends where the mouse took it, not back at the start");
  }

  detail = Format("%d checks", checks);
  std::printf("swing: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SwingGates() {
  static const std::vector<Gate> g = {
      // No deps and no world: it builds its own MeleeState fixtures, so it can
      // neither disturb pristine worldgen nor be disturbed by anything.
      {"swing", "player", {}, false, GateSwing},
  };
  return g;
}

}  // namespace selftest
