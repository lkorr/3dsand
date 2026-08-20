#include "game/prefab.h"

#include <algorithm>
#include <unordered_set>

namespace {

// 90° CCW about +Y inside a box of pre-rotation size s: (x,z) -> (z, sx-1-x).
IVec3 RotY90(IVec3 p, IVec3 s) { return {p.z, p.y, s.x - 1 - p.x}; }

// deterministic palette-variant pick from world coords (op payload: any
// stable choice is fine — it's recorded in the op stream)
uint32_t VariantOf(IVec3 c) {
  uint32_t h = (uint32_t)c.x * 73856093u ^ (uint32_t)c.y * 19349663u ^
               (uint32_t)c.z * 83492791u;
  return (h >> 8) % 3u;
}

uint64_t PackCell(IVec3 c) {
  auto u = [](int v) { return (uint64_t)(uint32_t)(v + (1 << 20)) & 0x1FFFFF; };
  return u(c.x) | (u(c.y) << 21) | (u(c.z) << 42);
}

}  // namespace

IVec3 PrefabPlacer::RotatedSize(const Prefab& pf, int rotY) {
  return (rotY & 1) ? IVec3{pf.size.z, pf.size.y, pf.size.x} : pf.size;
}

void PrefabPlacer::Place(const Prefab& pf, IVec3 at, int rotY, bool overwrite,
                         const std::vector<MaterialDef>& mats, IVec3& boxLo,
                         IVec3& boxHi) {
  rotY &= 3;
  boxLo = at;
  IVec3 rs = RotatedSize(pf, rotY);
  boxHi = {at.x + rs.x - 1, at.y + rs.y - 1, at.z + rs.z - 1};

  // models may overlap after scene-graph flattening: first writer wins so one
  // cell never gets two ops in flight (GPU same-cell write order is undefined)
  std::unordered_set<uint64_t> claimed;
  claimed.reserve(256);

  for (const PrefabModel& m : pf.models) {
    for (const PrefabVoxel& v : m.voxels) {
      uint32_t mat = v.material;
      if (mat == 0 || mat >= mats.size()) continue;  // unmapped palette index
      IVec3 p{m.offset.x + v.x, m.offset.y + v.y, m.offset.z + v.z};
      IVec3 s = pf.size;
      for (int r = 0; r < rotY; r++) {
        p = RotY90(p, s);
        s = {s.z, s.y, s.x};
      }
      IVec3 cell{at.x + p.x, at.y + p.y, at.z + p.z};
      if (!claimed.insert(PackCell(cell)).second) continue;
      // same word rules as brush paints (sim_mutate.wgsl): liquids born full,
      // others get a palette variant; stamp 0xFF = "hasn't acted", falls now
      uint32_t state = mats[mat].gpu.klass == CLASS_LIQUID ? 7u : VariantOf(cell);
      uint32_t word = (mat & 0xFFFu) | (state << 12) | (0xFFu << 16);
      if (!overwrite) word |= kCellOpIfAir;
      pending_.push_back({cell, word});
    }
  }
}

void PrefabPlacer::PreTick(const World& world, std::vector<CellOp>& cellOps) {
  if (pending_.empty()) return;

  // slots already written this tick (island removal / rubble): those ops win,
  // colliding prefab voxels retry next tick
  std::unordered_set<uint32_t> taken;
  taken.reserve(cellOps.size());
  for (const CellOp& op : cellOps) taken.insert(op.cellIdx);

  size_t room = cellOps.size() >= kMaxCellOpsPerTick
                    ? 0
                    : kMaxCellOpsPerTick - cellOps.size();
  uint32_t budget = (uint32_t)std::min<size_t>(kPlacePerTick, room);
  std::deque<PendingCell> retry;
  while (budget > 0 && !pending_.empty()) {
    PendingCell pc = pending_.front();
    pending_.pop_front();
    if (!world.CellInWindow(pc.cell)) continue;  // streamed out: drop
    uint32_t idx = World::SlotCellIndex(pc.cell);
    if (taken.count(idx)) {
      retry.push_back(pc);
      continue;
    }
    cellOps.push_back({idx, pc.word});
    budget--;
  }
  for (const PendingCell& pc : retry) pending_.push_front(pc);
}
