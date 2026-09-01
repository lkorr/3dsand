// vk_smoke.cpp — PINNED world-hash sequence regression gates
//                (--vk-smoke, --vk-smoke-loud).
//
// DAWN REMOVAL REWRITE (2026-08-22). These two harnesses used to run the SAME
// scenario on Dawn and on Vulkan and diff the hash sequences; Dawn's
// auto-generated barriers were the reference implementation of
// docs/vulkan_barrier_graph.md, so a divergence localised a missing barrier.
// With Dawn gone there is no second backend to diff against.
//
// WHAT REPLACES IT, AND WHY THE REGRESSION POWER SURVIVES. The comparison was
// never really "Dawn vs Vulkan" — it was "this recording vs a known-good hash
// sequence", and Dawn happened to be how that sequence was produced. So the
// sequences are now PINNED as constants below, taken verbatim from the commits
// that established them under the cross-backend diff (10156bb for the quiet
// scenario, c0cc28f for the loud one) and reproduced unchanged by every phase
// since. A run PASSES when every probe equals its pinned value.
//
// That is strictly MORE regression coverage than the diff had in one respect
// and less in another, and both are worth being precise about:
//
//   + The pinned value catches a change that would have moved BOTH backends
//     identically — a WGSL edit, a materials.json reorder, a worldgen tweak.
//     The old diff was blind to those by construction.
//   - It cannot catch a barrier bug by *disagreement*, because there is
//     nothing left to disagree. What covers that now is the other half of the
//     original evidence, which was always the stronger half:
//     `--vk-validation` turns on synchronization validation, barrier_graph
//     §6.2's PRIMARY missing-barrier detector, and BOTH gates FAIL on a
//     single message. `--barriers=sledgehammer` remains as the §6.2 A/B
//     oracle: identical hashes under maximal ordering exonerates the barrier
//     graph (weak evidence, as §6.2 rates it, but free).
//
// An intentional content change flips the pinned values here in the same
// commit, exactly like flipping a known failure in tests/baseline.json — and
// the same warning applies: doing that to silence a surprise is how a real
// regression gets buried.
//
// The scenario scripts are pure functions of tick (CLAUDE.md rule 1
// discipline), unchanged since phase 3c — that is what makes pinning legal at
// all. The streaming walk stays ONE-DIRECTIONAL for the reason phase 3c
// documented: a return leg's content rides store policy and snapshot timing
// rather than barriers. The store-hit round trip is covered for real by the
// save-load / region-store selftest gates.
//
// PHASE 7 WILL REUSE THIS DRIVER. `RunScenario` takes the scenario and returns
// a hash sequence; `--residency` (dense vs paged) is the same shape as the old
// cross-backend diff — two configurations of one driver, sequences compared —
// so the RunResult/probe/compare structure is kept intact rather than folded
// into the pinned check.

#include "gpu/vk_smoke.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>
#include <vector>

#include "gpu/context.h"
#include "gpu/rhi_vk.h"
#include "gpu/rhi_vulkan.h"
#include "sim/materials.h"
#include "sim/pagetable.h"   // WorldSeed(), for the JITTER digest
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

// The tick whose command-buffer recording stats get printed. Both are ordinary
// non-hash, non-probe ticks in the middle of the scenario: quiet t=20 is the
// plain CA path, loud t=50 has ops, explosion fallout and the particle chain
// all live at once.
constexpr uint32_t kQuietStatsTick = 20;
constexpr uint32_t kLoudStatsTick = 50;

// ------------------------------------------------------- the pinned truth ---
//
// Probe tables live in tests/baseline.json ("smokeQuiet", "smokeLoud"), edited
// by --rebaseline without a C++ rebuild. The constants below are compile-time
// fallbacks used only when the JSON is missing or unparseable — they MUST stay
// in sync with the JSON (--rebaseline updates both).
//
// History and interpretation of each rebaseline: tests/SMOKE_PROBES.md.
struct Pinned {
  uint32_t tick;
  uint32_t hash;
};

constexpr Pinned kQuietPinnedFallback[] = {
    {0, 0xf97ba745}, {1, 0xcfe240ba}, {15, 0xeb6d7ae2},
    {30, 0x3d5c1554}, {50, 0xaddff010},
};

constexpr Pinned kLoudPinnedFallback[] = {
    {0, 0xf97ba745},
    {15, 0xc0cac9cc},  {30, 0xdf26b3d5},  {45, 0x61307cdc},  {46, 0xb8dc4159},
    {47, 0x9c0dc35f},  {52, 0x96d0e00c},  {53, 0x0f088b6a},  {60, 0x53750651},
    {75, 0x47251d4c},  {76, 0xe08670f6},  {84, 0xc42a6f17},  {85, 0x26be6f11},
    {86, 0xf0e69bc0},  {87, 0x27d0fee2},  {88, 0xccb16cb2},  {90, 0x3aa40820},
    {105, 0xbd84ff0a}, {120, 0x3145fde8},
};

std::string BaselinePath() {
  namespace fs = std::filesystem;
  fs::path assets(AssetDir());
  fs::path guess = assets.parent_path() / "tests" / "baseline.json";
  return fs::exists(guess) ? guess.string() : std::string("tests/baseline.json");
}

// Parse a JSON array of [tick, "hexhash"] pairs from baseline.json. Hand-rolled
// to avoid a JSON dependency — the format is rigid enough that a simple scanner
// works.
bool LoadPinnedFromJson(const std::string& path, const char* key,
                        std::vector<Pinned>& out) {
  std::ifstream f(path);
  if (!f) return false;
  std::string text((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  // Find "smokeQuiet": [ or "smokeLoud": [
  std::string needle = std::string("\"") + key + "\"";
  size_t pos = text.find(needle);
  if (pos == std::string::npos) return false;
  pos = text.find('[', pos);
  if (pos == std::string::npos) return false;
  pos++; // skip outer [
  while (pos < text.size()) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' ||
           text[pos] == '\r' || text[pos] == '\t' || text[pos] == ','))
      pos++;
    if (pos >= text.size() || text[pos] == ']') break;
    if (text[pos] != '[') return false;
    pos++;
    // parse tick number. Whitespace here and before the hash includes
    // NEWLINES: --rebaseline writes the file through a pretty-printer that puts
    // each element on its own line, and a parser that only skipped spaces fell
    // back to the stale hardcoded tables in silence, failing every probe
    // against numbers the JSON had long since moved past.
    auto ws = [&](char c) {
      return c == ' ' || c == '\n' || c == '\r' || c == '\t';
    };
    while (pos < text.size() && ws(text[pos])) pos++;
    uint32_t tick = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
      tick = tick * 10 + (text[pos++] - '0');
    while (pos < text.size() && (ws(text[pos]) || text[pos] == ',')) pos++;
    // parse "hexhash"
    if (pos >= text.size() || text[pos] != '"') return false;
    pos++;
    uint32_t hash = 0;
    while (pos < text.size() && text[pos] != '"') {
      char c = text[pos++];
      hash <<= 4;
      if (c >= '0' && c <= '9') hash |= (c - '0');
      else if (c >= 'a' && c <= 'f') hash |= (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') hash |= (c - 'A' + 10);
    }
    if (pos < text.size()) pos++; // skip closing "
    while (pos < text.size() && text[pos] != ']') pos++;
    if (pos < text.size()) pos++; // skip ]
    out.push_back({tick, hash});
  }
  return !out.empty();
}

struct PinnedTable {
  std::vector<Pinned> vec;
  const Pinned* data;
  size_t count;
};

PinnedTable LoadPinned(const char* jsonKey, const Pinned* fallback, size_t fallbackCount) {
  PinnedTable t;
  std::string path = BaselinePath();
  if (LoadPinnedFromJson(path, jsonKey, t.vec)) {
    t.data = t.vec.data();
    t.count = t.vec.size();
    std::printf("  loaded %zu %s probes from %s\n", t.count, jsonKey, path.c_str());
  } else {
    t.data = fallback;
    t.count = fallbackCount;
    std::printf("  using %zu fallback %s probes (no JSON)\n", t.count, jsonKey);
  }
  return t;
}

// The loud scenario, verbatim from phase 3c: ops shaped to light up every
// condition the quiet world leaves dark, and to keep them OVERLAPPING (a tick
// with ops AND particles AND a hash tick is where a missing barrier between two
// conditional rows would show).
// Terrain-relative, for the reason spelled out over SelftestOps in
// test/support.cpp: an absolute Y here is silently anchored to whatever terrain
// band happened to exist when it was written, and the smoke scenario stops
// exercising anything the moment the datum moves. This is the SECOND,
// independent copy of that scenario (vk_smoke deliberately does not link the
// selftest harness), so both had to move together.
std::vector<BrushOp> LoudOps(uint32_t tick, uint32_t seed) {
  std::vector<BrushOp> ops;
  auto ground = [&](int x, int z) { return World::TerrainHeight(x, z, seed); };
  if (tick >= 3 && tick < 100)
    ops.push_back({100, ground(100, 100) + 110, 100, 6, kMatSand, 0, 0, 0});
  if (tick >= 8 && tick < 90)
    ops.push_back({176, ground(176, 176) + 90, 176, 5, kMatWater, 0, 0, 0});
  if (tick >= 40 && tick < 100)
    ops.push_back({176, ground(176, 150) + 60, 150, 4, kMatLava, 0, 0, 0});
  if (tick >= 60 && tick < 110)
    ops.push_back({110, ground(110, 110) + 20, 110, 3, kMatFire, 0, 0, 0});
  if (tick >= 70 && tick < 100)
    ops.push_back({100, ground(100, 100) + 106, 100, 3, 0, 2u, 0, 0});
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
  // Load-bearing evidence for §3.4's adjacency argument (see RunSmoke).
  uint32_t pageFaults = 0;
  rhi::vkr::Stats stats{};       // last recorded command buffer
  size_t validationMsgCount = 0;
  std::vector<std::string> validationMsgs;
};

// One driver, one configuration today. Phase 7's --residency dense-vs-paged
// comparison is the same function called twice with a different residency
// mode, which is why it still returns a whole RunResult rather than a verdict.
bool RunScenario(bool loud, bool lowPower, bool sledgehammer, bool validation,
                 bool paged,
                 const std::vector<MaterialDef>& mats,
                 const std::vector<ReactionGpu>& reactions, const std::string& assetDir,
                 RunResult& out) {
  GpuContext ctx;
  if (!ctx.Init(nullptr, 1600, 900, lowPower, false, rhi::BackendKind::Vulkan,
                validation, sledgehammer)) {
    std::printf("device init: FAIL\n");
    return false;
  }
  SetHarnessSnapshotDrain(true);  // see test/support.h
  World world;
  // The residency axis (§4.4 Gate A): one driver, two configurations. Both must
  // produce identical hashes at every probe AND reproduce the pinned constants.
  world.residency = paged ? World::Residency::Paged : World::Residency::Dense;
  world.Init(ctx.device);
  Simulation sim;
  MicroSet micro;
  // The smoke harness runs the REAL worldgen, so it needs the real trees --
  // without them every probe would hash a treeless world and the pinned tables
  // would silently describe a different game.
  TreeAtlas treeAtlas;
  {
    std::string tlog;
    if (!LoadTreeAtlas(assetDir + "/trees", mats, treeAtlas, tlog)) {
      std::printf("tree atlas: FAIL\n%s", tlog.c_str());
      return false;
    }
  }
  if (!sim.Init(ctx.device, world, mats, reactions, micro, treeAtlas,
                assetDir + "/shaders")) {
    std::printf("sim init: FAIL\n");
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
      ops = LoudOps(tick, kDefaultSeed);
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
    // Snapshot the recording stats HERE, off a representative tick, rather
    // than after the loop: the last command buffer a run submits is a
    // standalone rehash or a stream flush, whose 0 rows / 1 copy says nothing
    // about the tick table. `kStatsTick` is chosen inside each scenario's
    // busiest stretch so the printed line is the one worth comparing against
    // the phase-3c/4a records (quiet 11/59/2/3/63, loud 20/64/38/5/104).
    if (tick == (loud ? kLoudStatsTick : kQuietStatsTick))
      out.stats = rhi::vkr::LastStats(ctx.device);
    ctx.ProcessEvents();

    if (loud && LoudShiftTick(tick)) {
      // Stream::Update's hysteresis decides the shift; a player two chunks past
      // centre is how the game reaches the same code.
      IVec3 target{wo.x + (int)kNChunk / 2 + 2, wo.y + (int)kNChunk / 2,
                   wo.z + (int)kNChunk / 2};
      uint32_t before = stream.ShiftCount();
      stream.Update(target, tick);
      out.shifts += stream.ShiftCount() - before;
    }

    // PAGED-DIVERGENCE DIAGNOSTICS, all behind SANDVOX_PT_DEBUG and each behind
    // its own variable, so a normal run pays nothing (not even a readback).
    // These are the tools that localised BOTH phase-7 bugs and they are kept
    // deliberately: a dense-vs-paged hash mismatch is otherwise a 32,768-chunk
    // haystack, and this is the dump-and-diff that reduces it to one voxel.
    //
    //   SANDVOX_PT_DUMPSLOT=<slot>   per-tick contents + page-table entry of one
    //                                slot. Found the induction base case: slot
    //                                11531 sat at PT_EMPTY through tick 8 in
    //                                paged while dense already held the water.
    //   SANDVOX_PT_DIGEST=<tick>     per-chunk FNV digest of the WHOLE world at
    //                                that tick. Diff a dense run against a paged
    //                                run to get the exact divergent slots.
    //   SANDVOX_PT_WORDS=<slot>      with DIGEST, dump that slot's non-zero
    //                                words. Found the RNG bug: one lava voxel
    //                                and its neighbour SWAPPED, nothing lost.
    if (getenv("SANDVOX_PT_DEBUG")) {
      if (const char* ds = getenv("SANDVOX_PT_DUMPSLOT")) {
        const uint32_t slot = (uint32_t)atoi(ds);
        const uint32_t entry = world.PageEntryOfSlot(slot);
        uint32_t nonZero = 0, fullSum = 0, solid = 0;
        if ((entry & kPtSentinelBit) == 0u) {
          std::vector<uint32_t> words(kChunkVol);
          rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels,
                                (uint64_t)entry * kChunkVol * 4, words.data(),
                                kChunkVol * 4, "slotDump");
          for (uint32_t w : words)
            if (w != 0) {
              nonZero++;
              fullSum += (w >> 12) & 0xFu;
              if ((w & 0xFFFu) != 0u) solid++;
            }
        }
        std::printf("[pin] tick %u slot %u entry=0x%08X nonZero=%u mat!=0=%u "
                    "fullSum=%u\n",
                    tick, slot, entry, nonZero, solid, fullSum);
      }
      // SANDVOX_PT_DIGEST=<tick>: per-chunk digest of the WHOLE world at that
      // tick, so a dense run and a paged run can be diffed to the exact slot.
      // This is the dump-and-diff that localises a divergence instead of
      // guessing at it.
      if (const char* dt = getenv("SANDVOX_PT_DIGEST")) {
        if (tick == (uint32_t)atoi(dt)) {
          ctx.WaitIdle();
          std::vector<uint32_t> words(kChunkVol);
          for (uint32_t s = 0; s < kNumChunks; s++) {
            const uint32_t entry = world.PageEntryOfSlot(s);
            uint32_t h = 2166136261u;
            if ((entry & kPtSentinelBit) == 0u) {
              rhi::ReadbackBlocking(ctx.device, ctx.queue, world.voxels,
                                    (uint64_t)entry * kChunkVol * 4,
                                    words.data(), kChunkVol * 4, "digest");
              for (uint32_t w : words) { h ^= w; h *= 16777619u; }
            } else {
              // Positional, so a JITTER chunk digests as the page it would
              // materialize into — otherwise the digest would report a
              // paged/dense difference that does not exist in the world.
              const IVec3 wc = world.SlotToWorldChunk(s);
              const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
                        bz = wc.z * (int)kChunk;
              for (uint32_t i = 0; i < kChunkVol; i++) {
                const uint32_t sw = SynthWordAt(
                    entry, bx + (int)(i % kChunk),
                    by + (int)((i / kChunk) % kChunk),
                    bz + (int)(i / (kChunk * kChunk)), world.pages->WorldSeed());
                h ^= sw; h *= 16777619u;
              }
            }
            std::printf("[dig] %u %08X\n", s, h);
            if (const char* ws = getenv("SANDVOX_PT_WORDS")) {
              if (s == (uint32_t)atoi(ws) && (entry & kPtSentinelBit) == 0u)
                for (uint32_t i = 0; i < kChunkVol; i++)
                  if (words[i] != 0u)
                    std::printf("[wrd] %u %u %08X\n", s, i, words[i]);
            }
          }
        }
      }
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

  // READ THE REAL FAULT COUNTER. This used to be a never-assigned zero, so the
  // "=== page faults === 0" line was a hardcoded literal that never touched the
  // GPU — which is why a genuine lost write (the induction base case, one fault
  // at tick 8) went unseen for the whole phase while the counter it was
  // supposed to trip reported clean. The snapshot path cannot serve here: its
  // async map never retires in a headless harness, so this is a blocking read,
  // taken once after WaitIdle rather than per tick.
  {
    uint32_t pf[4] = {0u, 0u, 0u, 0u};
    rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, pf,
                          sizeof(pf), "pageFaultRead");
    out.pageFaults = pf[0];
  }

  if (vk::Backend* be = ctx.VkBackend()) {
    out.validationMsgCount = be->ValidationMessages().size();
    for (size_t i = 0; i < be->ValidationMessages().size() && i < 8; i++)
      out.validationMsgs.push_back(be->ValidationMessages()[i]);
  }
  out.ok = true;
  return true;
}

// ------------------------------------------------------------- reporting ----

// Compare the run's probe sequence against the pinned one. Every probe must
// match; a count mismatch is itself a failure (a scenario edit that changed
// which ticks are probed must update the pinned table in the same commit).
int CompareAndReport(const char* name, const RunResult& run, const Pinned* pinned,
                     size_t pinnedCount, bool validation) {
  std::printf("\n=== validation ===\n");
  if (!validation) {
    std::printf("  (off — rerun with --vk-validation)\n");
  } else if (run.validationMsgCount == 0) {
    std::printf("  ZERO messages (no synchronization hazards reported)\n");
  } else {
    std::printf("  *** %zu validation message(s) ***\n", run.validationMsgCount);
    for (const std::string& m : run.validationMsgs)
      std::printf("    %s\n", m.c_str());
  }

  // The run's sequence, worldgen first, in pinned order.
  std::vector<Probe> got;
  got.push_back({0, run.genHash});
  for (const Probe& p : run.probes) got.push_back(p);

  std::printf("\n=== hashes vs pinned ===\n");
  std::printf("  stage              pinned       measured\n");
  bool allMatch = true;
  uint32_t matches = 0, total = 0;
  const size_t n = got.size() < pinnedCount ? got.size() : pinnedCount;
  for (size_t i = 0; i < n; i++) {
    char label[32];
    if (pinned[i].tick == 0)
      std::snprintf(label, sizeof(label), "worldgen");
    else
      std::snprintf(label, sizeof(label), "tick %u", pinned[i].tick);
    const bool tickOk = got[i].tick == pinned[i].tick;
    const bool m = tickOk && got[i].hash == pinned[i].hash;
    allMatch = allMatch && m;
    total++;
    if (m) matches++;
    std::printf("  %-16s   %08x     %08x     %s\n", label, pinned[i].hash,
                got[i].hash, m ? "MATCH" : "*** MISMATCH ***");
    if (!tickOk)
      std::printf("      *** probe is tick %u, pinned expects tick %u ***\n",
                  got[i].tick, pinned[i].tick);
  }
  if (got.size() != pinnedCount) {
    std::printf("  *** probe count differs: measured %zu, pinned %zu ***\n",
                got.size(), pinnedCount);
    allMatch = false;
  }
  std::printf("  %u/%u MATCH\n", matches, total);

  const bool hazards = validation && run.validationMsgCount != 0;
  const bool pass = allMatch && !hazards;
  if (!pass && !allMatch)
    std::printf(
        "\n  A MISMATCH is a world-content regression unless you changed content on\n"
        "  purpose. The tick where it FIRST appears localises it (barrier_graph\n"
        "  §6.2): worldgen -> SPIR-V, zero-init or a binding; tick 1 -> recording\n"
        "  or the CA loop; the first hash tick (15) -> the occupancy path; drift\n"
        "  appearing later -> re-run with --barriers=sledgehammer, and read §6.2\n"
        "  on why a matching sledgehammer run is weak evidence.\n");
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

// Write the smoke result to a JSON file (build/last_run.json or a path the
// caller provides). Always written so an agent never re-runs to read data.
void WriteSmokeJson(const char* path, const char* scenario, const RunResult& run,
                    const Pinned* pinned, size_t pinnedCount, bool pass) {
  std::ofstream f(path);
  if (!f) return;
  f << "{\n  \"mode\": \"" << scenario << "\",\n";
  f << "  \"pass\": " << (pass ? "true" : "false") << ",\n";
  f << "  \"genHash\": \"" << std::setfill('0') << std::hex;
  f << std::setw(8) << run.genHash << "\",\n";
  f << "  \"pageFaults\": " << std::dec << run.pageFaults << ",\n";
  f << "  \"validationMessages\": " << run.validationMsgCount << ",\n";
  f << "  \"probes\": [\n";
  // worldgen + tick probes
  std::vector<Probe> got;
  got.push_back({0, run.genHash});
  for (const Probe& p : run.probes) got.push_back(p);
  for (size_t i = 0; i < got.size(); i++) {
    f << "    [" << std::dec << got[i].tick << ", \""
      << std::hex << std::setw(8) << std::setfill('0') << got[i].hash << "\"]"
      << (i + 1 < got.size() ? "," : "") << "\n";
  }
  f << "  ],\n";
  f << "  \"stats\": {"
    << "\"rows\": " << std::dec << run.stats.rows
    << ", \"dispatches\": " << run.stats.dispatches
    << ", \"copies\": " << run.stats.copies
    << ", \"fills\": " << run.stats.fills
    << ", \"barrierCalls\": " << run.stats.barrierCalls
    << ", \"bufferBarriers\": " << run.stats.bufferBarriers
    << ", \"globalBarriers\": " << run.stats.globalBarriers
    << "},\n";
  if (run.shifts > 0)
    f << "  \"shifts\": " << run.shifts << ",\n"
      << "  \"storeCount\": " << run.storeCount << ",\n";
  f << "  \"pinnedCount\": " << pinnedCount << "\n";
  f << "}\n";
  std::printf("wrote %s\n", path);
}

// Rebaseline: replace one probe table in baseline.json with the observed values.
bool RebaselineSmoke(const char* jsonKey, const RunResult& run) {
  std::string path = BaselinePath();
  std::ifstream fi(path);
  if (!fi) {
    std::fprintf(stderr, "rebaseline: cannot read %s\n", path.c_str());
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(fi)),
                   std::istreambuf_iterator<char>());
  fi.close();

  std::string needle = std::string("\"") + jsonKey + "\"";
  size_t keyPos = text.find(needle);
  if (keyPos == std::string::npos) {
    std::fprintf(stderr, "rebaseline: key '%s' not found in %s\n", jsonKey, path.c_str());
    return false;
  }
  size_t arrStart = text.find('[', keyPos);
  if (arrStart == std::string::npos) return false;
  // Find matching ]
  int depth = 1;
  size_t arrEnd = arrStart + 1;
  while (arrEnd < text.size() && depth > 0) {
    if (text[arrEnd] == '[') depth++;
    else if (text[arrEnd] == ']') depth--;
    arrEnd++;
  }

  // Build replacement array
  std::string newArr = "[\n";
  std::vector<Probe> got;
  got.push_back({0, run.genHash});
  for (const Probe& p : run.probes) got.push_back(p);
  for (size_t i = 0; i < got.size(); i++) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "    [%u, \"%08x\"]", got[i].tick, got[i].hash);
    newArr += buf;
    if (i + 1 < got.size()) newArr += ",";
    newArr += "\n";
  }
  newArr += "  ]";

  text = text.substr(0, arrStart) + newArr + text.substr(arrEnd);
  std::ofstream fo(path);
  if (!fo) {
    std::fprintf(stderr, "rebaseline: cannot write %s\n", path.c_str());
    return false;
  }
  fo << text;
  std::printf("\n*** REBASELINED %s in %s (%zu probes) ***\n", jsonKey, path.c_str(),
              got.size());
  return true;
}

int RunSmoke(bool loud, bool lowPower, bool sledgehammer, bool validation,
             bool paged, bool rebaseline) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* name = loud ? "--vk-smoke-loud" : "--vk-smoke";
  std::printf("=== sandvox %s (Vulkan, pinned hash sequence, residency %s) ===\n",
              name, paged ? "paged" : "dense");
  std::printf("mode: barriers=%s validation=%s adapter=%s seed=%u ticks=%u\n",
              sledgehammer ? "sledgehammer" : "precise", validation ? "ON" : "off",
              lowPower ? "low" : "default", kDefaultSeed,
              loud ? kLoudTicks : kQuietTicks);
  if (loud)
    std::printf(
        "scenario: brush+melt ops, 3 explosions (spawn/integrate/resolve), exact-cell\n"
        "          ops, readback ring active, 8-shift streaming walk (evict + procgen\n"
        "          refill; the store-hit round trip is covered by the save gates in\n"
        "          --selftest)\n");

  // Load pinned probe tables from JSON.
  const char* jsonKey = loud ? "smokeLoud" : "smokeQuiet";
  PinnedTable pt = LoadPinned(jsonKey,
      loud ? kLoudPinnedFallback : kQuietPinnedFallback,
      loud ? std::size(kLoudPinnedFallback) : std::size(kQuietPinnedFallback));

  std::vector<MaterialDef> mats;
  std::vector<ReactionGpu> reactions;
  std::string assetDir;
  if (!LoadSmokeAssets(mats, reactions, assetDir)) {
    std::printf("\n=== %s FAIL ===\n", name);
    return 1;
  }

  RunResult run;
  if (!RunScenario(loud, lowPower, sledgehammer, validation, paged, mats, reactions,
                   assetDir, run)) {
    std::printf("\n=== %s FAIL ===\n", name);
    return 1;
  }

  std::printf("\n  tick recording: %u rows, %u dispatches, %u copies, %u fills, "
              "%u barrier calls (%u buffer + %u global)\n",
              run.stats.rows, run.stats.dispatches, run.stats.copies, run.stats.fills,
              run.stats.barrierCalls, run.stats.bufferBarriers,
              run.stats.globalBarriers);
  if (loud)
    std::printf("\n=== streaming ===\n  %u window shifts, %zu chunks in store\n",
                run.shifts, run.storeCount);

  std::printf("\n=== page faults ===\n  %u%s\n", run.pageFaults,
              run.pageFaults == 0
                  ? "  (every sim write reached a materialized page)"
                  : "  *** A WRITE ESCAPED THE ADJACENCY ARGUMENT ***");

  int result = CompareAndReport(name, run, pt.data, pt.count, validation);

  // Always dump a machine-readable record.
  bool pass = (result == 0);
  WriteSmokeJson("build/last_run.json", name, run, pt.data, pt.count, pass);

  if (rebaseline) {
    if (run.validationMsgCount > 0) {
      std::printf("\n*** REFUSING to rebaseline: %zu validation message(s) ***\n",
                  run.validationMsgCount);
      return 1;
    }
    if (run.pageFaults > 0) {
      std::printf("\n*** REFUSING to rebaseline: %u page fault(s) ***\n",
                  run.pageFaults);
      return 1;
    }
    if (!RebaselineSmoke(jsonKey, run)) return 1;
    std::printf("*** THIS WAS A REBASELINE, NOT A PASS. ***\n");
    return 0;
  }

  return result;
}

}  // namespace

int RunVkSmoke(bool lowPower, bool sledgehammer, bool validation, bool paged,
               bool rebaseline) {
  return RunSmoke(/*loud=*/false, lowPower, sledgehammer, validation, paged,
                  rebaseline);
}

int RunVkSmokeLoud(bool lowPower, bool sledgehammer, bool validation, bool paged,
                   bool rebaseline) {
  return RunSmoke(/*loud=*/true, lowPower, sledgehammer, validation, paged,
                  rebaseline);
}

}  // namespace sandvox
