// selftest_voxregion.cpp — the tuner's voxel terrain view, from both ends.
//
// TWO THINGS THIS PROTECTS, and they fail differently:
//
//   1. The REGION DUMP (src/tools/voxregion.h). The viewer draws whatever this
//      hands it, so a dump that is subtly wrong — an off-by-one in the chunk
//      walk, a slot run mis-scattered, an RLE that loses the last run — shows up
//      as terrain that looks plausible and is not the world. There is nothing
//      in a picture that catches that, which is exactly why it is asserted
//      against a DIRECT readback of the same voxels here.
//
//   2. The EDIT LAYER (src/sim/worldedit.h). A layer emits CellOps against a
//      WINDOW-RELATIVE slot index. Get that wrong and the edits land in some
//      other chunk — a hole punched a kilometre from where it was drawn — and
//      the failure is invisible unless you happen to fly over the right place.
//
// Both halves run without the render path and without a window.
//
// GATE ORDER: this runs LAST, and it restores the window and regenerates
// afterwards regardless. BuildVoxRegion moves the residency window and resets
// the whole page table, which is exactly the state every other gate's fixture
// placement assumes (selftest.h's ordering note, CLAUDE.md rule 7). Leaving
// that mess behind would fail the next gate and blame it.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "sim/pagetable.h"
#include "sim/worldedit.h"
#include "test/selftest.h"
#include "test/support.h"
#include "tools/voxregion.h"

using namespace sandvox;

namespace selftest {
namespace {

// Decode the 'SVVX' RLE back to a flat sample grid — the same walk
// assets/worldview.js does, so a format change that breaks one breaks this.
bool DecodeRegion(const std::vector<uint8_t>& b, VoxRegionReq& head,
                  std::vector<uint32_t>& out, std::string& why) {
  auto rd = [&](size_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
           ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
  };
  if (b.size() < kVoxRegionHeaderBytes || rd(0) != kVoxRegionMagic) {
    why = "bad magic";
    return false;
  }
  head.ox = (int32_t)rd(8); head.oy = (int32_t)rd(12); head.oz = (int32_t)rd(16);
  head.nx = rd(20); head.ny = rd(24); head.nz = rd(28);
  head.lod = rd(32); head.seed = rd(36);
  const uint32_t runs = rd(48);
  const size_t total = (size_t)head.nx * head.ny * head.nz;
  if (b.size() != kVoxRegionHeaderBytes + (size_t)runs * 8) {
    why = "payload length disagrees with runCount";
    return false;
  }
  out.assign(total, 0);
  size_t o = 0;
  for (uint32_t i = 0; i < runs; i++) {
    const uint32_t w = rd(kVoxRegionHeaderBytes + (size_t)i * 8);
    const uint32_t n = rd(kVoxRegionHeaderBytes + (size_t)i * 8 + 4);
    for (uint32_t k = 0; k < n && o < total; k++) out[o++] = w;
  }
  if (o != total) {
    why = "runs cover " + std::to_string(o) + " of " + std::to_string(total) +
          " samples";
    return false;
  }
  return true;
}

Status GateVoxRegion(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const IVec3 savedOrigin = world.WindowOrigin();

  int failures = 0;
  std::string notes;
  auto check = [&](bool ok, const std::string& msg) {
    if (!ok) { failures++; notes += (notes.empty() ? "" : "; ") + msg; }
  };

  // ---- A: lod 1 is EXACT against a direct readback -----------------------
  //
  // Anchored on the terrain rather than on a literal Y (support.h): the box has
  // to straddle the surface or "every sample matches" would be two stone chunks
  // agreeing with each other.
  //
  // TerrainHeight and NOT FixtureY, which is the trap this gate fell into on
  // its first run: FixtureY CLAMPS to the current residency window, and this
  // gate is the one caller that then moves the window itself. Anchored against
  // the window the earlier gates left behind, the box came out 500 voxels
  // underground and every sample was stone — a green "the dump matches the
  // readback" over a box that could not have caught anything.
  // The MEAN over the footprint, and the box CENTRED on it. The max is what a
  // fixture wants (a slab laid at the centre height has its uphill half
  // buried); a probe wants the opposite, because a box hung off the highest
  // peak in the footprint is mostly sky — measured 2% solid, which passed a
  // "does it straddle" check that was only checking that it was not 0%.
  long long sum = 0;
  for (int j = 0; j <= 4; j++)
    for (int i = 0; i <= 4; i++)
      sum += World::TerrainHeight(i * 16, j * 16, kDefaultSeed);
  const int gy = (int)(sum / 25);
  const int boxY = ((gy - 32) / (int)kChunk) * (int)kChunk;
  VoxRegionReq req;
  req.ox = 0; req.oy = boxY; req.oz = 0;
  req.nx = req.ny = req.nz = 64;
  req.lod = 1;
  req.seed = kDefaultSeed;

  std::vector<uint8_t> blob;
  std::string err;
  bool built = BuildVoxRegion(ctx, world, sim, req, blob, err);
  check(built, "BuildVoxRegion failed: " + err);

  std::vector<uint32_t> grid;
  VoxRegionReq head{};
  uint32_t solidSamples = 0, distinctMats = 0;
  if (built) {
    std::string why;
    check(DecodeRegion(blob, head, grid, why), "decode: " + why);
    check(head.ox == req.ox && head.oy == req.oy && head.oz == req.oz &&
          head.nx == 64 && head.lod == 1 && head.seed == req.seed,
          "header does not echo the request");
  }

  // The oracle: the same voxels, read straight out of the pool. The window is
  // still where BuildVoxRegion left it, so the box is resident.
  if (built && grid.size() == 64ull * 64 * 64) {
    bool matched = true;
    size_t firstBad = SIZE_MAX;
    std::vector<uint32_t> chunkWords(kChunkVol);
    bool seen[4096] = {false};
    for (uint32_t cz = 0; cz < 4 && matched; cz++)
      for (uint32_t cy = 0; cy < 4 && matched; cy++)
        for (uint32_t cx = 0; cx < 4 && matched; cx++) {
          const IVec3 wc{req.ox / (int)kChunk + (int)cx,
                         req.oy / (int)kChunk + (int)cy,
                         req.oz / (int)kChunk + (int)cz};
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wc), 1,
                         chunkWords.data(), "voxregionGate");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            const uint32_t lx = k % kChunk, ly = (k / kChunk) % kChunk,
                           lz = k / (kChunk * kChunk);
            const size_t gi = (((size_t)(cz * kChunk + lz) * 64) +
                               (cy * kChunk + ly)) * 64 + (cx * kChunk + lx);
            // The tick stamp is per-tick scheduling scratch, excluded from the
            // world hash and meaningless in a dump; everything else must be
            // bit-identical.
            const uint32_t a = grid[gi] & ~kStampBits;
            const uint32_t b = chunkWords[k] & ~kStampBits;
            if (a != b) { matched = false; firstBad = gi; break; }
          }
        }
    check(matched, matched ? "" :
          ("lod-1 sample " + std::to_string(firstBad) + " differs from readback"));
    for (uint32_t w : grid) {
      const uint32_t m = w & 0xFFFu;
      if (m) solidSamples++;
      if (m < 4096 && !seen[m]) { seen[m] = true; distinctMats++; }
    }
    // A REAL straddle, not merely "not empty": the oracle comparison below is
    // only as good as the variety in the box, and a 2%-solid box agrees with a
    // readback for reasons that have nothing to do with the walk being right.
    const double frac = (double)solidSamples / (double)grid.size();
    check(frac > 0.15 && frac < 0.85,
          "box is " + std::to_string(frac) + " solid — wanted a surface through "
          "the middle, not a slab or the sky");
    check(distinctMats >= 3, "only " + std::to_string(distinctMats) +
          " distinct materials — expected terrain, not one slab");
  }

  // ---- B: lod > 1 invents nothing ----------------------------------------
  //
  // The downsample is a MAJORITY over real cells, so every material it emits
  // must be a material that is actually there. A sampler that read out of
  // bounds, or a tally that leaked counts between blocks, shows up here as a
  // material the fine grid never contained.
  {
    VoxRegionReq c2 = req;
    c2.lod = 4;
    c2.nx = c2.ny = c2.nz = 16;         // same 64-voxel box, 4x coarser
    std::vector<uint8_t> b2;
    std::string e2;
    if (BuildVoxRegion(ctx, world, sim, c2, b2, e2)) {
      std::vector<uint32_t> g2;
      VoxRegionReq h2{};
      std::string why;
      if (DecodeRegion(b2, h2, g2, why) && grid.size()) {
        bool fineHas[4096] = {false};
        for (uint32_t w : grid) fineHas[w & 0xFFFu] = true;
        uint32_t invented = 0, coarseSolid = 0;
        for (uint32_t w : g2) {
          const uint32_t m = w & 0xFFFu;
          if (m) coarseSolid++;
          if (!fineHas[m]) invented++;
        }
        check(invented == 0, std::to_string(invented) +
              " lod-4 samples hold a material the lod-1 box does not");
        // Majority, not point-sampling: a coarse box over a surface must stay
        // roughly as full as the fine one. Point sampling would drift far.
        const double fineFrac = (double)solidSamples / (double)grid.size();
        const double coarseFrac = (double)coarseSolid / (double)g2.size();
        check(std::abs(fineFrac - coarseFrac) < 0.25,
              "lod-4 fullness " + std::to_string(coarseFrac) +
              " vs lod-1 " + std::to_string(fineFrac));
      } else {
        check(false, "lod-4 decode: " + why);
      }
    } else {
      check(false, "lod-4 build: " + e2);
    }
  }

  // ---- C: the request contract is a REFUSAL, not a clamp ------------------
  // Every one of these is reachable from a browser, and a viewer that silently
  // received a different box than it asked for would place the mesh at the
  // origin it requested and blame the seam on worldgen.
  {
    std::vector<uint8_t> junk;
    std::string e;
    VoxRegionReq bad = req;
    bad.lod = 3;
    check(!BuildVoxRegion(ctx, world, sim, bad, junk, e), "lod 3 was accepted");
    bad = req; bad.ox = 8;
    check(!BuildVoxRegion(ctx, world, sim, bad, junk, e),
          "unaligned origin was accepted");
    bad = req; bad.lod = 16; bad.nx = bad.ny = bad.nz = 64;   // 1024 voxels/axis
    check(!BuildVoxRegion(ctx, world, sim, bad, junk, e),
          "a box wider than the residency window was accepted");
  }

  // ---- restore the window BEFORE part D ----------------------------------
  //
  // D applies real ops to a real world, so it needs one: BuildVoxRegion left
  // the page table reset to all-EMPTY except its last batch, and a CellOp
  // against a sentinel chunk is a page fault, not an edit. Restoring here also
  // means an early `return` cannot leave the window moved for the next gate.
  world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // ---- D: the edit layer round-trips, and the voxel actually changes ------
  //
  // Written as bytes and read back through the real loader, because the file is
  // the contract between assets/worldview.js and the engine and a struct copy
  // would test neither side of it.
  {
    const std::filesystem::path p = "selftest_edits.svedit";
    // A chunk well inside the restored window, high enough to be open air so
    // the before/after is unambiguous.
    const IVec3 wc{savedOrigin.x + 16, savedOrigin.y + (int)kNChunk - 3,
                   savedOrigin.z + 16};
    const uint32_t localIdx = (7u * kChunk + 3u) * kChunk + 11u;   // (11,3,7)
    const uint32_t word = PackVoxNew(2, 1);
    {
      std::vector<uint8_t> f(32 + 16 + 8, 0);
      auto put = [&](size_t o, uint32_t v) {
        f[o] = (uint8_t)v; f[o+1] = (uint8_t)(v>>8);
        f[o+2] = (uint8_t)(v>>16); f[o+3] = (uint8_t)(v>>24);
      };
      put(0, 0x44455653u); put(4, 1); put(8, 1); put(12, 1); put(16, kDefaultSeed);
      put(32, (uint32_t)wc.x); put(36, (uint32_t)wc.y); put(40, (uint32_t)wc.z);
      put(44, 1);
      put(48, localIdx); put(52, word);
      std::ofstream o(p, std::ios::binary);
      o.write((const char*)f.data(), (std::streamsize)f.size());
    }
    WorldEdits layer;
    std::string e;
    check(layer.Load(p.string(), e), "layer load: " + e);
    check(layer.VoxelCount() == 1 && layer.ChunkCount() == 1,
          "layer holds " + std::to_string(layer.VoxelCount()) + " voxels in " +
          std::to_string(layer.ChunkCount()) + " chunks");

    layer.QueueWindow(world);
    std::vector<CellOp> ops;
    const uint32_t n = layer.Drain(world, ops, 64);
    check(n == 1 && ops.size() == 1, "drain produced " + std::to_string(n) + " ops");
    if (ops.size() == 1) {
      // THE ASSERTION THIS GATE EXISTS FOR: the op's cell index must resolve to
      // the world voxel the layer named, through the engine's own mapping.
      const IVec3 wantVox{wc.x * (int)kChunk + 11, wc.y * (int)kChunk + 3,
                          wc.z * (int)kChunk + 7};
      check(ops[0].cellIdx == World::SlotCellIndex(wantVox),
            "cell index " + std::to_string(ops[0].cellIdx) + " != " +
            std::to_string(World::SlotCellIndex(wantVox)));
      check(ops[0].word == word, "word was not carried through");
      check((ops[0].word & kCellOpIfAir) == 0, "kCellOpIfAir survived the load");

      // THE END-TO-END ASSERTION: push the ops through the ordinary tick and
      // read the voxel back. Everything above proves the layer computes the
      // right op; only this proves the op reaches the grid, which is the whole
      // claim the tuner makes when it says an edit will be there when you play.
      const IVec3 wantVox2{wc.x * (int)kChunk + 11, wc.y * (int)kChunk + 3,
                           wc.z * (int)kChunk + 7};
      std::vector<uint32_t> before(kChunkVol), after(kChunkVol);
      const uint32_t slot = World::SlotChunkIndex(wc);
      const uint32_t inChunk = World::SlotCellIndex(wantVox2) - slot * kChunkVol;
      ReadVoxelsSync(ctx, world, slot, 1, before.data(), "editBefore");
      SubmitTick(ctx, world, sim, 1, kDefaultSeed, {}, {}, ops, false,
                 {8, 3, 8}, false, false);
      ctx.WaitIdle();
      ReadVoxelsSync(ctx, world, slot, 1, after.data(), "editAfter");
      check((after[inChunk] & 0xFFFu) == 2u,
            "the applied voxel reads material " +
            std::to_string(after[inChunk] & 0xFFFu) + ", wanted 2 (was " +
            std::to_string(before[inChunk] & 0xFFFu) + ")");
    }
    check(!layer.HasPending(), "layer still has pending chunks after a full drain");

    // A chunk outside the window must be DROPPED, not clamped: a cell index is
    // window relative, so applying one for a chunk that is not resident writes
    // into whatever now owns that slot.
    WorldEdits far;
    far.Load(p.string(), e);
    far.QueueChunk({wc.x + (int)kNChunk * 4, wc.y, wc.z});
    std::vector<CellOp> none;
    check(far.Drain(world, none, 64) == 0 && none.empty(),
          "an out-of-window chunk emitted ops");

    // A malformed file must be refused whole, not half-loaded.
    {
      std::vector<uint8_t> f(32 + 16 + 8, 0);
      auto put = [&](size_t o, uint32_t v) {
        f[o] = (uint8_t)v; f[o+1] = (uint8_t)(v>>8);
        f[o+2] = (uint8_t)(v>>16); f[o+3] = (uint8_t)(v>>24);
      };
      put(0, 0x44455653u); put(4, 1); put(8, 1); put(12, 1);
      put(44, 1);
      put(48, kChunkVol + 5);            // a local index past the chunk
      std::ofstream o("selftest_edits_bad.svedit", std::ios::binary);
      o.write((const char*)f.data(), (std::streamsize)f.size());
    }
    WorldEdits bad;
    std::string be;
    check(!bad.Load("selftest_edits_bad.svedit", be) && bad.Empty(),
          "an out-of-range cell index was accepted");
    std::error_code ec;
    std::filesystem::remove(p, ec);
    std::filesystem::remove("selftest_edits_bad.svedit", ec);
  }

  // Part D edited the world; hand the next gate a pristine one.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  detail = Format("%u samples solid, %u materials, %zu-byte region; %d checks failed%s%s",
                  solidSamples, distinctMats, blob.size(), failures,
                  failures ? " — " : "", notes.c_str());
  std::printf("voxregion: %s (%s)\n", failures ? "FAIL" : "PASS", detail.c_str());
  return failures ? Status::Fail : Status::Pass;
}

}  // namespace

const std::vector<Gate>& VoxRegionGates() {
  static const std::vector<Gate> g = {
      {"voxregion", "tools", {}, false, GateVoxRegion},
  };
  return g;
}

}  // namespace selftest
