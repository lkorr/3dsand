// worldwater.h -- map.json rivers[] and lakes[] (docs/PLAN_worldbuilder.md W1):
// read, validated and BAKED at load into kSiteRiver / kSiteLakeEdge /
// kSiteLakeCell records, plus the spline every curved map feature shares.
// LoadWorldMap (worldmap.cpp) is the only caller; worldmap.h owns the record
// layout and the shader readers (worldgen.wgsl waterApply / lakeTest).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "sim/worldmap.h"

namespace biomes { struct BiomeSet; }

namespace worldmap {

using WaterWarn = std::function<void(const std::string&)>;

/** map.json `rivers` and `lakes` into out.rivers / out.lakes, validated (a
 *  malformed shape is a refusal naming the entry; a clamped number, an
 *  unknown material or a point off the extent a warning). Nothing baked. */
bool ParseRiversLakes(const nlohmann::json& j, const biomes::BiomeSet& set, WorldMapData& out,
                      const std::string& at, std::string& log, const WaterWarn& warn);

/** A road's points through THE SPLINE (smooth: true), rounded to voxels and
 *  simplified (Douglas-Peucker, 0.75 voxel) so straight stretches stay one
 *  piece. */
std::vector<std::pair<int, int>> SmoothRoadPoints(const std::vector<std::pair<int, int>>& pts);

/**
 * THE BAKE (worldwater.cpp "THE RIVER BAKE"): lakes first (outline, edges,
 * per-cell row parity), then the rivers in join order (surface, reaches,
 * pieces). Reads the bare ground through BareGroundHeight against
 * CurrentWorldMap() -- the caller installs the map being loaded. Appends the
 * records to `sites` (river pieces, then lake edges, then lake cells) and
 * fills each River / Lake's baked fields. False + `log` on a join cycle or a
 * join naming no river.
 */
bool BakeRiversLakes(WorldMapData& out, uint32_t seed, std::vector<WorldMapData::StampSite>& sites,
                     std::string& log, const std::string& at, const WaterWarn& warn);

/** How far past its box a water record is listed in the site index (the
 *  widest thing any reader asks from it). */
int WaterIndexReach(const WorldMapData::StampSite& st, int treeMaxReach);

}  // namespace worldmap
