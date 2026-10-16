# Running water that sleeps (2026-10-16)

Owner ask, verbatim in spirit: *rivers, the ocean and large bodies of water
have essentially infinite volume; running paths of voxels that do the same
thing over and over should fall asleep while still looking like they push water
down the path; water should run a little vertically, even over waterfalls,
without tanking performance.*

Owner decisions (asked before starting): waterfalls are a **held voxel sheet**
(real water voxels, swimmable, reacting); persistent player-made streams
**auto-convert to held** (phase 2); river bake becomes **sloped + rapids +
falls**; work **lands on main when green**.

## The problem, precisely

A reach-1 CA cannot let a falling surface rest. Every step of a slope is a
lateral or diagonal move `stepLiquid` will make; feed the top and drain the
bottom (the reservoir already does both) and the river is a conveyor that never
sleeps — CLAUDE.md rule 2. That is why the old bake made rivers a STAIRCASE OF
STILL POOLS (flat reaches, weirs, "waterfalls drawn as a step"), and why
`PLAN_water_master.md` §5 named the conveyor a non-goal.

Simulating the motion is unaffordable. Faking it with a separate non-voxel
river breaks everything that reads voxels (swimming, fire, buoyancy, stains,
digging). The answer here is a third thing: **the voxels are real and at rest,
and the CA is told that a river at its surface is already where it is going.**

## Phase 1 — HELD WATER (landed with this document)

### 1. The surface in eighths (the still table)

`worldgen.wgsl cols` writes one record per window column (binding 58): the
authored surface of a map lake / river / the sea. The surface is now **T8, in
eighths**: full cells to `(T8 - 1) >> 3`, the top cell holding the rest.
Lakes and the sea are whole-voxel (`8 level + 8`); rivers carry a ramp.

### 2. The held rule (`sim_step.wgsl`, HELD WATER)

> A cell **at rest** in a held column — `8 y + f <= T8(column)` — never sends
> liquid sideways or diagonally into a cell of **another held column** (same
> liquid) that is **at or over** that column's own target.

Held water does not pour into held water that is already full. Nothing else
changes:

* a held column **under** its target (a crater, a scoop, a gap in a curtain)
  takes water from its held neighbours like any water; the neighbours that gave
  it are then under *their* targets and the reservoir refills them;
* a held cell beside a column that is **not held** (a dug bank) spills into it
  like any water — a **breach**, fed forever;
* water **over** a target (a bucket, a splash, a breach's stream coming back)
  is not at rest; it flows as water and the reservoir takes it back.

The reservoir (stage 5) now works in eighths, and its **deficit** branches
(TAKE, GROW) fire on every visit and fill the cell to its target in one write.
The **excess** branch (GIVE) keeps its roll (`sim.stillWaterRelax`/1024, one
eighth), so a splash still runs out before it is gone. `stillWaterRelax = 0`
switches the whole feature off (it is `lake-settle`'s control arm).

Rule 1: pure reads of reach-1 cells and the still table (a function of seed,
map and column). Rule 2: the rule only ever *refuses* a move; an undisturbed
held river has no move left and sleeps.

`canFlowAnywhere` and `bridgeLevel` make the same test in the same places
(`heldRest`, `heldBlocks`, `heldBlocksAt`) — the mirror must agree or a chunk
pins awake or sleeps with work left.

### 3. The bake (`worldwater.cpp` step 8, `riverbake.js` the port)

The staircase S (running min, `stepVox`) is unchanged. Then every step that
drops at most `kRiverWaterfall` (20 voxels, 2 m) is **soft**: the surface ramps
down to it linearly in arc, in eighths, over at most `kRiverRampVox` (48) of
arc per voxel of drop, never before the previous step, never inside a source
lake's lead. A drop over 2 m stays **hard**: a waterfall. Reaches are now runs
between hard steps; pieces split on a 2-eighth surface deviation as well.

Piece words: `kS_RivSurf` = T8 at end A; `kS_RivLipY` = lip Y (i16) | fall A→B
in eighths (u16).

### 4. `waterApply` (GPU `worldgen.wgsl`, CPU `world.cpp`, per voxel equal)

* channel water = the lowest T8 claiming the column, top cell part-full;
* dams between reaches are gone except at a waterfall, which is two strips past
  its cut: the **CREST** (`RIVER_LIP` voxels: a sill one cell under the upstream
  surface, that water over it) and the **CURTAIN** (`RIVER_CURTAIN` = 2: the
  upstream surface's water standing on the lower reach's floor — a held sheet
  of water against the cliff);
* banks rise to hold any crest or curtain within `2 RIVER_LIP`.

The top cell's missing eighths travel as `LandCol/Col.fluidGap` (zero-init =
full; colCache `CCW_WP` bits 25..27) into `genCellIn`.

### 5. The look (`raymarch.wgsl`)

* `fallingSheet`: every **side face** of water is drawn as falling whitewater —
  vertical streaks scrolling down at 3.2 m/s. A held curtain, a rapid's step
  and the CA's own spill all read as water going down.
* `fallCueAt`: a held curtain (liquid over liquid, air in front, liquid along
  it) throws the waterfall mist and spray, its foot is the plunge.
* the top surface keeps the flow grid's streaks, boils and foam lines.

### 6. Gate: `rivers`

B (pieces on the profile), D (CPU == GPU including the top cell's eighths),
E (contained: air beside water only over a held column below it; the falls'
curtain faces counted), G (sleep), and new **R (running)**: a crater scooped
out of main's surface refills; a trench cut through main's bank fills from the
river. Thresholds in `tests/baseline.json` (`rivers.*`).

## Phase 2 — persistent streams become held (design, not landed)

A breach stream that pools sleeps by itself. One that runs from a held body to
another held body (or the same river downstream) is fed and drained forever:
the conveyor again, now player-made. Phase 2 adopts such a stream into a
**dynamic held layer** once it is steady, and releases it when its source is
cut. Sketch:

* a GPU per-window-column table beside the still table, `heldDyn` (tag, T8 |
  mat, hop | age), read by `stillLevel8` as a fallback when the authored record
  is empty — so the held rule, the reservoir and the renderer need no change;
* an adoption pass over awake chunks' water columns: a column whose surface has
  stayed within one voxel for `sim.streamAdoptTicks` and that has a held
  neighbour upstream (authored, or dynamic with a smaller hop count, T8 >= its
  own) is adopted at its current surface with hop = 1 + that neighbour's;
* release: a dynamic column whose support is gone (no water-connected held
  neighbour with smaller hop and T8 >= own) counts its hop up each tick and
  expires past `kStreamHopMax`; a plugged breach therefore drains its stream
  from the top down, as ordinary water that then pools and sleeps;
* not saved (derived: a reloaded stream runs as CA water and re-adopts), not
  hashed directly (it is a function of the tick stream, decided on the GPU in
  tick order — deterministic like any other kernel).

Cost: one table read per held-candidate cell, already paid by phase 1's rule,
plus an O(awake water columns) pass.
