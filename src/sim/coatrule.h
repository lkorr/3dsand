#pragma once
// ---- THE COAT RULE AND STAIN PRECEDENCE, CPU twins (rule-unification W2-J2) --
//
// DESIGN.md §6 "A coat is a co-located virtual neighbour" is ONE rule for two
// populations: the grid (sim_step.wgsl coatReact / doReactions / scaledChance,
// and common.wgsl stainStep) and every body (MobSystem::BurnOneLimb, and the
// body-stain writers in phys/bodystain.*). The parts of that rule that are pure
// arithmetic -- what a matched coat rule does to the cover and to the coat's
// amount, how a coat counts in a neighbour-count ramp, and which of two stains
// wins a voxel -- are written ONCE here and ONCE in WGSL, token for token, and
// scripts/check_invariants.py `coatrule` / `stainprec` compares the streams and
// the constants they read. Everything else (where the partners come from, how a
// product is written) is each population's own plumbing and is not mirrored.
//
// Written in the shader's spelling (lower-camel names, braces on every if)
// BECAUSE it is compared as text.

#include <algorithm>
#include <cstdint>

// ---- coatRuleVerdict: rules 2 and 3 for one MATCHED coat rule ---------------
// `flame`: the coat side's product is a flame (kMatFlagFlame). `openFace`: the
// wearer has a face a released flame can go into (air, else a gas). A flame
// with nowhere to go is not a match at all (rule 2); a matched rule whose coat
// side is not a flame COVERS the wearer for the tick (rule 3).
constexpr uint32_t kCoatVerdictMatch = 1u;
constexpr uint32_t kCoatVerdictCovers = 2u;
// ---- stainPrecedence: which of two stains owns a voxel ----------------------
constexpr uint32_t kStainPrecRefuse = 0u;   // a foreign stain stays; no write
constexpr uint32_t kStainPrecOwn = 1u;      // clean or this stain: climb (caller's ceiling)
constexpr uint32_t kStainPrecRinse = 2u;    // a washer steps the foreign stain DOWN
constexpr uint32_t kStainPrecOver = 3u;     // the foreign stain is displaced

// MIRROR-BEGIN coatrule
inline uint32_t coatRuleVerdict(bool flame, bool openFace) {
  if (flame && !openFace) { return 0u; }
  if (flame) { return kCoatVerdictMatch; }
  return kCoatVerdictMatch | kCoatVerdictCovers;
}
// The coat's amount after a firing that costs `price` levels (rule 2: one; a
// DEEP coat eating matter: its layer price, clause 7).
inline uint32_t coatLevelsAfter(uint32_t amt, uint32_t price) {
  if (amt > price) { return amt - price; }
  return 0u;
}
// A direct neighbour-count ramp's count with the coat in it (rule 4): the
// matching faces, plus the coat as ONE more when it matches, capped at 6; on a
// lattice finer than the world (clause 4a) a matching coat counts as a face
// WIDENED at world pitch -- the face and its four tangential neighbours, 5 --
// taken as a max, exactly as a face into world matter is widened.
inline uint32_t coatRampCount(uint32_t faces, bool coatHit, bool pitchFine) {
  if (!coatHit) { return std::min(faces, 6u); }
  uint32_t n = std::min(faces + 1u, 6u);
  if (pitchFine) { n = std::max(n, 5u); }
  return n;
}
// MIRROR-END coatrule

// ONE stain-precedence rule for the ground and the body. `curAmt` / `sameType`:
// what the voxel wears now and whether it is this stain. `newAmt`: the level the
// write would lay. `paid`: the level is substance (every body coat; every
// replacement on the ground, which costs the liquid an eighth) rather than a
// free mark. THE NEWEST STAIN WINS (2026-09-25): a washer RINSES a foreign
// stain (the ground in one contact, a body at the washer's rinse rate); any
// other stainer REPLACES it with a paid level -- except that nothing that is
// not corrosive paints over a corrosive coat. The termination argument
// (DESIGN.md §6 "Absorption and washing") is mass: every replacement is paid,
// so two liquids fighting over one voxel drain each other and stop.
// MIRROR-BEGIN stainprec
inline uint32_t stainPrecedence(uint32_t curAmt, bool sameType, uint32_t newAmt, bool washes,
                   bool corrodes, bool curCorrodes, bool paid) {
  if (curAmt == 0u || sameType) { return kStainPrecOwn; }
  if (washes) { return kStainPrecRinse; }
  if (curCorrodes && !corrodes) { return kStainPrecRefuse; }
  if (paid) { return kStainPrecOver; }
  if (newAmt > curAmt) { return kStainPrecOver; }
  return kStainPrecRefuse;
}
// MIRROR-END stainprec
