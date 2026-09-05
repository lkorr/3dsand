# World Map: an authored finite overworld — plan of record

Status 2026-09-04: **P0–P5 BUILT** on branch `worktree-world-map-p1`
(P0 `ffa2c3f` on `worktree-world-map`; P1 `c61f928`, P2a `3693314`, P2b
`48b5168`, P3 `907179f`, P4 `5e67077`, P5 `c76669d`). `DESIGN.md` §9d is the
architecture truth for what shipped; this file is the plan and its
rationale. Follow-up plans named at the end.

## Context

The Environment tab authored biomes (`assets/biomes/*.json`) that worldgen
never read: biome came from a noise band and every cover / density / cave
decision was a hard-coded chain. Only tree weights got through, via a
`.svtree` bake. The owner's decision (2026-09-04): the world becomes a
**finite authored core** — a ~20 km painted map with an ocean ring outside,
biome regions and structure sites placed by hand on a map in the tuner,
everything inside a region still seeded-procedural (Noita's model). The
engine's infinite i32 math stays; only the MAP is finite.
`docs/RESEARCH_worldgen.md` §8 had already argued for exactly this and asked
that Tier A be "one function so the swap to a coarse-map lookup is a
one-function change"; this plan is that swap plus the editor and a site table.

Owner decisions: v1 roster forest, meadow, pine, desert, **tundra, swamp,
alpine, ocean**; the hand-coded set pieces (deck, arena, oil/lava pools, ruin
scatter) are **deleted** — only the harness pad and its test tarn survive;
dungeons are site table + `stamp` kind only (the interior grammar is a separate
plan); the 2-D biome-painting map in the tuner is the headline deliverable; no
per-phase rebaseline — one verification push at the end.

## The three tiers (the seed discipline)

```
Tier A  the MAP      seed-INDEPENDENT: biome plane, landform plane, sites. Same on every seed.
Tier B  the REGION   seeded: the boundary warp (<= cell/4), rule-placed sites, which lake.
Tier C  the LOCAL    seeded: trees, cover, caves, boulders.
```
A Tier-A read never takes `seed`; a Tier-B/C read always salts it.

## Scale and arithmetic

`kVoxelMeters = 0.10`, 20 km = 200,000 voxels; cell = 1024 voxels (102.4 m),
196×196 cells, ~115 KB for three u8 planes. Authoring, not storage, is the
constraint: 38 K cells paint, 611 K do not; a biome region must stay many
tree tiles wide (`worldgen.wgsl` says so). `vnoise2d` (log2 cell, Q15) has no
i32 ceiling; nothing new calls the legacy `vnoise`. The old pools' `pd2 =
pdx*pdx + pdz*pdz` (overflow past 4.6 km) went with the pools.

## Architecture (as built)

- **Asset** `assets/worldmap/<name>/`: `map.json` (cellLog2, size,
  originCell, seaLevelY, oceanFadeCells, warpAmpVox, `biomes[]` palette by
  NAME, `sites[]`, `rules[]`) + `map.svmap` (`SVMP`, three u8 planes: biome,
  landform, moisture). `scripts/seed_worldmap.py` draws the shipped default.
- **One buffer at binding 31** (`worldMap`, both sim layouts; 27 was taken —
  `PLAN_biomes.md` was stale): header | biome records | cover rows | planes |
  site index plane | site records | stamp blocks. `src/sim/worldmap.h` is the
  layout; `check_invariants.py world map layout` holds it to the shader's
  `WM_*` consts.
- **Samplers** (`worldgen.wgsl`): `wmBiome/wmCover` (P1), `mapBiomeAt`
  behind `biomeAt` (P2a), `mapLandformQ8` → `landformOctave` = the
  continental rung inside the height mirror (P4), `seaLevelY()`, `wmSiteAt` /
  `sitePadAt` (in the mirror) / `wmStampCell` / `siteKeepOut` (P5). CPU twins
  in `world.cpp` spelled identically where they enter a mirror; the `terrain`
  gate's C1 (9,409 columns) is the per-voxel proof; the `worldmap` gate
  holds `World::MapBiomeAt` to the plane and to the GPU skin.
- **Stamps are worldgen INPUT**, not CPU ops: template run-lists in the
  buffer (the tree atlas's encoding), overlaid per cell, so `far` shows a
  building at 6 km and nothing needs a drain or persistence.
- **Editor**: Environment → World map (`assets/editor/map.js`): biome brush,
  landform brush, pad box, stamp sites; `/api/worldmap[/planes]` routes.

## Lessons recorded while building it

- Data-driven plant heights must feed `genChunk`'s sky margin AND the far
  blocker band (`WM_B_MAX_COVER_H`, `WM_H_MAX_COVER_H`), or a skipped chunk
  drops voxels its own cover rows would have written.
- A gate that builds a pad must clear the air above it (`floaters`).
- Landform units are coarse (contAmplitude/256 = 4 voxels) against the fine
  octaves' ±84-voxel dip: painted land must sit ≥ ~132 over a sea of 112.
- A wandering driver NULL-read after adding a `pass::Buf` was a stale
  cross-worktree sccache object (two struct sizes in one binary); purge
  the TUs that include the header and rebuild with `SCCACHE_RECACHE=1`.
- The Vulkan descriptor pool is a never-refilled budget; allocation failure
  now aborts with a count instead of handing the driver a null set.

## Follow-ups (own plans)

- **Dungeons**: room-template grammar over `stamp:` + a `proc:` kind
  (`RESEARCH_worldgen` §6.7 (i)+(ii)); the ruin shell that was deleted in
  P2b returns as the first `proc:` generator if wanted.
- Slope-gated rules; sites wider than 512 voxels; kelp/aquatic rows under
  the sea; moisture plane → lake fullness (`PLAN_biomes` §6); `contLog2`
  retirement; `meta.svm` recording map name + hash (save identity).
- The World map page: heightmap backdrop, PNG import/export, rules editor
  (rules are JSON-only today).
