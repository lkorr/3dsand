#pragma once
// The one conversion from the 3D sim's material table to the alchemy bench's
// substances (flasksim.h). Everything the bench knows about a material comes
// through here, so "a liquid behaves in the flask the way it behaves in the
// world" has one place to be true: class, density (the ORDER of who sinks),
// moveEvery (thickness) and the three palette colours -- and since
// 2026-09-27 (docs/PLAN_alchemy_chemistry.md package C) its tags, its
// reaction rules, their effects and the solute table (BuildBenchChemistry).
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "game/benchchem.h"
#include "game/flasksim.h"
#include "sim/materials.h"
#include "sim/solutes.h"

namespace alchemy {

// Can a vessel hold this material on the bench? Liquids and powders always;
// a GAS too (a stoppered flask of chlorine); a solid only as what a reaction
// made of something that could (BenchSubstances registers those).
inline bool BenchHolds(const MaterialDef& m) {
  return m.gpu.klass == CLASS_LIQUID || m.gpu.klass == CLASS_POWDER || m.gpu.klass == CLASS_GAS;
}

inline Substance SubstanceFromMaterial(const MaterialDef& m, uint16_t id) {
  Substance s;
  s.mat = id;
  s.klass = (uint8_t)m.gpu.klass;
  // A solid on the bench is a reaction's product (lava + water = stone): it
  // is kept as grains.
  s.powder = m.gpu.klass == CLASS_POWDER || m.gpu.klass == CLASS_SOLID;
  s.gas = m.gpu.klass == CLASS_GAS;
  s.heavy = s.gas && (m.gpu.flags & kMatFlagHeavyGas) != 0;
  s.tagMask = m.gpu.tagMask;
  s.density = m.gpu.density;
  s.moveEvery = m.gpu.moveEvery ? m.gpu.moveEvery : 1;
  s.color[0] = m.gpu.color0 | 0xFF000000u;
  s.color[1] = m.gpu.color1 | 0xFF000000u;
  s.color[2] = m.gpu.color2 | 0xFF000000u;
  // The look, from the fields the world's renderer reads for it.
  s.emission = (uint8_t)std::min<uint32_t>(m.gpu.emission, 255);
  s.opacity = (uint8_t)std::min<uint32_t>(m.gpu.opacity, 255);
  s.opaque = (m.gpu.flags & kMatFlagOpaque) != 0;
  return s;
}

// Every material the bench may meet in one session: every liquid, powder
// and gas, plus -- to closure -- whatever a rule of one of those can turn
// something into (a solid product is grains). Registered once at Open, so a
// vessel brought in later needs no re-register. 254 at most (a gas pixel
// keeps its slot in a byte, 255 = none).
inline std::vector<Substance> BenchSubstances(const std::vector<MaterialDef>& mats,
                                              const std::vector<ReactionGpu>& reactions) {
  std::vector<uint8_t> in(mats.size(), 0);
  for (size_t i = 1; i < mats.size(); i++) in[i] = BenchHolds(mats[i]);
  for (bool grew = true; grew;) {
    grew = false;
    for (size_t i = 1; i < mats.size(); i++) {
      if (!in[i]) continue;
      const MaterialGpu& g = mats[i].gpu;
      for (uint32_t k = 0; k < g.reactCount && g.reactOffset + k < reactions.size(); k++) {
        const ReactionGpu& r = reactions[g.reactOffset + k];
        for (uint32_t p : {r.prodSelf, r.prodNbr})
          if (p != kProdKeep && p != 0 && p < mats.size() && !in[p]) { in[p] = 1; grew = true; }
      }
    }
  }
  std::vector<Substance> out;
  for (size_t i = 1; i < mats.size() && out.size() < 254; i++)
    if (in[i]) out.push_back(SubstanceFromMaterial(mats[i], (uint16_t)i));
  return out;
}

// Substances for every material named in `comps`, deduplicated, in first-seen
// order. A material the bench cannot hold (a solid that somehow got into a
// vessel) is skipped; FlaskSim then never seeds it and its amount is reported
// back untouched by the caller. A dissolved portion names its powder.
inline std::vector<Substance> SubstancesFor(const std::vector<MaterialDef>& mats,
                                            const std::vector<const Composition*>& comps) {
  std::vector<Substance> out;
  for (const Composition* c : comps) {
    if (!c) continue;
    for (int i = 0; i < c->n; i++) {
      uint16_t id = BaseMat(c->p[i].mat);
      if (id >= mats.size() || !BenchHolds(mats[id])) continue;
      bool seen = false;
      for (const Substance& s : out) seen |= s.mat == id;
      if (!seen) out.push_back(SubstanceFromMaterial(mats[id], id));
    }
  }
  return out;
}

// The bit a tag NAME has in MaterialGpu::tagMask, read back off the table:
// the bits every material carrying the tag shares, minus every bit a
// material without it has. 0 when no material carries the tag.
inline uint32_t TagBitByName(const std::vector<MaterialDef>& mats, const std::string& tag) {
  uint32_t with = 0xFFFFFFFFu, without = 0;
  bool any = false;
  for (size_t i = 1; i < mats.size(); i++) {
    const bool has = std::find(mats[i].tags.begin(), mats[i].tags.end(), tag) != mats[i].tags.end();
    if (has) { with &= mats[i].gpu.tagMask; any = true; }
    else without |= mats[i].gpu.tagMask;
  }
  if (!any) return 0;
  const uint32_t bit = with & ~without;
  return bit ? bit : with;
}

// THE WORLD'S RULES IN BENCH SLOTS. Every substance's bucket of the compiled
// table (MaterialDef::gpu.reactOffset/reactCount -- exactly what the body
// evaluators read, synthesized tail rules included) with its products turned
// into slots and its effects (MaterialDef::ruleFx, parallel to the bucket)
// copied by kind name. A rule whose product the bench has no slot for is
// dropped (the table is full; it cannot happen below 254 substances).
//
// The two virtual neighbours are resolved BY TAG, not by name: the burner's
// heat is "anything tagged hot" (the ignition rules' own predicate), the
// discharge is the gas tagged electric (the contract's `spark`).
// `solutes` is solutes.json (sim/solutes.h), which the world reads too.
inline Chemistry BuildBenchChemistry(const std::vector<MaterialDef>& mats,
                                     const std::vector<ReactionGpu>& reactions,
                                     const std::vector<SoluteDef>& solutes,
                                     const std::vector<Substance>& subs) {
  Chemistry c;
  c.chanceDen = kReactChanceDen;
  // Indoors, by lamplight: a day-gated rule sees a dim day; a sky- or
  // rain-gated one never fires (benchchem.h ChemGateOpen).
  c.daylight = 128;
  std::vector<int> slot(mats.size(), -1);
  for (size_t s = 0; s < subs.size(); s++)
    if (subs[s].mat < slot.size()) slot[subs[s].mat] = (int)s;
  auto prod = [&](uint32_t p, bool& ok) {
    if (p == kProdKeep) return kChemKeep;
    if (p == 0) return kChemAir;
    if (p < slot.size() && slot[p] >= 0) return slot[p];
    ok = false;
    return kChemKeep;
  };
  c.rules.resize(subs.size());
  for (size_t s = 0; s < subs.size(); s++) {
    const MaterialDef& m = mats[subs[s].mat];
    for (uint32_t k = 0; k < m.gpu.reactCount && m.gpu.reactOffset + k < reactions.size(); k++) {
      const ReactionGpu& g = reactions[m.gpu.reactOffset + k];
      ChemRule r;
      // A CONCENTRATION-CONDITIONED rule (reactions.json "solute"/"cMin"/
      // "cMax": brine electrolysis). Resolved BY NAME against the species
      // table the world uploads (Simulation::UploadSolutes does the same),
      // and evaluated by FlaskSim::TryRules against the self particle's own
      // dissolved mass (psol_/pmass_), in the world's concentration units
      // (benchchem.h ChemConcentration). A name the table lacks can never be
      // met -- never fired unconditionally, which would electrolyse FRESH
      // water.
      if (RuleNeedsSolute(m, k)) {
        const RuleFx& f = m.ruleFx[k];
        r.soluteSpecies = kChemSoluteNever;
        for (const SoluteDef& d : solutes)
          if (d.name == f.solute && d.species > 0 && d.species < kChemSoluteNever)
            r.soluteSpecies = (uint8_t)d.species;
        r.soluteMin = (uint8_t)std::min<uint32_t>(f.soluteMin, 255u);
        r.soluteMax = (uint8_t)std::min<uint32_t>(f.soluteMax, 255u);
      }
      r.kind = (uint8_t)(g.packed & 3u);
      r.dirs = (uint8_t)((g.packed >> 2) & 7u);
      r.nbrMat = g.nbrMat;
      r.nbrTags = g.nbrTags;
      r.nbrClass = g.nbrClass;
      r.chance = g.chance;
      r.cond = g.cond;
      r.worldIndex = m.gpu.reactOffset + k;
      bool ok = true;
      r.prodSelf = prod(g.prodSelf, ok);
      r.prodNbr = prod(g.prodNbr, ok);
      if (!ok) continue;
      if (k < m.ruleFx.size() && !m.ruleFx[k].effects.empty()) {
        // One effect a rule on the bench (the first); the registry keys on
        // its kind.
        const ReactionEffect& e = m.ruleFx[k].effects.front();
        ChemEffect fx;
        fx.kind = e.kind;
        fx.radius = e.radius;
        fx.power = e.power;
        fx.amount = e.amount;
        fx.what = e.what;
        r.fx = (int)c.effects.size();
        c.effects.push_back(fx);
      }
      c.rules[s].push_back(r);
    }
  }
  if (const uint32_t hot = TagBitByName(mats, "hot")) {
    c.heat.on = true;
    // "tag:hot" plus every SYNTHETIC bit a `neighborChance` rule made of it
    // (materials.cpp ExpandNeighborChance: "hot-except-fire" is carried by
    // every hot material but fire, and the ignition rules match on it) -- a
    // bit only hot materials carry. The glass is hot, and not fire.
    uint32_t tags = hot;
    for (int b = 0; b < 32; b++) {
      const uint32_t bit = 1u << b;
      bool any = false, onlyHot = true;
      for (size_t i = 1; i < mats.size() && onlyHot; i++)
        if (mats[i].gpu.tagMask & bit) {
          any = true;
          onlyHot = (mats[i].gpu.tagMask & hot) != 0;
        }
      if (any && onlyHot) tags |= bit;
    }
    c.heat.tags = tags;
    c.heat.klass = CLASS_GAS;
  }
  if (const uint32_t el = TagBitByName(mats, "electric")) {
    for (size_t i = 1; i < mats.size(); i++)
      if ((mats[i].gpu.tagMask & el) && mats[i].gpu.klass == CLASS_GAS) {
        c.spark.on = true;
        c.spark.mat = (uint32_t)i;
        c.spark.tags = mats[i].gpu.tagMask;
        c.spark.klass = (uint8_t)mats[i].gpu.klass;
        break;
      }
    if (!c.spark.on) {
      c.spark.on = true;
      c.spark.tags = el;
    }
  }
  for (const SoluteDef& d : solutes) {
    ChemSolute s;
    s.species = (uint8_t)d.species;
    s.from = d.from < slot.size() ? slot[d.from] : -1;
    if (s.from < 0) continue;
    s.precipitate = d.precipitatesTo && d.precipitatesTo < slot.size() ? slot[d.precipitatesTo] : -1;
    s.yieldPerVoxel = std::max<uint32_t>(1, d.yieldPerVoxel);
    s.saturation = d.saturation;
    s.dissolveChance = d.dissolveChance;
    s.diffusivity = d.diffusivity;
    s.tint = d.tint;
    s.tintStrength = d.tintStrength;
    s.glow = d.glow;
    for (uint16_t v : d.solvents)
      if (v < slot.size() && slot[v] >= 0) s.solvents.push_back(slot[v]);
    for (const SoluteConvert& cv : d.converts) {
      ChemSolute::Convert x;
      x.solvent = cv.solvent < slot.size() ? slot[cv.solvent] : -1;
      x.into = cv.into < slot.size() ? slot[cv.into] : -1;
      x.cMin = cv.cMin;
      if (x.solvent >= 0 && x.into >= 0) s.converts.push_back(x);
    }
    // The species index must stay the table's (a particle stores it).
    while (c.solutes.size() + 1 < s.species) c.solutes.push_back(ChemSolute{});
    c.solutes.push_back(s);
  }
  return c;
}

}  // namespace alchemy
