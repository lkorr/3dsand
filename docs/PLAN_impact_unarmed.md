# PLAN: damage KINDS, natural weapons, unarmed attacks, the zombie's lunge + bite

**Status (2026-10-02 audit): LANDED 2026-09-15..17** — `src/game/impact.h`
(`c267617`), natural weapons / bite / lunge / player unarmed, `--shot-strike`
(`src/main.cpp`), gates `impact-blunt`, `impact-armor`, `impact-fist`,
`bite-rot`, `unarmed-attack`, `lunge`, `bite-target`, `player-unarmed`,
`npc-styles` in `src/test/selftest.cpp`. Later reworked by rule unification
W2-H (one damage event, `c881f05`).

Original status: APPROVED 2026-09-15. Orchestrated; packages land via worktree merges
off `a8a262a` on `lighting-cave-daylight`. Owner of this document: the
orchestrator session. Workers: read the whole thing before touching a file.

## 0. What the owner asked for, in their words, and what it decomposes into

> zombies need to attack even if disarmed: a LUNGE + BITE (jump forward, then
> a procedural attack from the head) and PUNCHES with the hands. Humans punch
> too, so we need a general punching animation. Punching never dismembers, it
> bloodies a spot if you hit it repeatedly; with iron gauntlets it also
> deletes voxels and replaces them with gore, caving a face in SLOWLY. That is
> a BLUNT kind of damage. Fists and maces bruise, indent/destroy plate
> (revealing flesh) and bloody the spot without dismemberment or copious
> bleeding. Plate stops swords almost entirely; maces go through. A sword does
> a tiny bit of blunt. Disarmed or crawling, a zombie still bites. Animations
> must not look jank across the combinatorial pose space (dismemberment
> combos, crawl). A zombie bite that touches FLESH (armour defends) leaves a
> greenish, glowing, rotten wound. Bites go for arms and torso too, not only
> the head. Build it as robust, data-driven, iterable systems that talk to
> each other; integrate with the tuner so a human can tune by eye.

Decomposed, that is SIX systems, each small, each data, all meeting in one
function:

| # | System | Where it lives | One-line contract |
|---|---|---|---|
| S1 | **Impact model** (`StrikeProfile`) | `src/game/impact.h`, `melee.cpp` | A strike is a CUT part, a BLUNT part and a BITE part, each a number. One sweep resolves all three against what it hit. |
| S2 | **Natural weapons** | `MobDef::natural`, `human.json` | A rig part can BE a weapon (fist, jaws): an authored edge segment + a `StrikeProfile`, exactly as an item has. A worn item over that part may OVERRIDE the profile (gauntlets). |
| S3 | **Strike effectors** | `Mob::SetStrikeEffector`, `ApplyWeaponArm`, new `ApplyAimPart` | The stroke driver drives WHATEVER PART the style names, not "the held item's hand". Two modes: chain (arm IK, as today) and aim (rotate a part and a share of the spine toward the tip — the head). |
| S4 | **Style availability + target choice** | `strokes.*`, `attack_styles.json` | A style names its `weapon`; `PickAttackStyle` filters to what the creature still HAS. A style may name a `target` weight table over limb tags, so a bite chooses a part. |
| S5 | **Lunge** | `Mob::Launch`, `UpdateFall`, style `lunge` block | An NPC can be given a ballistic velocity. A style may fire one at its start; the body lands where the cut needs it. Scaled by the loco state (a crawler pounces low). |
| S6 | **Wound source material** | `Mob::StainWoundAs`, `bite.infect` | The material a wound REWRITES exposed tissue to is a parameter of the wound's SOURCE, not of the victim: cut → victim's `woundMat` (blood), blunt → `skin_bruised`, zombie bite → `rotflesh` + `ichor` stain. Armour in the way = no rewrite. |

Plus the surfaces: items (`mace`, `iron_gauntlets`), materials (`rotflesh`,
`ichor`, `skin_bruised`), clips (`punch_r`, `punch_l`, `bite`, `lunge`),
styles, a `zombie` behaviour profile, tuner rows, a `--shot-strike` harness,
and gates.

## 1. Rules that bind every package

- **CLAUDE.md applies in full.** Determinism: every draw is `rng::Hash3`
  keyed on (mob id, tick, index). Nothing here touches the CA; the world hash
  moves ONLY through `materials.json` gaining three materials (rule 1: that is
  a notification, rebaseline once at the end — the orchestrator does it).
- **Behaviour is data (design rule 4).** No `enum StyleKind`, no
  `if (name == "zombie")`, no list of natural-weapon names in C++. The engine
  learns what a fist is from `human.json`, what a mace does from `items.json`,
  and what a zombie's bite carries from `zombie.json`.
- **Rule 3: damage reaches the world only through the paths it does today**
  (`CarveLimb`/`CarveLimbRadial`/`StainWound`/`Damage`). No new voxel paths.
- **Verification is a budget.** Each worker builds ONCE, iterates with
  `--gate <name>`, and finishes with ONE `--verify` launch. Do not run the
  suite. Do not rebaseline (the orchestrator does, once, at the end).
- **Mirror what the tuner previews.** `assets/editor/melee.js` and
  `assets/editor/anim.js` are line-cited ports of `melee.cpp`/`strokes.cpp`
  and `anim.cpp`. A change to the RUNNER (`StepStrokeProgram`,
  `PickAttackStyle`, the effector math) must be ported in the same package,
  and `node scripts/test_melee.mjs` must stay green. Package C owns the port
  of the effector generalisation; A and B leave `melee.js` correct for what
  they change in `strokes.cpp` (the availability filter, the `weapon`/
  `lunge`/`target` fields of the style schema).
- **Comment density**: every new struct and every non-obvious invariant gets
  a paragraph in the style of `strokes.h`. Future agents will iterate on
  these; write for them.
- **Thresholds in `tests/baseline.json`**, never in C++ (`BaselineNumber`).
- **Do not edit** `src/game/dye.h`, `persist.*`, `ui/overlay.*`,
  `inventory_ui.*` beyond the one combo entry noted in §7 — a concurrent
  session (peasant clothes + dye) owns them.

## 2. S1 — the impact model (`src/game/impact.h`, shared contract)

The header is written by the orchestrator and is the vocabulary every package
uses. Read it. Summary:

```cpp
struct StrikeProfile {
  float cut = 0;         // hp at full swing speed that arrives as a KERF
  float blunt = 0;       // hp at full swing speed that arrives as TRAUMA
  float bluntCarve = 0;  // 0..1: how much of gore.bluntCarveRadius a full-power
                         // blunt hit DENTS out of flesh (fist 0, gauntlet ~0.35,
                         // mace ~0.6). The dent is a radial crater, never a kerf.
  float armorBreak = 0;  // 0..1: how much of gear.bluntDentRadius a full-power
                         // hit breaks out of a WORN SHELL (fist 0, mace ~0.8)
  float bite = 0;        // hp at full speed that arrives as a TEAR (a rot-blob
                         // carve, bleeds like a cut, never severs by blade rules)
  uint16_t infectMat = 0;   // material the bite REWRITES exposed flesh to (0 = none)
  uint16_t infectStain = 0; // liquid whose stain the bite smears (0 = none)
  bool Any() const;
};
```

`ItemDef` gains `StrikeProfile strike` (parsed from `items.json`: `damage`
stays and IS `strike.cut` — do not rename the key; add `blunt`, `bluntCarve`,
`armorBreak`). A worn item may carry a `strike` block too (a gauntlet): that
is the override S2 applies when the fist under it swings.

`EdgeSweep` gains `StrikeProfile strike` (replacing the bare `damage` float —
keep a `damage` accessor for the two existing callers until they are moved;
`heft` stays). `MeleeSweepDamage` resolves, per body hit, in this order:

1. Parry (held slot) — unchanged.
2. Classify: FLESH (`< AppendedBase()`), SHELL (`IsWornSlot`), else debris.
3. **CUT part** (`strike.cut > 0`): exactly today's path (`Damage` +
   `CutLimb` with the hardness gate). Unchanged numbers.
4. **BLUNT part** (`strike.blunt > 0`): `MobSystem::BluntHit(body, BluntHit)`.
   - On a SHELL: charge shell hp `blunt·power·gear.bluntShellHp`; break shell
     voxels with `CarveLimbRadial` at radius
     `gear.bluntDentRadius · armorBreak · power · clamp(gear.bluntHardnessRef / hardness, gear.bluntHardnessMin, 1)`;
     then TRANSMIT `blunt·power·gear.bluntThrough` to the host limb
     (`wornHost`) as hp with a bruise at the same spot (no dent, no bleed).
     This is "maces beat plate".
   - On FLESH: `limb.hp -= blunt·power` through `Damage` with a `blunt`
     flag: NO `bleedBudget` top-up beyond `gore.bluntBleedScale` (default
     0.1), the flinch and the hurt cry still fire. Bruise:
     `StainWoundAs(limb, at, gore.bruiseRadius·(0.5+0.5·power), seed, gore.bruiseMat)`.
     Dent (only if `bluntCarve > 0`): `CarveLimbRadial` at radius
     `gore.bluntCarveRadius · bluntCarve · power` inside a `BluntCarveScope`
     that makes `CarveLimb` (a) skip the collapse sever and the blade rules —
     **a blunt hit never takes a limb off** — and (b) still stain the crater
     with the victim's `woundMat` (the "replace with gore" the owner asked
     for). hp reaching 0 on a VITAL limb still kills (a caved-in skull).
5. **BITE part** (`strike.bite > 0`): `MobSystem::BiteHit(body, BiteHit)`.
   - On a SHELL: the bite's blunt equivalent only (`bite·gear.biteOnShell`
     as blunt, no break). No infection: **armour defends**.
   - On FLESH: `Damage(bite·power)` with normal bleeding, then a TEAR carve:
     the rot-blob predicate from `RotAtSpawn` (extract it into a helper
     `Mob::CarveBlob(limb, centreLocal, radius, blob, seed, scope)`) at
     radius `gore.biteRadius·(0.4+0.6·power)`, inside a `BiteScope` (no blade
     sever rules; collapse sever allowed — enough bites DO take a hand off,
     that is not dismemberment by punch). If `infectMat != 0` and the carve
     exposed flesh-class cells, `StainWoundAs(..., infectMat)` over the
     exposed cells with rim `gore.craterStainRim·gore.biteStainScale`, and if
     `infectStain != 0` smear that liquid's stain over the hole.
6. Debris: unchanged (`MeltBodyAt` with `cut + blunt`).

`Mob::StainWoundAs` is `StainWound` with the rewrite material as a parameter;
`StainWound` becomes the one-line wrapper passing `def_->woundMat`. It records
into `woundWas` as today, so bruises and rot HEAL back through
`ReviveWoundVoxel` when the victim's `WoundsHeal()` says so — a zombie's rot
in a living human heals slowly (`gore.infectHealSlow`, default 6.0 vs the
`woundHealSlow` 2.0); in an undead never.

Materials (append to `materials.json`, AFTER `blood` so stain slots 1/2 keep
their numbers): `rotflesh` (solid, greenish, `emission` ~70, tags organic +
dissolvable, rubble `ichor`), `ichor` (LIQUID, dark green, `emission` ~40,
`stain: {type:"rot", ...washes:false}` → stain slot 3), `skin_bruised`
(solid, `tints` = purples/yellows, hardness as skin, rubble blood). Tuner
Materials page already edits `emission`; the stain block is hand-authored.

Tuning (CPU-only, `Tuning::Gore` + `Tuning::Gear`, three edits each:
`tuning.h`, `tuning.cpp`, `tuner_schema.js`): `gore.bruiseRadius` (0.9),
`gore.bruiseMat` ("skin_bruised", resolved by name at load like `bleed.material`),
`gore.bluntBleedScale` (0.1), `gore.bluntCarveRadius` (0.7), `gore.biteRadius`
(1.1), `gore.biteBlob` (2.5), `gore.biteStainScale` (1.5), `gore.infectHealSlow`
(6.0), `gear.bluntDentRadius` (1.2), `gear.bluntHardnessRef` (60),
`gear.bluntHardnessMin` (0.15), `gear.bluntThrough` (0.55), `gear.bluntShellHp`
(0.6), `gear.biteOnShell` (0.3). Add every one to the `combat-tuning` gate's
round-trip table.

Items: `items.json` rows gain `blunt`/`bluntCarve`/`armorBreak`. Sword
`blunt: 2, armorBreak: 0.05`; cleaver `blunt: 4`; dagger/shortsword `blunt: 1`.
New `mace` (`scripts/gen_mace_item.py` after `gen_sword_item.py`: a haft and a
flanged head, `edge` = the head's span, `flat` unauthored so `MeleeEdgeAlign`
returns 1 — a mace has no edge to align): `damage: 2, blunt: 16, bluntCarve:
0.6, armorBreak: 0.8, reach: 8`. New `iron_gauntlets` (extend
`gen_stock_armor.py`, kind `hands`, cover `hand.L`+`hand.R`) with
`"strike": {"blunt": 9, "bluntCarve": 0.35, "armorBreak": 0.3}` in its
sidecar.

## 3. S2 — natural weapons

`human.json` gains:

```json
"natural": [
  { "name": "fist.R", "part": "hand.R", "edge": { "from": [...], "to": [...], "halfWidth": 2.5 },
    "strike": { "blunt": 4.0 } },
  { "name": "fist.L", "part": "hand.L", "edge": {...}, "strike": { "blunt": 4.0 } },
  { "name": "jaws",   "part": "head",   "edge": { "from": [...], "to": [...], "halfWidth": 2.0 },
    "strike": { "blunt": 1.5, "bite": 7.0 } }
]
```

`edge` is in the PART's own art frame (same convention as an item's `edge`
but `from`/`to` are points, not axis offsets — the part has no hilt). Scaled
by `artVoxelsPerMetre` at load exactly as `MobLimbDef::edgeFrom/To` are.
`MobDef::natural` is a `std::vector<MobNaturalWeaponDef>` (name, partIndex,
edge, StrikeProfile). Merge-patch: an array REPLACES, so `zombie.json` that
wants different jaws restates the array; the zombie's bite INFECTION is not
here (see §6, it is on the creature: `"bite": {"infect": "rotflesh",
"stain": "ichor"}`), so the zombie inherits the human's `natural` untouched.

**Gauntlet override**: when a fist swings, `Mob::StrikeProfileFor(natural)`
looks for a worn slot whose `ItemCover.part` is that fist's part and whose
`ItemDef::strike.Any()`; if found, the worn profile REPLACES the fist's
(`cut/blunt/bluntCarve/armorBreak`), and the swept edge stays the fist's own.
`infectMat`/`infectStain` come from the creature (§6) and are ORed in for
`jaws` only.

The `mob` gate asserts every `natural` entry names a live part and has a
non-degenerate edge.

## 4. S3 — strike effectors

Today `ApplyWeaponArm` bails on `heldPartIndex_ < 0`, finds the arm chain by
the held part's parent, and `WeaponEdge`/`WeaponStrokePose` read the item.
Generalise:

- `Mob::SetStrikeEffector(int partIndex, StrikeEffectorMode mode)` with
  `enum class StrikeEffectorMode { Held, Chain, Aim }`; `ClearStrikeEffector()`.
  `EquipItem` sets `Held` on the held part (as now via `heldPartIndex_`);
  `BeginStroke` sets it from the style's `weapon` (fist → `Chain` on the hand
  part, jaws → `Aim` on the head part) and clears it at the stroke's end.
- `ApplyWeaponArm`: in `Held`/`Chain` mode the chain is the one whose
  `effector == part` or `effector == parent(part)`; in `Aim` mode call
  `ApplyAimPart(part, weapon_.hand direction, weight, def.aimSpineShare)`.
- `ApplyAimPart`: MOVE `PlayerAvatar`'s head-look application
  (`avatar.cpp:1404-1459`, spine share across `tag=="spine"` excluding root,
  remainder on the part) DOWN to `Mob` as the generic "rotate a part and a
  share of the spine so the part's forward points along a world direction".
  The avatar keeps calling it for its look. An NPC with a target and no
  stroke may ALSO use it to look at the target (`ai` `face` intent) — cheap,
  and it sells the bite: the head is already on the victim before the lunge.
- `WeaponEdge(base, tip)`: when the effector is a natural weapon, return its
  edge in the part's live Jolt frame. `WeaponStrokePose`: hand = effector
  part's anchor (wrist for a fist, the head's own anchor = neck for jaws);
  tip = the edge's far end; reach = chain length (Chain) or `neck+head`
  length (Aim); `HeadKeepOut` is skipped when the effector IS the head.
- The BLADE FRAME for a fist: `bladeDir` = along the forearm, `bladeFlat`
  unauthored (zero → `MeleeEdgeAlign` = 1). For jaws: `bladeDir` = the head's
  forward. The driver's wrist steering (`wristMaxAngle`) is set to 0 for a
  natural weapon (there is no blade to lay along a line).
- Pose robustness (the owner's "must not look jank" clause): the effector
  drive runs AFTER the loco clip and the gait, exactly where the arm drive
  runs today, so a crawling body's punch is solved from wherever the crawl
  clip left the shoulder, and the ground-aligned prone frame is the basis
  the driver sees (`MobBasis` must use the body's live up, not world up, when
  `groundAlign > 0` — check `SettleClipOwnedBody` for the fitted plane).
  A missing chain part (elbow severed) → `StyleUsable` refuses the style
  before it starts (§5), so the driver never sees a broken chain.

`assets/editor/rig.js`'s `applyWeaponArm` and `assets/editor/anim.js` must
learn the same effector modes (Package C), or the Attacks lane cannot preview
a punch or a bite.

## 5. S4 — style availability, `weapon`, `target`; S5 — lunge

`AttackStyle` gains:

```jsonc
{
  "name": "bite_lunge", "label": "Lunge and bite",
  "weapon": "jaws",                 // "held" (default) | a natural weapon name
  "fallback": false,                // true = only when no held-weapon style is usable
  "reach": 22,                      // world voxels, overrides the profile's attack.reach for THIS style (0 = profile)
  "lunge": { "ticks": 9, "speed": 2.4, "rise": 1.1, "at": "windup" },   // m/s, m/s; fires at the start of that phase
  "target": { "head": 0.35, "arm": 0.30, "hand": 0.05, "spine": 0.20, "leg": 0.10 },  // weights over LIMB TAGS
  "windup": {...}, "cut": {...}, "recover": {...}, "jitter": {...}, "clip": "bite"
}
```

- `StyleUsable(const Mob&, const AttackStyle&)`: `held` → `HeldSlot() >= 0`;
  natural → the named weapon exists on the def AND its part is live (not
  severed) AND, for `Chain` mode, every part of its chain is live.
  `PickAttackStyle` takes the mob, filters the profile's list to usable
  styles, drops `fallback` styles when any non-fallback usable style
  remains, then draws counter-based as today. Empty → -1 → the request is
  dropped LOUDLY once per (mob, reason) in the dev readout, not silently.
- `target`: at `BeginStroke`, choose the victim's limb by tag weights
  (counter-based on (mobId, tick)); only LIVE base limbs; absent table →
  today's chest. The aim point is that limb's live centre; recorded on
  `NpcStroke::targetLimb` so the gates and `--shot-strike` can report it.
- `reach`: `MobSystem` computes each mob's `attackReach = max(profile.reach,
  max over usable styles of style.reach)` once per tick and hands it to
  `ai::Think` as an INPUT (the AI layer stays style-ignorant, `ai_behavior.h`
  says why). `BeginStroke` then refuses a style whose own reach the target
  is outside, before drawing.
- `lunge`: `Mob::Launch(Vec3 vel)` sets `airborne_`, `fallVel_ = vel.y`, and
  a new `airVel_` (xz) that `UpdateFall` integrates with the same `fits()`
  wall test `DriveLocomotion` uses, cleared on landing; `UpdateFall` stops
  zeroing `fallVel_` on the airborne edge when a launch set it (a
  `launched_` latch). Velocity: horizontal toward the target, magnitude
  `min(speed, (dist - reachAtLanding) / airTime)` so the body arrives at
  striking distance rather than through the victim; scaled by the loco
  state's `lungeScale` (new `AnimStateRule` field, default = `speedScale`)
  so a crawler pounces low and short. The stroke's cut phase should coincide
  with landing: `windup.ticks` ≈ flight ticks is the AUTHOR's job, and the
  Attacks lane shows both (Package C).
- The `attack` intent's `commitTicks` facing hold covers the flight.

Styles to author in `attack_styles.json` (Package B; numbers are starting
points, the tuner is where they get finished): `punch_r`, `punch_l`
(fist, thrust-shaped: short windup, `reach` cut), `hook_r` (fist, azimuth
sweep), `bite` (jaws, no lunge, for in-range and for crawlers), `bite_lunge`
(jaws + lunge), `player_punch_r`, `player_punch_l`, `player_punch_thrust`
(+ a `playerUnarmed` sector map beside `player`). Every punch style is
`fallback: true`; the zombie's are not.

`behaviors.json`: new `zombie` profile (styles `bite_lunge`, `bite`,
`punch_r`, `punch_l`, `hook_r`; reach 9; slower cadence, reckless band,
hostile, faction monster); `duelist`/`duelist_blue`/`swordsman_static` get
`punch_r`, `punch_l`, `hook_r` appended (they are fallback, so armed
behaviour is unchanged — assert that in `npc-styles`). `zombie.json`:
`"behavior": "zombie"`.

Clips (`assets/anims/`, the empty library): `punch_r.json`, `punch_l.json`
(the OFF hand guards, shoulder drops, hips twist; masked off the striking
arm which the driver owns), `bite.json` (jaw drop is not a part — the clip
is the shoulders hunching and the torso dipping), `lunge.json` (a crouch
then extension over the flight, additive so it composes with crawl/limp).
Write them with a small node script (`scripts/gen_attack_clips.mjs`) in the
schema `ParseClipJson` reads, so they are regenerable; the tuner's clip
lane is where a human refines them afterwards.

## 6. S6 — the creature's bite, and the player

`MobDef` gains `bite: {infect, stain}` (materials by name, 0 when absent).
`zombie.json`: `"bite": {"infect": "rotflesh", "stain": "ichor"}`. The
human has none, so a human bite (never authored today) would just tear.

Player: `main.cpp`'s `meleeArmed` becomes `meleeArmed || meleeUnarmed` where
unarmed = melee tool, no held item, and the avatar's def has a `fist.*`
natural weapon. The discrete-strike path picks from `playerUnarmed` when
unarmed; `avatar.SetStrikeEffector(fist part, Chain)` for the stroke; the
sweep's `strike` comes from `avatar.StrikeProfileFor(fist)` so worn
gauntlets upgrade it. The HUD's weapon readout says "fists".

## 7. Tuner and harness (Package C)

- **Items tab** (`tuner.html` `ITEM_BEHAVIOUR`): rows `blunt`, `bluntCarve`,
  `armorBreak`; `ITEM_SIDECAR` gains a `strike.*` group for worn items (a
  path-aware accessor: the sidecar rows are flat today — add one).
- **Attacks lane** (`attacks.js`): `weapon` picker (held + the open rig's
  `natural` names), `fallback` toggle, `reach`, the `lunge` block, the
  `target` weight row (one slider per tag present on the rig). The preview
  drives the chosen effector (rig.js port of §4) and, for a lunge, moves the
  preview body along the arc and shows "lands at tick N vs cut starts at
  tick M" so the author can line them up.
- **Models tab**: a "Natural weapons" block in the rig panel (part, edge
  from/to, halfWidth, strike numbers), the edge drawn as a segment like the
  item edge. `zombie.json` (and any `extends` sidecar) appears in the file
  list resolved to its base `.vox`, read-only art, editable overrides.
- **In-game AI panel** (`overlay.cpp` Spawn tab): the weapon combo gains
  `fists` (= none) and `mace`; nothing else in `overlay.*` moves.
- **`--shot-strike <attacker>[+item,...] <style> [<target>[+item,...]]`**
  in `main.cpp` beside `--shot-mob`: spawns both, faces them, `ForceAttack`
  with the named style, photographs windup / cut / recover / +20 ticks from
  a three-quarter camera, and prints one line per hit: kind fractions, limb
  struck, voxels lost (flesh/shell), bruise cells, infected cells, hp
  before/after, bleed budget. This is how the owner and every future agent
  look at a strike; make it good.
- **Engine map** (`ARCH_NODES`): new node `impact` "Damage kinds" (files
  `impact.h`, `melee.cpp`, tests), `strokes` gains the availability/lunge/
  target write-up, `itemDefs` drops its stale `wip` and adds the mace ladder,
  `mobSys` `details` gets the natural-weapon + effector paragraph.
- `DESIGN.md`: a section "Damage kinds and natural weapons" after "The wound
  model: a blade cuts", and the lunge under locomotion. Orchestrator writes
  it from the landed code.

## 8. Gates (each runnable alone with `--gate`; thresholds in baseline.json)

New file `src/test/selftest_impact.cpp` (Package A) — fabricated sweeps like
`selftest_wound.cpp`'s `CutOnce`, never through the AI:
- `impact-blunt`: mace on a bare arm ×N: hp falls, voxels lost ≤ dent band,
  limb STILL ATTACHED at hp ≤ 0, bruise cells > 0, bleed budget < the same
  blows as cuts.
- `impact-armor`: iron cuirass on the torso. Sword ×N: shell voxels lost ≤ 1
  cell per hit, host hp unchanged. Mace ×N: shell voxels lost > band, host hp
  fell, no bleed, no infection.
- `impact-fist`: bare fist ×N: zero voxels lost, bruise > 0. Gauntlet ×N:
  voxels lost > 0 and per-hit fraction < `woundChipLostFraction`.
- `bite-rot`: a `bite` profile with `infectMat` on a bare arm: rotflesh cells
  > 0, rot stain > 0, bleeds. Same on an armoured torso: rotflesh = 0.

`selftest_combat.cpp` additions (Package B), through the AI and the stroke:
- `unarmed-attack`: a disarmed duelist still lands a punch (`aiHits > 0`,
  zero flesh lost); a zombie with both arms severed picks only `jaws`
  styles; headless picks only fists; both gone → no stroke and one loud
  readout line.
- `lunge`: a zombie 2× its reach away launches (airborne, xz velocity), lands
  inside reach, cuts; a zombie with both `legU` severed (crawl state) does
  the same lower and shorter and does not tunnel (`fits()` respected).
- `bite-target`: 40 forced bites, chosen limb tags ≥ 2 distinct, head not
  100 %.
- `npc-styles` extended: every usable style still sweeps its own channel with
  its own effector; armed duelists never draw a `fallback` style.

`selftest_swing.cpp` (Package B): `player-unarmed` — the `playerUnarmed` map
resolves, a punch program drives the fist effector through `arm-limits`
bounds.

## 9. Packages and order

| Pkg | Worker | Scope | Depends on |
|---|---|---|---|
| **A** | Opus 5, worktree | S1 + S6's materials/tuning + items (mace, gauntlets) + `StainWoundAs` + `CarveBlob` extraction + scopes + `selftest_impact.cpp` + `combat-tuning` rows + Items tab rows + `tuner_schema.js` rows | `impact.h` (given) |
| **B** | Opus 5, worktree | S2 + S3 + S4 + S5 + the creature `bite` block + player unarmed + styles/profiles/clips + `selftest_combat.cpp`/`selftest_swing.cpp` gates + `melee.js` port of the runner changes | `impact.h` (given); merges A at the end for the sweep's `strike` field (until then fills `EdgeSweep::damage` from `strike.cut + strike.blunt` as a stand-in) |
| **C** | Opus 5, worktree off A+B merged | §7: tuner (Items rows are A's; Attacks lane, Models natural block, rig.js/anim.js effector port), `--shot-strike`, AI panel combo, ARCH_NODES | A + B |
| **D** | orchestrator | DESIGN.md, rebaseline, acceptance, memory, look-iteration on the shots | C |

A and B run concurrently. Both touch `mob.h`/`mob.cpp` in DIFFERENT regions
(A: `Damage`/`CarveLimb`/`StainWound`/`RotAtSpawn` ~6300–8700 and ~10900–
11250; B: `BeginStroke`/`StepStroke` ~3080–3350, `UpdateFall` ~3549, the
effector code ~13690–14260, loaders ~700–1300). A owns `melee.h/.cpp` and
`item.h`; B owns `strokes.*`, `ai_behavior.*`, `anim.h/.cpp`, `avatar.*`,
`main.cpp`'s strike block. If B needs a line in A's region (or vice versa),
post a board note and keep it additive.

## 10. Definition of done, per package

A worker is done when: its gates pass under `--verify <its gates>`, the gates
it did not touch in the same files still pass (`npc-strike`, `npc-styles`,
`wound-chip`, `wound-accumulate`, `armor-wear`, `armor-react`, `undead`,
`mob`, `swing-plane`), `node scripts/test_melee.mjs` is green (B, C),
`python scripts/check_invariants.py` is green, the worktree is committed with
a message in this repo's voice (what changed and WHY, one paragraph), and
the final report names: every new JSON field with its default, every new
tuning row, every gate with its numbers, and every deliberate deviation from
this plan with the reason.
