// arrows.cpp -- the bow's numbers, the kit's arrows, and the arrows in flight
// (docs/PLAN_projectiles.md package C; read game/arrows.h first).

#include "game/arrows.h"

#include <algorithm>
#include <cmath>

#include "game/anim.h"  // QuatFromTo / QuatRotate
#include "game/equipment.h"
#include "game/flightprobe.h"
#include "game/itemcoat.h"  // the arrow's own lattice: what wears its poison
#include "game/quiver.h"
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
  const int pulled = ticks - bow.bow.windupTicks;
  if (!bow.bow.has || pulled <= 0) return 0.0f;
  const float t = std::clamp(
      (float)pulled / (float)std::max(1, bow.bow.drawTicks), 0.0f, 1.0f);
  return 1.0f - (1.0f - t) * (1.0f - t);
}

int BowFullDrawTicks(const ItemDef& bow) {
  return bow.bow.windupTicks + std::max(1, bow.bow.drawTicks);
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

float ArrowHeadLength(const ItemDef& a) {
  if (!a.IsAmmo() || a.voxels.empty()) return 0.0f;
  int maxX = -(1 << 20);
  for (const PrefabVoxel& v : a.voxels) maxX = std::max(maxX, (int)v.x);
  // Back from the point while the slice still holds the head's matter.
  auto sliceIsHead = [&](int x) {
    for (const PrefabVoxel& v : a.voxels)
      if ((int)v.x == x && (v.material & 0xFFFu) == a.arrow.headMaterial)
        return true;
    return false;
  };
  int x = maxX;
  while (sliceIsHead(x - 1)) x--;
  const float inv = 1.0f / (float)(a.scale ? a.scale : 1u);
  return (float)(maxX + 1 - x) * inv;
}

bool ArrowHeadIn(const ItemDef& a, const PierceResult& r) {
  return r.hit && !r.missed && r.outcome == PierceOutcome::Embedded &&
         r.depth >= ArrowHeadLength(a);
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
  // The Back slot takes nothing but a quiver (equipment.h), so its held list
  // IS the worn quiver's -- no library needed to ask how many shots are left.
  int n = 0;
  for (const ItemInstance& h : kit.equip.At((int)EquipSlotId::Back).held)
    if (!h.Empty() && h.name == ammo) n += std::max(0, h.count);
  return n;
}

bool KitTakeAmmo(Kit& kit, const std::string& ammo, uint32_t key,
                 ItemInstance* out) {
  if (ammo.empty()) return false;
  ItemInstance& q = kit.equip.slots[(int)EquipSlotId::Back];
  const int e = QuiverPick(q, key, ammo);
  if (e < 0) return false;
  ItemInstance one = QuiverTakeOne(q, e);
  if (one.Empty()) return false;
  if (out) *out = std::move(one);
  return true;
}

ItemInstance KitPeekAmmo(const Kit& kit, const std::string& ammo, uint32_t key) {
  if (ammo.empty()) return ItemInstance{};
  const ItemInstance& q = kit.equip.At((int)EquipSlotId::Back);
  const int e = QuiverPick(q, key, ammo);
  if (e < 0) return ItemInstance{};
  ItemInstance one = q.held[(size_t)e];
  one.count = 1;
  return one;
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
  return PullArrowSlotToKit(*m, li, kit, items, itemOut, noRoom);
}

bool PullArrowSlotToKit(Mob& m, int li, Kit& kit, const ItemLibrary& items,
                        std::string* itemOut, bool* noRoom) {
  if (noRoom) *noRoom = false;
  if (!m.IsArrowSlot(li)) return false;
  const std::string name = m.ArrowItemOf(li);
  const int di = items.Find(name);
  if (di < 0) return false;
  // THE ARROW AS IT COMES OUT: wearing whatever the wound and the poison left
  // on it (Mob::PullArrowLattice), so it goes into the quiver as itself.
  ItemStack one = StackOf(items, di, 1);
  m.PullArrowLattice(li, one.damage);
  // A slot that will not come out (mid-sever hold) stays where it is.
  // ROOM FIRST: a full kit refuses and the arrow stays in the body, the
  // ground pickup's rule (nothing a take does can lose an item). The quiver
  // on your back, then the pack, then the hotbar (KitReceiveArrow).
  Kit trial = kit;
  if (!KitReceiveArrow(trial, items, one)) {
    if (noRoom) *noRoom = true;
    return false;
  }
  if (!m.PullArrow(li)) return false;
  kit = std::move(trial);
  if (itemOut) *itemOut = name;
  return true;
}

// ---- THE ARROWS IN FLIGHT ------------------------------------------------------

bool ArrowFlights::Launch(const ItemDef& arrow, Vec3 fromWorld, Vec3 dirWorld,
                          float speedVox, Vec3 carryVox, uint32_t seed,
                          std::vector<uint64_t> ignore, uint64_t shooterActor,
                          const ItemInstance* inst) {
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
  f.prev = f.pos;
  f.vel = FxOf(v * (1.0f / (float)kArrowTicksPerSecond));
  f.item = arrow.name;
  if (inst != nullptr && !inst->Empty() && inst->name == arrow.name) {
    f.arrow = *inst;
  } else {
    f.arrow.name = arrow.name;
  }
  f.arrow.count = 1;
  f.arrow.held.clear();
  f.seed = seed;
  f.ticksLeft = kLifeTicks;
  f.ignore = std::move(ignore);
  f.shooter = shooterActor;
  f.from = fromWorld;
  live_.push_back(std::move(f));
  stats.launched++;
  return true;
}

void ArrowFlights::AppendRenderBodies(const ItemLibrary& items, float tickAlpha,
                                      std::vector<RenderOnlyBody>& out) const {
  const float a = std::clamp(tickAlpha, 0.0f, 1.0f);
  for (const ArrowFlight& f : live_) {
    const ItemDef* def = items.Named(f.item);
    if (def == nullptr || def->microModel < 0) continue;
    const Vec3 p0 = FloatOf(f.prev), p1 = FloatOf(f.pos);
    // Along the segment it is flying (gravity tips it over the arc), else
    // along its velocity (the tick it was loosed, before it has moved).
    Vec3 dir = p1 - p0;
    if (dir.len() < 1e-4f) dir = FloatOf(f.vel);
    const float dl = dir.len();
    dir = dl > 1e-6f ? dir * (1.0f / dl) : Vec3{1, 0, 0};
    const BodyTransform xf = ArrowXf(*def, p0 + (p1 - p0) * a, dir);
    RenderOnlyBody b;
    b.pos = xf.pos;
    for (int k = 0; k < 4; k++) b.quat[k] = xf.quat[k];
    b.model = (uint32_t)def->microModel;
    out.push_back(b);
  }
}

// ---- THE DRAWN BOW AS SEEN ----------------------------------------------------

namespace {

// A one-cell-thick straight length of string from `a` to `b`, out of the
// bow's rods (its art's own string cell, BowStringArt::rods).
void PushStringRod(const ItemDef::BowStringArt& bs, Vec3 a, Vec3 b,
                   std::vector<RenderOnlyBody>& out) {
  const Vec3 ab = b - a;
  const float len = ab.len();
  if (!(len > 1e-4f) || bs.rods.empty()) return;
  const int n = std::clamp((int)std::lround(len / bs.cell), bs.rodMin,
                           bs.rodMin + (int)bs.rods.size() - 1);
  const Vec3 dir = ab * (1.0f / len);
  const Quat q = QuatNormalize(QuatFromTo(Vec3{1, 0, 0}, dir));
  RenderOnlyBody r;
  // The rod's cells sit at y, z in [0, 1) cell: its centre line half a cell
  // in from the corner the body transform is at.
  r.pos = a - QuatRotate(q, Vec3{0.0f, 0.5f * bs.cell, 0.5f * bs.cell});
  r.quat[0] = q.x;
  r.quat[1] = q.y;
  r.quat[2] = q.z;
  r.quat[3] = q.w;
  r.model = (uint32_t)bs.rods[(size_t)(n - bs.rodMin)];
  out.push_back(r);
}

}  // namespace

void PresentDrawnBow(Mob& m, const ItemLibrary& items, std::vector<RenderOnlyBody>& out,
                     const Vec3* nearEye, float nearClip) {
  // The half string from its tip `a` toward the fingers `b`, cut where it
  // first comes within nearClip of the eye (nothing at all if the tip is).
  auto clipNear = [&](Vec3 a, Vec3& b) {
    if (nearEye == nullptr || !(nearClip > 0.0f)) return true;
    const Vec3 ab = b - a, ae = a - *nearEye;
    const float r2 = nearClip * nearClip;
    if (ae.dot(ae) <= r2) return false;
    const float A = ab.dot(ab), B = 2.0f * ae.dot(ab), C = ae.dot(ae) - r2;
    const float disc = B * B - 4.0f * A * C;
    if (!(A > 1e-8f) || disc < 0.0f) return true;
    const float t = (-B - std::sqrt(disc)) / (2.0f * A);
    if (t >= 0.0f && t < 1.0f) b = a + ab * t;
    return true;
  };
  for (Hand h : {Hand::Right, Hand::Left}) {
    m.SetHeldRenderModel(h, -1);
    const std::string& held = m.HeldItem(h);
    if (held.empty()) continue;
    const ItemDef* bow = items.Named(held);
    if (bow == nullptr || !bow->IsBow()) continue;
    // DRAWN = the draw clip is on and not being let go, and has come most of
    // the way in: under its blend-in the string hand is still coming up from
    // the hip, and a string stretched down to it reads as a fault.
    float w = 0.0f;
    for (const char* c : {"bow_draw", "bow_draw_crouch"}) {
      const std::string clip = m.HandClip(c, h);
      if (!clip.empty() && m.ClipPlaying(clip)) w = std::max(w, m.ClipWeight(clip));
    }
    if (w < 0.8f) continue;
    Vec3 nock{}, grip{};
    BodyTransform bx{};
    if (!m.HandSocketWorld(OtherHand(h), nock) || !m.HandSocketWorld(h, grip) ||
        !m.HeldBodyXf(h, bx))
      continue;
    const Quat bq{bx.quat[0], bx.quat[1], bx.quat[2], bx.quat[3]};
    const ItemDef::BowStringArt& bs = bow->bowString;
    const float cell = 1.0f / (float)std::max(1u, bow->scale);
    // THE STRING: the bow's own art with its string taken out, and the
    // string from each tip to the fingers. A carved bow keeps its own brick
    // (the damage is only in that one) and its straight string.
    if (bs.has && !m.HeldCarved(h)) {
      m.SetHeldRenderModel(h, bs.unstrungModel);
      for (const Vec3& tip : {bs.from, bs.to}) {
        const Vec3 a = bx.pos + QuatRotate(bq, tip);
        Vec3 b = nock;
        if (clipNear(a, b)) PushStringRod(bs, a, b, out);
      }
    }
    // THE ARROW ON THE STRING: nock at the fingers, laid along the line to
    // the ARROW REST -- beside the grip, two cells out of the stave on the
    // side the archer's head is (the bow's flat normal, signed toward it),
    // where the shaft lies against the bow and where the flight leaves from.
    const ItemDef* ammo = items.Named(bow->bow.ammo);
    if (ammo == nullptr || !ammo->IsAmmo() || ammo->microModel < 0) continue;
    Vec3 rest = grip;
    if (bs.has) {
      // The flat normal: across the plane the string and the stave's bend lie
      // in -- the string's axis crossed with the line from the string's
      // middle to the grip.
      const Vec3 axis = QuatRotate(bq, bs.to - bs.from);
      const Vec3 bend = grip - (bx.pos + QuatRotate(bq, (bs.from + bs.to) * 0.5f));
      Vec3 n = axis.cross(bend);
      const float nl = n.len();
      Vec3 head{};
      if (nl > 1e-5f && m.PartCornerWorld("head", head)) {
        n = n * (1.0f / nl);
        if ((head - grip).dot(n) < 0.0f) n = n * -1.0f;
        rest = grip + n * (2.0f * cell);
      }
    }
    Vec3 dir = rest - nock;
    const float dl = dir.len();
    if (!(dl > 1e-4f)) continue;
    dir = dir * (1.0f / dl);
    const BodyTransform axf = ArrowXf(*ammo, nock + dir * ammo->arrow.length, dir);
    RenderOnlyBody a;
    a.pos = axf.pos;
    for (int k = 0; k < 4; k++) a.quat[k] = axf.quat[k];
    a.model = (uint32_t)ammo->microModel;
    out.push_back(a);
  }
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
  // ...as ITSELF: the flight's own instance, its coat with it (a clean one
  // settles back to a plain arrow first, so it stacks again when picked up).
  auto placeItem = [&](const ItemDef& def, ArrowFlight& fl, Vec3 tip, Vec3 dir,
                       Vec3 vel, bool stuck) -> uint64_t {
    if (w.ground == nullptr) return 0;
    ItemInstance inst = fl.arrow;
    inst.name = def.name;
    inst.count = 1;
    ItemLatticeSettle(inst, def);
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
      // THE TWANG (stealth): a bow loosed is heard -- quietly -- at the
      // archer, once, on the flight's first tick.
      if (!f.twanged) {
        f.twanged = true;
        stealth::Noise n;
        n.pos = n.point = f.from;
        n.radius = w.mobs.StealthParams().twangNoise;
        n.source = f.shooter;
        n.kind = stealth::NoiseKind::Loose;
        w.mobs.PostNoise(n);
      }
      f.vel.y -= kArrowGravityFx;
      prefetch(f.pos, f.vel);
      const SpellFxVec start = f.pos;
      f.prev = start;   // render-only: the lerp's far end
      // ---- THE GRID: half-voxel sub-steps, as the spell VM sweeps ----------
      const int32_t kHalf = kSpellFxOne / 2;
      const int64_t maxComp =
          std::max({(int64_t)std::abs(f.vel.x), (int64_t)std::abs(f.vel.y),
                    (int64_t)std::abs(f.vel.z)});
      int32_t steps = (int32_t)((maxComp + kHalf - 1) / kHalf);
      steps = std::clamp(steps, 1, 512);
      bool wall = false, left = false;
      SpellFxVec p = f.pos, wallAt{};
      for (int32_t s = 0; s < steps; s++) {
        SpellFxVec next = p;
        next.x += f.vel.x / steps;
        next.y += f.vel.y / steps;
        next.z += f.vel.z / steps;
        // LEAVING THE WINDOW IS NOT A WALL. FlightSolidAt answers "solid" out
        // of window (a spell bolt detonates on the face, by design), but an
        // arrow that reached the face would stick there in mid-air as a
        // kinematic item. It flies on out of the world instead: the segment
        // ends at the last in-window sample and the flight expires below.
        if (!w.world.CellInWindow({SpellFxFloor(next.x), SpellFxFloor(next.y),
                                   SpellFxFloor(next.z)})) {
          left = true;
          break;
        }
        if (solidAt(next)) {
          wall = true;
          wallAt = next;
          break;
        }
        p = next;
      }
      const SpellFxVec segEnd = wall ? wallAt : p;
      // AN ARROW STRIKING MATTER IS HEARD WHERE IT STRUCK (stealth), and it
      // betrays nobody: a creature that hears it goes to look at the spot,
      // not at the archer -- which is what makes a shot into the wall behind
      // a guard a distraction.
      if (wall) {
        stealth::Noise n;
        n.pos = n.point = FloatOf(wallAt);
        n.radius = w.mobs.StealthParams().impactNoise;
        n.source = 0;
        n.kind = stealth::NoiseKind::Impact;
        w.mobs.PostNoise(n);
      }
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
        // WHAT THE POINT WEARS (PierceCoat): the arrow's own lattice,
        // materialised on its first flight tick (a plain arrow's is the
        // authored one, clean) -- the poison it carries goes into a wound it
        // opens, and the wound's blood onto it.
        PierceCoat pc;
        pc.lattice = &ItemLatticeMut(f.arrow, *def);
        pc.scale = (float)(def->scale ? def->scale : 1u);
        pc.tipCell = def->arrow.tip * pc.scale;
        pc.headVox = ArrowHeadLength(*def);
        pc.item = &f.arrow;
        if (pc.lattice->empty()) pc.lattice = nullptr;
        const PierceFlight pf =
            PierceAlongRay(w.phys, w.mobs, w.debris, w.world, spawns, a, seg,
                           len, pen, f.ignore, def->name, f.shooter, f.from, &pc);
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
          // IN ONLY WITH ITS HEAD IN (ArrowHeadIn): a stop shallower than the
          // head leaves it nothing to hold by, and it falls out below.
          if (last.r.hit && last.r.outcome == PierceOutcome::Embedded &&
              ArrowHeadIn(*def, last.r)) {
            if (last.mobId != 0) {
              // IT STAYS IN THE LIMB -- on a creature this machine owns. A
              // peer's creature gets it from its owner (the routed record
              // names the item), so here it is spent.
              Mob* m = w.mobs.FindCreature(last.mobId);
              int slot = -1;
              if (m != nullptr && MobSystem::OwnedHere(*m) && !last.r.severed &&
                  last.limb >= 0)
                slot = m->EmbedArrow(*def, last.limb, last.r.stopWorld, d,
                                     &f.arrow);
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
                ab = placeItem(*def, f, last.r.stopWorld, d, Vec3{}, true);
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
            // GLANCED, STOPPED SHORT OF ITS OWN HEAD, or a body nobody carves
            // (a door, a bomb): it falls from where it met it, kicked back a
            // little. A shallow stop has done its harm already (the channel
            // and the flat damage were charged by the hit); it just does not
            // stay.
            const Vec3 at = last.r.hit ? last.r.entryWorld : pf.endWorld;
            const Vec3 back =
                d * (-0.15f * MetresPerSecToCells(speed0)) + Vec3{0, 2.0f, 0};
            const uint64_t ab = placeItem(*def, f, at - d * 0.5f, d, back, false);
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
        // Never shallower than the head (ArrowHeadIn's rule, on the ground):
        // an arrow standing in the earth shows its shaft, not its point.
        const float depth = std::max(
            ArrowHeadLength(*def),
            std::clamp(0.3f + 1.7f * mps / 65.0f, 0.3f, 2.5f));   // voxels
        const Vec3 tip = FloatOf(p) + d * depth;
        const uint64_t ab = placeItem(*def, f, tip, d, Vec3{}, true);
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
      // ---- OUT OF THE WINDOW: gone, as the tick-start check would say ------
      if (!done && left) {
        ArrowEvent e = event(ArrowEvent::Kind::Expired, f);
        e.at = FloatOf(p);
        Log(e);
        stats.expired++;
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
