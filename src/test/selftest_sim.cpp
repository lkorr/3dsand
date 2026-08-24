// selftest_sim.cpp — sim selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "game/prefab.h"
#include "gpu/resources.h"
#include "test/selftest.h"
#include "test/support.h"

#include "sim/pagetable.h"
#include "sim/stream.h"  // RleEncodeChunk / RleEncodeSentinelChunk (fusion gate)

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
    uint32_t hist[64] = {};
    {
      // Through the CPU seam (§2.1a) — one call per awake slot, since they are
      // arbitrary slot indices rather than a contiguous range.
      std::vector<uint32_t> v_((size_t)kChunkVol * 4, 0);
      for (int k = 0; k < 4 && k < (int)awake.size(); k++)
        ReadVoxelsSync(ctx, world, awake[k], 1,
                       v_.data() + (size_t)k * kChunkVol, "voxRead");
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
    // Through the CPU seam (§2.1a): sentinel slots are synthesized, so the
    // result is a dense-looking snapshot in SLOT order and the
    // World::SlotCellIndex indexing below is right in either residency mode.
    ReadVoxelsSync(ctx, world, 0, kNumChunks, vox.data(), "pondRead");
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
    ReadVoxelsSync(ctx, world, 0, kNumChunks, vox.data(), "evapRead");  // §2.1a
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
    ReadVoxelsSync(ctx, world, 0, kNumChunks, vox.data(), "stainRead");  // §2.1a
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
    ReadVoxelsSync(ctx, world, 0, kNumChunks, fv.data(), "fullRead");  // §2.1a
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

// ---- fluid-det -----------------------------------------------------------
// The MLS-MPM prototype's determinism spike (docs/PLAN_mpm_fluids.md Phase 0,
// run in-engine): drop a block of fluid particles into a stone basin, run the
// solver, and require the ENTIRE particle buffer to hash identically across
// two from-worldgen runs. This is the gate on the plan's central bet — that
// fixed-point integer-atomic P2G accumulation makes a GPU MPM scatter
// scheduling-independent. It also asserts basic physical sanity (the basin
// held the fluid; velocities and J stayed inside the solver's clamps) and
// that the WORLD hash is identical across runs too — the fluid must never
// leak into CA state (it writes no voxels by construction).
Status GateFluidDet(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const int kTicks = 80;
  // Window-local basin placement (slot space via SlotCellIndex, same as the
  // pond gates — each run re-worldgens, so these are stable).
  const int px = 96, py = 120, pz = 96, R = 8, H = 8;

  // The particle's settled identity: what the settle converter writes back as
  // voxels. A zero mat is a DEAD particle to the seam's compaction, so this
  // is load-bearing, not cosmetic.
  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) {
    detail = "no 'water' material";
    return Status::Fail;
  }

  // The spawn block: 4^3 cells x 8 particles on the half-cell lattice with a
  // hash jitter — a pure function of the index, so both runs see identical
  // ops (the twice-run comparison's precondition, like SelftestOps).
  auto detSpawns = [&]() {
    std::vector<FluidSpawnOp> fs;
    for (int cz = -2; cz < 2; cz++)
      for (int cy = 0; cy < 4; cy++)
        for (int cx = -2; cx < 2; cx++)
          for (int s = 0; s < 8; s++) {
            uint32_t h = ((uint32_t)fs.size() * 6271u + 12345u) * 747796405u +
                         2891336453u;
            FluidSpawnOp op{};
            op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                    (int32_t)(h % 8192u) - 4096;
            op.py = ((py + 3 + cy) << 16) + ((s & 2) ? 49152 : 16384) +
                    (int32_t)((h >> 13) % 8192u) - 4096;
            op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384) +
                    (int32_t)((h >> 19) % 8192u) - 4096;
            op.mat = waterId;
            fs.push_back(op);
          }
    return fs;
  };

  uint64_t partHash[2] = {0, 0};
  uint32_t worldHash[2] = {0, 0};
  uint32_t spawned = 0;
  uint32_t live = 0;
  std::vector<uint32_t> last;  // run-2 live particle words, for the sanity sweep
  uint32_t settledEighths = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    std::vector<CellOp> basin;
    auto put = [&](int x, int y, int z, uint32_t m) {
      basin.push_back({World::SlotCellIndex({x, y, z}),
                       (uint32_t)((m & 0xFFFu))});
    };
    for (int z = -R - 1; z <= R + 1; z++)
      for (int x = -R - 1; x <= R + 1; x++) {
        put(px + x, py - 1, pz + z, kMatStone);
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < H; y++)
          put(px + x, py + y, pz + z, rim ? kMatStone : kMatAir);
      }

    uint32_t fluidN = 0;
    uint32_t ft = 30000;
    for (int i = 0; i < kTicks; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i == 1) fs = detSpawns();
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? basin : std::vector<CellOp>{}, false, {6, 7, 6},
                 /*wantReadback=*/false, /*particlesActive=*/false,
                 {}, 0, fs, fluidN);
      fluidN += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
    }
    spawned = fluidN;

    // The live count is GPU-owned now (the seam's compaction — settle may
    // have converted some or all of the pool back to voxels inside the run).
    uint32_t fa[16] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                          64, "fluidDetArgs");
    live = std::min(fa[7], kFluidCap);

    // Hash the LIVE particles only (kFluidParticleWords stride): slots past
    // the live count are compaction leftovers — deterministic garbage within
    // a run but stale across runs, so they must not enter the hash.
    std::vector<uint32_t> buf((size_t)live * kFluidParticleWords);
    if (live > 0 &&
        !rhi::ReadbackBlocking(ctx.device, ctx.queue,
                               world.fluidParticles[sim.Page()], 0, buf.data(),
                               buf.size() * 4, "fluidDet")) {
      detail = "fluid particle readback failed";
      std::printf("fluid det: FAIL (readback failed)\n");
      return Status::Fail;
    }
    uint64_t h = 1469598103934665603ull;  // FNV-1a over the raw words
    for (uint32_t w : buf) {
      h ^= w;
      h *= 1099511628211ull;
    }
    partHash[run] = h ^ ((uint64_t)live << 32);
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);
    if (run == 1) last.swap(buf);

    // Settled water in the basin: mass that left the particle pool through
    // the settle converter. Counted in eighths from the voxel words — the
    // seam's whole claim is that this plus the live pool equals the spawn.
    if (run == 1) {
      settledEighths = 0;
      std::vector<uint32_t> cbuf((size_t)kChunkVol);
      for (int cy = (py - 2) / 16; cy <= (py + H + 8) / 16; cy++)
        for (int cz2 = (pz - R - 2) / 16; cz2 <= (pz + R + 2) / 16; cz2++)
          for (int cx2 = (px - R - 2) / 16; cx2 <= (px + R + 2) / 16; cx2++) {
            uint32_t slot =
                World::SlotChunkIndex({cx2, cy, cz2});
            ReadVoxelsSync(ctx, world, slot, 1, cbuf.data(), "fluidDetVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              int lx = (int)(i % 16) + cx2 * 16, ly = (int)((i / 16) % 16) + cy * 16,
                  lz = (int)(i / 256) + cz2 * 16;
              if (lx < px - R || lx > px + R || lz < pz - R || lz > pz + R ||
                  ly < py || ly > py + H)
                continue;
              uint32_t w = cbuf[i];
              if ((w & 0xFFFu) == waterId)
                settledEighths += ((w >> 12) & 0xFu) + 1u;
            }
          }
    }
  }

  // Physical sanity on the final live pool: the basin held (positions inside
  // the walls, nothing tunneled through the floor), and every particle
  // respects the solver's own clamps. Bounds are deliberately slack — this is
  // "the solver did not explode", not a look test.
  uint32_t escaped = 0, badV = 0, badJ = 0;
  uint32_t liveEighths = 0;
  for (uint32_t i = 0; i < live; i++) {
    const int32_t* p = (const int32_t*)&last[(size_t)i * kFluidParticleWords];
    int x = p[0] >> 16, y = p[1] >> 16, z = p[2] >> 16;
    if (x < px - R - 2 || x > px + R + 2 || y < py - 2 || y > py + H + 8 ||
        z < pz - R - 2 || z > pz + R + 2)
      escaped++;
    for (int a = 3; a < 6; a++)
      if (p[a] < -200000 || p[a] > 200000) { badV++; break; }
    if (p[15] < 30000 || p[15] > 100000) badJ++;
    liveEighths += ((uint32_t)p[18] >> 12) & 0x7u;  // attr fullness
  }

  bool det = partHash[0] == partHash[1];
  bool worldOk = worldHash[0] == worldHash[1];
  // Mass conservation across the seam: every spawned particle is either still
  // live (its fullness eighths) or settled into basin voxels. Exact integer
  // accounting — the seam's core claim.
  bool massOk = liveEighths + settledEighths == spawned;
  bool sane = spawned > 0 && escaped == 0 && badV == 0 && badJ == 0;
  bool ok = det && worldOk && sane && massOk;
  std::printf(
      "fluid det: %s (%u spawned -> %u live + %u settled eighths, %d ticks: "
      "particle hash %016llx %s, world hash %s, %u escaped, %u bad vel, "
      "%u bad J)\n",
      ok ? "PASS" : "FAIL", spawned, live, settledEighths, kTicks,
      (unsigned long long)partHash[0], det ? "matches" : "DIVERGED",
      worldOk ? "matches" : "DIVERGED", escaped, badV, badJ);
  detail = Format("%u spawned, %u live, %u settled, hash %016llx, det %s, "
                  "world %s, mass %s",
                  spawned, live, settledEighths,
                  (unsigned long long)partHash[0], det ? "ok" : "DIVERGED",
                  worldOk ? "ok" : "DIVERGED", massOk ? "ok" : "LOST");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-settle --------------------------------------------------------
// The settle converter in isolation (plan §7, Phase 2): pour MPM water into a
// stone basin, stop, and require the WHOLE pool to convert back to fullness
// voxels within a bounded tick count — zero live particles, zero active fluid
// blocks, and exact integer mass (spawned eighths == basin voxel eighths).
// Twice-run: the end-state world hash must match across two from-worldgen
// runs (the seam is inside the hashed domain now, so this is the determinism
// gate for its writes).
Status GateFluidSettle(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // Pin the cycle at DIM DAWN (daylight ~15 of 255): freezing needs night
  // (day == 0) and evaporation needs minLight 120, so BOTH authored water
  // sinks are off and the mass audit is exact. Noon was the first attempt —
  // it blocked freezing but left evaporation live, and `seesSky` probes only
  // ONE cell up, so a roof does not stop it: this gate caught the sun
  // sipping 2-3 eighths of settled rim film per run before the phase moved
  // here. The seam's own flow counters (binned == settled == died ==
  // poured) were exact throughout — that is the separation this gate's two
  // different checks exist to make.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  const int px = 96, py = 120, pz = 96, R = 8, H = 8;
  const int kMaxTicks = 400;
  uint32_t worldHash[2] = {0, 0};
  uint32_t spawned = 0, live = ~0u, blocks = ~0u;
  int settledAt = -1;
  uint32_t basinEighths = 0;
  uint32_t settledSum = 0, deadSum = 0, excitedSum = 0, binnedSum = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> basin;
    auto put = [&](int x, int y, int z, uint32_t m) {
      basin.push_back({World::SlotCellIndex({x, y, z}), m & 0xFFFu});
    };
    // TWO-cell shell everywhere. MPM's separate-BC nodes live at cell
    // centres; a hard splash can push a particle fractionally through a
    // 1-cell wall at a corner (measured: 2 of 1280 eighths ended up outside
    // the audit box over a 130-tick settle). Two cells of stone is beyond
    // any single-substep reach. The roof also blocks needsSky reactions.
    for (int z = -R - 2; z <= R + 2; z++)
      for (int x = -R - 2; x <= R + 2; x++) {
        put(px + x, py - 1, pz + z, kMatStone);
        put(px + x, py - 2, pz + z, kMatStone);
        put(px + x, py + H, pz + z, kMatStone);
        put(px + x, py + H + 1, pz + z, kMatStone);
        bool rim = (x < -R || x > R || z < -R || z > R);
        for (int y = 0; y < H; y++)
          put(px + x, py + y, pz + z, rim ? kMatStone : kMatAir);
      }
    auto pourOps = [&](uint32_t t) {
      std::vector<FluidSpawnOp> fs;
      for (int cz = -2; cz < 2; cz++)
        for (int cy = 0; cy < 2; cy++)
          for (int cx = -2; cx < 2; cx++)
            for (int s = 0; s < 8; s++) {
              uint32_t h = ((t * 131u + (uint32_t)fs.size()) * 6271u + 12345u) *
                               747796405u + 2891336453u;
              FluidSpawnOp op{};
              op.px = ((px + cx) << 16) + ((s & 1) ? 49152 : 16384) +
                      (int32_t)(h % 8192u) - 4096;
              op.py = ((py + 4 + cy) << 16) + ((s & 2) ? 49152 : 16384) +
                      (int32_t)((h >> 13) % 8192u) - 4096;
              op.pz = ((pz + cz) << 16) + ((s & 4) ? 49152 : 16384) +
                      (int32_t)((h >> 19) % 8192u) - 4096;
              op.vy = -19661;
              op.mat = waterId;
              fs.push_back(op);
            }
      return fs;
    };

    // The interior mass audit, callable mid-run (see the timing probe below).
    // Fills `cells` with every interior water cell's packed (coord, word) so
    // consecutive audits can be diffed to the exact voxel when mass moves.
    auto auditBasin = [&](std::map<uint64_t, uint32_t>* cells) {
      uint32_t eighths = 0;
      if (cells) cells->clear();
      std::vector<uint32_t> cbuf2((size_t)kChunkVol);
      for (int cy = (py - 2) / 16; cy <= (py + H + 8) / 16; cy++)
        for (int cz2 = (pz - R - 2) / 16; cz2 <= (pz + R + 2) / 16; cz2++)
          for (int cx2 = (px - R - 2) / 16; cx2 <= (px + R + 2) / 16; cx2++) {
            ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}),
                           1, cbuf2.data(), "settleVox");
            for (uint32_t i = 0; i < kChunkVol; i++) {
              int lx = (int)(i % 16) + cx2 * 16,
                  ly = (int)((i / 16) % 16) + cy * 16,
                  lz = (int)(i / 256) + cz2 * 16;
              if (lx < px - R || lx > px + R || lz < pz - R || lz > pz + R ||
                  ly < py || ly > py + H)
                continue;
              if ((cbuf2[i] & 0xFFFu) == waterId) {
                eighths += ((cbuf2[i] >> 12) & 0xFu) + 1u;
                if (cells)
                  (*cells)[((uint64_t)lx << 40) | ((uint64_t)ly << 20) |
                           (uint64_t)lz] = cbuf2[i];
              }
            }
          }
      return eighths;
    };

    uint32_t ft = 40000;
    uint32_t liveEst = 0;
    spawned = 0;
    settledAt = -1;
    settledSum = 0;
    deadSum = 0;
    excitedSum = 0;
    binnedSum = 0;
    uint32_t lastCount = 0;
    std::map<uint64_t, uint32_t> prevCells;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<FluidSpawnOp> fs;
      if (i >= 1 && i < 6) fs = pourOps((uint32_t)i);  // 5 ticks x 256
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
                 i == 0 ? basin : std::vector<CellOp>{}, false, {6, 7, 6},
                 false, false, {}, 0, fs, liveEst);
      spawned += (uint32_t)fs.size();
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= 6) {
        // Per-tick: the FA event counters clear at the top of every fluid
        // tick, so mass-flow bookkeeping (settled/dead/excited sums — the
        // audit that localizes any leak) must not skip a tick.
        uint32_t fa[16] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 64, "settleArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        settledSum += fa[10];
        deadSum += fa[8];
        excitedSum += fa[11];
        binnedSum += fa[15];
        liveEst = live;
        if (live == 0 && blocks == 0 && settledAt < 0) {
          settledAt = i;
          if (run == 1) {
            lastCount = auditBasin(&prevCells);
            std::printf("  at quiet (t%d): %u eighths standing\n", i,
                        lastCount);
          }
        }
        // Leak hunt: once quiet, diff the pool cell-by-cell every tick and
        // print the exact voxel transitions whenever the count moves — the
        // mass is provably particle-free at this point, so whatever changes
        // is the CA acting alone.
        if (run == 1 && settledAt >= 0 && i > settledAt) {
          std::map<uint64_t, uint32_t> cells;
          uint32_t n = auditBasin(&cells);
          if (n != lastCount) {
            std::printf("  t%d: %u -> %u eighths; diffs:\n", i, lastCount, n);
            for (auto& [k, w] : prevCells) {
              auto it = cells.find(k);
              uint32_t nw2 = it == cells.end() ? 0u : it->second;
              if (nw2 != w)
                std::printf("    (%d,%d,%d) %08x -> %08x\n",
                            (int)(k >> 40), (int)((k >> 20) & 0xFFFFF),
                            (int)(k & 0xFFFFF), w, nw2);
            }
            for (auto& [k, w] : cells)
              if (!prevCells.count(k))
                std::printf("    (%d,%d,%d) 00000000 -> %08x\n",
                            (int)(k >> 40), (int)((k >> 20) & 0xFFFFF),
                            (int)(k & 0xFFFFF), w);
            lastCount = n;
          }
          prevCells.swap(cells);
        }
        if (settledAt >= 0 && i >= settledAt + 20) break;  // +CA calm margin
      } else {
        liveEst = spawned;  // conservative until the first readback
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // Basin sweep: every spawned eighth must be standing water now.
    basinEighths = auditBasin(nullptr);
  }
  SetCurrentTuning(saved);

  bool settled = settledAt >= 0 && live == 0 && blocks == 0;
  bool massOk = basinEighths == spawned;
  bool det = worldHash[0] == worldHash[1];
  bool ok = settled && massOk && det && spawned > 0;
  std::printf(
      "fluid settle: %s (%u eighths poured -> %u settled voxel eighths, "
      "quiet at tick %d, %u live / %u blocks at end, flow: %u binned / "
      "%u settled / %u died / %u re-excited, world hash %s)\n",
      ok ? "PASS" : "FAIL", spawned, basinEighths, settledAt, live, blocks,
      binnedSum, settledSum, deadSum, excitedSum,
      det ? "matches" : "DIVERGED");
  detail = Format("%u poured, %u settled, quiet@%d, det %s", spawned,
                  basinEighths, settledAt, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-excite --------------------------------------------------------
// The excite converter (plan §7): a SEALED two-chamber stone box — settled
// water on an upper floor, an empty catch chamber below — has its floor plug
// carved out with sim.fluidExciteMode on. The unsupported water must convert
// to MPM particles (hydrostatically pre-compressed: J < 1 in the early
// drain), drain through the hole, and re-settle in the catch chamber, with
// exact mass and a twice-run-identical world hash. Sealed + noon-pinned so no
// authored reaction can eat water out of the audit.
Status GateFluidExcite(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);  // both water sinks off
  t.sim.fluidExciteMode = 1;  // the disturbance trigger under test
  // A drained sealed pool keeps a small persistent fountain near the hole
  // (~1% of particles above 1 vox/s — solver-quality churn, not seam
  // behaviour), and the calm judgement is a per-slot MAX. Relax the calm
  // threshold for this gate so the bulk can settle around the outliers; the
  // wake threshold scales with it to keep the hysteresis gap.
  t.sim.fluidSettleEps = 6.0f;
  t.sim.fluidWakeSpeed = 24.0f;
  // A SEALED box is adversarial for settling: with the default damping of 0
  // the drained pool rings between the walls indefinitely (measured: max
  // particle speed still ~15 vox/s after 200 ticks, against a 0.9 vox/s calm
  // threshold — nothing radiates out of a closed chamber). Real damping is a
  // look knob; the gate turns it up so the drain's END STATE is reachable in
  // a bounded run.
  t.sim.fluidDamping = 0.9f;
  // Softer water for the sealed chamber: stock stiffness (5400 (vox/s)² ->
  // c ≈ 0.41 cells/substep) sits at the CFL edge, and a 3-deep pool's
  // pressurized bottom tips it into sustained numerical churn that no
  // damping can drain (measured: max particle speed pinned at ~0.42
  // cells/tick for 280 ticks with zero external input). 2400 keeps the wave
  // speed comfortably stable so the drained pool can actually calm.
  t.sim.fluidStiffness = 2400.0f;
  // Attraction/cohesion probe: the species-attraction terms subtract pressure
  // in dense regions, which can limit-cycle (clump -> EOS spike -> eject ->
  // re-clump) and keep a sealed pool fizzing above the calm threshold
  // forever. Off for this gate — the seam under test is excite/settle, not
  // the look of the water.
  t.sim.fluidAttractSame = 0.0f;
  t.sim.fluidAttractDiff = 0.0f;
  t.sim.fluidCohesion = 0.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  // fluidDamping is a WGSL const (folded into the kernels at compile time —
  // the sim.fluid* human-unit exception), so unlike the CPU-read knobs above
  // it only takes effect through a shader reload: the F5 path, run here for
  // the same reason F5 exists.
  sim.ReloadShaders(ctx.device);

  // Box: interior x,z in [-6,6] around (96,·,96); DOUBLE outer shell (the
  // settle gate's wall-leak lesson), catch chamber 110..119, upper floor
  // y=120 with a 4x4 plug at the centre, water 121..123 (3 deep, full),
  // double roof 126..127.
  // upperY names the LOWER of the two internal-floor layers (119, 120): a
  // 1-cell internal floor let particles embed in it and quiver forever.
  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 340;
  const uint32_t kWaterEighths = 13u * 13u * 3u * 8u;  // 4056

  uint32_t worldHash[2] = {0, 0};
  uint32_t excitedSum = 0, live = ~0u, blocks = ~0u, endEighths = 0;
  uint32_t compressed = 0, sampled = 0, liveEighths = 0;
  uint32_t setBlocksSum = 0;  // settle picks over the run (refusal-loop probe)
  int32_t endMaxS2 = -1;      // max (v>>8)^2 at end (never-calm probe)
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t m, uint32_t state = 0) {
      box.push_back({World::SlotCellIndex({x, y, z}),
                     (m & 0xFFFu) | (state << 12)});
    };
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) {
            put(px + x, y, pz + z, kMatStone);
          } else if (y >= upperY + 2 && y <= upperY + 4) {
            put(px + x, y, pz + z, waterId, 7u);  // full water
          } else {
            put(px + x, y, pz + z, kMatAir);
          }
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 50000;
    uint32_t liveEst = 0;
    excitedSum = 0;
    compressed = 0;
    sampled = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= kCarveTick) {
        uint32_t fa[16] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 64, "exciteArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        excitedSum += fa[11];
        setBlocksSum += fa[13];
        if (run == 1 && i % 40 == 0)
          std::printf("  excite t%d: live %u, blocks %u, excited+ %u, "
                      "picks+ %u\n", i, live, blocks, fa[11], fa[13]);
        // Exact one-tick-stale count; the exciteMode predicate keeps the
        // seam recording even at zero while the CA is active.
        liveEst = live;
        if (i > kCarveTick + 60 && live == 0 && blocks == 0) break;
        // Hydrostatic check, early in the drain: excited particles seeded
        // below a submerged surface must carry J < 1 (pre-compression).
        if (i == kCarveTick + 2 && live > 0 && run == 1) {
          uint32_t n = std::min(live, 256u);
          std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
          rhi::ReadbackBlocking(ctx.device, ctx.queue,
                                world.fluidParticles[sim.Page()], 0,
                                pbuf.data(), pbuf.size() * 4, "exciteJ");
          for (uint32_t k = 0; k < n; k++) {
            int32_t j = (int32_t)pbuf[k * kFluidParticleWords + 15];
            sampled++;
            if (j < 65536 - 1024) compressed++;
          }
        }
      } else {
        liveEst = 0;
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // End-state particle sweep: the live pool's fullness (for the exact mass
    // audit) plus the residual-churn diagnostics.
    liveEighths = 0;
    if (live > 0) {
      uint32_t n = std::min(live, kFluidCap);
      std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "exciteEndV");
      endMaxS2 = 0;
      uint32_t fast = 0;
      for (uint32_t k = 0; k < n; k++) {
        const int32_t* p = (const int32_t*)&pbuf[k * kFluidParticleWords];
        liveEighths += ((uint32_t)p[18] >> 12) & 0x7u;
        int32_t sx = p[3] >> 8, sy = p[4] >> 8, sz = p[5] >> 8;
        int32_t s2 = sx * sx + sy * sy + sz * sz;
        if (s2 > 49) fast++;
        endMaxS2 = std::max(endMaxS2, s2);
      }
      if (run == 1)
        std::printf("  end: %u live carrying %u eighths, %u above 0.9 vox/s\n",
                    n, liveEighths, fast);
    }

    // Mass audit over the whole sealed interior (both chambers): standing
    // water eighths at the end must equal the eighths placed at the start.
    endEighths = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "exciteVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            int lx = (int)(i % 16) + cx2 * 16,
                ly = (int)((i / 16) % 16) + cy * 16,
                lz = (int)(i / 256) + cz2 * 16;
            if (lx < px - RB + 2 || lx > px + RB - 2 || lz < pz - RB + 2 ||
                lz > pz + RB - 2 || ly <= floorY || ly >= roofY)
              continue;
            if ((cbuf[i] & 0xFFFu) == waterId)
              endEighths += ((cbuf[i] >> 12) & 0xFu) + 1u;
          }
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);  // restore the default-tuning kernels

  // What Phase 2 must prove here: the disturbance excited a real drain with
  // hydrostatic seeding, the mass account is EXACT across every conversion
  // (standing voxels + live fullness == the water placed), MOST of the water
  // made it back to settled voxels, and the whole story is twice-run
  // identical. Full quiescence of a sealed box is deliberately NOT asserted:
  // a ~1% residual fountain near the hole is solver churn (documented in
  // DESIGN.md), and the zero-live end state is already gated by fluid-settle
  // and fluid-det on gentler geometry.
  bool excited = excitedSum > 3000;        // the basin genuinely drained
  bool hydro = sampled > 0 && compressed > 0;
  bool massOk = endEighths + liveEighths == kWaterEighths;
  bool resettled = endEighths > kWaterEighths * 3u / 4u && live < 800;
  bool det = worldHash[0] == worldHash[1];
  bool ok = excited && hydro && massOk && resettled && det;
  std::printf(
      "fluid excite: %s (%u eighths excited over the drain, %u/%u sampled "
      "particles pre-compressed, %u standing + %u live eighths of %u, "
      "%u live / %u blocks at end, %u settle picks, end max s2 %d, "
      "world hash %s)\n",
      ok ? "PASS" : "FAIL", excitedSum, compressed, sampled, endEighths,
      liveEighths, kWaterEighths, live, blocks, setBlocksSum, endMaxS2,
      det ? "matches" : "DIVERGED");
  detail = Format("%u excited, %u/%u compressed, mass %u+%u/%u, det %s",
                  excitedSum, compressed, sampled, endEighths, liveEighths,
                  kWaterEighths, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-stain ---------------------------------------------------------
// Staining parity across the seam (plan §6.2, task 4a): water voxels placed
// CARRYING a foreign stain (type 3 — "blood-water") are excited, drain
// through the box, and must (a) stain the solid surfaces the particles touch
// with that SAME carried type (the CA's own staining would apply water's
// authored "wet" type, so type 3 on a wall can only have come through the
// particle attr word), and (b) re-settle still carrying the stain bits.
// Twice-run world-hash equality as always.
Status GateFluidStain(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }
  const uint32_t kSType = 3u, kSAmt = 12u;

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  t.sim.fluidExciteMode = 1;
  t.sim.fluidDamping = 0.9f;      // the excite gate's sealed-box overrides
  t.sim.fluidStiffness = 2400.0f;
  t.sim.fluidSettleEps = 6.0f;
  t.sim.fluidWakeSpeed = 24.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);

  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 260;
  uint32_t worldHash[2] = {0, 0};
  uint32_t stainedWalls = 0, stainedWater = 0, appliedSum = 0;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t w) {
      box.push_back({World::SlotCellIndex({x, y, z}), w});
    };
    const uint32_t stainedWaterWord =
        waterId | (7u << 12) | (kSAmt << 24) | (kSType << 28);
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y >= upperY + 2 && y <= upperY + 3)
            put(px + x, y, pz + z, stainedWaterWord);
          else put(px + x, y, pz + z, 0u);
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 60000;
    uint32_t liveEst = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= kCarveTick) {
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "stainArgs");
        liveEst = std::min(fa[7], kFluidCap);
        appliedSum += fa[17];  // FA_STAINED
        // Wall sweep MID-DRAIN: the settled pool later WASHES the walls it
        // touches (water's authored `washes: true` rinsing the foreign
        // type — the same behaviour that lets CA water clean blood off
        // stone), so the deposited stains must be observed while the flow
        // is live, not at the washed end state.
        if (i == kCarveTick + 50) {
          stainedWalls = 0;
          std::vector<uint32_t> cb2((size_t)kChunkVol);
          for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
            for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
              for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
                ReadVoxelsSync(ctx, world,
                               World::SlotChunkIndex({cx2, cy, cz2}), 1,
                               cb2.data(), "stainMid");
                for (uint32_t k = 0; k < kChunkVol; k++) {
                  uint32_t w = cb2[k];
                  if (((w >> 28) & 0x7u) == kSType &&
                      ((w >> 24) & 0xFu) != 0 && (w & 0xFFFu) == kMatStone)
                    stainedWalls++;
                }
              }
        }
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    stainedWater = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "stainVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            uint32_t w = cbuf[i];
            if (((w >> 28) & 0x7u) == kSType && ((w >> 24) & 0xFu) != 0 &&
                (w & 0xFFFu) == waterId)
              stainedWater++;
          }
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  bool det = worldHash[0] == worldHash[1];
  // stainedWalls is a STEADY-STATE snapshot: application (~27%/tick/cell)
  // races the pool's wash (~26%/tick/contact), so only a couple of wall
  // cells are visibly stained at any instant. The volume claim lives in
  // appliedSum; the snapshot just proves the bits land on real voxels.
  bool ok = appliedSum >= 50 && stainedWalls >= 1 && stainedWater >= 20 && det;
  std::printf(
      "fluid stain: %s (%u contact stains applied, %u wall cells carried the "
      "excited type mid-drain, %u settled water cells kept it to the end, "
      "world hash %s)\n",
      ok ? "PASS" : "FAIL", appliedSum, stainedWalls, stainedWater,
      det ? "matches" : "DIVERGED");
  detail = Format("%u applied, %u walls, %u water, det %s", appliedSum,
                  stainedWalls, stainedWater, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-react ---------------------------------------------------------
// CA reactions consume EXCITED fluid (plan §6.2, task 4b): plants next to
// water grow into it (`neighborBecomes: plant` — a water-consuming rule).
// A plant bed on the catch floor eats from the drained pool: consumption
// must occur while the water is PARTICLES (the doReactions synthesis + the
// seam's consume flags), and the mass account must stay exact:
// placed == standing water + live fullness + consumed eighths.
Status GateFluidReact(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0, plantId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (c.mats[i].name == "plant") plantId = (uint32_t)i;
  }
  if (waterId == 0 || plantId == 0) {
    detail = "no water/plant material";
    return Status::Fail;
  }

  Tuning t = CurrentTuning();
  t.dayNight.freeze = 1;
  t.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  t.sim.fluidExciteMode = 1;
  t.sim.fluidDamping = 0.9f;
  t.sim.fluidStiffness = 2400.0f;
  t.sim.fluidSettleEps = 6.0f;
  t.sim.fluidWakeSpeed = 24.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);

  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 260;
  const uint32_t kWaterEighths = 13u * 13u * 2u * 8u;  // 2 deep this time
  uint32_t worldHash[2] = {0, 0};
  uint32_t consumedSum = 0, standing = 0, liveEighths = 0, plantsEnd = 0;
  const uint32_t kPlantsStart = 5 * 5;
  for (int run = 0; run < 2; run++) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t w) {
      box.push_back({World::SlotCellIndex({x, y, z}), w});
    };
    for (int y = floorY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                       z >= RB - 1 || y <= floorY || y >= roofY ||
                       y == upperY || y == upperY + 1;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y >= upperY + 2 && y <= upperY + 3)
            put(px + x, y, pz + z, waterId | (7u << 12));
          else if (y == floorY + 1 && std::abs(x) <= 2 && std::abs(z) <= 2)
            put(px + x, y, pz + z, plantId);  // the plant bed (5x5)
          else put(px + x, y, pz + z, 0u);
        }
    std::vector<CellOp> carve;
    for (int z = -2; z < 2; z++)
      for (int x = -2; x < 2; x++) {
        carve.push_back({World::SlotCellIndex({px + x, upperY, pz + z}), 0u});
        carve.push_back(
            {World::SlotCellIndex({px + x, upperY + 1, pz + z}), 0u});
      }

    uint32_t ft = 70000;
    uint32_t liveEst = 0;
    consumedSum = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i >= kCarveTick) {
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "reactArgs");
        liveEst = std::min(fa[7], kFluidCap);
        consumedSum += fa[16];  // FA_CONSUMED
      }
    }
    worldHash[run] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    // Final live fullness for the mass equation.
    liveEighths = 0;
    uint32_t fa[32] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                          128, "reactArgsEnd");
    uint32_t live = std::min(fa[7], kFluidCap);
    if (live > 0) {
      std::vector<uint32_t> pbuf((size_t)live * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "reactEndP");
      for (uint32_t k = 0; k < live; k++)
        liveEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }

    standing = 0;
    plantsEnd = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (floorY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "reactVox");
          for (uint32_t i = 0; i < kChunkVol; i++) {
            int lx = (int)(i % 16) + cx2 * 16,
                ly = (int)((i / 16) % 16) + cy * 16,
                lz = (int)(i / 256) + cz2 * 16;
            if (lx < px - RB + 2 || lx > px + RB - 2 || lz < pz - RB + 2 ||
                lz > pz + RB - 2 || ly <= floorY || ly >= roofY)
              continue;
            uint32_t m = cbuf[i] & 0xFFFu;
            if (m == waterId) standing += ((cbuf[i] >> 12) & 0xFu) + 1u;
            if (m == plantId) plantsEnd++;
          }
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  bool det = worldHash[0] == worldHash[1];
  bool consumed = consumedSum > 0;
  bool grew = plantsEnd > kPlantsStart;
  bool massOk = standing + liveEighths + consumedSum == kWaterEighths;
  bool ok = consumed && grew && massOk && det;
  std::printf(
      "fluid react: %s (%u eighths consumed by reactions, plants %u -> %u, "
      "%u standing + %u live + %u consumed of %u placed, world hash %s)\n",
      ok ? "PASS" : "FAIL", consumedSum, kPlantsStart, plantsEnd, standing,
      liveEighths, consumedSum, kWaterEighths, det ? "matches" : "DIVERGED");
  detail = Format("%u consumed, plants %u->%u, mass %u+%u+%u/%u, det %s",
                  consumedSum, kPlantsStart, plantsEnd, standing, liveEighths,
                  consumedSum, kWaterEighths, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
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


// ---- page-roundtrip (PLAN_page_table.md §4.4 Gate B) ---------------------
//
// THE gate that reads THROUGH the translation rather than around it: every
// other gate goes via ReadVoxelsSync, which synthesizes sentinels so a gate
// testing sim behaviour sees a dense-looking snapshot. This one asserts the
// page table itself.
//
// Anchored to world.WindowOrigin(), never a fixed world position — gates run
// in kOrder sequence and the streaming gate leaves the origin ~20 chunks out,
// which is the documented trap that made the spell gate detonate on tick 1.
Status GatePageRoundtrip(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  const bool paged = world.residency == World::Residency::Paged;
  PageTable& pt = *world.pages;

  // ---- step 0: the SENTINEL RLE FUSION is byte-identical (CPU only) --------
  //
  // RleEncodeSentinelChunk computes the RLE a sentinel chunk would produce
  // WITHOUT materializing its 4,096 words — it fuses SynthWordAt's synthesis
  // loop into RleEncodeChunk's run scan, and (for the EMPTY/UNIFORM shape)
  // skips the loop entirely. That is only legitimate if it is a fusion rather
  // than a second encoding, so assert the equality directly against the
  // definition: synthesize with SynthWordAt, encode with RleEncodeChunk, and
  // demand the same bytes. The save format depends on this (§4.2), and a
  // divergence here would corrupt evicted chunks silently and permanently.
  //
  // Covers all three sentinel shapes and NEGATIVE world-chunk coordinates —
  // the two's-complement bitcast is the documented trap in the jitter formula.
  {
    std::vector<uint32_t> words(kChunkVol), refRle, fusedRle;
    const uint32_t seed = pt.WorldSeed();
    const uint32_t entries[] = {
        kPtEmpty,
        kPtSentinelBit | kMatStone,
        kPtSentinelBit | kPtJitterBit | kMatStone,
        kPtSentinelBit | kPtJitterBit | kMatSand,
    };
    const IVec3 coords[] = {{0, 0, 0}, {3, 5, 7}, {-4, -1, -9}, {130, -7, 22}};
    for (uint32_t e : entries)
      for (const IVec3& wc : coords) {
        const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                  bz = wc.z * (int)kChunk;
        for (uint32_t k = 0; k < kChunkVol; k++)
          words[k] = SynthWordAt(e, bx + (int)(k % kChunk),
                                 by + (int)((k / kChunk) % kChunk),
                                 bz + (int)(k / (kChunk * kChunk)), seed);
        RleEncodeChunk(words.data(), refRle);
        RleEncodeSentinelChunk(e, wc, seed, fusedRle);
        if (refRle != fusedRle) {
          detail = "sentinel RLE fusion diverged: entry " + std::to_string(e) +
                   " at chunk " + std::to_string(wc.x) + "," +
                   std::to_string(wc.y) + "," + std::to_string(wc.z) +
                   " (" + std::to_string(refRle.size()) + " vs " +
                   std::to_string(fusedRle.size()) + " words)";
          return Status::Fail;
        }
      }
  }

  // Standalone (--gate) the world is the untouched identity map: every pool
  // page is claimed by ResetIdentity and the free list is EMPTY, so the paint
  // below would hit §3.8's fatal-exhaustion abort before testing anything.
  // In-suite the previous gates have long since generated and demoted, so
  // this never fires there.
  if (paged && pt.PagesInUse() == pt.PoolPages()) {
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
  }

  // Paint into provably-empty sky, well above any terrain. The window origin
  // is in CHUNK units; +24 chunks of Y from the origin is 384 voxels up.
  const IVec3 o = world.WindowOrigin();
  const IVec3 sky{(o.x + (int)kNChunk / 2) * (int)kChunk + 8,
                  (o.y + 24) * (int)kChunk + 8,
                  (o.z + (int)kNChunk / 2) * (int)kChunk + 8};
  const uint32_t skySlot =
      World::SlotChunkIndex({sky.x >> 4, sky.y >> 4, sky.z >> 4});

  uint32_t t = 900000;  // past any other gate's tick range
  auto tick = [&](const std::vector<BrushOp>& ops) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, ops, {}, {}, false,
               {sky.x >> 4, sky.y >> 4, sky.z >> 4}, true, false);
    ctx.WaitIdle();
  };

  // ---- step 1: ALLOC -----------------------------------------------------
  // The target chunk must start as a sentinel in paged mode: it is open sky.
  const bool wasSentinel =
      (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  const uint32_t before = pt.PagesInUse();
  tick({{sky.x, sky.y, sky.z, 4, kMatSand, 1, 0, 0}});
  const bool nowResident =
      (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) == 0u;
  const uint32_t afterAlloc = pt.PagesInUse();

  // ---- step 2: CONTENT ---------------------------------------------------
  // Read the chunk back THROUGH the translation and assert the paint landed.
  // This is the assertion that a write into a chunk that was a sentinel one
  // tick earlier actually reached memory rather than being no-oped.
  std::vector<uint32_t> chunk(kChunkVol, 0);
  ReadVoxelsSync(ctx, world, skySlot, 1, chunk.data(), "prtRead");
  uint32_t painted = 0;
  for (uint32_t w : chunk)
    if ((w & 0xFFFu) == kMatSand) painted++;

  // ---- step 8: CPU/GPU SYNTHESIS AGREEMENT (§4.4 step 8) -----------------
  // SynthWord (world.h) and synthWord (common.wgsl) are the two halves of one
  // contract — the hash contract. EMPTY's half is the strong one and it is
  // checkable directly: synthWord(PT_EMPTY) must be exactly 0, which is why a
  // materialized EMPTY page is a plain fillBuffer(0).
  const bool synthEmptyZero = SynthWord(kPtEmpty) == 0u;
  bool synthAgrees = true;
  {
    // An untouched sky slot next door is still a sentinel; read it through the
    // seam (which synthesizes CPU-side) and assert every word matches the rule.
    const uint32_t nbrSlot = World::SlotChunkIndex(
        {(sky.x >> 4) + 2, sky.y >> 4, sky.z >> 4});
    std::vector<uint32_t> nbr(kChunkVol, 0);
    ReadVoxelsSync(ctx, world, nbrSlot, 1, nbr.data(), "prtSynth");
    const uint32_t entry = world.PageEntryOfSlot(nbrSlot);
    if ((entry & kPtSentinelBit) != 0u) {
      const uint32_t want = SynthWord(entry);
      for (uint32_t w : nbr)
        if (w != want) { synthAgrees = false; break; }
    }
  }

  // ---- step 3: FREE WITH HYSTERESIS -------------------------------------
  // Erase the ball, then tick past the threshold. The hysteresis is the thing
  // under test, so assert the page did NOT come back before it — a gate that
  // only checked the end state would pass with hysteresis removed.
  tick({{sky.x, sky.y, sky.z, 6, kMatAir, 1, 0, 0}});
  bool heldThroughHysteresis = true;
  const int kFreeTicks = 8;  // mirrors kPageFreeTicks (pagetable.h)
  for (int i = 0; i < kFreeTicks + 6; i++) {
    tick({});
    if (i < kFreeTicks - 2 && paged &&
        (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u)
      heldThroughHysteresis = false;  // freed too early
  }
  const uint32_t afterFree = pt.PagesInUse();
  // The free assertion is SLOT-LEVEL: the painted chunk's entry is a sentinel
  // again once hysteresis has run. It was originally a global count
  // (afterFree <= afterAlloc), which is the wrong invariant on a live world:
  // this gate runs after the spells gate, whose fires are still spreading, so
  // background materialization legitimately outpaces the one freed page and
  // the count RISES while the roundtrip under test works perfectly. The
  // slot-level form is also strictly stronger for the property under test —
  // the global count could pass with this chunk never freed at all, carried
  // by unrelated demotions. (Same class as gotcha "a world-wide sweep must
  // assert invariants, not that every stain is blood".)
  //
  // The wait is BOUNDED, not fixed: free probes are capped per tick
  // (kMaxFreeProbesPerTick), so on a live world this chunk queues behind
  // whatever demotion backlog the previous gates left, and a fixed 6-tick
  // grace reads a working mechanism as a leak. The early-free assertion
  // above already pinned the hysteresis floor; this loop just gives the
  // capped drain time to reach our slot.
  bool freedAtEnd = (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  for (int i = 0; i < 400 && paged && !freedAtEnd; i++) {
    tick({});
    freedAtEnd = (world.PageEntryOfSlot(skySlot) & kPtSentinelBit) != 0u;
  }

  // ---- step 7: pageFaults == 0 ------------------------------------------
  // The assertion that turns §2.4's structural claim into evidence. Read
  // directly rather than via the snapshot so it covers this gate's own ticks.
  uint32_t faults = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, &faults, 4,
                        "prtFaults");

  // Verdict. In DENSE mode there are no sentinels by construction, so the
  // alloc/free assertions are vacuous and only the content, synthesis and
  // fault assertions apply — which is right: dense is the oracle, and what it
  // must prove is that the translation path produces the same voxels.
  const bool allocOk =
      !paged || (wasSentinel && nowResident && afterAlloc > before);
  const bool freeOk = !paged || freedAtEnd;
  const bool ok = allocOk && freeOk && painted > 0 && faults == 0 &&
                  synthEmptyZero && synthAgrees && heldThroughHysteresis;

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "%s: sky slot %u sentinel->resident %d->%d, pages %u->%u->%u, "
                "%u sand voxels painted through the table, hysteresis held=%d "
                "freed at end=%d, synthWord(EMPTY)==0 %d, CPU/GPU synth agree "
                "%d, pageFaults %u",
                paged ? "paged" : "dense", skySlot, (int)wasSentinel,
                (int)nowResident, before, afterAlloc, afterFree, painted,
                (int)heldThroughHysteresis, (int)(!paged || freedAtEnd),
                (int)synthEmptyZero, (int)synthAgrees, faults);
  detail = buf;
  return ok ? Status::Pass : Status::Fail;
}

// ---- daylight-boundary, Gate D (PLAN_page_table.md §3.2a / §4.4) ---------
//
// THE SUITE STRUCTURALLY CANNOT DO WITHOUT THIS, and it is new value even
// without paging: Simulation::EncodeWakeAll has ZERO test coverage today.
// Every day/night gate PINS the phase (dayNight.freeze = 1 at midnight and at
// noon), so wasDay != isDay is never true anywhere in the suite and the
// wake-all path — which sets all 32,768 dirty flags — has never once run under
// test.
//
// It is also the case that decides whether §3.8's fatal abort is reachable in
// normal play: without the "intersect nonSentinel" filter on the
// materialization set, a wake-all would demand 32,768 pages from an
// 8,192-page pool and crash TWICE PER IN-GAME DAY.
Status GateDaylightBoundary(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  PageTable& pt = *world.pages;
  const bool paged = world.residency == World::Residency::Paged;

  // Run the cycle UNFROZEN — the whole point — and short, so a boundary is
  // guaranteed inside the tick budget.
  const Tuning saved = CurrentTuning();
  Tuning tun = saved;
  tun.dayNight.freeze = 0;
  tun.dayNight.cycleMinutes = 1;   // 1800 ticks per day at 30 Hz
  SetCurrentTuning(tun);

  const uint32_t tpd = TicksPerDay(tun);
  uint32_t t = 800000;
  uint32_t crossings = 0;
  uint32_t peakPages = pt.PagesInUse();
  bool everExhausted = false;

  bool prevDay = DaylightStrengthCpu(DayPhaseForTick(t, tpd, false, 0)) > 0;
  for (uint32_t i = 0; i < tpd; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
               {8, 3, 8}, true, false);
    const bool isDay =
        DaylightStrengthCpu(DayPhaseForTick(t, tpd, false, 0)) > 0;
    if (isDay != prevDay) crossings++;
    prevDay = isDay;
    peakPages = std::max(peakPages, pt.PagesInUse());
    // Only meaningful in PAGED mode: dense is the identity map, so
    // pagesInUse_ == PoolPages() by construction and always has been.
    if (paged && pt.PagesInUse() >= pt.PoolPages()) everExhausted = true;
    if (crossings >= 1 && i > 64) break;   // a boundary plus settle time
  }
  ctx.WaitIdle();

  // (a) the hash matches a dense run — carried by the suite running in both
  //     residency modes rather than re-derived here.
  // (b) pagesInUse_ < kPoolPages THROUGHOUT: the abort must stay unreachable.
  // (c) pageFaults == 0: nothing the wake made dirty was written unmaterialized.
  // (d) chunks return to sleep afterwards.
  uint32_t faults = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, &faults, 4,
                        "dayFaults");
  const uint32_t active = ReadActiveChunksSync(ctx, world, sim);
  const uint32_t hash = HashWorldNow(ctx, world, sim, kDefaultSeed);

  SetCurrentTuning(saved);

  const bool ok = crossings >= 1 && !everExhausted && faults == 0;
  char buf[400];
  std::snprintf(buf, sizeof(buf),
                "%s: %u daylight crossing(s) with freeze OFF (EncodeWakeAll "
                "fired), peak pages %u / %u, exhausted=%d, pageFaults %u, "
                "%u chunks active after, hash %08x",
                paged ? "paged" : "dense",
                crossings, peakPages, pt.PoolPages(), (int)everExhausted,
                faults, active, hash);
  detail = buf;
  return ok ? Status::Pass : Status::Fail;
}

// ---- celestial: the orbital simulation behind the sky --------------------
//
// A screenshot cannot tell a Keplerian solve from a phase ramp that happens to
// look plausible, so this asserts the PROPERTIES that separate them. Each one
// fails under a specific plausible bug rather than under "it looks wrong":
//
//  (a) The day closes. Sun elevation at t and at t + one solar day must match.
//      Fails if the sidereal/solar distinction is inverted — the classic bug,
//      and one that drifts a whole hour per game-year, i.e. invisibly at first.
//  (b) Seasons exist and have the RIGHT AMPLITUDE. Noon elevation must swing
//      by ~2x the axial tilt across the year, and peak at 90 - |lat - tilt|.
//      A ramp with a tilt knob wired to nothing passes any "does it move"
//      test; only the amplitude pins it to real geometry.
//  (c) The moons hit their AUTHORED SYNODIC periods. celestial.cpp converts
//      synodic -> sidereal before integrating; if that conversion is dropped
//      an "8-day moon" runs at 8.7 days and nobody notices for a week.
//  (d) The 8/9 periods are coprime: the PAIR of phases must not repeat inside
//      72 days. This is the whole reason for the second moon's period choice.
//  (e) Eclipses happen, and are RARE. Both halves matter: inclination wired to
//      zero gives one a month, and a sign error in the node gives none ever.
//  (f) Purity. The same tick must produce a bit-identical SkyState, and the
//      solve must never produce a NaN — including at negative ticks, which the
//      reverse-time slider generates.
//  (g) The clock's identity property, which is what protects the pinned hash:
//      a disengaged CelestialClock must return the sim tick unchanged.
//
// CPU-only: no GPU work, no world, runs in milliseconds.
Status GateCelestial(Ctx& c, std::string& detail) {
  (void)c;
  const Tuning saved = CurrentTuning();
  Tuning tun = saved;
  tun.dayNight.freeze = 0;
  SetCurrentTuning(tun);

  const Tuning::DayNight& d = tun.dayNight;
  const double tpd = (double)TicksPerDay(tun);
  const double kRad = 57.2957795130823;
  auto elevDeg = [&](const SkyState& s) {
    return std::asin(std::max(-1.0f, std::min(1.0f, s.sunDir[1]))) * kRad;
  };

  std::string fail;
  auto require = [&](bool cond, const char* what) {
    if (!cond && fail.empty()) fail = what;
  };

  // (a) THE SOLAR DAY CLOSES ON ITSELF. This is the sidereal/solar test, and
  // it is measured as the drift of the sun's AZIMUTH at a fixed time of day,
  // not of its elevation.
  //
  // Elevation is the wrong probe and the first version of this test used it:
  // one day of orbital motion legitimately moves the sun's declination (~1.5
  // deg/day at the default 96-day year), and near the horizon the elevation
  // response to that is amplified several-fold — 6 deg of perfectly correct
  // seasonal drift, indistinguishable from the bug. AZIMUTH at a fixed hour is
  // fixed by the ROTATION alone, so it isolates exactly the question being
  // asked: if the sidereal and solar rates were swapped the sun would land
  // ~3.75 deg further round the compass each day (a full turn per year) and
  // this fails on day one.
  auto azDeg = [&](const SkyState& s) {
    return std::atan2((double)s.sunDir[0], (double)s.sunDir[2]) * kRad;
  };
  // The residual that survives is the EQUATION OF TIME: on a tilted, eccentric
  // orbit the sun's right ascension does not advance uniformly, so apparent
  // solar noon oscillates about mean solar noon across the year. That is real
  // physics, it is bounded and PERIODIC (it returns to zero), and the test has
  // to distinguish it from a rate error, which is unbounded and accumulates.
  //
  // So the assertion is on the ACCUMULATED drift over a whole year, not on the
  // day-to-day step: sum the signed daily azimuth changes and require the
  // total to come back to ~0. A sidereal/solar swap accumulates a full 360;
  // the equation of time cancels to within a degree.
  double dayCloseErr = 0.0, accum = 0.0;
  const int closeDays = std::max(2, (int)d.yearLengthDays);
  double aPrev = azDeg(ComputeSky(tun, 0.5 * tpd));
  for (int k = 1; k <= closeDays; k++) {
    const double a = azDeg(ComputeSky(tun, (k + 0.5) * tpd));
    const double diff = std::fmod(a - aPrev + 540.0, 360.0) - 180.0;
    accum += diff;
    dayCloseErr = std::max(dayCloseErr, std::fabs(diff));
    aPrev = a;
  }
  require(std::fabs(accum) < 2.0,
          "the solar year does not close (sidereal/solar rate error)");
  // The equation of time itself is bounded: on a 23.4-degree, e=0.017 orbit
  // its full swing is about 30 minutes of hour angle = ~8 degrees, so no
  // single day may step more than that. This catches a discontinuity (a
  // wrapped angle, a branch cut) that the accumulated sum would cancel out.
  require(dayCloseErr < 10.0, "apparent noon jumps by more than the equation of time allows");

  // (a2) MEAN SOLAR NOON IS dayT == 0.5. This is the tie between the float sky
  // and the sim's INTEGER day phase: DayPhaseForTick maps tick -> phase with
  // 0x8000 = noon, daylightStrength() peaks there, and the reactions gated on
  // it must fire when the sun the player can see is actually up. If the sun's
  // peak drifted off 0.5, water would evaporate in the dark.
  //
  // Tolerance is 0.02 of a day (~17 min at the default cycle), which covers
  // the equation of time (the apparent-vs-mean noon oscillation an eccentric
  // tilted orbit genuinely has) and nothing else.
  double worstNoon = 0.0;
  for (int day = 0; day < 24; day++) {
    double bestT = 0.0, bestE = -1e9;
    for (int q = 0; q < 480; q++) {
      const double frac = q / 480.0;
      const double e = elevDeg(ComputeSky(tun, (day * 4 + frac) * tpd));
      if (e > bestE) { bestE = e; bestT = frac; }
    }
    worstNoon = std::max(worstNoon, std::fabs(bestT - 0.5));
  }
  require(worstNoon < 0.02, "the sun does not peak at dayT 0.5 (sky and sim day phase disagree)");

  // (b) seasons: amplitude and peak.
  double noonLo = 1e9, noonHi = -1e9;
  const int yearDays = (int)d.yearLengthDays;
  for (int day = 0; day < yearDays; day++) {
    // Sample the whole day and keep its maximum: "noon" drifts with the
    // equation of time, so sampling at exactly 0.5 would measure that drift
    // rather than the season.
    double best = -1e9;
    for (int q = 0; q < 24; q++)
      best = std::max(best, elevDeg(ComputeSky(tun, (day + q / 24.0) * tpd)));
    noonLo = std::min(noonLo, best);
    noonHi = std::max(noonHi, best);
  }
  const double swing = noonHi - noonLo;
  const double wantPeak = 90.0 - std::fabs((double)d.latitudeDeg -
                                           (double)d.axialTilt);
  require(std::fabs(swing - 2.0 * d.axialTilt) < 3.0,
          "seasonal swing is not 2x the axial tilt");
  require(std::fabs(noonHi - wantPeak) < 3.0,
          "solstice noon elevation is not 90 - |lat - tilt|");

  // (c) synodic periods. Count full moons (phase maxima at 0.5) over a long
  // span and divide — robust to where in the cycle the epoch happens to fall.
  // Detected as LOCAL MAXIMA of the phase, not as a threshold crossing. An
  // inclined orbit never reaches exact opposition — moon A at 5.1 degrees tops
  // out around phase 0.486, never 0.5 — so a "phase >= 0.4995" test finds no
  // full moons at all and reports a period of zero. (It did, on the first run.)
  auto synodic = [&](bool second) {
    int fulls = 0;
    double first = -1.0, last = -1.0;
    float p0 = 0.0f, p1 = 0.0f;
    const int span = 400;  // days
    for (int k = 0; k < span * 240; k++) {
      const double t = k * (tpd / 240.0);
      const SkyState s = ComputeSky(tun, t);
      const float p2 = second ? s.moon2Phase : s.moonPhase;
      // A strict interior maximum, and only in the gibbous half — the phase
      // curve has a matching minimum at new moon, and counting both would
      // halve the reported period.
      if (k >= 2 && p1 > p0 && p1 >= p2 && p1 > 0.40f) {
        const double tf = (t - tpd / 240.0) / tpd;
        if (first < 0.0) first = tf;
        last = tf;
        fulls++;
      }
      p0 = p1;
      p1 = p2;
    }
    return fulls > 1 ? (last - first) / (fulls - 1) : 0.0;
  };
  const double synA = synodic(false), synB = synodic(true);
  require(std::fabs(synA - (double)d.lunarPeriodDays) < 0.35,
          "moon A does not hit its authored synodic period");
  require(std::fabs(synB - (double)d.moon2PeriodDays) < 0.35,
          "moon B does not hit its authored synodic period");

  // (d) the phase PAIR must not repeat before the 72-day grand cycle. Compare
  // day 0's pair against every later day; the closest match inside the cycle
  // must be meaningfully far from an exact repeat.
  const SkyState s0 = ComputeSky(tun, 0.0);
  double nearestPair = 1e9;
  int nearestDay = 0;
  for (int day = 1; day < 71; day++) {
    const SkyState s = ComputeSky(tun, day * tpd);
    const double dp = std::fabs(s.moonPhase - s0.moonPhase) +
                      std::fabs(s.moon2Phase - s0.moon2Phase);
    if (dp < nearestPair) { nearestPair = dp; nearestDay = day; }
  }
  require(nearestPair > 0.02,
          "the two moons' phase pair repeats inside 72 days (periods not coprime?)");

  // (e) eclipses: they happen, and they are rare. Sampled every 4 ticks over
  // 400 game-days, which is fine enough not to step over totality (a total
  // eclipse lasts minutes of game time, i.e. hundreds of ticks).
  long eclSamples = 0, samples = 0;
  double maxCover = 0.0;
  for (long k = 0; k < (long)(400.0 * tpd); k += 4) {
    const SkyState s = ComputeSky(tun, (double)k);
    if (s.solarEclipse > 0.0f) {
      eclSamples++;
      maxCover = std::max(maxCover, (double)s.solarEclipse);
    }
    samples++;
  }
  const double eclFrac = samples ? (double)eclSamples / (double)samples : 0.0;
  // Rare, but not impossible: under 2% of all daylit time, and at least one
  // grazing contact in 400 days. A zero here means the geometry never lines
  // up — which is what a node/inclination sign error looks like.
  require(eclSamples > 0, "no solar eclipse in 400 game-days (geometry never aligns)");
  require(eclFrac < 0.02, "solar eclipses are not rare");

  // (f) purity + finiteness, including negative ticks (reverse time).
  bool pure = true, finite = true;
  for (double t : {0.0, 1.0, 12345.0, 987654.0, -12345.0, -987654.5}) {
    const SkyState a = ComputeSky(tun, t);
    const SkyState b = ComputeSky(tun, t);
    if (std::memcmp(&a, &b, sizeof(SkyState)) != 0) pure = false;
    // Checked field by field rather than by walking the struct as an array of
    // floats: SkyState holds a u32 (eclipseBody) and three nested BodyStates,
    // so a flat reinterpret both reads padding and reads that u32 as a float,
    // where a perfectly valid bit pattern can look like a NaN.
    const float scalars[] = {
        a.sunDir[0],   a.sunDir[1],   a.sunDir[2],
        a.moonDir[0],  a.moonDir[1],  a.moonDir[2],
        a.moon2Dir[0], a.moon2Dir[1], a.moon2Dir[2],
        a.dayT,        a.sunUp,       a.starRot,
        a.moonPhase,   a.moon2Phase,
        a.moonAngRadius, a.moon2AngRadius,
        a.moonPhaseSign, a.moon2PhaseSign,
        a.solarEclipse, a.lunarEclipse, a.yearT,
        a.sun.angRadius, a.moonA.angRadius, a.moonB.angRadius,
        a.sun.dist,      a.moonA.dist,      a.moonB.dist,
    };
    for (float v : scalars) {
      if (!std::isfinite(v)) finite = false;
    }
  }
  require(pure, "ComputeSky is not a pure function of the tick");
  require(finite, "ComputeSky produced a non-finite value");

  // (g) the clock's identity property. THIS is what protects the pinned hash:
  // a disengaged clock must hand back the sim tick untouched, and setting the
  // scale to 1.0x must not engage it.
  CelestialClock clk;
  clk.SetScale(1.0f, 500u);
  for (int i = 0; i < 10; i++) clk.Advance();
  const bool identity = !clk.engaged && clk.SimTick(777u) == 777u &&
                        clk.PrevSimTick(777u) == 776u &&
                        clk.RenderTick(777u) == 777.0;
  require(identity, "a disengaged CelestialClock is not the identity map");
  // ...and once engaged it must be exact, including in reverse. At -1x from
  // tick 1000, ten advances must land exactly on 990 with no remainder.
  CelestialClock rev;
  rev.SetScale(-1.0f, 1000u);
  for (int i = 0; i < 10; i++) rev.Advance();
  require(rev.engaged && rev.ticks == 990 && rev.rem == 0,
          "reverse time is not the exact mirror of forward time");
  // A fractional scale must not drift: 0.5x for 100 ticks is exactly 50.
  CelestialClock half;
  half.SetScale(0.5f, 0u);
  for (int i = 0; i < 100; i++) half.Advance();
  require(half.ticks == 50, "fractional time scale drifts");

  // (h) THE CELESTIAL POLE IS THE LATITUDE. The starfield wheels about
  // SkyState::poleDir, and the sun arcs about the same axis implicitly — they
  // agree only if the pole is derived from latitude rather than assumed. The
  // shader used to rotate the stars about a hardcoded vec3(0.28, 0.92, 0):
  // ~18 deg off vertical toward the EAST, which is not where any pole is and
  // ignores latitude, so the stars turned about one axis while the sun tracked
  // another. Nothing caught it, because a wheeling starfield looks plausible
  // whatever it wheels about.
  //
  // Probe the two independent claims: elevation == latitude, and due north
  // (zero east component). Sweep several latitudes, since a constant would
  // pass at exactly one of them by luck.
  double worstPoleElev = 0.0, worstPoleEast = 0.0;
  for (int latI = -80; latI <= 80; latI += 20) {
    Tuning lt = tun;
    lt.dayNight.latitudeDeg = (float)latI;
    lt.dayNight.sunAzimuth = 0.0f;  // yaw rotates the pole with everything else
    SetCurrentTuning(lt);
    const SkyState ps = SkyForTick(lt, 12345u);
    const double elev =
        std::asin(std::max(-1.0, std::min(1.0, (double)ps.poleDir[1]))) * kRad;
    worstPoleElev = std::max(worstPoleElev, std::abs(elev - (double)latI));
    worstPoleEast = std::max(worstPoleEast, std::abs((double)ps.poleDir[0]));
  }
  require(worstPoleElev < 0.01,
          "the celestial pole is not at elevation == latitude");
  require(worstPoleEast < 1e-4, "the celestial pole is not due north");

  // The pole must also be the axis the sky actually turns about: the one
  // direction the spin leaves fixed. Compare it across a half-day of rotation.
  SetCurrentTuning(tun);
  const SkyState p0 = SkyForTick(tun, 0u);
  const SkyState p1 = SkyForTick(tun, (uint32_t)(tpd / 2.0));
  const double poleDrift =
      std::abs((double)p0.poleDir[0] - (double)p1.poleDir[0]) +
      std::abs((double)p0.poleDir[1] - (double)p1.poleDir[1]) +
      std::abs((double)p0.poleDir[2] - (double)p1.poleDir[2]);
  require(poleDrift < 1e-5, "the celestial pole moves as the planet spins");

  SetCurrentTuning(saved);

  char buf[700];
  std::snprintf(buf, sizeof(buf),
                "year closes to %.3f deg (max step %.2f); noon at dayT 0.5 +-%.4f; "
                "season swing %.1f deg (tilt %.1f, "
                "peak %.1f vs %.1f expected); synodic A %.2f / B %.2f days "
                "(authored %d / %d); nearest phase-pair repeat day %d at "
                "%.3f; eclipses %.4f%% of samples, max coverage %.2f; "
                "pole elev err %.4f deg east %.1e drift %.1e; "
                "pure=%d finite=%d clock=%d%s%s",
                accum, dayCloseErr, worstNoon,
                swing, (double)d.axialTilt, noonHi, wantPeak,
                synA, synB, d.lunarPeriodDays, d.moon2PeriodDays,
                nearestDay, nearestPair, eclFrac * 100.0, maxCover,
                worstPoleElev, worstPoleEast, poleDrift,
                (int)pure, (int)finite, (int)identity,
                fail.empty() ? "" : " -- FAILED: ", fail.c_str());
  detail = buf;
  return fail.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SimGates() {
  static const std::vector<Gate> g = {
      {"celestial", "sim", {}, false, GateCelestial},
      {"determinism", "sim", {}, false, GateDeterminism},
      {"sleep", "sim", {}, false, GateSleep},
      {"pond-freeze", "sim", {}, false, GatePondFreeze},
      {"evaporation", "sim", {}, false, GateEvaporation},
      {"blood-stain", "sim", {}, false, GateBloodStain},
      {"flung-liquid", "sim", {}, false, GateFlungLiquid},
      {"fluid-det", "sim", {}, false, GateFluidDet},
      {"fluid-settle", "sim", {}, false, GateFluidSettle},
      {"fluid-excite", "sim", {}, false, GateFluidExcite},
      {"fluid-stain", "sim", {}, false, GateFluidStain},
      {"fluid-react", "sim", {}, false, GateFluidReact},
      {"prefab", "sim", {}, false, GatePrefab},
      {"page-roundtrip", "sim", {}, false, GatePageRoundtrip},
      {"daylight-boundary", "sim", {}, false, GateDaylightBoundary},
      // No draw of its own, but its verdict reads bestFrameMs, which only the
      // screenshots gate sets — so it needs the render path transitively.
      {"perf", "sim", {"screenshots"}, true, GatePerf, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
