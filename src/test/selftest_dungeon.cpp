// selftest_dungeon.cpp -- the dungeon layer (docs/PLAN_dungeons.md §6; D1).
//
//   dungeon-format  CPU only. Every assets/dungeons/<id>/ loads: shell.bin is
//                   there, reads back, says the same as its shell.json twin
//                   (CONTENT, dungeon::SameContent), is CURRENT with its
//                   dungeon.json (the sourceHash), resolves its materials,
//                   fits the index limits (a trial pack), and round-trips
//                   through the engine's own writers. Every map's dungeons[]
//                   names an asset that exists. SVED v3 (dungeon-local detail)
//                   writes, loads and resolves through the loaded map's
//                   placement, rigidly; an unplaced dungeon's cells apply
//                   nowhere.
//   dungeon-gen     GPU. The harness map's `fixture` (assets/dungeons/_fixture,
//                   placed with a quarter turn) through the real genChunk:
//                     A. TWIN: chunks across the whole layout generated twice
//                        -- the evaluator OFF (the terrain the dungeon is
//                        applied to, caves already suppressed) and ON -- and
//                        every cell of the ON arm equals the CPU twin
//                        (dungeon::TwinCell) of the OFF arm's word. Exact.
//                     B. THE OPS DID SOMETHING: carved cells, the floor fill,
//                        the cobble skin, the timber sets, the kept pillar, the
//                        prop's shelf and its carve; scatter counts reported.
//                     C. CAVES: no natural cave cell below the ground inside
//                        any sampled chunk of the keep box.
//                     D. REST: the window regenerated round the chamber and
//                        ticked; <= dungeon.maxActive chunks awake after the
//                        settle, the page pool's residency reported against
//                        the same window with the evaluator off, and the
//                        settled world hash equal on a second regenerate +
//                        settle (generation and settle reproducible).
//                   Exit: the evaluator back on, the environment reloaded, the
//                   window regenerated at the origin it found.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "phys/debris.h"
#include "game/camera.h"
#include "sim/biomes.h"
#include "sim/tuning.h"
#include "sim/chunkstore.h"
#include "sim/dungeon.h"
#include "sim/pagetable.h"
#include "sim/stream.h"
#include "sim/world.h"
#include "sim/worldedit.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "test/tickrig.h"
#include "tools/voxregion.h"
#include "world/refs.h"
#include "world/refs_npc.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;

bool ReadAll(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// ============================================================================
// dungeon-format
// ============================================================================
Status GateDungeonFormat(Ctx& c, std::string& detail) {
  const std::string ad = AssetDir();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok && why.size() < 16) why.push_back(w);
    return ok;
  };
  std::vector<std::string> names;
  std::vector<uint32_t> klass;
  for (const MaterialDef& m : c.mats) {
    names.push_back(m.name);
    klass.push_back(m.gpu.klass);
  }
  std::vector<fs::path> dirs;
  {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(ad) / "dungeons", ec))
      if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
  }
  int loaded = 0, fixturePrims = 0;
  std::string lines;
  for (const fs::path& dir : dirs) {
    const std::string id = dir.filename().string();
    // A directory with no dungeon.json is not a dungeon (D2's props/ kits live
    // beside them: assets/dungeons/props/).
    if (!fs::is_regular_file(dir / "dungeon.json")) continue;
    const std::string who = "dungeons/" + id + ": ";
    // a. dungeon.json: parses, its entrance names one of its pieces.
    std::string text;
    ReadAll(dir / "dungeon.json", text);
    nlohmann::json dj;
    bool djOk = true;
    try { dj = nlohmann::json::parse(text); } catch (const std::exception& e) {
      djOk = check(false, who + "dungeon.json does not parse: " + e.what());
    }
    if (djOk) {
      std::set<std::string> pieces;
      for (const auto& p : dj.value("pieces", nlohmann::json::array()))
        if (p.is_object() && p.contains("id") && p["id"].is_string()) pieces.insert(p["id"].get<std::string>());
      const std::string ent = dj.contains("entrance") && dj["entrance"].is_object()
                                  ? dj["entrance"].value("piece", std::string())
                                  : std::string();
      check(!pieces.empty(), who + "dungeon.json has no pieces");
      check(pieces.count(ent) == 1, who + "entrance.piece \"" + ent + "\" is not one of its pieces");
    }
    uint32_t src = 0;
    dungeon::SourceHashOfFile((dir / "dungeon.json").string(), src);
    // b. shell.bin: there, reads.
    std::vector<uint8_t> bin;
    {
      std::ifstream f(dir / "shell.bin", std::ios::binary);
      if (f) bin.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    dungeon::Shell B, J;
    std::string err;
    if (!check(!bin.empty(), who + "no shell.bin (the engine loads the .bin; compile the dungeon)")) continue;
    const bool binOk = dungeon::ReadShellBin(bin.data(), bin.size(), B, err);
    if (!check(binOk, who + "shell.bin: " + err)) continue;
    // c. the json twin says the same.
    std::string jt;
    if (ReadAll(dir / "shell.json", jt)) {
      const bool jsonOk = dungeon::ParseShellJson(jt, J, err);
      if (check(jsonOk, who + "shell.json: " + err)) {
        const bool same = dungeon::SameContent(J, B, &err);
        check(same, who + "shell.bin and shell.json differ: " + err);
      }
    }
    // d. current with dungeon.json.
    check(B.sourceHash == src, who + Format("shell.bin is STALE: compiled from dungeon.json hash %u, the file hashes %u "
                                            "(recompile; the hand fixture: re-stamp + --dungeon-bake)",
                                            B.sourceHash, src));
    // e. materials + limits; f. the index limits (a trial pack, alone).
    auto shell = std::make_shared<dungeon::Shell>(B);
    auto res = std::make_shared<dungeon::Resolved>();
    const bool resOk = dungeon::ResolveShell(*shell, names, klass, *res, err);
    if (!check(resOk, who + err)) continue;
    dungeon::Placed pl;
    pl.shell = shell;
    pl.res = res;
    pl.origin = dungeon::OriginFor(*shell, 0, {0, 0, 0});
    dungeon::PlacedBounds(pl);
    std::vector<uint32_t> W(worldmap::kHeaderWords, 0u);
    dungeon::PackStats ps;
    const bool packOk = dungeon::PackDungeons({pl}, W, err, &ps);
    check(packOk, who + err);
    // g. the engine's writers round-trip.
    {
      dungeon::Shell R1, R2;
      const std::vector<uint8_t> b2 = dungeon::WriteShellBin(B);
      std::string e1, e2;
      const bool rt1 = dungeon::ReadShellBin(b2.data(), b2.size(), R1, e1) && dungeon::SameContent(B, R1, &e1);
      const bool rt2 = dungeon::ParseShellJson(dungeon::ShellToJson(B), R2, e2) && dungeon::SameContent(B, R2, &e2);
      check(rt1,
            who + "WriteShellBin does not round-trip: " + e1);
      check(rt2,
            who + "ShellToJson does not round-trip: " + e2);
    }
    loaded++;
    if (id == "_fixture") fixturePrims = static_cast<int>(B.prims.size());
    lines += Format("%s%s %zu prims %zu props, box (%d,%d,%d)..(%d,%d,%d), %u cells %u list words", lines.empty() ? "" : "; ",
                    id.c_str(), B.prims.size(), B.instances.size(), pl.boxMin[0], pl.boxMin[1], pl.boxMin[2],
                    pl.boxMax[0], pl.boxMax[1], pl.boxMax[2], ps.indexCells, ps.listWords);
  }
  check(fixturePrims > 0, "assets/dungeons/_fixture (the engine's fixture) did not load");
  // Every map's dungeons[] names an asset that exists.
  int mapRefs = 0;
  {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(ad) / "worldmap", ec)) {
      if (!e.is_directory() || !fs::is_regular_file(e.path() / "map.json")) continue;
      for (const std::string& a : worldmap::ReadDungeonAssets(ad, e.path().filename().string())) {
        mapRefs++;
        check(fs::is_directory(fs::path(ad) / "dungeons" / a),
              "worldmap/" + e.path().filename().string() + " places dungeon asset \"" + a + "\", which does not exist");
      }
    }
  }
  // SVED v3: written, loaded, resolved through the loaded map's placement.
  std::string svedLine;
  {
    const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
    const worldmap::DungeonDef* fx = nullptr;
    for (const worldmap::DungeonDef& g : m.dungeons)
      if (g.asset == "_fixture" && g.Placed()) fx = &g;
    if (check(fx != nullptr, "the loaded map (" + m.name + ") does not place assets/dungeons/_fixture")) {
      const fs::path tmp = fs::temp_directory_path() /
                           ("sv_dgfmt_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      const std::vector<WorldEdits::AbsCell> local = {{10, 2, -3, 1u}, {600, -40, 0, 4u}, {-5, 7, 12, 1u}};
      std::string err;
      const bool w1 = WorldEdits::WriteDungeonLocal((tmp / "_fixture" / "detail.svedit").string(), local, err);
      const bool w2 = WorldEdits::WriteDungeonLocal((tmp / "nowhere" / "detail.svedit").string(), local, err);
      WorldEdits e;
      std::string lerr;
      e.LoadFiles({(tmp / "_fixture" / "detail.svedit").string(), (tmp / "nowhere" / "detail.svedit").string()}, lerr);
      e.Resolve(kDefaultSeed);
      std::vector<WorldEdits::AbsCell> got;
      e.ResolvedCells(got);
      int match = 0;
      for (const auto& l : local) {
        const int x = dungeon::RotX(fx->yawQ, l.x, l.z) + fx->origin[0];
        const int y = l.y + fx->origin[1];
        const int z = dungeon::RotZ(fx->yawQ, l.x, l.z) + fx->origin[2];
        for (const auto& g : got)
          if (g.x == x && g.y == y && g.z == z && g.word == l.word) match++;
      }
      check(w1 && w2 && e.VoxelCount() == 6 && got.size() == 3 && match == 3 && e.UnplacedDungeonCells() == 3,
            Format("SVED v3: wrote %d/%d, loaded %zu cells, resolved %zu (want 3), %d at the placement's world cells "
                   "(want 3), %zu unplaced (want 3)",
                   (int)w1, (int)w2, e.VoxelCount(), got.size(), match, e.UnplacedDungeonCells()));
      svedLine = Format("SVED v3 %d/3 cells at the yaw-%d placement, %zu unplaced", match, fx->yawQ * 90,
                        e.UnplacedDungeonCells());
      std::error_code ec;
      fs::remove_all(tmp, ec);
    }
  }
  RecordObserved("dungeon.assets", static_cast<double>(loaded));
  detail = Format("%d dungeon asset(s): %s | %d map placement(s) | %s", loaded, lines.c_str(), mapRefs, svedLine.c_str());
  if (!why.empty()) {
    detail += " | FAIL: " + why[0];
    for (size_t i = 1; i < why.size() && i < 6; i++) detail += " | " + why[i];
    std::printf("dungeon-format: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  std::printf("dungeon-format: PASS (%s)\n", detail.c_str());
  return Status::Pass;
}

// ============================================================================
// dungeon-gen
// ============================================================================
int FloorDiv16(int v) { return v >> 4; }

// SANDVOX_DUNGEON_SHOT=<path.bmp>: a picture from inside the settled fixture
// (the harness-cellar recipe: warm frames so the shadow cache and the
// irradiance grid catch up, the last one grabbed). Render only.
void DungeonShot(Ctx& c, Vec3 eye, Vec3 at, const std::string& path, int frames) {
  const Vec3 d = at - eye;
  const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  Camera cam;
  cam.yaw = std::atan2(d.z, d.x);
  cam.pitch = std::asin(std::clamp(d.y / std::max(len, 1e-3f), -1.0f, 1.0f));
  const uint32_t noon = (uint32_t)(0.5 * (double)TicksPerDay(CurrentTuning()));
  for (int f = 0; f < frames; f++) {
    WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)c.width / c.height, true, 11.7f, kFarFogDensity,
                      (float)c.height, noon);
    rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
    c.sim.EncodeShadowResolve(enc);
    rhi::RenderPass rp = c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, c.width, c.height);
    c.sim.DrawWorld(rp);
    rp.End();
    c.ctx.queue.Submit(enc.Finish());
  }
  c.ctx.WaitIdle();
  c.Grab(path.c_str());
}
// The voxel word's stain bits (amount 24..27, type 28..30).
constexpr uint32_t kStainBits = 0x7F000000u;

Status GateDungeonGen(Ctx& c, std::string& detail) {
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& w) {
    if (!ok && why.size() < 16) why.push_back(w);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idWas = c.mobs.NextIdCounter();
  const std::string ad = AssetDir();
  auto reload = [&](bool evalOn) {
    worldmap::SetDungeonsEnabled(evalOn);
    biomes::EnvironmentStamp st;
    std::string l;
    const bool ok = ReloadEnvironment(c.ctx, c.sim, c.mats, st, l);
    if (!ok) check(false, "environment reload failed: " + l);
    return ok;
  };
  auto restore = [&]() {
    worldmap::SetScratchDungeon(nullptr);
    refs::ResetResidents();
    c.debris.Reset();
    c.mobs.Reset();
    c.mobs.SetNextIdCounter(idWas);
    c.stream.Store().Clear();
    reload(true);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  };
  auto finish = [&](const std::string& d) {
    detail = d;
    if (!why.empty()) {
      detail += " | FAIL: " + why[0];
      for (size_t i = 1; i < why.size() && i < 6; i++) detail += " | " + why[i];
      std::printf("dungeon-gen: FAIL (%s)\n", detail.c_str());
      return Status::Fail;
    }
    std::printf("dungeon-gen: PASS (%s)\n", detail.c_str());
    return Status::Pass;
  };
  if (!reload(true)) { restore(); return finish("no environment"); }
  const worldmap::DungeonDef* fxp = nullptr;
  for (const worldmap::DungeonDef& g : worldmap::CurrentWorldMap().dungeons)
    if (g.id == "fixture") fxp = &g;
  if (!check(fxp && fxp->Placed(), "the harness map does not place dungeon `fixture` (assets/dungeons/_fixture)")) {
    restore();
    return finish("no fixture");
  }
  const worldmap::DungeonDef fx = *fxp;   // a COPY: the environment reloads below
  auto toWorld = [&](int lx, int ly, int lz) {
    return IVec3{dungeon::RotX(fx.yawQ, lx, lz) + fx.origin[0], ly + fx.origin[1],
                 dungeon::RotZ(fx.yawQ, lx, lz) + fx.origin[2]};
  };

  // ---- A. the twin, over chunks across the whole layout ----------------------
  // Points of interest in the fixture's LOCAL frame (shell.json's comments):
  // each brings its chunk and the +1 chunk on every axis.
  const int poi[][3] = {
      {0, 0, 0},       {40, -10, 0},    {100, -45, 0},   {201, -50, -9},  {250, -45, 0},  {301, -36, 0},
      {400, -35, 10},  {200, -62, -12}, {600, -40, 0},   {560, -60, -30}, {640, -25, 30}, {679, -55, 0},
      {530, -61, 55},  {600, -80, 100}, {600, -60, 62},  {600, -112, 180}, {576, -100, 200}, {600, -90, 250},
      {300, -100, 0},  {-60, -40, -60}, {600, -140, 0}};
  std::set<std::tuple<int, int, int>> chunkSet;
  for (const auto& q : poi) {
    const IVec3 w = toWorld(q[0], q[1], q[2]);
    for (int dz = 0; dz <= 1; dz++)
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) chunkSet.insert({FloorDiv16(w.x) + dx, FloorDiv16(w.y) + dy, FloorDiv16(w.z) + dz});
  }
  // Grouped by 32-chunk window block: GenerateChunks wants every chunk of a
  // call inside one window.
  auto generate = [&](const std::set<std::tuple<int, int, int>>& want,
                      std::map<std::tuple<int, int, int>, std::vector<uint32_t>>& out) {
    std::map<std::tuple<int, int, int>, std::vector<IVec3>> groups;
    for (const auto& [x, y, z] : want) groups[{x >> 5, y >> 5, z >> 5}].push_back(IVec3{x, y, z});
    for (const auto& [blk, list] : groups) {
      const IVec3 origin{std::get<0>(blk) * 32, std::get<1>(blk) * 32, std::get<2>(blk) * 32};
      GenerateChunks(c.ctx, c.world, c.sim, kDefaultSeed, origin, list, [&](IVec3 wc, const uint32_t* w) {
        out[{wc.x, wc.y, wc.z}] = std::vector<uint32_t>(w, w + kChunkVol);
      });
    }
  };
  std::map<std::tuple<int, int, int>, std::vector<uint32_t>> T, D;
  if (!reload(false)) { restore(); return finish("no environment (evaluator off)"); }
  generate(chunkSet, T);
  if (!reload(true)) { restore(); return finish("no environment (evaluator on)"); }
  generate(chunkSet, D);
  std::vector<uint32_t> W;
  {
    biomes::BiomeSet set;
    std::string log;
    check(biomes::LoadBiomeSet(ad, c.mats, set, log) && worldmap::PackWorldMap(set, worldmap::CurrentWorldMap(), W, log),
          "the CPU pack of the world map failed: " + log);
  }
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return static_cast<uint32_t>(i);
    return 0xFFFFu;
  };
  long long cells = 0, listed = 0, mism = 0, carved = 0, changedSolid = 0, caveInKeep = 0, soaked = 0;
  std::map<uint32_t, long long> byMat;   // material of a cell the dungeon changed to non-air
  std::string firstMism;
  for (const auto& [key, dw] : D) {
    const std::vector<uint32_t>& tw = T[key];
    const int bx = std::get<0>(key) * 16, by = std::get<1>(key) * 16, bz = std::get<2>(key) * 16;
    for (uint32_t i = 0; i < kChunkVol; i++) {
      const int x = bx + static_cast<int>(i % kChunk), y = by + static_cast<int>((i / kChunk) % kChunk),
                z = bz + static_cast<int>(i / (kChunk * kChunk));
      cells++;
      if (dungeon::TwinListAt(W, x, y, z) != 0u) listed++;
      const uint32_t exp = dungeon::TwinCell(W, c.mats, kDefaultSeed, x, y, z, tw[i]);
      // THE WET BANKS (worldgen.wgsl `wetbanks`, in the same gen dispatch,
      // after genChunk) soak the stone round a generated liquid -- a puddle's
      // walls -- which the twin of dgCell does not model: a difference in the
      // stain bits ALONE, on the same material and state, is that pass.
      if (exp != dw[i] && (exp & ~kStainBits) == (dw[i] & ~kStainBits) && (exp & kStainBits) == 0u) {
        soaked++;
      } else if (exp != dw[i]) {
        if (mism++ == 0)
          firstMism = Format("(%d,%d,%d): terrain %08x, GPU %08x, twin %08x", x, y, z, tw[i], dw[i], exp);
      }
      if (dw[i] != tw[i]) {
        if ((dw[i] & 0xFFFu) == 0u) carved++;
        else { changedSolid++; byMat[dw[i] & 0xFFFu]++; }
      }
      // C. no natural cave inside the keep box: an air cell well under the
      // ground in the evaluator-off arm (the dungeon is not drawn there).
      if (x >= fx.keepMin[0] && x <= fx.keepMax[0] && y >= fx.keepMin[1] && y <= fx.keepMax[1] && z >= fx.keepMin[2] &&
          z <= fx.keepMax[2] && (tw[i] & 0xFFFu) == 0u && y < World::TerrainHeight(x, z, kDefaultSeed) - 16)
        caveInKeep++;
    }
  }
  check(mism == 0, Format("%lld of %lld cells differ from the CPU twin; first %s", mism, cells, firstMism.c_str()));
  check(listed > 0 && carved > 0, Format("the sampled chunks have %lld listed cells and %lld carved (want both > 0)", listed, carved));
  // B. what each op made, by the material it laid.
  const uint32_t mFlag = matId("flagstone"), mCobble = matId("cobble"), mTimber = matId("timber"),
                 mPlank = matId("plank"), mGravel = matId("gravel"), mLime = matId("limestone"),
                 mMoss = matId("moss_patch"), mWater = matId("water"), mBone = matId("bone"), mIron = matId("iron");
  check(byMat[mFlag] > 0, "the chamber's flagstone floor fill laid nothing");
  check(byMat[mCobble] > 0, "the corridor's cobble skin laid nothing");
  check(byMat[mTimber] > 0, "the corridor's timber sets laid nothing");
  check(byMat[mPlank] > 0, "the alcove prop's shelf laid nothing");
  // The kept pillar: its axis cells inside the chamber's carve stay terrain.
  int keptSolid = 0, keptN = 0;
  for (int ly = -58; ly <= -24; ly += 4) {
    const IVec3 w = toWorld(600, ly, 0);
    const auto it = D.find({FloorDiv16(w.x), FloorDiv16(w.y), FloorDiv16(w.z)});
    if (it == D.end()) continue;
    const uint32_t i = static_cast<uint32_t>((w.x & 15) + (w.y & 15) * 16 + (w.z & 15) * 256);
    keptN++;
    if ((it->second[i] & 0xFFFu) != 0u && it->second[i] == T[it->first][i]) keptSolid++;
  }
  check(keptN > 0 && keptSolid == keptN, Format("the kept pillar: %d of %d axis cells are the terrain's (want all)", keptSolid, keptN));
  check(caveInKeep == 0, Format("%lld natural cave cells under the ground inside the keep box (want 0)", caveInKeep));
  const std::string aLine = Format(
      "A twin: %lld cells in %zu chunks, %lld listed, %lld mismatches (+%lld wet-bank soaks) | B ops: %lld carved, %lld laid (flagstone %lld, cobble "
      "%lld, timber %lld, plank %lld; scatter: gravel %lld, limestone %lld, moss %lld, water %lld, bone %lld, iron %lld), pillar "
      "%d/%d kept | C caves in keep %lld",
      cells, D.size(), listed, mism, soaked, carved, changedSolid, byMat[mFlag], byMat[mCobble], byMat[mTimber], byMat[mPlank],
      byMat[mGravel], byMat[mLime], byMat[mMoss], byMat[mWater], byMat[mBone], byMat[mIron], keptSolid, keptN, caveInKeep);

  // ---- E. every OTHER compiled asset, alone ----------------------------------
  // Each assets/dungeons/<id>/ with a shell.bin (D2's compiled samples, the
  // examples), placed as the voxserve scratch would place it on harness land
  // 4 km from the fixture: the twin over chunks round a sample of its prims.
  std::string eLine;
  {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(fs::path(ad) / "dungeons", ec))
      if (e.is_directory() && e.path().filename() != "_fixture" && fs::is_regular_file(e.path() / "shell.bin"))
        dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    std::vector<std::string> names;
    std::vector<uint32_t> klass;
    for (const MaterialDef& m : c.mats) {
      names.push_back(m.name);
      klass.push_back(m.gpu.klass);
    }
    int k = 0;
    for (const fs::path& dir : dirs) {
      const std::string id = dir.filename().string();
      auto sh = std::make_shared<dungeon::Shell>();
      auto rs = std::make_shared<dungeon::Resolved>();
      std::string err, used;
      const bool eOk = dungeon::LoadShellPath(dir.string(), *sh, err, &used) &&
                       dungeon::ResolveShell(*sh, names, klass, *rs, err);
      if (!check(eOk, "E " + id + ": " + err)) continue;
      worldmap::ScratchDungeon sd;
      sd.def.id = "_gate_" + id;
      sd.def.asset = id;
      sd.def.shell = sh;
      sd.def.res = rs;
      sd.def.shellPath = used;
      sd.def.ax = -2000 + 1500 * k++;
      sd.def.az = -2000;
      worldmap::SetScratchDungeon(&sd);
      if (!reload(true)) { worldmap::SetScratchDungeon(nullptr); continue; }
      const worldmap::DungeonDef* g = nullptr;
      for (const worldmap::DungeonDef& d : worldmap::CurrentWorldMap().dungeons)
        if (d.id == sd.def.id && d.Placed()) g = &d;
      if (!check(g != nullptr, "E " + id + ": not placed (" + (worldmap::CurrentWorldMap().warnings.empty()
                                                                     ? std::string("?")
                                                                     : worldmap::CurrentWorldMap().warnings.back()) + ")")) {
        worldmap::SetScratchDungeon(nullptr);
        continue;
      }
      const worldmap::DungeonDef gd = *g;
      std::set<std::tuple<int, int, int>> want;
      const size_t np = sh->prims.size();
      const size_t stride = std::max<size_t>(1, np / 24);
      for (size_t i = 0; i < np; i += stride) {
        const dungeon::Prim& pr = sh->prims[i];
        std::array<int, 3> lc{};
        if (pr.shape == dungeon::kShapeTube && pr.tubeCount > 0) {
          const dungeon::TubePt& t = sh->tube[pr.tubeFirst];
          lc = {t.x, t.y, t.z};
        } else {
          for (int a = 0; a < 3; a++) lc[a] = (pr.a[a] + pr.b[a]) / 2;
        }
        const int wx = dungeon::RotX(gd.yawQ, lc[0], lc[2]) + gd.origin[0];
        const int wy = lc[1] + gd.origin[1];
        const int wz = dungeon::RotZ(gd.yawQ, lc[0], lc[2]) + gd.origin[2];
        for (int dz = 0; dz <= 1; dz++)
          for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) want.insert({FloorDiv16(wx) + dx, FloorDiv16(wy) + dy, FloorDiv16(wz) + dz});
      }
      std::map<std::tuple<int, int, int>, std::vector<uint32_t>> Te, De;
      reload(false);
      generate(want, Te);
      reload(true);
      generate(want, De);
      std::vector<uint32_t> We;
      {
        biomes::BiomeSet set;
        std::string log;
        biomes::LoadBiomeSet(ad, c.mats, set, log);
        worldmap::PackWorldMap(set, worldmap::CurrentWorldMap(), We, log);
      }
      long long ec2 = 0, em = 0, ech = 0, esoak = 0;
      std::string first;
      for (const auto& [key, dw] : De) {
        const std::vector<uint32_t>& tw = Te[key];
        const int bx = std::get<0>(key) * 16, by = std::get<1>(key) * 16, bz = std::get<2>(key) * 16;
        for (uint32_t i = 0; i < kChunkVol; i++) {
          const int x = bx + static_cast<int>(i % kChunk), y = by + static_cast<int>((i / kChunk) % kChunk),
                    z = bz + static_cast<int>(i / (kChunk * kChunk));
          ec2++;
          const uint32_t exp = dungeon::TwinCell(We, c.mats, kDefaultSeed, x, y, z, tw[i]);
          if (exp != dw[i] && (exp & ~kStainBits) == (dw[i] & ~kStainBits) && (exp & kStainBits) == 0u)
            esoak++;
          else if (exp != dw[i] && em++ == 0)
            first = Format("(%d,%d,%d): terrain %08x, GPU %08x, twin %08x", x, y, z, tw[i], dw[i], exp);
          if (dw[i] != tw[i]) ech++;
        }
      }
      check(em == 0, Format("E %s: %lld of %lld cells differ from the CPU twin; first %s", id.c_str(), em, ec2, first.c_str()));
      check(ech > 0, Format("E %s: the dungeon changed no cell of the %zu sampled chunks", id.c_str(), De.size()));
      eLine += Format("%s%s: %zu prims, %lld cells in %zu chunks, %lld changed, %lld mismatches (+%lld wet-bank soaks)",
                      eLine.empty() ? "" : "; ", id.c_str(), np, ec2, De.size(), ech, em, esoak);
      worldmap::SetScratchDungeon(nullptr);
    }
    if (dirs.empty()) eLine = "no other compiled asset";
  }

  // ---- D. rest, residency, reproducibility ------------------------------------
  const IVec3 centre = toWorld(600, -40, 0);
  const IVec3 cc{FloorDiv16(centre.x), FloorDiv16(centre.y), FloorDiv16(centre.z)};
  const IVec3 origin{cc.x - (int)kNChunk / 2, cc.y - (int)kNChunk / 2, cc.z - (int)kNChunk / 2};
  const int settle = (int)BaselineNumber("dungeon.settleTicks", 120);
  auto settleRun = [&](uint32_t* hash, uint32_t* active, uint32_t* pages) {
    refs::ResetResidents();
    c.debris.Reset();
    c.mobs.Reset();
    c.stream.Store().Clear();
    c.stream.OnRegen();
    c.world.SetWindowOrigin(origin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    if (pages) *pages = c.world.pages ? c.world.pages->PagesInUse() : 0u;
    auto rig = std::make_unique<support::TickRig>(c, 27000u, cc);
    support::RunTicks(*rig, settle);
    c.ctx.WaitIdle();
    if (active) *active = ReadActiveChunksSync(c.ctx, c.world, c.sim);
    if (hash) *hash = HashWorldNow(c.ctx, c.world, c.sim, kDefaultSeed);
  };
  uint32_t pagesOff = 0, pagesOn = 0, active = 0, h1 = 0, h2 = 0;
  reload(false);
  {
    refs::ResetResidents();
    c.stream.Store().Clear();
    c.stream.OnRegen();
    c.world.SetWindowOrigin(origin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    pagesOff = c.world.pages ? c.world.pages->PagesInUse() : 0u;
  }
  reload(true);
  settleRun(&h1, &active, &pagesOn);
  if (const char* shot = std::getenv("SANDVOX_DUNGEON_SHOT"); shot && *shot) {
    // In the chamber looking down the corridor-side wall at the pillar, then
    // up the stair from the lower vault, then down the entrance from outside.
    auto at3 = [&](int lx, int ly, int lz) {
      const IVec3 w = toWorld(lx, ly, lz);
      return Vec3{(float)w.x + 0.5f, (float)w.y + 0.5f, (float)w.z + 0.5f};
    };
    std::string base = shot;
    const size_t dot = base.rfind('.');
    base = dot == std::string::npos ? base : base.substr(0, dot);
    const int frames = (int)BaselineNumber("dungeon.shotFrames", 12);
    DungeonShot(c, at3(665, -45, 40), at3(600, -40, 0), base + "_chamber.bmp", frames);
    DungeonShot(c, at3(600, -96, 230), at3(600, -100, 150), base + "_vault.bmp", frames);
    DungeonShot(c, at3(500, -44, 0), at3(380, -46, 0), base + "_corridor.bmp", frames);
  }
  settleRun(&h2, nullptr, nullptr);
  const uint32_t pool = c.world.pages ? c.world.pages->PoolPages() : 0u;
  const double maxActive = BaselineNumber("dungeon.maxActive", 32);
  check((double)active <= maxActive,
        Format("%u chunks awake after %d settle ticks in the window round the chamber (want <= %.0f)", active, settle, maxActive));
  check(h1 == h2, Format("the settled world hash differs between two regenerate + settle runs: %08x vs %08x", h1, h2));
  RecordObserved("dungeon.activeAfterSettle", (double)active);
  RecordObserved("dungeon.pagesInUse", (double)pagesOn);
  RecordObserved("dungeon.pagesInUseNoDungeon", (double)pagesOff);
  const std::string dLine =
      Format("D rest: %u awake after %d ticks (<= %.0f), pages %u with the dungeon vs %u without (%+d, pool %u, %.1f%%), "
             "settled hash %08x twice %s",
             active, settle, maxActive, pagesOn, pagesOff, (int)pagesOn - (int)pagesOff, pool,
             pool ? 100.0 * pagesOn / pool : 0.0, h1, h1 == h2 ? "EQUAL" : "DIFFERENT");
  restore();
  return finish(Format("fixture at (%d,%d) yaw %d, origin (%d,%d,%d), box (%d,%d,%d)..(%d,%d,%d) | ", fx.ax, fx.az,
                       fx.yawQ * 90, fx.origin[0], fx.origin[1], fx.origin[2], fx.boxMin[0], fx.boxMin[1], fx.boxMin[2],
                       fx.boxMax[0], fx.boxMax[1], fx.boxMax[2]) +
                aLine + " | E others: " + eLine + " | " + dLine);
}

}  // namespace

const std::vector<Gate>& DungeonGates() {
  static const std::vector<Gate> g = {
      {"dungeon-format", "world", {}, false, GateDungeonFormat, /*needsRender=*/false},
      {"dungeon-gen", "world", {}, false, GateDungeonGen, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
