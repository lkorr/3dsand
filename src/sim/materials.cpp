#include "sim/materials.h"

#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;

static bool ParseColor(const std::string& hex, uint32_t& out) {
  if (hex.size() != 7 || hex[0] != '#') return false;
  uint32_t v = 0;
  for (int i = 1; i < 7; i++) {
    char c = hex[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
    else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
    else return false;
    v = (v << 4) | d;
  }
  // stored as 0xAABBGGRR so the shader unpacks R from the low byte
  uint32_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  out = 0xFF000000u | (b << 16) | (g << 8) | r;
  return true;
}

bool LoadMaterials(const std::string& path, std::vector<MaterialDef>& out,
                   std::string& errors) {
  errors.clear();
  std::ifstream f(path);
  if (!f) {
    errors = "cannot open " + path;
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    errors = std::string("JSON parse error: ") + e.what();
    return false;
  }

  std::vector<MaterialDef> mats;
  MaterialDef air;
  air.name = "air";
  air.gpu.klass = CLASS_GAS;
  air.gpu.density = 10;
  mats.push_back(air);

  if (!j.contains("materials") || !j["materials"].is_array()) {
    errors = path + ": missing top-level \"materials\" array";
    return false;
  }

  for (auto& m : j["materials"]) {
    MaterialDef d;
    d.name = m.value("id", "");
    if (d.name.empty()) {
      errors += path + ": material missing \"id\"\n";
      continue;
    }
    for (auto& prev : mats) {
      if (prev.name == d.name) errors += path + ": duplicate material id \"" + d.name + "\"\n";
    }
    std::string cls = m.value("class", "");
    if (cls == "solid") d.gpu.klass = CLASS_SOLID;
    else if (cls == "powder") d.gpu.klass = CLASS_POWDER;
    else if (cls == "liquid") d.gpu.klass = CLASS_LIQUID;
    else if (cls == "gas") d.gpu.klass = CLASS_GAS;
    else errors += path + ": material \"" + d.name + "\": unknown class \"" + cls + "\"\n";

    d.gpu.density = m.value("density", 0);
    if (d.gpu.density <= 0)
      errors += path + ": material \"" + d.name + "\": density must be > 0\n";
    d.gpu.decayPerMille = m.value("decayPerMille", 0);
    if (d.gpu.decayPerMille > 1000)
      errors += path + ": material \"" + d.name + "\": decayPerMille > 1000\n";

    auto colors = m.value("colors", std::vector<std::string>{});
    if (colors.size() != 3) {
      errors += path + ": material \"" + d.name + "\": need exactly 3 colors\n";
    } else {
      uint32_t c[3];
      bool ok = true;
      for (int i = 0; i < 3; i++) ok &= ParseColor(colors[i], c[i]);
      if (!ok) {
        errors += path + ": material \"" + d.name + "\": bad color (want #rrggbb)\n";
      } else {
        d.gpu.color0 = c[0];
        d.gpu.color1 = c[1];
        d.gpu.color2 = c[2];
      }
    }
    d.tags = m.value("tags", std::vector<std::string>{});
    mats.push_back(d);
  }

  if (mats.size() > 4096) errors += path + ": more than 4095 materials (12-bit ID limit)\n";
  if (!errors.empty()) return false;
  out = std::move(mats);
  return true;
}
