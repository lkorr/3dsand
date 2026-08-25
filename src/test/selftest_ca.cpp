// selftest_ca.cpp — gates for the CA DISPATCH-RECORDING decision, as opposed
// to what the CA computes.
//
// Its own domain, and its own file, because the thing under test is not a rule
// or a material: it is `Cond::CaActive` (ROADMAP_scale.md §3.4/§3.2d), the CPU
// latch that decides whether the tick records the CA's 54 indirect dispatches,
// the 32,768-flag `compact` scan and the args staging copy at all. That latch
// is pure recording-side policy — it must be invisible in the world hash — and
// the only way to test invisibility is DIFFERENTIALLY, by running one scripted
// history twice with the latch live and defeated and demanding the per-tick
// hash sequences match. A single-run hash cannot see "this chunk was processed
// one tick late"; two runs can.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// ---- ca-skip -------------------------------------------------------------
//
// THE HAZARD THIS EXERCISES, stated before the code so the numbers below can be
// read as evidence for something.
//
// `particleResolve` (sim_particle.wgsl:251,:274) reinserts a voxel into the
// grid and marks its chunk dirty for the NEXT tick. It is the one dirty-writer
// whose target the CPU never chose, so "no ops this tick" does not mean "no
// work next tick" while anything is in flight. Until §3.2d that was covered by
// main.cpp's `particlesActive` — a 400-tick timer — being folded into
// EncodeTick's `inputsThisTick`, which re-stamped `lastDirtyTick_` every tick
// and disabled the skip entirely for 13.3 s after every explosion. §3.2d
// replaces the timer with a conjunct on the arriving snapshot's
// `particleCount`, and the risk it takes on is precisely this:
//
//   if the latch clears in the one-tick window between a particle rejoining the
//   grid and the CPU learning of it, the CA skips a tick whose dirty set is NOT
//   empty. Nothing is corrupted — the chunk is simply processed one tick LATE,
//   and the world hash moves.
//
// So the script is: settle, detonate, let the ejecta fly and LAND, keep the
// particle pipeline nominally alive well past the last landing (as the game
// does), and settle again — hashing every single tick. Run it twice, once with
// the latch live and once with `SetCaForced(true)` recording the CA on every
// tick regardless. Identical hash sequences is the acceptance.
//
// The gate is not only a correctness test. It also asserts the skip ENGAGES on
// ticks where `particlesActive` is true and the world is quiet, which is the
// exact regime §3.2d recovered — before the fix that count is 0 by
// construction, so this is the regression guard for the optimization itself.
Status GateCaSkip(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  // Settle far enough that the dirty set actually reaches ZERO — the latch
  // needs a snapshot reporting no active chunks, not merely few. 300 is what
  // --measure uses to reach its (c) SETTLED scenario, where the skip fires on
  // 119 of 120 ticks.
  constexpr uint32_t kSettleTicks = 300;
  // The hashed window. Long enough to contain: quiet, a blast, the ejecta's
  // whole flight and reinsertion, and a long quiet tail while the particle
  // pipeline is STILL recording (kPipelineTail) — that tail is where the old
  // behaviour lost ~17% of the ACTIVE scenario's CA time.
  constexpr uint32_t kScriptTicks = 220;
  constexpr uint32_t kBoomAt = 10;        // tick within the scripted window
  constexpr uint32_t kPipelineTail = 160; // main.cpp's 400, scaled to the gate

  std::vector<uint32_t> hashes[2];
  uint64_t skips[2] = {0, 0};         // over the scripted (hashed) window
  uint64_t settleSkips[2] = {0, 0};   // over the 300-tick settle, reported only
  // Skips taken on a tick where the particle pipeline was recorded. This is
  // the number that was structurally zero before §3.2d.
  uint64_t skipsWhileParticlesActive = 0;
  uint32_t particlesLeft = 0;
  uint32_t faults = 0;
  int firstSkipAfterBoom = -1;

  for (int run = 0; run < 2; run++) {
    // run 0: the latch as shipped. run 1: the oracle — every tick records the
    // CA, whose indirect count on a settled tick is zero, so it adds no
    // invocation and can only differ from run 0 if run 0 skipped a tick that
    // had work.
    sim.SetCaForced(run == 1);
    // CaSkipCount is a process-lifetime counter and earlier gates have already
    // moved it; only the delta over THIS run means anything.
    const uint64_t skips0 = sim.CaSkipCount();
    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    ctx.WaitIdle();

    uint32_t t = 0;
    for (uint32_t i = 0; i < kSettleTicks; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, t % 15 == 0,
                 {8, 3, 8}, /*wantReadback=*/true, /*particlesActive=*/false);
    ctx.WaitIdle();

    // Anchor to the residency window, never to a literal world position
    // (CLAUDE.md): a gate that runs after `streaming` would otherwise fire into
    // space. TerrainHeight puts the blast at the surface, where there is
    // material to eject — the same lever --measure's ACTIVE scenario uses.
    const IVec3 wo = world.WindowOrigin();
    const int bx = wo.x * (int)kChunk + 100;
    const int bz = wo.z * (int)kChunk + 100;
    const int by = World::TerrainHeight(bx, bz, kDefaultSeed);

    // Counted over the SCRIPTED window only, not the settle phase. The settle
    // phase's skip count depends on what the previous gate left in the page
    // table and swings 3x between a standalone run and a full-suite run; the
    // scripted window is the thing under test and is stable.
    const uint64_t scriptSkips0 = sim.CaSkipCount();
    bool exploded = false;
    uint32_t lastExplosionTick = 0;
    for (uint32_t i = 0; i < kScriptTicks; i++) {
      std::vector<ExplosionOp> exps;
      if (i == kBoomAt) exps.push_back({bx, by, bz, 14, 400, 0, 0, 0});
      t++;
      // `particlesActive` is derived exactly as main.cpp derives it — history
      // plus the snapshot's particle count, never frame timing (rule 1) — and
      // is latched BEFORE the tick that consumes it, same order as the game.
      if (!exps.empty()) {
        exploded = true;
        lastExplosionTick = t;
      }
      const bool pactive =
          exploded && (t - lastExplosionTick < kPipelineTail ||
                       world.Snap().particleCount > 0);

      const uint64_t before = sim.CaSkipCount();
      SubmitTick(ctx, world, sim, t, kDefaultSeed, {}, exps, {},
                 /*hashEnable=*/true, {8, 3, 8}, /*wantReadback=*/true, pactive);
      hashes[run].push_back(ReadHashSync(ctx, world));
      if (run == 0 && sim.CaSkipCount() != before) {
        if (pactive) skipsWhileParticlesActive++;
        if (firstSkipAfterBoom < 0 && i > kBoomAt)
          firstSkipAfterBoom = (int)(i - kBoomAt);
      }
    }
    ctx.WaitIdle();
    skips[run] = sim.CaSkipCount() - scriptSkips0;
    settleSkips[run] = scriptSkips0 - skips0;

    if (run == 0) {
      // Every ejected voxel must have reinserted or died: a particle still
      // alive here would mean the script never actually reached the settled
      // state the skip is supposed to be taken in, and the differential below
      // would be testing nothing.
      uint32_t counts[2] = {};
      ReadCountsSync(ctx, world, counts);
      particlesLeft = std::min(counts[sim.Page()], kParticleCap);
      if (world.Snap().valid) faults = world.Snap().pageFaults;
    }
  }
  sim.SetCaForced(false);

  // Run 1 must have taken NO skips (that is what forced means, and it is what
  // makes it an oracle rather than a second sample of the same code path); run
  // 0 must have taken some, or the differential proved nothing.
  const uint64_t run0Skips = skips[0];
  const uint64_t run1Skips = skips[1];

  size_t firstDiff = hashes[0].size();
  for (size_t i = 0; i < hashes[0].size() && i < hashes[1].size(); i++)
    if (hashes[0][i] != hashes[1][i]) { firstDiff = i; break; }
  const bool identical = hashes[0] == hashes[1];

  if (!identical && firstDiff < hashes[0].size()) {
    std::printf(
        "  ca-skip: hash diverged at scripted tick %zu (%08x skip-on vs %08x "
        "forced).\n"
        "  The CA latch cleared while work was still pending — a chunk was\n"
        "  processed one tick late. See simulation.cpp's reinsertion-window\n"
        "  argument; this is the failure it exists to prevent.\n",
        firstDiff, hashes[0][firstDiff], hashes[1][firstDiff]);
  }

  const bool ok = identical && particlesLeft == 0 && faults == 0 &&
                  run1Skips == 0 && skipsWhileParticlesActive > 0;
  char got[16];
  std::snprintf(got, sizeof(got), "%08x",
                hashes[0].empty() ? 0u : hashes[0].back());
  detail = Format(
      "hash %s identical over %zu scripted ticks skip-on vs forced, %llu / %zu "
      "skipped (%llu of them with the particle pipeline live, first %d ticks "
      "after the blast), forced run skipped %llu, %llu skips over the settle, "
      "%u particles alive, %u page faults",
      got, hashes[0].size(), (unsigned long long)run0Skips, hashes[0].size(),
      (unsigned long long)skipsWhileParticlesActive, firstSkipAfterBoom,
      (unsigned long long)run1Skips, (unsigned long long)settleSkips[0],
      particlesLeft, faults);
  // The harness does not reprint a verdict — the gate bodies own their console
  // line (see Run() in selftest.cpp).
  std::printf("ca-skip: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CaGates() {
  static const std::vector<Gate> g = {
      {"ca-skip", "sim", {}, false, GateCaSkip},
  };
  return g;
}

}  // namespace selftest
