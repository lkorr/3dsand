# The sandvox tuner & voxel editor — a working guide

This is the hands-on tutorial for the tuner app, with most of its weight on
the **Models** tab (the 3D voxel editor). The in-app `[?]` button on that tab
shows a condensed version of this.

---

## 1. Starting the app

```bash
./sandvox_tuner.exe                 # desktop app, from the project root
python scripts/tuner_server.py      # same UI in your browser instead
```

The tuner edits this checkout in place. The **Build** button runs the CMake
build; **Play** launches `sandvox.exe`. In-game, `R` hot-reloads materials and
reactions, `F5` hot-reloads tuning — so the loop for most edits is: change a
value, alt-tab, press a key.

### The five tabs

| Tab | What it edits | Reaches the game via |
|---|---|---|
| Overview | load/save of the three JSONs, validation | — |
| Materials | `materials.json` — colours, densities, tags, micro blocks | `R` in game |
| Reactions | `reactions.json` — tag-driven interactions | `R` in game |
| Tuning | `tuning.json` — 160+ look/feel/sim knobs | `F5` in game |
| **Models** | `.vox` art + mob sidecar JSONs | restart / respawn the mob |
| **Dialogue** | `assets/dialogue/*.json` — conversations (§9) | `R` in game |

Ctrl+S saves whichever tab you're on (on Models it saves the model, not the
JSONs).

---

## 2. The Models tab, top to bottom

```
[ATTACH] [Attach|Erase|Paint] [Voxel|Box|Face|Select] [Mirror X]   [Undo][Redo][?]
[open ▾][↻][new… ▾][Save][Save as…]        ■ material name     status line
anatomy [Peel −] 0 [Peel +] [Unpeel]  [Fill layer] [Apply recipe]
[palette swatches]
┌────────────────────────────────┐ ┌──────────────┐
│                                │ │ Models        │
│         3D viewport            │ │ Selection     │
│                                │ │ Limbs         │
│                                │ │ IK chains     │
│                                │ │ Gait preview  │
└────────────────────────────────┘ └──────────────┘
[flipbook tags · frames strip · play/onion]
[clips · scrubber · key lanes]
```

### Camera

- **Right-drag** orbits, **middle-drag** pans, **wheel** zooms.
- The **left button never moves the camera** — it always edits. This is
  deliberate: an unnoticed orbit mid-stroke is how models get wrecked.

### Modes — what a click does

| Key | Mode | Effect |
|---|---|---|
| `T` | **Attach** | grows a voxel outward from the face you click |
| `R` | **Erase** | deletes the voxel you click |
| `E` | **Paint** | recolours the voxel you click |

The loud badge at the top-left always shows the current mode. Watch it.

### Brushes — what a click covers

| Key | Brush | Effect |
|---|---|---|
| `B` | **Voxel** | one cell (or a sphere — see brush size); drag to stroke |
| `G` | **Box** | drag a rectangle along the clicked face, fill/erase/paint it |
| `F` | **Face** | flood across the connected same-material face (stops at corners) |
| `V` | **Select** | drag a box selection — feeds "Make Part" / "Split to model" |
| `N` | **Noise** | scatter the active material over exposed surface voxels |

**Brush size** (toolbar slider, 1–6) makes the voxel and noise brushes
spherical. Big brushes stay mode-safe: paint/erase/noise only touch existing
voxels, attach only fills empty cells — a fat paint brush never conjures a
ball out of thin air.

**Noise** is how you rough up a surface: it repaints a random fraction
(the density slider) of the surface voxels under the brush with the active
material. Voxels store a material ID — not a free colour — so shading noise
means *mixing materials*, which is exactly how the engine itself varies
surfaces. Typical use: duplicate a material on the Materials tab, darken it a
step with the colour wheel, and noise it over the base coat.

**Whole mode** (`W`) treats the assembled prefab as one canvas: picking and
brushes cross model boundaries, and every write lands in the model that owns
that cell. Perfect for painting or noising a finished mob without limb-by-limb
bookkeeping. Two rules: attach can't grow outside the union of the existing
model boxes, and the select brush needs whole mode off. Undo works across
models either way — the log follows the voxels, not the active model.

Everything else: **Alt-click** eyedrops the material under the cursor in any
mode, `1`–`9` picks the first nine materials, `M` toggles X mirroring (the
orange plane), `Ctrl+Z`/`Ctrl+Y` undo/redo (one entry per stroke, however big),
`Esc` clears the selection.

### The palette

Swatches come straight from `materials.json` — array position i is engine
material ID i+1, and the `.vox` palette index equals the material ID. That is
why the palette can't be edited here: paint with the material you want the
*engine* to simulate, and its colour follows. If the palette is empty, load
`materials.json` on the Overview tab first.

### The colour wheel

Click the material chip (or name) next to the palette to open the wheel. It
edits the **active material's** `colors[]` — its shade variants — because a
voxel stores a material ID, and colour lives on the material. Drag the wheel
for hue/saturation, the slider for brightness, or type a hex; the `+` swatch
adds a variant (the engine hash-picks between variants per voxel, which is
where baked-in surface variation comes from; the viewport tints by variant 0).
Edits go into `materials.json` through the tuner's normal dirty/save flow and
hot-reload in-game with `R`.

### Files

The `open` dropdown lists every `.vox` under `assets/`. `new…` presets create
fresh boxes (8³…64³, micro bricks, custom up to 128³). Save paths are relative
to `assets/`:

- `models/` — plain art and props
- `mobs/` — rigged creatures (`.vox` + `.json` sidecar pair)
- `microvox/` — micro bricks referenced by materials
- `prefabs/` — worldgen prefabs

Every save round-trips the file through the reader and diffs it voxel-for-voxel
before touching disk; a failed round-trip aborts the save loudly. Multi-model
files also save a MagicaVoxel scene graph so limb names and placements survive
(and the file still opens in MagicaVoxel).

---

## 3. Making a model, start to finish

1. `new…` → 16³. You get an empty box with a ground grid; click the floor to
   place the first voxel.
2. Pick a material in the palette. Attach `T` + Box `G` roughs in mass fast:
   click a face, drag the rectangle.
3. Turn on `M` mirror for anything symmetric — every edit reflects across X,
   including erases.
4. Paint `E` + Face `F` recolours whole surfaces at once.
5. `Save` → `models/thing.vox`. Done — palette index == material ID, so the
   engine can drop it in as-is.

---

## 4. Making a mob

A mob is one `.vox` with **one model per limb**, plus a JSON sidecar that
describes the rig. The editor writes both.

### 4.1 Split the sculpt into limbs

1. Sculpt the whole creature as one model (§3).
2. Select brush `V`, drag a box around a limb (leg, head, tail…).
3. Side panel → **Split to model**, name it (`legU.FL`, `head`…). The voxels
   move into a new named model; the prefab keeps its shape on screen.
4. Repeat until everything is a named part. The Models list in the side panel
   shows each one; click to edit it (others dim for context), `⧉` duplicates,
   `✕` deletes.

Names matter: the engine resolves parents, chains, clip tracks and flipbook
parts **by model name**. The editor validates duplicates and dangling names
inline.

### 4.2 Rig it (Limbs section)

1. **sync** creates a limb entry for every model.
2. Set **root** (usually the torso). Everything else hangs off it.
3. Per limb: **parent**, **joint** (`ball`/`hinge`/`fixed`), **hp**,
   **severable**/**vital**, **tag** (`leg`, `spine`, `head` — chains and gait
   query by tag).
4. **Anchor** — the joint pivot, in prefab coordinates. Select the limb and
   drag the **orange ball** into the socket (it snaps to half voxels — that's
   the engine's authoring granularity). The blue line is the swing **axis**.
   Bad anchors are the #1 cause of wrong-looking animation; the gizmo draws on
   top of everything for exactly that reason.

### 4.3 Legs: IK chains and gait

- Select the **lower** bone of a leg, press **+ chain**: that builds the
  two-bone chain (upper → lower, effector = lower) the foot-planting runtime
  needs. Without a chain the limb falls back to the sine swing.
- Add a **gait block**. The parameters mirror `anim.h` exactly (cadence,
  strideBias, stepHeight, rideHeight, bob/sway/roll…), and the preview runs
  the *engine's transcribed code* (`editor/anim.js`), not an imitation.
- **Leg groups** are the gait state machine: exactly one group may swing at a
  time. Two singleton groups = biped alternation; diagonal pairs = a trot.
  Members are limb names, comma-separated.
- **K** (or the walk button) makes it walk in place. The readout shows the
  phase and each foot's state (▲ swinging ▼ planted). The preview-speed
  slider sweeps the same speed factor the engine derives from velocity.

### 4.4 What is under the skin (the anatomy row)

A limb is solid, and the engine keeps every enclosed voxel — a cut, a burn or a
severed joint shows whatever is inside. The **anatomy** toolbar row is how you
author that inside without digging:

- **Peel + / Peel −** (`PageDown` / `PageUp`) hide the outermost layer of
  *every* limb, one depth layer at a time. Depth is measured from the surface
  over the whole assembled creature, so the face where a thigh meets the hips
  counts as interior and gets bone like the rest. Peeled voxels are not drawn
  and not picked: the brushes, the noise brush and the eyedropper all work on
  the layer you can see. The status line says what is exposed
  (`PEEL 2/7 exposed 1240: muscle 91% blood 6% bone 3%`).
- **Fill layer** paints the whole exposed layer of the active model (every
  visible model in Whole mode) with the active material. Art colour is
  cleared unless a brush colour is set — an interior voxel wearing the skin's
  paint would show the paint, not the material, in-game. One `Ctrl+Z`.
- **Apply recipe** rewrites the entire interior by depth from the sidecar's
  `"anatomy"` block (see `assets/mobs/human.json`: skin kept, flesh, muscle
  speckled with blood, bone core; a bone skull around a flesh brain for the
  head; `garments` name surface materials that are clothes so skin goes under
  them). A sidecar with no recipe gets the stock human one written in. The
  painted surface is never touched. One `Ctrl+Z`.

The loop the row is built for: peel once, look, change the material of what
shows (Fill layer, or a brush), peel again, repeat, **Unpeel**, Save. The same
recipe bakes from the command line — `node scripts/anatomize_mob.mjs <mob>`
— and `node scripts/test_anatomy.mjs` asserts the committed human matches
its own recipe. A creature made of steel under a painted shell is a different
recipe in its sidecar, not a different tool.

### 4.5 Save

Save to `mobs/name.vox`; the sidecar `mobs/name.json` is written next to it
with all rig data. Fields this editor doesn't know about are preserved
verbatim, so hand-edited extras survive. Spawn the mob in-game to see it.

---

## 5. Animation

The timeline has **two lanes**, picked with the tabs above it:

- **animation** — flipbook frames and keyframed clips (§5.1–5.2). This is the
  pose data in *this rig's own sidecar*.
- **attacks** — the stroke programs in `assets/mobs/attack_styles.json` (§5.3),
  previewed on this rig through the real melee driver.

Four layers compose in the preview, same as the engine, in this order:

1. **Clips** — the locomotion family plus anything you have open. **This is
   where the arm swing comes from.**
2. **Gait** — procedural foot placement from the rig + gait block (§4.3). It
   places *feet*; it does not move arms. You don't keyframe walking.
3. **The weapon arm** — a live stroke takes over the arm the weapon socket
   names (§5.3).
4. **Flipbooks** — per-part model swapping (blinking faces, mouth shapes,
   crumbling states) — the voxel equivalent of sprite frames.

### 5.0 What "walk [K]" actually shows

Press **K** and the preview does what the *player avatar* does, not what an
NPC does — the two engine drivers differ and the rigs you author here are
avatars:

- **auto-loco** (on by default) starts one clip of the locomotion family —
  `idle`, `walk`, `run`, `fall`, `hang` — and retires the rest. These five are
  **reserved names**: the engine resolves them by string, so a clip called
  `walk2` will never play by itself. The clip list marks them with `◆`.
- The **preview speed** slider is a fraction of the sidecar's `speed`, and
  `speed` is the **sprint** reference — so 1.0 is a sprint, not a walk. It
  defaults to 0.58, where a plain walk sits. Past 0.80 the family switches to
  `run` (with hysteresis down to 0.70, so speeds near the boundary pick one
  and stay there).
- The **stride clock is measured between footfalls**, not derived from the
  slider, and the `walk`/`run` clip is re-rated so one authored arm cycle spans
  one live stride at any pace. The readout under the sliders shows the live
  stride rate and exactly which clips are playing at what weight and rate — if
  the arms look wrong, read that line first.
- Turn **auto-loco off** while authoring a clip: then the preview plays the
  clip you have open instead of whatever the family picked.

### 5.1 Clips (the keyframe lane)

1. **+ clip**, name it (`attack`). Set `durationMs`, `loop`, `mode`
   (`override` replaces the base pose, `additive` layers a delta measured
   against the clip's own first key), blend in/out.
2. **Mask**: which limbs the clip owns. Empty = all. An attack that only
   moves the head should mask to the head so the gait keeps the legs.
3. Select a part (click its name in a lane or the Limbs list). Three
   **rotation rings** appear at its anchor — drag one to pose; the delta is
   in the part's parent frame, which is what clip keys store.
4. **Scrub** by clicking/dragging the lane. **I** writes a key at the cursor.
   With no ring touched, `I` writes the pose the clip *samples* there — i.e.
   a hold key, never a snap-to-rest.
5. **auto-key** writes a key every time you release a ring.
6. Click a key diamond to select it: retime it, change its **ease**
   (`linear`, `quadOut`, `cubicInOut`… — ease belongs to the segment *after*
   the key), or delete it (`Del`).
7. **P** plays the clip. The preview samples with the engine's own
   nlerp/fused-key code, so what you see is what ships.
8. **all parts** shows a lane for every rigged part, not just the keyed ones —
   what you want while blocking out a new clip, when nothing has keys yet.
   **+ / −** zoom the lane; the default 0.45 px/ms makes a 2-second clip
   unreadable.
9. **copy** / **paste** / **mirror →** on a selected key. Mirror pastes onto
   the opposite limb (`.L` ↔ `.R`) reflected about the model's X axis — a walk
   cycle is one arm authored twice, half a period apart, and doing that by hand
   is where sign errors come from.

### 5.2 Flipbooks (the frame strip)

Flipbooks mean two different things depending on the file:

**Rigged mob** — a flipbook animates **one part** by swapping which model that
part renders. The workflow:

1. **+ tag**, name it (`blink`). It binds to the selected limb (change it in
   the **part** dropdown). Frame 0 is the part's own model.
2. Duplicate the part's model (`⧉` in the Models list), edit the copy (closed
   eyes, open mouth…). Keep it the same size — the engine draws frame voxels
   in the part's box, so model it in place. Onion skin (`O`) shows prev/next
   frames red/blue, aligned in that box.
3. Press `+` in the frame strip to append the active model as a frame. Set
   per-frame durations (ms) right in the strip; drag frames to reorder;
   `D` duplicates, `Del` deletes, `[` `]` step.
4. **Space** plays — the viewport shows the *composed* mob with the part
   swapping, exactly what the engine renders.

Every frame must name a part or **the engine silently drops it** — the editor
warns in the strip if any frame is partless.

**Plain file** (no rig) — every model is a frame, played in order. That is the
microvox flipbook convention (grass sway, fire): author each frame as a model
in the same box, and play steps through them one at a time. Create a tag if
you want per-frame durations.

---

### 5.3 Attacks (the stroke programs)

The **attacks** lane edits `assets/mobs/attack_styles.json`: the swings both
the NPCs and the player's discrete strikes replay. It is a lane in the model
editor rather than a row in the Tuning tab because a style's numbers are not a
pose — `reach` is a position in *this arm's* reach band, the hand is the tip
minus the *measured* blade in the fist, and three clamps can each move the
result. The only way to author one is next to a rig that is swinging it.

1. Pick a style chip. They are grouped: `player` (the discrete strikes, short
   windups and jitter 0 so a strike goes exactly where it was flicked) and
   `npc` (longer telegraphs, jittered so ten swings don't look stamped).
2. Edit **windup** (a *pose*, relative to the aim — its length **is** the
   telegraph, there is no UI indicator), **cut** (a *travel*: how far the point
   goes and how fast — this is what does damage, and speed is damage),
   **recover**, and **jitter**. Angles show radians and degrees side by side;
   ticks show their duration at 30 Hz; `reach` shows where it lands in this
   arm's band and warns when it would be clamped.
3. **`+ sword`** arms the rig in one click — it picks the socket for you. Do
   this first: the driver *measures* the blade and solves the whole reach band
   against it, so an empty fist is a different set of numbers, not the same
   swing undressed. (Same button sits in the Held item panel; that one also
   lets you choose which weapon.)
4. **▶ swing** runs the program at 30 Hz. The **aim** sliders set the target's
   bearing: the cut is centred on the aim, so the windup lands half a cut short
   of it and the blade passes *through* where it was aimed. **trail** draws the
   swept edge, blue for the telegraph and amber for the cut.
5. The readout names the arm the rig resolved, the reach band, the blade
   length, both phase machines, and the **tip speed** and **edge alignment** —
   the two things the damage formula scales by. "The attack doesn't hit hard"
   has four separable causes and those numbers tell them apart.
6. **per-limb**: each arm part's `poseLimit` (its range of motion — a *pose*
   stage, not a physics constraint; Jolt's joint limits say nothing about an
   IK-driven pose) beside the body-wide `melee.*` knobs for how much the torso,
   elbow and head join in. Each field says which file it writes: the pose
   limits go to the rig sidecar, the knobs to `assets/materials/tuning.json`.
7. **player flick compass**: the screen-space direction that picks each strike.
   Drag the pad to test a flick. The partition is drawn by asking the engine's
   own quantizer, and sectors *compete* for the circle by max dot rather than
   tiling it — so adding one can swallow another, and the panel says so when a
   style has become unreachable.

Every edit in this lane — style fields, pose limits, the `melee.*` knobs, the
compass — goes on the editor's own undo stack, so **Ctrl+Z / Ctrl+Y** work
across all of it in the order you made the changes. Save writes the JSON; press
**R** in the running game to hot-reload it.

Angles are **authored in degrees** and stored as radians (the value the file
holds is printed under each box). `ticks` show their duration at 30 Hz, and
`reach` shows where that band position lands in voxels *on the arm currently
previewing* — with a `⚠` when it would be clamped.

## 6. Micro detail — finer than one world voxel

Three routes, all engine-supported:

- **Micro bricks (terrain & prefabs)**: a lone 2³/4³/8³ model is one world
  cell subdivided — the green wireframe marks the cell boundary. Save under
  `microvox/` and point a material's `micro` block at it
  (`materials.json`). Every placed cell of that material — hand-painted,
  worldgen, or inside a prefab — renders the brick. A multi-model micro file
  plays as a flipbook (§5.2).
- **Micro-scale mobs**: set **scale** (2 or 4) in the rig panel. The whole
  file is then authored in micro units — `scale` voxels per world voxel — so
  a 128³ sculpt at scale 4 is a 32-world-voxel creature with 4× detail.
  Anchors, chains and clips stay in the same authoring units; the engine
  shrinks everything at load. The status bar shows the world-space size, and
  the green cell marks one world voxel.

  **Adding detail to an existing mob — the `2×` button.** Setting scale alone
  makes the mob *smaller* (same voxels, finer units). To keep its size and
  gain detail, press `2×` in the Models panel: every voxel becomes a 2×2×2
  block, offsets, limb anchors, and clip position keys all double, and scale
  bumps (1→2→4). E.g. the critter ships at scale 2; one press takes it to
  scale 4 — same creature in-world, voxels 1/4 world size — then re-sculpt
  the surfaces to actually use the finer grid (whole mode + noise + a size-2
  brush is a good combo for that). The engine caps mob scale at 4, and limbs
  must stay within ~120 micro units per axis; the editor enforces both.
- **Micro bodies (spheres, debris)**: engine-side, driven by the same pool.

---

## 7. Keyboard reference

| Key | Where | Does |
|---|---|---|
| `T` / `R` / `E` | viewport | attach / erase / paint mode |
| `B` / `G` / `F` / `V` / `N` | viewport | voxel / box / face / select / noise brush |
| `M` | viewport | mirror X |
| `W` | viewport | whole-model mode |
| `1`–`9` | viewport | pick material |
| Alt-click | viewport | eyedropper |
| `Ctrl+Z` / `Ctrl+Y` | anywhere | undo / redo |
| `Ctrl+S` | anywhere | save model (+ sidecar) |
| `Esc` | viewport | clear selection |
| `PageDown` / `PageUp` | viewport | peel / unpeel one depth layer (anatomy row) |
| `Space` | timeline | play / stop flipbook |
| `[` / `]` | timeline | step frame |
| `D` | timeline | duplicate frame |
| `Del` | timeline | delete selected key, else frame |
| `O` | timeline | onion skin |
| `K` | rig | gait walk preview |
| `P` | clips | play / stop clip |
| `I` | clips | write key at cursor |
| Drag left edge | rig panel | resize the panel (double-click resets to 320px) |

Keys never fire while you're typing in a field.

The rig panel's width is remembered per browser, so a rigging pass can keep it
wide for readable limb names and a painting pass can hand the width back to the
viewport. It stops at 200px, and never shrinks the viewport below 220px.

---

## 8. Troubleshooting

- **Palette is empty / everything paints grey** — load `materials.json` on
  the Overview tab; the editor derives its palette from it.
- **"view capped at N cubes"** — you exceeded the instance budget (onion skin
  on a huge scale-4 mob can do it). The document is intact; turn off onion or
  hide models. Nothing is lost on save.
- **My flipbook does nothing in-game** — every frame needs a `part` that
  matches a limb name; the strip shows a ⚠ warning when one is missing.
- **The gait won't move a leg** — it needs a two-bone chain *and* a gait
  block; a limb in no chain only gets the legacy `swingAmp` sine.
- **The editor pane says it failed to load** — it needs WebGL plus ES-module
  and import-map support (Chrome/Edge 89+, Firefox 108+), and the vendored
  three.js under `assets/editor/vendor/`. The rest of the tuner works
  regardless.
- **Save aborted: round-trip failed** — the writer refused to produce a file
  that reads back differently. That's the guard doing its job; report the
  toast text.

---

## 9. Writing a conversation (the Dialogue tab)

A conversation is one file, `assets/dialogue/<name>.json`. The file's NAME is
what everything calls it: an NPC's `dialogue` property, a `met` condition, the
in-game picker. The worked example is `sample_stranger.json` — open it first;
it uses every condition and every action there is.

### The loop

1. Tuner → **Dialogue** tab. Pick a file on the left, or **+ new file**.
2. Edit (below). The list under the graph shows problems as you type.
3. **Save** (Ctrl+S). The file is written in a tidy one-line-per-choice
   layout, so `git diff` shows exactly what you changed.
4. In the game press **R** — conversations reload with the materials; the
   world's flags are kept.
5. F1 → **Spawn** → **Dialogue**: pick the file, then **talk to nearest
   creature** (anything alive within 12 m speaks the lines — it does not have
   to be a person) or **talk (no speaker)** to read it through with nobody
   there. **forget flags + met** puts the world back to "never spoken to
   anyone" so you can try it from the start. The same section lists any
   problems with the files, in red (errors) and amber (warnings).

In a conversation: **1-9** or a click picks a choice, **Space/Enter** is
[continue], **Esc** leaves (unless that line says you may not). You cannot
walk while talking.

### How a conversation is put together

- **Nodes** are lines. Each has an **id** (unique in the file), the **text**
  the speaker says, and usually some **choices** — what the player can answer.
- A **choice** has its text and **leads to** a node. "(end the conversation)"
  is also a destination.
- A node with no choice showing gets **[continue]**, which goes where the
  node's own **"With no choice showing, [continue] goes to"** says — or ends.
- **Entries** (the file settings, the ⚙ item at the top of the node list)
  decide where a conversation STARTS: the first entry whose conditions hold
  wins, so put the special cases first and a plain "start at hello" last.
- The **speaker** name in the header comes from the node, else the file.

### Conditions ("only if")

A condition row is a dropdown and a value. A choice with conditions is HIDDEN
unless all of them hold; an entry is skipped; a node you arrive at goes to its
**otherwise go to** node instead (or ends).

| Condition | Holds when |
|---|---|
| `flag` x | flag x is set (non-zero). Put a number after `=` to need exactly that value |
| `!flag` x | flag x is not set |
| `time` from–to | the in-game clock is in that range (wraps midnight: 20:00–05:00 is night) |
| `activity` / `!activity` | the speaker's current schedule row is (not) that activity — sleep, work, wander, socialize, eat, goto. Until NPC schedules exist this is always false |
| `has` / `!has` item | the player's pack or hotbar holds (at least N of) that item. Worn gear does not count |
| `met` (this conversation) | the player has finished this conversation before |
| `met` other_name | the player has finished THAT conversation before — how Agnes knows you spoke to Osric |

### Actions ("do")

Run in order, on arriving at a node or on picking a choice (before it moves on).

| Action | Does |
|---|---|
| `set` x = n | flag x becomes n (default 1) |
| `add` x + n | flag x goes up by n (count visits, favours owed) |
| `clear` x | flag x is unset |
| `give` item × n | into the player's pack (the bag, then the hotbar; a full pack refuses) |
| `take` item × n | out of the pack/hotbar (guard the choice with `has` so it cannot fail) |
| `end` | the conversation ends after this |

**Flags are world-wide.** A flag set in Wat's conversation is the same flag
Osric's reads. That is how two NPCs talk about you: Wat's choice does `set
wat_asked`, and Osric's reply has a choice shown only if `flag wat_asked`.
Flags and who you have met are saved with the world (F9).

### The graph

Boxes are nodes, in columns by how many steps they are from the start. Blue
numbered arrows are choices, grey `>` is [continue], dashed red is "otherwise".
Faint arrows go back to an earlier node (most conversations return to a
question hub like `ask`). A dashed box is a node nothing leads to. Drag to pan,
wheel to zoom, **Fit graph** to see everything, click a box to edit it. The
**→** buttons beside a "leads to" jump to that node.

### What the checks mean

| Message | Fix |
|---|---|
| `goes to 'x', which is not a node in this file` | a typo in a destination, or a node you deleted |
| `cannot be reached from any entry` | nothing leads there; link it or delete it |
| `flag 'x' is set ... but no condition reads it` | usually a typo in one of the two spellings |
| `flag 'x' is read ... but nothing sets it` | same, from the other side |
| `'x' is not an item` | item names are the ones in the Items tab |
| `unknown condition` / `unknown action` | a typo in the key if you edited the JSON by hand |

A file with an **error** is skipped by the game (every other file still
loads). Warnings load.

### Editing the JSON by hand

It is plain text; a text editor and R work fine. Keep one choice per line.
The full schema, with every key, is at the top of `src/game/dialogue.h`.
`node scripts/test_dialogue.mjs` checks every file the way the tab does.

## 10. References — the placed things in a map (in game: F1 → World → References)

A **reference** ("ref") is one thing you placed on purpose: a villager, a well,
later a house, a door, a chest. Each has an **id** like `harrowby/osric` that
never changes — schedules, dialogue and saves all point at things by that id.

Refs live in plain text: `assets/worldmap/<map>/refs/<group>.json`, one ref per
line. A group is a place (`harrowby.json`). You can edit these files in any text
editor and press **R** in game; or use the in-game page below, which writes the
same file.

```json
{ "id": "harrowby/osric", "kind": "npc", "base": "human", "pos": [612, 204, 3530], "yaw": 180, "props": { "name": "Osric" } }
```

| Field | Means |
|---|---|
| `id` | `group/name`, lowercase letters, digits, `_` and `-` only |
| `kind` | what sort of thing: `marker`, `npc` (§10.2), `waynode` (§10.2), `door`, `container`, `bed` (§10.1; `structure` arrives with houses) |
| `base` | what it's an instance of — for an `npc`, the mob def (`human`, `dummy`, ...) |
| `pos` | world position in voxels (10 voxels = 1 m); the crosshair readout on F1 shows cells |
| `yaw` | facing in degrees: 0 faces +Z, 90 faces +X |
| `props` | anything else; each kind reads what it needs (`name`, `tags`, ...) |

Anything the game doesn't recognise (a prop, a field, even a kind) is kept
exactly as you wrote it and shown as a warning — nothing you typed is ever
silently thrown away.

### How do I…

- **See what's placed?** F1 → **World** tab → open **References**. Refs are
  listed by group; `*` marks the ones active right now (near you). Filter by
  kind or type in the search box (it searches ids, bases and props). A red row
  is a kind this build doesn't know.
- **Go and look at one?** Double-click its row, or select it and press **fly to
  it**. You're put in fly mode a few metres off it, looking at it.
- **Place a new one?** Under **new reference**: type an id (`harrowby/well`),
  pick a kind, optionally a base, then **place at crosshair** (the block you're
  looking at, one above) or **place at my feet**. The group file is created if
  it's new.
- **Move or turn one?** Select it, edit **pos** / **yaw** (the ± buttons step
  yaw by 15°), press **apply pos/yaw**. Or **move to my feet** / **move to
  crosshair**.
- **Change what it is?** Pick another **kind**; type a new **base** and press
  Enter.
- **Edit a prop?** Change the value and press **Enter**. Values are JSON (`3`,
  `true`, `["gather", "water"]`, `"text"`), but a bare word like `Osric` is taken
  as text. Add a prop with the empty row at the bottom; **x** removes one.
- **Delete one?** **delete...**, then **really delete?**.
- **Fix a typo by hand?** Edit the `.json` in a text editor, save, press **R**
  in game. Only the refs whose line changed are re-applied.
- **See what's wrong?** The **warnings** list at the bottom of the page names
  the file, the ref id and the field for every problem (unknown kind, a missing
  `pos`, two refs with the same id, a saved state for a ref you deleted).

### Using things in the world

Walk up to a usable thing (a villager, a door, a chest, a bed)
and look at it: the prompt under the crosshair says what **G** will do ("G  Talk
to Osric"). Tap **G**. If something lying on the ground is also under the
crosshair, G picks that up first — look at the thing you mean.

### What saves, and what doesn't

The ref files are the design; a save only remembers what *changed in play*
(a villager was spawned and walked off, later: a door left open, a chest
emptied). So:

- Editing a ref file between saves is safe. A saved state for a ref you've since
  deleted or renamed is dropped with a warning — it does not break the save.
- A villager, once spawned, belongs to the world: it keeps its identity
  (`harrowby/osric`) through saving, loading and walking out of range. Editing
  its ref (moving it, changing its base) re-spawns it where the file now says.
- The harness map's `refs/fixture.json` is for the automated tests only; the
  game never loads it.

### 10.1 Doors, chests and beds

These are three more kinds of ref. The door, chest and bed themselves are
**voxels you built** (in the world, or later in a house's structure file); the
ref only says "this box of cells is a door", "this cell is a chest", "a person
lies here".

#### How do I add a door?

1. **Build the doorway and the leaf** out of voxels: a gap in a wall, filled
   with the door's material (any solid — `wood` today, `door_wood` once the
   building materials land). A human-sized door is 9 wide × 20 tall × 1 thick.
2. **Stand on the side you want it to swing toward**, look at the doorway, and
   place a ref of kind **door** (F1 → World → References → new reference,
   kind `door`, **place at my feet**).
3. Set its **pos** to the leaf's **bottom cell on the hinge side, in the front
   layer** (the layer facing you), and its **yaw** to the direction it should
   swing toward (0 = +Z, 90 = +X, 180, 270 — whole quarter-turns only).
   Apply pos/yaw.
4. The door's own fields appear under the props:
   - **hinge** `left` / `right` — seen from where you're standing (the side it
     opens toward), facing the door, which hand the hinge is on;
   - **open angle** — how far it swings (default 95°);
   - **w / h / thick** — the leaf size in cells (default 9 / 20 / 1; thickness
     goes *away* from you, behind the front face). Press Enter to apply;
   - **locked** — a locked door says "Locked" and won't open (no keys yet);
   - **auto-close s** — seconds before it swings shut on its own (0 = never).
5. **Check the arc**: while the door is selected, its outline (gold), its
   fully-open position (orange) and the path its edge sweeps are drawn in the
   world, with the hinge in red. If the arc goes through a wall or the hinge is
   on the wrong side, flip **hinge** or change **yaw**.
6. Press **test open/close** to swing it from wherever you are (the door must be
   near enough to be active, `*` in the list). Or walk up and tap **G**
   ("Open door" / "Close door").

What happens: open lifts the leaf out of the wall and hangs it on a hinge as
one solid object — you can't walk through it, and it stops against anyone in
its way. Close swings it home and puts the exact same voxels back. If something
was put in the doorway, it waits and tries again. A door that burns or gets
cut while it's open stops being a door ("Broken door") and the pieces fall —
rebuild the leaf and edit the ref (any change) to revive it. An open door
can't be dragged with hold-G.

#### How do I add a chest?

1. Build the chest out of voxels.
2. Place a ref of kind **container** on the chest's cell (stand close: the use
   prompt answers when the crosshair passes within half a metre of that cell).
3. Under **contents**, type in the search box and click an item to add it;
   set counts with the number fields; **x** removes a row. That writes
   `props.items`, e.g. `[{"item": "bread", "count": 3}]`. A red name is an
   item the game doesn't have (it's skipped). Optional `props.title`
   (`"Strongbox"`) changes what the prompt and the panel call it, and
   `props.locked: true` makes it say "Locked strongbox" and stay shut (no keys
   yet — the same rule as a locked door).
4. In game, **G** on it says "Search chest" and opens the loot panel beside
   your pack: right-click a slot or **take all** to take; drag from your bag or
   hotbar onto the panel to put something in.

Once anything is taken or put, the chest's contents are part of the **save**,
and editing `props.items` only changes what a *new* game starts with. The page
says "this playthrough has changed it" and offers **reset to authored**.

#### How do I add a bed?

Place a ref of kind **bed** with **pos** on the mattress cell where the **head**
lies and **yaw** pointing from head to foot; `props.length` (default 18 cells)
is how long it is. The page draws a blue line head → foot. In game **G** says
"Rest" (sleeping through the night isn't in yet). Villagers lie down on it
(§10.2).

#### What a door, a chest and a bed save

A door left open saves as open (with its exact voxels) and comes back open; a
chest saves what's in it now; a bed saves nothing.

### 10.2 How do I give an NPC a home and a day?

A villager is a ref of kind **npc**. Its *day* is a **schedule** — a short list
of "from this time to that time, do this, there" — and the places it names are
other refs: its bed, the spot where it works, a well it chats at. It walks
between them over **waynodes** (dots you place along its paths and through
doorways), opens the doors on the way and shuts them behind it.

#### 1. Place the villager

F1 → World → References → **new reference**: id `harrowby/osric`, kind **npc**,
base `human` (or any character: `brug`, `jujunud`, ...), **place at my feet**.
Then set its props (the page shows a **villager** panel under them):

| Prop | What |
|---|---|
| `name` | what the prompt calls it ("Talk to Osric") |
| `schedule` | which `assets/schedules/<name>.json` it follows (pick it in the panel) |
| `bed`, `work`, `home` | the refs it sleeps on / works at / eats at — pick them from the dropdowns. Any other prop naming a ref (`"tavern": "harrowby/alehouse/hearth"`) becomes a place a schedule can name too |
| `dialogue` | which conversation **G** starts (`assets/dialogue/<name>.json`, §9) |
| `outfit` | what it wears: `["tunic#B4472A", "breeches", "boots"]` (`#RRGGBB` dyes a dyeable piece) |
| `weapon` | what it holds: `"cleaver"` |
| `behavior` | its character (default `villager`: minds its own business, runs from monsters, hits back if you hit it) |

The bed is a **bed** ref (§10.1), the work spot is any **marker** (its **yaw** is
the way the villager faces while it works), and a gathering place is a marker
with a tag: `props.tags: ["gather"]`.

#### 2. Write its day

With the villager selected, the **schedule** table is right there on the page:
one row per span of the day — **from** / **to** (drag the hour and minute),
**do** (the activity), **at** (where), **m** (a radius in metres), and **jump**.

| do | What it does there |
|---|---|
| `sleep` | walks to its bed and lies down on it |
| `work` | stands at the spot facing the marker's yaw |
| `eat` | stands at home (a house's hearth) |
| `socialize` | stands at the place and turns to the nearest other villager |
| `wander` | strolls around the place, within the radius |
| `goto` | goes there and stands |

**at** is a *role* (`bed`, `work`, `home`, or one you added), a *tag* (the
nearest marker wearing it, e.g. `gather`) or a ref id (`harrowby/green_well`).
Rows may wrap midnight (`22:00` → `06:00`). Under the table the page says if a
minute of the day isn't covered (the villager then idles at home) or two rows
overlap (the upper one wins). **save** writes `assets/schedules/<name>.json`;
type a new name first to save a copy and switch this villager to it. Several
villagers can share one schedule.

The same files are on the tuner's **Environment → Schedules** page (a table,
time pickers, the same checks, the raw JSON), or in any text editor — press
**R** in game after a hand edit:

```json
{
  "about": "Osric: the anvil by day, the alehouse of an evening.",
  "rows": [
    { "from": "22:00", "to": "06:00", "do": "sleep", "at": "bed" },
    { "from": "07:00", "to": "18:00", "do": "work", "at": "work" },
    { "from": "18:00", "to": "22:00", "do": "socialize", "at": "tavern", "radius": 3 }
  ]
}
```

#### 3. Give it paths (waynodes)

A villager finds its way across a room by itself, but not across a village or
through a door. Place refs of kind **waynode** where people walk: in each room,
a pace **inside and outside every doorway**, and along the paths between
houses. A house made on the Structures page already has its own (inside/outside
each door and in each room).

Select a waynode: its **links** are listed and drawn in the world (orange = the
link goes through a door). Pick another waynode in the list under it to **link**
them; **x** unlinks. A link works both ways, so you only write it once — and
that is how houses join the village: put a waynode on the green and link it to
the house's *outside-door* node (`harrowby/smithy/waynode_front_0_out`); the
house file never changes. **autoLink** (metres) joins a node to every node that
close — handy on an open green, but it doesn't check for walls, so use explicit
links wherever there's something in the way.

#### 4. Try it

The villager panel shows what it is doing **now** — the row, where that is, the
walk (`travel`, `door: harrowby/smithy/door_front_0`, `at anchor`), and what
comes **next**. Its route is drawn in the world (blue, with an orange post at
each door it will open). Press a **jump clock to** button (or **jump** on a row)
to move the in-game clock to that row and watch it go. Tap **G** on it to talk:
it turns to you and waits until you're done.

If jumping does nothing, the day is frozen by tuning (`dayNight.freeze`); the
panel says so.

#### When it doesn't go where you expect

- *"no schedule named ..."* — the `schedule` prop names a file that doesn't
  exist yet; save one from the table.
- *"'x' is not a prop on ..., not a ref id and no ref has it as a tag"* — a row's
  **at** names nothing. Add the role prop, or the tag on a marker.
- *"no progress toward ..."* / *"gave up ..."* — something blocks the walk
  (a wall between two linked waynodes, a missing link). Select the waynodes and
  look at the drawn links; add one through the doorway.
- *"door ... is locked"* — villagers don't have keys either.
- Walked out of range and came back? A villager is put where its day says it
  should be *now* (asleep in bed at night), not where you last saw it.

## 11. Buildings — the Structures page

A **structure** is a building blueprint: `assets/structures/<name>.vox` (the
voxels) plus `<name>.struct.json` (where the doors, beds, chests, hearth and
walking nodes are). You make one from the house generator, then hand-edit it.
Open **Environment → Structures** (sidebar, under Components).

### How do I make a new house?

1. Pick a starting point in **new from…** (cottage, longhouse, smithy,
   alehouse, townhouse).
2. Move the sliders. Every row has a tooltip saying what it does. The preview
   regenerates as you drag; the stats line under it says how many voxels,
   rooms, doors, windows, beds and chests you got, and prints a warning in
   orange if something you asked for did not fit (a window between two doors,
   a bed with no headroom, a stair in a house too narrow for it).
3. Not keen on the small choices (which way the corner braces run, where the
   beds went)? Press **Reroll** — a new seed, same house. Type a seed to get a
   particular one back.
4. **Save as…** and give it a lowercase name (`harrowby_smithy`, or
   `samples/barn` to put it in a folder). Two files appear in
   `assets/structures/`.

Undo/redo is `Ctrl+Z` / `Ctrl+Shift+Z` (or the ↶ ↷ buttons); a whole slider
drag is one undo step.

### How do I see inside?

The **view** select: *roof off*, *ground floor*, *upper / loft*. It only cuts
the preview — the saved house is never cut. Clicking a row in the slot table
selects that slot, points the camera at it and cuts the roof away for you if
it is inside.

### What are the coloured markers and labels?

Slots. Toggle them with the **slots** / **labels** boxes; the legend is in
the view bar.

| Colour | Slot | What it is for |
|---|---|---|
| orange | `door_front_0`, `door_back_0`, `door_left_0` … | the door leaf (box) and its hinge (red line). Doors open inward. |
| blue | `bed_0` … | where a sleeper lies; the tick points foot → head |
| yellow | `chest_0` … | a container; contents are added later (References page) |
| green | `hearth`, `work_1` … | where someone stands to cook / work, facing the tick |
| cyan | `waynode_*` | walking points NPCs route through; the lines are the links |

### How do I change an existing house?

Click it in the list (or pick it in the **structure** dropdown). You first see
the file **on disk**. Move any slider and the preview switches to the
**generated** scaffold; **Save** writes it back.

Houses marked **hand-edited** (orange badge) are different: someone edited
the voxels, so the `.vox` is now the real house and the generator must not
overwrite it. Save on one of those offers `<name>_v2` instead, and the tuner
refuses a direct overwrite too. To regenerate a hand-edited house for real,
delete its two files yourself.

### How do I make one from a terminal?

```bash
node scripts/bake_structure.mjs harrowby_smithy --preset smithy --seed 12
node scripts/bake_structure.mjs harrowby_smithy                    # re-bake from its own settings
node scripts/bake_structure.mjs my_house params.json               # {"seed": 3, "params": {...}}
node scripts/bake_structure.mjs --samples                          # rebuild the three samples
```

Same generator as the page, same bytes. It never overwrites a hand-edited
structure.

### Where do the materials come from?

Nine building materials in `materials.json` (Materials tab): `timber` (dark
beams), `plank` (boards), `daub` (pale plaster panels), `cobble` (rubble
stone), `flagstone` (floors, hearths, quoins), `roof_tile`, `thatch`,
`door_wood` (only ever a door leaf), `straw_bed`. Wood, thatch and straw
burn; stone, daub and tile do not. Change a colour there and press `R` in
game.

### Troubleshooting

- **A window / bed / door I asked for is missing** — read the orange warning
  in the stats line; it names what did not fit and why. Usually: widen the
  house, or ask for fewer.
- **"materials.json renumbered since this was written"** — the house's `.vox`
  stores material numbers; someone inserted a material instead of appending.
  Re-bake it (`bake_structure.mjs <name>`), or if it is hand-edited, fix the
  materials list.
- **Save is refused with "HAND-EDITED"** — working as intended; save under a
  new name.
- **Check the page works:** `bash scripts/check_structures.sh`; a picture:
  `bash scripts/check_structures.sh --shot out.png --structure samples/smithy --clean`.
- **Which maps use this house?** The **used by** line under the structure
  list names every map ref that places it (`default: harrowby/smithy`).

## 12. Putting a house in the world (in game: F1 → World → References)

A house in the world is a **`structure` reference**: one line in a map's refs
file that says *which* blueprint, *where* and *which way round*.

```json
{ "id": "harrowby/smithy", "kind": "structure", "base": "samples/smithy", "pos": [612, 204, 3530], "yaw": 90 }
```

| Field | Means |
|---|---|
| `base` | the blueprint: the path under `assets/structures/` without the extension (`samples/smithy`, `harrowby_smithy`) |
| `pos` | where the house's **front-door floor** goes: x and z are the middle of the house, **y is the floor you walk on**. The ground around the house is levelled so its top is one voxel below that. |
| `yaw` | which way the front door faces: `0` = +Z, `90` = +X, `180`, `270`. **Only quarter turns** — anything else is refused with a warning and the house is not built. |
| `props.padMargin` | optional: how many voxels the levelled ground takes to blend back into the terrain (default 12 = 1.2 m; 1..64) |

The house is part of the **terrain**: it shows at any distance, it is not in
your save, and it comes back the same every time. Damage done to it in play is
an ordinary edit and *is* saved.

### How do I place a house?

1. F1 → **World** → **References**, scroll to **place a structure**.
2. Pick the blueprint in the dropdown (**rescan** if you just saved a new one
   in the tuner).
3. Type an id: `harrowby/smithy` (group / name; the group file is created if
   it is new).
4. Press **place structure here...** A gold wireframe box shows the space the
   house will take, standing on the block under your crosshair; the small
   **red box marks the front door side**. Look around — the box follows the
   crosshair.
5. **-90 / +90** turn it. Tick **pin** (or type numbers into **floor**) to stop
   it following the crosshair while you walk round it. The line above says the
   floor position and the footprint in metres.
6. **confirm: place it.** The line is written to the refs file and the house
   appears within a frame or two (the status line says "re-applied 1
   structure(s): N chunks regenerated").

**cancel** leaves without writing anything.

### How do I move, turn or remove a house?

Select it in the list (structures show `+N slots`). Then:

- **move**: edit **pos** and **apply pos/yaw**, or **move to my feet** / **move
  to crosshair**;
- **turn**: the **-90 / +90** buttons beside **reload asset**;
- **swap it for another blueprint**: type a new **base** and press Enter;
- **remove**: **delete...** → **really delete?**.

Each re-builds only the ground the house covered and now covers; the rest of
the world (and everyone in it) carries on.

> A re-build puts the chunks it touches back to the terrain + house. Anything
> you dug, burnt or built by hand **inside the house's box** in this session is
> reset, and the status line counts those chunks. Edit houses before you play
> in them.

### The doors, beds and chests inside

Every slot of the blueprint becomes its own reference, named
`<house id>/<slot>`: `harrowby/smithy/door_front_0`, `.../bed_0`,
`.../chest_0`, `.../hearth`, `.../waynode_room_0`. Open **slots (N)** under a
selected house to see them; click one for where it is and what it holds (the
door's leaf box and hinge, already turned to match the house). They are
**read-only here**: they come from the blueprint, so to move a bed you edit the
house. The doors, chests and beds are the real thing from §10.1: walk up to a
house's door and tap **G** to open it (it swings inward, the way the blueprint
drew it), search its chest, rest in its bed. A chest's starting contents are
the blueprint's `items` for that slot. Walking points (`waynode`) stay grey
until NPC residents arrive. Schedules and dialogue can name any slot by id.

### I edited the blueprint — how do I see it in the world?

Select any house built from it and press **reload asset**. Every copy of that
blueprint is re-stamped, and its slots are re-read. The in-game editor (F8,
§13) does this for you on save; **open in editor** is its button.

Editing the refs file in a text editor and pressing **R** works too, and **F7**
(regenerate the world) always shows the latest of everything.

### Troubleshooting

- **The house isn't there** — look at the **warnings** list on the References
  page: a `base` that names no file, a `yaw` that is not a quarter turn, or a
  blueprint built at a different voxel size each say so, naming the ref.
- **It floats / is buried** — `pos.y` is the floor. Use **move to crosshair**
  aimed at the ground where the door should be (that sets y to one above the
  block), or type y.
- **"N sites reach map cell … a cell holds at most 32"** — that many houses,
  lakes and authored trees within one 102 m square; spread them out.
- **Check it works:** `--selftest --gate structure-stamp` (every voxel of a
  sample, four turns, the levelled ground, the slots) and
  `--gate structure-reload` (the live re-build equals a fresh world).
  `SANDVOX_STRUCTURE_SHOT=house.bmp` with `structure-stamp` also saves a
  picture.

---

## 13. Editor mode — building in the game (F8)

Editor mode is where you build a place by hand: the houses' walls, doors and
furniture, and every villager, chest and path in the map. It runs inside the
game, on the real world.

**Press F8.** The game stops being played:

- **The world pauses.** The orange **WORLD PAUSED** at the top-left means
  nothing burns, flows or walks while you work. Each edit you make advances the
  world by exactly one tick so the change reaches it (so sand you paint does
  fall one step). F8 again returns you to your body where you left it.
- **The camera leaves your body.** Hold the **right mouse button** and move the
  mouse to look; **W A S D** fly, **Q / E** go down / up, **Shift** is 4x
  faster, the **mouse wheel** sets the speed (shown in the toolbar). **F**
  frames whatever is selected.
- **The cursor is free** and three panels come up: the **toolbar** (left),
  the **inspector** (right, when something is selected) and the **status bar**
  (bottom: tool, selection size, `* unsaved`, undo / redo depth, the keys for
  the tool in your hand, and the last thing that happened — red when something
  was refused, with the reason).

Everything you do is **undoable**: **Ctrl+Z** undo, **Ctrl+Y** (or
Ctrl+Shift+Z) redo, 256 steps. A brush drag or a cut is one step.

### Two kinds of thing you edit

| | What | Saved |
|---|---|---|
| **References** | villagers, doors, chests, beds, markers, waynodes, and where each house stands (`assets/worldmap/<map>/refs/*.json`) | **immediately**, every edit |
| **A house** (open it first) | its voxels and its slots — the doors, beds, chests, markers and paths that are part of the blueprint (`assets/structures/<name>.vox` + `.struct.json`) | on **Ctrl+S**; until then it is shown live in the world and the status bar says `* unsaved` |

Saving a house re-stamps **every copy** of it in the world (the same thing as
**reload asset**), marks it `handEdited` so the generator on the Structures page
never overwrites it, and moves the world hash (that is expected). Placing,
moving or saving a house lets the world run for about two seconds so the
rebuilt ground and house appear; otherwise it stays paused.

### The keys

| Key | Does |
|---|---|
| `F8` | enter / leave the editor (leaving asks about unsaved houses: Save all / Discard / Keep editing) |
| right mouse + drag | look |
| `W A S D`, `Q E` | fly, down / up (`Shift` 4x) |
| mouse wheel | flying speed |
| `F` | frame the selection |
| `1` – `8` | tools: Select, Pencil, Brush, Box, Line, Eyedropper, Place, Link |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo |
| `Ctrl+S` | save the open house |
| `Esc` | cancel what you are doing (paste, line, link), then clear the selection |
| `H` | show all the keys in a window |

### The tools

**1 Select.** Click a thing (its box and label are drawn in its kind's colour)
to select it; the inspector opens on the right. A selected thing has a
**gizmo**: drag the **red / green / blue arrow** to move it along x / y / z one
voxel at a time (hold **Shift** for 1 m steps), drag the **yellow ring** to turn
it (15° steps; houses and slots in 90° steps — the red line shows which way it
faces). Keyboard: **arrows** nudge x / z, **PgUp / PgDn** nudge y, **R** turns
(Shift+R back), **Delete** deletes (Ctrl+Z puts it back on the same line of its
file), **Ctrl+D** duplicates. **Double-click a house** to open it.

**2 Pencil** *(house open)*. Click a face to put one voxel on it; drag to draw.
**Shift+click** erases the voxel under the cursor. **Alt+click** takes its
material. The cursor shows the cell a click will fill.

**3 Brush** *(house open)*. Paints a sphere, or a cube (**B** toggles), of the
current material at the cursor. **[ / ]** size. Shift erases, Alt picks.

**4 Box** *(house open)*. Drag across the house to select a box (drawn in blue
with its size). **PgUp / PgDn** raise / lower the top (with **Shift**, the
bottom); **arrows** slide the box, **Ctrl+arrows** move the voxels in it; the
toolbar has the exact min / max to type into. Then:

| Key / button | Does |
|---|---|
| `Enter` — **Fill** | fill the box with the current material |
| `Shift+Enter` — **Walls** | the six faces in the material (walls, floor, ceiling), inside untouched |
| `Backspace` — **Hollow** | empty the inside, keep the faces (carve a room out of a block) |
| `Delete` — **Clear** | empty it |
| `T` — **replace** | every voxel of the "replace" material (the list shows what is in the box) becomes the current one |
| `Ctrl+C` / `Ctrl+X` | copy / cut |
| `Ctrl+V` — **Paste...** | a green **ghost** of the copy follows the cursor; **R** turns it 90°, **M** mirrors it (x, then z, then off), hold **Shift** to leave the air in the copy out; **click** to land it (click again for another copy), **Esc** to stop |

**5 Line** *(house open)*. Click one end, then the other: a beam of the current
material between them. **[ / ]** thickness (0 = one voxel). Shift-click ends on
the voxel itself instead of on its face.

**6 Eyedropper** *(house open)*. Click a voxel of any house to take its
material (Alt+click does this in the other voxel tools).

**The material** is shown with a **change** button in the voxel tools; the
picker is the same one as the F1 brush (search, columns by class). A house can
only hold materials 1–255.

**7 Place.** With **no house open** it places **references** into the map: pick
a kind (npc, door, container, bed, marker, waynode, structure), the group file
it goes in (`village` → `refs/village.json`; ids are `<group>/<kind>_<n>`) and
the facing, then click the ground or a floor. For **structure**, pick the
blueprint: its footprint box follows the cursor with its front marked. With
**a house open** it adds **slots** to the house instead (door, bed, container,
marker, waynode), with sensible starting boxes: a 9 x 20 door leaf, a bed frame,
a chest box.

**8 Link.** Click a waynode, then another: they are joined (a link is two-way;
it is written on the first one you clicked, or on whichever is not part of a
house). **Shift+click** the second to unlink. Inside an open house, linking two
slots writes the house's own path.

### Editing a house

1. Open it: **double-click** it with Select, double-click it in the toolbar's
   **Houses in this map** list, or press **open in editor** on the References
   page (F1 → World → References). The camera swings to a three-quarter view.
2. The house's **box** (gold), its **front** (red line) and its **slots** are
   drawn. Doors show their leaf, the **orange swing** and the **red hinge** —
   if a door opens into a wall you see it before anyone tries.
3. Edit with tools 2–6. Everything you do shows in the world at once.
   **A turned house edits correctly**: you work on the house as it stands, and
   the editor writes the change into the blueprint's own frame.
4. Slots: select one (Select tool), drag it with the gizmo, or type its pos /
   yaw / props in the inspector. Props in a house are in the **house frame**
   (0 0 0 = the front-centre floor cell, y up, +z toward the front). Delete
   removes a slot; Place adds one.
5. **Ctrl+S** saves (or **Save** in the toolbar). **Close house** asks first if
   there is anything unsaved (Save then close / Discard / Cancel).

The blueprint may grow past its old box (a porch, a chimney): the files are
re-based around the house's anchor, so nothing else moves. A `.vox` is at most
256 voxels on a side; the save says so if you go past it.

### The inspector

For a **reference**: kind, base, pos / yaw (type and **apply**), every prop as
JSON (`3`, `true`, `"text"`, `["a", "b"]`; a bare word is text; Enter applies,
`x` removes, the last row adds), **duplicate**, **delete**, **open house** for a
structure — and underneath, the same panels as the References page: a door's
hinge / angle / size / locked with **test open/close**, a chest's contents, a
bed's line, a villager's schedule and live state, a waynode's links. Their
edits are undoable here too.

For a **slot**: pos / yaw in the house frame, its props, **remove slot**.

A slot of a house you have NOT opened is read-only (it comes from the
blueprint) — the inspector offers **open its house**.

### The session journal

Every command you run is appended to `build/editor/session_<date>_<time>.jsonl`
(the path is at the bottom of the **H** window). It is an edit script: read it
to see what you did, or replay it with `--edit-script`.

### Edit scripts (building from a text file)

```bash
bash scripts/run.sh ./build/Release/sandvox.exe --edit-script my_village.jsonl
./build/Release/sandvox.exe --edit-commands          # every command and its arguments
```

One command per line, as JSON: `{"cmd": "<name>", "args": {...}}` (or the args
inline). `#` and `//` lines are comments. It runs on the map the game would
load (`SANDVOX_MAP=<name>` picks another) and is **all or nothing**: if any
line is refused, everything the script had done is undone and the error names
the file, the line, the command and the field:

    EDIT-SCRIPT FAILED: my_village.jsonl:12: vox.box_fill: mat: "cobbel" is not a material in materials.json (did you mean "cobble"?)

On success every house it edited is saved. `--edit-no-save` leaves houses
unsaved (refs are still written).

Coordinates: **refs in world voxels**. **Voxels and slots in the house frame**
of the open house (y = 0 is the floor, +z the front, unrotated — the same
numbers whichever way a copy is turned), or in world voxels with
`"frame": "world"` (converted through the copy you opened).

```
# a well, a villager, a path node joined to the smithy's front door node
{"cmd": "ref.place", "args": {"id": "harrowby/well", "kind": "marker", "pos": [4120, 188, 3960], "props": {"tags": ["gather", "water"]}}}
{"cmd": "ref.place", "args": {"id": "harrowby/osric", "kind": "npc", "base": "human", "pos": [4130, 188, 3972], "yaw": 180, "props": {"name": "Osric"}}}
{"cmd": "ref.place", "args": {"id": "harrowby/path_1", "kind": "waynode", "pos": [4125, 188, 3965]}}
{"cmd": "ref.link",  "args": {"a": "harrowby/path_1", "b": "harrowby/smithy/waynode_front_0_out"}}
# the smithy: a hearth, a window bay, a ceiling beam, a new chest slot
{"cmd": "struct.open",  "args": {"ref": "harrowby/smithy"}}
{"cmd": "vox.box_fill", "args": {"min": [-40, 0, -6], "max": [-34, 8, 6], "mat": "cobble"}}
{"cmd": "vox.clear",    "args": {"min": [-16, 8, 36], "max": [-10, 14, 36]}}
{"cmd": "vox.line",     "args": {"from": [-40, 22, -30], "to": [40, 22, -30], "mat": "timber", "radius": 1}}
{"cmd": "slot.add",     "args": {"name": "chest_1", "kind": "container", "pos": [20, 0, 10], "props": {"box": {"min": [17, 0, 8], "max": [23, 4, 12]}, "items": [{"item": "bread", "count": 3}]}}}
{"cmd": "struct.save",  "args": {}}
```

**The commands** (`--edit-commands` prints this list from the build you have):

| Command | Args | Does |
|---|---|---|
| `ref.place` | `{id, kind, base?, pos, yaw?, props?}` | a new reference |
| `ref.move` | `{id, pos?, by?, yaw?}` | move / turn |
| `ref.set_prop` | `{id, key, value}` | set a prop (`null` removes it) |
| `ref.set_field` | `{id, field: "kind" or "base", value}` | change kind / base |
| `ref.delete` | `{id}` | delete |
| `ref.duplicate` | `{id, as, by?}` | copy to a new id |
| `ref.link` / `ref.unlink` | `{a, b}` | join / part two waynodes |
| `struct.open` | `{ref}` or `{asset}` | open a house for editing |
| `struct.new` | `{asset}` | start a NEW, empty structure (a well, a fence, a field): build it with `vox.*` / `slot.*` in its own frame (y = 0 the ground, x/z centred), then `struct.save` writes both files and `ref.place` puts it down |
| `struct.save` | `{struct?}` | write it and re-stamp every copy |
| `struct.close` | `{discard?}` | stop editing (refuses unsaved edits unless `discard`) |
| `struct.revert` | `{struct?}` | throw away unsaved edits |
| `vox.set` | `{pos, mat}` | one voxel (`"air"` erases) |
| `vox.set_cells` | `{cells: [[x, y, z, mat], ...]}` | a list of voxels |
| `vox.brush` | `{pos, radius, shape?: "sphere" or "cube", mat}` | a ball / block |
| `vox.box_fill` | `{min, max, mat}` | fill a box |
| `vox.clear` | `{min, max}` | empty a box |
| `vox.box_hollow` | `{min, max}` | empty the inside, keep the faces |
| `vox.box_shell` | `{min, max, mat}` | the six faces in a material |
| `vox.replace` | `{min, max, from, to}` | swap one material for another in a box |
| `vox.line` | `{from, to, mat, radius?}` | a beam |
| `vox.copy` | `{min, max}` | to the clipboard |
| `vox.paste` | `{pos, rot?: 0-3, mirror?: "none", "x" or "z", air?: "keep" or "skip"}` | the clipboard, min corner at pos (mirrored, then turned) |
| `vox.move` | `{min, max, by}` | move a box of voxels |
| `slot.add` | `{name, kind, pos, yaw?, props?}` | a slot in the open house |
| `slot.move` | `{name, pos?, by?, yaw?}` | move / turn a slot (its boxes go with it) |
| `slot.set_prop` | `{name, key, value}` | a slot prop |
| `slot.remove` | `{name}` | remove a slot |
| `batch` | `{cmds: [{cmd, args}, ...]}` | several commands as one undo step |
| `undo` / `redo` | — | as Ctrl+Z / Ctrl+Y |

Every vox / slot command also takes `"frame": "world"` and `"struct": "<asset>"`
(a house that is loaded but not the open one). `mat` is a `materials.json`
name (`"cobble"`) or id.

### Troubleshooting

- **A voxel tool is greyed out** — open a house first (double-click it).
- **"opened by asset name"** — the house was opened without a placed copy
  (a script's `{"asset": ...}`), so there is nothing in the world to click;
  open a placed copy instead.
- **My edit shows but the other copies of the house did not change** — they
  update when you save (Ctrl+S).
- **The far world looks unedited / cannot be clicked** — the loaded window
  stays around your body (where you pressed F8, about 25 m each way); leave
  the editor, walk there and press F8 again.
- **Picking goes through a tree or a rock** — the editor's clicks see houses,
  the ground and the placed things, not every voxel of the world.
- **Check it works:** `--selftest --gate editor-commands` (every command,
  undo-all byte-identical, redo-all byte-identical, a refused script changes
  nothing) and `--gate editor-struct-edit` (a turned house edited in the
  world, saved, re-stamped, equal to a fresh world). `--shot-editor` writes
  three pictures of the editor.

---

## 14. Building a village: Harrowby, step by step

Harrowby is the hamlet just north of where a new game starts: three houses
round a green, a well, a barley field against the treeline, and five people
with homes and days. Everything in it was made with the tools in §9–§13, and
every step is a file you can read in `assets/worldmap/default/scripts/`, in
number order:

| File | What it did |
|---|---|
| `harrowby_00_ground.sh` | the forest clearing, and the levelled ground (the map's sculpt layer) |
| `harrowby_*.params.json` | the house generator's settings for the three houses |
| `harrowby_01_place.jsonl` | put the three houses on the ground |
| `harrowby_02_smithy.jsonl` | Osric's forge bay, anvil, trough, tool rack, strongbox |
| `harrowby_03_longhouse.jsonl` | the byre, the open hearth, the board, the loft stair and rail |
| `harrowby_04_alehouse.jsonl` | Agnes's trestle, barrels, cask rack, cellar chest, the regulars' places |
| `harrowby_05_green.jsonl` | the green and its well, built from nothing |
| `harrowby_06_field.jsonl` | Edric's barley field, fence and scarecrow |
| `harrowby_07_people.jsonl` | the paths between the doors (waynodes), the yard marker, the five villagers |
| `harrowby_08_paths.sh` | the gravel and dirt paths painted on the ground |

The `.jsonl` files are edit scripts (§13): `#` lines are comments, one command
per line. You never need to run them again — the result is already in the
files below — but they are the exact record of what was done, and you can
copy any line into your own script.

**Where it is** (world voxels, 10 to the metre; north is -z):

```
         z 3131  . . . . . . clearing, north edge . . . . . .
         z 3133        Edric's spot, the field's north gate
         z 3170  [ field: harrowby/field, barley + scarecrow ]
         z 3212           south gate  |  lane (dirt)
         z 3340  [ longhouse: harrowby/longhouse, door faces south ]
                    byre door (west)          back door (north) -> field
         z 3378                    | gravel
  alehouse ------- gravel ------ [ well ] ------- gravel ------- smithy
  harrowby/alehouse           harrowby/green              harrowby/smithy
  x 415, door faces east      (555, 3467)                 x 700, forge bay faces west
                                    |
         z 3518  . . . . . . clearing, south edge . . . . . .
                        (the wood)
         z 3620                   spawn (580, 3620)
```

The files that make it up:

- `assets/worldmap/default/refs/harrowby.json` — the placed things: five
  structures, five villagers, the outdoor waynodes, the smithy-yard marker.
- `assets/structures/harrowby_{longhouse,smithy,alehouse,green,field}.vox` +
  `.struct.json` — the buildings; their doors, beds, chests, hearths and the
  places people stand are SLOTS in these files (`harrowby/smithy/anvil`, ...).
- `assets/worldmap/default/map.json` — the forest clearing `harrowby_clearing`
  (§14.1: no tree's crown over that box, the wood thinning out round it) and
  `editLayer: "default_ground"`.
- `assets/worldmap/default/sculpt.svsculpt` — the levelled ground.
- `assets/worldedits/default_ground.svedit` — the painted paths.
- `assets/schedules/{edric,maud,osric,agnes,wat}.json`,
  `assets/dialogue/{edric,maud,osric,agnes,wat}.json` — their days and their
  talk.

### Adding a fourth house, the way the first three were made

Say a cottage for a woodcutter, **Hob**, west of the field.

**1. Make the house** (§11). Environment → Structures → **new from…**
`cottage`, move the sliders (a woodcutter: plank walls, a chimney, one bed,
one chest), **Save as…** `harrowby_cottage`. Or from a terminal, with a params
file like `harrowby_smithy.params.json`:

```bash
node scripts/bake_structure.mjs harrowby_cottage my_cottage.params.json
```

**2. Find it room.** A house levels a SQUARE of ground as wide as its longest
side plus its `padMargin` (4 voxels in Harrowby), and two squares that touch
are a warning (the References page's warnings list names both). The stats line
on the Structures page gives the house's size; a 7 m cottage needs a clear
square about 7.5 m across. Then **widen the clearing over it** (§14.1): the
forest keeps its crowns out of the clearing's box and nowhere else, so a house
outside the box gets a tree through its roof (the gate counts those). The
levelled ground is at 200, so its floor is **201**; outside the flattened area
use **move to crosshair** (§12).

**3. Place it** (§12). In game: F1 → World → References → **place a
structure**, pick `harrowby_cottage`, id `harrowby/cottage`, turn it with
**-90 / +90** until the red box (the door side) faces where people will come
from, **confirm**. Or in F8 with the **Place** tool, or one script line:

```
{"cmd": "ref.place", "args": {"id": "harrowby/cottage", "kind": "structure", "base": "harrowby_cottage", "pos": [420, 201, 3230], "yaw": 90, "props": {"padMargin": 4}}}
```

**4. Make it his** (§13, F8). Double-click the house to open it. Use Box to
fill a chopping block, Line for a saw-horse, Pencil for the axe on the wall.
Look at its slots while it is open (they are drawn): **the generator's walking
nodes do not know about your furniture** — if a `waynode_*` now sits in the
table or behind it, select it and drag it clear (or `slot.move`). Villagers
walk STRAIGHT from node to node (they only find their own way round things
near the player), so every drawn line between two nodes must be clear of
furniture, and a door's inside node must be out of the leaf's swing. Give the
chest its contents in the inspector (`items`, `title`, `locked`). **Ctrl+S**.

**5. Join it to the village.** Its door has an outside node,
`harrowby/cottage/waynode_front_0_out`. With the **Link** tool click that, then
the nearest village node (`harrowby/lane_back` for this spot); a link works
both ways and is written in the village file, so the house file never changes.
Check the drawn line does not cross a wall or a fence.

**6. The villager** (§10.2). Place a ref of kind **npc**, id `harrowby/hob`,
base `human` or one of the characters (`brug`, `sherp`, …), and set its props:
`name` "Hob", `home` `harrowby/cottage`, `bed` `harrowby/cottage/bed_0`,
`work` (a marker you place where he chops, facing the woodpile), `schedule`
`hob`, `dialogue` `hob`, `outfit` e.g. `["smock#6B5A3E", "breeches", "clogs"]`.
If he should drink at Agnes's of an evening, give him his own place there: open
the alehouse, add a marker slot (`seat_hob`), and set Hob's prop `alehouse` to
`harrowby/alehouse/seat_hob` — two people sent to one marker shoulder each
other off it all evening.

**7. His day** (§10.2): the schedule table under the villager, **save** as
`hob` → `assets/schedules/hob.json`. Rows name `bed`, `home`, `work`, a tag
(`well`, `green`, `alehouse`, `smithy_yard`) or a ref id.

**8. His talk** (§9): the tuner's Dialogue tab → new file `hob`. Read what the
others set (`edric_lights`, `wat_asked`, ...) so he can have an opinion about
them.

**9. A path to his door.** The ground between the houses is the map's, not a
house's, so the F8 voxel tools do not reach it. Paint it:

```bash
node scripts/paint_ground.mjs default --mat dirt --width 12 --ragged 3 --path 470,3292 430,3250
```

Add the line to `harrowby_08_paths.sh` so the record stays complete. (The
World map page's voxel view paints the same layer by hand.)

**10. Try it.** In game, select Hob on the References page and press the
**jump clock to** buttons: he should walk each row, open his door and shut it
behind him. Then the gate:

```bash
bash scripts/run.sh ./build/Release/sandvox.exe --selftest --gate village-harrowby
```

It loads the game's map, checks every file loads with no warnings, every
schedule row of every villager resolves and has a waynode route from home,
spawns everyone, runs a fast day (the clock waits at each change of row until
everyone has arrived), and checks every door ends the day shut and nobody got
hurt. The day's trace is PINNED: after any change to the village it will say
the trace differs from the pin — that is expected. If everything else passed,
record the new day with

```bash
bash scripts/run.sh ./build/Release/sandvox.exe --selftest --gate village-harrowby --rebaseline
```

Pictures of the day: `SANDVOX_HARROWBY_SHOTS=build/hb` on the same command
writes `build/hb_overview.bmp`, `_green`, `_osric_at_work`, `_smithy`,
`_longhouse`, `_byre`, `_alehouse`, `_field`. A conversation, on the real
map: `SANDVOX_SHOT_TALK=harrowby/wat bash scripts/run.sh ./build/Release/sandvox.exe --shot-dialogue`
(`SANDVOX_SHOT_TALK_AT=1080` for 18:00).

### 14.1 A clearing in the forest (the World map page's Clearing tool)

A **clearing** is a box on the map that no forest tree's crown reaches over;
past it the trees thin out across a **feather** band, so the wood's edge is
ragged rather than ruled. It changes nothing else: the ground keeps its shape
and its grass and flowers, ponds stay, and trees you placed by hand (the Tree
tool) still stand. Harrowby sits in one; use one for any village, camp or glade.

1. Tuner → Environment → **World map**. Zoom in on the place (wheel) until one
   screen pixel is a few voxels — the box snaps to the column under the cursor.
2. Pick the **Clearing** tool and **drag** a box over everything that must stay
   open: the houses, the green, the field. Lime outline = the box; the dashed
   line outside it = the feather band. `clearing feather` in the toolbar sets
   the band for new boxes (64 voxels = 6.4 m is a natural edge; 0 is a hard one).
3. Fine-tune in the **Sites** panel: select the clearing (click inside it with
   the Clearing or Select tool) and type its exact corners (`min x`, `min z`,
   `max x`, `max z`, world voxels) and its `feather`. The panel says the size.
4. Drag inside a clearing to move it; **Shift+click** inside it (or × in the
   Sites list) deletes it; **Ctrl+Z** undoes any of it. **Save map**, then F7 in
   the game (or Apply) regenerates the world.

How big: trees are kept back by their OWN crown's width, so the open ground you
see is the box plus a few metres (a bush's 1.8 m, an oak's 5.4 m, a great oak's
11.5 m) plus the ragged band. So draw the box just round what must stay open —
Harrowby's is its buildings' outline plus 2 voxels.

Keep the **spawn** outside every clearing if a new game should wake among trees:
the Load check panel warns when it is inside one. (A clearing whose corners are
the wrong way round is refused, naming it.) Not the **Pad box**: that is the
selftest's fixture ground — it bares the grass too and flattens the hills, which
is what put Harrowby on bald, jittered ground before it had a clearing.

The gate `village-harrowby` counts tree voxels inside every house (want 0);
`clearing` tests the site kind itself. The look, from where a new game starts:

```bash
SANDVOX_SHOT_SPAWN_LOOK=557,3440 bash scripts/run.sh ./build/Release/sandvox.exe --shot-spawn
```

writes `screenshot_spawn.bmp` (standing at the spawn, looking at that column),
`screenshot_spawn_back.bmp` (turned round) and `screenshot_spawn_high.bmp`
(from above and behind); `SANDVOX_SHOT_SPAWN_AT=<minutes>` sets the hour.

### When it goes wrong

- **"… and site … overlap"** in the warnings — two squares touch (step 2):
  move one house, or lower its `padMargin`.
- **A villager "never reached its anchor"** in the gate — the line names where
  it stood, which way it was going and its last notes ("no progress toward
  harrowby/alehouse/waynode_bar"). Almost always a node line through
  furniture, a node inside a door's swing, or two people sent to one spot.
- **"lost blood over an ordinary day"** — something in the village hurts
  people: a door that hit them was one (fixed in the engine), a stair they
  fall off another (the longhouse loft has a rail for that reason).
- **The ground round a house is a step or a ditch** — the house's floor is not
  one above the ground there. On the levelled clearing use 201; elsewhere use
  **move to crosshair** on the ground (§12), or level the ground first:
  `node scripts/sculpt_flatten.mjs default --rect x0,z0,x1,z1 --y <ground> --feather 96`
  (the World map page's sculpt brush edits the same layer).
- **"tree-material voxels inside its stamped box"** in the gate — the forest
  reaches into that house: widen the clearing over it (§14.1).
- **The village moved out of the gate's window** — the gate centres a 51 m
  window on Harrowby's refs; a house much further out than the field needs its
  own place, not this group.
