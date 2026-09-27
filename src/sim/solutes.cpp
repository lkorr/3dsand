#include "sim/solutes.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sim/materials.h"

using nlohmann::json;

namespace {

int FindMat(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

uint32_t ParseHex(const std::string& s, bool& ok) {
  ok = s.size() == 7 && s[0] == '#';
  if (!ok) return 0;
  return (uint32_t)std::strtoul(s.c_str() + 1, nullptr, 16);
}

}  // namespace

bool LoadSolutes(const std::string& path, const std::vector<MaterialDef>& mats,
                 std::vector<SoluteDef>& out, std::string& errors) {
  out.clear();
  std::ifstream f(path);
  if (!f) return true;  // no table = nothing dissolves
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    errors += path + ": JSON parse error: " + e.what() + "\n";
    return false;
  }
  if (!j.contains("species") || !j["species"].is_array()) {
    errors += path + ": missing top-level \"species\" array\n";
    return false;
  }
  const size_t before = errors.size();
  for (const json& r : j["species"]) {
    if (!r.is_object() || !r.contains("name")) continue;  // comment rows
    SoluteDef d;
    d.name = r.value("name", std::string{});
    const std::string where = path + ": species \"" + d.name + "\": ";
    auto mat = [&](const char* key, bool required) -> uint16_t {
      if (!r.contains(key)) {
        if (required) errors += where + "missing \"" + key + "\"\n";
        return 0;
      }
      const std::string n = r[key].get<std::string>();
      const int id = FindMat(mats, n);
      if (id <= 0) errors += where + "unknown material \"" + n + "\" in \"" + key + "\"\n";
      return (uint16_t)(id > 0 ? id : 0);
    };
    d.from = mat("from", true);
    d.precipitatesTo = mat("precipitatesTo", false);
    d.yieldPerVoxel = r.value("yieldPerVoxel", 256u);
    d.saturation = std::min(255u, r.value("saturation", 255u));
    d.dissolveChance = std::min(1000u, r.value("dissolveChance", 60u));
    d.diffusivity = std::min(256u, r.value("diffusivity", 12u));
    d.floor = r.value("floor", 1u);
    d.densityPerUnit = r.value("densityPerUnit", 0);
    d.tintStrength = std::min(255u, r.value("tintStrength", 0u));
    d.glow = std::min(255u, r.value("glow", 0u));
    if (r.contains("tint")) {
      bool ok = false;
      d.tint = ParseHex(r["tint"].get<std::string>(), ok);
      if (!ok) errors += where + "tint must be #rrggbb\n";
    }
    if (r.contains("solvents") && r["solvents"].is_array()) {
      for (const json& s : r["solvents"]) {
        const std::string n = s.get<std::string>();
        const int id = FindMat(mats, n);
        if (id <= 0) {
          errors += where + "unknown solvent \"" + n + "\"\n";
          continue;
        }
        if (mats[id].gpu.klass != CLASS_LIQUID)
          errors += where + "solvent \"" + n + "\" is not a liquid\n";
        d.solvents.push_back((uint16_t)id);
      }
    }
    if (d.solvents.empty()) errors += where + "no solvents\n";
    if (r.contains("converts") && r["converts"].is_array()) {
      for (const json& c : r["converts"]) {
        SoluteConvert cv;
        const int s = FindMat(mats, c.value("solvent", std::string{}));
        const int into = FindMat(mats, c.value("into", std::string{}));
        if (s <= 0 || into <= 0) {
          errors += where + "converts row names an unknown solvent/into\n";
          continue;
        }
        cv.solvent = (uint16_t)s;
        cv.into = (uint16_t)into;
        cv.cMin = std::min(255u, c.value("cMin", 128u));
        if (!d.DissolvesIn(cv.solvent))
          errors += where + "converts a solvent it does not dissolve in\n";
        d.converts.push_back(cv);
      }
    }
    for (const SoluteDef& o : out)
      if (o.from == d.from && d.from)
        errors += where + "powder already dissolves as \"" + o.name + "\"\n";
    d.species = (uint16_t)(out.size() + 1);
    out.push_back(std::move(d));
  }
  if (out.size() > 255) errors += path + ": more than 255 species\n";
  return errors.size() == before;
}

const SoluteDef* SoluteFromPowder(const std::vector<SoluteDef>& table, uint16_t mat) {
  for (const SoluteDef& d : table)
    if (d.from == mat) return &d;
  return nullptr;
}
