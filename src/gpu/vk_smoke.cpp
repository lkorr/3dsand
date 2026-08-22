// vk_smoke.cpp — the cross-backend hash comparison. See vk_smoke.h.

#include "gpu/vk_smoke.h"

#include <cstdio>
#include <string>
#include <vector>

#include "gpu/context.h"
#include "gpu/vk_record.h"
#include "gpu/vk_sim.h"
#include "sim/materials.h"
#include "sim/simulation.h"
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

}  // namespace sandvox
