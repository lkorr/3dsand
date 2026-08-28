#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "sim/materials.h"
#include "sim/world.h"

// CPU mirror of the reaction GATES in sim_step.wgsl.
//
// WHY THIS EXISTS. One authored reaction table has two evaluators: the GPU runs
// it over the grid, and the CPU runs it over rigid bodies and mob limbs
// (DebrisSystem::BurnBodies, MobSystem::BurnLimbs) because those are CPU state
// no CA pass touches. Two evaluators of one table is a divergence waiting to
// happen, and it already had one: the CPU side read `packed`, `nbrMat`,
// `nbrTags`, `nbrClass`, `chance`, `prodSelf` and `prodNbr` — and never `cond`.
// So a rule authored with `scaleByNeighbors` or a day/night gate fired
// UNCONDITIONALLY AT ITS BASE CHANCE on a body while behaving completely
// differently two voxels away in the grid.
//
// That stayed latent only because no body material used `cond`. Per-voxel body
// burning ends that: its whole "a lone hot voxel gutters out, a wide front
// spreads" mechanic is authored as `scaleByNeighbors` with a `minCount`, and
// without this file that authoring would silently do nothing on the one
// population it was written for. The symptom of a `cond` divergence is never a
// crash — it is "the tuning I authored has no effect".
//
// What lives here is the half that is genuinely shared: the gates and the ramp
// arithmetic. What does NOT live here is where the six neighbours come from —
// the grid reads them from the voxel buffer, a body from its own lattice plus
// the chunk cache — because that is the only part that legitimately differs.
//
// INTEGER THROUGHOUT, matching the shader bit for bit. A float here would round
// differently from the GPU and reintroduce exactly the divergence the file
// exists to close.

// Does material `nmat` satisfy a rule's neighbour predicate? Mirrors
// nbrMatches() in sim_step.wgsl. `g` must be materials[nmat].
//
// Air is deliberately NOT matched by a tag or class predicate: slot 0 has no
// meaningful Material entry, so only an exact `nbrMat == 0` names it. Callers
// hand air in as nmat 0 and this returns false unless the rule asked for it.
inline bool ReactNbrMatches(const ReactionGpu& r, uint32_t nmat,
                            const std::vector<MaterialGpu>& mats) {
  if (nmat == 0 || nmat >= mats.size()) return nmat == r.nbrMat;
  const MaterialGpu& g = mats[nmat];
  if (r.nbrClass != 0 && ((r.nbrClass >> g.klass) & 1u) == 0) return false;
  if (r.nbrMat != kNbrAny) return nmat == r.nbrMat;
  if (r.nbrTags != 0) return (g.tagMask & r.nbrTags) != 0;
  return true;  // wildcard "any"
}

// Light / day-phase gate. Mirrors lightMatches() in sim_step.wgsl.
//
// `seesSky` is the caller's answer to RCOND_SKY, which the shader gets from a
// column raycast it cannot afford to reproduce here. A BODY has no column: it
// is a free-floating lattice that may be in the open, indoors, or halfway down
// a shaft. Callers pass what they can defend; DebrisSystem and MobSystem pass
// true, because a limb or a plank is overwhelmingly outdoors and the
// alternative silently makes every sky-gated rule inert on bodies forever —
// which is the same class of bug (authored rule does nothing) that this file
// exists to prevent.
inline bool ReactLightMatches(const ReactionGpu& r, uint32_t dayPhase,
                              bool seesSky) {
  const uint32_t cond = r.cond & 0xFFu;
  if (cond == 0) return true;  // unconditional: the common case, free
  const uint32_t day = DaylightStrengthCpu(dayPhase);
  if ((cond & kCondDay) != 0 && day == 0) return false;
  if ((cond & kCondNight) != 0 && day != 0) return false;
  const uint32_t minLight = (r.cond >> 8) & 0xFFu;
  if (day < minLight) return false;
  if ((cond & kCondSky) != 0 && !seesSky) return false;
  return true;
}

// Is the neighbour-count ramp armed on this rule? Callers test this BEFORE
// counting: an unscaled rule must not pay for six neighbour lookups.
inline bool ReactScaleArmed(const ReactionGpu& r) {
  return (r.cond & kScaleEnable) != 0;
}
// Does the ramp count neighbours that do NOT match the predicate?
inline bool ReactScaleInverted(const ReactionGpu& r) {
  return (r.cond & kScaleInvert) != 0;
}

// The rule's effective chance in units of 1/kReactChanceDen, given how many of
// the six face neighbours satisfied the (possibly inverted) predicate. Returns
// 0 when the rule cannot fire at all. Mirrors scaledChance() in sim_step.wgsl,
// divide-last and all.
//
// The divide-last shape is not stylistic. The interesting rules are authored at
// chance 1-2, and computing `(chance * q) / 4` first would truncate 1.5x and
// 2.75x onto the same integer, collapsing the six-step ramp to four steps.
inline uint32_t ReactScaledChance(const ReactionGpu& r, uint32_t count) {
  if (!ReactScaleArmed(r)) return r.chance;
  const uint32_t minCount = ((r.cond >> kScaleMinShift) & kScaleMinMask) + 1u;
  if (count < minCount) return 0;  // no frontier, no reaction
  const uint32_t maxQ =
      ((r.cond >> kScaleMulShift) & kScaleMulMask) + kScaleMulUnit;
  const uint32_t span = maxQ - kScaleMulUnit;  // quarters above 1.0x
  const uint32_t num = r.chance * (kScaleMulUnit * 5u + span * (count - 1u));
  const uint32_t scaled = num / (kScaleMulUnit * 5u);
  return std::min(scaled, kReactChanceDen);
}
