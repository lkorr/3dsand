#include <cstdlib>
#include "game/strokes.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>

#include "sim/rng.h"

namespace {
// An angle ABOUT AN AXIS (a lean, an elbow swivel), brought into [-pi, pi]:
// any number is one of these, and a blend between two must go the short way.
float WrapPi(float a) {
  if (!std::isfinite(a)) return 0.0f;
  return std::remainder(a, 6.28318531f);
}
}  // namespace
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Distinct salt per draw KIND, so the style pick, the start bow and the tempo
// are independent sequences rather than three views of one hash. Same
// convention ai_behavior.cpp uses for its attack cadence.
constexpr uint32_t kSaltStyle = 0x51E1Eu;
constexpr uint32_t kSaltBow = 0x8014Du;

// A SEGMENT'S OWN PACING (strokes.h "PER-SEGMENT PACING"). Leaves `paced`
// alone when the key is absent, so a merge (the player block) inherits; an
// unknown name is REPORTED rather than silently read as linear, for the
// reason the style-wide ease is (ParseEase cannot tell a typo from "linear").
void ReadEaseKey(const json& j, bool& paced, Ease& ease,
                 const std::string& where, std::string& log) {
  if (!j.is_object() || !j.contains("ease") || !j["ease"].is_string()) return;
  const std::string en = j["ease"].get<std::string>();
  const Ease e = ParseEase(en);
  if (e == Ease::Linear && en != "linear") {
    log += where + " has unknown ease \"" + en + "\" — ignored\n";
    return;
  }
  paced = true;
  ease = e;
}

StrokeSegment ReadSegment(const json& j, StrokeSegment dflt,
                          const std::string& where, std::string& log) {
  StrokeSegment s = dflt;
  if (!j.is_object()) return s;
  s.ticks = std::max(1, j.value("ticks", s.ticks));
  s.az = j.value("az", s.az);
  s.el = j.value("el", s.el);
  s.reach = j.value("reach", s.reach);
  ReadEaseKey(j, s.paced, s.ease, where, log);
  return s;
}

// THE PER-JOINT BRAKES (strokes.h AttackStyle::joints, melee.h ArmSmooth).
// Field-wise onto `out`, so the player block merges the same way the
// segments do: an unstated field inherits. Negative is clamped to 0 (off).
void ReadJoints(const json& j, ArmSmooth& out) {
  if (!j.is_object()) return;
  static const char* kNames[kArmJoints] = {"shoulder", "elbow", "wrist"};
  for (int k = 0; k < kArmJoints; k++) {
    if (!j.contains(kNames[k]) || !j[kNames[k]].is_object()) continue;
    const json& q = j[kNames[k]];
    ArmJointSmooth& js = out.joint[k];
    js.smooth = std::clamp(q.value("smooth", js.smooth), 0.0f, 60.0f);
    js.maxDeg = std::clamp(q.value("maxDeg", js.maxDeg), 0.0f, 180.0f);
  }
}

// ---- A FRAME IS A POSE (strokes.h) ----------------------------------------
// [x, y, z, w] -> a unit quaternion; false for anything else.
bool ReadQuat(const json& j, Quat& q) {
  if (!j.is_array() || j.size() != 4) return false;
  float v[4];
  for (int i = 0; i < 4; i++) {
    if (!j[i].is_number()) return false;
    v[i] = j[i].get<float>();
    if (!std::isfinite(v[i])) return false;
  }
  q = QuatNormalize(Quat{v[0], v[1], v[2], v[3]});
  return true;
}
// A pose needs its shoulder and elbow; the wrist is optional.
StrokePose ReadPose(const json& j) {
  StrokePose p;
  if (!j.is_object()) return p;
  static const char* kNames[kArmJoints] = {"shoulder", "elbow", "wrist"};
  for (int k = 0; k < kArmJoints; k++)
    if (j.contains(kNames[k]))
      p.hasJoint[k] = ReadQuat(j[kNames[k]], p.joint[k]);
  p.has = p.hasJoint[kArmShoulder] && p.hasJoint[kArmElbow];
  const auto num = [&](const char* key) {
    return j.contains(key) && j[key].is_number() ? j[key].get<float>() : 0.0f;
  };
  p.twist = num("twist");
  p.pitch = num("pitch");
  if (!std::isfinite(p.twist)) p.twist = 0.0f;
  if (!std::isfinite(p.pitch)) p.pitch = 0.0f;
  return p;
}

// ---- FRAMES (strokes.h "AN ATTACK IS A LIST OF FRAMES") -------------------
//
// One reader for every frame, because every frame is the same kind of thing.
// `styleJoints` is the style-level `joints` block, if any: a frame that states
// none of its own inherits it, and one that states some merges onto it field
// by field (ReadJoints' convention).
StrokeFrame ReadFrame(const json& j, const ArmSmooth& styleJoints,
                      const std::string& where, std::string& log) {
  StrokeFrame f;
  f.joints = styleJoints;
  if (!j.is_object()) return f;
  f.name = j.value("name", std::string());
  f.ticks = std::max(1, j.value("ticks", f.ticks));
  f.az = j.value("az", 0.0f);
  f.el = j.value("el", 0.0f);
  f.reach = j.value("reach", 0.0f);
  const std::string from = j.value("from", std::string("target"));
  if (from != "target" && from != "body")
    log += where + " has unknown \"from\": \"" + from +
           "\" — measuring from the target\n";
  f.fromBody = from == "body";
  bool paced = false;
  if (j.value("ease", std::string()) == "chase") {
    f.chase = true;
    f.chaseRate = std::clamp(j.value("chaseRate", f.chaseRate), 1.0f, 200.0f);
  } else {
    ReadEaseKey(j, paced, f.ease, where, log);
  }
  f.cuts = j.value("cuts", false);
  const std::string lean = j.value("lean", std::string("follow"));
  if (lean == "hold") f.lean = FrameLean::Hold;
  else if (lean == "left") f.lean = FrameLean::Left;
  else if (lean == "right") f.lean = FrameLean::Right;
  else if (lean == "angle") {
    f.lean = FrameLean::Angle;
    f.leanAngle = WrapPi(j.value("leanAngle", 0.0f));
  }
  else {
    if (lean != "follow")
      log += where + " has unknown \"lean\": \"" + lean +
             "\" — following the travel\n";
    f.lean = FrameLean::Follow;
  }
  f.wristAlign = std::clamp(j.value("wrist", 1.0f), 0.0f, 1.0f);
  if (j.contains("bladeAngle"))
    f.bladeAngle = std::clamp(j.value("bladeAngle", 0.0f), 0.0f, 3.14159265f);
  if (j.contains("elbow")) {
    f.elbowSet = true;
    f.elbow = WrapPi(j.value("elbow", 0.0f));
  }
  f.torsoTwist = j.contains("torsoTwist")
                     ? std::clamp(j.value("torsoTwist", 0.0f), 0.0f, 1.0f)
                     : -1.0f;
  f.torsoPitch = j.contains("torsoPitch")
                     ? std::clamp(j.value("torsoPitch", 0.0f), 0.0f, 1.0f)
                     : -1.0f;
  ReadJoints(j.value("joints", json::object()), f.joints);
  f.pose = ReadPose(j.value("pose", json::object()));
  return f;
}

void ReadFrames(const json& arr, const ArmSmooth& styleJoints,
                const std::string& path, const std::string& name,
                std::vector<StrokeFrame>& out, std::string& log) {
  out.clear();
  for (const auto& fj : arr) {
    if ((int)out.size() >= kMaxFrames) {
      log += path + ": style \"" + name + "\" has more than " +
             std::to_string(kMaxFrames) + " frames — the rest are dropped\n";
      break;
    }
    out.push_back(ReadFrame(fj, styleJoints,
                            path + ": style \"" + name + "\" frame " +
                                std::to_string(out.size() + 1),
                            log));
  }
}

// THE CUT FRAMES ARE ONE RUN. The phases the rest of the engine reads are
// derived from them (strokes.h), and a gap would make "the cut" two cuts with
// a windup in the middle. A style with none never damages anything, which is
// a content error: the LAST frame becomes the cut and the loader says so.
void ValidateFrames(AttackStyle& st, const std::string& path, std::string& log) {
  if (st.frames.empty()) return;
  int fc = st.FirstCut(), lc = st.LastCut();
  if (fc < 0) {
    log += path + ": style \"" + st.name +
           "\" has no frame with \"cuts\": true — its last frame is made the "
           "cut so it can hurt anything\n";
    st.frames.back().cuts = true;
    return;
  }
  for (int k = fc; k <= lc; k++)
    if (!st.frames[k].cuts) {
      log += path + ": style \"" + st.name + "\" frame " +
             std::to_string(k + 1) +
             " sits between two cutting frames — it cuts too (the cut is one "
             "run)\n";
      st.frames[k].cuts = true;
    }
}

// ---- THE CUT PATH (strokes.h "A CUT IS A PATH") ----------------------------
//
// `cut` is EITHER one segment or a list of legs, and the two spellings are read
// by the one function so the single-segment form cannot drift into being a
// special case with its own defaults. A one-leg list and a bare object produce
// byte-identical programs; that is the whole compatibility story, and it is why
// nothing in the shipped library had to be rewritten for this.
//
// A leg of the list defaults to a SHORT, STATIONARY segment rather than to the
// single cut's `{7, -2.0, 0, 0.10}`: those defaults are a whole horizontal
// slash, which is a sensible thing for an unstated cut to be and a nonsensical
// thing for an unstated SECOND leg to be. An author who writes `{"az": -0.4}`
// as a third leg means "and then a little further", not "and then a whole
// second swing".
void ReadCutPath(const json& cutJ, const std::string& path,
                 const std::string& name, std::vector<StrokeSegment>& out,
                 int& aimLeg, std::string& log) {
  out.clear();
  aimLeg = -1;
  if (cutJ.is_array()) {
    for (const auto& leg : cutJ) {
      if (!leg.is_object()) {
        log += path + ": style \"" + name +
               "\" has a cut leg that is not an object — skipped\n";
        continue;
      }
      if ((int)out.size() >= kMaxCutLegs) {
        log += path + ": style \"" + name + "\" has more than " +
               std::to_string(kMaxCutLegs) +
               " cut legs — the rest are dropped (a path with that many "
               "corners is a body clip, not a stroke)\n";
        break;
      }
      // THE AIM IS ONE LEG'S. Two legs claiming it is an authoring slip with a
      // silent outcome — the target would sit wherever the tie-break landed —
      // so the first wins and the second is reported.
      if (leg.value("aim", false)) {
        if (aimLeg >= 0)
          log += path + ": style \"" + name + "\" marks leg " +
                 std::to_string(out.size() + 1) +
                 " with \"aim\" as well — the first marked leg keeps it\n";
        else
          aimLeg = (int)out.size();
      }
      out.push_back(ReadSegment(
          leg, StrokeSegment{4, 0.0f, 0.0f, 0.0f},
          path + ": style \"" + name + "\" cut leg " +
              std::to_string(out.size() + 1),
          log));
    }
    if (out.empty())
      log += path + ": style \"" + name +
             "\" has an empty cut list — falling back to one default cut\n";
  }
  // The single-segment spelling, and the fallback for a list with nothing
  // usable in it. `cutJ` being null (no `cut` key at all) lands here too and
  // gets the historical default, which is what `ReadSegment` has always done
  // with an absent object.
  if (out.empty())
    out.push_back(ReadSegment(cutJ.is_object() ? cutJ : json::object(),
                              StrokeSegment{7, -2.0f, 0.0f, 0.10f},
                              path + ": style \"" + name + "\" cut", log));
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
  // WHICH FRAME THE NUMBERS ARE IN (strokes.h "THE STROKE FRAME"). Absent =
  // the pre-2026-09-25 frame, converted below once the player copies exist.
  const bool legacyFrame = j.value("strokeFrame", std::string()) != "target";
  const auto stylesJson = j.value("styles", json::array());
  std::unordered_map<std::string, json> rawByName;
  for (const auto& s : stylesJson) {
    AttackStyle st;
    st.name = s.value("name", "");
    if (st.name.empty()) {
      log += path + ": a style with no \"name\" was skipped\n";
      continue;
    }
    st.label = s.value("label", st.name);
    // HOW THE CUT IS PACED (strokes.h, above AttackStyle). Absent is "linear",
    // the drive every style shipped before this had, so an older file loads
    // and swings identically.
    //
    // ParseEase is SILENT on a name it does not know (anim.cpp returns Linear,
    // which is right for a keyframe: a clip must not fail to load over a
    // typo). Here the typo is worth saying out loud, and the round-trip is how
    // to tell "the author wrote linear" from "the author wrote quadOOut" —
    // both parse to Linear, and only one of them is a mistake.
    if (s.contains("ease")) {
      const std::string en = s.value("ease", "linear");
      st.ease = ParseEase(en);
      if (st.ease == Ease::Linear && en != "linear")
        log += path + ": style \"" + st.name + "\" has unknown ease \"" + en +
               "\" — using linear (the names are anim.h's: linear, instant, "
               "quadIn/Out/InOut, cubicIn/Out/InOut)\n";
    }
    // THE NEW SPELLING (strokes.h "AN ATTACK IS A LIST OF FRAMES"). A style
    // with `frames` is read from them and nothing else describes its motion;
    // one without is the pre-frames spelling, read below and converted once
    // the player copies exist.
    ReadJoints(s.value("joints", json::object()), st.joints);
    if (s.contains("frames") && s["frames"].is_array()) {
      ReadFrames(s["frames"], st.joints, path, st.name, st.frames, log);
      st.release = std::max(1, s.value("release", st.release));
    }
    st.windup = ReadSegment(s.value("windup", json::object()),
                            StrokeSegment{12, 0.30f, 0.10f, -0.05f},
                            path + ": style \"" + st.name + "\" windup", log);
    ReadCutPath(s.contains("cut") ? s["cut"] : json(), path, st.name, st.cut,
                st.aimLeg, log);
    // ---- THE RETURN (strokes.h StrokeRecover) ------------------------------
    // Every field defaults to what a style written before this existed already
    // did, and `posed` is derived rather than authored: stating an az, an el or
    // a reach is what asks for a driven return, so there is no second switch to
    // forget. A recover of `{ "ticks": 10 }` is the old four-line hand-back,
    // byte for byte.
    if (s.contains("recover") && s["recover"].is_object()) {
      const auto& rv = s["recover"];
      st.recover.ticks = std::max(1, rv.value("ticks", 10));
      st.recover.posed =
          rv.contains("az") || rv.contains("el") || rv.contains("reach");
      st.recover.az = rv.value("az", 0.0f);
      st.recover.el = rv.value("el", 0.0f);
      st.recover.reach = rv.value("reach", 0.0f);
      st.recover.settle = std::max(0, rv.value("settle", 0));
      st.recover.fade = std::max(0, rv.value("fade", 0));
      ReadEaseKey(rv, st.recover.paced, st.recover.ease,
                  path + ": style \"" + st.name + "\" recover", log);
      // A SETTLE WITH NOTHING TO SETTLE TO holds the arm at the end of the cut
      // with the button still down and then drops it, which is the crossfade
      // this whole block exists to remove, arrived at the long way round.
      if (st.recover.settle > 0 && !st.recover.posed) {
        log += path + ": style \"" + st.name +
               "\" has `recover.settle` but no return pose (az/el/reach) — the "
               "arm has nowhere to be driven, so the settle is ignored\n";
        st.recover.settle = 0;
      }
      if (st.recover.settle > st.recover.ticks) {
        log += path + ": style \"" + st.name + "\" settles for " +
               std::to_string(st.recover.settle) + " of " +
               std::to_string(st.recover.ticks) +
               " recover ticks — clamped to the segment\n";
        st.recover.settle = st.recover.ticks;
      }
      // ...AND THE FADE HAS TO FIT IN WHAT IS LEFT. The program ENDS at
      // `ticks`, and the NPC drops its pose claim on that tick (mob.cpp pushes
      // an empty WeaponPose). A fade still running when that happens is a
      // PoseWeight stepping from whatever it had reached straight to 0 in one
      // tick — which is exactly the snap melee.cpp:2430 was just fixed to
      // remove, reintroduced from the content side. Extend the segment rather
      // than shortening the fade: the author asked for that return, and the
      // cost of honouring it is ticks, not a visible snap.
      if (st.recover.fade > 0 &&
          st.recover.settle + st.recover.fade > st.recover.ticks) {
        const int want = st.recover.settle + st.recover.fade;
        log += path + ": style \"" + st.name + "\" settles " +
               std::to_string(st.recover.settle) + " + fades " +
               std::to_string(st.recover.fade) + " = " + std::to_string(want) +
               " ticks in a " + std::to_string(st.recover.ticks) +
               "-tick recover — extended to " + std::to_string(want) +
               " so the claim is not dropped mid-fade\n";
        st.recover.ticks = want;
      }
    }
    ReadJoints(s.value("joints", json::object()), st.joints);
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
    // Both target tables read the same way, because they ARE the same table
    // asked under two postures (strokes.h AttackStyle::targetProne).
    auto readTargets = [&](const char* key,
                           std::vector<StyleTargetWeight>& out) {
      if (!s.contains(key) || !s[key].is_object()) return;
      for (auto t = s[key].begin(); t != s[key].end(); ++t) {
        if (!t.value().is_number()) continue;
        const float w = t.value().get<float>();
        if (w <= 0.0f) continue;   // a zero weight IS the absent entry
        out.push_back(StyleTargetWeight{t.key(), w});
      }
      // Deterministic order, because the draw walks this vector and JSON
      // object order is the library's business, not the author's. Two files
      // that spell the same weights in a different order must pick the same
      // limb from the same (mob, tick) — that is rule 1 applied to content.
      std::sort(out.begin(), out.end(),
                [](const StyleTargetWeight& a, const StyleTargetWeight& b) {
                  return a.tag < b.tag;
                });
    };
    readTargets("target", st.target);
    readTargets("targetProne", st.targetProne);
    // A prone table with no upright one to fall back to would make the style
    // aim at the chest while standing and at the legs while crawling, which
    // is a content bug that looks like a targeting bug.
    if (!st.targetProne.empty() && st.target.empty())
      log += path + ": style \"" + st.name +
             "\" has `targetProne` but no `target` — it will aim at the chest "
             "unless the attacker is on the ground\n";
    // A CUT THAT GOES NOWHERE IS NOT A CUT. It would pose the blade, commit
    // nothing, and hand back a stroke that could never damage anything — and
    // the only symptom would be an NPC that swings and never hits, which is
    // exactly the sort of content bug that gets blamed on the damage path.
    //
    // MEASURED OVER THE WHOLE PATH, not per leg: a stationary leg is a legal
    // hitch (strokes.h), and a path that ends where it started still passed
    // through the target on the way — a hook out and back is a real stroke.
    // What this refuses is a path with no motion anywhere in it, which is why
    // the sum is of each leg's own |travel| rather than of the net
    // displacement.
    float travel = 0;
    for (const StrokeSegment& leg : st.cut)
      travel += std::fabs(leg.az) + std::fabs(leg.el) + std::fabs(leg.reach);
    if (travel < 1e-3f) {
      log += path + ": style \"" + st.name +
             "\" has a cut that travels nowhere — skipped\n";
      continue;
    }
    if (lib.Find(st.name) >= 0)
      log += path + ": duplicate style \"" + st.name + "\" — last wins\n";
    rawByName[st.name] = s;
    lib.styles.push_back(std::move(st));
  }
  if (lib.styles.empty())
    log += path + ": no usable styles — NPCs will not swing\n";

  // ---- PLAYER OVERRIDES (attack_styles.json `player` block per style) ------
  // Each style may carry a `player` block stating only the fields that differ
  // when the player swings it (shorter windup, zero jitter, sometimes different
  // geometry). The loader creates a DERIVED copy with overrides merged and
  // appends it to `styles`; the player compass resolves to the derived copy.
  // Merge rule: per-field within windup/cut/jitter (unstated fields inherit).
  // For `recover`: if the override is present but states no az/el/reach, the
  // result has no posed return even if the base did.
  std::unordered_map<std::string, int> playerDerived;
  for (size_t base = 0, count = lib.styles.size(); base < count; base++) {
    auto it = rawByName.find(lib.styles[base].name);
    if (it == rawByName.end()) continue;
    const auto& sj = it->second;
    if (!sj.contains("player") || !sj["player"].is_object()) continue;
    const auto& pj = sj["player"];
    AttackStyle ps = lib.styles[base];
    ps.name += ":player";
    // The player's copy may pace the cut differently from the NPC's: a click
    // has to feel owned and an authored NPC telegraph does not, which is the
    // same reason the block already overrides windup ticks and zeroes jitter.
    if (pj.contains("ease")) {
      const std::string en = pj.value("ease", "linear");
      const Ease pe = ParseEase(en);
      if (pe == Ease::Linear && en != "linear")
        log += path + ": style \"" + ps.name + "\" has unknown ease \"" + en +
               "\" — inheriting the base's\n";
      else
        ps.ease = pe;
    }
    if (pj.contains("windup") && pj["windup"].is_object()) {
      const auto& w = pj["windup"];
      ps.windup.ticks = std::max(1, w.value("ticks", ps.windup.ticks));
      ps.windup.az = w.value("az", ps.windup.az);
      ps.windup.el = w.value("el", ps.windup.el);
      ps.windup.reach = w.value("reach", ps.windup.reach);
      ReadEaseKey(w, ps.windup.paced, ps.windup.ease,
                  path + ": style \"" + ps.name + "\" windup", log);
    }
    // ---- THE CUT, WHICH MAY NOW BE A PATH ON EITHER SIDE ------------------
    // A LIST REPLACES, AN OBJECT MERGES INTO THE FIRST LEG. Merging a list
    // into a list per index would silently reinterpret leg 2 of the player's
    // hook as leg 2 of a base with a different number of corners, which is a
    // rule nobody could hold in their head; and an object override was only
    // ever "the same stroke, tightened" — a shorter windup, a shorter travel —
    // so it lands on the leg that opens the cut and leaves the rest of the
    // path as the base authored it.
    if (pj.contains("cut") && pj["cut"].is_array()) {
      ReadCutPath(pj["cut"], path, ps.name, ps.cut, ps.aimLeg, log);
    } else if (pj.contains("cut") && pj["cut"].is_object()) {
      const auto& c = pj["cut"];
      StrokeSegment& leg0 = ps.cut[0];
      leg0.ticks = std::max(1, c.value("ticks", leg0.ticks));
      leg0.az = c.value("az", leg0.az);
      leg0.el = c.value("el", leg0.el);
      leg0.reach = c.value("reach", leg0.reach);
      ReadEaseKey(c, leg0.paced, leg0.ease,
                  path + ": style \"" + ps.name + "\" cut", log);
    }
    if (pj.contains("recover") && pj["recover"].is_object()) {
      const auto& rv = pj["recover"];
      ps.recover.ticks = std::max(1, rv.value("ticks", ps.recover.ticks));
      ps.recover.posed =
          rv.contains("az") || rv.contains("el") || rv.contains("reach");
      if (ps.recover.posed) {
        ps.recover.az = rv.value("az", 0.0f);
        ps.recover.el = rv.value("el", 0.0f);
        ps.recover.reach = rv.value("reach", 0.0f);
      } else {
        ps.recover.az = ps.recover.el = ps.recover.reach = 0.0f;
        ps.recover.settle = 0;
      }
      ps.recover.settle = std::max(0, rv.value("settle", ps.recover.settle));
      ps.recover.fade = std::max(0, rv.value("fade", ps.recover.fade));
      ReadEaseKey(rv, ps.recover.paced, ps.recover.ease,
                  path + ": style \"" + ps.name + "\" recover", log);
      if (ps.recover.settle > 0 && !ps.recover.posed)
        ps.recover.settle = 0;
      if (ps.recover.settle > ps.recover.ticks)
        ps.recover.settle = ps.recover.ticks;
      if (ps.recover.fade > 0 &&
          ps.recover.settle + ps.recover.fade > ps.recover.ticks)
        ps.recover.ticks = ps.recover.settle + ps.recover.fade;
    }
    ReadJoints(pj.value("joints", json::object()), ps.joints);
    // A player block in the frames spelling REPLACES the list (a partial
    // merge by index would silently re-aim frame 2 of a different path).
    if (pj.contains("frames") && pj["frames"].is_array())
      ReadFrames(pj["frames"], ps.joints, path, ps.name, ps.frames, log);
    if (pj.contains("release"))
      ps.release = std::max(1, pj.value("release", ps.release));
    if (pj.contains("jitter") && pj["jitter"].is_object()) {
      const auto& q = pj["jitter"];
      ps.jitter.az = q.value("az", ps.jitter.az);
      ps.jitter.el = q.value("el", ps.jitter.el);
      ps.jitter.tempo = std::clamp(q.value("tempo", ps.jitter.tempo), 0.0f, 0.6f);
    }
    playerDerived[lib.styles[base].name] = (int)lib.styles.size();
    lib.styles.push_back(std::move(ps));
  }
  // ---- WEAPON FORMS (strokes.h) -------------------------------------------
  // Over base AND player copies: a form replaces the frames (and release) and
  // keeps everything else, the player copy's jitter included, so `forms` and
  // `player` compose instead of one silently winning.
  {
    const size_t pre = lib.styles.size();
    std::vector<std::array<int, kWeaponForms>> rows(pre, {-1, -1, -1});
    for (size_t i = 0; i < pre; i++) {
      std::string base = lib.styles[i].name;
      const size_t colon = base.find(":player");
      const bool isPlayer = colon != std::string::npos;
      if (isPlayer) base.resize(colon);
      auto it = rawByName.find(base);
      if (it == rawByName.end()) continue;
      const auto& sj = it->second;
      if (!sj.contains("forms") || !sj["forms"].is_object()) continue;
      const auto& fo = sj["forms"];
      for (auto kv = fo.begin(); kv != fo.end(); ++kv)
        if (WeaponFormOf(kv.key()) < 0 && !isPlayer)
          log += path + ": style \"" + base + "\" has unknown form \"" +
                 kv.key() + "\" (short / long / blunt) - ignored\n";
      for (int f = 0; f < kWeaponForms; f++) {
        const char* key = kWeaponFormNames[f];
        if (!fo.contains(key) || !fo[key].is_object()) continue;
        const auto& fj = fo[key];
        AttackStyle fs = lib.styles[i];
        fs.name = base + "@" + key + (isPlayer ? ":player" : "");
        fs.form = key;
        if (fj.contains("frames") && fj["frames"].is_array())
          ReadFrames(fj["frames"], fs.joints, path, fs.name, fs.frames, log);
        if (fj.contains("release"))
          fs.release = std::max(1, fj.value("release", fs.release));
        rows[i][f] = (int)lib.styles.size();
        lib.styles.push_back(std::move(fs));
      }
    }
    rows.resize(lib.styles.size(), {-1, -1, -1});
    lib.forms = std::move(rows);
  }
  // AFTER the merge, and over base and player copies alike: a player block in
  // an old file is in the old frame too, so it has to be merged in it first.
  if (legacyFrame)
    for (AttackStyle& st : lib.styles)
      if (st.frames.empty()) st.FromLegacyFrame();
  // EVERY STYLE RUNS ON FRAMES. The pre-frames spelling is converted here
  // (exactly: the targets are the old phases' own expressions); the frames
  // spelling refills the derived windup/cut/recover views its readers use.
  for (AttackStyle& st : lib.styles) {
    if (st.frames.empty()) {
      st.BuildFramesFromLegacy();
    } else {
      ValidateFrames(st, path, log);
      st.DeriveLegacyViews();
    }
  }

  // ---- the player's flick compass (strokes.h PlayerStrikeMap) --------------
  // The compass references BASE style names; the resolver uses the derived
  // player copy if one exists, otherwise the base directly.
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
      auto it = playerDerived.find(name);
      sec.style = (it != playerDerived.end()) ? it->second : lib.Find(name);
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
      auto it = playerDerived.find(name);
      map.neutral[i] = (it != playerDerived.end()) ? it->second : lib.Find(name);
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

const PlayerStrikeMap& PlayerCompass(const StyleLibrary& lib, bool armed) {
  return armed ? lib.player : lib.playerUnarmed;
}

const char* const kWeaponFormNames[kWeaponForms] = {"short", "long", "blunt"};

int WeaponFormOf(const std::string& weaponClass) {
  for (int f = 0; f < kWeaponForms; f++)
    if (weaponClass == kWeaponFormNames[f]) return f;
  return -1;
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

// ---- the conversions (strokes.h AttackStyle) ------------------------------
void AttackStyle::BuildFramesFromLegacy() {
  frames.clear();
  StrokeFrame w;
  w.name = "windup";
  w.ticks = windup.ticks;
  w.az = windup.az;
  w.el = windup.el;
  w.reach = windup.reach;
  // An unpaced windup WAS the capped chase: kept, as a visible pacing.
  w.ease = windup.paced ? windup.ease : Ease::Linear;
  w.chase = !windup.paced;
  w.joints = joints;
  frames.push_back(w);
  for (int k = 0; k < (int)cut.size() && (int)frames.size() < kMaxFrames; k++) {
    StrokeFrame f;
    f.name = cut.size() > 1 ? "cut " + std::to_string(k + 1) : "cut";
    f.ticks = cut[k].ticks;
    const StrokeSegment through = CutThrough(k);
    f.az = windup.az + through.az;
    f.el = windup.el + through.el;
    f.reach = windup.reach + through.reach;
    f.ease = LegEase(k);
    f.cuts = true;
    f.joints = joints;
    frames.push_back(f);
  }
  const int settle = recover.posed ? std::max(0, recover.settle) : 0;
  if (settle > 0 && (int)frames.size() < kMaxFrames) {
    StrokeFrame r;
    r.name = "recover";
    r.ticks = settle;
    r.az = recover.az;
    r.el = recover.el;
    r.reach = recover.reach;
    r.fromBody = true;
    r.ease = recover.paced ? recover.ease : Ease::Linear;
    r.chase = !recover.paced;
    r.lean = FrameLean::Hold;   // a return does not re-lean (melee.h)
    r.joints = joints;
    frames.push_back(r);
  }
  release = recover.fade > 0 ? recover.fade
                             : std::max(1, recover.ticks - settle);
}

void AttackStyle::DeriveLegacyViews() {
  const int fc = FirstCut(), lc = LastCut();
  if (frames.empty() || fc < 0) return;
  // The windup view is the pose the cut starts from, reached over every
  // frame before it. With no frame before it, the first cut frame's own pose
  // and one tick (the view has no zero-length spelling).
  int pre = 0;
  for (int k = 0; k < fc; k++) pre += frames[k].ticks;
  const StrokeFrame& w = fc > 0 ? frames[fc - 1] : frames[fc];
  windup = StrokeSegment{std::max(1, pre), w.az, w.el, w.reach};
  windup.paced = !w.chase;
  windup.ease = w.ease;
  cut.clear();
  float pAz = w.az, pEl = w.el, pR = w.reach;
  for (int k = fc; k <= lc && (int)cut.size() < kMaxCutLegs; k++) {
    const StrokeFrame& f = frames[k];
    StrokeSegment leg{f.ticks, f.az - pAz, f.el - pEl, f.reach - pR};
    leg.paced = true;
    leg.ease = f.ease;
    cut.push_back(leg);
    pAz = f.az; pEl = f.el; pR = f.reach;
  }
  aimLeg = -1;
  ease = Ease::Linear;
  int post = 0;
  for (int k = lc + 1; k < (int)frames.size(); k++) post += frames[k].ticks;
  recover = StrokeRecover{};
  recover.posed = lc + 1 < (int)frames.size();
  if (recover.posed) {
    const StrokeFrame& r = frames.back();
    recover.az = r.az;
    recover.el = r.el;
    recover.reach = r.reach;
    recover.paced = true;
    recover.ease = r.ease;
  }
  recover.settle = post;
  recover.fade = release;
  recover.ticks = post + std::max(1, release);
  joints = frames[fc].joints;
}

const ArmSmooth& StrokeJointsNow(const StrokeCursor& cur, const AttackStyle& sty) {
  if (sty.frames.empty()) return sty.joints;
  const int k = std::clamp(cur.frame, 0, (int)sty.frames.size() - 1);
  return sty.frames[k].joints;
}

void StrokeKeyedPose(const StrokeCursor& cur, const AttackStyle& sty, WeaponPose& wp) {
  if (!sty.Keyed()) return;
  const int n = std::max(1, std::min((int)sty.frames.size(),
                                     cur.frames > 0 ? cur.frames : (int)sty.frames.size()));
  const StrokePose& last = sty.frames[n - 1].pose;
  wp.keyed = WeaponPose::Keyed{};
  const float bowAz = sty.jitter.az * rng::SignedUnit(rng::Hash3(cur.seed, 2, 0));
  const float bowEl = sty.jitter.el * rng::SignedUnit(rng::Hash3(cur.seed, 3, 0));
  // Each END of the blend carries its own aim turn, so a frame that goes from
  // body-relative to target-relative swings over instead of jumping.
  auto aimOf = [&](int k, float& yaw, float& pitch) {
    const StrokeFrame& f = sty.frames[k];
    yaw = pitch = 0.0f;
    if (f.fromBody) return;
    const bool preCut = cur.firstCut < 0 || k < cur.firstCut;
    yaw = (cur.aimed ? cur.aimAz : cur.liveAz) + (preCut ? bowAz : 0.0f);
    pitch = (cur.aimed ? cur.aimEl : cur.liveEl) + (preCut ? bowEl : 0.0f);
  };
  int k = cur.frame;
  float t = 1.0f;
  // THE TICK THE LAST FRAME ENDS the program is already releasing while the
  // driver has not begun its hand-back (release < 0): the arm is still the
  // stroke's, so it HOLDS the last pose -- left alone it was the driver's IK
  // arm for one tick, a 100-degree snap.
  const bool hold = cur.releasing && wp.release < 0.0f &&
                    cur.phase != StrokeCursor::Phase::Idle;
  if (!hold && (cur.releasing || cur.phase == StrokeCursor::Phase::Idle)) {
    // The torso comes home on the same curve the arm does.
    const float w = wp.release >= 0.0f ? 1.0f - wp.release : 0.0f;
    wp.torsoTwist = last.twist * w;
    wp.torsoPitch = last.pitch * w;
    return;
  }
  if (hold || k >= n) {
    k = n - 1;
  } else {
    const int len = std::max(1, cur.frameTicks[k]);
    // `chase` has no meaning between two poses: it plays linear.
    const Ease e = sty.frames[k].chase ? Ease::Linear : sty.frames[k].ease;
    t = ApplyEase(e, std::clamp((float)cur.frameTick / (float)len, 0.0f, 1.0f));
  }
  const StrokePose& to = sty.frames[k].pose;
  const StrokePose* from = k > 0 ? &sty.frames[k - 1].pose : nullptr;
  WeaponPose::Keyed& K = wp.keyed;
  K.on = true;
  K.fromLive = from == nullptr;
  K.t = t;
  for (int j = 0; j < kArmJoints; j++) {
    K.to[j] = to.joint[j];
    K.from[j] = from ? from->joint[j] : to.joint[j];
    K.hasJoint[j] = to.hasJoint[j] && (from == nullptr || from->hasJoint[j]);
  }
  if (from) aimOf(k - 1, K.aimFromYaw, K.aimFromPitch);
  aimOf(k, K.aimToYaw, K.aimToPitch);
  const float tw0 = from ? from->twist : 0.0f, pt0 = from ? from->pitch : 0.0f;
  wp.torsoTwist = tw0 + (to.twist - tw0) * t;
  wp.torsoPitch = pt0 + (to.pitch - pt0) * t;
}

WeaponPose StrokePoseNow(const StrokeCursor& cur, const AttackStyle* sty,
                         const MeleeState& m) {
  WeaponPose wp = m.Pose();
  if (sty != nullptr && cur.phase != StrokeCursor::Phase::Idle) {
    wp.smooth = StrokeJointsNow(cur, *sty);
    StrokeKeyedPose(cur, *sty, wp);
  }
  return wp;
}

namespace {
StrokeCursor::Phase PhaseOfFrame(const StrokeCursor& cur, int k) {
  if (cur.firstCut < 0 || k < cur.firstCut) return StrokeCursor::Phase::Windup;
  if (k <= cur.lastCut) return StrokeCursor::Phase::Cut;
  return StrokeCursor::Phase::Recover;
}
}  // namespace

void BeginStrokeProgram(StrokeCursor& cur, const AttackStyle& sty,
                        int styleIndex, uint32_t seed) {
  cur.style = styleIndex;
  cur.seed = seed;
  cur.fromPending = false;
  cur.fromTick = 0;
  const float tempo =
      1.0f + sty.jitter.tempo * rng::SignedUnit(rng::Hash3(seed, 1, 0));
  cur.frames = std::min((int)sty.frames.size(), kMaxFrames);
  cur.firstCut = std::min(sty.FirstCut(), cur.frames - 1);
  cur.lastCut = std::min(sty.LastCut(), cur.frames - 1);
  // THE TEMPO JITTER stretches everything up to and including the cut — the
  // telegraph and the travel an opponent reads — and never the return
  // (strokes.h StrokeJitter says why tempo exists at all).
  for (int k = 0; k < cur.frames; k++) {
    const int base = sty.frames[k].ticks;
    const bool jittered = cur.lastCut < 0 || k <= cur.lastCut;
    cur.frameTicks[k] =
        std::max(1, jittered ? (int)std::lround(base * tempo) : base);
  }
  int windup = 0, cut = 0, post = 0;
  for (int k = 0; k < cur.frames; k++) {
    const StrokeCursor::Phase ph = PhaseOfFrame(cur, k);
    if (ph == StrokeCursor::Phase::Windup) windup += cur.frameTicks[k];
    else if (ph == StrokeCursor::Phase::Cut) cut += cur.frameTicks[k];
    else post += cur.frameTicks[k];
  }
  // A telegraph and a cut each keep at least two ticks, as they always have:
  // a one-tick windup is an attack with no warning.
  if (cur.firstCut > 0 && windup < 2) {
    cur.frameTicks[cur.firstCut - 1] += 2 - windup;
    windup = 2;
  }
  if (cur.lastCut >= 0 && cut < 2) {
    cur.frameTicks[cur.lastCut] += 2 - cut;
    cut = 2;
  }
  cur.windupTicks = windup;
  cur.cutTicks = cut;
  cur.settleTicks = post;
  cur.recoverTicks = post + std::max(1, sty.release);
  cur.cutLegs = cur.firstCut >= 0
                    ? std::clamp(cur.lastCut - cur.firstCut + 1, 1, kMaxCutLegs)
                    : 1;
  for (int i = 0; i < kMaxCutLegs; i++)
    cur.legTicks[i] = (cur.firstCut >= 0 && i < cur.cutLegs)
                          ? cur.frameTicks[cur.firstCut + i]
                          : 0;
  cur.cutLeg = 0;
  cur.frame = 0;
  cur.frameTick = 0;
  cur.releasing = false;
  cur.releaseLeft = 0;
  cur.aimed = false;
  if (cur.frames <= 0) {
    cur.StartRelease(std::max(1, sty.release));
    return;
  }
  cur.phase = PhaseOfFrame(cur, 0);
  cur.phaseTick = 0;
}

StrokeStepResult StepStrokeProgram(StrokeCursor& cur, const AttackStyle* sty,
                                   MeleeState& m, float liveAz, float liveEl,
                                   float liveDist, float dt, const Vec3& right,
                                   const Vec3& up, const Vec3& fwd) {
  // THE REACH A TARGET-MEASURED FRAME ASKS FOR is capped a little past the
  // target itself, so a stroke aimed at something close does not punch
  // through it to the far side of the band.
  auto toTarget = [&](float banded) {
    return liveDist > 0.0f ? std::min(banded, liveDist * 1.2f) : banded;
  };
  cur.liveAz = liveAz;
  cur.liveEl = liveEl;
  // The start bow: a deterministic wobble on every frame BEFORE the cut, so
  // ten swings do not look stamped.
  const float bowAz =
      sty ? sty->jitter.az * rng::SignedUnit(rng::Hash3(cur.seed, 2, 0)) : 0.0f;
  const float bowEl =
      sty ? sty->jitter.el * rng::SignedUnit(rng::Hash3(cur.seed, 3, 0)) : 0.0f;

  // ---- THIS FRAME'S SHAPING, pushed to the driver every tick --------------
  // During the release the LAST frame's settings stay in force.
  const StrokeFrame* fr = nullptr;
  if (sty != nullptr && !sty->frames.empty() &&
      cur.phase != StrokeCursor::Phase::Idle &&
      cur.phase != StrokeCursor::Phase::Guard)
    fr = &sty->frames[std::clamp(cur.frame, 0, (int)sty->frames.size() - 1)];
  if (cur.phase == StrokeCursor::Phase::Guard || fr != nullptr) {
    MeleeState::ProgramDrive d;
    if (fr != nullptr) {
      d.lean = (int)fr->lean;
      d.wristAlign = fr->wristAlign;
      d.torsoTwist = fr->torsoTwist;
      d.torsoPitch = fr->torsoPitch;
      // THE ARM'S SHAPE, BLENDED over the frame from where it was when the
      // frame began, on the frame's own curve (linear for a chase), so a
      // change between frames is a motion. During the release the last
      // frame's end values simply hold.
      const bool live = !cur.releasing && cur.frame < cur.frames;
      // THE TAKE-OVER TICK HAS NO SHAPE TO READ: the driver is still Idle,
      // its "now" angles are the last swing's, and the take-over skips its
      // rebuild. It measures the seeded arm (MeasureShapeNow); the capture is
      // taken on the tick after it.
      if (live && (cur.frameTick == 0 || cur.fromPending)) {
        if (m.Phase() == SwingPhase::Idle) {
          cur.fromPending = true;
        } else {
          cur.bladeFrom = m.BladeAngleNow();
          cur.elbowFrom = m.ElbowSwivelNow();
          cur.leanFrom = m.LeanAngleNow();
          cur.fromPending = false;
          cur.fromTick = cur.frameTick;
        }
      }
      // Over the ticks LEFT once the shape was captured, so a capture
      // deferred past the take-over does not spend two ticks' worth at once.
      float p = 1.0f;
      if (live) {
        const int len = std::max(1, cur.frameTicks[cur.frame]);
        const float q = (float)(cur.frameTick + 1 - cur.fromTick) /
                        (float)std::max(1, len - cur.fromTick);
        p = fr->chase ? q : ApplyEase(fr->ease, q);
      }
      if (fr->bladeAngle >= 0.0f)
        d.bladeAngle = cur.bladeFrom + (fr->bladeAngle - cur.bladeFrom) * p;
      // The two ANGLES ABOUT AN AXIS go the SHORT way round, decided by the
      // captured start and the frame's end -- both fixed for the frame, so the
      // direction round cannot change mid-frame. The angles are measured in
      // melee.cpp AxisFrame, which has no bad point an arm can reach, so an
      // angle blend is a smooth motion (the old projected-down reference
      // spun at the vertical and made this jump).
      if (fr->elbowSet) {
        d.elbowSet = true;
        d.elbowSwivel = cur.elbowFrom + WrapPi(fr->elbow - cur.elbowFrom) * p;
      }
      if (fr->lean == FrameLean::Angle)
        d.leanAngle = cur.leanFrom + WrapPi(fr->leanAngle - cur.leanFrom) * p;
    }
    m.SetProgramDrive(d);
  }
  // THE RELEASE'S OWN CLOCK: the hand-back fade takes exactly `release`
  // ticks. No style (a dropped guard) keeps the global melee.recoverTime.
  m.SetRecoverTime(sty != nullptr ? std::max(1, sty->release) * dt : 0.0f);

  StrokeSample smp;
  smp.held = true;

  switch (cur.phase) {
    case StrokeCursor::Phase::Guard: {
      // GUARD is a pose held indefinitely (strokes.h), driven closed-loop at
      // a capped rate because it has no duration to arrive in.
      const MeleeTuning& t = m.tuning;
      float lo = 0, hi = 0;
      m.ReachBand(lo, hi);
      const float wantR = std::clamp(lo + cur.wantReach * (hi - lo), lo, hi);
      smp.dx = std::clamp((cur.wantAz - m.StrokeAz()) / t.aimGainX, -16.0f, 16.0f);
      smp.dy = std::clamp(-(cur.wantEl - m.StrokeEl()) / t.aimGainY, -16.0f, 16.0f);
      smp.dReach = std::clamp(
          (wantR - m.StrokeRadius()) / std::max(t.reachGain, 1e-4f), -18.0f, 18.0f);
      m.Step(smp, dt, true, right, up, fwd);
      break;
    }
    case StrokeCursor::Phase::Windup:
    case StrokeCursor::Phase::Cut:
    case StrokeCursor::Phase::Recover: {
      // ---- THE RELEASE: the arm is handed back ---------------------------
      if (cur.releasing) {
        smp.held = false;
        m.Step(smp, dt, true, right, up, fwd);
        cur.phaseTick++;
        if (--cur.releaseLeft <= 0) return StrokeStepResult::Finished;
        break;
      }
      if (sty == nullptr || sty->frames.empty() || cur.frame >= cur.frames) {
        cur.StartRelease(sty != nullptr ? sty->release : 1);
        break;
      }
      const int k = cur.frame;
      const StrokeFrame& f = sty->frames[k];
      // THE AIM IS FROZEN when the first cutting frame begins (normally at the
      // end of the windup, below); a style that opens with a cut commits here.
      if (!cur.aimed && cur.firstCut >= 0 && k >= cur.firstCut) {
        cur.aimAz = liveAz;
        cur.aimEl = liveEl;
        cur.aimed = true;
      }
      float toAz = f.az, toEl = f.el;
      float toR = StrokeReachIn(m, f.reach);
      if (!f.fromBody) {
        const bool preCut = cur.firstCut < 0 || k < cur.firstCut;
        toAz += (cur.aimed ? cur.aimAz : liveAz) + (preCut ? bowAz : 0.0f);
        toEl += (cur.aimed ? cur.aimEl : liveEl) + (preCut ? bowEl : 0.0f);
        toR = toTarget(toR);
      }
      cur.wantAz = toAz;
      cur.wantEl = toEl;
      cur.wantReach = toR;
      // HOW MUCH OF THE REMAINING GAP THIS TICK SPENDS. Linear keeps the
      // literal gap/ticks-left form the cut has always used; a curve spends
      // its share. Either way the frame ARRIVES on its last tick.
      const MeleeTuning& t = m.tuning;
      const float rg = std::max(t.reachGain, 1e-4f);
      const int len = std::max(1, cur.frameTicks[k]);
      const int into = cur.frameTick;
      if (f.chase) {
        // CHASE: close on the pose at a capped rate (the frame's own).
        const float cap = f.chaseRate, capR = f.chaseRate * (18.0f / 16.0f);
        smp.dx = std::clamp((toAz - m.StrokeAz()) / t.aimGainX, -cap, cap);
        smp.dy = std::clamp(-(toEl - m.StrokeEl()) / t.aimGainY, -cap, cap);
        smp.dReach = std::clamp((toR - m.StrokeRadius()) / rg, -capR, capR);
      } else if (f.ease == Ease::Linear) {
        const float left = (float)std::max(1, len - into);
        smp.dx = ((toAz - m.StrokeAz()) / left) / t.aimGainX;
        smp.dy = -((toEl - m.StrokeEl()) / left) / t.aimGainY;
        smp.dReach = ((toR - m.StrokeRadius()) / left) / rg;
      } else {
        const float share = StrokeEaseStep(f.ease, (float)into / (float)len,
                                           (float)(into + 1) / (float)len);
        smp.dx = ((toAz - m.StrokeAz()) * share) / t.aimGainX;
        smp.dy = -((toEl - m.StrokeEl()) * share) / t.aimGainY;
        smp.dReach = ((toR - m.StrokeRadius()) * share) / rg;
      }
      m.Step(smp, dt, true, right, up, fwd);
      if (cur.phase == StrokeCursor::Phase::Cut) cur.cutLeg = k - cur.firstCut;
      cur.phaseTick++;
      if (++cur.frameTick >= len) {
        // COMMIT at the end of the last windup frame, on that tick's aim.
        if (k + 1 == cur.firstCut) {
          cur.aimAz = liveAz;
          cur.aimEl = liveEl;
          cur.aimed = true;
        }
        cur.frame++;
        cur.frameTick = 0;
        StrokeCursor::Phase next;
        if (cur.frame >= cur.frames) {
          cur.releasing = true;
          cur.releaseLeft = std::max(1, sty->release);
          next = StrokeCursor::Phase::Recover;
        } else {
          next = PhaseOfFrame(cur, cur.frame);
        }
        if (next != cur.phase) {
          cur.phase = next;
          cur.phaseTick = 0;
        }
      }
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

