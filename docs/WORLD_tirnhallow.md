# Tirnhallow — gazetteer of the `default` map

The `default` world map (`assets/worldmap/default/`) is the island-continent of
**Tirnhallow**, built by `scripts/design_world.mjs` from `docs/WORLD_brief.md`.
This file is the player-and-designer view of it: what is where, and what each
place is for. The script is the truth for HOW it was made; re-running it
regenerates everything listed here (see `docs/EDITOR_GUIDE.md` §15.8).

Conventions: coordinates are **metres**, `(x, z)`, x east, z south, origin at
the map centre; the map spans ±10,035 m. Heights are metres above the world
floor; **sea level is 11.2 m** (`seaLevelY` 112 voxels). Heights quoted for
places are the authored plane; the engine adds its noise rungs on top (up to
±4.8 m with `rangeAmplitude` 96, faded out round Harrowby).

The climate runs on a **west wind**: the west is wet (forest, fen, moor), the
east is dry (steppe, mesa, sand), the north is cold (the Greyspine and its
glacier coast), the south-east is volcanic.

## The home valley

**Hearthvale** (region centre 503, 3310; the region runs from the foot of
Mirrormere south to Gull Bay) — the calm lowland where the game starts. Its
north end, round Harrowby, is a forest-ringed plain at ~20 m, flat on purpose
so the noise is quiet and the village sits level.

- **Harrowby** (locale, anchor 55.5, 346.7, ground ~20 m) — the starting
  village, its 8 lanes and the Harrowby Lane spur west to the West Road, which
  meets the King's Road ~280 m away. The plane under the locale bounds and
  soften box (33..79, 309..365) is held at the original map's height, so the
  locale, its refs and its edit layer ride it unchanged.
- **Spawn** (58, 362) — on forest ground just south of the village green.
- **Home lake** — the small authored lake site that already belonged to
  Harrowby.
- **Mirrormere's south shore** is 1.2 km north of the village; the
  **Silverrun** runs 0.9 km to the east.
- **Twinmere** (lake, centre 1179, 2424, level 18.3 m) — the confluence pool
  ~2.2 km south-east of the village where the Amberbrook meets the Silverrun.

## Zones

| Zone | Where (centre) | Biomes | Ground | What it offers |
|---|---|---|---|---|
| **Hearthvale** | 503, 3310 | forest, meadow | 15–30 m | home; safe farmland, the river junction, roads out in every direction |
| **Amberwood** | -2133, -1001 | amberwood, forest | 60–150 m rolling | autumn hardwood forest; Amberhall (hut, -2300, -1200, 106 m) on the Amberbrook's head |
| **Mirrormere** | 907, -2240 | meadow, forest | lake at 56.9 m | the great lake (3.3 × 1.7 km) behind the Merewall rock lip; Mereholm village on its east shore |
| **The Greyspine** | 664, -5886 | alpine, glacier, tundra, pine | 400–1970 m | the north wall: one long range, Kingsgate Pass (col ~780 m) and Hollowcrown (1,972 m, the highest point); 14 mountain tarns |
| **Rimeholt coast** | 5553, -7724 | tundra, glacier | sea – 400 m | cold fjord coast north of the range: three fjords, a glacier tongue reaching the sea, the Rimeholt fishing village |
| **Heatherhigh** | -5320, -3301 | moor, alpine | 300–550 m plateau | wind-scoured upland moor with 9 tarns and a kettle lake; the Tarn Cairn |
| **Saltreach** | -8074, -1762 | swamp, meadow, ocean | sea – 20 m | salt marsh coast on the west; Saltreach camp; the Eldwater reaches the sea here in cascades |
| **The Eldwood** | -6383, 1129 | eldwood, jungle | 30–80 m | the old wet forest of the west, deep and dark; Eldheart shrine |
| **Glimmerfen** | -3168, 4269 | glowfen, swamp | 11–25 m | luminous fen, ~18 pools, the Glimmer meandering out to Teeth Sound; Glimmerfen Lights shrine |
| **The Verdant Teeth** | -7240, 6928 | karst, jungle | sea – 300 m | clustered karst towers in a green drowned bay (Teeth Sound); sea caves, the Drowned Gate ruin |
| **Whitecliff Downs** | -1530, 6799 | downs, meadow | 60–100 m, chalk cliffs | rolling chalk downland ending in white sea cliffs; the Chalkbourne; Whitecliff Lighthouse |
| **Gull Bay** | 3450, 6741 | meadow, forest, ocean | sea – 30 m | the south bay: Silverrun estuary, Gullhaven port |
| **Goldsea** | 3810, 3718 | savanna, steppe | 30–60 m | dry golden grassland east of home; Goldsea Kraal; the Goldwater drains south to Gull Bay |
| **Sunfall Steppe** | 3981, -903 | steppe, savanna | ~175 m raised plain | open high steppe east of the Mere; Sunfall camp |
| **The Shardspire** | 5804, -4066 | crystal_barrens, mesa | 440–800 m tiered | crystal-shot caprock tablelands and spires; the Shardwell crater (mine) and The Shardspire tower; the Redrun cuts its gorge through it |
| **Redreach** | 6735, 1436 | mesa, desert | tiered caprock, slot canyon | red mesas and buttes; the Redrun's slot canyon; Redreach Watch (95 m) |
| **Sunscour** | 8172, -3436 | dunes, desert | 15–60 m | the eastern sand sea; the Oasis of Sunscour (level 16.7 m) |
| **Ashfall** | 6649, 5803 | ashlands | volcano to 750 m+ | Mount Cinder, a breached caldera with three lava pools and lava flows down its south-east breach; Cinder's Maw ruin |
| **The Mere Isles** | -8026, -7323 | pine, tundra, ocean | islands | a scatter of cold islands off the north-west; a ruin |

## Rivers

All rivers are `rivers[]` entries baked by the engine (flat reaches, falls at
steps). Widths are in metres at source → mouth.

| River | From | To | Length | Notes |
|---|---|---|---|---|
| **Hollowcrown Beck** | Hollowcrown (362, -4810) | Mirrormere (-266, -2109) | 3.9 km | cascading mountain stream (falls every 3.2 m) |
| **Merebeck** | Greyspine (-611, -4016) | Mirrormere | 2.4 km | cascades; enters beside the Beck |
| **The Silverrun** (upper) | Mirrormere outflow (1322, -1238) | Twinmere (1200, 2470) | 4.5 km | the main river, 27–36 m wide |
| **The Silverrun** (lower) | Twinmere | Gull Bay (1405, 6638) | 5.1 km | wide, slow, past Hearthvale to the estuary |
| **The Amberbrook** | Amberwood (-2750, -1900) | Twinmere | 6.0 km | incised forest brook |
| **The Eldwater** | Heatherhigh (-5603, -1930) | Saltreach coast (-7941, 1035) | 4.4 km | cascades off the plateau edge (3.2 m steps) |
| **Mossbeck** | Eldwood (-7062, 2934) | west coast | 1.9 km | small woodland stream |
| **The Redrun** | Greyspine east end (5650, -5850) | east coast (8249, 1110) | 9.7 km | the longest; gorge through the Shardspire, slot canyon through Redreach |
| **The Glimmer** | Glimmerfen (-2000, 3700) | Teeth Sound (-4777, 6992) | 4.7 km | meanders through the fen |
| **The Teethwater** | Teeth hills (-5757, 6019) | Teeth Sound | 5.3 km | |
| **The Chalkbourne** | Downs (-1550, 6200) | south cliffs (-2086, 9100) | 3.0 km | chalk stream in an incised valley |
| **The Goldwater** | Goldsea (4500, 3200) | Gull Bay (3254, 7150) | 4.3 km | |
| **Skarbeck, Kingsbeck, Isbeck** | north face of the Greyspine | Rimeholt fjords | 0.5–0.8 km | short glacier-melt torrents |

## Lakes

- **Mirrormere** (517, -1689) level 56.9 m — the great lake; feeds the Silverrun.
- **Twinmere** (1179, 2424) level 18.3 m — the confluence pool by Harrowby.
- **The Oasis of Sunscour** (8184, -1964) level 16.7 m, plus the `sunscour_spring` oasis site.
- **Heatherhigh tarns** — 9 moorland tarns and the `heather_kettle` kettle site.
- **Glimmerfen pools** — ~18 fen pools.
- **Greyspine mountain tarns** — 14 hollow lakes, levels 270–677 m, plus the
  `rime_tarn` site.
- **Shardwell** — the crater on the Shardspire (mine marker at 6769, -4963).
- **Lava** — three `lava_pool` sites in Mount Cinder's caldera.

## Roads

Roads are smooth graded splines (`roads[]`) conformed to the ground.

| Road | Kind | Length | Runs |
|---|---|---|---|
| **The King's Road** | road | 15.0 km | Gullhaven → up the Silverrun past Harrowby → Mirrormere → foot of the Greyspine (-950, -3400) |
| **The King's Road (Kingsgate Pass)** | road | 4.7 km | switchbacks up to the pass and the Kingsgate castle (-1615, -6135) |
| **The King's Road (Rimeholt)** | road | 4.3 km | switchbacks down the north face to Rimeholt |
| **The East Road** | road | 11.2 km | off the King's Road (768, 1894) → Sunfall Steppe → the Oasis of Sunscour |
| **The West Road** | track | 12.1 km | Harrowby → the Eldwood (past Eldheart) → Saltreach |
| **The Tarn Way** | track | 6.4 km | from Amberhall up onto Heatherhigh, past the tarns to the Cairn |
| **The Fen Walk** | path | 5.0 km | off the West Road (-998, -256) south into Glimmerfen to the Lights |
| **The Teeth Track** | track | 7.0 km | Glimmerfen Lights → the Drowned Gate → the sea caves |
| **The Clifftop Path** | path | 5.6 km | Gullhaven west along Whitecliff Downs, past the lighthouse |
| **The Goldsea Track** | track | 12.0 km | off the King's Road (1178, 5325) across Goldsea and Ashfall to Cinder's Maw |
| **The Shard Road** | track | 6.5 km | off the East Road near Sunfall up onto the Shardspire |
| **Harrowby Lane** | path | 0.3 km | village spur to the West Road; plus the 8 `harrowby_*` village lanes |

## Places (markers)

| Place | Icon | (x, z) m | Plane height |
|---|---|---|---|
| Gullhaven | port | 1026, 7139 | 13 m |
| Mereholm | village | 2350, -1550 | 71 m |
| Kingsgate | castle | -1615, -6135 | 204 m (pass approach) |
| Rimeholt | village | -1683, -7564 | 13 m |
| Oasis of Sunscour | camp | 8100, -2250 | 17 m |
| Redreach Watch | tower | 6450, 350 | 95 m |
| Cinder's Maw | ruin | 6400, 6800 | 749 m |
| The Shardspire | tower | 6000, -3803 | 770 m |
| Eldheart | shrine | -6550, 700 | 43 m |
| Glimmerfen Lights | shrine | -2900, 4200 | 22 m |
| The Verdant Teeth sea caves | cave | -8100, 8750 | 21 m |
| Whitecliff Lighthouse | tower | -1014, 8586 | 71 m |
| Hollowcrown | shrine | 175, -6845 | 1,173 m |
| Saltreach | camp | -8600, -1200 | 18 m |
| The Tarn Cairn | ruin | -5600, -3362 | 508 m |
| Amberhall | hut | -2300, -1200 | 106 m |
| The Shardwell | mine | 6769, -4963 | 482 m |
| The Ice Tongue | cave | 5450, -8200 | 13 m |
| The Mere Isles | ruin | -8300, -6000 | 45 m |
| Sunfall | camp | 3900, -800 | 181 m |
| Goldsea Kraal | hut | 4300, 4000 | 40 m |
| The Drowned Gate | ruin | -6700, 7400 | 30 m |

Markers are map annotations only; no structures are placed at them yet. They
are where the next content packages (villages, dungeons, camps) should go.
