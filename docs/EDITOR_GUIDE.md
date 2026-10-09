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
| `grant` glyph | the player now owns that spell glyph, by its glyphs.json id (`summon_skerrick`). Saved with the kit. A glyph that does not exist yet is a warning and does nothing |
| `learn` name | the world now knows that name: sets the flag `name:<name>` (read it with `flag name:skerrick`) |
| `present_contract` | a DEMON's conversation only: opens the picker of the player's contract pages (and the stock ones) over the panel. Picking one presents it; the conversation reopens on the demon's answer |
| `release` | a demon's conversation only: lets it out of the circle, weighed with its contract (strength >= power + weight) |
| `dismiss` | a demon's conversation only: sends the demon home |

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
| `kind` | what sort of thing: `marker`, `npc` (§10.2), `waynode` (§10.2), `door`, `container`, `bed` (§10.1), `readable` (§10.3; a book), `structure` (§12) |
| `base` | what it's an instance of — for an `npc`, the mob def (`human`, `dummy`, ...) |
| `pos` | world position in voxels (10 voxels = 1 m); the crosshair readout on F1 shows cells. In a **locale**'s group (a group named like one of the map's `locales`, e.g. `harrowby`) the FILE stores `y` relative to the locale's anchor, so the place moves with its ground; the game, F8, the References page and edit scripts all show and take world `y` |
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

### 10.3 How do I add a book (something to read)?

A **readable** is a ref that reads as a conversation. The book itself is
voxels you built: a lectern, a few leather and linen cells. The ref only says
"this cell is a book, and these are its pages".

1. Write the pages as a conversation (§9), e.g. `assets/dialogue/osric_notes.json`.
   Set the file's `speaker` to what the header should say ("Osric's daybook").
   Each node is a page and [continue] turns it. A page's `do` can `grant` a
   spell glyph or `learn` a name.
2. Place a ref of kind **readable** on the book's cell (F8 → Place →
   `readable`, or a line in `refs/<group>.json`):

   ```json
   { "id": "harrowby/smithy_notes", "kind": "readable", "pos": [676, 182, 3461], "yaw": 90, "props": { "title": "Osric's daybook", "dialogue": "osric_notes" } }
   ```

   - `dialogue` is required.
   - `title` is what the prompt says: "G  Read Osric's daybook".
   - `verb` replaces "Read" ("Study", "Look at").
3. Look at it in game and tap **G**. The prompt answers when the crosshair
   passes within about 0.4 m of the cell.

A readable saves nothing of its own. What its pages DO (flags, items, glyphs)
saves the way those always do.

**The cellar under the Harrowby smithy** is the worked example. It is too far
under the floor for a house blueprint (a house can only sink 8 cells), so it
lives in Harrowby's own voxel layer, `locales/harrowby/ground.svedit` (§15.5).
`node scripts/paint_cellar.mjs` owns those columns: edit the plan at its top
and run it again. It writes the map's edit layer, so follow it with
`python assets/worldmap/default/scripts/harrowby_09_locale.py`, which moves
the cellar's columns into the locale's layer.
Walk in through the smithy's door, go to the back of the bedroom past the
bed, and take the stairs down beside the rail. The book is on the lectern
west of the salt ring. `--selftest --gate harrowby-cellar` checks all of it.

### 10.4 How do I write a demon's conversation?

A demon's def (`assets/demons/<name>.json`) names its conversation with
`"dialogue": "demon_skerrick"`; the file is an ordinary conversation in
`assets/dialogue/` (§9), opened when the player presses **T** beside the
demon while it is CONTAINED in a salt circle (and ended the moment it is
not). Four things are special:

- **The engine sets two flags before it opens**, so the `entry` list can pick
  the right face: `demon:present` (1 = the last contract was refused, 2 = it
  bound the demon, 3 = the player could not carry the upkeep, 0 = none) and
  `demon:bound` (1 while bound). The checks know the engine writes `demon:*`.
- **Text may say** `{tell}` (a line from `assets/demons/tells.json` for the
  current margin), `{name}`, `{margin}`, `{weight}`, `{contract}`.
- **Three actions** (§9's table): `present_contract`, `release`, `dismiss`.
  `release` and `dismiss` end the conversation; give `present_contract` a
  node to go to (the picker shows while it is up).
- **The gaze**: while it runs the camera is locked on the demon; L looks away.

`assets/dialogue/demon_skerrick.json` is the worked example.

**Contracts** are not dialogue: the player drafts them in game (Tab → the
spellbook's header → **contracts**). The STOCK pages everybody starts with
are `assets/demons/contracts.json`, and what a clause costs is
`assets/demons/contract_tariff.json` (both reload with R). The language is in
`src/game/contract.h`'s header comment.

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
| `pos` | where the house's **front-door floor** goes: x and z are the middle of the house, **y is the floor you walk on**. The ground around the house is levelled so its top is one voxel below that. In a LOCALE's group file (§15.5) `y` is stored relative to the locale's anchor; this page, F8 and edit scripts show and take the world y. |
| `yaw` | which way the front door faces: `0` = +Z, `90` = +X, `180`, `270`. **Only quarter turns** — anything else is refused with a warning and the house is not built. |
| `props.padApron` | optional: how many voxels round the walls stay dead level with the floor -- the doorsteps (default 8 = 0.8 m; 0..32) |
| `props.padMargin` | optional: how many voxels the levelled ground takes to ease (a smooth S-curve) back into the terrain past the apron (default 24 = 2.4 m; 1..64; widened automatically where the ground is too far from the floor for that to stay one voxel a column) |

No forest tree grows whose crown would come within 1 m of the house (§14.1), so
a house needs no clearing. Both pad props are sliders in the inspector ("ground
round the house").

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
- **"N sites reach map cell … a cell holds at most 128"** — that many houses,
  lakes, authored trees and road pieces within one 102.4 m site cell; spread
  them out.
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
| `harrowby_00_ground.sh` | the village's ground: nothing to run -- it says where the softened ground, the houses' ramps and the tree rule live |
| `harrowby_*.params.json` | the house generator's settings for the three houses |
| `harrowby_01_place.jsonl` | put the three houses on the ground |
| `harrowby_02_smithy.jsonl` | Osric's forge bay, anvil, trough, tool rack, strongbox |
| `harrowby_03_longhouse.jsonl` | the byre, the open hearth, the board, the loft stair and rail |
| `harrowby_04_alehouse.jsonl` | Agnes's trestle, barrels, cask rack, cellar chest, the regulars' places |
| `harrowby_05_green.jsonl` | the green and its well, built from nothing |
| `harrowby_06_field.jsonl` | Edric's barley field, fence and scarecrow |
| `harrowby_07_people.jsonl` | the paths between the doors (waynodes), the yard marker, the five villagers |
| `harrowby_08_paths.sh` | the gravel and dirt paths, once painted on the ground; since map v2 they are `roads[]` in `map.json` (§15.2) and this script only erases the old paint |
| `harrowby_09_locale.py` | made the village a **locale** (map.json `locales`): its refs' `y` became relative to the anchor at the green, and the cellar's ground cells moved into the locale's own layer. Re-runnable |

The `.jsonl` files are edit scripts (§13): `#` lines are comments, one command
per line. You never need to run them again — the result is already in the
files below — but they are the exact record of what was done, and you can
copy any line into your own script.

**Where it is** (world voxels, 10 to the metre; north is -z):

```
         z 3090  . . . . softened ground, north edge . . . . .
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
                        (the wood, among and round the houses)
         z 3620                   spawn (580, 3620)
         z 3650  . . . . softened ground, south edge . . . . .
```

The files that make it up:

- `assets/worldmap/default/refs/harrowby.json` — the placed things: five
  structures, five villagers, the outdoor waynodes, the smithy-yard marker.
  Their `y` is relative to the locale's anchor (`anchor.y` 201, the ground at
  the green): every house's file y 5 is world y 206.
- `assets/structures/harrowby_{longhouse,smithy,alehouse,green,field}.vox` +
  `.struct.json` — the buildings; their doors, beds, chests, hearths and the
  places people stand are SLOTS in these files (`harrowby/smithy/anvil`, ...).
- `assets/worldmap/default/map.json` — `harrowby_ground`, the village's
  SOFTENED GROUND (§14.1: the forest floor's hills kept at 25 %, its bumps and
  grain at 0, the tracks between the doors flatter still), the eight paths as
  `roads[]` (ids `harrowby_*`, kind `path`), the locale `harrowby`
  (`locales[]`, §15.5) and `editLayer: "default_ground"`. There is no
  clearing: the wood stands among the houses, and no tree's crown comes within
  1 m of a wall (§14.1).
- `assets/worldedits/default_ground.svedit` — the map's own edit layer, empty
  now (its paths became roads, its cellar moved to the locale's layer).
- `assets/worldmap/default/locales/harrowby/ground.svedit` — the village's own
  ground cells (Osric's cellar): the locale's edit layer, applied after the
  map's.
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

**2. Find it room.** A house levels its own footprint to its floor, plus a
flat ring round it (the doorsteps, `padApron`, 0.8 m in Harrowby), and eases
back into the ground round it with a smooth S-curve over `padMargin` (3.6 m in
Harrowby; it widens itself where the ground is further from the floor than it
can ease without a two-voxel step). Two houses on the same floor may share
ramp; a ramp that reaches the flat ring of a house on ANOTHER floor is a
warning (the References page's warnings list names both). The stats line on
the Structures page gives the house's size. You do **not** need a clearing:
worldgen leaves out every tree whose crown would come within 1 m of a wall, and
keeps trunks off the pad, its ramp and the villagers' walking lines (§14.1).
Every Harrowby floor is **206** in the world (file y 5 over the anchor's 201:
the v2 conversion lifted the village onto its new ground). Away from the
houses the ground differs; use **move to crosshair** (§12) aimed at the ground
where the door goes, and read the y it gives.

**3. Place it** (§12). In game: F1 → World → References → **place a
structure**, pick `harrowby_cottage`, id `harrowby/cottage`, turn it with
**-90 / +90** until the red box (the door side) faces where people will come
from, **confirm**. Or in F8 with the **Place** tool, or one script line:

```
{"cmd": "ref.place", "args": {"id": "harrowby/cottage", "kind": "structure", "base": "harrowby_cottage", "pos": [420, 206, 3230], "yaw": 90, "props": {"padMargin": 4}}}
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

**9. A path to his door.** A path is a road (§15.2). On the World map page
pick the **Road** tool, start on the back lane (the vertex snaps onto
`harrowby_back_lane`, making a junction) and end at his door, e.g. 470,3292 →
430,3250. In the inspector: kind `path`, surface `packed_earth`, width 12.
**Save map**, check its profile chart, F7. The engine grades it, keeps trunks
and cover off it and draws it at any distance. (Ground that is not a path, a
stone yard or a woodpile's bare patch, is the locale's voxel layer: open
Harrowby on the map page and use the 3D view, §15.5.)

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

### 14.1 The ground and the trees of a village

Harrowby stands in the forest the way a hamlet does: trees between the houses,
the ground rolling a little, level where the houses are, and the tracks between
the doors flatter than the green round them. Three things make that, and all
three are yours to change.

**The trees: automatic.** No tree grows whose CROWN would come within 1 m of
any building (every placed structure, on every map -- the crown's full width,
so a tall tree does not overhang a low roof either); no trunk stands on a
house's pad or its ramp; and no trunk stands within 1.5 m of a line a villager
walks (every waynode link of the map's refs, the houses' own door nodes
included). So big oaks stand back from the walls and bushes, birches and pines
come closer. To keep a spot open that is not a house -- a field, a glade --
use a clearing (below). To have a particular tree right beside a house, place
it with the Tree tool (the rule is for the forest's own trees). Moving a
waynode link moves its keep-out after the next F7 (Apply).

**The ground: softened.** Environment → **World map** → the **Soften** tool.
Drag a box over the village. Inside it the forest floor keeps a fraction of
its own relief, easing back to the untouched forest across the dashed feather.
Select it (click inside it) and set, in the **Sites** panel:

| field | what it keeps | Harrowby |
|---|---|---|
| `hills %` | the 12.8 m swells: how rolling the village is | 25 |
| `bumps %` | the 3 m bumps: what makes a walk lumpy | 0 |
| `grain %` | the sub-metre grain: the one-voxel steps and pits | 0 |
| `paths %` | the swells kept ON the villagers' routes (1.2 m either side of each waynode link, easing out by 3.6 m) | 10 |
| `feather` | how far past the box it eases back, voxels | 120 |

100 everywhere is the biome's own ground; 0 hills is the flat level of the
calm home area. The authored height plane (the hillside the village is on)
is never touched. **Save map**, then F7 in the game (or **Apply**).

**Each house: its pad.** Select the house (F8, or F1 → World → References) and
under **ground round the house**: `flat apron` (voxels of level ground round
the walls -- the doorsteps, so every door meets the ground at its threshold)
and `ramp` (how wide the S-curve back to the ground is; wider is gentler).
Let go of the slider and the house re-applies.

**A clearing** (the **Clearing** tool) is a box no forest tree's crown reaches
over; past it the trees thin out across a **feather** band, so the edge is
ragged. It changes nothing else: the ground keeps its shape and its grass and
flowers, ponds stay, and trees you placed by hand still stand. Use it for an
open field, a glade or a meadow -- not for a village, which has its houses'
own rule. Drag a box; fine-tune its corners and feather in the Sites panel;
drag inside one to move it; **Shift+click** deletes it; **Ctrl+Z** undoes.
Trees are kept back by their OWN crown's width (a bush 1.8 m, an oak 5.4 m, a
great oak 11.5 m), so the open ground is the box plus a few metres. Keep the
**spawn** outside every clearing if a new game should wake among trees (the
Load check panel warns). Not the **Pad box**: that is the selftest's fixture
ground -- it bares the grass and flattens the hills.

The gate `village-harrowby` counts tree voxels inside and over every house
(want 0); `structure-ground` tests the tree rule, the pad and the soften box
themselves; `clearing` tests clearings. The look, on foot, from where a new
game starts:

```bash
SANDVOX_SHOT_SPAWN_LOOK=557,3440 bash scripts/run.sh ./build/Release/sandvox.exe --shot-spawn
```

writes `screenshot_spawn.bmp` (standing at the spawn, looking at that column),
`screenshot_spawn_back.bmp` (turned round) and `screenshot_spawn_high.bmp`
(from above and behind); `SANDVOX_SHOT_SPAWN_AT=<minutes>` sets the hour. The
village gate's pictures (`SANDVOX_HARROWBY_SHOTS=build/hb`) include three on
foot from the green (`_ground_north`, `_ground_west`, `_ground_east`) and two
doorsteps (`_door_longhouse`, `_door_smithy`).

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
- **The ground round a house is a step or a ditch** — the house's floor is
  far from the ground there. Widen its `ramp` (it widens itself as far as 64
  voxels to avoid a two-voxel step), or put the floor nearer the ground with
  **move to crosshair** (§12).
- **The village looks lumpy or pitted** — lower the soften box's `bumps` and
  `grain`; a lower `hills` flattens the swells. The tracks are roads in solid
  `packed_gravel` / `packed_earth` (§15.2), so they no longer shed grains; a
  dark pit in one is something dug or burnt in play.
- **"tree-material voxels inside its stamped box"** in the gate — a tree
  reaches into that house, which the building rule exists to refuse: a Tree
  site placed there by hand, or a bug.
- **The village moved out of the gate's window** — the gate centres a 51 m
  window on Harrowby's refs; a house much further out than the field needs its
  own place, not this group.

---

## 15. Designing the world: the World map page (map v2)

Environment → **World map** is where the whole world is drawn: its height,
what grows where, its roads, its named places and the hand-made spots in it.
Everything on the page is the map named in the **map** selector at the top
left (that selector also writes `tuning.json world.mapLayer`, the map the
GAME loads; `SANDVOX_MAP=<name>` overrides it for one run). A map is a folder:

| File | What it holds |
|---|---|
| `assets/worldmap/<name>/map.json` | version 2: the extent, the planes' settings, sea level, the biome palette, the `terrain` noise, and the vector layers: `sites[]`, `roads[]`, `regions[]`, `markers[]`, `ridges[]`, `locales[]`, `editLayer`; editor-only `ranges[]`, `gen`, `climate` (§15.1e) |
| `planes/height/<tx>_<tz>.svt` | the HEIGHT plane: absolute heights, one post every **6.4 m** |
| `planes/biome/<tx>_<tz>.svt` | the BIOME plane: one biome per **25.6 m** cell |
| `sculpt.svsculpt` | the fine height layer (0.8 m samples), on top of everything |
| `refs/<group>.json` | the placed things (§10, §12) |
| `locales/<id>/ground.svedit` | a locale's own voxel cells (§15.5) |

A tile file holds 256 × 256 samples, so one height tile covers 1.6 km and one
biome tile 6.6 km. A tile that is not there is "every sample is the plane's
default" (`planes.height.default`, `planes.biome.default`): the open ocean
costs nothing. **Save map** writes map.json and only the tiles you changed (a
tile painted back to the default is deleted). Every save moves the world hash;
a running game sees it on **F7** (or **Apply** in the Environment sidebar).

There is no "new map" button. Start a map by copying a folder
(`assets/worldmap/default` minus its `refs/`, `locales/` and `scripts/`), by
writing a generator like `scripts/gen_map_peaks.py`, or from a v1 map with
`python scripts/convert_map_v2.py <name>` (v1 = `map.json` + `map.svmap`; the
engine refuses a v1 map and names the converter).

**The limits** (the loader refuses past them):

| Limit | Value |
|---|---|
| height | ±32,000 voxels = ±3.2 km (absolute Y; the sea is `seaLevelY`, 112 on the shipped maps) |
| plane size | 4,097 posts a side = ~26 km at 6.4 m, and 64 MiB packed |
| extent | a multiple of 1,024 voxels (the 102.4 m site cell) on both axes |
| biome warp | `warpAmpVox` ≤ a quarter of a biome cell (64 voxels) |
| sites per site cell | 128 (a 102.4 m square; road pieces count) |

Units: the page speaks metres; map.json speaks world voxels (10 to the metre,
north is −z). The **hover readout** under the map gives x / z in metres, the
authored height (in metres, its y, and above the sea), the biome, the tile,
and the engine's real ground when the backdrop is loaded.

### 15.1 Topographic editing: the height plane

The height plane IS the shape of the world. A post's value is exact: put a
summit at 1,100 m and the ground there is 1,100 m, give or take the noise. The
engine reads the posts with a Catmull-Rom spline (smooth slopes, no steps at
post edges), adds the map's seeded noise on top (the **Terrain** panel:
range, hills, detail, grain, each scaled per biome), then the sculpt layer.
The noise is texture; it never scales or overrides what you painted.

**The display.** The canvas is a topographic map of the plane itself, drawn
without the engine: **topo** (a colour ramp over the plane's real lowest and
highest posts, sea in blues), **shade** (a hillshade from the north-west),
**contours** (10 / 25 / 50 / 100 m, or auto by zoom; every 5th line bold; the
0 m line is the coast) and **biomes** (the biome plane as a coloured overlay,
slider = opacity). Right- or middle-drag pans, the wheel zooms, **Fit** frames
the extent.

**The real-terrain backdrop.** **Real terrain** lays the engine's own
`--heightmap` of the view over the map at the opacity of the slider beside it:
authored height + noise + sculpt + roads + sites, for the SAVED files. Zoomed
in, it follows the view. Use it to see what the noise did to a slope you
painted; press it again to hide it. It is a picture of the last save, so save
first.

**The Height brush** (left-drag). Shared with the Biome brush: **radius**
(logarithmic, 1 m to 5 km; `[` and `]` resize) and **spacing** (dabs every
N % of the radius along a drag, so mouse speed does not change the result).
Then:

| Control | What it does |
|---|---|
| **mode** `raise` / `lower` | move posts up / down; **str** = strength % of radius/10 per dab at the centre (a 1 km brush at 50 % = 50 m a dab) |
| `smooth` | toward the local average |
| `flatten` | toward the height where the stroke started |
| `set` | toward a typed height, **to … m** ABOVE SEA (Alt+click picks the height under the cursor into it) |
| `terrace` | quantises into shelves: **step** (m, from the sea) and **sharp** % (0 = soft steps, 90 = a 90 % flat shelf and a 10 % cliff) — the cliff tool |
| `noise` | seeded relief: **amp** (m), **size** (largest feature, m), `ridged` (crests) or `fbm` (rolling), **seed** |
| `erode` | water + scree erosion under the brush |
| **edge** | the falloff: `smooth`, `linear`, `flat` (hard core, short soft rim), `sphere` |
| **mask** | where every mode may act: `no mask`, `land only` (above the sea), `sea only`, `height band` (between two heights in m above sea) or `slope band` (between two slopes in degrees), each soft over **soft** m / degrees. Raise the sea floor without touching the coast, paint rock only on the steep ground, smooth only the valley floors. The mask reads the posts as they were before each dab |

A stroke is one undo step (**Ctrl+Z** / **Ctrl+Y**, or the buttons) and only
remembers the tiles it touched, so a 5 km stroke undoes as cheaply as a 5 m
one.

**The Shape tool** (one click, one undo step): stamps a landform into the
plane: `peak`, `ridge`, `basin`, `volcano` (added to the land) or `plateau`,
`mesa` (level it to a flat top). Set the height in m and whether it is a
**rise** above the land at the centre or the **summit** height above sea (a
basin's floor), the radius (`[` `]`), **aspect** (1 = round, 0.25 = a long
ridge) and **rot** (`,` `.`). These are the old v1 landform sites; they are now
edits to the plane, not things the engine reads.

**Ridge and valley lines** (the **Line** tool). Click to draw a polyline
(double-click or Enter ends), drag a vertex, click a segment to insert one,
shift+click a vertex to delete it. The tool bar sets what a NEW line takes:
`ridge` (raises toward its crest) or `valley` (lowers toward its floor), its
cross-section (`V`, `U`, `round`, `sharp`), its **half-width** in m, and each
new vertex's height (`sea` = m above sea, or `ground` = the plane under it ±
the rise). In the **Height tools** panel each line has a vertex table and
**all to** (every vertex to one height), **ramp ends** (the inner vertices on a
straight grade between the ends), **from plane ±** (each vertex that far
above / below the plane under it now), and **Bake**: that writes the line into
the plane. The line itself is kept in map.json `ridges[]` so you can change it
and **Rebake**; the engine never reads it, only the posts it baked. **Unbake**
takes the bake back out and keeps whatever you painted since. The limit:
rebake / unbake work from a snapshot held by the page, so they only reach a
bake made in this session. After a reload, Bake bakes on top of the old one;
Ctrl+Z, or paint it out, if that is not what you want.

**Erosion** (the **Area** tool, then the Height tools panel). Drag a rectangle
(shift+click clears it), pick the **method** and press **Erode area** (with no
Area the stream-power methods erode the **whole map**):

| method | what it does |
|---|---|
| `stream power: landscape` | the land is grown again under an uplift shaped like itself while rivers cut it (the Range tool's erosion), then its heights are put back: branching valleys up to every ridge, the summits where they were. The big one: use it on a template, a painted mountain, a whole map |
| `stream power: incise` | rivers cut the ground as it is (no regrowth): gentler, keeps the shape you painted, deepens the valleys that are already there |
| `droplet + thermal` | the old particle erosion: **passes**, **water** (droplets: gullies and fans) and **thermal** (slopes over **talus °** slump into scree). An Area only, at most ~2048 × 2048 posts |

**strength %** and **seed** apply to all three. The stream-power methods run on
a coarse copy of the plane (25.6 m for an Area, 51.2 m for the whole map) and
bring the CHANGE back to the 6.4 m posts, so the detail you painted stays; the
window's edge is feathered into the land around it. It all runs in the
background (the **working** bar under the toolbar shows the progress and has
**Cancel**) and shows the result in the plane; **Commit** keeps it (one undo
step), **Discard** puts the posts back, and the next edit of any kind commits
it. The same seed on the same posts gives the same result.

**PNG import and export** (Gaea, World Machine, QGIS, …). In the Height tools
panel, **Export height PNG** writes the Area (or the whole map if there is no
Area) as a 16-bit greyscale PNG, exact: 1 unit = 1 voxel, value 0 = the
lowest post, and the mapping rides in the file. **Export biome PNG** writes the
biomes in the palette colours. **Import height PNG…** reads a 16-bit (or 8-bit)
greyscale heightmap and opens a dialog: **onto** (the PNG's own posts, for a
file this page exported; the Area; or the whole map), **metres per unit**
(Gaea: the terrain's height ÷ 65535) and **value 0 is at … m above sea**; the
dialog shows the height range that gives you. A file the same size as the
target goes in post for post; any other size is resampled. **Import biome
PNG…** maps every colour to the nearest palette biome and lists the colours it
could not match (tick "leave unmapped cells as they are" to keep the old
biome there). **Apply import** writes it as one undo step. The codec is
`scripts/map_png.py` (`python scripts/map_png.py info <file.png>` says what a
file holds). Plain Python, so a full-map Paeth-filtered PNG takes ~10 s to
read.

**Biomes.** Click a biome chip under the tool bar (the palette is map.json
`biomes`, names of `assets/biomes/` files), then paint with the **Biome
brush**; Alt+click picks the biome under the cursor. Biome boundaries wobble
by `warpAmpVox` (seeded), so a 25.6 m cell edge never reads as a straight
line.

**What follows the height by itself** (nothing to paint): the treeline is a
FRACTION of the map's height above the sea, scaled per biome
(`terrain.treelineScale` in the biome file) and jittered by
`terrain.treelineJitter`, so one biome works on a 20 m harness and a 1,400 m
range; snow, trees and cover rows read it. The climate cools with altitude
(`sim.heatLapsePerKm`, 6 heat units per km above the sea), and snow falls
above the freezing level. A
column steeper than `terrain.rockSlope` (2.5:1 shipped) is bare rock: no
powder, no snow cap, no grass. Sediment fills valleys relative to the authored
ground, so a high valley gets soil like a low one. Caves stay under the
column's own ground, and no ground within 20 m of the magma table floods with
lava.

### 15.1a Mountain ranges: the Range tool

Spamming the raise brush makes blobs. A real range has a crest that rises
and falls in peaks and cols, side ridges (spurs) running down its flanks with
valleys between them that branch like trees, foothills fading out into the
plain, and rock that rivers have cut. The **Range** tool draws all of that
from a few clicks.

**Drawing.** Pick **Range** and click along the crest you want; each click is a
**knot** (double-click or Enter ends). The tool bar sets what a NEW knot
gets: its **crest** height (m above sea; a later knot copies the one before it)
and its **half-width** (crest to the foot of the mountains, m; `[` `]` scale
it), and the **profile**. On the map a range is an amber crest line with a
triangle per knot and dotted lines at its feet; the selected range shows each
knot's height and a small **diamond** beside each knot. Drag a knot to move it,
**Alt+drag** it up or down to set its crest height (5 m a pixel), drag its
diamond to set that knot's half-width, click the selected crest to
insert a knot there, shift+click a knot to delete it. The crest is a smooth
spline through the knots (the same centripetal Catmull-Rom the engine uses for
smooth roads and rivers, `assets/editor/spline.js`).

**The Ranges panel** (side bar) lists every range; select one for its
inspector:

| control | means |
|---|---|
| `profile` | the cross-section: `alpine` (steep upper walls, concave flanks: the default), `sharp` (a knife edge), `round` (an old, worn range), `massif` (a broad high block with steep edges) |
| `seed` | the same knots + seed = the same range; another seed = another range on the same line |
| `peaks` | how much the crest rises and falls along its length: peaks at the knots' heights, cols between (0 = an even crest) |
| `spurs` | the side ridges and the valleys between them |
| `foothills` | how far rough hills reach out past the foot, and how high |
| `erode` | the stream-power erosion: the branching valleys cut into the range and the jagged crest of peaks and cols (0 = the designed shape alone) |
| `wander` | how far the crest line and the spurs bend away from the drawn spline |
| the knot table | each knot's crest (m above sea) and half-width (m); **all crests to** / **all widths to** set every knot |

With **live preview** on (tool bar), every change runs a coarse version
(~51 m cells) in the background and draws it over the map inside a dashed
yellow box marked PREVIEW: nothing is written yet. **Bake** (panel or tool
bar) writes the range into the height plane: the full erosion at 25.6 m,
brought to the 6.4 m posts with rock detail, in the background with a
progress bar; one undo step. A range only ever RAISES the ground, so baking
one over a higher hill leaves the hill. **Rebake** (this session) takes the
last bake out (strokes painted since stay, as offsets) and bakes the range as
it is now; **Unbake** takes it out. Like the Line tool's, they only reach a
bake made in this session; after a reload Bake bakes on top.

The range lives in map.json `ranges[]` (`{id, name, points: [[x, z, crestY,
halfWidth], ...], profile, seed, peaks, spurs, foothills, erode, wander,
baked}`, voxels). It is EDITOR-ONLY: the game reads only the posts a bake
wrote.

How it is made (`assets/editor/terraingen.js`, `rangeField`): the knots give
an envelope (the profile across, ridged noise along the crest for peaks and
cols, ridged noise in the range's own along / across frame for spurs, warped
ridged hills for the foothills); that envelope is an UPLIFT field, and a
landscape-evolution run (implicit stream power after Braun & Willett, with
hardness bands and talus slumping) grows the range from low ground under it,
so the drainage net forms while the land is low and branches, captures and
reaches the crest, as a real range's does. The relief's local maxima are then
put back on the envelope, so the crest stays at the knots' heights. Size and
time: a 10 km crest with a 3 km half-width (a 17 × 12 km window) bakes in
about 7-10 s. `node scripts/render_terrain_preview.mjs` renders what it makes
as hillshaded PNGs (`build/terrain_preview/`).

### 15.1b The Generate panel: continents and landforms

**Templates** make the land of a whole map (no Area) or of the Area, the way
Azgaar's generator does: an op list run with a seed. `continent`, `island`,
`archipelago`, `peninsula` (the mainland off the north edge) and `highlands`
ship. Each is a list of ops you can see and edit in the panel (JSON):
`fill` (everything to a height), `hill` / `pit` (organic blobs), `range` /
`trough` (a random-walk crest made into a real range spline / a U valley),
`strait` (a channel across), `mask` (drown the edges: an island; `open` keeps
one side as mainland), `noise` (ridged or rolling relief), `smooth`, `add`,
`multiply`, `erode` (the landscape stream-power pass) and every landform
below. Coordinates are fractions of the target (0..1), heights metres above
the sea, sizes fractions of its larger side. **Preview** draws a coarse run
over the map; **Generate** runs it in full (51.2 m, brought to the posts with
detail) and holds it in the plane: **Commit** keeps it (one undo step,
recorded in map.json `gen`, editor-only: `{template, seed, ops, box}`),
**Discard** puts the land back. Into the Area, the result blends into the land
round it over its outer edge.

**Landforms** are placed by the Area (draw one first): `crater` (a bowl,
raised rim and ejecta, slump terraces and a central peak when big; at the
Area's centre), `volcano` (a gullied cone with a summit crater), `canyon` (a
meandering terraced gorge with side canyons, along the Area's long side),
`glacial valley` (a U trough), `fjord` (a U trough dug below the sea, so the
sea floods it), `mesa field` (flat-topped mesas and buttes with terraced
cliffs and talus aprons over the Area) and `dunes` (asymmetric dunes, gentle
upwind, steep slip faces downwind, crests that fork; **wind toward** is the
direction the wind blows to, 0° = east, 90° = south). Their numbers are in
metres. Preview / Apply / Commit / Discard as for templates.

### 15.1c Reading the map: overlays, grid, minimap, Measure

The display bar's overlay picker:

| overlay | shows |
|---|---|
| `slope bands` | the land coloured by the game's slope bands instead of height: GREEN walkable (under 35°, where soil and scree hold), YELLOW steep (still climbable: the player steps 0.58 m per 0.1 m voxel, so stairs up to ~80°), RED cliff (at or past the map's `terrain.rockSlope`, 2.5:1 = 68° shipped: bare rock, no soil, snow or plants). Measured post to post on the 6.4 m plane; the engine's noise adds steepness on top |
| `flow` | where rivers would run: drainage area over a ~51 m copy of the whole plane, water routed downhill across filled hollows (Priority-Flood); brighter = more water. Recomputed a moment after an edit |
| `lakes` | where water would stand: every closed hollow filled to its spill point, deeper = darker. A hint for lakes; the game puts no water there by itself |

**km grid** draws a kilometre grid (1 / 2 / 5 / 10 km by zoom), labelled.
**minimap** (when the canvas is big enough) puts the whole map in the corner
with the view's box; click it to jump there.

**Measure** (tool): click points along a route; drag a point to move it;
shift+click, Esc or **clear** start over. Under the tool bar: the horizontal
and 3D length, the climb and descent, the steepest and the mean grade (%),
and the elevation profile coloured by the slope bands (hover it to find the
spot). It reads the authored plane and writes nothing.

**Adding a tool to this page** (for whoever builds the next one): one
`registerTool({key, label, group, title, opts, down, move, up, cursor, paint,
onKey, dblclick, resize, sync, guide})` call, from map.js or from a layer
module handed the page's context (`C.registerTool`, as `maprange.js`,
`mapgen.js` and `mapoverlay.js` do); the tool bar, the options strip, the
pointer / key routing, the cursor and this guide's box all come from it.
`assets/editor/maptools.js` documents every hook.

### 15.1d Coast tools: wobble, beach, cliff, smooth

The **Coast** tool (height group) shapes the shoreline and nothing else: it
only moves ground within the **band** round sea level (± metres; a cliff
works on the land within its plateau of the waterline), so the inland and
the deep sea floor stay put. Four modes:

| mode | what it does |
|---|---|
| `wobble` | frays the coastline: every post within the band takes the height of a point displaced by fractal noise (**amp** = how far, **size** = the largest cove / headland, **seed**), clamped to the band, so a ruler-straight shore grows coves, spits and headlands |
| `beach` | a beach band: the land within **width** of the waterline becomes a gentle ramp from the water up to **top** m (easing back to the ground at the band's inner edge), and the sea within a shelf off it a shallow floor (**shelf** m deep) |
| `cliff` | a sea cliff: the land within **plateau** of the waterline is raised to at least **height** m (a plateau edge, easing back inland), the sea at its foot dropped to at least **drop** m deep; the waterline stays where it was and the coast becomes a wall |
| `smooth` | rounds the shoreline: the posts within the band are pulled toward a ~**blur** m average |

Left-drag along the coast ([ ] radius, **str** = strength). A stroke is
stable: going over the same spot again does not stack (each post moves
toward its target by the largest weight any dab of the stroke gave it). Each
stroke is one undo step. **apply to Area** runs the mode over the whole Area
at once (feathered at its edge). The **mask** keeps it to the land or the
sea side. The 6.4 m plane is the limit: a beach narrower than ~13 m or a
cliff of a few metres hardly shows; Sculpt does finer coast work. The sand /
rock skin comes from the biome and the slope, not from the mode.

### 15.1e Climate and auto-biomes; the Lock tool

The **Climate** panel paints the BIOME plane from a climate it computes over
the height plane, so a continent gets wet windward forests, a dry lee, cold
high ground and wetlands where water gathers, instead of hand-painted blobs.
Its inputs are map.json `climate` (EDITOR-ONLY: the engine reads the biome
plane it writes, never these):

| input | meaning |
|---|---|
| wind from (dial) | the prevailing wind, compass degrees it blows FROM (270 = westerlies); drag the dial |
| sea temp | mean annual sea-level air temperature on the map's centre line, °C (the biome files' scale: tundra −10, forest 10, desert 24, jungle 26) |
| latitude / cold edge | °C colder per km toward the cold edge, and which edge that is (0 = north) — a fantasy map compresses latitude |
| lapse | °C colder per km of height above sea (6.5 = the standard atmosphere) |
| aspect | a steep poleward valley side is up to this much colder, an equatorward one warmer |
| coastal | how much coastal land is pulled toward the sea's temperature (maritime: milder north, cooler south) |
| rainfall / orographic | rainfall multiplier (1 ≈ 1,000 mm a year on a windward coast) / how hard climbing air rains out — the windward-wet, leeward-dry contrast |
| wetness | weight of flow accumulation, rivers and lakes (map.json `rivers[]`, `lakes[]`) and waterlogged flats on moisture |
| T / M weight | how much temperature vs moisture decides the nearest biome |
| jitter | border jitter, so biomes interlock instead of following contour lines |
| min patch | patches smaller than this (cells) merge into a neighbour: no speckle |
| glacier T / alt | glacier only where at least this cold AND this high |
| auto biomes | untick a biome and the generator never picks it (`climate.never`; ashlands and crystal barrens are off by default — place them by hand) |

**How it decides.** Temperature = sea temp + latitude (pulled toward the sea
near the coast) − lapse × height − aspect. Precipitation comes from a
**prevailing-wind march**: the map is cut into scanlines along the wind; a
parcel of vapour fills up over the sea (and off the map, which is ocean),
rains out over land at a base rate plus an orographic share of every metre it
climbs, and reaches the lee wrung out — the **rain shadow**. Moisture is
precipitation / 3,000 mm (the biome files' 0..1 scale) plus wetness. Each
25.6 m cell then takes the NEAREST biome in (temperature, moisture) over every
biome file's `climate` pair, with rules: ocean only below sea level (and
nothing else there); glacier only cold and high; swamp / glowfen only on flat,
low, wet ground; dunes only by the coast; mesa / karst / downs / alpine prefer
their landform (slope and height); jungle wants real heat. Slope → bare rock
and height → snow stay the engine's (the biome only says what grows).

**Overlays** (the panel's own picker, with an opacity): temperature,
precipitation, moisture, wetness, and the **predicted biome** of every cell.
They are computed in the terrain worker a moment after the map or an input
changes; the line under the picker reads the values at the cursor.

**Generate biomes** classifies the Area (or the whole map with no Area) in the
worker and shows the result as a PREVIEW over the map (dashed yellow box);
nothing is written. **Commit** writes the biome plane — one undo step — and
adds any biome the map's palette (`biomes[]`) lacks (at most 256);
**Discard** drops it.

**Locks: your hand painting survives.** While **lock painted cells** is on,
every cell the Biome brush paints is LOCKED (white hatching while the Biome or
Lock tool is active), and Generate keeps its biome. The **Lock** tool (paint
group) paints locks directly: drag to lock, shift+drag (or mode unlock) to
unlock; **lock all land** locks every non-ocean cell of the Area or map;
**clear locks** (panel) unlocks everything. Locks are stored in map.json
`climate.locks` as a run-length string over the biome cells (`{cells: [nx,
nz], rle}`), undo with the stroke that made them, and are dropped if the
map's extent or biome cell size changes. Typical use: paint the places that
matter (Harrowby's meadow, a cursed fen), Generate the rest, unlock and
regenerate where you change your mind.

### 15.2 Roads

A road is graded ground the engine builds: it cuts through bumps, fills dips,
eases a shoulder back into the land either side, lays a surface, keeps trees,
cover and tarns off itself and shows at any distance. It lives in map.json
`roads[]`.

**Drawing.** The **Road** tool: click on open map to start, click for each
vertex, double-click, Enter or clicking the last vertex ends it. Click a road
to select it; click a segment of the selected road to insert a vertex; drag a
vertex to move it; shift+click (or Delete) deletes one.

**Junctions.** A vertex dropped within a few pixels of another road's vertex
lands ON it; dropped on another road's segment, it lands on the segment and
that road gains a vertex there (a T). Dragging a shared vertex moves every road
on it; hold **Alt** when you press to drag one road's vertex out alone, or when
you release to drop without snapping. **Ctrl+click** on a road starts a
branch there. The vertex table marks shared vertices `⋈`.

**The inspector** (Vector layers panel, select a road):

| Field | Means | Defaults road / track / path |
|---|---|---|
| `id`, `name` | the id is required and unique; the name is optional | |
| `kind` | `road`, `track` or `path`; changing it moves the fields still at the old kind's defaults | |
| `surface` | the top material; offered only SOLID ones (`packed_gravel`, `packed_earth`, `cobble`, `flagstone`, …) | |
| `width` | carriageway, voxels (2..256) | 40 / 24 / 12 |
| `shoulder` | the ramp back to the bare ground either side, voxels (0..64); the engine widens it where the cut or fill would step more than a voxel | 16 / 12 / 6 |
| `max grade` | steepest the profile may climb, % (1..100) | 10 / 15 / 25 |
| `depth` | surface cells from the top down (1..8) | 3 |

**Continue from end** adds vertices after the last; **Reverse** flips the
order (the profile then runs from the other end).

**Solid surfaces only.** A graded road is a slope, and a powder (gravel, dirt,
sand) on a slope never stops sliding, so the CA never sleeps. The loader
refuses the whole map if a road's surface is not a solid. `packed_gravel` and
`packed_earth` are the solid look-alikes of gravel and dirt.

**The profile chart** under the inspector is the ENGINE's bake (`--mapcheck`
of the saved map), not a guess: the road surface over the bare ground it was
graded from, cut tinted red and fill blue, vertices ticked, a span over
`max grade` drawn red (it should never be). Hover the chart to see the spot
on the map. A road you changed since the last save is labelled as the SAVED
road's bake; a road not saved yet has no chart. Save to see it.

**max grade, in practice.** The bake samples the bare ground every 0.4 m,
smooths it over ±3.2 m, then fits the road between the steepest-allowed line
under the ground and the one over it. A low `max grade` across a steep slope
means deep cuts and high fills, with shoulders up to 6.4 m wide; if the chart
shows a big cut, add vertices so the road switches back across the slope, or
raise the grade.

**What it does not do yet.** Junctions are not profile-matched: two roads meeting at
a vertex each bake their own height there and the ground blends them in
table order. Keep a junction on level ground, or give the later road's end
the same grade. The page refuses to save a road with fewer than two distinct
points or a duplicate id, because the engine would refuse the map.

Harrowby's paths are roads (ids `harrowby_*`, kind `path`, `packed_gravel` on
the green, `packed_earth` lanes); `default/scripts/harrowby_08_paths.sh` now
only erases the old painted paths from the edit layer.

**Curved roads.** A road with `"smooth": true` bends THROUGH its points (a
centripetal Catmull-Rom spline, the same curve the river tool draws); without
it the road is the straight polyline it always was, so old roads do not move.

* **New roads bend.** Every road you draw now starts with `smooth: true`. An
  older road stays straight until you tick **bend (smooth)** in its
  inspector (untick it to go back).
* **The map draws the curve the engine builds** (the spline at 16 voxels,
  rounded, simplified -- exactly what the loader grades), and the points sit
  ON it. To reshape a bend, drag a point; to add one, click the selected
  road's curve where you want it and drag (that is "drag the curve").
  Junctions snap onto the curve too.
* **The profile chart follows the curve**: distance along it, the points
  ticked where they fall on it. Before the first save it shows the height
  plane under the curve you drew (no engine numbers yet).
* **Fords.** Where a road crosses a river the map draws stepping stones and
  the river's inspector lists the road: the engine keeps the road deck across
  the channel under the water.

### 15.2b Rivers and lakes (what the engine does with them)

The drawing tools for these are the River and Lake tools (§15.2c); this is
what the GAME makes of the `rivers[]` / `lakes[]` they write, so you know
what to expect when you walk there.

**A river is a staircase of still pools, not a current.** Water that flows
never lets the world rest, so the engine takes your line (source first), finds
the lowest ground along it, and lays the water in FLAT stretches (reaches)
that step down every `stepVox` voxels of fall (default 4 = 40 cm). Each step is
a small weir; a step of more than 2 m is a waterfall (the map page's check
lists them). The channel is cut `d` voxels deep at its middle (a U, or a V
with `"profile": "V"`), `w` wide, and the banks blend back into the land over
`bank` voxels (default 1.5 times the width): where the land is higher the
banks are cut down, where it is lower a low levee holds the water in. No trees
grow on a river's banks.

* **Ends.** A river ends at the sea, in a lake (put its last point inside the
  lake's outline), or in another river (`"joins": "<id>"`, last point on that
  river's channel): its water meets the other body at that body's level, with
  a weir or waterfall just before if it arrives from higher up. A river whose
  first point is inside a lake is that lake's OUTFLOW and starts at its level.
* **Fords.** A road crossing a river keeps its deck under the water: a ford.
  Bridges are stamps (later).
* **Beds.** `bed` (default gravel) lines the channel; a loose bed stays only
  where it can rest and the rest is rock.

**A lake** is an outline and a `level`: every column inside the outline whose
ground is below the level is water up to it; columns above it are islands or
dry shore. Just outside the outline, within the shore `band` (default 24
voxels), the ground is raised to level + 1, so a sloppy outline never leaks.
`bed` (default shore_mud) lines the bottom, `shore` (default sand) the dry
band. The check panel warns when a lake's ground is above its level everywhere
(it holds no water) or the spawn is in water.

### 15.2c Drawing rivers and lakes: the River and Lake tools

Both write map.json and nothing else: the engine carves the channel and fills
the water when it loads the map, so the height plane is never touched (undo,
rebake and so on are not needed). They are listed, inspected and derived in
the **Water** panel on the right.

**River tool.** Click the SOURCE, then along the course down to the MOUTH;
double-click or Enter ends it. The first and last knots carry the width and
depth (the toolbar's `source w / d` and `mouth w / d`, in metres); the knots
between take them along the way unless you set them in the knot table.

* **The mouth snaps** when you finish, or when you drop the last knot after
  dragging it: onto another river's channel (it becomes a TRIBUTARY: `joins`
  is set and its water meets the parent's at the junction), inside a lake
  (it drains into it), or onto ground under the sea (the coast). A yellow box
  under the cursor shows the target while you draw. The status line says what
  it snapped to; "ends on open ground" means it snapped to nothing.
* **Editing.** Drag a knot to move it. The **diamond** beside each knot of the
  selected river drags its WIDTH; **Alt+drag** a knot up / down sets its DEPTH.
  Click the selected river's line to insert a knot there; shift+click a knot
  deletes it (an end knot hands its width and depth to the new end).
* **What you see is the engine's bake.** The page runs the engine's own river
  bake (ported to JS, and pinned to the C++ by a test that must match it
  sample for sample): the water drawn at its width, a thin white tick across
  it where a reach steps down, a white fall glyph at each waterfall (a step of
  more than 2 m), a yellow square where a tributary joins. The one thing it
  cannot copy is the ground: it uses the HEIGHT PLANE, while the game adds the
  seeded terrain noise and the sculpt layer, so the game's reaches can land a
  little differently. After a Save, the inspector's profile also draws the
  ENGINE's own bake of the saved river, dashed.
* **The inspector** (Water panel): id, name, `smooth`, `bed` (gravel by
  default), `stepVox` (how far the bed falls before the surface steps down;
  bigger = longer, flatter pools and bigger steps), `bank` (blank = auto, 1.5 x
  the width), `profile` U or V, `joins`, the knot table (x, z, width and depth
  in metres; blank = interpolated), and the stats: length, reaches, pieces,
  waterfalls, where it drains, any fords. Its warnings are the engine's (for
  example a tributary arriving below its parent's water, which the engine
  clamps).
* **The profile chart**: the ground under the centreline (green), the water
  body and its surface (blue: flat reaches and steps; waterfalls white), the
  knots ticked; hover it to see the spot on the map.
* **Meander** (inspector): bends the river with a sum of sine waves along its
  line, wavelength about 12 x its width, the loops widening where the ground
  is flat and almost vanishing where it is steep; the source and mouth stay
  where they are. The slider sets the amount, the seed which bends you get.
  One undo step -- try a few seeds.

**Derive rivers** (Water panel). The terrain already says where water goes:
Derive runs the drainage network (water routed downhill over filled hollows,
the same as the "flow" overlay) over the **Area**, or the whole map when no
Area is set, on a coarse copy (~13-50 m cells), and keeps every channel whose
catchment is at least the **threshold** (km², a log slider). The preview is
coloured and thickened by ORDER (pale = a headwater, deep blue = the trunk of
a network). Click rivers with the River tool to pick them (white), then
**Accept picked**, or **Accept all** of an order and higher. Accepted rivers
keep their `joins` to each other, get a width from their catchment (about
2.5 m per square root of km²) and a depth of a sixth of that; a tributary
whose parent you did not accept snaps its mouth instead. They are ordinary
rivers afterwards: edit, meander or delete them.

**Lake tool.** Click inside a hollow: the lake fills it to its SPILL level
(the lowest point on its rim, found by a whole-map Priority-Flood) and its
outline is the shore at that level, traced on the 6.4 m posts. In the Water
panel the **level** slider raises or lowers it, and with **re-flood** ticked
the outline follows the water live (untick it to move only the level).
Polygon mode (toolbar) draws the outline by hand: click the vertices, click
the first one, double-click or Enter ends; its level starts 1 m over the
lowest vertex. Drag a vertex, click the selected lake's edge to add one,
shift+click deletes one. Click inside a lake to select it.

* **The sill warning.** Red squares round the selected lake mark shore-band
  ground OUTSIDE the outline that is lower than the level + 1: the engine
  raises it there to hold the water (a sill). A few squares at a narrow
  outlet are what a sill is for; a long red wall means the outline does not
  follow the basin (re-flood, or lower the level).
* **A click that finds no basin** ("the ground drains away") is on a slope or
  in a hollow open to the sea; ground under the sea level is sea, not a lake.

**What they do not do yet.** Rivers never change the height plane: a river
drawn uphill is legal, it just becomes a long flat pool held by its banks.
The live preview ignores the terrain noise (see above). Derive works on a
coarse copy, so a derived river can wander a few metres off the valley floor:
drag its knots, or let the engine's banks take it. The meander keeps the
endpoints but not a knot you placed in the middle.

### 15.3 Regions and markers

**Regions** (`regions[]`: `{id, name, colour, points}`) are named outlines,
"the Mirkwood", "the March of Ost". The **Region** tool: click out a polygon,
click its first vertex to close it; vertices drag / insert / shift+click delete
like a road's, and snap to other regions' vertices and edges so two regions
share a border. The inspector sets id, name and colour. They are data only:
the engine loads and checks them (an id, at least three distinct points) and
answers `RegionAt(x, z)` with the last region in the list that holds the
column. Nothing in the game reads them yet.

**Markers** (`markers[]`: `{id, name, icon, at}`) are pins on the map for
you: the **Marker** tool places one with the icon picked beside it (hut,
village, castle, tower, cave, ruin, shrine, camp, mine, port, 12 × 12 pixel
art), drag moves it, shift+click deletes it. A marker builds nothing. A place
you want to BUILD is a locale.

### 15.4 Sites and the sculpt layer, briefly

The site tools are unchanged (§14.1 for clearings and softened ground):
**Pad box**, **Spawn**, **Stamp site**, **Water** (an authored lake from a
preset), **Tree**, **Clearing**, **Soften**, and **Select** to pick any of them
for the Sites panel. **Sculpt** paints `sculpt.svsculpt`, a ±offset on top of
everything at 0.8 m (raise / lower / smooth / flatten / erase). Use the height
plane for the world and sculpt for a place. The **Load check** panel is
`sandvox --mapcheck` on the saved files: every warning the engine has about
the map, the baked roads and the locales' anchors.

### 15.5 Locales: hand-made places that ride their ground

A **locale** is a place you build by hand: a village, a witch's hut, a ruined
tower. It is a box on the map (`locales[]`: `{id, name, icon, bounds: {min,
max}, anchor: {at, y}}`), a ref group with the same name (`refs/<id>.json`:
its houses, people, paths, chests) and its own voxel layer
(`locales/<id>/ground.svedit`).

**Why the anchor.** You will keep reshaping the world after you build a place
on it. A locale's refs store their `y` RELATIVE to `anchor.y`, the ground at
`anchor.at` when you built it. At load the engine measures the bare ground at
`anchor.at` again and lifts every ref of the group by the difference: raise the
hill under the village 20 m and every house, pad, waynode and villager comes up
20 m together, still standing on each other as you built them. It is ONE
offset for the whole locale, so the village stays rigid; it follows the ground
at the anchor, not the shape of the ground under each house (a house on a slope
you steepened still levels its own pad). The voxel layer is stored per column
relative to the ground already, so it follows too. The References page, F8 and
edit scripts all show and take WORLD positions; only the file is relative.

**Creating one.** Pick the **Locale** tool, choose an icon beside it, and drag
out the bounds (at least 3.2 m a side). The anchor starts at the centre with
`anchor.y` from the real-terrain backdrop or the plane. Select it in the
**Locales** panel to set id (a folder name, `[a-z0-9_-]`, fixed once saved),
name, icon, the bounds and the anchor. **Save map**, then the panel shows what
the engine measured ("ground at the anchor y…, delta …"); **anchor.y := engine
ground** makes the delta 0. Do that before you build, so nothing jumps.
Deleting a locale (×) removes it from map.json only; its refs file then reads
as absolute y.

**Opening it.** Double-click it on the map (or **Open**). The page becomes the
LOCALE PAGE: a breadcrumb (**World map › Harrowby**) leads back, the view frames
the bounds over the engine's heights at one sample a voxel, and the tools are
cut down to what a place is made of:

- **Sculpt**, **Clearing**, **Soften**, **Tree**, **Water**: CLIPPED to the
  bounds. A sculpt stroke dragged over the edge changes no ground outside it,
  exactly. A box is clamped (feather included); a lake or tree outside is
  refused. These are map.json / sculpt edits: **Save map** writes them.
- **Structure**: pick a blueprint (Environment → Structures), a facing (0° =
  door to +z / south, 90° = +x / east) and an optional name; click inside the
  bounds to place it on the ground, drag to move, **R** (or ⟳ / ⟲ in the panel)
  to turn, shift+click or Delete to remove.
- **Link**: click a waynode, then another, to join them (again, or Shift, to
  part them); click open ground for a new waynode joined to the last one;
  shift+click deletes one.

**Structures and waynodes are written through the F8 command layer**, never by
the page: each action becomes `ref.place` / `ref.move` / `ref.delete` /
`ref.link` / `ref.unlink` commands that the engine runs headless
(`--edit-script`, `/api/refs/commands`), exactly as the in-game editor (§13)
would, so the anchor arithmetic is the engine's. They are written at once (no
Save needed) and each batch is one undo step. They are REFUSED until the
locale in map.json is saved exactly as shown (the engine converts y through
the saved anchor). Villagers, markers, doors and chests are edited in game
(F8, F1 → References); the panel counts them.

**The locale's voxel layer.** Under the map, the **3D (voxels)** view streams
the real world of the box (the map's layer and every locale's applied, as the
game applies them). **tool**: `none` (orbit: drag to turn, wheel to zoom),
`brush`, `erase`, `picker`; the material; **r** (radius, voxels); **Undo**,
**Redo**; **Save voxels** writes `locales/<id>/ground.svedit`, ground-relative.
Only cells inside the bounds can be edited. Use it for what a blueprint cannot
do: a cellar below a pad, a stone yard, a well shaft, rubble. An empty layer
deletes the file.

**Layers stack.** The map's own `editLayer` (`assets/worldedits/<name>.svedit`,
the **edits** selector) applies first, then each locale's layer in map.json
order; where two name the same cell, the later wins.

**Harrowby is the example** (§14). `locales[0]` of `default`: bounds
(330, 3090)..(790, 3650), the soften box; anchor at the green (555, 3467),
`anchor.y` 201. `refs/harrowby.json` is relative to it,
`locales/harrowby/ground.svedit` holds Osric's cellar, and the paths are
`roads[]`. `default/scripts/harrowby_09_locale.py` is how it was converted,
and is re-runnable. The gates: `locale` (raise the plane under a locale: house,
pad, refs and voxel patch all move by exactly the anchor's change) and
`village-harrowby`.

### 15.6 The `peaks` map: the tall-terrain reference

`assets/worldmap/peaks/` is 8 km of mountains, the fixture for everything that
has to work on tall ground. A coast along the south edge (sea y 112),
foothills from 30 to 200 m, a **1,400 m massif** north-east of centre with a
**knife ridge** running west from the summit, a **300 m cliff band** across the
foothills, a **600 m gorge** cut through the massif's south flank, and a **high
valley at ~900 m** west of the massif with a tarn and the spawn. Biomes
follow altitude: meadow, pine, tundra, alpine.

It is GENERATED: `python scripts/gen_map_peaks.py` writes it identically
every time. Edit the script, not the tiles (a hand edit is lost on the next
run). Copy the folder under another name to make a map of your own that
starts from it.

Walk it: pick `peaks` in the map selector (or `SANDVOX_MAP=peaks`) and start
the game; the boot window is placed on the spawn's ground (~900 m). What it is
used for:

- the gate `altitude` (the window, the per-biome treeline, the lapse rate, the
  lava clearance under the gorge, valley sediment, bare rock on the cliff band
  that settles to ≤ 32 active chunks);
- the look frames `--shot-frames screenshot_peaks_valley,screenshot_peaks_summit,screenshot_peaks_sea10k,screenshot_peaks_dusk,screenshot_peaks_storm,screenshot_peaks_cloudcap`;
- the budget cameras `--render-budget --budget-cams summit,peaksea`;
- the perf flight `--perf --scenario peaks-route` (summit → gorge → high
  valley).

Past the near field, a mountain is drawn by the **far horizon**: the height
plane marched out to the map's edge, so a 1,400 m peak shows from 10 km and
more. The air thins with height (`render.fogSeaDensity`, `render.fogScaleHeight`,
`render.fogHaze` for the distance blue), ridges shade their valleys at the
scale of the map, and clouds stop at the terrain, so a summit can stand in the
deck. All render-only: `tuning.json`, live on F5.

### 15.7 When it goes wrong

- **"map.json is a v1 map … convert it with `python scripts/convert_map_v2.py`"**:
  run that command.
- **"N sites reach map cell … a cell holds at most 128"**: that many sites
  and road pieces in one 102.4 m square. Spread them out, or use fewer
  vertices on a long road.
- **A road refused: "not a solid"**: pick a solid surface (`packed_gravel`,
  `packed_earth`, `cobble`, `flagstone`).
- **The profile chart says "not saved yet"**, or the backdrop shows the old
  ground: both read the SAVED files. Save the map.
- **A locale's houses float or are buried after you reshaped the ground**:
  the locale follows the ground at its ANCHOR. Put the anchor where the
  village stands (the green, not the edge of a slope), save, check the delta
  in the Locales panel.
- **Placing a structure is refused on the locale page**: save the map first;
  the engine needs the locale as shown.
- **Rebake does nothing useful after a reload**: rebake only reaches this
  session's bake (§15.1, §15.1a). Ctrl+Z, paint it out, or bake again on top.
- **"another terrain job is running"**: one bake / generate / erosion at a
  time; wait for the **working** bar to finish, or press its **Cancel**
  (nothing is written by a cancelled job).
- **A bake, a template or an erosion says "tiles are still loading"**: the
  page loads the plane's tiles in the background after opening a map; wait a
  moment (the status line stops saying "(loading)") and try again.
- **A range came out lower than its knots**: the `peaks` slider puts cols
  between peaks, so a knot can sit in a col; the highest point near it is the
  knot's height. Lower `peaks` for an even crest.
