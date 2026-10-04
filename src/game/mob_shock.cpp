// mob_shock.cpp — SHOCKS REACH BODIES (docs/PLAN_electricity.md section 4,
// package E4; DESIGN.md "Electricity -- shocks reach bodies").
//
// The charge field (src/sim/elec.h, sim_elec.wgsl) lives on the GPU and the
// bodies live on the CPU, so a body learns what it stands in by ASKING:
//
//   QueueShockQueries   end of MobSystem::PreTick, tick T: one world box per
//                       live base limb (the limb's collider box under its
//                       pose, dilated a cell -- a foot ON a charged plate or IN
//                       charged water overlaps the charged cells), queued on
//                       World::QueueElecQuery. Only while
//                       World::ElecMayBeLive(T): a world with no charge asks
//                       nothing (rule 2).
//   SubmitTick(T)       uploads them; sim_elec.wgsl elecQuery answers each with
//                       the settled field's max P, charged cells, P sum and
//                       cells scanned; the answers ride tick T's readback slot.
//   ApplyShocks         top of PreTick, tick T + K + 1 (K =
//                       World::kSnapshotLatency): World::TakeElecHits hands
//                       over the answers of every snapshot published since the
//                       last call -- exactly tick T's at the steady state -- in
//                       query order. FIXED LATENCY, the same contract every
//                       gameplay readback keeps: the answer a body acts on is a
//                       pure function of the tick, never of when a fence
//                       retired, so two runs (and the op record's replay, whose
//                       world is reproduced from the ops this pass emits)
//                       agree.
//
// What a shock does (Tuning::Gore section H, every number a knob):
//   effective P = box max P x (1 + shockWetGain x the limb's CONDUCTING coat
//                 fraction) x shockArmourGain when a worn shell conducts
//   hp          DamageCause::Electric on each touching limb and, by
//               shockTorsoShare, on the vital core (the current crosses the
//               body); no wound, no bleed (severpolicy.h's Electric row)
//   stun        Mob::stunUntil_ pushed later (never earlier); honoured by
//               DecideIntent and, for a player, TickAuthority
//   knock-down  at shockRagdollP: StartRagdoll(shockRagdollSeconds, "shock")
//   fire        at shockIgniteMinP, a hashed roll: Mob::Ignite on the hair, the
//               clothes and the touching limbs -- the burn pass's own door, so
//               a wet voxel refuses the flame exactly as it refuses a torch
//   twitch      TickStuns: Mob::HitReact in a hashed direction every
//               shockTwitchTicks while stunned (presentation only)
//
// Every roll is an integer hash of (creature id, tick, limb); nothing reads
// a clock or a pointer. Corpses and ghosts never ask: a corpse has no hp to
// lose, and a ghost's body is its owner's to shock.

#include <algorithm>
#include <cmath>

#include "game/anim.h"
#include "game/mob.h"
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/voxload.h"   // kBodyStainAmtMax
#include "sim/world.h"

namespace {

// The world box of one limb's collider under its current pose, dilated by a
// cell, clamped to kElecQueryAxisMax an axis (a pathological pose cannot make
// the GPU scan a building). The stain pass walks the same box
// (MobSystem::StainOneLimb's "IS ANYTHING THERE?").
bool LimbWorldBox(const BurnLimbView& v, int32_t lo[3], int32_t hi[3]) {
  if (v.xf == nullptr) return false;
  const Quat q{v.xf->quat[0], v.xf->quat[1], v.xf->quat[2], v.xf->quat[3]};
  const float sinv = 1.0f / (float)std::max(1u, v.physScale);
  Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
  for (int k = 0; k < 8; k++) {
    const Vec3 c{(float)((k & 1) ? v.size.x : v.sizeMin.x) * sinv,
                 (float)((k & 2) ? v.size.y : v.sizeMin.y) * sinv,
                 (float)((k & 4) ? v.size.z : v.sizeMin.z) * sinv};
    const Vec3 w = v.xf->pos + QuatRotate(q, c);
    mn.x = std::min(mn.x, w.x); mn.y = std::min(mn.y, w.y); mn.z = std::min(mn.z, w.z);
    mx.x = std::max(mx.x, w.x); mx.y = std::max(mx.y, w.y); mx.z = std::max(mx.z, w.z);
  }
  if (!(mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)) return false;
  const float m[3] = {mn.x, mn.y, mn.z}, M[3] = {mx.x, mx.y, mx.z};
  for (int a = 0; a < 3; a++) {
    lo[a] = ifloor(m[a]) - 1;
    hi[a] = ifloor(M[a]) + 1;
    if (hi[a] - lo[a] + 1 > (int32_t)kElecQueryAxisMax) {
      // Keep the LOW end on the vertical axis (the feet are what stand in
      // the water), the middle on the others.
      const int32_t over = hi[a] - lo[a] + 1 - (int32_t)kElecQueryAxisMax;
      if (a == 1) {
        hi[a] -= over;
      } else {
        lo[a] += over / 2;
        hi[a] = lo[a] + (int32_t)kElecQueryAxisMax - 1;
      }
    }
  }
  return true;
}

}  // namespace

void MobSystem::QueueShockQueries(uint32_t tick, World& world) {
  if (!world.ElecMayBeLive(tick)) return;
  auto ask = [&](Mob& m) {
    if (m.def_ == nullptr || !m.alive_ || m.IsGhost() || m.rigReleased_) return;
    const int n = std::min<int>(m.baseLimbs_, (int)m.limbs_.size());
    for (int li = 0; li < n; li++) {
      MobLimb& L = m.limbs_[li];
      if (!L.body) continue;
      const BurnLimbView v = m.ViewOf(L);
      ElecQuery q;
      if (!LimbWorldBox(v, q.lo, q.hi)) continue;
      q.mobId = m.id_;
      q.limb = li;
      world.QueueElecQuery(q);
      shockCounters_.queued++;
    }
  };
  // The players first: a crowd that fills the kElecQueryMax boxes refuses
  // NPCs (counted by World::QueueElecQuery), never the player.
  for (Mob* av : avatars_)
    if (av != nullptr) ask(*av);
  for (Mob& m : mobs_) ask(m);
}

void MobSystem::ApplyShocks(uint32_t tick, World& world) {
  std::vector<ElecHit> hits = world.TakeElecHits();
  if (hits.empty()) return;
  const Tuning::Gore& g = CurrentTuning().gore;
  shockCounters_.hitsRead += hits.size();
  // The answers arrive in QUERY order, so one body's limbs of one tick are
  // contiguous; a run is (mobId, tick).
  size_t i = 0;
  while (i < hits.size()) {
    size_t j = i + 1;
    while (j < hits.size() && hits[j].mobId == hits[i].mobId && hits[j].tick == hits[i].tick)
      j++;
    uint32_t anyP = 0;
    for (size_t k = i; k < j; k++) {
      anyP = std::max(anyP, hits[k].maxP);
      if (hits[k].maxP != 0) shockCounters_.hitsCharged++;
    }
    Mob* m = anyP != 0 ? FindCreature(hits[i].mobId) : nullptr;
    if (anyP != 0 && (m == nullptr || !m->alive_ || m->def_ == nullptr || m->IsGhost()))
      shockCounters_.stale++;
    if (anyP == 0 || m == nullptr || !m->alive_ || m->def_ == nullptr || m->IsGhost()) {
      i = j;
      continue;
    }
    Mob& mob = *m;
    Mob::ShockRecord& rec = mob.shock_;
    rec.maxP = std::max(rec.maxP, anyP);

    // ---- METAL ARMOUR: a worn shell of a conductor ---------------------------
    // Sampled, not swept: a few hundred voxels of each worn slot decide it,
    // and only on a tick this body is in charge at all.
    bool armour = false;
    for (int s = mob.baseLimbs_; s < (int)mob.limbs_.size() && !armour; s++) {
      if (!mob.IsWornSlot(s) || !mob.limbs_[s].body) continue;
      const BurnLimbView v = mob.ViewOf(mob.limbs_[s]);
      const size_t n = v.Size();
      const size_t step = std::max<size_t>(1, n / 256);
      for (size_t k = 0; k < n && !armour; k += step) {
        const uint32_t mt = v.Mat(k);
        const uint32_t r = mt < matElecResist_.size() ? matElecResist_[mt] : 0u;
        armour = r != 0 && (int)r <= g.shockArmourResistMax;
      }
    }
    const float armourGain = armour ? std::max(1.0f, g.shockArmourGain) : 1.0f;
    const float minP = (float)g.shockMinP * (armour ? g.shockArmourMinPScale : 1.0f);

    // ---- WHICH LIMBS ARE IN IT, and how hard --------------------------------
    struct Touch {
      int limb;
      float eff;
    };
    std::vector<Touch> touch;
    float peak = 0.0f, wetMax = 0.0f;
    for (size_t k = i; k < j; k++) {
      const ElecHit& h = hits[k];
      if (h.maxP == 0 || h.limb < 0 || h.limb >= (int)mob.limbs_.size()) continue;
      const MobLimb& L = mob.limbs_[h.limb];
      if (!L.body) continue;
      // WET: the share of the limb's coat that is a CONDUCTOR (water, blood,
      // brine: materials.json electric.resist), amount-weighted, 0..1.
      uint64_t num = 0;
      for (const CoatEntry& e : L.coat.top)
        if (e.mat != 0 && e.mat < matElecResist_.size() && matElecResist_[e.mat] != 0)
          num += e.sumAmt;
      const float wet =
          L.coat.voxels ? std::min(1.0f, (float)num / (float)(kBodyStainAmtMax * L.coat.voxels))
                        : 0.0f;
      const float eff = (float)h.maxP * (1.0f + std::max(0.0f, g.shockWetGain) * wet) * armourGain;
      if (eff < minP) continue;
      touch.push_back({h.limb, eff});
      peak = std::max(peak, eff);
      wetMax = std::max(wetMax, wet);
    }
    i = j;
    if (touch.empty()) continue;
    shockCounters_.bodiesShocked++;
    PushShockCue(mob, touch.front().limb, peak);  // the sound + HUD cue (presentation)
    if (rec.ticks == 0) rec.firstTick = tick;
    rec.ticks++;
    rec.lastTick = tick;
    rec.limbHits += (uint32_t)touch.size();
    rec.maxEffP = std::max(rec.maxEffP, peak);
    rec.wetMax = std::max(rec.wetMax, wetMax);
    rec.armour = armour;

    // ---- THE VITAL CORE the current crosses: the vital base limb with the
    // most authored hp (the torso on a human), alive.
    int core = -1;
    for (int li = 0; li < mob.baseLimbs_ && li < (int)mob.limbDefs_.size(); li++) {
      if (!mob.limbDefs_[li].vital || !mob.limbs_[li].body) continue;
      if (core < 0 || mob.limbDefs_[li].hp > mob.limbDefs_[core].hp) core = li;
    }

    // ---- HP ------------------------------------------------------------------
    float total = 0.0f;
    for (const Touch& t : touch) total += std::max(0.0f, g.shockHpPerKiloP) * t.eff / 1000.0f;
    const float scale =
        total > g.shockHpMaxPerTick && total > 0.0f ? g.shockHpMaxPerTick / total : 1.0f;
    const float share = std::clamp(g.shockTorsoShare, 0.0f, 1.0f);
    const DamageCtx ctx(DamageCause::Electric);
    float coreDose = 0.0f;
    for (const Touch& t : touch) {
      if (!mob.alive_) break;
      const float d = std::max(0.0f, g.shockHpPerKiloP) * t.eff / 1000.0f * scale;
      const float toCore = (core >= 0 && core != t.limb) ? d * share : 0.0f;
      const MobLimb& L = mob.limbs_[t.limb];
      if (d - toCore > 0.0f && L.body) {
        mob.Damage(L.body, d - toCore, L.xf.pos, 0.0f, ctx);
        rec.hp += d - toCore;
      }
      coreDose += toCore;
    }
    if (mob.alive_ && core >= 0 && coreDose > 0.0f && mob.limbs_[core].body) {
      mob.Damage(mob.limbs_[core].body, coreDose, mob.limbs_[core].xf.pos, 0.0f, ctx);
      rec.hp += coreDose;
    }

    // ---- STUN, and the lightning-class knock-down ----------------------------
    if (mob.alive_) {
      const int lo = std::max(0, g.shockStunMinTicks), hi = std::max(lo, g.shockStunMaxTicks);
      const int want = std::clamp(
          (int)std::lround(std::max(0.0f, g.shockStunTicksPerKiloP) * peak / 1000.0f), lo, hi);
      const uint32_t until = tick + (uint32_t)want;
      if (until > mob.stunUntil_) {
        rec.stunTicks += until - std::max(mob.stunUntil_, tick);
        mob.stunUntil_ = until;
      }
      if (g.shockRagdollP > 0 && peak >= (float)g.shockRagdollP && !mob.Ragdolled()) {
        mob.StartRagdoll(std::max(0.0f, g.shockRagdollSeconds), "shock");
        if (mob.Ragdolled()) rec.ragdolls++;
      }
    }

    // ---- FIRE: hair, clothes and the touching limbs --------------------------
    // One roll per body per tick, then Mob::Ignite on each candidate (the
    // burn pass takes it from there; a wet voxel refuses, IgniteOneLimb).
    if (g.shockIgniteMinP > 0 && peak >= (float)g.shockIgniteMinP && g.shockIgniteVoxels > 0 &&
        g.shockIgniteGain > 0.0f) {
      const float chance = std::min(1.0f, g.shockIgniteGain * peak / 1000.0f);
      const uint32_t h = rng::Hash3((uint32_t)mob.id_ ^ 0xE1EC5u, tick, (uint32_t)(mob.id_ >> 32));
      if (rng::Unit01(h) < chance) {
        for (int li = 0; li < (int)mob.limbs_.size(); li++) {
          if (!mob.limbs_[li].body) continue;
          bool cand = mob.IsWornSlot(li) || (li < mob.baseLimbs_ && mob.IsBloodless(li));
          for (const Touch& t : touch) cand = cand || t.limb == li;
          if (!cand) continue;
          rec.ignited += mob.Ignite(li, (uint32_t)g.shockIgniteVoxels);
        }
      }
    }
  }
}

void MobSystem::TickStuns(uint32_t tick) {
  const Tuning::Gore& g = CurrentTuning().gore;
  const uint32_t every = (uint32_t)std::max(1, g.shockTwitchTicks);
  auto twitch = [&](Mob& m) {
    if (!m.Stunned(tick) || m.def_ == nullptr || m.IsGhost() || m.Ragdolled()) return;
    if ((tick - m.shock_.lastTick) % every != 0) return;
    const int n = std::min<int>(m.baseLimbs_, (int)m.limbs_.size());
    if (n <= 0) return;
    const uint32_t h = rng::Hash3((uint32_t)m.id_ ^ 0x7A11C4u, tick, 0x5u);
    const int limb = (int)(h % (uint32_t)n);
    if (!m.limbs_[limb].body) return;
    const Vec3 dir{rng::SignedUnit(rng::Hash3(h, 1u, 0u)), 0.25f * rng::SignedUnit(rng::Hash3(h, 2u, 0u)),
                   rng::SignedUnit(rng::Hash3(h, 3u, 0u))};
    m.HitReact(limb, dir, std::max(0.0f, g.shockTwitchHp), 1.0f);
    m.shock_.twitches++;
  };
  for (Mob* av : avatars_)
    if (av != nullptr) twitch(*av);
  for (Mob& m : mobs_) twitch(m);
}

// The shock's cue for the frame (MobSystem::ShockCues): where it bit, how hard.
// Intensity is the effective P against the knock-down P (gore.shockRagdollP),
// so a lightning-class jolt is 1 and a tingle from wet ground is a fraction.
void MobSystem::PushShockCue(const Mob& m, int limb, float peakP) {
  constexpr size_t kMaxShockCues = 64;
  if (shockCues_.size() >= kMaxShockCues) return;
  const Tuning::Gore& g = CurrentTuning().gore;
  ShockCue c;
  c.mobId = m.id_;
  c.posVoxel = limb >= 0 && limb < (int)m.limbs_.size() ? m.limbs_[limb].xf.pos : Vec3{};
  const float ref = g.shockRagdollP > 0 ? (float)g.shockRagdollP : 30000.0f;
  c.intensity = std::clamp(peakP / ref, 0.05f, 1.0f);
  shockCues_.push_back(c);
}
