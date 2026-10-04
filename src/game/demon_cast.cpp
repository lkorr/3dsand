// demon_cast.cpp — creatures that cast; blink; the demon spell kit. See
// demon_cast.h for the shape of it.
#include "game/demon_cast.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "game/demon.h"
#include "game/mob.h"
#include "game/session.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/materials.h"
#include "sim/rng.h"
#include "sim/windprim.h"
#include "sim/world.h"

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

using json = nlohmann::json;

// ---- D3 hook --------------------------------------------------------------------------

namespace {
bool NothingSevered(TickAuthorityCtx&, uint64_t, const char*) { return false; }
}  // namespace

DemonSealQuery& DemonSealHook() {
  // D3 hook: the orchestrator points this at demon_seals.h's query at merge.
  static DemonSealQuery q = &NothingSevered;
  return q;
}

// ---- the kit ----------------------------------------------------------------------------

const char* KitAimName(KitAim a) {
  switch (a) {
    case KitAim::Body: return "body";
    case KitAim::Feet: return "feet";
    case KitAim::Self: return "self";
  }
  return "?";
}

const char* CastOutcomeName(MobCastOutcome o) {
  switch (o) {
    case MobCastOutcome::Cast: return "cast";
    case MobCastOutcome::Blinked: return "blinked";
    case MobCastOutcome::NothingInRange: return "nothing castable";
    case MobCastOutcome::BlinkNoFloor: return "blink: no floor";
    case MobCastOutcome::BlinkNoSight: return "blink: no line";
    case MobCastOutcome::BlinkFenced: return "blink: outside the circle";
    case MobCastOutcome::BlinkSevered: return "blink: severed";
    case MobCastOutcome::NoRoom: return "no room";
  }
  return "?";
}

bool LoadCreatureKit(const std::string& dir, CreatureKit& out, std::string& log) {
  out = CreatureKit{};
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    log += "kit: no directory " + dir + "\n";
    return false;
  }
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());   // load order is file order: deterministic
  for (const auto& p : files) {
    json j;
    try {
      std::ifstream f(p);
      j = json::parse(f, nullptr, true, true);
    } catch (const std::exception& ex) {
      log += "kit: " + p.filename().string() + ": " + ex.what() + "\n";
      continue;
    }
    KitSpell s;
    s.name = p.stem().string();
    s.blink = j.value("kind", std::string("spell")) == "blink";
    s.words = j.value("words", std::string());
    const std::string aim = j.value("aim", std::string("body"));
    s.aim = aim == "feet" ? KitAim::Feet : aim == "self" ? KitAim::Self : KitAim::Body;
    if (aim != "body" && aim != "feet" && aim != "self")
      log += "kit: " + s.name + ": aim \"" + aim + "\" is not body | feet | self (body)\n";
    if (j.contains("range") && j["range"].is_array() && j["range"].size() == 2) {
      s.rangeMin = std::max(0.0f, j["range"][0].get<float>());
      s.rangeMax = std::max(s.rangeMin, j["range"][1].get<float>());
    }
    s.lead = j.value("lead", true);
    s.releaseAfterTicks = std::clamp(j.value("releaseAfterTicks", 0), 0, 900);
    s.once = j.value("once", false);
    if (j.contains("tags") && j["tags"].is_object()) {
      const json& t = j["tags"];
      s.tags.targets = t.value("targets", std::string());
      s.tags.direct = t.value("direct", false);
      for (const json& c : t.value("creates", json::array()))
        if (c.is_string()) s.tags.creates.push_back(c.get<std::string>());
      s.tags.alters = t.value("alters", std::string());
      s.tags.affectsBody = t.value("affectsBody", false);
      s.tags.region = std::max(0, t.value("region", 0));
    } else {
      log += "kit: " + s.name + ": no footprint tags (D5/D6 filter on them)\n";
    }
    s.distance = std::clamp(j.value("distance", s.distance), 2.0f, 64.0f);
    s.standoff = std::clamp(j.value("standoff", s.standoff), 0.0f, 32.0f);
    s.toward = j.value("toward", std::string("target")) != "away";
    s.los = j.value("los", true);
    s.cooldownTicks = std::clamp(j.value("cooldownTicks", s.cooldownTicks), 0, 3000);
    if (!s.blink && s.words.empty()) {
      log += "kit: " + s.name + ": a spell with no words -- skipped\n";
      continue;
    }
    out.spells.push_back(std::move(s));
  }
  return true;
}

// ---- helpers ------------------------------------------------------------------------------

namespace {

SpellFxVec Fx(Vec3 v) {
  return SpellFxVec{SpellFxFromFloat(v.x), SpellFxFromFloat(v.y), SpellFxFromFloat(v.z)};
}

// The words of a kit spell, in the grammar: "lift@2 aura bolt" -> a stack.
// Unknown words are dropped LOUDLY once per (spell, word) through `log`.
SpellStack StackOf(const GlyphLibrary& lib, const std::string& words, std::string* unknown) {
  SpellStack st;
  size_t i = 0;
  while (i < words.size()) {
    while (i < words.size() && words[i] == ' ') i++;
    size_t j = i;
    while (j < words.size() && words[j] != ' ') j++;
    if (j > i) {
      std::string id;
      int32_t mag = kMagUnset;
      SplitMagnitude(words.substr(i, j - i), id, mag);
      const int g = lib.Find(id);
      if (g >= 0) st.Push(g, mag);
      else if (unknown) *unknown += (unknown->empty() ? "" : " ") + id;
    }
    i = j;
  }
  return st;
}

// Material at a cell in the T-4 snapshot (WorldSpellProbe), and what it means
// for a body: OPEN (air, gas, liquid) or FLOOR (solid, powder).
struct Cells {
  SpellProbe probe;
  const std::vector<MaterialDef>* mats = nullptr;
  bool Open(int x, int y, int z, bool& known) const {
    known = true;
    const uint32_t m = probe.matAt ? probe.matAt(probe.ctx, x, y, z, known) : 0;
    if (!known) return false;
    if (m == 0 || m >= mats->size()) return true;
    const uint32_t k = (*mats)[m].gpu.klass;
    return k == CLASS_GAS || k == CLASS_LIQUID;
  }
};

// The top of the floor under (x, z), searching down from `fromY` for `depth`
// cells: the first OPEN cell over a non-open one. False when unseen or none.
bool FloorUnder(const Cells& c, int x, int z, int fromY, int depth, int& floorY) {
  bool known = true;
  bool prevOpen = c.Open(x, fromY, z, known);
  if (!known) return false;
  for (int y = fromY - 1; y >= fromY - depth; y--) {
    const bool open = c.Open(x, y, z, known);
    if (!known) return false;
    if (prevOpen && !open) {
      floorY = y + 1;
      return true;
    }
    prevOpen = open;
  }
  return false;
}

Vec3 BoxCentre(const MobSystem& mobs, uint64_t id, Vec3* size = nullptr) {
  Vec3 lo{}, hi{};
  if (!mobs.MobBodyBox(id, lo, hi)) return mobs.MobOrigin(id);
  if (size) *size = hi - lo;
  return (lo + hi) * 0.5f;
}

const LiveDemon* ContainedDemon(const TickAuthorityCtx& w, uint64_t mobId) {
  if (!w.demons) return nullptr;
  const LiveDemon* ld = w.demons->Find(mobId);
  return ld != nullptr && ld->state == DemonState::Contained ? ld : nullptr;
}

void Log(MobCastWorld& m, const CastEvent& e) {
  if (m.events.size() >= MobCastWorld::kEventRing) m.events.erase(m.events.begin());
  m.events.push_back(e);
}

// ---- the VM's questions about bodies, for a CREATURE caster ------------------------------
//
// The player's probe (session.cpp phase I) answers for one session; this one
// answers for every session at once, naming a player by the AI's actor band
// (ai::kPlayerActorBase + session index) so the id a status rides is the id
// the creature targeted.
struct ProbeCtx {
  TickAuthorityCtx* w = nullptr;
  std::span<SessionTick> players;
  uint64_t caster = 0;
  std::vector<uint64_t> ownBodies;   // the caster's limbs: a ray from its chest
};

PlayerSession* SessionFor(std::span<SessionTick> players, uint64_t id) {
  if (!ai::IsPlayerActorId(id)) return nullptr;
  const int idx = (int)(id - ai::kPlayerActorBase);
  for (SessionTick& p : players)
    if (p.s && p.s->index == idx) return p.s;
  return nullptr;
}

SpellBodyProbe MakeProbe(ProbeCtx& pc) {
  SpellBodyProbe bp;
  bp.ctx = &pc;
  bp.bodyHit = [](void* c, Vec3 from, Vec3 to, uint64_t, Vec3& out) {
    ProbeCtx& p = *(ProbeCtx*)c;
    const Vec3 seg = to - from;
    const float len = seg.len();
    if (len < 1e-4f) return false;
    const Vec3 dir = seg * (1.0f / len);
    float frac = 1.0f;
    if (p.w->phys.CastRayBody(from, dir, len, frac, p.ownBodies) == 0) return false;
    out = from + dir * (frac * len);
    return true;
  };
  bp.bodyIdAt = [](void* c, Vec3 from, float radius, uint64_t& id, Vec3& out) {
    ProbeCtx& p = *(ProbeCtx*)c;
    MobSystem& mobs = p.w->mobs;
    auto boxDist = [](const Vec3& q, const Vec3& lo, const Vec3& hi) {
      const float dx = std::max(std::max(lo.x - q.x, 0.0f), q.x - hi.x);
      const float dy = std::max(std::max(lo.y - q.y, 0.0f), q.y - hi.y);
      const float dz = std::max(std::max(lo.z - q.z, 0.0f), q.z - hi.z);
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    float best = radius;
    bool found = false;
    for (uint32_t i = 0; i < mobs.MobCount(); i++) {
      const uint64_t mid = mobs.MobIdAt(i);
      if (!mobs.IsAlive(mid)) continue;
      Vec3 lo, hi;
      if (!mobs.MobBodyBox(mid, lo, hi)) continue;
      const float d = boxDist(from, lo, hi);
      if (d <= best) {
        best = d;
        id = mid;
        out = (lo + hi) * 0.5f;
        found = true;
      }
    }
    // Every player, as the figure box the player's own probe uses (~1.7 m
    // round `pos`), nearest wins.
    for (SessionTick& st : p.players) {
      if (!st.s) continue;
      const Vec3 dp = st.s->player.pos - from;
      const float d = std::max(0.0f, dp.len() - 9.0f);
      if (d <= radius && (!found || d < best)) {
        best = d;
        id = ai::kPlayerActorBase + (uint64_t)st.s->index;
        out = st.s->player.pos;
        found = true;
      }
    }
    return found;
  };
  bp.bodyPos = [](void* c, uint64_t id, Vec3& out) {
    ProbeCtx& p = *(ProbeCtx*)c;
    if (PlayerSession* s = SessionFor(p.players, id)) {
      out = s->player.pos;
      return true;
    }
    if (!p.w->mobs.IsAlive(id)) return false;
    out = BoxCentre(p.w->mobs, id);
    return true;
  };
  bp.bodyAt = [](void* c, uint64_t h, Vec3& out) {
    BodyTransform xf;
    if (!((ProbeCtx*)c)->w->phys.GetTransform(h, xf)) return false;
    out = xf.pos;
    return true;
  };
  // A creature's seeking bolt homes on what IT is fighting.
  bp.nearestTarget = [](void* c, Vec3, uint64_t casterId, Vec3& out) {
    ProbeCtx& p = *(ProbeCtx*)c;
    const ai::Brain* b = p.w->mobs.MobBrain(casterId);
    if (b == nullptr || !b->hasTarget) return false;
    out = b->targetPos;
    return true;
  };
  return bp;
}

// Is a world point OUTSIDE a contained demon's circle? (cast_out severed)
bool OutsideRing(const LiveDemon& ld, float x, float z) { return !ld.circle.Inside(x, z); }

// ---- THE EMISSION, OWNED LIKE THE PLAYER'S ----------------------------------------------
//
// session.cpp phase I's handling of one SpellEmission, for a creature: wards
// first (every VM's), then each stream to where the player's goes.
void Route(TickAuthorityCtx& w, std::span<SessionTick> players, MobCastWorld& m,
           MobCaster& c, SpellEmission& em, uint32_t tick, OpBatch& out) {
  // cast_out SEVERED (D3) for a contained demon: what its spell would do
  // OUTSIDE the ring does not happen. Ops, blasts, spawns by where they land;
  // a wind by where its far end reaches; a push by where the body is.
  if (const LiveDemon* ld = ContainedDemon(w, c.mobId);
      ld != nullptr && DemonSealHook()(w, c.mobId, "cast_out")) {   // D3 hook
    const size_t before = em.ops.size() + em.explosions.size() + em.spawns.size() +
                          em.winds.size() + em.bodyImpulses.size();
    std::erase_if(em.ops, [&](const BrushOp& o) {
      return OutsideRing(*ld, (float)o.x + 0.5f, (float)o.z + 0.5f);
    });
    std::erase_if(em.explosions, [&](const ExplosionOp& o) {
      return OutsideRing(*ld, (float)o.x + 0.5f, (float)o.z + 0.5f);
    });
    std::erase_if(em.spawns, [&](const ParticleSpawn& o) {
      return OutsideRing(*ld, SpellFxToFloat(o.px), SpellFxToFloat(o.pz));
    });
    std::erase_if(em.winds, [&](const WindPrim& o) {
      // The far end of the jet (a burst's reach is its radius).
      const float reach = (float)(o.kind == kWindPrimCone ? o.reach : o.radius);
      const float fx = (float)o.x + 0.5f + (float)o.dirX / 65536.0f * reach;
      const float fz = (float)o.z + 0.5f + (float)o.dirZ / 65536.0f * reach;
      return OutsideRing(*ld, (float)o.x + 0.5f, (float)o.z + 0.5f) || OutsideRing(*ld, fx, fz);
    });
    std::erase_if(em.bodyImpulses, [&](const SpellBodyImpulse& bi) {
      Vec3 at{};
      if (PlayerSession* s = SessionFor(players, bi.target)) at = s->player.pos;
      else at = BoxCentre(w.mobs, bi.target);
      return OutsideRing(*ld, at.x, at.z);
    });
    const size_t after = em.ops.size() + em.explosions.size() + em.spawns.size() +
                         em.winds.size() + em.bodyImpulses.size();
    m.stats.ringRefused += before - after;
  }
  // WARDS: every creature's (its own included) and every player's.
  {
    int refused = 0;
    for (MobCaster& o : m.casters)
      refused += o.spells.FilterStreams(em.ops, em.explosions, em.spawns, em.winds,
                                        &em.strikes, &em.summons);
    for (SessionTick& st : players)
      if (st.s)
        refused += st.s->spells.FilterStreams(em.ops, em.explosions, em.spawns, em.winds,
                                              &em.strikes, &em.summons);
    m.stats.filtered += (uint64_t)refused;
  }
  // Brush ops: the creatures' shared share of the tick budget.
  for (const BrushOp& b : em.ops) {
    if (m.stats.opsThisTick >= MobCastWorld::kOpsPerTick || out.ops.size() >= kMaxOpsPerTick) {
      m.stats.opsDropped++;
      continue;
    }
    out.ops.push_back(b);
    m.stats.opsThisTick++;
  }
  // Blasts go off in the PRIMARY's explosion slot (phase K, this tick), the
  // grenade path: crater scan, body damage, carving, impulse.
  if (!players.empty() && players[0].s)
    for (const ExplosionOp& e : em.explosions) players[0].s->pendingBlasts.push_back(e);
  for (const ParticleSpawn& p : em.spawns) out.spawns.push_back(p);
  for (WindPrim wp : em.winds) {
    wp.spawnTick = tick;
    wp.ownerId = c.mobId;
    WindPrims().Spawn(wp);
  }
  m.stats.unsupported += em.strikes.size() + em.summons.size();
  // Sustained lift on a body, whoever's.
  for (const SpellBodyImpulse& bi : em.bodyImpulses) {
    if (PlayerSession* s = SessionFor(players, bi.target)) {
      s->avatar.BindPlayer(&s->player);
      s->avatar.AddBodyVelocity(bi.vps);
    } else {
      w.mobs.LiftMob(bi.target, bi.vps);
    }
  }
  if (em.casterImpulseVps.y != 0.0f) w.mobs.LiftMob(c.mobId, em.casterImpulseVps);
  for (const SpellRestore& rs : em.restores) w.mobs.RestoreMob(rs.casterId, rs.material, rs.count);
  // BOMBS ARE DEBRIS (session.cpp phase I's block, for a creature's bomb).
  for (const SpellBodyRequest& rq : em.bodyRequests) {
    const float r = rq.radius;
    std::vector<DebrisVoxel> ball;
    const int ext = (int)std::ceil(r);
    for (int z = -ext; z < ext; z++)
      for (int y = -ext; y < ext; y++)
        for (int x = -ext; x < ext; x++) {
          const float dx = x + 0.5f, dy = y + 0.5f, dz = z + 0.5f;
          if (dx * dx + dy * dy + dz * dz <= r * r)
            ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, (uint16_t)rq.material});
        }
    BodyTransform xf{};
    xf.pos = rq.pos;
    xf.quat[3] = 1;
    const float density =
        rq.material < w.mats.size() ? (float)w.mats[rq.material].gpu.density : 2000.0f;
    const uint64_t h = w.phys.CreateSphereBody(rq.pos, r, density);
    if (!h) continue;
    w.phys.SetBodyVelocity(h, rq.vel);
    w.phys.SetBodyRole(h, Physics::BodyRole::Debris);
    w.debris.AdoptBody(h, std::move(ball), xf);
    c.spells.AdoptBody(rq.token, h);
  }
  for (uint64_t h : em.bodyDone) w.debris.DestroyBody(h);
  // The bills: statuses / beams per tick, and the wildcard's bill on resolve.
  // MANA ONLY -- a creature does not bleed for its magic; when the pool cannot
  // pay, everything it sustains drops.
  int32_t bill = em.billOnResolve;
  for (const SpellBill& sb : em.bills)
    if (sb.casterId == c.mobId) bill += sb.amount;
  if (bill > 0) {
    if (bill <= c.mana.mana) {
      c.mana.mana -= bill;
    } else {
      c.mana.mana = 0;
      c.spells.DropAll(c.mobId);
    }
  }
}

// A creature's cast origin and the line to its aim point.
struct Aim {
  Vec3 from{}, to{}, dir{0, 1, 0};
  bool ok = false;
};

Aim AimFor(TickAuthorityCtx& w, const KitSpell& ks, uint64_t mobId, const ai::CastRequest& req,
           const CastList& list) {
  Aim a;
  Vec3 size{};
  const Vec3 c = BoxCentre(w.mobs, mobId, &size);
  if (ks.aim == KitAim::Self) {
    a.from = a.to = c;
    a.dir = Vec3{0, 1, 0};
    a.ok = true;
    return a;
  }
  // From the upper chest.
  const Vec3 chest{c.x, c.y + size.y * 0.2f, c.z};
  Vec3 to = req.targetPoint;
  if (ks.aim == KitAim::Feet) {
    Cells cells{WorldSpellProbe(w.world), &w.mats};
    int fy = 0;
    if (!FloorUnder(cells, (int)std::floor(to.x), (int)std::floor(to.z), (int)std::floor(to.y) + 2,
                    32, fy))
      return a;   // the target's floor is unseen: nothing to aim at
    to.y = (float)fy - 0.5f;   // into the top cell of the floor, so the bolt meets it
  }
  // WHERE IT WILL BE (flight only): the target's velocity over the flight
  // time, bounded so a sprint does not throw the aim into the next field.
  if (ks.lead && !list.casts.empty() && list.casts[0].delivery.mech == DeliveryMech::Flight &&
      list.casts[0].delivery.speedFx > 0) {
    const float vTick = (float)list.casts[0].delivery.speedFx / (float)kSpellFxOne;
    const float t = (to - chest).len() / std::max(0.5f, vTick) / 30.0f;   // seconds
    Vec3 lead{req.targetVel.x * t, 0.0f, req.targetVel.z * t};
    const float m = lead.len();
    if (m > 8.0f) lead = lead * (8.0f / m);
    to = to + lead;
  }
  Vec3 d = to - chest;
  const float len = d.len();
  if (len < 1e-3f) return a;
  a.dir = d * (1.0f / len);
  // Out of its own body before the flight starts (bodyHit also ignores its
  // limbs, but a muzzle inside the collider is the backfire nobody cast).
  a.from = chest + a.dir * (std::max(size.x, size.z) * 0.5f + 1.5f);
  a.to = to;
  a.ok = true;
  return a;
}

// ---- BLINK -------------------------------------------------------------------------------
MobCastOutcome Blink(TickAuthorityCtx& w, MobCaster& c, const KitSpell& ks,
                     const ai::CastRequest& req, uint32_t tick, CastEvent& ev) {
  Vec3 size{};
  const Vec3 centre = BoxCentre(w.mobs, c.mobId, &size);
  Vec3 flat{req.targetPoint.x - centre.x, 0.0f, req.targetPoint.z - centre.z};
  const float dist = flat.len();
  if (dist < 1e-3f) return MobCastOutcome::BlinkNoFloor;
  flat = flat * (1.0f / dist);
  float hop = ks.distance;
  if (ks.toward) hop = std::min(hop, std::max(0.0f, dist - ks.standoff));
  if (hop < 3.0f) return MobCastOutcome::NothingInRange;   // already there
  const Vec3 dirH = ks.toward ? flat : flat * -1.0f;
  const float dx = centre.x + dirH.x * hop, dz = centre.z + dirH.z * hop;
  ev.from = centre;
  // THE FENCE FIRST, whatever D3 says: a contained demon never blinks out of
  // its circle (D1's containment is a property of the circle, not a seal).
  if (const LiveDemon* ld = ContainedDemon(w, c.mobId)) {
    if (!ld->circle.Inside(dx, dz)) return MobCastOutcome::BlinkFenced;
    if (DemonSealHook()(w, c.mobId, "blink")) return MobCastOutcome::BlinkSevered;   // D3 hook
  }
  // A FLOOR, from a little above the body's own feet, and HEADROOM over it.
  const Vec3 origin = w.mobs.MobOrigin(c.mobId);
  Cells cells{WorldSpellProbe(w.world), &w.mats};
  int floorY = 0;
  if (!FloorUnder(cells, (int)std::floor(dx), (int)std::floor(dz), (int)std::floor(origin.y) + 8,
                  24, floorY))
    return MobCastOutcome::BlinkNoFloor;
  const int tall = std::max(2, (int)std::ceil(size.y));
  for (int y = floorY; y < floorY + tall; y++) {
    bool known = true;
    if (!cells.Open((int)std::floor(dx), y, (int)std::floor(dz), known))
      return MobCastOutcome::BlinkNoFloor;
  }
  // THE LINE, if the entry wants one: chest to the landing's chest, through the
  // snapshot, every half voxel.
  if (ks.los) {
    const Vec3 a{centre.x, centre.y + size.y * 0.2f, centre.z};
    const Vec3 b{dx, (float)floorY + size.y * 0.7f, dz};
    const Vec3 d = b - a;
    const int steps = std::max(1, (int)std::ceil(d.len() * 2.0f));
    for (int i = 1; i < steps; i++) {
      const Vec3 p = a + d * ((float)i / (float)steps);
      bool known = true;
      if (!cells.Open((int)std::floor(p.x), (int)std::floor(p.y), (int)std::floor(p.z), known) ||
          !known)
        return MobCastOutcome::BlinkNoSight;
    }
  }
  const Vec3 newOrigin{dx - size.x * 0.5f, (float)floorY, dz - size.z * 0.5f};
  if (!w.mobs.BlinkMob(c.mobId, newOrigin)) return MobCastOutcome::BlinkNoFloor;
  c.blinkReadyAt = tick + (uint32_t)ks.cooldownTicks;
  c.blinks++;
  ev.to = Vec3{dx, (float)floorY + size.y * 0.5f, dz};
  return MobCastOutcome::Blinked;
}

void EnsureKit(MobCastWorld& m, bool reload) {
  if (m.kitLoaded && !reload) return;
  std::string log;
  LoadCreatureKit(sandvox::AssetDir() + "/demons/spells", m.kit, log);
  m.kitLoaded = true;
  if (!log.empty() && log != m.kitLog) std::fprintf(stderr, "%s", log.c_str());
  m.kitLog = log;
}

const ai::CastTuning* TuningOf(const MobSystem& mobs, uint64_t mobId) {
  const ai::Brain* b = mobs.MobBrain(mobId);
  if (b == nullptr) return nullptr;
  const ai::Profile* p = mobs.Behaviors().At(b->profile);
  return p != nullptr ? &p->cast : nullptr;
}

int LiveOf(const MobCaster& c) {
  return c.spells.LiveCount() + c.spells.BombCount();
}

}  // namespace

// ---- the world ------------------------------------------------------------------------------

MobCastWorld& MobCasters(TickAuthorityCtx& w) {
  if (!w.mobCast) w.mobCast = std::make_shared<MobCastWorld>();
  return *w.mobCast;
}

MobCastOutcome MobCastServe(TickAuthorityCtx& w, std::span<SessionTick> players,
                            const ai::CastRequest& req, uint32_t tick,
                            const std::string& spell, OpBatch& out) {
  MobCastWorld& m = MobCasters(w);
  m.stats.requests++;
  CastEvent ev;
  ev.tick = tick;
  ev.mobId = req.mobId;
  auto refuse = [&](MobCastOutcome o) {
    ev.outcome = o;
    m.stats.refused++;
    if (MobCaster* c = m.Find(req.mobId)) c->refused++;
    Log(m, ev);
    return o;
  };
  const ai::CastTuning* ct = TuningOf(w.mobs, req.mobId);
  if (ct == nullptr || !w.mobs.IsAlive(req.mobId)) return refuse(MobCastOutcome::NoRoom);
  // THE CREATURE'S VM: created the first time it casts (and the kit is read
  // again then, so a kit edit is live for the next creature that starts).
  MobCaster* c = m.Find(req.mobId);
  if (c == nullptr) {
    if (m.casters.size() >= MobCastWorld::kMaxCasters) return refuse(MobCastOutcome::NoRoom);
    EnsureKit(m, true);
    m.casters.emplace_back();
    c = &m.casters.back();
    c->mobId = req.mobId;
    c->mana.mana = c->mana.manaMax = ct->mana;
    c->mana.regenPerMillePerTick = ct->regenPerMille;
  }
  c->spells.SetLibrary(&w.glyphs);
  // ---- THE DRAW: what on its list it can do RIGHT NOW --------------------------
  struct Cand {
    const KitSpell* ks;
    CastList list;
  };
  std::vector<Cand> cands;
  auto consider = [&](const std::string& name) {
    const KitSpell* ks = m.kit.Find(name);
    if (ks == nullptr) return;
    if (req.distance < ks->rangeMin || req.distance > ks->rangeMax) return;
    if (ks->blink) {
      if (tick < c->blinkReadyAt) return;
      cands.push_back({ks, CastList{}});
      return;
    }
    if (ks->aim != KitAim::Self && LiveOf(*c) >= ct->maxLive) return;
    if (ks->once)
      for (const SpellStatus& st : c->spells.Statuses())
        if (st.casterId == c->mobId && st.target == c->mobId) return;
    std::string unknown;
    CastList list = CompileSpell(w.glyphs, StackOf(w.glyphs, ks->words, &unknown));
    if (list.Empty() || !unknown.empty()) return;
    // MANA ONLY: a creature never overcasts into its own body.
    if (ResolveCast(c->mana, 0, list.manaCost).outcome != CastOutcome::Normal) return;
    cands.push_back({ks, std::move(list)});
  };
  if (!spell.empty()) {
    consider(spell);
  } else {
    // Every entry of the list is one ticket: a repeated name is drawn more.
    for (const std::string& s : ct->spells) consider(s);
  }
  ev.spell = spell;
  if (cands.empty()) return refuse(MobCastOutcome::NothingInRange);
  const uint32_t pick =
      rng::Hash3((uint32_t)req.mobId ^ 0xD4CA57u, tick, (uint32_t)cands.size()) %
      (uint32_t)cands.size();
  Cand& cd = cands[pick];
  ev.spell = cd.ks->name;
  if (cd.ks->blink) {
    const MobCastOutcome o = Blink(w, *c, *cd.ks, req, tick, ev);
    if (o != MobCastOutcome::Blinked) return refuse(o);
    ev.outcome = o;
    m.stats.blinked++;
    Log(m, ev);
    return o;
  }
  // ---- THE CAST: the player's own Cast(), from this body -------------------------
  const Aim a = AimFor(w, *cd.ks, req.mobId, req, cd.list);
  if (!a.ok) return refuse(MobCastOutcome::NothingInRange);
  ProbeCtx pc;
  pc.w = &w;
  pc.players = players;
  pc.caster = req.mobId;
  if (const Mob* mob = w.mobs.FindMobById(req.mobId); mob && mob->Def())
    for (int i = 0; i < (int)mob->Def()->limbs.size(); i++)
      if (const uint64_t h = w.mobs.LimbBody(req.mobId, i)) pc.ownBodies.push_back(h);
  const SpellBodyProbe bp = MakeProbe(pc);
  const SpellProbe probe = WorldSpellProbe(w.world);
  const SpellFxVec selfAt = Fx(a.from);
  CasterHealth noBody;   // reads 0: an unaffordable cast was never drawn
  SpellEmission em;
  const int32_t manaBefore = c->mana.mana;
  const CastResult res = c->spells.Cast(cd.list, c->mana, noBody, req.mobId, Fx(a.from),
                                        Fx(a.dir), tick, em, &probe,
                                        cd.ks->aim == KitAim::Self ? &selfAt : nullptr, &bp);
  if (res.outcome == CastOutcome::Nothing) return refuse(MobCastOutcome::NothingInRange);
  if (cd.ks->releaseAfterTicks > 0) c->releaseAfter = cd.ks->releaseAfterTicks;
  Route(w, players, m, *c, em, tick, out);
  c->casts++;
  m.stats.cast++;
  ev.outcome = MobCastOutcome::Cast;
  ev.from = a.from;
  ev.to = a.to;
  ev.cost = manaBefore - c->mana.mana;
  Log(m, ev);
  m.stats.maxLiveSeen = std::max(m.stats.maxLiveSeen, LiveOf(*c));
  return MobCastOutcome::Cast;
}

void MobCastTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
                 OpBatch& out) {
  // RULE 2: nothing asked and nobody casting = nothing to do and no world.
  if (w.mobs.CastRequests().empty() && (!w.mobCast || w.mobCast->casters.empty())) return;
  MobCastWorld& m = MobCasters(w);
  m.stats.opsThisTick = 0;
  // ---- this tick's requests, in the order MobSystem issued them ----------------
  {
    const std::vector<ai::CastRequest> reqs = w.mobs.CastRequests();
    w.mobs.ClearCastRequests();
    int served = 0;
    for (const ai::CastRequest& r : reqs) {
      if (served++ >= MobCastWorld::kCastsPerTick) {
        m.stats.requests++;
        m.stats.refused++;
        continue;
      }
      MobCastServe(w, players, r, tick, std::string(), out);
    }
  }
  // ---- every creature's VM, one tick -------------------------------------------------
  for (MobCaster& c : m.casters) {
    c.spells.SetLibrary(&w.glyphs);
    if (!c.gone && !w.mobs.IsAlive(c.mobId)) {
      // Dead or gone: what it sustains drops; its bolts in flight still land.
      c.gone = true;
      c.spells.DropAll(c.mobId);
    }
    if (!c.gone) {
      c.mana.Tick();
      c.mana.reserved = c.spells.ReservationFor(c.mobId);
    }
    ProbeCtx pc;
    pc.w = &w;
    pc.players = players;
    pc.caster = c.mobId;
    if (!c.gone)
      if (const Mob* mob = w.mobs.FindMobById(c.mobId); mob && mob->Def())
        for (int i = 0; i < (int)mob->Def()->limbs.size(); i++)
          if (const uint64_t h = w.mobs.LimbBody(c.mobId, i)) pc.ownBodies.push_back(h);
    const SpellBodyProbe bp = MakeProbe(pc);
    SpellEmission em;
    c.spells.Tick(tick, w.world, w.classOf, em, &bp);
    Route(w, players, m, c, em, tick, out);
    // LIFT, THEN DROP: a status this creature put on ANOTHER body lets go
    // `releaseAfterTicks` after it first held (KitSpell::releaseAfterTicks).
    if (c.releaseAfter > 0) {
      std::vector<std::pair<uint32_t, uint32_t>> keep;
      for (const SpellStatus& st : c.spells.Statuses()) {
        if (st.target == 0 || st.target == c.mobId) continue;
        uint32_t first = tick;
        for (const auto& s : c.seen)
          if (s.first == st.id) first = s.second;
        keep.push_back({st.id, first});
      }
      c.seen.clear();
      for (const auto& s : keep) {
        if (tick - s.second >= (uint32_t)c.releaseAfter) {
          if (c.spells.DropStatus(s.first)) m.stats.released++;
        } else {
          c.seen.push_back(s);
        }
      }
    }
    // cast_out SEVERED (D3) for a contained demon: its carriers die AT THE
    // RING, before anything they carry can land outside it.
    if (const LiveDemon* ld = ContainedDemon(w, c.mobId);
        ld != nullptr && DemonSealHook()(w, c.mobId, "cast_out")) {   // D3 hook
      m.stats.ringRefused += (uint64_t)c.spells.RefuseCarriers(
          [](void* ctx, const SpellProjectile& p) {
            return OutsideRing(*(const LiveDemon*)ctx, SpellFxToFloat(p.pos.x),
                               SpellFxToFloat(p.pos.z));
          },
          (void*)ld);
    }
    m.stats.maxLiveSeen = std::max(m.stats.maxLiveSeen, LiveOf(c));
  }
  // ---- wards across VMs: yours absorb its bolts, its absorb yours ------------------
  for (MobCaster& c : m.casters) {
    for (SessionTick& st : players) {
      if (!st.s) continue;
      m.stats.absorbed += (uint64_t)c.spells.AbsorbForeign(st.s->spells);
      m.stats.absorbed += (uint64_t)st.s->spells.AbsorbForeign(c.spells);
    }
    for (MobCaster& o : m.casters)
      if (&o != &c) m.stats.absorbed += (uint64_t)c.spells.AbsorbForeign(o.spells);
  }
  // ---- forget the drained dead --------------------------------------------------------
  std::erase_if(m.casters, [](const MobCaster& c) {
    return c.gone && c.spells.LiveCount() == 0 && c.spells.BombCount() == 0 &&
           c.spells.Statuses().empty() && c.spells.Echoes().empty() &&
           c.spells.Beams().empty() && c.spells.Filters().empty();
  });
}

void MobCastWardFilter(TickAuthorityCtx& w, OpBatch& out) {
  if (!w.mobCast) return;
  std::vector<WindPrim> noWinds;
  for (MobCaster& c : w.mobCast->casters)
    w.mobCast->stats.filtered +=
        (uint64_t)c.spells.FilterStreams(out.ops, out.exps, out.spawns, noWinds);
}

void MobCastAppendLive(const TickAuthorityCtx& w, std::vector<const SpellProjectile*>& out) {
  if (!w.mobCast) return;
  for (const MobCaster& c : w.mobCast->casters)
    for (const SpellProjectile& p : c.spells.Live()) out.push_back(&p);
}
