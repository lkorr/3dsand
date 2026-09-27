#pragma once
// The one conversion from the 3D sim's material table to the alchemy bench's
// substances (flasksim.h). Everything the bench knows about a material comes
// through here, so "a liquid behaves in the flask the way it behaves in the
// world" has one place to be true: class, density (the ORDER of who sinks),
// moveEvery (thickness) and the three palette colours.
#include <cstdint>
#include <vector>

#include "game/flasksim.h"
#include "sim/materials.h"

namespace alchemy {

inline bool BenchHolds(const MaterialDef& m) {
  return m.gpu.klass == CLASS_LIQUID || m.gpu.klass == CLASS_POWDER;
}

inline Substance SubstanceFromMaterial(const MaterialDef& m, uint16_t id) {
  Substance s;
  s.mat = id;
  s.powder = m.gpu.klass == CLASS_POWDER;
  s.density = m.gpu.density;
  s.moveEvery = m.gpu.moveEvery ? m.gpu.moveEvery : 1;
  s.color[0] = m.gpu.color0 | 0xFF000000u;
  s.color[1] = m.gpu.color1 | 0xFF000000u;
  s.color[2] = m.gpu.color2 | 0xFF000000u;
  return s;
}

// Substances for every material named in `comps`, deduplicated, in first-seen
// order. A material the bench cannot hold (a solid or a gas that somehow got
// into a vessel) is skipped; FlaskSim then never seeds it and its amount is
// reported back untouched by the caller.
inline std::vector<Substance> SubstancesFor(const std::vector<MaterialDef>& mats,
                                            const std::vector<const Composition*>& comps) {
  std::vector<Substance> out;
  for (const Composition* c : comps) {
    if (!c) continue;
    for (int i = 0; i < c->n; i++) {
      uint16_t id = c->p[i].mat;
      if (id >= mats.size() || !BenchHolds(mats[id])) continue;
      bool seen = false;
      for (const Substance& s : out) seen |= s.mat == id;
      if (!seen) out.push_back(SubstanceFromMaterial(mats[id], id));
    }
  }
  return out;
}

}  // namespace alchemy
