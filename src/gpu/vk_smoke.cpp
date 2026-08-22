// vk_smoke.cpp — the cross-backend hash comparison. See vk_smoke.h.

#include "gpu/vk_smoke.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/context.h"
#include "gpu/vk_record.h"
#include "gpu/vk_sim.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "test/support.h"  // AssetDir, kDefaultSeed, SubmitWorldgen, SubmitTick, ReadHashSync

namespace sandvox {
namespace {

// The ticks whose hashes are compared. 15 and 30 are hash ticks
// (tick % 15 == 0), so both phase-5 branches are covered and the comparison
// straddles the branch rather than sampling one side of it.
constexpr uint32_t kTicks = 50;
const uint32_t kProbes[] = {1, 15, 30, 50};

bool IsProbe(uint32_t t) {
  for (uint32_t p : kProbes)
    if (p == t) return true;
  return false;
}

// The tick's hashEnable, derived exactly as the game derives it. This is what
// makes a hash READABLE at a probe tick: the hash buffer is only refreshed on a
// hash tick, so a probe that is not one would read a stale value on BOTH
// backends — equal, and meaningless. Every probe above is either 1 (where the
// hash-only pass supplies the value) or a multiple of 15.
bool HashTick(uint32_t tick) { return tick % 15 == 0; }

}  // namespace

int RunVkSmoke(bool lowPower, bool sledgehammer, bool validation) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== sandvox --vk-smoke (Vulkan port phase 3b) ===\n");
  std::printf("mode: barriers=%s validation=%s adapter=%s seed=%u ticks=%u\n",
              sledgehammer ? "sledgehammer" : "precise", validation ? "ON" : "off",
              lowPower ? "low" : "default", kDefaultSeed, kTicks);

  const std::string assetDir = AssetDir();

  // Tuning FIRST: LoadShader bakes the tuning constants into every shader's
  // prelude, so both backends must compile against the same live values. This
  // is the same ordering main.cpp uses.
  Tuning tuning;
  if (LoadTuning(assetDir + "/materials/tuning.json", tuning)) SetCurrentTuning(tuning);

  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string errors;
  if (!LoadAssets(assetDir + "/materials/materials.json",
                  assetDir + "/materials/reactions.json", mats, reactions, errors)) {
    std::printf("asset load failed:\n%s\n", errors.c_str());
    std::printf("\n=== --vk-smoke FAIL ===\n");
    return 1;
  }
  std::printf("loaded %zu materials, %zu reactions\n", mats.size(), reactions.size());

  // ---------------------------------------------------------------- Dawn --
  //
  // The reference. Its auto-generated barriers are what the Vulkan graph is
  // being compared against, so it runs first and its numbers are the ones the
  // Vulkan side must reproduce.
  std::vector<uint32_t> dawnHashes;
  uint32_t dawnGenHash = 0;
  {
    GpuContext ctx;
    if (!ctx.Init(nullptr, 1600, 900, lowPower, /*wantTimestamps=*/false)) {
      std::printf("Dawn device init: FAIL\n");
      std::printf("\n=== --vk-smoke FAIL ===\n");
      return 1;
    }
    World world;
    world.Init(ctx.device);
    Simulation sim;
    MicroSet micro;
    if (!sim.Init(ctx.device, world, mats, reactions, micro, assetDir + "/shaders")) {
      std::printf("Dawn sim init: FAIL\n");
      std::printf("\n=== --vk-smoke FAIL ===\n");
      return 1;
    }

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    dawnGenHash = HashWorldNow(ctx, world, sim, kDefaultSeed);

    for (uint32_t tick = 1; tick <= kTicks; tick++) {
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, {}, {}, {}, HashTick(tick),
                 {0, 0, 0}, /*wantReadback=*/false, /*particlesActive=*/false, {}, 0);
      if (!IsProbe(tick)) continue;
      // Tick 1 is not a hash tick, so the hash buffer holds whatever the last
      // hash pass wrote. Force a rehash there so the probe means something —
      // and do it identically on both backends, which is what keeps the
      // comparison honest rather than convenient.
      uint32_t h = HashTick(tick) ? ReadHashSync(ctx, world)
                                  : HashWorldNow(ctx, world, sim, kDefaultSeed);
      dawnHashes.push_back(h);
    }
    ctx.WaitIdle();
  }

  // -------------------------------------------------------------- Vulkan --
  std::vector<uint32_t> vkHashes;
  uint32_t vkGenHash = 0;
  {
    vk::SimBackend be;
    std::string err;
    if (!be.Init(assetDir, mats, reactions, lowPower, validation,
                 sledgehammer ? vk::BarrierMode::Sledgehammer : vk::BarrierMode::Precise,
                 err)) {
      std::printf("Vulkan init: FAIL (%s)\n", err.c_str());
      std::printf("\n=== --vk-smoke FAIL ===\n");
      return 1;
    }
    std::printf("Vulkan device: %s\n", be.GetCaps().deviceName.c_str());
    std::printf("  validation layer: %s   sync validation: %s\n",
                be.GetCaps().validationEnabled ? "ENABLED" : "not enabled",
                be.GetCaps().syncValidationEnabled ? "ENABLED" : "not enabled");

    if (!be.SubmitWorldgen(kDefaultSeed, err) || !be.SubmitHashOnly(kDefaultSeed, err) ||
        !be.ReadHash(vkGenHash, err)) {
      std::printf("Vulkan worldgen: FAIL (%s)\n", err.c_str());
      std::printf("\n=== --vk-smoke FAIL ===\n");
      return 1;
    }
    const vk::RecordStats gs = be.LastStats();
    (void)gs;

    for (uint32_t tick = 1; tick <= kTicks; tick++) {
      if (!be.SubmitTick(tick, kDefaultSeed, HashTick(tick), err)) {
        std::printf("Vulkan tick %u: FAIL (%s)\n", tick, err.c_str());
        std::printf("\n=== --vk-smoke FAIL ===\n");
        return 1;
      }
      if (tick == 1) {
        // Report what the tick table actually recorded, once. The CA row's 54
        // iterations must each carry their own barrier — that count is the
        // colour lattice (barrier_graph §7.1), so seeing it is worth a line.
        const vk::RecordStats& s = be.LastStats();
        std::printf(
            "  tick recording: %u rows, %u dispatches, %u copies, %u fills, "
            "%u barrier calls (%u buffer + %u global)\n",
            s.rows, s.dispatches, s.copies, s.fills, s.barrierCalls, s.bufferBarriers,
            s.globalBarriers);
      }
      if (!IsProbe(tick)) continue;
      if (!HashTick(tick)) {
        if (!be.SubmitHashOnly(kDefaultSeed, err)) {
          std::printf("Vulkan rehash at tick %u: FAIL (%s)\n", tick, err.c_str());
          std::printf("\n=== --vk-smoke FAIL ===\n");
          return 1;
        }
      }
      uint32_t h = 0;
      if (!be.ReadHash(h, err)) {
        std::printf("Vulkan hash read at tick %u: FAIL (%s)\n", tick, err.c_str());
        std::printf("\n=== --vk-smoke FAIL ===\n");
        return 1;
      }
      vkHashes.push_back(h);
    }

    // Validation findings are a RESULT, not decoration. A sync hazard reported
    // here is a real finding even when the hashes match — §6.2 is explicit that
    // matching hashes on one GPU are weak evidence, and that the layer detects
    // a hazard from the recorded commands without needing a divergence.
    const auto& msgs = be.Be().ValidationMessages();
    std::printf("\n=== validation ===\n");
    if (!be.GetCaps().validationEnabled) {
      std::printf("  layer not enabled for this run\n");
    } else if (msgs.empty()) {
      std::printf("  ZERO messages (no synchronization hazards reported)\n");
    } else {
      std::printf("  %zu message(s):\n", msgs.size());
      for (const auto& m : msgs) std::printf("    %s\n", m.c_str());
    }
    be.Shutdown();

    if (be.GetCaps().validationEnabled && !msgs.empty()) {
      std::printf("\n=== --vk-smoke FAIL (validation reported %zu message(s)) ===\n",
                  msgs.size());
      return 1;
    }
  }

  // ------------------------------------------------------------ compare --
  bool ok = true;
  std::printf("\n=== hashes ===\n");
  std::printf("  %-18s %-12s %-12s %s\n", "stage", "Dawn", "Vulkan", "");
  std::printf("  %-18s %08x     %08x     %s\n", "worldgen", dawnGenHash, vkGenHash,
              dawnGenHash == vkGenHash ? "MATCH" : "*** MISMATCH ***");
  if (dawnGenHash != vkGenHash) ok = false;

  for (size_t i = 0; i < std::size(kProbes); i++) {
    uint32_t d = i < dawnHashes.size() ? dawnHashes[i] : 0;
    uint32_t v = i < vkHashes.size() ? vkHashes[i] : 0;
    char label[32];
    std::snprintf(label, sizeof(label), "tick %u", kProbes[i]);
    std::printf("  %-18s %08x     %08x     %s\n", label, d, v,
                d == v ? "MATCH" : "*** MISMATCH ***");
    if (d != v) ok = false;
  }

  if (!ok) {
    // The divergence pattern is the diagnosis (barrier_graph §6.2).
    std::printf("\ninterpretation:\n");
    if (dawnGenHash != vkGenHash) {
      std::printf("  worldgen already differs -> NOT a barrier bug. Suspect the\n"
                  "  SPIR-V (compare against the WGSL Dawn ran), zero-init, or a\n"
                  "  wrong descriptor binding.\n");
    } else if (!vkHashes.empty() && !dawnHashes.empty() && vkHashes[0] != dawnHashes[0]) {
      std::printf("  worldgen matches, tick 1 differs -> the recording or the CA\n"
                  "  loop. Check the dynamic passUBO offsets and the CA row's\n"
                  "  per-iteration barrier.\n");
    } else {
      std::printf("  early ticks match, a later one drifts -> a barrier race.\n"
                  "  Rerun with --barriers=sledgehammer to bisect; note that\n"
                  "  barrier_graph 6.2 rates a matching sledgehammer run as WEAK\n"
                  "  evidence on a single GPU, and as EXONERATION when it still\n"
                  "  diverges.\n");
    }
  }

  std::printf("\n=== --vk-smoke %s ===\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// ===========================================================================
// --vk-smoke-loud : phase 3c's determinism acceptance evidence. See vk_smoke.h.
// ===========================================================================
namespace {

constexpr uint32_t kLoudTicks = 120;

// The scripted scenario. A PURE FUNCTION OF TICK, which is what lets both
// backends drive byte-identical inputs without sharing state — the same
// discipline `SelftestOps` follows and for the same reason (CLAUDE.md rule 1:
// same seed + same tick + same inputs).
//
// The op stream is deliberately shaped to light up every condition the quiet
// smoke leaves dark, and to keep them OVERLAPPING rather than sequential: a
// tick with ops AND particles AND a hash tick is where a missing barrier
// between two conditional rows would show, and a scenario that visits them one
// at a time never records that adjacency.
std::vector<BrushOp> LoudOps(uint32_t tick) {
  std::vector<BrushOp> ops;
  // Falling sand into air, continuously: keeps the CA busy and keeps chunks
  // from sleeping, so the dirty list is never empty.
  if (tick >= 3 && tick < 100) ops.push_back({100, 170, 100, 6, kMatSand, 0, 0, 0});
  // A liquid, which takes a different render/sim path and stains its banks.
  if (tick >= 8 && tick < 90) ops.push_back({176, 150, 176, 5, kMatWater, 0, 0, 0});
  // Fire on the wood platform + lava into the pool: reaction coverage, and both
  // are emitters that keep chunks awake.
  if (tick >= 40 && tick < 100) ops.push_back({176, 120, 150, 4, kMatLava, 0, 0, 0});
  if (tick >= 60 && tick < 110) ops.push_back({110, 80, 110, 3, kMatFire, 0, 0, 0});
  // Melt mode (mode 2): the laser path, molten-glass conversion.
  if (tick >= 70 && tick < 100) ops.push_back({100, 166, 100, 3, 0, 2u, 0, 0});
  return ops;
}

// Two explosions, at ticks chosen so the second lands while the first's ejecta
// is still airborne — the particle chain running with a NON-EMPTY read page and
// a fresh spawn in the same tick, which is the case where `particleSpawn`
// (T14, appending to the read page) races `pArgs1` (T40, sizing the integrate
// dispatch from the same counts) if the barrier between them is missing.
std::vector<ExplosionOp> LoudExps(uint32_t tick, uint32_t seed) {
  std::vector<ExplosionOp> exps;
  if (tick == 45) {
    int h = World::TerrainHeight(100, 100, seed);
    exps.push_back({100, h, 100, 14, 400, 0, 0, 0});
  }
  if (tick == 52) exps.push_back({176, 50, 176, 10, 300, 0, 0, 0});
  // A third, deliberately in the same tick as a hash tick (120 % 15 == 0 band):
  // the explosion chain and the whole-world occupancy scan in one command
  // buffer.
  if (tick == 75) {
    int h = World::TerrainHeight(120, 120, seed);
    exps.push_back({120, h, 120, 12, 350, 0, 0, 0});
  }
  return exps;
}

// Exact-cell ops (the prefab/CellOp path, T11). A small block written straight
// into the grid, which is a different kernel from the brush and does NOT
// consult the material table.
std::vector<CellOp> LoudCells(uint32_t tick, IVec3 windowOrigin) {
  std::vector<CellOp> cells;
  if (tick != 20 && tick != 64) return cells;
  // Anchor to the LIVE window origin, never a fixed world position — the
  // streaming walk below moves the window, and a hardcoded coordinate would
  // land outside it where space is solid. This is the same trap CLAUDE.md
  // records for selftest gates.
  IVec3 base{windowOrigin.x * (int)kChunk + 64, windowOrigin.y * (int)kChunk + 96,
             windowOrigin.z * (int)kChunk + 64};
  for (int dz = 0; dz < 4; dz++)
    for (int dy = 0; dy < 4; dy++)
      for (int dx = 0; dx < 4; dx++) {
        IVec3 c{base.x + dx, base.y + dy, base.z + dz};
        cells.push_back({World::SlotCellIndex(c), kMatStone | (kStampNever << kStampShift)});
      }
  return cells;
}

bool LoudParticlesActive(uint32_t tick) { return tick >= 45; }

// The ticks whose hashes are compared. Dense through the active window, so a
// divergence is localised to a few ticks rather than to "somewhere in 120".
bool LoudProbe(uint32_t t) {
  return t == 15 || t == 30 || t == 45 || t == 46 || t == 47 || t == 52 || t == 53 ||
         t == 60 || t == 75 || t == 76 || t == 84 || t == 85 || t == 86 || t == 87 ||
         t == 88 || t == 90 || t == 105 || t == 120;
}

bool LoudHashTick(uint32_t tick) { return tick % 15 == 0; }

// The streaming walk, and WHY IT IS ONE-DIRECTIONAL.
//
// The window advances one chunk on +X at these ticks, which forces eviction of
// the leaving plane and a refill of the entering one. Both backends do exactly
// the same thing here, so the hash comparison across the walk is meaningful:
// the entering plane is always virgin world, so BOTH sides refill it by
// procgen, from the same seed and the same origin.
//
// A RETURN LEG WAS TRIED AND REMOVED, and the reason is worth recording because
// it looks like a gap and is not. Walking back re-enters planes the outbound leg
// evicted, which is the only way to reach the STORE-HIT refill — §4.1's hardest
// case, where four buffers are written per slot through the deferred-upload
// queue and NOTHING IS SUBMITTED AT ALL. But the store-hit path's *content*
// comes from a store, and the two backends cannot share one: Dawn drives the
// real `Stream` (a sticky `modified_` set, `dropIfAir = modified_[s] == 0`, RLE
// through the region-file `ChunkStore`, force-completion of in-flight evictions
// when the player doubles back), while the Vulkan side has no `Stream` at all —
// `World`/`Stream` own `rhi::` handles, which is the same reason `vk_sim.h`
// exists. Any store the smoke emulates makes DIFFERENT refill decisions, so the
// two worlds legitimately diverge in CONTENT and the hashes differ for reasons
// that have nothing to do with barriers.
//
// That was confirmed rather than assumed: with a return leg, both backends were
// bit-STABLE run to run (Dawn c4c5178f, Vulkan 2879f83e, reproducing exactly
// every run) and diverged only after the reversal. A barrier race varies between
// runs; a policy difference does not. Three different emulated store policies
// each matched through the whole outbound leg and each diverged on the return.
//
// So the cross-backend hash covers the procgen leg, and the store-hit path is
// proven separately by `StoreHitSelfCheck` below — which is a stronger test of
// the thing actually being ported (the deferred, submit-less upload) than a
// hash comparison against a store that is not the same store.
bool LoudShiftTick(uint32_t t) { return t >= 85 && t <= 100 && (t % 2) == 0; }

}  // namespace

int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("=== sandvox --vk-smoke-loud (Vulkan port phase 3c) ===\n");
  std::printf("mode: barriers=%s validation=%s adapter=%s seed=%u ticks=%u\n",
              sledgehammer ? "sledgehammer" : "precise", validation ? "ON" : "off",
              lowPower ? "low" : "default", kDefaultSeed, kLoudTicks);
  std::printf(
      "scenario: brush+melt ops, 3 explosions (particles: spawn/integrate/resolve),\n"
      "          exact-cell ops, readback ring active, %s\n",
      "streaming walk with eviction + store-hit refill + procgen fill");

  const std::string assetDir = AssetDir();
  Tuning tuning;
  if (LoadTuning(assetDir + "/materials/tuning.json", tuning)) SetCurrentTuning(tuning);

  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string errors;
  if (!LoadAssets(assetDir + "/materials/materials.json",
                  assetDir + "/materials/reactions.json", mats, reactions, errors)) {
    std::printf("asset load failed:\n%s\n", errors.c_str());
    std::printf("\n=== --vk-smoke-loud FAIL ===\n");
    return 1;
  }
  std::printf("loaded %zu materials, %zu reactions\n", mats.size(), reactions.size());

  struct Probe {
    uint32_t tick;
    uint32_t hash;
    uint32_t activeChunks;
    uint32_t particleCount;
    uint64_t voxelTotal;
  };

  // ---------------------------------------------------------------- Dawn --
  std::vector<Probe> dawnProbes;
  uint32_t dawnGenHash = 0;
  uint32_t dawnShifts = 0, dawnEvicted = 0, dawnStoreHits = 0, dawnGenFills = 0;
  {
    GpuContext ctx;
    if (!ctx.Init(nullptr, 1600, 900, lowPower, false)) {
      std::printf("Dawn device init: FAIL\n=== --vk-smoke-loud FAIL ===\n");
      return 1;
    }
    World world;
    world.Init(ctx.device);
    Simulation sim;
    MicroSet micro;
    if (!sim.Init(ctx.device, world, mats, reactions, micro, assetDir + "/shaders")) {
      std::printf("Dawn sim init: FAIL\n=== --vk-smoke-loud FAIL ===\n");
      return 1;
    }
    Stream stream;
    stream.Init(&ctx, &world, &sim, kDefaultSeed);
    stream.OnMaterialsReloaded(mats);

    SubmitWorldgen(ctx, world, sim, kDefaultSeed);
    dawnGenHash = HashWorldNow(ctx, world, sim, kDefaultSeed);

    for (uint32_t tick = 1; tick <= kLoudTicks; tick++) {
      IVec3 wo = world.WindowOrigin();
      auto ops = LoudOps(tick);
      auto exps = LoudExps(tick, kDefaultSeed);
      auto cells = LoudCells(tick, wo);
      // playerChunk tracks the window centre so the 3x3x3 mirror is populated
      // and the readback ring carries something real.
      IVec3 playerChunk{wo.x + (int)kNChunk / 2, wo.y + (int)kNChunk / 2,
                        wo.z + (int)kNChunk / 2};
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cells,
                 LoudHashTick(tick), playerChunk, /*wantReadback=*/true,
                 LoudParticlesActive(tick), {}, 0);
      ctx.ProcessEvents();

      if (LoudShiftTick(tick)) {
        // Drive the window one chunk along +X. Stream::Update's hysteresis is
        // what decides to shift; feeding it a player two chunks past centre is
        // how the game reaches the same code.
        IVec3 target{wo.x + (int)kNChunk / 2 + 2, wo.y + (int)kNChunk / 2,
                     wo.z + (int)kNChunk / 2};
        uint32_t before = stream.ShiftCount();
        stream.Update(target);
        dawnShifts += stream.ShiftCount() - before;
      }
      if (!LoudProbe(tick)) continue;
      uint32_t h = LoudHashTick(tick) ? ReadHashSync(ctx, world)
                                      : HashWorldNow(ctx, world, sim, kDefaultSeed);
      const WorldSnapshot& sn = world.Snap();
      dawnProbes.push_back({tick, h, sn.activeChunks, sn.particleCount, sn.voxelTotal});
    }
    // FlushResident is the public drain (it evicts everything and waits), which
    // is what a save does. Used here only to settle the store before counting.
    stream.FlushResident();
    dawnEvicted = (uint32_t)stream.Store().Count();
    (void)dawnStoreHits; (void)dawnGenFills;
    ctx.WaitIdle();
  }

  // -------------------------------------------------------------- Vulkan --
  std::vector<Probe> vkProbes;
  uint32_t vkGenHash = 0;
  uint32_t vkShifts = 0, vkStoreHits = 0, vkGenFills = 0, vkEvictedChunks = 0;
  std::vector<std::string> vkMsgs;
  bool vkValidationOn = false;
  // The store-hit refill self-check (see below): did the deferred, submit-less
  // writes actually land, and how much of the plane came back.
  bool storeHitOk = false;
  double storeHitPct = 0.0;
  std::string storeHitErr;
  {
    vk::SimBackend be;
    std::string err;
    if (!be.Init(assetDir, mats, reactions, lowPower, validation,
                 sledgehammer ? vk::BarrierMode::Sledgehammer : vk::BarrierMode::Precise,
                 err)) {
      std::printf("Vulkan init: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n", err.c_str());
      return 1;
    }
    std::printf("Vulkan device: %s\n", be.GetCaps().deviceName.c_str());
    std::printf("  validation layer: %s   sync validation: %s\n",
                be.GetCaps().validationEnabled ? "ENABLED" : "not enabled",
                be.GetCaps().syncValidationEnabled ? "ENABLED" : "not enabled");

    if (!be.SubmitWorldgen(kDefaultSeed, err) || !be.SubmitHashOnly(kDefaultSeed, err) ||
        !be.ReadHash(vkGenHash, err)) {
      std::printf("Vulkan worldgen: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n",
                  err.c_str());
      return 1;
    }

    // A CPU-side mirror of the streaming store, so the refill can take the
    // store-hit path (deferred, submit-less) as well as the procgen path. This
    // is deliberately a plain map rather than Stream's region-file ChunkStore:
    // what phase 3c is proving is the GPU-side ordering, and a second copy of
    // the region format would prove nothing about it.
    std::unordered_map<uint64_t, std::vector<uint32_t>> store;
    IVec3 origin{0, 0, 0};

    for (uint32_t tick = 1; tick <= kLoudTicks; tick++) {
      auto ops = LoudOps(tick);
      auto exps = LoudExps(tick, kDefaultSeed);
      auto cells = LoudCells(tick, origin);
      IVec3 playerChunk{origin.x + (int)kNChunk / 2, origin.y + (int)kNChunk / 2,
                        origin.z + (int)kNChunk / 2};

      vk::SimBackend::TickInputs in{};
      in.tick = tick;
      in.seed = kDefaultSeed;
      in.hashEnable = LoudHashTick(tick);
      in.particlesActive = LoudParticlesActive(tick);
      in.ops = ops.empty() ? nullptr : ops.data();
      in.opsCount = (uint32_t)ops.size();
      in.exps = exps.empty() ? nullptr : exps.data();
      in.expCount = (uint32_t)exps.size();
      in.cells = cells.empty() ? nullptr : cells.data();
      in.cellCount = (uint32_t)cells.size();
      in.wantReadback = true;
      in.playerChunkBase = {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1};
      in.windowOrigin = origin;
      if (!be.SubmitTickFull(in, err)) {
        std::printf("Vulkan tick %u: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n", tick,
                    err.c_str());
        return 1;
      }
      // ProcessEvents' replacement, at the same point in the frame.
      be.PollReadbacks();

      if (tick == 46) {
        const vk::RecordStats& s = be.LastStats();
        std::printf(
            "  active-tick recording (t46, ops+explosion+particles+readback):\n"
            "    %u rows, %u dispatches, %u copies, %u fills, %u barrier calls "
            "(%u buffer + %u global)\n",
            s.rows, s.dispatches, s.copies, s.fills, s.barrierCalls, s.bufferBarriers,
            s.globalBarriers);
      }

      if (LoudShiftTick(tick)) {
        // The streaming shift, driven through the SAME sequence Stream::ShiftAxis
        // runs: evict the leaving plane (eager submit), advance the origin, then
        // refill — some slots from the store (deferred writes, NO submit), the
        // rest by procgen (a genList submit).
        //
        // The leaving plane is the LOW x edge; the entering plane is the HIGH x
        // edge after the origin moves. Their SLOTS are the same set (the window
        // is toroidal), which is the property `ShiftAxis` relies on and which
        // this reproduces.
        const int leaveX = origin.x;
        std::vector<IVec3> leaveChunks;
        std::vector<uint32_t> slots;
        leaveChunks.reserve(kNChunk * kNChunk);
        slots.reserve(kNChunk * kNChunk);
        for (int u = 0; u < (int)kNChunk; u++)
          for (int v = 0; v < (int)kNChunk; v++) {
            IVec3 wc{leaveX, origin.y + u, origin.z + v};
            leaveChunks.push_back(wc);
            slots.push_back(World::SlotChunkIndex(wc));
          }

        // Evict in batches, exactly as Stream does.
        for (size_t off = 0; off < slots.size(); off += 256) {
          size_t n = std::min<size_t>(256, slots.size() - off);
          vk::SimBackend::EvictBatch batch;
          if (!be.EvictSlots(slots.data() + off, (uint32_t)n, batch, err)) {
            std::printf("Vulkan evict: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n",
                        err.c_str());
            return 1;
          }
          const void* data = nullptr;
          if (!be.CompleteEvict(batch, data, err)) {
            std::printf("Vulkan evict complete: FAIL (%s)\n"
                        "=== --vk-smoke-loud FAIL ===\n", err.c_str());
            return 1;
          }
          if (data) {
            const uint32_t* w = (const uint32_t*)data;
            for (size_t i = 0; i < n; i++) {
              const uint32_t* chunkW = w + i * kChunkVol;
              // EVERY evicted chunk is stored — no all-air drop.
              //
              // Stream's `dropIfAir` is `modified_[s] == 0`, i.e. a chunk that
              // was MODIFIED is stored even when it decodes to all air, because
              // procgen would otherwise regenerate the terrain an explosion just
              // carved away. The first version here dropped all-air chunks
              // unconditionally, which is a DIFFERENT policy, and it produced a
              // divergence that appeared only on the walk's return leg — the
              // exact signature of a store-hit bug, and again not one. Storing
              // unconditionally is the policy `EvictSlots(filter=false)` uses
              // (what a save does) and needs no `modified_` mirror to be right.
              // THE STORE ROUND-TRIP IS NOT A MEMCPY, and getting this wrong
              // produced the phase-3c harness's one false divergence.
              //
              // `Stream`'s store goes through RleEncodeChunk/RleDecodeChunk,
              // which apply `kPersistMask` (stripping the tick-stamp bits
              // 16..23) on the way out and re-stamp every word with
              // `kStampNever` on the way back in. That is deliberate: the stamp
              // is per-tick scheduling scratch, and a restored voxel must be
              // born "has never acted" or it silently loses a substep
              // (CLAUDE.md's tick-stamp rule).
              //
              // Storing the raw evicted words instead preserved LIVE stamps, so
              // refilled chunks acted on a different substep than Dawn's and the
              // hashes diverged ~15 ticks after the first shift. That read
              // exactly like a barrier race and was not one — the emulation was
              // simply not the same store. Reproducing the mask + re-stamp here
              // is what makes the comparison a comparison.
              std::vector<uint32_t> chunk(kChunkVol);
              for (uint32_t k = 0; k < kChunkVol; k++)
                chunk[k] = (chunkW[k] & kPersistMask) | (kStampNever << kStampShift);
              store[World::PackChunkKey(leaveChunks[off + i])] = std::move(chunk);
              vkEvictedChunks++;
            }
          }
        }

        origin.x += 1;
        vkShifts++;

        // Refill the entering plane — the high edge, after the move.
        const int enterX = origin.x + (int)kNChunk - 1;
        std::vector<uint32_t> genSlots;
        for (int u = 0; u < (int)kNChunk; u++)
          for (int v = 0; v < (int)kNChunk; v++) {
            IVec3 wc{enterX, origin.y + u, origin.z + v};
            uint32_t slot = World::SlotChunkIndex(wc);
            auto it = store.find(World::PackChunkKey(wc));
            if (it != store.end()) {
              // THE STORE-HIT PATH. Four deferred writes per slot and NO submit
              // — barrier_graph §4.1's case that a "uploads ride their own
              // submit" model has no home for. They drain into the next tick's
              // command buffer.
              uint32_t occ = 0, blockers = 0;
              for (uint32_t v2 : it->second)
                if ((v2 & 0xFFFu) != 0) occ++;
              be.FillSlotFromStore(slot, it->second.data(), occ | (blockers << 16));
              vkStoreHits++;
            } else {
              genSlots.push_back(slot);
            }
          }
        if (!genSlots.empty()) {
          if (!be.FillSlotsByGen(genSlots.data(), (uint32_t)genSlots.size(), kDefaultSeed,
                                 origin, err)) {
            std::printf("Vulkan genfill: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n",
                        err.c_str());
            return 1;
          }
          vkGenFills += (uint32_t)genSlots.size();
        }
      }

      if (!LoudProbe(tick)) continue;
      if (!LoudHashTick(tick)) {
        if (!be.SubmitHashOnly(kDefaultSeed, err)) {
          std::printf("Vulkan rehash t%u: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n",
                      tick, err.c_str());
          return 1;
        }
      }
      uint32_t h = 0;
      if (!be.ReadHash(h, err)) {
        std::printf("Vulkan hash read t%u: FAIL (%s)\n=== --vk-smoke-loud FAIL ===\n",
                    tick, err.c_str());
        return 1;
      }
      const WorldSnapshot& sn = be.Snap();
      vkProbes.push_back({tick, h, sn.activeChunks, sn.particleCount, sn.voxelTotal});
    }

    // ---- the store-hit refill, proven directly -------------------------
    //
    // This is the one path the cross-backend hash above deliberately does NOT
    // cover (see LoudShiftTick). It is also the path §4.1 singles out as the one
    // a naive port has no home for, so it gets its own assertion rather than
    // being taken on trust:
    //
    //   1. Evict a plane (eager submit, fence wait) and keep the words.
    //   2. Overwrite those same slots on the GPU with a recognisable pattern,
    //      via a genList procgen fill, so a "refill did nothing" bug cannot pass
    //      by leaving the original data in place.
    //   3. Refill them from the captured words through FillSlotFromStore —
    //      four deferred writes per slot, AND NOTHING SUBMITTED.
    //   4. Run one ordinary tick. Its command buffer is the first one recorded
    //      after step 3, so per §4.1 it must flush those uploads at its head.
    //   5. Evict the plane again and compare against step 1.
    //
    // Step 4 is the whole point: if the deferred writes did not attach to the
    // next command buffer from a DIFFERENT code path, the comparison in step 5
    // fails. A model where uploads ride their own submit passes steps 1-3 and
    // fails here.
    {
      std::vector<uint32_t> checkSlots;
      std::vector<IVec3> checkChunks;
      const int planeX = origin.x + 1;  // a plane well inside the window
      for (int u = 0; u < 8; u++)
        for (int v = 0; v < 8; v++) {
          IVec3 wc{planeX, origin.y + u, origin.z + v};
          checkChunks.push_back(wc);
          checkSlots.push_back(World::SlotChunkIndex(wc));
        }

      std::vector<uint32_t> captured;
      vk::SimBackend::EvictBatch b1;
      const void* d1 = nullptr;
      bool sc = be.EvictSlots(checkSlots.data(), (uint32_t)checkSlots.size(), b1, err) &&
                be.CompleteEvict(b1, d1, err) && d1 != nullptr;
      if (sc) {
        const uint32_t* w = (const uint32_t*)d1;
        captured.assign(w, w + checkSlots.size() * kChunkVol);
        // Apply the store's round-trip so the comparison is against what a real
        // store would have handed back, not against the raw eviction.
        for (uint32_t& x : captured)
          x = (x & kPersistMask) | (kStampNever << kStampShift);

        // Step 2: clobber the slots with procgen for a DIFFERENT origin, which
        // guarantees different terrain in them.
        IVec3 bogus{origin.x + 4096, origin.y, origin.z};
        sc = be.FillSlotsByGen(checkSlots.data(), (uint32_t)checkSlots.size(),
                               kDefaultSeed ^ 0x5bd1u, bogus, err);
      }
      if (sc) {
        // Step 3: the store-hit refill. Deferred, submit-less.
        for (size_t i = 0; i < checkSlots.size(); i++) {
          const uint32_t* src = captured.data() + i * kChunkVol;
          uint32_t occ = 0;
          for (uint32_t k = 0; k < kChunkVol; k++)
            if ((src[k] & 0xFFFu) != 0) occ++;
          be.FillSlotFromStore(checkSlots[i], src, occ);
          vkStoreHits++;
        }
        // Step 4: one ordinary tick, whose command buffer must flush them.
        vk::SimBackend::TickInputs quiet{};
        quiet.tick = kLoudTicks + 1;
        quiet.seed = kDefaultSeed;
        quiet.windowOrigin = origin;
        sc = be.SubmitTickFull(quiet, err);
      }
      if (sc) {
        // Step 5: read them back and compare the MATERIAL fields. The tick in
        // step 4 may legitimately have moved a voxel (falling sand), so the
        // assertion is that the plane is the restored world rather than the
        // clobbered one — a byte-exact compare would be testing the CA, not the
        // upload path.
        vk::SimBackend::EvictBatch b2;
        const void* d2 = nullptr;
        sc = be.EvictSlots(checkSlots.data(), (uint32_t)checkSlots.size(), b2, err) &&
             be.CompleteEvict(b2, d2, err) && d2 != nullptr;
        if (sc) {
          const uint32_t* w = (const uint32_t*)d2;
          size_t total = checkSlots.size() * kChunkVol, same = 0;
          for (size_t k = 0; k < total; k++)
            if ((w[k] & 0xFFFu) == (captured[k] & 0xFFFu)) same++;
          double pct = 100.0 * (double)same / (double)total;
          storeHitPct = pct;
          // One tick of settling moves a small fraction of cells; anything
          // above 99% means the restore landed and the clobber did not survive.
          storeHitOk = pct > 99.0;
        }
      }
      if (!sc) storeHitErr = err;
    }

    vkMsgs = be.Be().ValidationMessages();
    vkValidationOn = be.GetCaps().validationEnabled;
    be.Shutdown();
  }

  // ------------------------------------------------------------ compare --
  bool ok = true;
  std::printf("\n=== validation ===\n");
  if (!vkValidationOn) {
    std::printf("  layer not enabled for this run\n");
  } else if (vkMsgs.empty()) {
    std::printf("  ZERO messages (no synchronization hazards reported)\n");
  } else {
    std::printf("  %zu message(s):\n", vkMsgs.size());
    for (const auto& m : vkMsgs) std::printf("    %s\n", m.c_str());
    ok = false;
  }

  std::printf("\n=== streaming ===\n");
  std::printf("  Dawn:   %u window shifts, %zu chunks in store\n", dawnShifts,
              (size_t)dawnEvicted);
  std::printf("  Vulkan: %u window shifts, %u chunks evicted, %u store-hit refills,"
              " %u procgen refills\n",
              vkShifts, vkEvictedChunks, vkStoreHits, vkGenFills);
  // The store-hit path is proven here rather than by the hash table, because the
  // two backends cannot share a store (see LoudShiftTick).
  if (!storeHitErr.empty()) {
    std::printf("  store-hit refill self-check: ERROR (%s)\n", storeHitErr.c_str());
    ok = false;
  } else {
    std::printf("  store-hit refill self-check: %s (%.2f%% of the plane restored"
                " through deferred, submit-less writes flushed by the next tick)\n",
                storeHitOk ? "PASS" : "*** FAIL ***", storeHitPct);
    if (!storeHitOk) ok = false;
  }

  std::printf("\n=== hashes ===\n");
  std::printf("  %-8s %-10s %-10s %-8s %s\n", "stage", "Dawn", "Vulkan", "", "snapshot");
  std::printf("  %-8s %08x   %08x   %s\n", "worldgen", dawnGenHash, vkGenHash,
              dawnGenHash == vkGenHash ? "MATCH" : "*** MISMATCH ***");
  if (dawnGenHash != vkGenHash) ok = false;

  size_t n = std::min(dawnProbes.size(), vkProbes.size());
  for (size_t i = 0; i < n; i++) {
    const Probe& d = dawnProbes[i];
    const Probe& v = vkProbes[i];
    bool m = d.hash == v.hash;
    if (!m) ok = false;
    char label[24];
    std::snprintf(label, sizeof(label), "t%u", d.tick);
    // The snapshot columns are the READBACK RING's output, not the hash's: they
    // come from the CPU mirror both backends populate from their own slot
    // buffers. Reporting them alongside is what makes the ring part of the
    // evidence rather than something that merely did not crash.
    std::printf("  %-8s %08x   %08x   %-8s active %u/%u  parts %u/%u\n", label, d.hash,
                v.hash, m ? "MATCH" : "*** MISMATCH ***", d.activeChunks, v.activeChunks,
                d.particleCount, v.particleCount);
  }
  if (dawnProbes.size() != vkProbes.size()) {
    std::printf("  *** probe count differs: Dawn %zu, Vulkan %zu ***\n",
                dawnProbes.size(), vkProbes.size());
    ok = false;
  }

  std::printf("\n=== --vk-smoke-loud %s ===\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

}  // namespace sandvox
