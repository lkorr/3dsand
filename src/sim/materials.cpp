#include "sim/materials.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

// For the voxel-word stain limits (kStainTypeMax / kStainAmtMax): the stain a
// material applies has to fit the field the voxel word reserves for it, and
// world.h is the single source of truth for that layout.
#include "sim/world.h"

using nlohmann::json;

std::string FormatMille(double mille) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4f", mille);
  std::string s(buf);
  // trim trailing zeros, then a bare trailing '.'
  size_t last = s.find_last_not_of('0');
  if (s.find('.') != std::string::npos && last != std::string::npos) {
    s.erase(s[last] == '.' ? last : last + 1);
  }
  return s;
}

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

// Stain-type registry: stain name -> slot 1..7, assigned first-seen. Keyed by
// NAME rather than by material so several materials can share one stain look
// (any of the future gore materials can all stain "blood"), and so the slot
// numbers stay stable as long as the file order of first use does.
//
// Only 7 slots exist — the voxel word can spare 3 bits and no more (world.h).
// That is a deliberate ceiling: stains are a visual vocabulary of a handful of
// kinds, not a per-material property. Running out is a loud load error.
struct StainRegistry {
  std::map<std::string, uint32_t> slots;
  bool full = false;
  uint32_t SlotOf(const std::string& name) {
    auto it = slots.find(name);
    if (it != slots.end()) return it->second;
    if (slots.size() >= kStainTypeMax) {
      full = true;
      return 0;
    }
    uint32_t slot = (uint32_t)slots.size() + 1;  // 0 means "unstained"
    slots[name] = slot;
    return slot;
  }
};

// Parses "stain": { type, color, amount, chance, consume } into stainPack +
// stainColor. Absent = this material does not stain (stainPack 0), which is
// every material that predates the feature.
static void ParseStain(const json& m, const std::string& path,
                       StainRegistry& stainReg, MaterialDef& d,
                       std::string& errors) {
  if (!m.contains("stain")) return;
  const json& s = m["stain"];
  if (!s.is_object()) {
    errors += path + ": material \"" + d.name + "\": \"stain\" must be an object\n";
    return;
  }
  // Only a liquid can stain what it touches. The sim rule runs off the liquid
  // movement path, and a staining SOLID would need a different mechanism
  // entirely — rejecting it here beats silently doing nothing at runtime.
  if (d.gpu.klass != CLASS_LIQUID) {
    errors += path + ": material \"" + d.name +
              "\": only liquids can stain (class is not liquid)\n";
    return;
  }
  d.stain = s.value("type", d.name);
  uint32_t slot = stainReg.SlotOf(d.stain);
  if (slot == 0) {
    errors += path + ": material \"" + d.name + "\": more than " +
              std::to_string(kStainTypeMax) + " distinct stain types\n";
    return;
  }

  uint32_t color = 0;
  if (s.contains("color")) {
    if (!ParseColor(s["color"].get<std::string>(), color))
      errors += path + ": material \"" + d.name +
                "\": bad stain color (want #rrggbb)\n";
  } else {
    // Default to the material's own darkest palette entry: a stain is what the
    // liquid leaves once it has soaked in and dried down, so it is a darker
    // relative of the liquid, never a brand-new hue.
    color = d.gpu.color1;
  }
  d.gpu.stainColor = color;

  int amount = s.value("amount", 5);
  int chance = s.value("chance", 60);
  int consume = s.value("consume", 0);
  if (amount < 1 || amount > (int)kStainAmtMax) {
    errors += path + ": material \"" + d.name + "\": stain amount must be 1.." +
              std::to_string(kStainAmtMax) + "\n";
    amount = 5;
  }
  if (chance < 0 || chance > (int)kStainChanceMax) {
    errors += path + ": material \"" + d.name +
              "\": stain chance must be 0..1000 per-mille\n";
    chance = 0;
  }
  if (consume < 0 || consume > (int)kStainChanceMax) {
    errors += path + ": material \"" + d.name +
              "\": stain consume must be 0..1000 per-mille\n";
    consume = 0;
  }
  d.gpu.stainPack = ((uint32_t)slot << kStainPackTypeShift) |
                    ((uint32_t)amount << kStainPackAmtShift) |
                    ((uint32_t)chance << kStainPackChanceShift) |
                    ((uint32_t)consume << kStainPackConsumeShift);
}

static bool LoadMaterialsJson(const std::string& path, std::vector<MaterialDef>& mats,
                              TagRegistry& tagReg, StainRegistry& stainReg,
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
    // Sound slots. The "sounds" object is the general form; the flat
    // "footstep" key predates it and is still honoured so no existing
    // materials.json has to be rewritten. The object wins when both appear —
    // a file that says two things should not have the loader pick the older
    // one.
    d.sounds.clear();
    if (m.contains("footstep") && m["footstep"].is_string())
      d.sounds["footstep"] = m["footstep"].get<std::string>();
    if (m.contains("sounds") && m["sounds"].is_object())
      for (auto& [slot, name] : m["sounds"].items())
        if (name.is_string() && !name.get<std::string>().empty())
          d.sounds[slot] = name.get<std::string>();
    ParseStain(m, path, stainReg, d, errors);
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

// Parses the neighbour-matching vocabulary — "neighbor" (exact id / "tag:x" /
// "any") plus an optional "neighborClass" list — into nbrMat/nbrTags/nbrClass.
// Shared by pair rules (where it selects the reacting partner) and by
// scaleByNeighbors (where it selects what gets counted), so both speak exactly
// the same language.
static void ParseNbrPredicate(const json& r, const std::vector<MaterialDef>& mats,
                              TagRegistry& tagReg, const std::string& path,
                              const std::string& self, ReactionGpu& g,
                              std::string& errors) {
  if (r.contains("neighbor")) {
    std::string nbr = r["neighbor"].get<std::string>();
    if (nbr.rfind("tag:", 0) == 0) {
      std::string tag = nbr.substr(4);
      uint32_t bit = tagReg.MaskOf(tag, false);
      if (bit == 0)
        errors += path + ": reaction self=\"" + self + "\": tag \"" + tag +
                  "\" not declared by any material\n";
      g.nbrTags = bit;
    } else if (nbr == "air") {
      g.nbrMat = 0;
    } else if (nbr != "any") {
      int id = FindMaterial(mats, nbr);
      if (id < 0)
        errors += path + ": reaction self=\"" + self + "\": unknown neighbor \"" + nbr + "\"\n";
      else
        g.nbrMat = (uint32_t)id;
    }
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
}

// "scaleByNeighbors": scales the rule's chance by how many of the 6 face
// neighbours match a predicate, and forbids it entirely at a count of zero.
// See the ReactionGpu.cond comment in materials.h for the bit layout and the
// reactions.json note for why a frontier rule needs this.
static void ParseNeighborScale(const json& r, const std::vector<MaterialDef>& mats,
                               TagRegistry& tagReg, const std::string& path,
                               const std::string& self, uint32_t kind,
                               ReactionGpu& g, std::string& errors) {
  if (!r.contains("scaleByNeighbors")) return;
  const json& s = r["scaleByNeighbors"];
  if (!s.is_object()) {
    errors += path + ": reaction self=\"" + self +
              "\": scaleByNeighbors must be an object\n";
    return;
  }
  // A pair rule already consumes nbrMat/nbrTags/nbrClass to pick its partner,
  // so it has nowhere to put a second, different predicate. Rejecting this is
  // better than silently counting the partner set.
  if (kind != kReactDecay) {
    errors += path + ": reaction self=\"" + self +
              "\": scaleByNeighbors is only supported on decay rules "
              "(a pair rule's neighbor fields already select its partner)\n";
    return;
  }
  g.cond |= kScaleEnable;
  if (s.value("invert", false)) g.cond |= kScaleInvert;
  ParseNbrPredicate(s, mats, tagReg, path, self, g, errors);
  if (g.nbrMat == kNbrAny && g.nbrTags == 0 && g.nbrClass == 0 &&
      (g.cond & kScaleInvert) == 0) {
    // "count anything" matches all 6 neighbours in solid matter and 0 in a
    // vacuum, which is not a frontier — almost certainly an authoring slip.
    errors += path + ": reaction self=\"" + self +
              "\": scaleByNeighbors needs a neighbor/neighborClass to count\n";
  }
  // "minCount": a floor on the matching count. 1 (the default) is the plain
  // "any matching neighbour lets it fire" behaviour; higher values demand a
  // wider frontier, which is how evaporation says a lone droplet boils off
  // but a flat pond surface does not.
  if (s.contains("minCount")) {
    int mc = s.value("minCount", 1);
    if (mc < (int)kScaleMinCountMin || mc > (int)kScaleMinCountMax) {
      errors += path + ": reaction self=\"" + self + "\": minCount must be " +
                std::to_string(kScaleMinCountMin) + ".." +
                std::to_string(kScaleMinCountMax) + "\n";
      return;
    }
    g.cond |= ((uint32_t)(mc - 1) & kScaleMinMask) << kScaleMinShift;
  }
  float mul = s.value("scaleMax", 1.0f);
  if (mul < kScaleMulMin || mul > kScaleMulMax) {
    errors += path + ": reaction self=\"" + self + "\": scaleMax must be " +
              std::to_string(kScaleMulMin) + ".." + std::to_string(kScaleMulMax) + "\n";
    return;
  }
  // Quantized to quarters and biased by 1.0x — see materials.h.
  uint32_t q = (uint32_t)(mul * (float)kScaleMulUnit + 0.5f) - kScaleMulUnit;
  g.cond |= (q & kScaleMulMask) << kScaleMulShift;
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

    // Chance is authored in per-mille but MAY be fractional, because per-mille
    // bottoms out at a mean wait of ~33 s and rare ambient events want to be
    // far rarer than that. It is compiled into units of 1/kReactChanceDen so
    // the shader can roll against it directly with no scaling.
    //
    // The conversion is done in double and rounded ONCE here on the CPU, so
    // the GPU never sees a float (rule 1). Every machine loading the same JSON
    // gets the same integer.
    double chanceMille = r.value("chance", 0.0);
    if (!(chanceMille >= kReactChanceMinMille) || chanceMille > 1000.0) {
      errors += path + ": reaction self=\"" + self + "\": chance must be " +
                FormatMille(kReactChanceMinMille) + "..1000 per-mille\n";
    } else {
      // round-half-up on a non-negative value; the floor of 1 unit means a
      // rule that passed validation can never quantize to "never fires".
      g.chance = (uint32_t)(chanceMille * (double)kReactChanceScale + 0.5);
      if (g.chance == 0) g.chance = 1;
    }

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
      ParseNbrPredicate(r, mats, tagReg, path, self, g, errors);
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
    // After `kind` is known: scaling rejects non-decay rules, whose neighbour
    // fields are already spoken for.
    ParseNeighborScale(r, mats, tagReg, path, self, kind, g, errors);
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
  StainRegistry stains;
  bool ok = LoadMaterialsJson(materialsPath, m, tags, stains, errors);
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
