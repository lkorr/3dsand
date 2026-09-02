// renderstats.h — reads the raymarch's RENDER_STATS counters back, one frame
// behind, with no fence on the frame path.
//
// WHAT THIS IS FOR. The world raymarch is ONE fullscreen fragment shader, so
// the GPU timestamp around its draw is one number (perfnodes.h `raymarch`).
// Everything inside it — the primary DDA, the sun shadow rays, media, the far
// cascades, micro bricks, reflections, the MPM surface — is invisible to a
// timestamp. raymarch.wgsl counts steps per call site into World::renderStats
// (world.h kRenderStat*) when the RENDER_STATS prelude const is on, and this
// ring brings those words to the CPU so the Performance tab can show which
// trace took the steps. Measurement-only: nothing here runs unless --telemetry
// or --perf asked for it, and nothing here can move the world hash.
//
// THE SHAPE is PassTimer's deferred ring (gpu/passtimer.h): copy into a staging
// slot at the end of the frame's render command buffer, MapReadDeferred it,
// poll a few frames later, hand the caller the frame tag it belongs to. The
// copy goes through rhi::CommandEncoder::CopyRenderWritten — the one place a
// render-domain write is declared to the barrier tracker — because the source
// was written by fragment atomics in the SAME command buffer and the
// fragment->transfer barrier has to be derived, not assumed.
//
// THE COUNTERS ARE MONOTONIC. The shader never clears them; a harvested frame
// is the difference from the previous harvested frame, in u32 arithmetic. That
// removes a clear (and its barrier) from the frame, and it means a slot the
// ring could not spare is a GAP in the series rather than a corrupted value.
// The first harvest after Init has nothing to difference against and is
// dropped for the same reason.

#pragma once

#include <cstdint>
#include <cstring>

#include "gpu/context.h"
#include "gpu/resources.h"
#include "gpu/rhi.h"
#include "sim/world.h"

namespace sandvox {

class RenderStatsRing {
 public:
  // One harvested frame: kRenderStatSlots per-frame totals, already summed
  // over the stripes and scaled by the shader's pixel sample (world.h
  // kRenderStatSample), so slot 0 is ~the pixel count and slots 1.. are the
  // per-frame step / pixel totals perfnodes.h's PerfCounter::Rm* name.
  struct Frame {
    uint32_t frame = 0;
    double counters[kRenderStatSlots] = {};
  };

  bool Init(GpuContext& ctx) {
    for (int i = 0; i < kRing; i++) {
      ring_[i].staging = CreateBuffer(
          ctx.device, kRenderStatBytes,
          rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
          "renderStatsStaging");
      if (!ring_[i].staging) return false;
    }
    ok_ = true;
    return true;
  }
  bool Valid() const { return ok_; }

  // Record AFTER rhi::RenderPass::End() in the frame's render encoder. Returns
  // false when every slot is still in flight (the frame is a gap, see above).
  bool Encode(const rhi::CommandEncoder& enc, const World& world, uint32_t frame) {
    if (!ok_) return false;
    Slot& s = ring_[head_];
    if (s.ticket) return false;
    enc.CopyRenderWritten(world.renderStats, 0, s.staging, 0, kRenderStatBytes);
    s.tag = frame;
    s.encoded = true;
    return true;
  }

  // Kick the map for the slot Encode filled. Call after the Queue::Submit that
  // carried the copy; MapReadDeferred borrows that submit's fence.
  void Kick(GpuContext& ctx) {
    if (!ok_) return;
    Slot& s = ring_[head_];
    if (!s.encoded || s.ticket) return;
    s.ticket = rhi::MapReadDeferred(ctx.device, s.staging, 0, kRenderStatBytes);
    s.encoded = false;
    head_ = (head_ + 1) % kRing;
  }

  // Harvest the OLDEST landed frame, if any. Walks the ring from the tail and
  // stops at the first slot still in flight, so frames come out in order —
  // which the differencing below depends on. Call until it returns false.
  bool Poll(Frame& out) {
    if (!ok_) return false;
    Slot& s = ring_[tail_];
    if (!s.ticket) return false;
    if (!s.ticket.Ready()) return false;
    bool got = false;
    if (s.ticket.Succeeded()) {
      const uint32_t* w = static_cast<const uint32_t*>(s.ticket.Data());
      if (w) {
        uint32_t tot[kRenderStatSlots] = {};
        for (uint32_t st = 0; st < kRenderStatStripes; st++)
          for (uint32_t k = 0; k < kRenderStatSlots; k++)
            tot[k] += w[st * kRenderStatSlots + k];
        if (havePrev_) {
          out.frame = s.tag;
          for (uint32_t k = 0; k < kRenderStatSlots; k++)
            out.counters[k] =
                (double)(uint32_t)(tot[k] - prev_[k]) * (double)kRenderStatSample;
          got = true;
        }
        std::memcpy(prev_, tot, sizeof prev_);
        havePrev_ = true;
      }
    }
    s.ticket.Unmap();
    s.ticket = rhi::MapTicket();
    tail_ = (tail_ + 1) % kRing;
    return got;
  }

 private:
  static constexpr int kRing = 6;
  struct Slot {
    rhi::Buffer staging;
    rhi::MapTicket ticket;   // in flight when truthy
    uint32_t tag = 0;
    bool encoded = false;    // Encode ran, Kick has not
  };
  Slot ring_[kRing];
  int head_ = 0;   // slot Encode writes next
  int tail_ = 0;   // slot Poll harvests next
  bool ok_ = false;
  bool havePrev_ = false;
  uint32_t prev_[kRenderStatSlots] = {};
};

}  // namespace sandvox
