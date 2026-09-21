#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "net/authority.h"  // PeerView, Comparable, ChunkAuthority, memory
#include "net/protocol.h"   // MsgType, kOpDelayTicks
#include "sim/world.h"      // World, IVec3, kChunkVol

// M9.3 package C: THE CONVERGENCE HALF (docs/PLAN_multiplayer_m9.md §M9.3-C).
//
// WHAT PROBLEM THIS SOLVES, and why packages A and B do not already solve it.
// M9.3-B made two machines submit the SAME op vectors under the SAME tick
// label. Determinism (CLAUDE.md rule 1) then says the two worlds stay equal
// forever, and for every chunk both machines hold, that is true — right up
// until it is not. The ways it stops being true are all real and none of them
// is a bug in the CA:
//
//   * A CHUNK ONE MACHINE DOES NOT HOLD. The residency window is 51.2 m on a
//     side; a peer twelve chunks away simply has no copy, its CA never ran
//     there, and when it walks back in the chunk is re-generated from the seed
//     with every edit the other player made to it missing (that is M9.5's
//     ChunkStore, not this package — but the same divergence shows here first).
//   * A DROPPED OP. `SubmitTick` clamps every stream to its cap and COUNTS the
//     refusals (CLAUDE.md rule 3). Two machines whose op vectors are equal but
//     whose CAPS bite at different points — because one of them also had a
//     local-only producer running — submit different worlds.
//   * A WINDOW EDGE. §4 finding 1: a chunk on the outermost plane of the
//     window sees SOLID on one side and real neighbours on the other, so its
//     CA is not even SUPPOSED to agree between two machines whose windows are
//     offset. `net::Comparable` is the rule that says which chunks are allowed
//     to be compared at all, and it lives in authority.h beside the ownership
//     arithmetic because the two read the same two origins.
//   * ANY FUTURE DEFECT. The honest reason: "the sim is deterministic" is a
//     property of the code, and a shared world with no way to DETECT a
//     divergence is a shared world in which the first defect is silent and
//     permanent. The hash tree is the alarm; the resync is the repair.
//
// THE SHAPE, in four messages and one rule.
//
//   1. PUBLISH.  Every hash tick each machine sends `HashBlocks`: one 32-bit
//      sum per 4x4x4 BLOCK of chunks, for the blocks whose 64 chunks are all
//      comparable and all QUIET. 512^3 is 32,768 chunks = 512 blocks, so a
//      full publish is at most ~8 KiB and in practice far less.
//   2. DRILL.    A receiver whose own sum for a block differs sends
//      `HashDrill{block}` and gets `HashChunks{block, 64 digests}` back. Two
//      round trips to go from "somewhere in 512 chunks" to "these three".
//   3. REQUEST.  For each differing chunk whose AUTHORITY is the peer
//      (`net::ChunkAuthority`, the same pure function both machines run), the
//      requester sends `ChunkRequest{wc}`. The authority answers `ChunkSync`
//      slices from its fetch cache — or `ChunkBusy{wc}` if the chunk is not
//      quiet enough to be shipped, and the requester retries next hash tick.
//   4. REPLACE.  The reassembled RLE is decoded to 4,096 words and handed to
//      `Stream::ReplaceChunk` at the phase-B position of the next tick, so the
//      tick's CA sees the corrected chunk. `opstream::NoteChunkReplace` puts
//      it in the op record, which is what keeps a replay reproducible.
//
// THE ONE RULE THAT MAKES ALL OF IT SOUND: ONLY QUIET CHUNKS ARE COMPARED.
// `World::QuietTicks(slot) >= kSyncQuietTicks` on BOTH ends. It buys three
// separate things and it is worth naming each, because dropping it would
// break them in three different ways:
//
//   (a) TICK SKEW STOPS MATTERING. The two machines' digest tables describe
//       different ticks — `World::ChunkHashTick()` is K ticks latent and the
//       two hash schedules are not phase-locked. Comparing a chunk that is
//       still churning would report a mismatch for every such chunk on every
//       publish, which is noise, not a signal. A chunk that has been quiet for
//       K + D ticks has the same digest at H and at H', so the comparison is
//       exact without either side keeping a history of tables.
//   (b) THE REPLACE HAS NOTHING TO CLOBBER. See the KNOWN GAP below.
//   (c) THE ALARM ONLY RINGS FOR REAL. A divergence in settled matter is a
//       divergence that will not fix itself; one in a burning tree might.
//
// ---- KNOWN GAP, STATED SO IT IS NOT A SURPRISE (plan §M9.3-C build step 5)
//
// A REPLACED CHUNK'S IN-FLIGHT PARTICLES, GAS AND MPM MATTER ARE NOT FLUSHED.
// `Stream::ReplaceChunk` overwrites 4,096 voxel words. It does not touch the
// particle buffer, the gas grid or the MLS-MPM live set, so a particle that was
// mid-flight through the chunk keeps flying and will land on the NEW contents,
// and an MPM body straddling the boundary keeps its off-grid state.
//
// THE QUIET REQUIREMENT IS THE ARGUMENT THAT THERE ARE NONE. A chunk with no
// dirty flag for K + D published ticks has had no CA activity in it for longer
// than any of those three systems' residence time in a single chunk, so the
// sets are empty by construction rather than by cleanup. That argument is
// exactly as strong as the quiet threshold, which is why `kSyncQuietTicks` is
// a named constant with this paragraph attached and not an inline `>= 8`.
// If a future package ever syncs a BUSY chunk (a late-join bulk transfer, say),
// it owes a flush of all three and this comment is the checklist.
//
// ---- WHAT IS NOT HERE
//
// No sockets: the class produces and consumes byte payloads and main.cpp's
// `netPump` moves them, exactly as it does for `TickBatch`. No globals. No
// World mutation except `RequestChunkFetch` (a queue push) — the actual
// replace happens in phase B, through Stream, on the tick that owns it.
namespace net {

// Bumped when any message below changes layout. Rides inside each payload
// rather than in the frame header, for the reason protocol.h gives: the frame
// header is shared by three packages and a bump there is everybody's bump.
constexpr uint32_t kChunkSyncVersion = 1;

// ---- the tree's shape ----------------------------------------------------

// A block is 4x4x4 chunks = 64 chunks = 64 x 64 x 64 voxels. Why four and not
// two or eight: the message is one entry per block, so the publish cost falls
// as the cube of this number while the DRILL cost (64 digests, 256 B) rises
// linearly. At 4 the whole-window publish is 512 entries (8 KiB, under the
// 2 KiB-per-message target only because the quiet+comparable filter keeps the
// real count far below the maximum) and a drill is one 268-byte reply. At 8 a
// drill would be 512 digests and would mostly re-send data the block sum had
// already proven equal.
constexpr int kHashBlockShift = 2;
constexpr uint32_t kChunksPerBlock = 64;  // 1 << (3 * kHashBlockShift)

// THE QUIET THRESHOLD, and the three things it buys are in the header comment.
// K + D: the snapshot latency (the digest table describes tick T - K) plus the
// op delay (an op authored D ticks ago has not been submitted yet, so a chunk
// that looks quiet could still have an edit inbound). Both are compile-time
// constants, so this is one too.
constexpr uint32_t kSyncQuietTicks = World::kSnapshotLatency + kOpDelayTicks;

// At most this many chunks may be in flight at once, nearest to the local
// player first. A mixed surface chunk RLEs to ~32 KiB (stream.cpp), so four is
// ~128 KiB of wire per repair round — a tenth of a second on a LAN, and the
// cap is what keeps a wholesale divergence (a dropped op stream, a desynced
// worldgen) from turning into a 512 MiB blast that starves the tick batches
// the pacer is waiting on.
constexpr uint32_t kMaxChunksInFlight = 4;

// One ChunkSync message carries at most this many BYTES of RLE. Slices are
// reassembled by the receiver. 4 KiB because net/link.h treats a frame over
// 4 MiB as a protocol error and a single 32 KiB message would sit in front of
// a tick batch on the same TCP stream for longer than D ticks of slack.
constexpr size_t kChunkSyncSliceBytes = 4096;

// How many hash ticks a request may go unanswered before it is abandoned and
// re-detected from scratch. The authority answers from its FETCH CACHE, which
// lands 1-2 ticks plus K after the request — so this is generous by design,
// and a request that expires is a counted number rather than a stuck sync.
constexpr uint32_t kRequestTimeoutTicks = 60;

// ---- messages ------------------------------------------------------------

// One block and the wrapping sum of its 64 chunk digests. Wrapping and not a
// mixing fold: the SUM of the 64 already-mixed digests is order-independent,
// which means neither side has to agree on a traversal order, and a single
// changed chunk changes the sum with probability 1 - 2^-32.
struct HashBlockEntry {
  int32_t bx = 0, by = 0, bz = 0;  // block coords = world chunk coords >> 2
  uint32_t sum = 0;
};

// What one machine publishes on a hash tick.
//
// `hashTick` is `World::ChunkHashTick()`: the tick the DIGEST TABLE describes,
// not the tick this was sent on. The receiver never compares it against its
// own — see rule (a) in the header comment, the quiet filter is what makes the
// two tables comparable across skew — but it is carried because a drill has to
// name which publish it is drilling into, and because a report that cannot say
// which tick a mismatch was at cannot be debugged.
struct HashBlocksMsg {
  uint32_t version = kChunkSyncVersion;
  uint32_t hashTick = 0;
  int32_t origin[3] = {0, 0, 0};  // the sender's window origin AS OF hashTick
  std::vector<HashBlockEntry> blocks;
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// "Your sum for this block differs from mine; send me the 64."
struct HashDrillMsg {
  uint32_t version = kChunkSyncVersion;
  uint32_t hashTick = 0;
  int32_t b[3] = {0, 0, 0};
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// The 64 digests of one block, in CANONICAL ORDER: x fastest, then y, then z,
// which is `BlockChunk(block, i)` below. Both sides index by the same `i`, so
// a disagreement at index i names a chunk without the message carrying 64
// coordinates.
struct HashChunksMsg {
  uint32_t version = kChunkSyncVersion;
  uint32_t hashTick = 0;
  int32_t b[3] = {0, 0, 0};
  uint32_t digest[kChunksPerBlock] = {};
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// "Send me your copy of this chunk." Only ever sent to the chunk's AUTHORITY.
struct ChunkRequestMsg {
  uint32_t version = kChunkSyncVersion;
  uint32_t hashTick = 0;
  int32_t wc[3] = {0, 0, 0};
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// "Not now." Either the chunk is not resident here, or it is not quiet enough
// to ship (the KNOWN GAP's precondition). The requester retries next hash tick;
// this is a BACK-OFF, not a refusal, and the two must not be conflated in the
// report because one of them is normal and the other is a bug.
struct ChunkBusyMsg {
  uint32_t version = kChunkSyncVersion;
  int32_t wc[3] = {0, 0, 0};
  uint32_t reason = 0;  // 0 = not resident, 1 = not quiet
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// One slice of one chunk's RLE. `pairs` is the TOTAL pair count of the whole
// RLE (stream.h's RleEncodeChunk emits (word, runLength) pairs as u32s), so
// the receiver can size its buffer from the first slice it sees.
struct ChunkSyncMsg {
  uint32_t version = kChunkSyncVersion;
  uint32_t tick = 0;  // the authority's tick when it encoded (report only)
  int32_t wc[3] = {0, 0, 0};
  uint32_t slice = 0, slices = 0;
  uint32_t pairs = 0;
  std::vector<uint8_t> bytes;
  void Encode(std::vector<uint8_t>& out) const;
  bool Decode(const uint8_t* p, size_t n);
};

// ---- the tree ------------------------------------------------------------

// Pure block arithmetic + the two folds over World's digest table. Static
// because there is no state: everything it needs is the World's published
// snapshot and the two origins.
struct HashTree {
  // Arithmetic shift, not division: -1 >> 2 is -1 and -1 / 4 is 0, and the
  // second would put every chunk in the block below the origin into the block
  // above it — the same trap authority.h's ChunkOfVoxel documents.
  static IVec3 BlockOfChunk(IVec3 wc) {
    return {wc.x >> kHashBlockShift, wc.y >> kHashBlockShift,
            wc.z >> kHashBlockShift};
  }
  // The i'th chunk of a block, x fastest. THE canonical order; both ends index
  // HashChunksMsg::digest with it.
  static IVec3 BlockChunk(IVec3 b, uint32_t i) {
    const int n = 1 << kHashBlockShift;
    return {(b.x << kHashBlockShift) + (int)(i % (uint32_t)n),
            (b.y << kHashBlockShift) + (int)((i / (uint32_t)n) % (uint32_t)n),
            (b.z << kHashBlockShift) + (int)(i / (uint32_t)(n * n))};
  }

  // May this chunk's digest be compared with the peer's at all? Comparable
  // under BOTH origins (authority.h, §4 finding 1) AND quiet on this machine
  // for kSyncQuietTicks. The caller supplies `world` for the quiet streak and
  // for the residency test.
  static bool ChunkSyncable(const World& world, IVec3 wc, IVec3 myOrigin,
                            IVec3 peerOrigin);

  // The 64 digests of one block, or false if ANY of its chunks is not
  // syncable. All-or-nothing on purpose: a block sum over a subset is a sum
  // over a set the two machines might disagree about, and then a mismatch
  // would mean "we chose different subsets" rather than "the world differs".
  static bool BlockDigests(const World& world, IVec3 b, IVec3 myOrigin,
                           IVec3 peerOrigin, uint32_t out[kChunksPerBlock]);

  // Every syncable block of the local window, with its sum. Deterministic
  // order (ascending block coords) so two runs of the same state produce the
  // same message bytes — which is what lets a gate compare them.
  static void Build(const World& world, IVec3 myOrigin, IVec3 peerOrigin,
                    std::vector<HashBlockEntry>& out);
};

// ---- the state machine ---------------------------------------------------

// Owned by main() (the frame loop is what polls the socket), borrowed by
// TickAuthorityCtx, and NULL IN EVERY HARNESS except the gate that drives it
// directly. Null or disconnected means phase B does nothing it did not do
// before this package, which is the oracle-comparison acceptance criterion.
class ChunkSync {
 public:
  // One outgoing message, ready for link->Send. The class never touches a
  // socket; main.cpp drains this vector after every call that takes one.
  struct Out {
    MsgType type;
    std::vector<uint8_t> payload;
  };

  // One chunk waiting to be installed by phase B.
  struct PendingReplace {
    IVec3 wc{};
    std::vector<uint32_t> words;  // kChunkVol
  };

  void Connect(uint32_t localId, uint32_t peerId);
  void Disconnect();
  bool Connected() const { return connected_; }
  uint32_t LocalId() const { return localId_; }
  uint32_t PeerId() const { return peerId_; }

  // ---- the two origin rings ---------------------------------------------
  //
  // A comparison at hash tick H must use the origins AS OF H, not the ones
  // both machines have now: the digest table is K ticks latent and a walking
  // player shifts the window inside that. Keyed by tick, pruned to
  // kOriginRingTicks, and `OriginsAt` answers false rather than guessing when
  // the tick has aged out — a comparison with a guessed origin would compare
  // chunks that were never comparable and report a divergence that is really
  // a window edge.
  static constexpr uint32_t kOriginRingTicks = 64;
  void NoteOwnTick(uint32_t tick, IVec3 origin, IVec3 playerChunk);
  void NotePeerState(uint32_t tick, IVec3 origin, IVec3 playerChunk);
  bool OriginsAt(uint32_t tick, IVec3& own, IVec3& peer) const;

  // ---- the frame-loop driver --------------------------------------------
  //
  // Called once per frame from netPump, AFTER the messages have been drained.
  // Publishes a HashBlocks when the digest table has advanced to a tick it has
  // not published yet, services the outbox (fetches that have landed), expires
  // stale requests and issues queued requests up to the in-flight cap.
  void Pump(World& world, uint32_t nowTick, std::vector<Out>& out);

  // One received message. Everything this class understands; anything else is
  // main.cpp's. `world` is non-const because answering a ChunkRequest pushes a
  // fetch.
  void OnMessage(MsgType t, const uint8_t* p, size_t n, World& world,
                 uint32_t nowTick, std::vector<Out>& out);

  // ---- phase B ----------------------------------------------------------
  // Everything that arrived complete since the last call, MOVED OUT. Phase B
  // owns them from here: it calls Stream::ReplaceChunk and records each one.
  std::vector<PendingReplace> TakePending();
  bool HavePending() const { return !pending_.empty(); }

  // Phase B reports back, because the class cannot see the Stream and
  // "applied" vs "refused because the window moved under it" are the two
  // numbers the plan's acceptance names.
  void NoteApplied() { syncsApplied++; }
  void NoteRefusedNotResident() { syncsRefusedNotResident++; }

  // ---- the numbers a report can name (CLAUDE.md rule 6) ------------------
  uint64_t hashBlocksSent = 0, hashBlocksRecv = 0;
  uint64_t blockEntriesSent = 0, blockEntriesCompared = 0;
  uint64_t blockMismatches = 0;   // blocks whose sums differed
  uint64_t chunkMismatches = 0;   // chunks a drill reply proved differ
  uint64_t drillsSent = 0, drillsRecv = 0;
  uint64_t requestsSent = 0, requestsRecv = 0;
  uint64_t busySent = 0, busyRecv = 0;
  uint64_t slicesSent = 0, slicesRecv = 0;
  uint64_t bytesShipped = 0;
  uint64_t syncsApplied = 0, syncsRefusedNotResident = 0;
  uint64_t requestsExpired = 0;
  uint64_t notMine = 0;  // mismatched chunks where WE are the authority

  // THE SERIES, not the total. "12 mismatches" over a run says nothing about
  // whether the repair worked; "3, 3, 0, 0, 2, 0" says it fired twice and
  // converged both times. One entry per received HashBlocks.
  struct MismatchPoint {
    uint32_t hashTick;
    uint32_t blocks;   // blocks whose sums differed
    uint32_t compared; // blocks both ends published
  };
  std::vector<MismatchPoint> series;

  // For the gate and the HUD.
  size_t InFlight() const { return inFlight_.size(); }
  size_t Wanted() const { return wanted_.size(); }

 private:
  // A chunk we have decided we need from the peer, not yet requested.
  struct Want {
    IVec3 wc{};
    uint32_t hashTick = 0;
  };
  // A request on the wire, waiting for slices or a busy. NOT named InFlight:
  // that is the accessor above, and a member function and a nested type of the
  // same name are not both nameable.
  struct Inbound {
    uint32_t askedTick = 0;
    uint32_t pairs = 0, slices = 0, have = 0;
    std::vector<uint8_t> bytes;
  };
  // A request WE must answer: the fetch was pushed, the cache has not landed.
  struct Outbox {
    IVec3 wc{};
    uint32_t askedTick = 0;
  };

  void QueueWant(IVec3 wc, uint32_t hashTick);
  void IssueRequests(const World& world, uint32_t nowTick,
                     std::vector<Out>& out);
  void ShipChunk(const World& world, IVec3 wc, const uint32_t* words,
                 uint32_t nowTick, std::vector<Out>& out);
  void SendBusy(IVec3 wc, uint32_t reason, std::vector<Out>& out);
  std::vector<PeerView> Peers(IVec3 own, IVec3 peer) const;

  bool connected_ = false;
  uint32_t localId_ = 0, peerId_ = 1;
  uint32_t lastPublished_ = 0;
  bool everPublished_ = false;

  struct Origins {
    IVec3 origin{};
    IVec3 chunk{};
  };
  std::map<uint32_t, Origins> ownRing_, peerRing_;

  std::vector<Want> wanted_;
  std::map<uint64_t, Inbound> inFlight_;  // World::PackChunkKey(wc) -> state
  std::deque<Outbox> outbox_;
  std::vector<PendingReplace> pending_;
  // The incumbent table for ChunkAuthority's hysteresis. Ours to keep
  // (authority.h: the memory is the CALLER's, never a file static), and it is
  // NOT sim-affecting state — nothing in it reaches a kernel.
  AuthorityMemory chunkMem_;
};

}  // namespace net
