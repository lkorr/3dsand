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

Ctrl+S saves whichever tab you're on (on Models it saves the model, not the
JSONs).

---

## 2. The Models tab, top to bottom

```
[ATTACH] [Attach|Erase|Paint] [Voxel|Box|Face|Select] [Mirror X]   [Undo][Redo][?]
[open ▾][↻][new… ▾][Save][Save as…]        ■ material name     status line
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

### 4.4 Save

Save to `mobs/name.vox`; the sidecar `mobs/name.json` is written next to it
with all rig data. Fields this editor doesn't know about are preserved
verbatim, so hand-edited extras survive. Spawn the mob in-game to see it.

---

## 5. Animation

Three layers, same as the engine, and they compose:

1. **Gait** — procedural locomotion from the rig + gait block (§4.3). You
   don't keyframe walking.
2. **Clips** — keyframed poses layered on top (attacks, flinches, idles).
3. **Flipbooks** — per-part model swapping (blinking faces, mouth shapes,
   crumbling states) — the voxel equivalent of sprite frames.

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
