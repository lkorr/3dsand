# PLAN: projectiles, penetration, bows, matter spells

Approved by the owner 2026-10-12. One physical model -- the **penetrator** --
serves arrows, thrown points and spell projectiles. Bodies resist by what they
are made of (skin/flesh/muscle/bone/brain/heart, wood, steel, ...) through the
same hardness pricing `KerfBite` already uses, so armoured or wooden or steel
skins are naturally "bulletproof" with no per-race rule.

## Owner decisions (do not re-litigate)

1. **No special kill rules.** Brain already kills by volume
   (`gore.brainHpPerVoxel`, materials.json `brainHp`). A HEART is added the
   same way: a material inside the ribcage whose lost cells each cost a big
   flat hp charge. Ballpark: a heart of ~100 cells, ~15 of them destroyed kills
   a full-hp torso. Size the charge from that ratio. If a thin arrow through
   the brain or heart does NOT kill, REPORT the measured cells crossed and the
   hp it cost -- do not add a rule, a multiplier or a bleed coupling to make it
   kill. The owner decides with numbers.
2. **Arrows carve on the fine (skin, 80 vox/m) lattice.** Arrows are thin;
   they pierce micro skin/flesh/muscle layers as a narrow channel, not a
   world-voxel-wide bore.
3. **Spell projectiles: the matter IS the projectile.** A flight delivery
   carrying a matter glyph flies as that matter -- density gives mass, the
   delivery gives speed, hardness decides what it can get through.
   - solid matter (stone, iron, ice, ...): penetrates by energy vs hardness;
   - powder: shallow scour, no deep channel;
   - **liquid / gas: never penetrates -- it splashes onto the skin** (acid
     projectile = acid splashed on them, through the existing body-coat path);
   - wherever it STOPS, the leftover matter is deposited there: inside a body
     it acts on the wound site through body reactions (lava burns the channel,
     ice freezes it...); in terrain it behaves as today.
   - **Price includes kinetic energy** (mass x speed^2): a stone bolt costs
     more than a stone projectile. Speed is free today; that ends.
   - `pierce` mod additionally adds energy budget against bodies.

## The penetrator (package B)

```
Penetrator { massKg, speedMps (or energy J), tipRadius (fine cells),
             hardness (projectile matter), sharpness 0..1, material id }
PierceResult { Embedded | Exited | Glanced; exit speed; stop point and
               direction (limb-local); cells crossed by material; depth }
```

`PierceChannel(...)` lives beside `KerfBite` in `src/phys/kerf.h` (shape) so
living limbs AND corpse debris use it. It walks the ray through the fine
lattice, spending energy per cell:

    cost(cell) = gore.pierceCellJ * (cell.hardness / gear.cutHardnessRef)
                 * softness_penalty(cell.hardness / projectile.hardness)
                 / max(sharpness, floor)

- Energy left after the exit face -> **Exited**, flight continues with the
  residual speed (through an arm into the torso, through one man into the next).
- Energy runs out inside -> **Embedded** at that depth.
- Cannot pay the first skin cell -> **Glanced**: falls back to `BluntHit` with
  the impulse (pebbles bruise, arrows skip off plate).
- The channel is carved through the ordinary carve path, so hp, woundHp
  weighting, brain/heart charges, bleeding, gore particles and blood
  BrushOps all happen exactly as for blades. Nothing new writes the world.
- Knobs are `gore.pierce*` rows in `tuning_params.def` (follow CLAUDE.md's
  6-step knob recipe).
- Integer/deterministic where it touches sim; it runs on the CPU in the tick,
  like melee.

Gate `pierce`: a fixture human shot along fixed rays at 3 energies: low ->
glance; mid -> embeds in muscle / stops at the skull; high -> through the
skull into the brain. Plus a wooden or steel-skinned mob resisting an energy
that goes through flesh. The gate PRINTS cells crossed per material and hp
charged, so decision 1 has its numbers.

## Heart (package A)

- `heart` material in `materials.json` (near `brain`: soft tissue, organic,
  shares flesh's far slot, mob voxel id <= 127).
- Generalise the brain charge so it is DATA, not a second hard-coded flag:
  a per-material flat hp charge (e.g. `organHpPerVoxel`, brain migrates its
  value from `gore.brainHpPerVoxel`) -- keep behaviour of brain identical.
- Human anatomy recipe: a `carve` entry in the torso placing `heart` inside
  the ribcage, behind the sternum, left of centre, in the flesh hollow.
  Re-anatomize human and every variant that inherits it
  (`node scripts/anatomize_mob.mjs`), update `scripts/test_anatomy.mjs`.
- Gate `heart`: a stab (existing melee stab) into the heart kills; the same
  stab beside it does not. Print heart cell count and cells destroyed.

## Arrows and bows (package C, after A+B)

- Items: `bow` (weaponClass "bow": drawSpeedMps, drawTimeMs) and `arrow`
  (massKg, tipRadius, head material, stack count; carried in the pack).
  Art as `.vox` + sidecar like every other item.
- Flight: CPU, integer, ballistic, swept -- the `SpellProjectile` way, NOT a
  Jolt body in flight (thin fast bodies tunnel; CCD is the 3 fps trap).
  Factor the spell's terrain probe into a shared helper rather than copying it.
  Body hits through `phys.CastRayBody` (as spells already do) -> `PierceChannel`.
- Embedded arrow = an appended slot on the struck limb (worn-shell style:
  follows the limb, no collider, NO physics joint -- see debris.h StrapBody
  note on why). Survives death and severing (follows the piece). G pulls it
  out into the pack and reopens the wound. Hits terrain -> sticks as a world
  item you can pick up.
- Player: hold to draw, release to shoot; draw fraction scales speed. Inputs
  go through `TickInput` / `TickAuthority` (replayable, peer-safe). Pixel-art
  draw indicator.
- Gate `arrow`: embed, through-and-through, terrain stick, pull-out, and a
  volley that is bit-identical across two runs.

## Matter spell projectiles (package D, after B)

Decision 3, in `spell.cpp`'s flight + resolve. Body hit with a solid matter
payload -> `PierceChannel`; liquid/gas -> splash coat; leftover matter
deposited at the stop point; kinetic term in the price; `pierce` mod body
budget. Hash moves -> rebaseline once at the end. Gate `spell-pierce`.

## NPC archers (package E, after C)

`behaviors.json` `ranged` block shaped like `cast`: preferred range, draw,
lead aim (reuse the caster aim/lead code), ammo, back off when crowded. An
`archer` profile; NPCs armed with a bow through the existing weapon paths
(`props.weapon`, spawn panel). Gate `archer`.
