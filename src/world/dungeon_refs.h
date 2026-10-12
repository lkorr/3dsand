// dungeon_refs.h — A DUNGEON'S REF GROUP, placed in the world
// (docs/PLAN_dungeons.md §2.1 / §3.1 / §3.7, package D3).
//
// A dungeon is authored in its OWN LOCAL FRAME (voxels) and placed on the map
// by `map.json` `dungeons[]` {id, dungeon, at: [x, z], yOffset, yaw}. Its L4
// entities -- mobs, spawners, doors, gates, levers, plates, traps, keys,
// containers, waynodes -- live in `assets/dungeons/<dungeon>/refs.json`, a
// ref group file exactly like a map's `refs/<group>.json`, with positions in
// the dungeon-local frame. The RefStore loads it as the group
// "dungeon/<placement id>" and holds WORLD positions, like every other group;
// only the file is local (the same split a locale's anchor-relative y makes).
//
// THE TRANSFORM (rigid, integer, quarter turns):
//
//   anchorWorld = (at.x, BareGroundHeight(at) + yOffset, at.z)
//   world       = anchorWorld + R(yaw) * (local - anchorLocal)
//   worldYaw    = localYaw + yaw
//
// where R(yaw) turns a heading by `yaw` (heading 0 = +Z, 90 = +X, the
// CLAUDE.md convention): R(90) takes local (x, z) to (z, -x). Cells rotate
// about the anchor CELL, not its corner, so the anchor cell maps to itself.
// `anchorLocal` is the entrance socket's local position: `shell.json`
// "anchor" (D1/D2 write it), else `dungeon.json` "anchorLocal", else [0,0,0].
//
// IDS. The file's ids are `dungeon/<dungeon>/<name>`; a placement whose id
// differs from its dungeon asset (the same dungeon placed twice) re-prefixes
// them to `dungeon/<placement id>/<name>` at load -- and every prop string
// that names one ("dungeon/<dungeon>/wn_a") with them -- and takes the prefix
// back off on write. Signals are scoped to the group, so two placements of
// one dungeon do not share a lever.
//
// ORDER OF LOADING / THE OVERLAP WITH D1. worldmap.cpp (package D1) parses
// `dungeons[]` for the SHELL; this file parses the same array independently
// for the ref side (the structures.h "both sides read the file" pattern).
// Both must agree on the anchor rule above.

#pragma once

#include <string>
#include <vector>

#include "math3d.h"

namespace refs {

struct DungeonPlacement {
  std::string id;          // map.json dungeons[].id; the group is "dungeon/<id>"
  std::string name;        // display name
  std::string dungeon;     // asset dir: assets/dungeons/<dungeon>/
  std::string refsFile;    // dungeon.json "refs" (default "refs.json")
  int ax = 0, az = 0;      // the anchor column, world voxels
  int yOffset = 0;
  int yaw = 0;             // quarter turns only (0, 90, 180, 270)
  IVec3 anchorLocal{};     // the entrance socket, dungeon-local voxels
  // RESOLVED against the loaded world map (BareGroundHeight at the anchor).
  int groundY = 0;
  bool resolved = false;

  std::string Group() const { return "dungeon/" + id; }
  // "dungeon/<dungeon>" -- the id prefix the file is written with.
  std::string FilePrefix() const { return "dungeon/" + dungeon; }
  IVec3 AnchorWorld() const { return {ax, groundY + yOffset, az}; }
  int Quarter() const { return (((yaw / 90) % 4) + 4) % 4; }
  IVec3 ToWorld(IVec3 local) const;
  IVec3 ToLocal(IVec3 world) const;
  int YawToWorld(int localYaw) const;
  int YawToLocal(int worldYaw) const;
};

// Turn (x, z) by q quarter turns of heading (R(90): (x, z) -> (z, -x)).
IVec3 RotateQuarter(IVec3 v, int q);

// map.json `dungeons[]` of <assetDir>/worldmap/<map>, table order, with each
// dungeon.json's refs-file name and anchorLocal read. UNRESOLVED (groundY 0).
// Problems (a bad entry, a missing dungeon dir) go to `warn` and that entry
// is skipped; a map with no `dungeons` key is zero placements.
std::vector<DungeonPlacement> ReadDungeonPlacements(const std::string& assetDir,
                                                    const std::string& mapName,
                                                    std::vector<std::string>* warn);
// Resolve groundY for every placement when `mapName` is the loaded world map
// (worldmap::CurrentWorldMap); false (nothing changed) when it is not.
bool ResolveDungeonPlacements(std::vector<DungeonPlacement>& ps, const std::string& mapName);

// The placement of a "dungeon/<id>" group in `ps`, or null.
const DungeonPlacement* PlacementOfGroup(const std::vector<DungeonPlacement>& ps,
                                         const std::string& group);

// Rewrite every string in `j` (deep) that starts with `from` + "/" to start
// with `to` + "/" instead. The id re-prefix (see IDS above).
template <typename J>
void RewritePrefix(J& j, const std::string& from, const std::string& to) {
  if (from == to) return;
  if (j.is_string()) {
    const std::string s = j.template get<std::string>();
    if (s.size() > from.size() && s.compare(0, from.size(), from) == 0 && s[from.size()] == '/')
      j = to + s.substr(from.size());
  } else if (j.is_array() || j.is_object()) {
    for (auto& v : j) RewritePrefix(v, from, to);
  }
}

}  // namespace refs
