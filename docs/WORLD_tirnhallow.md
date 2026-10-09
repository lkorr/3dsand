# Tirnhallow: gazetteer of the `default` map

The `default` world map (`assets/worldmap/default/`) is the island-continent of
**Tirnhallow**, built by `scripts/design_world.mjs` from `docs/WORLD_brief.md`.
This file is the player-and-designer view of it: what is where, and what each
place is for. The script is the truth for HOW it was made; re-running it
regenerates everything listed here (see `docs/EDITOR_GUIDE.md` §15.8).

Conventions: coordinates are **metres**, `(x, z)`, x east, z south, origin at
the map centre; the map spans ±10,035 m. Heights are **metres above sea
level** (`seaLevelY` 112 voxels = 11.2 m above the world floor). Heights quoted
for places are the authored plane; the engine adds its noise rungs on top (up
to ±4.8 m with `rangeAmplitude` 96, faded out round Harrowby).

The climate runs on a **west wind**: the west is wet (forest, fen, moor), the
east is dry (steppe, mesa, sand), the north is cold (the Greyspine and its
fjord coast), the south-east is volcanic. Biome borders are not the zones'
outlines: each zone proposes a biome from the ground (height, slope, valley or
ridge, wetness from the drainage and the nearness of water), and across a
border band the neighbouring zone takes the cells whose ground suits its biome
better, so woods run up the valleys, fen spreads along the wet hollows, dunes
lie only behind the beaches and the grass holds the ridges.

## The home valley

**Hearthvale** (region centre 503, 3310; the region runs from the foot of
Mirrormere south to Gull Bay) is the calm lowland where the game starts. Its
north end, round Harrowby, is a forest-ringed plain about 9 m above the sea,
flat on purpose so the noise is quiet and the village sits level.

- **Harrowby** (locale, anchor 55.5, 346.7): the starting village, its 8
  lanes and the Harrowby Lane spur west to the West Road, which meets the
  King's Road ~280 m away. The plane under the locale bounds and soften box
  (33..79, 309..365) is held at the original map's height, so the locale, its
  refs and its edit layer ride it unchanged.
- **Spawn** (58, 362): on forest ground just south of the village green.
- **Home lake**: the small authored lake site that already belonged to
  Harrowby.
- **Mirrormere's south shore** is 1.2 km north of the village; the
  **Silverrun** runs 0.9 km to the east.
- **Twinmere** (lake, centre 1179, 2424, level 7 m): the confluence pool
  ~2.2 km south-east of the village where the Amberbrook meets the Silverrun.

## Zones

| Zone | Where (centre) | Biomes | Ground | What it offers |
|---|---|---|---|---|
| **Hearthvale** | 503, 3310 | forest, meadow | 5–30 m | home; farmland and woods along every watercourse, the river junction, roads out in every direction |
| **Amberwood** | -2133, -1001 | amberwood, forest, meadow clearings | 50–140 m rolling | autumn hardwood forest; Amberhall (hut) at the Amberbrook's head |
| **Mirrormere** | 907, -2240 | meadow, forest, pine on the slopes | lake at 46 m | the great lake (3.3 × 1.7 km) behind the Merewall rock lip; Mereholm village on its east shore |
| **The Greyspine** | 664, -5886 | alpine, glacier, pine (climbing the valleys), tundra | 400–1,972 m | the north wall: one long range, Kingsgate Pass (col ~780 m) and Hollowcrown (1,972 m, the highest point); 14 mountain tarns |
| **Rimeholt coast** | 5553, -7724 | tundra, pine in the sheltered valleys, glacier | sea – ~350 m, rugged | a raised, ice-scoured highland cut by six fjords (below) and the glacier tongue; the Rimeholt fishing village at the head of the Kings' fjord |
| **Heatherhigh** | -5320, -3301 | moor, bog (swamp) in the wet hollows, pine on the slopes | 290–540 m plateau | wind-scoured upland moor with 9 tarns and a kettle lake; the Tarn Cairn |
| **Saltreach** | -8074, -1762 | dunes behind the beaches, salt marsh, heath and wood behind | sea – 20 m | the west coast: a ragged strip of dune, marsh and heath giving way to the Eldwood; Saltreach camp; the Eldwater reaches the sea here in cascades |
| **The Eldwood** | -6383, 1129 | eldwood, swamp in the wet flats | 20–70 m | the old wet forest of the west, deep and dark; Eldheart shrine |
| **Glimmerfen** | -3168, 4269 | glowfen, swamp, carr wood and meadow on the rises | 0–15 m | luminous fen in drifts and pools (~18), the Glimmer meandering out to Teeth Sound; Glimmerfen Lights shrine |
| **The Verdant Teeth** | -7240, 6928 | karst, jungle | sea – 300 m | karst in clusters (fengcong) on shared rocky bases: sheer towers, cone peaks and sawtooth ridges of every size, sea stacks off the coast; sea caves, the Drowned Gate ruin |
| **Whitecliff Downs** | -1530, 6799 | downs, meadow, wood in the valleys | 50–90 m, chalk cliffs | rolling chalk downland ending in white sea cliffs; the Chalkbourne; Whitecliff Lighthouse |
| **Gull Bay** | 3450, 6741 | meadow, forest, marsh | sea – 20 m | the south bay: Silverrun estuary, Gullhaven port |
| **Goldsea** | 3810, 3718 | savanna, steppe on the rises, gallery wood by the water | 20–50 m | dry golden grassland east of home; Goldsea Kraal; the Goldwater drains south to Gull Bay |
| **Sunfall Steppe** | 3981, -903 | steppe, savanna in the hollows | ~165 m raised plain | open high steppe east of the Mere; Sunfall camp |
| **The Shardspire** | 5804, -4066 | crystal_barrens | 430–790 m tiered | crystal-shot caprock tableland, spires and the Shardwell crater (mine); The Shardspire tower; the Redrun's canyon cuts through it |
| **Redreach** | 6735, 1436 | mesa, desert | buttes and mesas on strata | red mesas and buttes with sheer stepped flanks and flat caprock tops; the Redrun's canyon; Redreach Watch |
| **Sunscour** | 8172, -3436 | desert, dunes in the erg | 5–50 m | the eastern sand sea; the Oasis of Sunscour |
| **Ashfall** | 6649, 5803 | ashlands (grey tephra, black basalt on the steep faces), regrowing heath and steppe | to ~1,200 m | Mount Cinder (below); Cinder's Maw ruin in its crater |
| **The Mere Isles** | -8026, -7323 | pine, tundra | islands | a scatter of cold islands off the north-west; a ruin |

### Set pieces

- **The fjords of Rimeholt.** Six flooded glacial troughs, each different: the
  **West fjord** (-4200, -7200 → -4850, -9850), long and sinuous with two side
  arms and three hanging valleys whose floors stop 140–260 m up its walls; the
  **Kings' fjord** (-1750, -7620 → -2000, -9850), Rimeholt at its head, one arm
  and two hanging valleys; the **East fjord** (3000, -7750 → 3300, -9850) with
  an arm and a hanging valley; the narrow **Skar fjord** (-6150, -8150), the
  **Tongue fjord** where the glacier tongue (5000, -6200 → 5600, -8800) meets the
  sea, and **Isvik** (7350, -8200). The West, Kings' and East fjords continue
  inland as U-shaped troughs up into the Greyspine; their floors fall in a
  staircase to the fjord heads (the Skarbeck, Kingsbeck and Isbeck fall down
  them). Floors 30–70 m below the sea, rising to a sill at each mouth.
- **The Redrun canyon.** From the Shardspire's tableland down through Redreach
  to the east coast the Redrun runs in a canyon of entrenched meanders. Its
  walls are built from one shared set of strata (hard beds stand as cliffs,
  soft beds weather back into benches and talus), so the ledges line up along
  and across the canyon; side canyons branch off in tongues. Where the river
  leaves the Shardspire it drops in a series of big falls. Two broken-down
  stretches (5020, -1620) and (5120, -2950) are where a traveller can get down
  to the water.
- **Mount Cinder** (6400, 6800). A stratovolcano: concave flanks steepening to
  the summit, deep radial gullies in some sectors and smooth young flows in
  others, a small deep summit crater with a jagged lip, breached to the
  south-east, three lava pools in the crater, five parasitic cones on
  the flanks, each with its own small crater, and eight lava flows with steep lobate fronts running
  down towards the coast. The Maw Path climbs to the crater's rim in hairpins.

## Rivers

All rivers are `rivers[]` entries baked by the engine (flat reaches, falls at
steps).

| River | From | To | Length | Notes |
|---|---|---|---|---|
| **Hollowcrown Beck** | Hollowcrown (362, -4810) | Mirrormere | 3.9 km | cascading mountain stream (falls every 3.2 m) |
| **Merebeck** | Greyspine (-611, -4016) | Mirrormere | 2.4 km | cascades; enters beside the Beck |
| **The Silverrun** (upper) | Mirrormere outflow (1322, -1238) | Twinmere | 4.5 km | the main river, 27–36 m wide |
| **The Silverrun** (lower) | Twinmere | Gull Bay (1405, 6638) | 5.1 km | wide, slow, past Hearthvale to the estuary |
| **The Amberbrook** | Amberwood (-2750, -1900) | Twinmere | 6.1 km | forest brook |
| **The Eldwater** | Heatherhigh (-5603, -1930) | Saltreach coast | 4.7 km | cascades off the plateau edge (3.2 m steps) |
| **Mossbeck** | Eldwood (-7062, 2934) | west coast | 1.9 km | small woodland stream |
| **The Redrun** | Greyspine east end (5650, -5850) | east coast (8200, 1100) | 10.8 km | the longest; entrenched meanders in its canyon (above), big falls off the Shardspire |
| **The Glimmer** | Glimmerfen (-2000, 3700) | Teeth Sound (-4777, 6992) | 4.7 km | meanders through the fen |
| **The Teethwater** | Teeth hills (-5757, 6019) | Teeth Sound | 5.2 km | |
| **The Chalkbourne** | Downs (-1550, 6200) | south cliffs (-2086, 9100) | 3.0 km | chalk stream, falls into the sea |
| **The Goldwater** | Goldsea (4500, 3200) | Gull Bay (3254, 7150) | 4.3 km | |
| **Skarbeck, Kingsbeck, Isbeck** | north face of the Greyspine | the West, Kings' and East fjords | 1.0–1.8 km | meltwater torrents down the troughs, a fall every few metres |

## Lakes

- **Mirrormere** (517, -1689), level 46 m: the great lake; feeds the Silverrun.
- **Twinmere** (1179, 2424), level 7 m: the confluence pool by Harrowby.
- **The Oasis of Sunscour** (8184, -1964), level 5.5 m, plus the
  `sunscour_spring` oasis site.
- **Heatherhigh tarns**: 9 moorland tarns and the `heather_kettle` kettle site.
- **Glimmerfen pools**: ~18 fen pools.
- **Greyspine mountain tarns**: 14 hollow lakes, levels 260–665 m, plus the
  `rime_tarn` site.
- **Shardwell**: the crater on the Shardspire (mine marker at 6769, -4963).
- **Lava**: three `lava_pool` sites in Mount Cinder's crater.

## Roads

Roads are smooth graded splines (`roads[]`) conformed to the ground. The
hairpin roads are laid (uneven legs, so no two bends repeat); the rest are
routed over the ground by grade.

| Road | Kind | Length | Runs |
|---|---|---|---|
| **The King's Road** | road | 15.0 km | Gullhaven → up the Silverrun past Harrowby → Mirrormere → foot of the Greyspine (-950, -3400) |
| **The King's Road (Kingsgate Pass)** | road | 4.4 km | hairpins up to the pass and the Kingsgate castle |
| **The King's Road (Rimeholt)** | road | 3.6 km | hairpins down the Kings' trough to Rimeholt |
| **The East Road** | road | 9.9 km | off the King's Road (768, 1894) → Sunfall Steppe → across the Redrun's canyon → the Oasis of Sunscour |
| **The West Road** | track | 11.8 km | Harrowby → the Eldwood (past Eldheart) → Saltreach |
| **The Tarn Way** | track | 5.6 km | from Amberhall up onto Heatherhigh, past the tarns to the Cairn |
| **The Fen Walk** | path | 5.0 km | off the West Road (-998, -256) south into Glimmerfen to the Lights |
| **The Teeth Track** | track | 7.1 km | Glimmerfen Lights → the Drowned Gate → the sea caves |
| **The Clifftop Path** | path | 5.6 km | Gullhaven west along Whitecliff Downs, past the lighthouse |
| **The Goldsea Track** | track | 7.4 km | off the King's Road (1178, 5325) across Goldsea to the foot of Mount Cinder (5980, 6060) |
| **The Maw Path** | path | 3.7 km | hairpins from Cinder's foot to the crater's rim |
| **The Shard Road** | track | 1.8 km | off the East Road near Sunfall to the foot of the Shardspire's escarpment (4550, -2300) |
| **The Shard Road (the Climb)** | track | 2.3 km | hairpins up onto the tableland, to the rim over the Redrun's canyon |
| **Harrowby Lane** | path | 0.3 km | village spur to the West Road; plus the 8 `harrowby_*` village lanes |

## Places (markers)

| Place | Icon | (x, z) m | Plane height |
|---|---|---|---|
| Gullhaven | port | 1026, 7139 | 2 m |
| Mereholm | village | 2350, -1550 | 60 m |
| Kingsgate | castle | -1585, -5965 | 533 m (the pass approach) |
| Rimeholt | village | -1850, -7450 | 21 m |
| Oasis of Sunscour | camp | 8100, -2250 | 6 m |
| Redreach Watch | tower | 6450, 350 | 64 m |
| Cinder's Maw | ruin | 6400, 6800 | 973 m (the crater floor) |
| The Shardspire | tower | 6000, -3803 | 757 m |
| Eldheart | shrine | -6550, 700 | 32 m |
| Glimmerfen Lights | shrine | -2900, 4200 | 11 m |
| The Verdant Teeth sea caves | cave | -8100, 8750 | 4 m |
| Whitecliff Lighthouse | tower | -1014, 8586 | 60 m |
| Hollowcrown | shrine | 570, -6950 | 1,971 m (the summit) |
| Saltreach | camp | -8600, -1200 | 6 m |
| The Tarn Cairn | ruin | -5600, -3362 | 496 m |
| Amberhall | hut | -2300, -1200 | 95 m |
| The Shardwell | mine | 6769, -4963 | 470 m |
| The Ice Tongue | cave | 5450, -8200 | 20 m |
| The Mere Isles | ruin | -8300, -6000 | 33 m |
| Sunfall | camp | 3900, -800 | 168 m |
| Goldsea Kraal | hut | 4300, 4000 | 34 m |
| The Drowned Gate | ruin | -6700, 7400 | 19 m |

Markers are map annotations only; no structures are placed at them yet. They
are where the next content packages (villages, dungeons, camps) should go.
