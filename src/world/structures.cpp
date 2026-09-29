// structures.cpp — see structures.h (docs/PLAN_world_editor.md P4, DESIGN.md §9f).

#include "world/structures.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "sim/worldmap.h"

namespace sandvox {
std::string AssetDir();   // test/support.cpp: the one asset-path chokepoint
}

namespace structures {

namespace fs = std::filesystem;

namespace {

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

uint32_t Fnv(uint32_t h, const std::string& s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// The worldmap packer's quarter turn, restated (worldmap.cpp RotXZ): a cell
// (x, z) of an nx x nz box -> the rotated box.
void Rot(int rot, int nx, int nz, int x, int z, int* ox, int* oz) {
  switch (rot & 3) {
    case 0: *ox = x;          *oz = z;          break;
    case 1: *ox = nz - 1 - z; *oz = x;          break;
    case 2: *ox = nx - 1 - x; *oz = nz - 1 - z; break;
    default:*ox = z;          *oz = nx - 1 - x; break;
  }
}
// The same turn for cell CORNERS (0..nx, 0..nz): a cell [x, x+1] maps to the
// rotated cell's corners, so the "- 1" goes.
void RotCorner(int rot, int nx, int nz, int x, int z, int* ox, int* oz) {
  switch (rot & 3) {
    case 0: *ox = x;      *oz = z;      break;
    case 1: *ox = nz - z; *oz = x;      break;
    case 2: *ox = nx - x; *oz = nz - z; break;
    default:*ox = z;      *oz = nx - x; break;
  }
}

bool ReadIVec3(const Json& j, IVec3& v) {
  if (!j.is_array() || j.size() != 3) return false;
  for (const Json& e : j)
    if (!e.is_number()) return false;
  v = {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
  return true;
}

// materials.json names in id order (id = index + 1; 0 is air, implicit):
// what the LIVE engine will call each id.
std::vector<std::string> LiveMaterialNames(const std::string& assetDir) {
  std::vector<std::string> names{"air"};
  std::string text;
  if (!ReadFile(assetDir + "/materials/materials.json", text)) return names;
  try {
    const Json j = Json::parse(text);
    if (j.contains("materials") && j["materials"].is_array())
      for (const Json& m : j["materials"])
        if (m.is_object() && m.contains("id") && m["id"].is_string())
          names.push_back(m["id"].get<std::string>());
  } catch (...) {
  }
  return names;
}

std::string& ReapplyWhy() {
  static std::string w;
  return w;
}
bool& ReapplyFlag() {
  static bool f = false;
  return f;
}

}  // namespace

// ---- assets -------------------------------------------------------------------

bool AssetExists(const std::string& assetDir, const std::string& name) {
  if (name.empty() || name.find("..") != std::string::npos) return false;
  std::error_code ec;
  return fs::is_regular_file(assetDir + "/structures/" + name + ".struct.json", ec);
}

std::vector<std::string> ListAssets(const std::string& assetDir) {
  std::vector<std::string> out;
  const fs::path root = fs::path(assetDir) / "structures";
  std::error_code ec;
  if (!fs::is_directory(root, ec)) return out;
  const std::string suffix = ".struct.json";
  for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 1) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file()) continue;
    std::string rel = fs::relative(it->path(), root, ec).generic_string();
    if (rel.size() <= suffix.size() || rel.compare(rel.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    rel.resize(rel.size() - suffix.size());
    if (!rel.empty() && rel[0] == '_') continue;   // scratch (the gates' _gate/)
    out.push_back(rel);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool LoadAsset(const std::string& assetDir, const std::string& name, bool withVoxels, Asset& out,
               std::string& err, std::vector<std::string>& warn) {
  out = Asset{};
  out.name = name;
  if (name.empty()) {
    err = "names no structure (a path under assets/structures, e.g. \"samples/smithy\")";
    return false;
  }
  const std::string jsonPath = assetDir + "/structures/" + name + ".struct.json";
  const std::string label = "structures/" + name + ".struct.json";
  std::string text;
  if (!ReadFile(jsonPath, text)) {
    err = "assets/" + label + " does not exist";
    return false;
  }
  Json j;
  try {
    j = Json::parse(text);
  } catch (const std::exception& e) {
    err = "assets/" + label + " is not valid JSON (" + e.what() + ")";
    return false;
  }
  if (!j.is_object() || !j.contains("origin") || !ReadIVec3(j["origin"], out.origin)) {
    err = "assets/" + label + ": origin: must be [x, y, z] (local cells)";
    return false;
  }
  if (j.contains("size")) ReadIVec3(j["size"], out.size);
  out.voxelsPerMetre = j.value("voxelsPerMetre", 0);
  out.handEdited = j.value("handEdited", false);
  if (j.contains("slots") && j["slots"].is_array()) {
    int i = 0;
    for (const Json& js : j["slots"]) {
      const std::string at = label + ": slots[" + std::to_string(i++) + "]";
      Slot sl;
      if (!js.is_object() || !js.contains("name") || !js["name"].is_string() ||
          !js.contains("kind") || !js["kind"].is_string() || !js.contains("pos") ||
          !ReadIVec3(js["pos"], sl.pos)) {
        warn.push_back(at + ": needs name, kind and pos [x, y, z]; skipped");
        continue;
      }
      sl.name = js["name"].get<std::string>();
      sl.kind = js["kind"].get<std::string>();
      std::string why;
      if (!refs::ValidId("x/" + sl.name, &why) || sl.name.find('/') != std::string::npos) {
        warn.push_back(at + ": name \"" + sl.name + "\": " +
                       (why.empty() ? std::string("may not contain '/'") : why) + "; skipped");
        continue;
      }
      sl.yaw = js.contains("yaw") && js["yaw"].is_number() ? js["yaw"].get<int>() : 0;
      if (js.contains("props") && js["props"].is_object()) sl.props = js["props"];
      out.slots.push_back(std::move(sl));
    }
  }
  out.contentHash = Fnv(2166136261u, text);
  if (!withVoxels) return true;

  // ---- the voxels -------------------------------------------------------------
  const std::string voxPath = assetDir + "/structures/" + name + ".vox";
  std::string voxBytes;
  if (!ReadFile(voxPath, voxBytes)) {
    err = "assets/structures/" + name + ".vox does not exist";
    return false;
  }
  out.contentHash = Fnv(out.contentHash, voxBytes);
  const std::vector<std::string> live = LiveMaterialNames(assetDir);
  Prefab pf;
  std::string verr, vwarn;
  if (!LoadVoxFromMemory(reinterpret_cast<const uint8_t*>(voxBytes.data()), voxBytes.size(),
                         live.size(), pf, verr, vwarn)) {
    err = "assets/structures/" + name + ".vox did not load: " + verr;
    return false;
  }
  // REMAP BY NAME (DESIGN.md §9e "materials"): the json records which id each
  // name had when the .vox was written; a materials.json renumbered since
  // then would otherwise paint the house in whatever now sits at those ids.
  std::map<int, int> remap;   // written id -> live id (-1 = drop)
  if (j.contains("materials") && j["materials"].is_object()) {
    for (auto it = j["materials"].begin(); it != j["materials"].end(); ++it) {
      if (!it.value().is_number_integer()) continue;
      const int written = it.value().get<int>();
      int liveId = -1;
      for (size_t k = 1; k < live.size(); k++)
        if (live[k] == it.key()) liveId = static_cast<int>(k);
      if (liveId < 0) {
        warn.push_back(label + ": materials: \"" + it.key() +
                       "\" is not in materials.json any more; its voxels are left out");
      } else if (liveId != written) {
        warn.push_back(label + ": materials: \"" + it.key() + "\" was id " +
                       std::to_string(written) + " when the .vox was written and is " +
                       std::to_string(liveId) + " now; remapped by name");
      }
      remap[written] = liveId;
    }
  } else {
    warn.push_back(label + ": materials: missing, so a renumbered materials.json cannot be "
                   "detected; the .vox ids are used as they are");
  }
  // The occupied box (P2: "the local frame is the .vox's OCCUPIED box") and
  // the voxels rebased onto it, as ONE model.
  IVec3 lo{INT_MAX, INT_MAX, INT_MAX}, hi{INT_MIN, INT_MIN, INT_MIN};
  for (const PrefabModel& m : pf.models)
    for (const PrefabVoxel& v : m.voxels) {
      const int x = m.offset.x + v.x, y = m.offset.y + v.y, z = m.offset.z + v.z;
      lo = {std::min(lo.x, x), std::min(lo.y, y), std::min(lo.z, z)};
      hi = {std::max(hi.x, x), std::max(hi.y, y), std::max(hi.z, z)};
    }
  if (lo.x > hi.x) {
    err = "assets/structures/" + name + ".vox has no voxels";
    return false;
  }
  PrefabModel one;
  one.name = "structure";
  one.size = {hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
  size_t unnamed = 0;
  for (const PrefabModel& m : pf.models)
    for (const PrefabVoxel& v : m.voxels) {
      PrefabVoxel w = v;
      w.x = static_cast<int16_t>(m.offset.x + v.x - lo.x);
      w.y = static_cast<int16_t>(m.offset.y + v.y - lo.y);
      w.z = static_cast<int16_t>(m.offset.z + v.z - lo.z);
      if (!remap.empty()) {
        auto r = remap.find(v.material);
        if (r == remap.end()) {
          unnamed++;
        } else {
          if (r->second < 0) continue;
          w.material = static_cast<uint16_t>(r->second);
        }
      }
      w.color = 0;   // world voxels carry no art palette
      one.voxels.push_back(w);
    }
  if (unnamed > 0)
    warn.push_back(label + ": materials: " + std::to_string(unnamed) +
                   " voxels use an id the map does not name; used as they are");
  const IVec3 sz = one.size;
  if ((out.size.x | out.size.y | out.size.z) != 0 &&
      (out.size.x != sz.x || out.size.y != sz.y || out.size.z != sz.z))
    warn.push_back(label + ": size: says [" + std::to_string(out.size.x) + "," +
                   std::to_string(out.size.y) + "," + std::to_string(out.size.z) +
                   "] but the .vox occupies [" + std::to_string(sz.x) + "," + std::to_string(sz.y) +
                   "," + std::to_string(sz.z) + "]; the .vox wins (origin and slots are read in its frame)");
  out.size = sz;
  out.prefab = Prefab{};
  out.prefab.name = name;
  out.prefab.size = sz;
  out.prefab.models.push_back(std::move(one));
  out.hasVoxels = true;
  return true;
}

// ---- the transform ----------------------------------------------------------------

bool ValidYaw(int yaw) { return yaw % 90 == 0; }

int YawToRot(int yaw) {
  const int q = (((yaw / 90) % 4) + 4) % 4;   // quarter turns toward +X
  return (4 - q) & 3;                          // RotXZ turns the other way
}

Frame MakeFrame(const Asset& a, IVec3 pos, int yaw) {
  Frame f;
  f.rot = YawToRot(yaw);
  f.nx0 = std::max(1, a.size.x);
  f.ny = std::max(1, a.size.y);
  f.nz0 = std::max(1, a.size.z);
  const bool swap = (f.rot & 1) != 0;
  f.nx = swap ? f.nz0 : f.nx0;
  f.nz = swap ? f.nx0 : f.nz0;
  f.origin = a.origin;
  f.pos = pos;
  f.yaw = ((yaw % 360) + 360) % 360;
  int ox, oz;
  Rot(f.rot, f.nx0, f.nz0, a.origin.x, a.origin.z, &ox, &oz);
  // worldgen.wgsl wmStampCell: world x of rotated column c = siteX - nx/2 + c.
  f.siteX = pos.x - ox + f.nx / 2;
  f.siteZ = pos.z - oz + f.nz / 2;
  f.sink = a.origin.y;
  return f;
}

IVec3 Frame::Cell(IVec3 l) const {
  int rx, rz;
  Rot(rot, nx0, nz0, l.x, l.z, &rx, &rz);
  return {siteX - nx / 2 + rx, pos.y - origin.y + l.y, siteZ - nz / 2 + rz};
}

void Frame::Corner(int cx, int cz, int* wx, int* wz) const {
  int ox, oz;
  RotCorner(rot, nx0, nz0, cx, cz, &ox, &oz);
  *wx = siteX - nx / 2 + ox;
  *wz = siteZ - nz / 2 + oz;
}

int Frame::Yaw(int localYaw) const { return (((localYaw + yaw) % 360) + 360) % 360; }

void Frame::Box(IVec3& lo, IVec3& hi) const {
  const IVec3 a = Cell({0, 0, 0});
  const IVec3 b = Cell({nx0 - 1, ny - 1, nz0 - 1});
  lo = {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
  hi = {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

void DeriveChildren(const Asset& a, const refs::Ref& inst, std::vector<refs::Ref>& out) {
  const Frame f = MakeFrame(a, inst.pos, inst.yaw);
  for (const Slot& sl : a.slots) {
    refs::Ref c;
    c.id = inst.id + "/" + sl.name;
    c.kind = sl.kind;
    c.pos = f.Cell(sl.pos);
    c.yaw = f.Yaw(sl.yaw);
    c.group = inst.group;
    c.derivedFrom = inst.id;
    c.hash = refs::RefHash(c.id);
    Json p = Json::object();
    for (auto it = sl.props.begin(); it != sl.props.end(); ++it) {
      const Json& v = it.value();
      IVec3 mn, mx;
      if (v.is_object() && v.contains("min") && v.contains("max") && ReadIVec3(v["min"], mn) &&
          ReadIVec3(v["max"], mx)) {
        // An inclusive cell box: both corners through the cell transform.
        const IVec3 A = f.Cell(mn), B = f.Cell(mx);
        Json b = v;
        b["min"] = Json::array({std::min(A.x, B.x), std::min(A.y, B.y), std::min(A.z, B.z)});
        b["max"] = Json::array({std::max(A.x, B.x), std::max(A.y, B.y), std::max(A.z, B.z)});
        p[it.key()] = b;
      } else if (it.key() == "hingeLine" && v.is_array() && v.size() == 2 && v[0].is_number() &&
                 v[1].is_number()) {
        int wx, wz;
        f.Corner(v[0].get<int>(), v[1].get<int>(), &wx, &wz);
        p[it.key()] = Json::array({wx, wz});
      } else if (it.key() == "links" && v.is_array()) {
        Json l = Json::array();
        for (const Json& e : v) l.push_back(e.is_string() ? Json(inst.id + "/" + e.get<std::string>()) : e);
        p[it.key()] = l;
      } else {
        p[it.key()] = v;
      }
    }
    p["slot"] = sl.name;
    p["structure"] = inst.id;
    c.props = std::move(p);
    out.push_back(std::move(c));
  }
}

// ---- the map side ---------------------------------------------------------------

void ReadPlacements(const std::string& assetDir, const std::string& mapName,
                    std::vector<worldmap::StructurePlacement>& out,
                    std::vector<std::string>& warn) {
  out.clear();
  const fs::path dir = fs::path(assetDir) / "worldmap" / mapName / "refs";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> files;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  std::map<std::string, worldmap::StructurePlacement> byId;
  for (const fs::path& p : files) {
    std::string text;
    const std::string label = "refs/" + p.filename().string();
    if (!ReadFile(p.string(), text)) continue;   // RefStore says so, by name
    refs::RefGroupFile g;
    std::vector<std::string> ignored;   // the RefStore reports parse problems
    if (!refs::ParseGroup(text, label, g, ignored)) continue;
    for (const refs::Ref& r : g.refs) {
      if (r.kind != "structure") continue;
      if (byId.count(r.id)) continue;   // ids are map-wide; RefStore warns
      worldmap::StructurePlacement sp;
      sp.id = r.id;
      sp.base = r.base;
      sp.file = label;
      sp.x = r.pos.x;
      sp.y = r.pos.y;
      sp.z = r.pos.z;
      sp.yaw = r.yaw;
      if (r.props.contains("padMargin") && r.props["padMargin"].is_number_integer())
        sp.padMargin = r.props["padMargin"].get<int>();
      byId[r.id] = sp;
    }
  }
  for (auto& [id, sp] : byId) out.push_back(sp);   // id order
  (void)warn;
}

// ---- live re-apply ----------------------------------------------------------------

bool SiteExists(const std::string& id) {
  for (const worldmap::WorldMapData::StampSite& s : worldmap::CurrentWorldMap().sites)
    if (s.structure && s.id == id) return true;
  return false;
}

bool SiteInSync(const refs::Ref& r) {
  const bool placeable = ValidYaw(r.yaw) && AssetExists(sandvox::AssetDir(), r.base);
  for (const worldmap::WorldMapData::StampSite& s : worldmap::CurrentWorldMap().sites) {
    if (!s.structure || s.id != r.id) continue;
    return placeable && s.base == r.base && s.refX == r.pos.x && s.refY == r.pos.y &&
           s.refZ == r.pos.z && s.refYaw == r.yaw;
  }
  return !placeable;
}

void RequestReapply(const std::string& why) {
  if (!ReapplyFlag()) ReapplyWhy() = why;
  else if (ReapplyWhy().size() < 200) ReapplyWhy() += "; " + why;
  ReapplyFlag() = true;
}

bool TakeReapply(std::string* why) {
  if (!ReapplyFlag()) return false;
  if (why) *why = ReapplyWhy();
  ReapplyFlag() = false;
  ReapplyWhy().clear();
  return true;
}

void Reload(refs::RefStore* store, const std::string& name) {
  if (store != nullptr) store->RefreshDerived();
  RequestReapply("structure.reload " + name);
}

std::vector<std::string> UsedBy(const std::string& assetDir, const std::string& name) {
  std::vector<std::string> out;
  const fs::path maps = fs::path(assetDir) / "worldmap";
  std::error_code ec;
  if (!fs::is_directory(maps, ec)) return out;
  std::vector<fs::path> dirs;
  for (const fs::directory_entry& e : fs::directory_iterator(maps, ec))
    if (e.is_directory()) dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const fs::path& d : dirs) {
    std::vector<worldmap::StructurePlacement> ps;
    std::vector<std::string> w;
    ReadPlacements(assetDir, d.filename().string(), ps, w);
    for (const worldmap::StructurePlacement& p : ps)
      if (p.base == name) out.push_back(d.filename().string() + ": " + p.id);
  }
  return out;
}

// ---- the kind -----------------------------------------------------------------------

void RegisterStructureKinds() {
  refs::RefKind k;
  k.name = "structure";
  k.validate = [](const refs::Ref& r, std::vector<std::string>& problems) {
    if (r.base.empty())
      problems.push_back("base: names no structure (pick one under assets/structures, e.g. "
                         "\"samples/smithy\"); nothing is placed");
    else if (!AssetExists(sandvox::AssetDir(), r.base))
      problems.push_back("base: assets/structures/" + r.base +
                         ".struct.json does not exist; nothing is placed");
    if (!ValidYaw(r.yaw))
      problems.push_back("yaw: " + std::to_string(r.yaw) +
                         " is not a quarter turn (0, 90, 180, 270); structures only turn in "
                         "90-degree steps, so it is not placed");
    if (r.props.contains("padMargin")) {
      const refs::Json& m = r.props["padMargin"];
      if (!m.is_number_integer() || m.get<int>() < 1 || m.get<int>() > 64)
        problems.push_back("props.padMargin: must be a whole number of voxels 1..64 (the ramp "
                           "from the levelled pad back to the terrain)");
    }
  };
  k.derive = [](const refs::Ref& r, std::vector<refs::Ref>& children,
                std::vector<std::string>& problems) {
    if (r.base.empty() || !ValidYaw(r.yaw)) return;   // validate said so
    Asset a;
    std::string err;
    std::vector<std::string> warn;
    if (!LoadAsset(sandvox::AssetDir(), r.base, false, a, err, warn)) return;
    for (const std::string& w : warn) problems.push_back("base: " + w);
    DeriveChildren(a, r, children);
  };
  // The voxels are worldgen's (fact 1): activation writes nothing. What it
  // does is notice an edit worldgen has not seen yet -- a placed, moved,
  // turned or re-based house -- and ask the frame loop to re-apply.
  k.activate = [](refs::RefCtx&, const refs::Ref& r) {
    if (!SiteInSync(r)) RequestReapply("structure " + r.id + " changed");
    return refs::Activation::Done;
  };
  k.deactivate = [](refs::RefCtx&, const refs::Ref& r, refs::RefEvent why) {
    if (why == refs::RefEvent::Deleted && SiteExists(r.id))
      RequestReapply("structure " + r.id + " deleted");
  };
  // Placed / edited / deleted ANYWHERE -- a house a kilometre off is in the
  // far field, so its edit must not wait for it to be activated.
  k.onEdit = [](const refs::Ref* before, const refs::Ref* after) {
    const std::string id = after ? after->id : before ? before->id : std::string();
    RequestReapply("structure " + id + (after == nullptr ? " deleted" : before == nullptr ? " placed" : " edited"));
  };
  refs::Kinds().Register(std::move(k));
}

}  // namespace structures
