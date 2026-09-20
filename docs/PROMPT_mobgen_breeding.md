# HANDOFF: a parametric character generator + breeding/selection page in the tuner

You are picking this up cold. Everything you need to know that was already
established is in this file; the facts below were read out of the tree, not
guessed, but **verify anything you are about to depend on** — this was written
2026-09-19 and files move.

---

## 1. THE END GOAL

The owner wants to stop hand-authoring NPC bodies one at a time. The result,
usable from the tuner with no rebuild:

1. **A pool of character models.** Generate a batch of random characters, look
   at them, save the good ones as real mob assets. The saved pool is what the
   game spawns from — so everything in it is guaranteed to look good, because a
   human picked it.
2. **Manual tweaking.** Take any character (saved or freshly rolled) and adjust
   it by hand — sliders for body shape, pickers for colour and features. Not a
   black box you can only re-roll.
3. **Offspring.** Pick two (or more) characters and breed them: mix their
   parameters, get a litter, keep what you like. Iterate.
4. **Visual distinctness, not just body shape.** Different hair (style AND
   colour), different eyes, different skin tones, different clothing colour.
   Two characters with identical proportions must be able to look like
   different people.

This is **interactive evolution** — a human is the fitness function. There is no
automatic scoring, no tournaments, no generation counter. The algorithm is
small; the generator and the UI are the work.

**Non-goal, explicitly out of scope: non-humanoid creatures.** See §8.

---

## 2. WHAT EXISTS TODAY (verified)

### `scripts/gen_human.py` — the thing you are porting

1,588 lines of Python. Run once, by hand, it writes `assets/mobs/human.vox` and
`assets/mobs/human.json` — the stock base player model. Read its module
docstring first; it explains the design decisions and two traps that were
already got wrong once (facing/handedness, and centring).

Structure:

- **Scale contract, in METRES** (this is the important part — it is already
  derived, not hardcoded):
  `HEIGHT_M = 1.70`, `EYE_M = 1.50`, `ART_VOXELS_PER_METRE = 80`,
  `SKIN_UPSCALE = 2` → `AUTHORED_VOXELS_PER_METRE = 40`, `ART_SCALE = 4`,
  `SCALE = ART_SCALE * SKIN_UPSCALE = 8`.
  `WORLD_H = 17` world voxels, `MICRO_H = 68` authored art voxels.
  `kVoxelMeters` is **parsed out of `src/sim/world.h`**, never copied — do the
  same. (`scripts/bake_trees.mjs` explains why in its header: treegen.js once
  carried a hand-copied `VOX_PER_M = 10` "mirroring world.h", world.h moved to
  5 cm, and every tree in the game baked at half height with nothing able to
  notice.)
- **Shape helpers:** `ellipse_mask`, `profile(u, pts)` (piecewise-linear radius
  profile), `limb_tube(size, r0, r1, col_for)`, `shade_back`, `flip_y`,
  `upscale_voxels`.
- **Per-limb builders:** `head_vox`, `torso_vox`, `hips_vox`, `upper_arm_vox`,
  `fore_arm_vox`, `hand_vox`, `thigh_vox`, `shin_vox`, `foot_vox`.
- **Tables:** `ART_LIMBS` (authored sizes + scene offsets), `SHAPES`, `ORDER`,
  and `LIMBS` (the shipped table = `ART_LIMBS` upscaled by `SKIN_UPSCALE`).
- **.vox writing:** `chunk`, `dict_bytes`, `model_chunks`, `rgba_chunk`, `ntrn`,
  `ngrp`, `nshp` — a full MagicaVoxel scene-graph writer.

**Shipped limb model sizes** (so you know the resolution you are working at —
this is small, hand-art scale, not a voxelized mesh):

| limb | size (x,y,z) |
|---|---|
| head | 20 × 12 × 20 |
| torso | 20 × 18 × 32 |
| hips | 20 × 12 × 20 |
| armU / armL | 8 × 8 × 22 |
| hand | 8 × 8 × 10 |
| legU | 8 × 8 × 28 |
| legL | 8 × 8 × 26 |
| foot | 8 × 14 × 8 |

**Facing / handedness (got wrong once already, do not re-derive):** the loader
maps scene → engine as `(x, z, -y)`, which negates scene y and therefore FLIPS
handedness. The model is authored facing scene **+Y** and mirrored once by
`flip_y` at the end of every builder. Model **+X is the character's LEFT**, so
`.L` limbs take positive scene x.

**Centring:** every box is centred so its ellipse centre (`mn.x + sx*0.5`) lands
on `x = 0`, and left/right boxes are exact mirrors about it. Keep this — it is
invisible on a robe and very visible on a bare shoulder line.

### Hair and eyes are ALREADY procedural

This matters a lot for deliverable 4, so read `head_vox` (around line 300)
carefully. It is not a painted-on texture:

- The skull is a **swept ellipsoid** from two radius profiles, `prof_rx` and
  `prof_ry`, each a list of `(u, radius)` knots read as a face: chin, jaw,
  cheekbones, temples, crown. Change the knots, change the face.
- `hcy = cy - 0.6` puts the skull **behind** the face plane, because a human has
  more cranium behind the eyes than in front; centring the ellipsoid gives a
  snout.
- **Hair is a function**, `hair_line(y)` → "the z at or above which this depth
  row is hair", lower at the nape than at the forehead so the cap reads as swept
  hair rather than a helmet. **A different hairstyle is a different
  `hair_line`** — this is your cheapest, highest-yield lever for visual variety.
- **Ears are appended**, not carved — they sit one cell proud of the widest part
  of the skull, where the ellipse left empty space, so nothing conflicts.
- **Eyes and mouth are repaints of the FRONTMOST existing cell** in their column
  (`paint_front`), so they can never float in front of the face or sink into it.
  Eye separation is `eye_dx = 2.5` micro about the centre line. Eye row `eye_z`
  is **derived** from `EYE_M`, minus the head limb's own scene z, read out of the
  AUTHORED table `ART_LIMBS` — never the upscaled one. There is an `assert`
  backstop. `gen_mina.py` got exactly this wrong and her face void has silently
  not been carved since the 2× upscale landed. Preserve the derivation.

### The material / colour split

- **Palette index == material ID** (`.vox` palette index i+1 == `materials.json[i]`).
  `src/sim/voxload.h` states RGB→material matching is deliberately **not**
  supported (lossy). Do not invent one.
- Mob voxel material ids must stay **≤ 127**; **128..255 is the art palette**.
- **Every voxel of the human is one material** — `FLESH = 51` (`"skin"`), the
  material the flesh reaction chain is authored against. All visual variation is
  `color`, an art-palette slot carried in a **parallel `"<name>.col"` model** in
  the same `.vox` file. Art colour never reaches the world grid, so it can never
  touch the sim hash. `human` is the first shipped asset using the `.col` path.
- Art slots currently used: `SKIN_BASE 255`, `SKIN_SHADE 254`,
  `SKIN_LIGHT 253` (reserved, unused by geometry), `HAIR 252`,
  `HAIR_SHADE 251`, `EYE 250`, `CLOTH 249`, `CLOTH_SHADE 248`,
  `NAIL 247` (reserved). `ART_RGB` maps slot → RGB.

### The sidecar (`assets/mobs/human.json`)

Blocks: `root`, `artVoxelsPerMetre`, `bleed`, `anatomy`, `speed`, `gait`,
`limbs` (15 entries), `sockets`, `natural`, `chains`, `states`, `clips`
(~15 KB), `flipbooks`.

15 limbs, named: `hips`, `torso`, `head`, `armU.L/.R`, `armL.L/.R`,
`hand.L/.R`, `legU.L/.R`, `legL.L/.R`, `foot.L/.R`.

Per limb: `hp`, `severable`, `vital`, `tag`, `parent`, `joint`, `axis`,
`minAngle`/`maxAngle`, `anchor` (integer art-voxel coords), `poseLimit`,
`severImpactSpeed`, `spring`.

A limb is joined to its `.vox` model **by name and by nothing else**
(`mob.cpp:308`). Nothing binds a limb to the creature it was drawn on — see
`assets/editor/limblib.js`, which already exploits this to make limbs
transferable between creatures.

### ★ Mob defs already support inheritance and a palette filter ★

This is the single most useful thing in this document for deliverable 4, and it
is already shipped. From the engine map (`ARCH_NODES` in `assets/tuner.html`,
the mob-def node):

- A def is named for its **sidecar**, not its art. `"model": "<stem>"` uses
  another file's `.vox`. `"extends": "<stem>"` also starts from its contents
  (and implies `model`).
- Merging is **RFC 7396 merge-patch**: objects merge key by key, an array or
  scalar REPLACES wholesale, an explicit `null` deletes. Depth-bounded at 8.
- **`palette` is a FILTER, not a colour table**: `saturation`, `brightness`,
  `tint` + `tintAmount`, applied to this def's copy of the prefab art palette
  BEFORE `MicroBodyMergeArt` folds it into the shared one. Desaturation is
  Rec. 709 luma, not channel mean (a mean desaturate darkens reds and lightens
  greens, which on skin reads as the wrong ethnicity rather than the wrong
  health).
- **`assets/mobs/zombie.json` is 30 lines and there is no `zombie.vox`.**

**The consequence for your design:** a character whose *shape* matches an
existing one needs **no new `.vox` at all** — it is a sidecar with a `model`
reference and a `palette` filter. Only a character with new *geometry* needs new
art. Exploit this. It keeps the pool small, keeps variants tracking their base,
and avoids the failure the engine map already warns about (copying a 150 KB
binary stops it tracking the original, and the ANATOMY is baked into the `.vox`,
so the copy quietly diverges in what is under its skin with no test to say so).

A palette filter is global to the def, though — it cannot say "this hair is red
and this skin is unchanged". For **per-feature** colour (hair vs eye vs skin vs
cloth independently) you must either regenerate the `.col` model, or extend the
palette block. Decide which and say why in your commit; regenerating `.col`
while reusing the geometry model is probably the sweet spot, but check whether
the `.vox` format and loader let a def override only the `.col` models.

### Anatomy

`assets/editor/anatomy.js` (+ `node scripts/anatomize_mob.mjs <mob>`) measures
depth-from-surface over the union of every limb and rewrites the interior from
the sidecar's `anatomy` recipe: skin kept, `flesh`, `muscle` speckled with
`blood`, `bone` at the core, plus a per-limb override — a bone skull two deep
around a `brain` core for the head. `node scripts/test_anatomy.mjs` pins the
committed human to its own recipe.

**Every generated body must go through this**, and the peel needs a **solid**
volume. Your generator produces solids, so this is fine — but make it a step in
the pipeline, not an afterthought. Note the head override is keyed by the limb
**name** `head`.

---

## 3. THE ARCHITECTURE IS ALREADY DECIDED BY PRECEDENT

This repo has solved parametric procedural voxel generation three times. Follow
the pattern exactly; do not invent a fourth shape.

| pure generator (browser + Node) | tuner page | Node bake | Node test | browser test |
|---|---|---|---|---|
| `assets/editor/treegen.js` | `trees.js` | `scripts/bake_trees.mjs` | `test_treegen.mjs` | `check_trees.sh` |
| `assets/editor/biomegen.js` | `biome.js` | `seed_environment.mjs` | `test_environment.mjs` | `check_environment.sh` |
| `assets/editor/watergen.js` | `water.js` | — | — | — |

`treegen.js` is the model to copy. It exports `defaultParams()`,
`normalizeParams(src, vpm)`, `generateTree(params, seed, opts)`,
`bakeAtlas(params, seeds, opts)`, `readAtlas(buf)`, a `PRESETS` table and
`PRESET_ORDER`. It is a plain ES module with **zero dependencies**, imported by
the browser page and by `node` alike.

So you write:

```
assets/editor/mobgen.js      PURE. No DOM, no fetch. Exports:
                               defaultGenome()
                               normalizeGenome(src)
                               generateMob(genome, seed) -> { models, col,
                                   limbs, gait, sockets, natural, chains,
                                   states, anatomy, ... }
                               mutate(genome, sigma, rng)
                               cross(a, b, rng)
                               PRESETS, PRESET_ORDER
assets/editor/breed.js       The tuner page (pool + tweak + litter grid).
scripts/gen_mobs.mjs         Node bake — the headless path, like bake_trees.mjs.
scripts/test_mobgen.mjs      The data gate.
```

`assets/editor/vox.js` is an existing zero-dependency `.vox` reader/writer
(1,106 lines) that both MagicaVoxel and `src/sim/voxload.cpp` accept, and it
already understands the `"<name>.col"` convention. **Use it. Do not write a
second `.vox` writer.** The Python one in `gen_human.py` is what you are
replacing, not porting.

---

## 4. THE ONE TEST THAT MAKES THE PORT SAFE

**`generateMob(defaultGenome(), <the human's seed>)` must reproduce the shipped
`assets/mobs/human.vox` byte-for-byte, and its sidecar field-for-field.**

Assert this in `scripts/test_mobgen.mjs`. It is cheap, it runs under `node` with
no GPU and no build, and it is the difference between finding a translation bug
in five seconds and finding it after the owner has bred forty characters on top
of it. `scripts/test_anatomy.mjs` pins the human to its anatomy recipe the same
way — same trick, same reason.

**Do not overwrite `assets/mobs/human.vox` as part of this work.** The test
proves you *could*. Regenerating it would move the world hash for no gain. New
characters are new files.

---

## 5. THE GENOME: ROLL SHAPE, DERIVE EVERYTHING ELSE

This is the one design rule that matters, and it is not about aesthetics.

A human eye looking at a thumbnail grid filters *ugly* perfectly well. It does
**not** filter *broken*, because the breakage is invisible in a static preview:
a body whose ride height does not match its leg length looks fine standing
still, then hovers, sinks, or foot-slides once it walks in the game. A shoulder
anchor slightly outside the shoulder is invisible until the arm swings.

So: **only shape is rolled. Everything that must agree with the shape is
computed from it.** This is not extra work — it is *less* work, because a
derived value needs no mutation range and cannot drift wrong.

| ROLLED (genes) | DERIVED (never a gene) |
|---|---|
| height (bounded — see below) | every limb `anchor` |
| bulk / girth | `gait.rideHeight`, `cadence`, `strideBias`, `stepThreshold`, `stepDuration`, `stepHeight`, `bobAmp`, `swayAmp`, `phaseLag` |
| limb-length ratios (leg : torso : arm) | `speed` (there is a sprint-speed contract) |
| shoulder width, hip width | `sockets[].offset` (e.g. `held_right` on `hand.R`) |
| head scale, face profile knots (`prof_rx`/`prof_ry`) | `natural[].edge` — fist and jaws capsules, in art units |
| hair style (which `hair_line`) + hair length | `chains[].pole`, IK effectors |
| eye row offset, separation, size | per-limb `hp` (scale with limb volume) |
| skin / hair / eye / cloth palette rows | `eye_z` (derived from `EYE_M` — preserve the existing derivation) |

**Two genes need a leash, not freedom:**

- **`HEIGHT_M` / `EYE_M` are contracts.** `EYE_M = 1.50` is the first-person
  camera row. The 17-world-voxel height and the sprint-speed contract are things
  reach, melee range, and several gate fixtures bet on (there is a known gotcha
  about gate fixtures betting on NPC walk distance). Vary height inside a band
  with every derived quantity following, and consider locking it entirely for
  anything in the player's lineage.
- There is documented history of exactly this class of coupling going wrong in
  this engine: gait height feeding itself, avatar height taken from player
  constants, gait speed vs leg length. Do not add a fourth.

**Per-gene locks in the UI.** The owner explicitly wants manual tweaking, so
every gene needs a slider *and* a lock toggle, so a litter can vary hair and
face while holding the body fixed (or vice versa).

---

## 6. VISUAL DISTINCTNESS — the deliverable most at risk

At a 20 × 12 × 20 head, an eye is one or two voxels. Do not expect facial
features to carry identity. What actually reads at this resolution, roughly in
order of yield:

1. **Hair silhouette** — `hair_line(y)` variants: cropped, swept, long (extends
   past the skull and down the neck/back), bald, topknot, fringe. This is the
   biggest single lever and it is a pure function swap.
2. **Colour** — skin tone, hair colour, eye colour, cloth colour, each as its
   own palette row. Note the existing art palette has **`SKIN_LIGHT 253`
   reserved and unused** — the docstring says "the customisation pass varies
   this row", i.e. this work was anticipated.
3. **Body proportion** — height, bulk, shoulder-to-hip ratio, limb length.
4. **Head profile** — the `prof_rx`/`prof_ry` knots: a heavy jaw, a narrow
   skull, a high crown.
5. **Facial marks** — eyebrow row, stubble/beard as a `paint_front` region,
   mouth width. Cheap, using the existing `paint_front` mechanism, but low yield.

Decide early **which of these regenerate geometry and which are palette-only**,
because that decides whether a pool entry is a full `.vox` or a 30-line sidecar
(see §2, mob def inheritance). Colour-only variants should be sidecars.

---

## 7. THE UI

Lives in the **Models** tab (`data-tab="models"` in `assets/tuner.html`) or a
new sibling tab — your call, but Models already owns the three.js preview, the
rig panel, the anatomy row and the clip/attack timeline, and the owner is used
to finding characters there.

Integration vocabulary that already exists: `window.__tunerRegisterModels`,
`window.__tunerModelsHooks` (`el`, `toast`, `materials`, `onDirty`, …). The
Environment tab gets "the Models/Trees vocabulary plus three things" — read how
`environment.js` bootstraps and copy it.

Three surfaces:

1. **Pool** — a grid of saved characters, read from `assets/mobs/`. Click to
   load into the editor. This is the shipping set.
2. **Tweak** — the loaded character's genome as locked/unlocked sliders and
   colour pickers, with a live preview. Save writes a real mob asset.
3. **Litter** — N offspring in a thumbnail grid. Sources: *randomize* (from
   scratch), *mutate* (one parent + a σ slider), or *cross* (two or more
   parents). Click any offspring to promote it into the pool, or into the tweak
   pane, or make it a parent of the next litter.

**Performance note:** N rigged bodies at ~30k voxels each is real geometry.
Render each offspring **once** to a cached thumbnail canvas and only re-render
the ones whose genome actually changed. Do not run N live three.js scenes.

**Saving is already plumbed.** `scripts/tuner_server.py` has `/api/model` routes
that write mob `.vox` + `.json` pairs (the Models tab already owns them), and
`/api/limbs` for the limb library. `/api/save` is for the allowlisted shared
JSONs and is **not** the route for a mob. Read the routing comment near the top
of `tuner_server.py` — it explains which goes where and why.

---

## 8. SCOPE BOUNDARY: humanoid only, and here is why

Do not try to make this generate non-humanoid creatures (quadrupeds, tails,
extra limbs). It is not a bigger genome; it is a different project, and the
blocker is not the generator.

**Every downstream system is keyed to limb NAMES, not to structure:**

- `gait.groups`: `[["legU.L"], ["legU.R"]]`
- `chains`: four hardcoded two-bone IK chains listing `armU.L, armL.L, hand.L` …
- `states`: `missing: ["legU.L","legU.R"] → crawl`, six-plus entries by name
- `clips`: ~15 KB of tracks, every one `mask`ed by limb name; `assets/anims/*.json`
  is "compiled onto every rig it fits" by `LoadMobDefs`
- `natural`: `fist.L` on `hand.L`, `jaws` on `head`, with per-part edge coords
- `sockets`: `held_right` on `hand.R`
- `anatomy.limbs.head`: the skull/brain override, by name
- `assets/mobs/attack_styles.json` → clip names

A four-legged creature has no `armU.L`, so idle / bite / crawl / limp / onearm
all fail to bind, the IK chains dangle, and the gait has nothing to group. The
tractable future version is **archetypes** — `humanoid`, `quadruped`, … each a
limb-name vocabulary with its own clip set, chains, states and anatomy recipe,
with evolution happening *within* an archetype. Structure `mobgen.js` so an
archetype could be added later (keep the limb table and the name vocabulary in
one place, don't scatter `"armU.L"` literals through the builders), but ship one
archetype.

---

## 9. VERIFICATION BUDGET

Read the "When to run what" section of `CLAUDE.md` before running anything. The
short version for this package:

- **This is a JS + assets package. It needs NO C++ BUILD.** `mobgen.js`,
  `breed.js`, the tuner HTML and the Node scripts are all runtime-loaded or
  Node-run. If you find yourself configuring cmake, stop and re-read.
- **Your gate is `node scripts/test_mobgen.mjs`.** It costs nothing, takes no
  lock, and covers the byte-exact reproduction (§4), genome normalisation
  round-trips, and "every rolled genome produces a body that passes the
  structural asserts" (anchors inside their limbs, eye row inside the head,
  materials ≤ 127, no floating voxels).
- **Browser half:** follow `scripts/check_trees.sh` / `check_environment.sh` if
  you need to prove the page renders and saves. There is an established headless
  Chrome harness; do not invent one.
- **You should not need a single `sandvox.exe` run** unless you regenerate a
  shipped asset. If you do regenerate one, the world hash moves, and that is one
  command — `--selftest --rebaseline` — at commit time, not an investigation.
  Read rule 8 in `CLAUDE.md` before you treat a moved hash as a problem.

---

## 10. HOUSEKEEPING

- **Claim the board before editing shared files.** `assets/tuner.html`,
  `assets/tuner_schema.js` and `scripts/tuner_server.py` are all on the
  claim-first list:
  `bash scripts/board.sh claim "<files>" "<what>"`, and
  `bash scripts/board.sh active` first to see who holds what. Never Edit/Write
  `AGENTS_BOARD.md` directly.
- **Update `ARCH_NODES` in `assets/tuner.html`** when you land — add a node for
  the generator, update the Models-tab node's `details`, and keep the
  Development Status panel's recently-landed list current. Use only ASCII `'`
  for JS string delimiters in that file.
- **`DESIGN.md` is architecture truth.** If this contradicts it, update it in
  the same commit.
- **Comment density**: this codebase documents *why*, at length, especially
  where two things must agree or where something was got wrong once. Match it —
  particularly around the facing/handedness flip, the derived eye row, and the
  rolled-vs-derived split, all three of which are traps that have already cost
  someone time.

---

## 11. SUGGESTED ORDER

1. Read `gen_human.py` end to end (its docstrings are the design rationale),
   `assets/editor/treegen.js` (the pattern), and `assets/editor/vox.js` (the
   writer you will use).
2. Port to `assets/editor/mobgen.js` with a **fixed** genome — no parameters yet
   — and get `scripts/test_mobgen.mjs` asserting byte-exact reproduction of
   `human.vox`. **Do not proceed until this is green.**
3. Lift the genome out: shape genes in, derived quantities computed. Re-assert
   §4 still passes at the default genome.
4. Add `mutate` / `cross` / `PRESETS`.
5. Build the tuner page: pool → tweak → litter, in that order.
6. Add the visual-variety levers from §6, cheapest first (hair silhouette and
   palette rows before face profile knots).
7. Wire saving through `/api/model`, including the sidecar-only path for
   colour-variant characters (§2).
