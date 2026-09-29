# PLAN: world editor, first slice — the hamlet of Harrowby

Status: SLICE COMPLETE 2026-09-29 (P1-P8 on main, last 5e62edf). Plan of record, 2026-09-28. Orchestrated; packages P1–P8 below are the
agent prompts. Owner decisions (2026-09-28): rural medieval hamlet; branching
dialogue; doors are STATIC VOXELS WHEN CLOSED and a HINGED RIGID BODY WHILE
OPEN; packages merge to main as they land.

## 0. Goal, and what this is not

Build the smallest set of tools that lets a human hand-craft one place, and
then use them to hand-craft it: **one village of 3 houses, 5 NPCs with homes
and daily schedules, doors, chests and branching dialogue.** Generators make
scaffolding; a person (first the build agent under creative direction, then the
owner) makes the deliberate choices on top, with the same tools.

Not in this slice (the gap list in the 2026-09-28 analysis keeps them): quests
and journal, a script language, roads/road graph, town-layout generation,
factions matrix, economy, NPC LOD past `kMaxMobs = 16`, off-window NPC
simulation beyond "catch up on return", multi-layer plugin merging, arbitrary
(non-90°) structure yaw.

## 1. The owner's rule for every package: a HUMAN picks this up

> "Whatever you make should have a clean and easy human-usable interface for
> me to pick up, make edits and tweaks myself." — owner, 2026-09-28

Concretely, every package is only done when:

1. **Every authoring action has a UI a person can use without reading code**:
   the in-game editor (ImGui dev panels are fine for tools) or the tuner
   (browser). Player-facing UI (conversation box, chest window) is pixel art per
   `src/ui/theme.h` — no TTF, no rounding.
2. **Every authoring action is ALSO a named command** (see §3.3). The UI calls
   the command; undo records the command; an agent's build script is a list of
   commands. There is no action an agent can take that a human cannot click, and
   nothing an agent built that a human cannot select and change.
3. **Content is plain, diffable, hand-editable text** (JSON with stable key
   order, one object per line where lists get long) plus `.vox` for voxels. A
   person can fix a typo in a dialogue line in a text editor and press R.
4. **Hot reload**: content edits take effect without a rebuild and, where
   possible, without a restart (R reloads content; the editor re-applies).
5. **Errors name the file, the object id and the field**, and show in-game
   (dev panel warnings list), not only on stderr.
6. The package adds its section to **`docs/EDITOR_GUIDE.md`** — a
   task-oriented "how do I …" written for the owner, not an architecture note.

## 2. Architecture decisions (fixed for all packages)

### 2.1 References (P1 owns)

A **reference** is one authored, placed thing with a stable id. It is the unit
everything else names.

```jsonc
// assets/worldmap/<map>/refs/<group>.json   (group = a place; "harrowby.json")
{
  "group": "harrowby",
  "refs": [
    { "id": "harrowby/smithy",        "kind": "structure", "base": "harrowby_smithy",
      "pos": [x, y, z], "yaw": 90 },
    { "id": "harrowby/osric",          "kind": "npc", "base": "human",
      "pos": [x, y, z], "yaw": 180,
      "props": { "name": "Osric", "home": "harrowby/smithy", "bed": "harrowby/smithy/bed_0",
                 "schedule": "smith", "dialogue": "osric", "outfit": "...", "behavior": "villager" } },
    { "id": "harrowby/green_well",     "kind": "marker", "pos": [...], "props": { "tags": ["gather"] } }
  ]
}
```

- `id` is `group/name`, lowercase, unique map-wide, **never reused and never
  derived from load order**. Child refs a structure instance creates from its
  slots are `<instance id>/<slot name>` (e.g. `harrowby/smithy/door_front`).
- `pos` is world voxels (integers, the owning chunk derives from it); `yaw` is
  degrees, heading 0 = +Z (CLAUDE.md conventions). Structures accept only
  multiples of 90 in this slice.
- `kind` is an open string resolved at load by a registry (`RefKindRegistry`):
  each system registers the kinds it owns (`structure`, `npc`, `door`,
  `container`, `bed`, `marker`, `waynode`) with a validate + activate +
  deactivate + save-delta hook. Unknown kind = load warning, ref kept verbatim.
- `props` is a free object validated by the kind's hook. Unknown props are kept
  and round-tripped (never dropped on save).
- **Authored vs save.** The refs files are the authored truth and are read-only
  at play time. A save stores only **deltas keyed by ref id** in the region
  entity bucket of the ref's home region (`persist.h` `MakeEntityIO`): "door
  open", "chest contents", "npc dead / npc at pos". A ref with no delta is
  exactly as authored. Editing the authored file between saves must not corrupt
  a save: a delta for a vanished ref id is dropped with a warning.
- **Activation.** A ref becomes live when its region streams into the window
  and goes dormant when it leaves (same cadence as `MobParking`). Activation is
  deterministic (ordered by id) and ALL voxel writes go through the
  MutationQueue (rule 3).
- **Stable NPC identity.** A mob spawned from an `npc` ref carries the ref id
  (`Mob::refId`); saves and parking key it by ref id, never by `nextId_`. This
  closes `PLAN_save_system.md` S5b's open item for authored NPCs.

### 2.2 Structures (P2 authors the format, P4 places it)

A structure is a building blueprint:

```
assets/structures/<name>.vox          voxels, MagicaVoxel, local frame, Y-up per geometry.py
assets/structures/<name>.struct.json  { "name", "origin": [x,y,z] (local, the ground anchor),
                                        "slots": [ { "name": "door_front", "kind": "door",
                                                     "pos": [..local..], "yaw": 0, "props": {...} }, ... ],
                                        "generator": { "tool": "housegen", "params": {...}, "seed": n } | null }
```

- Slots are refs-in-waiting: instancing a structure creates child refs
  `<instance>/<slot>` with the slot's kind, local→world transformed.
- `generator` records where the scaffold came from. **Once a human edits the
  `.vox`, it is the truth**: regenerating writes a NEW file (`<name>_v2`) and
  never overwrites a hand-edited one (the editor warns and asks). Hand-editing
  sets `"handEdited": true`.
- A structure placed by a `structure` ref is **worldgen input** (the existing
  `stamp` site path, `wmStampCell`): it appears at any distance, is not saved,
  and regenerates identically. Player damage to it is an ordinary chunk edit in
  the save. Consequence the owner should know: editing a house moves the world
  hash (a notification, rule 1).
- Hand edits of a house live **in its structure asset**, not in the map's
  `.svedit` layer. That is the instance-relative edit problem solved the cheap
  way for this slice: each Harrowby house is a unique asset.

### 2.3 Editor commands (P5 owns the layer; P1–P7 register commands)

`src/editor/commands.h`: a command is `{ name, args(JSON) }`, applied by a
registered handler that returns its inverse (for undo). Examples:
`ref.place`, `ref.move`, `ref.set_prop`, `ref.delete`, `vox.box_fill`,
`vox.brush`, `vox.replace`, `vox.paste`, `struct.open`, `struct.save`,
`struct.generate`, `door.set_hinge`, `schedule.set_row`, `dialogue.set_line`.

- The in-game editor UI only ever calls commands.
- `--edit-script <file.jsonl>` applies a command list headlessly, then saves:
  this is how an agent builds, and the file it leaves behind is a readable
  record of what it did.
- Commands write the AUTHORED FILES (refs json, structure `.vox`/json) on the
  CPU; the live world is then updated by re-applying the affected
  structure/refs through the MutationQueue. The authored file is the truth, so
  undo is a data diff, not a GPU readback.

### 2.4 Doors (P6)

Closed: the door leaf is ordinary static voxels in the structure (it collides,
sleeps, burns, costs nothing idle). The `door` ref names the leaf's cell box
and hinge edge (local to the structure). **Open**: the leaf cells are removed
by CellOps and the same voxels become a rigid body on a Jolt hinge to the
world, motor-driven to the open angle. **Close**: motor drives to 0; when
within tolerance and at rest, the body is destroyed and the exact words are
written back by CellOps. Blocked close (a body in the box) waits and retries.
The body path is the existing debris/island body path; the write-back is the
existing settle-back path. Open/closed state persists as a ref delta. A door
that was burned or broken (leaf voxels no longer match) stops being a door and
says so.

### 2.5 Interaction verb (P1)

One "use" key, raycast from the crosshair to the nearest usable ref (door,
container, bed, npc) within reach, dispatching to the kind's `OnUse`. The HUD
prompt says what it will do ("Open door", "Talk to Osric", "Search chest") —
same style as E's existing corpse prompt. Pick a free key (check `main.cpp`
bindings; E and Q are hand-swap). Multiplayer: the use is a player INPUT
carried in `TickInput`, executed by `TickAuthority`, not a local side effect.

### 2.6 NPC residents (P7)

- **Schedules** `assets/schedules/<name>.json`: rows
  `{ "from": "21:00", "to": "06:00", "do": "sleep", "at": "bed" }` over the
  in-game clock (`DayPhaseForTick`). `at` is a role on the NPC ref (`home`,
  `bed`, `work`) or a ref id or a marker tag. Activities (new intents in
  `ai_behavior`, arbitrated like the existing ones so combat still interrupts):
  `sleep` (lie on bed), `work` (stand at anchor facing its yaw, work clip if
  one exists), `wander` (radius around anchor), `socialize` (go to a gather
  marker, face another NPC), `eat` (sit/stand at home), `goto`.
- **Navigation**: `waynode` refs with explicit `links`, placed through doors
  and along paths; long legs route over the waynode graph (Dijkstra), short
  legs use the existing local A* (`ai_nav`). NPCs open and close doors they path
  through via the same `OnUse` as the player.
- **Catch-up**: an NPC re-activated after dormancy is placed where its schedule
  says it would be now (at its anchor), not where it froze. Cheap, deterministic,
  and good enough until real off-window simulation.
- Behaviour profile `villager` in `assets/mobs/behaviors.json`: passive,
  faction `harrowby`, flees from hostiles, fights back if struck.

### 2.7 Dialogue (P3)

`assets/dialogue/<name>.json`: a graph of nodes
`{ "id", "speaker", "text", "choices": [ { "text", "goto", "if": [...], "do": [...] } ], "if", "do" }`
with an `entry` list chosen by conditions (first match). Conditions:
`flag`, `!flag`, `time` range, `activity` (NPC's current schedule row), `has`
item, `met` (player spoke to this NPC before). Actions: `set`/`clear` flag,
`give`/`take` item by name, `end`. Flags are a world-scoped string→int store,
saved in `world.sve`. A choice made is a player INPUT (TickInput), like use.
Player UI: pixel-art conversation panel (portrait slot optional), numbered
choices, keyboard and mouse. Authoring UI: tuner **Dialogue** page — node list +
graph view, edit text inline, add choice, jump to target, validate (dangling
`goto`, unreachable node, unknown flag written but never read) — plus hot
reload with R in game.

## 3. Packages, waves, dependencies

| Wave | Pkg | What | Needs | Rebuild? |
|---|---|---|---|---|
| 1 | P1 | References core, stable NPC ids, save deltas, use verb, ref inspector panel | — | C++ |
| 1 | P2 | Building materials + house generator (JS) + Structures page + bake | — | NO (JSON/JS only) |
| 1 | P3 | Dialogue: data, flags, conversation UI, tuner Dialogue page | — | C++ |
| 2 | P4 | Structure instances: `structure` refs → worldgen stamps, slots → child refs | P1, P2 | C++ |
| 2 | P6 | Doors (static↔hinged), containers (chests), beds | P1 | C++ |
| 3 | P5 | In-game editor mode: command layer, free cam, freeze, voxel tools on structures, ref gizmo, undo, `--edit-script` | P1, P4 | C++ |
| 3 | P7 | NPC residents: spawn from refs, schedules, activities, waynodes, doors, talk | P1, P3, P6 | C++ |
| 4 | P8 | Build Harrowby (content) + EDITOR_GUIDE walkthrough | all | NO (content) |

The orchestrator merges each package to main as it lands, resolves conflicts
(expect `main.cpp`, `session.cpp`, `selftest.cpp`, `baseline.json`), and
starts the next wave from the merged main.

## 4. Rules every package agent follows

- You are in a **git worktree** off main. Read `CLAUDE.md` and the relevant
  `DESIGN.md` sections before touching a system. Commit your work to your
  worktree branch; do NOT merge to main — the orchestrator does.
- **Build once** (`bash scripts/build.sh` in your worktree; sccache shares
  objects). JSON/JS/WGSL-only work never builds: use the main checkout's exe
  with `SANDVOX_ASSET_DIR=<worktree>/assets` (CLAUDE.md "What needs a rebuild").
- Every exe launch goes through `bash scripts/run.sh` (absolute path to the
  main checkout's copy if yours predates it). `export SANDVOX_NO_CRASH_DIALOG=1`.
- **Verification is ONE `--verify <your gates>` boot at the end**, your new
  gate(s) plus at most two that cover code you touched. Never `--suite
  acceptance`, never a bare `--selftest`, never a smoke unless you changed a
  render path (owner directive). A hash that moves because you added content is
  a notification: `--selftest --gate determinism --rebaseline`, one command.
- New gates tick via `support::RunTicks` / `TickCursor` (`src/test/tickrig.h`),
  must pass under `--gate <name>` alone, and keep thresholds in
  `tests/baseline.json`.
- Determinism: integer/fixed math on anything that writes the world; RNG only
  via the counter hash; all voxel writes through the MutationQueue; player
  actions (use, dialogue choice) as `TickInput`.
- Update `DESIGN.md` (a new section for your system), `assets/tuner.html`
  `ARCH_NODES` (+ Development Status lists), and `docs/EDITOR_GUIDE.md` (the
  human "how do I" section, §1.6) in your commit.
- Claim shared files on the board (`bash scripts/board.sh claim ...`) at start
  and `done` at the end.
- Final report (≤ 400 words): what landed, file list, gates + results, what a
  human does to use it (the UI path), known gaps.

## 5. Package prompts

### P1 — References core

Implement §2.1 and §2.5.
- `src/world/refs.{h,cpp}`: parse / validate / write refs group files (stable
  key order, props round-trip), `RefKindRegistry`, `RefStore` (all loaded refs
  by id, per-region index, activation/deactivation as regions enter/leave the
  window, ordered by id).
- Map ownership: the map's refs live in `assets/worldmap/<map>/refs/*.json`;
  `worldmap::` loads them with the map. The harness map gets a small fixture
  group for gates.
- Save deltas keyed by ref id through `persist.h` `MakeEntityIO`; dropped-id
  warning.
- `Mob::refId` (string or interned id) set when a mob is spawned by a ref;
  `MobParking`/save records key authored NPCs by it. A temporary `npc` kind
  handler that spawns the named mob def at the ref's pose is enough here (P7
  replaces the brain).
- Use verb (§2.5) with HUD prompt, `OnUse` dispatch, `TickInput` plumbing.
- **Human UI**: F1 → new **World → References** page: list refs in the window
  grouped by group (filter by kind, text search), click to fly the camera to
  it, inspector showing kind/base/pos/yaw/props with edit fields that write back
  to the group file, warnings list. Register `ref.place / ref.move /
  ref.set_prop / ref.delete` as plain functions P5 will wrap as commands.
- Gates: `refs-roundtrip` (parse→write→parse byte-identical incl. unknown
  props), `refs-activate` (fixture refs activate/deactivate with the window,
  deterministic order), `refs-npc-identity` (NPC from ref survives save/load
  and parking with the same ref id; delta for missing id is dropped with a
  warning).

### P2 — Building materials, house generator, Structures page

No C++. Follow the trees pipeline (`assets/editor/treegen.js`,
`assets/editor/trees.js`, `scripts/bake_trees.mjs`) as the model.
- Materials in `assets/materials/materials.json` (static solids, sensible
  density/hardness/flammability tags, colour palettes that read as rural
  medieval): `plank` (sawn boards), `timber` (dark oak beams), `thatch`
  (flammable, straw), `daub` (wattle-and-daub, pale ochre plaster), `cobble`
  (rubble stone wall), `flagstone` (floor), `roof_tile` (fired clay),
  `door_wood` (flammable, distinct so a door leaf reads), `straw_bed`. Check
  existing reactions for `wood` burning and give the flammable ones the same
  behaviour by tag, not new rules.
- `assets/editor/housegen.js` — pure, Node-runnable, deterministic from
  `(params, seed)`: footprint w×d (m), storeys, wall style (timber-frame+daub /
  cobble / plank), roof (thatch gable / tile gable / hip), roof pitch, overhang,
  chimney on/off + side, door count + walls, window count, floor material,
  interior partitions (0–2), loft on/off. Output: voxel grid (materials by
  name) + slots (`door_*` with hinge edge + leaf box, `bed_*`, `chest_*`,
  `hearth`, `work_*`, `waynode_*` at inside-door / outside-door / room centres
  linked). Voxel = 0.1 m; a door is ~0.9×2.0 m; walls ~0.3 m. Windows are
  openings with `glass` or open shutters (choose; document).
- Writes `assets/structures/<name>.vox` + `<name>.struct.json` per §2.2
  (`generator` block filled, `handEdited: false`). `scripts/bake_structure.mjs
  <name> [params.json]`; never overwrites a `handEdited: true` asset.
- **Human UI**: Environment tab → new **Structures** page (sidebar entry like
  Trees): parameter sliders with live 3D preview (reuse the Trees page viewer),
  slot markers drawn and labelled, seed reroll, "Save as structure…" (name
  prompt), list of existing structures with `handEdited` badge. Tuner server
  endpoint for saving if the existing file-save endpoint is not enough.
- Gate: `scripts/test_housegen.mjs` (determinism: same params+seed →
  byte-identical output; every door slot's leaf box is all `door_wood`; every
  bed slot has headroom; slots inside footprint) + `scripts/check_environment.sh`
  still green. Produce 3 sample structures in `assets/structures/samples/` so P4
  has fixtures.

### P3 — Dialogue

Implement §2.7.
- `src/game/dialogue.{h,cpp}`: load/validate `assets/dialogue/*.json`, runtime
  conversation state per player (`PlayerSession`), condition/action evaluation,
  flags store (world-scoped, saved in `world.sve` via `persist.h`), `met` set.
  Entry point `dialogue::Begin(session, speakerId, dialogueName)`; P7 and the
  use verb call it. Until P7 lands, a dev hook "Talk (dialogue …)" on the Spawn
  page / mob inspector starts a conversation with any mob.
- Choices and begin/end are `TickInput` events applied in `TickAuthority`.
  While talking: the player's movement input is suppressed, the NPC faces the
  player (a hook P7 wires into the arbiter; expose `IsInConversation(mob)`).
- **Player UI**: pixel-art conversation panel (`src/ui/theme.h` toolkit):
  speaker name header bar, wrapped text in the pixel font, numbered choices
  (1–9 keys + mouse), "[continue]" when a node has no choices, Esc to leave
  when allowed. Verify with a `--shot`-style capture path (add
  `--shot-dialogue <file>` if none fits).
- **Authoring UI**: tuner **Dialogue** tab/page: file list, node list, a graph
  view (nodes as boxes, choices as edges), inline edit of text/conditions/
  actions with dropdowns for known flags/items/activities, "add node", "add
  choice", validation panel (§2.7 checks), save. R in game hot-reloads.
- Content: one sample `assets/dialogue/sample_stranger.json` exercising every
  condition and action.
- Gates: `dialogue-graph` (load + validate sample, walk scripted choices,
  flags/items end states exact), `dialogue-save` (flags + met survive save/load).

### P4 — Structure instances

Implement the placement half of §2.2.
- `structure` ref kind: resolves `base` to `assets/structures/<base>.*`,
  registers a worldgen stamp site (existing stamp machinery, 90° yaw, ground
  pad levelling to the ref's `pos.y` — the ref's y IS the floor height the
  author chose, not a recomputed centre height).
- Raise `kSiteCellMax` (4 → 32 or a per-cell list) so a village fits in one map
  cell; keep `--mapcheck` honest.
- Slot instancing: child refs `<instance>/<slot>` created at load, transformed
  local→world with the yaw, registered with their kinds (unknown kinds kept
  inert until P6/P7 register them).
- Hot re-apply: `structure.reload(name)` re-stamps every instance of a changed
  asset in the live window (P5's save calls it); the far field refreshes.
- **Human UI**: References page gets "place structure here" (picker of
  `assets/structures/`), yaw buttons (±90), a footprint preview box in the world
  before confirming, and "open in editor" (hands off to P5 once it exists).
- Gates: `structure-stamp` (a sample structure placed by a ref appears exactly,
  rotated ×4, pad levelled, child refs at the right world cells, reproducible),
  `structure-reload` (edit asset → re-apply → cells match).

### P5 — In-game editor mode

Implement §2.3 and the editor proper. Largest package; keep it to what the
village needs, but make every piece human-usable.
- Command layer `src/editor/commands.{h,cpp}`: registry, apply, inverse, undo
  /redo stack (per editing session, 256 deep), JSONL journal of the session,
  `--edit-script <file>` headless runner. Wrap P1/P4 functions as commands.
- **Editor mode** toggle (a key, e.g. F8 — check bindings): detached free
  camera (fly, scroll speed, focus-on-selection), sim freeze for the edited
  area (pause is acceptable for this slice; say which), crosshair picking of
  voxels AND refs (GPU pick + ref bounding boxes).
- **Structure editing**: "open" a placed structure instance → edits within its
  box write to the structure asset in its LOCAL frame (so rotated instances
  edit correctly), mark `handEdited: true`, save re-applies via P4's reload.
  Voxel tools: single voxel place/erase, brush (sphere/cube, size), box select
  with fill / hollow / replace / clear, copy / paste with rotate 90° and mirror,
  eyedropper, material picker (reuse `MaterialPicker`), line tool for beams.
  Port behaviour from `assets/worldview.js` where it exists.
- **Ref editing**: select, translate gizmo (axis drag with voxel snapping) and
  yaw ring (15° steps; structures 90°), place from a palette (npc / door /
  container / bed / marker / waynode / structure), delete, duplicate, props
  inspector (reuse P1's), waynode link drawing (click node → click node), and
  debug draw of all refs in editor mode (icons + labels + waynode edges + door
  hinge arcs).
- Status bar: current tool, selection size, unsaved changes marker, undo depth;
  Ctrl+S saves, Ctrl+Z/Y undo/redo; exiting with unsaved changes prompts.
- Gates: `editor-commands` (script of every command applies; undo all → files
  byte-identical to start; redo all → identical to applied), `editor-struct-edit`
  (edit a rotated instance, save, reload: asset changed in local frame, world
  cells match).

### P6 — Doors, containers, beds

Implement §2.4 plus:
- `container` kind: a `Bag` owned by the ref, initial contents from
  `props.items` (names + counts) or a loot table name, opened by use, UI reuses
  `inventory_ui`'s loot panel (take one / take all / put). Contents persist as a
  ref delta. The chest itself is voxels in the structure.
- `bed` kind: an anchor (pos + yaw + lie pose target) P7 uses; player use =
  "Rest" (skip-time is out of scope; a no-op message is fine).
- Door `props`: `hinge` (left/right), `openAngle` (default 95°), `locked`
  (bool; locked doors say so, no keys yet), `autoClose` seconds (optional).
- **Human UI**: References inspector gains door fields with a live preview of
  the swing arc, a "test open/close" button, and container contents editing
  (add item by name with a searchable list, count).
- Gates: `door-cycle` (open → body exists, cells empty; close → cells
  byte-identical to before, body gone; blocked close waits; open state survives
  save/load), `container-persist` (take/put, save/load, contents exact).

### P7 — NPC residents

Implement §2.6.
- `npc` kind (replaces P1's temporary handler): spawns the mob def with outfit
  / weapon props, `villager` behaviour, stable ref id, `home` / `bed` / `work`
  roles resolved to refs.
- Schedules, activities as arbiter intents, waynode routing, door use,
  catch-up placement on activation, conversation hook (`IsInConversation` →
  face player, pause schedule), use verb on an NPC → `dialogue::Begin` with the
  ref's `dialogue` prop.
- **Human UI**: NPC inspector (References page, npc ref selected): current
  activity + next row + route drawn in the world, schedule table editor (rows
  with time pickers, activity dropdown, anchor picker from refs), "jump clock
  to …" buttons for testing. Tuner: `assets/schedules/` editable via the
  generic JSON editor at minimum (add a schema row if simple).
- Gates: `npc-schedule` (fixture NPC with a 4-row schedule reaches each anchor
  within a bounded tick count across a simulated day, passing through a door
  that opens and closes; deterministic twice-run), `npc-catchup` (dormant
  → reactivated at the scheduled anchor).

### P8 — Build Harrowby

The creative package. Uses only the tools (commands / `--edit-script`, the
generator, the tuner pages); any tool gap found is reported, and fixed in the
tool, not worked around in content. The orchestrator gives the creative brief
(§6) and reviews screenshots.
- Generate 3 scaffolds with housegen, then hand-edit each (edit scripts kept in
  `assets/worldmap/default/scripts/harrowby_*.jsonl` so the owner can read what
  was done): furniture, hearths, interior clutter, beds, chests with contents,
  unique touches per §6.
- Place the structures, NPCs, waynodes, markers near the default map's spawn,
  sculpt/flatten the ground via the map page's sculpt layer if needed, paint
  paths with materials.
- Write the 5 dialogue files and 5 schedules (§6), wire flags between them.
- `--shot` captures: village overview, each interior, a conversation.
- `docs/EDITOR_GUIDE.md`: "Building a village" walkthrough that a person can
  follow to add a 4th house themselves.

## 6. Creative brief: Harrowby (orchestrator's choices)

A hamlet of three households at a stream-side clearing, a day's walk from
anywhere that matters. Low fantasy, grounded, a little wry. Late summer.

- **The Cotter's longhouse** — timber-frame and daub, deep thatch gable
  roof, byre end with a manger (no livestock yet), central hearth, one shared
  sleeping loft. **Edric Cotter** (farmer, 40s, practical, worried about the
  harvest and the things seen at the treeline at night) and **Maud Cotter**
  (his wife, sharp-tongued, runs the household, keeps the village's gossip and
  its only book — a herbal).
- **The smithy** — cobble walls, tile roof, open-fronted forge bay with anvil
  and quench trough, a small back room with one bed. **Osric** (smith, 50s,
  taciturn, ex-soldier; his work anchor is the anvil). Keeps a locked chest.
- **The alewife's cottage** — plank and daub, thatch hip roof, a front room
  with a trestle table and barrels (the hamlet's alehouse in all but name),
  two beds behind a partition. **Agnes** (widow, brewer, 60s, warm, nosy, the
  one who talks to strangers) and her grandson **Wat** (16, idle, wants to be
  apprenticed to Osric, who keeps saying no).
- The green: a well marker (`gather`), a bench, a path of packed dirt/gravel
  linking the three doors and the well.
- Schedules: Edric works the field edge by day; Maud home/well; Osric at the
  anvil 07–18, alehouse evenings; Agnes home all day (her cottage is the
  gathering place 18–22); Wat wanders, turns up at the smithy uninvited, sleeps
  late. Everyone sleeps 22–06 (Wat 23–08).
- Dialogue threads (flags only, no quest system): Wat asks the player to put in
  a word with Osric → Osric's reply depends on whether you did; Edric and Maud
  disagree about what's at the treeline; Agnes knows everyone's business and
  comments on what you've talked about with the others (`met` + flags).
