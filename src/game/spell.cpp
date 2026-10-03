#include "game/spell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include <nlohmann/json.hpp>

#include "game/lightning.h"
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

// The same growth for a FRACTIONAL scale (PLAN_spell_magnitude §2): `sMille`
// is n × magnitude, per-mille. A whole multiple of 1000 goes through the table
// above, so every word without a magnitude keeps its exact old radius (law
// L-M0); anything else is round(cbrt(s)) per-mille by integer search — no
// float in an authoritative number (thesis 3).
int32_t ScaleRadiusCbrtMille(int32_t r, int32_t sMille) {
  if (sMille % 1000 == 0) return ScaleRadiusCbrt(r, sMille / 1000);
  if (sMille > 8000) sMille = 8000;
  if (sMille < 1) sMille = 1;
  // c (per-mille) with c³ closest to sMille × 10⁶.
  const int64_t want = (int64_t)sMille * 1000000;
  int64_t lo = 0, hi = 2001;
  while (hi - lo > 1) {
    const int64_t mid = (lo + hi) / 2;
    if (mid * mid * mid <= want) lo = mid;
    else hi = mid;
  }
  const int64_t c = (want - lo * lo * lo) <= (hi * hi * hi - want) ? lo : hi;
  return (int32_t)(((int64_t)r * c + 500) / 1000);
}

// v × sMille / 1000, rounded toward zero, saturating — and at least `floor1`
// when v > 0 (a quarter of three sprayed voxels is still one voxel, not none).
int32_t ScaleMille(int32_t v, int32_t sMille, int32_t floor1 = 0);

// `echo`'s repeat count at its magnitude: the authored repeats at 1, scaled
// (and at least one) otherwise. Multiplicity is NOT in it - `echo echo` was
// never twice the repeats, and must not start being so.
int32_t EchoRepeats(const GlyphDef* g, const EffectInst& e) {
  if (!g) return 1;
  if (e.mag == kMagOne) return g->repeats;
  return std::max<int32_t>(1, (int32_t)((int64_t)g->repeats * e.mag / 1000));
}

// HOW MANY TIMES an item fires in a carrier (M3): once on hit, launch or
// expiry; once per bounce; once per `every` period of the carrier's life. A
// carrier that cannot produce the event fires it NEVER (a bomb has no bounce,
// a hand has no life), and an item that never fires costs only its word.
// Anything but a flight fires every item once, at its resolve.
int32_t FiresIn(const DeliveryRec& d, const EffectInst& e) {
  if (d.mech != DeliveryMech::Flight) return 1;
  switch (e.timing.trigger) {
    case SpellTrigger::Hit:
    case SpellTrigger::Launch: return 1;
    case SpellTrigger::Bounce: return d.body ? 0 : d.bounces;
    case SpellTrigger::Expire: return d.body ? 0 : 1;
    case SpellTrigger::Every:
      return e.timing.every > 0 ? std::max<int32_t>(1, d.lifetimeTicks / e.timing.every) : 1;
  }
  return 1;
}

int32_t ScaleMille(int32_t v, int32_t sMille, int32_t floor1) {
  int64_t x = (int64_t)v * sMille / 1000;
  if (x > 0x3FFFFFFF) x = 0x3FFFFFFF;
  if (v > 0 && x < floor1) x = floor1;
  return (int32_t)x;
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
// The fastest a flight may go, in 24.8 fixed voxels/tick (512 voxels a tick
// was the old whole-voxel ceiling; it stays as the ceiling of the unit).
constexpr int32_t kMaxSpeedFx = 512 * kSpellFxOne;
// A speed authored in glyphs.json (voxels/tick, fractional) to fixed point.
int32_t SpeedFxFrom(double voxPerTick) {
  const double fx = voxPerTick * (double)kSpellFxOne;
  if (fx < 1.0) return 1;
  if (fx > (double)kMaxSpeedFx) return kMaxSpeedFx;
  return (int32_t)(fx + 0.5);
}
// "#RRGGBB" or "#AARRGGBB" -> the sprite's 0xAABBGGRR. Anything else: white.
uint32_t ParseLookColor(const std::string& hex) {
  if (hex.size() != 7 && hex.size() != 9) return 0xFFFFFFFFu;
  if (hex[0] != '#') return 0xFFFFFFFFu;
  uint32_t v = 0;
  for (size_t i = 1; i < hex.size(); i++) {
    const char c = hex[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return 0xFFFFFFFFu;
    v = (v << 4) | d;
  }
  const uint32_t a = hex.size() == 9 ? (v >> 24) & 0xFF : 0xFF;
  const uint32_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  return (a << 24) | (b << 16) | (g << 8) | r;
}
constexpr int32_t kGravityFxPerTick2 =
    (int32_t)((int64_t)981 * kVoxelsPerMetre * kSpellFxOne /
              (100 * kTicksPerSecond * kTicksPerSecond));

const char* kSortNames[kGlyphSortCount] = {"matter", "effect",   "delivery",
                                          "mod",    "operator", "separator"};

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
    {"repeat", SpellVerb::Repeat},   {"launch", SpellVerb::Launch},
    {"strike", SpellVerb::Strike},
};

bool ParseModField(const std::string& s, ModField& out) {
  static const std::pair<const char*, ModField> k[] = {
      {"count", ModField::Count},       {"gravity", ModField::Gravity},
      {"speed", ModField::Speed},
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

// ---- magnitude (PLAN_spell_magnitude §2) ----------------------------------------

namespace {
// Floor division, for a lattice that crosses zero (a signed magnitude).
int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

int32_t ClampMagnitude(const GlyphDef& g, int32_t mag) {
  if (!g.graded) return kMagOne;
  if (mag == kMagUnset) return g.magDefault;
  const int64_t step = g.magStep < 1 ? 1 : g.magStep;
  // Snap to the nearest multiple of the step, then into range - rounding UP
  // when the range's floor is off the lattice (a mul mod's x1 floor may be).
  int64_t m = FloorDiv((int64_t)mag + step / 2, step) * step;
  if (m < g.magMin) m = -FloorDiv(-(int64_t)g.magMin, step) * step;
  if (m > g.magMax) m = FloorDiv(g.magMax, step) * step;
  if (m < g.magMin) m = g.magMin;
  return (int32_t)m;
}

void SplitMagnitude(const std::string& word, std::string& id, int32_t& mag) {
  mag = kMagUnset;
  const size_t cut = word.find_first_of("@!+");
  id = word.substr(0, cut);
  const size_t at = word.find('@');
  if (at == std::string::npos) return;
  // Decimal, parsed by hand: a locale-dependent strtod would read "0,5" on a
  // machine that says comma, and pages are shared. A leading '-' is a signed
  // component's other direction (`lift@-1` presses down).
  size_t k = at + 1;
  bool neg = false;
  if (k < word.size() && word[k] == '-') {
    neg = true;
    k++;
  }
  int64_t whole = 0, frac = 0, scale = 1;
  bool dot = false, any = false;
  for (; k < word.size(); k++) {
    const char ch = word[k];
    if (ch == '!' || ch == '+') break;   // a timing suffix follows
    if (ch == '.' && !dot) {
      dot = true;
      continue;
    }
    if (ch < '0' || ch > '9') return;   // malformed: the word's default
    any = true;
    if (!dot) whole = std::min<int64_t>(whole * 10 + (ch - '0'), 100000);
    else if (scale < 1000) {
      frac = frac * 10 + (ch - '0');
      scale *= 10;
    }
  }
  if (!any) return;
  const int64_t v = std::min<int64_t>(whole * 1000 + frac * 1000 / scale, 0x3FFFFFFF);
  mag = (int32_t)(neg ? -v : v);
}

std::string MagnitudeDecimal(int32_t mag) {
  const bool neg = mag < 0;
  const int64_t a = neg ? -(int64_t)mag : (int64_t)mag;
  std::string s = (neg ? "-" : "") + std::to_string(a / 1000);
  const int32_t frac = (int32_t)(a % 1000);
  if (frac) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%03d", (int)frac);
    std::string f = buf;
    while (!f.empty() && f.back() == '0') f.pop_back();
    s += "." + f;
  }
  return s;
}

const char* SpellTriggerName(SpellTrigger t) {
  switch (t) {
    case SpellTrigger::Hit: return "hit";
    case SpellTrigger::Bounce: return "bounce";
    case SpellTrigger::Expire: return "expire";
    case SpellTrigger::Launch: return "launch";
    case SpellTrigger::Every: return "every";
  }
  return "hit";
}

std::string TimingSuffix(const SpellTiming& t) {
  std::string s;
  if (t.trigger != SpellTrigger::Hit) {
    s += "!";
    s += SpellTriggerName(t.trigger);
    if (t.trigger == SpellTrigger::Every) s += std::to_string(t.every);
  }
  if (t.delay > 0) s += "+" + std::to_string(t.delay);
  return s;
}

std::string TimingPhrase(const SpellTiming& t) {
  std::string s;
  switch (t.trigger) {
    case SpellTrigger::Hit: s = "on hit"; break;
    case SpellTrigger::Bounce: s = "at each bounce"; break;
    case SpellTrigger::Expire: s = "when its life runs out"; break;
    case SpellTrigger::Launch: s = "at launch"; break;
    case SpellTrigger::Every: s = "every " + std::to_string(t.every) + " ticks of flight"; break;
  }
  if (t.delay > 0) s += ", " + std::to_string(t.delay) + " ticks later";
  return s;
}

namespace {
// Which words may carry a timing at all: the things that HAPPEN in a payload.
// A mod edits a record and a mark is structure; `trail` is a mod too (it is
// already "every marked voxel") - so for those the suffix is dropped.
bool TimeableGlyph(const GlyphDef& g) {
  if (g.sort == GlyphSort::Mod || g.sort == GlyphSort::Separator) return false;
  if (g.sort == GlyphSort::Operator && g.result == GlyphSort::Mod) return false;
  return true;
}
SpellTiming SanitizeTiming(const GlyphLibrary& lib, const GlyphDef& g, SpellTiming t) {
  if (!TimeableGlyph(g)) return SpellTiming{};
  t.delay = std::clamp(t.delay, 0, lib.budgets.maxDelayTicks);
  if (t.trigger == SpellTrigger::Every)
    t.every = std::clamp(t.every <= 0 ? 10 : t.every, lib.budgets.minEveryTicks,
                         lib.budgets.maxLifetimeTicks);
  else
    t.every = 0;
  return t;
}
int32_t ReadDigits(const std::string& w, size_t k, int32_t fallback) {
  int64_t v = 0;
  bool any = false;
  for (; k < w.size() && w[k] >= '0' && w[k] <= '9'; k++) {
    v = std::min<int64_t>(v * 10 + (w[k] - '0'), 100000);
    any = true;
  }
  return any ? (int32_t)v : fallback;
}
}  // namespace

int ParseWord(const GlyphLibrary& lib, const std::string& word, int32_t& mag,
              SpellTiming& timing) {
  std::string id;
  int32_t raw = kMagUnset;
  SplitMagnitude(word, id, raw);
  timing = SpellTiming{};
  mag = kMagOne;
  const int gi = lib.Find(id);
  if (gi < 0) return -1;
  const GlyphDef& g = lib.glyphs[(size_t)gi];
  mag = ClampMagnitude(g, raw);
  const size_t bang = word.find('!');
  if (bang != std::string::npos) {
    size_t k = bang + 1;
    std::string name;
    while (k < word.size() && word[k] >= 'a' && word[k] <= 'z') name += word[k++];
    for (int t = 0; t < kSpellTriggerCount; t++)
      if (name == SpellTriggerName((SpellTrigger)t)) timing.trigger = (SpellTrigger)t;
    if (timing.trigger == SpellTrigger::Every) timing.every = ReadDigits(word, k, 10);
  }
  const size_t plus = word.find('+');
  if (plus != std::string::npos) timing.delay = ReadDigits(word, plus + 1, 0);
  timing = SanitizeTiming(lib, g, timing);
  return gi;
}

std::string SerializeWord(const GlyphDef& g, int32_t mag, const SpellTiming& timing) {
  std::string s = g.id;
  if (g.graded && mag != g.magDefault) s += "@" + MagnitudeDecimal(mag);
  return s + TimingSuffix(timing);
}

std::string WordWithMagnitude(const GlyphDef& g, int32_t mag) {
  return SerializeWord(g, mag, SpellTiming{});
}

int32_t MagnitudeWordCost(int32_t word, int32_t mag, int32_t magDefault) {
  if (word <= 0) return word;
  if (mag == magDefault) return word;
  // word × (mag / default)², rounded up. CONVEX, so a middle magnitude is the
  // efficient one and "always max it" is a real price rather than the default
  // (Morrowind's spellmaking collapsed to max magnitude on a linear price).
  // CENTRED ON THE WORD'S OWN DEFAULT, not on 1: `speed` defaults to 2, and a
  // price centred on 1 charged a freshly placed `speed` 4x its authored cost
  // (16 against its alias `swift`'s 4) for saying nothing.
  const int64_t m = mag < 0 ? -(int64_t)mag : mag;
  int64_t d = magDefault < 0 ? -(int64_t)magDefault : magDefault;
  if (d == 0) d = kMagOne;
  const int64_t c = ((int64_t)word * m * m + d * d - 1) / (d * d);
  return (int32_t)std::min<int64_t>(std::max<int64_t>(c, 1), 0x3FFFFFFF);
}

std::string MagnitudeLabel(const GlyphLibrary& lib, int glyph, int32_t n, int32_t mag) {
  const GlyphDef* g = lib.At(glyph);
  if (!g) return std::string();
  if (n < 1) n = 1;
  const int64_t s = (int64_t)n * mag;   // effective scale, per-mille
  auto fix = [](int64_t mille, int decimals) {
    // A per-mille number as a decimal with `decimals` places, sign kept.
    const bool neg = mille < 0;
    int64_t a = neg ? -mille : mille;
    int64_t div = decimals == 2 ? 10 : (decimals == 1 ? 100 : 1000);
    int64_t q = (a + div / 2) / div;
    std::string out = std::to_string(q / (1000 / div));
    if (decimals > 0) {
      std::string f = std::to_string(q % (1000 / div));
      while ((int)f.size() < decimals) f = "0" + f;
      out += "." + f;
    }
    return (neg ? "-" : "") + out;
  };
  switch (g->sort) {
    case GlyphSort::Matter: {
      const int64_t v = std::max<int64_t>(1, (int64_t)lib.budgets.sprayVoxels * s / 1000);
      return std::to_string(v) + " voxels";
    }
    case GlyphSort::Effect:
      switch (g->verb) {
        case SpellVerb::Explode: return "power " + std::to_string((int64_t)g->power * s / 1000);
        case SpellVerb::Wind:
          return "wind " + fix((int64_t)std::lround(g->wind.speedMs * 1000.0f) * s / 1000, 0) +
                 " m/s";
        default: return "x" + fix(s, 2);
      }
    case GlyphSort::Mod: {
      if (g->field == ModField::Count) return std::string();
      if (g->op == ModOp::Add) {
        const int64_t total = (int64_t)g->amount * s / 1000;
        switch (g->field) {
          case ModField::Gravity: return "gravity " + std::string(total > 0 ? "+" : "") + fix(total, 2) + " g";
          case ModField::Bounces: return std::to_string(total) + " bounces";
          case ModField::Pierce: return "pierce " + std::to_string(total);
          case ModField::Seek: return "homing " + std::to_string(total);
          case ModField::Fuse: return "fuse " + fix(total * 1000 / 30, 1) + " s";
          default: return "+" + std::to_string(total);
        }
      }
      // A mul/div mod's factor is amount × magnitude, applied n times.
      int64_t f = 1000;
      for (int32_t k = 0; k < n; k++) f = f * ((int64_t)g->amount * mag) / 1000;
      const char* what = g->field == ModField::Speed      ? "speed "
                         : g->field == ModField::Lifetime ? "life "
                         : g->field == ModField::Radius   ? "radius "
                                                          : "";
      return std::string(what) + (g->op == ModOp::Div ? "/" : "x") + fix(f, 2);
    }
    case GlyphSort::Operator: return "x" + fix(s, 2);
    default: return std::string();
  }
}

const char* ModFieldName(ModField f) {
  switch (f) {
    case ModField::Count: return "count";
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
    rd("maxDelayTicks", b.maxDelayTicks);
    rd("minEveryTicks", b.minEveryTicks);
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
  b.maxDelayTicks = ClampI(b.maxDelayTicks, 0, 1800);
  b.minEveryTicks = ClampI(b.minEveryTicks, 1, 60);
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
  out.hand.noun = "hand";
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
        d.foreignPenaltyMille = ClampI(g.value("foreignPenaltyMille", 1000), 0, 100000);
        for (const json& nm : g.value("native", json::array())) {
          uint32_t id;
          if (!nm.is_string() || !resolveMat(nm.get<std::string>(), id)) {
            errors += where + "native list names unknown material\n";
            return false;
          }
          d.nativeMats.push_back(id);
        }
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
        if (d.verb == SpellVerb::Strike) {
          // THE STRIKE BLOCK (docs/PLAN_electricity.md E3): what the bolt and
          // its splash are made of, BY NAME, and the shape. Clamped to
          // game/lightning.h's caps here so a plan can never exceed them.
          const json sj = g.value("strike", json::object());
          GlyphStrike& st = d.strike;
          st.has = true;
          const std::string bolt = sj.value("bolt", std::string());
          const std::string splash = sj.value("splash", std::string());
          if (!bolt.empty() && !resolveMat(bolt, st.boltMat)) {
            errors += where + "strike bolt names unknown material \"" + bolt + "\"\n";
            return false;
          }
          if (!splash.empty() && !resolveMat(splash, st.splashMat)) {
            errors += where + "strike splash names unknown material \"" + splash + "\"\n";
            return false;
          }
          st.height = ClampI(sj.value("height", 0), 0, kStrikeMaxHeight);
          st.search = ClampI(sj.value("search", 0), 0, kStrikeMaxSearch);
          st.splash = ClampI(sj.value("splashRadius", 2), 1, kStrikeMaxSplash);
          st.arcs = ClampI(sj.value("arcs", 4), 0, kStrikeMaxArcs);
          st.forks = ClampI(sj.value("forks", 2), 0, 3);
          st.conductBonus = ClampI(sj.value("conductBonus", 6), 0, 64);
          st.tariffMille = ClampI(sj.value("tariffMille", 1000), 1, 100000);
          if ((st.boltMat == 0 || st.height == 0) && (st.splashMat == 0 || st.arcs == 0)) {
            // Neither a bolt nor a splash: a word that does nothing.
            errors += where + "is a strike with neither a bolt nor a splash\n";
            return false;
          }
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
        d.speedFx = SpeedFxFrom(g.value("speed", 4.8));
        d.lifetimeTicks = ClampI(g.value("lifetimeTicks", 150), 1, b.maxLifetimeTicks);
        d.impactRadius = ClampI(g.value("impactRadius", 3), 0, 8);
        d.gravityMille = ClampI(g.value("gravity", 0), -8000, 8000);
        d.fuseTicks = ClampI(g.value("fuse", 0), 0, b.maxLifetimeTicks);
        d.reach = ClampI(g.value("reach", 0), 0, 64);
        d.ticks = ClampI(g.value("ticks", 90), 1, b.maxStatusTicks);
        d.body = g.value("body", false);
        d.resolveOnExpiry = g.value("resolveOnExpiry", false);
        // The look: render-only, so a bad value is a default and not an error.
        d.look.shape = d.body ? LookShape::Spark : LookShape::Ball;
        if (g.contains("look") && g["look"].is_object()) {
          const json& lk = g["look"];
          const std::string shape = lk.value("shape", std::string());
          if (shape == "bolt") d.look.shape = LookShape::Bolt;
          else if (shape == "ball") d.look.shape = LookShape::Ball;
          else if (shape == "orb") d.look.shape = LookShape::Orb;
          else if (shape == "spark") d.look.shape = LookShape::Spark;
          d.look.size = std::clamp((float)lk.value("size", 0.6), 0.1f, 4.0f);
          d.look.tail = ClampI(lk.value("tail", 0), 0, 16);
          d.look.tailStep = std::clamp((float)lk.value("tailStep", 0.5), 0.1f, 4.0f);
          d.look.glow = std::clamp((float)lk.value("glow", 1.0), 0.0f, 4.0f);
          d.look.color = ParseLookColor(lk.value("color", std::string()));
        }
        if (d.body) {
          // A rigid-body carrier is MADE of something: the ball the owner
          // adopts as debris needs a material, and it is content.
          d.materialName = g.value("material", std::string());
          if (!resolveMat(d.materialName, d.material) || d.material == 0) {
            errors += where + "is a body delivery with unknown material \"" +
                      d.materialName + "\"\n";
            return false;
          }
        }
        // What the describe line calls one of these. Content: a modder's
        // `comet` reads as "a comet" without a line of C++.
        d.noun = g.value("noun", d.id);
        break;
      }
      case GlyphSort::Separator: {
        // RULE 4's two marks. `lane` OPENS a lane scope on the current pile,
        // `end` CLOSES the innermost open one. Which of the two a word is is
        // CONTENT ("scope": "open" | "close"), so a modder's synonym needs no
        // C++; nothing else about a separator is a field.
        const std::string sc = g.value("scope", std::string("open"));
        if (sc != "open" && sc != "close") {
          errors += where + "has scope \"" + sc + "\" (want \"open\" or \"close\")\n";
          return false;
        }
        d.scopeClose = sc == "close";
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

    // MAGNITUDE (PLAN_spell_magnitude §2.2): which words the page may scale,
    // and over what range. The DEFAULT follows the sort, so an authored word
    // needs no block: nouns and field mods scale ¼..4 in quarters; a whole-unit
    // field (a bounce, a pierce, a homing step) scales in whole units; a count
    // mod (the fan), a delivery and a mark do not scale at all; an operator
    // scales only when its glyph says so. A mul/div mod never goes below ×1, so
    // `swift` cannot be turned into `slow` by the handle.
    switch (d.sort) {
      case GlyphSort::Matter:
      case GlyphSort::Effect:
        d.graded = true;
        d.magMin = 250, d.magMax = 4000, d.magStep = 250;
        break;
      case GlyphSort::Mod:
        if (d.field == ModField::Count) break;
        d.graded = true;
        if (d.field == ModField::Bounces || d.field == ModField::Pierce ||
            d.field == ModField::Seek) {
          d.magMin = 1000, d.magMax = 8000, d.magStep = 1000;
        } else {
          d.magMin = 250, d.magMax = 4000, d.magStep = 250;
        }
        break;
      default:
        break;
    }
    bool explicitMag = false;
    // A SIGNED component may go below zero: gravity added (`lift@-1` presses
    // down) and a wind's speed (`wind@-1` pulls in). Nothing else has a
    // meaning for "negative this much".
    const bool signedOk =
        (d.sort == GlyphSort::Mod && d.op == ModOp::Add && d.field == ModField::Gravity) ||
        (d.sort == GlyphSort::Effect && d.verb == SpellVerb::Wind);
    d.hidden = g.value("hidden", false);
    if (g.contains("magnitude")) {
      explicitMag = true;
      const json& m = g["magnitude"];
      const bool scalable = d.sort != GlyphSort::Delivery && d.sort != GlyphSort::Separator &&
                            !(d.sort == GlyphSort::Mod && d.field == ModField::Count);
      if (m.is_boolean()) {
        d.graded = m.get<bool>() && scalable;
        if (d.graded && d.magStep == 1000 && d.magMin == 1000 && d.magMax == 1000)
          d.magMin = 250, d.magMax = 4000, d.magStep = 250;
      } else if (m.is_object() && scalable) {
        d.graded = true;
        auto mille = [&](const char* k, int32_t def) {
          return m.contains(k) && m[k].is_number()
                     ? (int32_t)std::lround(m[k].get<double>() * 1000.0)
                     : def;
        };
        d.magMin = mille("min", 250);
        d.magMax = mille("max", 4000);
        d.magStep = mille("step", 250);
        d.magDefault = mille("default", 1000);
      } else if (!m.is_boolean()) {
        errors += where + "\"magnitude\" must be false or {min, max, step}"
                  " (and a delivery, a mark or a count mod does not scale)\n";
        return false;
      }
    }
    // The x1 floor on a mul/div mod is a DEFAULT, for the words that existed
    // before magnitudes (`swift` must not become `slow`). A glyph that states
    // its range - `speed`, whose whole point is going both ways - keeps it.
    if (d.graded && !explicitMag && (d.sort == GlyphSort::Mod) &&
        (d.op == ModOp::Mul || d.op == ModOp::Div) && d.amount > 0)
      d.magMin = std::max(d.magMin, (1000 + d.amount - 1) / d.amount);
    d.magStep = ClampI(d.magStep, 1, 8000);
    d.magMin = ClampI(d.magMin, signedOk ? -8000 : d.magStep, 8000);
    d.magMax = ClampI(d.magMax, d.magMin, 8000);
    if (!d.graded) d.magMin = d.magMax = d.magStep = kMagOne;
    d.magDefault = d.graded ? ClampMagnitude(d, d.magDefault) : kMagOne;
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
  out.crumbles.assign(mats.size(), 0);
  for (size_t i = 0; i < mats.size(); i++)
    out.crumbles[i] = mats[i].gpu.klass == CLASS_POWDER &&
                      (mats[i].gpu.flags & kMatFlagWander) == 0u;
  return true;
}

// ---- parse (the three rules) ---------------------------------------------------

GlyphSort NodeSort(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return GlyphSort::Effect;
  const SpellNode& n = t.nodes[node];
  // A BOX is an Effect value (rule 2), whatever its delivery's own sort is.
  // That is the whole trick: `explosive projectile` is a noun again, so it can
  // go back in the pile, be boxed again, or be taken by `echo`.
  if (n.box) return GlyphSort::Effect;
  // A VALUE CALL is the sort its body's items share: a page that says `fire`
  // is Matter, one that says `swift twin` is a Mod (it sticks to the next box
  // like the words it holds), one that says a bolt is an Effect. A body of
  // mixed sorts is an Effect - the sort a box's payload is.
  if (!n.call.empty()) {
    if (n.items.empty()) return GlyphSort::Effect;
    const GlyphSort s0 = NodeSort(lib, t, n.items[0]);
    for (int ii : n.items)
      if (NodeSort(lib, t, ii) != s0) return GlyphSort::Effect;
    return s0;
  }
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return GlyphSort::Effect;
  return n.group ? g->result : g->sort;
}

int CallOutputs(const SpellTree& t, int node) {
  if (!IsValueCall(t, node)) return 0;
  return (int)t.nodes[(size_t)node].items.size();
}

namespace {

bool SlotAccepts(const GlyphLibrary& lib, const SpellTree& t, uint8_t mask, int node) {
  // A page with several outputs is several items, and a slot holds ONE: the
  // operator sees a wall rather than half a page.
  if (IsValueCall(t, node) && CallOutputs(t, node) != 1) return false;
  if (mask == kSortAny) return true;
  return (mask & (uint8_t)(1u << (int)NodeSort(lib, t, node))) != 0;
}

}  // namespace

namespace {

// The PILE, merged: identical items collapse (sum n, capped) in first-seen
// order. Rule 1's "order inside the pile does not matter" is only true if this
// merge is order-insensitive, and it is: the multiset is the same however the
// words arrived.
std::vector<int> MergePile(const GlyphLibrary& lib, SpellTree& t,
                           const std::vector<int>& pile) {
  const int32_t cap = lib.budgets.maxMultiplicity;
  std::vector<int> outItems;
  std::vector<std::string> keys;
  for (int ni : pile) {
    // A PAGE CALL NEVER MERGES. Two of them are two uses of the page, each with
    // its own inputs; a multiplicity on a call has no meaning to give it.
    if (IsValueCall(t, ni)) {
      keys.push_back(std::string());
      outItems.push_back(ni);
      continue;
    }
    const std::string k = NodeKey(lib, t, ni);
    bool merged = false;
    for (size_t j = 0; j < keys.size(); j++) {
      if (keys[j] != k) continue;
      SpellNode& first = t.nodes[outItems[j]];
      first.n = std::min(first.n + t.nodes[ni].n, cap);
      first.last = std::max(first.last, t.nodes[ni].last);
      merged = true;
      break;
    }
    if (merged) continue;
    keys.push_back(k);
    outItems.push_back(ni);
  }
  return outItems;
}

// ONE SCOPE of the pile while parsing (rule 4). The root scope is the pile the
// hand box closes; a `lane` word pushes a new one and `end` pops it back into
// its parent's `lanes`. A scope holds its own shared items AND the lane
// segments already closed inside it, because a delivery spoken here boxes
// exactly that: the innermost OPEN scope, lanes and all.
struct ParseScope {
  std::vector<int> pile;                 // the shared segment of this scope
  std::vector<std::vector<int>> lanes;   // segments closed inside it, in order
  std::vector<int> laneAt;               // two per lane: the `lane`, then `end`
  int openAt = -1;                       // the `lane` word that opened it
  int startPos = 0;                      // where its first item could be spoken
  // >= 0: not a lane but a PAGE's body (the index into the stack's book). An
  // `end` never closes it and a delivery inside it boxes only what is in it.
  int page = -1;
};

// Every operator slot under `node` that is still EMPTY - the inputs of a page
// whose body this is - appended as `group * 2 + side`. Walks into boxes and
// into nested calls' bodies (an input nobody filled in the inner page is an
// input of the outer one too), but not into an operand that was filled.
void CollectHoles(const GlyphLibrary& lib, const SpellTree& t, int node,
                  std::vector<int>& out) {
  if (node < 0 || node >= (int)t.nodes.size()) return;
  const SpellNode& n = t.nodes[(size_t)node];
  if (n.group) {
    const GlyphDef* g = lib.At(n.glyph);
    CollectHoles(lib, t, n.left, out);
    if (g && g->hasLeft && n.left < 0) out.push_back(node * 2);
    if (g && g->hasRight && n.right < 0) out.push_back(node * 2 + 1);
    CollectHoles(lib, t, n.right, out);
    return;
  }
  for (int ii : n.items) CollectHoles(lib, t, ii, out);
  for (int ii : n.callItems) CollectHoles(lib, t, ii, out);
}

uint8_t HoleMask(const GlyphLibrary& lib, const SpellTree& t, int hole) {
  const GlyphDef* g = lib.At(t.nodes[(size_t)(hole / 2)].glyph);
  if (!g) return 0;
  return (hole & 1) ? g->rightMask : g->leftMask;
}

void SetHole(const GlyphLibrary& lib, SpellTree& t, int hole, int value) {
  SpellNode& grp = t.nodes[(size_t)(hole / 2)];
  if (hole & 1) grp.right = value;
  else grp.left = value;
  const GlyphDef* g = lib.At(grp.glyph);
  grp.complete = g && (!g->hasLeft || grp.left >= 0) && (!g->hasRight || grp.right >= 0);
}

// RULE 2 + RULE 4: box a whole SCOPE under `deliveryGlyph` (-1 = the implicit
// hand) — its shared items first, then one segment per closed lane. Returns
// the new node index. `at` is the delivery word's spoken position, -1 for the
// hand. The items are stamped with their lane HERE, before the merge, because
// the lane is part of NodeKey and the merge must not join two segments.
int CloseBox(const GlyphLibrary& lib, SpellTree& t, const ParseScope& sc,
             int deliveryGlyph, int at, int spokenEnd) {
  SpellNode b;
  b.box = true;
  b.glyph = deliveryGlyph;
  b.n = 1;
  b.at = at;
  b.laneAt = sc.laneAt;
  std::vector<int> all;
  for (int ni : sc.pile) {
    t.nodes[ni].lane = 0;
    all.push_back(ni);
  }
  for (size_t k = 0; k < sc.lanes.size(); k++)
    for (int ni : sc.lanes[k]) {
      t.nodes[ni].lane = (int32_t)k + 1;
      all.push_back(ni);
    }
  b.items = MergePile(lib, t, all);
  b.first = at >= 0 ? at : spokenEnd;
  b.last = at >= 0 ? at : spokenEnd;
  // A BOX SPANS ITS WHOLE SCOPE, from where the scope's first word could have
  // been spoken: anything said there would have been in the pile this box
  // closed, so it is inside the box even when the pile ended up empty. (Law
  // L6 reads the span to decide what "far away" means, and `end projectile`
  // is a box whose delivery word is not where its pile began.)
  b.first = std::min(b.first, sc.startPos);
  // The `lane` / `end` marks belong to it for the same reason.
  for (int la : sc.laneAt) {
    if (la < 0) continue;
    b.first = std::min(b.first, la);
    b.last = std::max(b.last, la);
  }
  for (int ni : b.items) {
    b.first = std::min(b.first, t.nodes[ni].first);
    b.last = std::max(b.last, t.nodes[ni].last);
  }
  t.nodes.push_back(std::move(b));
  return (int)t.nodes.size() - 1;
}

}  // namespace

SpellTree ParseSpell(const GlyphLibrary& lib, const SpellStack& stack) {
  SpellTree t;
  const int32_t cap = lib.budgets.maxMultiplicity;

  // RUNS MERGE first, and NOT for deliveries or the scope marks: `shotgun
  // shotgun` is one item ×2, but `projectile projectile` is two boxes, because
  // each delivery boxes what is in front of it and a merged pair would
  // silently drop a nesting. (A mark between two identical words is what makes
  // `fire lane fire end` two items in two segments rather than `fire×2`.)
  std::vector<int> items;
  t.book = stack.book;
  for (size_t i = 0; i < stack.spoken.size(); i++) {
    const int gi = stack.spoken[i];
    // A PAGE MARK is an item of the pass below (it opens or closes a scope)
    // and never merges with anything, which is also what keeps `fire <fire>`
    // from becoming `fire×2` across the page's edge.
    if (SpokenIsMark(gi)) {
      SpellNode m;
      m.glyph = gi;
      m.first = m.last = (int)i;
      t.nodes.push_back(m);
      items.push_back((int)t.nodes.size() - 1);
      continue;
    }
    const GlyphDef* gd = lib.At(gi);
    if (!gd) continue;
    const bool mergeable =
        gd->sort != GlyphSort::Delivery && gd->sort != GlyphSort::Separator;
    // A word's magnitude is clamped to what its glyph allows HERE, once, so
    // every consumer downstream may trust it. Only EQUAL magnitudes merge: a
    // run is "the same word again", and `float@0.5 float` is not.
    const int32_t mag = ClampMagnitude(*gd, stack.MagAt(i));
    const SpellTiming tm = SanitizeTiming(lib, *gd, stack.TimingAt(i));
    if (mergeable && !items.empty() && !t.nodes[items.back()].group &&
        t.nodes[items.back()].glyph == gi && t.nodes[items.back()].mag == mag &&
        t.nodes[items.back()].timing == tm) {
      SpellNode& prev = t.nodes[items.back()];
      prev.n = std::min(prev.n + 1, cap);
      prev.last = (int)i;
      continue;
    }
    SpellNode n;
    n.glyph = gi;
    n.n = 1;
    n.mag = mag;
    n.timing = tm;
    n.first = n.last = (int)i;
    t.nodes.push_back(n);
    items.push_back((int)t.nodes.size() - 1);
  }

  // ONE left-to-right pass over the merged words. Operators bind (they must
  // run here, not in a separate pass, because the item to an operator's left
  // may be a BOX a delivery just made); deliveries box the innermost OPEN
  // scope; `lane` opens a scope and `end` closes one; everything else falls
  // into the current scope's pile.
  std::vector<ParseScope> scopes(1);
  const int spokenEnd = (int)stack.spoken.size();

  // RULE 4: close the innermost open lane back into its parent. `endPos` is
  // the `end` word, or -1 when the sentence simply ran out and the lane is
  // closed implicitly. A lane that itself opened lanes and never boxed them
  // has no record to make them columns OF, so they flatten into it — the one
  // place a scope is not a column.
  auto closeLane = [&](int endPos) {
    ParseScope sc = std::move(scopes.back());
    scopes.pop_back();
    for (const std::vector<int>& sub : sc.lanes)
      for (int ni : sub) sc.pile.push_back(ni);
    ParseScope& parent = scopes.back();
    parent.lanes.push_back(std::move(sc.pile));
    parent.laneAt.push_back(sc.openAt);
    parent.laneAt.push_back(endPos);
  };
  auto finishClause = [&](int at) {
    SpellClause c;
    c.root = CloseBox(lib, t, scopes[0], -1, -1, at);
    c.delivery = -1;
    c.weight = 1;
    c.bag = t.nodes[c.root].items;
    t.clauses.push_back(std::move(c));
    scopes[0] = ParseScope{};
  };

  // THE CLOSING MARK OF A PAGE: its body becomes ONE item of the scope around
  // it (see "A PAGE USED AS ONE GLYPH" in spell.h). `i` is the mark's index in
  // `items` and may advance past words a right-hand input takes.
  auto closePage = [&](int endPos, size_t& i) {
    ParseScope sc = std::move(scopes.back());
    scopes.pop_back();
    ParseScope& outer = scopes.back();
    const std::string name = sc.page >= 0 && sc.page < (int)stack.book.names.size()
                                 ? stack.book.names[(size_t)sc.page]
                                 : std::string("?");
    const int openPos = sc.openAt;
    // A CARRIER is a page that is one box and nothing else, whose pile holds
    // only finished mods: a delivery with its settings, and no payload. It is
    // a delivery word, spoken as a page.
    bool carrier = false;
    if (sc.lanes.empty() && sc.pile.size() == 1) {
      const SpellNode& pb = t.nodes[(size_t)sc.pile[0]];
      carrier = pb.box && pb.glyph >= 0 && pb.laneAt.empty() && pb.call.empty();
      for (int ii : pb.items) {
        std::vector<int> h;
        CollectHoles(lib, t, ii, h);
        carrier = carrier && NodeSort(lib, t, ii) == GlyphSort::Mod && h.empty();
      }
    }
    if (carrier) {
      const SpellNode pb = t.nodes[(size_t)sc.pile[0]];
      const int b = CloseBox(lib, t, outer, pb.glyph, openPos, spokenEnd);
      SpellNode& bn = t.nodes[(size_t)b];
      bn.timing = pb.timing;
      bn.call = name;
      bn.callItems = pb.items;
      bn.first = std::min(bn.first, openPos);
      bn.last = std::max(bn.last, endPos);
      outer.pile.clear();
      outer.lanes.clear();
      outer.laneAt.clear();
      outer.pile.push_back(b);
      return;
    }
    // A VALUE: the body as the page parses alone - its pile, its lanes - held
    // by one node that is not a box (it delivers nothing) and not a group.
    const int c = CloseBox(lib, t, sc, -1, openPos, spokenEnd);
    {
      SpellNode& cn = t.nodes[(size_t)c];
      cn.box = false;
      cn.glyph = -1;
      cn.call = name;
      cn.at = openPos;
      cn.first = openPos;
      cn.last = endPos;
    }
    // ITS INPUTS: every slot it left empty, in spoken order.
    std::vector<int> holes;
    for (int ii : t.nodes[(size_t)c].items) CollectHoles(lib, t, ii, holes);
    std::stable_sort(holes.begin(), holes.end(), [&](int a, int b) {
      const int pa = t.nodes[(size_t)(a / 2)].at, pb = t.nodes[(size_t)(b / 2)].at;
      return pa != pb ? pa < pb : (a & 1) < (b & 1);
    });
    t.nodes[(size_t)c].holes = holes;
    // LEFT INPUTS TAKE THE ITEMS SPOKEN BEFORE THE PAGE, postfix: the last
    // left input takes the item nearest the page, the one before it the item
    // before that, and the first item a slot refuses ends the binding. Only
    // within this scope - a lane mark is a wall here as everywhere.
    for (int k = (int)holes.size() - 1; k >= 0; k--) {
      if (holes[(size_t)k] & 1) continue;
      if (outer.pile.empty()) break;
      const int top = outer.pile.back();
      if (!SlotAccepts(lib, t, HoleMask(lib, t, holes[(size_t)k]), top)) break;
      outer.pile.pop_back();
      SetHole(lib, t, holes[(size_t)k], top);
      t.nodes[(size_t)c].first = std::min(t.nodes[(size_t)c].first, t.nodes[(size_t)top].first);
    }
    outer.pile.push_back(c);
    // RIGHT INPUTS TAKE THE WORDS AFTER IT, in order, exactly as an infix
    // operator's right slot takes the next raw word.
    for (int h : holes) {
      if (!(h & 1)) continue;
      if (i + 1 >= items.size()) break;
      const int nx = items[i + 1];
      if (t.nodes[(size_t)nx].glyph < 0) break;   // a mark: no word there
      if (!SlotAccepts(lib, t, HoleMask(lib, t, h), nx)) break;
      SetHole(lib, t, h, nx);
      t.nodes[(size_t)c].last = std::max(t.nodes[(size_t)c].last, t.nodes[(size_t)nx].last);
      i++;
    }
    bool complete = true;
    for (int h : holes) {
      const SpellNode& gn = t.nodes[(size_t)(h / 2)];
      complete = complete && ((h & 1) ? gn.right : gn.left) >= 0;
    }
    t.nodes[(size_t)c].complete = complete;
  };

  for (size_t i = 0; i < items.size(); i++) {
    const int ni = items[i];
    // ---- a page mark ----------------------------------------------------------
    if (SpokenIsMark(t.nodes[ni].glyph)) {
      const int mark = t.nodes[ni].glyph;
      if (SpokenIsPageOpen(mark)) {
        ParseScope inner;
        inner.page = SpokenPageIndex(mark);
        inner.openAt = t.nodes[ni].first;
        inner.startPos = t.nodes[ni].first + 1;
        scopes.push_back(std::move(inner));
      } else {
        // Close any lane the page left open (implicitly, as the end of a
        // sentence would), then the page. A close with no page open is a
        // stray mark and changes nothing.
        bool open = false;
        for (const ParseScope& s : scopes) open = open || s.page >= 0;
        if (open) {
          while (scopes.back().page < 0) closeLane(-1);
          closePage(t.nodes[ni].first, i);
        }
      }
      continue;
    }
    const GlyphDef& g = lib.glyphs[t.nodes[ni].glyph];
    ParseScope* sc = &scopes.back();
    switch (g.sort) {
      case GlyphSort::Operator: {
        SpellNode grp;
        grp.glyph = t.nodes[ni].glyph;
        grp.n = t.nodes[ni].n;
        grp.mag = t.nodes[ni].mag;
        grp.timing = t.nodes[ni].timing;
        grp.group = true;
        grp.first = t.nodes[ni].first;
        grp.last = t.nodes[ni].last;
        grp.at = t.nodes[ni].first;
        // THE ONE ITEM TO ITS LEFT — the top of THIS SCOPE's pile, which may
        // be a box (`explosive projectile echo` is a turret). A `lane` / `end`
        // boundary is a wall: an operator can never reach past it, because the
        // items on the other side are in another scope entirely.
        if (g.hasLeft && !sc->pile.empty() &&
            SlotAccepts(lib, t, g.leftMask, sc->pile.back())) {
          grp.left = sc->pile.back();
          sc->pile.pop_back();
          grp.first = std::min(grp.first, t.nodes[grp.left].first);
        } else if (g.hasLeft) {
          // AN EMPTY SLOT REACHES BACK TO THE TOP OF ITS SCOPE'S PILE. Any
          // word spoken between that item and here would become the new top
          // and could fill the slot, so that stretch is part of this group's
          // neighbourhood — which is what law L6 reads the span for. With an
          // empty pile the stretch is the whole scope (`lane` … here), and
          // with a word in the way it is whatever separates them, which is how
          // `explosive end transmute` is honest about the `end` between them.
          grp.first = std::min(grp.first, sc->pile.empty()
                                              ? sc->startPos
                                              : t.nodes[sc->pile.back()].last + 1);
        }
        if (g.hasRight && i + 1 < items.size() && !SpokenIsMark(t.nodes[items[i + 1]].glyph) &&
            SlotAccepts(lib, t, g.rightMask, items[i + 1])) {
          grp.right = items[i + 1];
          grp.last = std::max(grp.last, t.nodes[grp.right].last);
          i++;
        }
        grp.complete = (!g.hasLeft || grp.left >= 0) && (!g.hasRight || grp.right >= 0);
        t.nodes.push_back(std::move(grp));
        scopes.back().pile.push_back((int)t.nodes.size() - 1);
        break;
      }
      case GlyphSort::Delivery: {
        // RULE 2, scoped by RULE 4. Inside an open lane the box takes only
        // that lane's items and the lane STAYS OPEN, so more may follow in it;
        // outside any lane it takes the shared items and every closed lane,
        // which is the multi-socket box.
        const int b = CloseBox(lib, t, *sc, t.nodes[ni].glyph, t.nodes[ni].first, spokenEnd);
        // The box's TIMING is what its delivery word said (`projectile!bounce`).
        t.nodes[b].timing = t.nodes[ni].timing;
        sc->pile.clear();
        sc->lanes.clear();
        sc->laneAt.clear();
        sc->pile.push_back(b);
        break;
      }
      case GlyphSort::Separator:
        if (!g.scopeClose) {
          ParseScope inner;
          inner.openAt = t.nodes[ni].first;
          inner.startPos = t.nodes[ni].first + 1;
          scopes.push_back(std::move(inner));
        } else if (scopes.size() > 1 && scopes.back().page < 0) {
          // ...and never a PAGE's scope: an `end` inside a page with no lane
          // of its own open is the same charged no-op it is anywhere else.
          closeLane(t.nodes[ni].first);
        }
        // An `end` with nothing open is a charged no-op, like every other word
        // the grammar has no work for: total in, total out.
        break;
      default:
        sc->pile.push_back(ni);
        break;
    }
  }
  // Silence is the one thing that is not a spell: an empty stack lowers to no
  // clauses at all, not to an empty hand cast. Everything still open is closed
  // implicitly, innermost first.
  if (!stack.spoken.empty()) {
    while (scopes.size() > 1) {
      if (scopes.back().page >= 0) {
        size_t past = items.size();
        closePage(spokenEnd - 1, past);
      } else {
        closeLane(-1);
      }
    }
    finishClause(spokenEnd);
  }
  return t;
}

std::string NodeKey(const GlyphLibrary& lib, const SpellTree& t, int node) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  // RULE 4: the segment is part of the identity, so `fire lane fire` is two
  // items in two lanes and not `fire×2`. Only a non-shared lane is spelled,
  // so every sentence without a `lane` word keys exactly as it always did.
  const std::string lane = n.lane > 0 ? "@" + std::to_string(n.lane) : std::string();
  // A PAGE CALL is its name and what fills its inputs - the body is the page's
  // and is the same wherever the name is spoken.
  if (!n.call.empty() && !n.box) {
    std::string s = "{" + n.call;
    for (int h : n.holes) {
      const SpellNode& gn = t.nodes[(size_t)(h / 2)];
      const int arg = (h & 1) ? gn.right : gn.left;
      s += "|" + (arg >= 0 ? NodeKey(lib, t, arg) : std::string("_"));
    }
    return s + "}" + lane;
  }
  if (n.box) {
    // A box's identity is its pile AND its delivery, multiplicities included:
    // two boxes are the same item only if they would fire the same thing the
    // same way.
    std::string s = "[";
    for (size_t i = 0; i < n.items.size(); i++) {
      if (i) s += ",";
      s += NodeKey(lib, t, n.items[i]) + "#" + std::to_string(t.nodes[n.items[i]].n);
    }
    const std::string head = n.call.empty() ? lib.Delivery(n.glyph).id : "{" + n.call + "}";
    return s + "|" + head + TimingSuffix(n.timing) + "]" + lane;
  }
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return "?";
  // A magnitude is part of the identity (only equal ones merge), spelled only
  // when it is not 1, so every key a sentence without one had is unchanged.
  const std::string mag = (n.mag != kMagOne ? "~" + std::to_string(n.mag) : std::string()) +
                          TimingSuffix(n.timing);
  if (!n.group) return g->id + mag + lane;
  return "(" + (n.left >= 0 ? NodeKey(lib, t, n.left) : std::string()) + "|" + g->id + mag +
         "|" + (n.right >= 0 ? NodeKey(lib, t, n.right) : std::string()) + ")" + lane;
}

// A list of pile items with its LANE SCOPES marked (rule 4). Items are stored
// with the lane they belong to and the shared segment is lane 0, so each
// segment is a contiguous run; a lane is drawn `/ ... /` — the two marks the
// sentence actually contains, so the readout round-trips back to words. ` / `
// in BOTH bracket styles, because the oracle compares these strings verbatim
// and two spellings would be two truths.
static std::string ShowItems(const GlyphLibrary& lib, const SpellTree& t,
                             const std::vector<int>& items, BracketStyle style,
                             int32_t laneCount) {
  std::vector<std::string> parts;
  for (int ii : items)
    if (t.nodes[ii].lane == 0) parts.push_back(ShowNode(lib, t, ii, style));
  for (int32_t k = 1; k <= laneCount; k++) {
    parts.push_back("/");
    for (int ii : items)
      if (t.nodes[ii].lane == k) parts.push_back(ShowNode(lib, t, ii, style));
    parts.push_back("/");
  }
  std::string s;
  for (const std::string& one : parts) {
    if (!s.empty()) s += " ";
    s += one;
  }
  return s;
}

std::string ShowNode(const GlyphLibrary& lib, const SpellTree& t, int node,
                     BracketStyle style) {
  if (node < 0 || node >= (int)t.nodes.size()) return "";
  const SpellNode& n = t.nodes[node];
  // A PAGE is shown as its NAME, in angle quotes, with whatever fills its
  // inputs on the side it was spoken: `(fire <seeker>)`. Its body is the
  // page's own business, and the readout of the page itself shows it.
  const std::string pageMark =
      n.call.empty() ? std::string()
                     : (style == BracketStyle::Oracle ? "\xE2\x80\xB9" + n.call + "\xE2\x80\xBA"
                                                      : "<" + n.call + ">");
  if (!n.call.empty() && !n.box) {
    std::vector<std::string> l, r;
    for (int h : n.holes) {
      const SpellNode& gn = t.nodes[(size_t)(h / 2)];
      const int arg = (h & 1) ? gn.right : gn.left;
      if (arg >= 0) ((h & 1) ? r : l).push_back(ShowNode(lib, t, arg, style));
    }
    if (l.empty() && r.empty()) return pageMark;
    std::string s;
    for (const std::string& a : l) s += a + " ";
    s += pageMark;
    for (const std::string& a : r) s += " " + a;
    return "(" + s + ")";
  }
  if (n.box) {
    // `[ ... DELIVERY]`: the brackets ARE the nesting, so the HUD shows the
    // fold the way the sentence built it, with ` / ` where a lane opened.
    std::string s = ShowItems(lib, t, n.items, style, (int32_t)n.laneAt.size() / 2);
    if (!n.call.empty()) {
      if (!s.empty()) s += " ";
      return "[" + s + pageMark + TimingSuffix(n.timing) + "]";
    }
    const std::string id = lib.Delivery(n.glyph).id + TimingSuffix(n.timing);
    if (!s.empty()) s += " ";
    if (style == BracketStyle::Oracle) {
      s += "**" + id + Times(n.n, style) + "**";
    } else {
      std::string up = id;
      for (char& ch : up) ch = (char)toupper((unsigned char)ch);
      s += up + Times(n.n, style);
    }
    return "[" + s + "]";
  }
  const GlyphDef* g = lib.At(n.glyph);
  if (!g) return "?";
  const std::string at = (n.mag != kMagOne ? "@" + MagnitudeDecimal(n.mag) : std::string()) +
                         TimingSuffix(n.timing);
  if (!n.group) return g->id + at + Times(n.n, style);
  const std::string l =
      n.left >= 0 ? ShowNode(lib, t, n.left, style) : (g->hasLeft ? "_" : "");
  const std::string r =
      n.right >= 0 ? ShowNode(lib, t, n.right, style) : (g->hasRight ? "_" : "");
  // Infix operators are drawn as a join; every unary one as "took the word on
  // its left".
  std::string sym;
  if (g->hasLeft && g->hasRight)
    sym = std::string(style == BracketStyle::Oracle ? "\xE2\x8B\x88" : "><") + at;
  else
    sym = (style == BracketStyle::Oracle ? "\xE2\x97\x82" : "<") + g->id + at;
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
    // The outermost box is always the hand (rule 3), so it is drawn bare —
    // the brackets are reserved for the deliveries you actually spoke. Its
    // lanes (rule 4) are drawn the same way as any other box's.
    const int32_t lanes =
        c.root >= 0 ? (int32_t)t.nodes[c.root].laneAt.size() / 2 : 0;
    std::string s = ShowItems(lib, t, c.bag, style, lanes);
    if (s.empty()) s = "(empty)";
    if (!out.empty()) out += " ";
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
  int32_t c = g ? SatMul(MagnitudeWordCost(g->word, n.mag, g->magDefault), n.n) : 0;
  if (n.group) c = SatAdd(c, SatAdd(WordCostOf(lib, t, n.left), WordCostOf(lib, t, n.right)));
  // A box owns every word inside it: word costs SUM over the whole tree,
  // whatever the tariff does.
  if (n.box)
    for (int ii : n.items) c = SatAdd(c, WordCostOf(lib, t, ii));
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
  r.speedFx = ClampI(SatMul(g.speedFx, weight), 1, kMaxSpeedFx);
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

// DOES THIS MOD MEAN ANYTHING ON THIS RECORD? (rule 3.) A mod sticks to the
// box that closes the pile, and the outermost box is the hand — which has no
// speed, no lifetime, nothing to bounce off. `shotgun` there is three fanned
// resolve points and `float` is the hop; everything else is charged and does
// nothing, and the describe line says which word was wasted rather than
// letting it edit a field nobody reads.
bool ModMeansAnything(ModField f, DeliveryMech mech, bool isHand) {
  if (isHand) return f == ModField::Count || f == ModField::Gravity;
  switch (mech) {
    case DeliveryMech::Flight:
      return f != ModField::None;
    case DeliveryMech::Instant:   // `self`: a point on a body
      return f == ModField::Count || f == ModField::Gravity ||
             f == ModField::Radius || f == ModField::Lifetime;
    case DeliveryMech::Continuous:   // `beam`: a ray with a tick cap
      return f == ModField::Count || f == ModField::Radius ||
             f == ModField::Lifetime;
  }
  return false;
}

// A Mod applied N times: compose, not add. At magnitude `mag` the authored
// amount is scaled first (PLAN_spell_magnitude §2.2): an add mod adds
// amount × mag, a mul mod multiplies by amount × mag, a div mod divides by it.
// At 1000 each is the integer formula it replaced, bit for bit.
void ApplyMod(DeliveryRec& r, const GlyphDef& g, int32_t n, const SpellBudgets& b,
              int32_t mag = kMagOne) {
  auto edit = [&](int32_t v) -> int32_t {
    if (mag == kMagOne) {
      switch (g.op) {
        case ModOp::Mul: return SatMul(v, g.amount);
        case ModOp::Div: return v / (g.amount < 1 ? 1 : g.amount);
        case ModOp::Add: return SatAdd(v, g.amount);
      }
      return v;
    }
    const int64_t f = (int64_t)g.amount * mag;   // the factor, per-mille
    switch (g.op) {
      case ModOp::Mul: {
        const int64_t x = (int64_t)v * f / 1000;
        return (int32_t)std::max<int64_t>(-0x3FFFFFFF, std::min<int64_t>(x, 0x3FFFFFFF));
      }
      case ModOp::Div: return (int32_t)((int64_t)v * 1000 / (f < 1000 ? 1000 : f));
      case ModOp::Add: return SatAdd(v, (int32_t)(f / 1000));
    }
    return v;
  };
  for (int32_t k = 0; k < n; k++) {
    switch (g.field) {
      case ModField::Count: r.count = ClampI(edit(r.count), 1, b.maxInstances); break;
      case ModField::Gravity: r.gravityMille = ClampI(edit(r.gravityMille), -8000, 8000); break;
      case ModField::Speed: r.speedFx = ClampI(edit(r.speedFx), 1, kMaxSpeedFx); break;
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
                       bool coerceMatterToPlace,
                       std::vector<BoxPrice>* prices = nullptr);
// The whole fold, in mutual recursion with LowerEffect: a box's payload may
// hold operators whose operand is another box (`explosive projectile echo`).
// `prices`, when given, collects one row per box on the way out — the graph
// page's per-level subtotal, derived data the VM never reads.
SpellCast LowerBox(const GlyphLibrary& lib, const SpellTree& t, int node,
                   std::vector<BoxPrice>* prices = nullptr);

// A BOX as one Effect value (rule 2): verb `launch`, the record it carries,
// its payload in `inner`.
EffectInst LaunchEffectOf(const GlyphLibrary& lib, const SpellTree& t, int node,
                          std::vector<BoxPrice>* prices) {
  SpellCast c = LowerBox(lib, t, node, prices);
  EffectInst e;
  e.verb = SpellVerb::Launch;
  e.glyph = t.nodes[node].glyph;
  e.n = t.nodes[node].n;
  e.timing = t.nodes[node].timing;
  e.node = node;
  e.inner = c.payload;
  e.launch.push_back(c.delivery);
  return e;
}

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
  e.mag = n.mag;
  e.timing = n.timing;
  e.radius = 0;
  e.node = node;
  return e;
}

EffectInst LowerEffect(const GlyphLibrary& lib, const SpellTree& t, int node,
                       bool coerceMatterToPlace, std::vector<BoxPrice>* prices) {
  const SpellNode& n = t.nodes[node];
  if (n.box) return LaunchEffectOf(lib, t, node, prices);
  const GlyphDef& g = lib.glyphs[n.glyph];
  if (!n.group) {
    if (g.sort == GlyphSort::Matter) return LowerMatter(lib, t, node, coerceMatterToPlace);
    EffectInst e;
    e.verb = g.verb;
    e.glyph = n.glyph;
    e.n = n.n;
    e.mag = n.mag;
    e.timing = n.timing;
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
      e.modMag = n.mag;
    }
    return e;
  }
  // An operator group. The verb is the operator's; the children are its
  // arguments.
  EffectInst e;
  e.verb = g.verb;
  e.glyph = n.glyph;
  e.n = n.n;
  e.mag = n.mag;
  e.timing = n.timing;
  e.node = node;
  e.complete = n.complete;
  e.radius = g.radius;
  auto matterOf = [&](int child, uint32_t& mat, bool& any, int& gl) {
    if (child < 0) return;
    const SpellNode& c = t.nodes[child];
    if (c.group || c.box) return;
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
      e.radius = ClampI(ScaleRadiusCbrtMille(g.radius, SatMul(n.n, n.mag)), 0, 8);
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
        w.mag = t.nodes[child].mag;
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
          e.modMag = c.mag;
        } else {
          // A trail under aura: the body lays it along its own path.
          e.inner.push_back(LowerEffect(lib, t, child, true, prices));
        }
      } else {
        e.inner.push_back(LowerEffect(lib, t, child, g.verb == SpellVerb::Trail, prices));
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
  // A `wide` sticks to ONE box and widens what THAT box resolves. It does not
  // reach through a nested carrier — the inner box has its own record and its
  // own `wide`, and compounding the two would price one word twice.
  if (e.verb == SpellVerb::Launch) return;
  const int32_t cap = e.verb == SpellVerb::Explode ? kMaxExplosionRadius : 8;
  e.radius = ClampI((int32_t)(((int64_t)e.radius * radiusMille + 500) / 1000), 0, cap);
  for (EffectInst& i : e.inner) ScaleEffectRadius(i, radiusMille);
}

int32_t EffectTicks(const GlyphLibrary& lib, const EffectInst& e) {
  const GlyphDef* g = lib.At(e.glyph);
  int32_t ticks = 1;
  int32_t inner = 0;
  switch (e.verb) {
    case SpellVerb::Wind: ticks = g && g->wind.has ? g->wind.ttlTicks : 1; break;
    case SpellVerb::Sustain: ticks = g ? g->ticks : 1; break;
    case SpellVerb::Repeat: ticks = g ? SatMul(EchoRepeats(g, e), g->everyTicks) : 1; break;
    case SpellVerb::Filter: ticks = 1; break;
    case SpellVerb::Launch:
      // A carrier's own clock, THEN whatever its payload keeps running: the
      // rule-2 bound on a nested spell is the whole chain, not one link. With
      // lanes, the clock is the LONGEST instance's (`long` in one lane makes
      // that bolt live longer and none of the others).
      if (!e.launch.empty()) {
        ticks = SatAdd(e.launch[0].lifetimeTicks, e.launch[0].fuseTicks);
        for (const SpellLane& ln : e.launch[0].lanes) {
          ticks = std::max(ticks, SatAdd(ln.rec.lifetimeTicks, ln.rec.fuseTicks));
          for (const EffectInst& x : ln.extra) inner = std::max(inner, EffectTicks(lib, x));
        }
      }
      break;
    default: break;
  }
  for (const EffectInst& i : e.inner) inner = std::max(inner, EffectTicks(lib, i));
  // A DELAY is time the item keeps the spell alive for (rule 2's tick bound).
  return SatAdd(e.verb == SpellVerb::Launch ? SatAdd(ticks, inner) : std::max(ticks, inner),
                e.timing.delay);
}

}  // namespace

int32_t RecInstances(const GlyphLibrary& lib, const DeliveryRec& d) {
  // RULE 4: a lane past the fan GROWS it — "two lanes" is how you say "two of
  // them, carrying different things" without a count mod at all.
  const int32_t want = std::max(d.count, (int32_t)d.lanes.size());
  return ClampI(want, 1, lib.budgets.maxInstances);
}

int32_t LaneSplit(const GlyphLibrary& lib, const DeliveryRec& d, int32_t k) {
  if (k < 0 || k >= (int32_t)d.lanes.size()) return 1;
  return ClampI(d.lanes[(size_t)k].rec.count, 1, lib.budgets.maxInstances);
}

int32_t RecBolts(const GlyphLibrary& lib, const DeliveryRec& d) {
  // THE NUMBER THAT ACTUALLY FLIES. `RecInstances` counts BRANCHES, which is
  // what the socket row draws; a branch with a `shotgun` of its own fires
  // three of itself, and every price, budget and cap has to be charged on the
  // sum. With no count inside any lane every term is 1 and this is
  // `RecInstances` EXACTLY - which is what keeps every laneless sentence,
  // every pinned price and every recorded direction where it was.
  const int32_t branches = RecInstances(lib, d);
  int32_t total = 0;
  for (int32_t k = 0; k < branches; k++) total = SatAdd(total, LaneSplit(lib, d, k));
  return ClampI(total, 1, lib.budgets.maxInstances);
}

// A strike's cells, as the tariff and the volume bound count them: the bolt
// (height plus its forks) and the splash (arcs x 2 x splash), scaled by
// repetition and capped at what one plan may ever hold (game/lightning.h).
static int32_t StrikeVolume(const GlyphDef& g, int32_t scaleMille) {
  if (!g.strike.has) return 0;
  const GlyphStrike& s = g.strike;
  int32_t cells = 0;
  if (s.boltMat && s.height > 0) cells += s.height + s.forks * 6;
  if (s.splashMat) cells += s.arcs * 2 * s.splash;
  return ClampI(ScaleMille(cells, scaleMille, 1), 1, (int32_t)kStrikeMaxCells);
}

int32_t EffectVolume(const GlyphLibrary& lib, const EffectInst& e) {
  const SpellBudgets& b = lib.budgets;
  const GlyphDef* g = lib.At(e.glyph);
  if (!e.complete) return 0;
  switch (e.verb) {
    case SpellVerb::Spray:
      if (e.matA == 0 && !e.anyA) return 0;
      return std::min(ScaleMille(b.sprayVoxels, e.Scale(), 1), b.maxSprayVoxels);
    case SpellVerb::Place: return ScaleMille(Cube(e.radius), e.Scale(), 1);
    case SpellVerb::Convert: return Cube(e.radius);
    case SpellVerb::Explode:
      return Cube(ClampI(ScaleRadiusCbrtMille(e.radius, e.Scale()), 0, kMaxExplosionRadius));
    case SpellVerb::Wind: {
      if (!g || !g->wind.has) return 0;
      const int32_t r = g->wind.radius;
      return SatMul(SatMul(r, r), g->wind.reach);
    }
    case SpellVerb::Mend: return g ? ScaleMille(g->perTick, e.Scale(), 1) : e.n;
    case SpellVerb::Strike: return g ? StrikeVolume(*g, e.Scale()) : 0;
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
      return SatMul(v, EchoRepeats(g, e));
    }
    case SpellVerb::Launch: {
      // A box's footprint is what it carries, plus what it lays on the way,
      // times how many of it fly. With lanes (rule 4) the instances differ, so
      // the BOUND is the widest instance times the fan — still finite, still
      // an over-estimate rather than an under-estimate, which is the direction
      // rule 2 needs.
      if (e.launch.empty()) return 0;
      int32_t shared = 0;
      for (const EffectInst& i : e.inner)
        shared = SatAdd(shared, SatMul(EffectVolume(lib, i), FiresIn(e.launch[0], i)));
      int32_t v = SatAdd(shared, e.launch[0].trailBudget);
      for (const SpellLane& ln : e.launch[0].lanes) {
        int32_t lv = shared;
        for (const EffectInst& i : ln.extra)
          lv = SatAdd(lv, SatMul(EffectVolume(lib, i), FiresIn(ln.rec, i)));
        v = std::max(v, SatAdd(lv, ln.rec.trailBudget));
      }
      return SatMul(v, RecBolts(lib, e.launch[0]));
    }
    default: return 0;
  }
}

namespace {

// The trail's share of a record's tariff: marks x the per-mark tariff of what
// it lays. The budget is a voxel COUNT, so the number of marks is
// budget / mark volume.
int32_t TrailTariffOf(const GlyphLibrary& lib, const DeliveryRec& d, bool withCarry);

// THE TARIFF, with or without the carry premiums nested inside it. Two arms of
// one function rather than two functions, because only the Launch case
// differs and the HUD wants the split (`word + tariff + carry`) after the
// premiums have composed all the way down the tree.
int32_t EffectTariffIn(const GlyphLibrary& lib, const EffectInst& e, bool withCarry) {
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
      const int32_t r = ClampI(ScaleRadiusCbrtMille(e.radius, e.Scale()), 1, kMaxExplosionRadius);
      const int64_t power = std::max<int64_t>(1, (int64_t)(g ? g->power : 220) * e.Scale() / 1000);
      const int64_t v = power * r * r * r * b.rateExplode / 1000;
      return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)std::max<int64_t>(v, 1);
    }
    case SpellVerb::Wind: {
      if (!g || !g->wind.has) return 0;
      // |scale|: a signed wind (`wind@-1` pulls) costs what the push would.
      const int64_t sc = e.Scale() < 0 ? -(int64_t)e.Scale() : (int64_t)e.Scale();
      const int64_t v = (int64_t)vol * g->wind.ttlTicks * sc * b.rateWind / 1000000;
      return v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)std::max<int64_t>(v, 1);
    }
    case SpellVerb::Mend: {
      // voxels × arcane(M) × rate.graft, plus the FOREIGN penalty when M is
      // not the anatomy's own: blood/flesh/bone none, wood some, steel a lot.
      if (e.anyA) return 0;
      int32_t per = SatMul(lib.Arcane(e.matA), b.rateGraft);
      bool native = false;
      if (g)
        for (uint32_t nm : g->nativeMats) native = native || nm == e.matA;
      if (!native && g)
        per = SatAdd(per, (int32_t)((int64_t)lib.Arcane(e.matA) * g->foreignPenaltyMille / 1000));
      return SatMul(vol, per);
    }
    case SpellVerb::Filter: return std::max(1, vol / 1000);
    case SpellVerb::Strike: {
      // cells x arcane(what it lays) x rate.place: the bolt's matter priced as
      // if it were placed, which it is (CellOps, IfAir).
      if (!g || !g->strike.has) return 0;
      const uint32_t m = g->strike.boltMat ? g->strike.boltMat : g->strike.splashMat;
      const int32_t full = SatMul(vol, SatMul(std::max(1, lib.Arcane(m)), b.ratePlace));
      return std::max<int32_t>(
          1, (int32_t)(((int64_t)full * g->strike.tariffMille + 500) / 1000));
    }
    case SpellVerb::Trail: {
      int32_t t = 0;
      for (const EffectInst& i : e.inner) t = SatAdd(t, EffectTariffIn(lib, i, withCarry));
      return t;
    }
    case SpellVerb::Sustain:
      // Billed per tick as it runs (P3), never up front.
      return 0;
    case SpellVerb::Repeat: {
      int32_t t = 0;
      for (const EffectInst& i : e.inner) t = SatAdd(t, EffectTariffIn(lib, i, withCarry));
      return SatMul(t, EchoRepeats(g, e));
    }
    case SpellVerb::Launch: {
      // THE PRICE IS RECURSIVE, and carry composes MULTIPLICATIVELY down the
      // tree: a bolt that fires a bolt pays 3.0 x 3.0 on what the inner one
      // finally does, which is exactly the "you are paying to move it twice"
      // reading the carry premium exists to give.
      if (e.launch.empty()) return 0;
      const DeliveryRec& d = e.launch[0];
      int32_t shared = 0;
      // M3: an item priced x how many times it fires in THIS carrier - a
      // blast at every bounce of a three-bounce bolt is three blasts. x1 for
      // every item on hit, which is every item that existed before triggers.
      for (const EffectInst& i : e.inner)
        shared = SatAdd(shared, SatMul(EffectTariffIn(lib, i, withCarry), FiresIn(d, i)));
      // RULE 4: SUM over instances, not the shared tariff times the fan. The
      // two are the same number when there are no lanes, which is why every
      // pinned price of a laneless sentence is unmoved.
      const int32_t inst = RecInstances(lib, d);
      const int32_t L = std::min((int32_t)d.lanes.size(), inst);
      int32_t t = 0;
      for (int32_t i = 0; i < L; i++) {
        int32_t li = shared;
        for (const EffectInst& x : d.lanes[i].extra)
          li = SatAdd(li, SatMul(EffectTariffIn(lib, x, withCarry), FiresIn(d.lanes[i].rec, x)));
        // ...TIMES THAT BRANCH'S OWN SPLIT: a branch that fires three of itself
        // is paid for three times. 1 when nothing inside the lane fans, which
        // is every sentence that existed before per-branch splitting.
        t = SatAdd(t, SatMul(SatAdd(li, TrailTariffOf(lib, d.lanes[i].rec, withCarry)),
                             LaneSplit(lib, d, i)));
      }
      if (inst > L)
        t = SatAdd(t, SatMul(SatAdd(shared, TrailTariffOf(lib, d, withCarry)), inst - L));
      if (withCarry) {
        const int64_t v = (int64_t)t * d.carryMille / 1000;
        t = v > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)v;
      }
      return t;
    }
    default: return 0;
  }
}

int32_t TrailTariffOf(const GlyphLibrary& lib, const DeliveryRec& d, bool withCarry) {
  if (d.trail.empty()) return 0;
  int32_t markVol = 0, markTariff = 0;
  for (const EffectInst& e : d.trail) {
    markVol = SatAdd(markVol, EffectVolume(lib, e));
    markTariff = SatAdd(markTariff, EffectTariffIn(lib, e, withCarry));
  }
  const int32_t marks = d.trailBudget / std::max(1, markVol);
  return SatMul(marks, markTariff);
}

}  // namespace

int32_t EffectTariff(const GlyphLibrary& lib, const EffectInst& e) {
  return EffectTariffIn(lib, e, true);
}

void PriceCast(const GlyphLibrary& lib, SpellCast& cast) {
  int32_t shared = 0, sharedC = 0;
  for (const EffectInst& e : cast.payload) {
    shared = SatAdd(shared, EffectTariffIn(lib, e, false));
    sharedC = SatAdd(sharedC, EffectTariffIn(lib, e, true));
  }
  const int32_t inst = std::max(1, cast.instances);
  const int32_t L = std::min((int32_t)cast.delivery.lanes.size(), inst);
  // RULE 4: tariff = SUM over instances of tariff(shared ∪ lane_i). The lanes
  // beyond L are shared-only copies, so this is the old product exactly when
  // there are no lanes at all.
  int32_t base = 0, total = 0;
  for (int32_t i = 0; i < L; i++) {
    const SpellLane& ln = cast.delivery.lanes[i];
    int32_t lb = shared, lt = sharedC;
    for (const EffectInst& e : ln.extra) {
      lb = SatAdd(lb, EffectTariffIn(lib, e, false));
      lt = SatAdd(lt, EffectTariffIn(lib, e, true));
    }
    // The lane's record already carries the shared trail merged with its own,
    // and a branch that splits is paid for once per bolt it makes.
    const int32_t split = LaneSplit(lib, cast.delivery, i);
    base = SatAdd(base, SatMul(SatAdd(lb, TrailTariffOf(lib, ln.rec, false)), split));
    total = SatAdd(total, SatMul(SatAdd(lt, TrailTariffOf(lib, ln.rec, true)), split));
  }
  if (inst > L) {
    base = SatAdd(base, SatMul(SatAdd(shared, TrailTariffOf(lib, cast.delivery, false)),
                               inst - L));
    total = SatAdd(total, SatMul(SatAdd(sharedC, TrailTariffOf(lib, cast.delivery, true)),
                                 inst - L));
  }
  // A held beam pays the tariff every tick it is held (P3 bills it as it
  // emits); the up-front price is one resolve.
  cast.tariff = base;
  // The delivery premium: carry is per-mille on the payload tariff, and the
  // part above x1 is what the HUD shows as "carry" — including every nested
  // carrier's premium, which is where the cost of a nested spell lives.
  const int64_t withCarry = (int64_t)total * cast.delivery.carryMille / 1000;
  const int64_t premium = withCarry - (int64_t)cast.tariff;
  cast.carryCost = premium <= 0 ? 0 : (premium > 0x3FFFFFFF ? 0x3FFFFFFF : (int32_t)premium);
}

SpellCast InstanceCast(const SpellCast& cast, int32_t i) {
  // The fast, and overwhelmingly common, path: no lanes, so every instance is
  // the cast itself and nothing about a laneless sentence changes.
  if (cast.delivery.lanes.empty()) return cast;
  SpellCast out = cast;
  const int32_t L = (int32_t)cast.delivery.lanes.size();
  if (i < 0 || i >= L) {
    // A shared-only COPY: it fans, and it must not carry the lane list on to
    // whatever it launches, or the fan would happen twice. Nor its count: AN
    // INSTANCE IS ONE OF IT, and leaving the box's fan on a record that has
    // already been fanned is a squared fan waiting for the first reader.
    out.delivery.lanes.clear();
    out.delivery.count = 1;
    return out;
  }
  const SpellLane& ln = cast.delivery.lanes[i];
  out.delivery = ln.rec;         // already laneless by construction
  out.delivery.lanes.clear();
  out.delivery.count = 1;
  for (const EffectInst& e : ln.extra) out.payload.push_back(e);
  return out;
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

namespace {

// LOWER ONE BOX (rule 2), recursively. A box is a delivery record, the mods
// that stuck to it, and a payload whose items may themselves be boxes — so
// this is the whole fold, and the `hand` box at the root is just the outermost
// call.
SpellCast LowerBox(const GlyphLibrary& lib, const SpellTree& tree, int node,
                   std::vector<BoxPrice>* prices) {
  const SpellBudgets& b = lib.budgets;
  const SpellNode& bn = tree.nodes[node];
  const bool isHand = bn.glyph < 0;
  // RULE 4: how many segments this box closed (`laneAt` holds the `lane` and
  // the `end` position of each). L may be cut back below by the leaf cap, so
  // it is not final until the clamp.
  int32_t L = (int32_t)bn.laneAt.size() / 2;
  SpellCast cast;
  cast.delivery = RecordFor(lib, bn.glyph, bn.n);
  cast.wordCost = SatMul(lib.Delivery(bn.glyph).word, bn.n);

  // RULE 1 IN THE LOWERING: the pile is a set, so the record is built from it
  // in a CANONICAL order (by key), not the spoken one. Mods compose and
  // integer arithmetic does not commute under clamping (49 halved then doubled
  // is 48), so without this `swift slow` and `slow swift` would lower to two
  // different records and law L2 would be false. RULE 4 narrows the claim: the
  // order is canonical WITHIN a segment, and the segments keep the order they
  // were spoken in, because lane 1 is instance 0 and the player put it there.
  std::vector<int> bag = bn.items;
  std::stable_sort(bag.begin(), bag.end(), [&](int a, int bb) {
    const int32_t la = tree.nodes[a].lane, lb = tree.nodes[bb].lane;
    if (la != lb) return la < lb;
    return NodeKey(lib, tree, a) < NodeKey(lib, tree, bb);
  });

  // A box owns every word inside it, wherever in the pile it was spoken.
  for (int ni : bag) cast.wordCost = SatAdd(cast.wordCost, WordCostOf(lib, tree, ni));

  // ONE Mod item onto one record (rule 3 decides whether it means anything,
  // rule 4 decides WHICH record). A word that edits nothing is charged and
  // named once, on the cast, whichever segment it was spoken in.
  auto applyMod = [&](DeliveryRec& rec, int ni) {
    const SpellNode& n = tree.nodes[ni];
    const GlyphDef& g = lib.glyphs[n.glyph];
    if (!n.group) {
      if (ModMeansAnything(g.field, rec.mech, isHand))
        ApplyMod(rec, g, n.n, b, n.mag);
      else
        cast.wastedMods.push_back(n.glyph);
      return;
    }
    // A trail group: E <trail -> a Mod. Runs its Effect at every marked voxel
    // of the flight path, under a hard voxel budget (rule 2). Only a FLIGHT
    // travels, so on any other record it is a charged no-op.
    if (g.verb != SpellVerb::Trail || !n.complete) return;
    if (rec.mech != DeliveryMech::Flight) {
      cast.wastedMods.push_back(n.glyph);
      return;
    }
    EffectInst tr = LowerEffect(lib, tree, ni, true, prices);
    for (EffectInst& i : tr.inner) rec.trail.push_back(i);
    rec.trailBudget =
        std::min(SatAdd(rec.trailBudget, ScaleMille(g.voxelBudget, SatMul(n.n, n.mag), 1)), b.maxTrailVoxels);
    rec.trailEvery = g.everyTicks;
  };
  auto isMod = [&](int ni) { return NodeSort(lib, tree, ni) == GlyphSort::Mod; };
  auto isCount = [&](int ni) {
    const SpellNode& n = tree.nodes[ni];
    return !n.group && lib.glyphs[n.glyph].field == ModField::Count;
  };

  // ONE SPLIT PER SCOPE, AND THE STRONGEST WORD WINS (2026-09-22).
  //
  // Count mods used to COMPOSE like every other field: `shotgun shotgun` was
  // count x9 and `shotgun` three times was 27. That made the fan a number you
  // stacked rather than a branch point you placed, and it is the opposite of
  // what a split is for - if you want nine, you put a shotgun on each of the
  // three branches, and the drawing shows you that you did.
  //
  // So: within one SCOPE - the shared segment, or one lane - at most one count
  // word is effective, and it applies EXACTLY ONCE however many times it was
  // said (`n` is ignored, which is why `ApplyMod` is called with 1 here). The
  // winner is the word whose application yields the LARGEST count, ties broken
  // by the lowest `NodeKey`.
  //
  // Largest-then-key rather than first-spoken, because `bag` is sorted by
  // (lane, NodeKey) precisely so that lowering does not depend on spoken order
  // - law L2 says the pile is a SET. A "first spoken" rule would put the order
  // back. Largest-result is a property of the word, not of where it stands, it
  // is total over any count glyph anybody adds later, and it reads in one
  // sentence: the strongest count word in a scope wins. `shotgun twin` and
  // `twin shotgun` both give three, and both waste the `twin`.
  //
  // Everything that loses is WASTED: charged, named in the readout, drawn
  // slashed. By TREE NODE, not by glyph - `wastedMods` is a glyph list and
  // slashing `shotgun` by glyph would slash the one that is working too.
  auto applyCount = [&](DeliveryRec& rec, int32_t scopeLane) {
    int win = -1;
    int32_t best = rec.count;
    for (int ni : bag) {
      if (!isMod(ni) || !isCount(ni) || tree.nodes[ni].lane != scopeLane) continue;
      if (!ModMeansAnything(ModField::Count, rec.mech, isHand)) {
        cast.wastedMods.push_back(tree.nodes[ni].glyph);
        continue;
      }
      DeliveryRec probe = rec;
      ApplyMod(probe, lib.glyphs[tree.nodes[ni].glyph], 1, b);
      if (win < 0 || probe.count > best) {
        if (win >= 0) cast.wastedNodes.push_back(win);
        win = ni;
        best = probe.count;
      } else {
        cast.wastedNodes.push_back(ni);
      }
    }
    if (win >= 0) ApplyMod(rec, lib.glyphs[tree.nodes[win].glyph], 1, b);
  };

  // A COUNT MOD SPLITS THE SCOPE IT WAS SPOKEN IN (2026-09-22). It used to be
  // RECORD-WIDE wherever it was spoken - `shotgun` inside a lane trebled the
  // whole box - which made a fan a property of the box and nothing else, and
  // left no way to say "this branch splits and that one does not". A count in
  // the shared segment is still the box's own fan; a count inside lane k is
  // that BRANCH's split, applied to the lane record down in the lane loop. Put
  // three shotguns on three branches and you get nine, because you put them
  // there.
  applyCount(cast.delivery, 0);
  // The shared segment's other mods.
  for (int ni : bag)
    if (isMod(ni) && !isCount(ni) && tree.nodes[ni].lane == 0) applyMod(cast.delivery, ni);

  // The shared payload. A nested BOX becomes one Launch value carrying its own
  // record and payload; it was priced and clamped by its own call, and this one
  // only adds its leaves and its depth to the totals.
  int32_t childLeaves = 0, childDepth = 0;
  auto lowerNouns = [&](int32_t lane, std::vector<EffectInst>& into, int32_t radiusMille,
                        int32_t& leaves, int32_t& depth) {
    for (int ni : bag) {
      if (tree.nodes[ni].lane != lane) continue;
      const GlyphSort s = NodeSort(lib, tree, ni);
      if (s != GlyphSort::Effect && s != GlyphSort::Matter) continue;
      if (tree.nodes[ni].box) {
        SpellCast child = LowerBox(lib, tree, ni, prices);
        EffectInst e;
        e.verb = SpellVerb::Launch;
        e.glyph = tree.nodes[ni].glyph;
        e.n = tree.nodes[ni].n;
        // WHEN the box fires in its carrier (M3). `LaunchEffectOf` (a box
        // under an operator) always carried it; a box standing in a bag lost
        // it here, so `projectile+20` / `projectile!launch` as an item parsed,
        // round-tripped, drew its tag on the page and then fired on hit, now.
        e.timing = tree.nodes[ni].timing;
        e.node = ni;
        e.inner = child.payload;
        e.launch.push_back(child.delivery);
        leaves = SatAdd(leaves, child.leaves);
        depth = std::max(depth, child.depth);
        cast.priceUnknown = cast.priceUnknown || child.priceUnknown;
        cast.instancesClamped = cast.instancesClamped || child.instancesClamped;
        // A CHILD BOX'S WASTED WORDS ARE THE SENTENCE'S WASTED WORDS. A nested
        // box's cast becomes one Launch value and is otherwise dropped, so
        // until 2026-09-22 a mod wasted inside a spoken delivery was never
        // named in the readout and never slashed on the page - the two lists
        // only ever carried what the HAND wasted. Both are per-sentence facts
        // and both belong to the cast the player is looking at.
        cast.wastedMods.insert(cast.wastedMods.end(), child.wastedMods.begin(),
                               child.wastedMods.end());
        cast.wastedNodes.insert(cast.wastedNodes.end(), child.wastedNodes.begin(),
                                child.wastedNodes.end());
        into.push_back(std::move(e));
        continue;
      }
      EffectInst e = LowerEffect(lib, tree, ni, false, prices);
      ScaleEffectRadius(e, radiusMille);
      if (e.anyA || e.anyB) cast.priceUnknown = true;
      for (const EffectInst& i : e.inner)
        if (i.anyA || i.anyB) cast.priceUnknown = true;
      into.push_back(std::move(e));
    }
  };
  lowerNouns(0, cast.payload, cast.delivery.radiusMille, childLeaves, childDepth);
  for (EffectInst& tr : cast.delivery.trail) {
    ScaleEffectRadius(tr, cast.delivery.radiusMille);
    if (tr.anyA) cast.priceUnknown = true;
  }

  // RULE 4: ONE LANE PER SEGMENT. The lane's record is the shared record with
  // the lane's own Mods applied on top and its trail merged in; its `extra` is
  // what only that instance carries. A `wide` inside a lane widens that lane's
  // nouns (it edited that instance's record), not the shared ones - the shared
  // payload is one object every instance reads.
  int32_t laneLeaves = 0, laneDepth = 0;
  const size_t sharedTrail = cast.delivery.trail.size();
  for (int32_t k = 1; k <= L; k++) {
    SpellLane ln;
    ln.rec = cast.delivery;
    ln.rec.lanes.clear();
    // A BRANCH DOES NOT INHERIT THE FAN IT IS A BRANCH OF. `count` on a lane
    // record means that branch's own split, so the box's count has to be reset
    // here or it would be counted twice - once as the branch, once as the
    // branch's split. Nothing read a lane record's count before this day.
    ln.rec.count = 1;
    // This lane's own mods. A count among them is this BRANCH's split, and
    // `ln.rec.count` was reset to 1 just above so it starts from "one of it"
    // rather than from the box's fan; one of them wins and the rest are wasted,
    // exactly as in the shared segment.
    for (int ni : bag)
      if (isMod(ni) && !isCount(ni) && tree.nodes[ni].lane == k) applyMod(ln.rec, ni);
    applyCount(ln.rec, k);
    // Only the trail entries this lane added need the lane's own radius; the
    // shared ones were scaled above, by the shared record's.
    for (size_t ti = sharedTrail; ti < ln.rec.trail.size(); ti++) {
      ScaleEffectRadius(ln.rec.trail[ti], ln.rec.radiusMille);
      if (ln.rec.trail[ti].anyA) cast.priceUnknown = true;
    }
    int32_t lLeaves = 0, lDepth = 0;
    lowerNouns(k, ln.extra, ln.rec.radiusMille, lLeaves, lDepth);
    // The leaf bound takes the WIDEST lane: the instances of this box are not
    // all the same, and rule 2 wants the bound that cannot be exceeded.
    laneLeaves = std::max(laneLeaves, lLeaves);
    laneDepth = std::max(laneDepth, lDepth);
    cast.delivery.lanes.push_back(std::move(ln));
  }
  childLeaves = SatAdd(childLeaves, laneLeaves);
  childDepth = std::max(childDepth, laneDepth);

  // THE LEAF CAP (rule 2). Instances multiply down the tree - three bolts
  // each firing three is nine - so the bound that matters is the number of
  // LEAF instances the whole tree can produce, not the fan at one level. Clamp
  // this box's own fan so the product still fits, and say so in the readout
  // rather than quietly firing fewer than the sentence asked for. A lane past
  // the clamp goes with it: an instance that cannot fire cannot carry a lane.
  if (childLeaves < 1) childLeaves = 1;
  cast.instances = RecInstances(lib, cast.delivery);
  // `maxOwn` is a BOLT budget, not a branch budget: what has to stay bounded is
  // what flies, and a branch that splits flies more than once.
  const int32_t maxOwn = std::max(1, b.maxInstances / childLeaves);
  // (a) CUT THE SPLITS BACK FIRST, LAST BRANCH FIRST, one bolt at a time.
  // Tail-first because lane 1 is instance 0 is the aim: dropping a bolt off the
  // last branch moves nothing the player aimed, and cutting from the front
  // would renumber every branch after it. One step at a time so the result is a
  // pure function of the list - no division, no float, no ordering choice - and
  // bounded, because `ApplyMod` already clamps each split to `maxInstances`.
  {
    int32_t bolts = RecBolts(lib, cast.delivery);
    for (int32_t k = (int32_t)cast.delivery.lanes.size() - 1; bolts > maxOwn && k >= 0;
         k--) {
      while (bolts > maxOwn && cast.delivery.lanes[(size_t)k].rec.count > 1) {
        cast.delivery.lanes[(size_t)k].rec.count--;
        bolts--;
        cast.instancesClamped = true;
      }
    }
  }
  // (b) ...and only then the BRANCH count, exactly as before.
  if (cast.instances > maxOwn) {
    cast.instances = maxOwn;
    cast.delivery.count = std::min(cast.delivery.count, maxOwn);
    cast.instancesClamped = true;
  }
  if ((int32_t)cast.delivery.lanes.size() > cast.instances) {
    cast.delivery.lanes.resize((size_t)cast.instances);
    cast.instancesClamped = true;
  }
  L = (int32_t)cast.delivery.lanes.size();
  (void)L;
  cast.bolts = RecBolts(lib, cast.delivery);
  cast.leaves = ClampI(SatMul(cast.bolts, childLeaves), 1, b.maxInstances);
  cast.depth = childDepth + (isHand ? 0 : 1);
  cast.generation = 0;
  PriceCast(lib, cast);

  // Rule 2 budgets, all finite. The volume bound is the WIDEST instance times
  // the fan, and the clock the longest-lived one's.
  int32_t ticks = 1;
  switch (cast.delivery.mech) {
    case DeliveryMech::Instant: ticks = 1; break;
    case DeliveryMech::Flight:
      ticks = SatAdd(cast.delivery.lifetimeTicks, cast.delivery.fuseTicks);
      break;
    case DeliveryMech::Continuous: ticks = cast.delivery.lifetimeTicks; break;
  }
  int32_t shared = 0;
  for (const EffectInst& e : cast.payload) {
    shared = SatAdd(shared, EffectVolume(lib, e));
    ticks = std::max(ticks, EffectTicks(lib, e));
  }
  int32_t voxels = SatAdd(shared, cast.delivery.trailBudget);
  for (const SpellLane& ln : cast.delivery.lanes) {
    int32_t lv = shared;
    for (const EffectInst& e : ln.extra) {
      lv = SatAdd(lv, EffectVolume(lib, e));
      ticks = std::max(ticks, EffectTicks(lib, e));
    }
    voxels = std::max(voxels, SatAdd(lv, ln.rec.trailBudget));
    ticks = std::max(ticks, ln.rec.mech == DeliveryMech::Flight
                                ? SatAdd(ln.rec.lifetimeTicks, ln.rec.fuseTicks)
                                : ln.rec.lifetimeTicks);
  }
  if (cast.delivery.mech == DeliveryMech::Continuous)
    voxels = SatMul(voxels, cast.delivery.lifetimeTicks);
  cast.voxels = SatMul(voxels, cast.bolts);
  cast.ticks = ticks;
  if (prices) {
    BoxPrice bp;
    bp.node = node;
    bp.wordCost = cast.wordCost;
    bp.tariff = cast.tariff;
    bp.carryCost = cast.carryCost;
    bp.instances = cast.instances;
    bp.bolts = cast.bolts;
    bp.laneSplits.clear();
    for (int32_t k = 0; k < cast.instances; k++)
      bp.laneSplits.push_back(LaneSplit(lib, cast.delivery, k));
    bp.leaves = cast.leaves;
    bp.instancesClamped = cast.instancesClamped;
    prices->push_back(bp);
  }
  return cast;
}

}  // namespace

// ---- page calls dissolve before lowering ----------------------------------------

namespace {

// The single item a value call stands for in an operator's slot (the parser
// only lets a one-output call in), flattened; -1 when it has none.
int FlattenNode(const GlyphLibrary& lib, SpellTree& t, int node, int depth);

int SlotItem(const GlyphLibrary& lib, SpellTree& t, int node, int depth) {
  if (node < 0) return -1;
  if (!IsValueCall(t, node)) return FlattenNode(lib, t, node, depth);
  FlattenNode(lib, t, node, depth);
  const SpellNode& c = t.nodes[(size_t)node];
  return c.items.size() == 1 ? c.items[0] : -1;
}

// Rewire `node`'s children so no call is left under it. Returns `node`.
int FlattenNode(const GlyphLibrary& lib, SpellTree& t, int node, int depth) {
  if (node < 0 || node >= (int)t.nodes.size() || depth > 64) return node;
  if (t.nodes[(size_t)node].group) {
    const int l = SlotItem(lib, t, t.nodes[(size_t)node].left, depth + 1);
    const int r = SlotItem(lib, t, t.nodes[(size_t)node].right, depth + 1);
    t.nodes[(size_t)node].left = l;
    t.nodes[(size_t)node].right = r;
    if (l >= 0) t.nodes[(size_t)l].lane = 0;
    if (r >= 0) t.nodes[(size_t)r].lane = 0;
    return node;
  }
  // A box, a value call's body, or a leaf (no items: nothing to do).
  std::vector<int> in = t.nodes[(size_t)node].items;
  // A CARRIER's own mods join its pile as shared items.
  if (t.nodes[(size_t)node].box && !t.nodes[(size_t)node].callItems.empty()) {
    for (int ii : t.nodes[(size_t)node].callItems) {
      t.nodes[(size_t)ii].lane = 0;
      in.push_back(ii);
    }
    t.nodes[(size_t)node].callItems.clear();
  }
  std::vector<int> out;
  for (int ii : in) {
    if (!IsValueCall(t, ii)) {
      out.push_back(FlattenNode(lib, t, ii, depth + 1));
      continue;
    }
    // A VALUE CALL SPLICES ITS BODY into this pile. Standing in the shared
    // segment, the body's own lanes become new lanes of this box (a page of
    // two columns is two columns wherever it is spoken); standing in a lane,
    // everything goes into that lane, which cannot hold lanes of its own.
    FlattenNode(lib, t, ii, depth + 1);
    const SpellNode c = t.nodes[(size_t)ii];
    const int32_t at = c.lane;
    const int32_t bodyLanes = (int32_t)c.laneAt.size() / 2;
    const int32_t base = (int32_t)t.nodes[(size_t)node].laneAt.size() / 2;
    if (at == 0 && bodyLanes > 0)
      for (int32_t k = 0; k < bodyLanes; k++) {
        t.nodes[(size_t)node].laneAt.push_back(-1);
        t.nodes[(size_t)node].laneAt.push_back(-1);
      }
    for (int bi : c.items) {
      const int32_t from = t.nodes[(size_t)bi].lane;
      t.nodes[(size_t)bi].lane = at == 0 ? (from > 0 ? base + from : 0) : at;
      out.push_back(bi);
    }
  }
  t.nodes[(size_t)node].items = out;
  return node;
}

}  // namespace

SpellTree FlattenCalls(const GlyphLibrary& lib, const SpellTree& tree) {
  SpellTree t = tree;
  bool any = false;
  for (const SpellNode& n : t.nodes) any = any || !n.call.empty();
  if (!any) return t;
  for (SpellClause& c : t.clauses) {
    if (c.root < 0) continue;
    FlattenNode(lib, t, c.root, 0);
    c.bag = t.nodes[(size_t)c.root].items;
  }
  return t;
}

void SpeakWordsOnto(const GlyphLibrary& lib, const PageLookup& find,
                    const std::vector<std::string>& words, int maxWords, SpellStack& out,
                    PageExpansionReport& report) {
  std::vector<std::string> trail;
  std::function<void(const std::vector<std::string>&, int)> speak =
      [&](const std::vector<std::string>& ws, int depth) {
        for (const std::string& w : ws) {
          if (out.Words() >= maxWords) {
            report.truncated = true;
            return;
          }
          int32_t mag = kMagOne;
          SpellTiming tm;
          const int gi = ParseWord(lib, w, mag, tm);
          if (gi >= 0) {
            out.Push(gi, mag, tm);
            report.readout.push_back(w);
            continue;
          }
          const std::vector<std::string>* body = find ? find(w) : nullptr;
          if (!body) {
            // Content removed or renamed: the word drops with a log line and
            // the readout shows `?`; the page is not deleted (DESIGN §8b).
            report.dropped++;
            report.readout.push_back("?");
            std::fprintf(stderr, "grimoire: \"%s\" names nothing that exists; dropped\n",
                         w.c_str());
            continue;
          }
          bool cycle = false;
          for (const std::string& tr : trail) cycle = cycle || tr == w;
          if (cycle || depth + 1 > lib.budgets.maxMacroDepth) {
            // A cycle a save check missed (content edited under it), or a nest
            // past the cap: the name speaks nothing rather than everything.
            report.tooDeep = true;
            report.readout.push_back("?");
            continue;
          }
          // A COPY: a lookup may hand back a scratch buffer the next lookup
          // (inside the recursion) overwrites.
          const std::vector<std::string> said = *body;
          const int k = out.book.Add(w, said);
          out.Push(kSpokenPageOpen0 - k);
          trail.push_back(w);
          speak(said, depth + 1);
          trail.pop_back();
          // ALWAYS closed, even cut short: an unbalanced page would swallow the
          // rest of the sentence into itself.
          out.Push(kSpokenPageClose);
          if (report.truncated) return;
        }
      };
  speak(words, 0);
}

void AppendStack(SpellStack& dst, const SpellStack& src) {
  for (size_t k = 0; k < src.spoken.size(); k++) {
    int s = src.spoken[k];
    if (SpokenIsPageOpen(s)) {
      const int pi = SpokenPageIndex(s);
      if (pi >= 0 && pi < (int)src.book.names.size())
        s = kSpokenPageOpen0 - dst.book.Add(src.book.names[(size_t)pi], src.book.words[(size_t)pi]);
    }
    dst.Push(s, src.MagAt(k), src.TimingAt(k));
  }
}

CastList LowerSpell(const GlyphLibrary& lib, const SpellTree& tree) {
  CastList list;
  // THE TREE KEPT IS THE ONE THAT WAS SPOKEN, page calls and all - the page
  // draws a call as one cell - and the one LOWERED is the same arena with the
  // calls dissolved, so every box price is keyed by a node of both.
  list.tree = tree;
  const SpellTree flat = FlattenCalls(lib, tree);
  for (size_t ci = 0; ci < flat.clauses.size(); ci++) {
    if (flat.clauses[ci].root < 0) continue;
    SpellCast cast = LowerBox(lib, flat, flat.clauses[ci].root, &list.boxPrice);
    cast.clause = (int)ci;
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
//
// One recursive sentence per cast, in the shape the fold has: "You fire a
// bomb; when its fuse runs down, it fires a bolt; when that hits, it explodes
// and sprays fire." Nothing here knows a glyph by name -- the carrier's noun,
// the material names and the verbs all come out of glyphs.json, so a modder's
// new word reads as itself.

namespace {

std::string MatName(const GlyphLibrary& lib, int glyph, uint32_t mat, bool any) {
  if (any) return "whatever is there";
  const GlyphDef* g = lib.At(glyph);
  if (g && !g->materialName.empty()) return g->materialName;
  if (mat == 0) return "nothing";
  return "material " + std::to_string(mat);
}

std::string Capitalize(const std::string& s) {
  if (s.empty()) return s;
  std::string o = s;
  o[0] = (char)toupper((unsigned char)o[0]);
  return o;
}

std::string LaunchSentence(const GlyphLibrary& lib, const EffectInst& e,
                           const std::string& that);
std::string LaneSentence(const GlyphLibrary& lib, const DeliveryRec& d, int32_t i);

// A short third-person verb phrase for one payload item: what it DOES where it
// lands. Derived from the verb, never from the glyph id.
std::string EffectPhraseBase(const GlyphLibrary& lib, const EffectInst& e);
// What an item does, and WHEN when that is not "on hit" (M3).
std::string EffectPhrase(const GlyphLibrary& lib, const EffectInst& e) {
  std::string s = EffectPhraseBase(lib, e);
  if (!e.timing.IsDefault()) s += " (" + TimingPhrase(e.timing) + ")";
  return s;
}

std::string EffectPhraseBase(const GlyphLibrary& lib, const EffectInst& e) {
  const GlyphDef* g = lib.At(e.glyph);
  const std::string id = g ? g->id : "?";
  if (!e.complete) return "wastes `" + id + "` (a word it needed was missing)";
  std::string times = e.n > 1 ? " x" + std::to_string(e.n) : std::string();
  // A MAGNITUDE reads as the number it produces ("power 330"), not as a
  // multiplier on a multiplier. Absent at 1, so every old line is unchanged.
  if (e.mag != kMagOne) {
    const std::string lbl = MagnitudeLabel(lib, e.glyph, e.n, e.mag);
    if (!lbl.empty()) times = " (" + lbl + ")";
  }
  switch (e.verb) {
    case SpellVerb::Launch: return "fires " + LaunchSentence(lib, e, "that");
    case SpellVerb::Spray:
      if (e.anyA) return "scoops up whatever is there and throws it";
      if (e.matA == 0) return "throws nothing";
      return "sprays " + MatName(lib, e.glyphA, e.matA, false) + times;
    case SpellVerb::Place:
      return "lays " + MatName(lib, e.glyphA, e.matA, e.anyA) + times;
    case SpellVerb::Convert: {
      const std::string a = MatName(lib, e.glyphA, e.matA, e.anyA);
      const std::string bb = MatName(lib, e.glyphB, e.matB, e.anyB);
      if (!e.anyA && !e.anyB && e.matA == e.matB) return "converts " + a + " to itself: nothing";
      if (!e.anyA && e.matA == 0) return "conjures " + bb + " into empty space" + times;
      if (!e.anyB && e.matB == 0) return "unmakes " + a + times;
      return "turns " + a + " into " + bb + times;
    }
    case SpellVerb::Explode: return "explodes" + times;
    case SpellVerb::Wind: return "blows a wind jet along the aim" + times;
    case SpellVerb::Strike: {
      const GlyphDef* sg = lib.At(e.glyph);
      if (sg && sg->strike.height > 0 && sg->strike.boltMat != 0)
        return "calls down a bolt of lightning on the tallest conductor near it" + times;
      return "goes off in a crackle of arcs" + times;
    }
    case SpellVerb::Mend:
      return "draws " + MatName(lib, e.glyphA, e.matA, e.anyA) +
             " into the caster's missing anatomy";
    case SpellVerb::Sustain: {
      std::string what;
      if (e.modField != ModField::None)
        what = std::string("the mod ") + ModFieldName(e.modField);
      else if (!e.inner.empty())
        what = EffectPhrase(lib, e.inner[0]);
      else
        what = "nothing";
      return "sustains [" + what + "] on whatever it lands on, every tick, billed per tick";
    }
    case SpellVerb::Filter: {
      const GlyphDef* w = e.inner.empty() ? nullptr : lib.At(e.inner[0].glyph);
      return "refuses incoming " + (w ? w->id : std::string("?")) + " for a moment";
    }
    case SpellVerb::Repeat:
      return "repeats [" +
             (e.inner.empty() ? std::string("nothing") : EffectPhrase(lib, e.inner[0])) +
             "] every few ticks, a bounded number of times";
    case SpellVerb::Trail:
      return "lays " +
             (e.inner.empty() ? std::string("nothing") : EffectPhrase(lib, e.inner[0])) +
             " along a path it does not have";
    default: return id;
  }
}

// What the mods on a record did to it, as adjectives before the noun and a
// clause after the trigger. Read off the RECORD against the glyph's own
// defaults, so a modder's new mod on an old field describes itself.
void RecordWords(const GlyphLibrary& lib, const DeliveryRec& d, std::string& adj,
                 std::string& post, bool& fan) {
  const GlyphDef& g = lib.Delivery(d.glyph);
  fan = d.count > 1;
  auto add = [&](const char* w) {
    if (!adj.empty()) adj += " ";
    adj += w;
  };
  if (d.speedFx > g.speedFx) add("fast");
  else if (d.speedFx < g.speedFx) add("slow");
  if (d.gravityMille < g.gravityMille) add("floating");
  else if (d.gravityMille > g.gravityMille) add("heavy");
  if (d.lifetimeTicks > g.lifetimeTicks) add("long-lived");
  if (d.radiusMille > 1000) add("wide");
  if (d.pierce > 0) add("piercing");
  if (d.seek > 0) add("seeking");
  if (d.bounces > 0) post = "bounce";
}

// THE RECURSIVE SENTENCE for one launch box.
std::string LaunchSentence(const GlyphLibrary& lib, const EffectInst& e,
                           const std::string& that) {
  if (e.launch.empty()) return "nothing";
  const DeliveryRec& d = e.launch[0];
  const GlyphDef& g = lib.Delivery(d.glyph);
  std::string adj, post;
  bool fan = false;
  RecordWords(lib, d, adj, post, fan);
  // RULE 4: lanes grow the fan, so the number spoken is the INSTANCE count,
  // not the count mod's.
  const int32_t inst = RecInstances(lib, d);
  fan = inst > 1;
  const std::string noun = g.noun.empty() ? g.id : g.noun;

  std::string head;
  if (fan) {
    head = std::to_string(inst) + " fanned " + (adj.empty() ? "" : adj + " ") + noun + "s";
  } else {
    head = std::string("a ") + (adj.empty() ? "" : adj + " ") + noun;
  }
  const std::string it = fan ? "each" : that;

  // The trail is what it does ON THE WAY, so it rides the noun phrase.
  for (const EffectInst& tr : d.trail)
    head += " that " + EffectPhrase(lib, tr) + " along its whole path";

  std::string trig, join = ", ";
  switch (d.mech) {
    case DeliveryMech::Instant:
      trig = d.glyph < 0 ? "at reach in front of you" : "at once, on the caster";
      break;
    case DeliveryMech::Continuous: trig = "every tick at the ray hit"; break;
    case DeliveryMech::Flight:
      if (d.body) trig = "when its fuse runs down";
      else if (d.resolveOnExpiry) trig = "when it hits or its life runs out";
      else if (!post.empty())
        trig = d.bounces > 1 ? "it bounces " + std::to_string(d.bounces) +
                                   " times, then when it hits again"
                             : "it bounces once, then when it hits again";
      else trig = "when " + it + " hits";
      if (d.fuseTicks > g.fuseTicks) {
        trig += " it waits " + std::to_string(d.fuseTicks) + " ticks, then";
        join = " ";
      }
      break;
  }

  std::vector<std::string> parts;
  for (const EffectInst& p : e.inner) parts.push_back(EffectPhrase(lib, p));
  if (parts.empty())
    parts.push_back(d.mech == DeliveryMech::Flight ? "does nothing but knock what it hit"
                                                   : "does nothing");
  std::string body;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) body += " and ";
    body += parts[i];
  }
  std::string out = head + "; " + trig + join + (fan ? "each " : "it ") + body;
  // And what each lane of THIS carrier carries on top (rule 4), as a trailing
  // clause so the sentence it is embedded in still ends where its caller says.
  for (size_t li = 0; li < d.lanes.size(); li++)
    out += "; " + LaneSentence(lib, d, (int32_t)li);
  return out;
}

// What a Mod word had no field to edit, for the wasted line: the record's own
// vocabulary, never the glyph's name.
const char* MissingOn(ModField f) {
  switch (f) {
    case ModField::Count: return "count";
    case ModField::Gravity: return "weight";
    case ModField::Speed: return "speed";
    case ModField::Lifetime: return "lifetime";
    case ModField::Radius: return "radius";
    case ModField::Bounces: return "surface to bounce off";
    case ModField::Pierce: return "wall to pierce";
    case ModField::Seek: return "target to steer for";
    case ModField::Fuse: return "fuse";
    default: return "field for it";
  }
}

// ORDINALS for the lane lines. Beyond a handful the number reads better than
// a word, and a fan cannot exceed `maxInstances` anyway.
const char* Ordinal(int32_t i) {
  static const char* const kNames[8] = {"first",  "second", "third",   "fourth",
                                        "fifth",  "sixth",  "seventh", "eighth"};
  return (i >= 0 && i < 8) ? kNames[i] : "next";
}

// WHAT ONE LANE CHANGES, as one sentence (rule 4). Read off the lane's record
// against the SHARED one, so only the difference is spoken: "the second bolt
// is fast", "The first bolt also sprays gunpowder."
std::string LaneSentence(const GlyphLibrary& lib, const DeliveryRec& d, int32_t i) {
  const SpellLane& ln = d.lanes[(size_t)i];
  const GlyphDef& g = lib.Delivery(d.glyph);
  std::string noun = g.noun.empty() ? g.id : g.noun;
  if (noun.empty()) noun = "resolve";
  std::string sharedAdj, sharedPost, laneAdj, lanePost;
  bool f0 = false, f1 = false;
  RecordWords(lib, d, sharedAdj, sharedPost, f0);
  RecordWords(lib, ln.rec, laneAdj, lanePost, f1);
  std::vector<std::string> parts;
  if (laneAdj != sharedAdj && !laneAdj.empty()) parts.push_back("is " + laneAdj);
  for (const EffectInst& e : ln.extra) parts.push_back("also " + EffectPhrase(lib, e));
  std::string head = std::string("the ") + Ordinal(i) + " " + noun + " ";
  if (parts.empty()) return head + "carries only what they all carry";
  std::string body;
  for (size_t k = 0; k < parts.size(); k++) {
    if (k) body += " and ";
    body += parts[k];
  }
  return head + body;
}

}  // namespace

std::string DescribeCast(const GlyphLibrary& lib, const SpellCast& cast) {
  const DeliveryRec& d = cast.delivery;
  const GlyphDef& g = lib.Delivery(d.glyph);
  const bool isHand = d.glyph < 0;
  std::string adj, post;
  bool fan = false;
  RecordWords(lib, d, adj, post, fan);
  // Lanes grow the fan (rule 4), so the number in the line is the instance
  // count the cast settled on, cap and all.
  const int32_t inst = std::max(1, cast.instances);
  fan = inst > 1;

  std::vector<std::string> sentences;
  // Nouns first, then the carriers they were spoken beside: the sentence reads
  // outward from the caster, the way the spell happens.
  for (int pass = 0; pass < 2; pass++) {
    for (const EffectInst& e : cast.payload) {
      const bool isLaunch = e.verb == SpellVerb::Launch;
      if ((pass == 0) == isLaunch) continue;
      if (isLaunch) {
        sentences.push_back((fan ? "From " + std::to_string(inst) +
                                       " fanned points in front of you, "
                                 : std::string("You fire ")) +
                            LaunchSentence(lib, e, "it") + ".");
      } else {
        sentences.push_back(Capitalize(EffectPhrase(lib, e)) +
                            (isHand ? " right in front of you" : " on the caster") +
                            (fan ? " at " + std::to_string(inst) + " fanned points" : "") +
                            ".");
      }
    }
  }
  if (cast.payload.empty())
    sentences.push_back(isHand ? "Nothing happens; the words are charged."
                               : "Nothing happens.");
  // A gravity mod on an anchored delivery acts on the caster's body.
  if (d.gravityMille != g.gravityMille && d.mech == DeliveryMech::Instant)
    sentences.push_back(d.gravityMille < g.gravityMille ? "You hop." : "You are shoved down.");
  // A COUNT WASTED BY POSITION, not by field: a second split word in a scope
  // that already fans. Named once however many lost, because the sentence the
  // player needs is "you already split this", not a list.
  if (!cast.wastedNodes.empty())
    sentences.push_back(
        std::string("A second fan word is wasted: one split per scope, and the "
                    "strongest wins. Put it on a branch instead."));
  for (int gi : cast.wastedMods) {
    const GlyphDef* w = lib.At(gi);
    if (!w) continue;
    if (w->sort == GlyphSort::Operator)
      sentences.push_back("The trail is wasted: your hand does not travel anywhere.");
    else
      sentences.push_back("`" + w->id + "` is wasted: it landed on your hand, which has no " +
                          MissingOn(w->field) + ".");
  }
  // RULE 4: one line per lane, after the shared sentence, because a lane is a
  // difference from what they all do and reads as one.
  for (size_t li = 0; li < cast.delivery.lanes.size(); li++)
    sentences.push_back(Capitalize(LaneSentence(lib, cast.delivery, (int32_t)li)) + ".");
  if (cast.instancesClamped)
    sentences.push_back("The fan was cut back to " + std::to_string(cast.leaves) +
                        " in all: past that the instance cap refuses it.");
  if (cast.priceUnknown)
    sentences.push_back("The price is not known until it lands.");
  std::string out;
  for (size_t i = 0; i < sentences.size(); i++) {
    if (i) out += " ";
    out += sentences[i];
  }
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
                      int32_t instabilityMille, uint32_t salt, bool flatten) {
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
    // A DELAYED item (M3) is not applied now: it is scheduled, as a one-shot
    // echo, `delay` ticks out - the same queue `echo` uses, so it is bounded,
    // ticked and op-budgeted by the code that already is. A FLATTENED payload
    // (a fatal cast, a misfire) does everything at once: nothing is deferred
    // out of the caster's chest.
    if (e.timing.delay > 0 && !flatten) {
      SpellEcho ec;
      EffectInst now = e;
      now.timing.delay = 0;
      ec.inner.push_back(std::move(now));
      ec.at = atFx;
      ec.dir = dirFx;
      ec.left = 1;
      ec.period = e.timing.delay;
      ec.instability = instabilityMille;
      out.echoes.push_back(std::move(ec));
      continue;
    }
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
        int64_t n = ScaleMille(b.sprayVoxels, e.Scale(), 1);
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
          // that must land in the grid and stay there. A POWDER spray lands as
          // a CRUMBLE (world.h POWDER ENTERS THE WORLD AS GRAINS).
          s.payload = mat & 0xFFFu;
          if (lib.Crumbles(mat))
            s.payload |= PowderCrumbleState(Hash3((uint32_t)atFx.x, (uint32_t)atFx.y,
                                                  (uint32_t)atFx.z)) << 12;
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
            const int32_t vox = ScaleMille(Cube(e.radius), e.Scale(), 1);
            out.billOnResolve = SatAdd(
                out.billOnResolve,
                SatMul(vox, SatAdd(SatMul(val, b.ratePlace),
                                   (int32_t)((int64_t)val * b.anythingSurchargeMille / 1000))));
          }
        }
        if (mat == 0) break;
        const int32_t r = ClampI(ScaleRadiusCbrtMille(e.radius, e.Scale()), 0, 8);
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
        // The EFFECT's radius, which lowering copied from the glyph and `wide`
        // widened. It used to be max(glyph, effect), which let the glyph's 6
        // override the character screen's clamp to the part (Cast's `selfAt`):
        // a blast "on the hand" was a 1.2 m sphere and took the torso with it.
        const int32_t r0 = e.radius > 0 ? e.radius : (g ? g->radius : 1);
        const int32_t r = ClampI(scaled(ScaleRadiusCbrtMille(r0, e.Scale())), 1,
                                 kMaxExplosionRadius);
        const int32_t power =
            (int32_t)((int64_t)(g ? g->power : 220) * e.Scale() / 1000 * strengthMille / 1000);
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
        const float speed =
            g->wind.speedMs * ((float)e.Scale() / 1000.0f) * ((float)strengthMille / 1000.0f);
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
      case SpellVerb::Strike: {
        // THE VM REPORTS, THE OWNER STRIKES (thesis 1): the bolt needs the
        // mirror (the target search) and the tick's strike budget, neither of
        // which the VM may reach. session.cpp turns this into CellOps through
        // game/lightning.h LightningStrike -- the storm's own strike path.
        if (!g || !g->strike.has) break;
        SpellStrike sk;
        sk.x = cx;
        sk.y = cy;
        sk.z = cz;
        sk.glyph = e.glyph;
        sk.scaleMille = e.Scale();
        sk.strengthMille = strengthMille;
        sk.salt = Hash3(here, 0x5781CEu, (uint32_t)ei);
        out.strikes.push_back(sk);
        break;
      }
      case SpellVerb::Launch: {
        // RULE 2 AT RUNTIME. A box does not reach into the system — it asks,
        // and the system adopts the request at the end of the call. The only
        // channel out of ApplySpellEffect is still `out`.
        if (e.launch.empty()) break;
        if (flatten) {
          // A FATAL CAST FLATTENS: every carrier resolves its own payload
          // where the caster stands, recursively, and nothing leaves the body.
          ApplySpellEffect(lib, e.inner, atFx, dirFx, strengthMille, out, probe,
                           instabilityMille, salt ^ 0xF1A7u, true);
          if (!e.launch[0].trail.empty())
            ApplySpellEffect(lib, e.launch[0].trail, atFx, dirFx, strengthMille, out, probe,
                             instabilityMille, salt ^ 0x7A11u, true);
          break;
        }
        SpellLaunchReq rq;
        rq.delivery = e.launch[0];
        rq.payload = e.inner;
        rq.at = atFx;
        rq.dir = dirFx;
        rq.instability = instabilityMille;
        rq.salt = salt;
        out.launches.push_back(std::move(rq));
        break;
      }
      case SpellVerb::Repeat: {
        // The inner effect, now, and again every `everyTicks` for `repeats`
        // in all: the lowering charged repeats × the effect up front. The
        // rest is scheduled as an echo the system ticks.
        ApplySpellEffect(lib, e.inner, atFx, dirFx, strengthMille, out, probe,
                         instabilityMille, salt ^ 0x5EC0u, flatten);
        if (g && EchoRepeats(g, e) > 1) {
          SpellEcho ec;
          ec.inner = e.inner;
          ec.at = atFx;
          ec.dir = dirFx;
          ec.left = EchoRepeats(g, e) - 1;
          ec.period = std::max(1, g->everyTicks);
          ec.instability = instabilityMille;
          out.echoes.push_back(std::move(ec));
        }
        break;
      }
      case SpellVerb::Sustain: {
        // `X aura`: SUSTAIN the inner effect or mod on the body at the point,
        // or on the place if no body is there. The system attaches it; every
        // tick it runs and bills the caster. Repeating `aura` widens the
        // attach radius.
        SpellStatus st;
        st.effect = e;
        st.at = atFx;
        st.dir = dirFx;
        st.ticksLeft = ClampI(g ? g->ticks : 1, 1, b.maxStatusTicks);
        st.instability = instabilityMille;
        int32_t per = 0;
        for (const EffectInst& i : e.inner) per = SatAdd(per, EffectTariff(lib, i));
        st.perTick = per;
        out.statuses.push_back(std::move(st));
        break;
      }
      case SpellVerb::Filter: {
        // `W null`: a filter entry at the point for the word's kind, one tick
        // unless an aura re-issues it. What it refuses is decided at the
        // splice (SpellSystem::FilterStreams), by the word's sort and verb.
        if (e.inner.empty()) break;
        SpellFilter f;
        f.glyph = e.inner[0].glyph;
        f.at = atFx;
        f.radius = ClampI(ScaleRadiusCbrtMille(e.radius, e.Scale()), 1, 64);
        f.ticksLeft = ClampI(g ? g->ticks : 1, 1, b.maxStatusTicks);
        out.filters.push_back(f);
        break;
      }
      case SpellVerb::Mend: {
        // `M mend`: the graft loop. Take up to perTick × n voxels of M from
        // within the radius of the point (a convert(cell->air) op per voxel,
        // filtered to M so nothing else leaves) and post a restore request
        // for the caster's body: fill the next missing anatomy cells with M.
        // The probe is the CPU mirror, so this reaches only what is mirrored
        // — reach around the caster, which is what a hand channel is.
        if (!probe || !probe->matAt) break;
        const int32_t want = ScaleMille(g ? g->perTick : 1, e.Scale(), 1);
        const int32_t r = ClampI(e.radius, 0, 8);
        uint32_t src = e.anyA ? 0u : e.matA;
        if (!e.anyA && src == 0) break;   // mends from nothing: charged
        int32_t took = 0;
        for (int32_t dz = -r; dz <= r && took < want; dz++)
          for (int32_t dy = -r; dy <= r && took < want; dy++)
            for (int32_t dx = -r; dx <= r && took < want; dx++) {
              if (dx * dx + dy * dy + dz * dz > r * r) continue;
              bool known;
              const uint32_t m = probe->matAt(probe->ctx, cx + dx, cy + dy, cz + dz, known);
              if (!known || m == 0) continue;
              if (src == 0) src = m;   // the wildcard takes the first matter it finds
              if (m != src) continue;
              out.ops.push_back({cx + dx, cy + dy, cz + dz, 0, 0u /*air*/, 1u, src, 0});
              took++;
            }
        if (took > 0) {
          out.restores.push_back({0, src, took});
          if (e.anyA) {
            const int32_t val = lib.Arcane(src);
            out.billOnResolve = SatAdd(
                out.billOnResolve,
                SatMul(took, SatAdd(SatMul(val, b.rateGraft),
                                    (int32_t)((int64_t)val * b.anythingSurchargeMille / 1000))));
          }
        }
        break;
      }
      case SpellVerb::Trail:
      case SpellVerb::None:
        break;
    }
  }
}

// ---- the system ------------------------------------------------------------

namespace {

// THE ITEMS OF A PAYLOAD THAT FIRE ON THIS EVENT (M3). `age` is the carrier's
// tick of flight, for `every` items. Every item that existed before triggers
// is `hit`, so the hit list of such a payload is the payload itself.
std::vector<EffectInst> ItemsOn(const std::vector<EffectInst>& payload, SpellTrigger t,
                                int32_t age = 0) {
  std::vector<EffectInst> out;
  for (const EffectInst& e : payload) {
    if (e.timing.trigger != t) continue;
    if (t == SpellTrigger::Every && (e.timing.every <= 0 || age % e.timing.every != 0)) continue;
    out.push_back(e);
  }
  return out;
}
bool HasTrigger(const std::vector<EffectInst>& payload, SpellTrigger t) {
  for (const EffectInst& e : payload)
    if (e.timing.trigger == t) return true;
  return false;
}

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
    // FLATTENED, for the same reason a Fatal cast is: a misfire must be a
    // bounded event. Letting the refused carrier's payload ask for carriers of
    // its own would put the budget it just failed straight back on the queue.
    ApplySpellEffect(*lib_, cast.payload, originFx, aim, 400, out, probe, 0, 0, true);
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
  p.vel = Unit(aim, (int64_t)cast.delivery.speedFx);
  p.seq = ++nextSeq_;
  // Trail budget: a HARD voxel count that only ever decreases (rule 2).
  p.trailBudget = cast.delivery.trail.empty() ? 0 : cast.delivery.trailBudget;
  p.bouncesLeft = cast.delivery.bounces;
  p.pierceLeft = cast.delivery.pierce;
  live_.push_back(std::move(p));
}

// Give every echo a payload scheduled since `from` the launch context the
// immediate path gives a launch: where a carrier it fires is born, along what,
// at which generation, for whom (SpellEcho::launchGen). The first stamp wins,
// the same way a launch keeps the at/dir of the resolve that asked for it.
static void StampEchoLaunches(SpellEmission& out, size_t from, SpellFxVec at, SpellFxVec dir,
                              int32_t gen, uint64_t casterId) {
  for (size_t k = from; k < out.echoes.size(); k++) {
    SpellEcho& ec = out.echoes[k];
    if (ec.casterId == 0) ec.casterId = casterId;
    if (ec.launchGen >= 0) continue;
    ec.launchAt = at;
    ec.launchDir = dir;
    ec.launchGen = gen;
  }
}

CastResult SpellSystem::Cast(const CastList& list, CasterState& caster,
                             const CasterHealth& health, uint64_t casterId,
                             SpellFxVec originFx, SpellFxVec dirFx,
                             uint32_t tick, SpellEmission& out,
                             const SpellProbe* probe, const SpellFxVec* selfAt,
                             const SpellBodyProbe* bodies) {
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
      // FLATTENED (law L11): every nested carrier resolves in place, so an
      // overcast `explosive projectile projectile bomb` goes off in the chest
      // with everything it was ever going to do and nothing leaves the body.
      ApplySpellEffect(*lib_, c.payload, originFx, dirFx, 1000, out, probe, 1000, 0, true);
      // The trail too: a fatal trail bolt lays its wake in the caster.
      if (!c.delivery.trail.empty())
        ApplySpellEffect(*lib_, c.delivery.trail, originFx, dirFx, 1000, out, probe, 1000, 0,
                         true);
      // And every lane's own nouns (rule 4): a fatal cast does EVERYTHING it
      // was ever going to do, in the chest, and a column is part of that.
      for (const SpellLane& ln : c.delivery.lanes) {
        if (!ln.extra.empty())
          ApplySpellEffect(*lib_, ln.extra, originFx, dirFx, 1000, out, probe, 1000, 0, true);
        for (const EffectInst& le : ln.extra) carve = std::max(carve, le.radius);
      }
      carve = std::max(carve, c.delivery.impactRadius);
      for (const EffectInst& e : c.payload) carve = std::max(carve, e.radius);
    }
    out.launches.clear();
    out.carveCaster = true;
    out.carveAt = Vec3{SpellFxToFloat(originFx.x), SpellFxToFloat(originFx.y),
                       SpellFxToFloat(originFx.z)};
    out.carveRadius = (float)std::min(carve, 8);
    return r;
  }

  // Instability makes the spell IMPRECISE, not merely costly.
  const SpellFxVec aim = SpellWobble(dirFx, r.instability, tick, casterId);

  // EVERY cast is now the implicit `hand` box (rule 3), so there is one path
  // here: resolve the hand's payload at reach, along the aim, once per fanned
  // instance. Whatever carriers the payload asks for leave as launch requests
  // and the adopt loop below gives birth to them.
  selfAt_ = selfAt ? *selfAt : SpellFxVec{};
  selfAtValid_ = selfAt != nullptr;
  for (size_t ci = 0; ci < list.casts.size(); ci++) {
    const SpellCast& c = list.casts[ci];
    const int32_t branches = ClampI(c.instances, 1, lib_->budgets.maxInstances);
    const int32_t lanes = (int32_t)c.delivery.lanes.size();
    const int32_t bolts = RecBolts(*lib_, c.delivery);
    // THE FAN IS TWO LEVELS DEEP (2026-09-22): the box's branches, and each
    // branch's own split. `b` is the GLOBAL BOLT ORDINAL and it is what every
    // key below is taken on - which is the whole reason it is global. With no
    // split anywhere it counts 0,1,2..  exactly as `i` did and `bolts` is
    // `branches`, so `SpellFan` is called with the arguments it has always been
    // called with and no sentence that existed before this day moves a bit.
    int32_t b = 0;
    for (int32_t k = 0; k < branches; k++) {
      // RULE 4: COPIES FAN, COLUMNS DO NOT. An instance with its own lane is
      // not a copy of anything — it is a second spell the player asked for, so
      // it resolves ON THE AIM. Only the shared-only copies spread. ...And a
      // branch that splits puts its FIRST bolt on the aim and fans the rest
      // around it, which is the same sentence with one word added.
      const SpellCast ci2 = InstanceCast(c, k);
      const bool column = k < lanes;
      const int32_t split = LaneSplit(*lib_, c.delivery, k);
      for (int32_t j = 0; j < split; j++, b++) {
        const bool onAim = column && j == 0;
        const SpellFxVec d =
            onAim ? aim : SpellFan(aim, b, bolts, tick + (uint32_t)ci * 131u, casterId);
        SpellFxVec at = originFx;
        const SpellFxVec u = Unit(d, (int64_t)ci2.delivery.reach * kSpellFxOne);
        at = {originFx.x + u.x, originFx.y + u.y, originFx.z + u.z};
        // Fanned resolve points around the anchor: bolt b lands a voxel or
        // two off (`shotgun` on the hand, rule 3).
        if (b > 0 && !onAim) {
          const SpellFxVec o = Unit(SpellFan({kSpellFxOne, 0, 0}, b, bolts, tick, casterId),
                                    2 * kSpellFxOne);
          at = {at.x + o.x, at.y + o.y, at.z + o.z};
        }
        const size_t before = out.launches.size();
        const size_t ebefore = out.echoes.size();
        ApplySpellEffect(*lib_, ci2.payload, at, d, 1000, out, probe, r.instability,
                         (uint32_t)b);
        // A carrier the hand spoke starts AT THE CASTER, not at reach: the
        // muzzle is where a bolt leaves from and where `self` resolves.
        for (size_t kk = before; kk < out.launches.size(); kk++) {
          out.launches[kk].at = originFx;
          out.launches[kk].dir = d;
          out.launches[kk].generation = 0;
          out.launches[kk].casterId = casterId;
        }
        StampEchoLaunches(out, ebefore, originFx, d, 0, casterId);
        // A gravity Mod on the hand acts on the caster's body: `float` hops,
        // `heavy` shoves down. Once, as an impulse; `aura` makes it a status.
        if (b == 0 && ci2.delivery.gravityMille != 0)
          out.casterImpulseVps.y += -(float)ci2.delivery.gravityMille * 0.012f;
      }
    }
  }
  // Statuses, echoes and filters the payload asked for become system state;
  // the caster who spoke them pays for them.
  for (SpellStatus& st : out.statuses) st.casterId = casterId;
  for (SpellEcho& ec : out.echoes) ec.casterId = casterId;
  for (SpellFilter& f : out.filters) f.casterId = casterId;
  for (SpellRestore& rs : out.restores)
    if (rs.casterId == 0) rs.casterId = casterId;
  Adopt(out, bodies, tick, probe);
  selfAtValid_ = false;
  return r;
}

// THE NESTED LAUNCHES A RESOLVE ASKED FOR (rule 2). Each one becomes exactly
// what the spoken delivery would have become at the top level: a flight, a
// body, a bounded beam, or an instant resolve — one generation down, fanned by
// its own `count`.
//
// The drain LOOPS, because an instant nested box (`... self`) resolves its
// payload right here and that payload may ask for carriers of its own. It is
// bounded twice over: nothing past `maxGeneration` launches, and the loop
// itself stops after that many passes.
void SpellSystem::AdoptLaunches(SpellEmission& out, const SpellBodyProbe* bodies,
                                uint32_t tick, const SpellProbe* probe) {
  const SpellBudgets& b = lib_->budgets;
  std::vector<SpellLaunchReq> batch;
  for (int pass = 0; pass <= b.maxGeneration + 1 && !out.launches.empty(); pass++) {
    batch.clear();
    batch.swap(out.launches);
    for (SpellLaunchReq& rq : batch) {
      const int32_t gen = rq.generation < 0 ? 0 : rq.generation;
      if (gen > b.maxGeneration) continue;   // the subcriticality guarantee
      SpellCast c;
      c.delivery = rq.delivery;
      c.payload = rq.payload;
      // RULE 4: a nested box carries its lanes inside the record it was asked
      // for with, so the fan here is the same max(count, lanes) the lowering
      // priced.
      c.instances = RecInstances(*lib_, rq.delivery);
      c.generation = gen;
      PriceCast(*lib_, c);
      const int32_t branches = c.instances;
      const int32_t lanes = (int32_t)c.delivery.lanes.size();
      const int32_t bolts = RecBolts(*lib_, c.delivery);
      // Two levels, one GLOBAL bolt ordinal - see `Cast`. `b` and `bolts`
      // reduce to the old `i` and `inst` whenever nothing inside a lane fans,
      // which is what keeps every nested fan that existed bit-identical.
      int32_t b2 = 0;
      for (int32_t k = 0; k < branches; k++) {
       const SpellCast ic = InstanceCast(c, k);
       const bool column = k < lanes;
       const int32_t split = LaneSplit(*lib_, c.delivery, k);
       for (int32_t j = 0; j < split; j++, b2++) {
        // Copies fan, columns do not - and a branch's FIRST bolt is its column.
        const bool onAim = column && j == 0;
        const SpellFxVec d =
            onAim ? rq.dir : SpellFan(rq.dir, b2, bolts, tick ^ rq.salt, rq.casterId);
        // AT LAUNCH (M3): a flight's launch items go off at the muzzle, the
        // moment it is born, and any child they fire leaves along its aim.
        if (ic.delivery.mech == DeliveryMech::Flight &&
            HasTrigger(ic.payload, SpellTrigger::Launch)) {
          const size_t lb = out.launches.size();
          const size_t eb = out.echoes.size();
          ApplySpellEffect(*lib_, ItemsOn(ic.payload, SpellTrigger::Launch), rq.at, d, 1000, out,
                           probe, rq.instability, rq.salt ^ 0x1A0u ^ (uint32_t)b2);
          for (size_t kk = lb; kk < out.launches.size(); kk++) {
            out.launches[kk].at = rq.at;
            out.launches[kk].dir = d;
            out.launches[kk].generation = gen + 1;
            out.launches[kk].casterId = rq.casterId;
          }
          StampEchoLaunches(out, eb, rq.at, d, gen + 1, rq.casterId);
        }
        switch (ic.delivery.mech) {
          case DeliveryMech::Flight:
            if (ic.delivery.body)
              RequestBody(ic, rq.at, d, rq.casterId, rq.instability, out);
            else
              Launch(ic, rq.at, d, rq.casterId, tick, b2, rq.instability, out, probe);
            break;
          case DeliveryMech::Continuous: {
            // A beam the HAND spoke (generation 0) is the caster's own: they
            // hold it and releasing ends it. A beam a carrier spoke is
            // ANCHORED — it burns from where its parent resolved for its tick
            // cap, and nobody is holding anything, which is the only reading
            // of a nested beam that is bounded.
            if (b2 > 0) break;  // one beam; a fan fans its resolve, not its ray
            SpellBeam bm;
            bm.cast = ic;
            bm.casterId = rq.casterId;
            bm.origin = rq.at;
            bm.dir = d;
            bm.ticksLeft = ClampI(ic.delivery.lifetimeTicks, 1, b.maxStatusTicks);
            bm.perTick = SatAdd(c.tariff, c.carryCost);
            bm.instability = rq.instability;
            bm.held = true;
            bm.anchored = gen > 0;
            beams_.push_back(std::move(bm));
            break;
          }
          case DeliveryMech::Instant: {
            // `self`: at the caster's body when the owner can say where that
            // is, otherwise where the parent resolved. Position-parameterised
            // either way (thesis 2) — the same call, different arguments.
            SpellFxVec at = rq.at;
            const std::vector<EffectInst>* payload = &ic.payload;
            std::vector<EffectInst> clamped;
            if (selfAtValid_) {
              at = selfAt_;
              clamped = ic.payload;
              for (EffectInst& e : clamped)
                if (e.radius > ic.delivery.impactRadius) e.radius = ic.delivery.impactRadius;
              payload = &clamped;
            } else if (bodies && bodies->bodyPos) {
              Vec3 cc;
              if (bodies->bodyPos(bodies->ctx, rq.casterId, cc))
                at = {SpellFxFromFloat(cc.x), SpellFxFromFloat(cc.y), SpellFxFromFloat(cc.z)};
            }
            if (b2 > 0 && !onAim) {
              const SpellFxVec o = Unit(SpellFan({kSpellFxOne, 0, 0}, b2, bolts, tick,
                                                 rq.casterId),
                                        2 * kSpellFxOne);
              at = {at.x + o.x, at.y + o.y, at.z + o.z};
            }
            const size_t before = out.launches.size();
            const size_t ebefore = out.echoes.size();
            ApplySpellEffect(*lib_, *payload, at, d, 1000, out, probe, rq.instability,
                             rq.salt ^ (uint32_t)b2);
            for (size_t kk = before; kk < out.launches.size(); kk++) {
              out.launches[kk].at = at;
              out.launches[kk].dir = {0, kSpellFxOne, 0};
              out.launches[kk].generation = gen + 1;
              out.launches[kk].casterId = rq.casterId;
            }
            StampEchoLaunches(out, ebefore, at, {0, kSpellFxOne, 0}, gen + 1, rq.casterId);
            if (b2 == 0 && ic.delivery.gravityMille != 0)
              out.casterImpulseVps.y += -(float)ic.delivery.gravityMille * 0.012f;
            break;
          }
        }
       }
      }
      for (SpellStatus& st : out.statuses)
        if (st.casterId == 0) st.casterId = rq.casterId;
      for (SpellEcho& ec : out.echoes)
        if (ec.casterId == 0) ec.casterId = rq.casterId;
      for (SpellFilter& f : out.filters)
        if (f.casterId == 0) f.casterId = rq.casterId;
      for (SpellRestore& rs : out.restores)
        if (rs.casterId == 0) rs.casterId = rq.casterId;
    }
  }
  out.launches.clear();
}

void SpellSystem::Adopt(SpellEmission& out, const SpellBodyProbe* bodies, uint32_t tick,
                        const SpellProbe* probe) {
  const SpellBudgets& b = lib_->budgets;
  AdoptLaunches(out, bodies, tick, probe);
  for (SpellStatus& st : out.statuses) {
    // Rule 2: a hard cap on live statuses per caster. Over it the aura is
    // charged (it was) and attaches nothing.
    if (StatusCountFor(st.casterId) >= b.maxStatusPerCaster) continue;
    st.id = ++nextStatusId_;
    st.ticksLeft = ClampI(st.ticksLeft, 1, b.maxStatusTicks);
    // The body at the point, or the place.
    if (bodies && bodies->bodyIdAt) {
      const Vec3 from{SpellFxToFloat(st.at.x), SpellFxToFloat(st.at.y), SpellFxToFloat(st.at.z)};
      const float r = (float)std::max(1, ScaleRadiusCbrtMille(st.effect.radius, st.effect.Scale())) + 2.0f;
      uint64_t id;
      Vec3 c;
      if (bodies->bodyIdAt(bodies->ctx, from, r, id, c)) {
        st.target = id;
        st.at = {SpellFxFromFloat(c.x), SpellFxFromFloat(c.y), SpellFxFromFloat(c.z)};
      }
    }
    statuses_.push_back(std::move(st));
  }
  out.statuses.clear();
  for (SpellEcho& ec : out.echoes) {
    if ((int)echoes_.size() >= 64) break;   // bounded, like everything here
    echoes_.push_back(std::move(ec));
  }
  out.echoes.clear();
  for (SpellFilter& f : out.filters) {
    if ((int)filters_.size() >= 64) break;
    filters_.push_back(f);
  }
  out.filters.clear();
}

void SpellSystem::HoldBeam(uint64_t casterId, SpellFxVec originFx, SpellFxVec dirFx,
                           bool held) {
  for (SpellBeam& bm : beams_) {
    // An anchored beam is a nested one: it burns where its parent resolved,
    // on its own clock. The caster's aim is not its aim and releasing the key
    // does not end it.
    if (bm.casterId != casterId || bm.anchored) continue;
    bm.origin = originFx;
    bm.dir = dirFx;
    bm.held = bm.held && held;
  }
}

bool SpellSystem::DropNewestStatus(uint64_t casterId) {
  int best = -1;
  for (size_t i = 0; i < statuses_.size(); i++)
    if (statuses_[i].casterId == casterId &&
        (best < 0 || statuses_[i].id > statuses_[best].id))
      best = (int)i;
  if (best < 0) return false;
  statuses_[best] = statuses_.back();
  statuses_.pop_back();
  return true;
}

void SpellSystem::DropAll(uint64_t casterId) {
  for (size_t i = 0; i < statuses_.size();) {
    if (statuses_[i].casterId == casterId) {
      statuses_[i] = statuses_.back();
      statuses_.pop_back();
    } else {
      i++;
    }
  }
  for (SpellBeam& bm : beams_)
    if (bm.casterId == casterId) bm.held = false;
}

int32_t SpellSystem::ReservationFor(uint64_t casterId) const {
  int32_t per = 0;
  for (const SpellStatus& st : statuses_)
    if (st.casterId == casterId) per = SatAdd(per, st.perTick);
  for (const SpellBeam& bm : beams_)
    if (bm.casterId == casterId && bm.held) per = SatAdd(per, bm.perTick);
  return SatMul(per, kReserveHorizonTicks);
}

int SpellSystem::StatusCountFor(uint64_t casterId) const {
  int n = 0;
  for (const SpellStatus& st : statuses_)
    if (st.casterId == casterId) n++;
  return n;
}

// What a filter's word refuses, by the word's SORT and VERB — never by name:
//   matter        ops and spawns of that material
//   convert       overwrite/melt ops (mode != 0)
//   place / spray paint ops / spawns
//   explode       explosions
//   wind          wind primitives
//   delivery      carriers of that mech (absorbed in Tick)
// `kindMode`: 0 brush op (material, mode in `material`/x?), see callers.
bool SpellSystem::FilterRefuses(const SpellFilter& f, int32_t x, int32_t y, int32_t z,
                                uint32_t material, int kindMode) const {
  const GlyphDef* w = lib_->At(f.glyph);
  if (!w) return false;
  const int64_t dx = x - SpellFxFloor(f.at.x), dy = y - SpellFxFloor(f.at.y),
                dz = z - SpellFxFloor(f.at.z);
  if (dx * dx + dy * dy + dz * dz > (int64_t)f.radius * f.radius) return false;
  // kindMode: 0 = paint op, 1 = overwrite op, 2 = melt op, 3 = spawn,
  //           4 = explosion, 5 = wind, 6 = strike
  switch (w->sort) {
    case GlyphSort::Matter:
      if (w->wildcard) return kindMode <= 3;   // `anything null`: all matter ops
      return (kindMode <= 3) && material == w->material;
    case GlyphSort::Effect:
    case GlyphSort::Operator:
      switch (w->verb) {
        case SpellVerb::Convert: return kindMode == 1 || kindMode == 2;
        case SpellVerb::Place: return kindMode == 0;
        case SpellVerb::Spray: return kindMode == 3;
        case SpellVerb::Explode: return kindMode == 4;
        case SpellVerb::Wind: return kindMode == 5;
        case SpellVerb::Strike: return kindMode == 6;
        default: return false;
      }
    default:
      return false;
  }
}

int SpellSystem::FilterStreams(std::vector<BrushOp>& ops, std::vector<ExplosionOp>& exps,
                               std::vector<ParticleSpawn>& spawns,
                               std::vector<WindPrim>& winds,
                               std::vector<SpellStrike>* strikes) const {
  if (filters_.empty() || !lib_) return 0;
  int n = 0;
  auto sweep = [&](auto& v, auto pred) {
    size_t w = 0;
    for (size_t i = 0; i < v.size(); i++) {
      bool refuse = false;
      for (const SpellFilter& f : filters_)
        if (pred(f, v[i])) {
          refuse = true;
          break;
        }
      if (refuse) n++;
      else v[w++] = v[i];
    }
    v.resize(w);
  };
  sweep(ops, [&](const SpellFilter& f, const BrushOp& o) {
    return FilterRefuses(f, o.x, o.y, o.z, o.material & 0xFFFu, (int)std::min(o.mode, 2u));
  });
  sweep(exps, [&](const SpellFilter& f, const ExplosionOp& o) {
    return FilterRefuses(f, o.x, o.y, o.z, 0, 4);
  });
  sweep(spawns, [&](const SpellFilter& f, const ParticleSpawn& o) {
    return FilterRefuses(f, SpellFxFloor(o.px), SpellFxFloor(o.py), SpellFxFloor(o.pz),
                         o.payload & 0xFFFu, 3);
  });
  sweep(winds, [&](const SpellFilter& f, const WindPrim& o) {
    return FilterRefuses(f, o.x, o.y, o.z, 0, 5);
  });
  // A strike is refused at its AIM (a `lightning null` ward over a village
  // turns a bolt called on it away; the target search cannot reach past it).
  if (strikes)
    sweep(*strikes, [&](const SpellFilter& f, const SpellStrike& o) {
      return FilterRefuses(f, o.x, o.y, o.z, 0, 6);
    });
  return n;
}

void SpellSystem::RequestBody(const SpellCast& cast, SpellFxVec originFx,
                              SpellFxVec aim, uint64_t casterId,
                              int32_t instability, SpellEmission& out) {
  const SpellBudgets& b = lib_->budgets;
  // Bombs share the live-projectile cap (rule 2); over it, the bomb goes off
  // in the hand like an over-budget bolt would.
  if ((int)(live_.size() + bombs_.size()) >= b.maxLiveProjectiles ||
      cast.generation > b.maxGeneration) {
    ApplySpellEffect(*lib_, cast.payload, originFx, aim, 400, out, nullptr, instability, 0,
                     true);
    return;
  }
  SpellBomb bm;
  bm.cast = cast;
  bm.token = ++nextToken_;
  bm.fuseLeft = std::max(1, cast.delivery.fuseTicks);
  bm.ticksLeft = ClampI(SatAdd(cast.delivery.lifetimeTicks, cast.delivery.fuseTicks), 1,
                        2 * b.maxLifetimeTicks);
  bm.lastPos = originFx;
  bm.trailBudget = cast.delivery.trail.empty() ? 0 : cast.delivery.trailBudget;
  bm.instability = instability;
  bm.gen = cast.generation;
  bm.casterId = casterId;
  bombs_.push_back(std::move(bm));

  SpellBodyRequest rq;
  rq.token = nextToken_;
  rq.pos = Vec3{SpellFxToFloat(originFx.x), SpellFxToFloat(originFx.y),
                SpellFxToFloat(originFx.z)};
  const SpellFxVec v = Unit(aim, (int64_t)cast.delivery.speedFx);
  // voxels/tick -> voxels/second at the physics boundary
  rq.vel = Vec3{SpellFxToFloat(v.x) * (float)kTicksPerSecond,
                SpellFxToFloat(v.y) * (float)kTicksPerSecond,
                SpellFxToFloat(v.z) * (float)kTicksPerSecond};
  rq.radius = 1.5f;
  rq.material = lib_->Delivery(cast.delivery.glyph).material;
  out.bodyRequests.push_back(rq);
}

bool SpellSystem::AdoptBody(uint32_t token, uint64_t handle) {
  for (SpellBomb& bm : bombs_) {
    if (bm.token == token) {
      bm.body = handle;
      return true;
    }
  }
  return false;
}

void SpellSystem::Tick(uint32_t tick, World& world,
                       const std::vector<uint32_t>& classOf,
                       SpellEmission& out, const SpellBodyProbe* bodies) {
  opsDropped_ = 0;
  refused_ = 0;
  if (!lib_) return;
  int opsUsed = 0;
  const SpellProbe probe = WorldSpellProbe(world);
  const uint32_t seed = world.WorldSeed();

  // WHAT A FLIGHT CAN HIT, in three tiers of knowledge. The CPU mirror is only
  // the 3x3x3 chunks around the PLAYER (~48 voxels); everything a bolt does
  // happens past it. Reading Unknown as solid detonated every bolt in the
  // caster's face; reading it as passable (the previous answer) meant a bolt
  // never hit anything at all once it left the mirror -- it fizzled at the end
  // of its life, which is what "explosives don't deliver" looked like.
  //   1. the mirror, when the cell is in it (one tick latent, the truth);
  //   2. the on-demand chunk cache (World::Cached), which the flight itself
  //      keeps warm by asking for the chunks ahead of it (prefetch below);
  //   3. the ground contract, World::TerrainHeight == genColumn(x,z).h
  //      exactly, for a cell nobody has fetched yet -- so at worst a bolt
  //      lands ON THE GROUND, never under it, and never through it.
  // Out-of-window space stays solid (DESIGN.md section 3).
  auto solidAt = [&](int32_t x, int32_t y, int32_t z) {
    if (!world.CellInWindow({(int)x, (int)y, (int)z})) return true;
    const CellKind k = world.KindAt({(int)x, (int)y, (int)z}, classOf);
    if (k != CellKind::Unknown) return k == CellKind::Solid;
    const IVec3 wc{x >> 4, y >> 4, z >> 4};
    if (const CachedChunk* cc = world.Cached(wc); cc && cc->voxels.size() == kChunkVol) {
      const uint32_t mat =
          cc->voxels[(size_t)(((z & 15) * (int)kChunk + (y & 15)) * (int)kChunk + (x & 15))] &
          0xFFFu;
      if (mat == 0 || mat >= classOf.size()) return false;
      // Solid and powder both stop a flight, exactly as World::KindAt says.
      return classOf[mat] == 0u || classOf[mat] == 1u;
    }
    world.RequestChunkFetch(wc);
    return y <= World::TerrainHeight(x, z, seed);
  };
  // Ask for the chunks a flight is about to cross: its own and a few along the
  // velocity. Coalesced by World (one request per chunk per readback), bounded
  // by kFetchPerTick, and nothing when the chunk is already in the mirror.
  auto prefetch = [&](SpellFxVec at, SpellFxVec vel) {
    const SpellFxVec step = Unit(vel, 12 * kSpellFxOne);
    for (int k = 0; k < 5; k++) {
      const IVec3 cell{SpellFxFloor(at.x), SpellFxFloor(at.y), SpellFxFloor(at.z)};
      if (world.CellInWindow(cell) && world.KindAt(cell, classOf) == CellKind::Unknown)
        world.RequestChunkFetch({cell.x >> 4, cell.y >> 4, cell.z >> 4});
      at.x += step.x;
      at.y += step.y;
      at.z += step.z;
    }
  };

  // A trail mark at a whole voxel: charge the hard budget BEFORE emitting and
  // refuse when it does not fit (rule 2). Returns false when the budget died.
  auto mark = [&](const SpellCast& cast, int32_t& budget, int32_t& phase, int32_t& mx0,
                  int32_t& my0, int32_t& mz0, bool& valid, SpellFxVec at, SpellFxVec dir) {
    if (budget <= 0 || cast.delivery.trail.empty()) return true;
    const int32_t mx = SpellFxFloor(at.x), my = SpellFxFloor(at.y), mz = SpellFxFloor(at.z);
    if (valid && mx == mx0 && my == my0 && mz == mz0) return true;
    if (!world.CellInWindow({(int)mx, (int)my, (int)mz})) return true;
    const int32_t every = std::max(1, cast.delivery.trailEvery);
    if ((phase++ % every) != 0) return true;
    if (opsUsed >= kSpellOpsPerTick) {
      opsDropped_++;
      return true;
    }
    int32_t vol = 0;
    for (const EffectInst& e : cast.delivery.trail) vol = SatAdd(vol, EffectVolume(*lib_, e));
    if (vol < 1) vol = 1;
    if (vol > budget) {
      // Cannot afford another mark: the trail is the spell's whole point, so
      // the carrier dies with its budget rather than flying on inertly.
      budget = 0;
      return false;
    }
    budget -= vol;
    const size_t before = out.ops.size();
    const SpellFxVec c{mx * kSpellFxOne + kSpellFxOne / 2, my * kSpellFxOne + kSpellFxOne / 2,
                       mz * kSpellFxOne + kSpellFxOne / 2};
    ApplySpellEffect(*lib_, cast.delivery.trail, c, dir, 1000, out, &probe, 0, (uint32_t)phase);
    opsUsed += (int)(out.ops.size() - before);
    mx0 = mx;
    my0 = my;
    mz0 = mz;
    valid = true;
    return budget > 0;
  };

  // Everything a resolve asked to sustain belongs to the caster who threw it.
  auto stamp = [&](uint64_t casterId) {
    for (SpellStatus& st : out.statuses)
      if (st.casterId == 0) st.casterId = casterId;
    for (SpellEcho& ec : out.echoes)
      if (ec.casterId == 0) ec.casterId = casterId;
    for (SpellFilter& f : out.filters)
      if (f.casterId == 0) f.casterId = casterId;
    for (SpellRestore& rs : out.restores)
      if (rs.casterId == 0) rs.casterId = casterId;
  };

  // WHICH WAY A CHILD LEAVES. The parent's direction reflected off the surface
  // it hit, per axis, exactly the way `bounce` reflects — so a nested bolt
  // comes off the wall rather than launching into it and detonating on its own
  // first sub-step. With no surface (a bomb at rest, a fuse, an orb expiring,
  // a `self`) there is nothing to reflect off and it goes straight up.
  auto launchDir = [&](SpellFxVec at, SpellFxVec from, SpellFxVec dir, bool surface) {
    const SpellFxVec up{0, kSpellFxOne, 0};
    if (!surface) return up;
    const int32_t cx = SpellFxFloor(at.x), cy = SpellFxFloor(at.y), cz = SpellFxFloor(at.z);
    const int32_t px = SpellFxFloor(from.x), py = SpellFxFloor(from.y),
                  pz = SpellFxFloor(from.z);
    bool fx = solidAt(cx, py, pz), fy = solidAt(px, cy, pz), fz = solidAt(px, py, cz);
    if (!fx && !fy && !fz) fx = fy = fz = true;
    SpellFxVec d = dir;
    if (fx) d.x = -d.x;
    if (fy) d.y = -d.y;
    if (fz) d.z = -d.z;
    if (d.x == 0 && d.y == 0 && d.z == 0) return up;
    return d;
  };

  // Resolve a cast at a point through the SAME function backfire uses
  // (thesis 2), then the carriers its payload asked for. `from` is the last
  // free position, which is where a child launches from — a child born inside
  // the wall its parent hit would impact on its first sub-step and chain.
  // `items` is WHICH PART of the payload this event fires (M3: the hit items on
  // an impact, the bounce items at a bounce, ...); `childDir`, when given, is
  // the direction a child leaves in (a bounce's rebound, the flight's own
  // velocity) instead of the reflection off a surface.
  auto resolve = [&](const SpellCast& cast, const std::vector<EffectInst>& items, SpellFxVec at,
                     SpellFxVec from, SpellFxVec dir, int32_t instability, int32_t gen,
                     uint64_t casterId, uint32_t salt, bool surface,
                     const SpellFxVec* childDir = nullptr) {
    {
      SpellImpactFx fx;
      fx.at = at;
      fx.tint = CastTintMaterial(cast);
      fx.radius = std::max(1, cast.delivery.impactRadius);
      for (const EffectInst& e : items) fx.radius = std::max(fx.radius, e.radius);
      fx.deliveryGlyph = cast.delivery.glyph;
      out.impacts.push_back(fx);
    }
    if (opsUsed < kSpellOpsPerTick) {
      const size_t before = out.ops.size();
      const size_t lbefore = out.launches.size();
      const size_t ebefore = out.echoes.size();
      ApplySpellEffect(*lib_, items, at, dir, 1000, out, &probe, instability, salt);
      opsUsed += (int)(out.ops.size() - before);
      // NESTING (rule 2): a child leaves from the last free position, one
      // generation down. The generation counter is the subcriticality
      // guarantee — nothing past budgets.maxGeneration launches.
      const SpellFxVec ld = childDir ? *childDir : launchDir(at, from, dir, surface);
      for (size_t k = lbefore; k < out.launches.size(); k++) {
        out.launches[k].at = from;
        out.launches[k].dir = ld;
        out.launches[k].generation = gen + 1;
        out.launches[k].casterId = casterId;
      }
      // A DELAYED child (M3 `delay`, or an `echo` over a box) is the same
      // child later: same free position, same direction, same generation.
      StampEchoLaunches(out, ebefore, from, ld, gen + 1, casterId);
      stamp(casterId);
    } else {
      opsDropped_++;
      out.launches.clear();
    }
  };

  // Steer a seeking bolt toward the nearest target. The target is a float
  // position from the owner (a mob origin); it is converted once and every
  // update to the authoritative velocity is integer.
  auto seek = [&](SpellProjectile& p) {
    if (p.cast.delivery.seek <= 0 || !bodies || !bodies->nearestTarget) return;
    Vec3 tgt;
    const Vec3 from{SpellFxToFloat(p.pos.x), SpellFxToFloat(p.pos.y), SpellFxToFloat(p.pos.z)};
    if (!bodies->nearestTarget(bodies->ctx, from, p.casterId, tgt)) return;
    const int64_t sp = IntSqrt((int64_t)p.vel.x * p.vel.x + (int64_t)p.vel.y * p.vel.y +
                               (int64_t)p.vel.z * p.vel.z);
    if (sp <= 0) return;
    const SpellFxVec to{SpellFxFromFloat(tgt.x) - p.pos.x, SpellFxFromFloat(tgt.y) - p.pos.y,
                        SpellFxFromFloat(tgt.z) - p.pos.z};
    const SpellFxVec u = Unit(to, sp);
    const int32_t k = std::min(p.cast.delivery.seek, 8);
    SpellFxVec v{(int32_t)(((int64_t)p.vel.x * (16 - k) + (int64_t)u.x * k) / 16),
                 (int32_t)(((int64_t)p.vel.y * (16 - k) + (int64_t)u.y * k) / 16),
                 (int32_t)(((int64_t)p.vel.z * (16 - k) + (int64_t)u.z * k) / 16)};
    p.vel = Unit(v, sp);
  };

  for (size_t i = 0; i < live_.size();) {
    SpellProjectile& p = live_[i];
    bool impact = false;
    SpellFxVec impactAt = p.pos;

    bool impactSurface = false;
    bool expiredNow = false;
    if (p.resting) {
      // A fused bolt sits where it landed until the fuse runs out.
      if (--p.fuseLeft <= 0) {
        resolve(p.cast, ItemsOn(p.cast.payload, SpellTrigger::Hit), p.pos, p.pos,
                {0, kSpellFxOne, 0}, p.instability, p.gen, p.casterId, (uint32_t)tick, false);
        p.alive = false;
      }
    } else if (--p.ticksLeft <= 0) {
      // Lifetime expiry is a HARD bound (rule 2). An expired bolt fizzles —
      // unless the delivery says its life running out IS its resolve (orb).
      if (p.cast.delivery.resolveOnExpiry) {
        impact = true;
        impactAt = p.pos;
      }
      expiredNow = true;
      p.alive = false;
    } else {
      // EVERY N TICKS OF FLIGHT (M3): a pulse from where the carrier is, along
      // the way it is going.
      p.age++;
      if (HasTrigger(p.cast.payload, SpellTrigger::Every)) {
        const std::vector<EffectInst> ev = ItemsOn(p.cast.payload, SpellTrigger::Every, p.age);
        if (!ev.empty())
          resolve(p.cast, ev, p.pos, p.pos, p.vel, p.instability, p.gen, p.casterId,
                  (uint32_t)tick ^ 0xE7E7u, false, &p.vel);
      }
      // Gravity and homing, per the record.
      if (p.cast.delivery.gravityMille != 0)
        p.vel.y -= (int32_t)((int64_t)kGravityFxPerTick2 * p.cast.delivery.gravityMille / 1000);
      seek(p);
      prefetch(p.pos, p.vel);
      const SpellFxVec tickStart = p.pos;
      // Swept integration with anti-tunneling: step at most half a voxel at a
      // time, exactly as sim_particle.wgsl does.
      const int32_t kHalf = kSpellFxOne / 2;
      int64_t maxComp = std::max({(int64_t)std::abs(p.vel.x), (int64_t)std::abs(p.vel.y),
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
        const bool solid = solidAt(cx, cy, cz);
        if (solid && p.piercing) {
          // Still inside the wall it is passing through.
          p.pos = next;
          continue;
        }
        if (!solid) p.piercing = false;
        if (solid) {
          if (p.pierceLeft > 0) {
            // PIERCE: through this wall, resolving on the next one.
            p.pierceLeft--;
            p.piercing = true;
            p.pos = next;
            continue;
          }
          if (p.bouncesLeft > 0) {
            // BOUNCE: reflect the axis that entered the solid (each axis
            // probed alone), losing a fifth of the speed so it settles.
            p.bouncesLeft--;
            const int32_t px = SpellFxFloor(p.pos.x), py = SpellFxFloor(p.pos.y),
                          pz = SpellFxFloor(p.pos.z);
            bool fx = solidAt(cx, py, pz), fy = solidAt(px, cy, pz), fz = solidAt(px, py, cz);
            if (!fx && !fy && !fz) fx = fy = fz = true;
            if (fx) p.vel.x = -p.vel.x;
            if (fy) p.vel.y = -p.vel.y;
            if (fz) p.vel.z = -p.vel.z;
            p.vel.x = p.vel.x * 4 / 5;
            p.vel.y = p.vel.y * 4 / 5;
            p.vel.z = p.vel.z * 4 / 5;
            // AT EACH BOUNCE (M3): from the last free position, a child
            // leaving along the rebound.
            if (HasTrigger(p.cast.payload, SpellTrigger::Bounce))
              resolve(p.cast, ItemsOn(p.cast.payload, SpellTrigger::Bounce), p.pos, p.pos, p.vel,
                      p.instability, p.gen, p.casterId, (uint32_t)tick ^ 0xB0B0u, false, &p.vel);
            continue;
          }
          if (p.cast.delivery.fuseTicks > 0) {
            // FUSE: rest where it landed and count down.
            p.resting = true;
            p.fuseLeft = p.cast.delivery.fuseTicks;
            break;
          }
          impact = true;
          impactSurface = true;
          impactAt = next;
          break;
        }
        p.pos = next;
        if (!mark(p.cast, p.trailBudget, p.trailPhase, p.markedX, p.markedY, p.markedZ,
                  p.markedValid, p.pos, p.vel)) {
          p.alive = false;
          break;
        }
        stamp(p.casterId);
      }
      // BODIES. Mobs, debris and other casters are not in the voxel grid, so
      // the sweep above cannot see them: one ray over this tick's segment
      // (up to the wall it stopped at, if it stopped) asks the owner. A body
      // on the way resolves the cast on the body, whatever the record said
      // about bounces or piercing -- flesh is what a bolt is for.
      if (p.alive && bodies && bodies->bodyHit) {
        const SpellFxVec segEnd = impact ? impactAt : p.pos;
        const Vec3 a{SpellFxToFloat(tickStart.x), SpellFxToFloat(tickStart.y),
                     SpellFxToFloat(tickStart.z)};
        const Vec3 bEnd{SpellFxToFloat(segEnd.x), SpellFxToFloat(segEnd.y),
                        SpellFxToFloat(segEnd.z)};
        Vec3 hit;
        if (bodies->bodyHit(bodies->ctx, a, bEnd, p.casterId, hit)) {
          impact = true;
          impactSurface = true;
          impactAt = {SpellFxFromFloat(hit.x), SpellFxFromFloat(hit.y), SpellFxFromFloat(hit.z)};
          p.pos = impactAt;
          p.resting = false;
        }
      }
    }

    if (impact) {
      // The hit items - and, when life running out IS the resolve (an orb),
      // the expiry items with them.
      std::vector<EffectInst> hit = ItemsOn(p.cast.payload, SpellTrigger::Hit);
      if (expiredNow)
        for (const EffectInst& x : ItemsOn(p.cast.payload, SpellTrigger::Expire)) hit.push_back(x);
      resolve(p.cast, hit, impactAt, p.pos, p.vel, p.instability, p.gen, p.casterId,
              (uint32_t)tick, impactSurface);
      p.alive = false;
    } else if (expiredNow && HasTrigger(p.cast.payload, SpellTrigger::Expire)) {
      // A bolt that hit nothing still has an ending, and that is an event.
      resolve(p.cast, ItemsOn(p.cast.payload, SpellTrigger::Expire), p.pos, p.pos, p.vel,
              p.instability, p.gen, p.casterId, (uint32_t)tick ^ 0xE0E0u, false, &p.vel);
    }

    if (!p.alive) {
      live_[i] = live_.back();
      live_.pop_back();
    } else {
      i++;
    }
  }

  // A carrier inside a filter for its delivery's kind is ABSORBED: it dies
  // without resolving. `projectile null aura self` absorbs bolts.
  auto absorbed = [&](int deliveryGlyph, SpellFxVec at) {
    const GlyphDef& dg = lib_->Delivery(deliveryGlyph);
    for (const SpellFilter& f : filters_) {
      const GlyphDef* w = lib_->At(f.glyph);
      if (!w || w->sort != GlyphSort::Delivery || w->mech != dg.mech) continue;
      const int64_t dx = SpellFxFloor(at.x) - SpellFxFloor(f.at.x),
                    dy = SpellFxFloor(at.y) - SpellFxFloor(f.at.y),
                    dz = SpellFxFloor(at.z) - SpellFxFloor(f.at.z);
      if (dx * dx + dy * dy + dz * dz <= (int64_t)f.radius * f.radius) return true;
    }
    return false;
  };
  for (size_t i = 0; i < live_.size();) {
    if (absorbed(live_[i].cast.delivery.glyph, live_[i].pos)) {
      refused_++;
      live_[i] = live_.back();
      live_.pop_back();
    } else {
      i++;
    }
  }

  // BOMBS: rigid bodies the owner adopted. The VM asks where each one is,
  // lays its trail as it rolls, and resolves the payload there when the fuse
  // runs out — or when the body is gone (something blew it up first), or when
  // the hard tick bound passes (the owner never adopted it).
  for (size_t i = 0; i < bombs_.size();) {
    SpellBomb& bm = bombs_[i];
    bool gone = false;
    if (bm.body != 0 && bodies && bodies->bodyAt) {
      Vec3 c;
      if (bodies->bodyAt(bodies->ctx, bm.body, c)) {
        bm.lastPos = {SpellFxFromFloat(c.x), SpellFxFromFloat(c.y), SpellFxFromFloat(c.z)};
      } else {
        gone = true;
      }
    }
    if (bm.trailBudget > 0) {
      mark(bm.cast, bm.trailBudget, bm.trailPhase, bm.markedX, bm.markedY, bm.markedZ,
           bm.markedValid, bm.lastPos, {0, kSpellFxOne, 0});
      stamp(bm.casterId);
    }
    // EVERY N TICKS of a bomb's life (M3), from where the body is.
    bm.age++;
    if (HasTrigger(bm.cast.payload, SpellTrigger::Every)) {
      const std::vector<EffectInst> ev = ItemsOn(bm.cast.payload, SpellTrigger::Every, bm.age);
      const SpellFxVec up{0, kSpellFxOne, 0};
      if (!ev.empty())
        resolve(bm.cast, ev, bm.lastPos, bm.lastPos, up, bm.instability, bm.gen, bm.casterId,
                (uint32_t)tick ^ bm.token ^ 0xE7E7u, false, &up);
    }
    const bool fused = --bm.fuseLeft <= 0;
    const bool expired = --bm.ticksLeft <= 0;
    if (absorbed(bm.cast.delivery.glyph, bm.lastPos)) {
      // Absorbed: the body is handed back without going off.
      refused_++;
      if (bm.body != 0 && !gone) out.bodyDone.push_back(bm.body);
      bombs_[i] = bombs_.back();
      bombs_.pop_back();
      continue;
    }
    if (fused || gone || expired) {
      resolve(bm.cast, ItemsOn(bm.cast.payload, SpellTrigger::Hit), bm.lastPos, bm.lastPos,
              {0, kSpellFxOne, 0}, bm.instability, bm.gen, bm.casterId,
              (uint32_t)tick ^ bm.token, false);
      if (bm.body != 0 && !gone) out.bodyDone.push_back(bm.body);
      bombs_[i] = bombs_.back();
      bombs_.pop_back();
    } else {
      i++;
    }
  }

  // STATUSES: the aura operator's product. Every tick: where the body is now
  // (or the place), run the inner effect there — or put the sustained mod on
  // the body — and bill the caster. Ends when the tick cap runs out or the
  // body is gone; the owner drops it when the caster runs dry.
  for (size_t i = 0; i < statuses_.size();) {
    SpellStatus& st = statuses_[i];
    bool gone = false;
    if (st.target != 0) {
      Vec3 c;
      if (bodies && bodies->bodyPos && bodies->bodyPos(bodies->ctx, st.target, c))
        st.at = {SpellFxFromFloat(c.x), SpellFxFromFloat(c.y), SpellFxFromFloat(c.z)};
      else if (bodies && bodies->bodyPos)
        gone = true;
    }
    if (!gone) {
      if (st.effect.modField != ModField::None) {
        // A sustained MOD acts on the body as if the body were the delivery:
        // gravity is a per-tick impulse; the rest have no meaning on a body
        // and were charged for the word.
        if (st.effect.modField == ModField::Gravity && st.target != 0) {
          int32_t g = 0;
          for (int32_t k = 0; k < st.effect.modN; k++)
            g = ClampI(st.effect.modOp == ModOp::Add
                           ? g + (int32_t)((int64_t)st.effect.modAmount * st.effect.modMag / 1000)
                           : g,
                       -8000, 8000);
          // Per tick: what the record's edit would do to the flight, as a
          // velocity change on the body (voxels/s per tick).
          out.bodyImpulses.push_back({st.target, Vec3{0, -(float)g * 0.012f, 0}});
        }
      } else if (!st.effect.inner.empty()) {
        if (opsUsed < kSpellOpsPerTick) {
          const size_t before = out.ops.size();
          ApplySpellEffect(*lib_, st.effect.inner, st.at, st.dir, 1000, out, &probe,
                           st.instability, (uint32_t)tick ^ st.id);
          opsUsed += (int)(out.ops.size() - before);
          stamp(st.casterId);
        } else {
          opsDropped_++;
        }
      }
      if (st.perTick > 0) out.bills.push_back({st.casterId, st.perTick});
    }
    if (gone || --st.ticksLeft <= 0) {
      statuses_[i] = statuses_.back();
      statuses_.pop_back();
    } else {
      i++;
    }
  }

  // BEAMS: march the ray from the caster's aim to the first solid (or the
  // reach), resolve there, bill per tick.
  for (size_t i = 0; i < beams_.size();) {
    SpellBeam& bm = beams_[i];
    if (!bm.held || --bm.ticksLeft <= 0) {
      beams_[i] = beams_.back();
      beams_.pop_back();
      continue;
    }
    const int32_t reach = std::max(1, bm.cast.delivery.reach);
    const SpellFxVec step = Unit(bm.dir, kSpellFxOne / 2);
    SpellFxVec at = bm.origin;
    for (int32_t k = 0; k < reach * 2; k++) {
      const SpellFxVec next{at.x + step.x, at.y + step.y, at.z + step.z};
      if (solidAt(SpellFxFloor(next.x), SpellFxFloor(next.y), SpellFxFloor(next.z))) {
        at = next;
        break;
      }
      at = next;
    }
    if (opsUsed < kSpellOpsPerTick) {
      const size_t before = out.ops.size();
      ApplySpellEffect(*lib_, bm.cast.payload, at, bm.dir, 1000, out, &probe, bm.instability,
                       (uint32_t)tick);
      opsUsed += (int)(out.ops.size() - before);
      stamp(bm.casterId);
    } else {
      opsDropped_++;
    }
    if (bm.perTick > 0 && !bm.prepaid) out.bills.push_back({bm.casterId, bm.perTick});
    bm.prepaid = false;
    i++;
  }

  // ECHOES: the inner effect again, every period, a bounded number of times.
  for (size_t i = 0; i < echoes_.size();) {
    SpellEcho& ec = echoes_[i];
    if (++ec.phase >= ec.period) {
      ec.phase = 0;
      if (opsUsed < kSpellOpsPerTick) {
        const size_t before = out.ops.size();
        const size_t lbefore = out.launches.size();
        const size_t ebefore = out.echoes.size();
        ApplySpellEffect(*lib_, ec.inner, ec.at, ec.dir, 1000, out, &probe, ec.instability,
                         (uint32_t)tick ^ (uint32_t)i);
        opsUsed += (int)(out.ops.size() - before);
        // The carriers it fires are born exactly as the immediate path would
        // have born them (SpellEcho::launchAt): the caster's, one generation
        // down, from the last free position. An echo nobody stamped (a
        // trail's, a status's) keeps its old reading: from the echo's point.
        const bool ctx = ec.launchGen >= 0;
        for (size_t k = lbefore; k < out.launches.size(); k++) {
          SpellLaunchReq& lq = out.launches[k];
          if (ctx) {
            lq.at = ec.launchAt;
            lq.dir = ec.launchDir;
            lq.generation = ec.launchGen;
          }
          if (lq.casterId == 0) lq.casterId = ec.casterId;
        }
        if (ctx)
          StampEchoLaunches(out, ebefore, ec.launchAt, ec.launchDir, ec.launchGen, ec.casterId);
        stamp(ec.casterId);
      } else {
        opsDropped_++;
      }
      ec.left--;
    }
    if (ec.left <= 0) {
      echoes_[i] = echoes_.back();
      echoes_.pop_back();
    } else {
      i++;
    }
  }

  // FILTERS: one tick each unless an aura re-issued them this tick (below).
  for (size_t i = 0; i < filters_.size();) {
    if (--filters_[i].ticksLeft <= 0) {
      filters_[i] = filters_.back();
      filters_.pop_back();
    } else {
      i++;
    }
  }

  // Whatever this tick's resolves asked for: the carriers they launched, then
  // the statuses, echoes and filters they sustained — attached to the bodies at
  // their points (the owner's probe), priced per tick, capped per caster.
  Adopt(out, bodies, tick, &probe);
}
