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
struct PrefabVoxel {
  int16_t x, y, z;
  uint16_t material;  // 12-bit material ID (== .vox palette index)
};

struct PrefabModel {
  std::string name;   // scene-graph node name, or "modelN"
  IVec3 size{};       // engine-axis extents of this model
  IVec3 offset{};     // model min corner in PREFAB frame (prefab min = 0)
  std::vector<PrefabVoxel> voxels;
};

struct Prefab {
  std::string name;   // file stem
  IVec3 size{};       // overall engine-axis bounding box
  std::vector<PrefabModel> models;
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
