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
  uint32_t pad3, pad4;
};
static_assert(sizeof(MaterialGpu) == 64, "must match common.wgsl Material");

// Reaction kinds / direction bits — must match common.wgsl.
constexpr uint32_t kReactPair = 0, kReactDecay = 1, kReactEmit = 2;
constexpr uint32_t kDirDown = 1, kDirUp = 2, kDirSide = 4, kDirAny = 7;
constexpr uint32_t kProdKeep = 0xFFFF;
constexpr uint32_t kNbrAny = 0xFFFF;

// GPU-side layout, 32 bytes — must match struct Reaction in common.wgsl.
struct ReactionGpu {
  uint32_t packed;    // bits 0..1 kind, bits 2..4 dir mask
  uint32_t nbrMat;    // exact neighbor id, or kNbrAny
  uint32_t nbrTags;   // tag mask (nonzero => neighbor matches on any shared tag)
  uint32_t nbrClass;  // bit-per-class filter (1<<klass); 0 = any class
  uint32_t chance;    // per-mille per tick
  uint32_t prodSelf;  // kProdKeep = unchanged, 0 = air
  uint32_t prodNbr;   // pair: neighbor product; emit: emitted material
  uint32_t pad;
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
