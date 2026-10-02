# PLAN: map / worldgen overhaul (audit follow-through)

Date: 2026-09-26. Orchestrated: one orchestrator session plans, reviews and
merges; Opus worktree agents implement one package each.

**Status (2026-10-02 audit): P1–P7 ALL LANDED on main 2026-09-26** — P1 `b570227`,
P2 `1d5a51a`, P3 `51657b7`, P4 `0e9b59e`, P5 `5755e59`, P6 `86c19b2`, P7 `b5359be`
(`map-overhaul` merged with main at `85b1118`).

**Goal of the owner:** generate an interesting base terrain, paint biomes,
then HAND-SHAPE the terrain and add stuff (structures, water, trees). The audit
(2026-09-26) found the generator mostly sound but: no mid-scale sculpting tier,
the test harness living inside the shipped map, a broken edit->preview->play
loop in the tuner, hash-neutral perf waste, and rewrite leftovers.

## Rules for every package

- Work ONLY in your worktree. First command: `git reset --hard <BASE>` where
  BASE is the sha the orchestrator gives you (the integration branch
  `map-overhaul`), then read CLAUDE.md and this file.
- Commit on your worktree branch (small commits fine). Do NOT touch `main`, do
  not merge, do not push. The orchestrator merges.
- Never `git stash`. Never edit `AGENTS_BOARD.md` directly.
- Build with `bash scripts/build.sh` (C++ packages build ONCE, then only again
  after further C++ edits). WGSL/JSON/JS-only packages NEVER build: run the main
  checkout's exe with `SANDVOX_ASSET_DIR=<worktree>/assets` via
  `bash "<main>/scripts/run.sh"`, from your worktree dir.
  Main checkout: `C:/Users/Luke/Desktop/programming/3d sand voxel`.
- Always `export SANDVOX_NO_CRASH_DIALOG=1`. Always wrap runs in `scripts/run.sh`.
- **Testing budget (owner directive):** no `--suite acceptance`, no bare
  `--selftest`, no `--vk-smoke*`. Finish with ONE `--verify <gates>` boot
  naming the gates that cover your change. Static checkers
  (`check_shaders.sh`, `check_invariants.py`, `check_pass_table.py`,
  `node scripts/test_environment.mjs`) are free — run them per edit.
- A `worldgen.wgsl` logic edit costs ~45 s+ of pipeline compile on the next
  run (and the `far` entry more; see "compile" notes in CLAUDE.md memory). Do
  not iterate by run; reason first, batch edits, run once.
- **Hash-neutral packages** (marked HN) must leave `determinismHash`
  unchanged: record the BEFORE hash with one `--selftest --gate determinism` run
  at BASE (or read it from `tests/baseline.json` if BASE's pin is current —
  check `build/last_run.json`), and the AFTER in your final `--verify`. A moved
  hash in an HN package is a bug in your change: find it, don't rebaseline.
- **Hash-moving packages** (marked HM): rebaseline ONCE at the end with
  `--selftest --gate determinism --rebaseline` (plus any gate whose pinned
  numbers you intentionally moved). Do not investigate expected moves.
- Update `DESIGN.md` in the same commit if you contradict it; update comments
  you make stale. Delete, don't comment out.
- Final report (your last message): commits, files touched, what was
  verified (gate lines), what you deliberately did NOT do and why, any
  follow-ups. Under 400 words.

## Waves

| Wave | Packages | Why this order |
|---|---|---|
| 1 | P1 tuner loop + hygiene, P2 kernel cleanup + cheap perf | disjoint files |
| 2 | P3 harness map split, P4 streaming/tree caches | both need P2's cleaned kernel; P3 = landAt/landColumnBare/harness, P4 = genChunk/far/tree scans |
| 3 | P5 sculpt height layer | needs P3 (harness out of landAt) |
| 4 | P6 sites, P7 edit layers + biome swatch | build on P5's map format |

---

## P1 — Tuner edit loop bugs + authoring hygiene (HN, C++ small)

1. **Apply == F7.** `src/main.cpp:~14322` telemetry `apply-environment` sets
   only `ui.regenWorld`; F7 (`~9366`) also sets `ui.reloadShaders` (which
   reloads tuning -> mapLayer/editLayer and the `.svedit`). Make Apply do what
   F7 does.
2. **Voxel preview staleness.** `_world_signature()` in
   `scripts/tuner_server.py:~702` hashes only tuning.json + materials.json. Add
   the active worldmap dir (map.json + map.svmap), `assets/biomes/`,
   `assets/water/`, `assets/trees/*.svtree`, and the active `.svedit`. After a
   save on the World map / biome / water / trees pages, POST `/api/voxreload`
   and invalidate the worldview (`wv.invalidate()` or equivalent). Today
   `/api/voxreload` is sent only when tuning is dirty (`tuner.html:~4829`).
3. **Previews render the map open on the page**, not the one named in
   tuning.json on disk: `--heightmap` and voxserve read
   `CurrentTuning().world.mapLayer` (`main.cpp:~5086`). Pass the map name
   explicitly from the page (a request parameter) or save-then-render
   consistently; pick the simpler honest one and document it.
4. **Stale badge**: include the edit layer in the environment stamp so a
   `.svedit` change marks the game stale.
5. **Delete the dead tree-weight mirror.** The engine builds weights from the
   biome files (`src/sim/treeatlas.cpp:~113`, `biomes.cpp:~521`); `.svtree`
   words 12..15 are unread. Remove: the Sync-atlas button + stale-weights nag
   (`assets/editor/biome.js:~678-720`), `seed_environment.mjs --sync/--check`,
   `scripts/test_environment.mjs` check 3 (~163-171, its header claim is false),
   the `placement.biomes` bake + 4-entry `BIOME_ORDER` in `treegen.js`
   (~1375, ~1502). Stop WRITING the words (write zeros or keep the header
   layout — do not change the .svtree format version unless trivial); do NOT
   re-bake shipped .svtree files in this package. Strip `placement.biomes` from
   `assets/trees/*.json` only if nothing reads it. Fix the CLAUDE.md layout row
   for `assets/biomes/` (it says worldgen reads weights through the atlas mirror
   — false). CLAUDE.md is shared: keep the edit to that one row.
6. **Stamp picker**: `map.js:~514` creates stamps via `prompt()` with a typed
   `.vox` name and no existence check (a missing .vox stops the engine boot).
   Replace with a dropdown of `assets/prefabs/*.vox` (existing `/api/models` or
   a new listing route). Also make the engine's loader report a missing stamp
   `.vox` as a load warning and skip the site instead of refusing to boot, if
   that is a small change in `worldmap.cpp` — if not small, leave it and say so.
7. **Hygiene:** `scripts/seed_worldmap.py` must refuse to overwrite an existing
   map without `--force` (it would clobber the painted map; its spawn is stale).
   Delete `scripts/seed_terrain_rows.py` and `scripts/shore_probe.py` (dead;
   check nothing imports/calls them — `check_shaders.sh` may call the python
   mirrors, keep `map_terrain.py`/`pond_lattice.py`/`tree_lattice.py`).
   Collapse `TERRAIN_DEFAULTS` (`map.js:~68`, `seed_worldmap.py:~115`) to one
   source the server serves or the page reads from the map. Fix stale headers:
   `map.js:~30`, `biomegen.js:~28`, `watergen.js` header (engine DOES read
   presets since P-F; `scripts/test_watergen.mjs` doesn't exist),
   `tuner_server.py` `_heightmap` comment, `assets/worldedits/README.md`
   (`world.editLayer`, no "Worldgen tab"). Fix stale `about` strings in
   `assets/worldmap/default/map.json` (spawn names a dead knob; home_lake is
   not "east of spawn").
8. Map page: `paint()` calls `rebuildImage()` on every pointermove
   (`map.js:~273`) which rebuilds the whole plane image and makes temp
   canvases for colours; cache the palette, rebuild only on plane change /
   dirty rect. Heights button refresh needs two clicks (`map.js:~973`): fix.
   Heights request hardcodes `seed=1337` (`map.js:~849`): use the page's seed.

Verify: `node scripts/test_environment.mjs`, `bash scripts/check_environment.sh`
and `bash scripts/check_worldview.sh` if they run headless here; one
`--verify` with `worldmap,biomes,voxregion` (build once for the main.cpp
change). Files: main.cpp (one hunk), tuner_server.py, tuner.html (glue only),
assets/editor/{map,biome,envlive,biomegen,watergen,treegen}.js, scripts/*,
worldedits README, map.json about strings, CLAUDE.md (one row).

## P2 — Worldgen kernel cleanup + hash-neutral perf (HN)

Subject `assets/shaders/worldgen.wgsl` (5,676 lines; ~2,900 are comments,
much about deleted systems) and its C++ satellites.

1. **Cactus sky cost.** Cactus biomes never take the sky early-out
   (`genChunk` ~4684: `skySkippable = !wmFlag(col.biome, WM_BF_CACTI)`);
   `cactusAt` runs per air voxel (~230 hash3 each). Add a cactus ceiling (the
   tallest possible cactus top above ground, like `treeMaxTop()`), fold it into
   `colTop`, and let cactus biomes skip. Must be EXACT (same voxels).
2. **pondScan twice**: `genColumn` discards `LandCol.ponds`; `genChunk`
   re-runs `pondScan` (~4694). Reuse it (also check ~4349, ~4366, ~4450, ~5222).
3. **Buried chunks** still run tree scan + pond scan (`base.y <= treeMaxTop()`
   is true underground, ~4715). Skip when the whole chunk is below the column's
   ground (`base.y + CHUNK - 1 < col.h - margin` where no tree/pond/cave rule
   needs them — prove exactness from the genCellIn rules).
4. **Worldgen-only prelude**: `TREE_TILE`, `TREE_SCAN`, `TREE_CAND_MAX`,
   `POND_TILE`, `REF_VOXELS_PER_METRE` are emitted into the prelude every shader
   gets (`src/gpu/resources.cpp:~422-441`); only worldgen.wgsl reads them, so a
   biome/water edit recompiles every shader. Emit them only for worldgen (keep
   `scripts/check_shaders.sh` in agreement). Verify no other shader reads them.
5. **Dead code**: never-called `isin16` (~405), `vnoise3` (~427) and its
   `[[maybe_unused]]` twin in `world.cpp:~1201` (keep check_invariants token
   compare green — adjust the mirror blocks consistently), `genCell` (~4346),
   `caveAt` (~3037); unused consts `B_FOREST..B_DESERT` (~270), unused `M_*`
   ids (verify each with grep across all shaders), `HSCALE` (~482). Remove the
   dead `kEngineBiomes` validator (`src/sim/biomes.cpp:~21, ~444-450`).
   Retire `SANDVOX_GEN_VERDICT_CHECK` (`src/test/support.cpp:~2339`).
6. **Dead knobs**: a water row's `patchThreshold` (packed at `worldmap.cpp:~349`,
   ignored by `pondRoll`), water preset `kW_*` 37-39 (maxSlope/minY/maxY packed,
   unread), `kB_SedMax` (always 0). Either wire each (only if trivially
   hash-neutral given current data — e.g. data at a value where the rule is a
   no-op) or delete from packing AND from `envlive.js`'s live list / the
   schema so authors aren't offered dead fields. Prefer delete.
7. **Comment archaeology**: rewrite the file header (~1-29) to describe the
   kernel as it IS (entry points main/list/pagefill/far/farpatch/fardown, the
   column->cell pipeline, where map data enters). Remove comments about deleted
   systems (ruins, arena, deck, Poi, oil/lava pools, wood platform, `pondAt`,
   `pondInfo`, `shoreAt`, `flowerAt`, `pondSurface`, `surfHeightAt IS GONE`
   ~4371-4380, "birch skeleton" ~2309 -> rename to what it serves, "biome ==
   B_DESERT" ~4764). Keep invariant/why comments; drop "measured on date X"
   history unless it justifies a non-obvious choice. Target: comments explain
   the current code. Same pass for stale names in C++: `worldmap.h:~8-9,48-52`,
   `worldedit.h` (`worldgen.editLayer`), `main.cpp:~5249`, `world.cpp`
   `AuthoredPoolList` comment (3 pools -> 1), DESIGN.md §9d "`pondAt`'s
   parabolic disc". Add SUPERSEDED/STATUS lines to the tops of
   `docs/PLAN_biomes.md`, `docs/PLAN_terrain_overhaul.md`, and mark
   `docs/MEASURED_terrain_baseline.md` historical.
8. Collapse near-duplicate helper pairs only where trivially exact:
   `pondCovers`/`pondCoversP`, `shoreD2`/`candD2`. Skip anything that needs
   thought — P4 does the structural merges.

Do NOT touch the harness box / (420,420) lake code (P3 owns it) or restructure
genChunk/far (P4). Verify: `check_shaders.sh`, `check_invariants.py`; build
once; one `--verify determinism,terrain,worldmap,biomes,far-downsample`
showing the hash unchanged. Report the WGSL line count before/after.

## P3 — Test harness out of the shipped map (HM)

The `harness` pad site (-128..640) lives in `assets/worldmap/default/map.json`
and the shader special-cases it (`inHarness`, `crownMeetsHarness`,
`harnessOutside` feeding the home-area fade in `landAt` ~840-858, ~1769-1838).
The (420,420) test lake is HARDCODED: `worldgen.wgsl` `landColumnBare`
(~3400-3440, `inPoolFloor`/`inRim`) plus four retyped copies in `world.cpp`
(~2033 mirror, ~2111, ~2133, ~2239), outside the token compare.

1. Create `assets/worldmap/harness/` (map.json + map.svmap) that reproduces the
   ground the fixtures need: the pad, the lake as a real `water` site with a
   preset that gives the geometry the `waterbody` gate needs, forest at the
   origin, whatever the `terrain` gate bounds need. Selftest loads it (the
   env-truth gate's synthetic-map path, `selftest_envtruth.cpp:~212`, shows how
   to load a non-default map). Decide cleanly how selftest picks the map
   (e.g. a `SANDVOX_MAP`/`world.mapLayer` override the harness sets) — gates
   must NOT depend on the game's `default` map afterwards.
2. Delete the hardcoded lake from WGSL and all C++ copies; delete the harness
   special cases in the kernel if the pad site kind alone can express them (a
   generic `pad`/calm-area site the map authors can use is fine — the harness
   map uses it; the default map does not have to).
3. Remove the harness pad from `default/map.json`. Spawn site stays.
4. `pond-shore` (`selftest_ca.cpp:~1400`) pins a procedurally ROLLED desert tarn
   at (-2317,112,1674): move it onto an authored water site in the harness map.
5. Move `kDefaultSeed` and the game's startup `SubmitWorldgen` out of
   `src/test/support.*` into sim/game code (layering), without changing
   behaviour. Hardcoded fixture coords in `main.cpp` (~1633, ~1879, ~3564): if
   they are debug harness modes, leave them but make sure they still land on
   the harness map ground when the harness map is used; note in report.
6. `spawn-site` gate keeps validating the DEFAULT map's spawn (it is a map
   validator, not a fixture) — keep it pointed at default.

HM: rebaseline once at the end. Verify with one `--verify` of the gates that
read the harness ground: at least `terrain,waterbody,pond-shore,worldmap,
spawn-site,envtruth,determinism` plus any fixture gates you find reading the
pad (grep `FixtureSite`, 420, harness). Gates that were red at BASE stay out of
scope — compare against a BASE run only for gates you suspect.

## P4 — Streaming + tree caches (HN, perf)

1. **Per-plane column cache.** `genChunk` calls `genColumn` once per column per
   chunk; a streamed X/Z shift plane is 32 chunks tall, so each (x,z) column is
   evaluated ~32x (262k evaluations, ~8k unique). Add a pre-pass that evaluates
   each unique column of the gen list once into a scratch buffer (height,
   biome, fluidTop, pond, colTop, whatever genChunk reads), then genChunk reads
   it. The same buffer gives a GPU-side sky test (chunks entirely above colTop
   write zeros without the column work). `worldgenList` is ~98% of the
   streaming GPU bill (DESIGN.md / docs/RESEARCH_streaming_hitch.md). Pass
   table R/W sets must match bindings (`check_pass_table.py`).
2. **Tree tile cache.** Three scans of the same 25 tiles
   (`treeCandsInto`, `undergrowthSite`, `treeCanopyAt`) recompute each tree
   ~300x per chunk (~1,200 hash3 per forest column vs ~16 for the ground).
   Evaluate the chunk's tree tiles once into workgroup memory (or the column
   pre-pass buffer) and have all three read it. This also reduces the inlined
   copies the driver compiles (`far` compile cliff).
3. Merge the duplicated column setup of `genChunk` (~4668-4812) and `far`
   (~5218-5292) into one function; keep `farBlockerBitAt` and its inline copy
   (~5205-5216, ~5304-5309) as one.
4. Stamp-site pad height: `sitePadAt`/`wmSiteTopAt`/the stamp overlay recompute
   `landColumnBare` at the site centre per voxel (~4319, ~3504, ~1880). It is
   seed-dependent but position-fixed: compute once on the CPU at map load
   (the CPU mirror `World::TerrainColumn` exists) and pack into the site
   record. Must be exact (the mirror is token-compared).
5. `--voxserve` coarse LOD: a LOD-L region generates every chunk at full res
   and downsamples on CPU (`src/tools/voxregion.cpp:~252-310`). Skip chunks
   wholly above the column top / below bedrock-irrelevant depth using the CPU
   `TerrainColumn` (+ tree/cover clearance), same output.

Measure BEFORE/AFTER with the existing streaming perf harness
(`--perf` / `--autofly-hard` / whatever `docs/RESEARCH_streaming_hitch.md`
used) under `SANDVOX_RUN_EXCLUSIVE=1` — one run each side, quote the
worldgenList pass time. Record the cold compile time of worldgen entries
(`SANDVOX_SHADER_TIMING=1`) before/after. Verify hash unchanged with one
`--verify determinism,terrain,worldmap,far-downsample,voxregion,plants`.
Do NOT touch `landAt`/`landColumnBare` internals or the harness/lake code
(P3 runs concurrently there).

## P5 — Sculpt height layer (HM only if data non-empty; code HN with empty layer)

The missing tier between 100 m map cells and single voxels. A sparse, tiled,
signed height-offset layer (plus optional per-tile material override later —
height first) sampled inside the ONE height function, so everything follows:
CPU `World::TerrainHeight`, spawn, trees, ponds, sediment, cover, far field,
voxserve, heightmap.

1. Storage: a per-map file (e.g. `assets/worldmap/<name>/sculpt.svsculpt`),
   sparse tiles (e.g. 64x64 samples at 4 vox spacing; choose and justify),
   i16 offsets in voxels, bilinear sampled. Loaded by `LoadWorldMap`, packed
   into the worldMap buffer (or a new binding if size demands — size it for
   the whole 20 km map being sculpted in places, with a tile directory so
   empty tiles cost nothing).
2. Applied in `landAt` AFTER the seeded octaves and AFTER slope attenuation
   (the author's shape must not be scaled by noise), BEFORE the sediment wedge,
   ponds, sea and site pads, so those react to the sculpted ground. Decide
   whether the calm-area fade applies (it should not scale the author's
   offset). Mirror in `world.cpp` inside the token-compared blocks;
   `check_invariants.py` green.
3. Empty layer => byte-identical world (hash unchanged). Prove it.
4. Also expose an authored per-map toggle to scale down the seeded range
   octave in sculpted areas? — NO; keep scope. But DO document the relationship
   (rangeAmplitude +-256 vs landform unit 4) in the map page help text.
5. Tuner: World map page gets raise / lower / smooth / flatten(to sampled
   height) / noise-erase brushes painting the sculpt layer, with brush radius
   and strength, undo/redo, save. Show the heightmap preview live (the
   `--heightmap` route evaluates `World::TerrainColumn`, so it will include
   the layer). If practical, the same brushes in the 3D voxel view (worldview.js)
   editing the layer (not voxels). Server route to save/load the file
   (atomic write-then-rename like the others). Include the file in the
   environment stamp, the worldgen fingerprint (`worldio.cpp:~356-402`) and the
   voxserve signature.
6. New gate `sculpt`: a synthetic map with one sculpt tile, assert
   `TerrainHeight` == GPU column height over it (the C1-style per-voxel check),
   that far field sees it, and that an empty layer changes nothing.

Verify: `check_invariants.py`, `check_shaders.sh`, one `--verify
sculpt,terrain,worldmap,determinism,far-downsample,voxregion`.

## P6 — Sites you can trust (HN for existing data)

1. Site index is "first site wins a 100 m cell" (`worldmap.cpp:~1058`), cap
   254: replace with a short per-cell site list (e.g. up to 4 per cell + an
   overflow error at load), so nearby stamps/water don't silently vanish.
   Exact for the current map (no overlaps today — verify).
2. Load-time warnings (printed + surfaced in the tuner) for: unknown site
   kinds (currently silently ignored, `worldmap.cpp:~814-908`), overlapping
   footprints, stamp .vox missing (skip, don't refuse boot — if P1 didn't).
3. Stamp sites use `x`/`z` while every other kind uses `at[]` (`~848`): accept
   `at[]` (keep reading x/z for old files or migrate the shipped files).
4. New site kind `tree`: species + optional variant at a column, placed on the
   ground, kept out of the procedural tree lattice's way (siteKeepOut).
   Worldgen input (like stamps), visible in far field.
5. Map page tools for the new bits (tree site placement; warnings panel).

Verify: one `--verify worldmap,determinism,spawn-site` + a new/extended
`worldmap` gate case for overlap + tree site.

## P7 — Edit layers that survive, and a real biome swatch

1. **Ground-relative `.svedit`.** Today edits are absolute words at absolute
   coords (float/bury when the ground changes). Store each edit column's cells
   relative to the procedural ground height at that column
   (`World::TerrainHeight`, which after P5 includes sculpt), convert at apply
   time. Keep reading the old absolute format (version bump).
2. **editLayer belongs to the map**: move `world.editLayer` from tuning.json into
   `map.json` (`editLayer` key); migrate; tuner selector moves with it.
3. **Apply the layer as worldgen, not as player edits**: layer chunks must not
   be marked modified/baked into saves (`stream.cpp:~1081`,
   `session.cpp:~3747`), must reach the far field on load (feed FarEdits or a
   far patch list), and `--voxserve` must apply it (today the tuner overlays
   client-side, `worldview.js:~1882`). Prefer the smallest honest mechanism;
   the CellOp path can stay if the "modified" marking and far-field gaps are
   closed. It must stay within rule 3 (mutations through the queue) — say
   which exception clause, if any, you rely on.
4. **Export in-game edits to a layer**: a dev command (F-key or telemetry) that
   writes the modified chunks' diff vs the generator into a `.svedit`.
5. **Save selection as prefab `.vox`** from the voxel editor (worldview.js
   selection -> `assets/prefabs/<name>.vox` via a server route) so a
   hand-built structure becomes a reusable stamp.
6. **Biome swatch = the engine**: replace `biomegen.generateSwatch`
   (`biomegen.js:~427-600`, its own noise/trees/cover — a picture the engine
   never makes) with a voxserve render of a synthetic one-biome map (the
   env-truth gate already builds one). Keep `densityStats` numbers if
   `tests/env_predictions.json` pins them. Delete the dead JS.

Verify: one `--verify worldedit(or the existing edit-layer gate),voxregion,
save-load,determinism` + `check_worldview.sh`.
