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

// ---- THE FAR STRATA PLANE (2026-10-14; DESIGN.md "The far strata plane") ---
// A third plane, after the feature plane, ALL levels: one u32 per SUB-COLUMN
// saying what the column holds UNDER its top voxel, as up to three runs by
// depth d (fine voxels under the top; d = 0 is the skin, the map entry's):
//   1 .. d1        the SUB slot   (the voxel under the top: grass's dirt)
//   d1+1 .. d2     the MID slot   (a second layer: the sediment's gravel)
//   d2+1 ..        the DEEP slot  (the bulk: stone)
// The map entry's sub-skin field alone painted a sub-column's whole riser,
// however tall, so a mesa wall -- four voxels of red_sandstone over stone per
// column step -- drew all red past the handoff. Written by worldgen.wgsl
// `farmap` beside the entry; trusted only under a VALID entry, so an edit
// needs no handling of its own. Bit layout (worldgen.wgsl FAR_STRATA_*;
// raymarch.wgsl reads the same bits):
//   bits  0..4   d1 (1..31)       bits  5..9   d2 (d1..31)
//   bits 10..16  sub slot         bits 17..23  mid slot      bits 24..30 deep slot
//   bit  31      PRESENT (0 = no strata: the old rule, the sub-skin all the way)
constexpr uint64_t kFarStrataWords = kFarMapWords;
constexpr uint64_t kFarStrataBase = kFarMapWords + kFarFeatWords;
constexpr uint32_t kFarStrataPresent = 0x80000000u;
inline uint64_t FarStrataWord(uint32_t level, int mx, int mz) {
  return kFarStrataBase + kFarMapWord(level, mx, mz);
}
inline int FarStrataD1(uint32_t w) { return (int)(w & 31u); }
inline int FarStrataD2(uint32_t w) { return (int)((w >> 5) & 31u); }
inline uint32_t FarStrataSub(uint32_t w) { return (w >> 10) & 0x7Fu; }
inline uint32_t FarStrataMid(uint32_t w) { return (w >> 17) & 0x7Fu; }
inline uint32_t FarStrataDeep(uint32_t w) { return (w >> 24) & 0x7Fu; }
// The far slot the column holds at depth d >= 1 under its top.
inline uint32_t FarStrataSlotAt(uint32_t w, int d) {
  return d <= FarStrataD1(w) ? FarStrataSub(w)
                             : (d <= FarStrataD2(w) ? FarStrataMid(w) : FarStrataDeep(w));
}
