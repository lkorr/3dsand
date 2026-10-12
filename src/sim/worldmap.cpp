// worldmap.cpp -- loads the authored world map and packs it, with the biome
// record table, into the worldMap buffer. See worldmap.h for the layout and
// the seed discipline.
#include "sim/worldmap.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/dungeon.h"
#include "sim/rng.h"
#include "sim/treeatlas.h"   // ReadTreeSpeciesHeaders: a tree site's species index + reach
#include "sim/tuning.h"
#include "sim/tuningstamp.h"   // HashOneFile: the edit layers' fingerprint part (L1)
#include "sim/voxload.h"
#include "sim/world.h"
#include "sim/worldwater.h"   // rivers[] / lakes[] (W1) and the spline
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
  // Every amplitude is clamped to kNoiseAmpMax (worldmap.h: the octave's
  // n.dx * amp * biome multiplier bound); LoadWorldMap says so when it bites.
  at(kHTerrainRangeAmplitude) = U(std::clamp(t.rangeAmplitude, 0, kNoiseAmpMax));
  at(kHTerrainRangeLog2) = U(std::clamp(t.rangeLog2, 3, 15));
  at(kHTerrainHillAmplitude) = U(std::clamp(t.hillAmplitude, 0, kNoiseAmpMax));
  at(kHTerrainHillLog2) = U(std::clamp(t.hillLog2, 3, 15));
  at(kHTerrainDetailAmplitude) = U(std::clamp(t.detailAmplitude, 0, kNoiseAmpMax));
  at(kHTerrainDetailLog2) = U(std::clamp(t.detailLog2, 3, 15));
  at(kHTerrainGrainAmplitude) = U(std::clamp(t.grainAmplitude, 0, kNoiseAmpMax));
  at(kHTerrainGrainLog2) = U(std::clamp(t.grainLog2, 3, 15));
  at(kHTerrainFbmAtten) = U(std::clamp(t.fbmAtten, 0, 256));
  at(kHTerrainTreelineJitter) = U(std::clamp(t.treelineJitter, 0, kTreelineJitterMax));
  at(kHTerrainRockSlope) = U(std::max(0, t.rockSlope));
  at(kHTerrainHomeR) = U(std::max(0, t.homeR));
  at(kHTerrainHomeFade) = U(std::max(1, t.homeFade));
  at(kHTerrainSedAbove) = U(std::clamp(t.sedAbove, -4096, 4096));
  at(kHTerrainSedFraction) = U(std::max(0, t.sedFraction));
  at(kHTerrainSedStrip) = U(std::max(0, t.sedStrip));
  at(kHTerrainSedSlope) = U(std::max(0, t.sedSlope));
  at(kHTerrainSedMax) = U(std::max(0, t.sedMax));
  at(kHTerrainSedTopsoil) = U(std::clamp(t.sedTopsoil, 0, std::max(0, t.sedMax)));
  at(kHTerrainTreeline) = U(t.treeline);
  at(kHTerrainRefVpm) = U(std::max(1, t.refVoxelsPerMetre));
}

int BiomeTreelineQ8(const biomes::BiomeDef& b) {
  return std::clamp(static_cast<int>(std::lround(b.terrain.treelineScale * 256.0)), 0, 1024);
}

int TreelineMaxOf(const WorldMapData& map) {
  // treelineAt = sea + (((treeline - sea) * scale) >> 8) + jitter, scale over
  // every biome the map can show (the default 256 too: no planes = identity),
  // jitter +-treelineJitter. Monotone in the scale, so its two ends bound it.
  const int sea = map.seaLevelY, d = map.terrain.treeline - sea;
  int lo = 256, hi = 256;
  for (int q : map.biomeTreelineQ8) { lo = std::min(lo, q); hi = std::max(hi, q); }
  const int top = sea + std::max((d * lo) >> 8, (d * hi) >> 8);
  return top + std::clamp(map.terrain.treelineJitter, 0, kTreelineJitterMax) + 1;
}

int WindowChunkYForGround(int groundY) {
  // floor((groundY - 72) / 256) half-windows, as chunks. Arithmetic shift
  // floors on the negative side too.
  return ((groundY - 72) >> 8) * static_cast<int>(kNChunk / 2);
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
    r[kB_TreelineQ8] = U(BiomeTreelineQ8(b));
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
      row[kC_Treeline] = U(std::clamp(c.cond.treeline, 0, 2));
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

// ---- locales (L1, worldmap.h LocaleDef) ----------------------------------------
namespace {
// A locale id names its ref group and its folder: the ref id alphabet.
bool LocaleIdOk(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}
// map.json `locales` -> `out`, table order. False + `err` on a malformed entry
// (a missing id / bounds / anchor, corners the wrong way round, a duplicate
// id, a layer name that is a path): a half-read locale would put its refs at
// a height nobody authored. Softer doubts go to `warn`.
bool ParseLocales(const json& j, std::vector<LocaleDef>& out, std::string& err,
                  std::vector<std::string>* warn) {
  out.clear();
  if (!j.contains("locales")) return true;
  if (!j["locales"].is_array()) { err = "locales must be an array"; return false; }
  auto pair = [](const json& o, const char* k, int& a, int& b) {
    if (!o.contains(k) || !o[k].is_array() || o[k].size() != 2 || !o[k][0].is_number() || !o[k][1].is_number())
      return false;
    a = o[k][0].get<int>();
    b = o[k][1].get<int>();
    return true;
  };
  for (const json& l : j["locales"]) {
    LocaleDef d;
    if (!l.is_object() || !l.contains("id") || !l["id"].is_string()) { err = "a locale needs an \"id\""; return false; }
    d.id = l["id"].get<std::string>();
    const std::string who = "locale \"" + d.id + "\": ";
    if (!LocaleIdOk(d.id)) { err = who + "the id is its ref group and folder name: [a-z0-9_-]+"; return false; }
    for (const LocaleDef& o : out)
      if (o.id == d.id) { err = who + "two locales with this id"; return false; }
    d.name = l.value("name", d.id);
    d.icon = l.value("icon", std::string());
    const json b = l.contains("bounds") && l["bounds"].is_object() ? l["bounds"] : json::object();
    if (!pair(b, "min", d.x0, d.z0) || !pair(b, "max", d.x1, d.z1)) {
      err = who + "needs bounds {min: [x, z], max: [x, z]} in world voxels";
      return false;
    }
    if (d.x1 < d.x0 || d.z1 < d.z0) { err = who + "bounds min is not below max on both axes"; return false; }
    const json an = l.contains("anchor") && l["anchor"].is_object() ? l["anchor"] : json::object();
    if (!pair(an, "at", d.ax, d.az) || !an.contains("y") || !an["y"].is_number()) {
      err = who + "needs anchor {at: [x, z], y} (y = the authored ground at `at`)";
      return false;
    }
    d.ay = an["y"].get<int>();
    if (l.contains("editLayer")) {
      if (!l["editLayer"].is_string()) { err = who + "editLayer must be a file name"; return false; }
      d.editLayer = l["editLayer"].get<std::string>();
      if (d.editLayer.size() > 7 && d.editLayer.compare(d.editLayer.size() - 7, 7, ".svedit") == 0)
        d.editLayer.resize(d.editLayer.size() - 7);
      if (d.editLayer.empty() || d.editLayer.find_first_of("/\\:.") != std::string::npos) {
        err = who + "editLayer is a file name under locales/" + d.id + "/ (\"ground\"), not a path";
        return false;
      }
    }
    if (warn && (d.ax < d.x0 || d.ax > d.x1 || d.az < d.z0 || d.az > d.z1))
      warn->push_back(who + "anchor (" + std::to_string(d.ax) + "," + std::to_string(d.az) +
                      ") is outside its bounds; the locale rides the ground THERE");
    out.push_back(std::move(d));
  }
  return true;
}
// map.json `dungeons` -> `out`, table order (PLAN_dungeons §3.1). False + `err`
// on a malformed entry; the ASSET is loaded later and a missing one is only a
// warning (the dungeon is then not placed).
bool ParseDungeons(const json& j, std::vector<DungeonDef>& out, std::string& err) {
  out.clear();
  if (!j.contains("dungeons")) return true;
  if (!j["dungeons"].is_array()) { err = "dungeons must be an array"; return false; }
  for (const json& d : j["dungeons"]) {
    DungeonDef g;
    if (!d.is_object() || !d.contains("id") || !d["id"].is_string()) { err = "a dungeon needs an \"id\""; return false; }
    g.id = d["id"].get<std::string>();
    const std::string who = "dungeon \"" + g.id + "\": ";
    if (!LocaleIdOk(g.id)) { err = who + "the id is its asset dir and ref group: [a-z0-9_-]+"; return false; }
    for (const DungeonDef& o : out)
      if (o.id == g.id) { err = who + "two dungeons with this id"; return false; }
    g.name = d.value("name", g.id);
    g.asset = d.value("dungeon", g.id);
    if (!LocaleIdOk(g.asset)) { err = who + "\"dungeon\" names an asset dir: [a-z0-9_-]+"; return false; }
    if (!d.contains("at") || !d["at"].is_array() || d["at"].size() != 2 || !d["at"][0].is_number() ||
        !d["at"][1].is_number()) {
      err = who + "needs at: [x, z] (the anchor column, world voxels)";
      return false;
    }
    g.ax = d["at"][0].get<int>();
    g.az = d["at"][1].get<int>();
    g.yOffset = d.contains("yOffset") && d["yOffset"].is_number() ? d["yOffset"].get<int>() : 0;
    const int yaw = d.contains("yaw") && d["yaw"].is_number() ? d["yaw"].get<int>() : 0;
    if (yaw % 90 != 0) { err = who + "yaw must be a quarter turn (0, 90, 180, 270)"; return false; }
    g.yawQ = ((yaw / 90) % 4 + 4) % 4;
    out.push_back(std::move(g));
  }
  return true;
}

// The scratch dungeon (voxserve DUNGEON) and the gates' enable switch.
std::unique_ptr<ScratchDungeon>& ScratchSlot() {
  static std::unique_ptr<ScratchDungeon> s;
  return s;
}
bool& DungeonsOn() {
  static bool on = true;
  return on;
}
}  // namespace

void SetScratchDungeon(const ScratchDungeon* s) {
  if (s) ScratchSlot() = std::make_unique<ScratchDungeon>(*s);
  else ScratchSlot().reset();
}
const ScratchDungeon* ScratchDungeonSlot() { return ScratchSlot().get(); }
void SetDungeonsEnabled(bool on) { DungeonsOn() = on; }
bool DungeonsEnabled() { return DungeonsOn(); }

namespace {
std::string LocaleLayerStem(const LocaleDef& l) { return l.editLayer.empty() ? std::string("ground") : l.editLayer; }
}  // namespace

bool ReadLocales(const std::string& assetDir, const std::string& mapName, std::vector<LocaleDef>& out,
                 std::string& err) {
  out.clear();
  std::ifstream f(assetDir + "/worldmap/" + mapName + "/map.json");
  if (!f) return true;
  json j;
  try { f >> j; } catch (const std::exception& e) {
    err = "map.json does not parse: " + std::string(e.what());
    return false;
  }
  return ParseLocales(j, out, err, nullptr);
}

std::map<std::string, int> LocaleRefYOffsets(const std::string& assetDir, const std::string& mapName,
                                             std::vector<std::string>* warn) {
  std::map<std::string, int> off;
  std::vector<LocaleDef> ls;
  std::string err;
  if (!ReadLocales(assetDir, mapName, ls, err)) {
    if (warn) warn->push_back("worldmap/" + mapName + "/map.json: " + err + " -- no locale offsets applied");
    return off;
  }
  const WorldMapData& cur = CurrentWorldMap();
  for (const LocaleDef& l : ls) {
    const LocaleDef* live = nullptr;
    if (cur.name == mapName)
      for (const LocaleDef& c : cur.locales)
        if (c.id == l.id && c.ay == l.ay && c.ax == l.ax && c.az == l.az && c.resolved) live = &c;
    if (live) {
      off[l.id] = live->RefYOffset();
    } else {
      off[l.id] = l.ay;
      if (warn)
        warn->push_back("locale \"" + l.id + "\": the world map '" + mapName +
                        "' is not the loaded one, so its refs sit at their AUTHORED height (anchor y " +
                        std::to_string(l.ay) + ", delta 0)");
    }
  }
  return off;
}

std::vector<std::pair<std::string, std::string>> MapEditLayerFiles(const std::string& assetDir,
                                                                   const std::string& mapName,
                                                                   const std::string& editLayer,
                                                                   const std::vector<LocaleDef>& locales,
                                                                   const std::vector<std::string>& dungeonAssets) {
  std::vector<std::pair<std::string, std::string>> out;
  if (!editLayer.empty()) out.push_back({assetDir + "/worldedits/" + editLayer + ".svedit", editLayer + ".svedit"});
  for (const LocaleDef& l : locales) {
    const std::string stem = LocaleLayerStem(l);
    const std::string p = assetDir + "/worldmap/" + mapName + "/locales/" + l.id + "/" + stem + ".svedit";
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) out.push_back({p, l.id + "/" + stem + ".svedit"});
  }
  std::set<std::string> seen;
  for (const std::string& a : dungeonAssets) {
    if (a.empty() || !seen.insert(a).second) continue;
    const std::string p = assetDir + "/dungeons/" + a + "/detail.svedit";
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) out.push_back({p, "dungeon:" + a + "/detail.svedit"});
  }
  return out;
}

std::vector<std::string> ReadDungeonAssets(const std::string& assetDir, const std::string& mapName) {
  std::vector<std::string> out;
  std::ifstream f(assetDir + "/worldmap/" + mapName + "/map.json");
  if (!f) return out;
  json j;
  try { f >> j; } catch (const std::exception&) { return out; }
  std::vector<DungeonDef> ds;
  std::string err;
  if (!ParseDungeons(j, ds, err)) return out;
  for (const DungeonDef& g : ds) out.push_back(g.asset);
  return out;
}

std::vector<std::string> DungeonAssetsOf(const WorldMapData& m) {
  std::vector<std::string> out;
  for (const DungeonDef& g : m.dungeons) out.push_back(g.asset);
  return out;
}

uint32_t HashEditLayerFiles(const std::vector<std::pair<std::string, std::string>>& files) {
  uint32_t h = 0;
  for (size_t i = 0; i < files.size(); i++) {
    const uint32_t x = sandvox::HashOneFile(files[i].first, files[i].second);
    h = i == 0 ? x : (h * 16777619u) ^ x;
  }
  return h;
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
  // MAP V2 ONLY (docs/PLAN_map_v2.md). A v1 map -- map.svmap's u8 landform
  // plane, `cellLog2` / `size` / `originCell` -- is refused, naming the one
  // tool that turns it into a v2 map, rather than half-read.
  if (geti("version", 1) != 2) {
    log += at + "map.json is a v1 map (no \"version\": 2); convert it with "
                "`python scripts/convert_map_v2.py " + name + "` (docs/PLAN_map_v2.md)\n";
    return false;
  }
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
  // L1: the locales. Resolved (each anchor's bare ground for `seed`) at the
  // site-pad bake below, once the planes, the sculpt layer and every site the
  // bare ground reads are in place.
  {
    std::string lerr;
    std::vector<std::string> lwarn;
    if (!ParseLocales(j, out.locales, lerr, &lwarn)) {
      log += at + lerr + "\n";
      return false;
    }
    for (const std::string& w : lwarn) warn(w);
    for (const LocaleDef& l : out.locales)
      if (!l.editLayer.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dir + "/locales/" + l.id + "/" + l.editLayer + ".svedit", ec))
          warn("locale \"" + l.id + "\": editLayer locales/" + l.id + "/" + l.editLayer +
               ".svedit does not exist; the locale has no edit layer");
      }
  }
  // ---- DUNGEONS (docs/PLAN_dungeons.md §3.1, sim/dungeon.h) --------------------
  // Parsed and their programs loaded + resolved here; ANCHORED (the bare ground
  // at `at` for `seed`) at the site-pad bake below, beside the locales. A
  // dungeon whose asset is missing or refuses is a WARNING and is not placed:
  // a map must still boot while a dungeon is being authored.
  {
    std::string derr;
    if (!ParseDungeons(j, out.dungeons, derr)) {
      log += at + derr + "\n";
      return false;
    }
    if (const ScratchDungeon* sd = ScratchDungeonSlot()) {
      bool replaced = false;
      for (DungeonDef& g : out.dungeons)
        if (sd->replaceMapEntry && g.id == sd->def.id) {
          g.shell = sd->def.shell;
          g.res = sd->def.res;
          g.shellPath = sd->def.shellPath;
          g.shellHash = sd->def.shellHash;
          replaced = true;
        }
      if (!replaced) out.dungeons.push_back(sd->def);
    }
    for (DungeonDef& g : out.dungeons) {
      if (g.shell) continue;   // the scratch program, already loaded
      auto shell = std::make_shared<dungeon::Shell>();
      std::string serr, used;
      if (!dungeon::LoadShellAsset(assetDir, g.asset, *shell, serr, &used)) {
        warn("dungeon \"" + g.id + "\": " + serr + " -- not placed");
        continue;
      }
      if (used.size() > 11 && used.compare(used.size() - 11, 11, "shell.json") == 0)
        warn("dungeon \"" + g.id + "\": no shell.bin, loaded " + used + " (compile the dungeon to write the .bin)");
      uint32_t srcHash = 0;
      const std::string dj = assetDir + "/dungeons/" + g.asset + "/dungeon.json";
      if (dungeon::SourceHashOfFile(dj, srcHash) && srcHash != shell->sourceHash)
        warn("dungeon \"" + g.id + "\": shell is STALE (dungeon.json hash " + std::to_string(srcHash) +
             ", the shell was compiled from " + std::to_string(shell->sourceHash) + ") -- recompile it");
      auto res = std::make_shared<dungeon::Resolved>();
      if (!dungeon::ResolveShell(*shell, set.matNames, set.matClass, *res, serr)) {
        warn("dungeon \"" + g.id + "\": " + serr + " -- not placed");
        continue;
      }
      g.shellPath = used;
      if (!dungeon::SourceHashOfFile(used, g.shellHash)) g.shellHash = 0;
      g.shell = std::move(shell);
      g.res = std::move(res);
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
    // homeArea.y is not read (map v2 A1): the authored plane is the ground's
    // height and the calm fade flattens only the noise. A map that still
    // carries it gets a warning, not a refusal.
    if (ha.contains("y")) warn("terrain.homeArea.y is ignored since map v2 (the height plane is the home ground)");
    t.homeR = std::max(0, (hnum("radius", t.homeR) * kVoxelsPerMetre) / ref);
    // A fade of 0 would divide by zero in landAt and make the home area's
    // boundary a STEP of the whole coarse relief (the terrain gate's A4).
    t.homeFade = std::max(1, (hnum("fade", t.homeFade) * kVoxelsPerMetre) / ref);
    // The wedge's ceiling is RELATIVE to the authored ground (map v2 A1):
    // `sedAbove`. A map still written with the absolute v1 `sedCeil` is read
    // as sedCeil - baseHeight -- v1's baseHeight was the mean ground the
    // ceiling was tuned against (264 - 200 = 64, the shipped value) -- and
    // warned about.
    if (te.contains("sedAbove") && te["sedAbove"].is_number()) {
      t.sedAbove = len("sedAbove", t.sedAbove);
    } else if (te.contains("sedCeil") && te["sedCeil"].is_number()) {
      t.sedAbove = len("sedCeil", 0) - t.baseHeight;
      warn("terrain.sedCeil (absolute) read as sedAbove " + std::to_string(t.sedAbove) +
           " (sedCeil - baseHeight); write sedAbove");
    }
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
    t.treelineJitter = std::clamp(len("treelineJitter", t.treelineJitter), 0, kTreelineJitterMax);
    t.rockSlope = std::max(0, num("rockSlope", t.rockSlope));
    TerrainWords(t, out.terrainWords);
  }
  // Amplitudes past kNoiseAmpMax are clamped by TerrainWords (the octave's
  // i32 bound); said here, where the map that asked for it is named.
  for (const auto& [key, v] : {std::pair<const char*, int>{"rangeAmplitude", out.terrain.rangeAmplitude},
                               {"hillAmplitude", out.terrain.hillAmplitude},
                               {"detailAmplitude", out.terrain.detailAmplitude},
                               {"grainAmplitude", out.terrain.grainAmplitude}})
    if (v > kNoiseAmpMax)
      warn(std::string("terrain.") + key + " " + std::to_string(v) + " clamped to " + std::to_string(kNoiseAmpMax) +
           " voxels (the octave's overflow bound)");

  // ---- the extent and the lattices (map v2) -----------------------------------
  out.seaLevelY = geti("seaLevelY", 0);
  out.warpAmpVox = geti("warpAmpVox", 0);
  out.edgeFadeVox = geti("edgeFadeVox", 6144);
  out.siteCellLog2 = geti("siteCellLog2", 10);
  {
    const json ex = j.contains("extent") && j["extent"].is_object() ? j["extent"] : json::object();
    auto pair = [&](const char* k, int* a, int* b) -> bool {
      if (!(ex.contains(k) && ex[k].is_array() && ex[k].size() == 2 && ex[k][0].is_number_integer() &&
            ex[k][1].is_number_integer())) return false;
      *a = ex[k][0].get<int>(); *b = ex[k][1].get<int>();
      return true;
    };
    if (!pair("min", &out.extentX0, &out.extentZ0) || !pair("size", &out.extentW, &out.extentH)) {
      log += at + "map.json needs extent.min[2] and extent.size[2] in world voxels\n"; return false;
    }
  }
  if (out.siteCellLog2 < 6 || out.siteCellLog2 > 14) {
    log += at + "siteCellLog2 " + std::to_string(out.siteCellLog2) + " is outside 6..14\n"; return false;
  }
  const int siteVox = 1 << out.siteCellLog2;
  // The extent's corners are site-cell corners, so the site plane, every
  // finer plane and the extent tile each other exactly (floor division by a
  // power of two is then a shift on both sides of the mirror).
  if (out.extentW <= 0 || out.extentH <= 0 || out.extentW % siteVox || out.extentH % siteVox ||
      out.extentX0 % siteVox || out.extentZ0 % siteVox) {
    log += at + "extent min (" + std::to_string(out.extentX0) + "," + std::to_string(out.extentZ0) + ") size (" +
           std::to_string(out.extentW) + "," + std::to_string(out.extentH) + ") must be positive multiples of the " +
           std::to_string(siteVox) + "-voxel site cell (siteCellLog2)\n";
    return false;
  }
  if (static_cast<int64_t>(out.extentX0) + out.extentW > (int64_t(1) << 30) || out.extentX0 < -(1 << 30) ||
      static_cast<int64_t>(out.extentZ0) + out.extentH > (int64_t(1) << 30) || out.extentZ0 < -(1 << 30)) {
    log += at + "extent reaches past +-2^30 voxels\n"; return false;
  }
  out.siteW = out.extentW >> out.siteCellLog2;
  out.siteH = out.extentH >> out.siteCellLog2;
  out.siteOriginX = -(out.extentX0 >> out.siteCellLog2);
  out.siteOriginZ = -(out.extentZ0 >> out.siteCellLog2);
  if (out.siteW > 4096 || out.siteH > 4096) {
    log += at + "the extent is " + std::to_string(out.siteW) + "x" + std::to_string(out.siteH) +
           " site cells; at most 4096 a side\n";
    return false;
  }
  if (out.edgeFadeVox < 64 || out.edgeFadeVox > 65536) {
    // 65536: mapHeightAt's d * 32768 stays inside i32 for d < fade.
    log += at + "edgeFadeVox " + std::to_string(out.edgeFadeVox) + " is outside 64..65536\n"; return false;
  }
  // The two planes' declarations. Their DATA is read after the palette.
  const json pl = j.contains("planes") && j["planes"].is_object() ? j["planes"] : json::object();
  const json ph = pl.contains("height") && pl["height"].is_object() ? pl["height"] : json::object();
  const json pb = pl.contains("biome") && pl["biome"].is_object() ? pl["biome"] : json::object();
  if (ph.empty() || pb.empty()) {
    log += at + "map.json needs planes.height and planes.biome\n"; return false;
  }
  out.heightLog2 = ph.value("log2", 6);
  out.biomeLog2 = pb.value("log2", 8);
  out.heightDefault = ph.value("default", 0);
  if (ph.value("tile", kTileSide) != kTileSide || pb.value("tile", kTileSide) != kTileSide) {
    log += at + "planes.*.tile must be " + std::to_string(kTileSide) + "\n"; return false;
  }
  // 3..6: the Catmull-Rom's i32 bounds (worldgen.wgsl crStep) are argued for a
  // post fraction under 64; and a post may not be coarser than a site cell.
  if (out.heightLog2 < 3 || out.heightLog2 > 6 || out.heightLog2 > out.siteCellLog2) {
    log += at + "planes.height.log2 " + std::to_string(out.heightLog2) + " is outside 3..6 (and <= siteCellLog2)\n";
    return false;
  }
  if (out.biomeLog2 < 4 || out.biomeLog2 > out.siteCellLog2) {
    log += at + "planes.biome.log2 " + std::to_string(out.biomeLog2) + " is outside 4..siteCellLog2\n"; return false;
  }
  if (std::abs(out.heightDefault) > kHeightLimit) {
    log += at + "planes.height.default " + std::to_string(out.heightDefault) + " is outside +-" +
           std::to_string(kHeightLimit) + "\n";
    return false;
  }
  out.heightPostsX = (out.extentW >> out.heightLog2) + 1;
  out.heightPostsZ = (out.extentH >> out.heightLog2) + 1;
  out.biomePostsX = (out.extentW >> out.biomeLog2) + 1;
  out.biomePostsZ = (out.extentH >> out.biomeLog2) + 1;
  // THE 64 MiB CEILING (docs/PLAN_map_v2.md "GPU packing"): the planes are
  // packed DENSE; a map past this needs a sparse GPU directory, which this
  // loader does not have, so it says so instead of allocating.
  {
    const int64_t words = (int64_t(out.heightPostsX) * out.heightPostsZ + 1) / 2 +
                          (int64_t(out.biomePostsX) * out.biomePostsZ + 3) / 4 + int64_t(out.siteW) * out.siteH;
    if (out.heightPostsX > 4097 || out.heightPostsZ > 4097 || words * 4 > (int64_t(64) << 20)) {
      log += at + "the planes need " + std::to_string(words * 4 / (1 << 20)) + " MiB dense (" +
             std::to_string(out.heightPostsX) + "x" + std::to_string(out.heightPostsZ) +
             " height posts); the ceiling is 64 MiB and 4097 posts a side without a sparse directory\n";
      return false;
    }
  }
  // The warp must stay well inside a biome cell or a one-cell region pinches
  // below the tree-tile width the shader's comments require.
  if (out.warpAmpVox < 0 || out.warpAmpVox > (1 << out.biomeLog2) / 4) {
    log += at + "warpAmpVox " + std::to_string(out.warpAmpVox) + " exceeds the biome cell / 4 (" +
           std::to_string((1 << out.biomeLog2) / 4) + ")\n";
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
  int biomeDefaultIdx = -1;   // planes.biome.default as a palette index
  {
    const std::string bd = pb.value("default", std::string("ocean"));
    for (size_t i = 0; i < out.palette.size(); i++)
      if (out.palette[i] == bd) biomeDefaultIdx = static_cast<int>(i);
    if (biomeDefaultIdx < 0) {
      log += at + "planes.biome.default \"" + bd + "\" is not in the biomes[] palette\n"; return false;
    }
    out.biomeDefault = paletteId[static_cast<size_t>(biomeDefaultIdx)];
  }

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

  // ---- sites: the pad box, the spawn, stamps, lakes, trees ---------------------
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
        // Map v1's declared landforms are gone (docs/PLAN_map_v2.md): the
        // converter baked them into the height plane, and the World map page
        // offers the shapes as stamps that WRITE the plane. Said, not refused.
        warn("site \"" + id + "\": kind landform is a map v1 site; v2 bakes landforms into the height "
             "plane (scripts/convert_map_v2.py, the map page's shape stamps) -- ignored");
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
             "\" -- ignored (the loader knows spawn, pad, stamp, water, tree, clearing, soften)");
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
      // FILE y: a locale group's offset is not known until the planes are
      // read (the pad bake below adds it).
      structures::ReadPlacements(assetDir, name, placements, swarn, /*resolveLocales=*/false);
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
      // A locale's house: refY / padY are relative to its anchor until the
      // pad bake adds the locale's offset.
      for (size_t li = 0; li < out.locales.size(); li++)
        if (out.locales[li].id == sp.group) st.locale = static_cast<int>(li);
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

  // ---- the plane tiles (map v2): planes/<plane>/<tx>_<tz>.svt ------------------
  // A v1 map.svmap still on disk is a half-converted map: refuse it rather than
  // boot on planes nobody reads.
  {
    std::error_code ec;
    if (std::filesystem::exists(dir + "/map.svmap", ec)) {
      log += at + "map.svmap is a map v1 plane file; a v2 map keeps its planes as tiles -- re-run "
                  "`python scripts/convert_map_v2.py " + name + "` or delete the stale file\n";
      return false;
    }
  }
  out.height.assign(static_cast<size_t>(out.heightPostsX) * out.heightPostsZ, static_cast<int16_t>(out.heightDefault));
  out.biome.assign(static_cast<size_t>(out.biomePostsX) * out.biomePostsZ, static_cast<uint8_t>(out.biomeDefault));
  // Every tile file read, in (plane, tz, tx) order: the content hash folds them
  // in this order, so it is a pure function of the files whatever order the
  // directory lists them in.
  std::map<std::tuple<uint32_t, int, int>, std::vector<uint8_t>> tileBytes;
  auto readPlane = [&](const char* plane, uint32_t kind, int postsX, int postsZ) -> bool {
    const std::string pd = dir + "/planes/" + plane;
    std::error_code ec;
    if (!std::filesystem::is_directory(pd, ec)) return true;   // no tiles: every post is the default
    for (const auto& de : std::filesystem::directory_iterator(pd, ec)) {
      const std::string fn = de.path().filename().string();
      if (de.path().extension() != ".svt") continue;
      int tx = 0, tz = 0;
      char tail = 0;
      if (std::sscanf(fn.c_str(), "%d_%d.sv%c", &tx, &tz, &tail) != 3 || tail != 't') {
        log += at + "planes/" + plane + "/" + fn + ": a tile is named <tx>_<tz>.svt\n";
        return false;
      }
      std::ifstream f(de.path(), std::ios::binary);
      std::ostringstream ss; ss << f.rdbuf();
      const std::string raw = ss.str();
      const uint32_t sb = kind == kPlaneHeight ? 2u : 1u;
      const size_t need = kTileHeaderBytes + size_t(kTileSide) * kTileSide * sb;
      auto rd = [&](size_t o) { uint32_t v; std::memcpy(&v, raw.data() + o, 4); return v; };
      const std::string who = at + "planes/" + plane + "/" + fn + ": ";
      if (raw.size() != need) {
        log += who + std::to_string(raw.size()) + " bytes, a tile is " + std::to_string(need) + "\n"; return false;
      }
      if (rd(0) != kTileMagic || rd(4) != kind || static_cast<int>(rd(8)) != tx || static_cast<int>(rd(12)) != tz ||
          rd(16) != U(kTileSide) || rd(20) != U(kTileSide) || rd(24) != sb) {
        log += who + "header disagrees with its plane / name (magic 'SVT1', kind " + std::to_string(kind) +
               ", tx " + std::to_string(tx) + ", tz " + std::to_string(tz) + ", " + std::to_string(kTileSide) +
               "x" + std::to_string(kTileSide) + ", " + std::to_string(sb) + "-byte samples)\n";
        return false;
      }
      if (tx < 0 || tz < 0 || tx * kTileSide >= postsX || tz * kTileSide >= postsZ) {
        log += who + "lies outside the " + std::to_string(postsX) + "x" + std::to_string(postsZ) + "-post plane\n";
        return false;
      }
      for (int k = 0; k < kTileSide; k++) {
        const int pk = tz * kTileSide + k;
        if (pk >= postsZ) break;
        for (int i = 0; i < kTileSide; i++) {
          const int pi = tx * kTileSide + i;
          if (pi >= postsX) break;
          const size_t so = kTileHeaderBytes + (size_t(k) * kTileSide + i) * sb;
          const size_t di = size_t(pk) * postsX + pi;
          if (kind == kPlaneHeight) {
            int16_t v;
            std::memcpy(&v, raw.data() + so, 2);
            if (std::abs(static_cast<int>(v)) > kHeightLimit) {
              log += who + "post (" + std::to_string(pi) + "," + std::to_string(pk) + ") height " + std::to_string(v) +
                     " is outside +-" + std::to_string(kHeightLimit) + "\n";
              return false;
            }
            out.height[di] = v;
          } else {
            const uint8_t p = static_cast<uint8_t>(raw[so]);
            if (p >= paletteId.size()) {
              log += who + "biome index " + std::to_string(p) + " at post (" + std::to_string(pi) + "," +
                     std::to_string(pk) + ") is outside the palette (" + std::to_string(paletteId.size()) + " entries)\n";
              return false;
            }
            out.biome[di] = paletteId[p];
          }
        }
      }
      (kind == kPlaneHeight ? out.heightTiles : out.biomeTiles)++;
      tileBytes[{kind, tz, tx}] = std::vector<uint8_t>(raw.begin(), raw.end());
    }
    return true;
  };
  if (!readPlane("height", kPlaneHeight, out.heightPostsX, out.heightPostsZ) ||
      !readPlane("biome", kPlaneBiome, out.biomePostsX, out.biomePostsZ))
    return false;
  // ---- sculpt.svsculpt (P5): the sparse height-offset layer, optional --------
  // Read BEFORE the site-pad bake below: a pad's height is the sculpted ground
  // (BareGroundHeight runs the mirror, which reads out.sculpt). Absent = no
  // layer, and nothing downstream sees a difference; malformed = refuse, like
  // a bad tile -- a half-read layer would be a different world.
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
  // ---- each biome's terrain record for the CPU twin (worldmap.h kB_Curve*) ----
  out.biomeTerrain.assign(set.biomes.size(), {});
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.index >= 0 && b.index < static_cast<int>(set.biomes.size()))
      out.biomeTerrain[static_cast<size_t>(b.index)] = PackBiomeTerrain(b);
  out.biomeTreelineQ8.assign(set.biomes.size(), 256);
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.index >= 0 && b.index < static_cast<int>(set.biomes.size()))
      out.biomeTreelineQ8[static_cast<size_t>(b.index)] = BiomeTreelineQ8(b);
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
  // SITE-cell order, greedy spacing against sites already placed (hand-placed
  // sites first). Rolled once per SITE cell (siteCellLog2; map v2 -- the biome
  // plane is finer, and a roll per biome cell would multiply every density by
  // 16): expected sites per cell = perKm2 * cellVox^2 / 1e8 (a km is 10,000
  // voxels at 0.1 m); the roll is `hash % 1e6 < that * 1e6`. The cell counts
  // as the rule's biome when the biome CELL under its centre is (no warp).
  // minSpacing is voxels and defaults to one site cell.
  if (j.contains("rules") && j["rules"].is_array()) {
    const int cellVox = siteVox;
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
      for (int cz = 0; cz < out.siteH; cz++)
        for (int cx = 0; cx < out.siteW; cx++) {
          const int ccx = ((cx - out.siteOriginX) << out.siteCellLog2) + cellVox / 2;
          const int ccz = ((cz - out.siteOriginZ) << out.siteCellLog2) + cellVox / 2;
          if (out.BiomeCellAt(ccx, ccz) != static_cast<uint8_t>(bi->second)) continue;
          const uint32_t h = rng::Hash3(seed ^ salt ^ 0x51735u, static_cast<uint32_t>(cx),
                                        static_cast<uint32_t>(cz));
          if (h % 1000000u >= thr) continue;
          // the site: cell centre, jittered by up to a quarter cell
          const int jx = static_cast<int>((h >> 20) % static_cast<uint32_t>(cellVox / 2)) - cellVox / 4;
          const int jz = static_cast<int>((h >> 10) % static_cast<uint32_t>(cellVox / 2)) - cellVox / 4;
          const int wx = ccx + jx;
          const int wz = ccz + jz;
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

  // ---- roads[] (map v2 R1, worldmap.h kSiteRoad) -------------------------------
  // Read and validated here; the PIECES are cut twice: once now, a straight
  // split of every polyline segment into <= kSiteRoadPieceMax runs (the
  // geometry the bare ground's pond keep-out reads -- see sitePadNear), and
  // again after the profile bake below, along the same lines, wherever a
  // chord leaves the baked profile. Both cuts cover the same polylines, so the
  // bare ground the bake samples is the same under either.
  if (j.contains("roads")) {
    if (!j["roads"].is_array()) { log += at + "roads must be an array\n"; return false; }
    std::set<std::string> roadIds;
    for (const json& r : j["roads"]) {
      if (!r.is_object()) { log += at + "a roads[] entry that is not an object\n"; return false; }
      WorldMapData::Road rd;
      rd.id = r.value("id", std::string());
      const std::string who = at + "road \"" + rd.id + "\": ";
      if (rd.id.empty()) { log += at + "a roads[] entry needs an id\n"; return false; }
      if (!roadIds.insert(rd.id).second) { log += who + "a second road with this id\n"; return false; }
      rd.name = r.value("name", std::string());
      rd.kind = r.value("kind", std::string("road"));
      if (rd.kind != "road" && rd.kind != "track" && rd.kind != "path") {
        log += who + "kind \"" + rd.kind + "\" is not road, track or path\n";
        return false;
      }
      if (!r.contains("points") || !r["points"].is_array()) {
        log += who + "needs points: [[x, z], ...] in world voxels\n";
        return false;
      }
      for (const json& pt : r["points"]) {
        if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number() || !pt[1].is_number()) {
          log += who + "a point that is not [x, z]\n";
          return false;
        }
        const std::pair<int, int> q{static_cast<int>(std::lround(pt[0].get<double>())),
                                    static_cast<int>(std::lround(pt[1].get<double>()))};
        if (std::abs(q.first) > (1 << 24) || std::abs(q.second) > (1 << 24)) {
          log += who + "a point past +-16M voxels\n";
          return false;
        }
        if (!rd.points.empty() && rd.points.back() == q) continue;   // a repeated vertex is no segment
        rd.points.push_back(q);
      }
      if (rd.points.size() < 2) { log += who + "needs at least two distinct points\n"; return false; }
      // smooth: true (docs/PLAN_worldbuilder.md 2.3): the centreline is THE
      // SPLINE through the points (worldwater.cpp SmoothRoadPoints), and that
      // polyline is what the profile bake below grades; false (the default)
      // keeps the authored polyline, so an existing road does not move.
      if (r.contains("smooth") && r["smooth"].is_boolean() && r["smooth"].get<bool>()) {
        rd.smooth = true;
        rd.points = SmoothRoadPoints(rd.points);
        if (rd.points.size() < 2) { log += who + "its smoothed centreline collapses to one point\n"; return false; }
      }
      // The numbers, each clamped with a warning (the map page's inspector
      // keeps to these ranges; a hand edit may not). Defaults by kind.
      auto num = [&](const char* key, int def, int lo, int hi) -> int {
        int v = def;
        if (r.contains(key) && r[key].is_number()) v = static_cast<int>(std::lround(r[key].get<double>()));
        const int c = std::clamp(v, lo, hi);
        if (c != v)
          warn("road \"" + rd.id + "\": " + key + " " + std::to_string(v) + " clamped to " + std::to_string(c) +
               " (" + std::to_string(lo) + ".." + std::to_string(hi) + ")");
        return c;
      };
      const bool path = rd.kind == "path", track = rd.kind == "track";
      rd.width = num("width", path ? 12 : track ? 24 : 40, 2, 256);
      rd.shoulder = num("shoulder", path ? 6 : track ? 12 : 16, 0, static_cast<int>(kSiteRoadShoulderMax));
      rd.maxGrade = num("maxGrade", path ? 25 : track ? 15 : 10, 1, 100);
      rd.depth = num("depth", static_cast<int>(kSiteRoadDepthDefault), 1, static_cast<int>(kSiteRoadDepthMax));
      rd.surface = r.value("surface", std::string("packed_earth"));
      for (size_t i = 1; i < set.matNames.size(); i++)
        if (set.matNames[i] == rd.surface) rd.surfaceId = static_cast<uint32_t>(i);
      if (rd.surfaceId == 0) {
        log += who + "surface \"" + rd.surface + "\" is not a material in materials.json\n";
        return false;
      }
      // A SOLID, always: a graded road is a slope, and a powder on a slope
      // avalanches out from under itself and never sleeps (CLAUDE.md rule 2).
      if (set.matClass[rd.surfaceId] != CLASS_SOLID) {
        log += who + "surface \"" + rd.surface + "\" is not a solid; a road on a grade needs one "
               "(packed_gravel, packed_earth, cobble, flagstone, ...)\n";
        return false;
      }
      for (const auto& [x, z] : rd.points)
        if (!out.InExtent(x, z)) {
          warn("road \"" + rd.id + "\": point (" + std::to_string(x) + "," + std::to_string(z) +
               ") is outside the map's extent");
          break;
        }
      out.roads.push_back(std::move(rd));
    }
  }
  // ---- rivers[] and lakes[] (docs/PLAN_worldbuilder.md W1, worldwater.h) --------
  // Read and validated here; BAKED (surfaces, reaches, pieces, lake cells)
  // after every other site is in place, at the end of the site-pad block.
  if (!ParseRiversLakes(j, set, out, at, log, warn)) return false;
  // ---- regions[] and markers[] (map v2 E3): data only ---------------------------
  // Nothing in worldgen reads either (they still move contentHash: they are
  // in map.json). A malformed SHAPE (no id, a duplicate id, a point that is
  // not [x, z], a polygon of fewer than three distinct points, a marker
  // without `at`) is a refusal naming the entry, like a road's -- a half-read
  // region would answer RegionAt wrongly forever. A colour that is not
  // "#rrggbb", an icon the page has no art for, a vertex outside the extent
  // are warnings.
  auto xzPair = [](const json& p, std::pair<int, int>& q) {
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) return false;
    const double a = p[0].get<double>(), b = p[1].get<double>();
    if (std::abs(a) > (1 << 24) || std::abs(b) > (1 << 24)) return false;
    q = {static_cast<int>(std::lround(a)), static_cast<int>(std::lround(b))};
    return true;
  };
  if (j.contains("regions")) {
    if (!j["regions"].is_array()) { log += at + "regions must be an array\n"; return false; }
    std::set<std::string> ids;
    for (const json& r : j["regions"]) {
      if (!r.is_object() || !r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty()) {
        log += at + "a regions[] entry needs an id\n";
        return false;
      }
      WorldMapData::Region rg;
      rg.id = r["id"].get<std::string>();
      const std::string who = at + "region \"" + rg.id + "\": ";
      if (!ids.insert(rg.id).second) { log += who + "a second region with this id\n"; return false; }
      rg.name = r.contains("name") && r["name"].is_string() ? r["name"].get<std::string>() : std::string();
      rg.colour = r.contains("colour") && r["colour"].is_string() ? r["colour"].get<std::string>() : std::string();
      bool hex = rg.colour.size() == 7 && rg.colour[0] == '#';
      for (size_t k = 1; hex && k < 7; k++) hex = std::isxdigit(static_cast<unsigned char>(rg.colour[k])) != 0;
      if (!hex) warn("region \"" + rg.id + "\": colour \"" + rg.colour + "\" is not #rrggbb (the map page draws it grey)");
      if (!r.contains("points") || !r["points"].is_array()) {
        log += who + "needs points: [[x, z], ...] in world voxels\n";
        return false;
      }
      for (const json& p : r["points"]) {
        std::pair<int, int> q;
        if (!xzPair(p, q)) { log += who + "a point that is not [x, z] within +-16M voxels\n"; return false; }
        if (!rg.points.empty() && rg.points.back() == q) continue;
        rg.points.push_back(q);
      }
      if (rg.points.size() > 1 && rg.points.front() == rg.points.back()) rg.points.pop_back();   // closed by hand
      if (rg.points.size() < 3) { log += who + "a polygon needs at least three distinct points\n"; return false; }
      rg.x0 = rg.x1 = rg.points[0].first;
      rg.z0 = rg.z1 = rg.points[0].second;
      bool outside = false;
      for (const auto& [x, z] : rg.points) {
        rg.x0 = std::min(rg.x0, x); rg.x1 = std::max(rg.x1, x);
        rg.z0 = std::min(rg.z0, z); rg.z1 = std::max(rg.z1, z);
        outside = outside || !out.InExtent(x, z);
      }
      if (outside) warn("region \"" + rg.id + "\": a vertex is outside the map's extent");
      out.regions.push_back(std::move(rg));
    }
  }
  if (j.contains("markers")) {
    if (!j["markers"].is_array()) { log += at + "markers must be an array\n"; return false; }
    std::set<std::string> ids;
    for (const json& r : j["markers"]) {
      if (!r.is_object() || !r.contains("id") || !r["id"].is_string() || r["id"].get<std::string>().empty()) {
        log += at + "a markers[] entry needs an id\n";
        return false;
      }
      WorldMapData::Marker mk;
      mk.id = r["id"].get<std::string>();
      const std::string who = at + "marker \"" + mk.id + "\": ";
      if (!ids.insert(mk.id).second) { log += who + "a second marker with this id\n"; return false; }
      std::pair<int, int> q;
      if (!r.contains("at") || !xzPair(r["at"], q)) { log += who + "needs at: [x, z] in world voxels\n"; return false; }
      mk.x = q.first;
      mk.z = q.second;
      mk.name = r.contains("name") && r["name"].is_string() ? r["name"].get<std::string>() : std::string();
      mk.icon = r.contains("icon") && r["icon"].is_string() ? r["icon"].get<std::string>() : std::string();
      bool known = false;
      for (const char* ic : kMarkerIcons) known = known || mk.icon == ic;
      if (!known) warn("marker \"" + mk.id + "\": icon \"" + mk.icon + "\" is not one the map page draws");
      if (!out.InExtent(mk.x, mk.z)) warn("marker \"" + mk.id + "\": outside the map's extent");
      out.markers.push_back(std::move(mk));
    }
  }
  // The first cut: every segment in <= kSiteRoadPieceMax straight runs,
  // appended at the END of `sites` (every road after every other kind, so a
  // road's pieces never shift another site's id). Ends only; the bake below
  // fills in the surface and replaces them.
  const size_t roadSite0 = out.sites.size();
  for (size_t ri = 0; ri < out.roads.size(); ri++) {
    const WorldMapData::Road& rd = out.roads[ri];
    for (size_t i = 0; i + 1 < rd.points.size(); i++) {
      const auto [ax, az] = rd.points[i];
      const auto [bx, bz] = rd.points[i + 1];
      const int len = std::max(std::abs(bx - ax), std::abs(bz - az));
      const int n = std::max(1, (len + static_cast<int>(kSiteRoadPieceMax) - 1) / static_cast<int>(kSiteRoadPieceMax));
      for (int k = 0; k < n; k++) {
        WorldMapData::StampSite st;
        st.id = "road " + rd.id + " (uncut)";
        st.kind = kSiteRoad;
        st.road = static_cast<int>(ri);
        st.x = ax + (bx - ax) * k / n;
        st.z = az + (bz - az) * k / n;
        st.x1 = ax + (bx - ax) * (k + 1) / n;
        st.z1 = az + (bz - az) * (k + 1) / n;
        st.radius = std::max(1, rd.width / 2);
        st.padMargin = rd.shoulder;
        st.roadMat = rd.surfaceId;
        st.roadDepth = rd.depth;
        st.salt = 0;
        out.sites.push_back(std::move(st));
      }
    }
  }
  if (!out.roads.empty() && !readTreeSpecies()) return false;   // a road keeps crowns off: the widest one

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
  // Built twice when the map has roads (once here, once after the profile
  // bake re-cuts their pieces), so a lambda.
  // A ROAD piece's reach: the half-width + the widest thing a reader asks
  // from past it -- a rolled pond's centre (the fixed shoulder keep-out + a
  // pond's radius, the 64 a stamp allows too), a trunk's crown, the far tint.
  auto buildIndex = [&]() -> bool {
  for (WorldMapData::StampSite& st : out.sites) {
    if (st.kind == kSiteRiver || st.kind == kSiteLakeEdge || st.kind == kSiteLakeCell)
      st.indexReach = WaterIndexReach(st, treeMaxReach);
    else if (st.kind == kSiteRoad)
      st.indexReach = st.radius + std::max({static_cast<int>(kSiteRoadShoulderMax) + 64, treeMaxReach + 1,
                                            static_cast<int>(kSiteRoadFarTint)}) + 1;
    else if (st.kind == kSiteTree) st.indexReach = st.radius + treeMaxReach;
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
      if (st.kind == kSiteClearing || st.kind == kSiteSoften || st.kind == kSitePath || st.kind == kSiteRoad ||
          st.kind == kSiteRiver || st.kind == kSiteLakeEdge || st.kind == kSiteLakeCell) {
        lx = std::min(st.x, st.x1); hx = std::max(st.x, st.x1);
        lz = std::min(st.z, st.z1); hz = std::max(st.z, st.z1);
      } else if (st.kind == kSiteStamp) {
        lx = st.bx0; lz = st.bz0; hx = st.x1; hz = st.z1;
      }
      int c0x, c0z, c1x, c1z;
      out.CellOf(lx - st.indexReach, lz - st.indexReach, &c0x, &c0z);
      out.CellOf(hx + st.indexReach, hz + st.indexReach, &c1x, &c1z);
      for (int cz = std::max(c0z, 0); cz <= std::min(c1z, out.siteH - 1); cz++)
        for (int cx = std::max(c0x, 0); cx <= std::min(c1x, out.siteW - 1); cx++)
          byCell[static_cast<size_t>(cz) * out.siteW + cx].push_back(static_cast<uint32_t>(si + 1));
    }
    out.siteIndex.assign(static_cast<size_t>(out.siteW) * out.siteH, 0u);
    out.siteLists.assign(1, 0u);   // offset 0 = "no site"
    std::map<std::vector<uint32_t>, uint32_t> shared;
    out.siteCellWorst = 0;
    for (const auto& kv : byCell) {
      const std::vector<uint32_t>& ids = kv.second;
      out.siteCellWorst = std::max(out.siteCellWorst, static_cast<int>(ids.size()));
      if (ids.size() > kSiteCellMax) {
        const int cx = static_cast<int>(kv.first % static_cast<size_t>(out.siteW));
        const int cz = static_cast<int>(kv.first / static_cast<size_t>(out.siteW));
        std::string names;
        for (uint32_t id : ids) names += (names.empty() ? "\"" : ", \"") + out.sites[id - 1].id + "\"";
        log += at + std::to_string(ids.size()) + " sites reach map cell (" + std::to_string(cx) + "," +
               std::to_string(cz) + ") (world x " + std::to_string((cx - out.siteOriginX) << out.siteCellLog2) +
               ", z " + std::to_string((cz - out.siteOriginZ) << out.siteCellLog2) + ", " + std::to_string(siteVox) +
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
  return true;
  };
  if (!buildIndex()) return false;

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
        // (map stamps bake padY below; a locale's houses carry the locale's
        // offset there too, so only two of ONE locale -- or of none -- are
        // known to share a floor yet)
        if (a.structure && b.structure && a.locale == b.locale && a.padY == b.padY) return false;
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
          auto claimsNone = [](int k) {
            return k == kSiteClearing || k == kSiteSoften || k == kSitePath || k == kSiteRoad || k == kSiteRiver ||
                   k == kSiteLakeEdge || k == kSiteLakeCell;
          };
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
        if (s.kind == kSiteSoften || s.kind == kSitePath || s.kind == kSiteRoad || s.kind == kSiteRiver ||
            s.kind == kSiteLakeEdge || s.kind == kSiteLakeCell)
          continue;   // walkable ground or water, said below for water
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
      st.padY = st.kind == kSiteWater || st.kind == kSiteClearing || st.kind == kSiteSoften || st.kind == kSitePath ||
                        st.kind == kSiteRoad
                    ? 0 : BareGroundHeight(st.x, st.z, seed);
    }
    // THE LOCALES' ANCHORS (L1, worldmap.h LocaleDef): the bare ground at each
    // anchor for this seed, and every house of the locale's ref group lifted
    // by the locale's offset (anchor.y + delta) from its anchor-relative file
    // y. Bare ground reads no pad, so the houses being in the table still at
    // their relative floors does not matter; it does read the soften boxes
    // and the routes, which are all in place.
    for (LocaleDef& l : out.locales) {
      l.groundY = BareGroundHeight(l.ax, l.az, seed);
      l.resolved = true;
      std::printf("world map: '%s' locale '%s' anchor (%d,%d): ground y%d, authored y%d, delta %+d\n",
                  name.c_str(), l.id.c_str(), l.ax, l.az, l.groundY, l.ay, l.Delta());
    }
    for (WorldMapData::StampSite& st : out.sites) {
      if (!st.structure || st.locale < 0) continue;
      const int off = out.locales[static_cast<size_t>(st.locale)].RefYOffset();
      st.padY += off;
      st.refY += off;
    }
    // THE DUNGEONS' ANCHORS (PLAN_dungeons §2.1): the bare ground at `at`, the
    // same resolution a locale anchor takes, so the whole dungeon rides the
    // ground AT THE ANCHOR, rigidly. The scratch ISOLATE dungeon names its y.
    const ScratchDungeon* scratch = ScratchDungeonSlot();
    for (DungeonDef& g : out.dungeons) {
      if (!g.shell || !g.res) continue;
      const bool absY = scratch && scratch->absoluteY && !scratch->replaceMapEntry && g.id == scratch->def.id;
      g.groundY = absY ? scratch->anchorY - 1 : BareGroundHeight(g.ax, g.az, seed);
      // PLAN §2.1 / §3.7 (D3's world/dungeon_refs.*): the anchor cell lands at
      // (at.x, bare ground + yOffset, at.z) -- the GROUND cell itself at
      // yOffset 0 -- and the rest at base + R(yaw) (local - anchor).
      const std::array<int, 3> base{g.ax, absY ? scratch->anchorY : g.groundY + g.yOffset, g.az};
      dungeon::Placed pl;
      pl.shell = g.shell;
      pl.res = g.res;
      pl.yawQ = g.yawQ;
      pl.origin = dungeon::OriginFor(*g.shell, g.yawQ, base);
      dungeon::PlacedBounds(pl);
      {
        // A trial pack of this dungeon alone: a program the index cannot hold
        // (a cell over the list limits) is a warning, not a refused map.
        std::vector<uint32_t> trial(kHeaderWords, 0u);
        std::string perr;
        if (!dungeon::PackDungeons({pl}, trial, perr)) {
          warn("dungeon \"" + g.id + "\": " + perr + " -- not placed");
          g.shell.reset();
          g.res.reset();
          continue;
        }
      }
      g.origin = pl.origin;
      g.boxMin = pl.boxMin;
      g.boxMax = pl.boxMax;
      g.keepMin = pl.keepMin;
      g.keepMax = pl.keepMax;
      g.resolved = true;
      std::printf("world map: '%s' dungeon '%s' anchor (%d,%d): ground y%d, origin (%d,%d,%d) yaw %d, box "
                  "(%d,%d,%d)..(%d,%d,%d), %zu prims, %zu props\n",
                  name.c_str(), g.id.c_str(), g.ax, g.az, g.groundY, g.origin[0], g.origin[1], g.origin[2],
                  g.yawQ * 90, g.boxMin[0], g.boxMin[1], g.boxMin[2], g.boxMax[0], g.boxMax[1], g.boxMax[2],
                  g.shell->prims.size(), g.shell->instances.size());
    }
    // ---- THE ROAD PROFILES (map v2 R1, worldmap.h kSiteRoad) ------------------
    // Per road, deterministic integer arithmetic on the BARE ground (no road
    // and no pad in it: landColumnBare), sampled every kRoadStep voxels along
    // the polyline. Low-passed (a moving mean over +-kRoadSmooth samples, 32
    // voxels: a road does not follow the 3 m bumps),
    // then GRADE-LIMITED: `lo` is the largest profile under the smoothed
    // ground whose slope never exceeds maxGrade (a min-plus distance
    // transform, forward then backward), `hi` the smallest one over it, and
    // the road is (lo + hi) / 2 -- itself within maxGrade (the mean of two such
    // profiles, +1 Q8 unit of rounding), cutting every bump and filling every
    // dip by the same amount. Q8 voxels throughout.
    // Then the pieces are RE-CUT along each segment: a chord is kept when the
    // profile strays at most kRoadChordTolQ8 (one voxel) from it and it is <=
    // 256 voxels a side -- a chord between two profile points is within
    // maxGrade by construction, and a half-voxel tolerance cut rolling ground
    // into ~60-voxel pieces (209 for 12 km of test network), which is what
    // fills a village's site cell, else it is split at the sample farthest from it (the middle, when
    // only the length is the problem). The re-cut lies on the first cut's
    // lines to within the samples' integer rounding (a voxel), so the bare
    // ground sampled here is the ground under the final pieces too.
    if (!out.roads.empty()) {
      constexpr int kRoadStep = 4, kRoadSmooth = 8, kRoadChordTolQ8 = 256;
      std::vector<WorldMapData::StampSite> cut;
      for (size_t ri = 0; ri < out.roads.size(); ri++) {
        WorldMapData::Road& rd = out.roads[ri];
        std::vector<int> sx, sz, ss, vtx;   // sample x, z, arc length (voxels); each vertex's sample
        int arc = 0;
        for (size_t i = 0; i + 1 < rd.points.size(); i++) {
          const auto [ax, az] = rd.points[i];
          const auto [bx, bz] = rd.points[i + 1];
          const int64_t dx = bx - ax, dz = bz - az;
          const int len = std::max(1, static_cast<int>(std::llround(std::sqrt(static_cast<double>(dx * dx + dz * dz)))));
          const int n = std::max(1, (len + kRoadStep - 1) / kRoadStep);
          vtx.push_back(static_cast<int>(sx.size()));
          for (int k = 0; k < n; k++) {
            sx.push_back(ax + static_cast<int>(dx * k / n));
            sz.push_back(az + static_cast<int>(dz * k / n));
            ss.push_back(arc + len * k / n);
          }
          arc += len;
        }
        vtx.push_back(static_cast<int>(sx.size()));
        sx.push_back(rd.points.back().first);
        sz.push_back(rd.points.back().second);
        ss.push_back(arc);
        const int N = static_cast<int>(sx.size());
        std::vector<int> g(N), sm(N), pr(N);
        for (int i = 0; i < N; i++) g[i] = BareGroundHeight(sx[i], sz[i], seed) * 256;
        for (int i = 0; i < N; i++) {
          int64_t sum = 0;
          int cnt = 0;
          for (int k = std::max(0, i - kRoadSmooth); k <= std::min(N - 1, i + kRoadSmooth); k++) { sum += g[k]; cnt++; }
          sm[i] = static_cast<int>(sum / cnt);
        }
        // The grade allowance between samples i - 1 and i, Q8 voxels:
        // maxGrade percent of the arc between them (i64: <= 100 * 256 * arc).
        auto allow = [&](int i) {
          return static_cast<int>(static_cast<int64_t>(ss[i] - ss[i - 1]) * rd.maxGrade * 256 / 100);
        };
        std::vector<int> lo = sm, hi = sm;
        for (int i = 1; i < N; i++) {
          lo[i] = std::min(lo[i], lo[i - 1] + allow(i));
          hi[i] = std::max(hi[i], hi[i - 1] - allow(i));
        }
        for (int i = N - 2; i >= 0; i--) {
          lo[i] = std::min(lo[i], lo[i + 1] + allow(i + 1));
          hi[i] = std::max(hi[i], hi[i + 1] - allow(i + 1));
        }
        for (int i = 0; i < N; i++) pr[i] = static_cast<int>((static_cast<int64_t>(lo[i]) + hi[i]) >> 1);
        rd.vertexY.clear();
        rd.vertexGround.clear();
        for (int vi : vtx) {
          rd.vertexY.push_back((pr[vi] + 128) >> 8);
          rd.vertexGround.push_back(g[vi] >> 8);
        }
        // The reported profile: every k-th sample, the last always (map page).
        rd.profS.clear(); rd.profY.clear(); rd.profG.clear();
        const int pk = std::max(1, (N + kRoadProfileMax - 2) / (kRoadProfileMax - 1));
        for (int i = 0; i < N; i += pk) {
          rd.profS.push_back(ss[i]); rd.profY.push_back((pr[i] + 128) >> 8); rd.profG.push_back(g[i] >> 8);
        }
        if ((N - 1) % pk != 0) {
          rd.profS.push_back(ss[N - 1]); rd.profY.push_back((pr[N - 1] + 128) >> 8); rd.profG.push_back(g[N - 1] >> 8);
        }
        // The pieces, segment by segment (a vertex always ends a piece).
        int q = 0;
        rd.worstGradeQ8 = 0;
        for (size_t v = 0; v + 1 < vtx.size(); v++) {
          std::vector<std::pair<int, int>> stack{{vtx[v], vtx[v + 1]}}, done;
          while (!stack.empty()) {
            const auto [a, b] = stack.back();
            stack.pop_back();
            int farK = -1, dev = 0;
            for (int k = a + 1; k < b; k++) {
              const int64_t lin = pr[a] + static_cast<int64_t>(pr[b] - pr[a]) * (ss[k] - ss[a]) / std::max(ss[b] - ss[a], 1);
              const int d = static_cast<int>(std::llabs(pr[k] - lin));
              if (d > dev) { dev = d; farK = k; }
            }
            const bool tooLong =
                std::max(std::abs(sx[b] - sx[a]), std::abs(sz[b] - sz[a])) > static_cast<int>(kSiteRoadPieceMax);
            if (b - a > 1 && (tooLong || dev > kRoadChordTolQ8)) {
              const int m = (dev <= kRoadChordTolQ8 || farK < 0) ? (a + b) / 2 : farK;
              stack.push_back({m, b});   // popped second: the pieces come out in order
              stack.push_back({a, m});
              continue;
            }
            done.push_back({a, b});
          }
          // One SHOULDER per segment (the widest its pieces need) and the
          // joints between them OPEN: the pieces of one straight are then
          // one road to the height blend, with no cap and no step in the
          // shoulder where one piece hands over to the next.
          int segShoulder = rd.shoulder;
          const size_t seg0 = cut.size();
          // THE JOINT CUT (kS_RoadCut): the AUTHORED straight's direction,
          // ~1024 long, one value for every piece of it. A piece's own ends
          // are integer samples, so two pieces of one straight point a hair
          // apart; had each judged "past my open end" by its own direction,
          // a wedge of columns beside the joint would belong to neither (the
          // bare ground standing up as a 1-voxel wall across the shoulder)
          // and its mirror on the other side to both (blended twice: a
          // ridge). One normal and a strict / non-strict split partition the
          // plane exactly (roadProj, bug 2026-10-08_23-42-45).
          int segCut = 1024;
          {
            const double tx = rd.points[v + 1].first - rd.points[v].first;
            const double tz = rd.points[v + 1].second - rd.points[v].second;
            const double tl = std::sqrt(tx * tx + tz * tz);
            if (tl > 0) {
              const int cx = static_cast<int>(std::lround(tx / tl * 1024.0));
              const int cz = static_cast<int>(std::lround(tz / tl * 1024.0));
              segCut = static_cast<int>((static_cast<uint32_t>(cx) & 0xFFFFu) | ((static_cast<uint32_t>(cz) & 0xFFFFu) << 16));
            }
          }
          for (size_t pi = 0; pi < done.size(); pi++) {
            const auto [a, b] = done[pi];
            WorldMapData::StampSite st;
            st.id = "road " + rd.id + " #" + std::to_string(++q);
            st.kind = kSiteRoad;
            st.road = static_cast<int>(ri);
            st.x = sx[a]; st.z = sz[a]; st.x1 = sx[b]; st.z1 = sz[b];
            st.radius = std::max(1, rd.width / 2);
            st.padMargin = rd.shoulder;
            st.roadYA = pr[a]; st.roadYB = pr[b];
            st.roadMat = rd.surfaceId;
            st.roadDepth = rd.depth;
            st.salt = 0;
            const int arcAB = std::max(ss[b] - ss[a], 1);
            rd.worstGradeQ8 = std::max(
                rd.worstGradeQ8, static_cast<int>(std::llabs(static_cast<int64_t>(pr[b] - pr[a])) * 100 / arcAB));
            // A CUT OR A FILL NEVER STEPS MORE THAN A VOXEL A COLUMN: the
            // shoulder widens to twice the largest gap between the road and
            // the bare ground at its outer edge (+2), up to
            // kSiteRoadShoulderMax -- a stamp's ramp rule. Probed every 8
            // voxels along the piece, both sides, at the authored edge.
            const double vx = st.x1 - st.x, vz = st.z1 - st.z;
            const double vl = std::max(std::sqrt(vx * vx + vz * vz), 1.0);
            const double off = st.radius + std::max(st.padMargin, 1);
            int gap = 0;
            const int steps = std::max(1, static_cast<int>(vl) / 8);
            for (int k = 0; k <= steps; k++) {
              const double f = static_cast<double>(k) / steps;
              const int y = (st.roadYA + static_cast<int>(static_cast<int64_t>(st.roadYB - st.roadYA) * k / steps) + 128) >> 8;
              for (int side = -1; side <= 1; side += 2) {
                const int px = static_cast<int>(std::lround(st.x + vx * f - side * vz / vl * off));
                const int pz = static_cast<int>(std::lround(st.z + vz * f + side * vx / vl * off));
                gap = std::max(gap, std::abs(BareGroundHeight(px, pz, seed) - y));
              }
            }
            segShoulder = std::max(segShoulder, std::min(static_cast<int>(kSiteRoadShoulderMax), 2 * gap + 2));
            st.roadFlags = (pi > 0 ? 1 : 0) | (pi + 1 < done.size() ? 2 : 0);
            st.roadCut = segCut;
            cut.push_back(std::move(st));
          }
          for (size_t pi = seg0; pi < cut.size(); pi++) cut[pi].padMargin = segShoulder;
        }
        rd.pieces = q;
      }
      out.sites.resize(roadSite0);
      for (WorldMapData::StampSite& st : cut) out.sites.push_back(std::move(st));
      if (!buildIndex()) {
        Slot() = std::move(prev);
        return false;
      }
      Slot() = out;   // the stamp ramps below read the re-cut map (the same bare ground)
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
    // A tree at or above ITS COLUMN'S treeline does not grow (worldgen.wgsl
    // siteTree). Said, not refused: the author may be about to lower the
    // ground under it. TreelineAt reads the current map, so it is asked here,
    // with the map being loaded installed.
    for (const WorldMapData::StampSite& st : out.sites) {
      if (st.kind != kSiteTree) continue;
      const int tl = TreelineAt(st.x, st.z, seed);
      if (st.padY >= tl)
        warn("site \"" + st.id + "\" (tree) stands on ground y" + std::to_string(st.padY) +
             ", at or above its column's treeline y" + std::to_string(tl) + ": it will not grow");
    }
    // ---- RIVERS AND LAKES (W1, worldwater.cpp "THE RIVER BAKE") ----------------
    // Last: they read the BARE ground (no road, pad or water in it), which no
    // site appended above changes, and their records go at the END of the
    // table (river pieces, lake edges, lake cells), so no other site's id moves.
    if (!out.rivers.empty() || !out.lakes.empty()) {
      if (!out.lakes.empty() && out.siteCellLog2 > 12) {
        log += at + "lakes need siteCellLog2 <= 12 (a lake cell's row bitset)\n";
        Slot() = std::move(prev);
        return false;
      }
      if (!readTreeSpecies()) { Slot() = std::move(prev); return false; }
      std::vector<WorldMapData::StampSite> water;
      if (!BakeRiversLakes(out, seed, water, log, at, warn)) {
        Slot() = std::move(prev);
        return false;
      }
      for (WorldMapData::StampSite& st : water) out.sites.push_back(std::move(st));
      if (!buildIndex()) {
        Slot() = std::move(prev);
        return false;
      }
      // The spawn in the water is a mistake the page should show.
      if (out.spawnAuthored) {
        Slot() = out;
        const WaterColumn wc = WaterColumnAt(out.spawnX, out.spawnZ, seed);
        if (wc.water >= 0)
          warn("the spawn (" + std::to_string(out.spawnX) + "," + std::to_string(out.spawnZ) +
               ") is in a river or a lake (water to y" + std::to_string(wc.water) + " over ground y" +
               std::to_string(wc.h) + ")");
      }
    }
    Slot() = std::move(prev);
  }

  // THE CONTENT HASH: map.json, then every tile file in (plane, tz, tx) order
  // (name + bytes, so moving a tile is a change), then the sculpt layer and
  // the structure refs. biomes.cpp's environment stamp and the save
  // fingerprint hash the same files from disk.
  uint32_t hsh = 2166136261u;
  {
    const std::string js = j.dump();
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(js.data()), js.size());
  }
  for (const auto& [key, bytes] : tileBytes) {
    const std::string tn = std::to_string(std::get<0>(key)) + ":" + std::to_string(std::get<2>(key)) + "_" +
                           std::to_string(std::get<1>(key));
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(tn.data()), tn.size());
    hsh = FnvBytes(hsh, bytes.data(), bytes.size());
  }
  // Only when the file exists, so a map without one reports what it did.
  if (!sculptRaw.empty()) hsh = FnvBytes(hsh, sculptRaw.data(), sculptRaw.size());
  // ...and the structure refs, only when there are any, for the same reason.
  if (out.structureHash != 0) {
    const uint32_t sh = out.structureHash;
    hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(&sh), sizeof sh);
  }
  // ...and each placed dungeon's program (its file bytes; the placement is in
  // map.json already), only when there is one.
  for (const DungeonDef& g : out.dungeons)
    if (g.Placed()) {
      const uint32_t dh = g.shellHash ^ (static_cast<uint32_t>(g.origin[1]) * 0x9E3779B1u);
      hsh = FnvBytes(hsh, reinterpret_cast<const uint8_t*>(&dh), sizeof dh);
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
  // L1: each locale as resolved for the check's seed -- how far the ground at
  // its anchor has moved since it was authored (the locale page shows it).
  json ls = json::array();
  for (const LocaleDef& l : m.locales)
    ls.push_back({{"id", l.id}, {"groundY", l.groundY}, {"anchorY", l.ay}, {"delta", l.Delta()}});
  j["locales"] = ls;
  // PLAN_dungeons: each dungeon as placed for the check's seed (the World map
  // page's dungeons layer draws the box and the anchor).
  json dg = json::array();
  for (const DungeonDef& g : m.dungeons) {
    json o = {{"id", g.id}, {"name", g.name}, {"asset", g.asset}, {"at", {g.ax, g.az}}, {"yOffset", g.yOffset},
              {"yaw", g.yawQ * 90}, {"placed", g.Placed()}};
    if (g.Placed()) {
      o["groundY"] = g.groundY;
      o["origin"] = {g.origin[0], g.origin[1], g.origin[2]};
      o["box"] = {{"min", {g.boxMin[0], g.boxMin[1], g.boxMin[2]}}, {"max", {g.boxMax[0], g.boxMax[1], g.boxMax[2]}}};
      o["prims"] = g.shell->prims.size();
      o["props"] = g.shell->instances.size();
    }
    dg.push_back(o);
  }
  j["dungeons"] = dg;
  j["siteCellMax"] = static_cast<int>(kSiteCellMax);
  j["siteCellWorst"] = m.siteCellWorst;
  // Map v2 R1: each road as the engine baked it -- the map page's profile
  // preview draws vertexY (the surface) against vertexGround (the bare
  // ground) at each polyline vertex, the engine's numbers, not a JS re-bake.
  json roads = json::array();
  for (const WorldMapData::Road& r : m.roads) {
    json rj;
    rj["id"] = r.id;
    rj["kind"] = r.kind;
    rj["surface"] = r.surface;
    rj["width"] = r.width;
    rj["shoulder"] = r.shoulder;
    rj["maxGrade"] = r.maxGrade;
    rj["depth"] = r.depth;
    rj["pieces"] = r.pieces;
    rj["worstGrade"] = r.worstGradeQ8 / 256.0;
    rj["vertexY"] = r.vertexY;
    rj["vertexGround"] = r.vertexGround;
    // E3: the whole baked profile (decimated), so the chart is the bake's
    // own curve between the vertices, not a JS guess at it.
    rj["profile"] = {{"s", r.profS}, {"y", r.profY}, {"ground", r.profG}};
    roads.push_back(rj);
  }
  j["roads"] = roads;
  // W1: each river and lake as the engine baked it -- the map page's preview
  // draws these numbers (the reach surfaces, the waterfalls), not a re-bake.
  json rivers = json::array();
  for (const WorldMapData::River& r : m.rivers) {
    json rj;
    rj["id"] = r.id;
    rj["joins"] = r.joins;
    rj["bed"] = r.bedName;
    rj["stepVox"] = r.stepVox;
    rj["pieces"] = r.pieces;
    rj["reaches"] = r.reaches;
    rj["endLevel"] = r.endLevel;
    rj["lakeIn"] = r.lakeIn >= 0 ? json(m.lakes[static_cast<size_t>(r.lakeIn)].id) : json(nullptr);
    rj["lakeOut"] = r.lakeOut >= 0 ? json(m.lakes[static_cast<size_t>(r.lakeOut)].id) : json(nullptr);
    json falls = json::array();
    for (const auto& f : r.falls) falls.push_back({f[0], f[1], f[2]});
    rj["waterfalls"] = falls;
    rj["profile"] = {{"s", r.profS}, {"y", r.profY}, {"ground", r.profG}};
    rivers.push_back(rj);
  }
  j["rivers"] = rivers;
  json lakes = json::array();
  for (const WorldMapData::Lake& l : m.lakes)
    lakes.push_back({{"id", l.id}, {"level", l.level}, {"band", l.band}, {"outline", static_cast<int>(l.poly.size())},
                     {"cells", l.cells}, {"bbox", {l.x0, l.z0, l.x1, l.z1}}});
  j["lakes"] = lakes;
  // E3: the regions and markers as the loader accepted them.
  json rs = json::array();
  for (const WorldMapData::Region& r : m.regions)
    rs.push_back({{"id", r.id}, {"name", r.name}, {"points", static_cast<int>(r.points.size())},
                  {"bbox", {r.x0, r.z0, r.x1, r.z1}}});
  j["regions"] = rs;
  json ms = json::array();
  for (const WorldMapData::Marker& k : m.markers)
    ms.push_back({{"id", k.id}, {"icon", k.icon}, {"at", {k.x, k.z}}});
  j["markers"] = ms;
  // Map v2: the extent and what the planes hold (authored tiles; the rest
  // are the planes' defaults).
  j["extent"] = {m.extentX0, m.extentZ0, m.extentW, m.extentH};
  j["heightPosts"] = {m.heightPostsX, m.heightPostsZ};
  j["heightTiles"] = m.heightTiles;
  j["biomeTiles"] = m.biomeTiles;
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
  W[kHTreelineMax] = U(TreelineMaxOf(map));
  if (!map.Loaded()) return true;   // P0/P1 tools: records only, planes absent
  W[kHSiteCellLog2] = U(map.siteCellLog2);
  W[kHSiteW] = U(map.siteW);
  W[kHSiteH] = U(map.siteH);
  W[kHSiteOriginX] = U(map.siteOriginX);
  W[kHSiteOriginZ] = U(map.siteOriginZ);
  W[kHSeaLevelY] = U(map.seaLevelY);
  W[kHEdgeFade] = U(map.edgeFadeVox);
  W[kHWarpAmp] = U(map.warpAmpVox);
  W[kHBiomeDefault] = U(map.biomeDefault);
  W[kHPadX0] = U(map.padX0);
  W[kHPadZ0] = U(map.padZ0);
  W[kHPadX1] = U(map.padX1);
  W[kHPadZ1] = U(map.padZ1);
  W[kHExtentX0] = U(map.extentX0);
  W[kHExtentZ0] = U(map.extentZ0);
  W[kHExtentW] = U(map.extentW);
  W[kHExtentH] = U(map.extentH);
  W[kHHeightLog2] = U(map.heightLog2);
  W[kHHeightPostsX] = U(map.heightPostsX);
  W[kHHeightPostsZ] = U(map.heightPostsZ);
  W[kHHeightDefault] = U(map.heightDefault);
  W[kHBiomeLog2] = U(map.biomeLog2);
  W[kHBiomePostsX] = U(map.biomePostsX);
  W[kHBiomePostsZ] = U(map.biomePostsZ);
  // The planes, DENSE (docs/PLAN_map_v2.md "GPU packing"; the loader refused
  // anything past 64 MiB): biome ids four per word, height posts two i16 per
  // word, low half first -- the sculpt block's packing, read by the same
  // sign-extending shift (worldgen.wgsl wmHeightPost).
  W[kHBiomePlane] = U(static_cast<int>(W.size()));
  AppendPlane(W, map.biome);
  W[kHHeightPlane] = U(static_cast<int>(W.size()));
  {
    const size_t at = W.size();
    W.resize(at + (map.height.size() + 1) / 2, 0u);
    for (size_t i = 0; i < map.height.size(); i++)
      W[at + (i >> 1)] |= (static_cast<uint32_t>(static_cast<uint16_t>(map.height[i]))) << ((i & 1) * 16);
  }
  // ---- sites: the index plane + lists, the records, then each stamp block ----
  // One word per cell (the list's buffer offset, 0 = none), then the shared
  // lists. No sites at all packs no plane: kHSiteIndex stays 0 and every
  // reader's list walk ends at its first read.
  if (!map.sites.empty() && map.siteIndex.size() == static_cast<size_t>(map.siteW) * map.siteH) {
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
    if (s.kind == kSiteRoad) {
      r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1);
      r[kS_RoadYA] = U(s.roadYA); r[kS_RoadYB] = U(s.roadYB);
      r[kS_RoadMat] = s.roadMat;
      r[kS_RoadDepth] = U(s.roadDepth);
      r[kS_RoadFlags] = U(s.roadFlags);
      r[kS_RoadCut] = static_cast<uint32_t>(s.roadCut);
    }
    if (s.kind == kSiteRiver) {
      // Every word is the river piece's (worldmap.h kS_Riv*); the generic
      // words written above are re-written, and word 7 stays 0.
      r[kS_Radius] = U(s.radius); r[kS_PadMargin] = U(s.padMargin);
      r[kS_RivHwB] = U(s.hwB);
      r[kS_RivDepthA] = U(s.depthA); r[kS_StampOff] = 0u; r[kS_RivDepthB] = U(s.depthB);
      r[kS_RivSurf] = U(s.surf);
      r[kS_RivBed] = (s.bed & 0xFFFu) | ((static_cast<uint32_t>(s.wflags) & 15u) << 12) |
                     (static_cast<uint32_t>(std::clamp(s.len, 0, 1023)) << 16);
      r[kS_RivLipY] = U(s.lipY);
      r[kS_RivCutA] = static_cast<uint32_t>(s.cutA);
      r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1);
      r[kS_RivCutB] = static_cast<uint32_t>(s.cutB);
      continue;
    }
    if (s.kind == kSiteLakeEdge || s.kind == kSiteLakeCell) {
      r[kS_Radius] = 0u; r[kS_PadMargin] = U(s.padMargin);
      r[kS_Rot] = 0u; r[kS_Salt] = 0u; r[kS_StampOff] = 0u;
      r[kS_LakeId] = U(s.water + 1);
      r[kS_LakeLevel] = U(s.surf);
      r[kS_LakeBed] = s.bed;
      r[kS_LakeShore] = s.shore;
      r[kS_BoxX1] = U(s.x1); r[kS_BoxZ1] = U(s.z1);
      r[kS_LakeFlags] = U(s.wflags);
      r[kS_LakeBits] = 0u;
      if (s.kind == kSiteLakeCell && !s.bits.empty()) {
        const uint32_t base = static_cast<uint32_t>(W.size());
        W.insert(W.end(), s.bits.begin(), s.bits.end());
        W.data()[tab0 + i * kSiteRecWords + kS_LakeBits] = base;   // W moved: re-index
      }
      continue;
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
  // ---- the dungeon block (PLAN_dungeons §3.4; sim/dungeon.h PackDungeons) ----
  // Nothing for a map without placed dungeons: kHDungeon stays 0, the one word
  // worldgen's dungeon layer reads per chunk.
  {
    std::vector<dungeon::Placed> placed;
    for (const DungeonDef& g : map.dungeons) {
      if (!g.Placed()) continue;
      dungeon::Placed pl;
      pl.id = g.id;
      pl.shell = g.shell;
      pl.res = g.res;
      pl.yawQ = g.yawQ;
      pl.origin = g.origin;
      pl.boxMin = g.boxMin;
      pl.boxMax = g.boxMax;
      pl.keepMin = g.keepMin;
      pl.keepMax = g.keepMax;
      placed.push_back(std::move(pl));
    }
    dungeon::PackStats ds;
    if (!dungeon::PackDungeons(placed, W, log, &ds, !DungeonsEnabled())) return false;
    if (!placed.empty())
      std::printf("world map: %zu dungeon(s) packed: %u prims, %u props (%u templates), %u index cells, %u list "
                  "words, hash %u (probe <= %u), %u words, worst list %u\n",
                  placed.size(), ds.prims, ds.instances, ds.templates, ds.indexCells, ds.listWords, ds.hashCap,
                  ds.probeMax, ds.words, ds.worstList);
  }
  W[kHContentHash] = 0u;
  W[kHContentHash] = FnvWords(W) ^ map.contentHash;
  std::printf("world map: '%s' extent (%d,%d)+(%d,%d), height %dx%d posts @%d vox (%d tiles), "
              "biome %dx%d @%d vox (%d tiles), %zu palette, buffer %.1f MiB, content %08x\n",
              map.name.c_str(), map.extentX0, map.extentZ0, map.extentW, map.extentH, map.heightPostsX,
              map.heightPostsZ, 1 << map.heightLog2, map.heightTiles, map.biomePostsX, map.biomePostsZ,
              1 << map.biomeLog2, map.biomeTiles, map.palette.size(), W.size() * 4.0 / (1 << 20),
              W[kHContentHash]);
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

const WorldMapData::Region* WorldMapData::RegionAt(int x, int z) const {
  // Even-odd crossing test at the column CENTRE (x + 1/2, z + 1/2), in
  // doubled coordinates so it stays integer: a ray toward +x crosses edge
  // (a, b) when the edge straddles the centre's z (half-open, so a vertex is
  // counted once) and the crossing lies right of the centre. i64: every
  // coordinate is within +-2^25 doubled, products within 2^52.
  const int64_t px = 2 * static_cast<int64_t>(x) + 1, pz = 2 * static_cast<int64_t>(z) + 1;
  for (size_t ri = regions.size(); ri-- > 0;) {
    const Region& r = regions[ri];
    if (x < r.x0 || x > r.x1 || z < r.z0 || z > r.z1) continue;
    bool in = false;
    const size_t n = r.points.size();
    for (size_t i = 0, k = n - 1; i < n; k = i++) {
      const int64_t ax = 2 * static_cast<int64_t>(r.points[k].first), az = 2 * static_cast<int64_t>(r.points[k].second);
      const int64_t bx = 2 * static_cast<int64_t>(r.points[i].first), bz = 2 * static_cast<int64_t>(r.points[i].second);
      if ((az > pz) == (bz > pz)) continue;
      // crossing x = ax + (pz - az) * (bx - ax) / (bz - az) > px, without the division
      const int64_t lhs = (pz - az) * (bx - ax), rhs = (px - ax) * (bz - az);
      if (bz > az ? lhs > rhs : lhs < rhs) in = !in;
    }
    if (in) return &r;
  }
  return nullptr;
}

std::string RegionAt(int x, int z) {
  const WorldMapData::Region* r = CurrentWorldMap().RegionAt(x, z);
  return r ? (r->name.empty() ? r->id : r->name) : std::string();
}

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
