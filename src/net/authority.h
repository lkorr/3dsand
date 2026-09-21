// authority.h — WHO STEPS THIS? (docs/PLAN_multiplayer_m9.md §3, M9.4 package A)
//
// M9's model is that each machine is authoritative for its own player's
// controller and for the chunks and entities NEAREST ITS OWN PLAYER. That
// sentence is the whole of the milestone's correctness argument, and until
// this file it lived only in prose: `kWorldN = 512` at `kVoxelMeters = 0.10`
// is a 51.2 m cube, two players walking independently leave each other's
// window in seconds, and nothing in the tree could answer "is this mob mine?"
//
// WHAT THIS MODULE IS. Six pure functions over two `PlayerState`s' worth of
// facts. No sockets, no World, no globals, no I/O, no allocation on the hot
// path. Both machines call them with the same arguments and get the same
// answer — that is what makes ownership a DERIVED fact rather than a
// negotiated one, and it is why there is no "claim" message anywhere in the
// protocol. A negotiation can fail; arithmetic cannot.
//
// THE THREE RULES IT ENCODES, each of which cost a review finding:
//
//   1. AUTHORITY MUST BE RESIDENT (§4 finding 6). The winner is the nearest
//      machine AMONG THOSE WHOSE SENT WINDOW CONTAINS THE CHUNK. Plain
//      nearest-player can name a machine that does not have the chunk in
//      memory, which would hand a mob to a peer that cannot step it. The
//      candidate test is geometric and uses the window origin the peer SENT,
//      not anything local.
//   2. THE MARGIN IS NOT DECORATION (§4 finding 1). A chunk on the window's
//      outermost plane sees SOLID on one side and real neighbours on the
//      other, so its CA is not comparable between two machines and its owner
//      must not be the machine that only barely has it. Candidacy therefore
//      requires `kAuthorityMargin` chunks of slack on every face.
//   3. HYSTERESIS, OR THE BOUNDARY FLAPS. Two players walking side by side
//      sit at equal distance from a whole plane of chunks; without memory,
//      one step of one player re-homes hundreds of entities per tick and
//      every handoff is a wire message. The incumbent therefore KEEPS what it
//      holds unless it stops being a candidate or a challenger is at least
//      `kAuthorityHysteresis` chunks nearer.
//
// PURITY, AND WHY THE MEMORY IS A PARAMETER. Hysteresis needs state, and
// DESIGN.md §10 forbids a sim-affecting process global keyed on the residency
// window — which is exactly what a file-static incumbent map would be. So the
// memory is the CALLER's (`MobSystem`'s for mobs, `DebrisSystem`'s for
// bodies, the session's for chunks), it is passed in by const reference, and
// the query returns the winner without touching it. The caller writes the
// result back with `Remember()`. Two consequences worth stating: the gate can
// drive a hundred hysteresis cases with no fixture at all, and a caller that
// wants to ASK without committing (a UI overlay, a debug print) cannot
// accidentally move the boundary by asking.

#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>

#include "math3d.h"
#include "sim/oprecord.h"
#include "sim/world.h"

namespace net {

// ---- the facts one peer publishes about itself ---------------------------

// Everything the authority arithmetic is allowed to read about a machine.
// Deliberately NOT a pointer to that machine's session: on the receiving side
// there is no such object, only the last `PlayerState` that arrived, and a
// function that could reach further would stop giving the same answer on both
// ends. `windowOrigin` rides `PlayerState` for exactly this reason.
//
// `chunk` is the peer's own feet chunk — the distance metric's anchor.
// `windowOrigin` is the low corner of its resident cube in chunk coords.
struct PeerView {
  uint32_t playerId = 0;
  IVec3 chunk{};        // the peer's feet, in world CHUNK coords
  IVec3 windowOrigin{}; // low corner of the peer's resident cube, chunks
  bool connected = false;
};

// The caller's incumbent table: key -> playerId that held it last tick.
// Chunks key on `World::PackChunkKey(wc)`; entities key on the entity id.
// One type for both because the two tables never mix keys — a chunk key has
// its three 21-bit fields packed with a +2^20 bias, an entity id is a mob or
// body serial, and each caller owns exactly one of these maps.
using AuthorityMemory = std::unordered_map<uint64_t, uint32_t>;

// No candidate. 0 is a legal playerId (the host takes it), so "nobody" needs
// a value outside the id space rather than a zero.
constexpr uint32_t kNoAuthority = 0xFFFFFFFFu;

// Chunks of slack required on every face for a machine to be a candidate.
// 1 = "not on the outermost plane", which is rule 2 above at its minimum.
constexpr int kAuthorityMargin = 1;

// The margin `Comparable` asks for: §4 finding 1's "≥ 2 chunks inside BOTH
// windows". A chunk one plane in still has a neighbour that is itself an edge
// chunk, so its CA can differ in the second ring; two is the first depth at
// which the 3×3×3 neighbourhood of the neighbourhood is real on both sides.
constexpr int kComparableMargin = 2;

// How much nearer a challenger must be to take a held chunk. Two chunks is
// 32 voxels = 3.2 m at kVoxelMeters — a player has to genuinely commit to the
// boundary, not lean over it.
constexpr int kAuthorityHysteresis = 2;

// ---- geometry ------------------------------------------------------------

// Chebyshev distance in chunks. Chebyshev and not Euclidean because the
// residency window is a CUBE: the set of chunks within Chebyshev distance d
// is a cube concentric with it, so "nearer" and "deeper inside my window"
// point the same way. A Euclidean metric would prefer a diagonal chunk that
// is closer to the player but nearer the window's corner.
int ChunkChebyshev(IVec3 a, IVec3 b);

// Does the window at `origin` contain `wc` with `margin` chunks of slack on
// every face? The window covers [origin, origin + kNChunk) per axis
// (world.h's `ChunkInWindow`), so the margin test is `wc - origin` in
// [margin, kNChunk - margin) per axis. margin 0 is exactly `ChunkInWindow`.
bool ResidentWithMargin(IVec3 wc, IVec3 origin, int margin);

// §4 finding 1: may the two machines' copies of `wc` be COMPARED at all?
// Only if it is `margin` chunks inside BOTH windows. Used by the chunk-hash
// convergence work (M9.3) and stated here because it reads the same two
// origins the authority does and must not drift from them.
bool Comparable(IVec3 wc, IVec3 myOrigin, IVec3 peerOrigin,
                int margin = kComparableMargin);

// The chunk a voxel position belongs to. kChunk is 16, so the shift is 4 —
// but written as a shift of the FLOORED coordinate, because -1 / 16 is 0 in
// C++ and would put every voxel in the chunk below the origin into the chunk
// above it.
IVec3 ChunkOfVoxel(IVec3 vox);
IVec3 ChunkOfFeet(Vec3 feetVox);

// ---- the queries ---------------------------------------------------------

// Who owns `wc`? Pure: reads `mem` for the incumbent and does not write it.
// Returns `kNoAuthority` when no connected peer has the chunk with margin —
// which is a real and common answer (a chunk between two distant players is
// nobody's, and nothing steps it because nobody has it resident).
uint32_t ChunkAuthority(IVec3 wc, std::span<const PeerView> peers,
                        const AuthorityMemory& mem);

// Who owns the entity at `feetVox`? The chunk rule, but with the entity's OWN
// memory slot, keyed by `entityId` and not by the chunk. That is the whole
// difference and it is load-bearing: a mob standing on a chunk boundary
// changes chunk every few ticks, and a chunk-keyed incumbent would hand it
// back and forth at the rate it steps. Keyed on the entity, the hysteresis is
// about the ENTITY's relationship with the two players, which is the thing
// that actually changes slowly.
uint32_t EntityAuthority(uint64_t entityId, Vec3 feetVox,
                         std::span<const PeerView> peers,
                         const AuthorityMemory& mem);

// Overload for callers that already hold the entity's chunk.
uint32_t EntityAuthorityChunk(uint64_t entityId, IVec3 wc,
                              std::span<const PeerView> peers,
                              const AuthorityMemory& mem);

// Commit a query's result. Separate from the query so that asking cannot move
// the boundary (see the header comment). `kNoAuthority` ERASES rather than
// storing a sentinel, so a chunk nobody can reach does not keep a stale
// incumbent that would win the moment somebody walks back into range.
void Remember(AuthorityMemory& mem, uint64_t key, uint32_t owner);

// ---- the producer rule ---------------------------------------------------

// Does producer `p` need chunk authority, or is it author-gated?
//
// Brush / Spell / Avatar / Lab / EditLayer are PLAYER-AUTHORED: the player
// may paint, cast, bleed and drop things anywhere their arm reaches, and the
// gate on them is the AUTHOR (the batch is tagged with who sent it and the
// receiver drops a batch from a peer it does not know), not the chunk. Mob /
// Debris / Worldgen are WORLD-AUTHORED: they are the single-producer systems
// of §4 finding 4, and two machines both running them over one chunk is a
// duplicated op, not a race — so exactly one machine may.
bool ProducerNeedsChunkAuthority(uint8_t p);

// The full rule. `me` is the local playerId.
//
//   Brush/Spell/Avatar/Lab/EditLayer -> always
//   Mob/Debris/Worldgen             -> ChunkAuthority(wc) == me
//   Remote                          -> never (it is the peer's op; it was
//                                      applied because the peer owned it,
//                                      and re-emitting it here would double)
//   Unknown                         -> never, and the caller counts it
//                                      SEPARATELY: `Unknown` on a connected
//                                      run means a producer never adopted an
//                                      author scope, which is a missing
//                                      annotation rather than a trespass.
bool Owns(uint8_t producer, IVec3 wc, uint32_t me,
          std::span<const PeerView> peers, const AuthorityMemory& mem);

// ---- the SubmitTick hook -------------------------------------------------
//
// NULL UNLESS A CONNECTED GAME SETS IT, and that is the hash argument: with
// no hook, `SubmitTick` pays one null pointer test per tick and emits exactly
// the same bytes it did before this package. Single-player, every gate, both
// smokes and the whole op record are bit-identical, which is why this
// package's section says the determinismHash may not move.
//
// The hook OBSERVES; it never refuses an op. A violation is a bug in the
// ownership plumbing, and dropping the op would hide it behind a missing
// voxel — CLAUDE.md rule 6's failure mode exactly. So the default is COUNT
// (into build/last_run.json's `opstream` block, with the first offender's
// producer, chunk and rightful owner beside the count) and only
// SANDVOX_NET_STRICT=1 aborts.
struct AuthorityHook {
  uint32_t me = 0;                       // the local playerId
  std::span<const PeerView> peers{};     // every peer INCLUDING me
  const AuthorityMemory* chunkMemory = nullptr;  // may be null (no hysteresis)
  bool strict = false;                   // SANDVOX_NET_STRICT=1
};

void SetAuthorityHook(const AuthorityHook* h);
const AuthorityHook* Hook();

// SANDVOX_NET_STRICT, read once. Exposed so main can fill `strict` from the
// same read the hook's default uses.
bool StrictEnv();

}  // namespace net
