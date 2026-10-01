#pragma once
// ---- THE ITEM STAGE'S PICTURE (DESIGN.md §7, "The item stage") ----------------
//
// One item's voxel lattice drawn on the CPU into an RGBA8 buffer, the way the
// alchemy bench draws its table: no GPU pipeline, a texture upload and an
// ImGui image at an integer scale with nearest filtering, so it stays pixel
// art. The lattice is tiny (a sword is a few thousand cells), so an
// orthographic DDA per pixel is cheap -- and the SAME ray is the pick: the
// voxel `Pick` returns for a pixel is the voxel drawn there, and the voxel the
// tick will coat (game/itemstage.h BrushCells starts from the same Raycast).
//
// WHAT IT DRAWS:
//   * per-voxel colour: the art colour when the voxel is painted (a merged
//     art-palette index, MicroBodySet::artColors), else the material's
//     3-variant palette keyed on the cell -- microbody.wgsl's rule; a dyed
//     instance re-coloured by luminance over kDyeRef, as the shader does;
//   * the COAT over it, microbody.wgsl bodyStainCover / bodyStainTint on the
//     CPU: the coat material's stainColor and authored opacity, the
//     render.stain* knobs, a value-noise mottle; plus the coat's glow;
//   * studio light that turns with the camera (a key, a cool fill, a rim),
//     banded so it reads as pixel art, and a one-pixel ink outline plus a
//     darker line where the depth jumps.
//
// Reusable by the future crafting / repair bench: nothing here knows about
// coats being applied, kits or UI state -- a lattice, a view, a buffer.

#include <cstdint>
#include <vector>

#include "game/itemstage.h"
#include "math3d.h"
#include "sim/voxload.h"

struct MaterialDef;

namespace itemstage {

// The panel's orbit. `yaw` turns about the screen's up, `pitch` tips toward
// the viewer; zoom 1 = the item's bounding sphere fills the shorter side.
constexpr float kDefaultYaw = 0.45f;
constexpr float kDefaultPitch = -0.32f;
struct View {
  float yaw = kDefaultYaw, pitch = kDefaultPitch, zoom = 1.0f;
  void Reset() { *this = View{}; }
};

// An orthographic camera over the lattice, in lattice cells. The BASE pose
// lays the item's longest axis across the screen and its thinnest toward the
// viewer (a sword shows its flat), then the orbit turns it.
struct StageCamera {
  Vec3 centre{}, right{1, 0, 0}, up{0, 1, 0}, back{0, 0, 1};
  float halfW = 1.0f, halfH = 1.0f, depth = 1.0f;
  int w = 1, h = 1;
  // The ray through pixel (px, py) -- pixel units, (0,0) the top-left
  // corner, so a pixel's centre is +0.5.
  void Ray(float px, float py, Vec3& ro, Vec3& rd) const;
  // A lattice point -> pixel coordinates.
  void Project(Vec3 p, float& px, float& py) const;
  float PxPerCell() const { return (float)w / (2.0f * halfW); }
};
StageCamera MakeCamera(const LatticeGrid& g, const View& v, int w, int h);

struct Look {
  const std::vector<MaterialDef>* mats = nullptr;
  const std::vector<uint32_t>* artColors = nullptr;   // merged, 0xTTRRGGBB
  uint32_t dye = 0;                                   // game/dye.h, 0 = undyed
  uint32_t scale = 1;                                 // micro cells per world voxel
};

// Draw the lattice. `rgba` is resized to w*h*4; the background is alpha 0.
void Render(const LatticeGrid& g, const std::vector<PrefabVoxel>& lat,
            const StageCamera& cam, const Look& look, std::vector<uint8_t>& rgba);

// WHICH VOXEL IS UNDER THIS PIXEL: the lattice index, or -1.
int Pick(const LatticeGrid& g, const StageCamera& cam, float px, float py);

}  // namespace itemstage
