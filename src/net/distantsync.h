// distantsync.h — THE DISTANT TIER ON THE WIRE (docs/PLAN_distant_npcs.md
// §8.3 and §8.6, package D7a: "the tier across machines").
//
// THE MODEL IN ONE SENTENCE: THE DISTANT WORLD IS THE HOST'S. One machine --
// the host, whose ChunkStore is the world's -- holds every distant creature,
// steps them, retires and activates them, and saves them. A client holds none
// of its own: it holds a VIEW of the host's tier near its window (statues and
// the edge band) and a WAITING ROOM of creatures the host handed it to
// promote. Everything that crosses between the two for that is one of FOUR
// records, all here, all data (mobsync.h's shape: no sockets, no tick, no
// MobSystem):
//
//   DistantHandOut     client -> host. A creature the CLIENT stepped left the
//                      client's window (its park rule fired) and the client
//                      has nowhere to put it: no store of its own, no tier
//                      (persist.h: a client never parks). Instead of the old
//                      release -- the creature erased for good -- it goes to
//                      the host as its SaveOne record + the brain on the wire
//                      + its steer, velocity and statuses (the bleed and fire
//                      CaptureDistantVitals reads off the live rig before the
//                      rig goes) + the look the client baked NOW from the live
//                      body. The host makes it a distant creature under the
//                      SAME id (MobSystem::DistantFromRecord), or parks it.
//   DistantHandIn      host -> client. A REMOTE PROMOTION: a host distant
//                      creature over the CLIENT's promotion line (the
//                      ownership rule naming the client where both windows
//                      hold it). The PATCHED record (statuses in its v13
//                      tail, distant severs as kLimbSevered), the brain, the
//                      steer, whether it moved while distant, the look key.
//                      The host keeps it IN TRANSIT until the client answers.
//   DistantHandInDone  client -> host. The answer: PROMOTED (it is the
//                      client's near creature now; the host forgets it) or
//                      RETURNED (the client could not place it within
//                      kHandInMaxWaitTicks, or not at all; the host puts its
//                      own in-transit copy back in its tier -- the waiting
//                      room never steps, so that copy is the truth).
//   DistantView        host -> client, every batch, one per peer: the host's
//                      tier creatures within distant.radius of the peer's
//                      window centre, as statues (and, inside the edge band
//                      of the peer's window, as actors with body boxes) --
//                      the shape (def name, look key) on first sight and on
//                      a keyframe, the rest on change, a GONE when one leaves
//                      the set. What the client draws and what its near
//                      creatures see past its face.
//
// Plus one number on every batch that changed it: `livingFree`, the client's
// room for promotions (its living cap minus its living near creatures minus
// its waiting room), which the host's remote promotion is served under.
//
// WHY THE RECORD AGAIN (and not a struct of its own): mobsync.h's argument.
// Mob::SaveOne already answers "what is this creature" exactly and in one
// place; the overlay the tier owns rides inside it (MobSystem::PatchMobRecord),
// so a hand-out, a hand-in, a retire, a save and a promotion all read and
// write the same bytes, and the record round trip (distant-roundtrip's D0
// contract) holds across the wire because it is the same function.
//
// NOT A LOCAL SAVE FORMAT, and every record carries kDistantSyncVersion: a
// mismatched build is refused loudly (the reader is poisoned), never read as
// garbage -- mobsync.h's rule.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "net/mobsync.h"   // MobBrainWire: the brain, as much of it as travels
#include "sim/bytestream.h"

struct DistantEdgeWire;   // game/distant.h (D7b): one tick's edge traffic

namespace net {

// v1 (2026-10-16, D7a): the four records below.
// v2 (2026-10-17, D7b): DistantEdgeWire.
inline constexpr uint32_t kDistantSyncVersion = 2;

// `EntityBatch::livingFree` when the sender has nothing to report this batch
// (the host never reports; a client reports on change and on a keyframe).
inline constexpr uint32_t kNoLivingFree = 0xFFFFFFFFu;

// ---- the small parts --------------------------------------------------------

// A statue's look key as plain words (game/distant_draw.h DistantLookKey: four
// FNV hashes, a pure function of def + gear + damage on any machine running
// this build, so a key one machine baked names the same statue on the other).
struct WireLook {
  uint64_t full = 0, pristine = 0, bare = 0, def = 0;
};
// The steer state (game/steer.h SteerState).
struct WireSteer {
  float heading = 0.0f, desiredHeading = 0.0f, turnVel = 0.0f, speedNow = 0.0f;
};
// The statuses a hand-out carries (distant::DistantVitals' carried fields,
// read off the live rig by CaptureDistantVitals). A hand-IN needs none: they
// are in its patched record's v13 tail.
struct WireStatus {
  float bleedRate = 0.0f;
  int32_t bleedLimb = -1;
  uint32_t burnTicksLeft = 0;
  float burnFrac = 0.0f;
  uint32_t stumps = 0;
};

// ---- client -> host: a creature left the client's window --------------------
struct DistantHandOut {
  uint64_t id = 0;               // the creature's id, KEPT (ids are banded per machine)
  MobBrainWire brain;            // profile by name + the five facts
  uint32_t recordVersion = 0;    // MobSystem::kSaveVersion of the sender
  std::vector<uint8_t> record;   // Mob::SaveOne's bytes at the park rule, exactly
  WireSteer steer;
  Vec3 vel{};                    // voxels/sec (DemoteToDistant's: drive + fall + lunge)
  uint32_t airborne = 0;         // it left in the air: the host lands it (landPending)
  WireStatus status;
  WireLook look;                 // what the client baked from the live rig (0 = none)
};

// ---- host -> client: a remote promotion -------------------------------------
struct DistantHandIn {
  static constexpr uint32_t kMoved = 1u << 0;        // DistantMob::movedWhileDistant
  static constexpr uint32_t kLandPending = 1u << 1;  // DistantMob::landPending
  uint64_t id = 0;
  MobBrainWire brain;
  uint32_t recordVersion = 0;
  std::vector<uint8_t> record;   // MobSystem::DistantRecord(d, withStatus = true)
  WireSteer steer;
  Vec3 vel{};
  uint32_t flags = 0;
  WireLook look;                 // the host's key for it (the client prefers its own)
};

// ---- client -> host: what became of a hand-in -------------------------------
enum DistantHandInOutcome : uint32_t {
  kHandInPromoted = 1,   // it is the client's near creature now
  kHandInReturned = 2,   // the client let it go: the host's in-transit copy comes back
};
struct DistantHandInDone {
  uint64_t id = 0;
  uint32_t outcome = kHandInPromoted;
};

// ---- host -> client: the tier as the peer sees it ---------------------------
//
// ONE host creature. `flags` says which optional parts ride this entry:
//   kShape  the def name and the look key -- on first sight and on every
//           keyframe (a client that lost its mirror re-learns the shape at
//           the next one);
//   kDead   a corpse (drawn lying, never an actor);
//   kBand   inside the edge band of the PEER's window (kDistantEdgeBandVox of
//           its box, inside it too): `actor` is the creature's tick-start
//           snapshot as the host's own band publishes it (DistantActorOf);
//   kBox    its body box reaches into the peer's window (D9's DistantBox):
//           what an arrow in the peer's window tests.
// Every entry carries the statue's draw state: the draw origin (a corpse's
// y is its statue's rest height), heading, the flipbook state and gait phase.
struct DistantViewActor {
  Vec3 centre{};
  float radius = 1.0f, height = 2.0f, hpFrac = 1.0f, heading = 0.0f;
  float immersion = 0.0f, reach = 0.0f;
  uint32_t faction = 0;
  uint64_t targetId = 0;
  Vec3 velocity{};
  uint8_t armed = 0, hostile = 0, bleeding = 0, action = 0;
};
struct DistantViewEntry {
  static constexpr uint32_t kShape = 1u << 0;
  static constexpr uint32_t kDead = 1u << 1;
  static constexpr uint32_t kBand = 1u << 2;
  static constexpr uint32_t kBox = 1u << 3;
  uint64_t id = 0;
  uint32_t flags = 0;
  std::string defName;           // kShape
  WireLook look;                 // kShape
  Vec3 origin{};                 // the statue's origin (y: what it rests on)
  float heading = 0.0f;
  float gaitPhase = 0.0f;
  uint32_t state = 0;            // DistantDrawState
  DistantViewActor actor;        // kBand
  Vec3 boxLo{}, boxHi{};         // kBox (heading: the entry's)
};
struct DistantView {
  std::vector<DistantViewEntry> entries;   // ascending id
  std::vector<uint64_t> gones;             // ascending id
  bool Empty() const { return entries.empty() && gones.empty(); }
};

// ---- encode / decode --------------------------------------------------------
// Every Encode writes kDistantSyncVersion first; every Decode refuses another
// number by poisoning the reader (mobsync.cpp's ReadVersion rule).
void Encode(ByteWriter& w, const DistantHandOut& h);
bool Decode(ByteReader& r, DistantHandOut& h);
void Encode(ByteWriter& w, const DistantHandIn& h);
bool Decode(ByteReader& r, DistantHandIn& h);
void Encode(ByteWriter& w, const DistantHandInDone& d);
bool Decode(ByteReader& r, DistantHandInDone& d);
void Encode(ByteWriter& w, const DistantView& v);
bool Decode(ByteReader& r, DistantView& v);

// The bytes one view entry costs on the wire (the gate's and the smoke's
// "wire bytes per tick for the view" -- a measurement, never an input).
size_t EncodedSize(const DistantView& v);

// D7b: THE EDGE ACROSS A PEER'S WINDOW (game/distant.h DistantEdgeWire), field
// by field -- every record it holds, both directions, one codec. A kit travels
// as its STYLES (the roll reads the attacker's styles and slack, never its
// body), each StrikeProfile member by member (it has padding).
void Encode(ByteWriter& w, const ::DistantEdgeWire& e);
bool Decode(ByteReader& r, ::DistantEdgeWire& e);

}  // namespace net
