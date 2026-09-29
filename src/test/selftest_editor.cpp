// selftest_editor.cpp — THE IN-GAME EDITOR'S COMMAND LAYER (docs/PLAN_world_editor.md
// P5, editor/commands.h, DESIGN.md §16.P5).
//
//   editor-commands     A script (the --edit-script runner itself) that runs
//                       EVERY public command -- refs, waynode links, a house
//                       opened, every voxel tool, slots, batch, undo / redo,
//                       two saves -- against a scratch refs dir and a _gate
//                       copy of a sample house. Then: undo every step -> every
//                       file byte-identical to the start (the group file the
//                       script created is gone again); redo every step -> byte-
//                       identical to the applied state. A second script with a
//                       typo on line 2 must fail naming the line and the field,
//                       with "did you mean", and leave every file untouched.
//   editor-struct-edit  A house placed at yaw 90 is opened through its ref and
//                       edited in WORLD coordinates (a fill, an erase, one
//                       voxel outside the box that grows it); struct.save; the
//                       asset on disk changed in its LOCAL frame (re-based:
//                       origin moved by the growth, every edit at house + new
//                       origin, the door slot's derived world box unchanged,
//                       handEdited true); the live re-apply regenerates and the
//                       world cells hold the edits and equal a fresh worldgen;
//                       undoing the save puts both files back byte for byte.
//                       Also: editor::HouseToWorld == structures::Frame::Cell
//                       at all four turns.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/commands.h"
#include "editor/struct_edit.h"
#include "sim/biomes.h"
#include "sim/stream.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "world/refs.h"
#include "world/structures.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;
using Json = editor::Json;

std::string Slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Every file under `dir` (recursive) plus `extra`, path -> bytes.
std::map<std::string, std::string> Snapshot(const std::string& dir, const std::vector<std::string>& extra) {
  std::map<std::string, std::string> m;
  std::error_code ec;
  if (fs::is_directory(dir, ec))
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file()) m[it->path().generic_string()] = Slurp(it->path().string());
  for (const std::string& e : extra)
    if (fs::exists(e, ec)) m[e] = Slurp(e);
  return m;
}

std::string Diff(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b) {
  for (const auto& [k, v] : a) {
    auto it = b.find(k);
    if (it == b.end()) return k + " is missing";
    if (it->second != v) return k + " differs (" + std::to_string(v.size()) + " vs " + std::to_string(it->second.size()) + " bytes)";
  }
  for (const auto& [k, v] : b)
    if (!a.count(k)) return k + " should not exist";
  return "";
}

uint32_t MatId(const std::vector<MaterialDef>& mats, const char* name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (uint32_t)i;
  return 0;
}

// ---- editor-commands -----------------------------------------------------------------
Status GateEditorCommands(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const std::string ad = AssetDir();
  const std::string gateDir = ad + "/structures/_gate";
  const std::string root = "build/selftest_editor_cmds";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root + "/worldmap/gate/refs", ec);
  fs::create_directories(gateDir, ec);
  for (const char* ext : {".vox", ".struct.json"})
    fs::copy_file(ad + "/structures/samples/smithy" + ext, gateDir + "/edhouse" + ext,
                  fs::copy_options::overwrite_existing, ec);
  {
    refs::RefGroupFile g;
    g.group = "gate";
    refs::Ref a;
    a.id = "gate/first";
    a.kind = "marker";
    a.pos = {1, 2, 3};
    g.refs.push_back(a);
    refs::Ref b;
    b.id = "gate/existing";
    b.kind = "marker";
    b.pos = {40, 20, 40};
    b.yaw = 15;
    b.props = Json{{"tags", Json::array({"gather"})}, {"note", "kept"}, {"z", 3}};
    g.refs.push_back(b);
    std::ofstream(root + "/worldmap/gate/refs/gate.json", std::ios::binary) << refs::WriteGroup(g);
  }
  const std::vector<std::string> assetFiles{gateDir + "/edhouse.vox", gateDir + "/edhouse.struct.json"};
  const std::string refsDir = root + "/worldmap";

  // The script: every public command at least once.
  const char* lines[] = {
      "# editor-commands gate script (src/test/selftest_editor.cpp)",
      R"({"cmd":"ref.place","args":{"id":"gate/well","kind":"marker","pos":[10,20,30],"props":{"tags":["gather"]}}})",
      R"({"cmd":"ref.place","id":"newgrp/node_a","kind":"waynode","pos":[0,20,0]})",
      R"({"cmd":"ref.place","args":{"id":"newgrp/node_b","kind":"waynode","pos":[20,20,0]}})",
      R"({"cmd":"ref.link","args":{"a":"newgrp/node_a","b":"newgrp/node_b"}})",
      R"({"cmd":"ref.unlink","args":{"a":"newgrp/node_b","b":"newgrp/node_a"}})",
      R"({"cmd":"ref.link","args":{"a":"newgrp/node_b","b":"newgrp/node_a"}})",
      R"({"cmd":"ref.move","args":{"id":"gate/existing","by":[1,0,0],"yaw":90}})",
      R"({"cmd":"ref.set_prop","args":{"id":"gate/existing","key":"tags","value":null}})",
      R"({"cmd":"ref.set_prop","args":{"id":"gate/well","key":"depth","value":7}})",
      R"({"cmd":"ref.set_field","args":{"id":"gate/well","field":"base","value":"stone_well"}})",
      R"({"cmd":"ref.duplicate","args":{"id":"gate/well","as":"gate/well_2","by":[5,0,0]}})",
      R"({"cmd":"ref.delete","args":{"id":"gate/first"}})",
      R"({"cmd":"ref.place","args":{"id":"gate/house","kind":"structure","base":"_gate/edhouse","pos":[100,30,100],"yaw":90}})",
      R"({"cmd":"struct.open","args":{"ref":"gate/house"}})",
      R"({"cmd":"struct.revert","args":{}})",
      R"({"cmd":"vox.set","args":{"pos":[0,0,0],"mat":"cobble"}})",
      R"({"cmd":"vox.set_cells","args":{"cells":[[1,0,0,"timber"],[2,0,0,"air"],[3,1,0,196]]}})",
      R"({"cmd":"vox.brush","args":{"pos":[0,20,0],"radius":2,"shape":"sphere","mat":"plank"}})",
      R"({"cmd":"vox.brush","args":{"pos":[10,20,0],"radius":1,"shape":"cube","mat":"air"}})",
      R"({"cmd":"vox.box_fill","args":{"min":[-5,30,-5],"max":[5,34,5],"mat":"cobble"}})",
      R"({"cmd":"vox.box_hollow","args":{"min":[-5,30,-5],"max":[5,34,5]}})",
      R"({"cmd":"vox.box_shell","args":{"min":[-5,40,-5],"max":[0,44,0],"mat":"timber"}})",
      R"({"cmd":"vox.replace","args":{"min":[-60,-5,-40],"max":[60,70,40],"from":"plank","to":"timber"}})",
      R"({"cmd":"vox.clear","args":{"min":[8,0,8],"max":[10,2,10]}})",
      R"({"cmd":"vox.line","args":{"from":[-20,10,0],"to":[20,30,10],"mat":"timber","radius":1}})",
      R"({"cmd":"vox.copy","args":{"min":[-5,30,-5],"max":[5,34,5]}})",
      R"({"cmd":"vox.paste","args":{"pos":[-58,40,-10],"rot":1,"mirror":"x"}})",
      R"({"cmd":"vox.move","args":{"min":[-5,40,-5],"max":[0,44,0],"by":[0,3,0]}})",
      R"({"cmd":"vox.set","args":{"pos":[105,35,95],"mat":"timber","frame":"world"}})",
      R"({"cmd":"slot.add","args":{"name":"marker_x","kind":"marker","pos":[0,0,10],"props":{"tags":["x"],"box":{"min":[-1,0,9],"max":[1,2,11]}}}})",
      R"({"cmd":"slot.move","args":{"name":"marker_x","by":[1,0,0],"yaw":90}})",
      R"({"cmd":"slot.set_prop","args":{"name":"marker_x","key":"tags","value":["y"]}})",
      R"({"cmd":"slot.add","args":{"name":"waynode_tmp","kind":"waynode","pos":[4,0,4]}})",
      R"({"cmd":"slot.remove","args":{"name":"waynode_tmp"}})",
      R"({"cmd":"batch","args":{"cmds":[{"cmd":"vox.set","args":{"pos":[0,50,0],"mat":"cobble"}},{"cmd":"slot.set_prop","args":{"name":"marker_x","key":"note","value":"b"}}]}})",
      R"({"cmd":"undo"})",
      R"({"cmd":"redo"})",
      R"({"cmd":"struct.save","args":{}})",
      R"({"cmd":"vox.set","args":{"pos":[0,1,0],"mat":"cobble"}})",
      R"({"cmd":"struct.save","args":{}})",
      R"({"cmd":"struct.close","args":{}})",
  };
  std::string scriptText;
  for (const char* l : lines) scriptText += std::string(l) + "\n";
  // Coverage: every public command appears in the script.
  size_t covered = 0, total = 0;
  for (const editor::CommandDef* d : editor::Commands().All()) {
    if (d->internal) continue;
    total++;
    if (scriptText.find("\"" + d->name + "\"") != std::string::npos) covered++;
    else check(false, "the script does not exercise " + d->name);
  }
  std::ofstream(root + "/script.jsonl", std::ios::binary) << scriptText;

  const auto start = Snapshot(refsDir, assetFiles);
  refs::RefStore st;
  st.LoadMap(root, "gate");
  editor::Session es;
  es.ctx.refs = &st;
  es.ctx.assetDir = ad;
  es.ctx.mats = editor::LoadMaterialNames(ad);

  // 0. A script with a typo on line 2: refused, named, nothing changed.
  {
    std::ofstream(root + "/bad.jsonl", std::ios::binary)
        << R"({"cmd":"ref.place","args":{"id":"gate/tmp","kind":"marker","pos":[0,0,0]}})" "\n"
        << R"({"cmd":"struct.open","args":{"ref":"gate/house_missing"}})" "\n";
    editor::ScriptResult bad;
    const bool ok = editor::RunScript(es, root + "/bad.jsonl", bad, true);
    check(!ok && bad.error.find(":2: struct.open: ref:") != std::string::npos,
          "the bad script's error does not name line 2 and the field: " + bad.error);
    const std::string d0 = Diff(start, Snapshot(refsDir, assetFiles));
    check(d0.empty(), "a refused script left a change behind: " + d0);
    std::ofstream(root + "/bad2.jsonl", std::ios::binary)
        << R"({"cmd":"ref.place","args":{"id":"gate/tmp","kind":"marker","pos":[0,0,0]}})" "\n"
        << R"({"cmd":"struct.open","args":{"ref":"gate/first"}})" "\n";
    editor::ScriptResult bad2;
    editor::RunScript(es, root + "/bad2.jsonl", bad2, true);
    check(bad2.error.find("not a structure") != std::string::npos, "bad2: " + bad2.error);
  }
  // A typo'd material names the closest one.
  {
    std::string err;
    es.Run("struct.open", Json{{"asset", "_gate/edhouse"}}, &err);
    es.Run("vox.set", Json{{"pos", {0, 0, 0}}, {"mat", "cobbel"}}, &err);
    check(err.find("did you mean \"cobble\"") != std::string::npos, "no did-you-mean: " + err);
    es.Run("struct.close", Json::object(), &err);
  }

  // 1. The script.
  editor::ScriptResult res;
  const bool ran = editor::RunScript(es, root + "/script.jsonl", res, true);
  check(ran, "the script failed: " + res.error);
  const auto applied = Snapshot(refsDir, assetFiles);
  check(Diff(start, applied) != "", "the script changed nothing");
  check(fs::exists(refsDir + "/gate/refs/newgrp.json"), "newgrp.json was not created");
  // A few things the files must now say.
  {
    refs::RefStore chk;
    chk.LoadMap(root, "gate");
    const refs::Ref* e = chk.Find("gate/existing");
    check(e && e->pos.x == 41 && e->yaw == 90 && !e->props.contains("tags") && e->props.contains("note"),
          "gate/existing did not move / lose its tags");
    const refs::Ref* a = chk.Find("newgrp/node_b");
    check(a && a->props.contains("links"), "the second link was not written on node_b");
    check(chk.Find("gate/first") == nullptr && chk.Find("gate/well_2") != nullptr, "delete / duplicate");
    structures::Asset as;
    std::string err;
    std::vector<std::string> w;
    check(structures::LoadAsset(ad, "_gate/edhouse", true, as, err, w), "the saved house does not load: " + err);
    const std::string js = Slurp(gateDir + "/edhouse.struct.json");
    check(js.find("\"handEdited\": true") != std::string::npos, "handEdited is not true");
    bool slotOk = false;
    for (const structures::Slot& s : as.slots)
      if (s.name == "marker_x" && s.yaw == 90 && s.props.value("note", std::string()) == "b") slotOk = true;
    check(slotOk, "slot marker_x (moved, turned, batch prop) is not in the saved json");
  }

  // 2. Undo everything: byte-identical to the start.
  int undone = 0;
  std::string msg;
  while (es.UndoDepth() > 0) {
    if (!es.Undo(&msg)) {
      check(false, "undo failed: " + msg);
      break;
    }
    undone++;
  }
  const std::string dStart = Diff(start, Snapshot(refsDir, assetFiles));
  check(dStart.empty(), "after undo-all: " + dStart);
  // 3. Redo everything: byte-identical to the applied state.
  int redone = 0;
  while (es.RedoDepth() > 0) {
    if (!es.Redo(&msg)) {
      check(false, "redo failed: " + msg);
      break;
    }
    redone++;
  }
  const std::string dApplied = Diff(applied, Snapshot(refsDir, assetFiles));
  check(dApplied.empty(), "after redo-all: " + dApplied);

  fs::remove_all(root, ec);
  fs::remove(gateDir + "/edhouse.vox", ec);
  fs::remove(gateDir + "/edhouse.struct.json", ec);
  if (fs::is_empty(gateDir, ec)) fs::remove(gateDir, ec);
  structures::TakeReapply();
  detail = Format("%zu/%zu commands in the script, %d lines applied, %zu saved | undo %d steps -> start "
                  "byte-identical %d | redo %d -> applied byte-identical %d | bad script refused + "
                  "rolled back",
                  covered, total, res.applied, res.saved.size(), undone, dStart.empty() ? 1 : 0, redone,
                  dApplied.empty() ? 1 : 0);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- editor-struct-edit -----------------------------------------------------------------

struct BoxRead {
  std::unordered_map<uint32_t, std::vector<uint32_t>> chunks;
  GpuContext* ctx = nullptr;
  World* world = nullptr;
  uint32_t Word(IVec3 c) {
    if (!world->CellInWindow(c)) return 0xFFFFFFFFu;
    const uint32_t slot = World::SlotChunkIndex({c.x >> 4, c.y >> 4, c.z >> 4});
    auto it = chunks.find(slot);
    if (it == chunks.end()) {
      std::vector<uint32_t> w(kChunkVol);
      ReadVoxelsSync(*ctx, *world, slot, 1, w.data(), "editor gate");
      it = chunks.emplace(slot, std::move(w)).first;
    }
    return it->second[((uint32_t)(c.z & 15) * kChunk + (uint32_t)(c.y & 15)) * kChunk + (uint32_t)(c.x & 15)];
  }
  uint32_t Mat(IVec3 c) { return Word(c) & 0xFFFu; }
};

uint32_t BoxHash(BoxRead& r, IVec3 lo, IVec3 hi) {
  uint32_t h = 2166136261u;
  for (int z = lo.z; z <= hi.z; z++)
    for (int y = lo.y; y <= hi.y; y++)
      for (int x = lo.x; x <= hi.x; x++) {
        const uint32_t w = r.Word({x, y, z});
        for (int b = 0; b < 4; b++) {
          h ^= (w >> (8 * b)) & 0xFFu;
          h *= 16777619u;
        }
      }
  return h;
}

Status GateEditorStructEdit(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const std::string ad = AssetDir();
  const std::string gateDir = ad + "/structures/_gate";
  std::error_code ec;
  fs::create_directories(gateDir, ec);
  for (const char* ext : {".vox", ".struct.json"})
    fs::copy_file(ad + "/structures/samples/smithy" + ext, gateDir + "/sedit" + ext,
                  fs::copy_options::overwrite_existing, ec);
  const std::string voxBefore = Slurp(gateDir + "/sedit.vox"), jsonBefore = Slurp(gateDir + "/sedit.struct.json");

  // The transform, both ways, at every turn (pure).
  structures::Asset a0;
  {
    std::string err;
    std::vector<std::string> w;
    if (!structures::LoadAsset(ad, "_gate/sedit", true, a0, err, w)) {
      detail = "cannot load the gate house: " + err;
      return Status::Fail;
    }
  }
  int xformBad = 0;
  for (int yaw = 0; yaw < 360; yaw += 90) {
    const structures::Frame f = structures::MakeFrame(a0, {500, 100, -300}, yaw);
    for (const IVec3 h : {IVec3{0, 0, 0}, IVec3{-7, 3, 11}, IVec3{30, -2, -20}, IVec3{-60, 70, 44}}) {
      const IVec3 A = f.Cell({h.x + a0.origin.x, h.y + a0.origin.y, h.z + a0.origin.z});
      const IVec3 B = editor::HouseToWorld(h, {500, 100, -300}, yaw);
      const IVec3 C = editor::WorldToHouse(B, {500, 100, -300}, yaw);
      if (A.x != B.x || A.y != B.y || A.z != B.z || C.x != h.x || C.y != h.y || C.z != h.z) xformBad++;
    }
  }
  check(xformBad == 0, Format("editor::HouseToWorld disagrees with structures::Frame::Cell %d time(s)", xformBad));

  // A turned house on the harness pad (structure-reload's site).
  c.world.SetWindowOrigin({0, 0, 0});
  worldmap::StructurePlacement p;
  p.id = "gate/house";
  p.base = "_gate/sedit";
  p.file = "refs/gate.json";
  p.x = 180;
  p.z = 170;
  p.y = World::TerrainHeight(p.x, p.z, kDefaultSeed) + 2;
  p.yaw = 90;
  std::vector<worldmap::StructurePlacement> list{p};
  worldmap::SetStructureOverride(&list);
  biomes::EnvironmentStamp stamp;
  std::string log;
  auto restore = [&]() {
    worldmap::SetStructureOverride(nullptr);
    biomes::EnvironmentStamp s2;
    std::string l2;
    ReloadEnvironment(c.ctx, c.sim, c.mats, s2, l2);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    structures::TakeReapply();
    fs::remove(gateDir + "/sedit.vox", ec);
    fs::remove(gateDir + "/sedit.struct.json", ec);
    if (fs::is_empty(gateDir, ec)) fs::remove(gateDir, ec);
  };
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) {
    restore();
    detail = "ReloadEnvironment refused: " + log;
    return Status::Fail;
  }
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();

  const std::string root = "build/selftest_editor_sedit";
  fs::remove_all(root, ec);
  fs::create_directories(root + "/worldmap/gate/refs", ec);
  {
    refs::RefGroupFile g;
    g.group = "gate";
    refs::Ref r;
    r.id = p.id;
    r.kind = "structure";
    r.base = p.base;
    r.pos = {p.x, p.y, p.z};
    r.yaw = p.yaw;
    g.refs.push_back(r);
    std::ofstream(root + "/worldmap/gate/refs/gate.json", std::ios::binary) << refs::WriteGroup(g);
  }
  refs::RefStore st;
  st.LoadMap(root, "gate");
  const refs::Ref inst = *st.Find(p.id);
  std::vector<refs::Ref> kids0;
  structures::DeriveChildren(a0, inst, kids0, nullptr);

  editor::Session es;
  es.ctx.refs = &st;
  es.ctx.assetDir = ad;
  es.ctx.mats = editor::LoadMaterialNames(ad);
  int reloads = 0;
  es.ctx.onAssetSaved = [&](const std::string& name) {
    structures::Reload(&st, name);
    reloads++;
  };
  std::string err;
  check(es.Run("struct.open", Json{{"ref", p.id}}, &err), "struct.open: " + err);

  // The edits, in WORLD voxels, located through structures::Frame (not the
  // editor's transform): E1 fill, E2 one voxel 3 past the box's -x side (the
  // box grows, the frame re-bases), E3 an erase.
  const structures::Frame f0 = structures::MakeFrame(a0, inst.pos, inst.yaw);
  auto world = [&](IVec3 h) { return f0.Cell({h.x + a0.origin.x, h.y + a0.origin.y, h.z + a0.origin.z}); };
  auto wbox = [&](IVec3 lo, IVec3 hi) {
    const IVec3 A = world(lo), B = world(hi);
    return Json{{"min", {std::min(A.x, B.x), std::min(A.y, B.y), std::min(A.z, B.z)}},
                {"max", {std::max(A.x, B.x), std::max(A.y, B.y), std::max(A.z, B.z)}},
                {"frame", "world"}};
  };
  const IVec3 e1lo{-8, 10, 30}, e1hi{-4, 14, 33};
  const IVec3 e2{-a0.origin.x - 3, 0, 0};
  const IVec3 e3lo{10, 20, -5}, e3hi{12, 22, -3};
  Json j1 = wbox(e1lo, e1hi);
  j1["mat"] = "cobble";
  check(es.Run("vox.box_fill", j1, &err), "vox.box_fill: " + err);
  const IVec3 w2 = world(e2);
  check(es.Run("vox.set", Json{{"pos", {w2.x, w2.y, w2.z}}, {"mat", "timber"}, {"frame", "world"}}, &err),
        "vox.set: " + err);
  check(es.Run("vox.clear", wbox(e3lo, e3hi), &err), "vox.clear: " + err);
  check(es.Run("struct.save", Json::object(), &err), "struct.save: " + err);

  // The asset on disk, in its LOCAL frame.
  structures::Asset a1;
  std::vector<std::string> w;
  check(structures::LoadAsset(ad, "_gate/sedit", true, a1, err, w), "the saved asset does not load: " + err);
  std::unordered_map<uint64_t, uint16_t> v1, v0;
  for (const PrefabModel& m : a1.prefab.models)
    for (const PrefabVoxel& v : m.voxels)
      v1[editor::StructEdit::Key({m.offset.x + v.x, m.offset.y + v.y, m.offset.z + v.z})] = v.material;
  for (const PrefabModel& m : a0.prefab.models)
    for (const PrefabVoxel& v : m.voxels)
      v0[editor::StructEdit::Key({m.offset.x + v.x, m.offset.y + v.y, m.offset.z + v.z})] = v.material;
  auto at1 = [&](IVec3 h) {
    auto it = v1.find(editor::StructEdit::Key({h.x + a1.origin.x, h.y + a1.origin.y, h.z + a1.origin.z}));
    return it == v1.end() ? 0 : (int)it->second;
  };
  auto at0 = [&](IVec3 h) {
    auto it = v0.find(editor::StructEdit::Key({h.x + a0.origin.x, h.y + a0.origin.y, h.z + a0.origin.z}));
    return it == v0.end() ? 0 : (int)it->second;
  };
  const int mCobble = (int)MatId(c.mats, "cobble"), mTimber = (int)MatId(c.mats, "timber");
  check(a1.origin.x == a0.origin.x + 3 && a1.origin.y == a0.origin.y && a1.origin.z == a0.origin.z,
        Format("origin (%d,%d,%d) -> (%d,%d,%d): want x + 3 (the box grew 3 on -x)", a0.origin.x, a0.origin.y,
               a0.origin.z, a1.origin.x, a1.origin.y, a1.origin.z));
  int e1ok = 0, e1n = 0, e3ok = 0, e3n = 0, same = 0, sameN = 0;
  for (int z = e1lo.z; z <= e1hi.z; z++)
    for (int y = e1lo.y; y <= e1hi.y; y++)
      for (int x = e1lo.x; x <= e1hi.x; x++, e1n++) e1ok += at1({x, y, z}) == mCobble ? 1 : 0;
  for (int z = e3lo.z; z <= e3hi.z; z++)
    for (int y = e3lo.y; y <= e3hi.y; y++)
      for (int x = e3lo.x; x <= e3hi.x; x++, e3n++) e3ok += at1({x, y, z}) == 0 ? 1 : 0;
  for (const auto& [k, m] : v0) {   // untouched cells kept, in the new frame
    const IVec3 b = editor::StructEdit::Unkey(k);
    const IVec3 h{b.x - a0.origin.x, b.y - a0.origin.y, b.z - a0.origin.z};
    const bool inE1 = h.x >= e1lo.x && h.x <= e1hi.x && h.y >= e1lo.y && h.y <= e1hi.y && h.z >= e1lo.z && h.z <= e1hi.z;
    const bool inE3 = h.x >= e3lo.x && h.x <= e3hi.x && h.y >= e3lo.y && h.y <= e3hi.y && h.z >= e3lo.z && h.z <= e3hi.z;
    if (inE1 || inE3) continue;
    sameN++;
    same += at1(h) == (int)m ? 1 : 0;
  }
  check(e1ok == e1n, Format("fill: %d/%d cells cobble in the asset's local frame", e1ok, e1n));
  check(at1(e2) == mTimber, "the grown voxel is not timber in the asset");
  check(e3ok == e3n, Format("erase: %d/%d cells air in the asset", e3ok, e3n));
  check(same == sameN, Format("untouched voxels: %d/%d kept", same, sameN));
  (void)at0;
  check(Slurp(gateDir + "/sedit.struct.json").find("\"handEdited\": true") != std::string::npos,
        "handEdited is not true");
  // The slots moved with the re-base: every derived child is where it was.
  std::vector<refs::Ref> kids1;
  structures::DeriveChildren(a1, inst, kids1, nullptr);
  int kidBad = kids0.size() == kids1.size() ? 0 : 1;
  for (size_t i = 0; i < std::min(kids0.size(), kids1.size()); i++)
    if (refs::RefLine(kids0[i]) != refs::RefLine(kids1[i])) kidBad++;
  check(kidBad == 0, Format("%d derived slot ref(s) moved in the world after the re-base", kidBad));

  // The live re-apply: the world holds the edits, and equals a fresh worldgen.
  check(reloads == 1 && structures::TakeReapply(), "struct.save did not ask for a re-apply");
  StructureReapply rep;
  const bool okRe = ApplyStructureChanges(c.ctx, c.world, c.sim, c.stream, nullptr, c.mats, stamp, rep);
  check(okRe && rep.changed.size() == 1 && rep.chunks > 0,
        Format("re-apply: ok %d, %zu changed, %u chunks", okRe ? 1 : 0, rep.changed.size(), rep.chunks));
  c.ctx.WaitIdle();
  int wOk = 0, wN = 0;
  {
    BoxRead r{{}, &c.ctx, &c.world};
    for (int z = e1lo.z; z <= e1hi.z; z++)
      for (int y = e1lo.y; y <= e1hi.y; y++)
        for (int x = e1lo.x; x <= e1hi.x; x++, wN++) wOk += (int)r.Mat(world({x, y, z})) == mCobble ? 1 : 0;
    wN++;
    wOk += (int)r.Mat(w2) == mTimber ? 1 : 0;
    for (int z = e3lo.z; z <= e3hi.z; z++)
      for (int y = e3lo.y; y <= e3hi.y; y++)
        for (int x = e3lo.x; x <= e3hi.x; x++, wN++) wOk += r.Mat(world({x, y, z})) == 0 ? 1 : 0;
  }
  check(wOk == wN, Format("world cells after the re-apply: %d/%d hold the edit", wOk, wN));
  size_t eq = 0;
  {
    BoxRead a{{}, &c.ctx, &c.world};
    std::vector<uint32_t> ha;
    for (const auto& bx : rep.boxes) ha.push_back(BoxHash(a, bx.first, bx.second));
    c.stream.OnRegen();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    BoxRead b{{}, &c.ctx, &c.world};
    for (size_t i = 0; i < rep.boxes.size(); i++) eq += BoxHash(b, rep.boxes[i].first, rep.boxes[i].second) == ha[i] ? 1 : 0;
  }
  check(!rep.boxes.empty() && eq == rep.boxes.size(),
        Format("%zu/%zu re-applied boxes equal a fresh worldgen", eq, rep.boxes.size()));

  // Undo the save: both files back, byte for byte.
  std::string msg;
  check(es.Undo(&msg), "undo of the save: " + msg);
  const bool filesBack = Slurp(gateDir + "/sedit.vox") == voxBefore && Slurp(gateDir + "/sedit.struct.json") == jsonBefore;
  check(filesBack, "undoing the save did not restore both files byte for byte");

  fs::remove_all(root, ec);
  restore();
  detail = Format("yaw 90 house edited in world coords: fill %d/%d, grow +3 x (origin.x %d -> %d), erase %d/%d, "
                  "%d/%d untouched kept, slots unmoved %d | world %d/%d, %u chunks re-applied, %zu/%zu boxes = "
                  "fresh worldgen | save undone byte-identical %d | transform x4 ok %d",
                  e1ok, e1n, a0.origin.x, a1.origin.x, e3ok, e3n, same, sameN, kidBad == 0 ? 1 : 0, wOk, wN,
                  rep.chunks, eq, rep.boxes.size(), filesBack ? 1 : 0, xformBad == 0 ? 1 : 0);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& EditorGates() {
  static const std::vector<Gate> g = {
      {"editor-commands", "world", {}, false, GateEditorCommands, /*needsRender=*/false},
      {"editor-struct-edit", "world", {}, false, GateEditorStructEdit, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
