#include "gpu/passtimer.h"

#include <cstring>
#include <utility>

#include "gpu/context.h"
#include "gpu/resources.h"

bool PassTimer::Init(GpuContext& ctx, uint32_t capacity) {
  if (!ctx.timestampsEnabled) return false;
  // Two queries per pass (begin + end).
  capacity_ = capacity * 2;
  querySet_ = ctx.device.CreateTimestampQuerySet(capacity_, "passTimer");
  if (!querySet_) return false;
  resolve_ = CreateBuffer(ctx.device, (uint64_t)capacity_ * 8,
                          rhi::BufferUsage::QueryResolve | rhi::BufferUsage::CopySrc,
                          "passTimerResolve");
  for (int i = 0; i < kRing; i++) {
    ring_[i].staging =
        CreateBuffer(ctx.device, (uint64_t)capacity_ * 8,
                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                     "passTimerStaging");
    if (!ring_[i].staging) return false;
  }
  return true;
}

rhi::ComputePass PassTimer::BeginPass(const rhi::CommandEncoder& enc,
                                      const char* name) {
  if (!querySet_ || used_ + 2 > capacity_) return enc.BeginComputePass();
  rhi::PassTimestampWrites tw{};
  tw.querySet = querySet_;
  tw.beginIndex = used_;
  tw.endIndex = used_ + 1;
  used_ += 2;
  pending_.push_back(name);
  return enc.BeginComputePass(name, tw);
}

bool PassTimer::AllocPassPair(const char* name, uint32_t& beginIdx, uint32_t& endIdx) {
  if (!querySet_ || used_ + 2 > capacity_) return false;
  beginIdx = used_;
  endIdx = used_ + 1;
  used_ += 2;
  pending_.push_back(name);
  return true;
}

// Close out the command buffer being recorded: resolve its queries and hand the
// pass names to a ring slot.
//
// THE PER-BUFFER RESET IS LOAD-BEARING, and getting it wrong is not a wrong
// number — it is a hung device. ResolveQuerySet copies with
// VK_QUERY_RESULT_WAIT_BIT and then resets the range, so every query in
// [0, used_) must have been WRITTEN by the command buffer being finished. If
// `used_` carried over from a previous buffer, the copy would wait on queries
// that were reset and never rewritten. Measured as a hash of 00000000 out of a
// caller that attached a timer and never collected — the arithmetic was fine,
// the device was gone.
//
// So the counters are cleared HERE, at the end of the buffer that owns them,
// rather than in Collect/KickDeferred. A caller that forgets to collect now
// loses a frame of numbers instead of the device.
void PassTimer::EncodeResolve(const rhi::CommandEncoder& enc) {
  if (!querySet_ || used_ == 0) return;
  Slot& s = ring_[ringHead_];
  // A slot whose previous map is still in flight cannot be a copy destination.
  // Dropping the resolve rather than stalling is the correct trade for a live
  // session: a skipped frame of GPU numbers is a gap in a chart, and a stall is
  // a hitch in the game the chart is measuring.
  //
  // The queries still have to be resolved and reset, or the NEXT buffer hits
  // the hazard above — so the resolve is recorded either way and only the copy
  // to the (busy) staging buffer is skipped.
  enc.ResolveQuerySet(querySet_, 0, used_, resolve_, 0);
  if (!s.ticket) {
    enc.CopyBufferToBuffer(resolve_, 0, s.staging, 0, (uint64_t)used_ * 8);
    s.names = std::move(pending_);
    s.queries = used_;
  }
  pending_.clear();
  used_ = 0;
}

// Fold one resolved buffer into the running totals and into last_.
//
// `names` is index-parallel with the (begin, end) query PAIRS, so pass p owns
// ts[2p] and ts[2p+1].
void PassTimer::Absorb(GpuContext& ctx, const uint64_t* ts, size_t count,
                       const std::vector<const char*>& names, uint32_t tag) {
  last_.clear();
  lastTag_ = tag;
  for (size_t p = 0; p < names.size(); p++) {
    if (p * 2 + 1 >= count) break;
    uint64_t b = ts[p * 2], e = ts[p * 2 + 1];
    // A zero pair means the query was never written (pass disabled by the
    // backend); a decreasing pair should not happen but is not worth a crash.
    if (b == 0 || e == 0 || e < b) continue;
    uint64_t ns = (uint64_t)((double)(e - b) * ctx.timestampPeriodNs);
    Stat* s = nullptr;
    for (Stat& c : stats_)
      if (c.name == names[p]) { s = &c; break; }
    if (!s) {
      stats_.push_back(Stat{names[p], 0, 0, 0});
      s = &stats_.back();
    }
    s->totalNs += ns;
    s->samples++;
    // Rows can repeat within one buffer (the same name opened twice); sum them
    // into a single per-frame entry so the caller sees one number per pass.
    PassSample* ps = nullptr;
    for (PassSample& q : last_)
      if (q.name == names[p]) { ps = &q; break; }
    if (ps) ps->ns += ns;
    else last_.push_back(PassSample{names[p], ns});
  }
  // Count each distinct name once per collected command buffer.
  for (Stat& c : stats_) {
    for (const char* n : names)
      if (std::strcmp(n, c.name.c_str()) == 0) { c.frames++; break; }
  }
}

void PassTimer::Collect(GpuContext& ctx) {
  if (!querySet_) return;
  Slot& s = ring_[ringHead_];
  if (s.queries == 0) return;   // EncodeResolve declined, or nothing recorded
  const uint64_t bytes = (uint64_t)s.queries * 8;
  std::vector<uint64_t> ts(s.queries, 0);
  rhi::ReadBufferBlocking(ctx.device, s.staging, 0, ts.data(), (size_t)bytes);
  Absorb(ctx, ts.data(), ts.size(), s.names, 0);
  s.queries = 0;
  s.names.clear();
}

void PassTimer::KickDeferred(GpuContext& ctx, uint32_t frame) {
  if (!querySet_) return;
  Slot& s = ring_[ringHead_];
  if (s.queries == 0 || s.ticket) return;
  s.tag = frame;
  s.ticket = rhi::MapReadDeferred(ctx.device, s.staging, 0,
                                  (uint64_t)s.queries * 8);
  ringHead_ = (ringHead_ + 1) % kRing;
}

int PassTimer::PollDeferred(GpuContext& ctx) {
  int got = 0;
  // Harvest in ring order starting one past the head, so slots are consumed
  // oldest-first and LastFrame() ends up holding the newest one.
  for (int i = 0; i < kRing; i++) {
    Slot& s = ring_[(ringHead_ + i) % kRing];
    if (!s.ticket || !s.ticket.Ready()) continue;
    if (s.ticket.Succeeded()) {
      const uint64_t* ts = static_cast<const uint64_t*>(s.ticket.Data());
      if (ts) {
        Absorb(ctx, ts, s.queries, s.names, s.tag);
        got++;
      }
    }
    s.ticket.Unmap();
    s.ticket = rhi::MapTicket();
    s.queries = 0;
    s.names.clear();
  }
  return got;
}

void PassTimer::ResetStats() {
  stats_.clear();
  last_.clear();
  // Also drop any queries encoded but never resolved (e.g. a worldgen submit,
  // which does not encode a resolve) so they cannot be attributed to the next
  // scenario. In-flight ring slots are drained rather than abandoned: their map
  // would otherwise complete into a buffer the next scenario is already using.
  for (int i = 0; i < kRing; i++) {
    if (ring_[i].ticket) {
      ring_[i].ticket.Wait();
      ring_[i].ticket.Unmap();
      ring_[i].ticket = rhi::MapTicket();
    }
    ring_[i].queries = 0;
    ring_[i].names.clear();
  }
  pending_.clear();
  used_ = 0;
}
