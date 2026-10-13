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
#include <climits>
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
    if (djOk && dj.contains("extends")) {
      // A VARIANT (D8: `extends` + overrides only): its pieces live in the base
      // chain, resolved by compile_dungeon.mjs into the variant's own shell.
      // Here: the base exists; the compiled shell's anchor (step b on) is
      // what the entrance resolved to.
      const std::string base = dj["extends"].is_string() ? dj["extends"].get<std::string>() : std::string();
      check(!base.empty() && base != id && fs::is_regular_file(fs::path(ad) / "dungeons" / base / "dungeon.json"),
            who + "extends \"" + base + "\", which is not a dungeon asset");
    } else if (djOk) {
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
  // PORTABILITY (D9): the selftest's map places every real asset (a dir with a
  // dungeon.json whose name does not start with '_'), and one of them twice
  // at another yaw, so dungeon-gen runs each in place and checks the pair.
  std::string placeLine;
  {
    const worldmap::WorldMapData& m = worldmap::CurrentWorldMap();
    if (m.name == "harness") {
      int twice = 0;
      std::string missing;
      for (const fs::path& dir : dirs) {
        const std::string id = dir.filename().string();
        if (id.empty() || id[0] == '_' || !fs::is_regular_file(dir / "dungeon.json")) continue;
        int n = 0;
        std::set<int> yaws;
        for (const worldmap::DungeonDef& g : m.dungeons)
          if (g.asset == id && g.Placed()) {
            n++;
            yaws.insert(g.yawQ);
          }
        if (n == 0) missing += (missing.empty() ? "" : ", ") + id;
        if (n >= 2 && yaws.size() >= 2) twice++;
        placeLine += Format("%s%s x%d", placeLine.empty() ? "" : ", ", id.c_str(), n);
      }
      check(missing.empty(), "the harness map does not place (or could not place) real dungeon asset(s): " + missing +
                                 " -- add a dungeons[] entry (assets/worldmap/harness/map.json)");
      check(twice > 0, "the harness map places no real asset twice at two yaws (portability: dungeon-gen F)");
      placeLine = "harness places " + placeLine;
    }
  }
  // THE PREVIEW FAST PATH (voxregion.cpp PreviewDungeon): a program re-placed
  // on the loaded map by worldmap::ReplaceScratchDungeon must pack to exactly
  // the words -- and fold exactly the content hash -- a full LoadWorldMap
  // makes with that program installed. Both scratch shapes: the scratch's own
  // entry (PLACE / ISOLATE) and a map entry replaced (MAP). CPU only.
  std::string fastLine = "fast path: not run (no two placed assets)";
  {
    const worldmap::WorldMapData& cur = worldmap::CurrentWorldMap();
    std::vector<std::string> assets;
    for (const worldmap::DungeonDef& g : cur.dungeons)
      if (g.Placed() && std::find(assets.begin(), assets.end(), g.asset) == assets.end()) assets.push_back(g.asset);
    biomes::BiomeSet set;
    std::string blog;
    if (assets.size() >= 2 && biomes::LoadBiomeSet(ad, c.mats, set, blog)) {
      auto program = [&](const std::string& asset, worldmap::DungeonDef& d) {
        auto shell = std::make_shared<dungeon::Shell>();
        auto res = std::make_shared<dungeon::Resolved>();
        std::string err, used;
        if (!dungeon::LoadShellAsset(ad, asset, *shell, err, &used) ||
            !dungeon::ResolveShell(*shell, names, klass, *res, err))
          return check(false, "fast path: " + asset + ": " + err);
        d.shell = shell;
        d.res = res;
        d.shellPath = used;
        dungeon::SourceHashOfFile(used, d.shellHash);
        return true;
      };
      const std::string mapName = cur.name;
      int same = 0;
      for (int shape = 0; shape < 2; shape++) {
        worldmap::ScratchDungeon a, b;
        a.replaceMapEntry = b.replaceMapEntry = shape == 1;
        a.def.id = b.def.id = shape == 1 ? cur.dungeons.front().id : std::string("_scratch");
        a.def.ax = b.def.ax = cur.spawnX;
        a.def.az = b.def.az = cur.spawnZ;
        b.def.yawQ = 1;
        if (!program(assets[0], a.def) || !program(assets[1], b.def)) break;
        worldmap::WorldMapData ma, mb;
        std::vector<uint32_t> wFast, wFull;
        std::string l1, l2, l3;
        worldmap::SetScratchDungeon(&a);
        const bool la = worldmap::LoadWorldMap(ad, mapName, set, c.mats.size(), kDefaultSeed, ma, l1);
        worldmap::SetScratchDungeon(&b);
        const bool lb = worldmap::LoadWorldMap(ad, mapName, set, c.mats.size(), kDefaultSeed, mb, l2);
        const bool fast = la && worldmap::ReplaceScratchDungeon(ma, kDefaultSeed, l3);
        worldmap::SetScratchDungeon(nullptr);
        const bool packed = fast && lb && worldmap::PackWorldMap(set, ma, wFast, l3) &&
                            worldmap::PackWorldMap(set, mb, wFull, l3);
        if (check(packed, Format("fast path shape %d: load/replace/pack failed: %s%s%s", shape, l1.c_str(), l2.c_str(),
                                 l3.c_str())) &&
            check(wFast == wFull && ma.contentHash == mb.contentHash,
                  Format("fast path shape %d: re-placed map packs %zu words (content %08x), a full load %zu (%08x)",
                         shape, wFast.size(), ma.contentHash, wFull.size(), mb.contentHash)))
          same++;
      }
      fastLine = Format("fast path: %d/2 scratch shapes pack identically to a full load (%s -> %s)", same,
                        assets[0].c_str(), assets[1].c_str());
    }
  }
  RecordObserved("dungeon.assets", static_cast<double>(loaded));
  detail = Format("%d dungeon asset(s): %s | %d map placement(s) | %s | %s | %s", loaded, lines.c_str(), mapRefs,
                  svedLine.c_str(), placeLine.c_str(), fastLine.c_str());
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
    worldmap::SetDungeonSurfaceKeep(true);
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

  // Every placed dungeon of the map (the fixture and, D9, each real asset the
  // harness map places -- one of them twice, at another yaw), as COPIES: the
  // environment reloads below.
  std::vector<worldmap::DungeonDef> placedAll;
  for (const worldmap::DungeonDef& g : worldmap::CurrentWorldMap().dungeons)
    if (g.Placed()) placedAll.push_back(g);
  auto worldOf = [](const worldmap::DungeonDef& g, int lx, int ly, int lz) {
    return IVec3{dungeon::RotX(g.yawQ, lx, lz) + g.origin[0], ly + g.origin[1], dungeon::RotZ(g.yawQ, lx, lz) + g.origin[2]};
  };
  // THE GROUND of a column (genColumn's h; dgCell's `gh`), memoised.
  std::map<std::pair<int, int>, int> ghMemo;
  auto groundOf = [&](int x, int z) {
    auto it = ghMemo.find({x, z});
    if (it != ghMemo.end()) return it->second;
    const int g = World::TerrainHeight(x, z, kDefaultSeed);
    ghMemo.emplace(std::make_pair(x, z), g);
    return g;
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
  std::map<std::tuple<int, int, int>, size_t> chunkOwner;   // first placement that asked (index into placedAll)
  auto want2 = [&](std::set<std::tuple<int, int, int>>& into, const IVec3& w, size_t owner) {
    for (int dz = 0; dz <= 1; dz++)
      for (int dy = 0; dy <= 1; dy++)
        for (int dx = 0; dx <= 1; dx++) {
          const std::tuple<int, int, int> k{FloorDiv16(w.x) + dx, FloorDiv16(w.y) + dy, FloorDiv16(w.z) + dz};
          into.insert(k);
          chunkOwner.emplace(k, owner);
        }
  };
  size_t fxIdx = 0;
  for (size_t gi = 0; gi < placedAll.size(); gi++) {
    const worldmap::DungeonDef& g = placedAll[gi];
    if (g.id == fx.id) {
      fxIdx = gi;
      for (const auto& q : poi) want2(chunkSet, toWorld(q[0], q[1], q[2]), gi);
      continue;
    }
    // E (D9: every OTHER placement of the map, in place): chunks round a
    // sample of its prims.
    const dungeon::Shell& sh = *g.shell;
    const size_t np = sh.prims.size();
    const size_t stride = std::max<size_t>(1, np / 24);
    for (size_t i = 0; i < np; i += stride) {
      const dungeon::Prim& pr = sh.prims[i];
      std::array<int, 3> lc{};
      if (pr.shape == dungeon::kShapeTube && pr.tubeCount > 0) {
        const dungeon::TubePt& t = sh.tube[pr.tubeFirst];
        lc = {t.x, t.y, t.z};
      } else {
        for (int a = 0; a < 3; a++) lc[a] = (pr.a[a] + pr.b[a]) / 2;
      }
      want2(chunkSet, worldOf(g, lc[0], lc[1], lc[2]), gi);
    }
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
  auto packCpu = [&](std::vector<uint32_t>& out) {
    biomes::BiomeSet set;
    std::string log;
    check(biomes::LoadBiomeSet(ad, c.mats, set, log) && worldmap::PackWorldMap(set, worldmap::CurrentWorldMap(), out, log),
          "the CPU pack of the world map failed: " + log);
  };
  packCpu(W);
  auto matId = [&](const char* n) -> uint32_t {
    for (size_t i = 0; i < c.mats.size(); i++)
      if (c.mats[i].name == n) return static_cast<uint32_t>(i);
    return 0xFFFFu;
  };
  long long cells = 0, listed = 0, mism = 0, carved = 0, changedSolid = 0, caveInKeep = 0, soaked = 0;
  std::map<uint32_t, long long> byMat;   // material of a FIXTURE cell the dungeon changed to non-air
  std::vector<long long> pCells(placedAll.size(), 0), pChanged(placedAll.size(), 0), pMism(placedAll.size(), 0),
      pSoak(placedAll.size(), 0), pUnbound(placedAll.size(), 0);
  // THE GROUND BOUND's effect (D9): cells whose word differs from what the
  // same twin makes with no ground (gh = +inf, the pre-D9 rule: scatter grew
  // from any uncarved cell, open sky included) -- the stalactites / rubble /
  // puddles the bound refused, per placement.
  constexpr int kNoGround = 1 << 28;
  std::string firstMism;
  for (const auto& [key, dw] : D) {
    const std::vector<uint32_t>& tw = T[key];
    const size_t owner = chunkOwner.count(key) ? chunkOwner[key] : fxIdx;
    const bool isFx = owner == fxIdx;
    const int bx = std::get<0>(key) * 16, by = std::get<1>(key) * 16, bz = std::get<2>(key) * 16;
    for (uint32_t i = 0; i < kChunkVol; i++) {
      const int x = bx + static_cast<int>(i % kChunk), y = by + static_cast<int>((i / kChunk) % kChunk),
                z = bz + static_cast<int>(i / (kChunk * kChunk));
      cells++;
      pCells[owner]++;
      if (dungeon::TwinListAt(W, x, y, z) != 0u) listed++;
      const uint32_t exp = dungeon::TwinCell(W, c.mats, kDefaultSeed, x, y, z, tw[i], groundOf(x, z));
      if (dungeon::TwinCell(W, c.mats, kDefaultSeed, x, y, z, tw[i], kNoGround) != exp) pUnbound[owner]++;
      // THE WET BANKS (worldgen.wgsl `wetbanks`, in the same gen dispatch,
      // after genChunk) soak the stone round a generated liquid -- a puddle's
      // walls -- which the twin of dgCell does not model: a difference in the
      // stain bits ALONE, on the same material and state, is that pass.
      if (exp != dw[i] && (exp & ~kStainBits) == (dw[i] & ~kStainBits) && (exp & kStainBits) == 0u) {
        soaked++;
        pSoak[owner]++;
      } else if (exp != dw[i]) {
        pMism[owner]++;
        if (mism++ == 0)
          firstMism = Format("%s (%d,%d,%d): terrain %08x, GPU %08x, twin %08x", placedAll[owner].id.c_str(), x, y, z,
                             tw[i], dw[i], exp);
      }
      if (dw[i] != tw[i]) {
        pChanged[owner]++;
        if (isFx) {
          if ((dw[i] & 0xFFFu) == 0u) carved++;
          else { changedSolid++; byMat[dw[i] & 0xFFFu]++; }
        }
      }
      // C. no natural cave inside the keep box: an air cell well under the
      // ground in the evaluator-off arm (the dungeon is not drawn there).
      if (isFx && x >= fx.keepMin[0] && x <= fx.keepMax[0] && y >= fx.keepMin[1] && y <= fx.keepMax[1] &&
          z >= fx.keepMin[2] && z <= fx.keepMax[2] && (tw[i] & 0xFFFu) == 0u && y < groundOf(x, z) - 16)
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
  // D9: the chamber's rubble stands on its flagstone floor FILL, which lies
  // inside the chamber carve's noise (dgSolidAt; before, the carve field
  // called the fill "carved" and no pile found support).
  // Over the WHOLE chamber floor (the twin, on stone terrain: the sampled
  // chunks above see a fraction of it).
  long long floorGravel = 0, floorAir = 0;
  {
    const uint32_t mStone = matId("stone");
    for (int lz = -60; lz <= 60; lz++)
      for (int lx = 520; lx <= 680; lx++)
        for (int ly = -66; ly <= -54; ly++) {
          const IVec3 w = toWorld(lx, ly, lz);
          const uint32_t o = dungeon::TwinCell(W, c.mats, kDefaultSeed, w.x, w.y, w.z, mStone, groundOf(w.x, w.z));
          if ((o & 0xFFFu) == mGravel) floorGravel++;
          if ((o & 0xFFFu) == 0u) floorAir++;
        }
  }
  check(byMat[mGravel] > 0 || floorGravel > 0,
        Format("the chamber's rubble scatter laid nothing (sampled 0; whole floor via the twin %lld gravel, %lld air)", floorGravel,
               floorAir));
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
      "%d/%d kept, %lld scatter cells refused by the ground bound, chamber floor (twin) %lld gravel | C caves in keep %lld",
      cells, D.size(), listed, mism, soaked, carved, changedSolid, byMat[mFlag], byMat[mCobble], byMat[mTimber], byMat[mPlank],
      byMat[mGravel], byMat[mLime], byMat[mMoss], byMat[mWater], byMat[mBone], byMat[mIron], keptSolid, keptN,
      pUnbound[fxIdx], floorGravel, caveInKeep);

  // ---- E. every OTHER placement of the map, in place (D9) --------------------
  // The twin above already covered their chunks; here each must have changed
  // something, and is reported on its own.
  std::string eLine;
  for (size_t gi = 0; gi < placedAll.size(); gi++) {
    if (gi == fxIdx) continue;
    const worldmap::DungeonDef& g = placedAll[gi];
    check(pChanged[gi] > 0, Format("E %s (%s): the dungeon changed no cell of its sampled chunks", g.id.c_str(), g.asset.c_str()));
    eLine += Format("%s%s=%s yaw %d at (%d,%d) ground y%d: %zu prims %zu surface rects, %lld cells, %lld changed, %lld mismatches "
                    "(+%lld soaks), %lld refused by the ground bound",
                    eLine.empty() ? "" : "; ", g.id.c_str(), g.asset.c_str(), g.yawQ * 90, g.ax, g.az, g.groundY,
                    g.shell->prims.size(), g.surf.size(), pCells[gi], pChanged[gi], pMism[gi], pSoak[gi], pUnbound[gi]);
  }
  if (eLine.empty()) eLine = "no other placement";

  // ---- F. PORTABILITY (D9): one asset placed twice is the same dungeon -------
  // The carve field is evaluated in each placement's LOCAL frame, so at the
  // same local cell two placements of one asset must agree whatever their
  // anchor and yaw: sampled over every prim's box (a 5^3 lattice each),
  // through the packed block's own records (the twin -- which A holds to the
  // GPU cell by cell).
  std::string fLine;
  {
    int pairs = 0;
    long long fN = 0, fCarved = 0, fBad = 0;
    std::string fFirst;
    for (size_t a = 0; a < placedAll.size(); a++)
      for (size_t b = a + 1; b < placedAll.size(); b++) {
        const worldmap::DungeonDef& A = placedAll[a];
        const worldmap::DungeonDef& B = placedAll[b];
        if (A.asset != B.asset) continue;
        pairs++;
        check(A.yawQ != B.yawQ || A.ax != B.ax || A.az != B.az,
              Format("F %s and %s: two placements of %s on the same anchor and yaw", A.id.c_str(), B.id.c_str(), A.asset.c_str()));
        const dungeon::Shell& sh = *A.shell;
        for (const dungeon::Prim& pr : sh.prims) {
          std::array<int, 3> lo{}, hi{};
          if (pr.shape == dungeon::kShapeTube || pr.shape == dungeon::kShapeCapsule) {
            if (pr.shape == dungeon::kShapeCapsule) {
              for (int k = 0; k < 3; k++) { lo[k] = std::min(pr.a[k], pr.b[k]) - pr.r; hi[k] = std::max(pr.a[k], pr.b[k]) + pr.r; }
            } else {
              if (pr.tubeCount == 0) continue;
              const dungeon::TubePt& t = sh.tube[pr.tubeFirst];
              lo = {t.x - t.r, t.y - t.r, t.z - t.r};
              hi = {t.x + t.r, t.y + t.r, t.z + t.r};
            }
          } else {
            lo = pr.a;
            hi = pr.b;
          }
          for (int iz = 0; iz < 5; iz++)
            for (int iy = 0; iy < 5; iy++)
              for (int ix = 0; ix < 5; ix++) {
                const int lx = lo[0] + ((hi[0] - lo[0]) * ix) / 4, ly = lo[1] + ((hi[1] - lo[1]) * iy) / 4,
                          lz = lo[2] + ((hi[2] - lo[2]) * iz) / 4;
                // world = R(yaw) local + origin (origin already holds the anchor).
                const IVec3 wa2 = worldOf(A, lx, ly, lz);
                const IVec3 wb2 = worldOf(B, lx, ly, lz);
                const int ca = dungeon::TwinCarveAt(W, c.mats, kDefaultSeed, wa2.x, wa2.y, wa2.z);
                const int cb = dungeon::TwinCarveAt(W, c.mats, kDefaultSeed, wb2.x, wb2.y, wb2.z);
                fN++;
                if (ca < 0) fCarved++;
                if ((ca < 0) != (cb < 0)) {
                  if (fBad++ == 0)
                    fFirst = Format("local (%d,%d,%d): %s carve %d at (%d,%d,%d), %s carve %d at (%d,%d,%d)", lx, ly, lz,
                                    A.id.c_str(), ca, wa2.x, wa2.y, wa2.z, B.id.c_str(), cb, wb2.x, wb2.y, wb2.z);
                }
              }
        }
        fLine += Format("%s%s == %s (yaw %d / %d)", fLine.empty() ? "" : ", ", A.id.c_str(), B.id.c_str(), A.yawQ * 90,
                        B.yawQ * 90);
      }
    check(pairs > 0, "F: the map places no asset twice (the harness map should: heather_cairn + heather_cairn_b)");
    check(fBad == 0, Format("F: %lld of %lld sampled local cells carve differently in two placements of one asset; first %s", fBad,
                            fN, fFirst.c_str()));
    check(pairs == 0 || fCarved > 0, "F: no sampled local cell is carved (the sample missed the program)");
    fLine = Format("F portability: %d pair(s) [%s], %lld local cells (%lld carved), %lld disagree", pairs, fLine.c_str(), fN,
                   fCarved, fBad);
  }

  // ---- G. TREES AT THE ENTRANCES (D9) ----------------------------------------
  // Every placement's surface rects (dungeon.h SurfaceRects: where its prims /
  // props reach the ground), +16 columns, from under the ground to 20 m over
  // it, generated in four arms: the surface keep on / off x the evaluator on /
  // off.
  //   would-be  = above-ground matter the dungeon WOULD cut with the keep off,
  //               and how much of it is TREE (wood / bark / leaves / needles):
  //               the check is not vacuous only if some entrance is in trees;
  //   cut       = above-ground matter the dungeon cuts with the keep on (0:
  //               no trunk cut, no crown left floating);
  //   in rects  = above-ground matter standing inside a rect, keep on (0);
  //   reach     = every cell the keep removed is within a tree's reach of a
  //               rect by the CPU twin's dgSurfNear (TwinSurfNear).
  std::string gLine;
  {
    std::set<std::tuple<int, int, int>> gSet;
    std::map<std::tuple<int, int, int>, size_t> gOwner;
    size_t nRects = 0;
    // Per CHUNK COLUMN over the rects (+16), the layers from 1.6 m under its
    // lowest ground sample to 6.4 m over its highest: where a trunk meets the
    // ground and a carve could cut it (a cut trunk is cut at the rim; the
    // crown it would leave floating is above). Bounded, so the four arms stay
    // a few thousand chunks over every placement.
    std::map<std::pair<int, int>, size_t> gCols;
    for (size_t gi = 0; gi < placedAll.size(); gi++)
      for (const auto& r : placedAll[gi].surf) {
        nRects++;
        for (int cz = FloorDiv16(r[1] - 16); cz <= FloorDiv16(r[3] + 16); cz++)
          for (int cx = FloorDiv16(r[0] - 16); cx <= FloorDiv16(r[2] + 16); cx++) gCols.emplace(std::make_pair(cx, cz), gi);
      }
    for (const auto& [cc, gi] : gCols) {
      int lo = INT_MAX, hi = INT_MIN;
      for (int dz = 0; dz <= 15; dz += 5)
        for (int dx = 0; dx <= 15; dx += 5) {
          const int g = groundOf(cc.first * 16 + dx, cc.second * 16 + dz);
          lo = std::min(lo, g);
          hi = std::max(hi, g);
        }
      for (int cy = FloorDiv16(lo - 16); cy <= FloorDiv16(hi + 64); cy++) {
        gSet.insert({cc.first, cy, cc.second});
        gOwner.emplace(std::make_tuple(cc.first, cy, cc.second), gi);
      }
    }
    const auto gT0 = std::chrono::steady_clock::now();
    std::printf("dungeon-gen G: %zu rects, %zu chunk columns, %zu chunks x4 arms\n", nRects, gCols.size(), gSet.size());
    std::fflush(stdout);
    check(!fx.surf.empty(), "G: the fixture has no surface rect (its entrance stair should break the ground)");
    std::map<std::tuple<int, int, int>, std::vector<uint32_t>> T1, D1, T0, D0;
    reload(false);
    generate(gSet, T1);
    reload(true);
    generate(gSet, D1);
    worldmap::SetDungeonSurfaceKeep(false);
    reload(false);
    generate(gSet, T0);
    reload(true);
    generate(gSet, D0);
    worldmap::SetDungeonSurfaceKeep(true);
    reload(true);
    std::vector<uint32_t> Wg;
    packCpu(Wg);
    std::vector<bool> treeMat(c.mats.size(), false);
    for (size_t m = 0; m < c.mats.size(); m++) {
      const std::string& n = c.mats[m].name;
      treeMat[m] = n.find("wood") != std::string::npos || n.find("bark") != std::string::npos ||
                   n.find("lea") != std::string::npos || n.find("needle") != std::string::npos;
    }
    std::vector<long long> pWould(placedAll.size(), 0), pWouldTree(placedAll.size(), 0), pCut(placedAll.size(), 0),
        pRemoved(placedAll.size(), 0);
    long long inRects = 0, twinBad = 0;
    std::string cutFirst, rectFirst;
    // Per chunk column, per voxel column: the ground and "inside a rect", once.
    struct ColInfo {
      std::array<int, kChunk * kChunk> g{};
      std::array<bool, kChunk * kChunk> r{};
    };
    std::map<std::pair<int, int>, ColInfo> colInfo;
    const ColInfo* cur = nullptr;
    auto above = [&](uint32_t w, int ci, int y) {
      const uint32_t m = w & 0xFFFu;
      return m != 0u && m < c.mats.size() && c.mats[m].gpu.klass != CLASS_LIQUID && y > cur->g[ci];
    };
    for (const auto& [key, t1] : T1) {
      const size_t gi = gOwner[key];
      const std::vector<uint32_t>& d1 = D1[key];
      const std::vector<uint32_t>& t0 = T0[key];
      const std::vector<uint32_t>& d0 = D0[key];
      const int bx = std::get<0>(key) * 16, by = std::get<1>(key) * 16, bz = std::get<2>(key) * 16;
      {
        auto it = colInfo.find({bx, bz});
        if (it == colInfo.end()) {
          ColInfo ci;
          for (int k = 0; k < 256; k++) {
            const int x = bx + k % 16, z = bz + k / 16;
            ci.g[k] = groundOf(x, z);
            bool inR = false;
            for (const auto& g : placedAll)
              for (const auto& r : g.surf) inR = inR || (x >= r[0] && x <= r[2] && z >= r[1] && z <= r[3]);
            ci.r[k] = inR;
          }
          it = colInfo.emplace(std::make_pair(bx, bz), ci).first;
        }
        cur = &it->second;
      }
      for (uint32_t i = 0; i < kChunkVol; i++) {
        const int x = bx + static_cast<int>(i % kChunk), y = by + static_cast<int>((i / kChunk) % kChunk),
                  z = bz + static_cast<int>(i / (kChunk * kChunk));
        const int ci = static_cast<int>(i % kChunk) + static_cast<int>(i / (kChunk * kChunk)) * 16;
        if (d0[i] != t0[i] && above(t0[i], ci, y)) {
          pWould[gi]++;
          if (treeMat[t0[i] & 0xFFFu]) pWouldTree[gi]++;
        }
        if (d1[i] != t1[i] && above(t1[i], ci, y)) {
          if (pCut[gi]++ == 0 && cutFirst.empty())
            cutFirst = Format("%s (%d,%d,%d) %s", placedAll[gi].id.c_str(), x, y, z, c.mats[t1[i] & 0xFFFu].name.c_str());
        }
        if (t0[i] != t1[i]) {
          pRemoved[gi]++;
          if (!dungeon::TwinSurfNear(Wg, x, z, 160)) twinBad++;
        }
        if (cur->r[ci] && above(t1[i], ci, y)) {
          if (inRects++ == 0) rectFirst = Format("(%d,%d,%d) %s", x, y, z, c.mats[t1[i] & 0xFFFu].name.c_str());
        }
      }
    }
    long long wouldTree = 0, cut = 0;
    std::string per;
    for (size_t gi = 0; gi < placedAll.size(); gi++) {
      wouldTree += pWouldTree[gi];
      cut += pCut[gi];
      per += Format("%s%s %zu rects: would-be %lld (tree %lld), cut %lld, keep removed %lld", per.empty() ? "" : "; ",
                    placedAll[gi].id.c_str(), placedAll[gi].surf.size(), pWould[gi], pWouldTree[gi], pCut[gi], pRemoved[gi]);
    }
    check(wouldTree > 0, "G: with the surface keep off no entrance would cut a tree: the check is vacuous (place one in trees)");
    check(cut == 0, Format("G: the dungeons cut %lld above-ground cells (trees / cover) with the surface keep on; first %s", cut,
                           cutFirst.c_str()));
    check(inRects == 0, Format("G: %lld above-ground matter cells stand inside the surface rects; first %s", inRects, rectFirst.c_str()));
    check(twinBad == 0, Format("G: %lld cells the keep removed lie outside every rect's reach (GPU dgSurfNear vs the twin)", twinBad));
    gLine = Format("G trees: %zu rects, %zu chunks x4 arms (%.1f s), in rects %lld, outside reach %lld; %s", nRects, gSet.size(),
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - gT0).count(), inRects, twinBad, per.c_str());
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
                aLine + " | E others: " + eLine + " | " + fLine + " | " + gLine + " | " + dLine);
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
