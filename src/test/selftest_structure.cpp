// selftest_structure.cpp — STRUCTURE INSTANCES (docs/PLAN_world_editor.md P4,
// world/structures.h, DESIGN.md §9f).
//
//   structure-stamp   A sample house placed by a `structure` ref appears
//                     EXACTLY: every voxel of the asset at its transformed
//                     cell, at all four quarter turns (one placement each, on
//                     floors raised / level / cut against the terrain); the
//                     house's air above GRADE is air (no ground inside it);
//                     the pad is levelled to the ref's floor (ground top =
//                     pos.y - 1 around it); the derived child refs land on the
//                     right world cells (every door slot's leaf box is all
//                     door_wood, every bed slot stands on straw_bed); a
//                     yaw-45 ref is refused with a warning naming it; the
//                     RefStore derives the children and never writes them;
//                     and a second worldgen reproduces the boxes word for word.
//                     SANDVOX_STRUCTURE_SHOT=<file.bmp> also renders one house.
//   structure-reload  The live re-apply (sandvox::ApplyStructureChanges): an
//                     asset swapped on disk, then the ref moved and turned --
//                     each time only the changed boxes are regenerated through
//                     the streamer, and the result equals a from-scratch
//                     worldgen of the new state word for word; the RefStore
//                     re-derives the slots (structures::Reload).
//
// Both run on the HARNESS map through worldmap::SetStructureOverride (the
// harness refs dir is the refs gates' fixture), both restore the environment
// and regenerate the window on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "measure/perfsuite.h"
#include "sim/biomes.h"
#include "sim/stream.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "world/refs.h"
#include "world/structures.h"

using namespace sandvox;

namespace selftest {
namespace {

namespace fs = std::filesystem;

// Resident voxels of a box, read chunk by chunk through the CPU seam.
struct BoxRead {
  std::unordered_map<uint32_t, std::vector<uint32_t>> chunks;   // slot -> words
  GpuContext* ctx = nullptr;
  World* world = nullptr;
  uint32_t lastSlot = 0xFFFFFFFFu;
  const std::vector<uint32_t>* last = nullptr;
  uint32_t Word(IVec3 c) {
    if (!world->CellInWindow(c)) return 0xFFFFFFFFu;
    const uint32_t slot = World::SlotChunkIndex({c.x >> 4, c.y >> 4, c.z >> 4});
    if (slot != lastSlot) {
      auto it = chunks.find(slot);
      if (it == chunks.end()) {
        std::vector<uint32_t> w(kChunkVol);
        ReadVoxelsSync(*ctx, *world, slot, 1, w.data(), "structure gate");
        it = chunks.emplace(slot, std::move(w)).first;
      }
      lastSlot = slot;
      last = &it->second;
    }
    const uint32_t lx = (uint32_t)(c.x & 15), ly = (uint32_t)(c.y & 15), lz = (uint32_t)(c.z & 15);
    return (*last)[(lz * kChunk + ly) * kChunk + lx];
  }
  uint32_t Mat(IVec3 c) { return Word(c) & 0xFFFu; }
};

uint32_t MatId(const std::vector<MaterialDef>& mats, const char* name) {
  for (size_t i = 0; i < mats.size(); i++)
    if (mats[i].name == name) return (uint32_t)i;
  return 0;
}

// FNV of every word in a box (the reproducibility / equality key).
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

// Install the environment with `list` as the map's structure refs, regenerate
// the window at the harness origin.
bool Install(Ctx& c, const std::vector<worldmap::StructurePlacement>* list, std::string& log) {
  worldmap::SetStructureOverride(list);
  biomes::EnvironmentStamp stamp;
  if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log)) return false;
  c.stream.OnRegen();
  c.world.SetWindowOrigin({0, 0, 0});
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  return true;
}

void Restore(Ctx& c, IVec3 origin) {
  worldmap::SetStructureOverride(nullptr);
  biomes::EnvironmentStamp stamp;
  std::string log;
  ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, log);
  c.stream.OnRegen();
  c.world.SetWindowOrigin(origin);
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  structures::TakeReapply();
}

std::string ScratchRefs(const char* name, const std::string& groupText) {
  const std::string root = std::string("build/selftest_structure_") + name;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root + "/worldmap/gate/refs", ec);
  std::ofstream(root + "/worldmap/gate/refs/gate.json", std::ios::binary) << groupText;
  return root;
}

std::string GroupText(const std::vector<worldmap::StructurePlacement>& ps, const char* extra) {
  refs::RefGroupFile g;
  g.group = "gate";
  for (const worldmap::StructurePlacement& p : ps) {
    refs::Ref r;
    r.id = p.id;
    r.kind = "structure";
    r.base = p.base;
    r.pos = {p.x, p.y, p.z};
    r.yaw = p.yaw;
    g.refs.push_back(r);
  }
  std::string s = refs::WriteGroup(g);
  if (extra && *extra) {
    // One marker, so an unrelated edit can be made (and the file compared).
    refs::RefGroupFile g2;
    std::vector<std::string> w;
    refs::ParseGroup(s, "gate.json", g2, w);
    refs::Ref m;
    m.id = extra;
    m.kind = "marker";
    m.pos = {100, 170, 100};
    g2.refs.push_back(m);
    s = refs::WriteGroup(g2);
  }
  return s;
}

// ---- structure-stamp -----------------------------------------------------------
Status GateStructureStamp(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const std::string ad = AssetDir();
  const char* kBase = "samples/smithy";
  structures::Asset asset;
  {
    std::string err;
    std::vector<std::string> w;
    if (!structures::LoadAsset(ad, kBase, true, asset, err, w)) {
      detail = "cannot load " + std::string(kBase) + ": " + err;
      return Status::Fail;
    }
  }
  // Four placements, one per quarter turn, on the harness pad's calm ground
  // (a 0.1 m-rounded site each, well inside the window, clear of the lake).
  // Floors: level, raised 4 (the pad FILLS), cut 3 (the pad CUTS), level.
  c.world.SetWindowOrigin({0, 0, 0});
  const int xs[4] = {100, 250, 100, 250}, zs[4] = {100, 100, 250, 250};
  const int yaws[4] = {0, 90, 180, 270}, dy[4] = {1, 5, -2, 1};
  std::vector<worldmap::StructurePlacement> list;
  for (int i = 0; i < 4; i++) {
    worldmap::StructurePlacement p;
    p.id = "gate/house_" + std::to_string(yaws[i]);
    p.base = kBase;
    p.file = "refs/gate.json";
    p.x = xs[i];
    p.z = zs[i];
    p.y = World::TerrainHeight(xs[i], zs[i], kDefaultSeed) + dy[i];
    p.yaw = yaws[i];
    list.push_back(p);
  }
  worldmap::StructurePlacement bad = list[0];
  bad.id = "gate/bad_yaw";
  bad.x = 400;
  bad.z = 120;
  bad.yaw = 45;
  std::vector<worldmap::StructurePlacement> withBad = list;
  withBad.push_back(bad);
  std::string log;
  if (!Install(c, &withBad, log)) {
    Restore(c, savedOrigin);
    detail = "ReloadEnvironment refused: " + log;
    return Status::Fail;
  }
  // The refused yaw: a warning naming the ref, and no site.
  bool badWarned = false;
  for (const std::string& w : worldmap::CurrentWorldMap().warnings)
    if (w.find("gate/bad_yaw") != std::string::npos && w.find("quarter turn") != std::string::npos)
      badWarned = true;
  check(badWarned && !structures::SiteExists("gate/bad_yaw"),
        "yaw 45 was not refused with a warning naming gate/bad_yaw");
  int nSites = 0;
  for (const auto& s : worldmap::CurrentWorldMap().sites) nSites += s.structure ? 1 : 0;
  check(nSites == 4, "want 4 structure sites, have " + std::to_string(nSites));

  const uint32_t mDoor = MatId(c.mats, "door_wood"), mBed = MatId(c.mats, "straw_bed");
  BoxRead rd{{}, &c.ctx, &c.world};
  size_t wrong = 0, intrude = 0, padBad = 0, padChecked = 0, voxels = 0;
  size_t leafBad = 0, leafCells = 0, bedBad = 0, beds = 0, linkBad = 0, doors = 0;
  std::string firstWrong;
  uint32_t hash1 = 2166136261u;
  std::vector<std::pair<IVec3, IVec3>> boxes;
  for (int i = 0; i < 4; i++) {
    const worldmap::StructurePlacement& p = list[i];
    const structures::Frame f = structures::MakeFrame(asset, {p.x, p.y, p.z}, p.yaw);
    IVec3 lo, hi;
    f.Box(lo, hi);
    boxes.push_back({lo, hi});
    // (a) every asset voxel, (b) the air above GRADE.
    std::vector<uint16_t> grid((size_t)f.nx0 * f.ny * f.nz0, 0);
    for (const PrefabVoxel& v : asset.prefab.models[0].voxels)
      grid[((size_t)v.z * f.ny + v.y) * f.nx0 + v.x] = v.material;
    for (int z = 0; z < f.nz0; z++)
      for (int y = 0; y < f.ny; y++)
        for (int x = 0; x < f.nx0; x++) {
          const uint16_t want = grid[((size_t)z * f.ny + y) * f.nx0 + x];
          const IVec3 w = f.Cell({x, y, z});
          const uint32_t have = rd.Mat(w);
          if (want != 0) {
            voxels++;
            if (have != want) {
              if (wrong++ == 0)
                firstWrong = p.id + " local (" + std::to_string(x) + "," + std::to_string(y) + "," +
                             std::to_string(z) + ") world (" + std::to_string(w.x) + "," +
                             std::to_string(w.y) + "," + std::to_string(w.z) + "): want " +
                             std::to_string(want) + " have " + std::to_string(have);
            }
          } else if (y >= asset.origin.y && have != 0) {
            intrude++;
          }
        }
    // (c) the pad: columns just outside the house on each side, inside the
    // levelled footprint, have their ground top at pos.y - 1.
    const int sx = f.siteX, sz = f.siteZ;
    const int r = std::max(f.nx, f.nz) / 2 + 1;
    const IVec3 cols[4] = {{sx + r, 0, sz}, {sx - r, 0, sz}, {sx, 0, sz + r}, {sx, 0, sz - r}};
    for (const IVec3& col : cols) {
      if (col.x >= lo.x && col.x <= hi.x && col.z >= lo.z && col.z <= hi.z) continue;
      padChecked++;
      const bool ok = rd.Mat({col.x, p.y - 1, col.z}) != 0 && rd.Mat({col.x, p.y, col.z}) == 0 &&
                      rd.Mat({col.x, p.y + 1, col.z}) == 0;
      if (!ok) padBad++;
    }
    // (d) the child refs, from the same derivation the RefStore runs.
    refs::Ref inst;
    inst.id = p.id;
    inst.kind = "structure";
    inst.base = p.base;
    inst.pos = {p.x, p.y, p.z};
    inst.yaw = p.yaw;
    std::vector<refs::Ref> kids;
    structures::DeriveChildren(asset, inst, kids);
    std::map<std::string, const refs::Ref*> byId;
    for (const refs::Ref& k : kids) byId[k.id] = &k;
    for (const refs::Ref& k : kids) {
      if (k.kind == "door" && k.props.contains("leaf")) {
        doors++;
        const refs::Json& L = k.props["leaf"];
        for (int z = L["min"][2].get<int>(); z <= L["max"][2].get<int>(); z++)
          for (int y = L["min"][1].get<int>(); y <= L["max"][1].get<int>(); y++)
            for (int x = L["min"][0].get<int>(); x <= L["max"][0].get<int>(); x++) {
              leafCells++;
              if (rd.Mat({x, y, z}) != mDoor) leafBad++;
            }
      }
      if (k.kind == "bed") {
        beds++;
        if (rd.Mat(k.pos) != 0 || rd.Mat({k.pos.x, k.pos.y - 1, k.pos.z}) != mBed) bedBad++;
      }
      if (k.props.contains("links"))
        for (const refs::Json& l : k.props["links"])
          if (!l.is_string() || !byId.count(l.get<std::string>())) linkBad++;
    }
    hash1 = (hash1 ^ BoxHash(rd, lo, hi)) * 16777619u;
  }
  check(wrong == 0, std::to_string(wrong) + " of " + std::to_string(voxels) +
                        " asset voxels wrong; first " + firstWrong);
  check(intrude == 0, std::to_string(intrude) + " non-air cells inside the houses above grade");
  check(padChecked > 0 && padBad == 0,
        std::to_string(padBad) + "/" + std::to_string(padChecked) + " pad columns not levelled to the floor");
  check(doors > 0 && leafBad == 0,
        std::to_string(leafBad) + "/" + std::to_string(leafCells) + " door-leaf cells not door_wood");
  check(beds > 0 && bedBad == 0, std::to_string(bedBad) + "/" + std::to_string(beds) +
                                     " bed slots not standing on straw_bed");
  check(linkBad == 0, std::to_string(linkBad) + " waynode links name no child");

  // (e) THE REFSTORE: the four houses as a group file -> children derived,
  // never written; a slot cannot be edited; an unrelated edit leaves the
  // file free of them.
  bool storeOk = false;
  size_t storeKids = 0;
  {
    const std::string root = ScratchRefs("stamp", GroupText(list, "gate/well"));
    refs::RefStore st;
    st.LoadMap(root, "gate");
    for (const auto& p : list) storeKids += st.ChildrenOf(p.id).size();
    std::string err;
    const std::string kid = list[0].id + "/door_front_0";
    const bool moveRefused = st.Find(kid) != nullptr && !refs::Move(st, kid, {0, 0, 0}, 0, &err);
    refs::SetProp(st, "gate/well", "tags", refs::Json::array({"water"}), &err);
    std::ifstream f(st.GroupPath("gate"), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    const bool fileClean = ss.str().find("door_front_0") == std::string::npos &&
                           ss.str().find("\"water\"") != std::string::npos;
    // ...and the worldgen side reads the SAME file to the same placements.
    std::vector<worldmap::StructurePlacement> read;
    std::vector<std::string> rw;
    structures::ReadPlacements(root, "gate", read, rw);
    bool readOk = read.size() == list.size();
    for (size_t i = 0; readOk && i < read.size(); i++) {
      const auto it = std::find_if(list.begin(), list.end(),
                                   [&](const worldmap::StructurePlacement& q) { return q.id == read[i].id; });
      readOk = it != list.end() && it->x == read[i].x && it->y == read[i].y && it->z == read[i].z &&
               it->yaw == read[i].yaw && it->base == read[i].base;
    }
    check(readOk, "ReadPlacements: " + std::to_string(read.size()) + " placements read back");
    storeOk = storeKids == 4 * asset.slots.size() && moveRefused && fileClean && readOk;
    check(storeOk, "RefStore: " + std::to_string(storeKids) + " children (want " +
                       std::to_string(4 * asset.slots.size()) + "), slot edit refused " +
                       std::to_string(moveRefused) + ", file clean " + std::to_string(fileClean));
    std::error_code ec;
    fs::remove_all(root, ec);
    structures::TakeReapply();   // the SetProp above is not a structure edit, but be tidy
  }

  // (f) REPRODUCIBLE: a second worldgen of the same state, the same words.
  c.stream.OnRegen();
  SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
  c.ctx.WaitIdle();
  BoxRead rd2{{}, &c.ctx, &c.world};
  uint32_t hash2 = 2166136261u;
  for (const auto& [lo, hi] : boxes) hash2 = (hash2 ^ BoxHash(rd2, lo, hi)) * 16777619u;
  check(hash1 == hash2, "a second worldgen differs in the house boxes");

  // SANDVOX_STRUCTURE_SHOT=<file.bmp>: one look at the yaw-0 house.
  std::string shot;
  if (const char* sp = std::getenv("SANDVOX_STRUCTURE_SHOT"); sp && *sp) {
    shot = sp;
    RefillFarAround(c.ctx, c.world, c.sim, WindowCentreChunk(c.world));
    const worldmap::StructurePlacement& p = list[0];
    const Vec3 at{(float)p.x + 0.5f, (float)p.y + 25.0f, (float)p.z + 0.5f};
    const Vec3 eye = at + Vec3{110.0f, 70.0f, 150.0f};
    const Vec3 d = at - eye;
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    Camera cam;
    cam.yaw = std::atan2(d.z, d.x);
    cam.pitch = std::asin(d.y / len);
    const uint32_t tick = (uint32_t)(0.40 * (double)TicksPerDay(CurrentTuning()));
    for (int fr = 0; fr < 4; fr++) {
      WriteRenderParams(c.ctx.queue, c.world, eye, cam, (float)c.width / c.height, true, 11.7f,
                        kFarFogDensity, (float)c.height, tick);
      rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
      c.sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = c.sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm,
                                                 c.width, c.height);
      c.sim.DrawWorld(rp);
      rp.End();
      c.ctx.queue.Submit(enc.Finish());
    }
    c.ctx.WaitIdle();
    c.Grab(shot.c_str());
  }

  Restore(c, savedOrigin);
  detail = Format(
      "4 turns of %s: %zu/%zu voxels right, %zu intrusions, pad %zu/%zu level | %zu doors "
      "(%zu leaf cells) %zu bad, %zu beds %zu bad, %zu bad links | yaw 45 refused %d | store "
      "%zu children, clean %d | reproducible %d%s%s",
      kBase, voxels - wrong, voxels, intrude, padChecked - padBad, padChecked, doors, leafCells,
      leafBad, beds, bedBad, linkBad, badWarned ? 1 : 0, storeKids, storeOk ? 1 : 0,
      hash1 == hash2 ? 1 : 0, shot.empty() ? "" : " | shot ", shot.c_str());
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- structure-reload ------------------------------------------------------------
Status GateStructureReload(Ctx& c, std::string& detail) {
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
  auto use = [&](const char* sample) {
    for (const char* ext : {".vox", ".struct.json"})
      fs::copy_file(ad + "/structures/samples/" + sample + ext, gateDir + "/house" + ext,
                    fs::copy_options::overwrite_existing, ec);
  };
  use("smithy");
  c.world.SetWindowOrigin({0, 0, 0});
  worldmap::StructurePlacement p;
  p.id = "gate/house";
  p.base = "_gate/house";
  p.file = "refs/gate.json";
  p.x = 180;
  p.z = 170;
  p.y = World::TerrainHeight(p.x, p.z, kDefaultSeed) + 2;
  p.yaw = 90;
  std::vector<worldmap::StructurePlacement> list{p};
  std::string log;
  if (!Install(c, &list, log)) {
    Restore(c, savedOrigin);
    fs::remove_all(gateDir, ec);
    detail = "ReloadEnvironment refused: " + log;
    return Status::Fail;
  }
  // The RefStore half: one ref, its slots derived from the asset as it is.
  const std::string root = ScratchRefs("reload", GroupText(list, nullptr));
  refs::RefStore st;
  st.LoadMap(root, "gate");
  const size_t kidsBefore = st.ChildrenOf(p.id).size();

  // Compare the partial re-apply against a from-scratch worldgen of the
  // SAME state, over every box the re-apply said it touched WIDENED by a
  // margin on every side (more below: the cavern band follows the ground) --
  // so a change that leaked out of the regenerated box shows as a mismatch
  // instead of passing unseen.
  auto widen = [&](std::pair<IVec3, IVec3> b) {
    const IVec3 o = c.world.WindowOrigin();
    const int w0 = 0, w1 = (int)kWorldN - 1;
    auto cl = [&](int v, int base) { return std::clamp(v, base * (int)kChunk + w0, base * (int)kChunk + w1); };
    b.first = {cl(b.first.x - 48, o.x), cl(b.first.y - 96, o.y), cl(b.first.z - 48, o.z)};
    b.second = {cl(b.second.x + 48, o.x), cl(b.second.y + 32, o.y), cl(b.second.z + 48, o.z)};
    return b;
  };
  auto compare = [&](const StructureReapply& rep, const char* step) {
    BoxRead a{{}, &c.ctx, &c.world};
    std::vector<uint32_t> ha;
    for (const auto& bx : rep.boxes) {
      const auto w = widen(bx);
      ha.push_back(BoxHash(a, w.first, w.second));
    }
    c.stream.OnRegen();
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    BoxRead b{{}, &c.ctx, &c.world};
    size_t same = 0;
    for (size_t i = 0; i < rep.boxes.size(); i++) {
      const auto w = widen(rep.boxes[i]);
      same += BoxHash(b, w.first, w.second) == ha[i] ? 1 : 0;
    }
    check(!rep.boxes.empty() && same == rep.boxes.size(),
          std::string(step) + ": " + std::to_string(same) + "/" + std::to_string(rep.boxes.size()) +
              " boxes equal a fresh worldgen");
    return same == rep.boxes.size() && !rep.boxes.empty();
  };
  biomes::EnvironmentStamp stamp;

  // 1. THE ASSET CHANGES on disk (smithy -> alehouse, a bigger house with
  //    other slots): structure.reload re-derives, the re-apply regenerates.
  use("alehouse");
  structures::Reload(&st, "_gate/house");
  const bool asked = structures::TakeReapply();
  const size_t kidsAfter = st.ChildrenOf(p.id).size();
  structures::Asset ale;
  {
    std::string err;
    std::vector<std::string> w;
    structures::LoadAsset(ad, "_gate/house", false, ale, err, w);
  }
  check(asked, "structures::Reload did not request a re-apply");
  check(kidsAfter == ale.slots.size() && kidsAfter != kidsBefore,
        "slots re-derived: " + std::to_string(kidsBefore) + " -> " + std::to_string(kidsAfter) +
            " (want " + std::to_string(ale.slots.size()) + ")");
  StructureReapply r1;
  const bool ok1 = ApplyStructureChanges(c.ctx, c.world, c.sim, c.stream, nullptr, c.mats, stamp, r1);
  check(ok1 && r1.changed.size() == 1 && r1.chunks > 0,
        "asset re-apply: ok " + std::to_string(ok1) + ", " + std::to_string(r1.changed.size()) +
            " changed, " + std::to_string(r1.chunks) + " chunks");
  const bool eq1 = ok1 && compare(r1, "asset swap");

  // 2. THE REF MOVES AND TURNS (the References page's move / +90): the old
  //    box must return to terrain, the new one gain the house.
  list[0].x = 300;
  list[0].z = 260;
  list[0].y = World::TerrainHeight(300, 260, kDefaultSeed) + 1;
  list[0].yaw = 180;
  worldmap::SetStructureOverride(&list);
  StructureReapply r2;
  const bool ok2 = ApplyStructureChanges(c.ctx, c.world, c.sim, c.stream, nullptr, c.mats, stamp, r2);
  check(ok2 && r2.changed.size() == 1 && r2.boxes.size() == 2,
        "move re-apply: " + std::to_string(r2.changed.size()) + " changed, " +
            std::to_string(r2.boxes.size()) + " boxes");
  const bool eq2 = ok2 && compare(r2, "move+turn");

  // 3. NOTHING CHANGED: a re-apply finds nothing to do and touches nothing.
  StructureReapply r3;
  ApplyStructureChanges(c.ctx, c.world, c.sim, c.stream, nullptr, c.mats, stamp, r3);
  check(r3.changed.empty() && r3.chunks == 0, "an idle re-apply regenerated " +
                                                  std::to_string(r3.chunks) + " chunks");

  fs::remove_all(root, ec);
  fs::remove_all(gateDir, ec);
  Restore(c, savedOrigin);
  detail = Format(
      "asset swap: slots %zu -> %zu, %u chunks regenerated, equal to fresh %d | move+turn: %u "
      "chunks, %zu boxes, equal %d | idle re-apply %u chunks",
      kidsBefore, kidsAfter, r1.chunks, eq1 ? 1 : 0, r2.chunks, r2.boxes.size(), eq2 ? 1 : 0,
      r3.chunks);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& StructureGates() {
  static const std::vector<Gate> g = {
      {"structure-stamp", "world", {}, false, GateStructureStamp, /*needsRender=*/false},
      {"structure-reload", "world", {}, false, GateStructureReload, /*needsRender=*/false},
  };
  return g;
}

}  // namespace selftest
