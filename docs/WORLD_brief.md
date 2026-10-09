# World brief: the realm of Tirnhallow (default/ map, worldbuilder wave 3)

The design the world package (D1) builds into `assets/worldmap/default/` with
`scripts/design_world.mjs`. Coordinates in KILOMETRES, x east, z SOUTH (north
is -z, the top of the map page), origin = world origin. 1 km = 10,000 voxels.
The map extent is ±10.035 km with a 614 m edge fade to ocean: keep land
inside ±9.2 km. Heights are metres ABOVE SEA (sea = voxel Y 112).

## Design goals

1. **Reads like a real place**: ranges with dendritic valleys, rivers that
   start in the mountains, join, pass through lakes and end at the sea, a rain
   shadow that explains the dry east, coasts that are cliffs in one place and
   dunes in another.
2. **Plays like an RPG overworld**: a gentle, safe start (Hearthvale) that
   opens in every direction onto a distinct zone; roads as the spine between
   named places; landmarks visible from far away (Hollowcrown, Mount Cinder's
   plume, the Shardspire, white cliffs); natural barriers with deliberate gaps
   (Kingsgate Pass, fords, the canyon trail); secrets off the roads (tarns,
   waterfalls, a sea cave coast, islands).
3. **Every biome appears at least once**, sized for what it is: big zones for
   the overworld biomes, small jewels for the strange ones.

## The land (west wind: wet west, dry east)

| Zone | Where (km) | Landform | Biomes |
|---|---|---|---|
| **Hearthvale** (start) | x -2.5..3, z -0.5..5 | rolling lowland 5..60 m, the Silverrun down its middle | forest, meadow |
| **Mirrormere** | lake centred (0.6, -1.6), ~3 x 1.4 km, level ~45 m | glacial lake at the range's foot; streams and waterfalls fall into it from the north | meadow shore, pine north shore |
| **The Greyspine** | arc (-7.5,-5.2) -> (-3,-6.2) -> (0,-6.4) -> (3.5,-6.6) -> (7,-5.4) | the main range: crest 1,100..1,700 m, **Hollowcrown** 1,950 m at (0.2,-6.6), spurs and valleys both sides, foothills fading south to z -3 | pine low, alpine mid, glacier high (above ~1,250 m and in north-facing cirques), tundra on the cold north side |
| **Kingsgate Pass** | saddle at (-1.6,-5.9), ~780 m | the one road through the range; switchbacks | alpine |
| **Rimeholt** | north coast strip z -9.2..-7.4 | cold coastal lowland cut by 3 FJORDS (U-valleys flooded) and a glacier tongue reaching the sea | tundra, glacier |
| **Heatherhigh** (moor) | x -7.5..-3.5, z -4.5..-2 | upland plateau 300..550 m, broad and gentle, dotted with TARNS (small lakes) | moor, pine edges |
| **Amberwood** | x -4..-1, z -2.5..0.5 | autumn foothills 60..250 m, streams to Mirrormere's outflow | amberwood |
| **The Eldwood** | x -8.5..-4.5, z -1.5..4 | deep wet valley of the Eldwater river flowing west to the sea; huge trees; misty | eldwood, swamp at the river mouth |
| **Glimmerfen** | basin x -4..-1.5, z 2.5..5.5 | dead-flat lowland 2..8 m with many small lakes, a meandering slow river | glowfen |
| **Saltreach Dunes** | west coast x -9.2..-8, z -3..5 + the Silverrun estuary | coastal dunes, a spit, salt marsh | dunes, swamp |
| **Verdant Teeth** | SW peninsula x -9..-5, z 5.5..9 | jungle valleys between steep KARST towers 60..250 m, a sea-cave coast, two offshore karst islets | jungle, karst |
| **Whitecliff Downs** | south coast x -4..1, z 5.5..8.8 | smooth chalk downs 40..140 m ending in 60..100 m WHITE SEA CLIFFS | downs |
| **Gull Bay** | bay x 0.5..4, z 6.5..9.2 | the Silverrun's estuary + delta, a port town | meadow, swamp at the delta |
| **Sunfall Steppe** | x 2.5..6, z -3..2 | high flat grassland 120..220 m in the rain shadow | steppe |
| **Redreach** | x 5..8.8, z -1..4.5 | mesa canyonlands: TERRACED buttes 150..420 m cut by the Redrun canyon (a river in a deep slot) | mesa, desert at the coast |
| **Sunscour** | east coast x 7.5..9, z -4..-1 | desert with an OASIS lake | desert |
| **Goldsea** | x 3..6, z 2..6 | warm dry plain with lone flat-top trees | savanna |
| **Ashfall** | SE corner, MOUNT CINDER at (6.4, 6.8) | volcano cone 1,150 m with a caldera + lava pool, lava ridges and ash plains to a black-sand coast | ashlands |
| **The Shardspire** | NE plateau x 4..7.5, z -5..-2.5, spire at (6,-3.8) | broken tableland 400..650 m, the Shardspire a sheer 300 m pinnacle | crystal_barrens |
| **The Mere Isles** | offshore archipelago NW x -9.2..-7, z -8..-5 | 5-8 small rocky islands | pine, tundra |

## Water

- **Silverrun**: source in Hollowcrown's south cirques -> waterfalls into
  Mirrormere -> outflow at the lake's south end -> south through Hearthvale
  (passing 0.6..1 km east of Harrowby) -> joins the **Amberbrook** (from
  Amberwood) -> delta into Gull Bay. Width 10 m upper, 30-45 m lower.
- **Eldwater**: from Heatherhigh tarns west through the Eldwood to the sea,
  swampy mouth.
- **Redrun**: from the Greyspine's east end down through the steppe into a
  deep slot canyon in Redreach, to the east coast.
- **Glimmer**: short meandering fen river with oxbow ponds into Saltreach.
- **Rimeholt**: short steep streams + waterfalls off the range into the
  fjords.
- **Lakes**: Mirrormere (big), 6-10 Heatherhigh tarns, Glimmerfen pools, the
  Sunscour oasis, Cinder's lava pool (preset), a crater lake on the
  Shardspire plateau.
- Use the hydrology tools: derive the network from flow accumulation of the
  generated terrain, then keep the named rivers as authored vectors; every
  river must end in a lake, another river or the sea.

## Roads (smooth splines, conformed)

- **King's Road** (road): Gull Bay port -> Harrowby -> Mirrormere east shore
  -> Kingsgate Pass switchbacks -> Rimeholt fjord-head town.
- **East Road** (road): Harrowby -> Sunfall Steppe -> Redreach canyon rim ->
  Sunscour oasis.
- **West Road** (track): Harrowby -> Amberwood -> Eldwood -> Saltreach.
- **Tracks/paths**: Heatherhigh tarns, Whitecliff clifftop path, Ashfall
  (track to the caldera rim), Glimmerfen boardwalk-path, Verdant Teeth.
- Fords where roads cross rivers (bridges later as stamps).

## Places (markers; regions as the zones above)

Harrowby (existing village; MAY be moved along with the spawn, but keeping it
where it is, in Hearthvale ~(0.06, 0.35), is preferred: its gates pin it).
New markers: Gullhaven (port), Mereholm (lake town), Kingsgate (pass fort),
Rimeholt (fjord town), Oasis of Sunscour, Redreach Watch, Cinder's Maw
(caldera), the Shardspire, Eldheart (giant tree), Glimmerfen lights, the
Verdant Teeth sea caves, Whitecliff lighthouse, Hollowcrown summit.

## Hard constraints

- Harrowby's locale ground stays at its height (~plane 200 vox = 8.8 m): the
  village gates pin it. Its soften box and spawn stay valid (`spawn-site`:
  on land, off every lake/water, not in ocean biome).
- Nothing in the window around the spawn should keep the sim awake.
- Site-cell budgets (`kSiteCellMax`), plane ceilings, the loader's limits.
- The map must load with ZERO warnings (`village-harrowby` checks it).
