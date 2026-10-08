// selftest_locale.cpp -- `locale`: a hand-made place that rides its ground
// (docs/PLAN_map_v2.md L1, worldmap.h LocaleDef).
//
// WHAT THIS GUARDS. A locale is a box on the map with an ANCHOR; its ref group
// (refs/<id>.json) is stored with y relative to anchor.y, and at load every ref
// of it lands at file y + the bare ground at the anchor. Its own edit layer
// (locales/<id>/ground.svedit) applies after the map's. The promise: reshape
// the world under a locale and the WHOLE locale moves with it, rigidly, and
// nothing else does. A synthetic map is written to a TEMP asset directory, so
// nothing here depends on, or touches, the shipped maps. CPU only: the loader,
// the height mirror, the ref store and the layer resolver are what the GPU is
// handed (the `terrain` and `worldmap-v2` gates hold the GPU to the mirror).
//
//   A. TWO GROUNDS. Map A has a flat authored plane at G (noise off, so the
//      ground IS the plane); map B is the same with the plane 200 voxels
//      higher. The locale's delta is 0 on A (anchor.y == G) and 200 on B.
//   B. THE LOCALE MOVES BY EXACTLY THAT: its house's stamp site (refY and the
//      pad's floor padY), the ground ON the pad (TerrainHeight inside the
//      footprint), the house and a waynode as the RefStore holds them, and
//      the locale layer's cells on the pad. A waynode in an ordinary group
//      does NOT move.
//   C. OVERLAPPING LAYERS. The map layer and the locale layer name the same
//      cell: the locale's word wins (later in table order); cells only one
//      names both apply; two independent loads resolve to identical cell
//      lists (determinism).
//   D. WORLD IN, RELATIVE OUT. Moving the locale's waynode up 3 voxels (the
//      authoring action every editor command uses) writes file y + 3 -- the
//      file stays anchor-relative -- and the ordinary group's file is
//      untouched.
//
// Leaves the loaded map exactly as it found it (CurrentWorldMap restored, the
// structure override put back) and deletes the temp directory.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/biomes.h"
#include "sim/world.h"
#include "sim/worldedit.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "world/refs.h"

namespace selftest {
namespace {

namespace fs = std::filesystem;
using sandvox::kDefaultSeed;

bool WriteHeightTile(const fs::path& p, int value) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  if (!f) return false;
  const uint32_t hdr[8] = {worldmap::kTileMagic, worldmap::kPlaneHeight, 0u, 0u,
                           static_cast<uint32_t>(worldmap::kTileSide), static_cast<uint32_t>(worldmap::kTileSide), 2u, 0u};
  f.write(reinterpret_cast<const char*>(hdr), sizeof hdr);
  const int16_t s = static_cast<int16_t>(value);
  for (int i = 0; i < worldmap::kTileSide * worldmap::kTileSide; i++) f.write(reinterpret_cast<const char*>(&s), 2);
  return static_cast<bool>(f);
}

// A v2 ground-relative SVED (worldedit.cpp is the authority): {x, z, dy, word}.
bool WriteRelLayer(const fs::path& p, const std::vector<std::tuple<int, int, int, uint32_t>>& cells) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::map<std::pair<int, int>, std::vector<std::pair<int, uint32_t>>> cols;
  for (const auto& [x, z, dy, w] : cells) cols[{x, z}].push_back({dy, w});
  std::vector<uint32_t> b = {sandvox::WorldEdits::kMagic, 2u, static_cast<uint32_t>(cols.size()),
                             static_cast<uint32_t>(cells.size()), kDefaultSeed, sandvox::WorldEdits::kFlagRelative, 0u, 0u};
  for (const auto& [xz, cs] : cols) {
    b.push_back(static_cast<uint32_t>(xz.first));
    b.push_back(static_cast<uint32_t>(xz.second));
    b.push_back(static_cast<uint32_t>(cs.size()));
    for (const auto& [dy, w] : cs) {
      b.push_back(static_cast<uint32_t>(dy));
      b.push_back(w);
    }
  }
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size() * 4));
  return static_cast<bool>(f);
}

bool ReadText(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// What one ground (A or B) made of the locale.
struct Arm {
  bool loaded = false;
  std::string log;
  int offset = 0, delta = 0, groundY = 0;
  int siteRefY = 0, sitePadY = 0;
  bool site = false;
  int padGround = 0;              // TerrainHeight at a column inside the house's rect
  int houseY = 0, nodeY = 0, freeY = 0;   // as the RefStore holds them
  bool refs = false;
  size_t layerFiles = 0;
  std::vector<sandvox::WorldEdits::AbsCell> cells;   // the resolved layer
  std::vector<sandvox::WorldEdits::AbsCell> cells2;  // ...a second, independent load
};

Status GateLocale(Ctx& c, std::string& detail) {
  const std::string real = sandvox::AssetDir();
  std::vector<std::string> problems;
  auto fail = [&](const std::string& why) {
    detail = why;
    std::printf("locale: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  };
  biomes::BiomeSet set;
  std::string log;
  if (!biomes::LoadBiomeSet(real, c.mats, set, log)) return fail("biome files did not load: " + log);
  const worldmap::WorldMapData prevMap = worldmap::CurrentWorldMap();   // a COPY, restored on the way out

  // ---- the synthetic map ---------------------------------------------------------
  const fs::path tmp = fs::temp_directory_path() /
                       ("sv_locale_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path mdir = tmp / "worldmap" / "loc";
  // Far from every other gate's ground (and from the window): CPU only.
  constexpr int X0 = 40960, Z0 = 40960, W = 8192;
  constexpr int G = 260, kRaise = 200;
  constexpr int AX = X0 + 4096, AZ = Z0 + 4096;            // the anchor
  const int HX = AX + 40, HZ = AZ + 10;                    // the house (harrowby_green)
  const int NX = AX - 30, NZ = AZ - 40;                    // the locale's waynode
  const int FX = AX + 900, FZ = AZ + 900;                  // an ordinary group's waynode
  const int fileHouseY = 1, fileNodeY = 1;                 // anchor-relative, as stored
  const int freeY = G + 3;                                 // absolute
  nlohmann::json te = nlohmann::json::object();
  {
    std::ifstream f(real + "/worldmap/" + prevMap.name + "/map.json");
    nlohmann::json rj;
    try { f >> rj; } catch (...) {}
    if (rj.contains("terrain")) te = rj["terrain"];
  }
  // NOISE OFF (the worldmap-v2 gate's B arm): the ground IS the authored
  // plane, so the anchor's change is exactly the raise and every claim below
  // is "moved by exactly 200". With the noise on the anchor moves by whatever
  // the noise makes of a raised plane (measured 2026-10-08: +183 for +200 --
  // the octaves are not translation-invariant in y; A1's business), and the
  // locale still follows it exactly, which B asserts against the measured
  // offset rather than the raise.
  for (const char* k : {"rangeAmplitude", "hillAmplitude", "detailAmplitude", "grainAmplitude", "sedMax", "sedTopsoil"})
    te[k] = 0;
  const nlohmann::json mj = {
      {"version", 2}, {"name", "loc"},
      {"extent", {{"min", {X0, Z0}}, {"size", {W, W}}}},
      {"siteCellLog2", 10},
      {"planes", {{"height", {{"log2", 6}, {"tile", worldmap::kTileSide}, {"default", G}}},
                  {"biome", {{"log2", 8}, {"tile", worldmap::kTileSide}, {"default", "meadow"}}}}},
      {"seaLevelY", -20000}, {"edgeFadeVox", 2048}, {"warpAmpVox", 0},
      {"biomes", {"meadow"}},
      {"terrain", te},
      {"sites", {{{"id", "spawn"}, {"kind", "spawn"}, {"at", {FX + 400, FZ + 400}}}}},
      {"rules", nlohmann::json::array()},
      {"locales", {{{"id", "hut"}, {"name", "Hut"}, {"icon", "hut"},
                    {"bounds", {{"min", {AX - 200, AZ - 200}}, {"max", {AX + 200, AZ + 200}}}},
                    {"anchor", {{"at", {AX, AZ}}, {"y", G}}}}}},
      {"editLayer", "loc_ground"}};
  bool wrote = true;
  {
    std::error_code ec;
    fs::create_directories(mdir / "refs", ec);
    fs::create_directories(tmp / "structures", ec);
    for (const char* ext : {".vox", ".struct.json"})
      fs::copy_file(fs::path(real) / "structures" / (std::string("harrowby_green") + ext),
                    tmp / "structures" / (std::string("harrowby_green") + ext), fs::copy_options::overwrite_existing, ec);
    wrote = wrote && !ec;
    std::ofstream(mdir / "map.json") << mj.dump(2);
    // Both groups in the file format WriteGroup makes (one ref per line).
    std::ofstream(mdir / "refs" / "hut.json", std::ios::binary)
        << "{\n  \"group\": \"hut\",\n  \"refs\": [\n"
        << "    { \"id\": \"hut/house\", \"kind\": \"structure\", \"base\": \"harrowby_green\", \"pos\": [" << HX << ", "
        << fileHouseY << ", " << HZ << "], \"yaw\": 0 },\n"
        << "    { \"id\": \"hut/node\", \"kind\": \"waynode\", \"pos\": [" << NX << ", " << fileNodeY << ", " << NZ
        << "], \"yaw\": 0 }\n  ]\n}\n";
    std::ofstream(mdir / "refs" / "free.json", std::ios::binary)
        << "{\n  \"group\": \"free\",\n  \"refs\": [\n"
        << "    { \"id\": \"free/node\", \"kind\": \"waynode\", \"pos\": [" << FX << ", " << freeY << ", " << FZ
        << "], \"yaw\": 0 }\n  ]\n}\n";
  }
  // The layers. The map's: a cell on the house's pad (P) and one on open
  // ground (Q). The locale's: the SAME cell as P with another word, and its
  // own cell R on the pad. P and R sit on the flat apron round the green.
  const int PX = HX, PZ = HZ - 2, RX = HX + 1, RZ = HZ - 2;
  const uint32_t kMapWord = 1u, kLocWord = 4u;   // stone, gravel: any two solids
  wrote = wrote && WriteRelLayer(tmp / "worldedits" / "loc_ground.svedit", {{PX, PZ, 1, kMapWord}, {FX, FZ, 1, kMapWord}}) &&
          WriteRelLayer(mdir / "locales" / "hut" / "ground.svedit", {{PX, PZ, 1, kLocWord}, {RX, RZ, 2, kLocWord}});
  if (!wrote) {
    std::error_code ec;
    fs::remove_all(tmp, ec);
    return fail("could not write the synthetic map under " + tmp.string());
  }
  const std::vector<worldmap::StructurePlacement>* prevOverride = worldmap::StructureOverrideSlot();
  const std::vector<worldmap::StructurePlacement> prevList =
      prevOverride ? *prevOverride : std::vector<worldmap::StructurePlacement>{};
  worldmap::SetStructureOverride(nullptr);   // read the temp map's refs dir
  const std::string tdir = tmp.string();

  auto runArm = [&](int plane, Arm& a) {
    WriteHeightTile(mdir / "planes" / "height" / "0_0.svt", plane);
    worldmap::WorldMapData m;
    a.loaded = worldmap::LoadWorldMap(tdir, "loc", set, c.mats.size(), kDefaultSeed, m, a.log);
    if (!a.loaded) return;
    if (m.locales.size() == 1) {
      a.offset = m.locales[0].RefYOffset();
      a.delta = m.locales[0].Delta();
      a.groundY = m.locales[0].groundY;
    }
    for (const worldmap::WorldMapData::StampSite& s : m.sites)
      if (s.structure && s.id == "hut/house") {
        a.site = true;
        a.siteRefY = s.refY;
        a.sitePadY = s.padY;
      }
    const std::string editLayer = m.editLayer;
    const std::vector<worldmap::LocaleDef> locales = m.locales;
    worldmap::SetCurrentWorldMap(std::move(m));
    a.padGround = World::TerrainHeight(HX, HZ - 2, kDefaultSeed);
    refs::RefStore st;
    st.LoadMap(tdir, "loc");
    const refs::Ref* h = st.Find("hut/house");
    const refs::Ref* n = st.Find("hut/node");
    const refs::Ref* fr = st.Find("free/node");
    a.refs = h && n && fr;
    if (a.refs) { a.houseY = h->pos.y; a.nodeY = n->pos.y; a.freeY = fr->pos.y; }
    std::vector<std::string> paths;
    for (const auto& [p, nm] : worldmap::MapEditLayerFiles(tdir, "loc", editLayer, locales)) paths.push_back(p);
    a.layerFiles = paths.size();
    for (auto* out : {&a.cells, &a.cells2}) {
      sandvox::WorldEdits e;
      std::string err;
      e.LoadFiles(paths, err);
      e.Resolve(kDefaultSeed);
      e.ResolvedCells(*out);
    }
  };
  Arm A, B;
  runArm(G, A);
  runArm(G + kRaise, B);

  // ---- D. world in, relative out (on B's ground, which is still current) ----------
  bool dOk = false;
  std::string dWhy;
  {
    std::string freeBefore, freeAfter, hutAfter;
    ReadText(mdir / "refs" / "free.json", freeBefore);
    refs::RefStore st;
    st.LoadMap(tdir, "loc");
    std::string err;
    const refs::Ref* n = st.Find("hut/node");
    const bool moved = n && refs::Move(st, "hut/node", {n->pos.x, n->pos.y + 3, n->pos.z}, n->yaw, &err);
    ReadText(mdir / "refs" / "free.json", freeAfter);
    ReadText(mdir / "refs" / "hut.json", hutAfter);
    refs::RefGroupFile g;
    std::vector<std::string> w;
    int fileY = -99999;
    if (refs::ParseGroup(hutAfter, "refs/hut.json", g, w))
      for (const refs::Ref& r : g.refs)
        if (r.id == "hut/node") fileY = r.pos.y;
    const refs::Ref* n2 = st.Find("hut/node");
    dOk = moved && fileY == fileNodeY + 3 && freeAfter == freeBefore && n2 && n2->pos.y == B.nodeY + 3;
    dWhy = Format("move %s%s; file y %d (want %d); store y %d (want %d); ordinary group's file %s", moved ? "ok" : "REFUSED: ",
                  err.c_str(), fileY, fileNodeY + 3, n2 ? n2->pos.y : -99999, B.nodeY + 3,
                  freeAfter == freeBefore ? "untouched" : "REWRITTEN");
  }

  // ---- restore the world map before judging ------------------------------------------
  worldmap::SetCurrentWorldMap(prevMap);
  worldmap::SetStructureOverride(prevOverride ? &prevList : nullptr);
  {
    std::error_code ec;
    fs::remove_all(tmp, ec);
  }
  if (!A.loaded || !B.loaded) return fail("the synthetic locale map was REFUSED: " + A.log + B.log);

  // ---- A + B ---------------------------------------------------------------------------
  const int m = B.offset - A.offset;
  auto cellAt = [](const std::vector<sandvox::WorldEdits::AbsCell>& v, int x, int z, int* y, uint32_t* w) {
    int n = 0;
    for (const auto& cl : v)
      if (cl.x == x && cl.z == z) { *y = cl.y; *w = cl.word; n++; }
    return n;
  };
  int pyA = 0, pyB = 0, ryA = 0, ryB = 0, qyA = 0, qyB = 0;
  uint32_t pwA = 0, pwB = 0, rwA = 0, qwA = 0;
  uint32_t dummy = 0;
  const int nP = cellAt(A.cells, PX, PZ, &pyA, &pwA) + cellAt(B.cells, PX, PZ, &pyB, &pwB);
  const int nR = cellAt(A.cells, RX, RZ, &ryA, &rwA) + cellAt(B.cells, RX, RZ, &ryB, &dummy);
  const int nQ = cellAt(A.cells, FX, FZ, &qyA, &qwA) + cellAt(B.cells, FX, FZ, &qyB, &dummy);
  const bool aOk = A.site && B.site && A.refs && B.refs && m == kRaise && A.delta == 0 && B.delta == kRaise;
  std::printf("locale: A two grounds: %s (anchor ground y%d -> y%d, authored y%d: delta %+d -> %+d; the locale's offset "
              "moved %+d, the plane %+d)\n",
              aOk ? "PASS" : "FAIL", A.groundY, B.groundY, G, A.delta, B.delta, m, kRaise);
  if (!aOk)
    problems.push_back(Format("A: site %d/%d refs %d/%d, offset moved %d (want %d)", A.site, B.site, A.refs, B.refs, m, kRaise));
  const int dSite = B.siteRefY - A.siteRefY, dPad = B.sitePadY - A.sitePadY, dGround = B.padGround - A.padGround;
  const int dHouse = B.houseY - A.houseY, dNode = B.nodeY - A.nodeY, dFree = B.freeY - A.freeY;
  const int dP = pyB - pyA, dR = ryB - ryA;
  const bool bOk = dSite == m && dPad == m && dGround == m && dHouse == m && dNode == m && dFree == 0 && nP == 2 &&
                   nR == 2 && dP == m && dR == m && A.houseY == A.offset + fileHouseY && A.sitePadY == A.houseY - 1 &&
                   A.padGround == A.sitePadY;
  std::printf("locale: B rigid move: %s (structure refY %+d, pad floor %+d, ground on the pad %+d, RefStore house %+d, "
              "waynode %+d, layer cells on the pad %+d / %+d; ordinary waynode %+d; house at y%d = offset %d + file %d, "
              "pad top y%d, ground there y%d)\n",
              bOk ? "PASS" : "FAIL", dSite, dPad, dGround, dHouse, dNode, dP, dR, dFree, A.houseY, A.offset, fileHouseY,
              A.sitePadY, A.padGround);
  if (!bOk)
    problems.push_back(Format("B: moved site %d pad %d ground %d house %d node %d cells %d/%d (want %d each), ordinary %d "
                              "(want 0)", dSite, dPad, dGround, dHouse, dNode, dP, dR, m, dFree));
  const bool sameTwice = A.cells.size() == A.cells2.size() && B.cells.size() == B.cells2.size() &&
                         std::equal(A.cells.begin(), A.cells.end(), A.cells2.begin(), [](const auto& x, const auto& y) {
                           return x.x == y.x && x.y == y.y && x.z == y.z && x.word == y.word;
                         });
  const bool cOk = A.layerFiles == 2 && B.layerFiles == 2 && A.cells.size() == 3 && pwA == kLocWord && pwB == kLocWord &&
                   rwA == kLocWord && nQ == 2 && qwA == kMapWord && sameTwice;
  std::printf("locale: C overlapping layers: %s (%zu files; %zu cells resolved; the shared cell is word %u (the locale's %u, "
              "not the map's %u); map-only cell word %u; two loads identical %d)\n",
              cOk ? "PASS" : "FAIL", A.layerFiles, A.cells.size(), pwA, kLocWord, kMapWord, qwA, sameTwice);
  if (!cOk) problems.push_back("C: overlapping layers did not resolve as map-then-locale");
  std::printf("locale: D world in, relative out: %s (%s)\n", dOk ? "PASS" : "FAIL", dWhy.c_str());
  if (!dOk) problems.push_back("D: " + dWhy);

  if (!problems.empty()) {
    detail = problems[0];
    for (size_t i = 1; i < problems.size(); i++) detail += "; " + problems[i];
    std::printf("locale: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  detail = Format("a +%d plane moved the locale's house, pad, refs and layer by %+d, nothing else", kRaise, m);
  std::printf("locale: PASS\n");
  return Status::Pass;
}

}  // namespace

const std::vector<Gate>& LocaleGates() {
  static const std::vector<Gate> g = {
      {"locale", "sim", {}, false, GateLocale},
  };
  return g;
}

}  // namespace selftest
