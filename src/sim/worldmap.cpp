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

  // ---- sites: the harness pad box, and the stamp sites ------------------------
  bool havePad = false;
  if (j.contains("sites") && j["sites"].is_array()) {
    for (const json& s : j["sites"]) {
      if (!s.is_object()) continue;
      const std::string kind = s.value("kind", "");
      const std::string id = s.value("id", "?");
      if (kind == "pad") {
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
      }
    }
  }
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
    const int reach = st.radius + st.padMargin;
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
    r[kS_Kind] = kSiteStamp;
    r[kS_X] = U(s.x); r[kS_Z] = U(s.z);
    r[kS_Radius] = U(s.radius);
    r[kS_PadMargin] = U(s.padMargin);
    r[kS_Rot] = U(s.rot);
    r[kS_Salt] = s.salt;
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

}  // namespace worldmap
