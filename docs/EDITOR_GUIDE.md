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
   (`"Strongbox"`) changes what the prompt and the panel call it.
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
