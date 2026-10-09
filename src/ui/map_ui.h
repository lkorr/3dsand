#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ui/overlay.h"

namespace worldmap { struct WorldMapData; }

// THE WORLD MAP SCREEN (M).
//
// Two halves, the way the item stage is split:
//
//   BuildWorldMapImage  the PICTURE, drawn on the CPU from the active map's
//                       authored planes (worldmap::AuthoredHeightAt, the height
//                       plane's Catmull-Rom with no noise on it; the biome
//                       plane) plus its lakes, rivers and roads. Not from
//                       voxels: the map is the whole 20 km world and the
//                       window is 51 m of it. Built once per map (main.cpp
//                       rebuilds it on F7) and copied into a texture.
//   DrawWorldMapScreen  the PANEL: a pure drawing function over
//                       UIState::worldMap — the picture panned and zoomed, the
//                       player arrow, the spawn, the named places, a scale bar
//                       and the DEV TELEPORT latch. It never moves the player;
//                       main.cpp consumes `wantTeleport`.

// The picture: at most `maxSide` pixels on the long side, RGBA8 rows packed
// (imgW * 4 bytes each). `biomeNameById` names a biome plane byte (a BiomeSet
// index) so the colours are the tuner map page's (assets/editor/climate.js
// BIOME_COLORS). False when the map has no extent to draw.
bool BuildWorldMapImage(const worldmap::WorldMapData& m,
                        const std::vector<std::string>& biomeNameById, int maxSide,
                        std::vector<uint8_t>& rgba, int& imgW, int& imgH);

// The map's named places (locales, markers, regions), its extent and spawn,
// into the panel's mirror.
void FillWorldMapMirror(const worldmap::WorldMapData& m, UIState::WorldMapUI& out);

void DrawWorldMapScreen(UIState& s);
