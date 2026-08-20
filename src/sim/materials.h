#pragma once
#include <cstdint>
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
// 10 bits per chance = 0..1023, which covers the 0..1000 per-mille range the
// rest of the reaction system already speaks.
constexpr uint32_t kStainPackTypeShift = 0, kStainPackTypeMask = 0x7;
constexpr uint32_t kStainPackAmtShift = 3, kStainPackAmtMask = 0xF;
constexpr uint32_t kStainPackChanceShift = 7, kStainPackChanceMask = 0x3FF;
constexpr uint32_t kStainPackConsumeShift = 17, kStainPackConsumeMask = 0x3FF;
constexpr uint32_t kStainChanceMax = 1000;

// The stain palette lives at kStainPaletteBase in the material table — see
// world.h, which holds it because the WGSL prelude is generated from that file.

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
  uint32_t chance;    // per-mille per tick
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
