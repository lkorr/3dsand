#include "gpu/passtimer.h"

#include <cstring>

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
  staging_ = CreateBuffer(ctx.device, (uint64_t)capacity_ * 8,
                          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                          "passTimerStaging");
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
  pending_.emplace_back(name);
  return enc.BeginComputePass(name, tw);
}

void PassTimer::EncodeResolve(const rhi::CommandEncoder& enc) {
  if (!querySet_ || used_ == 0) return;
  enc.ResolveQuerySet(querySet_, 0, used_, resolve_, 0);
  enc.CopyBufferToBuffer(resolve_, 0, staging_, 0, (uint64_t)used_ * 8);
}

void PassTimer::Collect(GpuContext& ctx) {
  if (!querySet_ || used_ == 0) {
    pending_.clear();
    used_ = 0;
    return;
  }
  const uint64_t bytes = (uint64_t)used_ * 8;
  std::vector<uint64_t> ts(used_, 0);
  rhi::ReadBufferBlocking(ctx.device, staging_, 0, ts.data(), (size_t)bytes);

  for (size_t p = 0; p < pending_.size(); p++) {
    uint64_t b = ts[p * 2], e = ts[p * 2 + 1];
    // A zero pair means the query was never written (pass disabled by the
    // backend); a decreasing pair should not happen but is not worth a crash.
    if (b == 0 || e == 0 || e < b) continue;
    uint64_t ns = (uint64_t)((double)(e - b) * ctx.timestampPeriodNs);
    Stat* s = nullptr;
    for (Stat& c : stats_)
      if (c.name == pending_[p]) { s = &c; break; }
    if (!s) {
      stats_.push_back(Stat{pending_[p], 0, 0, 0});
      s = &stats_.back();
    }
    s->totalNs += ns;
    s->samples++;
  }
  // Count each distinct name once per collected command buffer.
  for (Stat& c : stats_) {
    for (const std::string& n : pending_)
      if (n == c.name) { c.frames++; break; }
  }
  pending_.clear();
  used_ = 0;
}

void PassTimer::ResetStats() {
  stats_.clear();
  // Also drop any queries encoded but never resolved (e.g. a worldgen submit,
  // which does not encode a resolve) so they cannot be attributed to the next
  // scenario.
  pending_.clear();
  used_ = 0;
}
