// =============================================================================
// distant-obstacles -- trees, buildings and ruins are obstacles at distance
// (docs/PLAN_distant_npcs.md §8.2 / §8.5, package D10; game/distant_obstacles.h)
// =============================================================================
//
// On the GAME's map (Tirnhallow: Harrowby, the spawn wood, the ruins), loaded
// through the real ReloadEnvironment exactly as `village-harrowby` loads it:
//
//   A. AGREEMENT WITH THE GPU. BuildVoxRegion -- the --voxdump machinery: the
//      real genChunk dispatch, patched with the edit layer as the game patches
//      every chunk -- generates boxes over each of Harrowby's houses, the spawn
//      wood, the first surface rect of each placed dungeon and a cactus-biome
//      patch. Every cell from 8 under to 40 over its column's ground is
//      compared: the near tier's "carries weight" of the GPU's word against
//      DistantGround::Solid. Per SOURCE (GenOverlay::Source: which source
//      decided the overlay's answer) match / mismatch, the first mismatching
//      cells named. Door cells are counted apart (air BY POLICY: the GPU's leaf
//      is solid). A mismatch the BASE answered is attributed: a tree cell
//      above the obstacle band (a whole-tree twin, the band lifted, decides it
//      and agrees with the GPU), else the GPU word's material by name -- so a
//      bare count never stands alone (CLAUDE.md rule 6). The whole-tree twin's
//      own agreement is the measure of the tree port. Floors in
//      tests/baseline.json (distantObst*).
//   B. THE WALKS. (1) A distant human on a straight Goto ACROSS Harrowby, on
//      the line through the locale that meets the most overlay matter while
//      every obstruction along it is within the planner's reach (chosen from
//      the overlay's own answers, before any walk): it reaches the far side
//      and never ends a tick with its body's column cells (feet to head)
//      inside matter. (2) One aimed straight through the tree trunk nearest
//      the spawn: it walks ROUND it (never inside, reaches the goal, its
//      nearest pass of the trunk axis > 0) -- and the same walk with the
//      overlay OFF walks through it (the control: the obstacle is what made
//      the difference).
//   C. THE DOOR. A villager outside the smithy's door with a goal inside the
//      room walks in through the door box, never inside matter; with the
//      overlay's doors closed (Options::doors false) it does not get in.
//   D. COST. The overlay's per-column build time (mean, max), its dungeon
//      cells, and a walk on the `peaks` fixture (the map distant-walk's W arm
//      uses) with the overlay off and on, cold caches both: TickDistant's own
//      wall time per tick. Measurements, never inputs.
//   E. TWICE. The trunk and door walks run twice from one saved state: the
//      per-tick traces are equal. The overlay's answers over every cell A
//      compared, re-asked in reverse order after dropping every cache, are
//      equal (a cache hit and a miss give the same answer).
//
// DIRECT PHASE CALLS ON PURPOSE (W2-O): B, C and D drive MobSystem::TickDistant
// alone -- the walks are over ground no window holds (the window is parked far
// off), which is the point of the tier; distant-walk's W arm is the pattern.
//
// LEAVES NOTHING: the harness map reloaded and the window regenerated at home
// (village-harrowby's exit), mobs / debris reset, store cleared, tuning and
// the id counter put back, ground caches dropped.

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "game/distant.h"
#include "game/distant_obstacles.h"
#include "game/mob.h"
#include "sim/biomes.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "sim/worldgen_twin.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "tools/voxregion.h"

using namespace sandvox;

namespace selftest {
namespace {

struct Digest {
  uint64_t h = 1469598103934665603ull;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  void F(float v) { Bytes(&v, sizeof v); }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
};

inline int Floor16(int v) { return (v >= 0 ? v / 16 : -((-v + 15) / 16)) * 16; }

// The 'SVVX' RLE back to a flat x-fastest grid (selftest_voxregion's walk).
bool DecodeBox(const std::vector<uint8_t>& b, uint32_t n, std::vector<uint32_t>& out) {
  auto rd = [&](size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
  };
  if (b.size() < kVoxRegionHeaderBytes || rd(0) != kVoxRegionMagic) return false;
  const uint32_t runs = rd(48);
  if (b.size() != kVoxRegionHeaderBytes + (size_t)runs * 8) return false;
  out.assign(n, 0);
  size_t o = 0;
  for (uint32_t i = 0; i < runs; i++) {
    const uint32_t w = rd(kVoxRegionHeaderBytes + (size_t)i * 8);
    const uint32_t k = rd(kVoxRegionHeaderBytes + (size_t)i * 8 + 4);
    for (uint32_t j = 0; j < k && o < n; j++) out[o++] = w;
  }
  return o == n;
}

struct SrcTally {
  uint64_t n = 0, ok = 0;
  std::vector<std::string> first;   // the first mismatching cells, named
};

}  // namespace

Status GateDistantObstacles(Ctx& c, std::string& detail) {
  IdCounterScope idScope(c.mobs);
  const Tuning tuneWas = CurrentTuning();
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const std::string prevMap = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
  std::vector<std::string> fails;
  auto check = [&](bool ok, const std::string& why) {
    if (!ok) fails.push_back(why);
    return ok;
  };
  std::string laps;
  auto lapT0 = std::chrono::steady_clock::now();
  auto lap = [&](const char* arm) {
    const auto now = std::chrono::steady_clock::now();
    laps += Format("%s%s %lld", laps.empty() ? "" : " ", arm,
                   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - lapT0).count());
    lapT0 = now;
  };
  DistantGround& ground = c.mobs.DistantGroundMut();
  auto restore = [&]() {
    c.mobs.Reset();
    c.debris.Reset();
    c.mobs.SetParkFn(nullptr);
    c.mobs.ClearPlayerActors();
    c.stream.Store().Clear();
    worldmap::SetMapOverride(prevMap);
    std::string l2;
    ReloadEnvironment(c.ctx, c.sim, c.mats, l2);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    ground.SetOverlayEnabled(true);
    ground.Overlay().SetOptions(GenOverlay::Options{});
    ground.InvalidateAll();
    SetCurrentTuning(tuneWas);
  };

  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetItems(&c.items);
  c.mobs.SetParkFn(nullptr);
  {
    Tuning t = CurrentTuning();
    t.distant.enabled = true;
    t.distant.radius = 1280.0f;
    t.distant.hysteresis = 256.0f;
    t.distant.idleStride = 4;
    t.distant.surfaceSnapMaxVox = 24.0f;
    t.distant.promoteMaxDropVox = 48.0f;
    SetCurrentTuning(t);
  }
  c.mobs.SetDistantStream(&c.stream);
  const int human = c.mobs.FindDef("human");
  if (human < 0) {
    restore();
    detail = "no 'human' def";
    return Status::Fail;
  }
  const Vec3 hs = c.mobs.Defs()[(size_t)human].worldSize;

  // ---- the walkers, made at home (the window's own ground) and demoted ----------
  const IVec3 home = c.world.WindowOrigin();
  const int baseX = home.x * (int)kChunk + (int)kWorldN / 2, baseZ = home.z * (int)kChunk + (int)kWorldN / 2;
  auto make = [&](const char* prof) -> uint64_t {
    const uint64_t id = c.mobs.Spawn(human, {baseX, World::TerrainHeight(baseX, baseZ, kDefaultSeed) + 1, baseZ});
    if (id == 0) return 0;
    c.mobs.SetMobBehavior(id, prof);
    return c.mobs.DemoteForTest(id) ? id : 0;
  };
  const uint64_t wLine = make("duelist"), wTrunk = make("duelist"), wDoor = make("villager"),
                 wCost = make("duelist");
  DistantMobs& tier = c.mobs.DistantMut();
  if (wLine == 0 || wTrunk == 0 || wDoor == 0 || wCost == 0) {
    restore();
    detail = "a walker did not demote";
    return Status::Fail;
  }
  // Out of everyone's way (a distant creature with no goal sleeps): the
  // walkers wait far apart and each walk moves only its own.
  auto park = [&](uint64_t id, int k) {
    if (const DistantMob* d = tier.Find(id))
      tier.Relocate(id, Vec3{-90000.0f - 3000.0f * (float)k, d->origin.y, -90000.0f});
  };
  park(wLine, 0);
  park(wTrunk, 1);
  park(wDoor, 2);
  park(wCost, 3);
  lap("setup");

  // ---- the game's map --------------------------------------------------------------
  worldmap::SetMapOverride("default");
  {
    std::string log;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, log)) {
      restore();
      detail = "the default map did not load: " + log;
      return Status::Fail;
    }
  }
  ground.SetOverlayEnabled(true);
  ground.Overlay().SetOptions(GenOverlay::Options{});
  ground.InvalidateAll();
  const worldmap::WorldMapData& map = worldmap::CurrentWorldMap();
  // What carries weight: MobSystem::Init's collision classes, the same rule
  // (passable vegetation reads as gas), from the same table the tier's
  // ground was bound with.
  std::vector<uint32_t> cls;
  cls.reserve(c.mats.size());
  for (const MaterialDef& m : c.mats)
    cls.push_back((m.gpu.flags & kMatFlagPassable) ? (uint32_t)CLASS_GAS : m.gpu.klass);
  std::vector<MaterialGpu> gpuMats;
  gpuMats.reserve(c.mats.size());
  for (const MaterialDef& m : c.mats) gpuMats.push_back(m.gpu);
  auto matName = [&](uint32_t m) -> std::string {
    m &= 0xFFFu;
    if (m == 0) return "air";
    return m < c.mats.size() ? c.mats[m].name : Format("#%u", m);
  };
  lap("env");

  // ======== A: agreement with the GPU ================================================
  struct Box {
    std::string what;
    int ox = 0, oy = 0, oz = 0;
  };
  std::vector<Box> boxes;
  auto addBox = [&](const std::string& what, int cx, int cz, int gy) {
    Box b;
    b.what = what;
    b.ox = Floor16(cx - 64);
    b.oz = Floor16(cz - 64);
    b.oy = Floor16(gy - 24);
    boxes.push_back(b);
  };
  for (const worldmap::WorldMapData::StampSite& s : map.sites)
    if (s.structure && s.kind == worldmap::kSiteStamp) addBox("house " + s.id, s.x, s.z, s.padY);
  addBox("spawn wood", map.spawnX, map.spawnZ, World::TerrainHeight(map.spawnX, map.spawnZ, kDefaultSeed));
  int nDungeonBoxes = 0;
  for (const worldmap::DungeonDef& g : map.dungeons) {
    if (!g.Placed() || g.surf.empty() || nDungeonBoxes >= 2) continue;
    const auto& r = g.surf.front();
    const int cx = (r[0] + r[2]) / 2, cz = (r[1] + r[3]) / 2;
    addBox("ruin " + g.id, cx, cz, World::TerrainHeight(cx, cz, kDefaultSeed));
    nDungeonBoxes++;
  }
  {
    // A cactus biome: the painted biome cell nearest the spawn whose biome
    // grows cacti, and whose 3 x 3 cells all do (so the warp cannot move it).
    const wgtwin::GenEnv& env = wgtwin::CurrentGenEnv();
    long long best = LLONG_MAX;
    int bx = 0, bz = 0;
    if (env.map)
      for (int k = 1; k + 1 < map.biomePostsZ; k++)
        for (int i = 1; i + 1 < map.biomePostsX; i++) {
          bool all = true;
          for (int dk = -1; dk <= 1 && all; dk++)
            for (int di = -1; di <= 1 && all; di++)
              all = (wgtwin::BiomeWord(*env.map, map.BiomePost(i + di, k + dk), worldmap::kB_Flags) &
                     worldmap::kBF_Cacti) != 0u;
          if (!all) continue;
          const int x = map.extentX0 + (i << map.biomeLog2) + (1 << (map.biomeLog2 - 1));
          const int z = map.extentZ0 + (k << map.biomeLog2) + (1 << (map.biomeLog2 - 1));
          const long long d = (long long)(x - map.spawnX) * (x - map.spawnX) + (long long)(z - map.spawnZ) * (z - map.spawnZ);
          if (d < best) {
            best = d;
            bx = x;
            bz = z;
          }
        }
    if (best != LLONG_MAX) addBox("cactus biome", bx, bz, World::TerrainHeight(bx, bz, kDefaultSeed));
  }

  GenOverlay& ov = ground.Overlay();
  GenOverlay whole(&ground);   // the whole-tree twin: the band lifted past any tree, doors as the GPU has them
  {
    GenOverlay::Options o;
    o.treeBand = 1 << 20;
    o.doors = false;
    whole.SetOptions(o);
    whole.Bind(&cls, &gpuMats, kDefaultSeed);
  }
  SrcTally tally[GenOverlay::kSrcCount];
  SrcTally wholeTree;
  uint64_t doorCells = 0, doorGpuSolid = 0, crownCells = 0, integration = 0, cellsAll = 0;
  std::map<std::string, uint64_t> baseBuckets;
  std::vector<IVec3> asked;   // every compared cell (E re-asks them)
  std::vector<uint8_t> answers;
  std::string boxWhy;
  for (const Box& b : boxes) {
    VoxRegionReq req;
    req.ox = b.ox;
    req.oy = b.oy;
    req.oz = b.oz;
    req.nx = req.nz = 128;
    req.ny = 64;
    req.lod = 1;
    req.seed = kDefaultSeed;
    std::vector<uint8_t> blob;
    std::string err;
    std::vector<uint32_t> grid;
    if (!BuildVoxRegion(c.ctx, c.world, c.sim, req, blob, err) ||
        !DecodeBox(blob, req.nx * req.ny * req.nz, grid)) {
      if (boxWhy.empty()) boxWhy = b.what + ": " + (err.empty() ? std::string("decode") : err);
      continue;
    }
    for (int lz = 0; lz < (int)req.nz; lz++)
      for (int lx = 0; lx < (int)req.nx; lx++) {
        const int x = b.ox + lx, z = b.oz + lz;
        const int h = ground.TerrainTop(x, z);
        const int y0 = std::max(b.oy, h - 8), y1 = std::min(b.oy + (int)req.ny - 1, h + 40);
        for (int y = y0; y <= y1; y++) {
          const uint32_t w = grid[((size_t)lz * req.ny + (size_t)(y - b.oy)) * req.nx + (size_t)lx];
          const bool gpu = WordCarriesWeight(w, cls);
          bool sol = false;
          GenOverlay::Source src = GenOverlay::kSrcBase;
          if (!ov.Cell(x, y, z, sol, &src)) {
            sol = y <= h;
            src = GenOverlay::kSrcBase;
          }
          if (ground.Solid(x, y, z) != sol) integration++;
          // Every 7th cell is kept for E's cold re-ask (all of them would be
          // ~90 MB on a box set this size).
          if (cellsAll % 7 == 0) {
            asked.push_back(IVec3{x, y, z});
            answers.push_back(sol ? 1 : 0);
          }
          cellsAll++;
          // The whole-tree twin, once per cell: its tree cells are the tree
          // port's measure, and it attributes a base mismatch to a crown.
          bool ws = false;
          GenOverlay::Source wsrc = GenOverlay::kSrcBase;
          const bool wholeTreeCell = whole.Cell(x, y, z, ws, &wsrc) &&
                                     (wsrc == GenOverlay::kSrcTree || wsrc == GenOverlay::kSrcSiteTree);
          if (wholeTreeCell) {
            wholeTree.n++;
            if (ws == gpu) wholeTree.ok++;
            else if (wholeTree.first.size() < 4)
              wholeTree.first.push_back(Format("(%d,%d,%d) gpu %s", x, y, z, matName(w).c_str()));
          }
          // BY POLICY, counted apart and in no rate: a door leaf (air here,
          // the GPU's leaf is solid) and a tree cell above the obstacle band
          // (base here; the whole-tree twin agrees with the GPU).
          if (src == GenOverlay::kSrcDoor) {
            doorCells++;
            doorGpuSolid += gpu ? 1 : 0;
            continue;
          }
          if (src == GenOverlay::kSrcBase && gpu != sol && wholeTreeCell && ws == gpu) {
            crownCells++;
            continue;
          }
          SrcTally& t = tally[src];
          t.n++;
          if (gpu == sol) {
            t.ok++;
          } else {
            if (t.first.size() < 4)
              t.first.push_back(Format("(%d,%d,%d) gpu %s %s, h %d", x, y, z, matName(w).c_str(),
                                       gpu ? "carries" : "open", h));
            if (src == GenOverlay::kSrcBase)
              baseBuckets[(gpu ? "gpu carries: " : "gpu open: ") + matName(w)]++;
          }
        }
      }
  }
  auto rate = [](const SrcTally& t) { return t.n == 0 ? 1.0 : (double)t.ok / (double)t.n; };
  std::string aLine;
  for (int s = 0; s < GenOverlay::kSrcCount; s++) {
    const SrcTally& t = tally[s];
    if (t.n == 0) continue;
    aLine += Format("%s%s %llu/%llu", aLine.empty() ? "" : ", ", GenOverlay::SourceName((GenOverlay::Source)s),
                    (unsigned long long)t.ok, (unsigned long long)t.n);
    RecordObserved(Format("distantObst.match.%s", GenOverlay::SourceName((GenOverlay::Source)s)).c_str(), rate(t));
    RecordObserved(Format("distantObst.cells.%s", GenOverlay::SourceName((GenOverlay::Source)s)).c_str(), (double)t.n);
  }
  RecordObserved("distantObst.match.wholeTree", rate(wholeTree));
  RecordObserved("distantObst.cells.wholeTree", (double)wholeTree.n);
  RecordObserved("distantObst.doorCells", (double)doorCells);
  RecordObserved("distantObst.crownCells", (double)crownCells);
  RecordObserved("distantObst.boxes", (double)boxes.size());
  std::string firstBad;
  for (int s = 0; s < GenOverlay::kSrcCount; s++)
    for (const std::string& f : tally[s].first)
      if (firstBad.size() < 900)
        firstBad += Format("%s%s: %s", firstBad.empty() ? "" : "; ", GenOverlay::SourceName((GenOverlay::Source)s), f.c_str());
  std::string buckets;
  {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (const auto& kv : baseBuckets) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 6; i++)
      buckets += Format("%s%s x%llu", buckets.empty() ? "" : ", ", v[i].second.c_str(), (unsigned long long)v[i].first);
  }
  std::printf("distant-obstacles: A %zu boxes, %llu cells: %s | whole trees %llu/%llu | by policy: doors %llu "
              "cells (gpu solid %llu), crowns above the band %llu | base mismatches by cause: %s\n",
              boxes.size(), (unsigned long long)cellsAll, aLine.c_str(), (unsigned long long)wholeTree.ok,
              (unsigned long long)wholeTree.n, (unsigned long long)doorCells, (unsigned long long)doorGpuSolid,
              (unsigned long long)crownCells, buckets.empty() ? "none" : buckets.c_str());
  if (!firstBad.empty()) std::printf("distant-obstacles: A first mismatches: %s\n", firstBad.c_str());
  if (!wholeTree.first.empty()) {
    std::string w;
    for (const std::string& f : wholeTree.first) w += (w.empty() ? "" : "; ") + f;
    std::printf("distant-obstacles: A whole-tree first mismatches: %s\n", w.c_str());
  }
  {
    const struct {
      GenOverlay::Source s;
      const char* minKey;
      const char* cellsKey;
      double minDef, cellsDef;
    } floors[] = {
        {GenOverlay::kSrcStamp, "distantObstStampMatchMin", "distantObstStampCellsMin", 1.0, 1000},
        {GenOverlay::kSrcLayer, "distantObstLayerMatchMin", "distantObstLayerCellsMin", 1.0, 100},
        {GenOverlay::kSrcTree, "distantObstTreeMatchMin", "distantObstTreeCellsMin", 0.98, 1000},
        {GenOverlay::kSrcSiteTree, "distantObstSiteTreeMatchMin", "distantObstSiteTreeCellsMin", 0.98, 0},
        {GenOverlay::kSrcCactus, "distantObstCactusMatchMin", "distantObstCactusCellsMin", 0.98, 100},
        {GenOverlay::kSrcDungeon, "distantObstDungeonMatchMin", "distantObstDungeonCellsMin", 0.98, 100},
        {GenOverlay::kSrcBase, "distantObstBaseMatchMin", "distantObstBaseCellsMin", 0.99, 10000},
    };
    for (const auto& f : floors) {
      const SrcTally& t = tally[f.s];
      const double want = BaselineNumber(f.minKey, f.minDef), cells = BaselineNumber(f.cellsKey, f.cellsDef);
      check((double)t.n >= cells && rate(t) >= want,
            Format("A: %s %llu/%llu (%.4f, want >= %.4f over >= %.0f cells)",
                   GenOverlay::SourceName(f.s), (unsigned long long)t.ok, (unsigned long long)t.n, rate(t),
                   want, cells));
    }
    check(rate(wholeTree) >= BaselineNumber("distantObstWholeTreeMatchMin", 0.98),
          Format("A: whole trees %llu/%llu", (unsigned long long)wholeTree.ok, (unsigned long long)wholeTree.n));
    check(doorCells >= (uint64_t)BaselineNumber("distantObstDoorCellsMin", 50),
          Format("A: only %llu door cells compared", (unsigned long long)doorCells));
    check(integration == 0, Format("A: DistantGround::Solid disagreed with the overlay at %llu cells",
                                   (unsigned long long)integration));
    check(boxWhy.empty(), "A: a box failed: " + boxWhy);
  }
  lap("A");

  // The walks run where no window is: park it far off (BuildVoxRegion left it
  // on the last box). Nothing reads the GPU until the exit regenerates.
  c.world.SetWindowOrigin(IVec3{savedOrigin.x + 4096, savedOrigin.y, savedOrigin.z + 4096});

  // ---- the walk runner ------------------------------------------------------------------
  GenOverlay probe(&ground);   // "is this cell matter" for the checks, on whatever the walk's ground does
  probe.Bind(&cls, &gpuMats, kDefaultSeed);
  auto matter = [&](int x, int y, int z) {
    bool s = false;
    if (probe.Cell(x, y, z, s)) return s;
    return y <= ground.TerrainTop(x, z);
  };
  // The standing height a little over the ground: the floor inside a house
  // (not its roof), the ground or a root outside.
  auto standAt = [&](int x, int z) {
    int y = ground.TerrainTop(x, z) + 1;
    ground.ProbeGround(x, z, ground.TerrainTop(x, z) + 6, y);
    return y;
  };
  // Is the column's standing room (feet to head) clear? A start or a goal
  // inside a trunk or a wall is moved along its line until it is.
  const int headCells = std::max((int)std::ceil(hs.y), 1);
  auto clearCol = [&](int x, int z) {
    const int f = standAt(x, z);
    for (int y = f; y < f + headCells; y++)
      if (matter(x, y, z)) return false;
    return true;
  };
  auto freeAlong = [&](int& x, int& z, int dx, int dz) {
    for (int k = 0; k < 24 && !clearCol(x, z); k++) {
      x += dx;
      z += dz;
    }
  };
  auto place = [&](uint64_t id, int x, int z) {
    tier.Relocate(id, Vec3{(float)x + 0.5f - hs.x * 0.5f, (float)standAt(x, z), (float)z + 0.5f - hs.z * 0.5f});
  };
  auto gotoGoal = [&](uint64_t id, Vec3 goal) {
    ai::Brain* br = c.mobs.BrainAnyTierMut(id);
    if (br == nullptr) return;
    br->routine = ai::Routine{};
    br->routine.active = true;
    br->routine.verb = ai::Intent::Goto;
    br->routine.goal = goal;
    br->routine.final = true;
    br->routine.arriveRadius = 4.0f;
    br->routine.speed = 1.0f;
    // Awake: it may have fallen asleep parked while another walk ticked.
    if (DistantMob* d = tier.Find(id)) {
      d->asleep = false;
      d->wakeTick = 0;
      tier.MarkAwakeHint();
    }
  };
  struct Walk {
    bool reached = false;
    int ticks = 0, inside = 0, maxDepth = 0;
    float walked = 0, minAxis = 1e9f, endDist = 0;
    bool crossed = false;
    Vec3 endPos{};
    uint64_t digest = 0;
    std::string why;
  };
  uint32_t tick = 50000;
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): the file header says why.
  auto walk = [&](uint64_t id, Vec3 goal, int maxTicks, Vec3 axis, const GenOverlay::DoorBox* door) {
    Walk r;
    Digest dg;
    const uint32_t t0 = tick;
    bool havePrev = false;
    Vec3 prev{};
    for (int k = 0; k < maxTicks; k++) {
      c.mobs.TickDistant(t0 + (uint32_t)k, c.world, nullptr);
      const DistantMob* d = tier.Find(id);
      if (d == nullptr) break;
      r.ticks = k + 1;
      dg.F(d->origin.x);
      dg.F(d->origin.y);
      dg.F(d->origin.z);
      dg.F(d->steer.heading);
      const Vec3 cc = d->Centre();
      const int gx = ifloor(cc.x), gz = ifloor(cc.z), fy = ifloor(d->origin.y + 1e-3f);
      int depth = 0;
      for (int y = fy; y < fy + std::max(d->headroomCells, 1); y++)
        if (matter(gx, y, gz)) depth++;
      if (depth > 0) {
        r.inside++;
        r.maxDepth = std::max(r.maxDepth, depth);
        if (r.why.empty())
          r.why = Format("tick %d: %d of its column cells in matter at (%d,%d,%d)", k, depth, gx, fy, gz);
      }
      r.minAxis = std::min(r.minAxis, std::hypot(cc.x - axis.x, cc.z - axis.z));
      // THROUGH THE LEAF: a stride moves several voxels between samples, so
      // the one-cell leaf is crossed when the step from the last sample to
      // this one passes its plane inside the leaf's span (+-1), not only when
      // a sample lands on it.
      if (door != nullptr && havePrev) {
        const bool thinX = door->hi.x - door->lo.x <= door->hi.z - door->lo.z;
        const float plane = thinX ? (float)door->lo.x + 0.5f : (float)door->lo.z + 0.5f;
        const float a = thinX ? prev.x : prev.z, b = thinX ? cc.x : cc.z;
        if ((a - plane) * (b - plane) <= 0.0f && a != b) {
          const float u = (plane - a) / (b - a);
          const float o = thinX ? prev.z + (cc.z - prev.z) * u : prev.x + (cc.x - prev.x) * u;
          const float lo = thinX ? (float)door->lo.z : (float)door->lo.x;
          const float hi = thinX ? (float)door->hi.z + 1.0f : (float)door->hi.x + 1.0f;
          if (o >= lo - 1.0f && o <= hi + 1.0f) r.crossed = true;
        }
      }
      havePrev = true;
      prev = cc;
      r.endPos = cc;
      r.walked = d->walked;
      r.endDist = std::hypot(goal.x - cc.x, goal.z - cc.z);
      if (d->brain.routine.arrived || r.endDist <= 6.0f) {
        r.reached = true;
        break;
      }
    }
    tick = t0 + (uint32_t)maxTicks + 16;
    r.digest = dg.h;
    return r;
  };
  // A walker's whole state, saved and put back (Erase + Insert files it afresh).
  auto save = [&](uint64_t id) {
    const DistantMob* d = tier.Find(id);
    return d != nullptr ? *d : DistantMob{};
  };
  auto restoreMob = [&](const DistantMob& s) {
    tier.Erase(s.id);
    DistantMob copy = s;
    tier.Insert(std::move(copy));
    tier.MarkAwakeHint();
  };

  // ======== B1: across Harrowby ==============================================================
  // THE LINE, chosen from the overlay's own answers before any walk: every
  // west-east and south-north line across the locale (8-voxel spacing) is
  // sampled at the standing level; a column is OBSTRUCTED when the overlay
  // puts matter in its feet-to-head cells. The pick is the line with the most
  // obstructed columns whose longest obstructed run is under the planner's
  // reach (navRadius 30: 2 x 24, room to see round it). Deterministic.
  Walk b1;
  int b1Obstructed = 0, b1Run = 0;
  std::string b1Line;
  {
    const worldmap::LocaleDef* hb = nullptr;
    for (const worldmap::LocaleDef& l : map.locales)
      if (l.id == "harrowby") hb = &l;
    int x0 = 330, z0 = 3090, x1 = 790, z1 = 3650;
    if (hb != nullptr) {
      x0 = hb->x0;
      z0 = hb->z0;
      x1 = hb->x1;
      z1 = hb->z1;
    }
    const int head = std::max((int)std::ceil(hs.y), 1);
    auto obstructed = [&](int x, int z) {
      const int f = ground.TerrainTop(x, z) + 1;
      for (int y = f; y < f + head; y++)
        if (matter(x, y, z)) return true;
      return false;
    };
    // THE HOUSE IN THE WAY. A first version took the most obstructed line
    // across the whole locale; it ran into the middle of the village's big
    // walled compound, whose way round is a route problem for a global
    // planner, not an obstacle test (the walker never stood in matter, it just
    // never found the far gate). The claim here is the one the plan makes:
    // a HOUSE between a walker and its goal is walked ROUND, never through.
    // So: the largest stamp in the locale no wider than the planner's local
    // reach (48 voxels each way), crossed through its middle on its longer
    // axis, from 24 voxels before its footprint to 24 past it. Deterministic:
    // the map's site order breaks ties.
    int bestN = -1;
    Vec3 from{}, to{};
    long long bestArea = -1;
    for (const auto& st : map.sites) {
      if (st.kind != worldmap::kSiteStamp) continue;
      const int w = st.x1 - st.bx0 + 1, d = st.z1 - st.bz0 + 1;
      if (w < 8 || d < 8 || w > 48 || d > 48) continue;
      if (st.bx0 < x0 || st.x1 > x1 || st.bz0 < z0 || st.z1 > z1) continue;
      const long long area = (long long)w * d;
      if (area <= bestArea) continue;
      // The crossing: along x through the middle when the house is wider in
      // x, else along z. Count its obstructed columns (the walker must meet
      // matter, or the arm proves nothing).
      const bool alongX = w >= d;
      const int cxm = (st.bx0 + st.x1) / 2, czm = (st.bz0 + st.z1) / 2;
      int n = 0, run = 0, worst = 0;
      const int a0 = alongX ? st.bx0 - 24 : st.bz0 - 24, a1 = alongX ? st.x1 + 24 : st.z1 + 24;
      for (int a = a0; a <= a1; a++) {
        const bool o = alongX ? obstructed(a, czm) : obstructed(cxm, a);
        run = o ? run + 1 : 0;
        worst = std::max(worst, run);
        n += o ? 1 : 0;
      }
      if (n == 0) continue;
      bestArea = area;
      bestN = n;
      b1Run = worst;
      from = alongX ? Vec3{(float)a0 + 0.5f, 0, (float)czm + 0.5f} : Vec3{(float)cxm + 0.5f, 0, (float)a0 + 0.5f};
      to = alongX ? Vec3{(float)a1 + 0.5f, 0, (float)czm + 0.5f} : Vec3{(float)cxm + 0.5f, 0, (float)a1 + 0.5f};
      b1Line = Format("round a house %dx%d at (%d,%d) along %s", w, d, cxm, czm, alongX ? "x" : "z");
    }
    b1Obstructed = std::max(bestN, 0);
    if (bestN > 0) {
      int fx = ifloor(from.x), fz = ifloor(from.z), tx = ifloor(to.x), tz = ifloor(to.z);
      const int ux = tx > fx ? 1 : (tx < fx ? -1 : 0), uz = tz > fz ? 1 : (tz < fz ? -1 : 0);
      freeAlong(fx, fz, -ux, -uz);   // outward, away from the village
      freeAlong(tx, tz, ux, uz);
      to = Vec3{(float)tx + 0.5f, 0, (float)tz + 0.5f};
      place(wLine, fx, fz);
      gotoGoal(wLine, to);
      b1 = walk(wLine, to, (int)BaselineNumber("distantObstLineMaxTicks", 2400), Vec3{-1e9f, 0, -1e9f}, nullptr);
      // WHERE IT STOPPED, drawn: the obstruction map round the end (#
      // obstructed, . open, @ the walker, G the goal side), so a stall names
      // its wall instead of a distance.
      if (!b1.reached) {
        const int ex = ifloor(b1.endPos.x), ez = ifloor(b1.endPos.z);
        std::printf("distant-obstacles: B1 stalled at (%d,%d); map x %d..%d (right = +x), z %d..%d (down = +z)\n",
                    ex, ez, ex - 48, ex + 48, ez - 24, ez + 24);
        for (int z = ez - 24; z <= ez + 24; z += 2) {
          std::string row;
          for (int x = ex - 48; x <= ex + 48; x++)
            row += (x == ex && (z == ez || z + 1 == ez)) ? '@' : (obstructed(x, z) || obstructed(x, z + 1)) ? '#' : '.';
          std::printf("  %s\n", row.c_str());
        }
        // ...and the ground along its row: TerrainTop, the probed standing
        // height from 6 over it, and the overlay's matter in the feet band,
        // plus the planner's own state -- a wall, a terrace edge and a route
        // the planner believes in are different stalls.
        std::string hs2;
        for (int x = ex - 8; x <= ex + 12; x++) {
          int st = 0;
          ground.ProbeGround(x, ez, ground.TerrainTop(x, ez) + 6, st);
          hs2 += Format(" %d:%d/%d%s", x, ground.TerrainTop(x, ez), st, obstructed(x, ez) ? "#" : "");
        }
        std::printf("distant-obstacles: B1 row z %d (x:top/stand):%s\n", ez, hs2.c_str());
        // The whole locale at 4 voxels a character (any obstructed column in
        // the 4x4 = '#'), the line's row marked '='.
        std::printf("distant-obstacles: B1 locale x %d..%d z %d..%d, 4 vox/char, '@' the walker, '=' the line\n",
                    x0, x1, z0, z1);
        for (int z = z0; z <= z1; z += 4) {
          std::string row;
          for (int x = x0; x <= x1; x += 4) {
            bool o = false;
            for (int k = 0; k < 16 && !o; k++) o = obstructed(x + (k & 3), z + (k >> 2));
            const bool here = ex >= x && ex < x + 4 && ez >= z && ez < z + 4;
            const bool line = ez >= z && ez < z + 4;
            row += here ? '@' : o ? '#' : line ? '=' : '.';
          }
          std::printf("  %s\n", row.c_str());
        }
        if (const DistantMob* d = tier.Find(wLine))
          std::printf("distant-obstacles: B1 brain: replans %u longPlans %u navFailed %d path %d/%d "
                      "stepUp %d stepDown %d headroom %d feet %.1f\n",
                      d->brain.replans, d->brain.longPlans, d->brain.navFailed ? 1 : 0,
                      (int)d->brain.path.cursor, (int)d->brain.path.pts.size(), d->stepUpCells,
                      d->stepDownCells, d->headroomCells, (double)d->origin.y);
      }
      park(wLine, 0);
    }
  }
  check(b1Obstructed > 0 && b1.reached && b1.inside == 0,
        Format("B1: line %s (%d obstructed columns, worst run %d): reached %d in %d ticks (%.0f from the "
               "goal at (%.0f,%.0f,%.0f), walked %.0f), %d ticks in matter%s%s",
               b1Line.c_str(), b1Obstructed, b1Run, b1.reached, b1.ticks, (double)b1.endDist,
               (double)b1.endPos.x, (double)b1.endPos.y, (double)b1.endPos.z, (double)b1.walked, b1.inside, b1.why.empty() ? "" : ": ", b1.why.c_str()));
  lap("B1");

  // ======== B2: through a trunk ================================================================
  // The trunk nearest the spawn: a column the overlay gives to a lattice tree
  // from 2 to 20 voxels over its ground (a stem, not a skirt or a bough).
  Walk b2[2], b2off;
  IVec3 trunk{INT_MIN, 0, 0};
  {
    long long best = LLONG_MAX;
    for (int dz = -96; dz <= 96; dz++)
      for (int dx = -96; dx <= 96; dx++) {
        const int x = map.spawnX + dx, z = map.spawnZ + dz;
        const long long d2 = (long long)dx * dx + (long long)dz * dz;
        if (d2 >= best) continue;
        const int h = ground.TerrainTop(x, z);
        bool ok = true;
        for (int y = h + 2; y <= h + 20 && ok; y += 6) {
          bool s = false;
          GenOverlay::Source src = GenOverlay::kSrcBase;
          ok = ov.Cell(x, y, z, s, &src) && s && src == GenOverlay::kSrcTree;
        }
        if (ok) {
          best = d2;
          trunk = IVec3{x, h, z};
        }
      }
  }
  if (trunk.x != INT_MIN) {
    const Vec3 axis{(float)trunk.x + 0.5f, 0, (float)trunk.z + 0.5f};
    int sx = trunk.x, sz = trunk.z - 40, gx = trunk.x, gz = trunk.z + 40;
    freeAlong(sx, sz, 0, -1);
    freeAlong(gx, gz, 0, 1);
    const Vec3 goal{(float)gx + 0.5f, 0, (float)gz + 0.5f};
    place(wTrunk, sx, sz);
    gotoGoal(wTrunk, goal);
    const DistantMob start = save(wTrunk);
    const uint32_t tStart = tick;
    for (int run = 0; run < 2; run++) {
      restoreMob(start);
      tick = tStart;
      b2[run] = walk(wTrunk, goal, 400, axis, nullptr);
    }
    // The control: the same walk on the D1 ground (no overlay).
    restoreMob(start);
    tick = tStart;
    ground.SetOverlayEnabled(false);
    b2off = walk(wTrunk, goal, 400, axis, nullptr);
    ground.SetOverlayEnabled(true);
    park(wTrunk, 1);
  }
  check(trunk.x != INT_MIN && b2[0].reached && b2[0].inside == 0 && b2[0].minAxis > 0.5f,
        Format("B2: trunk at (%d,%d,%d): reached %d in %d ticks, %d ticks in matter, nearest pass of the axis "
               "%.1f%s%s",
               trunk.x, trunk.y, trunk.z, b2[0].reached, b2[0].ticks, b2[0].inside, (double)b2[0].minAxis,
               b2[0].why.empty() ? "" : ": ", b2[0].why.c_str()));
  check(trunk.x == INT_MIN || b2off.inside > 0,
        Format("B2 control: with the overlay OFF it never entered the trunk (%d ticks in matter, nearest %.1f)",
               b2off.inside, (double)b2off.minAxis));
  lap("B2");

  // ======== C: the door ===========================================================================
  // THE HOUSE MUST BE SHUT BUT FOR ITS DOOR, or the arm proves nothing: an
  // open-sided building (the smithy's forge front) lets a walker in with
  // every leaf shut. So the doors are tried in the map's order and the FIRST
  // whose shut control keeps the walker out is the fixture (deterministic);
  // the ones skipped are named in the detail.
  Walk cw[2], cShut;
  const GenOverlay::DoorBox* door = nullptr;
  GenOverlay::DoorBox doorCopy;
  std::string cWhere, cSkipped;
  const size_t nDoors = std::min<size_t>(ov.Doors().size(), 8);
  for (size_t di = 0; di < nDoors; di++) {
    door = &ov.Doors()[di];
    doorCopy = *door;
    // The leaf is one cell thick on the axis it swings across: OUTSIDE is the
    // side with open sky over its standing height (no roof within 6 m).
    const bool thinX = doorCopy.hi.x - doorCopy.lo.x <= doorCopy.hi.z - doorCopy.lo.z;
    const int cx = (doorCopy.lo.x + doorCopy.hi.x) / 2, cz = (doorCopy.lo.z + doorCopy.hi.z) / 2;
    auto roofed = [&](int x, int z) {
      const int f = standAt(x, z);
      for (int y = f + 24; y < f + 60; y++)
        if (matter(x, y, z)) return true;
      return false;
    };
    const int ax = thinX ? 1 : 0, az = thinX ? 0 : 1;
    int sgn = roofed(cx + ax * 14, cz + az * 14) ? -1 : 1;   // +sgn = outside
    const IVec3 out{cx + ax * 20 * sgn, 0, cz + az * 20 * sgn};
    const IVec3 in{cx - ax * 14 * sgn, 0, cz - az * 14 * sgn};
    const Vec3 goal{(float)in.x + 0.5f, 0, (float)in.z + 0.5f};
    cWhere = Format("%s: leaf (%d,%d,%d)..(%d,%d,%d), outside (%d,%d) roofed %d, inside (%d,%d) roofed %d",
                    doorCopy.id.c_str(), doorCopy.lo.x, doorCopy.lo.y, doorCopy.lo.z, doorCopy.hi.x, doorCopy.hi.y,
                    doorCopy.hi.z, out.x, out.z, roofed(out.x, out.z), in.x, in.z, roofed(in.x, in.z));
    place(wDoor, out.x, out.z);
    gotoGoal(wDoor, goal);
    const DistantMob start = save(wDoor);
    const uint32_t tStart = tick;
    for (int run = 0; run < 2; run++) {
      restoreMob(start);
      tick = tStart;
      cw[run] = walk(wDoor, goal, 600, Vec3{-1e9f, 0, -1e9f}, &doorCopy);
    }
    // Closed doors: the same walk with the leaves read as the GPU has them.
    restoreMob(start);
    tick = tStart;
    {
      GenOverlay::Options o;
      o.doors = false;
      ov.SetOptions(o);
      probe.SetOptions(o);
      cShut = walk(wDoor, goal, 600, Vec3{-1e9f, 0, -1e9f}, &doorCopy);
      ov.SetOptions(GenOverlay::Options{});
      probe.SetOptions(GenOverlay::Options{});
    }
    park(wDoor, 2);
    if (!cShut.reached) break;
    cSkipped += (cSkipped.empty() ? "" : ", ") + doorCopy.id;
    if (di + 1 == nDoors) break;   // none shut: report the last one
  }
  if (!cSkipped.empty()) cWhere += " (open-sided, skipped: " + cSkipped + ")";
  check(door != nullptr && cw[0].reached && cw[0].crossed && cw[0].inside == 0,
        Format("C: %s: reached %d in %d ticks (%.0f from the goal), through the door %d, %d ticks in matter%s%s",
               door ? cWhere.c_str() : "no door on the map", cw[0].reached, cw[0].ticks, (double)cw[0].endDist,
               cw[0].crossed, cw[0].inside, cw[0].why.empty() ? "" : ": ", cw[0].why.c_str()));
  check(door == nullptr || !cShut.reached,
        Format("C control: with the doors shut it still got in (%d ticks)", cShut.ticks));
  lap("C");

  // ======== E: twice ================================================================================
  const bool twiceWalks = b2[0].digest == b2[1].digest && cw[0].digest == cw[1].digest;
  check(twiceWalks, Format("E: walk digests differ (trunk %016llx / %016llx, door %016llx / %016llx)",
                           (unsigned long long)b2[0].digest, (unsigned long long)b2[1].digest,
                           (unsigned long long)cw[0].digest, (unsigned long long)cw[1].digest));
  uint64_t reasked = 0, redif = 0;
  {
    ground.InvalidateAll();
    for (size_t i = asked.size(); i-- > 0;) {
      const IVec3 q = asked[i];
      reasked++;
      if ((ground.Solid(q.x, q.y, q.z) ? 1 : 0) != answers[i]) redif++;
    }
  }
  check(redif == 0, Format("E: %llu of %llu cells answered differently from cold caches in reverse order",
                           (unsigned long long)redif, (unsigned long long)reasked));
  lap("E");

  // ======== D: cost ===================================================================================
  const GenOverlay::Stats os = ov.GetStats();
  const double colUs = os.columns ? (double)os.columnNs / (double)os.columns / 1000.0 : 0.0;
  const double colMaxUs = (double)os.columnNsMax / 1000.0;
  const double dgUs = os.dungeonCells ? (double)os.dungeonNs / (double)os.dungeonCells / 1000.0 : 0.0;
  double usOff = 0, usOn = 0;
  float walkedOff = 0, walkedOn = 0;
  bool peaks = false;
  {
    const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY: the default map
    biomes::BiomeSet set;
    std::string log;
    worldmap::WorldMapData pk;
    if (biomes::LoadBiomeSet(AssetDir(), c.mats, set, log) &&
        worldmap::LoadWorldMap(AssetDir(), "peaks", set, c.mats.size(), kDefaultSeed, pk, log)) {
      peaks = true;
      worldmap::SetCurrentWorldMap(pk);
      const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
      place(wCost, m.spawnX, m.spawnZ);
      const Vec3 goal{(float)m.spawnX + 0.5f, 0, (float)m.spawnZ + 600.5f};
      gotoGoal(wCost, goal);
      const DistantMob start = save(wCost);
      const uint32_t tStart = tick;
      const int kTicks = (int)BaselineNumber("distantObstCostTicks", 300);
      for (int arm = 0; arm < 2; arm++) {
        restoreMob(start);
        tick = tStart;
        ground.SetOverlayEnabled(arm == 1);
        ground.InvalidateAll();   // cold caches, both arms
        const DistantMobs::Stats s0 = tier.GetStats();
        const Walk w = walk(wCost, goal, kTicks, Vec3{-1e9f, 0, -1e9f}, nullptr);
        const DistantMobs::Stats s1 = tier.GetStats();
        const double us = w.ticks ? (double)(s1.tickNs - s0.tickNs) / (double)w.ticks / 1000.0 : 0.0;
        (arm == 0 ? usOff : usOn) = us;
        (arm == 0 ? walkedOff : walkedOn) = w.walked;
      }
      ground.SetOverlayEnabled(true);
      worldmap::SetCurrentWorldMap(real);
      park(wCost, 3);
    }
  }
  const GenOverlay::Stats os2 = ov.GetStats();
  RecordObserved("distantObst.columnUs", colUs);
  RecordObserved("distantObst.columnUsMax", colMaxUs);
  RecordObserved("distantObst.columns", (double)os.columns);
  RecordObserved("distantObst.dungeonCellUs", dgUs);
  RecordObserved("distantObst.peaksTickUsOff", usOff);
  RecordObserved("distantObst.peaksTickUsOn", usOn);
  RecordObserved("distantObst.peaksWalkedOff", (double)walkedOff);
  RecordObserved("distantObst.peaksWalkedOn", (double)walkedOn);
  std::printf("distant-obstacles: D columns %llu built in %.1f us mean / %.0f us max (%llu with a feature, %llu "
              "asks, %llu tree tiles, %llu cactus tiles), %llu dungeon cells at %.1f us, %llu door boxes | peaks "
              "walk %s: %.1f us/tick off (walked %.0f), %.1f us/tick on (walked %.0f, %llu columns)\n",
              (unsigned long long)os.columns, colUs, colMaxUs, (unsigned long long)os.featureColumns,
              (unsigned long long)os.asks, (unsigned long long)os.treeTiles, (unsigned long long)os.cactusTiles,
              (unsigned long long)os.dungeonCells, dgUs, (unsigned long long)os.doors, peaks ? "ran" : "SKIPPED",
              usOff, (double)walkedOff, usOn, (double)walkedOn, (unsigned long long)os2.columns);
  check(peaks, "D: the peaks map did not load");
  lap("D");

  RecordObserved("distantObst.lineReached", b1.reached ? 1.0 : 0.0);
  RecordObserved("distantObst.lineTicks", (double)b1.ticks);
  RecordObserved("distantObst.trunkTicks", (double)b2[0].ticks);
  RecordObserved("distantObst.trunkNearestAxis", (double)b2[0].minAxis);
  RecordObserved("distantObst.doorTicks", (double)cw[0].ticks);
  detail = Format("A %s | B1 line %s: %d obstructed, reached %d in %d | B2 trunk: reached %d, nearest %.1f, "
                  "control inside %d | C door: in %d via door %d, shut %d | D %.1f us/col, peaks %.1f -> %.1f "
                  "us/tick | E %s",
                  aLine.c_str(), b1Line.c_str(), b1Obstructed, b1.reached, b1.ticks, b2[0].reached,
                  (double)b2[0].minAxis, b2off.inside, cw[0].reached, cw[0].crossed, cShut.reached, colUs, usOff,
                  usOn, twiceWalks && redif == 0 ? "ok" : "FAIL");
  for (size_t i = 0; i < fails.size(); i++) detail += (i == 0 ? " | FAIL: " : " / ") + fails[i];
  detail += " | ms: " + laps;
  restore();
  return fails.empty() ? Status::Pass : Status::Fail;
}

const std::vector<Gate>& DistantObstacleGates() {
  static const std::vector<Gate> g = {
      // Trees, buildings and ruins are obstacles at distance (D10, §8.2).
      {"distant-obstacles", "mob", {}, false, GateDistantObstacles, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
