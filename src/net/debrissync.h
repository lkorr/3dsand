#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "game/iteminstance.h"  // ItemInstance: a ground item's identity
#include "math3d.h"
#include "phys/physics.h"   // BodyTransform, DebrisVoxel
#include "sim/voxload.h"   // PrefabVoxel (the fine skin lattice)

// DEBRIS ON THE WIRE (PLAN_multiplayer_m9.md M9.4-C).
//
// WIRE PODS AND THEIR CODECS, AND NOTHING ELSE. There are no sockets here, no
// `net::Link`, no threads and no dependency on any other file in src/net/ —
// this header is the DESCRIPTION of what one machine has to say about a loose
// body so another machine can draw it, and the two functions that turn each
// record into bytes and back. Whoever owns the transport (M9.4-D) calls them.
//
// WHY THE RECORDS LIVE APART FROM DebrisSystem. Every field here is already a
// fact the debris system holds; a record is a PROJECTION of a `Body`, chosen
// for what the far machine needs rather than for what the near one stores.
// Keeping the projection out of debris.h is what stops "the wire format" from
// becoming a second, diverging owner of body state (design guideline 3): the
// body is the truth, these are derived and disposable.
//
// WHY NOT REUSE DebrisSystem::SaveState. A save is a whole-system snapshot
// written once at a quiet moment by the machine that also reads it; a pose is
// sent thirty times a second to a machine whose byte order and struct layout
// we are choosing to assume ONLY because this is LAN play between two builds
// of the same binary (the same assumption bytestream.h's header already
// states). The two have different cadences, different failure modes and
// different versioning clocks, so they get different formats.
//
// EVERY RECORD CARRIES A VERSION and every decode is bounds-checked through
// ByteReader's sticky `ok`, so a truncated or mismatched payload degrades to
// "this record failed to apply" rather than to a read past the buffer.

// sim/bytestream.h; only the .cpp needs the definition, and this header is
// included by debris.cpp which has no other reason to pull the stream in.
struct ByteReader;

namespace net {

// Bumped whenever any struct below changes shape. A peer that announces a
// different value is not compatible; the handshake (M9.2-A) is where that is
// noticed, and the decoders below refuse it on their own as a backstop.
// v2 (W2-M): the ground-item identity in BodyAnnounce and ItemGrant is a whole
// ItemInstance (name, count, dye, fill, damage) instead of name/dye/"damage".
constexpr uint32_t kDebrisWireVersion = 2;

// ---- identity ---------------------------------------------------------------
//
// A GLOBAL BODY ID IS `(ownerAtCreate << 48) | serial`, and that is the whole
// of the naming scheme. It needs no handshake and no allocator: each machine
// mints bodies out of its OWN player id's band, `serial` is already stable
// across the `bodies_` reshuffles that a Jolt handle is not (debris.h's
// `Body::serial`), and the two bands cannot collide because the high 16 bits
// differ. The Jolt handle can never serve: a collider rebuild mints a new one
// (see CarryStrap), so a handle names a body only until the next fire.
constexpr uint64_t kGlobalIdSerialBits = 48;
inline uint64_t MakeGlobalBodyId(uint32_t ownerAtCreate, uint32_t serial) {
  return ((uint64_t)ownerAtCreate << kGlobalIdSerialBits) | (uint64_t)serial;
}
inline uint32_t OwnerOfGlobalId(uint64_t gid) {
  return (uint32_t)(gid >> kGlobalIdSerialBits);
}
inline uint32_t SerialOfGlobalId(uint64_t gid) {
  return (uint32_t)(gid & ((1ull << kGlobalIdSerialBits) - 1ull));
}

// ---- the records ------------------------------------------------------------

// FIRST SIGHT. Everything the far machine needs to CREATE a ghost: the shape,
// the pitch of both lattices, how it renders, and — when the body is a thing
// you can pick up — the item identity WorldItems holds beside it. Sent once
// per body per peer, not per tick; the per-tick traffic is BodyPose.
struct BodyAnnounce {
  uint64_t globalId = 0;
  uint32_t owner = 0;        // who steps it right now (not always the minter)
  BodyTransform xf{};
  std::vector<DebrisVoxel> voxels;      // collider lattice, physScale units
  std::vector<PrefabVoxel> skinVoxels;  // fine skin, skinScale units; may be empty
  uint32_t physScale = 1;
  uint32_t skinScale = 1;
  uint32_t dye = 0;          // MicroBodyRef::dye — NOT derivable from the lattice
  uint32_t hadMicro = 0;     // rendered through a micro brick (the model index
                             // is meaningless across processes; the far side
                             // re-packs one from the lattice, as LoadState does)
  uint32_t bleedMat = 0;
  uint32_t dead = 0;         // came off a creature (Body::dead)
  // Ground-item identity (game/worlditems.h WorldItem's ItemInstance half),
  // `item.name` empty for matter that is not an item. By NAME, for the reason
  // worlditems.h gives: ItemLibrary indices are file-order and die on every R
  // hot-reload.
  ItemInstance item;
};

// PER TICK, FOR EVERY OWNED BODY IN THE PEER'S INTEREST SET. The smallest
// record here on purpose — this is the one that repeats.
//
// `tick` is not decoration: poses may arrive out of order on a lossy link and
// the receiver keeps the NEWEST, so an old datagram cannot drag a ghost
// backwards. `vel` rides along because a kinematic body with a stale velocity
// integrates away from where it was just put and reports every contact as a
// standing hit — the same reason DriveStraps writes one (debris.cpp).
struct BodyPose {
  uint64_t globalId = 0;
  uint32_t tick = 0;
  BodyTransform xf{};
  Vec3 vel{};
};

// AUTHORITY MOVES. The new owner recreates the body DYNAMIC (there is no
// motion-type edit that turns a ghost into a simulated body: a body created
// without `allowKinematic` cannot be switched, and one created with it carries
// the allowance forever) and adopts it; the old owner flips its copy kinematic
// and keeps it as a ghost. Both velocities travel so the body does not lose
// its momentum crossing the seam — the one property gate (c) measures.
struct BodyHandoff {
  BodyAnnounce announce;
  Vec3 vel{};
  Vec3 angVel{};
  uint32_t newOwner = 0;
};

// The owner let go of it: burnt away, settled back into the grid, culled,
// blown into particles. The ghost is destroyed. `reason` is diagnostic only.
struct BodyGone {
  uint64_t globalId = 0;
  uint32_t reason = 0;
};

// E ON A GHOST ITEM IS A REQUEST, NOT A PICKUP. Only the owner's copy of a
// body is real; a non-owner that removed its ghost would be inventing an item
// out of a render proxy, and both machines would end up holding the sword.
// So the request travels, the owner performs the ordinary pickup (the same
// code path its own E key takes) and replies with a grant.
struct ItemTake {
  uint64_t globalId = 0;
  uint32_t byPlayer = 0;
};

// THE REPLY, AND IT MAY BE A REFUSAL. `granted == 0` means the body was gone,
// was not an item, or was not mine — the asking machine must be able to tell
// "you have it" from "there was nothing there", or a failed pickup leaves a
// ghost item on the ground that nobody can ever take again.
struct ItemGrant {
  uint64_t globalId = 0;
  uint32_t toPlayer = 0;
  // The WHOLE item (W2-M): a flask granted across the wire arrives holding
  // what it held, a robe with the holes it had. It was name + dye + a damage
  // word nothing wrote, and a picked-up flask came back empty.
  ItemInstance item;
  uint32_t granted = 0;
};

// ---- codecs -----------------------------------------------------------------
//
// Each Encode APPENDS to `out` (so a caller may pack several records into one
// datagram); each Decode reads one record from a ByteReader and returns false
// on any short or version-mismatched read, leaving the reader poisoned.

void Encode(std::vector<uint8_t>& out, const BodyAnnounce& r);
void Encode(std::vector<uint8_t>& out, const BodyPose& r);
void Encode(std::vector<uint8_t>& out, const BodyHandoff& r);
void Encode(std::vector<uint8_t>& out, const BodyGone& r);
void Encode(std::vector<uint8_t>& out, const ItemTake& r);
void Encode(std::vector<uint8_t>& out, const ItemGrant& r);

bool Decode(::ByteReader& r, BodyAnnounce& out);
bool Decode(::ByteReader& r, BodyPose& out);
bool Decode(::ByteReader& r, BodyHandoff& out);
bool Decode(::ByteReader& r, BodyGone& out);
bool Decode(::ByteReader& r, ItemTake& out);
bool Decode(::ByteReader& r, ItemGrant& out);

}  // namespace net
