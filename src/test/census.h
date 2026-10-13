// census.h — "what is in this box", by material name, for a gate's FAIL line.
//
// CLAUDE.md rule 6: a bare count is not a measurement. When a fixture's
// subject is gone ("10/289 water", "sealed hut keeps 30"), the next question
// is always WHAT IS THERE INSTEAD, and this answers it in one line instead of
// one elimination run per hypothesis.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sim/materials.h"

namespace selftest {

// Count the materials (and liquid eighths) in [x0..x1] x [y0..y1] x [z0..z1]
// through `at` (a world/slot-cell word reader), most common first, at most
// `maxNames` names. Liquids also report their eighths, so a pond that went
// partial reads as "water 289 (1734/2312 e)" rather than as "water 289".
inline std::string MatCensus(const std::vector<MaterialDef>& mats,
                             const std::function<uint32_t(int, int, int)>& at, int x0, int y0,
                             int z0, int x1, int y1, int z1, size_t maxNames = 6) {
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> n;  // mat -> (cells, eighths)
  for (int z = z0; z <= z1; z++)
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        const uint32_t w = at(x, y, z);
        auto& e = n[w & 0xFFFu];
        e.first++;
        e.second += ((w >> 12) & 0xFu) + 1u;
      }
  std::vector<std::pair<uint32_t, uint32_t>> order;  // (cells, mat)
  for (const auto& [m, e] : n) order.push_back({e.first, m});
  std::sort(order.begin(), order.end(), [](auto a, auto b) { return a.first > b.first; });
  std::string out;
  for (size_t i = 0; i < order.size() && i < maxNames; i++) {
    const uint32_t m = order[i].second;
    const std::string name =
        m == 0 ? std::string("air") : m < mats.size() ? mats[m].name : "#" + std::to_string(m);
    char b[96];
    if (m != 0 && m < mats.size() && mats[m].gpu.klass == CLASS_LIQUID)
      std::snprintf(b, sizeof b, "%s%s %u (%u e)", out.empty() ? "" : ", ", name.c_str(),
                    order[i].first, n[m].second);
    else
      std::snprintf(b, sizeof b, "%s%s %u", out.empty() ? "" : ", ", name.c_str(), order[i].first);
    out += b;
  }
  if (order.size() > maxNames) out += ", ...";
  return out;
}

}  // namespace selftest
