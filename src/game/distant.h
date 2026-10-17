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
// D7a THE TIER ACROSS MACHINES (docs/PLAN_distant_npcs.md 8.3 / 8.6; the wire
// is net/distantsync.h). THE DISTANT WORLD IS THE HOST'S: in a two-player game
// one machine -- the host, whose ChunkStore is the world's -- holds every
// distant creature, steps, retires, activates and saves them. Every "the
// window" in the borders above becomes "a window of the game", W = the host's
// window plus each connected peer's (DistantPeerWindow, the very origins
// net::Authority reads):
//   * demote   the host's park rule, the host's window; the RADIUS is against
//              the nearest window centre in W;
//   * retire   only past radius + hysteresis of EVERY centre in W;
//   * activate within the radius of ANY centre in W, beyond the host window's
//              pad -- so a record parked inside a PEER's window activates too
//              (nothing else would ever unpark it there) and is handed in;
//   * promote  over the host's line -> local, as before; over a PEER's line ->
//              a REMOTE PROMOTION (a DistantHandIn: the patched record, the
//              brain, the steer, the look key) when only that peer's line has
//              it or the ownership rule names that peer for its feet where
//              both lines do; served nearest to that peer's window first, at
//              most EntityParking::kUnparkPerCall per peer per tick, under the
//              peer's reported room (livingFree) minus what is in transit and
//              the flip bound. The creature leaves the tier into an IN-TRANSIT
//              list (kept until the peer's DistantHandInDone, restored at a
//              disconnect, saved with the tier);
//   * hold     at the promotion line of every window it waits on (the drive's
//              box test walks the list).
// A CLIENT holds none of its own. Its park rule HANDS OUT a creature leaving
// its window (DistantHandOut: the SaveOne record, the brain, the steer and
// velocity, the statuses, the look it bakes now from the live rig) instead of
// erasing it; the host makes it a distant creature under the SAME id
// (DistantFromRecord), or parks it -- never loses it. A hand-in lands in the
// client's WAITING ROOM (its own DistantMobs, `DistantMob::handIn`): never
// stepped, retired or activated here; promoted by the client's own pass (the
// distant half of EntityParking::Unpark, run although its parking is off)
// under its own waits, or given back after kHandInMaxWaitTicks. The client's
// edge band and statues come from the host's DistantView of it (DistantViewMob,
// below). Nothing of this runs unconnected: with no role the code above is
// exactly the single-player tier.
//
// D8 CAP OVERFLOW (fights larger than 64; MobSystem::OverflowSwaps, called once
// from EntityParking::Unpark before its promotion pass). While the LIVING cap
// binds -- more living distant creatures wait over the promotion line than
// there are free slots -- the waiting are served NEAREST TO THE PLAYER first
// (the nearest living player actor; the window centre with none), and when no
// slot is free one is MADE: the FARTHEST near creature from the player that
// is at least `distant.overflowMinDistVox` away and farther than the waiting
// one by kDistantOverflowMarginVox is demoted where it stands (the park
// rule's own DemoteToDistant, inside the window) and the waiting one promoted
// into its slot. A near creature whose brain targets a PLAYER is the last
// choice (it stops seeing the player the moment it is distant). Both flips
// pass the thrash bound; at most kDistantOverflowSwapsPerTick swaps a tick.
// An overflow-demoted creature (DistantMob::overflow) lives INSIDE the window:
// it walks on the WINDOW'S ground (the fetch cache, DistantGround's window
// source) where the cache has it, fights near and distant creatures through
// the edge above (the near band widens to the whole near crowd while any
// distant creature is inside the window), is drawn as a statue, never
// perceives a player, and is HELD AT THE PLAYER'S RING -- no step that ends
// within overflowMinDistVox of a player AND nearer to it -- instead of at the
// promotion line, so the near side of a fight is always the full-fidelity one.
//
// D9 THE PLAYER'S REACH (game/arrows.h). Every flight in the window tests the
// distant creatures' body boxes (MobBodyBox's world box, published at the
// start of the tick: DistantBoxes) along each tick's segment: the earliest
// box, ties by id, ends it, and the hit is resolved on the next distant tick
// by distant::ResolveArrowContact (the limb nearest the arrow's line in the
// body as it stood at demotion -> its role, the shell, the bleed). A PLAYER's
// flight (no ArrowShot target) that leaves the window is not lost: the tier
// flies it on (an OUTBOUND DistantArrowFlight, ArrowStepOutside over
// DistantGround) testing the boxes round it every tick, until it hits, lands
// on the ground, re-enters the window (a real flight again) or runs out of
// life. Spells stay window-bound. NPC flights with a target keep the D3
// hand-over -- except at a target INSIDE the window, which the box test has
// already judged.
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

class GenOverlay;   // game/distant_obstacles.h: the worldgen overlay (D10)
class Stream;
class World;
struct MaterialGpu;

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
//      Trees, buildings, ruins and `.svedit` layers are NOT in it -- source 5
//      puts them back.
//   2. WATER: the higher of TerrainColumn().water (tarns, the sea) and
//      worldmap::WaterColumnAt (rivers, lakes). Liquid is never ground: a
//      body in water stands on the BED; the walk decides to wade or swim.
//   3. EDITED GROUND: a chunk the player changed and the window then left
//      is in the ChunkStore, and where it is its cells ARE the ground (a
//      crater you dug and walked away from is real to a distant creature).
//      Read through Stream::SettledStoredChunk, which first completes any
//      eviction of that chunk still in flight -- the RegenerateChunks loop --
//      because when an eviction's bytes reach the store is up to the GPU
//      (audit A3). A chunk not in the store is procgen, i.e. sources 5 + 1.
//   4. THE WINDOW (D8), for an overflow creature only (SetWindowSource).
//   5. THE WORLDGEN OVERLAY (D10, game/distant_obstacles.h): what genChunk
//      stands on and cuts into the base -- the low band of every lattice and
//      authored tree, cacti, the stamps (houses; their DOOR leaves read air),
//      the dungeons' ruins, the `.svedit` layers -- re-derived on the CPU from
//      the shader's own inputs. Between the stored chunk (which already holds
//      all of it, as the GPU made it) and the base. A column the overlay does
//      not touch keeps the TerrainHeight fast path.
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
  ~DistantGround();
  DistantGround(const DistantGround&) = delete;
  DistantGround& operator=(const DistantGround&) = delete;
  // The edited-ground source (null = TerrainHeight + water only: a gate's
  // CPU arm, a harness with no stream) and the material collision classes
  // that say what carries weight (MobSystem's `classOf_`). Cheap when
  // nothing changed; rebinding a different stream drops the chunk cache and
  // installs this object's listener on the new store (ONE listener per
  // store: ChunkStore::SetChangeFn). `gpu` (D10): the material table's GPU
  // words, which the overlay's dungeon twin reads (null = no dungeons).
  void Bind(Stream* stream, const std::vector<uint32_t>* classOf, uint32_t seed,
            const std::vector<MaterialGpu>* gpu = nullptr);
  // ---- 5. THE WORLDGEN OVERLAY (D10) -------------------------------------------
  // On by default. Off = the D1 ground exactly (the cost arm's "before", and a
  // gate's control). The overlay object itself, for the gates' attribution.
  void SetOverlayEnabled(bool on);
  bool OverlayEnabled() const;
  GenOverlay& Overlay() { return *overlay_; }
  // Everything cached, gone (a materials reload, a world reset, a gate that
  // swapped the map by hand).
  void InvalidateAll();
  // ---- 4. THE WINDOW'S OWN GROUND (D8) ---------------------------------------
  // For a creature the cap overflow demoted INSIDE the window (DistantMob::
  // overflow): while set, a cell in a window chunk the FETCH CACHE holds reads
  // from the cached words -- buildings, trees, the crater dug an hour ago and
  // still in the window, everything the near tier walks on -- by the same
  // "carries weight" test, and every other cell from the sources above. The
  // fetch cache's arrivals are fixed-latency (K = 4) and it is the very cache
  // the near tier's own ground reads, so this is the near tier's ground at the
  // near tier's determinism. Set by the distant tick around ONE overflow
  // creature's step and cleared after it, so no other creature's answers
  // change; null = off. A cached chunk is decoded once per fetched VERSION.
  void SetWindowSource(const World* w) { window_ = w; }
  const World* WindowSource() const { return window_; }

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
    uint64_t windowDecodes = 0; // D8: fetched window chunks decoded into a mask
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
  // D8: a fetched window chunk's mask, valid for the cache VERSION it was
  // decoded from (a newer fetch re-decodes; the words at one version are one
  // tick's end state, so a re-fetch at the same version is the same mask).
  struct WinChunk {
    uint32_t version = 0;
    uint64_t solid[kChunkVol / 64] = {};
    uint64_t lastUse = 0;
  };
  const WinChunk* WindowChunk(IVec3 wc);
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
  // D8: the window source (null = off) and its decoded chunks.
  const World* window_ = nullptr;
  std::unordered_map<uint64_t, WinChunk> winChunks_;
  // D10: the worldgen overlay. It holds a pointer back to this object (its
  // TerrainTop / WaterTop), so the ground is not copyable.
  std::unique_ptr<GenOverlay> overlay_;
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
  // D5+D6: per RECORD limb, 1 = still attached (DistantMob::limbRole's
  // kDistantLimbAttached). A base limb the record holds attached and this
  // says is off was SEVERED AT DISTANCE: PatchMobRecord writes it
  // kLimbSevered. Empty = keep the record's kinds.
  std::vector<uint8_t> limbAttached;
  bool dead = false;
  std::string cause;     // the death cause BY NAME (Mob::DeathCause), "" alive
  uint64_t deathSeq = 0; // the death order; 0 = "youngest" on load
  std::string refId;     // Mob::RefId: the dungeon spawner renames a corpse's
  // D5+D6: THE STATUSES (MOBS v13's tail, mob.h kSaveVersion): what a retire
  // or a save carries so the creature is still bleeding and burning when it
  // comes back, as a promotion always had it. All zero = no tail entry.
  float bleedRate = 0.0f;       // DistantMob::bloodLossPerTick
  int32_t bleedLimb = -1;       // DistantMob::lastWoundLimb
  uint32_t burnTicksLeft = 0;   // DistantMob::burnTicksLeft
  float burnFrac = 0.0f;        // DistantMob::burnFrac
  uint32_t stumps = 0;          // DistantMob::stumps
  void ClearStatuses() {
    bleedRate = 0.0f;
    bleedLimb = -1;
    burnTicksLeft = 0;
    burnFrac = 0.0f;
    stumps = 0;
  }
};

// A creature still burning when it comes back from the tier re-lights this
// many voxels of its trunk (MobSystem::RelightTrunk, IgniteLimb) -- a foothold
// the near burn pass then spreads by its own rules. Of the order of the
// calibration's burn probe (60).
constexpr uint32_t kDistantRelightVoxels = 40;

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
  // D8: DEMOTED BY THE CAP OVERFLOW, inside the window (MobSystem::
  // OverflowSwaps). While it stands over the promotion line it walks on the
  // window's ground (DistantGround::SetWindowSource) and is held at THE
  // PLAYER'S RING (no step ending within distant.overflowMinDistVox of a
  // player and nearer to it) rather than at the promotion line, so it may
  // walk and fight inside the window but never into the full-fidelity middle
  // of the fight. Cleared when it ends a step outside the promotion line: from
  // there it is an ordinary distant creature (held at the line if it comes
  // back in). Not in the record: a retire / save forgets it, as it should.
  bool overflow = false;
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
  // D5: THE ROUTINE'S OWN CLOCK (MobSystem::SetDistantRoutineClock): the tick
  // a villager's routine next changes by itself (a patrol's pause at a node
  // ends). When it falls asleep HOLDING its routine it wakes then. A schedule
  // ROW changing is the resident layer's to notice (it wakes the sleeper).
  uint32_t routineWakeTick = 0;
  // D5+D6: built by ACTIVATION from a parked record (MobSystem::
  // ActivateParked), not demoted from a rig. Diagnostic; nothing branches on
  // it but the counters.
  bool activated = false;
  // D7a: IN A CLIENT'S WAITING ROOM -- a DistantHandIn the host sent, held
  // here until this machine's promotion pass places it (EntityParking::Unpark's
  // waiting-room branch) or gives it back. Never stepped (TickDistant), never
  // published to the near band, never retired or activated: the distant world
  // is the host's, and the host keeps the in-transit copy that is its truth.
  // `handInTick` is the tick it arrived (the give-back clock).
  bool handIn = false;
  uint32_t handInTick = 0;

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
  // D9: an OUTBOUND flight -- a player's arrow (no ArrowShot target) that left
  // the window, flown on by the same integrator over DistantGround, testing
  // the distant body boxes round it every tick (no roll: it hits what it
  // meets). Uses pos / vel / seed / life as an inbound one does, plus the
  // arrow's flat damage; `target` 0 until it strikes.
  bool outbound = false;
  float damage = 0.0f;           // outbound: ItemDef::ArrowSpec::damage
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
  // D9: ticks already flown (a player's arrow coming back into the window after
  // a stretch outside keeps the life it has left; a D3 inbound arrow, 0).
  int life = 0;
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

// ---- D9: THE PLAYER'S REACH (game/arrows.h) ----------------------------------
//
// A distant creature's BODY BOX as the arrows in the window see it, published
// at the start of the tick (MobSystem::PublishDistantBand, before anybody
// moves): the near tier's own body box (MobBodyBox: the world box from the
// origin, worldSize up), plus the facing the hit's limb is read in. Every
// living distant creature whose box reaches into the window, in id order.
struct DistantBox {
  uint64_t id = 0;
  Vec3 lo{}, hi{};
  float heading = 0.0f;
};
// Where a segment met a body box: `t` along it (0..1), and the contact in the
// BODY'S frame -- x right, y up from the feet, z forward, from the box's
// centre column (distant::ArrowContact) -- with the segment's direction.
struct DistantBoxHit {
  uint64_t id = 0;
  float t = 0.0f;
  Vec3 at{};                     // world entry point
  float local[3] = {0, 0, 0};    // the entry, body frame
  float dir[3] = {0, 0, 1};      // the flight's direction, body frame (unit)
};
// A flight in the window that met a distant creature's box (ArrowFlights::
// Tick, through MobSystem::QueueEdgeArrowBoxHit): resolved at the top of the
// next distant impact pass by distant::ResolveArrowContact against the
// defender's tick-start state, in (tick, shooter) order.
struct NearArrowBoxHit {
  uint64_t shooter = 0, target = 0;   // the shooter's ACTOR id (a player's is in the band)
  uint32_t tick = 0;
  DistantBoxHit hit;
  float damage = 0.0f;               // ItemDef::ArrowSpec::damage
  std::string ammo;
};
// A PLAYER's flight (no ArrowShot target) that left the window (ArrowFlights::
// Tick, through MobSystem::QueueEdgeShotOut): taken into the tier's arrows as
// an OUTBOUND flight at the top of the next impact pass, at exactly this state.
struct NearShotOut {
  uint64_t shooter = 0;
  uint32_t tick = 0;
  int32_t pos[3] = {0, 0, 0};    // the last in-window sample (or where the window left it)
  int32_t vel[3] = {0, 0, 0};
  uint32_t seed = 0;
  int life = 0;                  // ticks flown in the window
  float damage = 0.0f;
  std::string ammo;
};
// What an arrow the distant tier flew or landed did (the gates' window into
// D9; bounded, never an input).
struct DistantShotEvent {
  enum class Kind : uint8_t {
    Hit,        // an OUTBOUND flight met a body box outside the window
    BoxHit,     // an in-window flight's box hit, applied
    Grounded,   // an outbound flight met DistantGround
    Reentered,  // ...crossed back into the window (a real flight again)
    Expired,    // ...ran out of life
    Lost,       // a box hit whose creature was no longer a living distant one
  };
  Kind kind = Kind::Hit;
  uint64_t shooter = 0, target = 0;
  uint32_t tick = 0;
  Vec3 at{};
  int limb = -1;
  float hp = 0.0f;
};
// One D8 swap, for the gates (bounded, never an input): who left, who came
// in, and how far each stood from the player when it happened.
struct DistantOverflowSwap {
  uint32_t tick = 0;
  uint64_t demoted = 0, promoted = 0;
  float demotedDist = 0.0f, promotedDist = 0.0f;
  bool lastResort = false;       // the demoted one targeted a player
};

// D8: at most this many cap-overflow swaps a tick (each is a demotion -- a
// statue bake, ~3-10 ms in the game -- and a LoadOne), and a swap only when
// the near creature stands farther from the player than the waiting one by
// this much: the hysteresis that stops two fighters at about the same range
// trading places every tick (the flip bound would stop it after four, but the
// rigs it builds and releases are not free).
constexpr int kDistantOverflowSwapsPerTick = 1;
constexpr float kDistantOverflowMarginVox = 24.0f;
// D9 / D8: the gates' logs (DistantShotEvent, DistantOverflowSwap), trimmed by
// half past this.
constexpr size_t kDistantLogMax = 256;

// THE EDGE BAND's width (section 2.7): the longest near sight (the archer's
// 160) plus a margin. A near creature within this of the window's boundary is
// in every distant actor list that reaches it; a distant creature within this
// of the window (or inside it, waiting at the promotion line) is published to
// the near tier's.
constexpr float kDistantEdgeBandVox = 192.0f;
// THE LONG REPLAN's radius (ai::WorldView::navRadiusLong): when a distant
// creature's local plan (its profile's navRadius, ~22-30) fails, it plans once
// more over this half-extent (193 x 193 columns), backed off 90 ticks after a
// failure. Wide enough to round a village block; the cost is paid only by a
// creature whose local plan failed.
constexpr int kDistantNavRadiusLong = 96;
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
    // Creatures that came back near still burning / bleeding: a promotion, and
    // since D5+D6 an unpark or a load of a record whose v13 tail carries it
    // (MobSystem::ApplyRecordStatus, inside LoadOne).
    uint64_t relit = 0;            // ...re-lit on the trunk (IgniteLimb)
    uint64_t rebled = 0;           // ...the owed blood re-opened
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
    // ---- D5+D6: ACTIVATION (parked -> distant, MobSystem::ActivateParked) ---
    uint64_t activateScans = 0;    // scans actually run (rule 2's witness)
    uint64_t activated = 0;        // parked records made distant creatures
    uint64_t activatedDead = 0;    // ...of which corpses
    uint64_t activateRecordsSeen = 0;  // candidate records looked at, summed over scans
    uint64_t activateBudgetWaits = 0;  // a candidate left for the next call: the budget
    uint64_t activateRoomWaits = 0;    // ...the tier's activation share was full
    uint64_t activateDefWaits = 0;     // ...its def needed a build and this call's was spent
    uint64_t activateKept = 0;     // left parked for good reason (old version, a remap,
                                   // no def, a player's corpse, a body the def no longer fits)
    uint64_t activateDropped = 0;  // dropped: its ref already has a body (LoadOne's rule)
    uint64_t activateUpgraded = 0; // v12 records taken as v13 (UpgradeRecordToCurrent)
    // ---- D5: schedules at distance (refs_npc.cpp's distant residents) -------
    uint64_t scheduleSteps = 0;    // resident steps run for a distant villager
    uint64_t scheduleWakes = 0;    // sleeping villagers woken by a row change
    uint64_t scheduleHolds = 0;    // ...steps that left one holding at its anchor
    uint64_t promoteCatchUps = 0;  // promotions that took the catch-up placement (never moved)
    // ---- D8: THE CAP OVERFLOW (MobSystem::OverflowSwaps) -------------------
    uint64_t overflowCalls = 0;     // calls in which the living cap bound
    uint64_t overflowFilled = 0;    // promotions into free slots, nearest to the player first
    uint64_t overflowSwaps = 0;     // swaps made: a near creature out, a waiting one in
    uint64_t overflowLastResort = 0;  // ...whose near creature targeted a player
    uint64_t overflowNoVictim = 0;  // a waiting creature with no near one farther to trade
    uint64_t overflowGroundWaits = 0; // a waiting creature whose ground was not known yet
    uint64_t overflowRefused = 0;   // a swap's demotion refused, or its promotion (the
                                    // near one stays distant; the slot is free)
    uint64_t overflowRingHolds = 0; // overflow creatures' steps refused at the player's ring
    uint64_t overflowInWindowPeak = 0;  // the most living distant creatures inside the window
    // ---- D9: THE PLAYER'S REACH (game/arrows.h) -----------------------------
    uint64_t boxHits = 0;           // in-window flights that met a distant body box
    uint64_t boxHitsApplied = 0;    // ...resolved onto the body (ResolveArrowContact)
    uint64_t boxHitsLost = 0;       // ...whose creature was no living distant one by then
    uint64_t shotsOut = 0;          // player flights that left the window, flown on here
    uint64_t shotsOutHits = 0;      // ...that met a distant body box
    uint64_t shotsOutGrounded = 0;  // ...that met DistantGround (a miss lands and ends)
    uint64_t shotsOutReentered = 0; // ...that crossed back into the window
    uint64_t shotsOutExpired = 0;   // ...that ran out of life
    uint64_t edgeArrowsOutInWindow = 0;  // D3 hand-overs refused: the target stood in the window
    // ---- D11: NEAR CREATURES WALK OUT (MobSystem::OutsideGroundFor) ---------
    uint64_t outsideGrounds = 0;    // near probes that met the face and read DistantGround past it
    uint64_t nearPastFace = 0;      // near creature-ticks with the footprint centre past the face
    uint64_t walkedOut = 0;         // demotions of a near creature that walked / swam over the pad
                                    // line (on its feet, <= kPad + 4 out: not a window move's)
    // ---- D7a: THE TIER ACROSS MACHINES (net/distantsync.h) -----------------
    // The client's side: creatures handed to the host at its park rule, and
    // the hand-ins it took into its waiting room and what became of each.
    uint64_t handOutsSent = 0;       // client: park rule -> DistantHandOut (not erased)
    uint64_t handInsReceived = 0;    // client: hand-ins taken into the waiting room
    uint64_t handInsPromoted = 0;    // ...promoted here (Done: promoted)
    uint64_t handInsReturned = 0;    // ...given back (Done: returned): the wait ran out, a refusal
    uint64_t handInGroundWaits = 0;  // a waiting-room promotion held: its ground not in the fetch cache
    // The host's side: hand-outs it took (or parked), remote promotions it
    // sent and the waits that held them, the answers, the disconnects.
    uint64_t handOutsTaken = 0;      // host: hand-outs made distant creatures under their own id
    uint64_t handOutsParked = 0;     // ...parked instead (no room, outside every radius, unreadable)
    uint64_t handOutsRefused = 0;    // ...refused: a disagreement (the id is live here), or no role
    uint64_t handInsSent = 0;        // host: remote promotions (the creature now in transit)
    uint64_t handInRoomWaits = 0;    // ...held: the peer reported no room (minus what is in transit)
    uint64_t handInBudgetWaits = 0;  // ...held: this tick's per-peer budget was spent
    uint64_t handInFlipWaits = 0;    // ...held: the thrash bound
    uint64_t donesPromoted = 0;      // host: transits closed, promoted on the peer
    uint64_t donesReturned = 0;      // ...returned: the in-transit copy back in the tier
    uint64_t donesUnknown = 0;       // ...for an id not in transit (counted, ignored)
    uint64_t transitRestored = 0;    // ...put back in the tier (or parked) at a disconnect
    uint64_t transitPeak = 0;        // the most creatures in transit at once
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

// ============================================================================
// D7a: THE TIER ACROSS MACHINES (the header's D7a paragraph; net/distantsync.h)
// ============================================================================

// Which side of a two-player game this MobSystem's tier is on. OFF in single
// player, in every gate that does not set it, and after a disconnect: then
// nothing below runs and the tier is exactly D0..D9's.
enum class DistantNetRole : uint8_t { Off = 0, Host, Client };

// ONE PEER'S WINDOW, as the host's tier rules read it: the peer's id, the
// origin of its residency window (chunks; PlayerState.windowOrigin, the very
// number net::Authority reads), and the room it last reported for promotions
// (its living cap minus its living near creatures minus its waiting room).
// `haveFree` false: it has not reported yet, so no LIVING creature is handed
// to it (a corpse needs no room). Set every tick before the tick by the net
// layer (net::EntitySync::ScanHandoffs).
struct DistantPeerWindow {
  uint32_t playerId = 0;
  IVec3 windowOrigin{};
  uint32_t livingFree = 0;
  bool haveFree = false;
};

// A hand-in a client holds this long without placing it (its ground not
// fetched, its cap full, the creature not over its line any more because its
// window moved) is GIVEN BACK: the host puts its in-transit copy back in its
// tier. 5 s: a fetch is K = 4 ticks plus the queue, so a hand-in that waits
// this long is waiting on something that will not come soon.
constexpr uint32_t kHandInMaxWaitTicks = 150;
// How often a view entry that did not change is sent anyway (the shape with
// it): the keyframe that lets a client re-learn a creature it lost, and the
// whole cost of an asleep one. 1 s.
constexpr uint32_t kDistantViewKeyframeTicks = 30;
// How many of its own baked looks a client remembers, by id (the creatures it
// handed out: their statues in the host's view draw as the body the client
// last saw, not the def's bare look). Oldest forgotten first.
constexpr size_t kHandedLooksMax = 1024;

// ONE HOST CREATURE AS A CLIENT SEES IT (the mirror a DistantView keeps up):
// the statue (def, look, draw state) and, inside the edge band of this
// client's window, the actor its near creatures see and the body box its
// arrows test. Plain data, rebuilt from the wire only.
struct DistantViewMob {
  uint64_t id = 0;
  bool haveShape = false;      // the def name and look have arrived (first sight / keyframe)
  std::string defName;
  DistantLookKey look;
  Vec3 origin{};               // the statue's origin (a corpse: its rest height)
  float heading = 0.0f;
  float gaitPhase = 0.0f;
  DistantDrawState state = DistantDrawState::Idle;
  bool dead = false;
  bool band = false;           // `actor` is live (in this window's edge band)
  ai::Actor actor;
  bool box = false;            // `boxLo/boxHi` reach into this window
  Vec3 boxLo{}, boxHi{};
};

// A HOST CREATURE IN TRANSIT to a peer (a remote promotion not yet answered):
// the whole DistantMob as it left the tier -- the truth the host puts back if
// the peer returns it or drops -- and to whom, since when.
struct DistantTransit {
  DistantMob d;
  uint32_t peer = 0;
  uint32_t sentTick = 0;
};

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
