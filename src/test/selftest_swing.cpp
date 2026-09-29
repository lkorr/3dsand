// selftest_swing.cpp — the mouse-directed swing's CONTROL LAW (game/melee.h).
//
// CPU-ONLY, ASSET-FREE, MILLISECONDS. MeleeState is a small float state machine
// with no world, no GPU and no content behind it, so the gate that covers it can
// be run on its own as the whole verification loop for a feel change:
//
//     ./build/Release/sandvox.exe --selftest --gate swing
//
// Its sibling `swing-plane` (selftest_mob.cpp) drives the SAME driver through
// the real rig, the real IK and the real blade and measures the sword's world
// trajectory. The split is deliberate and is the reason this one is cheap: the
// mapping is asserted here, in seconds; the pipeline is asserted there, once.
//
// WHAT IT IS FOR. The swing's input mapping has been rewritten twice, and both
// rewrites were forced by bugs no gate could have caught because nothing
// measured the mapping at all:
//
//   * mouse VELOCITY drove a lean off a fixed guard pose, so clicking
//     teleported the arm and the blade then sat wherever the *current* mouse
//     speed put it. "The moment I click, the arm shoots to the top right."
//   * mouse pixels then drove a HAND POSITION, which is three numbers into a
//     two-bone solver with a fixed pole and a hinge elbow — so forward mouse
//     came out as an elbow jab and the sweep plane was an accident.
//     "Moving the mouse forwards and backwards just jabs the hand forward."
//
// The properties below are the ways those failed, phrased so that the old laws
// fail each of them and the current one passes. Blocks 1-6 predate the tip
// rewrite and are unchanged in INTENT; where the arithmetic legitimately moved
// (a mapping that used to be linear in voxels is now linear in radians) the
// check moved with it and says so. Blocks 7-10 are what the tip law adds.
//
//   1. TAKE-OVER IS NOT A TELEPORT. The first driven tick asks for the pose the
//      blade is ALREADY in. Checked against a CONTROL (the same click with no
//      arm reported) so it cannot pass by the seed and the fallback coinciding.
//   2. THE MOUSE IS A DISPLACEMENT. Travel is proportional to pixels MOVED and
//      independent of how fast they were delivered.
//   3. THE BLADE STAYS PUT. With the mouse still, nothing drifts back.
//   4. THE CLAMP BANKS NOTHING. Pushing past a stop must not store travel that
//      has to be wound back before the blade moves again.
//   5. RELEASING HANDS THE ARM BACK; a combination does not.
//   6. A COMMITTED CUT STILL FIRES, and still ends where the mouse did.
//   7. RIGHT-TO-LEFT IS A HORIZONTAL ARC IN FRONT OF THE CHARACTER. The owner's
//      acceptance criterion, stated on the tip.
//   8. FORWARD/BACK RAISES AND LOWERS THE POINT — it does not thrust. This is
//      the recorded feel bug, as an assertion.
//   9. THE EDGE LEADS THE TRAVEL, and the elbow trails it.
//  10. THE DRIVER IS NOT PLAYER-SPECIFIC: an authored StrokeScript replayed
//      through Step() lands exactly where the same deltas do through Feed().
//
// Reach and the seed both come from the RIG in the live game (Mob::
// WeaponStrokePose), so the fixture here feeds SetStroke directly — that keeps
// the gate about the control law and not about whatever human.json's arm is
// this week.

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/melee.h"
#include "game/player.h"
#include "game/strike_pick.h"
#include "game/strokes.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// The camera basis the game hands in, at heading 0 (facing +Z): right is
// Camera::Right() = Forward x up = -X. THE SIGN IS THE CAMERA'S CHIRALITY and
// it is load-bearing (2026-09-01): the renderer maps exactly this vector to
// screen-right, the rigs author their .R limbs on the model side it names,
// and the world->rig conversion is a pure un-yaw — so a gate that hands the
// mirror basis (+X) drives every stroke to the wrong side of the body and
// measures the authored ACROSS stops where the game enjoys the weapon-side
// window. Axis-aligned still, so failures print readable numbers; world
// azimuths below are read with atan2(-x, z) to stay in this basis.
const Vec3 kRight{-1, 0, 0}, kUp{0, 1, 0}, kFwd{0, 0, 1};

// A shoulder-relative hand pose that is nothing like the guard fallback: down
// and slightly back, which is roughly where a walk cycle leaves an arm. If this
// ever coincided with `guard*` the take-over check would be vacuous, so the
// gate asserts they differ before relying on it.
const Vec3 kRestHand{-0.4f, -3.0f, -0.3f};   // x mirrored with the basis
const float kRestReach = 6.0f;

// A second fixture, out in front and level, for the arc blocks. The hanging
// rest pose above sits at ~127 degrees of azimuth, which is most of the way to
// the weapon-side stop — an arc measured from there would be measuring the
// clamp. Nothing about the mapping depends on which of the two is used.
const Vec3 kFrontHand{-1.0f, 0.0f, 3.0f};    // x mirrored with the basis

const float kDt = 1.0f / 60.0f;

// A per-tick mouse delta that is FAST for the pose and SLOW for the commit
// test: the mapping checks want as much travel as they can get, and every one
// of them is invalidated if a cut fires in the middle. 10 px/tick is 600 px/s
// against a 900 px/s commit threshold.
const float kNoCommitPx = 10.0f;

// One tick with a given per-tick mouse delta, button held.
void Step(MeleeState& m, float dx, float dy, bool held = true) {
  m.Feed(dx, dy);
  m.Update(kDt, held, true, kRight, kUp, kFwd);
}

// THE GATE'S SUBJECT IS THE CONTROL LAW, so the presentation smoothing is OFF
// in every fixture (2026-09-01, when `melee.armSmoothing` landed). With it on,
// "the hand is exactly where the integral says" is false by design — the eased
// stroke is still converging on the integral whenever the mouse just moved —
// and two arms given the same pixels over different tick counts differ by
// their settling, which is lag, not a mapping bug. At 0 the eased copy equals
// the integral every tick, bit for bit, and every assertion below means what
// it says. The knob's reachability is combat-tuning's job, not this gate's.
void SmoothingOff(MeleeState& m) {
  m.tuning.armSmoothing = 0.0f;
  m.tuning.wristSmoothing = 0.0f;
}

MeleeState MakeSeeded() {
  MeleeState m;
  SmoothingOff(m);
  m.SetArm(kRestHand, kRestReach);
  return m;
}

MeleeState MakeFront() {
  MeleeState m;
  SmoothingOff(m);
  m.SetArm(kFrontHand, kRestReach);
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
    // that ignored SetStroke and happened to guard where the fixture rests.
    MeleeState blind;
    SmoothingOff(blind);
    blind.ClearArm();
    blind.Feed(0, 0);
    blind.Update(kDt, true, true, kRight, kUp, kFwd);
    check(Dist(blind.HandOffset(), guard) < 0.01f,
          "with no arm reported the seed is the guard fallback");
    check(Dist(blind.HandOffset(), m.HandOffset()) > 1.0f,
          "the seeded and unseeded take-overs land in DIFFERENT places");

    // ...AND THE BLADE SEEDS TOO. The driver steers the TIP, so a take-over
    // that only knew where the HAND was would have to guess the blade's
    // orientation, and a guess is a visible pop at the instant of the click.
    // Fed a real hand/tip pair, BOTH ends must be exactly where they were.
    const Vec3 tip = kRestHand + Vec3{-0.6f, 0.2f, 2.4f};  // a blade, at an angle
    MeleeState bladed;
    SmoothingOff(bladed);
    bladed.SetStroke(kRestHand, tip, Vec3{0, 1, 0}, kRestReach);
    bladed.Feed(0, 0);
    bladed.Update(kDt, true, true, kRight, kUp, kFwd);
    check(Dist(bladed.TipOffset(), tip) < 0.01f,
          "the take-over tick asks for the POINT the blade is already at");
    check(Dist(bladed.HandOffset(), kRestHand) < 0.01f,
          "...and for the hand it is already at, with the blade between them");
    check(std::fabs(bladed.BladeDir().dot((tip - kRestHand).normalized()) -
                    1.0f) < 0.01f,
          "the seeded blade direction is the blade's own, not a guess");
  }

  // ---- 2. the mouse is a displacement, not a rate ---------------------------
  {
    // The SAME total travel, delivered in one tick and in nine. A velocity law
    // puts these in wildly different places (and the fast one on its clamp); an
    // integrator puts them in the same place.
    //
    // MEASURED ON THE STROKE STATE, which is where "a displacement" is now the
    // literal truth: az/el are the pure integral of the input and the tip is a
    // pure function of them. The hand is checked too, and it is exact here
    // because the fixture reports no blade — with a blade in the fist the hand
    // is the tip minus a SMOOTHED direction, which converges rather than
    // matching to the bit, and block 9 is where that is measured instead.
    //
    // ON THE FRONT FIXTURE, not the hanging one: a rest arm sits at ~127
    // degrees of azimuth, which is within half a radian of the weapon-side
    // stop, so 90 px of rightward mouse from there measures the CLAMP and not
    // the gain. Block 4 is where the clamp is measured, deliberately alone.
    const float kPx = 9.0f * kNoCommitPx;

    MeleeState fast = MakeFront();
    Step(fast, 0, 0);
    const float az0 = fast.StrokeAz(), el0 = fast.StrokeEl();
    for (int i = 0; i < 9; i++) Step(fast, kNoCommitPx, 0);

    MeleeState slow = MakeFront();
    Step(slow, 0, 0);
    for (int i = 0; i < 18; i++) Step(slow, kNoCommitPx * 0.5f, 0);

    check(std::fabs(fast.StrokeAz() - slow.StrokeAz()) < 1e-4f,
          "the same pixels reach the same azimuth fast or slow");
    check(Dist(fast.HandOffset(), slow.HandOffset()) < 0.01f,
          "...and therefore the same place");

    // ...and the azimuth is the seed plus pixels x gain. Radians now, not
    // voxels: the point rides a sphere about the shoulder, so a sideways drag
    // is an ARC and not a slide across a plane.
    check(std::fabs(slow.StrokeAz() - (az0 + kPx * t.aimGainX)) < 1e-4f,
          "rightward mouse turns the stroke right, by pixels x gain");
    check(slow.TipOffset().x < kFrontHand.x - 0.5f,
          "and rightward really is to the RIGHT in world terms (which is "
          "world -x: the render maps camRight = Forward x up to screen "
          "right)");

    MeleeState upward = MakeFront();
    Step(upward, 0, 0);
    for (int i = 0; i < 9; i++) Step(upward, 0, -kNoCommitPx);
    check(std::fabs(upward.StrokeEl() - (el0 + kPx * t.aimGainY)) < 1e-4f,
          "screen-up mouse (negative dy) raises the stroke's elevation");
    check(upward.HandOffset().y > kFrontHand.y + 0.5f,
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

  // ---- 4. the clamp banks nothing ------------------------------------------
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    // Push outward for long enough that an unclamped integrator would be
    // several turns round — at a speed that never commits, so what is measured
    // is the stroke and not the arc a cut would add on top of it.
    for (int i = 0; i < 400; i++) Step(m, kNoCommitPx, 0);
    check(m.StrokeAz() <= t.azOut + 1e-3f,
          "the stroke never leaves the weapon-side azimuth stop");
    check(m.StrokeAz() > t.azOut - 1e-3f,
          "a sustained push holds the stroke ON the limit");

    // The banking test: one tick of INWARD mouse must move the stroke by a full
    // tick's travel. If the clamp had let az_ grow unbounded, this tick would
    // move it by nothing at all.
    const float before = m.StrokeAz();
    Step(m, -kNoCommitPx, 0);
    const float moved = before - m.StrokeAz();
    const float want = kNoCommitPx * t.aimGainX;
    check(moved > want * 0.5f,
          "one inward tick after a long outward push moves the blade at once");
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
    check(Dist(m.HandOffset(), held) < 0.05f,
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

  // ---- 6. (removed 2026-09-25) the driver's own speed-triggered cut --------
  // MeleeState no longer commits a Slash of its own: every stroke is an
  // authored program and the program's Cut phase is the cut (melee.h
  // SwingPhase). A fast input is now just a fast move, which block 2 covers.

  // ---- 7. right-to-left is a horizontal arc IN FRONT ------------------------
  //
  // THE ACCEPTANCE CRITERION, stated on the tip: "smooth mouse movement to the
  // top right should move the sword to the top right relative to the
  // character... moving mouse right to left will swing the sword right to left
  // horizontally". Ready to the right, then flick left, and watch the point.
  {
    MeleeState m = MakeFront();
    Step(m, 0, 0);
    const float az0 = m.StrokeAz(), el0 = m.StrokeEl();
    // Ready: 20 slow ticks right. 600 px/s is under the 900 px/s commit, so no
    // cut fires — but it IS over the wind threshold, and Wind is a guard that
    // has noticed the mouse moving, not a committed stroke.
    for (int i = 0; i < 20; i++) Step(m, kNoCommitPx, 0);
    const float azReady = m.StrokeAz();
    check(azReady > az0 + 0.5f, "the ready really did load the blade right");

    // The flick, and every tip on the way through it.
    float azMin = 1e9f, azMax = -1e9f, elDev = 0, minFwd = 1e9f;
    float radMin = 1e9f, radMax = -1e9f;
    float prevAz = azReady;
    int backSteps = 0;
    for (int i = 0; i < 24; i++) {
      Step(m, -25.0f, 0);
      const Vec3 tip = m.TipOffset();
      const float az = std::atan2(-tip.x, tip.z);   // basis az (right=-X)
      const float el = std::asin(std::clamp(tip.y / std::max(tip.len(), 1e-4f),
                                            -1.0f, 1.0f));
      azMin = std::min(azMin, az);
      azMax = std::max(azMax, az);
      elDev = std::max(elDev, std::fabs(el - el0));
      minFwd = std::min(minFwd, tip.z);
      radMin = std::min(radMin, tip.len());
      radMax = std::max(radMax, tip.len());
      // Monotone leftward, once the anticipation's brief pull-back is spent.
      if (az > prevAz + 1e-4f) backSteps++;
      prevAz = az;
    }
    check(azMax - azMin > 1.4f,
          "the point sweeps most of a right-to-left arc (>80 degrees)");
    check(elDev < 0.35f,
          "a horizontal flick stays horizontal (<20 degrees of elevation)");
    check(minFwd > 0.0f,
          "the whole arc passes IN FRONT of the chest, never behind it");
    check(radMax - radMin < radMax * 0.4f,
          "the point rides an ARC about the shoulder, not a straight slide");
    check(backSteps <= 4,
          "the sweep runs one way: at most the anticipation's own pull-back "
          "goes against the cut");
  }

  // ---- 8. forward/back raises the point; it does not thrust -----------------
  //
  // THE RECORDED FEEL BUG, as an assertion (notes/to do.md): "moving mouse
  // forwards and backwards just jabs hand forward; seems to just extend/control
  // the elbow". Under a hand-position law that is what it did. Under a tip law
  // the vertical axis is pure elevation and the reach is a separate channel, so
  // pushing the mouse forward must RAISE the point and must not extend it.
  {
    MeleeState m = MakeFront();
    Step(m, 0, 0);
    const float r0 = m.StrokeRadius();
    const Vec3 tip0 = m.TipOffset();
    for (int i = 0; i < 20; i++) Step(m, 0, -kNoCommitPx);   // mouse forward
    check(m.TipOffset().y > tip0.y + 1.0f, "forward mouse RAISES the point");
    check(std::fabs(m.StrokeRadius() - r0) < 1e-4f,
          "...and does not extend it: the reach is untouched");
    check(std::fabs(m.StrokeAz() - std::atan2(-tip0.x, tip0.z)) < 1e-4f,
          "...nor does it swing the point sideways");

    // The radial channel exists, is bounded, and is the ONLY thing that moves
    // the reach. This is what an NPC lunge and a future thrust binding drive.
    const float rBefore = m.StrokeRadius();
    for (int i = 0; i < 400; i++) {
      m.FeedReach(kNoCommitPx);
      m.Update(kDt, true, true, kRight, kUp, kFwd);
    }
    check(m.StrokeRadius() > rBefore + 0.1f, "FeedReach thrusts the point out");
    check(m.StrokeRadius() <= kRestReach * t.reachFraction + 0.01f,
          "...and the thrust is bounded by the arm, not by the mousepad");
  }

  // ---- 9. the edge leads the travel, and the elbow trails it ---------------
  {
    // With a real blade in the fist this time, so the hand-from-tip derivation
    // is exercised as well as the frame.
    const Vec3 tip = kFrontHand + Vec3{0.0f, 0.0f, 2.5f};
    const float bladeLen = (tip - kFrontHand).len();
    MeleeState m;
    SmoothingOff(m);
    m.SetStroke(kFrontHand, tip, Vec3{0, 1, 0}, kRestReach);
    m.Feed(0, 0);
    m.Update(kDt, true, true, kRight, kUp, kFwd);
    Vec3 prevTip = m.TipOffset();
    // worstPole starts at -infinity, NOT at 0: the quantity it tracks is
    // supposed to be NEGATIVE, and a max() seeded at zero can only ever report
    // zero — a check that passes and fails for the same reason.
    float worstFlat = 0, worstPole = -1e9f, worstBlade = 0;
    float worstOffTrail = -1e9f, worstOffCone = 0, worstPinAim = 0;
    int samples = 0, offTrail = 0, pinnedFront = 0;
    for (int i = 0; i < 50; i++) {
      m.SetStroke(m.HandOffset(), m.TipOffset(), m.BladeFlat(), kRestReach);
      Step(m, -kNoCommitPx * 0.6f, 0);
      const Vec3 travel = m.TipOffset() - prevTip;
      prevTip = m.TipOffset();
      if (travel.len() < 1e-4f) continue;
      const Vec3 dir = travel.normalized();
      // Let the lean plane finish turning first. It is RATE limited (a wrist
      // does not snap), so a stroke that starts leaning one way takes a few
      // ticks to come round; measuring through that measures the ramp.
      if (i < 32) continue;
      samples++;
      worstFlat = std::max(worstFlat, std::fabs(m.BladeFlat().dot(dir)));
      // ---- THE POLE TRAILS THE TRAVEL, OR IT IS PINNED AT THE ANATOMY -----
      //
      // The pole is where the ELBOW bulges, and opposite the travel is what
      // puts the arm's bend plane IN the plane of the cut. It is no longer the
      // whole claim, and the reason is a real bound rather than a slipped
      // number: `MeleeTuning::elbowPoleCone` (2026-09-01) stops the elbow
      // coming FORWARD of the body, because "the elbow bends both ways" turned
      // out to be a fact about which side it bulges to and not about the hinge
      // (see game/mob.cpp). Late in a sweep across the body, "opposite the
      // travel" IS forward, and the two genuinely disagree.
      //
      // So the assertion is a disjunction, per tick: either the pole trails
      // the travel, or it is sitting on the cone RIM — the nearest legal
      // direction to trailing. What it may never be is somewhere else, which
      // is what an unbounded pole or a botched clamp would produce. Measured
      // with the bound off, this loop reports -0.99; with it on, -0.16 and
      // every off-trail tick pinned at the rim.
      const float opp = m.BendPole().dot(dir);
      worstPole = std::max(worstPole, opp);
      const float coneNow =
          std::acos(std::clamp(m.BendPole().dot(kFwd * -1.0f), -1.0f, 1.0f));
      const bool pinned = coneNow >= t.elbowPoleCone - 0.03f;
      if (opp >= -0.5f && !pinned) {
        offTrail++;
        worstOffTrail = std::max(worstOffTrail, opp);
        worstOffCone = coneNow;
      }
      // ...and the hand is still the tip minus a blade — EXCEPT on ticks the
      // front-plane clamp holds it (2026-09-01, MeleeTuning::handBackFrac):
      // there the hand is pinned at the plane, |tip - hand| is legitimately
      // short of a blade, and the contract is instead that the blade is still
      // AIMED at the commanded point from the pinned hand. Asserting exact
      // length through the clamp would be asserting the clamp off.
      {
        const Vec3 handNow = m.HandOffset();
        const float backLim =
            -m.tuning.handBackFrac * kRestReach * m.tuning.reachFraction;
        const Vec3 toTip = m.TipOffset() - handNow;
        if (handNow.z > backLim + 0.05f) {
          worstBlade =
              std::max(worstBlade, std::fabs(toTip.len() - bladeLen));
        } else if (toTip.len() > 1e-4f) {
          pinnedFront++;
          worstPinAim = std::max(
              worstPinAim, 1.0f - toTip.normalized().dot(m.BladeDir()));
        }
      }
    }
    std::printf(
        "swing block 9: worst |flat.travel| %.3f, worst pole.travel %.3f, "
        "worst blade-length drift %.3f vox (%d ticks pinned at the front "
        "plane, worst aim error there %.4f); %d ticks off-trail and not "
        "pinned at the elbow cone (worst %.3f at %.2f rad of %.2f)\n",
        worstFlat, worstPole, worstBlade, pinnedFront, worstPinAim, offTrail,
        offTrail ? worstOffTrail : 0.0f, offTrail ? worstOffCone : 0.0f,
        t.elbowPoleCone);
    // SAMPLES > 0 IS NOT PEDANTRY. `worstPole` is a max() over a quantity that
    // is supposed to be NEGATIVE, so a run that took no samples at all leaves it
    // at -infinity and every check below passes for the worst possible reason.
    // (It happened: a stroke long enough to settle the lean plane was also long
    // enough to run into the azimuth stop, and the last eighteen ticks moved
    // nothing.)
    check(samples > 8, "the settled part of the stroke was actually measured");
    check(worstFlat < 0.2f,
          "the blade's FLAT stays square to the travel, i.e. the edge leads");
    check(offTrail == 0,
          "the elbow trails the hand along the stroke (the bend plane IS the "
          "cut plane) - or is pinned at its anatomical cone, never elsewhere");
    check(worstBlade < 0.35f,
          "the hand stays exactly one blade behind the point");
    check(worstPinAim < 0.01f,
          "...and on front-plane-pinned ticks the blade is still aimed at "
          "the commanded point");
  }

  // ---- 10. the driver is not player-specific -------------------------------
  //
  // Phase C drives this with authored stroke curves instead of a mouse. The
  // whole NPC surface is Step(StrokeSample): if it did not land in the same
  // place as the player's Feed/Update pair, an authored attack would not be the
  // same motion a person can make, and every feel fix would need doing twice.
  {
    const std::vector<StrokeSample> script = {
        {0, 0, 0, true},      {12, -4, 0, true},   {14, -6, 0, true},
        {18, -2, 0, true},    {-26, 0, 0, true},   {-30, 2, 0, true},
        {-34, 4, 0, true},    {-30, 6, 0, true},   {-20, 4, 0, true},
        {0, 0, 3.0f, true},   {0, 0, 0, true},
    };
    MeleeState scripted = MakeFront();
    for (const StrokeSample& s : script)
      scripted.Step(s, kDt, true, kRight, kUp, kFwd);

    MeleeState manual = MakeFront();
    for (const StrokeSample& s : script) {
      manual.Feed(s.dx, s.dy);
      manual.FeedReach(s.dReach);
      manual.Update(kDt, s.held, true, kRight, kUp, kFwd);
    }
    check(Dist(scripted.TipOffset(), manual.TipOffset()) < 1e-4f,
          "an authored stroke curve lands exactly where the same mouse does");
    check(scripted.Phase() == manual.Phase(),
          "...and reaches the same phase of the state machine");
  }

  detail = Format("%d checks", checks);
  std::printf("swing: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// swing-plane — THE WHOLE PIPELINE, MEASURED ON THE SWORD
// =============================================================================
//
// `swing` above asserts the MAPPING and stops at MeleeState's outputs. This one
// asserts the thing the player actually sees, and there is a great deal of
// machinery between the two: the driver's hand/blade command goes through the
// mirrored-authoring frame flip, a two-bone IK solve with a steered pole, a
// steered elbow hinge, a wrist orientation, the pose-limit clamp, the flatten,
// the kinematic submit and Jolt — and then comes BACK as `WeaponEdge`, the
// blade's real hitbox read off its real transform. Any one of those links can
// silently turn a horizontal sweep into a forward jab, which is exactly the bug
// this feature was rewritten for.
//
// So every assertion here is on the SWORD'S OWN WORLD TRAJECTORY, measured
// about the live shoulder joint, and the input is a scripted mouse fed through
// the same per-tick path main.cpp uses. Nothing reads MeleeState's opinion of
// where the blade is.
//
//   A. right-to-left flick sweeps AZIMUTH, holds ELEVATION, stays IN FRONT
//   B. overhead flick sweeps ELEVATION, holds AZIMUTH
//   C. top-right to bottom-left sweeps BOTH, in one plane, in the right sense
//   D. while merely steering, mouse x maps MONOTONICALLY to tip azimuth
//   E. a cut through a standing dummy wounds the limb the edge PASSED THROUGH
//      and not the one on the far side
//   F. an edge-on cut takes more than a flat-on cut of the same speed
//
// Thresholds live in tests/baseline.json (CLAUDE.md: a bound in source costs a
// rebuild to tune), and every measured value is pushed back through
// RecordObserved so `--rebaseline` can retune them in one command.
//
// ---- REPAIRED 2026-08-31: WHAT WAS ACTUALLY WRONG WITH THIS FIXTURE --------
//
// This gate shipped known-failing with a note blaming the residency window: it
// claimed to pass standalone, to fail only in the suite, and to need the window
// PINNED the way `voxregion` pins it. Every clause of that was wrong, and it is
// worth writing down why, because the shape of the mistake is the one CLAUDE.md
// rule 6 is about — the note was a HYPOTHESIS derived from a bare count ("0 of
// 0 wounded limbs") and nothing had ever been recorded at the point of failure.
//
// It failed in BOTH scopes. Three faults, none of them residency:
//
//  1. THE TARGET WALKED OUT FROM UNDER THE BLADE. The dummy is a `human`, and
//     human.json declares no `behavior`, so MobSystem::DecideIntent falls
//     through to the LEGACY WANDER — which sets driveScale_ = 1 and only ever
//     steers when something blocks it. A human's authored speed is 31.5 vox/s,
//     so eight settling ticks carry it 8.4 voxels downrange: spawned at z 261,
//     read at z 269.4, with all fifteen limb bodies alive and present the whole
//     time. The sweep then swept the empty air where it used to be (measured
//     tip box z 257.5..268.0 against a body that had moved to 269.4), and pass
//     F's synthetic sweep — whose coordinates are computed from the SPAWN
//     position — missed by the same 8.4 voxels.
//
//     THE FIX IS DATA, not a special case: a training dummy is a BEHAVIOUR
//     PROFILE (`dummy` in behaviors.json — blind, passive, `mobile: false`), so
//     the fixture asks for it by name. `Movement::mobile` false is enforced once
//     at the bottom of ai::Think, which is precisely the guarantee a target
//     fixture needs and precisely what a def with no profile at all does not
//     have.
//
//  2. THE WINDOW ANCHOR MIXED UNITS. `WindowOrigin()` is in CHUNKS and the old
//     line added kWorldN/2 VOXELS to it. That is a no-op at the origin (which
//     is why it looked like a suite-only failure) and off by a factor of kChunk
//     anywhere else — under the full suite it placed the fixture 44 voxels
//     outside the window, where PreTick despawns it on tick one. See the note
//     at the anchor below.
//
//  3. PASSES A-C READIED INTO THE AZIMUTH STOP. `swing` block 7 already says
//     this in as many words about its own fixture: a walk cycle leaves the arm
//     at roughly 127 degrees of azimuth, half a radian short of `azOut`, so
//     "fourteen slow ticks of rightward mouse" is a measurement of the CLAMP
//     and of a shoulder pose-limit the rig cannot serve. The stroke and the
//     sword then part company and the arc reads as 12.9 voxels out of plane on
//     a 13.0 voxel radius. The ready is now CLOSED-LOOP on the stroke state
//     (`readyTo` below) and lands on an authored start pose well inside every
//     stop, so what the flick measures is the flick.
//
// Pass 0 (the tip follows the stroke, 2.39 of 3.92 voxels) passed throughout
// and was the correct discriminator: the CONTROL LAW was never at fault.

// One tick's reading of the blade, in the frame the assertions are stated in:
// the tip about the SHOULDER JOINT, which is the centre the stroke is defined
// around. Taken from the physics bodies, not from the pose — this is the hitbox.
// TWO READS OF THE SAME SWORD, and the gate needs both.
//
//   `tip`/`base`/`flat`  the PHYSICS edge (Mob::WeaponEdge), off the kinematic
//                        body the damage sweep actually casts against. This is
//                        the hitbox, so it is what passes E and F.
//   `fromShoulder`       the POSED point (Mob::WeaponStrokePose), shoulder-
//                        relative, straight out of the IK. This is what the
//                        SHAPE assertions use.
//
// They are not the same number, and pretending they were is what made the first
// version of this gate unreadable. The kinematic submit is a link further down
// the chain — one tick of Jolt motion, plus the held item's grip spring — and
// mid-sweep the two are up to ten voxels apart while the posed sword tracks its
// command to a fifth of a voxel. Measuring an ARC through that gap measures the
// gap. So: shape on the pose, damage on the physics, and one assertion that the
// physics edge tracks the pose at all, with the size of the gap reported.
struct BladeRead {
  bool valid = false;
  Vec3 tip{}, base{}, flat{};      // physics
  Vec3 fromShoulder{}, hand{};     // pose, shoulder-relative
  float az = 0, el = 0, r = 0;
  float physGap = 0;               // pose point -> physics point, world voxels
};

// A tiny statistics bag over a sampled stroke, so each assertion below is one
// comparison against one baseline number rather than a loop.
struct StrokeStats {
  int n = 0;
  float azMin = 1e9f, azMax = -1e9f;
  float elMin = 1e9f, elMax = -1e9f;
  float rMax = 0;
  float minFwd = 1e9f;         // most-backward tip, shoulder-relative z
  float planarWorst = 0;       // worst out-of-plane distance, world voxels
  Vec3 first{}, last{}, normal{};
  int azBacksteps = 0;
  // Where the ready actually left the stroke, and whether it converged. A
  // sweep measured from an unreachable start pose is not a measurement of the
  // sweep (see fault 3 in the header note), so the start is reported next to
  // every arc rather than assumed.
  bool readied = false;
  float readyAz = 0, readyEl = 0;
  // THE SAME THREE NUMBERS OFF THE DRIVER'S OWN COMMANDED TIP, so a failure
  // names its half of the pipeline instead of leaving both suspect. Pass 0
  // establishes that the rig follows the stroke while merely STEERING; these
  // establish it (or not) through a COMMITTED cut, where the arc's own bow and
  // the wrist limit are in play and pass 0 never looks.
  //
  // cmd* large + observed* large  -> the stroke itself is not planar (driver)
  // cmd* small + observed* large  -> the rig cannot reproduce it (rig)
  float cmdElDev = 0, cmdAzSweep = 0, cmdPlanarWorst = 0, cmdRMax = 0;
  int cmdAzBacksteps = 0;
  Vec3 cmdFirst{}, cmdLast{};
  float residWorst = 0;   // commanded tip -> posed tip, world voxels
  // ...AND WHY, from the rig's own recorder (Mob::WeaponArmDiag), sampled on
  // the tick the residual was worst. There are three independent ways for a
  // weapon pose to come out wrong and they all look the same downstream; this
  // is the struct that was built to tell them apart, so the gate reads it
  // instead of guessing (CLAUDE.md rule 6).
  Mob::WeaponArmDiag worstDiag{};
};

Status GateSwingPlane(Ctx& c, std::string& detail) {
  // ---- thresholds, all from tests/baseline.json ---------------------------
  const float azSweepMin = (float)BaselineNumber("swingPlane.azSweepMin", 0.90);
  const float elDevMax = (float)BaselineNumber("swingPlane.elDevMax", 0.60);
  const float elSweepMin = (float)BaselineNumber("swingPlane.elSweepMin", 0.70);
  const float azDevMax = (float)BaselineNumber("swingPlane.azDevMax", 0.80);
  const float planarMaxFrac =
      (float)BaselineNumber("swingPlane.planarMaxFrac", 0.35);
  const float frontMinFrac =
      (float)BaselineNumber("swingPlane.frontMinFrac", -0.35);
  const float diagRatioMax =
      (float)BaselineNumber("swingPlane.diagRatioMax", 4.0);
  const float edgeGainMin = (float)BaselineNumber("swingPlane.edgeGainMin", 1.15);
  // How far the POSED sword may sit from the COMMANDED tip, as a fraction of
  // the commanded radius. This is the rig's authored anatomy, not a swing
  // property: see the note at printCmd. Loose on purpose and paired with an
  // attribution line - the honest bound is "whatever human.json's shoulder
  // costs", and tightening it is a RIG change, not a threshold edit.
  const float rigResidualFrac =
      (float)BaselineNumber("swingPlane.rigResidualFrac", 0.85);
  // ...and how much of the commanded ARC the physical sword may lose, radians.
  // The strict claim; a pipeline that drops the stroke fails here.
  const float sweepTrackMax =
      (float)BaselineNumber("swingPlane.sweepTrackMax", 0.35);
  // cos of the angle between the commanded travel and the travel the sword
  // actually made. Used where azimuth is the wrong coordinate (the diagonal).
  const float travelAgreeMin =
      (float)BaselineNumber("swingPlane.travelAgreeMin", 0.85);

  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const char* what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("swing-plane: FAILED %s\n", what);
    }
  };

  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  const ItemLibrary& items = c.items;

  const int swordDef = items.Find("sword");
  const ItemDef* sword = items.At(swordDef);
  int avDef = -1, dummyDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++) {
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
    if (mobs.Defs()[i].name == "human") dummyDef = (int)i;
  }
  if (!sword || avDef < 0) {
    detail = "no sword item or no avatar def";
    std::printf("swing-plane: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  // This gate spawns four fixtures, and a mob id seeds id-keyed draws all over
  // the engine — so it puts the counter back (test/support.h IdCounterScope).
  IdCounterScope idScope(mobs);
  debris.Reset();
  mobs.Reset();
  // PRISTINE TERRAIN UNDER THE FIXTURE. This gate runs late, after gates that
  // light fires and pour acid at absolute coordinates; the avatar has to stand
  // on ground and the dummies have to spawn into open air, and neither is true
  // on whatever the neighbours left behind.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // ---- the fixture: an avatar with the blade drawn, facing +Z --------------
  // Heading 0 and an axis-aligned basis: -X is the camera's right (the note
  // on kRight at the top of this file — the camera's chirality, which is what
  // the game hands melee.Update and what the renderer calls screen-right),
  // +Y up, +Z the way the character faces.
  const Vec3 kR{-1, 0, 0}, kU{0, 1, 0}, kF{0, 0, 1};
  // ANCHORED TO THE RESIDENCY WINDOW, never to an absolute coordinate, and
  // ANCHORED IN THE RIGHT UNITS — which is the whole of the bug this gate was
  // landed known-failing for.
  //
  // `World::WindowOrigin()` is in CHUNKS (world.h; `ChunkInWindow` compares it
  // against a chunk coord). The first version of this line read
  //
  //     const int gx = wo.x + kWorldN / 2;          // chunks + VOXELS
  //
  // which is a chunk index plus a voxel count. It is accidentally correct at
  // wo = (0,0,0) and wrong by a factor of kChunk everywhere else: under the
  // full suite `streaming` has shifted the window ~20 chunks, so the fixture
  // was placed at voxel 276 while the window covered [320, 832) — 44 voxels
  // outside it. MobSystem::PreTick then despawns the dummy on its first tick
  // as out-of-window and the gate reads "0 of 0 wounded limbs", which is
  // exactly the shape selftest_mob.cpp's AiFixtureCentre note describes and is
  // NOT a fault in the sweep at all.
  //
  // Stated the same way AiFixtureCentre states it, deliberately: chunk centre
  // first, then convert once. There is no arithmetic here that mixes units.
  const IVec3 wo = world.WindowOrigin();
  const int gx = (wo.x + (int)kNChunk / 2) * (int)kChunk;
  const int gz = (wo.z + (int)kNChunk / 2) * (int)kChunk;
  // THE WINDOW CENTRE, AND NOT THE FLATTEST PATCH NEAR IT. Moving this fixture
  // 96 voxels away to find flat ground was tried and it broke `ai-approach`,
  // which reported "wall 0/33 columns in mirror" and walked its mob straight
  // through a stone wall: that gate writes its wall as BrushOps and then reads
  // the CPU MIRROR, and the mirror only holds chunks something has anchored —
  // which, until this gate moved, was this gate's own avatar standing on the
  // very spot ai-approach builds on. A latent dependency in a gate three
  // positions later, and worth recording rather than routing around silently.
  //
  // The slope this was meant to fix is handled where it belongs instead: the
  // cut AIMS AT THE TARGET'S LIVE CHEST (pass E) rather than assuming the two
  // bodies settled at the same height.
  const int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
  uint32_t t = 21000;
  // ...AND THE WINDOW MUST NOT MOVE UNDER THE FIXTURE while the gate runs.
  // `voxregion` saves the origin and puts it back so an early return cannot
  // leave it moved; this gate wants the stronger property, because every
  // fixture coordinate below was computed from `wo` ONCE and a shift halfway
  // through would strand them all. `avTick` re-asserts it every tick and the
  // check at the end of the gate reports if anything moved it.
  const IVec3 pinnedWindow = wo;

  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, c.mats, &mobs);
  avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
  Player pl;
  pl.fly = false;
  pl.grounded = true;   // else the loco clips never start (see the mob gate)
  pl.pos = Vec3{(float)gx + 0.5f, (float)(gh + 2) + Player::kHalfY,
                (float)gz + 0.5f};
  if (!avatar.Spawn(pl, 0.0f)) {
    detail = "avatar spawn failed";
    std::printf("swing-plane: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const bool equipped = avatar.EquipItem(sword);
  check(equipped, "the sword equips into the avatar's hand");

  auto avTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    const IVec3 pc{ifloor(pl.pos.x) >> 4, ifloor(pl.pos.y) >> 4,
                   ifloor(pl.pos.z) >> 4};
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false, pc,
               true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
    avatar.PostStep();
  };
  for (int i = 0; i < 10; i++) avTick();

  // ---- WHY THE FIXTURE IS NOT THERE, in one line ---------------------------
  //
  // "0 of 0 wounded limbs" and "edge-on removed 0 vox" are bare counts
  // (CLAUDE.md rule 6): they say the dummy lost nothing and name none of the
  // four reasons it could have. This says which one. A creature that was
  // DESPAWNED (out of window) reports differently from one that DIED, from one
  // that WALKED OFF, and from one that is standing exactly where it was put
  // while the blade goes somewhere else — and the first three are fixture
  // faults while only the last is a sweep fault.
  auto fixtureLine = [&](const char* what, uint64_t id) {
    if (!id || dummyDef < 0) {
      std::printf("swing-plane fixture %s: SPAWN REFUSED\n", what);
      return;
    }
    int live = 0;
    const MobDef& fd = mobs.Defs()[dummyDef];
    for (size_t i = 0; i < fd.limbs.size(); i++)
      if (mobs.LimbBody(id, (int)i)) live++;
    const bool present = mobs.MobIdAt(0) == id || mobs.IsAlive(id) ||
                         mobs.LimbBody(id, 0) != 0;
    const Vec3 o = mobs.MobOrigin(id);
    std::printf(
        "swing-plane fixture %s: id %llu, %s, alive %d, %d/%zu limb bodies, at "
        "(%.1f,%.1f,%.1f), window chunks (%d,%d,%d) = voxels [%d,%d)\n",
        what, (unsigned long long)id, present ? "present" : "GONE FROM mobs_",
        (int)mobs.IsAlive(id), live, fd.limbs.size(), o.x, o.y, o.z,
        world.WindowOrigin().x, world.WindowOrigin().y, world.WindowOrigin().z,
        world.WindowOrigin().x * (int)kChunk,
        world.WindowOrigin().x * (int)kChunk + (int)kWorldN);
  };

  // ---- A TARGET THAT STAYS WHERE IT IS PUT ---------------------------------
  //
  // The one call every fixture spawn in this gate goes through, and the whole
  // of fault 1 in the header note. `human` has no authored `behavior`, so a
  // bare Spawn() gets the legacy wander and walks off at 31.5 vox/s. Asking
  // for the `dummy` PROFILE by name is the data-shaped answer: passive, blind,
  // `mobile: false`, which ai::Think enforces once at the bottom for every
  // intent rather than trusting each branch.
  //
  // Loud on failure rather than silent: a behaviours.json that lost the
  // `dummy` profile would otherwise put this gate straight back where it was,
  // with a target that walks and a sweep blamed for missing it.
  auto spawnTarget = [&](int wx, int wy, int wz) -> uint64_t {
    const uint64_t id = mobs.Spawn(dummyDef, {wx, wy, wz});
    if (!id) return 0;
    if (!mobs.SetMobBehavior(id, "dummy"))
      std::printf(
          "swing-plane: WARNING no \"dummy\" behaviour profile — the target "
          "will WANDER out of reach and every wound check below is void\n");
    return id;
  };

  // THE SHOULDER JOINT IN WORLD SPACE, from the live body: the same
  // `xf.pos + rot * anchorLimb` composition the submit path inverts, so this is
  // the joint itself rather than the corner of the upper arm's box. Every angle
  // below is measured about it, because that is the centre the stroke is
  // defined around and an offset centre would smear azimuth into elevation.
  const int shoulderPart = avatar.PartIndex("armU.R");
  auto shoulderWorld = [&]() -> Vec3 {
    Vec3 j;
    if (shoulderPart >= 0 && avatar.PartJointWorld(shoulderPart, j)) return j;
    return avatar.Origin();
  };

  MeleeState melee;
  // THE GATE MEASURES WHAT THE GAME RUNS, and until this line it did not.
  // MeleeState holds a MeleeTuning by VALUE and a default-constructed one
  // carries the compiled initialisers in game/melee.h — so every number this
  // gate reports was a number nobody could change without a rebuild, and
  // `melee.wristMaxAngle` in tuning.json reached main.cpp's MeleeState and
  // stopped there. That cost a whole differential: raising the JSON wrist cap
  // moved the NPC gates (which do call this) and left every swing-plane figure
  // BYTE-IDENTICAL, which reads as "the wrist is not the constraint" and is
  // really "the knob is not connected". CLAUDE.md's cheap-to-verify rule: a
  // threshold that lives in source costs a rebuild to tune.
  ApplyMeleeTuning(melee.tuning);
  // ...AND THEN THE FEEL KNOBS ARE PINNED (2026-09-01), because this gate's
  // subject is the COMMAND -> POSE MAPPING and the shipped feel deliberately
  // stopped tracking the command tick-exactly:
  //
  //   * armSmoothing / wristSmoothing ease the stroke and the wrist's chase.
  //     Lag measured against the command reads as residual, and the residual
  //     bounds here would then be asserting a halflife, not the mapping.
  //   * The steer band moved to 0.6..3.0 m/s and only Slash bypasses it — the
  //     gate's driven sweeps are deliberately UNDER the commit cap (Guard/
  //     Wind), so at live values they run at partial alignment BY DESIGN
  //     (steer 0.52 mid-sweep, residual 13.35 vox, all of it intentional
  //     grip-pose blend). The pin is steerFloor = 1 — the documented A/B that
  //     restores always-align — and NOT a saturated speed band: a band still
  //     dips to the floor on the one tick of every REVERSAL (the tip passes
  //     through zero speed), and with the envelope's smoothing also pinned
  //     off, that dip is a snap. Measured: pass A's el drift went 0.34 ->
  //     1.38 rad from exactly those ticks (wrist applied 0.55 of a wanted
  //     2.69 mid-sweep), which is a flop to the grip pose in the middle of
  //     the arc the pass is measuring.
  //
  // Pass G tests the live floor semantics, so the pin is LIFTED around it
  // (search steerFloorLive below).
  //
  // What ships is the mapping measured here COMPOSED WITH those knobs; the
  // knobs' reachability is combat-tuning's assertion, and their feel is the
  // player's own Combat panel.
  const float steerFloorLive = melee.tuning.steerFloor;
  melee.tuning.armSmoothing = 0.0f;
  melee.tuning.wristSmoothing = 0.0f;
  melee.tuning.steerFloor = 1.0f;
  auto readBlade = [&]() -> BladeRead {
    BladeRead b;
    float hw = 0;
    if (!avatar.WeaponEdge(b.base, b.tip, hw, &b.flat)) return b;
    Vec3 flat;
    float reach = 0;
    if (!avatar.WeaponStrokePose(b.hand, b.fromShoulder, flat, reach)) return b;
    b.physGap = ((b.tip - shoulderWorld()) - b.fromShoulder).len();
    b.r = b.fromShoulder.len();
    if (b.r < 1e-4f) return b;
    b.az = std::atan2(-b.fromShoulder.x, b.fromShoulder.z);  // basis az
    b.el = std::asin(std::clamp(b.fromShoulder.y / b.r, -1.0f, 1.0f));
    b.valid = true;
    return b;
  };

  // ONE DRIVEN TICK, exactly as main.cpp does it: read where the blade is, feed
  // the control delta, advance, push the whole pose, then step the world. A
  // gate that skipped the read-back would be testing an open loop.
  auto drive = [&](float dx, float dy, bool held) {
    Vec3 hand, tip, flat;
    float reach = 0;
    if (avatar.WeaponStrokePose(hand, tip, flat, reach))
      melee.SetStroke(hand, tip, flat, reach);
    else
      melee.ClearArm();
    melee.Feed(dx, dy);
    melee.Update(kTickDt, held, true, kR, kU, kF);
    avatar.SetWeaponPose(melee.Pose());
    avTick();
  };

  // ---- BRING THE BLADE TO A STATED START POSE, CLOSED-LOOP -----------------
  //
  // Fault 3 of the header note, fixed at the source. An OPEN-LOOP ready ("14
  // ticks of +14 px") is a bet that the arm's rest pose plus fourteen gains
  // lands somewhere useful, and it lost: the walk cycle leaves the weapon arm
  // hanging at ~127 degrees of azimuth, half a radian from `azOut`, so the
  // ready drove into the stop and the sweep that followed measured the clamp.
  //
  // This steers the STROKE STATE (`StrokeAz`/`StrokeEl`, the pure integral of
  // the input — melee.h) onto an authored target instead. It is the same thing
  // an NPC attack style's WINDUP segment does, and deliberately so: a ready is
  // a slow move to a guard, and the only thing that makes it a ready rather
  // than a cut is that it stays under `commitSpeed`.
  //
  // The per-tick step is capped at 18 px on each axis (540 px/s against a 900
  // px/s commit threshold) so this can never fire a stroke by accident — which
  // would make everything measured afterwards the follow-through arc instead of
  // the flick. Returns false if it could not converge, so a rig that cannot
  // hold the pose says so rather than quietly measuring something else.
  auto readyTo = [&](float wantAz, float wantEl) -> bool {
    for (int i = 0; i < 60; i++) {
      const float dAz = wantAz - melee.StrokeAz();
      const float dEl = wantEl - melee.StrokeEl();
      if (std::fabs(dAz) < 0.03f && std::fabs(dEl) < 0.03f) return true;
      // Screen-DOWN is +dy and LOWERS the point (melee.h aimGainY is positive
      // and main.cpp feeds raw pixels), so a wanted RISE in elevation is a
      // negative dy. Getting this backwards is a ready that runs away from its
      // target for sixty ticks and then reports a stroke from the wrong place.
      // `melee.tuning`, not a local MeleeTuning: the gain the ready divides by
      // must be the gain the driver multiplies by, or the loop converges on a
      // pose nobody asked for. (`t` in this function is the TICK.)
      float dx = std::clamp(dAz / melee.tuning.aimGainX, -18.0f, 18.0f);
      float dy = std::clamp(-dEl / melee.tuning.aimGainY, -18.0f, 18.0f);
      drive(dx, dy, true);
    }
    return std::fabs(wantAz - melee.StrokeAz()) < 0.15f &&
           std::fabs(wantEl - melee.StrokeEl()) < 0.15f;
  };

  // Sample a stroke: ready to a stated start pose (no commit), then `flick`
  // fast ticks in one direction, recording the blade through the flick.
  auto runStroke = [&](float readyAz, float readyEl, float flickX,
                       float flickY, int flickN) -> StrokeStats {
    StrokeStats s;
    melee.Reset();
    // The click, with a still mouse: take-over, which must move nothing.
    drive(0, 0, true);
    s.readied = readyTo(readyAz, readyEl);
    s.readyAz = melee.StrokeAz();
    s.readyEl = melee.StrokeEl();
    std::vector<Vec3> path, cmdPath;
    float cmdAzMin = 1e9f, cmdAzMax = -1e9f, cmdElMin = 1e9f, cmdElMax = -1e9f;
    float prevAz = 0;
    bool havePrev = false;
    // ---- AZIMUTH IS UNWRAPPED BEFORE IT IS RANGED ---------------------------
    //
    // `az` is an atan2 and the shoulder is off to the weapon side, so the tip
    // crosses the +-pi branch on any arc that passes behind it — which a
    // committed cut of 2.0 rad does routinely. A min/max taken over the RAW
    // values then reports a sweep of nearly 2*pi for a stroke that never
    // reversed, and the "az drift" of a clean vertical cut becomes a number
    // about a branch cut rather than about the sword. Both channels (observed
    // and commanded) accumulate WRAPPED deltas onto a running angle instead,
    // which is continuous by construction; the backstep counters use the same
    // deltas, so a crossing can no longer be mistaken for a reversal either.
    // Same bug, same fix, as Mob::ApplyWeaponArm's hinge angle.
    auto wrapPi = [](float a) {
      while (a > 3.14159265f) { a -= 6.28318531f; }
      while (a <= -3.14159265f) { a += 6.28318531f; }
      return a;
    };
    float azAcc = 0, cmdAzAcc = 0, prevRawAz = 0, prevCmdRawAz = 0;
    bool haveCmdPrev = false;
    for (int i = 0; i < flickN; i++) {
      drive(flickX, flickY, true);
      const BladeRead b = readBlade();
      // The COMMANDED tip, in exactly the frame the observation is stated in.
      {
        const Vec3 w = melee.TipOffset();
        const float wr = std::max(w.len(), 1e-4f);
        const float waz = std::atan2(-w.x, w.z);   // basis az (right=-X)
        const float wel = std::asin(std::clamp(w.y / wr, -1.0f, 1.0f));
        if (!haveCmdPrev) {
          cmdAzAcc = waz;
          haveCmdPrev = true;
        } else {
          const float d = wrapPi(waz - prevCmdRawAz);
          cmdAzAcc += d;
          if (d > 1e-3f) s.cmdAzBacksteps++;
        }
        prevCmdRawAz = waz;
        cmdAzMin = std::min(cmdAzMin, cmdAzAcc);
        cmdAzMax = std::max(cmdAzMax, cmdAzAcc);
        cmdElMin = std::min(cmdElMin, wel);
        cmdElMax = std::max(cmdElMax, wel);
        s.cmdRMax = std::max(s.cmdRMax, wr);
        if (cmdPath.empty()) s.cmdFirst = w;
        s.cmdLast = w;
        cmdPath.push_back(w);
        if (b.valid) {
          const float resid = (b.fromShoulder - w).len();
          if (resid > s.residWorst) {
            s.residWorst = resid;
            s.worstDiag = avatar.WeaponArmDiagnostics();
          }
        }
      }
      if (!b.valid) continue;
      s.n++;
      if (!havePrev) azAcc = b.az;
      else azAcc += wrapPi(b.az - prevRawAz);
      prevRawAz = b.az;
      s.azMin = std::min(s.azMin, azAcc);
      s.azMax = std::max(s.azMax, azAcc);
      s.elMin = std::min(s.elMin, b.el);
      s.elMax = std::max(s.elMax, b.el);
      s.rMax = std::max(s.rMax, b.r);
      s.minFwd = std::min(s.minFwd, b.fromShoulder.z);
      if (path.empty()) s.first = b.fromShoulder;
      s.last = b.fromShoulder;
      path.push_back(b.fromShoulder);
      if (havePrev && azAcc > prevAz + 1e-3f) s.azBacksteps++;
      prevAz = azAcc;
      havePrev = true;
    }
    // THE SWEEP'S OWN PLANE, from the first and last radii. A sweep is planar
    // when every intermediate point lies near the plane those two span; that is
    // the geometric content of "it swings, it does not wander".
    if (path.size() >= 3) {
      s.normal = s.first.cross(s.last);
      if (s.normal.len() > 1e-3f) {
        s.normal = s.normal.normalized();
        for (const Vec3& p : path)
          s.planarWorst = std::max(s.planarWorst, std::fabs(p.dot(s.normal)));
      }
    }
    if (cmdPath.size() >= 3) {
      s.cmdAzSweep = cmdAzMax - cmdAzMin;
      s.cmdElDev = cmdElMax - cmdElMin;
      Vec3 n = cmdPath.front().cross(cmdPath.back());
      if (n.len() > 1e-3f) {
        n = n.normalized();
        for (const Vec3& p : cmdPath)
          s.cmdPlanarWorst = std::max(s.cmdPlanarWorst, std::fabs(p.dot(n)));
      }
    }
    // Release and let the arm be handed back, so the next stroke starts from a
    // rest pose rather than from the last one's follow-through.
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    return s;
  };

  // ---- WHOSE FAULT, AS ASSERTIONS AND NOT ONLY AS A PRINTOUT --------------
  //
  // The shape claims used to be stated ONLY on the posed sword, and that
  // conflated two independent properties that fail for unrelated reasons:
  //
  //   * IS THE STROKE A SWEEP? - a property of the control law, and the thing
  //     this feature was rewritten twice to get right. Stated on the COMMANDED
  //     arc, which is what the driver produced.
  //   * CAN THIS BODY MAKE IT? - a property of human.json's authored joint
  //     limits. `swingPlane.rigResidualFrac` bounds it and the arm diagnostic
  //     line names which limit spent it.
  //
  // Measured, and the reason for the split: pass A's commanded arc is planar to
  // 1.09 voxels while the posed sword is 8.83 off it, ENTIRELY because the
  // right shoulder's authored reach (`max: 30` degrees past the midline) clamps
  // 0.93 rad out of the last third of a committed horizontal cut. One number
  // reported both, so the gate said "the swing wanders" about a swing that does
  // not - and a real bug found underneath it (the hinge-angle wrap, anim.h
  // AnimHingeAngleAbout) moved that number from 12.91 to 8.83 without ever
  // being visible as the separate thing it was.
  //
  // A tighter posed-sword claim survives at each pass and is the one that
  // matters: the ARC the physical sword covers must track the commanded one
  // (`sweepTrackMax`). A rig that dropped the stroke on the floor would fail
  // that however planar its own path happened to be.
  auto printCmd = [&](const char* which, const StrokeStats& s) {
    check(s.cmdRMax > 1e-3f && s.cmdPlanarWorst < planarMaxFrac * s.cmdRMax,
          "the STROKE the driver commanded is planar");
    check(s.cmdAzBacksteps <= 3,
          "...and runs one way through the target rather than reversing");
    check(s.cmdRMax > 1e-3f && s.residWorst < rigResidualFrac * s.cmdRMax,
          "...and the RIG serves it within its authored joint limits (if this "
          "fails, read the arm line below: ikMiss = cannot reach, wrist = the "
          "wrist limit, shoulder/elbow = a pose limit clamped it)");
    std::printf(
        "swing-plane %s (commanded): az sweep %.2f, el drift %.2f, "
        "out-of-plane %.2f of r %.2f, %d backsteps; worst commanded->posed "
        "residual %.2f vox (max %.2f)\n",
        which, s.cmdAzSweep, s.cmdElDev, s.cmdPlanarWorst, s.cmdRMax,
        s.cmdAzBacksteps, s.residWorst, rigResidualFrac * s.cmdRMax);
    const Mob::WeaponArmDiag& d = s.worstDiag;
    std::printf(
        "swing-plane %s (arm at worst): ikMiss %.2f vox, wrist %.2f/%.2f rad, "
        "clamp %.2f rad (%.2f vox), shoulder %.2f, elbow %.2f, roundTrip "
        "%.2f\n",
        which, d.ikMiss, d.wristApplied, d.wristWant, d.clampMove,
        d.clampShift, d.shoulderClamp, d.elbowClamp, d.roundTrip);
  };

  // ---- 0. DOES THE RIG DO WHAT IT IS TOLD? --------------------------------
  //
  // Every assertion below this one compares the sword's trajectory against what
  // the player asked for, and there are two entirely different ways to fail
  // that: the DRIVER can compute the wrong stroke, or the RIG can fail to
  // reproduce the stroke it was handed. A gate that only measured the sword
  // would report one number for both, and the first version of this gate did
  // exactly that — "az sweep 4.31 rad, out-of-plane 9.91 of r 14.09" names no
  // cause at all (CLAUDE.md rule 6: a bare count is not a measurement).
  //
  // So the residual between COMMANDED tip and OBSERVED tip is measured first
  // and reported on its own line. A large residual here means the arm cannot
  // reach, or the wrist clamp is fighting, or the pose limits are eating the
  // solve — and the sweeps below are then measuring the rig, not the mapping.
  {
    // ---- AND THE FIRST TWO TICKS ARE NOT PART OF THE CLAIM -----------------
    //
    // TAKE-OVER IS A RAMP BY DESIGN, and since the wrist steer throttle landed
    // (melee.h MeleeTuning::steerSpeedLo) it is a ramp in ORIENTATION as well
    // as in position. Control starts at the blade's actual pose — which, on a
    // still mouse, is the pose the authored GRIP gives it — and eases into the
    // commanded alignment over the envelope's attack as soon as the stroke is
    // moving. The first tick or two of any stroke therefore have the sword
    // where the grip put it rather than where the stroke asks, on purpose, and
    // it is the same continuity property the whole take-over design is built
    // on: nothing may pop on the tick the player clicks.
    //
    // So the ASSERTION is on the settled residual and the transient is
    // reported beside it, rather than the bound being loosened to swallow
    // both. Measured here: worst 4.40 vox over all 24 ticks, 1.02 from tick 3
    // on, against a 3.92 bound at r 11.20.
    melee.Reset();
    drive(0, 0, true);
    float worst = 0, worstSettled = 0, mean = 0, gapWorst = 0;
    float worstSteer = 1.0f;
    int n = 0, worstTick = -1;
    Mob::WeaponArmDiag worstFollowDiag{};
    const int kSettleTicks = 3;
    for (int i = 0; i < 24; i++) {
      drive(i < 12 ? 12.0f : -12.0f, 0, true);
      const BladeRead b = readBlade();
      if (!b.valid) continue;
      const Vec3 want = melee.TipOffset();
      const float err = (b.fromShoulder - want).len();
      gapWorst = std::max(gapWorst, b.physGap);
      if (err > worst) {
        // WHY, at the point of failure. A bare "worst residual 4.40" names
        // none of the four things that produce it (CLAUDE.md rule 6), and the
        // throttle added a fifth: a blade that is deliberately NOT aligned
        // because the stroke is not committed reads exactly like a wrist that
        // could not reach.
        worst = err;
        worstTick = i;
        worstSteer = melee.SteerAmount();
        worstFollowDiag = avatar.WeaponArmDiagnostics();
      }
      if (i >= kSettleTicks) worstSettled = std::max(worstSettled, err);
      mean += err;
      n++;
    }
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    mean = n ? mean / (float)n : 0;
    const float followMax = (float)BaselineNumber("swingPlane.followMaxFrac", 0.35);
    const float rNow = std::max(melee.StrokeRadius(), 1e-3f);
    check(n > 18, "the blade stayed readable through the follow test");
    check(worstSettled < followMax * rNow,
          "the SWORD goes where the stroke says it goes (if this fails, every "
          "sweep below is measuring the rig and not the mapping)");
    RecordObserved("swingPlane.followWorstObserved", worst);
    RecordObserved("swingPlane.followSettledObserved", worstSettled);
    std::printf(
        "swing-plane 0 (follow): worst residual %.2f vox over all ticks, %.2f "
        "from tick %d on, mean %.2f, over %d ticks at r %.2f (max %.2f)\n"
        "swing-plane 0 (at worst): tick %d, steer %.2f, wrist %.2f/%.2f rad, "
        "ikMiss %.2f, clamp %.2f rad (%.2f vox), shoulder %.2f, elbow %.2f\n",
        worst, worstSettled, kSettleTicks, mean, n, rNow, followMax * rNow,
        worstTick, worstSteer, worstFollowDiag.wristApplied,
        worstFollowDiag.wristWant, worstFollowDiag.ikMiss,
        worstFollowDiag.clampMove, worstFollowDiag.clampShift,
        worstFollowDiag.shoulderClamp, worstFollowDiag.elbowClamp);
  }

  // ---- R/G. WHERE THE SWORD POINTS WHEN NOBODY IS SWINGING IT -------------
  //
  // Every other pass in this gate measures a MOVING blade, and the whole set
  // of them was green while the sword looked wrong for 99% of a session. Four
  // owner reports, all about a blade that is not being swung:
  //
  //   "the hilt sticks through the hand instead of jutting out of the fist"
  //   "the wrist only aligns while actively swinging"
  //   "holding the hand out in front, the sword points vertically DOWN"
  //   "I want it pointing outward in front at rest, and UP with the arm out"
  //
  // Two poses, two properties, and neither is a swing:
  //
  //   R (REST). Idle: `weaponWeight_` is 0, ApplyWeaponArm returns before it
  //     steers anything, and the item renders at `handQ * gripRot`. So this
  //     measures the AUTHORED GRIP and nothing else. A grip that lays the
  //     blade along the forearm passes every dynamic assertion in this file
  //     and is a hilt buried in a fist.
  //   G (GUARD, HELD STILL). The button is down and the mouse is not moving.
  //     The stroke still commands the blade along its (mostly radial)
  //     law-of-cosines direction, so with the wrist applied at full strength
  //     the fist is wrenched round to lay the sword along the shoulder-to-
  //     point line — pointing DOWN the arm rather than up out of it. The
  //     steer ramp (melee.h MeleeTuning::steerSpeedLo) is what makes this a
  //     grip pose again, and this pass is what says so.
  //
  // MEASURED ON THE PHYSICS EDGE, like passes E and F: the hitbox off the
  // kinematic body, which is what the player is looking at. Angles rather than
  // voxels so nothing here moves with kVoxelMeters.
  {
    const int elbowPart = avatar.PartIndex("armL.R");
    const int wristPart = avatar.PartIndex("hand.R");
    auto bladeAndArm = [&](Vec3& blade, Vec3& fore) -> bool {
      Vec3 base, tip, e, w;
      float hw = 0;
      if (!avatar.WeaponEdge(base, tip, hw, nullptr)) return false;
      blade = tip - base;
      if (blade.len() < 1e-4f) return false;
      blade = blade.normalized();
      if (elbowPart < 0 || wristPart < 0) return false;
      if (!avatar.PartJointWorld(elbowPart, e)) return false;
      if (!avatar.PartJointWorld(wristPart, w)) return false;
      fore = w - e;
      if (fore.len() < 1e-4f) return false;
      fore = fore.normalized();
      return true;
    };
    auto deg = [](float c) {
      return std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.2957795f;
    };

    // ---- R: hand down at the side, no claim on the arm --------------------
    melee.Reset();
    avatar.SetWeaponPose(WeaponPose{});   // weight 0: Idle, the walk cycle owns it
    for (int i = 0; i < 24; i++) avTick();
    Vec3 rBlade, rFore;
    const bool rOk = bladeAndArm(rBlade, rFore);
    const float restVsArm = rOk ? deg(rBlade.dot(rFore)) : 0.0f;
    const float restVsFwd = rOk ? deg(rBlade.dot(kF)) : 0.0f;
    const float restVsDown = rOk ? deg(rBlade.dot(kU * -1.0f)) : 0.0f;
    const float restAcrossMin =
        (float)BaselineNumber("swingPlane.restBladeAcrossArmMinDeg", 55.0);
    const float restFwdMax =
        (float)BaselineNumber("swingPlane.restBladeFwdMaxDeg", 55.0);
    check(rOk, "the resting sword and the forearm are both readable");
    check(!rOk || restVsArm > restAcrossMin,
          "AT REST THE BLADE JUTS OUT OF THE FIST rather than lying down the "
          "forearm (the hilt-through-the-hand grip)");
    check(!rOk || restVsFwd < restFwdMax,
          "...and it points outward IN FRONT of the character");
    RecordObserved("swingPlane.restBladeAcrossArmObservedDeg", restVsArm);
    RecordObserved("swingPlane.restBladeFwdObservedDeg", restVsFwd);
    std::printf(
        "swing-plane R (rest grip): blade (%.2f,%.2f,%.2f) — %.1f deg off the "
        "forearm (min %.0f), %.1f deg off forward (max %.0f), %.1f off down\n",
        rBlade.x, rBlade.y, rBlade.z, restVsArm, restAcrossMin, restVsFwd,
        restFwdMax, restVsDown);

    // ---- G: THE BLADE IS RAISED AND THEN NOTHING HAPPENS ------------------
    //
    // The literal shape of the report, and the strongest discriminator in this
    // gate: click, and then hold perfectly still. The stroke is seeded from
    // the arm the walk cycle left HANGING, so its azimuth/elevation are down
    // at the bottom of the window and its (mostly radial) commanded blade
    // direction points AT THE FLOOR. Applied at full strength that is exactly
    // "the sword points vertically down"; ramped down it is the grip pose,
    // which is the same forward-out-of-the-fist blade pass R just measured.
    //
    // Sixty still ticks rather than five: the ramp's release eases over four
    // blade halflives and a reading taken mid-decay measures the transient.
    //
    // Measured on this fixture, the two arms of that A/B are 1.80 rad of wrist
    // applied against 0.26, with the steer amount at 1.00 against 0.15.
    //
    // THE LIVE FLOOR, not the gate's always-align pin: this pass IS the ramp's
    // own test, so it runs at the shipped semantics and re-pins afterwards.
    melee.tuning.steerFloor = steerFloorLive;
    melee.Reset();
    const bool gReady = true;   // no ready: taking over IS the pose
    for (int i = 0; i < 60; i++) drive(0, 0, true);
    Vec3 gBlade, gFore;
    const bool gOk = bladeAndArm(gBlade, gFore);
    const float guardVsUp = gOk ? deg(gBlade.dot(kU)) : 180.0f;
    const float guardVsArm = gOk ? deg(gBlade.dot(gFore)) : 0.0f;
    const Mob::WeaponArmDiag gd = avatar.WeaponArmDiagnostics();
    // THE CLAIM IS "NOT DOWN", not "exactly up", and the difference is a
    // measurement rather than a softening. The report is "holding the hand out
    // in front, the sword points vertically DOWN": with the wrist applied at
    // full strength a motionless guard lays the blade along the shoulder-to-
    // point radius, and a stroke seeded from a hanging arm has that radius
    // aimed at the floor. What the grip pose gives instead is measured below
    // and asserted as an ELEVATION FLOOR on the blade's own y, which is the
    // axis the complaint is about. An exact "up" is a property of the solved
    // hand's ROLL, which the two-bone solver picks from the pole and does not
    // promise; asserting it would be asserting something this pipeline does
    // not offer, and the honest number is printed next to the bound.
    const float guardUpMin =
        (float)BaselineNumber("swingPlane.guardBladeUpMinY", -0.20);
    const float guardArmMin =
        (float)BaselineNumber("swingPlane.guardBladeAcrossArmMinDeg", 45.0);
    check(gReady && melee.Phase() == SwingPhase::Guard,
          "holding the button with a still mouse stays in Guard");
    check(gOk, "the guarding sword and the forearm are both readable");
    check(!gOk || gBlade.y > guardUpMin,
          "A MOTIONLESS GUARD DOES NOT DROP THE POINT AT THE FLOOR (the blade "
          "keeps its grip angle instead of being laid along the radius)");
    check(!gOk || guardVsArm > guardArmMin,
          "...and it is still out of the fist rather than laid along the arm");
    check(melee.SteerAmount() < 0.5f,
          "a motionless guard has ramped the wrist steering down (if this "
          "fails the two above are measuring a fully-steered blade)");
    RecordObserved("swingPlane.guardBladeUpObservedY", gBlade.y);
    RecordObserved("swingPlane.guardBladeAcrossArmObservedDeg", guardVsArm);
    RecordObserved("swingPlane.guardSteerObserved", melee.SteerAmount());
    std::printf(
        "swing-plane G (guard hold): blade (%.2f,%.2f,%.2f) y >= %.2f, %.1f "
        "deg off UP; forearm (%.2f,%.2f,%.2f), %.1f deg off it (min %.0f); "
        "steer %.2f, wrist want %.2f applied %.2f, elbow axis %.2f rad\n",
        gBlade.x, gBlade.y, gBlade.z, guardUpMin, guardVsUp, gFore.x, gFore.y,
        gFore.z, guardVsArm, guardArmMin, melee.SteerAmount(), gd.wristWant,
        gd.wristApplied, gd.elbowAxisTurn);
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    melee.tuning.steerFloor = 1.0f;   // back to the mapping pin (see the top)
  }

  // ---- H. THE ARM STAYS IN FRONT AND THE ELBOW BENDS ONE WAY --------------
  //
  // The other two reports, and they are one measurement each. Both are stated
  // on the ANATOMY rather than on the stroke, because both were caused by the
  // rig's authored limits being bypassed rather than by the driver asking for
  // anything unreasonable:
  //
  //   "the arm can be driven BEHIND the player"   -> azOut was 2.36 rad (135
  //      degrees), which commands a point past the frontal plane, so the
  //      shoulder sat on its authored 50-degrees-past-the-back stop.
  //   "the elbow can bend both ways"              -> ApplyWeaponArm re-signed
  //      the hinge-axis override until the measured bend was positive, which
  //      makes the authored [0, 130] hinge accept either direction.
  //
  // Driven INTO both stops on purpose (60 ticks of hard rightward mouse is far
  // past `azOut` at any gain), because a limit is only a limit where it binds.
  {
    melee.Reset();
    drive(0, 0, true);
    const int elbowPart = avatar.PartIndex("armL.R");
    const int wristPart = avatar.PartIndex("hand.R");
    // THE POINT, not the fist, and that distinction is the whole measurement.
    // `azOut` bounds the TIP's azimuth, and the hand is a blade-length in
    // front of the tip — so at 135 degrees the POINT is most of a blade behind
    // the shoulder while the hand has barely moved. A check written on the
    // hand reports almost nothing about the knob it is testing.
    float worstBack = 1e9f;       // most-backward TIP, shoulder-relative z
    float worstHandBack = 1e9f;   // ...and the hand, for the record
    float worstElbowFwd = -1e9f;  // most-FORWARD elbow bulge, -1..1
    int n = 0;
    for (int i = 0; i < 60; i++) {
      // Under the commit threshold, so this is a steer into the stop and not
      // a cut: what is being measured is where the arm may be PUT.
      drive(i < 30 ? 14.0f : -14.0f, i < 30 ? -6.0f : 6.0f, true);
      Vec3 hand, tip, flat;
      float reach = 0;
      if (!avatar.WeaponStrokePose(hand, tip, flat, reach)) continue;
      n++;
      worstBack = std::min(worstBack, tip.z);
      worstHandBack = std::min(worstHandBack, hand.z);
      // WHICH SIDE THE ELBOW BULGES TO, from the three joints in world space.
      //
      // "THE ELBOW BENDS BOTH WAYS" IS A SIDE, NOT AN ANGLE, and the first
      // version of this check measured the angle and therefore measured
      // nothing. A three-point chain has no notion of hyperextension: the
      // shoulder-elbow-wrist angle of an elbow bent 40 degrees "backwards" is
      // identical to one bent 40 degrees forwards with the elbow on the other
      // side of the arm. What a player calls a backwards elbow is the joint
      // POINTING THE WRONG WAY, which is entirely the two-bone solver's pole.
      //
      // So: project the elbow off the shoulder-to-wrist line and ask which way
      // that offset faces. A human elbow trails DOWN, BACK or OUT; one that
      // leads out in FRONT of its own arm is the bug.
      Vec3 sh, el, wr;
      if (elbowPart < 0 || wristPart < 0) continue;
      if (!avatar.PartJointWorld(shoulderPart, sh)) continue;
      if (!avatar.PartJointWorld(elbowPart, el)) continue;
      if (!avatar.PartJointWorld(wristPart, wr)) continue;
      Vec3 line = wr - sh;
      if (line.len() < 1e-3f) continue;
      line = line.normalized();
      const Vec3 rel = el - sh;
      Vec3 off = rel - line * rel.dot(line);
      // A degenerate offset is a STRAIGHT arm, which has no side and cannot be
      // the wrong one. Skipping it is not skipping a failure.
      if (off.len() < 0.15f) continue;
      worstElbowFwd = std::max(worstElbowFwd, off.normalized().dot(kF));
    }
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    // A HAND behind the shoulder plane is the report. Stated as a fraction of
    // the arm's own reach so it does not move with kVoxelMeters, and generous:
    // some rearward travel is a wind-up, a hand a third of an arm behind the
    // shoulder is the arm being driven round the back.
    const float backMinFrac =
        (float)BaselineNumber("swingPlane.armBehindMinFrac", -0.34);
    const float armReach = std::max(melee.StrokeRadius(), 1e-3f);
    // ...and the elbow. +1 is an elbow bulging straight forward out of its own
    // arm, -1 is one trailing straight back. Some forward lead is real (a
    // raised guard puts the elbow slightly ahead of the shoulder-wrist line),
    // so the bound is not 0; an elbow more than halfway to leading its own arm
    // is the joint pointing the wrong way.
    const float elbowFwdMax =
        (float)BaselineNumber("swingPlane.elbowForwardMax", 0.50);
    check(n > 40, "the arm stayed readable through the drive into the stops");
    check(n == 0 || worstBack > backMinFrac * armReach,
          "THE BLADE IS NOT DRIVEN BEHIND THE PLAYER even when the stroke is "
          "pushed into its azimuth stop");
    check(worstElbowFwd < -1e8f || worstElbowFwd < elbowFwdMax,
          "THE ELBOW TRAILS THE ARM rather than leading it: no pose in the "
          "drive put the joint out in front of its own shoulder-to-wrist line");
    RecordObserved("swingPlane.armBehindObservedVox", worstBack);
    RecordObserved("swingPlane.handBehindObservedVox", worstHandBack);
    RecordObserved("swingPlane.elbowForwardObserved", worstElbowFwd);
    std::printf(
        "swing-plane H (limits): tip reached z %.2f vox behind the shoulder "
        "(min %.2f at r %.2f; hand %.2f), elbow bulged forward at most %.3f "
        "(max %.2f), over %d ticks\n",
        worstBack, backMinFrac * armReach, armReach, worstHandBack,
        worstElbowFwd, elbowFwdMax, n);
  }

  // ---- A. right to left is a horizontal sweep in front --------------------
  // THE OWNER'S ACCEPTANCE CRITERION: "moving mouse right to left will swing
  // the sword right to left horizontally".
  {
    // READIED WELL OUT TO THE WEAPON SIDE AND LEVEL, and the number is chosen
    // by the SHOULDER'S authored reach rather than by taste. human.json bounds
    // the right upper arm to 30 degrees past the midline
    // (`poseLimit.reach normal [1,0,0] max 30`), and a committed cut carries
    // ~2.0 rad of arc on its own (the since-removed `swingArc`) whatever the mouse
    // does afterwards. So the last third of any committed horizontal cut is
    // spent ON that stop: the ball limit clamps the shoulder by ~0.93 rad and
    // drags the posed sword out of the plane the driver commanded (measured:
    // commanded arc planar to 1.09 vox, posed 8.83 at r 13.0). Starting further
    // OUT does not help — it buys a longer arc that ends in the same place, and
    // measured worse (10.58). That deviation is the ANATOMY, and the checks
    // below are split so it is reported as anatomy instead of as a swing bug.
    const StrokeStats s = runStroke(1.15f, 0.05f, -34.0f, 0.0f, 16);
    const float azSweep = s.azMax - s.azMin;
    const float elDev = s.elMax - s.elMin;
    check(s.readied, "the ready reached its start pose without committing");
    check(s.n > 8, "the blade was readable through the horizontal flick");
    check(azSweep > azSweepMin,
          "a right-to-left flick sweeps the TIP through a real arc of azimuth");
    check(elDev < elDevMax, "...and holds its height while it does");
    check(s.rMax > 1e-3f && s.minFwd > frontMinFrac * s.rMax,
          "...and the whole arc passes in front of the chest");
    // THE PHYSICAL SWORD COVERED THE ARC IT WAS TOLD TO. Planarity and
    // reversal now belong to the commanded stroke (see printCmd); this is the
    // posed-sword claim that survives, and it is the strict one - a pipeline
    // that ate a third of the sweep fails here at 0.35 rad of slack.
    check(std::fabs(azSweep - s.cmdAzSweep) < sweepTrackMax,
          "the SWORD sweeps the arc the stroke asked for");
    RecordObserved("swingPlane.planarPosedObserved", s.planarWorst);
    RecordObserved("swingPlane.rigResidualObserved", s.residWorst);
    RecordObserved("swingPlane.azSweepObserved", azSweep);
    RecordObserved("swingPlane.elDevObserved", elDev);
    std::printf(
        "swing-plane A (horizontal): az sweep %.2f rad (min %.2f), el drift "
        "%.2f (max %.2f), min forward %.2f of r %.2f, out-of-plane %.2f (max "
        "%.2f), %d backsteps\n",
        azSweep, azSweepMin, elDev, elDevMax, s.minFwd, s.rMax, s.planarWorst,
        planarMaxFrac * s.rMax, s.azBacksteps);
    printCmd("A", s);
  }

  // ---- B. an overhead flick is a vertical sweep ---------------------------
  {
    // Readied to a raised guard straight ahead, then driven DOWN: an overhead
    // cut is a fall, and starting it from a hanging arm (which is what the old
    // open-loop ready did) leaves nowhere to fall from.
    const StrokeStats s = runStroke(0.0f, 0.90f, 0.0f, 34.0f, 16);
    const float elSweep = s.elMax - s.elMin;
    const float azDev = s.azMax - s.azMin;
    check(s.readied, "the ready reached its start pose without committing");
    check(s.n > 8, "the blade was readable through the vertical flick");
    check(elSweep > elSweepMin,
          "an overhead flick sweeps the TIP through a real arc of elevation");
    check(azDev < azDevMax, "...and holds its bearing while it does");
    check(std::fabs(elSweep - s.cmdElDev) < sweepTrackMax,
          "the SWORD sweeps the elevation arc the stroke asked for");
    RecordObserved("swingPlane.elSweepObserved", elSweep);
    RecordObserved("swingPlane.azDevObserved", azDev);
    std::printf(
        "swing-plane B (vertical): el sweep %.2f rad (min %.2f), az drift %.2f "
        "(max %.2f), out-of-plane %.2f of r %.2f\n",
        elSweep, elSweepMin, azDev, azDevMax, s.planarWorst, s.rMax);
    printCmd("B", s);
  }

  // ---- C. a diagonal flick is a diagonal cut ------------------------------
  //
  // Stated on the TRAVEL rather than on the plane normal, and deliberately: a
  // normal is sign-ambiguous (either sense names the same plane), so an
  // assertion against an expected normal quietly becomes an assertion about
  // which way a cross product happened to come out. The travel is not
  // ambiguous — top-right to bottom-left goes left AND down, in comparable
  // measure — and it is the thing the player asked for.
  {
    // Readied high and to the weapon side; the flick then goes left AND down.
    const StrokeStats s = runStroke(1.00f, 0.80f, -26.0f, 26.0f, 16);
    check(s.readied, "the ready reached its start pose without committing");
    // world x: LEFTWARD IS POSITIVE — screen-left is -camRight, and camRight
    // is world -X at this heading (the kRight note at the top of the file).
    const float dAz = s.last.x - s.first.x;
    const float dEl = s.last.y - s.first.y;   // world y: downward is negative
    const float ratio = std::fabs(dAz) > 1e-4f && std::fabs(dEl) > 1e-4f
                            ? std::max(std::fabs(dAz) / std::fabs(dEl),
                                       std::fabs(dEl) / std::fabs(dAz))
                            : 1e9f;
    check(dAz > 0.0f, "a top-right to bottom-left flick takes the point LEFT");
    check(dEl < 0.0f, "...and DOWN");
    check(ratio < diagRatioMax,
          "...in comparable measure, i.e. genuinely diagonal rather than a "
          "horizontal or vertical cut with a wobble");
    // TRACKED ON THE TRAVEL, not on azimuth. A and B each have one dominant
    // angular axis and azimuth/elevation are the natural coordinates for them;
    // a steep diagonal has neither, and azimuth is ill-conditioned wherever the
    // point passes near the vertical — a sweep that tracks its command
    // perfectly can then report a wildly different azimuth range for reasons
    // that are pure spherical coordinates. The travel vector has no such
    // degeneracy and is the thing the player asked for anyway.
    const Vec3 cmdTravel = s.cmdLast - s.cmdFirst;
    const Vec3 gotTravel = s.last - s.first;
    const float travelAgree =
        cmdTravel.len() > 1e-3f && gotTravel.len() > 1e-3f
            ? cmdTravel.normalized().dot(gotTravel.normalized())
            : -1.0f;
    check(travelAgree > travelAgreeMin,
          "...and the SWORD travels the way the stroke asked it to");
    RecordObserved("swingPlane.diagTravelAgreeObserved", travelAgree);
    RecordObserved("swingPlane.diagRatioObserved", ratio);
    std::printf(
        "swing-plane C (diagonal): ready (%.2f, %.2f); travel (%.2f, %.2f) "
        "vox, ratio %.2f (max %.2f), out-of-plane %.2f of r %.2f, travel "
        "agreement %.3f (min %.2f)\n",
        s.readyAz, s.readyEl, dAz, dEl, ratio, diagRatioMax, s.planarWorst,
        s.rMax, travelAgree, travelAgreeMin);
    printCmd("C", s);
  }

  // ---- D. steering is monotone --------------------------------------------
  //
  // Not a sweep: this is the GUARD, where the player is aiming rather than
  // cutting. Every equal step of mouse x must move the tip's azimuth the same
  // way, or the blade cannot be aimed at all — and a mapping that only behaved
  // during the committed arc would still pass A above.
  {
    melee.Reset();
    drive(0, 0, true);
    // Start from a leftward lean so the whole sweep is inside the azimuth
    // window: the stop is a clamp, and a monotonicity test that ran into one
    // would be measuring the clamp.
    for (int i = 0; i < 10; i++) drive(-12.0f, 0, true);
    float prev = readBlade().az;
    int steps = 0, backwards = 0, wraps = 0;
    float total = 0;
    // EVERY STEP IS WRAPPED INTO (-pi, pi], and this is not a nicety: `az` is
    // an atan2, so it jumps by 2*pi the moment the tip crosses behind the
    // shoulder, and the shoulder is off to the weapon side — a left-leaning
    // start puts the tip there routinely. Summed raw, ONE crossing turns a
    // clean +2.25 rad of rightward travel into -4.03 and reports "steady
    // rightward mouse carries the tip LEFT", which is the same unwrapped-angle
    // bug this branch already fixed once in Mob::ApplyWeaponArm. The wrap
    // count is printed rather than hidden, because a step that genuinely
    // moved more than pi would be indistinguishable from a crossing and is
    // worth seeing.
    auto wrapPi = [](float a) {
      while (a > 3.14159265f) { a -= 6.28318531f; }
      while (a <= -3.14159265f) { a += 6.28318531f; }
      return a;
    };
    for (int i = 0; i < 22; i++) {
      drive(12.0f, 0, true);
      const BladeRead b = readBlade();
      if (!b.valid) continue;
      steps++;
      const float raw = b.az - prev;
      const float d = wrapPi(raw);
      if (std::fabs(raw - d) > 1e-3f) wraps++;
      if (d < -1e-3f) backwards++;
      total += d;
      prev = b.az;
    }
    check(steps > 15, "the blade stayed readable through the steer");
    check(total > 0.5f, "steady rightward mouse carries the tip right");
    check(backwards <= (int)BaselineNumber("swingPlane.steerBackMax", 4),
          "...monotonically: equal steps of mouse never send the tip back");
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    RecordObserved("swingPlane.steerTotalObserved", total);
    RecordObserved("swingPlane.steerBackObserved", (double)backwards);
    std::printf(
        "swing-plane D (steering): %d steps, %.2f rad of azimuth, %d "
        "backwards, %d atan2 wraps\n",
        steps, total, backwards, wraps);
  }

  // ---- E. the wound lands where the edge passed ---------------------------
  //
  // The same shape as the `mob` gate's melee subtest — near limb loses voxels,
  // far limb does not — but driven by a REAL swing rather than by a direct
  // carve call, so it is the sweep's own geometry under test and not just
  // CarveLimbRadial's.
  bool hitOk = true;
  if (dummyDef < 0) {
    std::printf("swing-plane E: SKIP (no \"human\" mob def to cut)\n");
  } else {
    const MobDef& dd = mobs.Defs()[dummyDef];
    // WITHIN REACH, which is much closer than it sounds: an arm is 0.6 m and
    // this sword is 0.55 m, so the point never gets much past 1.2 m from the
    // shoulder. The interactive --duel-dummy stands at 3 m because you walk to
    // it; a gate that placed its target there would be asserting that a sword
    // cannot reach three metres, which is true and useless.
    // SPAWN TAKES THE MIN CORNER IN WORLD CELLS, and `prefab.size` is in ART
    // units — for this rig, eight of them to the cell. Subtracting half the
    // ART size to "centre" the body put it fourteen voxels off to the side,
    // which the gate reported as a clean miss with a straight face until the
    // failure line started printing the swept box next to the body's own
    // position. `worldSize` is the same box in cells and is what a placement
    // should be reasoning in.
    const int dx = gx, dz = gz + 5;
    // THE PLAYER'S OWN GROUND PLANE, not the dummy column's. A slope between
    // the two puts a dummy spawned at its own terrain height either buried
    // (no limb bodies at all) or standing where the blade cannot reach; the
    // avatar and its target want the same floor.
    const int dh = gh;
    // RESET FIRST, exactly as pass F does: a gate that spawns a fixture should
    // be clearing the system it spawns into. (The comment that used to be here
    // blamed this reset for pass F "working" while E read every limb as gone.
    // It did no such thing — both were reading a target that had WALKED, and
    // the difference was only that E measured eight ticks later. See fault 1
    // in the header note.)
    mobs.Reset();
    debris.Reset();
    const uint64_t tid = spawnTarget(dx, dh + 2, dz);
    // A SECOND BODY, PAST THE BLADE'S REACH. This is what carries "and not the
    // far side": a per-limb near/far split on ONE body cannot, because a
    // committed cut with this sword severs, and severing a torso drops
    // everything hanging off it — four limbs the edge came nowhere near lost
    // all their voxels for a reason that is correct engine behaviour and has
    // nothing to do with where the blade went. Two bodies cannot leak into each
    // other that way, and the claim is the one that matters: damage is located
    // by the WEAPON'S POSE, so a body outside the arc is untouched however hard
    // the swing is aimed at it.
    const int fz = gz + 17;
    const int fh = gh;
    const uint64_t farId = spawnTarget(dx, fh + 2, fz);
    if (!tid) {
      std::printf("swing-plane E: SKIP (dummy spawn failed)\n");
    } else {
      fixtureLine("E near (fresh)", tid);
      for (int i = 0; i < 8; i++) avTick();
      fixtureLine("E near (settled)", tid);
      fixtureLine("E far  (settled)", farId);
      std::vector<uint32_t> before(dd.limbs.size(), 0), farBeforeV(dd.limbs.size(), 0);
      std::vector<Vec3> beforeWhere(dd.limbs.size());
      std::vector<float> beforePos(dd.limbs.size(), 1e9f);
      for (size_t i = 0; i < dd.limbs.size(); i++) {
        before[i] = mobs.LimbBody(tid, (int)i)
                        ? mobs.LimbVoxelCount(tid, (int)i)
                        : 0;
        // WHERE THE LIMB WAS BEFORE THE CUT. Read here and not after, because
        // "after" is a body that may not exist: a committed cut severs, and a
        // severed limb has no transform to measure a distance from.
        if (before[i]) beforeWhere[i] = mobs.LimbVoxelPos(tid, (int)i, 0);
        farBeforeV[i] = farId && mobs.LimbBody(farId, (int)i)
                            ? mobs.LimbVoxelCount(farId, (int)i)
                            : 0;
      }

      // The cut: load right, then flick left through the dummy's front. THE
      // SWEEP IS DRIVEN TOO — main.cpp calls MeleeSweepDamage once per tick off
      // the blade's own previous and current edge, and a gate that only posed
      // the arm would be asserting that a swing looks right while never
      // touching anything.
      melee.Reset();
      drive(0, 0, true);
      // The ready ALSO brings the point up to chest height: the walk cycle
      // leaves the arm hanging at about -30 degrees of elevation, and a
      // purely horizontal ready keeps it there — six voxels under a target
      // whose chest is what a cut is aimed at. Closed-loop, for the reason
      // `readyTo` exists: an open-loop version of this line parked the blade
      // on the azimuth stop.
      // THE ELEVATION IS MEASURED, NOT ASSUMED. A fixed ready of 0.15 rad puts
      // the blade at chest height only when the avatar and its target settled
      // on the same floor, and on a slope they do not: in the full suite the
      // dummy came to rest 2.8 voxels lower than it does standalone and the
      // whole sweep passed over its head (tip box y 233..241 against a body
      // spanning 217..234), which the gate reported as "0 of 0 wounded limbs"
      // about a fixture that was alive and fifteen-limbed the whole time.
      //
      // Taken about the LIVE shoulder and against the LIVE chest, so it is
      // right on any ground and needs no flat-spot search (see the anchor note).
      const Vec3 tgtChest{(float)dx + dd.worldSize.x * 0.5f,
                          mobs.MobOrigin(tid).y + dd.worldSize.y * 0.60f,
                          (float)dz + dd.worldSize.z * 0.5f};
      const Vec3 sh = shoulderWorld();
      const Vec3 toTgt = tgtChest - sh;
      const float aimEl = std::asin(std::clamp(
          toTgt.y / std::max(toTgt.len(), 1e-3f), -1.0f, 1.0f));
      const bool eReady = readyTo(1.15f, aimEl);
      check(eReady, "the cut's ready reached its start pose");
      std::printf(
          "swing-plane E: shoulder y %.1f, target chest y %.1f, ready el "
          "%.2f rad\n",
          sh.y, tgtChest.y, aimEl);
      std::vector<Vec3> tipPath, basePath;
      BladeRead prev = readBlade();
      int sweptTicks = 0;
      for (int i = 0; i < 18; i++) {
        drive(-34.0f, 0, true);
        const BladeRead b = readBlade();
        if (!b.valid) continue;
        tipPath.push_back(b.tip);
        basePath.push_back(b.base);
        // Every tick of the drive IS the cut: the driver has no Slash of its
        // own to gate on any more (melee.h SwingPhase), and a stroke program
        // sweeps on its own Cut phase, which this drive stands in for.
        if (prev.valid) {
          EdgeSweep sw;
          sw.aPrev = prev.base;
          sw.bPrev = prev.tip;
          sw.aNow = b.base;
          sw.bNow = b.tip;
          sw.flatNow = b.flat;
          sw.dt = kTickDt;
          sw.halfWidth = sword->edgeHalfWidth;
          sw.strike = sword->strike;
          sw.carveBonus = sword->carveBonus;
          // The SHIPPED sword's heft, not the neutral default: the kerf's
          // depth scales with it, so a gate that left it at 1 would be
          // measuring a wound no weapon in the game makes.
          sw.heft = sword->HeftFactor(CurrentTuning().gore.woundHeftRef,
                                      CurrentTuning().gore.woundHeftMax);
          sw.tick = (uint32_t)i;
          sw.valid = true;
          std::vector<ParticleSpawn> spawns;
          MeleeSweepDamage(sw, melee.tuning, avatar, phys, mobs, debris, world,
                           spawns);
          sweptTicks++;
        }
        prev = b;
      }
      for (int i = 0; i < 20; i++) drive(0, 0, false);
      check(sweptTicks > 0, "the flick committed a cut the sweep could run");

      // WHERE THE WOUNDS ARE, against WHERE THE BLADE WAS.
      //
      // Not a "nearest limb / farthest limb" pair, which is what this started
      // as and could not survive its own fixture: a committed cut with this
      // sword severs whatever it touches, so by the time the pair was chosen
      // there were no limbs left to choose from and both ends read as -1. The
      // claim that actually matters is per-limb and survives dismemberment —
      // EVERY limb that lost voxels was one the edge passed through, and a limb
      // the edge never came near kept all of them.
      //
      // Distances are to the whole EDGE SEGMENT, not to the point: the sweep
      // casts rays down the blade's length, so a limb the ricasso went through
      // is a limb the edge hit.
      auto distToEdge = [&](const Vec3& lp) {
        float best = 1e9f;
        for (size_t k = 0; k < tipPath.size(); k++) {
          const Vec3 a = basePath[k], b = tipPath[k];
          const Vec3 ab = b - a;
          const float len2 = ab.dot(ab);
          const float t = len2 > 1e-6f
                              ? std::clamp((lp - a).dot(ab) / len2, 0.0f, 1.0f)
                              : 0.0f;
          best = std::min(best, (a + ab * t - lp).len());
        }
        return best;
      };
      for (size_t i = 0; i < dd.limbs.size(); i++)
        if (before[i]) beforePos[i] = distToEdge(beforeWhere[i]);
      const float nearVox = (float)BaselineNumber("swingPlane.woundNearVox", 3.0);
      int hitNear = 0, wounded = 0;
      for (size_t i = 0; i < dd.limbs.size(); i++) {
        if (before[i] == 0) continue;   // nothing there to lose
        const uint32_t after = mobs.LimbBody(tid, (int)i)
                                   ? mobs.LimbVoxelCount(tid, (int)i)
                                   : 0;
        if (after >= before[i]) continue;
        wounded++;
        if (beforePos[i] < nearVox) hitNear++;
      }
      uint32_t farLost = 0, farTotal = 0;
      for (size_t i = 0; i < dd.limbs.size(); i++) {
        farTotal += farBeforeV[i];
        const uint32_t after = farId && mobs.LimbBody(farId, (int)i)
                                   ? mobs.LimbVoxelCount(farId, (int)i)
                                   : 0;
        if (after < farBeforeV[i]) farLost += farBeforeV[i] - after;
      }
      const bool struck = hitNear > 0;
      const bool spared = farId != 0 && farTotal > 0 && farLost == 0;
      hitOk = struck && spared;
      check(struck, "a real swing wounds the limbs the edge passed through");
      check(spared,
            "...and leaves a body outside the arc completely alone");
      // WHERE THE BLADE WENT, on the line. A miss here is nearly always the
      // FIXTURE — the stroke swept over the dummy's head, or past its shoulder
      // — rather than the sweep, and a bare "148->148" cannot tell those apart
      // (CLAUDE.md rule 6).
      Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
      for (const Vec3& p : tipPath) {
        lo = Vec3{std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = Vec3{std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
      std::printf(
          "swing-plane E (wound): %d of %d wounded limbs were under the edge "
          "(within %.1f vox); the out-of-reach body lost %u of %u vox; %d "
          "swept ticks, tip box (%.1f,%.1f,%.1f)..(%.1f,%.1f,%.1f)\n",
          hitNear, wounded, nearVox, farLost, farTotal, sweptTicks, lo.x, lo.y,
          lo.z, hi.x, hi.y, hi.z);
      // HOW CLOSE THE EDGE ACTUALLY CAME, when nothing was wounded. "0 of 0"
      // alone cannot distinguish a geometric miss (fixture/arc fault) from
      // probes that passed through the body without hitting it (sweep fault),
      // and the distances are already computed — print the three closest.
      if (wounded == 0) {
        std::vector<std::pair<float, int>> close;
        for (size_t i = 0; i < dd.limbs.size(); i++)
          if (before[i]) close.push_back({beforePos[i], (int)i});
        std::sort(close.begin(), close.end());
        std::printf("swing-plane E (miss attribution): edge-to-limb closest");
        for (size_t i = 0; i < close.size() && i < 3; i++)
          std::printf(" | %s %.2f vox",
                      dd.limbs[close[i].second].name.c_str(), close[i].first);
        std::printf("\n");
      }
      mobs.Reset();
    }
  }
  (void)hitOk;

  // ---- F. the edge leads, or the cut is a slap ----------------------------
  //
  // Driven through MeleeSweepDamage directly, with two synthetic sweeps that
  // are IDENTICAL except for the blade's roll. Going through the rig instead
  // would make the two arms differ in speed and position as well, and then the
  // comparison would not be about alignment at all.
  if (dummyDef >= 0) {
    const MobDef& dd = mobs.Defs()[dummyDef];
    EdgeSweepResult resEdge{}, resFlat{};
    auto cutOnce = [&](bool edgeOn, EdgeSweepResult& res) -> uint32_t {
      mobs.Reset();
      const int dx = gx, dz = gz + 5;
      const int dh = gh;
      const uint64_t tid = spawnTarget(dx, dh + 2, dz);
      if (!tid) return 0;
      for (int i = 0; i < 8; i++) avTick();
      fixtureLine(edgeOn ? "F edge-on" : "F flat-on", tid);
      uint32_t total0 = 0;
      for (size_t i = 0; i < dd.limbs.size(); i++)
        if (mobs.LimbBody(tid, (int)i))
          total0 += mobs.LimbVoxelCount(tid, (int)i);

      // THE BLADE'S OWN LENGTH HAS TO CROSS THE BODY, because the sweep casts
      // its rays ALONG the blade (that is how a thin fast edge hits at all),
      // and each ray only reaches about two voxels. A segment laid ALONGSIDE
      // the target finds nothing however hard it is swung: the first version
      // did exactly that and reported zero voxels lost for BOTH arms, which
      // reads as "edge alignment is broken" and was really "the fixture
      // missed". Hilt in front of the chest, point behind it.
      // OFF THE LIVE BODY. Computing this from the SPAWN cell assumes the rig
      // settled where it was put, and a rig settles onto whatever ground is
      // under it — the same slope that put pass E's sweep over the target's
      // head would put this reference cut through empty air.
      const Vec3 chest{(float)dx + dd.worldSize.x * 0.5f,
                       mobs.MobOrigin(tid).y + dd.worldSize.y * 0.66f,
                       (float)dz + dd.worldSize.z * 0.5f};
      const Vec3 travel{-3.0f, 0, 0};
      EdgeSweep sw;
      sw.aPrev = chest + Vec3{1.5f, 0, 4.0f};
      sw.bPrev = chest + Vec3{1.5f, 0, -3.0f};
      sw.aNow = sw.aPrev + travel;
      sw.bNow = sw.bPrev + travel;
      // Edge-on: the flat's normal is square to the travel. Flat-on: the normal
      // points along the travel, which is the blade going through sideways.
      sw.flatNow = edgeOn ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
      sw.dt = kTickDt;
      sw.halfWidth = sword->edgeHalfWidth;
      sw.strike = sword->strike;
      sw.carveBonus = sword->carveBonus;
      // Same sword, same tick seed in both arms — the whole point of the pair
      // is that they differ ONLY in the blade's roll, and the kerf's seed and
      // heft are two more things that must not be allowed to vary.
      sw.heft = sword->HeftFactor(CurrentTuning().gore.woundHeftRef,
                                  CurrentTuning().gore.woundHeftMax);
      sw.tick = 7u;
      sw.valid = true;
      std::vector<ParticleSpawn> spawns;
      res = MeleeSweepDamage(sw, melee.tuning, avatar, phys, mobs, debris,
                             world, spawns);
      for (int i = 0; i < 4; i++) avTick();
      uint32_t total1 = 0;
      for (size_t i = 0; i < dd.limbs.size(); i++)
        if (mobs.LimbBody(tid, (int)i))
          total1 += mobs.LimbVoxelCount(tid, (int)i);
      mobs.Reset();
      return total0 > total1 ? total0 - total1 : 0;
    };
    const uint32_t lostEdge = cutOnce(true, resEdge);
    const uint32_t lostFlat = cutOnce(false, resFlat);
    // The pure arithmetic too, so a zero-hit positioning failure is
    // distinguishable from an alignment failure.
    const float alignEdge =
        MeleeEdgeAlign(Vec3{0, 1, 0}, Vec3{-1, 0, 0}, melee.tuning.edgeFloor);
    const float alignFlat =
        MeleeEdgeAlign(Vec3{1, 0, 0}, Vec3{-1, 0, 0}, melee.tuning.edgeFloor);
    check(alignEdge > alignFlat * edgeGainMin,
          "MeleeEdgeAlign rates an edge-on cut above a flat-on one");
    check(alignFlat >= melee.tuning.edgeFloor - 1e-4f,
          "...and a flat still bruises: the floor is a floor, not a gate");
    check(resEdge.bodiesHit > 0 && resFlat.bodiesHit > 0,
          "both reference cuts reached the dummy");
    // THE LIVE SWEEP'S OWN VERDICT, which is the number that scales the damage
    // and the carve radius. Asserting on it rather than only on voxels lost is
    // what makes this a test of the DAMAGE PATH: a build that computed the
    // alignment and then forgot to multiply by it would pass a voxel
    // comparison whenever the two cuts happened to sever the same limbs.
    check(resEdge.edgeAlign > resFlat.edgeAlign * edgeGainMin,
          "the live sweep rates the edge-on cut above the flat-on one");
    // ...AND VOXELS, but only >=. A sword this fast severs whatever it touches
    // in one tick (the rig's severImpactSpeed is well under the tip speed a
    // committed cut reaches), so both arms can saturate at "the limb came off"
    // and a strict > would be asserting that the sever threshold sits in a
    // particular place rather than that alignment matters.
    check(lostEdge >= lostFlat,
          "an edge-on cut never takes less than a flat-on cut of the same "
          "speed");
    check(lostEdge > 0, "the edge-on reference cut removed real voxels");
    RecordObserved("swingPlane.edgeVoxObserved", (double)lostEdge);
    RecordObserved("swingPlane.flatVoxObserved", (double)lostFlat);
    std::printf(
        "swing-plane F (edge): edge-on removed %u vox, flat-on %u; align "
        "%.2f vs %.2f (floor %.2f)\n",
        lostEdge, lostFlat, alignEdge, alignFlat, melee.tuning.edgeFloor);
  }

  // THE WINDOW DID NOT MOVE UNDER THE FIXTURE. Every coordinate in this gate
  // was computed from `wo` once, at the top; a shift halfway through would
  // strand all of them at once and the failures would name the sweep. Nothing
  // in the tick path here streams, so this is a claim rather than a repair —
  // but it is the claim the old known-failing note guessed at and never made.
  check(world.WindowOrigin().x == pinnedWindow.x &&
            world.WindowOrigin().y == pinnedWindow.y &&
            world.WindowOrigin().z == pinnedWindow.z,
        "the residency window stayed where the fixture was anchored to it");

  // LEAVE THE WORLD AS THIS GATE FOUND IT (CLAUDE.md rule 7). It stands an
  // avatar on real terrain, spawns bodies and carves them; the gates after it
  // place fixtures by ABSOLUTE coordinate and would find the wreckage.
  avatar.Despawn();
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("%d checks", checks);
  std::printf("swing-plane: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// ============================================================================
// player-styles — THE DISCRETE STRIKES, each authored style replayed through
// the PLAYER'S OWN PATH (StepStrokeProgram driving a MeleeState with the
// camera basis, exactly session.cpp's strike tick) on the real
// avatar rig, and asserted AGAINST ITS OWN AUTHORED NUMBERS — the npc-styles
// contract, so a new style entry inherits every check without naming one.
//
// Per style: it spent real ticks cutting; the POSED sword moved; the commanded
// arcs are dominant in the authored
// channel (stated on the COMMAND, not the posed sword — human.json's shoulder
// serves a vertical chop by going round; npc-styles says why at length); and
// THE BLADE NEVER ENTERS THE WIELDER'S OWN HEAD — measured INDEPENDENTLY of
// the driver's keep-out inputs (the circular-probe rule): the physics
// WeaponEdge segment against the head limb's own joint, neither of which the
// clamp in RebuildFrame ever sees.
//
// Aimed at OPEN AIR, no target, no damage sweep: the gate emits zero
// mutations, so it cannot move the world hash by construction.
// ============================================================================
Status GatePlayerStyles(Ctx& c, std::string& detail) {
  // Thresholds in tests/baseline.json (playerStyles.*), seeded from the
  // npcStyles values; --rebaseline records the observed head clearance.
  const float dominanceMin =
      (float)BaselineNumber("playerStyles.dominanceMin", 1.3);
  const float minSweep = (float)BaselineNumber("playerStyles.minSweepRad", 0.25);
  const float minReach = (float)BaselineNumber("playerStyles.minThrustVox", 1.0);
  const float diagRatioMax =
      (float)BaselineNumber("playerStyles.diagRatioMax", 4.0);
  const float thrustSwingMax =
      (float)BaselineNumber("playerStyles.thrustSwingMax", 1.0);
  const float headClearMin =
      (float)BaselineNumber("playerStyles.headClearMin", 1.0);

  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("player-styles: FAILED %s\n", what.c_str());
    }
  };

  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;
  const ItemLibrary& items = c.items;

  // SANDVOX_PS_ITEM names the held item (default the sword): the reported
  // "hand teleports for one frame" was on the DAGGER, whose short blade puts
  // the hand where the sword's never goes.
  const char* psItem = std::getenv("SANDVOX_PS_ITEM");
  const int swordDef = items.Find(psItem && *psItem ? psItem : "sword");
  const ItemDef* sword = items.At(swordDef);
  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  if (!sword || avDef < 0) {
    detail = "no sword item or no avatar def";
    std::printf("player-styles: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  const StyleLibrary& lib = mobs.AttackStyles();
  // The flick compass must resolve, or a click in the shipped game swings
  // nothing — the loader skips bad sectors LOUDLY but only this asserts.
  check(lib.player.Usable(), "attack_styles.json ships a usable player map");
  check(lib.player.sectors.size() >= 4,
        "the flick compass has at least four directions");
  check(lib.player.neutral[0] >= 0 && lib.player.neutral[1] >= 0,
        "both neutral-alternate strikes resolve");

  // ---- STRIKE CHAINING (strokes.h StrikeChains) ----------------------------
  // Pure functions of the shipped compass, so they sit ahead of the fixture.
  // Flicked right, the follow-up that chains is flicked left, plus the two
  // diagonals beside left at leeway 1; right itself, up and down do not.
  {
    const PlayerStrikeMap& m = lib.player;
    const int hr = QuantizeStrike(m, 1, 0), hl = QuantizeStrike(m, -1, 0);
    const int up = QuantizeStrike(m, 0, -1), dn = QuantizeStrike(m, 0, 1);
    const int ul = QuantizeStrike(m, -0.7f, -0.7f);
    const int dl = QuantizeStrike(m, -0.7f, 0.7f);
    const int ur = QuantizeStrike(m, 0.7f, -0.7f);
    check(StrikeChains(m, hr, hl, 1) && StrikeChains(m, hl, hr, 1),
          "chain: the two horizontals chain off each other");
    check(StrikeChains(m, hr, ul, 1) && StrikeChains(m, hr, dl, 1),
          "chain: leeway 1 takes the diagonals beside the opposite side");
    check(!StrikeChains(m, hr, ul, 0) && StrikeChains(m, hr, hl, 0),
          "chain: leeway 0 is dead opposite only");
    check(!StrikeChains(m, hr, hr, 1) && !StrikeChains(m, hr, up, 1) &&
              !StrikeChains(m, hr, dn, 1) && !StrikeChains(m, hr, ur, 1),
          "chain: the same side and the perpendiculars wait the recover out");
    // A SHORT BLADE'S STAB REPEATS on a plain click (`clickRepeat`); a long
    // blade's, a blunt's, a fist's and any other stroke's does not.
    const int fShort = WeaponFormOf("short"), fLong = WeaponFormOf("long");
    check(StrikeRepeats(m, dn, fShort) && !StrikeRepeats(m, dn, fLong) &&
              !StrikeRepeats(m, dn, WeaponFormOf("blunt")) &&
              !StrikeRepeats(m, dn, -1) && !StrikeRepeats(m, hr, fShort),
          "repeat: only the thrust, only with a short blade");
    // ...and the chained windup is the rate faster, never under 2 ticks.
    const AttackStyle* s = lib.At(hl);
    if (s != nullptr) {
      StrokeCursor a, b;
      BeginStrokeProgram(a, *s, hl, 0x5C1Au);
      BeginStrokeProgram(b, *s, hl, 0x5C1Au, 2.0f);
      check(b.windupTicks >= 2 && b.windupTicks <= std::max(2, (a.windupTicks + 1) / 2 + 1),
            "chain: a rate-2 windup is about half the normal one (" +
                std::to_string(a.windupTicks) + " -> " +
                std::to_string(b.windupTicks) + " ticks)");
      check(b.cutTicks == a.cutTicks && b.recoverTicks == a.recoverTicks,
            "chain: only the windup is sped up, not the cut or the recover");
    }
  }

  IdCounterScope idScope(mobs);
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // The swing-plane fixture exactly: axis basis (camera chirality), window-
  // anchored spawn in the right UNITS (chunk centre first, convert once).
  const Vec3 kR{-1, 0, 0}, kU{0, 1, 0}, kF{0, 0, 1};
  const IVec3 wo = world.WindowOrigin();
  const int gx = (wo.x + (int)kNChunk / 2) * (int)kChunk;
  const int gz = (wo.z + (int)kNChunk / 2) * (int)kChunk;
  const int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
  uint32_t t = 23000;

  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, c.mats, &mobs);
  avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
  Player pl;
  pl.fly = false;
  pl.grounded = true;
  pl.pos = Vec3{(float)gx + 0.5f, (float)(gh + 2) + Player::kHalfY,
                (float)gz + 0.5f};
  if (!avatar.Spawn(pl, 0.0f)) {
    detail = "avatar spawn failed";
    std::printf("player-styles: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  check(avatar.EquipItem(sword), "the sword equips into the avatar's hand");

  auto avTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    const IVec3 pc{ifloor(pl.pos.x) >> 4, ifloor(pl.pos.y) >> 4,
                   ifloor(pl.pos.z) >> 4};
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false, pc,
               true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
    avatar.PostStep();
  };
  for (int i = 0; i < 10; i++) avTick();

  // The INDEPENDENT head probe: the head limb's joint off the live physics
  // transform — the keep-out clamp never sees this composition, so agreement
  // here is evidence rather than an identity.
  const int headPart = avatar.PartIndex("head");
  check(headPart >= 0, "the avatar rig has a part named \"head\"");
  auto segPointDist = [](const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const float len2 = ab.dot(ab);
    const float u =
        len2 > 1e-6f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (a + ab * u - p).len();
  };
  // The posed tip about the shoulder, swing-plane's readBlade coordinates.
  const int shoulderPart = avatar.PartIndex("armU.R");
  auto shoulderWorld = [&]() -> Vec3 {
    Vec3 j;
    if (shoulderPart >= 0 && avatar.PartJointWorld(shoulderPart, j)) return j;
    return avatar.Origin();
  };

  MeleeState melee;
  // LIVE tuning, deliberately NOT pinned (contrast swing-plane): the subject
  // is the SHIPPED strike — the authored program composed with the shipped
  // smoothing and the shipped keep-out — because that is what a click fires.
  ApplyMeleeTuning(melee.tuning);
  melee.SetHandSign(avatar.HandSign());

  float headMinOverall = 1e9f;
  for (size_t si = 0; si < lib.styles.size(); si++) {
    const AttackStyle& sty = lib.styles[si];
    melee.Reset();
    ApplyMeleeTuning(melee.tuning);
    melee.SetHandSign(avatar.HandSign());
    StrokeCursor cur;
    // Fixed seed: reproducible, and the player styles author jitter 0 anyway.
    BeginStrokeProgram(cur, sty, (int)si, 0x504C1u + (uint32_t)si);

    float azArc = 0, elArc = 0, prevAz = 0, prevEl = 0;
    bool havePrev = false;
    float cmdAzArc = 0, cmdElArc = 0, cPrevAz = 0, cPrevEl = 0;
    bool cHavePrev = false;
    float rMin = 1e9f, rMax = -1e9f;
    // The POSED tip's distance from the shoulder: a keyed style has no
    // commanded radius (strokes.h "A FRAME IS A POSE"), so a thrust's reach is
    // read off the sword itself.
    float prMin = 1e9f, prMax = -1e9f;
    int cutTicks = 0;
    float headMin = 1e9f;
    // ---- THE ONE-FRAME JUMP, ATTRIBUTED (the "hand teleports above the head
    // for one frame" report). The POSED hand (the rig's hand.R joint) and the
    // COMMANDED hand (melee.HandOffset about the shoulder) are recorded side
    // by side: a posed jump with a smooth command is the rig (IK / hinge /
    // pose limits), a jump in both is the driver.
    const int handPart = avatar.PartIndex("hand.R");
    Vec3 prevPosed{}, prevCmd{};
    bool havePosed = false;
    float jumpPosed = 0, jumpCmdAtPosed = 0, jumpCmd = 0, posedAboveHead = -1e9f;
    int jumpTick = -1, jumpPhase = -1, jumpDrv = -1;

    for (int i = 0; i < 90 && cur.Active(); i++) {
      // main.cpp's discrete tick, in its order: seed the driver from the rig,
      // feed the keep-out, step the program, push the pose, step the world.
      Vec3 hand, tip, flat;
      float reach = 0;
      if (avatar.WeaponStrokePose(hand, tip, flat, reach))
        melee.SetStroke(hand, tip, flat, reach);
      else
        melee.ClearArm();
      {
        Vec3 kc;
        float kr = 0;
        if (avatar.HeadKeepOut(kc, kr))
          melee.SetKeepOut(kc, kr);
        else
          melee.ClearKeepOut();
      }
      const StrokeStepResult r =
          StepStrokeProgram(cur, &sty, melee, 0.0f, 0.0f, 0.0f, kTickDt,
                            kR, kU, kF);
      if (r == StrokeStepResult::Finished) cur.Reset();
      avatar.SetWeaponPose(StrokePoseNow(cur, &sty, melee));
      avTick();

      {
        Vec3 hp, headJ;
        const Vec3 cmd = melee.HandOffset();
        if (handPart >= 0 && avatar.PartJointWorld(handPart, hp) &&
            melee.PoseWeight() > 0.05f) {
          if (havePosed) {
            const float dp = (hp - prevPosed).len();
            const float dc = (cmd - prevCmd).len();
            jumpCmd = std::max(jumpCmd, dc);
            if (dp > jumpPosed) {
              jumpPosed = dp;
              jumpCmdAtPosed = dc;
              jumpTick = i;
              jumpPhase = (int)cur.phase;
              jumpDrv = (int)melee.Phase();
            }
          }
          if (headPart >= 0 && avatar.PartJointWorld(headPart, headJ))
            posedAboveHead = std::max(posedAboveHead, hp.y - headJ.y);
          // SANDVOX_PS_TRACE=<style name>: one line per tick for that style —
          // posed hand RELATIVE TO THE SHOULDER beside the commanded one, the
          // pose weight and wrist alignment, so a one-tick jump names its tick.
          if (const char* tr = std::getenv("SANDVOX_PS_TRACE");
              tr != nullptr && sty.name == tr) {
            const Vec3 sh = shoulderWorld();
            const Vec3 rel = hp - sh;
            const WeaponPose wp = melee.Pose();
            // ...and WHICH STAGE of the rig moved it (mob.h WeaponArmDiag):
            // the IK's miss, then what the anatomy clamp took back.
            const Mob::WeaponArmDiag& wd = avatar.WeaponArmDiagnostics();
            std::printf(
                "ps-trace %s t%02d prog %d drv %d w %.2f steer %.2f posed "
                "(%.2f,%.2f,%.2f) cmd (%.2f,%.2f,%.2f) d %.2f | ikMiss %.2f "
                "clampShift %.2f shoulderClamp %.2f elbowClamp %.2f "
                "elbowAxisTurn %.2f\n",
                sty.name.c_str(), i, (int)cur.phase, (int)melee.Phase(),
                wp.weight, wp.steerAmount, rel.x, rel.y, rel.z, cmd.x, cmd.y,
                cmd.z, havePosed ? (hp - prevPosed).len() : 0.0f, wd.ikMiss,
                wd.clampShift, wd.shoulderClamp, wd.elbowClamp,
                wd.elbowAxisTurn);
          }
          prevPosed = hp;
          prevCmd = cmd;
          havePosed = true;
        } else {
          havePosed = false;
        }
      }

      if (cur.Cutting()) {
        cutTicks++;
        // Posed sword, wrapped-arc accumulation (npc-styles' law: |dAz| is
        // weighted by cos(el) so it is the ARC travelled, not an angle span).
        Vec3 h2, fs, fl2;
        float rr = 0;
        if (avatar.WeaponStrokePose(h2, fs, fl2, rr) && fs.len() > 1e-4f) {
          const float paz = std::atan2(-fs.x, fs.z);
          const float pel =
              std::asin(std::clamp(fs.y / fs.len(), -1.0f, 1.0f));
          if (havePrev) {
            float dAz = paz - prevAz;
            while (dAz > 3.14159265f) dAz -= 6.28318531f;
            while (dAz <= -3.14159265f) dAz += 6.28318531f;
            azArc += std::fabs(dAz) * std::cos(std::clamp(pel, -1.5f, 1.5f));
            elArc += std::fabs(pel - prevEl);
          }
          prevAz = paz;
          prevEl = pel;
          havePrev = true;
          prMin = std::min(prMin, fs.len());
          prMax = std::max(prMax, fs.len());
        }
        const float cr = melee.StrokeRadius();
        rMin = std::min(rMin, cr);
        rMax = std::max(rMax, cr);
        const float ca = melee.StrokeAz(), ce = melee.StrokeEl();
        if (cHavePrev) {
          cmdAzArc +=
              std::fabs(ca - cPrevAz) * std::cos(std::clamp(ce, -1.5f, 1.5f));
          cmdElArc += std::fabs(ce - cPrevEl);
        }
        cPrevAz = ca;
        cPrevEl = ce;
        cHavePrev = true;
      }
      // Head clearance on EVERY tick the arm is claimed, windup included — a
      // windup through the face is exactly as wrong as a cut through it.
      if (melee.PoseWeight() > 0.05f && headPart >= 0) {
        Vec3 eb, et, ef, neck;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef) &&
            avatar.PartJointWorld(headPart, neck))
          headMin = std::min(headMin, segPointDist(eb, et, neck));
      }
    }
    // Hand the arm back before the next style, exactly as main.cpp idles the
    // driver between strikes, so every style starts from a settled take-over.
    for (int i = 0; i < 12; i++) {
      Vec3 hand, tip, flat;
      float reach = 0;
      if (avatar.WeaponStrokePose(hand, tip, flat, reach))
        melee.SetStroke(hand, tip, flat, reach);
      else
        melee.ClearArm();
      melee.Update(kTickDt, false, true, kR, kU, kF);
      avatar.SetWeaponPose(melee.Pose());
      avTick();
    }

    // ---- the style's own claims (npc-styles' branch logic) -----------------
    // Over the WHOLE path: a cut may be several legs (strokes.h "A CUT IS A
    // PATH"), and which channel a stroke travels in is a fact about its total
    // displacement rather than about its opening leg.
    const StrokeSegment cutAll = sty.CutTravel();
    const float wantAz = std::fabs(cutAll.az);
    const float wantEl = std::fabs(cutAll.el);
    const float wantR = std::fabs(cutAll.reach);
    const float dr = rMax > rMin ? rMax - rMin : 0.0f;
    const std::string n = "\"" + sty.name + "\"";
    check(cutTicks >= 2, "style " + n + " spent time cutting");
    // A KEYED style (strokes.h "A FRAME IS A POSE") commands no arc and states
    // no az / el / reach, so the claims below that CLASSIFY a style by its
    // authored travel and read the driver's commanded arc have nothing to
    // read: the poses are the whole of it. What still applies is that the
    // sword itself moved -- sweeping, or reaching -- and the head clearance.
    const bool keyed = sty.Keyed();
    const float posedDr = prMax > prMin ? prMax - prMin : 0.0f;
    check(azArc + elArc > minSweep || (keyed && posedDr > minReach),
          "style " + n + ": the SWORD moved, not just the stroke");
    if (keyed) {
      // (nothing authored to classify)
    } else if (wantR > wantAz && wantR > wantEl) {
      check(dr > minReach, "style " + n + " (thrust) extended its reach");
      // Commanded arcs, npc-styles' restated claim (see _npcStyles_about):
      // the posed sword always sweeps some arc serving a straight-line ask,
      // and the torso lean moves the very shoulder that arc is measured
      // about. "It stayed a thrust" is a claim about the STROKE.
      check(cmdAzArc < thrustSwingMax && cmdElArc < thrustSwingMax,
            "style " + n + " (thrust) did not turn into a swing");
    } else if (wantAz > wantEl * dominanceMin) {
      check(cmdAzArc > minSweep, "style " + n + " swept azimuth");
      check(cmdAzArc > cmdElArc,
            "style " + n + " is azimuth-DOMINANT, as authored");
    } else if (wantEl > wantAz * dominanceMin) {
      check(cmdElArc > minSweep, "style " + n + " swept elevation");
      check(cmdElArc > cmdAzArc,
            "style " + n + " is elevation-DOMINANT, as authored");
    } else {
      check(cmdAzArc > minSweep && cmdElArc > minSweep,
            "style " + n + " (diagonal) swept BOTH channels");
      const float ratio = std::max(cmdAzArc / std::max(cmdElArc, 1e-3f),
                                   cmdElArc / std::max(cmdAzArc, 1e-3f));
      check(ratio < diagRatioMax, "style " + n + " is genuinely diagonal");
    }
    check(headMin >= headClearMin,
          "style " + n + " kept the blade clear of the wielder's own head");
    headMinOverall = std::min(headMinOverall, headMin);
    std::printf(
        "player-styles %-20s hand jump: posed %.2f vox (commanded %.2f that "
        "tick, worst commanded %.2f) at tick %d, program phase %d, driver "
        "phase %d; hand peaked %.2f vox above the head joint\n",
        sty.name.c_str(), jumpPosed, jumpCmdAtPosed, jumpCmd, jumpTick,
        jumpPhase, jumpDrv, posedAboveHead);
    std::printf(
        "player-styles %-20s cmd arc az %.2f el %.2f, posed az %.2f el %.2f, "
        "dr %.2f vox (posed %.2f) over %d cut ticks, head clearance %.2f vox\n",
        sty.name.c_str(), cmdAzArc, cmdElArc, azArc, elArc, dr, posedDr, cutTicks,
        headMin >= 1e9f ? -1.0f : headMin);
  }
  RecordObserved("playerStyles.headClearObserved",
                 headMinOverall >= 1e9f ? -1.0 : (double)headMinOverall);

  // Pristine terrain for whoever runs next, the world-touching convention
  // (rule 7): this gate carved nothing, but ~500 driven ticks settle sand.
  avatar.Despawn();
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("%d checks", checks);
  std::printf("player-styles: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// player-unarmed — the fist compass resolves, and a punch drives the fist
// =============================================================================
//
// `player-styles` above is this gate's armed twin and the two differ in
// exactly one thing: what is on the end of the arm. Everything the player's
// discrete-strike path does with a sword — resolve a flick to a style, seed
// the driver from the live rig, step the program, push the pose — has to work
// with nothing in the fist, and before this package it could not: the whole
// chain started at `heldPartIndex_`, so an empty hand had no edge, no arm
// claim and no way to ask for one.
//
// THE THREE THINGS THAT CAN SILENTLY BREAK, and one claim each:
//
//   1. THE COMPASS. `playerUnarmed` is a second PlayerStrikeMap and main.cpp
//      picks between the two on `meleeArmed`. A sector pointing at a style the
//      avatar cannot swing is a click that does nothing.
//   2. THE EFFECTOR. `ArmForStyle` has to land a Chain effector on the fist
//      the style names, and `WeaponEdge` has to report that fist's knuckles —
//      not the sword slot's (there is none) and not the hand's origin.
//   3. THE ARM. The punch is solved by the same two-bone IK and clamped by the
//      same pose limits as a sword swing, so the hand must stay inside the
//      arm's own reach and the clamp must not be fighting the solve. That is
//      `arm-limits`' subject, asked of a driver it has never seen.
Status GatePlayerUnarmed(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("player-unarmed: FAILED %s\n", what.c_str());
    }
  };

  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Physics& phys = c.phys;
  DebrisSystem& debris = c.debris;
  MobSystem& mobs = c.mobs;

  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  if (avDef < 0) {
    detail = "no avatar def";
    std::printf("player-unarmed: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  const MobDef& ad = mobs.Defs()[avDef];
  const StyleLibrary& lib = mobs.AttackStyles();

  // ---- 1. THE COMPASS, before anything is spawned -------------------------
  check(lib.playerUnarmed.Usable(),
        "attack_styles.json ships a usable playerUnarmed flick map");
  check(lib.playerUnarmed.sectors.size() >= 3,
        "...with at least three directions");
  check(lib.playerUnarmed.neutral[0] >= 0 && lib.playerUnarmed.neutral[1] >= 0,
        "...and both neutral-alternate strikes resolve");
  check(!ad.natural.empty(),
        "the avatar's def declares natural weapons at all");
  std::vector<int> unarmedStyles;
  for (const PlayerStrikeMap::Sector& s : lib.playerUnarmed.sectors) {
    const AttackStyle* sty = lib.At(s.style);
    if (sty == nullptr) continue;
    // FISTS THROW THE DIRECTIONAL STRIKES (2026-09-27): a sector may name a
    // `held` style, which an empty hand throws with its own fist
    // (Mob::ArmForStyle); anything else must be a natural weapon the rig has.
    const bool held = sty->weapon.empty() || sty->weapon == "held";
    check(held || ad.FindNatural(sty->weapon) >= 0,
          "playerUnarmed sector -> \"" + sty->name +
              "\" is held (thrown with a fist) or names a natural weapon the "
              "avatar rig declares");
    // THE STYLE THE PRESS ACTUALLY SWINGS: the empty hand's `unarmed` form
    // (strokes.h FormForItem), resolved exactly as the player's press does.
    // A held sector must have one authored -- otherwise the fist swings the
    // sword's poses and nothing an author tunes for fists would reach it.
    const int formed = lib.ResolveForm(s.style, FormForItem(nullptr));
    if (held)
      check(formed != s.style,
            "...and \"" + sty->name + "\" has an authored forms.unarmed");
    if (std::find(unarmedStyles.begin(), unarmedStyles.end(), formed) ==
        unarmedStyles.end())
      unarmedStyles.push_back(formed);
  }
  // EVERY NATURAL WEAPON'S EDGE IS REAL. A degenerate one (from == to) has no
  // direction for the driver to steer and no segment for the sweep to sweep,
  // and the only symptom is a punch that connects with nothing.
  for (const MobNaturalWeaponDef& nw : ad.natural) {
    check(nw.partIndex >= 0,
          "natural weapon \"" + nw.name + "\" names a live part");
    check((nw.edgeTo - nw.edgeFrom).len() > 1e-3f,
          "natural weapon \"" + nw.name + "\" has a non-degenerate edge");
    check(nw.edgeHalfWidth > 0.0f, "...and a carve width");
  }
  if (unarmedStyles.empty()) {
    detail = "no usable playerUnarmed style";
    std::printf("player-unarmed: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }

  IdCounterScope idScope(mobs);
  debris.Reset();
  mobs.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // The swing-plane / player-styles fixture exactly: axis basis (camera
  // chirality), window-anchored spawn in the right UNITS.
  const Vec3 kR{-1, 0, 0}, kU{0, 1, 0}, kF{0, 0, 1};
  const IVec3 wo = world.WindowOrigin();
  const int gx = (wo.x + (int)kNChunk / 2) * (int)kChunk;
  const int gz = (wo.z + (int)kNChunk / 2) * (int)kChunk;
  const int gh = World::TerrainHeight(gx, gz, kDefaultSeed);
  uint32_t t = 24200;

  PlayerAvatar avatar;
  avatar.Init(&phys, &world, &debris, c.mats, &mobs);
  avatar.SetDefs(&mobs.Defs(), kAvatarDefName);
  Player pl;
  pl.fly = false;
  pl.grounded = true;
  pl.pos = Vec3{(float)gx + 0.5f, (float)(gh + 2) + Player::kHalfY,
                (float)gz + 0.5f};
  if (!avatar.Spawn(pl, 0.0f)) {
    detail = "avatar spawn failed";
    std::printf("player-unarmed: SKIP (%s)\n", detail.c_str());
    return Status::Skip;
  }
  // NOTHING IS EQUIPPED, and that is the fixture: `player-styles` puts a sword
  // in this same hand, so the pair is a differential over one variable.
  check(avatar.HeldSlot() < 0, "the avatar is holding nothing");

  auto avTick = [&]() {
    std::vector<BrushOp> ops;
    std::vector<ParticleSpawn> spawns;
    std::vector<CellOp> cellOps;
    avatar.PreTick(t + 1, pl, 0.0f, kTickDt, world, ops, cellOps, spawns);
    mobs.PreTick(t + 1, world, ops, cellOps, spawns);
    debris.QueueSupportEvents(world.Snap());
    debris.PreTick(t + 1, world, cellOps, spawns);
    ++t;
    const IVec3 pc{ifloor(pl.pos.x) >> 4, ifloor(pl.pos.y) >> 4,
                   ifloor(pl.pos.z) >> 4};
    SubmitTick(ctx, world, sim, t, kDefaultSeed, ops, {}, cellOps, false, pc,
               true, false, spawns);
    ctx.WaitIdle();
    ctx.ProcessEvents();
    phys.Step(kTickDt);
    debris.PostStep();
    mobs.PostStep();
    avatar.PostStep();
  };
  for (int i = 0; i < 10; i++) avTick();

  const int headPart = avatar.PartIndex("head");
  auto segPointDist = [](const Vec3& a, const Vec3& b, const Vec3& p) {
    const Vec3 ab = b - a;
    const float len2 = ab.dot(ab);
    const float u =
        len2 > 1e-6f ? std::clamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
    return (a + ab * u - p).len();
  };

  MeleeState melee;
  ApplyMeleeTuning(melee.tuning);
  melee.SetHandSign(avatar.HandSign());

  const float minSweep =
      (float)BaselineNumber("playerUnarmed.minSweepRad", 0.20);
  const float minEdgeTravel =
      (float)BaselineNumber("playerUnarmed.minEdgeTravelVox", 0.8);
  const float headClearMin =
      (float)BaselineNumber("playerUnarmed.headClearMin", 0.5);
  const float maxClampRad =
      (float)BaselineNumber("playerUnarmed.maxClampRad", 1.2);
  const float maxIkMiss =
      (float)BaselineNumber("playerUnarmed.maxIkMissVox", 2.5);
  float headMinOverall = 1e9f;

  for (int si : unarmedStyles) {
    const AttackStyle& sty = *lib.At(si);
    melee.Reset();
    ApplyMeleeTuning(melee.tuning);
    melee.SetHandSign(avatar.HandSign());

    // ---- 2. THE EFFECTOR ---------------------------------------------------
    check(avatar.ArmForStyle(sty),
          "style \"" + sty.name + "\" arms an effector on the avatar");
    // A `held` style on an empty hand arms THAT HAND'S fist; a fist style
    // arms the weapon it names.
    const MobNaturalWeaponDef* nw = avatar.EffectorWeapon();
    const bool heldStyle = sty.weapon.empty() || sty.weapon == "held";
    const MobNaturalWeaponDef* want =
        heldStyle ? avatar.HandFist(avatar.StrikeHand()) : nullptr;
    check(nw != nullptr &&
              (heldStyle ? nw == want : nw->name == sty.weapon),
          "...which is the fist the style resolves to");
    check(avatar.StrikeEffectorKind() == StrikeEffectorMode::Chain,
          "...on a CHAIN effector (a fist is on an arm, not aimed like jaws)");
    Vec3 eb0, et0, ef0;
    float ehw0 = 0;
    check(avatar.WeaponEdge(eb0, et0, ehw0, &ef0),
          "...and WeaponEdge reports that fist's knuckles");
    check((et0 - eb0).len() > 1e-3f, "...as a real segment");
    check(ef0.len() < 1e-4f,
          "...with NO flat: a fist cannot land edge-on (MeleeEdgeAlign reads "
          "a zero vector as 'no evidence', not as a bad angle)");

    StrokeCursor cur;
    BeginStrokeProgram(cur, sty, si, 0x504D1u + (uint32_t)si);

    float cmdAzArc = 0, cmdElArc = 0, cPrevAz = 0, cPrevEl = 0;
    bool cHavePrev = false;
    float rMin = 1e9f, rMax = -1e9f;
    float edgeTravel = 0, worstClamp = 0, worstMiss = 0;
    Vec3 prevTip{};
    bool havePrevTip = false;
    int cutTicks = 0;
    float headMin = 1e9f;

    for (int i = 0; i < 90 && cur.Active(); i++) {
      // main.cpp's discrete tick, in its order.
      Vec3 hand, tip, flat;
      float reach = 0;
      if (avatar.WeaponStrokePose(hand, tip, flat, reach))
        melee.SetStroke(hand, tip, flat, reach);
      else
        melee.ClearArm();
      {
        Vec3 kc;
        float kr = 0;
        if (avatar.HeadKeepOut(kc, kr))
          melee.SetKeepOut(kc, kr);
        else
          melee.ClearKeepOut();
      }
      const StrokeStepResult r =
          StepStrokeProgram(cur, &sty, melee, 0.0f, 0.0f, 0.0f, kTickDt,
                            kR, kU, kF);
      if (r == StrokeStepResult::Finished) cur.Reset();
      avatar.SetWeaponPose(StrokePoseNow(cur, &sty, melee));
      avTick();

      // ONLY WHILE THE ARM IS FULLY CLAIMED. `ikMiss` is measured after a
      // solve that AnimSolveTwoBone scaled by `chain.weight * weaponWeight_`,
      // and `weaponWeight_` is the driver's PoseWeight ramping in from zero —
      // so on the first ticks of a take-over the hand is deliberately part of
      // the way to its target and the "miss" is the ramp, not the arm. Read at
      // full weight it is the claim it says it is: the arm can serve this
      // punch. (Measured: sampling every tick reported 3.2-5.8 voxels on
      // strokes whose settled miss is a fraction of one.)
      const Mob::WeaponArmDiag& d = avatar.WeaponArmDiagnostics();
      if (d.ran && melee.PoseWeight() > 0.9f) {
        worstClamp = std::max(worstClamp, d.clampMove);
        worstMiss = std::max(worstMiss, d.ikMiss);
      }
      if (cur.Cutting()) {
        cutTicks++;
        const float cr = melee.StrokeRadius();
        rMin = std::min(rMin, cr);
        rMax = std::max(rMax, cr);
        const float ca = melee.StrokeAz(), ce = melee.StrokeEl();
        if (cHavePrev) {
          cmdAzArc +=
              std::fabs(ca - cPrevAz) * std::cos(std::clamp(ce, -1.5f, 1.5f));
          cmdElArc += std::fabs(ce - cPrevEl);
        }
        cPrevAz = ca;
        cPrevEl = ce;
        cHavePrev = true;
        // THE KNUCKLES, IN THE WORLD. The commanded arc says the driver
        // asked; this says the FIST went — which is the claim that fails if
        // the effector never reached the rig (a stroke driving nothing looks
        // perfect from the cursor's side).
        Vec3 eb, et, ef;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef)) {
          if (havePrevTip) edgeTravel += (et - prevTip).len();
          prevTip = et;
          havePrevTip = true;
        }
      }
      // Head clearance on EVERY tick the arm is claimed, windup included. The
      // keep-out still applies here: the effector is a hand, not the head, so
      // `HeadKeepOut` reports rather than declining (mob.cpp says why it would
      // decline for jaws).
      if (melee.PoseWeight() > 0.05f && headPart >= 0) {
        Vec3 eb, et, ef, neck;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef) &&
            avatar.PartJointWorld(headPart, neck))
          headMin = std::min(headMin, segPointDist(eb, et, neck));
      }
    }
    // Hand the arm back before the next style, exactly as main.cpp idles the
    // driver between strikes.
    avatar.ClearStrikeEffector();
    for (int i = 0; i < 12; i++) {
      melee.Update(kTickDt, false, true, kR, kU, kF);
      avatar.SetWeaponPose(melee.Pose());
      avTick();
    }

    const float dr = rMax > rMin ? rMax - rMin : 0.0f;
    const std::string n = "\"" + sty.name + "\"";
    check(cutTicks >= 2, "style " + n + " spent time cutting");
    // A KEYED style (frames are poses) moves the arm by its joints, not by
    // the driver's az/el channels, so its evidence is the knuckles' own
    // travel -- the same allowance player-styles makes for the sword.
    check(cmdAzArc + cmdElArc + dr > minSweep ||
              (sty.Keyed() && edgeTravel > minEdgeTravel),
          "style " + n + ": the stroke actually drove the arm");
    // ---- 3. THE ARM ------------------------------------------------------
    check(edgeTravel > minEdgeTravel,
          "style " + n + ": the FIST moved through the world, not just the "
                         "cursor");
    check(worstMiss < maxIkMiss,
          "style " + n + ": the arm could serve the punch (IK miss in bounds)");
    check(worstClamp < maxClampRad,
          "style " + n + ": the pose clamp was not fighting the solve");
    check(headMin >= headClearMin,
          "style " + n + " kept the fist clear of the wielder's own head");
    headMinOverall = std::min(headMinOverall, headMin);
    std::printf(
        "player-unarmed %-22s weapon %-7s cmd arc az %.2f el %.2f, dr %.2f "
        "vox, knuckles travelled %.2f vox over %d cut ticks; ik miss %.2f, "
        "clamp %.3f rad, head clearance %.2f vox\n",
        sty.name.c_str(), sty.weapon.c_str(), cmdAzArc, cmdElArc, dr,
        edgeTravel, cutTicks, worstMiss, worstClamp,
        headMin >= 1e9f ? -1.0f : headMin);
  }
  RecordObserved("playerUnarmed.headClearObserved",
                 headMinOverall >= 1e9f ? -1.0 : (double)headMinOverall);

  // ---- 4. THE AIM: A PUNCH LANDS ON WHAT THE CROSSHAIR IS ON --------------
  //
  // THE FOURTH SILENT BREAK, and the one the three above cannot see: every
  // check so far is aimed at OPEN AIR — liveAz/liveEl/liveDist all zero — so a
  // punch that drives the fist beautifully in entirely the wrong DIRECTION
  // passes all of them. That was the shipped bug: main.cpp passed (0, 0, 0)
  // because "the camera IS the aim", which is true of a direction and false of
  // a blow. The stroke is a bearing about the ARM'S PIVOT, and the shoulder
  // sits a couple of voxels below the eye and a couple to the side, so a
  // bearing copied from the camera put the knuckles a whole shoulder offset
  // low and wide of the thing under the crosshair — the difference between a
  // head and a collarbone at punching range.
  //
  // A DIFFERENTIAL, not an absolute (rule 6): the same punch twice at the same
  // fixed world point, once aimed at it through `StrokeAimAt` and once with
  // the old camera-parallel (0, 0, 0), reported as the closest the KNUCKLES
  // came to the point. One number alone would answer "does a punch go near a
  // target" with something that depends on where the fixture put the target;
  // the pair answers "does aiming DO anything", which is the claim.
  {
    const int si = lib.Find("player_punch_r");
    const AttackStyle* sty = lib.At(si);
    Vec3 eyeish{};
    if (sty != nullptr && headPart >= 0 &&
        avatar.PartJointWorld(headPart, eyeish)) {
      // Four voxels down the crosshair line FROM THE HEAD: inside the arm's
      // own band, and a full shoulder offset off the line the camera-parallel
      // aim drives the fist along. Fixed before either run so both are scored
      // against the same point.
      const Vec3 target = eyeish + kF * 4.0f;
      auto runAt = [&](bool aimed) -> float {
        melee.Reset();
        ApplyMeleeTuning(melee.tuning);
        melee.SetHandSign(avatar.HandSign());
        avatar.ArmForStyle(*sty);
        StrokeCursor cur;
        BeginStrokeProgram(cur, *sty, si, 0x504E1u);
        float best = 1e9f;
        for (int i = 0; i < 90 && cur.Active(); i++) {
          Vec3 hand, tip, flat;
          float reach = 0;
          if (avatar.WeaponStrokePose(hand, tip, flat, reach))
            melee.SetStroke(hand, tip, flat, reach);
          else
            melee.ClearArm();
          Vec3 kc;
          float kr = 0;
          if (avatar.HeadKeepOut(kc, kr))
            melee.SetKeepOut(kc, kr);
          else
            melee.ClearKeepOut();
          // main.cpp's strike tick: the pivot off the live rig, the bearing to
          // the aim point in the swing basis. The control arm passes the zeros
          // it used to.
          float az = 0, el = 0, dist = 0;
          Vec3 pivot;
          if (aimed && avatar.StrokePivotWorld(pivot))
            StrokeAimAt(pivot, target, kR, kU, kF, az, el, dist);
          const StrokeStepResult r = StepStrokeProgram(
              cur, sty, melee, az, el, dist, kTickDt, kR, kU, kF);
          if (r == StrokeStepResult::Finished) cur.Reset();
          avatar.SetWeaponPose(StrokePoseNow(cur, sty, melee));
          avTick();
          Vec3 eb, et, ef;
          float ehw = 0;
          if (avatar.WeaponEdge(eb, et, ehw, &ef))
            best = std::min(best, segPointDist(eb, et, target));
        }
        avatar.ClearStrikeEffector();
        for (int i = 0; i < 12; i++) {
          melee.Update(kTickDt, false, true, kR, kU, kF);
          avatar.SetWeaponPose(melee.Pose());
          avTick();
        }
        return best >= 1e9f ? -1.0f : best;
      };
      const float blind = runAt(false);
      const float aimed = runAt(true);
      const float missMax =
          (float)BaselineNumber("playerUnarmed.aimMissVox", 1.6);
      const float gainMin =
          (float)BaselineNumber("playerUnarmed.aimGainVox", 0.5);
      check(aimed >= 0.0f && aimed < missMax,
            "an AIMED punch puts the knuckles on the point the crosshair is "
            "on");
      check(blind >= 0.0f && aimed < blind - gainMin,
            "...and closer to it than the camera-parallel aim it replaced");
      RecordObserved("playerUnarmed.aimMissObserved", (double)aimed);
      RecordObserved("playerUnarmed.aimBlindObserved", (double)blind);
      std::printf(
          "player-unarmed aim              knuckles came %.2f vox from the aim "
          "point (camera-parallel: %.2f)\n",
          aimed, blind);
    }
  }

  // Pristine terrain for whoever runs next (rule 7).
  avatar.Despawn();
  mobs.Reset();
  debris.Reset();
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("%d checks", checks);
  std::printf("player-unarmed: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// swing-smooth — THE WEAPON MOVES SMOOTHLY AT EVERY POINT OF EVERY STYLE
// =============================================================================
//
// THE REPORT THIS EXISTS FOR, in the owner's words: "many of them will look
// good for like 60% of the animation and then the sword will spin / clip
// through things / teleport around super crazy fast."
//
// Every gate in this file before it asserts on where a stroke ENDS UP — the
// arc it swept, the channel it travelled in, the pose it started from. None of
// them could see a stroke that arrives in the right place by a route with a
// step in it, and all three of the faults behind that report were exactly
// that: a one-tick discontinuity somewhere in the middle or at the very end.
//
// So this one asserts on the DERIVATIVE, per tick, over every authored style
// replayed through the shared runner:
//
//   * the HAND's travel may not CHANGE much from one tick to the next
//   * nor the TIP's
//   * the blade's DIRECTION may not turn more than so far in one tick
//   * the blade's ROLL may not either — this is the one that caught the flat's
//     sign flip, which is invisible in position and is the whole "spin"
//   * the arm CLAIM may not step — PoseWeight, which the rig blends the entire
//     IK solve by, so a 1 -> 0 step is the arm teleporting to the walk cycle
//
// The ceilings are in tests/baseline.json so tuning them costs no rebuild, and
// each failure prints the STYLE, the TICK, the PHASE and the measurement, so
// a red line says where to look rather than that something is wrong.
//
// CPU-ONLY AND ASSET-LIGHT: it loads attack_styles.json (the styles ARE the
// subject) and nothing else — no world, no rig, no GPU. Milliseconds.
//
// SMOOTHING STAYS AT ITS AUTHORED VALUES here, unlike every fixture above.
// The other blocks turn it off because they assert on the mapping and the
// easing is lag; this one asserts on CONTINUITY, and the easing is half of
// what delivers it. Measuring the shipped feel means measuring the shipped
// knobs.
Status GateSwingSmooth(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("swing-smooth: FAILED %s\n", what.c_str());
    }
  };

  StyleLibrary lib;
  std::string log;
  if (!LoadAttackStyles(AssetDir() + "/mobs/attack_styles.json", lib, log) ||
      lib.empty()) {
    detail = "attack_styles.json did not load";
    std::printf("swing-smooth: SKIP (%s)\n%s", detail.c_str(), log.c_str());
    return Status::Skip;
  }

  // The ceilings. Per TICK at 30 Hz, which is the rate both live feeders run
  // the runner at (MobSystem::StepStroke's fixed 1/30, session.cpp's kTickDt).
  //
  // ---- WHY POSITION IS MEASURED AS AN ACCELERATION AND ANGLE IS NOT -------
  //
  // A committed cut legitimately moves the point six voxels in a thirtieth of
  // a second — that is 180 vox/s, and `melee.fullSpeedMps` says full damage
  // starts at 200. So a ceiling on a SINGLE tick's travel cannot separate "a
  // fast sword" from "a teleport", and one tight enough to catch the second
  // forbids the first. What distinguishes them is whether the speed ARRIVED:
  // a real swing accelerates over several ticks, a discontinuity changes the
  // travel-per-tick by the whole of it in one. So hand and point are bounded
  // on the SECOND difference.
  //
  // The two ANGLES are not, and the asymmetry is real rather than an
  // inconsistency. A blade's direction and roll have no equivalent of a
  // committed cut's speed: the stroke only ever turns the blade as fast as the
  // arm carries it, so a large first difference there IS the defect — it is
  // the lean plane spinning about a stalled point, or the flat's sign
  // flipping, both of which arrive at full size on one tick and leave on the
  // next. Bounding their acceleration would let a 180-degree roll through as
  // long as it took two ticks to do it.
  const float maxHand =
      (float)BaselineNumber("swingSmooth.maxHandAccelVox", 1.30);
  const float maxTip =
      (float)BaselineNumber("swingSmooth.maxTipAccelVox", 2.60);
  const float maxDir = (float)BaselineNumber("swingSmooth.maxDirStepRad", 0.90);
  const float maxRoll =
      (float)BaselineNumber("swingSmooth.maxRollStepRad", 0.90);
  const float maxWeight =
      (float)BaselineNumber("swingSmooth.maxWeightStep", 0.20);
  // ---- AND THE ROLL'S WHOLE JOURNEY, not just its worst step -------------
  //
  // A per-tick ceiling cannot see the defect this gate was written for. The
  // blade's flat is `bladeDir x travel`, whose SIGN reverses when the travel
  // does — every cut, at its end — and the rig then rolls the sword through
  // 180 degrees to follow it. Smoothed on `bladeSmoothing`, that flip takes
  // three or four ticks, so no single tick's step is remarkable and the total
  // is a whole pi of roll the stroke never asked for. Summing the per-tick
  // turn is what catches it: a cut that keeps its edge spends well under a
  // radian of roll over the whole swing, and one that flips spends pi more.
  const float maxRollArc =
      (float)BaselineNumber("swingSmooth.maxRollArcRad", 1.60);

  const float dt = 1.0f / 30.0f;
  // A HELD BLADE'S GEOMETRY, not a bare arm: the lean machinery — which is
  // where two of the three faults lived — only exists when `bladeLen_` is
  // non-zero, and a fixture reporting its tip at its hand would exercise none
  // of it. 5.5 voxels is the shipped sword (assets/items/sword.json's edge at
  // 10 cm voxels), stated here rather than loaded because the claim is about
  // the DRIVER and not about this week's art.
  const Vec3 fixtureHand{-0.4f, -3.0f, -0.3f};   // x mirrored with the basis
  const Vec3 fixtureTip = fixtureHand + Vec3{0, 5.5f, 0};

  auto angleBetween = [](const Vec3& a, const Vec3& b) {
    if (a.len() < 1e-5f || b.len() < 1e-5f) return 0.0f;
    return std::acos(
        std::clamp(a.normalized().dot(b.normalized()), -1.0f, 1.0f));
  };
  const char* kPhase[] = {"idle", "guard", "windup", "cut", "recover"};

  int styles = 0;
  float worstHand = 0, worstTip = 0, worstDir = 0, worstRoll = 0, worstW = 0;
  float worstRollArc = 0;
  int totalFlips = 0;
  for (const AttackStyle& sty : lib.styles) {
    // ONLY THE STYLES THE DRIVER CAN REPLAY WITHOUT A RIG. `StyleUsable` needs
    // a Mob; here the question is narrower and is answered by the style: a
    // natural weapon's stroke is driven through a part the fixture does not
    // have, and a bite is an AIM effector whose whole motion happens in a
    // channel this bladeless fixture cannot express. The held styles are the
    // ones the owner's report is about and the ones with a blade to spin.
    if (!(sty.weapon.empty() || sty.weapon == "held")) continue;
    styles++;

    MeleeState m;
    ApplyMeleeTuning(m.tuning);
    m.SetStroke(fixtureHand, fixtureTip, Vec3{0, 0, 1}, kRestReach);
    m.SetHandSign(1.0f);

    StrokeCursor cur;
    // A FIXED SEED off the style's NAME, for the reason npc-styles pins its
    // own: the tempo jitter scales the tick counts, so an unpinned seed makes
    // "the same style" a different number of ticks from run to run.
    BeginStrokeProgram(cur, sty, lib.Find(sty.name),
                       (uint32_t)std::hash<std::string>{}(sty.name) | 1u);

    Vec3 prevHand{}, prevTip{}, prevDir{}, prevFlat{};
    float prevW = 0;
    float lastHandStep = 0, lastTipStep = 0;
    bool have = false, haveStep = false;
    bool hitHand = false, hitTip = false, hitDir = false, hitRoll = false,
         hitW = false;
    float rollArc = 0, dirArc = 0;
    int rollFlips = 0;
    for (int i = 0; i < 200; i++) {
      const StrokeCursor::Phase ph = cur.phase;
      const StrokeStepResult r =
          StepStrokeProgram(cur, &sty, m, 0.0f, 0.0f, 0.0f, dt, kRight, kUp,
                            kFwd);
      if (r == StrokeStepResult::Idle) break;
      const WeaponPose p = m.Pose();
      if (have) {
        const float dHand = (p.hand - prevHand).len();
        const float dTip = (m.TipOffset() - prevTip).len();
        const float dDir = angleBetween(p.bladeDir, prevDir);
        // ---- ROLL IS ROTATION ABOUT THE BLADE, NOT THE FLAT'S TRAVEL ----
        //
        // The naive angle between consecutive flats is dominated by the blade
        // TURNING: the flat is perpendicular to the blade by construction, so
        // a 2.5-radian sweep carries it 2.5 radians whether or not the sword
        // rolled at all in the wielder's fist. Measured that way every style
        // reported ~2.3 rad of "roll" and the number said nothing.
        //
        // PARALLEL-TRANSPORT the previous flat along the blade's own turn
        // first — the minimal rotation taking the old direction to the new one
        // — and what is left is the rotation about the blade axis, which is
        // the only thing a wrist is doing and the only thing a sign flip
        // shows up in.
        const Vec3 carried =
            QuatRotate(QuatFromTo(prevDir, p.bladeDir), prevFlat);
        const float dRoll = angleBetween(p.bladeFlat, carried);
        const float dW = std::fabs(p.weight - prevW);
        // The SECOND difference: how much the travel-per-tick changed. Zero on
        // a constant-speed sweep however fast, and the whole of the step on a
        // discontinuity.
        const float aHand = haveStep ? std::fabs(dHand - lastHandStep) : 0.0f;
        const float aTip = haveStep ? std::fabs(dTip - lastTipStep) : 0.0f;
        lastHandStep = dHand;
        lastTipStep = dTip;
        haveStep = true;
        worstHand = std::max(worstHand, aHand);
        worstTip = std::max(worstTip, aTip);
        worstDir = std::max(worstDir, dDir);
        worstRoll = std::max(worstRoll, dRoll);
        worstW = std::max(worstW, dW);
        rollArc += dRoll;
        dirArc += dDir;
        // ---- AND THE SHARP ONE: DID THE FLAT REVERSE? --------------------
        //
        // Roll ARC is a blunt instrument — a cut legitimately rolls the edge
        // round to lead its travel, and on these styles that is two radians of
        // honest wrist. What is never legitimate is the flat pointing out of
        // the OTHER FACE of the blade from one tick to the next, because a
        // flat normal names a plane and its sign carries no information at
        // all (MeleeEdgeAlign takes fabs of it). A reversal is the commanded
        // normal having jumped to its own negative, which the rig then spends
        // several ticks rolling the sword through 180 degrees to follow.
        //
        // COUNTED AGAINST THE PARALLEL-TRANSPORTED flat, so the blade turning
        // through a right angle is not mistaken for one.
        if (carried.dot(p.bladeFlat) < 0.0f) rollFlips++;
        // ---- AND WHAT ELSE WAS TRUE ON THAT TICK (CLAUDE.md rule 6) -------
        //
        // "The hand jumped by 2.3 voxels" has at least three causes and from
        // outside they are one number: the POINT jumped and the hand followed
        // it, the blade TURNED and swung the hand round the point (hand = tip
        // minus a blade, so 0.5 rad on a 5.5-voxel blade is 2.75 voxels of
        // hand with the point perfectly still), or the lean angle moved
        // because the RADIUS did and the law of cosines is steep near the ends
        // of the reach band. Printing the co-measurements is one line and
        // saves an elimination run per hypothesis.
        auto where = [&](const char* what, float got, float lim) {
          return Format(
              "%s: %s %.3f (limit %.3f) at tick %d, phase %s | point step "
              "%.3f, dir turn %.3f rad, roll %.3f rad, radius %.2f, steer "
              "%.2f",
              sty.name.c_str(), what, got, lim, i,
              kPhase[(int)ph <= 4 ? (int)ph : 0], dTip, dDir, dRoll,
              m.StrokeRadius(), m.SteerAmount());
        };
        // ONE CHECK PER STYLE PER CHANNEL, not one per tick: a broken style
        // breaks on many ticks and five hundred identical FAILED lines bury
        // the one that says which style it was. `firstHand`/... latch that.
        if (aHand > maxHand && !hitHand) {
          hitHand = true;
          check(false, where("the hand's travel jumped by", aHand, maxHand));
        }
        if (aTip > maxTip && !hitTip) {
          hitTip = true;
          check(false, where("the point's travel jumped by", aTip, maxTip));
        }
        if (dDir > maxDir && !hitDir) {
          hitDir = true;
          check(false, where("the blade direction turned", dDir, maxDir));
        }
        if (dRoll > maxRoll && !hitRoll) {
          hitRoll = true;
          check(false, where("the blade rolled", dRoll, maxRoll));
        }
        if (dW > maxWeight && !hitW) {
          hitW = true;
          check(false, where("the arm claim stepped", dW, maxWeight));
        }
      }
      prevHand = p.hand;
      prevTip = m.TipOffset();
      prevDir = p.bladeDir;
      prevFlat = p.bladeFlat;
      prevW = p.weight;
      have = true;
      if (r == StrokeStepResult::Finished) {
        // ---- AND THE LAST TICK IS NOT A CLIFF -----------------------------
        //
        // `Finished` is the one tick where the CALLER drops the pose claim
        // outright (MobSystem::StepStroke pushes an empty WeaponPose), so
        // whatever weight the driver still had at that instant is a step the
        // rig takes in a single frame. Checked separately from the per-tick
        // ceiling above because the step happens OUTSIDE the driver and no
        // amount of smoothing inside it can cover one.
        checks++;
        if (p.weight > maxWeight) {
          ok = false;
          std::printf(
              "swing-smooth: FAILED %s: the program finished while the arm "
              "was still claimed at %.3f (limit %.3f) — the caller drops the "
              "claim on this tick, so that is a one-frame snap to the walk "
              "pose\n",
              sty.name.c_str(), p.weight, maxWeight);
        }
        break;
      }
    }
    check(have, sty.name + ": the program stepped at all");
    // THE ROLL IS NOT A SECOND SWING. Stated against the DIRECTION's own arc
    // as well as an absolute ceiling, because the two failures are different:
    // an absolute bound catches a flip on a short stroke, and the ratio
    // catches one on a long sweep whose direction legitimately travels two
    // radians. A blade that keeps its edge rolls a fraction of what it swings.
    worstRollArc = std::max(worstRollArc, rollArc);
    totalFlips += rollFlips;
    check(rollFlips == 0,
          Format("%s: the blade's flat reversed %d time(s) — the commanded "
                 "normal jumped to its own negative, which is a 180-degree "
                 "roll of the sword that no part of the stroke asked for",
                 sty.name.c_str(), rollFlips));
    check(rollArc <= maxRollArc,
          Format("%s: the blade rolled %.3f rad over the whole stroke (limit "
                 "%.3f) while its direction turned %.3f — a roll that large "
                 "beside a swing that size is the flat's sign having reversed, "
                 "not the edge leading the cut",
                 sty.name.c_str(), rollArc, maxRollArc, dirArc));
  }
  check(styles >= 5, "the library ships held styles to measure");

  std::printf(
      "swing-smooth: %d styles | worst: hand accel %.3f/%.2f vox, point accel "
      "%.3f/%.2f vox, dir %.3f/%.2f rad/tick, roll %.3f/%.2f rad/tick, claim "
      "step %.3f/%.2f\n",
      styles, worstHand, maxHand, worstTip, maxTip, worstDir, maxDir,
      worstRoll, maxRoll, worstW, maxWeight);
  detail = Format("%d styles, %d checks", styles, checks);
  std::printf("swing-smooth: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// =============================================================================
// cut-path — A CUT IS A PATH, AND THE PATH IS DRIVEN (strokes.h, 2026-09-21)
//
// `cut` may be a LIST OF LEGS run back to back inside the one Cut phase, so a
// stroke can hook, dogleg or hitch instead of being a chord through the
// target. Nothing in the shipped library uses one yet — the feature is an
// AUTHORING surface, and the styles that will use it are authored in the
// tuner — which is exactly why it needs a gate of its own: a path that parsed
// and then ran as a straight line would look, from every other gate in this
// file, like a library nobody had got round to editing.
//
// FOUR CLAIMS, and each is the one whose failure would be silent:
//
//   1. THE OLD SPELLING IS THE OLD PROGRAM. A bare `"cut": {…}` and a
//      one-element list must be the same stroke tick for tick, or every style
//      in the library quietly changed.
//   2. THE CORNER IS REALLY DRIVEN. Two legs summing to a straight cut's
//      travel must END where it ends and go somewhere ELSE on the way —
//      arriving in the right place is what a straight line already does.
//   3. THE AIM MARKER MOVES THE TARGET ALONG THE PATH. `"aim": true` on leg 2
//      has to make the blade meet the target later; a marker that parsed and
//      did nothing reads identically from outside.
//   4. THE LOADER REFUSES WHAT IT SAYS IT REFUSES — too many legs, an empty
//      list, a path with no motion in it — because those are the messages an
//      author actually meets.
//
// CPU-ONLY AND WORLD-FREE, like `swing` and `swing-smooth` beside it: its
// styles are built in memory and its loader probe is a temp file, so it costs
// milliseconds, disturbs nothing, and `--gate cut-path` alone is the whole
// verification loop for a change to the path arithmetic.
Status GateCutPath(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("cut-path: FAILED %s\n", what.c_str());
    }
  };

  const float dt = 1.0f / 30.0f;
  // swing-smooth's fixture, for its reasons: a HELD BLADE's geometry, because
  // the reach band a leg's `reach` is a position in only exists when there is
  // a blade to measure.
  const Vec3 fixtureHand{-0.4f, -3.0f, -0.3f};
  const Vec3 fixtureTip = fixtureHand + Vec3{0, 5.5f, 0};
  const float kAimAz = 0.25f;

  struct Sample {
    StrokeCursor::Phase phase;
    float az, el, r;
  };
  // One style, run to completion against a fresh driver, sampled every tick.
  // The phase is read BEFORE the step for the reason test_melee.mjs states:
  // StepStrokeProgram advances and transitions inside the one call.
  // `legacy`: the hand-built styles below are authored in the PRE-2026-09-25
  // stroke frame (strokes.h "THE STROKE FRAME") and are converted exactly as
  // the loader converts an old file, so every number this gate asserts keeps
  // its meaning. Styles that came through the loader are already converted.
  auto run = [&](const AttackStyle& styIn, std::vector<Sample>& out,
                 bool legacy = true) {
    AttackStyle sty = styIn;
    if (legacy) sty.FromLegacyFrame();
    // Hand-built in the pre-frames spelling: the runner reads frames.
    if (sty.frames.empty()) sty.BuildFramesFromLegacy();
    MeleeState m;
    ApplyMeleeTuning(m.tuning);
    m.SetStroke(fixtureHand, fixtureTip, Vec3{0, 0, 1}, kRestReach);
    m.SetHandSign(1.0f);
    StrokeCursor cur;
    BeginStrokeProgram(cur, sty, 0, 0xC17A11u);
    out.clear();
    for (int i = 0; i < 200; i++) {
      const StrokeCursor::Phase ph = cur.phase;
      const StrokeStepResult r =
          StepStrokeProgram(cur, &sty, m, kAimAz, 0.0f, 0.0f, dt, kRight, kUp,
                            kFwd);
      out.push_back(Sample{ph, m.StrokeAz(), m.StrokeEl(), m.StrokeRadius()});
      if (r != StrokeStepResult::Live) break;
    }
    return cur;
  };
  auto cutOnly = [](const std::vector<Sample>& all) {
    std::vector<Sample> out;
    for (const Sample& s : all)
      if (s.phase == StrokeCursor::Phase::Cut) out.push_back(s);
    return out;
  };
  // A style with nothing authored but its path: no jitter, so the trace is the
  // program and not a draw.
  auto styleWith = [](std::vector<StrokeSegment> legs, int aimLeg) {
    AttackStyle s;
    s.name = "probe";
    s.windup = StrokeSegment{6, 0.20f, 0.10f, -0.05f};
    s.cut = std::move(legs);
    s.aimLeg = aimLeg;
    s.recover.ticks = 4;
    s.jitter = StrokeJitter{};
    return s;
  };

  // ---- 2 and 3: the runner ------------------------------------------------
  const AttackStyle straight =
      styleWith({StrokeSegment{8, -2.0f, 0.0f, 0.10f}}, -1);
  const AttackStyle dogleg =
      styleWith({StrokeSegment{4, -1.0f, -0.40f, 0.05f},
                 StrokeSegment{4, -1.0f, 0.40f, 0.05f}},
                -1);
  const AttackStyle marked =
      styleWith({StrokeSegment{4, -1.0f, -0.40f, 0.05f},
                 StrokeSegment{4, -1.0f, 0.40f, 0.05f}},
                1);
  std::vector<Sample> ts, td, tm;
  const StrokeCursor cs = run(straight, ts);
  const StrokeCursor cd = run(dogleg, td);
  const StrokeCursor cm = run(marked, tm);
  const std::vector<Sample> ds = cutOnly(ts), dd = cutOnly(td),
                            dm = cutOnly(tm);

  check(cd.cutLegs == 2 && cd.legTicks[0] == 4 && cd.legTicks[1] == 4 &&
            cd.cutTicks == 8,
        Format("the path resolves to two 4-tick legs in one 8-tick Cut phase "
               "(got %d legs, %d+%d, total %d)",
               cd.cutLegs, cd.legTicks[0], cd.legTicks[1], cd.cutTicks));
  check(!ds.empty() && ds.size() == dd.size(),
        Format("a two-leg path spends the same ticks cutting as the straight "
               "cut it sums to (%d vs %d)",
               (int)dd.size(), (int)ds.size()));

  // ---- THE CORNER, MEASURED WHERE THE DRIVER IS NOT IN THE WAY -----------
  //
  // (HISTORY: the driver's own Slash and its folded arc were removed
  // 2026-09-25, so the fast pair below now ends where it is authored too; the
  // slow pair is kept because it is the cleaner measurement.)
  // The obvious assertion — "the dogleg ends where the straight cut of the
  // same travel ends" — WAS FALSE, and it is worth saying why rather than
  // quietly not testing it. A cut that commits the driver's own Slash gets
  // its follow-through ARC FOLDED BACK INTO THE STROKE when the Slash ends
  // (melee.cpp, "a cut ENDS WHERE IT WENT"), and how much arc that is depends
  // on how the travel was delivered. Measured here: the straight 2.0-rad cut
  // finishes at az -1.40 and the two-leg path that sums to it at -0.75, which
  // is the authored end of the path exactly. Neither number is wrong; they
  // differ because one committed a single long Slash and the other did not,
  // and comparing them is comparing FOLLOW-THROUGH, not geometry.
  //
  // So the path's geometry is asserted on a SLOW pair, whose travel stays
  // under `commitSpeed` in every channel: there the stored stroke is the
  // program and nothing else, and "the corner is really driven" is a claim
  // about the runner rather than about the driver's arc. The fast pair above
  // still carries the structural claims (legs, ticks) and the aim marker
  // below, which are the ones that need the shipped regime.
  const AttackStyle slowLine =
      styleWith({StrokeSegment{10, -0.50f, 0.0f, 0.0f}}, -1);
  const AttackStyle slowBend =
      styleWith({StrokeSegment{5, -0.25f, -0.30f, 0.0f},
                 StrokeSegment{5, -0.25f, 0.30f, 0.0f}},
                -1);
  std::vector<Sample> tl, tb;
  run(slowLine, tl);
  run(slowBend, tb);
  const std::vector<Sample> dl = cutOnly(tl), db = cutOnly(tb);
  // The tolerances are data, so tuning them costs no rebuild.
  const float endEps = (float)BaselineNumber("cutPath.endAgreeRad", 0.08);
  const float bendMin = (float)BaselineNumber("cutPath.minCornerDipRad", 0.15);
  float dipL = 1e9f, dipB = 1e9f;
  int dipAt = -1;
  for (const Sample& s : dl) dipL = std::min(dipL, s.el);
  for (size_t i = 0; i < db.size(); i++)
    if (db[i].el < dipB) {
      dipB = db[i].el;
      dipAt = (int)i;
    }
  if (!dl.empty() && !db.empty()) {
    check(std::fabs(db.back().az - dl.back().az) <= endEps,
          Format("the path ENDS where the straight cut of the same total "
                 "travel ends (%.3f vs %.3f, tolerance %.3f)",
                 db.back().az, dl.back().az, endEps));
    check(std::fabs(db.back().el - dl.back().el) <= endEps,
          Format("...in elevation too, having been below it on the way "
                 "(%.3f vs %.3f)",
                 db.back().el, dl.back().el));
    check(dipB < dipL - bendMin,
          Format("...and it goes somewhere ELSE on the way: elevation dipped "
                 "to %.3f against the straight cut's %.3f (needs %.3f lower)",
                 dipB, dipL, bendMin));
    // ---- THE CORNER, AS A DEPARTURE FROM THE PATH'S OWN CHORD ------------
    //
    // Not as a reversal count: the arc rings as it unwinds, and the two-leg
    // path was measured turning three times rather than once. Not as an
    // absolute depth either — the rate this fixture delivers is close enough
    // to `commitSpeed` that the driver amplifies the -0.30 corner to -1.15.
    //
    // What survives both is DEVIATION FROM ITS OWN CHORD: how far the point
    // gets from the straight line between where this very stroke's cut began
    // and where it ended. A chord is zero by construction whatever the driver
    // adds on top, which is what makes the control arm meaningful — and the
    // control is a DIAGONAL straight cut, one that really moves in elevation,
    // rather than the flat one above, so "a straight cut deviates little"
    // is measured on a straight cut with something to deviate from.
    auto chordDev = [](const std::vector<Sample>& d, int& where) {
      where = -1;
      if (d.size() < 3) return 0.0f;
      const float e0 = d.front().el, e1 = d.back().el;
      const float span = (float)(d.size() - 1);
      float worst = 0;
      for (size_t i = 1; i + 1 < d.size(); i++) {
        const float chord = e0 + (e1 - e0) * ((float)i / span);
        const float dev = std::fabs(d[i].el - chord);
        if (dev > worst) {
          worst = dev;
          where = (int)i;
        }
      }
      return worst;
    };
    const AttackStyle slowDiag =
        styleWith({StrokeSegment{10, -0.50f, -0.60f, 0.0f}}, -1);
    std::vector<Sample> tg;
    run(slowDiag, tg);
    const std::vector<Sample> dg = cutOnly(tg);
    int devAtDiag = -1, devAtBend = -1;
    const float devDiag = chordDev(dg, devAtDiag);
    const float devBend = chordDev(db, devAtBend);
    check(devBend > devDiag + bendMin,
          Format("the path leaves its own chord where a straight cut does not "
                 "(%.3f rad against a diagonal chord's %.3f, needs %.3f more)",
                 devBend, devDiag, bendMin));
    // A leg is its own clock, so the departure must be AT the corner. At the
    // end of the cut it would be a slow arc; at the start, a windup that
    // missed its pose and spent the cut catching up.
    const int boundary = 5;   // slowBend's first leg is 5 of its 10 ticks
    check(devAtBend >= boundary - 2 && devAtBend <= boundary + 2,
          Format("the corner lands at the leg boundary rather than at either "
                 "end of the cut (furthest from the chord at tick %d of %d, "
                 "boundary %d)",
                 devAtBend, (int)db.size(), boundary));
    std::printf(
        "cut-path: chord departure — two-leg %.3f rad at tick %d, diagonal "
        "one-leg control %.3f at %d\n",
        devBend, devAtBend, devDiag, devAtDiag);
    (void)dipAt;
  }
  // THE AIM MARKER. The cut sweeps az DOWNWARD (-2.0 over the path), so the
  // crossing is the first tick at or under the aim.
  auto crossAt = [&](const std::vector<Sample>& d) {
    for (size_t i = 0; i < d.size(); i++)
      if (d[i].az <= kAimAz) return (int)i;
    return -1;
  };
  const int xd = crossAt(dd), xm = crossAt(dm);
  check(xd >= 0 && xm > xd,
        Format("marking leg 2 with \"aim\" makes the cut meet the target LATER "
               "in the path (unmarked tick %d, marked %d)",
               xd, xm));
  check(!dm.empty() && dm.front().az > kAimAz && xm >= 0,
        "...and the marked path still crosses the aim rather than starting "
        "past it");
  check(cm.cutTicks == cd.cutTicks,
        "the aim marker moves where the target is, not how long the cut takes");

  // ---- the path arithmetic, which three readers share ---------------------
  const StrokeSegment tot = dogleg.CutTravel();
  check(std::fabs(tot.az + 2.0f) < 1e-5f && std::fabs(tot.el) < 1e-5f &&
            std::fabs(tot.reach - 0.10f) < 1e-5f && tot.ticks == 8,
        "CutTravel sums the legs");
  const StrokeSegment thr = dogleg.CutThrough(0);
  check(std::fabs(thr.az + 1.0f) < 1e-5f && std::fabs(thr.el + 0.40f) < 1e-5f,
        "CutThrough(0) is where the point stands when the first leg ends");
  float offAz = 0, offEl = 0;
  dogleg.CutAimOffset(offAz, offEl);
  check(std::fabs(offAz + 1.0f) < 1e-5f && std::fabs(offEl) < 1e-5f,
        "an unmarked path puts the aim at the midpoint of the whole travel");
  marked.CutAimOffset(offAz, offEl);
  check(std::fabs(offAz + 1.5f) < 1e-5f && std::fabs(offEl + 0.20f) < 1e-5f,
        "...and a marked one puts it at the middle of the marked leg");

  // ---- 1 and 4: THE LOADER, through a written file ------------------------
  //
  // Through a FILE and not through a hand-built struct, because the claim is
  // about the two SPELLINGS and a struct has only one. It is the one thing in
  // this gate that touches the disk; a temp path, so a read-only asset tree or
  // a parallel run cannot collide with it.
  {
    const std::string probe =
        (std::filesystem::temp_directory_path() / "sandvox_cutpath_probe.json")
            .string();
    std::ofstream f(probe);
    if (!f) {
      std::printf("cut-path: could not write %s — loader half skipped\n",
                  probe.c_str());
    } else {
      f << R"({ "styles": [
        { "name": "asObject",
          "windup": { "ticks": 6, "az": 0.20, "el": 0.10, "reach": -0.05 },
          "cut": { "ticks": 8, "az": -2.0, "el": 0.0, "reach": 0.10 },
          "recover": { "ticks": 4 } },
        { "name": "asList",
          "windup": { "ticks": 6, "az": 0.20, "el": 0.10, "reach": -0.05 },
          "cut": [ { "ticks": 8, "az": -2.0, "el": 0.0, "reach": 0.10 } ],
          "recover": { "ticks": 4 } },
        { "name": "marked",
          "cut": [ { "ticks": 4, "az": -1.0 },
                   { "ticks": 4, "az": -1.0, "aim": true } ] },
        { "name": "twoAims",
          "cut": [ { "ticks": 4, "az": -1.0, "aim": true },
                   { "ticks": 4, "az": -1.0, "aim": true } ] },
        { "name": "tooMany",
          "cut": [ { "ticks": 2, "az": -0.2 }, { "ticks": 2, "az": -0.2 },
                   { "ticks": 2, "az": -0.2 }, { "ticks": 2, "az": -0.2 },
                   { "ticks": 2, "az": -0.2 }, { "ticks": 2, "az": -0.2 },
                   { "ticks": 2, "az": -0.2 }, { "ticks": 2, "az": -0.2 } ] },
        { "name": "emptyList", "cut": [] },
        { "name": "goesNowhere",
          "cut": [ { "ticks": 4 }, { "ticks": 4 } ] },
        { "name": "hitch",
          "cut": [ { "ticks": 4, "az": -1.0 }, { "ticks": 3 },
                   { "ticks": 4, "az": -1.0 } ] }
      ] })";
      f.close();
      StyleLibrary pl;
      std::string plog;
      check(LoadAttackStyles(probe, pl, plog), "the probe library loads");
      const AttackStyle* a = pl.At(pl.Find("asObject"));
      const AttackStyle* b = pl.At(pl.Find("asList"));
      check(a != nullptr && b != nullptr && a->cut.size() == 1 &&
                b->cut.size() == 1,
            "both cut spellings parse to exactly one leg");
      if (a != nullptr && b != nullptr) {
        std::vector<Sample> ta, tb;
        run(*a, ta, false);
        run(*b, tb, false);
        bool same = ta.size() == tb.size();
        for (size_t i = 0; same && i < ta.size(); i++)
          same = ta[i].az == tb[i].az && ta[i].el == tb[i].el &&
                 ta[i].r == tb[i].r;
        check(same,
              "a one-leg LIST and a bare cut OBJECT are the same stroke, tick "
              "for tick");
      }
      const AttackStyle* mk = pl.At(pl.Find("marked"));
      check(mk != nullptr && mk->aimLeg == 1, "\"aim\" names the leg it is on");
      const AttackStyle* two = pl.At(pl.Find("twoAims"));
      check(two != nullptr && two->aimLeg == 0 &&
                plog.find("keeps it") != std::string::npos,
            "two legs claiming the aim: the first wins, LOUDLY");
      const AttackStyle* tm2 = pl.At(pl.Find("tooMany"));
      check(tm2 != nullptr && (int)tm2->cut.size() == kMaxCutLegs &&
                plog.find("more than") != std::string::npos,
            Format("more than %d legs is dropped LOUDLY", kMaxCutLegs));
      const AttackStyle* em = pl.At(pl.Find("emptyList"));
      check(em != nullptr && em->cut.size() == 1 &&
                plog.find("empty cut list") != std::string::npos,
            "an empty cut list falls back to one default leg, loudly");
      check(pl.Find("goesNowhere") < 0 &&
                plog.find("travels nowhere") != std::string::npos,
            "a path with no motion anywhere in it is REFUSED, as a single "
            "motionless cut always was");
      const AttackStyle* h = pl.At(pl.Find("hitch"));
      check(h != nullptr && h->cut.size() == 3,
            "...but a motionless leg INSIDE a path is a legal hitch");
      if (h != nullptr) {
        std::vector<Sample> th;
        run(*h, th, false);
        const std::vector<Sample> dh = cutOnly(th);
        // The hitch has to be visible as a hitch: the middle three ticks move
        // the point far less than the legs either side of them. Measured on
        // the commanded azimuth, which is what the leg drives.
        float moveLegs = 0, moveHitch = 0;
        for (size_t i = 1; i < dh.size(); i++) {
          const float d = std::fabs(dh[i].az - dh[i - 1].az);
          // Ticks 4, 5 and 6 are the hitch's own (the sample is the state
          // AFTER that tick's step, and the hitch's first tick is 4).
          if (i >= 4 && i <= 6) moveHitch += d;
          else moveLegs += d;
        }
        check(dh.size() == 11 && moveHitch < moveLegs * 0.35f,
              Format("a hitch really holds: %.3f rad over its 3 ticks against "
                     "%.3f over the 8 that travel",
                     moveHitch, moveLegs));
      }
    }
  }

  // The evidence, printed whether or not it failed: three numbers that say
  // what the path did rather than that something is wrong (CLAUDE.md rule 6).
  std::printf(
      "cut-path: straight 2.0-rad cut ends az %.3f, the 2-leg path summing to "
      "it ends %.3f (follow-through, not geometry) | slow pair: ends az %.3f "
      "vs %.3f, el dipped %.3f vs %.3f at tick %d of %d | aim crossing "
      "unmarked tick %d, marked %d\n",
      ds.empty() ? 0.0f : ds.back().az, dd.empty() ? 0.0f : dd.back().az,
      db.empty() ? 0.0f : db.back().az, dl.empty() ? 0.0f : dl.back().az, dipB,
      dipL, dipAt, (int)db.size(), xd, xm);
  detail = Format("%d checks", checks);
  std::printf("cut-path: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

// ---- charged-strike: THE HELD STRIKE ON THE RUNNER ITSELF -------------------
//
// The player's charged strike (session.cpp, strokes.h StrokeCursor::
// holdWindup) is three claims about the stroke runner, asserted here on the
// shipped library with no world and no rig, through the same two calls the
// session makes (StepStrokeProgram, then StrokePoseNow):
//   1. held, the program PARKS at its last windup frame's end — for as long as
//      the button is down — and latches `charged`; unheld it never does;
//   2. a re-aim (ReaimChargedStroke) blends from the live arm to the new
//      style's windup end over exactly chargeBlendTicks on QuadInOut, a
//      re-aim mid-blend re-takes the live arm, and a style with no windup is
//      refused;
//   3. released, the cut starts on the next tick, from the live arm, and the
//      stroke still finishes.
Status GateChargedStrike(Ctx& c, std::string& detail) {
  (void)c;
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("charged-strike: FAILED %s\n", what.c_str());
    }
  };
  StyleLibrary lib;
  std::string log;
  if (!LoadAttackStyles(AssetDir() + "/mobs/attack_styles.json", lib, log) ||
      lib.empty()) {
    detail = "attack_styles.json did not load";
    std::printf("charged-strike: SKIP (%s)\n%s", detail.c_str(), log.c_str());
    return Status::Skip;
  }
  const int iA = lib.Find("horizontal_r"), iB = lib.Find("diag_tr_bl"),
            iC = lib.Find("overhead");
  if (iA < 0 || iB < 0 || iC < 0) {
    detail = "compass styles missing";
    std::printf("charged-strike: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  const AttackStyle &A = lib.styles[iA], &B = lib.styles[iB], &C = lib.styles[iC];
  const float dt = 1.0f / 30.0f;
  const int blend = 15;
  auto fresh = [&](MeleeState& m) {
    m = MeleeState{};
    ApplyMeleeTuning(m.tuning);
    m.SetStroke(Vec3{-0.4f, -3.0f, -0.3f}, Vec3{-0.4f, 2.5f, -0.3f},
                Vec3{0, 0, 1}, kRestReach);
    m.SetHandSign(1.0f);
  };
  auto step = [&](StrokeCursor& cur, const AttackStyle& sty, MeleeState& m) {
    const StrokeStepResult r =
        StepStrokeProgram(cur, &sty, m, 0.0f, 0.0f, 0.0f, dt, kRight, kUp, kFwd);
    return std::make_pair(r, StrokePoseNow(cur, &sty, m));
  };
  auto sameQuat = [](const Quat& a, const Quat& b) {
    return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) +
               std::fabs(a.w - b.w) < 1e-5f;
  };
  auto atWindupEnd = [&](const WeaponPose& wp, const AttackStyle& sty) {
    const StrokePose& p = sty.frames[sty.FirstCut() - 1].pose;
    return wp.keyed.on && wp.keyed.t > 0.9999f &&
           sameQuat(wp.keyed.to[kArmShoulder], p.joint[kArmShoulder]) &&
           sameQuat(wp.keyed.to[kArmElbow], p.joint[kArmElbow]);
  };

  // ---- P. THE TWO READS OF ONE PICKER (strike_pick.h) ---------------------
  // A normal click reads the LIVE flick, gone once the mouse settles; a
  // charged hold reads the REMEMBERED one, which only a short FLICK sets — a
  // long or big motion is a camera turn and leaves it alone — and which the
  // press re-seeds.
  {
    const float minSp = 250.0f;
    const int fT = 8;
    const float fPx = 240.0f;
    StrikePicker pk;
    auto feed = [&](int n, float dx, float dy) {
      for (int i = 0; i < n; i++) pk.Feed(dx, dy, dt, minSp, fT, fPx);
    };
    float fx = 0, fy = 0;
    check(!pk.Pick(minSp, fx, fy) && !pk.Remembered(fx, fy),
          "a fresh picker has no direction");
    feed(3, 0.0f, -40.0f);   // a flick up: 0.1 s, 120 px
    feed(20, 0.0f, 0.0f);
    check(!pk.Pick(minSp, fx, fy), "live: a settled mouse has no flick (click alternates)");
    check(pk.Remembered(fx, fy) && fy < -0.99f && pk.lastGestureFlick,
          Format("remembered: the flick up survives 20 still ticks (%.2f, %.2f)", fx, fy));
    feed(20, 0.3f, 0.0f);    // 9 px/s drift
    check(pk.Remembered(fx, fy) && fy < -0.99f,
          "remembered: a slow drift under the bar does not move it");
    feed(20, 40.0f, 0.0f);   // a camera turn right: 0.67 s, 800 px
    feed(20, 0.0f, 0.0f);
    check(pk.Remembered(fx, fy) && fy < -0.99f && !pk.lastGestureFlick,
          Format("a long turn right is a TURN: the held side stays (%.2f, %.2f)", fx, fy));
    feed(3, 200.0f, 0.0f);   // a snap turn: short but 600 px
    feed(20, 0.0f, 0.0f);
    check(pk.Remembered(fx, fy) && fy < -0.99f && !pk.lastGestureFlick,
          "a short but huge snap is a TURN too");
    feed(3, 40.0f, 0.0f);    // a flick right
    feed(20, 0.0f, 0.0f);
    check(pk.Remembered(fx, fy) && fx > 0.99f,
          Format("a flick right re-aims (%.2f, %.2f)", fx, fy));
    // A press lands mid-flick: the flick's tail curls another way before it
    // stops, and the hold must still start where the press pointed.
    feed(2, 0.0f, 40.0f);                 // flicking down...
    pk.Seed(true, 0.0f, 1.0f);            // ...the click, pointing down
    feed(3, 60.0f, 0.0f);                 // the tail swings right
    feed(20, 0.0f, 0.0f);
    check(pk.Remembered(fx, fy) && fy > 0.99f,
          Format("the motion a press landed in is the press's (%.2f, %.2f)", fx, fy));
    feed(3, 40.0f, 0.0f);                 // the NEXT flick still re-aims
    feed(20, 0.0f, 0.0f);
    check(pk.Remembered(fx, fy) && fx > 0.99f, "the next flick after it re-aims");
    pk.Seed(false, 0.0f, 0.0f);
    check(!pk.Remembered(fx, fy), "a neutral press clears it");
    pk.Seed(true, 0.0f, 1.0f);
    check(pk.Remembered(fx, fy) && fy > 0.99f, "a flicked press seeds it");
  }

  // ---- T. THE HOLD'S CHEST TURN (AttackStyle::chargeTwist) ---------------
  // A style authoring `chargeTwistDeg` turns the torso that much further
  // while parked — eased in, never a step — and hands it back after release.
  if (const int iL = lib.Find("horizontal_l"); iL >= 0 &&
                                               lib.styles[iL].chargeTwist != 0.0f) {
    const AttackStyle& L = lib.styles[iL];
    MeleeState ml;
    fresh(ml);
    StrokeCursor cl;
    BeginStrokeProgram(cl, L, iL, 7u);
    float prev = 0.0f, worstStep = 0.0f;
    for (int i = 0; i < cl.windupTicks + 30; i++) {
      cl.holdWindup = true;
      step(cl, L, ml);
      worstStep = std::max(worstStep, std::fabs(cl.chargeTwistNow - prev));
      prev = cl.chargeTwistNow;
    }
    const float want = L.chargeTwist;
    check(std::fabs(cl.chargeTwistNow - want) < 0.02f,
          Format("held horizontal_l turns the chest %.1f deg (want %.1f)",
                 cl.chargeTwistNow * 57.2958f, want * 57.2958f));
    check(worstStep <= std::fabs(want) * 0.26f,
          Format("the chest turn eases in (worst step %.2f deg)", worstStep * 57.2958f));
    const WeaponPose held = StrokePoseNow(cl, &L, ml);
    WeaponPose bare = ml.Pose();
    StrokeKeyedPose(cl, L, bare);
    check(std::fabs(held.torsoTwist - (bare.torsoTwist + cl.chargeTwistNow)) < 1e-5f,
          "the turn rides on top of the frames' own twist");
    cl.holdWindup = false;
    for (int i = 0; i < 14; i++) step(cl, L, ml);
    check(std::fabs(cl.chargeTwistNow) < std::fabs(want) * 0.05f,
          Format("released, the turn hands back (%.2f deg left)",
                 cl.chargeTwistNow * 57.2958f));
  } else {
    check(false, "horizontal_l authors a chargeTwistDeg");
  }

  // ---- 0. the control: unheld, the cut starts when the windup ends --------
  {
    MeleeState m;
    fresh(m);
    StrokeCursor cur;
    BeginStrokeProgram(cur, A, iA, 7u);
    int cutAt = -1;
    for (int i = 0; i < 200 && cutAt < 0; i++) {
      step(cur, A, m);
      if (cur.Cutting()) cutAt = i + 1;
    }
    check(cutAt == cur.windupTicks,
          Format("unheld cut begins at tick %d, want the windup's %d", cutAt,
                 cur.windupTicks));
    check(!cur.charged, "an unheld strike is not charged");
  }

  MeleeState m;
  fresh(m);
  StrokeCursor cur;
  BeginStrokeProgram(cur, A, iA, 7u);
  const int windA = cur.windupTicks;
  // ---- 1. held: parked, as long as the button is down -----------------------
  bool parkedAll = true, poseAll = true;
  for (int i = 0; i < windA + 30; i++) {
    cur.holdWindup = true;
    const auto [r, wp] = step(cur, A, m);
    if (i + 1 >= windA) {
      parkedAll = parkedAll && cur.Holding() && r == StrokeStepResult::Live;
      poseAll = poseAll && atWindupEnd(wp, A);
    }
  }
  check(parkedAll, "held past the windup, the program stays parked");
  check(poseAll, "parked, the arm is the last windup frame's end pose");
  check(cur.charged, "reaching the hold latches `charged`");

  // ---- 2. re-aim: QuadInOut over `blend`, from the live arm ---------------
  check(ReaimChargedStroke(cur, A, B, iB, blend), "re-aim A->B accepted");
  check(cur.style == iB && cur.Holding() && cur.charged,
        "after the re-aim the cursor holds B, still charged");
  std::vector<float> ts;
  bool toB = true, live = true;
  int recaptures = 0;
  for (int i = 0; i < blend + 5; i++) {
    cur.holdWindup = true;
    const auto [r, wp] = step(cur, B, m);
    (void)r;
    ts.push_back(wp.keyed.t);
    live = live && wp.keyed.fromLive;
    recaptures += wp.keyed.recapture ? 1 : 0;
    const StrokePose& p = B.frames[B.FirstCut() - 1].pose;
    toB = toB && sameQuat(wp.keyed.to[kArmShoulder], p.joint[kArmShoulder]);
  }
  check(toB, "the re-aim blends TO B's windup end");
  check(live && recaptures == 1,
        Format("the blend is from the live arm, taken once (%d)", recaptures));
  bool mono = true;
  for (size_t i = 1; i < ts.size(); i++) mono = mono && ts[i] >= ts[i - 1];
  check(mono, "the blend never goes backward");
  check(std::fabs(ts[blend - 1] - 1.0f) < 1e-5f && ts[blend - 2] < 1.0f,
        Format("the blend arrives on tick %d exactly (t %.3f, %.3f)", blend,
               ts[blend - 2], ts[blend - 1]));
  const float first = ts[0], mid = ts[blend / 2] - ts[blend / 2 - 1],
              last = ts[blend - 1] - ts[blend - 2];
  check(std::fabs(first - ApplyEase(Ease::QuadInOut, 1.0f / blend)) < 1e-5f,
        Format("first tick is QuadInOut(1/%d) (%.4f)", blend, first));
  check(mid > 2.0f * first && mid > 2.0f * last,
        Format("eased in AND out: steps %.3f .. %.3f .. %.3f", first, mid, last));

  // ...a re-aim mid-blend re-takes the live arm at once.
  check(ReaimChargedStroke(cur, B, C, iC, blend), "re-aim B->C accepted");
  for (int i = 0; i < 5; i++) {
    cur.holdWindup = true;
    step(cur, C, m);
  }
  check(ReaimChargedStroke(cur, C, A, iA, blend), "re-aim mid-blend accepted");
  {
    cur.holdWindup = true;
    const auto [r, wp] = step(cur, A, m);
    (void)r;
    check(wp.keyed.recapture && wp.keyed.t < 0.05f,
          Format("a re-aim mid-blend restarts from the live arm (t %.3f)",
                 wp.keyed.t));
  }
  // ...and a style with no windup has nothing to hold.
  {
    AttackStyle noWind = B;
    noWind.frames[0].cuts = true;
    StrokeCursor probe = cur;
    check(!ReaimChargedStroke(probe, A, noWind, iB, blend) &&
              probe.style == cur.style,
          "a style whose first frame cuts is refused, cursor untouched");
  }

  // ---- 3. release: the cut starts next tick, from the live arm -----------
  {
    cur.holdWindup = false;
    const auto [r, wp] = step(cur, A, m);
    (void)r;
    check(cur.Cutting(), "released, the cut starts on the next tick");
    check(wp.keyed.fromLive && wp.keyed.recapture,
          "the cut after a re-aim starts from the live arm");
    check(cur.charged, "the cut is a charged one");
    int left = 0;
    StrokeStepResult rr = StrokeStepResult::Live;
    while (rr != StrokeStepResult::Finished && left++ < 300)
      rr = step(cur, A, m).first;
    check(rr == StrokeStepResult::Finished, "the charged stroke finishes");
  }

  detail = Format("%d checks", checks);
  std::printf("charged-strike: %s (%d checks)\n", ok ? "PASS" : "FAIL", checks);
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SwingGates() {
  static const std::vector<Gate> g = {
      // No deps and no world: it builds its own MeleeState fixtures, so it can
      // neither disturb pristine worldgen nor be disturbed by anything.
      {"swing", "player", {}, false, GateSwing},
      // The derivative of the same driver, over every authored held style:
      // nothing may step in one tick. Asset-light and world-free like `swing`,
      // so it sits beside it in kOrder and is the whole verification loop for
      // a continuity change.
      {"swing-smooth", "player", {}, false, GateSwingSmooth},
      // The cut PATH: multi-leg cuts, the aim marker, and the loader's two
      // spellings. Same shape as the two above — its own in-memory styles, one
      // temp file, no world — so it sits with them in kOrder.
      {"cut-path", "player", {}, false, GateCutPath},
      // The player's CHARGED strike on the runner: the hold, the re-aim
      // blend, the release. Same footing as the three above.
      {"charged-strike", "player", {}, false, GateChargedStrike},
      // The opposite: the whole pipeline, on real terrain, against a real body.
      // Expensive, so it runs LATE in kOrder with the other world-touching
      // gates and regenerates worldgen on the way out (CLAUDE.md rule 7).
      {"swing-plane", "player", {}, false, GateSwingPlane},
      // The discrete strikes: every authored style through the player's own
      // runner on the real rig, plus the head keep-out. World-touching like
      // swing-plane and placed beside it in kOrder.
      {"player-styles", "player", {}, false, GatePlayerStyles},
      // ...and the same thing with nothing in the fist (plan §8). World-
      // touching like its two neighbours; kOrder puts it at the very end of
      // the run with the other unarmed gates, because it spawns an avatar and
      // that perturbs every id-keyed draw after it.
      {"player-unarmed", "player", {}, false, GatePlayerUnarmed},
  };
  return g;
}

}  // namespace selftest
