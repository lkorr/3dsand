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

#include <algorithm>
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

// A second fixture, out in front and level, for the arc blocks. The hanging
// rest pose above sits at ~127 degrees of azimuth, which is most of the way to
// the weapon-side stop — an arc measured from there would be measuring the
// clamp. Nothing about the mapping depends on which of the two is used.
const Vec3 kFrontHand{1.0f, 0.0f, 3.0f};

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

MeleeState MakeSeeded() {
  MeleeState m;
  m.SetArm(kRestHand, kRestReach);
  return m;
}

MeleeState MakeFront() {
  MeleeState m;
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
    const Vec3 tip = kRestHand + Vec3{0.6f, 0.2f, 2.4f};   // a blade, at an angle
    MeleeState bladed;
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
    check(fast.Phase() != SwingPhase::Slash &&
              slow.Phase() != SwingPhase::Slash,
          "neither arm of the rate test committed a cut (which would make it "
          "measure the arc instead of the mapping)");

    // ...and the azimuth is the seed plus pixels x gain. Radians now, not
    // voxels: the point rides a sphere about the shoulder, so a sideways drag
    // is an ARC and not a slide across a plane.
    check(std::fabs(slow.StrokeAz() - (az0 + kPx * t.aimGainX)) < 1e-4f,
          "rightward mouse turns the stroke right, by pixels x gain");
    check(slow.TipOffset().x > kFrontHand.x + 0.5f,
          "and rightward really is to the RIGHT in world terms");

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
    check(m.Phase() != SwingPhase::Slash, "the sustained push did not commit");
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

  // ---- 6. a committed cut still fires, and still ends where the mouse did ---
  {
    MeleeState m = MakeSeeded();
    Step(m, 0, 0);
    const float azSeed = m.StrokeAz(), elSeed = m.StrokeEl();
    // Fast enough to clear commitSpeed, in a direction with both axes so the
    // "a diagonal flick is a diagonal cut" property is what is measured.
    for (int i = 0; i < 6; i++) Step(m, 40.0f, -40.0f);
    check(m.Phase() == SwingPhase::Slash, "a fast flick commits a cut");
    const Vec3 cut = m.CutDir();
    check(cut.x > 0.3f && cut.y > 0.3f,
          "the cut goes the way the mouse went (right and up)");
    // NO POP ON THE COMMITTING TICK. The previous law's arc was centred on the
    // hand, so it jumped half a swing backwards the instant it fired; this one
    // anticipates on a bow that is zero at both ends.
    const Vec3 atCommit = m.TipOffset();
    Step(m, 40.0f, -40.0f);
    check(Dist(m.TipOffset(), atCommit) < kRestReach * 0.5f,
          "committing does not teleport the point");
    // Let the slash and its follow-through run out with the mouse now still.
    for (int i = 0; i < 40; i++) Step(m, 0, 0);
    check(m.Phase() == SwingPhase::Guard,
          "the cut returns to guard while the button is held");
    // The cut ENDS WHERE IT WENT: up and to the right of where the blade
    // started, not back at the seed. STATED ON THE STROKE, because the hanging
    // fixture's azimuth is already near its stop and a diagonal flick from
    // there finishes nearly overhead — where the world-space x of a raised
    // point is small for a reason that has nothing to do with the mapping.
    check(m.StrokeEl() > elSeed + 0.5f && m.StrokeAz() >= azSeed - 1e-4f,
          "the cut ends where the mouse took it, not back at the start");
  }

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
    check(m.Phase() != SwingPhase::Slash, "the ready did not fire a cut");
    const float azReady = m.StrokeAz();
    check(azReady > az0 + 0.5f, "the ready really did load the blade right");

    // The flick, and every tip on the way through it.
    float azMin = 1e9f, azMax = -1e9f, elDev = 0, minFwd = 1e9f;
    float radMin = 1e9f, radMax = -1e9f;
    float prevAz = azReady;
    int backSteps = 0;
    bool fired = false;
    for (int i = 0; i < 24; i++) {
      Step(m, -25.0f, 0);
      fired = fired || m.Phase() == SwingPhase::Slash;
      const Vec3 tip = m.TipOffset();
      const float az = std::atan2(tip.x, tip.z);
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
    check(fired, "a fast leftward flick commits a cut");
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
    check(std::fabs(m.StrokeAz() - std::atan2(tip0.x, tip0.z)) < 1e-4f,
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
    m.SetStroke(kFrontHand, tip, Vec3{0, 1, 0}, kRestReach);
    m.Feed(0, 0);
    m.Update(kDt, true, true, kRight, kUp, kFwd);
    Vec3 prevTip = m.TipOffset();
    // worstPole starts at -infinity, NOT at 0: the quantity it tracks is
    // supposed to be NEGATIVE, and a max() seeded at zero can only ever report
    // zero — a check that passes and fails for the same reason.
    float worstFlat = 0, worstPole = -1e9f, worstBlade = 0;
    for (int i = 0; i < 30; i++) {
      m.SetStroke(m.HandOffset(), m.TipOffset(), m.BladeFlat(), kRestReach);
      Step(m, -kNoCommitPx, 0);
      const Vec3 travel = m.TipOffset() - prevTip;
      prevTip = m.TipOffset();
      if (travel.len() < 1e-4f) continue;
      const Vec3 dir = travel.normalized();
      if (i < 10) continue;   // let the smoothed frame settle first
      worstFlat = std::max(worstFlat, std::fabs(m.BladeFlat().dot(dir)));
      // The pole is where the ELBOW bulges, and it must be opposite the travel:
      // that is what puts the arm's bend plane IN the plane of the cut.
      worstPole = std::max(worstPole, m.BendPole().dot(dir));
      // ...and the hand is still the tip minus a blade.
      worstBlade = std::max(
          worstBlade,
          std::fabs((m.TipOffset() - m.HandOffset()).len() - bladeLen));
    }
    check(worstFlat < 0.2f,
          "the blade's FLAT stays square to the travel, i.e. the edge leads");
    check(worstPole < -0.5f,
          "the elbow trails the hand along the stroke (the bend plane IS the "
          "cut plane)");
    check(worstBlade < 0.35f,
          "the hand stays exactly one blade behind the point");
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
