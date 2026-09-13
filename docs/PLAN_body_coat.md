# PLAN: body coats — a stain on a body is a substance, not a look

Status: scaffolding landed 2026-09-13 (branch `body-coat`). Substances beyond
blood and water, and every EFFECT, are future work; this document records what
the scaffolding is for and where an effect plugs in, so the first one is a data
edit plus one read, not a redesign.

## 1. What the scaffolding is

**On the ground** a stain is residue: seven palette LOOKS (`stain.type`, bits
28..30 of the voxel word), monotone rules so chunks sleep, no drying. The
substance itself is the liquid cell; the stain is what it left. Unchanged.

**On a body** there is no cell. The stain IS the substance, so it has to name a
material. `PrefabVoxel::stain` / `DebrisVoxel::stain` are `uint16_t`: 12-bit
material id in the low bits, 4-bit amount above (`voxload.h` `BodyStain*`).
Seven looks are not seven substances: a future nullifier and blood may share a
ground colour and must still be told apart on a hand. The RENDER lattice in the
micro brick stays one byte (`slot << 4 | amt`); `microbody.cpp` converts through
`MicroBodySet::stainSlotOfMat`, filled at every materials load, so
`microbody.wgsl` never changed and a material with no `stain` block simply
renders nothing.

The body stain rides the limb's own voxel list, so it is SAVED with the MOBS and
DBRS sections (raw `PodVec` of the lattices; `kSaveVersion` bumped once for the
width change). It is never hashed: the body is not in the grid.

**`coat` block on a material** (`materials.json`, parsed by `ParseCoat`, requires
a `stain` block so the substance has a look):

```json
"coat": { "decay": 20, "shed": 400, "effects": [] }
```

- `decay` seconds per amount level lost while on a body; 0 = only washing
  removes it. Blood 20 (a splash is gone in ~5 min), water 4 (wet dries).
- `shed` per-mille chance per footfall that a coated foot puts one droplet on
  the ground. Blood 400.
- `effects` raw string tags. Parsed, stored on `MaterialDef::coatEffects`,
  queryable, consumed by nothing yet.

**The ledger** (`Mob::RecountCoat`, every `coat.recountTicks` and only when a
stain byte changed): per limb, amount-weighted sums by material and the live
voxel count; body totals over the base rig (worn shells and held items
excluded). Fraction = `sumAmt / (15 * voxels)`, so one splash cannot flip a
threshold and a soaked limb reads 1.0. Queries on `MobSystem`, resolving the
avatar by id: `LimbCoatOf`, `BodyCoat`, `CoatTagFraction(mob, tag, limbTag)`.

**Decay** runs in `Mob::StainTick` after the contact pass (which early-outs
when nothing is touching the limb; decay must not), budgeted like the sweep,
rolled per voxel on `Hash3(limbKey ^ cell, tick, salt)` so a coat thins
unevenly rather than vanishing in one tick.

**Shedding** (`Mob::ShedCoat` from both plant sites): a foot whose ledger names
a `shed` material rolls once per footfall and emits ONE micro droplet per
distinct ground cell of the sole (the particle kernel's claim takes one deposit
per cell per tick), born inside the solid cell under the foot with life 1, so
`sim_particle.wgsl`'s existing deterministic stain path puts the material's own
`stain` on the floor. Zero shader changes; the foot loses what it shed. NPCs
gained `Mob::Footfall` for this (the avatar's audio drain is unchanged).

**UI**: `UIState::BodyPartUI::stainFrac/stainMat/stainColor`; the HUD's
"stained NN%" line and figure tint; the character screen's chip and callout.

## 2. Where the first effect plugs in (the nullifier example)

A magic nullifier is a liquid with `"stain": {"type": ...}` (any look; it may
share one) and `"coat": {"decay": 0, "shed": 0, "effects": ["nullify"]}`.
Everything below is a READ of the ledger; none of it exists yet.

| Effect | Read | Where |
|---|---|---|
| cannot cast with coated hands | `CoatTagFraction(player, "nullify", "hand")` over a hysteresis pair (on 0.25 / off 0.15) | the cast latch in `main.cpp` (refuse the click, keep the sentence) or `SpellSystem::Cast`'s early-out for NPCs too |
| worn gear effects disabled | the worn slot's limb fraction | there are no gear effects yet (`DESIGN.md` §8c: "no enchantment system") — the read lands when the first one does |
| incoming spells weakened by coverage | `BodyCoat(target).Frac()` | an attenuation beside `SpellSystem::FilterStreams` (binary today) keyed on the target body, not the position |
| spells cast ON you refused | same | the `self` cast path in the inspector |

Hysteresis belongs to the consumer, not the ledger: two thresholds per effect,
state kept where the effect lives.

## 3. Deliberately not done

- Ground stains do not decay: `sim_step.wgsl`'s stain rule is monotone so a
  stained chunk can sleep; a drying rule would keep every stained chunk awake.
- Corpses take no contact stain (no debris contact pass), and a dragged body
  smears nothing (no ragdoll ground-contact event).
- A `washable` key: water's `washes` is the authored surface; a material that
  refuses washing would need a second rule in the rinse branch.
- A hysteresis helper with no consumer.
