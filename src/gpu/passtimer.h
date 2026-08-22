// passtimer.h — per-ComputePass GPU timestamps, for the `--measure` harness only.
//
// MEASUREMENT-ONLY TOOLING. Nothing in the frame path constructs one of these,
// and Simulation holds a NULL PassTimer* by default, so the game and the
// selftest encode byte-identical command buffers whether or not this file is
// compiled in. It exists to size the planned Vulkan port: which pass is
// bandwidth-bound, which is occupancy-bound, and what a settled world actually
// costs.
//
// Why an interface rather than Simulation owning a QuerySet: the timer needs a
// stable pass NAME to attribute a number to, and the only place that knows the
// names is the encode site. Passing a pointer lets EncodeTick stay one
// function with one structure — a second "timed" copy of EncodeTick would be
// exactly the drift CLAUDE.md warns about for SubmitTick.
//
// Determinism note: timestamp writes are a pass-descriptor attachment. They
// observe the pass, they do not reorder or alter any dispatch, so a timed run
// produces the same world hash as an untimed one. The measure harness asserts
// that.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/rhi.h"

class GpuContext;

// One timed compute pass. Begin() returns a pass encoder that already carries
// ComputePassTimestampWrites for `name`; End() closes it. Call sites that have
// no timer attached call enc.BeginComputePass() directly.
class PassTimer {
 public:
  // capacity = max timed passes per submitted command buffer. Each pass burns
  // two query slots (begin + end).
  bool Init(GpuContext& ctx, uint32_t capacity);
  bool Valid() const { return (bool)querySet_; }

  // Begin a compute pass with timestamps attached. Falls back to an untimed
  // pass if the query set is full or unavailable, so encoding never fails.
  // DAWN PATH ONLY — the Vulkan recorder has no compute-pass concept and takes
  // its (begin, end) indices through AllocPassPair below instead.
  rhi::ComputePass BeginPass(const rhi::CommandEncoder& enc, const char* name);

  // Vulkan path (--measure --backend vulkan): hand out a (begin, end) query
  // index pair for a named pass — the bookkeeping of BeginPass without the
  // encoder. Returns false when the set is full or absent (pass goes untimed).
  // The vk recorder writes the timestamps itself at its group transitions.
  bool AllocPassPair(const char* name, uint32_t& beginIdx, uint32_t& endIdx);
  // The query set handle, for the recorder's timestamp writes.
  const rhi::QuerySet& NativeQuerySet() const { return querySet_; }

  // Called once per command buffer, immediately before enc.Finish(): resolves
  // the query set into the resolve buffer and copies it to the readback
  // staging buffer.
  void EncodeResolve(const rhi::CommandEncoder& enc);

  // Blocking map + accumulate into the per-name totals. Call after the queue
  // has gone idle. Measurement harness only — this is a synchronous readback.
  void Collect(GpuContext& ctx);

  // Reset accumulated statistics (not the query set).
  void ResetStats();

  struct Stat {
    std::string name;
    uint64_t totalNs = 0;
    uint64_t samples = 0;   // number of passes recorded under this name
    uint64_t frames = 0;    // number of collected command buffers containing it
  };
  // Accumulated per-name totals, in first-seen order.
  const std::vector<Stat>& Stats() const { return stats_; }
  // Number of timed passes encoded into the command buffer currently being
  // built — i.e. ComputePassEncoders per tick, which is the number the Vulkan
  // barrier plan cares about.
  uint32_t PassesThisBuffer() const { return used_; }

 private:
  rhi::QuerySet querySet_;
  rhi::Buffer resolve_;
  rhi::Buffer staging_;
  uint32_t capacity_ = 0;
  uint32_t used_ = 0;                 // query slots consumed this command buffer
  std::vector<std::string> pending_;  // pass names in slot order, this buffer
  std::vector<Stat> stats_;
};
