# PLAN — Dungeons (kit pieces, stitched layouts, placed in the world map)

Status: DRAFT 2026-10-12 (orchestrator). Owner answers recorded in §0. This
document is the FORMAT CONTRACT the parallel packages build against: §3 is
frozen once wave 1 launches; a package that needs to change it says so on the
board and the orchestrator amends this file, never a package alone.

## 0. What the owner asked for (2026-10-12)

- Dungeons = connected reusable PIECES (corridors, chambers, pit rooms, stairs,
  shafts, entrances...) like Oblivion's kit, EXCEPT every piece is driven by
  SLIDERS (ceiling height, corridor width, rubble, furniture, decoration,
  decay...) so dungeons do not read as repeated blocks.
- Own tuner tab ("Dungeons"): pick pieces, size/configure them, stitch them,
  place individual things, mobs, spawners, schedules; fly around in 3D and
  inspect; see how the entrance sits in the overworld (real worldgen around it).
- Three styles to start, each with its own art and own pieces: **caves**,
  **mines**, **ruins** (a ruined temple). Then three example dungeons, one per
  style, built WITH the tools.
- Decisions:
  - **Contiguous**, not instanced: dungeons are carved into the real world;
    the sim runs inside them; digging breaks into them.
  - **Arbitrarily large** dungeons must be possible. The examples are
    small-medium but each crosses the 51.2 m residency window somewhere.
  - **Hand stitching + seeded layout assist** ("grow a branch from this socket",
    keep / reroll). The saved dungeon is always explicit; nothing is
    re-randomised at runtime.
  - **Placed mobs + spawners**; monster schedule actions (guard, patrol, lair,
    ambush) on the existing schedule system.
  - **Material-driven fragility**: rock is static; styles use sim materials
    where they want it (timber supports burn, rubble is loose, water seeps).
    Nothing is awake at rest.
  - Gameplay furniture, all four: doors + containers (loot), light sources,
    traps/hazards, keys/locks/levers.
  - Examples go on Tirnhallow (`default` map) POIs.

## 1. Scale facts every package must hold in its head

- 1 voxel = **0.10 m**. Residency window = 512³ voxels = **51.2 m**. A 3 m
  corridor is 30 voxels wide; a 20 m cavern is 200 voxels; a 150 m dungeon is
  1,500 voxels = three windows. "Fits in a 64³ preview region" is false for
  almost every piece.
- Map coordinates in `map.json` are voxels (`at: [67685, -49628]` = 6.77 km E).
- Underground is dark: `render.enclosedAmbient` ≈ 0.012–0.016 regardless of sun.
  Emissive materials (`emission`) light rooms through the irradiance grid and
  glow field. Lighting a dungeon = placing emissive voxels.
- No vertical bound; the window scrolls in y. Natural caves are two 2D bands
  (≈4–10 m and ≈24–31 m below ground) plus a constant-level lava table.

## 2. Architecture

A dungeon is WORLDGEN INPUT, like a structure stamp: it regenerates identically
from the map, appears at any distance it is visible from, is not saved, and
player damage to it is ordinary chunk edits. Four layers, each with one owner:

| Layer | Truth | Applied by | Size |
|---|---|---|---|
| **L1 Shell** — rock carved into rooms, floors, walls, vaults, pillars, stairs, supports, rubble, scatter | piece generator (JS, `assets/editor/dungeon/`) emits a **primitive program**; `worldgen.wgsl` rasterises it per cell | genChunk, GPU, per cell | ~params only: tens of primitives per piece |
| **L2 Props** — furniture, rails, carts, statues, torches, altars, bones, crates | prop **templates** (generated or `.vox`) + instances (pos, quarter-turn yaw) | genChunk overlay, same run-encoded template format as stamps, extended (§3.5) | one template per distinct prop |
| **L3 Hand detail** — the author's per-voxel touch-ups in the 3D editor | `assets/dungeons/<id>/detail.svedit` (new **v3 = anchor-relative**, rigid) | CellOps after genChunk, like edit layers today | small, by intent |
| **L4 Entities** — mobs, spawners, doors/gates, containers+loot, keys/locks/levers, traps, waynodes | `assets/dungeons/<id>/refs.json` (a ref group, ids `dungeon/<id>/...`) | RefStore, as locales' refs | |

Why the shell is a GPU-evaluated primitive program and not baked voxels:

1. **Arbitrary size.** A baked 150 m dungeon is tens of millions of
   non-trivial voxels; via CellOps that is minutes of queue drain, via disk
   patches it is >1 MB/tick of upload. A primitive program is kilobytes for any
   size and costs nothing until a chunk inside it generates.
2. **Far field for free.** Ruins that break the surface show up in the far
   cascades because they run the same overlay.
3. **One implementation.** The tuner previews through `--voxserve` (the real
   GPU genChunk), the same path the World map's 3D pane uses. There is no JS
   rasteriser to drift from the WGSL one. The JS generator is the truth for
   LAYOUT decisions (where the pillars go, beam spacing, step count); WGSL is
   the truth for VOXELS (noise displacement, material pick, jitter).
4. **Organic shapes.** Caves need noisy walls; noise is native to worldgen.

The **seam rule** that kills the "connected blocks" look: pieces are unioned in
one field, not pasted. Carve primitives from adjacent pieces combine with a
smooth-min blend (`blend` voxels, per piece), wall noise is in WORLD space
(continuous across piece joins), and a socket join with mismatched profiles is
bridged by an auto-generated taper primitive (§3.3). A corridor's walls should
flow into the chamber it enters without a visible plane.

### 2.1 Placement and the anchor

`map.json` gains `dungeons[]` (§3.1). A dungeon has a local frame in voxels;
its ENTRANCE piece's anchor socket sits at the map anchor `at: [x, z]`, y =
`BareGroundHeight(x, z) + yOffset` (same resolution `LoadWorldMap` does for a
locale anchor, so edits to the terrain carry the dungeon rigidly with the
ground AT THE ANCHOR, never per column). `yaw` is a quarter turn. Everything
below is rigid in that frame: the per-column shear of `.svedit` v2 is exactly
what a dungeon must not have.

### 2.2 Interaction with natural worldgen

- **Natural caves and the lava table are suppressed** inside each piece's
  shell bounds + `keepOut` margin (default 24 voxels), so a cave band never
  slices through a crypt. A style may opt in to natural caves (`caves` kit:
  `naturalCaves: allow`).
- **The shell overrides terrain only where it says so.** Primitive ops are
  `carve` (→ air), `fill` (→ material), `skin` (wall layer: replace solid
  within d of the carved surface), `keep` (explicit no-op guard so a later
  carve cannot cut, e.g. a pillar). Outside every primitive the world is
  untouched.
- **Surface breach is a validation error unless the piece is marked
  `surface: true`** (entrances, ruin tops). Under-water breach (a lake or sea
  above) is an error unless the piece declares `flooded: true`.
- **Rest.** Worldgen births powder at rest and liquids full: rubble piles are
  generated at the style's angle of repose; pools fill to a flat level. No
  growth seeds (rule 2). The gate in §6 asserts ≤32 active chunks after a
  settle inside each example dungeon.
- **Page pool.** Carved rock breaks the JITTER/UNIFORM sentinel compression of
  buried bulk. The engine package measures pool residency standing inside the
  largest example and reports the number.

## 3. Formats (FROZEN at wave-1 launch)

### 3.1 `map.json` `dungeons[]`

```jsonc
"dungeons": [
  { "id": "shardwell",            // [a-z0-9_-]+, also the asset dir name
    "name": "The Shardwell",
    "dungeon": "shardwell",       // assets/dungeons/<this>/dungeon.json
    "at": [67685, -49628],        // anchor column, voxels
    "yOffset": 0,                 // anchor socket y = bare ground + yOffset
    "yaw": 90 }                   // quarter turns only
]
```

Loader: `worldmap.cpp` beside `ParseLocales`. A dungeon referenced by a map
but missing on disk is a load warning and is not placed (like a bad locale).

### 3.2 `assets/dungeons/<id>/dungeon.json`

```jsonc
{ "version": 1,
  "name": "The Shardwell",
  "style": "mines",               // default kit for new pieces; pieces may mix kits
  "seed": 81723,                  // dungeon seed; piece seeds derive from it + piece id
  "palette": { },                 // per-dungeon overrides of the style's material slots
  "pieces": [
    { "id": "p0",                 // stable, never reused in this file
      "kit": "mines", "type": "adit_entrance",
      "params": { "width": 34, "height": 38, "timberSpacing": 22, "decay": 0.3 },
      "seed": 0,                  // 0 = derive; else explicit (reroll writes it)
      "pos": [0, 0, 0],           // local voxels: the piece's ORIGIN socket position
      "yaw": 0,
      "links": { "back": { "piece": "p1", "socket": "front" } } },
    ...
  ],
  "entrance": { "piece": "p0", "socket": "surface" },   // the anchor socket
  "props": [ { "id": "x12", "prop": "mines/ore_cart", "params": {}, "pos": [..], "yaw": 90, "piece": "p3" } ],
  "detail": "detail.svedit",      // optional L3
  "refs": "refs.json" }            // optional L4
```

`params` stores only values that differ from the piece type's defaults? NO —
store the full normalized param set (like housegen's `.params.json`), so a
generator default changing later does not silently reshape authored dungeons.
Generators gain a per-type `version`; a version bump with a migration is how
defaults change.

### 3.3 Piece generator contract (JS, pure, Node-runnable)

`assets/editor/dungeon/kit_<style>.js` exports per piece type:

```js
{ type, label, version, SCHEMA,          // housegen-style rows: {sec} | {k,n,d,min,max,step,int|kind}
  PRESETS,
  generate(params, seed, ctx) -> {
    prims:   [Prim],                     // L1, local frame
    sockets: [{ name, pos, dir, w, h, profile, tags }],  // dir = unit axis; profile: rect|arch|round|rough|shaft
    props:   [{ prop, params, pos, yaw }],                // suggested L2, author may delete/move
    refs:    [{ kind, ... }],                             // suggested L4 (doors at sockets, chests in niches, lights)
    waynodes:[{ name, pos, links:[name|socket] }],        // nav graph; sockets link across pieces
    bounds:  { min, max },                                // local AABB incl. skin + blend
    surface: bool, flooded: bool } }
```

Rules (from housegen/mobgen): no `Math.random`; every choice is
`frand(seed, key...)` keyed by WHAT it decides, so one slider never reshuffles
unrelated choices; derived quantities are derived, not genes.
`dungeongen.js` (core) owns: transforms (quarter-turn yaw + translation),
socket matching (`w`,`h`,`profile` compatibility, auto-taper prim when they
differ within tolerance, refusal beyond), the union of all pieces into one
compiled program, validation (§5.4), and layout assist.

### 3.4 Primitive program (JS → C++ → GPU)

Compiled per dungeon by `dungeongen.compile(dungeon)` → `assets/dungeons/<id>/
shell.bin` + `shell.json` (human-readable twin, same content). The engine
loads the `.bin`; the tuner can regenerate both. A `--selftest` gate checks
`.bin` is current with its `dungeon.json` (hash recorded in the header).

`Prim` (dungeon-local voxels, already transformed):

```
op      : carve | fill | skin | keep
shape   : box | rbox(r) | ellipsoid | cylinder(axis) | capsule(a,b,r) |
          ramp(stairs: step h,d) | vault(arch: span, rise, axis) | spline_tube(pts[], r[])
xform   : min/max or centre/half-extent; quarter-turn only (no arbitrary rotation)
noise   : amp, scale, octaves, seedKey     -- world-space displacement of the surface
blend   : smooth-min radius with other carve prims (0 = hard)
mat     : style slot (wall|floor|ceil|trim|support|rubble|accent|light|liquid|ore|...) or explicit material name
scatter : optional rule: {kind: rubble|stalagmite|stalactite|crystal|moss|puddle|bones|ore_vein,
          density, size range, surface: floor|ceil|wall, seedKey}
priority: order; later wins, except `keep`
```

GPU side: a per-map **dungeon table** in the `worldMap` buffer (prims as fixed
16-word records, scatter rules, material slot table) plus a **sparse chunk
index**: hash of 64³ chunk coord → list of prim ids whose bounds (incl. noise
amp, blend, skin) touch the chunk. genChunk looks the chunk up ONCE; a chunk
with no entry pays one hash probe. Arbitrarily large dungeons cost index
entries proportional to touched chunks only. Engine package owns the exact
packing; the JS side owns `shell.json` semantics. Material names resolve at
load (`materials.json`), never ids in files.

### 3.4.1 `shell.json` / `shell.bin` schema — D1 AMENDMENT (2026-10-12)

D1 (engine) owns this section; D2 emits exactly this. It refines §3.4 without
changing its meaning. Engine loader: `src/sim/dungeon.{h,cpp}`
(`ParseShellJson`, `ReadShellBin`, `WriteShellBin`); the `dungeon-format`
gate holds every `assets/dungeons/*/shell.bin` to its `shell.json` twin
(CONTENT equality, not bytes) and to its `dungeon.json` (the hash).
`sandvox.exe --dungeon-bake <id>` rewrites `shell.bin` from `shell.json`
with the engine's own writer (handy while D2's binary writer is young).

**Frames and conventions.**
- Everything in the shell is in the DUNGEON LOCAL FRAME: integer voxel
  CELLS, Y up. Boxes are INCLUSIVE cell ranges `min..max`. A point (`a`, `b`,
  tube `pts`, `anchor`) names a CELL and means that cell's centre.
- `anchor` = the local cell of the entrance socket (shell.json / shell.bin
  always carry it). Placement (map `dungeons[]`, §3.1) -- THE SAME RULE AS
  D3's refs (§3.7 D3 AMENDMENT, `src/world/dungeon_refs.*`): `world = (at.x,
  BareGroundHeight(at.x, at.z) + yOffset, at.z) + R(yaw) · (local - anchor)`
  (the same bare ground a locale anchor reads). With `yOffset` 0 the anchor
  cell IS the ground cell at `at`.
- `R(q)`, q = yaw / 90 (quarter turns only), is LINEAR on cell coordinates
  (a rotation about the anchor cell): q0 `(x, z)`, q1 `(z, -x)`, q2
  `(-x, -z)`, q3 `(-z, x)`; y unchanged. (q1 takes local +Z to world +X.)
  [Corrected 2026-10-12 to D3's rule: the first D1 text had a `-1` cell
  rotation and the anchor one cell up; no data was written against it.]
- Noise and scatter are evaluated in LOCAL coordinates, so a dungeon is the
  same voxels wherever and however it is placed (only its world position
  moves). The tuner's isolated preview is therefore exactly the in-situ one.
- Material references are strings: a key of `materials` (a SLOT), else a
  name from `materials.json`. Unknown = the dungeon does not load
  (`dungeon-format` fails, the map warns and skips it).

```jsonc
{ "format": "sandvox-dungeon-shell", "version": 1,
  "dungeon": "shardwell",              // == the asset dir name
  "sourceHash": 3735928559,            // u32 FNV-1a of dungeon.json's bytes with every '\r' removed
  "anchor": [0, 0, 0],                 // local cell of the entrance socket
  "keepOut": 24,                       // natural caves/lava suppressed within bounds + this (voxels)
  "naturalCaves": false,               // true = the caves kit opts in: no suppression
  "materials": { "wall": "limestone", "floor": "flagstone", "rubble": "gravel", ... },
  "prims": [ Prim, ... ],              // ARRAY ORDER IS PRIORITY ORDER (see "evaluation")
  "props": { "templates": [ Template, ... ], "instances": [ Instance, ... ] } }
```

**Prim** — common fields: `op` (`carve|fill|skin|keep`), `shape`, `mat`,
`noise` (optional `{amp, scale, octaves, seed}`: amp voxels 0..64, scale =
LOG2 of the lattice cell in voxels 1..10, octaves 1..3, seed u32), `blend`
(carve only: smooth-min radius in voxels with the carve prims before it,
0..256), `skin` (skin op: wall-layer thickness, voxels 1..64), `level`
(carve with a liquid `mat`: local y of the flat surface), `firm` (carve: the
firm-shell material, below), `piece` (optional string, informational: the
piece id), `scatter` (optional array, ≤ 4 rules). `mat` is required for fill
and skin, optional for carve (absent = air; else a LIQUID), absent for keep.
Optional EXTRAS (added after D2's first pass, 2026-10-12): `openOnly: true`
on a fill = it lands only where the cell is still AIR at that point of the
ordered pass (the carve's or the sky's) -- a rubble heap or a fallen block
must not turn the buried rock inside a wall into gravel; `mat2` + `checker`
(cells, 1..256) on a fill or a skin = a checker of `checker`-cell squares in
the dungeon's LOCAL XZ plane, `mat` on the even squares ((⌊x/c⌋ + ⌊z/c⌋)
even) and `mat2` on the odd ones (tiled floors).
Shapes:

| shape | fields | the solid it describes |
|---|---|---|
| `box` | `min`, `max` | the cell box |
| `rbox` | `min`, `max`, `r` | the box with edges rounded by `r` (clamped to half the shortest side) |
| `ellipsoid` | `min`, `max` | the ellipsoid inscribed in the box |
| `cylinder` | `min`, `max`, `axis` `x\|y\|z` | the (elliptic) cylinder inscribed in the box along `axis` |
| `capsule` | `a`, `b`, `r` | segment a–b swept by radius r (cells; any length — the engine splits at 256) |
| `tube` | `pts` [[x,y,z],...], `r` [..] (one per point) | a polyline of tapered capsules (radius lerps along each segment); the JS densifies its own spline, the engine joins straight segments |
| `ramp` | `min`, `max`, `dir` `+x\|-x\|+z\|-z`, `stepH`, `stepD`, `height`, `below` | stairs: the floor PROFILE starts at `min.y` at the edge opposite `dir` and rises `stepH` every `stepD` cells along `dir` (`stepD` ≥ 1; 1/2 = a smooth 1-in-2 ramp). The solid is the cells of the XZ footprint with `profile − below ≤ y < profile + height` (and `y ≤ max.y`). Carve with `height` = headroom → a stair passage; fill with `below` = thickness, `height` 0 → a solid flight under a hall. `profile` = the walking surface (first air cell) |
| `vault` | `min`, `max`, `axis` `x\|z`, `rise` | a barrel vault running along `axis`: walls from `min.y` to `max.y − rise`, then a half-ellipse of half-span = half the cross width and height `rise` |

Limits (refused by the loader with the prim's index): every box axis ≤ 2048
cells, radii ≤ 512, `rise` ≤ the box height, |local coord| ≤ 2^20, at most
64 prims and 32 prop instances whose inflated bounds touch one 64³ cell.

**Op semantics — the evaluation, per cell** (`worldgen.wgsl` `dgCell`, CPU
twin `dungeon.cpp` `EvalCell`; d = the shape's signed distance + noise,
negative inside):
1. `C` = the smooth union, in array order, of every `carve` prim's d
   (`blend` = the smooth-min radius of each prim with the union before it).
   `C < 0` → the cell is carved: AIR, or the carve's LIQUID (`mat` a liquid,
   with `level` = local y: carved cells at or below it are that liquid, full).
   The carve that "owns" a carved cell (liquid, firm) is the one with the
   smallest own d.
2. THE FIRM SHELL (rest, rule 2): a terrain cell within 2 voxels outside the
   carve (0 ≤ C < 2) that is a POWDER or a LIQUID becomes the owning carve's
   `firm` material (prim field `firm`; default slot `wall`, else `stone`), so
   no sand / sediment / lake water spills into a carve on its birth tick.
3. Then every non-carve prim, and every carve prim again, in ARRAY ORDER,
   later wins: `fill` (d < 0) → `mat` (or the checker; `openOnly`: only if the cell is air at that point); `keep` (d < 0) → the TERRAIN cell back
   (natural rock — a pillar), and a kept cell is not re-cut by any LATER
   carve; `skin` (d < 0 inside the skin prim's shape, the cell solid terrain
   and not carved, and C ≤ `skin` voxels) → `mat` — the wall layer;
   `carve` (own d < 0, not kept) → air/liquid again (a doorway through an
   earlier fill). The smooth-min fillet of step 1 is NOT re-cut by step 3.
4. Scatter rules of each prim (below), in array order.
5. Prop instances (§3.5), in array order, last wins.
Outside every prim's inflated bounds the world is untouched, bit for bit.

**Scatter** rule fields: `kind`, `density` (0..1), `size` [min, max]
(cells, ≤ 32), `surface` (`floor|ceil|wall|any`), `mat` (string; default by
kind: rubble→slot `rubble` else `gravel`; stalagmite/stalactite→`wall` else
`stone`; crystal→`crystal`; moss→`moss` else `moss_patch`; puddle→`water`;
bones→`bone`; ore_vein→`ore` else `iron`), `seed` (u32), `spacing`
(optional lattice cell, cells; default 2·size.max·(repose run)+4). A rule
applies to the cells INSIDE its prim (own d < 0 for floor/ceil kinds; the
solid band behind the prim's carve surface for moss/ore). Kinds, all born at
rest:

- `rubble` (floor): square-ish piles on a lattice, height = size, flanks at
  the material's angle of repose (`materials[].repose`), grown up from the
  carve floor below (the first solid within the pile's height).
- `stalagmite` / `stalactite` / `crystal`: cones of height = size and base
  radius size/3 (crystal size/2) standing on the floor / hanging from the
  ceiling (`crystal` follows `surface`; `wall` and `any` pick floor or
  ceiling per spike).
- `moss`: the first solid cell behind a carve surface on the `surface`
  side(s), patchy (value noise against `density`).
- `puddle`: floor cells whose four side neighbours and the cell below are
  solid become the liquid (one cell deep, contained by construction).
- `bones`: one-cell scatter lying on the floor, discs of radius = size.
- `ore_vein`: solid cells within `size` voxels behind the carve surface
  where 3D value noise exceeds the density threshold.

**Props.** `Template` = `{ "id": "mines/ore_cart#3a9f", "size": [nx, ny, nz],
"palette": ["@carve", "plank", "wall", ...], "voxels": [[x, y, z, p], ...] }`
— `p` indexes `palette`; `"@carve"` is the CARVE CODE (the cell becomes AIR);
a cell not listed is template-air ("leave the world alone"). Sizes ≤ 256 on
each axis. `Instance` = `{ "template": <index into templates>, "pos": [x, y,
z], "yaw": 0|90|180|270, "id": "x12" }`: the template rotated by yaw
(template-local `(x, z)` → q1 `(z, nx−1−x)`, q2 `(nx−1−x, nz−1−z)`, q3
`(nz−1−z, x)`, i.e. `R` re-based to the footprint's min corner) lands
with its rotated footprint's MIN CELL at `pos`. Any y; no pad, no sink band.
The JS dedupes identical (prop, params, seed) into one template; the engine
additionally shares identical packed (template, total yaw) blocks.

**`shell.bin`** (little-endian, every field 4 bytes): `'SVDG'`
(0x47445653), version 1, sourceHash, flags (bit 0 naturalCaves), anchor
x y z, keepOut; STRINGS: n, then per string u32 byteLength + UTF-8 bytes
padded with zeros to 4; MATERIALS: n, then n × (slot string, material
string); PRIMS: n, then n × 26 words —
`[0] op (0 carve 1 fill 2 skin 3 keep) [1] shape (0 box 1 rbox 2 ellipsoid
3 cylinder 4 capsule 5 ramp 6 vault 7 tube) [2] mat string (0xFFFFFFFF none)
[3..5] min | a [6..8] max | b [9] r | rise | stepH [10] axis (0 x 1 y 2 z) |
dir (0 +x 1 -x 2 +z 3 -z) [11] stepD [12] height [13] below [14] noise amp
[15] noise scale [16] noise octaves (0 = no noise) [17] noise seed
[18] blend [19] skin [20] level (i32; INT32_MIN = none) [21] firm string
[22] tube first point [23] tube point count [24] first scatter [25] scatter
count`; TUBE POINTS: n, then n × (x, y, z, r); SCATTER: n, then n × 8 words
`kind (0 rubble 1 stalagmite 2 stalactite 3 crystal 4 moss 5 puddle 6 bones
7 ore_vein), density Q16 (floor(d·65536+0.5)), size min, size max, surface
(0 floor 1 ceil 2 wall 3 any), mat string, seed, spacing (0 = default)`;
TEMPLATES: n, then per template `id string, nx, ny, nz, nPalette, palette
strings…, nVoxels, nVoxels × (x, y, z, p)`; INSTANCES: n, then n × (template,
x, y, z, yaw/90, id string); then `'END!'` (0x21444E45). Prim `piece` strings
go in a trailing PIECES block before `'END!'`: n (== prims) × piece string.
Then, ONLY when some prim uses an extra, the EXTRAS block, still before
`'END!'`: `'PRMX'` (0x584D5250), n (== prims), n × 4 words (flags: bit 0
openOnly; mat2 string or 0xFFFFFFFF; checker; 0). A reader that sees
`'END!'` where `'PRMX'` could be has no extras. Unused fields are 0.

### 3.5 Props (L2)

`assets/dungeons/props/<kit>/<name>.js|.vox`: generated props are tiny pure
generators (`SCHEMA` + `generate(params, seed) -> cells`), baked to the same
run-encoded column template the stamp path uses, with two extensions the
engine package adds: (a) any y (no pad / sink band), (b) a **carve code** so a
prop can clear air around itself (an alcove) distinct from template-air
"leave the world alone". Instances live in the dungeon table, indexed by the
same chunk hash. Identical (prop, params, seed) instances share one template.

### 3.6 `detail.svedit` — SVED v3 (anchor-relative)

v2 header; per entry `{i32 lx, ly, lz, u32 word}` in the DUNGEON local frame,
transformed by the placement at load. Applied as CellOps through the
MutationQueue after genChunk, exactly as edit layers are today. Authoring cap:
the editor warns over 200k voxels (that is what the shell/props are for).

D1 AMENDMENT: the header is v2's 32 bytes with version = 3, `count` = the
number of entries, flags 0; entries follow as `i32 lx, ly, lz, u32 word`.
The DUNGEON a v3 file belongs to is its parent directory's name
(`assets/dungeons/<id>/detail.svedit`); a v3 file elsewhere, or for a dungeon
the map does not place, contributes nothing (load warning). Detail layers
apply after the map's and the locales' layers, in `dungeons[]` order.
Writer: `WorldEdits::WriteDungeonLocal` (C++).

### 3.7 Entities (L4) — `refs.json`, ids `dungeon/<id>/<name>`

Positions in the dungeon local frame, transformed at load. New/extended kinds
(gameplay package):

- `npc` (existing) — hostile profiles from `behaviors.json` work as-is.
- `spawner` — `{ mobs:[{base, weight}], count, radius, respawn: never|ticks:N|onLeave,
  schedule, behavior, trigger: always|onEnter(regionRef) }`. Deterministic: spawn
  decisions are a function of tick + ref id; spawned mobs get derived ids.
- `door` (existing) + `props.lock: "<keyId>"`; `gate` (portcullis/bars, voxel
  door variant opened by signal); `lever` / `plate` (pressure plate) emitting a
  named **signal**; receivers (`gate`, `door`, `trap`) list `on: "<signal>"`.
- `key` — an item ref; picking it up grants `keyId`. Keys are ordinary items
  (`assets/items/`), so they drop/persist like items.
- `container` (existing) + `props.loot: "<table>"`; loot tables in
  `assets/loot/<name>.json` (weighted rows, rolls; deterministic by ref id).
- `trap` — `{ type: collapse_floor | spikes | dart | flood | fire_vent, region,
  on: signal|plate }`; all of them act by emitting MutationQueue ops (a
  collapse converts a static floor material to its powder twin; flood opens a
  water source volume; fire_vent places fire). No direct buffer writes.
- `waynode` (existing) — generated per piece; patrol routes reference them.
- Schedules: `assets/schedules/*.json` rows gain `do: guard | patrol | lair | ambush`
  (`at:` a waynode / marker tag; `patrol` takes `route: [waynode...]`).

## 4. The three kits (starting piece lists; sliders are examples, not limits)

Universal sliders on every piece (core supplies them): size (w/l/h), ceiling
height, floor unevenness, wall roughness (noise amp/scale), **decay/age**
(rubble, broken trim, collapsed corners), **moisture** (moss, stains, puddles,
drips), **deco density**, **light** (count, type, warmth), blend radius,
symmetry break.

**Caves** (limestone/basalt/ice by palette; organic; few right angles)
- `mouth` (surface: hillside/sinkhole/sea-cave variants, vegetation lip)
- `tunnel` (spline; radius profile; squeeze points; meander)
- `cavern` (ellipsoid cluster; dome height; stalactite/stalagmite density;
  columns; flowstone; crystal clusters; mushroom patches)
- `shaft` / `pit` (vertical drop; ledges; rope/no)
- `chasm` (long crack; natural bridge / rope bridge)
- `grotto_pool` (still water, flat level; glow fungus)
- `crawl` (low squeeze, 1–1.5 m)
- `rubble_choke` (blocked passage with dig-through)

**Mines** (timber + stone; rectilinear with sag; industrial clutter)
- `adit_entrance` (timber portal into a slope, spoil heap, sign, rails out)
- `drift` (corridor; timber set spacing, beam size, broken/sagging %, rails,
  lamp hooks; side niches)
- `crosscut` / `junction` (T, X, offsets)
- `incline` (ramp / stairs, with or without rails)
- `shaft` (vertical; ladder; cage + headframe if surface; landings per level)
- `stope` (ore chamber; pillar grid; ore vein density + ore type; scaffolds)
- `collapse` (cave-in section; rubble cone; loose ceiling)
- `flooded_sump` (water level slider)
- `camp` (miners' room: bunks, table, crates, tools)

**Ruins — a ruined temple** (dressed stone; symmetry; decayed grandeur)
- `ruin_entrance` (surface: broken colonnade, stairs down, toppled columns,
  overgrowth)
- `hall` (column count/rows, column style, vault vs flat, aisle width, banners)
- `corridor` (arch spacing, alcoves/niches, tiled floor pattern)
- `stair` (straight / spiral / switchback)
- `crypt` (niche grid, sarcophagi, bones)
- `pit_room` (arena or trap pit; ledges; bridge)
- `sanctum` (altar, raised dais, braziers, statue)
- `flooded_hall` (water level slider)
- `collapsed_passage`

Each kit also ships props (bunks, crates, carts, rails, lanterns, barrels,
altars, statues, sarcophagi, braziers, bones, candles, tables, rubble piles).

## 5. The tuner — top-level "Dungeons" tab

New `data-tab` + `<section id="view-dungeons">` (`scripts/check_tabs.sh`).
Modules under `assets/editor/dungeon/`. Preview always comes from
`--voxserve` (real worldgen); new voxserve commands:
`DUNGEON <json>` (load a scratch dungeon/piece program, no map needed,
optionally anchored at a map column so the TERRAIN around it is real) and the
existing region fetch. WorldView renders it.

### 5.0 `--voxserve` dungeon protocol — D1 AMENDMENT (2026-10-12)

Line protocol as the rest of voxserve (`src/tools/voxregion.h`): one ack
line `OK <n> <ms>` / `ERR <msg>`; paths are the REMAINDER of the line,
two paths separated by `|`. `<src>` is a path to a `dungeon.json` (its
sibling `shell.bin`, else `shell.json`, is loaded), a `shell.json` or a
`shell.bin`. The tuner writes an unsaved program to a temp file first.

```
DUNGEON ISOLATE <seed> <src>|<info.json>
    the program alone, in solid rock: anchored at a fixed column far from
    every map feature and deep enough that no ground, cave or lava is within
    reach (a cut-away rock block). The map's own dungeons stay.
DUNGEON PLACE <x> <z> <yOffset> <yaw> <seed> <src>|<info.json>
    in situ at an arbitrary map column, on the real terrain.
DUNGEON MAP <id> <seed> <src>|<info.json>
    replaces the map's own dungeons[] entry <id> (its anchor, yaw, yOffset)
    with this program: editing a placed dungeon in place.
DUNGEON CLEAR
    drops the scratch program; the map's dungeons are what they were.
```

Each loads the program, re-packs the world map with it (a scratch dungeon
id `_scratch`, or the replaced id) and uploads it, so every later `REGION`
returns real genChunk voxels with the dungeon carved in. `info.json` =
`{ ok, id, seed, origin: [x,y,z] (world cell of local 0,0,0), yaw, anchor:
[x,y,z] (world), groundY, bounds: {min, max} (world, inclusive, inflated),
keep: {min, max}, prims, instances, templates, indexCells, listWords,
warnings: [..] }` — the box to request regions over. The ack's `<n>` is the
prim count. `tuner_server.py`: `POST /api/dungeon/preview` with JSON
`{mode: "isolate"|"place"|"map"|"clear", seed, shell?: <shell.json object>,
src?: <asset-relative path>, id?, at?: [x, z], yOffset?, yaw?}` → the
`info.json` object (the server writes `shell` to a temp file when given);
then the existing `/api/voxregion` (REGION) calls fetch voxels.

### 5.1 Kit page (piece designer)
Style + piece type list → slider panel (built from `SCHEMA`), presets, seed
reroll, 3D preview of the piece alone (rock shown as a cut-away block) with
sockets drawn as coloured frames. "Save as preset" writes to the kit's presets.

### 5.2 Dungeon page (the stitcher)
- 3D fly camera (WASD + mouse, like the World map 3D pane), plus orbit.
- **X-ray / cutaway**: terrain hidden or translucent; slice by y-range
  (level view); per-piece isolate.
- Place a piece by dragging from the palette onto an open socket: it snaps,
  rotates to fit, sockets link. Move/rotate (quarter turns) with a gizmo; links
  re-solve or report broken.
- Selected piece → its slider panel live (re-generate → recompile → voxserve
  refresh of only the touched regions).
- **Layout assist**: select an open socket → "Grow" (N pieces, style, branch
  factor, seed) → proposal shown ghosted → keep / reroll / undo.
- 2D graph overview (pieces as nodes, links as edges, levels as bands).
- Place props (palette), mobs (any mob def), spawners, lights, containers +
  loot table, keys/locks/levers/gates/plates/traps, with **signal wires** drawn
  between emitters and receivers. Mob schedule picker reuses `schedules.js`.
- **Validation panel** (always live): open sockets, overlapping pieces,
  surface/water breaches, unreachable pieces (waynode graph), a lock with no
  reachable key, dead signals, slope too steep to walk, prop inside rock.
- Undo/redo for everything (envui undo).
- **Playtest**: launches the game at the entrance (`--at` with the anchor), the
  same way the Play button does.

### 5.3 Placement (World map integration)
On the World map page: a `dungeons` layer shows each dungeon's footprint
(projected AABB per level), its anchor and its depth. Drag the anchor, set yaw
/ yOffset; "Open in Dungeons tab". The Dungeons tab's 3D view shows the real
terrain around the entrance (voxserve with the map) with the dungeon x-rayed
beneath it.

## 6. Verification (gates; one `--verify` per package at the end)

- `dungeon-format`: every `assets/dungeons/*` loads, `shell.bin` hash matches
  `dungeon.json`, all sockets of `entrance` resolve, no prim exceeds limits.
- `dungeon-gen`: on the harness map, a fixture dungeon (harness copy of a
  small multi-window layout) generates; CPU-side expectation of a few
  carved/filled cells matches GPU; determinism twice-run; ≤32 active chunks
  after settle standing in it; pool residency reported.
- `dungeon-entities`: spawner determinism, lock/key/lever/gate signal chain,
  trap emits ops (collapse floor turns to powder), loot roll determinism,
  patrol route walks waynodes across pieces.
- JS: `node scripts/test_dungeongen.mjs` — every piece type × presets
  generates, sockets valid, slider monotonicity spot checks (width slider
  widens the carve), compile is deterministic, layout assist reproducible.
- Browser: `scripts/check_dungeons.sh` (headless Chrome, like
  `check_worldview.sh`): tab opens, a piece previews (pixel readback), a snap
  link, an undo.

## 7. Packages and waves (Opus 5.5 worktree agents; orchestrator merges)

Agents commit on their own worktree branches, never main. Orchestrator merges,
rebaselines once at the end, updates DESIGN.md / EDITOR_GUIDE.md / tuner
ARCH_NODES, rebuilds the main exe.

**Wave 1 (parallel)**
- **D1 Engine shell + props + placement** (C++/WGSL): `dungeons[]` parse,
  `shell.bin` loader, dungeon table + sparse chunk index in `worldMap`,
  worldgen evaluation (carve/fill/skin/keep, shapes, noise, blend, scatter),
  props with any-y + carve code, cave/lava suppression, far-field coverage of
  surface pieces, SVED v3, voxserve `DUNGEON`, gates `dungeon-format` /
  `dungeon-gen`. Uses a HAND-WRITTEN `shell.json` fixture until D2 lands.
- **D2 JS generator library**: `dungeongen.js` core (transforms, sockets,
  taper, compile, validate, layout assist) + `kit_caves.js`, `kit_mines.js`,
  `kit_ruins.js` with every piece type in §4 + prop generators;
  `scripts/compile_dungeon.mjs`, `scripts/test_dungeongen.mjs`. Node-only; no
  engine needed.
- **D3 Gameplay entities**: spawner, lock/key, lever/plate/gate signals,
  traps via ops, loot tables, schedule actions guard/patrol/lair/ambush,
  dungeon ref-group loading with the local-frame transform; gate
  `dungeon-entities`.

**Wave 2**
- **D4 Tuner Dungeons tab**: Kit page, Dungeon stitcher, validation, layout
  assist UI, entity placement + signal wires, World map placement layer,
  playtest; `check_dungeons.sh`. Needs D1 (voxserve DUNGEON) + D2 merged.

**Wave 3 (parallel, after D4)**
- **D5/D6/D7 Example dungeons**, built with the tools (scripted through the same
  modules the tab uses; screenshots from the tab and `--shot` in game):
  - Caves — **The Verdant Teeth sea caves** (`teeth_caves`, -81000, 87500):
    karst; entrance above sea level; a grotto pool; one shaft drop.
  - Mines — **The Shardwell** (`shardwell`, 67685, -49628): adit into the
    crater wall, two levels, crystal ore stopes, a flooded sump, a collapse.
  - Ruins — **The Tarn Cairn** (`heather_cairn`, -56000, -33616): surface
    ruin, stairs down to a columned hall, crypts, a sanctum behind a locked
    gate with the key in a crypt, a collapse-floor trap.
  Each: ≥ 51.2 m extent on some axis, at least 12 pieces, lights, mobs +
  spawner + a guard/patrol schedule, containers with loot, and a validation
  panel with zero errors.

**Wave 4 — orchestrator**: merge, one rebaseline, docs, ARCH_NODES, memory.

## 8. Open risks

- `worldgen.wgsl` is ~8.7k lines and the far-cascade compile cliff is real:
  D1 keeps dungeon evaluation in its own functions, behind a header-word
  early-out, and measures pipeline compile time before/after.
- Concurrency: the 2026-10-12 ticket wave has worktree agents in worldgen/far
  code; D1 rebases at merge time, not continuously.
- Signals/locks are new interaction state: D3 keeps it in refs (persisted by
  the save system's entity split), never in voxels.
- Arbitrary size vs the site lattice: dungeons do NOT go through the 2D site
  lattice (`kSiteCellMax`); they have their own 3D chunk index.

## 9. D1 report (engine shell, 2026-10-12)

Landed on branch `worktree-agent-a3caddfa571ce1f41`: `src/sim/dungeon.*`
(format, resolution, placement, `PackDungeons`, the CPU twin),
`assets/shaders/worldgen_dungeon.wgsli` (appended to worldgen.wgsl by
`AssembleShaderSource` / `check_shaders.sh`), the `map.json dungeons[]` parse
and anchor in `worldmap.cpp`, SVED v3 in `worldedit.*`, voxserve `DUNGEON`
(§5.0) + `tuner_server.py POST /api/dungeon/preview`, `--dungeon-bake`, the
`_fixture` (written by `scripts/gen_dungeon_fixture.py`) on the harness map,
gates `dungeon-format` / `dungeon-gen`, DESIGN.md §9h.

**Pipeline compile, cold** (same exe, fresh CWD AND private shader / pipeline
caches per arm, `SANDVOX_SHADER_TIMING=1`, `__GL_SHADER_DISK_CACHE=0`,
exclusive locks; base assets = dd56aa1):

| worldgen entry | before | after | |
|---|---:|---:|---|
| `main` | 6.7 s | 9.6 s | +2.9 s (the full `dgCell`) |
| `list` | 7.3 s | 9.9 s | +2.6 s |
| `cols` | 52.1 s | 51.0 s | noise (only `dgColTop`) |
| `far` | 78.0 s | 71.2 s | noise (`dgCellCoarse`); SPIR-V 22,813 -> 26,511 instructions, ceiling raised 25k -> 28k |
| `farpatch` / `fardown` / `farmap` | 65 / 69 / 72 s | 58 / 59 / 62 s | noise (only the cave keep-out) |

The first version cost `main`/`list` +18 s each: `dgScatter` called the carve
field from ~12 sites and the driver inlines every one. Rolling the probes into
ONE loop (one inlined copy of the carve field) took it to +2.9 s. Keep
`dgCarveAt` / `dgPrimD` at one call site per kernel path.

**Residency** (the window round the fixture's chamber, after 120 settle ticks):
5,133 pages with the dungeon vs 4,645 with the evaluator off: +488 pages
(+10.5 %), 13.9 % of the pool. **Rest**: 0 chunks awake after the settle.
**Twin**: 671,744 fixture cells and 827,392 cells of D2's `_d2_sample` (328
prims, 36 props) bit-identical to the CPU twin (the only differences are the
45 wet-bank stain soaks round the fixture's puddles, a separate post-pass).
The determinism hash does not move (`ef7d9a06`): the fixture is 2 km from the
harness window and a map with no dungeon packs no block.

**Known limitations** (for D2 / D4 and the orchestrator):
- Floor / ceiling scatter searches the CARVE FIELD for its support, not the
  final voxels: a feature can stand on a fill the carve cut (fine) but also
  "on" open sky where a carve meets the surface. Keep stalactites off
  `surface: true` pieces.
- A puddle's containment is checked against the carve field; terrain air next
  to it (a surface piece) is not seen.
- The firm shell turns powder / liquid within 2 voxels of a carve to `firm`,
  but a tree whose trunk a carve cuts keeps its crown (the island detector may
  later drop it as debris). Entrances through forest want a clearing.
- Far field: shapes and props only (`dgCellCoarse`); underground pieces are
  not drawn from afar, and the far surface map (`farmap`) does not see a ruin
  above the ground (the cascades do).
- 64 prims / 32 prop instances per 64^3 index cell; elongated ellipses /
  cylinders / vaults inflate their index bounds by their axis ratio (<= 256).
- voxserve resolves the anchor at the environment's seed (kDefaultSeed),
  whatever the DUNGEON command's `<seed>`.
