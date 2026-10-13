// arrows.cpp -- the bow's numbers, the kit's arrows, and the arrows in flight
// (docs/PLAN_projectiles.md package C; read game/arrows.h first).

#include "game/arrows.h"

#include <algorithm>
#include <cmath>

#include "game/anim.h"  // QuatFromTo / QuatRotate
#include "game/equipment.h"
#include "game/flightprobe.h"
#include "game/mob.h"
#include "game/worlditems.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/world.h"

namespace {

// The fixed tick (test/support.h kTickDt) and gravity in the flight's units:
// 24.8 fixed world voxels per tick^2 -- the spell VM's kGravityFxPerTick2,
// spelled the same way so the two fall alike.
constexpr int32_t kArrowTicksPerSecond = 30;
constexpr int32_t kArrowGravityFx =
    (int32_t)((int64_t)981 * kVoxelsPerMetre * kSpellFxOne /
              (100 * kArrowTicksPerSecond * kArrowTicksPerSecond));

// Integer square root (the spell VM's: a float sqrt feeding authoritative
// flight state is the kind of thing that differs in the last bit).
int64_t ISqrt(int64_t x) {
  if (x <= 0) return 0;
  int64_t r = 0, b = 1LL << 40;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) {
      x -= r + b;
      r = (r >> 1) + b;
    } else {
      r >>= 1;
    }
    b >>= 2;
  }
  return r;
}

SpellFxVec FxUnit(SpellFxVec v, int64_t scale) {
  const int64_t rr =
      ISqrt((int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z);
  if (rr <= 0) return {0, 0, 0};
  return {(int32_t)((int64_t)v.x * scale / rr),
          (int32_t)((int64_t)v.y * scale / rr),
          (int32_t)((int64_t)v.z * scale / rr)};
}

SpellFxVec FxOf(Vec3 v) {
  return {SpellFxFromFloat(v.x), SpellFxFromFloat(v.y), SpellFxFromFloat(v.z)};
}
Vec3 FloatOf(SpellFxVec v) {
  return Vec3{SpellFxToFloat(v.x), SpellFxToFloat(v.y), SpellFxToFloat(v.z)};
}

// A per-tick fixed velocity as m/s, for the penetrator (whose bodies are
// priced in float on the physics state, as melee's are).
float SpeedMpsOf(SpellFxVec vel) {
  const double l = std::sqrt((double)vel.x * vel.x + (double)vel.y * vel.y +
                             (double)vel.z * vel.z);
  return (float)(l / kSpellFxOne * kArrowTicksPerSecond * kVoxelMeters);
}

// Where the arrow's body sits for its POINT at `tip` heading along `dir`: the
// art's axis (+x) onto the direction, the art's point onto the tip.
BodyTransform ArrowXf(const ItemDef& def, Vec3 tip, Vec3 dir) {
  const Quat q = QuatNormalize(QuatFromTo(Vec3{1, 0, 0}, dir));
  BodyTransform xf{};
  xf.pos = tip - QuatRotate(q, def.arrow.tip);
  xf.quat[0] = q.x;
  xf.quat[1] = q.y;
  xf.quat[2] = q.z;
  xf.quat[3] = q.w;
  return xf;
}

}  // namespace

// ---- THE BOW, AS NUMBERS -------------------------------------------------------

float BowDrawFraction(const ItemDef& bow, int ticks) {
  if (!bow.bow.has || ticks <= 0) return 0.0f;
  const float t = std::clamp(
      (float)ticks / (float)std::max(1, bow.bow.drawTicks), 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

float BowArrowSpeed(const ItemDef& bow, int ticks) {
  const float f = BowDrawFraction(bow, ticks);
  if (f <= 0.0f || f < bow.bow.minDraw) return 0.0f;
  return f * bow.bow.drawSpeed;
}

Penetrator ArrowPenetrator(const ItemDef& a, float speedMps, uint32_t seed) {
  Penetrator p;
  p.massKg = a.arrow.massKg;
  p.speedMps = speedMps;
  p.tipRadius = a.arrow.tipRadius;
  p.hardness = a.arrow.hardness;   // 0: the head's material says
  p.material = a.arrow.headMaterial;
  p.sharpness = a.arrow.sharpness;
  p.hp = a.arrow.damage;
  p.seed = seed;
  return p;
}

Vec3 ArrowHoldover(Vec3 from, Vec3 to, float speedVox) {
  const float perTick = speedVox / (float)kArrowTicksPerSecond;
  if (!(perTick > 1e-3f)) return to;
  // Ticks of flight along the straight line, and what Tick's integrator (the
  // velocity loses g BEFORE each step) drops over them: g (1 + 2 + .. + n).
  const float n = (to - from).len() / perTick;
  const float g = (float)kArrowGravityFx / (float)kSpellFxOne;   // voxels/tick^2
  return Vec3{to.x, to.y + g * n * (n + 1.0f) * 0.5f, to.z};
}

// ---- THE BOW IN A CREATURE'S HAND ------------------------------------------------

const ItemDef* HeldBow(const Mob& m, const ItemLibrary& items, Hand* handOut) {
  for (Hand h : {Hand::Right, Hand::Left}) {
    const std::string& name = m.HeldItem(h);
    if (name.empty()) continue;
    const ItemDef* d = items.Named(name);
    if (d != nullptr && d->IsBow()) {
      if (handOut) *handOut = h;
      return d;
    }
  }
  return nullptr;
}

int MobShotsLeft(const Mob& m, const ItemLibrary& items) {
  const ItemDef* bow = HeldBow(m, items);
  return bow != nullptr ? KitAmmoCount(m.GetKit(), bow->bow.ammo) : 0;
}

// ---- AMMUNITION IN THE KIT -------------------------------------------------------

int KitAmmoCount(const Kit& kit, const std::string& ammo) {
  if (ammo.empty()) return 0;
  int n = 0;
  for (const ItemStack& s : kit.hotbar.slots)
    if (!s.Empty() && s.name == ammo) n += std::max(0, s.count);
  for (const ItemStack& s : kit.bag.slots)
    if (!s.Empty() && s.name == ammo) n += std::max(0, s.count);
  return n;
}

bool KitTakeAmmo(Kit& kit, const std::string& ammo) {
  if (ammo.empty()) return false;
  auto takeFrom = [&](ItemStack* slots, int n) {
    for (int i = 0; i < n; i++) {
      ItemStack& s = slots[i];
      if (s.Empty() || s.name != ammo || s.count <= 0) continue;
      if (--s.count <= 0) s = ItemStack{};
      return true;
    }
    return false;
  };
  return takeFrom(kit.hotbar.slots, kItemSlots) ||
         takeFrom(kit.bag.slots, Bag::kSlots);
}

// ---- G ON AN ARROW STUCK IN A CREATURE ------------------------------------------

bool IsEmbeddedArrowBody(MobSystem& mobs, uint64_t body) {
  int li = -1;
  Mob* m = body ? mobs.FindOwner(body, &li) : nullptr;
  return m != nullptr && li >= 0 && m->IsArrowSlot(li) &&
         MobSystem::OwnedHere(*m);
}

bool PullArrowToKit(MobSystem& mobs, uint64_t body, Kit& kit,
                    const ItemLibrary& items, std::string* itemOut,
                    bool* noRoom) {
  if (noRoom) *noRoom = false;
  if (!IsEmbeddedArrowBody(mobs, body)) return false;
  int li = -1;
  Mob* m = mobs.FindOwner(body, &li);
  const std::string name = m->ArrowItemOf(li);
  const int di = items.Find(name);
  if (di < 0) return false;
  // ROOM FIRST: a full pack refuses and the arrow stays where it is, the
  // ground pickup's rule (nothing a take does can lose an item).
  const ItemStack one = StackOf(items, di, 1);
  int where = kit.bag.Add(one);
  if (where < 0) where = kit.hotbar.Add(one);
  if (where < 0) {
    if (noRoom) *noRoom = true;
    return false;
  }
  m->PullArrow(li);
  if (itemOut) *itemOut = name;
  return true;
}

// ---- THE ARROWS IN FLIGHT ------------------------------------------------------

bool ArrowFlights::Launch(const ItemDef& arrow, Vec3 fromWorld, Vec3 dirWorld,
                          float speedVox, Vec3 carryVox, uint32_t seed,
                          std::vector<uint64_t> ignore) {
  if (!arrow.IsAmmo()) return false;
  const float dl = dirWorld.len();
  if (!(dl > 1e-6f) || !(speedVox > 0.0f)) return false;
  // Rule 2: a hard cap, checked before the spawn.
  if ((int)live_.size() >= kMaxLive) {
    stats.refused++;
    return false;
  }
  const Vec3 v = dirWorld * (speedVox / dl) + carryVox;   // voxels/s
  ArrowFlight f;
  f.pos = FxOf(fromWorld);
  f.vel = FxOf(v * (1.0f / (float)kArrowTicksPerSecond));
  f.item = arrow.name;
  f.seed = seed;
  f.ticksLeft = kLifeTicks;
  f.ignore = std::move(ignore);
  live_.push_back(std::move(f));
  stats.launched++;
  return true;
}

void ArrowFlights::Log(const ArrowEvent& e) {
  // Bounded (rule 2): the gate's window, not a history.
  if (events_.size() >= kMaxEvents)
    events_.erase(events_.begin(), events_.begin() + (ptrdiff_t)(kMaxEvents / 2));
  events_.push_back(e);
}

void ArrowFlights::Tick(uint32_t tick, ArrowWorld& w,
                        std::vector<ParticleSpawn>& spawns) {
  if (live_.empty()) return;
  const uint32_t wseed = w.world.WorldSeed();
  auto solidAt = [&](SpellFxVec p) {
    return FlightSolidAt(w.world, w.classOf, wseed, SpellFxFloor(p.x),
                         SpellFxFloor(p.y), SpellFxFloor(p.z));
  };
  // The chunks a flight is about to cross, asked for ahead of it (the spell
  // VM's prefetch): its own and a few along the velocity, so the probe
  // reads the cache rather than the analytic ground.
  auto prefetch = [&](SpellFxVec at, SpellFxVec vel) {
    const SpellFxVec step = FxUnit(vel, 12 * kSpellFxOne);
    for (int k = 0; k < 5; k++) {
      const IVec3 cell{SpellFxFloor(at.x), SpellFxFloor(at.y),
                       SpellFxFloor(at.z)};
      if (w.world.CellInWindow(cell) &&
          w.world.KindAt(cell, w.classOf) == CellKind::Unknown)
        w.world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, cell.z >> 4});
      at.x += step.x;
      at.y += step.y;
      at.z += step.z;
    }
  };
  auto event = [&](ArrowEvent::Kind k, const ArrowFlight& f) {
    ArrowEvent e;
    e.kind = k;
    e.tick = tick;
    e.seed = f.seed;
    return e;
  };
  // An item body for this arrow: stuck (kinematic) or dropped (falling).
  auto placeItem = [&](const ItemDef& def, Vec3 tip, Vec3 dir, Vec3 vel,
                       bool stuck) -> uint64_t {
    if (w.ground == nullptr) return 0;
    ItemInstance inst;
    inst.name = def.name;
    inst.count = 1;
    return DropItemToWorldXf(def, inst, ArrowXf(def, tip, dir), vel, w.phys,
                             w.debris, w.micro, *w.ground, nullptr, stuck);
  };

  for (size_t i = 0; i < live_.size();) {
    ArrowFlight& f = live_[i];
    const ItemDef* def = w.items.Named(f.item);
    bool done = false;
    const IVec3 cell{SpellFxFloor(f.pos.x), SpellFxFloor(f.pos.y),
                     SpellFxFloor(f.pos.z)};
    if (def == nullptr || !def->IsAmmo() || --f.ticksLeft <= 0 ||
        !w.world.CellInWindow(cell)) {
      ArrowEvent e = event(ArrowEvent::Kind::Expired, f);
      e.at = FloatOf(f.pos);
      Log(e);
      stats.expired++;
      done = true;
    } else {
      f.vel.y -= kArrowGravityFx;
      prefetch(f.pos, f.vel);
      const SpellFxVec start = f.pos;
      // ---- THE GRID: half-voxel sub-steps, as the spell VM sweeps ----------
      const int32_t kHalf = kSpellFxOne / 2;
      const int64_t maxComp =
          std::max({(int64_t)std::abs(f.vel.x), (int64_t)std::abs(f.vel.y),
                    (int64_t)std::abs(f.vel.z)});
      int32_t steps = (int32_t)((maxComp + kHalf - 1) / kHalf);
      steps = std::clamp(steps, 1, 512);
      bool wall = false;
      SpellFxVec p = f.pos, wallAt{};
      for (int32_t s = 0; s < steps; s++) {
        SpellFxVec next = p;
        next.x += f.vel.x / steps;
        next.y += f.vel.y / steps;
        next.z += f.vel.z / steps;
        if (solidAt(next)) {
          wall = true;
          wallAt = next;
          break;
        }
        p = next;
      }
      const SpellFxVec segEnd = wall ? wallAt : p;
      const Vec3 a = FloatOf(start), b = FloatOf(segEnd);
      const Vec3 seg = b - a;
      const float len = seg.len();
      Vec3 d = FloatOf(f.vel);
      d = d.len() > 1e-6f ? d * (1.0f / d.len()) : Vec3{1, 0, 0};
      if (len > 1e-4f) d = seg * (1.0f / len);

      // ---- THE BODIES ON THE SEGMENT: one ray, body to body ----------------
      if (len > 1e-4f) {
        const float speed0 = SpeedMpsOf(f.vel);
        const Penetrator pen =
            ArrowPenetrator(*def, speed0, f.seed ^ (tick * 2654435761u));
        const PierceFlight pf =
            PierceAlongRay(w.phys, w.mobs, w.debris, w.world, spawns, a, seg,
                           len, pen, f.ignore, def->name);
        for (const PierceHit& h : pf.hits) {
          ArrowEvent e = event(ArrowEvent::Kind::Hit, f);
          e.mobId = h.mobId;
          e.limb = h.limb;
          e.at = h.r.entryWorld;
          e.dir = d;
          e.r = h.r;
          Log(e);
          stats.hits++;
        }
        if (pf.stopped && !pf.hits.empty()) {
          const PierceHit& last = pf.hits.back();
          ArrowEvent e = event(ArrowEvent::Kind::Broke, f);
          e.mobId = last.mobId;
          e.limb = last.limb;
          e.dir = d;
          e.at = last.r.hit ? last.r.stopWorld : pf.endWorld;
          if (last.r.hit && last.r.outcome == PierceOutcome::Embedded) {
            if (last.mobId != 0) {
              // IT STAYS IN THE LIMB -- on a creature this machine owns. A
              // peer's creature gets it from its owner (the routed record
              // names the item), so here it is spent.
              Mob* m = w.mobs.FindCreature(last.mobId);
              int slot = -1;
              if (m != nullptr && MobSystem::OwnedHere(*m) && !last.r.severed &&
                  last.limb >= 0)
                slot = m->EmbedArrow(*def, last.limb, last.r.stopWorld, d);
              if (slot >= 0) {
                e.kind = ArrowEvent::Kind::Embedded;
                e.slot = slot;
                stats.embedded++;
              } else {
                stats.broke++;
              }
            } else {
              // LOOSE MATTER: strapped to the body it stopped in (a carve may
              // have rebuilt it under a new handle -- Physics::Successor).
              uint64_t host = last.body;
              BodyTransform hx{};
              for (int hop = 0; hop < 16 && host && !w.phys.GetTransform(host, hx);
                   hop++)
                host = w.phys.Successor(host);
              uint64_t ab = 0;
              if (host != 0 && !w.debris.IsGhost(host)) {
                ab = placeItem(*def, last.r.stopWorld, d, Vec3{}, true);
                if (ab != 0 && !w.debris.StrapBody(ab, host)) {
                  // No strap: it stands where it stopped, a stuck item.
                }
              }
              if (ab != 0) {
                e.kind = ArrowEvent::Kind::Strapped;
                e.body = ab;
                stats.strapped++;
              } else {
                stats.broke++;
              }
            }
          } else {
            // GLANCED, or a body nobody carves (a door, a bomb): it falls
            // from where it met it, kicked back a little.
            const Vec3 at = last.r.hit ? last.r.entryWorld : pf.endWorld;
            const Vec3 back =
                d * (-0.15f * MetresPerSecToCells(speed0)) + Vec3{0, 2.0f, 0};
            const uint64_t ab = placeItem(*def, at - d * 0.5f, d, back, false);
            e.kind = ArrowEvent::Kind::Dropped;
            e.body = ab;
            e.at = at;
            stats.dropped++;
          }
          Log(e);
          done = true;
        } else if (!pf.hits.empty()) {
          // THROUGH: on at what is left (PierceFlight::speedMps).
          const float r = speed0 > 1e-4f ? pf.speedMps / speed0 : 0.0f;
          f.vel.x = (int32_t)((float)f.vel.x * r);
          f.vel.y = (int32_t)((float)f.vel.y * r);
          f.vel.z = (int32_t)((float)f.vel.z * r);
        }
      }

      // ---- THE GROUND: it sticks, point first ------------------------------
      if (!done && wall) {
        // From the last free sample, a hand's breadth or two into whatever
        // stopped it, by how fast it was going (a spent arrow barely bites).
        const float mps = SpeedMpsOf(f.vel);
        const float depth =
            std::clamp(0.3f + 1.7f * mps / 65.0f, 0.3f, 2.5f);   // voxels
        const Vec3 tip = FloatOf(p) + d * depth;
        const uint64_t ab = placeItem(*def, tip, d, Vec3{}, true);
        ArrowEvent e = event(ab ? ArrowEvent::Kind::Stuck : ArrowEvent::Kind::Broke, f);
        e.body = ab;
        e.at = tip;
        e.dir = d;
        Log(e);
        if (ab != 0) {
          stats.stuck++;
          // BOUNDED: the oldest stuck arrow goes when one more would pass the
          // cap -- by global id, so a handle reused since it was picked up
          // is never mistaken for it.
          stuck_.push_back(w.debris.GlobalIdOf(ab));
          while ((int)stuck_.size() > kMaxStuck) {
            const uint64_t gid = stuck_.front();
            stuck_.erase(stuck_.begin());
            const uint64_t h = gid ? w.debris.HandleOfGlobalId(gid) : 0;
            if (h != 0 && w.ground != nullptr && w.ground->Find(h) != nullptr &&
                w.debris.DestroyBody(h))
              stats.cleared++;
          }
        } else {
          stats.broke++;
        }
        done = true;
      }
      if (!done) f.pos = segEnd;
    }
    if (done) {
      if (i + 1 != live_.size()) live_[i] = std::move(live_.back());
      live_.pop_back();
    } else {
      i++;
    }
  }
}
