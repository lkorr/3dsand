// tickets.cpp — the chunk-ticket lifecycle. See tickets.h for the contract and
// docs/PLAN_chunk_tickets.md for the design it implements.

#include "sim/tickets.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gpu/context.h"
#include "sim/oprecord.h"
#include "sim/stream.h"

namespace {
TicketStats g_lastStats;
}  // namespace

const TicketStats& LastTicketStats() { return g_lastStats; }

const char* TicketReasonName(uint32_t r) {
  switch ((TicketReason)r) {
    case TicketReason::Manual: return "manual";
    case TicketReason::Particle: return "particle";
    case TicketReason::Idle: return "idle";
    case TicketReason::Timeout: return "timeout";
    case TicketReason::WindowOverlap: return "window-overlap";
    case TicketReason::Recentre: return "recentre";
    case TicketReason::Gate: return "gate";
    case TicketReason::Cap: return "cap";
    case TicketReason::Placement: return "placement";
  }
  return "?";
}

void Tickets::Init(World* world, Stream* stream) {
  world_ = world;
  stream_ = stream;
  for (Ticket& t : t_) {
    t = Ticket{};
    t.everDirty.assign(kTicketChunks, 0);
    t.lastDirty.assign(kTicketChunks, 0);
    t.shellHit.assign(kTicketChunks, 0);
    t.shellOcc.assign(kTicketChunks, kOccUnset);
    t.since.assign(kTicketChunks, 0);
  }
  Publish();
}

void Tickets::Request(IVec3 chunk, TicketReason reason, uint32_t tick) {
  pending_.push_back({chunk, (uint32_t)reason, tick});
}

uint32_t Tickets::LiveCount() const {
  uint32_t n = 0;
  for (const Ticket& t : t_) n += t.state == State::Live ? 1u : 0u;
  return n;
}

uint32_t Tickets::TicketHolding(IVec3 c) const {
  const int n = (int)kTicketBoxN;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    const Ticket& t = t_[i];
    if (t.state != State::Live) continue;
    if (c.x >= t.lo.x && c.y >= t.lo.y && c.z >= t.lo.z && c.x < t.lo.x + n &&
        c.y < t.lo.y + n && c.z < t.lo.z + n)
      return i;
  }
  return kTicketMax;
}

std::vector<uint32_t> Tickets::SlotsOf(uint32_t i) const {
  // Slot order IS local-index order: index k of the returned list is
  // TicketLocalIndex == k, which is what the keep bits are indexed by.
  std::vector<uint32_t> s(kTicketChunks);
  for (uint32_t l = 0; l < kTicketChunks; l++) s[l] = World::TicketSlotBase(i) + l;
  return s;
}

void Tickets::Record(uint32_t kind, uint32_t reason, uint32_t ticket, uint32_t tick,
                     IVec3 wc) {
  TicketOp op;
  op.kind = kind;
  op.reason = reason;
  op.ticket = ticket;
  op.tick = tick;
  op.wc[0] = wc.x;
  op.wc[1] = wc.y;
  op.wc[2] = wc.z;
  sandvox::opstream::NoteTicketOp(tick, op);
}

bool Tickets::BoxHitsWindow(IVec3 lo, IVec3 o) const {
  const int n = (int)kTicketBoxN, w = (int)kNChunk;
  return lo.x < o.x + w && lo.x + n > o.x && lo.y < o.y + w && lo.y + n > o.y &&
         lo.z < o.z + w && lo.z + n > o.z;
}

bool Tickets::BoxHitsLive(IVec3 lo, uint32_t except) const {
  const int n = (int)kTicketBoxN;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    if (i == except || t_[i].state != State::Live) continue;
    const IVec3 b = t_[i].lo;
    if (lo.x < b.x + n && lo.x + n > b.x && lo.y < b.y + n && lo.y + n > b.y &&
        lo.z < b.z + n && lo.z + n > b.z)
      return true;
  }
  return false;
}

// THE PLACEMENT (§2.4's dedupe, made total). The box is centred on the
// requested chunk when it can be; otherwise it slides, one of 125 offsets
// that still contain the chunk, to clear the window and every live box.
// Offsets that keep the chunk in the ACTIVE interior (|d| <= 1 on every axis)
// are tried first, nearest the centre first, then lexicographic — a fixed
// order, so the placement is a pure function of (chunk, window, table).
bool Tickets::PlaceBox(IVec3 chunk, IVec3& lo) const {
  struct Cand {
    int shell, l1, dz, dy, dx;
  };
  static std::vector<Cand> order;
  if (order.empty()) {
    for (int dz = -2; dz <= 2; dz++)
      for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
          const int shell = (std::abs(dx) > 1 || std::abs(dy) > 1 || std::abs(dz) > 1) ? 1 : 0;
          order.push_back({shell, std::abs(dx) + std::abs(dy) + std::abs(dz), dz, dy, dx});
        }
    std::stable_sort(order.begin(), order.end(), [](const Cand& a, const Cand& b) {
      if (a.shell != b.shell) return a.shell < b.shell;
      if (a.l1 != b.l1) return a.l1 < b.l1;
      if (a.dz != b.dz) return a.dz < b.dz;
      if (a.dy != b.dy) return a.dy < b.dy;
      return a.dx < b.dx;
    });
  }
  const IVec3 o = world_->WindowOrigin();
  const int h = (int)kTicketBoxN / 2;
  for (const Cand& c : order) {
    const IVec3 cand{chunk.x - h + c.dx, chunk.y - h + c.dy, chunk.z - h + c.dz};
    if (BoxHitsWindow(cand, o)) continue;
    if (BoxHitsLive(cand, kTicketMax)) continue;
    lo = cand;
    return true;
  }
  return false;
}

void Tickets::Activate(uint32_t i, IVec3 lo, uint32_t reason, uint32_t tick) {
  Ticket& t = t_[i];
  t.state = State::Live;
  t.lo = lo;
  t.activated = tick;
  t.releaseTick = 0;
  t.idleSnaps = 0;
  t.lastRecentre = tick;
  t.reason = reason;
  t.evictHandle = 0;
  t.conservative = false;
  std::fill(t.everDirty.begin(), t.everDirty.end(), (uint8_t)0);
  std::fill(t.lastDirty.begin(), t.lastDirty.end(), (uint8_t)0);
  std::fill(t.shellHit.begin(), t.shellHit.end(), (uint8_t)0);
  std::fill(t.shellOcc.begin(), t.shellOcc.end(), kOccUnset);
  t.firstHit.clear();
  std::fill(t.since.begin(), t.since.end(), tick);
  world_->SetTicketBox(i, lo, true);
  // NOW, not at Publish: the fill below dispatches genChunk, which resolves
  // its slots' world chunks through the GPU table (common.wgsl
  // ticketSlotWorldChunk). Deferred writes drain ahead of that submit.
  world_->UploadTicketTable(stream_->Ctx()->queue);
  tableDirty_ = false;
  stream_->FillTicketSlots(SlotsOf(i));
  stats_.activated++;
  Record(TicketOp::kActivate, reason, i, tick, lo);
}

void Tickets::Release(uint32_t i, uint32_t reason, uint32_t tick) {
  Ticket& t = t_[i];
  if (t.state != State::Live) return;
  const std::vector<uint32_t> slots = SlotsOf(i);
  // Copy out FIRST (an eager submit), then take the box out of the table and
  // clear the slots (deferred writes, ordered after the copy). The store
  // decision is made once the snapshot of tick-1 is published (FinalizeReleases).
  PendingKeep pk;
  pk.handle = stream_->EvictTicketSlots(slots);
  pk.slots = slots;
  pk.bits = t.everDirty;
  pk.until = tick;
  pk.ticket = i;
  pk.conservative = t.conservative;
  keeps_.push_back(std::move(pk));
  world_->SetTicketBox(i, t.lo, false);
  tableDirty_ = true;
  stream_->ClearTicketSlots(slots);
  t.state = State::Releasing;
  t.releaseTick = tick;
  stats_.released++;
  if (reason == (uint32_t)TicketReason::Idle) stats_.releasedIdle++;
  else if (reason == (uint32_t)TicketReason::Timeout) stats_.releasedTimeout++;
  else if (reason == (uint32_t)TicketReason::WindowOverlap) stats_.releasedOverlap++;
  Record(TicketOp::kRelease, reason, i, tick, t.lo);
}

// ---- THE FOLD: one published snapshot, once --------------------------------
// World::Snap() at tick T is the snapshot of T - kSnapshotLatency, exactly, so
// folding it once per Tick is folding every published snapshot exactly once —
// provided Tick runs every tick. A harness that skipped ticks shows up as a
// GAP in the snapshot ticks; the fold cannot know what the missed snapshots
// said, so every ticket and pending batch it was owed to keeps all its chunks
// (conservative is never lossy).
void Tickets::Fold(uint32_t tick) {
  (void)tick;
  const WorldSnapshot& snap = world_->Snap();
  if (!snap.valid) return;
  if (haveFold_ && snap.tick == lastFoldTick_) return;
  const bool gap = haveFold_ && snap.tick != lastFoldTick_ + 1;
  lastFoldTick_ = snap.tick;
  haveFold_ = true;
  if (snap.dirtyFlags.size() != kNumSlots) return;
  const int hi = (int)kTicketBoxN - 1;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    Ticket& t = t_[i];
    if (t.state != State::Live) continue;
    if (gap) t.conservative = true;
    if (snap.tick < t.activated) continue;  // the slots' previous occupant
    bool activeDirty = false;
    const uint32_t base = World::TicketSlotBase(i);
    for (uint32_t l = 0; l < kTicketChunks; l++) {
      if (snap.tick < t.since[l]) { t.lastDirty[l] = 0; continue; }
      const uint8_t d = snap.dirtyFlags[base + l] != 0 ? 1 : 0;
      t.lastDirty[l] = d;
      t.everDirty[l] |= d;
      // Active iff the chunk is in the box's interior. The slot holds chunk
      // lo + ((m - lo) mod 5); interior = offset 1..3 on every axis.
      const IVec3 wc = world_->TicketSlotWorldChunk(base + l);
      const IVec3 dd{wc.x - t.lo.x, wc.y - t.lo.y, wc.z - t.lo.z};
      const bool interior =
          dd.x >= 1 && dd.y >= 1 && dd.z >= 1 && dd.x < hi && dd.y < hi && dd.z < hi;
      if (interior) {
        if (d) activeDirty = true;
        continue;
      }
      // SHELL: did matter cross its faces? (Not on the fill's own snapshot:
      // the count is still settling from the copy-in.)
      if (snap.tick <= t.since[l] + 1 || snap.occupancy.size() != kNumSlots) continue;
      const uint32_t occ = snap.occupancy[base + l];
      if (t.shellOcc[l] != kOccUnset && occ != t.shellOcc[l]) {
        if (t.firstHit.empty()) {
          char buf[128];
          std::snprintf(buf, sizeof buf, "chunk (%d,%d,%d) occ %u->%u at snapshot %u",
                        wc.x, wc.y, wc.z, t.shellOcc[l], occ, snap.tick);
          t.firstHit = buf;
        }
        t.shellHit[l] = 1;
      }
      t.shellOcc[l] = occ;
    }
    t.idleSnaps = activeDirty ? 0 : t.idleSnaps + 1;
  }
  for (PendingKeep& pk : keeps_) {
    if (gap) pk.conservative = true;
    if (snap.tick >= pk.until) continue;  // after the copy: not this batch's
    for (size_t k = 0; k < pk.slots.size(); k++)
      if (snap.dirtyFlags[pk.slots[k]] != 0) pk.bits[k] = 1;
  }
  // ---- PARTICLE LANDINGS ASK FOR TICKETS (P2) ----------------------------
  // The parked particles of the snapshot's tick, one request per bucket the
  // kernel filled (sim_particle.wgsl farRequest). Once per published
  // snapshot (this function's guard), stamped with the snapshot's tick so
  // ApplyRequests orders them with everything else by (tick, chunk).
  for (const IVec3& c : snap.ticketReq) Request(c, TicketReason::Particle, snap.tick);
}

void Tickets::FinalizeReleases(uint32_t tick) {
  (void)tick;
  const WorldSnapshot& snap = world_->Snap();
  for (size_t k = 0; k < keeps_.size();) {
    PendingKeep& pk = keeps_[k];
    // The copy describes ticks < until; the last of them is published when
    // Snap().tick reaches until - 1. (A batch released on tick 0 has nothing
    // older to wait for.)
    const bool ready = pk.until == 0 || (snap.valid && snap.tick + 1 >= pk.until);
    if (!ready) { k++; continue; }
    std::vector<uint8_t> keep = pk.bits;
    if (pk.conservative) {
      std::fill(keep.begin(), keep.end(), (uint8_t)1);
      stats_.conservative++;
    }
    for (uint8_t b : keep) {
      if (b) stats_.chunksKept++;
      else stats_.chunksSkipped++;
    }
    stream_->SetEvictKeep(pk.handle, keep);
    if (pk.ticket < kTicketMax && t_[pk.ticket].state == State::Releasing)
      t_[pk.ticket].state = State::Free;
    keeps_.erase(keeps_.begin() + (ptrdiff_t)k);
  }
}

void Tickets::ReleaseIdle(uint32_t tick) {
  for (uint32_t i = 0; i < kTicketMax; i++) {
    Ticket& t = t_[i];
    if (t.state != State::Live) continue;
    // Matter that reached the shell is frozen there, not settled: a ticket
    // with a shell arrival still pending its re-centre does not idle out.
    // Bounded: Recentre clears every shellHit within kTicketRecentreTicks,
    // whether it moves the box or finds the step blocked.
    const bool shellPending =
        std::find(t.shellHit.begin(), t.shellHit.end(), (uint8_t)1) != t.shellHit.end();
    if (t.idleSnaps >= kTicketIdleTicks && !shellPending)
      Release(i, (uint32_t)TicketReason::Idle, tick);
    else if (tick >= t.activated && tick - t.activated >= kTicketMaxTicks)
      Release(i, (uint32_t)TicketReason::Timeout, tick);
  }
}

void Tickets::ApplyRequests(uint32_t tick) {
  if (pending_.empty()) return;
  // "Lower tick, then lexicographic wc" (§2.4) — and the reason last, so two
  // requests for one chunk resolve the same way in every run.
  std::sort(pending_.begin(), pending_.end(), [](const Req& a, const Req& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.chunk.x != b.chunk.x) return a.chunk.x < b.chunk.x;
    if (a.chunk.y != b.chunk.y) return a.chunk.y < b.chunk.y;
    if (a.chunk.z != b.chunk.z) return a.chunk.z < b.chunk.z;
    return a.reason < b.reason;
  });
  std::vector<Req> reqs;
  reqs.swap(pending_);
  for (const Req& r : reqs) {
    stats_.requests++;
    if (world_->ChunkInWindow(r.chunk)) { stats_.inWindow++; continue; }
    if (TicketHolding(r.chunk) != kTicketMax) { stats_.absorbed++; continue; }
    uint32_t free = kTicketMax;
    for (uint32_t i = 0; i < kTicketMax; i++)
      if (t_[i].state == State::Free) { free = i; break; }
    if (free == kTicketMax) {
      stats_.refusedCap++;
      Record(TicketOp::kRefuse, (uint32_t)TicketReason::Cap, kTicketMax, tick, r.chunk);
      continue;
    }
    IVec3 lo;
    if (!PlaceBox(r.chunk, lo)) {
      stats_.refusedPlacement++;
      Record(TicketOp::kRefuse, (uint32_t)TicketReason::Placement, kTicketMax, tick,
             r.chunk);
      continue;
    }
    Activate(free, lo, r.reason, tick);
  }
}

void Tickets::ReleaseOverlapping(IVec3 newOrigin, uint32_t tick) {
  bool any = false;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    if (t_[i].state != State::Live) continue;
    if (!BoxHitsWindow(t_[i].lo, newOrigin)) continue;
    Release(i, (uint32_t)TicketReason::WindowOverlap, tick);
    any = true;
  }
  if (any) Publish();
}

// ---- RE-CENTRING (P4, docs/PLAN_chunk_tickets.md §3) -----------------------
//
// Matter the interior pushes into the shell is frozen there (the shell is
// never dispatched). When the latest published snapshot shows shell chunks on
// one FACE of the box dirty, the box steps one chunk toward that face — at
// most once per kTicketRecentreTicks per ticket. Because a box chunk lives at
// its coordinate MOD 5, the plane the box leaves and the plane it enters use
// the SAME 25 slots: the leaving plane is copied out (a two-phase keep batch,
// exactly as a release), the table moves, and the same slots are refilled
// from the store or procgen for the entering plane — a window shift at the
// box's scale, recorded as one TicketOp. The face is the one with the most
// dirty shell chunks; ties go to the lower axis, then the negative side, so
// the choice is a pure function of the snapshot. A step that would touch the
// window or another live box is not taken.
void Tickets::Recentre(uint32_t tick) {
  const int n = (int)kTicketBoxN;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    Ticket& t = t_[i];
    if (t.state != State::Live) continue;
    if (tick < t.lastRecentre + kTicketRecentreTicks) continue;
    int best = 0, bestAxis = -1, bestDir = 0;
    for (int a = 0; a < 3; a++)
      for (int side = 0; side < 2; side++) {
        const int off = side == 0 ? 0 : n - 1;
        int count = 0;
        for (uint32_t l = 0; l < kTicketChunks; l++) {
          if (!t.shellHit[l]) continue;
          const IVec3 wc = world_->TicketSlotWorldChunk(World::TicketSlotBase(i) + l);
          const int d[3] = {wc.x - t.lo.x, wc.y - t.lo.y, wc.z - t.lo.z};
          if (d[a] == off) count++;
        }
        if (count > best) {
          best = count;
          bestAxis = a;
          bestDir = side == 0 ? -1 : 1;
        }
      }
    if (bestAxis < 0) continue;
    // THE BOX FOLLOWS STRAY MATTER ONLY WHEN IT CAN LEAVE NOTHING BEHIND: the
    // interior plane the step turns into shell (offset 1 stepping +, offset
    // n-2 stepping -) must be quiet in the latest snapshot. Otherwise the
    // ticket's own activity would freeze in the new shell to chase a grain at
    // the old one (a pile spilling one cell over an edge, a leaf, a ripple).
    {
      const int freezeOff = bestDir > 0 ? 1 : n - 2;
      bool busy = false;
      for (uint32_t l = 0; l < kTicketChunks && !busy; l++) {
        if (!t.lastDirty[l]) continue;
        const IVec3 wc = world_->TicketSlotWorldChunk(World::TicketSlotBase(i) + l);
        const int d[3] = {wc.x - t.lo.x, wc.y - t.lo.y, wc.z - t.lo.z};
        bool inner = true;
        for (int a = 0; a < 3; a++) inner = inner && d[a] >= 1 && d[a] <= n - 2;
        if (inner && d[bestAxis] == freezeOff) busy = true;
      }
      if (busy) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "#%u REFUSED axis %d dir %+d (%d shell chunks hit, first %s): the "
                      "plane it would freeze is active",
                      i, bestAxis, bestDir, best, t.firstHit.c_str());
        recentreNote_ = buf;
        std::fill(t.shellHit.begin(), t.shellHit.end(), (uint8_t)0);
        t.firstHit.clear();
        stats_.recentreRefused++;
        continue;
      }
    }
    IVec3 nlo = t.lo;
    if (bestAxis == 0) nlo.x += bestDir;
    else if (bestAxis == 1) nlo.y += bestDir;
    else nlo.z += bestDir;
    if (BoxHitsWindow(nlo, world_->WindowOrigin()) || BoxHitsLive(nlo, i)) {
      // Not takeable: forget the arrivals, so the ticket can idle out.
      std::fill(t.shellHit.begin(), t.shellHit.end(), (uint8_t)0);
      t.firstHit.clear();
      stats_.recentreRefused++;
      continue;
    }
    // The leaving plane: offset (n-1) on the axis when stepping -, 0 when +.
    const int leaveOff = bestDir > 0 ? 0 : n - 1;
    std::vector<uint32_t> slots, locals;
    for (uint32_t l = 0; l < kTicketChunks; l++) {
      const IVec3 wc = world_->TicketSlotWorldChunk(World::TicketSlotBase(i) + l);
      const int d[3] = {wc.x - t.lo.x, wc.y - t.lo.y, wc.z - t.lo.z};
      if (d[bestAxis] != leaveOff) continue;
      slots.push_back(World::TicketSlotBase(i) + l);
      locals.push_back(l);
    }
    PendingKeep pk;
    pk.handle = stream_->EvictTicketSlots(slots);
    pk.slots = slots;
    for (uint32_t l : locals) pk.bits.push_back(t.everDirty[l]);
    pk.until = tick;
    pk.ticket = kTicketMax;   // the ticket stays live; nothing to free
    pk.conservative = t.conservative;
    keeps_.push_back(std::move(pk));
    t.lo = nlo;
    world_->SetTicketBox(i, nlo, true);
    // Before the refill's genChunk resolves the entering chunks (Activate's
    // reason for uploading at once).
    world_->UploadTicketTable(stream_->Ctx()->queue);
    for (uint32_t l : locals) {
      t.everDirty[l] = 0;
      t.lastDirty[l] = 0;
      t.since[l] = tick;
    }
    stream_->FillTicketSlots(slots);
    std::fill(t.shellHit.begin(), t.shellHit.end(), (uint8_t)0);
    {
      char buf[256];
      std::snprintf(buf, sizeof buf, "#%u axis %d dir %+d (%d shell chunks hit, first %s)",
                    i, bestAxis, bestDir, best, t.firstHit.c_str());
      recentreNote_ = buf;
    }
    t.firstHit.clear();
    // The interior/shell split moved with the box: every baseline is stale.
    std::fill(t.shellOcc.begin(), t.shellOcc.end(), kOccUnset);
    t.lastRecentre = tick;
    t.idleSnaps = 0;
    stats_.recentred++;
    Record(TicketOp::kRecentre, (uint32_t)TicketReason::Recentre, i, tick, nlo);
  }
}

void Tickets::Tick(uint32_t tick) {
  Fold(tick);
  FinalizeReleases(tick);
  ReleaseIdle(tick);
  TakeDeposits();
  ApplyRequests(tick);
  Recentre(tick);
  QueueResidentLandings();
  for (const Ticket& t : t_)
    if (t.state == State::Live) stats_.activeTickSum += 27;
  Publish();
}

void Tickets::DropAll(const rhi::Queue& queue) {
  for (uint32_t i = 0; i < kTicketMax; i++) {
    Ticket& t = t_[i];
    if (t.state == State::Live && stream_) stream_->ClearTicketSlots(SlotsOf(i));
    if (t.state != State::Free) world_->SetTicketBox(i, t.lo, false);
    t.state = State::Free;
    t.conservative = false;
  }
  // A pending batch's bytes belong to the replaced world: never put them.
  for (PendingKeep& pk : keeps_)
    if (stream_)
      stream_->SetEvictKeep(pk.handle, std::vector<uint8_t>(pk.slots.size(), 0));
  keeps_.clear();
  // A PARTICLE's request describes the replaced world; a manual or a gate's
  // does not (--ticket is queued before the startup worldgen runs).
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [](const Req& r) {
                                  return r.reason == (uint32_t)TicketReason::Particle;
                                }),
                 pending_.end());
  landings_.clear();
  spawnQueue_.clear();
  haveFold_ = false;
  world_->UploadTicketTable(queue);
  tableDirty_ = false;
  Publish();
}

void Tickets::Publish() {
  if (tableDirty_ && stream_ && stream_->Ctx()) {
    world_->UploadTicketTable(stream_->Ctx()->queue);
    tableDirty_ = false;
  }
  uint32_t live = 0, releasing = 0;
  for (const Ticket& t : t_) {
    live += t.state == State::Live ? 1u : 0u;
    releasing += t.state == State::Releasing ? 1u : 0u;
  }
  stats_.live = live;
  stats_.releasing = releasing;
  stats_.highWater = std::max(stats_.highWater, live);
  stats_.landingsParked = (uint32_t)landings_.size();
  g_lastStats = stats_;
}

std::vector<std::string> Tickets::Describe(uint32_t tick) const {
  std::vector<std::string> out;
  for (uint32_t i = 0; i < kTicketMax; i++) {
    const Ticket& t = t_[i];
    if (t.state == State::Free) continue;
    char buf[160];
    if (t.state == State::Live)
      std::snprintf(buf, sizeof buf, "#%u live  box (%d,%d,%d)+5  age %u  idle %u/%u  %s",
                    i, t.lo.x, t.lo.y, t.lo.z, tick - t.activated, t.idleSnaps,
                    kTicketIdleTicks, TicketReasonName(t.reason));
    else
      std::snprintf(buf, sizeof buf, "#%u releasing  box (%d,%d,%d)+5  since t%u", i,
                    t.lo.x, t.lo.y, t.lo.z, t.releaseTick);
    out.push_back(buf);
  }
  return out;
}

// ---- far landings (P2) -------------------------------------------------------
//
// A deposited particle is dead on the GPU; World holds its state from the
// published snapshot until this takes it (World::TakeTicketDeposits), and
// this holds it until its chunk is resident. In arrival order, which is
// snapshot-tick order then deposit-slot order: a pure function of the tick.
void Tickets::TakeDeposits() {
  const uint64_t dropped0 = world_->TicketDepositsDropped();
  for (const ParticleSpawn& d : world_->TakeTicketDeposits()) {
    Landing l;
    l.chunk = {(d.px >> 8) >> 4, (d.py >> 8) >> 4, (d.pz >> 8) >> 4};
    l.p = d;
    l.p.vx = l.p.vy = l.p.vz = 0;
    l.p.flags = 0;  // the spawn kernel forces ALIVE; nothing else survives
    landings_.push_back(l);
    stats_.landingsDeposited++;
  }
  stats_.landingsDropped += world_->TicketDepositsDropped() - dropped0;
  if (landings_.size() > kFarLandingMax) {
    const size_t over = landings_.size() - kFarLandingMax;
    stats_.landingsDropped += over;
    landings_.erase(landings_.begin(), landings_.begin() + (ptrdiff_t)over);
  }
}

// Kept as the named hook the refill paths call (Stream::FillSlots): the move
// itself happens in QueueResidentLandings at the next ticket step, which
// asks residency directly, so a landing whose deposit arrives AFTER its chunk
// became resident is found the same way.
void Tickets::OnChunkResident(IVec3 wc) {
  (void)wc;
}

void Tickets::QueueResidentLandings() {
  if (landings_.empty()) return;
  size_t w = 0;
  for (size_t i = 0; i < landings_.size(); i++) {
    if (world_->ChunkResident(landings_[i].chunk)) {
      spawnQueue_.push_back(landings_[i].p);
    } else {
      if (w != i) landings_[w] = landings_[i];
      w++;
    }
  }
  landings_.resize(w);
}

uint32_t Tickets::DrainLandingSpawns(std::vector<ParticleSpawn>& out, uint32_t max) {
  const uint32_t n = (uint32_t)std::min<size_t>(max, spawnQueue_.size());
  out.insert(out.end(), spawnQueue_.begin(), spawnQueue_.begin() + n);
  spawnQueue_.erase(spawnQueue_.begin(), spawnQueue_.begin() + n);
  stats_.landingsRespawned += n;
  Publish();
  return n;
}
