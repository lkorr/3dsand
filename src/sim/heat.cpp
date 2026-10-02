#include "sim/heat.h"

#include <algorithm>
#include <cmath>

#include "sim/materials.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldmap.h"

static_assert(kHeatChunk == kChunk, "heat.h restates world.h kChunk");
static_assert(kHeatWindowChunks == kNumChunks, "heat.h restates world.h kNumChunks");
static_assert(kHeatWorldN == kWorldN, "heat.h restates world.h kWorldN");
static_assert(kHeatRadiusMax <= kHeatBlocksPerAxis,
              "the tent must stay inside a chunk's 3x3x3 neighbourhood");
static_assert(kHeatPoolPages <= kHeatEntryPage, "page index must fit the entry word");



// The loaded map carries each biome's climate by id (LoadWorldMap fills it from
// the same BiomeSet it packs), so a gate that swaps the map swaps the climate.
const HeatClimate& HeatBiomeClimate(uint32_t biomeIndex) {
  static const HeatClimate kDefault{};
  const std::vector<HeatClimate>& c = worldmap::CurrentWorldMap().biomeClimate;
  return biomeIndex < c.size() ? c[biomeIndex] : kDefault;
}

HeatClimate HeatSnowlineClimate() {
  HeatClimate c;
  c.base = CurrentTuning().sim.heatSnowlineBase;
  c.swing = CurrentTuning().sim.heatSnowlineSwing;
  return c;
}

// The snow caps start at treeline - 1 (worldgen.wgsl: snow at y > h - 2 for
// h >= treeline), so that is where the frozen climate begins.
int32_t HeatSnowlineY() { return worldmap::CurrentTerrain().treeline - 1; }

int32_t HeatAmbientCpu(int x, int y, int z, uint32_t seed, bool day) {
  const HeatClimate c = y >= HeatSnowlineY()
                            ? HeatSnowlineClimate()
                            : HeatBiomeClimate(World::MapBiomeAt(x, z, seed));
  return day ? c.Day() : c.Night();
}

void PackHeatMaterial(const ThermalDef& t, uint32_t out[kHpMatStride]) {
  for (uint32_t i = 0; i < kHpMatStride; i++) out[i] = 0;
  for (size_t k = 0; k < t.transitions.size() && k < kHeatMaxTransitions; k++) {
    const HeatTransition& h = t.transitions[k];
    const uint32_t thr = (uint32_t)std::clamp(h.threshold + kHeatThrBias, 0, 1023);
    const uint32_t full = (uint32_t)std::clamp(h.full + kHeatThrBias, 0, 1023);
    // The frontier: a melt or a freeze counts the faces that are NOT this
    // material (a bank melts from its surface in, a pond skins from its
    // banks); an ignition counts AIR faces (a buried log has nothing to burn
    // in).
    const uint32_t scale = h.kind == kHeatKindIgnite ? kHeatScaleAir : kHeatScaleNotSelf;
    uint32_t chance = (uint32_t)std::lround(h.chanceMille * (double)kReactChanceScale);
    if (chance == 0) chance = 1;
    out[3 * k + 0] = thr | (full << 10) | ((h.kind & 3u) << 20) | (scale << 22) |
                     (h.surface ? (1u << 24) : 0u);
    out[3 * k + 1] = chance;
    out[3 * k + 2] = (h.intoId & 0xFFFu) | ((h.partialIntoId & 0xFFFu) << 12);
  }
}

namespace {
HeatRunStats gRun;
}
void HeatNoteRun(const uint32_t* h) {
  gRun.runs++;
  gRun.pagesPeak = std::max(gRun.pagesPeak, h[kHmPagesPeak]);
  gRun.refused += h[kHmRefused];
  gRun.melts += h[kHmMelts];
  gRun.ignites += h[kHmIgnites];
  gRun.freezes += h[kHmFreezes];
  gRun.srcPeak = std::max(gRun.srcPeak, h[kHmSrcPeak]);
  gRun.recompPeak = std::max(gRun.recompPeak, h[kHmRecompPeak]);
  gRun.relaxPeak = std::max(gRun.relaxPeak, h[kHmRelaxPeak]);
}
const HeatRunStats& HeatRunTotals() { return gRun; }
