#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <unordered_map>
#include <utility>

#include "game/item.h"
#include "game/itemcoat.h"
#include "sim/microbody.h"
#include "phys/debris.h"
#include "phys/physics.h"

// ITEMS LYING ON THE GROUND — the other half of "you can pick things up".
//
// WHY THERE IS NO WORLD-ITEM ENTITY. A dropped sword needs to fall, settle,
// roll, burn, dissolve, be blown up and be carved — and DebrisSystem already
// does every one of those for a body made of voxels. A severed held item
// ALREADY takes exactly this path and behaves correctly; inventing a parallel
// "pickup entity" would be a second physics owner for the same shape, and the
// two would drift the first time a rigidbody rule changed.
//
// So a dropped item IS an ordinary debris body. The ONLY thing debris cannot
// carry is IDENTITY — a pile of voxels does not know it used to be a robe —
// and that is the whole of what this registry adds: body handle -> item name.
//
// BY NAME, NOT BY INDEX. ItemLibrary indices are file-order and die on every R
// hot-reload (item.h's index hazard), and a sword lying in a field can outlive
// several of those.
//
// THE REGISTRY MUST NOT OUTLIVE THE BODY. Debris is culled, burned away and
// blown up, and an entry pointing at a freed handle would eventually be
// re-matched against a NEW body that reused it — "I picked up a rock and got a
// sword". DebrisSystem calls OnBodyGone for every body it releases, which is
// the one seam that keeps the two in step. A robe that burns up on the ground
// is GONE, and that is correct.
struct WorldItem : ItemInstance {
  uint64_t body = 0;
  // The ItemInstance half (game/iteminstance.h) is the thing's IDENTITY — the
  // facts a pile of voxels cannot reproduce: its name; its DYE (the art a dye
  // colours is a neutral greyscale weave, so a red tunic whose dye the
  // registry forgot is a grey tunic when you pick it up again); what a vessel
  // HOLDS (a corked flask of blood lying in the grass is not recoverable from
  // its glass); and what a worn piece has BEEN THROUGH (the per-shell damage
  // its wearer's shells carried when it came off — the body on the ground is
  // only its largest panel). `count` is always 1: a ground body is one object.
  //
  // It is the whole of what a remote pickup hands the asking machine
  // (net::ItemGrant embeds the same struct) and of what a pickup puts back in
  // a slot, which is the point: before W2-M a grant carried name, dye and a
  // "damage" word nothing ever wrote, and a flask picked up over the network
  // arrived empty.

  // The lattice as it actually is, when the thing on the ground is DAMAGED.
  // Empty means "as authored", which is the overwhelmingly common case and
  // costs nothing to store. Only the save format reads this; the live body
  // already holds its own voxels.
  std::vector<DebrisVoxel> voxels;
};

class WorldItems {
 public:
  // Register `body` as the item `it` (one of it: the count is forced to 1).
  void Add(uint64_t body, const ItemInstance& it) {
    if (!body || it.name.empty()) return;
    Remove(body);   // a reused handle must not resolve to the old item
    WorldItem w;
    static_cast<ItemInstance&>(w) = it.One();
    w.body = body;
    items_.push_back(std::move(w));
  }
  const WorldItem* Find(uint64_t body) const {
    for (const WorldItem& w : items_)
      if (w.body == body) return &w;
    return nullptr;
  }
  WorldItem* FindMut(uint64_t body) {
    for (WorldItem& w : items_)
      if (w.body == body) return &w;
    return nullptr;
  }
  bool Remove(uint64_t body) {
    for (size_t i = 0; i < items_.size(); i++)
      if (items_[i].body == body) {
        items_.erase(items_.begin() + i);
        return true;
      }
    return false;
  }
  // DebrisSystem's release seam. Deliberately the same call as Remove: a body
  // the physics side has let go of is not a thing you can pick up.
  void OnBodyGone(uint64_t body) { Remove(body); }
  void Clear() { items_.clear(); }

  // ---- THE DEBRIS SYSTEM'S VIEW OF THIS REGISTRY (M9.4-C) -----------------
  //
  // A ghost item on the far machine is an ordinary ghost body, and E on it
  // sends an ItemTake to whoever owns it. The owner has to answer "which item
  // is that, and take it" -- two questions this registry alone can answer and
  // which DebrisSystem, a layer below, must not learn to answer itself.
  //
  // So they are handed DOWN as callbacks rather than the layering being
  // inverted. Bind once, next to SetOnBodyGone:
  //
  //   debris.SetItemLookupFn(reg.LookupFn());
  //   debris.SetItemTakeFn([&](uint64_t h) { return debris.DestroyBody(h); });
  //
  // (the take function is the CALLER's, because "picked up" means putting the
  // thing in somebody's inventory and only the session knows whose.)
  std::function<bool(uint64_t, ItemInstance&)> LookupFn() const {
    return [this](uint64_t body, ItemInstance& out) {
      const WorldItem* w = Find(body);
      if (!w) return false;
      out = static_cast<const ItemInstance&>(*w);
      return true;
    };
  }
  const std::vector<WorldItem>& All() const { return items_; }
  std::vector<WorldItem>& All() { return items_; }
  size_t Count() const { return items_.size(); }

 private:
  std::vector<WorldItem> items_;
};

// ---- dropping ---------------------------------------------------------------
//
// WHICH VOXELS. A held item has one model and there is nothing to decide. A
// WORN piece does not: a robe is a torso panel, two sleeves and a skirt, each
// authored in the frame of a different limb, so there is no single lattice
// that is "the robe". Merging them at their authored offsets would produce a
// person-shaped shell hanging in the air, which is worse than either
// alternative.
//
// So a worn piece drops as its LARGEST panel — the torso for a robe, the cowl
// for a hood — which is what the piece reads as at a glance. A crumpled-
// garment ground model is CONTENT (another entry in the .vox, a "ground" cover
// row) and belongs there rather than in a heuristic here.
inline const std::vector<PrefabVoxel>* ItemGroundVoxels(const ItemDef& d,
                                                        uint32_t& outScale) {
  outScale = d.scale ? d.scale : 1u;
  if (!d.cover.empty()) {
    const ItemCover* best = &d.cover[0];
    for (const ItemCover& cv : d.cover)
      if (cv.voxels.size() > best->voxels.size()) best = &cv;
    return &best->voxels;
  }
  return d.voxels.empty() ? nullptr : &d.voxels;
}

// Put `def` on the ground at `at` (world voxels, the body's min corner) moving
// at `vel`. Returns the body handle, or 0 if physics refused.
//
// Adopted by DebrisSystem rather than held here, so from this line on the item
// is an ordinary rigidbody: it falls, settles, catches fire, dissolves and can
// be blown apart, with none of that written twice.
// `lattice` overrides the item's authored geometry — the SAVE path uses it to
// put a burnt robe back burnt (persist.cpp 'ITMS'). Null is the ordinary case:
// a thing you just dropped is whatever the library says it is.
// `inst` is WHICH one of `def` this is (its dye, what it holds, what it has
// been through) and is registered whole; its name is `def`'s.
inline uint64_t DropItemToWorld(const ItemDef& def, const ItemInstance& inst,
                                Vec3 at, Vec3 vel, Physics& phys,
                                DebrisSystem& debris, MicroBodySet* micro,
                                WorldItems& reg,
                                const std::vector<PrefabVoxel>* lattice =
                                    nullptr) {
  const uint32_t dye = inst.dye;
  uint32_t scale = 1;
  const std::vector<PrefabVoxel>* authored = ItemGroundVoxels(def, scale);
  const std::vector<PrefabVoxel>* src =
      (lattice && !lattice->empty()) ? lattice : authored;
  if (!src || src->empty()) return 0;
  std::vector<DebrisVoxel> vox;
  vox.reserve(src->size());
  for (const PrefabVoxel& v : *src) {
    const uint32_t variant = ((uint32_t)(v.x * 7 + v.y * 13 + v.z * 29)) % 3u;
    vox.push_back({(int8_t)v.x, (int8_t)v.y, (int8_t)v.z, v.color,
                   (uint16_t)((v.material & 0xFFFu) | (variant << 12))});
  }
  // ---- THE COAT GOES DOWN WITH IT (2026-10-01) ------------------------------
  // The instance's recorded lattice for the panel the ground body is built
  // from (itemcoat.h ItemGroundShell) carries the coat words; they land on the
  // body voxel for voxel where positions coincide, and the brick below is an
  // OWNED copy poked with them -- the shared item brick is every sword's.
  // A venom-coated sword dropped in the grass is still drawn venom-coated.
  std::vector<std::pair<size_t, uint16_t>> coats;
  if (const std::vector<PrefabVoxel>* rec =
          ItemLatticeIfAny(inst, ItemGroundShell(def))) {
    std::unordered_map<uint64_t, uint16_t> at;
    auto key = [](int x, int y, int z) {
      return ((uint64_t)(uint16_t)(int16_t)x << 32) |
             ((uint64_t)(uint16_t)(int16_t)y << 16) |
             (uint64_t)(uint16_t)(int16_t)z;
    };
    for (const PrefabVoxel& v : *rec)
      if (v.stain) at.emplace(key(v.x, v.y, v.z), v.stain);
    if (!at.empty())
      for (size_t i = 0; i < vox.size(); i++) {
        const auto f = at.find(key(vox[i].x, vox[i].y, vox[i].z));
        if (f == at.end()) continue;
        vox[i].stain = f->second;
        coats.push_back({i, f->second});
      }
  }
  BodyTransform xf{};
  xf.pos = at;
  xf.quat[3] = 1;
  const float pitch = 1.0f / (float)std::max(1u, scale);
  // allowKinematic=false: a dropped item is DYNAMIC from the first frame. It
  // has left the rig and nothing is animating it any more.
  const uint64_t body =
      phys.CreateDebrisBodyXf(vox, xf, debris.DensityOf(), false, pitch);
  if (!body) return 0;
  phys.SetBodyVelocity(body, vel);
  // Thrown from the eye — which is INSIDE the player's capsule proxy (3
  // voxels of half-width against a 2-voxel throw). A loose role clears the
  // player before it is ordinary debris, or the proxy shoves the player away
  // from their own drop. (A THROW re-sets it Thrown, with its thrower,
  // straight after: session.cpp.)
  phys.SetBodyRole(body, Physics::BodyRole::Debris);
  // The micro brick travels with it so a dropped item keeps its detail — the
  // same argument a severed limb makes, and the reason a dropped sword does
  // not visibly coarsen the moment it leaves your hand.
  // A DAMAGED item does not get the shared brick: it is not the authored
  // shape any more, and every other instance of the item is drawn through
  // that brick. It renders through the coarse path instead, which is the same
  // graceful degradation a limb takes when the micro pool is full.
  MicroBodyRef mref{};
  if (lattice && !lattice->empty()) {
    // no brick
  } else if (micro && !def.cover.empty()) {
    const ItemCover* best = &def.cover[0];
    for (const ItemCover& cv : def.cover)
      if (cv.voxels.size() > best->voxels.size()) best = &cv;
    if (best->microModel >= 0)
      mref = MicroBodyRef{(uint32_t)best->microModel, scale, dye};
  } else if (micro && def.microModel >= 0) {
    mref = MicroBodyRef{(uint32_t)def.microModel, scale, dye};
  }
  // A COATED item draws through its OWN copy of the brick (MicroBodyOwn: the
  // body frees it with the body), with the coat poked in.
  if (micro && mref.Valid() && !coats.empty()) {
    const int own = MicroBodyOwn(*micro, mref.model);
    if (own >= 0) {
      mref.model = (uint32_t)own;
      for (const auto& c : coats)
        MicroBodyPokeStain(*micro, (uint32_t)own, vox[c.first].x,
                           vox[c.first].y, vox[c.first].z, c.second);
    }
  }
  debris.AdoptBody(body, vox, xf, mref, scale);
  debris.MarkItemBody(body);
  // The colour goes in BOTH places on purpose: on the ref so the thing on the
  // ground LOOKS right, and on the registry entry so picking it up gives you
  // back the garment you dropped. Neither is derivable from the other — the
  // body's word is render state a reload re-packs, the registry's is identity.
  ItemInstance it = inst;
  it.name = def.name;
  reg.Add(body, it);
  return body;
}

// ---- THE COAT COMES BACK UP WITH IT (2026-10-01) ----------------------------
// The ground body's coats -> the instance (`it`, the registry entry or a copy
// of it), voxel for voxel: called before a pickup hands the instance over and
// before a save writes it, so a dropped sword that lay in a puddle of oil
// comes back oiled and one that dried on the ground comes back dry. Returns
// the recorded coat words that changed; 0 for an unknown body.
inline uint32_t SyncGroundCoat(ItemInstance& it, uint64_t body,
                               const ItemDef& def, const DebrisSystem& debris) {
  std::vector<PrefabVoxel> lat;
  uint32_t scale = 1;
  if (!body || !debris.BodyLatticeOf(body, lat, scale)) return 0;
  return CaptureGroundCoat(it, def, lat);
}
