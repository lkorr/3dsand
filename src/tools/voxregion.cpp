#include "tools/voxregion.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "sim/pagetable.h"
#include "sim/tuning.h"
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
      const uint32_t type = d.gpu.stainPack & kStainPackTypeMask;
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
    ctx.queue.WriteBuffer(world.genList, 0, batchSlots.data(), n * 4);
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

    // Read back in maximal runs of consecutive slots. ReadVoxelsSync already
    // coalesces consecutive PAGES inside a run and synthesises sentinels, so
    // this only has to find the slot runs.
    uint32_t k = 0;
    while (k < n) {
      uint32_t run = 1;
      while (k + run < n && batchSlots[k + run] == batchSlots[k] + run) run++;
      scratch.resize((size_t)run * kChunkVol);
      ReadVoxelsSync(ctx, world, batchSlots[k], run, scratch.data(),
                     "voxregion");
      for (uint32_t r = 0; r < run; r++) {
        const uint32_t boxIdx = order[base + k + r];
        const uint32_t cx = boxIdx % cdx, cy = (boxIdx / cdx) % cdy,
                       cz = boxIdx / (cdx * cdy);
        const uint32_t* src = scratch.data() + (size_t)r * kChunkVol;
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
    fail("unknown command '" + cmd + "'");
  }
  return 0;
}

}  // namespace sandvox
