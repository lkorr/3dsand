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

**STATUS (2026-10-12): DONE on branch worktree-agent-a817365c4a9b142d0**
(DESIGN.md "The bow and its arrows"). Bow + arrow items (kind `ammo`, art
from `scripts/gen_bow_items.py`), flat `damage` 35 via `Penetrator::hp`,
starting kit (bow on the hotbar, 30 arrows in the pack), draw/release through
TickInput's held mask in PhaseG, `bow_draw` clip, pixel HUD bow, CPU integer
flights in `TickAuthorityCtx::arrows` (`game/arrows.*`) sharing the spell
VM's grid probe (`game/flightprobe.h`), embedded arrows as follower slots at
an angle (`Mob::EmbedArrow`, `MobLimb::wornRotated`), terrain shots as
kinematic ground items, G pull-out (`PullArrowToKit`), peers' bodies routed
as `MeleeHitKind::Pierce`, gate `arrow`, `--shot-mob human:>N`. Open: embedded
arrows are not saved and do not cross a handoff; the shooter does not see an
arrow stuck in a peer's creature.

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

**Status (2026-10-12): built, on its worktree branch** (DESIGN.md "The matter
is the projectile"). How it came out:

- **Matter -> penetrator.** Mass = materials.json density x
  `budgets.matterProjectile.lumpMl` (250 mL) x matter scale, summed over the
  matter items; the heaviest is the core. Stone 0.65 kg, steel 1.95 kg, gold
  4.8 kg, water 0.25 kg, fire 0.75 g. Speed = the record's launch speed
  (projectile 14.4 m/s, bolt 28.8, lob 7.2). Point = the delivery's new
  `penetrator` block (projectile tip 1.5 fine cells / sharpness 0.4, bolt
  1.0 / 0.7, lob 2.5 / 0.15, orb 2.0 / 0.2); hardness = the matter's.
- **Class decides the strike:** solid PIERCES (PierceAlongRay; through =
  flies on at the residual speed), powder SCOURS (sharpness x0.4, tip x2,
  capped at 2 skin bins, always stops), liquid SPLASHES a coat (PourOnBody;
  acid eats, lava sears/lights, water wets -- the coats' own rules), gas
  TOUCHES (resolve at the hit, as before: the NPC firebolt is unchanged).
- **Deposit:** effects resolve at the stop point; the matter the body did not
  take is dropped there with no velocity. Embedded inert solids (stone,
  steel) are dropped, not lodged -- a lodged lump wants package C's embedded
  slot (owner decision).
- **Price:** kinetic term = 1/2 m v^2 x pierce bonus x `rates.kinetic` (100
  mana/kJ), in carry after the box's multiplier. Stone: lob 17 -> 19,
  projectile 25 -> 32, bolt 33 -> 60. NPC kits carry no solid matter
  (fire weighs 0.75 g): firebolt / hellfire / lava_floor / lift prices are
  unchanged.
- **`pierce`** adds x1.0 of the matter's energy per word against bodies
  (priced), and keeps its one-wall terrain role.
- **Gate `spell-pierce`** (torso, front, real casts through the tick): stone
  projectile 67.5 J embeds 3.8 cm (18 cells, 0.6 hp + bleed); stone bolt 270 J
  goes through 14.6 cm (41 cells incl. 6 bone) and flies on at 24.7 m/s; sand
  bolt scours 2.5 cm; water coats 596 skin cells, carves nothing; acid coats
  and eats 75 cells / 20 hp in 2 s; lava coats and burns 129 cells / 70 hp in
  2 s; the stone bolt replays identically. World hash unmoved (ef7d9a06).
- **Owner decisions open:** (1) whether an embedded lump should LODGE (needs
  package C's embedded slot) instead of dropping out of the wound; (2) the
  calibration -- lumpMl 250, the delivery points, rates.kinetic 100/kJ -- is a
  first cut: the stone projectile's wound is 0.6 hp, i.e. the damage is the
  bleed and what the channel hits, by design (decision 1's no-kill-rule);
  (3) `speed@4` now costs 149 for a stone projectile (was 41), so fast heavy
  matter is out of reach of a 100-mana player.

## NPC archers (package E, after C)

`behaviors.json` `ranged` block shaped like `cast`: preferred range, draw,
lead aim (reuse the caster aim/lead code), ammo, back off when crowded. An
`archer` profile; NPCs armed with a bow through the existing weapon paths
(`props.weapon`, spawn panel). Gate `archer`.
