# PLAN: the worldbuilder (World map page, round 3)

Owner request, 2026-10-09: make the tuner's World map page a fully equipped
worldbuilding tool, the kind of thing Gaea / World Machine / World Creator /
Azgaar / Unreal Landmass offer, so the world can be designed in the tuner and
the game reflects it. Then design a fantasy-RPG world on `default/` with many
(new, invented) biomes. Owner decisions: build over `default/` (keep Harrowby
and the spawn, may MOVE both); engine changes are OK for real rivers; new
materials / cover / tree species are OK.

The complaint that started it: "if I make a mountain by spamming raise terrain
it looks bad; we need effective topological mountain ranges and generators,
realistic water, rivers that connect as they should."

Read first: `docs/PLAN_map_v2.md` (the format this builds on),
`docs/EDITOR_GUIDE.md` section 15 (the page as the owner uses it), CLAUDE.md.

## 0. What exists (surveyed 2026-10-09 on d893663)

- `assets/editor/map.js` (3,236 lines) is the page. Tools are wired BY HAND:
  global `tool` string (map.js:108), a button in `els.tools` (:2868), an
  options span keyed by tool name (:3031), `if (tool === ...)` branches in
  `wire()` (:1212), `paintCursor` (:1120), `syncControls` (:2702).
  Sub-layers follow a context-object pattern: `VEC.createVectorLayer(C)` /
  `LOC.createLocaleLayer(C)` get `{map, view, SX, SZ, worldAtF, pushUndo,
  snapshot, markDirty, paint, setTool, ...}` and return hooks
  `pointerDown / toolOpts / buildPanel / sync / afterRestore / problems / key /
  dblclick`.
- Height brush modes raise/lower/smooth/flatten/set/terrace/noise/erode
  (`maptiles.js` `heightDab`); Shape tool (peak, ridge, basin, volcano, plateau,
  mesa — `heightshapes.js`); Line tool (ridge/valley polylines, editor-only
  `ridges[]`); Area tool (rectangle droplet+thermal erosion in a worker,
  `erosion.js`); biome paint; 0.8 m sculpt layer; sites (pad, spawn, stamp,
  water, tree, clearing, soften); roads / regions / markers (`mapvector.js`);
  locales (`maplocale.js`). Undo: tile snapshots + json snapshots, 60 deep.
- Rendering: client-side hypsometric LUT + NW hillshade + contours + biome
  overlay alpha.
- Engine (`worldgen.wgsl` + token-mirrored `world.cpp`, loader `worldmap.cpp`):
  height plane = Catmull-Rom over 6.4 m posts; biome plane 25.6 m cells,
  u8 ids, palette `biomes[]`; global sea plane at `seaLevelY`; lakes = `water`
  sites, a DISC bowl whose surface is the ground at its centre; roads = STRAIGHT
  polylines baked into graded `kSiteRoad` chords (`worldmap.cpp:2145-2300`).
- **No rivers anywhere.** `docs/PLAN_water_master.md` lists flowing rivers as a
  non-goal: a flowing CA river never sleeps (rule 2).
- Gates run on the `harness` map. Only `spawn-site`, `village-harrowby`
  (+ `village-twice`, `harrowby-cellar`) and `roads` H read `default`.
- 8 biomes: forest, meadow, pine, desert, tundra, swamp, alpine, ocean.
  `climate.temperature` / `climate.moisture` exist in the biome JSON but are
  not read by anything.

## 1. Feature catalogue (from the survey of Gaea, World Machine, World Creator, Houdini, UE Landmass/Water, Unity Terrain Tools, WorldPainter, Azgaar, Wonderdraft, Inkarnate, CC3+)

Must-have, all of it in this plan:
1. Mountain RANGE from a spline: per-knot crest height + width, cross-section
   profile, peaks/saddles along the crest (1D ridged noise in arc length +
   explicit peak knots), spurs + foothills (domain-warped ridged noise under a
   wide mask), and the correct version: the range is an UPLIFT field and a
   stream-power erosion pass grows dendritic valleys and spurs (Cordonnier 2016,
   Schott 2023).
2. Brushes with masks: "land only" / "sea only" / height band / slope band
   locks for every brush; stamp a heightmap image (rot/scale/blend
   add|max|min|replace); clone.
3. Splines with handles: centripetal Catmull-Rom (and bezier handles where a
   vertex wants a sharp control), insert/delete knots, per-knot attributes.
   Roads that BEND between points; rivers as splines.
4. Water: Priority-Flood lakes (spill level), D8 flow accumulation -> river
   network extraction, rivers that join, end in a lake or the sea, never run
   uphill; deltas/meanders; waterfalls where a reach drops.
5. Climate -> biomes: temperature (lapse rate, latitude/authored gradient,
   aspect), moisture by prevailing-wind march (orographic rain + rain shadow),
   wetness from flow accumulation, a Whittaker-style lookup into the biome
   files' `climate` fields; manual biome paint stays and WINS.
6. Overlays: hillshade (exists), contours (exist), SLOPE map in the game's
   walkable/steep/cliff bands, flow/wetness, temperature, moisture; measure tool
   (distance + elevation profile + grade %), grid, minimap.
7. Generators: continent / island / archipelago templates (Azgaar-style op
   lists: hill, pit, range, trough, strait, mask), volcano (exists), crater,
   canyon, mesa/terrace fields, dunes, fjord (U-valley carve then flood),
   glacial valley, coastline wobble, beach band.
8. Erosion as a global pass and a brush: stream-power (coarse proxy, 25.6 m or
   51.2 m), droplet hydraulic (exists, area-limited), thermal (exists).

## 2. Formats (PINNED: the editor and engine packages both target these)

All coordinates are world voxels (10 vox = 1 m), x/z as in `roads[]`; heights
are absolute voxel Y like the height plane.

### 2.1 `rivers[]`

```json
{"id": "river_silverrun", "name": "The Silverrun",
 "points": [[x, z, w, d], ...],
 "smooth": true, "bed": "gravel", "joins": "river_other"|null,
 "stepVox": 4, "about": "..."}
```

- `points` are ordered SOURCE -> MOUTH. `w` = channel width at that knot (vox,
  8..1024), `d` = depth of water at the centreline (vox, 2..120). A knot may be
  `[x, z]` only; then `w`/`d` are interpolated from neighbours (the first/last
  must carry them).
- `smooth: true` = the centreline is a centripetal Catmull-Rom through the
  points (tessellated by the ENGINE at load, same function as roads, 2.3).
- `joins`: the river this one is a tributary of; its last point must lie on
  that river's channel. Its water surface at the junction is >= the parent's
  there (the loader warns otherwise and clamps).
- The WATER SURFACE is computed by the engine at load, not stored: sample the
  bare ground along the centreline, make it MONOTONE NON-INCREASING
  source->mouth (running min), clamp the mouth to `seaLevelY` (or the lake
  level it ends in), then QUANTIZE into flat REACHES: the surface stays flat
  until the bed has fallen `stepVox` (default 4) below it, then steps down. A
  step drop of more than ~2 m is a WATERFALL (recorded, the renderer may
  foam it). Each reach is flat, so the water is AT REST: it sleeps (rule 2).
  Flow LOOKS from shading / current primitives, never from CA mass transport.
- The channel: cross-section `depth * (1 - (u/hw)^2)` below the surface
  (parabolic, U for wide/slow, V-ish for narrow/steep: `profile` optional
  `"U"|"V"`), banks blend back to the terrain over `bank` (default 1.5 x
  width), and the bank lip is held at >= surface + 1 voxel so the water is
  contained (cut where the terrain is higher, fill a levee where it is lower).
- Rivers carve the ground UNDER sites and roads? No: a road crossing a river is
  a ford (road deck wins inside its width, river water above it). Bridges are
  stamps (later).

### 2.2 `lakes[]`

```json
{"id": "lake_mirrormere", "name": "Mirrormere", "level": 1840,
 "points": [[x, z], ...], "smooth": true, "bed": "mud",
 "shore": "sand", "about": "..."}
```

- Water fills every column INSIDE the polygon whose ground is below `level`, up
  to `level`. The editor makes the polygon from a Priority-Flood fill (the
  spill outline); hand-drawn polygons are allowed.
- Containment is the ENGINE's job too: within a shore band outside the polygon
  any ground below `level + 1` is raised to it (a natural sill), so a sloppy
  polygon never leaks.
- `water` sites (disc presets) stay as they are, for small ponds.

### 2.3 `roads[]` additions

- `smooth: true` = centripetal Catmull-Rom through `points` before the profile
  bake (default false = today's polyline, so existing roads do not move).
  New roads drawn in the editor default to `smooth: true`.
- Optional per-point third element is NOT added; shape is controlled by points.

### 2.4 Editor-only keys (the engine ignores; `envlive.js` manifest lists them)

- `ranges[]`: `{id, name, points: [[x, z, crestY, halfWidth], ...], profile,
  seed, peaks, spurs, foothills, erode, baked}` — the range generator's input,
  re-bakeable like `ridges[]`.
- `climate`: `{windDir, lapse, seaTemp, poleZ, ...}` the auto-biome inputs.
- `gen`: the generator op list used to build the base (for re-running).

## 3. Packages

Every package: worktree off the `worldbuilder` integration branch (first
command `git reset --hard <sha given in your prompt>`), commit on your branch,
report the sha + a summary + anything the next package must know. Do NOT
touch `assets/worldview.js`, `assets/editor/maplocale.js`,
`assets/worldview_test.html` (a live peer session holds them). Do NOT run full
acceptance; run your own gate(s) once at the end (CLAUDE.md "verification is a
budget"). WGSL/JSON-only packages never build (use main's exe + 
`SANDVOX_ASSET_DIR`). Node-test every pure JS module (`scripts/test_*.mjs`
pattern). Keep the pixel-art/dark UI style of the page.

### Wave 1 (parallel)

**K1 — terrain kernel + editor core** (JS only, no build)
Owns `assets/editor/map.js`, `maptiles.js`, `heightshapes.js`, `erosion*.js`,
`maplegend.js`, new modules.
1. Tool registry: refactor the hand wiring into `registerTool({key, label,
   group, cursor, opts(), down, move, up, panel?})` so later packages add a
   tool in ONE place. Behaviour of existing tools unchanged.
2. `assets/editor/spline.js` (pure): centripetal Catmull-Rom (alpha 0.5)
   tessellation, arc length, closest point (t, u, signed offset), per-knot
   attribute interpolation, insert/delete knot, bezier-handle override per
   knot. THE SAME tessellation the engine implements (W1 mirrors it; pin a
   shared test vector table in `tests/spline_vectors.json` that both the Node
   test and the C++ gate read).
3. `assets/editor/hydrology.js` (pure, typed arrays, worker-friendly):
   Priority-Flood (+epsilon variant, Barnes 2014), lake extraction (component,
   spill level, outline polygon simplified), D8 receivers + O(n) accumulation,
   MFD wetness, river extraction (threshold, trace, Douglas-Peucker, Chaikin,
   width from accumulation `w = k*sqrt(A)`), stream-power implicit solver
   (Braun-Willett, m=0.5 n=1) with uplift + hardness fields, thermal pass.
   Runs on a coarse proxy (25.6 m / 51.2 m) of the region, upsampled with
   detail added back.
4. `assets/editor/terraingen.js` (pure): ridged multifractal, domain warp,
   range-spline field (crest profile, peaks/saddles, spurs, foothills,
   `erode` = uplift + stream-power), primitives (crater, canyon, mesa field,
   dunes, fjord/glacial U-valley, island/continent masks), Azgaar-style
   template op lists (hill, pit, range, trough, strait, mask) with a seed.
5. Tools on the page: **Range** (draw a spline, per-knot crest height/width
   handles, sliders for peaks/spurs/foothills/erode, live preview, bake,
   re-bake from `ranges[]`), **Generate** panel (templates/primitives applied
   to the whole map or a dragged box, preview then commit, undoable),
   **Erode** upgraded (stream-power global/box pass + existing droplet/thermal),
   brush MASKS for every height brush (land only / sea only / height band /
   slope band), **Measure** (distance, elevation profile chart, grade %), and
   OVERLAYS: slope bands (walkable / steep / cliff, from the game's movement
   limits — find them in `src/game/player*`), flow accumulation, lake preview,
   grid in km, minimap with viewport box.
6. Undo for everything (tile + json snapshots, existing mechanism). Big ops
   (whole-map generate/erode) must stay responsive: worker + progress bar.
7. Node tests: `scripts/test_hydrology.mjs`, `scripts/test_terraingen.mjs`,
   `scripts/test_spline.mjs`; extend `test_heighttools.mjs` if touched.
8. Update `docs/EDITOR_GUIDE.md` §15 for every tool added, and the legend
   (`maplegend.js`).

**W1 — rivers, lakes, curved roads in the engine** (C++ + WGSL, builds once)
Owns `src/sim/worldmap.*`, `assets/shaders/worldgen.wgsl`, `src/sim/world.cpp`
mirror, horizon/far files as needed, new `src/test/selftest_rivers.cpp`.
1. Load + validate `rivers[]`, `lakes[]`, `roads[].smooth` exactly as §2.
   Spline tessellation = the C++ twin of `spline.js` (test vectors file).
2. Bake rivers into reaches at load (surface monotone + quantized), into a
   per-cell piece table the GPU reads (respect or extend `kSiteCellMax`; a 10 km
   river crosses ~100 site cells: budget pieces sensibly, length ≤ 256 vox
   chords like roads). Lakes into polygon records (edge list per cell).
3. worldgen: carve the channel + fill water to the reach surface AT REST; bank
   lip containment; bed material; lakes fill to level with sill containment;
   CPU mirror (MIRROR tokens, `check_invariants.py`). Waterfall steps: the
   lower reach's lip holds the upper water; the drop face is the bed material.
4. Far field: rivers and lakes must appear at distance (horizon / far planes —
   see how the sea and roads are drawn far: `src/sim/horizon.*`, the far
   fields, `roadPadAt`). A river that pops in at 200 m is a fail.
5. Gate `rivers` (synthetic temp map like `roads`): surface monotone along
   every river, every reach flat, no water cell adjacent to air below its level
   outside the channel (= contained), tributary surface >= parent at the join,
   lake fills only below level inside, chunks sleep (≤32 awake after settle —
   THE key assertion: rivers must not keep the sim awake), CPU == GPU, smooth
   road profile still respects maxGrade. `--mapcheck` reports river/lake
   errors to the tuner check panel.
6. `envlive.js` manifest + `tuner_server.py` `_map_v2_check` accept the new
   keys. Document in DESIGN.md (worldmap section) and EDITOR_GUIDE §15.
7. Visual "flow": if cheap, a current primitive or shading drift along the
   river direction (render-only, never hashed). Optional.

**B1 — new biomes + content** (JSON/JS assets; build only if a new material
needs C++ — it should not)
Owns `assets/biomes/`, `assets/materials/materials.json` (claim it on the
board), `assets/trees/`, `assets/editor/biomegen.js` (`ENGINE_BIOMES`),
`assets/editor/treegen.js` only if a species needs a new shape, 
`tests/env_predictions.json`, `assets/water/`.
1. Design 10-14 NEW fantasy-RPG biomes that read as distinct places at a
   glance AND play differently. Suggested (invent better ones): ashlands /
   volcanic badlands, red-rock mesa canyonlands, birch-and-heather highland
   moor, misty giant-mushroom fen, frost-glass glacier, savanna with flat-top
   trees, coastal dunes + salt marsh, autumn maple/oak "amberwood", dark
   old-growth "eldwood" (redwood/great oak), crystal barrens, steppe /
   grassland, jungle-ish lush valley, bamboo/reed lowland, white-cliff chalk
   downs, karst/limestone pillars. Each with: skin/subsoil/firmSkin, relief
   curve fitting its landform, cover rows, trees, caves, a treeline, and
   `climate.temperature` + `climate.moisture` filled HONESTLY for every biome
   (the auto-biome generator in wave 2 reads them as a Whittaker lookup: write
   down the scale you used in the biome schema docs).
2. New materials where a biome needs them (ash, red sandstone, chalk, basalt,
   moss variants, glowcap mushrooms, autumn leaves...): follow the material
   rules in materials.json (tags, colours, reactions). Material ids are the
   index: APPEND only.
3. New tree species where needed (maple/autumn, baobab or acacia flat-top,
   giant mushroom, dead charred, birch already exists...) authored as
   `assets/trees/<species>.json` and baked with `node scripts/bake_trees.mjs`.
4. Pass the env gates: `node scripts/test_environment.mjs`, `--selftest --gate
   biomes`, `--gate env-truth` once at the end (main exe +
   `SANDVOX_ASSET_DIR`).
5. Add the new biomes to the map.js `COLOR` table? NO (K1 owns map.js): list
   the colours in your report; the merge adds them.

### Wave 2 (after wave 1 merges)

**E2 — water, roads, climate tools on the page** (JS only)
1. **River** tool: draw a spline source->mouth (handles, per-knot width/depth),
   OR "derive rivers" from flow accumulation over a box/the whole map
   (threshold slider, preview the network, accept selected ones into
   `rivers[]` with `joins` set). Meander button (sum-of-sines offset,
   wavelength ~12x width, amplitude grows as slope falls). Live preview of the
   ENGINE's surface/reaches (port the bake to JS from W1's C++, shared test
   vectors) with waterfall markers. Snap: a river's mouth snaps onto another
   river (sets `joins`), a lake, or the coast.
2. **Lake** tool: click a basin -> Priority-Flood spill level + outline ->
   `lakes[]` (raise/lower the level with a slider, the outline follows); hand
   polygon mode.
3. **Roads bend**: the road tool gets spline editing (drag the curve, handle
   per knot, insert by clicking the curve), new roads `smooth: true`, the
   profile chart shows the smoothed path. Rivers crossing -> ford marker.
4. **Climate / auto-biome**: inputs (wind direction, sea temperature,
   latitude gradient, lapse), overlays (temperature, rainfall, wetness), and
   "Generate biomes" over a box/the map with a lock for hand-painted cells
   (store the hand-painted mask in the editor so regenerate keeps them).
   Whittaker lookup over all biomes' `climate` fields; slope -> rock and
   height -> snow are the engine's job already, do not duplicate.
5. Coast tools: coastline wobble brush, beach band (flatten to sea level ±),
   cliffs.
6. Tests + EDITOR_GUIDE + legend.

### Wave 3 — the world (orchestrator + D1 agent)

`scripts/design_world.mjs`: a reproducible script that builds `default/`
from the editor's own modules (terraingen, hydrology, spline, climate): the
continent, its ranges, erosion, lakes, rivers, roads between towns, biomes,
regions + markers, Harrowby/spawn placed in a fitting spot. Run once, then the
owner hand-edits in the tuner. Design brief written by the orchestrator
(regions, story, routes) before D1 starts.

### Endgame

Merge main in, one `--verify` of the union gates (rivers, roads, worldmap,
worldmap-v2, locale, altitude, terrain, biomes, env-truth, spawn-site,
village-harrowby), `--rebaseline` once, land on main, rebuild main's exe, shots
of the new world, ARCH_NODES in tuner.html.
