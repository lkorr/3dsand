// dungeon_refs.cpp — see dungeon_refs.h.

#include "world/dungeon_refs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "sim/worlddefaults.h"
#include "sim/worldmap.h"

namespace refs {

namespace fs = std::filesystem;

namespace {

bool ReadText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool ValidName(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  return true;
}

bool ReadIVec3(const nlohmann::json& j, IVec3& out) {
  if (!j.is_array() || j.size() != 3) return false;
  for (const auto& v : j)
    if (!v.is_number()) return false;
  out = {(int)j[0].get<double>(), (int)j[1].get<double>(), (int)j[2].get<double>()};
  return true;
}

}  // namespace

IVec3 RotateQuarter(IVec3 v, int q) {
  switch (((q % 4) + 4) % 4) {
    case 1: return {v.z, v.y, -v.x};
    case 2: return {-v.x, v.y, -v.z};
    case 3: return {-v.z, v.y, v.x};
    default: return v;
  }
}

IVec3 DungeonPlacement::ToWorld(IVec3 local) const {
  const IVec3 d{local.x - anchorLocal.x, local.y - anchorLocal.y, local.z - anchorLocal.z};
  const IVec3 r = RotateQuarter(d, Quarter());
  const IVec3 a = AnchorWorld();
  return {a.x + r.x, a.y + r.y, a.z + r.z};
}

IVec3 DungeonPlacement::ToLocal(IVec3 world) const {
  const IVec3 a = AnchorWorld();
  const IVec3 d{world.x - a.x, world.y - a.y, world.z - a.z};
  const IVec3 r = RotateQuarter(d, 4 - Quarter());
  return {anchorLocal.x + r.x, anchorLocal.y + r.y, anchorLocal.z + r.z};
}

int DungeonPlacement::YawToWorld(int localYaw) const { return (((localYaw + yaw) % 360) + 360) % 360; }
int DungeonPlacement::YawToLocal(int worldYaw) const { return (((worldYaw - yaw) % 360) + 360) % 360; }

std::vector<DungeonPlacement> ReadDungeonPlacements(const std::string& assetDir,
                                                    const std::string& mapName,
                                                    std::vector<std::string>* warn) {
  std::vector<DungeonPlacement> out;
  const std::string mapPath = assetDir + "/worldmap/" + mapName + "/map.json";
  std::string text;
  if (!ReadText(mapPath, text)) return out;
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const std::exception&) {
    return out;   // the map loader says what is wrong with map.json
  }
  if (!j.is_object() || !j.contains("dungeons")) return out;
  const auto& arr = j["dungeons"];
  auto W = [&](const std::string& w) {
    if (warn) warn->push_back("worldmap/" + mapName + "/map.json: " + w);
  };
  if (!arr.is_array()) {
    W("dungeons: not a list; no dungeon refs placed");
    return out;
  }
  for (size_t i = 0; i < arr.size(); i++) {
    const auto& e = arr[i];
    const std::string at = "dungeons[" + std::to_string(i) + "]";
    if (!e.is_object()) {
      W(at + ": not an object; skipped");
      continue;
    }
    DungeonPlacement p;
    p.id = e.value("id", std::string());
    p.name = e.value("name", p.id);
    p.dungeon = e.value("dungeon", p.id);
    if (!ValidName(p.id) || !ValidName(p.dungeon)) {
      W(at + ": id / dungeon: want [a-z0-9_-]+; skipped");
      continue;
    }
    bool dup = false;
    for (const DungeonPlacement& q : out) dup |= q.id == p.id;
    if (dup) {
      W(at + ": id \"" + p.id + "\" is used twice; the second is skipped");
      continue;
    }
    if (!e.contains("at") || !e["at"].is_array() || e["at"].size() != 2 || !e["at"][0].is_number() ||
        !e["at"][1].is_number()) {
      W(at + ": at: want [x, z] in voxels; skipped");
      continue;
    }
    p.ax = (int)e["at"][0].get<double>();
    p.az = (int)e["at"][1].get<double>();
    p.yOffset = e.contains("yOffset") && e["yOffset"].is_number() ? (int)e["yOffset"].get<double>() : 0;
    p.yaw = e.contains("yaw") && e["yaw"].is_number() ? (int)e["yaw"].get<double>() : 0;
    if (p.yaw % 90 != 0) {
      W(at + ": yaw: " + std::to_string(p.yaw) + " is not a quarter turn; 0 used");
      p.yaw = 0;
    }
    const std::string dir = assetDir + "/dungeons/" + p.dungeon;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      W(at + ": dungeon \"" + p.dungeon + "\" has no directory assets/dungeons/" + p.dungeon +
        "; not placed");
      continue;
    }
    p.refsFile = "refs.json";
    std::string dt;
    if (ReadText(dir + "/dungeon.json", dt)) {
      try {
        const nlohmann::json d = nlohmann::json::parse(dt);
        if (d.contains("refs") && d["refs"].is_string()) p.refsFile = d["refs"].get<std::string>();
        if (d.contains("anchorLocal")) ReadIVec3(d["anchorLocal"], p.anchorLocal);
      } catch (const std::exception&) {
        W(at + ": assets/dungeons/" + p.dungeon + "/dungeon.json does not parse; refs.json assumed");
      }
    }
    // The compiled shell's anchor wins: it is what the generator computed
    // from the entrance socket (docs/PLAN_dungeons.md §3.4).
    std::string st;
    if (ReadText(dir + "/shell.json", st)) {
      try {
        const nlohmann::json s = nlohmann::json::parse(st);
        if (s.contains("anchor")) ReadIVec3(s["anchor"], p.anchorLocal);
      } catch (const std::exception&) {
      }
    }
    out.push_back(std::move(p));
  }
  return out;
}

bool ResolveDungeonPlacements(std::vector<DungeonPlacement>& ps, const std::string& mapName) {
  const worldmap::WorldMapData& cur = worldmap::CurrentWorldMap();
  if (cur.name != mapName || mapName.empty()) return false;
  for (DungeonPlacement& p : ps) {
    p.groundY = worldmap::BareGroundHeight(p.ax, p.az, sandvox::kDefaultSeed);
    p.resolved = true;
  }
  return true;
}

const DungeonPlacement* PlacementOfGroup(const std::vector<DungeonPlacement>& ps,
                                         const std::string& group) {
  for (const DungeonPlacement& p : ps)
    if (p.Group() == group) return &p;
  return nullptr;
}

}  // namespace refs
