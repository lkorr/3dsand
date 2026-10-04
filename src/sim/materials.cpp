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

static_assert(kMatFarPalSlots == kFarPalBlocker && kFarPalBlocker < kFarPaletteSlotsGpu &&
                  kMatFarPalMask == 0xFFu,
              "far slots are 0..254 of one byte; 255 is the blocker-only cell");

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

std::vector<uint8_t> SmokeSourceTable(const std::vector<MaterialDef>& mats,
                                      const std::vector<ReactionGpu>& reactions) {
  std::vector<uint8_t> out(mats.size(), 0u);
  // The two products that ARE smoke in the sky, resolved by name like every
  // other authored reference (an absent one simply never matches).
  uint32_t fireId = 0, smokeId = 0;
  for (size_t i = 1; i < mats.size(); i++) {
    if (mats[i].name == "fire") fireId = (uint32_t)i;
    if (mats[i].name == "smoke") smokeId = (uint32_t)i;
  }
  auto smoky = [&](uint32_t p) {
    return p != 0 && p != kProdKeep && (p == fireId || p == smokeId);
  };
  for (size_t i = 1; i < mats.size(); i++) {   // 0 is air
    const MaterialGpu& g = mats[i].gpu;
    for (uint32_t k = 0; k < g.reactCount; k++) {
      const size_t ri = (size_t)g.reactOffset + k;
      if (ri >= reactions.size()) break;
      const ReactionGpu& r = reactions[ri];
      const uint32_t kind = r.packed & 3u;
      if ((kind == kReactDecay && smoky(r.prodSelf)) ||
          (kind == kReactEmit && smoky(r.prodNbr))) {
        out[i] = 1u;
        break;
      }
    }
  }
  return out;
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

// Stain-type registry: stain name -> slot, assigned first-seen. Keyed by NAME
// rather than by material so several materials can share one stain (any of
// the future gore materials can all stain "blood"), and so the slot numbers
// stay stable as long as the file order of first use does.
//
// TWO SLOT SPACES, because only one of them is scarce:
//   * GROUND slots 1..7 -- a stain that can reach the world lives in the voxel
//     word's 3 stain-type bits (world.h), and that is all the bits there are.
//     Running out is a loud load error.
//   * BODY-ONLY slots 8..255 -- a `"bodyOnly": true` stain (a bruise, acid's
//     coat) never touches a grid cell, and a body coat is DRAWN BY ITS
//     MATERIAL (sim/microbody.h, the stain lattice), not through the palette.
//     It still needs a nonzero slot -- gameplay asks "does this substance
//     stain at all?" as `stainSlot != 0` -- but it must not spend one of the
//     seven, which it used to: acid and bruise held two of them while
//     enchanted water and blood had to borrow wet's and blood's look.
// One name is one kind: naming a stain bodyOnly in one material and a ground
// stain in another is an error rather than a silent promotion.
struct StainRegistry {
  static constexpr uint32_t kFirstBodySlot = kStainTypeMax + 1;
  std::map<std::string, uint32_t> slots;
  uint32_t groundUsed = 0, bodyUsed = 0;
  // Why the last SlotOf returned 0.
  bool full = false, mixed = false;
  uint32_t SlotOf(const std::string& name, bool bodyOnly) {
    full = mixed = false;
    auto it = slots.find(name);
    if (it != slots.end()) {
      if ((it->second >= kFirstBodySlot) != bodyOnly) {
        mixed = true;
        return 0;
      }
      return it->second;
    }
    uint32_t slot;
    if (bodyOnly) {
      if (kFirstBodySlot + bodyUsed > 255u) { full = true; return 0; }
      slot = kFirstBodySlot + bodyUsed++;
    } else {
      if (groundUsed >= kStainTypeMax) { full = true; return 0; }
      slot = 1u + groundUsed++;  // 0 means "unstained"
    }
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
  // `"stain": false` is a liquid opting OUT of the default body coat
  // (DefaultLiquidStain); it stains nothing, exactly as an absent block used to.
  if (s.is_boolean() && !s.get<bool>()) return;
  if (!s.is_object()) {
    errors += path + ": material \"" + d.name + "\": \"stain\" must be an object\n";
    return;
  }
  // Only a liquid can stain what it touches. The sim rule runs off the liquid
  // movement path, and a staining SOLID would need a different mechanism
  // entirely — rejecting it here beats silently doing nothing at runtime.
  //
  // ---- ...AND THAT MECHANISM NOW EXISTS, FOR BODIES (2026-09-16) ----------
  //
  // `"bodyOnly": true` is a solid saying "I am a COAT, not a spill". The body
  // stain (phys/bodystain.h) is a 16-bit word on a creature's own lattice,
  // written by C++ at the point of injury and drawn by its material — it
  // never goes near the liquid movement path, so the
  // objection above does not apply to it and the rejection was costing the one
  // thing a solid legitimately wants a stain slot FOR: a bruise, which is a
  // colour that deepens under the skin rather than a fluid lying on it.
  //
  // The world-facing half of the block is FORCED to zero rather than trusted,
  // because those fields only mean anything on the liquid path: a bodyOnly
  // stain with a `chance` would read as a rule that never fires, which is the
  // silent-no-op this check exists to prevent in the first place.
  const bool bodyOnly = s.value("bodyOnly", false);
  if (d.gpu.klass != CLASS_LIQUID && !bodyOnly) {
    errors += path + ": material \"" + d.name +
              "\": only liquids can stain (class is not liquid; a solid that "
              "means a body COAT must say \"bodyOnly\": true)\n";
    return;
  }
  d.stain = s.value("type", d.name);
  uint32_t slot = stainReg.SlotOf(d.stain, bodyOnly);
  if (slot == 0) {
    if (stainReg.mixed)
      errors += path + ": material \"" + d.name + "\": stain type \"" + d.stain +
                "\" is bodyOnly on one material and a ground stain on another "
                "(one stain name is one kind; give one of them its own name)\n";
    else if (bodyOnly)
      errors += path + ": material \"" + d.name +
                "\": more than 248 distinct bodyOnly stain types\n";
    else
      errors += path + ": material \"" + d.name + "\": more than " +
                std::to_string(kStainTypeMax) +
                " distinct GROUND stain types (the voxel word has 3 stain-type "
                "bits; bodyOnly stains do not count against them)\n";
    return;
  }
  // A stain GLOWS on a body as its material glows, unless the coat block says
  // otherwise (ParseCoat reads "glow" with this as its default): enchanted
  // blood's coat shines because the liquid does, with nothing authored twice.
  d.coatGlow = std::min<uint32_t>(d.gpu.emission, 255u);

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
  // A BODY COAT HAS NO SPILL. `chance`/`consume` drive the liquid soak in
  // sim_particle.wgsl and a solid never reaches it, so they are forced rather
  // than defaulted: an authored value there would be a rule that reads as live
  // and never fires. `amount` survives because the BODY path uses it.
  int chance = bodyOnly ? 0 : s.value("chance", 60);
  int consume = bodyOnly ? 0 : s.value("consume", 0);
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
  // "dries": a GROUND stain of this type comes off by itself, `dries` per
  // mille per tick per level, while the liquid that made it is not touching
  // it (sim_step.wgsl stainDry; the top surfaces are dried by sim_mutate.wgsl
  // `rainFall`'s column sample). "rain": this is what falls as rain.
  const int dries = bodyOnly ? 0 : s.value("dries", 0);
  if (dries < 0 || dries > (int)kStainChanceMax) {
    errors += path + ": material \"" + d.name +
              "\": stain dries must be 0..1000 per-mille\n";
  } else {
    d.stainDries = (uint32_t)dries;
  }
  d.stainIsRain = !bodyOnly && s.value("rain", false);
  // "opacity" 0..1: how strongly the stain shows on the GROUND (the body
  // coat's is "coat": {"opacity"}). Render-only.
  const float gOpacity = s.value("opacity", 1.0f);
  if (gOpacity < 0.0f || gOpacity > 1.0f) {
    errors += path + ": material \"" + d.name +
              "\": stain opacity must be 0..1\n";
  } else {
    d.stainGroundOpacity = (uint32_t)(gOpacity * 255.0f + 0.5f);
  }

  // Preserve any absorb capacity already parsed for this material: the two
  // halves of stainPack are authored in separate JSON blocks and either may be
  // read first, so neither may clobber the other's bits.
  // A BODY-ONLY stain keeps its slot on the CPU side and out of the GPU pack:
  // every kernel that marks the ground (sim_step doStaining, sim_particle's
  // landing, the fluid seam) gates on the pack's type bits, so zero there is
  // "never stains the world" with no kernel knowing the flag exists. That is
  // what lets a LIQUID (acid) coat a body without painting every rock it eats.
  d.stainSlot = slot;
  const uint32_t gpuSlot = bodyOnly ? 0u : slot;
  d.gpu.stainPack = (d.gpu.stainPack & ((kStainPackAbsorbMask << kStainPackAbsorbShift) |
                                        kStainPackWashesBit)) |
                    ((uint32_t)gpuSlot << kStainPackTypeShift) |
                    ((uint32_t)amount << kStainPackAmtShift) |
                    ((uint32_t)chance << kStainPackChanceShift) |
                    ((uint32_t)consume << kStainPackConsumeShift);
}

// Parses "coat": { decay, shed, effects } — what this substance does while it
// is ON A BODY, as opposed to what it does to the ground (that is "stain").
// Absent = it sits there until something washes it off, which is every
// material that predates the feature.
//
// REQUIRES a "stain" block, and says so rather than defaulting: the coat and
// the stain are two readings of one substance (the coat word carries the
// material id, the stain block is how that material is DRAWN), so a coat with
// no stain is invisible on a body and a load-time error is the only way the
// author finds out. Must therefore be parsed AFTER ParseStain.
static void ParseCoat(const json& m, const std::string& path, MaterialDef& d,
                      std::string& errors) {
  if (!m.contains("coat")) return;
  const json& co = m["coat"];
  if (!co.is_object()) {
    errors += path + ": material \"" + d.name + "\": \"coat\" must be an object\n";
    return;
  }
  if (d.stain.empty()) {
    errors += path + ": material \"" + d.name +
              "\": coat requires stain (a coat is drawn through the stain "
              "palette; author a \"stain\" block first)\n";
    return;
  }
  float decay = co.value("decay", 0.0f);
  int shed = co.value("shed", 0);
  int decayFloor = co.value("decayFloor", 0);
  if (decay < 0.0f) {
    errors += path + ": material \"" + d.name +
              "\": coat decay must be >= 0 seconds per level (0 = never)\n";
    decay = 0.0f;
  }
  if (shed < 0 || shed > (int)kStainChanceMax) {
    errors += path + ": material \"" + d.name +
              "\": coat shed must be 0..1000 per-mille\n";
    shed = 0;
  }
  if (decayFloor < 0 || decayFloor > 15) {
    errors += path + ": material \"" + d.name +
              "\": coat decayFloor must be 0..15\n";
    decayFloor = 0;
  }
  // "opacity" 0..1: how strongly this coat SHOWS on a body, at any amount.
  // Water soaks skin to the same amount blood does (that amount is what the
  // wash / wick / dry rules count), but a wet arm is still an arm; at full
  // strength the near-grey wet colour turned people into statues. The value
  // rides in the ALPHA byte of stainColor, which nothing else reads
  // (unpackColor is RGB), and only microbody.wgsl bodyStainTint applies it:
  // the ground's stain path is untouched.
  float opacity = co.value("opacity", 1.0f);
  if (opacity < 0.0f || opacity > 1.0f) {
    errors += path + ": material \"" + d.name +
              "\": coat opacity must be 0..1\n";
    opacity = 1.0f;
  }
  d.gpu.stainColor = (d.gpu.stainColor & 0x00FFFFFFu) |
                     ((uint32_t)(opacity * 255.0f + 0.5f) << 24);
  d.coatDecay = decay;
  d.coatDecayFloor = (uint32_t)decayFloor;
  d.coatShed = (uint32_t)shed;
  d.coatEffects = co.value("effects", std::vector<std::string>{});
  // Glow + pulse (render-only; MaterialDef::coatGlow), contact rate and depth
  // (MaterialDef::coatContact / coatDepth).
  const int glow = co.value("glow", (int)d.coatGlow);  // default: the emission
  if (glow < 0 || glow > 255) {
    errors += path + ": material \"" + d.name + "\": coat glow must be 0..255\n";
  } else {
    d.coatGlow = (uint32_t)glow;
  }
  const float pulse = co.value("pulse", 0.0f);
  if (pulse < 0.0f || pulse * 100.0f > (float)kCoatPulseMask) {
    errors += path + ": material \"" + d.name +
              "\": coat pulse must be 0..40 Hz\n";
  } else {
    d.coatPulseHz = pulse;
  }
  const int contact = co.value("contact", -1);
  if (contact < -1 || contact > (int)kStainChanceMax) {
    errors += path + ": material \"" + d.name +
              "\": coat contact must be 0..1000 per-mille (or -1)\n";
  } else {
    d.coatContact = contact;
  }
  const float depth = co.value("depth", 1.0f);
  if (!(depth > 0.0f) || depth > 64.0f) {
    errors += path + ": material \"" + d.name +
              "\": coat depth must be > 0 and <= 64 world voxels\n";
  } else {
    d.coatDepth = depth;
  }
  // RESTORATION (materials.h coatRestore): the `restore` EFFECT is the switch
  // and the two numbers are how much and how fast. Both halves are required
  // of each other, so a coat cannot claim to heal and heal nothing, or heal
  // without saying so in the tag list other readers (CoatTagFraction) see.
  bool restoreTag = false;
  for (const std::string& fx : d.coatEffects) restoreTag |= fx == "restore";
  const float restore = co.value("restore", 0.0f);
  const float restoreRate = co.value("restoreRate", 1.0f);
  if (restore < 0.0f || restore > 16.0f) {
    errors += path + ": material \"" + d.name +
              "\": coat restore must be 0..16 world voxels per level\n";
  } else if (restoreRate <= 0.0f || restoreRate > 64.0f) {
    errors += path + ": material \"" + d.name +
              "\": coat restoreRate must be > 0 and <= 64 world voxels/s\n";
  } else if (restoreTag != (restore > 0.0f)) {
    errors += path + ": material \"" + d.name +
              "\": coat \"restore\" effect and coat restore > 0 go together\n";
  } else if (restoreTag) {
    d.coatRestore = restore;
    d.coatRestoreRate = restoreRate;
  }
  // A COAT THAT CARRIES AN INFECTION (materials.h coatInfects): by name,
  // resolved once the whole table exists (LoadMaterials), because the
  // infection is usually authored after the liquid that carries it.
  if (co.contains("infects")) {
    if (co["infects"].is_string())
      d.coatInfectsName = co["infects"].get<std::string>();
    else
      errors += path + ": material \"" + d.name +
                "\": coat infects must be a material name\n";
  }
  const int infectCost = co.value("infectCost", 5);
  if (infectCost < 1 || infectCost > 15) {
    errors += path + ": material \"" + d.name +
              "\": coat infectCost must be 1..15 levels\n";
  } else {
    d.coatInfectCost = (uint32_t)infectCost;
  }
}

// "infect": {...} -- this material IS an infection (materials.h `infect`).
// Targets stay as authored here; LoadMaterials resolves them against the tag
// registry and the material table once both are complete.
static void ParseInfect(const json& m, const std::string& path, MaterialDef& d,
                        std::string& errors) {
  if (!m.contains("infect")) return;
  const json& in = m["infect"];
  if (!in.is_object()) {
    errors += path + ": material \"" + d.name + "\": \"infect\" must be an object\n";
    return;
  }
  if (d.gpu.klass != CLASS_SOLID) {
    errors += path + ": material \"" + d.name +
              "\": an infection is tissue gone wrong and must be class solid\n";
    return;
  }
  auto rate = [&](const char* key, float& out) {
    if (!in.contains(key)) return;  // absent: the gore knobs (sentinel < 0)
    const float r = in.value(key, -1.0f);
    if (!(r >= 0.0f) || r > 30.0f) {
      errors += path + ": material \"" + d.name + "\": infect " + key +
                " must be 0..30 per voxel per second\n";
      return;
    }
    out = r;
  };
  rate("spread", d.infectSpread);
  rate("eat", d.infectEat);
  if ((d.infectSpread < 0.0f) != (d.infectEat < 0.0f))
    errors += path + ": material \"" + d.name +
              "\": infect spread and eat are authored together or not at all\n";
  const int floor = in.value("floor", 0);
  if (floor < 0 || floor > 64) {
    errors += path + ": material \"" + d.name + "\": infect floor must be 0..64\n";
  } else {
    d.infectFloor = (uint32_t)floor;
  }
  if (in.contains("targets")) {
    if (!in["targets"].is_array()) {
      errors += path + ": material \"" + d.name +
                "\": infect targets must be a list of names / \"tag:x\"\n";
    } else {
      for (const json& t : in["targets"])
        if (t.is_string()) d.infectTargets.push_back(t.get<std::string>());
      if (d.infectTargets.empty())
        errors += path + ": material \"" + d.name +
                  "\": infect targets is empty (omit it for the rotRate rule)\n";
    }
  }
  const float hp = in.value("hp", 0.0f);
  if (!(hp >= 0.0f) || hp > 100000.0f) {
    errors += path + ": material \"" + d.name +
              "\": infect hp must be 0..100000 per world voxel\n";
  } else {
    d.infectHp = hp;
  }
  d.infectTurns = in.value("turns", false);
  const std::string cause = in.value("cause", std::string("infection"));
  if (cause != "burn" && cause != "infection")
    errors += path + ": material \"" + d.name +
              "\": infect cause must be \"burn\" or \"infection\"\n";
  d.infectBooksBurn = cause == "burn";
  d.infectDeath = in.value("death", std::string());
  d.infectCuredName = in.value("cured", std::string());
  // What the UI calls it ("rot", "venom"): the HUD row and the remedy line
  // name the infection by this, never by a hardcoded word. Absent = the
  // material's own name.
  d.infectLabel = in.value("label", d.name);
  d.infect = true;
}

// ---- THE CHARGE FIELD: materials.json "electric" (docs/PLAN_electricity.md) --
// Names (`ignite.into`, `char`) are resolved in LoadAssets once the whole table
// exists (forward references).
static void ParseElectric(const json& m, const std::string& path, MaterialDef& d,
                          std::string& errors) {
  d.elec = ElecDef{};
  if (!m.contains("electric")) return;
  const json& t = m["electric"];
  const std::string at = path + ": material \"" + d.name + "\": electric";
  if (!t.is_object()) {
    errors += at + " must be an object\n";
    return;
  }
  const int resist = t.value("resist", 0);
  if (resist < 0 || resist > (int)kElecResistInsulator - 1)
    errors += at + ".resist must be 0..4094 (0 = insulator)\n";
  d.elec.resist = (uint32_t)std::clamp(resist, 0, (int)kElecResistInsulator - 1);
  const int source = t.value("source", 0);
  if (source < 0 || source > (int)kElecPMax) errors += at + ".source must be 0..65535\n";
  d.elec.source = (uint32_t)std::clamp(source, 0, (int)kElecPMax);
  // Wave 2: the resist a conducting liquid falls to with this dissolved in
  // it at saturation (salt -> brine). 0 = not an electrolyte.
  const int dissolved = t.value("dissolved", 0);
  if (dissolved < 0 || dissolved > (int)kElecResistInsulator - 1)
    errors += at + ".dissolved must be 0..4094 (0 = not an electrolyte)\n";
  d.elec.dissolved = (uint32_t)std::clamp(dissolved, 0, (int)kElecResistInsulator - 1);
  if (t.contains("ignite")) {
    const json& g = t["ignite"];
    if (!g.is_object() || !g.contains("into") || !g["into"].is_string()) {
      errors += at + ".ignite needs \"into\"\n";
    } else {
      d.elec.igniteInto = g["into"].get<std::string>();
      d.elec.igniteChanceMille = g.value("chance", 10.0);
      if (!(d.elec.igniteChanceMille >= kReactChanceMinMille) ||
          d.elec.igniteChanceMille > 1000.0)
        errors += at + ".ignite: chance must be " + FormatMille(kReactChanceMinMille) +
                  "..1000 per-mille\n";
    }
  }
  if (t.contains("char")) {
    if (!t["char"].is_string()) errors += at + ".char must be a material name\n";
    else d.elec.charInto = t["char"].get<std::string>();
    // E2: one chance caps both rolls (ignite with an air face, char without);
    // a char-only block takes ignite's default.
    if (d.elec.igniteChanceMille <= 0.0) d.elec.igniteChanceMille = 10.0;
  }
  // BODIES ONLY (wave 2 package B, mob_shock.cpp): the per-mille of a body
  // cell's P a creature made of this material feels (hp + stun).
  const int shock = t.value("shock", 0);
  if (shock < 0 || shock > 1000) errors += at + ".shock must be 0..1000 per-mille\n";
  d.elec.shockMille = (uint32_t)std::clamp(shock, 0, 1000);
  for (auto& [key, v] : t.items()) {
    (void)v;
    if (key != "resist" && key != "source" && key != "ignite" && key != "char" && key != "note" &&
        key != "dissolved" && key != "shock")
      errors += at + ": unknown key \"" + key + "\"\n";
  }
}

// ---- THE TEMPERATURE LAYER: materials.json "thermal" (docs/PLAN_temperature.md)
// Names (`into`, `partialInto`) are resolved in LoadAssets once the whole table
// exists (forward references). Thresholds are heat units, 0 = water freezes.
static void ParseThermal(const json& m, const std::string& path, MaterialDef& d,
                         std::string& errors) {
  d.thermal = ThermalDef{};
  if (!m.contains("thermal")) return;
  const json& t = m["thermal"];
  const std::string at = path + ": material \"" + d.name + "\": thermal";
  if (!t.is_object()) {
    errors += at + " must be an object\n";
    return;
  }
  const int emit = t.value("emit", 0);
  if (emit < 0 || emit > 255) errors += at + ".emit must be 0..255\n";
  d.thermal.emit = (uint32_t)std::clamp(emit, 0, 255);
  if (t.contains("inertia")) {
    const int k = t.value("inertia", 3);
    if (k < 0 || k > 7) errors += at + ".inertia must be 0..7\n";
    d.thermal.inertia = std::clamp(k, 0, 7);
  }
  struct Key { const char* name; uint32_t kind; const char* thr; };
  const Key keys[] = {{"melt", kHeatKindMelt, "above"},
                      {"ignite", kHeatKindIgnite, "above"},
                      {"freeze", kHeatKindFreeze, "below"}};
  for (const Key& k : keys) {
    if (!t.contains(k.name)) continue;
    const json& r = t[k.name];
    const std::string rat = at + "." + k.name;
    if (!r.is_object() || !r.contains(k.thr) || !r.contains("full") ||
        !r.contains("into") || !r["into"].is_string()) {
      errors += rat + " needs \"" + k.thr + "\", \"full\" and \"into\"\n";
      continue;
    }
    HeatTransition h;
    h.kind = k.kind;
    h.threshold = r.value(k.thr, 0);
    h.full = r.value("full", 0);
    h.chanceMille = r.value("chance", 10.0);
    h.into = r["into"].get<std::string>();
    h.partialInto = r.value("partialInto", std::string());
    h.surface = r.value("surface", false);
    const bool below = k.kind == kHeatKindFreeze;
    if (h.threshold < -256 || h.threshold > 400 || h.full < -256 || h.full > 400)
      errors += rat + ": thresholds must be -256..400 heat units\n";
    if (below ? h.full >= h.threshold : h.full <= h.threshold)
      errors += rat + ": \"full\" must be " + (below ? "below" : "above") + " \"" +
                k.thr + "\"\n";
    if (!(h.chanceMille >= kReactChanceMinMille) || h.chanceMille > 1000.0)
      errors += rat + ": chance must be " + FormatMille(kReactChanceMinMille) +
                "..1000 per-mille\n";
    if (d.thermal.transitions.size() >= kHeatMaxTransitions) {
      errors += rat + ": at most " + std::to_string(kHeatMaxTransitions) +
                " transitions per material\n";
      continue;
    }
    d.thermal.transitions.push_back(h);
  }
  for (auto& [key, v] : t.items()) {
    (void)v;
    if (key != "emit" && key != "inertia" && key != "melt" && key != "ignite" &&
        key != "freeze" && key != "note")
      errors += at + ": unknown key \"" + key + "\"\n";
  }
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

// ---- EVERY LIQUID COATS A BODY (2026-09-29) ---------------------------------
//
// A body coat exists only for a material with a stain slot, and every writer
// (contact in StainOneLimb, the pour brush, splashes) refuses one without --
// so a liquid nobody wrote a "stain" block for (syrup, slime, quicksilver, the
// potions: 19 of 24 liquids when this landed) could be walked through or poured
// on someone and leave nothing, and had no colour to draw if it had. This gives
// such a liquid the table's DEFAULT coat instead, built as ordinary JSON and fed
// through the same ParseStain / ParseCoat as an authored one, so there is one
// parser and one set of range checks:
//   * the stain is `bodyOnly`: the ground's seven slots are all spent (lava/oil
//     note in DESIGN.md), and a bodyOnly slot has no GPU type bits, so no kernel
//     sees it and the world hash cannot move;
//   * its colour is the liquid's OWN base colour (color0), not the darker
//     color1 an authored block defaults to -- syrup on an arm should read as
//     syrup, and the coat `opacity` (0.5 by default) is what makes it a film;
//   * the coat numbers come from materials.json `liquidStainDefault.coat`
//     (built-in fallback below), and any key the material's own "coat" block
//     authors wins, so a liquid can tune one number without owning the stain.
// `"stain": false` opts a liquid out. Hot and corrosive liquids pick up the
// hot/corrosive coat behaviour the same way lava and acid do (mob.cpp
// matCorrodes_, from their rules and tags) -- which is why the default decays.
static bool DefaultLiquidStain(const json& table, const json& m, const MaterialDef& d,
                               json& out) {
  if (d.gpu.klass != CLASS_LIQUID || m.contains("stain")) return false;
  static const json kBuiltIn = {
      {"stain", {{"amount", 5}}},
      {"coat", {{"opacity", 0.5}, {"decay", 6.0}, {"contact", 60}}}};
  const json& def = table.contains("liquidStainDefault") &&
                            table["liquidStainDefault"].is_object()
                        ? table["liquidStainDefault"]
                        : kBuiltIn;
  out = m;
  json stain = def.value("stain", json::object());
  if (!stain.is_object()) stain = json::object();
  stain.erase("//");
  stain["type"] = d.name;
  stain["bodyOnly"] = true;
  char hex[8];
  const uint32_t c = d.gpu.color0;  // 0xAABBGGRR (ParseColor)
  std::snprintf(hex, sizeof hex, "#%02x%02x%02x", c & 0xFFu, (c >> 8) & 0xFFu,
                (c >> 16) & 0xFFu);
  if (!stain.contains("color")) stain["color"] = hex;
  out["stain"] = stain;
  json coat = def.value("coat", json::object());
  if (!coat.is_object()) coat = json::object();
  coat.erase("//");
  if (m.contains("coat") && m["coat"].is_object())
    for (auto& [k, v] : m["coat"].items()) coat[k] = v;
  out["coat"] = coat;
  return true;
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

    // ---- angle of repose: the slope a PILE of this powder rests at ----
    //
    //   "repose": 34          // degrees, kReposeDegMin..kReposeDegMax
    //
    // POWDERS ONLY, and refused rather than ignored on anything else: it is the
    // one field whose meaning is a property of the powder movement chain, and
    // authoring it on a liquid or a solid can only be a mistake about what the
    // field does. Absent compiles to the word 0, which sim_step.wgsl reads as
    // "one down, one across" — the rule every powder had before the field
    // existed, so a material that says nothing is bit-identical to the old sim.
    //
    // The reposeDeg mirror is set for EVERY material, powder or not, so the
    // tuner and the wiki can read the value back without knowing the packing
    // (the same reason absorbCapacity and the wind nibbles are mirrored).
    d.reposeDeg = kReposeDegDefault;
    if (m.contains("repose")) {
      if (!m["repose"].is_number_integer()) {
        errors += path + ": material \"" + d.name +
                  "\": \"repose\" must be an integer number of degrees\n";
      } else if (d.gpu.klass != CLASS_POWDER) {
        errors += path + ": material \"" + d.name +
                  "\": \"repose\" is powders only (this one is a " + cls +
                  ") — it is the slope a PILE rests at\n";
      } else {
        const int32_t deg = m["repose"].get<int32_t>();
        // The two ends get DIFFERENT diagnostics, because they are different
        // facts and a shared message sent an author looking at the wrong one:
        // below the floor the lattice cannot express a flatter face at all,
        // above the ceiling a powder is a wall and should be a solid. Refused
        // rather than clamped either way — a clamp reads as the feature not
        // working.
        if (deg < kReposeDegMin) {
          errors += path + ": material \"" + d.name + "\": repose " +
                    std::to_string(deg) + " is below the floor of " +
                    std::to_string(kReposeDegMin) +
                    " degrees — 3:1 (18.43) is the flattest run:rise a cell "
                    "lattice can hold, and anything flatter would be a silent "
                    "clamp to it (see materials.h kReposeTiers)\n";
        } else if (deg > kReposeDegMax) {
          errors += path + ": material \"" + d.name + "\": repose " +
                    std::to_string(deg) + " is above the ceiling of " +
                    std::to_string(kReposeDegMax) +
                    " degrees — 1:3 (71.57) is the steepest run:rise, and a "
                    "powder that stands steeper than that is a solid\n";
        } else {
          d.reposeDeg = deg;
          d.gpu.repose = PackRepose(deg);
        }
      }
    }

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

    // ---- far-field look-alike (sim/materials.h kMatFarPalShift) ------------
    //
    //   "far": "stone"        at cascade distance I am stone
    //
    // Resolved to a slot in a POST-PASS below, for the reason the tint runs
    // give: it names another material, which may not have been read yet.
    d.farAlias = m.value("far", std::string{});

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

    // ---- fluid coupling (kFluidPack* in materials.h) ----
    // What a voxel of this does while it is inside a liquid. Same shape as the
    // wind block above and for the same reasons: authored by name, absent means
    // derived, out of range is an error rather than a wrap.
    //
    //   "fluid": { "lift": 0..15, "drag": 0..15, "wander": 0..15 }
    //
    // The one that carries meaning on its own is lift 0 — "liquids do not touch
    // this" — which is how a material opts OUT of buoyancy entirely and keeps
    // the older behaviour of stopping dead at the surface. Everything else is a
    // strength, and which way a material MOVES is decided by `density` against
    // the liquid's, never by a knob here.
    {
      const json* fj =
          m.contains("fluid") && m["fluid"].is_object() ? &m["fluid"] : nullptr;
      const int dLift = (int)DeriveFluidLift(d.gpu.klass);
      const int dDrag = (int)DeriveFluidDrag(d.gpu.density);
      const int dWander = (int)DeriveFluidWander(d.gpu.density);
      int lift = fj ? fj->value("lift", dLift) : dLift;
      int drag = fj ? fj->value("drag", dDrag) : dDrag;
      int wander = fj ? fj->value("wander", dWander) : dWander;
      if (lift < 0 || lift > (int)kFluidMax || drag < 0 ||
          drag > (int)kFluidMax || wander < 0 || wander > (int)kFluidMax) {
        errors += path + ": material \"" + d.name +
                  "\": fluid lift/drag/wander must be 0..15\n";
        auto cl = [](int v) {
          return v < 0 ? 0 : (v > (int)kFluidMax ? (int)kFluidMax : v);
        };
        lift = cl(lift);
        drag = cl(drag);
        wander = cl(wander);
      }
      d.fluidLift = (uint32_t)lift;
      d.fluidDrag = (uint32_t)drag;
      d.fluidWander = (uint32_t)wander;
      d.gpu.fluidPack = ((uint32_t)lift & kFluidPackLiftMask)
                            << kFluidPackLiftShift |
                        ((uint32_t)drag & kFluidPackDragMask)
                            << kFluidPackDragShift |
                        ((uint32_t)wander & kFluidPackWanderMask)
                            << kFluidPackWanderShift;
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
    // ---- gore weights (materials.h woundHp / brainHp / rotRate) -------------
    // All three are CPU-only body rules: nothing here reaches a shader, the
    // sim, or the world hash. Clamped rather than rejected because a silly
    // number in a data file should misbehave visibly, not refuse to load.
    d.woundHp = m.value("woundHp", 1.0f);
    if (!(d.woundHp >= 0.0f)) d.woundHp = 0.0f;   // also catches NaN
    d.brainHp = m.value("brainHp", false);
    // -1 = unauthored, which MobDef resolves per creature against `tissue`.
    // Only clamp the TOP: the negative sentinel has to survive.
    d.rotRate = m.value("rotRate", -1.0f);
    if (d.rotRate > 1.0f) d.rotRate = 1.0f;
    ParseInfect(m, path, d, errors);  // after klass: it requires a solid
    d.bareBlood = std::clamp(m.value("bareBlood", 0.0f), 0.0f, 1.0f);
    if (!(d.bareBlood == d.bareBlood)) d.bareBlood = 0.0f;   // NaN
    d.bleed = std::clamp(m.value("bleed", 1.0f), 0.0f, 4.0f);
    if (!(d.bleed == d.bleed)) d.bleed = 1.0f;               // NaN
    // The fluid a wound in this leaks, by name; resolved (and derived from
    // `rubble` when absent) after the whole table exists -- LoadMaterials.
    if (m.contains("bleedFluid")) {
      if (m["bleedFluid"].is_string())
        d.bleedFluidName = m["bleedFluid"].get<std::string>();
      else if (m["bleedFluid"].is_boolean() && !m["bleedFluid"].get<bool>())
        d.bleedFluidOff = true;
    }
    // A body made of this under a blow (materials.h shell / struck / burst):
    // names resolved after the whole table exists, like bleedFluid.
    d.shell = m.value("shell", false);
    if (m.contains("struck") && m["struck"].is_object()) {
      const auto& s = m["struck"];
      d.struckName = s.value("material", std::string{});
      d.struckChance = std::clamp(s.value("chance", 1.0f), 0.0f, 1.0f);
      if (!(d.struckChance == d.struckChance)) d.struckChance = 0.0f;
      d.struckRadius = std::clamp(s.value("radius", 1), 1, 4);
      if (s.contains("arcs")) {
        if (s["arcs"].is_boolean())
          d.struckArcs = s["arcs"].get<bool>() ? 1.0f : 0.0f;
        else if (s["arcs"].is_number())
          d.struckArcs = std::clamp(s["arcs"].get<float>(), 0.0f, 1.0f);
        if (!(d.struckArcs == d.struckArcs)) d.struckArcs = 0.0f;
      }
    }
    if (m.contains("burst") && m["burst"].is_object()) {
      const auto& b = m["burst"];
      d.burstName = b.value("material", std::string{});
      d.burstRadius = std::clamp(b.value("radius", 3), 1, 6);
      d.burstArcs = b.value("arcs", false);
    }
    // 0 intact / 1 half / 2 whole (materials.h burnStage). Clamped, like the
    // weights above: a silly number should misbehave visibly, not refuse.
    d.burnStage = (uint8_t)std::clamp(m.value("burnStage", 0), 0, 2);
    // The tariff base. Derived from density when not authored: a voxel of
    // something heavy is worth more to conjure than a voxel of smoke, which is
    // the right default for the long tail and wrong for exactly the materials
    // a designer will author (gold, steel, blood) -- so those carry the key.
    {
      int32_t derived = d.gpu.density / 1000;
      if (derived < 1) derived = 1;
      if (derived > 12) derived = 12;
      d.arcane = m.value("arcane", derived);
      if (d.arcane < 0) d.arcane = 0;
    }
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
    {
      json withDefault;  // a liquid with no stain block: DefaultLiquidStain
      d.stainAuto = DefaultLiquidStain(j, m, d, withDefault);
      const json& sm = d.stainAuto ? withDefault : m;
      ParseStain(sm, path, stainReg, d, errors);
      ParseCoat(sm, path, d, errors);  // after ParseStain: it checks d.stain
    }
    ParseAbsorb(m, path, d, errors);
    ParseThermal(m, path, d, errors);
    ParseElectric(m, path, d, errors);
    d.tags = m.value("tags", std::vector<std::string>{});
    for (auto& t : d.tags) {
      uint32_t bit = tagReg.MaskOf(t, true);
      if (bit == 0 && tagReg.full)
        errors += path + ": material \"" + d.name + "\": more than 32 distinct tags\n";
      d.gpu.tagMask |= bit;
      // A hot GAS is a flame (materials.h kMatFlagFlame): the one thing a
      // reacting coat may release into the world.
      if (t == "hot" && d.gpu.klass == CLASS_GAS) d.gpu.flags |= kMatFlagFlame;
      // A gas_heavy GAS sinks and creeps instead of rising (kMatFlagHeavyGas,
      // sim_step.wgsl stepGas). The tag on a non-gas means nothing.
      if (t == "gas_heavy" && d.gpu.klass == CLASS_GAS) d.gpu.flags |= kMatFlagHeavyGas;
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

  // ---- assign FAR PALETTE SLOTS (sim/materials.h kMatFarPalShift) ----------
  // A far cascade cell is ONE byte (common.wgsl FAR_PAL_BLOCKER): 0 air, 255
  // "no material but the conservative blocker", and 1..254 an index into the
  // FAR PALETTE (world.h kFarPaletteBaseGpu), which maps back to a material.
  // Until 2026-10-02 it was seven bits of slot and a blocker flag: 128 slots
  // for ~207 materials, so the table needed 79 `"far"` aliases just to load,
  // and charcoal drew as white ash at distance, a thatch roof as pale tussock.
  // With 255 slots every material owns one; `"far": "<material>"` is now only
  // for two materials that should be indistinguishable at distance (sandstone
  // is sand's colour, so a firmed sand skin does not change the far field).
  //
  // IDENTITY FIRST: while every material's id fits, slot == id. Aliases FREE
  // slots, and pass 2 hands the freed ones to the ids that no longer fit.
  {
    std::vector<int> aliasOf(mats.size(), -1);
    for (size_t i = 0; i < mats.size(); i++) {
      if (mats[i].farAlias.empty()) continue;
      int t = FindMaterial(mats, mats[i].farAlias);
      if (t < 0) {
        errors += path + ": material \"" + mats[i].name + "\": far alias \"" +
                  mats[i].farAlias + "\" is not a material\n";
      } else if ((size_t)t == i) {
        errors += path + ": material \"" + mats[i].name +
                  "\": far alias points at itself\n";
      } else if (t == 0) {
        // Slot 0 is how every reader spells "nothing here" -- farOcc counts a
        // zero byte as an empty cell. A material that aliased to air would not
        // be "drawn as air at distance", it would delete the cell from the
        // cascade's occupancy, which is not what anyone means by it.
        errors += path + ": material \"" + mats[i].name +
                  "\": far alias \"air\" -- slot 0 means an EMPTY far "
                  "cell, not a transparent one\n";
      } else {
        aliasOf[i] = t;
      }
    }
    // ONE HOP ONLY, checked against the snapshot so the diagnosis does not
    // depend on file order: the slot a far byte names must always belong to a
    // material that paints itself, or a reader would have to chase a chain it
    // has no table for.
    const std::vector<int> alias0 = aliasOf;
    for (size_t i = 0; i < mats.size(); i++) {
      if (alias0[i] < 0 || alias0[alias0[i]] < 0) continue;
      errors += path + ": material \"" + mats[i].name + "\": far alias \"" +
                mats[i].farAlias + "\" is itself an alias (of \"" +
                mats[alias0[i]].farAlias + "\") -- name the material that owns "
                "the slot\n";
      aliasOf[i] = -1;
    }

    size_t owned = 0;
    for (size_t i = 0; i < mats.size(); i++)
      if (aliasOf[i] < 0) owned++;
    if (owned > kMatFarPalSlots)
      errors += path + ": " + std::to_string(owned) +
                " materials (including the implicit air at id 0) each want their"
                " own colour in the far-field cascade, which has only " +
                std::to_string(kMatFarPalSlots) +
                " palette slots -- one per-cell byte, whose last value is the"
                " blocker-only cell. Give the ones that look"
                " alike at cascade distance a `\"far\": \"<existing material>\"`"
                " in materials.json so they share a slot, or widen the far cell"
                " (farVox is already 1 GiB).\n";

    std::vector<int> ownerOf(kMatFarPalSlots, -1);   // slot -> material
    std::vector<int> slotOf(mats.size(), -1);
    for (size_t i = 0; i < mats.size() && i < kMatFarPalSlots; i++) {
      if (aliasOf[i] >= 0) continue;
      ownerOf[i] = (int)i;
      slotOf[i] = (int)i;
    }
    uint32_t probe = 0;
    for (size_t i = 0; i < mats.size(); i++) {
      if (aliasOf[i] >= 0 || slotOf[i] >= 0) continue;
      while (probe < kMatFarPalSlots && ownerOf[probe] >= 0) probe++;
      if (probe >= kMatFarPalSlots) break;   // already reported above
      ownerOf[probe] = (int)i;
      slotOf[i] = (int)probe;
    }
    for (size_t i = 0; i < mats.size(); i++)
      if (aliasOf[i] >= 0) slotOf[i] = slotOf[aliasOf[i]];
    for (size_t i = 0; i < mats.size(); i++) {
      // A material with no slot only exists on a table that already errored;
      // 0 keeps it out of the cascade rather than aliasing it onto someone.
      uint32_t slot = slotOf[i] < 0 ? 0u : (uint32_t)slotOf[i];
      mats[i].farPalSlot = slot;
      mats[i].farPalOwner = slotOf[i] >= 0 && ownerOf[slot] == (int)i;
      mats[i].gpu.flags |= (slot & kMatFarPalMask) << kMatFarPalShift;
    }
  }
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
// THERE ARE NONE TODAY. The two that existed, `waterFreezes` and `iceMelts`,
// gated the sun-melt / night-freeze rules the temperature layer replaced
// (docs/PLAN_temperature.md); both rules and both switches are gone, so any
// "requires" is an unknown-switch load error until a new switch is added.
//
// Returns false and sets `known` = false for an unrecognised name.
static bool WeatherFlagEnabled(const std::string& name, bool& known) {
  (void)name;
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
    // ---- combustion.flamePct: how much of a fire the DRIFTING FLAME carries.
    //
    // The authored multiplier is the per-rule ratio -- WHICH rules let the
    // flame off lightly is a content decision, and the skin sear deliberately
    // is not one of them -- and this knob is the global strength over all of
    // them, exactly as spreadPct is the global strength over the ignition
    // chances those rules scale from. One authoritative source for each: the
    // JSON owns the ratios, the knob owns the strength.
    //
    // Keyed on the excepted neighbour being a HOT GAS, not on the name
    // "fire". That is what the exception is actually about -- a thing that
    // rises off a fire and floats away is a weaker igniter than the coals that
    // made it -- so a second hot gas is covered by construction, and an
    // exception naming a hot SOLID or LIQUID ("lava lights this faster") is
    // left alone, because that is not a drifting flame and this knob has no
    // business scaling it.
    double mult = e.second;
    {
      const int id = FindMaterial(mats, e.first);
      const uint32_t hot = tagReg.MaskOf("hot", false);
      if (id >= 0 && mats[(size_t)id].gpu.klass == CLASS_GAS && hot != 0 &&
          (mats[(size_t)id].gpu.tagMask & hot) != 0) {
        const int pct = CurrentTuning().combustion.flamePct;
        mult = mult * (double)(pct < 0 ? 0 : pct) / 100.0;
      }
    }
    // Same rounding as the authored chance: once, in double, on the CPU.
    double c = chanceMille * mult;
    if (c > 1000.0) c = 1000.0;
    x.chance = (uint32_t)(c * (double)kReactChanceScale + 0.5);
    if (x.chance == 0) x.chance = 1;
    extra.push_back(x);
  }
  return true;
}

// ---- "effects": what a rule does besides rewriting cells (2026-09-27) -------
//
// docs/PLAN_alchemy_chemistry.md 2.2. Parsed here into MaterialDef::ruleFx;
// ACTED ON by each consumer's own registry, by kind name (the world:
// game/session.cpp's reaction-effect pass; the alchemy bench: package C's
// event handlers). This list is only the loader's spell-checker: a kind
// outside it still loads (a consumer that does not know a kind ignores it)
// but is warned about once, so "exlpode" is visible instead of silent.
// explode / flash / shock: world AND bench. eject / burst / pop: the bench's
// (a vessel and a player at a bench are what they act on); the world ignores
// them by design (session.cpp ReactFxToBlasts).
const char* const kKnownEffectKinds[] = {"explode", "flash", "eject", "shock",
                                         "burst", "pop"};
const size_t kKnownEffectKindCount =
    sizeof(kKnownEffectKinds) / sizeof(kKnownEffectKinds[0]);

const RuleFx* FindRuleFx(const std::vector<MaterialDef>& mats, uint32_t fxId,
                         uint32_t* selfMat) {
  if (fxId == 0) return nullptr;
  for (size_t i = 0; i < mats.size(); i++)
    for (const RuleFx& f : mats[i].ruleFx)
      if (f.fxId == fxId) {
        if (selfMat) *selfMat = (uint32_t)i;
        return &f;
      }
  return nullptr;
}

// One rule's "effects" array. Ranges are checked per kind where the kind has
// a meaning the engine enforces (explode: radius <= kMaxExplosionRadius, the
// blast box sim_explode.wgsl is dispatched over; power > 0). Returns false
// with `errors` filled on a malformed entry.
static bool ParseEffects(const json& r, const std::string& path,
                         const std::string& self, RuleFx& out,
                         std::string& errors) {
  const json& e = r["effects"];
  if (!e.is_array()) {
    errors += path + ": reaction self=\"" + self + "\": effects must be an array\n";
    return false;
  }
  for (const json& x : e) {
    if (!x.is_object() || !x.contains("kind") || !x["kind"].is_string()) {
      errors += path + ": reaction self=\"" + self +
                "\": each effect needs a string \"kind\"\n";
      return false;
    }
    ReactionEffect fx;
    fx.kind = x["kind"].get<std::string>();
    fx.radius = x.value("radius", 0);
    fx.power = x.value("power", 0);
    fx.amount = x.value("amount", 0.0f);
    fx.what = x.value("what", std::string());
    bool known = false;
    for (size_t k = 0; k < kKnownEffectKindCount; k++)
      known |= fx.kind == kKnownEffectKinds[k];
    if (!known) {
      static std::vector<std::string> warned;
      if (std::find(warned.begin(), warned.end(), fx.kind) == warned.end()) {
        warned.push_back(fx.kind);
        std::fprintf(stderr,
                     "%s: reaction self=\"%s\": effect kind \"%s\" is not one "
                     "any consumer registers (materials.cpp kKnownEffectKinds); "
                     "it will be ignored\n",
                     path.c_str(), self.c_str(), fx.kind.c_str());
      }
    }
    if (fx.kind == "explode" &&
        (fx.radius < 1 || fx.radius > kMaxExplosionRadius || fx.power < 1 ||
         fx.power > 5000)) {
      errors += path + ": reaction self=\"" + self +
                "\": explode needs radius 1.." +
                std::to_string(kMaxExplosionRadius) + " and power 1..5000\n";
      return false;
    }
    out.effects.push_back(std::move(fx));
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
  // MaterialDef::ruleFx, built IN STEP with buckets/tails so the parallel
  // index (ruleFx[k] <-> rule reactOffset + k) cannot drift: every push to a
  // bucket or a tail pushes its RuleFx here in the same statement block.
  // Weather-dropped and infectSpread-dropped rules `continue` before either
  // push, so they vanish from both.
  std::vector<std::vector<RuleFx>> bucketFx(mats.size()), tailFx(mats.size());
  uint32_t nextFxId = 1;

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
    // infectSpread rules are dropped entirely when the rate is zero, same as
    // a weather switch that is off: the rot is turned off by the slider.
    // THE RATE IS THE SELF MATERIAL'S OWN (`infect.spread`, per voxel per
    // second; absent = gore.infectSpreadRate, the rot's knob) -- the same
    // number Mob::InfectStep runs on a body, so there is one per infection.
    const float infectRate =
        mats[selfId].infect && mats[selfId].infectSpread >= 0.0f
            ? mats[selfId].infectSpread
            : CurrentTuning().gore.infectSpreadRate;
    if (r.value("infectSpread", false) && infectRate <= 0.0f) continue;

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
    // ---- "infectSpread": a grid-side infection, at its body rate -----------
    //
    // On a body (Mob::InfectStep) an infectious voxel converts ONE matching
    // face-neighbour with chance `spread` per second. A grid cell is one world
    // voxel and a rule is a per-face, per-tick chance, so the same rate is
    // `spread` spread over the six faces: c per-mille a face a tick with
    // 6 * c/1000 * 30 = spread, i.e. c = spread * 1000 / 180. A cell walled in
    // by tissue on all six sides converts at exactly the body rate; one with
    // k tissue faces at k/6 of it. The authored chance is a placeholder.
    // Rate == 0 is handled above (the rule is dropped).
    if (r.value("infectSpread", false)) {
      chanceMille = (double)infectRate * 1000.0 / 180.0;
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
    // ---- weather (TickParams::weatherRain; kCondRain / kCondRainDamp) ----
    // "rain": the rule only happens to a rain-exposed cell while it rains (a
    // douse). "rainDamped": the rule's odds fall with rain and wet ground on
    // an exposed cell (an ignition). Both read the tick stream's integer, so
    // the weather reaches the world hash the way the day phase does.
    if (r.value("rain", false)) g.cond |= kCondRain;
    if (r.value("rainDamped", false)) g.cond |= kCondRainDamp;
    if ((g.cond & kCondRain) && (g.cond & kCondRainDamp))
      errors += path + ": reaction self=\"" + self +
                "\": rain and rainDamped are exclusive\n";

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
    // ---- effects (docs/PLAN_alchemy_chemistry.md 2.2) ----------------------
    // Parsed BEFORE the neighborChance split so the fx id rides g.cond into
    // the synthesized tail rules: the floating-flame exception of an
    // exploding rule still explodes (the tail is the same reaction at a
    // different rate, and the id names the effect list, not the slot).
    RuleFx fx;
    if (r.contains("effects") && ParseEffects(r, path, self, fx, errors) &&
        !fx.effects.empty()) {
      if (nextFxId > kMaxReactFx) {
        errors += path + ": reaction self=\"" + self + "\": more than " +
                  std::to_string(kMaxReactFx) +
                  " rules with effects (the GPU fx record keys on 5 bits; "
                  "world.h kPageFaultReactFx*)\n";
      } else {
        fx.fxId = nextFxId++;
        g.cond |= (fx.fxId & kCondFxMask) << kCondFxShift;
      }
    }
    // ---- a concentration condition (package B, docs/PLAN_solutes.md §4.1) --
    // Rides the RuleFx beside the effects so the neighborChance tails below
    // inherit it exactly as they inherit an fx id: the tail is the same
    // reaction at another rate. The species is a NAME, resolved against
    // solutes.json by Simulation::UploadSolutes (which loads after this).
    if (r.contains("solute")) {
      if (!r["solute"].is_string()) {
        errors += path + ": reaction self=\"" + self + "\": \"solute\" must be a species name\n";
      } else {
        fx.solute = r["solute"].get<std::string>();
        // The condition reads the SELF cell's dissolved mass, and only a
        // liquid carries any -- sim_step skips the side-array read for every
        // other class, so a condition on one would fire unconditionally.
        if (mats[selfId].gpu.klass != CLASS_LIQUID)
          errors += path + ": reaction self=\"" + self +
                    "\": a \"solute\" condition needs a liquid self\n";
        const int lo = r.value("cMin", 1), hi = r.value("cMax", 255);
        if (lo < 0 || lo > 255 || hi < lo || hi > 255)
          errors += path + ": reaction self=\"" + self +
                    "\": cMin/cMax must satisfy 0 <= cMin <= cMax <= 255\n";
        fx.soluteMin = (uint32_t)std::clamp(lo, 0, 255);
        fx.soluteMax = (uint32_t)std::clamp(hi, 0, 255);
      }
    }
    // ---- a DRYING rule (materials.h RuleFx::drying) -----------------------
    // CPU-side only: no fx id, no GPU field. Inherited by any neighborChance
    // tail below with the rest of the RuleFx.
    if (r.contains("drying")) {
      if (!r["drying"].is_boolean())
        errors += path + ": reaction self=\"" + self + "\": \"drying\" must be true or false\n";
      else
        fx.drying = r["drying"].get<bool>();
    }
    // A per-member exception splits this rule in two (see ExpandNeighborChance).
    // The base rule keeps its place; the exact-neighbour rules go to the tail
    // of the bucket, so a voxel touching both an ember and a flame rolls the
    // full rate first and nothing authored changes its index.
    if (r.contains("neighborChance")) {
      const size_t t0 = tails[selfId].size();
      ExpandNeighborChance(r, mats, tagReg, path, self, kind, chanceMille, g,
                           tails[selfId], errors);
      for (size_t k = t0; k < tails[selfId].size(); k++) tailFx[selfId].push_back(fx);
    }
    buckets[selfId].push_back(g);
    bucketFx[selfId].push_back(std::move(fx));
  }
  for (size_t i = 0; i < mats.size(); i++) {
    buckets[i].insert(buckets[i].end(), tails[i].begin(), tails[i].end());
    bucketFx[i].insert(bucketFx[i].end(), tailFx[i].begin(), tailFx[i].end());
  }

  out.clear();
  for (size_t i = 0; i < mats.size(); i++) {
    mats[i].gpu.reactOffset = (uint32_t)out.size();
    mats[i].gpu.reactCount = (uint32_t)buckets[i].size();
    out.insert(out.end(), buckets[i].begin(), buckets[i].end());
    // Empty (the common case) unless some rule of this material has effects;
    // otherwise exactly reactCount long, ruleFx[k] <-> rule reactOffset + k.
    bool any = false;
    for (const RuleFx& f : bucketFx[i]) any |= f.fxId != 0 || !f.solute.empty() || f.drying;
    mats[i].ruleFx.clear();
    if (any) mats[i].ruleFx = std::move(bucketFx[i]);
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
  // WHAT A WOUND IN EACH LEAKS (materials.h bleedFluid): authored by name,
  // else derived from a LIQUID rubble (flesh crumbles to blood, so it bleeds
  // blood), else no opinion. A named fluid that is not a liquid is refused:
  // a wound cannot drip stone.
  for (auto& d : m) {
    d.bleedFluid = 0;
    if (d.bleedFluidOff) continue;
    if (!d.bleedFluidName.empty()) {
      const int id = FindMaterial(m, d.bleedFluidName);
      if (id <= 0 || m[id].gpu.klass != CLASS_LIQUID)
        errors += materialsPath + ": material \"" + d.name +
                  "\": bleedFluid \"" + d.bleedFluidName +
                  "\" is not a liquid material\n";
      else
        d.bleedFluid = (uint32_t)id;
    } else if (!d.rubble.empty()) {
      const int id = FindMaterial(m, d.rubble);
      if (id > 0 && m[id].gpu.klass == CLASS_LIQUID) d.bleedFluid = (uint32_t)id;
    }
  }
  // WHAT A BLOW KNOCKS OUT AND WHAT A BREACH LETS GO (materials.h struck /
  // burst): by name, and only something that can be laid in the AIR -- a
  // gas, a liquid or a powder. A solid ball round a body would entomb it.
  for (auto& d : m) {
    auto resolveAir = [&](const std::string& nm, const char* key) -> uint32_t {
      if (nm.empty()) return 0u;
      const int id = FindMaterial(m, nm);
      if (id <= 0 || m[id].gpu.klass == CLASS_SOLID) {
        errors += materialsPath + ": material \"" + d.name + "\": " + key +
                  " \"" + nm + "\" is not a gas, liquid or powder material\n";
        return 0u;
      }
      return (uint32_t)id;
    };
    d.struckMat = resolveAir(d.struckName, "struck.material");
    d.burstMat = resolveAir(d.burstName, "burst.material");
  }
  // INFECTIONS AND THE COATS THAT CARRY THEM (materials.h `infect`,
  // `coatInfects`): names and tags resolved now that the table and the tag
  // registry are both complete. A coat may only name a material that IS an
  // infection, and a target tag must be one some material actually carries
  // -- a typo there would otherwise be an infection that attacks nothing.
  for (auto& d : m) {
    d.coatInfects = 0;
    if (!d.coatInfectsName.empty()) {
      const int id = FindMaterial(m, d.coatInfectsName);
      if (id <= 0 || !m[id].infect)
        errors += materialsPath + ": material \"" + d.name + "\": coat infects \"" +
                  d.coatInfectsName + "\" is not a material with an infect block\n";
      else
        d.coatInfects = (uint32_t)id;
    }
    d.infectCured = 0;
    if (!d.infectCuredName.empty()) {
      const int id = FindMaterial(m, d.infectCuredName);
      if (id <= 0)
        errors += materialsPath + ": material \"" + d.name +
                  "\": infect cured \"" + d.infectCuredName + "\" is not a material\n";
      else
        d.infectCured = (uint32_t)id;
    }
    d.infectTargetTags = 0;
    d.infectTargetIds.clear();
    for (const std::string& t : d.infectTargets) {
      if (t.rfind("tag:", 0) == 0) {
        const uint32_t bit = tags.MaskOf(t.substr(4), false);
        if (bit == 0)
          errors += materialsPath + ": material \"" + d.name + "\": infect target \"" +
                    t + "\" names a tag no material carries\n";
        d.infectTargetTags |= bit;
      } else {
        const int id = FindMaterial(m, t);
        if (id <= 0)
          errors += materialsPath + ": material \"" + d.name +
                    "\": infect target \"" + t + "\" is not a material\n";
        else
          d.infectTargetIds.push_back((uint32_t)id);
      }
    }
  }
  // THE TEMPERATURE LAYER (materials.json "thermal"): transition products by
  // NAME, now that the table is whole. An ignition's chance takes the same
  // combustion.spreadPct every other ignition does (one fire-speed knob, not
  // two). A material with transitions must be able to ACT, or the CA's
  // inert-solid skip (common.wgsl matCanAct) would never visit it.
  for (auto& d : m) {
    for (HeatTransition& h : d.thermal.transitions) {
      if (h.kind == kHeatKindIgnite) {
        const int pct = CurrentTuning().combustion.spreadPct;
        h.chanceMille = std::clamp(h.chanceMille * (double)(pct > 0 ? pct : 100) / 100.0,
                                   kReactChanceMinMille, 1000.0);
      }
      const int id = FindMaterial(m, h.into);
      if (id < 0)
        errors += materialsPath + ": material \"" + d.name + "\": thermal into \"" +
                  h.into + "\" is not a material\n";
      h.intoId = id < 0 ? 0u : (uint32_t)id;
      h.partialIntoId = 0;
      if (!h.partialInto.empty()) {
        const int pid = FindMaterial(m, h.partialInto);
        if (pid <= 0)
          errors += materialsPath + ": material \"" + d.name +
                    "\": thermal partialInto \"" + h.partialInto + "\" is not a material\n";
        else
          h.partialIntoId = (uint32_t)pid;
      }
    }
    if (!d.thermal.transitions.empty() && d.gpu.klass == CLASS_SOLID &&
        d.gpu.reactCount == 0 && (d.gpu.stainPack & kStainPackTypeMask) == 0)
      errors += materialsPath + ": material \"" + d.name +
                "\": a solid with thermal transitions needs at least one reaction "
                "rule (the CA skips inert solids)\n";
    if (d.thermal.transitions.size() > kHeatMaxTransitions)
      errors += materialsPath + ": material \"" + d.name + "\": too many thermal transitions\n";
  }
  // THE CHARGE FIELD (materials.json "electric"): E2's products by NAME.
  for (auto& d : m) {
    auto resolve = [&](const std::string& name, const char* what, uint32_t& id) {
      id = 0;
      if (name.empty()) return;
      const int r = FindMaterial(m, name);
      if (r < 0)
        errors += materialsPath + ": material \"" + d.name + "\": electric " + what + " \"" +
                  name + "\" is not a material\n";
      else
        id = (uint32_t)r;
    };
    resolve(d.elec.igniteInto, "ignite.into", d.elec.igniteIntoId);
    resolve(d.elec.charInto, "char", d.elec.charIntoId);
  }
  CheckPinnedMaterialIds(m, materialsPath, errors);
  if (!errors.empty()) return false;
  mats = std::move(m);
  reactions = std::move(r);
  return true;
}

// ---- the kMat* literals, resolved by NAME (rule-unification W1-D) ------------
//
// world.h's kMat* constants are compile-time ids because they feed the WGSL
// prelude and a hundred switch statements; they stay literals. What was
// missing is the RUNTIME half of the promise they make: that the material at
// that position in materials.json is the one the name says. check_invariants.py
// (check_material_ids) pins it statically; this makes the running build refuse
// too, so an edited materials.json cannot load stone where the code means
// water (kMatMushroomLarge drifted one slot exactly that way on 2026-09-17).
//
// The name of each constant is spelled out rather than derived from the C++
// identifier: `check_invariants.py pinnedids` holds this table and world.h to
// the same list, so a new kMat* without a row here fails the static check.
void CheckPinnedMaterialIds(const std::vector<MaterialDef>& mats,
                            const std::string& path, std::string& errors) {
  struct Pin {
    uint32_t id;
    const char* name;
  };
  static const Pin kPins[] = {
      {kMatAir, "air"}, {kMatStone, "stone"}, {kMatWood, "wood"},
      {kMatSand, "sand"}, {kMatGravel, "gravel"}, {kMatWater, "water"},
      {kMatOil, "oil"}, {kMatSmoke, "smoke"}, {kMatSteam, "steam"},
      {kMatFire, "fire"}, {kMatEmber, "ember"}, {kMatAsh, "ash"},
      {kMatLava, "lava"}, {kMatAcid, "acid"}, {kMatIce, "ice"},
      {kMatSnow, "snow"}, {kMatDirt, "dirt"}, {kMatPlant, "plant"},
      {kMatSeed, "seed"}, {kMatSprout, "sprout"}, {kMatStem, "stem"},
      {kMatFlower, "flower"}, {kMatVine, "vine"}, {kMatFungus, "fungus"},
      {kMatDust, "dust"}, {kMatMoltenGlass, "molten_glass"},
      {kMatGlass, "glass"}, {kMatSourceWater, "source_water"},
      {kMatSourceSand, "source_sand"}, {kMatSourceLava, "source_lava"},
      {kMatVoid, "void"}, {kMatMite, "mite"}, {kMatBlood, "blood"},
      {kMatGrass, "grass"}, {kMatLeaves, "leaves"},
      {kMatPineNeedles, "pine_needles"}, {kMatAutumnLeaves, "autumn_leaves"},
      {kMatBirchWood, "birch_wood"}, {kMatPetal, "petal"},
      {kMatGrassTuft, "grass_tuft"}, {kMatFoliageBush, "foliage_bush"},
      {kMatFlowerPoppy, "flower_poppy"}, {kMatFlowerDaisy, "flower_daisy"},
      {kMatPetalRed, "petal_red"}, {kMatPetalWhite, "petal_white"},
      {kMatPetalYellow, "petal_yellow"}, {kMatLeafGreen, "leaf_green"},
      {kMatStemGreen, "stem_green"}, {kMatFlowerBluebell, "flower_bluebell"},
      {kMatFlowerFoxglove, "flower_foxglove"},
      {kMatFlowerButtercup, "flower_buttercup"}, {kMatFern, "fern"},
      {kMatMushroomCluster, "mushroom_cluster"},
      {kMatToadstoolPale, "toadstool_pale"}, {kMatTallGrass, "tall_grass"},
      {kMatTallGrassHead, "tall_grass_head"},
      {kMatMushroomLarge, "mushroom_large"},
  };
  for (const Pin& p : kPins) {
    if (p.id < mats.size() && mats[p.id].name == p.name) continue;
    const int at = FindMaterial(mats, p.name);
    errors += path + ": world.h pins material id " + std::to_string(p.id) +
              " to \"" + p.name + "\" but " +
              (p.id < mats.size() ? "that position holds \"" + mats[p.id].name + "\""
                                  : std::string("the table ends before it")) +
              (at >= 0 ? " (\"" + std::string(p.name) + "\" is at " +
                             std::to_string(at) + ")"
                       : std::string(" (no material by that name)")) +
              " -- a material was inserted, removed or renamed above it; the "
              "compiled-in id would name the wrong substance\n";
  }
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

bool RuleNeedsSolute(const MaterialDef& m, uint32_t k) {
  return k < m.ruleFx.size() && !m.ruleFx[k].solute.empty();
}
