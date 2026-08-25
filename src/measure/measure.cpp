// measure.cpp — `--measure`, a headless sizing harness for the planned Vulkan
// port. MEASUREMENT ONLY: nothing here runs in the game or in --selftest, and
// it is reached solely from the `--measure` branch in main.cpp.
//
// Two questions, two measurements:
//
//   1. OCCUPANCY HISTOGRAM. How much of the 512^3 residency window is actually
//      non-air, and how many 16^3 chunks are entirely empty? An empty chunk is
//      a page sparse residency would leave unbound, so this number IS the
//      upper bound on what sparse binding can save. Read straight off the
//      per-chunk occupancy buffer the sim already maintains — (rayBlockers
//      << 16) | nonAirCount per chunk, packOcc() in common.wgsl.
//
//   1b. CHUNK UNIFORMITY. Of the chunks that are not all-air, how many are a
//      single repeated WORD? This is the number PLAN_page_table.md §3.6 turns
//      on: EMPTY is free, but UNIFORM(material) only pays if whole-word
//      uniform chunks actually exist. DENSE RESIDENCY ONLY — it reads voxels
//      at slot*16 KiB, which is ground truth only under the identity map.
//
//   2. PER-PASS GPU TIME. Where does a tick go, and does a settled world truly
//      cost nothing (rule 2)? ComputePassTimestampWrites on every pass
//      EncodeTick opens, averaged over 3 scenarios.
//
// Both use blocking readbacks. That is fine here and only here: this is not
// the frame path, and the whole point is to stop the GPU and look at it.

#include "measure/measure.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "gpu/resources.h"
#include "sim/materials.h"
#include "sim/pagetable.h"
#include "sim/simulation.h"
#include "sim/world.h"
#include "test/support.h"

namespace sandvox {
namespace {

// Blocking read of the whole per-chunk occupancy buffer (kNumChunks u32 =
// 128 KiB at 512^3 / 16^3). Measurement harness only.
std::vector<uint32_t> ReadOccupancySync(GpuContext& ctx, World& world) {
  const uint64_t bytes = (uint64_t)kNumChunks * 4;
  std::vector<uint32_t> out(kNumChunks, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.occupancy, 0, out.data(),
                        (size_t)bytes, "occRead");
  return out;
}

// The sub-chunk bitmask that follows the counts in the same buffer (world.h
// kSubOccShift). Read separately so the histogram above keeps its exact shape.
std::vector<uint32_t> ReadSubOccSync(GpuContext& ctx, World& world) {
  const uint64_t bytes = (uint64_t)kNumChunks * kSubOccStride * 4;
  std::vector<uint32_t> out((size_t)kNumChunks * kSubOccStride, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.occupancy,
                        (uint64_t)kNumChunks * 4, out.data(), (size_t)bytes,
                        "subOccRead");
  return out;
}

// ---- how much can the sub-chunk mask ACTUALLY skip? ------------------------
// The whole premise of A2 is that half-full canopy/meadow chunks hold a lot of
// empty sub-blocks. That is a claim about CONTENT, and it decides the ceiling
// on the optimisation before a single frame is timed — so measure it directly
// off the mask the producers write, rather than inferring it from frame times.
//
// The number that matters is NOT "what fraction of all blocks are empty"
// (dominated by all-air chunks, which the chunk-level skip already handles for
// free). It is: OF THE CHUNKS A RAY MUST MARCH — the ones with a non-zero,
// non-full count — what fraction of their blocks are empty? That is the share
// of per-voxel stepping the mask is able to remove, and nothing else is.
void MeasureSubOcc(GpuContext& ctx, World& world) {
  std::vector<uint32_t> occ = ReadOccupancySync(ctx, world);
  std::vector<uint32_t> sub = ReadSubOccSync(ctx, world);
  const uint32_t blocksPerChunk = kSubOccDim * kSubOccDim * kSubOccDim;

  struct Acc {
    uint64_t chunks = 0, blocks = 0, setBlocks = 0, allSet = 0, noneSet = 0;
    uint64_t slabs = 0, emptySlabs = 0;
  };
  Acc mixed, full;  // "mixed" = 1..CHUNK_VOL-1 cells, "full" = all cells
  for (uint32_t i = 0; i < kNumChunks; i++) {
    const uint32_t count = occ[i] & 0xFFFFu;
    if (count == 0) continue;  // chunk-level skip already handles these
    uint32_t set = 0;
    for (uint32_t w = 0; w < kSubOccWords; w++) {
      uint32_t word = sub[(size_t)i * kSubOccStride + w];
      // Bits past the block grid are written as ones by the conservative
      // fills; mask them off or a shift-3 build reads 100% everywhere.
      if (blocksPerChunk < 32 * (w + 1)) {
        const uint32_t used = blocksPerChunk - 32 * w;
        word &= used >= 32 ? 0xFFFFFFFFu : ((1u << used) - 1u);
      }
      for (uint32_t b = 0; b < 32; b++) set += (word >> b) & 1u;
    }
    // ---- the Y-SLAB question ----------------------------------------------
    // Terrain is a HEIGHTFIELD, so the empty part of a surface chunk is not
    // scattered blocks: it is the top of the chunk, all the way across. A
    // horizontal slab (CHUNK x block x CHUNK) that is entirely empty can be
    // jumped in ONE step, and for a near-level ray that step is up to 22
    // voxels of chord instead of a block's 2.7 — which is the difference
    // between a jump that pays for itself and one that does not. Counted here
    // with the SAME bits, so the number is comparable to the block row above.
    for (uint32_t by = 0; by < kSubOccDim; by++) {
      bool any = false;
      for (uint32_t bz = 0; bz < kSubOccDim && !any; bz++)
        for (uint32_t bx = 0; bx < kSubOccDim && !any; bx++) {
          const uint32_t b = (bz * kSubOccDim + by) * kSubOccDim + bx;
          if ((sub[(size_t)i * kSubOccStride + (b >> 5)] >> (b & 31)) & 1u)
            any = true;
        }
      Acc& sa = count >= kChunkVol ? full : mixed;
      sa.slabs++;
      if (!any) sa.emptySlabs++;
    }
    Acc& a = count >= kChunkVol ? full : mixed;
    a.chunks++;
    a.blocks += blocksPerChunk;
    a.setBlocks += set;
    if (set == blocksPerChunk) a.allSet++;
    if (set == 0) a.noneSet++;
  }

  std::printf("\n=== MEASUREMENT 1d: sub-chunk occupancy mask (world.h "
              "kSubOccShift=%u) ===\n", kSubOccShift);
  std::printf("  block grid: %ux%ux%u per chunk, %u voxels per block edge\n",
              kSubOccDim, kSubOccDim, kSubOccDim, 1u << kSubOccShift);
  auto row = [&](const char* label, const Acc& a) {
    std::printf("  %-22s chunks %6" PRIu64 "  blocks set %6.2f%%  "
                "all-set chunks %5.1f%%  empty y-slabs %6.2f%%\n",
                label, a.chunks,
                a.blocks ? 100.0 * (double)a.setBlocks / (double)a.blocks : 0.0,
                a.chunks ? 100.0 * (double)a.allSet / (double)a.chunks : 0.0,
                a.slabs ? 100.0 * (double)a.emptySlabs / (double)a.slabs : 0.0);
  };
  row("partly-filled chunks", mixed);
  row("100%-full chunks", full);
  std::printf("  READ THIS AS THE CEILING: the mask can only remove per-voxel\n"
              "  stepping inside the partly-filled row, and only for the\n"
              "  (100 - blocks set)%% of it that is empty.\n");
}

// Six fullness buckets, matching the brief. Boundaries chosen so "exactly 0"
// and "exactly full" are their OWN buckets: those two are the ones sparse
// binding and a compressed-page scheme care about, and folding them into a
// range would hide the answer.
struct Hist {
  uint64_t zero = 0, lt5 = 0, lt25 = 0, lt75 = 0, lt99 = 0, full = 0;
  void Add(uint32_t count) {
    if (count == 0) { zero++; return; }
    if (count >= kChunkVol) { full++; return; }
    double pct = 100.0 * (double)count / (double)kChunkVol;
    if (pct < 5.0) lt5++;
    else if (pct < 25.0) lt25++;
    else if (pct < 75.0) lt75++;
    else lt99++;
  }
  void Print(const char* label) const {
    uint64_t n = zero + lt5 + lt25 + lt75 + lt99 + full;
    auto row = [&](const char* b, uint64_t v) {
      std::printf("    %-14s %7" PRIu64 "  (%6.2f%%)\n", b, v,
                  n ? 100.0 * (double)v / (double)n : 0.0);
    };
    std::printf("  %s histogram (%" PRIu64 " chunks of %u voxels each):\n",
                label, n, kChunkVol);
    row("exactly 0", zero);
    row("0-5%", lt5);
    row("5-25%", lt25);
    row("25-75%", lt75);
    row("75-99%", lt99);
    row("100% full", full);
  }
};

void MeasureOccupancy(GpuContext& ctx, World& world) {
  std::vector<uint32_t> occ = ReadOccupancySync(ctx, world);

  Hist nonAir, blockers;
  uint64_t totalNonAir = 0, totalBlockers = 0, zeroChunks = 0;
  // Per chunk-Y-layer tallies. Chunk index is slot-linear:
  // (cz * NCHUNK + cy) * NCHUNK + cx  (chunkIndexOf, common.wgsl).
  struct Layer { uint64_t empty = 0, full = 0, mixed = 0, nonAir = 0; };
  std::vector<Layer> layers(kNChunk);

  for (uint32_t i = 0; i < kNumChunks; i++) {
    uint32_t count = occ[i] & 0xFFFFu;
    uint32_t block = occ[i] >> 16;
    nonAir.Add(count);
    blockers.Add(block);
    totalNonAir += count;
    totalBlockers += block;
    if (occ[i] == 0) zeroChunks++;
    uint32_t cy = (i / kNChunk) % kNChunk;
    Layer& L = layers[cy];
    L.nonAir += count;
    if (count == 0) L.empty++;
    else if (count >= kChunkVol) L.full++;
    else L.mixed++;
  }

  const uint64_t totalVox = (uint64_t)kNumChunks * kChunkVol;
  std::printf("\n=== MEASUREMENT 1: occupancy of the %ux%ux%u window ===\n",
              kWorldN, kWorldN, kWorldN);
  std::printf("  window origin (chunks): shown in slot space; the window is\n"
              "  toroidal, so slot Y == world Y here (no vertical scroll).\n");
  std::printf("  chunks: %u (%ux%ux%u of %u^3 voxels)\n", kNumChunks, kNChunk,
              kNChunk, kNChunk, kChunk);
  std::printf("  total voxels:   %" PRIu64 "\n", totalVox);
  std::printf("  non-air voxels: %" PRIu64 "  (%.3f%% full)\n", totalNonAir,
              100.0 * (double)totalNonAir / (double)totalVox);
  std::printf("  ray blockers:   %" PRIu64 "  (%.3f%% of window)\n",
              totalBlockers, 100.0 * (double)totalBlockers / (double)totalVox);
  std::printf("  chunks with occ == 0 (unbindable pages): %" PRIu64
              "  (%.2f%% of chunks)\n",
              zeroChunks, 100.0 * (double)zeroChunks / (double)kNumChunks);
  // What a sparse-resident voxel buffer would actually cost. One chunk of
  // voxel words = kChunkVol * 4 B = 16 KiB, which is a convenient multiple of
  // the 64 KiB Vulkan sparse page — 4 chunks per page — so this is an upper
  // bound on the saving, not a promise.
  const double chunkKiB = (double)kChunkVol * 4.0 / 1024.0;
  std::printf("  voxel buffer: %.1f MiB dense, %.1f MiB if occ==0 chunks were "
              "unbound (%.1f%% saved)\n",
              (double)kNumChunks * chunkKiB / 1024.0,
              (double)(kNumChunks - zeroChunks) * chunkKiB / 1024.0,
              100.0 * (double)zeroChunks / (double)kNumChunks);
  std::printf("  NOTE: one chunk of voxels is %.0f KiB; a Vulkan sparse page is\n"
              "        64 KiB = 4 chunks, so a page is only droppable when all\n"
              "        4 of its chunks are empty. See the page-granular line below.\n",
              chunkKiB);

  // Page-granular estimate: group consecutive chunk indices in fours (the
  // linear order is x-major, so 4 consecutive chunks are a 64-voxel run in X
  // within one chunk row — spatially contiguous, which is the good case).
  uint64_t emptyPages = 0, totalPages = kNumChunks / 4;
  for (uint32_t p = 0; p < totalPages; p++) {
    bool allEmpty = true;
    for (uint32_t k = 0; k < 4; k++)
      if ((occ[p * 4 + k] & 0xFFFFu) != 0) { allEmpty = false; break; }
    if (allEmpty) emptyPages++;
  }
  std::printf("  64 KiB pages fully empty: %" PRIu64 " / %" PRIu64
              "  (%.2f%%) -> %.1f MiB resident of %.1f MiB\n",
              emptyPages, totalPages,
              100.0 * (double)emptyPages / (double)totalPages,
              (double)(totalPages - emptyPages) * 64.0 / 1024.0,
              (double)totalPages * 64.0 / 1024.0);

  std::printf("\n");
  nonAir.Print("non-air");
  std::printf("\n");
  blockers.Print("ray-blocker");

  std::printf("\n  per chunk-Y layer (each layer = %ux%u = %u chunks, "
              "world y %u..%u per layer):\n",
              kNChunk, kNChunk, kNChunk * kNChunk, 0u, kChunk - 1);
  std::printf("    %-10s %8s %8s %8s   %s\n", "chunkY", "empty", "full",
              "mixed", "avg fill");
  for (uint32_t cy = 0; cy < kNChunk; cy++) {
    const Layer& L = layers[cy];
    double avg = 100.0 * (double)L.nonAir /
                 ((double)kNChunk * kNChunk * kChunkVol);
    std::printf("    y%-3u %3u-%-3u %8" PRIu64 " %8" PRIu64 " %8" PRIu64
                "   %6.2f%%\n",
                cy, cy * kChunk, (cy + 1) * kChunk - 1, L.empty, L.full,
                L.mixed, avg);
  }

  // Fully-solid underground vs fully-air sky, inferred from the layer split:
  // a layer whose chunks are all 100% full is underground, one whose chunks
  // are all empty is sky. Everything between is the surface band.
  uint64_t skyChunks = 0, solidChunks = 0;
  for (uint32_t cy = 0; cy < kNChunk; cy++) {
    if (layers[cy].mixed == 0 && layers[cy].full == 0) skyChunks += layers[cy].empty;
    if (layers[cy].mixed == 0 && layers[cy].empty == 0) solidChunks += layers[cy].full;
  }
  std::printf("\n  layers that are ENTIRELY empty (pure sky): %" PRIu64
              " chunks; ENTIRELY full (pure rock): %" PRIu64 " chunks\n",
              skyChunks, solidChunks);
  std::printf("  everything else is the surface band / partially-filled "
              "interior.\n");
}

// ---------------------------------------------------------------------------
// MEASUREMENT 1b: the UNIFORMITY histogram (PLAN_page_table.md §3.6, commit 0).
//
// The occupancy buffer above answers "how many chunks are all-air" — the EMPTY
// sentinel's payoff — but it packs only two 16-bit counts and so cannot answer
// the question UNIFORM's scope turns on: of the chunks that are NOT all-air,
// how many are a single repeated WORD? A UNIFORM sentinel carries 12 bits of
// material and nothing else, so promotion is by whole-word equality, never by
// material equality (§2.3): worldgen assigns a `rnd % 3` palette variant to
// each cell's state nibble, which is enough to make a chunk of one material
// not be a chunk of one word.
//
// That distinction is exactly what this histogram measures, in four buckets:
//
//   all-air              -> EMPTY. Free, already counted by the occupancy pass.
//   all-one-WORD         -> UNIFORM is representable. THE number that decides
//                           whether tick-path uniformity discovery is worth a
//                           later commit.
//   all-one-MATERIAL     -> one material, differing state/stain bits. UNIFORM
//     (mixed state)         canNOT represent these without widening the entry.
//   mixed                -> a real page, always.
//
// DENSE RESIDENCY ONLY (§5.4, review m5). This does a blocking whole-buffer
// read of `voxels` at slot*16 KiB — the raw-slot-offset shape §2.1a is about —
// so it is ground truth only under the identity page map. It exists to SIZE
// the pool, which wants the dense truth anyway; making it page-aware would
// mean synthesizing sentinel chunks in order to count them, which is circular.
// The paged side is covered by pagesInUse_/pagesHighWater_ reporting.
struct UniformHist {
  uint64_t air = 0;        // every word == 0
  uint64_t oneWord = 0;    // every word identical, non-air
  uint64_t oneMat = 0;     // one material, >1 distinct word
  uint64_t mixed = 0;      // >1 material
};

void MeasureUniformity(GpuContext& ctx, World& world) {
  std::printf("\n=== MEASUREMENT 1b: chunk uniformity (page-table sentinels) ==="
              "\n");
  std::printf("  *** DENSE RESIDENCY ONLY: this reads `voxels` at slot*%u B,\n"
              "      which is ground truth only under the identity page map.\n"
              "      It sizes the pool; it does not describe a paged run.\n",
              kChunkVol * 4);

  // Read the 512 MiB buffer in slot batches rather than one allocation: the
  // staging buffer for a whole-buffer copy would itself be 512 MiB of host
  // memory, and nothing here needs more than one chunk at a time to classify.
  const uint32_t kBatchChunks = 256;                 // 4 MiB per readback
  const size_t kBatchWords = (size_t)kBatchChunks * kChunkVol;
  std::vector<uint32_t> batch(kBatchWords, 0);

  UniformHist h;
  // Which materials the one-word chunks are made of — the payoff is per
  // material, and "3,000 chunks of stone" is a different finding from "3,000
  // chunks spread over 20 materials".
  std::unordered_map<uint32_t, uint64_t> oneWordMats;

  for (uint32_t base = 0; base < kNumChunks; base += kBatchChunks) {
    const uint32_t n = std::min(kBatchChunks, kNumChunks - base);
    if (!rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels,
                               (uint64_t)base * kChunkVol * 4, batch.data(),
                               (size_t)n * kChunkVol * 4, "uniformityRead")) {
      std::printf("  READBACK FAILED at chunk %u — histogram abandoned\n", base);
      return;
    }
    for (uint32_t c = 0; c < n; c++) {
      const uint32_t* w = batch.data() + (size_t)c * kChunkVol;
      const uint32_t w0 = w[0];
      bool sameWord = true, sameMat = true;
      const uint32_t m0 = w0 & 0xFFFu;
      for (uint32_t i = 1; i < kChunkVol; i++) {
        if (w[i] != w0) {
          sameWord = false;
          if ((w[i] & 0xFFFu) != m0) { sameMat = false; break; }
        }
      }
      if (sameWord) {
        if (w0 == 0u) { h.air++; }
        else { h.oneWord++; oneWordMats[m0]++; }
      } else if (sameMat) {
        h.oneMat++;
      } else {
        h.mixed++;
      }
    }
  }

  const uint64_t n = h.air + h.oneWord + h.oneMat + h.mixed;
  auto row = [&](const char* label, uint64_t v, const char* note) {
    std::printf("    %-26s %7" PRIu64 "  (%6.2f%%)  %s\n", label, v,
                n ? 100.0 * (double)v / (double)n : 0.0, note);
  };
  std::printf("\n  %" PRIu64 " chunks of %u voxels:\n", n, kChunkVol);
  row("all-air (EMPTY)", h.air, "sentinel, 4 B");
  row("all-one-WORD (UNIFORM)", h.oneWord, "sentinel, 4 B");
  row("all-one-MATERIAL, mixed", h.oneMat, "needs a page (state/stain differ)");
  row("mixed material", h.mixed, "needs a page");

  const double pageKiB = (double)kChunkVol * 4.0 / 1024.0;
  const uint64_t sentinels = h.air + h.oneWord;
  std::printf("\n  resident pages if EMPTY only:            %7" PRIu64
              "  (%.1f MiB)\n",
              n - h.air, (double)(n - h.air) * pageKiB / 1024.0);
  std::printf("  resident pages if EMPTY + UNIFORM:       %7" PRIu64
              "  (%.1f MiB)\n",
              n - sentinels, (double)(n - sentinels) * pageKiB / 1024.0);
  std::printf("  UNIFORM's marginal saving over EMPTY:    %7" PRIu64
              " chunks (%.1f MiB)\n",
              h.oneWord, (double)h.oneWord * pageKiB / 1024.0);

  if (!oneWordMats.empty()) {
    std::vector<std::pair<uint32_t, uint64_t>> mats(oneWordMats.begin(),
                                                    oneWordMats.end());
    std::sort(mats.begin(), mats.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::printf("\n  one-word chunks by material (word = mat|state<<12|...):\n");
    for (size_t i = 0; i < mats.size() && i < 8; i++)
      std::printf("    material %4u  %7" PRIu64 " chunks\n", mats[i].first,
                  mats[i].second);
  }

  // The decision rule, stated in the output so the number is not read without
  // it (PLAN_page_table.md §3.6 / §9 open question 2).
  std::printf("\n  DECISION RULE (§3.6): tick-path uniformity DISCOVERY is\n"
              "  worth a later commit only if all-one-WORD is a material\n"
              "  fraction of the non-air chunks. Streaming/load-path demotion\n"
              "  lands regardless — those paths already have the words in hand.\n");
}

// ---------------------------------------------------------------------------

struct Scenario {
  const char* name;
  uint32_t ticks;
};

// ---------------------------------------------------------------------------
// MEASUREMENT 3: PER-COLOUR-PHASE OCCUPANCY OF THE DIRTY SET.
//
// The question this exists to answer (ROADMAP_scale.md §3.0's open end): the
// CA's 251 µs/tick floor is 54 RECORDED dispatches, and the only lever on it
// is recording fewer. §3.4 already skips all 54 when the whole dirty set is
// empty. Could a per-COLOUR version skip iteration k when no dirty chunk holds
// a cell of colour k that can act?
//
// This measures the CEILING on that idea, and the ceiling only. For each
// sampled tick it reads the dirty flags and the voxels of every dirty chunk,
// and asks, per colour c in 0..26:
//
//   nonAir[c]  — any non-air cell of colour c anywhere in the dirty set?
//                (the loosest conceivable skip predicate: even an oracle that
//                 knew the future could not skip a phase that has matter it
//                 might have to move.)
//   canAct[c]  — any cell of colour c passing matCanAct() (common.wgsl:118),
//                i.e. not a plain inert solid? This is the predicate sim_step
//                itself uses to return early, and it is the tightest predicate
//                a CHEAP per-phase mask could evaluate.
//
// Both are measured on FRESH, UNDILATED state — strictly better than anything
// an implementable mechanism could use, since a CPU-side latch (which is what
// §3.4 is, and what a per-phase version would have to be, because the 4.65 µs
// is paid at RECORD time on the CPU) can only ever see a readback that is at
// least one tick stale and must then be dilated for movement. So:
//
//     empty-phase fraction measured here  >=  any achievable win.
//
// Colour encoding matches simulation.cpp's passUBO fill exactly:
//   c = (Z%3)*9 + (Y%3)*3 + (X%3), in WORLD cell coords (the lattice is
//   global — CLAUDE.md's first critical invariant).
struct PhaseHist {
  uint64_t ticks = 0;          // sampled ticks with a non-empty dirty set
  uint64_t settledTicks = 0;   // sampled ticks with an EMPTY dirty set
  uint64_t emptyCanActSum = 0; // sum over ticks of #{c : canAct[c] == 0}
  uint64_t emptyNonAirSum = 0; // sum over ticks of #{c : nonAir[c] == 0}
  uint32_t minEmptyCanAct = 27, maxEmptyCanAct = 0;
  uint64_t histEmptyCanAct[28] = {};  // how many ticks had exactly k empty
  // Per-CHUNK diagnostic: how many of the 27 colours does ONE dirty chunk
  // hold actionable cells of? This is the number that explains the result —
  // if it saturates at 27 the whole idea is dead no matter how few chunks are
  // dirty, because the union over chunks can only grow.
  uint64_t chunks = 0;
  uint64_t chunkColoursSum = 0;
  uint32_t chunkColoursMin = 28, chunkColoursMax = 0;
  uint64_t chunkAllAir = 0;  // dirty chunks that are entirely air (the §3.1
                             // bug-2 case: dilated in by a neighbour)

  // ---- what an IMPLEMENTABLE mechanism could prove --------------------------
  // The 4.65/2.27 µs is paid when the dispatch is RECORDED, on the CPU, so a
  // per-phase skip has to be a CPU latch like §3.4's — and the CPU's only view
  // of colour occupancy is a readback that is at least one tick old. Between
  // that readback and the tick being recorded, cells MOVE, so last tick's
  // occupied-colour set must be DILATED by every offset the CA can write to
  // before it can be trusted.
  //
  // The exact write-target set of sim_step.wgsl (self + tryMove + transferLiquid
  // + doReactions' faceDir products) is 15 of the 27 offsets:
  //     (0,0,0); the 6 faces; the 8 vertical diagonals (±1,±1,0)/(0,±1,±1).
  // The 4 horizontal diagonals and the 8 corners are unreachable in one tick.
  // Modelled at ONE tick of lag, which is the most optimistic lag the engine
  // has. `staleUnsafe` counts ticks where the dilated set FAILED to cover the
  // true set — it must stay 0 or the offset model above is wrong.
  //
  // Measured ONLY on ticks whose dirty set is non-empty, because a tick with an
  // empty dirty set is already handled in full by §3.4 — counting those would
  // credit this idea with the settled skip's existing win.
  //
  // TWO models, and the difference between them is the whole answer:
  //   `staleEmptySum`     — colour dilation only.
  //   `staleSoundSum`     — colour dilation AND the newly-dirtied-chunk
  //                         fallback. `markDirty` wakes every chunk a written
  //                         cell BORDERS, and a chunk entering the dirty list
  //                         brings its ENTIRE contents' colour set with it —
  //                         terrain that was nowhere in last tick's set. Last
  //                         tick's colours therefore say nothing about it, so
  //                         a correct latch must fall back to all-27 on any
  //                         tick where the dirty set GREW. This is ROADMAP
  //                         §3.1's bug 2, restated at colour granularity.
  uint64_t staleTicks = 0, staleEmptySum = 0, staleUnsafe = 0;
  uint64_t staleInputTicks = 0, staleSoundSum = 0, staleGrewTicks = 0;
  uint32_t staleEmptyMax = 0;
  bool prevValid = false;
  bool prevAct[27] = {};
  std::vector<uint8_t> prevDirty;

  void Print(const char* label) const {
    const uint64_t n = ticks + settledTicks;
    std::printf("\n  --- MEASUREMENT 3: colour-phase occupancy — %s ---\n", label);
    std::printf("    sampled ticks: %" PRIu64 " (%" PRIu64 " with a non-empty "
                "dirty set, %" PRIu64 " settled)\n", n, ticks, settledTicks);
    if (ticks == 0) {
      std::printf("    no active ticks — nothing to say about phase skipping\n");
      return;
    }
    std::printf("    dirty chunks sampled: %" PRIu64 "; of those %" PRIu64
                " (%.1f%%) are ALL AIR\n", chunks, chunkAllAir,
                chunks ? 100.0 * (double)chunkAllAir / (double)chunks : 0.0);
    std::printf("    colours with actionable cells PER DIRTY CHUNK: "
                "%.2f avg (min %u, max %u) of 27\n",
                chunks ? (double)chunkColoursSum / (double)chunks : 0.0,
                chunkColoursMin == 28 ? 0 : chunkColoursMin, chunkColoursMax);
    std::printf("    EMPTY colour phases per ACTIVE tick (union over the whole "
                "dirty set):\n");
    std::printf("      by matCanAct : %.3f / 27  (%.2f%%)   min %u  max %u\n",
                (double)emptyCanActSum / (double)ticks,
                100.0 * (double)emptyCanActSum / (double)ticks / 27.0,
                minEmptyCanAct == 27 && maxEmptyCanAct == 0 ? 0 : minEmptyCanAct,
                maxEmptyCanAct);
    std::printf("      by non-air   : %.3f / 27  (%.2f%%)\n",
                (double)emptyNonAirSum / (double)ticks,
                100.0 * (double)emptyNonAirSum / (double)ticks / 27.0);
    std::printf("      distribution (empty-phase count -> ticks):");
    bool any = false;
    for (uint32_t k = 0; k <= 27; k++)
      if (histEmptyCanAct[k]) {
        std::printf(" %u:%" PRIu64, k, histEmptyCanAct[k]);
        any = true;
      }
    if (!any) std::printf(" (none)");
    std::printf("\n");
    std::printf("    what a ONE-TICK-STALE CPU latch could prove empty, over "
                "the %" PRIu64 " ticks\n    that DISPATCH work (i.e. "
                "incremental over §3.4's settled skip):\n", staleTicks);
    std::printf("      movement dilation only : %.3f / 27  (%.2f%%)  max %u   "
                "[VIOLATIONS: %" PRIu64 "]\n",
                staleTicks ? (double)staleEmptySum / (double)staleTicks : 0.0,
                staleTicks ? 100.0 * (double)staleEmptySum / (double)staleTicks / 27.0
                           : 0.0,
                staleEmptyMax, staleUnsafe);
    std::printf("      + newly-dirtied-chunk  : %.3f / 27  (%.2f%%)   <- THE "
                "SOUND MODEL\n",
                staleTicks ? (double)staleSoundSum / (double)staleTicks : 0.0,
                staleTicks ? 100.0 * (double)staleSoundSum / (double)staleTicks / 27.0
                           : 0.0);
    std::printf("      surrendered: %" PRIu64 " ticks to the op/particle "
                "fallback, %" PRIu64 " to dirty-set growth\n",
                staleInputTicks, staleGrewTicks);
  }
};

// One sample. Blocking readbacks; measurement harness only.
void SamplePhaseOccupancy(GpuContext& ctx, World& world, Simulation& sim,
                          const std::vector<MaterialDef>& mats, PhaseHist& h,
                          bool inputsThisTick) {
  static std::vector<uint32_t> flags;
  static std::vector<uint32_t> chunk;
  flags.assign(kNumChunks, 0);
  chunk.resize(kChunkVol);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                        flags.data(), (size_t)kNumChunks * 4, "phaseDirtyRead");

  // matCanAct, on the CPU, from the same compiled table the GPU is handed.
  auto canAct = [&mats](uint32_t mat) {
    if (mat >= mats.size()) return true;  // unknown id: assume it acts
    const MaterialGpu& g = mats[mat].gpu;
    return g.klass != CLASS_SOLID || g.reactCount > 0u || (g.stainPack & 0x7u) != 0u;
  };

  bool nonAir[27] = {}, act[27] = {};
  uint64_t dirty = 0;
  bool grew = false;  // did a chunk ENTER the dirty set since the last sample?
  if (h.prevDirty.size() != kNumChunks) { h.prevDirty.assign(kNumChunks, 0); grew = true; }
  for (uint32_t slot = 0; slot < kNumChunks; slot++) {
    if (flags[slot] == 0) continue;
    if (!h.prevDirty[slot]) grew = true;
    dirty++;
    ReadVoxelsSync(ctx, world, slot, 1, chunk.data(), "phaseVoxRead");
    const IVec3 wc = world.SlotToWorldChunk(slot);
    const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
              bz = wc.z * (int)kChunk;
    // (X%3) for a whole chunk row is just (bx + lx) % 3; precompute the base
    // residues once so the inner loop is three table lookups.
    auto res = [](int b) { return ((b % 3) + 3) % 3; };
    const int rx0 = res(bx), ry0 = res(by), rz0 = res(bz);
    bool chunkAct[27] = {};
    bool anyNonAir = false;
    for (uint32_t k = 0; k < kChunkVol; k++) {
      const uint32_t w = chunk[k];
      const uint32_t mat = w & 0xFFFu;
      if (mat == 0u) continue;  // MAT_AIR
      anyNonAir = true;
      const uint32_t lx = k % kChunk, ly = (k / kChunk) % kChunk,
                     lz = k / (kChunk * kChunk);
      const uint32_t c = (uint32_t)((rz0 + (int)lz) % 3) * 9u +
                         (uint32_t)((ry0 + (int)ly) % 3) * 3u +
                         (uint32_t)((rx0 + (int)lx) % 3);
      nonAir[c] = true;
      if (canAct(mat)) { act[c] = true; chunkAct[c] = true; }
    }
    uint32_t nc = 0;
    for (uint32_t c = 0; c < 27; c++) nc += chunkAct[c] ? 1u : 0u;
    h.chunks++;
    h.chunkColoursSum += nc;
    h.chunkColoursMin = std::min(h.chunkColoursMin, nc);
    h.chunkColoursMax = std::max(h.chunkColoursMax, nc);
    if (!anyNonAir) h.chunkAllAir++;
  }

  // The stale-latch model, evaluated against the truth we just computed.
  // `inputsThisTick` is §3.4's own disjunction: a tick carrying mutation ops,
  // explosions, spawns or live particles can put matter at ANY colour, so a
  // correct latch must fall back to "all 27 active" exactly as the settled
  // skip does. Without this fallback the model MIS-PREDICTS (staleUnsafe > 0),
  // which in a real implementation is a wrong world hash.
  //
  // Counted only where it can pay: `dirty != 0` means the CA is dispatching
  // real work this tick, so §3.4's whole-set skip cannot fire and anything
  // proved here is INCREMENTAL. (Crediting the empty-dirty-set ticks would be
  // double-counting the settled skip, which already takes all 54.)
  if (h.prevValid && dirty != 0 && inputsThisTick) {
    h.staleTicks++;
    h.staleInputTicks++;
    if (grew) h.staleGrewTicks++;
  } else if (h.prevValid && dirty != 0) {
    static const int kOff[15][3] = {
        {0, 0, 0},  {1, 0, 0},  {-1, 0, 0}, {0, 1, 0},  {0, -1, 0},
        {0, 0, 1},  {0, 0, -1}, {1, 1, 0},  {-1, 1, 0}, {1, -1, 0},
        {-1, -1, 0}, {0, 1, 1}, {0, 1, -1}, {0, -1, 1}, {0, -1, -1}};
    bool dil[27] = {};
    for (uint32_t c = 0; c < 27; c++) {
      if (!h.prevAct[c]) continue;
      const int cx = (int)(c % 3), cy = (int)((c / 3) % 3), cz = (int)(c / 9);
      for (const auto& o : kOff)
        dil[(uint32_t)(((cz + o[2] + 3) % 3) * 9 + ((cy + o[1] + 3) % 3) * 3 +
                       ((cx + o[0] + 3) % 3))] = true;
    }
    uint32_t se = 0;
    bool unsafe = false;
    for (uint32_t c = 0; c < 27; c++) {
      if (!dil[c]) se++;
      if (act[c] && !dil[c]) unsafe = true;  // the model missed real work
    }
    h.staleTicks++;
    h.staleEmptySum += se;
    h.staleEmptyMax = std::max(h.staleEmptyMax, se);
    if (unsafe) h.staleUnsafe++;
    // The SOUND model additionally surrenders every tick on which a chunk
    // entered the dirty set — see the header comment: a new chunk's terrain is
    // at colours last tick's set never mentioned.
    if (grew) h.staleGrewTicks++;
    else h.staleSoundSum += se;
  }
  std::memcpy(h.prevAct, act, sizeof(act));
  for (uint32_t i = 0; i < kNumChunks; i++) h.prevDirty[i] = flags[i] ? 1u : 0u;
  h.prevValid = true;

  if (dirty == 0) { h.settledTicks++; return; }
  uint32_t emptyAct = 0, emptyNon = 0;
  for (uint32_t c = 0; c < 27; c++) {
    if (!act[c]) emptyAct++;
    if (!nonAir[c]) emptyNon++;
  }
  h.ticks++;
  h.emptyCanActSum += emptyAct;
  h.emptyNonAirSum += emptyNon;
  h.minEmptyCanAct = std::min(h.minEmptyCanAct, emptyAct);
  h.maxEmptyCanAct = std::max(h.maxEmptyCanAct, emptyAct);
  h.histEmptyCanAct[emptyAct]++;
}

void PrintPassTable(const char* label, const PassTimer& timer, uint32_t ticks,
                    double wallMsPerTick, uint32_t passesPerTick,
                    uint64_t caSkips, double avgActive, uint32_t maxActive) {
  std::printf("\n  --- %s (%u ticks, %u ComputePassEncoders/tick) ---\n", label,
              ticks, passesPerTick);
  // ROADMAP_scale.md §3.4: how many of these ticks recorded NO CA rows at all.
  // Reported unconditionally, including the 0 case, because "the skip did not
  // fire" is the thing a reader most needs to know when a settled number looks
  // higher than expected — a silent 0 reads as "no such mechanism".
  std::printf("    CA skipped on %" PRIu64 " / %u ticks (settled-tick skip)\n",
              caSkips, ticks);
  // §5.4: active chunks, and the per-chunk cost derived from THIS scenario's
  // own CA time rather than carried over from another one. The CA row is 54
  // dispatches over the same chunk list, so µs/chunk/tick divided by 54 is the
  // per-chunk-per-PASS figure; the budget in §2 is quoted per tick, so that is
  // what is printed.
  double caUs = 0;
  for (const PassTimer::Stat& s : timer.Stats())
    if (s.name.find("ca(") == 0) caUs = (double)s.totalNs / 1000.0 / (double)ticks;
  std::printf("    active chunks/tick: %.1f avg, %u max", avgActive, maxActive);
  if (avgActive > 0.5 && caUs > 0)
    std::printf("   -> CA %.3f us/chunk/tick", caUs / avgActive);
  std::printf("\n");
  std::printf("    %-34s %10s %10s %8s\n", "pass", "us/tick", "% of sim",
              "ticks");
  double sum = 0;
  for (const PassTimer::Stat& s : timer.Stats())
    sum += (double)s.totalNs;
  for (const PassTimer::Stat& s : timer.Stats()) {
    double usPerTick = (double)s.totalNs / 1000.0 / (double)ticks;
    std::printf("    %-34s %10.3f %9.1f%% %8" PRIu64 "\n", s.name.c_str(),
                usPerTick, sum > 0 ? 100.0 * (double)s.totalNs / sum : 0.0,
                s.frames);
  }
  std::printf("    %-34s %10.3f %9.1f%%\n", "TOTAL (GPU compute)",
              sum / 1000.0 / (double)ticks, sum > 0 ? 100.0 : 0.0);
  std::printf("    %-34s %10.3f  (submit + WaitIdle, includes CPU encode)\n",
              "wall clock", wallMsPerTick * 1000.0);
}

// Run `ticks` ticks with the timer attached, collecting after each submit.
// Collect() blocks, which serialises the GPU — so the wall-clock number here
// is a per-tick LATENCY, not throughput. Reported as such.
double RunTimedTicks(GpuContext& ctx, World& world, Simulation& sim,
                     PassTimer& timer, uint32_t firstTick, uint32_t ticks,
                     bool withExplosions, uint32_t* passesPerTick,
                     uint64_t* caSkips, double* avgActive, uint32_t* maxActive,
                     uint32_t heavyEvery = 0, int heavyRadius = 0,
                     PhaseHist* phase = nullptr,
                     const std::vector<MaterialDef>* mats = nullptr) {
  double t0 = NowSeconds();
  uint32_t passes = 0;
  const uint64_t skips0 = sim.CaSkipCount();
  // ROADMAP_scale.md §5.4: the active-chunk count is the unit the whole compute
  // budget is denominated in ("~30-50k active chunks/tick"), and it was being
  // extrapolated from a single settling measurement. Sampling it per scenario
  // costs nothing — the snapshot already carries it — and it is what turns the
  // 0.2 µs/chunk anchor into something checkable.
  uint64_t activeSum = 0;
  uint32_t activeMax = 0, activeSamples = 0;
  for (uint32_t i = 0; i < ticks; i++) {
    uint32_t t = firstTick + i;
    std::vector<ExplosionOp> exps;
    if (withExplosions && (t % 20) == 0) {
      // Cheapest available "make the world active" lever: the same explosion
      // op the selftest gates use. Reused rather than re-invented so the
      // active-scenario numbers describe work the engine really does.
      int h = World::TerrainHeight(100, 100, kDefaultSeed);
      exps.push_back({100, h, 100, 14, 400, 0, 0, 0});
    }
    // ROADMAP_scale.md §5.4: a THIRD point on the cost curve, far from the
    // other two. The active/settling scenarios sit at ~3 and ~25 active chunks,
    // which are close enough together that a two-point fit of
    // "cost = fixed + k*chunks" is badly conditioned — and that fit is what the
    // whole compute budget in §2 rests on. Walking the blast around the world
    // keeps successive explosions from landing in the crater the last one made.
    if (heavyEvery > 0 && (t % heavyEvery) == 0) {
      const int gx = 80 + (int)((t / heavyEvery * 37u) % 300u);
      const int gz = 80 + (int)((t / heavyEvery * 53u) % 300u);
      int h = World::TerrainHeight(gx, gz, kDefaultSeed);
      exps.push_back({gx, h, gz, heavyRadius, 400, 0, 0, 0});
    }
    // wantReadback = TRUE. It used to be false, which made this harness cheaper
    // per tick but also made it measure a configuration the game never runs:
    // the §3.4 settled skip is licensed by an arriving SNAPSHOT, and a harness
    // that never asks for one can never take the skip. The settled numbers were
    // therefore reporting the un-skipped CA cost while the game was already
    // paying nothing — a measurement that flatters the OLD code. The game and
    // every gate pass true here, so this makes --measure agree with them.
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, exps, {},
               t % 15 == 0, {8, 3, 8}, true, withExplosions);
    if (i == 0) passes = timer.PassesThisBuffer() / 2;
    ctx.WaitIdle();
    timer.Collect(ctx);
    // SetHarnessSnapshotDrain(true) is on for --measure, so the snapshot this
    // reads is the one this tick produced rather than an arbitrarily old one.
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) {
      activeSum += sn.activeChunks;
      activeMax = std::max(activeMax, sn.activeChunks);
      activeSamples++;
    }
    // MEASUREMENT 3. Sampled AFTER the tick and its WaitIdle, so the dirty
    // flags read are exactly the set the NEXT tick's CA will dispatch over —
    // which is the set a per-phase skip decision would have to be made about.
    // The blocking reads here dilate wall clock; they cannot touch the GPU
    // pass timings, which is what the model is fitted to.
    if (phase && mats)
      SamplePhaseOccupancy(ctx, world, sim, *mats, *phase,
                           !exps.empty() || withExplosions);
  }
  double dt = NowSeconds() - t0;
  if (passesPerTick) *passesPerTick = passes;
  if (caSkips) *caSkips = sim.CaSkipCount() - skips0;
  if (avgActive)
    *avgActive = activeSamples ? (double)activeSum / (double)activeSamples : 0.0;
  if (maxActive) *maxActive = activeMax;
  return dt / (double)ticks;
}

}  // namespace

int RunMeasure(GpuContext& ctx, World& world, Simulation& sim,
               const std::vector<MaterialDef>& mats) {
  SetHarnessSnapshotDrain(true);  // see test/support.h
  std::printf("=== sandvox --measure: Vulkan-port sizing ===\n");
  std::printf("timestamp queries: %s\n",
              ctx.timestampsEnabled ? "SUPPORTED (GPU pass timings below)"
                                    : "UNSUPPORTED (wall-clock fallback)");

  // ---- world setup: default seed, default window, settle ----
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Settle. 300 ticks is comfortably past what the selftest needs for the
  // powders to come to rest, and covers the streaming/reaction transients.
  const uint32_t kSettleTicks = 300;
  std::printf("settling %u ticks after worldgen...\n", kSettleTicks);
  for (uint32_t t = 1; t <= kSettleTicks; t++)
    SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, t % 15 == 0,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();

  // Occupancy is only refreshed over the WHOLE world on a hash tick; between
  // them it is updated incrementally over the dirty list. A settled world has
  // an empty dirty list, so the incremental path touches nothing and a stale
  // read is a real hazard. Force one full-world pass before reading.
  {
    uint32_t hash = HashWorldNow(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::printf("settled. world hash = %08x\n", hash);
  }
  MeasureOccupancy(ctx, world);
  MeasureSubOcc(ctx, world);
  MeasureUniformity(ctx, world);

  // ---- resident memory, the phase-7 acceptance number (§3.7) --------------
  // Two numbers, and conflating them is how a phase claims a win it did not
  // get: pagesInUse_ * 16 KiB is RESIDENT CONTENT and is what compares to the
  // 77.7 MiB measurement; the pool reservation is RESERVED VRAM and is what
  // kPoolPages costs whether or not it is used.
  {
    const PageTable& pt = *world.pages;
    const double pageMiB = (double)kChunkVol * 4.0 / 1048576.0;
    std::printf("\n=== MEASUREMENT 1c: page residency ===\n");
    std::printf("  mode:                 %s\n",
                world.residency == World::Residency::Paged ? "paged" : "dense");
    std::printf("  pages in use:         %7u  (%.1f MiB resident)\n",
                pt.PagesInUse(), (double)pt.PagesInUse() * pageMiB);
    std::printf("  high water:           %7u  (%.1f MiB)\n",
                pt.PagesHighWater(), (double)pt.PagesHighWater() * pageMiB);
    std::printf("  pool reservation:     %7u  (%.1f MiB) <- kPoolPages\n",
                pt.PoolPages(), (double)pt.PoolPages() * pageMiB);
    std::printf("  dense equivalent:     %7u  (%.1f MiB)\n", kNumChunks,
                (double)kNumChunks * pageMiB);
    std::printf("  page fills issued:    %7llu   pages freed: %llu\n",
                (unsigned long long)pt.FillsIssued(),
                (unsigned long long)pt.PagesFreed());
    std::printf("  compare resident against 77.7 MiB measured / 86.9 MiB "
                "estimated (§3.7).\n");
  }

  uint32_t active = ReadActiveChunksSync(ctx, world, sim);
  std::printf("\n  active (dirty) chunks after settling: %u / %u\n", active,
              kNumChunks);

  // ---- MEASUREMENT 2: per-pass GPU timing ----
  std::printf("\n=== MEASUREMENT 2: per-pass GPU time ===\n");
  PassTimer timer;
  bool haveTimer = timer.Init(ctx, 32);
  if (!haveTimer) {
    std::printf("  TimestampQuery unavailable — reporting WALL-CLOCK per tick "
                "only (coarse).\n");
  } else {
    sim.SetPassTimer(&timer);
  }
  std::printf("  Granularity note: timings are per ComputePassEncoder, which "
              "is\n  what the encoder structure gives. Several dispatches share "
              "one pass\n  (the CA pass is 54 indirect dispatches; prep is "
              "mutate+explode+compact),\n  so those numbers are aggregates.\n");

  const uint32_t kPerScenario = 120;

  // MEASUREMENT 3 accumulators, one per scenario. Opt-in via SANDVOX_PHASE_HIST=1
  // because the per-tick blocking voxel reads it needs cost far more wall clock
  // than the tick does — it must not silently slow the default sizing run.
  const bool phaseHistOn = [] {
    const char* e = std::getenv("SANDVOX_PHASE_HIST");
    return e && e[0] == '1';
  }();
  PhaseHist phSettled, phActive, phSettling, phHeavy;
  const std::vector<MaterialDef>* pm = phaseHistOn ? &mats : nullptr;

  // (c) SETTLED — measured first, because it is the state we are already in
  // and running the active scenario would destroy it.
  {
    timer.ResetStats();
    uint32_t ppt = 0;
    uint64_t skips = 0;
    double avgAct = 0; uint32_t maxAct = 0;
    double wall = RunTimedTicks(ctx, world, sim, timer, kSettleTicks + 1,
                                kPerScenario, false, &ppt, &skips, &avgAct,
                                &maxAct, 0, 0, phaseHistOn ? &phSettled : nullptr, pm);
    PrintPassTable("(c) SETTLED world (no edits, chunks asleep)", timer,
                   kPerScenario, wall, ppt, skips, avgAct, maxAct);
  }

  // (b) ACTIVE — periodic explosions keep chunks awake and particles flying.
  {
    timer.ResetStats();
    uint32_t ppt = 0;
    uint64_t skips = 0;
    double avgAct = 0; uint32_t maxAct = 0;
    double wall = RunTimedTicks(ctx, world, sim, timer,
                                kSettleTicks + kPerScenario + 1, kPerScenario,
                                true, &ppt, &skips, &avgAct, &maxAct, 0, 0,
                                phaseHistOn ? &phActive : nullptr, pm);
    PrintPassTable("(b) ACTIVE world (explosion every 20 ticks + particles)",
                   timer, kPerScenario, wall, ppt, skips, avgAct, maxAct);
  }

  // (a) SETTLING — regenerate the world and time the first ticks, when
  // everything that is going to fall is still falling.
  {
    // Detach for the regen: only EncodeTick encodes the query RESOLVE, so a
    // timed worldgen submit leaves its two queries dangling and the next
    // Collect() attributes them to this scenario — which is how a 4.7 ms
    // "worldgen" row turned up inside a per-tick table on the first run.
    sim.SetPassTimer(nullptr);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    if (haveTimer) sim.SetPassTimer(&timer);
    timer.ResetStats();
    uint32_t ppt = 0;
    uint64_t skips = 0;
    double avgAct = 0; uint32_t maxAct = 0;
    double wall = RunTimedTicks(ctx, world, sim, timer, 1, kPerScenario, false,
                                &ppt, &skips, &avgAct, &maxAct, 0, 0,
                                phaseHistOn ? &phSettling : nullptr, pm);
    PrintPassTable("(a) SETTLING world (first ticks after worldgen)", timer,
                   kPerScenario, wall, ppt, skips, avgAct, maxAct);
  }

  // (d) HEAVY — the third point on the cost curve (§5.4). A large blast every
  // 4 ticks, walked around the world so each one lands on intact terrain.
  // This is the only scenario that puts the CA in the regime the 5 cm / 2048³
  // plan actually cares about, where the per-chunk term dominates the fixed
  // per-dispatch floor.
  {
    sim.SetPassTimer(nullptr);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 200; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false,
                 {8, 3, 8}, true, false);
    ctx.WaitIdle();
    if (haveTimer) sim.SetPassTimer(&timer);
    timer.ResetStats();
    uint32_t ppt = 0;
    uint64_t skips = 0;
    double avgAct = 0; uint32_t maxAct = 0;
    double wall = RunTimedTicks(ctx, world, sim, timer, 201, kPerScenario, false,
                                &ppt, &skips, &avgAct, &maxAct, 4, 30,
                                phaseHistOn ? &phHeavy : nullptr, pm);
    PrintPassTable("(d) HEAVY world (r=30 blast every 4 ticks, walked)", timer,
                   kPerScenario, wall, ppt, skips, avgAct, maxAct);
  }

  // (e) MINIMAL — the SMALLEST event the engine can have, and therefore the
  // best case the per-phase-skip hypothesis will ever get. ONE powder voxel
  // released 40 cells above the terrain, every 40 ticks, into empty sky: a
  // single acting cell, a dirty set of one or two chunks that are almost
  // entirely air. If a per-colour skip cannot win here it cannot win anywhere,
  // because every other scenario is this one plus more matter.
  PhaseHist phMinimal;
  {
    // Any powder will do; sand by name so the scene is describable.
    uint32_t sandId = 0;
    for (uint32_t i = 0; i < (uint32_t)mats.size(); i++)
      if (mats[i].name == "sand") { sandId = i; break; }
    if (sandId == 0)
      for (uint32_t i = 1; i < (uint32_t)mats.size(); i++)
        if (mats[i].gpu.klass == CLASS_POWDER) { sandId = i; break; }

    sim.SetPassTimer(nullptr);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    for (uint32_t t = 1; t <= 200; t++)
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, {}, false,
                 {8, 3, 8}, true, false);
    ctx.WaitIdle();
    if (haveTimer) sim.SetPassTimer(&timer);
    timer.ResetStats();

    const int gx = 100, gz = 100;
    const int gy = World::TerrainHeight(gx, gz, kDefaultSeed) + 40;
    double t0 = NowSeconds();
    uint32_t ppt = 0;
    const uint64_t skips0 = sim.CaSkipCount();
    uint64_t activeSum = 0;
    uint32_t activeMax = 0, activeSamples = 0;
    for (uint32_t i = 0; i < kPerScenario; i++) {
      const uint32_t t = 401 + i;
      std::vector<CellOp> cells;
      if ((i % 40) == 0)
        cells.push_back({World::SlotCellIndex({gx, gy, gz}), sandId & 0xFFFu});
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, {}, cells, false,
                 {8, 3, 8}, true, false);
      if (i == 0) ppt = timer.PassesThisBuffer() / 2;
      ctx.WaitIdle();
      timer.Collect(ctx);
      const WorldSnapshot& sn = world.Snap();
      if (sn.valid) {
        activeSum += sn.activeChunks;
        activeMax = std::max(activeMax, sn.activeChunks);
        activeSamples++;
      }
      if (phaseHistOn)
        SamplePhaseOccupancy(ctx, world, sim, mats, phMinimal, !cells.empty());
    }
    const double wall = (NowSeconds() - t0) / (double)kPerScenario;
    PrintPassTable("(e) MINIMAL (one sand voxel dropped every 40 ticks)", timer,
                   kPerScenario, wall, ppt, sim.CaSkipCount() - skips0,
                   activeSamples ? (double)activeSum / (double)activeSamples : 0.0,
                   activeMax);
  }

  if (phaseHistOn) {
    std::printf("\n=== MEASUREMENT 3: per-colour-phase occupancy of the dirty "
                "set ===\n");
    std::printf("  Ceiling on per-phase CA dispatch skipping. See the comment on\n"
                "  PhaseHist: these are FRESH, UNDILATED numbers, so they bound\n"
                "  any implementable (stale, dilated) mechanism from above.\n");
    phSettled.Print("(c) SETTLED");
    phActive.Print("(b) ACTIVE");
    phSettling.Print("(a) SETTLING");
    phHeavy.Print("(d) HEAVY");
    phMinimal.Print("(e) MINIMAL");
  }

  sim.SetPassTimer(nullptr);
  std::printf("\n=== measure complete ===\n");
  return 0;
}

}  // namespace sandvox
