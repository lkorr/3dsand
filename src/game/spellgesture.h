#pragma once
// ---- SPELL GESTURES (assets/spells/gestures.json) ----------------------------
//
// How the arm puts a spell out into the world. A cast from a spell hand is not
// spat from the fist the tick the button goes down: the arm PERFORMS it — an
// overhand throw for a projectile, a snap for a bolt, an underhand swing for a
// lob, a slow push for an orb, a held reach for a beam, the palm drawn to the
// chest for a spell on yourself — and the spell leaves the hand at the
// gesture's RELEASE, from wherever the hand then is, along the aim then.
//
// CONTENT, by name (DESIGN.md guideline 4): a delivery glyph names its gesture
// (`"gesture": "lob"` in glyphs.json, the implicit hand in its "hand" block),
// gestures.json says what each one is, and both resolve at load and hot-reload
// with R. A modder's new carrier gets a new throw without a line of C++.
//
// A gesture is keyframes for the HAND, offset from the shoulder, in a frame
// that turns with the caster:
//   forward  the aim (frame "aim", pitched with the view) or the body's
//            level facing (frame "body"), never through the body's back;
//   up       world up made square to forward;
//   out      away from the body on the casting hand's side (a left-hand cast
//            mirrors itself through this one axis, and its torso twist).
// Lengths are in REACHES (the arm's full length), so one gesture fits any rig.
// t = 0 is implicit: the hand wherever the animation had it when the button
// went down, so nothing snaps. The pose is driven through the ONE weapon-arm
// IK claim (session.cpp PoseSpellHands); the timing — the release tick — is
// integer and on the tick, because it decides when and from where the cast
// leaves (rule 1: the same press on the same tick casts on the same tick).
//
// Header-only on purpose: the parse lives with the rest of glyphs.json's in
// spell.cpp, and the sampler is a few lerps both the tick and a gate call.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"

enum class GestureFrame : uint8_t {
  Aim = 0,   // forward is the aim ray, pitch and all
  Body,      // forward is the body's level facing (a spell on yourself)
};

enum class GestureEase : uint8_t {
  Smooth = 0,   // smoothstep: settles into and out of the key
  Linear,
  In,           // cubic in: gathers speed toward the key (a wind-up dropping)
  Out,          // cubic out: fastest as it leaves (the push, the whip)
};

struct GestureKey {
  int32_t tMs = 0;            // > 0, ascending; t = 0 is the live hand
  Vec3 hand{};                // (forward, up, out), reaches from the shoulder
  Vec3 pole{0, -1, 0};        // (forward, up, out): where the elbow bulges
  float twistDeg = 0;         // torso yaw toward the casting side's back (+) / across (-)
  float pitchDeg = 0;         // chest pitch, + up
  GestureEase ease = GestureEase::Smooth;   // the ease INTO this key
};

struct SpellGesture {
  std::string name;
  GestureFrame frame = GestureFrame::Aim;
  // TICKS, from the ms the file authors, rounded UP (30 Hz). Integer because
  // the release is when the cast happens.
  int32_t releaseTicks = 0;   // the cast leaves the hand on this tick of the clock
  int32_t holdTicks = -1;     // the clock stops here while the button is held (a beam); <0 never
  int32_t endTicks = 0;       // the last key; after it the claim hands the arm back
  std::vector<GestureKey> keys;
};

struct SpellGestureLib {
  std::vector<SpellGesture> gestures;
  // What a delivery that names no gesture (or an unknown one) plays: the
  // file's "default". -1 = no file, no gesture: the cast leaves on the press,
  // the way it did before there were gestures.
  int fallback = -1;
  int Find(const std::string& n) const {
    for (size_t i = 0; i < gestures.size(); i++)
      if (gestures[i].name == n) return (int)i;
    return -1;
  }
  const SpellGesture* At(int i) const {
    return i >= 0 && i < (int)gestures.size() ? &gestures[i] : nullptr;
  }
};

inline int32_t GestureMsToTicks(int32_t ms) {
  return ms <= 0 ? 0 : (ms * 30 + 999) / 1000;
}
inline float GestureTicksToMs(int32_t ticks) { return (float)ticks * (1000.0f / 30.0f); }

// One instant of a gesture, still in its own frame.
struct GestureSample {
  Vec3 hand{};
  Vec3 pole{0, -1, 0};
  float twistDeg = 0;
  float pitchDeg = 0;
};

inline float GestureEaseApply(GestureEase e, float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  switch (e) {
    case GestureEase::Linear: return u;
    case GestureEase::In: return u * u * u;
    case GestureEase::Out: {
      const float v = 1.0f - u;
      return 1.0f - v * v * v;
    }
    case GestureEase::Smooth:
    default: return u * u * (3.0f - 2.0f * u);
  }
}

// `from`: the live hand at the start, in the gesture's frame (reaches). The
// implicit t = 0 key holds it, the first key's pole and no torso.
inline GestureSample SampleGesture(const SpellGesture& g, float tMs, Vec3 from) {
  GestureSample s;
  s.hand = from;
  if (g.keys.empty()) return s;
  GestureKey prev;
  prev.tMs = 0;
  prev.hand = from;
  prev.pole = g.keys[0].pole;
  for (const GestureKey& k : g.keys) {
    if (tMs < (float)k.tMs) {
      const float span = (float)std::max(1, k.tMs - prev.tMs);
      const float e = GestureEaseApply(k.ease, (tMs - (float)prev.tMs) / span);
      s.hand = prev.hand + (k.hand - prev.hand) * e;
      s.pole = prev.pole + (k.pole - prev.pole) * e;
      s.twistDeg = prev.twistDeg + (k.twistDeg - prev.twistDeg) * e;
      s.pitchDeg = prev.pitchDeg + (k.pitchDeg - prev.pitchDeg) * e;
      return s;
    }
    prev = k;
  }
  s.hand = prev.hand;
  s.pole = prev.pole;
  s.twistDeg = prev.twistDeg;
  s.pitchDeg = prev.pitchDeg;
  return s;
}

// THE FRAME, in world axes. `aim` is the view ray, `bodyFwd` the body's level
// facing, `side` +1 for the right hand and -1 for the left. The forward axis
// is kept in front of the body (a camera looking back at the caster's face
// would otherwise ask the arm to reach behind its own back); pitch is the
// aim's. Up is world up squared to it; out = forward x up, which is the
// body's right (anim.cpp AnimApplySpineTwist names the same basis), flipped
// for the left hand.
struct GestureBasis {
  Vec3 fwd{0, 0, 1}, up{0, 1, 0}, out{-1, 0, 0};
  Vec3 ToWorld(Vec3 v) const { return fwd * v.x + up * v.y + out * v.z; }
  Vec3 FromWorld(Vec3 w) const {
    return Vec3{w.x * fwd.x + w.y * fwd.y + w.z * fwd.z, w.x * up.x + w.y * up.y + w.z * up.z,
                w.x * out.x + w.y * out.y + w.z * out.z};
  }
};
inline GestureBasis MakeGestureBasis(GestureFrame frame, Vec3 aim, Vec3 bodyFwd, float side) {
  GestureBasis b;
  Vec3 level{bodyFwd.x, 0, bodyFwd.z};
  level = level.len() > 1e-4f ? level.normalized() : Vec3{0, 0, 1};
  Vec3 f = level;
  if (frame == GestureFrame::Aim && aim.len() > 1e-4f) {
    f = aim.normalized();
    const float along = f.x * level.x + f.z * level.z;
    if (along < 0.3f) f = (f + level * (0.3f - along)).normalized();
  }
  // Up squared to forward. Looking almost straight up or down, world up is
  // nearly parallel to it; the body's facing tilted back stands in.
  Vec3 u = Vec3{0, 1, 0} - f * f.y;
  if (u.len() < 0.2f) u = level * (f.y > 0 ? -1.0f : 1.0f);
  u = u.normalized();
  const Vec3 right{f.y * u.z - f.z * u.y, f.z * u.x - f.x * u.z, f.x * u.y - f.y * u.x};
  b.fwd = f;
  b.up = u;
  b.out = right * side;
  return b;
}
