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
// Support-loss events: a chunk re-flags constantly while sand pours or fire
// burns, so rescans are rate-limited per chunk. The final flags after activity
// stops always land (pendingSupport_ is never dropped), so the cooldown only
// delays detection, never loses it.
constexpr uint32_t kSupportCooldownTicks = 45;
// Support events use a wider margin than blast events (24 -> 64^3 region):
// the flagged chunk holds the support POINT, but the structure that may float
// (a grown plant, a burnt-through pillar) extends well beyond it, and a
// component touching the region boundary is conservatively kept.
constexpr int kSupportMargin = 24;
constexpr int kSupportDrainPerTick = 2;

// Body burn budgets: voxel scans across all bodies per tick, grid writes
// (emitted fire / escaping ash+smoke) per tick, and how many voxels must burn
// away before the Jolt collider is rebuilt to match the charred shape.
constexpr uint32_t kBurnScanPerTick = 4096;
constexpr uint32_t kBurnOpsPerTick = 384;
constexpr uint32_t kBurnRebuildVoxels = 12;

// CPU mirror of common.wgsl pcg/hash3 — counter-based, stateless, so burn
// rolls replay identically for a given (body serial, tick, voxel, rule).
uint32_t Pcg(uint32_t v) {
  uint32_t s = v * 747796405u + 2891336453u;
  uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}
uint32_t Hash3(uint32_t a, uint32_t b, uint32_t c) {
  return Pcg(a ^ Pcg(b ^ Pcg(c)));
}

uint32_t LocalKey(int x, int y, int z) {
  return (uint32_t)(x & 0xFF) | ((uint32_t)(y & 0xFF) << 8) |
         ((uint32_t)(z & 0xFF) << 16);
}

Vec3 QuatRot(const float q[4], Vec3 v) {
  Vec3 u{q[0], q[1], q[2]};
  Vec3 t = u.cross(v) * 2.0f;
  return v + t * q[3] + u.cross(t);
}

// world CHUNK coord of a world cell (floor shift: valid for negatives)
IVec3 ChunkOfCell(int x, int y, int z) { return {x >> 4, y >> 4, z >> 4}; }
// slot linear cell index (what cellOps / sim_mutate `cells` consume)
uint32_t CellIndexOf(int x, int y, int z) {
  return World::SlotCellIndex({x, y, z});
}

int FindMaterialId(const std::vector<MaterialDef>& mats, const std::string& name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (int)i;
  return -1;
}

}  // namespace

void DebrisSystem::Init(Physics* phys, World* world, const std::vector<MaterialDef>& mats,
                        const std::vector<ReactionGpu>& reactions) {
  phys_ = phys;
  world_ = world;
  OnMaterialsReloaded(mats, reactions);
}

void DebrisSystem::OnMaterialsReloaded(const std::vector<MaterialDef>& mats,
                                       const std::vector<ReactionGpu>& reactions) {
  classOf_.clear();
  densityOf_.clear();
  rubbleOf_.clear();
  matGpu_.clear();
  matSelfActive_.clear();
  matHasPair_.clear();
  reactions_ = reactions;
  int gravel = FindMaterialId(mats, "gravel");
  int dust = FindMaterialId(mats, "dust");
  for (const auto& m : mats) {
    classOf_.push_back(m.gpu.klass);
    densityOf_.push_back((float)m.gpu.density);
    matGpu_.push_back(m.gpu);
    // which rule shapes this material owns (drives the burn-pass gates)
    uint8_t selfActive = 0, hasPair = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++) {
      uint32_t kind = reactions_[m.gpu.reactOffset + ri].packed & 3u;
      if (kind == kReactDecay || kind == kReactEmit) selfActive = 1;
      if (kind == kReactPair) hasPair = 1;
    }
    matSelfActive_.push_back(selfActive);
    matHasPair_.push_back(hasPair);
    int r = m.rubble.empty() ? -1 : FindMaterialId(mats, m.rubble);
    if (r < 0) {
      bool organic = false;
      for (const auto& t : m.tags)
        if (t == "organic" || t == "flammable") organic = true;
      r = organic && dust > 0 ? dust : (gravel > 0 ? gravel : 0);
    }
    rubbleOf_.push_back((uint32_t)r);
  }
  for (Body& b : bodies_) RecountBurn(b);  // hot-reload can change rule sets
}

void DebrisSystem::RecountBurn(Body& b) const {
  b.activeCount = 0;
  b.pairCount = 0;
  for (const DebrisVoxel& v : b.voxels) {
    uint32_t m = v.payload & 0xFFFu;
    if (m >= matGpu_.size()) continue;
    if (matSelfActive_[m]) b.activeCount++;
    if (matHasPair_[m]) b.pairCount++;
  }
}

void DebrisSystem::Reset() {
  for (Body& b : bodies_) phys_->RemoveBody(b.handle);
  bodies_.clear();
  for (auto& [ci, t] : terrain_)
    if (t.handle) phys_->RemoveBody(t.handle);
  terrain_.clear();
  events_.clear();
  pendingSupport_.clear();
  supportPending_.clear();
  supportCooldown_.clear();
  instancesDirty_ = true;
  instanceCount_ = 0;
}

bool DebrisSystem::AddDestructionEvent(uint32_t tick, IVec3 lo, IVec3 hi, int margin) {
  Event e;
  e.tick = tick;
  // expand for support context, clamp to the bounded region + world
  e.lo = {lo.x - margin, lo.y - margin, lo.z - margin};
  e.hi = {hi.x + margin, hi.y + margin, hi.z + margin};
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
  // clamp to the residency window (world coords)
  IVec3 wlo = world_->WindowOrigin();
  wlo = {wlo.x * (int)kChunk, wlo.y * (int)kChunk, wlo.z * (int)kChunk};
  IVec3 whi{wlo.x + (int)kWorldN - 1, wlo.y + (int)kWorldN - 1, wlo.z + (int)kWorldN - 1};
  e.lo.x = std::max(e.lo.x, wlo.x); e.lo.y = std::max(e.lo.y, wlo.y); e.lo.z = std::max(e.lo.z, wlo.z);
  e.hi.x = std::min(e.hi.x, whi.x); e.hi.y = std::min(e.hi.y, whi.y); e.hi.z = std::min(e.hi.z, whi.z);
  if (e.lo.x > e.hi.x || e.lo.y > e.hi.y || e.lo.z > e.hi.z) return true;  // degenerate: done
  if (events_.size() >= 64) return false;
  events_.push_back(e);
  return true;
}

void DebrisSystem::QueueSupportEvents(const WorldSnapshot& snap) {
  if (!snap.valid || snap.tick == lastSupportSnapTick_) return;  // one pass per snapshot
  lastSupportSnapTick_ = snap.tick;
  int m = (int)kNChunk - 1;
  for (uint32_t ci = 0; ci < (uint32_t)snap.supportFlags.size(); ci++) {
    if (!snap.supportFlags[ci]) continue;
    // slot -> world chunk under the origin the snapshot was captured at
    IVec3 s{(int)(ci % kNChunk), (int)((ci / kNChunk) % kNChunk),
            (int)(ci / (kNChunk * kNChunk))};
    IVec3 o = snap.windowOrigin;
    IVec3 wc{o.x + ((s.x - o.x) & m), o.y + ((s.y - o.y) & m),
             o.z + ((s.z - o.z) & m)};
    uint64_t key = World::PackChunkKey(wc);
    if (supportPending_.count(key)) continue;
    auto it = supportCooldown_.find(key);
    if (it != supportCooldown_.end() && snap.tick < it->second + kSupportCooldownTicks)
      continue;
    supportCooldown_[key] = snap.tick;
    supportPending_[key] = 1;
    pendingSupport_.push_back(wc);
  }
}

bool DebrisSystem::EventReady(const Event& e, World& world, uint32_t required) const {
  bool ready = true;
  for (int cz = e.lo.z >> 4; cz <= (e.hi.z >> 4); cz++)
    for (int cy = e.lo.y >> 4; cy <= (e.hi.y >> 4); cy++)
      for (int cx = e.lo.x >> 4; cx <= (e.hi.x >> 4); cx++) {
        IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;  // streamed out: skip
        const CachedChunk* cc = world.Cached(wc);
        if (!cc || cc->version < required) {
          world.RequestChunkFetch(wc);
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
        const CachedChunk* cc = world.Cached(ChunkOfCell(wx, wy, wz));
        if (!cc || cc->voxels.size() != kChunkVol) continue;
        uint32_t lx = (uint32_t)(wx & 15), ly = (uint32_t)(wy & 15),
                 lz = (uint32_t)(wz & 15);
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
      // resting on powder = supported: without this, every slab on a sand
      // pile would convert to a body the moment a support-loss scan runs.
      // When the powder flows away the sim re-flags the chunk and the next
      // scan sees air below.
      if (y > 0) {
        uint32_t bmat = words[lidx(x, y - 1, z)] & 0xFFF;
        if (bmat != 0 && bmat < classOf_.size() && classOf_[bmat] == CLASS_POWDER)
          comp.anchored = true;
      }
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
    body.serial = nextSerial_++;
    RecountBurn(body);
    bodies_.push_back(std::move(body));
    instancesDirty_ = true;
    std::printf("debris: island of %zu voxels -> body (total %zu)\n",
                comp.cells.size(), bodies_.size());
  }
}

void DebrisSystem::PreTick(uint32_t tick, World& world, std::vector<CellOp>& cellOps,
                           std::vector<ParticleSpawn>& spawns) {
  // promote flagged support-loss chunks into events while there is queue room
  for (int i = 0; i < kSupportDrainPerTick && !pendingSupport_.empty(); i++) {
    IVec3 wc = pendingSupport_.front();
    IVec3 lo{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    IVec3 hi{lo.x + (int)kChunk - 1, lo.y + (int)kChunk - 1, lo.z + (int)kChunk - 1};
    if (world_->ChunkInWindow(wc)) {  // streamed out: forget it
      if (!AddDestructionEvent(tick, lo, hi, kSupportMargin)) break;  // full: retry
    }
    pendingSupport_.pop_front();
    supportPending_.erase(World::PackChunkKey(wc));
  }

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
    } else if ((events_.size() > 1 && tick > events_.front().tick + 120) ||
               tick > events_.front().tick + 300) {
      // stuck event (readback starvation, or its region streamed out): drop
      // rather than stall the queue
      events_.pop_front();
    }
  }
  BurnBodies(tick, world, cellOps, spawns);
  SettleBodies(tick, world, cellOps);
  ManageTerrain(tick, world);
}

void DebrisSystem::SettleBodies(uint32_t tick, World& world,
                                std::vector<CellOp>& cellOps) {
  constexpr uint32_t kSettleAfterTicks = 60;   // 2 s asleep before converting
  constexpr float kAlignCos = 0.94f;           // ~20°: snap or stay a body

  for (size_t bi = 0; bi < bodies_.size(); bi++) {
    Body& b = bodies_[bi];
    if (phys_->IsActive(b.handle)) {
      b.inactiveTicks = 0;
      continue;
    }
    if (++b.inactiveTicks < kSettleAfterTicks) continue;
    if (b.inactiveTicks % 30 != 0) continue;  // re-test alignment cheaply

    // rotation -> 3x3, then the nearest signed permutation. Reject when any
    // axis strays past the snap tolerance (resampling odd angles looks like
    // mush — PLAN §B6 explicitly leaves those as bodies).
    const float x = b.xf.quat[0], y = b.xf.quat[1], z = b.xf.quat[2],
                w = b.xf.quat[3];
    float m[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    int snap[3][3] = {};
    bool aligned = true;
    bool rowUsed[3] = {};
    for (int col = 0; col < 3 && aligned; col++) {
      int best = 0;
      for (int row = 1; row < 3; row++)
        if (std::abs(m[row][col]) > std::abs(m[best][col])) best = row;
      if (std::abs(m[best][col]) < kAlignCos || rowUsed[best]) {
        aligned = false;
        break;
      }
      rowUsed[best] = true;
      snap[best][col] = m[best][col] > 0 ? 1 : -1;
    }
    if (!aligned) continue;

    // whole body must land inside the residency window, and this tick's op
    // budget must hold every voxel — partial settles would lose matter
    if (cellOps.size() + b.voxels.size() > kMaxCellOpsPerTick) continue;

    // snapped basis is a lattice bijection: with the body origin rounded to a
    // cell corner, voxel centers land on distinct cells — no self-collisions.
    IVec3 base{(int)std::lround(b.xf.pos.x), (int)std::lround(b.xf.pos.y),
               (int)std::lround(b.xf.pos.z)};
    bool inWindow = true;
    size_t opsStart = cellOps.size();
    for (const DebrisVoxel& v : b.voxels) {
      float lx = (float)v.x + 0.5f, ly = (float)v.y + 0.5f, lz = (float)v.z + 0.5f;
      IVec3 cell{
          base.x + ifloor(snap[0][0] * lx + snap[0][1] * ly + snap[0][2] * lz),
          base.y + ifloor(snap[1][0] * lx + snap[1][1] * ly + snap[1][2] * lz),
          base.z + ifloor(snap[2][0] * lx + snap[2][1] * ly + snap[2][2] * lz)};
      if (!world.CellInWindow(cell)) {
        inWindow = false;
        break;
      }
      // fill-air-only: occupied cells win on the GPU (deterministic — grid
      // state is hashed, and the op replays identically). Minor volume loss
      // where the world grew into the footprint is accepted (§B6).
      uint32_t word = (uint32_t)v.payload | (0xFFu << 16) | kCellOpIfAir;
      cellOps.push_back({World::SlotCellIndex(cell), word});
    }
    if (!inWindow) {
      cellOps.resize(opsStart);
      continue;
    }

    lastCellWriteTick_ = tick;
    phys_->RemoveBody(b.handle);
    bodies_[bi] = std::move(bodies_.back());
    bodies_.pop_back();
    instancesDirty_ = true;
    settledBack_++;
    break;  // one body per tick: bounded CPU + op traffic
  }
}

// ---- body burn: fire continuity on rigidbodies ----
// A detached island is CPU state no CA pass touches, so without this a burning
// plank froze mid-flame the moment it became a body: its embers never advanced,
// never spread, never lit anything. This pass runs the SAME reaction table
// (per-material buckets, file order, first-fire-wins, per-mille chances) over
// body voxel payloads each tick:
//   - decay/emit rules advance in place: ember -> ash, ember emits fire. The
//     emitted fire and any non-solid product (ash, smoke) land in the GRID at
//     the voxel's world cell as fill-air-only CellOps — real fire voxels that
//     rise, spread, and ignite neighbors through the normal CA rules. Escaped
//     voxels leave the body, so burning debris visibly wastes away.
//   - pair rules match body-internal 6-neighbors (ember ignites the wood next
//     to it inside the body) and world cells sampled from the chunk cache
//     (already fetched for terrain meshing around every live body), so grid
//     fire licking a cold wooden body ignites it and water douses its embers.
// Grid writes ride the MutationQueue like settle-back; RNG is counter-based
// (serial, tick, voxel, rule). Idle cost is zero: bodies with no self-driven
// voxels skip unless the sim is actually moving in a chunk they overlap.
void DebrisSystem::BurnBodies(uint32_t tick, World& world,
                              std::vector<CellOp>& cellOps,
                              std::vector<ParticleSpawn>& spawns) {
  if (reactions_.empty() || bodies_.empty()) return;
  const WorldSnapshot& snap = world.Snap();
  uint32_t scanBudget = kBurnScanPerTick;
  uint32_t opsBudget = kBurnOpsPerTick;
  bool rebuiltOne = false;
  // fragment bodies split off by ShatterBody, appended after the loop (a
  // push_back into bodies_ mid-iteration would invalidate `b`)
  std::vector<Body> fragments;

  for (size_t bi = 0; bi < bodies_.size();) {
    Body& b = bodies_[bi];
    uint32_t n = (uint32_t)b.voxels.size();
    bool active = b.activeCount > 0;
    if (n == 0 || scanBudget == 0 || (!active && b.pairCount == 0) ||
        (!active && !AnyDirtyNear(b, snap, world))) {
      bi++;
      continue;
    }

    // rotation helpers (same quaternion sandwich as SplitBody)
    const float qx = b.xf.quat[0], qy = b.xf.quat[1], qz = b.xf.quat[2],
                qw = b.xf.quat[3];
    auto rotQ = [&](Vec3 v) {
      Vec3 u{qx, qy, qz};
      Vec3 t = u.cross(v) * 2.0f;
      return v + t * qw + u.cross(t);
    };
    auto rotInvQ = [&](Vec3 v) {
      Vec3 u{-qx, -qy, -qz};
      Vec3 t = u.cross(v) * 2.0f;
      return v + t * qw + u.cross(t);
    };
    auto worldCellOf = [&](const DebrisVoxel& v) {
      Vec3 wp = b.xf.pos +
                rotQ(Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f});
      return IVec3{ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)};
    };
    // world direction -> nearest body-local lattice offset (occlusion checks)
    auto localDirOf = [&](IVec3 d) {
      Vec3 l = rotInvQ(Vec3{(float)d.x, (float)d.y, (float)d.z});
      return IVec3{(int)std::lround(l.x), (int)std::lround(l.y),
                   (int)std::lround(l.z)};
    };
    // grid material at a world cell via the chunk cache (terrain meshing keeps
    // chunks around live bodies fetched + refreshed while they are dirty).
    // Unknown/missing reads as air: ignition is best-effort, never wrong-way.
    auto worldMatAt = [&](IVec3 c) -> uint32_t {
      if (!world.CellInWindow(c)) return 0u;
      const CachedChunk* cc = world.Cached(ChunkOfCell(c.x, c.y, c.z));
      if (!cc || cc->voxels.size() != kChunkVol) return 0u;
      uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
               lz = (uint32_t)(c.z & 15);
      return cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFFu;
    };
    auto nbrMatches = [&](uint32_t nm, const ReactionGpu& r) -> bool {
      if (nm == 0 || nm >= matGpu_.size()) return false;
      const MaterialGpu& g = matGpu_[nm];
      if (r.nbrClass != 0 && ((r.nbrClass >> g.klass) & 1u) == 0) return false;
      if (r.nbrMat != kNbrAny) return nm == r.nbrMat;
      if (r.nbrTags != 0) return (g.tagMask & r.nbrTags) != 0;
      return true;
    };

    // local occupancy for internal spread; values are voxel indices, entries
    // whose payload was zeroed this pass read as absent
    std::unordered_map<uint32_t, uint32_t> local;
    if (active) {
      local.reserve(n * 2);
      for (uint32_t i = 0; i < n; i++)
        local[LocalKey(b.voxels[i].x, b.voxels[i].y, b.voxels[i].z)] = i;
    }
    auto localMatAt = [&](int x, int y, int z) -> uint32_t {
      if (!active) return 0u;
      auto it = local.find(LocalKey(x, y, z));
      if (it == local.end()) return 0u;
      return b.voxels[it->second].payload & 0xFFFu;
    };

    uint32_t removed = 0;
    bool changed = false;
    // rewrite a voxel to a rule product. Solids swap in place; anything else
    // (ash, smoke, fire, air) escapes into the grid at the voxel's world cell
    // and the voxel leaves the body.
    auto applyProduct = [&](uint32_t vi, uint32_t prod, uint32_t rr) {
      if (prod == kProdKeep) return;
      DebrisVoxel& tv = b.voxels[vi];
      uint32_t pm = prod & 0xFFFu;
      if (pm != 0 && pm < matGpu_.size() && matGpu_[pm].klass == CLASS_SOLID) {
        tv.payload = (uint16_t)(pm | (((rr >> 6u) % 3u) << 12u));
      } else {
        if (pm != 0 && pm < matGpu_.size() && opsBudget > 0 &&
            cellOps.size() < kMaxCellOpsPerTick) {
          IVec3 cell = worldCellOf(tv);
          if (world.CellInWindow(cell)) {
            uint32_t state = matGpu_[pm].klass == CLASS_LIQUID
                                 ? 7u  // LIQ_FULL_STATE
                                 : (rr >> 6u) % 3u;
            cellOps.push_back({World::SlotCellIndex(cell),
                               pm | (state << 12u) | (0xFFu << 16u) | kCellOpIfAir});
            opsBudget--;
          }
        }
        tv.payload = 0;  // compacted below
        removed++;
      }
      changed = true;
    };

    const IVec3 kDirs[6] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0},
                            {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    uint32_t steps = std::min(n, scanBudget);
    scanBudget -= steps;
    for (uint32_t s = 0; s < steps; s++) {
      uint32_t vi = (b.burnCursor + s) % n;
      DebrisVoxel& v = b.voxels[vi];
      uint32_t m = v.payload & 0xFFFu;
      if (m == 0 || m >= matGpu_.size()) continue;
      const MaterialGpu& mg = matGpu_[m];
      if (mg.reactCount == 0) continue;
      if (!active && !matHasPair_[m]) continue;

      for (uint32_t ri = 0; ri < mg.reactCount; ri++) {
        const ReactionGpu& r = reactions_[mg.reactOffset + ri];
        uint32_t kind = r.packed & 3u;
        uint32_t dmask = (r.packed >> 2u) & 7u;
        uint32_t rr = Hash3(b.serial * 0x9E3779B9u + vi, tick, ri);
        if (rr % 1000u >= r.chance) continue;  // one roll per rule, GPU-style
        bool fired = false;

        if (kind == kReactDecay) {
          applyProduct(vi, r.prodSelf, rr);
          fired = true;
        } else if (kind == kReactEmit) {
          // emit in an allowed WORLD direction (fire rises in world space no
          // matter how the body tumbles), first direction not occluded by the
          // body itself; IfAir lets grid content win
          IVec3 cand[6];
          int nc = 0;
          if (dmask & kDirUp) cand[nc++] = {0, 1, 0};
          if (dmask & kDirDown) cand[nc++] = {0, -1, 0};
          if (dmask & kDirSide) {
            const IVec3 side[4] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
            uint32_t rot = rr >> 12u;
            for (int k = 0; k < 4; k++) cand[nc++] = side[(rot + k) & 3u];
          }
          IVec3 wc0 = worldCellOf(v);
          for (int k = 0; k < nc; k++) {
            IVec3 ld = localDirOf(cand[k]);
            if (localMatAt(v.x + ld.x, v.y + ld.y, v.z + ld.z) != 0) continue;
            IVec3 t{wc0.x + cand[k].x, wc0.y + cand[k].y, wc0.z + cand[k].z};
            if (world.CellInWindow(t) && opsBudget > 0 &&
                cellOps.size() < kMaxCellOpsPerTick) {
              cellOps.push_back({World::SlotCellIndex(t),
                                 (r.prodNbr & 0xFFFu) | (((rr >> 8u) % 3u) << 12u) |
                                     (0xFFu << 16u) | kCellOpIfAir});
              opsBudget--;
            }
            fired = true;  // rule consumed even if the write missed the budget
            break;
          }
        } else {  // kReactPair
          // body-internal neighbors first (ember ignites adjacent wood inside
          // the plank), then the voxel's own world cell + 6 world neighbors
          // (grid fire drifts into / around the body's footprint)
          int matched = -2;  // -2 none, -1 world, >=0 internal voxel index
          if (active) {
            for (const IVec3& d : kDirs) {
              uint32_t nm = localMatAt(v.x + d.x, v.y + d.y, v.z + d.z);
              if (nm != 0 && nbrMatches(nm, r)) {
                matched = (int)local[LocalKey(v.x + d.x, v.y + d.y, v.z + d.z)];
                break;
              }
            }
          }
          if (matched == -2 && r.prodNbr == kProdKeep) {
            // world neighbors are read-only: only rules that keep the
            // neighbor are eligible (a body cannot rewrite grid content)
            IVec3 wc = worldCellOf(v);
            if (nbrMatches(worldMatAt(wc), r)) {
              matched = -1;
            } else {
              for (const IVec3& d : kDirs) {
                if (nbrMatches(worldMatAt({wc.x + d.x, wc.y + d.y, wc.z + d.z}), r)) {
                  matched = -1;
                  break;
                }
              }
            }
          }
          if (matched != -2) {
            applyProduct(vi, r.prodSelf, rr);
            if (matched >= 0 && r.prodNbr != kProdKeep)
              applyProduct((uint32_t)matched, r.prodNbr, Pcg(rr));
            fired = true;
          }
        }
        if (fired) {
          changed = true;
          break;  // at most one rule per voxel per tick, file order
        }
      }
    }
    b.burnCursor = n > 0 ? (b.burnCursor + steps) % n : 0;

    if (removed) {
      b.voxels.erase(std::remove_if(b.voxels.begin(), b.voxels.end(),
                                    [](const DebrisVoxel& v) { return v.payload == 0; }),
                     b.voxels.end());
      b.burnedSinceRebuild += removed;
      if (!b.voxels.empty()) b.burnCursor %= (uint32_t)b.voxels.size();
      // removals can disconnect the remainder: split fragments off (bodies /
      // ballistic particles) before recounting
      ShatterBody(b, world, fragments, spawns);
    }
    if (changed) {
      RecountBurn(b);
      instancesDirty_ = true;
      // NOTE: burn ops deliberately do NOT bump lastCellWriteTick_. They are
      // additive fill-air-only writes a stale island scan can safely miss;
      // bumping it every burning tick would hold EventReady's required
      // version at the current tick forever and starve island detection.
    }

    // burned/broken below body-worthiness: the remainder re-enters the world
    // as ballistic voxels carrying the body's momentum (the moving-body
    // analogue of the <8-voxel island rubble handoff). Gated on THIS pass
    // having removed voxels — a small body that isn't burning (a 4-voxel
    // split half, a mob hand) is legitimate and must persist.
    if (removed > 0 && b.voxels.size() < 8) {
      Vec3 lin{}, ang{};
      phys_->GetBodyVelocities(b.handle, lin, ang);
      VoxelsToParticles(b, b.voxels, lin, ang, world, spawns);
      phys_->RemoveBody(b.handle);
      bodies_[bi] = std::move(bodies_.back());
      bodies_.pop_back();
      instancesDirty_ = true;
      continue;  // re-examine the swapped-in body at this index
    }

    // batched collider refresh: the charred shape sheds its burned voxels
    // (at most one Jolt rebuild per tick across all bodies)
    if (b.burnedSinceRebuild >= kBurnRebuildVoxels && !rebuiltOne) {
      Vec3 lin{}, ang{};
      phys_->GetBodyVelocities(b.handle, lin, ang);
      uint64_t nh = phys_->CreateDebrisBodyXf(b.voxels, b.xf, densityOf_);
      if (nh != 0) {
        phys_->RemoveBody(b.handle);
        b.handle = nh;
        phys_->SetBodyVelocities(nh, lin, ang);
        b.burnedSinceRebuild = 0;
        rebuiltOne = true;
      }
    }
    bi++;
  }

  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
}

void DebrisSystem::ShatterBody(Body& b, World& world, std::vector<Body>& fragments,
                               std::vector<ParticleSpawn>& spawns) {
  const uint32_t n = (uint32_t)b.voxels.size();
  if (n < 2) return;
  std::unordered_map<uint32_t, uint32_t> map;
  map.reserve(n * 2);
  for (uint32_t i = 0; i < n; i++)
    map[LocalKey(b.voxels[i].x, b.voxels[i].y, b.voxels[i].z)] = i;

  // 6-connected components in body-local space
  std::vector<int32_t> comp(n, -1);
  std::vector<uint32_t> compSize;
  std::vector<uint32_t> stack;
  for (uint32_t seed = 0; seed < n; seed++) {
    if (comp[seed] != -1) continue;
    int32_t c = (int32_t)compSize.size();
    uint32_t size = 0;
    stack.assign(1, seed);
    comp[seed] = c;
    while (!stack.empty()) {
      uint32_t i = stack.back();
      stack.pop_back();
      size++;
      const DebrisVoxel& v = b.voxels[i];
      const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                           {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (auto& dd : d) {
        auto it = map.find(LocalKey(v.x + dd[0], v.y + dd[1], v.z + dd[2]));
        if (it != map.end() && comp[it->second] == -1) {
          comp[it->second] = c;
          stack.push_back(it->second);
        }
      }
    }
    compSize.push_back(size);
  }
  if (compSize.size() <= 1) return;

  uint32_t keep = 0;
  for (uint32_t c = 1; c < compSize.size(); c++)
    if (compSize[c] > compSize[keep]) keep = c;

  std::vector<std::vector<DebrisVoxel>> parts(compSize.size());
  for (uint32_t c = 0; c < compSize.size(); c++) parts[c].reserve(compSize[c]);
  for (uint32_t i = 0; i < n; i++) parts[comp[i]].push_back(b.voxels[i]);

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  b.voxels = std::move(parts[keep]);
  bool madeBody = false;

  for (uint32_t c = 0; c < (uint32_t)parts.size(); c++) {
    if (c == keep) continue;
    if (parts[c].size() >= 8 && bodies_.size() + fragments.size() < kMaxBodies) {
      // body-worthy fragment: its own body at the same pose, rebased to its
      // min corner (like SplitBody halves), keeping the parent's momentum
      IVec3 mn{127, 127, 127};
      for (const DebrisVoxel& v : parts[c]) {
        mn.x = std::min<int>(mn.x, v.x);
        mn.y = std::min<int>(mn.y, v.y);
        mn.z = std::min<int>(mn.z, v.z);
      }
      for (DebrisVoxel& v : parts[c]) {
        v.x = (int8_t)(v.x - mn.x);
        v.y = (int8_t)(v.y - mn.y);
        v.z = (int8_t)(v.z - mn.z);
      }
      Body nb;
      nb.xf = b.xf;
      nb.xf.pos += QuatRot(b.xf.quat, Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
      nb.handle = phys_->CreateDebrisBodyXf(parts[c], nb.xf, densityOf_);
      if (nb.handle != 0) {
        phys_->SetBodyVelocities(nb.handle, lin, ang);
        nb.voxels = std::move(parts[c]);
        float r = 0;
        for (const DebrisVoxel& v : nb.voxels)
          r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
        nb.radiusVoxels = r + 2.0f;
        nb.serial = nextSerial_++;
        RecountBurn(nb);
        fragments.push_back(std::move(nb));
        madeBody = true;
        continue;
      }
      // body creation failed: fall through to particles (coords were rebased,
      // but VoxelsToParticles reads them against nb.xf — rebuild not worth it;
      // un-rebase instead)
      for (DebrisVoxel& v : parts[c]) {
        v.x = (int8_t)(v.x + mn.x);
        v.y = (int8_t)(v.y + mn.y);
        v.z = (int8_t)(v.z + mn.z);
      }
    }
    // small clump: back to loose voxels, flying with the body's point velocity
    VoxelsToParticles(b, parts[c], lin, ang, world, spawns);
  }

  if (madeBody) {
    // fragment bodies occupy space the parent's old compound still covers:
    // rebuild the parent NOW or the ghost boxes fight the new bodies
    uint64_t nh = phys_->CreateDebrisBodyXf(b.voxels, b.xf, densityOf_);
    if (nh != 0) {
      phys_->RemoveBody(b.handle);
      b.handle = nh;
      phys_->SetBodyVelocities(nh, lin, ang);
      b.burnedSinceRebuild = 0;
    }
  }
}

void DebrisSystem::VoxelsToParticles(const Body& b,
                                     const std::vector<DebrisVoxel>& voxels,
                                     Vec3 lin, Vec3 ang, World& world,
                                     std::vector<ParticleSpawn>& spawns) const {
  for (const DebrisVoxel& v : voxels) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) return;  // ring full: lost
    Vec3 wp = b.xf.pos + QuatRot(b.xf.quat, Vec3{(float)v.x + 0.5f,
                                                 (float)v.y + 0.5f,
                                                 (float)v.z + 0.5f});
    if (!world.CellInWindow({ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)})) continue;
    // rigid point velocity (voxels/s), converted to fixed 24.8 voxels/tick
    Vec3 vel = lin + ang.cross(wp - b.xf.pos);
    ParticleSpawn s;
    s.px = (int32_t)std::lround(wp.x * 256.0f);
    s.py = (int32_t)std::lround(wp.y * 256.0f);
    s.pz = (int32_t)std::lround(wp.z * 256.0f);
    s.vx = (int32_t)std::lround(vel.x * 256.0f / 30.0f);
    s.vy = (int32_t)std::lround(vel.y * 256.0f / 30.0f);
    s.vz = (int32_t)std::lround(vel.z * 256.0f / 30.0f);
    s.payload = v.payload;
    s.flags = 1u;  // PFLAG_ALIVE
    spawns.push_back(s);
  }
}

bool DebrisSystem::AnyDirtyNear(const Body& b, const WorldSnapshot& snap,
                                World& world) const {
  if (!snap.valid || snap.dirtyFlags.empty()) return false;
  float r = b.radiusVoxels + 1.0f;
  int lo[3] = {ifloor(b.xf.pos.x - r) >> 4, ifloor(b.xf.pos.y - r) >> 4,
               ifloor(b.xf.pos.z - r) >> 4};
  int hi[3] = {ifloor(b.xf.pos.x + r) >> 4, ifloor(b.xf.pos.y + r) >> 4,
               ifloor(b.xf.pos.z + r) >> 4};
  for (int cz = lo[2]; cz <= hi[2]; cz++)
    for (int cy = lo[1]; cy <= hi[1]; cy++)
      for (int cx = lo[0]; cx <= hi[0]; cx++) {
        IVec3 wc{cx, cy, cz};
        if (!world.ChunkInWindow(wc)) continue;
        uint32_t si = World::SlotChunkIndex(wc);
        if (si < snap.dirtyFlags.size() && snap.dirtyFlags[si]) return true;
      }
  return false;
}

void DebrisSystem::AddTerrainAnchor(Vec3 posVoxel, float radiusVoxels) {
  extraAnchors_.push_back({posVoxel, radiusVoxels});
}

void DebrisSystem::AdoptBody(uint64_t handle, std::vector<DebrisVoxel> voxels,
                             const BodyTransform& xf) {
  if (handle == 0 || voxels.empty()) return;
  Body body;
  body.handle = handle;
  body.voxels = std::move(voxels);
  body.xf = xf;
  IVec3 mn{127, 127, 127}, mx{-128, -128, -128};
  for (const DebrisVoxel& v : body.voxels) {
    mn.x = std::min<int>(mn.x, v.x); mn.y = std::min<int>(mn.y, v.y); mn.z = std::min<int>(mn.z, v.z);
    mx.x = std::max<int>(mx.x, v.x); mx.y = std::max<int>(mx.y, v.y); mx.z = std::max<int>(mx.z, v.z);
  }
  float ex = (float)(mx.x - mn.x + 1), ey = (float)(mx.y - mn.y + 1),
        ez = (float)(mx.z - mn.z + 1);
  body.radiusVoxels = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez) + 2.0f;
  body.serial = nextSerial_++;
  RecountBurn(body);
  bodies_.push_back(std::move(body));
  instancesDirty_ = true;
}

bool DebrisSystem::SplitBody(uint64_t handle, Vec3 planePointVoxel,
                             Vec3 planeNormal) {
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;
  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);

  // plane into body-local space (conjugate rotation)
  const float qx = -b.xf.quat[0], qy = -b.xf.quat[1], qz = -b.xf.quat[2],
              qw = b.xf.quat[3];
  auto rotInv = [&](Vec3 v) {
    Vec3 u{qx, qy, qz};
    Vec3 t = u.cross(v) * 2.0f;
    return v + t * qw + u.cross(t);
  };
  Vec3 pLocal = rotInv(planePointVoxel - b.xf.pos);
  Vec3 nLocal = rotInv(planeNormal).normalized();

  std::vector<DebrisVoxel> halves[2];
  for (const DebrisVoxel& v : b.voxels) {
    Vec3 c{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    halves[(c - pLocal).dot(nLocal) >= 0 ? 1 : 0].push_back(v);
  }
  if (halves[0].size() < 4 || halves[1].size() < 4) return false;

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  auto rot = [&](Vec3 v) {
    Vec3 u{-qx, -qy, -qz};
    Vec3 t = u.cross(v) * 2.0f;
    return v + t * qw + u.cross(t);
  };

  Body newBodies[2];
  for (int h = 0; h < 2; h++) {
    // rebase to the half's own min corner (keeps int8 coords tight) and
    // shift the body position by the rotated offset so nothing moves
    IVec3 mn{127, 127, 127};
    for (const DebrisVoxel& v : halves[h]) {
      mn.x = std::min<int>(mn.x, v.x);
      mn.y = std::min<int>(mn.y, v.y);
      mn.z = std::min<int>(mn.z, v.z);
    }
    for (DebrisVoxel& v : halves[h]) {
      v.x = (int8_t)(v.x - mn.x);
      v.y = (int8_t)(v.y - mn.y);
      v.z = (int8_t)(v.z - mn.z);
    }
    BodyTransform xf = b.xf;
    Vec3 shift = rot(Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
    xf.pos += shift;
    newBodies[h].handle = phys_->CreateDebrisBodyXf(halves[h], xf, densityOf_);
    if (newBodies[h].handle == 0) {
      if (h == 1 && newBodies[0].handle) phys_->RemoveBody(newBodies[0].handle);
      return false;
    }
    newBodies[h].voxels = std::move(halves[h]);
    newBodies[h].xf = xf;
    float r = 0;
    for (const DebrisVoxel& v : newBodies[h].voxels)
      r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
    newBodies[h].radiusVoxels = r + 2.0f;
    newBodies[h].serial = nextSerial_++;
    RecountBurn(newBodies[h]);
    phys_->SetBodyVelocities(newBodies[h].handle, lin, ang);
  }

  phys_->RemoveBody(b.handle);
  bodies_[bi] = std::move(newBodies[0]);
  bodies_.push_back(std::move(newBodies[1]));
  instancesDirty_ = true;
  return true;
}

void DebrisSystem::ManageTerrain(uint32_t tick, World& world) {
  const WorldSnapshot& snap = world.Snap();

  // which chunks need collision right now? (around every dynamic body and
  // this tick's registered mob-limb anchors)
  std::vector<IVec3> needed;
  auto needAround = [&](Vec3 pos, float radius) {
    float r = radius + 6.0f;
    int lo[3] = {ifloor(pos.x - r) >> 4, ifloor(pos.y - r) >> 4,
                 ifloor(pos.z - r) >> 4};
    int hi[3] = {ifloor(pos.x + r) >> 4, ifloor(pos.y + r) >> 4,
                 ifloor(pos.z + r) >> 4};
    for (int cz = lo[2]; cz <= hi[2]; cz++)
      for (int cy = lo[1]; cy <= hi[1]; cy++)
        for (int cx = lo[0]; cx <= hi[0]; cx++)
          if (world.ChunkInWindow({cx, cy, cz})) needed.push_back({cx, cy, cz});
  };
  for (const Body& b : bodies_) needAround(b.xf.pos, b.radiusVoxels);
  for (const auto& [pos, r] : extraAnchors_) needAround(pos, r);
  extraAnchors_.clear();
  auto keyLess = [](IVec3 a, IVec3 b) {
    return World::PackChunkKey(a) < World::PackChunkKey(b);
  };
  auto keyEq = [](IVec3 a, IVec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
  std::sort(needed.begin(), needed.end(), keyLess);
  needed.erase(std::unique(needed.begin(), needed.end(), keyEq), needed.end());

  for (IVec3 wc : needed) {
    TerrainEntry& t = terrain_[World::PackChunkKey(wc)];
    t.wc = wc;
    t.lastNeeded = tick;
    const CachedChunk* cc = world.Cached(wc);
    if (!cc) {
      world.RequestChunkFetch(wc);
      continue;
    }
    // refresh when the sim says the chunk changed (rate-limited). dirtyFlags
    // are slot-indexed under the CURRENT window origin.
    uint32_t slot = World::SlotChunkIndex(wc);
    if (slot < snap.dirtyFlags.size() && snap.dirtyFlags[slot] &&
        tick > t.lastRefreshReq + kTerrainRefreshTicks) {
      t.lastRefreshReq = tick;
      world.RequestChunkFetch(wc);
    }
    if (t.builtVersion >= cc->version && t.handle != 0) continue;
    if (t.builtVersion >= cc->version && t.handle == 0 && t.builtVersion != 0)
      continue;  // built empty at this version

    // (re)build the marching-cubes patch. Solids AND powders carry weight;
    // liquids don't (debris sinks). Missing neighbor chunks sample as empty —
    // transient until their fetch lands.
    auto solidAt = [&](int x, int y, int z) -> bool {
      if (!world.CellInWindow({x, y, z}))
        return true;  // residency edge is solid (matches sim rule)
      const CachedChunk* n = world.Cached(ChunkOfCell(x, y, z));
      if (!n || n->voxels.size() != kChunkVol) return false;
      uint32_t lx = (uint32_t)(x & 15), ly = (uint32_t)(y & 15),
               lz = (uint32_t)(z & 15);
      uint32_t mat = n->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
      if (mat == 0 || mat >= classOf_.size()) return false;
      return classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER;
    };
    IVec3 origin{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    PolygonizeChunk(origin, solidAt, verts, indices);

    // identical collision surface (liquids flowed, blood dried, gases moved):
    // keep the existing mesh and — critically — do NOT wake sleeping bodies.
    // Without this, a drying pool re-wakes every settled body nearby forever.
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the mesh bytes
    auto mix = [&h](const void* p, size_t n) {
      const uint8_t* b = (const uint8_t*)p;
      for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(verts.data(), verts.size() * sizeof(float));
    mix(indices.data(), indices.size() * sizeof(uint32_t));
    if (t.builtVersion != 0 && h == t.meshHash) {
      t.builtVersion = cc->version;
      continue;
    }

    if (t.handle) phys_->RemoveBody(t.handle);
    t.handle = indices.empty() ? 0 : phys_->CreateTerrainMesh(verts, indices);
    t.builtVersion = cc->version;
    t.meshHash = h;
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
  IVec3 wo = world_->WindowOrigin();
  Vec3 wlo{(float)(wo.x * (int)kChunk), (float)(wo.y * (int)kChunk),
           (float)(wo.z * (int)kChunk)};
  for (size_t i = 0; i < bodies_.size();) {
    Body& b = bodies_[i];
    phys_->GetTransform(b.handle, b.xf);
    // bodies that leave the residency window despawn: there is no terrain to
    // collide with out there (Noita despawns offscreen bodies the same way)
    const float kPad = 32.0f;
    bool dead = b.xf.pos.x < wlo.x - kPad || b.xf.pos.y < wlo.y - kPad ||
                b.xf.pos.z < wlo.z - kPad ||
                b.xf.pos.x > wlo.x + (float)kWorldN + kPad ||
                b.xf.pos.y > wlo.y + (float)kWorldN + kPad ||
                b.xf.pos.z > wlo.z + (float)kWorldN + kPad;
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
