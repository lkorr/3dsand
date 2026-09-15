#pragma once
#include <cstdint>

// ============================================================================
// THE IMPACT MODEL — what a strike is MADE OF (docs/PLAN_impact_unarmed.md §2).
//
// Until 2026-09-15 every blow in this engine was a KERF: `EdgeSweep::damage`
// was one number, and the only thing `MeleeSweepDamage` could do with a body
// it met was `Damage` it and `CutLimb` it. A sword and a mace and a fist all
// arrived as the same thing, which is why there was no mace and no fist.
//
// A strike is now THREE PARTS, each a number, resolved by ONE sweep against
// whatever it hit:
//
//   CUT    a kerf. Today's whole wound model, unchanged: the hardness gate on
//          a worn shell, the wedge carve, the blood soak, the blade sever
//          rules (cut-through, hanging by a thread). Plate stops it almost
//          entirely (iron k = 0.05, floored at one lattice cell).
//   BLUNT  trauma. hp charged to the struck limb WITHOUT removing voxels: a
//          bruise (skin rewritten to `gore.bruiseMat`), at most a shallow
//          radial DENT (`bluntCarve`), and NEVER a sever — a blunt hit does
//          not take a limb off, however many land. Against a worn shell it
//          breaks shell voxels (`armorBreak`) and a share TRANSMITS through
//          to the limb underneath (`gear.bluntThrough`). This is the part
//          that makes a mace the answer to armour.
//   BITE   a tear. A rot-blob carve (the same predicate `Mob::RotAtSpawn`
//          draws the undead's holes with), bleeding like a cut, severing
//          only by collapse (enough bites DO take a hand off; a punch never
//          does). If the biter carries an infection (`infectMat`) and the
//          tear exposed FLESH — not a shell, not bone — the exposed tissue is
//          rewritten to that material and its stain smeared over the hole.
//          Armour in the way means no infection: that is the whole reason
//          the classification happens before the rewrite.
//
// WHY NUMBERS AND NOT A KIND. `enum DamageKind { Cut, Blunt }` would make a
// sword pure cut and a mace pure blunt, and neither is true: a sword's flat
// and pommel bruise, a mace with a flange tears skin. A profile of three
// numbers lets `items.json` say "sword: cut 14, blunt 2" and "mace: cut 2,
// blunt 16" and the SAME resolution code does the right thing for both. It
// also means a gauntlet is not a weapon KIND either — it is a profile that
// REPLACES the fist's when the fist under it swings (`Mob::StrikeProfileFor`).
//
// WHERE THE NUMBERS COME FROM. An item's from `items.json` (`damage` is the
// cut part — the key is not renamed, four rows and a gate already use it);
// a NATURAL weapon's (a fist, a set of jaws) from the creature's own
// `natural` block in its sidecar; the infection from the creature's `bite`
// block (`zombie.json`), ORed onto the jaws' profile at swing time. Nothing
// in C++ knows a name — see CLAUDE.md design rule 4.
//
// hp numbers are AT FULL SWING SPEED, scaled by the sweep's speed ramp exactly
// as `damage` always was (melee.h note 2: SPEED IS THE DAMAGE). The 0..1
// fractions scale a tuning radius (`gore.bluntCarveRadius`,
// `gear.bluntDentRadius`) rather than naming a size in voxels, for the
// kVoxelMeters reason every other length in this engine is derived.
//
// This is CPU presentation state feeding the ordinary MutationQueue paths
// (CarveLimb / CarveLimbRadial / StainWoundAs / Damage). It never touches the
// CA and is never hashed.
// ============================================================================
struct StrikeProfile {
  float cut = 0.0f;         // hp at full speed arriving as a KERF
  float blunt = 0.0f;       // hp at full speed arriving as TRAUMA
  float bluntCarve = 0.0f;  // 0..1 of gore.bluntCarveRadius dented from FLESH at full power
                            // (fist 0, gauntlet ~0.35, mace ~0.6). Radial, never a kerf.
  float armorBreak = 0.0f;  // 0..1 of gear.bluntDentRadius broken out of a WORN SHELL
                            // at full power (fist 0, sword 0.05, mace ~0.8)
  float bite = 0.0f;        // hp at full speed arriving as a TEAR
  uint16_t infectMat = 0;   // material the tear REWRITES exposed flesh to (0 = none)
  uint16_t infectStain = 0; // LIQUID whose stain the tear smears over the hole (0 = none)

  bool Any() const { return cut > 0.0f || blunt > 0.0f || bite > 0.0f; }
  // The one number the pre-impact callers wanted: everything that can hurt.
  // Debris (`MeltBodyAt`) and the dev readout use it; the wound model does not.
  float Total() const { return cut + blunt + bite; }
};

// HOW A STRUCK RIG SLOT IS CLASSIFIED before any part of the profile is
// applied. The three-way split is the rig's own (melee.h, MeleeSweepDamage):
// a slot below `AppendedBase()` is FLESH, one at or above it tagged `worn` is a
// SHELL, the one at `HeldSlot()` is a WEAPON (a parry, resolved before this).
// Named here because three resolvers (cut, blunt, bite) and two gates must
// agree on it, and an enum is cheaper to keep agreeing than three `if`s.
enum class StruckKind : uint8_t { Flesh = 0, Shell, Weapon, Debris };

// WHICH RIG PART THE STROKE DRIVER IS MOVING, and how (plan §4).
//   Held   the part holding an item; the arm chain whose effector is that
//          part's parent hand is solved to the driver's hand target and the
//          wrist lays the blade along `bladeDir`. Today's behaviour.
//   Chain  a natural weapon on a chain effector (a fist): the same solve, no
//          wrist steering (there is no blade), the swept edge is the part's own.
//   Aim    a natural weapon on a part in no chain (the jaws): the part and an
//          authored share of the spine are ROTATED so the part's forward lies
//          along the driver's tip direction; the body's travel (the lunge) is
//          what carries it to the target.
enum class StrikeEffectorMode : uint8_t { None = 0, Held, Chain, Aim };
