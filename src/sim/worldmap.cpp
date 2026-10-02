// worldmap.cpp -- loads the authored world map and packs it, with the biome
// record table, into the worldMap buffer. See worldmap.h for the layout and
// the seed discipline.
#include "sim/worldmap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/intmath.h"   // the landform bake's integer sine/sqrt (no libm)
#include "sim/rng.h"
#include "sim/treeatlas.h"   // ReadTreeSpeciesHeaders: a tree site's species index + reach
#include "sim/tuning.h"
#include "sim/voxload.h"
#include "sim/world.h"
#include "world/structures.h"   // P4: the map's `structure` refs become stamp sites

using nlohmann::json;

namespace worldmap {
namespace {

uint32_t U(int v) { return static_cast<uint32_t>(v); }
uint32_t Vox(float metres) {
  const int v = static_cast<int>(std::lround(metres * kVoxelsPerMetre));
  return U(std::max(1, v));
}
// A depth or a reach may legitimately be zero ("from the waterline").
uint32_t Vox0(float metres) {
  const int v = static_cast<int>(std::lround(metres * kVoxelsPerMetre));
  return U(std::max(0, v));
}

// ---- the flora half of a water preset (P-E) --------------------------------
// The shader jitters a shore stalk of 3+ cells by +-(H/6, at least 1) per
// column so a bed of stalks is not a fence; the ceiling has to include it.
uint32_t ShoreJitter(uint32_t h) { return h >= 3u ? std::max(1u, h / 6u) : 0u; }

// Whether a row is really on: an authored chance AND a material that
// resolved. ValidateBiomeSet reports the half-authored case; here it is
// simply not packed, so the shader never rolls for a material 0.
bool ShoreRowOn(const biomes::ShorePlantRow& r) { return r.chance > 0 && r.materialId != 0; }
bool BandOn(const biomes::AquaticBand& b) { return b.chance > 0 && b.materialId != 0; }

// The tallest thing this preset puts above the ground (shore rows, jitter
// included) or above the bed (the emergent band): the sky-skip / far-blocker
// margin for any column the preset can touch.
uint32_t MaxPlantH(const biomes::WaterPresetDef& w) {
  uint32_t m = 0;
  for (const biomes::ShorePlantRow& r : w.shorePlants)
    if (ShoreRowOn(r)) m = std::max(m, Vox(r.heightM) + ShoreJitter(Vox(r.heightM)));
  if (BandOn(w.emergent)) m = std::max(m, Vox(w.emergent.heightM));
  return m;
}

// ---- the geometry half of a water preset (P-F) ------------------------------
// watergen.js profileAt, ported: monotone cubic (Fritsch-Carlson) through the
// sanitized (u, fraction) points. Evaluated on the CPU at load only -- the
// shader sees the sampled integer knots and nothing else.
double ProfileAt(const std::vector<std::pair<float, float>>& pts, double u) {
  const size_t n = pts.size();
  if (n == 0) return 1.0 - u;
  if (n == 1) return pts[0].second;
  u = std::clamp(u, 0.0, 1.0);
  std::vector<double> d(n - 1), m(n);
  for (size_t i = 0; i + 1 < n; i++) {
    const double h = pts[i + 1].first - pts[i].first;
    d[i] = h > 0 ? (pts[i + 1].second - pts[i].second) / h : 0.0;
  }
  m[0] = d[0]; m[n - 1] = d[n - 2];
  for (size_t i = 1; i + 1 < n; i++) {
    if (d[i - 1] * d[i] <= 0) { m[i] = 0; continue; }
    const double w1 = 2 * (pts[i + 1].first - pts[i].first) + (pts[i].first - pts[i - 1].first);
    const double w2 = (pts[i + 1].first - pts[i].first) + 2 * (pts[i].first - pts[i - 1].first);
    m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
  }
  size_t i = 0;
  while (i + 2 < n && u > pts[i + 1].first) i++;
  const double h = pts[i + 1].first - pts[i].first;
  if (h <= 0) return pts[i].second;
  const double t = (u - pts[i].first) / h, t2 = t * t, t3 = t2 * t;
  const double h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
  const double h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
  const double v = h00 * pts[i].second + h10 * h * m[i] + h01 * pts[i + 1].second + h11 * h * m[i + 1];
  return std::clamp(v, 0.0, 1.0);
}

}  // namespace

WaterGeom WaterGeomOf(const biomes::WaterPresetDef& w) {
  WaterGeom g;
  const auto vox = [](float m) { return static_cast<int>(std::lround(m * kVoxelsPerMetre)); };
  const int r = vox(w.radiusM), rv = std::max(0, vox(w.radiusVM));
  g.radiusMin = std::clamp(r - rv, 4, 2048);
  g.radiusSpan = std::clamp(2 * rv + 1, 1, 4096);
  g.depth = std::clamp(vox(w.depthM), 1, 200);
  g.rimDepth = std::clamp(vox(w.rimDepthM), 0, g.depth);
  g.bermH = std::clamp(vox(w.bermHeightM), 0, 255);
  g.bermW = std::clamp(vox(w.bermWidthM), 1, 255);
  g.shoreBand = std::clamp(vox(w.shoreBandM), 0, 255);
  g.shoreLift = std::clamp(vox(w.shoreLiftM), 0, 4096);
  g.mudWidth = std::clamp(vox(w.mudWidthM), 0, g.shoreBand);
  g.bedShallowDepth = std::clamp(vox(w.bedShallowDepthM), 0, 4096);
  g.bedThickness = std::clamp(vox(w.bedThicknessM), 0, 64);
  g.band = std::max(g.shoreBand, g.bermW);
  g.fill = w.fillId;
  g.mudMat = w.mudId;
  g.bedShallow = w.bedShallowId;
  g.bedDeep = w.bedDeepId;
  g.bedSubstrate = w.bedSubstrateId;
  // Seventeen knots at u_k = sqrt(k / 16), the radii the shader's
  // d^2-parametrised interpolation lands on; forced NON-INCREASING so the
  // bowl is a bowl (the basin curve inverts it by bisection) and pinned to
  // 256 at the centre and 0 at the rim like the sanitized curve.
  int prev = 256;
  for (int k = 0; k <= 16; k++) {
    int v = static_cast<int>(std::lround(256.0 * ProfileAt(w.profile, std::sqrt(k / 16.0))));
    v = std::clamp(v, 0, 256);
    if (k == 0) v = 256;
    if (k == 16) v = 0;
    v = std::min(v, prev);
    g.knots[k] = v;
    prev = v;
  }
  return g;
}

int WaterGeomSteepestQ8(const WaterGeom& g, int r) {
  int worst = 0;
  const double dd = g.depth - g.rimDepth;
  for (int k = 0; k < 16; k++) {
    const double dr = (std::sqrt((k + 1) / 16.0) - std::sqrt(k / 16.0)) * std::max(r, 1);
    const double drop = (g.knots[k] - g.knots[k + 1]) / 256.0 * dd;
    if (dr > 0) worst = std::max(worst, static_cast<int>(std::lround(256.0 * drop / dr)));
  }
  return worst;
}

uint32_t WaterPresetIndex(const biomes::BiomeSet& set, const std::string& name) {
  for (size_t i = 0; i < set.water.size(); i++)
    if (set.water[i].name == name) return static_cast<uint32_t>(i + 1);
  return 0u;
}

// ---- the map's terrain (P-G) --------------------------------------------------
void TerrainWords(const TerrainParams& t, uint32_t out[kTerrainWords]) {
  const auto at = [&](uint32_t word) -> uint32_t& { return out[word - kHTerrainBaseHeight]; };
  at(kHTerrainBaseHeight) = U(t.baseHeight);
  at(kHTerrainLandformRange) = U(std::max(0, t.landformRangeVox));
  at(kHTerrainRangeAmplitude) = U(std::max(0, t.rangeAmplitude));
  at(kHTerrainRangeLog2) = U(std::clamp(t.rangeLog2, 3, 15));
  at(kHTerrainHillAmplitude) = U(std::max(0, t.hillAmplitude));
  at(kHTerrainHillLog2) = U(std::clamp(t.hillLog2, 3, 15));
  at(kHTerrainDetailAmplitude) = U(std::max(0, t.detailAmplitude));
  at(kHTerrainDetailLog2) = U(std::clamp(t.detailLog2, 3, 15));
  at(kHTerrainGrainAmplitude) = U(std::max(0, t.grainAmplitude));
  at(kHTerrainGrainLog2) = U(std::clamp(t.grainLog2, 3, 15));
  at(kHTerrainFbmAtten) = U(std::clamp(t.fbmAtten, 0, 256));
  at(kHTerrainHomeY) = U(t.homeY);
  at(kHTerrainHomeR) = U(std::max(0, t.homeR));
  at(kHTerrainHomeFade) = U(std::max(1, t.homeFade));
  at(kHTerrainSedCeil) = U(t.sedCeil);
  at(kHTerrainSedFraction) = U(std::max(0, t.sedFraction));
  at(kHTerrainSedStrip) = U(std::max(0, t.sedStrip));
  at(kHTerrainSedSlope) = U(std::max(0, t.sedSlope));
  at(kHTerrainSedMax) = U(std::max(0, t.sedMax));
  at(kHTerrainSedTopsoil) = U(std::clamp(t.sedTopsoil, 0, std::max(0, t.sedMax)));
  at(kHTerrainTreeline) = U(t.treeline);
  at(kHTerrainRefVpm) = U(std::max(1, t.refVoxelsPerMetre));
}

BiomeTerrainPacked PackBiomeTerrain(const biomes::BiomeDef& b) {
  BiomeTerrainPacked p;
  for (int i = 0; i < 9; i++)
    p.w[kB_CurveKnot0 - kB_CurveKnot0 + static_cast<uint32_t>(i)] = U(std::clamp(b.terrain.curve[i], -16384, 16384));
  p.w[kB_HillMul - kB_CurveKnot0] = U(std::clamp(b.terrain.hill, 0, 4096));
  p.w[kB_DetailMul - kB_CurveKnot0] = U(std::clamp(b.terrain.detail, 0, 4096));
  p.w[kB_GrainMul - kB_CurveKnot0] = U(std::clamp(b.terrain.grain, 0, 4096));
  return p;
}

// A declared landform, onto the plane. Tier A: no seed anywhere in here. The
// plane's unit is landformRangeVox / 256 voxels (the shader's
// ((L << 8) - 32768) * range >> 16), so a site's heightVox becomes
// heightVox * 256 / range units at its centre. Shapes are profiles of the
// normalised distance t in 0..1 from the centre:
//   peak     1 - t                      (a cone)
//   basin    -(1 - t)                   (the cone, sunk; heightVox's sign is
//                                        taken as a magnitude either way)
//   plateau  1 inside 0.6, then 1 - (t - 0.6) / 0.4   (a flat top, ramped out)
//   ridge    the cone over an ellipse of semi-axes (radius, radius / 3)
//            turned `rotation` degrees, so it reads as a range with a crest
// The value is accumulated in whole units and clamped to the byte at the end,
// so a peak painted over a basin sums rather than replaces.
//
// INTEGER, AND WHY. This bake used to run in doubles with libm cos and sin on
// the ridge rotation. The plane it writes is read by worldgen — by the shader
// AND by the CPU height twin — so one byte that differs between two machines
// is a terrain that differs between two machines, which is a desync no gate on
// one box can see. libm is not specified to be correctly rounded and is not
// bit-identical across platforms or compiler versions, so the bake is now
// integer end to end: imath::BamFromDegreesI turns whole degrees into an exact
// BAM (90 degrees really is a quarter turn), imath's Q30 sine replaces libm's,
// distances come out of an exact integer sqrt, and the accumulator is Q16.16
// landform units rounded half-up at the end. See docs/PLAN_multiplayer_now.md
// L6. `+ - * / sqrt` on floats WOULD have been safe (IEEE specifies them
// exactly, which is why ProfileAt above still uses std::sqrt); cos and sin are
// the two that are not.
void OverlayLandformSites(const std::vector<LandformSite>& sites, int landformRangeVox,
                          int cellLog2, int width, int height, int originCellX, int originCellZ,
                          std::vector<uint8_t>& landform) {
  if (sites.empty() || landform.size() != static_cast<size_t>(width) * height) return;
  const int64_t range = std::max(1, landformRangeVox);
  const int64_t half = 1 << (cellLog2 - 1);
  // Q16.16 landform units. The plane's own bytes seed it, so a site adds to
  // what the author painted rather than replacing it.
  std::vector<int64_t> acc(landform.size());
  for (size_t i = 0; i < landform.size(); i++) acc[i] = static_cast<int64_t>(landform[i]) << 16;
  for (const LandformSite& st : sites) {
    const int64_t r = std::max(1, st.radius);
    const int64_t hv = st.heightVox < 0 ? -static_cast<int64_t>(st.heightVox)
                                        : static_cast<int64_t>(st.heightVox);
    // heightVox * 256 / range landform units, Q16.16; a basin sinks the plane.
    int64_t ampQ = imath::DivRound(hv * 256 * 65536, range);
    if (st.shape == "basin") ampQ = -ampQ;
    if (ampQ == 0) continue;
    const uint32_t rotBam = imath::BamFromDegreesI(st.rotation);
    const int64_t ca = imath::CosQ30(rotBam), sa = imath::SinQ30(rotBam);
    // The ridge's cross-crest semi-axis, radius / 3, kept as Q16.16 so the
    // thirds are not thrown away on a small footprint.
    const int64_t bQ = std::max<int64_t>(65536, (r * 65536) / 3);
    // cells the footprint can reach, +1 for the ridge's rotated corners
    int c0x, c0z, c1x, c1z;
    const auto cellOf = [&](int x, int z, int* cx, int* cz) {
      *cx = (x >> cellLog2) + originCellX;
      *cz = (z >> cellLog2) + originCellZ;
    };
    cellOf(st.x - st.radius - 1, st.z - st.radius - 1, &c0x, &c0z);
    cellOf(st.x + st.radius + 1, st.z + st.radius + 1, &c1x, &c1z);
    c0x = std::max(c0x - 1, 0); c0z = std::max(c0z - 1, 0);
    c1x = std::min(c1x + 1, width - 1); c1z = std::min(c1z + 1, height - 1);
    for (int cz = c0z; cz <= c1z; cz++)
      for (int cx = c0x; cx <= c1x; cx++) {
        const int64_t wx = (static_cast<int64_t>(cx - originCellX) << cellLog2) + half;
        const int64_t wz = (static_cast<int64_t>(cz - originCellZ) << cellLog2) + half;
        const int64_t dx = wx - st.x, dz = wz - st.z;
        int64_t tQ;  // normalised distance from the centre, Q16.16
        if (st.shape == "ridge") {
          // into the ridge's frame: `u` along the crest, `v` across it. The
          // Q30 sine times a whole-voxel offset is a Q30 length; >>14 (rounded,
          // never a bare arithmetic shift, which would floor both signs the
          // same way and drag the crest one way) lands it in Q16.16.
          const int64_t uQ = imath::DivRound(dx * ca + dz * sa, 1 << 14);
          const int64_t vQ = imath::DivRound(-dx * sa + dz * ca, 1 << 14);
          const int64_t ru = imath::DivRound(uQ, r);         // u / a, Q16.16
          const int64_t rv = imath::DivRound(vQ << 16, bQ);  // v / b, Q16.16
          // Two Q16.16s squared make a Q32.32; its exact integer sqrt is a
          // Q16.16 again, with no intermediate rounding to argue about.
          tQ = static_cast<int64_t>(imath::Sqrt64(static_cast<uint64_t>(ru * ru + rv * rv)));
        } else {
          const uint64_t d2 = static_cast<uint64_t>(dx * dx + dz * dz);
          // `d2 << 32` is exact only while d2 < 2^32, i.e. while the distance
          // is under 65,536 voxels; past that it wraps the u64 and bakes
          // garbage. That is REACHABLE, because the loader clamps radius to
          // 1 << 20 (a 105 km site), so take the root first and shift after
          // for the far branch: the fraction it drops is under one voxel out
          // of a distance already past 65,536, hence under 1/65,536 of t --
          // below the Q16.16 LSB, let alone the byte the plane stores.
          // The ridge branch needs no such guard: dx * ca peaks at
          // 2^20 * 2^30 = 2^50, vQ << 16 at 2^52, and ru / rv are bounded by
          // ~2^18 each, so its ru*ru + rv*rv sits around 2^36.
          tQ = d2 < (1ull << 32)
                   ? imath::DivRound(static_cast<int64_t>(imath::Sqrt64(d2 << 32)), r)
                   : imath::DivRound(static_cast<int64_t>(imath::Sqrt64(d2) << 16), r);
        }
        if (tQ >= 65536) continue;
        int64_t fQ;
        if (st.shape == "plateau") {
          // 1 inside 0.6, then 1 - (t - 0.6) / 0.4 == (5 - 5t) / 2. Written as
          // exact fifths rather than as 0.6/0.4 rounded into Q16.16, so the
          // flat top ends exactly where the comment above says it does.
          fQ = (5 * tQ <= 3 * 65536) ? 65536 : imath::DivRound(5 * (65536 - tQ), 2);
        } else {
          fQ = 65536 - tQ;
        }
        acc[static_cast<size_t>(cz) * width + cx] += imath::MulShiftRound(ampQ, fQ, 16);
      }
  }
  for (size_t i = 0; i < landform.size(); i++) {
    // Explicit integer half-up: +0.5 then floor, the floor being the
    // arithmetic shift. Written out rather than left to std::lround because
    // the rounding MODE is part of the byte the whole world reads.
    const int64_t v = (acc[i] + 32768) >> 16;
    landform[i] = static_cast<uint8_t>(std::clamp<int64_t>(v, 0, 255));
  }
}

namespace {
int TileVoxOf(const biomes::WaterRow& r) {
  return std::max(0, static_cast<int>(std::lround(r.tileM * kVoxelsPerMetre)));
}
// A row that can place something: a loaded preset and a rarity that rolls.
bool WaterRowLive(const biomes::BiomeSet& set, const biomes::WaterRow& r) {
  return r.rarity > 0 && TileVoxOf(r) > 0 && WaterPresetIndex(set, r.preset) != 0u;
}
}  // namespace

int PondLatticeVox(const biomes::BiomeSet& set) {
  int finest = 0;
  for (const biomes::BiomeDef& b : set.biomes)
    for (const biomes::WaterRow& r : b.water)
      if (WaterRowLive(set, r)) finest = finest == 0 ? TileVoxOf(r) : std::min(finest, TileVoxOf(r));
  // The lattice floor: a disc needs room to be a disc. 64 vox = 6.4 m.
  return finest == 0 ? 0 : std::max(finest, 64);
}

int PondBandVox(const biomes::BiomeSet& set) {
  int band = 0;
  for (const biomes::WaterPresetDef& w : set.water) band = std::max(band, WaterGeomOf(w).band);
  return band;
}

std::vector<WaterRowPacked> PackWaterRows(const biomes::BiomeSet& set, const biomes::BiomeDef& b, int latticeVox) {
  std::vector<WaterRowPacked> out;
  if (latticeVox <= 0) return out;
  for (const biomes::WaterRow& r : b.water) {
    if (!WaterRowLive(set, r)) continue;
    if (out.size() >= kWaterRowsMax) break;   // the shader rolls four, unrolled
    // chance = (T / tile)^2 / rarity in Q16, integer: T^2 * 65536 / (tile^2 * rarity).
    // A biome whose tile IS the lattice and rarity 4 rolls 1 in 4 lattice
    // tiles; a coarser tile thins by the area ratio, so bodies per km^2 match
    // the page's rarityStats whatever lattice the finest biome imposed.
    const int64_t T = latticeVox, tile = TileVoxOf(r);
    int64_t chance = (T * T * 65536) / std::max<int64_t>(1, tile * tile * r.rarity);
    chance = std::clamp<int64_t>(chance, 1, 65536);
    WaterRowPacked p;
    p.w[kR_Preset] = WaterPresetIndex(set, r.preset);
    p.w[kR_ChanceQ16] = static_cast<uint32_t>(chance);
    p.w[kR_MinY] = U(r.cond.minY);
    p.w[kR_MaxY] = U(r.cond.maxY);
    p.w[kR_MaxSlope] = U(std::clamp(r.cond.maxSlope, 0, 1024));
    out.push_back(p);
  }
  return out;
}

namespace {

// FNV-1a. Not a security hash; a change-detector so the boot line can name
// the table/map that produced this world's hash.
uint32_t FnvBytes(uint32_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
uint32_t FnvWords(const std::vector<uint32_t>& w) {
  uint32_t h = 2166136261u;
  for (uint32_t x : w)
    for (int i = 0; i < 4; i++) { h ^= (x >> (8 * i)) & 0xFFu; h *= 16777619u; }
  return h;
}

// Four cells per word, little-endian.
void AppendPlane(std::vector<uint32_t>& W, const std::vector<uint8_t>& plane) {
  const size_t words = (plane.size() + 3) / 4;
  const size_t at = W.size();
  W.resize(at + words, 0u);
  for (size_t i = 0; i < plane.size(); i++)
    W[at + (i >> 2)] |= static_cast<uint32_t>(plane[i]) << ((i & 3) * 8);
}

WorldMapData& Slot() {
  static WorldMapData g;
  return g;
}

}  // namespace

bool PackBiomeTable(const biomes::BiomeSet& set, std::vector<uint32_t>& W,
                    std::string& log) {
  const int n = static_cast<int>(set.biomes.size());
  std::vector<const biomes::BiomeDef*> byId(static_cast<size_t>(std::max(n, 0)), nullptr);
  for (const biomes::BiomeDef& b : set.biomes) {
    if (b.index < 0 || b.index >= n || byId[b.index]) {
      log += "worldmap: biome ids are not contiguous 0.." + std::to_string(n - 1) +
             " (biomes/" + b.file + " has index " + std::to_string(b.index) + ")\n";
      return false;
    }
    byId[b.index] = &b;
  }
  // The one tree lattice every biome is thinned on (biomes.h). The tree atlas
  // derives the same number for the shader's scan; both are pure functions of
  // the set, so there is no ordering between the two loaders.
  const int treeLattice = biomes::FinestTreeTileVox(set);
  // The one pond lattice (worldmap.h kHPondTile), the same way.
  const int pondLattice = PondLatticeVox(set);

  W.assign(kHeaderWords, 0u);
  W[kHMagic] = kMagic;
  W[kHVersion] = kVersion;
  W[kHBiomeCount] = U(n);
  W[kHBiomeRecords] = U(kHeaderWords);
  const size_t rec0 = W.size();
  W.resize(rec0 + static_cast<size_t>(n) * kBiomeRecWords, 0u);

  for (int i = 0; i < n; i++) {
    const biomes::BiomeDef& b = *byId[i];
    uint32_t* r = W.data() + rec0 + static_cast<size_t>(i) * kBiomeRecWords;
    r[kB_Skin] = b.skinId;
    r[kB_Subsoil] = b.subsoilId;
    // 0 = the biome authored no cover.firmSkin; genCellIn falls back to the
    // subsoil (if solid) or stone. The class test lives in the shader because
    // the packer has no material table to ask.
    r[kB_FirmCover] = b.firmSkinId;
    r[kB_SkinDepth] = U(std::max(1, b.skinDepth));
    r[kB_PatchThreshold] = U(std::clamp(b.patchThreshold, 0, 255));
    r[kB_PatchCellLog2] = U(std::clamp(b.patchCellLog2, 2, 12));
    r[kB_TreeTileVox] = U(biomes::TreeTileVox(b));
    r[kB_TreeDensity] = U(std::clamp(b.treeDensity, 0, 100));
    r[kB_TreeChanceQ16] = biomes::TreeChanceQ16(b, treeLattice);
    // Cave thresholds: the biome's rows. A biome that authors no row keeps
    // the pre-biome world's (150 / 148); there is no global knob any more.
    int t1 = 150, t2 = 148;
    for (const biomes::CaveRow& c : b.caves) {
      if (c.preset == "near_surface") t1 = c.threshold;
      else if (c.preset == "deep") t2 = c.threshold;
    }
    r[kB_CaveThreshold1] = U(std::clamp(t1, 0, 255));
    r[kB_CaveThreshold2] = U(std::clamp(t2, 0, 255));
    uint32_t flags = 0;
    if (b.groundFlora) flags |= kBF_GroundFlora;
    if (b.cacti) flags |= kBF_Cacti;
    if (b.sandCap) flags |= kBF_SandCap;
    if (b.frozen) flags |= kBF_Frozen;
    for (const biomes::CoverRow& c : b.cover)
      if (c.chance > 0 && c.materialId != 0 && (c.cond.canopyMin > 0 || c.cond.canopyMax < 255)) flags |= kBF_CanopyRows;
    r[kB_Flags] = flags;
    // P-G: the biome's terrain record, the same PackBiomeTerrain the loader
    // keeps on WorldMapData::biomeTerrain for the CPU twin.
    {
      const BiomeTerrainPacked bt = PackBiomeTerrain(b);
      for (uint32_t k = 0; k < kBiomeTerrainWords; k++) r[kB_CurveKnot0 + k] = bt.w[k];
    }
    // P-E: cave flora from the band rows (mushrooms on the near band's floor,
    // crystal on the deep band's floor and ceiling), the cactus density, and
    // the water preset the biome's ponds and shores wear (see WaterPresetOf).
    // A biome that authors no row / no key gets 0 = never; there is no global
    // default any more.
    int mushroom = 0, crystal = 0;
    for (const biomes::CaveRow& c : b.caves) {
      if (c.preset == "near_surface") mushroom = c.mushroomChance;
      else if (c.preset == "deep") crystal = c.crystalChance;
    }
    r[kB_CaveMushroomChance] = U(std::max(0, mushroom));
    r[kB_CaveCrystalChance] = U(std::max(0, crystal));
    r[kB_CactusChance] = U(std::clamp(b.cactusChance, 0, 100));
    r[kB_SaguaroFraction] = U(std::clamp(b.saguaroFraction, 0, 100));
    // Cover rows are appended AFTER every record so the record table stays a
    // fixed stride; a row with chance 0 is authored-off and skipped here.
    r[kB_CoverOff] = U(static_cast<int>(W.size()));
    int count = 0;
    uint32_t maxH = 0;
    for (const biomes::CoverRow& c : b.cover) {
      if (c.chance <= 0 || c.materialId == 0) continue;
      // +1: the shader jitters stalks of 3+ by -1..+1 per column.
      maxH = std::max(maxH, Vox(c.heightM) + 1u);
      const size_t at = W.size();
      W.resize(at + kCoverRowWords, 0u);
      // `r` may have moved; re-derive after every resize.
      uint32_t* row = W.data() + at;
      row[kC_Mat] = c.materialId;
      row[kC_Head] = c.headId;
      row[kC_Chance] = U(c.chance);
      row[kC_HeightVox] = Vox(c.heightM);
      row[kC_MinY] = U(c.cond.minY);
      row[kC_MaxY] = U(c.cond.maxY);
      row[kC_MaxSlope] = U(std::clamp(c.cond.maxSlope, 0, 1024));
      row[kC_PatchThreshold] = U(std::clamp(c.cond.patchThreshold, 0, 255));
      // Water distances: metres -> voxels, -1 stays "unbounded".
      row[kC_NearWaterMax] = c.cond.nearWaterMaxM < 0
          ? U(-1) : U(static_cast<int>(std::lround(c.cond.nearWaterMaxM * kVoxelsPerMetre)));
      row[kC_NearWaterMin] = U(std::max(0, static_cast<int>(std::lround(c.cond.nearWaterMinM * kVoxelsPerMetre))));
      row[kC_CanopyMin] = U(std::clamp(c.cond.canopyMin, 0, 255));
      row[kC_CanopyMax] = U(std::clamp(c.cond.canopyMax, 0, 255));
      count++;
    }
    // The biome's ceiling includes EVERY preset its water rows can roll (and,
    // through kHMaxCoverH below, every preset an authored site can wear): a
    // shore stalk stands on ground the sky-skip would otherwise clear above
    // (the same "skipped chunk drops voxels" failure the cover rows had).
    for (const biomes::WaterRow& wr : b.water) {
      const uint32_t wp = WaterPresetIndex(set, wr.preset);
      if (wp) maxH = std::max(maxH, MaxPlantH(set.water[wp - 1]));
    }
    W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_CoverCount] = U(count);
    // P-F: the biome's water rows, after its cover rows.
    {
      const std::vector<WaterRowPacked> rows = PackWaterRows(set, b, pondLattice);
      W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_WaterOff] = U(static_cast<int>(W.size()));
      W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_WaterCount] = U(static_cast<int>(rows.size()));
      for (const WaterRowPacked& p : rows) W.insert(W.end(), p.w, p.w + kWaterRowWords);
    }
    W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_MaxCoverH] = maxH;
    W[kHMaxCoverH] = std::max(W[kHMaxCoverH], maxH);
  }
  // An authored water site may wear any preset in any biome, so the far
  // blocker band and the sky-skip take the widest plant of them all.
  for (const biomes::WaterPresetDef& w : set.water) W[kHMaxCoverH] = std::max(W[kHMaxCoverH], MaxPlantH(w));
  W[kHPondTile] = U(pondLattice);
  W[kHPondBand] = U(PondBandVox(set));

  // ---- the water preset table (P-E): records, then each preset's shore rows --
  // Every preset is packed whether or not a biome names it: the index is the
  // loader's order, and P-F will address the table from pond sites as well.
  const int nw = static_cast<int>(set.water.size());
  W[kHWaterCount] = U(nw);
  W[kHWaterRecords] = U(static_cast<int>(W.size()));
  const size_t wrec0 = W.size();
  W.resize(wrec0 + static_cast<size_t>(nw) * kWaterRecWords, 0u);
  for (int i = 0; i < nw; i++) {
    const biomes::WaterPresetDef& w = set.water[static_cast<size_t>(i)];
    auto rec = [&](uint32_t word) -> uint32_t& { return W[wrec0 + static_cast<size_t>(i) * kWaterRecWords + word]; };
    rec(kW_Fill) = w.fillId;
    rec(kW_MossChance) = w.mossId ? U(std::max(0, w.mossChance)) : 0u;
    rec(kW_MossMat) = w.mossId;
    const biomes::AquaticBand& e = w.emergent;
    if (BandOn(e)) {
      rec(kW_EmergentMat) = e.materialId;
      rec(kW_EmergentChance) = U(e.chance);
      rec(kW_EmergentMinDepth) = Vox0(e.minDepthM);
      rec(kW_EmergentMaxDepth) = Vox0(e.maxDepthM);
      rec(kW_EmergentHeight) = Vox(e.heightM);
    }
    const biomes::AquaticBand& f = w.floating;
    if (BandOn(f)) {
      rec(kW_FloatingMat) = f.materialId;
      rec(kW_FloatingFlower) = f.flowerId;
      rec(kW_FloatingChance) = U(f.chance);
      rec(kW_FloatingFlowerChance) = f.flowerId ? U(std::max(0, f.flowerChance)) : 0u;
      rec(kW_FloatingMinDepth) = Vox0(f.minDepthM);
      rec(kW_FloatingMaxDepth) = Vox0(f.maxDepthM);
    }
    const biomes::AquaticBand& s = w.submerged;
    if (BandOn(s)) {
      rec(kW_SubmergedMat) = s.materialId;
      rec(kW_SubmergedChance) = U(s.chance);
      rec(kW_SubmergedMinDepth) = Vox0(s.minDepthM);
      rec(kW_SubmergedHeight) = Vox(s.heightM);
      rec(kW_SubmergedClearance) = Vox0(s.clearanceM);
    }
    rec(kW_MaxPlantH) = MaxPlantH(w);
    // ---- the geometry half (P-F): one WaterGeomOf, the same call the loader
    // keeps for the CPU twin, so the two sides pack the same integers ----
    {
      const WaterGeom g = WaterGeomOf(w);
      rec(kW_RadiusMin) = U(g.radiusMin);
      rec(kW_RadiusSpan) = U(g.radiusSpan);
      rec(kW_Depth) = U(g.depth);
      rec(kW_RimDepth) = U(g.rimDepth);
      rec(kW_BermH) = U(g.bermH);
      rec(kW_BermW) = U(g.bermW);
      rec(kW_ShoreBand) = U(g.shoreBand);
      rec(kW_ShoreLift) = U(g.shoreLift);
      rec(kW_MudWidth) = U(g.mudWidth);
      rec(kW_MudMat) = g.mudMat;
      rec(kW_BedShallow) = g.bedShallow;
      rec(kW_BedDeep) = g.bedDeep;
      rec(kW_BedShallowDepth) = U(g.bedShallowDepth);
      rec(kW_BedThickness) = U(g.bedThickness);
      rec(kW_BedSubstrate) = g.bedSubstrate;
      rec(kW_Band) = U(g.band);
      for (int k = 0; k <= 16; k++)
        rec(kW_Knots + static_cast<uint32_t>(k >> 1)) |= U(g.knots[k]) << ((k & 1) * 16);
    }
    // Shore rows, in authored order (the shader rolls them in order, first
    // hit wins, so the author puts the common ground layer last).
    rec(kW_ShoreOff) = U(static_cast<int>(W.size()));
    uint32_t count = 0;
    for (const biomes::ShorePlantRow& p : w.shorePlants) {
      if (!ShoreRowOn(p)) continue;
      const size_t at = W.size();
      W.resize(at + kShoreRowWords, 0u);
      uint32_t* row = W.data() + at;   // W moved: never hold `rec` across this
      row[kP_Mat] = p.materialId;
      row[kP_Head] = p.headId;
      row[kP_Chance] = U(p.chance);
      row[kP_Reach] = Vox0(p.reachM);
      row[kP_Height] = Vox(p.heightM);
      count++;
    }
    W[wrec0 + static_cast<size_t>(i) * kWaterRecWords + kW_ShoreCount] = count;
  }
  W[kHContentHash] = 0u;
  W[kHContentHash] = FnvWords(W);
  return true;
}

// ---- stamps: a .vox template -> rotated columns of runs -------------------
namespace {

// Rotate a footprint-local (x, z) by rot * 90 degrees inside an nx x nz box.
void RotXZ(int rot, int nx, int nz, int x, int z, int* ox, int* oz, int* onx, int* onz) {
  switch (rot & 3) {
    case 0: *ox = x;          *oz = z;          *onx = nx; *onz = nz; break;
    case 1: *ox = nz - 1 - z; *oz = x;          *onx = nz; *onz = nx; break;
    case 2: *ox = nx - 1 - x; *oz = nz - 1 - z; *onx = nx; *onz = nz; break;
    default:*ox = z;          *oz = nx - 1 - x; *onx = nz; *onz = nx; break;
  }
}

bool PackStamp(const Prefab& pf, int rot, WorldMapData::StampSite& s, std::string& log) {
  // Gather every voxel of every model into one grid, prefab frame (min = 0).
  const int nx0 = pf.size.x, ny0 = pf.size.y, nz0 = pf.size.z;
  if (nx0 <= 0 || ny0 <= 0 || nz0 <= 0 || ny0 >= 2048 || nx0 > 512 || nz0 > 512) {
    log += "worldmap: stamp \"" + pf.name + "\" has an unusable size\n";
    return false;
  }
  int nx, nz, dummy;
  RotXZ(rot, nx0, nz0, 0, 0, &dummy, &dummy, &nx, &nz);
  s.nx = nx; s.ny = ny0; s.nz = nz;
  std::vector<uint16_t> grid(static_cast<size_t>(nx) * nz * ny0, 0);
  for (const PrefabModel& m : pf.models)
    for (const PrefabVoxel& v : m.voxels) {
      const int x = m.offset.x + v.x, y = m.offset.y + v.y, z = m.offset.z + v.z;
      if (x < 0 || y < 0 || z < 0 || x >= nx0 || y >= ny0 || z >= nz0) continue;
      int rx, rz, a, b;
      RotXZ(rot, nx0, nz0, x, z, &rx, &rz, &a, &b);
      grid[(static_cast<size_t>(rz) * nx + rx) * ny0 + y] = v.material;
    }
  // Header + column directory, then the runs.
  std::vector<uint32_t>& W = s.words;
  W.assign(kStampHdrWords + static_cast<size_t>(nx) * nz * 2, 0u);
  W[kStamp_NX] = static_cast<uint32_t>(nx);
  W[kStamp_NY] = static_cast<uint32_t>(ny0);
  W[kStamp_NZ] = static_cast<uint32_t>(nz);
  W[kStamp_Columns] = kStampHdrWords;   // relative; rebased by the packer
  for (int cz = 0; cz < nz; cz++)
    for (int cx = 0; cx < nx; cx++) {
      const size_t col = static_cast<size_t>(cz) * nx + cx;
      const uint32_t runOff = static_cast<uint32_t>(W.size());
      uint32_t count = 0;
      int y = 0;
      while (y < ny0) {
        const uint16_t m = grid[col * ny0 + y];
        if (m == 0) { y++; continue; }
        int len = 1;
        while (y + len < ny0 && len < 31 && grid[col * ny0 + y + len] == m) len++;
        W.push_back((static_cast<uint32_t>(m) & 0xFFFu) | (static_cast<uint32_t>(y) << 16) |
                    (static_cast<uint32_t>(len) << 27));
        count++;
        y += len;
      }
      W[kStampHdrWords + col * 2] = runOff;       // relative
      W[kStampHdrWords + col * 2 + 1] = count;
    }
  return true;
}

}  // namespace

bool PackSculpt(const uint8_t* data, size_t n, std::vector<uint32_t>& block,
                int* tilesOut, std::string& log) {
  block.clear();
  if (tilesOut) *tilesOut = 0;
  if (n == 0) return true;
  constexpr size_t kHdrBytes = 32;
  constexpr int T = kSculptTileSamples;
  constexpr size_t kRecBytes = 8 + size_t(T) * T * 2;
  auto rd32 = [&](size_t off) { uint32_t v; std::memcpy(&v, data + off, 4); return v; };
  if (n < kHdrBytes) { log += "sculpt: file is " + std::to_string(n) + " bytes, shorter than its header\n"; return false; }
  if (rd32(0) != kSculptFileMagic || rd32(4) != kSculptVersion) { log += "sculpt: bad magic/version\n"; return false; }
  if (rd32(8) != kSculptSpacingLog2 || rd32(12) != kSculptTileLog2) {
    log += "sculpt: sample spacing log2 " + std::to_string(rd32(8)) + " / tile log2 " + std::to_string(rd32(12)) +
           " -- this build reads " + std::to_string(kSculptSpacingLog2) + " / " + std::to_string(kSculptTileLog2) + "\n";
    return false;
  }
  const uint32_t count = rd32(16);
  if (n != kHdrBytes + size_t(count) * kRecBytes) {
    log += "sculpt: " + std::to_string(count) + " tiles need " + std::to_string(kHdrBytes + size_t(count) * kRecBytes) +
           " bytes, the file has " + std::to_string(n) + "\n";
    return false;
  }
  // The authored tiles, non-zero only, in a SORTED map: the packed block is a
  // pure function of the file's content, whatever order it lists tiles in.
  using Key = std::pair<int, int>;   // (tz, tx): z-major
  std::map<Key, std::vector<int16_t>> tiles;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* r = data + kHdrBytes + size_t(i) * kRecBytes;
    int32_t tx, tz;
    std::memcpy(&tx, r, 4); std::memcpy(&tz, r + 4, 4);
    std::vector<int16_t> s(size_t(T) * T);
    std::memcpy(s.data(), r + 8, s.size() * 2);
    bool any = false;
    for (int16_t v : s) any = any || v != 0;
    if (!any) continue;
    if (!tiles.emplace(Key{tz, tx}, std::move(s)).second) {
      log += "sculpt: tile (" + std::to_string(tx) + "," + std::to_string(tz) + ") appears twice\n";
      return false;
    }
  }
  if (tiles.empty()) return true;
  auto sampleAt = [&](int sx, int sz) -> int {
    auto it = tiles.find(Key{sz >> kSculptTileLog2, sx >> kSculptTileLog2});
    if (it == tiles.end()) return 0;
    return it->second[size_t(sz & (T - 1)) * T + size_t(sx & (T - 1))];
  };
  // The bodies to materialise: every authored tile and the three whose +1
  // edge reads it (west, north, north-west), kept only if non-zero.
  constexpr int S = kSculptTileSide;
  std::map<Key, std::vector<int>> bodies;
  for (const auto& kv : tiles)
    for (int dz = -1; dz <= 0; dz++)
      for (int dx = -1; dx <= 0; dx++) {
        const Key k{kv.first.first + dz, kv.first.second + dx};
        if (bodies.count(k)) continue;
        std::vector<int> v(size_t(S) * S);
        bool any = false;
        for (int j = 0; j < S; j++)
          for (int i = 0; i < S; i++) {
            const int s = sampleAt(k.second * T + i, k.first * T + j);
            v[size_t(j) * S + i] = s;
            any = any || s != 0;
          }
        if (any) bodies.emplace(k, std::move(v));
      }
  if (bodies.empty()) return true;
  int rx0 = INT32_MAX, rz0 = INT32_MAX, rx1 = INT32_MIN, rz1 = INT32_MIN;
  for (const auto& kv : bodies) {
    const int rx = kv.first.second >> kSculptRegionLog2, rz = kv.first.first >> kSculptRegionLog2;
    rx0 = std::min(rx0, rx); rx1 = std::max(rx1, rx);
    rz0 = std::min(rz0, rz); rz1 = std::max(rz1, rz);
  }
  const int64_t rw = int64_t(rx1) - rx0 + 1, rh = int64_t(rz1) - rz0 + 1;
  if (rw * rh > kSculptMaxRegions) {
    log += "sculpt: the tiles span " + std::to_string(rw) + " x " + std::to_string(rh) +
           " directory regions (limit " + std::to_string(kSculptMaxRegions) + " entries)\n";
    return false;
  }
  block.assign(kSculptHdrWords + size_t(rw * rh), 0u);
  block[kSc_RegionX0] = U(rx0);
  block[kSc_RegionZ0] = U(rz0);
  block[kSc_RegionW] = U(static_cast<int>(rw));
  block[kSc_RegionH] = U(static_cast<int>(rh));
  constexpr int R = 1 << kSculptRegionLog2;
  for (const auto& kv : bodies) {
    const int tx = kv.first.second, tz = kv.first.first;
    const size_t e = kSculptHdrWords + size_t((tz >> kSculptRegionLog2) - rz0) * size_t(rw) +
                     size_t((tx >> kSculptRegionLog2) - rx0);
    if (block[e] == 0u) {
      block[e] = U(static_cast<int>(block.size()));
      block.resize(block.size() + size_t(R) * R, 0u);
    }
    const size_t slot = block[e] + size_t(tz & (R - 1)) * R + size_t(tx & (R - 1));
    const uint32_t body = U(static_cast<int>(block.size()));
    block.resize(block.size() + kSculptTileWords, 0u);
    block[slot] = body;
    for (int k = 0; k < S * S; k++)
      block[body + (k >> 1)] |= (static_cast<uint32_t>(kv.second[size_t(k)]) & 0xFFFFu) << ((k & 1) * 16);
  }
  if (tilesOut) *tilesOut = static_cast<int>(tiles.size());
  return true;
}

bool LoadWorldMap(const std::string& assetDir, const std::string& name,
                  const biomes::BiomeSet& set, size_t materialCount, uint32_t seed,
                  WorldMapData& out, std::string& log, const std::string* mapJson) {
  out = WorldMapData{};
  const std::string dir = assetDir + "/worldmap/" + name;
  const std::string at = "worldmap/" + name + ": ";
  // What the loader skips or doubts but will not refuse a boot over: said on
  // stderr AND kept on the map, where --mapcheck hands it to the tuner.
  auto warn = [&](const std::string& m) {
    std::fprintf(stderr, "world map WARNING: %s%s\n", at.c_str(), m.c_str());
    out.warnings.push_back(m);
  };

  // ---- map.json -------------------------------------------------------------
  json j;
  if (mapJson) {
    try { j = json::parse(*mapJson); } catch (const std::exception& e) {
      log += at + "the supplied map.json does not parse: " + e.what() + "\n"; return false;
    }
  } else {
    std::ifstream f(dir + "/map.json");
    if (!f) { log += at + "map.json not found (" + dir + ")\n"; return false; }
    try { f >> j; } catch (const std::exception& e) {
      log += at + "map.json does not parse: " + e.what() + "\n"; return false;
    }
  }
  auto geti = [&](const char* k, int d) { return j.contains(k) && j[k].is_number() ? j[k].get<int>() : d; };
  out.name = name;
  // P7: the edit layer is the map's (was tuning.json world.editLayer). A bare
  // name: worldedit.cpp joins it under assets/worldedits/.
  if (j.contains("editLayer") && j["editLayer"].is_string()) {
    out.editLayer = j["editLayer"].get<std::string>();
    if (out.editLayer.find_first_of("/\\:.") != std::string::npos) {
      log += at + "editLayer must be a bare layer name; ignored\n";
      out.editLayer.clear();
    }
  }
  // ---- terrain (P-G): the numbers that used to be worldgen.* --------------------
  // Every length is authored at `refVoxelsPerMetre` voxels to the metre and
  // rescaled to world.h's kVoxelsPerMetre here, exactly as LoadTuning used
  // to rescale the rows: (v * live) / ref, so the shipped case is exact.
  // Log2 cells shift by the ratio's log2. Counts and Q8 ratios are untouched.
  {
    const json te = j.contains("terrain") && j["terrain"].is_object() ? j["terrain"] : json::object();
    TerrainParams& t = out.terrain;
    auto num = [&](const char* k, int d) { return te.contains(k) && te[k].is_number() ? te[k].get<int>() : d; };
    t.refVoxelsPerMetre = std::max(1, num("refVoxelsPerMetre", t.refVoxelsPerMetre));
    const int ref = t.refVoxelsPerMetre;
    auto len = [&](const char* k, int d) { return (num(k, d) * kVoxelsPerMetre) / ref; };
    int cellShift = 0;
    for (int r = kVoxelsPerMetre; r > ref; r /= 2) cellShift++;
    for (int r = ref; r > kVoxelsPerMetre; r /= 2) cellShift--;
    auto log2c = [&](const char* k, int d) { return std::clamp(num(k, d) + cellShift, 3, 15); };
    t.baseHeight = len("baseHeight", t.baseHeight);
    t.landformRangeVox = len("landformRangeVox", t.landformRangeVox);
    t.rangeAmplitude = len("rangeAmplitude", t.rangeAmplitude);
    t.rangeLog2 = log2c("rangeLog2", t.rangeLog2);
    t.hillAmplitude = len("hillAmplitude", t.hillAmplitude);
    t.hillLog2 = log2c("hillLog2", t.hillLog2);
    t.detailAmplitude = len("detailAmplitude", t.detailAmplitude);
    t.detailLog2 = log2c("detailLog2", t.detailLog2);
    t.grainAmplitude = len("grainAmplitude", t.grainAmplitude);
    t.grainLog2 = log2c("grainLog2", t.grainLog2);
    t.fbmAtten = std::clamp(num("fbmAtten", t.fbmAtten), 0, 256);
    const json ha = te.contains("homeArea") && te["homeArea"].is_object() ? te["homeArea"] : json::object();
    auto hnum = [&](const char* k, int d) { return ha.contains(k) && ha[k].is_number() ? ha[k].get<int>() : d; };
    t.homeY = (hnum("y", t.homeY) * kVoxelsPerMetre) / ref;
    t.homeR = std::max(0, (hnum("radius", t.homeR) * kVoxelsPerMetre) / ref);
    // A fade of 0 would divide by zero in landAt and make the home area's
    // boundary a STEP of the whole coarse relief (the terrain gate's A4).
    t.homeFade = std::max(1, (hnum("fade", t.homeFade) * kVoxelsPerMetre) / ref);
    t.sedCeil = len("sedCeil", t.sedCeil);
    t.sedFraction = std::max(0, num("sedFraction", t.sedFraction));
    t.sedStrip = std::max(0, len("sedStrip", t.sedStrip));
    t.sedSlope = std::max(0, num("sedSlope", t.sedSlope));
    t.sedMax = std::max(0, len("sedMax", t.sedMax));
    // THE CAVE SHELL: caveBands caps the near-surface cavern at h - 40 (a
    // shader literal scaled by vlen); a wedge thicker than that is undercut
    // and drops a column of loose powder into it -- `ca-skip` finds the world
    // never quiet three gates away.
    {
      const int shell = (36 * kVoxelsPerMetre) / ref;
      if (t.sedMax > shell) {
        std::printf("world map: '%s' terrain.sedMax %d reaches into the cave shell; clamped to %d\n", name.c_str(), t.sedMax, shell);
        t.sedMax = shell;
      }
    }
    t.sedTopsoil = std::clamp(len("sedTopsoil", t.sedTopsoil), 0, t.sedMax);
    t.treeline = len("treeline", t.treeline);
    TerrainWords(t, out.terrainWords);
  }
  out.cellLog2 = geti("cellLog2", 10);
  out.seaLevelY = geti("seaLevelY", 0);
  out.oceanFadeCells = geti("oceanFadeCells", 0);
  out.warpAmpVox = geti("warpAmpVox", 0);
  if (j.contains("size") && j["size"].is_array() && j["size"].size() == 2) {
    out.width = j["size"][0].get<int>(); out.height = j["size"][1].get<int>();
  }
  if (j.contains("originCell") && j["originCell"].is_array() && j["originCell"].size() == 2) {
    out.originCellX = j["originCell"][0].get<int>(); out.originCellZ = j["originCell"][1].get<int>();
  }
  if (out.cellLog2 < 4 || out.cellLog2 > 14 || out.width <= 0 || out.height <= 0 ||
      out.width > 4096 || out.height > 4096) {
    log += at + "bad cellLog2/size\n"; return false;
  }
  // The warp must stay well inside a cell or a one-cell region pinches below
  // the tree-tile width the shader's comments require (plan: <= cell/6).
  const int cellVox = 1 << out.cellLog2;
  if (out.warpAmpVox < 0 || out.warpAmpVox > cellVox / 4) {
    log += at + "warpAmpVox " + std::to_string(out.warpAmpVox) + " exceeds cell/4 (" +
           std::to_string(cellVox / 4) + ")\n";
    return false;
  }

  // ---- the palette, resolved against the biome files ------------------------
  std::unordered_map<std::string, int> idOf;
  for (const biomes::BiomeDef& b : set.biomes) idOf[b.name] = b.index;
  std::vector<uint8_t> paletteId;
  if (!j.contains("biomes") || !j["biomes"].is_array() || j["biomes"].empty()) {
    log += at + "map.json needs a non-empty biomes[] palette\n"; return false;
  }
  for (const json& e : j["biomes"]) {
    const std::string n = e.is_string() ? e.get<std::string>() : "";
    auto it = idOf.find(n);
    if (it == idOf.end()) {
      log += at + "palette names biome \"" + n + "\", which has no assets/biomes/<name>.json\n";
      return false;
    }
    out.palette.push_back(n);
    paletteId.push_back(static_cast<uint8_t>(it->second));
  }
  auto oc = idOf.find("ocean");
  out.oceanBiome = oc == idOf.end() ? 0 : oc->second;

  // ---- stamp templates: one .vox load + pack per (template, rotation) --------
  // A missing template is a broken AUTHORED reference; refuse, the way a
  // biome naming a species with no atlas refuses.
  std::unordered_map<std::string, WorldMapData::StampSite> stampCache;
  auto loadStamp = [&](WorldMapData::StampSite& st, const std::string& id) -> bool {
    const std::string key = st.templateName + "#" + std::to_string(st.rot);
    auto it = stampCache.find(key);
    if (it == stampCache.end()) {
      Prefab pf;
      std::string err, voxWarn;
      const std::string vox = assetDir + "/prefabs/" + st.templateName + ".vox";
      if (!LoadVoxFile(vox, materialCount, pf, err, voxWarn)) {
        log += at + "site \"" + id + "\": " + vox + " did not load: " + err + "\n";
        return false;
      }
      WorldMapData::StampSite packed;
      if (!PackStamp(pf, st.rot, packed, log)) return false;
      it = stampCache.emplace(key, std::move(packed)).first;
    }
    st.nx = it->second.nx; st.ny = it->second.ny; st.nz = it->second.nz;
    st.words = it->second.words;
    st.radius = std::max(st.nx, st.nz) / 2 + 1;
    return true;
  };

  // ---- tree species (P6): the atlas's order, header only, read on first use ----
  // A tree site names a species; the shader needs the atlas's INDEX for it and
  // the index plane needs its reach (and the widest reach of all, for how far
  // the lattice's own trunks are kept away). Headers only: 32 words a file.
  std::vector<TreeSpeciesHeader> treeSpecies;
  bool treeSpeciesRead = false;
  int treeMaxReach = 0;
  auto readTreeSpecies = [&]() -> bool {
    if (treeSpeciesRead) return true;
    std::string tl;
    if (!ReadTreeSpeciesHeaders(assetDir + "/trees", treeSpecies, tl)) { log += at + tl; return false; }
    for (const TreeSpeciesHeader& h : treeSpecies) treeMaxReach = std::max(treeMaxReach, h.reach);
    treeSpeciesRead = true;
    return true;
  };
  // `at: [x, z]` in world voxels, the one spelling of a site's column. A stamp
  // also takes the pre-P6 `x` / `z` pair so old files keep loading.
  auto readAt = [](const json& s, int* x, int* z) -> bool {
    if (!(s.contains("at") && s["at"].is_array() && s["at"].size() == 2 &&
          s["at"][0].is_number() && s["at"][1].is_number())) return false;
    *x = s["at"][0].get<int>(); *z = s["at"][1].get<int>();
    return true;
  };

  // ---- sites: the pad box, the spawn, stamps, lakes, landforms, trees ---------
  bool havePad = false;
  if (j.contains("sites") && j["sites"].is_array()) {
    for (const json& s : j["sites"]) {
      if (!s.is_object()) { warn("a sites[] entry that is not an object -- ignored"); continue; }
      const std::string kind = s.value("kind", "");
      const std::string id = s.value("id", "?");
      if (kind == "spawn") {
        // ONE per map: two spawns is an authoring error, not a choice, and
        // "the first one wins" would make the map page's marker a lie.
        if (out.spawnAuthored) {
          log += at + "site \"" + id + "\": a second kind spawn (the map already has one at (" +
                 std::to_string(out.spawnX) + "," + std::to_string(out.spawnZ) + "))\n";
          return false;
        }
        if (!(s.contains("at") && s["at"].is_array() && s["at"].size() == 2 &&
              s["at"][0].is_number() && s["at"][1].is_number())) {
          log += at + "site \"" + id + "\" kind spawn needs at[2] in world voxels\n";
          return false;
        }
        out.spawnX = s["at"][0].get<int>();
        out.spawnZ = s["at"][1].get<int>();
        out.spawnAuthored = true;
      } else if (kind == "pad") {
        if (havePad) {   // one pad box until the pad becomes a site kind proper
          warn("site \"" + id + "\": a second kind pad -- ignored (a map has one pad box)");
          continue;
        }
        if (!(s.contains("min") && s.contains("max") && s["min"].is_array() &&
              s["max"].is_array() && s["min"].size() == 2 && s["max"].size() == 2)) {
          log += at + "site \"" + id + "\" kind pad needs min[2]/max[2] in world voxels\n";
          return false;
        }
        out.padX0 = s["min"][0].get<int>(); out.padZ0 = s["min"][1].get<int>();
        out.padX1 = s["max"][0].get<int>(); out.padZ1 = s["max"][1].get<int>();
        havePad = true;
      } else if (kind == "stamp") {
        WorldMapData::StampSite st;
        st.id = id;
        st.templateName = s.value("template", "");
        if (!readAt(s, &st.x, &st.z)) {
          if (!(s.contains("x") && s.contains("z") && s["x"].is_number() && s["z"].is_number())) {
            log += at + "site \"" + id + "\" kind stamp needs at[2] in world voxels\n";
            return false;
          }
          st.x = s["x"].get<int>(); st.z = s["z"].get<int>();   // the pre-P6 spelling
        }
        st.rot = s.value("rot", 0) & 3;
        st.padMargin = std::clamp(s.value("padMargin", 8), 1, 64);
        st.apron = std::clamp(s.value("padApron", 0), 0, 32);
        st.salt = static_cast<uint32_t>(s.value("salt", 0));
        if (st.templateName.empty()) {
          log += at + "site \"" + id + "\" kind stamp needs a template name (assets/prefabs/<name>.vox)\n";
          return false;
        }
        // A template that is not THERE is a warning and a skipped site, not
        // a refused boot: one stale stamp should not stop the whole world
        // (the tuner's picker only offers files that exist). A template that
        // is there and does not load is still a refusal, below.
        {
          std::error_code ec;
          if (!std::filesystem::exists(assetDir + "/prefabs/" + st.templateName + ".vox", ec)) {
            warn("site \"" + id + "\" names assets/prefabs/" + st.templateName +
                 ".vox, which does not exist -- site skipped");
            continue;
          }
        }
        if (!loadStamp(st, id)) return false;
        if (s.contains("radius") && s["radius"].is_number()) st.radius = std::max(st.radius, s["radius"].get<int>());
        out.sites.push_back(std::move(st));
      } else if (kind == "landform") {
        // P-G: a DECLARED landform, Tier A. Overlaid onto the plane once the
        // planes are read (below); never in the site table.
        LandformSite ls;
        ls.id = id;
        ls.shape = s.value("shape", "peak");
        if (ls.shape != "peak" && ls.shape != "ridge" && ls.shape != "basin" && ls.shape != "plateau") {
          log += at + "site \"" + id + "\" kind landform: shape must be peak | ridge | basin | plateau, not \"" + ls.shape + "\"\n";
          return false;
        }
        if (!(s.contains("at") && s["at"].is_array() && s["at"].size() == 2 &&
              s["at"][0].is_number() && s["at"][1].is_number())) {
          log += at + "site \"" + id + "\" kind landform needs at[2] in world voxels\n";
          return false;
        }
        ls.x = s["at"][0].get<int>(); ls.z = s["at"][1].get<int>();
        ls.radius = std::clamp(s.value("radius", 1024), 1, 1 << 20);
        ls.heightVox = std::clamp(s.value("heightVox", 0), -100000, 100000);
        ls.rotation = s.value("rotation", 0);
        out.landformSites.push_back(std::move(ls));
      } else if (kind == "water") {
        // P-F: an AUTHORED LAKE. Tier A -- its centre is the map's, its
        // geometry the preset's (radius overridable, in world voxels), and it
        // is a site record so the shader and the CPU twin find it through
        // the per-cell index plane like a stamp. A preset the set does not
        // have is a broken authored reference: refuse, like a missing .vox.
        WorldMapData::StampSite st;
        st.id = id;
        st.kind = kSiteWater;
        st.templateName = s.value("preset", "");
        if (!(s.contains("at") && s["at"].is_array() && s["at"].size() == 2 &&
              s["at"][0].is_number() && s["at"][1].is_number())) {
          log += at + "site \"" + id + "\" kind water needs at[2] in world voxels\n";
          return false;
        }
        st.x = s["at"][0].get<int>(); st.z = s["at"][1].get<int>();
        st.preset = static_cast<int>(WaterPresetIndex(set, st.templateName));
        if (st.preset == 0) {
          log += at + "site \"" + id + "\" kind water names preset \"" + st.templateName +
                 "\", which has no assets/water/<name>.json\n";
          return false;
        }
        const WaterGeom g = WaterGeomOf(set.water[static_cast<size_t>(st.preset - 1)]);
        st.radius = g.radiusMin + (g.radiusSpan - 1) / 2;   // the preset's authored radius
        if (s.contains("radius") && s["radius"].is_number())
          st.radius = std::clamp(s["radius"].get<int>(), 4, 2048);
        st.padMargin = std::max(1, g.band);                  // the index plane's reach past the disc
        st.salt = 0;
        out.sites.push_back(std::move(st));
      } else if (kind == "tree") {
        // P6: ONE AUTHORED TREE, Tier A in where (and, if the map says, which
        // variant and which way round); a rolled variant / turn is Tier B. A
        // species with no .svtree is skipped with a warning, like a stamp
        // whose .vox has gone: one stale tree must not stop the world.
        WorldMapData::StampSite st;
        st.id = id;
        st.kind = kSiteTree;
        st.templateName = s.value("species", "");
        if (!readAt(s, &st.x, &st.z)) {
          log += at + "site \"" + id + "\" kind tree needs at[2] in world voxels\n";
          return false;
        }
        if (!readTreeSpecies()) return false;
        const TreeSpeciesHeader* sp = nullptr;
        for (const TreeSpeciesHeader& h : treeSpecies)
          if (h.name == st.templateName) sp = &h;
        if (!sp) {
          warn("site \"" + id + "\" names tree species \"" + st.templateName +
               "\", which has no assets/trees/<name>.svtree -- site skipped");
          continue;
        }
        st.species = sp->index;
        st.radius = std::max(1, sp->reach);
        st.padMargin = 0;
        st.variant = 0;
        if (s.contains("variant") && s["variant"].is_number_integer()) {
          const int v = s["variant"].get<int>();
          const int n = std::max(1, sp->variants);
          if (v < 0 || v >= n)
            warn("site \"" + id + "\": " + st.templateName + " has " + std::to_string(n) +
                 " variants, not a variant " + std::to_string(v) + " -- using " + std::to_string(((v % n) + n) % n));
          st.variant = 1 + ((v % n) + n) % n;
        }
        st.rot = (s.contains("rot") && s["rot"].is_number_integer()) ? (s["rot"].get<int>() & 3)
                                                                     : static_cast<int>(kSiteRotRolled);
        st.salt = 0;
        out.sites.push_back(std::move(st));
      } else if (kind == "clearing") {
        // A FOREST CLEARING (worldmap.h kSiteClearing): a box no lattice
        // crown reaches over, the forest thinning across `feather` past it.
        // A box whose corners are the wrong way round is a refusal naming
        // the site (the map page cannot draw one, so a hand edit made it).
        WorldMapData::StampSite st;
        st.id = id;
        st.kind = kSiteClearing;
        if (!(s.contains("min") && s.contains("max") && s["min"].is_array() && s["max"].is_array() &&
              s["min"].size() == 2 && s["max"].size() == 2 && s["min"][0].is_number() &&
              s["min"][1].is_number() && s["max"][0].is_number() && s["max"][1].is_number())) {
          log += at + "site \"" + id + "\" kind clearing needs min[2]/max[2] in world voxels\n";
          return false;
        }
        st.x = s["min"][0].get<int>(); st.z = s["min"][1].get<int>();
        st.x1 = s["max"][0].get<int>(); st.z1 = s["max"][1].get<int>();
        if (st.x1 < st.x || st.z1 < st.z) {
          log += at + "site \"" + id + "\" kind clearing: min (" + std::to_string(st.x) + "," +
                 std::to_string(st.z) + ") is not below max (" + std::to_string(st.x1) + "," +
                 std::to_string(st.z1) + ") on both axes\n";
          return false;
        }
        const int fe = s.value("feather", static_cast<int>(kClearingFeatherDefault));
        st.padMargin = std::clamp(fe, 0, static_cast<int>(kClearingFeatherMax));
        if (fe != st.padMargin)
          warn("site \"" + id + "\": feather " + std::to_string(fe) + " clamped to " +
               std::to_string(st.padMargin) + " (0.." + std::to_string(kClearingFeatherMax) + ")");
        st.radius = 0;
        st.salt = 0;
        if (!readTreeSpecies()) return false;   // the index reach needs the widest crown
        out.sites.push_back(std::move(st));
      } else if (kind == "soften") {
        // SOFTENED GROUND (worldmap.h kSiteSoften): the local octaves scaled
        // inside a box, easing back across the feather. Same box rules as a
        // clearing: corners the wrong way round are a refusal naming it.
        WorldMapData::StampSite st;
        st.id = id;
        st.kind = kSiteSoften;
        if (!(s.contains("min") && s.contains("max") && s["min"].is_array() && s["max"].is_array() &&
              s["min"].size() == 2 && s["max"].size() == 2 && s["min"][0].is_number() &&
              s["min"][1].is_number() && s["max"][0].is_number() && s["max"][1].is_number())) {
          log += at + "site \"" + id + "\" kind soften needs min[2]/max[2] in world voxels\n";
          return false;
        }
        st.x = s["min"][0].get<int>(); st.z = s["min"][1].get<int>();
        st.x1 = s["max"][0].get<int>(); st.z1 = s["max"][1].get<int>();
        if (st.x1 < st.x || st.z1 < st.z) {
          log += at + "site \"" + id + "\" kind soften: min (" + std::to_string(st.x) + "," +
                 std::to_string(st.z) + ") is not below max (" + std::to_string(st.x1) + "," +
                 std::to_string(st.z1) + ") on both axes\n";
          return false;
        }
        const int fe = s.value("feather", static_cast<int>(kSoftenFeatherDefault));
        st.padMargin = std::clamp(fe, 0, static_cast<int>(kClearingFeatherMax));
        if (fe != st.padMargin)
          warn("site \"" + id + "\": feather " + std::to_string(fe) + " clamped to " +
               std::to_string(st.padMargin) + " (0.." + std::to_string(kClearingFeatherMax) + ")");
        // Percent of the biome's own amplitude -> Q8. Out of 0..100 is a
        // warning and a clamp (more than the biome's own relief is what the
        // sculpt layer and the biome's terrain block are for).
        auto pct = [&](const char* key, int def) -> int {
          int v = def;
          if (s.contains(key) && s[key].is_number()) v = static_cast<int>(std::lround(s[key].get<double>()));
          const int c = std::clamp(v, 0, 100);
          if (c != v) warn("site \"" + id + "\": " + key + " " + std::to_string(v) + " clamped to " + std::to_string(c) + " (0..100 %)");
          return (c * 256 + 50) / 100;
        };
        st.softHill = pct("hills", static_cast<int>(kSoftenHillDefault));
        st.softBump = pct("bumps", static_cast<int>(kSoftenBumpDefault));
        st.softGrain = pct("grain", static_cast<int>(kSoftenGrainDefault));
        st.softPath = pct("paths", static_cast<int>(kSoftenPathsDefault));
        st.radius = 0;
        st.salt = 0;
        out.sites.push_back(std::move(st));
      } else {
        warn("site \"" + id + "\": unknown kind \"" + kind +
             "\" -- ignored (the loader knows spawn, pad, stamp, water, landform, tree, clearing, soften)");
      }
    }
  }
  // ---- structure refs (PLAN_world_editor P4, world/structures.h) -------------
  // The map's `structure` refs, read from its refs group files (the SAME parse
  // the RefStore uses) -- or the gates' override -- each one a stamp site:
  // the asset's voxels packed at the ref's quarter turn, centred so the
  // asset's ORIGIN lands on the ref's pos, the pad levelled to the AUTHORED
  // floor (pos.y - 1) and the footing sunk `origin.y` rows into it. After
  // the map.json sites (a hand-placed map site wins an overlap, as the
  // earlier site in the table always has) and BEFORE the rules, so a rolled
  // crypt keeps its minSpacing from a house. A structure that cannot be
  // placed (no asset, a yaw that is not a quarter turn, a scale mismatch) is
  // a warning naming the ref and the file, and the world boots without it:
  // one stale house must not stop the world.
  {
    std::vector<StructurePlacement> placements;
    std::vector<std::string> swarn;
    if (StructureOverrideActive()) {
      placements = *StructureOverrideSlot();
    } else {
      structures::ReadPlacements(assetDir, name, placements, swarn);
    }
    for (const std::string& w : swarn) warn(w);
    std::map<std::string, structures::Asset> assets;   // one load per base
    for (const StructurePlacement& sp : placements) {
      const std::string who = sp.file + ": " + sp.id + ": ";
      if (!structures::ValidYaw(sp.yaw)) {
        warn(who + "yaw: " + std::to_string(sp.yaw) +
             " is not a quarter turn (0, 90, 180, 270); structures only turn in 90-degree "
             "steps in this slice -- structure not placed");
        continue;
      }
      auto it = assets.find(sp.base);
      if (it == assets.end()) {
        structures::Asset a;
        std::string err;
        std::vector<std::string> aw;
        if (!structures::LoadAsset(assetDir, sp.base, true, a, err, aw)) {
          for (const std::string& w : aw) warn(w);
          warn(who + "base: " + err + " -- structure not placed");
          continue;
        }
        for (const std::string& w : aw) warn(w);
        it = assets.emplace(sp.base, std::move(a)).first;
      }
      const structures::Asset& a = it->second;
      if (a.voxelsPerMetre != 0 && a.voxelsPerMetre != kVoxelsPerMetre) {
        warn(who + "base: \"" + sp.base + "\" was built at " + std::to_string(a.voxelsPerMetre) +
             " voxels/m and this world is " + std::to_string(kVoxelsPerMetre) +
             " -- re-bake it (scripts/bake_structure.mjs); structure not placed");
        continue;
      }
      const structures::Frame f = structures::MakeFrame(a, IVec3{sp.x, sp.y, sp.z}, sp.yaw);
      WorldMapData::StampSite st;
      st.id = sp.id;
      st.templateName = "structures/" + sp.base;
      st.kind = kSiteStamp;
      st.structure = true;
      st.base = sp.base;
      st.refX = sp.x; st.refY = sp.y; st.refZ = sp.z; st.refYaw = sp.yaw;
      st.x = f.siteX;
      st.z = f.siteZ;
      st.rot = f.rot;
      st.padMargin = sp.padMargin > 0 ? std::clamp(sp.padMargin, 1, 64) : kStructurePadMargin;
      st.apron = sp.padApron >= 0 ? std::clamp(sp.padApron, 0, 32) : kStructurePadApron;
      st.refPadMargin = sp.padMargin;
      st.refPadApron = sp.padApron;
      st.salt = 0;
      st.padY = sp.y - 1;   // the AUTHORED floor: ground top is one below GRADE
      st.sink = f.sink;
      if (st.sink > static_cast<int>(kStampSinkMax)) {
        warn(who + "base: \"" + sp.base + "\" has " + std::to_string(st.sink) +
             " rows below its origin; only " + std::to_string(kStampSinkMax) +
             " can sink into the ground -- the lowest rows are not drawn");
        st.sink = static_cast<int>(kStampSinkMax);
      }
      if (!PackStamp(a.prefab, st.rot, st, log)) return false;
      st.radius = std::max(st.nx, st.nz) / 2 + 1;
      out.sites.push_back(std::move(st));
    }
    if (!placements.empty())
      std::printf("world map: '%s' %zu structure ref(s) -> stamp sites\n", name.c_str(), placements.size());
    // What the save fingerprint / env stamp sees: the placements and every
    // placed asset's bytes (folded into contentHash below, only when any).
    uint32_t sh = 0;
    for (const auto& [b, a] : assets) sh = (sh ^ a.contentHash) * 16777619u;
    for (const StructurePlacement& sp : placements) {
      const std::string k = sp.id + "|" + sp.base + "|" + std::to_string(sp.x) + "," +
                            std::to_string(sp.y) + "," + std::to_string(sp.z) + "|" +
                            std::to_string(sp.yaw) + "|" + std::to_string(sp.padMargin) + "|" +
                            std::to_string(sp.padApron);
      sh = FnvBytes(sh == 0 ? 2166136261u : sh, reinterpret_cast<const uint8_t*>(k.data()), k.size());
    }
    // THE WALKING ROUTES (worldmap.h kSitePath): every waynode link of the
    // map's refs outside the houses becomes a trunk keep-out, so the forest
    // that now stands in a village (no clearing needed) never puts a trunk
    // on a villager's straight line. Read from the files like the
    // placements; a gate's structure override has no routes.
    if (!StructureOverrideActive()) {
      std::vector<structures::WaySegment> segs;
      structures::ReadWaySegments(assetDir, name, segs);
      auto inHouse = [&](int x, int z) {
        for (const WorldMapData::StampSite& h : out.sites)
          if (h.structure && x >= h.x - h.nx / 2 && x < h.x - h.nx / 2 + h.nx &&
              z >= h.z - h.nz / 2 && z < h.z - h.nz / 2 + h.nz)
            return true;
        return false;
      };
      int nPath = 0;
      for (const structures::WaySegment& g : segs) {
        // A link with both ends indoors is a house's own route: no trunk can
        // stand inside a house, so it needs no keep-out (and no index words).
        if (inHouse(g.ax, g.az) && inHouse(g.bx, g.bz)) continue;
        // Pieces of at most 256 voxels a side: the shader's projection
        // (worldgen.wgsl routeNear) then stays far inside i32.
        const int len = std::max(std::abs(g.bx - g.ax), std::abs(g.bz - g.az));
        const int pieces = std::max(1, (len + 255) / 256);
        for (int q = 0; q < pieces; q++) {
          WorldMapData::StampSite st;
          st.id = "route " + g.a + " - " + g.b + (pieces > 1 ? " #" + std::to_string(q + 1) : std::string());
          st.kind = kSitePath;
          st.x = g.ax + (g.bx - g.ax) * q / pieces;
          st.z = g.az + (g.bz - g.az) * q / pieces;
          st.x1 = g.ax + (g.bx - g.ax) * (q + 1) / pieces;
          st.z1 = g.az + (g.bz - g.az) * (q + 1) / pieces;
          st.padMargin = kPathTrunkKeepOut;
          st.radius = 0;
          st.salt = 0;
          out.sites.push_back(std::move(st));
        }
        nPath++;
        const std::string k = "route|" + g.a + "|" + g.b + "|" + std::to_string(g.ax) + "," + std::to_string(g.az) +
                              "|" + std::to_string(g.bx) + "," + std::to_string(g.bz);
        sh = FnvBytes(sh == 0 ? 2166136261u : sh, reinterpret_cast<const uint8_t*>(k.data()), k.size());
      }
      if (nPath > 0) std::printf("world map: '%s' %d waynode link(s) keep trunks off their line\n", name.c_str(), nPath);
    }
    out.structureHash = sh;
  }

  // ---- the water table for the CPU twin (worldmap.h WorldMapData::water) ----
  // The same WaterGeomOf / PackWaterRows the packer runs, kept here so
  // World::TerrainHeight reads the integers the shader reads.
  out.water.clear();
  for (const biomes::WaterPresetDef& w : set.water) out.water.push_back(WaterGeomOf(w));
  out.pondTile = PondLatticeVox(set);
  out.pondBand = PondBandVox(set);
  out.biomeWater.assign(set.biomes.size(), {});
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.index >= 0 && b.index < static_cast<int>(set.biomes.size()))
      out.biomeWater[static_cast<size_t>(b.index)] = PackWaterRows(set, b, out.pondTile);
  // A map that names no spawn starts where every map did before P-C. Said
  // out loud, because a player standing on the harness pad's bare grass is
  // otherwise indistinguishable from a biome that failed to author.
  if (!out.spawnAuthored)
    std::printf("world map: '%s' has no kind \"spawn\" site; defaulting spawn to (%d,%d)\n",
                name.c_str(), out.spawnX, out.spawnZ);
  // Rules are resolved after the planes are read (they need the biome plane);
  // see below.

  // ---- map.svmap --------------------------------------------------------------
  std::vector<uint8_t> raw;
  {
    std::ifstream f(dir + "/map.svmap", std::ios::binary);
    if (!f) { log += at + "map.svmap not found\n"; return false; }
    std::ostringstream ss; ss << f.rdbuf();
    const std::string s = ss.str();
    raw.assign(s.begin(), s.end());
  }
  const size_t cells = static_cast<size_t>(out.width) * out.height;
  if (raw.size() < 16 + cells * 3) {
    log += at + "map.svmap is " + std::to_string(raw.size()) + " bytes; expected " +
           std::to_string(16 + cells * 3) + " for " + std::to_string(out.width) + "x" +
           std::to_string(out.height) + "\n";
    return false;
  }
  uint32_t magic, ver, w, h;
  std::memcpy(&magic, raw.data(), 4); std::memcpy(&ver, raw.data() + 4, 4);
  std::memcpy(&w, raw.data() + 8, 4); std::memcpy(&h, raw.data() + 12, 4);
  if (magic != kMagic || ver != kVersion) { log += at + "map.svmap: bad magic/version\n"; return false; }
  if (w != U(out.width) || h != U(out.height)) {
    log += at + "map.svmap is " + std::to_string(w) + "x" + std::to_string(h) +
           " but map.json says " + std::to_string(out.width) + "x" + std::to_string(out.height) + "\n";
    return false;
  }
  out.biome.resize(cells); out.landform.resize(cells); out.moisture.resize(cells);
  for (size_t i = 0; i < cells; i++) {
    const uint8_t p = raw[16 + i];
    if (p >= paletteId.size()) {
      log += at + "biome plane byte " + std::to_string(p) + " at cell " + std::to_string(i) +
             " is outside the palette (" + std::to_string(paletteId.size()) + " entries)\n";
      return false;
    }
    out.biome[i] = paletteId[p];
  }
  std::memcpy(out.landform.data(), raw.data() + 16 + cells, cells);
  std::memcpy(out.moisture.data(), raw.data() + 16 + cells * 2, cells);
  // ---- sculpt.svsculpt (P5): the sparse height-offset layer, optional --------
  // Read BEFORE the site-pad bake below: a pad's height is the sculpted ground
  // (BareGroundHeight runs the mirror, which reads out.sculpt). Absent = no
  // layer, and nothing downstream sees a difference; malformed = refuse, like
  // a bad map.svmap -- a half-read layer would be a different world.
  std::vector<uint8_t> sculptRaw;
  {
    std::ifstream f(dir + "/sculpt.svsculpt", std::ios::binary);
    if (f) {
      std::ostringstream ss; ss << f.rdbuf();
      const std::string s = ss.str();
      sculptRaw.assign(s.begin(), s.end());
      std::string slog;
      if (!PackSculpt(sculptRaw.data(), sculptRaw.size(), out.sculpt, &out.sculptTiles, slog)) {
        log += at + slog;
        return false;
      }
      std::printf("world map: '%s' sculpt layer: %d tiles, %zu words\n", name.c_str(), out.sculptTiles,
                  out.sculpt.size());
    }
  }
  // ---- the declared landforms, onto the painted plane (P-G) --------------------
  // After the plane is read, before anything samples it. The shader and the
  // CPU twin both read the OVERLAID plane and nothing else: a landform site
  // exists at load and nowhere in the height mirror.
  OverlayLandformSites(out.landformSites, out.terrain.landformRangeVox, out.cellLog2,
                       out.width, out.height, out.originCellX, out.originCellZ, out.landform);
  // ---- each biome's terrain record for the CPU twin (worldmap.h kB_Curve*) ----
  out.biomeTerrain.assign(set.biomes.size(), {});
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.index >= 0 && b.index < static_cast<int>(set.biomes.size()))
      out.biomeTerrain[static_cast<size_t>(b.index)] = PackBiomeTerrain(b);
  out.biomeClimate.assign(set.biomes.size(), {});
  out.biomeName.assign(set.biomes.size(), std::string());
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.index >= 0 && b.index < static_cast<int>(set.biomes.size())) {
      out.biomeClimate[static_cast<size_t>(b.index)] = b.ambient;
      out.biomeName[static_cast<size_t>(b.index)] = b.name;
    }

  // ---- rules (P5b): seeded placement per biome ----------------------------------
  // "3 crypts per km2 of forest, never within 400 m of each other" is one
  // row. Tier B: takes the world seed (and the rule's salt), so every seed
  // puts them somewhere else, but the SAME seed puts them in the same
  // places on every machine -- integer arithmetic throughout, row-major
  // cell order, greedy spacing against sites already placed (hand-placed
  // sites first). Expected sites per cell = perKm2 * cellVox^2 / 1e8 (a km is
  // 10,000 voxels at 0.1 m); the roll is `hash % 1e6 < that * 1e6`.
  if (j.contains("rules") && j["rules"].is_array()) {
    const int cellVox = 1 << out.cellLog2;
    for (const json& ru : j["rules"]) {
      if (!ru.is_object() || ru.value("kind", "") != "stamp") continue;
      const std::string tpl = ru.value("template", "");
      const std::string bname = ru.value("biome", "");
      auto bi = idOf.find(bname);
      if (tpl.empty() || bi == idOf.end()) {
        log += at + "rule \"" + ru.value("id", "?") + "\" needs a template and a biome that has a file\n";
        return false;
      }
      const double perKm2 = ru.value("perKm2", 0.0);
      const int64_t perKm2m = static_cast<int64_t>(std::lround(perKm2 * 1000.0));
      const uint32_t thr = static_cast<uint32_t>(std::min<int64_t>(
          1000000, perKm2m * static_cast<int64_t>(cellVox) * cellVox / 100000));
      const int minSpacing = ru.value("minSpacing", cellVox);
      const int rotRule = ru.value("rot", -1);            // -1 = per-site random
      const int padMargin = std::clamp(ru.value("padMargin", 8), 1, 64);
      const uint32_t salt = static_cast<uint32_t>(ru.value("salt", 0));
      const std::string rid = ru.value("id", tpl);
      int placed = 0;
      for (int cz = 0; cz < out.height; cz++)
        for (int cx = 0; cx < out.width; cx++) {
          const size_t i = static_cast<size_t>(cz) * out.width + cx;
          if (out.biome[i] != static_cast<uint8_t>(bi->second)) continue;
          const uint32_t h = rng::Hash3(seed ^ salt ^ 0x51735u, static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(cz));
          if (h % 1000000u >= thr) continue;
          // the site: cell centre, jittered by up to a quarter cell
          const int jx = static_cast<int>((h >> 20) % static_cast<uint32_t>(cellVox / 2)) - cellVox / 4;
          const int jz = static_cast<int>((h >> 10) % static_cast<uint32_t>(cellVox / 2)) - cellVox / 4;
          const int wx = ((cx - out.originCellX) << out.cellLog2) + cellVox / 2 + jx;
          const int wz = ((cz - out.originCellZ) << out.cellLog2) + cellVox / 2 + jz;
          bool tooClose = false;
          for (const WorldMapData::StampSite& o : out.sites)
            if (o.kind != kSiteClearing &&   // a clearing's (x, z) is a corner, and it keeps nothing out
                std::max(std::abs(o.x - wx), std::abs(o.z - wz)) < minSpacing) { tooClose = true; break; }
          if (tooClose || out.InPadBox(wx, wz)) continue;
          WorldMapData::StampSite st;
          st.id = rid + "_" + std::to_string(placed);
          st.templateName = tpl;
          st.x = wx; st.z = wz;
          st.rot = rotRule < 0 ? static_cast<int>((h >> 5) & 3u) : (rotRule & 3);
          st.padMargin = padMargin;
          st.salt = h;
          if (!loadStamp(st, st.id)) return false;
          out.sites.push_back(std::move(st));
          placed++;
        }
      std::printf("world map: rule '%s' placed %d x %s in %s\n", rid.c_str(), placed, tpl.c_str(), bname.c_str());
    }
  }

  // ---- the site index: every cell a site's REACH touches lists the site ------
  // The reach is the widest thing any reader tests against the site (worldmap.h
  // "the site table"): a stamp's footprint + pad margin; a lake's disc + its
  // shore/berm band, + 1 for the bisection's outer column; a tree's own reach
  // + the WIDEST species' reach, because the lattice keeps its trunks a
  // crown's width (site reach + its own) away and asks from the trunk's cell.
  // A clearing's reach is past its BOX (its x, z is the min corner): the
  // feather + the widest crown, because a trunk that far out still has its
  // crown's gap to the box measured (siteBlocksTrunk asks from the trunk).
  // A STAMP's footprint RECT (worldmap.h "STAMPS carry their FOOTPRINT
  // RECT"): exactly the cells wmStampCell draws into, x - nx/2 .. + nx - 1.
  // Its reach is the pad (rect + apron + margin) or, for the per-tree
  // building rule, the widest crown + kStampTreeClear, whichever is further:
  // the lattice asks from the TRUNK's cell whether its crown meets the rect.
  // A soften box is listed over box + feather; a route over its segment's
  // box + the keep-out.
  bool anyStamp = false;
  for (WorldMapData::StampSite& st : out.sites) {
    if (st.kind != kSiteStamp) continue;
    anyStamp = true;
    st.bx0 = st.x - st.nx / 2;
    st.bz0 = st.z - st.nz / 2;
    st.x1 = st.bx0 + std::max(st.nx, 1) - 1;
    st.z1 = st.bz0 + std::max(st.nz, 1) - 1;
  }
  if (anyStamp && !readTreeSpecies()) return false;
  for (WorldMapData::StampSite& st : out.sites) {
    if (st.kind == kSiteTree) st.indexReach = st.radius + treeMaxReach;
    else if (st.kind == kSiteClearing) st.indexReach = st.padMargin + treeMaxReach + 1;
    else if (st.kind == kSiteSoften) st.indexReach = st.padMargin + 1;
    else if (st.kind == kSitePath) st.indexReach = std::max(st.padMargin, static_cast<int>(kRouteSoftOuter)) + 1;
    else if (st.kind == kSiteStamp)
      st.indexReach = std::max(st.apron + 64, static_cast<int>(kStampTreeClear) + treeMaxReach) + 1;
    else st.indexReach = st.radius + st.padMargin + (st.kind == kSiteWater ? 1 : 0);
  }
  {
    std::map<size_t, std::vector<uint32_t>> byCell;   // ordered: the pack is a pure function of the map
    for (size_t si = 0; si < out.sites.size(); si++) {
      const WorldMapData::StampSite& st = out.sites[si];
      // The listed box: a centre +- reach, or a BOX kind's own corners + reach
      // (a clearing / soften: min .. max; a stamp: its rect; a route: its two
      // ends in either order).
      int lx = st.x, lz = st.z, hx = st.x, hz = st.z;
      if (st.kind == kSiteClearing || st.kind == kSiteSoften || st.kind == kSitePath) {
        lx = std::min(st.x, st.x1); hx = std::max(st.x, st.x1);
        lz = std::min(st.z, st.z1); hz = std::max(st.z, st.z1);
      } else if (st.kind == kSiteStamp) {
        lx = st.bx0; lz = st.bz0; hx = st.x1; hz = st.z1;
      }
      int c0x, c0z, c1x, c1z;
      out.CellOf(lx - st.indexReach, lz - st.indexReach, &c0x, &c0z);
      out.CellOf(hx + st.indexReach, hz + st.indexReach, &c1x, &c1z);
      for (int cz = std::max(c0z, 0); cz <= std::min(c1z, out.height - 1); cz++)
        for (int cx = std::max(c0x, 0); cx <= std::min(c1x, out.width - 1); cx++)
          byCell[static_cast<size_t>(cz) * out.width + cx].push_back(static_cast<uint32_t>(si + 1));
    }
    out.siteIndex.assign(cells, 0u);
    out.siteLists.assign(1, 0u);   // offset 0 = "no site"
    std::map<std::vector<uint32_t>, uint32_t> shared;
    for (const auto& kv : byCell) {
      const std::vector<uint32_t>& ids = kv.second;
      if (ids.size() > kSiteCellMax) {
        const int cx = static_cast<int>(kv.first % static_cast<size_t>(out.width));
        const int cz = static_cast<int>(kv.first / static_cast<size_t>(out.width));
        std::string names;
        for (uint32_t id : ids) names += (names.empty() ? "\"" : ", \"") + out.sites[id - 1].id + "\"";
        log += at + std::to_string(ids.size()) + " sites reach map cell (" + std::to_string(cx) + "," +
               std::to_string(cz) + ") (world x " + std::to_string((cx - out.originCellX) << out.cellLog2) +
               ", z " + std::to_string((cz - out.originCellZ) << out.cellLog2) + ", " + std::to_string(cellVox) +
               " vox square): " + names + "; a cell holds at most " + std::to_string(kSiteCellMax) +
               " -- move one of them\n";
        return false;
      }
      auto it = shared.find(ids);
      if (it == shared.end()) {
        const uint32_t off = static_cast<uint32_t>(out.siteLists.size());
        out.siteLists.push_back(static_cast<uint32_t>(ids.size()));
        out.siteLists.insert(out.siteLists.end(), ids.begin(), ids.end());
        it = shared.emplace(ids, off).first;
      }
      out.siteIndex[kv.first] = it->second;
    }
  }

  // ---- overlap warnings: two sites that claim the same ground ----------------
  // Each kind's FOOTPRINT as its readers test it (a stamp's pad square, a
  // lake's disc + band, a tree's trunk keep-out square). Where two meet the
  // earlier site in the table wins -- the first pad, the first lake -- which
  // is a definite answer and almost never the author's intent, so say it.
  // Only pairs that share a map cell can meet, so the lists are the pair
  // source. The spawn inside a footprint is said too (spawn-site asserts it
  // for the game's map; this names it for every map, on the page).
  {
    // A footprint is a disc (a lake) or an axis box (x0..x1, z0..z1). A
    // stamp is TWO: its FLAT core (rect + apron) and its whole pad (+ the
    // ramp). Two stamps' ramps may meet -- sitePadAt blends them in turn and
    // stays continuous -- but one's ramp must not reach the other's flat
    // core, or that floor tilts; `fpOf(s, core)` picks which.
    struct Fp { bool disc; int64_t x0, z0, x1, z1, r; };
    auto fpOf = [&](const WorldMapData::StampSite& s, bool core) -> Fp {
      if (s.kind == kSiteWater) return {true, s.x, s.z, s.x, s.z, s.radius + s.padMargin};
      if (s.kind == kSiteTree)
        return {false, s.x - kSiteTreeKeepOut, s.z - kSiteTreeKeepOut, s.x + kSiteTreeKeepOut, s.z + kSiteTreeKeepOut, 0};
      const int64_t e = s.apron + (core ? 0 : s.padMargin);
      return {false, s.bx0 - e, s.bz0 - e, s.x1 + e, s.z1 + e, 0};
    };
    auto meets = [](const Fp& a, const Fp& b) -> bool {
      if (a.disc && b.disc) {
        const int64_t dx = a.x0 - b.x0, dz = a.z0 - b.z0;
        return dx * dx + dz * dz < (a.r + b.r) * (a.r + b.r);
      }
      if (!a.disc && !b.disc) return a.x0 <= b.x1 && b.x0 <= a.x1 && a.z0 <= b.z1 && b.z0 <= a.z1;
      const Fp& d = a.disc ? a : b;
      const Fp& q = a.disc ? b : a;
      const int64_t ex = std::max<int64_t>(std::max(q.x0 - d.x0, d.x0 - q.x1), 0);
      const int64_t ez = std::max<int64_t>(std::max(q.z0 - d.z0, d.z0 - q.z1), 0);
      return ex * ex + ez * ez < d.r * d.r;
    };
    // Two stamps: the pad's own octagon distance (worldgen.wgsl stampRectDist)
    // between the rects, less the other's apron, against the ramp's reach.
    // Two pads at the SAME height never tilt each other (the blend of a floor
    // with itself is that floor), so they may overlap freely -- a village's
    // houses all on one level is the common case.
    auto rectGap = [](const WorldMapData::StampSite& a, const WorldMapData::StampSite& b) -> int64_t {
      const int64_t gx = std::max<int64_t>({(int64_t)b.bx0 - a.x1, (int64_t)a.bx0 - b.x1, 0});
      const int64_t gz = std::max<int64_t>({(int64_t)b.bz0 - a.z1, (int64_t)a.bz0 - b.z1, 0});
      return std::max(gx, gz) + (std::min(gx, gz) >> 1);
    };
    auto clash = [&](const WorldMapData::StampSite& a, const WorldMapData::StampSite& b) -> bool {
      if (a.kind == kSiteStamp && b.kind == kSiteStamp) {
        const int64_t g = rectGap(a, b);
        if (g <= a.apron || g <= b.apron) return true;   // the flat cores themselves meet
        if (a.structure && b.structure && a.padY == b.padY) return false;   // map stamps bake padY below
        return g - b.apron < a.apron + a.padMargin || g - a.apron < b.apron + b.padMargin;
      }
      return meets(fpOf(a, false), fpOf(b, false));
    };
    auto kindName = [](int k) { return k == kSiteWater ? "lake" : k == kSiteTree ? "tree" : "stamp"; };
    std::set<std::pair<uint32_t, uint32_t>> seen;
    for (size_t off = 1; off < out.siteLists.size(); off += 1 + out.siteLists[off]) {
      const uint32_t n = out.siteLists[off];
      for (uint32_t a = 0; a < n; a++)
        for (uint32_t b = a + 1; b < n; b++) {
          const uint32_t ia = out.siteLists[off + 1 + a], ib = out.siteLists[off + 1 + b];
          if (!seen.insert({ia, ib}).second) continue;
          const WorldMapData::StampSite& sa = out.sites[ia - 1];
          const WorldMapData::StampSite& sb = out.sites[ib - 1];
          // A clearing claims no ground: it is MEANT to hold houses, lakes
          // and authored trees (two clearings overlapping is one bigger one).
          // Nor does softened ground or a route.
          auto claimsNone = [](int k) { return k == kSiteClearing || k == kSiteSoften || k == kSitePath; };
          if (claimsNone(sa.kind) || claimsNone(sb.kind)) continue;
          if (!clash(sa, sb)) continue;
          warn("site \"" + sa.id + "\" (" + kindName(sa.kind) + " at " + std::to_string(sa.x) + "," +
               std::to_string(sa.z) + ") and site \"" + sb.id + "\" (" + kindName(sb.kind) + " at " +
               std::to_string(sb.x) + "," + std::to_string(sb.z) + ") overlap; where they meet \"" + sa.id +
               "\" wins");
        }
    }
    if (out.spawnAuthored)
      for (const WorldMapData::StampSite& s : out.sites) {
        if (s.kind == kSiteClearing) {
          // Legal (it only keeps crowns off), but the player then wakes in
          // open ground, which is rarely what a map with a forest means.
          if (out.spawnX >= s.x && out.spawnX <= s.x1 && out.spawnZ >= s.z && out.spawnZ <= s.z1)
            warn("the spawn (" + std::to_string(out.spawnX) + "," + std::to_string(out.spawnZ) +
                 ") is inside clearing \"" + s.id + "\" (" + std::to_string(s.x) + "," + std::to_string(s.z) +
                 ")..(" + std::to_string(s.x1) + "," + std::to_string(s.z1) +
                 "): the player starts in the open, not among trees");
          continue;
        }
        if (s.kind == kSiteSoften || s.kind == kSitePath) continue;   // walkable ground, both
        const Fp f = fpOf(s, false);
        const Fp sp{false, out.spawnX, out.spawnZ, out.spawnX, out.spawnZ, 0};
        if (meets(f, sp))
          warn("the spawn (" + std::to_string(out.spawnX) + "," + std::to_string(out.spawnZ) +
               ") is inside site \"" + s.id + "\"'s footprint (" + kindName(s.kind) + ")");
      }
  }

  // ---- the site pads: bare ground at each pad / stamp centre, for `seed` ----
  // sitePadAt, the stamp overlay and the stamp's sky ceiling all need the
  // ground the CENTRE column would have without its pad. It is seed-dependent
  // but position-fixed, so it is computed ONCE here instead of by a
  // landColumnBare per column (sitePadAt) and per voxel (the stamp) on the GPU.
  // The CPU twin reads the CURRENT map, so the map being loaded is installed
  // for the duration and the previous one put back: nothing it computes reads
  // a pad (landColumnBare is the ground BEFORE the pad), so the order of the
  // sites does not matter. Exact because the twin is the height mirror the
  // `terrain` gate holds to the shader per voxel.
  {
    WorldMapData prev = std::move(Slot());
    Slot() = out;
    for (WorldMapData::StampSite& st : out.sites) {
      if (st.structure) continue;   // the AUTHORED floor, set above (P4)
      st.padY = st.kind == kSiteWater || st.kind == kSiteClearing || st.kind == kSiteSoften || st.kind == kSitePath
                    ? 0 : BareGroundHeight(st.x, st.z, seed);
    }
    // A RAMP NEVER STEPS MORE THAN A VOXEL (2026-09-29). The smoothstep's
    // steepest point is 1.5 x the height it makes up over the margin, so a
    // pad cut into a hillside, or raised off a hollow, by more than the
    // margin can ease would step 2+ voxels a column. The authored margin is
    // the MINIMUM: it widens to twice the largest gap between the floor and
    // the bare ground round the pad's outer edge (+2), up to the 64 the prop
    // allows. Harrowby's houses sit within a metre of their floors and keep
    // their authored ramps. The index reach of a stamp already covers 64
    // (kStampTreeClear + the widest crown is further).
    for (WorldMapData::StampSite& st : out.sites) {
      if (st.kind != kSiteStamp) continue;
      const int e = st.apron + st.padMargin;
      int gap = 0;
      auto probe = [&](int x, int z) { gap = std::max(gap, std::abs(BareGroundHeight(x, z, seed) - st.padY)); };
      for (int x = st.bx0 - e; x <= st.x1 + e; x += 8) { probe(x, st.bz0 - e); probe(x, st.z1 + e); }
      for (int z = st.bz0 - e; z <= st.z1 + e; z += 8) { probe(st.bx0 - e, z); probe(st.x1 + e, z); }
      int need = std::min(64, 2 * gap + 2);
      // ...but never so far that it reaches the flat core of a stamp on
      // ANOTHER floor (the overlap warning above judged the authored ramps;
      // a widened one must not tilt a neighbour's floor that was fine).
      for (const WorldMapData::StampSite& o : out.sites) {
        if (&o == &st || o.kind != kSiteStamp || o.padY == st.padY) continue;
        const int gx = std::max({o.bx0 - st.x1, st.bx0 - o.x1, 0});
        const int gz = std::max({o.bz0 - st.z1, st.bz0 - o.z1, 0});
        const int g = std::max(gx, gz) + (std::min(gx, gz) >> 1);
        need = std::min(need, g - o.apron - st.apron - 1);
      }
      if (need > st.padMargin) st.padMargin = need;
    }
    Slot() = std::move(prev);
  }
  // A tree at or above the treeline does not grow (worldgen.wgsl siteTree:
  // treeMaxTop() is the world-wide bound the sky skips rely on). Said, not
  // refused: the author may be about to lower the ground under it.
  for (const WorldMapData::StampSite& st : out.sites)
    if (st.kind == kSiteTree && st.padY >= out.terrain.treeline)
      warn("site \"" + st.id + "\" (tree) stands on ground y" + std::to_string(st.padY) +
           ", at or above the treeline y" + std::to_string(out.terrain.treeline) + ": it will not grow");

  uint32_t hsh = 2166136261u;
  hsh = FnvBytes(hsh, raw.data(), raw.size());
  {
    const std::string js = j.dump();
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(js.data()), js.size());
  }
  // Only when the file exists, so a map without one reports what it did.
  if (!sculptRaw.empty()) hsh = FnvBytes(hsh, sculptRaw.data(), sculptRaw.size());
  // ...and the structure refs, only when there are any, for the same reason.
  if (out.structureHash != 0) {
    const uint32_t sh = out.structureHash;
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(&sh), sizeof sh);
  }
  out.contentHash = hsh;
  return true;
}

std::string MapCheckJson(const std::string& name, bool ok, const WorldMapData& m,
                         const std::string& error) {
  json j;
  j["ok"] = ok;
  j["map"] = name;
  j["sites"] = static_cast<int>(m.sites.size());
  int nStruct = 0;
  for (const WorldMapData::StampSite& s : m.sites) nStruct += s.structure ? 1 : 0;
  j["structures"] = nStruct;           // P4: structure refs placed as sites
  int nClear = 0;
  for (const WorldMapData::StampSite& s : m.sites) nClear += s.kind == kSiteClearing ? 1 : 0;
  j["clearings"] = nClear;
  int nSoft = 0, nRoute = 0;
  for (const WorldMapData::StampSite& s : m.sites) {
    nSoft += s.kind == kSiteSoften ? 1 : 0;
    nRoute += s.kind == kSitePath ? 1 : 0;
  }
  j["softens"] = nSoft;
  j["routes"] = nRoute;
  j["siteCellMax"] = static_cast<int>(kSiteCellMax);
  j["warnings"] = m.warnings;
  j["error"] = error;
  return j.dump();
}

bool PackWorldMap(const biomes::BiomeSet& set, const WorldMapData& map,
                  std::vector<uint32_t>& W, std::string& log) {
  if (!PackBiomeTable(set, W, log)) return false;
  // The spawn words are written even for an unloaded map (the default), so
  // the shader's spawnCentre() and the C++ twin never disagree about where
  // the home area is -- the twin reads WorldMapData's default either way.
  W[kHSpawnX] = U(map.spawnX);
  W[kHSpawnZ] = U(map.spawnZ);
  // The terrain words likewise (P-G): an unloaded map carries the defaults,
  // and the twin reads WorldMapData's copy of the same words either way.
  for (uint32_t k = 0; k < kTerrainWords; k++) W[kHTerrainBaseHeight + k] = map.terrainWords[k];
  if (!map.Loaded()) return true;   // P0/P1 tools: records only, planes absent
  W[kHCellLog2] = U(map.cellLog2);
  W[kHWidth] = U(map.width);
  W[kHHeight] = U(map.height);
  W[kHOriginX] = U(map.originCellX);
  W[kHOriginZ] = U(map.originCellZ);
  W[kHSeaLevelY] = U(map.seaLevelY);
  W[kHOceanFade] = U(map.oceanFadeCells);
  W[kHWarpAmp] = U(map.warpAmpVox);
  W[kHOceanBiome] = U(map.oceanBiome);
  W[kHPadX0] = U(map.padX0);
  W[kHPadZ0] = U(map.padZ0);
  W[kHPadX1] = U(map.padX1);
  W[kHPadZ1] = U(map.padZ1);
  W[kHBiomePlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.biome);
  W[kHLandformPlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.landform);
  W[kHMoisturePlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.moisture);
  // ---- sites: the index plane + lists, the records, then each stamp block ----
  // One word per cell (the list's buffer offset, 0 = none), then the shared
  // lists. No sites at all packs no plane: kHSiteIndex stays 0 and every
  // reader's list walk ends at its first read.
  if (!map.sites.empty() && map.siteIndex.size() == static_cast<size_t>(map.width) * map.height) {
    const size_t plane0 = W.size();
    W[kHSiteIndex] = U(static_cast<int>(plane0));
    W.resize(plane0 + map.siteIndex.size(), 0u);
    const uint32_t lists0 = static_cast<uint32_t>(W.size());
    W.insert(W.end(), map.siteLists.begin(), map.siteLists.end());
    for (size_t i = 0; i < map.siteIndex.size(); i++)
      if (map.siteIndex[i] != 0u) W[plane0 + i] = lists0 + map.siteIndex[i];
  }
  W[kHSiteCount] = U(static_cast<int>(map.sites.size()));
  W[kHSiteTable] = U(static_cast<int>(W.size()));
  const size_t tab0 = W.size();
  W.resize(tab0 + map.sites.size() * kSiteRecWords, 0u);
  for (size_t i = 0; i < map.sites.size(); i++) {
    const WorldMapData::StampSite& s = map.sites[i];
    uint32_t* r = W.data() + tab0 + i * kSiteRecWords;
    r[kS_Kind] = U(s.kind);
    r[kS_X] = U(s.x); r[kS_Z] = U(s.z);
    r[kS_Radius] = U(s.radius);
    r[kS_PadMargin] = U(s.padMargin);
    r[kS_Rot] = U(s.rot);
    r[kS_Salt] = s.salt;
    r[kS_Preset] = U(s.preset);
    r[kS_PadY] = U(s.padY);
    r[kS_Species] = U(s.species);
    r[kS_Variant] = U(s.variant);
    r[kS_Sink] = U(s.sink);
    if (s.kind == kSiteClearing || s.kind == kSitePath) { r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1); }
    if (s.kind == kSiteSoften) {
      r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1);
      r[kS_SoftHill] = U(s.softHill); r[kS_SoftBump] = U(s.softBump); r[kS_SoftGrain] = U(s.softGrain);
      r[kS_SoftPath] = U(s.softPath);
    }
    if (s.kind == kSiteStamp) {
      r[kS_BoxX0] = U(s.bx0); r[kS_BoxZ0] = U(s.bz0);
      r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1);
      r[kS_Apron] = U(s.apron);
    }
    if (s.kind != kSiteStamp || s.words.empty()) continue;   // a water site has no block
    // The stamp block: rebase its relative offsets (column dir + run offsets)
    // onto the buffer as it is appended.
    const uint32_t base = static_cast<uint32_t>(W.size());
    std::vector<uint32_t> blk = s.words;
    blk[kStamp_Columns] += base;
    const size_t cols = static_cast<size_t>(s.nx) * s.nz;
    for (size_t c = 0; c < cols; c++) blk[kStampHdrWords + c * 2] += base;
    W.insert(W.end(), blk.begin(), blk.end());
    W.data()[tab0 + i * kSiteRecWords + kS_StampOff] = base;   // W moved: re-index
  }
  // ---- the sculpt block (P5): appended as-is, its offsets are block-relative ----
  // Nothing at all for a map without a layer: kHSculpt stays 0 and the words
  // are the words a pre-P5 build packed.
  if (!map.sculpt.empty()) {
    W[kHSculpt] = U(static_cast<int>(W.size()));
    W.insert(W.end(), map.sculpt.begin(), map.sculpt.end());
  }
  W[kHContentHash] = 0u;
  W[kHContentHash] = FnvWords(W) ^ map.contentHash;
  std::printf("world map: '%s' %dx%d cells of %d vox, origin cell (%d,%d), %zu palette, "
              "content %08x\n",
              map.name.c_str(), map.width, map.height, 1 << map.cellLog2,
              map.originCellX, map.originCellZ, map.palette.size(), W[kHContentHash]);
  return true;
}

namespace {
std::vector<StructurePlacement>& OverrideList() {
  static std::vector<StructurePlacement> v;
  return v;
}
bool& OverrideOn() {
  static bool on = false;
  return on;
}
}  // namespace
void SetStructureOverride(const std::vector<StructurePlacement>* list) {
  OverrideOn() = list != nullptr;
  OverrideList() = list ? *list : std::vector<StructurePlacement>{};
}
bool StructureOverrideActive() { return OverrideOn(); }
const std::vector<StructurePlacement>* StructureOverrideSlot() {
  return OverrideOn() ? &OverrideList() : nullptr;
}

const WorldMapData& CurrentWorldMap() { return Slot(); }
void SetCurrentWorldMap(WorldMapData map) { Slot() = std::move(map); }

namespace {
std::string& MapOverrideSlot() {
  static std::string s;
  return s;
}
// A bare name only (world.mapLayer's rule in LoadTuning): the name is a
// directory under assets/worldmap/, never a path.
bool BareMapName(const std::string& n) {
  return !n.empty() && n.find_first_of("/\\:") == std::string::npos;
}
}  // namespace

void SetMapOverride(const std::string& name) { MapOverrideSlot() = name; }

std::string ActiveMapName(const std::string& tuned) {
  if (const char* e = std::getenv("SANDVOX_MAP")) {
    if (BareMapName(e)) return e;
    if (*e) std::fprintf(stderr, "SANDVOX_MAP='%s' is not a bare map name -- ignored\n", e);
  }
  if (BareMapName(MapOverrideSlot())) return MapOverrideSlot();
  return tuned;
}
const TerrainParams& CurrentTerrain() { return Slot().terrain; }

}  // namespace worldmap
