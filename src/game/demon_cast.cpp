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
#include "game/demon_seals.h"
#include "game/demon_talk.h"
#include "game/mob.h"
#include "game/pierce.h"  // SpellMatterStrike: a creature's spell matter meets a body
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
    case MobCastOutcome::BlinkSevered: return "blink: severed";
    case MobCastOutcome::NoRoom: return "no room";
    case MobCastOutcome::Forbidden: return "forbidden by contract";
    case MobCastOutcome::Gesturing: return "in the hand";
    case MobCastOutcome::Busy: return "busy casting";
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
  // The tick's particle stream: the gore a spell's matter tears out of a body
  // (docs/PLAN_projectiles.md D) leaves with the batch, as a wound's does.
  std::vector<ParticleSpawn>* gore = nullptr;
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
  // THE MATTER IS THE PROJECTILE (docs/PLAN_projectiles.md D): the creature's
  // matter strikes a body the way the player's does, its own limbs ignored.
  if (pc.gore != nullptr)
    bp.matterHit = [](void* c, const SpellBodyShot& shot, SpellBodyStrike& r) {
      ProbeCtx& p = *(ProbeCtx*)c;
      SpellMatterStrike(p.w->phys, p.w->mobs, p.w->debris, p.w->world, *p.gore, shot,
                        p.ownBodies, r);
      return r.hit;
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
      float d = boxDist(from, lo, hi);
      // ...and to its TRAILING parts, which the box leaves out (the player's
      // probe, session.cpp bodyIdAt, says why).
      if (d > best)
        if (const Mob* m = mobs.FindCreature(mid))
          for (int li = 0; li < m->LimbCount(); li++) {
            if (!m->LimbDefAt(li).trailing ||
                mobs.LimbVoxelsNearWorld(mid, li, from, best) == 0)
              continue;
            d = std::min(d, (mobs.LimbNearestVoxelWorld(mid, li, from) - from).len());
          }
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
  // a wind by where its far end reaches; a push by where the body is. (Per
  // element, the test demon::AllowCastOut makes for a carrier: contained,
  // severed, and outside the circle.)
  if (const LiveDemon* ld = ContainedDemon(w, c.mobId);
      ld != nullptr && demon::ChannelSevered(w, c.mobId, demon::Channel::CastOut)) {
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
  // ai::LeadPoint, the one lead every creature's projectile takes (the NPC
  // archers' too, game/mob_archery.cpp).
  if (ks.lead && !list.casts.empty() && list.casts[0].delivery.mech == DeliveryMech::Flight &&
      list.casts[0].delivery.speedFx > 0) {
    const float vTick = (float)list.casts[0].delivery.speedFx / (float)kSpellFxOne;
    to = ai::LeadPoint(chest, to, req.targetVel, vTick, 1.0f, 8.0f);
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
MobCastOutcome Blink(TickAuthorityCtx& w, std::span<SessionTick> players, MobCaster& c,
                     const KitSpell& ks, const ai::CastRequest& req, uint32_t tick,
                     CastEvent& ev) {
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
  const Mob* mob = w.mobs.FindMobById(c.mobId);
  if (mob == nullptr || !mob->Alive() || mob->Ragdolled()) return MobCastOutcome::BlinkNoFloor;
  // THE SEALS LAST, once the hop is otherwise certain (D3, demon_seals.h): a
  // contained demon's quicksilver-severed `blink` refuses it; an unsevered one
  // ALLOWS it -- and a landing outside the circle unbinds the demon there and
  // then. That is the loophole the seal exists to close.
  const Vec3 landing{dx, (float)floorY + size.y * 0.5f, dz};
  if (!demon::AllowBlink(w, players, c.mobId, landing, tick)) return MobCastOutcome::BlinkSevered;
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

namespace {

// How a served request ends. Now: cast on the spot from the chest (scripted
// casts, a creature with no hand). Pick: draw and check only, and report what
// was drawn -- the spell goes into a hand (MobCastBegin). Release: the drawn
// spell, by name, leaving the hand at `handAt` (MobHandCast's release tick).
enum class ServeMode : uint8_t { Now = 0, Pick, Release };
struct Picked {
  std::string spell;
  int gesture = -1;
  uint32_t color = 0;
  KitAim aim = KitAim::Body;
};

MobCastOutcome ServeImpl(TickAuthorityCtx& w, std::span<SessionTick> players,
                         const ai::CastRequest& req, uint32_t tick, const std::string& spell,
                         OpBatch& out, ServeMode mode, const Vec3* handAt, Picked* picked) {
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
    const MobCastOutcome o = Blink(w, players, *c, *cd.ks, req, tick, ev);
    if (o != MobCastOutcome::Blinked) return refuse(o);
    ev.outcome = o;
    m.stats.blinked++;
    Log(m, ev);
    return o;
  }
  // ---- THE CAST: the player's own Cast(), from this body -------------------------
  // D5: a BOUND demon's contract may forbid casting at this target -- any
  // spell, or only one whose footprint TAGS match the clause (`never cast
  // creates:fire at me`; game/contract.h). Nothing has been emitted yet and no
  // mana spent, so a refusal costs the creature only the cadence the AI
  // already spent on the request.
  if (!demon::AllowCastAt(w, req.mobId, req.targetId, &cd.ks->tags)) {
    m.stats.forbidden++;
    return refuse(MobCastOutcome::Forbidden);
  }
  // THE HAND PATH'S DRAW: the spell, its gesture and its light; nothing cast,
  // nothing logged -- the event is the release's.
  if (mode == ServeMode::Pick) {
    if (picked) {
      picked->spell = cd.ks->name;
      picked->gesture = CastGesture(w.glyphs, cd.list);
      picked->aim = cd.ks->aim;
      picked->color = CastLightColor(w.glyphs, w.mats, cd.list);
    }
    return MobCastOutcome::Gesturing;
  }
  Aim a = AimFor(w, *cd.ks, req.mobId, req, cd.list);
  if (!a.ok) return refuse(MobCastOutcome::NothingInRange);
  // FROM THE HAND (MobHandCast's release): one step out of the fist along the
  // line from the fist to the aim point, as the player's leaves its hand. A
  // spell on the caster itself still resolves at the body.
  if (handAt != nullptr && cd.ks->aim != KitAim::Self) {
    const Vec3 d = a.to - *handAt;
    const float len = d.len();
    if (len > 1e-3f) {
      a.dir = d * (1.0f / len);
      a.from = *handAt + a.dir * 1.5f;
    }
  }
  ProbeCtx pc;
  pc.w = &w;
  pc.players = players;
  pc.caster = req.mobId;
  pc.gore = &out.spawns;
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

// ---- THE CAST IN THE HAND (MobHandCast) -----------------------------------------------

// Where the target is NOW: a live creature's box, or a player's body. False
// for a target that is gone (the cast then flies at its last known point).
bool TargetNow(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t id, Vec3& out) {
  if (id == 0) return false;
  for (SessionTick& p : players)
    if (p.s && ai::kPlayerActorBase + (uint64_t)p.s->index == id) {
      out = p.s->player.pos;
      return true;
    }
  if (w.mobs.IsAlive(id)) {
    out = BoxCentre(w.mobs, id);
    return true;
  }
  return false;
}

bool HandCanCast(const Mob& mob, Hand h) {
  return mob.HandPart(h) >= 0 && mob.HandUsable(h) && !mob.HeldBorrowed(h);
}

// WHICH HAND: an empty one that works, right first -- the player's own rule,
// a spell needs an empty hand, and a free fist is one. Else the OFF hand's
// item is put away (the weapon it swings stays out), else the main hand's.
int ChooseCastHand(const Mob& mob) {
  for (Hand h : {Hand::Right, Hand::Left})
    if (HandCanCast(mob, h) && mob.HeldItem(h).empty()) return HandIndex(h);
  const Hand main = mob.StrikeHand(), off = OtherHand(main);
  if (HandCanCast(mob, off)) return HandIndex(off);
  if (HandCanCast(mob, main)) return HandIndex(main);
  return -1;
}

// PUT AWAY what the hand holds, AS IT IS (coat, damage, contents): into the
// sheath when it is a blade and the sheath is free, else the first free bag
// slot. False when there is nowhere to put it (the hand then cannot cast).
bool StowHand(MobHandCast& hc, Mob& mob, const ItemLibrary& items, Hand h) {
  const std::string name = mob.HeldItem(h);
  if (name.empty()) return true;
  if (mob.HeldBorrowed(h)) return false;
  ItemStack st;
  st.name = name;
  st.count = 1;
  mob.CaptureHeld(h, st.damage);
  st.contents = mob.HeldContents(h);
  Kit& kit = mob.KitMut();
  KitRef ref;
  const int sheath = (int)EquipSlotId::Sheath;
  const ItemDef* def = items.Named(name);
  if (def && EquipSlotAccepts(sheath, def->kind) && kit.equip.slots[sheath].Empty()) {
    ref = KitRef{KitSpace::Equip, sheath};
  } else {
    for (int i = 0; i < Bag::kSlots && !ref.Valid(); i++)
      if (kit.bag.slots[i].Empty()) ref = KitRef{KitSpace::Bag, i};
  }
  ItemStack* to = ref.Valid() ? kit.Resolve(ref) : nullptr;
  if (to == nullptr) return false;
  *to = st;
  mob.EquipItem(nullptr, h);
  hc.stowed = true;
  hc.stowedAt = ref;
  hc.stowedItem = name;
  hc.stows++;
  return true;
}

// ...AND DRAW IT BACK into the same hand, as it was. Only what was put away,
// only if it is still there and the hand is still empty (a corpse looted, a
// hand that picked something else up: then it stays where it is).
void DrawBack(MobHandCast& hc, Mob& mob, const ItemLibrary& items) {
  if (!hc.stowed) return;
  hc.stowed = false;
  const Hand h = HandAt(hc.hand);
  const ItemStack* st = mob.GetKit().Resolve(hc.stowedAt);
  if (st == nullptr || st->name != hc.stowedItem || !mob.HeldItem(h).empty() ||
      !mob.HandUsable(h))
    return;
  const ItemDef* def = items.Named(hc.stowedItem);
  if (def == nullptr) return;
  ItemStack taken = mob.KitTake(hc.stowedAt);
  if (!mob.EquipItemAsWas(def, h, &taken.damage)) {
    *mob.KitMut().Resolve(hc.stowedAt) = taken;   // refused: it stays put away
    return;
  }
  mob.SetHeldContents(taken.contents, h);
  hc.draws++;
}

void EndHandCast(MobHandCast& hc, Mob* mob, const ItemLibrary& items, bool drawBack) {
  if (mob != nullptr) {
    DropCastArm(*mob, hc.arm);
    mob->SetCastArm(Mob::CastArm{});
    for (int k = 0; k < kHands; k++) {
      SpellReadyClip(*mob, HandAt(k), false, hc.clipOn[k]);
      mob->SetSpellLight(HandAt(k), 0);
    }
    if (drawBack) DrawBack(hc, *mob, items);
    mob->SetCastBusy(false);
  }
  hc.phase = MobHandCast::Phase::Idle;
  hc.ticks = 0;
  hc.run = CastGestureRun{};
  hc.arm = CastArmState{};
  hc.gesture = -1;
}

// One tick of every creature's cast in the hand: the raise, the gesture and
// its release, the linger, the let-go and the draw-back; and the arm's pose.
void StepHandCasts(TickAuthorityCtx& w, std::span<SessionTick> players, MobCastWorld& m,
                   uint32_t tick, OpBatch& out) {
  const SpellGestureLib& lib = w.glyphs.gestures;
  for (MobCaster& c : m.casters) {
    MobHandCast& hc = c.hand;
    if (hc.phase == MobHandCast::Phase::Idle) continue;
    Mob* mob = w.mobs.FindMobById(c.mobId);
    const Hand h = HandAt(hc.hand);
    // The cast dies with the body (or its arm): no spell leaves, and what was
    // put away stays in the kit -- it is the corpse's, loot like the rest.
    if (mob == nullptr || c.gone || !mob->Alive() || mob->Ragdolled()) {
      EndHandCast(hc, mob, w.items, false);
      continue;
    }
    if (!mob->HandUsable(h) && hc.phase != MobHandCast::Phase::Lower) {
      hc.phase = MobHandCast::Phase::Lower;
      hc.ticks = 0;
      DropCastArm(*mob, hc.arm);
      hc.run = CastGestureRun{};
    }
    // The line to the target as it is now: the arm turns with it, and the
    // release flies at it.
    Vec3 size{};
    const Vec3 centre = BoxCentre(w.mobs, c.mobId, &size);
    Vec3 tgt;
    if (TargetNow(w, players, hc.req.targetId, tgt)) {
      hc.req.targetPoint = tgt;
      hc.req.distance = (tgt - centre).len();
    }
    const Vec3 chest{centre.x, centre.y + size.y * 0.2f, centre.z};
    const Vec3 d = hc.req.targetPoint - chest;
    if (d.len() > 1e-3f) hc.aimDir = d.normalized();
    switch (hc.phase) {
      case MobHandCast::Phase::Idle: break;
      case MobHandCast::Phase::Raise:
        SpellReadyClip(*mob, h, true, hc.clipOn[hc.hand]);
        if (++hc.ticks >= MobHandCast::kRaiseTicks) {
          BeginCastGesture(hc.run, hc.arm, hc.gesture, hc.hand);
          hc.phase = MobHandCast::Phase::Gesture;
          hc.ticks = 0;
        }
        break;
      case MobHandCast::Phase::Gesture: {
        SpellReadyClip(*mob, h, true, hc.clipOn[hc.hand]);
        const SpellGesture* g = lib.At(hc.run.gesture);
        if (g == nullptr) {
          hc.phase = MobHandCast::Phase::Hold;
          hc.ticks = 0;
          break;
        }
        if (CastGestureReleases(hc.run, *g)) {
          hc.run.fired = true;
          hc.releases++;
          Vec3 at;
          const bool fromHand = SpellHandWorld(w.mobs, w.phys, c.mobId, h, at);
          ServeImpl(w, players, hc.req, tick, hc.spell, out, ServeMode::Release,
                    fromHand ? &at : nullptr, nullptr);
        }
        AdvanceCastGesture(hc.run, *g, false);
        if (hc.run.gesture < 0) {
          hc.phase = MobHandCast::Phase::Hold;
          hc.ticks = 0;
        }
        break;
      }
      case MobHandCast::Phase::Hold:
        SpellReadyClip(*mob, h, true, hc.clipOn[hc.hand]);
        if (++hc.ticks >= MobHandCast::kHoldTicks) {
          hc.phase = MobHandCast::Phase::Lower;
          hc.ticks = 0;
        }
        break;
      case MobHandCast::Phase::Lower:
        SpellReadyClip(*mob, h, false, hc.clipOn[hc.hand]);
        if (++hc.ticks >= MobHandCast::kLowerTicks) {
          EndHandCast(hc, mob, w.items, true);
          continue;
        }
        break;
    }
    // THE ARM: the gesture's pose through the one weapon-arm claim, applied
    // now and re-applied after the creature's own stroke step next tick
    // (Mob::SetCastArm), so the melee driver never has the last word on it.
    WeaponPose wp;
    if (hc.arm.hand >= 0 && CastArmPose(*mob, hc.arm, hc.run, lib, hc.aimDir, wp)) {
      mob->SetWeaponPose(wp);
      mob->SetCastArm(Mob::CastArm{true, HandAt(hc.arm.hand), wp});
    } else {
      mob->SetCastArm(Mob::CastArm{});
    }
    mob->SetCastBusy(true);
    // THE LIGHT IN THE FIST while the spell is in it (raise through hold);
    // let go with it (Mob::SpellLight -- streamed, so a peer sees it too).
    const bool lit = hc.phase != MobHandCast::Phase::Lower;
    mob->SetSpellLight(HandAt(hc.hand), lit ? hc.color : 0);
    mob->SetSpellLight(OtherHand(HandAt(hc.hand)), 0);
  }
}

// A SHEATHED BLADE GOES BACK IN THE HAND. The cast put it there; a cast is
// not saved, so a creature saved mid-cast loads with its blade in the sheath
// and an empty fist -- this draws it, once a second, for any AI creature with
// a free working hand and no cast in it. (The player's sheath is the player's
// business: the session's equip seam owns their hands.)
void DrawSheathed(TickAuthorityCtx& w, const ItemLibrary& items) {
  const int sheath = (int)EquipSlotId::Sheath;
  for (uint32_t i = 0; i < w.mobs.MobCount(); i++) {
    Mob* mob = w.mobs.FindMobById(w.mobs.MobIdAt(i));
    if (mob == nullptr || mob->GetController() != Mob::Controller::Ai || !mob->Alive() ||
        mob->CastBusy() || mob->Ragdolled())
      continue;
    const ItemStack& st = mob->GetKit().equip.slots[sheath];
    if (st.Empty()) continue;
    // A BOW TAKES BOTH HANDS (the NPC archers, game/mob_archery.h): the off
    // hand is the string hand, and a sidearm drawn into it would be drawn
    // into the bow_draw pose. The archer's sidearm comes out when its arrows
    // run out (mob_archery.cpp, with the bow put away first).
    bool bowInHand = false;
    for (Hand h : {Hand::Right, Hand::Left})
      if (const ItemDef* hd = items.Named(mob->HeldItem(h)); hd != nullptr && hd->IsBow())
        bowInHand = true;
    if (bowInHand) continue;
    for (Hand h : {Hand::Right, Hand::Left}) {
      if (!HandCanCast(*mob, h) || !mob->HeldItem(h).empty()) continue;
      const ItemDef* def = items.Named(st.name);
      if (def == nullptr) break;
      ItemStack taken = mob->KitTake(KitRef{KitSpace::Equip, sheath});
      if (!mob->EquipItemAsWas(def, h, &taken.damage)) {
        *mob->KitMut().Resolve(KitRef{KitSpace::Equip, sheath}) = taken;
        break;
      }
      mob->SetHeldContents(taken.contents, h);
      break;
    }
  }
}

}  // namespace

MobCastOutcome MobCastServe(TickAuthorityCtx& w, std::span<SessionTick> players,
                            const ai::CastRequest& req, uint32_t tick,
                            const std::string& spell, OpBatch& out) {
  return ServeImpl(w, players, req, tick, spell, out, ServeMode::Now, nullptr, nullptr);
}

MobCastOutcome MobCastBegin(TickAuthorityCtx& w, std::span<SessionTick> players,
                            const ai::CastRequest& req, uint32_t tick,
                            const std::string& spell, OpBatch& out) {
  MobCastWorld& m = MobCasters(w);
  Mob* mob = w.mobs.FindMobById(req.mobId);
  if (MobCaster* c0 = m.Find(req.mobId);
      c0 != nullptr && (c0->hand.phase == MobHandCast::Phase::Raise ||
                        c0->hand.phase == MobHandCast::Phase::Gesture))
    return MobCastOutcome::Busy;
  // Mid-swing: the blade is not put away under a live stroke. A held guard
  // is let go over the raise instead (the AI picked casting over guarding).
  if (mob != nullptr && mob->Stroke().Active()) {
    if (mob->Stroke().phase != NpcStroke::Phase::Guard) return MobCastOutcome::Busy;
  }
  Picked pk;
  const MobCastOutcome o =
      ServeImpl(w, players, req, tick, spell, out, ServeMode::Pick, nullptr, &pk);
  if (o != MobCastOutcome::Gesturing) return o;   // a blink, or the draw's refusal
  MobCaster* c = m.Find(req.mobId);
  if (c == nullptr || mob == nullptr || pk.gesture < 0)
    return ServeImpl(w, players, req, tick, pk.spell, out, ServeMode::Now, nullptr, nullptr);
  MobHandCast& hc = c->hand;
  // A spell still in the hand from the last cast (Hold, Lower before the
  // draw-back): cast straight from it.
  const bool inHand = (hc.phase == MobHandCast::Phase::Hold ||
                       hc.phase == MobHandCast::Phase::Lower) &&
                      HandCanCast(*mob, HandAt(hc.hand));
  if (!inHand) {
    if (hc.phase != MobHandCast::Phase::Idle) EndHandCast(hc, mob, w.items, true);
    const int hk = ChooseCastHand(*mob);
    if (hk < 0 || !StowHand(hc, *mob, w.items, HandAt(hk)))
      return ServeImpl(w, players, req, tick, pk.spell, out, ServeMode::Now, nullptr, nullptr);
    hc.hand = hk;
  }
  if (mob->Stroke().Active() && mob->Stroke().phase == NpcStroke::Phase::Guard)
    mob->Stroke().StartRelease(MobHandCast::kRaiseTicks);
  hc.spell = pk.spell;
  hc.req = req;
  hc.gesture = pk.gesture;
  hc.color = pk.color;
  hc.ticks = 0;
  if (inHand) {
    BeginCastGesture(hc.run, hc.arm, hc.gesture, hc.hand);
    hc.phase = MobHandCast::Phase::Gesture;
  } else {
    hc.phase = MobHandCast::Phase::Raise;
  }
  mob->SetCastBusy(true);
  return MobCastOutcome::Gesturing;
}

const MobHandCast* MobHandCastOf(const TickAuthorityCtx& w, uint64_t mobId) {
  if (!w.mobCast) return nullptr;
  for (const MobCaster& c : w.mobCast->casters)
    if (c.mobId == mobId) return &c.hand;
  return nullptr;
}

const KitSpell* KitSpellNamed(TickAuthorityCtx& w, const std::string& spell) {
  MobCastWorld& m = MobCasters(w);
  EnsureKit(m, false);
  return m.kit.Find(spell);
}

bool MobCanCast(TickAuthorityCtx& w, uint64_t mobId, const std::string& spell, float distance,
                uint32_t tick, int32_t* costOut) {
  if (costOut) *costOut = 0;
  const ai::CastTuning* ct = TuningOf(w.mobs, mobId);
  if (ct == nullptr || !w.mobs.IsAlive(mobId)) return false;
  MobCastWorld& m = MobCasters(w);
  EnsureKit(m, false);
  const KitSpell* ks = m.kit.Find(spell);
  if (ks == nullptr || distance < ks->rangeMin || distance > ks->rangeMax) return false;
  const MobCaster* c = m.Find(mobId);
  if (ks->blink) return c == nullptr || tick >= c->blinkReadyAt;
  if (c != nullptr && ks->aim != KitAim::Self && LiveOf(*c) >= ct->maxLive) return false;
  if (c != nullptr && ks->once)
    for (const SpellStatus& st : c->spells.Statuses())
      if (st.casterId == c->mobId && st.target == c->mobId) return false;
  std::string unknown;
  const CastList list = CompileSpell(w.glyphs, StackOf(w.glyphs, ks->words, &unknown));
  if (list.Empty() || !unknown.empty()) return false;
  if (costOut) *costOut = list.manaCost;
  CasterState pool;
  if (c != nullptr) {
    pool = c->mana;
  } else {
    pool.mana = pool.manaMax = ct->mana;
  }
  return ResolveCast(pool, 0, list.manaCost).outcome == CastOutcome::Normal;
}

void MobCastTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
                 OpBatch& out) {
  if (tick % 30 == 0) DrawSheathed(w, w.items);
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
      MobCastBegin(w, players, r, tick, std::string(), out);
    }
  }
  // ---- the casts in creatures' hands: raise, gesture, release, draw back ----------
  StepHandCasts(w, players, m, tick, out);
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
    pc.gore = &out.spawns;
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
    // A CONTAINED DEMON'S CARRIER AT ITS RING (D3, demon::AllowCastOut): asked
    // ONCE, the first tick the carrier is outside the circle. Severed -> it
    // dies there, before anything it carries can land outside (what it
    // resolved on the way out this very tick was filtered in Route). Open ->
    // it flies on, and is not asked again.
    if (const LiveDemon* ld = ContainedDemon(w, c.mobId); ld != nullptr) {
      struct RingCtx {
        TickAuthorityCtx* w;
        MobCaster* c;
        const LiveDemon* ld;
        Vec3 from;
      } rc{&w, &c, ld, BoxCentre(w.mobs, c.mobId)};
      m.stats.ringRefused += (uint64_t)c.spells.RefuseCarriers(
          [](void* ctx, const SpellProjectile& p) {
            RingCtx& r = *(RingCtx*)ctx;
            const Vec3 at{SpellFxToFloat(p.pos.x), SpellFxToFloat(p.pos.y),
                          SpellFxToFloat(p.pos.z)};
            if (!OutsideRing(*r.ld, at.x, at.z)) return false;
            if (std::find(r.c->passedRing.begin(), r.c->passedRing.end(), p.seq) !=
                r.c->passedRing.end())
              return false;
            if (!demon::AllowCastOut(*r.w, r.c->mobId, r.from, at)) return true;
            r.c->passedRing.push_back(p.seq);
            return false;
          },
          &rc);
    }
    // Forget the crossings of carriers no longer in the air.
    std::erase_if(c.passedRing, [&](uint32_t seq) {
      for (const SpellProjectile& p : c.spells.Live())
        if (p.seq == seq) return false;
      return true;
    });
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

// ---- THE AIM, SHARED (game/mob_archery.cpp's archers) -------------------------------
Vec3 MobAimCentre(const MobSystem& mobs, uint64_t id, Vec3* size) {
  return BoxCentre(mobs, id, size);
}

bool MobTargetNow(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t id,
                  Vec3& out, float* heightOut) {
  if (!TargetNow(w, players, id, out)) return false;
  if (heightOut != nullptr) {
    if (ai::IsPlayerActorId(id)) {
      *heightOut = 2.0f * Player::kHalfY;
    } else {
      Vec3 size{};
      BoxCentre(w.mobs, id, &size);
      *heightOut = size.y;
    }
  }
  return true;
}
