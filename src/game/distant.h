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
// D1 CREATURES WALK (MobSystem::TickDistant, distant.cpp): every awake one
// THINKS (ai::Think, with a WorldView of the distant creatures round it plus
// the near ones in the edge band -- never a player, §2.7), STEERS (SteerStep,
// the very function the near tier turns with) and DRIVES over DistantGround
// below, held at the promotion line while it waits; an idle one SLEEPS and
// costs nothing.
//
// D2b CREATURES FIGHT (the same TickDistant, an IMPACT PASS after the steps):
// Think's `attack` starts an abstract swing (distant::PickDistantStyle) that
// lands at the style's impact tick and is RESOLVED against the defender as it
// stood at the start of that tick (distant::ResolveStrike: the guard roll,
// the hit roll, the limb by role, the hp through the shell, the bleed), the
// outcomes APPLIED in attacker-id order; `shoot` draws a bow for the bow's
// own draw time and looses an abstract arrow (ResolveArrow at the loose,
// applied at its flight's impact tick); `cast` is HELD (owner decision 5.3).
// They bleed and burn every tick (StepBleed / StepBurn; awake while they do),
// lose a weapon from a hand too hurt to hold it (melee.injuredArmDrop), die by
// the near rules (DeathTest) and lie as dead DistantMobs -- asleep forever --
// until a promotion brings them back as corpses through the record's dead
// block. The kit and the statuses are on DistantMob (below).
//
// D3 THE EDGE (section 2.7; "a pair resolves at the lower fidelity of its two
// members, and damage lands through the receiver's own door"). The near tier
// and the distant one share a BAND kDistantEdgeBandVox wide outside the
// window: the distant creatures in it are published, as tick-start actors,
// into the near tier's actor list (MobSystem::PublishDistantBand), and the
// near CREATURES in it are in every distant actor list (TickDistant) -- never
// a player, local or remote, whatever the geometry. Across it:
//   * NEAR HITS DISTANT. The near stroke plays; at its first cutting tick a
//     stroke whose target is a distant creature queues a blow
//     (MobSystem::QueueEdgeBlow, from StepStroke), resolved at the top of the
//     next distant impact pass by the same roll (ResolveStrike) against the
//     defender's tick-start state, in attacker-id order.
//   * DISTANT HITS NEAR. The roll as for any distant swing; a hit enters the
//     near creature through its ORDINARY door (game/melee.h ApplyEdgeBlow:
//     ResolveOnCreature, the cover retarget onto a worn shell) at a contact
//     point on the struck limb's surface facing the attacker -- a real wound,
//     real blood, the real shell response. The ops are the near creature's.
//   * ARROWS BOTH WAYS. A distant archer's arrow at a near creature is flown
//     for REAL (ArrowFlights' own fixed-point integrator, DistantGround as the
//     ground) and becomes a real ArrowFlight where it crosses into the window
//     (MobArcheryTick drains them); an abstract arrow whose target was promoted
//     mid-flight converts the same way. A near archer's flight that leaves the
//     window at a distant creature is handed back (QueueEdgeArrowOut) and
//     resolved by ResolveArrow at the flight time it has left.
//   * THRASH. Demotion cannot be refused (a creature past the pad has no
//     ground), so PROMOTION is: a creature that changed tier kDistantMaxFlips
//     - 2 times in the last kDistantFlipWindowTicks waits at the promotion
//     line (Stats::flipWaits) until the oldest flip ages out, which bounds a
//     creature pacing the edge to kDistantMaxFlips flips per window.
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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <memory>
#include <unordered_map>

#include "game/ai_behavior.h"  // ai::Brain
#include "game/anim.h"         // LocomotionDef
#include "game/distant_combat.h"  // DistantKit, the abstract hit model (D2a, driven by D2b)
#include "game/distant_draw.h" // DistantLookKey, DistantDrawItem (the statues, D4a)
#include "game/steer.h"        // SteerState
#include "math3d.h"

class Stream;

// ---- the ground MODE a distant creature has (§2.4) ---------------------------
//
// Decided ONCE, at demotion. SURFACE walks on DistantGround (the source class
// below); ANCHORED thinks and turns but never drives and its y never changes
// (a dungeon's skeletons stay in the dungeon, a guard on a wall stays on the
// wall), and it promotes where it stood. (D0 named this enum `DistantGround`;
// D1 renamed it, because the plan's ground SOURCE is `DistantGround`.)
enum class DistantGroundMode : uint8_t { Surface = 0, Anchored = 1 };
const char* DistantGroundModeName(DistantGroundMode g);

// SURFACE when the feet (the origin's y: Mob::origin_ is the body box's min
// corner, and the feet stand on it) are within `surfaceSnapMaxVox` of the
// standing height over World::TerrainHeight at the footprint's centre column,
// AND that cell is outside every placed dungeon's world box of the current map
// (worldmap::CurrentWorldMap().dungeons, boxMin/boxMax inclusive). ANCHORED
// otherwise. Pure: TerrainHeight is a function of (seed, map) and the boxes
// are the map's. `gapOut` (optional) = feet minus standing height, voxels.
DistantGroundMode ClassifyDistantGround(Vec3 origin, Vec3 worldSize,
                                        float surfaceSnapMaxVox, uint32_t seed,
                                        float* gapOut = nullptr);
// Is world cell `c` inside a placed dungeon's world box (current map)?
bool InPlacedDungeonBox(IVec3 c);

// ---- THE GROUND A DISTANT CREATURE WALKS ON (§2.4; package D1) ---------------
//
// "Where can a body stand in column (x, z)?" -- answered ONLY from sources
// that are pure functions of (seed, map, the tick), never from a GPU readback
// whose arrival is scheduling-dependent (the far surface map would be better
// ground and would break rule 1):
//
//   1. BASE: World::TerrainHeight, the height contract (worldgen's ground,
//      roads, site pads, pond bowls, rivers). A cell is solid at or below it.
//      Trees, buildings, ruins and `.svedit` layers are NOT in it: a distant
//      creature walks through a house 60 m away (accepted for v1, §2.4).
//   2. WATER: the higher of TerrainColumn().water (tarns, the sea) and
//      worldmap::WaterColumnAt (rivers, lakes). Liquid is never ground: a
//      body in water stands on the BED; the walk decides to wade or swim.
//   3. EDITED GROUND: a chunk the player changed and the window then left
//      is in the ChunkStore, and where it is its cells ARE the ground (a
//      crater you dug and walked away from is real to a distant creature).
//      Read through Stream::SettledStoredChunk, which first completes any
//      eviction of that chunk still in flight -- the RegenerateChunks loop --
//      because when an eviction's bytes reach the store is up to the GPU
//      (audit A3). A chunk not in the store is procgen, i.e. source 1.
//
// "Walkable" is the plan's rule: the HIGHEST cell that carries weight (the
// near tier's own test: solid, or a powder of at least kPowderMobSupportMin)
// whose standing cell is within the body's step budget of its CURRENT feet
// and has `headroom` clear cells above it -- not the column top, because a
// stored chunk holds canopy and roofs too.
//
// TWO CACHES, both pure (a hit and a miss give the same answer, so neither
// can make two runs differ):
//   * base columns in 16 x 16 tiles: the height filled on first ask, the
//     water only when asked (it costs two more column evaluations);
//   * stored chunks, as a 4,096-bit "carries weight" mask (or "not stored"),
//     looked up only AFTER the chunk's eviction drain, and dropped by
//     ChunkStore's change listener on every Put / Erase / Clear -- so an
//     entry is never older than the bytes the store would hand out now.
// Both are dropped when the map, the seed or the material classes change.
//
// CPU only, single-threaded (the tick), never hashed, never saved.
class DistantGround {
 public:
  static constexpr int kNoWater = INT32_MIN;
  // The NavProbe's reach (ai_nav.h): how far below a probe's start the ground
  // query looks, and how far up it climbs out of matter it started inside.
  // Wider than the near probe's 2.4 m / 1.2 m on purpose: nothing at this
  // tier is unknown, so a column past the reach answers its real height and
  // the planner's own step rule refuses it, rather than "unknown = open".
  static constexpr int kProbeDown = 64;
  static constexpr int kProbeUp = 32;

  DistantGround();
  // The edited-ground source (null = TerrainHeight + water only: a gate's
  // CPU arm, a harness with no stream) and the material collision classes
  // that say what carries weight (MobSystem's `classOf_`). Cheap when
  // nothing changed; rebinding a different stream drops the chunk cache and
  // installs this object's listener on the new store (ONE listener per
  // store: ChunkStore::SetChangeFn).
  void Bind(Stream* stream, const std::vector<uint32_t>* classOf, uint32_t seed);
  // Everything cached, gone (a materials reload, a world reset, a gate that
  // swapped the map by hand).
  void InvalidateAll();

  // TerrainHeight(x, z): the top solid cell of the pristine column.
  int TerrainTop(int x, int z);
  // The water surface over (x, z) -- the y of the first cell above the
  // liquid -- or kNoWater.
  int WaterTop(int x, int z);
  // Does cell (x, y, z) carry weight?
  bool Solid(int x, int y, int z);

  // THE WALK'S QUESTION. The standing height (the feet's y, one above the
  // supporting cell) in column (x, z) for a body whose feet are at `fromY`:
  // the highest support cell c with c + 1 in [fromY - stepDown, fromY +
  // stepUp] and cells c + 1 .. c + headroom clear. False = there is no such
  // place (a wall past the climb budget, a drop past the step-down one, no
  // headroom). `edited` (optional): the answer came from stored cells.
  bool Stand(int x, int z, int fromY, int stepUp, int stepDown, int headroom,
             int& outY, bool* edited = nullptr);
  // THE PLANNER'S QUESTION (ai_nav.h NavProbe::ground): the first standing
  // height scanning DOWN from `yFrom`, escaping UP (kProbeUp) when `yFrom`
  // is inside matter. Never "unknown" at this tier: past the reach it
  // answers the real column, and A*'s StepTo refuses what is too far.
  bool ProbeGround(int x, int z, int yFrom, int& outY);
  // A LANDING: the first support at or below `fromY - 1` within `maxDown`
  // (the feet land on c + 1), or -- feet inside matter -- the first clear cell
  // within `maxUp` above. False = nothing within the bounds.
  bool Land(int x, int z, int fromY, int maxUp, int maxDown, int& outY);

  struct Stats {
    uint64_t columnEvals = 0;   // TerrainHeight calls (cache misses)
    uint64_t waterEvals = 0;    // water evaluations (cache misses)
    uint64_t chunkDecodes = 0;  // stored chunks decoded into a mask
    uint64_t chunkAbsent = 0;   // lookups that found no stored chunk
    uint64_t drains = 0;        // evictions this reader had to complete
    uint64_t invalidations = 0; // chunk entries dropped by the listener
    uint64_t flushes = 0;       // whole-cache drops (map, seed, classes, Clear)
  };
  const Stats& GetStats() const { return stats_; }
  // The stream this reads (null when unbound).
  Stream* BoundStream() const { return stream_; }

 private:
  struct Tile {
    int32_t h[256];
    int32_t water[256];
    uint64_t haveH[4] = {0, 0, 0, 0};
    uint64_t haveW[4] = {0, 0, 0, 0};
    uint64_t lastUse = 0;
  };
  struct Chunk {
    bool stored = false;
    uint64_t solid[kChunkVol / 64] = {};   // bit (lz*16 + ly)*16 + lx
    uint64_t lastUse = 0;
  };
  // What the store's listener writes and Absorb drains: shared, so a store
  // that outlives this object (or the reverse) never calls into freed memory.
  struct Inbox {
    std::vector<uint64_t> changed;   // packed chunk keys
    bool all = false;
  };

  Tile& TileOf(int x, int z);
  void Absorb();          // apply the listener's inbox to the chunk cache
  void CheckEpoch();      // the map serial / seed this cache answers for
  // The stored chunk `wc` after its drain, or null when it is not stored.
  const Chunk* StoredChunk(IVec3 wc);
  // Is any chunk of column (x, z) in cell rows [yLo, yHi] stored?
  bool AnyStored(int x, int z, int yLo, int yHi);
  // Could ANY chunk be stored (a store with chunks, a bound save dir, an
  // eviction in flight)? False = every column is procgen: the fast path.
  bool EditedPossible() const;
  void Trim();

  Stream* stream_ = nullptr;
  const std::vector<uint32_t>* classOf_ = nullptr;
  size_t classOfSize_ = 0;
  uint32_t seed_ = 0;
  uint64_t mapSerial_ = ~0ull;
  uint64_t useClock_ = 0;
  std::unordered_map<uint64_t, Tile> tiles_;
  std::unordered_map<uint64_t, Chunk> chunks_;
  std::shared_ptr<Inbox> inbox_;
  // Solid()'s one-entry memo: the last chunk asked, valid while gen_ holds.
  uint64_t memoKey_ = ~0ull;
  uint64_t memoGen_ = ~0ull;
  const Chunk* memoChunk_ = nullptr;
  bool memoStored_ = false;
  uint64_t gen_ = 0;      // bumped whenever a cached chunk entry is dropped
  std::vector<uint32_t> decodeScratch_;
  Stats stats_;
};

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
  Vec3 vel{};          // voxels/sec at demotion (planar drive + fall); the
                       // mid-air landing reads its y, then it is zeroed
  SteerState steer;    // the facing lives HERE (steer.heading); SteerStep
  ai::Brain brain;     // MOVED in at demotion, moved back at promotion
  DistantGroundMode groundMode = DistantGroundMode::Surface;
  uint32_t demotedTick = 0;

  // ---- cached from the def / rig at demotion (no def lookups past the edge)
  Vec3 worldSize{};    // MobDef::worldSize
  float speed = 0.0f;  // MobDef::speed, voxels/sec (SteerStep's defSpeed)
  LocomotionDef loco;  // the rig's own (Mob::skel_.loco): turn + terrain budgets
  // D1: the body's terrain budgets IN CELLS, resolved by the rig once
  // (Mob::StepUpCells & co., clamped by its own leg span) -- the drive and
  // the planner refuse the same wall here as they do near (ai_behavior.h
  // SelfView), so a crossing of the edge cannot change what a wall is.
  int stepUpCells = 2, stepDownCells = 5, headroomCells = 3;
  bool aquatic = false;    // MobDef::swim.present: a fish, CONFINED to water
  bool canFloat = false;   // MobSystem::CanSwimAfloat: swims deep water
  bool armed = false;      // SelfView::armed / canGuard / ammo, as they were
  bool canGuard = false;   //   at demotion (D2b's kit replaces these)
  int ammo = 0;
  // BodyFacts' denominator (the def's hp over the base limbs that bleed) and
  // the base-limb count of the record, for SelfView::hpFrac / limbsLost.
  float hpFull = 0.0f;
  int baseLimbs = 0;

  // ---- D1: the walk, as this tier keeps it ---------------------------------
  // The afloat Schmitt pair's state (game/mob_afloat.cpp kAfloatEnter /
  // kAfloatLeave): a walker in water over its chin swims.
  bool swimming = false;
  // Demoted while AIRBORNE (thrown, falling off the window's bottom face) and
  // SURFACE: the first step lands it on DistantGround with fall damage from
  // the drop and `vel.y`, by Mob::ApplyFallDamage's thresholds (section 2.4).
  bool landPending = false;
  uint32_t lastStepTick = 0;   // the stride's clock (0 = never stepped)
  uint32_t lastMoveTick = 0;   // the last step that moved it (the statue walks)
  float gaitPhase = 0.0f;      // gait cycles walked (the statue's WalkA / WalkB)
  float walked = 0.0f;         // planar voxels walked while distant (diagnostic)
  uint32_t wakes = 0;          // times woken from sleep (diagnostic, the gates)
  uint32_t steps = 0;          // times stepped (diagnostic)
  // It MOVED while distant (a step, a landing, a gate's Relocate): its y is
  // DistantGround's now, not the real ground the near tier left it on, so its
  // promotion snaps it onto the real ground (MobSystem::SnapPromotionGround).
  // One that never moved stands where the near tier stood it -- outside the
  // window, where nothing can have been edited since -- and comes back there
  // byte for byte (D0's round-trip contract).
  bool movedWhileDistant = false;
  // The spatial hash cell this creature is filed under (DistantMobs::kCellVox
  // cells of its body CENTRE, x / z). Maintained by DistantMobs; read only.
  int32_t cellX = 0, cellZ = 0;

  // ---- D1 + D4a: the statue -------------------------------------------------
  // The baked look (DistantDraw::Bake at demotion, before the rig went) and
  // the height a CORPSE's statue rests on (DistantBake::restY: a corpse's
  // origin follows its pelvis, which is below the ground when it lies flat).
  DistantLookKey look;
  float statueRestY = 0.0f;

  // ---- health: the abstract body (§2.2, §2.6) -----------------------------
  // PER LIMB, parallel to the record's limbs, exactly MobLimb::hp at
  // demotion (bloodless kept, unspent). Severed limbs carry their record hp.
  std::vector<float> limbHp;
  std::vector<DistantLimbRole> limbRole;

  // ---- D2b: THE ABSTRACT FIGHTER (section 2.6; game/distant_combat.h) -----
  // THE KIT, built from the live rig at demotion BEFORE ReleaseRig
  // (distant::BuildDistantKit): the styles it can swing and what each lands
  // with, what it holds up (guard class), its BASE limbs' roles / vitality /
  // authored hp / the shell over each, its bow. The tier's own copy -- a grip
  // that fails or an archer's sidearm edits it (RearmHeld). Index-parallel to
  // the first kit.limbs.size() entries of `limbHp` (the record writes every
  // base limb first). `hasKit` false: a corpse, or a body with no def.
  distant::DistantKit kit;
  bool hasKit = false;
  // THE FISTS (Mob::GripFails' rule, melee.injuredArmDrop): per hand
  // (HandIndex), the base limbs of the arm chain that serves it (Mob::
  // HandCondition's parts), what it held at demotion, and the fist it punches
  // with once it is empty (Mob::HandFist; its reach is the chain's live bone
  // length, MobSystem::StyleReachOn's fist term).
  struct DistantHand {
    int16_t parts[4] = {-1, -1, -1, -1};
    bool weapon = false;     // a weapon (not a shield) in this fist
    bool shield = false;     // a shield in it
    bool fist = false;       // a usable natural fist (Mob::NaturalWeaponUsable)
    StrikeProfile weaponStrike;
    std::string weaponName;
    StrikeProfile fistStrike;
    std::string fistName;
    float fistReach = 0.0f;
  };
  DistantHand hands[2];
  // THE SIDEARM (game/mob_archery.cpp DrawSidearm): the weapon in an archer's
  // sheath, drawn into the bow hand when the last arrow is loosed.
  bool haveSidearm = false;
  StrikeProfile sidearmStrike;
  std::string sidearm;
  float sidearmReach = 0.0f;
  // THE BOW's full draw in ticks (ItemDef::BowSpec windupTicks + drawTicks):
  // a shoot request's loose comes this long (x the profile's `draw`, plus its
  // hold) after it, as MobArcheryTick's BeginDraw times it. `bowHand` is the
  // fist that holds it (HandIndex; -1 = no bow).
  int bowWindupTicks = 0, bowDrawTicks = 0;
  int8_t bowHand = -1;

  // ---- the statuses (distant::DistantVitals' fields, carried here) --------
  float bloodLossPerTick = 0.0f;  // the bleed RATE, hp/tick; decays by bleed.decay
  uint16_t burnTicksLeft = 0;     // > 0 while the body burns (abstractly)
  float burnFrac = 0.0f;          // burnt surface fraction (Mob::BurnFraction)
  // Open stumps: limbs SEVERED at this tier (distant::SeverLimb), each
  // draining distant::SeverCalib::stumpHpPerTick until death. The limbs
  // themselves are marked off in kit.limbs[i].alive and in limbRole's
  // kDistantLimbAttached flag; Mob::ApplyDistantOverlay severs them for real
  // at promotion (the near stump then bleeds on by its own rule).
  uint8_t stumps = 0;
  // The last tick its statuses were stepped (0 = never). A creature whose
  // only status is open stumps may SLEEP (its wake clock is the tick it bleeds
  // out); when it next wakes, the ticks it slept through are paid in one sum
  // (distant::StepStumps), so sleeping changes when the cost is paid, not
  // what it is.
  uint32_t statusTick = 0;
  // The base limb the latest cut landed on: a promotion that is still
  // bleeding re-opens its owed blood THERE (Mob::ApplyDistantOverlay).
  int16_t lastWoundLimb = -1;
  // ---- a stroke in flight (PickDistantStyle at the request) ----------------
  uint32_t strokeImpactTick = 0;  // the tick it lands (0 = none pending)
  uint32_t strokeStyle = 0;       // index into kit.styles
  uint32_t strokeStartTick = 0;
  uint32_t strokeEndTick = 0;     // busy (no new swing, no draw) until this tick
  uint64_t strokeTarget = 0;
  // ---- a bow drawn (MobArcheryTick's BeginDraw, abstractly) ---------------
  uint32_t drawLooseTick = 0;     // the tick it looses (0 = not drawing)
  uint32_t drawStartTick = 0;
  uint64_t drawTarget = 0;
  // How it died at this tier (distant::DistantDeath; 0 = alive or demoted dead).
  uint8_t deathKind = 0;

  // ---- life + sleep ---------------------------------------------------------
  bool dead = false;
  std::string cause;     // death cause by name (from the record, or D2b)
  uint64_t deathSeq = 0;
  uint32_t deathTick = 0;  // D2b: the tick it died distant (0 = demoted dead)
  // SLEEP (section 2.11): an asleep creature is not stepped at all. A living
  // one falls asleep when nothing hostile is within its keep range + a
  // margin, it has no target, no threat and no goal to walk to; it is woken
  // by a creature ENTERING its 3x3 hash neighbourhood, or at `wakeTick` (its
  // wander pause ends; 0 = no clock). A CORPSE is asleep forever. A demotion
  // arrives AWAKE, so it decides once before it may sleep.
  bool asleep = true;
  uint32_t wakeTick = 0;

  Vec3 Centre() const {
    return Vec3{origin.x + worldSize.x * 0.5f, origin.y + worldSize.y * 0.5f,
                origin.z + worldSize.z * 0.5f};
  }
  DistantOverlay Overlay() const;
};

// ---- an arrow in the air between two distant creatures (section 2.6) --------
//
// RESOLVED AT THE LOOSE (distant::ResolveArrow: the hit roll, the limb, the
// hp, against the target as it stood at the start of that tick) and APPLIED
// at `impactTick` (ApplyArrow), when it is still a distant creature. `from` /
// `to` are the two body centres at the loose: render-only, the arc a statue
// archer's arrow is drawn on (AppendDistantArrowBodies). Never hashed, never
// saved (a save loses an arrow in the air, as a near save does).
//
// D3: an INBOUND flight (`inbound`) is one aimed at a NEAR creature (or an
// abstract one whose target was promoted mid-flight): no roll, a ballistic
// state in ArrowFlights' own units (24.8 fixed world voxels, velocity per
// tick) stepped by the same integrator (game/arrows.h ArrowStepOutside) over
// DistantGround until it crosses into the window -- where it becomes a real
// ArrowFlight that hits or misses physically.
struct DistantArrowFlight {
  uint64_t shooter = 0, target = 0;
  uint32_t looseTick = 0, impactTick = 0;
  Vec3 from{}, to{};
  std::string ammo;              // the item drawn in flight (its micro model)
  distant::ArrowOutcome out;
  bool inbound = false;
  int32_t pos[3] = {0, 0, 0};    // inbound: the point, 24.8 fixed world voxels
  int32_t vel[3] = {0, 0, 0};    // inbound: per tick, 24.8 fixed
  uint32_t seed = 0;             // inbound: the real flight's soak seed
  int life = 0;                  // inbound: ticks flown (ArrowFlights::kLifeTicks bounds it)
};

// ---- THE EDGE's three hand-overs (section 2.7, D3) ---------------------------
//
// A NEAR stroke at a DISTANT creature, queued at the stroke's first cutting
// tick (MobSystem::QueueEdgeBlow) and resolved at the top of the next distant
// impact pass, in attacker-id order, against the defender's tick-start state.
// The attacker's kit is taken at the cut (its styles only: the reach, the
// strike profile and the table row the roll reads).
struct NearEdgeBlow {
  uint64_t attacker = 0, target = 0;
  uint32_t tick = 0;
  Vec3 from{};                   // the attacker's body centre at the cut
  distant::DistantKit kit;
  int style = -1;                // index into kit.styles
};
// An INBOUND arrow crossing into the window this tick: what MobArcheryTick
// turns into a real ArrowFlight (ArrowFlights::Inject), in the order queued.
struct DistantArrowIn {
  uint64_t shooter = 0, target = 0;
  uint32_t tick = 0;
  int32_t pos[3] = {0, 0, 0};    // the first in-window sample, 24.8 fixed
  int32_t vel[3] = {0, 0, 0};    // per tick, 24.8 fixed
  uint32_t seed = 0;
  std::string ammo;
};
// A NEAR flight that left the window at a DISTANT creature (ArrowFlights::
// Tick, through MobSystem::QueueEdgeArrowOut): resolved by ResolveArrow at
// the top of the next distant impact pass over the WHOLE shot's distance (the
// table's knots are the shot's, `from` -> the target), landing after the
// flight time it has left (`exit` -> the target at `speed`).
struct NearArrowOut {
  uint64_t shooter = 0, target = 0;
  uint32_t tick = 0;
  Vec3 from{}, exit{};
  float speed = 0.0f;            // world voxels/s as it left
  float damage = 0.0f;           // the ammo's flat damage (ItemDef::ArrowSpec)
  std::string ammo;
};

// THE EDGE BAND's width (section 2.7): the longest near sight (the archer's
// 160) plus a margin. A near creature within this of the window's boundary is
// in every distant actor list that reaches it; a distant creature within this
// of the window (or inside it, waiting at the promotion line) is published to
// the near tier's.
constexpr float kDistantEdgeBandVox = 192.0f;
// ...and at most this many distant creatures a tick are published (nearest
// to the window first, ties by id, then in id order): Think walks the near
// crowd's list several times a step, and 64 near creatures each weighing a
// held-at-the-line crowd of 500 is the O(N^2) the distant lists cap away too.
constexpr size_t kDistantBandMax = 128;
// THE THRASH BOUND (section 2.7): at most kDistantMaxFlips tier changes per
// creature in any kDistantFlipWindowTicks. Enforced on PROMOTION (a demotion
// cannot wait), with room kept for the demotion that may follow it.
constexpr uint32_t kDistantFlipWindowTicks = 600;
constexpr int kDistantMaxFlips = 4;

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
    // ---- D1: the walk (MobSystem::TickDistant) ------------------------------
    uint64_t steps = 0;          // creature-steps run (think + steer + drive)
    uint64_t tickCalls = 0;      // TickDistant calls that stepped anybody
    uint64_t tickCallsAll = 0;   // TickDistant calls with the tier non-empty
    uint64_t tickNs = 0;         // wall time inside those calls (a MEASUREMENT,
                                 // never an input to anything)
    uint64_t sleeps = 0;         // creatures that fell asleep
    uint64_t wakesNeighbour = 0; // ...woken by a creature entering their cells
    uint64_t wakesTimer = 0;     // ...woken by their wakeTick
    uint64_t lineHolds = 0;      // steps refused at the promotion line (waiting)
    uint64_t climbRefusals = 0;  // steps refused: a rise past the step budget
    uint64_t dropRefusals = 0;   // ...a drop past it
    uint64_t waterRefusals = 0;  // ...a fish's step out of the water
    uint64_t swimSteps = 0;      // steps taken swimming (a walker afloat)
    uint64_t combatHolds = 0;    // attack / shoot / cast intents held (D2b)
    uint64_t landings = 0;       // mid-air demotions landed
    uint64_t landingHurt = 0;    // ...that billed fall damage
    uint64_t landingDeaths = 0;  // ...that killed
    uint64_t snaps = 0;          // promotions snapped onto the real ground
    uint64_t snapMisses = 0;     // ...that found no ground within the bound
    uint64_t snapErrQ = 0;       // the worst snap |dy| seen, in 1/16 voxel
    // ---- D2b: the fight (MobSystem::TickDistant's impact pass) --------------
    uint64_t strokes = 0;          // abstract swings started
    uint64_t strokesRefused = 0;   // attack / shoot intents that started nothing: the arm
                                   // busy, no style reaches, no bow or arrow
    uint64_t strokeHits = 0;       // ...landed (ResolveStrike: Hit)
    uint64_t strokeMisses = 0;     // ...Miss
    uint64_t strokeBlocked = 0;    // ...Blocked (the defender's guard roll)
    uint64_t strokeOutOfReach = 0; // ...the target stepped out of reach + slack
    uint64_t strokeNoTarget = 0;   // ...the target was dead or gone (a near one of the band: edgeFar*)
    uint64_t draws = 0;            // bows drawn
    uint64_t drawsAborted = 0;     // ...let down: the target gone at the loose
    uint64_t looses = 0;           // arrows loosed (ResolveArrow)
    uint64_t arrowHits = 0;        // ...that hit at their impact tick
    uint64_t arrowMisses = 0;      // ...that missed
    uint64_t arrowsLost = 0;       // ...whose target was no living creature in either tier at impact
                                   // (a near one converts: edgeArrowsConverted)
    uint64_t deaths = 0;           // creatures that died at this tier (landings excluded)
    uint64_t deathsVital = 0;      // ...a vital / root limb at zero
    uint64_t deathsBlood = 0;      // ...bled white
    uint64_t deathsBurns = 0;      // ...burnt past the cap
    uint64_t gripFails = 0;        // a weapon dropped from a hand too hurt to hold it
    uint64_t severs = 0;           // limbs cut off at this tier (with all under them)
    uint64_t sidearms = 0;         // an archer out of arrows drew its sidearm
    uint64_t bleedTicks = 0;       // creature-ticks of StepBleed
    uint64_t burnTicks = 0;        // creature-ticks of StepBurn
    uint64_t relit = 0;            // promotions that came back burning (IgniteLimb)
    uint64_t rebled = 0;           // promotions that came back bleeding
    uint64_t hpDealtMilli = 0;     // life hp the blows and arrows took, x1000
    // ---- D2b: WHERE THE TIME GOES (section 2.11), ns -- MEASUREMENTS, never an
    // input to anything. Inside TickDistant, summed over every call:
    uint64_t nsSetup = 0;          // the band, who steps, the snapshot
    uint64_t nsSelf = 0;           // per step: SelfView + the ground fan + water
    uint64_t nsWorld = 0;          // per step: the actor list (hash + band)
    uint64_t nsThink = 0;          // per step: ai::Think (A* included)
    uint64_t nsMove = 0;           // per step: steer + spacing + drive
    uint64_t nsCombat = 0;         // the impact pass + the statuses + deaths
    uint64_t nsSleep = 0;          // phase 2 (the sleep test)
    uint64_t nsFile = 0;           // phase 3 (refile, latches, summary)
    uint64_t stepsEngaged = 0;     // steps of creatures with a target / threat / swing
    uint64_t actorCaps = 0;        // steps whose actor list was cut to kDistantActorsMax
    // ---- D3: THE EDGE (section 2.7; MobSystem::PublishDistantBand, the impact
    // pass's near branches, the arrows across the boundary) -------------------
    uint64_t bandPublished = 0;    // distant actors handed to the near tier, summed over ticks
    uint64_t bandPeak = 0;         // ...the most in one tick
    uint64_t bandCapped = 0;       // ticks the published band was cut to kDistantBandMax
    uint64_t edgeNearBlows = 0;    // near strokes at a distant creature, queued at their cut
    uint64_t edgeNearHits = 0;     // ...resolved: Hit (applied to the distant body)
    uint64_t edgeNearMisses = 0;   // ...Miss
    uint64_t edgeNearBlocked = 0;  // ...Blocked
    uint64_t edgeNearOutOfReach = 0;  // ...the target was out of reach + slack by then
    uint64_t edgeNearNoTarget = 0;    // ...it was dead, promoted or gone when it landed
    uint64_t edgeNearUnmatched = 0;   // a near stroke whose style the attacker's kit lacks
    uint64_t edgeFarSwings = 0;    // distant swings at a NEAR creature of the band, resolved
    uint64_t edgeFarHits = 0;      // ...Hit
    uint64_t edgeFarMisses = 0;    // ...Miss
    uint64_t edgeFarBlocked = 0;   // ...Blocked
    uint64_t edgeFarOutOfReach = 0;   // ...out of reach + slack
    uint64_t edgeFarApplied = 0;   // hits that entered the near creature's own door
    uint64_t edgeFarUnapplied = 0; // hits with no door to enter (no physics / spawns: a CPU arm;
                                   // the struck limb already gone; a ghost)
    uint64_t edgeFarRayMisses = 0; // ...whose contact fell back to the limb's centre
    uint64_t edgeFarCovered = 0;   // ...that a worn shell took (the cover retarget)
    uint64_t edgeFarHpRollMilli = 0;  // life hp the roll gave those hits, x1000
    uint64_t edgeFarHpDoorMilli = 0;  // life hp the near door charged for them, x1000
    uint64_t edgeArrowsIn = 0;     // inbound real flights loosed at a near creature
    uint64_t edgeArrowsConverted = 0;  // abstract arrows whose target was promoted mid-flight
    uint64_t edgeArrowsInjected = 0;   // inbound flights handed to ArrowFlights at the boundary
    uint64_t edgeArrowsGrounded = 0;   // ...that met DistantGround first
    uint64_t edgeArrowsExpired = 0;    // ...that ran out of life first
    uint64_t edgeArrowsOut = 0;    // near flights handed to the tier as they left the window
    uint64_t edgeArrowsOutResolved = 0;  // ...resolved (ResolveArrow) into an abstract flight
    uint64_t edgeArrowsOutLost = 0;      // ...whose target was no living distant creature by then
    uint64_t flipWaits = 0;        // promotions held for the flip bound (kDistantMaxFlips)
    uint64_t flips = 0;            // tier changes recorded (demotions + promotions)
    // A player-band id found in a distant creature's actor list. MUST stay 0:
    // section 2.7's "never a player", counted at the point the list is built
    // so a regression shows where it happens (`distant-edge` asserts it).
    uint64_t playerActorsSeen = 0;
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
  // ...each latch on its own: the drive moves creatures every tick, and only
  // one that ended a step OVER a border needs its scan (D1).
  void MarkPromoteDue() { promoteScanDue_ = true; }
  void MarkRetireDue() { retireScanDue_ = true; }
  bool PromoteScanDue() const { return promoteScanDue_; }
  void ClearPromoteScan() { promoteScanDue_ = false; }
  bool RetireScanDue() const { return retireScanDue_; }
  void ClearRetireScan() { retireScanDue_ = false; }

  // ---- THE SPATIAL HASH (section 2.11; package D1) --------------------------
  //
  // 2D cells of kCellVox voxels over the creatures' body CENTRES (x / z), id
  // lists kept sorted. It serves perception, spacing, the sleep test and the
  // wake: everything a creature asks about "who is round me" is a walk of a
  // few cells, never of the tier. Maintained HERE, by every door a creature
  // enters, leaves or moves through (Insert, Erase, Take, Clear, Relocate), so
  // nothing can file a creature in a cell it left.
  static constexpr int kCellVox = 64;
  static int CellOf(float v) { return (int)std::floor(v / (float)kCellVox); }
  // Move creature `id` to `origin` (its body box min corner) and refile it.
  // WAKES every sleeper whose 3x3 neighbourhood it ENTERED (a cell of the new
  // neighbourhood that was not in the old one); returns how many it woke.
  // A gate that teleports a distant creature calls this too.
  uint32_t Relocate(uint64_t id, Vec3 origin);
  // Every creature filed within `radiusCells` cells (Chebyshev) of cell
  // (cx, cz), ASCENDING ID.
  void Neighbourhood(int cx, int cz, int radiusCells,
                     std::vector<uint64_t>& out) const;
  // Wake every sleeper filed within 1 cell (Chebyshev) of (cx, cz) that is
  // NOT within 1 of (fromX, fromZ) -- the cells a mover ENTERED -- or all of
  // them when `fromValid` is false (a creature that arrived). The near
  // tier's creatures in the edge band wake distant sleepers through this too.
  uint32_t WakeEntered(int cx, int cz, bool fromValid, int fromX, int fromZ);
  // Creatures awake right now (the living not asleep). A walk; the gates'.
  size_t AwakeCount() const;
  // THE SLEEP SUMMARY TickDistant's fast path reads instead of walking the
  // tier: "somebody may be awake" (conservative: set by every door that can
  // wake one -- Insert, WakeEntered, MarkAwakeHint) and the earliest
  // wakeTick among the sleepers (UINT32_MAX = none). Exact after each
  // TickDistant pass (SetSleepSummary). A gate that writes `asleep` by hand
  // calls MarkAwakeHint.
  bool AnyAwakeHint() const { return anyAwakeHint_; }
  void MarkAwakeHint() { anyAwakeHint_ = true; }
  uint32_t NextWakeTick() const { return nextWakeTick_; }
  void SetSleepSummary(bool anyAwake, uint32_t nextWake) {
    anyAwakeHint_ = anyAwake;
    nextWakeTick_ = nextWake;
  }
  void RefreshNextWake();
  // THE MOVE GENERATION (D3): bumped by every door a creature enters, leaves or
  // moves through (Insert, Erase, Take, Clear, Relocate). The published edge
  // band's membership is cached against it (MobSystem::PublishDistantBand):
  // nobody moved, nobody joined or left, so the members are the same.
  uint64_t MoveGen() const { return moveGen_; }
  // ...and anything else that changes who is published (a death).
  void TouchMoveGen() { moveGen_++; }

  // ---- ARROWS IN THE AIR (D2b) ------------------------------------------------
  // In LOOSE order (tick, then the shooter's id): the impact pass appends at
  // the loose and applies + erases at the impact tick. Cleared with the tier.
  // A gate that rewinds the tier (Erase + Insert) clears these too.
  const std::vector<DistantArrowFlight>& Arrows() const { return arrows_; }
  std::vector<DistantArrowFlight>& ArrowsMut() { return arrows_; }

  const Stats& GetStats() const { return stats_; }
  // Bump one counter here AND in DistantRunTotals().
  void Note(uint64_t Stats::*field, uint64_t n = 1);
  // Raise one maximum here AND in DistantRunTotals().
  void NoteMax(uint64_t Stats::*field, uint64_t v);

 private:
  std::vector<DistantMob> list_;   // sorted by id, strictly ascending
  size_t capOverride_ = 0;
  bool promoteScanDue_ = false;
  bool retireScanDue_ = false;
  Stats stats_;
  // The hash: packed cell -> ids filed there, ascending.
  std::unordered_map<uint64_t, std::vector<uint64_t>> cells_;
  std::vector<DistantArrowFlight> arrows_;
  uint32_t nextWakeTick_ = UINT32_MAX;
  bool anyAwakeHint_ = false;
  uint64_t moveGen_ = 0;
  static uint64_t CellKey(int cx, int cz) {
    return ((uint64_t)(uint32_t)cx << 32) | (uint64_t)(uint32_t)cz;
  }
  void File(DistantMob& d);
  void Unfile(const DistantMob& d);
  size_t LowerBound(uint64_t id) const;
};

// The whole process's distant-tier counters (every DistantMobs adds into it),
// for build/last_run.json's "distant" block: like the op stream's refusals,
// a demotion that parked for want of room is a number, not a missing NPC.
const DistantMobs::Stats& DistantRunTotals();

// THE STATUES' ITEM LIST (D4a's seam, game/distant_draw.h): one DistantDrawItem
// per distant creature that has a baked look -- STRIKE while an abstract
// swing is in flight (D2b), WALK while it moved within its last stride (WalkA
// / WalkB on its gait phase), IDLE otherwise, the dead lying at their
// statue's rest height. `out` is cleared first. Render-only: the frame loop
// calls it before DistantDraw::Produce.
void FillDistantDrawItems(const DistantMobs& tier, uint32_t tick, int idleStride,
                          std::vector<DistantDrawItem>& out);

struct ItemLibrary;
struct RenderOnlyBody;
// THE ARROWS IN THE AIR between distant creatures (D2b), as render-only
// bodies on their arc: the ammo item's own micro model (game/arrows.cpp's
// ArrowXf placement), from the shooter's centre to the target's over the
// flight's ticks, lifted by the drop gravity would give that flight, at the
// frame's tick interpolation. Appended to `out`. Render-only, never hashed.
void AppendDistantArrowBodies(const DistantMobs& tier, const ItemLibrary& items,
                              uint32_t tick, float tickAlpha,
                              std::vector<RenderOnlyBody>& out);
