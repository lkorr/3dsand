#pragma once
// distant_draw.h -- STATUES FOR CREATURES PAST THE WINDOW (docs/PLAN_distant_npcs.md
// §2.9, package D4a, 2026-10-15).
//
// A creature that leaves the residency window stops being a rig: its limbs are
// released (Mob::ReleaseRig) and the distant tier (game/distant.h, D0/D1) keeps
// only a record, a brain and a position. It still has to be SEEN -- a fight
// 60 m away must not be an empty field -- and the plan's answer is a FLIPBOOK
// STATUE: a handful of whole-body micro models (idle, two walk frames, a
// strike, lying) baked once from the creature's own limbs, posed by its own
// clips, merged into ONE lattice, downsampled, and drawn as one RenderOnlyBody
// (game/renderonly.h) that swaps between them by state and gait phase.
//
// THIS FILE IS PURE OF THE TIER. Its input is a plain list of DistantDrawItem;
// it knows nothing about DistantMobs, ground or combat. D1 connects the two.
//
// ---- THE API D1 CALLS ---------------------------------------------------------
//
//   DistantDraw::Bake(mob, set)    at DEMOTION, while the Mob STILL HOLDS ITS
//                                  RIG -- i.e. BEFORE Mob::ReleaseRig. It reads
//                                  the live limbs (bricks, blood, char, lost
//                                  parts) and the live skeleton; after
//                                  ReleaseRig there is nothing left to read.
//                                  Returns the look's key (store it on the
//                                  DistantMob) and the height the statue rests
//                                  on now (`restY`, see DistantBake).
//   DistantDraw::Items()           every frame: the list to draw (fill it, or
//                                  SetItems). Render-only; order does not matter.
//   DistantDraw::Produce(...)      the frame loop calls it (main.cpp); it culls,
//                                  orders, budgets and emits RenderOnlyBody.
//
// ---- THE THREE BUDGETS, IN THE ORDER THEY BITE ---------------------------------
//
//  1. POOL WORDS (kDistantPoseWordsMax). Statues share the micro-body pool with
//     every live body's gore clones (world.h kMicroBodyPoolWordsWorld). Baked
//     poses are kept on the CPU (a few KB each) and PACKED into the pool lazily,
//     the first frame one is drawn; packed poses live in an LRU under this many
//     words and the least recently drawn are released (MicroBodyReleaseShared)
//     to make room. A pose never takes the pool's last kDistantPoolReserveWords:
//     a statue that does not fit is a statue not drawn, never a wound that does
//     not show.
//  2. MODEL RECORDS (world.h kDistantPoseModelsMax): one shared record per
//     packed pose, part of the model-table sizing assert.
//  3. BODY SLOTS (world.h kMaxBodySlots). Render-only bodies are walked LAST
//     (game/bodyreg.h), and Produce is told how many slots the real bodies and
//     the arrows left. It fills them nearest-first, the living before the dead,
//     and counts the rest as STARVED. It can never take a near body's slot.
//
// Everything is counted (DistantDrawStats, and the process totals that
// build/last_run.json's `distantDraw` block prints). Nothing here is fatal.
//
// RENDER-ONLY: never hashed, never saved, no ops, no sim effect. Floats are
// fine. The bake reads the live rig; it writes nothing on it.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/renderonly.h"
#include "math3d.h"
#include "sim/world.h"

class Mob;
struct MobDef;
struct MicroBodySet;

// ---- the budgets ----------------------------------------------------------------

// Pool words the packed statue poses may hold at once: 1 MiW of the 8 MiW pool
// (an eighth). A human pose at 20 cells/m is ~3-4k words (~10k with a blood
// lattice), so this is ~100-300 packed poses -- every statue a 128 m radius can
// put on screen, several times over. The plan's number (§2.9, audit A7).
constexpr uint32_t kDistantPoseWordsMax = 1u << 20;
static_assert(kDistantPoseWordsMax <= kMicroBodyPoolWordsWorld / 4,
              "statues may hold at most a quarter of the micro-body pool");
// Never pack a statue pose that would leave the pool with less than this many
// free words: gore (MicroBodyOwn / MicroBodyClone) is the pool's first
// customer, and a statue is the thing that should go without.
constexpr uint32_t kDistantPoolReserveWords = 512u << 10;
// ...nor take the model table's last records, for the same reason.
constexpr uint32_t kDistantModelReserve = 256;
// Looks kept on the CPU (each ~5 poses of a few thousand cells, ~50-80 KB).
// Bounded because a look is per (def, gear, damage) and pool humans are each
// their own def: a long session demotes thousands. LRU by last use.
constexpr uint32_t kDistantLooksMax = 512;
// Poses packed per Produce call: a statue that appears costs one pack (~0.1
// ms), and a hundred appearing at once must not be a hundred in one frame.
// The rest draw a packed fallback or wait a frame (counted as `deferred`).
constexpr uint32_t kDistantPacksPerFrame = 8;
// Bare (def-only) looks baked on demand inside Produce, per call -- ~3-8 ms
// each, so one.
constexpr uint32_t kDistantDefBakesPerFrame = 1;

// The statue lattice: 20 cells per metre (5 cm), about one pixel per cell at
// 50 m on a 1080p, 60-degree view. As a micro-body scale that is
// kDistantCellsPerMetre / kVoxelsPerMetre micro voxels per world voxel.
constexpr int kDistantCellsPerMetre = 20;
static_assert(kDistantCellsPerMetre % kVoxelsPerMetre == 0,
              "the statue lattice must be a whole number of cells per voxel");
constexpr uint32_t kDistantStatueScale =
    (uint32_t)(kDistantCellsPerMetre / kVoxelsPerMetre);

// ---- poses, states, keys --------------------------------------------------------

enum class DistantPose : uint8_t { Idle = 0, WalkA, WalkB, Strike, Lying, Count };
constexpr int kDistantPoseCount = (int)DistantPose::Count;
const char* DistantPoseName(DistantPose p);

// What the creature is DOING, as far as the statue cares. The flipbook picks
// the pose: Walk alternates WalkA/WalkB on the gait phase, a dead item always
// lies (whatever its state).
enum class DistantDrawState : uint8_t { Idle = 0, Walk, Strike };

// WHICH STATUE. A look is (def, worn + held gear with dyes, a coarse damage
// signature): a dressed villager and a naked one of the same def get different
// statues, two clean guards in the same outfit share one, and a bloodied or
// one-armed creature gets its own. `pristine` and `bare` are the fallbacks the
// producer walks when `full` is not available (evicted, never baked, or no
// room to pack): the same gear undamaged, then the def alone. All three are
// 64-bit FNV hashes, so a key is plain data D1 can store on a DistantMob.
struct DistantLookKey {
  uint64_t full = 0;
  uint64_t pristine = 0;   // same def + gear, damage signature 0
  uint64_t bare = 0;       // the def alone (no gear, no damage)
  uint64_t def = 0;        // hash of MobDef::name, to find the def for a bare bake
  bool Valid() const { return full != 0; }
};

// What Bake returns.
struct DistantBake {
  DistantLookKey key;
  // The world y the statue's LOWEST CELL should sit at to reproduce where the
  // body is right now. Every pose is GROUNDED (its lowest cell at the item's
  // origin.y), so for a standing creature this is just Mob::Origin().y (its
  // feet), and for a CORPSE it is the lowest point of the body as it lies --
  // a corpse's origin_ follows its pelvis's rest corner (Mob::TickDeadLimp),
  // which is well below the ground when the body is flat. D1: use it as the
  // dead item's origin.y unless your ground says otherwise.
  float restY = 0.0f;
  bool baked = false;      // false: nothing drawable (no micro limbs at all)
};

// ONE STATUE TO DRAW THIS FRAME. Plain data: D1 fills a vector of these from
// its DistantMobs every frame.
struct DistantDrawItem {
  DistantLookKey look;
  DistantDrawState state = DistantDrawState::Idle;
  // Gait cycles, any real number: WalkA for frac in [0, 0.5), WalkB after.
  float gaitPhase = 0.0f;
  // The creature's origin in Mob::origin_'s frame (the prefab MIN corner,
  // world voxels), with y = the height the statue rests on (its feet; see
  // DistantBake::restY for a corpse). Heading as Mob::Heading() (radians about
  // +Y, 0 = +Z).
  Vec3 origin{};
  float heading = 0.0f;
  // Drawn after every living item, in the Lying pose.
  bool dead = false;
};

// The camera Produce culls and orders against. Fill from the frame's camera
// (main.cpp passes the jittered camera the body cull uses).
struct DistantView {
  Vec3 eye{};
  Vec3 fwd{0, 0, 1}, right{1, 0, 0}, up{0, 1, 0};
  float tanHalfFovY = 0.6f;
  float aspect = 16.0f / 9.0f;
  // False: no frustum cull (every item is "visible"); distance order still
  // applies. The gate's slot-accounting arm uses it.
  bool cull = true;
};

// Counters, per Produce call (`frame` fields) and cumulative (the rest).
struct DistantDrawStats {
  // ---- this frame ----
  uint32_t items = 0;       // items handed in
  uint32_t drawn = 0;       // RenderOnlyBody emitted
  uint32_t culled = 0;      // outside the frustum
  uint32_t starved = 0;     // visible, no body slot left (slotsFree ran out)
  uint32_t unbaked = 0;     // no look (or fallback) baked for the item at all
  uint32_t deferred = 0;    // baked, but its pose could not be packed this
                            // frame (per-frame pack cap) and no packed
                            // fallback existed
  uint32_t noFit = 0;       // baked, but no room under the word budget / pool
                            // reserve, and no packed fallback
  uint32_t fallbacks = 0;   // drawn as `pristine` or `bare` instead of `full`
  uint32_t slotsFree = 0;   // what the caller said was left
  // ---- state ----
  uint32_t looks = 0;           // CPU looks held
  uint32_t packedPoses = 0;     // poses resident in the pool
  uint32_t residentWords = 0;   // their words (<= the budget, always)
  // ---- cumulative ----
  uint64_t bakes = 0;           // Bake + BakeDef calls that produced poses
  uint64_t packs = 0;
  uint64_t evictions = 0;       // packed poses released to make room
  uint64_t lookEvictions = 0;   // CPU looks dropped at kDistantLooksMax
  uint64_t packRefused = 0;     // MicroBodyPack said no (counted, retried later)
  uint64_t epochDrops = 0;      // MicroBodySet replaced under us: state dropped
  uint64_t artAdded = 0;        // art palette entries statues added
  uint64_t artNearest = 0;      // colours drawn as the nearest existing entry
  double bakeMsMax = 0.0;
};

// Process-wide totals across every DistantDraw (the selftest's last_run.json
// `distantDraw` block; render-only, never hashed).
struct DistantDrawRunStats {
  uint64_t produces = 0, drawnTotal = 0, culledTotal = 0, starvedTotal = 0;
  uint64_t unbakedTotal = 0, deferredTotal = 0, noFitTotal = 0;
  uint64_t fallbacksTotal = 0;
  uint32_t drawnPeak = 0, residentWordsPeak = 0, packedPosesPeak = 0;
  uint32_t looksPeak = 0;
  uint64_t bakes = 0, packs = 0, evictions = 0, lookEvictions = 0;
  uint64_t packRefused = 0, epochDrops = 0, artAdded = 0, artNearest = 0;
  double bakeMsMax = 0.0;
};
const DistantDrawRunStats& DistantDrawRunTotals();

// ---- the cache and the producer ---------------------------------------------------

class DistantDraw {
 public:
  // `wordBudget` defaults to kDistantPoseWordsMax; the gate builds a small
  // one to exercise the LRU.
  explicit DistantDraw(uint32_t wordBudget = kDistantPoseWordsMax);
  ~DistantDraw();
  DistantDraw(const DistantDraw&) = delete;
  DistantDraw& operator=(const DistantDraw&) = delete;

  // BAKE (or top up) the look of a creature that still holds its rig. Call at
  // demotion, BEFORE Mob::ReleaseRig. A look that already exists is reused
  // as is -- except that a CORPSE replaces a look's Lying pose that was only
  // a rotated idle, since the real corpse pose is better. ~5-15 ms for a
  // human the first time a key is seen, ~0.1 ms after (the key only).
  // Also bakes the def's bare look if it is missing, so every look has a
  // fallback that needs no live creature.
  DistantBake Bake(const Mob& m, MicroBodySet& set);
  // The def alone, in its rest pose (no gear, no damage): the `bare` key.
  // What the test hook draws, and what Produce bakes on demand when an item's
  // looks have all been evicted (pass `defs` to Produce).
  DistantBake BakeDef(const MobDef& def, MicroBodySet& set);
  // The key Bake would use, without baking.
  DistantLookKey KeyOf(const Mob& m, const MicroBodySet& set) const;
  static DistantLookKey KeyOfDef(const MobDef& def);
  bool HasLook(uint64_t key) const { return index_.count(key) != 0; }

  // The items to draw. D1 rewrites this every frame (clear + push_back keeps
  // the capacity, so the steady state allocates nothing).
  std::vector<DistantDrawItem>& Items() { return items_; }
  void SetItems(const std::vector<DistantDrawItem>& items) { items_ = items; }

  // THE PRODUCER. Culls Items() against `view`, orders the visible ones
  // (living before dead, nearest first), and emits at most `slotsFree`
  // RenderOnlyBody into `out` (cleared first), packing poses into `set` as
  // needed under the word budget. `defs` (nullable) lets it bake a missing
  // bare look on demand. `outItems` (nullable) receives, parallel to `out`,
  // the index into Items() each body draws -- for the gate and for a caller
  // that wants to know who made it.
  //
  // Call it BEFORE Simulation::UploadMicroBodies for the frame: a pose packed
  // here dirties the set, and the upload is what puts it on the GPU before
  // the draw reads it.
  void Produce(MicroBodySet& set, const std::vector<MobDef>* defs,
               const DistantView& view, uint32_t slotsFree,
               std::vector<RenderOnlyBody>& out,
               std::vector<uint32_t>* outItems = nullptr);

  // Release every packed pose back to `set` and forget every look. For a
  // harness that is done (the gate), or a caller that wants the pool back.
  // Packed poses belonging to a DIFFERENT set (an epoch that has passed) are
  // dropped without being freed: their table is gone.
  void Reset(MicroBodySet& set);

  const DistantDrawStats& Stats() const { return stats_; }
  uint32_t WordBudget() const { return budget_; }

  // ---- introspection (the gate, the F1 panel later) ----
  struct PoseInfo {
    bool present = false;     // baked on the CPU
    bool fromCorpse = false;  // Lying taken from a real corpse
    uint32_t cells = 0;       // solid statue cells
    IVec3 dims{};             // the brick, in statue cells
    uint32_t words = 0;       // pool words it takes (exact once packed)
    int model = -1;           // packed model index, -1 = not resident
  };
  bool PoseOf(uint64_t key, DistantPose pose, PoseInfo& out) const;

  // The bake input -- limbs as source voxels plus how to pose them -- built
  // from a live Mob or from a MobDef's rest pose. Opaque; distant_draw.cpp.
  struct Source;

 private:
  // The two readers of a creature (Mob befriends DistantDraw for these: they
  // read the rig, the gear and the damage, and write nothing).
  static bool SourceFromMob(const Mob& m, const MicroBodySet& set, Source& src);
  static void KeyPartsOf(const Mob& m, const MicroBodySet& set, uint64_t& def,
                         uint64_t& gear, uint64_t& damage);

  struct Cell {
    uint16_t x, y, z;
    uint8_t mat;
    uint8_t pad = 0;
    uint16_t colour;   // index into Look::palette, 0 = unpainted
    uint16_t coat;     // body coat word (voxload.h PackBodyStain), 0 = clean
  };
  struct Pose {
    bool present = false;
    bool fromCorpse = false;
    bool anyCoat = false;
    IVec3 dims{};
    Vec3 minCorner{};   // the brick's min corner, PREFAB frame, grounded
    Vec3 centre{};      // the brick's centre, prefab frame (cull sphere)
    float radius = 0;
    std::vector<Cell> cells;
    // ---- the GPU half (lazy) ----
    int model = -1;
    uint32_t words = 0;         // estimate before packing, exact after
    uint64_t lastUsed = 0;      // frame
    uint64_t refusedUntil = 0;  // frame: a refused pack is not retried before
  };
  struct Look {
    uint64_t key = 0;
    uint64_t def = 0;
    Vec3 pivot{};                    // yaw pivot, prefab frame
    std::vector<uint32_t> palette;   // [0] unused; 0xAARRGGBB as artColors
    uint32_t bank = 0;               // the art bank most of its paint came from
    std::vector<uint8_t> art;        // palette -> art byte in `bank`, this epoch
    bool artResolved = false;
    Pose pose[kDistantPoseCount];
    uint64_t lastUsed = 0;
    std::string label;
  };
  struct Packed {
    uint32_t look;
    uint8_t pose;
  };

  void CheckEpoch(MicroBodySet& set);
  void DropAll(bool freeModels, MicroBodySet* set);
  void RefreshState();
  Look* Find(uint64_t key);
  const Look* Find(uint64_t key) const;
  uint32_t NewLook(uint64_t key, MicroBodySet& set);
  // Bake `src`'s poses into look `lookIdx`: every pose the look lacks, plus
  // Lying when `src` is a corpse and the look's Lying is only a rotated idle.
  void BakeInto(MicroBodySet& set, uint32_t lookIdx, const Source& src);
  int EnsurePacked(MicroBodySet& set, uint32_t lookIdx, int pose,
                   bool allowPack, bool* noFit);
  void Evict(MicroBodySet& set, uint32_t lookIdx, int pose);
  bool MakeRoom(MicroBodySet& set, uint32_t words);
  void ResolveArt(MicroBodySet& set, Look& lk);

  uint32_t budget_;
  uint64_t epoch_ = 0;
  uint64_t frame_ = 0;
  uint32_t resident_ = 0;
  uint32_t packsThisFrame_ = 0;
  uint32_t artAddedThisEpoch_ = 0;
  std::vector<Look> looks_;
  std::vector<uint32_t> freeLooks_;
  std::unordered_map<uint64_t, uint32_t> index_;
  std::vector<Packed> packed_;
  std::vector<DistantDrawItem> items_;
  DistantDrawStats stats_;
  // Produce scratch, reused so the steady state allocates nothing.
  struct Order {
    uint64_t sortKey;
    uint32_t item;
    uint32_t look;
    uint8_t pose;
    uint8_t which;   // 0 full, 1 pristine, 2 bare
  };
  std::vector<Order> order_;
  // def name hash -> def index into the last `defs` Produce saw.
  std::unordered_map<uint64_t, uint32_t> defIndex_;
  size_t defIndexFor_ = 0;
  const void* defIndexOf_ = nullptr;
};

// ---- the owner's look check without the tier ---------------------------------------
//
// SANDVOX_DISTANT_DRAW_TEST=<n>[:<def>[,<def>...]] in the windowed game: `n`
// statues of the named defs (default: the human) baked from each def's REST
// POSE (DistantDraw::BakeDef -- no creature is spawned, so nothing in the sim
// changes), stood in a ring 30..80 m around where the player was the first
// frame, a mix of idle, walking (the flipbook animating on the clock),
// striking and dead. Re-bakes after a materials reload. Render-only.
class DistantDrawTestHook {
 public:
  DistantDrawTestHook();   // reads the environment once
  bool Enabled() const { return count_ > 0; }
  // Rewrites dd.Items() with the ring (baking what is missing first).
  void Fill(DistantDraw& dd, const std::vector<MobDef>& defs,
            MicroBodySet& set, const Vec3& playerPos, double timeSec,
            uint32_t seed);

 private:
  int count_ = 0;
  std::vector<std::string> names_;
  bool haveCentre_ = false;
  Vec3 centre_{};
  std::vector<DistantLookKey> keys_;   // per def in names_ order
  std::vector<int> defOf_;
  std::vector<Vec3> spots_;            // per statue: x, ground y, z
};
