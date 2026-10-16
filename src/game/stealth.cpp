#include "game/stealth.h"

#include <algorithm>
#include <cmath>

#include "sim/tuning.h"

namespace stealth {

Params FromTuning(const Tuning& t) {
  const auto& s = t.stealth;
  Params p;
  p.cautionAt = s.cautionAt;
  p.investigateAt = std::max(s.cautionAt, s.investigateAt);
  p.soundCap = std::clamp(s.soundCap, 0.0f, 0.999f);
  p.sightRate = s.sightRate;
  p.soundRate = s.soundRate;
  p.decay = s.decay;
  p.searchDecay = s.searchDecay;
  p.searchHold = s.searchHold;
  p.searchGainMul = s.searchGainMul;
  p.alertedGainMul = s.alertedGainMul;
  p.alertedTicks = (uint32_t)std::max(0, s.alertedTicks);
  p.sightFalloff = s.sightFalloff;
  p.nearAlways = s.nearAlways;
  p.lightFloor = s.lightFloor;
  p.lightGamma = s.lightGamma;
  p.crouchVis = s.crouchVis;
  p.proneVis = s.proneVis;
  p.stillVis = s.stillVis;
  p.runVis = s.runVis;
  p.sneakVisCut = s.sneakVisCut;
  p.awarenessReach = s.awarenessReach;
  p.refHeight = s.refHeight;
  p.combatSightMul = s.combatSightMul;
  p.combatLightFloor = s.combatLightFloor;
  p.keepGain = s.keepGain;
  p.walkNoise = s.walkNoise;
  p.crouchNoise = s.crouchNoise;
  p.proneNoise = s.proneNoise;
  p.runNoisePow = s.runNoisePow;
  p.metalNoise = s.metalNoise;
  p.sneakNoiseCut = s.sneakNoiseCut;
  p.splashNoise = s.splashNoise;
  p.muffled = s.muffled;
  p.rainMask = s.rainMask;
  p.alarmGain = s.alarmGain;
  p.landNoise = s.landNoise;
  p.landRefSpeed = s.landRefSpeed;
  p.strikeNoise = s.strikeNoise;
  p.voiceNoise = s.voiceNoise;
  p.impactNoise = s.impactNoise;
  p.twangNoise = s.twangNoise;
  p.lightFull = std::max(0.01f, s.lightFull);
  p.rainKeyDim = s.rainKeyDim;
  p.burningLight = s.burningLight;
  p.sneakMelee = s.sneakMelee;
  p.sneakRanged = s.sneakRanged;
  p.sneakNeedsCrouch = s.sneakNeedsCrouch;
  p.playerSneak = s.playerSneak;
  return p;
}

const Params& Defaults() {
  // A default Tuning IS the .def's defaults (every member initialiser is a
  // TPD). Built once; the Tuning itself is not kept.
  static const Params p = FromTuning(Tuning{});
  return p;
}

const char* AlertName(Alert a) {
  switch (a) {
    case Alert::Calm: return "calm";
    case Alert::Suspicious: return "suspicious";
    case Alert::Searching: return "searching";
    case Alert::Combat: return "combat";
  }
  return "?";
}

namespace {
float Clamp01(float x) { return std::clamp(x, 0.0f, 1.0f); }
}  // namespace

// ---------------------------------------------------------------------------
// SIGHT
//
// Visibility is a product of independent terms, Skyrim's shape (its detection
// formula multiplies a light term, a movement term and the sneak skill's), and
// each is a number a designer can read: a lit person walking is ~1.
//
//   light   lightFloor + (1 - lightFloor) x light^gamma, never below the
//           viewer's own floor (dark vision; a creature already fighting the
//           body). `light` is the body's ON-SCREEN brightness over
//           stealth.lightFull (MobSystem::LightAt: the renderer's own body
//           lighting, tonemapped), already perceptual -- gamma > 1 is the
//           designer's contrast: how much more a shadow hides than it looks.
//   stance  1 / crouchVis / proneVis.
//   motion  stillVis for a body that does not move, 1 at its walk, and
//           runVis more per walk-speed past that: the eye catches motion.
//   size    sqrt(effective height / refHeight), 0.5..2. A crouched body is
//           a smaller body; the stance term is the posture, this the area.
//   sneak   1 - sneakVisCut x skill/100.
float Visibility(const Params& p, const Exposure& e, float heightVox,
                 float lightFloor) {
  float L = p.lightFloor + (1.0f - p.lightFloor) *
                               std::pow(Clamp01(e.light), std::max(0.05f, p.lightGamma));
  L = std::max(L, Clamp01(lightFloor));
  // A body ALIGHT is a lamp, whatever the night is doing.
  L = std::max(L, Clamp01(e.burning) * p.burningLight);
  const float stance = e.stance == Stance::Crouch ? p.crouchVis
                       : e.stance == Stance::Prone ? p.proneVis
                                                   : 1.0f;
  const float mf = std::max(0.0f, e.moveFrac);
  const float motion = p.stillVis + (1.0f - p.stillVis) * std::min(1.0f, mf) +
                       p.runVis * std::max(0.0f, mf - 1.0f);
  const float effH = heightVox * (e.stance == Stance::Crouch ? 0.65f
                                  : e.stance == Stance::Prone ? 0.3f
                                                              : 1.0f);
  const float size =
      std::clamp(std::sqrt(std::max(0.0f, effH) / std::max(1.0f, p.refHeight)), 0.5f, 2.0f);
  const float sneak = 1.0f - p.sneakVisCut * Clamp01(e.sneak / 100.0f);
  return L * stance * motion * size * std::max(0.0f, sneak);
}

float SightReach(const Params& p, float sightRange, float awareness, float vis) {
  const float aw = 0.6f + p.awarenessReach * Clamp01(awareness / 100.0f);
  return std::max(0.0f, sightRange) * aw * std::max(0.0f, vis);
}

// The cone. Inside the CENTRAL cone (Splinter Cell's guards see sharply ahead,
// dimly to the side) the gain is whole; from its edge to the edge of the
// field of view it falls linearly to `peripheral`; past the field of view it
// is 0 -- the blind spot behind a guard is real.
float ConeGain(float offAxis, float fovDeg, float centralDeg, float peripheral) {
  const float a = std::abs(offAxis);
  constexpr float kDeg = 3.14159265f / 180.0f;
  if (fovDeg >= 359.9f && centralDeg >= 359.9f) return 1.0f;
  const float half = 0.5f * std::min(360.0f, fovDeg) * kDeg;
  const float core = std::min(half, 0.5f * std::max(0.0f, centralDeg) * kDeg);
  if (fovDeg < 359.9f && a > half) return 0.0f;
  if (a <= core) return 1.0f;
  // Omnidirectional field but a narrower core: the fall reaches `peripheral`
  // directly behind.
  const float edge = fovDeg >= 359.9f ? 3.14159265f : half;
  const float t = edge > core ? (a - core) / (edge - core) : 1.0f;
  return 1.0f + (Clamp01(peripheral) - 1.0f) * Clamp01(t);
}

// The rate: sightRate at point blank, (1 - d/reach)^falloff of it out to the
// reach. The METER also drains at `decay`, so the edge at which a body is
// noticed at all is where this falls below the decay -- about 80% of reach
// with the shipped numbers -- and a body near that edge fills it slowly and
// loses it the moment it stops moving.
float SightGain(const Params& p, float d, float reach, float cone) {
  if (reach <= 0.0f || d >= reach || cone <= 0.0f) return 0.0f;
  const float x = 1.0f - std::max(0.0f, d) / reach;
  return p.sightRate * cone * std::pow(x, std::max(0.1f, p.sightFalloff));
}

// ---------------------------------------------------------------------------
// SOUND
//
// Every noise is a RADIUS: where a listener of awareness 50 in silence just
// hears it. A walk on stone is walkNoise; speed raises it by moveFrac^pow (a
// sprint is loud out of proportion -- footfalls land harder AND faster), the
// crouch and the crawl cut it, worn metal and a hard floor raise it, and the
// sneak skill quiets all of it. Wading is its own splash.
float MovementNoise(const Params& p, Stance st, float moveFrac, float metal,
                    float surface, float wading, float sneak) {
  const float mf = std::max(0.0f, moveFrac);
  if (mf < 0.02f) return 0.0f;
  float n = p.walkNoise * std::pow(mf, std::max(0.5f, p.runNoisePow));
  n *= st == Stance::Crouch ? p.crouchNoise : st == Stance::Prone ? p.proneNoise : 1.0f;
  n *= 1.0f + p.metalNoise * Clamp01(metal);
  n *= std::max(0.0f, surface);
  n += p.splashNoise * Clamp01(wading) * std::min(mf, 2.0f);
  n *= 1.0f - p.sneakNoiseCut * Clamp01(sneak / 100.0f);
  return std::max(0.0f, n);
}

// THE AMBIENT FLOOR (Chaos Theory's marker on the sound meter). A noise is
// heard while its level at the listener -- radius / distance -- is above
// 1 + ambient: in a downpour every sound reaches about half as far.
float Ambient(const Params& p, uint32_t rainAmount) {
  return std::max(0.0f, p.rainMask) * Clamp01((float)rainAmount / 255.0f);
}

namespace {
float HeardRadius(const Params& p, float radius, float hearing, float awareness,
                  float ambient, bool blocked) {
  const float aw = 0.6f + p.awarenessReach * Clamp01(awareness / 100.0f);
  float r = std::max(0.0f, radius) * std::max(0.0f, hearing) * aw;
  if (blocked) r *= Clamp01(p.muffled);
  return r / (1.0f + std::max(0.0f, ambient));
}
}  // namespace

float SoundGain(const Params& p, float radius, float d, float hearing,
                float awareness, float ambient, bool blocked) {
  const float r = HeardRadius(p, radius, hearing, awareness, ambient, blocked);
  if (r <= 0.0f || d >= r) return 0.0f;
  return p.soundRate * (1.0f - std::max(0.0f, d) / r);
}

// An EVENT is heard once: the meter jumps by (1 - d/r), so a blow next to a
// creature is a near-certainty of "what was that", a distant clang a nudge.
float SoundImpulse(const Params& p, float radius, float d, float hearing,
                   float awareness, float ambient, bool blocked) {
  const float r = HeardRadius(p, radius, hearing, awareness, ambient, blocked);
  if (r <= 0.0f || d >= r) return 0.0f;
  return 1.0f - std::max(0.0f, d) / r;
}

}  // namespace stealth
