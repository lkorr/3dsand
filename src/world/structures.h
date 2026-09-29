// structures.h — STRUCTURE INSTANCES: a `structure` ref places a building
// blueprint in the world (docs/PLAN_world_editor.md §2.2 / P4; DESIGN.md §9f).
//
// P2 authored the FORMAT (assets/structures/<name>.vox + <name>.struct.json,
// DESIGN.md §9e). This file is the PLACEMENT half:
//
//   { "id": "harrowby/smithy", "kind": "structure", "base": "samples/smithy",
//     "pos": [x, y, z], "yaw": 90, "props": { "padMargin": 12 } }
//
// THE FOUR FACTS THIS FILE IS BUILT AROUND
//
//   1. A PLACED STRUCTURE IS WORLDGEN INPUT, NOT A MUTATION. LoadWorldMap reads
//      the map's `structure` refs straight from the group files (ReadPlacements
//      below) and turns each into a stamp SITE (worldmap.h kSiteStamp, the
//      authored-.vox path P5 of the map overhaul built). So a house appears at
//      any distance (the far cascades run the same overlay), is not saved, and
//      regenerates identically. Player damage to it is an ordinary chunk edit.
//   2. THE REF'S pos IS THE ASSET'S `origin`, EXACTLY. origin = [footprint
//      centre x, GRADE, footprint centre z] in the .vox's occupied-box frame
//      (P2). pos.y is the floor height the AUTHOR chose: the pad is levelled so
//      the ground's top voxel is pos.y - 1 (not a height recomputed from the
//      terrain at the centre), and the rows below GRADE (the footing) sink into
//      it by `origin.y` rows (kS_Sink).
//   3. YAW IS A QUARTER TURN OR NOTHING. 0/90/180/270 (any multiple of 90);
//      anything else is refused with a warning naming the ref, and the house
//      is not placed. Heading 0 = +Z (the asset's front), 90 = +X.
//   4. SLOTS ARE DERIVED CHILD REFS. Each slot of the asset becomes the ref
//      `<instance id>/<slot name>` with the slot's kind, local -> world
//      transformed (pos, yaw, and the known box / hinge / link props). They
//      are never written into the group file; RefStore derives them at load
//      and re-derives on every edit and on Reload (refs.h RefKind::derive).
//      Kinds this build does not know yet (door/container/bed before P6,
//      waynode before P7) stay inert without a warning each.
//
// ORDER OF LOADING (the refs <-> worldgen question). Both sides read the SAME
// group files, independently, so neither waits on the other: the worldgen map
// (boot / F7 / ReloadEnvironment) parses the `structure` lines itself before any
// RefStore exists, and the RefStore parses the whole file for activation and
// children. They meet again only on an EDIT: the structure kind's activate
// hook compares the ref against the site table the GPU has (SiteInSync) and,
// when they differ, raises RequestReapply(); the frame loop then runs
// sandvox::ApplyStructureChanges between ticks (support.h), which reloads the
// environment, diffs old vs new sites and regenerates only the chunks the
// changed houses touch.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/voxload.h"
#include "world/refs.h"

namespace worldmap { struct StructurePlacement; }

namespace structures {

using Json = refs::Json;

// One slot of an asset, in its LOCAL frame (the .vox's occupied box).
struct Slot {
  std::string name, kind;
  IVec3 pos{};
  int yaw = 0;
  Json props = Json::object();
};

// assets/structures/<name>.struct.json (+ .vox when `withVoxels`).
struct Asset {
  std::string name;              // "samples/smithy"
  IVec3 origin{};                // local, the ground anchor (GRADE row)
  IVec3 size{};                  // occupied box (from the .vox when loaded)
  int voxelsPerMetre = 0;
  std::vector<Slot> slots;
  bool handEdited = false;
  // With voxels: one model, rebased to the occupied box, materials remapped
  // to the LIVE materials.json by name.
  Prefab prefab;
  bool hasVoxels = false;
  uint32_t contentHash = 0;      // FNV over both files (what a reload diffs)
};

// Load `<assetDir>/structures/<name>`. False + `err` when the json is missing
// or broken (or the .vox, with `withVoxels`). Non-fatal doubts (a material
// renumbered, a size that disagrees with the .vox) go to `warn`, each naming
// the file and the field.
bool LoadAsset(const std::string& assetDir, const std::string& name, bool withVoxels,
               Asset& out, std::string& err, std::vector<std::string>& warn);
// Every structure under <assetDir>/structures (one folder deep, "samples/x"),
// sorted: the References page's picker.
std::vector<std::string> ListAssets(const std::string& assetDir);
// Does <assetDir>/structures/<name>.struct.json exist?
bool AssetExists(const std::string& assetDir, const std::string& name);

// ---- the transform ------------------------------------------------------------
bool ValidYaw(int yaw);
// yaw (multiple of 90) -> the packer's quarter-turn index (worldmap RotXZ), such
// that local +Z (the front) faces heading `yaw`.
int YawToRot(int yaw);

// Where one asset lands for one (pos, yaw). Every world coordinate a placed
// structure has is computed HERE, so the stamp, the child refs and the gates
// cannot disagree.
struct Frame {
  int rot = 0;
  int nx0 = 0, ny = 0, nz0 = 0;   // local box
  int nx = 0, nz = 0;             // rotated box
  IVec3 origin{};                 // local origin
  IVec3 pos{};                    // the ref's pos (== world origin)
  int yaw = 0;
  int siteX = 0, siteZ = 0;       // the stamp site's centre (worldgen centres nx/2 on it)
  int sink = 0;                   // rows below GRADE (== origin.y)
  // Local cell -> world cell.
  IVec3 Cell(IVec3 l) const;
  // Local cell-CORNER (x, z) (0..nx0, 0..nz0) -> world corner.
  void Corner(int cx, int cz, int* wx, int* wz) const;
  int Yaw(int localYaw) const;    // heading, 0..359
  // The world box the asset's voxels occupy, inclusive.
  void Box(IVec3& lo, IVec3& hi) const;
};
Frame MakeFrame(const Asset& a, IVec3 pos, int yaw);

// The slots of `a` placed as `instance` -> child refs (id `<instance>/<slot>`,
// derivedFrom = instance id). Box props (`leaf`, `box`: {min, max}) and
// `hingeLine` are transformed to world coordinates, `links` to child ids;
// `slot` and `structure` props name where the child came from.
void DeriveChildren(const Asset& a, const refs::Ref& instance, std::vector<refs::Ref>& out,
                    std::vector<std::string>* problems = nullptr);

// ---- P2 slot -> P6 kind (the ONE place the two vocabularies meet) -------------
// P2's door slot says: a leaf box, a hinge LINE (cell-corner x/z on the leaf's
// inner face), the wall's outward heading (the slot yaw) and `opens: in|out`.
// P6's door (world/refs_doors.h) says: pos = the hinge-side bottom cell of the
// leaf's FRONT layer (the face it swings toward), yaw = the heading it swings
// toward, width/height/thickness (thickness behind the front face), hinge
// left|right in P6's own frame (DoorGeometry: along = HeadingAxis(yaw + 90)
// for "left"). The translation is GEOMETRIC, so P2's and P6's differing
// "left/right" conventions never meet: swing heading = outward + 180 for
// `in`; thickness = the leaf's extent along it; the hinge END is whichever end
// of the leaf's width the hinge line sits on; then P6's `hinge` is the word
// whose `along` points from that end across the leaf. False + `why` for a leaf
// that is not an axis-aligned slab. Pure; `worldLeaf*`, `hingeX/Z` in world.
bool DoorFromSlot(IVec3 leafLo, IVec3 leafHi, int hingeX, int hingeZ, int swingYaw,
                  IVec3& pos, std::string& hinge, int& width, int& height, int& thickness,
                  std::string* why);
// P2 bed slot (pos = first air row over the mattress centre, yaw = foot -> head,
// box = the frame) -> P6 bed (pos = the mattress cell where the head lies,
// yaw = head -> foot, props.length = cells head to foot). The frame box's
// head-end layer is the headboard, so the head cell is one inside it.
void BedFromSlot(IVec3 slotPos, int slotYaw, IVec3 boxLo, IVec3 boxHi, IVec3& pos, int& yaw,
                 int& length);

// ---- the map side -------------------------------------------------------------
// Every `structure` ref in <assetDir>/worldmap/<map>/refs/*.json (the SAME
// parse RefStore uses), id order. Problems -> `warn`. Called by LoadWorldMap.
void ReadPlacements(const std::string& assetDir, const std::string& mapName,
                    std::vector<worldmap::StructurePlacement>& out,
                    std::vector<std::string>& warn);

// ---- live re-apply --------------------------------------------------------------
// Is the GPU's site table in step with this ref (a site of this id, same base,
// pos and yaw)? False = an edit has not reached worldgen yet.
bool SiteInSync(const refs::Ref& r);
// Is there a site of this id in the current map at all?
bool SiteExists(const std::string& id);
// Ask the frame loop to reload the environment and regenerate what changed
// (sandvox::ApplyStructureChanges). Cheap; idempotent until taken.
void RequestReapply(const std::string& why);
bool TakeReapply(std::string* why = nullptr);
// structure.reload(name): an ASSET changed on disk (P5's save calls this).
// Re-derives the children of every instance of it in `store` (may be null) and
// requests the re-apply; the diff finds the changed voxels by itself.
void Reload(refs::RefStore* store, const std::string& name);

// Which maps place `name` (the Structures page's "used by"): "map: id" rows.
std::vector<std::string> UsedBy(const std::string& assetDir, const std::string& name);

// Register kind `structure` (refs_kinds.cpp RegisterAllKinds).
void RegisterStructureKinds();

}  // namespace structures
