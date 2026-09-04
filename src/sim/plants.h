#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "sim/rng.h"

// ---- TILE PLANTS, the CPU twin ---------------------------------------------
// plantTileAt() in assets/shaders/common.wgsl is the truth: worldgen paints a
// tile plant's footprint from it and the raymarcher rebuilds the plant from
// it. This is a line-for-line transcription so the --shot harness and the
// `plants` gate can put a fern or a big toadstool exactly where the renderer
// will look for one. check_invariants.py (`plants`) holds the constants below
// equal to the PLANT_* consts in common.wgsl and to the tile/foot/minH/maxH
// each species declares in materials.json.
struct PlantTileCpu {
  bool present = false;
  int cx = 0, cz = 0;   // centre column, world voxels
  int h = 0;            // cells above the base
  uint32_t rnd = 0;
};

inline int PlantFdiv(int a, int b) { return a < 0 ? (a - b + 1) / b : a / b; }

inline PlantTileCpu PlantTileAtCpu(int x, int z, uint32_t seed, uint32_t salt,
                                   int tile, int foot, int minH, int maxH,
                                   uint32_t chancePct) {
  PlantTileCpu t;
  const int tx = PlantFdiv(x, tile);
  const int tz = PlantFdiv(z, tile);
  const uint32_t r = rng::Hash3(seed ^ salt, (uint32_t)tx, (uint32_t)tz);
  t.rnd = r;
  const int half = foot / 2;
  const int span = std::max(tile - 2 * half, 1);
  t.cx = tx * tile + half + (int)((r >> 4u) % (uint32_t)span);
  t.cz = tz * tile + half + (int)((r >> 12u) % (uint32_t)span);
  t.h = minH + (int)((r >> 20u) % (uint32_t)std::max(maxH - minH + 1, 1));
  t.present = (r % 100u) < chancePct;
  return t;
}

// Must match common.wgsl PLANT_*.
constexpr uint32_t kPlantFernSalt = 0xFE21u;
constexpr int kPlantFernTile = 7, kPlantFernFoot = 5, kPlantFernMinH = 3,
              kPlantFernMaxH = 5;
constexpr uint32_t kPlantShroomSalt = 0x5A1Cu;
constexpr int kPlantShroomTile = 7, kPlantShroomFoot = 3, kPlantShroomMinH = 2,
              kPlantShroomMaxH = 3;
