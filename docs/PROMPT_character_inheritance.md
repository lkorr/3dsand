# Characters should INHERIT the human, not copy it once

Plan a refactor. Do not implement it. Orchestrate with Opus 5 subagents,
read `CLAUDE.md` and `DESIGN.md` first, and land a phased plan doc in `docs/`.

## The problem

Every character in `assets/mobs/` is a full, frozen copy of the human's rig.
The generator (`assets/editor/mobgen.js`, Characters tab) stamps out limbs,
chains, clips, loco states, natural weapons and the anatomy recipe at birth,
and from then on that character is on its own. Commit `2c4f29b` is the
evidence: four shipped commits changed how `assets/mobs/human.json` fights and
falls apart, none of them reached the generated pool, and characters shipped
with a bounce-on-two-stumps loco state, a third of the standard's limb hp and a
skull with no scalp. That commit added gates (`test_mobgen.mjs` §L/§M) that
DETECT the drift. It did not make the drift impossible, and it cannot: the
copies are still copies.

## Goal 1 — the human is the base class

Adding a clip, a loco state, a natural weapon, an anatomy layer or a limb
property to the human should reach every character **automatically**, at load,
without re-baking a single file. A character should carry what makes it that
character — its genome, its colours, its name — and inherit everything else.

Per-body derived numbers must STILL be derived per body: `gait.rideHeight`, the
walk/run arm-cycle periods, hp scaled by limb volume, the 15 anchors, the fist
and jaw edge segments, the socket. Inheritance must not flatten a small
character onto the human's proportions. That is the whole design tension —
inherit the CONTRACT, derive the NUMBERS.

## Goal 2 — zombie is an applied effect, not a mob def

Zombification should be a modifier applied to **any** character (any genome),
composing with whatever body it lands on: its own clips and loco states, its
gait, its bite and infection, how it comes apart, and its appearance. Today
`assets/mobs/zombie.json` is a colour variant of one specific body. It should
be a thing you apply to jujunud, or to newcomer, or to the player's corpse, and
get a zombie of that person.

Assume more such effects follow (burned, skeletal, armoured). Design for the
second one, not just this one.

## What already works, and is probably the model

- `assets/anims/` — a shared clip library compiled onto every rig it fits by
  `LoadMobDefs`, named by `attack_styles.json`. **This is the one thing in the
  system that already propagates.** Look hard at why before inventing anything.
- `extends` in a sidecar (`zombie.json`, `_harness_pale.json`) already pulls in
  a base's whole rig plus a `palette` filter — but it is whole-file, has no
  per-field override, and follows the base's `.vox`.
- `ARCHETYPE` in `mobgen.js` is the single limb-name vocabulary; every
  downstream system binds by limb NAME.
- Each generated sidecar carries its `genome`; `gen_mobs.mjs <name> --rebake`
  rebuilds from it.

## Name these in the plan — they are where it gets hard

- **Anatomy is baked into the `.vox` interior.** A body's insides are art, not
  data. Changing the human's recipe cannot propagate without re-baking art.
  Decide explicitly: resolve at load, or keep baking and make the re-bake
  automatic and cheap. Do not leave it implicit.
- Mob defs feed the sim, so the world hash and determinism are in scope.
- A rig resolved at load is a different save/replay story than a file with
  everything already in it.
- `test_mobgen.mjs` §L and §M should become largely unnecessary, or be
  re-aimed. Say which.
- Several Claude sessions work this repo at once — coordinate per `CLAUDE.md`.

## Deliverable

A phased plan in `docs/`. Each phase independently verifiable with a CHEAP
gate — `node scripts/test_mobgen.mjs` costs no build, no GPU lock and about a
second, and that is the bar. Respect the verification budget in `CLAUDE.md`:
the plan should spend runs on the behaviour it changes, not on re-confirming
hashes it moved on purpose.
