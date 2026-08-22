// selftest_sim.cpp — sim selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/prefab.h"
#include "gpu/resources.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- determinism -------------------------------------------------------
Status GateDeterminism(Ctx& c, std::string& detail) {
  constexpr int kTicks = 200;
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

// determinism: two identical runs must produce identical hash sequences
std::vector<uint32_t> hashes[2];
for (int run = 0; run < 2; run++) {
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  for (uint32_t t = 1; t <= kTicks; t++) {
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t),
               SelftestExps(t, kDefaultSeed), {}, true, {8, 3, 8}, false,
               SelftestParticlesActive(t));
    hashes[run].push_back(ReadHashSync(ctx, world));
  }
}
bool deterministic = hashes[0] == hashes[1];

// The GOLDEN check. Twice-run equality proves the sim reproduces itself; it
// does NOT prove it still simulates the same world. A change that quietly makes
// the sim do less stays perfectly self-consistent and sails through — the
// phase-2b Vulkan-port work found exactly that, a build where the mutate and
// explode passes dispatched ZERO workgroups with the full suite green.
//
// So the final hash is pinned in tests/baseline.json ("determinismHash") and a
// mismatch is a REGRESSION, handled like a known-fail flip: an intentional
// content or sim change updates the recorded value in the same commit. An
// absent key means "not pinned" and only reports — a checkout predating the key
// still behaves as before. See tests/BASELINE.md.
char got[16];
std::snprintf(got, sizeof(got), "%08x", hashes[0].back());
const std::string& golden = GoldenDeterminismHash();
bool goldenOk = golden.empty() || golden == got;

std::printf("determinism: %s (final hash %s over %d ticks%s)\n",
            (deterministic && goldenOk) ? "PASS" : "FAIL", got, kTicks,
            golden.empty() ? ", not pinned"
            : goldenOk     ? ", matches baseline"
                           : "");
if (!deterministic) {
  for (int i = 0; i < kTicks; i++) {
    if (hashes[0][i] != hashes[1][i]) {
      std::printf("  first divergence at tick %d: %08x vs %08x\n", i + 1,
                  hashes[0][i], hashes[1][i]);
      break;
    }
  }
}
if (!goldenOk) {
  std::printf(
      "  GOLDEN HASH MISMATCH: baseline says %s, this build produced %s.\n"
      "  The sim is self-consistent but simulates a DIFFERENT world than the\n"
      "  one recorded. If that was intentional (a material, reaction, tuning\n"
      "  sim.* or kernel change), set \"determinismHash\": \"%s\" in\n"
      "  tests/baseline.json in the SAME commit. If it was not, you have found\n"
      "  a real behaviour change — see tests/BASELINE.md.\n",
      golden.c_str(), got, got);
}

  std::string goldenNote = golden.empty() ? ", golden hash not pinned"
                           : goldenOk     ? ", matches golden"
                                          : ", GOLDEN MISMATCH (baseline " +
                                                golden + ")";
  detail = Format("final hash %s over %d ticks%s", got, kTicks,
                  goldenNote.c_str());

  // Verdict: self-consistent AND simulating the recorded world.
  return (deterministic && goldenOk) ? Status::Pass : Status::Fail;
}

// ---- sleep -------------------------------------------------------------
Status GateSleep(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// sleep: a settled world must go (nearly) fully idle — the M2 exit
// criterion, and the guard against reaction rules that never stop matching.
// Includes an explosion: every ejected particle must reinsert and die.
uint32_t sleepActive = 0;
uint32_t particlesLeft = 0;
int settled = 0;  // tick at which the world went quiet (or the cap)
{
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  uint32_t t = 0;
  // Settle budget is ADAPTIVE: 500 fixed ticks was tuned for the 256^3
  // window; the 512^3 window holds 8x the content (every pond and lava
  // pocket in 32 m of world), and freshly generated liquid legitimately
  // takes longer to equalize. Tick until the world is quiet, hard-capped —
  // the cap is what still catches never-sleeping content (rule 2).
  uint32_t quiet = kNumChunks;
  for (int i = 0; i < 3000; i++) {
    std::vector<ExplosionOp> exps;
    if (i == 30) exps.push_back({110, 76, 110, 12, 350, 0, 0, 0});  // wood slab
    bool pactive = i >= 30 && i < 460;
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, exps, {}, false, {8, 3, 8},
               false, pactive);
    if (i >= 500 && i % 100 == 0) {
      ctx.WaitIdle();
      quiet = ReadActiveChunksSync(ctx, world, sim);
      settled = i;
      if (quiet < 32) break;
    }
  }
  ctx.WaitIdle();
  uint32_t counts[2] = {};
  ReadCountsSync(ctx, world, counts);
  particlesLeft = std::min(counts[sim.Page()], kParticleCap);
  double s0 = NowSeconds();  // settled-world cost: the whole point of dirty dispatch
  for (int i = 0; i < 100; i++)
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false, {8, 3, 8},
               false, false);
  ctx.WaitIdle();
  std::printf("sim settled: %.3f ms/tick\n", (NowSeconds() - s0) * 1000.0 / 100.0);

  rhi::Buffer staging = CreateBuffer(ctx.device, kNumChunks * 4,
                                      rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                      "dirtyRead");
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  enc.CopyBufferToBuffer(sim.DirtyActive(), 0, staging, 0, kNumChunks * 4);
  ctx.queue.Submit(enc.Finish());
  std::vector<uint32_t> awake;
  {
    std::vector<uint32_t> d_((kNumChunks * 4) / 4, 0);
    rhi::ReadBufferBlocking(ctx.device, staging, 0, d_.data(), (size_t)(kNumChunks * 4));
    const uint32_t* d = d_.data();

          for (uint32_t i = 0; i < kNumChunks; i++) {
            if (d[i] != 0) {
              sleepActive++;
              awake.push_back(i);
            }
          }
  }

  // diagnosis on failure: where are the awake chunks, and what's in them?
  if (sleepActive >= 32 && !awake.empty()) {
    for (size_t i = 0; i < awake.size() && i < 12; i++) {
      uint32_t ci = awake[i];
      std::printf("  awake chunk (%u,%u,%u)", ci % kNChunk, (ci / kNChunk) % kNChunk,
                  ci / (kNChunk * kNChunk));
      if (i % 4 == 3) std::printf("\n");
    }
    std::printf("\n");
    // material histogram of the first awake chunks
    rhi::Buffer vstage = CreateBuffer(ctx.device, kChunkVol * 4 * 4,
                                       rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                       "voxRead");
    rhi::CommandEncoder e2 = ctx.device.CreateCommandEncoder();
    for (int k = 0; k < 4 && k < (int)awake.size(); k++)
      e2.CopyBufferToBuffer(world.voxels, (uint64_t)awake[k] * kChunkVol * 4, vstage,
                            (uint64_t)k * kChunkVol * 4, kChunkVol * 4);
    ctx.queue.Submit(e2.Finish());
    uint32_t hist[64] = {};
    {
      std::vector<uint32_t> v_((kChunkVol * 4 * 4) / 4, 0);
      rhi::ReadBufferBlocking(ctx.device, vstage, 0, v_.data(), (size_t)(kChunkVol * 4 * 4));
      const uint32_t* v = v_.data();

            for (uint32_t i = 0; i < kChunkVol * 4; i++) hist[std::min(v[i] & 0xFFFu, 63u)]++;
    }
    std::printf("  first-4-chunk contents:");
    for (uint32_t m = 1; m < 64; m++)
      if (hist[m]) std::printf(" %s=%u", m < mats.size() ? mats[m].name.c_str() : "?", hist[m]);
    std::printf("\n");
  }
}
bool sleepOk = sleepActive < 32 && particlesLeft == 0;
std::printf("sleep: %s (%u / %u chunks active, %u particles alive, quiet "
            "after ~%d settle ticks)\n",
            sleepOk ? "PASS" : "FAIL", sleepActive, kNumChunks, particlesLeft,
            settled);

  // Verdict: the flag the moved body already computed.
  return sleepOk ? Status::Pass : Status::Fail;
}

// ---- pond-freeze -------------------------------------------------------
Status GatePondFreeze(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- pond freezes shore-first, not uniformly -------------------------------
// The water->ice rule is scaled by its count of non-water neighbours
// (reactions.json), so ice appears at the rim first and works inward. That
// SHAPE is the whole point of the rule and it is invisible to the hash test,
// which only proves the sim is reproducible, not that it is right.
//
// What this measures is a RATE gradient, not an absolute gate, and the
// distinction is worth stating because it is easy to write a test that
// asserts the wrong thing (this one did, first time round):
//
//   An open pond's whole surface has AIR above it, so every surface cell
//   counts >= 1 non-water neighbour and CAN freeze. Only water enclosed by
//   water on all six sides is truly gated to zero, which in a real pond
//   means the sub-surface body, not the top face.
//
// So the rim's advantage is that it counts 2-3 (bank + air) against the
// middle's 1, i.e. it freezes 1.6-2.2x faster and the ice front then feeds
// itself inward. The check therefore samples EARLY, while that ratio is
// still visible, and separately asserts the hard gate on a cell that really
// is enclosed: the pond's bottom layer under unfrozen water.
bool pondOk = false;
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int wi = matId("water"), si = matId("stone"), ii = matId("ice");
  // Pin the cycle at midnight so the night-gated rule fires every tick.
  Tuning night = CurrentTuning();
  night.dayNight.freeze = 1;
  night.dayNight.freezePhase = 0;  // 0 = midnight
  Tuning saved = CurrentTuning();
  SetCurrentTuning(night);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A 24x24 pond, 3 deep, walled and floored in stone, with open air above so
  // needsSky passes. Placed inside the window, well clear of terrain. Three
  // deep is what gives a middle layer (y+1) that is enclosed by water above
  // and below — the cells the count-0 gate must forbid outright.
  const int px = 96, py = 120, pz = 96, R = 12, kDepth = 3;
  std::vector<CellOp> pond;
  auto put = [&](int x, int y, int z, int m) {
    uint32_t state = (m == wi) ? 7u : 0u;  // liquids are born full (LIQ_FULL_STATE)
    pond.push_back({World::SlotCellIndex({x, y, z}),
                    (uint32_t)((m & 0xFFF) | (state << 12))});
  };
  for (int z = -R - 1; z <= R + 1; z++)
    for (int x = -R - 1; x <= R + 1; x++) {
      put(px + x, py - 1, pz + z, si);  // floor
      bool rim = (x < -R || x > R || z < -R || z > R);
      for (int y = 0; y < kDepth; y++) put(px + x, py + y, pz + z, rim ? si : wi);
      for (int y = kDepth; y < kDepth + 3; y++) put(px + x, py + y, pz + z, 0);
    }
  uint32_t pt = 1;
  SubmitTick(ctx, world, sim, pt, kDefaultSeed, {}, {}, pond, false,
             {6, 7, 6}, false, false);
  ctx.WaitIdle();

  // Sampled while the rim/middle rate ratio is still visible. Run much
  // longer and the whole surface saturates at 100% ice, which says nothing
  // about the ordering.
  for (uint32_t t = 2; t <= 250; t++)
    SubmitTick(ctx, world, sim, ++pt, kDefaultSeed, {}, {}, {}, false,
               {6, 7, 6}, false, false);
  ctx.WaitIdle();

  // Pull the whole voxel buffer down once — ~1200 sample points, and a
  // blocking map each would be far slower than one copy.
  std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
  {
    const uint64_t bytes = (uint64_t)vox.size() * 4;
    rhi::Buffer st = CreateBuffer(ctx.device, bytes,
                                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                   "pondRead");
    rhi::CommandEncoder e = ctx.device.CreateCommandEncoder();
    e.CopyBufferToBuffer(world.voxels, 0, st, 0, bytes);
    ctx.queue.Submit(e.Finish());
    rhi::ReadBufferBlocking(ctx.device, st, 0, vox.data(), (size_t)(bytes));
  }
  auto readCell = [&](int x, int y, int z) {
    return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };
  // Surface layer (y+kDepth-1): rim ring touches the bank and counts 2-3;
  // the middle 5x5 only has air above and counts 1. Rim must be visibly
  // ahead.
  const int surf = py + kDepth - 1;
  uint32_t edgeIce = 0, edgeN = 0, midIce = 0, midN = 0;
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      bool edge = (x == -R || x == R || z == -R || z == R);
      bool mid = (std::abs(x) <= 2 && std::abs(z) <= 2);
      if (!edge && !mid) continue;
      uint32_t m = readCell(px + x, surf, pz + z);
      if (edge) { edgeN++; if (m == (uint32_t)ii) edgeIce++; }
      else      { midN++;  if (m == (uint32_t)ii) midIce++; }
    }
  // The hard gate, asserted directly on the final state rather than by
  // sampling a rate: NO ice voxel anywhere in the pond may be one that had
  // zero non-water neighbours. Equivalently — since ice only ever replaces
  // water in place — every ice voxel must still touch something that is not
  // water. An ice voxel fully surrounded by water is a voxel that froze at
  // count 0, which the rule forbids outright.
  //
  // This is what makes the check decisive. A rate comparison cannot separate
  // "the gate works and the front crept down from the surface" from "the
  // gate is ignored" — both land near the same percentage. The invariant
  // can: it is violated by exactly one of those two.
  uint32_t violations = 0, iceTotal = 0;
  for (int y = 0; y < kDepth; y++)
    for (int z = -R; z <= R; z++)
      for (int x = -R; x <= R; x++) {
        if (readCell(px + x, py + y, pz + z) != (uint32_t)ii) continue;
        iceTotal++;
        const int d[6][3] = {{0,-1,0},{0,1,0},{1,0,0},{-1,0,0},{0,0,1},{0,0,-1}};
        bool touchesNonWater = false;
        for (auto& o : d)
          if (readCell(px + x + o[0], py + y + o[1], pz + z + o[2]) != (uint32_t)wi)
            touchesNonWater = true;
        if (!touchesNonWater) violations++;
      }
  // Rim ahead of the middle by a clear margin, and no voxel froze in the
  // interior of open water. Both halves matter: the gradient alone would
  // pass a rule that froze everything, the invariant alone would pass one
  // that never fired.
  bool gradient = edgeN > 0 && midN > 0 && edgeIce * midN > 2 * midIce * edgeN;
  pondOk = gradient && violations == 0 && edgeIce > 0;
  std::printf("pond freeze: %s (rim %u/%u vs middle %u/%u ice at 250 night "
              "ticks; %u ice voxels, %u frozen with 0 non-water neighbours)\n",
              pondOk ? "PASS" : "FAIL", edgeIce, edgeN, midIce, midN,
              iceTotal, violations);
  SetCurrentTuning(saved);
}

  // Verdict: the flag the moved body already computed.
  return pondOk ? Status::Pass : Status::Fail;
}

// ---- evaporation -------------------------------------------------------
Status GateEvaporation(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- the sun dries spread water, not bodies of water -----------------------
// The mirror of the pond-freeze test above, and it asserts the thing that
// actually went wrong: evaporation used to be a flat chance on any sunlit
// surface cell, so a pond boiled off from its whole top face in seconds.
// The fix is scaleByNeighbors with minCount 4 — a cell must have >= 4
// non-water face neighbours before the sun can take it.
//
// A rate comparison would be weak here for the same reason it was for
// freezing, so this asserts the two ENDS of the rule as hard invariants on
// the final state:
//
//   1. A pond does NOT shrink. Its surface counts 1 non-water neighbour
//      (the air above), which is below minCount, so after thousands of
//      sunlit ticks every last surface cell must still be water. One
//      missing cell means the gate is not being applied.
//   2. Isolated droplets DO go. A single water voxel sitting on stone
//      counts 5-6 non-water neighbours and must evaporate.
//
// Together they pin the rule from both sides: (1) alone passes a rule that
// never fires, (2) alone passes the old always-fires rule.
bool evapOk = false;
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int wi = matId("water"), si = matId("stone");
  // Pin the cycle at noon so the day-gated rule is at full strength every
  // tick (minLight 120 needs a high sun).
  Tuning noon = CurrentTuning();
  noon.dayNight.freeze = 1;
  noon.dayNight.freezePhase = 32768;  // 32768 = noon
  Tuning saved = CurrentTuning();
  SetCurrentTuning(noon);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // Left: a walled pond, 3 deep, open to the sky. Right: a row of single
  // water voxels on a stone shelf, spaced 3 apart so none touches another
  // (spacing matters — two adjacent droplets would each count a water
  // neighbour and drop toward the gate).
  const int px = 80, py = 120, pz = 96, R = 8, kDepth = 3;
  const int dx = 120, kDrops = 12;
  std::vector<CellOp> scene;
  auto put = [&](int x, int y, int z, int m) {
    uint32_t state = (m == wi) ? 7u : 0u;  // liquids are born full
    scene.push_back({World::SlotCellIndex({x, y, z}),
                     (uint32_t)((m & 0xFFF) | (state << 12))});
  };
  for (int z = -R - 1; z <= R + 1; z++)
    for (int x = -R - 1; x <= R + 1; x++) {
      put(px + x, py - 1, pz + z, si);  // floor
      bool rim = (x < -R || x > R || z < -R || z > R);
      for (int y = 0; y < kDepth; y++) put(px + x, py + y, pz + z, rim ? si : wi);
      for (int y = kDepth; y < kDepth + 3; y++) put(px + x, py + y, pz + z, 0);
    }
  for (int i = 0; i < kDrops; i++) {
    const int x = dx + i * 3;
    put(x, py - 1, pz, si);            // shelf under the droplet
    put(x, py, pz, wi);                // the droplet itself
    for (int y = 1; y < 4; y++) put(x, py + y, pz, 0);  // open sky above
  }
  uint32_t et = 1;
  SubmitTick(ctx, world, sim, et, kDefaultSeed, {}, {}, scene, false,
             {6, 7, 6}, false, false);
  ctx.WaitIdle();

  // Long enough that a droplet at ~2-3 per-mille is overwhelmingly likely to
  // have gone (P(survive) < 1e-3 at 2500 ticks), and long enough that the
  // old flat 2 per-mille rule would have stripped the pond surface many
  // times over.
  for (uint32_t t = 2; t <= 2500; t++)
    SubmitTick(ctx, world, sim, ++et, kDefaultSeed, {}, {}, {}, false,
               {6, 7, 6}, false, false);
  ctx.WaitIdle();

  std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
  {
    const uint64_t bytes = (uint64_t)vox.size() * 4;
    rhi::Buffer st = CreateBuffer(ctx.device, bytes,
                                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                   "evapRead");
    rhi::CommandEncoder e = ctx.device.CreateCommandEncoder();
    e.CopyBufferToBuffer(world.voxels, 0, st, 0, bytes);
    ctx.queue.Submit(e.Finish());
    rhi::ReadBufferBlocking(ctx.device, st, 0, vox.data(), (size_t)(bytes));
  }
  auto readCell = [&](int x, int y, int z) {
    return vox[World::SlotCellIndex({x, y, z})] & 0xFFFu;
  };

  // (1) The pond's surface layer must be intact — every cell still water.
  const int surf = py + kDepth - 1;
  uint32_t surfN = 0, surfWater = 0;
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      surfN++;
      if (readCell(px + x, surf, pz + z) == (uint32_t)wi) surfWater++;
    }
  // (2) The droplets must be gone.
  uint32_t dropsLeft = 0;
  for (int i = 0; i < kDrops; i++)
    if (readCell(dx + i * 3, py, pz) == (uint32_t)wi) dropsLeft++;

  evapOk = surfWater == surfN && dropsLeft == 0;
  std::printf("evaporation: %s (pond surface %u/%u water after 2500 noon "
              "ticks, %u/%d isolated droplets left)\n",
              evapOk ? "PASS" : "FAIL", surfWater, surfN, dropsLeft, kDrops);
  SetCurrentTuning(saved);
}

  // Verdict: the flag the moved body already computed.
  return evapOk ? Status::Pass : Status::Fail;
}

// ---- blood-stain -------------------------------------------------------
Status GateBloodStain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// ---- blood stains what it touches, and then goes to sleep ------------------
// Staining writes the voxel word's spare bits (kStain*, world.h) from the
// liquid movement path in sim_step.wgsl. Three separate things have to hold,
// and none of them is visible to the hash test:
//
//   1. It HAPPENS — stone under a pool of blood ends up carrying a stain of
//      the right type. (A rule that silently never fires still hashes fine.)
//   2. It TERMINATES — the chunk goes back to sleep. This is the rule-2 risk
//      and the one that would not show up until a level is full of gore: a
//      pool of blood sits on stone forever, so a naive "keep me awake while
//      I'm touching something stainable" would pin those chunks awake for
//      the rest of the session. doStaining only holds the chunk while there
//      is UNSTAINED surface left in reach, and this is what proves it.
//   3. It stays BOUNDED — stain amounts saturate at kStainAmtMax rather than
//      overflowing into the neighbouring bits of the word (which would
//      corrupt the tick-stamp and, one bit further, the material id).
bool stainOk = false;
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int bi = matId("blood"), si = matId("stone");
  const uint32_t stainType = mats[bi].gpu.stainPack & kStainPackTypeMask;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A stone basin with blood poured into it, in open air well clear of the
  // terrain. The floor and walls are what should end up stained.
  const int px = 96, py = 120, pz = 96, R = 6;
  std::vector<CellOp> pool;
  auto put = [&](int x, int y, int z, int m) {
    uint32_t state = (m == bi) ? 7u : 0u;  // liquids are born full
    pool.push_back({World::SlotCellIndex({x, y, z}),
                    (uint32_t)((m & 0xFFF) | (state << 12))});
  };
  for (int z = -R - 1; z <= R + 1; z++)
    for (int x = -R - 1; x <= R + 1; x++) {
      put(px + x, py - 1, pz + z, si);  // floor
      bool rim = (x < -R || x > R || z < -R || z > R);
      for (int y = 0; y < 2; y++) put(px + x, py + y, pz + z, rim ? si : bi);
      for (int y = 2; y < 5; y++) put(px + x, py + y, pz + z, 0);
    }
  uint32_t st = 1;
  SubmitTick(ctx, world, sim, st, kDefaultSeed, {}, {}, pool, false,
             {6, 7, 6}, false, false);
  ctx.WaitIdle();
  // Run until the blood has DRIED AWAY, not merely until it has finished
  // staining. Blood carries an unconditional decay rule ("blood dries away",
  // reactions.json) at 8 per-mille, which correctly holds its chunks awake
  // for as long as any blood is left — so a shorter run would measure a pool
  // that is still mid-evaporation and say nothing about sleep.
  //
  // Waiting for the dry-out is the stronger test anyway: it asserts that the
  // STAIN OUTLIVES THE LIQUID. That is the whole point of putting stain in
  // the voxel word rather than deriving it from what is standing there — the
  // mark on the floor has to survive the blood evaporating off it, and it
  // has to do so without keeping the chunk awake.
  // 8 per-mille gives a half-life of ~87 ticks; 4000 is ~46 half-lives.
  const uint32_t kDryTicks = 4000;
  for (uint32_t t = 2; t <= kDryTicks; t++)
    SubmitTick(ctx, world, sim, ++st, kDefaultSeed, {}, {}, {}, false,
               {6, 7, 6}, false, false);
  ctx.WaitIdle();

  std::vector<uint32_t> vox(kNumChunks * (size_t)kChunkVol);
  {
    const uint64_t bytes = (uint64_t)vox.size() * 4;
    rhi::Buffer sb = CreateBuffer(ctx.device, bytes,
                                   rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                   "stainRead");
    rhi::CommandEncoder e = ctx.device.CreateCommandEncoder();
    e.CopyBufferToBuffer(world.voxels, 0, sb, 0, bytes);
    ctx.queue.Submit(e.Finish());
    rhi::ReadBufferBlocking(ctx.device, sb, 0, vox.data(), (size_t)(bytes));
  }

  // Count stained floor voxels, and check every stain in the world is
  // well-formed: right type, amount within the field, and never on air.
  uint32_t stainedFloor = 0, floorN = 0, badStain = 0, consumed = 0;
  for (int z = -R; z <= R; z++)
    for (int x = -R; x <= R; x++) {
      floorN++;
      uint32_t w = vox[World::SlotCellIndex({px + x, py - 1, pz + z})];
      if ((w & 0xFFFu) == 0u) { consumed++; continue; }  // eaten by the stain
      if (VoxStainType(w) == stainType && VoxStainAmt(w) > 0) stainedFloor++;
    }
  for (size_t i = 0; i < vox.size(); i++) {
    uint32_t w = vox[i];
    uint32_t type = VoxStainType(w), amt = VoxStainAmt(w);
    if (type == 0 && amt == 0) continue;
    // A stain must have both halves, name a registered type, fit the field,
    // and sit on actual matter.
    //
    // Water now wets absorbent ground, and worldgen paints ponds on grass and
    // sand by the thousand, so a world-wide scan legitimately sees tens of
    // thousands of "wet" stains. Asserting they were blood reported them as
    // malformed packing — a broken test, not a broken sim. What is actually
    // invariant is that every stain is WELL-FORMED; the blood-specific
    // assertion is `stainedFloor` above, which looks only at the test's floor.
    if (type > kStainTypeMax || amt == 0 || amt > kStainAmtMax ||
        (w & 0xFFFu) == 0u) {
      badStain++;
    }
  }

  // How much blood is left? The sleep assertion only means anything once the
  // pool has actually dried, so report it rather than assuming.
  uint32_t bloodLeft = 0;
  for (size_t i = 0; i < vox.size(); i++)
    if ((vox[i] & 0xFFFu) == (uint32_t)bi) bloodLeft++;

  uint32_t stainActive = ReadActiveChunksSync(ctx, world, sim);
  // Four assertions, each covering a different way this could be broken:
  //   covered   — the stain HAPPENED (a rule that never fires still hashes)
  //   badStain  — every stain is well-formed and none landed on air, so the
  //               packing never overflowed into the stamp or material bits
  //   bloodLeft — the pool really did dry, so the sleep check below is
  //               measuring a settled world and not a mid-reaction one
  //   active    — and the stained floor SLEEPS. This is the rule-2 half: a
  //               stain is permanent, so if it held its chunk awake the way
  //               a naive "am I touching something stainable" rule would,
  //               every gore-soaked chunk would stay awake for the session.
  //
  // bloodLeft is checked against a small threshold, not zero. A handful of
  // isolated voxels can settle in a chunk that then goes to sleep with no
  // neighbour left to wake it, and a sleeping chunk does not run reactions —
  // so their decay is simply paused until something disturbs them. That is
  // the sleep rule working as designed (cost scales with activity), not a
  // stuck reaction, and it is exactly why `active` is the assertion that
  // matters here rather than a demand that every last voxel evaporate.
  bool covered = stainedFloor + consumed > floorN / 2 && stainedFloor > 0;
  const uint32_t kBloodDregs = 16;  // isolated voxels in sleeping chunks
  stainOk = covered && badStain == 0 && bloodLeft <= kBloodDregs &&
            stainActive < 32;
  std::printf("blood stain: %s (%u/%u floor stained, %u consumed, %u malformed, "
              "%u blood left, %u chunks active after %u ticks)\n",
              stainOk ? "PASS" : "FAIL", stainedFloor, floorN, consumed,
              badStain, bloodLeft, stainActive, kDryTicks);
}

  // Verdict: the flag the moved body already computed.
  return stainOk ? Status::Pass : Status::Fail;
}

// ---- flung-liquid ------------------------------------------------------
Status GateFlungLiquid(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
bool fullOk = false;
// ---- flung liquid lands FULL (the anti-gelatin invariant) --------------
// A liquid that rejoins the grid from flight must be born at full fullness,
// exactly like one a brush paints (sim_mutate.wgsl: "liquids are born
// full"). This is a LOOK invariant with no visible symptom in any other
// check: the state nibble is fullness, the renderer builds blood's smooth
// surface from that as a density field, and a spawn that leaves the nibble
// at 0 lands at 1/8 density. The hash still matches, the world still
// settles, nothing fails — the blood just renders as separately shaded
// translucent cubes (the "gelatin" look shadeViscous exists to avoid).
//
// So this asserts the nibble directly rather than trusting a screenshot.
{
  auto matId = [&](const char* n) {
    for (size_t i = 0; i < mats.size(); i++)
      if (mats[i].name == n) return (int)i;
    return -1;
  };
  const int bloodMat = matId("blood");

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A stone slab in open air, and blood particles dropped onto it — the
  // same coordinates and window the stain test uses, which are known to sit
  // inside the residency window with nothing else going on around them.
  const int px = 96, py = 120, pz = 96;
  std::vector<CellOp> slab;
  for (int z = -3; z <= 3; z++)
    for (int x = -3; x <= 3; x++)
      slab.push_back({World::SlotCellIndex({px + x, py, pz + z}),
                      (uint32_t)(matId("stone") & 0xFFF)});

  std::vector<ParticleSpawn> drops;
  for (int i = 0; i < 25 && bloodMat > 0; i++) {
    ParticleSpawn s{};
    s.px = (px - 2 + (i % 5)) * 256 + 128;
    s.py = (py + 6) * 256 + 128;
    s.pz = (pz - 2 + (i / 5)) * 256 + 128;
    s.vx = 0; s.vy = -128; s.vz = 0;
    s.payload = (uint32_t)bloodMat;  // state nibble deliberately left 0
    s.flags = kPFlagAlive;
    drops.push_back(s);
  }
  uint32_t ft = 20000;
  for (int i = 0; i < 40; i++) {
    SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
               i == 0 ? slab : std::vector<CellOp>{}, false, {6, 7, 6},
               /*wantReadback=*/false, /*particlesActive=*/true,
               i == 1 ? drops : std::vector<ParticleSpawn>{});
    ctx.WaitIdle();
    ctx.ProcessEvents();
  }

  std::vector<uint32_t> fv(kNumChunks * (size_t)kChunkVol);
  {
    const uint64_t bytes = (uint64_t)fv.size() * 4;
    rhi::Buffer sb = CreateBuffer(
        ctx.device, bytes,
        rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst, "fullRead");
    rhi::CommandEncoder e = ctx.device.CreateCommandEncoder();
    e.CopyBufferToBuffer(world.voxels, 0, sb, 0, bytes);
    ctx.queue.Submit(e.Finish());
    rhi::ReadBufferBlocking(ctx.device, sb, 0, fv.data(), (size_t)(bytes));
  }
  // Count landed blood and how much of it is at less than full fullness.
  // Blood FLOWS once it lands, and flowing splits a cell's fullness across
  // its neighbours, so partial cells are expected at the spreading edge —
  // the bug being caught is "everything lands at the 1/8 floor", so the
  // assertion is that the maximum reached full, not that every cell did.
  uint32_t landed = 0, maxState = 0;
  for (size_t i = 0; i < fv.size(); i++) {
    if ((fv[i] & 0xFFFu) != (uint32_t)bloodMat) continue;
    landed++;
    maxState = std::max(maxState, (fv[i] >> 12) & 0xFu);
  }
  fullOk = bloodMat > 0 && landed > 0 && maxState == 7;
  std::printf("flung liquid fullness: %s (%u blood voxels landed, max "
              "fullness %u/7 - 0 would render as gelatin cubes)\n",
              fullOk ? "PASS" : "FAIL", landed, maxState);
}

  // Verdict: the flag the moved body already computed.
  return fullOk ? Status::Pass : Status::Fail;
}

// ---- prefab ------------------------------------------------------------
Status GatePrefab(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const std::vector<MaterialDef>& mats = c.mats;
// Milestone A prefab placement: a 48^3 stone cube (110,592 voxels) must
// spread across ticks under the 16384/tick placer budget and land with
// ZERO dropped voxels (exact count via chunk fetches).
bool prefabOk = false;
{
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  Prefab pf;
  pf.name = "testcube";
  PrefabModel pm;
  pm.name = "cube";
  pm.size = {48, 48, 48};
  for (int z = 0; z < 48; z++)
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 48; x++)
        pm.voxels.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                             (uint16_t)kMatStone});
  pf.size = pm.size;
  pf.models.push_back(std::move(pm));

  PrefabPlacer placer;
  IVec3 lo{100, 150, 100}, hi;
  placer.Place(pf, lo, 0, true, mats, lo, hi);
  uint32_t t = 4000;
  int drainTicks = 0;
  while (placer.PendingCount() > 0 && drainTicks < 32) {
    std::vector<CellOp> cellOps;
    placer.PreTick(world, cellOps);
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, cellOps, false,
               {8, 10, 8}, false, false);
    drainTicks++;
  }
  ctx.WaitIdle();

  // count the stamped voxels back out through the async fetch path
  uint32_t placedTick = t;
  uint64_t stone = 0;
  bool allFresh = false;
  for (int tries = 0; tries < 90 && !allFresh; tries++) {
    allFresh = true;
    stone = 0;
    for (int cz = 100 >> 4; cz <= 147 >> 4; cz++)
      for (int cy = 150 >> 4; cy <= 197 >> 4; cy++)
        for (int cx = 100 >> 4; cx <= 147 >> 4; cx++) {
          const CachedChunk* cc = world.Cached({cx, cy, cz});
          if (!cc || cc->version < placedTick) {
            world.RequestChunkFetch({cx, cy, cz});
            allFresh = false;
            continue;
          }
          for (uint32_t w : cc->voxels)
            if ((w & 0xFFFu) == kMatStone) stone++;
        }
    if (!allFresh) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {8, 10, 8}, true, false);
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }
  }
  prefabOk = allFresh && stone == 48ull * 48 * 48 && drainTicks >= 6;
  std::printf("prefab: %s (%llu / %llu voxels landed over %d ticks)\n",
              prefabOk ? "PASS" : "FAIL", (unsigned long long)stone,
              48ull * 48 * 48, drainTicks);
}

  // Verdict: the flag the moved body already computed.
  return prefabOk ? Status::Pass : Status::Fail;
}

// ---- perf --------------------------------------------------------------
Status GatePerf(Ctx& c, std::string& detail) {
  // Advisory. The numbers track kVoxelMeters more than they track correctness,
  // so this reports MARGINAL and never turns the run red (Gate::advisory).
  bool perfOk = c.simMs < 8.0 && c.bestFrameMs < 16.0;
  std::printf("perf: %s\n", perfOk ? "PASS" : "MARGINAL (see numbers above)");
  detail = Format("sim %.2f ms/tick, best frame %.2f ms", c.simMs,
                  c.bestFrameMs);
  // The overall verdict moved to the harness (selftest.cpp), which diffs every
  // gate against tests/baseline.json. The old aggregate AND-ed a hand-kept list
  // of flags that had already drifted out of step with the gates that existed.
  return perfOk ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SimGates() {
  static const std::vector<Gate> g = {
      {"determinism", "sim", {}, false, GateDeterminism},
      {"sleep", "sim", {}, false, GateSleep},
      {"pond-freeze", "sim", {}, false, GatePondFreeze},
      {"evaporation", "sim", {}, false, GateEvaporation},
      {"blood-stain", "sim", {}, false, GateBloodStain},
      {"flung-liquid", "sim", {}, false, GateFlungLiquid},
      {"prefab", "sim", {}, false, GatePrefab},
      {"perf", "sim", {"screenshots"}, true, GatePerf},
  };
  return g;
}

}  // namespace selftest
