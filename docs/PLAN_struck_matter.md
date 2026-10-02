# PLAN: one strike, three populations — struck matter

Status 2026-09-20: **A, B, C and D landed** — committed to main as `1e55977` (2026-09-20; audit 2026-10-02).

What a blow does is now authored once and answered by whatever it lands on. The
five divergences this plan opened with are closed, and closing the last of them
turned a LIVING gate green that had been red before any of this started.

| | before | after |
|---|---|---|
| reaction on a corpse / live ragdoll | nothing at all | impulse at the contact point, chosen by DRIVE |
| the three parts of a blow | built twice, 70 lines apart | `BuildStrikeParts`, once |
| spall (the hole growing into its rim) | `Mob::CarveLimb` only | `phys/lattice.h SpallGrow`, both |
| the bruise ladder | `Mob::BruiseLimb` only | `phys/bodystain.h SoakBruise`, both |
| a mace on a corpse | an instant sphere | mark -> break -> crumble, same three rungs |
| cutting a corpse's neck | head stays on forever | the joint parts (`PartJointsAt`) |
| a corpse under a blade | sounded like a crate | its own species' Sever/Dismember takes |
| armour on a corpse | decoration | the march-back retarget runs on it too |

**AND LIMP IS NOT DEAD.** An earlier draft of this plan said the two were
"bit-identical", which is true of the REACTION and false of the blow: a limp
creature still has hp, still cries, still dies of it. `hit-drive` now asserts
both halves — the reaction shared by drive, the physiology not shared at all —
because a gate that asserts a false thing teaches it.

Gates: `corpse-cut` `corpse-dismember` `corpse-blunt` `hit-drive`
`corpse-intact` `corpse-armor` `corpse-bleed` `corpse-burn` `impact-armor`
`impact-blunt` `body-coat` `wound-chip` `npc-strike` `bite-limbs` `swing-plane`
`ai-reach` `ai-pursue` PASS.

**`impact-blunt` was RED before this work and is now GREEN**, and that is the
best evidence the unification was worth doing: the corpse's ladder exposed a
bug in the living one. Rung 2 (a saturated bruise breaking and going bloody)
applied to every cell in the radius, which made the INTERIOR of a beaten limb
one solid reservoir of pulp — every voxel the dissolution took exposed more of
it, so a beating ate the whole limb (the gate read "1344 voxels -> 0 after 3
ticks, 100% gone, cap 60%"). Skin breaks where there IS a surface to break;
gating the rung on exposure bounds the reservoir and a mace caves a body in
instead of deleting it. Now 0.8% gone.

NOT THIS WORK, measured rather than assumed:
* `impact-fist` fails with the dissolution turned OFF and the impulse turned
  OFF — it is the hp/sever path, which this work never touched.
* `joint-rot` is a bite-infection claim; the zombie-bite session's `melee.h`
  and `attack_styles.json` are uncommitted in this tree.
* `determinism` reports PIN MOVED and says "determinism itself passed" — the
  twice-run comparison is intact, and this tree carries three other sessions'
  sim edits. Not rebaselined here: it is not this work's pin to move.
* `swing-plane` `ai-reach` `ai-pursue` `npc-strike` `body-coat` `bite-limbs`
  fail at FULL-SUITE scope and pass at subset scope with every change in
  (CLAUDE.md rule 7). `ai-pursue` was briefly attributed to the impulse and
  that was WRONG: it passes alone with the impulse on, and its fixture reports
  a different `relief` depending on what ran before it.

## The complaint

> "seems like we're treating corpses through an entirely different system than
> we were mobs; is there a way to make behavior consistent with weapons and body
> reactions etc... every time we switch behavior for a living mob that has to
> then be rewritten for corpse behavior? like right now mobs can ragdoll and
> when ragdolling they essentially behave just like corpses; so all the parts
> are practically there" — owner, 2026-09-20

Correct on every count, and the last sentence is the design. The engine has been
splitting on ALIVE vs DEAD, which is the wrong axis and is why a behaviour has
to be written twice. The axis that actually predicts what a body does is **how
it is driven**.

## The three layers

| Layer | What it is | Who has it |
|---|---|---|
| **Matter** | a voxel lattice and the carve that takes pieces out of it | a limb, a corpse piece, a crate, a dropped sword — everything a blow can land on |
| **Drive** | RIG-POSED (kinematic; the pose pipeline writes the transform; a reaction is a pose spring) vs SOLVER-OWNED (dynamic; Jolt writes the transform; a reaction is an impulse) | a LIMP LIVING LIMB and a CORPSE LIMB are the same thing here — that is the whole point |
| **Physiology** | hp, infection, voices, `Sever()`, `Die()` | an owning creature, ragdolling or not. Loose matter has none of it and must not be asked |

Written this way, "a blow rocks a body" is authored ONCE against Drive and is
correct standing, limp and dead, with no second implementation to keep in step.

The code already half-believes this and says so out loud. `Mob::HitReact`:

> "A LIMP OR DEAD BODY DOES NOT FLINCH. Once the ragdoll owns the rig, Jolt is
> what places every limb and the pose pipeline has stopped re-posing it — so a
> spring written here would be a write nobody reads... A corpse being hit
> already has an answer for where it goes, and it is a better one than this:
> **the impulse goes into Jolt.**"

That impulse has never existed. `Physics` carries only `ApplyRadialImpulse`
(explosions). So today a ragdolling LIVING mob gets no reaction at all: the
flinch correctly declines and the thing it defers to was never built.

## What diverges today (2026-09-20 audit)

1. **The carve core is two copies.** `Mob::CarveLimb` and
   `DebrisSystem::DamageBody` are the same seven steps. They have already
   drifted twice: the corpse copy decided "nothing in range" on the DERIVED
   collider (fixed today — every blade blow on a corpse was a no-op), and it
   still has no SPALL, so a second blow on a corpse stipples fresh flesh beside
   the first instead of widening the hole.
2. **Only the CUT shares its geometry** (`phys/kerf.h`). A mace on a corpse is
   `BluntBody` — a plain sphere — and a bite is the same sphere at another
   radius. Living flesh gets `Mob::BluntHit` / `Mob::BiteHit`.
3. **No reaction on solver-owned matter** (above).
4. **Armour on a corpse is decoration.** The march-back retarget
   (`Mob::WornShellAlong`) runs only for `StruckKind::Flesh`.
5. **A corpse cannot be dismembered at a joint.** Severing on the debris side is
   `ShatterBody` connectivity INSIDE one body; the Jolt joints `Die()` leaves
   are never broken by a cut. Cut a corpse's neck through and the head stays on.
6. **A corpse sounds like a crate.** main.cpp's tier logic reads "no sever, no
   voice, but a body was hit → a chip". `Body::dead` exists for exactly this and
   nothing reads it.

## The packages

### A. The reaction is chosen by DRIVE — LANDED

* `Physics::IsBodyDynamic` — the drive question, asked of the one system that
  actually owns the answer. NOT of `Mob`: a limp limb and a corpse limb give the
  same answer and neither has to be recognised.
* `Physics::ApplyImpulseAt` — impulse at a world point, so a blow spins a body
  as well as shoving it. Speed-capped by mass exactly as `ApplyRadialImpulse`
  is, for the same reason (a 0.3 kg hand is not a rocket).
* `melee.cpp`: the reaction moves ABOVE the population branch and becomes one
  call. Rig-posed → `MobSystem::HitReact` (unchanged). Solver-owned → the
  impulse, whether the body belongs to a live ragdoll, a corpse, or a crate.
* `combatfx.hitReactImpulse` (kg·m/s at the reference blow) scales it on the
  same `hp x power / hitReactRefDamage` ramp the flinch uses, so one dial moves
  both and a mace shoves harder than a fist because it IS harder.
* Gate `hit-drive`: the same blow at the same limb of the same fixture,
  standing / limp / dead. Standing must flinch and not move; limp and dead must
  MOVE, by more than the standing arm does.

### B. One strike resolver — LANDED

`melee.cpp`'s `if (kind == StruckKind::Debris) { ...; continue; }` block
re-derived the kerf and called sphere carves. It is gone: `BuildStrikeParts`
assembles the three parts once, `ResolveOnLooseMatter` is the loose answer, and
the creature branch reads the same `parts.cut` / `parts.blunt` / `parts.bite`.
The first-contact test and the bite holdout are now shared too, so a corpse's
dent is once per stroke for the same reason a bruise is.

STILL OPEN inside B, and marked as such at the call site rather than left to be
discovered: a corpse's blunt part is a crater rather than the bruise-and-pulp
the living get, and its bite is the same crater at the tooth radius. Both want
the geometry to move beside the kerf in `phys/` first — the same treatment
`phys/kerf.h` already gave the cut.

What it did NOT do, deliberately: `DebrisSystem` still exposes `CutBody` /
`BluntBody` rather than taking the `BluntHit` / `BiteHit` structs themselves.
Those structs live in `game/impact.h` and `phys/` must not depend on `game/`, so
the vocabulary of a blow stays in `melee.cpp` — which is the right home for it:
melee owns what a blow IS, and each system owns its own matter.

### C. The carve core — LANDED (as two shared passes, not one function)

The rule the corpse copy was missing — **the authoritative lattice decides
whether anything happened; the collider is derived and its loss is a
DIFFERENCE, never a prediction** — is now written into `DamageBody` itself, and
the two BEHAVIOURS that had drifted are shared outright:

* `phys/lattice.h SpallGrow` — the hole growing into its own rim, the mechanism
  that makes sustained hits dismember. It was a `Mob` member, so a blade on
  loose matter left a groove that never widened and a corpse could not be
  chopped apart at any number of blows. Templated on the cell so one
  implementation serves the int16 skin and the int8 collider.
* `phys/bodystain.h SoakBruise` — the three-rung bruise ladder, extracted from
  `Mob::BruiseLimb` with its rig preamble left behind.

A single `CarveLattices` swallowing both tails was NOT done: the tails are
genuinely different (a rig's anchors and hp on one side, a Jolt compound and a
connectivity split on the other) and folding them together would have bought
nothing the two shared passes do not already buy.

### D. The consequences — LANDED

* `DebrisSystem::PartJointsAt` — a cut that parts the flesh at a corpse's joint
  BREAKS it, by the living rule's own shape (a FRACTION of the flesh that was
  there when the blows started, `gore.corpseJointHold` / `corpseJointCut`).
  Measured: 40 sword chops take a human corpse's head off, both ends bleed. The
  fraction is load-bearing — an all-or-nothing test left 4 voxels of 476 and
  kept the head on forever.
* `DebrisSystem::GoreEvent` — dead flesh reports its own gore, carrying the DEF
  INDEX so a corpse makes its OWN species' Sever and Dismember takes. The blade
  half of the blow is what arms the wet one, exactly as for the living.
  `EdgeSweepResult::hitDeadFlesh` gives main.cpp the third answer its
  queue-differencing could never produce, so a corpse reads as FLESH.
* The armour retarget runs on loose matter, asked of physics rather than of a
  rig: a corpse's garment is a strapped follower, so one cast back along the
  blade's travel says whether the plate is in the way.

## Verification budget

Each package is one `--gate` run. A is `hit-drive` plus `corpse-cut`; B is
`impact-armor`, `npc-strike`, `corpse-cut`; C is the wound gates. No full
acceptance until D, and none of these move the world hash on their own (every
one of them is CPU gameplay state — bodies and limbs are not hashed).
