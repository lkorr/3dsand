// stroke_log.cpp -- the calibration log (see the header) and its reducer.

#include "game/stroke_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "game/ai_behavior.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/shellresponse.h"
#include "game/strokes.h"
#include "sim/tuning.h"

namespace strokelog {

Sink* g_active = nullptr;

const char* OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::Open: return "open";
    case Outcome::Miss: return "miss";
    case Outcome::Blocked: return "blocked";
    case Outcome::Parried: return "parried";
    case Outcome::Hit: return "hit";
    case Outcome::HitOther: return "hit-other";
  }
  return "?";
}

namespace {

Vec3 CentreOf(const Mob& m) {
  const MobDef* d = m.Def();
  return d != nullptr ? m.Origin() + d->worldSize * 0.5f : m.Origin();
}

// What a creature holds up against a blow (distant::GuardClass), live.
distant::GuardClass GuardOf(const Mob& m) {
  if (m.HoldsShield()) return distant::GuardClass::Shield;
  for (Hand h : {Hand::Right, Hand::Left}) {
    const int s = m.HeldSlot(h);
    if (s >= 0 && !m.IsShieldSlot(s)) return distant::GuardClass::Blade;
  }
  return distant::GuardClass::None;
}

float HeldHp(const Mob& m) {
  float sum = 0.0f;
  for (Hand h : {Hand::Right, Hand::Left}) {
    const int s = m.HeldSlot(h);
    if (s >= 0) sum += std::max(0.0f, m.LimbHpAt(s));
  }
  return sum;
}

}  // namespace

Sink::Pending* Sink::Find(uint64_t attacker) {
  for (Pending& p : open_)
    if (p.attacker == attacker) return &p;
  return nullptr;
}

void Sink::OnStart(MobSystem& sys, const Mob& attacker, uint32_t tick) {
  // A SWING THAT REPLACED ONE STILL OPEN (a guard taken over, a scripted
  // swing on top of the AI's) closes the old record first: one creature, one
  // stroke in flight, which is what the runtime has.
  if (Pending* p = Find(attacker.Id())) Close(*p, tick, EndReason::Replaced);
  const NpcStroke& st = attacker.Stroke();
  const StyleLibrary& lib = sys.AttackStyles();
  const ItemLibrary* items = items_ != nullptr ? items_ : sys.Items();
  Stroke s;
  s.startTick = tick;
  s.attacker = attacker.Id();
  s.target = st.targetId;
  if (attacker.Def() != nullptr) s.attackerDef = attacker.Def()->name;
  if (const ai::Brain* br = sys.MobBrain(attacker.Id()))
    if (const ai::Profile* pr = sys.Behaviors().At(br->profile)) s.profile = pr->name;
  if (const AttackStyle* sty = lib.At(st.style)) {
    // A FORM is its own library entry named "<base>@<form>" (strokes.cpp): the
    // table row is the BASE style, so a dagger's short horizontal and a
    // sword's are one row with two weapons in it.
    const size_t at = sty->name.find('@');
    s.style = at == std::string::npos ? sty->name : sty->name.substr(0, at);
    s.form = sty->form;
  }
  StrikeProfile strike;
  if (const MobNaturalWeaponDef* nw = attacker.EffectorWeapon()) {
    strike = attacker.StrikeProfileFor(*nw);
    s.weapon = nw->name;
    s.weaponClass = "natural";
    s.natural = true;
  } else if (const ItemDef* it = items != nullptr && !attacker.HeldItem().empty()
                                     ? items->Named(attacker.HeldItem())
                                     : nullptr) {
    strike = it->strike;
    s.weapon = it->name;
    s.weaponClass = it->weaponClass;
  } else {
    strike.cut = 12.0f;   // StepStroke's own default for an edge with no item
    s.weapon = "?";
  }
  s.strikeCut = strike.cut;
  s.strikeBlunt = strike.blunt;
  s.strikeBite = strike.bite;
  s.kind = distant::DominantKind(strike);
  s.windupTicks = st.windupTicks;
  s.cutTicks = st.cutTicks;

  Pending p;
  p.attacker = attacker.Id();
  if (Mob* v = sys.FindCombatantById(st.targetId)) {
    s.distance = (CentreOf(*v) - CentreOf(attacker)).len();
    s.guard = GuardOf(*v);
    s.targetAlive = v->Alive();
    const int base = v->AppendedBase();
    p.lifeByLimb.assign((size_t)std::max(0, base), 0.0f);
    p.shellByHost.assign((size_t)std::max(0, base), 0.0f);
    p.hardness = distant::ShellHardnessOfLimbs(*v);
    p.roles.resize((size_t)std::max(0, base));
    for (int i = 0; i < base; i++) {
      const MobLimbDef& ld = v->LimbDefAt(i);
      p.roles[(size_t)i] = distant::LimbRoleOf(ld.tag, ld.name, ld.bloodless);
    }
    for (float h : p.hardness) s.targetArmoured = s.targetArmoured || h > 0.0f;
    if (st.targetLimb >= 0 && st.targetLimb < base)
      s.drawnRole = p.roles[(size_t)st.targetLimb];
  } else {
    s.targetAlive = false;
  }
  p.index = strokes_.size();
  strokes_.push_back(std::move(s));
  open_.push_back(std::move(p));
}

void Sink::BeforeSweep(MobSystem& sys, const Mob& attacker) {
  snap_ = Snap{};
  if (Find(attacker.Id()) == nullptr) return;
  snap_.valid = true;
  Mob* v = sys.FindCombatantById(attacker.Stroke().targetId);
  if (v == nullptr) return;
  snap_.victim = v->Id();
  snap_.alive = v->Alive();
  const int base = v->AppendedBase();
  snap_.limbHp.resize((size_t)std::max(0, base));
  snap_.attached.assign((size_t)std::max(0, base), 0);
  float owed = 0.0f;
  for (int i = 0; i < base; i++) {
    snap_.limbHp[(size_t)i] = v->LimbHpAt(i);
    snap_.attached[(size_t)i] = sys.LimbBody(v->Id(), i) != 0 ? 1 : 0;
    owed += std::max(0.0f, sys.LimbBleedBudget(v->Id(), i));
  }
  snap_.owed = owed;
  for (int s = base; s < v->LimbCount(); s++)
    if (v->LimbDefAt(s).tag == "worn") snap_.shells.push_back({s, v->LimbHpAt(s)});
  snap_.held = HeldHp(*v);
  for (int c = 0; c < (int)DamageCause::Count; c++)
    snap_.lostBy[c] = v->HpLostBy((DamageCause)c);
}

void Sink::OnSweep(const Mob& attacker, const EdgeSweepResult& r) {
  (void)attacker;
  if (!snap_.valid) return;
  snap_.sweeps++;
  const bool contact = r.hasHitAt || r.bodiesHit > 0;
  snap_.contact = snap_.contact || contact;
  snap_.arrested = snap_.arrested || r.arrested;
  snap_.shield = snap_.shield || (r.arrested && r.block.shield);
  if (contact || r.arrested) snap_.power = std::max(snap_.power, r.power * r.edgeAlign);
  snap_.tipSpeed = std::max(snap_.tipSpeed, r.tipSpeed);
}

void Sink::AfterSweep(MobSystem& sys, const Mob& attacker, uint32_t tick) {
  Pending* p = Find(attacker.Id());
  if (p == nullptr || !snap_.valid) {
    snap_ = Snap{};
    return;
  }
  Stroke& s = strokes_[p->index];
  s.sweeps += snap_.sweeps;
  float landed = 0.0f;
  if (snap_.victim != 0)
    if (Mob* v = sys.FindCombatantById(snap_.victim)) {
      const int base = v->AppendedBase();
      for (int i = 0; i < base && i < (int)snap_.limbHp.size(); i++) {
        const float d = snap_.limbHp[(size_t)i] - v->LimbHpAt(i);
        if (d <= 0.0f) continue;
        if ((size_t)i < p->lifeByLimb.size()) p->lifeByLimb[(size_t)i] += d;
        s.lifeHp += d;
        landed += d;
      }
      for (const auto& sh : snap_.shells) {
        if (sh.first >= v->LimbCount()) continue;
        const float d = sh.second - v->LimbHpAt(sh.first);
        if (d <= 0.0f) continue;
        const int host = v->WornHostOf(sh.first);
        if (host >= 0 && (size_t)host < p->shellByHost.size()) p->shellByHost[(size_t)host] += d;
        s.shellHp += d;
        landed += d;
      }
      const float held = snap_.held - HeldHp(*v);
      if (held > 0.0f) {
        s.guardHp += held;
        landed += held;
      }
      for (int c = 0; c < (int)DamageCause::Count; c++)
        s.hpByCause[c] += std::max(0.0f, v->HpLostBy((DamageCause)c) - snap_.lostBy[c]);
      float owed = 0.0f;
      for (int i = 0; i < base; i++) owed += std::max(0.0f, sys.LimbBleedBudget(v->Id(), i));
      if (owed > snap_.owed)
        s.bleedOwed += (owed - snap_.owed) * CurrentTuning().gore.bleedHpPerVoxel;
      if (snap_.alive && !v->Alive()) s.killed = true;
      // D2b: what came OFF (a body before this sweep, none after).
      for (int i = 0; i < base && i < (int)snap_.attached.size(); i++)
        if (snap_.attached[(size_t)i] && sys.LimbBody(v->Id(), i) == 0) {
          s.severed++;
          if (i < 64) s.severedMask |= 1ull << i;
          const MobLimbDef& ld = v->LimbDefAt(i);
          const distant::LimbRole r = distant::LimbRoleOf(ld.tag, ld.name, ld.bloodless);
          if ((int)r < distant::kRoles) s.severedByRole[(int)r]++;
        }
    }
  if (snap_.contact) s.contacts++;
  if ((snap_.contact || snap_.arrested) && s.impactTick == 0) s.impactTick = tick;
  if (snap_.arrested) {
    s.arrested = true;
    s.arrestedByShield = s.arrestedByShield || snap_.shield;
  }
  if (snap_.contact && landed <= 0.0f) s.metOther = true;
  s.power = std::max(s.power, snap_.power);
  s.tipSpeed = std::max(s.tipSpeed, snap_.tipSpeed);
  snap_ = Snap{};
}

void Sink::OnEnd(const Mob& attacker, uint32_t tick, EndReason why) {
  if (Pending* p = Find(attacker.Id())) Close(*p, tick, why);
}

void Sink::Close(Pending& p, uint32_t tick, EndReason why) {
  Stroke& s = strokes_[p.index];
  s.end = why;
  s.endTick = tick;
  // WHERE IT LANDED: the limb that lost the most life; a blow the armour took
  // whole is placed on the limb under the shell that took the most.
  int best = -1;
  float bv = 0.0f;
  for (size_t i = 0; i < p.lifeByLimb.size(); i++)
    if (p.lifeByLimb[i] > bv) {
      bv = p.lifeByLimb[i];
      best = (int)i;
    }
  if (best < 0)
    for (size_t i = 0; i < p.shellByHost.size(); i++)
      if (p.shellByHost[i] > bv) {
        bv = p.shellByHost[i];
        best = (int)i;
      }
  s.limb = best;
  if (best >= 0) {
    if ((size_t)best < p.roles.size()) s.role = p.roles[(size_t)best];
    if ((size_t)best < p.hardness.size()) s.shellHardness = p.hardness[(size_t)best];
  }
  if (s.lifeHp > 0.0f || s.shellHp > 0.0f) s.outcome = Outcome::Hit;
  else if (s.arrested) s.outcome = s.arrestedByShield ? Outcome::Blocked : Outcome::Parried;
  else if (s.contacts > 0 || s.metOther) s.outcome = Outcome::HitOther;
  else s.outcome = Outcome::Miss;
  const size_t k = (size_t)(&p - open_.data());
  if (k < open_.size()) open_.erase(open_.begin() + (ptrdiff_t)k);
}

void Sink::Flush(uint32_t tick) {
  while (!open_.empty()) Close(open_.front(), tick, EndReason::Flushed);
}

void Sink::Clear() {
  strokes_.clear();
  open_.clear();
  snap_ = Snap{};
}

// =============================================================================
// The reducer
// =============================================================================

namespace {

// Additive smoothing toward the pooled value: a style with five strokes must
// not get a hit rate of 0 or 1 because of them. `kPrior` pseudo-samples of the
// pooled answer ride along with every style's own.
constexpr float kPrior = 4.0f;

struct Acc {
  int n = 0, hits = 0, blocked = 0;
  float roles[distant::kRoles] = {};
  double pw = 0, pw2 = 0;   // life hp / strike total over UNARMOURED hits
  int pwN = 0;
  double imp = 0;
  int impN = 0;
  void Add(const Stroke& s) {
    n++;
    if (s.outcome == Outcome::Hit) {
      hits++;
      if (s.role != distant::LimbRole::Other && (int)s.role < distant::kRoles)
        roles[(int)s.role] += 1.0f;
      if (s.shellHardness <= 0.0f && s.lifeHp > 0.0f && s.StrikeTotal() > 0.0f) {
        const double x = s.lifeHp / s.StrikeTotal();
        pw += x;
        pw2 += x * x;
        pwN++;
      }
    }
    if (s.outcome == Outcome::Blocked || s.outcome == Outcome::Parried) blocked++;
    if (s.impactTick > 0 && s.impactTick >= s.startTick) {
      imp += (double)(s.impactTick - s.startTick);
      impN++;
    }
  }
  float Rate() const {
    const int d = n - blocked;
    return d > 0 ? (float)hits / (float)d : 0.0f;
  }
  float Power() const { return pwN > 0 ? (float)(pw / pwN) : 0.0f; }
  float Cv() const {
    if (pwN < 2) return 0.0f;
    const double m = pw / pwN, var = std::max(0.0, pw2 / pwN - m * m);
    return m > 1e-9 ? (float)(std::sqrt(var) / m) : 0.0f;
  }
};

void Fill(const Acc& a, const Acc& pool, distant::StyleCalib& out) {
  out.n = a.n;
  out.hits = a.hits;
  out.blocked = a.blocked;
  const float d = (float)(a.n - a.blocked);
  out.hitRate = std::clamp(((float)a.hits + kPrior * pool.Rate()) / (d + kPrior), 0.0f, 1.0f);
  float rs = 0.0f, ps = 0.0f;
  for (int r = 0; r < distant::kRoles; r++) {
    rs += a.roles[r];
    ps += pool.roles[r];
  }
  for (int r = 0; r < distant::kRoles; r++) {
    const float pr = ps > 0.0f ? pool.roles[r] / ps : (r == (int)distant::LimbRole::Torso ? 1.0f : 0.0f);
    out.roleWeights[r] = (a.roles[r] + kPrior * pr) / (rs + kPrior);
  }
  const float pp = pool.Power();
  out.meanPower = ((float)a.pw + kPrior * pp) / ((float)a.pwN + kPrior);
  out.powerCv = std::clamp(a.pwN >= 4 ? a.Cv() : pool.Cv(), 0.0f, 1.0f);
  out.impactTicks = a.impN > 0 ? (float)(a.imp / a.impN)
                               : (pool.impN > 0 ? (float)(pool.imp / pool.impN) : 0.0f);
}

bool Counted(const Stroke& s) {
  return s.outcome != Outcome::Open && s.target != 0 && s.targetAlive;
}

}  // namespace

void ReduceStrokes(const std::vector<Stroke>& log, int minSamples,
                   float drainShare, const Tuning::Gear& gear,
                   distant::DistantCalib& out, ReduceStats& st,
                   std::string& report) {
  st = ReduceStats{};
  st.strokes = (int)log.size();
  Acc pool;
  std::map<std::string, Acc> byStyle;
  std::map<std::pair<std::string, std::string>, Acc> byWeapon;
  int guardN[distant::kGuards] = {}, guardB[distant::kGuards] = {};
  double owed[distant::kKinds] = {}, owedHp[distant::kKinds] = {};
  for (const Stroke& s : log) {
    if (!Counted(s)) continue;
    st.counted++;
    switch (s.outcome) {
      case Outcome::Hit: st.hits++; break;
      case Outcome::Blocked:
      case Outcome::Parried: st.blocked++; break;
      case Outcome::Miss: st.misses++; break;
      default: st.other++; break;
    }
    pool.Add(s);
    byStyle[s.style].Add(s);
    byWeapon[{s.style, s.weapon}].Add(s);
    guardN[(int)s.guard]++;
    if (s.outcome == Outcome::Blocked || s.outcome == Outcome::Parried) guardB[(int)s.guard]++;
    if (s.outcome == Outcome::Hit && s.lifeHp > 0.0f) {
      owed[(int)s.kind] += s.bleedOwed;
      owedHp[(int)s.kind] += s.lifeHp;
    }
  }
  out.styles.clear();
  out.minSamples = minSamples;
  Fill(pool, pool, out.fallback);
  out.fallback.name = "*";
  for (const auto& kv : byStyle) {
    distant::StyleCalib sc;
    sc.name = kv.first;
    Fill(kv.second, pool, sc);
    for (const auto& w : byWeapon) {
      if (w.first.first != kv.first || w.second.n < minSamples) continue;
      distant::WeaponCalib wc;
      wc.weapon = w.first.second;
      wc.n = w.second.n;
      wc.hits = w.second.hits;
      wc.blocked = w.second.blocked;
      const float d = (float)(wc.n - wc.blocked);
      wc.hitRate = std::clamp(((float)wc.hits + kPrior * sc.hitRate) / (d + kPrior), 0.0f, 1.0f);
      wc.meanPower = ((float)w.second.pw + kPrior * sc.meanPower) / ((float)w.second.pwN + kPrior);
      sc.byWeapon.push_back(wc);
    }
    out.styles.push_back(sc);
    char line[256];
    std::snprintf(line, sizeof line,
                  "  %-16s n %4d hits %4d blocked %3d -> hitRate %.3f meanPower %.3f "
                  "(cv %.2f, %d unarmoured) impact %.1f t, %zu weapon rows\n",
                  kv.first.c_str(), sc.n, sc.hits, sc.blocked, sc.hitRate, sc.meanPower,
                  sc.powerCv, kv.second.pwN, sc.impactTicks, sc.byWeapon.size());
    report += line;
  }
  for (int g = 0; g < distant::kGuards; g++) {
    out.blockN[g] = guardN[g];
    // Smoothed toward zero (a guard is an exception, not a coin).
    out.blockChance[g] = guardN[g] > 0 ? (float)guardB[g] / (float)(guardN[g] + kPrior) : 0.0f;
  }
  // ---- ARMOUR: what a covered limb let through, against the style's own
  // unarmoured mean, less what the shell row already passes.
  double rSum[distant::kKinds] = {}, pSum[distant::kKinds] = {};
  int rN[distant::kKinds] = {};
  for (const Stroke& s : log) {
    if (!Counted(s) || s.outcome != Outcome::Hit || s.shellHardness <= 0.0f ||
        s.StrikeTotal() <= 0.0f)
      continue;
    const int k = (int)s.kind;
    if (s.kind == distant::StrikeKind::Cut && s.shellHardness <= gear.cutHardnessRef)
      continue;   // cloth passes an edge whole: it says nothing about armour
    auto it = byStyle.find(s.style);
    const float ref = it != byStyle.end() && it->second.pwN > 0 ? it->second.Power()
                                                                : pool.Power();
    if (ref <= 1e-6f) continue;
    const float passed =
        s.kind == distant::StrikeKind::Pierce
            ? ShellResponseOf(s.shellHardness, DamageCause::Blade, gear).carve
            : ShellResponseOf(s.shellHardness, distant::CauseOf(s.kind, s.natural), gear).passed;
    rSum[k] += std::min(2.0, (double)(s.lifeHp / s.StrikeTotal()) / ref);
    pSum[k] += passed;
    rN[k]++;
  }
  for (int k = 0; k < distant::kKinds; k++) {
    out.armour.n[k] = rN[k];
    if (rN[k] == 0) continue;
    const double r = rSum[k] / rN[k], p = pSum[k] / rN[k];
    const double e = p < 0.999 ? (r - p) / (1.0 - p) : 1.0;
    // Smoothed toward the prior already in the table by kPrior samples.
    const float raw = (float)std::clamp(e, 0.0, 1.0);
    out.armour.exposure[k] =
        (raw * (float)rN[k] + kPrior * out.armour.exposure[k]) / ((float)rN[k] + kPrior);
  }
  // ---- D2b: THE SIZE OF A HIT, AND WHAT COMES OFF, BY ROLE ----------------
  //
  // SIZE: life hp over (strike total x the style's own mean power), over the
  // unarmoured hits -- the multiplier the resolver draws -- as an inverse CDF
  // per role (DistantCalib::spread). Its TAIL is the organs (a brain, a heart
  // charges a flat hp per voxel), which is what ends a full-fidelity fight
  // early; powerCv, clamped at 1, could not carry it.
  //
  // DISMEMBERMENT: of the unarmoured hits of a kind on a role, the share that
  // took a limb OF THAT ROLE off inside the stroke (DistantCalib::sever) --
  // the struck limb or the one under it; the resolver takes the struck limb
  // and everything under it off, which is what a cut through it does.
  {
    std::vector<double> xs[distant::kRoles];
    int kills[distant::kRoles] = {};
    int roleHits[distant::kRoles][distant::kKinds] = {};
    int roleSevers[distant::kRoles][distant::kKinds] = {};
    int killedHits = 0, severKills = 0;
    for (const Stroke& s : log) {
      if (!Counted(s) || s.outcome != Outcome::Hit || (int)s.role >= distant::kRoles) continue;
      killedHits += s.killed ? 1 : 0;
      severKills += (s.killed && s.severed > 0) ? 1 : 0;
      if (s.shellHardness > 0.0f) continue;   // unarmoured: a shell scales it at distance
      roleHits[(int)s.role][(int)s.kind]++;
      if (s.severedByRole[(int)s.role] > 0) roleSevers[(int)s.role][(int)s.kind]++;
    }
    for (int r = 0; r < distant::kRoles; r++)
      for (int k = 0; k < distant::kKinds; k++) {
        out.sever.hits[k][r] = roleHits[r][k];
        // A cell with few hits says little: it is not trusted below a
        // handful (no sever), and a zero stays zero.
        out.sever.rate[k][r] =
            roleHits[r][k] >= 8 ? (float)roleSevers[r][k] / (float)(roleHits[r][k] + 1) : 0.0f;
        if (roleHits[r][k] == 0) continue;
        char line[200];
        std::snprintf(line, sizeof line,
                      "  severs %-5s %-6s unarmoured hits %4d that took one off %3d -> %.3f\n",
                      distant::LimbRoleName((distant::LimbRole)r),
                      distant::StrikeKindName((distant::StrikeKind)k), roleHits[r][k],
                      roleSevers[r][k], out.sever.rate[k][r]);
        report += line;
      }
    {
      char line[160];
      std::snprintf(line, sizeof line, "  strokes that killed: %d (%d of them severing)\n",
                    killedHits, severKills);
      report += line;
    }
    for (const Stroke& s : log) {
      if (!Counted(s) || s.outcome != Outcome::Hit || s.shellHardness > 0.0f ||
          s.lifeHp <= 0.0f || s.StrikeTotal() <= 0.0f || (int)s.role >= distant::kRoles)
        continue;
      auto it = byStyle.find(s.style);
      const double mp = it != byStyle.end() && it->second.pwN > 0 ? it->second.Power()
                                                                 : pool.Power();
      if (mp <= 1e-6) continue;
      xs[(int)s.role].push_back(s.lifeHp / s.StrikeTotal() / mp);
      kills[(int)s.role] += s.killed ? 1 : 0;
    }
    // The knots: the body of the distribution every 5%, the tail at 97.5,
    // 99, 99.5 and its ends.
    static const double kKnotP[] = {0.0,  0.05, 0.10, 0.15, 0.20,  0.25, 0.30, 0.35,
                                    0.40, 0.45, 0.50, 0.55, 0.60,  0.65, 0.70, 0.75,
                                    0.80, 0.85, 0.90, 0.95, 0.975, 0.99, 0.995, 1.0};
    for (int r = 0; r < distant::kRoles; r++) {
      std::vector<double>& v = xs[r];
      out.spread[r] = distant::HitSpread{};
      if (v.empty()) continue;
      std::sort(v.begin(), v.end());
      auto q = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (double)v.size()))]; };
      // Too few hits for a shape: the CV spread stays (empty knots).
      if (v.size() >= 40)
        for (const double p : kKnotP) {
          out.spread[r].p.push_back((float)p);
          out.spread[r].x.push_back((float)(p >= 1.0 ? v.back() : q(p)));
        }
      double m = 0;
      int over3 = 0, over5 = 0;
      for (double x : v) {
        m += x;
        over3 += x > 3.0;
        over5 += x > 5.0;
      }
      char line[320];
      std::snprintf(line, sizeof line,
                    "  hit size %-5s n %4zu mean %.2f | p10 %.2f p25 %.2f p50 %.2f p75 %.2f p90 "
                    "%.2f p95 %.2f p99 %.2f max %.2f | >3x %.3f >5x %.3f | killed %d%s\n",
                    distant::LimbRoleName((distant::LimbRole)r), v.size(), m / (double)v.size(),
                    q(0.10), q(0.25), q(0.50), q(0.75), q(0.90), q(0.95), q(0.99), v.back(),
                    (double)over3 / (double)v.size(), (double)over5 / (double)v.size(), kills[r],
                    out.spread[r].p.empty() ? " (too few: CV spread)" : "");
      report += line;
    }
  }
  // ---- BLEED: blood opened per hp of each kind, x what actually drains.
  out.bleed.drainShare = drainShare;
  for (int k = 0; k < distant::kKinds; k++)
    if (owedHp[k] > 1e-6)
      out.bleed.perHp[k] = (float)(owed[k] / owedHp[k]) * drainShare;
  char line[256];
  std::snprintf(line, sizeof line,
                "  pooled: %d strokes counted of %d (%d hits, %d blocked/parried, %d "
                "misses, %d other) -> hitRate %.3f meanPower %.3f; block none %.3f "
                "blade %.3f shield %.3f; exposure cut %.2f blunt %.2f bite %.2f\n",
                st.counted, st.strokes, st.hits, st.blocked, st.misses, st.other,
                out.fallback.hitRate, out.fallback.meanPower, out.blockChance[0],
                out.blockChance[1], out.blockChance[2], out.armour.exposure[0],
                out.armour.exposure[1], out.armour.exposure[2]);
  report += line;
}

}  // namespace strokelog
