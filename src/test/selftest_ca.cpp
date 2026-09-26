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

#include <algorithm>
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

  // ---- SUBJECT ISOLATION: no governed body of water in this fixture -------
  //
  // The subject here is the CA SKIP MACHINERY, not water governance. The
  // assertion below — that skips are taken on ticks where the particle pipeline
  // is recorded — was authored against a world whose lake is an ordinary CA
  // pond, and its premise is that on a quiet tick NOTHING is moving. From the
  // day `sim.waterBodyMode` shipped at 1 that premise is false: the harness lake
  // sits beside the scripted blast, the blast is a mutation, and a mutation arms
  // the body's hot window — so for the rest of the scripted window the relevel
  // is doing GENUINE work, marking chunks dirty, and `NoteSnapshot` correctly
  // refuses the idle proof. The gate then measures the water system.
  //
  // MEASURED, the commit that flipped the default, in three same-scope arms:
  // shipped config 10 of 220 skips and 0 with particles live; `waterBodyMode 0`
  // 155 of 220 and 95 with particles; `waveMode 0` bit-identical to the shipped
  // tree, and disarming the drain's op reservation likewise — so it is
  // governance itself and neither W2 nor W3.
  //
  // This is pass H's situation exactly (a fixture that measures the discharge
  // law had to disarm the surface-momentum layer once it shipped on) and it
  // takes pass H's fix: disarm the foreign system in the fixture that does not
  // measure it. NOTHING about the thresholds moves. The CORRECTNESS half —
  // skip-on and forced produce identical hash sequences — is untouched by this
  // pin and still runs against whatever else the shipped config does; and the
  // shipped-config interaction (a governed lake settles and stops waking
  // chunks) is asserted where it belongs, by the `waterbody` passes' own
  // awake-at-rest bounds.
  const Tuning caSaved = CurrentTuning();
  {
    Tuning ct = caSaved;
    ct.sim.waterBodyMode = 0;
    SetCurrentTuning(ct);
  }

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
  // The pin is this gate's own world and nobody else's. Restored before the
  // verdict so a later gate inherits the shipped config, exactly as the dawn
  // pins further down this file restore theirs.
  SetCurrentTuning(caSaved);

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
//
// ---- TWO ARMS, since WP5 flipped sim.fluidExciteMode to 1 -------------------
//
// The paragraph above says "no particles, no seam, no solver". At the shipped
// default that is no longer what this script produces — the seam takes the
// water off the deck and the gate stops measuring the CA at all. Measured, at
// exciteMode 1 with the original single-arm audit: 0.1% in the basin, 731 of
// 768 eighths simply ABSENT from a sweep that only counts voxels, reported as
// a mass LEAK. Neither number was about the CA.
//
// So the gate splits, and the split is not a suppression — both arms assert:
//
//   `ca-slope`         pins exciteMode 0. THE CA ALONE, which is the question
//                      the gate was written to ask and the one merge a2e723e's
//                      four fixes answer. Its acceptance is unchanged: >=90% in
//                      the basin, mass exact, box asleep.
//   `ca-slope-hybrid`  runs the SAME script at the shipped default, with both
//                      movers live in one scene. This is WP5's own acceptance
//                      criterion in the suite: the ledger has to close across
//                      the seam (standing eighths + eighths carried by live
//                      particles == poured), the water still has to ARRIVE, and
//                      the box still has to go quiet.
//
// The hybrid arm's audit is the strictly richer one — it classifies particles
// into the same deck/ramp/basin bands as voxels — because "where is the water"
// and "which representation is it in" are different questions and only the
// first one is the founding complaint.
struct SlopeArm {
  int exciteMode;
  double minDrain;     // fraction of poured eighths that must reach the basin
  bool requireIdle;
};

Status RunCaSlope(Ctx& c, std::string& detail, const SlopeArm& arm) {
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
  // The arm's whole configuration. exciteMode is a live CPU-read knob, so
  // neither arm needs a shader reload.
  dawn.sim.fluidExciteMode = arm.exciteMode;
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

  // AUDIT-ONLY WIDENING, and it is load-bearing at exciteMode 1. Excited water
  // is BALLISTIC: it hits the far wall of the channel with momentum the CA
  // never had. WP4 recorded the lab `pool` scene's ledger as LEAK for exactly
  // this before the bench widened its own sweep. The structure still builds
  // from the tight bounds; only the mass audit looks wider, and everything it
  // finds out there lands in `elseE` — reported, not silently forgiven.
  const int ax0 = x0 - 12, ax1 = x1 + 12, az0 = z0 - 12, az1 = z1 + 12;
  const int ay0 = floorY - 8, ay1 = roofY + 9;
  uint64_t deckE = 0, rampE = 0, basinE = 0, elseE = 0;
  auto sweepVoxels = [&]() -> uint64_t {
    deckE = 0; rampE = 0; basinE = 0; elseE = 0;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = az0 >> 4; cz <= (az1 >> 4); cz++)
      for (int cy = ay0 >> 4; cy <= (ay1 >> 4); cy++)
        for (int cx = ax0 >> 4; cx <= (ax1 >> 4); cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "slopeVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != waterId) continue;
            const int x = (int)(k % 16) + cx * 16,
                      y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (x < ax0 || x > ax1 || y < ay0 || y > ay1 || z < az0 ||
                z > az1)
              continue;
            const bool inBox = x >= x0 && x <= x1 && y >= floorY &&
                               y <= roofY + 1 && z >= z0 && z <= z1;
            const uint64_t e = ((cbuf[k] >> 12) & 0xFu) + 1u;
            if (!inBox) elseE += e;
            else if (x >= basinX0) basinE += e;
            else if (x >= rampX0) rampE += e;
            else if (x >= px) deckE += e;
            else elseE += e;
          }
        }
    return deckE + rampE + basinE + elseE;
  };

  uint32_t t = 30000;
  int quietAt = -1;
  uint32_t activeInBox = 0;
  // The seam's per-tick event counters, accumulated. They are the ONLY way to
  // read a mass verdict: "LEAK" with no breakdown says a number did not add up
  // and nothing about which of the four sinks took it, and the four have
  // completely different fixes. Cheap — 64 bytes a tick, no extra sync beyond
  // what ReadbackBlocking already does.
  uint64_t sumConsumed = 0, sumBinned = 0, sumSettled = 0, sumExcited = 0;
  uint64_t sumEmitted = 0, sumDead = 0, sumRefused = 0;
  int divergeAt = -1;
  long long divergeBy = 0;
  // IS THE LEFTOVER POOL STILL DRAINING, OR IS IT STUCK? The hybrid arm ends
  // with live particles that are not in the basin, and "N particles at the end"
  // cannot tell a slow drain from a permanent residue — which is the whole
  // difference between "run it longer" and "rule 2 is violated, this pool never
  // goes away". The count at four points does: falling means draining, flat
  // means stuck. fa[7] is already read every tick for the ledger.
  uint32_t liveAt[4] = {0, 0, 0, 0};
  for (int i = 0; i < kTicks; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
               i == 0   ? build
               : i == 2 ? pour
                        : std::vector<CellOp>{},
               false, {6, 7, 6}, false, false);
    {
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            sizeof(fa), "slopeTickArgs");
      sumConsumed += fa[16];   // FA_CONSUMED: eighths eaten by CA reactions
      sumBinned += fa[15];     // FA_BINNED:   eighths settle could not place
      sumSettled += fa[10];
      sumExcited += fa[11];
      sumEmitted += fa[9];    // FA_EMITTED: PARTICLES emitted (1 eighth each)
      sumDead += fa[8];       // FA_DEAD:    particles killed this tick
      sumRefused += fa[12];
      const int q = (i + 1) * 4 / kTicks;  // quarter of the run, 1..4
      if (q >= 1 && q <= 4) liveAt[q - 1] = std::min(fa[7], kFluidCap);
    }
    if (i >= 10 && i % 10 == 0) {
      ctx.WaitIdle();
      // WHERE the ledger first parts company, which is the only question a
      // whole-run "LEAK" cannot answer. Expected standing voxel mass is the
      // pour minus the seam's own net conversion; a gap means eighths left the
      // voxel grid without the seam recording that it took them.
      if (divergeAt < 0) {
        const uint64_t standing = sweepVoxels();
        const long long expect =
            (long long)poured - (long long)sumExcited + (long long)sumSettled;
        if ((long long)standing != expect) {
          divergeAt = i;
          divergeBy = expect - (long long)standing;
        }
      }
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumSlots * 4, "slopeActive");
      activeInBox = 0;
      for (uint32_t ci : boxChunks)
        if (flags[ci] != 0) activeInBox++;
      if (activeInBox == 0 && quietAt < 0) quietAt = i;
    }
  }
  ctx.WaitIdle();

  // ---- where did the water end up? ----------------------------------------
  sweepVoxels();

  // ---- and how much of it is still PARTICLES? -----------------------------
  // Only the hybrid arm can produce any, but the sweep runs on both arms: a
  // CA-only arm that somehow excited water would otherwise report a silent
  // mass leak, which is the failure mode this whole block exists to name.
  // Particles are classified into the same x-bands as voxels, because "the
  // water reached the basin" must not depend on whether it settled first.
  uint64_t liveE = 0, liveBasinE = 0;
  uint32_t liveCount = 0;
  {
    uint32_t fa[16] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                          64, "slopeArgs");
    liveCount = std::min(fa[7], kFluidCap);   // FA_LIVE, as the fluid gates read it
    if (liveCount > 0) {
      std::vector<uint32_t> pbuf((size_t)liveCount * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "slopeParts");
      for (uint32_t k = 0; k < liveCount; k++) {
        const uint32_t* pw = pbuf.data() + (size_t)k * kFluidParticleWords;
        const uint64_t e = (pw[18] >> 12) & 0x7u;
        liveE += e;
        if (((int32_t)pw[0] >> 16) >= basinX0) liveBasinE += e;
      }
    }
  }
  SetCurrentTuning(saved);

  const uint64_t total = deckE + rampE + basinE + elseE + liveE;
  const bool massOk = total == poured;
  const double drain =
      poured ? (double)(basinE + liveBasinE) / (double)poured : 0.0;
  const bool drainOk = drain >= arm.minDrain;
  const bool idleOk = !arm.requireIdle || activeInBox == 0;
  const bool ok = massOk && drainOk && idleOk;

  detail = Format(
      "%llu eighths poured on the deck, %.1f%% reached the basin (%llu basin / "
      "%llu ramp / %llu deck / %llu outside / %llu carried by %u particles, "
      "%llu of them in the basin), mass %s (%lld unaccounted; seam over the run: "
      "%llu excited -> %llu emitted, %llu settled, %llu dead, %llu refused, "
      "%llu binned, %llu eaten by reactions; ledger first parted at tick %d "
      "by %lld), live particles by quarter of the run %u/%u/%u/%u (flat means "
      "the residue is permanent, not slow), %u of "
      "%zu structure chunks awake at tick %d (quiet from %d)",
      (unsigned long long)poured, drain * 100.0,
      (unsigned long long)basinE, (unsigned long long)rampE,
      (unsigned long long)deckE, (unsigned long long)elseE,
      (unsigned long long)liveE, liveCount, (unsigned long long)liveBasinE,
      massOk ? "EXACT" : "LEAK", (long long)poured - (long long)total,
      (unsigned long long)sumExcited, (unsigned long long)sumEmitted,
      (unsigned long long)sumSettled, (unsigned long long)sumDead,
      (unsigned long long)sumRefused,
      (unsigned long long)sumBinned, (unsigned long long)sumConsumed,
      divergeAt, divergeBy, liveAt[0], liveAt[1], liveAt[2], liveAt[3],
      activeInBox, boxChunks.size(), kTicks, quietAt);
  return ok ? Status::Pass : Status::Fail;
}

// ---- ca-level ------------------------------------------------------------
//
// THE SECOND COMPLAINT, as a gate: *"if I add a splash of water onto a pond it
// doesn't propagate, it just sits on top and creates an elevated mound instead
// of dispersing and trying to be as flat as possible."*
//
// `ca-slope` asks whether water gets DOWN a hill. This asks what shape it comes
// to rest in once it is somewhere flat, which turns out to be a different
// question with a different answer. The old rules settled a blob into a CONE:
// lateral spread is repeated halving so only the rim ever touches air, the
// same-liquid EQUALIZE branch only fires at a difference of `liquidEqualize`
// (2), and therefore a surface sloping by exactly ONE eighth per cell has no
// unstable pair anywhere in it and no way to advance. (8,7,6,5,4,3,2,1) was a
// stable resting state. On a pond that is a mound sitting on the surface that
// never disperses, and since b799a58 draws partial cells at fullness height it
// is a mound you can see.
//
// So the assertion is the SHAPE AT REST, and the numbers are chosen to have one
// obvious right answer rather than to be a tolerance:
//
//   1. MASS is exact, as always.
//   2. STACKING — no column of the puddle holds water in more cells than the
//      arm allows. A dome is by definition taller than a level puddle of the
//      same mass, and on a floor big enough to hold the flat answer, a level
//      puddle is exactly ONE cell deep.
//   3. PEAK — the deepest column, in eighths. The flat answer for 216 eighths
//      spread over a floor with room for 216 cells is one eighth per wetted
//      column. Two is the slack (a 2 with only 1s around it is a legal resting
//      state — the pair differs by one, which is the integer equilibrium).
//   4. IDLE — the box sleeps. A rule that levels water by never settling is
//      rule 2 traded for a screenshot, exactly as in ca-slope, and the
//      termination argument in sim_step.wgsl's filmPressed block is what this
//      line tests.
//
// THE POND ARM is the reported case literally: the same blob dropped on standing
// water, where descent is refused (the cells below are already full) so lateral
// levelling is the ONLY mechanism available. It runs at the shipped exciteMode,
// so it also covers the seam's surface-step trigger not leaving the pool
// churning — the water goes to the solver, splashes, and must come back and go
// quiet.
//
// THE HONEST LIMIT, stated because the gate is built to avoid it. `filmPressed`
// frees the RIM, and the dome then unwinds from the outside in. Where there is
// no rim — a basin filled wall to wall, so the surface has no air to advance
// into — the equalize threshold is again the only lateral rule and a slope-1
// surface is again stable. That is bounded by the basin's width and is the
// plateau problem in its irreducible form: moving one eighth from the middle of
// a ramp to its end is neutral in SUM(f*f) at every step, so no reach-1 rule can
// make it strictly downhill. Every real splash has a rim, which is why this
// gate's floor is wider than the puddle.
struct LevelArm {
  int exciteMode;
  int pondLayers;      // full water cells laid over the whole floor first
  int blob;            // splash edge, in full water cells
  uint64_t maxPeak;    // deepest column allowed, in eighths
  uint32_t maxLayers;  // most cells of water allowed in one column
};

Status RunCaLevel(Ctx& c, std::string& detail, const LevelArm& arm) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // Dim dawn, for the reason ca-slope records: freezing and evaporation are the
  // two authored sinks that would make the mass audit inexact.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  dawn.sim.fluidExciteMode = arm.exciteMode;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  const int px = 96, py = 120, pz = 96;
  const int kHalf = 14;                 // interior half-width in x and z
  const int kBlob = arm.blob;
  const int kTicks = 700;
  const int floorY = py;                // solid floor; water rests at floorY+1
  const int roofY = py + 12;
  const int x0 = px - kHalf, x1 = px + kHalf;
  const int z0 = pz - kHalf, z1 = pz + kHalf;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A sealed box: stone floor, stone walls, stone roof, air inside. Wide enough
  // that the level answer (one eighth per wetted cell) fits with room to spare —
  // see THE HONEST LIMIT above, a puddle that reaches the wall stops levelling.
  std::vector<CellOp> build;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = floorY; y <= roofY; y++) {
        const bool wall = x < x0 || x > x1 || z < z0 || z > z1;
        const bool solid = wall || y <= floorY || y >= roofY;
        build.push_back({World::SlotCellIndex({x, y, z}), solid ? (uint32_t)kMatStone : 0u});
      }

  // The pond, if this arm has one: full cells wall to wall.
  std::vector<CellOp> pond;
  for (int L = 0; L < arm.pondLayers; L++)
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++)
        pond.push_back({World::SlotCellIndex({x, floorY + 1 + L, z}),
                        (waterId & 0xFFFu) | (7u << 12)});

  // The splash: a solid cube of full water, standing on the surface (or on the
  // floor) so it has to disperse LATERALLY. Dropping it from height would test
  // descent, which ca-slope already covers.
  std::vector<CellOp> blob;
  const int by = floorY + 1 + arm.pondLayers;
  for (int y = by; y < by + kBlob; y++)
    for (int z = pz - kBlob / 2; z <= pz + kBlob / 2; z++)
      for (int x = px - kBlob / 2; x <= px + kBlob / 2; x++)
        blob.push_back({World::SlotCellIndex({x, y, z}),
                        (waterId & 0xFFFu) | (7u << 12)});
  const uint64_t placed = ((uint64_t)pond.size() + blob.size()) * 8u;

  std::vector<uint32_t> boxChunks;
  for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
    for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
      for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++)
        boxChunks.push_back(World::SlotChunkIndex({cx, cy, cz}));

  uint32_t t = 41000;
  int quietAt = -1;
  uint32_t activeInBox = 0;
  uint64_t sumSettled = 0, sumExcited = 0;
  for (int i = 0; i < kTicks; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
               i == 0   ? build
               : i == 2 ? pond
               : i == 4 ? blob
                        : std::vector<CellOp>{},
               false, {6, 7, 6}, false, false);
    {
      uint32_t fa[32] = {};
      rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa,
                            sizeof(fa), "levelTickArgs");
      sumSettled += fa[10];
      sumExcited += fa[11];
    }
    if (i >= 20 && i % 10 == 0) {
      ctx.WaitIdle();
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumSlots * 4, "levelActive");
      activeInBox = 0;
      for (uint32_t ci : boxChunks)
        if (flags[ci] != 0) activeInBox++;
      if (activeInBox == 0 && quietAt < 0) quietAt = i;
      else if (activeInBox != 0) quietAt = -1;
    }
  }
  ctx.WaitIdle();

  // ---- the resting shape, per COLUMN --------------------------------------
  // Eighths and occupied cells for every (x,z) of the interior. The profile
  // histogram is what makes a failure readable: a dome reports a spread of
  // fullnesses, a level puddle reports one bucket.
  const int span = x1 - x0 + 1;
  std::vector<uint32_t> colE((size_t)span * span, 0), colN((size_t)span * span, 0);
  uint64_t standing = 0;
  uint32_t profile[9] = {};
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "levelVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != waterId) continue;
            const int x = (int)(k % 16) + cx * 16,
                      y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (x < x0 || x > x1 || z < z0 || z > z1) continue;
            if (y <= floorY || y >= roofY) continue;
            const uint32_t e = ((cbuf[k] >> 12) & 0xFu) + 1u;
            standing += e;
            profile[e]++;
            const size_t ci = (size_t)(z - z0) * span + (x - x0);
            colE[ci] += e;
            colN[ci]++;
          }
        }
  }

  // Water still in flight as particles (the pond arm's seam can hold some).
  uint64_t liveE = 0;
  uint32_t liveCount = 0;
  {
    uint32_t fa[16] = {};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, fa, 64,
                          "levelArgs");
    liveCount = std::min(fa[7], kFluidCap);
    if (liveCount > 0) {
      std::vector<uint32_t> pbuf((size_t)liveCount * kFluidParticleWords);
      rhi::ReadbackBlocking(ctx.device, ctx.queue,
                            world.fluidParticles[sim.Page()], 0, pbuf.data(),
                            pbuf.size() * 4, "levelParts");
      for (uint32_t k = 0; k < liveCount; k++)
        liveE += (pbuf[(size_t)k * kFluidParticleWords + 18] >> 12) & 0x7u;
    }
  }
  SetCurrentTuning(saved);

  uint64_t peak = 0;
  uint32_t layers = 0, wetted = 0;
  for (size_t i = 0; i < colE.size(); i++) {
    if (colE[i] == 0) continue;
    wetted++;
    if (colE[i] > peak) peak = colE[i];
    if (colN[i] > layers) layers = colN[i];
  }

  const bool massOk = standing + liveE == placed;
  const bool peakOk = peak <= arm.maxPeak;
  const bool layersOk = layers <= arm.maxLayers;
  const bool idleOk = activeInBox == 0;
  const bool ok = massOk && peakOk && layersOk && idleOk;

  detail = Format(
      "%llu eighths placed (%d pond layers + a %d^3 blob), at rest over %u "
      "wetted columns: deepest %llu eighths (allow %llu), tallest %u cells "
      "(allow %u), fullness profile 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u, "
      "mass %s (standing %llu + %llu in %u particles), seam %llu excited / %llu "
      "settled, %u of %zu box chunks awake at tick %d (quiet from %d)",
      (unsigned long long)placed, arm.pondLayers, kBlob, wetted,
      (unsigned long long)peak, (unsigned long long)arm.maxPeak, layers,
      arm.maxLayers, profile[1], profile[2], profile[3], profile[4], profile[5],
      profile[6], profile[7], profile[8], massOk ? "EXACT" : "LEAK",
      (unsigned long long)standing, (unsigned long long)liveE, liveCount,
      (unsigned long long)sumExcited, (unsigned long long)sumSettled,
      activeInBox, boxChunks.size(), kTicks, quietAt);
  return ok ? Status::Pass : Status::Fail;
}

// THE ALLOWANCES ARE MEASURED, not aspirational, and the arithmetic in
// sim_step.wgsl's bridgeLevel block says why they cannot all be 1: a reach-1
// rule set cannot flatten a wide shallow surface past a slope of one eighth per
// two cells, so a 216-eighth puddle keeps a residual swell in the middle. What
// these arms assert is that the swell does not GROW. Numbers taken at the commit
// that landed filmPressed + bridgeLevel; the same script on the rules before it
// reported, for reference, 57 wetted columns at 6 eighths deep with 31 cells at
// 4 or more (dry), and a surface film peaking at 5 eighths with 17 cells at 4 or
// more (pond).
Status GateCaLevelOne(Ctx& c, std::string& detail) {
  // THE OWNER'S LITERAL TEST: "if I use the smallest brush and place water on a
  // flat plane I want it to keep spreading until every single voxel is the
  // smallest height possible." One full cell is 8 eighths, so the answer is 8
  // cells of one eighth and nothing deeper — this arm allows NO slack, because
  // at this size there is no dome for the slope limit to hide in.
  const Status s = RunCaLevel(c, detail, {0, 0, 1, 1, 1});
  std::printf("ca-level-one: %s (%s)\n", s == Status::Pass ? "PASS" : "FAIL",
              detail.c_str());
  return s;
}

Status GateCaLevel(Ctx& c, std::string& detail) {
  // Dry floor, CA alone, 27 full cells. One cell deep everywhere — the
  // no-stacking assertion is the "not a mound" one — with the residual swell
  // bounded at half a voxel.
  const Status s = RunCaLevel(c, detail, {0, 0, 3, 4, 1});
  std::printf("ca-level: %s (%s)\n", s == Status::Pass ? "PASS" : "FAIL",
              detail.c_str());
  return s;
}

Status GateCaLevelPond(Ctx& c, std::string& detail) {
  // The reported case: the same blob on two full layers of standing water, at
  // the shipped exciteMode so the seam's surface-step trigger is live. Two full
  // cells is 16 eighths, so the allowance is 16 plus the film's own swell, and
  // the layer count is what says the splash did not stay a lump on the surface.
  const Status s = RunCaLevel(c, detail, {1, 2, 3, 20, 3});
  std::printf("ca-level-pond: %s (%s)\n", s == Status::Pass ? "PASS" : "FAIL",
              detail.c_str());
  return s;
}

// ---------------------------------------------------------------------------
// oil-slick — oil on water stays a slick, not a checkerboard
// ---------------------------------------------------------------------------
// The owner, 2026-09-24: "when oil pours on top of water it often makes weird
// checkerboard crazy patterns instead of tending to be clumped up ... if I
// remove some or burn some then they all go crazy crawling around and
// separating into this pattern."
//
// The mechanism was stepLiquid's same-level LATERAL DISPLACE: water beside a
// lighter fluid at the same height swapped whole cells with it, oil included.
// That swap has no driving force (same y, same SUM(f*f)), so it is an unbiased
// random walk of every oil cell through the water's surface layer — diffusion,
// which is the opposite of immiscible. It only ran under FILM_LICENCE, which is
// why a settled slick sat still and a disturbed one (a hole cut, a cell burnt)
// dissolved into isolated cells.
//
// FIXTURE. A sealed stone box, two full water layers and a third at HALF
// fullness (a real lake surface has partial cells, and the half layer is where
// oil and water share a level). A 6x6 slab of full oil is laid IN that layer,
// settles, then the -x half of whatever oil is left is deleted — the owner's
// "remove some". Measured at the end:
//   ISOLATED  oil cells with no oil face-neighbour, as a fraction of oil cells.
//             A checkerboard is ~all isolated; a slick is ~none.
//   LARGEST   the biggest 6-connected oil component's share of the oil cells.
//   IDLE      the box sleeps (rule 2).
// Thresholds live in baseline.json (oilSlick.*).
//
// DIRECT PHASE CALLS ON PURPOSE (W2-O): a sim-only CA fixture, like every
// ca-* gate in this file — it tests the liquid rules, and nothing the rest of
// the tick grows (mobs, bodies, the celestial clock) may touch a sealed box.
Status GateOilSlick(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0, oilId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (c.mats[i].name == "oil") oilId = (uint32_t)i;
  }
  if (!waterId || !oilId) { detail = "no 'water' or 'oil' material"; return Status::Fail; }

  // Dim dawn, as ca-level: no freezing or evaporation in the box.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  const int px = 96, py = 120, pz = 96;
  const int kHalf = 14;
  const int floorY = py, roofY = py + 12;
  const int x0 = px - kHalf, x1 = px + kHalf;
  const int z0 = pz - kHalf, z1 = pz + kHalf;
  const int surfY = floorY + 3;  // the half-full layer oil shares
  const int kRemoveAt = 200, kTicks = 700;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  std::vector<CellOp> build, pond, slab;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = floorY; y <= roofY; y++) {
        const bool wall = x < x0 || x > x1 || z < z0 || z > z1;
        const bool solid = wall || y <= floorY || y >= roofY;
        build.push_back({World::SlotCellIndex({x, y, z}), solid ? (uint32_t)kMatStone : 0u});
      }
  for (int z = z0; z <= z1; z++)
    for (int x = x0; x <= x1; x++)
      for (int y = floorY + 1; y <= surfY; y++)
        pond.push_back({World::SlotCellIndex({x, y, z}),
                        (waterId & 0xFFFu) | ((y == surfY ? 3u : 7u) << 12)});
  for (int z = pz - 3; z < pz + 3; z++)
    for (int x = px - 3; x < px + 3; x++)
      slab.push_back({World::SlotCellIndex({x, surfY, z}), (oilId & 0xFFFu) | (7u << 12)});

  std::vector<uint32_t> boxChunks;
  for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
    for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
      for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++)
        boxChunks.push_back(World::SlotChunkIndex({cx, cy, cz}));

  // The oil in the box, as slot cells.
  struct Cell { int x, y, z; };
  auto readOil = [&]() {
    std::vector<Cell> out;
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "oilSlickVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != oilId) continue;
            const int x = (int)(k % 16) + cx * 16, y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (x < x0 || x > x1 || z < z0 || z > z1 || y <= floorY || y >= roofY) continue;
            out.push_back({x, y, z});
          }
        }
    return out;
  };

  uint32_t t = 43000;
  uint32_t activeInBox = 0;
  for (int i = 0; i < kTicks; i++) {
    std::vector<CellOp> ops = i == 0 ? build : i == 2 ? pond : i == 4 ? slab
                                                                     : std::vector<CellOp>{};
    if (i == kRemoveAt) {
      ctx.WaitIdle();
      for (const Cell& o : readOil())
        if (o.x < px) ops.push_back({World::SlotCellIndex({o.x, o.y, o.z}), 0u});
    }
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, ops, false, {6, 7, 6},
               false, false);
    if (i == kTicks - 1) {
      ctx.WaitIdle();
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumSlots * 4, "oilSlickActive");
      for (uint32_t ci : boxChunks)
        if (flags[ci] != 0) activeInBox++;
    }
  }
  ctx.WaitIdle();
  SetCurrentTuning(saved);

  // Components over the box interior, 6-connected.
  const std::vector<Cell> oil = readOil();
  const int sx = x1 - x0 + 1, sy = roofY - floorY + 1;
  auto key = [&](int x, int y, int z) { return ((size_t)(z - z0) * sy + (y - floorY)) * sx + (x - x0); };
  std::vector<int> comp((size_t)sx * sy * sx, -2);  // -2 no oil, -1 unvisited
  for (const Cell& o : oil) comp[key(o.x, o.y, o.z)] = -1;
  const int dirs[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  auto isOil = [&](int x, int y, int z) {
    return x >= x0 && x <= x1 && z >= z0 && z <= z1 && y > floorY && y < roofY &&
           comp[key(x, y, z)] != -2;
  };
  uint32_t isolated = 0, comps = 0, largest = 0;
  std::vector<Cell> stack;
  for (const Cell& o : oil) {
    bool lone = true;
    for (auto& d : dirs) if (isOil(o.x + d[0], o.y + d[1], o.z + d[2])) lone = false;
    if (lone) isolated++;
    if (comp[key(o.x, o.y, o.z)] != -1) continue;
    uint32_t n = 0;
    stack.push_back(o);
    comp[key(o.x, o.y, o.z)] = (int)comps;
    while (!stack.empty()) {
      const Cell q = stack.back(); stack.pop_back(); n++;
      for (auto& d : dirs) {
        const int ax = q.x + d[0], ay = q.y + d[1], az = q.z + d[2];
        if (!isOil(ax, ay, az) || comp[key(ax, ay, az)] != -1) continue;
        comp[key(ax, ay, az)] = (int)comps;
        stack.push_back({ax, ay, az});
      }
    }
    comps++;
    largest = std::max(largest, n);
  }

  const double nOil = (double)std::max<size_t>(oil.size(), 1);
  const double isoFrac = isolated / nOil, bigFrac = largest / nOil;
  const double isoMax = BaselineNumber("oilSlick.isolatedMax", 0.2);
  const double bigMin = BaselineNumber("oilSlick.largestMin", 0.5);
  RecordObserved("oilSlick.isolatedObserved", isoFrac);
  RecordObserved("oilSlick.largestObserved", bigFrac);
  const bool ok = !oil.empty() && isoFrac <= isoMax && bigFrac >= bigMin && activeInBox == 0;
  detail = Format(
      "%zu oil cells after removing the -x half at tick %d: %u isolated (%.2f, allow "
      "%.2f), %u components, largest %u (%.2f, need %.2f), %u of %zu box chunks "
      "awake at tick %d",
      oil.size(), kRemoveAt, isolated, isoFrac, isoMax, comps, largest, bigFrac,
      bigMin, activeInBox, boxChunks.size(), kTicks);
  std::printf("oil-slick: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---------------------------------------------------------------------------
// plant-crush — passable vegetation holds nothing up
// ---------------------------------------------------------------------------
// The owner, 2026-09-25: voxels thrown off a killed mob "land on top of and
// float above small prefab plant objects; bramble probably shouldn't be able to
// support or hold voxels above it". A passable plant is one full SOLID cell
// drawn as a small micro-model, so anything resting on it sat a voxel up.
//
// FIXTURE. A sealed stone box, four brambles on its floor, and one thing
// dropped on each of three of them, each through a different door:
//   A  sand          the CA powder fall (tryCrush in sim_step.wgsl)
//   B  a lone stone  the CA one-voxel-island fall (soloSolid + tryCrush)
//   C  a particle    grid ejecta, what a carve throws (sim_particle.wgsl)
//   D  nothing       control: a plant with nothing on it stays a plant
// Asserted: A, B, C each end ON THE FLOOR (the bramble under them crushed) with
// air above; D is still a bramble.
//
// DIRECT PHASE CALLS ON PURPOSE (W2-O): a sim-only CA fixture in a sealed box,
// like every ca-* gate in this file.
Status GatePlantCrush(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t brambleId = 0, sandId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "bramble") brambleId = (uint32_t)i;
    if (c.mats[i].name == "sand") sandId = (uint32_t)i;
  }
  if (!brambleId || !sandId) { detail = "no 'bramble' or 'sand' material"; return Status::Fail; }

  const int px = 96, py = 120, pz = 96, kHalf = 9;
  const int floorY = py, roofY = py + 12;
  const int x0 = px - kHalf, x1 = px + kHalf, z0 = pz - kHalf, z1 = pz + kHalf;
  const int plantY = floorY + 1, dropY = floorY + 5;
  const int xA = px - 6, xB = px - 2, xC = px + 2, xD = px + 6;
  const int kTicks = 160;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  std::vector<CellOp> build, drops;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = floorY; y <= roofY; y++) {
        const bool solid = x < x0 || x > x1 || z < z0 || z > z1 || y <= floorY || y >= roofY;
        // In the same pass, never as a second op on the cell: two cell ops on
        // one cell keep the FIRST (sim/oprecord.h), so a bramble appended
        // after the box's air write would silently never exist.
        const bool plant = y == plantY && z == pz &&
                           (x == xA || x == xB || x == xC || x == xD);
        build.push_back({World::SlotCellIndex({x, y, z}),
                         plant ? (brambleId & 0xFFFu) : solid ? (uint32_t)kMatStone : 0u});
      }
  drops.push_back({World::SlotCellIndex({xA, dropY, pz}), sandId & 0xFFFu});
  drops.push_back({World::SlotCellIndex({xB, dropY, pz}), (uint32_t)kMatStone});
  // Cell centre in 24.8 fixed point, falling half a voxel a tick.
  ParticleSpawn ps{};
  ps.px = xC * 256 + 128; ps.py = (dropY + 2) * 256 + 128; ps.pz = pz * 256 + 128;
  ps.vy = -128;
  ps.payload = (uint32_t)kMatStone;

  uint32_t t = 47000;
  for (int i = 0; i < kTicks; i++) {
    const std::vector<CellOp> ops = i == 0 ? build : i == 2 ? drops : std::vector<CellOp>{};
    std::vector<ParticleSpawn> spawns;
    if (i == 2) spawns.push_back(ps);
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, ops, false,
               {px >> 4, py >> 4, pz >> 4}, false, true, spawns);
  }
  ctx.WaitIdle();

  std::vector<uint32_t> cbuf((size_t)kChunkVol);
  auto matAt = [&](int x, int y, int z) -> uint32_t {
    ReadVoxelsSync(ctx, world, World::SlotChunkIndex({x >> 4, y >> 4, z >> 4}), 1,
                   cbuf.data(), "plantCrushVox");
    return cbuf[(size_t)(x & 15) + (size_t)(y & 15) * 16 + (size_t)(z & 15) * 256] & 0xFFFu;
  };
  // Each column, floor up to the drop height: what is on the floor, and the
  // highest non-air cell (the thing floating, if anything is).
  struct Col { uint32_t floorMat; int top; };
  auto column = [&](int x) {
    Col col{matAt(x, plantY, pz), plantY - 1};
    for (int y = plantY; y <= dropY + 3; y++)
      if (matAt(x, y, pz) != 0) col.top = y;
    return col;
  };
  const Col a = column(xA), b = column(xB), cc = column(xC), d = column(xD);
  const bool okA = a.floorMat == sandId && a.top == plantY;
  const bool okB = b.floorMat == (uint32_t)kMatStone && b.top == plantY;
  const bool okC = cc.floorMat == (uint32_t)kMatStone && cc.top == plantY;
  const bool okD = d.floorMat == brambleId && d.top == plantY;
  const bool ok = okA && okB && okC && okD;
  auto desc = [&](const Col& col) {
    return Format("floor mat %u, top y+%d", col.floorMat, col.top - floorY);
  };
  detail = Format("sand %s [%s]; stone %s [%s]; particle %s [%s]; control %s [%s] "
                  "(bramble %u, sand %u, stone %u)",
                  okA ? "ok" : "FAIL", desc(a).c_str(), okB ? "ok" : "FAIL",
                  desc(b).c_str(), okC ? "ok" : "FAIL", desc(cc).c_str(),
                  okD ? "ok" : "FAIL", desc(d).c_str(), brambleId, sandId,
                  (uint32_t)kMatStone);
  std::printf("plant-crush: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

Status GateCaSlope(Ctx& c, std::string& detail) {
  const Status s = RunCaSlope(c, detail, {0, 0.90, true});
  std::printf("ca-slope: %s (%s)\n", s == Status::Pass ? "PASS" : "FAIL",
              detail.c_str());
  return s;
}

Status GateCaSlopeHybrid(Ctx& c, std::string& detail) {
  const Status s = RunCaSlope(c, detail, {1, 0.90, true});
  std::printf("ca-slope-hybrid: %s (%s)\n", s == Status::Pass ? "PASS" : "FAIL",
              detail.c_str());
  return s;
}

// ---- ca-gutter -----------------------------------------------------------
//
// THE GEOMETRY THE THIN-FILM RISER STEP CANNOT SEE, as a gate.
//
// `ca-slope` asks whether water gets DOWN a stepped hill, and the rule that
// makes it — a lone eighth may step away from a riser exactly one voxel proud
// of its own level — was written for the 2-cell TREAD in that gate's ramp.
// sim_step.wgsl's own block admitted, at the time, the one shape it does not
// terminate on: two such risers FACING each other with two air cells between
// them. A film in there steps away from one riser into the foot of the other,
// forever, and no reach-1 predicate can break the tie because the two cells
// have byte-identical 3x3x3 neighbourhoods.
//
// It was left to the `sleep` gate to say whether the generated world contains
// that shape. On 2026-09-08 it did: eight chunks awake permanently at the
// authored home_lake, `MOVE 8 film 8`, and 21 of the 27 words that changed over
// 20 further ticks came back to the value they started with. A 2-cycle, at a
// shoreline, at a third of a voxel per tick, forever — CLAUDE.md rule 2 with
// nothing to show for it, and invisible in the game because there is nothing
// to SEE.
//
// So build the shape on purpose and assert it sleeps. This is a slot cut two
// cells wide into a stone plateau: the floor of the slot is one voxel below the
// plateau top, so every interior cell has a one-voxel riser behind it whichever
// way it faces, and the sky above the rim is open so the riser test's
// `!liquidWall(back + up)` clause is satisfied. One full cell of water (8
// eighths) is dropped in, which halves out to eight cells of one eighth — the
// resting state of the CA at minFilm 1, and the state from which nothing but
// the riser step can move.
//
// WHAT IT ASSERTS, and the second one is the point:
//   1. MASS is exact. A slot that "settles" by losing its water is not a pass.
//   2. The box goes IDLE. Every chunk of the structure asleep, with the tick it
//      went quiet reported, because "quiet from 40" and "quiet from -1" is the
//      whole difference between the fix and the bug.
//
// The fix is FILM_LICENCE in sim_step.wgsl: the riser step may only fire in a
// chunk where something that strictly decreases one of the CA's Lyapunov
// functions happened last tick, so it cannot be its own cause. Run this gate
// against the shader without it and it reports `2 of 4 box chunks awake ...
// (quiet from -1)`.
Status GateCaGutter(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t waterId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") { waterId = (uint32_t)i; break; }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // Dim dawn and no seam, for the reason ca-slope records: freezing and
  // evaporation are authored mass sinks and would make the audit inexact, and
  // an excited film is the MPM's problem, not the CA's.
  Tuning dawn = CurrentTuning();
  dawn.dayNight.freeze = 1;
  dawn.dayNight.freezePhase = (int)(kDaySunrise + 1024u);
  dawn.sim.fluidExciteMode = 0;
  Tuning saved = CurrentTuning();
  SetCurrentTuning(dawn);

  const int px = 96, py = 120, pz = 96;
  const int kHalf = 6;                  // chamber interior half-width
  const int kSlotZ = 3;                 // slot half-length in z (6 cells long)
  const int kTicks = 300;
  const int floorY = py;                // slot floor; water rests at floorY+1
  const int rimY = floorY + 1;          // the plateau top == the water's level
  const int roofY = py + 6;
  const int x0 = px - kHalf, x1 = px + kHalf;
  const int z0 = pz - kHalf, z1 = pz + kHalf;
  // The slot: TWO cells wide in x, which is the width the rule cannot resolve.
  const int sx0 = px, sx1 = px + 1;
  const int sz0 = pz - kSlotZ, sz1 = pz + kSlotZ - 1;

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();

  // A sealed chamber, and inside it a stone plateau at rimY with the slot cut
  // out of it. Everything at or below floorY is solid, rimY is solid EXCEPT the
  // slot, and everything above is air up to the roof.
  std::vector<CellOp> build;
  for (int z = z0 - 1; z <= z1 + 1; z++)
    for (int x = x0 - 1; x <= x1 + 1; x++)
      for (int y = floorY; y <= roofY; y++) {
        const bool wall = x < x0 || x > x1 || z < z0 || z > z1;
        const bool inSlot = x >= sx0 && x <= sx1 && z >= sz0 && z <= sz1;
        const bool solid = wall || y >= roofY || y <= floorY ||
                           (y == rimY && !inSlot);
        build.push_back({World::SlotCellIndex({x, y, z}),
                         solid ? (uint32_t)kMatStone : 0u});
      }

  // One full cell of water, standing on the slot floor in the middle of it.
  // Eight eighths, so the level answer is eight cells of one eighth and the
  // slot (12 cells) has room for it with none left over to stack.
  const std::vector<CellOp> drop = {
      {World::SlotCellIndex({sx0, rimY, pz}), (waterId & 0xFFFu) | (7u << 12)}};
  const uint64_t placed = 8;

  std::vector<uint32_t> boxChunks;
  for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
    for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
      for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++)
        boxChunks.push_back(World::SlotChunkIndex({cx, cy, cz}));

  uint32_t t = 44000;
  int quietAt = -1;
  uint32_t activeInBox = 0;
  for (int i = 0; i < kTicks; i++) {
    SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
               i == 0 ? build : i == 4 ? drop : std::vector<CellOp>{}, false,
               {6, 7, 6}, false, false);
    if (i >= 20 && i % 10 == 0) {
      ctx.WaitIdle();
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumSlots * 4, "gutterActive");
      activeInBox = 0;
      for (uint32_t ci : boxChunks)
        if (flags[ci] != 0) activeInBox++;
      if (activeInBox == 0 && quietAt < 0) quietAt = i;
      else if (activeInBox != 0) quietAt = -1;
    }
  }
  ctx.WaitIdle();

  // The resting shape. Everything is inside the chamber by construction — the
  // rim is at the water's own level and nothing in the CA lifts water — so a
  // sweep of the chamber is the whole ledger.
  uint64_t standing = 0;
  uint32_t wetted = 0, profile[9] = {}, outsideSlot = 0;
  {
    std::vector<uint32_t> cbuf((size_t)kChunkVol);
    for (int cz = (z0 - 1) >> 4; cz <= ((z1 + 1) >> 4); cz++)
      for (int cy = floorY >> 4; cy <= (roofY >> 4); cy++)
        for (int cx = (x0 - 1) >> 4; cx <= ((x1 + 1) >> 4); cx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({cx, cy, cz}), 1,
                         cbuf.data(), "gutterVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != waterId) continue;
            const int x = (int)(k % 16) + cx * 16,
                      y = (int)((k / 16) % 16) + cy * 16,
                      z = (int)(k / 256) + cz * 16;
            if (x < x0 - 1 || x > x1 + 1 || z < z0 - 1 || z > z1 + 1) continue;
            if (y < floorY || y > roofY) continue;
            standing += ((cbuf[k] >> 12) & 0xFu) + 1u;
            profile[((cbuf[k] >> 12) & 0xFu) + 1u]++;
            wetted++;
            if (x < sx0 || x > sx1 || z < sz0 || z > sz1 || y != rimY)
              outsideSlot++;
          }
        }
  }
  SetCurrentTuning(saved);

  const bool massOk = standing == placed;
  const bool idleOk = activeInBox == 0;
  const bool ok = massOk && idleOk && outsideSlot == 0;
  detail = Format(
      "one full cell dropped in a 2-wide rimmed slot: at rest over %u cells "
      "(profile 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u), %u outside the slot, "
      "mass %s (standing %llu of %llu), %u of %zu box chunks awake at tick %d "
      "(quiet from %d)",
      wetted, profile[1], profile[2], profile[3], profile[4], profile[5],
      profile[6], profile[7], profile[8], outsideSlot,
      massOk ? "EXACT" : "LEAK", (unsigned long long)standing,
      (unsigned long long)placed, activeInBox, boxChunks.size(), kTicks,
      quietAt);
  std::printf("ca-gutter: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- pond-shore ----------------------------------------------------------
//
// THE SHAPE `ca-gutter` PROVED SAFE, AT A PLACE WHERE IT IS NOT.
//
// The owner's report, 2026-09-16, from the live game at world (-2317,112,1674):
// "in a pond in the desert ... water voxels just swap back and forth really
// really fast endlessly, keeping the simulation alive in all of those chunks."
// A whole-cell swap of water and air is the CA's FILM STEP and nothing else —
// it is the only liquid move in sim_step.wgsl that is neutral in BOTH Lyapunov
// functions (same SUM(f*y), same SUM(f*f)) and can therefore 2-cycle.
//
// THE GUARD ALREADY EXISTS AND `ca-gutter` ALREADY TESTS IT. FILM_LICENCE says
// a film may only step in a chunk where something that strictly decreased a
// Lyapunov function happened last tick, so a neutral move cannot be its own
// cause. So why is there still a report?
//
// BECAUSE ca-gutter DISABLES EVERY LICENCE SOURCE IN ORDER TO ISOLATE THE RULE.
// Read its fixture: `fluidExciteMode = 0` (no seam), a chamber cut from STONE
// (no absorb capacity, so no staining that spends mass), no water body, no
// particles. Nothing in that box can hand the film a licence, so the licence is
// never granted and the 2-cycle cannot run. It is a correct test of the rule
// and a test of NOTHING ELSE — and the licence's soundness argument is entirely
// about what else is in the chunk:
//
//   "Every bit below is either a move that strictly decreases a bounded integer
//    ... or an EXTERNAL input (a mutation, the seam, a particle landing, a
//    reaction firing). NONE OF THEM CAN BE CAUSED BY A FILM STEP."
//
// That last sentence is the whole fix, it is quantified over the licence set,
// and `ca-gutter` cannot see it because its fixture empties the set. So this
// gate is the other half: the SAME rule, at a real place, with the shipped
// tuning and nothing switched off.
//
// WHY THIS SITE AND NOT A BUILT FIXTURE. A desert tarn's bank is a thin SAND
// cap over sandstone at a shallow angle, with the waterline cutting across the
// loose grains (`--voxdump -2352,96,1648,64,64,64,1` shows `s~sssss~~~` at the
// water's own level). Two things live there that a stone chamber cannot have:
//   * sand authors `absorb: {capacity: 6}`, so every film touching a dry grain
//     writes a stain (DIRTY_R_STAINW, in the licence set) and spends an eighth
//     of itself doing it;
//   * the seam ships ON (`sim.fluidExciteMode` 1), and exciteDetect runs POST-CA
//     over the same dirty chunks, marking DIRTY_R_SEAM on a chunk and its eight
//     neighbours whenever it converts a cell.
// Both are in FILM_LICENCE. Whether either can be CAUSED by a film step is
// exactly the claim above, and this gate is where it gets measured.
//
// No gate in the engine ticked a procedural pond before this one. `terrain`
// pass D reports `0 chunks still awake after 120 ticks` — and its window is the
// harness pad, which the map's own site table declares as "no tarns". That is
// why a never-sleeping shoreline survived a fix aimed at it.
//
// WHAT IT REPORTS, in the order a diagnosis needs it (CLAUDE.md rule 6 — a bare
// count of awake chunks is the non-measurement that cost 14 elimination runs
// once already):
//   1. HOW MANY chunks are awake, and the DIRTY_R_*/DIRTY_M_* reason histogram,
//      which names the stage: `film` is the riser branch, `film-press` the
//      pressed one, and whatever sits beside them is what granted the licence.
//   2. THE 2-CYCLE COUNT. Three voxel samples one tick apart: a cell with
//      S0 != S1 and S2 == S0 has come back to where it started, which is a
//      shuffle and not work. "N words changed" and "N words changed and N-2 of
//      them are back" are different findings and only the second names a cycle.
//   3. THE WATER LEDGER, start against end. Absorption into a sand bank is a
//      real mass sink and a legitimate reason for a shore to stay busy for a
//      while; a pond that is still losing eighths at tick 300 is a different
//      bug from one that is merely shuffling them.
//
// The window is moved and RESTORED (with a regeneration) on the way out, for
// the reason selftest.h states in as many words: a later gate that hardcodes a
// world position instead of anchoring to WindowOrigin() would fire into solid
// space.
Status GatePondShore(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t seed = kDefaultSeed;

  uint32_t waterId = 0, sandId = 0;
  for (size_t i = 0; i < c.mats.size(); i++) {
    if (c.mats[i].name == "water") waterId = (uint32_t)i;
    if (c.mats[i].name == "sand") sandId = (uint32_t)i;
  }
  if (waterId == 0) { detail = "no 'water' material"; return Status::Fail; }

  // The owner's shore, as a CHUNK origin that puts it in the middle of the
  // residency window. Voxel (-2317,112,1674) is chunk (-145,7,104); the window
  // is kWorldN/kChunk chunks on a side, so back off half of that on each axis.
  // Written from the voxel coordinate rather than as three magic numbers so
  // that a change to kWorldN moves the window instead of decentring the pond.
  const int half = (int)(kWorldN / kChunk) / 2;
  const IVec3 site{-2317, 112, 1674};
  const IVec3 siteChunk{site.x >> 4, site.y >> 4, site.z >> 4};
  const IVec3 shoreOrigin{siteChunk.x - half, siteChunk.y - half,
                          siteChunk.z - half};
  const IVec3 savedOrigin = world.WindowOrigin();

  // NOTHING IS SWITCHED OFF. That is the point of the gate: `ca-gutter` already
  // covers the rule in isolation, and every knob this one might pin (the seam,
  // the day phase, the water bodies) is a licence source under test.
  world.SetWindowOrigin(shoreOrigin);
  SubmitWorldgen(ctx, world, sim, seed);
  ctx.WaitIdle();

  // WORLD chunk coords, not window-local, and this is the one thing about the
  // gate that is easy to get wrong: SubmitTick's `playerChunk` reaches
  // World::MirrorBaseFor, which clamps against origin_, and SlotChunkIndex
  // masks a WORLD chunk into the torus. Handing either a local index reads a
  // different place in the world and measures nothing -- the first cut of this
  // gate did exactly that and reported `water 0 -> 0 eighths`, a fixture that
  // cannot fail. Every other gate here sits at origin (0,0,0) where the two
  // spaces coincide, which is why the mistake is available at all.
  const IVec3 playerChunk = siteChunk;

  // The chunk slots a 3-chunk-tall band around the waterline occupies, over a
  // 12-chunk square centred on the site: the shore, not the whole pond floor.
  // These are what the ledger and the diff are read over, so the numbers are
  // about the shoreline rather than about the window.
  std::vector<uint32_t> shoreChunks;
  for (int dz = -6; dz <= 6; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -6; dx <= 6; dx++)
        shoreChunks.push_back(World::SlotChunkIndex(
            {siteChunk.x + dx, siteChunk.y + dy, siteChunk.z + dz}));

  // ---- the water ledger before ------------------------------------------
  auto waterEighths = [&](const char* tag) -> uint64_t {
    uint64_t sum = 0;
    std::vector<uint32_t> buf((size_t)kChunkVol);
    for (uint32_t ci : shoreChunks) {
      ReadVoxelsSync(ctx, world, ci, 1, buf.data(), tag);
      for (uint32_t k = 0; k < kChunkVol; k++)
        if ((buf[k] & 0xFFFu) == waterId) sum += ((buf[k] >> 12) & 0xFu) + 1u;
    }
    return sum;
  };
  const uint64_t water0 = waterEighths("shoreLedger0");

  // ---- one measurement, used by both passes -------------------------------
  // A pass is: tick to quiet (or give up), then say HOW MANY chunks are awake,
  // WHY, and whether what they are doing is work or a shuffle. Written once
  // because the two passes must agree about all three or the comparison between
  // them means nothing.
  uint32_t t = 52000;
  struct PassOut {
    uint32_t awake = 0;
    int quietAt = -1;
    std::string why;
    uint32_t changed = 0, cycled = 0;
    uint32_t cycMat = 0, cycState = 0, cycStain = 0;
    uint32_t cycWater = 0, cycAir = 0, cycSand = 0, cycOther = 0;
    std::string worst;
  };
  auto runPass = [&](int ticks, const std::vector<CellOp>& ops) -> PassOut {
    PassOut o;
    for (int i = 0; i < ticks; i++) {
      SubmitTick(ctx, world, sim, ++t, seed, {}, {},
                 i == 0 ? ops : std::vector<CellOp>{}, false, playerChunk, false,
                 true);
      if (i >= 20 && i % 10 == 0) {
        ctx.WaitIdle();
        std::vector<uint32_t> flags(kNumSlots, 0);
        rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                              flags.data(), kNumSlots * 4, "shoreActive");
        o.awake = 0;
        for (uint32_t ci : shoreChunks)
          if (flags[ci] != 0) o.awake++;
        if (o.awake == 0 && o.quietAt < 0) o.quietAt = i;
        else if (o.awake != 0) o.quietAt = -1;
      }
    }
    ctx.WaitIdle();

    // ---- HOW MANY, AND WHY, FROM ONE READ ---------------------------------
    // The reason bitmask, through kDirtyReasonName (world.h) -- the one table,
    // for the reason the `sleep` gate records: a private copy of the names
    // silently drops whatever bit was added last.
    //
    // `o.awake` IS OVERWRITTEN HERE ON PURPOSE. The loop above samples every
    // tenth tick, so the count it leaves behind is tick 290's and the histogram
    // below is tick 300's -- and this gate printed "3 of 507 shore chunks awake
    // ... by reason: none - fully quiet" on its first run after the fix landed,
    // which is the `sleep` gate's own recorded mistake ("two readings of one
    // buffer that cannot both be true") reproduced exactly. The periodic sample
    // keeps its job, which is quietAt; the VERDICT number comes from the same
    // read as the reason for it.
    std::vector<uint32_t> awakeSlots;
    {
      std::vector<uint32_t> flags(kNumSlots, 0);
      rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                            flags.data(), kNumSlots * 4, "shoreWhy");
      uint32_t hist[kDirtyReasonBits] = {0};
      for (uint32_t ci : shoreChunks) {
        if (flags[ci] == 0) continue;
        awakeSlots.push_back(ci);
        for (int b = 0; b < kDirtyReasonBits; b++)
          if (flags[ci] & (1u << b)) hist[b]++;
      }
      o.awake = (uint32_t)awakeSlots.size();
      for (int b = 0; b < kDirtyReasonBits; b++)
        if (hist[b]) o.why += Format(" %s %u", kDirtyReasonName[b], hist[b]);
      if (o.why.empty()) o.why = " none - fully quiet";
    }

    // ---- IS IT WORK OR IS IT A SHUFFLE? ----------------------------------
    // Three samples one tick apart over the awake chunks. A word that changed on
    // the first tick and is back to its starting value on the second is a
    // 2-cycle, and that is the signature the film step leaves. Split by field so
    // a pond soaking into its bed (stain) cannot be mistaken for a film creeping
    // (fullness) or a grain moving (material).
    if (awakeSlots.empty()) return o;
    const size_t n = std::min<size_t>(awakeSlots.size(), 16);
    std::vector<std::vector<uint32_t>> s0(n), s1(n);
    for (size_t i = 0; i < n; i++) {
      s0[i].assign(kChunkVol, 0);
      ReadVoxelsSync(ctx, world, awakeSlots[i], 1, s0[i].data(), "shoreS0");
    }
    SubmitTick(ctx, world, sim, ++t, seed, {}, {}, {}, false, playerChunk, false,
               true);
    ctx.WaitIdle();
    for (size_t i = 0; i < n; i++) {
      s1[i].assign(kChunkVol, 0);
      ReadVoxelsSync(ctx, world, awakeSlots[i], 1, s1[i].data(), "shoreS1");
    }
    SubmitTick(ctx, world, sim, ++t, seed, {}, {}, {}, false, playerChunk, false,
               true);
    ctx.WaitIdle();
    std::vector<uint32_t> s2(kChunkVol, 0);
    for (size_t i = 0; i < n; i++) {
      ReadVoxelsSync(ctx, world, awakeSlots[i], 1, s2.data(), "shoreS2");
      for (size_t v = 0; v < kChunkVol; v++) {
        // The STAMP (bits 16..18) and the excite scratch (19..23) are excluded:
        // both legitimately differ tick to tick on a cell that did nothing, and
        // counting them would report every visited cell as a cycle.
        const uint32_t m = 0xFF00FFFFu;
        const uint32_t a = s0[i][v] & m, b = s1[i][v] & m, d = s2[v] & m;
        if (a == b) continue;
        o.changed++;
        if (d != a) continue;
        o.cycled++;
        if ((a & 0xFFFu) != (b & 0xFFFu)) o.cycMat++;
        if (((a >> 12) & 0xFu) != ((b >> 12) & 0xFu)) o.cycState++;
        if ((a & 0x7F000000u) != (b & 0x7F000000u)) o.cycStain++;
        const uint32_t am = a & 0xFFFu;
        if (am == waterId) o.cycWater++;
        else if (am == 0) o.cycAir++;
        else if (am == sandId) o.cycSand++;
        else o.cycOther++;
        // WHERE, AND WHAT IS AROUND IT. A coordinate alone says which chunk to
        // go and look at; the two vertical slices say which RULE, and that is
        // the difference this gate exists for. `ca-gutter`'s whole finding was
        // that a film against ONE riser and a film between TWO are told apart
        // only by cells the shader may not read -- so the CPU, with no lattice
        // bound, is the one place the distinction can be drawn.
        if (o.worst.empty()) {
          const IVec3 wc = world.SlotToWorldChunk(awakeSlots[i]);
          const IVec3 at{wc.x * 16 + (int)(v % 16),
                         wc.y * 16 + (int)((v / 16) % 16),
                         wc.z * 16 + (int)(v / 256)};
          auto nm = [&](uint32_t mm) {
            return mm == 0 ? "air"
                           : mm < c.mats.size() ? c.mats[mm].name.c_str() : "?";
          };
          o.worst = Format(" first at (%d,%d,%d) %s<->%s", at.x, at.y, at.z,
                           nm(am), nm(b & 0xFFFu));
          // One character per material, over a 7-wide x 5-tall window on each
          // of the two lateral axes, read from the POST-move sample so what is
          // printed is the state the next tick acts on. Chunk-local only: the
          // cycling cell can sit on a chunk face, and stitching neighbours in
          // would need four more readbacks to draw two more columns.
          const char* key = " .#~Ss*";  // air stone water sand sandstone other
          auto glyph = [&](int lx, int ly, int lz) -> char {
            if (lx < 0 || lx > 15 || ly < 0 || ly > 15 || lz < 0 || lz > 15)
              return '?';
            const uint32_t q = s2[(size_t)((lz * 16 + ly) * 16 + lx)] & 0xFFFu;
            if (q == 0) return '.';
            if (q == waterId) return '~';
            if (q == sandId) return 's';
            const std::string& n2 = c.mats[q].name;
            if (n2 == "steam") return '^';
            if (n2 == "sandstone") return 'S';
            if (n2 == "stone") return '#';
            return '*';
          };
          (void)key;
          const int lx = (int)(v % 16), ly = (int)((v / 16) % 16),
                    lz = (int)(v / 256);
          o.worst += "  [x-z slices, '.' air '~' water '^' steam 's' sand "
                     "'S' sandstone '#' stone, cell at the centre]";
          for (int yy = 2; yy >= -2; yy--) {
            o.worst += Format("\n  y%+d  ", yy);
            for (int xx = -3; xx <= 3; xx++) o.worst += glyph(lx + xx, ly + yy, lz);
            o.worst += "   ";
            for (int zz = -3; zz <= 3; zz++) o.worst += glyph(lx, ly + yy, lz + zz);
          }
        }
      }
    }
    return o;
  };

  auto say = [&](const char* label, const PassOut& o, int ticks) {
    return Format(
        "%s: %u of %zu shore chunks awake at tick %d (quiet from %d), by"
        " reason:%s, over 2 further ticks %u words changed / %u BACK where they"
        " started (material %u, fullness %u, stain %u; water %u, air %u,"
        " sand %u, other %u)%s",
        label, o.awake, shoreChunks.size(), ticks, o.quietAt, o.why.c_str(),
        o.changed, o.cycled, o.cycMat, o.cycState, o.cycStain, o.cycWater,
        o.cycAir, o.cycSand, o.cycOther, o.worst.c_str());
  };

  // ---- PASS A: the world as generated -------------------------------------
  // 300 ticks is ten seconds of game time on a world that arrives at rest: the
  // pond generates level (every column of the voxdump tops at y112 with fullness
  // 8), so anything still moving at the end is something the CA started itself.
  //
  // This pass is EXPECTED to be busy for a while and then stop. A desert tarn's
  // bank is loose sand and sand authors `absorb: {capacity: 6}`, so the first
  // hundred-odd ticks are the bank drinking -- real work, with a bounded end
  // (the reachable surface is finite and stain only ever climbs). The ledger
  // below is what says the drinking STOPPED rather than merely slowed.
  const int kTicksA = 300;
  const PassOut a = runPass(kTicksA, {});
  const uint64_t water1 = waterEighths("shoreLedger1");

  // ---- PASS B: AND NOW DISTURB IT -----------------------------------------
  //
  // THE PASS THAT MATCHES THE REPORT, and pass A is the reason it has to exist.
  // A pristine tarn settles; the owner's pond is one that has been PLAYED IN.
  // Every licence source in FILM_LICENCE is an external input, so the question
  // the report actually asks is not "does a fresh shore sleep" but "does a
  // shore that has been disturbed GO BACK to sleep" -- and those are different
  // claims the moment a neutral rule can be licensed by the consequences of its
  // own move.
  //
  // THE DISTURBANCE IS A SPLASH ON THE WATERLINE, which is what a body entering
  // the water is from the CA's point of view: a slug of water dropped where the
  // loose sand meets the pond. It is placed at the found waterline rather than
  // at a literal coordinate, because a shoreline moves when worldgen does and a
  // fixture that hardcodes its site measures open water the first time a curve
  // is retuned.
  std::vector<CellOp> splash;
  IVec3 splashAt{0, 0, 0};
  {
    // Find the waterline: a surface water cell (air above) with SAND on a
    // lateral face. That is the shore, by definition, and it is the one place
    // the two licence sources this gate exists to test -- the sand's absorb
    // capacity and the seam's free-surface triggers -- both apply.
    std::vector<uint32_t> buf((size_t)kChunkVol);
    for (int dz = -2; dz <= 2 && splash.empty(); dz++)
      for (int dx = -2; dx <= 2 && splash.empty(); dx++) {
        const IVec3 wc{siteChunk.x + dx, siteChunk.y, siteChunk.z + dz};
        ReadVoxelsSync(ctx, world, World::SlotChunkIndex(wc), 1, buf.data(),
                       "shoreFind");
        for (uint32_t k = 0; k < kChunkVol && splash.empty(); k++) {
          if ((buf[k] & 0xFFFu) != waterId) continue;
          const int lx = (int)(k % 16), ly = (int)((k / 16) % 16),
                    lz = (int)(k / 256);
          if (ly == 15) continue;                       // no cell above to read
          if ((buf[k + 16] & 0xFFFu) != 0u) continue;   // not a free surface
          bool touchesSand = false;
          if (lx > 0 && (buf[k - 1] & 0xFFFu) == sandId) touchesSand = true;
          if (lx < 15 && (buf[k + 1] & 0xFFFu) == sandId) touchesSand = true;
          if (lz > 0 && (buf[k - 256] & 0xFFFu) == sandId) touchesSand = true;
          if (lz < 15 && (buf[k + 256] & 0xFFFu) == sandId) touchesSand = true;
          if (!touchesSand) continue;
          splashAt = {wc.x * 16 + lx, wc.y * 16 + ly, wc.z * 16 + lz};
          // 4x2x4 full cells, three above the surface: 32 cells of water, the
          // same order as ca-level-pond's blob, dropped where it has to run
          // down a loose sand bank to find the level again.
          for (int sz = 0; sz < 4; sz++)
            for (int sy = 0; sy < 2; sy++)
              for (int sx = 0; sx < 4; sx++)
                splash.push_back(
                    {World::SlotCellIndex({splashAt.x + sx - 2, splashAt.y + sy + 3,
                                           splashAt.z + sz - 2}),
                     (waterId & 0xFFFu) | (7u << 12)});
        }
      }
  }
  const PassOut b = runPass(kTicksA, splash);
  const uint64_t water2 = waterEighths("shoreLedger2");

  // ---- PASS C: THE SUN IS UP ----------------------------------------------
  //
  // Passes A and B run at whatever day phase the tick counter lands on, which
  // is what every other gate here does and is exactly the wrong thing for this
  // question. reactions.json authors evaporation as
  //
  //   water --(when:"day", needsSky, minLight 120, >=4 NON-water faces)--> steam
  //
  // and the `minCount: 4` is the whole point of the rule: deep water and a flat
  // pond surface (1 non-water face, the air above) are IMMUNE, and a thin film
  // on an exposed bank -- air above, sand below, sand to one side -- is not.
  // That predicate is a description of a shoreline.
  //
  // WHY THAT IS A LICENCE AND NOT JUST A LEAK. Every firing writes a voxel and
  // marks DIRTY_R_REACTW, which is IN FILM_LICENCE. And it is self-renewing in
  // the one way the licence's soundness argument does not cover: evaporating a
  // rim cell leaves a gap, the pond levels into the gap, and the cell that
  // arrives is a new rim cell with the same 4 non-water faces. So for as long
  // as the sun is up, a sand shore hands the film rules a licence every tick,
  // and the 2-cycle `ca-gutter` proves cannot terminate on its own runs with
  // nothing to stop it.
  //
  // `ca-gutter` and `ca-slope` both pin the phase to a dim dawn and say why in
  // as many words -- "freezing and evaporation are authored mass sinks and
  // would make the audit inexact". Correct for an audit, and it is also the
  // reason neither of them could ever see this. THIS pass pins the opposite.
  //
  // Not a fixture knob: `dayNight.freeze` is a shipped tuning field and noon is
  // a state the world is in for half of every in-game day.
  Tuning noon = CurrentTuning();
  noon.dayNight.freeze = 1;
  noon.dayNight.freezePhase = (int)kDayNoon;
  const Tuning savedTuning = CurrentTuning();
  SetCurrentTuning(noon);
  // The world has to be TOLD the sun moved: a light-gated rule deliberately
  // does not hold its chunk awake (see doReactions), so the day phase itself is
  // the wake signal and a pinned phase change with nothing dirty would simply
  // never be noticed. One splash does it, and it is the same disturbance pass B
  // used so the two are comparable.
  const PassOut d = runPass(kTicksA, splash);
  const uint64_t water3 = waterEighths("shoreLedger3");
  SetCurrentTuning(savedTuning);

  // ---- restore, so the window is where the next gate expects it -----------
  world.SetWindowOrigin(savedOrigin);
  SubmitWorldgen(ctx, world, sim, seed);
  ctx.WaitIdle();

  const int cap = (int)BaselineNumber("pondShore.awakeMax", 32);
  // A BAND WITH NO WATER IN IT IS NOT A QUIET SHORE, it is a gate pointed at
  // the wrong place, and "0 awake" is what both look like. The first cut of
  // this gate handed SlotChunkIndex a window-local index and passed while
  // reading empty sky; the ledger is the one reading that tells those apart, so
  // it is a failure condition and not just a printed number.
  const bool foundPond = water0 > 0;
  // ...and a pass B that never found a waterline to splash on is pass A run
  // twice wearing a second name. Same argument, same failure.
  const bool foundShore = !splash.empty();
  const bool ok = foundPond && foundShore && (int)a.awake <= cap &&
                  (int)b.awake <= cap && (int)d.awake <= cap;
  detail = Format(
      "the owner's desert shore at (%d,%d,%d), cap %d | %s | %s | %s | splash of"
      " 32 cells at (%d,%d,%d) | water %llu -> %llu -> %llu -> %llu eighths"
      " (bank drank %+lld, splash %+lld, noon %+lld)",
      site.x, site.y, site.z, cap, say("A pristine", a, kTicksA).c_str(),
      say("B disturbed", b, kTicksA).c_str(),
      say("C disturbed at NOON", d, kTicksA).c_str(), splashAt.x, splashAt.y,
      splashAt.z, (unsigned long long)water0, (unsigned long long)water1,
      (unsigned long long)water2, (unsigned long long)water3,
      (long long)water1 - (long long)water0,
      (long long)water2 - (long long)water1,
      (long long)water3 - (long long)water2);
  if (!foundPond)
    detail += "  <-- NO WATER IN THE BAND: the gate is pointed at the wrong place";
  if (!foundShore)
    detail += "  <-- NO WATERLINE FOUND: pass B disturbed nothing";
  RecordObserved("pondShore.awakeObserved", (double)a.awake);
  RecordObserved("pondShore.cycledObserved", (double)b.cycled);
  RecordObserved("pondShore.disturbedAwakeObserved", (double)b.awake);
  RecordObserved("pondShore.noonAwakeObserved", (double)d.awake);
  RecordObserved("pondShore.noonCycledObserved", (double)d.cycled);
  std::printf("pond-shore: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- repose ---------------------------------------------------------------
//
// PER-MATERIAL ANGLE OF REPOSE. Until the `repose` field existed, every powder
// in the engine piled at exactly 45 degrees, because the movement chain is "one
// down, else one down and one across" and nothing else. The field adds two
// FLATTER tiers (2:1 and 3:1 run:rise, ~27 and ~18 degrees) as one extra
// lateral step toward a drop, and two STEEPER ones (1:2 and 1:3, ~63 and ~72)
// as a gate on the down-diagonal. Both read the tick-start occupancy snapshot;
// see the ANGLE OF REPOSE block in sim_step.wgsl.
//
// WHAT THIS GATE ASSERTS, and why each part is here rather than assumed:
//
//   1. THE SLOPES ORDER. Four piles of the SAME material, from the SAME column,
//      on the SAME floor, differing only in the packed `repose` word: 3:1, 2:1,
//      the 1:1 default, and 1:2. run:rise must come out strictly decreasing.
//      Bands per arm live in tests/baseline.json so retuning what counts as "a
//      2:1 pile" costs a JSON edit, not a rebuild.
//   2. THE BLEND IS NOT INERT. Two more arms use REAL LOADER OUTPUT --
//      PackRepose(30) and PackRepose(40), the words `dust` and `ash` actually
//      ship with -- and must produce DIFFERENT piles from each other. Those are
//      the only arms that execute the per-grain blend hash at all: the four pure
//      tiers have blend 0 and return before it. That matters because the first
//      cut of this feature gated the slide on the MATERIAL rather than on the
//      GRAIN'S OWN CODE, which made every angle between 27 and 44 behave as pure
//      2:1 -- a bug no pure-tier arm can see.
//   3. THE ROOM IS ASLEEP AT THE END of every arm. A lateral slide is the only
//      move in the powder chain that does not lower a grain, so an unbounded run
//      of them would keep chunks awake forever (CLAUDE.md rule 2). The
//      termination argument is written down in the shader; this is its test.
//   4. MASS IS EXACT. 400 grains poured, 400 grains standing, on every arm.
//
// IT USES `dust`, and the choice is load-bearing rather than arbitrary:
// worldgen never places dust (it is a rubble/decay product), and its only
// authored reactions need a `tag:hot` neighbour or a mite. So patching dust's
// repose word disturbs nothing outside this room, which is what makes the
// arm-to-arm differences attributable to the pile.
//
// ONE WORLDGEN FOR ALL SIX ARMS, and the room rebuilt between them. The arms are
// comparable because each replays the SAME tick window over a room reset to a
// clean sealed box; paying six fresh worldgens (1,746 woken chunks each) to
// establish the same thing would roughly quadruple the gate's runtime.
struct ReposeArm {
  const char* name;
  uint32_t word;      // the packed MaterialGpu.repose to patch in
  const char* minKey;
  const char* maxKey;
  double minFallback;
  double maxFallback;
};

Status GateRepose(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;

  uint32_t dustId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "dust") { dustId = (uint32_t)i; break; }
  if (dustId == 0) { detail = "no 'dust' material"; return Status::Fail; }

  constexpr int kR = 15;           // interior half-width; the walls are at kR+1
  constexpr int kFloorTop = 120;   // topmost solid floor cell (floor is 3 deep)
  constexpr int kCeil = 143;       // the lid
  constexpr int kColHalf = 2;      // 5x5 column cross-section
  constexpr int kColTop = 136;     // column fills kFloorTop+1 .. kColTop
  constexpr int kSettleTicks = 120;  // let the fresh world quieten, once
  constexpr int kMaxTicks = 300;     // per-arm cap
  constexpr int kMinTicks = 80;      // before the quiet poll starts
  constexpr int kPoll = 20;
  constexpr int kBuildAt = 3, kPourAt = 8;
  const int cx = 160, cz = 160;    // SLOT coords, like the other CA gates
  const int x0 = cx - kR - 1, x1 = cx + kR + 1;
  const int z0 = cz - kR - 1, z1 = cz + kR + 1;
  const int y0 = kFloorTop - 2, y1 = kCeil;
  const int span = 2 * kR + 1;

  // Stone where the shell is, AIR everywhere else in the box, so the chamber is
  // clean whatever worldgen (or the previous arm) left in it.
  std::vector<CellOp> build;
  for (int y = y0; y <= y1; y++)
    for (int z = z0; z <= z1; z++)
      for (int x = x0; x <= x1; x++) {
        const bool solid = y <= kFloorTop || y >= kCeil || x == x0 ||
                           x == x1 || z == z0 || z == z1;
        build.push_back({World::SlotCellIndex({x, y, z}),
                         solid ? (uint32_t)kMatStone : 0u});
      }
  std::vector<CellOp> pour;
  for (int y = kFloorTop + 1; y <= kColTop; y++)
    for (int z = cz - kColHalf; z <= cz + kColHalf; z++)
      for (int x = cx - kColHalf; x <= cx + kColHalf; x++)
        pour.push_back({World::SlotCellIndex({x, y, z}), dustId & 0xFFFu});
  const uint32_t poured = (uint32_t)pour.size();

  // The idle check is LOCAL: the rest of the generated world may still be
  // settling at these tick counts and would swamp a global count.
  std::vector<uint32_t> boxChunks;
  for (int qz = z0 >> 4; qz <= (z1 >> 4); qz++)
    for (int qy = y0 >> 4; qy <= (y1 >> 4); qy++)
      for (int qx = x0 >> 4; qx <= (x1 >> 4); qx++)
        boxChunks.push_back(World::SlotChunkIndex({qx, qy, qz}));

  auto pure = [](uint32_t code) {
    return (code & kMatReposeCodeAMask) << kMatReposeCodeAShift;
  };
  // The four PURE tiers first, in order of decreasing run:rise, then the two
  // BLENDED arms straight out of the loader. Order matters only for the report.
  const ReposeArm arms[6] = {
      {"3:1 pure  (18deg)", pure(kRepose3To1), "repose.runRiseMin_flowy3",
       "repose.runRiseMax_flowy3", 1.9, 5.0},
      {"2:1 pure  (27deg)", pure(kRepose2To1), "repose.runRiseMin_flowy2",
       "repose.runRiseMax_flowy2", 1.3, 3.2},
      {"1:1 default (45deg)", 0u, "repose.runRiseMin_default",
       "repose.runRiseMax_default", 0.55, 1.6},
      {"1:2 pure  (63deg)", pure(kRepose1To2), "repose.runRiseMin_steep",
       "repose.runRiseMax_steep", 0.05, 0.95},
      {"blend 30deg (loader)", PackRepose(30), "repose.runRiseMin_blend30",
       "repose.runRiseMax_blend30", 0.9, 3.4},
      {"blend 40deg (loader)", PackRepose(40), "repose.runRiseMin_blend40",
       "repose.runRiseMax_blend40", 0.5, 2.2},
  };
  constexpr int kArms = 6;
  double runRise[kArms] = {};
  int hPeak[kArms] = {}, rEdge[kArms] = {}, quietAt[kArms] = {};
  uint32_t mass[kArms] = {}, roomHash[kArms] = {}, awake[kArms] = {};

  // ONE worldgen, then let it quieten before the first arm so the awake-chunk
  // check below is measuring the PILE and not the terrain.
  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  ctx.WaitIdle();
  {
    uint32_t t = 39000;
    for (int i = 0; i < kSettleTicks; i++)
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {}, {}, false,
                 {10, 7, 10}, false, false);
    ctx.WaitIdle();
  }

  std::vector<MaterialDef> patched = c.mats;
  std::vector<uint32_t> flags(kNumChunks, 0);
  std::vector<uint32_t> cbuf((size_t)kChunkVol);
  for (int a = 0; a < kArms; a++) {
    patched[dustId].gpu.repose = arms[a].word;
    sim.UploadTables(ctx.queue, patched, c.reactions);
    ctx.WaitIdle();

    uint32_t t = 40000;   // the SAME tick window for every arm
    quietAt[a] = -1;
    for (int i = 0; i < kMaxTicks; i++) {
      SubmitTick(ctx, world, sim, ++t, kDefaultSeed, {}, {},
                 i == kBuildAt ? build
                 : i == kPourAt ? pour
                                : std::vector<CellOp>{},
                 false, {10, 7, 10}, false, false);
      // Stop as soon as the room is quiet -- the arms settle at very different
      // rates and a fixed tick count would be sized for the slowest of six.
      if (i >= kMinTicks && (i % kPoll) == 0) {
        ctx.WaitIdle();
        rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                              flags.data(), kNumChunks * 4, "reposeActive");
        uint32_t live = 0;
        for (uint32_t ci : boxChunks)
          if (flags[ci] != 0) live++;
        if (live == 0) { quietAt[a] = i; break; }
      }
    }
    ctx.WaitIdle();
    rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0,
                          flags.data(), kNumChunks * 4, "reposeActive");
    for (uint32_t ci : boxChunks)
      if (flags[ci] != 0) awake[a]++;

    // ---- the pile's shape, as a height map over the interior floor --------
    std::vector<int> hmap((size_t)span * span, 0);
    uint32_t fnv = 2166136261u;
    for (int qz = z0 >> 4; qz <= (z1 >> 4); qz++)
      for (int qy = y0 >> 4; qy <= (y1 >> 4); qy++)
        for (int qx = x0 >> 4; qx <= (x1 >> 4); qx++) {
          ReadVoxelsSync(ctx, world, World::SlotChunkIndex({qx, qy, qz}), 1,
                         cbuf.data(), "reposeVox");
          for (uint32_t k = 0; k < kChunkVol; k++) {
            if ((cbuf[k] & 0xFFFu) != dustId) continue;
            const int x = (int)(k % 16) + qx * 16;
            const int y = (int)((k / 16) % 16) + qy * 16;
            const int z = (int)(k / 256) + qz * 16;
            const int dx = x - cx, dz = z - cz;
            if (dx < -kR || dx > kR || dz < -kR || dz > kR) continue;
            if (y <= kFloorTop || y >= kCeil) continue;
            mass[a]++;
            // A hash of WHERE the grains ended up, not of the whole world: it
            // isolates the pile, so "this arm built a different pile" is a
            // claim about repose and not about anything else the tick did.
            const uint32_t key = World::SlotCellIndex({x, y, z});
            fnv = (fnv ^ (key & 0xFFu)) * 16777619u;
            fnv = (fnv ^ ((key >> 8) & 0xFFu)) * 16777619u;
            fnv = (fnv ^ ((key >> 16) & 0xFFu)) * 16777619u;
            fnv = (fnv ^ ((key >> 24) & 0xFFu)) * 16777619u;
            const int h = y - kFloorTop;   // 1 = one layer on the floor
            int& cell = hmap[(size_t)(dz + kR) * span + (dx + kR)];
            if (h > cell) cell = h;
          }
        }
    roomHash[a] = fnv;
    // The footprint is a DIAMOND, not a square: the spreading moves are the
    // four axis down-diagonals and the four axis slides, so the natural radius
    // is Manhattan. maxH at each radius is the pile's profile, and the slope is
    // the radius the pile reaches over the height it stands at.
    std::vector<int> profile((size_t)(2 * kR + 1), 0);
    for (int dz = -kR; dz <= kR; dz++)
      for (int dx = -kR; dx <= kR; dx++) {
        const int h = hmap[(size_t)(dz + kR) * span + (dx + kR)];
        if (h <= 0) continue;
        const int r = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
        if (h > hPeak[a]) hPeak[a] = h;
        if (r > rEdge[a]) rEdge[a] = r;
        if (h > profile[(size_t)r]) profile[(size_t)r] = h;
      }
    runRise[a] = hPeak[a] > 0 ? (double)rEdge[a] / (double)hPeak[a] : 0.0;

    // ONE LINE PER ARM, with the whole profile, so a failure names itself
    // instead of leaving a bare ratio with no cause attached.
    std::string prof;
    for (int r = 0; r <= rEdge[a] && r < 2 * kR + 1; r++)
      prof += (r ? "," : "") + std::to_string(profile[(size_t)r]);
    std::printf(
        "repose: %-21s word %08x  run:rise %.2f (reach %d over peak %d), "
        "%u/%u grains, %u/%zu room chunks awake (quiet at tick %d), pile "
        "%08x, profile h@r=0.. [%s]\n",
        arms[a].name, arms[a].word, runRise[a], rEdge[a], hPeak[a], mass[a],
        poured, awake[a], boxChunks.size(), quietAt[a], roomHash[a],
        prof.c_str());
  }
  // Put the authored table back before anything else runs on it (rule 7: gates
  // share one World and the next one must not inherit a patched material).
  sim.UploadTables(ctx.queue, c.mats, c.reactions);
  ctx.WaitIdle();

  static const char* kObs[kArms] = {"flowy3", "flowy2", "default", "steep",
                                    "blend30", "blend40"};
  const uint32_t awakeMax = (uint32_t)BaselineNumber("repose.awakeChunksMax", 0);
  bool bandsOk = true, massOk = true, sleepOk = true;
  for (int a = 0; a < kArms; a++) {
    const double lo = BaselineNumber(arms[a].minKey, arms[a].minFallback);
    const double hi = BaselineNumber(arms[a].maxKey, arms[a].maxFallback);
    if (runRise[a] < lo || runRise[a] > hi) bandsOk = false;
    if (mass[a] != poured) massOk = false;
    if (awake[a] > awakeMax) sleepOk = false;
    RecordObserved((std::string("repose.runRiseObserved_") + kObs[a]).c_str(),
                   runRise[a]);
  }
  // The four PURE tiers, strictly decreasing: flatter tier => wider, lower pile.
  const bool orderOk = runRise[0] > runRise[1] && runRise[1] > runRise[2] &&
                       runRise[2] > runRise[3];
  // THE BLEND ARMS. 30 degrees mixes more 2:1 grains than 40 does, so it must
  // build the flatter pile -- and the two must not be the same pile, which is
  // what fails if the slide is gated on the material instead of the grain.
  const bool blendOk = runRise[4] > runRise[5] && roomHash[4] != roomHash[5];
  // ... and each must land strictly INSIDE the pure tiers it mixes, or the
  // "blend" is really just one of the two tiers wearing a mixed word.
  const bool blendBracketOk = runRise[4] <= runRise[1] && runRise[4] >= runRise[2] &&
                              runRise[5] <= runRise[1] && runRise[5] >= runRise[2];
  const bool ok =
      bandsOk && orderOk && massOk && sleepOk && blendOk && blendBracketOk;

  detail = Format(
      "PURE run:rise %.2f (3:1) > %.2f (2:1) > %.2f (1:1) > %.2f (1:2) %s; "
      "BLEND %.2f (30deg) > %.2f (40deg) piles %08x/%08x %s, bracketed by the "
      "pure tiers %s; bands %s; peaks %d/%d/%d/%d/%d/%d over reaches "
      "%d/%d/%d/%d/%d/%d; mass %s (%u/%u/%u/%u/%u/%u of %u); room chunks awake "
      "%u/%u/%u/%u/%u/%u (max %u) %s",
      runRise[0], runRise[1], runRise[2], runRise[3],
      orderOk ? "ORDERED" : "OUT OF ORDER", runRise[4], runRise[5], roomHash[4],
      roomHash[5],
      blendOk ? "differ" : "IDENTICAL (the blend does not reach the kernel)",
      blendBracketOk ? "ok" : "OUTSIDE", bandsOk ? "ok" : "OUT OF BAND",
      hPeak[0], hPeak[1], hPeak[2], hPeak[3], hPeak[4], hPeak[5], rEdge[0],
      rEdge[1], rEdge[2], rEdge[3], rEdge[4], rEdge[5],
      massOk ? "EXACT" : "LOST GRAINS", mass[0], mass[1], mass[2], mass[3],
      mass[4], mass[5], poured, awake[0], awake[1], awake[2], awake[3],
      awake[4], awake[5], awakeMax, sleepOk ? "asleep" : "STILL AWAKE");
  std::printf("repose: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& CaGates() {
  static const std::vector<Gate> g = {
      {"ca-skip", "sim", {}, false, GateCaSkip},
      {"repose", "sim", {}, false, GateRepose},
      {"ca-slope", "sim", {}, false, GateCaSlope},
      {"ca-slope-hybrid", "sim", {}, false, GateCaSlopeHybrid},
      {"ca-level-one", "sim", {}, false, GateCaLevelOne},
      {"ca-level", "sim", {}, false, GateCaLevel},
      {"ca-level-pond", "sim", {}, false, GateCaLevelPond},
      {"ca-gutter", "sim", {}, false, GateCaGutter},
      {"oil-slick", "sim", {}, false, GateOilSlick},
      {"plant-crush", "sim", {}, false, GatePlantCrush},
      // The other half of ca-gutter: the same rule at a real shoreline, with
      // nothing switched off. It moves the residency window and regenerates on
      // the way out, so it owes nothing to what ran before it.
      {"pond-shore", "sim", {}, false, GatePondShore},
  };
  return g;
}

}  // namespace selftest
