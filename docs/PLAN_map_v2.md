# PLAN: world map v2 — topographic height, fine planes, roads, locales, Skyrim-scale mountains

## Status (2026-10-08): ALL 11 PACKAGES LANDED on `map-v2`

| Package | Merge |
|---|---|
| E1 map page core | a76b898 |
| E2 height tools | a752b12 |
| F1 format + authored height | d908d7f |
| L1 locales (engine) | 7f9eeea |
| A1 altitude is relative | 2e87ebc |
| R1 roads (engine) | 223f298 |
| H1 far horizon + height fog | 5b23269 |
| E3 vector layers editor | 5a65697 |
| S1 terrain light + clouds vs terrain | 5e12be8 |
| E4 locale editor | 86fb920 |
| P1 tall-terrain perf | 7c35116 |

Docs: `docs/EDITOR_GUIDE.md` §15 (the owner's how-to), DESIGN.md (the
world-map list's "map v2" entries, §9g locales, the far horizon / terrain
light sections), `CLAUDE.md` layout rows, `assets/tuner.html` ARCH_NODES.

**Open follow-ups:**

- **A1:** the tundra snow-skin edge slide (heat-ambient D): owner decision.
- **R1:** junctions are not profile-matched (two roads meeting at a vertex
  each bake their own Y; the ground blends them in table order).
- **S1:** ~~the sun-horizon rebuild is a ~3 ms spike about once a second~~
  measured 2026-10-13 at the 12-10-09 camera: 65-67 rebuilds per 1,200
  frames (one every ~0.3 s), median 0.77 ms, worst frame 1.35-3.13 ms; now
  SLICED over 8 frames (median 0.22 ms, worst 1.32-1.48 ms, same total;
  main 7ed7b20). Open: raster bodies (debris / microbodies, mobs) ignore
  the terrain shadow; the sea's LOD-handoff seam (`shadeWater` vs
  `shadeFarWater`).
- **P1:** a far column cache (the far list sits at its cap on `peaks-route`;
  the far sweep's column stack, 330e762, shares the prologue); the page pool
  ~~reaches 88.5 %~~ peaks at 81 % on `peaks-route` since the full-chunk
  revisit retries refused demotes above 75 % residency (main b8da14e;
  84.5 % before it on the same tree); liquid sentinels (16,384 sea chunks
  resident and a ~200 ms first tick on the sea window) are still open: they
  need a UNIFORM sentinel that carries the state nibble (world.h SynthWord,
  common.wgsl synthWord, the worldgen verdict, Classify) and a birth wake
  that skips an interior liquid chunk.
- **Pre-existing, not map v2:** `check_pass_table.py` fails on `sim_fluid`'s
  `fluidAlloc` binding (already failing at BASE).
- Out of scope, unchanged: rivers, generated towns, overhangs, extents past
  ~26 km.

Date: 2026-10-08. Orchestrated: one orchestrator session plans, reviews and
merges onto the integration branch `map-v2`; Opus worktree agents implement one
package each.

**Goal of the owner:** design a fantasy world BY HAND on the tuner's World map
page and have the game build it. Concretely:

1. **A topographic map.** An absolute height plane the author paints in metres
   (hillshade + contour lines), so "this summit is 1,100 m, this pass 620 m" is
   a decision, not a noise outcome. Mountains as tall as Skyrim's: peaks up to
   ~1,500 m above the sea, cliffs, gorges.
2. **Much finer map resolution** — biomes at 25.6 m, height at 6.4 m (16x the
   current 102.4 m cell), and a site index that stays coarse.
3. **Vector features:** roads (polylines with width, surface, grading), ridge /
   valley lines that shape the height plane, named region outlines, and markers.
4. **Locales:** a marker on the world map ("witch's hut") that opens a zoomed-in
   local editor where the author hand-sculpts terrain, paints ground, places
   structures and edits voxels. A locale moves WITH the world if the ground
   under it is later reshaped.

Survey facts this plan rests on (2026-10-08, four read-only surveys; file:line
references are at survey time and drift — re-find them):

- `.svmap` v1 = 16-byte header + three u8 planes (biome, landform, moisture),
  196x196, `cellLog2` 10. **Moisture has no reader anywhere.** The landform byte
  is a ±`landformRangeVox/2` (±51 m) BIAS through `landformOctave`, then
  `biomeCurve` reshapes `o0+o1`. Measured relief today: ~96 m.
- The **sculpt layer** (`sculpt.svsculpt`, `worldmap.h` ~155-220,
  `worldgen.wgsl` ~2061-2120) already exists: sparse i16 offset, one sample per
  8 vox, 32x32-sample tiles, bilinear, applied in `landAt` after the octaves.
  It is the locale-scale height tool. KEEP IT.
- Site index plane = one u32 per 1024-vox cell, per-cell site lists capped at
  `kSiteCellMax` 32. `kSitePath` already does integer segment-distance tests
  (`routeDist2`), pieces ≤ 256 vox; `sitePadAt` is the cut/fill template.
- `mapLandformGx/Gz` are piecewise-constant per cell; the octave ladder's
  derivative attenuation turns a gradient JUMP into a height STEP. Any new
  height interpolation must have a continuous gradient.
- The heightfield math has i32 overflow edges near curve span 32k and Q8 x
  16-bit products; the "amp <= 2048 after the LoadTuning clamp" comment in
  `octave()` is stale (nothing clamps since P-G).
- Tall terrain: the boot window is pinned at Y=0 (`SpawnWindowOrigin`,
  `main.cpp` ~1769, also every scripted scene and `perfsuite`); fog density is
  tied to the 6.5 km cascade edge and is homogeneous; `render.farShadowReach`
  24 m; openness up-ray stops at the window top; clouds treat the ground as the
  sea-level sphere; `treeline` (228) is one absolute Y used by snow, trees,
  cover rows (biome JSONs hard-code `minY 228 / maxY 227`), and the heat
  snowline; `sedCeil` 264 and `homeArea.y` 200 absolute; cave band 2 and
  `LAVA_LEVEL` absolute; `cactusMaxTop()` is world-wide and would defeat the
  desert sky skip; `skyTopReduce` keeps one ceiling per level; the `far` sweep
  calls `genCellIn` for buried rows. Height ENCODINGS (farMap u16 biased, wind
  i16, sculpt i16, chunk keys 21 bit) are all fine for ±3.2 km.
- The map page holds full u8 planes, rebuilds one whole-plane image per brush
  dab, snapshots whole planes for undo, POSTs the whole file on save, and its
  heightmap colours saturate at 90 m (`map.js` ~1646). It has no vector tools
  and no PNG import/export.
- Harrowby (default map) = refs (`refs/harrowby.json`), structure stamps with
  ABSOLUTE `pos.y`, soften site, edit layer `default_ground.svedit` (v2,
  ground-relative), paths painted by `paint_ground.mjs` from polylines that live
  only in a shell script.
- zstd is already a dependency (`chunkstore.cpp`), but this plan does not need
  compression: git zlib-compresses blobs, and tiling keeps a stroke's commit
  small.

---

## Rules for every package

- Work ONLY in your worktree. First command:
  `git reset --hard <BASE>` where BASE is the sha the orchestrator gives you
  (a commit on the integration branch `map-v2`), then read `CLAUDE.md` and this
  file. The main checkout is `C:/Users/Luke/Desktop/programming/3d sand voxel`;
  this plan is at `docs/PLAN_map_v2.md` in your tree after the reset.
- Commit on your worktree branch (small commits fine). Do NOT touch `main`, do
  not merge, do not push. The orchestrator merges.
- Never `git stash`. Never edit `AGENTS_BOARD.md` directly.
- `export SANDVOX_NO_CRASH_DIALOG=1` in every shell. Wrap every exe run in
  `bash scripts/run.sh` (absolute path to main's copy if yours lacks it).
- **Build:** C++ packages build ONCE with `bash scripts/build.sh` (sccache makes
  it incremental), then again only after further C++ edits. WGSL/JSON/JS-only
  packages NEVER build: run an exe that already has the C++ they depend on with
  `SANDVOX_ASSET_DIR=<your worktree>/assets`. If no such exe exists (the
  package depends on C++ not yet on main), say so in your report and build once.
  If `crash.log` names another worktree's source paths, it is a poisoned
  object (CLAUDE.md) — `python scripts/find_poisoned_objs.py --delete`, rebuild.
- **Testing budget (owner directive):** no `--suite acceptance`, no bare
  `--selftest`, no `--vk-smoke*` (unless the package is a render path and the
  question is "where did the image move"). Finish with ONE `--verify` boot
  naming the gates (+ `--shot-frames` / `--budget-arms` if render) that cover
  your change. Static checkers (`check_shaders.sh`, `check_invariants.py`,
  `check_pass_table.py`, `node scripts/test_environment.mjs`) are free — run
  them per edit.
- **`worldgen.wgsl` edits are expensive to run** (pipeline compile; the `far`
  entry has a superlinear driver compile cliff). Reason first, batch edits,
  run once. Every new inlined call in `landAt` lands in `far` too: keep new
  height code in small non-duplicated functions and record
  `SANDVOX_SHADER_TIMING=1` compile time of `far` before/after in your report
  if you touched the height path.
- **Every package here moves the world hash (HM).** Do not investigate expected
  moves. If a gate fails ONLY on pinned numbers your package intentionally moved
  (relief stats, pinned hashes), rebaseline that once in your final verify
  (`--verify <gates> --rebaseline` or `--selftest --gate <g> --rebaseline`). A
  gate failing on BEHAVIOUR is a bug in your change: fix it or report it.
  Gates already red at BASE are out of scope — check `tests/baseline.json`.
- **Mirror discipline:** anything in the height path changes BOTH
  `worldgen.wgsl` and `src/sim/world.cpp` inside the token-compared MIRROR
  blocks; `python scripts/check_invariants.py` must be green. Integer-only.
  Write the overflow bound of every new product in a comment next to it.
- Update `DESIGN.md` in the same commit when you contradict it; fix comments you
  make stale; delete, don't comment out. Do NOT edit `CLAUDE.md` or
  `assets/tuner.html` `ARCH_NODES` — the orchestrator does those at the end;
  put the text you want there in your report.
- Final report (your last message): commits, files touched, gate lines from the
  final verify, decisions you made that the plan left open, what you did NOT do
  and why, follow-ups. Under 500 words.

---

## The v2 map format (the contract F1 implements and E1/E2/E3 code against)

### `map.json` (version 2)

```jsonc
{
  "version": 2,
  "name": "default",
  // The authored extent, world voxels, inclusive min, exclusive max.
  // Must be a multiple of 1 << siteCellLog2 on both axes.
  "extent": { "min": [-100352, -100352], "size": [200704, 200704] },
  "siteCellLog2": 10,          // the site-index cell (102.4 m) — unchanged role
  "planes": {
    // posts at extent.min + i * (1 << log2), i = 0 .. size >> log2 (inclusive:
    // the last post is ON the max edge, so a plane has (size>>log2)+1 posts)
    "height": { "log2": 6, "tile": 256, "default": -600 },   // i16 voxels, absolute Y
    "biome":  { "log2": 8, "tile": 256, "default": "ocean" } // u8 palette index, cell-centred
  },
  "seaLevelY": 112,
  "edgeFadeVox": 6144,         // was oceanFadeCells 6 * 1024: height eases to planes.height.default outside the extent
  "warpAmpVox": 48,            // biome-boundary warp; <= (1 << planes.biome.log2) / 4
  "biomes": [ ... ],           // palette, unchanged
  "terrain": { ... },          // see "height function" below for what stays
  "sites": [ ... ],            // unchanged kinds (+ road / locale-related additions by R1 / L1)
  "rules": [ ... ],            // perKm2 now rolled per SITE cell; spacing defaults in voxels
  "roads": [ ... ],            // R1
  "regions": [ ... ],          // E3 (data only)
  "locales": [ ... ],          // L1
  "editLayer": "default_ground"
}
```

Removed in v2: `cellLog2`, `size`, `originCell`, `oceanFadeCells`,
`terrain.landformRangeVox`, landform SITES (`kind: landform`, peak / ridge /
basin / plateau) — the converter bakes them into the height plane and E2 offers
them as editor stamps that write the plane. The moisture plane is gone.

### Plane tiles: `assets/worldmap/<name>/planes/<plane>/<tx>_<tz>.svt`

One file per 256x256-sample tile (tile index = post index >> 8). Absent tile =
every sample is the plane's `default`. A tile is:

```
u32 magic 'SVT1'  u32 planeKind (1 height, 2 biome)  i32 tx  i32 tz
u32 w  u32 h (both 256; edge tiles still 256, samples past the plane are ignored)
u32 sampleBytes (2 height, 1 biome)  u32 reserved 0
w*h samples, z-major, little-endian
```

Why tiles: a brush stroke rewrites a few 128 KB tiles, not a 20 MB file — for
git, for the editor's save POST, and for undo. Ocean tiles are simply absent.
No compression library: git compresses blobs.

### The height function (what F1 makes `landAt` compute)

```
authored(x,z) = interpolated height plane      (exact at posts, C1, integer)
noise(x,z)    = the range rung o1 (seeded), shaped by the biome curve, then
                hill / detail / grain with per-biome relief multipliers and
                derivative attenuation fed by the CONTINUOUS gradient of
                authored + coarser rungs
bed           = authored + noise (+ home/pad calm fade on the NOISE only) + sculpt
```

- The AUTHORED height is never scaled by a curve, a fade or attenuation. With
  every noise amplitude 0, `TerrainHeight` at a post equals the post value
  (before ponds / sea / pads) — this is a gate assertion.
- Interpolation: interpolating and C1 (Catmull-Rom over the 4x4 posts, or an
  equivalent integer scheme F1 justifies). The gradient used for attenuation
  and the sediment slope gate must be continuous across post edges.
- `biomeCurve` keeps its knots but its domain becomes the range-noise rung only
  (±`rangeAmplitude`): a biome still decides "flat plains noise" vs "jagged
  noise" without overriding the author.
- Outside the extent: authored eases from the edge value to
  `planes.height.default` over `edgeFadeVox` (smooth, not a staircase).

### GPU packing

The worldMap buffer (binding 31) keeps its header + records layout; the header
gains per-plane `{log2, postsX, postsZ, wordOffset}` and the extent, and drops
the v1 words. Planes are packed DENSE (height: two i16 per word, 3137² posts ≈
19.7 MB; biome: four u8 per word ≈ 0.6 MB). The site index plane is sized by
`siteCellLog2` over the extent (196² words, as today). If the total exceeds
64 MiB F1 must stop and report rather than add a sparse GPU directory.

---

## Waves

| Wave | Packages | Why this order |
|---|---|---|
| 1 | **F1** format + height engine, **E1** map page core (JS/Python) | E1 codes against the format spec above; F1 is the engine side |
| 2 | **A1** altitude correctness, **H1** horizon + fog, **R1** roads engine, **E2** height tools | all need F1's height plane and the `peaks` test map; A1/R1/H1 touch different functions of `worldgen.wgsl` (A1: landAt/genCellIn bands + heat; R1: site kind + landheight + genCellIn surface; H1: `far` + raymarch) |
| 3 | **S1** terrain-scale light + clouds, **P1** tall-terrain perf, **E3** vector layer editor, **L1** locale engine | S1/P1 need H1; E3 needs R1's road schema; L1 needs F1 |
| 4 | **E4** locale editor | needs L1 + E3 |
| end | orchestrator: merge, one rebaseline, main exe rebuild, docs, ARCH_NODES, real-game shots | |

---

## F1 — Map format v2 + authored height in the engine (HM, C++ + WGSL, the big one)

Implements the format section above, end to end, in the engine.

1. **Loader** (`src/sim/worldmap.*`): parse map.json v2 and the tiles; validate
   (extent multiple of the site cell; tile magic/kind/size; biome index <
   palette; warp ≤ biome cell/4; heights within ±32000; overflow clamps for
   every terrain amplitude — write them). Refuse v1 maps with a message naming
   the converter. Keep `contentHash` covering map.json + every tile file +
   sculpt + structures (sorted tile order).
2. **Pack + header** (`PackWorldMap`, `worldmap.h` `kH*`): per-plane header
   words, dense packing, extent words; site index on its own `siteCellLog2`.
   `check_invariants.py check_worldmap_layout` updated so `kH*` ↔ `WM_*` agree.
   Fix the stale `kHOriginX` comment. Update `UploadEnvironment` if anything
   assumes the old size (it is grow-only — fine).
3. **Shader + CPU twin** (`worldgen.wgsl`, `world.cpp`): new plane readers
   (`mapHeightAt`, its continuous gradient, `mapBiomeAt` on the biome plane's
   own lattice with the warp, `biomeMixAt` on the biome lattice), the new
   `landAt` composition from "The height function", the edge fade. Delete
   `landformOctave`, `mapLandformQ8/Gx/Gz`, the moisture const, the u8 landform
   paths. Every reader that used the plane-cell lattice for SITES (`wmSiteList`,
   `CellOf`, rules) moves to `siteCellLog2`. Mirror green.
4. **Units:** everything that was in cells is now voxels or explicitly
   "site cells": `edgeFadeVox`, rules (`perKm2` per site cell, `minSpacing`
   default = site cell in voxels), `indexReach`. Grep for `cellLog2` and
   `1 << out.cellLog2` and settle every use.
5. **Converter** `scripts/convert_map_v2.py <name>` (Python; reuse
   `scripts/map_terrain.py`'s mirror where it helps): reads a v1 map, writes v2
   map.json + tiles. Height posts = `baseHeight + curve(landform dev)` sampled
   with the v1 bilinear at each post, with the v1 landform sites baked in
   UNclamped (no u8 saturation); biome tiles = nearest v1 cell (bake the v1 warp
   at the converter's seed only if it is cheap — otherwise nearest is fine,
   the author repaints). It need not be exact; the owner is redesigning the
   world. Convert `default` and `harness`, commit the results, delete the v1
   files. `seed_worldmap.py`: delete it (it writes v1 and refuses to overwrite
   anyway) unless something still needs it.
6. **Harrowby must survive the conversion:** the village's structure refs carry
   absolute `pos.y`. After converting `default`, compare bare ground (v1 vs v2)
   at each Harrowby structure and shift the refs' Y by the delta (a small
   script, committed under `assets/worldmap/default/scripts/`), so
   `village-harrowby` stays green. L1 later replaces this with locale anchors.
7. **Test map `peaks`** (`assets/worldmap/peaks/`): a generated (scripted,
   committed script) 8 km x 8 km map: a 1,400 m massif with a knife ridge, a
   300 m cliff band, a 600 m-deep gorge, a high valley at ~900 m with a lake
   site, foothills, a coast. Biomes: alpine/pine/meadow/tundra. Spawn on the
   high valley. This is the fixture every later wave uses (`SANDVOX_MAP=peaks`).
8. **Gates:** extend `worldmap` (or add `worldmap-v2`): tile absent = default;
   post exactness with zero noise; CPU == GPU column height over a post
   boundary and over a steep slope (C1-style); gradient continuity (no height
   step > 1 voxel across a post edge on a smooth ramp with noise on); v1 map
   refused with the converter message. Update `terrain`, `env-truth`,
   `voxregion`, `spawn-site`, `worldmap`, `sculpt`, `perfsuite`'s meadow search,
   `envtruth`/`voxregion` synthetic maps (they fill u8 planes today), and
   `scripts/test_environment.mjs` (svmap size checks → tile checks).
9. **Tools that read maps:** `--heightmap` (`main.cpp` ~5064; also fix the
   offset-40 `homeY`/`sea` mismatch with `map.js`), `--mapcheck`,
   `--voxserve`, `BuildBiomeSwatch`.
10. **Signatures:** `_world_signature` / `mapHash` in `tuner_server.py`, the
    environment stamp, the save fingerprint (`worldio.cpp`) and
    `biomes.cpp HashFileSet` include the tile files. The tile HTTP routes are
    E1's (both packages run in wave 1; keep your `tuner_server.py` edits to the
    signature functions so the merge stays clean).

Do NOT: touch the map page beyond what keeps it loading (E1 owns `map.js`);
change treeline/sediment/caves/heat (A1); add roads (R1).

Verify: `check_invariants.py`, `check_shaders.sh`, `test_environment.mjs`, one
`--verify worldmap,terrain,sculpt,env-truth,voxregion,spawn-site,village-harrowby,clearing,waterbody,pond-shore,far-surface,far-downsample,determinism`
(+ rebaseline pinned numbers you intentionally moved). Report `far` compile
time before/after and the worldMap buffer size for `default` and `peaks`.

## E1 — World map page core on v2 (JS + Python; no C++)

Owns `assets/editor/map.js` (and helpers it splits out, e.g.
`assets/editor/maptiles.js`) and the tile routes. Codes against the format
spec; F1 is the engine side, running concurrently.

0. **Server routes** (`scripts/tuner_server.py`): GET
   `/api/worldmap/tiles?name&plane` → listing (tx, tz, size, mtime); GET/POST
   `/api/worldmap/tile?name&plane&tx&tz` (validate the SVT1 header against the
   plane kind and tx/tz, atomic write-then-rename, empty body = delete = back
   to default); remove the v1 `/api/worldmap/planes` routes. POST `/api/worldmap`
   validates a v2 map.json (version, extent, planes). F1 edits only the
   signature functions in this file.

1. **Tiled planes in memory:** `Map<tileKey, TypedArray>` per plane, lazy-load
   visible tiles, default for absent. Dirty-tile tracking. Save = POST only
   dirty tiles + map.json. Undo per stroke stores only touched tiles
   (before/after), like the sculpt layer does.
2. **Tiled rendering:** one cached canvas per (plane, tile, zoom bucket),
   rebuilt only for dirty tiles; draw only visible tiles; mip levels for zoomed
   out (a 3137-post plane fits on screen at 1 px per 4+ posts).
3. **Topographic display:** hillshade from the height plane itself (no engine
   call), contour lines at a chosen interval (10 / 25 / 50 / 100 m) with every
   5th bold, a hypsometric colour ramp using real min/max (the 90 m saturation
   bug dies here), sea shown from `seaLevelY`. Biome overlay with opacity.
   Hover readout: world x/z in metres, height in metres, biome.
4. **Brushes in metres** (radius 1 m .. 5 km, log slider), pressure-free
   strength, spacing along the drag; biome paint brush; the existing site tools
   keep working (they are world-voxel already).
5. **Real-terrain backdrop** when zoomed in past the post resolution: the
   engine `--heightmap` of the view (it includes noise + sculpt + sites), as
   today's heights view, behind the authored plane at adjustable opacity.
6. Keep the sculpt tool (it is the fine layer) and the edit-layer selector.
7. Tests: extend `assets/environment_test.html` map section: load a v2 map,
   paint height + biome strokes, undo/redo, dirty-tile save payload is only the
   touched tiles, contour render runs, hover readout. `check_environment.sh
   ?only=map` must pass (it needs an exe that loads v2 maps — if F1 is not on an
   exe yet, run the client-only parts and say so).

Verify: `bash scripts/check_environment.sh` (map section), `node
scripts/test_environment.mjs`.

## A1 — Altitude correctness: nothing absolute-Y that should be relative (HM, C++ + WGSL + content)

Needs F1 and `peaks`.

1. **Spawn/window Y:** `SpawnWindowOrigin` and every scripted scene /
   `perfsuite` origin that hardcodes Y=0 derive Y from the ground at the spawn
   column (`(TerrainHeight >> 4) - half` in chunks, clamped sensibly). One
   helper, every caller uses it. Harness-map scenes must keep their current
   window (their ground is ~y 200 → the same window) — check.
2. **Treeline per biome + jitter:** `terrain.treeline` becomes the map default;
   each biome may override (`terrain.treeline` in the biome JSON), blended with
   `biomeMixAt`, plus a seeded ±jitter so it is not a ruled line. Snow cap,
   `colGrowsTrees`, shore/cover gates and `HeatSnowlineY` read the column's
   treeline. Re-express the biome JSON cover/tree/water rows' `minY/maxY` that
   encode "below/above treeline" as treeline-relative (`belowTreeline` /
   `aboveTreeline` or `minDy/maxDy`), loader + biome editor rows
   (`assets/editor/biome.js`, `biomegen.js`) + `tests/env_predictions.json`.
3. **Climate lapse rate:** ambient temperature falls with altitude above sea
   (`HeatAmbientCpu` + the GPU ambient + `kHpSnowlineY` path): one tuning row
   (`heat.lapsePerKm` or similar, per CLAUDE.md's knob recipe), the snowline
   derived from it where the biome climate crosses freezing. Integer sim math.
4. **Sediment and home area relative:** `sedCeil` becomes a height ABOVE a
   smoothed local floor (or the authored height), not absolute; `homeArea.y`
   is replaced by "flatten the NOISE around the spawn" (the authored height is
   the author's) — the calm fade applies to noise only per F1.
5. **Caves:** band 2 and `LAVA_LEVEL` become depth-relative (below the column's
   authored height), bounded so a valley floor never meets lava within N m.
6. **Steep rock:** columns above a slope threshold skin as the biome's rock
   (`firmSkin` / stone), no powder cap (rule 2: powder on a cliff never sleeps);
   check the existing slope gates and extend rather than duplicate.
7. Gate `altitude` (new): on `peaks` — boot window contains the spawn ground;
   treeline differs between two biomes and is not a straight line; a high-valley
   column gets sediment; ambient at 1,200 m is colder than at the coast by the
   lapse rate; no lava within the bound under the gorge; ≤32 active chunks at
   rest after settling on the cliff band (rule 2).

Verify: one `--verify altitude,terrain,biomes,heat-live,determinism,sleep`
(+ whichever cover/tree gates the row migration touches).

## H1 — Far horizon + height fog (HN for the sim; render only)

Needs F1 and `peaks`. Render-only: no hashed state.

1. **Horizon heightfield beyond the cascades:** a 2D height pyramid built from
   the authored height plane (CPU at load / reload, uploaded once; mips with
   MAX for silhouettes and AVG for shading), raymarched for rays that leave
   cascade level 8, out to the map extent + edge fade (≥ 30 km). Shaded with
   the biome colour plane + slope rock + snow above the column treeline (A1's
   function if landed; else the map default). This is the "mountains on the
   horizon" path; it must meet the cascades without a visible seam (blend
   band).
2. **Peak preservation in the cascades:** the `farMap` top keeps the
   sub-column MAX so ridges and summits are not shaved at levels 6-8 (the
   worldgen.wgsl corner-max note says spires between corners are lost).
3. **Fog:** height-exponential fog (density falls with altitude), decoupled from
   the cascade extent; distance haze tuned so a 1 km peak at 15 km reads as a
   blue silhouette. Tuning rows per CLAUDE.md. Remove the coupling
   `kFarFogDensity = 4.5 / kFarHalfExtentMeters(kFarLevels)`.
4. `--shot` scenes on `peaks`: from the high valley looking at the massif; from
   the summit looking out; from the coast at 10 km. Add them as named
   `--shot-frames` entries.
5. Budget: `--budget-arms` on the summit view vs BASE; the horizon march must
   cost < 1 ms at 1440p on this machine (report the number).

Verify: one `--verify far-surface,far-downsample,determinism --shot-frames <your scenes> --budget-arms baseline`.

## R1 — Roads in the engine (HM, C++ + WGSL + materials)

Needs F1.

1. **Data:** `map.json roads[]`: `{id, name?, points: [[x,z],...] (world
   voxels), width (vox), shoulder (vox), surface: material name, maxGrade
   (percent), kind?: "road"|"track"|"path"}`. Loader validates.
2. **Profile baking** (CPU, at load, deterministic integer): densify the
   polyline, sample bare ground (`BareGroundHeight`, no road influence) along
   it, low-pass + grade-limit to `maxGrade` (cut through bumps, fill dips),
   bake per-vertex Y. Split into pieces ≤ 256 vox (the `routeDist2` overflow
   bound) as new site kind `kSiteRoad` records: ends A/B, Y_A/Y_B, half-width,
   shoulder, surface material, piece flags. Per-cell list cap 32 still holds —
   report the worst cell on `default` with a test network; if it does not,
   raise the cap with a measured reason.
3. **Height:** `roadPadAt` beside `sitePadAt` in the `landheight` mirror:
   weight 256 within half-width, smoothstep across the shoulder toward the bare
   ground, the road Y lerped by the projection parameter; zero the sediment
   wedge where it moved h; multiple roads blend in table order like pads.
4. **Surface:** `genCellIn` paints the top `depth` cells within half-width with
   the surface material (a SOLID — add `packed_earth` and `road_cobble` /
   reuse cobble/flagstone; powders on graded slopes violate rule 2). Ragged
   edge via a counter hash. Footstep sounds via the material's `footstep`.
5. **Keep-outs:** `siteFootprintHas` true within the half-width (no cover, tile
   plants, tarns, cacti); trunks refused within half-width + crown reach
   (crowns do not overhang the road), like stamps.
6. **Far field:** the surface reaches the cascades through `genCellCol`; add a
   coverage test in `farSurfaceMat` so a road narrower than the cascade cell
   still tints the cell (distance < cell half-size → blend). One more inlined
   site walk at most (compile cliff).
7. **Harrowby paths:** replace the `paint_ground.mjs` paths in
   `harrowby_08_paths.sh` with `roads[]` entries (`kind: path`, gravel/dirt
   surface) and erase their cells from `default_ground.svedit` (`--erase` /
   the script) so they do not double up. Keep `kSitePath` (waynode trunk
   keep-out) — it is a different thing.
8. Gate `roads` (new, on a synthetic map): graded profile never exceeds
   maxGrade; road surface material at the top cell along the centreline; no
   trunk/cover within half-width; CPU `TerrainHeight` == GPU over a road;
   far cascade level 4 cell over a 1.2 m path is tinted; settles to ≤32 active
   chunks.

Verify: one `--verify roads,worldmap,village-harrowby,terrain,far-surface,determinism,sleep`.

## E2 — Height authoring tools (JS + Python)

Needs E1 (and F1's tile routes).

1. Brushes on the height plane: raise, lower, smooth, flatten (to sampled
   height), set height (typed metres), terrace / cliff (quantise to a step
   with a sharpness), noise (seeded, amplitude in m), erode (see 4). All with
   radius/strength/falloff, per-stroke tile undo.
2. **Line tools (vector, baked):** ridge line and valley line — draw a polyline,
   set crest/floor height (per vertex or along a profile), width and
   cross-section (V, U, rounded, sharp); preview; "bake" writes the plane (the
   line stays in `map.json` under `ridges[]` for re-baking/editing, with a
   "rebake" that re-applies from the pre-bake snapshot of those tiles stored
   in the undo record — keep it simple and honest; document limits).
3. **Shape stamps:** peak / ridge / basin / plateau / volcano / mesa with
   height + radius + rotation, writing the plane (the v1 landform site kinds,
   now an editor op).
4. **Erosion:** a hydraulic + thermal erosion pass on a selected rectangle,
   in a Web Worker, deterministic (seeded), with iterations/strength,
   preview-then-commit. Integer or float is fine here (authoring tool, output
   is quantised to i16 posts before it reaches the engine).
5. **16-bit PNG import/export** of the height plane (whole map or a selection):
   `scripts/map_png.py` (Python stdlib zlib + a minimal PNG reader/writer,
   16-bit grey) + server routes, so Gaea / World Machine heightmaps import,
   with a dialog for metres-per-unit and offset. Also biome import from an
   indexed/RGB PNG mapped to the palette colours (nearest; report unmapped).
6. Tests in `environment_test.html`: each brush changes only expected tiles;
   ridge bake + undo round-trips byte-exact; erosion is deterministic for a
   seed; PNG export→import round-trips exact. `python scripts/map_png.py
   --selftest` for the codec.

Verify: `check_environment.sh ?only=map`, `test_environment.mjs`,
`python scripts/map_png.py --selftest`.

## S1 — Terrain-scale lighting + clouds against mountains (render only)

Needs H1.

1. **Long sun shadows:** a sun-direction horizon map built from the H1 height
   pyramid (GPU compute, re-built when the sun moves past a threshold),
   sampled in shading for terrain beyond `render.farShadowReach` (and as an
   upper bound inside it), so a 1 km ridge shades its valley.
2. **Sky visibility:** a horizon-angle sky-occlusion term from the same pyramid
   for far shading and as a floor for the window openness in gorges.
3. **Clouds vs terrain:** clouds march against scene depth for terrain pixels
   whose hit is above the deck base (peaks wrapped in cloud from a valley);
   `cloudSunAtM` returns unshadowed above the deck top and fades inside;
   `deckSpan` stops assuming sea-level ground where the eye is high.
4. **Precipitation by altitude:** rain overlay gated on the eye vs the deck
   base; `precipType` blends to snow above the lapse-rate freezing level
   (A1's function; if A1 is not merged, read the map default treeline and
   report). Render only; the SIM rain word stays as is unless A1 already
   covers it.
5. `--shot` scenes from H1 re-shot + one storm scene on `peaks`; budget arm.

Verify: one `--verify clouds,determinism --shot-frames <scenes> --budget-arms baseline`.

## P1 — Tall-terrain perf (HN except where noted)

Needs F1, H1 merged.

1. `cactusMaxTop()` → per-column bound (`h + CACTUS_MAX`) so a high treeline
   does not defeat the desert sky skip (`cols` and `far`).
2. `skyTopReduce`: a coarse 2D tile grid of sky tops per level instead of one
   row, so one peak does not raise every ray's ceiling.
3. `far` sweep: skip fully buried rows (`yRow + step < h - caveFloor` →
   stone/blocker without `genCellIn`).
4. Vertical streaming: measure `--autofly-hard`-style descent/ascent on `peaks`
   (add a `peaks` route to the autofly harness: summit→gorge→valley), report
   page residency peak, worldgen ms, far list saturation (`kFarListCap`).
   Fix what it shows within reason; report the rest.
5. Before/after numbers under `SANDVOX_RUN_EXCLUSIVE=1`, one run each side.

Verify: one `--verify determinism,far-surface,far-downsample,terrain` (hash
must be UNCHANGED except where item 3's blocker shortcut is provably identical
— prove it; a moved hash in this package is a bug).

## E3 — Vector layer editor: roads, regions, markers (JS)

Needs E1 + R1's road schema.

1. A vector layer panel on the map page: roads (polyline draw/edit: add,
   move, insert, delete vertices; width/shoulder/surface/kind/maxGrade
   inspector; snapping to other roads' vertices for junctions), regions
   (named polygons with a colour; `regions[]` in map.json: `{id, name,
   colour, points}` — data only, the engine loads and validates them and
   exposes `RegionAt(x,z)` by name for later use; the loader part is small C++
   and is IN this package), markers/labels (`markers[]`: `{id, name, icon,
   at}` — map annotation only).
2. Road profile preview: a side panel graph of the baked profile vs the ground
   (use `--heightmap` along the polyline or a new cheap route; the engine's
   bake is the truth — show the engine's, not a JS re-implementation).
3. Pixel-art icons for markers (hut, village, castle, tower, cave, ruin,
   shrine, camp, mine, port) — UI stays pixel art.
4. Tests in `environment_test.html`: draw a road, edit vertices, undo; region
   round-trip; the saved map.json validates in `--mapcheck`.

Verify: `check_environment.sh ?only=map`, one `--verify worldmap` if you
touched the loader.

## L1 — Locales in the engine (HM, C++)

Needs F1.

1. **Data:** `map.json locales[]`: `{id, name, icon, bounds: {min:[x,z],
   max:[x,z]}, anchor: {at:[x,z], y}, editLayer?: "<file>"}`. Folder
   `assets/worldmap/<map>/locales/<id>/`: `ground.svedit` (the locale's own
   edit layer, v2 ground-relative), optional notes.
2. **Multiple edit layers:** the map's `editLayer` plus every locale's layer
   load and apply (as worldgen, as today: stream drain, FarEdits base,
   voxserve). Overlap between layers: later locale in table order wins, map
   layer first. Save fingerprint / contentHash include them.
3. **Rigid anchor:** refs in a locale's ref group (`refs/<id>.json`, the group
   name == locale id) are stored with Y relative to `anchor.y`; at load the
   locale's Δ = `BareGroundHeight(anchor.at, seed) - anchor.y` and every ref Y
   (structures → stamp `padY`, waynodes, NPC homes, markers) is offset by Δ.
   Reshaping the world under a locale moves the whole locale with it.
   Edit-layer cells are already per-column relative. Sculpt tiles are offsets —
   unaffected.
4. **Migrate Harrowby into a locale** (`harrowby`): bounds around the village,
   anchor at the green, refs converted to relative Y (a script), its ground
   cells moved from `default_ground.svedit` into
   `locales/harrowby/ground.svedit`. F1's item-6 shim is deleted.
5. **Refs authoring route for the tuner** (needed by E4):
   `tuner_server.py` routes that run the canonical F8 command layer headless
   (`--edit-script` with `ref.place/move/delete/set_prop`) so the page and
   scripts share one authoring surface; no hand-rolled JSON writer.
6. Gate `locale` (new): synthetic map + a locale with one structure and an
   edit-layer patch; raise the height plane under it by 200 vox → the
   structure, its pad, the patch and a waynode all move by exactly Δ; two
   overlapping layers resolve deterministically; `village-harrowby` still
   green.

Verify: one `--verify locale,village-harrowby,worldedit,voxregion,save-load,determinism`.

## E4 — Locale editor (JS + Python)

Needs L1 + E3.

1. On the world map, locale markers (pixel-art icon + name); a "new locale"
   tool drags its bounds and picks an icon; double-click opens it.
2. **Locale page** (Environment tab, or a mode of the map page): a top-down
   view of the bounds at full sculpt resolution (0.8 m) with the real-terrain
   heightmap backdrop, sculpt brushes (existing sculpt layer, clipped to the
   bounds), clearing/soften/tree/water site tools clipped to the bounds; a 3D
   `WorldView` of the bounds (reuse `makeView`) whose voxel edits save to the
   LOCALE's edit layer; structure placement from the Structures library
   (writes refs through L1's route), move/rotate/delete; waynode links.
3. Breadcrumb back to the world map; the world map shows each locale's bounds
   at low opacity.
4. Tests in `environment_test.html` (or a new `locale_test.html` run by
   `check_environment.sh`): create a locale, sculpt inside it (and NOT outside
   the bounds), place a structure, voxel edit saved to the locale layer.

Verify: `check_environment.sh`, `check_worldview.sh`, one `--verify locale`.

---

## Orchestrator endgame

1. Merge order per waves onto `map-v2`; after each merge run only the merged
   package's verify gates if the merge had conflicts in code (otherwise its
   report stands).
2. One `--selftest --gate determinism --rebaseline` + rebaseline of pinned
   gates moved by the content changes; one `--verify` of the union of the new
   gates (`worldmap,altitude,roads,locale,terrain,village-harrowby,determinism`).
3. Land `map-v2` on `main` (dirty-main procedure), rebuild main's exe (check
   no live `sandvox.exe` first), capture real-game shots on `default` and
   `peaks`.
4. Update `CLAUDE.md` (layout table rows for `assets/worldmap/`, the map format,
   the new gates), `DESIGN.md` world-map section, `assets/tuner.html`
   `ARCH_NODES` + Development Status, `docs/EDITOR_GUIDE.md` (new sections:
   topographic editing, roads, locales).

## Out of scope (say so if asked)

Rivers (a separate water-system project; draw them as valley lines for now),
procedural towns/cities at scale (locales are hand-made; a town generator is a
later plan), overhangs/arches (the ground is a heightfield; caves remain the
only overhangs), map extents larger than the 4096-post-per-axis plane at
64-vox posts (≈ 26 km) without a sparse GPU directory.
