// selftest_worldio.cpp — worldio selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "game/brush.h"
#include "game/player.h"
#include "sim/chunkstore.h"
#include "sim/worldio.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- save-load ---------------------------------------------------------
Status GateSaveLoad(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  Stream& stream = c.stream;
// M2 save/load: snapshot at tick 100, diverge 50 ticks, load — the world
// hash must return exactly to the snapshot value (stamp bytes excluded).
bool saveOk = false;
{
  const char* kPath = "selftest_world.svd";
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 3000;
  for (int i = 0; i < 100; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i), {}, {}, false,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();
  uint32_t h1 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  bool saved = SaveWorld(ctx, world, stream, kPath);
  for (int i = 100; i < 150; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, SelftestOps(i), {}, {}, false,
               {8, 3, 8}, false, false);
  ctx.WaitIdle();
  uint32_t hDiverged = HashWorldNow(ctx, world, sim, kDefaultSeed);
  bool loaded = LoadWorld(ctx, world, sim, stream, kPath);
  uint32_t h2 = HashWorldNow(ctx, world, sim, kDefaultSeed);
  saveOk = saved && loaded && h1 == h2 && h1 != hDiverged;
  std::printf("save/load: %s (hash %08x -> diverged %08x -> restored %08x)\n",
              saveOk ? "PASS" : "FAIL", h1, hDiverged, h2);
  stream.Store().Unbind();  // detach before deleting the directory
  std::filesystem::remove_all(kPath);
}

  // Verdict: the flag the moved body already computed.
  return saveOk ? Status::Pass : Status::Fail;
}

// ---- region-store ------------------------------------------------------
Status GateRegionStore(Ctx& c, std::string& detail) {
// region store: RAM must stay bounded past kMaxRamRegions (LRU spill to
// region files) and spilled chunks must read back from disk intact.
bool storeOk = false;
{
  const char* kDir = "selftest_store.svd";
  ChunkStore cs;
  storeOk = cs.BindSave(kDir);
  const size_t kRegions = ChunkStore::kMaxRamRegions + 16;
  for (size_t i = 0; i < kRegions; i++) {
    // one chunk per region: a full-chunk run of a per-region material
    std::vector<uint16_t> rle = {(uint16_t)kChunkVol,
                                 (uint16_t)(kMatStone + (i % 3))};
    cs.Put({(int)i * 16, 0, 0}, std::move(rle));
  }
  size_t ramAfterPuts = cs.Count();
  for (size_t i = 0; i < kRegions && storeOk; i++) {
    const std::vector<uint16_t>* rle = cs.Get({(int)i * 16, 0, 0});
    storeOk = rle && rle->size() == 2 && (*rle)[0] == (uint16_t)kChunkVol &&
              (*rle)[1] == (uint16_t)(kMatStone + (i % 3));
  }
  storeOk = storeOk && ramAfterPuts <= ChunkStore::kMaxRamRegions;
  std::printf("region store: %s (%zu regions written, %zu chunks in RAM "
              "after puts)\n",
              storeOk ? "PASS" : "FAIL", kRegions, ramAfterPuts);
  cs.Unbind();
  std::filesystem::remove_all(kDir);
}

  // Verdict: the flag the moved body already computed.
  return storeOk ? Status::Pass : Status::Fail;
}

// ---- streaming ---------------------------------------------------------
Status GateStreaming(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
  Stream& stream = c.stream;
// M2/M7 streaming: (a) slide the residency window +X across many shifts,
// twice — the per-tick hash sequences must match exactly (streaming +
// procgen are deterministic); (b) edit a far chunk, walk past its eviction,
// walk back, and verify the edit survived the store roundtrip.
bool streamOk = false;
{
  std::vector<uint32_t> shash[2];
  for (int run = 0; run < 2; run++) {
    stream.OnRegen();
    world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    uint32_t t = 5000;
    for (int i = 0; i < 300; i++) {
      IVec3 pc{8 + i / 10, 8, 8};  // one chunk every 10 ticks -> 30 shifts
      stream.Update(pc);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, true, pc,
                 false, false);
      shash[run].push_back(ReadHashSync(ctx, world));
    }
  }
  bool sdet = shash[0] == shash[1];

  // persistence roundtrip (live readbacks so eviction filters see reality)
  stream.OnRegen();
  world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 7000;
  auto tickAt = [&](IVec3 pc, std::vector<BrushOp> ops) {
    stream.Update(pc);
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {}, false, pc,
               true, false);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  };
  const int ballX = 25 * 16 + 8, ballZ = 128;
  int ballH = World::TerrainHeight(ballX, ballZ, kDefaultSeed);
  IVec3 ballCell{ballX, ballH + 1, ballZ};
  IVec3 ballChunk{ballCell.x >> 4, ballCell.y >> 4, ballCell.z >> 4};
  auto walkTo = [&](int fromCx, int toCx) {
    int step = toCx > fromCx ? 1 : -1;
    for (int cx = fromCx; cx != toCx; cx += step)
      for (int k = 0; k < 10; k++) tickAt({cx, 8, 8}, {});
  };
  walkTo(8, 25);
  // glass ball half-buried at the surface (anchored: resting on the ground)
  tickAt({25, 8, 8}, {{ballCell.x, ballCell.y, ballCell.z, 3, kMatGlass, 1, 0, 0}});
  stream.MarkModifiedBox({ballCell.x - 3, ballCell.y - 3, ballCell.z - 3},
                         {ballCell.x + 3, ballCell.y + 3, ballCell.z + 3});
  for (int k = 0; k < 10; k++) tickAt({25, 8, 8}, {});
  walkTo(25, 60);  // ball chunk streams out (origin.x reaches 52 > 25)
  bool evicted = !world.ChunkInWindow(ballChunk);
  walkTo(60, 25);  // and back in
  world.RequestChunkFetch(ballChunk);
  uint32_t glass = 0;
  for (int k = 0; k < 90; k++) {
    tickAt({25, 8, 8}, {});
    const CachedChunk* cc = world.Cached(ballChunk);
    if (cc && cc->version > t - 30 && cc->voxels.size() == kChunkVol) {
      for (uint32_t w : cc->voxels)
        if ((w & 0xFFFu) == kMatGlass) glass++;
      break;
    }
    world.RequestChunkFetch(ballChunk);
  }
  // a REAL player (collision through the async mirror) flies +X far beyond
  // the original 256-box — the literal M2 exit criterion. Catches any
  // leftover fixed-world assumption in the player/collision path (a v0
  // position clamp produced exactly this bug: an invisible wall at x=254).
  bool crossed = false;
  {
    std::vector<uint32_t> classOf;
    for (auto& m : mats) classOf.push_back(m.gpu.klass);
    Player p2;
    p2.fly = true;
    p2.pos = Vec3{140.5f, 110.0f, 140.5f};  // above the tallest hills (~90)
    auto kindAt = [&](IVec3 c) { return world.KindAt(c, classOf); };
    PlayerInput in{};
    in.forward = 1.0f;
    in.sprint = true;
    for (int i = 0; i < 1200 && !crossed; i++) {
      IVec3 pc{ifloor(p2.pos.x) >> 4, ifloor(p2.pos.y) >> 4,
               ifloor(p2.pos.z) >> 4};
      stream.Update(pc);
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, pc,
                 true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      p2.Update(1.0f / 30.0f, in, Vec3{1, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0},
                kindAt);
      crossed = p2.pos.x > 600.0f;
    }
    std::printf("  player flight: %s (reached x=%.0f, window origin.x=%d)\n",
                crossed ? "crossed" : "BLOCKED", (double)p2.pos.x,
                world.WindowOrigin().x);
  }

  streamOk = sdet && evicted && glass > 0 && crossed;
  std::printf("streaming: %s (hash sequences %s over %u shifts, ball chunk "
              "evicted=%d, %u glass voxels after re-entry, player crossed=%d, "
              "store %zu chunks)\n",
              streamOk ? "PASS" : "FAIL", sdet ? "match" : "DIVERGE",
              stream.ShiftCount(), evicted ? 1 : 0, glass, crossed ? 1 : 0,
              stream.Store().Count());
}

  // Verdict: the flag the moved body already computed.
  return streamOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& WorldIoGates() {
  static const std::vector<Gate> g = {
      {"save-load", "worldio", {}, false, GateSaveLoad},
      {"region-store", "worldio", {}, false, GateRegionStore},
      {"streaming", "worldio", {}, false, GateStreaming},
  };
  return g;
}

}  // namespace selftest
