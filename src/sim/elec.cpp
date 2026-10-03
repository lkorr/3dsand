#include "sim/elec.h"

#include <algorithm>
#include <cmath>

#include "sim/materials.h"
#include "sim/world.h"

static_assert(kElecChunk == kChunk, "elec.h restates world.h kChunk");
static_assert(kElecWindowChunks == kNumChunks, "elec.h restates world.h kNumChunks");
static_assert(kElecChunkVol == kChunkVol, "elec.h restates world.h kChunkVol");
// The kernel's halo walk and sweeps assume a 16-cell chunk edge and 256-thread
// groups: one thread per line of a sweep, one per face cell of the halo.
static_assert(kElecChunk == 16, "sim_elec.wgsl assumes 16^3 chunks");

void PackElecMaterial(const ElecDef& e, uint32_t out[kEpMatStride]) {
  for (uint32_t i = 0; i < kEpMatStride; i++) out[i] = 0;
  const uint32_t r = std::min<uint32_t>(e.resist, kElecResistInsulator - 1);
  const uint32_t s = std::min<uint32_t>(e.source, kElecPMax);
  out[0] = (r & 0xFFu) | (s << 16);
  out[1] = (e.igniteIntoId & 0xFFFu) | ((e.charIntoId & 0xFFFu) << 12);
  if (e.igniteChanceMille > 0.0) {
    uint32_t chance = (uint32_t)std::lround(e.igniteChanceMille * (double)kReactChanceScale);
    out[2] = std::max<uint32_t>(chance, 1u);
  }
}

uint32_t ElecWetResist(uint32_t wetResist, uint32_t amount) {
  if (amount == 0 || wetResist == 0) return kElecResistInsulator;
  const uint32_t r = (wetResist * 15u + amount - 1u) / amount;
  return std::clamp<uint32_t>(r, 1u, kElecResistInsulator - 1);
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
