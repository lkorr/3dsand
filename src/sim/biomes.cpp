#include "sim/biomes.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

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
      row.cond = ReadCond(r);
      b.caves.push_back(row);
    }
    const json& ov = Sub(Sub(j, "terrain"), "overrides");
    for (auto it = ov.begin(); it != ov.end(); ++it)
      if (it->is_number()) b.terrainOverrides[it.key()] = it->get<double>();
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
    for (const json& r : Arr(sh, "plants")) { add(GetS(r, "material")); add(GetS(r, "head")); }
    const json& aq = Sub(j, "aquatic");
    add(GetS(Sub(aq, "emergent"), "material"));
    add(GetS(Sub(aq, "floating"), "material")); add(GetS(Sub(aq, "floating"), "flower"));
    add(GetS(Sub(aq, "submerged"), "material"));
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
  s.biomes = HashFileSet(assetDir + "/biomes", {".json"});
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
    for (size_t i = 0; i < b.water.size(); i++) {
      const WaterRow& r = b.water[i];
      if (!water.count(r.preset)) bad(at + "water.features[" + std::to_string(i) + "] preset \"" + r.preset + "\" has no assets/water/<name>.json");
      if (r.tileM <= 0) bad(at + "water.features[" + std::to_string(i) + "] tile must be > 0");
      if (r.rarity < 0) bad(at + "water.features[" + std::to_string(i) + "] rarity < 0");
    }
    for (size_t i = 0; i < b.caves.size(); i++)
      if (b.caves[i].preset != "near_surface" && b.caves[i].preset != "deep")
        bad(at + "caves.features[" + std::to_string(i) + "] preset must be near_surface or deep");
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
    const bool wet = !w.fill.empty() && w.fill != "none";
    if (wet && !w.fillId) bad(at + "fill.material \"" + w.fill + "\" is not a material");
    for (const std::string& u : w.unresolved)
      if (u != w.fill) bad(at + "names material \"" + u + "\", which materials.json does not have");
    if (wet && w.bermHeightM > w.shoreLiftM && w.shoreBandM > 0)
      bad(at + "berm.height " + std::to_string(w.bermHeightM) + " exceeds shore.lift " + std::to_string(w.shoreLiftM) +
          " — no column near this body can be shore (the engine's pondBerm/shoreLift rule)");
  }
  return n;
}

}  // namespace biomes
