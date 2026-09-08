#include "sim/biomes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "sim/world.h"   // kVoxelsPerMetre: the tree lattice is in voxels
#include "sim/worldmap.h" // WaterGeomOf / PondLatticeVox: the pond rows are validated in voxels

namespace fs = std::filesystem;
using nlohmann::json;

namespace biomes {

const char* const kEngineBiomes[kEngineBiomeCount] = {"forest", "meadow", "pine", "desert"};

namespace {

// Tolerant readers: a missing key keeps the default, a wrong type is reported
// once. The tuner writes complete files, but a hand edit should degrade to
// "that row is default" rather than "the engine will not start".
template <typename T>
T Get(const json& j, const char* key, T def) {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) return def;
  try { return it->get<T>(); } catch (...) { return def; }
}
std::string GetS(const json& j, const char* key, const std::string& def = "") {
  if (!j.is_object()) return def;
  auto it = j.find(key);
  if (it == j.end() || !it->is_string()) return def;
  return it->get<std::string>();
}
const json& Sub(const json& j, const char* key) {
  static const json empty = json::object();
  if (!j.is_object()) return empty;
  auto it = j.find(key);
  return (it == j.end() || !it->is_object()) ? empty : *it;
}
const json& Arr(const json& j, const char* key) {
  static const json empty = json::array();
  if (!j.is_object()) return empty;
  auto it = j.find(key);
  return (it == j.end() || !it->is_array()) ? empty : *it;
}

Conditions ReadCond(const json& j) {
  Conditions c;
  const json& s = Sub(j, "conditions");
  c.minY = Get<int>(s, "minY", c.minY);
  c.maxY = Get<int>(s, "maxY", c.maxY);
  c.maxSlope = Get<int>(s, "maxSlope", c.maxSlope);
  c.nearWaterMaxM = Get<float>(s, "nearWaterMax", c.nearWaterMaxM);
  c.nearWaterMinM = Get<float>(s, "nearWaterMin", c.nearWaterMinM);
  c.patchThreshold = Get<int>(s, "patchThreshold", c.patchThreshold);
  c.canopyMin = std::clamp(Get<int>(s, "canopyMin", c.canopyMin), 0, 255);
  c.canopyMax = std::clamp(Get<int>(s, "canopyMax", c.canopyMax), 0, 255);
  return c;
}

bool ReadJsonFile(const fs::path& p, json& out, std::string& log) {
  std::ifstream f(p, std::ios::binary);
  if (!f) { log += "biomes: cannot open " + p.string() + "\n"; return false; }
  std::stringstream ss;
  ss << f.rdbuf();
  try {
    out = json::parse(ss.str());
  } catch (const std::exception& e) {
    log += "biomes: " + p.string() + " does not parse: " + e.what() + "\n";
    return false;
  }
  return true;
}

std::vector<fs::path> JsonFiles(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json" &&
        e.path().filename().string().rfind("_", 0) != 0)   // skip _harness.json and friends
      out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

bool LoadBiomeSet(const std::string& assetDir, const std::vector<MaterialDef>& mats,
                  BiomeSet& out, std::string& log) {
  out = BiomeSet{};
  std::unordered_map<std::string, uint32_t> byName;
  for (size_t i = 0; i < mats.size(); i++) byName[mats[i].name] = static_cast<uint32_t>(i);
  auto matId = [&](const std::string& n) -> uint32_t {
    if (n.empty() || n == "none") return 0;
    auto it = byName.find(n);
    return it == byName.end() ? 0 : it->second;
  };
  bool ok = true;

  // ---- biomes ---------------------------------------------------------------
  for (const fs::path& p : JsonFiles(fs::path(assetDir) / "biomes")) {
    json j;
    if (!ReadJsonFile(p, j, log)) { ok = false; continue; }
    BiomeDef b;
    b.file = p.filename().string();
    b.name = GetS(j, "name", p.stem().string());
    b.displayName = GetS(j, "displayName", b.name);
    b.index = Get<int>(j, "index", -1);
    const json& cl = Sub(j, "climate");
    b.temperature = Get<float>(cl, "temperature", 0.5f);
    b.moisture = Get<float>(cl, "moisture", 0.5f);
    const json& cv = Sub(j, "cover");
    b.skin = GetS(cv, "skin", "grass");
    b.subsoil = GetS(cv, "subsoil", "dirt");
    b.skinId = matId(b.skin);
    b.subsoilId = matId(b.subsoil);
    b.skinDepth = Get<int>(cv, "skinDepth", 1);
    const json& patch = Sub(cv, "patch");
    b.patchThreshold = Get<int>(patch, "threshold", 0);
    b.patchCellLog2 = Get<int>(patch, "cellLog2", 5);
    b.groundFlora = Get<bool>(cv, "groundFlora", true);
    b.cacti = Get<bool>(cv, "cacti", false);
    b.sandCap = Get<bool>(cv, "sandCap", false);
    b.cactusChance = Get<int>(cv, "cactusChance", 0);
    b.saguaroFraction = Get<int>(cv, "saguaroFraction", 0);
    for (const json& r : Arr(cv, "plants")) {
      CoverRow row;
      row.material = GetS(r, "material");
      row.head = GetS(r, "head");
      row.materialId = matId(row.material);
      row.headId = matId(row.head);
      row.chance = Get<int>(r, "chance", 0);
      row.heightM = Get<float>(r, "height", 0.3f);
      row.cond = ReadCond(r);
      b.cover.push_back(row);
    }
    const json& tr = Sub(j, "trees");
    b.treeTileM = Get<float>(tr, "tile", 14.4f);
    b.treeDensity = Get<int>(tr, "density", 0);
    for (const json& r : Arr(tr, "species")) {
      TreeRow row;
      row.species = GetS(r, "species");
      row.weight = Get<int>(r, "weight", 0);
      row.cond = ReadCond(r);
      b.trees.push_back(row);
    }
    for (const json& r : Arr(Sub(j, "water"), "features")) {
      WaterRow row;
      row.preset = GetS(r, "preset");
      row.tileM = Get<float>(r, "tile", 44.8f);
      row.rarity = Get<int>(r, "rarity", 0);
      row.cond = ReadCond(r);
      b.water.push_back(row);
    }
    for (const json& r : Arr(Sub(j, "caves"), "features")) {
      CaveRow row;
      row.preset = GetS(r, "preset", "near_surface");
      row.threshold = Get<int>(r, "threshold", 0);
      row.rarity = Get<int>(r, "rarity", 0);
      row.mushroomChance = Get<int>(r, "mushroomChance", 0);
      row.crystalChance = Get<int>(r, "crystalChance", 0);
      row.cond = ReadCond(r);
      b.caves.push_back(row);
    }
    // P-G: the relief record. A file without it (or with a short curve)
    // keeps the identity, which is exactly what a biome that says nothing
    // about its terrain means: the map's relief, unchanged.
    {
      const json& te = Sub(j, "terrain");
      const json& cv = te.contains("curve") ? te["curve"] : json();
      if (cv.is_array() && cv.size() == 9) {
        bool all = true;
        for (const json& k : cv) all = all && k.is_number();
        if (all)
          for (int i = 0; i < 9; i++) b.terrain.curve[i] = std::clamp(cv[static_cast<size_t>(i)].get<int>(), -16384, 16384);
      }
      b.terrain.hill = std::clamp(Get<int>(te, "hill", 256), 0, 4096);
      b.terrain.detail = std::clamp(Get<int>(te, "detail", 256), 0, 4096);
      b.terrain.grain = std::clamp(Get<int>(te, "grain", 256), 0, 4096);
    }
    out.biomes.push_back(std::move(b));
  }

  // ---- water presets ----------------------------------------------------------------
  for (const fs::path& p : JsonFiles(fs::path(assetDir) / "water")) {
    json j;
    if (!ReadJsonFile(p, j, log)) { ok = false; continue; }
    WaterPresetDef w;
    w.file = p.filename().string();
    w.name = GetS(j, "name", p.stem().string());
    w.displayName = GetS(j, "displayName", w.name);
    w.kind = GetS(j, "kind", "lake");
    const json& fp = Sub(j, "footprint");
    w.radiusM = Get<float>(fp, "radius", 0.f);
    w.radiusVM = Get<float>(fp, "radiusV", 0.f);
    const json& ba = Sub(j, "bathymetry");
    w.depthM = Get<float>(ba, "depth", 0.f);
    w.rimDepthM = Get<float>(ba, "rimDepth", 0.f);
    const json& fi = Sub(j, "fill");
    w.fill = GetS(fi, "material", "water");
    w.fillId = matId(w.fill);
    w.levelM = Get<float>(fi, "level", 0.f);
    const json& be = Sub(j, "berm");
    w.bermHeightM = Get<float>(be, "height", 0.f);
    w.bermWidthM = Get<float>(be, "width", 0.f);
    const json& sh = Sub(j, "shore");
    w.shoreBandM = Get<float>(sh, "band", 0.f);
    w.shoreLiftM = Get<float>(sh, "lift", 0.f);
    const json& pl = Sub(j, "placement");
    w.tileM = Get<float>(pl, "tile", 0.f);
    w.rarity = Get<int>(pl, "rarity", 0);
    w.maxSlope = Get<int>(pl, "maxSlope", 0);
    w.minY = Get<int>(pl, "minY", -1);
    w.maxY = Get<int>(pl, "maxY", -1);
    auto add = [&](const std::string& n) {
      if (n.empty() || n == "none") return;
      w.materials.push_back(n);
      if (!matId(n)) w.unresolved.push_back(n);
    };
    add(w.fill); add(GetS(fi, "surfaceMaterial"));
    const json& bd = Sub(j, "bed");
    add(GetS(bd, "shallow")); add(GetS(bd, "deep")); add(GetS(bd, "substrate"));
    const json& gr = Sub(j, "ground");
    add(GetS(gr, "skin")); add(GetS(gr, "soil")); add(GetS(gr, "rock"));
    add(GetS(sh, "mudMaterial")); add(GetS(sh, "mossMaterial"));
    // ---- the geometry half (P-F): the bed, the mud ring, the profile ----
    // The profile is read the way watergen.js sanitizeProfile reads it:
    // finite pairs, clamped to [0,1], sorted by u, pinned to u = 0 and u = 1,
    // exact-duplicate u collapsed. worldmap::WaterGeomOf samples it.
    w.bedShallow = GetS(bd, "shallow");
    w.bedDeep = GetS(bd, "deep");
    w.bedSubstrate = GetS(bd, "substrate");
    w.bedShallowId = matId(w.bedShallow);
    w.bedDeepId = matId(w.bedDeep);
    w.bedSubstrateId = matId(w.bedSubstrate);
    w.bedShallowDepthM = Get<float>(bd, "shallowDepth", 0.f);
    w.bedThicknessM = Get<float>(bd, "thickness", 0.3f);
    w.mudWidthM = Get<float>(sh, "mudWidth", 0.f);
    w.mudMaterial = GetS(sh, "mudMaterial");
    w.mudId = matId(w.mudMaterial);
    {
      std::vector<std::pair<float, float>> pts;
      for (const json& q : Arr(ba, "profile")) {
        if (!q.is_array() || q.size() < 2 || !q[0].is_number() || !q[1].is_number()) continue;
        const float u = q[0].get<float>(), v = q[1].get<float>();
        if (!std::isfinite(u) || !std::isfinite(v)) continue;
        pts.emplace_back(std::clamp(u, 0.f, 1.f), std::clamp(v, 0.f, 1.f));
      }
      std::stable_sort(pts.begin(), pts.end(),
                       [](const auto& a, const auto& b) { return a.first < b.first; });
      if (pts.empty()) { pts.emplace_back(0.f, 1.f); pts.emplace_back(1.f, 0.f); }
      if (pts.front().first > 0.f) pts.insert(pts.begin(), {0.f, pts.front().second});
      if (pts.back().first < 1.f) pts.emplace_back(1.f, pts.back().second);
      pts.front().first = 0.f; pts.back().first = 1.f;
      std::vector<std::pair<float, float>> dedup;
      dedup.push_back(pts[0]);
      for (size_t i = 1; i < pts.size(); i++) {
        if (pts[i].first - dedup.back().first > 1e-4f) dedup.push_back(pts[i]);
        else dedup.back() = pts[i];
      }
      w.profile = std::move(dedup);
    }
    // ---- the flora half (P-E), the part worldgen reads today ----
    w.mossChance = Get<int>(sh, "mossChance", 0);
    w.mossMaterial = GetS(sh, "mossMaterial");
    w.mossId = matId(w.mossMaterial);
    for (const json& r : Arr(sh, "plants")) {
      ShorePlantRow row;
      row.material = GetS(r, "material");
      row.head = GetS(r, "head");
      row.materialId = matId(row.material);
      row.headId = matId(row.head);
      row.chance = Get<int>(r, "chance", 0);
      row.reachM = Get<float>(r, "reach", 0.f);
      row.heightM = Get<float>(r, "height", 0.3f);
      add(row.material); add(row.head);
      w.shorePlants.push_back(row);
    }
    const json& aq = Sub(j, "aquatic");
    auto band = [&](const json& s, AquaticBand& o) {
      o.material = GetS(s, "material");
      o.flower = GetS(s, "flower");
      o.materialId = matId(o.material);
      o.flowerId = matId(o.flower);
      o.chance = Get<int>(s, "chance", 0);
      o.flowerChance = Get<int>(s, "flowerChance", 0);
      o.minDepthM = Get<float>(s, "minDepth", 0.f);
      o.maxDepthM = Get<float>(s, "maxDepth", 0.f);
      o.heightM = Get<float>(s, "height", 0.f);
      o.clearanceM = Get<float>(s, "clearance", 0.f);
      add(o.material); add(o.flower);
    };
    band(Sub(aq, "emergent"), w.emergent);
    band(Sub(aq, "floating"), w.floating);
    band(Sub(aq, "submerged"), w.submerged);
    out.water.push_back(std::move(w));
  }

  // ---- species mirrors ----------------------------------------------------------------
  for (const fs::path& p : JsonFiles(fs::path(assetDir) / "trees")) {
    json j;
    if (!ReadJsonFile(p, j, log)) { ok = false; continue; }
    SpeciesMirror s;
    s.name = p.stem().string();
    const json& bi = Sub(Sub(j, "placement"), "biomes");
    for (int i = 0; i < kEngineBiomeCount; i++) s.biome[i] = Get<int>(bi, kEngineBiomes[i], 0);
    std::error_code ec;
    s.hasAtlas = fs::exists(fs::path(p).replace_extension(".svtree"), ec);
    out.species.push_back(std::move(s));
  }
  return ok;
}

const BiomeDef* BiomeById(const BiomeSet& set, int id) {
  for (const BiomeDef& b : set.biomes) if (b.index == id) return &b;
  return nullptr;
}

int TreeTileVox(const BiomeDef& b) {
  // The same rounding worldmap.cpp's Vox() applies to every authored metre.
  const int v = static_cast<int>(std::lround(b.treeTileM * kVoxelsPerMetre));
  return std::max(kTreeTileFloorVox, v);
}

int FinestTreeTileVox(const BiomeSet& set) {
  int best = 0;
  for (const BiomeDef& b : set.biomes) {
    if (b.treeDensity <= 0 || b.trees.empty()) continue;   // grows nothing: no vote
    const int t = TreeTileVox(b);
    if (best == 0 || t < best) best = t;
  }
  return best == 0 ? kTreeTileDefaultVox : best;
}

uint32_t TreeChanceQ16(const BiomeDef& b, int latticeVox) {
  const int density = std::clamp(b.treeDensity, 0, 100);
  if (density == 0 || b.trees.empty()) return 0u;
  const int64_t T = std::max(1, latticeVox);
  const int64_t tile = std::max<int64_t>(T, TreeTileVox(b));   // never finer than the lattice
  // density/100 * (T/tile)^2 * 65536, rounded, all in i64: density <= 100,
  // T^2 <= 512^2, so the numerator stays under 2^45.
  const int64_t num = static_cast<int64_t>(density) * T * T * 65536 + 50 * tile * tile;
  const int64_t den = 100 * tile * tile;
  return static_cast<uint32_t>(std::min<int64_t>(65536, num / den));
}

// ---- the environment stamp ----------------------------------------------------
// MIRRORED by scripts/tuner_server.py `_fnv_file_set`: same seed, same prime,
// same (name, 0, bytes) sequence, same byte-order sort. A drift here shows as
// a permanently STALE badge in the tuner, which is loud enough.
uint32_t HashFileSet(const std::string& dir, const std::vector<std::string>& exts) {
  std::vector<fs::path> files;
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string ext = e.path().extension().string();
      bool want = exts.empty();
      for (const std::string& x : exts) want = want || ext == x;
      if (want) files.push_back(e.path());
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  });
  uint32_t h = 2166136261u;
  auto mix = [&](const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  };
  for (const fs::path& p : files) {
    const std::string name = p.filename().string();
    mix(reinterpret_cast<const unsigned char*>(name.data()), name.size());
    const unsigned char zero = 0;
    mix(&zero, 1);
    std::ifstream f(p, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    mix(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
  }
  return h;
}

EnvironmentStamp StampEnvironment(const std::string& assetDir, const std::string& mapName) {
  EnvironmentStamp s;
  s.mapName = mapName;
  s.map = HashFileSet(assetDir + "/worldmap/" + mapName, {".json", ".svmap"});
  // The water presets are a worldgen input since P-E (their flora rows are
  // packed into the same buffer as the biome records), so they are part of
  // the `biomes` stamp: an edited preset must light the apply button too.
  s.biomes = HashFileSet(assetDir + "/biomes", {".json"}) ^
             HashFileSet(assetDir + "/water", {".json"});
  s.trees = HashFileSet(assetDir + "/trees", {".json", ".svtree"});
  return s;
}

std::string EnvironmentStamp::Line() const {
  char buf[256];
  std::snprintf(buf, sizeof buf, "environment: map %s %08x | biomes %08x | trees %08x",
                mapName.c_str(), map, biomes, trees);
  return buf;
}

std::string EnvironmentStamp::Json() const {
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "{\"map\":\"%s\",\"mapHash\":\"%08x\",\"biomesHash\":\"%08x\",\"treesHash\":\"%08x\"}",
                mapName.c_str(), map, biomes, trees);
  return buf;
}

int ValidateBiomeSet(const BiomeSet& set, std::vector<std::string>& out) {
  int n = 0;
  auto bad = [&](const std::string& s) { out.push_back(s); n++; };
  std::unordered_map<std::string, const SpeciesMirror*> species;
  for (const SpeciesMirror& s : set.species) species[s.name] = &s;
  std::unordered_map<std::string, const WaterPresetDef*> water;
  for (const WaterPresetDef& w : set.water) water[w.name] = &w;

  if (set.biomes.empty()) bad("no biome files (assets/biomes/*.json) — run node scripts/seed_environment.mjs --seed");
  // THE ID SPACE IS THE FILES. `index` is the biome's id everywhere -- the
  // record slot in the worldMap buffer, the row in the tree atlas's weight
  // table, the value `Col.biome` carries -- so the indices must be exactly
  // 0..N-1 with no gap and no duplicate. The four the shader still names by
  // id (B_FOREST..B_DESERT, until P2 retires them) must keep those ids.
  {
    const int n = static_cast<int>(set.biomes.size());
    std::vector<int> seen(static_cast<size_t>(n), 0);
    for (const BiomeDef& b : set.biomes) {
      if (b.index < 0 || b.index >= n)
        bad("biomes/" + b.file + ": index " + std::to_string(b.index) + " is outside 0.." + std::to_string(n - 1) +
            " -- ids must be contiguous, one per file");
      else if (seen[b.index]++)
        bad("biomes/" + b.file + ": index " + std::to_string(b.index) + " is used by another biome file");
    }
    for (int i = 0; i < kEngineBiomeCount; i++) {
      const BiomeDef* b = BiomeById(set, i);
      if (!b) bad(std::string("biome id ") + std::to_string(i) + " must be \"" + kEngineBiomes[i] + "\" (worldgen.wgsl still names it) and has no file");
      else if (b->name != kEngineBiomes[i])
        bad("biomes/" + b->file + ": index " + std::to_string(i) + " belongs to \"" + kEngineBiomes[i] + "\" while worldgen.wgsl names it by id");
    }
  }

  for (const BiomeDef& b : set.biomes) {
    const std::string at = "biomes/" + b.file + ": ";
    if (b.name.empty() || b.name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
      bad(at + "name must be [a-z0-9_], got \"" + b.name + "\"");
    if (b.file != b.name + ".json") bad(at + "file name does not match name \"" + b.name + "\"");
    if (!b.skinId && b.skin != "air") bad(at + "cover.skin \"" + b.skin + "\" is not a material");
    if (!b.subsoilId && b.subsoil != "air") bad(at + "cover.subsoil \"" + b.subsoil + "\" is not a material");
    for (size_t i = 0; i < b.cover.size(); i++) {
      const CoverRow& r = b.cover[i];
      if (!r.materialId) bad(at + "cover.plants[" + std::to_string(i) + "] material \"" + r.material + "\" is not a material");
      if (!r.head.empty() && !r.headId) bad(at + "cover.plants[" + std::to_string(i) + "] head \"" + r.head + "\" is not a material");
      if (r.chance < 0) bad(at + "cover.plants[" + std::to_string(i) + "] chance < 0");
    }
    std::unordered_map<std::string, int> seen;
    for (size_t i = 0; i < b.trees.size(); i++) {
      const TreeRow& r = b.trees[i];
      auto it = species.find(r.species);
      if (it == species.end()) bad(at + "trees.species[" + std::to_string(i) + "] \"" + r.species + "\" has no assets/trees/<name>.json");
      else if (!it->second->hasAtlas) bad(at + "trees.species[" + std::to_string(i) + "] \"" + r.species + "\" has no baked .svtree — node scripts/bake_trees.mjs");
      if (seen[r.species]++) bad(at + "trees.species lists \"" + r.species + "\" twice");
      if (r.weight < 0) bad(at + "trees.species[" + std::to_string(i) + "] weight < 0");
    }
    {
      int live = 0;
      for (const WaterRow& r : b.water) if (r.rarity > 0 && r.tileM > 0 && water.count(r.preset)) live++;
      if (live > static_cast<int>(worldmap::kWaterRowsMax))
        bad(at + "water.features has " + std::to_string(live) + " rows that roll; worldgen rolls at most " +
            std::to_string(worldmap::kWaterRowsMax) + " per biome (the rest are dropped)");
    }
    for (size_t i = 0; i < b.water.size(); i++) {
      const WaterRow& r = b.water[i];
      const std::string row = at + "water.features[" + std::to_string(i) + "] ";
      auto it = water.find(r.preset);
      if (it == water.end()) { bad(row + "preset \"" + r.preset + "\" has no assets/water/<name>.json"); continue; }
      if (r.tileM <= 0) bad(row + "tile must be > 0");
      if (r.rarity < 0) bad(row + "rarity < 0");
      // P-F: a rolled disc never leaves its own tile (pondInfo's inset), so
      // the row's tile has to hold the preset's WIDEST disc plus the margin
      // the inset keeps, or the row can never place anything -- which the
      // page would show as a rarity that does nothing.
      const worldmap::WaterGeom g = worldmap::WaterGeomOf(*it->second);
      const int tileVox = static_cast<int>(std::lround(r.tileM * kVoxelsPerMetre));
      const int need = 2 * (g.radiusMin + g.radiusSpan - 1 + 4) + 1;
      if (r.rarity > 0 && tileVox < need)
        bad(row + "tile " + std::to_string(r.tileM) + " m cannot hold a \"" + r.preset + "\" disc (max radius " +
            std::to_string(g.radiusMin + g.radiusSpan - 1) + " vox): needs >= " +
            std::to_string(need / static_cast<float>(kVoxelsPerMetre)) + " m");
      // The shore/berm band scans at most one neighbouring tile per axis
      // (pondNear), which is sound only while the band is under half the
      // LATTICE -- and the lattice is the finest tile of any biome.
      const int lattice = worldmap::PondLatticeVox(set);
      if (r.rarity > 0 && lattice > 0 && g.band > lattice / 2 - 1)
        bad(row + "preset \"" + r.preset + "\" shore/berm band " + std::to_string(g.band) +
            " vox exceeds half the pond lattice (" + std::to_string(lattice) + " vox, the finest water tile of any biome)");
    }
    for (size_t i = 0; i < b.caves.size(); i++) {
      if (b.caves[i].preset != "near_surface" && b.caves[i].preset != "deep")
        bad(at + "caves.features[" + std::to_string(i) + "] preset must be near_surface or deep");
      if (b.caves[i].mushroomChance < 0 || b.caves[i].crystalChance < 0)
        bad(at + "caves.features[" + std::to_string(i) + "] mushroomChance / crystalChance must be >= 0 (0 = never)");
    }
    if (b.cactusChance < 0 || b.cactusChance > 100)
      bad(at + "cover.cactusChance is a percent of tiles, got " + std::to_string(b.cactusChance));
    if (b.saguaroFraction < 0 || b.saguaroFraction > 100)
      bad(at + "cover.saguaroFraction is a percent of cacti, got " + std::to_string(b.saguaroFraction));
  }

  // No species-mirror check any more: since P1 of the world map the tree
  // atlas builds its weight table from THESE files at load (treeatlas.cpp),
  // and the .svtree's baked weight words are not read. `placement.biomes` in
  // a species file is now informational, kept for the tree page's display.

  for (const WaterPresetDef& w : set.water) {
    const std::string at = "water/" + w.file + ": ";
    if (w.file != w.name + ".json") bad(at + "file name does not match name \"" + w.name + "\"");
    if (w.radiusM <= 0) bad(at + "footprint.radius must be > 0");
    if (w.depthM <= 0) bad(at + "bathymetry.depth must be > 0");
    if (w.rimDepthM > w.depthM + 1e-6f) bad(at + "rimDepth exceeds depth");
    // P-F: the geometry the engine carves. The band has two ceilings of its
    // own (pondNear's 8-step bisection resolves 0..255; half the lattice is
    // checked per biome row above), and the bed thickness must be a cell.
    {
      const worldmap::WaterGeom g = worldmap::WaterGeomOf(w);
      if (w.shoreBandM * kVoxelsPerMetre > 255.f)
        bad(at + "shore.band > 25.5 m exceeds pondNear's 8-step bisection");
      if (w.bermWidthM * kVoxelsPerMetre > 255.f)
        bad(at + "berm.width > 25.5 m exceeds pondNear's 8-step bisection");
      if (w.radiusVM < 0) bad(at + "footprint.radiusV must be >= 0");
      if (w.radiusM - w.radiusVM < 0.4f)
        bad(at + "footprint.radius - radiusV must be >= 0.4 m (a 4-voxel disc)");
      if (w.depthM > 20.f) bad(at + "bathymetry.depth > 20 m");
      if (w.bedThicknessM < 0) bad(at + "bed.thickness must be >= 0");
      // Not a refusal: a bowl face steeper than the CA's angle of repose gets
      // the substrate instead of the powder bed (genCellIn's bedSolid), so a
      // steep authored tarn stays settled. Said once here so the author
      // knows why the bed is stone on the walls.
      const int steep = worldmap::WaterGeomSteepestQ8(g, g.radiusMin);
      if (steep > 256 && g.bedSubstrate == 0 && (g.bedShallow || g.bedDeep))
        bad(at + "bathymetry is steeper than one voxel per column at the smallest radius (" +
            std::to_string(steep / 256.0) + " vox/col) and bed.substrate names no material -- "
            "the powder bed on those faces would avalanche forever (rule 2); name a solid substrate or flatten the profile");
    }
    const bool wet = !w.fill.empty() && w.fill != "none";
    if (wet && !w.fillId) bad(at + "fill.material \"" + w.fill + "\" is not a material");
    for (const std::string& u : w.unresolved)
      if (u != w.fill) bad(at + "names material \"" + u + "\", which materials.json does not have");
    if (wet && w.bermHeightM > w.shoreLiftM && w.shoreBandM > 0)
      bad(at + "berm.height " + std::to_string(w.bermHeightM) + " exceeds shore.lift " + std::to_string(w.shoreLiftM) +
          " — no column near this body can be shore (the engine's pondBerm/shoreLift rule)");
    // The flora half: a row that is ON (chance > 0) must name a real material,
    // because the packer drops it otherwise and the page would show a plant
    // the world does not have. Chances are modulo divisors: never negative.
    if (w.mossChance < 0) bad(at + "shore.mossChance must be >= 0");
    for (size_t i = 0; i < w.shorePlants.size(); i++) {
      const ShorePlantRow& r = w.shorePlants[i];
      const std::string row = at + "shore.plants[" + std::to_string(i) + "] ";
      if (r.chance < 0) bad(row + "chance must be >= 0");
      if (r.chance > 0 && !r.materialId) bad(row + "material \"" + r.material + "\" is not a material");
      if (r.reachM < 0 || r.heightM <= 0) bad(row + "reach must be >= 0 and height > 0");
    }
    auto bandOk = [&](const char* name, const AquaticBand& o, bool depthBand) {
      const std::string row = at + std::string("aquatic.") + name + " ";
      if (o.chance < 0 || o.flowerChance < 0) bad(row + "chance must be >= 0");
      if (o.chance > 0 && !o.materialId) bad(row + "material \"" + o.material + "\" is not a material");
      if (o.chance > 0 && depthBand && o.maxDepthM < o.minDepthM)
        bad(row + "maxDepth " + std::to_string(o.maxDepthM) + " is below minDepth " + std::to_string(o.minDepthM) + " — the band is empty");
    };
    bandOk("emergent", w.emergent, true);
    bandOk("floating", w.floating, true);
    bandOk("submerged", w.submerged, false);
  }
  return n;
}

}  // namespace biomes
