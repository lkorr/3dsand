// distant_obstacles.cpp -- THE WORLDGEN OVERLAY of DistantGround (header says
// what and why; docs/PLAN_distant_npcs.md §8.2 / §8.5, package D10).

#include "game/distant_obstacles.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <unordered_set>

#include "game/distant.h"
#include "sim/dungeon.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/worlddefaults.h"
#include "sim/worldedit.h"
#include "sim/worldmap.h"
#include "world/structures.h"

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

namespace {

using Clock = std::chrono::steady_clock;

// Bounds. All three are pure caches (an evicted entry is recomputed to the
// same answer), so a bound changes the cost of an answer, never the answer.
// A block is ~4 KiB of column headers plus its spans: 2,048 is ~10 MiB and
// covers a 720 x 720 voxel patch, far more than the tier walks between
// trims. Chunk records are ~1.5 KiB.
constexpr size_t kBlocksMax = 2048;
constexpr size_t kChunksMax = 4096;
constexpr size_t kTilesMax = 32768;

inline uint64_t Key2(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint64_t)(uint32_t)b; }
inline int FloorDiv(int a, int b) {
  int q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) q -= 1;
  return q;
}
inline uint64_t NsSince(Clock::time_point t0) {
  return (uint64_t)std::max<int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
}
// worldgen.wgsl wmSiteList over the CPU's copy of the site index: the
// column's list `[n, id_1 .. id_n]`, or null.
const uint32_t* SiteListAt(int x, int z, uint32_t& n) {
  n = 0;
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  if (!m.Loaded() || m.sites.empty()) return nullptr;
  int cx = 0, cz = 0;
  m.CellOf(x, z, &cx, &cz);
  if (!m.Inside(cx, cz)) return nullptr;
  const uint32_t* lst = m.SiteList(cx, cz);
  if (lst != nullptr) n = std::min<uint32_t>(lst[0], worldmap::kSiteCellMax);
  return lst;
}
// Drop the older half of a map by last use (Trim's rule in DistantGround).
template <class Map>
void TrimOlderHalf(Map& map) {
  if (map.empty()) return;
  std::vector<uint64_t> ages;
  ages.reserve(map.size());
  for (const auto& kv : map) ages.push_back(kv.second.lastUse);
  std::nth_element(ages.begin(), ages.begin() + (ptrdiff_t)(ages.size() / 2), ages.end());
  const uint64_t cut = ages[ages.size() / 2];
  for (auto it = map.begin(); it != map.end();)
    it = it->second.lastUse <= cut ? map.erase(it) : std::next(it);
}

// One tree candidate of an asking column (worldgen.wgsl TreeCand + what the
// overlay needs to decide its band and its pond gate).
struct TCand {
  int wx = 0, wz = 0, base = 0, ny = 0, vtop = 0;
  uint32_t colOff = 0, colCnt = 0, leafSw = 0;
  uint8_t src = 0;
  int pondIdx = -1;     // its PondAsk (lattice trees only; a site tree has none)
  bool inBand = false;  // its column here starts inside the obstacle band
};
struct CCand {
  wgtwin::Cactus c;
  int pondIdx = -1;
  int vtop = 0;
};

// worldgen.wgsl treeCellFrom over the atlas words.
uint32_t TreeCellFrom(const std::vector<uint32_t>& W, const TCand& c, int y) {
  auto w = [&](size_t i) { return i < W.size() ? W[i] : 0u; };
  int ly = y - c.base - 1;
  if (ly < 0) {
    // THE GROUND-ROW RULE (treeCellFrom 3492-3497): a column whose lowest run
    // starts at ly 0 fills down to the asking column's own ground.
    if (c.colCnt == 0u) return 0u;
    if (((w(c.colOff) >> 16u) & treeatlas::kRunMaxY0) != 0u) return 0u;
    ly = 0;
  }
  if (ly >= c.ny) return 0u;
  for (uint32_t k = 0; k < c.colCnt; k++) {
    const uint32_t r = w(c.colOff + k);
    const int y0 = (int)((r >> 16u) & treeatlas::kRunMaxY0);
    if (ly < y0) return 0u;
    if (ly < y0 + (int)((r >> 27u) & treeatlas::kRunMaxLen)) {
      uint32_t m = r & 0xFFFu;
      if (c.leafSw != 0u)
        for (uint32_t i = 0; i < 3u; i++)
          if (m == w(c.leafSw + treeatlas::kSLeaf0 + i)) {
            m = w(c.leafSw + treeatlas::kSAutumn0 + i);
            break;
          }
      return m;
    }
  }
  return 0u;
}

}  // namespace

const char* GenOverlay::SourceName(Source s) {
  switch (s) {
    case kSrcBase: return "base";
    case kSrcTree: return "tree";
    case kSrcSiteTree: return "siteTree";
    case kSrcCactus: return "cactus";
    case kSrcStamp: return "stamp";
    case kSrcDoor: return "door";
    case kSrcDungeon: return "dungeon";
    case kSrcLayer: return "layer";
    default: return "?";
  }
}

GenOverlay::GenOverlay(DistantGround* ground) : ground_(ground), seed_(sandvox::kDefaultSeed) {}
GenOverlay::~GenOverlay() = default;

void GenOverlay::SetOptions(const Options& o) {
  opt_ = o;
  DropAll();
}
void GenOverlay::SetEnabled(bool on) { enabled_ = on; }

void GenOverlay::Bind(const std::vector<uint32_t>* classOf, const std::vector<MaterialGpu>* gpu,
                      uint32_t seed) {
  const size_t cs = classOf != nullptr ? classOf->size() : 0;
  const size_t gs = gpu != nullptr ? gpu->size() : 0;
  if (classOf != classOf_ || cs != classOfSize_ || gpu != gpu_ || gs != gpuSize_) {
    // What carries weight changed: every decided span is wrong.
    classOf_ = classOf;
    classOfSize_ = cs;
    gpu_ = gpu;
    gpuSize_ = gs;
    DropAll();
  }
  seed_ = seed;   // CheckEpoch drops on a change
}

void GenOverlay::InvalidateAll() {
  DropAll();
  epochValid_ = false;
}

void GenOverlay::DropAll() {
  blocks_.clear();
  chunks_.clear();
  treeTiles_.clear();
  siteTrees_.clear();
  cactusTiles_.clear();
  memoBlockKey_ = memoChunkKey_ = ~0ull;
  memoBlock_ = nullptr;
  memoChunk_ = nullptr;
  doorsBuilt_ = dgBuilt_ = layerBuilt_ = false;
  doors_.clear();
  dg_.clear();
  twinMats_.clear();
  layer_.reset();
  layerCols_.clear();
  stats_.epochs++;
}

void GenOverlay::CheckEpoch() {
  // Everything the answers are a function of, besides the classes (Bind):
  // the map, the uploaded atlas + biome records, the layer's content, the
  // seed, and the vegetation switch the shaders were compiled with.
  const uint64_t ms = worldmap::CurrentWorldMapSerial();
  const uint64_t es = wgtwin::CurrentGenEnv().serial;
  const uint64_t lg = sandvox::WorldEditLayer().Generation();
  const uint32_t veg = CurrentTuning().debug.vegetation;
  if (epochValid_ && ms == mapSerial_ && es == envSerial_ && lg == layerGen_ && veg == vegetation_ &&
      seed_ == epochSeed_)
    return;
  DropAll();
  mapSerial_ = ms;
  envSerial_ = es;
  layerGen_ = lg;
  vegetation_ = veg;
  epochSeed_ = seed_;
  epochValid_ = true;
}

// ONE MAP PER TRIM, never all of them: a caller holds a reference into the
// block it just asked (Cell, Touches) while it asks a chunk, and a chunk trim
// that also trimmed blocks would leave it dangling.
void GenOverlay::TrimBlocks() {
  TrimOlderHalf(blocks_);
  memoBlockKey_ = ~0ull;
  memoBlock_ = nullptr;
}
void GenOverlay::TrimChunks() {
  TrimOlderHalf(chunks_);
  memoChunkKey_ = ~0ull;
  memoChunk_ = nullptr;
}

bool GenOverlay::Supports(uint32_t mat) const {
  return classOf_ != nullptr && WordCarriesWeight(mat & 0xFFFu, *classOf_);
}

// ---- the per-epoch inputs ------------------------------------------------------

void GenOverlay::BuildDungeonWords() {
  dgBuilt_ = true;
  dg_.clear();
  twinMats_.clear();
  // The dungeon block of the CURRENT map, packed the way PackWorldMap packs
  // it (its PlacedOf, the same switches), into a worldMap-shaped vector of
  // its own: the GPU's words belong to the map that was last UPLOADED, which
  // a CPU-only map swap (distant-walk's `peaks`) leaves behind.
  std::vector<dungeon::Placed> placed;
  for (const worldmap::DungeonDef& g : worldmap::CurrentWorldMap().dungeons) {
    if (!g.Placed()) continue;
    dungeon::Placed pl;
    pl.id = g.id;
    pl.shell = g.shell;
    pl.res = g.res;
    pl.yawQ = g.yawQ;
    pl.origin = g.origin;
    pl.boxMin = g.boxMin;
    pl.boxMax = g.boxMax;
    pl.keepMin = g.keepMin;
    pl.keepMax = g.keepMax;
    pl.surf = g.surf;
    placed.push_back(std::move(pl));
  }
  std::string log;
  if (!dungeon::PackDungeons(placed, dg_, log, nullptr, !worldmap::DungeonsEnabled(),
                             !worldmap::DungeonSurfaceKeep()))
    dg_.clear();
  // dungeon::TwinCell reads `klass` and `repose` off the material table.
  if (gpu_ != nullptr) {
    twinMats_.resize(gpu_->size());
    for (size_t i = 0; i < gpu_->size(); i++) twinMats_[i].gpu = (*gpu_)[i];
  }
}

void GenOverlay::BuildDoors() {
  doorsBuilt_ = true;
  doors_.clear();
  // Every placed STRUCTURE's door slots, through the same transform its child
  // refs get (structures::DeriveChildren: the leaf box, both corners through
  // the cell frame) -- so the box is the box the game's door ref holds. The
  // default map's doors are all slots of this kind; an authored `door` ref in
  // a group file (none on the shipped maps) is not read here.
  const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
  std::unordered_map<std::string, structures::Asset> assets;
  const std::string ad = sandvox::AssetDir();
  for (const worldmap::WorldMapData::StampSite& s : m.sites) {
    if (!s.structure || s.kind != worldmap::kSiteStamp || s.base.empty()) continue;
    auto it = assets.find(s.base);
    if (it == assets.end()) {
      structures::Asset a;
      std::string err;
      std::vector<std::string> warn;
      if (!structures::LoadAsset(ad, s.base, false, a, err, warn)) continue;
      it = assets.emplace(s.base, std::move(a)).first;
    }
    refs::Ref inst;
    inst.id = s.id;
    inst.kind = "structure";
    inst.base = s.base;
    inst.pos = IVec3{s.refX, s.refY, s.refZ};
    inst.yaw = s.refYaw;
    std::vector<refs::Ref> kids;
    structures::DeriveChildren(it->second, inst, kids);
    for (const refs::Ref& k : kids) {
      if (k.kind != "door" || !k.props.contains("leaf")) continue;
      const refs::Json& leaf = k.props["leaf"];
      if (!leaf.contains("min") || !leaf.contains("max") || !leaf["min"].is_array() ||
          !leaf["max"].is_array() || leaf["min"].size() != 3 || leaf["max"].size() != 3)
        continue;
      DoorBox d;
      d.lo = IVec3{leaf["min"][0].get<int>(), leaf["min"][1].get<int>(), leaf["min"][2].get<int>()};
      d.hi = IVec3{leaf["max"][0].get<int>(), leaf["max"][1].get<int>(), leaf["max"][2].get<int>()};
      d.id = k.id;
      doors_.push_back(std::move(d));
    }
  }
  stats_.doors = doors_.size();
}

const std::vector<GenOverlay::DoorBox>& GenOverlay::Doors() {
  CheckEpoch();
  if (!doorsBuilt_) BuildDoors();
  return doors_;
}

void GenOverlay::BuildLayer() {
  layerBuilt_ = true;
  layer_.reset();
  layerCols_.clear();
  const sandvox::WorldEdits& g = sandvox::WorldEditLayer();
  if (g.Empty()) return;
  // A PRIVATE COPY, resolved here: resolving the process's own layer for a
  // map it is not generating (a gate's CPU-only swap) would drop its pending
  // ops. Resolve is a pure function of (the files, the seed, the map), and a
  // no-op on a copy already resolved for both.
  layer_ = std::make_unique<sandvox::WorldEdits>(g);
  layer_->Resolve(seed_);
  std::vector<sandvox::WorldEdits::AbsCell> cells;
  layer_->ResolvedCells(cells);
  for (const auto& c : cells) layerCols_.insert(Key2(c.x >> 4, c.z >> 4));
}

const wgtwin::Tree& GenOverlay::TreeTile(int tx, int tz) {
  const uint64_t key = Key2(tx, tz);
  auto it = treeTiles_.find(key);
  if (it == treeTiles_.end()) {
    if (treeTiles_.size() >= kTilesMax) TrimOlderHalf(treeTiles_);
    if (!dgBuilt_) BuildDungeonWords();
    const wgtwin::GenEnv& env = wgtwin::CurrentGenEnv();
    TreeTileE e;
    if (env.atlas && env.map) e.t = wgtwin::LatticeTree(tx, tz, seed_, *env.atlas, *env.map, dg_);
    else wgtwin::LatticeTreeSite(tx, tz, seed_, e.t.wx, e.t.wz);
    stats_.treeTiles++;
    it = treeTiles_.emplace(key, e).first;
  }
  it->second.lastUse = ++useClock_;
  return it->second.t;
}

const wgtwin::Tree& GenOverlay::SiteTree(uint32_t sid) {
  auto it = siteTrees_.find(sid);
  if (it == siteTrees_.end()) {
    const wgtwin::GenEnv& env = wgtwin::CurrentGenEnv();
    wgtwin::Tree t;
    if (env.atlas) t = wgtwin::SiteTree(sid, seed_, *env.atlas);
    it = siteTrees_.emplace(sid, t).first;
  }
  return it->second;
}

const wgtwin::Cactus& GenOverlay::CactusTile(int tx, int tz) {
  const uint64_t key = Key2(tx, tz);
  auto it = cactusTiles_.find(key);
  if (it == cactusTiles_.end()) {
    if (cactusTiles_.size() >= kTilesMax) TrimOlderHalf(cactusTiles_);
    if (!dgBuilt_) BuildDungeonWords();
    const wgtwin::GenEnv& env = wgtwin::CurrentGenEnv();
    CactusTileE e;
    if (env.map) e.c = wgtwin::CactusAt(tx, tz, seed_, *env.map, dg_);
    stats_.cactusTiles++;
    it = cactusTiles_.emplace(key, e).first;
  }
  it->second.lastUse = ++useClock_;
  return it->second.c;
}

// ---- THE COLUMN MODEL ------------------------------------------------------------

GenOverlay::Col& GenOverlay::ColumnOf(int x, int z) {
  const uint64_t key = Key2(x >> 4, z >> 4);
  if (key != memoBlockKey_ || memoBlock_ == nullptr) {
    auto it = blocks_.find(key);
    if (it == blocks_.end()) {
      if (blocks_.size() >= kBlocksMax) TrimBlocks();
      it = blocks_.emplace(key, Block{}).first;
      // Can a per-cell record (a dungeon's index, the layer) exist anywhere
      // in this chunk column? If not, Touches never asks a chunk here.
      Block& nb = it->second;
      if (!layerBuilt_) BuildLayer();
      nb.mayChunk = layerCols_.count(key) != 0;
      if (!nb.mayChunk) {
        const int x0 = (x >> 4) * 16, z0 = (z >> 4) * 16;
        for (const worldmap::DungeonDef& g : worldmap::CurrentWorldMap().dungeons)
          if (g.Placed() && g.boxMax[0] >= x0 && g.boxMin[0] <= x0 + 15 && g.boxMax[2] >= z0 &&
              g.boxMin[2] <= z0 + 15)
            nb.mayChunk = true;
      }
    }
    memoBlockKey_ = key;
    memoBlock_ = &it->second;
  }
  Block& b = *memoBlock_;
  b.lastUse = ++useClock_;
  Col& c = b.col[((z & 15) << 4) | (x & 15)];
  if (!c.built) BuildColumn(b, c, x, z);
  return c;
}

void GenOverlay::BuildColumn(Block& b, Col& c, int x, int z) {
  const auto t0 = Clock::now();
  c.built = 1;
  c.first = (uint32_t)b.spans.size();
  c.n = 0;
  c.differs = 0;
  c.dLo = 0;
  c.dHi = -1;
  stats_.columns++;
  if (!dgBuilt_) BuildDungeonWords();
  if (!doorsBuilt_) BuildDoors();

  const int h = ground_->TerrainTop(x, z);
  const wgtwin::GenEnv& env = wgtwin::CurrentGenEnv();
  const TreeAtlas* A = env.atlas.get();
  const std::vector<uint32_t>* bio = env.map.get();
  const int band = std::max(opt_.treeBand, 1);

  // ---- 1. the tree candidates (treeCandsFromTiles + siteTreesInto) -------------
  std::vector<TCand> trees;
  std::vector<wgtwin::PondAsk> asks;
  bool anyTreeInBand = false;
  if (A != nullptr && A->speciesCount > 0 && A->words.size() > treeatlas::kHeaderWords) {
    const std::vector<uint32_t>& W = A->words;
    auto aw = [&](size_t i) { return i < W.size() ? W[i] : 0u; };
    auto add = [&](const wgtwin::Tree& t, uint8_t src) {
      // worldgen.wgsl treeCandAdd: the species' own reach, then the variant's
      // column under this one (an empty column contributes nothing).
      if (!t.present) return;
      if (std::abs(x - t.wx) > t.reach || std::abs(z - t.wz) > t.reach) return;
      int rx = x - t.wx, rz = z - t.wz;
      const int dx = rx, dz = rz;
      switch (t.rot) {
        case 1u: rx = -dz; rz = dx; break;
        case 2u: rx = -dx; rz = -dz; break;
        case 3u: rx = dz; rz = -dx; break;
        default: break;
      }
      if (t.mir) rx = -rx;
      const int lx = (int)aw(t.varOff + treeatlas::kVAnchorX) + rx;
      const int lz = (int)aw(t.varOff + treeatlas::kVAnchorZ) + rz;
      const int nx = (int)aw(t.varOff + treeatlas::kVNx), nz = (int)aw(t.varOff + treeatlas::kVNz);
      if (lx < 0 || lz < 0 || lx >= nx || lz >= nz) return;
      const uint32_t ci = aw(t.varOff + treeatlas::kVColumns) + (uint32_t)(lz * nx + lx) * 2u;
      const uint32_t cnt = aw(ci + 1u);
      if (cnt == 0u) return;
      TCand k;
      k.wx = t.wx;
      k.wz = t.wz;
      k.base = t.base;
      k.ny = (int)aw(t.varOff + treeatlas::kVNy);
      k.vtop = t.base + 1 + k.ny;
      k.colOff = aw(ci);
      k.colCnt = cnt;
      k.leafSw = t.autumn ? aw(treeatlas::kHSpeciesDir) + (uint32_t)t.sp * (uint32_t)treeatlas::kSpeciesWords : 0u;
      k.src = src;
      k.inBand = (int)((aw(k.colOff) >> 16u) & treeatlas::kRunMaxY0) < band;
      if (src == kSrcTree && asks.size() < 64) {
        k.pondIdx = (int)asks.size();
        asks.push_back(wgtwin::PondAsk{t.wx, t.wz, t.nwMax, t.nwMin});
      }
      anyTreeInBand = anyTreeInBand || k.inBand;
      trees.push_back(k);
    };
    const treeatlas::TreeLattice& L = treeatlas::CurrentTreeLattice();
    const int T = std::max(L.tile, 1), S = L.scan;
    const int maxReach = (int)aw(treeatlas::kHMaxReach);
    const int tx = FloorDiv(x, T), tz = FloorDiv(z, T);
    for (int oz = -S; oz <= S; oz++)
      for (int ox = -S; ox <= S; ox++) {
        const wgtwin::Tree& t = TreeTile(tx + ox, tz + oz);
        // The horizontal reject on the trunk SITE, against the widest species.
        if (std::abs(x - t.wx) > maxReach || std::abs(z - t.wz) > maxReach) continue;
        add(t, kSrcTree);
      }
    uint32_t n = 0;
    if (const uint32_t* lst = SiteListAt(x, z, n))
      for (uint32_t k = 0; k < n; k++) {
        const uint32_t sid = lst[1u + k];
        const auto& sites = worldmap::CurrentWorldMap().sites;
        if (sid == 0 || sid > sites.size() || sites[sid - 1].kind != worldmap::kSiteTree) continue;
        add(SiteTree(sid), kSrcSiteTree);
      }
  }

  // ---- 2. the cactus candidates (cactusAt's 3 x 3 tiles) ----------------------------
  std::vector<CCand> cacti;
  const int CT = bio != nullptr ? wgtwin::CactusTile() : 0;
  if (CT > 0) {
    const int tx = FloorDiv(x, CT), tz = FloorDiv(z, CT);
    for (int oz = -1; oz <= 1; oz++)
      for (int ox = -1; ox <= 1; ox++) {
        const wgtwin::Cactus& cc = CactusTile(tx + ox, tz + oz);
        if (!cc.present) continue;
        const int r = wgtwin::CactusReach(cc);
        if (std::abs(x - cc.wx) > r || std::abs(z - cc.wz) > r) continue;
        CCand k;
        k.c = cc;
        k.vtop = wgtwin::CactusVTop(cc);
        if (asks.size() < 64) {
          k.pondIdx = (int)asks.size();
          asks.push_back(wgtwin::PondAsk{cc.wx, cc.wz, -1, 0});
        }
        cacti.push_back(k);
      }
  }

  // ---- 3. the asking column, only where a feature can put an obstacle here ----
  wgtwin::AskColumn ask;
  uint64_t okMask = 0;
  bool asked = false;
  int fluidTop = INT_MIN;
  if (anyTreeInBand || !cacti.empty()) {
    ask = wgtwin::AskColumnAt(x, z, seed_, asks.data(), (int)asks.size(), okMask);
    asked = true;
    stats_.asks++;
    const int wt = ground_->WaterTop(x, z);
    if (wt != DistantGround::kNoWater) fluidTop = wt;
  }
  auto pondOk = [&](int idx) { return idx < 0 || ((okMask >> idx) & 1ull) != 0; };
  const bool vegOk = asked && h < ask.tl && !ask.inPond;   // genCellIn's tree / cactus column gates
  const int yFrom = std::max(h, fluidTop) + 1;             // over the ground and its water

  // The trees genCellIn actually reads: pond-gated, then capped in order
  // (treeCandAdd's TREE_CAND_MAX -- derived so it never bites, kept exact).
  std::vector<const TCand*> kept;
  int yTreeTop = INT_MIN, yTreeBand = INT_MIN;
  if (vegOk) {
    const int cap = std::max(treeatlas::CurrentTreeLattice().candMax, 1);
    for (const TCand& k : trees) {
      if (!pondOk(k.pondIdx)) continue;
      if ((int)kept.size() >= cap) break;
      kept.push_back(&k);
      yTreeTop = std::max(yTreeTop, k.vtop);
      if (k.inBand) yTreeBand = std::max(yTreeBand, std::min(k.vtop, k.base + band));
    }
  }
  const bool cactusBiome =
      vegOk && bio != nullptr &&
      (wgtwin::BiomeWord(*bio, ask.biome, worldmap::kB_Flags) & worldmap::kBF_Cacti) != 0u;
  int yCactusTop = INT_MIN;
  if (cactusBiome)
    for (const CCand& k : cacti)
      if (pondOk(k.pondIdx)) yCactusTop = std::max(yCactusTop, k.vtop);

  // ---- the window this column's features can decide ----------------------------------
  const auto& sites = worldmap::CurrentWorldMap().sites;
  uint32_t nSites = 0;
  const uint32_t* siteLst = SiteListAt(x, z, nSites);
  int lo = INT_MAX, hi = INT_MIN;
  if (yTreeBand != INT_MIN) {
    lo = std::min(lo, yFrom);
    hi = std::max(hi, std::max(yTreeBand, yCactusTop));
  }
  if (yCactusTop != INT_MIN) {
    lo = std::min(lo, yFrom);
    hi = std::max(hi, yCactusTop);
  }
  for (uint32_t k = 0; k < nSites; k++) {
    const uint32_t sid = siteLst[1u + k];
    if (sid == 0 || sid > sites.size()) continue;
    const worldmap::WorldMapData::StampSite& s = sites[sid - 1];
    if (s.kind != worldmap::kSiteStamp || s.words.empty()) continue;
    const int y0 = wgtwin::SitePadY(sid) + 1 - s.sink;
    lo = std::min(lo, std::max(y0, h - 1 - (int)worldmap::kStampSinkMax));
    hi = std::max(hi, y0 + s.ny - 1);
  }
  if (opt_.doors)
    for (const DoorBox& d : doors_)
      if (x >= d.lo.x && x <= d.hi.x && z >= d.lo.z && z <= d.hi.z) {
        lo = std::min(lo, d.lo.y);
        hi = std::max(hi, d.hi.y);
      }

  if (lo <= hi && hi - lo < 4096) {
    struct Cell8 {
      int8_t state = -1;   // -1 undecided, 0 air, 1 carries weight
      uint8_t src = 0;
      uint8_t treeTook = 0;   // a tree (in band or not) holds the cell: no cactus there
      uint8_t stampTook = 0;  // an earlier listed stamp holds the cell
      uint16_t mat = 0;
    };
    std::vector<Cell8> win((size_t)(hi - lo + 1));
    auto at = [&](int y) -> Cell8* { return y < lo || y > hi ? nullptr : &win[(size_t)(y - lo)]; };

    // 1. TREES: the first kept candidate with a non-air cell wins the cell
    // (treeFromCands); it is an OBSTACLE only inside its own band.
    if (!kept.empty()) {
      const int top = std::min(hi, yTreeTop);
      for (int y = yFrom; y <= top; y++) {
        for (const TCand* k : kept) {
          if (y > k->vtop) continue;
          const uint32_t m = TreeCellFrom(A->words, *k, y);
          if (m == 0u) continue;
          Cell8* cl = at(y);
          if (cl == nullptr) break;
          cl->treeTook = 1;
          const int ly = std::max(y - k->base - 1, 0);
          if (ly < band) {
            cl->state = Supports(m) ? 1 : 0;
            cl->src = k->src;
            cl->mat = (uint16_t)m;
          }
          break;
        }
      }
    }
    // 2. CACTI, into cells no tree took (cactusAt: first non-air wins; a cell
    // under the base is the bole's base cross-section).
    if (cactusBiome) {
      const int top = std::min(hi, yCactusTop);
      for (int y = yFrom; y <= top; y++) {
        Cell8* cl = at(y);
        if (cl == nullptr || cl->treeTook) continue;
        for (const CCand& k : cacti) {
          if (!pondOk(k.pondIdx) || y > k.vtop) continue;
          const uint32_t m = wgtwin::CactusCell(k.c, x, std::max(y, k.c.base), z);
          if (m == 0u) continue;
          cl->state = Supports(m) ? 1 : 0;
          cl->src = kSrcCactus;
          cl->mat = (uint16_t)m;
          break;
        }
      }
    }
    // 3. STAMPS: the first listed stamp with a non-air template cell
    // replaces whatever is there, from h - 1 - kStampSinkMax up.
    for (uint32_t k = 0; k < nSites; k++) {
      const uint32_t sid = siteLst[1u + k];
      if (sid == 0 || sid > sites.size()) continue;
      const worldmap::WorldMapData::StampSite& s = sites[sid - 1];
      if (s.kind != worldmap::kSiteStamp || s.words.empty()) continue;
      const int padY = wgtwin::SitePadY(sid);
      const int lx = x - (s.x - s.nx / 2), lz = z - (s.z - s.nz / 2);
      if (lx < 0 || lz < 0 || lx >= s.nx || lz >= s.nz) continue;
      const size_t ci = worldmap::kStampHdrWords + ((size_t)lz * s.nx + lx) * 2;
      if (ci + 1 >= s.words.size()) continue;
      const uint32_t runOff = s.words[ci], cnt = s.words[ci + 1];
      for (uint32_t r = 0; r < cnt && runOff + r < s.words.size(); r++) {
        const uint32_t run = s.words[runOff + r];
        const int y0 = (int)((run >> 16) & treeatlas::kRunMaxY0);
        const int len = (int)((run >> 27) & treeatlas::kRunMaxLen);
        const uint32_t m = run & 0xFFFu;
        for (int ly = y0; ly < y0 + len && ly < s.ny; ly++) {
          const int y = ly + padY + 1 - s.sink;
          if (y < h - 1 - (int)worldmap::kStampSinkMax) continue;
          Cell8* cl = at(y);
          if (cl == nullptr || cl->stampTook) continue;
          cl->stampTook = 1;
          cl->state = Supports(m) ? 1 : 0;
          cl->src = kSrcStamp;
          cl->mat = (uint16_t)m;
        }
      }
    }
    // 3b. DOORS read air.
    if (opt_.doors)
      for (const DoorBox& d : doors_) {
        if (x < d.lo.x || x > d.hi.x || z < d.lo.z || z > d.hi.z) continue;
        for (int y = d.lo.y; y <= d.hi.y; y++)
          if (Cell8* cl = at(y)) {
            cl->state = 0;
            cl->src = kSrcDoor;
            cl->mat = 0;
          }
      }

    // ---- to spans: runs of one (answer, source), split where the base flips ----
    for (int y = lo; y <= hi;) {
      const Cell8& a = win[(size_t)(y - lo)];
      if (a.state < 0) {
        y++;
        continue;
      }
      const bool baseSolid = y <= h;
      int e = y;
      while (e + 1 <= hi) {
        const Cell8& nx = win[(size_t)(e + 1 - lo)];
        if (nx.state != a.state || nx.src != a.src || ((e + 1) <= h) != baseSolid) break;
        e++;
      }
      Span sp;
      sp.lo = y;
      sp.hi = e;
      sp.mat = a.mat;
      sp.solid = (uint8_t)a.state;
      sp.src = a.src;
      b.spans.push_back(sp);
      c.n++;
      if ((a.state == 1) != baseSolid) {
        if (!c.differs) {
          c.dLo = y;
          c.dHi = e;
        } else {
          c.dLo = std::min(c.dLo, y);
          c.dHi = std::max(c.dHi, e);
        }
        c.differs = 1;
      }
      y = e + 1;
    }
  }
  if (c.n != 0) stats_.featureColumns++;
  const uint64_t ns = NsSince(t0);
  stats_.columnNs += ns;
  stats_.columnNsMax = std::max(stats_.columnNsMax, ns);
}

const GenOverlay::Span* GenOverlay::SpanAt(const Col& c, const Block& b, int y) const {
  for (uint32_t i = 0; i < c.n; i++) {
    const Span& s = b.spans[c.first + i];
    if (y < s.lo) return nullptr;   // ascending, disjoint
    if (y <= s.hi) return &s;
  }
  return nullptr;
}

// ---- THE PER-CELL RECORDS (the dungeon, the layer) ----------------------------------

GenOverlay::Chunk& GenOverlay::ChunkOf(IVec3 wc) {
  const uint64_t key = World::PackChunkKey(wc);
  if (key == memoChunkKey_ && memoChunk_ != nullptr) {
    memoChunk_->lastUse = ++useClock_;
    return *memoChunk_;
  }
  auto it = chunks_.find(key);
  if (it == chunks_.end()) {
    if (chunks_.size() >= kChunksMax) TrimChunks();
    if (!dgBuilt_) BuildDungeonWords();
    if (!layerBuilt_) BuildLayer();
    Chunk e;
    // The dungeon's 64^3 index cell holds the whole chunk (16 | 64).
    e.dungeon = dungeon::TwinListAt(dg_, wc.x * (int)kChunk, wc.y * (int)kChunk, wc.z * (int)kChunk) != 0u;
    e.layer = layer_ != nullptr && layer_->HasChunk(wc);
    if (e.layer && classOf_ != nullptr) {
      // The layer's resolved cells over a sentinel no layer word can be
      // (CleanWord strips bit 31): written cells, classified by the near
      // tier's own "carries weight".
      constexpr uint32_t kUnset = 0xFFFFFFFFu;
      scratch_.assign(kChunkVol, kUnset);
      layer_->ApplyToChunk(wc, scratch_.data());
      for (uint32_t v = 0; v < kChunkVol; v++) {
        if (scratch_[v] == kUnset) continue;
        const bool s = WordCarriesWeight(scratch_[v], *classOf_);
        e.ldef[v >> 6] |= 1ull << (v & 63);
        if (s) e.lsolid[v >> 6] |= 1ull << (v & 63);
        const int lx = (int)(v % kChunk), ly = (int)((v / kChunk) % kChunk), lz = (int)(v / (kChunk * kChunk));
        const int wx = wc.x * (int)kChunk + lx, wz = wc.z * (int)kChunk + lz;
        const bool base = wc.y * (int)kChunk + ly <= ground_->TerrainTop(wx, wz);
        if (s != base) e.lcolDiff[(lz * 16 + lx) >> 6] |= 1ull << ((lz * 16 + lx) & 63);
      }
    }
    stats_.chunks++;
    it = chunks_.emplace(key, e).first;
  }
  memoChunkKey_ = key;
  memoChunk_ = &it->second;
  memoChunk_->lastUse = ++useClock_;
  return *memoChunk_;
}

// ---- THE QUESTIONS -------------------------------------------------------------------

bool GenOverlay::Cell(int x, int y, int z, bool& solid, Source* src) {
  if (!enabled_ || classOf_ == nullptr) return false;
  CheckEpoch();
  const Col& c = ColumnOf(x, z);
  Span sp;
  bool haveSpan = false;
  if (c.n != 0)
    if (const Span* s = SpanAt(c, *memoBlock_, y)) {
      sp = *s;
      haveSpan = true;
    }
  // A door wins over everything: it is the policy, not a twin (the header).
  if (haveSpan && sp.src == kSrcDoor) {
    solid = false;
    if (src) *src = kSrcDoor;
    return true;
  }
  if (memoBlock_->mayChunk) {
    Chunk& ch = ChunkOf(IVec3{x >> 4, y >> 4, z >> 4});
    const uint32_t v = ((uint32_t)(z & 15) * kChunk + (uint32_t)(y & 15)) * kChunk + (uint32_t)(x & 15);
    const uint64_t bit = 1ull << (v & 63);
    // 5. THE LAYER, applied last.
    if (ch.layer && (ch.ldef[v >> 6] & bit) != 0) {
      solid = (ch.lsolid[v >> 6] & bit) != 0;
      if (src) *src = kSrcLayer;
      return true;
    }
    // 4. THE DUNGEON over the terrain word 0-3 made (genChunk 6813), evaluated
    // on first ask. The terrain word here is the overlay's own: the feature's
    // material where a span decides, else stone under the ground and the
    // column's water over it -- dgCell's solidity reads the word's class
    // (keep, a skin over non-air, an open-only fill, the firm shell round a
    // flowy cell), which these carry.
    if (ch.dungeon) {
      if ((ch.dEval[v >> 6] & bit) == 0) {
        const auto t0 = Clock::now();
        const int h = ground_->TerrainTop(x, z);
        uint32_t tw = 0u;
        if (haveSpan) {
          tw = sp.mat;
        } else if (y <= h) {
          tw = kMatStone;
        } else {
          const int wt = ground_->WaterTop(x, z);
          if (wt != DistantGround::kNoWater && y <= wt) tw = kMatWater | (7u << 12);
        }
        const uint32_t w = dungeon::TwinCell(dg_, twinMats_, seed_, x, y, z, tw, h);
        ch.dEval[v >> 6] |= bit;
        if (w != tw) ch.dTouched[v >> 6] |= bit;
        if (WordCarriesWeight(w, *classOf_)) ch.dSolid[v >> 6] |= bit;
        stats_.dungeonCells++;
        stats_.dungeonNs += NsSince(t0);
      }
      if ((ch.dTouched[v >> 6] & bit) != 0) {
        solid = (ch.dSolid[v >> 6] & bit) != 0;
        if (src) *src = kSrcDungeon;
        return true;
      }
    }
  }
  if (haveSpan) {
    solid = sp.solid != 0;
    if (src) *src = (Source)sp.src;
    return true;
  }
  return false;
}

bool GenOverlay::Touches(int x, int z, int yLo, int yHi) {
  if (!enabled_ || classOf_ == nullptr) return false;
  CheckEpoch();
  const Col& c = ColumnOf(x, z);
  if (c.differs && c.dLo <= yHi && c.dHi >= yLo) return true;
  if (!memoBlock_->mayChunk) return false;
  const uint32_t ci = (uint32_t)((z & 15) * 16 + (x & 15));
  for (int cy = yLo >> 4; cy <= (yHi >> 4); cy++) {
    const Chunk& ch = ChunkOf(IVec3{x >> 4, cy, z >> 4});
    if (ch.dungeon) return true;   // conservative: the per-cell path asks
    if (ch.layer && ((ch.lcolDiff[ci >> 6] >> (ci & 63)) & 1ull) != 0) return true;
  }
  return false;
}
