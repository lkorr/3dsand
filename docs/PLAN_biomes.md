# Biomes, water bodies and the Environment tab — plan of record

Status 2026-09-01: **the authoring layer and its validation are BUILT; the
worldgen consumer is NOT.** §1–§4 are what shipped and why; §5 is the wiring
plan, ordered by risk; §6 is the research this was built on, with sources.

## 1. What shipped

| Piece | Where | State |
|---|---|---|
| Biome files | `assets/biomes/<name>.json` (forest, meadow, pine, desert) | authored + validated; tree weights LIVE via the atlas mirror |
| Water-body presets | `assets/water/<name>.json` (tarn, kettle, marsh, spring_pool, oasis, playa, crater_lake, lava_pool, spawn_lake) | authored + validated + previewed; not read by worldgen |
| Generators | `assets/editor/watergen.js`, `biomegen.js` (pure, Node-runnable, hash RNG) | the preview truth; deterministic (gated) |
| The tab | `assets/editor/environment.js` (shell), `biome.js`, `water.js`, `envui.js`; `trees.js` mounts as a page | in `tuner.html` as **Environment**; the top-level Trees tab is gone |
| Engine | `src/sim/biomes.{h,cpp}`, gate `biomes` (`selftest_biomes.cpp`) | loads + validates every file; CPU only |
| Sync | `scripts/seed_environment.mjs --seed / --sync / --check` | biome weights → species `placement.biomes` → re-bake |
| Gates | `node scripts/test_environment.mjs`, `bash scripts/check_environment.sh`, `--selftest --gate biomes`, `check_invariants.py` `biome order` | all green at landing |

## 2. The data model, and why this one

**A biome SELECTS from component libraries and says how often and where.** The
alternative — each species listing the biomes it grows in, which is what the
tree files did — is fine for one component and wrong for four: "what is in a
forest" then lives in forty files. So:

* `assets/trees/<species>.json` owns the tree's look and its physical
  tolerances (altitude band, slope, shade). Its `placement.biomes` block is
  now a **mirror**, written by the biome page's Sync atlas or by
  `seed_environment.mjs --sync`, because the `.svtree` bake reads one file per
  species. The `biomes` gate fails if the mirror is stale.
* `assets/water/<preset>.json` owns a body of water's shape and its default
  tile/rarity.
* `assets/biomes/<biome>.json` owns the **feature stacks**:

```
cover.plants[]      {material, head, chance (1-in-N columns), height, conditions}
trees.species[]     {species, weight, conditions}        + trees.tile, trees.density (%)
water.features[]    {preset, tile (m), rarity (1-in-N tiles), conditions}
caves.features[]    {preset near_surface|deep, threshold, rarity, conditions}
terrain.overrides   {worldgen.<key>: value}
climate             {temperature, moisture}               (future 2-D grid coordinates)
index               worldgen B_* id, -1 for a biome the engine does not know yet
```

**Every row carries the same placement chain** — rarity, then `conditions =
{minY, maxY, maxSlope (Q8), nearWaterMax, nearWaterMin (m), patchThreshold}`.
This is Minecraft's `placed_feature` modifier list (`rarity_filter → count →
in_square → heightmap → biome → block/surface predicates`) reduced to the
predicates our worldgen can evaluate per column. It was chosen over Bedrock's
`scatter_chance + iterations` and over a bespoke schema because it is the
model the most people already know how to read, and because it is exactly
per-tile, integer-friendly and order-independent — the things rule 1 wants.

**Rarity is authored in one form per stack and the others are shown
read-only.** Trees: a weight (share of the stack) with `%` and per-hectare
beside it. Water: `1 in N tiles of T m` with `%`, per hectare and per km².
Cover: `1 in N columns` with `%` and per hectare before the patch mask. The
UX literature is unambiguous that users misread "weight" as "chance" and that
"1 in X" with varying X distorts perception; showing the conversions is the
fix (§6 C7).

## 3. The water-body shape language

`watergen.js` — one preset, one instance, one column at a time:

* **Footprint.** Superellipse `|x/a|^n + |z/b|^n = 1` (`squareness` = n: 2
  ellipse, 4 squircle, 1.2 diamond), `aspect`, rotation (random per instance
  or authored), an fBm **domain warp** of the sample point scaled to the radius
  (`warpAmp`, `warpFreq`, `warpOctaves`), optional **lobes** (extra ellipses
  smooth-min'd on; the bays a warped disc cannot make) and **islands**
  (subtracted domes). `instanceOf()` rolls everything per instance from the
  seed; `fieldAt()` returns the normalised radius `u` (0 centre, 1 shore, >1
  land) and the island dome height.
* **Bathymetry.** `depth = rimDepth + (depth - rimDepth) · profile(u)` where
  `profile` is a **monotone cubic** (Fritsch–Carlson) through authored points.
  Parabola `[[0,1],[0.5,0.75],[1,0]]` is the engine's `pondAt`; bathtub, shelf,
  cone and flat are the other presets; the section canvas is the editor. Plus
  floor noise fading to zero at the shore. Limnology's *volume development*
  `Vd = 3·mean/max` (1 cone, 1.5 parabola, 3 bathtub) is reported per body.
* **Fill.** A liquid material (or `none` → dry bed: playa) at `surface +
  level` (negative = drought shore), optional surface skin (ice).
* **Berm.** The engine's structural containment: the core is FORCED to
  `waterline + height`, then ramps to natural ground over `width`. The gate
  refuses `berm.height > shore.lift` when there is a shore band — the same
  trap `tuning.cpp` warns about for `pondBerm ≥ shoreLift`.
* **Bed and shore.** Shallow/deep bed by water depth; mud ring, wet-moss
  chance, and a **distance-ordered plant stack** (row 1 rolls first; each row
  has its own reach) on land within `shore.band` and under `shore.lift`.
* **Aquatic bands by depth.** Emergent (reeds, from the bed, meant to break
  the surface: real cattails stop at ~0.7 m), floating (lily pads on the
  surface, 0.3–2 m), submerged (weeds held `clearance` under the surface).
  **Band widths are not knobs** — they fall out of the bathymetry.

`columnAt(P, I, M, mx, mz, natural, surf, x, z, seed)` is the one answer to
"what is at (x, z)"; `generateWaterBody` and the biome swatch both call it.

## 4. The biome swatch

`biomegen.generateSwatch` composes a square of the biome: a heightfield
(preview-only fBm), water rows stamped through `columnAt` (**showcase** mode
places one of every row, centred — at true rarity a 24 m square is usually
empty, which is the truth about rarity and useless for judging a shoreline),
cover plants behind the patch mask and conditions, and trees by weighted draw
+ conditions from the real treegen cells (compacted per species/variant and
cached). A gated-out pick grows NOTHING rather than re-rolling, as the engine
does. Deterministic; asserted.

## 5. Wiring worldgen to the table — the plan, by risk

Two constraints bound every step (from the worldgen map, 2026-09-01):

* **(i)** anything inside `MIRROR-BEGIN noise|height|landheight` must have a
  C++ twin that survives `check_invariants.py`'s token compare;
* **(ii)** a new buffer must be bound at the same binding in BOTH `simBGL_`
  and `simSlimBGL_` (`far`/`fardown` call `genCell`), plus `R(...)` rows in
  `pass_table.def` and a `check_pass_table.py` pass. Bindings 0-4, 7, 16-26
  are taken; **27 is free.**

**Step 1 — `BiomeSet` → storage buffer at binding 27; cover blocks + tree
chance read it.** `LoadBiomeSet` already produces the structs; add a
`BiomeTableWords()` packer (header + per-biome record: skin/subsoil ids,
skinDepth, patch threshold/cell, treeTile, treeDensity, cover-row table with
material/head ids, chance, height cells, conditions). Replace in
`worldgen.wgsl`: ground skin (:2452-2512), desert cover (:2903-2919), pine
heath (:2932-2941), alpine cushion (:2963-2973), the flower chain
(:1858-1889), the shore species set (:2652-2686) and `TUNE_TREE_CHANCE_*` in
`treeInfoAt` (:1165-1168). **All outside every mirror; no CPU counterpart.**
Moves the hash once; rebaseline. Gates to watch: `sleep`, `ca-skip`,
`ca-slope`, `terrain` pass D (a new inert plant must still settle).

**Step 2 — `biomeAt` from the table.** Not mirrored. Either a `{lo, hi} → id`
band table (what the tuner's strip edits today through the three
`TUNE_*_THRESHOLD`s), or the plan-of-record `climateAt(cold, arid, pick, h,
slope)` grid (PLAN_terrain_overhaul §G, RESEARCH_worldgen §8) with the biome
files' `climate.temperature/moisture` as the grid coordinates. Keep the return
a `u32` id so `Col.biome` and its 14 consumers are untouched. Widening past
four biomes means bumping `treeatlas.h kBiomeCount`, the `.svtree` weight
block and `treegen.js BIOME_ORDER` together — `check_invariants.py` `biome
order` fails loudly on a partial edit.

**Step 3 — water presets drive pond geometry.** `pondInfo/pondAt/pondNear/
bermLift` are inside `MIRROR-BEGIN height`. The honest path: a C++ copy of the
per-biome water table with the same identifier spelling (normaliser alias, as
`POND_TILE → pondTile`), `biomeAt` mirrored into `world.cpp` too, and
`World::TerrainHeight` reading the same table. The profile curve must become
integer — sample it to a Q8 table of 17 points at load and lerp, so the
shader and C++ evaluate the same integers. The `terrain` gate's A6 (berm
invariant) and the `waterbody` gate's analytic bowl inversion are the tests
that will move; `waterbody.h`'s `ParabolicBowl` kind gains a `Profiled`
sibling. Cheaper interim: keep geometry scalar and give the shore/aquatic
layer (already outside the mirror) the per-preset treatment.

**Step 4 — caves per biome.** `caveBands` is outside the mirror; a per-biome
threshold pair is a table read. Rivers stay a named non-goal
(PLAN_water_master §5).

## 6. Research notes (2026-09-01), with sources

**A. Lakes and ponds in shipped worldgen.**
Minecraft's old lake feature: 4–7 random ellipsoids unioned in a 16×8×16 box,
aborted if the mask touches the box edge or a top-half neighbour is liquid; a
1/4 per-chunk roll, skipped in deserts (https://minecraft.wiki/w/Water_Lake).
Its 1.18 **aquifers** replaced lakes with a fluid LEVEL FIELD per 16×40×16
cell (floodedness / spread / barrier noises; disabled within 12 blocks of the
preliminary surface) — "fluid level as a field, not a feature"
(https://gist.github.com/jacobsjo/0ce1f9d02e5c3e490e228ac5ad810482). Shore
beds come from **surface rules**, not from the lake
(https://github.com/TheForsakenFurby/Surface-Rules-Guide-Minecraft-JE-1.18).
The **placed-feature modifier chain** — a list-of-positions pipeline:
`rarity_filter{chance}`, `count`, `noise_based_count`, `in_square`,
`heightmap`, `biome`, `block_predicate_filter`,
`surface_water_depth_filter` … — is the model adopted here
(https://minecraft.wiki/w/Placed_feature). Biomes attach features in 11
ordered decoration steps with a cross-biome ordering constraint so chunks
straddling biomes stay deterministic (https://minecraft.wiki/w/Biome_definition).

**Hydrology.** Priority-Flood (Barnes, Lehman, Mulla 2014,
https://arxiv.org/abs/1511.04463) fills depressions in O(n) for integer
elevations and yields each basin's **spill elevation** — the principled
waterline for an authored "fill to the spill" lake. Fill-Spill-Merge (Barnes,
Callaghan, Wickert 2021, https://esurf.copernicus.org/articles/9/105/2021/)
routes a water VOLUME through the depression hierarchy → partial lakes as a
function of moisture, which is how a biome's `moisture` becomes "how full".
Génevaux et al. 2013 generate terrain FROM a drainage network (river-centric;
https://www.cs.purdue.edu/cgvlab/www/resources/papers/Genevaux-ACM_Trans_Graph-2013-Terrain_Generation_Using_Procedural_Models_Based_on_Hydrology.pdf).
Red Blob's polygon maps: lakes = non-ocean water polygons, moisture = decayed
distance from fresh water, biome = elevation × moisture table
(https://www.redblobgames.com/maps/mapgen2/).

**Shape language.** Superellipse (https://en.wikipedia.org/wiki/Superellipse);
metaballs / smooth-min unions
(https://github.com/curv3d/curv/blob/master/docs/shapes/Distance_Field_Operations.rst);
domain warping (https://iquilezles.org/articles/warp/); the "circle + noise"
island recipe inverted for lakes
(https://heredragonsabound.blogspot.com/2016/10/making-islands.html).
Bathymetry: volume development `Vd = 3·mean/max`; kettles are steep-walled
flat-floored 8–45 m (https://www.britannica.com/science/kettle), oxbows
shallow crescents, playas cm-deep with slopes < 0.2 m/km, Crater Lake is a
bathtub (mean/max 350/594 m). Vegetation by depth: emergents to ~0.7 m
(*Typha latifolia* max 68 cm,
https://www.fs.usda.gov/database/feis/plants/graminoid/typlat/all.html),
floating-leaved 0.3–2 m, submerged to the light limit (~4.5 m in clear water,
https://www.dnr.state.mn.us/shorelandmgmt/apg/wheregrow.html).

**Deterministic placement.** Jittered grid / tile hash controls COUNT, Poisson
disk controls SPACING (https://www.redblobgames.com/x/1830-jittered-grid/);
for a true uniform infinite process draw the per-tile count from Poisson(ρA)
(https://www.boristhebrave.com/2024/10/30/infinite-uniform-point-distributions/).
Minecraft's Bernoulli-then-`in_square` is what the biome rows do; a coarser
tile than the footprint gives a minimum spacing for free.

**B. Biome selection models.** Minecraft 1.18 multi-noise (6-D nearest
point; needs a dedicated slicing editor, Snowcapped,
https://github.com/jacobsjo/snowcapped); Whittaker / Red Blob 2-D table; Dwarf
Fortress rainfall × drainage with temperature variants
(https://dwarffortresswiki.org/index.php/Biome); Valheim radial + sector with
bilinear zone-corner blending; Terraria fixed layout. **Verdict:** a 2-D grid
(temperature × moisture) is the only one a human authors by clicking cells —
hence the `climate` coordinates in every biome file — and the Minecraft lesson
that transfers is the DECOUPLING: terrain from continuous fields, biome label
from the same fields, so borders never need height blending
(PLAN_terrain_overhaul §G already says "biomes do not own height functions").

**C. UI rules applied.** (1) Layer stack, not node graph, for a biome's
features (World Creator vs Gaea). (2) Show evaluation order; reorder with
arrows (Blender T38178). (3) Master/detail with a compact list — the sidebar.
(4) "inherited from Worldgen" as WORDS in the label, not an icon (Unity
Foundations inheritance pattern; GitLab's rejected icon-only exploration) —
the terrain overrides section. (5) Presets are assets, copy-on-apply — "new
from…" on the water page. (6) Curve editor: click-empty-to-add, drag, per-key
delete, tangent presets — the profile canvas. (7) Rarity: one authored form,
conversions shown. (8) Live preview = 2-D map + 3-D swatch — plan + section
+ voxel view. (9) Schema-driven forms that round-trip to JSON (Misode) — the
`envui.js` row vocabulary is `tuner_schema.js`'s. (10) Progressive disclosure
per row — the ⚙ conditions line.
