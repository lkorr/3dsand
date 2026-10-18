// selftest_heldsplash.cpp -- `held-splash`: disturbed HELD water (a map river)
// goes through the MPM seam and comes back ONCE, and then rests; it does not
// cycle between running water and particles (bug report 2026-10-17_11-21-36:
// "running water sometimes constantly turns into MPM fluid and explodes a bit,
// then turns back into running water and resets in a loop").
//
// ONE synthetic map in a TEMP asset dir (the `rivers` seam and exit, and its
// valley): `main`, a smooth 4 m river (ramped in eighths), and `falls`, a
// tributary with waterfalls (held crest + curtain) joining it. Every arm starts
// on a fresh worldgen ticked to rest, makes ONE disturbance, and ticks
// heldSplash.ticks, sampling every heldSplash.sampleTicks:
//   A. SPLASH on main: a block of water dropped from over the surface.
//   B. SPLASH over falls' first crest.
//   C. BLAST at main's surface.
// Each sample prints the window's excited eighths BY TRIGGER (sim_fluid_seam
// EX_BY_*), the eighths settled, the live particles, the reservoir's TAKE /
// GROW / GIVE eighths (sim_step STILL_DIAG [0..2]), the eighths held water
// ABSORBED back out of the seam (EX_ABSORBED), the excite candidates' census
// by WHICH water they were (STILL_DIAG [3..7]: a held column over its top
// cell, a bank beside one, other unheld water, an adopted stream's column, a
// held cell at a breach), and the awake chunks -- so a failure
// names which trigger keeps taking the water and what keeps putting it back.
// Asserted per arm: the disturbance did excite (> 0 eighths in the first window), the LATE
// windows (the last heldSplash.lateTicks) excited at most
// heldSplash.lateExciteMax eighths, nothing is live at the end
// (<= heldSplash.liveMax) and at most heldSplash.maxAwake chunks are awake.
// Measured before the fix (held water excited and refilled INSIDE its own
// particles; see sim_fluid_seam.wgsl HELD WATER): every arm pinned at
// the 8,000-particle excite ceiling, ~5-12k eighths excited and settled per
// 100 ticks to the end, 54 / 11 / 56 chunks awake.
// Leaves the pristine world behind (the `rivers` exit).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/treeatlas.h"
#include "sim/tuning.h"
#include "sim/weather.h"
#include "sim/world.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"

namespace selftest {
namespace {

namespace fs = std::filesystem;
using sandvox::kDefaultSeed;

bool WriteTileH(const fs::path& p, uint32_t kind, int tx, int tz, const std::vector<int>& samples) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  if (!f) return false;
  const uint32_t sb = kind == worldmap::kPlaneHeight ? 2u : 1u;
  const uint32_t hdr[8] = {worldmap::kTileMagic, kind, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz),
                           static_cast<uint32_t>(worldmap::kTileSide), static_cast<uint32_t>(worldmap::kTileSide), sb, 0u};
  f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
  for (int v : samples) {
    if (sb == 2) { const int16_t s = static_cast<int16_t>(v); f.write(reinterpret_cast<const char*>(&s), 2); }
    else { const uint8_t s = static_cast<uint8_t>(v); f.write(reinterpret_cast<const char*>(&s), 1); }
  }
  return static_cast<bool>(f);
}

// Every counter the gate reads, at one instant. All cumulative except live.
struct Counters {
  uint32_t live = 0;           // FA_LIVE
  uint32_t excited = 0;        // FA_EXCITEDCUM
  uint32_t settled = 0;        // FA_SETWROTE
  uint32_t by[5] = {};         // EX_BY_FALL / PERCH / STEP / SHELL / WAKE
  uint32_t give = 0, take = 0, grow = 0;        // STILL_DIAG [0..2]
  uint32_t absorbed = 0;       // EX_ABSORBED: eighths held water took back
  uint32_t census[5] = {};     // STILL_DIAG [3..7]: held over its top / bank / unheld / stream / held at a breach
  uint32_t refused[5] = {};    // STILL_DIAG [8..12]: held water at rest a trigger fired on, refused (by trigger)
};

Counters ReadCounters(Ctx& c) {
  c.ctx.WaitIdle();
  Counters k;
  uint32_t fa[kFluidArgsWords] = {};
  sandvox::ReadFluidArgsSync(c.ctx, c.world, fa);
  k.live = fa[7];
  k.excited = fa[36];
  k.settled = fa[39];
  uint32_t ex[9] = {};   // exciteScratch [7..15]: EX_ABSORBED, the settle args, EX_BY_*
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.fluidExciteScratch, 7u * 4u, ex, sizeof ex, "hsExBy");
  k.absorbed = ex[0];
  for (int i = 0; i < 5; i++) k.by[i] = ex[4 + i];
  uint32_t d[13] = {};
  const uint64_t off = (uint64_t(kWorldN) * kWorldN * 4u + 4u + uint64_t(kWorldN) * kWorldN) * 4u;
  rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), off, d, sizeof d, "hsStillDiag");
  k.give = d[0];
  k.take = d[1];
  k.grow = d[2];
  for (int i = 0; i < 5; i++) k.census[i] = d[3 + i];
  for (int i = 0; i < 5; i++) k.refused[i] = d[8 + i];
  return k;
}

}  // namespace

Status GateHeldSplash(Ctx& c, std::string& detail) {
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("held-splash: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };

  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");
  uint32_t mWater = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "water") mWater = static_cast<uint32_t>(i);
  if (mWater == 0) return fail("no `water` material");

  // ---- the synthetic map: the `rivers` valley, main + falls only ----
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_heldsplash_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "syn";
  const IVec3 org = c.world.WindowOrigin();
  const int ox = org.x * static_cast<int>(kChunk), oz = org.z * static_cast<int>(kChunk);
  const int ylo = org.y * static_cast<int>(kChunk);
  constexpr int L = 6, P = 1 << L, T = worldmap::kTileSide;
  const int X0 = ((ox - 8192) >> 10) << 10, Z0 = ((oz - 8192) >> 10) << 10;
  const int G = ylo + 100;
  auto authored = [&](int i, int k) -> int {
    const double px = X0 + i * P, pz = Z0 + k * P;
    const double u = std::clamp((px - ox) / 512.0, -0.5, 1.5);
    const double v = std::clamp((pz - oz - 256) / 256.0, -1.5, 1.5);
    return static_cast<int>(std::lround(G + 60 - 70 * u + 40 * std::fabs(v)));
  };
  std::vector<int> hs(size_t(T) * T), bs(size_t(T) * T, 1);
  for (int k = 0; k < T; k++)
    for (int i = 0; i < T; i++) hs[size_t(k) * T + i] = authored(i, k);
  nlohmann::json te = nlohmann::json::object();
  {
    std::ifstream f(dir + "/worldmap/" + real.name + "/map.json");
    nlohmann::json rj;
    try { f >> rj; } catch (...) {}
    if (rj.contains("terrain")) te = rj["terrain"];
  }
  nlohmann::json mj = {
      {"version", 2}, {"name", "syn"},
      {"extent", {{"min", {X0, Z0}}, {"size", {32768, 32768}}}},
      {"siteCellLog2", 10},
      {"planes", {{"height", {{"log2", L}, {"tile", T}, {"default", -300}}},
                  {"biome", {{"log2", 8}, {"tile", T}, {"default", "ocean"}}}}},
      // The `rivers` gate's sea, for its reason (the lapse rate; ice at dusk).
      {"seaLevelY", ylo + 40}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "pad"}, {"kind", "pad"}, {"min", {ox - 256, oz - 256}}, {"max", {ox + 768, oz + 768}}},
                 {{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 480, oz + 60}}}}},
      {"rules", nlohmann::json::array()}};
  bool wrote = WriteTileH(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTileH(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  // main's centreline, tessellated, so the tributary's mouth lands ON it.
  using KP = std::vector<std::array<double, 2>>;
  const KP mainKnots = {{double(ox + 12), double(oz + 262)}, {double(ox + 170), double(oz + 236)},
                        {double(ox + 330), double(oz + 282)}, {double(ox + 500), double(oz + 256)}};
  const auto mainTess = worldmap::SplineTessellate(mainKnots, false, 16.0);
  std::array<int, 2> joinAt{ox + 262, oz + 256};
  {
    double best = 1e30;
    for (const auto& q : mainTess)
      if (std::fabs(q.x - (ox + 262)) < best) {
        best = std::fabs(q.x - (ox + 262));
        joinAt = {int(std::lround(q.x)), int(std::lround(q.z))};
      }
  }
  nlohmann::json rivers = nlohmann::json::array();
  rivers.push_back({{"id", "main"}, {"smooth", true}, {"bed", "gravel"}, {"stepVox", 4},
                    {"points", {{ox + 12, oz + 262, 36, 8}, {ox + 170, oz + 236}, {ox + 330, oz + 282}, {ox + 500, oz + 256, 44, 10}}}});
  rivers.push_back({{"id", "falls"}, {"smooth", false}, {"bed", "gravel"}, {"stepVox", 24}, {"joins", "main"},
                    {"points", {{joinAt[0] + 8, oz + 16, 20, 6}, {joinAt[0] + 14, oz + 120}, {joinAt[0], joinAt[1], 20, 6}}}});
  mj["rivers"] = rivers;
  {
    std::ofstream f(mdir / "map.json");
    f << mj.dump(2);
    wrote = wrote && static_cast<bool>(f);
  }
  if (!wrote) return fail("could not write the synthetic map under " + tmp.string());
  const std::vector<worldmap::StructurePlacement>* prevOverride = worldmap::StructureOverrideSlot();
  const std::vector<worldmap::StructurePlacement> prevList =
      prevOverride ? *prevOverride : std::vector<worldmap::StructurePlacement>{};
  static const std::vector<worldmap::StructurePlacement> kNone;
  worldmap::SetStructureOverride(&kNone);
  const std::string tdir = tmp.string();
  const Tuning savedTuning = CurrentTuning();
  const std::string prevPin = weather::Override();
  auto cleanup = [&] {
    worldmap::SetStructureOverride(prevOverride ? &prevList : nullptr);
    SetCurrentTuning(savedTuning);
    weather::SetOverride(prevPin);
    std::error_code ec;
    fs::remove_all(tmp, ec);
  };
  worldmap::WorldMapData on;
  {
    const std::string text = mj.dump();
    std::string l;
    if (!worldmap::LoadWorldMap(tdir, "syn", set, c.mats.size(), kDefaultSeed, on, l, &text)) {
      cleanup();
      return fail("the synthetic river map was REFUSED: " + l);
    }
  }
  const worldmap::WorldMapData::River* rMain = nullptr;
  const worldmap::WorldMapData::River* rFalls = nullptr;
  for (const auto& r : on.rivers) {
    if (r.id == "main") rMain = &r;
    if (r.id == "falls") rFalls = &r;
  }
  if (!rMain || !rFalls || rMain->line.size() < 8 || rFalls->falls.empty()) {
    cleanup();
    return fail("the synthetic map has no `main` line or no waterfall on `falls`");
  }
  worldmap::SetCurrentWorldMap(on);
  std::vector<uint32_t> words;
  if (!worldmap::PackWorldMap(set, on, words, log)) {
    worldmap::SetCurrentWorldMap(real);
    cleanup();
    return fail("the synthetic map did not pack: " + log);
  }
  c.ctx.WaitIdle();
  c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
  weather::SetOverride("clear");

  // The disturbance sites. main: a sample a third of the way down (clear of
  // the junction); falls: the upper reach's last sample before its first fall.
  const auto& M = rMain->line[rMain->line.size() / 3];
  const int mx = M[0], mz = M[1], my = M[2];
  size_t iF = 0;
  for (size_t i = 1; i < rFalls->line.size(); i++)
    if (rFalls->line[i][0] == rFalls->falls[0][0] && rFalls->line[i][1] == rFalls->falls[0][1]) { iF = i; break; }
  if (iF == 0) {
    worldmap::SetCurrentWorldMap(real);
    cleanup();
    return fail("falls' first waterfall is not on its baked line");
  }
  const auto& F = rFalls->line[iF - 1];
  const int fx = F[0], fz = F[1], fy = F[2];

  const int ticks = static_cast<int>(BaselineNumber("heldSplash.ticks", 1500));
  const int step = std::max(1, static_cast<int>(BaselineNumber("heldSplash.sampleTicks", 100)));
  const int lateTicks = static_cast<int>(BaselineNumber("heldSplash.lateTicks", 500));
  const uint32_t lateMax = static_cast<uint32_t>(BaselineNumber("heldSplash.lateExciteMax", 0));
  const uint32_t liveMax = static_cast<uint32_t>(BaselineNumber("heldSplash.liveMax", 0));
  const uint32_t maxAwake = static_cast<uint32_t>(BaselineNumber("heldSplash.maxAwake", 32));
  const int restTicks = static_cast<int>(BaselineNumber("heldSplash.restTicks", 3000));
  uint32_t t = 94000;

  // Fresh world, ticked until <= maxAwake chunks are awake (the `rivers` G
  // claim). Returns the ticks it took, or -1 if it never slept.
  auto regen = [&](IVec3 fixture) -> int {
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    sandvox::support::TickCursor tick{c, t, fixture};
    for (int k = 1; k <= restTicks; k++) {
      tick();
      if (k >= 200 && k % 100 == 0 && sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim) <= maxAwake) return k;
    }
    return -1;
  };
  auto waterBlock = [&](int cx, int y0, int cz, int r, int h) {
    sandvox::support::TickOps ops;
    const uint32_t full = mWater | (7u << 12);
    for (int y = y0; y < y0 + h; y++)
      for (int z = cz - r; z <= cz + r; z++)
        for (int x = cx - r; x <= cx + r; x++) ops.cells.push_back({World::SlotCellIndex({x, y, z}), full});
    return ops;
  };
  auto arm = [&](const char* tag, const char* what, IVec3 fixture, const sandvox::support::TickOps& ops) {
    const int rested = regen(fixture);
    sandvox::support::TickCursor tick{c, t, fixture};
    const Counters k0 = ReadCounters(c);
    Counters prev = k0;
    tick(ops);
    std::string curve;
    uint32_t firstExcited = 0, lateExcited = 0, awake = 0;
    for (int done = 1; done < ticks;) {
      const int n = std::min(step, ticks - done);
      for (int i = 0; i < n; i++) tick();
      done += n;
      const Counters k = ReadCounters(c);
      awake = sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim);
      const uint32_t ex = k.excited - prev.excited;
      if (done <= step + 1) firstExcited = k.excited - k0.excited;
      if (done > ticks - lateTicks) lateExcited += ex;
      curve += Format("\n    [%4d] live %5u | excited %5u (fall %u perch %u step %u shell %u wake %u) settled %5u | "
                      "reservoir take %u grow %u give %u | absorbed %u | candidates: held over its top %u, bank %u, unheld %u, stream %u, held at a breach %u | refused at rest: step %u wake %u | %u awake",
                      done, k.live, ex, k.by[0] - prev.by[0], k.by[1] - prev.by[1], k.by[2] - prev.by[2],
                      k.by[3] - prev.by[3], k.by[4] - prev.by[4], k.settled - prev.settled, k.take - prev.take,
                      k.grow - prev.grow, k.give - prev.give, k.absorbed - prev.absorbed, k.census[0] - prev.census[0],
                      k.census[1] - prev.census[1], k.census[2] - prev.census[2], k.census[3] - prev.census[3],
                      k.census[4] - prev.census[4], k.refused[2] - prev.refused[2], k.refused[4] - prev.refused[4], awake);
      prev = k;
    }
    const uint32_t absorbed = prev.absorbed - k0.absorbed;
    const uint32_t total = prev.excited - k0.excited;
    const bool ok = firstExcited > 0 && lateExcited <= lateMax && prev.live <= liveMax && awake <= maxAwake;
    std::printf("held-splash: %s %s: %s (rested in %d ticks; excited %u eighths in all, %u in the first %d ticks, %u in "
                "the last %d (<= %u); %u live at the end (<= %u); %u awake (<= %u); held water absorbed %u eighths of "
                "its own particles)%s\n",
                tag, what, ok ? "PASS" : "FAIL", rested, total, firstExcited, step + 1, lateExcited, lateTicks, lateMax,
                prev.live, liveMax, awake, maxAwake, absorbed, curve.c_str());
    RecordObserved(Format("heldSplash.observed%sLateExcited", tag).c_str(), static_cast<double>(lateExcited));
    RecordObserved(Format("heldSplash.observed%sAbsorbed", tag).c_str(), static_cast<double>(absorbed));
    if (!ok)
      problems.push_back(Format("%s %s: first %u, late %u, live %u, awake %u", tag, what, firstExcited, lateExcited,
                                prev.live, awake));
  };

  std::printf("held-splash: fixture: main at (%d, %d) surface y%d; falls' first crest at (%d, %d) y%d\n", mx, mz, my, fx,
              fz, fy);
  arm("A", "splash on main (a 5x5x4 block dropped from 10 over the surface)", IVec3{mx >> 4, my >> 4, mz >> 4},
      waterBlock(mx, my + 10, mz, 2, 4));
  arm("B", "splash over falls' first crest", IVec3{fx >> 4, fy >> 4, fz >> 4}, waterBlock(fx, fy + 10, fz, 2, 4));
  {
    sandvox::support::TickOps ops;
    ops.exps.push_back(ExplosionOp{mx, my, mz, 5, 256, 0, 0, 0});
    arm("C", "blast at main's surface (radius 5)", IVec3{mx >> 4, my >> 4, mz >> 4}, ops);
  }

  // ---- the pristine world back ----
  biomes::EnvironmentStamp stamp;
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    worldmap::SetCurrentWorldMap(real);
    problems.push_back("ReloadEnvironment REFUSED afterwards: " + log);
  }
  cleanup();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("held-splash: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = "splashes and a blast on held water excite once, settle, and rest";
  std::printf("held-splash: PASS\n");
  return Status::Pass;
}

const std::vector<Gate>& HeldSplashGates() {
  static const std::vector<Gate> g = {
      {"held-splash", "sim", {}, false, GateHeldSplash},
  };
  return g;
}

}  // namespace selftest
