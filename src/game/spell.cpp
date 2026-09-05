#include "game/spell.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "sim/rng.h"

using nlohmann::json;

namespace {

// Counter-based, stateless (sim/rng.h): a stateful stream here would desync a
// replay the moment a frame boundary moved.
using rng::Hash3;
using rng::Pcg;

int32_t ClampI(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Integer square root. Deliberately not std::sqrt: every vector normalization
// in this file feeds authoritative spell state, and a float sqrt is exactly
// the kind of thing that differs in the last bit across compilers and would
// desync a lockstep replay (spell.h thesis 3).
int64_t IntSqrt(int64_t x) {
  if (x <= 0) return 0;
  int64_t r = 0, b = 1LL << 40;
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) {
      x -= r + b;
      r = (r >> 1) + b;
    } else {
      r >>= 1;
    }
    b >>= 2;
  }
  return r;
}

// Radius growth for "×N volume": r × cbrt(N), per-mille, for N = 1..8. The
// multiplier means "N times as much matter", so the RADIUS grows as the cube
// root — ×2 converts twice the volume, not eight times.
int32_t ScaleRadiusCbrt(int32_t r, int32_t n) {
  static const int32_t kCbrtMille[9] = {1000, 1000, 1260, 1442, 1587,
                                        1710, 1817, 1913, 2000};
  if (n <= 1) return r;
  if (n > 8) n = 8;
  return (int32_t)(((int64_t)r * kCbrtMille[n] + 500) / 1000);
}

int32_t Cube(int32_t r) {
  const int64_t d = 2 * (int64_t)r + 1;
  const int64_t v = d * d * d;
  return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)v;
}

int32_t SatMul(int32_t a, int32_t b) {
  const int64_t v = (int64_t)a * b;
  return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)v;
}
int32_t SatAdd(int32_t a, int32_t b) {
  const int64_t v = (int64_t)a + b;
  return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)v;
}

// Gravity in 24.8 voxels per tick², from the engine's own constants: 9.81 m/s²
// over kVoxelMeters, over a 30 Hz tick, squared. Integer arithmetic on the
// authored constants so every machine derives the same number (~27).
constexpr int32_t kTicksPerSecond = 30;
constexpr int32_t kGravityFxPerTick2 =
    (int32_t)((int64_t)981 * kVoxelsPerMetre * kSpellFxOne /
              (100 * kTicksPerSecond * kTicksPerSecond));

const char* kSortNames[kGlyphSortCount] = {"matter", "effect", "delivery", "mod",
                                          "operator"};

struct VerbName {
  const char* name;
  SpellVerb verb;
};
const VerbName kVerbNames[] = {
    {"none", SpellVerb::None},       {"spray", SpellVerb::Spray},
    {"place", SpellVerb::Place},     {"convert", SpellVerb::Convert},
    {"explode", SpellVerb::Explode}, {"wind", SpellVerb::Wind},
    {"mend", SpellVerb::Mend},       {"trail", SpellVerb::Trail},
    {"sustain", SpellVerb::Sustain}, {"filter", SpellVerb::Filter},
    {"repeat", SpellVerb::Repeat},
};

bool ParseModField(const std::string& s, ModField& out) {
  static const std::pair<const char*, ModField> k[] = {
      {"count", ModField::Count},       {"children", ModField::Children},
      {"gravity", ModField::Gravity},   {"speed", ModField::Speed},
      {"lifetime", ModField::Lifetime}, {"radius", ModField::Radius},
      {"bounces", ModField::Bounces},   {"pierce", ModField::Pierce},
      {"seek", ModField::Seek},         {"fuse", ModField::Fuse},
  };
  for (const auto& p : k)
    if (s == p.first) {
      out = p.second;
      return true;
    }
  return false;
}
const char* ModFieldName(ModField f) {
  switch (f) {
    case ModField::Count: return "count";
    case ModField::Children: return "children";
    case ModField::Gravity: return "gravity";
    case ModField::Speed: return "speed";
    case ModField::Lifetime: return "lifetime";
    case ModField::Radius: return "radius";
    case ModField::Bounces: return "bounces";
    case ModField::Pierce: return "pierce";
    case ModField::Seek: return "seek";
    case ModField::Fuse: return "fuse";
    default: return "none";
  }
}

// Parses a slot spec: "any" | ["matter", "effect", ...]. Returns false on an
// unknown sort name.
bool ParseSlot(const json& j, uint8_t& mask, std::string& err) {
  mask = 0;
  if (j.is_string()) {
    if (j.get<std::string>() == "any") {
      mask = kSortAny;
      return true;
    }
    GlyphSort s;
    if (!ParseGlyphSort(j.get<std::string>(), s)) {
      err = "unknown sort \"" + j.get<std::string>() + "\"";
      return false;
    }
    mask = (uint8_t)(1u << (int)s);
    return true;
  }
  if (!j.is_array()) {
    err = "slot must be \"any\" or a list of sorts";
    return false;
  }
  for (const json& e : j) {
    if (!e.is_string()) {
      err = "slot entries must be sort names";
      return false;
    }
    if (e.get<std::string>() == "any") {
      mask = kSortAny;
      continue;
    }
    GlyphSort s;
    if (!ParseGlyphSort(e.get<std::string>(), s)) {
      err = "unknown sort \"" + e.get<std::string>() + "\"";
      return false;
    }
    mask |= (uint8_t)(1u << (int)s);
  }
  return true;
}

bool ReadWind(const json& w, GlyphWind& gw, const SpellBudgets& b,
              std::string& err) {
  gw.has = true;
  const std::string k = w.value("kind", std::string("cone"));
  if (k == "cone") gw.kind = kWindPrimCone;
  else if (k == "burst") gw.kind = kWindPrimBurst;
  else if (k == "vortex") gw.kind = kWindPrimVortex;
  else {
    err = "unknown wind kind \"" + k + "\" (cone | burst | vortex)";
    return false;
  }
  // m/s, and the ceiling is the primitive system's own cap expressed in the
  // authoring unit. A negative speed is legal and useful: it turns a burst
  // into a vacuum and a cone into a draw.
  const float maxMs = (float)kWindPrimMaxSpeed * kVoxelMeters;
  gw.speedMs = (float)w.value("speed", 20.0);
  if (gw.speedMs > maxMs) gw.speedMs = maxMs;
  if (gw.speedMs < -maxMs) gw.speedMs = -maxMs;
  gw.radius = ClampI(w.value("radius", 6), 1, kWindPrimMaxExtent);
  gw.reach = ClampI(w.value("reach", 32), 1, kWindPrimMaxExtent);
  gw.ttlTicks = ClampI(w.value("ticks", 45), 1, b.maxLifetimeTicks);
  gw.swirl = (float)w.value("swirl", 0.0);
  gw.rise = (float)w.value("rise", 0.0);
  gw.entrain = w.value("entrain", false);
  return true;
}

std::string Times(int32_t n, BracketStyle style) {
  if (n <= 1) return std::string();
  return (style == BracketStyle::Oracle ? "\xC3\x97" : "x") + std::to_string(n);
}

}  // namespace

// ---- names -------------------------------------------------------------------

const char* GlyphSortName(GlyphSort s) {
  const int i = (int)s;
  return (i >= 0 && i < kGlyphSortCount) ? kSortNames[i] : "?";
}
bool ParseGlyphSort(const std::string& s, GlyphSort& out) {
  for (int i = 0; i < kGlyphSortCount; i++)
    if (s == kSortNames[i]) {
      out = (GlyphSort)i;
      return true;
    }
  return false;
}
const char* SpellVerbName(SpellVerb v) {
  for (const VerbName& n : kVerbNames)
    if (n.verb == v) return n.name;
  return "?";
}
bool ParseSpellVerb(const std::string& s, SpellVerb& out) {
  for (const VerbName& n : kVerbNames)
    if (s == n.name) {
      out = n.verb;
      return true;
    }
  return false;
}

// ---- loading ---------------------------------------------------------------

bool LoadGlyphs(const std::string& path, const std::vector<MaterialDef>& mats,
                GlyphLibrary& out, std::string& errors) {
  out = GlyphLibrary{};
  std::ifstream f(path);
  if (!f) {
    errors += "glyphs: cannot open " + path + "\n";
    return false;
  }
  json j;
  try {
    f >> j;
  } catch (const std::exception& e) {
    errors += std::string("glyphs: parse error: ") + e.what() + "\n";
    return false;
  }

  // Material NAME -> 12-bit id. Never hardcode a material id in spell code
  // (project convention): a glyph names "acid" and the id is whatever
  // materials.json order made it.
  auto resolveMat = [&](const std::string& name, uint32_t& outId) {
    for (size_t i = 0; i < mats.size(); i++) {
      if (mats[i].name == name) {
        outId = (uint32_t)i;
        return true;
      }
    }
    return false;
  };

  SpellBudgets& b = out.budgets;
  if (j.contains("budgets")) {
    const json& bj = j["budgets"];
    auto rd = [&](const char* k, int32_t& dst) {
      if (bj.contains(k) && bj[k].is_number_integer()) dst = bj[k].get<int32_t>();
    };
    rd("maxLiveProjectiles", b.maxLiveProjectiles);
    rd("maxGeneration", b.maxGeneration);
    rd("maxTrailVoxels", b.maxTrailVoxels);
    rd("maxLifetimeTicks", b.maxLifetimeTicks);
    rd("maxMultiplicity", b.maxMultiplicity);
    rd("maxInstances", b.maxInstances);
    rd("sprayVoxels", b.sprayVoxels);
    rd("maxSprayVoxels", b.maxSprayVoxels);
    rd("spraySpeed", b.spraySpeed);
    rd("sprayConeMille", b.sprayConeMille);
    rd("anythingSurchargeMille", b.anythingSurchargeMille);
    rd("maxStatusTicks", b.maxStatusTicks);
    rd("maxStatusPerCaster", b.maxStatusPerCaster);
    rd("maxGrimoirePages", b.maxGrimoirePages);
    rd("maxMacroWords", b.maxMacroWords);
    rd("maxMacroDepth", b.maxMacroDepth);
    if (bj.contains("rates") && bj["rates"].is_object()) {
      const json& r = bj["rates"];
      auto rr = [&](const char* k, int32_t& dst) {
        if (r.contains(k) && r[k].is_number_integer()) dst = r[k].get<int32_t>();
      };
      rr("place", b.ratePlace);
      rr("convert", b.rateConvert);
      rr("graft", b.rateGraft);
      rr("explode", b.rateExplode);
      rr("wind", b.rateWind);
    }
  }
  // Rule 2: these are hard engine ceilings, so a content edit cannot author an
  // unbounded spell. Clamped rather than trusted.
  b.maxLiveProjectiles = ClampI(b.maxLiveProjectiles, 1, 256);
  b.maxGeneration = ClampI(b.maxGeneration, 0, 8);
  b.maxTrailVoxels = ClampI(b.maxTrailVoxels, 1, 4096);
  b.maxLifetimeTicks = ClampI(b.maxLifetimeTicks, 1, 1800);
  b.maxMultiplicity = ClampI(b.maxMultiplicity, 1, 8);
  b.maxInstances = ClampI(b.maxInstances, 1, 81);
  b.sprayVoxels = ClampI(b.sprayVoxels, 1, 64);
  // kMaxParticleSpawnsPerTick is 4096 and it is SHARED with gore and debris
  // shatter, so a spell may not author a spray that could crowd them out.
  b.maxSprayVoxels = ClampI(b.maxSprayVoxels, 1, 512);
  b.spraySpeed = ClampI(b.spraySpeed, 1, 128);
  b.sprayConeMille = ClampI(b.sprayConeMille, 0, 1000);
  b.ratePlace = ClampI(b.ratePlace, 0, 1000);
  b.rateConvert = ClampI(b.rateConvert, 0, 1000);
  b.rateGraft = ClampI(b.rateGraft, 0, 1000);
  b.rateExplode = ClampI(b.rateExplode, 0, 1000);
  b.rateWind = ClampI(b.rateWind, 0, 1000);
  b.anythingSurchargeMille = ClampI(b.anythingSurchargeMille, 0, 10000);
  b.maxStatusTicks = ClampI(b.maxStatusTicks, 1, 18000);
  b.maxStatusPerCaster = ClampI(b.maxStatusPerCaster, 1, 32);
  b.maxGrimoirePages = ClampI(b.maxGrimoirePages, 1, 256);
  b.maxMacroWords = ClampI(b.maxMacroWords, 1, kSpellStackMax);
  b.maxMacroDepth = ClampI(b.maxMacroDepth, 1, 16);

  // The implicit delivery.
  out.hand.id = "hand";
  out.hand.sort = GlyphSort::Delivery;
  out.hand.mech = DeliveryMech::Instant;
  out.hand.word = 0;
  out.hand.carryMille = 1000;
  out.hand.reach = 3;
  out.hand.impactRadius = 1;
  out.hand.desc = "Implicit. At reach: a few voxels in front of the caster along the aim.";
  if (j.contains("hand") && j["hand"].is_object()) {
    const json& h = j["hand"];
    out.hand.carryMille = ClampI(h.value("carry", 1000), 0, 100000);
    out.hand.reach = ClampI(h.value("reach", 3), 0, 64);
    out.hand.impactRadius = ClampI(h.value("impactRadius", 1), 0, 8);
    if (h.contains("desc")) out.hand.desc = h["desc"].get<std::string>();
  }

  if (!j.contains("glyphs") || !j["glyphs"].is_array()) {
    errors += "glyphs: missing \"glyphs\" array\n";
    return false;
  }

  for (const json& g : j["glyphs"]) {
    GlyphDef d;
    if (!g.contains("id") || !g["id"].is_string()) {
      errors += "glyphs: a glyph has no string \"id\"\n";
      return false;
    }
    d.id = g["id"].get<std::string>();
    if (out.Find(d.id) >= 0 || d.id == "hand") {
      errors += "glyphs: duplicate glyph id \"" + d.id + "\"\n";
      return false;
    }
    const std::string where = "glyphs: \"" + d.id + "\" ";
    if (g.contains("desc")) d.desc = g["desc"].get<std::string>();
    if (g.contains("example")) d.example = g["example"].get<std::string>();
    if (g.contains("axis")) d.axis = g["axis"].get<std::string>();
    d.word = g.value("word", 0);
    if (d.word < 0) {
      errors += where + "has negative word cost\n";
      return false;
    }
    const std::string sort = g.value("sort", std::string());
    if (!ParseGlyphSort(sort, d.sort)) {
      errors += where + "has unknown sort \"" + sort +
                "\" (matter | effect | delivery | mod | operator)\n";
      return false;
    }
    d.repeat = d.sort == GlyphSort::Mod ? RepeatKind::Compose : RepeatKind::Add;
    if (g.contains("repeat")) {
      const std::string r = g["repeat"].get<std::string>();
      if (r == "add") d.repeat = RepeatKind::Add;
      else if (r == "compose") d.repeat = RepeatKind::Compose;
      else {
        errors += where + "has unknown repeat \"" + r + "\" (add | compose)\n";
        return false;
      }
    }
    std::string err;

    switch (d.sort) {
      case GlyphSort::Matter: {
        d.wildcard = g.value("wildcard", false);
        if (!d.wildcard) {
          d.materialName = g.value("material", std::string());
          if (!resolveMat(d.materialName, d.material)) {
            // Loud, not silent: a spell that quietly conjures air is far
            // harder to diagnose than one that refuses to load (DESIGN.md §6).
            errors += where + "names unknown material \"" + d.materialName + "\"\n";
            return false;
          }
        }
        break;
      }
      case GlyphSort::Effect:
      case GlyphSort::Operator: {
        const std::string v = g.value("verb", std::string());
        if (!ParseSpellVerb(v, d.verb) || d.verb == SpellVerb::None) {
          errors += where + "has unknown verb \"" + v + "\"\n";
          return false;
        }
        if (d.sort == GlyphSort::Operator) {
          if (g.contains("left")) {
            d.hasLeft = true;
            if (!ParseSlot(g["left"], d.leftMask, err)) {
              errors += where + "left slot: " + err + "\n";
              return false;
            }
          }
          if (g.contains("right")) {
            d.hasRight = true;
            if (!ParseSlot(g["right"], d.rightMask, err)) {
              errors += where + "right slot: " + err + "\n";
              return false;
            }
          }
          if (!d.hasLeft && !d.hasRight) {
            errors += where + "is an operator with no slot\n";
            return false;
          }
          const std::string res = g.value("result", std::string("effect"));
          if (!ParseGlyphSort(res, d.result) ||
              (d.result != GlyphSort::Effect && d.result != GlyphSort::Mod)) {
            errors += where + "has unknown result sort \"" + res + "\" (effect | mod)\n";
            return false;
          }
        }
        // Brush ops are capped at radius 8 by the mutate kernel's 16^3 footprint;
        // explosions by their own kernel.
        d.radius = ClampI(g.value("radius", d.verb == SpellVerb::Explode ? 6 : 1), 0,
                          d.verb == SpellVerb::Explode ? kMaxExplosionRadius : 8);
        d.power = ClampI(g.value("power", 220), 1, 100000);
        d.voxelBudget = ClampI(g.value("voxelBudget", 64), 1, b.maxTrailVoxels);
        d.everyTicks = ClampI(g.value("everyTicks", 1), 1, 60);
        d.ticks = ClampI(g.value("ticks", d.verb == SpellVerb::Sustain ? b.maxStatusTicks : 30),
                         1, b.maxStatusTicks);
        d.repeats = ClampI(g.value("repeats", 4), 1, 64);
        d.perTick = ClampI(g.value("perTick", 1), 1, 64);
        if (d.verb == SpellVerb::Place) {
          d.materialName = g.value("material", std::string());
          if (!resolveMat(d.materialName, d.material) || d.material == 0) {
            errors += where + "places unknown material \"" + d.materialName + "\"\n";
            return false;
          }
        }
        if (g.contains("wind")) {
          if (!ReadWind(g["wind"], d.wind, b, err)) {
            errors += where + err + "\n";
            return false;
          }
        } else if (d.verb == SpellVerb::Wind) {
          // A wind glyph with no wind block is a word that does nothing, which
          // the language's "every sequence does something" rule forbids.
          errors += where + "is a wind effect but has no \"wind\" block\n";
          return false;
        }
        break;
      }
      case GlyphSort::Delivery: {
        const std::string m = g.value("mech", std::string());
        if (m == "instant") d.mech = DeliveryMech::Instant;
        else if (m == "flight") d.mech = DeliveryMech::Flight;
        else if (m == "continuous") d.mech = DeliveryMech::Continuous;
        else {
          errors += where + "has unknown mech \"" + m + "\" (instant | flight | continuous)\n";
          return false;
        }
        d.carryMille = ClampI(g.value("carry", 1000), 0, 100000);
        d.speed = ClampI(g.value("speed", 48), 1, 512);
        d.lifetimeTicks = ClampI(g.value("lifetimeTicks", 150), 1, b.maxLifetimeTicks);
        d.impactRadius = ClampI(g.value("impactRadius", 3), 0, 8);
        d.gravityMille = ClampI(g.value("gravity", 0), -8000, 8000);
        d.fuseTicks = ClampI(g.value("fuse", 0), 0, b.maxLifetimeTicks);
        d.reach = ClampI(g.value("reach", 0), 0, 64);
        d.ticks = ClampI(g.value("ticks", 90), 1, b.maxStatusTicks);
        d.body = g.value("body", false);
        d.resolveOnExpiry = g.value("resolveOnExpiry", false);
        break;
      }
      case GlyphSort::Mod: {
        const std::string fld = g.value("field", std::string());
        if (!ParseModField(fld, d.field)) {
          errors += where + "edits unknown field \"" + fld + "\"\n";
          return false;
        }
        const std::string op = g.value("op", std::string("mul"));
        if (op == "mul") d.op = ModOp::Mul;
        else if (op == "div") d.op = ModOp::Div;
        else if (op == "add") d.op = ModOp::Add;
        else {
          errors += where + "has unknown op \"" + op + "\" (mul | div | add)\n";
          return false;
        }
        d.amount = g.value("amount", 1);
        if ((d.op == ModOp::Mul || d.op == ModOp::Div) && d.amount < 1) {
          errors += where + "mul/div amount must be >= 1\n";
          return false;
        }
        break;
      }
    }
    out.glyphs.push_back(std::move(d));
  }

  // conjoined: saved lists of glyph names (the grimoire's authored starters).
  if (j.contains("conjoined") && j["conjoined"].contains("entries")) {
    for (const json& c : j["conjoined"]["entries"]) {
      ConjoinedGlyph cg;
      cg.id = c.value("id", std::string());
      cg.desc = c.value("desc", std::string());
      bool ok = true;
      for (const json& gid : c.value("glyphs", json::array())) {
        int idx = out.Find(gid.get<std::string>());
        if (idx < 0) {
          errors += "glyphs: conjoined \"" + cg.id + "\" names unknown glyph \"" +
                    gid.get<std::string>() + "\"\n";
          ok = false;
          break;
        }
        cg.glyphs.push_back(idx);
      }
      if (!ok) return false;
      if ((int)cg.glyphs.size() > b.maxMacroWords) cg.glyphs.resize(b.maxMacroWords);
      out.conjoined.push_back(std::move(cg));
    }
  }

  if (out.glyphs.empty()) {
    errors += "glyphs: no glyphs loaded\n";
    return false;
  }
  out.arcane.resize(mats.size(), 0);
  for (size_t i = 0; i < mats.size(); i++) out.arcane[i] = mats[i].arcane;
  return true;
}

// ---- parse (R1–R4, R6) --------------------------------------------------------

GlyphSort NodeSort(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return GlyphSort::Effect;
  const SpellNode& n = t.nodes[node];
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return GlyphSort::Effect;
  return n.group ? g->result : g->sort;
}

namespace {

bool SlotAccepts(const GlyphLibrary& lib, const SpellTree& t, uint8_t mask, int node) {
  if (mask == kSortAny) return true;
  return (mask & (uint8_t)(1u << (int)NodeSort(lib, t, node))) != 0;
}

}  // namespace

SpellTree ParseSpell(const GlyphLibrary& lib, const SpellStack& stack) {
  SpellTree t;
  const int32_t cap = lib.budgets.maxMultiplicity;

  // R1: runs merge. N consecutive utterances of one glyph are ONE item ×N.
  std::vector<int> items;
  for (size_t i = 0; i < stack.spoken.size(); i++) {
    const int gi = stack.spoken[i];
    if (!lib.At(gi)) continue;
    if (!items.empty() && !t.nodes[items.back()].group &&
        t.nodes[items.back()].glyph == gi) {
      SpellNode& prev = t.nodes[items.back()];
      prev.n = std::min(prev.n + 1, cap);
      prev.last = (int)i;
      continue;
    }
    SpellNode n;
    n.glyph = gi;
    n.n = 1;
    n.first = n.last = (int)i;
    t.nodes.push_back(n);
    items.push_back((int)t.nodes.size() - 1);
  }

  // R2/R3: operators bind greedily on their declared side, left to right. An
  // operator whose required slot is empty is INCOMPLETE: it stays as a
  // charged fizzle of its result sort.
  std::vector<int> out;
  for (size_t i = 0; i < items.size(); i++) {
    const int ni = items[i];
    const GlyphDef& g = lib.glyphs[t.nodes[ni].glyph];
    if (t.nodes[ni].group || g.sort != GlyphSort::Operator) {
      out.push_back(ni);
      continue;
    }
    SpellNode grp;
    grp.glyph = t.nodes[ni].glyph;
    grp.n = t.nodes[ni].n;
    grp.group = true;
    grp.first = t.nodes[ni].first;
    grp.last = t.nodes[ni].last;
    grp.at = t.nodes[ni].first;
    if (g.hasLeft && !out.empty() && SlotAccepts(lib, t, g.leftMask, out.back())) {
      grp.left = out.back();
      out.pop_back();
      grp.first = std::min(grp.first, t.nodes[grp.left].first);
    }
    if (g.hasRight && i + 1 < items.size() &&
        SlotAccepts(lib, t, g.rightMask, items[i + 1])) {
      grp.right = items[i + 1];
      grp.last = std::max(grp.last, t.nodes[grp.right].last);
      i++;
    }
    grp.complete = (!g.hasLeft || grp.left >= 0) && (!g.hasRight || grp.right >= 0);
    t.nodes.push_back(grp);
    out.push_back((int)t.nodes.size() - 1);
  }

  // R4: a Delivery closes the clause. A trailing headless clause is `hand`.
  std::vector<std::vector<int>> clauses;
  std::vector<int> cur;
  for (int ni : out) {
    cur.push_back(ni);
    const SpellNode& n = t.nodes[ni];
    if (!n.group && lib.glyphs[n.glyph].sort == GlyphSort::Delivery) {
      clauses.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) clauses.push_back(cur);

  // R6: the bag is a set. Identical items merge (sum n, capped).
  for (const std::vector<int>& cl : clauses) {
    SpellClause c;
    std::vector<int> bag = cl;
    const SpellNode& tail = t.nodes[cl.back()];
    if (!tail.group && lib.glyphs[tail.glyph].sort == GlyphSort::Delivery) {
      c.delivery = tail.glyph;
      c.weight = tail.n;
      bag.pop_back();
    }
    std::vector<std::string> keys;
    for (int ni : bag) {
      const std::string k = NodeKey(lib, t, ni);
      bool merged = false;
      for (size_t j = 0; j < keys.size(); j++) {
        if (keys[j] == k) {
          SpellNode& first = t.nodes[c.bag[j]];
          first.n = std::min(first.n + t.nodes[ni].n, cap);
          merged = true;
          break;
        }
      }
      if (merged) continue;
      keys.push_back(k);
      c.bag.push_back(ni);
    }
    t.clauses.push_back(std::move(c));
  }
  return t;
}

std::string NodeKey(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return "?";
  if (!n.group) return g->id;
  return "(" + (n.left >= 0 ? NodeKey(lib, t, n.left) : std::string()) + "|" + g->id +
         "|" + (n.right >= 0 ? NodeKey(lib, t, n.right) : std::string()) + ")";
}

std::string ShowNode(const GlyphLibrary& lib, const SpellTree& t, int node,
                     BracketStyle style) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return "?";
  if (!n.group) return g->id + Times(n.n, style);
  const std::string l =
      n.left >= 0 ? ShowNode(lib, t, n.left, style) : (g->hasLeft ? "_" : "");
  const std::string r =
      n.right >= 0 ? ShowNode(lib, t, n.right, style) : (g->hasRight ? "_" : "");
  // Infix operators are drawn as a join; every unary one as "took the word on
  // its left".
  std::string sym;
  if (g->hasLeft && g->hasRight)
    sym = style == BracketStyle::Oracle ? "\xE2\x8B\x88" : "><";
  else
    sym = (style == BracketStyle::Oracle ? "\xE2\x97\x82" : "<") + g->id;
  std::string inner;
  const std::string* parts[3] = {&l, &sym, &r};
  for (const std::string* p : parts) {
    if (p->empty()) continue;
    if (!inner.empty()) inner += " ";
    inner += *p;
  }
  return "(" + inner + ")" + Times(n.n, style);
}

std::string BracketSpell(const GlyphLibrary& lib, const SpellTree& t,
                         BracketStyle style) {
  std::string out;
  for (size_t ci = 0; ci < t.clauses.size(); ci++) {
    const SpellClause& c = t.clauses[ci];
    std::string s;
    for (int ni : c.bag) {
      if (!s.empty()) s += " ";
      s += ShowNode(lib, t, ni, style);
    }
    if (c.delivery >= 0) {
      const std::string id = lib.glyphs[c.delivery].id;
      if (!s.empty()) s += " ";
      if (style == BracketStyle::Oracle) {
        s += "**" + id + Times(c.weight, style) + "**";
      } else {
        std::string up = id;
        for (char& ch : up) ch = (char)toupper((unsigned char)ch);
        s += up + Times(c.weight, style);
      }
    }
    if (s.empty()) s = "(empty)";
    if (!out.empty()) out += style == BracketStyle::Oracle ? " \xE2\x80\x96 " : " | ";
    out += s;
  }
  return out;
}

// ---- lowering (plan §5, §6) --------------------------------------------------

namespace {

int32_t WordCostOf(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0) return 0;
  const SpellNode& n = t.nodes[node];
  const GlyphDef* g = lib.At(n.glyph);
  int32_t c = g ? SatMul(g->word, n.n) : 0;
  if (n.group) c = SatAdd(c, SatAdd(WordCostOf(lib, t, n.left), WordCostOf(lib, t, n.right)));
  return c;
}

DeliveryRec RecordFor(const GlyphLibrary& lib, int deliveryGlyph, int32_t weight) {
  const GlyphDef& g = lib.Delivery(deliveryGlyph);
  const SpellBudgets& b = lib.budgets;
  DeliveryRec r;
  r.glyph = deliveryGlyph;
  r.mech = g.mech;
  r.weight = weight;
  r.carryMille = g.carryMille;
  // Delivery×N is WEIGHT: speed, lifetime and kinetic impact ×N. Count is
  // shotgun's job.
  r.speed = ClampI(SatMul(g.speed, weight), 1, 512);
  r.lifetimeTicks = ClampI(SatMul(g.lifetimeTicks, weight), 1, b.maxLifetimeTicks);
  r.impactRadius = ClampI(SatMul(g.impactRadius, weight), 0, 8);
  r.gravityMille = g.gravityMille;
  r.fuseTicks = g.fuseTicks;
  r.reach = g.mech == DeliveryMech::Continuous ? g.reach : g.reach;
  r.body = g.body;
  r.resolveOnExpiry = g.resolveOnExpiry;
  if (g.mech == DeliveryMech::Continuous) r.lifetimeTicks = ClampI(g.ticks, 1, b.maxStatusTicks);
  return r;
}

// A Mod applied N times: compose, not add.
void ApplyMod(DeliveryRec& r, const GlyphDef& g, int32_t n, const SpellBudgets& b) {
  auto edit = [&](int32_t v) -> int32_t {
    switch (g.op) {
      case ModOp::Mul: return SatMul(v, g.amount);
      case ModOp::Div: return v / (g.amount < 1 ? 1 : g.amount);
      case ModOp::Add: return SatAdd(v, g.amount);
    }
    return v;
  };
  for (int32_t k = 0; k < n; k++) {
    switch (g.field) {
      case ModField::Count: r.count = ClampI(edit(r.count), 1, b.maxInstances); break;
      case ModField::Children: r.children = ClampI(edit(r.children < 1 ? 1 : r.children), 1, 16); break;
      case ModField::Gravity: r.gravityMille = ClampI(edit(r.gravityMille), -8000, 8000); break;
      case ModField::Speed: r.speed = ClampI(edit(r.speed), 1, 512); break;
      case ModField::Lifetime:
        r.lifetimeTicks = ClampI(edit(r.lifetimeTicks), 1, b.maxLifetimeTicks);
        if (r.fuseTicks > 0) r.fuseTicks = ClampI(edit(r.fuseTicks), 0, b.maxLifetimeTicks);
        // Anchored: reach.
        r.reach = ClampI(edit(r.reach), 0, 64);
        break;
      case ModField::Radius: r.radiusMille = ClampI(edit(r.radiusMille), 125, 8000); break;
      case ModField::Bounces: r.bounces = ClampI(edit(r.bounces), 0, 16); break;
      case ModField::Pierce: r.pierce = ClampI(edit(r.pierce), 0, 16); break;
      case ModField::Seek: r.seek = ClampI(edit(r.seek), 0, 16); break;
      case ModField::Fuse: r.fuseTicks = ClampI(edit(r.fuseTicks), 0, b.maxLifetimeTicks); break;
      case ModField::None: break;
    }
  }
}

EffectInst LowerEffect(const GlyphLibrary& lib, const SpellTree& t, int node,
                       bool coerceMatterToPlace);

// A Matter word standing alone in a bag, or under an operator that wants an
// Effect, is coerced: spray(M) — or place(M) as a trail mark.
EffectInst LowerMatter(const GlyphLibrary& lib, const SpellTree& t, int node,
                       bool asPlace) {
  const SpellNode& n = t.nodes[node];
  const GlyphDef& g = lib.glyphs[n.glyph];
  EffectInst e;
  e.verb = asPlace ? SpellVerb::Place : SpellVerb::Spray;
  e.glyph = n.glyph;
  e.glyphA = n.glyph;
  e.n = n.n;
  e.matA = g.material;
  e.anyA = g.wildcard;
  e.radius = 0;
  e.node = node;
  return e;
}

EffectInst LowerEffect(const GlyphLibrary& lib, const SpellTree& t, int node,
                       bool coerceMatterToPlace) {
  const SpellNode& n = t.nodes[node];
  const GlyphDef& g = lib.glyphs[n.glyph];
  if (!n.group) {
    if (g.sort == GlyphSort::Matter) return LowerMatter(lib, t, node, coerceMatterToPlace);
    EffectInst e;
    e.verb = g.verb;
    e.glyph = n.glyph;
    e.n = n.n;
    e.node = node;
    e.radius = g.radius;
    if (g.verb == SpellVerb::Place) {
      e.matA = g.material;
      e.glyphA = n.glyph;
    }
    // A Mod where an Effect was wanted (a sustained mod's inner): carried as a
    // field edit.
    if (g.sort == GlyphSort::Mod) {
      e.verb = SpellVerb::None;
      e.modField = g.field;
      e.modOp = g.op;
      e.modAmount = g.amount;
      e.modN = n.n;
    }
    return e;
  }
  // An operator group. The verb is the operator's; the children are its
  // arguments.
  EffectInst e;
  e.verb = g.verb;
  e.glyph = n.glyph;
  e.n = n.n;
  e.node = node;
  e.complete = n.complete;
  e.radius = g.radius;
  auto matterOf = [&](int child, uint32_t& mat, bool& any, int& gl) {
    if (child < 0) return;
    const SpellNode& c = t.nodes[child];
    if (c.group) return;
    const GlyphDef& cg = lib.glyphs[c.glyph];
    if (cg.sort != GlyphSort::Matter) return;
    mat = cg.material;
    any = cg.wildcard;
    gl = c.glyph;
  };
  switch (g.verb) {
    case SpellVerb::Convert:
      matterOf(n.left, e.matA, e.anyA, e.glyphA);
      matterOf(n.right, e.matB, e.anyB, e.glyphB);
      // Repeating `transmute` widens the conversion: ×N VOLUME.
      e.radius = ClampI(ScaleRadiusCbrt(g.radius, n.n), 0, 8);
      break;
    case SpellVerb::Mend:
      matterOf(n.left, e.matA, e.anyA, e.glyphA);
      break;
    case SpellVerb::Trail:
    case SpellVerb::Repeat:
    case SpellVerb::Sustain:
    case SpellVerb::Filter: {
      const int child = n.left >= 0 ? n.left : n.right;
      if (child < 0) break;
      const GlyphSort cs = NodeSort(lib, t, child);
      if (g.verb == SpellVerb::Filter) {
        // `W null`: the word is taken RAW — what it names is what is refused.
        EffectInst w;
        w.verb = SpellVerb::None;
        w.glyph = t.nodes[child].glyph;
        w.node = child;
        w.n = t.nodes[child].n;
        e.inner.push_back(w);
        break;
      }
      if (cs == GlyphSort::Mod) {
        const SpellNode& c = t.nodes[child];
        if (!c.group) {
          const GlyphDef& cg = lib.glyphs[c.glyph];
          e.modField = cg.field;
          e.modOp = cg.op;
          e.modAmount = cg.amount;
          e.modN = c.n;
        } else {
          // A trail under aura: the body lays it along its own path.
          e.inner.push_back(LowerEffect(lib, t, child, true));
        }
      } else {
        e.inner.push_back(LowerEffect(lib, t, child, g.verb == SpellVerb::Trail));
      }
      break;
    }
    default:
      break;
  }
  return e;
}

void ScaleEffectRadius(EffectInst& e, int32_t radiusMille) {
  if (radiusMille == 1000) return;
  const int32_t cap = e.verb == SpellVerb::Explode ? kMaxExplosionRadius : 8;
  e.radius = ClampI((int32_t)(((int64_t)e.radius * radiusMille + 500) / 1000), 0, cap);
  for (EffectInst& i : e.inner) ScaleEffectRadius(i, radiusMille);
}

int32_t EffectTicks(const GlyphLibrary& lib, const EffectInst& e) {
  const GlyphDef* g = lib.At(e.glyph);
  int32_t ticks = 1;
  switch (e.verb) {
    case SpellVerb::Wind: ticks = g && g->wind.has ? g->wind.ttlTicks : 1; break;
    case SpellVerb::Sustain: ticks = g ? g->ticks : 1; break;
    case SpellVerb::Repeat: ticks = g ? SatMul(g->repeats, g->everyTicks) : 1; break;
    case SpellVerb::Filter: ticks = 1; break;
    default: break;
  }
  for (const EffectInst& i : e.inner) ticks = std::max(ticks, EffectTicks(lib, i));
  return ticks;
}

}  // namespace

int32_t EffectVolume(const GlyphLibrary& lib, const EffectInst& e) {
  const SpellBudgets& b = lib.budgets;
  const GlyphDef* g = lib.At(e.glyph);
  if (!e.complete) return 0;
  switch (e.verb) {
    case SpellVerb::Spray:
      if (e.matA == 0 && !e.anyA) return 0;
      return std::min(SatMul(b.sprayVoxels, e.n), b.maxSprayVoxels);
    case SpellVerb::Place: return SatMul(Cube(e.radius), e.n);
    case SpellVerb::Convert: return Cube(e.radius);
    case SpellVerb::Explode: return Cube(ClampI(ScaleRadiusCbrt(e.radius, e.n), 0, kMaxExplosionRadius));
    case SpellVerb::Wind: {
      if (!g || !g->wind.has) return 0;
      const int32_t r = g->wind.radius;
      return SatMul(SatMul(r, r), g->wind.reach);
    }
    case SpellVerb::Mend: return g ? SatMul(g->perTick, e.n) : e.n;
    case SpellVerb::Filter: return Cube(e.radius);
    case SpellVerb::Trail: {
      int32_t v = 0;
      for (const EffectInst& i : e.inner) v = SatAdd(v, EffectVolume(lib, i));
      return v;
    }
    case SpellVerb::Sustain: {
      int32_t v = 0;
      for (const EffectInst& i : e.inner) v = SatAdd(v, EffectVolume(lib, i));
      return v;
    }
    case SpellVerb::Repeat: {
      int32_t v = 0;
      for (const EffectInst& i : e.inner) v = SatAdd(v, EffectVolume(lib, i));
      return SatMul(v, g ? g->repeats : 1);
    }
    default: return 0;
  }
}

int32_t EffectTariff(const GlyphLibrary& lib, const EffectInst& e) {
  const SpellBudgets& b = lib.budgets;
  const GlyphDef* g = lib.At(e.glyph);
  if (!e.complete) return 0;
  const int32_t vol = EffectVolume(lib, e);
  switch (e.verb) {
    case SpellVerb::Spray:
    case SpellVerb::Place:
      // voxels x arcane(M) x rate.place. The wildcard is priced when it lands.
      if (e.anyA) return 0;
      return SatMul(vol, SatMul(lib.Arcane(e.matA), b.ratePlace));
    case SpellVerb::Convert: {
      // voxels x (rate.convert + max(0, arcane(B) - arcane(A))): going DOWN in
      // value costs the base only; going up costs the gap. Water -> gold over a
      // pool of thousands of voxels is the story the brief wants told.
      if (e.anyB) return 0;
      if (!e.anyA && e.matA == e.matB) return 0;
      const int32_t gap = e.anyA ? 0 : std::max(0, lib.Arcane(e.matB) - lib.Arcane(e.matA));
      return SatMul(vol, SatAdd(b.rateConvert, gap));
    }
    case SpellVerb::Explode: {
      // power x r^3 x rate.explode / 1000: the only superlinear curve per word,
      // because the WORLD effect is.
      const int32_t r = ClampI(ScaleRadiusCbrt(e.radius, e.n), 1, kMaxExplosionRadius);
      const int64_t power = (int64_t)(g ? g->power : 220) * e.n;
      const int64_t v = power * r * r * r * b.rateExplode / 1000;
      return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)std::max<int64_t>(v, 1);
    }
    case SpellVerb::Wind: {
      if (!g || !g->wind.has) return 0;
      const int64_t v = (int64_t)vol * g->wind.ttlTicks * e.n * b.rateWind / 1000;
      return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)std::max<int64_t>(v, 1);
    }
    case SpellVerb::Mend:
      if (e.anyA) return 0;
      return SatMul(vol, SatMul(lib.Arcane(e.matA), b.rateGraft));
    case SpellVerb::Filter: return std::max(1, vol / 1000);
    case SpellVerb::Trail: {
      int32_t t = 0;
      for (const EffectInst& i : e.inner) t = SatAdd(t, EffectTariff(lib, i));
      return t;
    }
    case SpellVerb::Sustain:
      // Billed per tick as it runs (P3), never up front.
      return 0;
    case SpellVerb::Repeat: {
      int32_t t = 0;
      for (const EffectInst& i : e.inner) t = SatAdd(t, EffectTariff(lib, i));
      return SatMul(t, g ? g->repeats : 1);
    }
    default: return 0;
  }
}

void PriceCast(const GlyphLibrary& lib, SpellCast& cast) {
  int32_t t = 0;
  for (const EffectInst& e : cast.payload) t = SatAdd(t, EffectTariff(lib, e));
  // A trail is priced as marks x the per-mark tariff of what it lays: the
  // budget is a voxel count, so the number of marks is budget / mark volume.
  if (!cast.delivery.trail.empty()) {
    int32_t markVol = 0, markTariff = 0;
    for (const EffectInst& e : cast.delivery.trail) {
      markVol = SatAdd(markVol, EffectVolume(lib, e));
      markTariff = SatAdd(markTariff, EffectTariff(lib, e));
    }
    const int32_t marks = cast.delivery.trailBudget / std::max(1, markVol);
    t = SatAdd(t, SatMul(marks, markTariff));
  }
  // A held beam pays the tariff every tick it is held (P3 bills it as it
  // emits); the up-front price is one resolve.
  cast.tariff = SatMul(t, cast.instances);
  // The delivery premium: carry is per-mille on the payload tariff, and the
  // part above x1 is what the HUD shows as "carry".
  const int64_t premium = (int64_t)cast.tariff * (cast.delivery.carryMille - 1000) / 1000;
  cast.carryCost = premium <= 0 ? 0 : (premium > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)premium);
}

namespace {
uint32_t TintOf(const std::vector<EffectInst>& es) {
  for (const EffectInst& e : es) {
    if (e.matB != 0 && !e.anyB) return e.matB;
    if (e.matA != 0 && !e.anyA) return e.matA;
    const uint32_t inner = TintOf(e.inner);
    if (inner != 0) return inner;
  }
  return 0;
}
}  // namespace

uint32_t CastTintMaterial(const SpellCast& cast) {
  const uint32_t p = TintOf(cast.payload);
  return p != 0 ? p : TintOf(cast.delivery.trail);
}

CastList LowerSpell(const GlyphLibrary& lib, const SpellTree& tree) {
  CastList list;
  list.tree = tree;
  const SpellBudgets& b = lib.budgets;
  for (size_t ci = 0; ci < tree.clauses.size(); ci++) {
    const SpellClause& c = tree.clauses[ci];
    SpellCast cast;
    cast.clause = (int)ci;
    cast.delivery = RecordFor(lib, c.delivery, c.weight);
    cast.wordCost = SatMul(lib.Delivery(c.delivery).word, c.weight);

    // R6 IN THE LOWERING: the bag is a set, so the record is built from it in
    // a CANONICAL order (by key), not the spoken one. Mods compose and integer
    // arithmetic does not commute under clamping (49 halved then doubled is
    // 48), so without this `swift slow` and `slow swift` would lower to two
    // different records and law L2 would be false.
    std::vector<int> bag = c.bag;
    std::stable_sort(bag.begin(), bag.end(), [&](int a, int bb) {
      return NodeKey(lib, tree, a) < NodeKey(lib, tree, bb);
    });

    // Mods first (they edit the record the effects then read), effects second.
    for (int ni : bag) {
      const SpellNode& n = tree.nodes[ni];
      cast.wordCost = SatAdd(cast.wordCost, WordCostOf(lib, tree, ni));
      if (NodeSort(lib, tree, ni) != GlyphSort::Mod) continue;
      const GlyphDef& g = lib.glyphs[n.glyph];
      if (!n.group) {
        ApplyMod(cast.delivery, g, n.n, b);
        continue;
      }
      // A trail group: E ◂ trail -> Δ. Runs its Effect at every marked voxel
      // of the flight path, under a hard voxel budget (rule 2).
      if (g.verb == SpellVerb::Trail && n.complete) {
        EffectInst tr = LowerEffect(lib, tree, ni, true);
        for (EffectInst& i : tr.inner) cast.delivery.trail.push_back(i);
        cast.delivery.trailBudget =
            std::min(SatAdd(cast.delivery.trailBudget, SatMul(g.voxelBudget, n.n)),
                     b.maxTrailVoxels);
        cast.delivery.trailEvery = g.everyTicks;
      }
    }
    for (int ni : bag) {
      const GlyphSort s = NodeSort(lib, tree, ni);
      if (s != GlyphSort::Effect && s != GlyphSort::Matter) continue;
      EffectInst e = LowerEffect(lib, tree, ni, false);
      ScaleEffectRadius(e, cast.delivery.radiusMille);
      if (e.anyA || e.anyB) cast.priceUnknown = true;
      for (const EffectInst& i : e.inner)
        if (i.anyA || i.anyB) cast.priceUnknown = true;
      cast.payload.push_back(std::move(e));
    }
    for (EffectInst& tr : cast.delivery.trail) {
      ScaleEffectRadius(tr, cast.delivery.radiusMille);
      if (tr.anyA) cast.priceUnknown = true;
    }

    cast.instances = ClampI(cast.delivery.count, 1, b.maxInstances);
    cast.generation = 0;
    PriceCast(lib, cast);
    // Rule 2 budgets, all finite.
    int32_t ticks = 1;
    switch (cast.delivery.mech) {
      case DeliveryMech::Instant: ticks = 1; break;
      case DeliveryMech::Flight:
        ticks = SatAdd(cast.delivery.lifetimeTicks, cast.delivery.fuseTicks);
        break;
      case DeliveryMech::Continuous: ticks = cast.delivery.lifetimeTicks; break;
    }
    int32_t voxels = 0;
    for (const EffectInst& e : cast.payload) {
      voxels = SatAdd(voxels, EffectVolume(lib, e));
      ticks = std::max(ticks, EffectTicks(lib, e));
    }
    voxels = SatAdd(voxels, cast.delivery.trailBudget);
    if (cast.delivery.mech == DeliveryMech::Continuous)
      voxels = SatMul(voxels, cast.delivery.lifetimeTicks);
    cast.voxels = SatMul(voxels, cast.instances);
    cast.ticks = ticks;

    list.wordCost = SatAdd(list.wordCost, cast.wordCost);
    list.tariff = SatAdd(list.tariff, cast.tariff);
    list.carryCost = SatAdd(list.carryCost, cast.carryCost);
    if (cast.priceUnknown) list.priceUnknown = true;
    list.casts.push_back(std::move(cast));
  }
  list.manaCost = SatAdd(SatAdd(list.wordCost, list.tariff), list.carryCost);
  return list;
}

CastList CompileSpell(const GlyphLibrary& lib, const SpellStack& stack) {
  return LowerSpell(lib, ParseSpell(lib, stack));
}

// ---- describe ----------------------------------------------------------------

namespace {

std::string MatName(const GlyphLibrary& lib, int glyph, uint32_t mat, bool any) {
  if (any) return "whatever is there";
  const GlyphDef* g = lib.At(glyph);
  if (g && !g->materialName.empty()) return g->materialName;
  if (mat == 0) return "nothing";
  return "material " + std::to_string(mat);
}

std::string DescribeEffect(const GlyphLibrary& lib, const EffectInst& e,
                           const std::string& at) {
  const GlyphDef* g = lib.At(e.glyph);
  const std::string id = g ? g->id : "?";
  auto xn = [&](const char* axis) {
    return e.n > 1 ? std::string(", ") + axis + " x" + std::to_string(e.n) : std::string();
  };
  if (!e.complete) return id + " with a missing word: fizzles, the word is charged";
  switch (e.verb) {
    case SpellVerb::Spray:
      if (e.anyA) return "scoops whatever is at " + at + " and throws it (priced when it lands)";
      if (e.matA == 0) return "throws nothing (charged the word)";
      return "throws " + std::to_string(EffectVolume(lib, e)) + " voxels of " +
             MatName(lib, e.glyphA, e.matA, false);
    case SpellVerb::Place:
      return "lays " + MatName(lib, e.glyphA, e.matA, e.anyA) + xn("volume");
    case SpellVerb::Convert: {
      const std::string a = MatName(lib, e.glyphA, e.matA, e.anyA);
      const std::string bb = MatName(lib, e.glyphB, e.matB, e.anyB);
      if (!e.anyA && !e.anyB && e.matA == e.matB) return "converts " + a + " to itself: no change, charged";
      if (!e.anyA && e.matA == 0) return "conjures " + bb + " into empty space" + xn("volume");
      if (!e.anyB && e.matB == 0) return "unmakes " + a + xn("volume");
      return "converts " + a + " into " + bb + xn("volume");
    }
    case SpellVerb::Explode: return "explosion at " + at + ", power x" + std::to_string(e.n);
    case SpellVerb::Wind: return "wind from " + at + " along the aim, speed x" + std::to_string(e.n);
    case SpellVerb::Mend:
      return "draws " + MatName(lib, e.glyphA, e.matA, e.anyA) + " at " + at +
             " into the caster's missing anatomy, " +
             std::to_string(g ? SatMul(g->perTick, e.n) : e.n) + " voxel(s)/tick";
    case SpellVerb::Sustain: {
      std::string what;
      if (e.modField != ModField::None)
        what = std::string("the mod ") + ModFieldName(e.modField) +
               (e.modN > 1 ? " x" + std::to_string(e.modN) : "");
      else if (!e.inner.empty())
        what = DescribeEffect(lib, e.inner[0], "the body");
      return "sustains on the body at " + at + ", every tick, billed per tick: " + what;
    }
    case SpellVerb::Filter: {
      const GlyphDef* w = e.inner.empty() ? nullptr : lib.At(e.inner[0].glyph);
      return "an anti-" + (w ? w->id : std::string("?")) + " field at " + at +
             ": incoming " + (w ? w->id : std::string("?")) + " ops are refused";
    }
    case SpellVerb::Repeat:
      return "repeats at " + at + " every few ticks, bounded: " +
             (e.inner.empty() ? std::string("nothing") : DescribeEffect(lib, e.inner[0], at));
    default: return id;
  }
}

}  // namespace

std::string DescribeCast(const GlyphLibrary& lib, const SpellCast& cast) {
  const DeliveryRec& d = cast.delivery;
  const GlyphDef& g = lib.Delivery(d.glyph);
  std::string head, at;
  switch (d.mech) {
    case DeliveryMech::Instant:
      if (d.glyph < 0) {
        head = "from the hand, at reach";
        at = "reach";
      } else {
        head = "on the caster (" + g.id + ")";
        at = "the caster";
      }
      break;
    case DeliveryMech::Flight:
      head = g.id + ": " + (d.body ? "a dropped body" : "a bolt") +
             (d.fuseTicks > 0 ? ", resolves when the fuse runs out" : ", resolves on impact");
      at = "the impact point";
      break;
    case DeliveryMech::Continuous:
      head = g.id + ": held, resolves at the ray hit every tick";
      at = "the beam hit";
      break;
  }
  if (d.weight > 1) head += ", weight x" + std::to_string(d.weight);
  std::string pay;
  for (const EffectInst& e : cast.payload) {
    if (!pay.empty()) pay += "; ";
    pay += DescribeEffect(lib, e, at);
  }
  if (pay.empty())
    pay = d.mech == DeliveryMech::Flight ? "empty payload: kinetic impact only"
                                          : "empty payload: nothing, the word is charged";
  std::string mods;
  auto add = [&](const std::string& s) {
    if (!mods.empty()) mods += "; ";
    mods += s;
  };
  if (d.count > 1) add(std::to_string(d.count) + " fanned instances (everything paid " + std::to_string(d.count) + "x)");
  if (d.children > 1) add("on resolve " + std::to_string(d.children) + " children, generation-capped");
  if (d.gravityMille != g.gravityMille)
    add("gravity " + std::to_string(d.gravityMille) + "/1000 g" +
        (d.mech == DeliveryMech::Instant ? " on the caster's body" : ""));
  if (d.speed != ClampI(SatMul(g.speed, d.weight), 1, 512)) add("speed " + std::to_string(d.speed) + " vox/tick");
  if (d.radiusMille != 1000) add("resolve radius x" + std::to_string(d.radiusMille) + "/1000");
  if (d.bounces > 0) add(std::to_string(d.bounces) + " bounce(s)");
  if (d.pierce > 0) add("passes through " + std::to_string(d.pierce));
  if (d.seek > 0) add("homing x" + std::to_string(d.seek));
  if (d.fuseTicks != g.fuseTicks) add("fuse " + std::to_string(d.fuseTicks) + " ticks");
  if (!d.trail.empty()) {
    std::string tr;
    for (const EffectInst& e : d.trail) {
      if (!tr.empty()) tr += "; ";
      tr += DescribeEffect(lib, e, "each marked voxel");
    }
    add("lays along the path (" + std::to_string(d.trailBudget) + " voxels): " + tr);
  }
  std::string out = head + " - " + pay;
  if (!mods.empty()) out += " - mods: " + mods;
  return out;
}

SpellReadout DescribeSpell(const GlyphLibrary& lib, const CastList& list) {
  SpellReadout r;
  r.wellFormed = !list.casts.empty();
  r.text = BracketSpell(lib, list.tree, BracketStyle::Hud);
  if (list.casts.empty()) {
    r.verdict = "silence";
    return r;
  }
  for (size_t i = 0; i < list.casts.size(); i++) {
    if (i) r.verdict += " | ";
    r.verdict += DescribeCast(lib, list.casts[i]);
  }
  return r;
}

// ---- caster ----------------------------------------------------------------

void CasterState::Tick() {
  const int32_t cap = EffectiveMax();
  if (mana >= cap) {
    // A reservation that just came up can leave mana above the new effective
    // cap; bleed it down rather than leaving the bar overfull.
    if (mana > cap) mana = cap;
    regenAccum = 0;
    return;
  }
  regenAccum += regenPerMillePerTick;
  while (regenAccum >= 1000 && mana < cap) {
    regenAccum -= 1000;
    mana++;
  }
  if (mana >= cap) regenAccum = 0;
}

CastResult ResolveCast(const CasterState& caster, int32_t health,
                       int32_t manaCost) {
  CastResult r;
  if (manaCost <= 0) {
    // A free spell is still a spell (a test library may price words at 0):
    // it casts normally and spends nothing. Only silence is Nothing, and
    // silence never reaches here — Cast() returns before resolving.
    r.outcome = CastOutcome::Normal;
    return r;
  }
  if (health < 0) health = 0;
  const int32_t mana = caster.mana < 0 ? 0 : caster.mana;

  if (manaCost <= mana) {
    r.outcome = CastOutcome::Normal;
    r.manaSpent = manaCost;
    return r;
  }
  const int32_t overflow = manaCost - mana;
  if (overflow <= health) {
    // Casting into health: the spell goes off, but IMPRECISELY. Instability is
    // how deep into health it went, as a fraction of the health available —
    // which is what makes the mana bar a PRECISION meter rather than a second
    // HP bar.
    r.outcome = CastOutcome::Unstable;
    r.manaSpent = mana;
    r.healthSpent = overflow;
    r.instability = health > 0 ? ClampI((int32_t)((int64_t)overflow * 1000 / health), 0, 1000) : 1000;
    return r;
  }
  // Beyond mana + health: the spell still HAPPENS, it just happens here.
  r.outcome = CastOutcome::Fatal;
  r.manaSpent = mana;
  r.healthSpent = health;
  r.instability = 1000;
  return r;
}

SpellFxVec SpellWobble(SpellFxVec dirFx, int32_t instabilityMille,
                       uint32_t tick, uint64_t casterId) {
  if (instabilityMille <= 0) return dirFx;
  // Squared so a shallow dip into health barely wavers while a deep one sprays
  // wildly — the interesting part of the curve is the far end.
  const int64_t mag = (int64_t)instabilityMille * instabilityMille / 1000;
  auto jitter = [&](int32_t component, uint32_t salt) {
    uint32_t h = Hash3((uint32_t)casterId ^ salt, tick, (uint32_t)component);
    int32_t signed15 = (int32_t)(h & 0xFFFFu) - 32768;
    int64_t amp = (int64_t)kSpellFxOne * 2 * mag / 1000;  // ~0.4 vox at 1000
    return (int32_t)((int64_t)signed15 * amp / 32768);
  };
  int64_t len2 = (int64_t)dirFx.x * dirFx.x + (int64_t)dirFx.y * dirFx.y +
                 (int64_t)dirFx.z * dirFx.z;
  const int32_t len = (int32_t)IntSqrt(len2);
  if (len <= 0) return dirFx;
  SpellFxVec o = dirFx;
  o.x += (int32_t)((int64_t)jitter(dirFx.x, 0x9E37u) * len / kSpellFxOne);
  o.y += (int32_t)((int64_t)jitter(dirFx.y, 0x85EBu) * len / kSpellFxOne);
  o.z += (int32_t)((int64_t)jitter(dirFx.z, 0xC2B2u) * len / kSpellFxOne);
  return o;
}

SpellFxVec SpellFan(SpellFxVec dirFx, int32_t i, int32_t count, uint32_t tick,
                    uint64_t casterId) {
  if (i <= 0 || count <= 1) return dirFx;
  int64_t len2 = (int64_t)dirFx.x * dirFx.x + (int64_t)dirFx.y * dirFx.y +
                 (int64_t)dirFx.z * dirFx.z;
  const int64_t len = IntSqrt(len2);
  if (len <= 0) return dirFx;
  // Lateral scatter of up to ~25% of the length per axis, counter-based so
  // instance i of a fan lands the same way in a replay.
  auto jit = [&](uint32_t salt) {
    uint32_t h = Hash3((uint32_t)casterId ^ salt, tick, (uint32_t)i * 7919u + (uint32_t)count);
    return (int64_t)(int32_t)(h & 0xFFFFu) - 32768;  // [-32768, 32768)
  };
  SpellFxVec o = dirFx;
  o.x += (int32_t)(jit(0x9E37u) * len / 32768 / 4);
  o.y += (int32_t)(jit(0x85EBu) * len / 32768 / 4);
  o.z += (int32_t)(jit(0xC2B2u) * len / 32768 / 4);
  return o;
}

// ---- the world probe ----------------------------------------------------------

namespace {

uint32_t SnapMatAt(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) {
  const World& world = *(const World*)ctx;
  const WorldSnapshot& s = world.Snap();
  known = false;
  if (!s.valid) return 0;
  const int cx = (x >> 4) - s.mirrorBase.x;
  const int cy = (y >> 4) - s.mirrorBase.y;
  const int cz = (z >> 4) - s.mirrorBase.z;
  if (cx < 0 || cy < 0 || cz < 0 || cx >= 3 || cy >= 3 || cz >= 3) return 0;
  const int lx = x & 15, ly = y & 15, lz = z & 15;
  const size_t idx = (size_t)((cz * 3 + cy) * 3 + cx) * kChunkVol +
                     (size_t)((lz * (int)kChunk + ly) * (int)kChunk + lx);
  if (idx >= s.mirror.size()) return 0;
  known = true;
  return s.mirror[idx] & 0xFFFu;
}

}  // namespace

SpellProbe WorldSpellProbe(const World& world) {
  SpellProbe p;
  p.matAt = SnapMatAt;
  p.ctx = (void*)&world;
  return p;
}

// ---- the effect payload (thesis 2) -----------------------------------------

void ApplySpellEffect(const GlyphLibrary& lib, const std::vector<EffectInst>& payload,
                      SpellFxVec atFx, SpellFxVec dirFx, int32_t strengthMille,
                      SpellEmission& out, const SpellProbe* probe,
                      int32_t instabilityMille, uint32_t salt) {
  strengthMille = ClampI(strengthMille, 0, 1000);
  if (strengthMille == 0) return;
  const SpellBudgets& b = lib.budgets;

  const int32_t cx = SpellFxFloor(atFx.x);
  const int32_t cy = SpellFxFloor(atFx.y);
  const int32_t cz = SpellFxFloor(atFx.z);

  // Scale a radius by strength, never below 1 when the effect fires at all —
  // a 0-radius brush op is a wasted slot in the tick budget.
  auto scaled = [&](int32_t r) {
    int32_t v = (int32_t)((int64_t)r * strengthMille / 1000);
    return v < 1 ? 1 : v;
  };
  auto matHere = [&](bool& known) -> uint32_t {
    known = false;
    if (!probe || !probe->matAt) return 0;
    return probe->matAt(probe->ctx, cx, cy, cz, known);
  };
  const uint32_t here = Hash3((uint32_t)atFx.x, (uint32_t)atFx.z ^ salt, (uint32_t)atFx.y);

  for (size_t ei = 0; ei < payload.size(); ei++) {
    const EffectInst& e = payload[ei];
    if (!e.complete) continue;   // R3: charged, does nothing
    const GlyphDef* g = lib.At(e.glyph);
    switch (e.verb) {
      case SpellVerb::Spray: {
        // SPRAY — loose voxels of the element flung along `dirFx` as ballistic
        // particles: the existing gore/ejecta pipeline (DESIGN.md §5). They fly,
        // collide, and reinsert into the grid on landing.
        uint32_t mat = e.matA;
        if (e.anyA) {
          bool known;
          mat = matHere(known);
        }
        if (mat == 0) break;
        int64_t n = (int64_t)b.sprayVoxels * e.n;
        n = n * strengthMille / 1000;
        if (n > b.maxSprayVoxels) n = b.maxSprayVoxels;
        int64_t len2 = (int64_t)dirFx.x * dirFx.x + (int64_t)dirFx.y * dirFx.y +
                       (int64_t)dirFx.z * dirFx.z;
        int64_t len = IntSqrt(len2);
        if (len <= 0) len = 1;
        const int64_t sp = (int64_t)b.spraySpeed * kSpellFxOne / 30;  // per tick
        const int64_t cone = sp * b.sprayConeMille / 1000;
        for (int64_t i = 0; i < n; i++) {
          // Counter-based scatter: same tick + same index => same direction on
          // every machine, so a replay reproduces the spray exactly.
          uint32_t h = Hash3(mat * 2654435761u, here, (uint32_t)i);
          auto jit = [&](uint32_t s) {
            uint32_t v = Pcg(h ^ s);
            return (int64_t)(int32_t)(v & 0xFFFFu) - 32768;
          };
          ParticleSpawn s{};
          s.px = atFx.x;
          s.py = atFx.y;
          s.pz = atFx.z;
          s.vx = (int32_t)((int64_t)dirFx.x * sp / len + jit(0x9E37u) * cone / 32768);
          s.vy = (int32_t)((int64_t)dirFx.y * sp / len + jit(0x85EBu) * cone / 32768);
          s.vz = (int32_t)((int64_t)dirFx.z * sp / len + jit(0xC2B2u) * cone / 32768);
          // A FULL-SIZE particle, not a micro droplet: real voxels of matter
          // that must land in the grid and stay there.
          s.payload = mat & 0xFFFu;
          s.flags = kPFlagAlive;
          out.spawns.push_back(s);
        }
        if (e.anyA) {
          // THE WILDCARD IS PRICED WHEN IT LANDS: what it turned out to be, at
          // the place rate, PLUS its value times the surcharge.
          const int32_t val = lib.Arcane(mat);
          out.billOnResolve = SatAdd(
              out.billOnResolve,
              SatMul((int32_t)n, SatAdd(SatMul(val, b.ratePlace),
                                        (int32_t)((int64_t)val * b.anythingSurchargeMille / 1000))));
        }
        break;
      }
      case SpellVerb::Place: {
        uint32_t mat = e.matA;
        if (e.anyA) {
          bool known;
          mat = matHere(known);
          if (mat != 0) {
            const int32_t val = lib.Arcane(mat);
            const int32_t vox = SatMul(Cube(e.radius), e.n);
            out.billOnResolve = SatAdd(
                out.billOnResolve,
                SatMul(vox, SatAdd(SatMul(val, b.ratePlace),
                                   (int32_t)((int64_t)val * b.anythingSurchargeMille / 1000))));
          }
        }
        if (mat == 0) break;
        const int32_t r = ClampI(ScaleRadiusCbrt(e.radius, e.n), 0, 8);
        out.ops.push_back({cx, cy, cz, e.radius > 0 ? ClampI(scaled(r), 1, 8) : 0, mat,
                           0u /*paint into air*/, 0, 0});
        break;
      }
      case SpellVerb::Convert: {
        // A transmute B: an OVERWRITE brush op (mode 1) with a FROM filter in
        // pad0 (0 = any), NOT the laser's melt mode (2). Melt converts each cell
        // to its own authored heat product, which is right for a heat beam and
        // wrong for "transmute to acid", where the caster chose the target. The
        // melt mode is used for exactly one thing here: the share of an
        // UNSTABLE convert that goes wrong (plan §4).
        if (e.anyB) break;   // "into whatever is beside it": undefined, charged
        if (!e.anyA && e.matA == e.matB) break;   // to itself: no change, charged
        const int32_t r = ClampI(scaled(e.radius), 0, 8);
        BrushOp op{cx, cy, cz, r, e.matB, 1u, 0, 0};
        if (!e.anyA && e.matA == 0) {
          // air ⋈ B conjures B into empty space: paint into air only.
          op.mode = 0u;
        } else {
          op.pad0 = e.anyA ? 0u : e.matA;   // the from filter
          if (e.anyA) op.pad1 |= 1u;         // the wildcard matches matter, not the void
        }
        // Imprecision degrades the PRODUCT, not just the aim: a share of an
        // unstable convert's ops proportional to instability lands in melt mode,
        // so the water you meant to gild boils instead.
        if (op.mode == 1u && instabilityMille > 0) {
          const uint32_t roll = Hash3(here, 0x4D454C54u, (uint32_t)ei) % 1000u;
          if ((int32_t)roll < instabilityMille) {
            op.mode = 2u;
            op.material = 0;
          }
        }
        if (e.anyA) {
          // Billed on resolve: the conversion from what is actually there (the
          // centre cell stands for the volume), plus the wildcard's surcharge on
          // that matter's value. An `anything transmute gold` into a gold vein
          // is cheap; into a lake it bills the water->gold gap.
          bool known;
          const uint32_t actual = matHere(known);
          if (actual != 0) {
            const int32_t val = lib.Arcane(actual);
            const int32_t gap = std::max(0, lib.Arcane(e.matB) - val);
            const int32_t vox = Cube(r);
            out.billOnResolve = SatAdd(
                out.billOnResolve,
                SatMul(vox, SatAdd(SatAdd(b.rateConvert, gap),
                                   (int32_t)((int64_t)val * b.anythingSurchargeMille / 1000))));
          }
        }
        out.ops.push_back(op);
        break;
      }
      case SpellVerb::Explode: {
        // Repeating `explosive` scales POWER ×N; the engine's falloff law turns
        // that into reach. The op's radius bound grows as the cube root so the
        // bound does not clip what the power would have reached.
        const int32_t r0 = g ? g->radius : e.radius;
        const int32_t r = ClampI(scaled(ScaleRadiusCbrt(std::max(r0, e.radius), e.n)), 1,
                                 kMaxExplosionRadius);
        const int32_t power = (int32_t)((int64_t)(g ? g->power : 220) * e.n * strengthMille / 1000);
        out.explosions.push_back({cx, cy, cz, r, std::max(power, 1), 0, 0, 0});
        break;
      }
      case SpellVerb::Wind: {
        // The glyph that changes the AIR (docs/RESEARCH_wind.md §4.3). No spell
        // code knows what sand is, and no CA rule knows what a spell is — they
        // meet at a 48-byte parametric object.
        if (!g || !g->wind.has) break;
        Vec3 dir{SpellFxToFloat(dirFx.x), SpellFxToFloat(dirFx.y), SpellFxToFloat(dirFx.z)};
        // Repetition buys SPEED rather than size: widening the footprint would
        // multiply the chunk-wake cost eightfold for one extra word.
        const float speed = g->wind.speedMs * (float)e.n * ((float)strengthMille / 1000.0f);
        WindPrim p{};
        p.x = cx;
        p.y = cy;
        p.z = cz;
        p.kind = g->wind.kind;
        p.radius = g->wind.radius;
        p.reach = g->wind.reach;
        p.ttl = (uint32_t)g->wind.ttlTicks;
        p.swirlQ = (int32_t)(g->wind.swirl * 65536.0f);
        p.riseQ = (int32_t)(g->wind.rise * 65536.0f);
        p.flags = kWindPrimAir;
        if (g->wind.entrain) p.flags |= kWindPrimEntrain;
        WindPrimAim(p, dir, speed);
        out.winds.push_back(p);
        break;
      }
      case SpellVerb::Repeat: {
        // The inner effect, again, a bounded number of times. The lowering
        // charged repeats × the effect; this tick runs the first one. (P3
        // schedules the rest.)
        ApplySpellEffect(lib, e.inner, atFx, dirFx, strengthMille, out, probe,
                         instabilityMille, salt ^ 0x5EC0u);
        break;
      }
      case SpellVerb::Mend:
      case SpellVerb::Sustain:
      case SpellVerb::Filter:
      case SpellVerb::Trail:
      case SpellVerb::None:
        // Sustained / body verbs land in P3 and P4. Charged, nothing yet.
        break;
    }
  }
}

// ---- the system ------------------------------------------------------------

namespace {

SpellFxVec Unit(SpellFxVec v, int64_t scale) {
  const int64_t rr = IntSqrt((int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z);
  if (rr <= 0) return {0, 0, 0};
  return {(int32_t)((int64_t)v.x * scale / rr), (int32_t)((int64_t)v.y * scale / rr),
          (int32_t)((int64_t)v.z * scale / rr)};
}

}  // namespace

void SpellSystem::Launch(const SpellCast& cast, SpellFxVec originFx, SpellFxVec aim,
                         uint64_t casterId, uint32_t tick, int32_t instance,
                         int32_t instability, SpellEmission& out,
                         const SpellProbe* probe) {
  (void)instance;
  (void)tick;
  // Rule 2: a hard cap on live projectiles, checked before the spawn. Over
  // budget, the spell misfires at the caster rather than silently doing
  // nothing — a spell that sometimes doesn't fire is miserable to diagnose.
  if ((int)live_.size() >= lib_->budgets.maxLiveProjectiles ||
      cast.generation > lib_->budgets.maxGeneration) {
    ApplySpellEffect(*lib_, cast.payload, originFx, aim, 400, out, probe);
    return;
  }
  SpellProjectile p;
  p.cast = cast;
  p.pos = originFx;
  p.casterId = casterId;
  p.gen = cast.generation;
  p.instability = instability;
  p.ticksLeft = ClampI(cast.delivery.lifetimeTicks, 1, lib_->budgets.maxLifetimeTicks);
  // Normalize the aim to the record's speed, in fixed point. Integer sqrt so
  // two machines agree exactly (no libm, no float).
  p.vel = Unit(aim, (int64_t)cast.delivery.speed * kSpellFxOne);
  // Trail budget: a HARD voxel count that only ever decreases (rule 2).
  p.trailBudget = cast.delivery.trail.empty() ? 0 : cast.delivery.trailBudget;
  p.bouncesLeft = cast.delivery.bounces;
  p.pierceLeft = cast.delivery.pierce;
  live_.push_back(std::move(p));
}

CastResult SpellSystem::Cast(const CastList& list, CasterState& caster,
                             const CasterHealth& health, uint64_t casterId,
                             SpellFxVec originFx, SpellFxVec dirFx,
                             uint32_t tick, SpellEmission& out,
                             const SpellProbe* probe, const SpellFxVec* selfAt) {
  CastResult r;
  if (!lib_) return r;
  // The ONLY thing that is not a spell is silence.
  if (list.casts.empty()) return r;

  const int32_t hp = health.Get();
  r = ResolveCast(caster, hp, list.manaCost);

  caster.mana -= r.manaSpent;
  if (caster.mana < 0) caster.mana = 0;
  if (r.healthSpent > 0) health.Spend(r.healthSpent);

  // THESIS 2 IN ONE BRANCH. A fatal cast is not special-cased: it is the same
  // effect payload, applied at the caster instead of at the muzzle.
  if (r.outcome == CastOutcome::Fatal) {
    int32_t carve = 2;
    for (const SpellCast& c : list.casts) {
      ApplySpellEffect(*lib_, c.payload, originFx, dirFx, 1000, out, probe, 1000);
      // The trail too: a fatal trail bolt lays its wake in the caster.
      if (!c.delivery.trail.empty())
        ApplySpellEffect(*lib_, c.delivery.trail, originFx, dirFx, 1000, out, probe, 1000);
      carve = std::max(carve, c.delivery.impactRadius);
      for (const EffectInst& e : c.payload) carve = std::max(carve, e.radius);
    }
    out.carveCaster = true;
    out.carveAt = Vec3{SpellFxToFloat(originFx.x), SpellFxToFloat(originFx.y),
                       SpellFxToFloat(originFx.z)};
    out.carveRadius = (float)std::min(carve, 8);
    return r;
  }

  // Instability makes the spell IMPRECISE, not merely costly.
  const SpellFxVec aim = SpellWobble(dirFx, r.instability, tick, casterId);

  for (size_t ci = 0; ci < list.casts.size(); ci++) {
    const SpellCast& c = list.casts[ci];
    const int32_t inst = ClampI(c.instances, 1, lib_->budgets.maxInstances);
    for (int32_t i = 0; i < inst; i++) {
      const SpellFxVec d = SpellFan(aim, i, inst, tick + (uint32_t)ci * 131u, casterId);
      switch (c.delivery.mech) {
        case DeliveryMech::Instant: {
          SpellFxVec at = originFx;
          if (c.delivery.glyph < 0) {
            // hand: at reach, along the aim.
            const SpellFxVec u = Unit(d, (int64_t)c.delivery.reach * kSpellFxOne);
            at = {originFx.x + u.x, originFx.y + u.y, originFx.z + u.z};
          } else if (selfAt) {
            at = *selfAt;
          }
          // Fanned resolve points around the anchor for a shotgun on a
          // body-anchored delivery: instance i lands a voxel or two off.
          if (i > 0) {
            const SpellFxVec u = Unit(SpellFan({kSpellFxOne, 0, 0}, i, inst, tick, casterId),
                                      2 * kSpellFxOne);
            at = {at.x + u.x, at.y + u.y, at.z + u.z};
          }
          ApplySpellEffect(*lib_, c.payload, at, d, 1000, out, probe, r.instability,
                           (uint32_t)i);
          break;
        }
        case DeliveryMech::Flight:
          Launch(c, originFx, d, casterId, tick, i, r.instability, out, probe);
          break;
        case DeliveryMech::Continuous: {
          // Until the held beam lands (P3): one resolve at the ray's reach.
          const SpellFxVec u = Unit(d, (int64_t)std::max(c.delivery.reach, 1) * kSpellFxOne);
          const SpellFxVec at{originFx.x + u.x, originFx.y + u.y, originFx.z + u.z};
          ApplySpellEffect(*lib_, c.payload, at, d, 1000, out, probe, r.instability,
                           (uint32_t)i);
          break;
        }
      }
    }
  }
  return r;
}

void SpellSystem::Tick(uint32_t tick, const World& world,
                       const std::vector<uint32_t>& classOf,
                       SpellEmission& out) {
  opsDropped_ = 0;
  if (!lib_) return;
  int opsUsed = 0;
  const SpellProbe probe = WorldSpellProbe(world);

  auto solidAt = [&](int32_t x, int32_t y, int32_t z) {
    // OUT OF WINDOW = SOLID. The residency-window rule (DESIGN.md §3):
    // unloaded space is solid and inert, so a projectile leaving the simulated
    // world stops at the boundary rather than flying forever through nothing.
    if (!world.CellInWindow({(int)x, (int)y, (int)z})) return true;
    CellKind k = world.KindAt({(int)x, (int)y, (int)z}, classOf);
    // UNKNOWN = PASSABLE, the opposite of the player controller's choice. The
    // CPU mirror is only the 3x3x3 chunks around the PLAYER (~48 voxels), so a
    // 48 vox/tick projectile exits it inside one tick; reading Unknown as solid
    // detonated every bolt in the caster's face.
    return k == CellKind::Solid;
  };

  for (size_t i = 0; i < live_.size();) {
    SpellProjectile& p = live_[i];
    bool impact = false;
    SpellFxVec impactAt = p.pos;

    if (--p.ticksLeft <= 0) {
      // Lifetime expiry is a HARD bound (rule 2). An expired bolt fizzles —
      // unless the delivery says its life running out IS its resolve (orb).
      if (p.cast.delivery.resolveOnExpiry) impact = true;
      p.alive = false;
    } else {
      // Gravity, per the record.
      if (p.cast.delivery.gravityMille != 0)
        p.vel.y -= (int32_t)((int64_t)kGravityFxPerTick2 * p.cast.delivery.gravityMille / 1000);
      // Swept integration with anti-tunneling: step at most half a voxel at a
      // time, exactly as sim_particle.wgsl does.
      const int32_t kHalf = kSpellFxOne / 2;
      int64_t maxComp = std::max({(int64_t)std::abs(p.vel.x),
                                  (int64_t)std::abs(p.vel.y),
                                  (int64_t)std::abs(p.vel.z)});
      int32_t steps = (int32_t)((maxComp + kHalf - 1) / kHalf);
      if (steps < 1) steps = 1;
      if (steps > 512) steps = 512;  // bound the inner loop unconditionally

      for (int32_t s = 0; s < steps && !impact; s++) {
        SpellFxVec next = p.pos;
        next.x += p.vel.x / steps;
        next.y += p.vel.y / steps;
        next.z += p.vel.z / steps;
        const int32_t cx = SpellFxFloor(next.x), cy = SpellFxFloor(next.y),
                      cz = SpellFxFloor(next.z);
        if (solidAt(cx, cy, cz)) {
          impact = true;
          impactAt = next;
          break;
        }
        p.pos = next;

        // Trail: run the trail effects at each WHOLE VOXEL crossed, charging a
        // hard budget BEFORE emitting and refusing when it does not fit.
        if (p.trailBudget > 0 && !p.cast.delivery.trail.empty()) {
          const int32_t mx = SpellFxFloor(p.pos.x), my = SpellFxFloor(p.pos.y),
                        mz = SpellFxFloor(p.pos.z);
          if (p.markedValid && mx == p.markedX && my == p.markedY && mz == p.markedZ)
            continue;
          if (!world.CellInWindow({(int)mx, (int)my, (int)mz})) continue;
          const int32_t every = std::max(1, p.cast.delivery.trailEvery);
          if ((p.trailPhase++ % every) != 0) continue;
          if (opsUsed >= kSpellOpsPerTick) {
            opsDropped_++;
            continue;
          }
          int32_t vol = 0;
          for (const EffectInst& e : p.cast.delivery.trail)
            vol = SatAdd(vol, EffectVolume(*lib_, e));
          if (vol < 1) vol = 1;
          if (vol > p.trailBudget) {
            // Cannot afford another mark: the trail is the spell's whole
            // point, so the projectile dies with its budget rather than
            // flying on inertly.
            p.trailBudget = 0;
            p.alive = false;
            break;
          }
          p.trailBudget -= vol;
          const size_t before = out.ops.size();
          const SpellFxVec markAt{mx * kSpellFxOne + kSpellFxOne / 2,
                                  my * kSpellFxOne + kSpellFxOne / 2,
                                  mz * kSpellFxOne + kSpellFxOne / 2};
          ApplySpellEffect(*lib_, p.cast.delivery.trail, markAt, p.vel, 1000, out, &probe, 0,
                           (uint32_t)p.trailPhase);
          opsUsed += (int)(out.ops.size() - before);
          p.markedX = mx;
          p.markedY = my;
          p.markedZ = mz;
          p.markedValid = true;
          if (p.trailBudget <= 0) p.alive = false;
        }
        if (!p.alive) break;
      }
    }

    if (impact) {
      // Impact runs the payload through the SAME function backfire uses
      // (thesis 2), at the impact position.
      if (opsUsed < kSpellOpsPerTick) {
        size_t before = out.ops.size();
        ApplySpellEffect(*lib_, p.cast.payload, impactAt, p.vel, 1000, out, &probe,
                         p.instability, (uint32_t)tick);
        opsUsed += (int)(out.ops.size() - before);
      } else {
        opsDropped_++;
      }
      p.alive = false;
    }

    if (!p.alive) {
      live_[i] = live_.back();
      live_.pop_back();
    } else {
      i++;
    }
  }
}
