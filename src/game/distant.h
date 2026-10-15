#pragma once
// distant.h — THE DISTANT TIER: creatures that keep existing past the window
// (docs/PLAN_distant_npcs.md; this file is package D0, "the seam").
//
// THREE TIERS, AND THE BORDERS BETWEEN THEM (§2.1)
//
//   NEAR     MobSystem::mobs_      a whole Mob: rig, Jolt, bricks, every pass
//   DISTANT  DistantMobs (here)    id + brain + the record + an overlay; no
//                                  rig, no physics, no matter, NO OPS
//   PARKED   ChunkStore buckets    the record as bytes; nothing ticks
//
//   near -> distant  DEMOTE. MobSystem::PreTick's park rule (origin more than
//                    kPad outside the window), for a non-ghost the machine
//                    owns, when `distant.enabled`, within `distant.radius` of
//                    the window centre and with room in the tier. The id, the
//                    ai::Brain and the steer state MOVE here; the SaveOne
//                    record is kept as the bridge for everything this tier
//                    does not simulate (lattices, gear, pack, wounds, burns);
//                    then the rig is released exactly as parking releases it.
//                    THE DEAD DEMOTE TOO (audit A11): a corpse is a dead
//                    DistantMob, asleep forever, costing nothing.
//   distant -> near  PROMOTE. Inside the window by kUnparkMarginChunks (the
//                    PROMOTION LINE), with Unpark's three waits (budget, the
//                    living cap, ground known in the fetch cache), nearest to
//                    the window centre first, ties by id:
//                    MobSystem::PromoteDistant = PatchMobRecord -> LoadOne(...,
//                    keepId) -> the brain and steer state moved back ->
//                    Mob::ApplyDistantOverlay. Served by EntityParking::Unpark,
//                    which runs INSIDE TickAuthority's phase H now (§2.1), in
//                    one place and one budget with the parked records.
//   distant -> park  RETIRE. Past radius + hysteresis: the PATCHED record goes
//                    to the region bucket through the park function, the very
//                    record parking writes. Id and brain are dropped (today's
//                    rule for a parked creature).
//
// THE OVERLAY IS ALL THIS TIER OWNS (§2.2). Origin, heading, per-limb hp,
// dead + cause (+ the ref id, which the dungeon spawner renames on a corpse).
// Exactly two functions carry it: MobSystem::PatchMobRecord (retire, save,
// and the promotion's LoadOne input) and Mob::ApplyDistantOverlay (after the
// LoadOne). The round trip is the D0 contract and `distant-roundtrip` holds
// it: promote(demote(m)) re-saves byte for byte as m did, with the same id,
// the same brain and every limb's hp; save(demote(m)) loads the same parked
// record as save(m).
//
// D0 CREATURES ARE FROZEN: nobody walks, thinks or fights at this tier yet
// (D1 walks, D2b fights). The fields those packages will drive are here,
// marked with their package, and are written by nobody in D0.
//
// RULES (CLAUDE.md, §2.11-2.12):
//   1. Deterministic. The container is a vector SORTED BY ID and every walk is
//      in that order; tier changes are decided inside the authority tick from
//      tick-start state; ground is a pure function of (seed, map); any
//      randomness a later package adds is Hash3(id ^ salt, tick, i).
//   2. Cost scales with activity. A frozen creature costs nothing per tick:
//      the retire scan runs when the window moves (or the tier changed), the
//      promotion scan when Unpark's own "moved / pending" says so or the tier
//      flags a scan (PromoteScanDue / RetireScanDue), never every tick.
//   3. No ops. Distant creatures emit nothing into the world, by construction.
//
// Not hashed, never saved as itself: a save writes each distant creature as
// its patched record (persist.cpp 'MOBS' saveRecords, §2.10), so a save is
// "everyone distant parks".

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "game/ai_behavior.h"  // ai::Brain
#include "game/anim.h"         // LocomotionDef
#include "game/steer.h"        // SteerState
#include "math3d.h"

// ---- the ground a distant creature stands on (§2.4) -------------------------
//
// Decided ONCE, at demotion. SURFACE walks on DistantGround (D1); ANCHORED
// thinks and turns but never drives and its y never changes (a dungeon's
// skeletons stay in the dungeon, a guard on a wall stays on the wall), and it
// promotes where it stood.
enum class DistantGround : uint8_t { Surface = 0, Anchored = 1 };
const char* DistantGroundName(DistantGround g);

// SURFACE when the feet (the origin's y: Mob::origin_ is the body box's min
// corner, and the feet stand on it) are within `surfaceSnapMaxVox` of the
// standing height over World::TerrainHeight at the footprint's centre column,
// AND that cell is outside every placed dungeon's world box of the current map
// (worldmap::CurrentWorldMap().dungeons, boxMin/boxMax inclusive). ANCHORED
// otherwise. Pure: TerrainHeight is a function of (seed, map) and the boxes
// are the map's. `gapOut` (optional) = feet minus standing height, voxels.
DistantGround ClassifyDistantGround(Vec3 origin, Vec3 worldSize,
                                    float surfaceSnapMaxVox, uint32_t seed,
                                    float* gapOut = nullptr);
// Is world cell `c` inside a placed dungeon's world box (current map)?
bool InPlacedDungeonBox(IVec3 c);

// ---- one limb, as the abstract body sees it (§2.2 limbRole) -----------------
//
// Cached at demotion, parallel to the RECORD's limbs (the order Mob::SaveOne
// writes them: every base limb, then each appended slot that still has a
// body). Enough for D2b to sample a limb by role: what kind of limb it is (the
// rig's own tag: "arm", "leg", ... -- MobLimbDef::tag), and the flags below.
enum DistantLimbFlag : uint8_t {
  kDistantLimbAttached = 1u << 0,   // a body at demotion (else severed)
  kDistantLimbVital = 1u << 1,      // MobLimbDef::vital: at 0 the creature dies
  kDistantLimbRoot = 1u << 2,       // the def's root limb
  kDistantLimbSeverable = 1u << 3,  // MobLimbDef::severable
  kDistantLimbBloodless = 1u << 4,  // Mob::IsBloodless (hair, a worn shell, an item)
  kDistantLimbWorn = 1u << 5,       // a worn shell slot
  kDistantLimbHeld = 1u << 6,       // a held item slot
};
struct DistantLimbRole {
  uint8_t flags = 0;
  std::string tag;   // MobLimbDef::tag ("" when the rig gives it none)
};

// ---- what the tier owns on top of the record (§2.2) -------------------------
//
// The ONLY fields PatchMobRecord writes into a record and ApplyDistantOverlay
// applies to a loaded creature. Built from a DistantMob by DistantMob::Overlay.
struct DistantOverlay {
  Vec3 origin{};        // Mob::origin_ (body box min corner), world voxels
  float heading = 0.0f; // Mob::heading_, radians
  // The body box (MobDef::worldSize). PatchMobRecord turns the record's limb
  // transforms with the heading about the box's vertical centre line and
  // carries them with the origin, so a record that moved still places its
  // limbs round the new origin (an unpark and a promotion both place limbs).
  Vec3 worldSize{};
  // Per RECORD limb (DistantMob::limbHp), exactly MobLimb::hp.
  std::vector<float> limbHp;
  bool dead = false;
  std::string cause;     // the death cause BY NAME (Mob::DeathCause), "" alive
  uint64_t deathSeq = 0; // the death order; 0 = "youngest" on load
  std::string refId;     // Mob::RefId: the dungeon spawner renames a corpse's
};

// ---- one distant creature (§2.2) --------------------------------------------
struct DistantMob {
  // ---- identity (§2.3): the NEAR id, kept; nobody else can be issued it ----
  uint64_t id = 0;
  // The def BY NAME (the record's first field): LoadOne re-resolves it at
  // promotion. `defIndex` is only a HINT into MobSystem::Defs(), valid while
  // that slot still carries this name -- the tier does not hold its def
  // (runtime defs are evicted and their slots reused, and a distant crowd of
  // pool humans must not pin the near crowd's runtime slots and pool words),
  // which is why everything the tier needs from the def is CACHED below.
  std::string defName;
  int defIndex = -1;
  std::string refId;   // Mob::RefId at demotion (renamed by SetDistantRefId)

  // ---- the bridge (§2.2): everything this tier does not simulate ----------
  std::vector<uint8_t> record;   // Mob::SaveOne at demotion, MobSystem::kSaveVersion

  // ---- the body, as this tier holds it -------------------------------------
  Vec3 origin{};       // world voxels, Mob::origin_'s frame (body box min corner)
  Vec3 vel{};          // voxels/sec at demotion (planar drive + fall). D1: the
                       // mid-air landing reads it; frozen in D0
  SteerState steer;    // the facing lives HERE (steer.heading); SteerStep (D1)
  ai::Brain brain;     // MOVED in at demotion, moved back at promotion
  DistantGround ground = DistantGround::Surface;
  uint32_t demotedTick = 0;

  // ---- cached from the def / rig at demotion (no def lookups past the edge)
  Vec3 worldSize{};    // MobDef::worldSize
  float speed = 0.0f;  // MobDef::speed, voxels/sec (SteerStep's defSpeed)
  LocomotionDef loco;  // the rig's own (Mob::skel_.loco): turn + terrain budgets

  // ---- health: the abstract body (§2.2, §2.6) -----------------------------
  // PER LIMB, parallel to the record's limbs, exactly MobLimb::hp at
  // demotion (bloodless kept, unspent). Severed limbs carry their record hp.
  std::vector<float> limbHp;
  std::vector<DistantLimbRole> limbRole;
  // D2b: written by nobody in D0.
  float bloodLossPerTick = 0.0f;  // cuts taken while distant; decays
  uint16_t burnTicksLeft = 0;     // a burning demotion keeps burning abstractly
  uint32_t strokeImpactTick = 0;  // an abstract swing in progress (0 = none)
  uint32_t strokeStyle = 0;

  // ---- life + sleep ---------------------------------------------------------
  bool dead = false;
  std::string cause;     // death cause by name (from the record, or D2b)
  uint64_t deathSeq = 0;
  uint32_t deathTick = 0;  // D2b: the tick it died distant (0 = demoted dead)
  // EVERY D0 creature is asleep (frozen). D1 wakes the living by neighbourhood
  // and `wakeTick`; a corpse is asleep forever.
  bool asleep = true;
  uint32_t wakeTick = 0;

  Vec3 Centre() const {
    return Vec3{origin.x + worldSize.x * 0.5f, origin.y + worldSize.y * 0.5f,
                origin.z + worldSize.z * 0.5f};
  }
  DistantOverlay Overlay() const;
};

// ---- the container ----------------------------------------------------------
class DistantMobs {
 public:
  // The tier's cap (§2.11, owner decision §5.1). No GPU sizing: draw slots
  // are D4's separate budget. When full, demotion falls back to parking and
  // the refusal is counted (Stats::refusedFull).
  static constexpr size_t kMaxDistantMobs = 512;

  // Monotonic over the container's life (never saved, never hashed). Every
  // count is ALSO added into DistantRunTotals() for build/last_run.json.
  struct Stats {
    uint64_t demoted = 0;        // near -> distant (living + dead)
    uint64_t demotedDead = 0;    // ...of which corpses
    uint64_t anchored = 0;       // ...of which classified ANCHORED
    uint64_t promoted = 0;       // distant -> near
    uint64_t promotedDead = 0;   // ...of which corpses
    uint64_t retired = 0;        // distant -> parked (past radius + hysteresis, or the switch off)
    uint64_t refusedFull = 0;    // a demotion that parked instead: the tier was full
    uint64_t outsideRadius = 0;  // a leaver that parked instead: past distant.radius
    uint64_t groundWaits = 0;    // a promotion left waiting: its ground not in the fetch cache
    uint64_t capWaits = 0;       // ...the living cap was full
    uint64_t budgetWaits = 0;    // ...this call's budget was spent
    uint64_t spawnWaits = 0;     // ...Spawn refused (rig / pool): kept, retried
    // Logged each time: a promotion refused for good (def gone, a ref with a
    // near body: dropped), a retire whose record would not patch, or a
    // demotion whose overlay did not round-trip (that one parks instead).
    uint64_t failed = 0;
    uint64_t retireScans = 0;    // rule 2's witness: retire scans actually run
    uint64_t promoteScans = 0;   // ...and promotion scans
    uint32_t peak = 0;           // the most creatures the tier ever held at once
  };

  size_t Count() const { return list_.size(); }
  size_t LivingCount() const;
  // The cap in force: kMaxDistantMobs, or a gate's smaller override.
  size_t Cap() const { return capOverride_ != 0 ? capOverride_ : kMaxDistantMobs; }
  bool HasRoom() const { return list_.size() < Cap(); }
  // TEST SEAM (distant-roundtrip's full-tier arm): a cap below
  // kMaxDistantMobs; 0 restores it. Never set by the game.
  void SetCapForTest(size_t cap) { capOverride_ = cap < kMaxDistantMobs ? cap : 0; }

  // Every creature, ASCENDING ID. The only order anything walks the tier in.
  const std::vector<DistantMob>& All() const { return list_; }
  DistantMob* Find(uint64_t id);
  const DistantMob* Find(uint64_t id) const;
  // The first creature (in id order) whose ref id is `refId`; null for "".
  DistantMob* FindByRef(const std::string& refId);
  const DistantMob* FindByRef(const std::string& refId) const;
  // Insert keeping id order. False (nothing inserted) on a duplicate id or a
  // full tier -- the caller decided room first, so either is a bug it logs.
  bool Insert(DistantMob&& d);
  // Take one out (promotion, retire, RemoveMob). False if absent.
  bool Erase(uint64_t id);
  // Remove `id` and hand it back (retire, which needs the record after).
  bool Take(uint64_t id, DistantMob& out);
  // Everything gone: MobSystem::Reset (the world they lived in is gone).
  // Stats are kept (they are a run's history, like parkedTotal_).
  void Clear();

  // THE PROMOTION CANDIDATES (§2.1), in the order they are SERVED: every
  // creature whose min corner (origin) lies in a chunk inside the window
  // `wo` (its origin, in chunks) by `marginChunks` on every face -- THE
  // PROMOTION LINE, the test a parked record passes (EntityParking::
  // kUnparkMarginChunks) -- nearest to the window centre first (squared
  // distance of the body box centre), ties by id. EntityParking::Unpark walks
  // exactly this list; `distant-roundtrip` checks the order.
  void PromotionCandidates(IVec3 wo, int marginChunks,
                           std::vector<uint64_t>& out) const;

  // Mutable access by POSITION in id order (a pass that writes every
  // creature: SetBehaviors' re-resolve, D1's drive). Never change `id`.
  DistantMob& At(size_t i) { return list_[i]; }
  const DistantMob& At(size_t i) const { return list_[i]; }

  // THE SCAN LATCHES (rule 2). Both border scans skip their walk when the
  // window has not moved and nothing was left waiting; anything ELSE that can
  // move a distant creature across a border must say so here:
  //   promote  EntityParking::Unpark's walk (a creature over the promotion
  //            line). Set by Insert (harmless in D0: a demoted creature is
  //            outside the window by the park pad) and by MarkMoved.
  //   retire   MobSystem::RetireDistant (a creature past radius + hysteresis).
  //            Set by MarkMoved only: an inserted creature is inside the
  //            radius by construction.
  // D1's drive calls MarkMoved whenever it moves a creature; each scan clears
  // its own latch.
  void MarkMoved() { promoteScanDue_ = retireScanDue_ = true; }
  bool PromoteScanDue() const { return promoteScanDue_; }
  void ClearPromoteScan() { promoteScanDue_ = false; }
  bool RetireScanDue() const { return retireScanDue_; }
  void ClearRetireScan() { retireScanDue_ = false; }

  const Stats& GetStats() const { return stats_; }
  // Bump one counter here AND in DistantRunTotals().
  void Note(uint64_t Stats::*field, uint64_t n = 1);

 private:
  std::vector<DistantMob> list_;   // sorted by id, strictly ascending
  size_t capOverride_ = 0;
  bool promoteScanDue_ = false;
  bool retireScanDue_ = false;
  Stats stats_;
  size_t LowerBound(uint64_t id) const;
};

// The whole process's distant-tier counters (every DistantMobs adds into it),
// for build/last_run.json's "distant" block: like the op stream's refusals,
// a demotion that parked for want of room is a number, not a missing NPC.
const DistantMobs::Stats& DistantRunTotals();
