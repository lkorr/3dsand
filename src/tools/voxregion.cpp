#include "tools/voxregion.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "sim/biomes.h"
#include "sim/chunkstore.h"
#include "sim/mattable.h"
#include "sim/pagetable.h"
#include "sim/stream.h"      // RleDecodeChunk
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/worldedit.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/support.h"

namespace sandvox {
namespace {

void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = (uint8_t)v;
  b[off + 1] = (uint8_t)(v >> 8);
  b[off + 2] = (uint8_t)(v >> 16);
  b[off + 3] = (uint8_t)(v >> 24);
}

void Push32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back((uint8_t)v);
  b.push_back((uint8_t)(v >> 8));
  b.push_back((uint8_t)(v >> 16));
  b.push_back((uint8_t)(v >> 24));
}

// Floor-division by a power of two that is correct for negative operands.
// `ox / 16` in C++ truncates toward zero, so chunk -1 and chunk 0 both come
// back as 0 and every box west or below the origin lands one chunk off. The
// whole world south and west of spawn is negative, so this is not an edge case.
inline int FloorDiv16(int v) { return v >> 4; }

// ---- the per-chunk downsample --------------------------------------------
//
// One chunk (16^3 cells) collapses to (16/lod)^3 samples. MAJORITY over the
// non-air cells of each block, because point sampling deletes precisely what
// you zoom out to look at — a cave roof, a trunk, a shoreline are all one or
// two voxels thick and a point sample drops them at lod 2.
//
// Ties go to the LOWEST material id, and the emitted word is the FIRST cell
// carrying the winner. Both are arbitrary but must be deterministic: a viewer
// that re-fetches the same box must get the same bytes or a region seam
// flickers every time it re-streams.
struct Tally {
  std::vector<uint16_t> count;  // per material id
  std::vector<uint32_t> first;  // first word seen for that material
  std::vector<uint16_t> touched;
  Tally() : count(kMaterialSlots, 0), first(kMaterialSlots, 0) {
    touched.reserve(64);
  }
  void Add(uint32_t word) {
    const uint32_t m = word & 0xFFFu;
    if (m == 0) return;  // air never votes; it wins only by default
    if (count[m] == 0) {
      touched.push_back((uint16_t)m);
      first[m] = word;
    }
    count[m]++;
  }
  uint32_t Winner() {
    uint32_t bestMat = 0, bestN = 0;
    for (uint16_t m : touched) {
      if (count[m] > bestN || (count[m] == bestN && m < bestMat)) {
        bestN = count[m];
        bestMat = m;
      }
    }
    const uint32_t w = bestMat ? first[bestMat] : 0u;
    for (uint16_t m : touched) count[m] = 0;
    touched.clear();
    return w;
  }
};

}  // namespace

// ---------------------------------------------------------------------------

std::string VoxPaletteJson(const std::vector<MaterialDef>& mats) {
  auto hex = [](uint32_t rgba) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", rgba & 0xFF,
                  (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF);
    return std::string(buf);
  };
  std::string s = "{\"materials\":[";
  for (size_t i = 0; i < mats.size(); i++) {
    const MaterialDef& m = mats[i];
    if (i) s += ",";
    s += "{\"id\":" + std::to_string(i);
    s += ",\"name\":\"" + m.name + "\"";
    s += ",\"class\":" + std::to_string(m.gpu.klass);
    s += ",\"density\":" + std::to_string(m.gpu.density);
    s += ",\"hardness\":" + std::to_string(m.gpu.hardness);
    s += ",\"emission\":" + std::to_string(m.gpu.emission);
    s += ",\"opacity\":" + std::to_string(m.gpu.opacity);
    s += ",\"flags\":" + std::to_string(m.gpu.flags);
    s += ",\"colors\":[\"" + hex(m.gpu.color0) + "\",\"" + hex(m.gpu.color1) +
         "\",\"" + hex(m.gpu.color2) + "\"]";
    s += ",\"tags\":[";
    for (size_t t = 0; t < m.tags.size(); t++)
      s += (t ? ",\"" : "\"") + m.tags[t] + "\"";
    s += "]}";
  }
  // The stain palette the voxel word's top bits index. Slot 0 is "clean", so
  // the array is 1-based against VoxStainType.
  //
  // ASSEMBLED THE SAME WAY Simulation::UploadTables assembles it, and it has to
  // be: the colours are NOT entries in `mats` — they are scattered across the
  // staining materials' own `stainColor` and only gathered into slots
  // kStainPaletteBase + type at upload. Indexing mats[kStainPaletteBase + t]
  // reads off the end of a 97-entry vector's worth of ids and hands back
  // black, which is what this loop did before it was written out properly.
  s += "],\"stains\":[";
  {
    uint32_t pal[kStainTypeMax + 1] = {0};
    for (const MaterialDef& d : mats) {
      const uint32_t type = d.stainSlot;
      if (type == 0 || type > kStainTypeMax) continue;
      pal[type] = d.gpu.stainColor;  // shared name => shared slot, last wins
    }
    for (uint32_t t = 1; t <= kStainTypeMax; t++) {
      if (t > 1) s += ",";
      s += "\"" + hex(pal[t]) + "\"";
    }
  }
  s += "],\"airId\":0,\"voxelMeters\":" + std::to_string(kVoxelMeters);
  s += ",\"chunk\":" + std::to_string(kChunk);
  s += ",\"worldN\":" + std::to_string(kWorldN) + "}";
  return s;
}

// ---------------------------------------------------------------------------

bool BuildVoxRegion(GpuContext& ctx, World& world, Simulation& sim,
                    const VoxRegionReq& req, std::vector<uint8_t>& out,
                    std::string& err) {
  // ---- validate. Every one of these is a REFUSAL, not a clamp: a viewer that
  // silently received a different box than it asked for would place the mesh
  // at the requested origin and the seam would be blamed on worldgen.
  const uint32_t lod = req.lod;
  if (lod == 0 || (lod & (lod - 1)) != 0 || lod > kVoxRegionMaxLod ||
      (kChunk % lod) != 0) {
    err = "lod must be a power of two in 1.." + std::to_string(kVoxRegionMaxLod);
    return false;
  }
  if (req.nx == 0 || req.ny == 0 || req.nz == 0 ||
      req.nx > kVoxRegionMaxSamples || req.ny > kVoxRegionMaxSamples ||
      req.nz > kVoxRegionMaxSamples) {
    err = "sample counts must be 1.." + std::to_string(kVoxRegionMaxSamples);
    return false;
  }
  // The box must be chunk-aligned in world voxels, so that one chunk's cells
  // never straddle two samples and the downsample needs no cross-chunk state.
  if ((req.ox % (int)kChunk) || (req.oy % (int)kChunk) ||
      (req.oz % (int)kChunk)) {
    err = "box origin must be a multiple of " + std::to_string(kChunk);
    return false;
  }
  const uint64_t ex = (uint64_t)req.nx * lod, ey = (uint64_t)req.ny * lod,
                 ez = (uint64_t)req.nz * lod;
  if ((ex % kChunk) || (ey % kChunk) || (ez % kChunk)) {
    err = "samples * lod must be a multiple of " + std::to_string(kChunk);
    return false;
  }
  // THE HARD CEILING, and the reason lod and sample count trade against each
  // other: the box has to fit inside ONE residency window, because the slot
  // mapping is `chunk mod kNChunk` and a box wider than the window would alias
  // two different world chunks onto one slot and generate them on top of each
  // other.
  if (ex > kWorldN || ey > kWorldN || ez > kWorldN) {
    err = "box spans " + std::to_string(ex) + "x" + std::to_string(ey) + "x" +
          std::to_string(ez) + " voxels; the residency window is " +
          std::to_string(kWorldN);
    return false;
  }

  const uint32_t nx = req.nx, ny = req.ny, nz = req.nz;
  const uint32_t cdx = (uint32_t)(ex / kChunk), cdy = (uint32_t)(ey / kChunk),
                 cdz = (uint32_t)(ez / kChunk);
  const IVec3 cmin{FloorDiv16(req.ox), FloorDiv16(req.oy), FloorDiv16(req.oz)};

  // ---- THE EDIT LAYER (sim/worldedit.h, map-overhaul P7). The region is what
  // the game spawns into, and the game patches every generated chunk with the
  // layer, so the dump does too — per chunk, on the words as they are read
  // back, before the downsample. (The viewer used to composite the layer on
  // the client, which could not see a ground-relative layer's resolution and
  // drew edits the game would put somewhere else.)
  WorldEdits* edits = nullptr;
  if (req.applyEdits && !WorldEditLayer().Empty()) {
    WorldEditLayer().Resolve(req.seed);
    edits = &WorldEditLayer();
  }

  // ---- place the window and reset the pool.
  //
  // The origin is the box's own min chunk, so every chunk of the box is
  // resident by construction and nothing else is. Resetting the table to all
  // EMPTY first is what lets a 32^3-chunk box be generated by an 8,192-page
  // pool at all: pages are recycled batch by batch (see below), exactly as
  // SubmitWorldgen does for the full window.
  if (!world.pages) {
    err = "voxregion needs paged residency";
    return false;
  }
  world.SetWindowOrigin(cmin);
  world.pages->SetWorldSeed(req.seed);
  world.InvalidateSnapshot();
  {
    TickParams tp{0, req.seed, 0, 0};
    tp.origin[0] = cmin.x;
    tp.origin[1] = cmin.y;
    tp.origin[2] = cmin.z;
    tp.labMode = World::LabWorld() ? 1u : 0u;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  }
  world.pages->ResetAllEmpty(ctx.queue);
  {
    // Clears the transient buffers (hash, support, particle counts, both dirty
    // pages) over an EMPTY slot list, the same first submit SubmitWorldgen
    // makes. Skipping it leaves the dirty pages holding the previous request's
    // chunks, and the next EncodeGenList inherits them.
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeWorldgen(enc, /*denseGen=*/false);
    ctx.queue.Submit(enc.Finish());
  }

  // ---- the sample grid. Never the box at full resolution: a lod-16 32^3
  // region covers 512^3 voxels (512 MiB of words) and produces 128 KiB.
  std::vector<uint32_t> grid((size_t)nx * ny * nz, 0u);
  const uint32_t spc = kChunk / lod;  // samples per chunk axis

  // Slot -> chunk index inside the box, so a readback run (which is ordered by
  // SLOT) can be scattered back into box order.
  const uint32_t chunkCount = cdx * cdy * cdz;
  std::vector<uint32_t> slotOf(chunkCount);
  for (uint32_t cz = 0; cz < cdz; cz++)
    for (uint32_t cy = 0; cy < cdy; cy++)
      for (uint32_t cx = 0; cx < cdx; cx++)
        slotOf[(cz * cdy + cy) * cdx + cx] = World::SlotChunkIndex(
            {cmin.x + (int)cx, cmin.y + (int)cy, cmin.z + (int)cz});

  // Order the box's chunks BY SLOT. Consecutive slots share one readback and
  // one page run, and the box's own x order is not slot order once the box
  // straddles the mod-kNChunk wrap.
  std::vector<uint32_t> order(chunkCount);
  for (uint32_t i = 0; i < chunkCount; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return slotOf[a] < slotOf[b]; });

  Tally tally;
  std::vector<uint32_t> scratch;
  std::vector<uint32_t> verdict;   // genAct, one word per batch position
  // Batch size bounded by the pool so the materialize below can never fail,
  // and by a readback that stays a few MiB.
  const uint32_t poolBatch = std::max(64u, world.pages->PoolPages() / 2u);
  const uint32_t kBatch = std::min(2048u, poolBatch);

  for (uint32_t base = 0; base < chunkCount; base += kBatch) {
    const uint32_t n = std::min(kBatch, chunkCount - base);
    std::vector<uint32_t> batchSlots(n);
    for (uint32_t k = 0; k < n; k++) {
      batchSlots[k] = slotOf[order[base + k]];
      world.pages->EnsurePageForOverwrite(batchSlots[k]);
    }
    world.pages->FlushTableWrites(ctx.queue);
    sim.WriteGenList(ctx.queue, batchSlots);
    TickParams gp{};
    gp.seed = req.seed;
    gp.genCount = n;
    gp.origin[0] = cmin.x;
    gp.origin[1] = cmin.y;
    gp.origin[2] = cmin.z;
    gp.labMode = World::LabWorld() ? 1u : 0u;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &gp, sizeof(gp));
    {
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeGenList(enc, n);
      ctx.queue.Submit(enc.Finish());
    }

    // ---- THE SKY SKIP. genChunk publishes each listed chunk's page-table
    // class into genAct (world.h kGenVerdict*; SubmitWorldgen demotes on the
    // same words), and EMPTY is exact: genChunk writes no stain and no bit 31,
    // so a chunk with no non-air cell is 4,096 zero words. Every sample such a
    // chunk owns is therefore air -- 0, which `grid` already holds -- so it is
    // neither read back (16 KiB each) nor downsampled. The sky is about half of
    // a coarse box, and its readback was most of the request.
    verdict.resize(n);
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.genAct, 0, verdict.data(),
                          (size_t)n * 4, "voxVerdict");
    auto boxChunk = [&](uint32_t boxIdx) {
      return IVec3{cmin.x + (int)(boxIdx % cdx), cmin.y + (int)((boxIdx / cdx) % cdy),
                   cmin.z + (int)(boxIdx / (cdx * cdy))};
    };
    auto skyChunk = [&](uint32_t i) {
      const uint32_t v = verdict[i];
      // A sky chunk the edit layer builds into is not sky any more.
      if (edits && edits->HasChunk(boxChunk(order[base + i]))) return false;
      return (v & kGenVerdictValid) != 0u &&
             ((v >> kGenVerdictClassShift) & kGenVerdictClassMask) == kGenVerdictEmpty;
    };

    // Read back in maximal runs of consecutive non-sky slots. ReadVoxelsSync
    // already coalesces consecutive PAGES inside a run and synthesises
    // sentinels, so this only has to find the slot runs.
    uint32_t k = 0;
    while (k < n) {
      if (skyChunk(k)) { k++; continue; }
      uint32_t run = 1;
      while (k + run < n && !skyChunk(k + run) &&
             batchSlots[k + run] == batchSlots[k] + run)
        run++;
      scratch.resize((size_t)run * kChunkVol);
      ReadVoxelsSync(ctx, world, batchSlots[k], run, scratch.data(),
                     "voxregion");
      for (uint32_t r = 0; r < run; r++) {
        const uint32_t boxIdx = order[base + k + r];
        const uint32_t cx = boxIdx % cdx, cy = (boxIdx / cdx) % cdy,
                       cz = boxIdx / (cdx * cdy);
        uint32_t* src = scratch.data() + (size_t)r * kChunkVol;
        if (edits) edits->ApplyToChunk(boxChunk(boxIdx), src);
        // Sample origin of this chunk inside the grid.
        const uint32_t sx0 = cx * spc, sy0 = cy * spc, sz0 = cz * spc;
        for (uint32_t sk = 0; sk < spc; sk++)
          for (uint32_t sj = 0; sj < spc; sj++)
            for (uint32_t si = 0; si < spc; si++) {
              if (lod == 1) {
                const uint32_t c = (sk * kChunk + sj) * kChunk + si;
                grid[((size_t)(sz0 + sk) * ny + (sy0 + sj)) * nx + sx0 + si] =
                    src[c];
                continue;
              }
              for (uint32_t bz = 0; bz < lod; bz++)
                for (uint32_t by = 0; by < lod; by++)
                  for (uint32_t bx = 0; bx < lod; bx++) {
                    const uint32_t c = ((sk * lod + bz) * kChunk +
                                        (sj * lod + by)) * kChunk +
                                       (si * lod + bx);
                    tally.Add(src[c]);
                  }
              grid[((size_t)(sz0 + sk) * ny + (sy0 + sj)) * nx + sx0 + si] =
                  tally.Winner();
            }
      }
      k += run;
    }

    // Hand the batch's pages back so the next batch can have them.
    //
    // ResetAllEmpty rather than a per-slot demote, because this is a DUMP and
    // not a world: the words are already in `grid`, nothing will tick, and no
    // later request depends on what is resident now. Classifying each chunk
    // into a sentinel (what SubmitWorldgen does) would buy compression for a
    // pool that is about to be thrown away, at a Classify per chunk.
    if (base + n < chunkCount) world.pages->ResetAllEmpty(ctx.queue);
  }

  // ---- RLE + header.
  out.clear();
  out.resize(kVoxRegionHeaderBytes, 0);
  uint32_t runCount = 0, solid = 0;
  int yMin = -1, yMax = -1;
  {
    const size_t total = grid.size();
    size_t i = 0;
    while (i < total) {
      const uint32_t w = grid[i];
      size_t run = 1;
      while (i + run < total && grid[i + run] == w) run++;
      Push32(out, w);
      Push32(out, (uint32_t)run);
      runCount++;
      i += run;
    }
    for (uint32_t y = 0; y < ny; y++) {
      bool any = false;
      for (uint32_t z = 0; z < nz && !any; z++)
        for (uint32_t x = 0; x < nx; x++)
          if (grid[((size_t)z * ny + y) * nx + x] & 0xFFFu) { any = true; break; }
      if (any) {
        if (yMin < 0) yMin = (int)y;
        yMax = (int)y;
      }
    }
    for (uint32_t v : grid)
      if (v & 0xFFFu) solid++;
  }

  Put32(out, 0, kVoxRegionMagic);
  Put32(out, 4, kVoxRegionVersion);
  Put32(out, 8, (uint32_t)req.ox);
  Put32(out, 12, (uint32_t)req.oy);
  Put32(out, 16, (uint32_t)req.oz);
  Put32(out, 20, nx);
  Put32(out, 24, ny);
  Put32(out, 28, nz);
  Put32(out, 32, lod);
  Put32(out, 36, req.seed);
  Put32(out, 40, (uint32_t)kVoxelsPerMetre);
  Put32(out, 44, 0);
  Put32(out, 48, runCount);
  Put32(out, 52, solid);
  Put32(out, 56, (uint32_t)yMin);
  Put32(out, 60, (uint32_t)yMax);
  return true;
}

// ---------------------------------------------------------------------------

namespace {

bool WriteFileBytes(const std::string& path, const void* data, size_t n,
                    std::string& err) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot write " + path;
    return false;
  }
  f.write((const char*)data, (std::streamsize)n);
  f.close();
  return true;
}

// Re-reads tuning.json and rebuilds the pipelines from it.
//
// BOTH HALVES ARE REQUIRED and the second is the non-obvious one: worldgen's
// tuning values are WGSL consts emitted by ShaderConstantPrelude and const-
// evaluated at compile time, so a server that only called LoadTuning would keep
// answering with the parameters it booted on and every slider in the tab would
// look dead. This is the same pair F5 runs in the game.
bool ReloadTuningAndShaders(GpuContext& ctx, Simulation& sim, std::string& msg) {
  Tuning t;
  LoadTuning(AssetDir() + "/materials/tuning.json", t);
  SetCurrentTuning(t);
  if (!sim.ReloadShaders(ctx.device)) {
    msg = "shader reload FAILED (kept old pipelines)";
    return false;
  }
  return true;
}

bool ParseReq(const std::vector<long long>& v, VoxRegionReq& req) {
  if (v.size() < 7) return false;
  req.ox = (int32_t)v[0];
  req.oy = (int32_t)v[1];
  req.oz = (int32_t)v[2];
  req.nx = (uint32_t)v[3];
  req.ny = (uint32_t)v[4];
  req.nz = (uint32_t)v[5];
  req.lod = (uint32_t)v[6];
  if (v.size() >= 8) req.seed = (uint32_t)v[7];
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// map-overhaul P7: the export, the swatch, and the helpers they share.

namespace {

// Generate `chunks` — all inside the residency window whose min chunk is
// `origin`, so every one has its own slot — and hand each chunk's words to
// `fn`. Pages are recycled batch by batch exactly as BuildVoxRegion does; this
// is the same genChunk the game fills its window with.
void GenerateChunks(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed,
                    IVec3 origin, const std::vector<IVec3>& chunks,
                    const std::function<void(IVec3, const uint32_t*)>& fn) {
  world.SetWindowOrigin(origin);
  world.pages->SetWorldSeed(seed);
  world.InvalidateSnapshot();
  {
    TickParams tp{0, seed, 0, 0};
    tp.origin[0] = origin.x;
    tp.origin[1] = origin.y;
    tp.origin[2] = origin.z;
    tp.labMode = World::LabWorld() ? 1u : 0u;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  }
  world.pages->ResetAllEmpty(ctx.queue);
  {
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeWorldgen(enc, /*denseGen=*/false);
    ctx.queue.Submit(enc.Finish());
  }
  const uint32_t kBatch = std::min(2048u, std::max(64u, world.pages->PoolPages() / 2u));
  std::vector<uint32_t> words(kChunkVol);
  for (size_t base = 0; base < chunks.size(); base += kBatch) {
    const uint32_t n = (uint32_t)std::min<size_t>(kBatch, chunks.size() - base);
    std::vector<uint32_t> slots(n);
    for (uint32_t k = 0; k < n; k++) {
      slots[k] = World::SlotChunkIndex(chunks[base + k]);
      world.pages->EnsurePageForOverwrite(slots[k]);
    }
    world.pages->FlushTableWrites(ctx.queue);
    sim.WriteGenList(ctx.queue, slots);
    TickParams gp{};
    gp.seed = seed;
    gp.genCount = n;
    gp.origin[0] = origin.x;
    gp.origin[1] = origin.y;
    gp.origin[2] = origin.z;
    gp.labMode = World::LabWorld() ? 1u : 0u;
    ctx.queue.WriteBuffer(world.tickUBO, 0, &gp, sizeof(gp));
    {
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeGenList(enc, n);
      ctx.queue.Submit(enc.Finish());
    }
    for (uint32_t k = 0; k < n; k++) {
      ReadVoxelsSync(ctx, world, slots[k], 1, words.data(), "exportGen");
      fn(chunks[base + k], words.data());
    }
    if (base + n < chunks.size()) world.pages->ResetAllEmpty(ctx.queue);
  }
}

// Bits that are per-tick scratch (stamp 16..18, excite 19..23) or a transient
// CPU flag (31): a stored chunk and a generated one may differ there without
// the world differing.
constexpr uint32_t kDurableBits = ~(0x00FF0000u | kCellOpIfAir);

// "a|b" -> {a, b}. The two-path commands' separator (voxregion.h).
bool SplitPaths(const std::string& s, std::string& a, std::string& b) {
  const size_t bar = s.find('|');
  if (bar == std::string::npos) return false;
  a = s.substr(0, bar);
  b = s.substr(bar + 1);
  while (!b.empty() && b.back() == ' ') b.pop_back();
  return !a.empty() && !b.empty();
}

}  // namespace

bool ExportSaveEdits(GpuContext& ctx, World& world, Simulation& sim,
                     const std::vector<MaterialDef>& mats, const std::string& saveDir,
                     uint32_t seed, const std::string& outPath, std::string& report) {
  if (!world.pages) { report = "export needs paged residency"; return false; }
  ChunkStore store;
  store.Tables().SetRunning(MaterialNameTableOf(mats));
  if (!store.BindLoad(saveDir)) { report = "cannot open save " + saveDir; return false; }
  // Grouped by 32-chunk window block (std::map: deterministic order), so each
  // group generates inside one window without two chunks sharing a slot.
  std::map<std::tuple<int, int, int>, std::vector<std::pair<IVec3, std::vector<uint32_t>>>> groups;
  size_t stored = 0;
  store.ForEachStored([&](IVec3 wc, const uint32_t* rle, size_t pairs) {
    const int s = 5;   // log2 kNChunk
    static_assert(kNChunk == 32, "ExportSaveEdits groups by 32-chunk blocks");
    groups[{wc.x >> s, wc.y >> s, wc.z >> s}].push_back(
        {wc, std::vector<uint32_t>(rle, rle + pairs * 2)});
    stored++;
  });
  std::vector<WorldEdits::AbsCell> cells;
  size_t chunksDiffer = 0, gasSkipped = 0, badRle = 0;
  std::vector<uint32_t> cur(kChunkVol);
  for (const auto& g : groups) {
    const IVec3 origin{std::get<0>(g.first) * (int)kNChunk, std::get<1>(g.first) * (int)kNChunk,
                       std::get<2>(g.first) * (int)kNChunk};
    std::vector<IVec3> list;
    for (const auto& e : g.second) list.push_back(e.first);
    size_t idx = 0;
    GenerateChunks(ctx, world, sim, seed, origin, list, [&](IVec3 wc, const uint32_t* gen) {
      const auto& e = g.second[idx++];
      if (!RleDecodeChunk(e.second.data(), e.second.size() / 2, cur.data())) { badRle++; return; }
      bool any = false;
      for (uint32_t i = 0; i < kChunkVol; i++) {
        const uint32_t a = cur[i] & kDurableBits, b = gen[i] & kDurableBits;
        if (a == b) continue;
        const uint32_t m = a & 0xFFFu;
        if (m < mats.size() && mats[m].gpu.klass == CLASS_GAS) { gasSkipped++; continue; }
        cells.push_back({wc.x * (int)kChunk + (int)(i % kChunk),
                         wc.y * (int)kChunk + (int)((i / kChunk) % kChunk),
                         wc.z * (int)kChunk + (int)(i / (kChunk * kChunk)), a});
        any = true;
      }
      if (any) chunksDiffer++;
    });
  }
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "%zu stored chunks, %zu differ from the generator: %zu cells exported "
                "(%zu gas cells skipped, %zu undecodable chunks)",
                stored, chunksDiffer, cells.size(), gasSkipped, badRle);
  report = buf;
  // The diff is against the generator AS IT IS NOW (this process's map, seed
  // `seed`). A save made under another map or generator differs wherever the
  // two disagree, and all of that lands in the layer; say which map it was.
  report += " against map '" + worldmap::CurrentWorldMap().name + "' seed " + std::to_string(seed);
  std::string err;
  if (!WorldEdits::WriteRelative(outPath, cells, seed, err)) { report += "; " + err; return false; }
  return true;
}

int RunExportEdits(GpuContext& ctx, World& world, Simulation& sim,
                   const std::vector<MaterialDef>& mats, const std::string& spec) {
  // <saveDir>,<layerName>[,seed]
  std::vector<std::string> parts;
  {
    size_t p = 0;
    while (p <= spec.size()) {
      const size_t q = spec.find(',', p);
      parts.push_back(spec.substr(p, q == std::string::npos ? std::string::npos : q - p));
      if (q == std::string::npos) break;
      p = q + 1;
    }
  }
  if (parts.size() < 2 || parts[0].empty() || parts[1].empty() ||
      parts[1].find_first_of("/\\:.") != std::string::npos) {
    std::fprintf(stderr, "--export-edits wants <saveDir>,<layerName>[,seed] (a bare layer name)\n");
    return 1;
  }
  const uint32_t seed = parts.size() >= 3 ? (uint32_t)std::strtoul(parts[2].c_str(), nullptr, 10)
                                          : (uint32_t)kDefaultSeed;
  const std::string out = AssetDir() + "/worldedits/" + parts[1] + ".svedit";
  std::string report;
  const bool ok = ExportSaveEdits(ctx, world, sim, mats, parts[0], seed, out, report);
  std::printf("export-edits: %s -> %s: %s\n", parts[0].c_str(), ok ? out.c_str() : "(nothing written)",
              report.c_str());
  return ok ? 0 : 1;
}

bool BuildBiomeSwatch(GpuContext& ctx, World& world, Simulation& sim,
                      const std::vector<MaterialDef>& mats, const std::string& biomeName,
                      const std::string& biomeJson, uint32_t seed, uint32_t sizeVox,
                      std::vector<uint8_t>& out, std::string& err) {
  const std::string dir = AssetDir();
  sizeVox = std::clamp<uint32_t>((sizeVox + 15u) & ~15u, 64u, 2048u);
  biomes::BiomeSet set;
  std::string log;
  const bool loaded = biomeJson.empty() || biomeJson == "-"
                          ? biomes::LoadBiomeSet(dir, mats, set, log)
                          : biomes::LoadBiomeSet(dir, mats, set, log, biomeName, biomeJson);
  if (!loaded) { err = "biome files did not load: " + log; return false; }
  const biomes::BiomeDef* bd = nullptr;
  for (const biomes::BiomeDef& b : set.biomes)
    if (b.name == biomeName) bd = &b;
  if (!bd) { err = "no biome named '" + biomeName + "'"; return false; }
  if (bd->index < 0) {
    err = "'" + biomeName + "' is not an engine biome (index -1): worldgen has no slot for it";
    return false;
  }
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", mats, set, atlas, log)) {
    err = "tree atlas did not load: " + log;
    return false;
  }
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) { err = "no world map loaded"; return false; }

  // ---- the synthetic one-biome map: the env-truth gate's recipe ------------
  worldmap::WorldMapData syn = real;
  std::fill(syn.biome.begin(), syn.biome.end(), static_cast<uint8_t>(bd->index));
  std::fill(syn.landform.begin(), syn.landform.end(), static_cast<uint8_t>(128));   // "flat"
  std::fill(syn.moisture.begin(), syn.moisture.end(), static_cast<uint8_t>(128));
  syn.sites.clear();
  syn.siteIndex.assign(syn.siteIndex.size(), 0);
  syn.padX0 = syn.padZ0 = 0;
  syn.padX1 = syn.padZ1 = -1;
  syn.name = "swatch:" + biomeName;
  syn.contentHash = 0x9E3779B9u * static_cast<uint32_t>(bd->index + 1);
  std::vector<uint32_t> words;
  if (!worldmap::PackWorldMap(set, syn, words, log)) { err = "swatch map did not pack: " + log; return false; }
  worldmap::SetCurrentWorldMap(syn);   // the CPU twin (TerrainHeight) sees it too
  ctx.WaitIdle();
  sim.UploadEnvironment(ctx.device, ctx.queue, atlas, words);

  // ---- the box: sizeVox square around the real spawn, the ground's band ----
  const int x0 = (real.spawnX - (int)sizeVox / 2) & ~15;
  const int z0 = (real.spawnZ - (int)sizeVox / 2) & ~15;
  int hmin = 1 << 30, hmax = -(1 << 30);
  for (uint32_t dz = 0; dz <= sizeVox; dz += 8)
    for (uint32_t dx = 0; dx <= sizeVox; dx += 8) {
      const int h = World::TerrainHeight(x0 + (int)dx, z0 + (int)dz, seed);
      hmin = std::min(hmin, h);
      hmax = std::max(hmax, h);
    }
  // Down past the skin and the sediment wedge; up past the tallest crown the
  // atlas holds (a great oak is ~270 cells at 10 vpm).
  const int y0 = (hmin - 48) & ~15;
  const int y1 = ((hmax + 320) + 15) & ~15;
  const uint32_t height = (uint32_t)std::min(y1 - y0, 1024);
  uint32_t lod = 1;
  while (std::max(sizeVox, height) / lod > 384u && lod < kVoxRegionMaxLod) lod *= 2;
  const uint32_t tile = std::min(kVoxRegionMaxSamples * lod, kWorldN);

  out.assign(32, 0);
  uint32_t count = 0;
  bool ok = true;
  for (uint32_t ty = 0; ty < height && ok; ty += tile)
    for (uint32_t tz = 0; tz < sizeVox && ok; tz += tile)
      for (uint32_t tx = 0; tx < sizeVox && ok; tx += tile) {
        VoxRegionReq req;
        req.ox = x0 + (int)tx;
        req.oy = y0 + (int)ty;
        req.oz = z0 + (int)tz;
        req.nx = std::min(tile, sizeVox - tx) / lod;
        req.ny = std::min(tile, height - ty) / lod;
        req.nz = std::min(tile, sizeVox - tz) / lod;
        req.lod = lod;
        req.seed = seed;
        req.applyEdits = false;
        std::vector<uint8_t> blob;
        if (!BuildVoxRegion(ctx, world, sim, req, blob, err)) { ok = false; break; }
        Push32(out, (uint32_t)blob.size());
        out.insert(out.end(), blob.begin(), blob.end());
        count++;
      }

  // ---- restore the real environment, whatever happened above ----------------
  {
    biomes::EnvironmentStamp stamp;
    std::string elog;
    if (!ReloadEnvironment(ctx, sim, mats, stamp, elog)) {
      worldmap::SetCurrentWorldMap(real);
      err += (err.empty() ? "" : "; ") + std::string("environment restore failed: ") + elog;
      return false;
    }
  }
  if (!ok) return false;
  Put32(out, 0, kBiomeSwatchMagic);
  Put32(out, 4, 1);
  Put32(out, 8, count);
  Put32(out, 12, (uint32_t)x0);
  Put32(out, 16, (uint32_t)y0);
  Put32(out, 20, (uint32_t)z0);
  Put32(out, 24, sizeVox);
  Put32(out, 28, lod);
  return true;
}

int RunVoxDump(GpuContext& ctx, World& world, Simulation& sim,
               const std::vector<MaterialDef>& mats, const std::string& spec,
               const std::string& outPath) {
  std::vector<long long> v;
  {
    const char* p = spec.c_str();
    while (*p) {
      char* end = nullptr;
      long long n = std::strtoll(p, &end, 10);
      if (end == p) break;
      v.push_back(n);
      p = end;
      while (*p == ',' || *p == ' ') p++;
    }
  }
  VoxRegionReq req;
  req.seed = kDefaultSeed;
  if (!ParseReq(v, req)) {
    std::fprintf(stderr,
                 "--voxdump wants ox,oy,oz,nx,ny,nz,lod[,seed], got '%s'\n",
                 spec.c_str());
    return 1;
  }
  std::vector<uint8_t> buf;
  std::string err;
  const double t0 = NowSeconds();
  if (!BuildVoxRegion(ctx, world, sim, req, buf, err)) {
    std::fprintf(stderr, "--voxdump: %s\n", err.c_str());
    return 1;
  }
  if (!WriteFileBytes(outPath, buf.data(), buf.size(), err)) {
    std::fprintf(stderr, "--voxdump: %s\n", err.c_str());
    return 1;
  }
  // The palette next to it, because a region file the viewer cannot colour is
  // half an answer and the two always travel together.
  const std::string palPath = outPath + ".palette.json";
  const std::string pal = VoxPaletteJson(mats);
  WriteFileBytes(palPath, pal.data(), pal.size(), err);
  std::printf("voxdump: %ux%ux%u samples at lod %u from (%d,%d,%d) seed %u — "
              "%zu bytes, %.0f ms -> %s\n",
              req.nx, req.ny, req.nz, req.lod, req.ox, req.oy, req.oz, req.seed,
              buf.size(), (NowSeconds() - t0) * 1000.0, outPath.c_str());
  return 0;
}

int RunVoxServe(GpuContext& ctx, World& world, Simulation& sim,
                const std::vector<MaterialDef>& mats) {
  // The boot log has already gone to stdout by the time we get here, so the
  // client is told exactly where the protocol starts. Everything after this
  // line on stdout is one ack per request, and nothing else — diagnostics go
  // to stderr.
  std::printf("VOXSERVE READY %u %u\n", kVoxRegionVersion, kWorldN);
  std::fflush(stdout);

  std::vector<MaterialDef> live = mats;
  // LAYER's selection (voxregion.h): "@map" = the map's editLayer, "-" = none,
  // else a layer name. Re-applied before every REGION: the load is stat-cached,
  // and a RELOAD or SWATCH (both re-read the environment, which re-reads the
  // map's layer) must not silently change which layer the viewer is shown.
  std::string layerSel = "@map";
  auto applyLayer = [&]() {
    if (layerSel == "@map") LoadWorldEditLayerForMap(AssetDir());
    else LoadWorldEditLayerNamed(AssetDir(), layerSel == "-" ? std::string() : layerSel);
  };
  std::string line;
  while (std::getline(std::cin, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line.empty()) continue;
    const double t0 = NowSeconds();
    // Split on spaces, REMEMBERING where each token began.
    //
    // The trailing path is taken as the whole REMAINDER of the line, not as a
    // token: this repo's own checkout is "…/3d sand voxel", so a path token
    // would end at "3d" and every request would write a file nobody reads and
    // report success. Paths contain spaces; a space-delimited protocol has to
    // say where the delimiting stops.
    std::vector<std::string> tok;
    std::vector<size_t> tokAt;
    {
      size_t p = 0;
      while (p < line.size()) {
        while (p < line.size() && line[p] == ' ') p++;
        if (p >= line.size()) break;
        size_t q = line.find(' ', p);
        if (q == std::string::npos) q = line.size();
        tokAt.push_back(p);
        tok.push_back(line.substr(p, q - p));
        p = q;
      }
    }
    // The rest of the line from token `i` on, trimmed of trailing spaces.
    auto tail = [&](size_t i) {
      if (i >= tokAt.size()) return std::string();
      std::string s = line.substr(tokAt[i]);
      while (!s.empty() && s.back() == ' ') s.pop_back();
      return s;
    };
    if (tok.empty()) continue;
    const std::string& cmd = tok[0];
    auto ok = [&](size_t bytes) {
      std::printf("OK %zu %.0f\n", bytes, (NowSeconds() - t0) * 1000.0);
      std::fflush(stdout);
    };
    auto fail = [&](const std::string& m) {
      std::printf("ERR %s\n", m.c_str());
      std::fflush(stdout);
    };

    if (cmd == "QUIT") return 0;
    if (cmd == "PING") { ok(0); continue; }
    if (cmd == "RELOAD") {
      std::string msg;
      if (!ReloadTuningAndShaders(ctx, sim, msg)) { fail(msg); continue; }
      // Materials too: a colour edit has to reach the palette, and a material
      // added or removed would otherwise shift every id the viewer draws with.
      {
        std::vector<MaterialDef> nm;
        std::vector<ReactionGpu> nr;
        std::string errs;
        if (LoadAssets(AssetDir() + "/materials/materials.json",
                       AssetDir() + "/materials/reactions.json", nm, nr, errs)) {
          live = std::move(nm);
          sim.UploadTables(ctx.queue, live, nr);
        }
      }
      // And the environment (docs/PLAN_environment_truth.md P-A): the biome
      // files, the painted map and the tree atlas are most of what the
      // terrain view exists to show. A RELOAD that kept the boot copies drew
      // the biome you had, not the one you just saved.
      {
        biomes::EnvironmentStamp stamp;
        std::string elog;
        if (!ReloadEnvironment(ctx, sim, live, stamp, elog)) {
          std::fprintf(stderr, "%s", elog.c_str());
          fail("environment reload failed (kept old tables): " + elog);
          continue;
        }
        std::fprintf(stderr, "%s\n", stamp.Line().c_str());
      }
      ok(0);
      continue;
    }
    if (cmd == "PALETTE") {
      if (tok.size() < 2) { fail("PALETTE wants a path"); continue; }
      const std::string pal = VoxPaletteJson(live);
      std::string err;
      if (!WriteFileBytes(tail(1), pal.data(), pal.size(), err)) {
        fail(err);
        continue;
      }
      ok(pal.size());
      continue;
    }
    if (cmd == "REGION") {
      if (tok.size() < 9) {
        fail("REGION wants ox oy oz nx ny nz lod seed path");
        continue;
      }
      VoxRegionReq req;
      std::vector<long long> v;
      for (int i = 1; i <= 8; i++) v.push_back(std::strtoll(tok[i].c_str(), nullptr, 10));
      ParseReq(v, req);
      applyLayer();
      std::vector<uint8_t> buf;
      std::string err;
      if (!BuildVoxRegion(ctx, world, sim, req, buf, err)) { fail(err); continue; }
      if (!WriteFileBytes(tail(9), buf.data(), buf.size(), err)) {
        fail(err);
        continue;
      }
      ok(buf.size());
      continue;
    }
    if (cmd == "LAYER") {
      if (tok.size() < 2) { fail("LAYER wants @map, - or a layer name"); continue; }
      layerSel = tok[1];
      applyLayer();
      ok(WorldEditLayer().VoxelCount());
      continue;
    }
    if (cmd == "EDITS") {
      // EDITS <RESOLVE|RELATIVE> <seed> <in>|<out>
      std::string in, outp;
      if (tok.size() < 4 || !SplitPaths(tail(3), in, outp)) {
        fail("EDITS wants RESOLVE|RELATIVE <seed> <in>|<out>");
        continue;
      }
      const uint32_t seed = (uint32_t)std::strtoul(tok[2].c_str(), nullptr, 10);
      WorldEdits layer;
      std::string err;
      if (!layer.Load(in, err)) { fail(err); continue; }
      layer.Resolve(seed);
      std::vector<WorldEdits::AbsCell> cells;
      layer.ResolvedCells(cells);
      bool wrote = false;
      if (tok[1] == "RESOLVE") wrote = WorldEdits::WriteAbsolute(outp, cells, seed, err);
      else if (tok[1] == "RELATIVE") wrote = WorldEdits::WriteRelative(outp, cells, seed, err);
      else err = "EDITS wants RESOLVE or RELATIVE, got '" + tok[1] + "'";
      if (!wrote) { fail(err); continue; }
      ok(cells.size());
      continue;
    }
    if (cmd == "EXPORT") {
      std::string dir, outp;
      if (tok.size() < 3 || !SplitPaths(tail(2), dir, outp)) {
        fail("EXPORT wants <seed> <saveDir>|<out>");
        continue;
      }
      std::string report;
      const uint32_t seed = (uint32_t)std::strtoul(tok[1].c_str(), nullptr, 10);
      if (!ExportSaveEdits(ctx, world, sim, live, dir, seed, outp, report)) { fail(report); continue; }
      std::fprintf(stderr, "export: %s\n", report.c_str());
      ok(0);
      continue;
    }
    if (cmd == "SWATCH") {
      std::string json, outp;
      if (tok.size() < 5 || !SplitPaths(tail(4), json, outp)) {
        fail("SWATCH wants <biome> <seed> <sizeVox> <biomeJson|->|<out>");
        continue;
      }
      std::vector<uint8_t> buf;
      std::string err;
      if (!BuildBiomeSwatch(ctx, world, sim, live, tok[1], json,
                            (uint32_t)std::strtoul(tok[2].c_str(), nullptr, 10),
                            (uint32_t)std::strtoul(tok[3].c_str(), nullptr, 10), buf, err)) {
        fail(err);
        continue;
      }
      if (!WriteFileBytes(outp, buf.data(), buf.size(), err)) { fail(err); continue; }
      ok(buf.size());
      continue;
    }
    fail("unknown command '" + cmd + "'");
  }
  return 0;
}

}  // namespace sandvox
