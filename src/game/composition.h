#pragma once
// What a vessel holds: up to kMaxSubstances (material id, amount) portions.
//
// Amounts are in EIGHTHS OF A CELL, the unit every vessel path already speaks
// (item.h kContainerUnitsPerCell). A portion's order carries no meaning — the
// layering a player sees is re-derived from density whenever the alchemy
// panel opens (flasksim.h), so two compositions with the same portions are the
// same contents. Mixing never makes matter: every change to a Composition in
// gameplay is a transfer between two of them (or to the world), and the
// alchemy panel's tally is exact by construction (FlaskSim::Tally).
#include <cstdint>

namespace alchemy {

constexpr int kMaxSubstances = 16;

struct Portion {
  uint16_t mat = 0;
  uint32_t eighths = 0;
};

struct Composition {
  uint8_t n = 0;
  Portion p[kMaxSubstances]{};

  uint32_t Total() const {
    uint32_t t = 0;
    for (int i = 0; i < n; i++) t += p[i].eighths;
    return t;
  }
  bool Empty() const { return n == 0; }
  uint32_t AmountOf(uint16_t mat) const {
    for (int i = 0; i < n; i++)
      if (p[i].mat == mat) return p[i].eighths;
    return 0;
  }
  // Adds to an existing portion or opens a new one. False (and no change) if
  // this would be a 17th substance; the caller decides what that means.
  bool Add(uint16_t mat, uint32_t eighths) {
    if (eighths == 0) return true;
    for (int i = 0; i < n; i++)
      if (p[i].mat == mat) { p[i].eighths += eighths; return true; }
    if (n >= kMaxSubstances) return false;
    p[n].mat = mat;
    p[n].eighths = eighths;
    n++;
    return true;
  }
  // Removes up to `eighths` of `mat`; returns what was actually removed.
  // A portion that reaches zero is closed so `n` counts real substances.
  uint32_t Take(uint16_t mat, uint32_t eighths) {
    for (int i = 0; i < n; i++) {
      if (p[i].mat != mat) continue;
      uint32_t got = eighths < p[i].eighths ? eighths : p[i].eighths;
      p[i].eighths -= got;
      if (p[i].eighths == 0) {
        for (int j = i; j + 1 < n; j++) p[j] = p[j + 1];
        n--;
        p[n] = Portion{};
      }
      return got;
    }
    return 0;
  }
};

}  // namespace alchemy
