#pragma once
// ============================================================================
// ONE DAMAGE EVENT, ONE SHELL RESPONSE (W2-H,
// docs/PLAN_rule_unification_2026-09-24.md; the cause is phys/damagecause.h).
//
// What a worn shell (a cuirass, a robe, a helm: a rig slot tagged `worn`,
// Mob::IsWornSlot) does to a blow is a function of TWO things and nothing
// else: what the shell is made of (its material hardness, materials.json
// 0..255 -- the same field the blast crater and the dig read) and what the
// blow is (the DamageCause). Until 2026-09-24 that function existed three
// times, with three curves and three tuning pairs, in Mob::CutLimb
// (cutHardnessRef/Min), Mob::BluntHit (bluntHardnessRef/Min) and Mob::BiteHit
// (biteThroughSoft/Hard) -- and every other source (the blast, the laser, a
// landing) ignored shells altogether. All three read `skinVoxels[0]`, the
// FIRST voxel of the lattice, instead of the voxel the blow struck.
//
// Now there is one table (ShellRowOf, below: one row per cause, its numbers
// read from Tuning::Gear so the knobs every gate pins are the knobs that still
// exist) and one resolver (ShellResponseOf). The callers read the material of
// the voxel they STRUCK (Mob::ShellMaterialAt) and ask.
//
// WHAT THE ANSWER MEANS -- four numbers, each a scale on something the caller
// already does, so no caller gained a new mechanic by adopting it:
//
//   carve    how much of the blow's MATTER REMOVAL the shell takes, as a scale
//            on the caller's own geometry: the kerf's depth and length (Blade),
//            the dent's radius (Blunt), nothing (Bite: teeth do not dent
//            plate), the bore (Beam). For Blast it is 1 and the shell's
//            hardness acts per voxel through the terrain rule instead
//            (Mob::CarveLimbRadial's blast power), which is the point of Blast
//            having its own curve kind.
//   passed   the share of the blow's hp that arrives on the limb UNDER the
//            shell (MobLimb::wornHost): 0 for an edge (it chips, it does not
//            transmit), gear.bluntThrough for trauma, the bite ramp for teeth.
//   shellHp  the share of the blow's hp the SHELL is charged as its own hp.
//   stop     BLAST ONLY: the power, in sim_explode.wgsl's own units, a ray
//            loses crossing this shell -- hardness x gear.blastShellCells, the
//            same "sum the hardness of everything in between" the terrain
//            crater walks. 0 for every other cause.
//
// The curves, and why there are several: a RATIO (ref / hardness, floored,
// capped at 1) has no zero -- a kerf into iron is a twentieth of a kerf, never
// none -- where a RAMP (1 at or below `soft`, 0 at or above `hard`, linear
// between) does, which is what "plate stops a bite dead" needs. FLAT ignores
// the material (the transmitted share of a mace blow is the same through
// cloth and iron: the shell deforming IS how it arrives).
// ============================================================================
#include <algorithm>
#include <cstdint>

#include "phys/damagecause.h"
#include "sim/tuning.h"

enum class ShellCurve : uint8_t {
  One = 0,  // 1 whatever the material
  Zero,     // 0 whatever the material
  Flat,     // `a`, whatever the material
  Ratio,    // clamp(a / hardness, clamp(b, 0, 1), 1); hardness 0 (unknown) -> 1
  Ramp,     // 1 at or below a, 0 at or above b, linear between
};

struct ShellCurveSpec {
  ShellCurve kind = ShellCurve::One;
  float a = 0.0f;
  float b = 0.0f;
};

struct ShellResponse {
  float carve = 1.0f;
  float passed = 0.0f;
  float shellHp = 1.0f;
  float stop = 0.0f;
};

// One row per cause. `stopPerHardness` is the Blast column: power lost per
// unit of shell hardness when a ray crosses the shell (0 = the shell does not
// occlude this cause).
struct ShellRow {
  DamageCause cause = DamageCause::Other;
  ShellCurveSpec carve, passed, shellHp;
  float stopPerHardness = 0.0f;
};

inline float EvalShellCurve(const ShellCurveSpec& c, float hardness) {
  switch (c.kind) {
    case ShellCurve::One:
      return 1.0f;
    case ShellCurve::Zero:
      return 0.0f;
    case ShellCurve::Flat:
      return c.a;
    case ShellCurve::Ratio:
      // A shell whose material could not be read (hardness 0) and a ratio
      // switched off (ref 0) both answer "as if flesh" -- the failure is a blow
      // that lands, never a creature that is silently invulnerable.
      if (hardness <= 0.0f || c.a <= 0.0f) return 1.0f;
      return std::clamp(c.a / hardness, std::clamp(c.b, 0.0f, 1.0f), 1.0f);
    case ShellCurve::Ramp: {
      if (hardness <= c.a) return 1.0f;
      const float span = std::max(c.b - c.a, 1e-3f);
      return std::clamp((c.b - hardness) / span, 0.0f, 1.0f);
    }
  }
  return 1.0f;
}

// ---- THE TABLE -------------------------------------------------------------
//
//   cause     carve                         passed               shellHp           stop
//   Other     1                             0                    1                 -
//   Blade     Ratio(cutHardnessRef, Min)    0                    1                 -
//   Blunt     Ratio(bluntHardnessRef, Min)  Flat(bluntThrough)   Flat(bluntShellHp) -
//   Unarmed   = Blunt
//   Bite      0                             Ramp(biteThroughSoft, Hard)  Flat(biteOnShell) -
//   Beam      1                             0                    1                 -
//   Blast     1 (per-voxel terrain rule)    1                    1                 hardness x blastShellCells
//   Burn      1                             0                    1                 -   (never asked: the burn
//   SpawnRot  1                             0                    1                 -    reads coats, not shells)
//   Fall      0                             1                    0                 -
//
// Blade/Blunt/Bite are TODAY'S NUMBERS, moved, not changed: the melee gates
// pin them. Beam's row is the identity the laser always had (a shell in the
// beam is bored like flesh and takes the hp itself). Fall's says a plate does
// not soften a landing and is not charged for it.
//
// Bite's shellHp is `biteOnShell` of the bite delivered AS A BLUNT BLOW
// (Mob::BiteHit hands it to Mob::BluntHit), which then applies the Blunt row
// to it -- the chain the shipped tuning was written against.
inline ShellRow ShellRowOf(DamageCause cause, const Tuning::Gear& g) {
  ShellRow r;
  r.cause = cause;
  switch (cause) {
    case DamageCause::Blade:
      r.carve = {ShellCurve::Ratio, g.cutHardnessRef, g.cutHardnessMin};
      r.passed = {ShellCurve::Zero};
      r.shellHp = {ShellCurve::One};
      break;
    case DamageCause::Blunt:
    case DamageCause::Unarmed:
      r.carve = {ShellCurve::Ratio, g.bluntHardnessRef, g.bluntHardnessMin};
      r.passed = {ShellCurve::Flat, g.bluntThrough};
      r.shellHp = {ShellCurve::Flat, g.bluntShellHp};
      break;
    case DamageCause::Bite:
      r.carve = {ShellCurve::Zero};
      r.passed = {ShellCurve::Ramp, g.biteThroughSoft, g.biteThroughHard};
      r.shellHp = {ShellCurve::Flat, g.biteOnShell};
      break;
    case DamageCause::Blast:
      r.carve = {ShellCurve::One};
      r.passed = {ShellCurve::One};
      r.shellHp = {ShellCurve::One};
      r.stopPerHardness = std::max(g.blastShellCells, 0.0f);
      break;
    case DamageCause::Fall:
      r.carve = {ShellCurve::Zero};
      r.passed = {ShellCurve::One};
      r.shellHp = {ShellCurve::Zero};
      break;
    case DamageCause::Other:
    case DamageCause::Beam:
    case DamageCause::Burn:
    case DamageCause::SpawnRot:
    case DamageCause::Infection:
    case DamageCause::Count:
      r.carve = {ShellCurve::One};
      r.passed = {ShellCurve::Zero};
      r.shellHp = {ShellCurve::One};
      break;
  }
  return r;
}

inline ShellResponse ShellResponseOf(float hardness, DamageCause cause,
                                     const Tuning::Gear& g) {
  const ShellRow r = ShellRowOf(cause, g);
  ShellResponse s;
  s.carve = EvalShellCurve(r.carve, hardness);
  s.passed = EvalShellCurve(r.passed, hardness);
  s.shellHp = EvalShellCurve(r.shellHp, hardness);
  s.stop = r.stopPerHardness * std::max(hardness, 0.0f);
  return s;
}

inline ShellResponse ShellResponseOf(float hardness, const DamageCtx& ctx,
                                     const Tuning::Gear& g) {
  return ShellResponseOf(hardness, ctx.cause, g);
}
