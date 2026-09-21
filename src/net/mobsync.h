// mobsync.h — what one creature looks like ON THE WIRE (PLAN_multiplayer_m9
// M9.4-B).
//
// FOUR RECORDS AND NOTHING ELSE. This header knows nothing about sockets,
// nothing about net::Link, and nothing about the tick: it is the DATA half of
// mob ownership, so that the encode/decode rules live in one file that both
// sides of a connection compile and neither side can drift from. The sending
// and receiving of these is M9.4-D's business (`main.cpp` / `session.cpp`),
// and the building and applying of them is `MobSystem`'s (game/mob.h).
//
//   MobAnnounce  "this creature exists, it is mine, and this is what it is
//                 wearing" — sent once when a peer first becomes interested.
//   MobPose      "this is where its limbs are THIS tick" — the per-tick
//                 stream from the owner; the only thing a ghost is posed from.
//   MobHandoff   "it is yours now, here is everything you need to step it" —
//                 the per-mob save record + the brain + the announce.
//   MobGone      "stop drawing it" — death, despawn, or it left my window.
//
// WHY THE HANDOFF CARRIES THE SAVE RECORD AND NOT A STRUCT OF ITS OWN:
// `Mob::SaveOne` already answers "what is this creature's damage, carve state
// and rig geometry", exactly and in one place, and the save format is gated
// (`save-entities`). A second serializer for the same facts is the divergence
// DESIGN.md guideline 3 is about. What the save record deliberately does NOT
// carry — the brain and the gear — travels BESIDE it here rather than being
// added to it, because a save file has no use for either (a loaded world
// re-derives the brain from nothing and a rising re-dresses by name) and
// widening the save format would move the world hash for a network feature.
//
// NOT A LOCAL SAVE FORMAT. `bytestream.h`'s note says raw little-endian PODs
// are fine for one machine class; a wire format between two machines is a
// stronger claim than this milestone makes, and M9's peers are two builds of
// the same binary on the same platform. Every record carries
// `kMobSyncVersion` so that the first mismatched build is REFUSED loudly
// rather than read as garbage.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/equipment.h"  // WornDamage: the damage a worn piece carries
#include "math3d.h"
#include "sim/bytestream.h"

// GLOBAL `net`, not `sandvox::net`: game/mob.h and every header it includes
// (math3d.h, sim/bytestream.h, game/equipment.h) are in the global namespace,
// and this record set is consumed from there. Qualify as `::net::` at call
// sites inside `namespace sandvox` so a sibling `sandvox::net` cannot make the
// name ambiguous.
namespace net {

// Bumped whenever any record below changes shape. A decoder that sees another
// number fails the read (ByteReader::ok goes false) instead of guessing.
inline constexpr uint32_t kMobSyncVersion = 1;

// ---- one piece of kit, BY NAME ---------------------------------------------
//
// Names, not indices: an ItemLibrary index is file order and dies on an R
// reload (item.h's index hazard), so the two machines would have to agree on a
// directory listing. `MobSystem::ApplyHandoff` re-applies each of these
// through the ordinary `Mob::WearItem` / `Mob::EquipItem` — the same path a
// rising re-dresses a corpse through (mob.cpp ServiceRisings).
//
// `WornDamage` comes from game/equipment.h rather than being restated here:
// it is a plain data struct with no behaviour, and a second copy of "what is
// missing from a breastplate" is exactly the unowned diverging representation
// DESIGN.md guideline 3 forbids. Including it costs this header no dependency
// on the mob system, the physics or the tick.
struct WireGear {
  std::string item;        // ItemDef name; "" is never sent
  int32_t equipSlot = -1;  // -1 = the held item
  uint32_t held = 0;       // 1 = held in the fist rather than worn
  uint32_t dye = 0;        // the colour the piece was dyed
  WornDamage damage;       // what is already missing from it
};

// ---- "this creature exists" -------------------------------------------------
struct MobAnnounce {
  uint64_t id = 0;
  std::string defName;     // resolved on the far side by FindOrComposeDef
  uint32_t owner = 0;      // the playerId that steps it
  std::vector<WireGear> gear;
};

// ---- one limb's world transform --------------------------------------------
// A POD so the vector travels through ByteWriter::PodVec in one memcpy. The
// INDEX travels because a severed limb has no body and is therefore not in the
// stream: position in the vector is not the rig slot.
struct WireLimbPose {
  uint32_t index = 0;
  Vec3 pos{};
  float quat[4] = {0, 0, 0, 1};
};

// ---- "this is where it is, this tick" ---------------------------------------
//
// THE ONLY THING A GHOST IS POSED FROM. No velocities, no animation phase, no
// gait state: a ghost runs no animation, so anything else here would be state
// the receiver has no code to consume. See Mob::TickGhost.
struct MobPose {
  uint64_t id = 0;
  uint32_t tick = 0;
  Vec3 origin{};
  float heading = 0;
  uint32_t alive = 1;
  std::vector<WireLimbPose> limbs;
};

// ---- the brain, as much of it as is worth carrying --------------------------
//
// ai::Brain is ~40 fields of per-tick scratch (scores, cooldown clocks, the
// nav path, the velocity estimator) that the arbiter rebuilds within a few
// ticks of taking over. What it cannot rebuild is WHO THE CREATURE WAS
// FIGHTING and where it last saw them — lose that and a handed-over duelist
// turns around and wanders off mid-fight, which is the whole visible content
// of "the handoff kept its target".
//
// The profile travels BY NAME for the same reason the gear does: `Brain
// ::profile` is an index into ai::Library, which is file order and re-resolved
// on every R reload.
struct MobBrainWire {
  std::string profile;
  uint64_t targetId = 0;
  uint32_t hasTarget = 0;
  Vec3 targetPos{};
  Vec3 lastSeenPos{};
  uint32_t lastSeenTick = 0;
};

// ---- "it is yours now" ------------------------------------------------------
struct MobHandoff {
  MobAnnounce announce;    // identity + gear (carries the id and the new owner)
  MobBrainWire brain;
  uint32_t recordVersion = 0;    // MobSystem::kSaveVersion of the sender
  std::vector<uint8_t> record;   // Mob::SaveOne's bytes, exactly
};

enum GoneReason : uint32_t {
  kGoneDeath = 0,     // it died where its owner could see it
  kGoneDespawn = 1,   // it left the owner's window
  kGoneUnowned = 2,   // nobody's window contains it any more
};

struct MobGone {
  uint64_t id = 0;
  uint32_t reason = kGoneDeath;
};

// ---- encode / decode --------------------------------------------------------
// Every Encode writes kMobSyncVersion first; every Decode refuses another
// number by poisoning the reader (`ok = false`), which is the sticky failure
// ByteReader already gives a truncated payload.
void Encode(ByteWriter& w, const MobAnnounce& a);
bool Decode(ByteReader& r, MobAnnounce& a);
void Encode(ByteWriter& w, const MobPose& p);
bool Decode(ByteReader& r, MobPose& p);
void Encode(ByteWriter& w, const MobHandoff& h);
bool Decode(ByteReader& r, MobHandoff& h);
void Encode(ByteWriter& w, const MobGone& g);
bool Decode(ByteReader& r, MobGone& g);

}  // namespace net
