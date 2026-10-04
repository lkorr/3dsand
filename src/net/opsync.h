#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "game/session.h"   // OpBatch: the five op vectors one tick produces
#include "net/protocol.h"   // kOpDelayTicks
#include "sim/oprecord.h"   // Producer, the author scopes
#include "sim/world.h"      // the op structs, IVec3, World::ChunkInWindow

// M9.3 package B: THE OP EXCHANGE (docs/PLAN_multiplayer_m9.md §M9.3-B).
//
// WHAT THIS FILE IS FOR, in one sentence: two machines that each author their
// own world edits must SUBMIT THE SAME OP VECTORS UNDER THE SAME TICK LABEL,
// because the op stream's order is the tick's identity (CLAUDE.md rule 3: the
// lowest op index owns the cell) and two orders are two worlds.
//
// THE THREE MOVING PARTS.
//
//  1. DELAY. A machine's ops for what it did at tick T are not submitted at T.
//     They are labelled T + kOpLabelAhead (see that constant: it is D + 1, and
//     why is worth reading), travel to the peer inside that tick's TickBatch,
//     and BOTH machines submit them under that label. D is the jitter budget:
//     the wire has D ticks (~133 ms at kTickDt) to deliver a batch before the
//     lockstep pacer has to stall. This is why the local batch also goes
//     through the queue rather than straight into SubmitTick — a local edit
//     that landed at T while the peer's landed later would put the two worlds
//     in different states with no way back.
//
//  2. CANONICAL ORDER. For a given label the merged batch is
//     `batches sorted by author playerId ascending, push order within each`.
//     The host is player 0, so the host's ops always come first, on BOTH
//     machines. The rule is total and symmetric, and it is the whole reason
//     the two merges are byte-identical: neither side has a notion of "mine
//     first".
//
//  3. RESIDENCY. A CellOp carries a SLOT index (sim_mutate.wgsl), and a slot
//     is `wc & 31` per axis — GLOBAL, so the index itself needs no
//     translation. What it does NOT carry is which of the aliased world
//     chunks it meant, so a remote CellOp for a chunk this machine does not
//     hold would paint a chunk 32 chunks away. The sender's window origin
//     therefore rides the wire beside its ops, the receiver recovers `wc` with
//     it, and an op for a non-resident chunk is DROPPED AND COUNTED. Brush,
//     Explosion, ParticleSpawn and FluidSpawn ops are absolute world coords
//     and need none of this; cells outside the receiver's window are refused
//     by the kernel anyway.
//
// WHAT IS NOT EXCHANGED, stated so it is a known gap and not a surprise:
//   * GAS SPAWNS. OpBatch has five vectors, not six (game/session.h): gas is
//     queued inside World by its producers and joins at SubmitTick, so the
//     frame layer never holds it and there is nothing here to encode. A
//     peer's gas spawns do not cross in M9.3.
//   * THE CPU-SIDE CONSEQUENCES of a remote explosion (island rescan, limb
//     damage, body impulses). The crater crosses, because it is an op; the
//     debris scan and the ragdoll launch are raised by the AUTHORING machine
//     only. Moving them to the chunk authority is M9.4-D, which is why that
//     package's section says "D moves AddDestructionEvent into phase N".
//
// NOTHING HERE IS HASHED STATE. The queue is CPU-side bookkeeping over the op
// vectors; what reaches the GPU is still exactly the vectors SubmitTick gets.
// When nothing is connected, D is 0, nothing is stored, and phase N submits
// the batch the tick just built — which is why the single-player `--record-ops`
// stream is byte-identical to the pre-package oracle.
namespace net {

// Bumped when the ops blob's layout changes. It rides inside the blob rather
// than in TickBatchHeader because the blob is opaque to protocol.h by design
// (three packages' schemas in one file would make every one of them a protocol
// version bump).
constexpr uint32_t kOpsWireVersion = 1;

// HOW FAR AHEAD A TICK'S OPS ARE LABELLED, and it is D + 1, NOT D.
//
// This caught the first two-process smoke and cost it: phase N stored the
// local batch under `tick + kOpDelayTicks`, `netSendBatch` asked the queue for
// the label the PACER was sending, the two numbers were one apart, and every
// batch on the wire carried an empty ops blob. The run looked healthy --
// 282 batches each way, 276 paced ticks, zero stalls to speak of -- and
// exchanged no ops at all, which is exactly the kind of green-for-the-wrong-
// reason the plan's "ops sent/recv > 0" acceptance exists to catch.
//
// The pacer's numbering is the truth and it is stated in protocol.h: Reset()
// puts `sent` behind by D+1 so the SAME `while (ShouldSend())` loop emits
// T0..T0+D before the first tick runs. After running tick T the next label is
// therefore T + D + 1, and the batch labelled L is consumed by the peer when
// it runs tick L. So ops authored at T are applied at T + D + 1 on both
// machines -- one more tick of delay than the constant's name suggests, and
// the only thing that matters is that BOTH sides derive the label the same
// way. They do: this constant is the only place it is computed.
constexpr uint32_t kOpLabelAhead = kOpDelayTicks + 1;

// slot index -> world chunk UNDER A GIVEN ORIGIN.
//
// The free-function twin of World::SlotToWorldChunk, which reads the LOCAL
// window origin off the World it is a member of. Decoding a peer's CellOp
// needs the SENDER's origin, and there is no World here that has it. The
// arithmetic is copied deliberately and must stay identical to world.h's; the
// `ops-exchange` gate asserts the two agree for the local origin, so a change
// to one without the other fails a gate instead of silently painting a chunk
// 32 chunks away.
//
// Ticket slots (slotIdx >= kNumChunks) have no window arithmetic at all and
// are not exchanged: kTicketSlots is 0 at P0, and World::IsWindowSlot is the
// test every caller here makes first.
inline IVec3 SlotChunkUnderOrigin(uint32_t slotIdx, IVec3 origin) {
  IVec3 s{(int)(slotIdx % kNChunk), (int)((slotIdx / kNChunk) % kNChunk),
          (int)(slotIdx / (kNChunk * kNChunk))};
  const int m = (int)kNChunk - 1;
  return {origin.x + ((s.x - origin.x) & m), origin.y + ((s.y - origin.y) & m),
          origin.z + ((s.z - origin.z) & m)};
}

// ---- the blob inside TickBatchWire.ops ----------------------------------
//
// Five PodVecs, the producing machine's window origin, and the label. The
// origin is the one the ops were PRODUCED under (tick T), not the sender's
// origin at send time (tick T, but the header's copy is re-read every send and
// a window shift between production and send would make the two differ) —
// CellOp slot indices were computed under the production origin and are only
// interpretable under it.
struct OpsWire {
  static void Encode(const OpBatch& b, IVec3 origin, uint32_t label,
                     std::vector<uint8_t>& out);
  // False on a short, truncated or wrong-version blob, and `b` is then left
  // empty rather than half-filled: half a peer's tick is worse than none of
  // it, because the pacer would run the tick believing it had everything.
  static bool Decode(const uint8_t* p, size_t n, OpBatch& b, IVec3& origin,
                     uint32_t& label);
};

// What one Merge did. Every field is a NUMBER A REPORT CAN NAME (CLAUDE.md
// rule 6): "12 cells dropped" with no chunk attached is the failure mode that
// rule is about, so the first dropped cell's chunk is kept too.
struct MergeStats {
  bool haveLocal = false;
  bool haveRemote = false;
  uint32_t authors = 0;          // batches merged, local + remote
  uint32_t remoteBrush = 0;
  uint32_t remoteExps = 0;
  uint32_t remoteCells = 0;      // kept
  uint32_t remoteCellsDropped = 0;  // chunk not resident here
  uint32_t remoteSpawns = 0;
  uint32_t remoteFluid = 0;
  uint32_t merged = 0;           // ops of all five kinds in the merged batch
  int32_t firstDropChunk[3] = {0, 0, 0};
  bool haveFirstDrop = false;
};

// ---- the queue ----------------------------------------------------------
//
// PURE. No sockets, no clock, no World except as a const residency oracle in
// Merge. That is what lets the `ops-exchange` gate drive two of these against
// each other through a loopback pair and compare the merged vectors byte for
// byte, and it is why the merge rule can be a proof rather than an
// observation.
class OpDelayQueue {
 public:
  // One label's worth of what a single machine authored.
  struct Entry {
    OpBatch batch;
    IVec3 origin{0, 0, 0};
    uint32_t author = 0;
    // The author side table for `batch.ops` / `batch.exps`, resolved on the
    // PRODUCING tick. Carried because ranges are recorded against the tick
    // that produced them and consumed by the tick that SUBMITS them, and
    // under delay those are D ticks apart — see NoteLocalMeta below.
    std::vector<sandvox::opstream::OpMeta> opMeta, expMeta;
  };

  void SetDelay(uint32_t d) { d_ = d; }
  uint32_t D() const { return d_; }
  void SetLocalId(uint32_t id) { localId_ = id; }
  uint32_t LocalId() const { return localId_; }

  // Forget everything. Called at connect and at disconnect: a label from the
  // previous session means nothing in the new one (the host's clock decides
  // the numbering and a rejoin restarts it).
  void Reset() {
    labels_.clear();
    droppedCells = mergedMax = remoteBatches = localBatches = 0;
    missingRemote = missingLocal = lateRemote = 0;
  }

  // Store what tick `producedAt` authored, under label `producedAt + D`.
  // `origin` is the window origin it was produced under.
  void PushLocal(uint32_t producedAt, const OpBatch& b, IVec3 origin);
  // The author metadata for the batch just pushed, in the same call order.
  // Split from PushLocal so the caller can resolve it from oprecord's ranges
  // without this header needing to know how that is done.
  void NoteLocalMeta(uint32_t label,
                     std::vector<sandvox::opstream::OpMeta> opMeta,
                     std::vector<sandvox::opstream::OpMeta> expMeta);
  // One more brush op into an already-pushed local label. For the two-process
  // smoke's paint switch (SANDVOX_NET_SMOKE_PAINT), which needs an authoring
  // host without a human on the mouse. False when the label is not held.
  bool AppendLocalBrush(uint32_t label, const BrushOp& op,
                        sandvox::opstream::Producer p);

  // A peer's batch for `label`, decoded, with the origin it was produced
  // under. A second batch for the same (label, author) replaces the first:
  // TCP cannot duplicate, so this only happens on a resend.
  void NoteRemote(uint32_t label, uint32_t author, IVec3 remoteOrigin,
                  OpBatch b);

  bool HaveLocal(uint32_t label) const;
  bool HaveRemote(uint32_t label) const;
  // Local present AND remote present. The pacer already guarantees the peer's
  // batch for T arrived before T runs, so a false here is an ASSERTION
  // FAILURE, not a wait — it is counted (missingRemote) and the merge proceeds
  // with what there is, because stalling the world on a bookkeeping bug turns
  // a divergence into a hang.
  bool Ready(uint32_t label) const;

  // The local batch to SEND for `label`, and the origin it was produced
  // under. Never null: an empty batch is returned for a label nothing was
  // pushed for, because a batch goes out every tick whether or not it carries
  // anything (protocol.h invariant 2).
  const OpBatch& Outgoing(uint32_t label, IVec3* origin) const;

  // THE MERGE. Author-major, push-order-minor; remote CellOps filtered by
  // residency; remote ops wrapped in a Producer::Remote author scope and the
  // local ops' resolved metadata re-noted at their merged indices.
  //
  // `remoteBrushIdx` (optional) receives the merged indices of the REMOTE
  // brush ops, so the caller can MarkModifiedBox over exactly those — the
  // local ones were marked at their own tick and the remote ones touch chunks
  // this machine never marked.
  //
  // `remoteExpIdx` (optional) is the same for EXPLOSIONS: the merged indices
  // of the peer's blasts. Phase N applies exactly those to this machine's own
  // avatars (session.h RemoteExplosionsHitOwnAvatars) — the owner-side half of
  // a grenade that the author's machine cannot apply (DESIGN.md s10).
  //
  // Erases every label at or before this one: labels are consumed in tick
  // order and nothing ever asks for a past one twice.
  //
  // `remoteCellIdx` (optional) is the same for the peer's KEPT CellOps: phase
  // N reads it to announce a peer's lightning bolt to this machine's frame
  // (flash + thunder; session.cpp AnnounceRemoteStrikes). Presentation only.
  OpBatch Merge(uint32_t label, const World& world, MergeStats& st,
                std::vector<uint32_t>* remoteBrushIdx = nullptr,
                std::vector<uint32_t>* remoteExpIdx = nullptr,
                std::vector<uint32_t>* remoteCellIdx = nullptr);

  size_t LabelsHeld() const { return labels_.size(); }

  // Run counters, for the --frames report and the gate's detail line.
  uint64_t droppedCells = 0;   // remote cells for chunks not resident here
  uint64_t mergedMax = 0;      // largest merged op count over the run
  uint64_t remoteBatches = 0;
  uint64_t localBatches = 0;
  uint64_t missingRemote = 0;  // merged a label with no peer batch
  uint64_t missingLocal = 0;   // merged a label we pushed nothing for
  uint64_t lateRemote = 0;     // a peer batch for a label already merged

 private:
  struct Label {
    bool haveLocal = false;
    Entry local;
    // At most one peer in M9 (net/link.h: one peer per link), but a vector
    // because the merge rule is over N authors and a vector of one costs
    // nothing. Kept SORTED by author, so Merge walks it in the canonical
    // order without re-sorting.
    std::vector<Entry> remote;
  };
  // std::map, not unordered: it holds at most D+1 labels and Merge erases
  // "everything at or before T", which is one call on an ordered container.
  std::map<uint32_t, Label> labels_;
  uint32_t d_ = kOpLabelAhead;
  uint32_t localId_ = 0;
  uint32_t mergedUpTo_ = 0;
  bool everMerged_ = false;
};

// ---- what the tick sees -------------------------------------------------
//
// The queue plus the ONE BIT phase N reads. Separate from OpDelayQueue so the
// gate can drive the pure queue with no notion of "connected", and so the
// tick's test is a single bool rather than a null check plus a state query.
//
// Owned by main() (the frame loop is what polls the socket), borrowed by
// TickAuthorityCtx, and NULL IN EVERY HARNESS — which is the acceptance
// criterion: null or disconnected means phase N submits exactly the vectors
// the tick built, in exactly the order it built them.
class OpSync {
 public:
  bool Connected() const { return connected_; }
  // D when connected, 0 otherwise. Consumers that label a tick (the explosion
  // phase's AddDestructionEvent) read this one function rather than testing
  // `connected` themselves, so single-player is `tick + 0` by construction.
  uint32_t Delay() const { return connected_ ? q_.D() : 0u; }
  uint32_t LocalId() const { return q_.LocalId(); }
  uint32_t PeerId() const { return peerId_; }

  void Connect(uint32_t localId, uint32_t peerId) {
    q_.Reset();
    q_.SetLocalId(localId);
    peerId_ = peerId;
    connected_ = true;
  }
  void Disconnect() {
    connected_ = false;
    q_.Reset();
  }

  OpDelayQueue& Q() { return q_; }
  const OpDelayQueue& Q() const { return q_; }

 private:
  OpDelayQueue q_;
  uint32_t peerId_ = 1;
  bool connected_ = false;
};

}  // namespace net
