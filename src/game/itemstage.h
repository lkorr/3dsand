#pragma once
// ---- THE ITEM STAGE: ONE ITEM, ITS OWN VOXELS, AND A BRUSH (DESIGN.md §7,
//      "The item stage"; docs/PLAN_weapon_coats.md package C) ----------------
//
// The GAME half of the stage: the lattice as a grid you can shoot a ray into,
// which voxels a brush disc covers, and the one function that applies a
// stroke. The PICTURE is ui/item_stage.h, which draws through the same grid,
// so the voxel under the cursor is the voxel the tick coats.
//
// EVERYTHING IS IN LATTICE CELLS: the item's own micro cells (ItemDef::scale
// of them per world voxel), the frame the coat word lives in. Nothing here
// knows where the item is in the world, or whether it is in the world at all:
// a sword in the bag and a sword in a fist are the same lattice (itemcoat.h).
//
// AUTHORITY. The panel never mutates the item. main.cpp turns the cursor into
// a lattice-space ray and hands it to the tick as PlayerSession::itemStroke
// (the portrait brush's shape, session.h); the tick calls ApplyStageStroke,
// which spends the vessel, writes the coats through the body-coat writers
// (AddBodyStain / WashBodyStain, so stainPrecedence decides: water rinses,
// acid displaces oil, whatever the body rule says) and, when the item is in a
// hand or on the body, pushes the result onto the live rig slot
// (PushItemLatticeToLimb).
//
// GENERAL ON PURPOSE: this is the first tool of the item bench the owner
// described (replace micro voxels, repair, transmute). A future tool is
// another function over LatticeGrid + ItemLatticeMut; the picture, the pick
// and the stroke plumbing do not change.

#include <cstdint>
#include <string>
#include <vector>

#include "game/kitref.h"
#include "math3d.h"
#include "sim/voxload.h"

struct ItemDef;
struct ItemInstance;
struct Kit;
struct MaterialDef;
struct ItemLibrary;
class MobSystem;

namespace itemstage {

// A dense index over one lattice: -1 = empty, else the voxel's index in the
// lattice vector. Box = the lattice's own bounds (a fitted armour shell can
// sit outside its authored box, so the box is measured, never assumed).
struct LatticeGrid {
  IVec3 lo{}, dim{};
  std::vector<int32_t> idx;
  void Build(const std::vector<PrefabVoxel>& lat);
  bool Empty() const { return idx.empty(); }
  int At(int x, int y, int z) const {
    x -= lo.x;
    y -= lo.y;
    z -= lo.z;
    if (x < 0 || y < 0 || z < 0 || x >= dim.x || y >= dim.y || z >= dim.z) return -1;
    return idx[((size_t)z * dim.y + y) * dim.x + x];
  }
  // The box's centre and half-diagonal, cells.
  Vec3 Centre() const;
  float Radius() const;
};

// One ray into the grid (lattice cells). `axis`/`sign`: the face it entered
// through (0..2, +-1 along the RAY's opposite, i.e. the outward face normal
// is -sign on that axis); axis -1 = it started inside a voxel.
struct Hit {
  bool hit = false;
  int voxel = -1;
  IVec3 cell{};
  int axis = -1;
  int sign = 0;
  float t = 0.0f;
};
Hit Raycast(const LatticeGrid& g, Vec3 ro, Vec3 rd);

// The voxels a brush disc of `radius` cells covers when it is aimed along
// `rd` through `ro`: PourOnBody's rule at the lattice's own pitch -- every
// voxel within the disc, binned into one-cell columns across the ray, and in
// each column only the face (the nearest cell, and those within 1.5 cells
// behind it). Sorted by lattice index, so the writes are in a fixed order.
// Empty when the ray meets nothing.
void BrushCells(const LatticeGrid& g, const std::vector<PrefabVoxel>& lat,
                Vec3 ro, Vec3 rd, float radius, std::vector<int32_t>& out);

// The item as the stage shows it: the recorded lattice, or (none recorded)
// the authored one materialised clean into `scratch` -- exactly what
// ItemLatticeMut would produce, without writing to the instance.
const std::vector<PrefabVoxel>* ViewLattice(const ItemInstance& it,
                                            const ItemDef& def, int shell,
                                            std::vector<PrefabVoxel>& scratch);

// Does the stage take this item? Weapons and worn pieces with voxels.
bool StageTakes(const ItemDef& def);

// What a shell is called on the stage ("torso", "arm.L"; "" for a held item).
std::string ShellName(const ItemDef& def, int shell);

// ---- the stroke (an INPUT, carried into the tick) ----------------------------
struct Stroke {
  bool active = false;
  KitRef item{};       // the stack the stage is open on
  int shell = 0;       // itemcoat.h: 0 for a held item, the cover index for armour
  Vec3 ro{}, rd{};     // lattice cells, the stage camera's ray through the cursor
  float radius = 2.0f; // lattice cells
  KitRef vessel{};     // the FLASKS row's chosen vessel (UIState::activeVessel)
};

struct StrokeResult {
  bool hit = false;        // the ray met the item (and the vessel could pour)
  uint32_t marked = 0;     // voxels whose coat changed
  uint32_t spent = 0;      // eighths taken from the vessel
  uint16_t mat = 0;        // what was poured
  int pushed = -1;         // PushItemLatticeToLimb's answer (-1 = not on the rig)
  const char* refused = nullptr;   // why nothing happened, when it did not
};

// Per-tick coat a stroke adds where it lands: PourOnBody's +3 (five ticks
// under the brush soak a spot through).
constexpr uint32_t kStrokeAmount = 3u;

// ONE TICK OF A STROKE. `spendMilli` is the stroke's milli-eighth accumulator
// (PlayerSession::itemStageSpendMilli): the drain is PourBrushCellsPerSec of
// the brush's WORLD radius (radius / def.scale), paid only on ticks the ray
// meets the item, exactly the portrait brush's rule. `mobs`/`wearerId` name
// whoever wears the kit, for the capture before and the push after (null /
// 0 = the kit is nobody's rig, e.g. a gate's bare kit).
StrokeResult ApplyStroke(Kit& kit, MobSystem* mobs, uint64_t wearerId,
                         const ItemLibrary& items,
                         const std::vector<MaterialDef>& mats, const Stroke& s,
                         int64_t& spendMilli);

// ---- the readout ------------------------------------------------------------
// Coats on the lattice, heaviest coverage first (the per-limb ledger's shape):
// material, voxels wearing it, summed amount.
struct CoatShare {
  uint16_t mat = 0;
  uint32_t voxels = 0;
  uint32_t amount = 0;
};
std::vector<CoatShare> CoatSummary(const std::vector<PrefabVoxel>& lat);

}  // namespace itemstage
