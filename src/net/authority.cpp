// authority.cpp — the arithmetic behind net/authority.h.
//
// Every function here is pure except `SetAuthorityHook`/`Hook`, which are a
// pointer the game installs once and the harness leaves null. Nothing in this
// file reads a clock, a file, an environment variable per call, or any global
// that a second player could disagree about.

#include "net/authority.h"

#include <cstdlib>
#include <cstring>

namespace net {

int ChunkChebyshev(IVec3 a, IVec3 b) {
  const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
  const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
  const int dz = a.z > b.z ? a.z - b.z : b.z - a.z;
  int m = dx > dy ? dx : dy;
  return m > dz ? m : dz;
}

bool ResidentWithMargin(IVec3 wc, IVec3 origin, int margin) {
  // The window covers [origin, origin + kNChunk) per axis (world.h's
  // ChunkInWindow). With a margin, the acceptable band shrinks from BOTH
  // ends: [origin + margin, origin + kNChunk - margin).
  //
  // A margin at or past half the window admits nothing; that is not an error
  // to report, it is just an empty band, and returning false for every chunk
  // is the honest answer. (kNChunk is 32, kComparableMargin is 2, so this is
  // theoretical — but a future kWorldN shrink would make it reachable and a
  // silent wrap-around would be worse than "nobody owns anything".)
  const int n = (int)::kNChunk;
  const int lo = margin, hi = n - margin;
  if (lo >= hi) return false;
  const int dx = wc.x - origin.x, dy = wc.y - origin.y, dz = wc.z - origin.z;
  return dx >= lo && dx < hi && dy >= lo && dy < hi && dz >= lo && dz < hi;
}

bool Comparable(IVec3 wc, IVec3 myOrigin, IVec3 peerOrigin, int margin) {
  return ResidentWithMargin(wc, myOrigin, margin) &&
         ResidentWithMargin(wc, peerOrigin, margin);
}

IVec3 ChunkOfVoxel(IVec3 vox) {
  // Arithmetic shift, not division: >> 4 floors toward negative infinity for
  // signed values on every compiler this repo builds with, and the world has
  // negative coordinates everywhere below the spawn plane. `-1 / 16 == 0`
  // would put the first voxel below chunk 0 back INTO chunk 0, which is a
  // one-chunk-wide seam at the origin that no gate at positive coordinates
  // would ever see.
  return {vox.x >> 4, vox.y >> 4, vox.z >> 4};
}

IVec3 ChunkOfFeet(Vec3 feetVox) {
  return ChunkOfVoxel({ifloor(feetVox.x), ifloor(feetVox.y), ifloor(feetVox.z)});
}

namespace {

// The shared body of both queries. `memKey` selects the incumbent slot: the
// packed chunk key for chunks, the entity id for entities. That one parameter
// is the entire difference between the two public entry points, and it is
// what keeps a mob straddling a chunk edge from flapping (see the header).
uint32_t Decide(IVec3 wc, std::span<const PeerView> peers,
                const AuthorityMemory& mem, uint64_t memKey) {
  // Pass 1: the best CANDIDATE, by (Chebyshev distance, then lower id).
  //
  // The tie-break on id is not cosmetic. Two players walking abreast are at
  // equal distance from a whole plane of chunks every tick, so ties are the
  // COMMON case at a boundary, not an edge case; "lower id wins" is a total
  // order both machines compute from the same numbers, which is what stops
  // the two of them from each concluding that the other owns it and nobody
  // stepping the chunk at all.
  uint32_t best = kNoAuthority;
  int bestDist = 0;
  for (const PeerView& p : peers) {
    if (!p.connected) continue;
    if (!ResidentWithMargin(wc, p.windowOrigin, kAuthorityMargin)) continue;
    const int d = ChunkChebyshev(wc, p.chunk);
    if (best == kNoAuthority || d < bestDist ||
        (d == bestDist && p.playerId < best)) {
      best = p.playerId;
      bestDist = d;
    }
  }
  if (best == kNoAuthority) return kNoAuthority;

  // Pass 2: HYSTERESIS. The incumbent keeps it unless it has stopped being a
  // candidate, or the challenger is at least kAuthorityHysteresis nearer.
  //
  // Note the incumbent is re-validated against the SAME candidate test rather
  // than trusted: a peer that disconnected, or that walked far enough for the
  // chunk to leave its window, is no longer eligible to hold anything, and an
  // incumbent check that only compared distances would let a disconnected
  // machine keep a chunk forever.
  const auto it = mem.find(memKey);
  if (it == mem.end()) return best;
  const uint32_t incumbent = it->second;
  if (incumbent == best) return best;
  for (const PeerView& p : peers) {
    if (p.playerId != incumbent) continue;
    if (!p.connected) break;
    if (!ResidentWithMargin(wc, p.windowOrigin, kAuthorityMargin)) break;
    const int incDist = ChunkChebyshev(wc, p.chunk);
    // `bestDist <= incDist` always holds here (pass 1 picked the minimum), so
    // this is "did the challenger beat it by enough", written as a subtraction
    // that cannot underflow because both are non-negative ints.
    if (incDist - bestDist < kAuthorityHysteresis) return incumbent;
    break;
  }
  return best;
}

}  // namespace

uint32_t ChunkAuthority(IVec3 wc, std::span<const PeerView> peers,
                        const AuthorityMemory& mem) {
  return Decide(wc, peers, mem, ::World::PackChunkKey(wc));
}

uint32_t EntityAuthorityChunk(uint64_t entityId, IVec3 wc,
                              std::span<const PeerView> peers,
                              const AuthorityMemory& mem) {
  return Decide(wc, peers, mem, entityId);
}

uint32_t EntityAuthority(uint64_t entityId, Vec3 feetVox,
                         std::span<const PeerView> peers,
                         const AuthorityMemory& mem) {
  return EntityAuthorityChunk(entityId, ChunkOfFeet(feetVox), peers, mem);
}

void Remember(AuthorityMemory& mem, uint64_t key, uint32_t owner) {
  // kNoAuthority ERASES. A chunk with no candidate must not keep an incumbent
  // that would win unchallenged the instant somebody walks back into range —
  // that is a stale claim surviving a disconnect, which is the one way a pure
  // function can still produce a wrong answer.
  if (owner == kNoAuthority) mem.erase(key);
  else mem[key] = owner;
}

bool ProducerNeedsChunkAuthority(uint8_t p) {
  using P = sandvox::opstream::Producer;
  switch ((P)p) {
    case P::Mob:
    case P::Debris:
    case P::Worldgen:
      return true;
    default:
      return false;
  }
}

bool Owns(uint8_t producer, IVec3 wc, uint32_t me,
          std::span<const PeerView> peers, const AuthorityMemory& mem) {
  using P = sandvox::opstream::Producer;
  switch ((P)producer) {
    case P::Brush:
    case P::Spell:
    case P::Avatar:
    case P::EditLayer:
    case P::Lab:
      // Author-gated, not chunk-gated. The player's arm reaches where it
      // reaches; the receiver drops a batch whose author it does not know.
      return true;
    case P::Mob:
    case P::Debris:
    case P::Worldgen:
      return ChunkAuthority(wc, peers, mem) == me;
    case P::Remote:
      // A remote op was applied because the PEER owned it. Re-emitting it
      // locally would double the effect, so the local machine never "owns" a
      // Remote op — it only replays one.
      return false;
    case P::Unknown:
    default:
      // Counted separately by the caller: this is a producer that never
      // adopted an author scope, i.e. a missing annotation, not a trespass.
      return false;
  }
}

// ---- the hook ------------------------------------------------------------

namespace {
const AuthorityHook* g_hook = nullptr;
}

void SetAuthorityHook(const AuthorityHook* h) { g_hook = h; }
const AuthorityHook* Hook() { return g_hook; }

bool StrictEnv() {
  // Read once. Not `static const bool` inside the hot path for its own sake —
  // the hook is null in every harness so this is called at most once per
  // process — but because an env var that could change mid-run would make the
  // abort decision depend on something no record carries.
  static const bool v = [] {
    const char* e = std::getenv("SANDVOX_NET_STRICT");
    return e && e[0] && std::strcmp(e, "0") != 0;
  }();
  return v;
}

}  // namespace net
