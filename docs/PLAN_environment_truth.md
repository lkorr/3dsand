# PLAN: the Environment tab is the truth about the world

Status: PROPOSAL, 2026-09-04. Supersedes `PLAN_biomes.md` §5 (the wiring
order) and closes the follow-up list in `PLAN_world_map.md`. Nothing here is
implemented yet.

## 0. Why edits do not show today (measured on main 482b756)

The user painted the spawn cell desert and set forest to "a bunch of trees",
restarted, and saw forest with almost no trees. Five separate reasons, and
each one alone is enough:

1. **The paint never reached disk.** `assets/worldmap/default/map.svmap`
   still has forest on every cell around the origin, identical to the
   committed file, mtime 18:25 (a git write, before any painting). The map
   page only writes on the explicit `Save map` button; nothing autosaves and
   nothing tells you the game is reading a different file than you see.
2. **Nothing reloads the environment.** `LoadBiomeSet`, `LoadTreeAtlas`,
   `LoadWorldMap` run once at boot (`main.cpp:3394..3420`) and the packed
   words go into `worldMapBuf_` / `treeAtlasBuf_` in `Simulation::Init`.
   `F5` reloads tuning + shaders, `R` reloads materials, the overlay's
   **regen world** button reruns worldgen — with the OLD buffers. A saved map
   or biome needs a full restart, and no UI says so.
3. **The page shows knobs the engine does not read, indistinguishably from
   the ones it does.** Read today: skin/subsoil/skinDepth, patch, cover rows,
   `trees.density`, species weights, cave thresholds. NOT read: `trees.tile`,
   every `water.features` row, `terrain.overrides`, per-row `conditions`
   (only the species-file band/slope apply), the moisture plane. The tree
   density stat ("≈ N trees per hectare") is computed from the ignored tile,
   so it promised 153/ha where the engine makes 23/ha. The user's edit
   (tile 14.4→5.6, density 78→48) therefore HALVED the forest.
4. **Spawn is inside a tree ban.** The player starts at (140, 140); the
   harness pad site spans (-128..640)² and refuses trunks, crowns, tarns
   and cover so the selftest fixtures keep their ground. 77 m of bare grass
   around spawn whatever the biome says.
5. **Two authoring surfaces for one fact.** `tuning.json worldgen.*` has 101
   knobs; ~40 of them are the global versions of things the biome pages
   author (tree tile, pond geometry, shore/aquatic chances, the per-biome
   relief curves, four biome thresholds that are DEAD since `biomeAt` reads
   the map). The Worldgen tab still shows all of them as live.

## 1. Principles

- **One authoring surface per fact** (CLAUDE.md design rule 4). If a number
  is per-biome it lives in `assets/biomes/<name>.json`; per-water-body in
  `assets/water/`; per-map in `assets/worldmap/<name>/map.json`. Nothing
  per-biome or per-map stays in `tuning.json`.
- **Every field on a page is live or visibly disabled.** No third state. A
  field the engine ignores is greyed with the package that will read it.
- **The number the page shows is the number the world has.** Predictions
  (trees/ha, bodies/km², cover %) use ONE formula, and a gate measures the
  real GPU worldgen against it. Accuracy is a tested property, not a hope.
- **Edit → see in under ten seconds**, from either the tuner or the game,
  and the game always reports which map/biome content it is running.
- Every package moves the world hash once and is rebaselined at its end
  (CLAUDE.md rule 1: a moved pin is a notification). Determinism is untouched
  by construction: everything is a table read keyed on world coords.

## 2. Packages, by risk. Each is one worktree, one rebaseline.

### P-A  The apply loop (C++, small; the one that fixes "nothing changes")

- `ReloadEnvironment(ctx, sim, world)`: `LoadBiomeSet` → `LoadTreeAtlas` →
  `LoadWorldMap` → `PackWorldMap`; re-upload `worldMapBuf_` and
  `treeAtlasBuf_`. Sizes may grow (a new biome, a bigger map, a new species):
  recreate the two buffers and the bind groups that hold them, exactly as the
  micro-body pool is rebuilt on `R`. Then the existing `ui.regenWorld` path.
- Bound to: the overlay's **regen world** button (it now means "reload the
  environment and regenerate"), a dev key, and a tuner request
  `/api/game/apply-environment` over the telemetry channel the Play button
  already uses. Environment pages get **Save + Apply** next to Save.
- Boot and reload print `environment: map <name> <hash> | biomes <hash> |
  trees <hash>`; the same triple goes out on telemetry. The Environment tab
  shows game-vs-disk and a STALE badge with the apply button lit.
- The map page autosaves nothing, but its dirty state is unmistakable: the
  sidebar entry and the tab badge go amber, and closing the tab asks.
- Gate: `env-reload` — load, mutate one biome's skin in memory, reload,
  assert the GPU skin changed (reuses the `worldmap` gate's GPU probe).

### P-B  Honest pages (JS only, no build)

- One `LIVE` manifest in `assets/editor/envlive.js`: for each biome/water/map
  field, `{read: true}` or `{read: false, package: 'P-D'}`. The page renders
  the second kind disabled with the reason in the tooltip. `scripts/
  test_environment.mjs` asserts every editable field is in the manifest so a
  new field cannot ship in the third state.
- Until P-D, `densityStats` takes the ENGINE tile (already fetched as
  `treeTileM` at `biome.js:531`) and the tile row is disabled.
- The map page draws the spawn marker, the harness pad, and a 1 km grid with
  metres, so "spawn" is a thing you can see and paint around.
- The Worldgen tab marks the dead knobs (`meadow/pine/desertThreshold`,
  `biomeLog2`, `biomeBlend`, `alpineChance`) deprecated; they are deleted in
  P-I.

### P-C  Spawn is a site on the map (C++ small, map.json)

- `map.json sites[]` gains `{kind: "spawn", at: [x, z]}`; `main.cpp` reads
  it for the game start and for `regen world` (`player.pos = {140,..,140}`
  at 3914 / 5177 goes). The default map puts spawn outside the harness pad,
  on forest, on land above sea level (the map's own landform plane says
  where). The harness pad keeps its box; fixtures do not move.
- The `spawn`-relative constants (`spawnPlainY/R/Fade`, the calm home area)
  centre on the spawn site instead of the origin. `spawnPlain*` moves to
  `map.json terrain.homeArea` in P-I.

### P-D  Tree spacing per biome, WITHOUT a per-biome lattice (WGSL + loader)

The shader comment at `worldgen.wgsl:1712` is right that the 5×5 candidate
scan needs one lattice. The robust answer is to keep ONE lattice and make it
the FINEST authored spacing, then thin each biome to the density its page
predicts:

- At load: `T = min over biomes of trees.tile` (voxels), `chance_b =
  density_b × (T / tile_b)²` in Q16, packed per biome as `kB_TreeChanceQ16`.
  `kB_TreeTileVox` stays informational. A forest at 5.6 m / 48 % gets
  chance 0.48; a meadow at 14.4 m / 22 % gets 0.033 per fine tile. Trees per
  hectare match the page by construction; only the jitter pattern differs.
- `TREE_TILE`, `TREE_SCAN`, `TREE_CAND_MAX` become prelude constants derived
  at load from `T` and the atlas's `maxReach` (`ShaderConstantPrelude` +
  `check_shaders.sh` in the same commit): `scan = ceil((reach + T/2) / T)`,
  `candMax = (2·scan+1)²` capped, refusing at load past the cap with the
  existing message. `MaxReachForNineCandidates` generalises to
  `MaxReachForCandidates(tile, scan)`.
- Cost: the per-column site scan grows from 25 to (2·scan+1)² cheap hashes;
  `treeInfoAt` still runs only for sites within reach. Measured with
  `--perf` on the overlook before/after; the budget is "worldgen per chunk
  within 10 %", recorded in `PERF.md`. If the great oak (115 vox reach)
  against a 56-voxel tile blows it, the answer is a species `reach` cap in
  the tree page, not a global tile.
- Delete `worldgen.treeTile` (row in `tuning_params.def`, `tuning.h`,
  `LoadTuning`, `tuning.json`, `tuner_schema.js`, `treeatlas.cpp:373`).
- Per-row `conditions` (minY/maxY/maxSlope/nearWater) on tree rows become
  live here: they are one compare each on data already in hand
  (`land.h`, `land.slope`, `pondNear`). Cover-row conditions the same.

### P-E  Shore, aquatic and cave flora per biome / per water preset

All outside the height mirror: table reads. The shore plant rows of a water
preset (`shore.plants[]`, `aquatic.*`) replace `TUNE_SHORE_*_CHANCE/HEIGHT/
REACH`, `reed*`, `lily*`, `kelp*`; cave flora chances (`caveMushroomChance`,
`caveCrystalChance`) become rows on the biome's `caves.features`.
`cactusChance`/`saguaroFraction` become a `cacti` cover row with a `shape`
field (the cactus is the one implicit shape that is not a material stalk).
`autumnFraction` moves to the species file. Delete the knobs.

### P-F  Water presets drive pond geometry (the mirror package)

`pondInfo/pondAt/pondNear/bermLift` sit inside `MIRROR-BEGIN height`, so:

- Per-biome water rows → a packed table: tile, rarity, radius min/span,
  depth, rim depth, berm height/width, shore band/lift, bed materials. The
  preset's bathymetry curve is sampled to a Q8 table of 17 knots at load.
- `World::TerrainHeight` reads the same table through a C++ twin with the
  same identifier spelling (the `POND_TILE → pondTile` normaliser alias
  already exists for this). `terrain` C1 (9,409 columns) is the proof;
  `terrain` A6 and the `waterbody` gate gain a `Profiled` bowl.
- Delete the nine `pond*` and four `shore*` geometry knobs.

### P-G  Terrain per biome and per map

- The per-biome relief curves (`curve<Biome>0..8`, four biomes × 9 knots,
  36 knobs) become `terrain.curve[9]` in each biome file; `terrain.
  overrides` (already parsed into `BiomeDef::terrainOverrides`, read by
  nothing) becomes a fixed record: hill/grain/detail amplitude multipliers
  in Q8. Inside the mirror → the twin reads the same record; `terrain` C1.
  Note the driver-compile stall trap recorded in memory: the curves must
  stay const-evaluable or arrive as a table read, never as 36 uniform reads
  in the inner loop.
- Per-MAP globals move to `map.json terrain`: `baseHeight`, the octave
  amplitudes and log2 sizes, `fbmAtten`, the sediment wedge, `treeline`,
  `homeArea` (from P-C), `refVoxelsPerMetre`. `seaLevelY` is already there.
  The World map page gets a "Terrain" section for them with the heightmap
  backdrop the follow-up list already wanted.

### P-H  The accuracy gate: `env-truth`

What makes "changes the game accurately" a tested claim:

- For every biome: pack a synthetic one-biome map in memory (flat landform,
  no sites), run worldgen over a 4-chunk window on a fixed seed, and measure
  from the GPU: trees per hectare (trunk columns at ground), cover fraction
  per row, skin and subsoil materials, bodies of water per km² (on a larger
  window, P-F). Compare to predictions from a C++ port of `biomegen.js`'s
  `densityStats`/`rarityStats` — held to the JS by `scripts/
  test_environment.mjs` exporting the same numbers to JSON and
  `check_invariants.py` comparing. Tolerances in `tests/baseline.json`.
- Runs under `--gate env-truth` alone (~10 s); the per-biome table is
  printed so a miss names the biome and the row.

### P-I  The Worldgen tab is deleted

After P-D..P-G nothing in `worldgen.*` is per-biome or per-map. What is left
(`mapLayer`, `editLayer`) moves to the World map page's map selector. The
tab's heightmap and voxel views move to the World map page as the preview
pane (they are previews, not authoring). `tuning.json worldgen` is empty and
the group goes; `ARCH_NODES` and the Development Status panel are updated.

## 3. Order and what each one costs

| pkg | build? | hash | gates to watch | est. |
|---|---|---|---|---|
| P-A apply loop | C++ | no | env-reload (new), worldmap | 1 session |
| P-B honest pages | JS | no | test_environment.mjs, check_environment.sh | ½ session |
| P-C spawn site | C++ small | yes | terrain A4, floaters, corpse-burn (fixtures unmoved) | ½ session |
| P-D tree spacing | WGSL + loader | yes | trees, scale, sleep, --perf worldgen | 1–2 sessions |
| P-E flora rows | WGSL + packer | yes | sleep, ca-skip, terrain D | 1 session |
| P-F water presets | WGSL + twin | yes | terrain C1/A6, waterbody, settle-back | 2 sessions |
| P-G terrain per biome/map | WGSL + twin | yes | terrain C1, driver-compile time | 1–2 sessions |
| P-H env-truth gate | C++ test | no | itself | 1 session |
| P-I delete the tab | JS + .def | no | check_invariants, tuner smoke | ½ session |

P-A and P-B first: they are the two that make every later package
verifiable by eye in seconds, and P-B alone stops the page lying while the
rest lands. P-H can start after P-D (it needs live tree spacing to have
something to measure) and grows a row per package.

## 4. Non-goals

- A per-biome lattice for trees (one lattice + thinning gives the same
  density; a second lattice would double the scan for no authored benefit).
- Rivers (PLAN_water_master §5 keeps them a named non-goal).
- Moving the harness pad or the fixture columns. Tests keep their ground;
  the player moves.
- Replacing the world map's painted biomes with climate noise. The map is
  the decision (`RESEARCH_worldgen` §8); the Environment tab paints it.
