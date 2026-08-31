// passtimer.h — per-ComputePass GPU timestamps, for the measurement harnesses
// (`--measure`, `--perf`) and the live telemetry stream.
//
// MEASUREMENT-ONLY TOOLING. Nothing in the DEFAULT frame path constructs one of
// these, and Simulation holds a NULL PassTimer* unless a harness or --telemetry
// hands it one, so the game and the selftest encode byte-identical command
// buffers whether or not this file is compiled in. It exists to size the
// planned Vulkan port: which pass is bandwidth-bound, which is occupancy-bound,
// and what a settled world actually costs.
//
// TWO GRANULARITIES, and the difference matters. By default a timestamp pair
// spans a run of rows sharing a `group` label in pass_table.def — the
// granularity the phase-0 Vulkan baseline was measured at, so --measure's
// numbers stay comparable to it. `SetRowGranularity(true)` moves the pair onto
// the individual PASS() row instead, which is what the Performance tab needs:
// `prep(mutate+explode+compact)` is ONE group covering THREE architecture
// nodes, and a page that cannot separate them cannot answer "is the mutation
// queue or the explosion costing me this frame".
//
// Row granularity does NOT change the recording. A timestamp write is not a
// barrier and not a pass boundary — under Vulkan there is no compute-pass
// concept at all (barrier_graph §1.2), so `group` is already just a label. The
// dispatches, the barriers and therefore the world hash are identical either
// way, and the --perf harness asserts exactly that.
//
// TWO COLLECTION MODES. Collect() blocks, which is right for a sizing run that
// wants the GPU stopped and looked at, and fatal for a live session at 60 fps.
// KickDeferred()/PollDeferred() issue the map and harvest it some frames later
// through a ring, so the frame path never waits — the numbers arrive late, and
// the caller is told which frame they belong to rather than being allowed to
// assume they are this one.
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

// One pass's time in ONE command buffer, as opposed to PassTimer::Stat's
// running total. The Performance tab plots a value per frame, so the per-buffer
// number is the primary product and the totals are the summary.
struct PassSample {
  const char* name = nullptr;  // string literal from pass_table.def; stable
  uint64_t ns = 0;
};

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

  // GROUP (default) or ROW granularity — see the header comment. Must be set
  // before the first EncodeTick; changing it mid-run mixes two naming schemes
  // into one stats table and the totals stop meaning anything.
  void SetRowGranularity(bool on) { rowGranularity_ = on; }
  bool RowGranularity() const { return rowGranularity_; }

  // Called once per command buffer, immediately before enc.Finish(): resolves
  // the query set into the resolve buffer and copies it to the readback
  // staging buffer.
  void EncodeResolve(const rhi::CommandEncoder& enc);

  // Blocking map + accumulate into the per-name totals. Call after the queue
  // has gone idle. Measurement harness only — this is a synchronous readback.
  void Collect(GpuContext& ctx);

  // ---- deferred collection, for the live telemetry path -------------------
  //
  // KickDeferred() issues a non-blocking map of the slot EncodeResolve just
  // wrote and tags it with `frame`; PollDeferred() harvests every slot whose
  // fence has retired, accumulates it into the totals, and returns the number
  // harvested. Between them the frame path never waits on the GPU.
  //
  // `frame` is carried through rather than inferred because the whole point is
  // that the numbers are LATE: the caller gets back "these are frame 412's
  // passes", three frames after frame 412, and can put them in the right column
  // of the chart instead of smearing them onto the present.
  void KickDeferred(GpuContext& ctx, uint32_t frame);
  int PollDeferred(GpuContext& ctx);

  // The most recently harvested command buffer's per-pass times, and the frame
  // tag it was kicked with. Valid after Collect() or a PollDeferred() that
  // returned non-zero.
  const std::vector<PassSample>& LastFrame() const { return last_; }
  uint32_t LastFrameTag() const { return lastTag_; }

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
  // Accumulate one resolved buffer's timestamps into stats_ and last_.
  void Absorb(GpuContext& ctx, const uint64_t* ts, size_t count,
              const std::vector<const char*>& names, uint32_t tag);

  rhi::QuerySet querySet_;
  rhi::Buffer resolve_;
  uint32_t capacity_ = 0;
  uint32_t used_ = 0;                  // query slots consumed this command buffer
  std::vector<const char*> pending_;   // pass names in slot order, this buffer
  std::vector<Stat> stats_;
  bool rowGranularity_ = false;

  // The staging ring. ONE staging buffer is enough for the blocking path — the
  // GPU is idle when it is read — but a deferred map holds its buffer until the
  // fence retires, and the next frame's CopyBufferToBuffer would be writing
  // into a mapped allocation. Hence a ring, and hence EncodeResolve advancing
  // it: the buffer being copied into is never the one still in flight.
  static constexpr int kRing = 6;
  struct Slot {
    rhi::Buffer staging;
    rhi::MapTicket ticket;             // in flight when truthy
    std::vector<const char*> names;    // pass names this slot's queries carry
    uint32_t queries = 0;
    uint32_t tag = 0;                  // caller's frame number
  };
  Slot ring_[kRing];
  int ringHead_ = 0;                   // slot EncodeResolve will write next

  std::vector<PassSample> last_;
  uint32_t lastTag_ = 0;
};
