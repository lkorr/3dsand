# PLAN: weapon coats, venom, the item stage, the snake (2026-10-01)

**Status (2026-10-02 audit): A–D LANDED on main 2026-10-01** (`weapon-coats` merged;
tuner/engine map `b673ca1`; follow-ups `7bceaa1`, `1ceafbd`, `4689af6`). Gates
`coat-transfer`, `venom-blade`, `venom-wound`, `item-stage`, `snake`, `infect-perf`;
`assets/mobs/snake.{json,vox}`.

Owner request: coat weapons, armour and fists with any liquid and have the coat
TRANSFER onto what they hit. A blade carries its coat into the wound it cuts. A
blunt weapon splashes its coat onto the bruise. A fist works the same way. One
general rule covers this, with no venom-specific code: what a transferred coat
DOES afterwards is whatever that material already does as a body coat (acid
eats, oil flashes, water rinses, venom infects). Plus:

- `venom`, a liquid whose coat INFECTS soft tissue below the skin. It is a
  second rot-like infection that eats 1.5x as fast as it spreads, so it always
  burns itself out.
- An isolated 3D **item stage**: double-click a weapon or armour piece to look
  at it, rotate it, and splash it from a vial.
- A **snake** mob that slithers and bites venom into the wound.

Owner decisions (2026-10-01):
- **Viewer:** an isolated item stage, not a reuse of the portrait.
- **Coats on stored items:** FROZEN while the item is in the bag. They dry or
  decay only while the item is held or worn, at the material's own
  `coat.decay`.
- **Venom attacks soft tissue only:** flesh, muscle and organs (brain
  included). Not skin, not bone. Bone stops it.
- **Where venom comes from:** a venomous creature, the snake. It bites venom in
  like a zombie bite, a couple of voxels deep.
- This is the start of a larger item crafting and repair system (replacing
  microvoxels, repair, transmutation). Keep the stage and the item-lattice API
  general.

## What already exists (read these before coding)

- **The body coat is PER VOXEL.** `PrefabVoxel::stain` / `DebrisVoxel::stain`
  is a u16: material (12 bits) plus amount 0..15 (4 bits).
  - See `src/sim/voxload.h:56-100` and `StainLattice` in
    `src/phys/bodystain.h`.
  - Writers: `RaiseBodyStain`, `WashBodyStain`, `AddBodyStain`, `SoakCut`,
    `SoakBruise` (all in `bodystain.h/.cpp`). Each obeys `stainPrecedence`
    (`src/sim/coatrule.h`, mirrored in WGSL).
  - Render path: `MicroBodyPokeStain`, then `microbody.wgsl`
    `bodyStainCover`.
- **The coat ACTS through the material's own reaction rules.** See
  `MobSystem::BurnOneLimb` section 0, `mob.cpp` ~16624-16872: the coat is a
  co-located virtual neighbour, and a deep coat eats inward. The material JSON
  `coat` block is parsed by `ParseCoat` (`materials.cpp`).
- **Held items are rig limbs** at index `>= baseLimbs_`. They have their own
  lattices, so their voxels already carry a coat word. The contact, splash and
  burn passes loop over all of `limbs_`. `StainWoundAs` refuses held slots.
- **Hands are base limbs** with `tag "hand"`.
- **Worn armour already persists its exact lattice** off the body:
  `WornShellDamage::lattice` (`src/game/iteminstance.h:53`). `PrefabVoxel`
  includes the stain word.
- **Melee:**
  - `MeleeSweepDamage` (`src/game/melee.cpp` ~978) resolves the contact point
    `at` plus the struck limb. The weapon is an authored edge SEGMENT; which
    weapon VOXELS touched is never resolved.
  - `BuildStrikeParts` produces `KerfCut` / `BluntHit` / `BiteHit`.
  - `CutLimb` (`mob.cpp` ~12465) carves with NO `CarveReport`.
  - `BiteHit` and `RotAtSpawn` DO get `CarveReport{cells}`.
  - Blunt hits go through `Mob::BluntHit`, which calls `BruiseLimb`.
  - A fist is the same sweep: `EffectorWeapon`, `StrikeProfileFor`,
    `blunt.unarmed`.
- **Zombie rot:**
  - Seeded by `Mob::BiteHit` (`StainWoundAs` with `infectMat`, then
    `limbs_[li].infectMat/infectStain`).
  - Ticked by `Mob::InfectTick` / `InfectStep` (`mob.cpp` ~17590-18055, CPU,
    deterministic `Hash3` keys).
  - Rates come from `gore.infect*` tuning. Per-tissue admission is `rotRate`.
    The floor is `kInfectFloor = 4`.
  - hp comes through `CarveLimb` / `FlushBurn` accounting, booked as
    `DamageCause::Burn`.
- **Inventory:**
  - `ItemSlot` in `src/ui/inventory_ui.cpp` ~257: double-click currently
    opens the alchemy bench for VESSELS only.
  - The alchemy bench is a 2D CPU-painted sim uploaded as a texture
    (`alchemy_bench.*`, `main.cpp` ~6685, ~17171). That is the precedent for
    a CPU-rendered panel.
  - The portrait pour brush is `session.cpp` ~3726-3800, using
    `PourBrushCellsPerSec` and `ContainerTopMat`.

## Rules every package follows

- **Work in your worktree and commit on your branch. Do NOT merge to main.**
  The orchestrator merges.
- **Build ONCE with `bash scripts/build.sh`**, after the code is written. Every
  `build.sh` / `run.sh` call uses `run_in_background: true` OR
  `timeout: 600000`.
  - Never taskkill by image name.
  - Max 2 retries on a GPU command. A held lock (`bash scripts/svlock.sh`)
    means a queue, not a failure.
  - A LNK1104/LNK1181 on a shared dep means a concurrent link: wait about 45
    s and retry.
- **Verification is ONE `--verify <your gates>,<2-3 neighbouring gates>` boot
  at the end.**
  - No `--suite acceptance`, no bare `--selftest`, no smokes.
  - Run the exe via `bash scripts/run.sh` from your worktree.
  - A gate that was red at clean main is not yours. Attribute with a control
    arm before claiming anything.
- **Body state is CPU and must stay deterministic.**
  - Integer or counter-based `Hash3` RNG keyed on (mob id, limb, tick, salt).
  - Iteration order must not depend on pointers or hash-map order.
- **Thresholds and rates are data.**
  - Material behaviour goes in `materials.json` / `reactions.json`.
  - Global knobs go in `src/sim/tuning_params.def` rows, following the
    six-step recipe in CLAUDE.md, including `gen_tuning_params.py` and the
    `tuner_schema.js` row.
  - Gate expectations go in `tests/baseline.json`.
- **NO material ids or names hardcoded in C++.** Behaviour comes from JSON
  fields and tags.
- **Document in DESIGN.md** in the section named in your package (different
  packages, different sections, so merges stay clean). Do not touch
  `assets/tuner.html` or CLAUDE.md; the orchestrator does those at
  integration.
- **Final report:** branch name and HEAD sha; what landed; gates and their
  results (quote the gate lines); anything stubbed or deferred; any hash pin
  you expect to move.

---

## Package A: coat transfer on contact (C++)

**Goal.** When any striking limb (held weapon, worn gauntlet, bare fist, jaws)
lands a hit, part of the coat on the striker's voxels AT THE CONTACT moves onto
the target. Part of the target's matter at the contact (its coat, and the
wound's own bleed fluid) moves back onto the striker. There is no special
case per material.

**A1. Which striker voxels touched.**
- Transform the contact point `at` into the striking limb's local lattice. Use
  the same transform the renderer and collider use; find it, don't invent it.
- Gather the coat words of the striker's SURFACE voxels within a contact
  radius. Base the radius on `edgeHalfWidth` for blades, the probe radius for
  blunt and fist, and a `gear.coatContactRadius` knob as a floor or scale.
- Use the authoritative stain lattice: the skin if it is finer, otherwise the
  collider (`StainLattice`).
- Result: a small list `{mat, amount}` per touched voxel. If only the tip is
  coated, a hit with the base must transfer NOTHING.

**A2. Striker → target.**
- **Cuts and stabs.** Make `CutLimb` produce a `CarveReport` (cells
  removed), as `BiteHit` already does. Distribute the transferred coat onto the
  WOUND WALL: the surviving voxels face-adjacent to removed cells. Put more on
  the cells nearest the contact.
- **Blunt and unarmed.** Put the coat on the bruise footprint (surface voxels
  around the contact). It goes deeper only where that skin split or where the
  surface is already exposed (no skin voxel there: an open wound or a stump).
- **Shells.** A hit on a worn shell coats the shell surface.
- **Writes.** Use the existing writers so `stainPrecedence` decides who wins.
  Spend from the striker what was actually written; amounts are conserved.
  Knobs:
  - `gear.coatTransferFrac`: share of the touched coat that leaves per hit.
  - `gear.coatTransferMax`: cap per hit.
- Mark the coat dirty and poke the render (`MicroBodyPokeStain`) on both
  bodies.

**A3. Target → striker.**
- The target's coat at the contact moves the same way in reverse. This covers
  a blade hitting an acid-coated body, among others.
- The wound's bleed fluid (the limb's bleed material; for a cut, blood) coats
  the striker voxels that went in. Reuse the bleed material lookup; do not
  name `blood`.
- With precedence, blood can then displace a weaker coat on the blade. That is
  intended: a venom coat wears off after a few dirty hits.
- `StainWoundAs` refuses held slots so that "a sword never bleeds". Keep that
  refusal. The new reverse path writes coats, not bleeding.

**A4. Coats persist with the item.**
- Generalise the exact-lattice persistence armour already has
  (`WornShellDamage::lattice`) so a HELD weapon's lattice, and therefore its
  coat, is captured into its `ItemInstance` when it leaves the hand, and
  restored when it is drawn again. It must survive save and load (check the
  item savers).
- While the item is in the bag, nothing ticks it (FROZEN, owner decision).
  While held or worn, the existing drying and decay passes run on it as they
  do on any limb. Verify drying does reach held slots.
- **API contract for package C** (C builds on this; keep the names):

```cpp
// src/game/itemcoat.h (new)
// The item's exact voxel lattice, materialised as authored on first use.
// Coats live in PrefabVoxel::stain like on a body.
std::vector<PrefabVoxel>& ItemLatticeMut(ItemInstance&, const ItemDef&);
const std::vector<PrefabVoxel>* ItemLatticeIfAny(const ItemInstance&);
// lattice dims + the authored lattice for an item (for viewers):
bool ItemLatticeDims(const ItemDef&, int& sx, int& sy, int& sz);
// push an edited ItemInstance lattice onto the live limb if the item is
// currently held/worn by `mob` (no-op otherwise), and the reverse capture.
void PushItemLatticeToLimb(MobSystem&, MobId, KitRef);
void CaptureLimbToItem(MobSystem&, MobId, KitRef);
```

  Adapt the signatures to the real types, but keep the one-header,
  one-concept shape. Record the final signatures in your report.

**A5. Fists.** Hands already carry coats; check they pick up coats by contact
(dipping a hand in a pool) and transfer them on punches through the same A1-A3
path.

**Gate `coat-transfer`** (`src/test/`, tick with `support::RunTicks`, file
`selftest_wound.cpp` or a new `selftest_coat.cpp`). Use whatever test liquid
already has a distinctive body coat; do NOT depend on package B's venom.
- Blade coated on the TIP half only. A strike that lands with the base leaves
  0 coat in the wound. A strike with the tip leaves > 0 coat on wound-wall
  voxels.
- After the tip strike: the tip's coat amount fell, and the blade's contact
  voxels carry the target's bleed fluid.
- Blunt: a coated club on intact skin puts coat on surface voxels only, with
  no coat below depth 1.
- Fist: a coated hand transfers on a punch.
- An item's coat survives unequip → bag → re-equip with the lattice
  identical, and stays unchanged across N ticks in the bag.

**DESIGN.md:** a new subsection beside "A coat is a substance" (about line
6944): "A coat moves on contact".

---

## Package B: generic infection + venom (C++ + JSON)

**Goal.** Rot stops being one hardwired infection. An "infection" is any
material with an `infect` block. Venom is the second one.

**B1. Data-driven infection.**
- A material may carry an `infect` block, roughly:

```json
"infect": { "spread": <world vox/min>, "eat": <world vox/min>, "floor": <int>,
            "targets": ["tag:flesh", ...] , "leaves": "<material or null>" }
```

  Choose names and units that fit `ParseCoat` / the materials loader style.
  `rotflesh` gets a block carrying today's numbers (from the `gore.infect*`
  knobs and the per-tissue `rotRate`). Zombie behaviour must NOT change:
  existing infection, bite and rot gates must stay green at the same values.
  Keeping the `gore.infect*` knobs as the source of rotflesh's numbers is
  fine.
- **The infection's identity is the VOXEL's material.**
  - `InfectStep` spreads from each infected voxel using ITS material's block.
  - It admits a neighbour only if that neighbour matches the block's targets.
  - It eats using that material's eat rate.
  - Per-limb state becomes, at most, which infection materials are active.
    One limb can hold rot and venom at once.
  - Keep the counter-based RNG keys and add the material to the salt.
- **Damage** gets a new `DamageCause` per infection, or one generic "infection"
  cause with the material recorded. The death cause for venom must read as
  venom/poison, not "Burn". Keep rot's current booking if changing it would
  move a gate.

**B2. A coat can seed an infection.**
- A coat material may name an infection (e.g. `coat.infects:
  "<infect material>"`).
- In the coat pass (`BurnOneLimb` section 0, or beside it): where such a coat
  sits on, or is face-adjacent to, a voxel matching the infection's targets,
  that voxel converts to the infection material. Each conversion spends coat
  levels, so the dose is bounded by the coat.
- Skin is not a target, so a coat on intact skin does nothing but sit there
  and dry. A coat in a wound or on a split bruise touches flesh and seeds.
- This must be the generic path. Nothing may test for "venom".

**B3. Venom content (`materials.json`, `reactions.json`).**
- `venom`:
  - a liquid, viscous, a sickly yellow-green;
  - a bodyOnly stain plus a `coat` block (`decay` slow enough that a coated
    blade stays usable for a fight; `opacity` visible);
  - `coat.infects: "envenomed"`.
  - On the ground it is just a liquid. No world reactions are required.
- `envenomed`:
  - the tissue venom leaves: a solid organic material, dark and
    discoloured, tagged so remedies and fire treat it like rotted tissue where
    sensible;
  - its `infect` block targets SOFT tissue only (flesh, muscle, organs
    including brain; NOT skin, NOT bone);
  - `eat` = 1.5 × `spread`, `floor` 0.
  - Rate: a single sword wound's dose (on the order of 5-20 seeded voxels on
    a human limb) should spread and eat for roughly 60-120 s, then stop.
    Expected total eaten ≈ 3× the seeded dose. Write that arithmetic in the
    DESIGN text.
- `venom_gland`:
  - an organ tissue material whose bleed fluid is `venom` (see the per-material
    bleed, "bleedFluid is the matter's"), so cutting a snake's gland leaks
    venom you can scoop with a flask;
  - give it the anatomy tags that organs use.
  - Package D will place it in the snake's head.
- Pick ids following the materials file conventions. Body tissue ids must stay
  <= 127 if they go into mob `.vox` palettes (CLAUDE.md layout table).

**B4. Remedies.** If a "disinfect" coat effect clears rot today, decide whether
it should clear venom too, and make it data. Prefer: an effect that names the
tags it clears.

**Gate `venom-wound`.**
- Seed a human limb's exposed flesh with a venom coat (no package A needed:
  pour or `SoakCut` into a carved hole). `envenomed` voxels appear, spread,
  then the count returns to 0 within the expected window.
- Removed voxels ≈ 3× the dose, within a tolerance kept in `baseline.json`.
- Limb hp fell.
- A venom coat on INTACT skin converts nothing.
- Bone is untouched.
- Rot still behaves (the existing infection gates are your neighbours in
  `--verify`).

**DESIGN.md:** in or beside the zombie rot / infection section: "Infection is
a material".

---

## Package D: the snake (C++ + assets)

**Goal.** A snake mob that slithers convincingly, aggroes, and BITES. The bite
punctures a couple of voxels deep and leaves venom in the wound, the way a
zombie bite leaves rot.

**D1. Model.**
- `assets/mobs/snake.json` + `snake.vox`. Generate the `.vox` with a script
  under `scripts/`, the way other generated mobs are made.
- A segmented body: head, about 8-12 body segments, tail. Each is a limb with a
  ball joint and spring to its parent; read how `critter.json` does `tail`.
- About 1.5-2 m long and thin.
- Anatomy layers skin / flesh / muscle / bone. Bake through the anatomy
  pipeline (`assets/editor/anatomy.js`, `node scripts/anatomize_mob.mjs`) if
  it supports this rig; otherwise author the layers directly.
- A `venom_gland` tissue pocket in the head. Until package B lands, use a
  placeholder tissue name; the orchestrator renames it at merge. Make that
  ONE string in `snake.json`.
- Scaled colour pattern.

**D2. Locomotion: slithering.**
- No legs, so the gait is disabled (`disableGait` exists; also see the crawl
  and ground-conform work in `src/game`).
- Motion is lateral undulation: a travelling sine wave of yaw along the
  segment chain. The head leads, the phase lags per segment, and the
  amplitude grows toward the middle.
- Forward speed ties to wave frequency, so the body does not skate.
- Segments ride the ground (conform to the terrain height under each
  segment).
- Turns bend the wave.
- Find how existing mobs drive limb poses procedurally (springs, the pose
  system, `strokes.h` mentions a rearing snake prone form) and use THAT
  mechanism; do not build a parallel one.
- Wave parameters are data in `snake.json`.

**D3. Behaviour and bite.**
- A `behaviors.json` entry: wander and idle-coil, aggro on a nearby player or
  NPC, approach, then strike.
- The strike: the head and front segments draw back (an S coil), lunge, bite,
  and recover.
- The bite uses the existing `BiteHit` path with a SHALLOW blob: "punctures a
  couple of voxels in". `infect` = the infection material and `stain` = the
  venom liquid, the same fields the zombie uses
  (`assets/mobs/effects/zombie.json` `bite {infect, stain}`). Until B lands,
  point them at the zombie's `rotflesh` / `ichor` and list that in your report;
  the orchestrator switches them to `envenomed` / `venom`.
- Low hp. It can be cut in half (severable segments).

**D4. Spawn.**
- At minimum, the dev spawn list (find how `critter` and humans are spawned
  from the F1 dev panel).
- A natural spawn is welcome if the biome feature stack already supports mob
  spawns; do not build that system if it doesn't exist.

**Gate `snake`.**
- Spawned on flat ground, it travels > X m in N ticks toward a target with
  every segment within Y of the ground, and the segment lateral offsets
  alternate sign along the body (a real wave, not a rigid rod).
- Placed beside a dummy or human, it strikes within T ticks and leaves the
  bite's infect material in the victim.
- Thresholds go in `baseline.json`.
- Also produce one `--shot`-style screenshot of the snake mid-slither, if a
  fixture for mob shots exists (`--shot-mob`), and report the path.

**DESIGN.md:** in the mobs/creatures section: "The snake".

---

## Package C: the item stage (C++ + UI). Wave 2, after A merges

**Goal.** Double-click a weapon or armour piece in the inventory (bag, hotbar
or equipped). An **item stage** panel opens in place of the spellbook column,
the way the alchemy panel does. It shows ONLY that item, neutrally lit and
pixel-art framed:
- left-drag orbits;
- the wheel zooms;
- a "reset view" button.

With a vessel selected in the FLASKS row, painting on the item splashes the
vessel's top substance onto the voxels under the brush.

**C1. Rendering.**
- Render the item on the CPU into a texture, like the bench. Raymarch the
  item's voxel lattice (it is tiny) with DDA:
  - per-voxel material colour (palette, `.vox` art colours);
  - the coat overlay drawn the way `microbody.wgsl` `bodyStainCover` draws it
    (the material's `stainColor`, opacity, a mottle approximation);
  - simple key + fill + rim lighting and an outline.
- No new GPU pipeline. The same ray gives an EXACT brush pick.
- Texture resolution is pixel-art friendly and upscaled with nearest
  filtering.
- The renderer is its own file, e.g. `src/ui/item_stage.{h,cpp}`, reusable by
  the future crafting and repair bench. Keep the "which voxel is under this
  pixel" API public.

**C2. Applying.**
- **Brush.** The radius is in item voxels; a slider or wheel+shift sets it.
  The rate is `PourBrushCellsPerSec`. Spend from the vessel exactly like the
  portrait brush (`session.cpp` ~3726-3800; milli-eighth accumulator,
  `ContainerTopMat`, `ContainerIsolateOne`).
- **Writes.** Use `AddBodyStain` / `WashBodyStain` with precedence, so water
  rinses a coat off and acid over oil does what the body rule says. No special
  "wipe" tool: washing is water.
- **Authority.** The stroke is an INPUT carried into the tick, like
  `session.pourStroke` (multiplayer authority: the UI never mutates the
  item). The tick applies it to `ItemLatticeMut` and, if the item is held or
  worn, calls `PushItemLatticeToLimb` (package A's API in
  `src/game/itemcoat.h`). Read A's merged code first.
- **Info strip.** A small readout under the view: the item name and a coat
  summary (top substances by coverage, as the per-limb ledger shows them).
- Done / Esc closes the panel. Esc order: dialogue, bench, stage, inventory.

**C3. Input.**
- Double-click on Melee / Armor* items opens the stage. Vessels keep opening
  the bench.
- Tooltip hint: "double-click to inspect".
- Mouse-look stays gated because the inventory is open.

**Gate `item-stage`** (CPU-only, no GPU needed).
- Render a known item: the image is non-empty and the item silhouette is
  centred.
- A pick through the centre pixel hits a voxel.
- A scripted stroke with a filled water/oil vessel coats > 0 voxels, spends
  the vessel, and survives close → reopen.
- With the item held, the live limb's lattice matches the item lattice.

**DESIGN.md:** in the items/equipment section: "The item stage".

---

## Integration (orchestrator)

1. Merge A, B and D. Switch the snake's bite to `envenomed` / `venom` and its
   gland to `venom_gland`.
2. Launch C on the merged tree.
3. Merge C.
4. Update `tuner.html` `ARCH_NODES` and the dev status panel, and add the
   CLAUDE.md map rows if needed.
5. Run one `--verify coat-transfer,venom-wound,snake,item-stage,<rot/bite
   neighbours>`. Run `--selftest --gate determinism --rebaseline` only if the
   pin moved.
6. Rebuild main's exe.
7. Play-check: coat a sword with snake venom, cut a human, and watch the wound
   rot out over about 1-2 minutes.
