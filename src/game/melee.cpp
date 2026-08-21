#include "game/melee.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

// See melee.h for what this is and why. This file is only the motion: the
// damage side lives in main.cpp's tick loop, which owns the ray casts and the
// spawn/op lists a carve has to reach.

using nlohmann::json;

// ---- item library ----------------------------------------------------------

bool LoadItems(const std::string& path, ItemLibrary& out, std::string& errors) {
  out = ItemLibrary{};
  std::ifstream f(path);
  if (!f) {
    errors += "items: cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    errors += std::string("items: parse error: ") + e.what() + "\n";
    return false;
  }
  for (const auto& it : j.value("items", json::array())) {
    ItemDef d;
    d.name = it.value("id", "");
    if (d.name.empty()) {
      errors += "items: an entry has no \"id\" — skipped\n";
      continue;
    }
    const std::string kind = it.value("kind", "");
    if (kind == "melee") {
      d.kind = ItemKind::Melee;
    } else {
      errors += "items: \"" + d.name + "\" has unknown kind \"" + kind +
                "\" — skipped\n";
      continue;
    }
    d.part = it.value("part", "");
    d.damage = it.value("damage", 12.0f);
    d.carveBonus = it.value("carveBonus", 0.0f);
    d.reach = it.value("reach", 9.0f);
    out.items.push_back(std::move(d));
  }
  if (out.items.empty()) errors += "items: no usable items in " + path + "\n";
  return !out.items.empty();
}

namespace {

// Exponential smoothing that is framerate-independent: the fraction of the
// error removed per second is what is specified, not the fraction per frame.
// The naive `a += (b - a) * k` form makes every constant in this file mean a
// different thing at 30 fps than at 144.
float SmoothAlpha(float halflife, float dt) {
  if (halflife <= 1e-5f) return 1.0f;
  return 1.0f - std::exp2(-dt / halflife);
}

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

}  // namespace

void MeleeState::AddMouse(float dx, float dy) {
  mouseAccum_.x += dx;
  mouseAccum_.y += dy;
}

void MeleeState::Reset() {
  phase_ = SwingPhase::Idle;
  phaseTime_ = 0;
  mouseAccum_ = Vec3{};
  mouseVel_ = Vec3{};
  mouseSpeed_ = 0;
  cutDir_ = Vec3{};
  lean_ = Vec3{};
}

void MeleeState::Update(float dt, bool held, bool armed, const Vec3& right,
                        const Vec3& up, const Vec3& fwd) {
  if (dt <= 0) return;

  // ---- mouse velocity ------------------------------------------------------
  // The accumulator holds this frame's raw pixels; convert to px/sec and
  // smooth. Draining it here (rather than in AddMouse) is what makes the
  // per-frame/per-tick split in melee.h work: several ticks may run per frame,
  // and only the first sees new motion.
  Vec3 instant{mouseAccum_.x / dt, mouseAccum_.y / dt, 0};
  mouseAccum_ = Vec3{};
  mouseVel_ = Lerp(mouseVel_, instant, SmoothAlpha(tuning.dirSmoothing, dt));
  mouseSpeed_ = std::sqrt(mouseVel_.x * mouseVel_.x + mouseVel_.y * mouseVel_.y);

  // Screen motion -> a direction in the camera plane. Screen +y is DOWN, so it
  // maps to -up: a downward flick must cut downward, and getting this sign
  // wrong produces a weapon that mirrors the player's hand, which reads as
  // broken long before anyone works out why.
  Vec3 screenDir = (right * mouseVel_.x + up * -mouseVel_.y);
  if (screenDir.len() > 1e-4f) screenDir = screenDir.normalized();

  if (!armed) {
    // Unarmed: collapse to idle and forget any half-built swing, so picking a
    // weapon back up never resumes a cut the player started with empty hands.
    if (phase_ != SwingPhase::Idle) Reset();
  }

  phaseTime_ += dt;

  switch (phase_) {
    case SwingPhase::Idle:
      if (armed && held) {
        phase_ = SwingPhase::Guard;
        phaseTime_ = 0;
        lean_ = Vec3{};
      }
      break;

    case SwingPhase::Guard:
    case SwingPhase::Wind: {
      if (!held) {
        // Released without committing: drop the guard.
        phase_ = SwingPhase::Recover;
        phaseTime_ = 0;
        break;
      }
      // Track the mouse. The lean is what makes the blade feel connected to
      // the hand while merely aiming — it is the same input that will become
      // the cut, shown before it fires, so committing never feels arbitrary.
      Vec3 want = screenDir * (mouseSpeed_ * tuning.trackGain);
      const float kMaxLean = 2.4f;
      if (want.len() > kMaxLean) want = want.normalized() * kMaxLean;
      lean_ = Lerp(lean_, want, SmoothAlpha(0.05f, dt));

      phase_ = mouseSpeed_ > tuning.commitSpeed * 0.35f ? SwingPhase::Wind
                                                        : SwingPhase::Guard;
      // COMMIT. The direction is frozen here and not touched again until the
      // slash ends: a cut that keeps steering with the mouse mid-swing feels
      // like dragging the blade through treacle, and it also makes the sweep
      // curve, which the damage code would then have to chord.
      if (mouseSpeed_ > tuning.commitSpeed && screenDir.len() > 0.5f) {
        cutDir_ = screenDir;
        phase_ = SwingPhase::Slash;
        phaseTime_ = 0;
      }
      break;
    }

    case SwingPhase::Slash:
      if (phaseTime_ >= tuning.slashTime) {
        phase_ = SwingPhase::Recover;
        phaseTime_ = 0;
      }
      break;

    case SwingPhase::Recover:
      if (phaseTime_ >= tuning.recoverTime) {
        phase_ = (armed && held) ? SwingPhase::Guard : SwingPhase::Idle;
        phaseTime_ = 0;
        lean_ = Vec3{};
      }
      break;
  }

  // ---- pose ----------------------------------------------------------------
  // The guard: blade up and slightly forward, on the weapon side.
  const Vec3 guard = fwd * tuning.guardForward + up * tuning.guardUp +
                     right * tuning.guardSide;

  switch (phase_) {
    case SwingPhase::Idle:
      // Let the arm hang; the walk cycle owns the pose. Decaying rather than
      // snapping means sheathing mid-guard does not pop.
      hand_ = Lerp(hand_, Vec3{}, SmoothAlpha(0.12f, dt));
      bladeDir_ = up;
      bladeUp_ = fwd;
      break;

    case SwingPhase::Guard:
    case SwingPhase::Wind:
      // The lean IS the tell: the whole hand shifts the way the mouse is
      // moving, so the player can see which way the weapon is loaded without
      // the blade twisting in the grip.
      hand_ = Lerp(hand_, guard + lean_, SmoothAlpha(0.05f, dt));
      bladeDir_ = up;
      bladeUp_ = fwd;
      break;

    case SwingPhase::Slash: {
      // The cut: the HAND travels from the far side of the cut direction,
      // through the guard, out to the near side — so the weapon passes ACROSS
      // the player's front rather than poking outward. `t` is eased so the
      // middle of the stroke is the fast part, which is both how a real cut
      // works and what makes the speed-scaled damage land at the middle of the
      // arc where the player aimed.
      //
      // The arc BOWS OUTWARD (the fwd term below) instead of being a straight
      // slide: an arm swings about a shoulder, so the hand is furthest from
      // the body at mid-stroke. That bulge is also what carries the blade
      // through a target rather than past it.
      float t = std::clamp(phaseTime_ / std::max(tuning.slashTime, 1e-4f), 0.0f,
                           1.0f);
      float e = t * t * (3.0f - 2.0f * t);           // smoothstep
      float bow = 4.0f * e * (1.0f - e);             // 0 at the ends, 1 mid
      Vec3 start = guard - cutDir_ * (tuning.swingReach * 0.5f);
      hand_ = start + cutDir_ * (tuning.swingReach * e) +
              fwd * (bow * tuning.swingReach * 0.28f);
      // The blade is NOT re-aimed: it keeps the angle the fist holds it at,
      // and the arm is what moves (see PlayerAvatar::SetWeaponPose). These are
      // reported for the HUD only.
      bladeDir_ = up;
      bladeUp_ = fwd;
      break;
    }

    case SwingPhase::Recover: {
      float t = std::clamp(phaseTime_ / std::max(tuning.recoverTime, 1e-4f),
                           0.0f, 1.0f);
      Vec3 target = (armed && held) ? guard : Vec3{};
      hand_ = Lerp(hand_, target, SmoothAlpha(0.07f, dt));
      bladeDir_ = up;
      bladeUp_ = fwd;
      (void)t;
      break;
    }
  }

  if (bladeDir_.len() < 1e-4f) bladeDir_ = up;
  if (bladeUp_.len() < 1e-4f) bladeUp_ = fwd;
}
