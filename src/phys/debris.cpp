#include "phys/debris.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "phys/marching_cubes.h"

namespace {

constexpr int kMaxRegionCells = 80;      // <= 5 chunks per axis (bounded fill)
constexpr uint32_t kMaxIslandVoxels = 32000;  // DESIGN.md §7 abort threshold
constexpr uint32_t kTerrainEvictTicks = 300;
constexpr uint32_t kTerrainRefreshTicks = 8;

uint32_t ChunkIndexOfCell(int x, int y, int z) {
  return (uint32_t)(((z / (int)kChunk) * (int)kNChunk + (y / (int)kChunk)) *
                        (int)kNChunk +
                    (x / (int)kChunk));
}
uint32_t CellIndexOf(int x, int y, int z) {
  uint32_t lx = (uint32_t)x % kChunk, ly = (uint32_t)y % kChunk,
           lz = (uint32_t)z % kChunk;
  return ChunkIndexOfCell(x, y, z) * kChunkVol + (lz * kChunk + ly) * kChunk + lx;
}

int FindMaterialId(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

}  // namespace

void DebrisSystem::Init(Physics* phys, const std::vector<MaterialDef>& mats) {
  phys_ = phys;
  OnMaterialsReloaded(mats);
}

void DebrisSystem::OnMaterialsReloaded(const std::vector<MaterialDef>& mats) {
  classOf_.clear();
  densityOf_.clear();
  rubbleOf_.clear();
  int gravel = FindMaterialId(mats, "gravel");
  int dust = FindMaterialId(mats, "dust");
  for (const auto& m : mats) {
    classOf_.push_back(m.gpu.klass);
    densityOf_.push_back((float)m.gpu.density);
    int r = m.rubble.empty() ? -1 : FindMaterialId(mats, m.rubble);
    if (r < 0) {
      bool organic = false;
      for (const auto& t : m.tags)
        if (t == "organic" || t == "flammable") organic = true;
      r = organic && dust > 0 ? dust : (gravel > 0 ? gravel : 0);
    }
    rubbleOf_.push_back((uint32_t)r);
  }
}

void DebrisSystem::Reset() {
  for (Body& b : bodies_) phys_->RemoveBody(b.handle);
  bodies_.clear();
  for (auto& [ci, t] : terrain_)
    if (t.handle) phys_->RemoveBody(t.handle);
  terrain_.clear();
  events_.clear();
  instancesDirty_ = true;
  instanceCount_ = 0;
}

void DebrisSystem::AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi) {
  Event e;
  e.tick = tick;
  // expand for support context, clamp to the bounded region + world
  constexpr int kMargin = 10;
  e.lo = {lo.x - kMargin, lo.y - kMargin, lo.z - kMargin};
  e.hi = {hi.x + kMargin, hi.y + kMargin, hi.z + kMargin};
  IVec3 c{(e.lo.x + e.hi.x) / 2, (e.lo.y + e.hi.y) / 2, (e.lo.z + e.hi.z) / 2};
  auto clampAxis = [&](int& lo, int& hi, int center) {
    if (hi - lo + 1 > kMaxRegionCells) {
      lo = center - kMaxRegionCells / 2;
      hi = lo + kMaxRegionCells - 1;
    }
  };
  clampAxis(e.lo.x, e.hi.x, c.x);
  clampAxis(e.lo.y, e.hi.y, c.y);
  clampAxis(e.lo.z, e.hi.z, c.z);
  e.lo.x = std::max(e.lo.x, 0); e.lo.y = std::max(e.lo.y, 0); e.lo.z = std::max(e.lo.z, 0);
  int n = (int)kWorldN - 1;
  e.hi.x = std::min(e.hi.x, n); e.hi.y = std::min(e.hi.y, n); e.hi.z = std::min(e.hi.z, n);
  if (e.lo.x > e.hi.x || e.lo.y > e.hi.y || e.lo.z > e.hi.z) return;
  if (events_.size() < 64) events_.push_back(e);
}

bool DebrisSystem::EventReady(const Event& e, World& world, uint32_t required) const {
  bool ready = true;
  for (int cz = e.lo.z / (int)kChunk; cz <= e.hi.z / (int)kChunk; cz++)
    for (int cy = e.lo.y / (int)kChunk; cy <= e.hi.y / (int)kChunk; cy++)
      for (int cx = e.lo.x / (int)kChunk; cx <= e.hi.x / (int)kChunk; cx++) {
        uint32_t ci = (uint32_t)((cz * (int)kNChunk + cy) * (int)kNChunk + cx);
        const CachedChunk* cc = world.Cached(ci);
        if (!cc || cc->version < required) {
          world.RequestChunkFetch(ci);
          ready = false;
        }
      }
  return ready;
}

void DebrisSystem::RunIslandDetection(const Event& e, uint32_t tick, World& world,
                                      std::vector<CellOp>& cellOps) {
  const int dx = e.hi.x - e.lo.x + 1;
  const int dy = e.hi.y - e.lo.y + 1;
  const int dz = e.hi.z - e.lo.z + 1;
  const size_t vol = (size_t)dx * dy * dz;
  auto lidx = [&](int x, int y, int z) {
    return (size_t)((z * dy + y) * dx + x);
  };

  // solid mask from the chunk cache (solids only: powders/liquids fall on
  // their own in the CA)
  std::vector<uint32_t> words(vol, 0);
  std::vector<uint8_t> solid(vol, 0);
  for (int z = 0; z < dz; z++)
    for (int y = 0; y < dy; y++)
      for (int x = 0; x < dx; x++) {
        int wx = e.lo.x + x, wy = e.lo.y + y, wz = e.lo.z + z;
        const CachedChunk* cc = world.Cached(ChunkIndexOfCell(wx, wy, wz));
        if (!cc || cc->voxels.size() != kChunkVol) continue;
        uint32_t lx = (uint32_t)wx % kChunk, ly = (uint32_t)wy % kChunk,
                 lz = (uint32_t)wz % kChunk;
        uint32_t w = cc->voxels[(lz * kChunk + ly) * kChunk + lx];
        uint32_t mat = w & 0xFFF;
        words[lidx(x, y, z)] = w;
        solid[lidx(x, y, z)] =
            mat != 0 && mat < classOf_.size() && classOf_[mat] == CLASS_SOLID;
      }

  // 6-connected components; a component touching the region boundary is
  // anchored to the world (or too big to judge) and stays put
  std::vector<int32_t> label(vol, -1);
  std::vector<size_t> stack;
  int32_t next = 0;
  struct Comp {
    std::vector<size_t> cells;
    bool anchored = false;
  };
  std::vector<Comp> comps;
  for (size_t seed = 0; seed < vol; seed++) {
    if (!solid[seed] || label[seed] != -1) continue;
    Comp comp;
    stack.assign(1, seed);
    label[seed] = next;
    while (!stack.empty()) {
      size_t i = stack.back();
      stack.pop_back();
      comp.cells.push_back(i);
      if (comp.cells.size() > kMaxIslandVoxels) comp.anchored = true;  // abort: too big
      int x = (int)(i % dx), y = (int)((i / dx) % dy), z = (int)(i / ((size_t)dx * dy));
      if (x == 0 || y == 0 || z == 0 || x == dx - 1 || y == dy - 1 || z == dz - 1)
        comp.anchored = true;
      const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                            {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (auto& d : nb) {
        int nx = x + d[0], ny = y + d[1], nz = z + d[2];
        if (nx < 0 || ny < 0 || nz < 0 || nx >= dx || ny >= dy || nz >= dz) continue;
        size_t ni = lidx(nx, ny, nz);
        if (solid[ni] && label[ni] == -1) {
          label[ni] = next;
          stack.push_back(ni);
        }
      }
    }
    comps.push_back(std::move(comp));
    next++;
  }

  for (const Comp& comp : comps) {
    if (comp.anchored) continue;
    if (cellOps.size() + comp.cells.size() > kMaxCellOpsPerTick) break;  // next tick

    if (comp.cells.size() < 8) {
      // rubble handoff: crumble to the powder form, stay in the CA
      for (size_t i : comp.cells) {
        int x = (int)(i % dx), y = (int)((i / dx) % dy), z = (int)(i / ((size_t)dx * dy));
        int wx = e.lo.x + x, wy = e.lo.y + y, wz = e.lo.z + z;
        uint32_t mat = words[i] & 0xFFF;
        uint32_t rub = mat < rubbleOf_.size() ? rubbleOf_[mat] : 0;
        uint32_t cellIdx = CellIndexOf(wx, wy, wz);
        uint32_t word = (rub & 0xFFF) | (((cellIdx * 2654435761u) >> 8 & 3u) << 12) |
                        (0xFFu << 16);
        cellOps.push_back({cellIdx, word});
      }
      lastCellWriteTick_ = tick;
      continue;
    }

    if (bodies_.size() >= kMaxBodies) continue;  // body budget spent: leave it

    // island -> rigidbody: min-corner local frame, voxels leave the grid
    IVec3 mn{dx, dy, dz};
    IVec3 mx{0, 0, 0};
    for (size_t i : comp.cells) {
      int x = (int)(i % dx), y = (int)((i / dx) % dy), z = (int)(i / ((size_t)dx * dy));
      mn.x = std::min(mn.x, x); mn.y = std::min(mn.y, y); mn.z = std::min(mn.z, z);
      mx.x = std::max(mx.x, x); mx.y = std::max(mx.y, y); mx.z = std::max(mx.z, z);
    }
    if (mx.x - mn.x > 120 || mx.y - mn.y > 120 || mx.z - mn.z > 120) continue;

    Body body;
    body.voxels.reserve(comp.cells.size());
    for (size_t i : comp.cells) {
      int x = (int)(i % dx), y = (int)((i / dx) % dy), z = (int)(i / ((size_t)dx * dy));
      DebrisVoxel v;
      v.x = (int8_t)(x - mn.x);
      v.y = (int8_t)(y - mn.y);
      v.z = (int8_t)(z - mn.z);
      v.payload = (uint16_t)(words[i] & 0xFFFF);
      body.voxels.push_back(v);
      cellOps.push_back({CellIndexOf(e.lo.x + x, e.lo.y + y, e.lo.z + z), 0u});
    }
    lastCellWriteTick_ = tick;

    IVec3 origin{e.lo.x + mn.x, e.lo.y + mn.y, e.lo.z + mn.z};
    body.handle = phys_->CreateDebrisBody(body.voxels, origin, densityOf_);
    if (body.handle == 0) continue;
    body.xf.pos = Vec3{(float)origin.x, (float)origin.y, (float)origin.z};
    body.xf.quat[0] = body.xf.quat[1] = body.xf.quat[2] = 0;
    body.xf.quat[3] = 1;
    float ex = (float)(mx.x - mn.x + 1), ey = (float)(mx.y - mn.y + 1),
          ez = (float)(mx.z - mn.z + 1);
    body.radiusVoxels = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez) + 2.0f;
    bodies_.push_back(std::move(body));
    instancesDirty_ = true;
    std::printf("debris: island of %zu voxels -> body (total %zu)\n",
                comp.cells.size(), bodies_.size());
  }
}

void DebrisSystem::PreTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps) {
  // process at most one ready event per tick (bounded CPU)
  if (!events_.empty()) {
    Event e = events_.front();
    uint32_t required = std::max(e.tick, lastCellWriteTick_);
    if (EventReady(e, world, required)) {
      events_.pop_front();
      RunIslandDetection(e, tick, world, cellOps);
      // terrain under the blast changed: sleeping debris nearby must re-check
      Vec3 c{(float)(e.lo.x + e.hi.x) * 0.5f, (float)(e.lo.y + e.hi.y) * 0.5f,
             (float)(e.lo.z + e.hi.z) * 0.5f};
      phys_->WakeNear(c, (float)kMaxRegionCells);
    } else if (events_.size() > 1 && tick > events_.front().tick + 120) {
      // stuck event (readback starvation): drop rather than stall the queue
      events_.pop_front();
    }
  }
  ManageTerrain(tick, world);
}

void DebrisSystem::ManageTerrain(uint32_t tick, World& world) {
  const WorldSnapshot& snap = world.Snap();

  // which chunks need collision right now? (around every dynamic body)
  std::vector<uint32_t> needed;
  for (const Body& b : bodies_) {
    float r = b.radiusVoxels + 6.0f;
    int lo[3] = {(int)((b.xf.pos.x - r) / kChunk), (int)((b.xf.pos.y - r) / kChunk),
                 (int)((b.xf.pos.z - r) / kChunk)};
    int hi[3] = {(int)((b.xf.pos.x + r) / kChunk), (int)((b.xf.pos.y + r) / kChunk),
                 (int)((b.xf.pos.z + r) / kChunk)};
    for (int a = 0; a < 3; a++) {
      lo[a] = std::clamp(lo[a], 0, (int)kNChunk - 1);
      hi[a] = std::clamp(hi[a], 0, (int)kNChunk - 1);
    }
    for (int cz = lo[2]; cz <= hi[2]; cz++)
      for (int cy = lo[1]; cy <= hi[1]; cy++)
        for (int cx = lo[0]; cx <= hi[0]; cx++)
          needed.push_back((uint32_t)((cz * (int)kNChunk + cy) * (int)kNChunk + cx));
  }
  std::sort(needed.begin(), needed.end());
  needed.erase(std::unique(needed.begin(), needed.end()), needed.end());

  for (uint32_t ci : needed) {
    TerrainEntry& t = terrain_[ci];
    t.lastNeeded = tick;
    const CachedChunk* cc = world.Cached(ci);
    if (!cc) {
      world.RequestChunkFetch(ci);
      continue;
    }
    // refresh when the sim says the chunk changed (rate-limited)
    if (ci < snap.dirtyFlags.size() && snap.dirtyFlags[ci] &&
        tick > t.lastRefreshReq + kTerrainRefreshTicks) {
      t.lastRefreshReq = tick;
      world.RequestChunkFetch(ci);
    }
    if (t.builtVersion >= cc->version && t.handle != 0) continue;
    if (t.builtVersion >= cc->version && t.handle == 0 && t.builtVersion != 0)
      continue;  // built empty at this version

    // (re)build the marching-cubes patch. Solids AND powders carry weight;
    // liquids don't (debris sinks). Missing neighbor chunks sample as empty —
    // transient until their fetch lands.
    auto solidAt = [&](int x, int y, int z) -> bool {
      if (x < 0 || y < 0 || z < 0 || x >= (int)kWorldN || y >= (int)kWorldN ||
          z >= (int)kWorldN)
        return true;  // world edge is solid (matches sim rule)
      const CachedChunk* n = world.Cached(ChunkIndexOfCell(x, y, z));
      if (!n || n->voxels.size() != kChunkVol) return false;
      uint32_t lx = (uint32_t)x % kChunk, ly = (uint32_t)y % kChunk,
               lz = (uint32_t)z % kChunk;
      uint32_t mat = n->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
      if (mat == 0 || mat >= classOf_.size()) return false;
      return classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER;
    };
    IVec3 origin{(int)(ci % kNChunk) * (int)kChunk,
                 (int)((ci / kNChunk) % kNChunk) * (int)kChunk,
                 (int)(ci / (kNChunk * kNChunk)) * (int)kChunk};
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    PolygonizeChunk(origin, solidAt, verts, indices);

    if (t.handle) phys_->RemoveBody(t.handle);
    t.handle = indices.empty() ? 0 : phys_->CreateTerrainMesh(verts, indices);
    t.builtVersion = cc->version;
    // ground under sleeping debris may have moved: let them re-settle
    phys_->WakeNear(Vec3{(float)origin.x + 8, (float)origin.y + 8,
                         (float)origin.z + 8},
                    24.0f);
  }

  // evict patches nothing has needed for a while
  for (auto it = terrain_.begin(); it != terrain_.end();) {
    if (it->second.lastNeeded + kTerrainEvictTicks < tick) {
      if (it->second.handle) phys_->RemoveBody(it->second.handle);
      it = terrain_.erase(it);
    } else {
      ++it;
    }
  }
}

void DebrisSystem::PostStep() {
  for (size_t i = 0; i < bodies_.size();) {
    Body& b = bodies_[i];
    phys_->GetTransform(b.handle, b.xf);
    bool dead = b.xf.pos.y < -64.0f || b.xf.pos.y > (float)kWorldN + 256.0f;
    if (dead) {
      phys_->RemoveBody(b.handle);
      bodies_[i] = std::move(bodies_.back());
      bodies_.pop_back();
      instancesDirty_ = true;
    } else {
      i++;
    }
  }
  // body budget: oldest bodies despawn first (they are usually settled rubble)
  while (bodies_.size() > kMaxBodies) {
    phys_->RemoveBody(bodies_.front().handle);
    bodies_.erase(bodies_.begin());
    instancesDirty_ = true;
  }
}

void DebrisSystem::BuildInstances(std::vector<BodyVoxInst>& out) {
  out.clear();
  for (size_t bi = 0; bi < bodies_.size() && bi < kMaxBodies; bi++) {
    for (const DebrisVoxel& v : bodies_[bi].voxels) {
      if (out.size() >= kMaxBodyVoxInstances) break;
      out.push_back({(float)v.x, (float)v.y, (float)v.z,
                     (uint32_t)v.payload | ((uint32_t)bi << 16)});
    }
  }
  instanceCount_ = (uint32_t)out.size();
  instancesDirty_ = false;
}

void DebrisSystem::BuildXforms(std::vector<BodyXformGpu>& out) const {
  out.clear();
  for (size_t i = 0; i < bodies_.size() && i < kMaxBodies; i++) {
    const Body& b = bodies_[i];
    BodyXformGpu x{};
    x.pos[0] = b.xf.pos.x;
    x.pos[1] = b.xf.pos.y;
    x.pos[2] = b.xf.pos.z;
    std::memcpy(x.quat, b.xf.quat, sizeof(x.quat));
    out.push_back(x);
  }
}

uint32_t DebrisSystem::ActiveBodyCount() const {
  uint32_t n = 0;
  for (const Body& b : bodies_)
    if (phys_->IsActive(b.handle)) n++;
  return n;
}
