#pragma once
// DISSOLVED SUBSTANCES: the species table (assets/materials/solutes.json).
//
// A solute is not a material. A powder that dissolves (salt, fairy dust)
// stops being cells and becomes MASS carried by the liquid it dissolved in;
// concentration = mass / fullness is derived, never stored
// (docs/PLAN_solutes.md section 1). This file is only the DATA: which powder
// dissolves into which liquids, how much of it a cell can hold, how fast it
// spreads, how it tints and weighs the liquid, and what a solvent BECOMES once
// it is concentrated enough (fairy dust in water -> enchanted_water).
//
// Two consumers read the same table so the bench and the world agree:
//   - the world's solute layer (docs/PLAN_solutes.md P1-P3, package B of
//     docs/PLAN_alchemy_chemistry.md),
//   - the alchemy bench (game/flasksim.*: per-particle solute) and the vessel
//     Composition (dissolved portions, game/composition.h kDissolvedBit).
// The unit bridge between them: one EIGHTH of a dissolved portion is
// yieldPerVoxel / 8 units of world solute mass.
#include <cstdint>
#include <string>
#include <vector>

struct MaterialDef;

// Units: a cell's solute MASS is 0..255 at full fullness (8 eighths); one
// voxel of the source powder dissolves into `yieldPerVoxel` units. So a
// concentration of 255 is about one voxel of salt per voxel of water.
struct SoluteConvert {
  uint16_t solvent = 0;   // material id of the liquid
  uint16_t into = 0;      // material id it becomes
  uint32_t cMin = 0;      // concentration (0..255 per full cell) at which it converts
};

struct SoluteDef {
  std::string name;              // species name ("salt", "fairy")
  uint16_t species = 0;          // 1-based index; 0 = none (the aux layer's "no species")
  uint16_t from = 0;             // material id of the powder that dissolves
  uint32_t yieldPerVoxel = 256;  // units one voxel of `from` becomes
  uint32_t saturation = 255;     // max concentration a solvent holds; the rest stays powder
  uint32_t dissolveChance = 60;  // per-mille per tick a grain touching a solvent dissolves (world + bench)
  uint32_t diffusivity = 12;     // pair-exchange rate, /256 (PLAN_solutes 3.3)
  uint32_t floor = 1;            // dilution floor: below this concentration mass is discarded (3.3.1)
  int32_t densityPerUnit = 0;    // density modifier: density += (densityPerUnit * c) >> 8  (4.2)
  uint32_t tint = 0;             // 0x00RRGGBB, linear palette colour the solvent shifts toward
  uint32_t tintStrength = 0;     // 0..255 at saturation
  uint32_t glow = 0;             // 0..255 emission added at saturation
  uint16_t precipitatesTo = 0;   // material a solvent leaves when it evaporates/boils away (0 = nothing)
  std::vector<uint16_t> solvents;       // liquids it dissolves into
  std::vector<SoluteConvert> converts;  // solvent -> material at concentration
  bool DissolvesIn(uint16_t mat) const {
    for (uint16_t s : solvents)
      if (s == mat) return true;
    return false;
  }
};

// Loads and validates solutes.json against a loaded material table (names
// resolve to ids). False with `errors` filled on any bad row; an absent file
// is an empty table and true. Species ids are 1-based in file order.
bool LoadSolutes(const std::string& path, const std::vector<MaterialDef>& mats,
                 std::vector<SoluteDef>& out, std::string& errors);

// The species a powder material dissolves as, or nullptr.
const SoluteDef* SoluteFromPowder(const std::vector<SoluteDef>& table, uint16_t mat);

// THE TABLE THE GPU HOLDS. Set by Simulation::UploadSolutes, the one place the
// species table is uploaded (every materials load and R reload comes through
// it), so a CPU reader -- the vessel seam, a gate's fixture, the selftest op
// stream -- sees exactly the ids the kernels resolve. Empty before the first
// upload. Process-global like CurrentTuning(): there is one GPU table.
const std::vector<SoluteDef>& CurrentSolutes();
void SetCurrentSolutes(const std::vector<SoluteDef>& table);
// The species named `name` in CurrentSolutes(), or nullptr.
const SoluteDef* CurrentSoluteNamed(const char* name);
// The species with 1-based id `species` in CurrentSolutes(), or nullptr.
const SoluteDef* CurrentSoluteById(uint32_t species);
