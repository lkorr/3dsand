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
#include "sim/tuning.h"
#include "sim/world.h"

using nlohmann::json;

namespace worldmap {
namespace {

uint32_t U(int v) { return static_cast<uint32_t>(v); }
uint32_t Vox(float metres) {
  const int v = static_cast<int>(std::lround(metres * kVoxelsPerMetre));
  return U(std::max(1, v));
}

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
  const auto& wg = CurrentTuning().worldgen;

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
    r[kB_TreeTileVox] = Vox(b.treeTileM);
    r[kB_TreeDensity] = U(std::clamp(b.treeDensity, 0, 100));
    // Cave thresholds: the biome's rows override the global knobs, which stay
    // the default so a biome that says nothing about caves keeps today's.
    int t1 = wg.caveThreshold1, t2 = wg.caveThreshold2;
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
    r[kB_Flags] = flags;
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
      count++;
    }
    W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_CoverCount] = U(count);
    W[rec0 + static_cast<size_t>(i) * kBiomeRecWords + kB_MaxCoverH] = maxH;
    W[kHMaxCoverH] = std::max(W[kHMaxCoverH], maxH);
  }
  W[kHContentHash] = 0u;
  W[kHContentHash] = FnvWords(W);
  return true;
}

bool LoadWorldMap(const std::string& assetDir, const std::string& name,
                  const biomes::BiomeSet& set, WorldMapData& out, std::string& log) {
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

  // ---- sites: only the harness pad box is read until P5 ------------------------
  if (j.contains("sites") && j["sites"].is_array()) {
    for (const json& s : j["sites"]) {
      if (!s.is_object() || s.value("kind", "") != "pad") continue;
      if (!(s.contains("min") && s.contains("max") && s["min"].is_array() &&
            s["max"].is_array() && s["min"].size() == 2 && s["max"].size() == 2)) {
        log += at + "site \"" + s.value("id", "?") + "\" kind pad needs min[2]/max[2] in world voxels\n";
        return false;
      }
      out.harnessX0 = s["min"][0].get<int>(); out.harnessZ0 = s["min"][1].get<int>();
      out.harnessX1 = s["max"][0].get<int>(); out.harnessZ1 = s["max"][1].get<int>();
      break;  // one pad until the site table lands
    }
  }

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

}  // namespace worldmap
