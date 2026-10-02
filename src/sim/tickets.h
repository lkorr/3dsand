// tickets.h — chunk tickets: a bounded number of 5^3 chunk boxes outside the
// residency window, simulated by the same CA at the same tick rate
// (docs/PLAN_chunk_tickets.md; DESIGN.md "Chunk tickets").
//
// WHAT A TICKET IS. A box of kTicketBoxN^3 = 125 chunks whose inner 3^3 is
// ACTIVE (dispatched) and whose shell is RESIDENT ONLY (read by the interior,
// written by reach-1 moves out of it, never dispatched — sim_compact.wgsl).
// Ticket i owns the fixed slot range [kNumChunks + i*125, +125); a box chunk
// sits at the slot its coordinate takes modulo 5 (world.h TicketLocalIndex).
// The live boxes are World's ticket table, uploaded into pageTable's tail, and
// every sim kernel resolves a ticket cell through it (common.wgsl ticketSlotOf).
//
// THE LIFECYCLE IS AN OP STREAM (rule 3). Every residency decision —
// activate, release, refuse, re-centre — is a TicketOp, taken at the
// between-ticks point (Stream::Update -> Tick), recorded into the op record
// (sim/oprecord.h NoteTicketOp) and a pure function of:
//   - the requests queued since the last Tick, applied in (tick, chunk) order;
//   - World::Snap(), the FIXED-latency snapshot (kSnapshotLatency behind), for
//     idleness, particle-landing requests and the release's store decision.
// Nothing reads a timing-dependent readback, so a release lands on the same
// tick in every run and twice-run determinism holds with tickets live.
//
// POLICY (§2.4): cap kTicketMax, refused past it (never queued unboundedly);
// a request inside the window is a no-op; one inside a live box is absorbed;
// released when the 27 active chunks have been clean for kTicketIdleTicks
// published snapshots, or after kTicketMaxTicks regardless, or when a window
// shift is about to cover the box. A box never overlaps the window or another
// live box: a request's box is placed at the first of up to 125 offsets that
// keeps its chunk inside and collides with neither.
//
// THE RELEASE IS TWO-PHASE, and exact. At tick T the box leaves the table (no
// kernel can write it from T on) and all 125 slots are COPIED out; which of
// them the store keeps is decided only once the snapshot of T-1 is published
// (T + kSnapshotLatency - 1): a chunk is kept iff some published snapshot
// since activation showed it dirty. Deciding at T would miss a particle that
// landed in the last kSnapshotLatency ticks. Until then the index is
// RELEASING (not reusable); a fill that needs one of its chunks first forces
// the batch, which then keeps all 125 (conservative, never lossy).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

class Stream;

// One residency decision, as the op record carries it. 32 bytes, POD.
struct TicketOp {
  enum Kind : uint32_t {
    kActivate = 1,  // wc = the box lo; ticket = its index
    kRelease = 2,   // wc = the box lo
    kRefuse = 3,    // wc = the requested chunk; ticket = kTicketMax
    kRecentre = 4,  // wc = the NEW box lo
  };
  uint32_t kind = 0;
  uint32_t reason = 0;   // TicketReason
  uint32_t ticket = 0;
  uint32_t tick = 0;
  int32_t wc[3] = {0, 0, 0};
  uint32_t pad = 0;
};
static_assert(sizeof(TicketOp) == 32, "TicketOp is a record field");

enum class TicketReason : uint32_t {
  Manual = 0,         // --ticket, a dev control
  Particle = 1,       // a particle landed outside residency (P2)
  Idle = 2,           // release: the active chunks slept kTicketIdleTicks
  Timeout = 3,        // release: kTicketMaxTicks reached
  WindowOverlap = 4,  // release: a shift is about to cover the box
  Recentre = 5,       // P4: activity reached the shell
  Gate = 6,           // a selftest fixture
  Cap = 7,            // refuse: all kTicketMax indices taken
  Placement = 8,      // refuse: no box placement avoids the window / live boxes
};
const char* TicketReasonName(uint32_t r);

// Run counters, for the F1 panel and build/last_run.json. Not hashed, not
// saved: a refused ticket is a NUMBER, never a silently missing pile.
struct TicketStats {
  uint32_t live = 0;
  uint32_t releasing = 0;
  uint32_t highWater = 0;
  uint64_t requests = 0;
  uint64_t activated = 0;
  uint64_t absorbed = 0;       // request inside a live box
  uint64_t inWindow = 0;       // request inside the window: a no-op
  uint64_t refusedCap = 0;
  uint64_t refusedPlacement = 0;
  uint64_t released = 0;
  uint64_t releasedIdle = 0, releasedTimeout = 0, releasedOverlap = 0;
  uint64_t recentred = 0;
  uint64_t recentreRefused = 0;  // a shell hit whose step was blocked (window, live box, active plane)
  uint64_t chunksKept = 0;     // release batches: chunks the store took
  uint64_t chunksSkipped = 0;  // ...and chunks never dirtied (procgen / store reproduce them)
  uint64_t conservative = 0;   // releases forced to keep all 125
  uint64_t activeTickSum = 0;  // sum over ticks of active chunks dispatched-eligible (27/ticket)
  // Far landings (P2): particles that came to rest outside residency and were
  // refused or not served in time, parked on the CPU until their chunk is
  // resident again, then re-thrown as a still particle.
  uint64_t landingsDeposited = 0;
  uint64_t landingsRespawned = 0;
  uint64_t landingsDropped = 0;   // the CPU park was full (kFarLandingMax)
  uint32_t landingsParked = 0;    // currently held
};
// The last-written stats of the process's Tickets (one per Stream; the game
// and the selftest each have one), for writers with no Stream in hand
// (selftest.cpp WriteJson).
const TicketStats& LastTicketStats();

class Tickets {
 public:
  void Init(World* world, Stream* stream);

  // Queue a request for a ticket whose box contains `chunk`. Applied at the
  // next Tick in (tick, chunk) order, subject to the policy above.
  void Request(IVec3 chunk, TicketReason reason, uint32_t tick);

  // THE BETWEEN-TICKS STEP. Called once per tick by Stream::Update (or by a
  // harness that drives SubmitTick without it, through Stream::TicketTick),
  // BEFORE the tick's submit. In order: fold the published snapshot, finalize
  // releases whose decision snapshot has arrived, release idle / timed-out
  // tickets, take particle requests off the snapshot, apply the queue,
  // re-centre, upload the table if anything changed.
  void Tick(uint32_t tick);

  // Release every live ticket whose box intersects the window at `newOrigin`
  // (the window-overlap rule, §2.4). Stream::Update calls it before ShiftAxis.
  void ReleaseOverlapping(IVec3 newOrigin, uint32_t tick);

  // The world is being replaced (worldgen, load, regen): every ticket is
  // forgotten without being stored, its slots cleared to air, the table
  // emptied. Safe to call with no ticket live.
  void DropAll(const rhi::Queue& queue);

  // Every chunk a FarLanding is waiting on that is now resident gets its
  // particles queued for re-throw (P2). Stream calls this at each fill.
  void OnChunkResident(IVec3 wc);
  // Pay out queued far-landing particles into the tick's spawn stream
  // (bounded by `max`). Returns how many.
  uint32_t DrainLandingSpawns(std::vector<ParticleSpawn>& out, uint32_t max);
  bool HasLandingSpawns() const { return !spawnQueue_.empty(); }

  uint32_t LiveCount() const;
  const TicketStats& Stats() const { return stats_; }
  // ATTRIBUTION (CLAUDE.md rule 6): what the last re-centre / refused
  // re-centre saw — the face, how many shell chunks were hit, and the first
  // hit (chunk, occupancy before -> after, snapshot tick). Not hashed.
  const std::string& LastRecentreNote() const { return recentreNote_; }
  // Human-readable one-liners for the dev panel (one per live ticket).
  std::vector<std::string> Describe(uint32_t tick) const;
  // A ticket's state, for gates.
  enum class State : uint8_t { Free, Live, Releasing };
  State StateOf(uint32_t i) const { return t_[i].state; }
  IVec3 BoxLo(uint32_t i) const { return t_[i].lo; }
  uint32_t ActivatedTick(uint32_t i) const { return t_[i].activated; }
  uint32_t ReleasedTick(uint32_t i) const { return t_[i].releaseTick; }
  // The live ticket whose box holds `chunk`, or kTicketMax.
  uint32_t TicketHolding(IVec3 chunk) const;
  // The chunks the far landings held right now are waiting on (gates).
  std::vector<IVec3> LandingChunks() const {
    std::vector<IVec3> v;
    for (const Landing& l : landings_) v.push_back(l.chunk);
    return v;
  }

 private:
  struct Req {
    IVec3 chunk;
    uint32_t reason;
    uint32_t tick;
  };
  static constexpr uint32_t kOccUnset = 0xFFFFFFFFu;
  struct Ticket {
    State state = State::Free;
    IVec3 lo{0, 0, 0};
    uint32_t activated = 0;     // tick it became live
    uint32_t releaseTick = 0;   // tick it left the table (Releasing)
    uint32_t idleSnaps = 0;     // consecutive clean snapshots of the 27
    uint32_t lastRecentre = 0;
    uint32_t reason = 0;
    uint64_t evictHandle = 0;   // Stream's deferred-keep eviction batch
    bool conservative = false;  // a snapshot was missed: keep all 125
    // Per LOCAL index (World::TicketLocalIndex order): dirty in some
    // published snapshot since activation. The release keeps exactly these.
    std::vector<uint8_t> everDirty;
    // Per LOCAL index: dirty in the most recent snapshot.
    std::vector<uint8_t> lastDirty;
    // Per LOCAL index, SHELL chunks only: matter crossed into or out of it
    // since the last activation or re-centre (P4's shell test). Measured as a
    // change of the chunk's non-air count (snapshot occupancy) against
    // shellOcc, NOT as a dirty flag: an interior write on the face cell marks
    // the shell dirty through the fan-out without moving anything into it,
    // and a shell is dirty only the one tick an arrival lands, so neither
    // "dirty" nor "dirty in the latest snapshot" says matter reached it.
    // Cleared when Recentre acts on it or gives up.
    std::vector<uint8_t> shellHit;
    // Per LOCAL index: the shell chunk's occupancy at the last fold that saw
    // it (kOccUnset until the first snapshot after its fill).
    std::vector<uint32_t> shellOcc;
    std::string firstHit;  // attribution: the first shell hit since the last re-centre
    // Per LOCAL index: the first tick whose snapshot describes THIS chunk in
    // the slot (activation, or the re-centre that refilled it). An older
    // snapshot is the previous occupant's and is not folded.
    std::vector<uint32_t> since;
  };
  // A copied-out batch whose store decision waits for the snapshot of
  // `until - 1` (see THE RELEASE IS TWO-PHASE above). `bits` are per entry of
  // `slots`; `ticket` is the index to free on finalize (kTicketMax for a
  // re-centre's plane, which frees nothing).
  struct PendingKeep {
    uint64_t handle = 0;
    std::vector<uint32_t> slots;
    std::vector<uint8_t> bits;
    uint32_t until = 0;
    uint32_t ticket = kTicketMax;
    bool conservative = false;
  };
  std::vector<PendingKeep> keeps_;

  void Fold(uint32_t tick);
  void FinalizeReleases(uint32_t tick);
  void ReleaseIdle(uint32_t tick);
  void ApplyRequests(uint32_t tick);
  void Recentre(uint32_t tick);
  bool PlaceBox(IVec3 chunk, IVec3& lo) const;
  bool BoxHitsWindow(IVec3 lo, IVec3 origin) const;
  bool BoxHitsLive(IVec3 lo, uint32_t except) const;
  void Activate(uint32_t i, IVec3 lo, uint32_t reason, uint32_t tick);
  void Release(uint32_t i, uint32_t reason, uint32_t tick);
  std::vector<uint32_t> SlotsOf(uint32_t i) const;
  void Record(uint32_t kind, uint32_t reason, uint32_t ticket, uint32_t tick, IVec3 wc);
  void Publish();

  World* world_ = nullptr;
  Stream* stream_ = nullptr;
  Ticket t_[kTicketMax];
  std::vector<Req> pending_;
  bool tableDirty_ = false;
  bool haveFold_ = false;
  uint32_t lastFoldTick_ = 0;
  TicketStats stats_;
  std::string recentreNote_;

  // ---- far landings (P2) -------------------------------------------------
  // Keyed by world chunk; each entry is the particle as it was parked, to be
  // re-thrown with zero velocity when its chunk is resident again. Bounded:
  // past kFarLandingMax the oldest are dropped and counted.
  static constexpr size_t kFarLandingMax = 1u << 16;
  struct Landing {
    IVec3 chunk;
    ParticleSpawn p;
  };
  std::vector<Landing> landings_;   // in arrival (= snapshot tick) order
  std::vector<ParticleSpawn> spawnQueue_;
  void TakeDeposits();
  void QueueResidentLandings();
};
