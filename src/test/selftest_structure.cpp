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
//   far-contain       The far field's CONTAINER RULE on the real case: the
//                     smithy's 1-voxel quench trough must read as stone, not
//                     water, at cascade levels 1-2 from all three producers
//                     (sieve, downsample, CPU patch), which agree byte for byte.
//   far-cover         The far field's COVER RULE on a real procgen crown: an
//                     empty-centre cell whose sub-samples are mostly leaf
//                     reads leaf at levels 1-4; downsample and CPU patch match
//                     the oracle exactly, the sieve (tree atlas) nearly.
//   lod-seam          The near/far HANDOFF, per material class and sky: the
//                     same pixels shaded near (render.lodHandoffDist) and far
//                     (the handoff pulled in to 8 m), far - near luma per
//                     class (leaf, bark, open ground, ground under a canopy)
//                     under noon / afternoon / dusk / overcast, against
//                     tolerances in tests/baseline.json. Draws.
//
// All run on the HARNESS map through worldmap::SetStructureOverride (the
// harness refs dir is the refs gates' fixture), both restore the environment
// and regenerate the window on the way out.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/camera.h"
#include "gpu/resources.h"
#include "measure/perfsuite.h"
#include "sim/biomes.h"
#include "sim/materials.h"
#include "sim/pass_table.h"
#include "sim/stream.h"
#include "sim/treeatlas.h"
#include "sim/weather.h"
#include "sim/worldgen_run.h"
#include "sim/worldmap.h"
#include "test/selftest.h"
#include "test/support.h"
#include "game/equipment.h"
#include "game/session.h"
#include "phys/debris.h"
#include "test/tickrig.h"
#include "world/refs.h"
#include "world/refs_doors.h"
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

  const uint32_t mDoor = MatId(c.mats, "door_wood");
  BoxRead rd{{}, &c.ctx, &c.world};
  size_t wrong = 0, intrude = 0, padBad = 0, padChecked = 0, voxels = 0;
  size_t leafBad = 0, leafCells = 0, bedBad = 0, beds = 0, linkBad = 0, doors = 0;
  std::string firstWrong, firstPadBad;
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
    // (c) the pad: columns just outside the house on each side, on its flat
    // apron (worldmap.h kStructurePadApron: level with the floor), have their
    // ground top at pos.y - 1.
    const int sx = f.siteX, sz = f.siteZ;
    const int a = std::max(worldmap::kStructurePadApron / 2, 1);
    const IVec3 cols[4] = {{hi.x + a, 0, sz}, {lo.x - a, 0, sz}, {sx, 0, hi.z + a}, {sx, 0, lo.z - a}};
    for (const IVec3& col : cols) {
      if (col.x >= lo.x && col.x <= hi.x && col.z >= lo.z && col.z <= hi.z) continue;
      padChecked++;
      const bool ok = rd.Mat({col.x, p.y - 1, col.z}) != 0 && rd.Mat({col.x, p.y, col.z}) == 0 &&
                      rd.Mat({col.x, p.y + 1, col.z}) == 0;
      if (!ok && padBad++ == 0)
        firstPadBad = Format("%s (%d,%d): mats y-1 %u, y %u, y+1 %u, y+2 %u (floor %d)", p.id.c_str(), col.x, col.z,
                             rd.Mat({col.x, p.y - 1, col.z}), rd.Mat({col.x, p.y, col.z}), rd.Mat({col.x, p.y + 1, col.z}),
                             rd.Mat({col.x, p.y + 2, col.z}), p.y);
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
        if (rd.Mat(k.pos) == 0 || rd.Mat({k.pos.x, k.pos.y + 1, k.pos.z}) != 0) bedBad++;   // P6 head cell: bed below, air above
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
        std::to_string(padBad) + "/" + std::to_string(padChecked) + " pad columns not levelled to the floor (first: " +
            firstPadBad + ")");
  check(doors > 0 && leafBad == 0,
        std::to_string(leafBad) + "/" + std::to_string(leafCells) + " door-leaf cells not door_wood");
  check(beds > 0 && bedBad == 0, std::to_string(bedBad) + "/" + std::to_string(beds) +
                                     " bed head cells not a bed under air");
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

// ---- structure-door ---------------------------------------------------------------
//
// The slots a house derives are P6's REAL kinds, through structures::
// DoorFromSlot / BedFromSlot. Two copies of the smithy (yaw 0 and yaw 90), and
// through THE TICK (TickRig, the References page's dev-use queue):
//   A. each front door's derived ref is a valid P6 door whose leaf box and
//      hinge line are exactly the transformed P2 slot's, and whose box holds
//      door_wood in the world;
//   B. a use opens it (a hinged body, the leaf cells empty), a second use
//      closes it and the box's words come back byte-identical;
//   C. the derived chest carries its authored contents and a player's use
//      opens it (the loot panel request names it); the beds satisfy P6's
//      anchor (the head cell is inside the frame).
Status GateStructureDoor(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const uint64_t idCounterWas = c.mobs.NextIdCounter();
  c.stream.Store().Clear();
  c.debris.Reset();
  c.mobs.Reset();
  const std::string ad = AssetDir();
  std::string itemName;
  for (const ItemDef& d : c.items.items)
    if (itemName.empty() && !d.name.empty()) itemName = d.name;
  // The smithy with an AUTHORED chest: a copy whose chest_0 holds 3 of an item.
  const std::string gateDir = ad + "/structures/_gate";
  std::error_code ec;
  fs::create_directories(gateDir, ec);
  fs::copy_file(ad + "/structures/samples/smithy.vox", gateDir + "/doorhouse.vox",
                fs::copy_options::overwrite_existing, ec);
  {
    std::ifstream f(ad + "/structures/samples/smithy.struct.json", std::ios::binary);
    refs::Json j = refs::Json::parse(f);
    for (refs::Json& sl : j["slots"])
      if (sl["name"] == "chest_0")
        sl["props"]["items"] = refs::Json::array({refs::Json{{"item", itemName}, {"count", 3}}});
    std::ofstream(gateDir + "/doorhouse.struct.json", std::ios::binary) << j.dump(1);
  }
  c.world.SetWindowOrigin({0, 0, 0});
  std::vector<worldmap::StructurePlacement> list;
  const int yaws[2] = {0, 90}, xs[2] = {130, 130}, zs[2] = {130, 330};
  for (int i = 0; i < 2; i++) {
    worldmap::StructurePlacement p;
    p.id = "gate/door_" + std::to_string(yaws[i]);
    p.base = "_gate/doorhouse";
    p.file = "refs/gate.json";
    p.x = xs[i];
    p.z = zs[i];
    p.y = World::TerrainHeight(p.x, p.z, kDefaultSeed) + 1;
    p.yaw = yaws[i];
    list.push_back(p);
  }
  std::string log;
  if (!Install(c, &list, log)) {
    Restore(c, savedOrigin);
    fs::remove_all(gateDir, ec);
    detail = "ReloadEnvironment refused: " + log;
    return Status::Fail;
  }
  const std::string root = ScratchRefs("door", GroupText(list, nullptr));
  refs::RefStore st;
  st.LoadMap(root, "gate");
  st.BindChunkStore(&c.stream.Store());
  const uint32_t mDoor = MatId(c.mats, "door_wood");

  uint32_t t = 15000;
  std::string doorLines;
  int cycles = 0;
  for (int i = 0; i < 2; i++) {
    const std::string id = list[i].id + "/door_front_0";
    const refs::Ref* d = st.Find(id);
    if (!check(d != nullptr && d->kind == "door", id + ": no derived door")) continue;
    // A. geometry == the transformed slot.
    const refs::DoorGeom g = refs::DoorGeometry(*d);
    IVec3 llo{}, lhi{};
    const refs::Json& L = d->props["leaf"];
    llo = {L["min"][0].get<int>(), L["min"][1].get<int>(), L["min"][2].get<int>()};
    lhi = {L["max"][0].get<int>(), L["max"][1].get<int>(), L["max"][2].get<int>()};
    const int hx = d->props["hingeLine"][0].get<int>(), hz = d->props["hingeLine"][1].get<int>();
    const bool boxSame = g.ok && g.lo.x == llo.x && g.lo.y == llo.y && g.lo.z == llo.z &&
                         g.hi.x == lhi.x && g.hi.y == lhi.y && g.hi.z == lhi.z;
    const bool hingeSame = g.ok && std::fabs(g.hinge.x - (float)hx) < 0.01f &&
                           std::fabs(g.hinge.z - (float)hz) < 0.01f;
    check(boxSame && hingeSame,
          Format("%s: P6 geometry (%s) box (%d,%d,%d)-(%d,%d,%d) hinge (%.1f,%.1f) vs slot leaf "
                 "(%d,%d,%d)-(%d,%d,%d) hinge line (%d,%d)",
                 id.c_str(), g.ok ? "ok" : g.why.c_str(), g.lo.x, g.lo.y, g.lo.z, g.hi.x, g.hi.y,
                 g.hi.z, g.hinge.x, g.hinge.z, llo.x, llo.y, llo.z, lhi.x, lhi.y, lhi.z, hx, hz));
    if (!g.ok) continue;
    const std::vector<IVec3> box = g.Cells();

    // B. through the tick.
    auto rig = std::make_unique<support::TickRig>(c, t, IVec3{d->pos.x >> 4, d->pos.y >> 4, d->pos.z >> 4});
    rig->Authority().refs = &st;
    PlayerSession& ps = rig->Session();
    const IVec3 out = {-g.face.x, 0, -g.face.z};   // the side it does NOT swing to
    auto stand = [&]() {
      ps.player.pos = Vec3{(float)d->pos.x + 0.5f + out.x * 30.0f, (float)d->pos.y + 20.0f,
                           (float)d->pos.z + 0.5f + out.z * 30.0f};
      ps.player.vel = Vec3{0, 0, 0};
    };
    stand();
    support::RunTicks(*rig, 8);
    auto boxWords = [&](std::vector<uint32_t>& o) {
      const uint32_t since = rig->tick;
      for (int k = 0; k < 40; k++) {
        bool fresh = true;
        for (const IVec3& cell : box) {
          const IVec3 wc{cell.x >> 4, cell.y >> 4, cell.z >> 4};
          const CachedChunk* cc = c.world.Cached(wc);
          if (cc == nullptr || cc->voxels.size() != kChunkVol || cc->version < since) {
            fresh = false;
            c.world.RequestChunkFetch(wc);
          }
        }
        if (fresh) break;
        stand();
        support::RunTicks(*rig, 1);
      }
      o.clear();
      for (const IVec3& cell : box) {
        const CachedChunk* cc = c.world.Cached({cell.x >> 4, cell.y >> 4, cell.z >> 4});
        const uint32_t w = cc ? cc->voxels[(size_t)(((cell.z & 15) * 16 + (cell.y & 15)) * 16 +
                                                     (cell.x & 15))]
                              : 0xFFFFFFFFu;
        o.push_back(w & 0x7F00FFFFu);
      }
    };
    auto waitPhase = [&](refs::DoorPhase want, int maxTicks) {
      refs::DoorStatus s;
      for (int k = 0; k < maxTicks; k++) {
        if (refs::DoorStatusOf(id, s) && s.phase == want) return k;
        stand();
        support::RunTicks(*rig, 1);
      }
      return -1;
    };
    std::vector<uint32_t> before, opened, after;
    boxWords(before);
    int wood = 0;
    for (uint32_t w : before) wood += (w & 0xFFFu) == mDoor ? 1 : 0;
    check(st.IsActive(id), id + ": never activated");
    check(wood == (int)box.size(), Format("%s: %d of %zu leaf cells are door_wood", id.c_str(), wood,
                                          box.size()));
    st.QueueDevUse(id);
    const int openAfter = waitPhase(refs::DoorPhase::Open, 60);
    for (int k = 0; k < 40; k++) {
      stand();
      support::RunTicks(*rig, 1);
    }
    refs::DoorStatus sO;
    refs::DoorStatusOf(id, sO);
    boxWords(opened);
    bool air = true;
    for (uint32_t w : opened) air &= (w & 0xFFFu) == 0;
    const bool body = sO.body != 0 && c.debris.HasBody(sO.body);
    check(openAfter >= 0 && body && air && sO.leafCells == (uint32_t)box.size(),
          Format("%s: open after %d ticks, body %d, cells empty %d, leaf %u (%s)", id.c_str(),
                 openAfter, body, air, sO.leafCells, sO.note.c_str()));
    st.QueueDevUse(id);
    const int closedAfter = waitPhase(refs::DoorPhase::Closed, 200);
    for (int k = 0; k < 3; k++) {
      stand();
      support::RunTicks(*rig, 1);
    }
    boxWords(after);
    const bool gone = sO.body == 0 || !c.debris.HasBody(sO.body);
    const bool exact = after == before;
    check(closedAfter >= 0 && gone && exact,
          Format("%s: closed after %d ticks, body gone %d, box byte-identical %d", id.c_str(),
                 closedAfter, gone, exact));
    if (openAfter >= 0 && body && air && closedAfter >= 0 && exact) cycles++;
    doorLines += Format("%s%s yaw %d hinge %s: open %d / close %d ticks", doorLines.empty() ? "" : "; ",
                        id.c_str(), d->yaw, d->props.value("hinge", "?").c_str(), openAfter, closedAfter);
    t = rig->tick;
  }

  // C. the chest (house 0) and the beds.
  bool chestOk = false, bedOk = true;
  {
    const std::string cid = list[0].id + "/chest_0";
    const refs::Ref* ch = st.Find(cid);
    Bag bag;
    if (ch != nullptr) refs::ContainerContents(st, *ch, &c.items, bag);
    const bool contents = ch != nullptr && ch->kind == "container" && bag.Count() == 1 &&
                          bag.slots[0].name == itemName && bag.slots[0].count == 3;
    bool opened = false;
    if (ch != nullptr) {
      support::TickRig rig(c, t, IVec3{ch->pos.x >> 4, ch->pos.y >> 4, ch->pos.z >> 4});
      rig.Authority().refs = &st;
      PlayerSession& ps = rig.Session();
      ps.player.pos = Vec3{(float)ch->pos.x + 0.5f, (float)ch->pos.y + 0.0f, (float)ch->pos.z + 8.5f};
      ps.player.vel = Vec3{0, 0, 0};
      support::RunTicks(rig, 4);
      st.ClearUses();
      rig.Authority().ui.lootRef.clear();
      support::RunTicks(rig, 1, [&](uint32_t, support::TickOps& o) {
        o.input.SetPressed(TB_USE, true);
        o.input.useRef = ch->hash;
      });
      const std::vector<refs::UseRecord>& u = st.Uses();
      opened = !u.empty() && u.back().used && u.back().id == cid &&
               rig.Authority().ui.lootRef == cid;
      check(opened, cid + ": the use did not open it: " +
                        (u.empty() ? std::string("no use recorded") : u.back().message));
      t = rig.tick;
    }
    check(contents, Format("%s: contents %d stacks (want 1 x3 %s)", cid.c_str(), bag.Count(),
                           itemName.c_str()));
    chestOk = contents && opened;
    for (const auto& p : list)
      for (const refs::Ref* k : st.ChildrenOf(p.id)) {
        if (k->kind != "bed") continue;
        refs::BedAnchor a;
        IVec3 blo, bhi;
        const refs::Json& B = k->props["box"];
        blo = {B["min"][0].get<int>(), B["min"][1].get<int>(), B["min"][2].get<int>()};
        bhi = {B["max"][0].get<int>(), B["max"][1].get<int>(), B["max"][2].get<int>()};
        const bool in = refs::BedAnchorOf(*k, a) && k->pos.x >= blo.x && k->pos.x <= bhi.x &&
                        k->pos.z >= blo.z && k->pos.z <= bhi.z && k->pos.y >= blo.y &&
                        k->pos.y <= bhi.y && a.foot.x >= blo.x && a.foot.x <= bhi.x + 1 &&
                        a.foot.z >= blo.z && a.foot.z <= bhi.z + 1;
        if (!in) bedOk = false;
        check(in, k->id + ": P6 bed anchor outside the slot's frame");
      }
  }
  bool noWarn = true;
  for (const std::string& w : st.Warnings())
    if (w.find("gate/door_") != std::string::npos) {
      noWarn = false;
      check(false, "store warning: " + w);
    }

  c.debris.Reset();
  c.mobs.Reset();
  c.mobs.SetNextIdCounter(idCounterWas);
  c.stream.Store().Clear();
  fs::remove_all(root, ec);
  fs::remove_all(gateDir, ec);
  Restore(c, savedOrigin);
  detail = Format("%d/2 doors cycled exact (%s) | chest %s | beds %s | P6 validate clean %d",
                  cycles, doorLines.c_str(), chestOk ? "authored contents + opened" : "FAIL",
                  bedOk ? "anchored in frame" : "FAIL", noWarn ? 1 : 0);
  if (!why.empty()) detail += " | FAIL: " + why[0];
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- far-contain ---------------------------------------------------------------
// THE CONTAINER RULE (worldgen.wgsl farContainedBy,
// faredits.h kContainMaxShift / StampContainers; F12 report 2026-10-07_20-46-57): past the LOD
// handoff a basin with 1-voxel walls must not read as a box of liquid. The
// fixture is the REAL case: Harrowby's smithy (whose quench trough is an 8x3x8
// pool inside a 1-voxel stone box) placed as a structure ref on the harness
// pad, shifted so the trough and a one-voxel ring sit inside ONE fine chunk
// -- x walls at chunk-local 3 and 12, z walls at 2 and 11 -- so level 1 and
// level 2 both see a wall whose cell centre is OUTSIDE air and one whose
// cell centre is the WATER, and the CPU twin (which sees one chunk) has
// everything it needs. Three producers, one answer:
//
//   S  the SIEVE and its base patch (a full refill: the stamp is pristine
//      procgen, its cells come from `far`, which centre-samples, and then
//      `farpatch` lays FarEdits::StampContainers' words over them -- the
//      rule evaluated on the CPU over the stamp template at worldgen);
//   D  the DOWNSAMPLE (an edit in the trough's chunk after that refill, so
//      `fardown` rewrites its cells from the live grid; the edit is the proof);
//   P  the CPU PATCH (the trough's chunk noted into FarEdits, then a refill:
//      `farpatch` writes Sweep's words over the sieve's).
//
// Over every level-1/2 cell of the trough box (+1 ring), with the cell's
// voxels read back and classified the way the rule does (liquid / wall /
// open):
//   (a) a cell holding a CONTAINER WALL voxel (a wall with a liquid and an open
//       face neighbour) reads a wall slot in all three -- never water, never
//       air -- and at each level some such cell has a NON-wall centre (the
//       rule fired; without it the gate would pass on centre samples alone);
//   (b) a cell whose centre is liquid and which holds no container wall reads
//       LIQUID in all three: the trough FLOOR (stone under water, stone under
//       that) is the lake-bed / puddle case and must not eat the water;
//   (c) S == D == P byte for byte.
Status GateFarContain(Ctx& c, std::string& detail) {
  refs::RegisterAllKinds();
  structures::TakeReapply();
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  const char* kBase = "harrowby_smithy";
  structures::Asset asset;
  {
    std::string err;
    std::vector<std::string> w;
    if (!structures::LoadAsset(AssetDir(), kBase, true, asset, err, w)) {
      detail = "cannot load " + std::string(kBase) + ": " + err;
      return Status::Fail;
    }
  }
  // The rule's three classes, exactly as stream.cpp feeds FarEdits.
  enum : uint8_t { kOpen = 0, kLiquid = 1, kWall = 2 };
  auto kindOf = [&](uint32_t m) -> uint8_t {
    if (m == kMatAir || m >= c.mats.size()) return kOpen;
    const MaterialDef& d = c.mats[m];
    if (d.gpu.klass == CLASS_LIQUID) return kLiquid;
    return (d.gpu.klass != CLASS_GAS && (d.gpu.flags & kMatFlagMicro) == 0) ? kWall : kOpen;
  };
  std::vector<uint8_t> slotKind(256, kOpen);
  for (size_t m = 1; m < c.mats.size(); m++)
    if (c.mats[m].farPalOwner && c.mats[m].farPalSlot < 255)
      slotKind[c.mats[m].farPalSlot] = kindOf((uint32_t)m);
  const uint32_t mWater = MatId(c.mats, "water");

  // ---- place the house so the trough lands where the claims need it ------
  c.world.SetWindowOrigin({0, 0, 0});
  auto troughBox = [&](const structures::Frame& f, IVec3& lo, IVec3& hi) {
    lo = {INT32_MAX, INT32_MAX, INT32_MAX};
    hi = {INT32_MIN, INT32_MIN, INT32_MIN};
    for (const PrefabVoxel& v : asset.prefab.models[0].voxels) {
      if (v.material != mWater) continue;
      const IVec3 w = f.Cell({v.x, v.y, v.z});
      lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
      hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
    }
    return lo.x <= hi.x;
  };
  worldmap::StructurePlacement p;
  p.id = "gate/contain_smithy";
  p.base = kBase;
  p.file = "refs/gate.json";
  p.x = 100;
  p.z = 100;
  p.y = World::TerrainHeight(p.x, p.z, kDefaultSeed) + 1;
  p.yaw = 0;
  IVec3 wlo, whi;
  if (!troughBox(structures::MakeFrame(asset, {p.x, p.y, p.z}, 0), wlo, whi)) {
    detail = std::string(kBase) + " has no water voxels (the trough moved?)";
    return Status::Fail;
  }
  // x wall (wlo.x - 1) -> local 3, z wall (wlo.z - 1) -> local 2, floor
  // (wlo.y - 1) -> local 3 (y shift kept within -8..7 so the pad stays sane).
  auto up16 = [](int want, int v) { return (((want - v) % 16) + 16) % 16; };
  p.x += up16(3, wlo.x - 1);
  p.z += up16(2, wlo.z - 1);
  int dy = up16(3, wlo.y - 1);
  if (dy > 7) dy -= 16;
  p.y += dy;
  const structures::Frame frame = structures::MakeFrame(asset, {p.x, p.y, p.z}, 0);
  troughBox(frame, wlo, whi);
  const IVec3 bLo{wlo.x - 2, wlo.y - 2, wlo.z - 2};   // walls + the one-voxel ring
  const IVec3 bHi{whi.x + 2, whi.y + 3, whi.z + 2};   // (the rim stands 2 over the water)
  const IVec3 wc{bLo.x >> 4, bLo.y >> 4, bLo.z >> 4};
  if ((bHi.x >> 4) != wc.x || (bHi.y >> 4) != wc.y || (bHi.z >> 4) != wc.z ||
      ((wlo.x - 1) & 15) != 3 || ((wlo.z - 1) & 15) != 2) {
    detail = Format("trough box (%d,%d,%d)..(%d,%d,%d) does not fit one chunk at the planned "
                    "offsets (the asset's trough changed size?)",
                    bLo.x, bLo.y, bLo.z, bHi.x, bHi.y, bHi.z);
    return Status::Fail;
  }
  std::vector<worldmap::StructurePlacement> list{p};
  std::string log;
  if (!Install(c, &list, log)) {
    Restore(c, savedOrigin);
    detail = "ReloadEnvironment refused: " + log;
    return Status::Fail;
  }

  // ---- the oracle: per cell, from the live voxels -------------------------
  BoxRead rd{{}, &c.ctx, &c.world};
  struct CellInfo {
    int level;
    IVec3 cc;
    uint8_t centreKind;
    bool hasContainer;
  };
  std::vector<CellInfo> cells;
  for (int level = 1; level <= 2; level++) {
    const int shift = (int)(level + kFarShiftBase), step = 1 << shift;
    for (int cz = bLo.z >> shift; cz <= bHi.z >> shift; cz++)
      for (int cy = bLo.y >> shift; cy <= bHi.y >> shift; cy++)
        for (int cx = bLo.x >> shift; cx <= bHi.x >> shift; cx++) {
          CellInfo ci{level, {cx, cy, cz}, kOpen, false};
          const IVec3 c0{cx << shift, cy << shift, cz << shift};
          ci.centreKind = kindOf(rd.Mat({c0.x + step / 2, c0.y + step / 2, c0.z + step / 2}));
          for (int z = 0; z < step; z++)
            for (int y = 0; y < step; y++)
              for (int x = 0; x < step; x++) {
                const IVec3 q{c0.x + x, c0.y + y, c0.z + z};
                if (kindOf(rd.Mat(q)) != kWall) continue;
                const IVec3 nb[6] = {{q.x + 1, q.y, q.z}, {q.x - 1, q.y, q.z}, {q.x, q.y + 1, q.z},
                                     {q.x, q.y - 1, q.z}, {q.x, q.y, q.z + 1}, {q.x, q.y, q.z - 1}};
                bool liq = false, open = false;
                for (const IVec3& n : nb) {
                  const uint8_t k = kindOf(rd.Mat(n));
                  liq = liq || k == kLiquid;
                  open = open || k == kOpen;
                }
                ci.hasContainer = ci.hasContainer || (liq && open);
              }
          cells.push_back(ci);
        }
  }
  auto readArm = [&]() {
    std::vector<uint32_t> b;
    for (const CellInfo& ci : cells) b.push_back(FarVoxByte(c.ctx, c.world, (uint32_t)ci.level, ci.cc));
    return b;
  };

  // ---- S: the sieve -------------------------------------------------------
  DrainFullRefill(c.ctx, c.world, c.sim, wc);
  const std::vector<uint32_t> S = readArm();
  // ---- D: the downsample ------------------------------------------------------
  // DIRECT PHASE CALLS ON PURPOSE (W2-O): this gate tests the far producers,
  // not the tick; a bare SubmitTick with one cell op is exactly what puts the
  // trough's chunk through `fardown`, the phase under test (far-downsample
  // does the same). The op is the PROOF it ran: a level-1 cell centre in this
  // chunk, above every checked cell, changed so its cell must read
  // differently from the sieve's byte -- a wall voxel carved to air, or, if
  // none stands there, glass laid on top of one (a free-floating voxel would
  // be lifted out as debris before the downsample saw it). Without it an arm
  // that silently skipped the chunk reads the sieve's bytes back and agrees
  // with everything, which is how this gate's first draft passed with the
  // rule taken out of `fardown`.
  const IVec3 cb{wc.x * 16, wc.y * 16, wc.z * 16};
  const int sh1 = (int)(1 + kFarShiftBase);
  IVec3 probe{0, 0, 0};
  uint32_t probeWord = 0;
  bool haveProbe = false;
  for (int pass = 0; pass < 2 && !haveProbe; pass++)
    for (int y = ((bHi.y >> sh1) + 1) << sh1; y < cb.y + 16 && !haveProbe; y += 1 << sh1)
      for (int z = cb.z; z < cb.z + 16 && !haveProbe; z += 1 << sh1)
        for (int x = cb.x; x < cb.x + 16 && !haveProbe; x += 1 << sh1) {
          const IVec3 q{x + (1 << (sh1 - 1)), y + (1 << (sh1 - 1)), z + (1 << (sh1 - 1))};
          const uint8_t k = kindOf(rd.Mat(q));
          if (pass == 0 && k == kWall) { probe = q; probeWord = kMatAir; haveProbe = true; }
          if (pass == 1 && k == kOpen && kindOf(rd.Mat({q.x, q.y - 1, q.z})) == kWall) {
            probe = q;
            probeWord = kMatGlass;
            haveProbe = true;
          }
        }
  const IVec3 probeCell{probe.x >> sh1, probe.y >> sh1, probe.z >> sh1};
  const uint32_t probeS = haveProbe ? FarVoxByte(c.ctx, c.world, 1, probeCell) : 0;
  for (uint32_t t = 1; t <= 3; t++) {
    std::vector<CellOp> ops;
    if (t == 1 && haveProbe) ops.push_back(CellOp{World::SlotCellIndex(probe), probeWord});
    SubmitTick(c.ctx, c.world, c.sim, t, kDefaultSeed, {}, {}, ops, false, wc, false, false);
  }
  c.ctx.WaitIdle();
  const std::vector<uint32_t> D = readArm();
  const uint32_t probeD = haveProbe ? FarVoxByte(c.ctx, c.world, 1, probeCell) : 0;
  // ---- P: the CPU patch -----------------------------------------------------
  FarEdits& edits = c.stream.Edits();
  edits.Clear();
  {
    std::vector<uint32_t> words(kChunkVol);
    ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(wc), 1, words.data(), "farContain");
    edits.NoteChunk(wc, words.data());
  }
  DrainFullRefill(c.ctx, c.world, c.sim, wc);
  const std::vector<uint32_t> P = readArm();
  const uint32_t probeP = haveProbe ? FarVoxByte(c.ctx, c.world, 1, probeCell) : 0;
  edits.Clear();

  // ---- verdict ----------------------------------------------------------------
  auto kindOfByte = [&](uint32_t b) { return b == kFarPalBlocker ? kOpen : slotKind[b & 0xFFu]; };
  int contain[3] = {0, 0, 0}, fired[3] = {0, 0, 0}, wallBad = 0, pools = 0, poolBad = 0;
  int diffSD = 0, diffSP = 0;
  std::string firstBad, firstDiff;
  for (size_t i = 0; i < cells.size(); i++) {
    const CellInfo& ci = cells[i];
    const uint32_t arms[3] = {S[i], D[i], P[i]};
    const auto where = [&]() {
      return Format("L%d cell (%d,%d,%d) centre %s: S %02x D %02x P %02x", ci.level, ci.cc.x, ci.cc.y,
                    ci.cc.z, ci.centreKind == kWall ? "wall" : ci.centreKind == kLiquid ? "liquid" : "open",
                    S[i], D[i], P[i]);
    };
    if (ci.hasContainer) {
      contain[ci.level]++;
      if (ci.centreKind != kWall) fired[ci.level]++;
      for (uint32_t b : arms)
        if (kindOfByte(b) != kWall) {
          if (wallBad++ == 0) firstBad = "container cell not a wall: " + where();
          break;
        }
    } else if (ci.centreKind == kLiquid) {
      pools++;
      for (uint32_t b : arms)
        if (kindOfByte(b) != kLiquid) {
          if (poolBad++ == 0 && firstBad.empty()) firstBad = "pool cell lost its liquid: " + where();
          break;
        }
    }
    if (S[i] != D[i] && diffSD++ == 0) firstDiff = where();
    if (P[i] != D[i] && diffSP++ == 0 && firstDiff.empty()) firstDiff = where();
  }
  check(haveProbe && probeD != probeS && probeP == probeD,
        Format("the arms did not run: probe cell (%d,%d,%d) %s, byte sieve %02x downsample %02x patch %02x",
               probeCell.x, probeCell.y, probeCell.z, haveProbe ? (probeWord ? "glassed" : "carved") : "NOT FOUND",
               probeS, probeD, probeP));
  check(contain[1] > 0 && contain[2] > 0 && fired[1] > 0 && fired[2] > 0,
        Format("vacuous: container cells L1 %d (%d off-centre), L2 %d (%d off-centre)", contain[1],
               fired[1], contain[2], fired[2]));
  check(wallBad == 0, Format("%d container cells not a wall in some arm; first %s", wallBad, firstBad.c_str()));
  check(pools > 0 && poolBad == 0,
        Format("%d/%d liquid-centred cells without a container wall lost the liquid; first %s", poolBad,
               pools, firstBad.c_str()));
  check(diffSD == 0 && diffSP == 0,
        Format("producers disagree: sieve!=downsample %d, patch!=downsample %d of %zu cells; first %s", diffSD,
               diffSP, cells.size(), firstDiff.c_str()));

  Restore(c, savedOrigin);
  detail = Format("trough chunk (%d,%d,%d): container cells L1 %d (%d off-centre) L2 %d (%d off-centre), "
                  "%d walls lost | %d pool cells, %d lost | S/D/P disagree %d/%d of %zu | probe %02x->%02x/%02x",
                  wc.x, wc.y, wc.z, contain[1], fired[1], contain[2], fired[2], wallBad, pools, poolBad, diffSD,
                  diffSP, cells.size(), probeS, probeD, probeP);
  for (const std::string& w : why) detail += " | FAIL: " + w;
  std::printf("far-contain: %s (%s)\n", why.empty() ? "PASS" : "FAIL", detail.c_str());
  return why.empty() ? Status::Pass : Status::Fail;
}

// THE FOREST FIXTURE (far-cover, lod-seam): env-truth's synthetic one-biome
// map, FOREST everywhere (flat authored ground, no sites, no pad), so the
// window is sure to hold crowns whatever the harness map paints at its origin.
// Install regenerates the window at origin (0,0,0); Restore puts the real
// environment, the window origin and its worldgen back.
struct ForestFixture {
  worldmap::WorldMapData real;
  bool Install(Ctx& c, const char* name, uint32_t contentHash, std::string& err) {
    const std::string dir = AssetDir();
    biomes::BiomeSet set;
    TreeAtlas atlas;
    std::string log;
    real = worldmap::CurrentWorldMap();
    int forest = -1;
    if (biomes::LoadBiomeSet(dir, c.mats, set, log) &&
        LoadTreeAtlas(dir + "/trees", c.mats, set, atlas, log))
      for (const biomes::BiomeDef& b : set.biomes)
        if (b.name == "forest") forest = b.index;
    if (forest < 0 || !real.Loaded()) {
      err = "no forest biome / tree atlas / world map to grow the fixture on: " + log;
      return false;
    }
    worldmap::WorldMapData syn = real;
    std::fill(syn.biome.begin(), syn.biome.end(), static_cast<uint8_t>(forest));
    std::fill(syn.height.begin(), syn.height.end(), static_cast<int16_t>(syn.terrain.baseHeight));
    syn.sites.clear();
    syn.siteIndex.assign(syn.siteIndex.size(), 0);
    syn.padX0 = syn.padZ0 = 0;
    syn.padX1 = syn.padZ1 = -1;
    syn.spawnX = syn.spawnZ = (int)kWorldN / 2;
    syn.spawnAuthored = true;
    syn.name = name;
    syn.contentHash = contentHash;
    std::vector<uint32_t> words;
    if (!worldmap::PackWorldMap(set, syn, words, log)) {
      err = "the forest map did not pack: " + log;
      return false;
    }
    worldmap::SetCurrentWorldMap(syn);
    c.ctx.WaitIdle();
    c.sim.UploadEnvironment(c.ctx.device, c.ctx.queue, atlas, words);
    c.stream.OnRegen();
    c.world.SetWindowOrigin({0, 0, 0});
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
    return true;
  }
  void Restore(Ctx& c, IVec3 savedOrigin) {
    biomes::EnvironmentStamp stamp;
    std::string rlog;
    if (!ReloadEnvironment(c.ctx, c.sim, c.mats, stamp, rlog)) worldmap::SetCurrentWorldMap(real);
    c.stream.OnRegen();
    c.world.SetWindowOrigin(savedOrigin);
    SubmitWorldgen(c.ctx, c.world, c.sim, kDefaultSeed);
    c.ctx.WaitIdle();
  }
};

// ---- far-cover -------------------------------------------------------------
// THE COVER RULE (worldgen.wgsl farCoverPick / `farcover`, faredits.h
// CoverPick; DESIGN.md "Far crowns keep their coverage"; ticket
// 2026-10-09_14-23-01): past the LOD handoff a leaf crown must keep its
// density, not become the porous lattice one centre sample per block makes
// of it. The fixture is a REAL procgen crown: the window at the origin of
// env-truth's synthetic all-FOREST map, searched for the fine chunk where the rule fires most at
// level 1. Three producers, the far-contain arrangement:
//
//   S  the SIEVE (`far` + `farcover`, a full refill: pristine procgen, the
//      rule evaluated from the tree atlas);
//   D  the DOWNSAMPLE (one leaf voxel at a level-1 cell centre turned to
//      glass -- the proof the chunk went through `fardown` -- so fardownFeat
//      re-applies the rule from the live grid);
//   P  the CPU PATCH (the chunk noted into FarEdits, then a refill).
//
// Over every level-1..4 cell inside the chunk whose centre is not far-solid
// (the only cells the rule may touch), against an oracle evaluated from the
// voxels read back (FarEdits::CoverPick on them):
//   (a) the rule fired (an empty-centre cell took leaf) at level 1 at least
//       farCover.firedL1Min times, and at level 2 at least once;
//   (b) D and P equal the oracle on every such cell (both read the live grid);
//   (c) S agrees with the oracle on at least farCover.sieveAgreeMin of the
//       cells either calls leaf: procgen asks the tree atlas, the live grid
//       holds what genCellIn put there (DESIGN.md names the differences).
// It also prints the level-1 coverage of the crown's cells -- centre rule,
// cover rule, sieve -- which is the number the ticket is about.
Status GateFarCover(Ctx& c, std::string& detail) {
  std::vector<std::string> why;
  auto check = [&](bool ok, const std::string& what) {
    if (!ok && why.size() < 8) why.push_back(what);
    return ok;
  };
  const IVec3 savedOrigin = c.world.WindowOrigin();
  ForestFixture forestMap;
  {
    std::string err;
    if (!forestMap.Install(c, "far-cover:forest", 0xC0FE7u, err)) {
      detail = err;
      return Status::Fail;
    }
  }
  FarEdits& edits = c.stream.Edits();
  edits.Clear();
  auto restore = [&]() {
    edits.Clear();
    forestMap.Restore(c, savedOrigin);
  };
  auto farSolid = [&](uint32_t m) {
    if (m == kMatAir || m >= c.mats.size()) return false;
    const MaterialDef& d = c.mats[m];
    return d.gpu.klass != CLASS_GAS && (d.gpu.flags & kMatFlagMicro) == 0;
  };
  auto slotOf = [&](uint32_t m) { return m < c.mats.size() ? c.mats[m].farPalSlot : 0u; };
  auto local = [](int x, int y, int z) {
    return ((uint32_t)z * kChunk + (uint32_t)y) * kChunk + (uint32_t)x;
  };
  // The rule's answer for the level-`level` cell at chunk-local index lc of a
  // chunk's words: -1 when the centre is far-solid (the rule may not touch
  // it), else the slot it fills (0 = stays empty).
  auto oracle = [&](const uint32_t* w, uint32_t level, IVec3 lc) -> int {
    const int shift = (int)(level + kFarShiftBase), step = 1 << shift, half = step >> 1;
    const int c0[3] = {lc.x * step, lc.y * step, lc.z * step};
    const uint32_t centre = w[local(c0[0] + half, c0[1] + half, c0[2] + half)] & 0xFFFu;
    if (farSolid(centre)) return -1;
    const uint32_t pick = edits.CoverPick(centre, level, half, c0, [&](int x, int y, int z) {
      return w[local(x, y, z)] & 0xFFFu;
    });
    return pick == centre ? 0 : (int)slotOf(pick);
  };
  auto firedL1 = [&](const uint32_t* w) {
    int n = 0;
    for (int z = 0; z < 8; z++)
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) n += oracle(w, 1, {x, y, z}) > 0 ? 1 : 0;
    return n;
  };

  // ---- A. the crown: the fine chunk where the rule fires most at level 1 ----
  IVec3 wc{0, 0, 0};
  int bestFired = 0;
  std::vector<uint32_t> before(kChunkVol);
  const int rows[] = {16, 8, 24, 4, 28, 12, 20};
  int scanned = 0;
  for (int cz : rows) {
    int hmin = INT32_MAX, hmax = INT32_MIN;
    for (int cx = 0; cx < (int)kNChunk; cx++) {
      const int h = World::TerrainHeight(cx * 16 + 8, cz * 16 + 8, kDefaultSeed);
      hmin = std::min(hmin, h);
      hmax = std::max(hmax, h);
    }
    const int cyLo = std::max(0, hmin >> 4);
    const int cyHi = std::min((int)kNChunk - 1, (hmax + 260) >> 4);
    std::vector<uint32_t> row((size_t)kNChunk * kChunkVol);
    for (int cy = cyLo; cy <= cyHi; cy++) {
      ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex({0, cy, cz}), kNChunk, row.data(),
                     "far-cover");
      for (int cx = 0; cx < (int)kNChunk; cx++) {
        const uint32_t* w = row.data() + (size_t)cx * kChunkVol;
        int cover = 0;
        for (uint32_t i = 0; i < kChunkVol; i++) cover += edits.CoverOf(w[i] & 0xFFFu) != 0 ? 1 : 0;
        scanned++;
        if (cover < 64) continue;
        const int f = firedL1(w);
        if (f > bestFired) {
          bestFired = f;
          wc = {cx, cy, cz};
          std::copy(w, w + kChunkVol, before.begin());
        }
      }
    }
    if (bestFired >= 96) break;
  }
  if (bestFired == 0) {
    restore();
    detail = Format("no crown: %d chunks scanned, none where the cover rule fires (no trees in "
                    "the harness window, or no material is farCover)", scanned);
    return Status::Fail;
  }

  // ---- the arms: every cell byte of the chunk, levels 1..4 -------------------
  const uint32_t kLevels = (uint32_t)std::max(0, FarEdits::kCoverMaxShift - (int)kFarShiftBase);
  auto readArm = [&]() {
    std::vector<std::vector<uint8_t>> lv(kLevels + 1);
    for (uint32_t level = 1; level <= kLevels; level++) {
      const int shift = (int)(level + kFarShiftBase);
      const IVec3 cc0{(wc.x * 16) >> shift, (wc.y * 16) >> shift, (wc.z * 16) >> shift};
      auto wrapv = [](int v) { return (uint32_t)(v & (int)(kFarN - 1)); };
      const uint32_t ch = ((wrapv(cc0.z) >> 4) * kFarNChunk + (wrapv(cc0.y) >> 4)) * kFarNChunk +
                          (wrapv(cc0.x) >> 4);
      const uint64_t off = (uint64_t)(level - 1) * kFarVox + (uint64_t)ch * kChunkVol;
      rhi::Buffer staging = CreateBuffer(c.ctx.device, kChunkVol,
                                         rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                         "farCoverRead");
      rhi::CommandEncoder enc = c.ctx.device.CreateCommandEncoder();
      enc.CopyBufferToBuffer(c.world.farVox, off, staging, 0, kChunkVol);
      c.ctx.queue.Submit(enc.Finish());
      lv[level].resize(kChunkVol);
      rhi::ReadBufferBlocking(c.ctx.device, staging, 0, lv[level].data(), kChunkVol);
    }
    return lv;
  };
  // The slot an arm holds at the level cell lc (chunk-local cell index).
  auto armSlot = [&](const std::vector<std::vector<uint8_t>>& lv, uint32_t level, IVec3 lc) {
    const int shift = (int)(level + kFarShiftBase);
    const IVec3 cc{((wc.x * 16) >> shift) + lc.x, ((wc.y * 16) >> shift) + lc.y,
                   ((wc.z * 16) >> shift) + lc.z};
    auto wrapv = [](int v) { return (uint32_t)(v & (int)(kFarN - 1)); };
    const uint8_t b = lv[level][((wrapv(cc.z) & 15) * kChunk + (wrapv(cc.y) & 15)) * kChunk +
                                (wrapv(cc.x) & 15)];
    return b == kFarPalBlocker ? 0u : (uint32_t)b;
  };

  // S: the sieve.
  DrainFullRefill(c.ctx, c.world, c.sim, wc);
  const auto S = readArm();
  // D: the downsample. DIRECT PHASE CALLS ON PURPOSE (W2-O): as far-contain,
  // this gate tests the far producers, not the tick; one cell op through a
  // bare SubmitTick is what puts the chunk through `fardown`. The op is the
  // proof it ran: a leaf at a level-1 cell centre becomes glass (solid for
  // solid, so no island breaks off), and that cell must then read glass.
  IVec3 probe{-1, -1, -1};
  for (int z = 0; z < 8 && probe.x < 0; z++)
    for (int y = 0; y < 8 && probe.x < 0; y++)
      for (int x = 0; x < 8 && probe.x < 0; x++)
        if (edits.CoverOf(before[local(2 * x + 1, 2 * y + 1, 2 * z + 1)] & 0xFFFu) != 0)
          probe = {x, y, z};
  const bool haveProbe = probe.x >= 0;
  const IVec3 probeVox{wc.x * 16 + 2 * probe.x + 1, wc.y * 16 + 2 * probe.y + 1,
                       wc.z * 16 + 2 * probe.z + 1};
  for (uint32_t t = 1; t <= 3; t++) {
    std::vector<CellOp> ops;
    if (t == 1 && haveProbe) ops.push_back(CellOp{World::SlotCellIndex(probeVox), kMatGlass});
    SubmitTick(c.ctx, c.world, c.sim, t, kDefaultSeed, {}, {}, ops, false, wc, false, false);
  }
  c.ctx.WaitIdle();
  const auto D = readArm();
  std::vector<uint32_t> after(kChunkVol);
  ReadVoxelsSync(c.ctx, c.world, World::SlotChunkIndex(wc), 1, after.data(), "far-cover");
  // P: the CPU patch.
  edits.Clear();
  edits.NoteChunk(wc, after.data());
  DrainFullRefill(c.ctx, c.world, c.sim, wc);
  const auto P = readArm();
  edits.Clear();

  // ---- verdict ---------------------------------------------------------------
  int fired[5] = {0, 0, 0, 0, 0}, sBoth = 0, sAgree = 0, liveBad = 0, liveCells = 0;
  std::string firstLive, firstS;
  for (uint32_t level = 1; level <= kLevels; level++) {
    const int n = 16 >> (int)(level + kFarShiftBase);
    for (int z = 0; z < n; z++)
      for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
          const IVec3 lc{x, y, z};
          const int ob = oracle(before.data(), level, lc);
          if (ob > 0) fired[level]++;
          if (ob >= 0) {
            const int s = (int)armSlot(S, level, lc);
            if (ob > 0 || s > 0) {
              sBoth++;
              if (s == ob) sAgree++;
              else if (firstS.empty())
                firstS = Format("L%u cell (%d,%d,%d): oracle %d sieve %d", level, x, y, z, ob, s);
            }
          }
          const int oa = oracle(after.data(), level, lc);
          if (oa < 0) continue;
          liveCells++;
          const int d = (int)armSlot(D, level, lc), p = (int)armSlot(P, level, lc);
          if (d != oa || p != oa) {
            if (liveBad++ == 0)
              firstLive = Format("L%u cell (%d,%d,%d): oracle %d downsample %d patch %d", level, x,
                                 y, z, oa, d, p);
          }
        }
  }
  // Level-1 coverage of the crown's cells (cells holding any leaf voxel).
  int crown = 0, centreOcc = 0, ruleOcc = 0, sieveOcc = 0;
  for (int z = 0; z < 8; z++)
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        bool any = false;
        for (int o = 0; o < 8; o++)
          any = any || edits.CoverOf(before[local(2 * x + (o & 1), 2 * y + ((o >> 1) & 1),
                                                  2 * z + ((o >> 2) & 1))] & 0xFFFu) != 0;
        if (!any) continue;
        crown++;
        const uint32_t centre = before[local(2 * x + 1, 2 * y + 1, 2 * z + 1)] & 0xFFFu;
        const bool cOcc = farSolid(centre);
        centreOcc += cOcc ? 1 : 0;
        ruleOcc += (cOcc || oracle(before.data(), 1, {x, y, z}) > 0) ? 1 : 0;
        sieveOcc += armSlot(S, 1, {x, y, z}) != 0 ? 1 : 0;
      }
  const uint32_t glassSlot = slotOf(kMatGlass);
  const uint32_t probeD = haveProbe ? armSlot(D, 1, probe) : 0;
  const uint32_t probeP = haveProbe ? armSlot(P, 1, probe) : 0;
  const double agree = sBoth > 0 ? (double)sAgree / sBoth : 0.0;
  const double agreeMin = BaselineNumber("farCover.sieveAgreeMin", 0.9);
  check(haveProbe && probeD == glassSlot && probeP == glassSlot,
        Format("the arms did not run: probe L1 cell (%d,%d,%d) downsample %u patch %u, glass is %u",
               probe.x, probe.y, probe.z, probeD, probeP, glassSlot));
  check(fired[1] >= (int)BaselineNumber("farCover.firedL1Min", 32) && fired[2] > 0,
        Format("vacuous: the rule fired at L1 %d, L2 %d times", fired[1], fired[2]));
  check(liveBad == 0, Format("%d of %d live cells disagree with the oracle; first %s", liveBad,
                             liveCells, firstLive.c_str()));
  check(agree >= agreeMin, Format("sieve agrees with the oracle on %.3f of %d leaf cells (< %.3f); "
                                  "first %s", agree, sBoth, agreeMin, firstS.c_str()));
  restore();
  detail = Format("crown chunk (%d,%d,%d): fired L1 %d L2 %d L3 %d L4 %d | live D/P vs oracle %d/%d "
                  "bad | sieve agrees %.3f of %d | L1 crown cells %d: occupied centre %.2f, rule "
                  "%.2f, sieve %.2f | probe glass %u/%u",
                  wc.x, wc.y, wc.z, fired[1], fired[2], fired[3], fired[4], liveBad, liveCells, agree,
                  sBoth, crown, crown ? (double)centreOcc / crown : 0.0,
                  crown ? (double)ruleOcc / crown : 0.0, crown ? (double)sieveOcc / crown : 0.0,
                  probeD, probeP);
  for (const std::string& w : why) detail += " | FAIL: " + w;
  std::printf("far-cover: %s (%s)\n", why.empty() ? "PASS" : "FAIL", detail.c_str());
  return why.empty() ? Status::Pass : Status::Fail;
}

// ---- lod-seam ---------------------------------------------------------------
// THE NEAR/FAR HANDOFF, MEASURED PER MATERIAL CLASS AND PER SKY (ticket
// 2026-10-09_14-23-01: "a sphere around me inside which things render more
// realistically", and the owner's note that it keeps coming back for new
// materials, biomes and lighting). The instrument the earlier passes of that
// ticket used by hand, made a gate:
//
//   THE SAME PIXELS, SHADED BOTH WAYS. The near arm draws with the shipped
//   render.lodHandoffDist (20.5 m): every pixel in the band [farHandoff + 2,
//   handoff - 1.5] m is a NEAR hit. The far arm draws the same camera with
//   the handoff pulled in to lodSeam.farHandoffM (8 m), so the very same
//   pixels are FAR hits -- the cascade's shade of the same voxels. Per-pixel
//   far - near luma IS the seam, with no "different trees on each side"
//   confound. (The far arm's band blend ends at farHandoff + 1.8 m, so the
//   band measured is the far LAW, not the blend.)
//
//   PER CLASS, by the near hit's voxel (read back): leaf (farCover
//   materials), bark (bark / wood), ground in the open, ground under a canopy
//   (leaf or bark anywhere above it in the column), plant (micro). A pixel
//   counts only where both arms hit and agree on the distance within
//   lodSeam.depthAgreeM (silhouettes the coarser cells moved are not shading).
//
//   PER SKY: noon, afternoon (sun ~20 degrees), dusk (sun just under the
//   horizon) and overcast noon -- the four lights the seam has been reported
//   under.
//
// The verdict is far - near luma (0..255) per class and sky -- the MEDIAN for
// leaf and bark, the MEAN for ground (blades included: the far field draws
// grass as cover statistics, so only an area's mean is like for like) -- for
// every class with at least lodSeam.minPx pixels, against lodSeam.maxAbsMedian
// (lodSeam.maxAbsMedian_<class>, then lodSeam.max.<sky>.<class> override it).
// Plants alone are reported, never judged. Leaf, bark and open ground at noon
// must have pixels, or the fixture is vacuous. Frames: build/lodseam_<sky>_
// {near,far}.bmp and the class mask build/lodseam_classes.bmp (view 0); every
// value is recorded. A per-class R/G/B table follows the verdict: with
// raymarch.wgsl's SEAM_DBG set (3: openness / lit / raw lambert, 12: the sun
// shadow's stages) it is the attribution of whatever the verdict says.
//
// The scene is far-cover's forest fixture (a synthetic all-forest map at the
// window origin), four headings from one clearing. The far arm costs one
// raymarch compile the first time a tuning is seen (only the shaders that
// read render.lodHandoffDist are rebuilt, and the SPIR-V cache keeps it).
Status GateLodSeam(Ctx& c, std::string& detail) {
  GpuContext& ctx = c.ctx;
  World& world = c.world;
  Simulation& sim = c.sim;
  const uint32_t W = c.width, H = c.height;
  const float aspect = (float)W / (float)H;
  const Tuning base = CurrentTuning();
  const float halfExtentM = (float)kWorldN * kVoxelMeters * 0.5f;
  const float nearHandoff = base.render.lodHandoffDist;
  const float farHandoff = (float)BaselineNumber("lodSeam.farHandoffM", 8.0);
  if (nearHandoff >= halfExtentM || farHandoff + 4.0f > nearHandoff) {
    detail = Format("no handoff to measure: render.lodHandoffDist %.1f m (window half extent %.1f, "
                    "far arm %.1f)", nearHandoff, halfExtentM, farHandoff);
    return Status::Skip;
  }
  const float bandLo = farHandoff + 2.0f, bandHi = nearHandoff - 1.5f;
  const uint32_t frames = (uint32_t)BaselineNumber("lodSeam.frames", 32);
  const double agreeM = BaselineNumber("lodSeam.depthAgreeM", 0.3);
  const size_t minPx = (size_t)BaselineNumber("lodSeam.minPx", 200);

  const IVec3 savedOrigin = world.WindowOrigin();
  ForestFixture forestMap;
  {
    std::string err;
    if (!forestMap.Install(c, "lod-seam:forest", 0x10D5EA4u, err)) {
      detail = err;
      return Status::Fail;
    }
  }
  bool farShaders = false;
  struct Restore {
    Ctx& c;
    const Tuning& base;
    ForestFixture& fx;
    IVec3 origin;
    bool& farShaders;
    ~Restore() {
      SetCurrentTuning(base);
      if (farShaders) c.sim.ReloadShaders(c.ctx.device);
      weather::SetOverride("");
      weather::Snap();
      fx.Restore(c, origin);
    }
  } restore{c, base, forestMap, savedOrigin, farShaders};

  // ---- the classes, by material ---------------------------------------------
  enum { kLeaf, kBark, kGroundOpen, kGroundCanopy, kPlant, kNCls };
  static const char* kClsName[kNCls] = {"leaf", "bark", "ground_open", "ground_canopy", "plant"};
  auto matClass = [&](uint32_t m) -> int {
    if (m == kMatAir || m >= c.mats.size()) return -1;
    const MaterialDef& d = c.mats[m];
    if ((d.gpu.flags & kMatFlagMicro) != 0) return kPlant;
    if (d.farCover) return kLeaf;
    if (d.name.find("bark") != std::string::npos || d.name.find("wood") != std::string::npos)
      return kBark;
    if (d.gpu.klass == CLASS_SOLID || d.gpu.klass == CLASS_POWDER) return kGroundOpen;
    return -1;
  };
  BoxRead box;
  box.ctx = &ctx;
  box.world = &world;
  // Leaf or bark anywhere in the column above a ground cell (from 0.4 m up).
  std::unordered_map<uint32_t, bool> canopyMemo;
  auto underCanopy = [&](IVec3 g) {
    const uint32_t key = ((uint32_t)g.x << 16) ^ (uint32_t)g.z;
    auto it = canopyMemo.find(key);
    if (it != canopyMemo.end()) return it->second;
    bool cov = false;
    for (int y = g.y + 4; y < (int)kWorldN && !cov; y++) {
      const int k = matClass(box.Mat({g.x, y, g.z}));
      cov = k == kLeaf || k == kBark;
    }
    canopyMemo.emplace(key, cov);
    return cov;
  };

  // ---- the ground under the camera: a column open to the sky near the centre
  IVec3 eyeCell{-1, -1, -1};
  for (int ring = 0; ring <= 12 && eyeCell.x < 0; ring++)
    for (int dz = -ring; dz <= ring && eyeCell.x < 0; dz++)
      for (int dx = -ring; dx <= ring && eyeCell.x < 0; dx++) {
        if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
        const int x = (int)kWorldN / 2 + dx * 6, z = (int)kWorldN / 2 + dz * 6;
        for (int y = (int)kWorldN - 1; y > 0; y--) {
          const uint32_t m = box.Mat({x, y, z});
          if (m == kMatAir) continue;
          const int k = matClass(m);
          if (k == kPlant) continue;
          if (k == kGroundOpen) eyeCell = {x, y, z};
          break;
        }
      }
  if (eyeCell.x < 0) {
    detail = "no ground open to the sky near the window centre";
    return Status::Fail;
  }
  const Vec3 eye{(float)eyeCell.x + 0.5f, (float)eyeCell.y + 18.0f, (float)eyeCell.z + 0.5f};
  const IVec3 eyeChunk{eyeCell.x >> 4, eyeCell.y >> 4, eyeCell.z >> 4};

  // ---- THE STATE THE FRAMES ARE DRAWN UNDER (attribution, printed every run) ----
  // The gate passed standalone and failed after the far / shadow-cache gates
  // (2026-10-12 integration): the near arm read ~27 luma brighter on leaves.
  // So it prints what it inherits -- the derived render caches an earlier gate
  // leaves VALID across a worldgen, every render.* row that differs from the
  // shipped tuning.json, the weather pin and the raymarch mode flags -- and
  // then resets the caches itself (below), so its verdict is order-free.
  auto countNonZero = [&](const rhi::Buffer& buf, uint64_t bytes) -> uint64_t {
    using U2 = rhi::BufferUsage;
    const uint64_t kStep = 8ull << 20;
    rhi::Buffer st = CreateBuffer(ctx.device, kStep, U2::MapRead | U2::CopyDst, "lodSeamProbe");
    std::vector<uint32_t> w(kStep / 4);
    uint64_t n = 0;
    for (uint64_t off = 0; off < bytes; off += kStep) {
      const uint64_t len = std::min(kStep, bytes - off);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      enc.CopyBufferToBuffer(buf, off, st, 0, len);
      ctx.queue.Submit(enc.Finish());
      if (!rhi::ReadBufferBlocking(ctx.device, st, 0, w.data(), len)) break;
      for (uint64_t i = 0; i < len / 4; i++) n += w[i] != 0u ? 1 : 0;
    }
    return n;
  };
  {
    Tuning disk;
    std::string renderDiff;
    if (LoadTuning(AssetDir() + "/materials/tuning.json", disk)) {
      const Tuning& cur = CurrentTuning();
      int nd = 0;
#define LS_ROW(g, m)                                                    \
  if (std::string(#g) == "render" && !(cur.g.m == disk.g.m)) {        \
    nd++;                                                              \
    renderDiff += Format(" render." #m " %g->%g", (double)disk.g.m,    \
                         (double)cur.g.m);                             \
  }
#define TP_F(g, m, n, d, lo, hi) LS_ROW(g, m)
#define TP_I(g, m, n, d, lo, hi) LS_ROW(g, m)
#define TP_U(g, m, n, d, lo, hi) LS_ROW(g, m)
#define TP_B(g, m, n, d) LS_ROW(g, m)
#define TP_S(g, m, n, d)
#define TP_V3(g, m, n, x, y, z)
#include "sim/tuning_params.def"
#undef TP_V3
#undef TP_S
#undef TP_B
#undef TP_U
#undef TP_I
#undef TP_F
#undef LS_ROW
      if (nd == 0) renderDiff = " none";
    } else {
      renderDiff = " (tuning.json did not load)";
    }
    ctx.WaitIdle();
    const uint64_t scBytes = world.shadowCache.Size();
    const uint64_t irBytes = world.irradiance.Size();
    const uint64_t scLive = countNonZero(world.shadowCache, scBytes);
    const uint64_t irLive = countNonZero(world.irradiance, irBytes);
    const uint64_t ogBytes = world.opennessGen.Size();
    const uint64_t ogLive = countNonZero(world.opennessGen, ogBytes);
    std::printf("lod-seam state: inherited shadow cache %llu of %llu words non-zero, irradiance "
                "grid %llu of %llu, openness stamps %llu of %llu; render.* vs tuning.json:%s; "
                "shadowCache %d giStrength %.2f "
                "handoff %.1f; weather '%s' autoCycle %d; shortRange %d\n",
                (unsigned long long)scLive, (unsigned long long)(scBytes / 4),
                (unsigned long long)irLive, (unsigned long long)(irBytes / 4),
                (unsigned long long)ogLive, (unsigned long long)(ogBytes / 4), renderDiff.c_str(), base.render.shadowCache, base.render.giStrength,
                base.render.lodHandoffDist, weather::Last().fromName.c_str(),
                base.weather.autoCycle ? 1 : 0, ShortRangeMode() ? 1 : 0);
  }
  // THE DERIVED CACHES START COLD. The shadow cache keys its slots on world
  // cell faces and a VALID patch survives a worldgen; since the staggered
  // refresh a valid patch is re-cast every 4th frame into a 16-sample mean, so
  // an earlier gate's (or this gate's previous sky's) verdicts keep shading
  // these cells for ~64 frames -- longer than an arm. It is zeroed before
  // EVERY arm (a fresh patch publishes its centre ray at once), so near and
  // far start from the same state. The irradiance grid is zeroed once, here,
  // BEFORE the fixture's ticks, whose openness walk refills it the way the
  // game's would. Both are render-only and disposable (selftest_render.cpp
  // ZeroShadowCache / ZeroIrradianceGrid, for gi-bounce and cave-time).
  //
  // THE OPENNESS GRID IS THE ONE THAT BIT (2026-10-12, the failing suite
  // prefix: near leaves +28 luma, ground +16, dusk too). Its bytes and per-slot
  // stamps survive a worldgen at the same window origin (far-contain and
  // far-cover regenerate at (0,0,0) too, with other worlds), and the rolling
  // refresh only re-marches a slot whose column was TOUCHED since it was last
  // walked (sim_openness.wgsl `refresh`), compared on T.tick -- and this
  // gate's ticks restart at 1 under stamps written at much larger ticks. So
  // the near arm shaded this forest with another world's sky. Cleared to the
  // cold start (all zero, world.cpp's allocation state) with the irradiance
  // grid, before the ticks whose walk rebuilds both.
  auto coldCache = [&](bool derived) {
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    enc.FillTracked(pass::Buf::ShadowCache, world.shadowCache);
    if (derived) {
      enc.FillTracked(pass::Buf::Irradiance, world.irradiance);
      enc.FillTracked(pass::Buf::Openness, world.openness);
      enc.FillTracked(pass::Buf::OpennessGen, world.opennessGen);
    }
    ctx.queue.Submit(enc.Finish());
    ctx.WaitIdle();
  };

  coldCache(true);

  // The openness grid is written by the tick's derived rows (the rolling
  // refresh covers the window in kNumChunks / opennessChunksPerFrame ticks);
  // the far cascade by a full refill.
  {
    uint32_t tick = 0;
    support::TickCursor tc(c, tick, eyeChunk);
    const int n = (int)(kNumChunks / (uint32_t)std::max(1, base.render.opennessChunksPerFrame)) + 8;
    for (int i = 0; i < n; i++) tc();
    ctx.WaitIdle();
  }
  DrainFullRefill(ctx, world, sim, eyeChunk);

  // ---- the skies ----------------------------------------------------------------
  struct Sky { const char* name; const char* preset; uint32_t tick; float sunY; };
  const uint32_t dayTicks = TicksPerDayFromTuning(base);
  auto findTick = [&](float targetY, bool below) {
    // The first tick after noon whose sun is at/under targetY.
    for (float f = 0.5f; f < 1.0f; f += 0.001f) {
      const uint32_t tk = (uint32_t)(dayTicks * f);
      const float y = SkyForTick(base, tk).sunDir[1];
      if (below ? y < targetY : y <= targetY) return tk;
    }
    return (uint32_t)(dayTicks * 0.75f);
  };
  std::vector<Sky> skies = {
      {"noon", "clear", (uint32_t)(dayTicks * 0.5f), 0.0f},
      {"afternoon", "clear", findTick(0.34f, false), 0.0f},
      {"dusk", "clear", findTick(-0.03f, true), 0.0f},
      {"overcast", "overcast", (uint32_t)(dayTicks * 0.5f), 0.0f},
  };
  for (Sky& s : skies) s.sunY = SkyForTick(base, s.tick).sunDir[1];

  // ---- one arm: `frames` frames of a view, the last read back -------------------
  using U = rhi::BufferUsage;
  rhi::Buffer shot = CreateBuffer(ctx.device, (uint64_t)W * H * 4, U::MapRead | U::CopyDst,
                                  "lodSeamShot");
  rhi::Buffer dshot = CreateBuffer(ctx.device, (uint64_t)W * H * 4, U::MapRead | U::CopyDst,
                                   "lodSeamDepth");
  auto render = [&](const Camera& cam, const Sky& sky, std::vector<uint8_t>& img,
                    std::vector<float>& dep) -> bool {
    Tuning t = CurrentTuning();
    t.weather.autoCycle = false;
    SetCurrentTuning(t);
    weather::SetOverride(sky.preset);
    weather::Snap();
    coldCache(false);
    auto copyOut = [&](const rhi::CommandEncoder& enc, const rhi::Texture& tex,
                       const rhi::Buffer& into) {
      rhi::TexelCopyTexture srcT{};
      srcT.texture = tex;
      rhi::TexelCopyBuffer dstB{};
      dstB.buffer = into;
      dstB.bytesPerRow = W * 4;
      dstB.rowsPerImage = H;
      enc.CopyTextureToBuffer(srcT, dstB, rhi::Extent3D{W, H, 1});
    };
    for (uint32_t f = 0; f < frames; f++) {
      WriteRenderParams(ctx.queue, world, eye, cam, aspect, true, 0.0f, kFarFogDensity,
                        (float)H, sky.tick);
      rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
      sim.EncodeShadowResolve(enc);
      rhi::RenderPass rp = sim.BeginRenderPass(enc, c.view, rhi::TextureFormat::RGBA8Unorm, W, H);
      sim.DrawWorld(rp);
      rp.End();
      if (f + 1 == frames) {
        copyOut(enc, c.offscreen, shot);
        copyOut(enc, sim.DepthTexture(), dshot);
      }
      ctx.queue.Submit(enc.Finish());
    }
    ctx.WaitIdle();
    img.assign((size_t)W * H * 4, 0);
    dep.assign((size_t)W * H, 0.0f);
    return rhi::ReadBufferBlocking(ctx.device, shot, 0, img.data(), img.size()) &&
           rhi::ReadBufferBlocking(ctx.device, dshot, 0, dep.data(), dep.size() * 4);
  };
  auto luma = [](const std::vector<uint8_t>& img, size_t p) -> float {
    return 0.299f * img[p * 4] + 0.587f * img[p * 4 + 1] + 0.114f * img[p * 4 + 2];
  };
  // Reversed-Z: depth = KNEAR / viewZ (common.wgsl, voxels); ray distance =
  // viewZ / (ray . forward).
  constexpr float kKnearVox = 0.4f;
  const float th = std::tan(base.camera.fovY * 0.5f);
  auto rayDir = [&](const Camera& cam, uint32_t x, uint32_t y, float ySign) {
    const float nx = ((float)x + 0.5f) / (float)W * 2.0f - 1.0f;
    const float ny = ySign * (1.0f - ((float)y + 0.5f) / (float)H * 2.0f);
    const Vec3 f = cam.Forward(), r = cam.Right(), u = cam.Up();
    Vec3 d{f.x + r.x * nx * th * aspect + u.x * ny * th, f.y + r.y * nx * th * aspect + u.y * ny * th,
           f.z + r.z * nx * th * aspect + u.z * ny * th};
    const float l = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    d.x /= l; d.y /= l; d.z /= l;
    return d;
  };
  auto rayT = [&](const Camera& cam, const Vec3& d, float depth) {
    const Vec3 f = cam.Forward();
    return kKnearVox / depth / std::max(1e-4f, d.x * f.x + d.y * f.y + d.z * f.z);
  };

  // ---- the views -------------------------------------------------------------------
  const float yaws[] = {0.4f, 2.0f, 3.6f, 5.2f};
  constexpr int kViews = 4;
  // cls: the near hit's class; g: the ground class a plant pixel also
  // counts toward (-1 for everything else).
  struct Px { uint32_t p; int cls; float t; int g; };
  std::vector<std::vector<Px>> px(kViews);
  // near luma per view x sky x selected pixel
  std::vector<std::vector<std::vector<float>>> nearL(kViews,
                                                     std::vector<std::vector<float>>(skies.size()));
  // ... and its RGB, for the per-channel table (with SEAM_DBG on in
  // raymarch.wgsl the channels ARE the shading terms: the attribution).
  std::vector<std::vector<std::vector<uint32_t>>> nearC(
      kViews, std::vector<std::vector<uint32_t>>(skies.size()));
  auto rgbAt = [](const std::vector<uint8_t>& img, size_t p) -> uint32_t {
    return (uint32_t)img[p * 4] | ((uint32_t)img[p * 4 + 1] << 8) | ((uint32_t)img[p * 4 + 2] << 16);
  };
  size_t misses = 0, probed = 0;
  float ySign = 1.0f;
  for (int v = 0; v < kViews; v++) {
    Camera cam;
    cam.yaw = yaws[v];
    cam.pitch = -0.08f;
    for (size_t s = 0; s < skies.size(); s++) {
      std::vector<uint8_t> img;
      std::vector<float> dep;
      if (!render(cam, skies[s], img, dep)) {
        detail = "near readback failed";
        return Status::Fail;
      }
      if (v == 0) WriteBmpFile(std::string("build/lodseam_") + skies[s].name + "_near.bmp", img, W, H);
      if (s == 0) {
        // The pixel's voxel: 0.3 voxel past the hit along the ray lands
        // inside the cell the ray entered. The NDC y sign is the one whose
        // probes find matter (decided on view 0).
        auto classify = [&](float sign, bool keep, size_t& miss, size_t& n) {
          for (uint32_t y = 0; y < H; y += keep ? 1 : 7)
            for (uint32_t x = 0; x < W; x += keep ? 1 : 7) {
              const size_t p = (size_t)y * W + x;
              if (dep[p] <= 1e-7f) continue;
              const Vec3 d = rayDir(cam, x, y, sign);
              const float t = rayT(cam, d, dep[p]);
              const float tm = t * kVoxelMeters;
              if (tm < bandLo || tm > bandHi) continue;
              const IVec3 cell{(int)std::floor(eye.x + d.x * (t + 0.3f)),
                               (int)std::floor(eye.y + d.y * (t + 0.3f)),
                               (int)std::floor(eye.z + d.z * (t + 0.3f))};
              const uint32_t m = box.Mat(cell);
              n++;
              int k = matClass(m);
              if (k < 0) { miss++; continue; }
              if (!keep) continue;
              if (k == kGroundOpen && underCanopy(cell)) k = kGroundCanopy;
              const int g = k == kPlant ? (underCanopy(cell) ? kGroundCanopy : kGroundOpen) : -1;
              px[v].push_back({(uint32_t)p, k, t, g});
            }
        };
        if (v == 0) {
          size_t mUp = 0, nUp = 0, mDn = 0, nDn = 0;
          classify(1.0f, false, mUp, nUp);
          classify(-1.0f, false, mDn, nDn);
          ySign = (nDn && (double)mDn / nDn < (nUp ? (double)mUp / nUp : 1.0)) ? -1.0f : 1.0f;
        }
        classify(ySign, true, misses, probed);
      }
      nearL[v][s].resize(px[v].size());
      nearC[v][s].resize(px[v].size());
      for (size_t i = 0; i < px[v].size(); i++) {
        nearL[v][s][i] = luma(img, px[v][i].p);
        nearC[v][s][i] = rgbAt(img, px[v][i].p);
      }
      if (v == 0 && s == 0) {
        // The class mask of view 0, for eyes: one colour per class.
        static const uint8_t kCol[kNCls][3] = {
            {40, 200, 40}, {150, 90, 30}, {230, 230, 120}, {60, 60, 220}, {230, 60, 230}};
        std::vector<uint8_t> mask((size_t)W * H * 4, 0);
        for (const Px& q : px[0])
          for (int ch = 0; ch < 3; ch++) mask[q.p * 4 + ch] = kCol[q.cls][ch];
        WriteBmpFile("build/lodseam_classes.bmp", mask, W, H);
      }
    }
  }

  // ---- the far arm: the same views, the handoff pulled in ------------------------
  {
    Tuning t = CurrentTuning();
    t.render.lodHandoffDist = farHandoff;
    SetCurrentTuning(t);
    farShaders = true;
    if (!sim.ReloadShaders(ctx.device)) {
      detail = "far-arm shader reload failed";
      return Status::Fail;
    }
  }
  std::vector<std::vector<std::vector<float>>> diffs(skies.size(),
                                                     std::vector<std::vector<float>>(kNCls));
  std::vector<std::vector<double>> nearSum(skies.size(), std::vector<double>(kNCls, 0.0));
  // Per sky x class: near R,G,B then far R,G,B (0..255).
  std::vector<std::vector<std::vector<std::vector<float>>>> chans(
      skies.size(), std::vector<std::vector<std::vector<float>>>(
                        kNCls, std::vector<std::vector<float>>(6)));
  size_t disagree = 0, farSky = 0;
  for (int v = 0; v < kViews; v++) {
    Camera cam;
    cam.yaw = yaws[v];
    cam.pitch = -0.08f;
    for (size_t s = 0; s < skies.size(); s++) {
      std::vector<uint8_t> img;
      std::vector<float> dep;
      if (!render(cam, skies[s], img, dep)) {
        detail = "far readback failed";
        return Status::Fail;
      }
      if (v == 0) WriteBmpFile(std::string("build/lodseam_") + skies[s].name + "_far.bmp", img, W, H);
      for (size_t i = 0; i < px[v].size(); i++) {
        const Px& q = px[v][i];
        if (dep[q.p] <= 1e-7f) { if (s == 0) farSky++; continue; }
        const uint32_t x = q.p % W, y = q.p / W;
        const float tF = rayT(cam, rayDir(cam, x, y, ySign), dep[q.p]);
        if (std::fabs(tF - q.t) * kVoxelMeters > agreeM) { if (s == 0) disagree++; continue; }
        diffs[s][q.cls].push_back(luma(img, q.p) - nearL[v][s][i]);
        nearSum[s][q.cls] += nearL[v][s][i];
        if (q.g >= 0) {
          diffs[s][q.g].push_back(luma(img, q.p) - nearL[v][s][i]);
          nearSum[s][q.g] += nearL[v][s][i];
        }
        const uint32_t nc = nearC[v][s][i], fc = rgbAt(img, q.p);
        for (int ch = 0; ch < 3; ch++) {
          chans[s][q.cls][ch].push_back((float)((nc >> (8 * ch)) & 0xFFu));
          chans[s][q.cls][3 + ch].push_back((float)((fc >> (8 * ch)) & 0xFFu));
        }
      }
    }
  }

  // ---- verdict ----------------------------------------------------------------------
  std::vector<std::string> why;
  std::string table;
  const double maxAbsAll = BaselineNumber("lodSeam.maxAbsMedian", 3.0);
  for (size_t s = 0; s < skies.size(); s++) {
    table += Format(" | %s (sun y %.2f):", skies[s].name, skies[s].sunY);
    for (int k = 0; k < kNCls; k++) {
      std::vector<float>& d = diffs[s][k];
      if (d.empty()) { table += Format(" %s -", kClsName[k]); continue; }
      // THE STATISTIC. Leaf and bark: the MEDIAN, per pixel -- a crown cell
      // and the voxels under it are the same surface, and the median shrugs
      // off the silhouettes the coarser cells move. Ground (blades included):
      // the MEAN -- the far field draws grass as cover statistics (a tint in
      // proportion to coverage, cards), not blade for blade, so only the
      // area's mean brightness is like for like. Plants alone are reported,
      // never judged: a near blade against whatever the far pixel drew there.
      double mean = 0.0;
      for (float x : d) mean += x;
      mean /= (double)d.size();
      std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
      const double medv = d[d.size() / 2];
      const bool ground = k == kGroundOpen || k == kGroundCanopy;
      const double med = ground ? mean : medv;
      const double meanNear = nearSum[s][k] / (double)d.size();
      table += Format(" %s %+.1f%s (n %zu, near %.0f)", kClsName[k], med, ground ? " mean" : "",
                      d.size(), meanNear);
      RecordObserved(Format("lodSeam.%s.%sObserved", skies[s].name, kClsName[k]).c_str(), med);
      if (k == kPlant || d.size() < minPx) continue;
      // The tolerance: lodSeam.max.<sky>.<class>, else lodSeam.maxAbsMedian_<class>,
      // else lodSeam.maxAbsMedian. Per-sky rows pin a KNOWN residual (named
      // in tests/baseline.json) so the gate still catches it growing.
      const double limCls =
          BaselineNumber(Format("lodSeam.maxAbsMedian_%s", kClsName[k]).c_str(), maxAbsAll);
      const double lim =
          BaselineNumber(Format("lodSeam.max.%s.%s", skies[s].name, kClsName[k]).c_str(), limCls);
      if (std::fabs(med) > lim)
        why.push_back(Format("%s %s %+.1f > %.1f", skies[s].name, kClsName[k], med, lim));
    }
  }
  // THE CHANNEL TABLE: median R/G/B of each class, near then far. In a
  // normal frame that is colour; with raymarch.wgsl's SEAM_DBG set it is the
  // shading terms of that debug frame (2: albedo / shadow / AO; 3: openness /
  // lit lambert / raw lambert; 4: colour / bounce / face), so one run names
  // the term a class's seam is made of.
  for (size_t s = 0; s < skies.size(); s++) {
    std::string row = Format("lod-seam channels %-9s", skies[s].name);
    for (int k = 0; k < kNCls; k++) {
      if (chans[s][k][0].empty()) continue;
      float m[6];
      double mu[6];
      for (int ch = 0; ch < 6; ch++) {
        std::vector<float>& v = chans[s][k][ch];
        mu[ch] = 0.0;
        for (float x : v) mu[ch] += x;
        mu[ch] /= (double)v.size();
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        m[ch] = v[v.size() / 2];
      }
      row += Format(" | %s median near %.0f/%.0f/%.0f far %.0f/%.0f/%.0f, mean near "
                    "%.1f/%.1f/%.1f far %.1f/%.1f/%.1f",
                    kClsName[k], m[0], m[1], m[2], m[3], m[4], m[5], mu[0], mu[1], mu[2], mu[3],
                    mu[4], mu[5]);
    }
    std::printf("%s\n", row.c_str());
  }
  for (int k : {kLeaf, kBark, kGroundOpen})
    if (diffs[0][k].size() < minPx)
      why.push_back(Format("vacuous: noon %s has %zu px (< %zu)", kClsName[k], diffs[0][k].size(), minPx));
  detail = Format("band %.1f-%.1f m, handoff %.1f vs %.1f m, eye (%d,%d,%d), %zu px probed (%zu "
                  "unclassified, y sign %+.0f), %zu far-sky, %zu depth-disagree; median far-near luma%s",
                  bandLo, bandHi, nearHandoff, farHandoff, eyeCell.x, eyeCell.y, eyeCell.z, probed,
                  misses, ySign, farSky, disagree, table.c_str());
  for (const std::string& w : why) detail += " | FAIL: " + w;
  std::printf("lod-seam: %s (%s)\n", why.empty() ? "PASS" : "FAIL", detail.c_str());
  return why.empty() ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& StructureGates() {
  static const std::vector<Gate> g = {
      {"structure-stamp", "world", {}, false, GateStructureStamp, /*needsRender=*/false},
      {"structure-reload", "world", {}, false, GateStructureReload, /*needsRender=*/false},
      {"structure-door", "world", {}, false, GateStructureDoor, /*needsRender=*/false},
      {"far-contain", "render", {}, false, GateFarContain, /*needsRender=*/false},
      {"far-cover", "render", {}, false, GateFarCover, /*needsRender=*/false},
      // Draws: the same pixels near and far, four skies (ticket 2026-10-09_14-23-01).
      {"lod-seam", "render", {}, false, GateLodSeam, /*needsRender=*/true},
  };
  return g;
}

}  // namespace selftest
