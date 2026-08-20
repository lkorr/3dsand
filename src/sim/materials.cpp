#include "sim/materials.h"

#include <fstream>
#include <map>
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

// Tag registry: tag string -> bit index, assigned first-seen while parsing
// materials. Reactions may only reference tags some material declares.
struct TagRegistry {
  std::map<std::string, int> bits;
  bool full = false;
  uint32_t MaskOf(const std::string& tag, bool allowNew) {
    auto it = bits.find(tag);
    if (it != bits.end()) return 1u << it->second;
    if (!allowNew || bits.size() >= 32) {
      if (allowNew) full = true;
      return 0;
    }
    int bit = (int)bits.size();
    bits[tag] = bit;
    return 1u << bit;
  }
};

static int FindMaterial(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

static bool LoadMaterialsJson(const std::string& path, std::vector<MaterialDef>& mats,
                              TagRegistry& tagReg, std::string& errors) {
  std::ifstream f(path);
  if (!f) {
    errors += "cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    errors += path + ": JSON parse error: " + e.what() + "\n";
    return false;
  }

  MaterialDef air;
  air.name = "air";
  air.gpu.klass = CLASS_GAS;
  air.gpu.density = 10;
  air.gpu.moveEvery = 1;
  mats.push_back(air);

  if (!j.contains("materials") || !j["materials"].is_array()) {
    errors += path + ": missing top-level \"materials\" array\n";
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

    d.gpu.emission = m.value("emission", 0);
    if (d.gpu.emission > 255)
      errors += path + ": material \"" + d.name + "\": emission > 255\n";

    d.gpu.moveEvery = m.value("moveEvery", 1);
    if (d.gpu.moveEvery < 1 || d.gpu.moveEvery > 16)
      errors += path + ": material \"" + d.name + "\": moveEvery must be 1..16\n";

    // media absorbance; defaults give thin gases and moderately deep liquids
    d.gpu.opacity = m.value("opacity", d.gpu.klass == CLASS_GAS ? 50
                                       : d.gpu.klass == CLASS_LIQUID ? 110 : 255);
    if (d.gpu.opacity > 255)
      errors += path + ": material \"" + d.name + "\": opacity > 255\n";

    // blast/dig resistance; class defaults so unlisted materials behave sanely
    d.gpu.hardness = m.value("hardness", d.gpu.klass == CLASS_SOLID ? 60
                                         : d.gpu.klass == CLASS_POWDER ? 12
                                         : d.gpu.klass == CLASS_LIQUID ? 4 : 0);
    if (d.gpu.hardness > 255)
      errors += path + ": material \"" + d.name + "\": hardness > 255\n";

    if (m.value("wanders", false)) d.gpu.flags |= kMatFlagWander;
    if (m.value("opaque", false)) d.gpu.flags |= kMatFlagOpaque;

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

    d.rubble = m.value("rubble", "");
    d.molten = m.value("molten", "");
    d.tags = m.value("tags", std::vector<std::string>{});
    for (auto& t : d.tags) {
      uint32_t bit = tagReg.MaskOf(t, true);
      if (bit == 0 && tagReg.full)
        errors += path + ": material \"" + d.name + "\": more than 32 distinct tags\n";
      d.gpu.tagMask |= bit;
    }
    mats.push_back(d);
  }

  if (mats.size() > 4096) errors += path + ": more than 4095 materials (12-bit ID limit)\n";
  return true;
}

// Resolves a product name: "air"/"nothing" -> 0, absent -> kProdKeep.
static uint32_t ParseProduct(const json& r, const char* key,
                             const std::vector<MaterialDef>& mats,
                             const std::string& path, const std::string& self,
                             std::string& errors) {
  if (!r.contains(key)) return kProdKeep;
  std::string name = r[key].get<std::string>();
  if (name == "air" || name == "nothing") return 0;
  int id = FindMaterial(mats, name);
  if (id < 0) {
    errors += path + ": reaction self=\"" + self + "\": unknown material \"" + name +
              "\" in " + key + "\n";
    return kProdKeep;
  }
  return (uint32_t)id;
}

static uint32_t ParseDir(const json& r, const std::string& path,
                         const std::string& self, std::string& errors) {
  std::string dir = r.value("dir", "any");
  if (dir == "any") return kDirAny;
  if (dir == "up") return kDirUp;
  if (dir == "down") return kDirDown;
  if (dir == "side") return kDirSide;
  errors += path + ": reaction self=\"" + self + "\": unknown dir \"" + dir + "\"\n";
  return kDirAny;
}

static bool LoadReactionsJson(const std::string& path, std::vector<MaterialDef>& mats,
                              TagRegistry& tagReg, std::vector<ReactionGpu>& out,
                              std::string& errors) {
  std::ifstream f(path);
  if (!f) {
    errors += "cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    errors += path + ": JSON parse error: " + e.what() + "\n";
    return false;
  }
  if (!j.contains("reactions") || !j["reactions"].is_array()) {
    errors += path + ": missing top-level \"reactions\" array\n";
    return false;
  }

  // Parse in file order, keyed by self id; bucketed after the loop so each
  // material's rules stay in authoring order (first match wins on the GPU).
  std::vector<std::vector<ReactionGpu>> buckets(mats.size());

  for (auto& r : j["reactions"]) {
    if (r.contains("note") && !r.contains("self")) continue;  // section comment
    std::string self = r.value("self", "");
    int selfId = FindMaterial(mats, self);
    if (selfId <= 0) {
      errors += path + ": reaction with unknown or missing self \"" + self + "\"\n";
      continue;
    }
    ReactionGpu g{};
    g.nbrMat = kNbrAny;
    g.prodSelf = kProdKeep;
    g.prodNbr = kProdKeep;

    g.chance = r.value("chance", 0);
    if (g.chance < 1 || g.chance > 1000)
      errors += path + ": reaction self=\"" + self + "\": chance must be 1..1000\n";

    // ---- light / day-phase condition (day-night cycle) ----
    // These gate the rule on the cell's light environment. They feed voxel
    // state, so everything here is integer and derived from the tick-based
    // day phase — see DayPhaseForTick in world.h.
    if (r.value("needsSky", false)) g.cond |= kCondSky;
    if (r.contains("when")) {
      std::string when = r["when"].get<std::string>();
      if (when == "day") g.cond |= kCondDay;
      else if (when == "night") g.cond |= kCondNight;
      else if (when != "any")
        errors += path + ": reaction self=\"" + self + "\": unknown when \"" +
                  when + "\" (expected day|night|any)\n";
    }
    if (r.contains("minLight")) {
      int ml = r.value("minLight", 0);
      if (ml < 0 || ml > 255)
        errors += path + ": reaction self=\"" + self +
                  "\": minLight must be 0..255\n";
      else
        g.cond |= ((uint32_t)ml & 0xFFu) << 8u;
    }
    if ((g.cond & kCondDay) && (g.cond & kCondNight))
      errors += path + ": reaction self=\"" + self +
                "\": when cannot be both day and night\n";

    uint32_t kind, dirMask = ParseDir(r, path, self, errors);
    if (r.value("decay", false)) {
      kind = kReactDecay;
      g.prodSelf = ParseProduct(r, "becomes", mats, path, self, errors);
      if (g.prodSelf == kProdKeep)
        errors += path + ": decay self=\"" + self + "\": missing \"becomes\"\n";
    } else if (r.contains("emit")) {
      kind = kReactEmit;
      g.prodNbr = ParseProduct(r, "emit", mats, path, self, errors);
      if (g.prodNbr == kProdKeep || g.prodNbr == 0)
        errors += path + ": emit self=\"" + self + "\": bad \"emit\" material\n";
      g.prodSelf = ParseProduct(r, "selfBecomes", mats, path, self, errors);
    } else if (r.contains("neighbor")) {
      kind = kReactPair;
      std::string nbr = r["neighbor"].get<std::string>();
      if (nbr.rfind("tag:", 0) == 0) {
        std::string tag = nbr.substr(4);
        uint32_t bit = tagReg.MaskOf(tag, false);
        if (bit == 0)
          errors += path + ": reaction self=\"" + self + "\": tag \"" + tag +
                    "\" not declared by any material\n";
        g.nbrTags = bit;
      } else if (nbr != "any") {
        int id = FindMaterial(mats, nbr);
        if (id < 0)
          errors += path + ": reaction self=\"" + self + "\": unknown neighbor \"" + nbr + "\"\n";
        else
          g.nbrMat = (uint32_t)id;
      }
      if (r.contains("neighborClass")) {
        for (auto& c : r["neighborClass"]) {
          std::string cls = c.get<std::string>();
          if (cls == "solid") g.nbrClass |= 1u << CLASS_SOLID;
          else if (cls == "powder") g.nbrClass |= 1u << CLASS_POWDER;
          else if (cls == "liquid") g.nbrClass |= 1u << CLASS_LIQUID;
          else if (cls == "gas") g.nbrClass |= 1u << CLASS_GAS;
          else errors += path + ": reaction self=\"" + self + "\": unknown class \"" + cls + "\"\n";
        }
      }
      g.prodSelf = ParseProduct(r, "selfBecomes", mats, path, self, errors);
      g.prodNbr = ParseProduct(r, "neighborBecomes", mats, path, self, errors);
      if (g.prodSelf == kProdKeep && g.prodNbr == kProdKeep)
        errors += path + ": reaction self=\"" + self +
                  "\": pair rule with no selfBecomes/neighborBecomes is unreachable\n";
    } else {
      errors += path + ": reaction self=\"" + self +
                "\": need one of \"neighbor\", \"decay\", \"emit\"\n";
      continue;
    }
    g.packed = (kind & 3u) | ((dirMask & 7u) << 2u);
    buckets[selfId].push_back(g);
  }

  out.clear();
  for (size_t i = 0; i < mats.size(); i++) {
    mats[i].gpu.reactOffset = (uint32_t)out.size();
    mats[i].gpu.reactCount = (uint32_t)buckets[i].size();
    out.insert(out.end(), buckets[i].begin(), buckets[i].end());
  }
  if (out.size() > kMaxReactions)
    errors += path + ": more than " + std::to_string(kMaxReactions) + " reactions\n";
  return true;
}

bool LoadAssets(const std::string& materialsPath, const std::string& reactionsPath,
                std::vector<MaterialDef>& mats, std::vector<ReactionGpu>& reactions,
                std::string& errors) {
  errors.clear();
  std::vector<MaterialDef> m;
  std::vector<ReactionGpu> r;
  TagRegistry tags;
  bool ok = LoadMaterialsJson(materialsPath, m, tags, errors);
  if (ok) LoadReactionsJson(reactionsPath, m, tags, r, errors);
  for (auto& d : m) {
    if (!d.rubble.empty() && FindMaterial(m, d.rubble) < 0)
      errors += materialsPath + ": material \"" + d.name + "\": unknown rubble \"" +
                d.rubble + "\"\n";
    // molten resolves after the whole table exists (forward references:
    // stone -> lava). The resolved ID rides the GPU material record.
    if (!d.molten.empty()) {
      int id = FindMaterial(m, d.molten);
      if (id < 0)
        errors += materialsPath + ": material \"" + d.name + "\": unknown molten \"" +
                  d.molten + "\"\n";
      else
        d.gpu.molten = (uint32_t)id;
    }
  }
  if (!errors.empty()) return false;
  mats = std::move(m);
  reactions = std::move(r);
  return true;
}
