// vk_smoke.cpp — cross-backend world-hash comparison (--vk-smoke, --vk-smoke-loud).
//
// PHASE 4a REWRITE: both backends now run the IDENTICAL driver — GpuContext +
// World + Simulation + Stream through the polymorphic rhi:: seam, differing
// only in the backend argument to GpuContext::Init. Phase 3b/3c's parallel
// vk::SimBackend (a second copy of every resource declaration) is deleted; what
// this smoke now proves is exactly what `--selftest --backend vulkan` relies
// on: the ONE World, driven by the one SubmitTick, hashes identically on both
// backends.
//
// The scenario scripts are pure functions of tick (CLAUDE.md rule 1 dis-
// cipline), unchanged from phase 3c. The streaming walk stays ONE-DIRECTIONAL:
// with real Streams on both sides the outbound leg's entering planes are always
// store misses, so both backends refill by procgen from the same seed — content
// identical by construction. A return leg's content depends on each side's
// store, whose dropIfAir decisions ride snapshot timing (ring latency), so its
// hashes can differ for reasons that are policy, not barriers — phase 3c
// measured exactly that. The store-hit round trip is now covered FOR REAL by
// the save-load / region-store gates on --backend vulkan.

#include "gpu/vk_smoke.h"

#include <cstdio>
#include <string>
#include <vector>

#include "gpu/context.h"
#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/tuning.h"
#include "sim/world.h"
#include "test/support.h"

namespace sandvox {
namespace {

// ---------------------------------------------------------- scenarios ------

constexpr uint32_t kQuietTicks = 50;
constexpr uint32_t kLoudTicks = 120;

bool QuietProbe(uint32_t t) { return t == 1 || t == 15 || t == 30 || t == 50; }

// The loud scenario, verbatim from phase 3c: ops shaped to light up every
// condition the quiet world leaves dark, and to keep them OVERLAPPING (a tick
// with ops AND particles AND a hash tick is where a missing barrier between two
// conditional rows would show).
std::vector<BrushOp> LoudOps(uint32_t tick) {
  std::vector<BrushOp> ops;
  if (tick >= 3 && tick < 100) ops.push_back({100, 170, 100, 6, kMatSand, 0, 0, 0});
  if (tick >= 8 && tick < 90) ops.push_back({176, 150, 176, 5, kMatWater, 0, 0, 0});
  if (tick >= 40 && tick < 100) ops.push_back({176, 120, 150, 4, kMatLava, 0, 0, 0});
  if (tick >= 60 && tick < 110) ops.push_back({110, 80, 110, 3, kMatFire, 0, 0, 0});
  if (tick >= 70 && tick < 100) ops.push_back({100, 166, 100, 3, 0, 2u, 0, 0});
  return ops;
}

std::vector<ExplosionOp> LoudExps(uint32_t tick, uint32_t seed) {
  std::vector<ExplosionOp> exps;
  if (tick == 45) {
    int h = World::TerrainHeight(100, 100, seed);
    exps.push_back({100, h, 100, 14, 400, 0, 0, 0});
  }
  if (tick == 52) exps.push_back({176, 50, 176, 10, 300, 0, 0, 0});
  if (tick == 75) {
    int h = World::TerrainHeight(120, 120, seed);
    exps.push_back({120, h, 120, 12, 350, 0, 0, 0});
  }
  return exps;
}

std::vector<CellOp> LoudCells(uint32_t tick, IVec3 windowOrigin) {
  std::vector<CellOp> cells;
  if (tick != 20 && tick != 64) return cells;
  // Anchor to the LIVE window origin, never a fixed world position — the
  // streaming walk moves the window (the CLAUDE.md selftest-gate trap).
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

bool LoudProbe(uint32_t t) {
  return t == 15 || t == 30 || t == 45 || t == 46 || t == 47 || t == 52 || t == 53 ||
         t == 60 || t == 75 || t == 76 || t == 84 || t == 85 || t == 86 || t == 87 ||
         t == 88 || t == 90 || t == 105 || t == 120;
}

bool HashTick(uint32_t tick) { return tick % 15 == 0; }

// The window advances one chunk on +X at these ticks: eviction of the leaving
// plane, procgen refill of the entering one. 8 shifts.
bool LoudShiftTick(uint32_t t) { return t >= 85 && t <= 100 && (t % 2) == 0; }

// ------------------------------------------------------------ the driver ----

struct Probe {
  uint32_t tick = 0;
  uint32_t hash = 0;
};

struct RunResult {
  bool ok = false;
  uint32_t genHash = 0;
  std::vector<Probe> probes;
  uint32_t shifts = 0;
  size_t storeCount = 0;
  rhi::vkr::Stats stats{};          // Vulkan only: last recorded command buffer
  size_t validationMsgCount = 0;    // Vulkan only
  std::vector<std::string> validationMsgs;
};

const char* KindName(rhi::BackendKind k) {
  return k == rhi::BackendKind::Vulkan ? "Vulkan" : "Dawn";
}

// ONE driver for both backends — that is the point of the phase-4a rewrite.
bool RunScenario(rhi::BackendKind kind, bool loud, bool lowPower, bool sledgehammer,
                 bool validation, const std::vector<MaterialDef>& mats,
                 const std::vector<ReactionGpu>& reactions, const std::string& assetDir,
                 RunResult& out) {
  GpuContext ctx;
  if (!ctx.Init(nullptr, 1600, 900, lowPower, false, kind,
                kind == rhi::BackendKind::Vulkan && validation,
                kind == rhi::BackendKind::Vulkan && sledgehammer)) {
    std::printf("%s device init: FAIL\n", KindName(kind));
    return false;
  }
  World world;
  world.Init(ctx.device);
  Simulation sim;
  MicroSet micro;
  if (!sim.Init(ctx.device, world, mats, reactions, micro, assetDir + "/shaders")) {
    std::printf("%s sim init: FAIL\n", KindName(kind));
    return false;
  }
  Stream stream;
  stream.Init(&ctx, &world, &sim, kDefaultSeed);
  stream.OnMaterialsReloaded(mats);

  SubmitWorldgen(ctx, world, sim, kDefaultSeed);
  out.genHash = HashWorldNow(ctx, world, sim, kDefaultSeed);

  const uint32_t ticks = loud ? kLoudTicks : kQuietTicks;
  for (uint32_t tick = 1; tick <= ticks; tick++) {
    IVec3 wo = world.WindowOrigin();
    std::vector<BrushOp> ops;
    std::vector<ExplosionOp> exps;
    std::vector<CellOp> cells;
    bool particles = false;
    if (loud) {
      ops = LoudOps(tick);
      exps = LoudExps(tick, kDefaultSeed);
      cells = LoudCells(tick, wo);
      particles = LoudParticlesActive(tick);
    }
    // playerChunk tracks the window centre so the 3x3x3 mirror is populated and
    // the readback ring carries something real (loud only, matching 3c).
    IVec3 playerChunk{wo.x + (int)kNChunk / 2, wo.y + (int)kNChunk / 2,
                      wo.z + (int)kNChunk / 2};
    SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cells, HashTick(tick),
               playerChunk, /*wantReadback=*/loud, particles, {}, 0);
    ctx.ProcessEvents();

    if (loud && LoudShiftTick(tick)) {
      // Stream::Update's hysteresis decides the shift; a player two chunks past
      // centre is how the game reaches the same code.
      IVec3 target{wo.x + (int)kNChunk / 2 + 2, wo.y + (int)kNChunk / 2,
                   wo.z + (int)kNChunk / 2};
      uint32_t before = stream.ShiftCount();
      stream.Update(target);
      out.shifts += stream.ShiftCount() - before;
    }

    const bool probe = loud ? LoudProbe(tick) : QuietProbe(tick);
    if (!probe) continue;
    // A hash tick's value is already in world.hash; anything else needs a
    // standalone rehash (PT_HASHONLY) so the probe reads THIS tick's world.
    uint32_t h = HashTick(tick) ? ReadHashSync(ctx, world)
                                : HashWorldNow(ctx, world, sim, kDefaultSeed);
    out.probes.push_back({tick, h});
  }

  if (loud) {
    stream.FlushResident();
    out.storeCount = stream.Store().Count();
  }
  ctx.WaitIdle();

  if (kind == rhi::BackendKind::Vulkan) {
    out.stats = rhi::vkr::LastStats(ctx.device);
    if (vk::Backend* be = ctx.VkBackend()) {
      out.validationMsgCount = be->ValidationMessages().size();
      for (size_t i = 0; i < be->ValidationMessages().size() && i < 8; i++)
        out.validationMsgs.push_back(be->ValidationMessages()[i]);
    }
  }
  out.ok = true;
  return true;
}

// ------------------------------------------------------------- reporting ----

int CompareAndReport(const char* name, const RunResult& dawn, const RunResult& vkr,
                     bool validation) {
  std::printf("\n=== validation ===\n");
  if (!validation) {
    std::printf("  (off — rerun with --vk-validation)\n");
  } else if (vkr.validationMsgCount == 0) {
    std::printf("  ZERO messages (no synchronization hazards reported)\n");
  } else {
    std::printf("  *** %zu validation message(s) ***\n", vkr.validationMsgCount);
    for (const std::string& m : vkr.validationMsgs)
      std::printf("    %s\n", m.c_str());
  }

  std::printf("\n=== hashes ===\n");
  std::printf("  stage              Dawn         Vulkan\n");
  bool allMatch = true;
  uint32_t matches = 0, total = 0;
  auto row = [&](const char* label, uint32_t a, uint32_t b) {
    bool m = a == b;
    allMatch = allMatch && m;
    total++;
    if (m) matches++;
    std::printf("  %-16s   %08x     %08x     %s\n", label, a, b,
                m ? "MATCH" : "*** MISMATCH ***");
  };
  row("worldgen", dawn.genHash, vkr.genHash);
  size_t n = dawn.probes.size() < vkr.probes.size() ? dawn.probes.size()
                                                    : vkr.probes.size();
  for (size_t i = 0; i < n; i++) {
    char label[32];
    std::snprintf(label, sizeof(label), "tick %u", dawn.probes[i].tick);
    row(label, dawn.probes[i].hash, vkr.probes[i].hash);
  }
  if (dawn.probes.size() != vkr.probes.size()) {
    std::printf("  *** probe count differs: Dawn %zu, Vulkan %zu ***\n",
                dawn.probes.size(), vkr.probes.size());
    allMatch = false;
  }
  std::printf("  %u/%u MATCH\n", matches, total);

  const bool hazards = validation && vkr.validationMsgCount != 0;
  const bool pass = allMatch && !hazards;
  std::printf("\n=== %s %s ===\n", name, pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

bool LoadSmokeAssets(std::vector<MaterialDef>& mats, std::vector<ReactionGpu>& reactions,
                     std::string& assetDir) {
  assetDir = AssetDir();
  Tuning tuning;
  if (LoadTuning(assetDir + "/materials/tuning.json", tuning)) SetCurrentTuning(tuning);
  std::string errors;
  if (!LoadAssets(assetDir + "/materials/materials.json",
                  assetDir + "/materials/reactions.json", mats, reactions, errors)) {
    std::printf("asset load failed:\n%s\n", errors.c_str());
    return false;
  }
  std::printf("loaded %zu materials, %zu reactions\n", mats.size(), reactions.size());
  return true;
}

int RunSmoke(bool loud, bool lowPower, bool sledgehammer, bool validation) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* name = loud ? "--vk-smoke-loud" : "--vk-smoke";
  std::printf("=== sandvox %s (Vulkan port phase 4a: one World, two backends) ===\n",
              name);
  std::printf("mode: barriers=%s validation=%s adapter=%s seed=%u ticks=%u\n",
              sledgehammer ? "sledgehammer" : "precise", validation ? "ON" : "off",
              lowPower ? "low" : "default", kDefaultSeed,
              loud ? kLoudTicks : kQuietTicks);
  if (loud)
    std::printf(
        "scenario: brush+melt ops, 3 explosions (spawn/integrate/resolve), exact-cell\n"
        "          ops, readback ring active, 8-shift streaming walk (evict + procgen\n"
        "          refill; the store-hit round trip is covered by the save gates on\n"
        "          --selftest --backend vulkan)\n");

  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string assetDir;
  if (!LoadSmokeAssets(mats, reactions, assetDir)) {
    std::printf("\n=== %s FAIL ===\n", name);
    return 1;
  }

  RunResult dawn, vkr;
  if (!RunScenario(rhi::BackendKind::Dawn, loud, lowPower, sledgehammer, validation,
                   mats, reactions, assetDir, dawn) ||
      !RunScenario(rhi::BackendKind::Vulkan, loud, lowPower, sledgehammer, validation,
                   mats, reactions, assetDir, vkr)) {
    std::printf("\n=== %s FAIL ===\n", name);
    return 1;
  }

  std::printf("\n  Vulkan tick recording: %u rows, %u dispatches, %u copies, %u fills, "
              "%u barrier calls (%u buffer + %u global)\n",
              vkr.stats.rows, vkr.stats.dispatches, vkr.stats.copies, vkr.stats.fills,
              vkr.stats.barrierCalls, vkr.stats.bufferBarriers,
              vkr.stats.globalBarriers);
  if (loud) {
    std::printf("\n=== streaming ===\n");
    std::printf("  Dawn:   %u window shifts, %zu chunks in store\n", dawn.shifts,
                dawn.storeCount);
    std::printf("  Vulkan: %u window shifts, %zu chunks in store\n", vkr.shifts,
                vkr.storeCount);
    std::printf("  (store counts may differ legitimately: dropIfAir rides snapshot\n"
                "   timing; world CONTENT on the outbound walk does not)\n");
  }

  return CompareAndReport(name, dawn, vkr, validation);
}

}  // namespace

int RunVkSmoke(bool lowPower, bool sledgehammer, bool validation) {
  return RunSmoke(/*loud=*/false, lowPower, sledgehammer, validation);
}

int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation) {
  return RunSmoke(/*loud=*/true, lowPower, sledgehammer, validation);
}

}  // namespace sandvox
