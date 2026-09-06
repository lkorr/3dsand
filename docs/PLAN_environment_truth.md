# PLAN: the Environment tab is the truth about the world

Status: IN PROGRESS. Supersedes `PLAN_biomes.md` §5 (the wiring order) and
closes the follow-up list in `PLAN_world_map.md`. **Landed:** P-A, P-B, P-C
(2026-09-04), P-D, P-E (2026-09-05), P-F (2026-09-06: lakes are map content,
presets carve the bowl). **Open:** P-G, P-H, P-I, run in that order as one
worktree + one rebaseline each, under the §5 brief below (owner, 2026-09-05),
which widens P-F and P-G so the map — not tuning — is the whole environment.

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

### P-C  Spawn is a site on the map (C++ small, map.json) — LANDED 2026-09-04

- `map.json sites[]` gains `{kind: "spawn", at: [x, z]}`; `main.cpp` reads
  it for the game start and for `regen world` (`player.pos = {140,..,140}`
  at 3914 / 5177 goes). The default map puts spawn outside the harness pad,
  on forest, on land above sea level (the map's own landform plane says
  where). The harness pad keeps its box; fixtures do not move.
- The `spawn`-relative constants (`spawnPlainY/R/Fade`, the calm home area)
  centre on the spawn site instead of the origin. `spawnPlain*` moves to
  `map.json terrain.homeArea` in P-I.

What landed (branch `worktree-agent-afc028c1e63df3e08`):

- Loader: `worldmap.cpp` parses kind `spawn` (`at[2]`, one per map — a
  second refuses; absent → (140, 140) with a printed notice) into
  `WorldMapData::spawnX/Z/spawnAuthored`; packed as header words
  `kHSpawnX = 26` / `kHSpawnZ = 27` (`WM_H_SPAWN_X/Z`; the world-map layout
  check holds them together). Written even for an unloaded map so the two
  mirrors never disagree.
- Game: `SpawnPos()` / `SpawnWindowOrigin()` in `main.cpp`; boot and the F7
  regen block start there AND centre the residency window on the spawn
  before worldgen (`Stream::Update` shifts one chunk-plane per axis per
  frame, so a spawn 40 chunks from a window at the origin would otherwise
  stream to the player). Lab scenes keep their own origins. Selftest player
  proxies keep (140, 140).
- Home area: **two centres, not one.** The pad's flatness only ever came
  from the origin-centred fade (kind `pad` is not in the site table, so
  `sitePadAt` never levels it), and the fixture gates were written against
  that ground. `landAt` now takes `min(w_spawn, w_pad)` where `w_spawn` is
  the Chebyshev ramp from `spawnCentre()` (R + fade) and `w_pad` is the same
  fade measured from the box's EDGE (`harnessOutside()`, 0 inside; "far"
  when the map has no pad). Both accessors are outside the mirror, spelled
  identically in `world.cpp`; the mirrored token stream is unchanged
  between the two sides (`check_invariants.py` green).
- Default map: spawn `(900, 900)` — 260 vox past the pad's edge (widest
  crown reach is 115), inside the forced-forest cells, ground ≈ y200 over
  sea y112, no tarn. Also in `scripts/seed_worldmap.py` so a re-seed keeps
  it.
- Page: `map.js` draws the spawn diamond (dim + "default" when the map
  names none) and has a `Spawn` tool that moves it by click; saved with
  the rest, undo-able. P-B's LIVE manifest should list `sites.spawn` as
  read.
- Gate: `spawn-site` (after `worldmap` in `kOrder`; CPU only): authored,
  outside the harness box and every stamp site's cells, above `seaLevelY`,
  not under a tarn, not the ocean biome. Reports the distance past the box
  and the ground.
- Hash: moved (the home area moved), rebaselined once at the end.

### P-D  Tree spacing per biome, WITHOUT a per-biome lattice (WGSL + loader)

Status: LANDED 2026-09-05 (branch `worktree-agent-a1ee467469277c744`). As
built: `kB_TreeChanceQ16` (biome word 14), `kCoverRowWords` 8→12 with
`kC_NearWaterMax/Min`, the atlas's `kHCondTable` per (biome, species),
`TREE_TILE/SCAN/CAND_MAX` from `treeatlas.h TreeLatticeFor` (56 / 2 / 25 on
the shipped biomes, cap 5x5), `worldgen.treeTile` deleted. Measured
`--perf flythrough` worldgen 12.6 → 29.6 µs/chunk (+135 %, with the forest
23 → 153 trees/ha as authored): the reach cap named below is now due.

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

### P-E  Shore, aquatic and cave flora per biome / per water preset — LANDED

All outside the height mirror: table reads. As built (2026-09-05):

- **The water preset table** in the worldMap buffer: `kHWaterRecords` /
  `kHWaterCount` (header words 26, 27), one 32-word record per
  `assets/water/<name>.json` in loader (sorted file name) order — `kW_*` in
  `worldmap.h`, `WM_W_*` in the shader — carrying the FLORA half of the
  preset: `shore.mossChance/mossMaterial`, the `aquatic.emergent /
  floating / submerged` bands (material, chance, min/max depth, height,
  clearance, flower + flowerChance) and `kW_MaxPlantH`. Each record points
  at its `shore.plants[]` rows (8 words each, `kP_*` / `WM_P_*`: material,
  head, chance, reach, height). Words 22..31 of a record are reserved for
  P-F's geometry. `genCellIn` reads the bands and the shore rows from there;
  the shore rows roll in authored order, first hit wins, per-row salt,
  stalk jitter ±(H/6, ≥1) for H ≥ 3, head on the top max(1, H/8) cells.
- **P-E INTERIM — which preset a pond uses:** a disc pond has no preset of
  its own until P-F, so every pond and shore in a biome wears the preset of
  the biome's FIRST `water.features[]` row (`kB_WaterPreset`, 1-based; 0 =
  the biome has no water rows and grows no shore, pond or moss flora).
  Documented at `wmWaterOf` in the shader and `WaterPresetOf` in
  `worldmap.cpp`. P-F replaces this with the pond site's own preset.
- **Cave flora per biome:** `mushroomChance` on the `near_surface` row and
  `crystalChance` on the `deep` row of `caves.features[]` →
  `kB_CaveMushroomChance` / `kB_CaveCrystalChance` (1-in-N, 0 = never; no
  global default). `caveFloraAt` takes the biome.
- **Cacti per biome:** `cover.cactusChance` (percent of 2.5 m tiles) and
  `cover.saguaroFraction` (percent of those that are columns) →
  `kB_CactusChance` / `kB_SaguaroFraction`; `cover.cacti` stays the on/off
  flag. (The plan's "a `cacti` cover row with a `shape` field" was not
  built; two fields beside the existing flag is one surface, not two.)
- **Autumn:** the species file's `autumnChance` was already the roll; the
  global `worldgen.autumnFraction` (a scale whose default was the no-op) is
  deleted. No re-bake.
- **Heights:** `kB_MaxCoverH` and `kHMaxCoverH` now include the biome's
  preset's `kW_MaxPlantH` (tallest shore row with jitter, or the emergent
  height), so the sky-skip margin and the far cascade's blocker band cover
  every data-driven plant. `genChunk`'s `skyMargin` is
  `max(FLOWER_MAX_H, WM_B_MAX_COVER_H)` alone.
- **Biome record stride** `kBiomeRecWords` 16 → 32; the new words are
  16..20, 14..15 left for P-D.
- **Deleted knobs (19):** `autumnFraction`, `lilyChance`,
  `lilyFlowerChance`, `reedChance`, `reedHeight`, `kelpChance`,
  `kelpHeight`, `shoreCattailChance/Reach/Height`, `shoreSedgeChance`,
  `shoreHorsetailChance/Height`, `shoreIrisChance`, `shoreMossChance`,
  `cactusChance`, `saguaroFraction`, `caveMushroomChance`,
  `caveCrystalChance` — from `tuning_params.def`, `tuning.h`, `LoadTuning`,
  `tuning.json`, `tuner_schema.js`, and the generated `tuning_prelude.py`.
  `shoreBand`, `shoreLift`, `shoreMudWidth` and the `pond*` geometry stay
  for P-F.
- Seeded by `scripts/seed_flora_rows.py` (adds only absent keys, with the
  old global values). `assets/water/tarn.json` had `floating.minDepth 6 /
  maxDepth 3` (an empty band — no lilies on any tarn); set to the generator
  default 1 / 3, and `ValidateBiomeSet` now refuses an empty band.
- Not done here, deliberately: the shore band's EXISTENCE is still gated on
  `kBF_GroundFlora` in `genColumn` (so an oasis's shore rows never show in
  the desert) — that gate is the band geometry's and moves with P-F.

### P-F  Water presets drive pond geometry (the mirror package) — LANDED

Status: LANDED 2026-09-06 (branch `worktree-agent-a00095fe9e8995fe3`), as
widened by §5. As built:

- **The water record grew, 32 → 64 words** (`worldmap.h kW_*`): the P-E
  flora half stays at 0..21; the geometry half is 22..49 — radius min/span,
  depth, rim depth, berm height/width, shore band/lift/mud width, mud
  material, bed shallow/deep/substrate + shallow depth + thickness, the
  informational placement gates, `kW_Band = max(shore band, berm width)`,
  and **17 Q8 profile knots** two per word. `WaterGeomOf` is the ONE
  conversion (metres → voxels, curve → knots, ceilings), used by the packer
  and kept on `WorldMapData::water` for the CPU twin, so both sides read the
  same integers by construction.
- **The profile is parametrised by d²/r², not d/r.** The shader has no sqrt
  (rule 1) and evaluates the bowl from `dx²+dz²`, so the curve
  (`watergen.js profileAt`, ported to C++ as the load-time sampler) is
  sampled at u_k = sqrt(k/16) and interpolated linearly in d² between knots
  (`bowlDepth`). Knots are forced non-increasing and pinned 256 → 0, so the
  bowl is a bowl and the basin curve can invert it by bisection
  (`waterbody.cpp` `WaterBasinKind::Profiled` asks `World::BowlDepth`, the
  mirrored function, rather than restating the shape).
- **One pond lattice, thinned per biome** — the tree rule again. The
  lattice is the finest live `water.features[].tile` of any biome
  (`kHPondTile`, floor 6.4 m; a header word, not a prelude constant, so
  `check_shaders.sh` needed nothing). Each biome's rows are packed after its
  cover rows (`kR_*`: preset, `(T/tile)²/rarity` in Q16, minY/maxY/maxSlope)
  and `pondInfo` rolls them in authored order at the tile's site, first hit
  wins, conditions at the pond centre. `kB_WaterPreset` (the P-E interim) is
  gone: a pond wears the preset of the row that rolled it.
- **Authored lakes are site records** (`kSiteWater`, `{kind: "water",
  preset, at, radius?}`; `kS_Preset`): found per column through the same
  site index plane as stamps (`waterSiteNear`), `sitePadAt` skips the kind,
  and `siteKeepOut` tests a water site by DISC + band rather than by cells,
  so a lake does not bald four 102 m cells of forest. `pondCover` takes the
  authored lake first, then the rolled pond; `pondNear` scans the widest
  band any preset asks for (`kHPondBand`) and tests each candidate against
  its own. Rolled ponds keep out of every site by the disc's centre and four
  extremes (`pondKeepOut`). The shipped map gained one, `home_lake`
  (`spawn_lake` preset, east of spawn).
- **The repose pairing became a per-column decision.** `LoadTuning` used to
  clamp the depth against the smallest radius so the sand bed never sat on a
  face steeper than a voxel per column. Now `bowlSteep` (in the mirror)
  compares the bowl depth here against one voxel further out; a face that
  steep wears the preset's `bed.substrate` instead of its powder bed
  (`Col.bedSolid`), so a preset may be as deep as its author drew it and
  the world still settles. `ValidateBiomeSet` reports a steep preset that
  names no substrate. The cave-shell depth clamp is gone too: caves sit 40
  under each column's OWN (carved) height, so a bowl cannot breach them.
- **The bed and the mud are the preset's:** `bed.shallow` under water
  shallower than `bed.shallowDepth`, `bed.deep` below, `bed.thickness`
  cells, `shore.mudMaterial` on the mud ring; the fill is `fill.material`
  (none = a dry bowl, a playa). The shore band's existence follows the pond
  now, not `kBF_GroundFlora`.
- **Deleted knobs (12):** `pondTile`, `pondChance`, `pondRadiusMin/Span`,
  `pondMaxSlope`, `pondBerm`, `pondBermWidth`, `pondDepth`, `pondDepthRim`,
  `shoreBand`, `shoreMudWidth`, `shoreLift` — from `tuning_params.def`,
  `tuning.h`, `LoadTuning` (reads + the whole clamp block),
  `WorldgenDefaultsJson`, `tuning.json`, `tuner_schema.js`,
  `tuning_prelude.py`. `World::PondTileSize` is the lattice; `PondDisc` /
  `PondQuery` carry the preset's geometry so the gates and the registry read
  nothing by hand; `World::WaterSiteCount/Disc`, `BowlDepth`, `PondReachMax`
  are new.
- **Gates:** `terrain` A6 asserts against the query's own berm; `waterbody`'s
  bowl arm is `Profiled` on the tarn it finds; `spawn-site` reads a water
  site's cells as fine unless the spawn is under or on the shore of it;
  `biomes` refuses a row whose tile cannot hold its preset's widest disc or
  whose band exceeds half the lattice (swamp's marsh tile went 25.6 → 28.8 m
  for exactly that). `check_invariants.py` learned `kR_*` and the site kinds.
- **Map page:** a `Water` tool — click places a lake with the chosen preset
  and radius, drag moves it, shift-click deletes; the footprint and its band
  are drawn to scale. `envlive.js`: the water rows and the geometry fields
  are live; the shaped footprint, floor noise, `fill.level`, `coreFrac` and
  the per-preset ground are `later`; `placement.*` are the row defaults.
- **The driver's compile time is a budget, and this package spent it three
  times before learning the shape.** The first cut read the table inside
  `pondInfo` and let the tree scans inline it: worldgen's cold compile went
  from ~5 min to never (26 min once, killed at 10 GB four times). What
  finally held it: (1) the pond lattice is a PRELUDE CONSTANT (`POND_TILE`,
  `gpu/resources.cpp`, mirrored by `scripts/pond_lattice.py` for
  `check_shaders.sh`; `Simulation::UploadEnvironment` recompiles when a
  reload moves it) because worldgen divides by it in ~1000 inlined places
  and a division by a buffer word there is a full expansion each time; (2)
  the radius roll is a multiply-and-shift, never a modulo by a table word;
  (3) the biome rows roll UNROLLED over a cap of four (`kWaterRowsMax`),
  never a buffer-bounded loop with a break; (4) **a column's pond
  candidates are scanned ONCE** (`pondScan` → `PondSet`, five named slots,
  the site plus up to four rolled tiles) and handed BY VALUE to the bowl,
  the shore, the tree/cactus scans and the near-water conditions, which
  are pure arithmetic on it — `pondRoll` (thirty table reads) was being
  inlined ~500 times per column path through the four 25-tile tree scans;
  (5) exactly two `landAt` per column for ponds (`pondGate` on the covering
  candidate and on the nearest shore candidate), none in the scans: trees
  and cacti refuse a CANDIDATE's disc and conditions measure distance to a
  candidate, ungated, which costs a tree on a refused hillside tarn now and
  then. Design rule for anything that reads the worldMap buffer from the
  height path from now on: resolve it per COLUMN into a value and pass the
  value into the per-candidate scans.
- **Not built, deliberately:** `fill.level` (a part-full body; the waterline
  is the rim ground); the harness tarn at (420,420) stays the authored pool
  in `landColumnBare` — it never came from `pondInfo`, and its flat stone
  floor is what the fixtures were written against (a `spawn_lake` preset
  with the same numbers exists for the day it becomes a site).

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

## 5. The map IS the environment (owner brief, 2026-09-05)

The owner's goal, in their words: *cleanly and easily declare what the
entire environment will look like — where the biomes are, how the terrain
looks — seeded to look slightly different but largely consistent: there is
always a mountain to the east, always a lake in region X. A comprehensive
map editor that controls the entire map, and later the locations of notable
buildings and towns.* The three-tier rule in `PLAN_world_map.md` is exactly
this: **Tier A (the map) is seed-independent and authored; Tier B/C is
seeded detail.** Every remaining package is judged by one question: after
it lands, is that fact declared ON THE MAP PAGE, and does the game show it?

What the remaining packages therefore deliver, beyond §2:

### P-F, widened: lakes are map content

- **Two sources for one pond table.** (a) **Authored water sites**, Tier A:
  `map.json sites[]` gains `{kind: "water", id, preset, at: [x, z], radius?,
  rotation?}`. Same on every seed; the preset's footprint / bathymetry /
  fill / berm / bed / shore geometry applies; a site may override the
  preset's radius. (b) **Rolled ponds**, Tier B: the biome's
  `water.features[]` rows (preset, tile, rarity, conditions) roll per tile
  with the seed, exactly as trees do. Both go through the SAME packed
  record (`kW_*` words 22..31 + the 17-knot Q8 bathymetry table) and the
  same shader path, so an authored lake and a rolled tarn differ only in
  where their centre came from.
- **A site wins its ground.** Inside an authored water site's footprint +
  shore band no rolled pond, tree, cover row or cave breach is placed
  (the `siteKeepOut` discipline the pad already has).
- **Inside the height mirror** (`pondInfo/pondAt/pondNear/bermLift`): the
  C++ twin reads the same table with the same identifier spelling;
  `terrain` C1, A6 and `waterbody` (with a `Profiled` bowl) are the proof.
  The bathymetry `profile` curve is sampled to Q8 knots at load — the
  shader never sees a float and never sees 30 uniform reads in the loop
  (the driver-compile stall recorded in memory).
- **Map page:** a `water` tool beside spawn/stamp: click places a lake,
  drag moves it, a side panel picks the preset and radius, the footprint
  is drawn to scale. The `spawn-site` gate's checks extend to "not under
  an authored lake". P-E's interim `kB_WaterPreset` (first row wins) is
  replaced by the site's / the roll's own preset.
- **Delete** the nine `pond*` and four `shore*` geometry knobs; the Water
  pages' greyed geometry fields go live (`envlive.js`).

### P-G, widened: the terrain's shape is authored on the map

- The landform plane is already the continental rung inside the mirror;
  give it authority. `map.json terrain` gets `landformRangeVox` (what a
  painted 0..255 spans, so a painted ridge can be a 150 m mountain, not a
  40 m swell), the octave amplitudes and log2 sizes, `fbmAtten`, the
  sediment wedge, `treeline`, `homeArea` (P-C's spawnPlain*),
  `refVoxelsPerMetre`. The map page gets a **Terrain** section for them and
  a **heightmap backdrop** (the `--heightmap` route already exists) so the
  painted landform is seen as relief, not as a colour.
- Per-biome relief: `curve[9]` and the hill/grain/detail Q8 multipliers in
  each `assets/biomes/<name>.json terrain` block (the parsed-but-unread
  `terrain.overrides` becomes that fixed record). Inside the mirror →
  twin + `terrain` C1. Curves stay const-evaluable or arrive as a table
  read.
- **Landform sites, Tier A** (the "mountain to the east" as a declared
  thing rather than a painted blob): `{kind: "landform", shape: peak |
  ridge | basin | plateau, at, radius, heightVox, rotation?}` overlaid on
  the plane at load (CPU, into the packed landform plane — the shader
  reads the plane as today, so nothing new enters the mirror). The map
  page draws and drags them.
- **Delete** the 36 curve knots, the octave/sediment/treeline/spawnPlain/
  ref rows, and the seven dead-or-duplicate rows still in `worldgen.*`:
  `biomeLog2`, `desert/pine/meadowThreshold`, `biomeBlend`,
  `caveThreshold1/2` (nothing reads `TUNE_CAVE_THRESHOLD*` since P-E) and
  `alpineChance` (the alpine biome already authors an `alpine_cushion`
  cover row that the cover stack's `h < TREELINE` gate silently ignores
  where the alpine biome lives — lift the gate, let rows use `minY`).

### P-H as planned; P-I, widened: the map page is the one front door

- P-I deletes the Worldgen tab and makes Environment → World map the
  single entry: map selector (`mapLayer`, `editLayer`), Terrain section,
  the heightmap + voxel views as a preview pane, and a **sites panel**
  listing every site by kind (pad, spawn, water, landform, stamp) with
  select / rename / delete — the list that towns and notable buildings
  will join (`proc:` kind, `PLAN_world_map.md` follow-ups). The
  `worldgen.vegetation` A/B switch moves to a dev group; it is not world
  content.
- The game keeps printing `environment: map <name> <hash> | biomes | trees`
  and F7 keeps meaning reload + regen, so the loop stays edit → F7 → see.

### Coordination

One implementer per package, sequential (they all touch `worldgen.wgsl`,
`world.cpp`, `worldmap.*`, `map.js`); each claims on the board, works in a
worktree, builds ONCE, verifies with ONE `--verify`/`--suite acceptance` at
the end, rebaselines once, updates this file's status line and the tuner's
`ARCH_NODES` + Development Status, and merges to main. The seven-knob
cleanup goes with P-G. `env-truth` (P-H) grows a row per package after it
exists.
