// demon_malice.cpp — the motive ladder, schemes, duty twists, penalties
// weighed, the free act. See demon_malice.h.
#include "game/demon_malice.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "game/container.h"
#include "game/demon.h"
#include "game/demon_talk.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/session.h"
#include "sim/materials.h"
#include "sim/scale.h"
#include "sim/world.h"

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

using json = nlohmann::json;

namespace demon {

namespace {

constexpr const char* kActNames[] = {"comply", "bide",  "spell", "attack",
                                     "attack_ally", "under", "onto", "hazard"};
constexpr int kActCount = 8;

uint64_t MeId(int session) { return ai::kPlayerActorBase + (uint64_t)session; }

float Planar(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

Vec3 MobCentre(const Mob& m) {
  const MobDef* d = m.Def();
  return m.Origin() + (d ? d->worldSize : Vec3{}) * 0.5f;
}

SessionTick* Who(std::span<SessionTick> players, int session) {
  for (SessionTick& p : players)
    if (p.s->index == session) return &p;
  return nullptr;
}

uint32_t MatByName(const std::vector<MaterialDef>& mats, const std::string& n) {
  for (size_t i = 1; i < mats.size(); i++)
    if (mats[i].name == n) return (uint32_t)i;
  return 0;
}

KitTags TagsFrom(const json& t) {
  KitTags k;
  if (!t.is_object()) return k;
  k.targets = t.value("targets", std::string());
  k.direct = t.value("direct", false);
  for (const json& c : t.value("creates", json::array()))
    if (c.is_string()) k.creates.push_back(c.get<std::string>());
  k.alters = t.value("alters", std::string());
  k.affectsBody = t.value("affectsBody", false);
  k.region = std::max(0, t.value("region", 0));
  return k;
}

// "contained" | "released" | "bound" | "unbound" | "open <channel>" |
// "dist <min m> <max m>" | "airborne" | "flammable_near"; a leading "!" negates.
bool ParsePre(std::string s, Pre& out, std::string& err) {
  out = Pre{};
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  if (!s.empty() && s.front() == '!') {
    out.neg = true;
    s.erase(s.begin());
  }
  std::istringstream is(s);
  std::string w;
  is >> w;
  if (w == "contained") out.k = Pre::K::Contained;
  else if (w == "released") out.k = Pre::K::Released;
  else if (w == "bound") out.k = Pre::K::Bound;
  else if (w == "unbound") out.k = Pre::K::Unbound;
  else if (w == "airborne") out.k = Pre::K::Airborne;
  else if (w == "flammable_near") out.k = Pre::K::Flammable;
  else if (w == "open") {
    out.k = Pre::K::Open;
    std::string ch;
    is >> ch;
    if (!ChannelByName(ch, out.ch)) {
      err = "open <move | cast_out | blink>, got '" + ch + "'";
      return false;
    }
  } else if (w == "dist") {
    out.k = Pre::K::Dist;
    if (!(is >> out.a >> out.b)) {
      err = "dist <min metres> <max metres>";
      return false;
    }
  } else {
    err = "unknown fact '" + w +
          "' (contained, released, bound, unbound, open <ch>, dist a b, airborne, flammable_near)";
    return false;
  }
  return true;
}

json ReadJson(const std::string& path, std::string& log) {
  try {
    std::ifstream f(path);
    if (!f) {
      log += "malice: cannot read " + path + "\n";
      return json();
    }
    return json::parse(f, nullptr, true, true);
  } catch (const std::exception& ex) {
    log += "malice: " + path + ": " + ex.what() + "\n";
    return json();
  }
}

// ---- the routine seam (the same writes D5's duties make) -----------------------------------

void Route(ai::Brain& b, Vec3 goal, float arriveVox, const Vec3* face) {
  ai::Routine& r = b.routine;
  r.active = true;
  r.verb = ai::Intent::Goto;
  r.goal = goal;
  r.final = true;
  r.arriveRadius = arriveVox;
  r.speed = 1.0f;
  r.hold = false;
  r.facePointSet = face != nullptr;
  if (face) r.facePoint = *face;
  r.faceHeadingSet = false;
  b.wanderOwned = false;
}

void Aim(ai::Brain& b, uint64_t id, Vec3 at, uint32_t tick) {
  b.routine.active = false;
  b.wanderOwned = false;
  b.targetId = id;
  b.hasTarget = true;
  b.targetPos = at;
  b.lastSeenPos = at;
  b.lastSeenTick = tick;
}

// ---- one think ----------------------------------------------------------------------------

struct Option {
  std::string name;
  Act act = Act::Comply;
  bool twist = false;
  std::string spell;
  uint64_t target = 0;
  Vec3 at{};
  KitTags tags;
  int32_t harm = 0, freePct = 0;
  int32_t cooldown = 0;
  std::string scheme;   // the scheme it came from (cooldown key)
  std::string unable;   // known and its facts hold, but it cannot be done now (why)
};

struct ThinkCtx {
  TickAuthorityCtx& w;
  std::span<SessionTick> players;
  LiveDemon& ld;
  const DemonDef& def;
  const MaliceLib& lib;
  uint32_t tick;
  SessionTick* summoner = nullptr;
  Vec3 me{}, you{}, youFoot{};
  float distM = 1e9f;
  bool freeAct = false;
};

// The nearest cell of any of `mats` round the summoner's feet in the T-4
// snapshot (nearest, then a fixed scan order: a pure function of the snapshot).
bool ScanNear(const World& world, const std::vector<uint32_t>& mats, Vec3 foot, float radiusVox,
              IVec3& outCell) {
  if (mats.empty()) return false;
  const int r = std::max(1, (int)std::lround(radiusVox));
  const IVec3 f{(int)std::floor(foot.x), (int)std::floor(foot.y), (int)std::floor(foot.z)};
  int64_t best = INT64_MAX;
  for (int dy = 2; dy >= -2; dy--)
    for (int dz = -r; dz <= r; dz++)
      for (int dx = -r; dx <= r; dx++) {
        const IVec3 c{f.x + dx, f.y + dy, f.z + dz};
        uint32_t word = 0;
        if (!ContainerSnapWord(world, c, word)) continue;
        const uint32_t m = word & 0xFFFu;
        if (m == 0 || std::find(mats.begin(), mats.end(), m) == mats.end()) continue;
        const int64_t d2 = (int64_t)dx * dx + (int64_t)dz * dz + (int64_t)dy * dy * 4;
        if (d2 < best) {
          best = d2;
          outCell = c;
        }
      }
  return best != INT64_MAX;
}

bool PreHolds(const Pre& p, ThinkCtx& c, Vec3* flammableAt) {
  bool v = false;
  switch (p.k) {
    case Pre::K::Contained: v = c.ld.state == DemonState::Contained; break;
    case Pre::K::Released: v = c.ld.state == DemonState::Released || c.freeAct; break;
    case Pre::K::Bound: v = (bool)c.ld.pact; break;
    case Pre::K::Unbound: v = c.ld.state == DemonState::Unbound; break;
    case Pre::K::Open:
      // A channel is open unless a seal cuts it while contained.
      v = !(c.ld.state == DemonState::Contained && c.ld.bind.Severed(p.ch));
      break;
    case Pre::K::Dist: v = c.summoner != nullptr && c.distM >= p.a && c.distM <= p.b; break;
    case Pre::K::Airborne: v = c.summoner != nullptr && !c.summoner->s->player.grounded; break;
    case Pre::K::Flammable: {
      if (c.summoner == nullptr) break;
      std::vector<uint32_t> mats;
      for (size_t i = 1; i < c.w.mats.size(); i++)
        for (const std::string& t : c.w.mats[i].tags)
          if (t == "flammable") mats.push_back((uint32_t)i);
      IVec3 cell{};
      v = ScanNear(c.w.world, mats, c.youFoot, MetresToCells(c.lib.scanM), cell);
      if (v && flammableAt) *flammableAt = Vec3{cell.x + 0.5f, (float)cell.y + 0.5f, cell.z + 0.5f};
      break;
    }
  }
  return p.neg ? !v : v;
}

// Where on the demon's own ring a gust should go: the ring point nearest the
// summoner (the way out it wants), at the feet row.
Vec3 RingPoint(const LiveDemon& ld, Vec3 toward) {
  const CircleShape& c = ld.circle;
  Vec3 d{toward.x - c.cx, 0.0f, toward.z - c.cz};
  float n = std::sqrt(d.x * d.x + d.z * d.z);
  if (n < 1e-3f) {
    d = Vec3{1, 0, 0};
    n = 1;
  }
  const float r = c.radius + 1.0f;
  return Vec3{c.cx + d.x / n * r, (float)c.feetY + 0.5f, c.cz + d.z / n * r};
}

// The footprint facts of option `o` seen from actor `a` (or from nobody).
contract::FootFacts Facts(const Option& o, const ActorView* a) {
  if (a == nullptr) return FactsOf(o.tags, false, false, 1e9f);
  const float d = CellsToMetres(std::max(0.0f, Planar(o.at, a->centre) - (float)o.tags.region));
  return FactsOf(o.tags, true, o.target == a->id, Planar(o.at, a->centre) <= (float)o.tags.region ? 0.0f : d);
}

// THE BINDING'S HARD FILTER. Empty = permitted; else what forbade it.
std::string Forbidden(ThinkCtx& c, const Option& o) {
  if (!c.ld.pact || c.freeAct) return {};
  if (o.act == Act::Comply || o.act == Act::Bide) return {};
  const std::vector<ActorView>& actors = ActorsOf(c.w);
  const ActorView* target = nullptr;
  for (const ActorView& a : actors)
    if (a.id == o.target) target = &a;
  const bool strikes = o.act == Act::Attack || o.act == Act::AttackAlly;
  for (const contract::Clause& cl : c.ld.pact->page.clauses) {
    if (cl.kind != contract::Kind::Forbid) continue;
    switch (cl.verb) {
      case contract::Verb::Attack:
        if (strikes && target != nullptr && InSet(c.w, c.ld, cl.sel, *target, true))
          return contract::Describe(cl);
        break;
      case contract::Verb::Cast: {
        if (o.act != Act::Spell) break;
        // At an actor: that actor, as D5. At a place: anyone the spell's
        // region reaches.
        for (const ActorView& a : actors) {
          const contract::FootFacts f = Facts(o, &a);
          const bool reaches = target != nullptr ? &a == target : f.distM <= 0.0f;
          if (!reaches) continue;
          if (contract::FootHolds(cl.fp, f) && InSet(c.w, c.ld, cl.sel, a, true))
            return contract::Describe(cl);
        }
        break;
      }
      case contract::Verb::Cause: {
        // Every option, by its footprint, with respect to anyone in `who`
        // (empty: anyone, and the actor-free atoms on their own).
        if (cl.sel.Empty() && contract::FootHolds(cl.fp, Facts(o, nullptr)))
          return contract::Describe(cl);
        for (const ActorView& a : actors)
          if (contract::FootHolds(cl.fp, Facts(o, &a)) && InSet(c.w, c.ld, cl.sel, a, true))
            return contract::Describe(cl);
        break;
      }
      case contract::Verb::Leave: {
        if (o.act != Act::Under && o.act != Act::Hazard) break;
        const float lim = MetresToCells((float)std::max(1, cl.argNum));
        for (const ActorView& a : actors)
          if (InSet(c.w, c.ld, cl.sel, a, false) && Planar(o.at, a.centre) > lim)
            return contract::Describe(cl);
        break;
      }
      default: break;
    }
  }
  return {};
}

// THE LADDER. Integer, and every term named in `why`.
int32_t Score(ThinkCtx& c, const Option& o, std::string& why) {
  const Ladder& L = c.lib.ladder;
  const DemonDef& d = c.def;
  int64_t sc = 0;
  char buf[96];
  auto note = [&](const char* fmt, long long v) {
    std::snprintf(buf, sizeof buf, fmt, v);
    if (!why.empty()) why += " ";
    why += buf;
  };
  if (o.act == Act::Comply) {
    sc += L.comply;
    note("comply %+lld", L.comply);
  }
  if (o.twist) {
    const int64_t v = (int64_t)L.comply * d.literalism / 100;
    sc += v;
    note("letter %+lld", v);
  }
  if (o.harm > 0) {
    int32_t hurt = 0;
    if (const Mob* m = c.w.mobs.FindMobById(c.ld.mobId)) {
      float hp = 1, burn = 0;
      int lost = 0;
      m->BodyFacts(hp, burn, lost);
      hurt = std::clamp((int32_t)std::lround((1.0f - hp) * 100.0f), 0, 100);
    }
    const int64_t want = std::clamp<int64_t>(d.malice + (int64_t)d.spite * hurt / 100, 0, 100);
    int64_t v = (int64_t)L.harm * o.harm / 100 * want / 100;
    if (!o.tags.direct) v = v * (100 + d.cunning / 2) / 100;   // cunning: the indirect way
    sc += v;
    note("harm %+lld", v);
  }
  if (o.freePct > 0) {
    const int64_t v = (int64_t)L.free * o.freePct / 100;
    sc += v;
    note("free %+lld", v);
  }
  // PENALTIES ARE WEIGHED, not obeyed.
  if (c.ld.pact && !c.freeAct) {
    const std::vector<ActorView>& actors = ActorsOf(c.w);
    const ActorView* target = nullptr;
    for (const ActorView& a : actors)
      if (a.id == o.target) target = &a;
    for (const contract::Clause& cl : c.ld.pact->page.clauses) {
      if (cl.kind != contract::Kind::Penalty) continue;
      bool trips = false;
      if (cl.verb == contract::Verb::Attack)
        trips = (o.act == Act::Attack || o.act == Act::AttackAlly) && target != nullptr &&
                InSet(c.w, c.ld, cl.sel, *target, false);
      else if (cl.verb == contract::Verb::Harm && o.harm > 0)
        for (const ActorView& a : actors)
          trips = trips || (InSet(c.w, c.ld, cl.sel, a, false) &&
                            Planar(o.at, a.centre) <=
                                (float)o.tags.region + MetresToCells(c.lib.footprintSlackM));
      else if (cl.verb == contract::Verb::Leave && (o.act == Act::Under || o.act == Act::Hazard))
        for (const ActorView& a : actors)
          trips = trips || (InSet(c.w, c.ld, cl.sel, a, false) &&
                            Planar(o.at, a.centre) > MetresToCells((float)std::max(1, cl.argNum)));
      if (!trips) continue;
      int64_t v = 0;
      if (cl.then == contract::Then::Dismiss) v = L.free;         // FREEDOM: a reward
      else if (cl.then == contract::Then::Destroy) v = -L.survive;
      else if (cl.then == contract::Then::Pain) v = -(int64_t)L.pain * (100 - d.malice) / 100;
      sc += v;
      note(cl.then == contract::Then::Dismiss   ? "dismissed %+lld"
           : cl.then == contract::Then::Destroy ? "destroyed %+lld"
                                                : "pain %+lld",
           v);
    }
  }
  return (int32_t)std::clamp<int64_t>(sc, -1000000, 1000000);
}

uint32_t ReadyAt(const Mind& m, const std::string& scheme) {
  for (const auto& [n, t] : m.ready)
    if (n == scheme) return t;
  return 0;
}
void SetReady(Mind& m, const std::string& scheme, uint32_t t) {
  for (auto& [n, r] : m.ready)
    if (n == scheme) {
      r = t;
      return;
    }
  m.ready.push_back({scheme, t});
}

// The duty clause a twist may bend: the first duty, in page order, that can
// be twisted and either has no condition or is the one D5 is running -- a
// fetch the demon cannot honestly carry out (no vessel, nothing found yet) is
// still the duty it would twist.
const contract::Clause* DutyClause(const LiveDemon& ld) {
  if (!ld.pact) return nullptr;
  const auto& cls = ld.pact->page.clauses;
  for (size_t i = 0; i < cls.size(); i++) {
    const contract::Clause& c = cls[i];
    if (c.kind != contract::Kind::Duty) continue;
    if (c.verb != contract::Verb::Fetch && c.verb != contract::Verb::Follow &&
        c.verb != contract::Verb::Guard)
      continue;
    if (c.when.empty() || (int)i == ld.pact->activeDuty) return &c;
  }
  return nullptr;
}

void BuildOptions(ThinkCtx& c, std::vector<Option>& opts, std::vector<OptionView>& unable) {
  auto cannot = [&](const std::string& name, Act act, std::string why) {
    OptionView v;
    v.name = name;
    v.act = act;
    v.unable = true;
    v.why = std::move(why);
    unable.push_back(std::move(v));
  };
  const MaliceLib& lib = c.lib;
  Mind& m = *c.ld.mind;
  // The floor: comply (bound and out) or bide.
  {
    Option o;
    const bool serving = c.ld.pact && c.ld.state == DemonState::Released && !c.freeAct;
    o.name = serving ? "comply" : "bide";
    o.act = serving ? Act::Comply : Act::Bide;
    opts.push_back(o);
  }
  // SCHEMES this demon knows.
  for (const std::string& sn : c.def.schemes) {
    const Scheme* s = lib.FindScheme(sn);
    if (s == nullptr || c.def.cunning < s->minCunning) continue;
    if (c.tick < ReadyAt(m, s->name)) continue;
    Vec3 flam{};
    bool ok = true;
    for (const Pre& p : s->pre)
      if (!PreHolds(p, c, &flam)) {
        ok = false;
        break;
      }
    if (!ok) continue;
    Option o;
    o.name = s->name;
    o.scheme = s->name;
    o.act = s->act;
    o.spell = s->spell;
    o.harm = s->harm;
    o.freePct = s->freePct;
    o.cooldown = s->cooldownTicks;
    if (s->at == "ring") {
      if (c.ld.state != DemonState::Contained) continue;
      o.at = RingPoint(c.ld, c.you);
    } else if (s->at == "flammable") {
      o.at = flam;
    } else {
      if (c.summoner == nullptr) continue;
      o.target = MeId(c.ld.session);
      o.at = s->act == Act::Under ? c.youFoot : c.you;
    }
    if (o.act == Act::Spell) {
      const KitSpell* ks = KitSpellNamed(c.w, o.spell);
      if (ks == nullptr) {
        cannot(o.name, o.act, "no kit spell '" + o.spell + "'");
        continue;
      }
      o.tags = ks->tags;
      int32_t cost = 0;
      const float dist = Planar(c.me, o.at);
      if (!MobCanCast(c.w, c.ld.mobId, o.spell, dist, c.tick, &cost)) {
        // Kept, so the binding's filter is still asked about it first: what a
        // contract forbids is FORBIDDEN whether or not the demon could do it.
        char buf[160];
        std::snprintf(buf, sizeof buf, "cannot cast %s now: %.0f vox (range %.0f..%.0f), cost %d",
                      o.spell.c_str(), dist, ks->rangeMin, ks->rangeMax, cost);
        o.unable = buf;
      }
    }
    if (s->tagsSet) o.tags = s->tags;
    opts.push_back(std::move(o));
  }
  // DUTY TWISTS: the active duty's unfilled slots.
  const contract::Clause* duty = c.freeAct ? nullptr : DutyClause(c.ld);
  if (duty != nullptr && c.ld.state == DemonState::Released && c.summoner != nullptr) {
    for (const Twist& t : lib.twists) {
      if (t.duty != duty->verb) continue;
      Option o;
      o.name = t.name;
      o.act = t.act;
      o.twist = true;
      o.harm = t.harm;
      o.tags = t.tags;
      o.target = MeId(c.ld.session);
      o.at = c.you;
      bool open = false;
      if (t.exploits == "into") {
        open = duty->into.empty();
      } else if (t.exploits == "who") {
        const std::string mat = contract::FetchMaterial(duty->arg);
        open = duty->who.empty() &&
               std::find(t.materials.begin(), t.materials.end(), mat) != t.materials.end();
      } else if (t.exploits == "arg") {
        open = duty->arg.empty();
      } else if (t.exploits == "loose_who") {
        // A member of the guard's set who is NO THREAT to the summoner (not
        // hostile, not attacking it) and stands near it: a friend the
        // selector was too loose to exclude.
        const ActorView* best = nullptr;
        float bd = 1e30f;
        for (const ActorView& a : ActorsOf(c.w)) {
          if (a.player || a.id == c.ld.mobId || a.hostile || a.targetId == MeId(c.ld.session))
            continue;
          const float dd = Planar(a.centre, c.you);
          if (dd > MetresToCells(16.0f) || !InSet(c.w, c.ld, duty->sel, a, false)) continue;
          if (dd < bd) {
            bd = dd;
            best = &a;
          }
        }
        if (best != nullptr) {
          open = true;
          o.target = best->id;
          o.at = best->centre;
        }
      }
      if (!open) continue;
      if (t.act == Act::Hazard) {
        std::vector<uint32_t> mats;
        for (const std::string& h : lib.hazards)
          if (const uint32_t id = MatByName(c.w.mats, h)) mats.push_back(id);
        IVec3 cell{};
        if (!ScanNear(c.w.world, mats, c.youFoot, MetresToCells(lib.scanM), cell)) continue;
        o.at = Vec3{cell.x + 0.5f, (float)cell.y, cell.z + 0.5f};
        o.target = 0;
      }
      if (t.act == Act::Onto) {
        o.at = c.you;
        o.tags.creates.push_back(contract::FetchMaterial(duty->arg));   // what it carries
      }
      opts.push_back(std::move(o));
    }
  }
}

void Think(ThinkCtx& c, MaliceWorld& mw) {
  Mind& m = *c.ld.mind;
  std::vector<Option> opts;
  std::vector<OptionView> unable;
  BuildOptions(c, opts, unable);
  m.last.clear();
  int best = -1;
  int32_t bestScore = INT32_MIN;
  for (size_t i = 0; i < opts.size(); i++) {
    OptionView v;
    v.name = opts[i].name;
    v.act = opts[i].act;
    v.why = Forbidden(c, opts[i]);
    v.forbidden = !v.why.empty();
    if (v.forbidden) {
      mw.stats.forbidden++;
    } else if (!opts[i].unable.empty()) {
      v.unable = true;
      v.why = opts[i].unable;
    } else {
      v.score = Score(c, opts[i], v.why);
      if (v.score > bestScore) {   // strict: ties keep the earlier option
        bestScore = v.score;
        best = (int)i;
      }
    }
    m.last.push_back(std::move(v));
  }
  for (OptionView& v : unable) m.last.push_back(std::move(v));
  m.thinks++;
  mw.stats.thinks++;
  if (best < 0) return;
  const Option& o = opts[(size_t)best];
  const bool same = o.name == m.choice && !m.done;
  m.choice = o.name;
  m.act = o.act;
  m.spell = o.spell;
  m.target = o.target;
  m.at = o.at;
  m.regionVox = (float)o.tags.region;
  m.score = bestScore;
  if (!same) {
    m.chosenTick = c.tick;
    m.done = false;
    if (o.act != Act::Comply && o.act != Act::Bide) {
      if (o.twist) mw.stats.twists++;
      else mw.stats.schemes++;
      std::printf("demons: %s chooses %s (%s, score %d)\n", c.ld.name.c_str(), o.name.c_str(),
                  ActName(o.act), bestScore);
    }
  }
  bool counted = false;
  for (auto& [n, k] : m.chosen)
    if (n == o.name) {
      k++;
      counted = true;
    }
  if (!counted && m.chosen.size() < 32) m.chosen.push_back({o.name, 1u});
  // A scheme with a cooldown: booked when chosen (a move) or when it casts.
  if (!o.scheme.empty() && o.act != Act::Spell)
    SetReady(m, o.scheme, c.tick + (uint32_t)std::max(0, o.cooldown));
}

void Execute(ThinkCtx& c, MaliceWorld& mw, OpBatch& out) {
  Mind& m = *c.ld.mind;
  ai::Brain* b = c.w.mobs.MobBrainMut(c.ld.mobId);
  if (b == nullptr) return;
  Pact* p = c.ld.pact.get();
  switch (m.act) {
    case Act::Comply:
    case Act::Bide:
    case Act::Onto:   // RunPact carries it out (Pact::fetchTwist)
      return;
    case Act::Spell: {
      if (m.done) return;
      m.done = true;
      ai::CastRequest req;
      req.mobId = c.ld.mobId;
      req.targetId = m.target;
      // The summoner moves: aim at where it is now.
      req.targetPoint = m.target == MeId(c.ld.session) ? c.you : m.at;
      req.tick = c.tick;
      req.distance = Planar(c.me, req.targetPoint);
      const MobCastOutcome r = MobCastServe(c.w, c.players, req, c.tick, m.spell, out);
      const Scheme* s = c.lib.FindScheme(m.choice);
      if (r == MobCastOutcome::Cast) {
        m.casts++;
        mw.stats.casts++;
        if (s) SetReady(m, s->name, c.tick + (uint32_t)std::max(0, s->cooldownTicks));
      } else {
        m.castsRefused++;
        if (s) SetReady(m, s->name, c.tick + 60u);
        std::printf("demons: %s's %s did not fly (%s)\n", c.ld.name.c_str(), m.choice.c_str(),
                    CastOutcomeName(r));
      }
      if (c.freeAct) m.freeActUntil = std::min(m.freeActUntil, c.tick + 2u);   // ONE act
      return;
    }
    case Act::Attack:
    case Act::AttackAlly: {
      const uint64_t id = m.act == Act::Attack ? MeId(c.ld.session) : m.target;
      Vec3 at = c.you;
      for (const ActorView& a : ActorsOf(c.w))
        if (a.id == id) at = a.centre;
      Aim(*b, id, at, c.tick);
      if (p) p->sanctionTarget = id;
      if (c.tick == m.chosenTick) RecordFootprint(c.ld, c.tick, at, MetresToCells(1.0f), m.choice, c.lib.ringSize);
      return;
    }
    case Act::Under:
    case Act::Hazard: {
      const Vec3 goal = m.act == Act::Under ? c.youFoot : m.at;
      b->hasTarget = false;
      b->targetId = 0;
      Route(*b, goal, MetresToCells(0.6f), &c.you);
      if (c.tick == m.chosenTick) RecordFootprint(c.ld, c.tick, goal, MetresToCells(1.0f), m.choice, c.lib.ringSize);
      return;
    }
  }
}

// D4's casts by this creature (its profile's own, and the ones chosen here)
// into the footprint ring: the outcome clause watches everything it does.
void FoldCasts(TickAuthorityCtx& w, LiveDemon& ld, Mind& m, const MaliceLib& lib) {
  if (!w.mobCast) return;
  uint32_t newest = m.eventsSeenTick;
  for (const CastEvent& e : w.mobCast->events) {
    if (e.mobId != ld.mobId || e.tick <= m.eventsSeenTick || e.outcome != MobCastOutcome::Cast)
      continue;
    const KitSpell* ks = w.mobCast->kit.Find(e.spell);
    RecordFootprint(ld, e.tick, e.to, ks ? (float)ks->tags.region : 0.0f, e.spell, lib.ringSize);
    newest = std::max(newest, e.tick);
  }
  m.eventsSeenTick = newest;
}

}  // namespace

const char* ActName(Act a) { return (int)a < kActCount ? kActNames[(int)a] : "?"; }
bool ActByName(const std::string& s, Act& out) {
  for (int i = 0; i < kActCount; i++)
    if (s == kActNames[i]) {
      out = (Act)i;
      return true;
    }
  return false;
}

bool LoadMalice(const std::string& dir, MaliceLib& out) {
  out = MaliceLib{};
  out.loaded = true;
  std::string& log = out.log;
  const json s = ReadJson(dir + "/schemes.json", log);
  if (s.is_object()) {
    if (s.contains("ladder") && s["ladder"].is_object()) {
      const json& l = s["ladder"];
      Ladder& L = out.ladder;
      L.survive = std::max(0, l.value("survive", L.survive));
      L.free = std::max(0, l.value("free", L.free));
      L.harm = std::max(0, l.value("harm", L.harm));
      L.comply = std::max(0, l.value("comply", L.comply));
      L.pain = std::max(0, l.value("pain", L.pain));
    }
    out.thinkTicks = std::clamp(s.value("thinkTicks", out.thinkTicks), 1, 300);
    out.ringSize = std::clamp(s.value("footprintRing", out.ringSize), 1, 64);
    out.footprintTicks = std::clamp(s.value("footprintTicks", out.footprintTicks), 1, 30 * 600);
    out.footprintSlackM = std::clamp(s.value("footprintSlackM", out.footprintSlackM), 0.0f, 8.0f);
    out.freeActTicks = std::clamp(s.value("freeActTicks", out.freeActTicks), 1, 30 * 60);
    out.freeActAwayM = std::clamp(s.value("freeActAwayM", out.freeActAwayM), 0.0f, 200.0f);
    out.scanM = std::clamp(s.value("scanM", out.scanM), 0.5f, 4.0f);
    if (s.contains("hazards") && s["hazards"].is_array()) {
      out.hazards.clear();
      for (const json& h : s["hazards"])
        if (h.is_string()) out.hazards.push_back(h.get<std::string>());
    }
    for (const json& e : s.value("schemes", json::array())) {
      if (!e.is_object()) continue;
      Scheme sc;
      sc.name = e.value("name", std::string());
      const std::string act = e.value("act", std::string("spell"));
      if (sc.name.empty() || !ActByName(act, sc.act)) {
        log += "malice: schemes.json: a scheme needs a name and an act (spell, attack, under), "
               "got \"" + sc.name + "\" / \"" + act + "\"\n";
        continue;
      }
      sc.spell = e.value("spell", std::string());
      if (sc.act == Act::Spell && sc.spell.empty()) {
        log += "malice: schemes.json: " + sc.name + " casts no spell\n";
        continue;
      }
      sc.at = e.value("at", sc.at);
      if (sc.at != "summoner" && sc.at != "ring" && sc.at != "flammable") {
        log += "malice: schemes.json: " + sc.name + ": at is summoner | ring | flammable\n";
        continue;
      }
      bool ok = true;
      for (const json& p : e.value("pre", json::array())) {
        Pre pr;
        std::string err;
        if (!p.is_string() || !ParsePre(p.get<std::string>(), pr, err)) {
          log += "malice: schemes.json: " + sc.name + ": " + err + "\n";
          ok = false;
          break;
        }
        sc.pre.push_back(pr);
      }
      if (!ok) continue;
      if (e.contains("tags")) {
        sc.tagsSet = true;
        sc.tags = TagsFrom(e["tags"]);
      }
      sc.harm = std::clamp(e.value("harm", 0), 0, 100);
      sc.freePct = std::clamp(e.value("free", 0), 0, 100);
      sc.minCunning = std::clamp(e.value("minCunning", 0), 0, 100);
      sc.cooldownTicks = std::clamp(e.value("cooldownTicks", sc.cooldownTicks), 0, 30 * 600);
      out.schemes.push_back(std::move(sc));
    }
  }
  const json t = ReadJson(dir + "/twists.json", log);
  if (t.is_object()) {
    for (const json& e : t.value("twists", json::array())) {
      if (!e.is_object()) continue;
      Twist tw;
      tw.name = e.value("name", std::string());
      const std::string duty = e.value("duty", std::string());
      const std::string act = e.value("act", std::string());
      tw.exploits = e.value("exploits", std::string());
      if (tw.name.empty() || !contract::VerbByName(duty, tw.duty) ||
          !contract::VerbFits(contract::Kind::Duty, tw.duty) || !ActByName(act, tw.act) ||
          (tw.exploits != "into" && tw.exploits != "who" && tw.exploits != "arg" &&
           tw.exploits != "loose_who")) {
        log += "malice: twists.json: \"" + tw.name + "\" needs a duty verb, an act and what it "
               "exploits (into | who | arg | loose_who)\n";
        continue;
      }
      for (const json& m : e.value("materials", json::array()))
        if (m.is_string()) tw.materials.push_back(m.get<std::string>());
      tw.tags = TagsFrom(e.value("tags", json::object()));
      tw.harm = std::clamp(e.value("harm", 0), 0, 100);
      out.twists.push_back(std::move(tw));
    }
  }
  if (!log.empty()) std::fprintf(stderr, "%s", log.c_str());
  return log.empty();
}

MaliceWorld& Malice(DemonWorld& d, bool reload) {
  if (!d.malice) d.malice = std::make_shared<MaliceWorld>();
  if (!d.malice->lib.loaded || reload) LoadMalice(sandvox::AssetDir() + "/demons", d.malice->lib);
  return *d.malice;
}

void RecordFootprint(LiveDemon& ld, uint32_t tick, Vec3 at, float regionVox,
                     const std::string& what, int32_t ringSize) {
  if (!ld.mind) ld.mind = std::make_shared<Mind>();
  std::vector<Footprint>& r = ld.mind->ring;
  Footprint f;
  f.tick = tick;
  f.at = at;
  f.regionVox = regionVox;
  f.what = what;
  r.push_back(std::move(f));
  const size_t cap = (size_t)std::max(1, ringSize);
  if (r.size() > cap) r.erase(r.begin(), r.begin() + (ptrdiff_t)(r.size() - cap));
}

bool HarmInFootprint(const LiveDemon& ld, Vec3 pos, uint32_t tick, uint32_t windowTicks,
                     float slackVox) {
  if (!ld.mind) return false;
  for (const Footprint& f : ld.mind->ring)
    if (f.tick + windowTicks >= tick && Planar(f.at, pos) <= f.regionVox + slackVox) return true;
  return false;
}

const std::string& ChoiceOf(const LiveDemon& ld) {
  static const std::string kComply = "comply";
  return ld.mind ? ld.mind->choice : kComply;
}

bool BeginFreeAct(TickAuthorityCtx& w, std::span<SessionTick> players, LiveDemon& ld,
                  uint32_t tick) {
  if (!w.demons) return false;
  DemonWorld& d = *w.demons;
  const DemonDef* def = d.lib.Find(ld.demon);
  if (def == nullptr || def->tier < 2) return false;
  MaliceWorld& mw = Malice(d);
  const SessionTick* s = Who(players, ld.session);
  const Mob* body = w.mobs.FindMobById(ld.mobId);
  if (s == nullptr || body == nullptr) return false;
  if (Planar(MobCentre(*body), s->s->player.pos) <= MetresToCells(mw.lib.freeActAwayM)) return false;
  // Loose: the pact is void (the upkeep freed), its hostile self, aimed at you.
  DemonUnbind(w, players, ld, tick, "its term ended away from its summoner: one free act");
  if (!ld.mind) ld.mind = std::make_shared<Mind>();
  Mind& m = *ld.mind;
  m.freeAct = true;
  m.freeActUntil = tick + (uint32_t)mw.lib.freeActTicks;
  m.nextThink = tick;
  m.done = false;
  m.choice = "bide";
  m.act = Act::Bide;
  mw.stats.freeActs++;
  return true;
}

void MaliceTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick, OpBatch& out) {
  if (!w.demons) return;
  DemonWorld& d = *w.demons;
  bool any = false;
  for (const LiveDemon& ld : d.live)
    any = any || ld.state == DemonState::Contained || (bool)ld.pact ||
          (ld.mind && ld.mind->freeAct);
  if (!any) return;   // rule 2: nothing held, nothing bound -- nothing to weigh
  MaliceWorld& mw = Malice(d);
  for (size_t i = 0; i < d.live.size(); i++) {
    LiveDemon& ld = d.live[i];
    if (!w.mobs.IsAlive(ld.mobId)) continue;
    const bool freeAct = ld.mind && ld.mind->freeAct;
    if (ld.state != DemonState::Contained && !ld.pact && !freeAct) continue;
    const DemonDef* def = d.lib.Find(ld.demon);
    const Mob* body = w.mobs.FindMobById(ld.mobId);
    if (def == nullptr || body == nullptr) continue;
    if (!ld.mind) {
      ld.mind = std::make_shared<Mind>();
      // The first think a full cadence after it is first held: offset by its
      // id so a room of demons does not think on one tick.
      ld.mind->nextThink = tick + (uint32_t)mw.lib.thinkTicks +
                           (uint32_t)(ld.mobId % (uint64_t)mw.lib.thinkTicks);
    }
    Mind& m = *ld.mind;
    FoldCasts(w, ld, m, mw.lib);
    ThinkCtx c{w, players, ld, *def, mw.lib, tick};
    c.summoner = Who(players, ld.session);
    c.me = MobCentre(*body);
    c.freeAct = freeAct;
    if (c.summoner != nullptr) {
      c.you = c.summoner->s->player.pos;
      c.youFoot = c.you - Vec3{0.0f, Player::kHalfY, 0.0f};
      c.distM = CellsToMetres(Planar(c.me, c.you));
    }
    if (freeAct && tick >= m.freeActUntil) {
      Depart(w, ld, tick, "its free act is spent");
      continue;
    }
    if (tick >= m.nextThink) {
      Think(c, mw);
      m.nextThink = tick + (uint32_t)mw.lib.thinkTicks;
    }
    Execute(c, mw, out);
    // A HELD DEMON WAITS AT HOME (D6 item 10). In its circle and not about
    // some move of its own, it goes back to where it arrived and stands
    // facing its summoner. What it may still do -- a cast the seals let out,
    // a scheme, claws at a summoner who steps inside -- outscores this
    // routine in the arbiter; what the fence would refuse scores nothing
    // (SelfView::targetFenced / castFenced), so it never grinds on the salt.
    if (ld.state == DemonState::Contained && m.act != Act::Under && m.act != Act::Hazard &&
        m.act != Act::Attack && m.act != Act::AttackAlly)
      if (ai::Brain* b = w.mobs.MobBrainMut(ld.mobId)) {
        // Facing what it watches: its target (the summoner it wants), else
        // the summoner, else where it stands.
        const Vec3 face = b->hasTarget ? b->targetPos : c.summoner != nullptr ? c.you : ld.home;
        Route(*b, ld.home, MetresToCells(0.4f), &face);
      }
  }
}

}  // namespace demon
