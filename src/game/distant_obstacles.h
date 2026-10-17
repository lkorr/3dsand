// distant_obstacles.h -- TREES, BUILDINGS AND RUINS ARE OBSTACLES AT DISTANCE
// (docs/PLAN_distant_npcs.md §8.2, package D10).
//
// DistantGround's ground was the HEIGHT CONTRACT (World::TerrainHeight) plus
// the chunks the player edited: a distant creature walked through a house
// 60 m away. This is the source that was missing, THE WORLDGEN OVERLAY: what
// genChunk puts on and into the contract's ground, re-derived on the CPU from
// the same inputs the shader reads (sim/worldgen_twin.h), never read back
// from the GPU (rule 1: a readback's arrival tick is the scheduler's).
//
// THE SOURCES, IN genChunk's ORDER -- later wins, exactly as the shader
// composes a cell (worldgen.wgsl genCellIn 5302-5718, genChunk 6787-6813,
// then the edit layer's op-stream patch after genChunk):
//
//   0. BASE        solid at or under TerrainHeight, air over it (DistantGround).
//   1. LATTICE TREES  genCellIn's tree block (5617-5627): only cells over the
//                  asking column's ground and its water, below its treeline,
//                  outside a tarn disc; the first candidate with a non-air
//                  cell wins (treeFromCands). ONLY A TREE'S LOW BAND is an
//                  obstacle (kTreeObstacleBandVox over its base: the trunk,
//                  the root flare, a bush's skirt, low boughs); a crown
//                  overhead stops no walker, so its cells stay BASE.
//   1b. AUTHORED TREE SITES  the same block (siteTreesInto, after the lattice).
//   2. CACTI        genCellIn 5669-5675, into cells no tree took.
//   3. STAMPS       genCellIn 5712-5716 (wmStampsCell): a non-air template cell
//                  replaces whatever 0-2 put there; template air leaves it.
//   3b. DOORS       every door slot's LEAF BOX of every placed structure reads
//                  AIR: closed, a door is template voxels, but the distant
//                  tier opens none and a villager walking its day (D5) goes
//                  through it. A policy, not a twin: the gate counts these.
//   4. DUNGEONS     genChunk 6813 (dgCell over the terrain word 0-3 made),
//                  through dungeon::TwinCell over a dungeon block packed from
//                  the CURRENT map (so a CPU-only map swap is consistent).
//   5. THE EDIT LAYER  the resolved .svedit layers, applied LAST as worldgen
//                  applies them (a private resolved copy of WorldEditLayer():
//                  resolving the process's own would drop its pending ops).
//
// NOT MODELLED (the gate attributes these by material when it meets them):
// caves (they stay under the surface by construction), the shore cover, the
// tile plants and the cover stack (passable vegetation, or a cell a cactus
// would have taken), the top cell's partial grains, and anything the CA did
// after generation (a stored chunk is a different, earlier source).
//
// THE SHAPE OF THE CACHE. Every feature but the dungeon and the layer is a
// set of RUNS per world column (an atlas variant's column, a stamp's column),
// and its expensive inputs (the asking column's pond scan, its treeline) are
// per column too, so the model is per COLUMN: a short list of y-spans that
// the overlay decides, built lazily on the first question about the column,
// held in 16 x 16 blocks (one per chunk column) under an age-trimmed bound.
// The dungeon and the layer are per CELL: per-chunk records, the layer's
// cells decoded when the chunk is first asked, a dungeon cell evaluated when
// it is first asked. All of it is a pure function of (seed, the map, the
// uploaded environment, the layer, the material classes): a hit and a miss
// give the same answer, so no cache state can make two runs differ.
//
// CPU only, single-threaded (DistantGround's caller: the tick), never hashed,
// never saved.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"   // CLASS_*, MaterialGpu
#include "sim/world.h"       // PowderMassOfState, kPowderMobSupportMin
#include "sim/worldgen_twin.h"

class DistantGround;
namespace sandvox { class WorldEdits; }

// How tall the obstacle band of a tree is, voxels over its base (3.2 m).
inline constexpr int kTreeObstacleBandVox = 32;

// The near tier's "carries weight" (Mob::GroundHeightAt's `supports`): solid,
// or a powder holding at least kPowderMobSupportMin of a cell. Liquids, gases
// and passable vegetation (mapped to gas in the class table) never.
inline bool WordCarriesWeight(uint32_t w, const std::vector<uint32_t>& cls) {
  const uint32_t mat = w & 0xFFFu;
  if (mat == 0 || mat >= cls.size()) return false;
  return cls[mat] == CLASS_SOLID ||
         (cls[mat] == CLASS_POWDER &&
          PowderMassOfState((w >> 12) & 0xFu) >= kPowderMobSupportMin);
}

class GenOverlay {
 public:
  enum Source : uint8_t {
    kSrcBase = 0, kSrcTree, kSrcSiteTree, kSrcCactus, kSrcStamp, kSrcDoor, kSrcDungeon, kSrcLayer,
    kSrcCount
  };
  static const char* SourceName(Source s);

  struct Options {
    // The low band of a tree that is an obstacle; the gate's attribution twin
    // sets it past any tree to compare WHOLE trees with the GPU.
    int treeBand = kTreeObstacleBandVox;
    bool doors = true;   // read door leaves as air (the policy above)
  };
  struct Stats {
    uint64_t columns = 0;        // columns modelled
    uint64_t columnNs = 0;       // wall time building them (a measurement, never an input)
    uint64_t columnNsMax = 0;
    uint64_t featureColumns = 0; // ...of which some feature reached (a span recorded)
    uint64_t asks = 0;           // asking-column evaluations (the pond scan)
    uint64_t treeTiles = 0;      // lattice tiles evaluated (tile-cache misses)
    uint64_t cactusTiles = 0;
    uint64_t chunks = 0;         // per-cell chunk records built (dungeon / layer)
    uint64_t dungeonCells = 0;   // dungeon cells evaluated (dungeon::TwinCell)
    uint64_t dungeonNs = 0;
    uint64_t epochs = 0;         // whole-cache drops
    uint64_t doors = 0;          // door boxes in force
  };

  explicit GenOverlay(DistantGround* ground);
  ~GenOverlay();
  // Drops everything; the next question rebuilds under the new options.
  void SetOptions(const Options& o);
  const Options& GetOptions() const { return opt_; }
  // Off = never decides anything (the D1 ground exactly; the cost arm's "before").
  void SetEnabled(bool on);
  bool Enabled() const { return enabled_; }
  // What carries weight (MobSystem's collision classes) and the material
  // table's GPU words (the dungeon twin reads klass / repose), and the seed.
  // Cheap when nothing changed.
  void Bind(const std::vector<uint32_t>* classOf, const std::vector<MaterialGpu>* gpu, uint32_t seed);
  void InvalidateAll();

  // THE QUESTIONS DistantGround ASKS.
  // Does the overlay decide cell (x, y, z)? True = `solid` is its answer
  // (and `src` which source decided); false = the base answers.
  bool Cell(int x, int y, int z, bool& solid, Source* src = nullptr);
  // Could any cell of column (x, z) in rows [yLo, yHi] be answered
  // DIFFERENTLY from the base? False = the column is the contract there and
  // DistantGround keeps its TerrainHeight fast path. (Conservative over a
  // dungeon chunk: true, and the per-cell path asks.)
  bool Touches(int x, int z, int yLo, int yHi);

  struct DoorBox {
    IVec3 lo{}, hi{};
    std::string id;   // the door's ref id ("harrowby/smithy/door_front_0")
  };
  // The door leaves in force (built per epoch from the current map's
  // structure sites and their assets' door slots).
  const std::vector<DoorBox>& Doors();
  const Stats& GetStats() const { return stats_; }

 private:
  struct Span {
    int32_t lo = 0, hi = 0;
    uint16_t mat = 0;     // the material that decided (0 = air)
    uint8_t solid = 0;
    uint8_t src = 0;
  };
  struct Col {
    uint32_t first = 0;   // into Block::spans
    uint16_t n = 0;
    uint8_t built = 0;
    uint8_t differs = 0;  // some span answers unlike the base
    int32_t dLo = 0, dHi = -1;   // the y range of those spans
  };
  struct Block {
    Col col[256];
    std::vector<Span> spans;
    uint64_t lastUse = 0;
    // Can a per-cell record (a dungeon's box, the layer) exist anywhere in
    // this chunk column? If not, no chunk is ever asked about here.
    bool mayChunk = false;
  };
  struct Chunk {
    bool layer = false;           // the edit layer writes cells here
    bool dungeon = false;         // a dungeon's index cell lists prims here
    uint64_t ldef[64] = {}, lsolid[64] = {};   // layer cells (bit = chunk-linear index)
    uint64_t lcolDiff[4] = {};    // columns with a layer cell unlike the base
    uint64_t dEval[64] = {}, dSolid[64] = {};  // dungeon cells evaluated / their answer
    uint64_t dTouched[64] = {};   // ...and whether dgCell changed the terrain word there
    uint64_t lastUse = 0;
  };
  struct TreeTileE {
    wgtwin::Tree t;
    uint64_t lastUse = 0;
  };
  struct CactusTileE {
    wgtwin::Cactus c;
    uint64_t lastUse = 0;
  };

  void CheckEpoch();
  void DropAll();
  void TrimBlocks();
  void TrimChunks();
  Col& ColumnOf(int x, int z);
  void BuildColumn(Block& b, Col& c, int x, int z);
  const wgtwin::Tree& TreeTile(int tx, int tz);
  const wgtwin::Tree& SiteTree(uint32_t sid);
  const wgtwin::Cactus& CactusTile(int tx, int tz);
  Chunk& ChunkOf(IVec3 wc);
  // The column model's span at y, or null.
  const Span* SpanAt(const Col& c, const Block& b, int y) const;
  bool Supports(uint32_t mat) const;
  void BuildDoors();
  void BuildDungeonWords();
  void BuildLayer();

  DistantGround* ground_ = nullptr;
  Options opt_;
  bool enabled_ = true;
  const std::vector<uint32_t>* classOf_ = nullptr;
  size_t classOfSize_ = 0;
  const std::vector<MaterialGpu>* gpu_ = nullptr;
  size_t gpuSize_ = 0;
  uint32_t seed_ = 0;
  // The epoch every cached answer is for.
  uint64_t mapSerial_ = ~0ull, envSerial_ = ~0ull, layerGen_ = ~0ull;
  uint32_t vegetation_ = ~0u;
  uint32_t epochSeed_ = ~0u;
  bool epochValid_ = false;

  uint64_t useClock_ = 0;
  std::unordered_map<uint64_t, Block> blocks_;
  std::unordered_map<uint64_t, Chunk> chunks_;
  std::unordered_map<uint64_t, TreeTileE> treeTiles_;
  std::unordered_map<uint32_t, wgtwin::Tree> siteTrees_;
  std::unordered_map<uint64_t, CactusTileE> cactusTiles_;
  // One-entry memos: the last block and chunk asked.
  uint64_t memoBlockKey_ = ~0ull;
  Block* memoBlock_ = nullptr;
  uint64_t memoChunkKey_ = ~0ull;
  Chunk* memoChunk_ = nullptr;

  bool doorsBuilt_ = false;
  std::vector<DoorBox> doors_;
  bool dgBuilt_ = false;
  std::vector<uint32_t> dg_;              // a worldMap-shaped vector: header + the dungeon block
  std::vector<MaterialDef> twinMats_;     // klass / repose for dungeon::TwinCell
  bool layerBuilt_ = false;
  std::unique_ptr<sandvox::WorldEdits> layer_;   // the resolved private copy (null = none)
  std::unordered_set<uint64_t> layerCols_;       // the chunk columns it writes in
  std::vector<uint32_t> scratch_;
  Stats stats_;
};
