#include "game/itemcoat.h"

#include "game/equipment.h"
#include "game/mob.h"

namespace {

std::vector<PrefabVoxel>& NoLattice() {
  static std::vector<PrefabVoxel> none;
  none.clear();   // a caller may have written into it; it is always empty
  return none;
}

// Voxels the item had when authored, for the completeness test.
size_t AuthoredCount(const ItemDef& def, int shell) {
  const std::vector<PrefabVoxel>* a = ItemAuthoredLattice(def, shell);
  return a ? a->size() : 0u;
}

}  // namespace

int ItemLatticeCount(const ItemDef& def) {
  if (!def.cover.empty()) return (int)def.cover.size();
  return def.voxels.empty() ? 0 : 1;
}

const std::vector<PrefabVoxel>* ItemAuthoredLattice(const ItemDef& def,
                                                    int shell) {
  if (shell < 0 || shell >= ItemLatticeCount(def)) return nullptr;
  return def.cover.empty() ? &def.voxels : &def.cover[(size_t)shell].voxels;
}

bool ItemLatticeDims(const ItemDef& def, int& sx, int& sy, int& sz,
                     int shell) {
  sx = sy = sz = 0;
  if (shell < 0 || shell >= ItemLatticeCount(def)) return false;
  const IVec3 s = def.cover.empty() ? def.size : def.cover[(size_t)shell].size;
  sx = s.x;
  sy = s.y;
  sz = s.z;
  return sx > 0 && sy > 0 && sz > 0;
}

std::vector<PrefabVoxel>& ItemLatticeMut(ItemInstance& it, const ItemDef& def,
                                         int shell) {
  const int n = ItemLatticeCount(def);
  if (shell < 0 || shell >= n) return NoLattice();
  if (it.damage.shells.size() < (size_t)n) it.damage.shells.resize((size_t)n);
  WornShellDamage& sh = it.damage.shells[(size_t)shell];
  if (sh.lattice.empty()) {
    const std::vector<PrefabVoxel>* a = ItemAuthoredLattice(def, shell);
    if (a) {
      sh.lattice.reserve(a->size());
      const bool held = def.cover.empty();
      for (const PrefabVoxel& v : *a) {
        PrefabVoxel p = v;
        p.stain = 0;
        p.bruise = 0;
        // The palette variant Mob::EquipItem gives a held item's voxels, so
        // this is exactly what CaptureHeld records of the untouched item.
        if (held) {
          const uint32_t variant =
              ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
          p.material = (uint16_t)((v.material & 0xFFFu) | (variant << 12));
        }
        sh.lattice.push_back(p);
      }
      if (sh.atSpawn == 0) sh.atSpawn = (uint32_t)a->size();
      if (sh.live == 0) sh.live = (uint32_t)a->size();
    }
  }
  return sh.lattice;
}

const std::vector<PrefabVoxel>* ItemLatticeIfAny(const ItemInstance& it,
                                                 int shell) {
  if (shell < 0 || (size_t)shell >= it.damage.shells.size()) return nullptr;
  const std::vector<PrefabVoxel>& l = it.damage.shells[(size_t)shell].lattice;
  return l.empty() ? nullptr : &l;
}

bool ItemLatticeSettle(ItemInstance& it, const ItemDef& def) {
  for (size_t i = 0; i < it.damage.shells.size(); i++) {
    WornShellDamage& sh = it.damage.shells[i];
    if (sh.lattice.empty() || sh.hp >= 0.0f) continue;
    if (sh.lattice.size() != AuthoredCount(def, (int)i)) continue;
    bool clean = true;
    for (const PrefabVoxel& v : sh.lattice)
      if (v.stain) { clean = false; break; }
    if (clean) sh.lattice.clear();
  }
  if (it.damage.Empty()) {
    it.damage.Clear();
    return true;
  }
  return false;
}

bool Mob::KitShellLattice(int equipSlot, int shell,
                          std::vector<PrefabVoxel>& out) const {
  out.clear();
  if (equipSlot < 0 || equipSlot >= kEquipSlotCount || rigReleased_) return false;
  const ItemStack& st = kit_.equip.slots[equipSlot];
  if (st.Empty()) return false;
  Hand h = Hand::Right;
  if (EquipSlotIsHand(equipSlot, &h)) {
    const HeldHand& hh = held_[HandIndex(h)];
    if (shell != 0 || hh.slot < 0 || hh.borrowed || hh.kitStale || hh.item != st.name)
      return false;
    LatticeOfSlot(hh.slot, out);
    return !out.empty();
  }
  for (const WornPiece& p : worn_) {
    if (p.equipSlot != equipSlot || p.item != st.name) continue;
    for (size_t k = 0; k < p.slots.size() && k < p.cover.size(); k++)
      if (p.cover[k] == shell) {
        LatticeOfSlot(p.slots[k], out);
        return !out.empty();
      }
    return false;
  }
  return false;
}

int PushItemLatticeToLimb(Mob& wearer, const KitRef& ref) {
  if (ref.space != KitSpace::Equip) return -1;
  return wearer.ApplyKitCoats(ref.index);
}

int PushItemLatticeToLimb(MobSystem& mobs, uint64_t mobId, const KitRef& ref) {
  Mob* m = mobs.FindCreature(mobId);
  return m ? PushItemLatticeToLimb(*m, ref) : -1;
}

bool CaptureLimbToItem(Mob& wearer, const KitRef& ref) {
  if (ref.space != KitSpace::Equip) return false;
  return wearer.KitFlushWorn(ref.index);
}

bool CaptureLimbToItem(MobSystem& mobs, uint64_t mobId, const KitRef& ref) {
  Mob* m = mobs.FindCreature(mobId);
  return m ? CaptureLimbToItem(*m, ref) : false;
}
