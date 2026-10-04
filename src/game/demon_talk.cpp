// demon_talk.cpp — the demon conversation, binding, and what a bound demon
// does. See demon_talk.h.
#include "game/demon_talk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/container.h"
#include "game/demon_cast.h"
#include "game/demon.h"
#include "game/dialogue.h"
#include "game/item.h"
#include "game/mob.h"
#include "game/player.h"
#include "game/session.h"
#include "sim/materials.h"
#include "sim/rng.h"
#include "sim/scale.h"
#include "sim/tickinput.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldio.h"
#include "ui/overlay.h"

namespace demon {

const char* PresentResultName(PresentResult r) {
  switch (r) {
    case PresentResult::None: return "none";
    case PresentResult::Bound: return "bound";
    case PresentResult::Refused: return "refused";
    case PresentResult::CannotHold: return "cannot hold";
    case PresentResult::NoContract: return "no such contract";
    case PresentResult::NotContained: return "not contained";
  }
  return "?";
}

namespace {

using contract::Clause;
using contract::Kind;
using contract::Verb;

TalkWorld& TW(DemonWorld& d) {
  if (!d.talk) d.talk = std::make_shared<TalkWorld>();
  return *d.talk;
}

LiveDemon* Live(TickAuthorityCtx& w, uint64_t id) {
  if (!w.demons || id == 0) return nullptr;
  for (LiveDemon& ld : w.demons->live)
    if (ld.mobId == id) return &ld;
  return nullptr;
}
const LiveDemon* Live(const TickAuthorityCtx& w, uint64_t id) {
  if (!w.demons || id == 0) return nullptr;
  for (const LiveDemon& ld : w.demons->live)
    if (ld.mobId == id) return &ld;
  return nullptr;
}

SessionTick* Who(std::span<SessionTick> players, int session) {
  for (SessionTick& p : players)
    if (p.s->index == session) return &p;
  return nullptr;
}

uint64_t MeId(int session) { return ai::kPlayerActorBase + (uint64_t)session; }

Vec3 MobCentre(const Mob& m) {
  const MobDef* d = m.Def();
  const Vec3 ws = d ? d->worldSize : Vec3{};
  return m.Origin() + ws * 0.5f;
}
Vec3 MobFoot(const Mob& m) {
  const MobDef* d = m.Def();
  const Vec3 ws = d ? d->worldSize : Vec3{};
  return m.Origin() + Vec3{ws.x * 0.5f, 0.0f, ws.z * 0.5f};
}
Vec3 MobHead(const Mob& m) {
  const MobDef* d = m.Def();
  const Vec3 ws = d ? d->worldSize : Vec3{};
  return m.Origin() + Vec3{ws.x * 0.5f, ws.y * 0.85f, ws.z * 0.5f};
}
Vec3 PlayerFoot(const PlayerSession& s) {
  return s.player.pos - Vec3{0.0f, Player::kHalfY, 0.0f};
}
float Planar(Vec3 a, Vec3 b) {
  const float dx = a.x - b.x, dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

UIState& UiOf(TickAuthorityCtx& w, PlayerSession& s) { return s.sink ? s.sink->ui : w.ui; }

const DemonDef* DefOf(const TickAuthorityCtx& w, const LiveDemon& ld) {
  return w.demons ? w.demons->lib.Find(ld.demon) : nullptr;
}

uint32_t TicksPerHour() {
  return std::max<uint32_t>(1u, TicksPerDayFromTuning(CurrentTuning()) / 24u);
}

// ---- the actors (players + live mobs), and set membership ------------------------------

void BuildActors(TickAuthorityCtx& w, std::span<SessionTick> players, TalkWorld& t,
                 uint32_t tick) {
  t.actors.clear();
  const uint32_t fPlayer = ai::FactionId("player");
  for (SessionTick& p : players) {
    ActorView a;
    a.id = MeId(p.s->index);
    a.centre = p.s->player.pos;
    a.faction = fPlayer;
    a.player = true;
    a.type = "player";
    for (const TalkWorld::Strike& st : t.strikes)
      if (st.session == p.s->index && tick - st.tick <= kStrikeMemoryTicks &&
          w.mobs.IsAlive(st.mob))
        a.targetId = st.mob;
    t.actors.push_back(std::move(a));
  }
  for (uint32_t i = 0; i < w.mobs.MobCount(); i++) {
    const Mob* m = w.mobs.MobAt(i);
    if (m == nullptr || !m->Alive() || m->Def() == nullptr) continue;
    ActorView a;
    a.id = m->Id();
    a.centre = MobCentre(*m);
    a.type = m->Def()->name;
    const ai::Brain* b = w.mobs.MobBrain(a.id);
    const ai::Profile* pr = b ? w.mobs.Behaviors().At(b->profile) : nullptr;
    a.faction = pr ? ai::FactionId(pr->faction) : 0u;
    a.hostile = pr != nullptr && pr->perception.aggro == ai::Aggro::Hostile;
    a.targetId = (b != nullptr && b->hasTarget) ? b->targetId : 0;
    t.actors.push_back(std::move(a));
  }
  t.actorsTick = tick;
}

struct Eval {
  const std::vector<ActorView>& actors;
  uint64_t me = 0, self = 0;
  int32_t budget = kEvalBudget;
  bool exhausted = false;

  const ActorView* Find(uint64_t id) const {
    for (const ActorView& a : actors)
      if (a.id == id) return &a;
    return nullptr;
  }
  bool In(const contract::Selector& s, int node, const ActorView& a) {
    if (node < 0 || node >= (int)s.nodes.size()) return false;
    if (--budget < 0) {
      exhausted = true;
      return false;
    }
    const contract::SelNode& n = s.nodes[(size_t)node];
    using contract::SelOp;
    switch (n.op) {
      case SelOp::Me: return a.id == me;
      case SelOp::Self: return a.id == self;
      case SelOp::All: return true;
      case SelOp::AttackedBy:
        // a member of S is attacking `a`
        for (const ActorView& o : actors)
          if (o.targetId == a.id && o.id != a.id && In(s, n.a, o)) return true;
        return false;
      case SelOp::Attacking: {
        if (a.targetId == 0) return false;
        const ActorView* t = Find(a.targetId);
        return t != nullptr && In(s, n.a, *t);
      }
      case SelOp::HostileTo: {
        if (a.targetId != 0) {
          const ActorView* t = Find(a.targetId);
          if (t != nullptr && In(s, n.a, *t)) return true;
        }
        if (!a.hostile) return false;
        for (const ActorView& o : actors)
          if (o.id != a.id && o.faction != a.faction && In(s, n.a, o)) return true;
        return false;
      }
      case SelOp::Faction: return n.name == ai::FactionName(a.faction);
      case SelOp::Type: return a.type == n.name;
      case SelOp::And: return In(s, n.a, a) && In(s, n.b, a);
      case SelOp::Or: return In(s, n.a, a) || In(s, n.b, a);
      case SelOp::Not: return !In(s, n.a, a);
    }
    return false;
  }
  bool Member(const contract::Selector& s, const ActorView& a) { return In(s, s.root, a); }
  // A forbid's set: fail CLOSED (a prohibition the binding cannot evaluate holds).
  bool Forbidden(const contract::Selector& s, const ActorView& a) {
    const bool was = exhausted;
    exhausted = false;
    const bool r = In(s, s.root, a);
    const bool out = r || exhausted;
    exhausted = exhausted || was;
    return out;
  }
};

// ---- triggers ------------------------------------------------------------------------

struct FactCtx {
  TickAuthorityCtx& w;
  const LiveDemon& ld;
  const Pact& pact;
  const SessionTick* summoner;
  const Mob* body;
  uint32_t tick;
};

int32_t FactValue(const contract::Cond& c, FactCtx& f, Eval& ev) {
  using contract::FactKind;
  switch (c.fact) {
    case FactKind::Dist:
      if (f.summoner == nullptr || f.body == nullptr) return 1000000;
      return (int32_t)(Planar(MobCentre(*f.body), f.summoner->s->player.pos) * kVoxelMeters);
    case FactKind::Hp: {
      if (f.summoner == nullptr) return 0;
      float hp = 1, burn = 0;
      int lost = 0;
      f.summoner->s->avatar.BodyFacts(hp, burn, lost);
      return (int32_t)std::lround(hp * 100.0f);
    }
    case FactKind::DemonHp: {
      if (f.body == nullptr) return 0;
      float hp = 1, burn = 0;
      int lost = 0;
      f.body->BodyFacts(hp, burn, lost);
      return (int32_t)std::lround(hp * 100.0f);
    }
    case FactKind::Term:
      if (f.pact.expireTick == 0) return 1000000;
      return f.pact.expireTick > f.tick ? (int32_t)((f.pact.expireTick - f.tick) / 30u) : 0;
    case FactKind::Count: {
      if (f.body == nullptr) return 0;
      const Vec3 c0 = MobCentre(*f.body);
      const float r = MetresToCells(24.0f);
      int32_t n = 0;
      for (const ActorView& a : ev.actors)
        if (a.id != f.ld.mobId && Planar(a.centre, c0) <= r && ev.Member(c.sel, a)) n++;
      return n;
    }
    case FactKind::Counter:
      return (c.counter >= 0 && c.counter < contract::kMaxCounters)
                 ? f.pact.counters[(size_t)c.counter]
                 : 0;
  }
  return 0;
}

bool WhenHolds(const Clause& cl, FactCtx& f, Eval& ev) {
  for (const contract::Cond& c : cl.conds)
    if (!contract::CmpHolds(c.op, FactValue(c, f, ev), c.value)) return false;
  return true;
}

void ApplyEffect(Pact& p, const contract::Effect& e) {
  if (e.counter < 0 || e.counter >= contract::kMaxCounters) return;
  int32_t& v = p.counters[(size_t)e.counter];
  if (e.op == '+') v += e.value;
  else if (e.op == '-') v -= e.value;
  else v = e.value;
  v = std::clamp(v, -1000000, 1000000);
}

// ---- the brain seam --------------------------------------------------------------------

void SetRoutine(ai::Brain& b, Vec3 goal, float arriveVox, float speed, bool hold,
                const Vec3* facePoint, const float* faceHeading) {
  ai::Routine& r = b.routine;
  r.active = true;
  r.verb = ai::Intent::Goto;
  r.goal = goal;
  r.final = true;
  r.arriveRadius = arriveVox;
  r.speed = speed;
  r.hold = hold;
  r.facePointSet = facePoint != nullptr;
  if (facePoint) r.facePoint = *facePoint;
  r.faceHeadingSet = faceHeading != nullptr;
  if (faceHeading) r.faceHeading = *faceHeading;
  b.wanderOwned = false;
}
void ClearRoutine(ai::Brain& b) {
  b.routine.active = false;
  b.wanderOwned = false;
}
void ClearTarget(ai::Brain& b) {
  b.hasTarget = false;
  b.targetId = 0;
}
void SetTarget(ai::Brain& b, const ActorView& a, uint32_t tick) {
  b.targetId = a.id;
  b.hasTarget = true;
  b.targetPos = a.centre;
  b.lastSeenPos = a.centre;
  b.lastSeenTick = tick;
}

// ---- fetch -----------------------------------------------------------------------------

uint32_t MatByName(const std::vector<MaterialDef>& mats, const std::string& n) {
  for (size_t i = 1; i < mats.size(); i++)
    if (mats[i].name == n) return (uint32_t)i;
  return 0;
}

// The summoner's vessel a fetch fills: the hotbar, then the bag; a single
// container that names `into` (or any, when empty) and accepts `mat`.
ItemStack* FetchVessel(TickAuthorityCtx& w, PlayerSession& s, const std::string& into,
                       uint32_t mat, const ItemDef** defOut) {
  Kit& kit = s.kit();
  auto take = [&](ItemStack* slots, int n) -> ItemStack* {
    for (int i = 0; i < n; i++) {
      ItemStack& st = slots[i];
      if (st.Empty() || st.count != 1) continue;
      const ItemDef* d = w.items.Named(st.name);
      if (d == nullptr || !d->IsContainer()) continue;
      if (!into.empty() && d->name != into) continue;
      if (!ContainerAccepts(*d, st, mat, w.mats, nullptr)) continue;
      *defOut = d;
      return &st;
    }
    return nullptr;
  };
  if (ItemStack* st = take(kit.hotbar.slots, kItemSlots)) return st;
  return take(kit.bag.slots, Bag::kSlots);
}

// The nearest cell of `mat` round `at` in the T-4 snapshot mirror (nearest,
// then highest, then z, x: a pure function of the snapshot).
bool FetchScan(const World& world, uint32_t mat, IVec3 at, IVec3& out) {
  int64_t best = INT64_MAX;
  bool found = false;
  for (int dy = 4; dy >= -4; dy--)
    for (int dz = -kFetchScanR; dz <= kFetchScanR; dz++)
      for (int dx = -kFetchScanR; dx <= kFetchScanR; dx++) {
        const IVec3 c{at.x + dx, at.y + dy, at.z + dz};
        uint32_t word = 0;
        if (!ContainerSnapWord(world, c, word) || (word & 0xFFFu) != mat) continue;
        const int64_t d2 = (int64_t)dx * dx + (int64_t)dz * dz + (int64_t)dy * dy * 4;
        if (d2 < best) {
          best = d2;
          out = c;
          found = true;
        }
      }
  return found;
}

// ---- departures and consequences ---------------------------------------------------------

void EndTalkWith(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t mobId) {
  if (w.talk == nullptr) return;
  for (SessionTick& p : players)
    if (p.s->talk.active && p.s->talk.speaker.mobId == mobId) dialogue::End(*w.talk, *p.s);
}

void Fire(TickAuthorityCtx& w, std::span<SessionTick> players, LiveDemon& ld, int clause,
          uint32_t tick) {
  Pact& p = *ld.pact;
  if (clause < 0 || clause >= (int)p.page.clauses.size()) return;
  const Clause& c = p.page.clauses[(size_t)clause];
  p.penaltiesFired++;
  ApplyEffect(p, c.effect);
  std::printf("demons: %s's contract: %s -> %s\n", ld.name.c_str(), contract::Describe(c).c_str(),
              contract::ThenName(c.then));
  switch (c.then) {
    case contract::Then::Dismiss:
      EndTalkWith(w, players, ld.mobId);
      Depart(w, ld, tick, "a penalty dismissed it");
      break;
    case contract::Then::Destroy:
      if (w.demons) TW(*w.demons).stats.destroyed++;
      EndTalkWith(w, players, ld.mobId);
      Depart(w, ld, tick, "a penalty destroyed it");
      break;
    case contract::Then::Pain:
      if (Mob* m = w.mobs.FindMobById(ld.mobId)) {
        std::vector<uint64_t> hs;
        m->AppendBodyHandles(hs);
        if (!hs.empty()) w.mobs.Damage(hs[0], m->TotalHp() * 0.15f, MobCentre(*m));
      }
      break;
    case contract::Then::None: break;
  }
}

// ---- one bound demon's tick -----------------------------------------------------------------

void RunPact(TickAuthorityCtx& w, std::span<SessionTick> players, LiveDemon& ld, TalkWorld& t,
             OpBatch& out, uint32_t tick) {
  Pact& p = *ld.pact;
  p.activeDuty = -1;
  p.guardTarget = 0;
  ai::Brain* b = w.mobs.MobBrainMut(ld.mobId);
  const Mob* body = w.mobs.FindMobById(ld.mobId);
  if (b == nullptr || body == nullptr) return;
  SessionTick* summoner = Who(players, ld.session);
  Eval ev{t.actors, MeId(ld.session), ld.mobId};
  FactCtx fc{w, ld, p, summoner, body, tick};
  const Vec3 me = MobCentre(*body);
  // Who of `sel` is nearest the demon (optionally within `range` of `about`).
  auto nearest = [&](const contract::Selector& sel, const Vec3* about, float range,
                     bool skipForbidden) -> const ActorView* {
    const ActorView* best = nullptr;
    float bd = 1e30f;
    for (const ActorView& a : t.actors) {
      if (a.id == ld.mobId) continue;
      if (about && Planar(a.centre, *about) > range) continue;
      if (!ev.Member(sel, a)) continue;
      if (skipForbidden) {
        bool no = false;
        for (const Clause& c : p.page.clauses)
          if (c.kind == Kind::Forbid && c.verb == Verb::Attack && ev.Forbidden(c.sel, a)) no = true;
        if (no) continue;
      }
      const float d = Planar(a.centre, me);
      if (d < bd) {
        bd = d;
        best = &a;
      }
    }
    return best;
  };
  // ---- PENALTIES on distance (leave) --------------------------------------------------
  for (size_t i = 0; i < p.page.clauses.size() && i < 32; i++) {
    const Clause& c = p.page.clauses[i];
    if (c.kind != Kind::Penalty || c.verb != Verb::Leave || !WhenHolds(c, fc, ev)) continue;
    const ActorView* a = nearest(c.sel, nullptr, 0, false);
    const float lim = MetresToCells((float)std::atoi(c.arg.c_str()));
    const bool away = a == nullptr || Planar(a->centre, me) > lim;
    if (away) {
      Fire(w, players, ld, (int)i, tick);
      if (!ld.pact) return;   // dismissed / destroyed
    }
  }
  // ---- FORBID leave: never further than N m from `who` -----------------------------------
  const ActorView* tether = nullptr;
  for (const Clause& c : p.page.clauses) {
    if (c.kind != Kind::Forbid || c.verb != Verb::Leave || !WhenHolds(c, fc, ev)) continue;
    const ActorView* a = nearest(c.sel, nullptr, 0, false);
    const float lim = MetresToCells((float)std::atoi(c.arg.c_str()));
    if (a != nullptr && Planar(a->centre, me) > lim * 0.8f) {
      tether = a;
      break;
    }
  }
  // ---- DUTIES, first that holds and can act --------------------------------------------------
  const ActorView* guard = nullptr;
  bool routine = false;
  if (tether != nullptr) {
    const Vec3 fp = tether->centre;
    SetRoutine(*b, Vec3{fp.x, fp.y - 8.0f, fp.z}, MetresToCells(1.5f), 1.0f, false, &fp, nullptr);
    routine = true;
  }
  for (size_t i = 0; i < p.page.clauses.size() && !routine && guard == nullptr; i++) {
    const Clause& c = p.page.clauses[i];
    if (c.kind != Kind::Duty || !WhenHolds(c, fc, ev)) continue;
    switch (c.verb) {
      case Verb::Follow: {
        const ActorView* a = nearest(c.sel, nullptr, 0, false);
        if (a == nullptr) break;
        const Vec3 fp = a->centre;
        const Vec3 goal = a->player && summoner && a->id == MeId(ld.session)
                              ? PlayerFoot(*summoner->s)
                              : Vec3{fp.x, fp.y - 8.0f, fp.z};
        SetRoutine(*b, goal, MetresToCells(2.0f), 1.0f, false, &fp, nullptr);
        p.activeDuty = (int)i;
        routine = true;
        break;
      }
      case Verb::Guard: {
        const Vec3 about = summoner ? summoner->s->player.pos : me;
        const ActorView* a = nearest(c.sel, &about, MetresToCells(16.0f), true);
        if (a == nullptr) break;
        guard = a;
        p.activeDuty = (int)i;
        break;
      }
      case Verb::Goto: {
        Vec3 goal = p.anchor;
        float x = 0, y = 0, z = 0;
        if (std::sscanf(c.arg.c_str(), "%f %f %f", &x, &y, &z) == 3) goal = Vec3{x, y, z};
        SetRoutine(*b, goal, MetresToCells(1.0f), 1.0f, false, nullptr, nullptr);
        p.activeDuty = (int)i;
        routine = true;
        if (b->routine.arrived && !p.gotoLatched) {
          // An arrival fires the clause once per trip.
          p.gotoLatched = true;
          p.dutiesDone++;
          ApplyEffect(p, c.effect);
        }
        break;
      }
      case Verb::Spin: {
        const float h = 6.2831853f * (float)(tick % 60u) / 60.0f;
        SetRoutine(*b, MobFoot(*body), MetresToCells(1.0f), 0.0f, true, nullptr, &h);
        p.activeDuty = (int)i;
        routine = true;
        break;
      }
      case Verb::Fetch: {
        if (summoner == nullptr || tick < p.fetchBlockedUntil) break;
        const uint32_t mat = MatByName(w.mats, c.arg);
        const ItemDef* vdef = nullptr;
        ItemStack* vessel = mat ? FetchVessel(w, *summoner->s, c.into, mat, &vdef) : nullptr;
        if (p.fetchPhase == 1) {
          // RETURN: back to the summoner; the duty is done on arrival.
          const Vec3 fp = summoner->s->player.pos;
          SetRoutine(*b, PlayerFoot(*summoner->s), MetresToCells(2.0f), 1.0f, false, &fp,
                     nullptr);
          if (Planar(me, fp) <= MetresToCells(2.5f)) {
            p.fetchPhase = 0;
            p.fetchHave = false;
            p.fetches++;
            p.dutiesDone++;
            ApplyEffect(p, c.effect);
          }
          p.activeDuty = (int)i;
          routine = true;
          break;
        }
        if (vessel == nullptr) break;   // nothing to carry it in: the duty cannot act
        if (!p.fetchHave || tick >= p.fetchScanTick + kFetchRescanTicks) {
          p.fetchScanTick = tick;
          const Vec3 f = MobFoot(*body);
          p.fetchHave = FetchScan(w.world, mat,
                                  IVec3{(int)std::floor(f.x), (int)std::floor(f.y),
                                        (int)std::floor(f.z)},
                                  p.fetchCell);
        }
        if (!p.fetchHave) break;
        const Vec3 cell{(float)p.fetchCell.x + 0.5f, (float)p.fetchCell.y,
                        (float)p.fetchCell.z + 0.5f};
        SetRoutine(*b, cell, MetresToCells(0.4f), 1.0f, false, &cell, nullptr);
        p.activeDuty = (int)i;
        routine = true;
        if (Planar(me, cell) <= MetresToCells(0.6f) && std::fabs(MobFoot(*body).y - cell.y) <= 6) {
          // THE TAKE: the summoner's own scoop path (conditional clears through
          // the queue, a claim the ledger pays into the vessel four ticks on).
          const char* why = nullptr;
          const int n = ContainerScoop(*vdef, *vessel, p.fetchCell,
                                       [&](IVec3 q, uint32_t& wd) {
                                         return ContainerSnapWord(w.world, q, wd);
                                       },
                                       w.world, w.mats, out.cells, &why,
                                       &summoner->s->scoopMemo, tick);
          p.fetchTaken = n;
          if (n > 0) {
            p.fetchPhase = 1;
          } else {
            p.fetchHave = false;
            p.fetchBlockedUntil = tick + kFetchRescanTicks;
          }
        }
        break;
      }
      default: break;
    }
  }
  if (p.activeDuty < 0 || p.page.clauses[(size_t)p.activeDuty].verb != Verb::Goto)
    p.gotoLatched = false;
  if (!routine) ClearRoutine(*b);
  // ---- THE TARGET IS THE DUTIES' ----------------------------------------------------------
  if (guard != nullptr) {
    ClearRoutine(*b);
    SetTarget(*b, *guard, tick);
    p.guardTarget = guard->id;
  } else {
    ClearTarget(*b);
  }
  if (ev.exhausted) p.budgetExhausted++;
}

void RecordStrike(TickAuthorityCtx& w, TalkWorld& t, SessionTick& p, uint32_t tick) {
  // Who the summoner's strike was aimed at: the creature nearest along the
  // look ray within a reach, its body round the line (attacked_by(me)).
  const Vec3 eye = p.s->player.EyePos();
  const Vec3 dir = p.ti.lookFwd.normalized();
  const float reach = MetresToCells(4.0f);
  uint64_t best = 0;
  float bestAlong = 1e30f;
  for (uint32_t i = 0; i < w.mobs.MobCount(); i++) {
    const Mob* m = w.mobs.MobAt(i);
    if (m == nullptr || !m->Alive() || m->Def() == nullptr) continue;
    const Vec3 v = MobCentre(*m) - eye;
    const float along = v.dot(dir);
    if (along <= 0.0f || along > reach) continue;
    const Vec3 perp = v - dir * along;
    const float rad = std::max(m->Def()->worldSize.x, m->Def()->worldSize.y) * 0.5f + 3.0f;
    if (perp.len() > rad) continue;
    if (along < bestAlong) {
      bestAlong = along;
      best = m->Id();
    }
  }
  if (best == 0) return;
  for (TalkWorld::Strike& s : t.strikes)
    if (s.session == p.s->index) {
      s.mob = best;
      s.tick = tick;
      return;
    }
  t.strikes.push_back({p.s->index, best, tick});
}

// The presser's most recent demon matching `pred`, or the one `ref` names.
LiveDemon* PickDemon(DemonWorld& d, int session, uint32_t ref,
                     bool (*pred)(const LiveDemon&)) {
  if (ref != 0) {
    for (LiveDemon& ld : d.live)
      if ((uint32_t)ld.mobId == ref && ld.session == session && pred(ld)) return &ld;
    return nullptr;
  }
  for (auto it = d.live.rbegin(); it != d.live.rend(); ++it)
    if (it->session == session && pred(*it)) return &*it;
  return nullptr;
}

void BeginTalk(TickAuthorityCtx& w, PlayerSession& s, LiveDemon& ld, uint32_t tick,
               int presentResult) {
  if (w.talk == nullptr) return;
  const DemonDef* def = DefOf(w, ld);
  if (def == nullptr || def->dialogue.empty()) {
    std::fprintf(stderr, "demons: %s has no dialogue\n", ld.name.c_str());
    return;
  }
  if (s.talk.active) dialogue::End(*w.talk, s);
  // The entry reads these: which face of the conversation to open on.
  w.talk->flags["demon:bound"] = ld.pact ? 1 : 0;
  w.talk->flags["demon:present"] = presentResult;
  dialogue::Speaker sp;
  sp.mobId = ld.mobId;
  sp.name = ld.name;
  std::string why;
  if (!dialogue::Begin(*w.talk, s, sp, def->dialogue, tick, &why))
    std::fprintf(stderr, "demons: cannot talk to %s: %s\n", ld.name.c_str(), why.c_str());
  else if (w.demons)
    TW(*w.demons).stats.talks++;
}

const contract::Page* ResolvePage(const PlayerSession& s, uint32_t hash) {
  for (const contract::Page* p : PresentablePages(s))
    if (contract::PageHash(p->name) == hash) return p;
  return nullptr;
}

bool IsContained(const LiveDemon& ld) { return ld.state == DemonState::Contained; }
bool IsHeld(const LiveDemon& ld) {
  return ld.state == DemonState::Contained || (ld.pact && ld.state == DemonState::Released);
}

}  // namespace

// ---- the queries ---------------------------------------------------------------------------

int32_t UpkeepFor(const TickAuthorityCtx& w, int session) {
  if (!w.demons) return 0;
  int32_t n = 0;
  for (const LiveDemon& ld : w.demons->live)
    if (ld.pact && ld.session == session) n += ld.pact->upkeep;
  return n;
}

int32_t ContractWeight(const LiveDemon& ld) { return ld.pact ? ld.pact->weight : 0; }

Pact* PactOf(TickAuthorityCtx& w, uint64_t demonId) {
  LiveDemon* ld = Live(w, demonId);
  return ld ? ld->pact.get() : nullptr;
}

bool IsDemon(const TickAuthorityCtx& w, uint64_t mobId) { return Live(w, mobId) != nullptr; }

std::vector<const contract::Page*> PresentablePages(const PlayerSession& s) {
  std::vector<const contract::Page*> out;
  for (const contract::Page& p : s.caster.contracts) out.push_back(&p);
  for (const contract::Page& p : contract::GetContent().stock) out.push_back(&p);
  return out;
}

bool PactAllowBlow(DemonWorld& d, LiveDemon& ld, uint64_t targetId) {
  Pact& p = *ld.pact;
  TalkWorld& t = TW(d);
  const ActorView* a = nullptr;
  for (const ActorView& v : t.actors)
    if (v.id == targetId) a = &v;
  if (targetId == ai::kPlayerActorBase + (uint64_t)ld.session) p.blowsAtSummoner++;
  if (targetId == 0 || a == nullptr) {
    p.blowsUnsanctioned++;
    return false;
  }
  Eval ev{t.actors, ai::kPlayerActorBase + (uint64_t)ld.session, ld.mobId};
  bool forbidden = false;
  for (size_t i = 0; i < p.page.clauses.size(); i++) {
    const Clause& c = p.page.clauses[i];
    if (c.kind == Kind::Forbid && c.verb == Verb::Attack && ev.Forbidden(c.sel, *a))
      forbidden = true;
    // A blow ASKED at a penalty's set: the consequence fires next tick.
    if (c.kind == Kind::Penalty && c.verb == Verb::Attack && p.pendingPenalty < 0 &&
        ev.Member(c.sel, *a))
      p.pendingPenalty = (int)i;
  }
  if (forbidden) {
    p.blowsForbidden++;
    return false;
  }
  if (targetId != p.guardTarget) {
    p.blowsUnsanctioned++;
    return false;
  }
  p.blowsAllowed++;
  for (auto& [id, n] : p.blowsAt)
    if (id == targetId) {
      n++;
      return true;
    }
  if (p.blowsAt.size() < 32) p.blowsAt.push_back({targetId, 1u});
  return true;
}

bool CastTagMatches(const std::string& pred, const KitTags* tags) {
  if (pred.empty() || tags == nullptr) return true;
  auto after = [&](const char* key) -> const char* {
    const size_t n = std::strlen(key);
    return pred.compare(0, n, key) == 0 ? pred.c_str() + n : nullptr;
  };
  if (pred == "direct") return tags->direct;
  if (pred == "affects_body") return tags->affectsBody;
  if (const char* m = after("creates:")) {
    for (const std::string& c : tags->creates)
      if (c == m) return true;
    return false;
  }
  if (const char* a = after("alters:")) return tags->alters == a;
  if (const char* t = after("targets:")) return tags->targets == t;
  return true;   // an unknown predicate cannot be checked: the forbid holds
}

bool AllowCastAt(TickAuthorityCtx& w, uint64_t demonId, uint64_t targetId,
                 const KitTags* tags) {
  LiveDemon* ld = Live(w, demonId);
  if (ld == nullptr || !ld->pact || !w.demons) return true;
  TalkWorld& t = TW(*w.demons);
  const ActorView* a = nullptr;
  for (const ActorView& v : t.actors)
    if (v.id == targetId) a = &v;
  if (a == nullptr) return true;   // not an actor: a point in the world
  Eval ev{t.actors, MeId(ld->session), ld->mobId};
  for (const Clause& c : ld->pact->page.clauses)
    if (c.kind == Kind::Forbid && c.verb == Verb::Cast && CastTagMatches(c.arg, tags) &&
        ev.Forbidden(c.sel, *a))
      return false;
  return true;
}

void Depart(TickAuthorityCtx& w, LiveDemon& ld, uint32_t tick, const char* why) {
  std::printf("demons: %s departs at tick %u: %s\n", ld.name.c_str(), tick, why);
  ld.pact.reset();
  ld.why = why;
  ld.stateTick = tick;
  w.mobs.RemoveMob(ld.mobId);   // gone home; DemonTick forgets it next tick
}

PresentInfo Present(TickAuthorityCtx& w, std::span<SessionTick> players, int session,
                    uint64_t demonId, const contract::Page& page, uint32_t tick) {
  PresentInfo info;
  info.page = page.name;
  LiveDemon* ld = Live(w, demonId);
  if (ld == nullptr || ld->state != DemonState::Contained) {
    info.result = PresentResult::NotContained;
    return info;
  }
  TalkWorld& t = TW(*w.demons);
  t.stats.presented++;
  contract::Page pg = page;
  std::vector<std::string> errs;
  if (!contract::Compile(pg, errs)) {
    info.result = PresentResult::NoContract;
    return info;
  }
  const contract::Content& content = contract::GetContent();
  const DemonDef* def = DefOf(w, *ld);
  info.weight = contract::Weigh(pg, content.tariff).total;
  info.power = def ? def->power : ld->bind.power;
  info.strength = ld->bind.strength;
  info.upkeep = contract::UpkeepFor(info.power, content.tariff);
  info.margin = info.strength - info.power - info.weight;
  int32_t room = 0;
  if (SessionTick* p = Who(players, session))
    room = p->s->caster.mana.EffectiveMax() + (ld->pact ? ld->pact->upkeep : 0);
  if (info.margin < 0) {
    info.result = PresentResult::Refused;
    t.stats.refused++;
  } else if (room < info.upkeep) {
    info.result = PresentResult::CannotHold;
    t.stats.refused++;
  } else {
    info.result = PresentResult::Bound;
    t.stats.bound++;
    auto pact = std::make_shared<Pact>();
    pact->page = std::move(pg);
    pact->weight = info.weight;
    pact->upkeep = info.upkeep;
    pact->bindTick = tick;
    pact->expireTick =
        pact->page.hours > 0 ? tick + (uint32_t)pact->page.hours * TicksPerHour() : 0u;
    if (SessionTick* p = Who(players, session)) pact->anchor = PlayerFoot(*p->s);
    ld->pact = std::move(pact);
    // BOUND: it stops prowling the ring (the def's calm profile, no target);
    // a circle that breaks now still lets it loose (DemonUnbind).
    if (def != nullptr && !def->released.empty()) w.mobs.SetMobBehavior(ld->mobId, def->released);
    if (ai::Brain* b = w.mobs.MobBrainMut(ld->mobId)) ClearTarget(*b);
  }
  std::printf("demons: %s is offered \"%s\" (weight %d, upkeep %d): strength %d vs power %d -> "
              "%s (margin %d)\n",
              ld->name.c_str(), page.name.c_str(), info.weight, info.upkeep, info.strength,
              info.power, PresentResultName(info.result), info.margin);
  for (auto& [s, pi] : t.lastPresent)
    if (s == session) {
      pi = info;
      return info;
    }
  t.lastPresent.push_back({session, info});
  return info;
}

// ---- the tick --------------------------------------------------------------------------------

void TalkPreTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick) {
  if (!w.demons) return;   // nothing was ever summoned in this world
  DemonWorld& d = *w.demons;
  TalkWorld& t = TW(d);
  for (SessionTick& p : players) {
    PlayerSession& s = *p.s;
    TickInput& ti = p.ti;
    if (ti.Pressed(TB_ATTACK) || ti.Pressed(TB_ALT)) RecordStrike(w, t, p, tick);
    // T: talk to your contained demon (in reach).
    if (ti.Pressed(TB_DEMON_TALK) && w.talk != nullptr && !s.talk.active) {
      for (auto it = d.live.rbegin(); it != d.live.rend(); ++it) {
        if (it->session != s.index || it->state != DemonState::Contained) continue;
        const Mob* m = w.mobs.FindMobById(it->mobId);
        if (m == nullptr || Planar(MobCentre(*m), s.player.pos) > MetresToCells(kTalkRangeM))
          continue;
        BeginTalk(w, s, *it, tick, 0);
        break;
      }
    }
    // PRESENT: a page, by hash, to a demon.
    if (ti.Pressed(TB_DEMON_PRESENT) && ti.contractHash != 0) {
      LiveDemon* ld = nullptr;
      if (ti.demonRef == 0 && s.talk.active) ld = Live(w, s.talk.speaker.mobId);
      if (ld == nullptr) ld = PickDemon(d, s.index, ti.demonRef, &IsContained);
      const contract::Page* page = ResolvePage(s, ti.contractHash);
      PresentInfo info;
      if (ld == nullptr) info.result = PresentResult::NotContained;
      else if (page == nullptr) info.result = PresentResult::NoContract;
      else info = Present(w, players, s.index, ld->mobId, *page, tick);
      if (page == nullptr) {
        bool had = false;
        for (auto& [ss, pi] : t.lastPresent)
          if (ss == s.index) {
            pi = info;
            had = true;
          }
        if (!had) t.lastPresent.push_back({s.index, info});
      }
      UiOf(w, s).demonTalk.pickerOpen = false;
      // Reopen the conversation on the demon's answer (its entry reads
      // demon:present -- 1 refused, 2 bound, 3 cannot hold).
      if (ld != nullptr && s.talk.active && s.talk.speaker.mobId == ld->mobId &&
          ld->state == DemonState::Contained)
        BeginTalk(w, s, *ld, tick,
                  info.result == PresentResult::Bound        ? 2
                  : info.result == PresentResult::CannotHold ? 3
                                                             : 1);
    }
    // DISMISS: your demon (bound or contained), from anywhere.
    if (ti.Pressed(TB_DEMON_DISMISS)) {
      LiveDemon* ld = nullptr;
      if (ti.demonRef == 0 && s.talk.active) ld = Live(w, s.talk.speaker.mobId);
      if (ld == nullptr) ld = PickDemon(d, s.index, ti.demonRef, &IsHeld);
      if (ld != nullptr) {
        t.stats.dismissed++;
        EndTalkWith(w, players, ld->mobId);
        Depart(w, *ld, tick, "dismissed");
      }
    }
    // THE GAZE IN CONVERSATION: the command's bit, not the mouse.
    if (s.talk.active) {
      const LiveDemon* ld = Live(w, s.talk.speaker.mobId);
      const Mob* m = ld ? w.mobs.FindMobById(ld->mobId) : nullptr;
      if (ld != nullptr && m != nullptr && ld->state == DemonState::Contained) {
        Vec3 v = MobHead(*m) - s.player.EyePos();
        if (v.len() > 1e-3f) {
          v = v.normalized();
          ti.lookFwd = ti.Held(TB_DEMON_LOOKAWAY) ? Vec3{-v.x, -0.6f, -v.z}.normalized() : v;
        }
      }
    }
  }
}

void ContractTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
                  OpBatch& out) {
  if (!w.demons) {
    if (w.talk != nullptr) w.talk->demonActs.clear();
    return;
  }
  DemonWorld& d = *w.demons;
  TalkWorld& t = TW(d);
  // ---- the conversation's demon actions ------------------------------------------------
  if (w.talk != nullptr && !w.talk->demonActs.empty()) {
    const std::vector<dialogue::Store::DemonAct> acts = std::move(w.talk->demonActs);
    w.talk->demonActs.clear();
    for (const dialogue::Store::DemonAct& a : acts) {
      LiveDemon* ld = Live(w, a.speaker);
      SessionTick* p = Who(players, a.session);
      if (ld == nullptr || p == nullptr) continue;   // not a demon's conversation
      switch (a.kind) {
        case dialogue::Act::Kind::PresentContract:
          UiOf(w, *p->s).demonTalk.pickerOpen = true;
          break;
        case dialogue::Act::Kind::Release:
          if (ld->state == DemonState::Contained) {
            const ReleaseResult r = Release(w, players, ld->mobId, ContractWeight(*ld), tick);
            if (r == ReleaseResult::Held) t.stats.released++;
            EndTalkWith(w, players, ld->mobId);
          }
          break;
        case dialogue::Act::Kind::Dismiss:
          t.stats.dismissed++;
          EndTalkWith(w, players, ld->mobId);
          Depart(w, *ld, tick, "dismissed");
          break;
        default: break;
      }
    }
  }
  // ---- talking only while contained ----------------------------------------------------
  if (w.talk != nullptr)
    for (SessionTick& p : players)
      if (p.s->talk.active && p.s->talk.speaker.mobId != 0) {
        const LiveDemon* ld = Live(w, p.s->talk.speaker.mobId);
        if (ld != nullptr && ld->state != DemonState::Contained) dialogue::End(*w.talk, *p.s);
      }
  // ---- restores (a loaded save's bound demons find their bodies) ------------------------
  if (!t.restores.empty()) {
    size_t keep = 0;
    for (size_t i = 0; i < t.restores.size(); i++) {
      Restore& r = t.restores[i];
      if (r.firstTick == 0) r.firstTick = tick;
      uint64_t best = 0;
      float bd = kRestoreMatchVox;
      for (uint32_t k = 0; k < w.mobs.MobCount(); k++) {
        const Mob* m = w.mobs.MobAt(k);
        if (m == nullptr || !m->Alive() || m->Def() == nullptr || m->Def()->name != r.mobDef)
          continue;
        if (Live(w, m->Id()) != nullptr) continue;
        const float dd = (m->Origin() - r.origin).len();
        if (dd <= bd) {
          bd = dd;
          best = m->Id();
        }
      }
      if (best != 0 && d.live.size() < DemonWorld::kMaxLive) {
        LiveDemon ld;
        ld.mobId = best;
        ld.demon = r.demon;
        ld.name = r.name;
        ld.session = r.session;
        ld.state = (DemonState)r.state;
        ld.spawnTick = ld.stateTick = ld.lastCheck = tick;
        ld.why = "restored from a save";
        ld.circle = r.circle;
        ld.pact = std::make_shared<Pact>(r.pact);
        const DemonDef* def = d.lib.Find(r.demon);
        if (def != nullptr && !def->released.empty()) w.mobs.SetMobBehavior(best, def->released);
        std::printf("demons: %s restored (%s, \"%s\", %d ticks of term left)\n", r.name.c_str(),
                    DemonStateName(ld.state), r.pact.page.name.c_str(), r.pact.restoreLeft);
        d.live.push_back(std::move(ld));
        t.stats.restored++;
        continue;
      }
      if (tick - r.firstTick > kRestoreTicks) {
        t.stats.restoreLost++;
        std::fprintf(stderr, "demons: a saved %s found no body; its contract is void\n",
                     r.name.c_str());
        continue;
      }
      if (keep != i) t.restores[keep] = std::move(r);
      keep++;
    }
    t.restores.resize(keep);
  }
  // ---- the pacts ---------------------------------------------------------------------------
  bool anyPact = false;
  for (const LiveDemon& ld : d.live) anyPact = anyPact || (bool)ld.pact;
  if (anyPact) {
    BuildActors(w, players, t, tick);
    for (size_t i = 0; i < d.live.size(); i++) {
      LiveDemon& ld = d.live[i];
      if (!ld.pact || !w.mobs.IsAlive(ld.mobId)) continue;
      Pact& p = *ld.pact;
      if (p.restoreLeft >= 0) {
        p.expireTick = p.restoreLeft > 0 ? tick + (uint32_t)p.restoreLeft : 0u;
        if (p.page.hours > 0 && p.restoreLeft == 0) p.expireTick = tick;
        p.restoreLeft = -1;
      }
      if (p.expireTick != 0 && tick >= p.expireTick) {
        t.stats.expired++;
        EndTalkWith(w, players, ld.mobId);
        Depart(w, ld, tick, "its term is over");
        continue;
      }
      if (p.pendingPenalty >= 0) {
        const int c = p.pendingPenalty;
        p.pendingPenalty = -1;
        Fire(w, players, ld, c, tick);
        if (!ld.pact) continue;
      }
      if (ld.state == DemonState::Released) RunPact(w, players, ld, t, out, tick);
      else if (ai::Brain* b = w.mobs.MobBrainMut(ld.mobId)) {
        // Bound but still in the circle: calm, waiting to be let out.
        ClearTarget(*b);
        ClearRoutine(*b);
      }
    }
  }
  // ---- the panel's mirror ----------------------------------------------------------------
  const contract::Content& content = contract::GetContent();
  for (SessionTick& p : players) {
    PlayerSession& s = *p.s;
    UIState& ui = UiOf(w, s);
    ui.boundDemons.clear();
    for (const LiveDemon& ld : d.live) {
      if (!ld.pact || ld.session != s.index || !w.mobs.IsAlive(ld.mobId)) continue;
      UIState::BoundDemonUI row;
      row.name = ld.name;
      row.contract = ld.pact->page.name;
      row.upkeep = ld.pact->upkeep;
      row.contained = ld.state == DemonState::Contained;
      row.termLeftS = ld.pact->expireTick == 0
                          ? -1
                          : (int)((ld.pact->expireTick > tick ? ld.pact->expireTick - tick : 0) /
                                  30u);
      const int ad = ld.pact->activeDuty;
      if (ad >= 0 && ad < (int)ld.pact->page.clauses.size())
        row.duty = contract::VerbName(ld.pact->page.clauses[(size_t)ad].verb);
      ui.boundDemons.push_back(std::move(row));
    }
    UIState::DemonTalkUI& dt = ui.demonTalk;
    const LiveDemon* ld = s.talk.active ? Live(w, s.talk.speaker.mobId) : nullptr;
    dt.active = ld != nullptr;
    if (!dt.active) {
      dt.pickerOpen = false;
      dt.mobId = 0;
      continue;
    }
    const DemonDef* def = DefOf(w, *ld);
    dt.mobId = ld->mobId;
    dt.name = ld->name;
    dt.strength = ld->bind.strength;
    dt.power = def ? def->power : ld->bind.power;
    dt.bound = (bool)ld->pact;
    dt.contract = ld->pact ? ld->pact->page.name : std::string();
    dt.weight = ld->pact ? ld->pact->weight : 0;
    dt.upkeep = ld->pact ? ld->pact->upkeep : contract::UpkeepFor(dt.power, content.tariff);
    dt.lookingAway = p.ti.Held(TB_DEMON_LOOKAWAY);
    dt.strain = std::clamp((float)ld->bind.strain / (float)std::max(1, ld->bind.strainMax), 0.0f,
                           1.0f);
    dt.gazeHold = ld->bind.gazeHold;
    dt.gazeBroken = ld->bind.gazeBroken;
    dt.offers.clear();
    for (const contract::Page* pg : PresentablePages(s)) {
      UIState::DemonTalkUI::Offer o;
      o.name = pg->name;
      o.hash = contract::PageHash(pg->name);
      o.stock = pg->stock;
      contract::Page c = *pg;
      std::vector<std::string> errs;
      o.compiles = contract::Compile(c, errs);
      o.weight = contract::Weigh(c, content.tariff).total;
      dt.offers.push_back(std::move(o));
    }
    dt.lastResult.clear();
    for (const auto& [ss, pi] : t.lastPresent)
      if (ss == s.index && pi.result != PresentResult::None) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "\"%s\": %s (weight %d, margin %d)", pi.page.c_str(),
                      PresentResultName(pi.result), pi.weight, pi.margin);
        dt.lastResult = buf;
      }
  }
}

// ---- presentation ------------------------------------------------------------------------------

std::string TalkText(const TickAuthorityCtx& w, uint64_t mobId, const std::string& text,
                     uint32_t salt) {
  if (text.find('{') == std::string::npos) return text;
  const LiveDemon* ld = Live(w, mobId);
  if (ld == nullptr) return text;
  const DemonDef* def = DefOf(w, *ld);
  const int32_t weight = ContractWeight(*ld);
  const int32_t power = def ? def->power : ld->bind.power;
  int32_t margin = ld->bind.strength - power - weight;
  // A refusal speaks for the page that was refused.
  if (w.demons && w.demons->talk)
    for (const auto& [ss, pi] : w.demons->talk->lastPresent)
      if (ss == ld->session && pi.result == PresentResult::Refused) margin = pi.margin;
  auto sub = [](std::string s, const std::string& key, const std::string& v) {
    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + v.size()))
      s.replace(at, key.size(), v);
    return s;
  };
  std::string s = text;
  s = sub(s, "{tell}", TellFor(w.demons->lib.seals, ld->demon, margin, salt));
  s = sub(s, "{name}", ld->name);
  s = sub(s, "{margin}", std::to_string(margin));
  s = sub(s, "{weight}", std::to_string(weight));
  s = sub(s, "{contract}", ld->pact ? ld->pact->page.name : std::string("no contract"));
  return s;
}

// ---- persistence -------------------------------------------------------------------------------

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char e) {
  return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) |
         ((uint32_t)(uint8_t)e << 24);
}
constexpr uint32_t kDemonSaveVersion = 1;
constexpr uint32_t kContractSaveVersion = 1;

void PutU32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i)));
}
void PutF(std::vector<uint8_t>& o, float f) {
  uint32_t u = 0;
  std::memcpy(&u, &f, 4);
  PutU32(o, u);
}
void PutStr(std::vector<uint8_t>& o, const std::string& s) {
  PutU32(o, (uint32_t)s.size());
  o.insert(o.end(), s.begin(), s.end());
}
struct Rd {
  const uint8_t* d;
  const uint8_t* e;
  bool ok = true;
  uint32_t U32() {
    if (e - d < 4) {
      ok = false;
      return 0;
    }
    const uint32_t v =
        (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    d += 4;
    return v;
  }
  float F() {
    const uint32_t u = U32();
    float f = 0;
    std::memcpy(&f, &u, 4);
    return f;
  }
  std::string Str() {
    const uint32_t n = U32();
    if (!ok || n > 4096 || (size_t)(e - d) < n) {
      ok = false;
      return {};
    }
    std::string s((const char*)d, n);
    d += n;
    return s;
  }
};

void PutCircle(std::vector<uint8_t>& o, const CircleShape& c) {
  PutU32(o, (uint32_t)c.verdict);
  for (int32_t v : {c.startX, c.startZ, c.feetY, c.x0, c.z0, c.w, c.h, c.cells, c.ringCells})
    PutU32(o, (uint32_t)v);
  PutF(o, c.cx);
  PutF(o, c.cz);
  PutF(o, c.radius);
  PutU32(o, (uint32_t)c.mask.size());
  o.insert(o.end(), c.mask.begin(), c.mask.end());
}
bool GetCircle(Rd& r, CircleShape& c) {
  c.verdict = (CircleVerdict)std::min<uint32_t>(r.U32(), 3u);
  int32_t* f[] = {&c.startX, &c.startZ, &c.feetY, &c.x0, &c.z0, &c.w, &c.h, &c.cells, &c.ringCells};
  for (int32_t* p : f) *p = (int32_t)r.U32();
  c.cx = r.F();
  c.cz = r.F();
  c.radius = r.F();
  const uint32_t n = r.U32();
  if (!r.ok || n > (1u << 20) || (size_t)(r.e - r.d) < n ||
      (int64_t)n != (int64_t)std::max(0, c.w) * std::max(0, c.h))
    return false;
  c.mask.assign(r.d, r.d + n);
  r.d += n;
  return true;
}

}  // namespace

void AppendSaveSections(EntityIO& io, TickAuthorityCtx& w, PlayerSession& s) {
  TickAuthorityCtx* wp = &w;
  PlayerSession* sp = &s;
  // 'DMNS' (world.sve): every BOUND demon.
  io.sections.push_back(EntitySection{
      FourCC('D', 'M', 'N', 'S'), kDemonSaveVersion,
      [wp] {
        DemonWorld& d = Demons(*wp);
        // The creatures are the MOBS section's and come back under new ids:
        // the old list names nobody. Bound ones come back through `restores`.
        d.live.clear();
        d.pending.clear();
        TW(d).restores.clear();
        TW(d).strikes.clear();
      },
      [wp](std::vector<uint8_t>& o) {
        DemonWorld& d = Demons(*wp);
        const uint32_t now = TW(d).actorsTick;
        std::vector<const LiveDemon*> bound;
        for (const LiveDemon& ld : d.live)
          if (ld.pact && wp->mobs.IsAlive(ld.mobId)) bound.push_back(&ld);
        PutU32(o, (uint32_t)bound.size());
        for (const LiveDemon* ld : bound) {
          const Mob* m = wp->mobs.FindMobById(ld->mobId);
          PutStr(o, ld->demon);
          PutStr(o, ld->name);
          PutStr(o, m && m->Def() ? m->Def()->name : std::string());
          PutU32(o, (uint32_t)ld->session);
          PutU32(o, (uint32_t)ld->state);
          const Vec3 org = m ? m->Origin() : Vec3{};
          PutF(o, org.x);
          PutF(o, org.y);
          PutF(o, org.z);
          PutCircle(o, ld->circle);
          const Pact& p = *ld->pact;
          contract::PutPage(o, p.page);
          PutU32(o, (uint32_t)p.weight);
          PutU32(o, (uint32_t)p.upkeep);
          // The term left, in ticks (the save's tick and the load's differ).
          const uint32_t left =
              p.restoreLeft >= 0 ? (uint32_t)p.restoreLeft
              : p.expireTick == 0 ? 0u
                                  : (p.expireTick > now ? p.expireTick - now : 1u);
          PutU32(o, left);
          for (int32_t c : p.counters) PutU32(o, (uint32_t)c);
          PutF(o, p.anchor.x);
          PutF(o, p.anchor.y);
          PutF(o, p.anchor.z);
        }
      },
      [wp](const uint8_t* data, size_t len, uint32_t v) {
        if (v != kDemonSaveVersion) return false;
        Rd r{data, data + len};
        DemonWorld& d = Demons(*wp);
        const uint32_t n = r.U32();
        if (!r.ok || n > DemonWorld::kMaxLive) return false;
        for (uint32_t i = 0; i < n; i++) {
          Restore rs;
          rs.demon = r.Str();
          rs.name = r.Str();
          rs.mobDef = r.Str();
          rs.session = (int)r.U32();
          rs.state = (uint8_t)std::min<uint32_t>(r.U32(), 2u);
          rs.origin.x = r.F();
          rs.origin.y = r.F();
          rs.origin.z = r.F();
          if (!GetCircle(r, rs.circle)) return false;
          if (!r.ok || !contract::GetPage(r.d, r.e, rs.pact.page)) return false;
          rs.pact.weight = (int32_t)r.U32();
          rs.pact.upkeep = (int32_t)r.U32();
          const uint32_t left = r.U32();
          rs.pact.restoreLeft = rs.pact.page.hours > 0 ? (int32_t)std::max(1u, left) : 0;
          for (int32_t& c : rs.pact.counters) c = (int32_t)r.U32();
          rs.pact.anchor.x = r.F();
          rs.pact.anchor.y = r.F();
          rs.pact.anchor.z = r.F();
          if (!r.ok) return false;
          TW(d).restores.push_back(std::move(rs));
        }
        return true;
      }});
  // 'CNTR' (players/<id>.svp): the player's contract pages.
  EntitySection cn{
      FourCC('C', 'N', 'T', 'R'), kContractSaveVersion, [sp] { sp->caster.contracts.clear(); },
      [sp](std::vector<uint8_t>& o) {
        PutU32(o, (uint32_t)sp->caster.contracts.size());
        for (const contract::Page& p : sp->caster.contracts) contract::PutPage(o, p);
      },
      [sp](const uint8_t* data, size_t len, uint32_t v) {
        if (v != kContractSaveVersion) return false;
        const uint8_t* d = data;
        const uint8_t* e = data + len;
        if (e - d < 4) return false;
        const uint32_t n =
            (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        d += 4;
        if (n > 256) return false;
        for (uint32_t i = 0; i < n; i++) {
          contract::Page p;
          if (!contract::GetPage(d, e, p)) return false;
          sp->caster.contracts.push_back(std::move(p));
        }
        return true;
      }};
  cn.scope = EntityScope::Player;
  io.sections.push_back(std::move(cn));
}

}  // namespace demon
