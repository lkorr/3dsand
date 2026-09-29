// struct_edit.h — ONE STRUCTURE ASSET OPEN FOR EDITING (docs/PLAN_world_editor.md
// §2.2 / P5; DESIGN.md §16.P5).
//
// The in-game editor edits a house's ASSET (assets/structures/<name>.vox +
// .struct.json), never the world: the world is re-derived from the asset
// (a placed structure is worldgen input, structures.h fact 1). This is the
// asset held in memory while it is being edited.
//
// THE HOUSE FRAME. Every coordinate here -- a voxel, a slot's pos, a slot's
// box / hingeLine props -- is relative to the asset's ORIGIN (the ground
// anchor: footprint centre x/z, y = 0 is the FLOOR row, the first row above
// the ground; negative y is the footing that sinks into the pad), on the
// asset's own axes (+Z = the front, unrotated). NOT the .vox's occupied-box
// frame the files use, because that frame MOVES whenever an edit grows or
// shrinks the occupied box -- and every undo entry, every copied selection
// and every line of an edit script would move with it. The house frame is
// the one thing a save cannot change: the placed ref's pos IS the origin, so
// world = pos + turn(yaw, house) for every instance (HouseToWorld below), and
// the files are re-based around it on save.
//
// DIRTY is exact, not a latch: an order-independent hash of (cells, slots)
// against the hash at the last load/save, so undoing back to the saved state
// clears the unsaved marker.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "math3d.h"
#include "world/structures.h"

namespace editor {

using Json = structures::Json;

// Material names in id order (index = id, 0 = "air"), from
// <assetDir>/materials/materials.json -- what a command's "mat": "cobble"
// resolves against, headless or in game.
std::vector<std::string> LoadMaterialNames(const std::string& assetDir);

// House frame <-> world for an instance at (pos, yaw) (yaw a multiple of 90,
// heading 0 = +Z). Equal to structures::Frame::Cell of (house + origin) for
// every asset size -- the editor-struct-edit gate holds the two together.
IVec3 HouseToWorld(IVec3 house, IVec3 pos, int yaw);
IVec3 WorldToHouse(IVec3 world, IVec3 pos, int yaw);
// A heading in the house frame -> the world, and back.
int HouseYawToWorld(int houseYaw, int yaw);

class StructEdit {
 public:
  std::string name;   // "samples/smithy"

  // Read the asset. False + err (names the file). Non-fatal doubts -> warn.
  bool Load(const std::string& assetDir, const std::string& name, std::string& err,
            std::vector<std::string>& warn);
  // A NEW, EMPTY structure (struct.new): no files yet, no voxels, no slots,
  // "generator": null. The house frame's origin is wherever the author puts
  // the ground: y = 0 is the floor row, (0, 0) the footprint centre by
  // convention. The first save writes both files.
  void InitNew(const std::string& name, int voxelsPerMetre);

  // ---- voxels (house frame; 0 = air) -----------------------------------------
  uint16_t Get(IVec3 h) const;
  // Returns the previous material. Records the change for the world preview.
  uint16_t Set(IVec3 h, uint16_t mat);
  size_t Count() const { return cells_.size(); }
  // The occupied box, house frame. False when empty.
  bool Bounds(IVec3& lo, IVec3& hi) const;
  // Every non-air cell (unordered). For the ray pick and the gates.
  const std::unordered_map<uint64_t, uint16_t>& Cells() const { return cells_; }
  static uint64_t Key(IVec3 h);
  static IVec3 Unkey(uint64_t k);

  // ---- slots (house frame: pos, box props {min,max}, hingeLine [x, z]) ------
  const std::vector<structures::Slot>& Slots() const { return slots_; }
  void SetSlots(std::vector<structures::Slot> s);
  int FindSlot(const std::string& name) const;
  // Slots as a JSON array (house frame) and back: the undo snapshot.
  Json SlotsJson() const;
  static bool SlotsFromJson(const Json& j, std::vector<structures::Slot>& out, std::string& err);

  // ---- the files -------------------------------------------------------------
  // The two files as a save would write them: the .vox (one model, trimmed to
  // the occupied box, palette index = material id, the original RGBA chunk
  // carried over) and the .struct.json (origin / size / materials / slots
  // re-based onto the new box, every other field kept in its order,
  // "handEdited": true). False + err: empty, an axis over 256, a material id
  // over 255 (named).
  bool BuildFiles(const std::vector<std::string>& matNames, std::string& vox,
                  std::string& json, std::string& err) const;
  // BuildFiles + write both (tmp + rename) + mark clean. `oldVox`/`oldJson`
  // receive what was on disk before (the undo snapshot).
  bool Save(const std::string& assetDir, const std::vector<std::string>& matNames,
            std::string& err, std::string* oldVox = nullptr, std::string* oldJson = nullptr);
  // The buffer as a structures::Asset (origin, size, slots in the BOX frame):
  // what DeriveChildren needs to draw the slots of a placed instance live.
  structures::Asset AsAsset() const;

  bool Dirty() const { return hash_ != savedHash_; }
  void MarkClean() { savedHash_ = hash_; }
  // What the files on disk hold, as a buffer hash: an undone save puts the
  // old files back and the old hash with them (so Dirty() stays exact).
  uint64_t SavedHash() const { return savedHash_; }
  void SetSavedHash(uint64_t h) { savedHash_ = h; }
  uint64_t Revision() const { return rev_; }
  // Cells changed since the last call (house frame, new material): the world
  // preview of the open instance.
  std::vector<std::pair<IVec3, uint16_t>> TakeChanged();
  IVec3 LoadedOrigin() const { return origin_; }
  IVec3 LoadedSize() const { return size_; }
  const Json& Doc() const { return json_; }

 private:
  static uint64_t CellHash(uint64_t key, uint16_t mat);
  void RehashSlots();
  std::unordered_map<uint64_t, uint16_t> cells_;
  std::vector<structures::Slot> slots_;
  Json json_ = Json::object();
  std::string rgba_;           // the original RGBA chunk (1024 bytes) or ""
  IVec3 origin_{}, size_{};    // box frame of the files as last read/written
  uint64_t cellHash_ = 0, slotHash_ = 0, hash_ = 0, savedHash_ = 0;
  uint64_t rev_ = 0;
  std::vector<std::pair<IVec3, uint16_t>> changed_;
};

// Slot props: transform every coordinate-bearing value ({min, max} boxes and
// hingeLine [x, z]) by +d (box <- house: d = origin; house <- box: -origin).
Json ShiftSlotProps(const Json& props, IVec3 d);

}  // namespace editor
