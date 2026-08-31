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

#include "game/avatar.h"
#include "game/melee.h"
#include "game/player.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

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
    int samples = 0;
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
      // The pole is where the ELBOW bulges, and it must be opposite the travel:
      // that is what puts the arm's bend plane IN the plane of the cut.
      worstPole = std::max(worstPole, m.BendPole().dot(dir));
      // ...and the hand is still the tip minus a blade.
      worstBlade = std::max(
          worstBlade,
          std::fabs((m.TipOffset() - m.HandOffset()).len() - bladeLen));
    }
    std::printf(
        "swing block 9: worst |flat.travel| %.3f, worst pole.travel %.3f, "
        "worst blade-length drift %.3f vox\n",
        worstFlat, worstPole, worstBlade);
    // SAMPLES > 0 IS NOT PEDANTRY. `worstPole` is a max() over a quantity that
    // is supposed to be NEGATIVE, so a run that took no samples at all leaves it
    // at -infinity and every check below passes for the worst possible reason.
    // (It happened: a stroke long enough to settle the lean plane was also long
    // enough to run into the azimuth stop, and the last eighteen ticks moved
    // nothing.)
    check(samples > 8, "the settled part of the stroke was actually measured");
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
  // Heading 0 and an axis-aligned basis, so every number printed by a failure
  // can be read by eye: +X is the camera's right, +Y up, +Z the way the
  // character faces.
  const Vec3 kR{1, 0, 0}, kU{0, 1, 0}, kF{0, 0, 1};
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
    b.az = std::atan2(b.fromShoulder.x, b.fromShoulder.z);
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
      if (melee.Phase() == SwingPhase::Slash) return false;   // never, by cap
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
    for (int i = 0; i < flickN; i++) {
      drive(flickX, flickY, true);
      const BladeRead b = readBlade();
      // The COMMANDED tip, in exactly the frame the observation is stated in.
      {
        const Vec3 w = melee.TipOffset();
        const float wr = std::max(w.len(), 1e-4f);
        const float waz = std::atan2(w.x, w.z);
        const float wel = std::asin(std::clamp(w.y / wr, -1.0f, 1.0f));
        cmdAzMin = std::min(cmdAzMin, waz);
        cmdAzMax = std::max(cmdAzMax, waz);
        cmdElMin = std::min(cmdElMin, wel);
        cmdElMax = std::max(cmdElMax, wel);
        s.cmdRMax = std::max(s.cmdRMax, wr);
        if (cmdPath.empty()) s.cmdFirst = w;
        s.cmdLast = w;
        if (!cmdPath.empty()) {
          const Vec3& pv = cmdPath.back();
          if (waz > std::atan2(pv.x, pv.z) + 1e-3f) s.cmdAzBacksteps++;
        }
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
      s.azMin = std::min(s.azMin, b.az);
      s.azMax = std::max(s.azMax, b.az);
      s.elMin = std::min(s.elMin, b.el);
      s.elMax = std::max(s.elMax, b.el);
      s.rMax = std::max(s.rMax, b.r);
      s.minFwd = std::min(s.minFwd, b.fromShoulder.z);
      if (path.empty()) s.first = b.fromShoulder;
      s.last = b.fromShoulder;
      path.push_back(b.fromShoulder);
      if (havePrev && b.az > prevAz + 1e-3f) s.azBacksteps++;
      prevAz = b.az;
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
    melee.Reset();
    drive(0, 0, true);
    float worst = 0, mean = 0, gapWorst = 0;
    int n = 0;
    for (int i = 0; i < 24; i++) {
      drive(i < 12 ? 12.0f : -12.0f, 0, true);
      const BladeRead b = readBlade();
      if (!b.valid) continue;
      const Vec3 want = melee.TipOffset();
      const float err = (b.fromShoulder - want).len();
      gapWorst = std::max(gapWorst, b.physGap);
      worst = std::max(worst, err);
      mean += err;
      n++;
    }
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    mean = n ? mean / (float)n : 0;
    const float followMax = (float)BaselineNumber("swingPlane.followMaxFrac", 0.35);
    const float rNow = std::max(melee.StrokeRadius(), 1e-3f);
    check(n > 18, "the blade stayed readable through the follow test");
    check(worst < followMax * rNow,
          "the SWORD goes where the stroke says it goes (if this fails, every "
          "sweep below is measuring the rig and not the mapping)");
    RecordObserved("swingPlane.followWorstObserved", worst);
    std::printf(
        "swing-plane 0 (follow): worst residual %.2f vox, mean %.2f, over "
        "%d ticks at r %.2f (max %.2f)\n",
        worst, mean, n, rNow, followMax * rNow);
  }

  // ---- A. right to left is a horizontal sweep in front --------------------
  // THE OWNER'S ACCEPTANCE CRITERION: "moving mouse right to left will swing
  // the sword right to left horizontally".
  {
    // READIED WELL OUT TO THE WEAPON SIDE AND LEVEL, and the number is chosen
    // by the SHOULDER'S authored reach rather than by taste. human.json bounds
    // the right upper arm to 30 degrees past the midline
    // (`poseLimit.reach normal [1,0,0] max 30`), and a committed cut carries
    // ~2.0 rad of arc on its own (`MeleeTuning::swingArc`) whatever the mouse
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
    const float dAz = s.last.x - s.first.x;   // world x: leftward is negative
    const float dEl = s.last.y - s.first.y;   // world y: downward is negative
    const float ratio = std::fabs(dAz) > 1e-4f && std::fabs(dEl) > 1e-4f
                            ? std::max(std::fabs(dAz) / std::fabs(dEl),
                                       std::fabs(dEl) / std::fabs(dAz))
                            : 1e9f;
    check(dAz < 0.0f, "a top-right to bottom-left flick takes the point LEFT");
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
    int steps = 0, backwards = 0;
    float total = 0;
    for (int i = 0; i < 22; i++) {
      drive(12.0f, 0, true);
      const BladeRead b = readBlade();
      if (!b.valid) continue;
      steps++;
      if (b.az < prev - 1e-3f) backwards++;
      total += b.az - prev;
      prev = b.az;
    }
    check(steps > 15, "the blade stayed readable through the steer");
    check(total > 0.5f, "steady rightward mouse carries the tip right");
    check(backwards <= 4,
          "...monotonically: equal steps of mouse never send the tip back");
    for (int i = 0; i < 20; i++) drive(0, 0, false);
    std::printf(
        "swing-plane D (steering): %d steps, %.2f rad of azimuth, %d "
        "backwards\n",
        steps, total, backwards);
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
      const bool eReady = readyTo(1.15f, 0.15f);
      check(eReady, "the cut's ready reached its start pose");
      std::vector<Vec3> tipPath, basePath;
      BladeRead prev = readBlade();
      int sweptTicks = 0;
      for (int i = 0; i < 18; i++) {
        drive(-34.0f, 0, true);
        const BladeRead b = readBlade();
        if (!b.valid) continue;
        tipPath.push_back(b.tip);
        basePath.push_back(b.base);
        if (prev.valid && melee.Cutting()) {
          EdgeSweep sw;
          sw.aPrev = prev.base;
          sw.bPrev = prev.tip;
          sw.aNow = b.base;
          sw.bNow = b.tip;
          sw.flatNow = b.flat;
          sw.dt = kTickDt;
          sw.halfWidth = sword->edgeHalfWidth;
          sw.damage = sword->damage;
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
      const Vec3 chest{(float)dx + dd.worldSize.x * 0.5f,
                       (float)(dh + 2) + dd.worldSize.y * 0.66f,
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
      sw.damage = sword->damage;
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

}  // namespace

const std::vector<Gate>& SwingGates() {
  static const std::vector<Gate> g = {
      // No deps and no world: it builds its own MeleeState fixtures, so it can
      // neither disturb pristine worldgen nor be disturbed by anything.
      {"swing", "player", {}, false, GateSwing},
      // The opposite: the whole pipeline, on real terrain, against a real body.
      // Expensive, so it runs LATE in kOrder with the other world-touching
      // gates and regenerates worldgen on the way out (CLAUDE.md rule 7).
      {"swing-plane", "player", {}, false, GateSwingPlane},
  };
  return g;
}

}  // namespace selftest
