// storesync.h — THE HOST'S STORE, ON THE WIRE (M9.5 package B).
//
// docs/PLAN_multiplayer_m9.md §M9.5-B. Package A gave `Stream` three hooks
// (`ChunkExchange::OnEvicted` / `Wanted` / `Request`) and gave `ChunkStore` a
// per-chunk tick tag. It deliberately bound none of them to a network: the
// gate there drove an in-process fake. THIS is the real implementation, and
// it is the last piece of "leave and come back to each other's edits".
//
// ---- THE PROBLEM, IN ONE PARAGRAPH ---------------------------------------
//
// `kWorldN = 512` at `kVoxelMeters = 0.10` is a 51.2 m cube, and two players
// walking independently leave each other's window in seconds. When a chunk
// leaves a machine's window it is RLE'd into that machine's own `ChunkStore`
// and forgotten by everyone else; when somebody walks back in, the refill
// asks the LOCAL store and, on a miss, regenerates from the seed. So today a
// crater the client dug is invisible to the host forever, and a crater the
// host dug is undone the moment the client walks over it — the two machines
// silently diverge in exactly the places nobody is looking. §4 finding 7 is
// the shape of the fix: authority-only puts, tick-tagged, acked, and a
// MANIFEST consulted before regenerating so the common "nobody ever touched
// this chunk" case costs zero messages.
//
// ---- THE MODEL: THE HOST'S STORE IS THE TRUTH ----------------------------
//
// Not "a" store — THE store. Both machines keep their own `ChunkStore` (they
// must: that is what a window eviction writes into, and it is what a
// single-player save is made of), but only one of them is persisted as the
// world and only one of them arbitrates. The rules, all four of them:
//
//   1. ONLY THE AUTHORITY PUTS. `net::ChunkAuthority(wc, peers, mem)` is the
//      same pure function both machines run for mobs and bodies, so the two
//      sides agree on who may speak for a chunk WITHOUT a claim message. A
//      machine that is not the authority evicts into its own store and says
//      nothing — its copy is a cache, not a claim.
//   2. THE HOST STORES; THE CLIENT ASKS IT TO. Host `OnEvicted` is a local
//      `Put` plus a one-row `ManifestDelta`. Client `OnEvicted` is a
//      `ChunkPut` that stays in an unacked list until the host says it
//      landed. A `ChunkPut` whose tick is OLDER than the host's tag is
//      refused — and still ACKED, because "mine is newer" resolves the
//      client's obligation just as completely as "yours is in".
//   3. THE MANIFEST IS CONSULTED BEFORE PROCGEN, NEVER AFTER. `Wanted` is
//      asked by `Stream` at the moment its own store missed and before it
//      dispatches worldgen, and it must answer from data already in hand
//      (sim/stream.h says so in as many words: an implementation that needs
//      a round trip to answer is a frame-long stall). The client therefore
//      keeps a MIRROR of the host's manifest — full on join, one row per
//      delta after — and the host answers from arithmetic, not from a table:
//      "is the client the authority for this, and does it have it resident?"
//   4. A HELD SLOT ALWAYS GETS AN ANSWER. `ChunkMiss` exists because
//      `Stream` holds a wanted slot INERT indefinitely. Silence is not a
//      valid reply; neither is a disconnect, which is why `Disconnect()`
//      misses every outstanding request rather than dropping it.
//
// ---- WHAT IS NOT HERE ----------------------------------------------------
//
// NO SOCKET OWNERSHIP AND NO WORLD. This object is handed a `net::Link*` and
// calls `Send` on it; `main.cpp`'s `netPump` does the polling and the
// dispatch, exactly as it does for `ChunkSync` and `EntitySync`. It never
// touches `World`, never allocates a GPU resource and never reads a voxel: a
// chunk is an RLE blob from the moment `Stream` hands it over to the moment
// `Stream` takes it back. That is what lets `--gate store-sync` drive the
// whole protocol over `MakeLoopback()` with two `ChunkStore`s, no GPU, no
// assets and no fixture.
//
// NO RELAY, AND THAT IS NOT AN OMISSION. The plan's §M9.5-B step 2 says the
// host "forwards to the machine that has it resident and relays". With TWO
// peers that machine is always the requester itself, so the forward would be
// a message a peer sends to ask itself a question it already knows the answer
// to. What the two-peer case actually needs is SYMMETRY, and that is what is
// built: EITHER side answers a `ChunkGet` out of its own store, so the host
// pulling a chunk the client is the authority for uses the same four lines as
// the client pulling one from the host. A third player is a second `Link` and
// a relay table; it is not a wider version of this.
//
// NO THREADS AND NO CLOCK. Everything happens inside `Pump` or inside a
// message handler, both called from the frame loop, so nothing here can
// introduce a scheduling-dependent outcome (rule 1).

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "math3d.h"
#include "net/authority.h"  // PeerView, ChunkAuthority, AuthorityMemory
#include "net/link.h"
#include "net/protocol.h"
#include "sim/chunkstore.h"
#include "sim/stream.h"  // ChunkExchange

namespace net {

// Bumped when any message below changes layout. Rides inside each payload
// rather than in the frame header, for the reason protocol.h gives: the frame
// header is shared by four packages now and a bump there is everybody's bump.
constexpr uint32_t kStoreSyncVersion = 1;

// ≤ 64 KiB per `ChunkManifest` slice (plan step 1). A manifest row is 16
// bytes, so this is 4,096 rows a slice and a full 32,768-chunk store is eight
// frames. Sliced for §4 finding 8's reason: one enormous message ahead of the
// first `TickBatch` is head-of-line blocking on the one message the peer's
// pacer is waiting for.
constexpr uint32_t kManifestRowsPerSlice = 4096;

// How many `ChunkGet`s may be outstanding at once. A window shift refills a
// whole 1,024-chunk plane in one `Update`, and if every one of those were
// wanted the unthrottled version would put 1,024 requests (16 KiB) on the
// wire in one frame ahead of that frame's `TickBatch`. The held slots are
// inert and patient (sim/stream.h: a hold waits indefinitely and costs no
// page), so draining the queue over several frames costs nothing but a few
// frames of sky in chunks six or more away from the player.
constexpr size_t kMaxInFlightGets = 64;

// ---- the object ----------------------------------------------------------

class StoreSync : public ChunkExchange {
 public:
  // Where a delivered chunk goes. In the game these are
  // `Stream::DeliverRemote` / `Stream::DeliverMiss`; in the gate they are a
  // recording stub. A std::function rather than a `Stream*` for exactly that
  // reason — the gate must be able to run the whole protocol with no GPU, and
  // a `Stream` is a device, a world and a page pool.
  using DeliverFn =
      std::function<void(IVec3 wc, uint32_t tick, const std::vector<uint32_t>& rle)>;
  using MissFn = std::function<void(IVec3 wc)>;

  // Everything the two-process smoke and the `--frames` report read. Not
  // hashed, not saved: they exist so that a chunk that never arrived is a
  // NUMBER rather than a hole in the world (CLAUDE.md rule 6).
  struct Counters {
    uint64_t putsSent = 0;        // client: ChunkPut messages emitted
    // DISTINCT CHUNKS behind `putsSent`, and the reason it exists is that
    // the smoke's acceptance compares the client's puts against the number
    // of TICK-TAGGED chunks in the host's saved store — and those are two
    // different units. A player who walks out of a chunk, back in and out
    // again sends two puts for one chunk, so `putsSent > tagged` is the
    // NORMAL outcome and says nothing. `putsDistinct == tagged` is the claim
    // that actually holds, and without this counter the gap between the two
    // is a mystery that costs a smoke run to attribute (CLAUDE.md rule 6).
    uint64_t putsDistinct = 0;
    uint64_t putsRecv = 0;        // host: ChunkPut messages accepted
    uint64_t putsRefusedOld = 0;  // host: ChunkPut older than my tag
    uint64_t putsAcked = 0;       // client: ChunkPutAck messages consumed
    uint64_t putsLocal = 0;       // host: OnEvicted straight into my store
    uint64_t manifestSent = 0;    // manifest ROWS sent (full + deltas)
    uint64_t manifestRecv = 0;    // manifest ROWS received
    uint64_t manifestEntries = 0; // rows in the mirror right now
    uint64_t getsSent = 0;
    uint64_t getsRecv = 0;
    uint64_t dataSent = 0;
    uint64_t dataRecv = 0;
    uint64_t missesSent = 0;
    uint64_t missesRecv = 0;
    uint64_t wanted = 0;          // Wanted() calls that answered true
    uint64_t unacked = 0;         // puts still waiting for an ack
    uint64_t reoffered = 0;       // unacked puts re-sent on a reconnect
    uint64_t abandoned = 0;       // in-flight gets missed by a disconnect
  };

  // ---- lifecycle ---------------------------------------------------------

  // `store` is MY store (the host's is the truth; the client's is a cache).
  // `link` is the peer. Neither is owned. Idempotent: calling Connect on an
  // already-connected object re-seats the link and RE-OFFERS every unacked
  // put, which is the reconnect path of plan step 3 — the client's unacked
  // list deliberately survives a disconnect, in RAM, for exactly that.
  void Connect(Link* link, bool host, ChunkStore* store, uint32_t me,
               uint32_t peer);
  void SetDelivery(DeliverFn deliver, MissFn miss);
  // Drop the peer. The unacked put list and the manifest mirror SURVIVE (a
  // rejoin in the same process re-offers them); everything that names the
  // dead connection does not. Every outstanding `ChunkGet` is answered with a
  // local MISS, because a held slot waits forever and a disconnect is not an
  // answer — that is the one thing this function must not leave undone.
  void Disconnect();
  bool Connected() const { return link_ != nullptr; }
  bool IsHost() const { return host_; }

  // The authority arithmetic's two facts, refreshed every tick by the same
  // caller that feeds `EntitySync::SetPeers` and from the same aged pair (see
  // main.cpp's `netMyRing`: both machines must compute ownership from the
  // SAME two views or "ownership is derived, not negotiated" is false).
  void SetPeers(const PeerView& mine, const PeerView* theirs);

  // ---- ChunkExchange (called by Stream, inside a tick) -------------------
  void OnEvicted(IVec3 wc, uint32_t tick,
                 const std::vector<uint32_t>& rle) override;
  bool Wanted(IVec3 wc) override;
  void Request(IVec3 wc) override;

  // ---- the wire ----------------------------------------------------------

  // Host, once per join, right after the HelloAck: the WHOLE store as
  // (wc, tick) rows, in ≤ 64 KiB slices. Returns the number of rows sent.
  size_t SendFullManifest();

  // True once the client has received a complete full manifest (the last
  // slice's `first + count == total`). Always true on the host, which needs
  // no mirror. The late-join `ReloadWindow` waits on this.
  bool ManifestReady() const { return manifestReady_; }

  // Dispatch. Returns false for a message type this object does not own, so
  // main's switch can fall through to its default. A malformed payload is
  // DROPPED and counted, never half-applied.
  bool OnMessage(MsgType t, const uint8_t* p, size_t n);

  // Once per frame, after the inbox is drained: issue queued `ChunkGet`s up
  // to the in-flight cap. Nothing else is periodic — TCP does not lose bytes,
  // so an unacked put is re-offered on a RECONNECT and never on a timer.
  void Pump();

  const Counters& Stats() const { return c_; }
  size_t UnackedCount() const { return unacked_.size(); }
  size_t ManifestSize() const { return manifest_.size(); }
  // The mirror's tag for `wc`, or 0. Exposed for the gate.
  uint32_t ManifestTickOf(IVec3 wc) const;

 private:
  void Send(MsgType t, const std::vector<uint8_t>& b);
  void SendPut(IVec3 wc, uint32_t tick, const std::vector<uint32_t>& rle);
  // Answer a ChunkGet out of MY store, or miss. The symmetric half: both
  // roles run this, which is why there is no relay (header comment).
  void AnswerGet(IVec3 wc);
  // May I speak for the bytes I just evicted? NOT `ChunkAuthority(wc) == me`:
  // the chunk has by definition already left my window, so that function's
  // residency requirement fails for every call. The rule it uses instead, and
  // the bug that rule exists to avoid, are written out at the definition.
  bool IAmAuthority(IVec3 wc) const;
  bool PeerIsAuthority(IVec3 wc) const;

  Link* link_ = nullptr;
  ChunkStore* store_ = nullptr;
  bool host_ = false;
  uint32_t me_ = 0, peer_ = 1;

  DeliverFn deliver_;
  MissFn miss_;

  // The two peers, in the shape `net::ChunkAuthority` wants. `havePeer_` is
  // false until the first applied `PlayerState`, and while it is false this
  // object claims nothing and wants nothing: authority over a chunk requires
  // knowing where the other machine is.
  PeerView mine_{}, theirs_{};
  bool havePeer_ = false;
  // The chunk-authority hysteresis memory. OURS, not shared with EntitySync's
  // — authority.h's whole purity argument is that the memory belongs to the
  // caller, and a chunk's incumbent for PERSISTENCE (who may put it) changes
  // on a different schedule from a mob's.
  AuthorityMemory chunkMem_;

  // CLIENT ONLY: the mirror of the host's manifest. Packed chunk key ->
  // tick tag. A `std::unordered_map` and not a sorted vector because `Wanted`
  // is on the refill path — a window shift asks it up to 1,024 times in one
  // `Update` and every one of those must be a hash lookup, not a search.
  std::unordered_map<uint64_t, uint32_t> manifest_;
  bool manifestReady_ = false;
  uint32_t manifestExpected_ = 0, manifestGot_ = 0;

  // A put that has not been acked. Keyed by chunk so a second eviction of the
  // same chunk REPLACES the first rather than queueing behind it: the newer
  // bytes are the ones the host wants, and sending both would be sending a
  // chunk we already know is stale.
  struct Unacked {
    IVec3 wc{};
    uint32_t tick = 0;
    std::vector<uint32_t> rle;
  };
  std::unordered_map<uint64_t, Unacked> unacked_;
  // Every chunk key this machine has ever sent a put for, for the
  // `putsDistinct` counter above. A set of u64 and nothing else: at the
  // smoke's 2,193 puts that is ~18 KiB, and it is diagnostics-only — nothing
  // reads it to make a decision.
  std::unordered_set<uint64_t> everPut_;

  // Wanted said yes; Request queued it; Pump issues it. `inFlight_` is what
  // has actually gone out and is owed an answer.
  std::deque<IVec3> pendingGets_;
  std::unordered_map<uint64_t, IVec3> inFlight_;

  // HOST ONLY: chunks whose tick tag has to be re-asserted after `Stream`'s
  // own untagged `Put` lands. The full argument is in `Pump`; the one-line
  // version is that `Stream` calls `OnEvicted` and then `store_.Put(wc, rle)`
  // with no tick, an untagged Put ERASES a tag (chunkstore.h), and so the tag
  // written inside `OnEvicted` is gone one line after it is written.
  struct PendingTag {
    IVec3 wc{};
    uint32_t tick = 0;
  };
  std::vector<PendingTag> pendingTags_;

  Counters c_{};
};

}  // namespace net
