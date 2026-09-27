#pragma once
// THE ALCHEMY BENCH'S CHEMISTRY TABLE: the world's compiled reaction rules
// and solute species, re-indexed by BENCH SUBSTANCE SLOT so FlaskSim can run
// them on particles, grains and gas pixels (docs/PLAN_alchemy_chemistry.md
// package C). Engine-free on purpose, like flasksim.h: the one conversion from
// the engine's tables (MaterialDef::gpu.reactOffset/reactCount + the
// ReactionGpu vector + MaterialDef::ruleFx + solutes.json) is
// BuildBenchChemistry in flasksim_mats.h. Nothing in here names a material:
// every rule, effect and species arrives as data.
//
// SEMANTICS ARE THE WORLD'S (sim_step.wgsl doReactions): a substance's rules
// are tried IN ORDER, each gated then rolled ONCE (a pair rule against the
// first matching neighbour in a rotated scan, an emit against the first free
// neighbour), and the first that fires wins. Chances are per WORLD TICK
// (30 Hz) in units of 1/chanceDen; FlaskSim scales them to its own step.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace alchemy {

// A rule product that is not a substance slot.
constexpr int kChemKeep = -1;   // unchanged
constexpr int kChemAir = -2;    // nothing: the units are consumed

// Rule kinds and direction bits -- the world's (materials.h kReact*, kDir*).
constexpr uint8_t kChemPair = 0, kChemDecay = 1, kChemEmit = 2;
constexpr uint8_t kChemDown = 1, kChemUp = 2, kChemSide = 4;
constexpr uint32_t kChemNbrAny = 0xFFFF;

struct ChemRule {
  uint8_t kind = kChemPair;
  uint8_t dirs = 7;
  // The neighbour predicate, exactly ReactionGpu's (reactcpu.h
  // ReactNbrMatches): an exact material id, or kChemNbrAny with a tag mask
  // and/or a bit-per-class filter.
  uint32_t nbrMat = kChemNbrAny, nbrTags = 0, nbrClass = 0;
  uint32_t chance = 0;   // per world tick, in 1/Chemistry::chanceDen
  uint32_t cond = 0;     // ReactionGpu::cond (light gate + neighbour-count ramp)
  int prodSelf = kChemKeep;
  int prodNbr = kChemKeep;   // pair: the neighbour's product; emit: what is emitted
  // The rule's effects: Chemistry::effects[fx .. fx + fxCount), in the
  // order reactions.json lists them (-1 / 0 = none). EVERY effect of a rule
  // is raised when it fires, as the world's ReactFxToBlasts loops them all.
  int fx = -1;
  uint8_t fxCount = 0;
  uint32_t worldIndex = 0;   // the rule's index in the world table (diagnostics)
  // A CONCENTRATION CONDITION (reactions.json "solute"/"cMin"/"cMax";
  // materials.h RuleFx::solute): the rule fires only for a LIQUID self
  // carrying species `soluteSpecies` at a concentration in [soluteMin,
  // soluteMax] -- the world's units, 0..255 per full cell (sim_solute.wgsl
  // solRuleAllows). 0 = no condition; kChemSoluteNever = a species the table
  // does not have (the rule can never fire, as in the world).
  uint8_t soluteSpecies = 0;
  uint8_t soluteMin = 0, soluteMax = 255;
};
constexpr uint8_t kChemSoluteNever = 255;
// The concentration, in the world's units, of `mass` dissolved units in a
// particle of `weight` units of liquid (both in bench units: the world's
// c = mass * 8 / fullness is powder-per-liquid times yieldPerVoxel, and the
// bench keeps both sides in the same units, so the ratio carries over).
inline uint32_t ChemConcentration(uint32_t mass, uint32_t weight, uint32_t yieldPerVoxel) {
  if (!weight) return 0;
  return (uint32_t)std::min<uint64_t>(255u, (uint64_t)mass * yieldPerVoxel / weight);
}

// ReactionEffect (sim/materials.h), copied so this header stays engine-free.
struct ChemEffect {
  std::string kind;
  int32_t radius = 0, power = 0;
  float amount = 0;
  std::string what;
};

// A solute species (sim/solutes.h SoluteDef) in bench slots.
struct ChemSolute {
  uint8_t species = 0;           // 1-based
  int from = -1;                 // the powder slot that dissolves
  int precipitate = -1;          // what a boiled-off solvent leaves (-1: `from`)
  uint32_t yieldPerVoxel = 256;  // world units one voxel of `from` becomes
  uint32_t saturation = 255;     // concentration cap, 0..255 per full cell
  uint32_t dissolveChance = 0;   // per-mille per world tick per contact
  uint32_t diffusivity = 0;      // /256
  uint32_t tint = 0;             // 0x00RRGGBB (linear palette colour)
  uint32_t tintStrength = 0;     // 0..255 at saturation
  uint32_t glow = 0;             // 0..255 at saturation
  std::vector<int> solvents;     // liquid slots it dissolves in
  struct Convert { int solvent = -1, into = -1; uint32_t cMin = 0; };
  std::vector<Convert> converts;
  bool DissolvesIn(int slot) const {
    for (int s : solvents)
      if (s == slot) return true;
    return false;
  }
};

// A neighbour that is not matter in the flask: the burner's heat through the
// glass (a `tag:hot` neighbour, no material of its own) and the Electrify
// button's discharge (the `spark` material by tag). Its product, when a rule
// rewrites it, is MATERIALISED where it touched -- the world's spark voxel
// becomes chlorine; the bench's virtual spark leaves chlorine too.
struct ChemVirtual {
  bool on = false;
  uint32_t mat = 0xFFFFFFFFu;   // exact material id it answers to (none: ~0)
  uint32_t tags = 0;            // its tag mask
  uint8_t klass = 3;            // its class (gas)
};

struct Chemistry {
  uint32_t chanceDen = 2000000;                 // materials.h kReactChanceDen
  std::vector<std::vector<ChemRule>> rules;     // per substance slot
  std::vector<ChemEffect> effects;
  std::vector<ChemSolute> solutes;
  ChemVirtual heat, spark;
  // 0..255: the day phase the light-gated rules read (0 = night). A
  // sky- or rain-gated rule never fires on the bench: it is indoors.
  uint32_t daylight = 0;
  bool Empty() const {
    for (const auto& r : rules)
      if (!r.empty()) return false;
    return solutes.empty();
  }
  const ChemSolute* SoluteFrom(int powderSlot) const {
    for (const ChemSolute& s : solutes)
      if (s.from == powderSlot) return &s;
    return nullptr;
  }
  const ChemSolute* Species(uint8_t sp) const {
    return sp >= 1 && sp <= solutes.size() ? &solutes[sp - 1] : nullptr;
  }
};

// The light/weather gate of a rule on the bench (materials.h reactGate with
// no sky, no rain): false = the rule is skipped, exactly as the grid skips it.
inline bool ChemGateOpen(uint32_t cond, uint32_t daylight) {
  const uint32_t c = cond & 0xFFu;
  if (c == 0u) return true;
  if ((c & (1u | 8u)) != 0u) return false;       // kCondSky | kCondRain
  if ((c & 2u) != 0u && daylight == 0u) return false;   // kCondDay
  if ((c & 4u) != 0u && daylight != 0u) return false;   // kCondNight
  if (daylight < ((cond >> 8u) & 0xFFu)) return false;  // minLight
  return true;
}

// The neighbour-count ramp (materials.h kScale*, reactcpu.h
// ReactScaledChance), bit for bit.
inline bool ChemScaleArmed(uint32_t cond) { return (cond & (1u << 16)) != 0; }
inline bool ChemScaleInverted(uint32_t cond) { return (cond & (1u << 17)) != 0; }
inline uint32_t ChemScaledChance(uint32_t chance, uint32_t cond, uint32_t count, uint32_t den) {
  if (!ChemScaleArmed(cond)) return chance;
  const uint32_t minCount = ((cond >> 18) & 3u) + 1u;
  if (count < minCount) return 0;
  const uint32_t maxQ = ((cond >> 20) & 0xFu) + 4u;
  const uint32_t span = maxQ - 4u;
  const uint64_t num = (uint64_t)chance * (20u + span * (count - 1u));
  const uint64_t scaled = num / 20u;
  return (uint32_t)(scaled < den ? scaled : den);
}

// Does a neighbour (material id, tag mask, class) satisfy a rule's
// predicate? reactcpu.h ReactNbrMatches, with the material's fields handed
// in rather than looked up (a virtual neighbour has no row). Air is id 0 and
// only an exact `nbrMat == 0` names it.
inline bool ChemNbrMatches(const ChemRule& r, uint32_t mat, uint32_t tags, uint8_t klass,
                           bool isAir) {
  if (isAir) return r.nbrMat == 0;
  if (r.nbrClass != 0 && ((r.nbrClass >> klass) & 1u) == 0) return false;
  if (r.nbrMat != kChemNbrAny) return mat == r.nbrMat;
  if (r.nbrTags != 0) return (tags & r.nbrTags) != 0;
  return true;
}

}  // namespace alchemy
