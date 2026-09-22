#include "game/strokes.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>

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
      out.push_back(ReadSegment(leg, StrokeSegment{4, 0.0f, 0.0f, 0.0f}));
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
                              StrokeSegment{7, -2.0f, 0.0f, 0.10f}));
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
    st.windup = ReadSegment(s.value("windup", json::object()),
                            StrokeSegment{12, 0.30f, 0.10f, -0.05f});
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
    if (pj.contains("windup") && pj["windup"].is_object()) {
      const auto& w = pj["windup"];
      ps.windup.ticks = std::max(1, w.value("ticks", ps.windup.ticks));
      ps.windup.az = w.value("az", ps.windup.az);
      ps.windup.el = w.value("el", ps.windup.el);
      ps.windup.reach = w.value("reach", ps.windup.reach);
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
      if (ps.recover.settle > 0 && !ps.recover.posed)
        ps.recover.settle = 0;
      if (ps.recover.settle > ps.recover.ticks)
        ps.recover.settle = ps.recover.ticks;
      if (ps.recover.fade > 0 &&
          ps.recover.settle + ps.recover.fade > ps.recover.ticks)
        ps.recover.ticks = ps.recover.settle + ps.recover.fade;
    }
    if (pj.contains("jitter") && pj["jitter"].is_object()) {
      const auto& q = pj["jitter"];
      ps.jitter.az = q.value("az", ps.jitter.az);
      ps.jitter.el = q.value("el", ps.jitter.el);
      ps.jitter.tempo = std::clamp(q.value("tempo", ps.jitter.tempo), 0.0f, 0.6f);
    }
    playerDerived[lib.styles[base].name] = (int)lib.styles.size();
    lib.styles.push_back(std::move(ps));
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
  // ---- THE PATH'S TICK TABLE (strokes.h StrokeCursor::legTicks) ------------
  // PER LEG, ROUNDED ONCE. Scaling the total and splitting it would make the
  // corner drift with the jitter; scaling each leg is what keeps "leg 1 is
  // twice leg 2" true at every tempo, and rounding here rather than per tick
  // is what keeps a leg boundary from moving under the drive.
  //
  // A leg floors at ONE tick — a leg the jitter rounded away would delete a
  // corner from the path, and a stroke that loses its dogleg at tempo 0.9 and
  // has it at 1.1 is the "different animations randomly" report all over
  // again. The WHOLE cut still floors at two, exactly as it did when there was
  // only ever one leg, so a one-leg path's tick count is unchanged.
  cur.cutLegs = std::clamp((int)sty.cut.size(), 1, kMaxCutLegs);
  int total = 0;
  for (int k = 0; k < cur.cutLegs; k++) {
    cur.legTicks[k] = std::max(1, (int)std::lround(sty.cut[k].ticks * tempo));
    total += cur.legTicks[k];
  }
  if (total < 2) {
    cur.legTicks[cur.cutLegs - 1] += 2 - total;
    total = 2;
  }
  cur.cutTicks = total;
  cur.cutLeg = 0;
  // THE RECOVER IS NOT TEMPO-JITTERED, and that is deliberate rather than an
  // omission. `tempo` exists so two duelists do not beat time together, and
  // what carries that is the TELEGRAPH and the travel — the two segments an
  // opponent reads. Scaling the return as well would make the settle and the
  // fade drift out of the relationship the author set them in (settle + fade
  // <= ticks, enforced at load), and buy nothing an onlooker can see.
  cur.recoverTicks = std::max(1, sty.recover.ticks);
  cur.settleTicks = std::clamp(sty.recover.posed ? sty.recover.settle : 0, 0,
                               cur.recoverTicks);
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
  // ---- THIS STYLE'S OWN HAND-BACK CLOCK (StrokeRecover::fade) --------------
  //
  // Pushed every tick rather than once at the release, and from HERE rather
  // than from BeginStrokeProgram, because this is the only function that holds
  // both the style and the MeleeState — the program fields live on a cursor
  // that has never seen a driver. It is idempotent (a float assignment), it
  // costs nothing, and pushing it unconditionally is what guarantees a style
  // that authors no fade RESTORES the global one: a MeleeState is reused
  // across strokes (the player's is reused for the whole session), so a
  // previous style's override would otherwise outlive it.
  //
  // Zero means "the global melee.recoverTime", which is what every style
  // authored before `fade` existed asks for.
  m.SetRecoverTime(sty != nullptr && sty->recover.fade > 0
                       ? sty->recover.fade * dt
                       : 0.0f);
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
      // THE CUT IS CENTRED ON THE AIM, so the windup lands as far SHORT of it
      // as the target sits ALONG THE PATH: `aim - CutAimOffset + windup`. For
      // a one-leg cut that offset is `cut/2` and this is the line it always
      // was; for a path it is the middle of the leg that carries the aim
      // (strokes.h). A stroke that started at the aim would cut the air behind
      // the target every time — the blade only ever travels away from where it
      // began.
      float aimOffAz = 0, aimOffEl = 0;
      sty->CutAimOffset(aimOffAz, aimOffEl);
      cur.wantAz = liveAz - aimOffAz + sty->windup.az + bowAz;
      cur.wantEl = liveEl - aimOffEl + sty->windup.el + bowEl;
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
      // WHICH LEG OF THE PATH (strokes.h "A CUT IS A PATH"). `phaseTick`
      // counts the WHOLE cut — every reader outside this file measures it
      // against `cutTicks` — so the leg is found by walking the resolved tick
      // table, six additions at worst. Deriving it instead of latching it is
      // what makes the phase re-entrant: a caller that steps a cursor twice in
      // one tick (a zero-tempo lunge does) lands on the same leg both times.
      int leg = 0, legEnd = cur.legTicks[0];
      while (leg + 1 < cur.cutLegs && cur.phaseTick >= legEnd) {
        leg++;
        legEnd += cur.legTicks[leg];
      }
      cur.cutLeg = leg;
      // FROM WHERE THE BLADE ACTUALLY IS, THROUGH THE AIM, TO THE END OF THIS
      // LEG. Derived per tick from the live stroke state rather than baked at
      // commit, so a windup that could not quite reach its pose (a clamped
      // shoulder, a short arm) still produces a cut through the target instead
      // of one displaced by however much the arm fell short — and so a leg
      // that ran out of ticks short of its corner does not displace the legs
      // after it either, because each one's target is an ABSOLUTE point on the
      // path (`aim - offset + travel through this leg`) and not a delta from
      // wherever the last one actually got to.
      float aimOffAz = 0, aimOffEl = 0;
      sty->CutAimOffset(aimOffAz, aimOffEl);
      const StrokeSegment through = sty->CutThrough(leg);
      const float toAz = cur.aimAz - aimOffAz + through.az;
      const float toEl = cur.aimEl - aimOffEl + through.el;
      const float toR =
          toTarget(StrokeReachIn(m, sty->windup.reach + through.reach));
      // THE TICKS LEFT IN THIS LEG, not in the cut: the deltas are divided by
      // them and delivered per tick, so this is what sets the leg's SPEED, and
      // a short leg is a fast one. That is the whole of how a path varies its
      // tempo.
      const int left = std::max(1, legEnd - cur.phaseTick);
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
      // ---- THE RETURN, IN TWO PARTS (strokes.h StrokeRecover) -------------
      //
      // SETTLE: the button is still DOWN and the arm is steered — closed-loop,
      // under commitSpeed, the windup's own drive — from wherever the cut left
      // it to the style's ABSOLUTE return stance. PoseWeight stays at 1 for
      // every one of these ticks, so what the rig sees is an arm travelling a
      // path an arm can travel, not a weight fading on a frozen pose.
      //
      // The pose is absolute for the reason the header states: a recover is a
      // return to stance and not a second aim. `cur.wantAz`/`wantEl`/
      // `wantReach` are written the same way the other two phases write them,
      // so the tuner's readout and the gates report the return target in the
      // same three fields they already read.
      //
      // Then RELEASE: exactly the old four lines. The driver clears
      // `recoverHold_` and restarts its own clock at the release
      // (melee.cpp:2430), so the fade is a full one measured from HERE rather
      // than whatever was left of one that began back in the cut.
      if (sty != nullptr && cur.phaseTick < cur.settleTicks) {
        cur.wantAz = sty->recover.az;
        cur.wantEl = sty->recover.el;
        cur.wantReach = StrokeReachIn(m, sty->recover.reach);
        steerTo(cur.wantAz, cur.wantEl, cur.wantReach);
      } else {
        smp.held = false;
        m.Step(smp, dt, true, right, up, fwd);
      }
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

