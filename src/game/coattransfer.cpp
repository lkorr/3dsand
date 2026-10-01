// coattransfer.cpp — A COAT MOVES ON CONTACT (DESIGN.md §7, docs/PLAN_weapon_coats.md A)
//
// Two halves, both about a coat that belongs to a THING rather than to a body:
//
//   1. THE EXCHANGE (MobSystem::CoatOnContact). When a striking slot -- a held
//      weapon, a gauntlet, a bare fist, a set of jaws -- lands a blow, part of
//      the coat on ITS voxels at the contact moves onto the voxels it struck,
//      part of what those wear moves back, and the wound's own fluid smears the
//      voxels that went in. No material is special: oil goes in and is oil in
//      the wound, acid goes in and eats, water goes in and rinses. What a coat
//      DOES after it lands is the body-coat rule's (Mob::BurnOneLimb §0), not
//      this file's. The lattice arithmetic is phys/coatcontact.h.
//
//   2. THE ITEM KEEPS IT (Mob::CaptureHeld / EquipItemAsWas / ApplyLimbCoats).
//      A held item is a rig slot whose lattice is destroyed when it leaves the
//      hand. Worn armour already carried its lattice off the body in
//      WornShellDamage (iteminstance.h); a held item now does too, in the same
//      record, so a coated blade goes into the bag coated and comes out coated
//      -- and while it is in the bag nothing ticks it (frozen: the drying pass
//      only ever walks a rig's live slots).
//
// Member functions of Mob and MobSystem defined in their own TU, so the coat
// code does not live in the middle of mob.cpp's 30k lines.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "game/anim.h"
#include "game/item.h"
#include "game/mob.h"
#include "phys/coatcontact.h"
#include "sim/scale.h"
#include "sim/tuning.h"

namespace {

// A rig slot's AUTHORITATIVE lattice (the skin when it is finer, else the
// collider) and the scale its cells are at -- BurnLimbView's rule.
struct SlotLattice {
  StainLattice L;
  float scale = 1.0f;
};

uint64_t CellKey(int x, int y, int z) {
  return ((uint64_t)(uint16_t)(int16_t)x << 32) |
         ((uint64_t)(uint16_t)(int16_t)y << 16) | (uint64_t)(uint16_t)(int16_t)z;
}

}  // namespace

// ============================================================================
// THE ITEM KEEPS ITS COAT
// ============================================================================

void Mob::LatticeOfSlot(int slot, std::vector<PrefabVoxel>& out) const {
  out.clear();
  if (slot < 0 || slot >= (int)limbs_.size()) return;
  const MobLimb& L = limbs_[slot];
  if (L.HasFineSkin()) {
    out = L.skinVoxels;
    return;
  }
  out.reserve(L.voxels.size());
  for (const DebrisVoxel& v : L.voxels) {
    PrefabVoxel p{(int16_t)v.x, (int16_t)v.y, (int16_t)v.z, v.payload, v.color};
    p.stain = v.stain;
    out.push_back(p);
  }
}

bool Mob::CaptureHeld(Hand h, WornDamage& out) const {
  out.Clear();
  const HeldHand& hh = held_[HandIndex(h)];
  if (hh.slot < 0 || hh.slot >= (int)limbs_.size() || hh.borrowed ||
      hh.item.empty())
    return false;
  const MobLimb& L = limbs_[hh.slot];
  // One shell: a held item is one model. Recorded with CaptureWorn's rule --
  // only what differs from the authored item -- so a clean, whole blade
  // captures as nothing and its stack stays plain.
  WornShellDamage d;
  const float authoredHp =
      hh.slot < (int)limbDefs_.size() ? limbDefs_[hh.slot].hp : L.hp;
  if (L.hp != authoredHp) d.hp = L.hp;
  const size_t now = L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size();
  d.atSpawn = L.voxelsAtSpawn;
  d.live = (uint32_t)now;
  bool coated = false;
  if (L.HasFineSkin()) {
    for (const PrefabVoxel& v : L.skinVoxels)
      if (v.stain) { coated = true; break; }
  } else {
    for (const DebrisVoxel& v : L.voxels)
      if (v.stain) { coated = true; break; }
  }
  if (now != (size_t)L.voxelsAtSpawn || coated) LatticeOfSlot(hh.slot, d.lattice);
  out.shells.push_back(std::move(d));
  return true;
}

uint32_t Mob::ApplyLimbCoats(int slot, const std::vector<PrefabVoxel>& lat) {
  if (slot < 0 || slot >= (int)limbs_.size() || lat.empty()) return 0;
  MobLimb& L = limbs_[slot];
  const bool fine = L.HasFineSkin();
  const size_t n = fine ? L.skinVoxels.size() : L.voxels.size();
  if (n == 0) return 0;
  auto posOf = [&](size_t i) -> IVec3 {
    return fine ? IVec3{L.skinVoxels[i].x, L.skinVoxels[i].y, L.skinVoxels[i].z}
                : IVec3{L.voxels[i].x, L.voxels[i].y, L.voxels[i].z};
  };
  // The common case is the SAME lattice in the same order (the item was
  // captured from exactly this geometry); a position map only when it is not.
  bool aligned = lat.size() == n;
  for (size_t i = 0; aligned && i < n; i++) {
    const IVec3 p = posOf(i);
    aligned = p.x == lat[i].x && p.y == lat[i].y && p.z == lat[i].z;
  }
  std::unordered_map<uint64_t, size_t> at;
  if (!aligned) {
    at.reserve(n);
    for (size_t i = 0; i < n; i++) {
      const IVec3 p = posOf(i);
      at.emplace(CellKey(p.x, p.y, p.z), i);
    }
  }
  MicroBodySet* micro = MicroSet();
  int brick = -2;   // -2 = not asked yet, -1 = cannot poke
  auto poke = [&](const IVec3& p, uint16_t s) {
    if (brick == -2) {
      brick = -1;
      if (micro && L.microModel >= 0) {
        const int own = MicroBodyOwn(*micro, (uint32_t)L.microModel);
        if (own >= 0) {
          L.microModel = own;
          L.carved = true;
          L.flipbookModel = -1;
          brick = own;
        }
      }
    }
    if (brick >= 0) MicroBodyPokeStain(*micro, (uint32_t)brick, p.x, p.y, p.z, s);
  };
  uint32_t changed = 0;
  for (size_t k = 0; k < lat.size(); k++) {
    size_t i = k;
    if (!aligned) {
      auto it = at.find(CellKey(lat[k].x, lat[k].y, lat[k].z));
      if (it == at.end()) continue;
      i = it->second;
    }
    uint16_t& s = fine ? L.skinVoxels[i].stain : L.voxels[i].stain;
    if (s == lat[k].stain) continue;
    s = lat[k].stain;
    poke(posOf(i), s);
    changed++;
  }
  if (changed) coatDirty_ = twinDirty_ = true;
  return changed;
}

int Mob::ApplyKitCoats(int equipSlot) {
  if (equipSlot < 0 || equipSlot >= kEquipSlotCount || rigReleased_) return -1;
  const ItemStack& st = kit_.equip.slots[equipSlot];
  if (st.Empty()) return -1;
  // A stack that records no lattice for a shell is CLEAN there: what the rig
  // wears is washed to match, so a stage that rinsed a blade and settled the
  // record (ItemLatticeSettle) rinses the blade in the fist too.
  auto push = [&](int slot, const WornShellDamage* sh) -> uint32_t {
    if (sh && !sh->lattice.empty()) return ApplyLimbCoats(slot, sh->lattice);
    std::vector<PrefabVoxel> clean;
    LatticeOfSlot(slot, clean);
    for (PrefabVoxel& v : clean) v.stain = 0;
    return ApplyLimbCoats(slot, clean);
  };
  Hand h = Hand::Right;
  if (EquipSlotIsHand(equipSlot, &h)) {
    const HeldHand& hh = held_[HandIndex(h)];
    if (hh.slot < 0 || hh.borrowed || hh.kitStale || hh.item != st.name)
      return -1;
    return (int)push(hh.slot, st.damage.shells.empty() ? nullptr
                                                       : &st.damage.shells[0]);
  }
  for (const WornPiece& p : worn_) {
    if (p.equipSlot != equipSlot || p.item != st.name) continue;
    uint32_t changed = 0;
    for (size_t k = 0; k < p.slots.size() && k < p.cover.size(); k++) {
      const int ci = p.cover[k];
      const WornShellDamage* sh =
          ci >= 0 && (size_t)ci < st.damage.shells.size()
              ? &st.damage.shells[(size_t)ci]
              : nullptr;
      changed += push(p.slots[k], sh);
    }
    return (int)changed;
  }
  return -1;
}

bool Mob::EquipItemAsWas(const ItemDef* item, Hand h, const WornDamage* was) {
  if (!EquipItem(item, h)) return false;
  if (!item || !was || was->shells.empty() || was->shells[0].Empty())
    return true;
  const int slot = held_[HandIndex(h)].slot;
  if (slot < 0 || slot >= (int)limbs_.size()) return true;
  const WornShellDamage& sh = was->shells[0];
  if (sh.lattice.empty()) {
    if (sh.hp >= 0.0f) limbs_[slot].hp = sh.hp;
    return true;
  }
  // THE SAME GEOMETRY: only coats differ, so only coats are written -- no
  // collider rebuild, no new body handle, the grip point stays where
  // EquipItem just measured it. A lattice that LOST voxels is put back whole
  // (RestoreShellLattice, the worn-shell path).
  const MobLimb& L = limbs_[slot];
  const size_t n = L.HasFineSkin() ? L.skinVoxels.size() : L.voxels.size();
  bool same = n == sh.lattice.size();
  for (size_t i = 0; same && i < n; i++) {
    const PrefabVoxel& v = sh.lattice[i];
    if (L.HasFineSkin()) {
      const PrefabVoxel& c = L.skinVoxels[i];
      same = c.x == v.x && c.y == v.y && c.z == v.z &&
             (c.material & 0xFFFu) == (v.material & 0xFFFu);
    } else {
      const DebrisVoxel& c = L.voxels[i];
      same = c.x == v.x && c.y == v.y && c.z == v.z &&
             (c.payload & 0xFFFu) == (v.material & 0xFFFu);
    }
  }
  if (same) {
    if (sh.hp >= 0.0f) limbs_[slot].hp = sh.hp;
    ApplyLimbCoats(slot, sh.lattice);
  } else {
    RestoreShellLattice(slot, sh);
  }
  return true;
}

// ============================================================================
// WHICH SLOT STRUCK
// ============================================================================

bool Mob::StrikerEdgeLocal(bool haft, int& slot, Vec3& from, Vec3& to) const {
  slot = -1;
  if (!def_) return false;
  int part = -1, natural = -1;
  StrikeEffectorMode mode = StrikeEffectorMode::None;
  if (!ResolveEffector(part, mode, natural)) return false;
  if (mode != StrikeEffectorMode::Held) {
    // A natural weapon: its edge on its part, WeaponEdge's composition.
    if (haft) return false;
    const MobNaturalWeaponDef* nw = NaturalWeapon(natural);
    if (!nw || part < 0 || part >= (int)limbs_.size()) return false;
    slot = part;
    from = nw->edgeFrom;
    to = nw->edgeTo;
    return true;
  }
  const int held = HeldSlot();
  if (held < 0 || held >= (int)limbDefs_.size() || held >= (int)limbs_.size())
    return false;
  const MobLimbDef& ld = limbDefs_[held];
  if (haft) {
    if (!ld.hasHaft) return false;
    from = ld.haftFrom;
    to = ld.haftTo;
  } else {
    if (!ld.hasEdge) return false;
    from = ld.edgeFrom;
    to = ld.edgeTo;
  }
  slot = held;
  return true;
}

// ============================================================================
// THE EXCHANGE
// ============================================================================

MobSystem::CoatContactResult MobSystem::CoatOnContact(const CoatContact& c) {
  CoatContactResult r;
  lastCoat_ = r;
  const Tuning& tune = CurrentTuning();
  const Tuning::Gear& gear = tune.gear;
  const float frac = std::clamp(gear.coatTransferFrac, 0.0f, 1.0f);
  const uint32_t maxLevels = (uint32_t)std::max(0, gear.coatTransferMax);
  const uint32_t bleedAmt =
      (uint32_t)std::clamp(gear.coatBleedPickup, 0, (int)kBodyStainAmtMax);
  if ((frac <= 0.0f || maxLevels == 0) && bleedAmt == 0) return r;
  if (phys_ == nullptr) return r;
  Mob* striker = FindCreature(c.strikerId);
  Mob* target = FindCreature(c.targetId);
  if (!striker || !target || striker == target) return r;

  // ---- THE STRUCK SLOT, STILL THE ONE THAT WAS STRUCK ----------------------
  // Resolved by the caller before the resolvers ran (a carve rebuilds the
  // body and changes its handle); checked by NAME after, because a severed
  // shell erases its slot and renumbers the appended tail.
  const int ts = c.targetSlot;
  if (ts < 0 || ts >= (int)target->limbs_.size() ||
      ts >= (int)target->limbDefs_.size() || !target->limbs_[ts].body ||
      target->limbDefs_[ts].name != c.targetSlotName)
    return r;

  // ---- WHICH OF THE STRIKER'S VOXELS TOUCHED (plan A1) ---------------------
  //
  // NOT the probe's world point. The sweep's contact lies ON its edge
  // segment (every probe runs down the blade's own axis), so the honest
  // statement of "where on the weapon did it land" is how far along that
  // segment -- `edgeU` -- and that maps straight onto the slot's own authored
  // edge in its own body frame, with no world transform to disagree with the
  // renderer's. A tip strike lands at the tip whatever pose the blade is in.
  int es = -1;
  Vec3 from, to;
  if (!striker->StrikerEdgeLocal(c.haft, es, from, to)) return r;
  if (es < 0 || es >= (int)striker->limbs_.size() || !striker->limbs_[es].body)
    return r;
  const float u = std::clamp(c.edgeU, 0.0f, 1.0f);
  Vec3 sLocal = from + (to - from) * u;
  int ss = es;
  const float rw =
      std::max(c.radius, MetresToCells(std::max(gear.coatContactRadius, 0.0f)));

  // ...ON THE GAUNTLET, when there is one. A worn shell over the striking
  // part is what actually meets the target (StrikeProfileFor already lets it
  // decide the blow), so it is also what carries the coat: the shell hosted on
  // the part whose surface comes nearest the contact, if it is within the
  // contact's reach of it.
  if (es < striker->baseLimbs_ && striker->LimbHasShells(es)) {
    const MobLimb& pl = striker->limbs_[es];
    const Quat pq{pl.xf.quat[0], pl.xf.quat[1], pl.xf.quat[2], pl.xf.quat[3]};
    const Vec3 w = pl.xf.pos + QuatRotate(pq, sLocal);
    float best = rw + 1.0f;
    for (int s = striker->baseLimbs_; s < (int)striker->limbs_.size(); s++) {
      MobLimb& sl = striker->limbs_[s];
      if (sl.wornHost != es || !sl.body || !striker->IsWornSlot(s)) continue;
      const Quat sq{sl.xf.quat[0], sl.xf.quat[1], sl.xf.quat[2], sl.xf.quat[3]};
      const Vec3 loc = QuatRotateInv(sq, w - sl.xf.pos);
      SlotLattice v;
      const bool fine = sl.HasFineSkin();
      v.L.skin = fine ? &sl.skinVoxels : nullptr;
      v.L.coll = fine ? nullptr : &sl.voxels;
      v.scale = (float)std::max(1u, fine ? striker->SkinScaleOf(sl)
                                         : striker->PhysScaleOf(sl));
      const LatticeOcc occ = BuildLatticeOcc(v.L);
      const std::vector<CoatCell> near =
          CoatContactCells(v.L, occ, loc * v.scale, 0.0f, false);
      if (near.empty()) continue;
      const float d = near.front().dist / v.scale;   // world voxels
      if (d < best) {
        best = d;
        ss = s;
        sLocal = loc;
      }
    }
  }
  r.strikerSlot = ss;

  auto latticeOf = [](Mob& m, int slot) {
    SlotLattice v;
    MobLimb& l = m.limbs_[slot];
    const bool fine = l.HasFineSkin();
    v.L.skin = fine ? &l.skinVoxels : nullptr;
    v.L.coll = fine ? nullptr : &l.voxels;
    v.scale = (float)std::max(1u, fine ? m.SkinScaleOf(l) : m.PhysScaleOf(l));
    return v;
  };
  SlotLattice S = latticeOf(*striker, ss);
  SlotLattice T = latticeOf(*target, ts);
  if (S.L.Size() == 0 || T.L.Size() == 0) return r;

  // The contact in the target's frame, off its live transform -- what every
  // resolver measured its own wound in.
  MobLimb& tl = target->limbs_[ts];
  phys_->GetTransform(tl.body, tl.xf);
  const Quat tq{tl.xf.quat[0], tl.xf.quat[1], tl.xf.quat[2], tl.xf.quat[3]};
  const Vec3 tLocal = QuatRotateInv(tq, c.at - tl.xf.pos);

  // ---- THE TWO PATCHES ----------------------------------------------------
  const float sReach = rw * S.scale;
  // A blunt blow's footprint is its BRUISE's (Mob::BluntHit, unarmed
  // overrides included): the coat goes where the skin was struck.
  float twr = rw;
  if (c.kind == CoatHitKind::Blunt) {
    const Tuning::Gore& g = tune.gore;
    const float br = (c.unarmed && g.unarmedBruiseRadius >= 0.0f)
                         ? g.unarmedBruiseRadius
                         : g.bruiseRadius;
    twr = std::max(rw, br * (0.5f + 0.5f * std::clamp(c.power, 0.0f, 1.0f)));
  }
  const float tReach = twr * T.scale;
  const LatticeOcc sOcc = BuildLatticeOcc(S.L);
  const std::vector<CoatCell> sCells =
      CoatContactCells(S.L, sOcc, sLocal * S.scale, sReach, false);
  std::vector<CoatCell> tCells;
  // A CUT puts it on the WOUND WALL (the kerf's survivors, face-adjacent to
  // what it took); a cut that took nothing, a bruise and a bite on the
  // surface patch -- one cell deeper only under skin that split.
  const bool cutWall = c.kind == CoatHitKind::Cut && c.woundCells &&
                       !c.woundCells->empty();
  if (cutWall) {
    tCells = CoatWoundWall(T.L, *c.woundCells, tLocal * T.scale);
  } else {
    const LatticeOcc tOcc = BuildLatticeOcc(T.L);
    tCells = CoatContactCells(T.L, tOcc, tLocal * T.scale, tReach,
                              c.kind == CoatHitKind::Blunt);
  }
  r.strikerTouched = (uint32_t)sCells.size();
  r.targetTouched = (uint32_t)tCells.size();
  if (sCells.empty() || tCells.empty()) {
    lastCoat_ = r;
    return r;
  }

  // ---- DID THE BLOW OPEN ANYTHING? -----------------------------------------
  // Only an opened wound has fluid to give, and only anatomy has any: a
  // garment's "wound" is a hole in the cloth (Mob::WoundFluid would answer
  // with the WEARER's blood for it).
  bool wounded = false;
  if (ts < target->baseLimbs_ && !target->IsBloodless(ts)) {
    if (c.kind == CoatHitKind::Cut) wounded = cutWall;
    else if (c.kind == CoatHitKind::Bite) wounded = c.landed;
    else
      for (const CoatCell& cc : tCells)
        if (BruiseBroken(T.L.Bruise(cc.idx))) { wounded = true; break; }
  }
  uint32_t fluid = 0;
  if (wounded && bleedAmt > 0) {
    fluid = target->WoundFluid(ts);
    if (fluid != 0 && StainTypeOf(fluid) == 0) fluid = target->SmearMatFor(fluid);
  }
  r.wounded = wounded;

  // ---- WHAT EACH SIDE OFFERS, taken BEFORE either side is written ---------
  std::vector<CoatParcel> fromS = CoatOffer(S.L, sCells, frac, maxLevels);
  std::vector<CoatParcel> fromT = CoatOffer(T.L, tCells, frac, maxLevels);
  if (fromS.empty() && fromT.empty() && fluid == 0) {
    lastCoat_ = r;
    return r;
  }

  // Both bricks owned before anything is poked (copy-on-write: a poke on a
  // shared model would repaint every sword of that kind in the world).
  MicroBodySet* micro = microSet_;
  auto own = [&](Mob& m, int slot) -> int {
    MobLimb& l = m.limbs_[slot];
    if (!micro || l.microModel < 0) return -1;
    const int o = MicroBodyOwn(*micro, (uint32_t)l.microModel);
    if (o < 0) return -1;
    l.microModel = o;
    l.carved = true;
    l.flipbookModel = -1;
    return o;
  };
  const int sModel = own(*striker, ss);
  const int tModel = own(*target, ts);

  // ---- STRIKER -> TARGET, then TARGET -> STRIKER (plan A2, A3) -------------
  CoatLay(T.L, tCells, tReach, fromS, micro, tModel);
  CoatSpend(S.L, sCells, fromS, micro, sModel);
  for (const CoatParcel& p : fromS) r.toTarget += p.levels;
  CoatLay(S.L, sCells, sReach, fromT, micro, sModel);
  CoatSpend(T.L, tCells, fromT, micro, tModel);
  for (const CoatParcel& p : fromT) r.toStriker += p.levels;
  // ...and what the wound LEAKS onto what went into it. A smear, not a
  // transfer: it fills clean voxels and its own, and displaces a coat only
  // when heavier (CoatSmear) -- a thick coat on a blade survives one dirty
  // hit and wears off under the next few. StainWoundAs's refusal to make a
  // sword BLEED is untouched: this is a coat ON the blade, not a wound in it.
  if (fluid != 0) {
    r.bled = CoatSmear(S.L, sCells, fluid, bleedAmt, micro, sModel);
    r.bleedMat = fluid;
  }
  if (r.toTarget || r.toStriker || r.bled) {
    striker->coatDirty_ = striker->twinDirty_ = true;
    target->coatDirty_ = target->twinDirty_ = true;
  }
  lastCoat_ = r;
  return r;
}
