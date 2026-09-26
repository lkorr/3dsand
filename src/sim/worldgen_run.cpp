// worldgen_run.cpp — SubmitWorldgen and ReadVoxelsSync, moved verbatim out of
// src/test/support.cpp (see worldgen_run.h for why they are sim-layer).

#include "sim/worldgen_run.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "gpu/rhi.h"
#include "sim/pagetable.h"
#include "sim/waterbody.h"
#include "sim/worldedit.h"

namespace sandvox {

void SubmitWorldgen(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  // The seed the CPU may assume the ground under: Stream::Init does this in
  // the game; the harness has no stream, and a spell flight past the mirror
  // reads World::TerrainHeight(x, z, world.WorldSeed()) for the cells nobody
  // has fetched (spell.cpp, SpellSystem::Tick). Mirror seed only -- the page
  // table's seed stays as it was, so residency classification is untouched.
  world.SetMirrorSeed(seed);
  // The authored edit layer patches whatever worldgen produces, so a fresh
  // world re-queues every edited chunk the window contains. Queue only — the
  // ops go out through the MutationQueue on the ticks that follow (rule 3), not
  // by writing voxels from here.
  WorldEditLayer().QueueWindow(world);
  // THE WATER-BODY LEDGER DESCRIBES A WORLD THAT NO LONGER EXISTS. Same
  // argument as InvalidateSnapshot below and the same failure shape: the ledger
  // is GPU-carried state (level, volume, debit, hole), a fresh worldgen refills
  // the lake to its authored height, and a descriptor that survived would go on
  // shaving at the old level against a hole that was filled in. Derived data is
  // reconstructible and DISPOSABLE (plan section 3.1) — so dispose of it, on
  // both sides: the CPU registry and the GPU record.
  WaterBodies().Reset();
  {
    static const std::vector<int32_t> kZero((size_t)kWaterBodyStateTotalWords,
                                            0);
    ctx.queue.WriteBuffer(world.waterBodyState, 0, kZero.data(),
                          kZero.size() * sizeof(int32_t));
  }
  // The held snapshot describes the OLD world; scenes and gates also restart
  // their tick counters, which can make its stamp read as newer than the new
  // world's early ticks. See World::InvalidateSnapshot. Measured on --shot's
  // oil-slick scene: the stale stamp suppressed every tightening AND the
  // staleness fallback at once, and the unsnapshotted mirror dilated a ring
  // per tick through the pool.
  world.InvalidateSnapshot();
  TickParams tp{0, seed, 0, 0};
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  tp.labMode = World::LabWorld() ? 1u : 0u;  // fluid-lab slab (world.h)
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (world.residency != World::Residency::Paged) {
    // The dense `main` reads the column cache like `list` does: every slot is
    // its own list position (before the encoder: WriteBuffer is deferred).
    sim.WriteDenseGenList(ctx.queue);
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeWorldgen(enc);
    ctx.queue.Submit(enc.Finish());
    return;
  }

  // ---- BATCHED worldgen (PLAN_page_table.md §3.5c, §9 open question 1) ----
  //
  // genChunk writes all 4,096 cells of every target slot and does NOT know in
  // advance which it will fill with air, so every slot it touches must have a
  // page before the dispatch — a kernel cannot allocate. Doing all 32,768 at
  // once would need a dense pool (512 MiB), i.e. no saving at all at the
  // moment of worldgen, and under §3.8's fatal policy it would simply ABORT at
  // startup with kPoolPages = 8192.
  //
  // So: materialize a batch, dispatch it through `worldgenList` (which already
  // takes a slot list — the primitive was there), read its occupancy, demote
  // the empties, and let those pages come back for the next batch. The
  // transient is bounded by the batch size and it costs 32768/2048 = 16
  // submits at startup, which is nothing off the frame path. It also
  // generalizes to a grown window, where a dense transient would be 4 GiB and
  // simply impossible.
  {
    const uint32_t kGenBatch = kWorldgenBatch;   // world.h: genAct covers it
    // The first submit still has to clear the transient buffers (hash,
    // support, particle counts, both dirty pages) exactly as EncodeWorldgen
    // does, so run it over an EMPTY slot list: the fills land, the dispatch
    // covers zero slots.
    world.pages->ResetAllEmpty(ctx.queue);
    {
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeWorldgen(enc, /*denseGen=*/false);
      ctx.queue.Submit(enc.Finish());
    }
    std::vector<uint32_t> batch;
    std::vector<uint32_t> verdict(kGenBatch, 0u);
    std::vector<uint32_t> vox;   // words: only for the check / a fallback
    batch.reserve(kGenBatch);
    for (uint32_t base = 0; base < kNumSlots; base += kGenBatch) {
      const uint32_t n = std::min(kGenBatch, kNumSlots - base);
      batch.clear();
      for (uint32_t k = 0; k < n; k++) {
        batch.push_back(base + k);
        world.pages->EnsurePageForOverwrite(base + k);
      }
      world.pages->FlushTableWrites(ctx.queue);
      sim.WriteGenList(ctx.queue, batch);
      TickParams gp{};
      gp.seed = seed;
      gp.genCount = (uint32_t)batch.size();
      gp.origin[0] = wo.x; gp.origin[1] = wo.y; gp.origin[2] = wo.z;
      gp.labMode = World::LabWorld() ? 1u : 0u;  // fluid-lab slab (world.h)
      ctx.queue.WriteBuffer(world.tickUBO, 0, &gp, sizeof(gp));
      rhi::CommandEncoder ge = ctx.device.CreateCommandEncoder();
      sim.EncodeGenList(ge, (uint32_t)batch.size());
      ctx.queue.Submit(ge.Finish());
      // Classify and demote, which returns the all-air pages to the free list
      // for the next batch. This is the compaction §3.5c calls for, run
      // eagerly once per batch rather than on the hysteresis cadence.
      //
      // ON THE GPU'S VERDICT, NOT THE WORDS. genChunk's `list` entry reduces
      // PageTable::Classify's three tests over the words it wrote and
      // publishes the class into genAct (world.h kGenVerdict*), so a batch
      // costs an 8 KiB readback instead of ReadVoxelsSync's 32 MiB — 512 MiB
      // per paged worldgen, which startup, every menu regen, vk_smoke and
      // ~220 selftest regens used to pay, plus a CPU Classify of 32,768
      // chunks. The words are read only when a verdict was not published.
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.genAct, 0,
                            verdict.data(), (size_t)n * 4, "wgVerdict");
      bool needWords = false;
      for (uint32_t k = 0; k < n && !needWords; k++)
        if ((verdict[k] & kGenVerdictValid) == 0u) needWords = true;
      if (needWords) {
        if (vox.size() < (size_t)kGenBatch * kChunkVol)
          vox.resize((size_t)kGenBatch * kChunkVol);
        ReadVoxelsSync(ctx, world, base, n, vox.data(), "wgClassify");
      }
      for (uint32_t k = 0; k < n; k++) {
        uint32_t e = world.pages->ClassifyGenVerdict(verdict[k], seed);
        if (needWords)
          e = world.pages->Classify(base + k, vox.data() + (size_t)k * kChunkVol);
        if (e != PageTable::kNeedsPage) world.pages->SetSentinel(base + k, e);
      }
      world.pages->FlushTableWrites(ctx.queue);
    }
    // ---- THE WAKE THE CPU MIRROR NEVER LEARNED ------------------------
    //
    // genChunk WAKES WHAT IT GENERATES: its tail stores
    // dirtyIn[slot] = dirtyOut[slot] = 1 for every slot holding a cell that
    // `matCanAct` (worldgen.wgsl, "THE STREAMING WAKE"). That is a dirty-set
    // MUTATION, and §3.2a's rule for those is that the CPU mirror must learn
    // the same set in the same breath — Simulation::EncodeWakeAll says it in
    // as many words and does both halves in one call, because two operations
    // that must agree is the shape this repo has checkers for everywhere.
    //
    // This path did neither half. ResetAllEmpty above clears cpuDirty, and
    // nothing here ever put the woken chunks back, so the FIRST TICK AFTER
    // ANY WORLDGEN dispatched the CA over every acting chunk in the window
    // while the mirror believed the world was asleep. Materialize's set was
    // that tick's op ring alone; every CA write that crossed a chunk
    // boundary into a chunk still held as a sentinel resolved to PT_NO_WORD
    // and voxStore DROPPED IT. Measured on main at ad35ed7: `lost sand
    // (id 3) | FIRST sim_step tick 80001 chunk (352,208,480) entry
    // 0x80000000` — tick 80001 is the first step of the gi-nightfall gate,
    // whose base tick is 80000 and which regenerates the world one line
    // before it. Exactly the streaming path's 217-fault bug with the sides
    // swapped (see PageTable::RefilledSlot), and invisible until the map
    // edit at 49137b8 moved spawn and repainted the planes: the fault needs
    // acting matter sitting on a chunk boundary with an ALL-AIR chunk on the
    // other side of it, which is a cliff edge, and which terrain the window
    // happens to hold decides.
    //
    // READ THE FLAGS BACK rather than recomputing matCanAct on the CPU. The
    // GPU's dirty buffer IS the wake, so a mirror taken from it cannot
    // disagree with it; a second implementation of the act predicate is the
    // divergence rule 3 of the design guidelines forbids. 128 KiB once per
    // worldgen, beside the sixteen 8 KiB verdict reads the loop above pays
    // (it was sixteen 32 MiB voxel reads before the GPU verdict).
    //
    // RefilledSlot, NOT WakeAll, and the difference is CLAUDE.md rule 2.
    // Contributor (d) is defined for precisely this case ("a slot the CPU
    // wrote dirty[0] AND dirty[1] for"), it is unioned AFTER the next tick's
    // tightening so an intersection cannot undo it, and it rides the C(j)
    // ring. WakeAll would also be sound and would be WRONG: it declares all
    // 32,768 slots, so Materialize's hasMatter half would page in every
    // buried stone chunk in the window plus its 26-ring — the JITTER
    // compression win deleted at every worldgen, in the one band CLAUDE.md
    // names as what actually fills the pool. The act set is DEMAND: these
    // are the chunks the CA is about to be dispatched over anyway, and their
    // ring is the write reach (<= 1 cell, rule 1) of that dispatch and
    // nothing wider.
    uint32_t woken = 0;
    {
      std::vector<uint32_t> woke((size_t)kNumSlots, 0u);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.dirty[0], 0,
                            woke.data(), (size_t)kNumSlots * 4, "wgWake");
      for (uint32_t s = 0; s < kNumSlots; s++) {
        if (woke[s] == 0u) continue;
        world.pages->RefilledSlot(s);
        woken++;
      }
    }

    // ZERO THE FAULT COUNTER AFTER WORLDGEN.
    //
    // HISTORY, not the current mechanism: the whole-window `main` dispatch
    // this path replaced stored through every slot a 8,192-page pool did not
    // hold, and those no-op stores counted — exactly kNumChunks * kChunkVol =
    // 134,217,728 before tick 1 in an otherwise correct run. The BATCHED
    // path above never does that: `list` writes only genList's slots, and
    // every one of them was given a page (EnsurePageForOverwrite) before its
    // dispatch, so a correct worldgen adds nothing to the counter and
    // ResetAllEmpty already zeroed it at the top. This write is kept as the
    // statement of what the gates assert — the TICK LOOP never writes through
    // a sentinel — and so that a counter left over from a pre-reset world
    // cannot leak into the first tick's reading.
    const uint32_t faultZero[kPageFaultWords] = {};
    ctx.queue.WriteBuffer(world.pageFaults, 0, faultZero, sizeof(faultZero));
    std::printf("worldgen (paged, %u-slot batches): %u pages in use "
                "(%.1f MiB of %.1f MiB pool), high water %u, woke %u chunks\n",
                kGenBatch, world.pages->PagesInUse(),
                (double)world.pages->PagesInUse() * kChunkVol * 4.0 / 1048576.0,
                (double)world.pages->PoolPages() * kChunkVol * 4.0 / 1048576.0,
                world.pages->PagesHighWater(), woken);
    return;
  }
}

void ReadVoxelsSync(GpuContext& ctx, World& world, uint32_t firstSlot,
                    uint32_t count, uint32_t* out, const char* label) {
  // One copy per RUN of consecutive resident slots whose pages are also
  // consecutive, so the dense case (the identity map) is still exactly one
  // copy of the whole range — which is what it was before paging.
  uint32_t i = 0;
  while (i < count) {
    const uint64_t off = world.PageOffsetOfSlot(firstSlot + i);
    if (off == World::kNoPage) {
      // Sentinel: synthesize, through the same rule the shader uses.
      // POSITIONAL (JITTER), so the world chunk travels. A JITTER chunk read
      // back through here must see the same words the GPU would, or a
      // classifier would refuse chunks it had itself just promoted. world.h
      // SynthChunkWords is the one whole-chunk form of SynthWordAt.
      SynthChunkWords(world.PageEntryOfSlot(firstSlot + i),
                      world.SlotToWorldChunk(firstSlot + i),
                      world.pages->WorldSeed(), out + (size_t)i * kChunkVol);
      i++;
      continue;
    }
    uint32_t run = 1;
    while (i + run < count) {
      const uint64_t nxt = world.PageOffsetOfSlot(firstSlot + i + run);
      if (nxt != off + (uint64_t)run * kChunkVol * 4) break;
      run++;
    }
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels, off,
                          out + (size_t)i * kChunkVol,
                          (size_t)run * kChunkVol * 4, label);
    i += run;
  }
}

}  // namespace sandvox
