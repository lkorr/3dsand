# PLAN: Armour, weapons, and equippable items

Status: **LANDED 2026-08-29** (P0-P9 complete; see §10 for what the plan got
wrong and what changed). Owner decisions taken up front and both honoured: the
sword is wielded via the **Sheath slot + a draw key**; armour damage
**persists exactly** (carved lattices round-trip through the save).

Architecture of record is now **DESIGN.md §8c**. This file is kept as the
design record — what was decided, what was rejected, and what turned out to be
false.

Original preamble: written for an implementing agent who has not seen the
research; every claim below carries a file:line so you can verify rather than
trust.

## 0. Base tree — read this first

Work happens on branch `worktree-armor-and-equippables` in the worktree
`.claude/worktrees/armor-and-equippables`. That branch is now merge commit
`75b385c` = origin/main's waterbody line (`3d713ae`) merged with the local
`main` line (`f941b31`, which carries the character screen `5aacb11`, the stock
human body `fa9fcf5`, and locomotion `3092dde`). The only merge conflict was
`fluid_bench.json` (generated bench data; the waterbody side was kept).

Two things follow:

1. **This merged tree has never been built or tested.** P0 below is a one-time
   baseline build+selftest. That run is justified under the verification
   budget: the tree is new, so it establishes a claim nothing else establishes.
2. **Local `main` and `origin/main` have diverged** (fork point `fe53302`;
   waterbody M4/M5 exist only on origin/main, character screen only on local
   main). Do not push; do not try to reconcile the two mains — that is the
   repo owner's call. Land this work by merging this branch into local `main`
   when done, as usual.

Claim files on the board before editing (`bash scripts/board.sh claim ...`):
`src/main.cpp`, `src/game/mob.cpp`/`mob.h`, `src/game/item.h`,
`src/game/equipment.h`, `src/game/melee.cpp`, `assets/materials/` (if touched),
`DESIGN.md`, `assets/tuner.html`.

## 1. What already exists (verified on the merged tree)

The task is much smaller than "build an item system" — most of it shipped in
the last week. Ledger, with the load-bearing locations:

| System | State | Where |
|---|---|---|
| Item data model: `ItemDef`, `ItemLibrary`, `ItemGrip` (context map), `ItemHilt`, `ItemStack`, 10-slot `Inventory` hotbar | EXISTS | `src/game/item.h` (whole file; loader `LoadItems` lives in `src/game/melee.cpp:257`) |
| Sword item: art, sidecar (grip/hilt/edge/spring/hp), behaviour row (damage 14, carveBonus, reach) | EXISTS | `assets/items/sword.{vox,json}`, `assets/items/items.json`, `scripts/gen_sword_item.py` |
| Held-item attachment: `Mob::EquipItem` appends a real rig limb ("borrowed slot") at the `held_right` socket; sever/drop/carve/burn/micro render inherited for free | EXISTS | `src/game/mob.cpp:4440-4646` (line numbers pre-merge; re-locate), rationale `src/game/item.h:32-53` |
| Mouse-directed melee: swing state machine, IK-driven hand, edge sweep, Jolt-ray hits, `Damage`+`CarveLimbRadial` | EXISTS | `src/game/melee.*`, tick wiring `src/main.cpp` (search `meleeArmed`) |
| Character screen: `I` opens, live avatar portrait (own submit), 8 armour slots + Sheath + Quick0-3, 4×8 bag, hotbar row, drag with validated swaps, injury inspector | EXISTS | `src/ui/inventory_ui.cpp`, `src/ui/overlay.h` (UIState kit block), `src/main.cpp` (latch consumption; search `ui.moveItem.pending`) |
| Equip schema: `EquipSlots()` table — one row per slot naming accepted `ItemKind`s. **All 8 armour rows accept nothing**; Sheath/Quick accept `Melee` | EXISTS (armour rows stubbed) | `src/game/equipment.h:48-133`, DESIGN.md §8b (~line 4549) |
| `PlayerKit` (bag+hotbar+equipment), `KitRef` addressing, `Move` swap validated both ends, `MoveResult` refusal text | EXISTS | `src/game/equipment.h`, `src/game/kitref.h` |
| Persistence: `PLYR` v1 section, name-keyed, truncation/version refusal | EXISTS | `src/game/persist.cpp` |
| `--gate player-kit` (~40 checks: refusal matrix, swap, name re-resolution, round trip) and `--shot-inventory` (scripted screenshots) | EXISTS | `src/test/selftest_playerkit.cpp`, `src/main.cpp` (search `kShotInvOpenFrame`) |
| Stock `human` avatar: 15 limbs, **every voxel material `skin`**, colour in `.col` art layers inside `human.vox`, 17 world voxels tall, only socket `held_right` | EXISTS | `assets/mobs/human.{vox,json}`, `scripts/gen_human.py` (limb boxes: `ART_LIMBS` ~line 516) |
| Body reactivity: CPU evaluator runs the authored reaction table over limb voxels; fire seeds from world contact, acid matches `tag:dissolvable`; tombstone → batched carve → hp charged by lost volume → sever | EXISTS | `src/game/mob.cpp` `BurnOneLimb` (search), `src/sim/reactcpu.h`, `docs/PLAN_body_reactivity.md` |
| COW micro bricks: first damage clones the shared brick, per-voxel `MicroBodyPoke`, carve re-derives collider | EXISTS | `src/sim/microbody.h`, `Mob::ReskinLimbMicro` |
| Armour design intent | WRITTEN | `docs/PLAN_body_reactivity.md` §"Package D — armour and clothing" (~line 349) |

What does NOT exist (the actual work):

- Any wearable `ItemKind` (enum has `None, Melee` — `src/game/item.h:61`).
- More than one simultaneous attachment (`heldSlot_` is a single int).
- A multi-limb item (a robe covering torso+arms+hips).
- Armour blocking fire/acid from the body underneath (shells are separate
  lattices; the body's burn pass samples the world directly).
- Any non-uniform scaling anywhere in the pipeline (every scale is a uniform
  integer lattice divisor) — needed for "goblin helmet on a human".
- Drop/pickup. `EquipItem(nullptr)` DESTROYS the body (`DestroyJoint`+
  `RemoveBody`); nothing in the world can enter an inventory.
- Armour content. Also: mina's outfit is NOT extractable clothing — her hips
  limb IS the robe skirt, her head IS the hood, welded geometry + materials
  (`scripts/gen_mina.py:35-45`). The stock armour set is authored fresh, using
  mina as the visual reference only.

## 2. Architecture (decisions, with reasons)

### 2.1 A worn piece is a set of borrowed rig slots ("shells")

Follow the held-item precedent exactly (`item.h:32-43`): wearing appends one
rig limb per covered body part — a **shell**: `parent` = the covered limb,
`joint` = Fixed, `tag = "worn"`, `vital = false`, its own hp, its own voxels,
its own micro brick. This inherits, with no new code: burning and dissolving
(the burn pass runs over every limb), per-voxel carving, severing (a
destroyed strap drops the pauldron), drop-to-debris, live-transform hitboxes
(a sword blow hits the shell's Jolt body before the arm's), and rendering.
"Degraded armour shows the body underneath" is automatic: the body limb is
enclosed by the shell and appears through its holes.

Rejected alternative: folding armour voxels into the body limb's own lattice
(the way mina's robe works). Burn ordering would be free, but unequip,
per-piece hp, sever-as-a-piece, drop, and persistence all become entangled
bookkeeping in one lattice. Separate slots keep one owner per fact.

### 2.2 Reactivity is materials + geometry, never flags

Cloth armour burns because it is `robe_cloth` (`tag:hot` neighbour rule,
chance 200/1000 vs skin's 90). Steel blocks acid because `steel` does not
carry `tag:dissolvable`, so `matAttacksBody_` never matches it. **Add no
resist/immunity/armour-value fields.** The one genuinely new mechanic is
**occlusion** (§2.3): the shell must also stop the world from reaching the
body while it's intact.

### 2.3 The worn-occlusion probe

Today `BurnOneLimb` seeds fire on a body limb from world contact cells and
evaluates inbound acid from world neighbours — it never knows a shell is in
the way (shell voxels are not in the grid, and not in the body limb's
lattice). Add one helper on `Mob`:

```
// Is worldPos inside a live voxel of any shell worn over bodyLimb?
bool WornOccludes(int bodyLimb, const Vec3& worldPos) const;
```

Implementation: for each shell registered over that limb, transform worldPos
into the shell's lattice (`RotateInv(q, p - xf.pos) * scale` — the same
transform the burn pass already uses on its own limb) and test occupancy
against a per-shell dense index (reuse the `BodyBurnState::idx` pattern or a
bitset rebuilt on carve). Consult it in exactly two places in `BurnOneLimb`:

1. **Seeding**: skip a world contact face-sample whose sample point is
   occluded (the fire is touching the robe, not the arm).
2. **Inbound world neighbour**: if the world-side neighbour position is
   occluded, substitute the shell's material at that cell for the world
   material. Flesh's neighbour becomes "cloth" instead of "fire"/"acid" —
   cloth-over-flesh semantics fall out, and the moment the shell burns
   through, the probe returns air there and the skin is exposed. Emergent,
   no integrity thresholds.

Cost: only runs when `scanHot` is non-empty (the existing cheap gate), over
at most a handful of shells. Determinism note: this reads Jolt transforms,
which the burn pass already does for its own sampling; keep every RNG draw
counter-based (`docs/PLAN_body_reactivity.md` §5 trap) and this adds no new
risk class. Mob state is game-layer, never hashed.

### 2.4 Fit: authored at stock size, resampled per-wearer at equip

Research summary (the codebase has NO non-uniform scaling; here is why adding
one narrowly is cheap and safe):

- Shell lattices are small integer boxes. A per-axis nearest-neighbour
  resample (`src = (i * srcDim) / dstDim`, integer math) is deterministic,
  hole-free, single-pass, and matches the blocky aesthetic for ratios up to
  ~2×. This is the editor's `upscale2x` generalized to rational per-axis
  ratios.
- Each cover entry records the **fitBox** it was authored against (the stock
  human limb's world-voxel box, from `gen_human.py`'s `ART_LIMBS`). At equip:
  `ratio[axis] = wearerLimbBox[axis] / fitBox[axis]` (wearer box =
  `MobLimb::size` scaled to world voxels). Target dims, anchors and offsets
  all scale by the same rationals.
- Per-wearer voxels need a per-instance brick — the COW pool already models
  exactly this (`MicroBodyOwn`); pack the resampled lattice as an owned brick,
  free it on unequip. When ratio == 1 on all axes (stock set on the stock
  human), skip the resample and share the def's brick until first damage,
  exactly like body limbs.
- Rejected: procedural shell generation from the wearer's surface (dilate 1).
  Perfect fit but loses authored silhouettes (the hood cone); revisit later
  for generic "cloth drape".

Cover entries bind by **limb name** (`head`, `armU.L`, ...). All humanoid
rigs in the repo share these names; a wearer lacking a named limb simply
skips that cover entry (a sleeve on a one-armed mob attaches to the arm that
exists). This is what makes "goblin helmets look right on anyone" content,
not code.

### 2.5 Sword: Sheath slot + draw key (owner decision)

The sword item and swing already exist end-to-end. The change is where the
"held" resolution comes from:

- The Sheath equip slot is THE weapon slot. A draw/stow key (propose `X`;
  **verify it is unbound** in main.cpp's key handling first) toggles drawn
  state: draw → `avatar.EquipItem(sheathedItem)` + auto-select
  `UIState::kToolMelee`; stow → `EquipItem(nullptr)` + restore the previous
  tool.
- `meleeArmed`'s item source (`src/main.cpp`, search `hotbar.HeldDef()`)
  switches from the hotbar to the drawn-from-sheath item. The hotbar remains
  for future consumables/tools; the number row keeps selecting hotbar slots.
- Sheathed-but-not-drawn rendering on the back (`sheath_back` socket + a
  `"sheath_back"` grip context on the item) is anticipated by DESIGN.md §8b
  as pure content — OPTIONAL, do last if at all.

### 2.6 Ground items: adopt debris, remember identity

Dropping and picking up reuse the debris pipeline rather than inventing a
world-item entity:

- **Drop**: build the item's voxels into a dynamic body
  (`CreateDebrisBodyXf`) and `DebrisSystem::AdoptBody` it (severed held items
  already take exactly this path and burn/settle correctly). Record identity
  in a new small registry `WorldItems`: `body handle → item name` (+ carved
  lattice if damaged). Sources: a `ui.dropItem` latch from the character
  screen (drag out of the panels), and unequip-when-bag-full refusal stays a
  refusal (message, no drop).
- **Pickup**: interact key `E` — short camera ray (`CastRayBody`, the melee
  precedent) filtered through the `WorldItems` map; on hit, `Bag`/hotbar
  `Add`, remove the body, drop the registry entry. Bag full → `kitMessage`
  refusal, item stays.
- DebrisSystem must notify when it destroys an adopted body (burned away,
  culled) so the registry drops the entry — `OnBodyReleasedToWorld` /
  `ReleaseBody` is the seam; a burned-up robe on the ground is GONE, which is
  correct.
- **Persistence**: new save section `ITMS`: count × {name, transform,
  optional carved lattice}. Registered beside `PLYR` in
  `src/game/persist.cpp`, same refusal discipline.

### 2.7 Wear path lives on `Mob`, not the avatar

`Mob::WearItem(const ItemDef*, slot)` / `UnwearItem(slot)` beside `EquipItem`,
so any mob can wear a goblin helmet through the identical path
(`MobSystem::WearItem(mobId, ...)` id-keyed wrapper, mirroring
`MobSystem::EquipItem`). The avatar inherits it. AI dressing is out of scope.

**The removal seam is the one real refactor.** Today the appended-slot
invariant is "appended LAST, removed by pop_back" (unequip pops the tail and
the four parallel vectors stay aligned). Multiple worn pieces + a held item
break that: removing a middle group must not renumber the others. Approach:
on any worn/held removal, tear down ALL appended slots (joints+bodies), then
re-append the survivors in order, **carrying each survivor's carved lattice,
owned brick index, and hp across the re-append** (move them out, re-attach —
do NOT re-load from the def, that would heal damage). Appended slots are few
(≤ ~10) and removal is rare, so rebuild cost is irrelevant. Assert
index-parallelism (`limbs_/skel_.parts/limbDefs_/hidden_` same size) after.

Bookkeeping structure:

```
struct WornPiece {
  int equipSlot;                // EquipSlotId as int
  std::string item;             // resolve by name (library indices are file-order)
  std::vector<int> slots;       // appended rig slot per cover entry
};
std::vector<WornPiece> worn_;   // beside heldSlot_
```

## 3. Data schema changes

### 3.1 `ItemKind` (src/game/item.h)

Add: `ArmorHead, ArmorChest, ArmorLegs, ArmorBoots, ArmorShoulders,
ArmorHands, ArmorBelt, Trinket`. DESIGN.md §8b promised exactly this shape:
"The day `ItemKind::ArmorHead` exists, the change is one row." Fill the 8
armour rows' `accepts` in `EquipSlots()` accordingly (`equipment.h`).

### 3.2 `ItemDef` cover entries

```
struct ItemCover {
  std::string part;      // body limb name this shell attaches to ("torso")
  std::string model;     // model name inside the item .vox ("robe.torso")
  Vec3 offset;           // shell min-corner relative to the covered limb's
                         // min corner, in the item's authored world voxels
  Vec3 fitBox;           // the stock limb's world-voxel box this was authored
                         // against (from gen_human ART_LIMBS / 4 art-micro)
  float hp;              // per-shell hp (piece durability is the sum)
};
std::vector<ItemCover> cover;   // empty for Melee/None kinds
```

The armour item `.vox` is multi-model, like a mob `.vox`: one model per cover
entry plus optional `<model>.col` art layers (the loader already folds those —
`src/sim/voxload.cpp`, search `kArtLayerSuffix`). `LoadItemAsset`
(`src/game/melee.cpp:23`) currently assumes one model; generalize it to load
the cover models by name. Sidecar example (`assets/items/robe.json`):

```json
{ "scale": 8, "severable": true,
  "cover": [
    {"part": "torso",  "model": "robe.torso",  "offset": [-1,-1,-1], "fitBox": [2.5,1.5,4.5], "hp": 20},
    {"part": "armU.L", "model": "robe.armU.L", "offset": [-1,-1,-1], "fitBox": [1,1,2.75],   "hp": 8},
    ... armU.R, armL.L, armL.R, hips ... ] }
```

and an `items.json` row `{"id": "robe", "kind": "armor_chest"}`. Parse kinds
by string exactly as `"melee"` is parsed today.

### 3.3 Materials

**No new materials.** The stock set uses existing rows: `robe_cloth` (robe,
hood — flammable, dissolvable), `robe_trim` (sash), `leather` (boots — slower
to catch, chance 25 vs 200). Black/grey colour rides in the art layers
(palette slots 128-255 → `Prefab::artColors`), NOT in materials — same split
the human body uses, and it keeps the reaction table untouched. A future
steel piece is also zero material work (`steel` exists, acid-proof by
tag-absence). Do not hardcode material IDs anywhere (repo rule).

## 4. Work packages

Ordered; each lands green on its own gate before the next starts. P1-P3 are
independent of P4-P6 after P1 lands.

### P0 — baseline the merged tree (half a day)

`bash scripts/build.sh --selftest`. This tree (waterbody × character-screen
merge) has never compiled. Expect: gates green, determinism hash `b717a33d`
unchanged (nothing in either line touched the sim since the pin... verify
against `tests/baseline.json`, which is the truth, not this sentence).
Known-failing gates listed in baseline.json stay known-failing. Also run
`--gate player-kit` and `--gate mob-burn` explicitly if the full run flags
them: the merge combined main's de-hardcoded fixtures (`wizard` deleted,
`human` added) with the waterbody line's test harness. If anything fails,
attribute before proceeding (revert-only-your-files rule — but you have no
files yet, so a failure here is the MERGE, report it).

### P1 — wear/unwear on Mob + equip-slot wiring (2-3 days, the core)

- `ItemKind` armour values + `EquipSlots()` accepts rows + `LoadItemAsset`
  cover parsing (§3).
- `Mob::WearItem/UnwearItem` + `worn_` registry + the removal-seam refactor
  (§2.7). Shells: `tag "worn"`, Fixed joint, parent = covered limb,
  `DisableCollisionsAmong` (existing call covers appended bodies), excluded
  from `worldSize` (the tag-skip exists for `prop`/`item` — extend to
  `worn`), excluded from the HUD body figure (`BodySlotFor` returns -1 for
  unknown tags already — verify).
- Wire the character screen: when `PlayerKit::Move` lands an armour item in /
  out of an equip slot, call `WearItem`/`UnwearItem` on the avatar (same
  change-detection seam the melee equip uses: compare wanted vs worn, in
  main.cpp after latch consumption). Unequip back to bag does NOT destroy
  voxel state: keep the piece's carved lattice + hp in the `Equipment` slot's
  stack entry (a small side blob keyed by slot) so re-equipping is not a heal.
- First-person hide mask (main.cpp, search the keep-list with `p.armUL`):
  keep worn shells whose parent part is in the keep list.
- **Gate `armor-wear`** (new, CPU+phys, no render, registered in a new
  `src/test/selftest_equipment.cpp` or beside player-kit): human wears the
  robe → N shells appended, parents correct, vectors index-parallel;
  `worldSize` unchanged; unwear → gone; wear two pieces, remove the FIRST,
  assert the second's slots still resolve and its lattice survived; carve a
  shell, unwear, re-wear → holes still there.

### P2 — the stock set: `scripts/gen_stock_armor.py` (1-2 days, parallelizable with P1 after schemas freeze)

Generates `assets/items/{hood,robe,sash,boots}.{vox,json}` + items.json rows.
Visual reference: mina (`scripts/gen_mina.py` — hood cone `:216-280`, skirt
`:328-374`); recolour black/grey via art palette slots (emit its own palette —
palette indices are file-local, never copy voxels between .vox files).
Geometry rule: each shell is a ≥1-skin-voxel-thick surface strictly OUTSIDE
the human limb's art (read `gen_human.py`'s `ART_LIMBS` boxes as the fitBox
truth — import or assert-equal, don't restate). Pieces:

| piece | slot/kind | covers | material |
|---|---|---|---|
| hood  | ArmorHead  | head (cone, face void) | robe_cloth |
| robe  | ArmorChest | torso, armU.L/R, armL.L/R, hips (skirt) | robe_cloth |
| sash  | ArmorBelt  | hips (band) | robe_trim |
| boots | ArmorBoots | foot.L, foot.R | leather |

Verify visually with `--shot-inventory` (extend its script to equip the set
before the gear-frame capture) and `--shot-mob`. Hands/legs/shoulders/trinket
slots ship empty — content later, schema already accepts kinds.

### P3 — sheath + draw key (1 day)

§2.5. Extend `--gate player-kit` or `armor-wear` with: sheathed sword +
draw → `HeldItem()=="sword"` and tool==melee; stow → unequipped, tool
restored; draw with empty sheath is a no-op with a kitMessage.

### P4 — occlusion probe + reactivity (2 days)

§2.3. **Gate `armor-react`**: (a) ignite the robe over the torso → cloth
voxels drop before ANY `skin` voxel converts (the existing mob-burn gate
asserts this ordering on one lattice; this asserts it across the shell/body
boundary); (b) acid bath with a steel test plate worn (author a tiny
`assets/items/test_plate.json` fixture or reuse boots vs a control limb):
covered limb loses 0 voxels while an uncovered control loses >0; (c) burn the
shell through, keep burning → NOW skin converts (exposure works); (d) budget:
an idle dressed mob costs zero (the scanHot early-out still holds). Reuse the
mob-burn gate's `burnTick` fixture pattern (`selftest_mob.cpp`). Do NOT
hardcode the cast (gotcha: a gate that hardcodes the asset list breaks when
assets change) — spawn `human` via `kAvatarDefName`/def lookup.

### P5 — drop + pickup (1-2 days)

§2.6. `WorldItems` registry + `E` pickup + drag-out drop latch + `ITMS`
persistence. **Gate `item-ground`**: drop sword → body exists, registry maps
it; pickup → in bag, body gone; save/load with an item on the ground →
still there, still pickupable; bag-full pickup refused, item persists.

### P6 — fit resample (1-2 days, LAST — nothing earlier depends on it)

§2.4. Pure function first: `ResampleLattice(voxels, srcDims, dstDims)` NN,
integer math. Apply in `WearItem` when any axis ratio ≠ 1; scale offsets/
anchors by the same rationals; pack as an owned brick. **Gate `armor-fit`**:
resample determinism (same inputs twice → identical bytes); no holes (every
target cell whose NN source was solid is solid); wear the hood on a rig with
a bigger head (spawn a test def scaled in the fixture, or use `critter` if
its head box differs) → shell box matches the wearer's box ± rounding, and
the shell still attaches/burns/carves.

### P7 — starting inventory + polish (half a day)

Replace the placeholder grant (`main.cpp`, comment "Placeholder acquisition")
with: sword → Sheath, hood/robe/sash/boots → bag, so the owner can test
equipping end-to-end. Keep it clearly marked as the pre-pickup-economy stub.

### P8 — exact durability persistence (1 day)

PLYR v2 (owner decision: exact holes round-trip): per equip slot, optionally
a carved-lattice blob + per-shell hp; version bump; truncated/unknown still
REFUSED (the v1 test already asserts refusal — keep it passing for v2).
Extend the player-kit round-trip: carve a worn robe, save, reset, load →
same voxel count, same holes, same hp. Ground-item lattices persist in
`ITMS` the same way.

### P9 — docs + close-out (half a day)

- DESIGN.md: update §8b (armour rows now accept kinds; wear path; occlusion;
  fit), and FIX the stale §8 paragraph (~line 2072) that still claims "the
  sword is a part of the avatar's own .vox" — it contradicts `item.h` and
  `items.json`, flagged during research.
- `assets/tuner.html` `ARCH_NODES`: add/refresh the items/equipment node,
  remove `wip` where landed (ASCII quotes only).
- `docs/PLAN_body_reactivity.md` Package D: mark landed, link here.
- Board: `done` note. Memory files if the session culture applies.

## 5. Verification budget (read CLAUDE.md's section; this is the application)

- Everything here is CPU game-layer. **The world determinism hash must not
  move.** If `--gate determinism` ever disagrees with `b717a33d` (or whatever
  baseline.json pins at P0), you accidentally wrote into the sim domain —
  stop and find it; do not rebaseline.
- Iterate with single gates (`--gate armor-wear`, ~seconds), not suites.
  Remember gates share one World and order matters — a subset run is not a
  small selftest; when a gate behaves differently under `--selftest`, run
  both arms at the same scope.
- WGSL is untouched by this plan; no shader validation runs needed.
- Full `--suite acceptance` exactly ONCE, on the final tree. Known:
  `page-roundtrip` may fail in-suite; re-confirm standalone if it is the only
  failure.
- Every run through `bash scripts/run.sh`; kill stale exes; check
  `build/last_run.json` before re-running anything.

## 6. Risks / traps (each cost someone a day before; don't rediscover them)

1. **The removal seam** (§2.7) is the highest-risk refactor: four parallel
   vectors, Jolt bodies, joints, owned bricks. Assert sizes after every
   mutation; the orphaned `GateMob` function in `selftest_mob.cpp` (compiled,
   never registered) shows the assertions worth copying (hilt-in-fist etc.).
2. **Carried state across re-append**: re-loading a survivor shell from its
   def silently HEALS it. Move lattices/brick indices, never rebuild.
3. **Micro pool pressure**: each damaged/resampled shell owns a brick.
   `MicroBodyOwn` returning -1 (pool full) must degrade gracefully (piece
   stops updating visually, never crashes) — the null-check lesson at the
   `ensureOwnedBrick` comment in mob.cpp applies.
4. **Counter-based RNG only** in anything the burn/occlusion path rolls;
   never key on a Jolt float (PLAN_body_reactivity §5 — "looks entirely
   innocent at the call site").
5. **Palette indices are file-local** — the generator emits its own palette;
   never copy voxel bytes between .vox files (memory: art palette gotcha).
6. **Don't inflate `worldSize`** with worn geometry (the `prop` measurement
   exclusion exists for exactly this; DESIGN.md §8 records the failure).
7. **A weapon must not cut its wielder** already handled via `OwnsBody`;
   shells are the same creature's bodies, so blade self-rejection covers
   them — verify a swing with full armour doesn't shred your own robe
   (add to `armor-wear` or the melee sub-checks).
8. **Item/glyph slots are file-order indices** — every new crossing (worn
   registry, WorldItems, ITMS/PLYR v2) snapshots NAMES and re-resolves, like
   everything else in §8b.
9. **The `I` screen runs with the sim live** — wear/unwear happens mid-tick
   via the latch; keep the WearItem call at the same point in the frame as
   the existing EquipItem seam (after latch consumption, before PreTick).

## 7. Out of scope (explicitly)

- Damage MITIGATION numbers (armour class, resist stats) — protection is
  geometric occlusion + material identity only.
- Mob AI that chooses to wear/draw (the API supports it; nothing calls it).
- `sheath_back` visual, `held_left`, shields — schema anticipates, no code.
- The remaining 4 slot contents (shoulders/hands/legs/trinket) — schema
  accepts kinds; content later.
- Reconciling the two diverged mains (owner).

## 10. What landed, and what the plan got wrong (2026-08-29)

Every package landed. What follows is the difference between the plan above and
the code, because the differences are where the information is.

### Gates

| Gate | Result |
|---|---|
| `armor-wear` | PASS, 81 checks. Slot table, kind round-trip, draw/stow, the appended tail, damage surviving a neighbour's removal, `worldSize` unchanged, the shipped set fitting the stock rig |
| `armor-react` | PASS. Occlusion probe, containment, dressed-idle costs zero, cloth-over-flesh, cover consumed, steel-stops-acid |
| `armor-fit` | PASS, 13 checks. Resample identity / determinism / no-holes / own brick |
| `item-ground` | PASS, 15 checks. Drop, registry, release hook, `ITMS` round trip + refusals |
| `player-kit` | PASS, 48 checks (was 40) — `PLYR` v2 worn damage |

World determinism hash `97c458a3`, unmoved. Everything here is CPU game-layer,
as §5 predicted.

### The four things the plan had wrong

**1. The baseline hash is `97c458a3`, not `b717a33d`.** §4's P0 said to verify
against `tests/baseline.json` rather than trust the sentence, which is what
saved it. CLAUDE.md's copy is stale too.

**2. `X` was already bound** (detonate). The draw key is **`Q`**. §2.5 said to
verify it was unbound first; it was not.

**3. The removal seam is an ERASE, not a tear-down-and-re-append.** §2.7
proposed rebuilding the whole appended tail and carrying each survivor's state
across by hand, with trap #2 warning that re-loading from the def would heal
damage. Erasing the range and fixing up the three kinds of index that referred
past it is smaller, and it makes trap #2 *unrepresentable* rather than merely
tested for: the survivor's `MobLimb` is moved wholesale and never let go of. An
appended slot is never a parent, so nothing can be orphaned.

**4. Damage is keyed by ITEM NAME, not by equip slot.** §4's P1 said to keep the
carved lattice "in the `Equipment` slot's stack entry". That mends a robe
dragged to the pack and back — the slot emptied, and the blob went with it.
Keyed by name, "your robe" is the unit; two robes would share one set of holes,
which is acceptable while nothing produces two of anything.

### The one thing nobody predicted

§2.3 described the occlusion probe as a point test: "transform worldPos into the
shell's lattice and test occupancy". That reads correct and **leaks
completely**, for two reasons the design could not have known without measuring:

* **A limb is a rounded tube inside a garment cut to its box.** On any diagonal
  there are several empty cells between the flesh and the cloth, so the point
  one lattice step outside a surface voxel lands in the gap. The probe has to
  march a SEGMENT. Measured before the fix: a fully enclosed arm caught fire
  three ticks *before* the bare one beside it.
* **It only works at the scale the grid can resolve.** An arm is 0.76 world
  voxels across; the shell adds 0.24 on each side; a grid cell is 1.0. For a
  limb thinner than a cell there is no "outside the coat" and no probe can put
  one between the fire and the flesh — 107 of 130 world threats had no shell in
  the outward direction *at any reach*, because the threat was already inside
  the garment's envelope. Armour occludes on the torso and larger. On a forearm
  it does not, and that is a property of a grid-coupled body rather than a bug
  to fix.

Finding this cost four runs and would have cost far more without CLAUDE.md's
rule 6. The sequence is worth recording because each step *replaced a
hypothesis with a measurement* rather than eliminating one:

1. "The covered limb burned" — three possible causes, no way to tell them apart.
2. Added `MobSystem::WornStats` (blocked / passed / substituted). Read
   `744 blocked / 15640 passed`, which looked catastrophic and was **a
   measurement artefact** — most face samples seed nothing at all. Resolving the
   lattice cell *before* asking about armour fixed both the counter and the
   cost: `12 / 39`.
3. Added a probe assertion. It read 32/32 and was **circular** — probing the
   shell's own voxels uses the same transform the shell was placed with, so a
   shell in entirely the wrong place still passes. Re-pointed it at the covered
   limb's voxels, looking outward.
4. Printed both world AABBs. `arm 173.62..174.38 / shell 173.38..174.62` — the
   geometry was exact, and the numbers themselves were the answer.

Both instruments are permanent: `WornStats` (including "would a 4× ray have
found it", which separates *reach too short* from *nothing in that direction*)
and the containment + inside-ness pair at the top of `armor-react`.

### Smaller corrections made on the way

* **Item art colours were never remapped into the shared palette.** `LoadItems`
  did not call `MicroBodyMergeArt`, so an item's file-local `.col` slots indexed
  whatever the *mobs* had put at those numbers. A gold sash rendered the same
  near-black as the robe over it, which reads as a lighting problem. `EquipItem`
  was also dropping `v.color` outright — invisible only because the one item in
  the game is unpainted.
* **`MobLimb` needed its own lattice scale.** Every scale-taking operation read
  `def_->skinScale`, which is wrong for any limb whose art is not the
  creature's. It happened to agree for every held item on every rig in the repo,
  which is exactly the kind of agreement that stops being true silently.
* **`worldSize` was already safe** — it is measured from `def.limbs`, so an
  appended slot cannot inflate it (§6 trap 6). Asserted anyway.
* **`BodySlotFor` was already safe** — `"worn"` is not one of the figure tags,
  so shells stay out of the HUD body diagram (§4 P1 asked to verify; verified).

### Out of scope, still

Everything §7 listed, plus two the work itself deferred:

* **A crumpled-garment ground model.** A dropped worn piece uses its largest
  panel; merging the shells at their authored offsets would hang a
  person-shaped shell in the air. A `"ground"` cover row is the content answer.
* **Item instance identity.** See correction 4 above.
