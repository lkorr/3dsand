#include "sim/solutes.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

#include "sim/materials.h"
#include "sim/world.h"  // kSolScoopSpecies, kCellOpSoluteSpeciesMax

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
    // Only a powder dissolves: sim_step gates the dissolve read on the class.
    if (d.from && mats[d.from].gpu.klass != CLASS_POWDER)
      errors += where + "\"from\" must be a powder\n";
    d.precipitatesTo = mat("precipitatesTo", false);
    d.yieldPerVoxel = r.value("yieldPerVoxel", 256u);
    // THE UNIT BRIDGE (contract 2.4): one dissolved eighth in a vessel is
    // yieldPerVoxel / 8 world units, and the world's per-cell mass is 0..255.
    // A yield that is not a multiple of 8 loses yield % 8 units per voxel on
    // every vessel <-> world round trip (the bench keeps the full yield, the
    // world y8 * 8); past 2040 one eighth does not fit a cell.
    if (d.yieldPerVoxel < 8 || d.yieldPerVoxel % 8 != 0 || d.yieldPerVoxel / 8 > 255)
      errors += where + "yieldPerVoxel must be a multiple of 8 in 8..2040\n";
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
  // The scoop ledger credits a vessel per species in kSolScoopSpecies
  // counters (world.h), and a pour op carries a 7-bit species: a species past
  // either would be scooped into nothing / poured as nothing.
  if (out.size() > std::min<size_t>(kSolScoopSpecies, kCellOpSoluteSpeciesMax))
    errors += path + ": more species than the scoop ledger has counters (world.h "
              "kSolScoopSpecies = " + std::to_string(kSolScoopSpecies) + ")\n";
  return errors.size() == before;
}

const SoluteDef* SoluteFromPowder(const std::vector<SoluteDef>& table, uint16_t mat) {
  for (const SoluteDef& d : table)
    if (d.from == mat) return &d;
  return nullptr;
}

namespace {
std::vector<SoluteDef>& CurrentSolutesStore() {
  static std::vector<SoluteDef> s;
  return s;
}
}  // namespace

const std::vector<SoluteDef>& CurrentSolutes() { return CurrentSolutesStore(); }
void SetCurrentSolutes(const std::vector<SoluteDef>& table) { CurrentSolutesStore() = table; }
const SoluteDef* CurrentSoluteNamed(const char* name) {
  for (const SoluteDef& d : CurrentSolutesStore())
    if (d.name == name) return &d;
  return nullptr;
}
const SoluteDef* CurrentSoluteById(uint32_t species) {
  for (const SoluteDef& d : CurrentSolutesStore())
    if (d.species == species) return &d;
  return nullptr;
}
