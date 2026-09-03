#include "sim/materials.h"

#include <cstdio>
#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

// For the voxel-word stain limits (kStainTypeMax / kStainAmtMax): the stain a
// material applies has to fit the field the voxel word reserves for it, and
// world.h is the single source of truth for that layout.
#include "sim/tuning.h"
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
  // "washes": this liquid RINSES foreign stains rather than replacing them with
  // its own. Without it, water flowing over blood-soaked ground would relabel
  // the blood as "wet" — the stain would change colour but never actually come
  // out, which is not what washing looks like.
  if (s.value("washes", false)) d.gpu.stainPack |= kStainPackWashesBit;

  // Preserve any absorb capacity already parsed for this material: the two
  // halves of stainPack are authored in separate JSON blocks and either may be
  // read first, so neither may clobber the other's bits.
  d.gpu.stainPack = (d.gpu.stainPack & ((kStainPackAbsorbMask << kStainPackAbsorbShift) |
                                        kStainPackWashesBit)) |
                    ((uint32_t)slot << kStainPackTypeShift) |
                    ((uint32_t)amount << kStainPackAmtShift) |
                    ((uint32_t)chance << kStainPackChanceShift) |
                    ((uint32_t)consume << kStainPackConsumeShift);
}

// Parses "absorb": { capacity } into the top nibble of stainPack. Authored on
// the SUBSTRATE (grass, sand, dirt) rather than on the liquid — see the absorb
// note in materials.h for why the ceiling and the per-contact step are separate
// axes. Absent = never absorbs, which is every material that predates this.
static void ParseAbsorb(const json& m, const std::string& path, MaterialDef& d,
                        std::string& errors) {
  if (!m.contains("absorb")) return;
  const json& a = m["absorb"];
  if (!a.is_object()) {
    errors += path + ": material \"" + d.name + "\": \"absorb\" must be an object\n";
    return;
  }
  // A liquid soaking into another liquid is not absorption, it is mixing, and
  // the stain rule deliberately refuses to stain liquids and gases at all
  // (see doStaining). Authoring capacity on one would silently do nothing.
  if (d.gpu.klass != CLASS_SOLID && d.gpu.klass != CLASS_POWDER) {
    errors += path + ": material \"" + d.name +
              "\": only solids and powders can absorb (class is not solid/powder)\n";
    return;
  }
  int capacity = a.value("capacity", 0);
  if (capacity < 0 || capacity > (int)kAbsorbCapacityMax) {
    errors += path + ": material \"" + d.name + "\": absorb capacity must be 0.." +
              std::to_string(kAbsorbCapacityMax) + "\n";
    capacity = 0;
  }
  d.absorbCapacity = (uint32_t)capacity;
  d.gpu.stainPack = (d.gpu.stainPack & ~(kStainPackAbsorbMask << kStainPackAbsorbShift)) |
                    ((uint32_t)capacity << kStainPackAbsorbShift);
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
    // Soft vegetation: bodies move through it. Collision only — the cell stays
    // a solid for the CA, the brush, fire and the renderer. See kMatFlagPassable.
    if (m.value("passable", false)) d.gpu.flags |= kMatFlagPassable;
    // Render-only: the palette is the UNBURNT colour and the renderer pulses
    // it toward the flame colour. See kMatFlagBurnTint.
    if (m.value("burnTint", false)) {
      if (m.value("emission", 0) <= 0)
        errors += path + ": material \"" + d.name +
                  "\": burnTint needs emission > 0 (it is the pulse's glow)\n";
      d.gpu.flags |= kMatFlagBurnTint;
    }

    // ---- tints: GRID colour, per material (sim/materials.h kMatFlagTinted) --
    //
    //   "tints": ["#3b2f6b", "#8b1a1a", ...]      up to kMatTintsMax
    //
    // The flag and the tint-run base are set in a POST-PASS below, not here:
    // the base is an offset into one shared palette run, so it can only be
    // assigned once every material's tint count is known.
    if (m.contains("tints")) {
      if (!m["tints"].is_array()) {
        errors += path + ": material \"" + d.name + "\": \"tints\" must be an array\n";
      } else if (m["tints"].size() > kMatTintsMax) {
        // A hard error rather than a truncation. The state nibble is four bits
        // wide, so a 17th tint is not "one colour lost" — it is unreachable,
        // and silently dropping it would leave the author's 17th dye looking
        // like their 1st with nothing to say why.
        errors += path + ": material \"" + d.name + "\": " +
                  std::to_string(m["tints"].size()) + " tints, max " +
                  std::to_string(kMatTintsMax) + " (the state nibble is 4 bits)\n";
      } else if (d.gpu.klass == CLASS_LIQUID) {
        // A liquid's state nibble is its FULLNESS (LIQ_FULL_STATE, DESIGN.md
        // §4), so tinting one would not merely mis-colour it — it would make
        // the renderer read flow depth as a dye index and the CA read a dye
        // index as flow depth. Refused rather than ignored.
        errors += path + ": material \"" + d.name + "\": liquids cannot be "
                  "tinted — the state nibble is fullness\n";
      } else {
        for (const auto& t : m["tints"]) {
          uint32_t abgr = 0;
          if (!t.is_string() || !ParseColor(t.get<std::string>(), abgr)) {
            errors += path + ": material \"" + d.name +
                      "\": tints must be \"#rrggbb\" strings\n";
            d.tints.clear();
            break;
          }
          // ParseColor hands back the GPU's 0xAABBGGRR, which is right for
          // color0/1/2 and WRONG here. Tints are stored as plain 0x00RRGGBB —
          // the packing MicroBodySet::artColors already uses — for two reasons
          // that both matter:
          //   * UploadTables runs ArtRgbToGpu over this run exactly as it does
          //     over the art run, so leaving it in GPU order swapped red and
          //     blue on every authored dye (a purple robe landing in the grid
          //     as a teal one, and reading as an art mistake, not a packing
          //     one — which is the bug ArtRgbToGpu's own comment warns about).
          //   * quantizing a body's 8-bit ART colour to its material's nearest
          //     tint (DebrisSystem::RefreshTintMap) compares the two lists
          //     channel for channel, and two packings would make "nearest"
          //     mean nothing.
          d.tints.push_back(((abgr & 0xFFu) << 16) | (abgr & 0xFF00u) |
                            ((abgr >> 16) & 0xFFu));
        }
      }
    }

    // ---- wind coupling (docs/RESEARCH_wind.md §4.5, invariant 7) ----
    // Two 4-bit numbers in the high half of the same flags word — see
    // kMatWind* in materials.h for why they are packed there. Absent means
    // derived from density, so every material has a sane answer without 96
    // edits, and a new material is windy the day it is added.
    //
    //   "wind": { "response": 0..15, "friction": 0..15 }
    //
    // Out of range is an ERROR rather than a clamp: these fields are four bits
    // wide, so a 20 would wrap into its neighbour and silently make a material
    // that blows away read as one that never entrains.
    {
      const json* wj = m.contains("wind") && m["wind"].is_object() ? &m["wind"]
                                                                  : nullptr;
      int resp = wj ? wj->value("response", (int)DeriveWindResponse(d.gpu.density))
                    : (int)DeriveWindResponse(d.gpu.density);
      int fric = wj ? wj->value("friction", (int)DeriveWindFriction(d.gpu.density))
                    : (int)DeriveWindFriction(d.gpu.density);
      if (resp < 0 || resp > (int)kMatWindMax || fric < 0 ||
          fric > (int)kMatWindMax) {
        errors += path + ": material \"" + d.name +
                  "\": wind response/friction must be 0..15\n";
        resp = resp < 0 ? 0 : (resp > (int)kMatWindMax ? (int)kMatWindMax : resp);
        fric = fric < 0 ? 0 : (fric > (int)kMatWindMax ? (int)kMatWindMax : fric);
      }
      d.windResponse = (uint32_t)resp;
      d.windFriction = (uint32_t)fric;
      d.gpu.flags |= (d.windResponse & kMatWindRespMask) << kMatWindRespShift;
      d.gpu.flags |= (d.windFriction & kMatWindFricMask) << kMatWindFricShift;
    }

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
    ParseAbsorb(m, path, d, errors);
    d.tags = m.value("tags", std::vector<std::string>{});
    for (auto& t : d.tags) {
      uint32_t bit = tagReg.MaskOf(t, true);
      if (bit == 0 && tagReg.full)
        errors += path + ": material \"" + d.name + "\": more than 32 distinct tags\n";
      d.gpu.tagMask |= bit;
    }
    mats.push_back(d);
  }

  // ---- assign tint runs (sim/materials.h kMatFlagTinted) --------------------
  // A POST-PASS because a material's tint base is an offset into ONE shared
  // palette run, so it cannot be known until every material's tint count is.
  // Walked in file order, so the assignment is stable across loads and a
  // material's base only moves when a material BEFORE it changes tint count —
  // which is a reload-time fact, never a runtime one.
  //
  // Nothing here can move the world hash: tints are colours, and the state
  // nibble they reinterpret was already hashed with exactly the same bits
  // whatever they mean. A tinted material changes what a voxel LOOKS like, not
  // what it IS.
  {
    uint32_t next = 0;
    for (MaterialDef& d : mats) {
      if (d.tints.empty()) continue;
      if (next + kMatTintsMax > kTintPaletteSlotsGpu) {
        // Refuse rather than wrap: a base that overflowed its 8 bits would
        // alias onto another material's dyes, which reads as "my armour is the
        // wrong colour" with nothing pointing at the palette.
        errors += path + ": material \"" + d.name +
                  "\": tint palette full (" +
                  std::to_string(kTintPaletteSlotsGpu / kMatTintsMax) +
                  " tinted materials max)\n";
        d.tints.clear();
        continue;
      }
      // Runs are kMatTintsMax apart regardless of how many tints the material
      // actually declared, so `base + state` is in range for ANY state nibble a
      // voxel can carry — including one a CPU path wrote before this material
      // grew its tint list. Unused entries stay black but are unreachable in
      // practice because nothing generates a state above the declared count.
      d.gpu.flags |= kMatFlagTinted;
      d.gpu.flags |= (next & kMatTintBaseMask) << kMatTintBaseShift;
      next += kMatTintsMax;
    }
  }

  if (mats.size() > 4096) errors += path + ": more than 4095 materials (12-bit ID limit)\n";
  // ---- THE FAR-FIELD CASCADE OWNS BIT 7 OF ITS MATERIAL BYTE --------------
  // A far cell is ONE byte: seven bits of material id and one conservative
  // "something is here" flag (common.wgsl FAR_BLOCKER_BIT, 13.2.2). That split
  // is only legal while every id fits in seven bits, and nothing else in the
  // engine would notice if it stopped: the 118th material would simply start
  // painting the wrong colour at distance and claiming a blocker wherever bit
  // 7 landed, with no crash and no failing gate to name it. So the loader
  // refuses, here, where the table is built and the number is known.
  //
  // `mats` includes the implicit air at index 0, so 128 entries is ids 0..127
  // and exactly fills the field. scripts/check_invariants.py refuses a
  // materials.json that would cross the same line without a build.
  if (mats.size() > 128)
    errors += path + ": " + std::to_string(mats.size()) +
              " materials (including the implicit air at id 0), but the far-field"
              " cascade packs a material id into SEVEN bits of its per-cell byte"
              " and bit 7 is the conservative blocker flag -- the maximum id is"
              " 127, i.e. 128 materials. Widen the far cell (farVox is already"
              " 1 GiB) or drop a material; do NOT raise this limit alone.\n";
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

// ---- weather switches: "requires" in reactions.json ----------------------
//
// A rule may name ONE named switch. If the switch is off the rule is dropped
// here and never reaches the GPU table, so an off switch costs nothing per
// cell — the alternative (a cond bit the shader tests every tick) would spend
// work to do nothing, which rule 2 exists to prevent.
//
// The flag names live here rather than in the JSON schema so that a typo is a
// load error instead of a rule that silently never compiles. Adding a switch
// = a bool in Tuning::Weather, a row here, and a row in the tuner schema.
//
// Returns false and sets `known` = false for an unrecognised name.
static bool WeatherFlagEnabled(const std::string& name, bool& known) {
  const auto& w = CurrentTuning().weather;
  known = true;
  if (name == "waterFreezes") return w.waterFreezes;
  if (name == "iceMelts") return w.iceMelts;
  known = false;
  return false;
}

// "neighborChance": ONE tag rule, a DIFFERENT chance for a named member of
// the tag.
//
//   { "self": "wood", "neighbor": "tag:hot", "chance": 12, "selfBecomes": "ember",
//     "neighborChance": { "fire": 0.0625 } }
//
// reads: any hot neighbour ignites wood at 12 per-mille, except that FIRE --
// the free-floating flame gas, the thing that rises off every burning voxel
// and drifts through the air -- does it at a sixteenth of that (an eighth until 2026-09-03). The stationary
// heat sources (ember, lava, burning cloth and flesh, a burning leaf) keep the
// authored rate.
//
// The GPU matches a tag rule with ONE mask test (nbrMatches, sim_step.wgsl),
// and a mask cannot say "hot but not fire". So this is compiled AWAY here into
// two ordinary rules the kernels already understand:
//   * the base rule, re-pointed at a SYNTHETIC tag ("hot-fire") that the loader
//     sets on every material carrying the tag EXCEPT the named ones, and
//   * one exact-neighbour rule per named material with the scaled chance,
// appended at the END of the material's bucket. Both evaluators (sim_step.wgsl
// and sim/reactcpu.h for bodies) get the split for free, because neither
// learns a new field. The synthetic bit lives in MaterialGpu.tagMask only --
// it is never added to MaterialDef::tags, so every consumer that walks the tag
// STRINGS (mob.cpp's tagBit, cues, debris rubble) still sees exactly what the
// author wrote, and tagBit("hot") still resolves to the authored bit: fire
// lacks the synthetic one, so the AND over hot materials drops it.
//
// Why not retag fire? "hot" is what water steams against, what a mob's limb
// scan reads, what the wiki lists. Splitting the tag at the authoring surface
// would have meant every future hot material remembering two tags; splitting
// it here means the author writes the exception once, on the rule it is an
// exception to, and a new hot material is covered by construction.
//
// AT THE END, not right behind the base rule, and this is load-bearing:
// sim_step.wgsl rolls each rule as hash3(rnd, ri, slot) where ri is the rule's
// index WITHIN THE BUCKET, so a rule inserted mid-bucket re-rolls every rule
// after it. Measured: putting wood's fire rule second shifted the plant
// buckets' growth rolls and moved the fluid-react gate's ledger gap from 1.37%
// to 4.47% with no behaviour change at all. Appending leaves every authored
// rule's random stream exactly where it was; the only thing the hash then
// moves for is the behaviour the author asked for. Priority is unaffected in
// any way that matters -- the exception is the WEAK case, and losing a tie to
// an earlier rule on the same tick is what a weak case should do.
//
// Only a PAIR rule with a `tag:` neighbour can carry it: an exact neighbour
// has nothing to except, and a decay rule's neighbour fields belong to
// scaleByNeighbors (a COUNT of hot faces has no per-material weight, which is
// also why flesh's minCount-3 ignition is untouched by this).
static bool ExpandNeighborChance(const json& r, std::vector<MaterialDef>& mats,
                                 TagRegistry& tagReg, const std::string& path,
                                 const std::string& self, uint32_t kind,
                                 double chanceMille, ReactionGpu& g,
                                 std::vector<ReactionGpu>& extra,
                                 std::string& errors) {
  const json& s = r["neighborChance"];
  if (!s.is_object() || s.empty()) {
    errors += path + ": reaction self=\"" + self +
              "\": neighborChance must be an object of { material: multiplier }\n";
    return false;
  }
  if (kind != kReactPair || g.nbrTags == 0 || g.nbrMat != kNbrAny) {
    errors += path + ": reaction self=\"" + self +
              "\": neighborChance needs a pair rule with a \"tag:\" neighbor "
              "(it excepts named members of that tag)\n";
    return false;
  }
  const std::string tagName = r["neighbor"].get<std::string>().substr(4);
  // Sorted, so two rules with the same exception set share one synthetic bit
  // whatever order they were authored in (the registry is 32 bits wide).
  std::vector<std::pair<std::string, double>> ex;
  for (auto it = s.begin(); it != s.end(); ++it) {
    const int id = FindMaterial(mats, it.key());
    if (id < 0) {
      errors += path + ": reaction self=\"" + self +
                "\": neighborChance names unknown material \"" + it.key() + "\"\n";
      return false;
    }
    if ((mats[(size_t)id].gpu.tagMask & g.nbrTags) == 0) {
      errors += path + ": reaction self=\"" + self + "\": neighborChance names \"" +
                it.key() + "\", which does not carry tag \"" + tagName + "\"\n";
      return false;
    }
    if (!it.value().is_number() || it.value().get<double>() <= 0.0) {
      errors += path + ": reaction self=\"" + self + "\": neighborChance[\"" +
                it.key() + "\"] must be a multiplier > 0\n";
      return false;
    }
    ex.emplace_back(it.key(), it.value().get<double>());
  }
  std::sort(ex.begin(), ex.end());
  std::string synth = tagName;
  for (auto& e : ex) synth += "-" + e.first;
  const uint32_t bit = tagReg.MaskOf(synth, true);
  if (bit == 0) {
    errors += path + ": reaction self=\"" + self +
              "\": neighborChance needs a synthetic tag \"" + synth +
              "\" and the 32-bit tag registry is full\n";
    return false;
  }
  for (auto& m : mats) {
    if ((m.gpu.tagMask & g.nbrTags) == 0) continue;
    bool excepted = false;
    for (auto& e : ex) excepted |= (m.name == e.first);
    if (!excepted) m.gpu.tagMask |= bit;
  }
  g.nbrTags = bit;
  for (auto& e : ex) {
    ReactionGpu x = g;
    x.nbrTags = 0;
    x.nbrMat = (uint32_t)FindMaterial(mats, e.first);
    // Same rounding as the authored chance: once, in double, on the CPU.
    double c = chanceMille * e.second;
    if (c > 1000.0) c = 1000.0;
    x.chance = (uint32_t)(c * (double)kReactChanceScale + 0.5);
    if (x.chance == 0) x.chance = 1;
    extra.push_back(x);
  }
  return true;
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
  // Rules the loader SYNTHESISED (neighborChance exceptions), appended after
  // every authored rule of their material so no authored rule's bucket index
  // -- and therefore its RNG stream -- moves. See ExpandNeighborChance.
  std::vector<std::vector<ReactionGpu>> tails(mats.size());

  for (auto& r : j["reactions"]) {
    if (r.contains("note") && !r.contains("self")) continue;  // section comment
    std::string self = r.value("self", "");
    int selfId = FindMaterial(mats, self);
    if (selfId <= 0) {
      errors += path + ": reaction with unknown or missing self \"" + self + "\"\n";
      continue;
    }
    // A rule may be gated on a weather switch. Dropped BEFORE any other
    // parsing so an off rule costs nothing and cannot report errors about
    // fields nobody will read; the bucket flattening below recomputes
    // reactOffset/reactCount from the surviving rules, so nothing else needs
    // to know a rule went missing.
    if (r.contains("requires")) {
      if (!r["requires"].is_string()) {
        errors += path + ": reaction self=\"" + self +
                  "\": \"requires\" must be a string\n";
        continue;
      }
      const std::string need = r["requires"].get<std::string>();
      bool known = false;
      const bool on = WeatherFlagEnabled(need, known);
      if (!known) {
        // A typo must be loud. Silently compiling the rule would make the
        // switch appear to do nothing; silently dropping it would delete
        // content — an error is the only honest option.
        errors += path + ": reaction self=\"" + self +
                  "\": unknown \"requires\" switch \"" + need + "\"\n";
        continue;
      }
      if (!on) continue;
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
    // ---- "burnDuration": the one authored knob that scales a chance --------
    //
    // A rule marked this way is one that RETIRES A BURNING VOXEL, and its
    // chance is divided by combustion.burnDurationPct — so 200 halves the
    // per-tick death rate and doubles how long anything in the world stays
    // alight. The whole point of doing it here rather than per-cell is that
    // the GPU never learns the knob exists: the scaled value is rounded into
    // the same integer an authored chance compiles to, once, on the CPU
    // (rule 1). See Tuning::Combustion for what is deliberately NOT scaled.
    //
    // Applied BEFORE the range check so the check is the one that catches an
    // out-of-range result, and so a scaled rule cannot slip past 1000
    // per-mille at a low setting. The floor below is the other half: a rule
    // scaled toward zero still fires eventually rather than becoming an
    // eternal flame.
    // Gated on the AUTHORED value already being in range, so a rule that
    // forgot its "chance" still reports "chance must be ..." rather than being
    // quietly floored into a legal one by the scaling.
    const bool burnDur = r.value("burnDuration", false) &&
                         chanceMille >= kReactChanceMinMille &&
                         chanceMille <= 1000.0;
    if (burnDur) {
      const int pct = CurrentTuning().combustion.burnDurationPct;
      chanceMille = chanceMille * 100.0 / (double)(pct > 0 ? pct : 100);
      if (chanceMille > 1000.0) chanceMille = 1000.0;
      if (chanceMille < kReactChanceMinMille) chanceMille = kReactChanceMinMille;
    }
    // ---- combustion.spreadPct: HOW FAST FIRE SPREADS THROUGH FUEL ----------
    //
    // burnDurationPct's twin, scaling the other of the two levers this file's
    // combustion note names: that one is how long a lit voxel stays lit, this
    // one is how readily the voxel beside it catches. Owner request
    // 2026-09-03, "slow down the burning voxel spread by like 8x", and it is a
    // knob rather than an edit to the authored numbers because those numbers
    // encode RATIOS the owner has tuned three separate times -- dry needles
    // against green leaves, cloth against flesh, leather against cloth. A
    // single multiplier moves the clock and leaves every one of those intact;
    // dividing thirty authored numbers by eight would not, and could not be
    // undone from the tuner.
    //
    // A RULE IS AN IGNITION IF ITS PRODUCT IS HOT. That is the same test
    // ExpandNeighborChance uses just below to decide what the floating flame's
    // weakness applies to, and it is the honest definition: the thing that
    // makes a voxel a heat source is tag:hot, so a rule that produces one is
    // the fuel catching, whatever the material happens to be called. It falls
    // out correctly for every shape in the file -- a pair rule's selfBecomes
    // (wood -> ember) and a scaled decay's becomes (flesh_cooked ->
    // flesh_burning, which is authored as a decay only because a pair rule's
    // neighbour fields are spoken for by scaleByNeighbors) both scale, while
    // an EMIT rule is excluded automatically because its product is named by
    // "emit" rather than by either of these keys.
    //
    // A rule already marked "burnDuration" is excluded outright, so every rule
    // has exactly one owner among the two knobs and the retire/relight loop
    // cannot be scaled twice by two sliders that look independent in the UI.
    //
    // ...AND SO IS THE SEAR, which "product is hot" alone does NOT catch and
    // which has to be here. `skin + tag:hot -> flesh_cooked` produces a
    // material that is not itself a heat source, so by the test above it is
    // not an ignition -- and leaving it at the authored rate while everything
    // around it slowed by eight broke the one comparison the whole body-burn
    // section of reactions.json is built on. Measured, first run at 12%: skin
    // halved at t+31 and cloth at t+124, i.e. the flesh now reacted to a
    // single hot face FOUR TIMES more eagerly than the cloth over it, when the
    // file authors cloth at about eight times flesh's rate. That is the exact
    // inversion the note above `skin` in reactions.json records catching once
    // already, from the other direction, and the mob-burn gate caught it again
    // here.
    //
    // So the second clause: a PAIR rule on FLAMMABLE matter driven by a hot
    // neighbour is heat converting fuel, whatever its product is called, and
    // scales with the rest. The flammable test is what keeps heat's other jobs
    // out of it -- `water + tag:hot -> steam` and ice melting are neither fuel
    // nor spread, and a flame over a pond still steams it at the authored rate
    // at any setting of this knob.
    //
    // Same placement as burnDur above, and for the same three reasons: before
    // the range check so the check catches an out-of-range result, before
    // ExpandNeighborChance so the floating-flame exception is scaled with its
    // parent rather than drifting away from it, and floored so a rule scaled
    // toward zero stays rare rather than becoming impossible.
    const uint32_t hotBit = tagReg.MaskOf("hot", false);
    auto hasTag = [&](int id, uint32_t bit) {
      return id >= 0 && bit != 0 && (mats[(size_t)id].gpu.tagMask & bit) != 0;
    };
    // Clause one: the product is a heat source, so this rule is fuel catching.
    auto productHot = [&] {
      const char* key = r.value("decay", false) ? "becomes" : "selfBecomes";
      if (!r.contains(key) || !r[key].is_string()) return false;
      // FindMaterial returns -1 for "air" and for a name the product parse
      // below will reject; neither is a heat source, so both answer false.
      return hasTag(FindMaterial(mats, r[key].get<std::string>()), hotBit);
    };
    // Clause two: a hot neighbour acting directly on FUEL -- the sear.
    auto heatOnFuel = [&] {
      if (r.value("decay", false) || r.contains("emit")) return false;
      if (!hasTag(FindMaterial(mats, self), tagReg.MaskOf("flammable", false)))
        return false;
      if (!r.contains("neighbor") || !r["neighbor"].is_string()) return false;
      const std::string n = r["neighbor"].get<std::string>();
      if (n.rfind("tag:", 0) == 0) return n.substr(4) == "hot";
      return hasTag(FindMaterial(mats, n), hotBit);
    };
    const bool ignites = !r.value("burnDuration", false) &&
                         chanceMille >= kReactChanceMinMille &&
                         chanceMille <= 1000.0 && (productHot() || heatOnFuel());
    if (ignites) {
      const int pct = CurrentTuning().combustion.spreadPct;
      chanceMille = chanceMille * (double)(pct > 0 ? pct : 100) / 100.0;
      if (chanceMille > 1000.0) chanceMille = 1000.0;
      if (chanceMille < kReactChanceMinMille) chanceMille = kReactChanceMinMille;
    }
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
    // A per-member exception splits this rule in two (see ExpandNeighborChance).
    // The base rule keeps its place; the exact-neighbour rules go to the tail
    // of the bucket, so a voxel touching both an ember and a flame rolls the
    // full rate first and nothing authored changes its index.
    if (r.contains("neighborChance"))
      ExpandNeighborChance(r, mats, tagReg, path, self, kind, chanceMille, g,
                           tails[selfId], errors);
    buckets[selfId].push_back(g);
  }
  for (size_t i = 0; i < mats.size(); i++)
    buckets[i].insert(buckets[i].end(), tails[i].begin(), tails[i].end());

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

std::vector<uint32_t> BuildCollisionClasses(const std::vector<MaterialDef>& mats) {
  std::vector<uint32_t> classOf;
  classOf.reserve(mats.size());
  for (const MaterialDef& m : mats) {
    // Passable vegetation reports as GAS so every CPU collision path — all of
    // which already test for Solid — reads it as empty space. See the header
    // for why this is a remap rather than a new CellKind.
    classOf.push_back((m.gpu.flags & kMatFlagPassable) ? (uint32_t)CLASS_GAS
                                                       : m.gpu.klass);
  }
  return classOf;
}
