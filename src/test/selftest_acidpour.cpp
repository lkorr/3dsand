// selftest_acidpour.cpp -- `acid-pour`: a small pour of acid into a HELD
// river is a FINITE event (ticket 2026-10-17_00-04-26: "after pouring a lil
// bit of acid in water (mpm fluid) the shaders start tweakin out").
//
// Measured at the ticket's camera before the fix: an 80-tick radius-3 pour of
// MPM acid into the river woke 949 -> 1943 chunks between ticks 600 and 1200
// (0 in a no-pour control), 1506 of them SKY chunks of noxious_gas, and the
// river round the pour ran as live CA water with gas cells in it. CLAUDE.md
// rule 2: cost scales with activity and every emergent process is bounded.
//
// THE FIXTURE is held-splash's synthetic valley (a TEMP asset dir, a smooth
// 4 m river `main` on a GRAVEL bed, the bed every river of the default map
// has), regenerated and ticked to rest; then the fluid tool's own pour (the
// SANDVOX_AT_POUR shape: a radius-3 sphere, 8 particles a cell, -0.3 cells a
// tick) of acid a few voxels over the surface for acidPour.pourTicks ticks;
// then acidPour.ticks ticks, sampled every acidPour.sampleTicks.
//
// EVERY SAMPLE IS ATTRIBUTED (CLAUDE.md rule 6), not a bare awake count:
//   * acid: settled voxels and live MPM particles (the particle pool read
//     back and split by material), in eighths, against what was poured;
//   * noxious gas: cells, and how many awake chunks hold any, how many hold
//     ONLY a trace (1-3 cells) and how many of those are SKY chunks (centre
//     more than 24 over the river surface);
//   * the bed: solid + powder cells gone from a box round the site;
//   * the reservoir's TAKE / GROW / GIVE eighths (sim_step STILL_DIAG) and
//     the live (unheld) water cells, so a river that has let go shows up as
//     reservoir traffic rather than as "awake".
// Asserted: the acid did something (bed eaten, gas seen); the gas peak is at
// most acidPour.gasPeakMax cells and the awake peak acidPour.awakePeakMax;
// and at the end at most acidPour.maxAwake chunks are awake. Thresholds live
// in tests/baseline.json -- where the three gas / wake bounds are set OUT OF
// REACH for now (owner, 2026-10-18: chunks waking with the gas is acceptable
// for the moment; `_acidPour_about` says how to re-arm them). Leaves the pristine world behind (the `rivers`
// exit).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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

// held-splash's tile writer (selftest_heldsplash.cpp), copied: the two gates
// build the same valley and neither should depend on the other's file.
bool WriteTileA(const fs::path& p, uint32_t kind, int tx, int tz, const std::vector<int>& samples) {
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

}  // namespace

Status GateAcidPour(Ctx& c, std::string& detail) {
  using sandvox::ReloadEnvironment;
  using sandvox::SubmitWorldgen;
  const std::string dir = sandvox::AssetDir();
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("acid-pour: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return static_cast<uint32_t>(i);
    return 0u;
  };
  const uint32_t mWater = matId("water"), mAcid = matId("acid"), mNox = matId("noxious_gas");
  if (mWater == 0 || mAcid == 0 || mNox == 0) return fail("missing one of water / acid / noxious_gas");

  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(dir, c.mats, set, log)) return fail("biome files did not load: " + log);
  TreeAtlas atlas;
  if (!LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log)) return fail("tree atlas did not load: " + log);
  const worldmap::WorldMapData real = worldmap::CurrentWorldMap();   // a COPY
  if (!real.Loaded()) return fail("no world map loaded");

  // ---- the synthetic map: held-splash's valley, `main` only ----
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_acidpour_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
      {"seaLevelY", ylo + 40}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"ocean", "meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "pad"}, {"kind", "pad"}, {"min", {ox - 256, oz - 256}}, {"max", {ox + 768, oz + 768}}},
                 {{"id", "spawn"}, {"kind", "spawn"}, {"at", {ox + 480, oz + 60}}}}},
      {"rules", nlohmann::json::array()}};
  bool wrote = WriteTileA(mdir / "planes" / "height" / "0_0.svt", worldmap::kPlaneHeight, 0, 0, hs) &&
               WriteTileA(mdir / "planes" / "biome" / "0_0.svt", worldmap::kPlaneBiome, 0, 0, bs);
  nlohmann::json rivers = nlohmann::json::array();
  rivers.push_back({{"id", "main"}, {"smooth", true}, {"bed", "gravel"}, {"stepVox", 4},
                    {"points", {{ox + 12, oz + 262, 36, 8}, {ox + 170, oz + 236}, {ox + 330, oz + 282}, {ox + 500, oz + 256, 44, 10}}}});
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
  for (const auto& r : on.rivers)
    if (r.id == "main") rMain = &r;
  if (!rMain || rMain->line.size() < 8) {
    cleanup();
    return fail("the synthetic map has no `main` line");
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

  const auto& M = rMain->line[rMain->line.size() / 3];
  const int mx = M[0], mz = M[1], my = M[2];

  const int pourTicks = static_cast<int>(BaselineNumber("acidPour.pourTicks", 80));
  const int pourR = static_cast<int>(BaselineNumber("acidPour.pourRadius", 3));
  const int ticks = static_cast<int>(BaselineNumber("acidPour.ticks", 3000));
  const int step = std::max(1, static_cast<int>(BaselineNumber("acidPour.sampleTicks", 100)));
  const uint32_t maxAwake = static_cast<uint32_t>(BaselineNumber("acidPour.maxAwake", 32));
  const uint32_t awakePeakMax = static_cast<uint32_t>(BaselineNumber("acidPour.awakePeakMax", 400));
  const uint32_t gasPeakMax = static_cast<uint32_t>(BaselineNumber("acidPour.gasPeakMax", 2000));
  const int restTicks = static_cast<int>(BaselineNumber("acidPour.restTicks", 3000));
  uint32_t t = 96000;
  const IVec3 fixture{mx >> 4, my >> 4, mz >> 4};

  // ---- fresh world, ticked to rest (held-splash's regen) ----
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  sandvox::support::TickCursor tick{c, t, fixture};
  int rested = -1;
  for (int k = 1; k <= restTicks; k++) {
    tick();
    if (k >= 200 && k % 100 == 0 && sandvox::ReadActiveChunksSync(c.ctx, c.world, c.sim) <= maxAwake) {
      rested = k;
      break;
    }
  }

  // ---- the census ----
  // The box round the site the bed is counted in (the pour lands at its
  // centre; the river is ~4 m = 40 voxels wide).
  const int bx0 = mx - 40, bx1 = mx + 40, bz0 = mz - 40, bz1 = mz + 40, by0 = my - 24, by1 = my + 6;
  struct Census {
    uint32_t awake = 0;
    uint64_t acidE = 0;            // settled acid, eighths
    uint64_t acidP = 0;            // live acid particles (1 eighth each... their fullness)
    uint64_t waterP = 0;           // live water particles
    uint32_t gas = 0;              // noxious gas cells (awake chunks + the box)
    uint32_t gasChunks = 0, traceChunks = 0, skyTrace = 0, skyGas = 0;
    uint32_t bed = 0;              // solid + powder cells in the box
    uint32_t water = 0;            // water cells in the box
    uint32_t give = 0, take = 0, grow = 0;
    uint32_t live = 0;
    uint32_t fa[kFluidArgsWords] = {};
    // WHERE the live acid particles are: the material of the cell each one
    // is in ([0] air, [1] water, [2] acid, [3] other liquid, [4] solid or
    // powder, [5] unread) and of the cell under it; and its height over the
    // river surface (<= -2, -1..1, >= 2).
    uint64_t pIn[6] = {}, pUnder[6] = {}, pDy[3] = {};
  };
  std::vector<uint32_t> buf(kChunkVol);
  auto census = [&]() {
    c.ctx.WaitIdle();
    Census k;
    std::vector<uint32_t> flags(kNumSlots, 0);
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.DirtyActive(), 0, flags.data(), kNumSlots * 4,
                          "apActive");
    std::set<uint32_t> slots;
    for (uint32_t i = 0; i < kNumSlots; i++)
      if (flags[i] != 0) {
        k.awake++;
        if (i < kNumChunks) slots.insert(i);   // a ticket slot is a peer's
      }
    std::set<uint32_t> box;
    for (int cz = bz0 >> 4; cz <= (bz1 >> 4); cz++)
      for (int cy = by0 >> 4; cy <= (by1 >> 4); cy++)
        for (int cx = bx0 >> 4; cx <= (bx1 >> 4); cx++) {
          const uint32_t s = World::SlotChunkIndex(IVec3{cx, cy, cz});
          box.insert(s);
          slots.insert(s);
        }
    std::map<uint32_t, std::vector<uint32_t>> read;
    for (uint32_t s : slots) {
      sandvox::ReadVoxelsSync(c.ctx, c.world, s, 1, buf.data(), "apCensus");
      read[s] = buf;
      const IVec3 wc = c.world.SlotToWorldChunk(s);
      const bool inBox = box.count(s) != 0;
      uint32_t g = 0;
      for (uint32_t i = 0; i < kChunkVol; i++) {
        const uint32_t w = buf[i], m = w & 0xFFFu;
        if (m == 0u) continue;
        if (m == mNox) { g++; continue; }
        if (m == mAcid) { k.acidE += ((w >> 12) & 15u) + 1u; continue; }
        if (!inBox) continue;
        const int x = wc.x * 16 + int(i % 16), y = wc.y * 16 + int((i / 16) % 16), z = wc.z * 16 + int(i / 256);
        if (x < bx0 || x > bx1 || y < by0 || y > by1 || z < bz0 || z > bz1) continue;
        if (m == mWater) { k.water++; continue; }
        if (m < c.mats.size() && (c.mats[m].gpu.klass == CLASS_SOLID || c.mats[m].gpu.klass == CLASS_POWDER)) k.bed++;
      }
      k.gas += g;
      if (g != 0 && flags[s] != 0) {
        k.gasChunks++;
        const bool sky = wc.y * 16 + 8 > my + 24;
        if (sky) k.skyGas++;
        if (g <= 3) { k.traceChunks++; if (sky) k.skyTrace++; }
      }
    }
    uint32_t* fa = k.fa;
    sandvox::ReadFluidArgsSync(c.ctx, c.world, fa);
    k.live = fa[7];
    auto classOf = [&](int x, int y, int z) -> int {
      const uint32_t s = World::SlotChunkIndex(IVec3{x >> 4, y >> 4, z >> 4});
      auto it = read.find(s);
      if (it == read.end()) {
        if (read.size() > 4096) return 5;
        std::vector<uint32_t> v(kChunkVol);
        sandvox::ReadVoxelsSync(c.ctx, c.world, s, 1, v.data(), "apPartCell");
        it = read.emplace(s, std::move(v)).first;
      }
      const uint32_t m = it->second[(uint32_t(z & 15) * kChunk + uint32_t(y & 15)) * kChunk + uint32_t(x & 15)] & 0xFFFu;
      if (m == 0u) return 0;
      if (m == mWater) return 1;
      if (m == mAcid) return 2;
      if (m >= c.mats.size()) return 5;
      if (c.mats[m].gpu.klass == CLASS_LIQUID) return 3;
      if (c.mats[m].gpu.klass == CLASS_SOLID || c.mats[m].gpu.klass == CLASS_POWDER) return 4;
      return 0;
    };
    if (k.live != 0) {
      const uint32_t n = std::min<uint32_t>(k.live, kFluidCap);
      std::vector<uint32_t> parts(size_t(n) * kFluidParticleWords);
      rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.world.fluidParticles[c.sim.Page()], 0, parts.data(),
                            parts.size() * 4, "apParts");
      for (uint32_t i = 0; i < n; i++) {
        const uint32_t a = parts[size_t(i) * kFluidParticleWords + 18];
        const uint32_t m = a & 0xFFFu, f = (a >> 12) & 7u;
        if (m == 0u || f == 0u) continue;
        if (m == mAcid) {
          k.acidP += f;
          const int32_t* q = reinterpret_cast<const int32_t*>(&parts[size_t(i) * kFluidParticleWords]);
          const int x = q[0] >> 16, y = q[1] >> 16, z = q[2] >> 16;
          k.pIn[classOf(x, y, z)] += f;
          k.pUnder[classOf(x, y - 1, z)] += f;
          k.pDy[y - my <= -2 ? 0 : y - my >= 2 ? 2 : 1] += f;
        }
        else if (m == mWater) k.waterP += f;
      }
    }
    uint32_t d[3] = {};
    const uint64_t off = (uint64_t(kWorldN) * kWorldN * 4u + 4u + 11u * uint64_t(kWorldN) * kWorldN) * 4u;
    rhi::ReadbackBlocking(c.ctx.device, c.ctx.queue, c.sim.StillWaterBuffer(), off, d, sizeof d, "apStillDiag");
    k.give = d[0];
    k.take = d[1];
    k.grow = d[2];
    return k;
  };

  // ---- the pour: the fluid tool's sphere, SANDVOX_AT_POUR's shape ----
  const int px = mx, py = my + 4, pz = mz;
  uint64_t poured = 0;
  auto pourOps = [&](uint32_t tk) {
    sandvox::support::TickOps ops;
    for (int z = -pourR; z <= pourR; z++)
      for (int y = -pourR; y <= pourR; y++)
        for (int x = -pourR; x <= pourR; x++) {
          if (x * x + y * y + z * z > pourR * pourR) continue;
          for (int s = 0; s < 8; s++) {
            const uint32_t h = (tk * 9781u + static_cast<uint32_t>(ops.fluid.size()) * 6271u) * 747796405u + 2891336453u;
            FluidSpawnOp op{};
            op.px = ((px + x) << 16) + ((s & 1) ? 49152 : 16384) + static_cast<int32_t>(h % 8192u) - 4096;
            op.py = ((py + y) << 16) + ((s & 2) ? 49152 : 16384) + static_cast<int32_t>((h >> 13) % 8192u) - 4096;
            op.pz = ((pz + z) << 16) + ((s & 4) ? 49152 : 16384) + static_cast<int32_t>((h >> 19) % 8192u) - 4096;
            op.vy = -19661;
            op.mat = mAcid;
            ops.fluid.push_back(op);
          }
        }
    return ops;
  };

  const Census k0 = census();
  std::string curve;
  Census prev = k0, last = k0;
  uint32_t awakePeak = 0, gasPeak = 0, settledAt = 0;
  for (int done = 0; done < ticks;) {
    const int n = std::min(step, ticks - done);
    for (int i = 0; i < n; i++) {
      if (done + i < pourTicks) {
        const auto ops = pourOps(t + 1);
        poured += ops.fluid.size();
        tick(ops);
      } else {
        tick();
      }
    }
    done += n;
    const Census k = census();
    awakePeak = std::max(awakePeak, k.awake);
    gasPeak = std::max(gasPeak, k.gas);
    if (done > pourTicks && k.awake <= maxAwake && settledAt == 0) settledAt = static_cast<uint32_t>(done);
    if (k.awake > maxAwake) settledAt = 0;
    curve += Format("\n    [%4d] %4u awake | acid %llu settled + %llu live eighths (water %llu live) | gas %u cells in %u "
                    "awake chunks (%u trace <= 3; sky %u, sky trace %u) | bed %d | box water %u | reservoir take %u grow %u "
                    "give %u\n           settle (last tick): %u blocks, %u refused (%u unstable), columns ceil %u floor %u, "
                    "forced %u sealed %u | live acid in air %llu water %llu acid %llu liquid %llu solid %llu ? %llu, over "
                    "air %llu water %llu acid %llu liquid %llu solid %llu ? %llu, dy<=-2 %llu, -1..1 %llu, >=2 %llu",
                    done, k.awake, (unsigned long long)k.acidE, (unsigned long long)k.acidP,
                    (unsigned long long)k.waterP, k.gas, k.gasChunks, k.traceChunks, k.skyGas, k.skyTrace,
                    static_cast<int>(k.bed) - static_cast<int>(k0.bed), k.water, k.take - prev.take,
                    k.grow - prev.grow, k.give - prev.give, k.fa[13], k.fa[25], k.fa[26], k.fa[30], k.fa[31], k.fa[32],
                    k.fa[33], (unsigned long long)k.pIn[0], (unsigned long long)k.pIn[1], (unsigned long long)k.pIn[2],
                    (unsigned long long)k.pIn[3], (unsigned long long)k.pIn[4], (unsigned long long)k.pIn[5],
                    (unsigned long long)k.pUnder[0], (unsigned long long)k.pUnder[1], (unsigned long long)k.pUnder[2],
                    (unsigned long long)k.pUnder[3], (unsigned long long)k.pUnder[4], (unsigned long long)k.pUnder[5],
                    (unsigned long long)k.pDy[0], (unsigned long long)k.pDy[1], (unsigned long long)k.pDy[2]);
    std::printf("acid-pour: sample%s\n", curve.substr(curve.rfind("\n    [")).c_str());
    std::fflush(stdout);
    prev = k;
    last = k;
  }

  const int eaten = static_cast<int>(k0.bed) - static_cast<int>(last.bed);
  const bool acted = eaten > 0 && gasPeak > 0;
  const bool ok = acted && gasPeak <= gasPeakMax && awakePeak <= awakePeakMax && last.awake <= maxAwake;
  std::printf("acid-pour: %s (river main at (%d, %d) surface y%d, rested in %d ticks; poured %llu particles "
              "(%llu cells) over %d ticks; awake peak %u (<= %u), %u at the end (<= %u), at or under %u from tick %u; "
              "gas peak %u cells (<= %u); bed eaten %d cells; acid left %llu settled + %llu live eighths)%s\n",
              ok ? "PASS" : "FAIL", mx, mz, my, rested, (unsigned long long)poured, (unsigned long long)(poured / 8),
              pourTicks, awakePeak, awakePeakMax, last.awake, maxAwake, maxAwake, settledAt, gasPeak, gasPeakMax,
              eaten, (unsigned long long)last.acidE, (unsigned long long)last.acidP, curve.c_str());
  RecordObserved("acidPour.observedAwakePeak", static_cast<double>(awakePeak));
  RecordObserved("acidPour.observedGasPeak", static_cast<double>(gasPeak));
  RecordObserved("acidPour.observedEndAwake", static_cast<double>(last.awake));
  RecordObserved("acidPour.observedSettledAt", static_cast<double>(settledAt));
  RecordObserved("acidPour.observedBedEaten", static_cast<double>(eaten));

  // ---- the pristine world back ----
  std::string problem;
  biomes::EnvironmentStamp stamp;
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    worldmap::SetCurrentWorldMap(real);
    problem = "ReloadEnvironment REFUSED afterwards: " + log;
  }
  cleanup();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  DrainFullRefill(c.ctx, c.world, c.sim, IVec3{108 >> 4, 122 >> 4, 108 >> 4});
  if (!problem.empty()) return fail(problem);
  if (!ok) {
    detail = Format("awake peak %u (<= %u), end %u (<= %u), gas peak %u (<= %u), bed eaten %d", awakePeak,
                    awakePeakMax, last.awake, maxAwake, gasPeak, gasPeakMax, eaten);
    return Status::Fail;
  }
  detail = Format("a %d-tick acid pour into a held river: awake peak %u, back to %u awake by tick %u, gas peak %u",
                  pourTicks, awakePeak, last.awake, settledAt, gasPeak);
  return Status::Pass;
}

const std::vector<Gate>& AcidPourGates() {
  static const std::vector<Gate> g = {
      {"acid-pour", "sim", {}, false, GateAcidPour},
  };
  return g;
}

}  // namespace selftest
