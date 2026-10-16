// distant_combat.cpp -- the distant tier's abstract hit model (see the header,
// and docs/PLAN_distant_npcs.md §2.6). Everything below the kit builder is a
// pure function of its arguments.

#include "game/distant_combat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include <nlohmann/json.hpp>

#include "game/ai_behavior.h"
#include "game/arrows.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/shellresponse.h"
#include "game/strokes.h"
#include "sim/rng.h"

namespace distant {

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool Has(const std::string& s, const char* sub) {
  return s.find(sub) != std::string::npos;
}

const char* const kRoleNames[kRoles] = {"head", "torso", "arm", "leg",
                                        "tail", "fin",   "other"};
const char* const kGuardNames[kGuards] = {"none", "blade", "shield"};
const char* const kKindNames[kKinds] = {"cut", "blunt", "bite", "pierce"};

}  // namespace

// =============================================================================
// Vocabulary
// =============================================================================

const char* LimbRoleName(LimbRole r) {
  return (int)r < kRoles ? kRoleNames[(int)r] : "?";
}
LimbRole LimbRoleByName(const std::string& n) {
  for (int i = 0; i < kRoles; i++)
    if (n == kRoleNames[i]) return (LimbRole)i;
  return LimbRole::Count;
}

// THE TAG FIRST, because the tag is what the rig's other systems already ask
// (gait reads "leg"/"foot", the IK chains read "arm", the fall bill reads
// "leg"/"foot", a style's `target` table weights tags). The NAME is the
// fallback for a part authored without one, and the last resort is TORSO: a
// part that is not a head, a limb, a tail or a fin is trunk -- a snake's
// `body3`, a shark's `body2`, the naga's `tail1` (tagged spine, and rightly:
// it is the trunk the arms hang off).
LimbRole LimbRoleOf(const std::string& tag0, const std::string& name0,
                    bool bloodless) {
  if (bloodless) return LimbRole::Other;
  const std::string tag = Lower(tag0);
  if (tag == "head" || tag == "jaw" || tag == "skull" || tag == "face")
    return LimbRole::Head;
  if (tag == "spine" || tag == "torso" || tag == "chest" || tag == "hips" ||
      tag == "pelvis" || tag == "body" || tag == "neck" || tag == "abdomen")
    return LimbRole::Torso;
  if (tag == "arm" || tag == "hand" || tag == "claw" || tag == "wing")
    return LimbRole::Arm;
  if (tag == "leg" || tag == "foot") return LimbRole::Leg;
  if (tag == "tail") return LimbRole::Tail;
  if (tag == "fin") return LimbRole::Fin;
  if (tag == "item" || tag == "worn" || tag == "arrow" || tag == "hair" ||
      tag == "prop")
    return LimbRole::Other;
  const std::string n = Lower(name0);
  if (Has(n, "hair") || Has(n, "mane") || Has(n, "beard")) return LimbRole::Other;
  if (Has(n, "head") || Has(n, "jaw") || Has(n, "skull")) return LimbRole::Head;
  if (Has(n, "tail")) return LimbRole::Tail;
  if (Has(n, "fin")) return LimbRole::Fin;
  if (Has(n, "arm") || Has(n, "hand")) return LimbRole::Arm;
  if (Has(n, "leg") || Has(n, "foot")) return LimbRole::Leg;
  return LimbRole::Torso;
}

const char* GuardClassName(GuardClass g) {
  return (int)g < kGuards ? kGuardNames[(int)g] : "?";
}
const char* StrikeKindName(StrikeKind k) {
  return (int)k < kKinds ? kKindNames[(int)k] : "?";
}

// THE PART THAT MAKES THE WOUND. A sword carries a token blunt part and a set
// of jaws a small one (the head's mass behind the teeth); neither is what the
// blow IS. Ties go to the edge, then the teeth.
StrikeKind DominantKind(const StrikeProfile& s) {
  if (s.bite > 0.0f && s.bite >= s.cut && s.bite >= s.blunt) return StrikeKind::Bite;
  if (s.cut > 0.0f && s.cut >= s.blunt) return StrikeKind::Cut;
  return StrikeKind::Blunt;
}

DamageCause CauseOf(StrikeKind k, bool natural) {
  switch (k) {
    case StrikeKind::Cut: return DamageCause::Blade;
    case StrikeKind::Blunt: return natural ? DamageCause::Unarmed : DamageCause::Blunt;
    case StrikeKind::Bite: return DamageCause::Bite;
    case StrikeKind::Pierce: return DamageCause::Pierce;
    case StrikeKind::Count: break;
  }
  return DamageCause::Other;
}

const char* DistantDeathName(DistantDeath d) {
  switch (d) {
    case DistantDeath::None: return "alive";
    case DistantDeath::VitalLimb: return "vital limb destroyed";
    case DistantDeath::BloodLoss: return "blood loss";
    case DistantDeath::Burns: return "burns";
    case DistantDeath::Other: return "dead at capture";
  }
  return "?";
}

const char* StrikeResultName(StrikeResult r) {
  switch (r) {
    case StrikeResult::Hit: return "hit";
    case StrikeResult::Miss: return "miss";
    case StrikeResult::Blocked: return "blocked";
    case StrikeResult::OutOfReach: return "out of reach";
    case StrikeResult::NoTarget: return "no target";
  }
  return "?";
}

// =============================================================================
// The table
// =============================================================================

const StyleCalib& DistantCalib::Style(const std::string& name) const {
  for (const StyleCalib& s : styles)
    if (s.name == name) return s;
  return fallback;
}

const MatchupCalib* DistantCalib::Matchup(const std::string& name) const {
  for (const MatchupCalib& m : matchups)
    if (m.name == name) return &m;
  return nullptr;
}

namespace {
// The near stump's law, from the TUNING (copied at load like the burn cap, so
// the resolver is a pure function of the table): what a sever owes at once
// (gore.severStumpBudget voxels of blood) and what an open stump then drains
// a tick -- one bleed clump topped up per drip period, scaled by the volume
// dial and the stump's gain and saturating at a whole clump (Mob::BleedTick,
// "THE STUMP THAT NEVER CLOSES"), each voxel gore.bleedHpPerVoxel of life.
void StumpLawFromTuning(SeverCalib& s) {
  const Tuning::Gore& g = CurrentTuning().gore;
  const float clump = (float)BleedClumpVoxels(std::max(0, g.bleedClumpRadius));
  const float per = std::min(1.0f, std::max(0.0f, g.bleedVoxelGain) * std::max(0.0f, g.stumpBleedGain));
  s.owedHp = std::max(0.0f, g.severStumpBudget) * g.bleedHpPerVoxel;
  s.stumpHpPerTick = g.stumpBleedsOpen
                         ? clump * per * g.bleedHpPerVoxel / (float)std::max(1, g.bleedDripTicks)
                         : 0.0f;
}
}  // namespace

DistantCalib DistantCalibBuiltin() {
  DistantCalib c;
  StumpLawFromTuning(c.sever);
  c.about =
      "BUILT-IN FALLBACK (game/distant_combat.cpp DistantCalibBuiltin): "
      "assets/mobs/distant_combat.json was absent or unreadable. Round "
      "first-calibration magnitudes; regenerate the file with "
      "SANDVOX_DISTANT_CALIB=<out.json> --selftest --gate distant-calib-matrix.";
  c.fallback.name = "*";
  const Tuning::Gore& g = CurrentTuning().gore;
  c.burn.capMidFraction = g.burnCapMidFraction;
  c.burn.capMidHealth = g.burnCapMidHealth;
  c.burn.deathFraction = g.burnDeathFraction;
  return c;
}

namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

// A number at `key` in `o`, clamped to [lo, hi]; absent keeps `def`, garbage
// and out-of-band values say so in `log`.
float Num(const json& o, const char* key, float def, float lo, float hi,
          const std::string& where, std::string& log) {
  if (!o.is_object() || !o.contains(key)) return def;
  const json& v = o[key];
  if (!v.is_number()) {
    log += where + "." + key + ": not a number, kept " + std::to_string(def) + "\n";
    return def;
  }
  double x = v.get<double>();
  if (!std::isfinite(x)) {
    log += where + "." + key + ": not finite, kept " + std::to_string(def) + "\n";
    return def;
  }
  if (x < lo || x > hi) {
    log += where + "." + key + ": " + std::to_string(x) + " clamped to [" +
           std::to_string(lo) + ", " + std::to_string(hi) + "]\n";
    x = std::clamp(x, (double)lo, (double)hi);
  }
  return (float)x;
}
int Int(const json& o, const char* key, int def, int lo, int hi,
        const std::string& where, std::string& log) {
  return (int)std::lround(Num(o, key, (float)def, (float)lo, (float)hi, where, log));
}

// A role-weight object ({"head": 0.1, ...}) normalised to sum 1. All zero or
// absent keeps `w` as it was.
void Roles(const json& o, float* w, const std::string& where, std::string& log) {
  if (!o.is_object()) return;
  float tmp[kRoles] = {};
  float sum = 0.0f;
  for (auto it = o.begin(); it != o.end(); ++it) {
    const LimbRole r = LimbRoleByName(it.key());
    if (r == LimbRole::Count) {
      log += where + ": unknown role \"" + it.key() + "\" ignored\n";
      continue;
    }
    if (!it.value().is_number()) continue;
    const float x = std::max(0.0f, (float)it.value().get<double>());
    if (!std::isfinite(x)) continue;
    tmp[(int)r] = x;
    sum += x;
  }
  if (sum <= 1e-6f) {
    log += where + ": role weights sum to zero, kept the previous ones\n";
    return;
  }
  for (int i = 0; i < kRoles; i++) w[i] = tmp[i] / sum;
}

void ReadStyle(const json& o, StyleCalib& s, const std::string& where,
               std::string& log) {
  s.n = Int(o, "n", s.n, 0, 1 << 30, where, log);
  s.hits = Int(o, "hits", s.hits, 0, 1 << 30, where, log);
  s.blocked = Int(o, "blocked", s.blocked, 0, 1 << 30, where, log);
  s.hitRate = Num(o, "hitRate", s.hitRate, 0.0f, 1.0f, where, log);
  s.meanPower = Num(o, "meanPower", s.meanPower, 0.0f, 100.0f, where, log);
  s.powerCv = Num(o, "powerCv", s.powerCv, 0.0f, 1.0f, where, log);
  s.impactTicks = Num(o, "impactTicks", s.impactTicks, 0.0f, 600.0f, where, log);
  if (o.contains("roleWeights")) Roles(o["roleWeights"], s.roleWeights, where + ".roleWeights", log);
  if (o.contains("byWeapon") && o["byWeapon"].is_object()) {
    for (auto it = o["byWeapon"].begin(); it != o["byWeapon"].end(); ++it) {
      WeaponCalib w;
      w.weapon = it.key();
      const std::string wh = where + ".byWeapon." + it.key();
      w.n = Int(it.value(), "n", 0, 0, 1 << 30, wh, log);
      w.hits = Int(it.value(), "hits", 0, 0, 1 << 30, wh, log);
      w.blocked = Int(it.value(), "blocked", 0, 0, 1 << 30, wh, log);
      w.hitRate = Num(it.value(), "hitRate", s.hitRate, 0.0f, 1.0f, wh, log);
      w.meanPower = Num(it.value(), "meanPower", s.meanPower, 0.0f, 100.0f, wh, log);
      s.byWeapon.push_back(w);
    }
  }
}

double R4(double x) { return std::round(x * 1e4) / 1e4; }

ojson RolesJson(const float* w) {
  ojson o = ojson::object();
  for (int i = 0; i < kRoles; i++)
    if (w[i] > 0.0f) o[kRoleNames[i]] = R4(w[i]);
  return o;
}

ojson StyleJson(const StyleCalib& s) {
  ojson o = ojson::object();
  o["n"] = s.n;
  o["hits"] = s.hits;
  o["blocked"] = s.blocked;
  o["hitRate"] = R4(s.hitRate);
  o["meanPower"] = R4(s.meanPower);
  o["powerCv"] = R4(s.powerCv);
  o["impactTicks"] = R4(s.impactTicks);
  o["roleWeights"] = RolesJson(s.roleWeights);
  if (!s.byWeapon.empty()) {
    ojson w = ojson::object();
    for (const WeaponCalib& x : s.byWeapon) {
      ojson e = ojson::object();
      e["n"] = x.n;
      e["hits"] = x.hits;
      e["blocked"] = x.blocked;
      e["hitRate"] = R4(x.hitRate);
      e["meanPower"] = R4(x.meanPower);
      w[x.weapon] = e;
    }
    o["byWeapon"] = w;
  }
  return o;
}

}  // namespace

bool LoadDistantCombat(const std::string& path, DistantCalib& out,
                       std::string& log) {
  out = DistantCalibBuiltin();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log += "distant_combat: " + path + " not found, using the built-in table\n";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    log += "distant_combat: " + path + " does not parse, using the built-in table\n";
    return false;
  }
  DistantCalib c = DistantCalibBuiltin();
  c.source = path;
  c.version = Int(j, "version", 1, 1, 1000, "root", log);
  if (c.version != 1)
    log += "distant_combat: version " + std::to_string(c.version) +
           " is newer than this reader (1); reading what it knows\n";
  if (j.contains("_about") && j["_about"].is_string())
    c.about = j["_about"].get<std::string>();
  c.minSamples = Int(j, "minSamples", c.minSamples, 1, 100000, "root", log);
  c.reachSlack = Num(j, "reachSlack", c.reachSlack, 0.0f, 50.0f, "root", log);

  if (j.contains("styles") && j["styles"].is_object()) {
    for (auto it = j["styles"].begin(); it != j["styles"].end(); ++it) {
      if (!it.value().is_object()) {
        log += "styles." + it.key() + ": not an object, skipped\n";
        continue;
      }
      StyleCalib s;
      s.name = it.key();
      ReadStyle(it.value(), s, "styles." + it.key(), log);
      if (s.name == "*") c.fallback = s;
      else c.styles.push_back(s);
    }
  }
  c.fallback.name = "*";

  if (j.contains("block") && j["block"].is_object()) {
    const json& b = j["block"];
    for (int g = 0; g < kGuards; g++) {
      if (!b.contains(kGuardNames[g])) continue;
      const std::string wh = std::string("block.") + kGuardNames[g];
      c.blockChance[g] = Num(b[kGuardNames[g]], "chance", c.blockChance[g], 0.0f, 0.95f, wh, log);
      c.blockN[g] = Int(b[kGuardNames[g]], "n", 0, 0, 1 << 30, wh, log);
    }
  }
  if (j.contains("armour") && j["armour"].is_object()) {
    const json& a = j["armour"];
    for (int k = 0; k < kKinds; k++) {
      if (a.contains("exposure"))
        c.armour.exposure[k] = Num(a["exposure"], kKindNames[k], c.armour.exposure[k],
                                   0.0f, 1.0f, "armour.exposure", log);
      if (a.contains("n"))
        c.armour.n[k] = Int(a["n"], kKindNames[k], 0, 0, 1 << 30, "armour.n", log);
    }
  }
  if (j.contains("bleed") && j["bleed"].is_object()) {
    const json& b = j["bleed"];
    for (int k = 0; k < kKinds; k++)
      if (b.contains("perHp"))
        c.bleed.perHp[k] = Num(b["perHp"], kKindNames[k], c.bleed.perHp[k], 0.0f, 20.0f,
                               "bleed.perHp", log);
    c.bleed.decay = Num(b, "decay", c.bleed.decay, 0.0f, 0.9999f, "bleed", log);
    c.bleed.drainShare = Num(b, "drainShare", c.bleed.drainShare, 0.0f, 4.0f, "bleed", log);
  }
  if (j.contains("burn") && j["burn"].is_object()) {
    const json& b = j["burn"];
    c.burn.fracPerTick = Num(b, "fracPerTick", c.burn.fracPerTick, 0.0f, 1.0f, "burn", log);
    c.burn.hpPerTick = Num(b, "hpPerTick", c.burn.hpPerTick, 0.0f, 1000.0f, "burn", log);
    c.burn.meanTicks = Int(b, "meanTicks", c.burn.meanTicks, 0, 1 << 20, "burn", log);
    // The cap curve is TUNING's (gore.burn*), copied here at load so the
    // resolver is a pure function of the table; the file's copy is a record of
    // what the calibration ran under, not an override.
  }
  if (j.contains("ranged") && j["ranged"].is_object()) {
    const json& r = j["ranged"];
    c.ranged.arrowSpeed = Num(r, "arrowSpeed", c.ranged.arrowSpeed, 1.0f, 100000.0f, "ranged", log);
    c.ranged.speedRef = Num(r, "speedRef", c.ranged.speedRef, 0.1f, 10000.0f, "ranged", log);
    c.ranged.speedPenalty = Num(r, "speedPenalty", c.ranged.speedPenalty, 0.0f, 1.0f, "ranged", log);
    c.ranged.damageFrac = Num(r, "damageFrac", c.ranged.damageFrac, 0.0f, 100.0f, "ranged", log);
    c.ranged.n = Int(r, "n", 0, 0, 1 << 30, "ranged", log);
    c.ranged.hits = Int(r, "hits", 0, 0, 1 << 30, "ranged", log);
    if (r.contains("roleWeights")) Roles(r["roleWeights"], c.ranged.roleWeights, "ranged.roleWeights", log);
    if (r.contains("rateByDistance") && r["rateByDistance"].is_array()) {
      std::vector<float> d, p;
      for (const json& e : r["rateByDistance"]) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_number()) {
          log += "ranged.rateByDistance: an entry is not [distance, rate], skipped\n";
          continue;
        }
        const float dd = (float)e[0].get<double>(), pp = (float)e[1].get<double>();
        if (!std::isfinite(dd) || !std::isfinite(pp) || (!d.empty() && dd <= d.back())) {
          log += "ranged.rateByDistance: knots must be finite and increasing, one skipped\n";
          continue;
        }
        d.push_back(dd);
        p.push_back(std::clamp(pp, 0.0f, 1.0f));
      }
      if (!d.empty()) {
        c.ranged.distKnots = d;
        c.ranged.rateKnots = p;
      }
    }
  }
  // D2b: the hit-size inverse CDF per role, and dismemberment.
  if (j.contains("spread") && j["spread"].is_object()) {
    for (auto it = j["spread"].begin(); it != j["spread"].end(); ++it) {
      const LimbRole r = LimbRoleByName(it.key());
      if (r == LimbRole::Count || !it.value().is_array()) {
        log += "spread." + it.key() + ": not a role with a knot list, skipped\n";
        continue;
      }
      HitSpread hs;
      bool ok = true;
      for (const json& e : it.value()) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number() || !e[1].is_number()) {
          ok = false;
          break;
        }
        const float pp = (float)e[0].get<double>(), xx = (float)e[1].get<double>();
        if (!std::isfinite(pp) || !std::isfinite(xx) || xx < 0.0f || pp < 0.0f || pp > 1.0f ||
            (!hs.p.empty() && (pp <= hs.p.back() || xx < hs.x.back()))) {
          ok = false;
          break;
        }
        hs.p.push_back(pp);
        hs.x.push_back(xx);
      }
      if (!ok || hs.p.size() < 2 || hs.p.front() != 0.0f || hs.p.back() != 1.0f) {
        log += "spread." + it.key() + ": knots must be [p, x] pairs, p from 0 to 1 and both "
               "ascending; the CV spread is kept\n";
        continue;
      }
      c.spread[(int)r] = std::move(hs);
    }
  }
  if (j.contains("sever") && j["sever"].is_object()) {
    const json& sv = j["sever"];
    if (sv.contains("rate") && sv["rate"].is_object())
      for (int k = 0; k < kKinds; k++) {
        if (!sv["rate"].contains(kKindNames[k])) continue;
        const json& rk = sv["rate"][kKindNames[k]];
        for (int r = 0; r < kRoles; r++)
          c.sever.rate[k][r] = Num(rk, kRoleNames[r], 0.0f, 0.0f, 1.0f,
                                   std::string("sever.rate.") + kKindNames[k], log);
      }
    if (sv.contains("hits") && sv["hits"].is_object())
      for (int k = 0; k < kKinds; k++) {
        if (!sv["hits"].contains(kKindNames[k])) continue;
        const json& hk = sv["hits"][kKindNames[k]];
        for (int r = 0; r < kRoles; r++)
          c.sever.hits[k][r] = Int(hk, kRoleNames[r], 0, 0, 1 << 30,
                                   std::string("sever.hits.") + kKindNames[k], log);
      }
    // owedHp / stumpHpPerTick are TUNING's (StumpLawFromTuning, in the
    // builtin this started from); the file's copy is a record.
  }
  if (j.contains("matchups") && j["matchups"].is_array()) {
    for (const json& m : j["matchups"]) {
      if (!m.is_object() || !m.contains("name") || !m["name"].is_string()) {
        log += "matchups: an entry with no name, skipped\n";
        continue;
      }
      MatchupCalib mc;
      mc.name = m["name"].get<std::string>();
      const std::string wh = "matchups." + mc.name;
      if (m.contains("a") && m["a"].is_string()) mc.a = m["a"].get<std::string>();
      if (m.contains("b") && m["b"].is_string()) mc.b = m["b"].get<std::string>();
      mc.aCount = Int(m, "aCount", 1, 1, 64, wh, log);
      mc.bCount = Int(m, "bCount", 1, 1, 64, wh, log);
      mc.gapVox = Num(m, "gapVox", mc.gapVox, 0.0f, 1000.0f, wh, log);
      mc.n = Int(m, "n", 0, 0, 1 << 20, wh, log);
      mc.winA = Num(m, "winA", 0.0f, 0.0f, 1.0f, wh, log);
      mc.winB = Num(m, "winB", 0.0f, 0.0f, 1.0f, wh, log);
      mc.draw = Num(m, "draw", 0.0f, 0.0f, 1.0f, wh, log);
      mc.ttkTicks = Num(m, "ttkTicks", 0.0f, 0.0f, 1e6f, wh, log);
      mc.ttkSd = Num(m, "ttkSd", 0.0f, 0.0f, 1e6f, wh, log);
      mc.timeoutTicks = Int(m, "timeoutTicks", 0, 0, 1 << 24, wh, log);
      mc.hpLeftA = Num(m, "hpLeftA", 0.0f, 0.0f, 1.0f, wh, log);
      mc.hpLeftB = Num(m, "hpLeftB", 0.0f, 0.0f, 1.0f, wh, log);
      c.matchups.push_back(mc);
    }
  }
  out = std::move(c);
  return true;
}

std::string DistantCalibToJson(const DistantCalib& c) {
  ojson o = ojson::object();
  o["_about"] = c.about;
  o["version"] = c.version;
  o["minSamples"] = c.minSamples;
  o["reachSlack"] = R4(c.reachSlack);
  ojson st = ojson::object();
  st["*"] = StyleJson(c.fallback);
  for (const StyleCalib& s : c.styles) st[s.name] = StyleJson(s);
  o["styles"] = st;
  ojson b = ojson::object();
  for (int g = 0; g < kGuards; g++) {
    ojson e = ojson::object();
    e["n"] = c.blockN[g];
    e["chance"] = R4(c.blockChance[g]);
    b[kGuardNames[g]] = e;
  }
  o["block"] = b;
  {
    ojson a = ojson::object(), ex = ojson::object(), n = ojson::object();
    for (int k = 0; k < kKinds; k++) {
      ex[kKindNames[k]] = R4(c.armour.exposure[k]);
      n[kKindNames[k]] = c.armour.n[k];
    }
    a["exposure"] = ex;
    a["n"] = n;
    o["armour"] = a;
  }
  {
    ojson bl = ojson::object(), p = ojson::object();
    for (int k = 0; k < kKinds; k++) p[kKindNames[k]] = R4(c.bleed.perHp[k]);
    bl["perHp"] = p;
    bl["decay"] = R4(c.bleed.decay);
    bl["drainShare"] = R4(c.bleed.drainShare);
    o["bleed"] = bl;
  }
  {
    ojson bu = ojson::object();
    bu["fracPerTick"] = R4(c.burn.fracPerTick);
    bu["hpPerTick"] = R4(c.burn.hpPerTick);
    bu["meanTicks"] = c.burn.meanTicks;
    bu["capMidFraction"] = R4(c.burn.capMidFraction);
    bu["capMidHealth"] = R4(c.burn.capMidHealth);
    bu["deathFraction"] = R4(c.burn.deathFraction);
    o["burn"] = bu;
  }
  {
    ojson r = ojson::object();
    r["n"] = c.ranged.n;
    r["hits"] = c.ranged.hits;
    r["arrowSpeed"] = R4(c.ranged.arrowSpeed);
    ojson knots = ojson::array();
    for (size_t i = 0; i < c.ranged.distKnots.size() && i < c.ranged.rateKnots.size(); i++)
      knots.push_back(ojson::array({R4(c.ranged.distKnots[i]), R4(c.ranged.rateKnots[i])}));
    r["rateByDistance"] = knots;
    r["speedRef"] = R4(c.ranged.speedRef);
    r["speedPenalty"] = R4(c.ranged.speedPenalty);
    r["damageFrac"] = R4(c.ranged.damageFrac);
    r["roleWeights"] = RolesJson(c.ranged.roleWeights);
    o["ranged"] = r;
  }
  {
    ojson sp = ojson::object();
    for (int r = 0; r < kRoles; r++) {
      const HitSpread& hs = c.spread[r];
      if (hs.p.empty()) continue;
      ojson knots = ojson::array();
      for (size_t i = 0; i < hs.p.size() && i < hs.x.size(); i++)
        knots.push_back(ojson::array({R4(hs.p[i]), R4(hs.x[i])}));
      sp[kRoleNames[r]] = knots;
    }
    if (!sp.empty()) o["spread"] = sp;
  }
  {
    ojson sv = ojson::object(), rate = ojson::object(), hits = ojson::object();
    for (int k = 0; k < kKinds; k++) {
      ojson rk = ojson::object(), hk = ojson::object();
      for (int r = 0; r < kRoles; r++) {
        if (c.sever.hits[k][r] == 0 && c.sever.rate[k][r] == 0.0f) continue;
        rk[kRoleNames[r]] = R4(c.sever.rate[k][r]);
        hk[kRoleNames[r]] = c.sever.hits[k][r];
      }
      rate[kKindNames[k]] = rk;
      hits[kKindNames[k]] = hk;
    }
    sv["rate"] = rate;
    sv["hits"] = hits;
    sv["owedHp"] = R4(c.sever.owedHp);
    sv["stumpHpPerTick"] = R4(c.sever.stumpHpPerTick);
    o["sever"] = sv;
  }
  {
    ojson ms = ojson::array();
    for (const MatchupCalib& m : c.matchups) {
      ojson e = ojson::object();
      e["name"] = m.name;
      e["a"] = m.a;
      e["b"] = m.b;
      e["aCount"] = m.aCount;
      e["bCount"] = m.bCount;
      e["gapVox"] = R4(m.gapVox);
      e["n"] = m.n;
      e["winA"] = R4(m.winA);
      e["winB"] = R4(m.winB);
      e["draw"] = R4(m.draw);
      e["ttkTicks"] = R4(m.ttkTicks);
      e["ttkSd"] = R4(m.ttkSd);
      e["timeoutTicks"] = m.timeoutTicks;
      e["hpLeftA"] = R4(m.hpLeftA);
      e["hpLeftB"] = R4(m.hpLeftB);
      ms.push_back(e);
    }
    o["matchups"] = ms;
  }
  return o.dump(2) + "\n";
}

// =============================================================================
// The kit, from a live creature
// =============================================================================

namespace {

// Authored impact timing of a style: every frame before the first cutting
// frame (the windup, with BeginStrokeProgram's two-tick floor), then half the
// cut -- the moment the blade crosses the aim. No tempo jitter: a distant
// stroke lands on the authored beat.
void StyleTiming(const AttackStyle& sty, int& impact, int& total) {
  int windup = 0, cut = 0, all = 0;
  const int first = sty.FirstCut(), last = sty.LastCut();
  if (!sty.frames.empty()) {
    for (int k = 0; k < (int)sty.frames.size(); k++) {
      const int t = std::max(1, sty.frames[k].ticks);
      all += t;
      if (first >= 0 && k < first) windup += t;
      else if (first >= 0 && k <= last) cut += t;
    }
  } else {
    windup = sty.windup.ticks;
    cut = sty.CutTravel().ticks;
    all = windup + cut + sty.recover.ticks;
  }
  if (first > 0 || sty.frames.empty()) windup = std::max(windup, 2);
  cut = std::max(cut, 2);
  impact = std::max(1, windup + cut / 2);
  total = std::max(impact, all + std::max(1, sty.release));
}

// The non-shield item in fist `h`, or null.
const ItemDef* HeldWeapon(const Mob& m, Hand h, const ItemLibrary* items) {
  const int s = m.HeldSlot(h);
  if (s < 0 || m.IsShieldSlot(s) || items == nullptr) return nullptr;
  return items->Named(m.HeldItem(h));
}

}  // namespace

std::vector<float> ShellHardnessOfLimbs(const Mob& m) {
  const int base = m.AppendedBase();
  std::vector<float> out((size_t)std::max(0, base), 0.0f);
  const MobSystem* sys = m.Sys();
  if (sys == nullptr) return out;
  std::vector<PrefabVoxel> lat;
  for (int s = base; s < m.LimbCount(); s++) {
    if (m.LimbDefAt(s).tag != "worn") continue;
    const int host = m.WornHostOf(s);
    if (host < 0 || host >= base || sys->LimbBody(m.Id(), s) == 0) continue;
    lat.clear();
    m.LatticeOfSlot(s, lat);
    if (lat.empty()) continue;
    std::map<uint32_t, uint32_t> count;
    for (const PrefabVoxel& v : lat) count[(uint32_t)(v.material & 0xFFFu)]++;
    uint32_t mat = 0, best = 0;
    for (const auto& kv : count)
      if (kv.second > best) {
        best = kv.second;
        mat = kv.first;
      }
    out[(size_t)host] = std::max(out[(size_t)host], m.MaterialHardness(mat));
  }
  return out;
}

DistantKit BuildDistantKit(const Mob& m, const ItemLibrary* items) {
  DistantKit k;
  const MobDef* def = m.Def();
  const MobSystem* sys = m.Sys();
  if (def == nullptr || sys == nullptr) return k;
  if (items == nullptr) items = sys->Items();
  k.id = m.Id();
  k.def = def->name;
  k.slack = def->worldSize.z * 0.5f;
  k.bleeds = def->bleedMat != 0;

  // ---- WHAT IT HOLDS UP ------------------------------------------------------
  const ItemDef* right = HeldWeapon(m, Hand::Right, items);
  const ItemDef* left = HeldWeapon(m, Hand::Left, items);
  k.guard = m.HoldsShield() ? GuardClass::Shield
            : (right != nullptr || left != nullptr) ? GuardClass::Blade
                                                    : GuardClass::None;

  // ---- WHAT IT SWINGS --------------------------------------------------------
  const ai::Brain* br = sys->MobBrain(m.Id());
  const ai::Profile* pr = br != nullptr ? sys->Behaviors().At(br->profile) : nullptr;
  if (pr != nullptr) k.profile = pr->name;
  const StyleLibrary& lib = sys->AttackStyles();
  // The form every held style resolves to: the strike hand's item's, or the
  // unarmed form for an empty fist (MobSystem::StartStroke's rule).
  const std::string& strikeItem = m.HeldItem();
  const int form = FormForItem(strikeItem.empty() || items == nullptr
                                   ? nullptr
                                   : items->Named(strikeItem));
  if (pr != nullptr) {
    for (const std::string& n : pr->attack.styles) {
      if (k.styles.size() >= 8) break;   // PickAttackStyle's own ceiling
      const int idx = lib.Find(n);
      const AttackStyle* sty = lib.At(idx);
      if (sty == nullptr || !StyleUsable(m, *sty)) continue;
      DistantStyle ds;
      ds.style = idx;
      ds.form = items != nullptr ? lib.ResolveForm(idx, form) : idx;
      ds.name = sty->name;
      ds.fallback = sty->fallback;
      ds.reach = StyleReachOn(m, *sty);
      const AttackStyle* played = lib.At(ds.form);
      StyleTiming(played != nullptr ? *played : *sty, ds.impactTicks, ds.totalTicks);
      const bool held = sty->weapon.empty() || sty->weapon == "held";
      ds.held = held;
      const MobNaturalWeaponDef* nw = nullptr;
      if (held) {
        const ItemDef* w = right != nullptr ? right : left;
        if (w != nullptr) {
          ds.strike = w->strike;
          ds.weapon = w->name;
        } else {
          nw = m.HeldSlot(Hand::Right) < 0 ? m.HandFist(Hand::Right) : nullptr;
          if (nw == nullptr && m.HeldSlot(Hand::Left) < 0) nw = m.HandFist(Hand::Left);
        }
      } else {
        nw = m.NaturalWeaponNamed(sty->weapon);
      }
      if (nw != nullptr) {
        ds.strike = m.StrikeProfileFor(*nw);
        ds.weapon = nw->name;
        ds.natural = true;
      }
      ds.kind = DominantKind(ds.strike);
      k.styles.push_back(ds);
    }
  }
  if (!def->closeStyle.empty())
    for (size_t i = 0; i < k.styles.size(); i++)
      if (k.styles[i].name == def->closeStyle) {
        k.closeStyle = (int)i;
        k.closeRange = def->closeStyleRange;
        break;
      }

  // ---- A BOW -------------------------------------------------------------------
  if (items != nullptr) {
    if (const ItemDef* bow = HeldBow(m, *items)) {
      const ItemDef* arrow = items->Named(bow->bow.ammo);
      k.ammo = bow->bow.ammo;
      k.arrowDamage = arrow != nullptr ? arrow->arrow.damage : 0.0f;
      k.arrowSpeed = BowArrowSpeed(*bow, BowFullDrawTicks(*bow));
      k.shots = MobShotsLeft(m, *items);
      k.ranged = k.shots > 0 && k.arrowSpeed > 0.0f;
    }
  }

  // ---- THE BODY: roles, vitals, and what is worn over each limb --------------
  const int base = m.AppendedBase();
  k.limbs.resize((size_t)std::max(0, base));
  for (int i = 0; i < base; i++) {
    const MobLimbDef& ld = m.LimbDefAt(i);
    DistantLimb& L = k.limbs[(size_t)i];
    L.name = ld.name;
    L.bloodless = ld.bloodless;
    L.role = LimbRoleOf(ld.tag, ld.name, ld.bloodless);
    L.vital = ld.vital || i == def->rootLimb;
    L.alive = sys->LimbBody(m.Id(), i) != 0;
    L.maxHp = i < (int)def->limbs.size() ? def->limbs[(size_t)i].hp : ld.hp;
    L.severable = ld.severable && i != def->rootLimb;
    for (int j = 0; j < base && !ld.parent.empty(); j++)
      if (m.LimbDefAt(j).name == ld.parent) {
        L.parent = j;
        break;
      }
  }
  const std::vector<float> hard = ShellHardnessOfLimbs(m);
  for (size_t i = 0; i < hard.size() && i < k.limbs.size(); i++)
    k.limbs[i].shellHardness = hard[i];
  return k;
}

DistantVitals CaptureDistantVitals(const Mob& m, const DistantCalib& cal) {
  DistantVitals v;
  const MobSystem* sys = m.Sys();
  const int base = m.AppendedBase();
  v.limbHp.resize((size_t)std::max(0, base));
  float owed = 0.0f;
  for (int i = 0; i < base; i++) {
    v.limbHp[(size_t)i] = m.LimbHpAt(i);
    if (sys != nullptr && !m.LimbDefAt(i).bloodless && sys->LimbBody(m.Id(), i) != 0)
      owed += std::max(0.0f, sys->LimbBleedBudget(m.Id(), i));
  }
  // Owed blood in VOXELS (MobLimb::bleedBudget) -> hp, the share that drains,
  // and the rate that pays it out over the table's decay.
  owed *= CurrentTuning().gore.bleedHpPerVoxel * cal.bleed.drainShare;
  v.bleedRate = owed * (1.0f - cal.bleed.decay);
  v.burnFrac = m.BurnFraction();
  float hpFrac = 0, burning = 0;
  int lost = 0;
  m.BodyFacts(hpFrac, burning, lost);
  v.burnTicksLeft = burning > 0.0f ? cal.burn.meanTicks : 0;
  v.dead = m.Alive() ? DistantDeath::None : DistantDeath::Other;
  return v;
}

// =============================================================================
// The resolvers
// =============================================================================

namespace {

// A limb a blow can land on: attached, flesh, a role -- and hp left, unless
// the kit says a ruined limb is still in the way (DistantKit::absorbRuined).
bool LiveLimb(const DistantKit& k, const std::vector<float>& hp, size_t i) {
  return i < k.limbs.size() && i < hp.size() && k.limbs[i].alive &&
         !k.limbs[i].bloodless && k.limbs[i].role != LimbRole::Other &&
         (hp[i] > 0.0f || k.absorbRuined);
}

// A role from the weights by the draw `u` in [0, 1).
LimbRole SampleRole(const float* w, float u) {
  float sum = 0.0f;
  for (int i = 0; i < kRoles; i++) sum += std::max(0.0f, w[i]);
  if (sum <= 1e-6f) return LimbRole::Torso;
  float acc = 0.0f;
  const float t = u * sum;
  for (int i = 0; i < kRoles; i++) {
    acc += std::max(0.0f, w[i]);
    if (t < acc) return (LimbRole)i;
  }
  for (int i = kRoles - 1; i >= 0; i--)
    if (w[i] > 0.0f) return (LimbRole)i;
  return LimbRole::Torso;
}

// A live limb of `role` (id-hashed pick among them, in index order); a role
// the defender lacks folds into torso; a body with no live torso takes the
// blow on its healthiest live limb (lowest index on a tie). -1 = nothing left.
int PickLimb(const DistantKit& k, const std::vector<float>& hp, LimbRole& role,
             uint32_t h) {
  for (int pass = 0; pass < 2; pass++) {
    const LimbRole want = pass == 0 ? role : LimbRole::Torso;
    int n = 0;
    for (size_t i = 0; i < k.limbs.size(); i++)
      if (LiveLimb(k, hp, i) && k.limbs[i].role == want) n++;
    if (n == 0) continue;
    int pick = (int)(h % (uint32_t)n);
    for (size_t i = 0; i < k.limbs.size(); i++)
      if (LiveLimb(k, hp, i) && k.limbs[i].role == want && pick-- == 0) {
        role = want;
        return (int)i;
      }
  }
  int best = -1;
  for (size_t i = 0; i < k.limbs.size(); i++)
    if (LiveLimb(k, hp, i) && (best < 0 || hp[i] > hp[(size_t)best])) best = (int)i;
  if (best >= 0) role = k.limbs[(size_t)best].role;
  return best;
}

// What a shell over the struck limb lets through (see ArmourCalib).
float ShellFactor(StrikeKind kind, DamageCause cause, float hardness,
                  const ArmourCalib& a, const Tuning::Gear& g) {
  if (hardness <= 0.0f) return 1.0f;
  // CLOTH IS NOT ARMOUR TO AN EDGE (melee.cpp, 2026-10-05): a shell no harder
  // than gear.cutHardnessRef is passed by an edged blow as if bare. A point
  // prices it by the same ratio below, which is 1 there anyway.
  if (kind == StrikeKind::Cut && hardness <= g.cutHardnessRef) return 1.0f;
  // A POINT prices the plate cell by cell by the cut-hardness ratio
  // (phys/kerf.h PierceChannel): the Blade row's carve, which is the share of
  // the channel that gets through.
  const float passed =
      kind == StrikeKind::Pierce
          ? ShellResponseOf(hardness, DamageCause::Blade, g).carve
          : ShellResponseOf(hardness, cause, g).passed;
  const float e = std::clamp(a.exposure[(int)kind], 0.0f, 1.0f);
  return std::clamp(passed + (1.0f - passed) * e, 0.0f, 1.0f);
}

// The CV spread, mean exactly 1: (1 - cv) + cv * Exp(1), Exp(1) by inversion
// of a 24-bit draw (u < 1 always, so the log is finite).
float Spread(float cv, float u) {
  cv = std::clamp(cv, 0.0f, 1.0f);
  const float e = -std::log(1.0f - u);
  return (1.0f - cv) + cv * e;
}

float Interp(const std::vector<float>& x, const std::vector<float>& y, float at) {
  const size_t n = std::min(x.size(), y.size());
  if (n == 0) return 0.0f;
  if (at <= x[0]) return y[0];
  for (size_t i = 1; i < n; i++)
    if (at <= x[i]) {
      const float t = (at - x[i - 1]) / std::max(1e-6f, x[i] - x[i - 1]);
      return y[i - 1] + (y[i] - y[i - 1]) * t;
    }
  return y[n - 1];
}

float Bleed(const DistantKit& def, int limb, float hp, StrikeKind kind,
            const DistantCalib& cal) {
  if (!def.bleeds || limb < 0 || limb >= (int)def.limbs.size() ||
      def.limbs[(size_t)limb].bloodless)
    return 0.0f;
  return hp * std::max(0.0f, cal.bleed.perHp[(int)kind]) *
         (1.0f - std::clamp(cal.bleed.decay, 0.0f, 0.9999f));
}

}  // namespace

int PickDistantStyle(const DistantKit& k, float distance, uint64_t id,
                     uint32_t tick) {
  if (k.styles.empty()) return -1;
  // CLOSE QUARTERS FIRST (MobDef::closeStyle), where it arrives.
  if (k.closeStyle >= 0 && k.closeStyle < (int)k.styles.size() && distance > 0.0f &&
      distance <= k.closeRange) {
    const DistantStyle& s = k.styles[(size_t)k.closeStyle];
    if (s.reach <= 0.0f || distance <= s.reach + k.slack) return k.closeStyle;
  }
  int found[8];
  int n = 0;
  bool haveReal = false;
  for (size_t i = 0; i < k.styles.size() && n < 8; i++) {
    const DistantStyle& s = k.styles[i];
    if (distance > 0.0f && s.reach > 0.0f && distance > s.reach + k.slack) continue;
    found[n++] = (int)i;
    haveReal = haveReal || !s.fallback;
  }
  if (haveReal) {
    int keep = 0;
    for (int j = 0; j < n; j++)
      if (!k.styles[(size_t)found[j]].fallback) found[keep++] = found[j];
    n = keep;
  }
  if (n == 0) return -1;
  if (n == 1) return found[0];
  const uint32_t h = rng::Hash3((uint32_t)id ^ ai::kSaltDistantStyle, tick, 0);
  return found[h % (uint32_t)n];
}

StrikeOutcome ResolveStrike(const DistantKit& att, const DistantKit& def,
                            const std::vector<float>& defHp, int style,
                            float distance, const DistantRngKey& key,
                            const DistantCalib& cal, const Tuning::Gear& gear) {
  StrikeOutcome o;
  if (style < 0 || style >= (int)att.styles.size()) {
    o.result = StrikeResult::Miss;
    return o;
  }
  const DistantStyle& s = att.styles[(size_t)style];
  o.kind = s.kind;
  o.cause = CauseOf(s.kind, s.natural);
  if (s.reach > 0.0f && distance > s.reach + att.slack + cal.reachSlack) {
    o.result = StrikeResult::OutOfReach;
    return o;
  }
  bool any = false;
  for (size_t i = 0; i < def.limbs.size() && !any; i++) any = LiveLimb(def, defHp, i);
  if (!any) {
    o.result = StrikeResult::NoTarget;
    return o;
  }
  const uint32_t seed = (uint32_t)key.id ^ ai::kSaltDistantStrike;
  // 0. THE GUARD: a shield or a blade in the way, per the defender's class.
  const float block = std::clamp(cal.blockChance[(int)def.guard], 0.0f, 0.95f);
  if (rng::Unit01(rng::Hash3(seed, key.tick, 0)) < block) {
    o.result = StrikeResult::Blocked;
    return o;
  }
  // 1. THE STYLE'S CONTACT RATE, the weapon's own row where it has samples.
  const StyleCalib& sc = cal.Style(s.name);
  float hitRate = sc.hitRate, meanPower = sc.meanPower;
  for (const WeaponCalib& w : sc.byWeapon)
    if (w.weapon == s.weapon && w.n >= cal.minSamples) {
      hitRate = w.hitRate;
      if (w.meanPower > 0.0f) meanPower = w.meanPower;
      break;
    }
  if (rng::Unit01(rng::Hash3(seed, key.tick, 1)) >= hitRate) {
    o.result = StrikeResult::Miss;
    return o;
  }
  // 2. WHERE: a role by the measured weights, then a live limb of it.
  o.role = SampleRole(sc.roleWeights, rng::Unit01(rng::Hash3(seed, key.tick, 2)));
  o.limb = PickLimb(def, defHp, o.role, rng::Hash3(seed, key.tick, 3));
  if (o.limb < 0) {
    o.result = StrikeResult::NoTarget;
    return o;
  }
  // 3. HOW MUCH: the strike total x the measured multiplier, spread by its CV,
  // through whatever is worn over that limb.
  o.shellFactor = ShellFactor(o.kind, o.cause, def.limbs[(size_t)o.limb].shellHardness,
                              cal.armour, gear);
  // THE SIZE: the role's measured inverse CDF (its tail is the organs), else
  // the CV spread round the mean.
  const float u4 = rng::Unit01(rng::Hash3(seed, key.tick, 4));
  const HitSpread& hs = cal.spread[(int)o.role];
  const float mult = hs.p.size() >= 2 ? Interp(hs.p, hs.x, u4) : Spread(sc.powerCv, u4);
  o.hp = std::max(0.0f, s.strike.Total()) * std::max(0.0f, meanPower) * mult * o.shellFactor;
  o.bleedAdded = Bleed(def, o.limb, o.hp, o.kind, cal);
  // ...AND WHETHER IT COMES OFF: the measured rate for this kind on this role,
  // through whatever is worn over it (an edge stopped by plate severs
  // nothing), only for a limb that CAN come off.
  const DistantLimb& L = def.limbs[(size_t)o.limb];
  if (L.severable && (int)o.kind < kKinds) {
    const float pSever = std::clamp(cal.sever.rate[(int)o.kind][(int)o.role], 0.0f, 1.0f) *
                         o.shellFactor;
    o.sever = pSever > 0.0f && rng::Unit01(rng::Hash3(seed, key.tick, 5)) < pSever;
  }
  o.hit = true;
  o.result = StrikeResult::Hit;
  return o;
}

ArrowOutcome ResolveArrow(const DistantKit& shooter, const DistantKit& def,
                          const std::vector<float>& defHp, float distance,
                          float targetSpeed, const DistantRngKey& key,
                          const DistantCalib& cal, const Tuning::Gear& gear) {
  ArrowOutcome o;
  const float speed = shooter.arrowSpeed > 0.0f ? shooter.arrowSpeed : cal.ranged.arrowSpeed;
  o.flightTicks = std::max(
      1, (int)std::ceil(std::max(0.0f, distance) / std::max(1e-3f, speed / 30.0f)));
  const float pd = std::clamp(Interp(cal.ranged.distKnots, cal.ranged.rateKnots, distance),
                              0.0f, 1.0f);
  const float sv = std::max(0.0f, targetSpeed) / std::max(1e-3f, cal.ranged.speedRef);
  o.pHit = pd * std::max(0.0f, 1.0f - std::clamp(cal.ranged.speedPenalty, 0.0f, 1.0f) *
                                          std::min(1.0f, sv));
  const uint32_t seed = (uint32_t)key.id ^ ai::kSaltDistantArrow;
  if (rng::Unit01(rng::Hash3(seed, key.tick, 0)) >= o.pHit) return o;
  o.role = SampleRole(cal.ranged.roleWeights, rng::Unit01(rng::Hash3(seed, key.tick, 1)));
  o.limb = PickLimb(def, defHp, o.role, rng::Hash3(seed, key.tick, 2));
  if (o.limb < 0) return o;
  const float dmg = shooter.arrowDamage > 0.0f ? shooter.arrowDamage : 35.0f;
  const float f = ShellFactor(StrikeKind::Pierce, DamageCause::Pierce,
                              def.limbs[(size_t)o.limb].shellHardness, cal.armour, gear);
  o.hp = dmg * std::max(0.0f, cal.ranged.damageFrac) * f;
  o.bleedAdded = Bleed(def, o.limb, o.hp, StrikeKind::Pierce, cal);
  o.hit = true;
  return o;
}

float TotalHp(const DistantVitals& v, const DistantKit& k) {
  float sum = 0.0f;
  for (size_t i = 0; i < k.limbs.size() && i < v.limbHp.size(); i++)
    if (k.limbs[i].alive && !k.limbs[i].bloodless && v.limbHp[i] > 0.0f)
      sum += v.limbHp[i];
  return sum;
}

float BurnCapFor(float f, const BurnCalib& b) {
  // Mob::BurnHealthCapFor, at the table's knots: (0, 1) -> (mid, midHealth)
  // -> (death, 0), piecewise linear.
  if (f <= 0.0f) return 1.0f;
  if (f >= b.deathFraction) return 0.0f;
  if (f <= b.capMidFraction)
    return 1.0f + (b.capMidHealth - 1.0f) * (f / b.capMidFraction);
  const float t = (f - b.capMidFraction) /
                  std::max(1e-4f, b.deathFraction - b.capMidFraction);
  return b.capMidHealth * (1.0f - t);
}

DistantDeath DeathTest(const DistantVitals& v, const DistantKit& k,
                       const DistantCalib& cal) {
  if (v.dead != DistantDeath::None) return v.dead;
  if (v.burnFrac > 0.0f && BurnCapFor(v.burnFrac, cal.burn) <= 0.0f)
    return DistantDeath::Burns;
  for (size_t i = 0; i < k.limbs.size() && i < v.limbHp.size(); i++)
    if (k.limbs[i].alive && !k.limbs[i].bloodless && k.limbs[i].vital &&
        v.limbHp[i] <= 0.0f)
      return DistantDeath::VitalLimb;
  if (TotalHp(v, k) <= 0.0f) return DistantDeath::BloodLoss;
  return DistantDeath::None;
}

DistantDeath ApplyStrike(DistantVitals& v, const DistantKit& k,
                         const StrikeOutcome& o, const DistantCalib& cal) {
  if (v.dead != DistantDeath::None) return v.dead;
  if (!o.hit || o.limb < 0 || o.limb >= (int)v.limbHp.size()) return v.dead;
  v.limbHp[(size_t)o.limb] -= o.hp;
  v.bleedRate += o.bleedAdded;
  v.dead = DeathTest(v, k, cal);
  return v.dead;
}

DistantDeath ApplyArrow(DistantVitals& v, const DistantKit& k,
                        const ArrowOutcome& o, const DistantCalib& cal) {
  StrikeOutcome s;
  s.hit = o.hit;
  s.limb = o.limb;
  s.hp = o.hp;
  s.bleedAdded = o.bleedAdded;
  return ApplyStrike(v, k, s, cal);
}

namespace {
// Spread `cost` over the live non-bloodless limbs in proportion to what each
// has (Mob::DrainBlood / Mob::SpendHp's rule: no limb is picked to be ruined).
void SpendProportional(DistantVitals& v, const DistantKit& k, float cost, float total) {
  const float frac = cost / total;
  for (size_t i = 0; i < k.limbs.size() && i < v.limbHp.size(); i++)
    if (k.limbs[i].alive && !k.limbs[i].bloodless && v.limbHp[i] > 0.0f)
      v.limbHp[i] -= v.limbHp[i] * frac;
}
}  // namespace

DistantDeath SeverLimb(DistantVitals& v, DistantKit& k, int limb, const DistantCalib& cal,
                       std::vector<int>* off) {
  if (off != nullptr) off->clear();
  if (v.dead != DistantDeath::None) return v.dead;
  if (limb < 0 || limb >= (int)k.limbs.size() || !k.limbs[(size_t)limb].alive)
    return v.dead;
  // The subtree: the limb, then (in index order, until nothing changes) every
  // live limb whose parent is off. Rigs list a parent before its children,
  // but the fixed point does not rely on it.
  const bool vital = k.limbs[(size_t)limb].vital;
  k.limbs[(size_t)limb].alive = false;
  if (off != nullptr) off->push_back(limb);
  for (bool more = true; more;) {
    more = false;
    for (size_t i = 0; i < k.limbs.size(); i++) {
      DistantLimb& L = k.limbs[i];
      if (!L.alive || L.parent < 0 || L.parent >= (int)k.limbs.size() ||
          k.limbs[(size_t)L.parent].alive)
        continue;
      L.alive = false;
      if (off != nullptr) off->push_back((int)i);
      more = true;
    }
  }
  // THE STUMP: open until death, and the gout's owed blood on the rate.
  if (k.bleeds) {
    v.stumps++;
    v.bleedRate += std::max(0.0f, cal.sever.owedHp) *
                   (1.0f - std::clamp(cal.bleed.decay, 0.0f, 0.9999f));
  }
  // A head off is a death (Mob::Sever routes a vital limb to Die).
  if (vital) {
    v.dead = DistantDeath::VitalLimb;
    return v.dead;
  }
  v.dead = DeathTest(v, k, cal);
  return v.dead;
}

float StepBleed(DistantVitals& v, const DistantKit& k, const DistantCalib& cal) {
  if (v.dead != DistantDeath::None || (v.bleedRate <= 0.0f && v.stumps <= 0)) return 0.0f;
  // The decaying wounds, and every open stump's drain (until death).
  const float cost = v.bleedRate + (float)std::max(0, v.stumps) *
                                       std::max(0.0f, cal.sever.stumpHpPerTick);
  v.bleedRate *= std::clamp(cal.bleed.decay, 0.0f, 0.9999f);
  if (v.bleedRate < 1e-4f) v.bleedRate = 0.0f;
  const float total = TotalHp(v, k);
  if (total <= cost) {
    // Bled white (DrainBlood): systemic, so the limbs keep what they had.
    v.dead = DistantDeath::BloodLoss;
    return total;
  }
  SpendProportional(v, k, cost, total);
  v.dead = DeathTest(v, k, cal);
  return cost;
}

int StumpTicksLeft(const DistantVitals& v, const DistantKit& k, const DistantCalib& cal) {
  const float c = (float)std::max(0, v.stumps) * std::max(0.0f, cal.sever.stumpHpPerTick);
  if (v.dead != DistantDeath::None || !(c > 0.0f)) return INT32_MAX;
  const float total = TotalHp(v, k);
  // StepBleed kills on the tick whose opening total is at or under its
  // bill: after j clean ticks the total is total - j c, so the first lethal
  // tick is the smallest j with total - j c <= c.
  if (total <= c) return 0;
  return (int)std::min<double>((double)INT32_MAX - 1.0, std::ceil((double)(total - c) / (double)c));
}

float StepStumps(DistantVitals& v, const DistantKit& k, const DistantCalib& cal, int ticks,
                 int* diedAfter) {
  if (diedAfter != nullptr) *diedAfter = -1;
  if (v.dead != DistantDeath::None || ticks <= 0 || v.stumps <= 0) return 0.0f;
  const float c = (float)v.stumps * std::max(0.0f, cal.sever.stumpHpPerTick);
  if (!(c > 0.0f)) return 0.0f;
  const int clean = StumpTicksLeft(v, k, cal);
  const float total = TotalHp(v, k);
  if (clean < ticks) {
    // It bled out inside the span: the clean ticks' drain, then the lethal
    // one (DrainBlood's "bled white": the limbs keep what they had).
    if (clean > 0) SpendProportional(v, k, (float)clean * c, total);
    v.dead = DistantDeath::BloodLoss;
    if (diedAfter != nullptr) *diedAfter = clean + 1;
    return total;
  }
  const float cost = (float)ticks * c;
  SpendProportional(v, k, cost, total);
  v.dead = DeathTest(v, k, cal);
  return cost;
}

float StepBurn(DistantVitals& v, const DistantKit& k, const DistantCalib& cal) {
  if (v.dead != DistantDeath::None || v.burnTicksLeft <= 0) return 0.0f;
  v.burnTicksLeft--;
  v.burnFrac = std::min(1.0f, v.burnFrac + std::max(0.0f, cal.burn.fracPerTick));
  const float before = TotalHp(v, k);
  const float cost = std::max(0.0f, cal.burn.hpPerTick);
  if (cost > 0.0f && before > cost) SpendProportional(v, k, cost, before);
  // THE CAP (Mob::ApplyBurnCap): every authored limb clamped to its share.
  const float cap = BurnCapFor(v.burnFrac, cal.burn);
  if (cap <= 0.0f) {
    v.dead = DistantDeath::Burns;
    return before;
  }
  for (size_t i = 0; i < k.limbs.size() && i < v.limbHp.size(); i++)
    if (k.limbs[i].alive && !k.limbs[i].bloodless)
      v.limbHp[i] = std::min(v.limbHp[i], k.limbs[i].maxHp * cap);
  v.dead = DeathTest(v, k, cal);
  return before - TotalHp(v, k);
}

}  // namespace distant
