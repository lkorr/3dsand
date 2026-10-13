// pierce.cpp -- the penetrator on creatures, and a point along a ray
// (docs/PLAN_projectiles.md package B; game/pierce.h, phys/kerf.h).
//
// Mob::PierceLimb is the creature half of the one model: the channel is
// priced on the slot's authoritative (fine) lattice by phys/kerf.h and carved
// through the ordinary CarveLimb, so the hp a wound costs, the brain charge,
// the bleed, the gore particles and the soak are the ones every other carve
// gets. There is NO kill rule here and there must not be one: whether a thin
// arrow through a brain kills is the brain charge's answer (decision 1).

#include "game/pierce.h"

#include <algorithm>
#include <cmath>

#include "game/anim.h"  // QuatRotate / QuatRotateInv
#include "game/mob.h"
#include "game/spell.h"  // SpellBodyShot / SpellBodyStrike (SpellMatterStrike)
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/tuning.h"
#include "sim/world.h"

bool Mob::PierceLimb(uint64_t bodyHandle, Vec3 fromWorld, Vec3 dirWorld,
                     const Penetrator& pen, World& world,
                     std::vector<ParticleSpawn>& spawns, PierceResult& out) {
  out = PierceResult{};
  if (!def_ || !phys_ || sys_ == nullptr) return false;
  int li = -1;
  for (size_t i = 0; i < limbs_.size(); i++)
    if (limbs_[i].body == bodyHandle) {
      li = (int)i;
      break;
    }
  if (li < 0) return false;
  out.hit = true;
  const float dl = dirWorld.len();
  if (dl < 1e-6f) return true;
  const Tuning& tune = CurrentTuning();
  const Vec3 dW = dirWorld * (1.0f / dl);

  // ---- THE AXIS, IN THE SLOT'S OWN FRAME -----------------------------------
  // The pose read ONCE, here, and carved against exactly that one (CutLimb's
  // rule, gotcha-live-limb-carve-pose: a live limb is re-posed every tick).
  MobLimb& limb = limbs_[li];
  phys_->GetTransform(limb.body, limb.xf);
  const Quat q{limb.xf.quat[0], limb.xf.quat[1], limb.xf.quat[2],
               limb.xf.quat[3]};
  const Vec3 oL = QuatRotateInv(q, fromWorld - limb.xf.pos);
  const Vec3 dL = QuatRotateInv(q, dW);
  out.dirLocal = dL;
  const bool fine = limb.HasFineSkin();
  const float s =
      (float)std::max(1u, fine ? SkinScaleOf(limb) : PhysScaleOf(limb));
  const PierceLattice lat = PierceLatticeFor(pen.tipRadius, s, kVoxelMeters);

  // ---- ONE PRICE PER MATERIAL (KerfBite's resistance table, in joules) ------
  // Every material id, not the palette's 127: a body's voxels are rewritten
  // past it at load (anatomy.becomes -- every robot's plating).
  const PierceCost pc = PierceCostOf(tune);
  const float tipHard =
      pen.hardness > 0.0f ? pen.hardness : MaterialHardness(pen.material);
  const size_t nMat = sys_->matGpu_.size();
  thread_local std::vector<float> price;  // reused: one table per shot
  price.assign(nMat, 0.0f);
  for (uint32_t m = 0; m < (uint32_t)nMat; m++)
    price[m] = PierceCellJ(pc, MaterialHardness(m), tipHard, pen.sharpness,
                           lat.volScale);
  const float unknown =
      PierceCellJ(pc, 0.0f, tipHard, pen.sharpness, lat.volScale);
  auto costOf = [&](uint32_t m) { return m < nMat ? price[m] : unknown; };

  const float energy = pen.EnergyJ();
  out.energyIn = energy;
  const PierceChannelResult ch = PierceChannel(
      oL, dL, s, lat.rCells, energy,
      [&](auto&& probe) {
        if (fine)
          for (const PrefabVoxel& v : limb.skinVoxels) {
            const uint32_t m = v.material & 0xFFFu;
            if (m) probe((float)v.x, (float)v.y, (float)v.z, m);
          }
        else
          for (const DebrisVoxel& v : limb.voxels) {
            const uint32_t m = v.payload & 0xFFFu;
            if (m) probe((float)v.x, (float)v.y, (float)v.z, m);
          }
      },
      costOf);
  out.firstLayerJ = ch.firstLayerJ;
  out.stopMat = ch.stopMat;
  out.byMat = ch.byMat;
  out.cells = ch.cells;
  out.entryLocal = oL + dL * ch.tEntry;
  out.entryWorld = fromWorld + dW * ch.tEntry;
  if (!ch.any) {
    // The collider was met and the matter was not: the ray went through the
    // box merge's convex radius beside a thin limb. Passed it by.
    out.missed = true;
    out.outcome = PierceOutcome::Exited;
    out.energyLeft = energy;
    out.exitSpeedMps = pen.speedMps;
    out.stopLocal = oL;
    out.stopWorld = fromWorld;
    return true;
  }
  // What this creature has lost to everything, so a shot's charge is a delta
  // whatever cause books it (the glance's Blunt, the channel's Pierce, the
  // flat hp's Pierce).
  auto lostAll = [this]() {
    float t = 0.0f;
    for (int c = 0; c < (int)DamageCause::Count; c++) t += hpLostBy_[c];
    return t;
  };
  const bool wasAlive = alive_;
  const float lost0 = lostAll();

  // ---- A GLANCE: IT COULD NOT GET IN ----------------------------------------
  // Nothing is carved. The blow is a thrown thing's (PierceGlanceOf), through
  // the one door every trauma comes in by -- which also carries it through a
  // worn shell to the flesh beneath at gear.bluntThrough.
  if (ch.glanced) {
    out.outcome = PierceOutcome::Glanced;
    out.stopLocal = out.entryLocal;
    out.stopWorld = out.entryWorld;
    const PierceGlance g = PierceGlanceOf(tune, pen);
    if (g.hp > 0.0f) {
      ::BluntHit b;
      b.at = out.entryWorld;
      b.hp = g.hp;
      b.power = g.power;
      b.seed = pen.seed ^ 0x6A1A9Eu;
      BluntHit(bodyHandle, b, world, spawns);
    }
    out.hpCharged = lostAll() - lost0;
    out.killed = wasAlive && !alive_;
    return true;
  }

  const float stopT = ch.exited ? ch.tExit : ch.tStop;
  out.depth = std::max(0.0f, stopT - ch.tEntry);
  out.stopLocal = oL + dL * stopT;
  out.stopWorld = fromWorld + dW * stopT;
  if (ch.exited) {
    out.outcome = PierceOutcome::Exited;
    out.energyLeft = ch.leftJ;
    out.exitSpeedMps = pen.SpeedFor(ch.leftJ);
  } else {
    out.outcome = PierceOutcome::Embedded;
  }
  if (ch.cells == 0) return true;
  WakeDead();  // a shot into a sleeping corpse wakes it (Mob::Damage's rule)
  const DamageCtx ctx(DamageCause::Pierce);

  // ---- A WEAPON'S FLAT HP, IF IT HAS ONE ------------------------------------
  // Melee's order: the strike's hp, then the wound. Impact speed 0: the
  // knock-loose rule is a sword-out-of-a-hand rule, and an arrow at 60 m/s is
  // 600 vox/s, past every authored severImpactSpeed.
  if (pen.hp > 0.0f) {
    Damage(bodyHandle, pen.hp, out.entryWorld, 0.0f, ctx);
    li = -1;
    for (size_t i = 0; i < limbs_.size(); i++)
      if (limbs_[i].body == bodyHandle) {
        li = (int)i;
        break;
      }
    if (li < 0) {
      out.hpCharged = lostAll() - lost0;
      out.killed = wasAlive && !alive_;
      out.severed = true;
      return true;
    }
  }

  // ---- THE CHANNEL, CARVED --------------------------------------------------
  // PierceKeepAt on the authoritative lattice IS the channel's own axis test,
  // so CarveLimb removes exactly the cells paid for; the collider is
  // re-derived from them. No spall: a bore is clean (the laser's reason).
  Mob::CarveBounds box;
  {
    const float r = lat.rCells / s + 1.0f / s;
    const Vec3 a = oL + dL * ch.tEntry, b = out.stopLocal;
    box.lo = Vec3{std::min(a.x, b.x) - r, std::min(a.y, b.y) - r,
                  std::min(a.z, b.z) - r};
    box.hi = Vec3{std::max(a.x, b.x) + r, std::max(a.y, b.y) + r,
                  std::max(a.z, b.z) + r};
  }
  const float rC = lat.rCells, t0Lat = ch.t0Lat;
  const int binStop = ch.binStop;
  Mob::CarveReport rep;
  const bool kept = CarveLimb(
      li, ctx, world, spawns, /*eject=*/true,
      [oL, dL, s, rC, t0Lat, binStop](float scale) -> LimbCarveKeep {
        const PierceKeep k = PierceKeepAt(oL, dL, s, rC, t0Lat, binStop, scale);
        return [k](int x, int y, int z) {
          return k((float)x, (float)y, (float)z);
        };
      },
      /*spall=*/nullptr, &rep, &box);
  out.carved = (uint32_t)rep.cells.size();
  out.hpCharged = lostAll() - lost0;
  out.killed = wasAlive && !alive_;
  // SEVERED OR DEAD-AND-RELEASED: nothing below may touch the slot (the
  // contract every CarveLimb caller keeps). NOT "the handle changed": a carve
  // rebuilds the slot's Jolt body under a new handle, and the slot (by index)
  // is the same limb (CutLimb reads limbs_[i] after a carve for that reason).
  const bool slotLives = li < (int)limbs_.size() && limbs_[li].body != 0;
  if (!kept || !slotLives) {
    out.severed = !slotLives;
    return true;
  }
  // The drip comes out of the HOLE, not the middle of the channel CarveLimb's
  // centroid would put it at (CutLimb's stab reason).
  limbs_[li].woundLocal = out.entryLocal;
  // ...and the soak goes exactly where matter left, as a crater's does.
  if (!rep.cells.empty())
    StainWound(li, rep.centreLocal, 0.0f, pen.seed ^ 0x9E2CEu, &rep.cells,
               tune.gore.craterStainRim);
  // ---- THE BODY ANSWERS (Mob::Damage's flash and cry, when no flat hp ran) --
  if (pen.hp <= 0.0f && (size_t)li < limbDefs_.size()) {
    const MobLimbDef& ld = limbDefs_[li];
    const Tuning::CombatFx& fx = tune.combatfx;
    limbs_[li].hitFlash = std::max(
        limbs_[li].hitFlash, ld.tag == "item" ? fx.flashChip : fx.flashFlesh);
    if (alive_ && li < baseLimbs_ && !IsBloodless(li) && out.hpCharged > 0.0f)
      sys_->PushVoice(*this, MobSystem::VoiceKind::Hurt, out.entryWorld,
                      ld.hp > 0 ? out.hpCharged / ld.hp : 1.0f);
  }
  return true;
}

bool MobSystem::PierceLimb(uint64_t bodyHandle, Vec3 fromWorld, Vec3 dirWorld,
                           const Penetrator& pen, World& world,
                           std::vector<ParticleSpawn>& spawns,
                           PierceResult& out) {
  // Every creature, the avatar included (FindOwner's walk, CutLimb's reason).
  int li = -1;
  Mob* m = FindOwner(bodyHandle, &li);
  if (m == nullptr || li < 0) {
    out = PierceResult{};
    return false;
  }
  return m->PierceLimb(bodyHandle, fromWorld, dirWorld, pen, world, spawns,
                       out);
}

PierceFlight PierceAlongRay(Physics& phys, MobSystem& mobs, DebrisSystem& debris,
                            World& world, std::vector<ParticleSpawn>& spawns,
                            Vec3 fromWorld, Vec3 dirWorld, float maxDistVox,
                            const Penetrator& pen,
                            const std::vector<uint64_t>& ignore) {
  PierceFlight f;
  f.speedMps = pen.speedMps;
  const float dl = dirWorld.len();
  if (dl < 1e-6f || !(maxDistVox > 0.0f)) {
    f.endWorld = fromWorld;
    return f;
  }
  const Vec3 d = dirWorld * (1.0f / dl);
  Penetrator cur = pen;
  // Bodies already met never stop the ray again: the channel's far side may
  // still be inside the collider's convex radius, and a cast from there would
  // report the same body at fraction 0.
  std::vector<uint64_t> skip = ignore;
  float at = 0.0f;  // world voxels of the ray consumed
  // kPierceMaxBodies resolved hits, plus as many capsule skips (each capsule
  // is skipped once), so the loop is bounded either way.
  for (int k = 0; k < 2 * kPierceMaxBodies &&
                  (int)f.hits.size() < kPierceMaxBodies;
       k++) {
    const float remain = maxDistVox - at;
    if (remain <= 1e-3f) break;
    const Vec3 from = fromWorld + d * at;
    float frac = 1.0f;
    const uint64_t body = phys.CastRayBody(from, d, remain, frac, skip);
    if (body == 0) break;
    skip.push_back(body);
    const float hitAt = at + std::clamp(frac, 0.0f, 1.0f) * remain;
    // A PLAYER'S CAPSULE encloses the torso; the limbs inside it are what the
    // point meets (ApplyContactDamage's W2-K note). Skipped, not advanced past.
    bool capsule = false;
    for (const Mob* av : mobs.Avatars())
      if (av != nullptr && av->CollisionOwner() == body) capsule = true;
    if (capsule) continue;
    // The channel looks for matter from a little in front of the collider
    // hit, never from behind where this cast started.
    const float back = std::min(kPierceBackVox, hitAt - at);
    const Vec3 origin = fromWorld + d * (hitAt - back);
    PierceHit h;
    h.body = body;
    h.along = hitAt;
    int li = -1;
    if (Mob* m = mobs.FindOwner(body, &li)) {
      h.mobId = m->Id();
      h.limb = li;
      mobs.PierceLimb(body, origin, d, cur, world, spawns, h.r);
    } else {
      debris.PierceBody(body, origin, d, cur, world, spawns, h.r);
    }
    // Met the collider, not the matter: no hit, and on past it.
    if (h.r.hit && h.r.missed) continue;
    f.hits.push_back(h);
    if (!h.r.hit) {
      // Nobody can carve it (a door leaf, a bomb, a prop): it stops the point
      // at its face, and what happens to the projectile is the caller's.
      f.stopped = true;
      f.speedMps = 0.0f;
      f.travelled = hitAt;
      f.endWorld = fromWorld + d * hitAt;
      return f;
    }
    if (h.r.outcome != PierceOutcome::Exited) {
      f.stopped = true;
      f.speedMps = 0.0f;
      f.travelled = std::max(0.0f, (h.r.stopWorld - fromWorld).dot(d));
      f.endWorld = h.r.stopWorld;
      return f;
    }
    // THROUGH: on from the far side, at what is left.
    cur.speedMps = h.r.exitSpeedMps;
    at = std::max(hitAt, (h.r.stopWorld - fromWorld).dot(d));
  }
  f.speedMps = cur.speedMps;
  f.travelled = maxDistVox;
  f.endWorld = fromWorld + d * maxDistVox;
  return f;
}

namespace {

// The first CREATURE SKIN (or loose body) on a segment, for a matter that does
// not go in: CastRayBody body by body (PierceAlongRay's walk, its capsule
// rule), and for a creature the nearest of its own voxels under the ray
// (MobSystem::PickBody) -- a collider met with no voxel under the ray is passed
// by, as a channel with no matter on its axis is. `pick.hit` false and
// `body` 0 = nothing met.
struct FirstBody {
  uint64_t body = 0;
  uint64_t mobId = 0;
  Vec3 at{};
  MobSystem::BodyRayHit pick;
};
FirstBody FirstBodyOn(Physics& phys, MobSystem& mobs, Vec3 from, Vec3 d, float len,
                      const std::vector<uint64_t>& ignore) {
  FirstBody fb;
  std::vector<uint64_t> skip = ignore;
  float at = 0.0f;
  for (int k = 0; k < 2 * kPierceMaxBodies; k++) {
    const float remain = len - at;
    if (remain <= 1e-3f) break;
    float frac = 1.0f;
    const uint64_t body = phys.CastRayBody(from + d * at, d, remain, frac, skip);
    if (body == 0) break;
    skip.push_back(body);
    const float hitAt = at + std::clamp(frac, 0.0f, 1.0f) * remain;
    bool capsule = false;
    for (const Mob* av : mobs.Avatars())
      if (av != nullptr && av->CollisionOwner() == body) capsule = true;
    if (capsule) continue;
    int li = -1;
    if (Mob* m = mobs.FindOwner(body, &li)) {
      // From a little in front of the collider (KerfFrame's `back`), over a
      // body's depth past it: the skin cell the ray actually meets.
      const float back = std::min(kPierceBackVox, hitAt);
      const Vec3 o = from + d * (hitAt - back);
      const MobSystem::BodyRayHit pk = mobs.PickBody(m->Id(), o, d, back + 8.0f);
      if (!pk.hit) continue;  // the box merge's convex radius, not the matter
      fb.body = body;
      fb.mobId = m->Id();
      fb.pick = pk;
      fb.at = pk.pos;
      return fb;
    }
    fb.body = body;  // loose matter (a crate, a severed piece): its collider
    fb.at = from + d * hitAt;
    return fb;
  }
  return fb;
}

}  // namespace

void SpellMatterStrike(Physics& phys, MobSystem& mobs, DebrisSystem& debris, World& world,
                       std::vector<ParticleSpawn>& spawns, const SpellBodyShot& shot,
                       const std::vector<uint64_t>& ignore, SpellBodyStrike& out) {
  out = SpellBodyStrike{};
  const Vec3 seg = shot.to - shot.from;
  const float len = seg.len();
  if (len < 1e-4f || shot.kind == SpellMatterKind::None) return;
  const Vec3 d = seg * (1.0f / len);
  const Tuning& tune = CurrentTuning();
  // What it carried in, whatever it does with it (the report's number).
  out.energyJ = 0.5f * std::max(shot.massKg, 0.0f) * shot.speedMps * shot.speedMps;

  if (shot.kind == SpellMatterKind::Splash || shot.kind == SpellMatterKind::Touch) {
    const FirstBody fb = FirstBodyOn(phys, mobs, shot.from, d, len, ignore);
    if (fb.body == 0) return;
    out.hit = true;
    out.bodies = 1;
    out.at = fb.at;
    out.mobId = fb.mobId;
    out.limb = fb.pick.limb;
    // A SPLASH: the coat on the skin under the ray, the pour brush's own
    // writers (accumulate up to a full coat, a washer rinses first, the
    // material's coat.effects run on every limb it touched). Its own rules do
    // the rest on later ticks: acid eats, lava sears and lights, water wets.
    if (shot.kind == SpellMatterKind::Splash && fb.mobId != 0 && shot.coatAmount > 0) {
      const float hp0 = mobs.TotalHp(fb.mobId);
      uint32_t marked = 0;
      mobs.PourOnBody(fb.mobId, fb.pick, d, std::max(0.1f, shot.splashRadius), shot.material,
                      shot.coatAmount, shot.tick, &marked);
      out.coated = marked;
      out.tookMatter = marked > 0;
      out.hp = std::max(0.0f, hp0 - mobs.TotalHp(fb.mobId));
    }
    return;
  }

  // ---- PIERCE / SCOUR: the matter as a point -----------------------------------
  Penetrator pen;
  pen.massKg = shot.massKg;
  pen.speedMps = shot.speedMps;
  pen.tipRadius = shot.tipRadius;
  pen.hardness = shot.hardness;
  pen.sharpness = shot.sharpness;
  pen.material = shot.material;
  pen.seed = shot.seed;
  const bool scour = shot.kind == SpellMatterKind::Scour;
  if (scour && shot.scourBins > 0) {
    // A SCOUR spends at most `scourBins` bins of skin-hard matter (hardness
    // gear.cutHardnessRef, the convention KerfBite's table keeps) at its own
    // tip: a full bin is pi r^2 fine cells. Whatever the grit carried past
    // that is not a deeper wound.
    const PierceCost pc = PierceCostOf(tune);
    const float tipHard = pen.hardness > 0.0f ? pen.hardness : 0.0f;
    const float r = std::max(pen.tipRadius, 0.25f);
    const float capJ = (float)shot.scourBins * 3.14159265f * r * r *
                       PierceCellJ(pc, pc.hardRef, tipHard, pen.sharpness, 1.0f);
    if (pen.EnergyJ() > capJ) pen.speedMps = pen.SpeedFor(capJ);
  }
  out.energyJ = pen.EnergyJ();
  const PierceFlight f =
      PierceAlongRay(phys, mobs, debris, world, spawns, shot.from, d, len, pen, ignore);
  if (f.hits.empty()) return;
  out.hit = true;
  out.bodies = (uint32_t)f.hits.size();
  const PierceHit& first = f.hits.front();
  out.mobId = first.mobId;
  out.limb = first.limb;
  out.outcome = (uint8_t)first.r.outcome;
  out.depthVox = first.r.depth;
  for (const PierceHit& h : f.hits) {
    out.cells += h.r.cells;
    out.hp += h.r.hpCharged;
    out.killed = out.killed || h.r.killed;
    for (const PierceMatCells& pm : h.r.byMat) {
      bool merged = false;
      for (SpellBodyStrike::MatCells& mc : out.byMat)
        if (mc.mat == pm.mat) {
          mc.cells += pm.cells;
          merged = true;
        }
      if (!merged) out.byMat.push_back({pm.mat, pm.cells});
    }
    if (h.r.hit)
      out.spentJ += h.r.outcome == PierceOutcome::Exited
                        ? std::max(0.0f, h.r.energyIn - h.r.energyLeft)
                        : h.r.energyIn;
  }
  if (!f.stopped && !scour) {
    // THROUGH: it flies on from the far side of the last body it left (which
    // may lie past this tick's segment) at what it has left.
    out.through = true;
    out.exitSpeedMps = f.speedMps;
    out.at = f.endWorld;
    const PierceHit& last = f.hits.back();
    if ((last.r.stopWorld - shot.from).dot(d) > (out.at - shot.from).dot(d))
      out.at = last.r.stopWorld;
    return;
  }
  // STOPPED (or a scour, which always stops): in the last body it met, at the
  // channel's end -- or, for grit that went clean through something thin, at
  // the first body's entry, where it struck.
  const PierceHit& last = f.hits.back();
  out.at = f.stopped ? f.endWorld : first.r.entryWorld;
  // THE LEFTOVER MATTER ACTS ON THE WOUND, through what a body already does
  // with it. Only inside a creature that is still there to hold it.
  if (!f.stopped || last.mobId == 0 || !last.r.hit || last.r.severed) return;
  if (last.r.outcome == PierceOutcome::Glanced) return;  // nothing got in
  if (shot.coatAmount > 0) {
    // A matter a body can wear coats the floor of the channel: the cells the
    // axis meets past where it stopped, and the wound's rim round them.
    const float back = 2.0f;
    const Vec3 o = out.at - d * back;
    const MobSystem::BodyRayHit pk = mobs.PickBody(last.mobId, o, d, back + 4.0f);
    if (pk.hit) {
      uint32_t marked = 0;
      mobs.PourOnBody(last.mobId, pk, d, std::max(0.1f, shot.splashRadius * 0.5f),
                      shot.material, shot.coatAmount, shot.tick, &marked);
      out.coated = marked;
      out.tookMatter = marked > 0;
    }
  }
  if (shot.hot && last.limb >= 0) {
    // A HOT lump lit inside the wound: the limb's own burn rules from there
    // (IgniteLimb resolves what each voxel burns into; bone simply refuses).
    const uint32_t lit = mobs.IgniteLimb(last.mobId, last.limb, 4u);
    out.tookMatter = out.tookMatter || lit > 0;
  }
}
