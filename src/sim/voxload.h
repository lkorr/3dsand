#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"

// MagicaVoxel .vox prefab loader (PLAN_voxel_art_and_mobs.md §A1).
//
// Palette convention: palette index == 12-bit material ID (index 1 = stone,
// matching materials.json order). Modeling is "painting with materials";
// RGB colour-matching back to materials is deliberately not supported (lossy).
// assets/prefabs/palette.png (scripts/gen_palette.py) shows the right colours
// while editing.
//
// Coordinates: MagicaVoxel is Z-up, the engine is Y-up. The loader converts
// once, chirality-preserving (engine.x = vox.x, engine.y = vox.z,
// engine.z = -vox.y, then rebased so the prefab min corner is 0). Multi-model
// scenes (mob limbs) keep their relative placement through the scene graph
// (nTRN/nGRP/nSHP, frame 0, translation + 90-degree rotations).

// One voxel in MODEL-local engine coords (min corner 0). int16, deliberately
// wider than DebrisVoxel's int8: prefabs may be large; only mob limbs must fit
// in a rigidbody's byte range.
//
// `material` and `color` are INDEPENDENT facts about the voxel, and keeping
// them apart is the whole point: a creature is "meat" everywhere — that is
// what the sim reacts to, what a severed limb becomes when its voxels land
// back in the grid — while its skin is painted per voxel. Art colour never
// reaches the world grid, so it can never affect the hash (rule 1).
//
// Free: the struct was already 8 bytes with `material`'s top nibble and two
// bytes of tail padding unused, so `color` costs nothing.
struct PrefabVoxel {
  int16_t x, y, z;
  uint16_t material;  // 12-bit material ID (== .vox palette index)
  uint8_t color = 0;  // art palette slot, 0 = use the material's own colour
};

struct PrefabModel {
  std::string name;   // scene-graph node name, or "modelN"
  IVec3 size{};       // engine-axis extents of this model
  IVec3 offset{};     // model min corner in PREFAB frame (prefab min = 0)
  // This model's min corner in the RAW ENGINE SCENE frame — exactly what was
  // subtracted from every cell to rebase `voxels` onto zero.
  //
  // Needed by anything that converts an AUTHORED BOX out of a sidecar (an
  // item's hilt or cutting edge) and has to land it on this art. The
  // scene -> engine map is (x, z, -y), so the negated axis puts a model at
  // NEGATIVE engine coordinates; the loader then rebases the voxels but
  // nothing rebases the sidecar's numbers, leaving the two a full model-depth
  // apart. Subtracting this puts them in one frame by construction.
  //
  // `offset` cannot stand in for it: that is measured AFTER the rebase and is
  // (0,0,0) for a single-model file. Nor can it be reconstructed from `size` —
  // that only matches while the model sits at the scene origin unrotated, and
  // a scene-graph translate or a rotated instance breaks it silently.
  //
  // A CELL INDEX, not a face. On the negated axis those differ by one cell, so
  // a caller converting a continuous box has to bias for it — see the note at
  // the one caller (game/melee.cpp), which is also where leaving this
  // un-recorded put a sword's hilt box on the far side of its own origin from
  // the blade.
  IVec3 sceneMin{};
  std::vector<PrefabVoxel> voxels;
};

// Art colours occupy the TOP of the .vox palette, growing downward from 255,
// while material IDs occupy the bottom. Mirrors ART_BASE/ART_TOP in
// assets/editor/vox.js — the two must agree or a painted model loads with its
// colours read as material IDs. See the art-colour note on PrefabVoxel.
constexpr int kArtPaletteBase = 128;
constexpr int kArtPaletteTop = 255;
constexpr int kArtPaletteSlots = kArtPaletteTop - kArtPaletteBase + 1;
inline bool IsArtPaletteIndex(int i) {
  return i >= kArtPaletteBase && i <= kArtPaletteTop;
}
// The scene-graph name suffix marking a model as another model's colour layer.
inline constexpr const char* kArtLayerSuffix = ".col";

struct Prefab {
  std::string name;   // file stem
  IVec3 size{};       // overall engine-axis bounding box
  std::vector<PrefabModel> models;
  // RGB for art palette slots, indexed by (slot - kArtPaletteBase); empty when
  // the file painted nothing. Packed 0x00RRGGBB.
  std::vector<uint32_t> artColors;
};

// Parse from memory (unit-testable without a GPU). materialCount gates the
// palette-index warnings appended to `warnings` (one line per bad index, the
// voxels are kept — hot-adding the material later fixes them in place).
bool LoadVoxFromMemory(const uint8_t* data, size_t len, size_t materialCount,
                       Prefab& out, std::string& errors, std::string& warnings);

// File wrapper; prefab name = file stem.
bool LoadVoxFile(const std::string& path, size_t materialCount, Prefab& out,
                 std::string& errors, std::string& warnings);

// All .vox files in a directory, sorted by filename (stable hot-reload order).
// Returns false only if the directory exists but nothing loaded cleanly.
bool LoadPrefabDir(const std::string& dir, size_t materialCount,
                   std::vector<Prefab>& out, std::string& log);

// Block-replicate every model's voxel grid by `u` on each axis (DESIGN.md §3b).
//
// A .vox has no intrinsic size — it is a lattice, and its physical size comes
// from the `artVoxelsPerMetre` its sidecar declares. When that is COARSER than
// the world (a 10 vox/m asset in a 20 vox/m world) there is no integer
// art-per-world-voxel scale, and the art would have to be redrawn to keep its
// metre size. This buys that size instead: the model is the shape it was,
// sampled u times more finely, so the silhouette is bit-identical and only the
// cell count changes (u^3 of them).
//
// It ADDS NO DETAIL and is not meant to. Re-authoring at a higher
// artVoxelsPerMetre later removes the upsample with no code change.
//
// Sizes, offsets and `sceneMin` all move with the lattice — sceneMin especially,
// since game/melee.cpp converts authored boxes through it and leaving it behind
// would land a sword's hilt a model-depth from its blade.
//
// The reverse (art finer than the world) is deliberately absent: downsampling
// is lossy and would need a deterministic tie-break to stay rule-1 clean, and
// it cannot arise while every asset is drawn at or above the coarsest voxel
// size in use.
void UpsamplePrefab(Prefab& p, uint32_t u);
