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

#include "sim/faredits.h"
#include "sim/pagetable.h"
#include "sim/stream.h"
#include "sim/worldedit.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
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
    layer.Resolve(kDefaultSeed);   // v1 is absolute: the seed changes nothing
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
      // AT ITS CELL, OR ONE BELOW IT, and the second alternative is not a
      // loosened assertion — it is the same assertion in a world where
      // isolated solids fall. This layer places ONE wood voxel into open air,
      // and since 2026-09-04 a solid with no solid or powder on any of its six
      // faces is a one-voxel island and drops in the CA (`soloSolid`,
      // sim_step.wgsl; owned by the `tree-fell` gate). The tick this gate
      // submits runs sim_mutate AND sim_step, so a correctly applied edit has
      // already taken its first step down by the time it is read back.
      //
      // The claim is unchanged and still cannot be satisfied by accident: if
      // the op never reached the grid, NEITHER cell holds wood. What the gate
      // says the tuner promises — "an edit will be there when you play" — is
      // exactly as true; a lone floating voxel simply now falls to the ground
      // like anything else with nothing under it, which is what an author
      // placing one should expect of a falling-sand world.
      // Searched down its own COLUMN rather than at one fixed offset: a tick
      // runs two gravity substeps, so the number of cells it has dropped by the
      // time this reads back is a property of the substep schedule and not
      // something this gate should be pinned to. The column stays inside the
      // chunk that was read (local y is 3), so this is still one chunk's data.
      const uint32_t landedAt = after[inChunk] & 0xFFFu;
      int foundY = -1;
      for (int y = wantVox2.y; y >= (wc.y * (int)kChunk) && foundY < 0; y--) {
        const uint32_t ic =
            World::SlotCellIndex({wantVox2.x, y, wantVox2.z}) - slot * kChunkVol;
        if ((after[ic] & 0xFFFu) == 2u) foundY = y;
      }
      check(foundY >= 0,
            "the applied voxel is nowhere in its column: reads material " +
                std::to_string(landedAt) + " at its own cell (was " +
                std::to_string(before[inChunk] & 0xFFFu) +
                "), and no wood anywhere below it in the chunk");
    }
    check(!layer.HasPending(), "layer still has pending chunks after a full drain");

    // A chunk outside the window must be DROPPED, not clamped: a cell index is
    // window relative, so applying one for a chunk that is not resident writes
    // into whatever now owns that slot.
    WorldEdits far;
    far.Load(p.string(), e);
    far.QueueChunk({wc.x + (int)kNChunk * 4, wc.y, wc.z}, kDefaultSeed);
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

// ============================ worldedit =====================================
//
// The edit layer AS WORLDGEN (map-overhaul P7, sim/worldedit.h). Four claims,
// each one a thing the layer did not do before P7:
//
//   A. GROUND-RELATIVE. A v2 file's cells land at TerrainHeight + dy for the
//      seed they are resolved at — and follow the ground to another seed —
//      while a v1 file's cells stay where they were written.
//   B. THE VOXEL SERVER APPLIES IT. A region dump holds the layer's word at
//      the layer's cell; with applyEdits off it does not.
//   C. THE FAR FIELD SEES IT. A layer cell that is a cascade sample center is
//      in FarEdits' patch for that level chunk as the BASE, and the base
//      survives the Clear a load or regen makes.
//   D. IT IS NOT A MODIFICATION. The window regenerated with the layer
//      installed, ticked through THE tick: the layer's word is in the grid,
//      and the chunk it went into is NOT in the stream's modified set, so a
//      save does not bake it and an eviction lets genChunk + the layer
//      re-derive it. The fold's exemption is counted, so a pass that never
//      exercised it cannot pass.
//
// The layer is installed in the PROCESS layer (WorldEditLayer) for B and D
// and cleared, with the window regenerated, before returning: no later gate
// may see a layer (a layer moves the world hash by construction).
Status GateWorldEdit(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const IVec3 origin = world.WindowOrigin();
  const uint32_t seed = kDefaultSeed;
  int failures = 0;
  std::string notes;
  auto check = [&](bool ok, const std::string& msg) {
    if (!ok) { failures++; notes += (notes.empty() ? "" : "; ") + msg; }
  };

  // The column: the window's middle, off the chunk grid's faces so the layer
  // chunk's neighbours are all resident.
  const int x = origin.x * (int)kChunk + (int)kWorldN / 2 + 5;
  const int z = origin.z * (int)kChunk + (int)kWorldN / 2 + 9;
  const int h = World::TerrainHeight(x, z, seed);
  const int y0 = origin.y * (int)kChunk;
  // BURIED: 12 cells into the ground, so the layer's wood has rock all round
  // it and nothing in the CA has a reason to touch the chunk after the op.
  const int dyBuried = -12;
  const uint32_t wood = PackVoxNew(2, 1);
  if (h + dyBuried - 2 < y0 || h + 2 >= y0 + (int)kWorldN) {
    detail = Format("fixture column (%d,%d) ground %d is outside the window's y range", x, z, h);
    std::printf("worldedit: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  // A cascade sample center at level 1 near the same ground, for C.
  const int shift1 = (int)(1 + kFarShiftBase);
  const int step1 = 1 << shift1, half1 = step1 >> 1;
  const int cx1 = ((x >> shift1) << shift1) + half1;
  const int cz1 = ((z >> shift1) << shift1) + half1;
  const int cy1 = (((h - 4) >> shift1) << shift1) + half1;

  // ---- A: the writers and the resolve ---------------------------------------
  const std::string relPath = "selftest_worldedit_rel.svedit";
  const std::string absPath = "selftest_worldedit_abs.svedit";
  std::vector<WorldEdits::AbsCell> cells = {
      {x, h + dyBuried, z, wood},
      {cx1, cy1, cz1, wood},
  };
  std::string e;
  check(WorldEdits::WriteRelative(relPath, cells, seed, e), "WriteRelative: " + e);
  check(WorldEdits::WriteAbsolute(absPath, cells, seed, e), "WriteAbsolute: " + e);
  {
    WorldEdits rel;
    check(rel.Load(relPath, e) && rel.Relative(), "v2 load: " + e);
    rel.Resolve(seed);
    std::vector<WorldEdits::AbsCell> got;
    rel.ResolvedCells(got);
    bool found = false;
    for (const auto& g : got) found |= (g.x == x && g.y == h + dyBuried && g.z == z && g.word == wood);
    check(found, Format("v2 cell did not resolve to (%d,%d,%d) at seed %u", x, h + dyBuried, z, seed));
    // Another seed: the cell follows ITS ground.
    const uint32_t seed2 = seed + 7919u;
    const int h2 = World::TerrainHeight(x, z, seed2);
    rel.Resolve(seed2);
    rel.ResolvedCells(got);
    found = false;
    for (const auto& g : got) found |= (g.x == x && g.y == h2 + dyBuried && g.z == z);
    check(found, Format("v2 cell did not follow the ground to seed %u (ground %d -> %d)", seed2, h, h2));
    WorldEdits abs;
    check(abs.Load(absPath, e) && !abs.Relative(), "v1 load: " + e);
    abs.Resolve(seed2);
    abs.ResolvedCells(got);
    found = false;
    for (const auto& g : got) found |= (g.x == x && g.y == h + dyBuried && g.z == z);
    check(found, "a v1 (absolute) cell moved with the seed");
  }

  // ---- B: the voxel server applies the layer --------------------------------
  WorldEditLayer().Clear();
  check(WorldEditLayer().Load(relPath, e), "process layer load: " + e);
  auto dumpCell = [&](bool apply, uint32_t& word) {
    VoxRegionReq req;
    req.ox = (x >> 4) << 4;
    req.oy = ((h + dyBuried) >> 4) << 4;
    req.oz = (z >> 4) << 4;
    req.nx = req.ny = req.nz = 16;
    req.lod = 1;
    req.seed = seed;
    req.applyEdits = apply;
    std::vector<uint8_t> blob;
    std::string err, why;
    if (!BuildVoxRegion(ctx, world, sim, req, blob, err)) { check(false, "region: " + err); return false; }
    std::vector<uint32_t> grid;
    VoxRegionReq head{};
    if (!DecodeRegion(blob, head, grid, why)) { check(false, "region decode: " + why); return false; }
    const int lx = x - req.ox, ly = h + dyBuried - req.oy, lz = z - req.oz;
    word = grid[((size_t)lz * 16 + ly) * 16 + lx] & ~kStampBits;
    return true;
  };
  uint32_t withLayer = 0, without = 0;
  if (dumpCell(true, withLayer) && dumpCell(false, without)) {
    check(withLayer == wood, Format("region with the layer holds 0x%08x at the layer cell, want 0x%08x", withLayer, wood));
    check(without != wood, "the bare-worldgen region already holds the layer's word: the fixture proves nothing");
  }

  // ---- C: the far field's base ----------------------------------------------
  {
    FarEdits fe;
    WorldEditLayer().Resolve(seed);
    WorldEditLayer().SeedFarField(fe);
    const IVec3 lc{cx1 >> (shift1 + 4), cy1 >> (shift1 + 4), cz1 >> (shift1 + 4)};
    const uint32_t ci = (uint32_t)((((cz1 >> shift1) & 15) * 16 + ((cy1 >> shift1) & 15)) * 16 +
                                   ((cx1 >> shift1) & 15));
    auto hasPatch = [&]() {
      const std::vector<uint32_t>* p = fe.Lookup(1, lc);
      if (!p) return false;
      for (uint32_t w : *p)
        if ((w & FarEdits::kCellMask) == ci && (w >> FarEdits::kCellBits) == (wood & 0xFFFu)) return true;
      return false;
    };
    check(hasPatch(), "the layer's sample-center cell is not in the far field's level-1 patch");
    fe.Clear();   // what a load / regen does
    check(hasPatch(), "the far-field base did not survive Clear()");
  }

  // ---- D: applied as worldgen, not as a modification -------------------------
  //
  // The claim is about the LAYER, so the fixture chunk must be one the world
  // leaves alone WITHOUT it: a freshly generated window still has settling and
  // staining chunks (measured: a buried chunk near wet soil reported
  // DIRTY_R_STAINW every tick), and those are modified whatever the layer
  // does. So each candidate column is first run as a CONTROL arm — regen with
  // no layer, the same ticks — and the first whose buried chunk stays
  // unmodified is the fixture. Then the same regen and ticks WITH a layer
  // that puts wood in that chunk.
  uint64_t ignored = 0;
  {
    const int nTicks = (int)World::kSnapshotLatency + 8;
    // Regenerate the window, tick it, and return the slot's modified flag plus
    // a "seq:reason:modified" trace (reason = the raw DIRTY_R_* word, hex).
    auto arm = [&](IVec3 wc, std::string& trace) {
      c.stream.OnRegen();
      world.SetWindowOrigin(origin);
      SubmitWorldgen(ctx, world, sim, seed);   // queues the layer, if any (QueueWindow)
      ctx.WaitIdle();
      const uint32_t slot = World::SlotChunkIndex(wc);
      support::TickRig rig(c, 90000u, wc);
      world.SetDirtyWatch(slot);
      trace.clear();
      for (int i = 0; i < nTicks; i++) {
        support::RunTicks(rig, 1);
        const WorldSnapshot& sn = world.Snap();
        trace += Format("%s%u:%x:%u", trace.empty() ? "" : " ", sn.valid ? sn.submitSeq : 0u,
                        sn.valid ? sn.watchReason : 0xFFFFu,
                        (uint32_t)c.stream.ModifiedFlags()[slot]);
      }
      world.SetDirtyWatch(0xFFFFFFFFu);
      ctx.WaitIdle();
      return c.stream.ModifiedFlags()[slot] != 0;
    };
    const int cand[][2] = {{5, 9}, {-70, 41}, {93, -60}, {-121, -110}, {141, 133}, {29, -150},
                           {-150, 150}, {150, -150}};
    int xD = 0, zD = 0, hD = 0;
    bool found = false;
    std::string controls;
    WorldEditLayer().Clear();
    for (const auto& cd : cand) {
      const int cx = origin.x * (int)kChunk + (int)kWorldN / 2 + cd[0];
      const int cz = origin.z * (int)kChunk + (int)kWorldN / 2 + cd[1];
      const int ch = World::TerrainHeight(cx, cz, seed);
      if (ch + dyBuried - 2 < y0 || ch + 2 >= y0 + (int)kWorldN) continue;
      std::string tr;
      const bool mod = arm({cx >> 4, (ch + dyBuried) >> 4, cz >> 4}, tr);
      controls += Format("%s(%d,%d):%s", controls.empty() ? "" : " ", cx, cz, mod ? "busy" : "quiet");
      if (!mod) { xD = cx; zD = cz; hD = ch; found = true; break; }
    }
    std::printf("worldedit: D control arms (no layer): %s\n", controls.c_str());
    check(found, "no candidate column had a quiet buried chunk without the layer: " + controls);
    if (found) {
      const std::string dPath = "selftest_worldedit_d.svedit";
      check(WorldEdits::WriteRelative(dPath, {{xD, hD + dyBuried, zD, wood}}, seed, e),
            "D layer write: " + e);
      WorldEditLayer().Clear();
      check(WorldEditLayer().Load(dPath, e), "D layer load: " + e);
      const IVec3 wc{xD >> 4, (hD + dyBuried) >> 4, zD >> 4};
      const uint32_t slot = World::SlotChunkIndex(wc);
      const uint64_t ignored0 = c.stream.LayerWakesIgnored();
      std::string trace;
      const bool mod = arm(wc, trace);
      ignored = c.stream.LayerWakesIgnored() - ignored0;
      std::string seqs;
      for (uint32_t s : WorldEditLayer().AppliedSeqs()) seqs += Format("%s%u", seqs.empty() ? "" : ",", s);
      std::printf("worldedit: D layer arm at (%d,%d): trace (snapSeq:reason:modified) %s | layer "
                  "applied at seqs [%s]\n",
                  xD, zD, trace.c_str(), seqs.c_str());
      std::vector<uint32_t> words(kChunkVol);
      ReadVoxelsSync(ctx, world, slot, 1, words.data(), "worldeditD");
      const uint32_t li = (uint32_t)(xD & 15) + (uint32_t)((hD + dyBuried) & 15) * kChunk +
                          (uint32_t)(zD & 15) * kChunk * kChunk;
      check((words[li] & 0xFFFu) == (wood & 0xFFFu),
            Format("after the ticks the layer cell holds material %u, not the layer's %u",
                   words[li] & 0xFFFu, wood & 0xFFFu));
      check(ignored > 0, "the modified fold never saw the layer's own wake (nothing exempted)");
      check(!mod, "the layer's chunk is in the stream's modified set (its control arm was not): "
                  "a save would bake the layer into it");
      std::error_code ec;
      std::filesystem::remove(dPath, ec);
    }
  }


  // ---- restore: no layer, a pristine window ----------------------------------
  WorldEditLayer().Clear();
  c.stream.OnRegen();
  world.SetWindowOrigin(origin);
  SubmitWorldgen(ctx, world, sim, seed);
  ctx.WaitIdle();
  std::error_code ec;
  std::filesystem::remove(relPath, ec);
  std::filesystem::remove(absPath, ec);

  detail = Format("column (%d,%d) ground %d; layer wakes exempted %llu; %d checks failed%s%s",
                  x, z, h, (unsigned long long)ignored, failures, failures ? " — " : "",
                  notes.c_str());
  std::printf("worldedit: %s (%s)\n", failures ? "FAIL" : "PASS", detail.c_str());
  return failures ? Status::Fail : Status::Pass;
}

}  // namespace

const std::vector<Gate>& VoxRegionGates() {
  static const std::vector<Gate> g = {
      {"voxregion", "tools", {}, false, GateVoxRegion},
      {"worldedit", "tools", {}, false, GateWorldEdit},
  };
  return g;
}

}  // namespace selftest
