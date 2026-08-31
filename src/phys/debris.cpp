#include "phys/debris.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "phys/lattice.h"
#include "phys/marching_cubes.h"
#include "sim/bytestream.h"
#include "sim/reactcpu.h"
#include "sim/rng.h"
#include "sim/tuning.h"

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

// Body budget policy. Anything that comes loose as a coherent object earns a
// rigidbody — a felled trunk falls as a log, not as a puff of powder. The only
// gates are the size floor (below it matter is individual voxels, not an
// object) and the global kMaxBodies ceiling.
//
// The per-scan cap used to be 4, which quietly decided that the 5th-largest
// piece of a shattered tree — however big — crumbled to rubble instead. That
// is what made felled wood change material on screen. It is now the body
// ceiling itself: a scan may fill every free slot, and PostStep's oldest-first
// despawn is what keeps the population bounded.
constexpr uint32_t kMinBodyVoxels = 8;               // below this: rubble
constexpr uint32_t kMaxNewBodiesPerScan = kMaxBodies;  // only the global cap
constexpr uint32_t kMaxNewBodiesPerTick = 4;   // shatter's share, all bodies

// Fragments broken off a BURNING body face a much higher bar than a fresh
// island. A body disintegrating in a fire re-fragments every few ticks, and
// each fragment that earns a body re-fragments in turn — that recursion is
// what turned one burning tree into hundreds of tiny bodies chugging the CPU.
// Charred bits that fall off a burning object are visually just embers, so
// below this they become ballistic particles and stay in the CA.
constexpr uint32_t kMinBurnFragmentVoxels = 24;

// Body burn budgets: voxel scans across all bodies per tick, grid writes
// (emitted fire / escaping ash+smoke) per tick, and how many voxels must burn
// away before the Jolt collider is rebuilt to match the charred shape.
constexpr uint32_t kBurnScanPerTick = 4096;
constexpr uint32_t kBurnOpsPerTick = 384;
constexpr uint32_t kBurnRebuildVoxels = 12;
// Connectivity re-check after burning is O(n) per body; run it only when
// enough matter has actually left to plausibly disconnect the remainder.
constexpr uint32_t kShatterCheckVoxels = 6;

// ---- impact cue budgets (DESIGN.md §12b) ------------------------------------
// The fixed sim rate, which is also the rate PostStep runs at, so the per-body
// impact gap authored in SECONDS can be expressed in PostStep counts without
// threading a clock through eight call sites.
constexpr float kSimTicksPerSecond = 30.0f;
// At most this many impacts are VOICED per step, loudest first. See the
// ImpactEvent comment in debris.h for why the cap picks rather than truncates.
constexpr size_t kMaxImpactsPerStep = 4;

// sim/rng.h — so burn rolls replay identically for a given
// (body serial, tick, voxel, rule).
using rng::Hash3;
using rng::Pcg;

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

// A body's AUTHORITATIVE voxel lattice, addressed uniformly.
//
// A body has one lattice or two: `skinVoxels` (int16 PrefabVoxel, skinScale
// units) when its skin is finer than its collider, and `voxels` (int8
// DebrisVoxel, physScale units) otherwise. Burning must edit whichever one is
// authoritative — writing to the derived collider of a fine-skinned body would
// be undone by the next majority-fill re-derive, silently — and it has to
// behave identically on both, because a corpse is a fine-skinned body and a
// plank is not, and "cloth catches faster than flesh" must not be two
// implementations that happen to agree.
//
// The twin of LimbLattice in game/mob.cpp; the two populations meet at
// AdoptBody, and the same voxel must burn the same on either side of it.
struct BodyLattice {
  std::vector<PrefabVoxel>* skin = nullptr;
  std::vector<DebrisVoxel>* coll = nullptr;
  uint32_t scale = 1;  // lattice units per world voxel

  size_t Size() const { return skin ? skin->size() : coll->size(); }
  IVec3 At(size_t i) const {
    return skin ? IVec3{(*skin)[i].x, (*skin)[i].y, (*skin)[i].z}
                : IVec3{(*coll)[i].x, (*coll)[i].y, (*coll)[i].z};
  }
  uint32_t Mat(size_t i) const {
    return skin ? (uint32_t)((*skin)[i].material & 0xFFFu)
                : (uint32_t)((*coll)[i].payload & 0xFFFu);
  }
  // Rewrite a voxel's material, keeping a cosmetic variant and ZEROING the art
  // slot — microbody.wgsl lets a nonzero art colour override the material
  // colour, so a charred voxel that kept its slot goes on being painted
  // robe-purple and the charring is invisible on exactly the painted surfaces
  // it matters most on.
  void Set(size_t i, uint32_t mat, uint32_t variant) const {
    const uint16_t w = (uint16_t)((mat & 0xFFFu) | ((variant & 3u) << 12));
    if (skin) {
      (*skin)[i].material = w;
      (*skin)[i].color = 0;
    } else {
      (*coll)[i].payload = w;
      (*coll)[i].color = 0;
    }
  }
};
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
  foliageOf_.clear();
  matGpu_.clear();
  matSelfActive_.clear();
  matHasPair_.clear();
  matHasScaled_.clear();
  reactions_ = reactions;
  uint32_t selfIdx = 0;
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
    // Does ANY of this material's rules use the neighbour-count ramp? The burn
    // pass needs the body-local occupancy map to count neighbours, and an
    // inactive body normally skips building it — so without this flag a scaled
    // rule on an inactive body would count every direction as "world", which is
    // the divergence in the opposite direction from the one reactcpu.h fixes.
    uint8_t hasScaled = 0;
    for (uint32_t ri = 0; ri < m.gpu.reactCount; ri++)
      if (ReactScaleArmed(reactions_[m.gpu.reactOffset + ri])) hasScaled = 1;
    matHasScaled_.push_back(hasScaled);
    // Rubble = what a voxel becomes when it crumbles to loose matter. An
    // undeclared rubble form defaults to the material ITSELF: a scrap of wood
    // is still wood, and transmuting it (the old organic->dust / ->gravel
    // guess) is why a felled tree's leftovers came out yellow-tan instead of
    // brown. Materials that genuinely pulverize say so in JSON.
    int r = m.rubble.empty() ? -1 : FindMaterialId(mats, m.rubble);
    if (r < 0) r = (int)selfIdx;
    rubbleOf_.push_back((uint32_t)r);
    selfIdx++;
    uint8_t foliage = 0;
    for (const auto& t : m.tags)
      if (t == "foliage") foliage = 1;
    foliageOf_.push_back(foliage);
  }
  for (Body& b : bodies_) RecountBurn(b);  // hot-reload can change rule sets
}

void DebrisSystem::RecountBurn(Body& b) const {
  b.activeCount = 0;
  b.pairCount = 0;
  // Counted on the AUTHORITATIVE lattice. On a fine-skinned body `voxels` is a
  // majority-filled derivation, so a handful of burning skin voxels can vanish
  // from it entirely — and a body whose activeCount reads 0 is a body this pass
  // skips, i.e. a corpse that stops burning for no visible reason.
  auto tally = [&](uint32_t m) {
    if (m == 0 || m >= matGpu_.size()) return;
    if (matSelfActive_[m]) b.activeCount++;
    if (matHasPair_[m]) b.pairCount++;
  };
  if (b.HasFineSkin()) {
    for (const PrefabVoxel& v : b.skinVoxels) tally(v.material & 0xFFFu);
  } else {
    for (const DebrisVoxel& v : b.voxels) tally(v.payload & 0xFFFu);
  }
  // Cached identity for the impact cue, refreshed here because this is already
  // the one function every site that rewrites a body's voxel lattice calls —
  // adoption, split, shatter fragment, damage, materials hot-reload. A body
  // that burns down to mostly ash starts sounding like ash, which is right.
  b.domMat = DominantMaterial(b.voxels);
}

void DebrisSystem::Reset() {
  for (Body& b : bodies_) ReleaseBody(b);
  bodies_.clear();
  for (auto& [ci, t] : terrain_)
    if (t.handle) phys_->RemoveBody(t.handle);
  terrain_.clear();
  events_.clear();
  pendingSupport_.clear();
  supportPending_.clear();
  supportCooldown_.clear();
  // Undrained breaks belong to the world that just went away; voicing them
  // after a regen or a LoadWorld would fire a burst of snaps at coordinates
  // that now mean something else entirely.
  breaks_.clear();
  // Same reasoning for undrained impacts: the bodies that made them are gone.
  impacts_.clear();
  instancesDirty_ = true;
  instanceCount_ = 0;
  // Body serials seed the burn RNG (Hash3(serial, tick, rule)), so a counter
  // that survives Reset() makes every body's burn sequence depend on how many
  // bodies happened to exist before it. That silently couples unrelated
  // scenarios: a worldgen tweak that changes how many islands an earlier
  // destruction produces re-rolls a later fire's outcome entirely. Resetting
  // here makes a body's burn a function of the scenario, not of history.
  nextSerial_ = 1;
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
                                      std::vector<CellOp>& cellOps,
                                      std::vector<ParticleSpawn>& spawns) {
  const int dx = e.hi.x - e.lo.x + 1;
  const int dy = e.hi.y - e.lo.y + 1;
  const int dz = e.hi.z - e.lo.z + 1;
  const size_t vol = (size_t)dx * dy * dz;
  auto lidx = [&](int x, int y, int z) {
    return (size_t)((z * dy + y) * dx + x);
  };

  // solid mask from the chunk cache (solids only: powders/liquids fall on
  // their own in the CA). The chunk pointer is hoisted out of the x loop: a
  // 64^3 region is 262144 cells but only ~64 chunks, and Cached() is a hash
  // lookup.
  std::vector<uint32_t> words(vol, 0);
  std::vector<uint8_t> solid(vol, 0);
  for (int z = 0; z < dz; z++)
    for (int y = 0; y < dy; y++) {
      int wy = e.lo.y + y, wz = e.lo.z + z;
      const CachedChunk* cc = nullptr;
      int ccx = INT32_MIN;
      for (int x = 0; x < dx; x++) {
        int wx = e.lo.x + x;
        if ((wx >> 4) != ccx) {
          ccx = wx >> 4;
          cc = world.Cached({ccx, wy >> 4, wz >> 4});
          if (cc && cc->voxels.size() != kChunkVol) cc = nullptr;
        }
        if (!cc) continue;
        uint32_t lx = (uint32_t)(wx & 15), ly = (uint32_t)(wy & 15),
                 lz = (uint32_t)(wz & 15);
        uint32_t w = cc->voxels[(lz * kChunk + ly) * kChunk + lx];
        uint32_t mat = w & 0xFFF;
        words[lidx(x, y, z)] = w;
        solid[lidx(x, y, z)] =
            mat != 0 && mat < classOf_.size() && classOf_[mat] == CLASS_SOLID;
      }
    }

  // Does a solid voxel sit just outside the region at this face cell? Used to
  // decide whether a component leaving the region is really attached to more
  // structure out there, or just happens to graze the box. One cache lookup
  // per query, only for boundary cells of unanchored components.
  auto solidOutside = [&](int wx, int wy, int wz) -> bool {
    if (!world.CellInWindow({wx, wy, wz})) return true;  // window edge is solid
    const CachedChunk* cc = world.Cached(ChunkOfCell(wx, wy, wz));
    if (!cc || cc->voxels.size() != kChunkVol) return true;  // unknown: assume
    uint32_t mat = cc->voxels[((uint32_t)(wz & 15) * kChunk +
                               (uint32_t)(wy & 15)) * kChunk +
                              (uint32_t)(wx & 15)] & 0xFFF;
    return mat != 0 && mat < classOf_.size() &&
           (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER);
  };

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
      // Leaving the region only anchors when the structure actually CONTINUES
      // outside: a cell on the boundary face whose outward neighbor is solid
      // is attached to matter we can't see, so the component stays put. A tree
      // crown that merely pokes through the top/side of the scan box has air
      // out there and is free to fall.
      //
      // Treating any boundary contact as anchored (the old rule) is why
      // felling a tree produced nothing: at kSupportMargin the region is 64^3,
      // a tree is taller than that, so the crown always grazed a face and was
      // pinned — while its dithered rim leaves became sub-8 islands and got
      // deleted. Now only the trunk's actual ground contact anchors it.
      if (x == 0 && solidOutside(e.lo.x - 1, e.lo.y + y, e.lo.z + z)) comp.anchored = true;
      if (y == 0 && solidOutside(e.lo.x + x, e.lo.y - 1, e.lo.z + z)) comp.anchored = true;
      if (z == 0 && solidOutside(e.lo.x + x, e.lo.y + y, e.lo.z - 1)) comp.anchored = true;
      if (x == dx - 1 && solidOutside(e.hi.x + 1, e.lo.y + y, e.lo.z + z)) comp.anchored = true;
      if (y == dy - 1 && solidOutside(e.lo.x + x, e.hi.y + 1, e.lo.z + z)) comp.anchored = true;
      if (z == dz - 1 && solidOutside(e.lo.x + x, e.lo.y + y, e.hi.z + 1)) comp.anchored = true;
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

  // Largest components first: when a scan turns up more loose structure than
  // the per-tick body budget covers, the tree gets the body and the twigs fall
  // back to rubble, rather than the arbitrary scan order deciding.
  std::vector<uint32_t> order;
  order.reserve(comps.size());
  for (uint32_t c = 0; c < (uint32_t)comps.size(); c++)
    if (!comps[c].anchored) order.push_back(c);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return comps[a].cells.size() > comps[b].cells.size();
  });

  uint32_t madeThisScan = 0;
  for (uint32_t ci : order) {
    const Comp& comp = comps[ci];
    if (cellOps.size() + comp.cells.size() > kMaxCellOpsPerTick) break;  // next tick

    // Body-worthiness. A scan over a burning forest can turn up dozens of
    // loose components; each body costs a compound-shape build plus permanent
    // per-tick broadphase/terrain-meshing work, so past the per-scan budget
    // (or below the size floor) matter goes back to the CA as rubble instead.
    // Rubble is nearly free: it is just voxels the GPU already simulates.
    bool worthBody = comp.cells.size() >= kMinBodyVoxels &&
                     madeThisScan < kMaxNewBodiesPerScan &&
                     bodies_.size() < kMaxBodies;
    if (!worthBody) {
      // Rubble handoff: too small to read as an object, so it crumbles to
      // individual voxels. It keeps its own material unless the JSON names a
      // rubble form (stone -> gravel, glass -> sand); matter is never
      // transmuted just because it came loose. Foliage clumps vanish instead:
      // procgen crowns dither their rims with isolated voxels by design, so any
      // support scan near a tree finds hundreds of sub-8 leaf "islands" — as
      // rubble that was a rain of ash through the canopy the first time a tree
      // burned or anything moved nearby.
      for (size_t i : comp.cells) {
        int x = (int)(i % dx), y = (int)((i / dx) % dy), z = (int)(i / ((size_t)dx * dy));
        int wx = e.lo.x + x, wy = e.lo.y + y, wz = e.lo.z + z;
        uint32_t mat = words[i] & 0xFFF;
        uint32_t cellIdx = CellIndexOf(wx, wy, wz);
        if (mat < foliageOf_.size() && foliageOf_[mat]) {
          cellOps.push_back({cellIdx, 0u});
          continue;
        }
        uint32_t rub = mat < rubbleOf_.size() ? rubbleOf_[mat] : 0;
        // palette variant, not a raw 2-bit mask: `& 3u` produced state 3, which
        // no material has a colour for (the shaders all select with `% 3u`).
        uint32_t state = ((cellIdx * 2654435761u) >> 8) % 3u;
        if (rub < matGpu_.size() && matGpu_[rub].klass == CLASS_LIQUID)
          state = 7u;  // LIQ_FULL_STATE: the nibble is fullness for liquids
        // A scrap that keeps its own SOLID material cannot fall in the grid
        // (sim_step returns early for CLASS_SOLID), so writing it back in place
        // would leave it hanging where its support used to be. Hand it to the
        // particle system instead: it carries the payload verbatim, falls, and
        // rejoins the grid as itself. Powder/liquid rubble still goes straight
        // back to the CA, which already moves it.
        bool frozen = rub < matGpu_.size() && matGpu_[rub].klass == CLASS_SOLID;
        if (frozen && spawns.size() < kMaxParticleSpawnsPerTick) {
          ParticleSpawn s{};
          s.px = (int32_t)((wx * 256) + 128);
          s.py = (int32_t)((wy * 256) + 128);
          s.pz = (int32_t)((wz * 256) + 128);
          s.payload = (uint16_t)((rub & 0xFFF) | (state << 12));
          s.flags = 1u;  // PFLAG_ALIVE
          spawns.push_back(s);
          cellOps.push_back({cellIdx, 0u});  // vacate the grid cell
          continue;
        }
        uint32_t word = PackVoxNew(rub, state);
        cellOps.push_back({cellIdx, word});
      }
      lastCellWriteTick_ = tick;
      continue;
    }

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

    // Audible break, and the body's identity for any later impact cue. Both
    // use the piece's MOST COMMON material — see DominantMaterial.
    body.domMat = DominantMaterial(body.voxels);
    breaks_.push_back(BreakEvent{
        Vec3{(float)origin.x + 0.5f * ex, (float)origin.y + 0.5f * ey,
             (float)origin.z + 0.5f * ez},
        body.domMat, (int32_t)body.voxels.size()});

    bodies_.push_back(std::move(body));
    instancesDirty_ = true;
    madeThisScan++;
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
      RunIslandDetection(e, tick, world, cellOps, spawns);
      // terrain under the blast changed: sleeping debris nearby must re-check
      Vec3 c{(float)(e.lo.x + e.hi.x) * 0.5f, (float)(e.lo.y + e.hi.y) * 0.5f,
             (float)(e.lo.z + e.hi.z) * 0.5f};
      settle_.blastWakes++;
      settle_.lastWakeTick = tick;
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

namespace {

// Collapse a MICRO-unit voxel set (scale micro voxels per world voxel) down to
// world voxels, one output voxel per scale^3 block. A block that is less than
// half full becomes air, and a block that survives takes its most common
// material — so a scale-2 critter leg settles as the solid 3x1x1 stub it
// visually is, rather than as a 6x2x2 lump at twice its real size.
//
// The vote is a linear scan over a tiny fixed array rather than a map: a limb
// block holds at most scale^3 (8 or 64) voxels and realistically one or two
// distinct materials, and at those sizes a hash map is all overhead. Past
// kMaxDistinct materials in one block the first-seen ones win, which is
// invisible — no authored limb has eight materials in a 2x2x2 box.
std::vector<DebrisVoxel> DownsampleMicro(const std::vector<DebrisVoxel>& src,
                                         uint32_t scale) {
  const int s = (int)scale;
  const uint32_t full = (uint32_t)(s * s * s);
  constexpr int kMaxDistinct = 8;
  struct Blk {
    uint32_t count = 0;
    int n = 0;
    uint16_t mat[kMaxDistinct]{};
    uint32_t hits[kMaxDistinct]{};
  };
  std::unordered_map<uint64_t, Blk> blocks;
  for (const DebrisVoxel& v : src) {
    // DebrisVoxel coords are non-negative by construction (bodies are rebased
    // to their min corner), so a plain divide is the block index.
    uint64_t key = ((uint64_t)(uint8_t)((int)v.x / s) << 32) |
                   ((uint64_t)(uint8_t)((int)v.y / s) << 16) |
                   (uint8_t)((int)v.z / s);
    Blk& blk = blocks[key];
    blk.count++;
    int k = 0;
    for (; k < blk.n; k++)
      if (blk.mat[k] == v.payload) break;
    if (k == blk.n && blk.n < kMaxDistinct) blk.mat[blk.n++] = v.payload;
    if (k < kMaxDistinct) blk.hits[k]++;
  }

  std::vector<DebrisVoxel> out;
  out.reserve(blocks.size());
  for (const auto& [key, blk] : blocks) {
    if (blk.count * 2 < full) continue;  // majority-fill: mostly air -> air
    int best = 0;
    for (int k = 1; k < blk.n; k++)
      if (blk.hits[k] > blk.hits[best]) best = k;
    out.push_back({(int8_t)(uint8_t)(key >> 32), (int8_t)(uint8_t)(key >> 16),
                   (int8_t)(uint8_t)key, 0, blk.mat[best]});
  }
  return out;
}

}  // namespace

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
    ++b.inactiveTicks;
    settle_.maxInactiveTicks =
        std::max(settle_.maxInactiveTicks, b.inactiveTicks);
    if (b.inactiveTicks < kSettleAfterTicks) continue;
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

    // ---- micro -> world downsample (PLAN §C, one body, only on settle) ----
    // A micro body's voxels are in 1/scale units; the GRID has no such thing.
    const std::vector<DebrisVoxel>* settleSrc = &b.voxels;
    std::vector<DebrisVoxel> downsampled;
    if (b.physScale > 1) {
      downsampled = DownsampleMicro(b.voxels, b.physScale);
      if (downsampled.empty()) continue;  // nothing survives: stay a body
      settleSrc = &downsampled;
    }

    // whole body must land inside the residency window, and this tick's op
    // budget must hold every voxel — partial settles would lose matter
    if (cellOps.size() + settleSrc->size() > kMaxCellOpsPerTick) continue;

    // snapped basis is a lattice bijection: with the body origin rounded to a
    // cell corner, voxel centers land on distinct cells — no self-collisions.
    IVec3 base{(int)std::lround(b.xf.pos.x), (int)std::lround(b.xf.pos.y),
               (int)std::lround(b.xf.pos.z)};
    bool inWindow = true;
    size_t opsStart = cellOps.size();
    for (const DebrisVoxel& v : *settleSrc) {
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
      // payload already carries mat+state; add the unstamped stamp field.
      uint32_t word = (uint32_t)v.payload | (kStampNever << kStampShift) |
                      kCellOpIfAir;
      cellOps.push_back({World::SlotCellIndex(cell), word});
    }
    if (!inWindow) {
      cellOps.resize(opsStart);
      continue;
    }

    lastCellWriteTick_ = tick;
    ReleaseBody(b);
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
  // shared across every body this tick: a forest fire breaks many bodies at
  // once, and each new body is a compound-shape build plus permanent per-tick
  // upkeep. Past the budget, fragments become particles instead.
  uint32_t newBodyBudget = kMaxNewBodiesPerTick;
  // fragment bodies split off by ShatterBody, appended after the loop (a
  // push_back into bodies_ mid-iteration would invalidate `b`)
  std::vector<Body> fragments;

  for (size_t bi = 0; bi < bodies_.size();) {
    Body& b = bodies_[bi];
    // THE AUTHORITATIVE LATTICE, which is not always `voxels`.
    //
    // A body with a finer skin carves `skinVoxels` and DERIVES `voxels` from it
    // by majority-fill, so burning the collider would be writing to derived
    // data — the "unowned diverging representation" failure the design
    // guidelines name, and it would be silently undone by the next re-derive.
    const bool fine = b.HasFineSkin();
    BodyLattice lat{fine ? &b.skinVoxels : nullptr, fine ? nullptr : &b.voxels,
                    fine ? b.micro.skinScale : b.physScale};
    uint32_t n = (uint32_t)lat.Size();
    bool active = b.activeCount > 0;
    // MICRO BODIES BURN. They did not in v1, for two structural reasons that
    // have both since expired: the copy-on-write brick pool shipped (so a
    // per-body edit IS visible — MicroBodyPoke rewrites one 16-bit voxel in
    // place), and this pass no longer maps body-local coordinates straight onto
    // world cells, it divides by the lattice scale like everything else here.
    //
    // Leaving them out was the visible half of "a corpse does not burn": every
    // mob limb of every rig with skinScale > 1 becomes a micro body the instant
    // it is severed or its owner dies, and so does every dropped item.
    if (n == 0 || scanBudget == 0 || (!active && b.pairCount == 0) ||
        (!active && !AnyDirtyNear(b, snap, world))) {
      bi++;
      continue;
    }
    // A micro body must OWN its brick before a poke can land: a shared model
    // backs every instance of its def, and charring one would char them all.
    // Done up front rather than at the first write so the poke sites stay
    // branch-free; a body that never changes pays one clone it did not need,
    // which only happens to a body the pass has already decided is reactive.
    if (b.micro.Valid() && microSet_) {
      const int own = MicroBodyOwn(*microSet_, b.micro.model);
      // Pool full: the body still really burns, its skin just stops keeping up.
      if (own >= 0) b.micro.model = (uint32_t)own;
    }
    const bool canPoke = b.micro.Valid() && microSet_;
    // The lattice scale is also the divisor for every world-space quantity
    // derived from a body-local coordinate. Getting this wrong does not fail
    // loudly — it puts a scale-8 limb's fire eight cells away from the limb.
    const float latInv = 1.0f / (float)std::max(1u, lat.scale);

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
    // Body-local LATTICE coordinate -> the world cell it sits in. The division
    // by the lattice scale is what makes this correct for a micro body: a
    // scale-8 voxel is one eighth of a world cell, and mapping its coordinate
    // straight onto a cell — which is what this did while micro bodies were
    // excluded — puts a limb's fire eight cells from the limb.
    auto worldOfLocal = [&](IVec3 v) {
      return b.xf.pos + rotQ(Vec3{((float)v.x + 0.5f) * latInv,
                                  ((float)v.y + 0.5f) * latInv,
                                  ((float)v.z + 0.5f) * latInv});
    };
    auto worldCellOf = [&](IVec3 v) {
      const Vec3 wp = worldOfLocal(v);
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
      return ReactNbrMatches(r, nm, matGpu_);
    };

    // Does any material present in this body use the neighbour-count ramp? If
    // so the local occupancy map has to exist even on an inactive body, or the
    // count would read every direction as world content.
    bool anyScaled = false;
    for (uint32_t i = 0; i < n && !anyScaled; i++) {
      uint32_t m = lat.Mat(i);
      if (m < matHasScaled_.size() && matHasScaled_[m]) anyScaled = true;
    }
    // local occupancy for internal spread; values are voxel indices, entries
    // whose payload was zeroed this pass read as absent
    const bool haveLocal = active || anyScaled;
    std::unordered_map<uint32_t, uint32_t> local;
    if (haveLocal) {
      local.reserve(n * 2);
      for (uint32_t i = 0; i < n; i++) {
        const IVec3 p = lat.At(i);
        local[LocalKey(p.x, p.y, p.z)] = i;
      }
    }
    auto localMatAt = [&](int x, int y, int z) -> uint32_t {
      if (!haveLocal) return 0u;
      auto it = local.find(LocalKey(x, y, z));
      if (it == local.end()) return 0u;
      return lat.Mat(it->second);
    };

    uint32_t removed = 0;
    bool changed = false;
    // rewrite a voxel to a rule product. Solids swap in place; anything else
    // (ash, smoke, fire, air) escapes into the grid at the voxel's world cell
    // and the voxel leaves the body.
    auto applyProduct = [&](uint32_t vi, uint32_t prod, uint32_t rr) {
      if (prod == kProdKeep) return;
      const IVec3 p = lat.At(vi);
      uint32_t pm = prod & 0xFFFu;
      if (pm != 0 && pm < matGpu_.size() && matGpu_[pm].klass == CLASS_SOLID) {
        lat.Set(vi, pm, (rr >> 6u) % 3u);
        // One 16-bit word of one model's block. NOT ReskinMicro, which
        // re-derives dims, rebases the origin and re-packs the whole payload —
        // right for a carve (the shape changed), catastrophic per tick for a
        // state change (the shape did not; one voxel's material did).
        if (canPoke)
          MicroBodyPoke(*microSet_, b.micro.model, p.x, p.y, p.z, (uint8_t)pm, 0);
      } else {
        if (pm != 0 && pm < matGpu_.size() && opsBudget > 0 &&
            cellOps.size() < kMaxCellOpsPerTick) {
          IVec3 cell = worldCellOf(p);
          if (world.CellInWindow(cell)) {
            uint32_t state = matGpu_[pm].klass == CLASS_LIQUID
                                 ? 7u  // LIQ_FULL_STATE
                                 : (rr >> 6u) % 3u;
            cellOps.push_back({World::SlotCellIndex(cell),
                               PackVoxNew(pm, state) | kCellOpIfAir});
            opsBudget--;
          }
        }
        lat.Set(vi, 0, 0);  // compacted below
        if (canPoke)
          MicroBodyPoke(*microSet_, b.micro.model, p.x, p.y, p.z, 0, 0);
        removed++;
      }
      changed = true;
    };

    const IVec3 kDirs[6] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0},
                            {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    // body-local lattice offset -> nearest WORLD direction (the inverse of
    // localDirOf, for the neighbour-count ramp's world fallback)
    auto worldDirOf = [&](IVec3 d) {
      Vec3 w = rotQ(Vec3{(float)d.x, (float)d.y, (float)d.z});
      return IVec3{(int)std::lround(w.x), (int)std::lround(w.y),
                   (int)std::lround(w.z)};
    };
    // The six face neighbours as scaleByNeighbors counts them: the body's own
    // lattice first, and the world cell in that direction when the lattice has
    // nothing there — because a SURFACE voxel's neighbours genuinely are grid
    // cells, and treating them as air would make every rule that ramps on
    // "matching neighbours" read a body's whole skin as isolated.
    auto countMatches = [&](IVec3 v, const ReactionGpu& r) {
      const bool invert = ReactScaleInverted(r);
      uint32_t count = 0;
      for (const IVec3& d : kDirs) {
        uint32_t nm = localMatAt(v.x + d.x, v.y + d.y, v.z + d.z);
        if (nm == 0) {
          // ONE LATTICE STEP, not one world cell: on a scale-8 body a whole
          // cell steps over eight of the body's own voxels and reads a
          // neighbourhood the voxel is nowhere near.
          const Vec3 wv = worldOfLocal(v) + rotQ(Vec3{(float)d.x * latInv,
                                                      (float)d.y * latInv,
                                                      (float)d.z * latInv});
          nm = worldMatAt({ifloor(wv.x), ifloor(wv.y), ifloor(wv.z)});
        }
        if (ReactNbrMatches(r, nm, matGpu_) != invert) count++;
      }
      return count;
    };
    uint32_t steps = std::min(n, scanBudget);
    scanBudget -= steps;
    for (uint32_t s = 0; s < steps; s++) {
      uint32_t vi = (b.burnCursor + s) % n;
      const IVec3 v = lat.At(vi);
      uint32_t m = lat.Mat(vi);
      if (m == 0 || m >= matGpu_.size()) continue;
      const MaterialGpu& mg = matGpu_[m];
      if (mg.reactCount == 0) continue;
      if (!active && !matHasPair_[m]) continue;

      for (uint32_t ri = 0; ri < mg.reactCount; ri++) {
        const ReactionGpu& r = reactions_[mg.reactOffset + ri];
        uint32_t kind = r.packed & 3u;
        uint32_t dmask = (r.packed >> 2u) & 7u;
        // The two gates sim_step.wgsl applies before the roll, in the same
        // order. Both were silently absent on this side until sim/reactcpu.h —
        // a rule authored with a day/night condition or a neighbour-count ramp
        // fired unconditionally at base chance on a body. See that header.
        //
        // seesSky = true: a rigidbody has no column to raycast, and refusing
        // instead would make a sky-gated rule permanently inert on bodies.
        if (!ReactLightMatches(r, dayPhase_, /*seesSky=*/true)) continue;
        uint32_t chance = r.chance;
        if (ReactScaleArmed(r)) {
          chance = ReactScaledChance(r, countMatches(v, r));
          if (chance == 0) continue;  // below minCount: no frontier, no rule
        }
        uint32_t rr = Hash3(b.serial * 0x9E3779B9u + vi, tick, ri);
        // one roll per rule, GPU-style — chance is in 1/kReactChanceDen units,
        // so this must use the same denominator sim_step.wgsl rolls against
        if (rr % kReactChanceDen >= chance) continue;
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
                                 PackVoxNew(r.prodNbr, (rr >> 8u) % 3u) |
                                     kCellOpIfAir});
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
          if (haveLocal) {
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
            const Vec3 wp = worldOfLocal(v);
            IVec3 wc{ifloor(wp.x), ifloor(wp.y), ifloor(wp.z)};
            if (nbrMatches(worldMatAt(wc), r)) {
              matched = -1;
            } else {
              for (const IVec3& d : kDirs) {
                // ONE LATTICE STEP (see countMatches): on a scale-8 limb a
                // whole-cell step reads eight voxels past its own surface.
                const Vec3 wn = wp + rotQ(Vec3{(float)d.x * latInv,
                                               (float)d.y * latInv,
                                               (float)d.z * latInv});
                if (nbrMatches(worldMatAt({ifloor(wn.x), ifloor(wn.y),
                                           ifloor(wn.z)}), r)) {
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
      // Compact the AUTHORITATIVE lattice, then re-derive the collider from it.
      // Data flows skin -> collider and never the other way, so a fine-skinned
      // body must not have its `voxels` edited here: the next re-derive would
      // silently undo it.
      if (fine) {
        b.skinVoxels.erase(
            std::remove_if(b.skinVoxels.begin(), b.skinVoxels.end(),
                           [](const PrefabVoxel& v) {
                             return (v.material & 0xFFFu) == 0;
                           }),
            b.skinVoxels.end());
        DeriveColliderFromSkin(b);
      } else {
        b.voxels.erase(
            std::remove_if(b.voxels.begin(), b.voxels.end(),
                           [](const DebrisVoxel& v) { return v.payload == 0; }),
            b.voxels.end());
      }
      b.burnedSinceRebuild += removed;
      const uint32_t latNow = (uint32_t)lat.Size();
      if (latNow) b.burnCursor %= latNow;
      // removals can disconnect the remainder: split fragments off (bodies /
      // ballistic particles) before recounting. The connectivity flood is O(n)
      // over the body, so it waits until enough matter has actually burned
      // away to plausibly disconnect anything — a body losing one voxel a tick
      // to embers doesn't re-flood every tick.
      // Threshold scales with the body: losing 4 voxels out of 13 can easily
      // sever it, out of 2000 it cannot. Small bodies therefore re-check
      // immediately (correctness — a clump must detach before the remainder
      // burns under the dissolve floor), big ones amortize (perf).
      b.burnedSinceShatter += removed;
      uint32_t shatterEvery =
          std::max(1u, std::min(kShatterCheckVoxels,
                                (uint32_t)b.voxels.size() / 16u));
      if (b.burnedSinceShatter >= shatterEvery) {
        b.burnedSinceShatter = 0;
        ShatterBody(b, world, fragments, spawns, kMinBurnFragmentVoxels,
                    newBodyBudget);
      }
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
      ReleaseBody(b);
      bodies_[bi] = std::move(bodies_.back());
      bodies_.pop_back();
      instancesDirty_ = true;
      continue;  // re-examine the swapped-in body at this index
    }

    // batched collider refresh: the charred shape sheds its burned voxels
    // (at most one Jolt rebuild per tick across all bodies)
    if (b.burnedSinceRebuild >= kBurnRebuildVoxels && !rebuiltOne) {
      // Re-pack the brick at the SAME batched cadence. The per-voxel pokes
      // above already made the burn visible; this only shrinks the OBB the
      // fragment shader marches, so it belongs on the expensive-work clock, not
      // on the per-voxel one. It may also rebase the origin, which is why it
      // runs BEFORE the collider is rebuilt from those voxels.
      if (b.micro.Valid()) ReskinMicro(b);
      Vec3 lin{}, ang{};
      phys_->GetBodyVelocities(b.handle, lin, ang);
      // Micro bodies build at 1/physScale: a scale-4 body's voxels are quarter
      // size, and building at pitch 1 would give it 64x its real volume and
      // mass (RebuildCollider says the same thing at the other call site).
      uint64_t nh = phys_->CreateDebrisBodyXf(
          b.voxels, b.xf, densityOf_, false,
          1.0f / (float)std::max(1u, b.physScale));
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
                               std::vector<ParticleSpawn>& spawns,
                               uint32_t minFragment, uint32_t& budget) {
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

  // A fine skin follows the COARSE partition: connectivity is decided once, on
  // the collider lattice, and each skin voxel joins whichever component owns
  // the collider block it sits in. Running a second connectivity pass on the
  // skin would be both slower and wrong — the two could disagree about how many
  // pieces there are, and then a fragment's art would not match its collider.
  std::vector<std::vector<PrefabVoxel>> skinParts;
  if (b.HasFineSkin()) {
    const int ratio = (int)std::max(1u, b.micro.skinScale / b.physScale);
    std::unordered_map<uint32_t, uint32_t> compOf;
    compOf.reserve(n * 2);
    for (uint32_t i = 0; i < n; i++)
      compOf[LocalKey(b.voxels[i].x, b.voxels[i].y, b.voxels[i].z)] = comp[i];
    skinParts.resize(compSize.size());
    for (const PrefabVoxel& sv : b.skinVoxels) {
      auto it = compOf.find(LocalKey((int)sv.x / ratio, (int)sv.y / ratio,
                                     (int)sv.z / ratio));
      // A skin voxel whose collider block did not survive majority-fill has no
      // component to join; it is interior detail of a block that reads as air,
      // and dropping it keeps skin and collider describing the same object.
      if (it != compOf.end()) skinParts[it->second].push_back(sv);
    }
  }

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  b.voxels = std::move(parts[keep]);
  if (!skinParts.empty()) b.skinVoxels = std::move(skinParts[keep]);
  bool madeBody = false;

  for (uint32_t c = 0; c < (uint32_t)parts.size(); c++) {
    if (c == keep) continue;
    if (parts[c].size() >= minFragment && budget > 0 &&
        bodies_.size() + fragments.size() < kMaxBodies) {
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
      // A fragment of a micro body is itself a micro body: it inherits the
      // scale so its collider pitch, radius and particle conversion all stay
      // right, and gets its OWN brick below (the parent's brick still shows
      // the whole original shape, so sharing it would draw each half as the
      // intact object). If that brick can't be allocated the fragment cannot
      // be drawn at all — the cube path would render it at scale-1 size,
      // twice too big — so it falls through to particles instead, which is
      // the same handoff every under-floor piece already takes.
      nb.micro = b.micro;
      nb.physScale = b.physScale;  // MUST precede the pitch below
      nb.bleedMat = b.bleedMat;
      // The fragment's skin rebases by the SAME corner, expressed in skin
      // units. Both lattices must land on one origin or the art slides off the
      // collider — the same agreement ReskinMicro maintains after a carve.
      if (!skinParts.empty()) {
        const int ratio = (int)std::max(1u, b.micro.skinScale / b.physScale);
        nb.skinVoxels = std::move(skinParts[c]);
        for (PrefabVoxel& sv : nb.skinVoxels) {
          sv.x = (int16_t)(sv.x - mn.x * ratio);
          sv.y = (int16_t)(sv.y - mn.y * ratio);
          sv.z = (int16_t)(sv.z - mn.z * ratio);
        }
      }
      nb.xf.pos += QuatRot(b.xf.quat, Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
      const float pitch = 1.0f / (float)std::max(1u, nb.physScale);
      nb.handle =
          phys_->CreateDebrisBodyXf(parts[c], nb.xf, densityOf_, false, pitch);
      if (nb.handle != 0) {
        phys_->SetBodyVelocities(nb.handle, lin, ang);
        nb.voxels = std::move(parts[c]);
        float r = 0;
        for (const DebrisVoxel& v : nb.voxels)
          r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
        nb.radiusVoxels = r / (float)std::max(1u, nb.physScale) + 2.0f;
        nb.serial = nextSerial_++;
        if (!nb.micro.Valid() || ReskinMicro(nb)) {
          RecountBurn(nb);
          fragments.push_back(std::move(nb));
          madeBody = true;
          budget--;
          continue;
        }
        // Brick pool full: undo the body and let it become particles below.
        phys_->RemoveBody(nb.handle);
        parts[c] = std::move(nb.voxels);
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
    // rebuild the parent NOW or the ghost boxes fight the new bodies.
    //
    // The pitch is NOT optional here: it was omitted before the skin/collider
    // split, so a micro parent was rebuilt at pitch 1 and its collider snapped
    // to physScale times its real size until the next RebuildCollider. The
    // fragment path two branches up (and RebuildCollider) always passed it.
    const float pitch = 1.0f / (float)std::max(1u, b.physScale);
    uint64_t nh = phys_->CreateDebrisBodyXf(b.voxels, b.xf, densityOf_,
                                            /*allowKinematic=*/false, pitch);
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
  // A micro body's voxels are 1/scale of a world voxel, so local coords must be
  // divided down to reach world space. They are also SMALLER than a particle:
  // emitting one particle per micro voxel would turn a scale-2 body into 8x the
  // matter it had. Sub-sampling on the micro lattice (one particle per world
  // cell) conserves the visible volume — the grid has no sub-voxel resolution
  // to receive the detail anyway.
  const float inv = 1.0f / (float)std::max(1u, b.physScale);
  const int step = (int)std::max(1u, b.physScale);
  for (const DebrisVoxel& v : voxels) {
    if (spawns.size() >= kMaxParticleSpawnsPerTick) return;  // ring full: lost
    if (step > 1 && (((int)v.x % step) || ((int)v.y % step) || ((int)v.z % step)))
      continue;
    Vec3 wp = b.xf.pos + QuatRot(b.xf.quat, Vec3{((float)v.x + 0.5f) * inv,
                                                 ((float)v.y + 0.5f) * inv,
                                                 ((float)v.z + 0.5f) * inv});
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
                             const BodyTransform& xf, MicroBodyRef micro,
                             uint32_t physScale,
                             std::vector<PrefabVoxel> skinVoxels,
                             uint32_t bleedMat) {
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
  // Micro rendering comes in with the body (PLAN §C4): a severed microvoxel
  // limb keeps its detail with no mob-specific code here, because the caller
  // hands over what it already knows. The extents above are in COLLIDER units
  // for such a body, so the radius divides by physScale — the +2 slack does not.
  //
  // physScale defaults to the skin scale, which is the pre-split behaviour and
  // stays exact for every caller whose two lattices coincide. A caller with a
  // genuinely finer skin passes the coarser collider scale explicitly and also
  // hands over `skinVoxels` (see AdoptBodySkin).
  body.micro = micro;
  body.physScale = physScale ? physScale : std::max(1u, micro.skinScale);
  // Only keep the fine lattice when it actually IS finer. A caller that hands
  // over a redundant copy at the collider's own resolution would otherwise pay
  // for a second array that carries no extra information.
  if (micro.skinScale > body.physScale) body.skinVoxels = std::move(skinVoxels);
  body.radiusVoxels = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez) /
                          (float)std::max(1u, body.physScale) +
                      2.0f;
  body.serial = nextSerial_++;
  body.bleedMat = bleedMat;
  RecountBurn(body);
  bodies_.push_back(std::move(body));
  instancesDirty_ = true;
}

void DebrisSystem::ReleaseBody(Body& b) {
  // Free the brick BEFORE the handle so an early return can never strand pool
  // words. Shared models (mob-def art, the cached sphere balls) are ignored by
  // MicroBodyFree — only copy-on-write clones this body owns are reclaimed.
  if (b.micro.Valid() && microSet_) MicroBodyFree(*microSet_, b.micro.model);
  b.micro = MicroBodyRef{};
  // Told BEFORE the handle is destroyed, so a listener may still ask physics
  // about it. Every path that lets go of a body funnels through here, which is
  // what makes one notification enough (see SetOnBodyGone).
  if (b.handle && onBodyGone_) onBodyGone_(b.handle);
  if (b.handle) phys_->RemoveBody(b.handle);
  b.handle = 0;
}

void DebrisSystem::RebaseVoxels(std::vector<DebrisVoxel>& voxels,
                                BodyTransform& xf) {
  if (voxels.empty()) return;
  IVec3 mn{127, 127, 127};
  for (const DebrisVoxel& v : voxels) {
    mn.x = std::min<int>(mn.x, v.x);
    mn.y = std::min<int>(mn.y, v.y);
    mn.z = std::min<int>(mn.z, v.z);
  }
  if (mn.x == 0 && mn.y == 0 && mn.z == 0) return;
  for (DebrisVoxel& v : voxels) {
    v.x = (int8_t)(v.x - mn.x);
    v.y = (int8_t)(v.y - mn.y);
    v.z = (int8_t)(v.z - mn.z);
  }
  // The origin moved to the new min corner, so the transform moves with it or
  // the body teleports by the rebase amount.
  xf.pos += QuatRot(xf.quat, Vec3{(float)mn.x, (float)mn.y, (float)mn.z});
}

bool DebrisSystem::RebuildCollider(Body& b) {
  if (b.voxels.empty()) return false;
  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  // A micro body's voxels are 1/scale world voxels on a side. Building at
  // pitch 1 would inflate a scale-2 body to twice its size and 8x its mass.
  const float pitch = 1.0f / (float)std::max(1u, b.physScale);
  uint64_t nh = phys_->CreateDebrisBodyXf(b.voxels, b.xf, densityOf_, false, pitch);
  if (nh == 0) return false;
  phys_->RemoveBody(b.handle);
  b.handle = nh;
  phys_->SetBodyVelocities(nh, lin, ang);
  b.burnedSinceRebuild = 0;
  // A damaged sphere is no longer a sphere: the analytic collider it spawned
  // with is replaced by the greedy-boxed voxel compound built above, so a
  // carved ball stops rolling like a perfect one. That is the whole reason
  // this goes through CreateDebrisBodyXf rather than patching the old shape.
  return true;
}

// Re-derive the collider lattice from the (authoritative) skin lattice.
//
// This is the ONE direction data flows between the two: skin -> collider,
// never back. Keeping it one-directional is what keeps the split outside the
// hashed domain — the skin is render state, and a collider decision that read
// from it in the other direction would drag render state into physics.
//
// Called after any edit to `skinVoxels`. Cheap relative to the carve that
// preceded it (one pass over the fine lattice, one hash map).
void DebrisSystem::DeriveColliderFromSkin(Body& b) {
  if (!b.HasFineSkin()) return;
  const uint32_t ratio = std::max(1u, b.micro.skinScale / b.physScale);
  bool overflow = false;
  b.voxels = DownsampleSkin(b.skinVoxels, ratio, &overflow);
  if (overflow && !skinOverflowWarned_) {
    skinOverflowWarned_ = true;
    std::printf(
        "debris: skin lattice exceeded the collider's +-127 bound; part of a "
        "body was dropped from its collider (physScale too fine for the art)\n");
  }
}

bool DebrisSystem::ReskinMicro(Body& b) {
  if (!b.micro.Valid() || !microSet_) return false;
  int own = MicroBodyOwn(*microSet_, b.micro.model);
  if (own < 0) return false;  // pool full: keep the stale skin, stay damaged
  b.micro.model = (uint32_t)own;
  // The brick is packed from the SKIN when there is one, and from the collider
  // voxels when the two lattices coincide. This is the only difference between
  // a fine-skinned body and a pre-split one, and it is why carving must edit
  // `skinVoxels` — whatever this reads is what the player sees.
  std::vector<PrefabVoxel> mv;
  const bool fine = b.HasFineSkin();
  if (fine) {
    mv = b.skinVoxels;
  } else {
    mv.reserve(b.voxels.size());
    for (const DebrisVoxel& v : b.voxels)
      mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                    (uint16_t)(v.payload & 0xFFF)});
  }
  IVec3 shift{};
  if (!MicroBodyEdit(*microSet_, b.micro.model, mv, shift)) return false;
  // MicroBodyEdit rebased the brick to ITS OWN min corner. The body's voxels
  // may not be rebased to the same corner yet — ShatterBody re-skins fragments
  // whose coords it already rebased (shift 0), but DamageBody re-skins a
  // survivor whose min corner has just moved inward as voxels were carved off
  // (shift > 0). The brick march runs [0..dims) from the body ORIGIN, so any
  // nonzero shift must move the transform or the art slides off the collider.
  // Doing it here, from what MicroBodyEdit actually chose, is what keeps the
  // two frames agreeing without either side having to assume the other's.
  if (shift.x || shift.y || shift.z) {
    const float inv = 1.0f / (float)std::max(1u, b.micro.skinScale);
    b.xf.pos += QuatRot(b.xf.quat, Vec3{(float)shift.x * inv,
                                        (float)shift.y * inv,
                                        (float)shift.z * inv});
    // Bring the body's own voxels into the brick's frame so collider rebuilds
    // and particle conversion measure from the same origin the art does.
    if (fine) {
      // The shift is in SKIN units, and a skin-unit shift is not generally a
      // whole number of collider voxels: at skin 8 / collider 2 a shift of 3
      // is 0.75 of a collider voxel. So the skin rebases exactly and the
      // collider is RE-DERIVED from it rather than shifted to match. The
      // derived lattice inherits the brick's origin instead of negotiating for
      // it, which is what makes the two frames agree by construction.
      for (PrefabVoxel& sv : b.skinVoxels) {
        sv.x = (int16_t)(sv.x - shift.x);
        sv.y = (int16_t)(sv.y - shift.y);
        sv.z = (int16_t)(sv.z - shift.z);
      }
      DeriveColliderFromSkin(b);
    } else {
      for (DebrisVoxel& v : b.voxels) {
        v.x = (int8_t)(v.x - shift.x);
        v.y = (int8_t)(v.y - shift.y);
        v.z = (int8_t)(v.z - shift.z);
      }
    }
  }
  return true;
}

bool DebrisSystem::DamageBody(size_t bi, World& world,
                              std::vector<ParticleSpawn>& spawns,
                              std::vector<Body>& fragments,
                              uint32_t& newBodyBudget, bool eject,
                              const CarveFactory& carveAt) {
  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);

  const bool fine = b.HasFineSkin();
  // The collider predicate always exists; the skin one only when the skin is a
  // separate lattice. Both describe the SAME world-space volume.
  const auto keep = carveAt((float)std::max(1u, b.physScale));

  std::vector<DebrisVoxel> removed;
  for (const DebrisVoxel& v : b.voxels)
    if (!keep((float)v.x, (float)v.y, (float)v.z)) removed.push_back(v);
  if (removed.empty()) return true;  // nothing in range

  Vec3 lin{}, ang{};
  phys_->GetBodyVelocities(b.handle, lin, ang);
  if (eject) VoxelsToParticles(b, removed, lin, ang, world, spawns);

  // Corpse bleeding: a body that was once flesh emits blood when carved.
  if (b.bleedMat != 0 && !removed.empty()) {
    const float ps = (float)std::max(1u, b.physScale);
    Vec3 centroid{};
    for (const DebrisVoxel& v : removed)
      centroid += Vec3{(float)v.x + 0.5f, (float)v.y + 0.5f, (float)v.z + 0.5f};
    centroid = centroid * (1.0f / (float)removed.size());
    Vec3 woundW = b.xf.pos + QuatRot(b.xf.quat, centroid * (1.0f / ps));

    const uint32_t nRemoved = (uint32_t)removed.size();
    // Micro spray: 2 droplets per removed voxel, capped.
    const uint32_t sprayN = std::min(nRemoved * 2u, 60u);
    for (uint32_t k = 0; k < sprayN; k++) {
      if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
      uint32_t h = rng::Hash3(b.serial * 0x9E3779B9u, (uint32_t)k, nRemoved);
      Vec3 dir{rng::SignedUnit(h) * 0.7f,
               0.4f + 0.6f * std::fabs(rng::SignedUnit(rng::Pcg(h ^ 0x51u))),
               rng::SignedUnit(rng::Pcg(h ^ 0xB0u)) * 0.7f};
      float sp = 3.5f * (0.6f + 0.8f * rng::Unit01(rng::Pcg(h ^ 0x1234u)));
      ParticleSpawn s{};
      s.px = (int32_t)std::lround(woundW.x * 256.0f);
      s.py = (int32_t)std::lround(woundW.y * 256.0f);
      s.pz = (int32_t)std::lround(woundW.z * 256.0f);
      s.vx = (int32_t)std::lround(dir.x * sp * 256.0f / 30.0f);
      s.vy = (int32_t)std::lround(dir.y * sp * 256.0f / 30.0f);
      s.vz = (int32_t)std::lround(dir.z * sp * 256.0f / 30.0f);
      s.payload = b.bleedMat & 0xFFFu;
      s.flags = kPFlagAlive | kPFlagMicro | ParticleMicroBits(4, 70);
      spawns.push_back(s);
    }
    // Whole-voxel blood: 1 per 4 removed voxels, capped. These pool and persist.
    const uint32_t bloodN = std::min((nRemoved + 3u) / 4u, 8u);
    for (uint32_t k = 0; k < bloodN; k++) {
      if (spawns.size() >= kMaxParticleSpawnsPerTick) break;
      uint32_t h = rng::Hash3(b.serial * 0x51A17u, (uint32_t)k, nRemoved ^ 0xB100Du);
      Vec3 dir{rng::SignedUnit(h) * 0.5f,
               0.3f + 0.5f * std::fabs(rng::SignedUnit(rng::Pcg(h ^ 0x77u))),
               rng::SignedUnit(rng::Pcg(h ^ 0xC0FFu)) * 0.5f};
      float sp = 2.5f * (0.5f + rng::Unit01(rng::Pcg(h ^ 0x9Eu)));
      ParticleSpawn s{};
      s.px = (int32_t)std::lround(woundW.x * 256.0f);
      s.py = (int32_t)std::lround(woundW.y * 256.0f);
      s.pz = (int32_t)std::lround(woundW.z * 256.0f);
      s.vx = (int32_t)std::lround(dir.x * sp * 256.0f / 30.0f);
      s.vy = (int32_t)std::lround(dir.y * sp * 256.0f / 30.0f);
      s.vz = (int32_t)std::lround(dir.z * sp * 256.0f / 30.0f);
      s.payload = b.bleedMat & 0xFFFu;
      s.flags = kPFlagAlive;
      spawns.push_back(s);
    }
  }

  if (fine) {
    // The SKIN is authoritative: carve it at its own resolution, then re-derive
    // the collider from what survived. Carving the two independently would let
    // them disagree about the shape; deriving one from the other cannot.
    const auto keepSkin = carveAt((float)std::max(1u, b.micro.skinScale));
    b.skinVoxels.erase(
        std::remove_if(b.skinVoxels.begin(), b.skinVoxels.end(),
                       [&](const PrefabVoxel& v) {
                         return !keepSkin((float)v.x, (float)v.y, (float)v.z);
                       }),
        b.skinVoxels.end());
    DeriveColliderFromSkin(b);
  } else {
    b.voxels.erase(
        std::remove_if(b.voxels.begin(), b.voxels.end(),
                       [&](const DebrisVoxel& v) {
                         return !keep((float)v.x, (float)v.y, (float)v.z);
                       }),
        b.voxels.end());
  }
  instancesDirty_ = true;

  // Wholly destroyed, or blown under the body-worthiness floor: the remainder
  // rejoins the world as loose voxels, exactly like the burn dissolve path.
  if (b.voxels.size() < kMinBodyVoxels) {
    VoxelsToParticles(b, b.voxels, lin, ang, world, spawns);
    ReleaseBody(b);
    bodies_[bi] = std::move(bodies_.back());
    bodies_.pop_back();
    return false;
  }

  // Blowing a body in half must yield two bodies, not one body with a hole:
  // the connectivity split is what turns "removed voxels" into "separated
  // pieces". Fresh blast damage uses the ISLAND floor (8), not the much higher
  // burn-fragment floor — a body blown apart by an explosion is a one-off
  // event, not the every-few-ticks re-fragmentation that forced burn's bar up.
  ShatterBody(b, world, fragments, spawns, kMinBodyVoxels, newBodyBudget);

  // Rebase AFTER shatter (which rebases fragments itself) so the surviving
  // body's coords stay tight in int8 range, then rebuild the collider.
  //
  // Micro bodies rebase through ReskinMicro INSTEAD, not as well: RebaseVoxels
  // shifts the transform in world-voxel units, but a micro body's coords are
  // 1/scale of one, so using it here would move a scale-2 body twice as far as
  // its voxels actually moved. ReskinMicro divides by the scale and takes the
  // corner MicroBodyEdit really chose, so art, collider and voxels stay in one
  // frame. Exactly one of these two runs for any body.
  if (b.micro.Valid())
    ReskinMicro(b);
  else
    RebaseVoxels(b.voxels, b.xf);
  RebuildCollider(b);
  float r = 0;
  for (const DebrisVoxel& v : b.voxels)
    r = std::max(r, Vec3{(float)v.x, (float)v.y, (float)v.z}.len());
  b.radiusVoxels = r / (float)std::max(1u, b.physScale) + 2.0f;
  b.burnCursor = b.voxels.empty() ? 0 : b.burnCursor % (uint32_t)b.voxels.size();
  RecountBurn(b);
  return true;
}

void DebrisSystem::DamageBodiesRadial(Vec3 centerVoxel, float radiusVoxels,
                                      World& world,
                                      std::vector<ParticleSpawn>& spawns) {
  if (!phys_ || radiusVoxels <= 0.0f) return;
  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  const float r2 = radiusVoxels * radiusVoxels;

  for (size_t bi = 0; bi < bodies_.size();) {
    Body& b = bodies_[bi];
    phys_->GetTransform(b.handle, b.xf);
    // cheap reject: blast sphere vs body bounding sphere
    if ((b.xf.pos - centerVoxel).len() > radiusVoxels + b.radiusVoxels + 2.0f) {
      bi++;
      continue;
    }
    // Blast centre into body-local units. The predicate is built PER LATTICE:
    // the same world-space sphere, expressed at whatever resolution the lattice
    // it is testing lives in. That is what lets one carve edit a fine skin and
    // a coarse collider without either one having to know about the other.
    const float q[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2], b.xf.quat[3]};
    const Vec3 cBody = QuatRot(q, centerVoxel - b.xf.pos);
    // Deterministic-enough jitter so the crater rim is ragged rather than a
    // billiard-ball scoop. This is CPU gameplay state (bodies are outside the
    // hashed domain), so a float hash is fine here — rule 1 governs the grid.
    const uint32_t seed = b.serial;
    // Jitter is keyed on the SKIN lattice for both tests, so the crater's rim
    // pattern is a property of the art rather than of whatever collider
    // resolution the engine happened to pick. Without this the same explosion
    // would rag differently depending on physScale.
    const uint32_t jScale = std::max(1u, b.micro.skinScale);

    auto carve = [&](float lat) {
      const float sc = lat;
      const Vec3 cLocal = cBody * sc;
      const float rLocal2 = r2 * sc * sc;
      const float jMul = (float)jScale / sc;  // lattice coord -> skin coord
      return [=](float vx, float vy, float vz) {
        Vec3 c{vx + 0.5f, vy + 0.5f, vz + 0.5f};
        Vec3 dv = c - cLocal;
        float d2 = dv.dot(dv);
        if (d2 >= rLocal2) return true;  // outside the blast
        // Falloff: certain removal in the core, thinning toward the rim.
        float t = std::sqrt(d2 / rLocal2);  // 0 centre .. 1 rim
        float chance = 1.0f - t * t;        // quadratic, wide core
        int jx = (int)(vx * jMul), jy = (int)(vy * jMul), jz = (int)(vz * jMul);
        uint32_t h = Hash3(seed, (uint32_t)jx * 73856093u,
                           (uint32_t)jy * 19349663u ^ (uint32_t)jz * 83492791u);
        return (float)(h & 0xFFFFu) / 65535.0f >= chance;
      };
    };

    bool alive = DamageBody(bi, world, spawns, fragments, budget, true, carve);
    if (alive) bi++;
  }
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
}

bool DebrisSystem::MeltBodyAt(uint64_t handle, Vec3 pointVoxel,
                              float radiusVoxels, World& world,
                              std::vector<ParticleSpawn>& spawns) {
  if (!phys_) return false;
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;

  Body& b = bodies_[bi];
  phys_->GetTransform(b.handle, b.xf);
  const float q[4] = {-b.xf.quat[0], -b.xf.quat[1], -b.xf.quat[2], b.xf.quat[3]};
  const Vec3 pBody = QuatRot(q, pointVoxel - b.xf.pos);
  const float r2 = radiusVoxels * radiusVoxels;

  std::vector<Body> fragments;
  uint32_t budget = kMaxNewBodiesPerTick;
  // Same world-space sphere, re-expressed per lattice — see DamageBodiesRadial.
  // A clean bore, so no jitter and nothing keyed on a lattice at all.
  auto carve = [&](float lat) {
    const Vec3 pLocal = pBody * lat;
    const float rLocal2 = r2 * lat * lat;
    return [=](float vx, float vy, float vz) {
      Vec3 c{vx + 0.5f, vy + 0.5f, vz + 0.5f};
      Vec3 dv = c - pLocal;
      return dv.dot(dv) >= rLocal2;
    };
  };
  // eject=false: the beam vaporizes. A held laser damages every tick, and
  // spraying particles from each one would drain the spawn ring in a second.
  // The return says whether the body survived; either way the hit landed, and
  // fragments still have to be adopted below.
  DamageBody(bi, world, spawns, fragments, budget, false, carve);
  for (Body& f : fragments) {
    bodies_.push_back(std::move(f));
    instancesDirty_ = true;
  }
  return true;
}

bool DebrisSystem::MicroDirty() const {
  return microSet_ && microSet_->dirty;
}

bool DebrisSystem::SplitBody(uint64_t handle, Vec3 planePointVoxel,
                             Vec3 planeNormal) {
  size_t bi = 0;
  for (; bi < bodies_.size(); bi++)
    if (bodies_[bi].handle == handle) break;
  if (bi == bodies_.size()) return false;
  Body& b = bodies_[bi];
  // Micro bodies are not cuttable in v1: both halves would need their own
  // brick, and the pool holds per-DEF models shared across instances (see the
  // BurnBodies note). The laser passes through instead of silently producing
  // two wrongly-scaled cube bodies.
  if (b.micro.Valid()) return false;
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
    newBodies[h].bleedMat = b.bleedMat;
    RecountBurn(newBodies[h]);
    phys_->SetBodyVelocities(newBodies[h].handle, lin, ang);
  }

  ReleaseBody(b);
  bodies_[bi] = std::move(newBodies[0]);
  bodies_.push_back(std::move(newBodies[1]));
  instancesDirty_ = true;
  return true;
}

bool DebrisSystem::DestroyBody(uint64_t handle) {
  if (!handle) return false;
  for (size_t i = 0; i < bodies_.size(); i++) {
    if (bodies_[i].handle != handle) continue;
    ReleaseBody(bodies_[i]);
    bodies_.erase(bodies_.begin() + i);
    instancesDirty_ = true;
    return true;
  }
  return false;
}

bool DebrisSystem::BodyLatticeOf(uint64_t handle, std::vector<PrefabVoxel>& out,
                                 uint32_t& outScale) const {
  for (const Body& b : bodies_) {
    if (b.handle != handle) continue;
    out.clear();
    if (b.HasFineSkin()) {
      outScale = b.micro.skinScale;
      out = b.skinVoxels;
    } else {
      outScale = b.physScale ? b.physScale : 1u;
      out.reserve(b.voxels.size());
      for (const DebrisVoxel& v : b.voxels)
        out.push_back(PrefabVoxel{(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                                  v.payload, v.color});
    }
    return true;
  }
  return false;
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
    //
    // Sample occupancy into an 18^3 bitmask up front, one source chunk at a
    // time: the 5832 samples touch at most 27 chunks, so the chunk-cache hash
    // lookup happens 27 times instead of once per sample (and the polygonizer
    // then reads bits, not a std::function).
    IVec3 origin{wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk};
    uint32_t occ[kMcOccWords] = {};
    for (int ncz = -1; ncz <= 1; ncz++)
      for (int ncy = -1; ncy <= 1; ncy++)
        for (int ncx = -1; ncx <= 1; ncx++) {
          IVec3 nwc{wc.x + ncx, wc.y + ncy, wc.z + ncz};
          // the sub-box of this neighbor chunk that lands inside the 18^3 box,
          // in occ coords (chunk-local + 1)
          int blo[3], bhi[3];
          const int nc[3] = {ncx, ncy, ncz};
          for (int a = 0; a < 3; a++) {
            blo[a] = nc[a] < 0 ? 0 : (nc[a] == 0 ? 1 : kMcOccDim - 1);
            bhi[a] = nc[a] < 0 ? 0 : (nc[a] == 0 ? kMcOccDim - 2 : kMcOccDim - 1);
          }
          bool inWin = world.ChunkInWindow(nwc);
          const CachedChunk* n = inWin ? world.Cached(nwc) : nullptr;
          bool haveVox = n && n->voxels.size() == kChunkVol;
          // Outside the residency window is solid and inert (matches the sim
          // rule); in-window but not yet fetched reads as empty until it lands.
          if (!inWin) {
            for (int z = blo[2]; z <= bhi[2]; z++)
              for (int y = blo[1]; y <= bhi[1]; y++)
                for (int x = blo[0]; x <= bhi[0]; x++) McOccSet(occ, x, y, z);
            continue;
          }
          if (!haveVox) continue;
          for (int z = blo[2]; z <= bhi[2]; z++)
            for (int y = blo[1]; y <= bhi[1]; y++)
              for (int x = blo[0]; x <= bhi[0]; x++) {
                // occ coord -> world cell -> local cell in THIS neighbor
                int lx = (origin.x + x - 1) & 15, ly = (origin.y + y - 1) & 15,
                    lz = (origin.z + z - 1) & 15;
                uint32_t mat =
                    n->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFF;
                if (mat == 0 || mat >= classOf_.size()) continue;
                if (classOf_[mat] == CLASS_SOLID || classOf_[mat] == CLASS_POWDER)
                  McOccSet(occ, x, y, z);
              }
        }
    std::vector<float> verts;
    std::vector<uint32_t> indices;
    PolygonizeChunk(origin, occ, verts, indices);

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
    settle_.terrainWakes++;
    settle_.lastWakeTick = tick;
    settle_.lastWakeChunk = wc;
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

// The piece's MOST COMMON material, not the first voxel's: an island is
// usually one substance, but a burnt stem carries a few ash voxels and a wall
// a few of whatever hit it, and picking voxel 0 would let that minority decide
// what the break or the impact sounds like. Counted over a small map rather
// than a full histogram because the voxel count is bounded by the body cap.
uint32_t DebrisSystem::DominantMaterial(const std::vector<DebrisVoxel>& voxels) {
  std::unordered_map<uint32_t, uint32_t> tally;
  for (const DebrisVoxel& v : voxels) tally[v.payload & 0xFFFu]++;
  uint32_t domMat = 0, domCount = 0;
  for (const auto& [mat, count] : tally) {
    // Ties break toward the lower id for stability of the REPORT: the world
    // hash never sees this, but an unstable pick would make the cue flip
    // between runs and make a bug here hard to reproduce.
    if (count > domCount || (count == domCount && mat < domMat)) {
      domMat = mat;
      domCount = count;
    }
  }
  return domMat;
}

// Turn this step's Jolt contacts into audible impacts (DESIGN.md §12b).
//
// WHAT IS ALREADY FILTERED OUT BEFORE WE GET HERE. Physics' contact listener
// only reports NEW manifolds (a landing, never a rest), only above the speed
// gate, and never anything touching the player proxy or a Layers::AVATAR limb.
// So a settled pile of debris and a walking player both cost zero here — which
// is the CLAUDE.md rule 2 property this hook has to have.
//
// A LIVE MOB'S LIMB IS NOT DEBRIS, and gets dropped for free: limb bodies are
// owned by MobSystem, so the handle lookup below misses and the contact is
// discarded. A SEVERED limb has been AdoptBody'd into bodies_ by then, so it
// does start thudding — which is exactly right.
void DebrisSystem::CollectImpacts() {
  const std::vector<Physics::ContactImpact>& raw = phys_->ContactImpacts();
  if (raw.empty()) return;
  const Tuning::Audio& ta = CurrentTuning().audio;
  // Voxels/sec. The gate the listener already applied is the same number; this
  // is the top of the ramp, and it must stay above the gate or every impact
  // reports full energy.
  const float fullVox = std::max(ta.impactFullSpeed, ta.impactMinSpeed + 0.1f) /
                        kVoxelMeters;
  const float minVox = ta.impactMinSpeed / kVoxelMeters;
  const uint32_t gapSteps =
      (uint32_t)std::max(1.0f, ta.impactMinGap * kSimTicksPerSecond);

  // grid material at a world cell via the chunk cache, the same read the body
  // burn uses. Chunks around live bodies are kept fetched by the terrain
  // meshing, so a body landing on terrain almost always resolves; a miss reads
  // as air and falls through to the body material below.
  auto worldMatAt = [&](Vec3 p) -> uint32_t {
    IVec3 c{ifloor(p.x), ifloor(p.y), ifloor(p.z)};
    if (!world_->CellInWindow(c)) return 0u;
    const CachedChunk* cc = world_->Cached(ChunkOfCell(c.x, c.y, c.z));
    if (!cc || cc->voxels.size() != kChunkVol) return 0u;
    uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15),
             lz = (uint32_t)(c.z & 15);
    return cc->voxels[(lz * kChunk + ly) * kChunk + lx] & 0xFFFu;
  };
  auto bodyIndexOf = [&](uint64_t h) -> size_t {
    if (h == 0) return (size_t)-1;
    for (size_t i = 0; i < bodies_.size(); i++)
      if (bodies_[i].handle == h) return i;
    return (size_t)-1;
  };

  // Candidates for this step, before the per-step cap.
  std::vector<ImpactEvent> cand;
  for (const Physics::ContactImpact& ci : raw) {
    const size_t ia = bodyIndexOf(ci.bodyA), ib = bodyIndexOf(ci.bodyB);
    if (ia == (size_t)-1 && ib == (size_t)-1) continue;  // neither is debris

    // Per-body gap. Charged to every debris body in the contact, so a rock
    // bouncing down a slope is one thud per bounce group rather than one per
    // manifold, and two bodies clattering together do not each report it.
    bool fresh = false;
    for (size_t i : {ia, ib}) {
      if (i == (size_t)-1) continue;
      if (stepCount_ - bodies_[i].lastImpactStep < gapSteps &&
          bodies_[i].lastImpactStep != 0)
        continue;
      fresh = true;
    }
    if (!fresh) continue;
    for (size_t i : {ia, ib})
      if (i != (size_t)-1) bodies_[i].lastImpactStep = stepCount_;

    // WHAT WAS STRUCK decides the sound (sound_schema.js: "debris striking
    // THIS material"), so probe the grid on both sides of the contact plane
    // before falling back to the bodies. Two probes, because the contact point
    // sits exactly on the surface and which side of the cell boundary it lands
    // on is a rounding accident.
    uint32_t mat = worldMatAt(ci.posVoxel + ci.normal * 0.6f);
    if (mat == 0) mat = worldMatAt(ci.posVoxel - ci.normal * 0.6f);
    if (mat == 0) mat = worldMatAt(ci.posVoxel);
    // Body-vs-body, or terrain we could not read: the OTHER body's material,
    // else our own. A limb hitting a boulder should sound like the boulder.
    if (mat == 0 && ib != (size_t)-1 && ia != (size_t)-1)
      mat = bodies_[ib].domMat;
    if (mat == 0) mat = bodies_[ia != (size_t)-1 ? ia : ib].domMat;
    if (mat == 0) continue;

    const float k = std::clamp((ci.speedVoxPerSec - minVox) / (fullVox - minVox),
                               0.0f, 1.0f);
    cand.push_back(ImpactEvent{ci.posVoxel, mat, k});
  }
  if (cand.empty()) return;

  // Loudest first, then take the cap. partial_sort rather than sort: the tail
  // is discarded, so ordering it is work nobody reads.
  const size_t keep = std::min(cand.size(), kMaxImpactsPerStep);
  std::partial_sort(cand.begin(), cand.begin() + keep, cand.end(),
                    [](const ImpactEvent& a, const ImpactEvent& b) {
                      return a.energy > b.energy;
                    });
  impacts_.insert(impacts_.end(), cand.begin(), cand.begin() + keep);
}

void DebrisSystem::PostStep() {
  stepCount_++;
  // The gate the contact listener applies, refreshed from tuning every step so
  // an F5 reload takes effect. Set here (game thread, between Updates) rather
  // than inside Step: the Jolt job threads must never read CurrentTuning().
  phys_->SetContactReportSpeed(CurrentTuning().audio.impactMinSpeed /
                               kVoxelMeters);
  CollectImpacts();
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
      ReleaseBody(b);
      bodies_[i] = std::move(bodies_.back());
      bodies_.pop_back();
      instancesDirty_ = true;
    } else {
      i++;
    }
  }
  // body budget: oldest bodies despawn first (they are usually settled rubble)
  while (bodies_.size() > kMaxBodies) {
    ReleaseBody(bodies_.front());
    bodies_.erase(bodies_.begin());
    instancesDirty_ = true;
  }
}

void DebrisSystem::BuildInstances(std::vector<BodyVoxInst>& out) {
  out.clear();
  for (size_t bi = 0; bi < bodies_.size() && bi < kMaxBodies; bi++) {
    // Micro bodies draw through the OBB/brick-march pass instead. They still
    // OWN their slot (the two passes share bodyXforms), they just contribute
    // no cube instances — emitting both would double-draw at the wrong size.
    if (bodies_[bi].micro.Valid()) continue;
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

void DebrisSystem::AppendMicroInsts(std::vector<MicroBodyInstGpu>& out) const {
  for (size_t i = 0; i < bodies_.size() && i < kMaxBodies; i++)
    if (bodies_[i].micro.Valid())
      out.push_back({(uint32_t)i, bodies_[i].micro.model, 0, 0});
}

uint32_t DebrisSystem::ActiveBodyCount() const {
  uint32_t n = 0;
  for (const Body& b : bodies_)
    if (phys_->IsActive(b.handle)) n++;
  return n;
}

bool DebrisSystem::BodyActive(uint32_t i) const {
  return i < bodies_.size() && phys_->IsActive(bodies_[i].handle);
}

// ---- persistence (entities.sve section 'DBRS') ------------------------------

void DebrisSystem::SaveState(std::vector<uint8_t>& out) const {
  ByteWriter w{out};
  w.U32((uint32_t)bodies_.size());
  for (const Body& b : bodies_) {
    w.Pod(b.xf);
    w.U32(b.physScale);
    w.U32(b.micro.skinScale);
    // Only "did it render as micro" travels; the model index is meaningless
    // across sessions (the pool is rebuilt), so load re-packs a brick from the
    // lattice below.
    w.U32(b.micro.Valid() ? 1u : 0u);
    w.U32(b.bleedMat);
    w.PodVec(b.voxels);
    w.PodVec(b.skinVoxels);
  }
}

bool DebrisSystem::LoadState(const uint8_t* data, size_t len, uint32_t version) {
  if (version != kSaveVersion) {
    std::fprintf(stderr, "debris: unknown DBRS section version %u\n", version);
    return false;
  }
  ByteReader r{data, len};
  uint32_t count = 0;
  r.U32(count);
  for (uint32_t i = 0; i < count && r.ok; i++) {
    BodyTransform xf{};
    uint32_t physScale = 1, skinScale = 1, hadMicro = 0, bleedMat = 0;
    std::vector<DebrisVoxel> voxels;
    std::vector<PrefabVoxel> skinVoxels;
    r.Pod(xf);
    r.U32(physScale);
    r.U32(skinScale);
    r.U32(hadMicro);
    r.U32(bleedMat);
    r.PodVec(voxels);
    r.PodVec(skinVoxels);
    if (!r.ok || voxels.empty()) continue;
    if (bodies_.size() >= kMaxBodies) break;  // same ceiling spawning obeys

    physScale = std::max(1u, physScale);
    const float pitch = 1.0f / (float)physScale;
    uint64_t h = phys_->CreateDebrisBodyXf(voxels, xf, densityOf_, false, pitch);
    if (h == 0) {
      std::fprintf(stderr, "debris: Jolt refused a loaded body (skipped)\n");
      continue;
    }

    // Re-pack the micro brick from the authoritative lattice — the same
    // skin-first rule ReskinMicro applies — and mark it OWNED so ReleaseBody
    // returns its words to the pool like any copy-on-write clone. Variant bits
    // are stripped: the brick stores plain 8-bit material ids.
    MicroBodyRef micro{};
    if (hadMicro && microSet_) {
      std::vector<PrefabVoxel> mv;
      const bool fine = skinScale > physScale && !skinVoxels.empty();
      const std::vector<PrefabVoxel>* src = fine ? &skinVoxels : nullptr;
      if (src) {
        mv.reserve(src->size());
        for (const PrefabVoxel& v : *src)
          mv.push_back({v.x, v.y, v.z, (uint16_t)(v.material & 0xFFu)});
      } else {
        mv.reserve(voxels.size());
        for (const DebrisVoxel& v : voxels)
          mv.push_back({(int16_t)v.x, (int16_t)v.y, (int16_t)v.z,
                        (uint16_t)(v.payload & 0xFFu)});
      }
      IVec3 mx{0, 0, 0};
      for (const PrefabVoxel& v : mv) {
        mx.x = std::max<int>(mx.x, v.x);
        mx.y = std::max<int>(mx.y, v.y);
        mx.z = std::max<int>(mx.z, v.z);
      }
      std::string plog;
      int mi = MicroBodyPack(*microSet_, mv, {mx.x + 1, mx.y + 1, mx.z + 1},
                             skinScale, "load", plog);
      if (mi >= 0) {
        microSet_->owned[mi] = 1;  // freeable: this body is the sole holder
        micro = MicroBodyRef{(uint32_t)mi, skinScale};
      } else if (!plog.empty()) {
        std::fprintf(stderr, "%s", plog.c_str());
      }
    }

    AdoptBody(h, std::move(voxels), xf, micro, physScale, std::move(skinVoxels),
              bleedMat);
    // Reload ASLEEP with zero velocity (worldio.h's rigidbody rule): a settled
    // pile reloads settled, and rule 2's sleep invariant holds from tick one.
    phys_->DeactivateBody(h);
  }
  return r.ok;
}
