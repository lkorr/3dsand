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
  if (best < 0) return want;   // boxed in: keep asking, the drive refuses anyway
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
    default: return "?";
  }
}

Intent IntentFromName(const std::string& s) {
  for (int i = 0; i < (int)Intent::Count; i++)
    if (s == IntentName((Intent)i)) return (Intent)i;
  return Intent::Count;
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
//       "repathTicks": 12, "navRadius": 22, "maxStepUp": 2, "maxStepDown": 5
//     },
//     "attack": {
//       "style": "slash", "reach": 9.5, "aimTolerance": 0.45,
//       "cadenceTicks": 38, "jitterTicks": 20,
//       "commitTicks": 10, "disengageTicks": 24
//     },
//     "intents": {                    // ABSENT = weight 0 = disabled
//       "idle":      { "weight": 0.05 },
//       "face":      { "weight": 0.5 },
//       "approach":  { "weight": 1.0, "minDwellTicks": 8 },
//       "holdRange": { "weight": 1.1, "minDwellTicks": 6 },
//       "circle":    { "weight": 0.9, "minDwellTicks": 20, "cooldownTicks": 14 },
//       "attack":    { "weight": 2.0 }
//     }
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
      pr.movement.circleTendency = q.value("circleTendency", 0.0f);
      pr.movement.circleHoldTicks = q.value("circleHoldTicks", 24u);
      pr.movement.repathTicks = q.value("repathTicks", 12u);
      pr.movement.navRadius = q.value("navRadius", 22.0f);
      pr.movement.maxStepUp = q.value("maxStepUp", 2);
      pr.movement.maxStepDown = q.value("maxStepDown", 5);
    }
    if (p.contains("attack")) {
      const auto& q = p["attack"];
      pr.attack.style = q.value("style", std::string("slash"));
      pr.attack.reach = q.value("reach", 8.0f);
      pr.attack.aimTolerance = q.value("aimTolerance", 0.45f);
      pr.attack.cadenceTicks = q.value("cadenceTicks", 40u);
      pr.attack.jitterTicks = q.value("jitterTicks", 18u);
      pr.attack.commitTicks = q.value("commitTicks", 10u);
      pr.attack.disengageTicks = q.value("disengageTicks", 22u);
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
      << ", \"keepRangeScale\": " << num(p.perception.keepRangeScale) << " },\n";
    o << "      \"movement\": { \"mobile\": "
      << (p.movement.mobile ? "true" : "false")
      << ", \"rangeMin\": " << num(p.movement.rangeMin)
      << ", \"rangeMax\": " << num(p.movement.rangeMax)
      << ", \"bandSlack\": " << num(p.movement.bandSlack)
      << ", \"approachSpeed\": " << num(p.movement.approachSpeed)
      << ", \"strafeSpeed\": " << num(p.movement.strafeSpeed)
      << ", \"retreatSpeed\": " << num(p.movement.retreatSpeed)
      << ", \"circleTendency\": " << num(p.movement.circleTendency)
      << ", \"circleHoldTicks\": " << p.movement.circleHoldTicks
      << ", \"repathTicks\": " << p.movement.repathTicks
      << ", \"navRadius\": " << num(p.movement.navRadius)
      << ", \"maxStepUp\": " << p.movement.maxStepUp
      << ", \"maxStepDown\": " << p.movement.maxStepDown << " },\n";
    o << "      \"attack\": { \"style\": \"" << p.attack.style
      << "\", \"reach\": " << num(p.attack.reach)
      << ", \"aimTolerance\": " << num(p.attack.aimTolerance)
      << ", \"cadenceTicks\": " << p.attack.cadenceTicks
      << ", \"jitterTicks\": " << p.attack.jitterTicks
      << ", \"commitTicks\": " << p.attack.commitTicks
      << ", \"disengageTicks\": " << p.attack.disengageTicks << " },\n";
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
    o << "\n      }\n";
    o << "    }" << (i + 1 < lib.profiles.size() ? "," : "") << "\n";
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
// because it is the expensive one) the line.
bool Perceives(const SelfView& self, const Profile& pr, const WorldView& v,
               const Actor& a, float range, float& outDist) {
  outDist = PlanarDist(self.Centre(), a.centre);
  if (outDist > range) return false;
  if (pr.perception.fovDegrees < 359.9f) {
    const float bearing = BearingTo(self.Centre(), a.centre);
    const float err = std::abs(WrapPi(bearing - self.heading));
    if (err > pr.perception.fovDegrees * (kPi / 180.0f) * 0.5f) return false;
  }
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

  const Actor* best = nullptr;
  float bestDist = 1e30f;
  for (const Actor& a : *v.actors) {
    if (!a.alive || a.id == self.id) continue;
    if (a.faction == self.faction) continue;
    float d = 0;
    const float range = (b.hasTarget && a.id == b.targetId) ? keep : acquire;
    if (!Perceives(self, pr, v, a, range, d)) continue;
    if (d < bestDist) {
      bestDist = d;
      best = &a;
    }
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
// Navigation follow-through
// ---------------------------------------------------------------------------

// How close a waypoint has to be before the follower moves to the next one.
// Generous: a body with a stride cannot stand on a point, and a tight radius
// makes it circle its own waypoint forever.
constexpr float kWaypointRadius = 1.9f;

void UpdatePath(Brain& b, const Profile& pr, const SelfView& self,
                const WorldView& v, uint32_t tick) {
  NavParams np;
  np.radius = (int)std::lround(pr.movement.navRadius);
  np.maxStepUp = pr.movement.maxStepUp;
  np.maxStepDown = pr.movement.maxStepDown;
  np.headroom = std::max(2, (int)std::ceil(self.size.y * 0.75f));

  // Retire waypoints we have arrived at BEFORE deciding whether to replan, so
  // "the path is finished" and "the path is stale" are different answers.
  const Vec3 foot = self.Foot();
  while (!b.path.Done() &&
         PlanarDist(foot, b.path.Current()) <= kWaypointRadius)
    b.path.cursor++;

  bool replan = false;
  if (tick >= b.nextRepathTick) replan = true;
  // The target walked away from the route it was planned for. Cheap test, and
  // it is what keeps a duelist from running to where you WERE.
  if (b.path.valid && PlanarDist(b.pathTarget, b.targetPos) > 4.0f) replan = true;
  if (b.path.Done()) replan = true;
  if (!replan) return;

  b.nextRepathTick = tick + std::max(2u, pr.movement.repathTicks);
  b.pathTarget = b.targetPos;

  // THE STRAIGHT LINE FIRST. Most of a fight is fought in the open, and asking
  // A* to confirm what a 12-sample line already knows is the expensive way to
  // learn nothing. This is also the graceful-degradation path: with no plan and
  // a clear line, "walk at them" is exactly right.
  if (LineWalkable(v.probe, np, foot, b.targetPos)) {
    b.path.Clear();
    b.navFailed = false;
    return;
  }
  b.navFailed = !FindPath(v.probe, np, foot, b.targetPos, b.path);
}

// Where the follower currently wants to walk: the live waypoint, or the target
// itself when there is no path (open ground, or the planner gave up).
Vec3 SteerPoint(const Brain& b) {
  return b.path.Done() ? b.targetPos : b.path.Current();
}

}  // namespace

// ---------------------------------------------------------------------------
// The arbiter
// ---------------------------------------------------------------------------

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

  Perceive(brain, pr, self, view, tick);

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
  const bool disengaging = tick < brain.disengageUntil;
  if (disengaging && hi > 0) lo = hi;
  const float d = brain.targetDist;
  float bandErr = 0;                       // + too far, - too close
  if (brain.hasTarget && hi > 0) {
    if (d > hi) bandErr = d - hi;
    else if (d < lo) bandErr = d - lo;
  } else if (brain.hasTarget) {
    bandErr = d;                           // no band authored: just close
  }
  const float slack = std::max(0.25f, pr.movement.bandSlack);

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

  // ---- the attack clock ---------------------------------------------------
  // Cadence plus a per-attack hash-RNG jitter. The jitter is the difference
  // between a duelist and a metronome, and it is drawn from (id, tick) so a
  // replay of the same fight produces the same rhythm.
  const bool attackReady =
      brain.hasTarget && brain.visible && tick >= brain.nextAttackTick &&
      !disengaging && d <= pr.attack.reach && aimErr <= pr.attack.aimTolerance;

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
        raw[(int)Intent::CircleStrafe] =
            std::clamp(pr.movement.circleTendency, 0.0f, 1.0f);
    }
  }
  if (attackReady) raw[(int)Intent::RequestAttack] = 1.0f;

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
  const bool locked = raw[(int)brain.intent] > 0.0f &&
                      dwell < curT.minDwellTicks;

  for (int i = 0; i < (int)Intent::Count; i++) brain.score[i] = 0;
  for (int i = 0; i < (int)Intent::Count; i++) {
    const IntentTuning& t = pr.intents[i];
    if (t.weight <= 0.0f) continue;
    if ((Intent)i != brain.intent && tick < brain.cooldownUntil[i]) continue;
    float s = raw[i] * t.weight;
    if (s <= 0.0f) continue;
    if ((Intent)i == brain.intent) s += pr.hysteresis;
    brain.score[i] = s;
    if (s > bestScore) {
      bestScore = s;
      winner = (Intent)i;
    }
  }

  if (locked) winner = brain.intent;

  // THE COMMIT WINDOW OVERRIDES EVERYTHING. Once an attack request has gone out
  // the body has promised the stroke system it will hold its facing, so no
  // amount of utility may spin it mid-swing.
  if (tick < brain.commitUntil) winner = Intent::FaceTarget;

  if (winner != brain.intent) {
    // Leaving an intent starts its cooldown. Charged on EXIT rather than on
    // entry so a long circle is not punished by its own duration.
    brain.cooldownUntil[(int)brain.intent] =
        tick + pr.intents[(int)brain.intent].cooldownTicks;
    brain.intent = winner;
    brain.intentSince = tick;
  }

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
      UpdatePath(brain, pr, self, view, tick);
      const Vec3 wp = SteerPoint(brain);
      out.desiredHeading = Deflect(BearingTo(self.Foot(), wp), self.heading, ground);
      out.driveScale = sp * pr.movement.approachSpeed;
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
            -sp * std::min(pr.movement.retreatSpeed, arrive(bandErr));
      } else if (bandErr > 0) {
        // Small closing correction. Deliberately NOT routed through the
        // navigator: inside a couple of voxels of the band there is nothing to
        // route around, and replanning here would cost a search per tick for a
        // step the mob takes anyway.
        out.driveScale =
            sp * std::min(pr.movement.approachSpeed * 0.55f, arrive(bandErr));
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
      const float strafeCap =
          std::min(pr.movement.strafeSpeed, maxTangential / std::max(1.0f, self.speed));
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
      brain.commitUntil = tick + pr.attack.commitTicks;
      brain.disengageUntil = brain.commitUntil + pr.attack.disengageTicks;
      brain.lastAttackTick = tick;
      brain.attacksIssued++;
      out.attack = true;
      out.request.mobId = self.id;
      out.request.targetId = brain.targetId;
      out.request.style = pr.attack.style;
      out.request.targetPoint = brain.targetPos;
      out.request.tick = tick;
      out.request.commitTicks = pr.attack.commitTicks;
      out.request.distance = d;
      break;
    }

    default:
      break;
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
