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

// ---- ca-slope ------------------------------------------------------------
//
// THE FOUNDING COMPLAINT, as a gate: *"on a hill it'll clump and settle on the
// hill instead of flowing down."*
//
// No existing test measures that. `--fluid-bench hill0` is the closest, and it
// cannot: WP3's settle veto refuses to convert perched water, so on the hill
// scene ~95% of the mass stays MLS-MPM particles and the CA never receives it
// (measured at cb1b4b9: 2,131 of 39,600 eighths standing). To ask whether the
// CELLULAR AUTOMATON can carry water down a slope you have to hand the water to
// the CA and nothing else, which is what this does — no particles, no seam, no
// solver, just voxel water on a stepped stone ramp.
//
// THE GEOMETRY IS THE POINT. Treads are TWO cells wide with a one-voxel riser,
// the dominant shape of the lab hill's ~31 deg ramp (HillDrop steps 1 voxel
// every 1-2 columns). A 2-wide tread is the exact case the old rules could not
// drain: the tread's INNER cell has stone below it and stone on both
// down-diagonals, so its only exit is one lateral step to the lip — which the
// old `f >= 2` gate refuses the moment the cell is down to its last eighth, and
// lateral spread is repeated halving, so every cell reaches its last eighth.
//
// THREE THINGS ARE ASSERTED, and the third is as load-bearing as the first:
//   1. MASS is exact — eighths in == eighths out. Every path through the
//      liquid rules moves eighths through transferLiquid/tryMove or it is a
//      leak, and a "drained" ramp that lost its water is not a pass.
//   2. The water ARRIVES: >= 90% of the poured eighths end in the catch basin.
//   3. The box goes IDLE — every chunk of the structure asleep. Mobility that
//      costs the sleep guarantee is not a fix, it is CLAUDE.md rule 2 traded
//      for a screenshot, so the drain and the sleep are one verdict.
Status GateCaSlope(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // Pin DIM DAWN, for the reason the fluid-settle gate records: freezing needs
  // night and evaporation needs minLight 120, so both authored water sinks are
  // off and the mass audit is exact. A roof does NOT substitute — `seesSky`
  // probes one cell up and water is not a ray blocker, so a stacked column
  // sees sky through its own surface.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  // Same neighbourhood the flung-liquid and fluid-settle gates use: known to
  // sit inside the residency window with nothing else going on around it.
  const int px = 96, py = 120, pz = 96;
  const int kTreads = 8;     // 2-wide treads, one voxel of drop each
  const int kDeck = 4;       // deck length in x — the pour lands here
  const int kW = 6;          // channel interior width in z
  const int kPit = 4;        // catch basin depth below the last tread
  const int kBasin = 8;      // catch basin length in x
  const int kTicks = 400;

  const int rampX0 = px + kDeck;
  const int basinX0 = rampX0 + 2 * kTreads;
  const int basinX1 = basinX0 + kBasin - 1;
  const int pitFloor = py - 1 - kTreads - kPit;
  const int floorY = pitFloor - 2;          // 2 cells of stone under the pit
  const int roofY = py + 6;
  const int x0 = px - 2, x1 = basinX1 + 2;
  const int z0 = pz - 2, z1 = pz + kW + 1;

  // The solid top of the column at x, or `floorY - 1` for the open pit floor.
  auto columnTop = [&](int x, int z) -> int {
    if (z < pz || z >= pz + kW) return roofY;          // channel side walls
    if (x < px) return roofY;                          // back wall
    if (x > basinX1) return roofY;                     // far wall
    if (x < rampX0) return py;                         // pour deck
    if (x < basinX0) return py - 1 - (x - rampX0) / 2; // the stepped ramp
    return pitFloor;                                   // catch basin floor
  };

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // ---- build: stone where the structure is, AIR everywhere else in the box,
  // so the chamber is clean whatever worldgen put here. A roof over the whole
  // thing keeps anything falling from above out of the audit.
  std::vector<CellOp> build;
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++) {
      const int top = columnTop(x, z);
      for (int y = floorY; y <= roofY + 1; y++) {
        const bool solid = y <= top || y >= roofY;
        build.push_back({World::SlotCellIndex({x, y, z}),
                         solid ? (uint32_t)kMatStone : 0u});
      }
    }

  // ---- the pour: full water cells standing on the deck. Liquids carry
  // fullness in the state nibble, and 7 is "8 eighths" (LIQ_FULL_STATE).
  std::vector<CellOp> pour;
  for (int y = py + 1; y <= py + 4; y++)
    for (int z = pz; z < pz + kW; z++)
      for (int x = px; x < px + kDeck; x++)
        pour.push_back({World::SlotCellIndex({x, y, z}),
                        (waterId & 0xFFFu) | (7u << 12)});
  const uint32_t poured = (uint32_t)pour.size() * 8u;

  // The chunks the structure occupies — the idle check is LOCAL, because the
  // rest of the generated world is still settling from worldgen at these tick
  // counts and would swamp a global count.
  std::vector<uint32_t> boxChunks;
  for (int cz = z0 >> 4; cz <= (z1 >> 4); cz++)
    for (int cy = floorY >> 4; cy <= ((roofY + 1) >> 4); cy++)
      for (int cx = x0 >> 4; cx <= (x1 >> 4); cx++)
        boxChunks.push_back(World::SlotChunkIndex({cx, cy, cz}));

  uint32_t t = 30000;
  int quietAt = -1;
  uint32_t activeInBox = 0;
  for (int i = 0; i < kTicks; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
               i == 0   ? build
               : i == 2 ? pour
                        : std::vector<CellOp>{},
               false, {6, 7, 6}, false, false);
    if (i >= 10 && i % 10 == 0) {
      ctx.WaitIdle();
      std::vector<uint32_t> flags(kNumChunks, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumChunks * 4, "slopeActive");
      activeInBox = 0;
      for (uint32_t ci : boxChunks)
        if (flags[ci] != 0) activeInBox++;
      if (activeInBox == 0 && quietAt < 0) quietAt = i;
    }
  }
  ctx.WaitIdle();

  // ---- where did the water end up? ----------------------------------------
  uint64_t deckE = 0, rampE = 0, basinE = 0, elseE = 0;
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = z0 >> 4; cz <= (z1 >> 4); cz++)
      for (int cy = floorY >> 4; cy <= ((roofY + 1) >> 4); cy++)
        for (int cx = x0 >> 4; cx <= (x1 >> 4); cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "slopeVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != waterId) continue;
            const int x = (int)(k % 16) + cx * 16,
                      y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (x < x0 || x > x1 || y < floorY || y > roofY + 1 || z < z0 ||
                z > z1)
              continue;
            const uint64_t e = ((cbuf[k] >> 12) & 0xFu) + 1u;
            if (x >= basinX0) basinE += e;
            else if (x >= rampX0) rampE += e;
            else if (x >= px) deckE += e;
            else elseE += e;
          }
        }
  }
  SetCurrentTuning(saved);

  const uint64_t total = deckE + rampE + basinE + elseE;
  const bool massOk = total == poured;
  const double drain = poured ? (double)basinE / (double)poured : 0.0;
  const bool drainOk = drain >= 0.90;
  const bool idleOk = activeInBox == 0;
  const bool ok = massOk && drainOk && idleOk;

  detail = Format(
      "%llu eighths poured on the deck, %.1f%% reached the basin (%llu basin / "
      "%llu ramp / %llu deck / %llu outside), mass %s, %u of %zu structure "
      "chunks awake at tick %d (quiet from %d)",
      (unsigned long long)poured, drain * 100.0,
      (unsigned long long)basinE, (unsigned long long)rampE,
      (unsigned long long)deckE, (unsigned long long)elseE,
      massOk ? "EXACT" : "LEAK", activeInBox, boxChunks.size(), kTicks,
      quietAt);
  std::printf("ca-slope: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CaGates() {
  static const std::vector<Gate> g = {
      {"ca-skip", "sim", {}, false, GateCaSkip},
      {"ca-slope", "sim", {}, false, GateCaSlope},
  };
  return g;
}

}  // namespace selftest
