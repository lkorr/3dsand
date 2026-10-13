// mob_shield.cpp -- a shield in hand (ItemKind::Shield, 2026-10-13): the
// raise and what holding it up costs, the block test against the shield's own
// lattice, and what a block does to the holder. Read game/item.h ShieldSpec
// and the SHIELD notes in game/mob.h first; DESIGN.md "Shields" is the why.
//
// What is NOT here, and why: the CHIP a block carves out of the shield and the
// trauma it carries through to the arm are built from the blow's strike parts
// (melee.cpp BuildStrikeParts, file-local by design: one place builds what a
// blow is made of), so they are resolved beside the sweep, in melee.cpp's
// ResolveShieldBlock, which then calls ApplyShieldBlock below for the rest.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/mob.h"
#include "sim/scale.h"
#include "sim/tuning.h"

// ---- WHICH HAND, WHICH SPEC ---------------------------------------------------

bool Mob::ShieldHand(Hand& out) const {
  // The left first: where a shield is carried, and the one answer a creature
  // holding two (nothing to swing, but legal) needs to give consistently.
  for (Hand h : {Hand::Left, Hand::Right}) {
    const HeldHand& hh = held_[HandIndex(h)];
    if (hh.shield && hh.slot >= 0 && hh.slot < (int)limbs_.size() &&
        limbs_[(size_t)hh.slot].body != 0 && LimbAlive(hh.slot)) {
      out = h;
      return true;
    }
  }
  return false;
}

bool Mob::IsShieldSlot(int slot) const {
  Hand h;
  return IsHeldSlot(slot, &h) && held_[HandIndex(h)].shield;
}

const ItemDef::ShieldSpec* Mob::HeldShieldSpec() const {
  Hand h;
  return ShieldHand(h) ? &held_[HandIndex(h)].shieldSpec : nullptr;
}

// ---- WHAT HOLDING IT UP COSTS ---------------------------------------------------

bool Mob::ShieldBlocksStrike() const {
  if (shieldRaise_ <= 0.0f || !HoldsShield()) return false;
  // STRICTLY above: 1 means "never barred" (the raise cannot exceed it), 0
  // "barred while it is up at all". The default leaves the bottom third of
  // the raise to swing through, so a swing can begin while the shield is on
  // its way down -- the lowering is part of the telegraph, not a dead stop.
  return shieldRaise_ >
         std::clamp(CurrentTuning().melee.shieldStrikeBelow, 0.0f, 1.0f);
}

float Mob::ShieldMoveScale() const {
  const ItemDef::ShieldSpec* s = HeldShieldSpec();
  if (s == nullptr || shieldRaise_ <= 0.0f) return 1.0f;
  return 1.0f + (std::clamp(s->moveScale, 0.0f, 1.0f) - 1.0f) * shieldRaise_;
}

float Mob::ShieldTurnRate(float freeRate) const {
  const ItemDef::ShieldSpec* s = HeldShieldSpec();
  if (s == nullptr || shieldRaise_ <= 0.0f) return freeRate;
  // THE SPEC'S RATE AT FULL RAISE, relaxing as 1/raise below it: half up
  // turns at twice the cap. A reciprocal rather than a lerp from freeRate,
  // because the player's free rate is effectively unbounded (first person
  // eases onto the camera) and a lerp from infinity caps nothing.
  return std::min(freeRate,
                  std::max(s->turnRate, 0.0f) / std::max(shieldRaise_, 0.05f));
}

void Mob::StaggerShield(int ticks) {
  shieldStagger_ = std::max(shieldStagger_, std::max(0, ticks));
}

void Mob::StepShield() {
  if (shieldStagger_ > 0) shieldStagger_--;
  Hand h;
  if (!ShieldHand(h)) {
    // Nothing to hold up. The pose block retires the clip on its own; the
    // want stays the owner's (it is re-asked every tick anyway).
    shieldRaise_ = 0.0f;
    return;
  }
  const ItemDef::ShieldSpec& s = held_[HandIndex(h)].shieldSpec;
  // WHAT HOLDS A SHIELD UP: an owner asking, a guard not beaten open, a live
  // body on its feet, and an arm that can grip. A ragdoll's arm is the
  // solver's, and a hurt arm that cannot hold a sword cannot hold a shield.
  const bool up = shieldWant_ && shieldStagger_ == 0 && alive_ &&
                  ragdoll_ == RagdollPhase::None && HandUsable(h);
  if (up) {
    shieldRaise_ =
        std::min(1.0f, shieldRaise_ + 1.0f / (float)std::max(1, s.raiseTicks));
  } else {
    // A BEATEN GUARD IS KNOCKED DOWN, not lowered: twice the lowering rate.
    const float rate =
        (shieldStagger_ > 0 ? 2.0f : 1.0f) / (float)std::max(1, s.lowerTicks);
    shieldRaise_ = std::max(0.0f, shieldRaise_ - rate);
  }
}

// ---- THE BREAK -------------------------------------------------------------------

bool Mob::BreakHeld(Hand h) {
  HeldHand& hh = held_[HandIndex(h)];
  const int slot = hh.slot;
  if (slot < 0 || slot >= (int)limbs_.size() || hh.item.empty()) return false;
  if (std::getenv("SANDVOX_SHIELD_TRACE"))
    std::printf("shield trace: mob %llu BreakHeld(slot %d) hp %.1f vox %zu\n",
                (unsigned long long)id_, slot, (double)limbs_[(size_t)slot].hp,
                limbs_[(size_t)slot].voxels.size());
  LostGear lg;
  lg.item = hh.item;
  lg.held = true;
  lg.hand = h;
  lg.broken = true;
  // THE KIT GIVES IT UP NOW (ShedGearBeforeDetach's reason): the player's rig
  // is dressed from the kit every tick, and a hand slot still naming the
  // shield would put a fresh one in the fist the tick after it broke.
  if (!hh.borrowed) {
    ItemStack& ks = kit_.equip.InHand(h);
    if (!ks.Empty() && ks.name == hh.item && --ks.count <= 0) ks = ItemStack{};
  }
  if (lostGear_.size() >= kMaxLostGear) lostGear_.erase(lostGear_.begin());
  lostGear_.push_back(std::move(lg));
  // WHAT IS LEFT FALLS AS JUNK. Forgetting the hand BEFORE the detach is what
  // makes ShedGearBeforeDetach name nothing (the slot is no longer a held
  // one), so DetachLimb adopts the remains as plain debris: they fall, burn
  // and rot like any other wood and iron, and G finds no item in them.
  const bool borrowed = hh.borrowed;
  hh.Clear();
  DetachLimb(slot, /*adopt=*/!borrowed);
  if (h == strikeHand_ && held_[HandIndex(OtherHand(h))].slot >= 0)
    strikeHand_ = OtherHand(h);
  shieldRaise_ = 0.0f;
  MarkInstancesDirty();
  return true;
}

int Mob::ShieldArmPart(Hand h) const {
  const int hand = HandPart(h);
  if (hand < 0 || hand >= (int)skel_.parts.size()) return -1;
  // The forearm: the hand's parent, as long as it is anatomy and still on.
  const int fore = skel_.parts[(size_t)hand].parent;
  if (fore >= 0 && fore < baseLimbs_ && LimbAlive(fore) &&
      limbs_[(size_t)fore].body != 0)
    return fore;
  return LimbAlive(hand) && limbs_[(size_t)hand].body != 0 ? hand : -1;
}

// ---- THE BLOCK TEST -------------------------------------------------------------

namespace {

// Liang-Barsky: the part of segment p0->p1 inside the box [lo, hi], as
// parameters t0 <= t1 in [0, 1]. False when it misses. A degenerate segment
// (a blade point that did not move) is a point test.
bool ClipSegToBox(const Vec3& p0, const Vec3& p1, const Vec3& lo, const Vec3& hi,
                  float& t0, float& t1) {
  t0 = 0.0f;
  t1 = 1.0f;
  const float d[3] = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
  const float p[3] = {p0.x, p0.y, p0.z};
  const float l[3] = {lo.x, lo.y, lo.z};
  const float h[3] = {hi.x, hi.y, hi.z};
  for (int a = 0; a < 3; a++) {
    if (std::fabs(d[a]) < 1e-7f) {
      if (p[a] < l[a] || p[a] > h[a]) return false;
      continue;
    }
    float ta = (l[a] - p[a]) / d[a];
    float tb = (h[a] - p[a]) / d[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

}  // namespace

bool MobSystem::FindShieldBlock(const Mob& wielder, const Vec3& aPrev,
                                const Vec3& bPrev, const Vec3& aNow,
                                const Vec3& bNow, float halfWidth,
                                ShieldHit& out) {
  out = ShieldHit{};
  // HOW NEAR MATTER the edge must pass: its own half-width plus a little
  // slack (the probes in MeleeSweepDamage overhang by a cell for the same
  // discretisation reason; a shield is solid, so this is much less).
  const float reach =
      std::max(halfWidth, 0.0f) +
      MetresToCells(std::max(CurrentTuning().melee.shieldContactM, 0.0f));
  // A fixed row of points down the edge, base to tip. Nine: a sword's edge is
  // ~8 voxels, so one a voxel, and a shield is ~5 voxels across -- an edge
  // cannot pass a shield between two of them.
  constexpr int kAlong = 8;
  // How finely a crossing of the box is walked for matter, world voxels: a
  // quarter of a voxel is two lattice cells at the items' 8-per-voxel, under
  // the thinnest board (3 cells), so a path cannot step over the face.
  constexpr float kWalk = 0.25f;
  constexpr int kWalkMax = 24;
  float bestT = 2.0f;
  auto test = [&](Mob& m) {
    if (&m == &wielder || !m.alive_) return;
    for (int k = 0; k < kHands; k++) {
      const Mob::HeldHand& hh = m.held_[k];
      if (!hh.shield || hh.slot < 0 || hh.slot >= (int)m.limbs_.size()) continue;
      const MobLimb& L = m.limbs_[(size_t)hh.slot];
      if (L.body == 0 || !m.LimbAlive(hh.slot)) continue;
      shieldStats_.tests++;
      // THE SHIELD'S FRAME: the slot's live transform sits at the model's
      // MIN CORNER (Mob::WeaponEdge's convention), so the box is [0, size].
      const Quat q{L.xf.quat[0], L.xf.quat[1], L.xf.quat[2], L.xf.quat[3]};
      const float physScale = (float)std::max(1u, m.PhysScaleOf(L));
      const Vec3 ext{(float)L.size.x / physScale, (float)L.size.y / physScale,
                     (float)L.size.z / physScale};
      auto toLocal = [&](const Vec3& p) { return QuatRotateInv(q, p - L.xf.pos); };
      const Vec3 la0 = toLocal(aPrev), lb0 = toLocal(bPrev);
      const Vec3 la1 = toLocal(aNow), lb1 = toLocal(bNow);
      const Vec3 lo{-reach, -reach, -reach};
      const Vec3 hi{ext.x + reach, ext.y + reach, ext.z + reach};
      // THE AUTHORITATIVE LATTICE (ShellMaterialAt's choice): the skin when
      // there is one, else the collider voxels, each on its own scale.
      const bool fine = L.HasFineSkin();
      const float latScale =
          (float)std::max(1u, fine ? m.SkinScaleOf(L) : m.PhysScaleOf(L));
      // A voxel counts when its CENTRE is within `reach` of the path point,
      // grown by half a cell so the test is to the voxel's surface.
      const float rLat = reach * latScale + 0.5f;
      const float r2 = rLat * rLat;
      auto matterNear = [&](const Vec3& pLocal) -> bool {
        const Vec3 p = pLocal * latScale;
        auto near = [&](float x, float y, float z) {
          const float dx = x + 0.5f - p.x, dy = y + 0.5f - p.y,
                      dz = z + 0.5f - p.z;
          return dx * dx + dy * dy + dz * dz <= r2;
        };
        if (fine) {
          for (const PrefabVoxel& v : L.skinVoxels)
            if ((v.material & 0xFFFu) != 0u && near(v.x, v.y, v.z)) return true;
        } else {
          for (const DebrisVoxel& v : L.voxels)
            if ((v.payload & 0xFFFu) != 0u && near(v.x, v.y, v.z)) return true;
        }
        return false;
      };
      bool boxed = false;
      float hitT = 2.0f;
      Vec3 hitLocal{};
      for (int i = 0; i <= kAlong; i++) {
        const float u = (float)i / (float)kAlong;
        const Vec3 p0 = la0 + (lb0 - la0) * u;
        const Vec3 p1 = la1 + (lb1 - la1) * u;
        float t0, t1;
        if (!ClipSegToBox(p0, p1, lo, hi, t0, t1)) continue;
        if (t0 >= hitT) continue;   // an earlier point already met it sooner
        boxed = true;
        const float span = (p1 - p0).len() * (t1 - t0);
        const int n = std::clamp((int)std::ceil(span / kWalk), 1, kWalkMax);
        for (int j = 0; j <= n; j++) {
          const float t = t0 + (t1 - t0) * (float)j / (float)n;
          if (t >= hitT) break;
          const Vec3 p = p0 + (p1 - p0) * t;
          if (matterNear(p)) {
            hitT = t;
            hitLocal = p;
            break;
          }
        }
      }
      if (boxed) shieldStats_.boxHits++;
      if (hitT > 1.0f) {
        if (boxed) shieldStats_.holes++;
        continue;
      }
      if (hitT < bestT) {
        bestT = hitT;
        out.holder = &m;
        out.hand = HandAt(k);
        out.slot = hh.slot;
        out.body = L.body;
        out.at = L.xf.pos + QuatRotate(q, hitLocal);
        out.t = hitT;
      }
    }
  };
  ForEachCreature(test);
  if (out.holder != nullptr) shieldStats_.blocks++;
  return out.holder != nullptr;
}

// ---- WHAT A BLOCK DOES TO THE HOLDER --------------------------------------------

MobSystem::ShieldBlockOut MobSystem::ApplyShieldBlock(
    Mob& holder, int slot, const Vec3& at, const Vec3& sweepDir,
    const StrikeProfile& strike, float power, float tipSpeed, float heft) {
  (void)tipSpeed;
  ShieldBlockOut o;
  Hand h;
  if (!holder.IsHeldSlot(slot, &h) || slot >= (int)holder.limbs_.size()) return o;
  Mob::HeldHand& hh = holder.held_[HandIndex(h)];
  if (!hh.shield) return o;
  const ItemDef::ShieldSpec sp = hh.shieldSpec;   // a copy: the break clears hh
  MobLimb& L = holder.limbs_[(size_t)slot];
  // WHAT IT RINGS LIKE: the struck voxel's material. Metal rings, wood and
  // leather thud -- read off the matter the blade met, so a heater hacked
  // through its iron face to the boards starts to thud.
  const uint32_t struck = holder.ShellMaterialAt(slot, at);
  o.ring = struck == 0u || holder.MaterialHardness(struck) >= 100.0f;
  // ---- THE WEAR: item hp, charged here and NOT through Mob::Damage ----------
  // Mob::Damage's hp-0 rule would Sever() an item slot (a sword knocked from a
  // hand), and a shield at zero BREAKS rather than drops -- see below. The
  // flash is the chip tier Mob::Damage gives every borrowed item slot.
  const Tuning& tune = CurrentTuning();
  const float wear =
      strike.Total() * std::clamp(power, 0.0f, 1.0f) *
      std::max(0.0f, tune.melee.blockItemDamage) * std::max(0.0f, sp.wear);
  if (std::getenv("SANDVOX_SHIELD_TRACE"))
    std::printf("shield trace: block on mob %llu slot %d: hp %.1f - wear %.1f "
                "(total %.1f power %.2f), vox %zu, impact %.1f vs %.1f\n",
                (unsigned long long)holder.Id(), slot, (double)L.hp,
                (double)wear, (double)strike.Total(), (double)power,
                L.voxels.size(), (double)BlockImpact(strike, power, heft),
                (double)sp.stability);
  if (holder.alive_) L.hp -= wear;
  L.hitFlash = std::max(L.hitFlash, tune.combatfx.flashChip);
  // ---- THE GUARD HOLDS, OR IS BEATEN OPEN -----------------------------------
  o.staggered = BlockImpact(strike, power, heft) > sp.stability;
  // The body rocks behind the guard (FindParry's reaction: the shield's own
  // borrowed slot has no worn host, so this is the creature behind it), and
  // harder when it was beaten open.
  const float react =
      o.staggered ? std::max(1.0f, tune.melee.shieldStaggerReact) : 1.0f;
  if (L.body != 0) HitReact(L.body, sweepDir, strike.Total() * react, power);
  if (o.staggered) {
    holder.StaggerShield(sp.staggerTicks);
    shieldStats_.staggers++;
  }
  // ---- AND AT ZERO IT COMES APART -------------------------------------------
  if (holder.alive_ && L.hp <= 0.0f) {
    o.broke = holder.BreakHeld(h);
    if (o.broke) shieldStats_.breaks++;
  }
  return o;
}
