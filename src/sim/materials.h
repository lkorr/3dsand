#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Material classes — must match common.wgsl.
enum MatClass : uint32_t {
  CLASS_SOLID = 0,
  CLASS_POWDER = 1,
  CLASS_LIQUID = 2,
  CLASS_GAS = 3,
};

// Material flags — must match common.wgsl.
constexpr uint32_t kMatFlagWander = 1;  // powder scuttles laterally / hops
constexpr uint32_t kMatFlagOpaque = 2;  // liquid renders as surface hit (lava)
// Static micro-detail: the raymarcher substitutes a subdiv^3 brick for this
// material's cells (sim/microvox.h). NOT authored directly in materials.json —
// it is SET BY THE MICRO LOADER on any material whose "micro" block resolved to
// a valid brick, so the flag and the brick table can never disagree.
constexpr uint32_t kMatFlagMicro = 4;
// PASSABLE: a solid that moving bodies pass straight through — pond weed,
// reeds, kelp, and anything else authored as soft vegetation. Authored in
// materials.json as `"passable": true`.
//
// This is a COLLISION property only, and deliberately not a class. The cells
// stay ordinary solids everywhere else: the CA still runs on them, the brush,
// explosions and the laser still remove them, they still burn if flammable,
// and the renderer still draws them as solid geometry. All that changes is
// that the player capsule, mob ground probes and spell projectiles read them
// as empty space rather than as a wall.
//
// Making them a new CLASS instead was the obvious alternative and it is wrong:
// class drives density ordering and the whole displacement/settling model in
// the sim, so a "passable" class would have to re-answer every one of those
// questions for no benefit. A flag on a solid changes exactly the one thing
// that needs changing.
constexpr uint32_t kMatFlagPassable = 8;

// ---- wind coupling, packed into the SAME flags word ------------------------
// docs/RESEARCH_wind.md §4.5, invariant 7. Bits 0..7 are the MATF_* booleans
// above (4 used, 4 spare); bits 8..11 and 12..15 are two authored 4-bit
// numbers; 16..31 are free.
//
// Packed rather than added as fields because MaterialGpu is exactly 64 bytes
// with no spare word, and growing a struct every sim thread reads to buy eight
// bits is the worse trade — the call stainPack already made. `flags`
// specifically is safe because every reader on both sides of the language
// boundary tests it with a MASK; not one compares the word whole.
//
// response: how hard the field pushes this material, 0 (wind does not touch it,
//   and every consumer early-outs) to 15 (it goes where the air goes).
// friction: the ENTRAINMENT threshold — how hard a per-axis wind must blow to
//   pull a SETTLED grain loose. A different axis from response, not a scaling
//   of it: snow lifts in a breeze and then flies far, wet sand needs a gale and
//   then barely moves.
//
// Authored in materials.json as `"wind": {"response": n, "friction": n}`;
// absent means DeriveWindResponse/DeriveWindFriction below pick a default from
// density and class. Never hardcoded per material in a shader (invariant 7).
constexpr uint32_t kMatWindRespShift = 8, kMatWindRespMask = 0xF;
constexpr uint32_t kMatWindFricShift = 12, kMatWindFricMask = 0xF;
constexpr uint32_t kMatWindMax = 15;

// The default when a material authors no "wind" block, so that adding wind did
// not mean editing 96 materials — and so that a NEW material is windy on the
// day it is added rather than inert until someone remembers.
//
// Response goes as 1/density, which is what acceleration under a fixed drag
// force does at a fixed voxel size. The constant is set so a gas saturates at
// 15, dust and snow sit near the top, sand and gravel near the bottom, and
// stone is a nudge. It is a STARTING POINT, not a law: real-world wind
// susceptibility is area-over-mass, i.e. SIZE, and a uniform grid has erased
// size — so iron filings and an iron bar are the same material here and only an
// author can say which one this is. That is why the Powder Toy hand-tunes
// `Advection` per element rather than computing it, and why this is a default
// with an override rather than a formula.
inline uint32_t DeriveWindResponse(int32_t density) {
  int32_t d = density > 0 ? density : 1;
  int32_t r = 4800 / d;
  return (uint32_t)(r > (int32_t)kMatWindMax ? (int32_t)kMatWindMax : r);
}
// Friction rises with density: a settled snowflake lifts in a breeze, gravel
// does not lift at all. Floored at 1 because 0 would mean the faintest
// air movement entrains this material, and "wind never moves it" is what
// response 0 says — two spellings of the same thing is one too many.
inline uint32_t DeriveWindFriction(int32_t density) {
  int32_t d = density > 0 ? density : 1;
  int32_t f = 1 + d / 400;
  return (uint32_t)(f > (int32_t)kMatWindMax ? (int32_t)kMatWindMax : f);
}

// GPU-side layout, 64 bytes — must match struct Material in common.wgsl.
struct MaterialGpu {
  uint32_t klass;
  int32_t density;
  uint32_t color0, color1, color2;  // RGBA8
  uint32_t emission;                // 0..255 glow
  uint32_t flags;
  uint32_t tagMask;
  uint32_t reactOffset, reactCount; // bucket into the flat reaction array
  uint32_t moveEvery;               // viscosity: move only when tick % moveEvery == 0
  uint32_t opacity;                 // 0..255 media absorbance (liquids/gases)
  uint32_t hardness;                // 0..255 blast/dig resistance (DESIGN.md §7)
  uint32_t molten;                  // laser/heat product ID (0 = vaporize to air)
  uint32_t stainPack;               // packed staining behaviour (see below)
  uint32_t stainColor;              // RGBA8 the renderer paints for this stain
};
static_assert(sizeof(MaterialGpu) == 64, "must match common.wgsl Material");

// ---- staining (MaterialGpu.stainPack) --------------------------------------
// A staining liquid marks the voxels it touches with a stain type + amount in
// the voxel word's spare bits (kStain* in world.h), and may CONSUME what it
// stains. Authored in materials.json:
//
//   "stain": { "type": "blood", "color": "#5e0d0d", "amount": 5,
//              "chance": 60, "consume": 8 }
//
// Packed into ONE u32 rather than four, because MaterialGpu had exactly two
// spare words and growing a 64-byte struct that is read by every sim thread is
// a worse trade than four bit-shifts. Layout:
//   bits 0..2   : stain type 1..7 (0 = does not stain) — a palette slot, NOT a
//                 material id; slots are assigned at load in file order so the
//                 renderer can hold a small stain table.
//   bits 3..6   : amount added per contact, 1..15 (voxel amounts saturate at 15)
//   bits 7..16  : per-mille chance per tick to stain a touching neighbour
//   bits 17..26 : per-mille chance a stain CONSUMES the voxel it stained
//   bits 27..30 : ABSORB CAPACITY, 0..15 — see below. Authored on the SUBSTRATE,
//                 not on the stainer, so it shares this word only because there
//                 was room; the two halves are read by opposite sides of the
//                 rule and never by the same material.
//   bit  31     : WASHES — this liquid scrubs FOREIGN stains away instead of
//                 overwriting them with its own (water rinsing blood off).
// 10 bits per chance = 0..1023, which covers the 0..1000 per-mille range the
// rest of the reaction system already speaks.
constexpr uint32_t kStainPackTypeShift = 0, kStainPackTypeMask = 0x7;
constexpr uint32_t kStainPackAmtShift = 3, kStainPackAmtMask = 0xF;
constexpr uint32_t kStainPackChanceShift = 7, kStainPackChanceMask = 0x3FF;
constexpr uint32_t kStainPackConsumeShift = 17, kStainPackConsumeMask = 0x3FF;
constexpr uint32_t kStainPackAbsorbShift = 27, kStainPackAbsorbMask = 0xF;
constexpr uint32_t kStainPackWashesBit = 1u << 31;
constexpr uint32_t kStainChanceMax = 1000;

// ---- absorption (MaterialGpu.stainPack bits 27..30) ------------------------
// How much staining liquid a GROUND material soaks up before the liquid starts
// to persist on top of it as a pool. Authored on the substrate:
//
//   "absorb": { "capacity": 12 }
//
// Capacity is measured in the same 0..15 units as a voxel's stain amount, so
// "grass absorbs 12" literally means "grass accepts stain up to level 12".
// Absent (or 0) = this material never absorbs, and a liquid touching it pools
// immediately — which is every material that predates the feature, including
// every kind of stone.
//
// Why it lives on the substrate and not on the liquid: the liquid's `amount` is
// the per-contact STEP (how fast it soaks in), and capacity is the CEILING (how
// much this ground can hold). Those are genuinely different axes — the same
// rain soaks into sand quickly but shallowly, and into loam slowly but deeply —
// and authoring the ceiling per material PAIR is the N x M explosion tags exist
// to avoid.
constexpr uint32_t kAbsorbCapacityMax = 15;

// The stain palette lives at kStainPaletteBase in the material table — see
// world.h, which holds it because the WGSL prelude is generated from that file.

// Reaction chance resolution — must match REACT_CHANCE_* in common.wgsl.
//
// Chances are AUTHORED in per-mille but stored in units of 1/kReactChanceDen,
// which buys two things: the neighbour-count ramp keeps 6 distinct steps even
// at chance 1 (see kScale* below), and a rule can be authored far below 1
// per-mille. Plain per-mille bottoms out at a mean wait of 1000 ticks ~= 33 s
// at 30 Hz — much too frequent for a "rare ambient event" rule, which is why
// the authored value is allowed to be fractional.
//
// kReactChanceScale must stay a multiple of kScaleMulUnit*5 (= 20) so the ramp
// divide in scaledChance() is exact. At 2000 the finest authorable chance is
// 0.0005 per-mille, a mean wait of ~2e9 ticks (~18.5 h at 30 Hz), and the
// worst-case ramp numerator (den * 95) is ~1.9e8 — 22x inside u32.
constexpr uint32_t kReactChanceScale = 2000;
constexpr uint32_t kReactChanceDen = 1000 * kReactChanceScale;
constexpr double kReactChanceMinMille = 1.0 / (double)kReactChanceScale;

// Formats a per-mille chance for diagnostics without trailing-zero noise
// (0.0005 rather than 0.000500).
std::string FormatMille(double mille);

// Reaction kinds / direction bits — must match common.wgsl.
constexpr uint32_t kReactPair = 0, kReactDecay = 1, kReactEmit = 2;
constexpr uint32_t kDirDown = 1, kDirUp = 2, kDirSide = 4, kDirAny = 7;
constexpr uint32_t kProdKeep = 0xFFFF;
constexpr uint32_t kNbrAny = 0xFFFF;

// Reaction light conditions (ReactionGpu.cond bits 0..7) — must match the
// RCOND_* consts in common.wgsl. Authored in reactions.json as
// "needsSky": true, "when": "day"|"night", "minLight": 0..255.
constexpr uint32_t kCondSky = 1, kCondDay = 2, kCondNight = 4;

// Neighbour-count scaling (ReactionGpu.cond bits 16..31) — see the RSCALE_*
// consts in common.wgsl. Authored as "scaleByNeighbors": {...}.
//
// The rule's chance is scaled by how many of the 6 face neighbours satisfy a
// predicate: 0 matching neighbours means the rule CANNOT fire, and a full 6
// means `scaleMax` times the base chance. That is what turns a uniform
// nucleation rule into a frontier that spreads — a pond freezes from its
// banks inward rather than icing over all at once.
//
// The predicate reuses nbrMat/nbrTags/nbrClass, which a DECAY rule otherwise
// leaves unused, so the counted set is expressed with exactly the vocabulary
// pair rules already use ("neighbor": "water" / "tag:organic" / class list)
// and no new field is needed for the 12-bit id.
constexpr uint32_t kScaleEnable = 1u << 16;  // bit 16: scaling armed
constexpr uint32_t kScaleInvert = 1u << 17;  // count neighbours NOT matching
// Bits 18..19: minCount-1, a floor on the matching count. The default 0 means
// "any count >= 1 fires", which is the original behaviour. Raising it makes a
// rule need a WIDER frontier before it can fire at all, which is what lets
// evaporation say "an exposed droplet boils off, but the flat surface of a
// pond does not". Without it the ramp's only hard gate is at count 0, so a
// pond surface (5 water neighbours, 1 air) always fires at the base rate.
//
// Stored biased by 1 so the common no-threshold case stays the zero value:
//   stored = minCount - 1, range 1..4.
constexpr uint32_t kScaleMinShift = 18, kScaleMinMask = 0x3u;
constexpr uint32_t kScaleMinCountMin = 1, kScaleMinCountMax = 4;
// Bits 20..23 hold the multiplier at a full count of 6, in
// quarters BIASED BY 1.0 — stored = round(scaleMax*4) - 4. A multiplier below
// 1.0x is meaningless (the count gate is what suppresses, not the scale), so
// spending codes on it would waste the field; the bias buys 1.0x..4.75x out
// of 4 bits, which covers the 4x this was built for.
constexpr uint32_t kScaleMulShift = 20, kScaleMulMask = 0xFu;
constexpr uint32_t kScaleMulUnit = 4;  // quarters per 1.0x
constexpr float kScaleMulMin = 1.0f, kScaleMulMax = 4.75f;

// GPU-side layout, 32 bytes — must match struct Reaction in common.wgsl.
struct ReactionGpu {
  uint32_t packed;    // bits 0..1 kind, bits 2..4 dir mask
  uint32_t nbrMat;    // exact neighbor id, or kNbrAny
  uint32_t nbrTags;   // tag mask (nonzero => neighbor matches on any shared tag)
  uint32_t nbrClass;  // bit-per-class filter (1<<klass); 0 = any class
  uint32_t chance;    // per tick, in units of 1/kReactChanceDen (see above)
  uint32_t prodSelf;  // kProdKeep = unchanged, 0 = air
  uint32_t prodNbr;   // pair: neighbor product; emit: emitted material
  // Light/day-phase condition + neighbour-count scaling (was pad — no struct
  // growth).
  //   bits 0..7   : kCondSky | kCondDay | kCondNight
  //   bits 8..15  : minimum daylight strength 0..255 (0 = no floor)
  //   bits 16..19 : kScaleEnable | kScaleInvert | (minCount-1)
  //   bits 20..23 : the multiplier at a full count of 6, quarters biased by 1
  // Zero means unconditional, which is every pre-existing rule.
  uint32_t cond;
};
static_assert(sizeof(ReactionGpu) == 32, "must match common.wgsl Reaction");

constexpr uint32_t kMaxReactions = 4096;

struct MaterialDef {
  std::string name;
  MaterialGpu gpu{};
  std::vector<std::string> tags;
  // What sub-8-voxel islands of this material crumble into (DESIGN.md §7).
  // Empty = default (dust for organics/flammables, gravel otherwise).
  std::string rubble;
  // What the laser/heat melt mode converts this into (stone -> lava,
  // sand -> molten_glass, wood -> fire ...). Empty = vaporize to air.
  std::string molten;
  // Name of the stain this material leaves, if any (materials.json "stain":
  // {"type": ...}). Shared across materials: two liquids naming the same stain
  // get the same palette slot. Empty = this material does not stain.
  std::string stain;
  // How much staining liquid this material soaks up before the liquid pools on
  // top (materials.json "absorb": {"capacity": ...}), in the same 0..15 units
  // as a voxel's stain amount. 0 = never absorbs. Mirrors the top nibble of
  // gpu.stainPack; kept unpacked here for the tuner and the wiki.
  uint32_t absorbCapacity = 0;
  // Unpacked mirrors of gpu.flags bits 8..15 (see kMatWind* above), kept for
  // the tuner and the wiki the way absorbCapacity is — the packed word is the
  // truth, these are for anything that wants to READ the value back without
  // knowing the layout. Always populated, whether authored or derived.
  uint32_t windResponse = 0;
  uint32_t windFriction = 0;
  // Sound sets for this surface, keyed by SLOT ("footstep", "impact",
  // "break", ...). Each value names a set relative to the slot's namespace, so
  // "footstep": "leaf" resolves to the set "footsteps/leaf" — one FOLDER under
  // assets/sounds/ whose files are the interchangeable variants.
  //
  // Authored either as a "sounds" object or, for footsteps only, as the older
  // flat "footstep": "leaf" key, which is still read (and still written by the
  // tuner for materials that already use it). assets/sound_schema.js is the
  // list of slots the tuner offers; the engine only cares that a key it looks
  // up is present.
  //
  // A missing slot is NOT an error: cues.cpp falls back by tag, so a new
  // material is audible the day it is added. Purely presentation — the sim
  // never reads any of this, and an unknown set name is a diagnostic.
  std::map<std::string, std::string> sounds;

  // The named slot, or "" if this material does not author one.
  const std::string& Sound(const char* slot) const {
    static const std::string kNone;
    auto it = sounds.find(slot);
    return it == sounds.end() ? kNone : it->second;
  }
};

// Loads materials.json + reactions.json and compiles them into GPU tables:
// tag strings become bits of a shared registry, reactions are grouped into
// per-material buckets preserving file order (first matching rule wins on the
// GPU, so specific rules must precede tag rules in the file). Returns false
// (with errors filled) on validation failure — modders get diagnostics, not
// silent breakage (DESIGN.md §6). Index in mats == 12-bit material ID; slot 0
// is air.
bool LoadAssets(const std::string& materialsPath, const std::string& reactionsPath,
                std::vector<MaterialDef>& mats, std::vector<ReactionGpu>& reactions,
                std::string& errors);

// Builds the material-id -> COLLISION class table that World::KindAt reads.
//
// This is not just `m.gpu.klass` per material, and the difference is the whole
// point: a material flagged PASSABLE (soft vegetation — reeds, kelp, lilypads)
// is reported as CLASS_GAS here, so KindAt returns Gas and every CPU collision
// path already treats it as empty space. The player capsule sweep, the mob and
// avatar ground probes and the spell projectile march all test against Solid,
// so none of them needs to learn about the flag.
//
// Gas rather than a new kind, because Gas is the class those paths ALREADY
// mean "you can move through this" for — smoke and steam. Reusing it means the
// behaviour drops out of the existing tests instead of adding a fourth case to
// each of them, and a caller that forgets to handle passable simply cannot
// exist.
//
// Everything else still sees a solid: the CA, fire, the brush, explosions, the
// laser and the renderer all read gpu.klass directly and are untouched.
std::vector<uint32_t> BuildCollisionClasses(const std::vector<MaterialDef>& mats);
