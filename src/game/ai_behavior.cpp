#include "game/ai_behavior.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "sim/rng.h"

using nlohmann::json;

namespace ai {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kTau = 6.2831853f;

// RNG salts. Distinct per decision so two draws taken on the same tick for the
// same mob cannot correlate — the same discipline the shader side uses when it
// keys hash3 on a stream id (sim/wind.h's salt note).
constexpr uint32_t kSaltAttack = 0xA77Au;
constexpr uint32_t kSaltCircle = 0xC141u;
constexpr uint32_t kSaltReact = 0x8EAC7u;
constexpr uint32_t kSaltFeint = 0xFE17u;
constexpr uint32_t kSaltDodge = 0xD0D6u;

float WrapPi(float a) { return std::remainder(a, kTau); }

// heading 0 = +Z (CLAUDE.md conventions), so a bearing is atan2(dx, dz).
float BearingTo(const Vec3& from, const Vec3& to) {
  return std::atan2(to.x - from.x, to.z - from.z);
}

float PlanarDist(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

// The faction intern table. Names are content and ids are runtime, exactly as
// materials work: authoring "bandit" must not require a C++ enum entry.
std::vector<std::string>& FactionTable() {
  static std::vector<std::string> t = {"player"};
  return t;
}

// The 8-probe fan index closest to a WORLD heading, in the mob's own frame.
int ProbeFor(float worldHeading, float selfHeading) {
  float off = WrapPi(worldHeading - selfHeading);
  if (off < 0) off += kTau;
  int i = (int)std::lround(off / (kTau / (float)GroundView::kProbes));
  return i % GroundView::kProbes;
}

// LOCAL AVOIDANCE, applied to whatever heading an intent asked for.
//
// The navigator handles the geometry it can see; this handles the geometry it
// could not — a body the path did not know about, a crater opened this tick, a
// mob that drifted into a step. It is deliberately the SAME rule the legacy
// wander uses (nearest clear probe by angular distance, aimed only 0.6 of the
// way toward it), because that rule is what produces free angles instead of
// 45-degree ricochets, and having two different avoidance behaviours in one
// creature would read as indecision.
float Deflect(float want, float selfHeading, const GroundView& g) {
  if (!g.haveGround) return want;
  const int p = ProbeFor(want, selfHeading);
  if (g.clear[p]) return want;
  int best = -1;
  float bestOff = 0;
  for (int i = 0; i < GroundView::kProbes; i++) {
    if (!g.clear[i]) continue;
    float off = (kTau * (float)i) / (float)GroundView::kProbes;
    if (off > kPi) off -= kTau;
    // Angular distance from what we WANTED, not from the nose: the mob is
    // trying to get somewhere, and deflecting toward its own current facing
    // would make it drift off the goal every time it grazed a rock.
    const float delta = std::abs(WrapPi(selfHeading + off - want));
    const float cost = delta + 0.12f * (float)std::abs(g.stepUp[i]);
    if (best < 0 || cost < bestOff) {
      best = i;
      bestOff = cost;
    }
  }
  // Boxed in on every probe. Reversing is the legacy wander's answer and it is
  // the right one here too: returning `want` leaves the mob asking for a
  // heading the drive will veto, which is a creature standing still against a
  // rock forever rather than a creature that backed out of a dead end. Still a
  // desired heading, so the turn rate applies and it pivots.
  if (best < 0) return selfHeading + kPi;
  float off = (kTau * (float)best) / (float)GroundView::kProbes;
  if (off > kPi) off -= kTau;
  return selfHeading + off * 0.6f;
}

}  // namespace

uint32_t FactionId(const std::string& name) {
  auto& t = FactionTable();
  for (size_t i = 0; i < t.size(); i++)
    if (t[i] == name) return (uint32_t)i;
  t.push_back(name);
  return (uint32_t)(t.size() - 1);
}

const char* FactionName(uint32_t id) {
  auto& t = FactionTable();
  return id < t.size() ? t[id].c_str() : "?";
}

const char* IntentName(Intent i) {
  switch (i) {
    case Intent::Idle: return "idle";
    case Intent::FaceTarget: return "face";
    case Intent::Approach: return "approach";
    case Intent::HoldRange: return "holdRange";
    case Intent::CircleStrafe: return "circle";
    case Intent::RequestAttack: return "attack";
    case Intent::Flee: return "flee";
    case Intent::Guard: return "guard";
    case Intent::Dodge: return "dodge";
    case Intent::Sleep: return "sleep";
    case Intent::Work: return "work";
    case Intent::Wander: return "wander";
    case Intent::Socialize: return "socialize";
    case Intent::Eat: return "eat";
    case Intent::Goto: return "goto";
    default: return "?";
  }
}

// The schedule's `do` words ARE the verb names, on purpose: one vocabulary,
// so a row, a profile's intent map and the dev panel all say "sleep".
Intent IntentForActivity(const std::string& act) {
  for (int i = (int)Intent::Sleep; i <= (int)Intent::Goto; i++)
    if (act == IntentName((Intent)i)) return (Intent)i;
  return Intent::Count;
}

Intent IntentFromName(const std::string& s) {
  for (int i = 0; i < (int)Intent::Count; i++)
    if (s == IntentName((Intent)i)) return (Intent)i;
  return Intent::Count;
}

const char* FactName(Fact f) {
  switch (f) {
    case Fact::Hp: return "hp";
    case Fact::Burning: return "burning";
    case Fact::LimbsLost: return "limbsLost";
    case Fact::SinceHurt: return "sinceHurt";
    case Fact::HasTarget: return "hasTarget";
    case Fact::Visible: return "visible";
    case Fact::TargetDist: return "targetDist";
    case Fact::Allies: return "allies";
    case Fact::Enemies: return "enemies";
    case Fact::Armed: return "armed";
    case Fact::MyReach: return "myReach";
    case Fact::TargetReach: return "targetReach";
    case Fact::ReachAdv: return "reachAdv";
    case Fact::TargetArmed: return "targetArmed";
    case Fact::TargetHp: return "targetHp";
    case Fact::HpAdv: return "hpAdv";
    case Fact::TargetAttacking: return "targetAttacking";
    case Fact::TargetRecovering: return "targetRecovering";
    case Fact::TargetGuarding: return "targetGuarding";
    case Fact::InTheirReach: return "inTheirReach";
    case Fact::TargetFacingMe: return "targetFacingMe";
    case Fact::EngagedAllies: return "engagedAllies";
    case Fact::PressRank: return "pressRank";
    case Fact::Hostiles: return "hostiles";
    case Fact::Shocked: return "shocked";
    default: return "?";
  }
}

Fact FactFromName(const std::string& s) {
  for (int i = 0; i < (int)Fact::Count; i++)
    if (s == FactName((Fact)i)) return (Fact)i;
  return Fact::Count;
}

const char* TuneName(Tune t) {
  switch (t) {
    case Tune::Cadence: return "cadence";
    case Tune::Speed: return "speed";
    case Tune::Band: return "band";
    case Tune::KeepOut: return "keepOut";
    case Tune::Disengage: return "disengage";
    case Tune::Circle: return "circle";
    case Tune::Skill: return "skill";
    case Tune::React: return "react";
    case Tune::Feint: return "feint";
    case Tune::Aim: return "aim";
    default: return "?";
  }
}

Tune TuneFromName(const std::string& s) {
  for (int i = 0; i < (int)Tune::Count; i++)
    if (s == TuneName((Tune)i)) return (Tune)i;
  return Tune::Count;
}

TuneMode TuneModeOf(Tune t) {
  if (t == Tune::Band) return TuneMode::Add;
  if (t == Tune::KeepOut) return TuneMode::Set;
  return TuneMode::Scale;
}

// ---------------------------------------------------------------------------
// behaviors.json
//
// SCHEMA (one object per named profile; every key optional, every default
// harmless — an empty profile is a blind, immobile statue):
//
// {
//   "profiles": [{
//     "name": "duelist",              // the id sidecars and the panel use
//     "label": "Duelist",             // debug text
//     "color": "0xC04060FFu"-ish int, // 0xAABBGGRR, matches DebugBox
//     "faction": "monster",           // targets are actors of ANOTHER faction
//     "hysteresis": 0.22,             // score bonus the incumbent intent keeps
//     "perception": {
//       "sightRange": 44, "fovDegrees": 260, "requireLos": true,
//       "aggro": "hostile",           // passive | neutral | hostile
//       "alertDecayTicks": 120, "keepRangeScale": 1.4
//     },
//     "movement": {
//       "mobile": true,
//       "rangeMin": 7, "rangeMax": 11, "bandSlack": 1.4,
//       "approachSpeed": 1, "strafeSpeed": 0.55, "retreatSpeed": 0.8,
//       "circleTendency": 0.7, "circleHoldTicks": 30,
//       "repathTicks": 12, "navRadius": 22
//       // maxStepUp / maxStepDown / headroom are OPTIONAL OVERRIDES. Omitted
//       // (or 0) the planner uses the creature's own rig budgets, which are
//       // what its legs actually do — see Movement in ai_behavior.h.
//     },
//     "attack": {
//       "style": "slash", "reach": 9.5, "aimTolerance": 0.45,
//       "cadenceTicks": 38, "jitterTicks": 20,
//       "commitTicks": 10, "disengageTicks": 24,
//       "pursueSpeed": 0.6,     // walk-speed fraction usable WHILE swinging
//       "holdGroundFrac": 0.08, // a target leaving this fast cancels the step-off
//       "leadTicks": 15         // aim ahead by this; -1 = commitTicks, 0 = none
//     },
//     "intents": {                    // ABSENT = weight 0 = disabled
//       "idle":      { "weight": 0.05 },
//       "face":      { "weight": 0.5 },
//       "approach":  { "weight": 1.0, "minDwellTicks": 8 },
//       "holdRange": { "weight": 1.1, "minDwellTicks": 6 },
//       "circle":    { "weight": 0.9, "minDwellTicks": 20, "cooldownTicks": 14 },
//       "attack":    { "weight": 2.0 },
//       "guard":     { "weight": 2.5, "minDwellTicks": 6 },   // Defense below
//       "dodge":     { "weight": 1.5, "minDwellTicks": 8 }
//     },
//
//     // ---- the fighting layer (2026-09-27); every key optional -------------
//     // perception: "stickiness", "preferWeak"          (target choice)
//     // movement:   "dodgeSpeed", "flank", "keepOut"    (see Movement)
//     // attack:     "feintChance", "feintTicks", "feintFollowTicks",
//     //             "riposteTicks"                       (see AttackTuning)
//     "defense": { "skill": 0.5, "reactTicks": 8, "reactJitter": 6,
//                  "margin": 2.5, "guardStance": 0.3, "guardReach": 0.85,
//                  "holdTicks": 4 },
//     // rules may also carry "tune": { "cadence": 0.7, "band": 3, ... } --
//     // HOW it fights while the rule holds. Names in TuneName.
//     "rules": [ { "when": { "hp": "<0.35" }, "weight": { "guard": 3 },
//                  "tune": { "band": 3, "cadence": 1.4 } } ]
//   }]
// }
//
// Unknown keys are ignored on purpose, so a file authored against a newer
// binary still loads; unknown INTENT names are reported, because a typo there
// silently disables a behaviour and that is worth a line in the log.
// ---------------------------------------------------------------------------

namespace {

Aggro AggroFromName(const std::string& s) {
  if (s == "hostile") return Aggro::Hostile;
  if (s == "neutral") return Aggro::Neutral;
  return Aggro::Passive;
}
const char* AggroName(Aggro a) {
  return a == Aggro::Hostile ? "hostile"
         : a == Aggro::Neutral ? "neutral"
                               : "passive";
}

const char* OpText(CmpOp op) {
  switch (op) {
    case CmpOp::Lt: return "<";
    case CmpOp::Le: return "<=";
    case CmpOp::Gt: return ">";
    case CmpOp::Ge: return ">=";
    case CmpOp::Eq: return "==";
    case CmpOp::Ne: return "!=";
  }
  return "?";
}

// One comparison as authored: "<0.3", ">= 2", "!=0", or a bare number (==).
// A JSON bool is 1/0 with ==, so `"visible": true` reads as it looks.
bool ParseCondition(const json& v, Condition& out) {
  if (v.is_boolean()) {
    out.op = CmpOp::Eq;
    out.value = v.get<bool>() ? 1.0f : 0.0f;
    return true;
  }
  if (v.is_number()) {
    out.op = CmpOp::Eq;
    out.value = v.get<float>();
    return true;
  }
  if (!v.is_string()) return false;
  std::string t = v.get<std::string>();
  t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
  // Two-character operators first, so "<=" is not read as "<" then "=0.3".
  static const std::pair<const char*, CmpOp> kOps[] = {
      {"<=", CmpOp::Le}, {">=", CmpOp::Ge}, {"==", CmpOp::Eq},
      {"!=", CmpOp::Ne}, {"<", CmpOp::Lt},  {">", CmpOp::Gt},
      {"=", CmpOp::Eq}};
  size_t skip = 0;
  out.op = CmpOp::Eq;
  for (const auto& [txt, op] : kOps) {
    const size_t n = std::char_traits<char>::length(txt);
    if (t.compare(0, n, txt) == 0) {
      out.op = op;
      skip = n;
      break;
    }
  }
  if (skip >= t.size()) return false;
  try {
    size_t used = 0;
    out.value = std::stof(t.substr(skip), &used);
    return used == t.size() - skip;
  } catch (...) {
    return false;
  }
}

bool Holds(const Condition& c, float x) {
  switch (c.op) {
    case CmpOp::Lt: return x < c.value;
    case CmpOp::Le: return x <= c.value;
    case CmpOp::Gt: return x > c.value;
    case CmpOp::Ge: return x >= c.value;
    case CmpOp::Eq: return x == c.value;
    case CmpOp::Ne: return x != c.value;
  }
  return false;
}

// Minimal string escape for the machine-written file: a rule's note is the
// only free text it carries, and a quote in one must not end the file.
std::string Escaped(const std::string& s) {
  std::string o;
  for (char ch : s) {
    if (ch == '\n') { o += "\\n"; continue; }
    if (ch == '"' || ch == '\\') o += '\\';
    o += ch;
  }
  return o;
}

// A rule's intent maps ("weight" / "scale"). An unknown intent name drops the
// WHOLE rule, reported: a rule that half-applies is worse than one that is
// visibly missing.
bool ParseIntentMap(const json& m, float (&dst)[(int)Intent::Count],
                    const std::string& where, std::string& log) {
  if (!m.is_object()) return true;
  for (auto& [k, v] : m.items()) {
    const Intent in = IntentFromName(k);
    if (in == Intent::Count || !v.is_number()) {
      log += where + ": unknown intent or non-number \"" + k +
             "\" -- rule dropped\n";
      return false;
    }
    dst[(int)in] = std::max(0.0f, v.get<float>());
  }
  return true;
}

}  // namespace

bool LoadBehaviors(const std::string& path, Library& out, std::string& log) {
  std::ifstream f(path);
  if (!f) {
    log += path + ": missing — mobs fall back to wander-and-avoid\n";
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    log += path + ": JSON parse error: " + e.what() + "\n";
    return false;
  }

  Library lib;
  for (auto& p : j.value("profiles", json::array())) {
    Profile pr;
    pr.name = p.value("name", "");
    if (pr.name.empty()) {
      log += path + ": a profile with no \"name\" was skipped\n";
      continue;
    }
    pr.label = p.value("label", pr.name);
    pr.color = p.value("color", 0xC0FFFFFFu);
    pr.faction = p.value("faction", "monster");
    pr.hysteresis = p.value("hysteresis", 0.22f);

    if (p.contains("perception")) {
      const auto& q = p["perception"];
      pr.perception.sightRange = q.value("sightRange", 0.0f);
      pr.perception.fovDegrees = q.value("fovDegrees", 360.0f);
      pr.perception.requireLos = q.value("requireLos", true);
      pr.perception.aggro = AggroFromName(q.value("aggro", "passive"));
      pr.perception.alertDecayTicks = q.value("alertDecayTicks", 90u);
      pr.perception.keepRangeScale = q.value("keepRangeScale", 1.4f);
      pr.perception.stickiness = q.value("stickiness", 0.0f);
      pr.perception.preferWeak = q.value("preferWeak", 0.0f);
      pr.perception.provokeTicks = q.value("provokeTicks", 240u);
    }
    if (p.contains("movement")) {
      const auto& q = p["movement"];
      pr.movement.mobile = q.value("mobile", false);
      pr.movement.rangeMin = q.value("rangeMin", 0.0f);
      pr.movement.rangeMax = q.value("rangeMax", 0.0f);
      pr.movement.bandSlack = q.value("bandSlack", 1.5f);
      pr.movement.approachSpeed = q.value("approachSpeed", 1.0f);
      pr.movement.strafeSpeed = q.value("strafeSpeed", 0.55f);
      pr.movement.retreatSpeed = q.value("retreatSpeed", 0.8f);
      pr.movement.fleeSpeed = q.value("fleeSpeed", 1.0f);
      pr.movement.circleTendency = q.value("circleTendency", 0.0f);
      pr.movement.circleHoldTicks = q.value("circleHoldTicks", 24u);
      pr.movement.repathTicks = q.value("repathTicks", 12u);
      pr.movement.navRadius = q.value("navRadius", 22.0f);
      // Absent, or 0, means "ask the body" -- see Movement in ai_behavior.h.
      pr.movement.maxStepUp = q.value("maxStepUp", 0);
      pr.movement.maxStepDown = q.value("maxStepDown", 0);
      pr.movement.headroom = q.value("headroom", 0);
      pr.movement.dodgeSpeed = q.value("dodgeSpeed", 1.3f);
      pr.movement.flank = std::clamp(q.value("flank", 0.0f), 0.0f, 1.0f);
      pr.movement.keepOut = q.value("keepOut", -1.0f);
      if (q.contains("wander") && q["wander"].is_object()) {
        const auto& w = q["wander"];
        pr.movement.wanderRadius = std::max(0.0f, w.value("radius", 0.0f));
        pr.movement.wanderSpeed = std::max(0.0f, w.value("speed", 0.5f));
        pr.movement.wanderPauseMin = w.value("pauseMin", 60u);
        pr.movement.wanderPauseMax =
            std::max(pr.movement.wanderPauseMin, w.value("pauseMax", 180u));
        pr.movement.wanderArrive = std::max(0.5f, w.value("arrive", 2.0f));
      }
    }
    if (p.contains("attack")) {
      const auto& q = p["attack"];
      // BOTH SPELLINGS, one meaning. `styles` wins when present; a lone
      // `style` is read as a one-entry list so older authored files load
      // unchanged (the same "a newer file still loads on an older binary"
      // contract this loader keeps in the other direction).
      if (q.contains("styles") && q["styles"].is_array()) {
        for (const auto& v : q["styles"])
          if (v.is_string()) pr.attack.styles.push_back(v.get<std::string>());
      }
      if (pr.attack.styles.empty())
        pr.attack.styles.push_back(q.value("style", std::string("slash")));
      pr.attack.reach = q.value("reach", 8.0f);
      pr.attack.aimTolerance = q.value("aimTolerance", 0.45f);
      pr.attack.cadenceTicks = q.value("cadenceTicks", 40u);
      pr.attack.jitterTicks = q.value("jitterTicks", 18u);
      pr.attack.commitTicks = q.value("commitTicks", 10u);
      pr.attack.disengageTicks = q.value("disengageTicks", 22u);
      // Absent means "a swing is a step forward, at a bit over half speed" and
      // "lead the blow by the commit window" -- both live defaults rather than
      // zeros, because the behaviour they replace (plant the feet, aim at the
      // present) is not one any profile would author on purpose. A profile that
      // genuinely wants a stock-still swing says `"pursueSpeed": 0`.
      pr.attack.pursueSpeed = q.value("pursueSpeed", 0.6f);
      pr.attack.holdGroundFrac = q.value("holdGroundFrac", 0.08f);
      // -1, NOT 0: "absent" and "no lead at all" are different answers, and the
      // second one has to remain sayable because it is the behaviour this
      // engine had until 2026-09-16 and the `ai-pursue` gate keeps an arm on it.
      pr.attack.leadTicks = q.value("leadTicks", -1);
      pr.attack.feintChance =
          std::clamp(q.value("feintChance", 0.0f), 0.0f, 1.0f);
      pr.attack.feintTicks = q.value("feintTicks", 6u);
      pr.attack.feintFollowTicks = q.value("feintFollowTicks", 10u);
      pr.attack.riposteTicks = q.value("riposteTicks", -1);
    }
    if (p.contains("defense")) {
      const auto& q = p["defense"];
      pr.defense.skill = std::clamp(q.value("skill", 0.0f), 0.0f, 1.0f);
      pr.defense.reactTicks = q.value("reactTicks", 8u);
      pr.defense.reactJitter = q.value("reactJitter", 6u);
      pr.defense.margin = q.value("margin", 2.5f);
      pr.defense.guardStance = q.value("guardStance", 0.0f);
      pr.defense.guardReach =
          std::clamp(q.value("guardReach", 0.85f), 0.2f, 1.0f);
      pr.defense.holdTicks = q.value("holdTicks", 4u);
    }
    if (p.contains("intents") && p["intents"].is_object()) {
      for (auto& [k, v] : p["intents"].items()) {
        const Intent in = IntentFromName(k);
        if (in == Intent::Count) {
          log += path + ": profile \"" + pr.name + "\" names unknown intent \"" +
                 k + "\" — ignored\n";
          continue;
        }
        IntentTuning& t = pr.intents[(int)in];
        t.weight = v.value("weight", 0.0f);
        t.cooldownTicks = v.value("cooldownTicks", 0u);
        t.minDwellTicks = v.value("minDwellTicks", 0u);
      }
    }
    if (p.contains("rules") && p["rules"].is_array()) {
      int ri = 0;
      for (const auto& r : p["rules"]) {
        const std::string where =
            path + ": profile \"" + pr.name + "\" rule " + std::to_string(ri++);
        if (!r.is_object()) {
          log += where + ": not an object -- dropped\n";
          continue;
        }
        Rule rule;
        rule.note = r.value("note", std::string());
        bool ok = true;
        if (r.contains("when") && r["when"].is_object()) {
          for (auto& [k, v] : r["when"].items()) {
            Condition cnd;
            cnd.fact = FactFromName(k);
            if (cnd.fact == Fact::Count) {
              log += where + ": unknown fact \"" + k + "\" -- rule dropped\n";
              ok = false;
              break;
            }
            if (!ParseCondition(v, cnd)) {
              log += where + ": cannot read the test on \"" + k +
                     "\" (want e.g. \"<0.3\") -- rule dropped\n";
              ok = false;
              break;
            }
            rule.when.push_back(cnd);
          }
        }
        if (ok && r.contains("weight"))
          ok = ParseIntentMap(r["weight"], rule.setWeight, where, log);
        if (ok && r.contains("scale"))
          ok = ParseIntentMap(r["scale"], rule.scale, where, log);
        // `tune`: HOW the creature fights while the rule holds (ai_behavior.h
        // Tune). Same drop-whole-and-say policy as an unknown intent.
        if (ok && r.contains("tune") && r["tune"].is_object()) {
          for (auto& [k, v] : r["tune"].items()) {
            const Tune t = TuneFromName(k);
            if (t == Tune::Count || !v.is_number()) {
              log += where + ": unknown tune or non-number \"" + k +
                     "\" -- rule dropped\n";
              ok = false;
              break;
            }
            rule.tune[(int)t] = v.get<float>();
            rule.tuneSet[(int)t] = true;
          }
        }
        if (ok) pr.rules.push_back(std::move(rule));
      }
    }
    if (lib.Find(pr.name) >= 0)
      log += path + ": duplicate profile \"" + pr.name + "\" — last wins\n";
    lib.profiles.push_back(std::move(pr));
  }
  out = std::move(lib);
  return true;
}

bool SaveBehaviors(const std::string& path, const Library& lib,
                   std::string& err) {
  // A hand-built emitter rather than nlohmann's dump(): the rest of this engine
  // writes JSON this way (sim/tuning.cpp WorldgenDefaultsJson), and it keeps
  // the file in the shape a human authored it — two-space indent, one profile
  // per block, no reordered keys.
  std::ostringstream o;
  auto num = [](float v) {
    std::ostringstream s;
    s.precision(4);
    s << std::defaultfloat << v;
    return s.str();
  };
  o << "{\n";
  o << "  \"comment\": \"NPC behaviour profiles. Vocabulary (which intents "
       "exist) is code; character (which a creature is allowed, how much it "
       "wants each, how far it sees, what range it likes) is this file. A mob "
       "sidecar opts in with \\\"behavior\\\": \\\"<name>\\\". Hot-reloads on R "
       "with materials, glyphs and items. Schema is documented at the top of "
       "src/game/ai_behavior.cpp.\",\n";
  o << "  \"profiles\": [\n";
  for (size_t i = 0; i < lib.profiles.size(); i++) {
    const Profile& p = lib.profiles[i];
    o << "    {\n";
    o << "      \"name\": \"" << p.name << "\",\n";
    o << "      \"label\": \"" << p.label << "\",\n";
    o << "      \"color\": " << p.color << ",\n";
    o << "      \"faction\": \"" << p.faction << "\",\n";
    o << "      \"hysteresis\": " << num(p.hysteresis) << ",\n";
    o << "      \"perception\": { \"sightRange\": " << num(p.perception.sightRange)
      << ", \"fovDegrees\": " << num(p.perception.fovDegrees)
      << ", \"requireLos\": " << (p.perception.requireLos ? "true" : "false")
      << ", \"aggro\": \"" << AggroName(p.perception.aggro)
      << "\", \"alertDecayTicks\": " << p.perception.alertDecayTicks
      << ", \"keepRangeScale\": " << num(p.perception.keepRangeScale)
      << ", \"stickiness\": " << num(p.perception.stickiness)
      << ", \"preferWeak\": " << num(p.perception.preferWeak)
      << ", \"provokeTicks\": " << p.perception.provokeTicks << " },\n";
    o << "      \"movement\": { \"mobile\": "
      << (p.movement.mobile ? "true" : "false")
      << ", \"rangeMin\": " << num(p.movement.rangeMin)
      << ", \"rangeMax\": " << num(p.movement.rangeMax)
      << ", \"bandSlack\": " << num(p.movement.bandSlack)
      << ", \"approachSpeed\": " << num(p.movement.approachSpeed)
      << ", \"strafeSpeed\": " << num(p.movement.strafeSpeed)
      << ", \"retreatSpeed\": " << num(p.movement.retreatSpeed)
      << ", \"fleeSpeed\": " << num(p.movement.fleeSpeed)
      << ", \"circleTendency\": " << num(p.movement.circleTendency)
      << ", \"circleHoldTicks\": " << p.movement.circleHoldTicks
      << ", \"repathTicks\": " << p.movement.repathTicks
      << ", \"navRadius\": " << num(p.movement.navRadius)
      << ", \"maxStepUp\": " << p.movement.maxStepUp
      << ", \"maxStepDown\": " << p.movement.maxStepDown
      << ", \"headroom\": " << p.movement.headroom
      << ", \"dodgeSpeed\": " << num(p.movement.dodgeSpeed)
      << ", \"flank\": " << num(p.movement.flank)
      << ", \"keepOut\": " << num(p.movement.keepOut);
    if (p.movement.wanderRadius > 0.0f)
      o << ", \"wander\": { \"radius\": " << num(p.movement.wanderRadius)
        << ", \"speed\": " << num(p.movement.wanderSpeed)
        << ", \"pauseMin\": " << p.movement.wanderPauseMin
        << ", \"pauseMax\": " << p.movement.wanderPauseMax
        << ", \"arrive\": " << num(p.movement.wanderArrive) << " }";
    o << " },\n";
    o << "      \"attack\": { \"styles\": [";
  for (size_t i = 0; i < p.attack.styles.size(); i++)
    o << (i ? ", " : "") << "\"" << p.attack.styles[i] << "\"";
  o << "]"
      << ", \"reach\": " << num(p.attack.reach)
      << ", \"aimTolerance\": " << num(p.attack.aimTolerance)
      << ", \"cadenceTicks\": " << p.attack.cadenceTicks
      << ", \"jitterTicks\": " << p.attack.jitterTicks
      << ", \"commitTicks\": " << p.attack.commitTicks
      << ", \"disengageTicks\": " << p.attack.disengageTicks
      << ", \"pursueSpeed\": " << num(p.attack.pursueSpeed)
      << ", \"holdGroundFrac\": " << num(p.attack.holdGroundFrac)
      << ", \"leadTicks\": " << p.attack.leadTicks
      << ", \"feintChance\": " << num(p.attack.feintChance)
      << ", \"feintTicks\": " << p.attack.feintTicks
      << ", \"feintFollowTicks\": " << p.attack.feintFollowTicks
      << ", \"riposteTicks\": " << p.attack.riposteTicks << " },\n";
    o << "      \"defense\": { \"skill\": " << num(p.defense.skill)
      << ", \"reactTicks\": " << p.defense.reactTicks
      << ", \"reactJitter\": " << p.defense.reactJitter
      << ", \"margin\": " << num(p.defense.margin)
      << ", \"guardStance\": " << num(p.defense.guardStance)
      << ", \"guardReach\": " << num(p.defense.guardReach)
      << ", \"holdTicks\": " << p.defense.holdTicks << " },\n";
    o << "      \"intents\": {\n";
    bool first = true;
    for (int k = 0; k < (int)Intent::Count; k++) {
      const IntentTuning& t = p.intents[k];
      if (t.weight <= 0 && t.cooldownTicks == 0 && t.minDwellTicks == 0) continue;
      if (!first) o << ",\n";
      first = false;
      o << "        \"" << IntentName((Intent)k)
        << "\": { \"weight\": " << num(t.weight)
        << ", \"cooldownTicks\": " << t.cooldownTicks
        << ", \"minDwellTicks\": " << t.minDwellTicks << " }";
    }
    o << "\n      }";
    if (!p.rules.empty()) {
      o << ",\n      \"rules\": [\n";
      for (size_t r = 0; r < p.rules.size(); r++) {
        const Rule& rl = p.rules[r];
        o << "        { ";
        if (!rl.note.empty()) o << "\"note\": \"" << Escaped(rl.note) << "\", ";
        o << "\"when\": {";
        for (size_t c = 0; c < rl.when.size(); c++)
          o << (c ? ", " : " ") << "\"" << FactName(rl.when[c].fact) << "\": \""
            << OpText(rl.when[c].op) << num(rl.when[c].value) << "\"";
        o << (rl.when.empty() ? "}" : " }");
        // `set` = the weight map (unset is < 0); otherwise the scale map
        // (unset is exactly 1).
        auto intentMap = [&](const char* key, const float (&v)[(int)Intent::Count],
                             bool set) {
          bool any = false;
          for (int k = 0; k < (int)Intent::Count; k++) {
            if (set ? v[k] < 0.0f : v[k] == 1.0f) continue;
            o << (any ? std::string(", ") : std::string(", \"") + key + "\": { ")
              << "\"" << IntentName((Intent)k) << "\": " << num(v[k]);
            any = true;
          }
          if (any) o << " }";
        };
        intentMap("weight", rl.setWeight, true);
        intentMap("scale", rl.scale, false);
        {
          bool any = false;
          for (int k = 0; k < (int)Tune::Count; k++) {
            if (!rl.tuneSet[k]) continue;
            o << (any ? std::string(", ") : std::string(", \"tune\": { "))
              << "\"" << TuneName((Tune)k) << "\": " << num(rl.tune[k]);
            any = true;
          }
          if (any) o << " }";
        }
        o << " }" << (r + 1 < p.rules.size() ? "," : "") << "\n";
      }
      o << "      ]";
    }
    o << "\n    }" << (i + 1 < lib.profiles.size() ? "," : "") << "\n";
  }
  o << "  ]\n}\n";

  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    err = "could not open " + path + " for writing";
    return false;
  }
  f << o.str();
  return true;
}

// ---------------------------------------------------------------------------
// Perception
// ---------------------------------------------------------------------------
namespace {

// Where a creature's eyes are. Not the AABB centre: a line of sight taken from
// the middle of a body clips the ground on a downhill and reports "blocked"
// when the thing is in plain view.
Vec3 EyeOf(const SelfView& s) {
  return Vec3{s.origin.x + s.size.x * 0.5f, s.origin.y + s.size.y * 0.82f,
              s.origin.z + s.size.z * 0.5f};
}
Vec3 EyeOf(const Actor& a) {
  return Vec3{a.centre.x, a.centre.y + a.height * 0.3f, a.centre.z};
}

const Actor* FindActor(const WorldView& v, uint64_t id) {
  if (v.actors == nullptr) return nullptr;
  for (const Actor& a : *v.actors)
    if (a.id == id) return &a;
  return nullptr;
}

// Can `self` perceive `a` right now? Distance, then cone, then (cheapest last,
// because it is the expensive one) the line -- split at the line so Perceive
// can rank the cheap survivors before it asks any line (its note).
bool PerceivesNoLine(const SelfView& self, const Profile& pr, const Actor& a,
                     float range, float& outDist) {
  outDist = PlanarDist(self.Centre(), a.centre);
  if (outDist > range) return false;
  if (pr.perception.fovDegrees < 359.9f) {
    const float bearing = BearingTo(self.Centre(), a.centre);
    const float err = std::abs(WrapPi(bearing - self.heading));
    if (err > pr.perception.fovDegrees * (kPi / 180.0f) * 0.5f) return false;
  }
  return true;
}
bool PerceivesLine(const SelfView& self, const Profile& pr, const WorldView& v,
                   const Actor& a) {
  if (pr.perception.requireLos && v.lineOfSight != nullptr)
    if (!v.lineOfSight(v.losCtx, EyeOf(self), EyeOf(a))) return false;
  return true;
}

void Perceive(Brain& b, const Profile& pr, const SelfView& self,
              const WorldView& v, uint32_t tick) {
  b.visible = false;
  if (pr.perception.aggro == Aggro::Passive || pr.perception.sightRange <= 0 ||
      v.actors == nullptr) {
    b.hasTarget = false;
    b.targetId = 0;
    return;
  }

  // The range test is asymmetric on purpose: a target already being tracked is
  // held to a LARGER radius than one being acquired. Without that, anything
  // sitting on the boundary is acquired and dropped on alternate ticks, which
  // makes the whole arbiter above it oscillate for reasons that look like a bug
  // in the arbiter.
  const float acquire = pr.perception.sightRange;
  const float keep = acquire * std::max(1.0f, pr.perception.keepRangeScale);

  // NEAREST, ADJUSTED (Perception::stickiness / preferWeak). Both are 0 on a
  // profile that does not author them, and then `score == d` exactly — the
  // plain nearest-enemy pick every profile had before.
  // NEUTRAL (P7): nobody is a NEW target until this creature has been hurt,
  // and then only for `provokeTicks`; one it already holds is kept below
  // exactly as a hostile creature keeps one.
  const bool neutral = pr.perception.aggro == Aggro::Neutral;
  const bool provoked =
      b.everHurt && tick - b.hurtTick <= pr.perception.provokeTicks;
  const Actor* best = nullptr;
  float bestDist = 1e30f;
  // THE LINE OF SIGHT IS ASKED IN SCORE ORDER, AND ONLY UNTIL ONE ANSWERS
  // (PLAN_fight64_perf M). The pick is the lowest score among the actors that
  // pass all three tests (range, cone, line), earliest in the list on a tie.
  // The line is the expensive test and the other two decide nothing about it,
  // so the cheap survivors are ranked by (score, list order) and the line is
  // walked down that ranking: the first that sees is exactly the actor the
  // all-tests loop would have kept, and in a 64-creature brawl it is nearly
  // always the first asked instead of every foe in range.
  struct Cand {
    float score, d;
    const Actor* a;
    uint32_t order;
  };
  thread_local std::vector<Cand> cands;
  cands.clear();
  uint32_t order = 0;
  for (const Actor& a : *v.actors) {
    order++;
    if (!a.alive || a.id == self.id) continue;
    if (a.faction == self.faction) continue;
    float d = 0;
    const bool current = b.hasTarget && a.id == b.targetId;
    if (neutral && !current && !provoked) continue;
    const float range = current ? keep : acquire;
    if (!PerceivesNoLine(self, pr, a, range, d)) continue;
    float score = d;
    if (current) score -= pr.perception.stickiness;
    // Whoever is fighting ME is the one a provoked neutral answers.
    if (neutral && a.targetId == self.id) score -= 1000.0f;
    score -= pr.perception.preferWeak *
             (1.0f - std::clamp(a.hpFrac, 0.0f, 1.0f));
    // The all-tests loop kept an actor only on `score < best` from 1e30: a
    // NaN, or a score at or past 1e30, was never kept, so it is not a candidate.
    if (!(score < 1e30f)) continue;
    cands.push_back({score, d, &a, order});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
    return x.score < y.score || (x.score == y.score && x.order < y.order);
  });
  for (const Cand& c : cands) {
    if (!PerceivesLine(self, pr, v, *c.a)) continue;
    best = c.a;
    bestDist = c.d;
    break;
  }

  if (best != nullptr) {
    b.targetId = best->id;
    b.hasTarget = true;
    b.visible = true;
    b.targetPos = best->centre;
    b.lastSeenPos = best->centre;
    b.lastSeenTick = tick;
    b.targetDist = bestDist;
    return;
  }

  // Nothing perceived. MEMORY, not amnesia: keep heading for the last known
  // spot until the alert decays, which is the difference between "it lost me
  // behind a rock and came looking" and "it lost me behind a rock and forgot I
  // exist".
  if (!b.hasTarget) return;
  if (tick > b.lastSeenTick + pr.perception.alertDecayTicks) {
    b.hasTarget = false;
    b.targetId = 0;
    return;
  }
  const Actor* held = FindActor(v, b.targetId);
  if (held != nullptr && !held->alive) {
    b.hasTarget = false;
    b.targetId = 0;
    return;
  }
  b.targetPos = b.lastSeenPos;
  b.targetDist = PlanarDist(self.Centre(), b.targetPos);
}

// ---------------------------------------------------------------------------
// HOW THE TARGET IS MOVING (2026-09-16)
//
// One finite difference and one low pass, and it is the only thing in this file
// that looks forward. Three decisions below cannot be made without it — where
// to aim a blow that lands fifteen ticks from now, whether the range will still
// be closed when it does, and whether the post-swing step-off is footwork or
// surrender — and all three were being made against the target's PRESENT
// position, which is why a creature walking backwards was untouchable.
//
// IT SAMPLES ONLY BETWEEN CONSECUTIVE VISIBLE TICKS OF ONE TARGET, and each of
// those three words is load-bearing:
//
//   * VISIBLE — `Perceive` freezes `targetPos` at `lastSeenPos` while the alert
//     decays, so differencing a remembered position reports a creature standing
//     perfectly still for as long as it stays out of sight. That is the single
//     most dangerous reading available: it says "no lead needed" about exactly
//     the target that just broke contact.
//   * CONSECUTIVE — the tick a target is re-acquired, the difference spans the
//     whole occlusion and reports a teleport.
//   * ONE TARGET — `Perceive` takes the NEAREST hostile every tick and will
//     switch mid-fight; differencing across a switch measures the gap between
//     two creatures, not the motion of either.
//
// Failing any of them zeroes the velocity rather than holding the last one.
// A stale extrapolation aims worse than no extrapolation: aiming where a
// creature IS misses a moving one, but aiming fifteen ticks along a heading it
// abandoned two seconds ago misses a stationary one too.
// ---------------------------------------------------------------------------
void TrackTargetMotion(Brain& b, const SelfView& self, uint32_t tick, float dt) {
  b.targetRadial = 0;
  if (!b.hasTarget || !b.visible) {
    b.haveTargetVel = false;
    b.targetVel = Vec3{};
    return;
  }

  const bool usable = b.haveTargetVel && tick == b.velSampleTick + 1 &&
                      b.velTargetId == b.targetId && dt > 1e-5f;
  if (usable) {
    float ix = (b.targetPos.x - b.prevTargetPos.x) / dt;
    float iz = (b.targetPos.z - b.prevTargetPos.z) / dt;
    // A SANITY CEILING, not a tuning knob. A body that is launched by a blast,
    // knocked back by a mace or moved by a gate fixture differences into
    // hundreds of voxels a second, and one such sample through the low pass
    // would aim the next several swings at the horizon.
    const float cap = std::max(1.0f, self.speed) * 4.0f;
    const float m = std::sqrt(ix * ix + iz * iz);
    if (m > cap) {
      ix *= cap / m;
      iz *= cap / m;
    }
    // A gait bobs and yaws the body centre by a fraction of a voxel every tick,
    // which the raw difference reports as several voxels a second of sideways
    // drift. Unfiltered, the aim point wanders by about that much per tick and
    // a lead is worse than none.
    constexpr float kAlpha = 0.25f;
    b.targetVel.x += (ix - b.targetVel.x) * kAlpha;
    b.targetVel.z += (iz - b.targetVel.z) * kAlpha;
  } else {
    b.targetVel = Vec3{};
  }
  b.prevTargetPos = b.targetPos;
  b.velSampleTick = tick;
  b.velTargetId = b.targetId;
  b.haveTargetVel = true;

  // The RADIAL component is the only one the decisions below want: a target
  // circling us at speed is not getting away, and treating its tangential
  // motion as flight would have every creature charge a fencer who has not
  // moved an inch further off.
  const Vec3 c = self.Centre();
  const float dx = b.targetPos.x - c.x, dz = b.targetPos.z - c.z;
  const float len = std::sqrt(dx * dx + dz * dz);
  if (len > 1e-3f)
    b.targetRadial = (b.targetVel.x * dx + b.targetVel.z * dz) / len;
}

// ---------------------------------------------------------------------------
// Navigation follow-through
// ---------------------------------------------------------------------------

// How close a waypoint has to be before the follower moves to the next one.
// Generous: a body with a stride cannot stand on a point, and a tight radius
// makes it circle its own waypoint forever.
constexpr float kWaypointRadius = 1.9f;

void UpdatePath(Brain& b, const Profile& pr, const SelfView& self,
                const WorldView& v, uint32_t tick, const Vec3& goal) {
  NavParams np;
  np.radius = (int)std::lround(pr.movement.navRadius);
  // THE BODY'S BUDGET UNLESS THE PROFILE OVERRODE IT. A planner that refuses
  // what the drive would happily climb makes a creature stand at the foot of a
  // slope; a planner that permits what the drive refuses makes it grind against
  // one. Both are the same bug -- two copies of one number -- and the fix is to
  // stop having two (SelfView::stepUpCells, resolved from the rig in
  // Mob::BuildRig).
  np.maxStepUp =
      pr.movement.maxStepUp > 0 ? pr.movement.maxStepUp : self.stepUpCells;
  np.maxStepDown =
      pr.movement.maxStepDown > 0 ? pr.movement.maxStepDown : self.stepDownCells;
  // HEADROOM MUST NOT BE STRICTER THAN THE LOCOMOTION IT STEERS. The obvious
  // value is the creature's own height, and it is wrong: the walk drive checks
  // ground rise and nothing else, so a planner that demands 13 clear voxels
  // over every column refuses whole regions — anything under a canopy, a ledge
  // or an arch — that the mob would in fact walk straight through. The planner
  // then reports "no way around" for a wall with open ground on both sides.
  // Three voxels is enough to reject a crawlspace and nothing else, and being
  // LOOSER than the drive is the safe direction to be wrong in: the drive's own
  // probe still refuses what it cannot walk.
  np.headroom =
      pr.movement.headroom > 0 ? pr.movement.headroom : self.headroomCells;

  // Retire waypoints we have arrived at BEFORE deciding whether to replan, so
  // "the path is finished" and "the path is stale" are different answers.
  const Vec3 foot = self.Foot();
  while (!b.path.Done() &&
         PlanarDist(foot, b.path.Current()) <= kWaypointRadius) {
    b.path.cursor++;
    b.lastWaypointDist = 1e9f;   // the progress clock is per WAYPOINT
    b.stuckTicks = 0;
  }

  // A LIVE PATH IS KEPT UNTIL IT IS ACTUALLY WRONG. The cadence is permission
  // to RE-EXAMINE, not an instruction to re-plan.
  //
  // Measured, and it is the single worst failure mode this planner has: routing
  // around a wall from the left and from the right cost almost exactly the
  // same, so a from-scratch plan every ten ticks alternated between them. The
  // mob walked 94 voxels in 400 ticks and made no progress at all — it spent
  // every tick turning, which also drove `align` (and therefore its forward
  // speed) to nearly nothing. Symmetric obstacles are the common case, not a
  // pathological one, so the fix has to be structural rather than a tie-break.
  if (tick < b.nextRepathTick && !b.path.Done()) return;
  b.nextRepathTick = tick + std::max(2u, pr.movement.repathTicks);

  // THE STRAIGHT LINE FIRST. Most of a fight is fought in the open, and asking
  // A* to confirm what a 24-sample line already knows is the expensive way to
  // learn nothing. This is also where a mob that has just rounded an obstacle
  // DROPS its detour, and the graceful-degradation path: with no plan and a
  // clear line, "walk at them" is exactly right.
  if (LineWalkable(v.probe, np, foot, goal)) {
    b.path.Clear();
    b.navFailed = false;
    return;
  }

  // Still committed? A live path is given up for exactly two reasons: the
  // target walked away from the route it was planned for (so it leads to where
  // you WERE), or the mob has STOPPED MAKING PROGRESS along it.
  //
  // The progress test is what replaced "is the straight line to my next
  // waypoint still walkable", which sounds like the right question and is not.
  // A string-pulled waypoint is straight-line reachable FROM WHERE THE PATH WAS
  // PLANNED; once the body has drifted a voxel off that line the segment can
  // clip the corner it was routed around, so the test failed on every healthy
  // path that hugged an obstacle — the mob threw its route away every ten ticks
  // and milled in front of the wall for 600 of them. Progress is the honest
  // question, and it also covers the destructible-terrain case this world
  // actually has: a path that now runs into a fresh crater stops making
  // progress by itself.
  if (!b.path.Done() && PlanarDist(b.pathTarget, goal) <= 4.0f) {
    const float dw = PlanarDist(foot, b.path.Current());
    if (dw < b.lastWaypointDist - 0.25f) {
      b.lastWaypointDist = dw;
      b.stuckTicks = 0;
      return;
    }
    b.stuckTicks += std::max(2u, pr.movement.repathTicks);
    if (b.stuckTicks < 30) return;
  }

  b.pathTarget = goal;
  b.replans++;
  b.stuckTicks = 0;
  b.navFailed = !FindPath(v.probe, np, foot, goal, b.path);
  b.lastWaypointDist =
      b.path.Done() ? 1e9f : PlanarDist(foot, b.path.Current());
}

// Where the follower currently wants to walk: the live waypoint, or the target
// itself when there is no path (open ground, or the planner gave up).
Vec3 SteerPoint(const Brain& b, const Vec3& goal) {
  return b.path.Done() ? goal : b.path.Current();
}

}  // namespace

// ---------------------------------------------------------------------------
// The arbiter
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Rules: the facts, then which rules hold (ai_behavior.h "RULES")
// ---------------------------------------------------------------------------
namespace {

constexpr float kNever = 1.0e9f;

// HURT = life went down since the last think, by any cause. Bleeding counts:
// a creature losing blood IS being hurt, and a rule that wants "struck in the
// last second" can say sinceHurt < 30 and let a slow bleed keep it true.
// Run FIRST in Think (before Perceive), because a neutral creature's
// perception depends on it (Perception::provokeTicks).
void NoteHurt(Brain& b, const SelfView& self, uint32_t tick) {
  if (b.prevHp >= 0.0f && self.hpFrac < b.prevHp) {
    b.hurtTick = tick;
    b.everHurt = true;
  }
  b.prevHp = self.hpFrac;
}

void EvaluateRules(Brain& b, const Profile& pr, const SelfView& self,
                   const WorldView& view, uint32_t tick) {

  // Who is around, within what this creature can see. No line-of-sight test:
  // these are counts for character ("alone", "outnumbered"), not targets, and
  // a LOS ray per actor per tick is the cost rule 2 says to keep off this path.
  // The nearest ENEMY is kept as the threat Flee runs from when there is no
  // target (a passive creature never picks one, and is the one most likely to
  // be authored to run).
  const Vec3 c = self.Centre();
  const float range = pr.perception.sightRange;
  int allies = 0, enemies = 0, hostiles = 0;
  float nearest = kNever, nearestHostile = kNever;
  b.hasThreat = false;
  Vec3 hostilePos{};
  if (view.actors != nullptr && range > 0.0f) {
    for (const Actor& a : *view.actors) {
      if (!a.alive || a.id == self.id) continue;
      const float d = PlanarDist(c, a.centre);
      if (d > range) continue;
      if (a.faction == self.faction) {
        allies++;
      } else {
        enemies++;
        if (d < nearest) {
          nearest = d;
          b.threatPos = a.centre;
          b.hasThreat = true;
        }
        if (a.hostile) {
          hostiles++;
          if (d < nearestHostile) {
            nearestHostile = d;
            hostilePos = a.centre;
          }
        }
      }
    }
  }
  // A HOSTILE is the threat when there is one: a villager bolting from the
  // zombie must not run from the (nearer) player standing beside it.
  if (hostiles > 0) b.threatPos = hostilePos;
  if (b.hasTarget) {
    b.threatPos = b.targetPos;
    b.hasThreat = true;
  }

  float* f = b.facts;
  f[(int)Fact::Hp] = self.hpFrac;
  f[(int)Fact::Burning] = self.burningFrac;
  f[(int)Fact::LimbsLost] = (float)self.limbsLost;
  f[(int)Fact::Shocked] = self.shocked;
  f[(int)Fact::SinceHurt] = b.everHurt ? (float)(tick - b.hurtTick) : kNever;
  f[(int)Fact::HasTarget] = b.hasTarget ? 1.0f : 0.0f;
  f[(int)Fact::Visible] = b.visible ? 1.0f : 0.0f;
  f[(int)Fact::TargetDist] = b.hasTarget ? b.targetDist : kNever;
  f[(int)Fact::Allies] = (float)allies;
  f[(int)Fact::Enemies] = (float)enemies;
  f[(int)Fact::Hostiles] = (float)hostiles;

  // ---- WHAT I HOLD AND WHAT I FACE (2026-09-27) ---------------------------
  // The target's side is read off its published Actor: what a watching
  // creature could see of it. A target that is not in the list (a gate's
  // bare fixture, a despawn this tick) reads as unknown-and-harmless.
  const Actor* tgt = b.hasTarget ? FindActor(view, b.targetId) : nullptr;
  const float myReach = std::max(0.0f, self.strikeReach);
  b.targetReach = tgt != nullptr ? std::max(0.0f, tgt->reach) : 0.0f;
  b.targetHp = tgt != nullptr ? std::clamp(tgt->hpFrac, 0.0f, 1.0f) : 0.0f;
  b.targetArmed = tgt != nullptr && tgt->armed;
  b.targetAction = tgt != nullptr ? tgt->action : Action::None;
  f[(int)Fact::Armed] = self.armed ? 1.0f : 0.0f;
  f[(int)Fact::MyReach] = myReach;
  f[(int)Fact::TargetReach] = b.targetReach;
  f[(int)Fact::ReachAdv] = tgt != nullptr ? myReach - b.targetReach : 0.0f;
  f[(int)Fact::TargetArmed] = b.targetArmed ? 1.0f : 0.0f;
  f[(int)Fact::TargetHp] = b.targetHp;
  f[(int)Fact::HpAdv] = tgt != nullptr ? self.hpFrac - b.targetHp : 0.0f;
  f[(int)Fact::TargetAttacking] =
      (b.targetAction == Action::Windup || b.targetAction == Action::Cut)
          ? 1.0f : 0.0f;
  f[(int)Fact::TargetRecovering] =
      b.targetAction == Action::Recover ? 1.0f : 0.0f;
  f[(int)Fact::TargetGuarding] = b.targetAction == Action::Guard ? 1.0f : 0.0f;
  // "Inside its reach" carries the same half-a-body slack the stroke system
  // forgives on the attacking side (BeginStroke): the reach is an estimate of
  // a centre-to-centre distance for a surface-to-surface event.
  f[(int)Fact::InTheirReach] =
      tgt != nullptr && b.targetReach > 0.0f &&
              b.targetDist <= b.targetReach + self.size.z * 0.5f
          ? 1.0f : 0.0f;
  float facing = 0.0f;
  if (tgt != nullptr) {
    const float dx = c.x - tgt->centre.x, dz = c.z - tgt->centre.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-3f)
      facing = (std::sin(tgt->heading) * dx + std::cos(tgt->heading) * dz) / len;
  }
  f[(int)Fact::TargetFacingMe] = facing;

  // THE QUEUE AROUND ONE TARGET. Allies are anyone of our faction who has
  // named the same target (Actor::targetId); rank is how many of them stand
  // nearer it than we do, ties to the lower id so two equidistant creatures
  // cannot both believe they are first. Not range-limited: this is about the
  // fight, and a creature out of sight of its friend is still in the queue.
  b.engagedAllies = 0;
  b.pressRank = 0;
  if (tgt != nullptr && view.actors != nullptr) {
    const float mine = PlanarDist(c, b.targetPos);
    for (const Actor& a : *view.actors) {
      if (!a.alive || a.id == self.id || a.faction != self.faction) continue;
      if (a.targetId != b.targetId) continue;
      b.engagedAllies++;
      const float theirs = PlanarDist(a.centre, b.targetPos);
      if (theirs < mine || (theirs == mine && a.id < self.id)) b.pressRank++;
    }
  }
  f[(int)Fact::EngagedAllies] = (float)b.engagedAllies;
  f[(int)Fact::PressRank] = (float)b.pressRank;

  b.rulesHeld = 0;
  for (size_t r = 0; r < pr.rules.size() && r < 32; r++) {
    bool all = true;
    for (const Condition& cnd : pr.rules[r].when)
      if (!Holds(cnd, f[(int)cnd.fact])) {
        all = false;
        break;
      }
    if (all) b.rulesHeld |= 1u << r;
  }

  // ---- THE TUNE, after every holding rule ---------------------------------
  // Identity first (scale 1, add 0, keepOut = the profile's own), so a
  // profile with no `tune` anywhere fights exactly as authored.
  for (int k = 0; k < (int)Tune::Count; k++) {
    const TuneMode m = TuneModeOf((Tune)k);
    b.tune[k] = m == TuneMode::Scale ? 1.0f
              : m == TuneMode::Add   ? 0.0f
                                     : pr.movement.keepOut;
  }
  for (size_t r = 0; r < pr.rules.size() && r < 32; r++) {
    if (!(b.rulesHeld & (1u << r))) continue;
    const Rule& rl = pr.rules[r];
    for (int k = 0; k < (int)Tune::Count; k++) {
      if (!rl.tuneSet[k]) continue;
      switch (TuneModeOf((Tune)k)) {
        case TuneMode::Scale: b.tune[k] *= std::max(0.0f, rl.tune[k]); break;
        case TuneMode::Add: b.tune[k] += rl.tune[k]; break;
        case TuneMode::Set: b.tune[k] = rl.tune[k]; break;
      }
    }
  }
}

// An intent's weight THIS tick: the authored one, then every holding rule in
// order (a `weight` replaces, a `scale` multiplies).
float EffectiveWeight(const Brain& b, const Profile& pr, int i) {
  float w = pr.intents[i].weight;
  for (size_t r = 0; r < pr.rules.size() && r < 32; r++) {
    if (!(b.rulesHeld & (1u << r))) continue;
    const Rule& rl = pr.rules[r];
    if (rl.setWeight[i] >= 0.0f) w = rl.setWeight[i];
    w *= rl.scale[i];
  }
  return w;
}

}  // namespace

bool Think(Brain& brain, const Library& lib, const SelfView& self,
           const GroundView& ground, const WorldView& view, uint32_t tick,
           float dt, IntentOut& out) {
  const Profile* prof = lib.At(brain.profile);
  if (prof == nullptr) return false;
  const Profile& pr = *prof;

  out.desiredHeading = self.heading;
  out.driveScale = 0;
  out.driveStrafe = 0;
  out.attack = false;

  out.guard = false;
  out.cancelSwing = false;

  NoteHurt(brain, self, tick);
  Perceive(brain, pr, self, view, tick);
  TrackTargetMotion(brain, self, tick, dt);
  EvaluateRules(brain, pr, self, view, tick);
  const float* tn = brain.tune;
  const float spd = tn[(int)Tune::Speed];
  const Actor* tgtActor =
      brain.hasTarget ? FindActor(view, brain.targetId) : nullptr;

  // ---- READING THE TARGET'S BLOW (Defense) --------------------------------
  //
  // One blow = one windup onset, and the reaction to it is decided ONCE, at
  // the onset: a skill roll (does this creature read it at all) and a delay
  // (how long until it can answer). Re-rolling per tick would turn a 10%
  // skill into a near-certain parry over a fifteen-tick windup, and a delay
  // counted from "now" would never elapse. Counter-based on (id, onset), so
  // a replayed fight reads the same blows the same way.
  //
  // Only a VISIBLE target's blows are read: a creature does not parry what
  // it cannot see, which is also what makes attacking from behind worth it.
  {
    const bool blowNow =
        brain.hasTarget && brain.visible &&
        (brain.targetAction == Action::Windup ||
         brain.targetAction == Action::Cut);
    if (brain.blowLive && brain.blowTarget != brain.targetId) {
      brain.blowLive = false;
      brain.blowEndTick = 0;
      brain.reacts = false;
    }
    if (blowNow && !brain.blowLive) {
      brain.blowLive = true;
      brain.blowTarget = brain.targetId;
      brain.blowOnset = tick;
      const float skill =
          std::clamp(pr.defense.skill * tn[(int)Tune::Skill], 0.0f, 1.0f);
      const uint32_t h =
          rng::Hash3((uint32_t)self.id ^ kSaltReact, tick, 2);
      brain.reacts =
          skill > 0.0f && (float)(h & 0xFFFFu) * (1.0f / 65536.0f) < skill;
      const uint32_t jit =
          pr.defense.reactJitter > 0 ? (h >> 16) % pr.defense.reactJitter : 0;
      brain.reactAt =
          tick +
          (uint32_t)std::lround((float)pr.defense.reactTicks *
                                std::max(0.0f, tn[(int)Tune::React])) +
          jit;
    } else if (!blowNow && brain.blowLive) {
      brain.blowLive = false;
      brain.blowEndTick = tick;
    }
  }

  // ---- A FEINT PULLS OUT ---------------------------------------------------
  // Before the arbiter, so the facing lock it releases is released THIS tick.
  // The real blow is booked through the riposte clock: same seam, same
  // "whatever the cadence says" meaning.
  if (brain.feinting && tick >= brain.feintAt) {
    brain.feinting = false;
    out.cancelSwing = true;
    brain.feints++;
    brain.commitUntil = std::min(brain.commitUntil, tick);
    brain.disengageUntil = std::min(brain.disengageUntil, tick);
    brain.riposteArmed = true;
    brain.riposteAt = tick + pr.attack.feintFollowTicks;
  }
  // A booked riposte that never fired (the target left, the creature was
  // busy) must not linger as a standing licence to ignore the cadence.
  if (brain.riposteArmed && tick > brain.riposteAt + 20) brain.riposteArmed = false;

  const Vec3 centre = self.Centre();
  const float bearing =
      brain.hasTarget ? BearingTo(centre, brain.targetPos) : self.heading;
  brain.bearingError = WrapPi(bearing - self.heading);
  const float aimErr = std::abs(brain.bearingError);

  // ---- band geometry ------------------------------------------------------
  // Everything about footwork is expressed against these two numbers. During
  // the disengage window the FLOOR of the band is lifted to its ceiling, which
  // is the whole "hit and step off" behaviour: the mob is momentarily content
  // only at the outer edge, so HoldRange gives ground without needing an
  // intent of its own.
  float lo = pr.movement.rangeMin, hi = pr.movement.rangeMax;

  // ---- THE BAND FOLLOWS THE WEAPON (2026-09-16) ---------------------------
  //
  // An authored band is a CHARACTER — "I like to fight four voxels wide, at
  // arm's length, and step off after" — and it was being read as a fixed pair
  // of distances. Those are not the same claim, and the difference is every
  // reported symptom of a passive AI: `duelist` authors 7..11 because a SWORD
  // lands at about 9, and hands the same 7..11 to a creature holding a dagger
  // that lands at 3.5. It then stands at 9 and swings at nothing, because
  // `BeginStroke` refuses a style the target is outside the reach of and the
  // profile's own `attack.reach` floor (see SelfView::strikeReach) keeps
  // saying the range is fine.
  //
  // So the band is slid INSIDE what this body can actually hit: keep its
  // authored WIDTH and drop its CEILING onto strike distance. Width is the
  // character and survives; the distances are facts about the weapon and do
  // not.
  //
  // THE CEILING RATHER THAN THE MIDDLE, and that is the second version of this
  // rule. Anchoring the MIDDLE was tried first and left the top half of the
  // band outside the weapon — which sounds harmless and is not, because the
  // disengage window below lifts the floor to the ceiling after every swing.
  // A creature therefore spent `disengageTicks` of every cadence parked at
  // exactly the one edge of its band it could not strike from, and committed
  // from there as often as not. Measured through `ai-reach`: bare fists, reach
  // 5.0 plus 1.1 of body, settled at 6.3 and landed ONE hit in 420 ticks. The
  // honest statement is "wherever in my band I am standing, I can hit you",
  // and only the ceiling expresses it.
  //
  // `+ half a body depth` is the same slack `BeginStroke` forgives, and for
  // the same reason: `StrikeReachOf` estimates a centre-to-centre distance for
  // a surface-to-surface event, so the victim's own width is the term it is
  // missing. Matching the two means the band's far edge lands exactly on the
  // last distance a swing is accepted from instead of a hair outside it.
  //
  // INWARD ONLY, deliberately: a creature must never be pushed FURTHER out
  // than its author asked. A zombie's longest drawable style is a 23-voxel
  // lunging bite, and widening its band onto that would have it orbit at 23
  // and pounce from across a field — a different creature. It keeps 2..7.
  if (self.strikeReach > 0.0f && hi > lo) {
    const float ceiling = self.strikeReach + self.size.z * 0.5f;
    if (hi > ceiling) {
      const float shift = hi - ceiling;
      // The floor may reach 0 — two bodies are held apart by ApplyCrowdSpacing
      // anyway, so a band that asks for contact gets as close as the spacing
      // rule permits and no closer. It must not INVERT, though: hi is pinned
      // above lo so the Schmitt trigger below still has a band to trigger on.
      lo = std::max(0.0f, lo - shift);
      hi = std::max(lo + 0.5f, hi - shift);
    }
  }

  // ---- ...THEN THE RULES MOVE IT (Tune::Band, Tune::KeepOut) --------------
  // After the weapon has placed the band, so "wounded: stand three voxels
  // further out" means three voxels further out than THIS weapon wants —
  // and it may push the ceiling past strike reach on purpose: a creature
  // hanging back is one that has stopped offering blows.
  const float bandAdd = tn[(int)Tune::Band];
  if (bandAdd != 0.0f && hi > 0.0f) {
    lo = std::max(0.0f, lo + bandAdd);
    hi = std::max(lo + 0.5f, hi + bandAdd);
  }
  // KITING: stand where I land and it does not. Only when that place EXISTS
  // (my ceiling is past its reach), otherwise there is no such band and the
  // creature fights where its weapon says.
  const float keepOut = tn[(int)Tune::KeepOut];
  if (keepOut >= 0.0f && brain.targetReach > 0.0f && hi > 0.0f) {
    const float floor = brain.targetReach + self.size.z * 0.5f + keepOut;
    if (floor < hi - 0.5f) lo = std::max(lo, floor);
  }

  // ---- ...AND THE STEP-OFF IS SUSPENDED AGAINST A TARGET THAT IS LEAVING ---
  //
  // `disengageTicks` is "hit and step off", and it is exactly right against
  // someone standing their ground. Against someone WALKING AWAY it is a SECOND
  // RETREAT: the mob gives ground for 26 ticks to a target already giving
  // ground, the gap grows at the sum of both speeds, and by the time the
  // cadence comes round the creature is outside its band and starts the whole
  // approach again. It never gets a second swing off.
  //
  // This is a DIFFERENT failure from the frozen feet the pursuit term below
  // fixes, and it is the one that survives it: it happens after the commit
  // window has expired, with the arbiter picking exactly the intent its author
  // asked for. Both had to go for "crouchwalk backwards and nothing can touch
  // you" to stop being true.
  //
  // THE THRESHOLD IS A NOISE FLOOR AND NOTHING MORE. Any target opening the
  // range is one you do not step away from; all a number is needed for is to
  // stop a gait's bob from reading as flight. AttackTuning::holdGroundFrac
  // records what sizing it as a judgement about PACE cost -- the first version
  // was 0.30 and never fired once against the crouchwalking player it was
  // written for.
  //
  // AUTHORED, so the whole of this change is
  // reversible from JSON: a profile with a huge `holdGroundFrac`, `pursueSpeed`
  // 0 and `leadTicks` 0 is the creature this engine shipped before today, which
  // is how `ai-pursue` carries its own repro arm instead of needing a second
  // binary to compare against.
  brain.heldGround =
      tick < brain.disengageUntil &&
      brain.targetRadial > self.speed * std::max(0.0f, pr.attack.holdGroundFrac);
  const bool disengaging = tick < brain.disengageUntil && !brain.heldGround;
  if (disengaging && hi > 0) lo = hi;
  const float d = brain.targetDist;
  const float slack = std::max(0.25f, pr.movement.bandSlack);

  // THE DEADBAND IS A SCHMITT TRIGGER, and it has to be: a band whose entry and
  // exit edges are the same number is a FIXPOINT, not a band. Measured — the
  // first version tested `d > rangeMax` both ways, so a duelist closing on an
  // 11-voxel ceiling asymptoted onto 11.000 and stayed "very slightly too far"
  // for 241 consecutive ticks: never content, so never circling, and parked
  // one voxel outside a 10-voxel weapon reach, so never attacking either. The
  // CA liquid levelling note in CLAUDE.md's memory is the same shape of bug.
  //
  // So: while CORRECTING, aim for the INNER band (pull all the way to
  // hi - slack); once CONTENT, only give the position up on crossing the OUTER
  // edge. Which of the two applies is read off the incumbent intent, which is
  // the only piece of state that already means "am I currently fixing this".
  const bool correcting = brain.intent == Intent::Approach ||
                          brain.intent == Intent::HoldRange;
  float tlo = lo, thi = hi;
  if (correcting && hi > lo) {
    const float mid = (lo + hi) * 0.5f;
    tlo = std::min(lo + slack, mid);
    thi = std::max(hi - slack, mid);
  }
  float bandErr = 0;                       // + too far, - too close
  if (brain.hasTarget && hi > 0) {
    if (d > thi) bandErr = d - thi;
    else if (d < tlo) bandErr = d - tlo;
  } else if (brain.hasTarget) {
    bandErr = d;                           // no band authored: just close
  }

  // ARRIVAL CLAMP. Never ask for more speed than closes the remaining error in
  // one tick. Without this the band is a property of the creature's STRIDE
  // rather than of its profile: mina walks 60 voxels a second, which is two
  // voxels per tick, and a 4-voxel band authored for a sword fight would be
  // crossed and re-crossed forever by a mob that can only ever be on one side
  // of it. Expressed as a fraction of full speed so it composes with the
  // authored approach/retreat factors instead of replacing them.
  const float perTick = std::max(1e-3f, self.speed * std::max(dt, 1e-4f));
  auto arrive = [&](float remaining) {
    return std::clamp(std::abs(remaining) / perTick, 0.0f, 1.0f);
  };

  // ---- STRIKE WHILE CLOSING (2026-09-16) ----------------------------------
  //
  // Two numbers come out of this block, and every remaining decision in Think
  // consumes one of them: how hard to keep walking forward WHILE a swing is
  // committed (`pursue`), and how far away the target will be when the edge
  // actually arrives (`brain.leadDist`). See AttackTuning::pursueSpeed for the
  // measured failure both of them answer.
  const float pursueCap =
      pr.movement.mobile
          ? std::clamp(pr.attack.pursueSpeed * spd, 0.0f, std::max(1.0f, spd))
          : 0.0f;
  float pursue = 0;
  if (pursueCap > 0.0f && brain.hasTarget && hi > 0) {
    // Aim to stand a slack INSIDE the far edge of what this body can strike
    // from rather than exactly on it. A blow taken at the last reachable
    // millivoxel is one the victim's next step defeats, and `hi` has already
    // been slid onto the weapon above — so this is "comfortably in range for
    // whatever is actually in my hand", not a re-guess of the band.
    const float goal = std::max(0.0f, hi - slack);
    if (d > goal) pursue = std::min(pursueCap, arrive(d - goal));
    // ...AND NEVER LET THE GAP OPEN WHILE THE BLADE IS OUT. Inside `goal`
    // there is no distance to close, but a target walking away is still walking
    // away — matching its radial speed is the difference between a swing that
    // arrives and one that is a body-length behind. Not applied when we are
    // inside the band FLOOR: down there HoldRange is deliberately backing off,
    // and overriding its retreat would make the creature chase a target that is
    // already too close to hit.
    if (brain.targetRadial > 0.0f && d > lo)
      pursue = std::max(
          pursue, std::min(pursueCap, brain.targetRadial /
                                          std::max(1e-3f, self.speed)));
  }

  // WHERE IT WILL BE, NOT WHERE IT IS.
  //
  // THE ALIGNMENT DISCOUNT IS APPLIED TO THE PREDICTION AND NOWHERE ELSE.
  // `DriveLocomotion` scales forward speed by how well the nose is on the
  // heading (`fwdDrive = base * driveScale * align`), so a creature forty
  // degrees off does not cover what a straight-line estimate promises. Crediting
  // closure it cannot deliver is not a harmless optimism: `brain.leadDist` is
  // what `PickAttackStyle` draws against, so an over-prediction picks a SHORTER
  // style than the fight needs and the swing arrives out of its own reach —
  // precisely the wasted-cadence failure the distance-aware draw exists to
  // stop. Cosine is what `align` is.
  const float closeRate = pursue * self.speed * std::max(0.0f, std::cos(aimErr)) -
                          brain.targetRadial;
  const float leadTicks = (float)(pr.attack.leadTicks >= 0
                                      ? (uint32_t)pr.attack.leadTicks
                                      : pr.attack.commitTicks);
  const float leadT = leadTicks * std::max(dt, 1e-4f);
  // ---- THE PREDICTION MAY ONLY BRING A COMMIT FORWARD, NEVER PUSH IT BACK --
  //
  // Clamped to the PRESENT distance, and this is not a safety margin — it is
  // the whole difference between the lead being a feature and being a way to
  // make a creature passive. Measured through `ai-pursue`'s per-knob arms: with
  // the lead on and the pursuit off, `closeRate` is just `-targetRadial`, so a
  // fleeing target predicts FURTHER than it is, `attackReady` fails, and the
  // duelist issued ONE attack request in 360 ticks against the eight the
  // unmodified creature managed. It did less damage than the bug this change
  // was written to fix.
  //
  // The asymmetry is justified rather than convenient: refusing to swing
  // because the target MIGHT be out of reach in fifteen ticks spends a whole
  // cadence on a certainty of nothing, while swinging and missing costs the
  // same cadence and sometimes lands. And the refusal is not this layer's to
  // make anyway — `BeginStroke` checks the drawn style's own reach at the
  // instant the stroke would start, which is later and better informed.
  //
  // The AIM POINT below is deliberately NOT clamped: where to put the edge is a
  // different question from whether to throw it, and there the extrapolation is
  // right in both directions.
  brain.leadDist = std::clamp(d - closeRate * leadT, 0.0f, d);

  // ---- the attack clock ---------------------------------------------------
  // Cadence plus a per-attack hash-RNG jitter. The jitter is the difference
  // between a duelist and a metronome, and it is drawn from (id, tick) so a
  // replay of the same fight produces the same rhythm.
  // THE REACH IS THE CALLER'S WHEN THE CALLER HAS ONE (ai_behavior.h
  // SelfView::attackReach): the profile states the creature's ordinary reach
  // and the stroke system widens it to whatever the longest USABLE style can
  // close. Which style actually fires, and whether the target is inside ITS
  // reach, is decided over there — this layer only decides "near enough to
  // try", and trying costs the attack clock either way.
  const float reach =
      self.attackReach > 0.0f ? self.attackReach : pr.attack.reach;
  //
  // ...AND IT IS TESTED AGAINST THE PREDICTED RANGE, NOT THE PRESENT ONE. The
  // question a commit answers is "will this land", and a blow decided now lands
  // `leadTicks` from now. Both directions matter and they are not symmetric: a
  // target closing with us (or one we are closing on, which is what `pursue`
  // above guarantees we will be doing) is committed to EARLIER, which is the
  // whole of "move forward and hit at the same time"; a target outrunning us is
  // committed to later or not at all, which saves the cadence for a turn the
  // creature can actually win instead of spending it on a swing `BeginStroke`
  // will drop.
  //
  // ...AND THE CADENCE IS READ LIVE (Tune::Cadence). The interval drawn at the
  // last swing is re-scaled every tick by whatever rules hold NOW, which is
  // what lets "it just whiffed: hit it" (a punish rule on targetRecovering)
  // bring a swing forward that was booked before the whiff. At a scale of
  // exactly 1 the booked tick is used untouched.
  uint32_t readyAt = brain.nextAttackTick;
  if (brain.attacksIssued > 0 && tn[(int)Tune::Cadence] != 1.0f)
    readyAt = brain.lastAttackTick +
              (uint32_t)std::lround((float)brain.attackInterval *
                                    std::max(0.0f, tn[(int)Tune::Cadence]));
  // A booked riposte / feint follow-up overrides the cadence either way.
  const bool riposteNow = brain.riposteArmed && tick >= brain.riposteAt;
  const bool attackReady =
      brain.hasTarget && brain.visible &&
      (tick >= readyAt || riposteNow) &&
      !disengaging && brain.leadDist <= reach &&
      aimErr <= pr.attack.aimTolerance * tn[(int)Tune::Aim];

  // ---- IS A BLOW COMING AT ME, AND HAVE I READ IT? ------------------------
  // "In range of it" is its reach (or a generous guess when unknown) plus
  // the defence margin; see Defense::margin for why that is not zero.
  const float threatReach =
      (brain.targetReach > 0.0f ? brain.targetReach : 8.0f) +
      self.size.z * 0.5f + std::max(0.0f, pr.defense.margin);
  const bool blowWindow =
      brain.blowLive ||
      (brain.blowEndTick > 0 && tick < brain.blowEndTick + pr.defense.holdTicks);
  const bool responding = brain.hasTarget && blowWindow && brain.reacts &&
                          tick >= brain.reactAt && d <= threatReach;
  const bool inTheirReach =
      brain.facts[(int)Fact::InTheirReach] > 0.0f;

  // ---- score every enabled intent ----------------------------------------
  float raw[(int)Intent::Count] = {};
  raw[(int)Intent::Idle] = 0.05f;
  if (brain.hasTarget) {
    // Face: wants the nose on the target, and wants it more the further off it
    // is. Also the intent the commit window pins the mob to.
    raw[(int)Intent::FaceTarget] = std::clamp(aimErr / 0.7f, 0.08f, 1.0f);

    if (pr.movement.mobile) {
      // Approach: the CLOSING verb, and the only one that plans. Scores on how
      // far past the band the target is, so a distant enemy dominates footwork
      // and a nearly-in-range one does not.
      if (bandErr > slack * 2.0f) {
        const float scale = std::max(4.0f, hi > 0 ? hi : 8.0f);
        raw[(int)Intent::Approach] = std::clamp(bandErr / scale, 0.3f, 1.0f);
      }
      // HoldRange: the MAINTAINING verb. Fine corrections in either direction,
      // without a plan — inside the band there is nothing to route around.
      if (bandErr < 0)
        raw[(int)Intent::HoldRange] =
            std::clamp(-bandErr / slack, 0.5f, 1.0f);
      else if (bandErr > 0)
        raw[(int)Intent::HoldRange] =
            bandErr <= slack * 2.0f ? 0.6f : 0.18f;
      // Circle: only when content. A mob that circles while out of position is
      // a mob that never closes.
      if (bandErr == 0.0f && brain.visible)
        raw[(int)Intent::CircleStrafe] = std::clamp(
            pr.movement.circleTendency * tn[(int)Tune::Circle], 0.0f, 1.0f);
    }
  }
  if (attackReady) raw[(int)Intent::RequestAttack] = 1.0f;
  // THE DAY'S ACTIVITY (P7): exactly the verb the schedule row names, at a
  // flat 1, while the resident layer has a routine on this brain. Everything
  // about WHEN it loses -- to a fight, to a fright -- is the profile's
  // weights and rules, like every other verb.
  // ---- THE PROFILE'S OWN IDLE WANDER (Movement::wanderRadius) -------------
  // A routine written HERE when no resident layer has one on this brain: a
  // leg to a point round the anchor, then a pause. A target takes it back at
  // once (the routine goes inactive, so `wander` scores 0 and the fight's
  // verbs win on their own scores); losing the target hands it back on the
  // next tick. Counter-based on (id, leg), so a replay walks the same legs.
  if (pr.movement.wanderRadius > 0.0f && pr.movement.mobile &&
      (!brain.routine.active || brain.wanderOwned)) {
    Routine& r = brain.routine;
    if (brain.hasTarget) {
      if (brain.wanderOwned) {
        r.active = false;
        brain.wanderOwned = false;
      }
      // The current leg is abandoned: on the way back to idling the creature
      // first rests where the fight left it.
      brain.wanderPauseUntil = 0;
      brain.wanderLegSince = 0;
    } else {
      const Vec3 foot = self.Foot();
      const Movement& mv = pr.movement;
      if (!brain.wanderAnchorSet) {
        brain.wanderAnchorSet = true;
        brain.wanderAnchor = foot;
        brain.wanderLegSince = 0;
      }
      auto pauseDraw = [&]() {
        const uint32_t span = mv.wanderPauseMax - mv.wanderPauseMin + 1u;
        return mv.wanderPauseMin +
               rng::Hash3((uint32_t)self.id ^ 0x3A11D3u, brain.wanderLeg, 7u) %
                   std::max(1u, span);
      };
      auto newGoal = [&]() {
        brain.wanderLeg++;
        const uint32_t h =
            rng::Hash3((uint32_t)self.id ^ 0x3A11D3u, brain.wanderLeg, 3u);
        const float ang = (float)(h & 0xFFFFu) * (6.2831853f / 65536.0f);
        const float u = (float)((h >> 16) & 0xFFFFu) / 65535.0f;
        // sqrt: uniform over the disc's AREA; floored so a leg is a walk.
        const float d = mv.wanderRadius * std::max(0.35f, std::sqrt(u));
        brain.wanderGoal = Vec3{brain.wanderAnchor.x + std::cos(ang) * d,
                                foot.y,
                                brain.wanderAnchor.z + std::sin(ang) * d};
        brain.wanderLegSince = tick;
      };
      // Rest, then walk the next leg; the leg's clock starts when the rest
      // ends, so the stuck guard below never counts standing still.
      auto restThenWalk = [&]() {
        newGoal();
        brain.wanderPauseUntil = tick + pauseDraw();
        brain.wanderLegSince = brain.wanderPauseUntil;
      };
      // First idle tick, or the first after a fight.
      if (brain.wanderLegSince == 0) restThenWalk();
      const float dist = PlanarDist(foot, brain.wanderGoal);
      // A leg that has taken twice its walking time is not getting there (a
      // wall, a drop, a crowd): give it up like an arrival.
      const float legSpeed =
          std::max(0.5f, self.speed * std::max(0.05f, mv.wanderSpeed));
      const uint32_t legBudget =
          (uint32_t)(2.0f * 30.0f * (2.0f * mv.wanderRadius) / legSpeed) + 60u;
      const bool resting = tick < brain.wanderPauseUntil;
      if (!resting && (dist <= mv.wanderArrive ||
                       tick - brain.wanderLegSince > legBudget))
        restThenWalk();
      r.active = true;
      r.verb = Intent::Wander;
      r.goal = brain.wanderGoal;
      r.final = true;
      r.arriveRadius = mv.wanderArrive;
      r.speed = mv.wanderSpeed;
      r.hold = tick < brain.wanderPauseUntil;
      r.faceHeadingSet = false;
      r.facePointSet = false;
      brain.wanderOwned = true;
    }
  }
  brain.routine.arrived = false;
  if (brain.routine.active && (int)brain.routine.verb >= (int)Intent::Sleep &&
      (int)brain.routine.verb <= (int)Intent::Goto)
    raw[(int)brain.routine.verb] = 1.0f;
  // Flee: always available to a body that can move. What decides WHEN is the
  // weight, which is 0 unless the profile or a holding rule says otherwise --
  // so a creature runs exactly when its JSON says it does, and never else.
  if (pr.movement.mobile) raw[(int)Intent::Flee] = 1.0f;
  // DEFENCE. Full score only for a blow this creature has read; the guard
  // STANCE (Defense::guardStance) is the weaker standing offer inside an
  // armed target's reach, for a creature not about to swing itself. Which
  // of guard / dodge wins is the weights' call — character, not code.
  if (responding) {
    if (self.canGuard) raw[(int)Intent::Guard] = 1.0f;
    if (pr.movement.mobile) raw[(int)Intent::Dodge] = 1.0f;
  } else if (self.canGuard && pr.defense.guardStance > 0.0f &&
             brain.targetArmed && inTheirReach && !attackReady &&
             brain.visible) {
    raw[(int)Intent::Guard] = std::clamp(pr.defense.guardStance, 0.0f, 1.0f);
  }

  // ---- arbitrate ----------------------------------------------------------
  // Weight, then the three dampers (see the header). The incumbent's bonus is
  // applied to the WEIGHTED score so it is expressed in the same units the
  // author tuned, and the dwell/cooldown gates are checked before, not after,
  // so a locked-out intent cannot win and then be vetoed (which would leave the
  // arbiter with no answer at all).
  Intent winner = Intent::Idle;
  float bestScore = -1.0f;
  const uint32_t dwell = tick - brain.intentSince;
  const IntentTuning& curT = pr.intents[(int)brain.intent];
  float weight[(int)Intent::Count];
  for (int i = 0; i < (int)Intent::Count; i++)
    weight[i] = EffectiveWeight(brain, pr, i);
  // A rule that zeroes the incumbent releases it at once: "stop attacking when
  // burning" must not wait out a dwell the author wrote for calmer moments.
  const bool locked = raw[(int)brain.intent] > 0.0f &&
                      weight[(int)brain.intent] > 0.0f &&
                      dwell < curT.minDwellTicks;

  for (int i = 0; i < (int)Intent::Count; i++) brain.score[i] = 0;
  for (int i = 0; i < (int)Intent::Count; i++) {
    if (weight[i] <= 0.0f) continue;
    if ((Intent)i != brain.intent && tick < brain.cooldownUntil[i]) continue;
    float s = raw[i] * weight[i];
    if (s <= 0.0f) continue;
    if ((Intent)i == brain.intent) s += pr.hysteresis;
    brain.score[i] = s;
    if (s > bestScore) {
      bestScore = s;
      winner = (Intent)i;
    }
  }

  // ...EXCEPT FOR WHAT CANNOT WAIT. A dwell is there to stop footwork
  // twitching; it must not hold a creature mid-circle while a blow it has read
  // lands on it, or make it sit out the one riposte window a defence earned.
  // Only the defence verbs against a READ blow and a booked riposte get
  // through — so a profile that uses neither is arbitrated exactly as before.
  const bool urgent =
      (responding && (winner == Intent::Guard || winner == Intent::Dodge)) ||
      (riposteNow && winner == Intent::RequestAttack);
  if (locked && !urgent) winner = brain.intent;

  // THE COMMIT WINDOW OVERRIDES EVERYTHING. Once an attack request has gone out
  // the body has promised the stroke system it will hold its facing, so no
  // amount of utility may spin it mid-swing.
  if (tick < brain.commitUntil) winner = Intent::FaceTarget;

  if (winner != brain.intent) {
    // Leaving an intent starts its cooldown. Charged on EXIT rather than on
    // entry so a long circle is not punished by its own duration.
    brain.cooldownUntil[(int)brain.intent] =
        tick + pr.intents[(int)brain.intent].cooldownTicks;
    // ---- A DEFENCE THAT SAW A BLOW THROUGH EARNS A RIPOSTE ----------------
    // Only when the blow it answered is actually over: dropping a guard to
    // swing into a cut still in the air is not a riposte, it is a trade.
    const bool wasDefending =
        brain.intent == Intent::Guard || brain.intent == Intent::Dodge;
    if (wasDefending && pr.attack.riposteTicks >= 0 && !brain.blowLive &&
        brain.defendedOnset == brain.blowOnset) {
      brain.riposteArmed = true;
      brain.riposteAt = tick + (uint32_t)pr.attack.riposteTicks;
      brain.defendedOnset = ~0u;   // one blow, one riposte
    }
    if (winner == Intent::Guard) brain.guards++;
    if (winner == Intent::Dodge) brain.dodges++;
    brain.dodgeSign = 0;
    brain.intent = winner;
    brain.intentSince = tick;
  }
  // Which blow the defence is answering: any tick of guard or dodge spent
  // while a READ blow is in the window (a guard stance that was already up
  // when the blow came counts — it met it).
  if ((brain.intent == Intent::Guard || brain.intent == Intent::Dodge) &&
      responding)
    brain.defendedOnset = brain.blowOnset;

  // ---- actuate ------------------------------------------------------------
  // The ONLY thing any branch below may produce is a desired heading and a
  // local drive vector. Nothing here can move a body or a facing directly.
  const float sp = pr.movement.mobile ? 1.0f : 0.0f;

  switch (brain.intent) {
    case Intent::Idle:
      // Deliberately writes the CURRENT heading rather than leaving the field
      // stale: Steer closes desired-minus-actual, so an idle mob whose desired
      // heading is a leftover from three seconds ago keeps turning toward it.
      out.desiredHeading = self.heading;
      break;

    case Intent::FaceTarget:
      out.desiredHeading = bearing;
      break;

    case Intent::Approach: {
      UpdatePath(brain, pr, self, view, tick, brain.targetPos);
      const Vec3 wp = SteerPoint(brain, brain.targetPos);
      const float want = BearingTo(self.Foot(), wp);
      // THE FAN STAYS, EVEN ON A PLANNED ROUTE — and this is the one place the
      // two-avoidance-layers objection has to lose, because the drive's veto is
      // not advice. `DriveLocomotion` refuses forward motion outright while
      // probe 0 is blocked, and that probe reaches half the creature's own
      // footprint plus two voxels — roughly eleven for a humanoid. A path that
      // hugs an obstacle therefore aims the nose at something eleven voxels
      // ahead, and the mob stands still forever with a perfectly good route in
      // hand. Measured exactly that: frozen at one position for 600 ticks,
      // waypoint 3.4 voxels away, never advancing the cursor.
      //
      // So the fan's job here is not "decide where to go" — the planner decided
      // — it is "find the nearest heading to that decision the drive will
      // actually honour". Deflect scores by angular distance from what was
      // WANTED rather than from the nose, which is what makes it a projection
      // onto the walkable set instead of a competing opinion. The dithering
      // this used to cause was never the fan: it was the path being thrown away
      // every ten ticks (see UpdatePath).
      out.desiredHeading = Deflect(want, self.heading, ground);
      out.driveScale = sp * pr.movement.approachSpeed * spd;
      // Only ease off when walking straight AT the target — a waypoint is not
      // the goal, and slowing into every corner of a path is a shuffle.
      if (brain.path.Done())
        out.driveScale = std::min(out.driveScale, sp * arrive(bandErr));
      break;
    }

    case Intent::HoldRange: {
      out.desiredHeading = bearing;   // never turn your back to give ground
      if (bandErr < 0) {
        out.driveScale =
            -sp * std::min(pr.movement.retreatSpeed * spd, arrive(bandErr));
      } else if (bandErr > 0) {
        // Small closing correction. Deliberately NOT routed through the
        // navigator: inside a couple of voxels of the band there is nothing to
        // route around, and replanning here would cost a search per tick for a
        // step the mob takes anyway.
        out.driveScale = sp * std::min(pr.movement.approachSpeed * spd * 0.55f,
                                       arrive(bandErr));
        const int p = ProbeFor(bearing, self.heading);
        if (ground.haveGround && !ground.clear[p]) out.driveScale = 0;
      }
      break;
    }

    case Intent::CircleStrafe: {
      if (brain.circleSign == 0 || tick >= brain.circleUntil) {
        // Redraw the orbit direction on a cadence from the counter-based RNG,
        // so two duelists on the same profile do not orbit in lockstep and one
        // duelist does not orbit forever in the same direction.
        const uint32_t h = rng::Hash3((uint32_t)self.id ^ kSaltCircle, tick, 0);
        brain.circleSign = (h & 1u) ? 1 : -1;
        // ---- FLANKING (Movement::flank) --------------------------------
        // Orbit AWAY from the ally nearest us around the same target, so a
        // pack opens out around its quarry instead of lining up in front of
        // it. Angles are positions around the TARGET (bearing target->body);
        // strafing to our own right, while facing it, DEcreases ours.
        // Drawn with probability `flank`, so a pack still shuffles.
        if (pr.movement.flank > 0.0f && brain.hasTarget &&
            view.actors != nullptr &&
            (float)((h >> 8) & 0xFFFFu) * (1.0f / 65536.0f) <
                pr.movement.flank) {
          const float mine = BearingTo(brain.targetPos, centre);
          float bestAbs = 1e9f, bestDelta = 0.0f;
          for (const Actor& a : *view.actors) {
            if (!a.alive || a.id == self.id || a.faction != self.faction ||
                a.targetId != brain.targetId)
              continue;
            const float dl = WrapPi(BearingTo(brain.targetPos, a.centre) - mine);
            if (std::abs(dl) < bestAbs) {
              bestAbs = std::abs(dl);
              bestDelta = dl;
            }
          }
          if (bestAbs < 1e8f) brain.circleSign = bestDelta > 0.0f ? 1 : -1;
        }
        brain.circleUntil = tick + std::max(6u, pr.movement.circleHoldTicks);
      }
      out.desiredHeading = bearing;
      // TANGENTIAL SPEED IS BOUNDED BY THE NECK, not by the legs. See
      // SelfView::turnRate: v/r is the angular rate an orbit demands, and a
      // body that cannot turn that fast orbits with its target off to one side
      // for the whole circle. 0.7 of the cap, not 1.0, because Steer has to
      // spend some of its rate ARRIVING at the bearing rather than only
      // tracking it, and because the target moves too.
      const float orbitR = std::max(1.0f, d);
      const float maxTangential = self.turnRate * 0.7f * orbitR;
      const float strafeCap = std::min(pr.movement.strafeSpeed * spd,
                                       maxTangential / std::max(1.0f, self.speed));
      out.driveStrafe = sp * strafeCap * (float)brain.circleSign;
      // Hold the radius while orbiting: a pure tangential step walks a polygon
      // outward, and the band would be lost within a couple of seconds.
      const float mid = (lo + hi) * 0.5f;
      if (hi > 0) {
        const float cap = std::min(0.35f, arrive(d - mid));
        out.driveScale = std::clamp((d - mid) * 0.12f, -cap, cap) * sp;
      }
      // Refuse to strafe into a wall we can already feel; the fan index for
      // +-90 degrees in the mob's own frame is 2 and 6.
      const int sp2 = out.driveStrafe > 0 ? 2 : 6;
      if (ground.haveGround && !ground.clear[sp2]) {
        out.driveStrafe = 0;
        brain.circleSign = -brain.circleSign;
        brain.circleUntil = tick + std::max(6u, pr.movement.circleHoldTicks);
      }
      break;
    }

    case Intent::RequestAttack: {
      out.desiredHeading = bearing;
      // Fire ONCE per cadence. The clock is advanced here rather than when the
      // stroke lands, because this layer does not know whether a stroke landed
      // and must not learn: that is Phase C's business, and an AI that waits
      // for a hit confirmation stops swinging the moment the seam is stubbed.
      const uint32_t jit =
          pr.attack.jitterTicks == 0
              ? 0
              : rng::Hash3((uint32_t)self.id ^ kSaltAttack, tick, 1) %
                    pr.attack.jitterTicks;
      brain.nextAttackTick = tick + pr.attack.cadenceTicks + jit;
      brain.attackInterval = pr.attack.cadenceTicks + jit;
      brain.commitUntil = tick + pr.attack.commitTicks;
      brain.disengageUntil =
          brain.commitUntil +
          (uint32_t)std::lround((float)pr.attack.disengageTicks *
                                std::max(0.0f, tn[(int)Tune::Disengage]));
      // A swing the cadence would not yet have allowed is a riposte (or a
      // feint's real blow): counted, and the booking is spent either way.
      if (brain.riposteArmed && riposteNow && tick < readyAt) brain.ripostes++;
      brain.riposteArmed = false;
      // ---- IS THIS ONE A BLUFF? (AttackTuning::feintChance) ---------------
      // Drawn per swing, keyed on the swing's own tick. The stroke is started
      // exactly as a real one; only the pull-out is booked here.
      {
        const float fc = std::clamp(
            pr.attack.feintChance * tn[(int)Tune::Feint], 0.0f, 1.0f);
        brain.feinting = false;
        if (fc > 0.0f) {
          const uint32_t hf =
              rng::Hash3((uint32_t)self.id ^ kSaltFeint, tick, 3);
          if ((float)(hf & 0xFFFFu) * (1.0f / 65536.0f) < fc) {
            brain.feinting = true;
            brain.feintAt = tick + std::max(1u, pr.attack.feintTicks);
          }
        }
      }
      brain.lastAttackTick = tick;
      brain.attacksIssued++;
      out.attack = true;
      out.request.mobId = self.id;
      out.request.targetId = brain.targetId;
      // The chosen style is resolved by the stroke system, not here: this
      // layer hands over the whole authored LIST's first entry as a label and
      // lets PickAttackStyle draw. Carrying a name at all is for the debug
      // readout and for a future scripted attack that wants to name one.
      out.request.style =
          pr.attack.styles.empty() ? std::string() : pr.attack.styles[0];
      // ---- THE BLOW IS AIMED AHEAD OF THE TARGET (AttackTuning::leadTicks) --
      //
      // This point is FROZEN by the stroke: `StartStroke` copies it and
      // `StepStroke` re-derives the bearing from the stored copy every tick,
      // never from the live victim (mob.cpp, "THE AIM"). That is right and
      // deliberate — a committed cut must not home — but it makes this the ONE
      // instant at which the blow's destination is chosen, and choosing the
      // target's present position at that instant aims fifteen ticks behind
      // anything that is moving.
      //
      // BOUNDED BY HALF A STRIKE REACH. A target that covers more than that
      // inside the window is not one this swing was ever going to meet, and an
      // unbounded extrapolation would aim the cut into open ground well past
      // the fight — which reads as the creature deliberately swinging at
      // nothing, and is worse than being a step behind.
      Vec3 aimPoint = brain.targetPos;
      {
        float lx = brain.targetVel.x * leadT, lz = brain.targetVel.z * leadT;
        const float span = self.strikeReach > 0.0f ? self.strikeReach : reach;
        const float budget = std::max(1.0f, span * 0.5f);
        const float m = std::sqrt(lx * lx + lz * lz);
        if (m > budget) {
          lx *= budget / m;
          lz *= budget / m;
        }
        aimPoint.x += lx;
        aimPoint.z += lz;
      }
      out.request.targetPoint = aimPoint;
      out.request.tick = tick;
      out.request.commitTicks = pr.attack.commitTicks;
      // THE PREDICTED RANGE, for the same reason `attackReady` used it: this is
      // the number `PickAttackStyle` filters on and `BeginStroke` refuses
      // against, and both of them are deciding what can reach at IMPACT.
      out.request.distance = brain.leadDist;
      break;
    }

    case Intent::Flee: {
      // Away from the threat; with none (burning, nothing seen) straight on
      // along the current heading -- a panic run, not a search. Deflect keeps
      // it off walls the same way Approach's route is kept off them.
      const float away = brain.hasThreat
                             ? BearingTo(brain.threatPos, centre)
                             : self.heading;
      out.desiredHeading = Deflect(away, self.heading, ground);
      out.driveScale = sp * std::max(0.0f, pr.movement.fleeSpeed) * spd;
      break;
    }

    case Intent::Guard: {
      // ---- BLADE ACROSS THE LINE --------------------------------------------
      // Face it and put the weapon where ITS weapon is: the point it is
      // swinging is the thing that has to be met, and a guard held on the
      // live point tracks the cut as it comes round. With no point known,
      // the attacker's upper chest — where a blow starts from. The PARRY
      // itself is not decided here: MeleeSweepDamage's blade-on-blade test
      // decides whether this guard was in the way.
      out.desiredHeading = bearing;
      out.guard = true;
      out.guardReach = pr.defense.guardReach;
      if (tgtActor != nullptr && tgtActor->haveTip) {
        out.guardPoint = tgtActor->tip;
      } else {
        out.guardPoint = brain.targetPos;
        out.guardPoint.y += (tgtActor != nullptr ? tgtActor->height : self.size.y) * 0.25f;
      }
      // Give a little ground while covering, inside its reach: a guard that
      // walks into the blade is a guard that takes the tip anyway.
      if (inTheirReach) {
        out.driveScale = -sp * std::max(0.0f, pr.movement.retreatSpeed) * spd * 0.35f;
        if (ground.haveGround && !ground.clear[4]) out.driveScale = 0;
      }
      break;
    }

    case Intent::Dodge: {
      // ---- OUT OF THE ARC ---------------------------------------------------
      // Back and to one side, facing it throughout (a creature that turns its
      // back to dodge is fleeing). The side is chosen ONCE per dodge: away
      // from the blade when its point is known, else a coin flip.
      out.desiredHeading = bearing;
      if (brain.dodgeSign == 0) {
        int sign = (rng::Hash3((uint32_t)self.id ^ kSaltDodge, tick, 4) & 1u) ? 1 : -1;
        if (tgtActor != nullptr && tgtActor->haveTip) {
          // Drive right is (cos h, 0, -sin h) (MobSystem::DriveLocomotion).
          const float rx = std::cos(self.heading), rz = -std::sin(self.heading);
          const float side = (tgtActor->tip.x - centre.x) * rx +
                             (tgtActor->tip.z - centre.z) * rz;
          if (std::abs(side) > 0.5f) sign = side > 0.0f ? -1 : 1;
        }
        brain.dodgeSign = sign;
      }
      const float burst = sp * std::max(0.0f, pr.movement.dodgeSpeed) * spd;
      float back = burst * 0.8f, lat = burst * 0.6f * (float)brain.dodgeSign;
      // The fan: 4 is dead astern, 2 / 6 are the right / left flanks.
      if (ground.haveGround) {
        if (!ground.clear[4]) {
          back = 0.0f;
          lat *= 1.4f;
        }
        const int sideProbe = lat > 0.0f ? 2 : 6;
        if (lat != 0.0f && !ground.clear[sideProbe]) {
          const int other = sideProbe == 2 ? 6 : 2;
          if (ground.clear[other]) {
            lat = -lat;
            brain.dodgeSign = -brain.dodgeSign;
          } else {
            lat = 0.0f;
          }
        }
      }
      out.driveScale = -back;
      out.driveStrafe = lat;
      break;
    }

    case Intent::Sleep:
    case Intent::Work:
    case Intent::Wander:
    case Intent::Socialize:
    case Intent::Eat:
    case Intent::Goto: {
      // ---- ONE ACTUATOR FOR THE WHOLE DAY (P7) ------------------------------
      // Walk to the routine's goal over the same local navigator Approach
      // uses (a straight line when it is clear, A* when it is not), and on
      // arrival -- or while holding at a closed door or in a conversation --
      // stand and turn to what the routine asks. The resident layer moves
      // `goal` along the waynode route; this only ever walks ONE leg.
      Routine& r = brain.routine;
      const Vec3 foot = self.Foot();
      const float dist = PlanarDist(foot, r.goal);
      const bool there = r.final && dist <= r.arriveRadius;
      r.arrived = there;
      float faceTo = self.heading;
      if (r.facePointSet) faceTo = BearingTo(foot, r.facePoint);
      else if (r.faceHeadingSet) faceTo = r.faceHeading;
      if (r.hold || there) {
        brain.path.Clear();
        out.desiredHeading = faceTo;
        out.driveScale = 0;
        break;
      }
      UpdatePath(brain, pr, self, view, tick, r.goal);
      const Vec3 wp = SteerPoint(brain, r.goal);
      out.desiredHeading = Deflect(BearingTo(foot, wp), self.heading, ground);
      float drive = sp * std::max(0.0f, r.speed) * spd;
      // Ease into the anchor (not into every waypoint: slowing at each
      // corner of a route is a shuffle).
      if (r.final && brain.path.Done())
        drive = std::min(drive, sp * std::max(0.25f, arrive(dist - r.arriveRadius * 0.5f)));
      out.driveScale = drive;
      break;
    }

    default:
      break;
  }

  // ---- ...AND THE FEET KEEP MOVING THROUGH THE SWING ----------------------
  //
  // APPLIED AFTER THE ARBITER, AND THAT IS THE POINT. The commit window
  // overrides the winning intent with FaceTarget outright — "no amount of
  // utility may spin it mid-swing" — and FaceTarget writes a heading and no
  // drive at all. So the override that keeps a creature from pirouetting was
  // also the thing nailing its feet to the ground for the whole telegraph, and
  // no intent weight anywhere in behaviors.json could have reached it. A
  // profile author looking at this was choosing between a mob that swings
  // straight and a mob that walks.
  //
  // The two are not actually in tension: holding a FACING and holding a
  // POSITION are different promises, and the stroke system only ever needed the
  // first. Nothing downstream cares that the body translated — the aim is taken
  // about the shoulder in the mob's own basis and re-derived from the live
  // pivot every tick, so walking forward through a windup carries the blade
  // with it and lengthens the blow rather than breaking it.
  //
  // `max`, never a replacement. An intent that already wants to close harder
  // keeps its own number, and a HoldRange retreat (negative drive, too close)
  // is untouched because `pursue` is zero inside the band floor.
  const bool swinging =
      brain.intent == Intent::RequestAttack || tick < brain.commitUntil;
  brain.pursueDrive = 0;
  if (swinging && pursue > 0.0f && brain.hasTarget) {
    float fwd = pursue;
    // The drive's forward probe is not advice, it is a veto: `DriveLocomotion`
    // refuses forward motion outright while probe 0 is blocked. Ask the same fan
    // HoldRange asks rather than promising a step into a wall and reporting a
    // pursuit that never happened.
    const int p = ProbeFor(bearing, self.heading);
    if (ground.haveGround && !ground.clear[p]) fwd = 0;
    if (fwd > out.driveScale) {
      out.driveScale = fwd;
      brain.pursueDrive = fwd;
    }
  }

  // A profile that cannot move its feet cannot move them by any route. Enforced
  // here, once, rather than trusted to every branch above — a training dummy
  // that shuffles because one intent forgot to check is exactly the kind of bug
  // a data-driven system is supposed to make impossible.
  if (!pr.movement.mobile) {
    out.driveScale = 0;
    out.driveStrafe = 0;
  }
  (void)dt;
  return true;
}

}  // namespace ai
