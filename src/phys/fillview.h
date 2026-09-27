#pragma once
// A VESSEL'S CONTENTS, DRAWN LEVEL (owner, 2026-09-26: "flasks are tinted /
// contain the color of the thing that they contain ... and the volume of it
// represented by the % volume"). Render-only: never hashed, never saved.
//
// WHAT IT LOOKS LIKE IS THE SUBSTANCE'S, NOT THE FLASK'S. The word carries the
// contents' MATERIAL ID, and microbody.wgsl shades a filled cell as that
// material -- its palette, its `emission`, its burn tint and ember flicker,
// through the same functions every other voxel of it goes through. So lava in
// a flask glows because lava glows (materials.json `emission`), and a future
// glowing liquid glows in a flask with no code: there is no per-vessel or
// per-contents switch to keep in step, the material row is the only one.
//
// One view, three holders: the flask in the player's hand, the flask in an
// NPC's hand (both Mob::SetHeldFill) and the flask lying on the ground or in
// flight (DebrisSystem::SetBodyFill). What is in it -- colour, how full, and
// which cells of the model are see-through -- is the item's
// (game/container.h ContainerHeldFillOf); where the surface lies is the body's
// ROTATION, so the word is recomputed every frame from the body's quaternion.
//
// THE SURFACE is a plane normal to WORLD up, placed by VOLUME: the
// see-through cells (the item's first `container.fillSlices` x-slices, base
// up) are ranked by height and the plane goes through the one the fill
// fraction reaches -- a half-full flask shows half its glass whichever way up
// it is, the wide belly fills slowly and the neck fast, and a flask lying on
// its side shows a puddle along its flank. Any fill at all shows its lowest
// cell.
//
// Here, under phys/, and not in game/container.h because DebrisSystem is a
// holder and must not know what an item is.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "math3d.h"

struct FillCell {
  int8_t x, y, z;
};

struct BodyFillView {
  uint16_t mat = 0;     // contents material id (1..4095), 0 = empty
  float frac = 0.0f;    // fill / capacity, 0..1
  int slices = 0;       // see-through x-slices (x < this)
  IVec3 dims{};         // the brick the cells are in (the item's model)
  // The see-through cells in brick coordinates, computed once per item at
  // load (ItemDef::container.fillCells) and shared, so a view outlives an R
  // reload of the item library that would free a raw pointer.
  std::shared_ptr<const std::vector<FillCell>> cells;
  bool On() const {
    return mat != 0 && mat < 4096 && slices > 0 && frac > 0.0f && cells &&
           !cells->empty();
  }
};

// THE WORD (sim/microbody.h MicroBodyInstGpu's fourth word, the one the dye
// rides; microbody.wgsl decodes it and check_invariants.py pins the shift):
//   bits  0..11  contents MATERIAL id (the shader reads its row)
//   bits 12..15  zero
//   bits 16..22  see-through slices (x < this)
//   bit  24      CLEAR -- the dye flag, so a fill word is never read as a dye
//   bits 25..31  the surface height along world up rotated into the brick,
//                0..127 across +-half the brick's diagonal about its centre
// `q` is the body's rotation (x,y,z,w). 0 when there is nothing to draw.
constexpr uint32_t kHeldFillLevelShift = 25;

inline uint32_t BodyFillWord(const BodyFillView& f, const float q[4]) {
  if (!f.On() || f.dims.x <= 0 || f.dims.y <= 0 || f.dims.z <= 0) return 0;
  // World up in the brick's frame: rotate (0,1,0) by the CONJUGATE of q.
  // v' = v + 2w(u x v) + 2 u x (u x v), with u = -q.xyz.
  const Vec3 u{-q[0], -q[1], -q[2]};
  const Vec3 v{0.0f, 1.0f, 0.0f};
  const Vec3 t = u.cross(v) * 2.0f;
  const Vec3 up = v + t * q[3] + u.cross(t);
  const Vec3 mid{f.dims.x * 0.5f, f.dims.y * 0.5f, f.dims.z * 0.5f};
  // Heights of the see-through cells' centres. Bounded: a vessel's glass is a
  // few hundred cells, and past the buffer the rest are simply not ranked.
  float h[1024];
  int n = 0;
  for (const FillCell& c : *f.cells) {
    if (n >= (int)(sizeof h / sizeof h[0])) break;
    const Vec3 p{c.x + 0.5f - mid.x, c.y + 0.5f - mid.y, c.z + 0.5f - mid.z};
    h[n++] = p.dot(up);
  }
  const int k = std::clamp((int)std::lround(f.frac * (float)n), 1, n);
  std::nth_element(h, h + (k - 1), h + n);
  const float thr = h[k - 1];
  // Quantized over +-half the diagonal, ROUNDED UP so the k-th cell is in.
  const float hd = 0.5f * std::sqrt((float)(f.dims.x * f.dims.x +
                                            f.dims.y * f.dims.y +
                                            f.dims.z * f.dims.z));
  const int lvl = std::clamp(
      (int)std::ceil((thr / hd * 0.5f + 0.5f) * 127.0f + 1e-3f), 0, 127);
  return ((uint32_t)f.mat & 0xFFFu) | ((uint32_t)std::min(f.slices, 127) << 16) |
         ((uint32_t)lvl << kHeldFillLevelShift);
}
