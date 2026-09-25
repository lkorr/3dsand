#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/materials.h"
#include "sim/reactcpu.h"

// ---- WHAT A MATERIAL CAN DO ON A BODY, DERIVED ONCE (W2-I, 2026-09-24) -------
//
// The per-material columns every CPU body-reaction consumer gates on: which
// materials are ALIGHT (an ungated self rule), which only react under a
// neighbour-count ramp, which can be ignited or doused (pair rules), which
// rewrite their neighbour (acid), which some rewrite rule can land on, and
// which are hot. All of it is a pure function of materials.json +
// reactions.json, rebuilt on every materials reload.
//
// ONE BUILDER, because there used to be two: MobSystem::OnMaterialsReloaded
// and DebrisSystem::OnMaterialsReloaded each walked the reaction table and
// derived their own flag columns ("the debris twins of mob.h's"), and the
// split that mattered most -- an ungated self rule is alight, a
// neighbour-count-gated one is NOT (flesh_charred, flesh_cooked,
// cloth_charred) -- existed on the debris side for a month before the living
// side made the same fix by hand (W1-F). A column added here reaches every
// population on the next build; a column added to one system's loop did not.
//
// Nothing here names a material or a tag other than the two the contract
// is written in (`hot`: a heat source; `dissolvable`: body matter a
// rewrite can land on), and both are recovered by NAME from the compiled
// masks, the way MobSystem always did.
struct BodyReactFlags {
  // Has an UNGATED decay/emit rule: it changes on its own clock, i.e. it is
  // alight / on the front (ember, flesh_burning, blood drying).
  std::vector<uint8_t> selfActive;
  // Has a decay/emit rule behind a neighbour-count ramp: inert until
  // something hot sits next to it (charred flesh relighting). Counted apart
  // so a body of nothing but char sleeps -- the light-gated-rules-never-sleep
  // trap wearing a different condition.
  std::vector<uint8_t> selfScaled;
  // Has pair rules (ignitable, dousable).
  std::vector<uint8_t> hasPair;
  // Has ANY scaleByNeighbors rule.
  std::vector<uint8_t> hasScaled;
  // Has a pair rule that REWRITES ITS NEIGHBOUR: it can act on a body from
  // the grid (acid, a solvent).
  std::vector<uint8_t> rewritesNbr;
  // Some material's rewrite rule matches THIS one: a voxel of it is something
  // the grid can eat, which is what decides whether a body needs the solvent
  // probe at all.
  std::vector<uint8_t> inboundTarget;
  // Carries tag:hot.
  std::vector<uint8_t> hot;
  // A rewrite of this material could land on BODY matter (its neighbour
  // predicate can match tag:dissolvable, or is a wildcard). Narrower than
  // rewritesNbr on purpose: `grass + tag:soil -> grass` rewrites a neighbour
  // and is under every creature in the world.
  std::vector<uint8_t> attacksBody;
  // What a voxel of this material CATCHES as when a flame is made on it
  // (DESIGN.md §6 clause 2c; CatchFormTable below). 0 = it does not catch.
  std::vector<uint32_t> catchForm;
};

// The BIT one tag name owns, recovered from the compiled masks: the bits
// common to every material carrying the tag, minus every bit any material
// without it carries. Exact, and it needs no access to the TagRegistry.
inline uint32_t BodyReactTagBit(const std::vector<MaterialDef>& mats,
                                const char* name) {
  uint32_t all = 0xFFFFFFFFu, none = 0;
  bool any = false;
  for (const auto& m : mats) {
    bool has = false;
    for (const auto& t : m.tags)
      if (t == name) has = true;
    if (has) {
      all &= m.gpu.tagMask;
      any = true;
    } else {
      none |= m.gpu.tagMask;
    }
  }
  return any ? (all & ~none) : 0u;
}

// ---- WHAT A MATERIAL CATCHES AS (DESIGN.md §6 clause 2c) --------------------
// A coat rule whose coat-side product is a FLAME makes that flame ON the voxel
// wearing it (an oiled plank's oil flashing), and the voxel CATCHES: it takes
// the form its OWN rules give it under heat. Read off the table, never named:
//   * a HEAT rule is a pair rule whose partner predicate matches some hot
//     material, or a decay behind a direct neighbour-count ramp that counts
//     one (flesh_cooked's minCount-3 relight);
//   * the catch form is the product of the material's first heat rule whose
//     product is itself hot (wood -> ember, grass -> fire, leaves ->
//     leaf_burning); failing that, the catch form of the product of its first
//     heat rule (skin -> flesh_cooked -> flesh_burning: the sear, then the
//     relight), one step only;
//   * 0 = does not catch: a material that is already hot, one with no heat
//     rule (dirt, sand, stone), or one whose heat product never burns (ice ->
//     water -> steam, which is not hot).
// ONE derivation for both populations: Simulation::UploadTables puts it in
// each material's spare GPU word for sim_step.wgsl coatReact, and
// BuildBodyReactFlags hands it to MobSystem::BurnOneLimb. Returns one id per
// material, sized mats.size().
inline std::vector<uint32_t> CatchFormTable(const std::vector<MaterialDef>& mats,
                                            const std::vector<ReactionGpu>& reactions) {
  const size_t n = mats.size();
  const uint32_t hotMask = BodyReactTagBit(mats, "hot");
  std::vector<MaterialGpu> gpu;
  gpu.reserve(n);
  for (const auto& m : mats) gpu.push_back(m.gpu);
  auto isHot = [&](uint32_t id) {
    return id != 0 && id < n && (gpu[id].tagMask & hotMask) != 0;
  };
  auto heatRule = [&](const ReactionGpu& r) {
    const uint32_t kind = r.packed & 3u;
    if (kind == kReactDecay && (!ReactScaleArmed(r) || ReactScaleInverted(r)))
      return false;
    if (kind != kReactPair && kind != kReactDecay) return false;
    for (size_t h = 1; h < n; h++)
      if (isHot((uint32_t)h) && ReactNbrMatches(r, (uint32_t)h, gpu)) return true;
    return false;
  };
  // (first hot product of a heat rule, first product of a heat rule)
  auto direct = [&](uint32_t mi, uint32_t& hot, uint32_t& first) {
    hot = first = 0;
    const MaterialGpu& g = gpu[mi];
    for (uint32_t ri = 0; ri < g.reactCount; ri++) {
      const size_t k = (size_t)g.reactOffset + ri;
      if (k >= reactions.size()) break;
      const ReactionGpu& r = reactions[k];
      if (r.prodSelf == kProdKeep || r.prodSelf == 0 || !heatRule(r)) continue;
      const uint32_t pm = r.prodSelf & 0xFFFu;
      if (!first) first = pm;
      if (isHot(pm)) { hot = pm; return; }
    }
  };
  std::vector<uint32_t> out(n, 0u);
  for (size_t mi = 1; mi < n; mi++) {
    if (isHot((uint32_t)mi)) continue;
    uint32_t hot = 0, first = 0;
    direct((uint32_t)mi, hot, first);
    if (!hot && first && first < n && first != mi && !isHot(first)) {
      uint32_t hot2 = 0, first2 = 0;
      direct(first, hot2, first2);
      hot = hot2;
    }
    out[mi] = hot;
  }
  return out;
}

inline void BuildBodyReactFlags(const std::vector<MaterialDef>& mats,
                                const std::vector<ReactionGpu>& reactions,
                                BodyReactFlags& out) {
  const size_t n = mats.size();
  out.selfActive.assign(n, 0);
  out.selfScaled.assign(n, 0);
  out.hasPair.assign(n, 0);
  out.hasScaled.assign(n, 0);
  out.rewritesNbr.assign(n, 0);
  out.inboundTarget.assign(n, 0);
  out.hot.assign(n, 0);
  out.attacksBody.assign(n, 0);
  out.catchForm = CatchFormTable(mats, reactions);
  const uint32_t hotMask = BodyReactTagBit(mats, "hot");
  const uint32_t dissolvableMask = BodyReactTagBit(mats, "dissolvable");
  std::vector<MaterialGpu> gpu;
  gpu.reserve(n);
  for (const auto& m : mats) gpu.push_back(m.gpu);
  for (size_t mi = 0; mi < n; mi++) {
    const MaterialGpu& g = mats[mi].gpu;
    for (uint32_t ri = 0; ri < g.reactCount; ri++) {
      const ReactionGpu& r = reactions[g.reactOffset + ri];
      const uint32_t kind = r.packed & 3u;
      if (ReactScaleArmed(r)) out.hasScaled[mi] = 1;
      if (kind == kReactDecay || kind == kReactEmit) {
        if (ReactScaleArmed(r)) out.selfScaled[mi] = 1;
        else out.selfActive[mi] = 1;
      }
      if (kind == kReactPair) {
        out.hasPair[mi] = 1;
        if (r.prodNbr != kProdKeep) {
          out.rewritesNbr[mi] = 1;
          // A wildcard predicate matches everything, so a body too.
          if ((r.nbrMat == kNbrAny && r.nbrTags == 0) ||
              (r.nbrTags & dissolvableMask) ||
              (r.nbrMat != kNbrAny && r.nbrMat < n &&
               (mats[r.nbrMat].gpu.tagMask & dissolvableMask)))
            out.attacksBody[mi] = 1;
        }
      }
    }
    out.hot[mi] = (g.tagMask & hotMask) != 0 ? 1 : 0;
  }
  // Which materials some rewrite rule can land on: a second pass, because the
  // predicate test needs the whole table.
  for (size_t sm = 1; sm < n; sm++) {
    if (!out.rewritesNbr[sm]) continue;
    const MaterialGpu& g = mats[sm].gpu;
    for (uint32_t ri = 0; ri < g.reactCount; ri++) {
      const ReactionGpu& r = reactions[g.reactOffset + ri];
      if ((r.packed & 3u) != kReactPair || r.prodNbr == kProdKeep) continue;
      for (size_t tm = 1; tm < n; tm++)
        if (ReactNbrMatches(r, (uint32_t)tm, gpu)) out.inboundTarget[tm] = 1;
    }
  }
}
