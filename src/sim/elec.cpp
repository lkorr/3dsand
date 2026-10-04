#include "sim/elec.h"

#include <algorithm>
#include <cmath>

#include "sim/materials.h"
#include "sim/world.h"

static_assert(kElecChunk == kChunk, "elec.h restates world.h kChunk");
static_assert(kElecLiveMobs == kMaxLiveMobs,
              "elec.h kElecLiveMobs restates world.h kMaxLiveMobs: the body query is sized from it");
static_assert(kElecWindowChunks == kNumChunks, "elec.h restates world.h kNumChunks");
static_assert(kElecChunkVol == kChunkVol, "elec.h restates world.h kChunkVol");
// The kernel's halo walk and sweeps assume a 16-cell chunk edge and 256-thread
// groups: one thread per line of a sweep, one per face cell of the halo.
static_assert(kElecChunk == 16, "sim_elec.wgsl assumes 16^3 chunks");
// The resist and the source share word 0 of a material (12 + 16 bits).
static_assert(kElecResistInsulator == kElecResistMask, "the insulator is the all-ones resist");
static_assert(kElecResistMask < (1u << 16), "resist must not reach the source half");

void PackElecMaterial(const ElecDef& e, uint32_t out[kEpMatStride]) {
  for (uint32_t i = 0; i < kEpMatStride; i++) out[i] = 0;
  const uint32_t r = std::min<uint32_t>(e.resist, kElecResistInsulator - 1);
  const uint32_t s = std::min<uint32_t>(e.source, kElecPMax);
  out[0] = (r & kElecResistMask) | (s << 16);
  out[1] = (e.igniteIntoId & 0xFFFu) | ((e.charIntoId & 0xFFFu) << 12);
  if (e.igniteChanceMille > 0.0) {
    uint32_t chance = (uint32_t)std::lround(e.igniteChanceMille * (double)kReactChanceScale);
    out[2] = std::max<uint32_t>(chance, 1u);
  }
  out[3] = std::min<uint32_t>(e.dissolved, kElecResistInsulator - 1) & kElecResistMask;
}

uint32_t ElecWetResist(uint32_t wetResist, uint32_t amount) {
  if (amount == 0 || wetResist == 0) return kElecResistInsulator;
  const uint32_t r = (wetResist * 15u + amount - 1u) / amount;
  return std::clamp<uint32_t>(r, 1u, kElecResistInsulator - 1);
}

uint32_t ElecTagMask(const std::vector<MaterialDef>& mats) {
  uint32_t mask = 0xFFFFFFFFu;
  bool any = false;
  for (const MaterialDef& d : mats) {
    const bool has = std::find(d.tags.begin(), d.tags.end(), "electric") != d.tags.end();
    any = any || has;
    mask &= has ? d.gpu.tagMask : ~d.gpu.tagMask;
  }
  return any ? mask : 0u;
}

uint32_t ElecCrackleThreshold(int knob, uint32_t emitMat, uint32_t emitSource) {
  if (emitMat == 0) return kElecPOff;
  const uint32_t k = (uint32_t)std::clamp(knob, 1, (int)kElecPMax);
  return std::min<uint32_t>(std::max<uint32_t>(k, emitSource + 1u), kElecPOff);
}

namespace {
ElecRunStats gRun;
}
void ElecNoteRun(const uint32_t* h) {
  gRun.runs++;
  gRun.pagesPeak = std::max(gRun.pagesPeak, h[kEmPagesPeak]);
  gRun.refused += h[kEmRefused];
  gRun.purges += h[kEmPurges];
  gRun.stranded += h[kEmStranded];
  gRun.livePeak = std::max(gRun.livePeak, h[kEmLivePeak]);
  gRun.pPeak = std::max(gRun.pPeak, h[kEmPPeak]);
}
const ElecRunStats& ElecRunTotals() { return gRun; }
