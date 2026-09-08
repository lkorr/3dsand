// worldmap.cpp -- loads the authored world map and packs it, with the biome
// record table, into the worldMap buffer. See worldmap.h for the layout and
// the seed discipline.
#include "sim/worldmap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/rng.h"
#include "sim/tuning.h"
#include "sim/voxload.h"
#include "sim/world.h"

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
  g.maxSlope = std::clamp(w.maxSlope <= 0 ? 1024 : w.maxSlope, 0, 1024);
  g.minY = w.minY; g.maxY = w.maxY;
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
void OverlayLandformSites(const std::vector<LandformSite>& sites, int landformRangeVox,
                          int cellLog2, int width, int height, int originCellX, int originCellZ,
                          std::vector<uint8_t>& landform) {
  if (sites.empty() || landform.size() != static_cast<size_t>(width) * height) return;
  const double unitsPerVox = 256.0 / static_cast<double>(std::max(1, landformRangeVox));
  const int half = 1 << (cellLog2 - 1);
  std::vector<double> acc(landform.size());
  for (size_t i = 0; i < landform.size(); i++) acc[i] = landform[i];
  for (const LandformSite& st : sites) {
    const double r = std::max(1, st.radius);
    const double amp = std::fabs(static_cast<double>(st.heightVox)) * unitsPerVox * (st.shape == "basin" ? -1.0 : 1.0);
    if (amp == 0.0) continue;
    const double ang = st.rotation * 3.14159265358979323846 / 180.0;
    const double ca = std::cos(ang), sa = std::sin(ang);
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
        const double wx = ((cx - originCellX) << cellLog2) + half;
        const double wz = ((cz - originCellZ) << cellLog2) + half;
        const double dx = wx - st.x, dz = wz - st.z;
        double t;
        if (st.shape == "ridge") {
          // into the ridge's frame: `u` along the crest, `v` across it
          const double u = dx * ca + dz * sa, v = -dx * sa + dz * ca;
          const double a = r, b = std::max(1.0, r / 3.0);
          t = std::sqrt((u * u) / (a * a) + (v * v) / (b * b));
        } else {
          t = std::sqrt(dx * dx + dz * dz) / r;
        }
        if (t >= 1.0) continue;
        double f;
        if (st.shape == "plateau") f = t <= 0.6 ? 1.0 : 1.0 - (t - 0.6) / 0.4;
        else f = 1.0 - t;
        acc[static_cast<size_t>(cz) * width + cx] += amp * f;
      }
  }
  for (size_t i = 0; i < landform.size(); i++)
    landform[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(acc[i])), 0, 255));
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
    p.w[kR_PatchThreshold] = U(std::clamp(r.cond.patchThreshold, 0, 255));
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
    r[kB_SedMax] = 0u;
    uint32_t flags = 0;
    if (b.groundFlora) flags |= kBF_GroundFlora;
    if (b.cacti) flags |= kBF_Cacti;
    if (b.sandCap) flags |= kBF_SandCap;
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
      rec(kW_MaxSlope) = U(g.maxSlope);
      rec(kW_MinY) = U(g.minY);
      rec(kW_MaxY) = U(g.maxY);
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

bool LoadWorldMap(const std::string& assetDir, const std::string& name,
                  const biomes::BiomeSet& set, size_t materialCount, uint32_t seed,
                  WorldMapData& out, std::string& log) {
  out = WorldMapData{};
  const std::string dir = assetDir + "/worldmap/" + name;
  const std::string at = "worldmap/" + name + ": ";

  // ---- map.json -------------------------------------------------------------
  json j;
  {
    std::ifstream f(dir + "/map.json");
    if (!f) { log += at + "map.json not found (" + dir + ")\n"; return false; }
    try { f >> j; } catch (const std::exception& e) {
      log += at + "map.json does not parse: " + e.what() + "\n"; return false;
    }
  }
  auto geti = [&](const char* k, int d) { return j.contains(k) && j[k].is_number() ? j[k].get<int>() : d; };
  out.name = name;
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
      std::string err, warn;
      const std::string vox = assetDir + "/prefabs/" + st.templateName + ".vox";
      if (!LoadVoxFile(vox, materialCount, pf, err, warn)) {
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

  // ---- sites: the harness pad box, the spawn site, and the stamp sites --------
  bool havePad = false;
  if (j.contains("sites") && j["sites"].is_array()) {
    for (const json& s : j["sites"]) {
      if (!s.is_object()) continue;
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
        if (havePad) continue;   // one pad box until the pad becomes a site kind proper
        if (!(s.contains("min") && s.contains("max") && s["min"].is_array() &&
              s["max"].is_array() && s["min"].size() == 2 && s["max"].size() == 2)) {
          log += at + "site \"" + id + "\" kind pad needs min[2]/max[2] in world voxels\n";
          return false;
        }
        out.harnessX0 = s["min"][0].get<int>(); out.harnessZ0 = s["min"][1].get<int>();
        out.harnessX1 = s["max"][0].get<int>(); out.harnessZ1 = s["max"][1].get<int>();
        havePad = true;
      } else if (kind == "stamp") {
        WorldMapData::StampSite st;
        st.id = id;
        st.templateName = s.value("template", "");
        st.x = s.value("x", 0); st.z = s.value("z", 0);
        st.rot = s.value("rot", 0) & 3;
        st.padMargin = std::clamp(s.value("padMargin", 8), 1, 64);
        st.salt = static_cast<uint32_t>(s.value("salt", 0));
        if (st.templateName.empty()) {
          log += at + "site \"" + id + "\" kind stamp needs a template name (assets/prefabs/<name>.vox)\n";
          return false;
        }
        if (!loadStamp(st, id)) return false;
        if (s.contains("radius") && s["radius"].is_number()) st.radius = std::max(st.radius, s["radius"].get<int>());
        if (out.sites.size() >= 254) { log += at + "more than 254 stamp sites\n"; return false; }
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
        if (out.sites.size() >= 254) { log += at + "more than 254 sites\n"; return false; }
        out.sites.push_back(std::move(st));
      }
    }
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
            if (std::max(std::abs(o.x - wx), std::abs(o.z - wz)) < minSpacing) { tooClose = true; break; }
          if (tooClose || out.InHarness(wx, wz)) continue;
          WorldMapData::StampSite st;
          st.id = rid + "_" + std::to_string(placed);
          st.templateName = tpl;
          st.x = wx; st.z = wz;
          st.rot = rotRule < 0 ? static_cast<int>((h >> 5) & 3u) : (rotRule & 3);
          st.padMargin = padMargin;
          st.salt = h;
          if (!loadStamp(st, st.id)) return false;
          if (out.sites.size() >= 254) { log += at + "more than 254 stamp sites (rule " + rid + ")\n"; return false; }
          out.sites.push_back(std::move(st));
          placed++;
        }
      std::printf("world map: rule '%s' placed %d x %s in %s\n", rid.c_str(), placed, tpl.c_str(), bname.c_str());
    }
  }

  // ---- the site index plane: every cell a stamp's footprint + margin reaches ----
  out.siteIndex.assign(cells, 0);
  for (size_t si = 0; si < out.sites.size(); si++) {
    const WorldMapData::StampSite& st = out.sites[si];
    // A stamp reaches its footprint + pad margin; a water site its disc + the
    // preset's shore/berm band (stored in padMargin), + 1 for the bisection's
    // outer column.
    const int reach = st.radius + st.padMargin + (st.kind == kSiteWater ? 1 : 0);
    int c0x, c0z, c1x, c1z;
    out.CellOf(st.x - reach, st.z - reach, &c0x, &c0z);
    out.CellOf(st.x + reach, st.z + reach, &c1x, &c1z);
    for (int cz = c0z; cz <= c1z; cz++)
      for (int cx = c0x; cx <= c1x; cx++) {
        if (!out.Inside(cx, cz)) continue;
        uint8_t& slot = out.siteIndex[static_cast<size_t>(cz) * out.width + cx];
        if (slot == 0) slot = static_cast<uint8_t>(si + 1);   // first site wins a cell
      }
  }

  uint32_t hsh = 2166136261u;
  hsh = FnvBytes(hsh, raw.data(), raw.size());
  {
    const std::string js = j.dump();
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(js.data()), js.size());
  }
  out.contentHash = hsh;
  return true;
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
  W[kHHarnessX0] = U(map.harnessX0);
  W[kHHarnessZ0] = U(map.harnessZ0);
  W[kHHarnessX1] = U(map.harnessX1);
  W[kHHarnessZ1] = U(map.harnessZ1);
  W[kHBiomePlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.biome);
  W[kHLandformPlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.landform);
  W[kHMoisturePlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.moisture);
  // ---- sites: the index plane, the records, then each stamp block -----------
  W[kHSiteIndex] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.siteIndex);
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
  W[kHContentHash] = 0u;
  W[kHContentHash] = FnvWords(W) ^ map.contentHash;
  std::printf("world map: '%s' %dx%d cells of %d vox, origin cell (%d,%d), %zu palette, "
              "content %08x\n",
              map.name.c_str(), map.width, map.height, 1 << map.cellLog2,
              map.originCellX, map.originCellZ, map.palette.size(), W[kHContentHash]);
  return true;
}

const WorldMapData& CurrentWorldMap() { return Slot(); }
void SetCurrentWorldMap(WorldMapData map) { Slot() = std::move(map); }
const TerrainParams& CurrentTerrain() { return Slot().terrain; }

}  // namespace worldmap
