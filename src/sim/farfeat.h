#pragma once
// ---- THE FAR FEATURE PLANE (LOD-seam package F, 2026-09-28; DESIGN.md §9) ---
// A second plane of the far surface map (world.h kFarMap*), for the refine
// levels only: one u32 per SUB-COLUMN recording the thin thing standing on its
// ground top — a cactus stalk, a grass tuft, a flower — which the cascade's
// centre-sampled cells cannot hold (a 1-voxel stalk survives level 1 with
// probability 1/4 and no MICRO plant survives at all). A MICRO plant's word is
// drawn as its blade card (raymarch.wgsl farFeatMarch); a SOLID stalk's word
// (kFarFeatSolid) is bookkeeping for fardown and the far-surface gate -- the
// stalk itself is drawn by the surface map, whose entry the fill raises to it.
//
// It lives in the SAME buffer as the map, after it, so it costs no binding:
// word = kFarMapWords + kFarMapWord(level, mx, mz), levels 1..kFarFeatLevels.
// The bit layout is worldgen.wgsl's FAR_FEAT_* block (the writer; raymarch.wgsl
// reads the same bits). Kept out of world.h on purpose: world.h is included by
// nearly every TU, and only world.cpp (the allocation) and the far-surface
// gate need this.
#include <cstdint>

#include "sim/world.h"

// worldgen.wgsl FAR_FEAT_LEVELS and raymarch.wgsl's copy must agree.
constexpr uint32_t kFarFeatLevels = 2;
constexpr uint64_t kFarFeatWords = (uint64_t)kFarFeatLevels * kFarMapN * kFarMapN;
constexpr uint32_t kFarFeatSolid = 0x40000000u;
inline uint64_t FarFeatWord(uint32_t level, int mx, int mz) {
  return kFarMapWords + kFarMapWord(level, mx, mz);
}
inline int FarFeatHeight(uint32_t w) { return (int)((w >> 8) & 15u); }
inline uint32_t FarFeatBody(uint32_t w) { return (w >> 16) & 0x7Fu; }
inline uint32_t FarFeatHead(uint32_t w) { return (w >> 23) & 0x7Fu; }
