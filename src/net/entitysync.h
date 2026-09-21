// entitysync.h — THE PER-TICK ENTITY TRAFFIC, ASSEMBLED IN ONE PLACE
// (docs/PLAN_multiplayer_m9.md §3, M9.4 package D).
//
// WHAT THIS FILE IS FOR. Packages A, B and C built three halves that do not
// touch each other: `net::Authority` answers "whose is it?", `MobSystem`
// knows how to be a ghost and how to hand a creature over, `DebrisSystem`
// knows the same for loose matter, and `mobsync.h`/`debrissync.h` turn each
// of their records into bytes. NONE of them knows what tick it is, which
// entities the peer can see, or when an announce has to precede a pose. That
// decision layer is this file, and it lives here rather than in `main.cpp`
// for one concrete reason: the frame loop is already 12,000 lines, and
// "which of my creatures is inside the peer's window this tick" is a rule
// with four edge cases (first sight, left the window, died, changed hands)
// that has to be READABLE to be trusted.
//
// THE FOUR RULES OF INTEREST, which are the whole of `Build`:
//
//   1. ANNOUNCE ON FIRST SIGHT. An entity I own is announced the first tick
//      its chunk lands inside the peer's window grown by
//      `kInterestMarginChunks`. The announce is the only record that carries
//      the SHAPE (a mob's def and gear, a body's two lattices), so a pose
//      that arrives before one has nothing to pose.
//   2. POSE EVERY TICK WHILE INSIDE. The smallest record, and the only one
//      that repeats. Bodies get the sleeping-body discount for free:
//      `DebrisSystem::OwnedBodiesNear` already declines to list a settled
//      body (debris.h's note on `inactiveTicks`).
//   3. GONE WHEN IT LEAVES, DIES, OR IS RELEASED. Three causes, one record,
//      distinguished by `reason` for the diagnostics and by nothing else:
//      the receiver's job is identical in all three.
//   4. HANDOFF WHEN AUTHORITY FLIPS, and it is sent by the SIDE LOSING IT.
//      See the long note on `ScanHandoffs` — the receiving side does not
//      wait for it.
//
// WHY IT OWNS THE TWO `AuthorityMemory` MAPS. `net::Authority` is pure and
// its hysteresis state is the caller's (authority.h says why: a file-static
// incumbent map would be exactly the origin-keyed process global DESIGN.md
// §10 forbids). `MobSystem` and `DebrisSystem` each take an ownership
// FUNCTION, and both of those functions have to consult the same peer views
// and the same incumbent tables that the handoff scan consults, or the scan
// and the systems would disagree about who owns what within one tick. So one
// object holds the memories and hands out the two closures — `MobOwnerFn()`
// and `BodyOwnerFn()` — and there is exactly one answer per entity per tick.
//
// NOT A SOCKET. `EntityBatch::Encode`/`Decode` produce and consume bytes;
// `main.cpp` sends them. Same separation `LockstepPacer` and `ChunkSync`
// keep, and for the same reason: everything here is drivable with two
// systems and a vector of bytes.
#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/authority.h"
#include "net/debrissync.h"
#include "net/mobsync.h"

class MobSystem;
class DebrisSystem;
class WorldItems;

namespace net {

// Bumped whenever the envelope's field order changes. The RECORDS inside
// carry their own versions (kMobSyncVersion, kDebrisWireVersion); this one is
// about the envelope alone.
inline constexpr uint32_t kEntityBatchVersion = 1;

// How far past the peer's window edge an entity is still interesting. ONE
// chunk, matching the plan's "the peer's window + 1 chunk": an entity exactly
// on the boundary is about to be visible, and announcing it a chunk early
// means the shape has arrived before the first pose that needs it. Larger
// costs bandwidth for things the peer cannot draw; smaller means a creature
// walking in pops a tick late.
inline constexpr int kInterestMarginChunks = 1;

// How long a ghost survives its owner's silence. 3 s at kTickDt (30 Hz) = 90
// ticks, the same 3 s the link's own silence rule uses (main.cpp) — one
// number for "this peer has stopped talking about this thing", applied to a
// single entity instead of to the whole connection.
//
// It is a BACKSTOP, not the normal exit: the normal exit is a `MobGone` /
// `BodyGone`. What it catches is the case no message can cover — the owner
// walked far enough away that I left ITS interest set, so it stopped posing
// and (because I am outside its window) it cannot tell whether I still care.
inline constexpr uint32_t kGhostExpiryTicks = 90;

// ---- the envelope -----------------------------------------------------------
//
// ONE TICK'S ENTITY TRAFFIC, addressed to one LABEL. The label is the tick the
// PEER will run when it applies this, exactly like the `TickBatch` it rides
// behind, and for the same reason (main.cpp's note on `netSendBatch`): a
// record applied on arrival would move a body D ticks ahead of every local
// thing it can stand on.
//
// THE FIELD ORDER IS THE APPLY ORDER and it is load-bearing:
//   handoffs -> announces -> poses -> gones -> takes -> grants.
// A handoff carries its own announce, so it must run before an announce that
// would re-create the thing it just adopted; an announce must run before the
// pose that moves it; a gone must run after the pose (a body that died this
// tick should be seen at its last position, not left at its previous one);
// and an item grant must run after the gone that may have removed its body.
struct EntityBatch {
  uint32_t tick = 0;      // the label
  uint32_t playerId = 0;  // the sender

  std::vector<MobHandoff> mobHandoffs;
  std::vector<MobAnnounce> mobAnnounces;
  std::vector<MobPose> mobPoses;
  std::vector<MobGone> mobGones;

  std::vector<BodyHandoff> bodyHandoffs;
  std::vector<BodyAnnounce> bodyAnnounces;
  std::vector<BodyPose> bodyPoses;
  std::vector<BodyGone> bodyGones;

  std::vector<ItemTake> itemTakes;
  std::vector<ItemGrant> itemGrants;

  bool Empty() const {
    return mobHandoffs.empty() && mobAnnounces.empty() && mobPoses.empty() &&
           mobGones.empty() && bodyHandoffs.empty() && bodyAnnounces.empty() &&
           bodyPoses.empty() && bodyGones.empty() && itemTakes.empty() &&
           itemGrants.empty();
  }
  void Clear();
  size_t RecordCount() const {
    return mobHandoffs.size() + mobAnnounces.size() + mobPoses.size() +
           mobGones.size() + bodyHandoffs.size() + bodyAnnounces.size() +
           bodyPoses.size() + bodyGones.size() + itemTakes.size() +
           itemGrants.size();
  }

  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// ---- the per-tick decision layer -------------------------------------------
class EntitySync {
 public:
  // ---- lifetime -----------------------------------------------------------
  //
  // NOTHING HERE RUNS UNCONNECTED, and that is this package's whole hash
  // argument: `main.cpp` binds the ownership functions inside `Connect` and
  // unbinds them in `Disconnect`, so a single-player tick calls
  // `MobSystem::RefreshOwnership` with a null `ownershipFn_` (an immediate
  // return) and `DebrisSystem::RefreshOwnership` likewise. The `--record-ops`
  // oracle is what proves it.
  void Connect(uint32_t me, uint32_t peer);
  void Disconnect();
  bool Connected() const { return connected_; }
  uint32_t Me() const { return me_; }
  uint32_t Peer() const { return peer_; }

  // ---- what the authority arithmetic reads --------------------------------
  //
  // Rebuilt every tick by main.cpp from this machine's own session and the
  // peer's latest applied `PlayerState`. Two entries (me, peer) when the peer
  // has spoken; one (me) before its first batch lands, which correctly makes
  // every entity mine until there is a second candidate.
  void SetPeers(const PeerView& mine, const PeerView* peer);
  std::span<const PeerView> Peers() const {
    return std::span<const PeerView>(peers_, (size_t)peerCount_);
  }
  // Has the peer told us where its window is? Interest cannot be computed
  // without it, so `Build` sends only handoffs and item traffic until it has.
  bool HavePeerView() const { return peerCount_ > 1; }
  IVec3 PeerWindowOrigin() const { return peers_[1].windowOrigin; }

  // ---- the two ownership closures -----------------------------------------
  //
  // Handed to MobSystem::SetOwnershipFn / DebrisSystem::SetOwnershipFn, and
  // the ONLY place either system's owner field comes from while connected.
  // They capture `this`, so the EntitySync must outlive the two systems'
  // bindings — main.cpp unbinds in `netDrop` before anything is destroyed.
  uint32_t MobOwner(uint64_t mobId, Vec3 feetVox);
  uint32_t BodyOwner(Vec3 posVox);
  // ...and the chunk question, which is NOT the body question: the island
  // scan is about a region of grid, not about anything standing in it.
  bool ChunkOwned(IVec3 wc);

  // ---- outgoing -----------------------------------------------------------
  //
  // BEFORE the tick, because a handoff must be built while I still own the
  // creature: `MobSystem::TakeHandoff` refuses a mob that is already a ghost,
  // and `RefreshOwnership` inside `PreTick` would have made it one.
  void ScanHandoffs(MobSystem& mobs, DebrisSystem& debris, uint32_t tick);

  // AFTER the tick, with the label the peer will apply it at. Drains the
  // handoffs `ScanHandoffs` staged, then walks the interest set.
  void Build(MobSystem& mobs, DebrisSystem& debris, uint32_t label,
             EntityBatch& out);

  // ---- incoming -----------------------------------------------------------
  void NoteRemote(EntityBatch&& b);
  // Apply everything labelled `tick`, in the envelope's field order, and drop
  // every queued batch at or before it. `items` may be null (no registry).
  void ApplyForTick(uint32_t tick, MobSystem& mobs, DebrisSystem& debris,
                    WorldItems* items);
  // Rule 3's backstop; run once per tick after ApplyForTick.
  void ExpireGhosts(uint32_t tick, MobSystem& mobs, DebrisSystem& debris);

  // ---- item grants addressed to ME ----------------------------------------
  //
  // `DebrisSystem::PopItemGrant` yields BOTH the replies a peer sent me and
  // the replies I generated for the peer (debris.h says so). `Build` splits
  // them by `toPlayer`: the peer's go on the wire, mine come out here for the
  // caller to put in a bag. main.cpp's E key is the only consumer.
  bool PopLocalGrant(ItemGrant& out);

  // ---- counters, for the `--frames` net report and the smoke --------------
  struct Counters {
    uint64_t announcesOut = 0, posesOut = 0, handoffsOut = 0, gonesOut = 0;
    uint64_t announcesIn = 0, posesIn = 0, handoffsIn = 0, gonesIn = 0;
    uint64_t takesOut = 0, takesIn = 0, grantsOut = 0, grantsIn = 0;
    uint64_t batchesOut = 0, batchesIn = 0, bytesOut = 0;
    uint64_t applyMisses = 0;   // poses for an id we do not hold
    uint32_t ghostsNow = 0;     // ghost mobs + ghost bodies at the last apply
    uint32_t ghostsMax = 0;
    uint64_t expired = 0;       // ghosts dropped by the 3 s rule
  };
  const Counters& Stats() const { return c_; }
  // Folded into main.cpp's running totals before Disconnect() clears them,
  // the same harvest-first discipline the op and chunk counters use.
  void ResetStats() { c_ = Counters{}; }

 private:
  bool connected_ = false;
  uint32_t me_ = 0, peer_ = 1;

  // [0] = me, [1] = the peer (present only once it has sent a state).
  PeerView peers_[2]{};
  int peerCount_ = 0;

  // The incumbent tables authority.h's hysteresis needs. Keyed by ENTITY id
  // (mob id / body global id) and by chunk key respectively — never mixed.
  AuthorityMemory mobMem_, bodyMem_, chunkMem_;

  // Which of my entities the peer has been told about. An id leaves these by
  // exactly three doors: it left the interest set (Gone/despawn), it stopped
  // existing (Gone/death), or it changed hands (the handoff carries its own
  // announce, so the new owner re-announces from its side).
  std::unordered_set<uint64_t> announcedMobs_, announcedBodies_;

  // Ghost id -> the tick we last saw a pose/announce for it. Rule 3's clock.
  std::unordered_map<uint64_t, uint32_t> ghostMobSeen_, ghostBodySeen_;

  // Handoffs staged before the tick, drained by `Build` after it.
  std::vector<MobHandoff> pendingMobHandoffs_;
  std::vector<BodyHandoff> pendingBodyHandoffs_;

  // Received batches, keyed by the label they must be applied at. Never more
  // than D+1 deep in the steady state; a deque because it is drained in order.
  std::deque<EntityBatch> inbox_;

  // Grants addressed to me, waiting for the caller's bag.
  std::deque<ItemGrant> localGrants_;

  Counters c_{};
};

}  // namespace net
