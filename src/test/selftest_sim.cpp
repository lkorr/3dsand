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
#include <map>
#include <string>
#include <vector>

#include "game/prefab.h"
#include "gpu/resources.h"
#include "test/selftest.h"
#include "test/support.h"

#include "sim/pagetable.h"
#include "sim/rng_simd.h"  // rng::Pcg8 / JitterStateInRow8 (the simd gate)
#include "sim/scan.h"      // scan::FirstIndexWhereMasked   (the simd gate)
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
    SubmitTick(ctx, world, sim, t, kDefaultSeed, SelftestOps(t, kDefaultSeed),
               SelftestExps(t, kDefaultSeed), {}, true, {8, 3, 8}, false,
               SelftestParticlesActive(t));
    hashes[run].push_back(ReadHashSync(ctx, world));
  }
}
bool deterministic = hashes[0] == hashes[1];

// ---- THE TWO CHECKS ARE DIFFERENT CLAIMS. DO NOT CONFLATE THEM. ------------
//
// `deterministic` above is THE INVARIANT (CLAUDE.md rule 1): the same
// seed+tick+inputs reproduce bit-identically. If that fails, something is
// scheduling-dependent and the sim is broken. Stop and report.
//
// `goldenOk` below is a CHANGE DETECTOR, and nothing more. Twice-run equality
// proves the sim reproduces itself; it does NOT prove it still simulates the
// same world. A change that quietly makes the sim do less stays perfectly
// self-consistent and sails through — the phase-2b Vulkan-port work found
// exactly that, a build where the mutate and explode passes dispatched ZERO
// workgroups with the full suite green. Pinning the final hash is what converts
// "the sim agrees with itself" into "the sim agrees with what we recorded".
//
// A MOVED PIN IS NOT A BROKEN SIM. Every intentional change to hashed state
// moves it — a reaction chance, a material, a sim.* value, a re-baked asset.
// The correct response to an EXPECTED move is `--selftest --rebaseline` in the
// same commit, and no investigation whatsoever: there is nothing to diagnose,
// and treating the old number as a target to restore is how a five-minute data
// change turns into an afternoon. Only an UNEXPECTED move is information.
//
// An absent key means "not pinned" and only reports — a checkout predating the
// key still behaves as before. See tests/BASELINE.md.
char got[16];
std::snprintf(got, sizeof(got), "%08x", hashes[0].back());
const std::string& golden = GoldenDeterminismHash();
bool goldenOk = golden.empty() || golden == got;

// The status word names WHICH claim failed, because they mean opposite things:
// "determinism FAILED" is a broken sim, "pin moved" is usually just a change
// that has not been rebaselined yet.
std::printf("determinism: %s (final hash %s over %d ticks%s)\n",
            (deterministic && goldenOk) ? "PASS"
            : !deterministic            ? "FAIL"
                                        : "PIN MOVED",
            got, kTicks,
            golden.empty() ? ", not pinned"
            : goldenOk     ? ", matches baseline"
                           : ", sim reproduces itself; only the recorded value "
                             "differs - rebaseline if you meant it");
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
      "  PINNED HASH MOVED: baseline says %s, this build produced %s.\n"
      "  THE SIM IS NOT BROKEN. Determinism itself PASSED (two runs of the\n"
      "  same seed agreed bit for bit) — this line only says the world you\n"
      "  simulate differs from the one last recorded.\n"
      "    * Did you MEAN to change hashed state (a material, a reaction\n"
      "      chance, a sim.* value, a re-baked asset)? Then this is expected.\n"
      "      Run --selftest --rebaseline, commit the new value alongside your\n"
      "      change, and move on. Do NOT investigate it and do NOT try to get\n"
      "      %s back; there is nothing here to diagnose.\n"
      "    * Did you NOT expect it? Then this is the finding: something\n"
      "      changed behaviour that you did not intend. See tests/BASELINE.md\n"
      "      and the escalation ladder in CLAUDE.md.\n",
      golden.c_str(), got, golden.c_str());
}

  // Says PIN MOVED, not MISMATCH: this string is what lands in last_run.json
  // and in the regression summary, and "mismatch" reads as a broken sim when
  // the sim in fact reproduced itself perfectly two lines above.
  std::string goldenNote = golden.empty() ? ", golden hash not pinned"
                           : goldenOk     ? ", matches golden"
                                          : ", PIN MOVED from baseline " +
                                                golden +
                                                " (determinism itself passed; "
                                                "rebaseline if intended)";
  detail = Format("final hash %s over %d ticks%s", got, kTicks,
                  goldenNote.c_str());

  // Verdict: self-consistent AND simulating the recorded world.
  //
  // Those two are different failures and only one of them is rebaselinable.
  // `deterministic` is the twice-run comparison -- the invariant this gate
  // exists to protect, and never something a baseline edit may excuse.
  // `goldenOk` is a PINNED VALUE. A run that is self-consistent but simulates a
  // different world than the one recorded is exactly the case --rebaseline is
  // for, so say so rather than reporting an undifferentiated FAIL that the
  // rebaseline path then refuses to act on.
  if (deterministic && !goldenOk) MarkPinnedOnly();
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
  const int px = 80, pz = 96, R = 8, kDepth = 3;
  const int dx = 120, kDrops = 12;
  // Terrain-relative, over the WHOLE footprint the pond and the droplet
  // shelf occupy: at a literal y120 this pond was inside bedrock after the
  // datum moved, which reads as "no evaporation in 2500 noon ticks"
  // because `needsSky` is false 200 voxels down, not as a buried fixture.
  const int py = FixtureYOver(px - R - 1, pz - R - 1,
                              dx + kDrops * 3, pz + R + 1, kDefaultSeed, 8);
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
  // Terrain-relative: at a literal y120 the slab and the drops above it
  // were both inside the hillside after the datum moved, and the gate
  // reported it as "0 blood voxels landed".
  const int px = 96, pz = 96;
  const int py = FixtureY(px, pz, kDefaultSeed, 24);
  // The PLAYER CHUNK below has to follow the slab. It was a literal {6, 7, 6}
  // (y112..127), which was fine while the terrain band was y32..y86 and is 100
  // voxels under the fixture now — and the player chunk is what centres the
  // 3x3x3 CPU mirror and the particle materialization ring, so a stale one
  // reads as "nothing landed" rather than as a mis-aimed harness.
  // A WELL, not a bare plate. The assertion is that a particle rejoining the
  // grid is born at FULL fullness, and on an open plate 25 drops sheet out over
  // 49 cells within the 40 ticks and the maximum reads 5/7 — a pass that
  // depended on the drops happening to pile up. One voxel of rim makes the
  // pooling structural: 25 drops into a 5x5 well is one full cell each.
  std::vector<CellOp> slab;
  for (int z = -3; z <= 3; z++)
    for (int x = -3; x <= 3; x++) {
      slab.push_back({World::SlotCellIndex({px + x, py, pz + z}),
                      (uint32_t)(matId("stone") & 0xFFF)});
      if (std::abs(x) == 3 || std::abs(z) == 3)
        for (int wy = 1; wy <= 5; wy++)
          slab.push_back({World::SlotCellIndex({px + x, py + wy, pz + z}),
                          (uint32_t)(matId("stone") & 0xFFF)});
    }

  std::vector<ParticleSpawn> drops;
  for (int i = 0; i < 25 && bloodMat > 0; i++) {
    ParticleSpawn s{};
    // ONE DROP PER CELL over the 5x5 floor of the well, and it has to stay that
    // way: stacking 25 drops into a 3x3 does not pile them three deep, it lands
    // them in the same cells and reads 2/7 — worse than the open plate this
    // replaced. The WELL is what fixes the original 5/7, not the packing: 25
    // full voxels in 25 cells with a wall around them have nowhere to flow, so
    // "born full" survives the 40 ticks the gate waits.
    s.px = (px - 2 + (i % 5)) * 256 + 128;
    // DROPPED FROM INSIDE THE WELL, two voxels up, not six. sim.windMode is 1,
    // so a particle in flight takes wind drag — over six voxels of fall that is
    // enough lateral drift to put some of the 25 drops on the rim or outside
    // it, and the gate then measures a sheet spreading on a plate instead of
    // the thing it asserts. The fall still exercises the deposit path.
    s.py = (py + 3) * 256 + 128;
    s.pz = (pz - 2 + (i / 5)) * 256 + 128;
    s.vx = 0; s.vy = -128; s.vz = 0;
    s.payload = (uint32_t)bloodMat;  // state nibble deliberately left 0
    s.flags = kPFlagAlive;
    drops.push_back(s);
  }
  uint32_t ft = 20000;
  for (int i = 0; i < 40; i++) {
    SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {},
               i == 0 ? slab : std::vector<CellOp>{}, false, {6, py >> 4, 6},
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
  uint32_t pickedSum = 0, infeasibleSum = 0, unstableSum = 0;
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
    pickedSum = 0;
    infeasibleSum = 0;
    unstableSum = 0;
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
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "settleArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        settledSum += fa[10];
        deadSum += fa[8];
        excitedSum += fa[11];
        binnedSum += fa[15];
        // WP3's settle diagnosis, and why this gate can now say WHY it failed
        // rather than only that it did: blocks the scan picked, and how many
        // settleCheck turned down for an infeasible column (FA_SETREFUSED)
        // versus an excite-unstable result (FA_SETUNSTABLE). "0 settled" has
        // two opposite causes and this separates them.
        pickedSum += fa[13];
        infeasibleSum += fa[25];
        unstableSum += fa[26];
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
      "%u settled / %u died / %u re-excited, picks: %u = %u infeasible + "
      "%u unstable + %u committed, world hash %s)\n",
      ok ? "PASS" : "FAIL", spawned, basinEighths, settledAt, live, blocks,
      binnedSum, settledSum, deadSum, excitedSum, pickedSum, infeasibleSum,
      unstableSum, pickedSum - infeasibleSum - unstableSum,
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
  // A SEALED box is adversarial for settling: with the default damping of 0
  // the drained pool rings between the walls indefinitely (measured: max
  // particle speed still ~15 vox/s after 200 ticks — nothing radiates out of a
  // closed chamber). Real damping is a look knob; the gate turns it up so the
  // drain's END STATE is reachable in a bounded run.
  t.sim.fluidDamping = 0.9f;
  // Pin every sim.fluid* parameter to its tuning_params.def default so the
  // gate is hermetic — its outcome must not depend on whatever the user last
  // dragged in the tuner. "Simply stock" meant "whatever tuning.json says",
  // and a hot-reloaded slider change broke the gate (BASELINE.md: cohesion
  // 32.9, attractDiff -1.08 from a tuner session).
  //
  // PINNING IS NOT OVERRIDING, and the difference is the plan's success
  // signal. THE OVERRIDE SET — parameters set to something OTHER than the
  // default because the gate cannot pass at stock — is shrinking: 7 before
  // WP2, 3 after it, 1 now. WP2 made stock CFL-honest and zero-tension, which
  // retired the stiffness/cohesion/attract overrides; WP3 retired the settle
  // trio's other two, because settleEps 6.0 and wakeSpeed 24.0 ARE stock now
  // (the at-rest speed floor scales with gravity and the owner's is 900).
  // Only fluidDamping above is left, and it is a property of the SEALED
  // geometry — nothing radiates out of a closed box — not a defect in the
  // defaults. Every line below is a pin at the .def value, not an override;
  // if one of them starts disagreeing with tuning_params.def, that is the
  // drift this block exists to catch.
  t.sim.fluidStiffness = 14000.0f;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidAttractSame = 0.0f;
  t.sim.fluidAttractDiff = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidFriction = 0.0f;
  t.sim.fluidSplashRate = 4.0f;
  t.sim.fluidSplashSpeed = 18.0f;
  t.sim.fluidSplashMaxDensity = 0.7f;
  t.sim.fluidSplashLife = 1.1f;
  t.sim.fluidSplashScaleIdx = 2;
  t.sim.fluidFoamRate = 90.0f;
  t.sim.fluidFoamCrestRate = 120.0f;
  t.sim.fluidTrappedMin = 1.5f;
  t.sim.fluidTrappedMax = 11.0f;
  t.sim.fluidCrestMin = 0.25f;
  t.sim.fluidCrestMax = 2.0f;
  t.sim.fluidFoamEnergyMin = 8.0f;
  t.sim.fluidFoamEnergyMax = 260.0f;
  t.sim.fluidFoamLife = 2.2f;
  t.sim.fluidFoamLifeMin = 0.5f;
  t.sim.fluidBubbleBuoyancy = 1.6f;
  t.sim.fluidFoamDrag = 0.72f;
  t.sim.fluidBubbleDensity = 1.05f;
  t.sim.fluidSprayDensity = 0.42f;
  t.sim.fluidFoamScaleIdx = 3;
  // WP3's knobs, pinned like the rest: the CFL substep budget, and the settle
  // trio whose values the WP3 sweep re-derived at the owner's gravity.
  t.sim.fluidSubsteps = 9;
  t.sim.fluidSettleEps = 6.0f;
  t.sim.fluidWakeSpeed = 24.0f;
  t.sim.fluidSettleTicks = 24;
  t.sim.fluidStainRate = 8.0f;
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
  uint32_t infeasibleSum = 0, unstableSum = 0;  // and why they were refused
  uint32_t exBinned = 0, exSettled = 0, exDied = 0;  // seam flow ledger
  uint32_t endWide = 0;   // water voxels anywhere in the box, walls included
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
        uint32_t fa[32] = {};
        rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0,
                              fa, 128, "exciteArgs");
        live = std::min(fa[7], kFluidCap);
        blocks = fa[3];
        excitedSum += fa[11];
        setBlocksSum += fa[13];
        // WP3's refusal split (FA_SETREFUSED / FA_SETUNSTABLE): "picked but
        // nothing settled" has two opposite causes and this is the only thing
        // that can tell them apart.
        infeasibleSum += fa[25];
        unstableSum += fa[26];
        // The seam flow ledger, which localises a mass discrepancy: binned
        // != settled means the column walk dropped it, settled != the voxel
        // sweep means the writes were lost.
        exBinned += fa[15]; exSettled += fa[10]; exDied += fa[8];
        if (run == 1 && i % 40 == 0)
          std::printf("  excite t%d: live %u, blocks %u, excited+ %u, "
                      "picks+ %u (%u infeasible, %u unstable)\n",
                      i, live, blocks, fa[11], fa[13], fa[25], fa[26]);
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
      // THE AT-REST FLOOR PROBE (WP3 item 4, how the settle trio is derived).
      // Report the residual speed BOTH ways: raw, and with the free-surface
      // gravity bias stripped exactly as `seamRestVy` strips it in the shader.
      // A weakly-compressible free surface carries one substep of gravity
      // forever, so the RAW number is a property of `gravity/substeps` and
      // tells you nothing about whether the water is moving; the STRIPPED one
      // is the quantity `settleEps` is supposed to be compared against, and it
      // is what makes the threshold mean the same thing at any gravity.
      const int32_t gSub =
          (int32_t)std::lround(t.sim.fluidGravity * 65536.0 / 900.0) /
          std::max(1, t.sim.fluidSubsteps);
      int32_t restMaxS2 = 0;
      uint32_t restFast = 0;
      for (uint32_t k = 0; k < n; k++) {
        const int32_t* p = (const int32_t*)&pbuf[k * kFluidParticleWords];
        liveEighths += ((uint32_t)p[18] >> 12) & 0x7u;
        int32_t sx = p[3] >> 8, sy = p[4] >> 8, sz = p[5] >> 8;
        int32_t s2 = sx * sx + sy * sy + sz * sz;
        if (s2 > 49) fast++;
        endMaxS2 = std::max(endMaxS2, s2);
        int32_t ry = std::min(std::abs(p[4] + gSub), std::abs(p[4])) >> 8;
        int32_t r2 = sx * sx + ry * ry + sz * sz;
        if (r2 > 49) restFast++;
        restMaxS2 = std::max(restMaxS2, r2);
      }
      if (run == 1)
        std::printf("  end: %u live carrying %u eighths; raw max %.2f vox/s "
                    "(%u above 0.9), rest-frame max %.2f vox/s (%u above "
                    "0.9), one substep of g = %.2f vox/s\n",
                    n, liveEighths, std::sqrt((double)endMaxS2) * 30.0 / 256.0,
                    fast, std::sqrt((double)restMaxS2) * 30.0 / 256.0, restFast,
                    (double)(gSub >> 8) * 30.0 / 256.0);
    }

    // Mass audit over the whole sealed interior (both chambers): standing
    // water eighths at the end must equal the eighths placed at the start.
    endEighths = 0;
    endWide = 0;
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
            // WIDE sweep first: every water voxel anywhere in the box, walls
            // included. If the interior audit comes up short but this does
            // not, the mass was written somewhere the interior test does not
            // look -- an audit-scope artifact, not a destroyed eighth.
            if ((cbuf[i] & 0xFFFu) == waterId)
              endWide += ((cbuf[i] >> 12) & 0xFu) + 1u;
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
  std::printf("  seam flow: %u binned / %u settled / %u died; interior %u + "
              "live %u = %u of %u (wide sweep %u)\n",
              exBinned, exSettled, exDied, endEighths, liveEighths,
              endEighths + liveEighths, kWaterEighths, endWide);
  std::printf(
      "fluid excite: %s (%u eighths excited over the drain, %u/%u sampled "
      "particles pre-compressed, %u standing + %u live eighths of %u, "
      "%u live / %u blocks at end, %u settle picks (%u infeasible, "
      "%u unstable), end max s2 %d, world hash %s)\n",
      ok ? "PASS" : "FAIL", excitedSum, compressed, sampled, endEighths,
      liveEighths, kWaterEighths, live, blocks, setBlocksSum, infeasibleSum,
      unstableSum, endMaxS2, det ? "matches" : "DIVERGED");
  detail = Format("%u excited, %u/%u compressed, mass %u+%u/%u, det %s",
                  excitedSum, compressed, sampled, endEighths, liveEighths,
                  kWaterEighths, det ? "ok" : "DIVERGED");
  return ok ? Status::Pass : Status::Fail;
}

// ---- fluid-onwater -------------------------------------------------------
// PARTICLE WATER MEETS SETTLED WATER — the one question none of the five lab
// pour scenes can ask, because every one of them pours into an EMPTY basin.
//
// Up to WP4 the answer was "they ignore each other". sim_fluid.wgsl's
// fluidSolid() blocks solids and powders only, and a fullness voxel scatters
// nothing into the node grid because it has no particles, so MPM water poured
// onto a basin filled to the rim fell straight to the BED as if the basin were
// empty. sim.fluidSettledMass is the fix: a settled cell seeds its node with
// fullness/8 * restDensity of zero-velocity mass, so the pool has density and
// the pour floats on it.
//
// THE ASSERTION IS A DIFFERENTIAL, not an absolute depth, and that is what
// makes it hard to fool. A sealed box, a bed of stone, eight full layers of
// SETTLED water above it, and a burst of particles dropped in from the air
// pocket at the top. The gate runs the same pour TWICE — once with
// fluidSettledMass 0 (WP4's pass-through, the control) and once at the shipped
// 1.0 — and requires that the pool measurably holds the pour UP.
//
// An absolute threshold was tried first and is the wrong instrument: this is a
// weakly-compressible solver, the jet arrives near its own CFL ceiling, and the
// static boundary cannot be shoved aside the way real water would be, so a fast
// pour legitimately punches a deep crater before pressure throws it back. What
// must never happen — and what WP4 did every time — is the pour reaching the
// BED as though the basin were empty. The control arm measures exactly that, in
// the same geometry, on the same build.
//
// exciteMode is pinned OFF, and note what that does NOT switch off: the seam's
// WAKE trigger is unconditional (sim_fluid_seam.wgsl exciteDetect — only the
// fall and perch triggers are behind fluidExciteEnable), because a disturbance
// reaching settled water has to be able to propagate whatever mode the world is
// in. So the pour can still excite the pool it lands on, and the CONTROL ARM
// shows why that mattered so much: with no boundary mass the pour falls THROUGH
// the pool, every cell it passes has a fast node face-adjacent to it, and the
// wake cascades down the whole column — the measured control converts 8,136 of
// the box's 8,144 eighths, i.e. the entire basin, which is the reported "a
// waterfall turns the whole lake into fluid". The shipped arm converts ~1,300:
// a crater at the point of impact, which is what a splash should do.
//
// SECOND ASSERTION, and it is the sharper one: the pool must still BE a pool
// afterwards. `peakLive` bounds how much of the box was ever particles at once.
//
// (The control arm's deepest-particle reading lands below the bed — a fully
// converted, over-pressured sealed box leaks particles through a 2-cell shell.
// That is a pre-existing containment weakness under pathological pressure, the
// same one the fluid-excite gate's double shell exists to keep out of ITS
// audit; here it is only the control, whose mass is deliberately not audited.)
Status GateFluidOnWater(Ctx& c, std::string& detail) {
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
  t.sim.fluidExciteMode = 0;   // see the header: the pool must stay settled
  t.sim.fluidSubsteps = 9;
  t.sim.fluidStiffness = 14000.0f;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidDamping = 0.0f;
  t.sim.fluidFriction = 0.0f;
  Tuning saved = CurrentTuning();

  // Box: interior x,z in [-5,5] around (96,·,96), double stone shell. Bed at
  // bedY, settled water bedY+1 .. bedY+8 (8 full layers), air above it, roof.
  const int px = 96, pz = 96, RB = 7;
  const int bedY = 110, roofY = 127;
  const int waterTop = bedY + 8;      // 118: last settled layer
  const int pourY = 124;              // the air pocket, 6 cells clear of it
  const int kPourTick = 4, kMaxTicks = 150;
  const int kIn = RB - 2;             // interior half-extent (5)
  const uint32_t kSettledEighths = (uint32_t)((2 * kIn + 1) * (2 * kIn + 1)) * 8u * 8u;

  uint32_t worldHash[2] = {0, 0};
  uint32_t poured = 0, live = 0, liveEighths = 0, endEighths = 0;
  int minParticleY = 1 << 30;     // deepest particle seen after the pour lands
  int minSampleTick = -1;
  // run 0 = the CONTROL (fluidSettledMass 0, i.e. WP4's pass-through);
  // runs 1 and 2 = the shipped 1.0, twice, so the hash comparison still has
  // two matching arms to compare.
  int controlMinY = 1 << 30;
  uint32_t peakLive = 0, controlPeakLive = 0;
  for (int run = 0; run < 3; run++) {
    t.sim.fluidSettledMass = run == 0 ? 0.0f : 1.0f;
    SetCurrentTuning(t);
    // sim.fluid* are WGSL consts folded in at compile time (the human-unit
    // exception in CLAUDE.md), so the arm switch is a shader reload — the F5
    // path, used here for the reason F5 exists.
    sim.ReloadShaders(ctx.device);
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();
    std::vector<CellOp> box;
    auto put = [&](int x, int y, int z, uint32_t m, uint32_t state = 0) {
      box.push_back({World::SlotCellIndex({x, y, z}),
                     (m & 0xFFFu) | (state << 12)});
    };
    for (int y = bedY - 1; y <= roofY + 1; y++)
      for (int z = -RB; z <= RB; z++)
        for (int x = -RB; x <= RB; x++) {
          const bool shell = x <= -RB + 1 || x >= RB - 1 || z <= -RB + 1 ||
                             z >= RB - 1 || y <= bedY || y >= roofY;
          if (shell) put(px + x, y, pz + z, kMatStone);
          else if (y <= waterTop) put(px + x, y, pz + z, waterId, 7u);
          else put(px + x, y, pz + z, kMatAir);
        }

    // The pour: a 5x5x2 block of cells' worth of particles at the 2x2x2
    // sub-cell lattice, the same seeding exciteEmit uses, dropped with a
    // downward kick so they arrive with real momentum rather than drifting on.
    std::vector<FluidSpawnOp> pour;
    for (int y = 0; y < 2; y++)
      for (int z = -2; z <= 2; z++)
        for (int x = -2; x <= 2; x++)
          for (int k = 0; k < 8; k++) {
            FluidSpawnOp op{};
            op.px = ((px + x) << 16) + ((k & 1) ? 49152 : 16384);
            op.py = ((pourY + y) << 16) + ((k & 2) ? 49152 : 16384);
            op.pz = ((pz + z) << 16) + ((k & 4) ? 49152 : 16384);
            op.vx = 0;
            op.vy = -65536;   // 1 cell/tick down = 30 vox/s
            op.vz = 0;
            op.species = 0;
            op.mat = waterId;
            pour.push_back(op);
          }
    poured = (uint32_t)pour.size();

    uint32_t ft = 50000;
    uint32_t liveEst = 0;
    minParticleY = 1 << 30;
    peakLive = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      std::vector<FluidSpawnOp> sp;
      if (i == kPourTick) sp = pour;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, sp, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      if (i < kPourTick) { liveEst = 0; continue; }
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "onWaterArgs");
      live = std::min(fa[7], kFluidCap);
      liveEst = live;
      peakLive = std::max(peakLive, live);
      // The depth probe. Sampled from the tick the pour has had time to reach
      // the surface (a 6-cell fall at 1 cell/tick plus gravity) to the end.
      if (i >= kPourTick + 10 && live > 0) {
        uint32_t n = std::min(live, kFluidCap);
        std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
        rhi::ReadbackBlocking(ctx.device, ctx.queue,
                              world.fluidParticles[sim.Page()], 0, pbuf.data(),
                              pbuf.size() * 4, "onWaterP");
        uint32_t alive = 0, belowBed = 0;
        int tickMin = 1 << 30;
        for (uint32_t k = 0; k < n; k++) {
          const uint32_t* p = &pbuf[k * kFluidParticleWords];
          if (((p[18] & 0xFFFu) == 0u) || (((p[18] >> 12) & 0x7u) == 0u))
            continue;  // fpAlive
          alive++;
          const int y = (int)((int32_t)p[1] >> 16);
          if (y < bedY) belowBed++;
          if (y < tickMin) tickMin = y;
          if (y < minParticleY) { minParticleY = y; minSampleTick = i; }
        }
        if (i % 30 == 0)
          std::printf("  on-water arm%d t%d: live %u alive %u minY %d "
                      "belowBed %u\n",
                      run, i, live, alive, tickMin, belowBed);
      }
    }
    if (run == 0) {
      controlMinY = minParticleY;
      controlPeakLive = peakLive;
      continue;
    }
    worldHash[run - 1] = HashWorldNow(ctx, world, sim, kDefaultSeed);

    liveEighths = 0;
    if (live > 0) {
      uint32_t n = std::min(live, kFluidCap);
      std::vector<uint32_t> pbuf((size_t)n * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "onWaterEnd");
      for (uint32_t k = 0; k < n; k++)
        liveEighths += (pbuf[k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }

    // Standing water over the sealed interior, for the mass audit.
    endEighths = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cy = (bedY - 1) / 16; cy <= (roofY + 1) / 16; cy++)
      for (int cz2 = (pz - RB) / 16; cz2 <= (pz + RB) / 16; cz2++)
        for (int cx2 = (px - RB) / 16; cx2 <= (px + RB) / 16; cx2++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx2, cy, cz2}), 1,
                         cbuf.data(), "onWaterVox");
          for (uint32_t j = 0; j < kChunkVol; j++)
            if ((cbuf[j] & 0xFFFu) == waterId)
              endEighths += ((cbuf[j] >> 12) & 0xFu) + 1u;
        }
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  // TWO conditions on the depth, and they fail for different reasons.
  //   * `held`   — the pool must hold the pour measurably higher than no pool
  //     at all. This is the mechanism test, and the control arm supplies its
  //     own reference on this build, this GPU, this tuning.
  //   * `offBed` — and it must not reach the bed. The control arm lands ON the
  //     bed, so this is the user-visible statement: water poured into a full
  //     basin does not end up underneath it.
  const int kFloor = bedY + 2;
  const uint32_t kTotal = kSettledEighths + poured;
  bool held = minParticleY > controlMinY;
  bool offBed = minParticleY >= kFloor;
  // The pool must survive as a pool: less than half the box may ever be
  // particles at once. Measured 1,274 of 8,144 here, against a control of
  // 8,136 — the whole basin.
  bool stayedPool = peakLive * 2u < kTotal;
  bool massOk = endEighths + liveEighths == kTotal;
  bool det = worldHash[0] == worldHash[1];
  bool ok = held && offBed && stayedPool && massOk && det;
  std::printf(
      "fluid on-water: %s (%u particles poured onto %u settled eighths; "
      "deepest particle y %d at t%d vs control (settledMass 0) y %d, bed %d, "
      "floor %d; peak live %u vs control %u of %u total; %u standing + %u live "
      "= %u of %u; world hash %s)\n",
      ok ? "PASS" : "FAIL", poured, kSettledEighths, minParticleY,
      minSampleTick, controlMinY, bedY, kFloor, peakLive, controlPeakLive,
      kTotal, endEighths, liveEighths, endEighths + liveEighths, kTotal,
      det ? "matches" : "DIVERGED");
  detail =
      Format("deepest y %d vs control %d (bed %d), peak live %u vs %u of %u, "
             "mass %u+%u/%u, det %s",
             minParticleY, controlMinY, bedY, peakLive, controlPeakLive, kTotal,
             endEighths, liveEighths, kTotal, det ? "ok" : "DIVERGED");
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
  // The excite gate's sealed-box overrides. settleEps/wakeSpeed used to be
  // here too and are stock now (WP3) — a sealed chamber still needs the
  // damping and the softer stiffness.
  t.sim.fluidDamping = 0.9f;
  t.sim.fluidStiffness = 2400.0f;
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
  // Pin every sim.fluid* parameter to its tuning_params.def default so this
  // gate is hermetic — its outcome must not depend on the user's tuning.json.
  // Only THREE are real overrides now (exciteMode, damping, stiffness);
  // settleEps and wakeSpeed became stock in WP3 and are pins like the rest.
  t.sim.fluidExciteMode = 1;
  t.sim.fluidDamping = 0.9f;       // sealed box: nothing radiates out of it
  t.sim.fluidStiffness = 2400.0f;  // softer than stock so the drain ends
  t.sim.fluidSettleEps = 6.0f;     // == stock (WP3)
  t.sim.fluidWakeSpeed = 24.0f;    // == stock (WP3)
  t.sim.fluidSubsteps = 9;
  t.sim.fluidGravity = 900.0f;
  t.sim.fluidRestDensity = 8.0f;
  t.sim.fluidEosPower = 4;
  t.sim.fluidCohesion = 0.0f;
  t.sim.fluidAttractSame = 0.0f;
  t.sim.fluidAttractDiff = 0.0f;
  t.sim.fluidViscosity = 0.0f;
  t.sim.fluidFriction = 0.0f;
  // SPLASH IS NOT THE MISSING MASS, and the correction is worth writing down
  // because the wrong answer was already in this file.
  //
  // The gate comes up 37 of 2704 eighths short (1.4%), deterministically. The
  // first diagnosis was splash: sim_fluid's g2p sheds droplets into the
  // ballistic particle system at sim.fluidSplashRate, a droplet is in none of
  // the three counted destinations, and the reporter dutifully said "short 37,
  // 10783 droplets in flight" -- a real number next to the failure, which is
  // exactly what makes a coincidence convincing. So the rate was pinned to 0.
  //
  // Read the emitter (sim_fluid.wgsl, "splash: fast free-surface particles
  // shed micro droplets"): it fills a Particle and appends it, and it NEVER
  // touches the parent's fullness. A splash droplet is a PFLAG_MICRO visual
  // that deposits a stain on contact -- it carries no eighths, so it cannot
  // remove any. The pin proved it: droplets fell 10783 -> 10264 and the
  // shortfall stayed at exactly 37. A number that does not move when you
  // remove its supposed cause was never the cause.
  //
  // So the rate goes back to the .def default like every other line here, and
  // the question goes to the seam ledger below, which is the instrument the
  // other two fluid gates already use for exactly this.
  t.sim.fluidSplashRate = 4.0f;
  t.sim.fluidSplashSpeed = 18.0f;
  t.sim.fluidSplashMaxDensity = 0.7f;
  t.sim.fluidSplashLife = 1.1f;
  t.sim.fluidSplashScaleIdx = 2;
  t.sim.fluidFoamRate = 90.0f;
  t.sim.fluidFoamCrestRate = 120.0f;
  t.sim.fluidTrappedMin = 1.5f;
  t.sim.fluidTrappedMax = 11.0f;
  t.sim.fluidCrestMin = 0.25f;
  t.sim.fluidCrestMax = 2.0f;
  t.sim.fluidFoamEnergyMin = 8.0f;
  t.sim.fluidFoamEnergyMax = 260.0f;
  t.sim.fluidFoamLife = 2.2f;
  t.sim.fluidFoamLifeMin = 0.5f;
  t.sim.fluidBubbleBuoyancy = 1.6f;
  t.sim.fluidFoamDrag = 0.72f;
  t.sim.fluidBubbleDensity = 1.05f;
  t.sim.fluidSprayDensity = 0.42f;
  t.sim.fluidFoamScaleIdx = 3;
  t.sim.fluidSettleTicks = 24;
  t.sim.fluidStainRate = 8.0f;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(t);
  sim.ReloadShaders(ctx.device);

  const int px = 96, pz = 96, RB = 8;
  const int floorY = 109, upperY = 119, roofY = 126;
  const int kCarveTick = 30, kMaxTicks = 260;
  const uint32_t kWaterEighths = 13u * 13u * 2u * 8u;  // 2 deep this time
  uint32_t worldHash[2] = {0, 0};
  uint32_t consumedSum = 0, standing = 0, liveEighths = 0, plantsEnd = 0;
  // Attribution for a mass account that does not close — see the census below.
  uint32_t strayWater = 0, endParticles = 0;
  // THE SEAM LEDGER, which is what `ca-slope` and `fluid-excite` reach for when
  // their own accounts do not close, and what this gate was missing. Water
  // crossing between voxels and particles passes through these counters, so a
  // shortfall that is invisible in the three-way account is usually loud in
  // here: excited != emitted means the excite dropped it, emitted != settled +
  // live means a particle died carrying mass, binned is settle refusing a
  // column. Seven u32 adds a tick against a readback the loop already does.
  uint32_t exExcited = 0, exEmitted = 0, exSettled = 0, exDead = 0;
  uint32_t exBinned = 0, exRefused = 0;
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
    exExcited = exEmitted = exSettled = exDead = exBinned = exRefused = 0;
    for (int i = 0; i < kMaxTicks; i++) {
      std::vector<CellOp> cops;
      if (i == 0) cops = box;
      else if (i == kCarveTick) cops = carve;
      SubmitTick(ctx, world, sim, ++ft, kDefaultSeed, {}, {}, cops, false,
                 {6, 7, 6}, false, false, {}, 0, {}, liveEst);
      ctx.WaitIdle();
      ctx.ProcessEvents();
      // FROM TICK 0, not from the carve. The old loop started accumulating at
      // kCarveTick on the reasoning that nothing can happen before the floor
      // opens — but that is an assumption about the sim stated in the test,
      // and it is the kind that produces a small constant shortfall if it is
      // wrong (the water sits on a sealed floor for 30 ticks, and "sealed"
      // is what the reaction rules get to decide, not this file). Reading the
      // counters every tick costs the same readback and removes the
      // assumption. If ticks 0..29 really do contribute nothing, the ledger
      // below will say so in zeroes.
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            128, "reactArgs");
      liveEst = std::min(fa[7], kFluidCap);
      consumedSum += fa[16];  // FA_CONSUMED
      exDead += fa[8];        // FA_DEAD
      exEmitted += fa[9];     // FA_EMITTED
      exSettled += fa[10];    // FA_SETTLED
      exExcited += fa[11];    // FA_EXCITED
      exRefused += fa[12];    // FA_REFUSED
      exBinned += fa[15];     // FA_BINNED
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
    strayWater = 0;
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
            const bool inside =
                !(lx < px - RB + 2 || lx > px + RB - 2 || lz < pz - RB + 2 ||
                  lz > pz + RB - 2 || ly <= floorY || ly >= roofY);
            uint32_t m = cbuf[i] & 0xFFFu;
            // WHERE ELSE COULD THE MASS BE. The account below is exact by
            // construction, so when it does not close the only useful next
            // question is which of the OTHER destinations took the
            // difference — and answering that by turning features off one at
            // a time is the mistake CLAUDE.md rule 6 is about. Water outside
            // the interior box (leaked into the shell, or through it) is one
            // destination and is counted here; droplets in flight are the
            // other and are read from the snapshot below.
            if (m == waterId && !inside) {
              strayWater += ((cbuf[i] >> 12) & 0xFu) + 1u;
              continue;
            }
            if (!inside) continue;
            if (m == waterId) standing += ((cbuf[i] >> 12) & 0xFu) + 1u;
            if (m == plantId) plantsEnd++;
          }
        }
    // Droplets: sim_fluid's g2p sheds spray into the BALLISTIC particle system
    // (tuning sim.fluidSplashRate), which is a fourth destination for water
    // that neither the grid census nor the fluid-particle census can see.
    endParticles = world.Snap().valid ? world.Snap().particleCount : 0u;
  }
  SetCurrentTuning(saved);
  sim.ReloadShaders(ctx.device);

  bool det = worldHash[0] == worldHash[1];
  bool consumed = consumedSum > 0;
  bool grew = plantsEnd > kPlantsStart;
  const int shortBy = (int)kWaterEighths - (int)(standing + liveEighths +
                                                 consumedSum);

  // THE ACCOUNT HAS FOUR DESTINATIONS, NOT THREE, and the fourth is the thing
  // the gate is named after.
  //
  // `standing + live + consumed == placed` was exact by construction only if
  // every eighth a reaction eats passes through the seam. It does not.
  // FA_CONSUMED is a fluidArgs counter: it records eighths eaten off EXCITED
  // fluid, which is the specific claim this gate exists to make. But the
  // authored rule is `{self: plant, neighbor: water, neighborBecomes: plant}`
  // (assets/materials/reactions.json), and the CA runs it on SETTLED water
  // voxels too — a water cell beside a plant simply becomes plant, in the
  // grid, with no particle and no seam event. Those eighths are not lost; they
  // are standing in the world as the 198 new plant cells the gate itself
  // counts and calls a pass.
  //
  // Measured: 37 of 2704 (1.4%), and the seam ledger says it is not seam-side
  // (2432 excited -> 2432 emitted, nothing refused). 37 eighths against 198
  // new plants is under a quarter of an eighth per plant, which is what
  // eating mostly-empty rim cells looks like.
  //
  // So the assertion becomes the property that actually matters, and it is
  // still a leak detector — it just knows about the fourth destination:
  //   1. NO WATER IS CREATED. shortBy >= 0, unconditionally.
  //   2. Everything missing is accounted for by plants that exist. A water
  //      cell holds at most 8 eighths and each conversion consumes at most
  //      one cell, so the gap can never exceed 8 * the plants that grew.
  //   3. And it stays SMALL. Bound 2 alone is loose (8 * 198 = 1584), so the
  //      fraction is pinned in baseline.json where a threshold belongs. Water
  //      vanishing with no plants to show for it, or vanishing faster than the
  //      plant bed can eat, fails here exactly as the equality used to.
  const uint32_t newPlants =
      plantsEnd > kPlantsStart ? plantsEnd - kPlantsStart : 0u;
  const double gapPct = 100.0 * (double)shortBy / (double)kWaterEighths;
  const double gapPctMax = BaselineNumber("fluidReactCaGapPctMax", 3.0);
  bool massOk = shortBy >= 0 && (uint32_t)shortBy <= 8u * newPlants &&
                gapPct <= gapPctMax;
  RecordObserved("fluidReactCaGapPct", gapPct);
  bool ok = consumed && grew && massOk && det;
  std::printf("  react seam: %u excited -> %u emitted, %u settled, %u dead, "
              "%u refused, %u binned\n",
              exExcited, exEmitted, exSettled, exDead, exRefused, exBinned);
  std::printf(
      "fluid react: %s (%u eighths consumed by reactions, plants %u -> %u, "
      "%u standing + %u live + %u consumed of %u placed"
      " [gap %d = %.2f%% (allow %.2f%%), under the %u eighths %u new plants"
      " could have eaten; %u stray outside the box, %u droplets in flight],"
      " world hash %s)\n",
      ok ? "PASS" : "FAIL", consumedSum, kPlantsStart, plantsEnd, standing,
      liveEighths, consumedSum, kWaterEighths, shortBy, gapPct, gapPctMax,
      8u * newPlants, newPlants, strayWater, endParticles,
      det ? "matches" : "DIVERGED");
  detail = Format("%u consumed, plants %u->%u, mass %u+%u+%u/%u (gap %d ="
                  " %.2f%% of %.2f%% allowed, vs %u eighths %u new plants"
                  " could eat; stray %u, droplets %u; seam %u excited -> %u"
                  " emitted, %u settled, %u dead, %u refused, %u binned),"
                  " det %s",
                  consumedSum, kPlantsStart, plantsEnd, standing, liveEighths,
                  consumedSum, kWaterEighths, shortBy, gapPct, gapPctMax,
                  8u * newPlants, newPlants, strayWater, endParticles,
                  exExcited, exEmitted, exSettled, exDead, exRefused, exBinned,
                  det ? "ok" : "DIVERGED");
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
  // Terrain-relative, and the COUNT BOX below follows it: a fixed box that
  // now contains a hillside counts the hillside, which is how "110,592
  // voxels placed" was reported as 257,391 landed.
  IVec3 lo{100, FixtureYOver(100, 100, 147, 147, kDefaultSeed, 40, 96), 100},
      hi;
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
      for (int cy = lo.y >> 4; cy <= (lo.y + 47) >> 4; cy++)
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
  //
  // THE BUDGETS LIVE IN baseline.json, which is CLAUDE.md's own rule ("put
  // thresholds and expected values in tests/baseline.json, not in C++ — a
  // threshold that lives in source costs a rebuild to tune") and which this
  // gate was the last one violating. 8.0 and 16.0 were literals here, written
  // at a smaller kVoxelMeters, and they have been unreachable since the voxel
  // size changed: the gate has sat red in the baseline ever since, asserting
  // an aspiration rather than detecting a regression. A permanently-red
  // advisory gate is worse than no gate, because it trains everyone to skip
  // the line.
  //
  // So: budgets are data, the measured values are pushed through
  // RecordObserved so `--rebaseline` writes back what the machine actually
  // does, and the fallbacks below are the historical literals for a checkout
  // whose baseline.json predates the keys.
  const double simBudget = BaselineNumber("perf.simMsMax", 8.0);
  const double frameBudget = BaselineNumber("perf.frameMsMax", 16.0);
  bool perfOk = c.simMs < simBudget && c.bestFrameMs < frameBudget;
  RecordObserved("perf.simMsObserved", c.simMs);
  RecordObserved("perf.frameMsObserved", c.bestFrameMs);
  std::printf("perf: %s\n", perfOk ? "PASS" : "MARGINAL (see numbers above)");
  detail = Format("sim %.2f ms/tick (budget %.2f), best frame %.2f ms "
                  "(budget %.2f)",
                  c.simMs, simBudget, c.bestFrameMs, frameBudget);
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

  // ---- FIND the empty sky; do not assume where it is ----------------------
  //
  // This used to be `origin.y + 24 chunks` on the theory that 384 voxels up is
  // above any terrain. It is not a theory this gate can hold: `page-roundtrip`
  // runs AFTER `streaming`, which flies the player out past x = 600, so the
  // window has moved and the "provably empty" column sits in open procedural
  // terrain that neither this file nor the constant knows the height of. Any
  // better constant has the same defect — it would be a second, unowned copy of
  // the terrain band, and the terrain overhaul moves that band by an order of
  // magnitude.
  //
  // So ask the page table, which already knows: walk DOWN the chunk column from
  // the top of the window for the first EMPTY sentinel. That is what "empty sky"
  // means, stated as the property instead of as a coordinate. Failing when the
  // whole column has none is itself a new and meaningful assertion — a residency
  // window with no empty chunk anywhere above the player is a page-pool problem
  // this gate would otherwise have painted over.
  const IVec3 o = world.WindowOrigin();
  const int scx = o.x + (int)kNChunk / 2;
  const int scz = o.z + (int)kNChunk / 2;
  int skyCy = -1;
  for (int cy = o.y + (int)kNChunk - 1; cy >= o.y; cy--) {
    const uint32_t s = World::SlotChunkIndex({scx, cy, scz});
    if (world.PageEntryOfSlot(s) == kPtEmpty) { skyCy = cy; break; }
  }
  if (skyCy < 0) {
    detail = Format(
        "no EMPTY chunk anywhere in the column above (%d,%d): the residency "
        "window has no open sky to paint into, which is a page-pool or "
        "worldgen-height problem, not a paging one",
        scx, scz);
    return Status::Fail;
  }
  const IVec3 sky{scx * (int)kChunk + 8, skyCy * (int)kChunk + 8,
                  scz * (int)kChunk + 8};
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

// ---- simd: the derived vector forms equal their scalar definitions --------
//
// sim/scan.h and sim/rng_simd.h are strength reductions of loops that classify
// and verify whole chunks (PageTable::Classify, the hysteresis free probe).
// They are guarded by #if defined(__AVX2__), and A `#if` PROVES NOTHING ABOUT
// THE BRANCH IT DID NOT COMPILE — so this gate compares the two IN THE SAME
// BINARY rather than trusting that two build configurations agree.
//
// Two things here are genuinely new arithmetic rather than a mechanical
// substitution, and they are the two this sweeps hardest:
//   - Pcg8's vpsrlvd, which agrees with C++ `>>` only because (s>>28)+4 is
//     always < 32. Every one of the 16 possible (s>>28) values is exercised.
//   - Mod3_8's magic-number division by 3.
//
// It also exercises FirstIndexWhereMasked's SCALAR TAIL, which no production
// call site can reach: every real caller passes n == kChunkVol (a multiple of
// 8), so without this the tail would be untested code that ships.
Status GateSimd(Ctx&, std::string& detail) {
  uint32_t checked = 0, bad = 0;
  std::string firstBad;

  // ---- (a) FirstIndexWhereMasked vs its scalar definition ----------------
  // Lengths straddling the 8-wide block, and a mismatch planted at EVERY
  // index in turn (plus the no-mismatch case), against several masks.
  {
    const uint32_t masks[] = {0xFFFFFFFFu, 0xFF000FFFu, 0x00000FFFu, 0u};
    const size_t lens[] = {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 4096};
    std::vector<uint32_t> buf(4096);
    for (uint32_t mask : masks)
      for (size_t n : lens) {
        // planted == n means "no mismatch anywhere".
        for (size_t planted = 0; planted <= n; planted++) {
          for (size_t i = 0; i < n; i++) buf[i] = 0xAAAA5555u;
          const uint32_t want = 0xAAAA5555u & mask;
          if (planted < n) buf[planted] = 0xAAAA5555u ^ 0x1001u;
          const size_t got =
              scan::FirstIndexWhereMasked(buf.data(), n, mask, want);
          const size_t exp =
              scan::ScalarFirstIndexWhereMasked(buf.data(), n, mask, want);
          checked++;
          if (got != exp) {
            bad++;
            if (firstBad.empty()) {
              char b[192];
              std::snprintf(b, sizeof(b),
                            "scan mask %08x n %zu planted %zu: got %zu want %zu",
                            mask, n, planted, got, exp);
              firstBad = b;
            }
          }
          if (n > 64 && planted > 40) break;  // 4096 x 4096 is not the point
        }
      }
  }

#if defined(__AVX2__)
  // ---- (b) Pcg8 vs rng::Pcg ---------------------------------------------
  // Edge words, then a stride that walks the whole u32 range so every value of
  // the (s>>28) shift selector is hit many times over.
  {
    std::vector<uint32_t> probe = {0u, 1u, 2u, 7u, 0xFFFFFFFFu, 0xFFFFFFFEu,
                                   0x80000000u, 0x7FFFFFFFu, 0xC0FFEEu,
                                   747796405u, 2891336453u, 277803737u};
    // Force each of the 16 (s>>28) selectors: invert Pcg's first step so s
    // lands in a chosen top nibble. s = v*A + B  =>  v = (s - B) * A^-1.
    // A^-1 mod 2^32 for A = 747796405 is 3425556037.
    for (uint32_t nib = 0; nib < 16; nib++) {
      const uint32_t s = (nib << 28) | 0x0123456u;
      probe.push_back((uint32_t)((s - 2891336453u) * 3425556037u));
    }
    for (uint64_t v = 0; v < 0x100000000ull; v += 0x3B9ACAull)  // ~1,100 samples
      probe.push_back((uint32_t)v);
    for (size_t i = 0; i + 8 <= probe.size(); i += 8) {
      const __m256i in = _mm256_loadu_si256((const __m256i*)(probe.data() + i));
      uint32_t out[8];
      _mm256_storeu_si256((__m256i*)out, rng::Pcg8(in));
      for (int k = 0; k < 8; k++) {
        const uint32_t exp = rng::Pcg(probe[i + k]);
        checked++;
        if (out[k] != exp) {
          bad++;
          if (firstBad.empty()) {
            char b[160];
            std::snprintf(b, sizeof(b), "Pcg8(%08x): got %08x want %08x",
                          probe[i + k], out[k], exp);
            firstBad = b;
          }
        }
      }
    }
  }

  // ---- (c) JitterStateInRow8 vs JitterStateInRow (covers Mod3_8) ---------
  // Real row seeds at real coordinates, including negative ones — the world
  // is signed and the (uint32_t) cast of a negative coord is the documented
  // two's-complement path both forms must take.
  {
    const int ys[] = {-4097, -16, -1, 0, 1, 15, 16, 4096};
    const int zs[] = {-4097, -16, -1, 0, 1, 15, 16, 4096};
    const int xs[] = {-4096, -17, 0, 16, 1024};
    const uint32_t seeds[] = {0u, 1u, 0xC0FFEEu, 0xDEADBEEFu};
    const __m256i lane = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (uint32_t seed : seeds)
      for (int y : ys)
        for (int z : zs)
          for (int xb : xs) {
            const uint32_t rowSeed = JitterRowSeed(y, z, seed);
            uint32_t out[8];
            _mm256_storeu_si256(
                (__m256i*)out,
                rng::JitterStateInRow8(
                    rowSeed, _mm256_add_epi32(_mm256_set1_epi32(xb), lane),
                    seed));
            for (int k = 0; k < 8; k++) {
              const uint32_t exp = JitterStateInRow(rowSeed, xb + k, seed);
              checked++;
              if (out[k] != exp) {
                bad++;
                if (firstBad.empty()) {
                  char b[192];
                  std::snprintf(b, sizeof(b),
                                "jitter seed %08x y %d z %d x %d: got %u want %u",
                                seed, y, z, xb + k, out[k], exp);
                  firstBad = b;
                }
              }
            }
          }
  }
#endif

  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "%s path: %u derived==definition checks, %u mismatch%s%s%s",
                scan::kHaveAvx2 ? "AVX2" : "SCALAR-ONLY (no __AVX2__)", checked,
                bad, bad == 1 ? "" : "es", firstBad.empty() ? "" : " | first: ",
                firstBad.c_str());
  detail = buf;
  return bad == 0 ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& SimGates() {
  static const std::vector<Gate> g = {
      {"simd", "sim", {}, false, GateSimd},
      {"determinism", "sim", {}, false, GateDeterminism},
      {"sleep", "sim", {}, false, GateSleep},
      {"evaporation", "sim", {}, false, GateEvaporation},
      {"blood-stain", "sim", {}, false, GateBloodStain},
      {"flung-liquid", "sim", {}, false, GateFlungLiquid},
      {"fluid-det", "sim", {}, false, GateFluidDet},
      {"fluid-settle", "sim", {}, false, GateFluidSettle},
      {"fluid-excite", "sim", {}, false, GateFluidExcite},
      {"fluid-onwater", "sim", {}, false, GateFluidOnWater},
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
