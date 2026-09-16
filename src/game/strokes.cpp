#include "game/strokes.h"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "sim/rng.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Distinct salt per draw KIND, so the style pick, the start bow and the tempo
// are independent sequences rather than three views of one hash. Same
// convention ai_behavior.cpp uses for its attack cadence.
constexpr uint32_t kSaltStyle = 0x51E1Eu;
constexpr uint32_t kSaltBow = 0x8014Du;

StrokeSegment ReadSegment(const json& j, StrokeSegment dflt) {
  StrokeSegment s = dflt;
  if (!j.is_object()) return s;
  s.ticks = std::max(1, j.value("ticks", s.ticks));
  s.az = j.value("az", s.az);
  s.el = j.value("el", s.el);
  s.reach = j.value("reach", s.reach);
  return s;
}

}  // namespace

bool LoadAttackStyles(const std::string& path, StyleLibrary& out,
                      std::string& log) {
  std::ifstream f(path);
  if (!f) {
    log += path + ": missing — NPCs will request attacks and never swing\n";
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    log += path + ": JSON parse error: " + e.what() + "\n";
    return false;
  }

  StyleLibrary lib;
  for (const auto& s : j.value("styles", json::array())) {
    AttackStyle st;
    st.name = s.value("name", "");
    if (st.name.empty()) {
      log += path + ": a style with no \"name\" was skipped\n";
      continue;
    }
    st.label = s.value("label", st.name);
    st.windup = ReadSegment(s.value("windup", json::object()),
                            StrokeSegment{12, 0.30f, 0.10f, -0.05f});
    st.cut = ReadSegment(s.value("cut", json::object()),
                         StrokeSegment{7, -2.0f, 0.0f, 0.10f});
    if (s.contains("recover") && s["recover"].is_object())
      st.recoverTicks = std::max(1, s["recover"].value("ticks", 10));
    if (s.contains("jitter") && s["jitter"].is_object()) {
      const auto& q = s["jitter"];
      st.jitter.az = q.value("az", 0.0f);
      st.jitter.el = q.value("el", 0.0f);
      // Clamped well under 1: a tempo jitter of 1 would allow a zero-tick
      // windup, i.e. an attack with no telegraph at all, which is a content
      // error rather than a character choice.
      st.jitter.tempo = std::clamp(q.value("tempo", 0.0f), 0.0f, 0.6f);
    }
    // The optional body animation (strokes.h AttackStyle::clip). Whether a
    // rig HAS it is a mob-load question, answered there with a loader line.
    st.clip = s.value("clip", std::string());
    // ---- WHAT SWINGS IT, and when (strokes.h; plan §4/§5) ------------------
    // Every one of these defaults to the behaviour a style authored before
    // they existed already had: the held weapon, never a fallback, the
    // profile's reach, no leap, the chest. So the shipped library loads
    // unchanged on a binary that has them and a file written against them
    // loads on one that does not (the loader ignores unknown keys by design).
    st.weapon = s.value("weapon", std::string("held"));
    if (st.weapon.empty()) st.weapon = "held";
    st.fallback = s.value("fallback", false);
    st.reach = std::max(0.0f, s.value("reach", 0.0f));
    if (s.contains("lunge") && s["lunge"].is_object()) {
      const auto& l = s["lunge"];
      st.lunge.ticks = std::max(0, l.value("ticks", 0));
      st.lunge.speed = std::max(0.0f, l.value("speed", 0.0f));
      st.lunge.rise = std::max(0.0f, l.value("rise", 0.0f));
      st.lunge.at = l.value("at", std::string("windup"));
      // Only two phases can OPEN a lunge: a leap during the recover is a
      // creature throwing itself at somebody after the blow has landed, which
      // is a content error rather than a move.
      if (st.lunge.at != "windup" && st.lunge.at != "cut") {
        log += path + ": style \"" + st.name + "\" lunges at \"" +
               st.lunge.at + "\", which is not \"windup\" or \"cut\" — using "
               "\"windup\"\n";
        st.lunge.at = "windup";
      }
      if (st.lunge.ticks > 0 && !st.lunge.Any())
        log += path + ": style \"" + st.name +
               "\" has a lunge with no speed and no rise — it will not leap\n";
    }
    if (s.contains("target") && s["target"].is_object()) {
      for (auto t = s["target"].begin(); t != s["target"].end(); ++t) {
        if (!t.value().is_number()) continue;
        const float w = t.value().get<float>();
        if (w <= 0.0f) continue;   // a zero weight IS the absent entry
        st.target.push_back(StyleTargetWeight{t.key(), w});
      }
      // Deterministic order, because the draw walks this vector and JSON
      // object order is the library's business, not the author's. Two files
      // that spell the same weights in a different order must pick the same
      // limb from the same (mob, tick) — that is rule 1 applied to content.
      std::sort(st.target.begin(), st.target.end(),
                [](const StyleTargetWeight& a, const StyleTargetWeight& b) {
                  return a.tag < b.tag;
                });
    }
    // A CUT THAT GOES NOWHERE IS NOT A CUT. It would pose the blade, commit
    // nothing, and hand back a stroke that could never damage anything — and
    // the only symptom would be an NPC that swings and never hits, which is
    // exactly the sort of content bug that gets blamed on the damage path.
    const float travel = std::fabs(st.cut.az) + std::fabs(st.cut.el) +
                         std::fabs(st.cut.reach);
    if (travel < 1e-3f) {
      log += path + ": style \"" + st.name +
             "\" has a cut that travels nowhere — skipped\n";
      continue;
    }
    if (lib.Find(st.name) >= 0)
      log += path + ": duplicate style \"" + st.name + "\" — last wins\n";
    lib.styles.push_back(std::move(st));
  }
  if (lib.styles.empty())
    log += path + ": no usable styles — NPCs will not swing\n";

  // ---- the player's flick compass (strokes.h PlayerStrikeMap) --------------
  // Parsed AFTER the styles so sectors resolve to indices right here, and
  // skipped as loudly as a bad style: a compass pointing at nothing is a
  // player who clicks and does not swing, which must never be silent.
  //
  // TWO BLOCKS THROUGH ONE READER: `player` (a weapon in the fist) and
  // `playerUnarmed` (fists). One function, because a second copy of "resolve a
  // sector to an index and skip it loudly" is a second place for the skip to
  // stop being loud.
  auto readMap = [&](const char* key, PlayerStrikeMap& map) {
    if (!j.contains(key) || !j[key].is_object()) return;
    const auto& p = j[key];
    for (const auto& s : p.value("sectors", json::array())) {
      if (!s.is_object()) continue;
      PlayerStrikeMap::Sector sec;
      const auto& d = s.value("dir", json::array());
      if (d.is_array() && d.size() >= 2 && d[0].is_number() &&
          d[1].is_number()) {
        sec.x = d[0].get<float>();
        sec.y = d[1].get<float>();
      }
      const std::string name = s.value("style", "");
      sec.style = lib.Find(name);
      if (sec.style < 0 || (sec.x == 0.0f && sec.y == 0.0f)) {
        log += path + ": " + key + " sector -> \"" + name +
               "\" is unknown or directionless — skipped\n";
        continue;
      }
      map.sectors.push_back(sec);
    }
    const auto& na = p.value("neutralAlternate", json::array());
    for (size_t i = 0; i < 2 && i < na.size(); i++) {
      if (!na[i].is_string()) continue;
      const std::string name = na[i].get<std::string>();
      map.neutral[i] = lib.Find(name);
      if (map.neutral[i] < 0)
        log += std::string(path) + ": " + key + " neutralAlternate \"" + name +
               "\" is unknown — skipped\n";
    }
  };
  readMap("player", lib.player);
  readMap("playerUnarmed", lib.playerUnarmed);
  out = std::move(lib);
  return true;
}

int QuantizeStrike(const PlayerStrikeMap& map, float dx, float dy) {
  // Max dot, not sector angles: adding a direction is adding a line of JSON,
  // and two sectors that overlap simply split at their bisector.
  int best = -1;
  float bestDot = -1e9f;
  for (const PlayerStrikeMap::Sector& s : map.sectors) {
    const float d = s.x * dx + s.y * dy;
    if (d > bestDot) {
      bestDot = d;
      best = s.style;
    }
  }
  return best;
}

int NeutralStrike(const PlayerStrikeMap& map, bool right) {
  const int a = map.neutral[right ? 0 : 1];
  const int b = map.neutral[right ? 1 : 0];
  return a >= 0 ? a : b;
}

// ============================================================================
// THE SHARED STROKE-PROGRAM RUNNER (moved from MobSystem::StepStroke when the
// player's discrete attacks became a second caller — strokes.h says why).
// ============================================================================

// WHERE AN ARM SITS WHEN IT IS NEITHER CHAMBERED NOR EXTENDED, as a position
// WITHIN THE REACH BAND (MeleeState::ReachBand) rather than as a fraction of
// the arm. Every authored `reach` in a style is an offset from this, so a style
// that says nothing about reach holds a normal guard and a thrust's -0.45/+0.85
// pair reads as "chamber a little, then drive to full extension" on any rig.
//
// THE BAND AND THE ARM ARE NOT THE SAME LENGTH, which is the whole reason this
// is stated here. The point rides a blade's length off a hand that is itself
// most of an arm out, so the two lengths largely cancel: measured on the human
// rig the arm is ~7 voxels and the band the driver can serve is [3.8, 6.5].
// Authored against the ARM, every chamber and lunge in the library landed
// outside that and was clamped — the commanded radius moved 0.15 voxels on a
// stroke asking for four, and a thrust read as a twitch.
//
// A little past the middle, because a guard is held forward of centre.
constexpr float kNeutralReach = 0.60f;

float StrokeReachIn(const MeleeState& m, float offset) {
  float lo = 0, hi = 0;
  m.ReachBand(lo, hi);
  const float span = hi > lo ? hi - lo : 0.0f;
  return std::clamp(lo + (kNeutralReach + offset) * span, lo, hi);
}

void StrokeAimAt(const Vec3& pivot, const Vec3& target, const Vec3& right,
                 const Vec3& up, const Vec3& fwd, float& outAz, float& outEl,
                 float& outDist) {
  const Vec3 to = target - pivot;
  const Vec3 local{to.dot(right), to.dot(up), to.dot(fwd)};
  const float r = std::max(local.len(), 1e-4f);
  outAz = std::atan2(local.x, local.z);
  outEl = std::asin(std::clamp(local.y / r, -1.0f, 1.0f));
  outDist = r;
}

void BeginStrokeProgram(StrokeCursor& cur, const AttackStyle& sty,
                        int styleIndex, uint32_t seed) {
  cur.style = styleIndex;
  cur.seed = seed;
  const float tempo =
      1.0f + sty.jitter.tempo * rng::SignedUnit(rng::Hash3(seed, 1, 0));
  cur.windupTicks = std::max(2, (int)std::lround(sty.windup.ticks * tempo));
  cur.cutTicks = std::max(2, (int)std::lround(sty.cut.ticks * tempo));
  cur.recoverTicks = std::max(1, sty.recoverTicks);
  cur.phase = StrokeCursor::Phase::Windup;
  cur.phaseTick = 0;
}

StrokeStepResult StepStrokeProgram(StrokeCursor& cur, const AttackStyle* sty,
                                   MeleeState& m, float liveAz, float liveEl,
                                   float liveDist, float dt, const Vec3& right,
                                   const Vec3& up, const Vec3& fwd) {
  // THE POINT MAY NOT BE DRIVEN MORE THAN A LITTLE PAST THE TARGET (strokes.h
  // says why at length). A fifth past it is the follow-through a blow needs to
  // pass THROUGH what it hits rather than stopping on its surface; beyond that
  // the weapon is only travelling somewhere the target is not.
  auto toTarget = [&](float banded) {
    return liveDist > 0.0f ? std::min(banded, liveDist * 1.2f) : banded;
  };
  // The start bow: a deterministic wobble on where the windup lands, so ten
  // swings do not look stamped. Zero for a guard, which has no style behind
  // it — and zero for the player's styles, which author `jitter` at 0 so a
  // strike goes exactly where it was flicked.
  const float bowAz =
      sty ? sty->jitter.az * rng::SignedUnit(rng::Hash3(cur.seed, 2, 0)) : 0.0f;
  const float bowEl =
      sty ? sty->jitter.el * rng::SignedUnit(rng::Hash3(cur.seed, 3, 0)) : 0.0f;
  StrokeSample smp;
  smp.held = true;
  // THE CLOSED-LOOP, UNDER-COMMIT DRIVE, shared by Guard and Windup because
  // they are the same motion: steer the stored stroke toward a stated pose
  // slowly enough that `commitSpeed` never fires. 16 units/tick is 480 px/s
  // against a 900 px/s threshold, so the driver stays in Guard however far it
  // has to travel — which is what makes a windup a readable telegraph rather
  // than an instant snap.
  auto steerTo = [&](float wantAz, float wantEl, float wantR) {
    const MeleeTuning& t = m.tuning;
    smp.dx = std::clamp((wantAz - m.StrokeAz()) / t.aimGainX, -16.0f, 16.0f);
    smp.dy = std::clamp(-(wantEl - m.StrokeEl()) / t.aimGainY, -16.0f, 16.0f);
    smp.dReach = std::clamp(
        (wantR - m.StrokeRadius()) / std::max(t.reachGain, 1e-4f), -18.0f,
        18.0f);
    m.Step(smp, dt, true, right, up, fwd);
  };

  switch (cur.phase) {
    case StrokeCursor::Phase::Guard: {
      // ABSOLUTE, not aim-relative: a guard is a pose, not a blow.
      // `wantReach` is a BAND POSITION (0 = as drawn back as this arm goes,
      // 1 = fully extended), resolved against the live arm so the same guard
      // is the same guard on any rig.
      float lo = 0, hi = 0;
      m.ReachBand(lo, hi);
      steerTo(cur.wantAz, cur.wantEl,
              std::clamp(lo + cur.wantReach * (hi - lo), lo, hi));
      break;
    }
    case StrokeCursor::Phase::Windup: {
      if (sty == nullptr) break;
      // THE CUT IS CENTRED ON THE AIM, so the windup lands HALF A CUT SHORT of
      // it: `aim - cut/2 + windup offset`. A stroke that started at the aim
      // would cut the air behind the target every time — the blade only ever
      // travels away from where it began.
      cur.wantAz = liveAz - 0.5f * sty->cut.az + sty->windup.az + bowAz;
      cur.wantEl = liveEl - 0.5f * sty->cut.el + sty->windup.el + bowEl;
      // AGAINST A NEUTRAL EXTENSION, not against the live radius. Computing
      // this as `StrokeRadius() + offset` every windup tick is a RUNAWAY: each
      // tick re-targets a further offset from wherever the last one landed, so
      // a thrust's rear chamber walked the point straight into the bottom of
      // the arm's reach band and stayed there — measured, the cut that
      // followed then extended 1.07 voxels instead of nine and spent its
      // travel flailing 1.13 rad of elevation instead. `kNeutralReach` is what
      // an arm sits at when it is neither chambered nor extended, so an
      // authored `reach` of 0 is "wherever a guard holds it" and the offsets
      // are readable as what they are.
      cur.wantReach = toTarget(StrokeReachIn(m, sty->windup.reach));
      steerTo(cur.wantAz, cur.wantEl, cur.wantReach);
      if (++cur.phaseTick >= cur.windupTicks) {
        // ---- COMMIT. The aim is frozen HERE and never refreshed: a target
        // that steps offline after this instant is missed, which is what makes
        // the windup a real telegraph rather than decoration.
        cur.aimAz = liveAz;
        cur.aimEl = liveEl;
        cur.aimed = true;
        cur.phase = StrokeCursor::Phase::Cut;
        cur.phaseTick = 0;
      }
      break;
    }
    case StrokeCursor::Phase::Cut: {
      if (sty == nullptr) break;
      // FROM WHERE THE BLADE ACTUALLY IS, THROUGH THE AIM, TO HALF A CUT PAST
      // IT. Derived per tick from the live stroke state rather than baked at
      // commit, so a windup that could not quite reach its pose (a clamped
      // shoulder, a short arm) still produces a cut through the target instead
      // of one displaced by however much the arm fell short.
      const float toAz = cur.aimAz + 0.5f * sty->cut.az;
      const float toEl = cur.aimEl + 0.5f * sty->cut.el;
      const float toR =
          toTarget(StrokeReachIn(m, sty->windup.reach + sty->cut.reach));
      const int left = std::max(1, cur.cutTicks - cur.phaseTick);
      const MeleeTuning& t = m.tuning;
      smp.dx = ((toAz - m.StrokeAz()) / (float)left) / t.aimGainX;
      smp.dy = -((toEl - m.StrokeEl()) / (float)left) / t.aimGainY;
      smp.dReach = ((toR - m.StrokeRadius()) / (float)left) /
                   std::max(t.reachGain, 1e-4f);
      m.Step(smp, dt, true, right, up, fwd);
      if (++cur.phaseTick >= cur.cutTicks) {
        cur.phase = StrokeCursor::Phase::Recover;
        cur.phaseTick = 0;
      }
      break;
    }
    case StrokeCursor::Phase::Recover: {
      // Button RELEASED: the driver's own recover ramps PoseWeight down and
      // hands the arm back to the walk cycle, which is the same hand-back the
      // player gets and the reason nothing snaps here.
      smp.held = false;
      m.Step(smp, dt, true, right, up, fwd);
      if (++cur.phaseTick >= cur.recoverTicks) return StrokeStepResult::Finished;
      break;
    }
    default:
      return StrokeStepResult::Idle;
  }
  return StrokeStepResult::Live;
}

int PickAttackStyle(const StyleLibrary& lib,
                    const std::vector<std::string>& names, uint64_t mobId,
                    uint32_t tick, const Mob* who, float distance, float slack) {
  if (lib.empty()) return -1;
  // RESOLVE BY NAME, THEN FILTER, THEN PICK, and the order is load-bearing at
  // every step.
  //
  //   * RESOLVE FIRST is what makes a profile that lists one unknown style
  //     among four still vary over the other three instead of stuttering on
  //     the hole: the draw is over what actually exists.
  //   * FILTER BEFORE DRAWING, not after: drawing and then rejecting would
  //     make a disarmed duelist miss three turns in four while it rolled its
  //     way onto its one punch, which reads as a creature that has stopped
  //     fighting rather than as one that has lost its sword.
  //   * FALLBACKS LAST, and as a GROUP. A style is not "worse", it is "only
  //     when there is nothing better" — so the test is whether ANY
  //     non-fallback style survived the usability filter, not a per-style
  //     comparison. That is what makes an armed duelist's punches invisible
  //     and a disarmed one's the whole of its repertoire.
  //   * AND THE REACH FILTER SITS WITH USABILITY, not after the draw. See the
  //     header: a style that cannot arrive is not a worse choice, it is not a
  //     choice, and rolling onto one spends the whole cadence on nothing.
  int found[8];
  int n = 0;
  bool haveReal = false;
  for (const std::string& s : names) {
    if (n >= 8) break;
    const int i = lib.Find(s);
    if (i < 0) continue;
    if (who != nullptr && !StyleUsable(*who, lib.styles[i])) continue;
    if (who != nullptr && distance > 0.0f) {
      // A style that reports no reach at all (nothing to measure, no rig
      // answer) is NOT filtered out: 0 means "unknown", not "zero voxels", and
      // the same convention the refusal in BeginStroke uses. Being wrong in the
      // permissive direction here costs one dropped swing; being wrong in the
      // strict one silently deletes a creature's whole repertoire.
      const float r = StyleReachOn(*who, lib.styles[i]);
      if (r > 0.0f && distance > r + slack) continue;
    }
    found[n++] = i;
    haveReal = haveReal || !lib.styles[i].fallback;
  }
  if (haveReal) {
    int keep = 0;
    for (int k = 0; k < n; k++)
      if (!lib.styles[found[k]].fallback) found[keep++] = found[k];
    n = keep;
  }
  if (n == 0) return -1;
  if (n == 1) return found[0];
  const uint32_t h = rng::Hash3((uint32_t)mobId ^ kSaltStyle, tick, 0);
  return found[h % (uint32_t)n];
}

