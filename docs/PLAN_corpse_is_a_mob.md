# PLAN: a corpse is a Mob

Branch `corpse-is-a-mob`, from main 8360a8c (2026-09-24).

## Why

`Mob::Die()` hands every limb body to `DebrisSystem::AdoptBody(dead=true)`. On the
next PreTick the Mob is swept out of `mobs_`. From then on the corpse is a heap of
anonymous debris. Everything the rig knew then has to be rebuilt from outside by
a parallel system:

- `CorpseReport` and the `Corpses` registry rebuild loot identity.
- `DyingStrap` and `StrapBody` re-tie the armour.
- `CorpseView`, `corpseCoat_` and `corpseShellIdx_` rebuild burn and coat state, keyed on body id and voxel count.
- `PendingRise` copies lattices under a 256k-voxel budget, so a zombie can rise from remains that are no longer a Mob.
- `MobGone` plus ghost debris replicate a death over the network.

The git history is mostly a record of porting living features over to the dead
(`60138f5`, `cb3b8e7`, `9603f91`, `92fb188`, `ab2250f`, `9d1701d`, `45e0f16`).

Several things corpses still lack:

- joint-twin sync;
- infection spread;
- blunt pulping;
- wet drip;
- fire state carried across death: `alight` is lost and only re-seeded from `selfActive`.

A corpse loaded from a save is worse off still. DBRS stores none of `dead`,
`creature`, `defIndex` or joints, so a loaded corpse is a jointless, non-flesh,
unlootable heap that cannot rise.

The handover was a day-one reuse decision (`3e7d499`, DESIGN.md "death = flip
dynamic and hand every limb to AdoptBody"). At that point no other ragdoll
existed. The live ragdoll (`b32b3ed`, mob.h `RagdollPhase::Limp`) is now exactly
the "flip dynamic, keep ownership" state a corpse needs.

## The design

**Death is a state of the Mob, not a change of owner.** `alive_ = false`. The rig
stays in `mobs_`, and every body, joint, worn shell, twin table, burn index, coat
ledger, wound, `worn_`, `heldItem_` and `carried_` stays where it is. A dead Mob
is a permanent limp ragdoll. It runs the matter passes and none of the agency
passes.

| Runs on a dead Mob | Never runs on a dead Mob |
|---|---|
| burn, char, `SyncJointTwins`, cross-limb heat | AI, steering, gait, animation, `SubmitPose` |
| stain, rain, dry, **wet**, splatter | get-up (`BeginGetUp` from any branch) |
| infection/rot spread, blunt pulp | fall damage (`TickRagdollArrest`), hit react |
| carve, cut, blunt, bite, blast, sever (as flesh) | voices after the death cry |
| bleed: wounds pay out and close, **no pump refill** | hp → death transitions, blood drain → death |
| `DriveWornShells`, `UntunnelRig`, read-back | crowd spacing, target selection, parry, attacks |

### What stays in DebrisSystem

Severed limbs (`DetachLimb`) and carved gobbets (`EmitCarvedFragment`) of living
AND dead creatures are still dead-flesh debris. That boundary is real: a part
that has left the rig has no rig to belong to. So `IsFlesh`, `ForEachDeadFlesh`,
`BurnFleshBodies`, `StrapBody` for a severed armoured arm, `PartJointsAt`,
`BleedBodies`, `CutBody`/`BluntBody` and `GoreEvent` all stay. The MobSystem
passes named `*Corpses`/`Corpse*` stay too, because they are the dead-flesh passes
for severed parts. They are renamed to say so (`BurnDeadFlesh`, `StainDeadFlesh`,
`SplatterDeadFlesh`, `FleshView`, `fleshCoat_`, `fleshShellIdx_`).

### Bounds (rule 2)

- `kMaxMobs = 16` counts **living** mobs only (`Spawn`, `HasRoomToSpawn`, unpark).
- New `kMaxDeadMobs` (count) plus a dead-rig body budget. On overflow, the
  **oldest dead Mob decays to debris** through `Mob::ReleaseRigToDebris()`, which is the
  current adoption loop from `Die()`, extracted unchanged. The evicted rig then
  falls under debris's existing 200-body FIFO cull and settle-back. So the old
  corpse lifetime is the tail of the new one, and nothing becomes unbounded.
- Render slots (`kMaxBodySlots = 512`, `bodyreg.cpp`): the avatar must not be the
  one starved. Order the avatar before mobs, or reserve its slots.
- **Sleep.** A dead Mob is asleep when all of these hold for ≥ 60 ticks:
  - every limb body is inactive in Jolt, with no `holdBody` in flight;
  - burn has no front, no `alight` and a non-zero `sleepKey`;
  - the coat has nothing active on it (a washer, a corrosive or hot coat) and
    nothing is dirty. A PASSIVE coat -- one that only dries (`coat.decay` > 0,
    not a washer, not corrosive/hot: blood, ichor, oil) -- does not hold it
    awake (P2d, `MobSystem::CoatDriesAsleep`);
  - every `bleedBudget`/`gushTicks` is 0;
  - `twinDirty_` is false, no pending spawns, and the hit flash has decayed.

  An asleep dead Mob skips:
  - `RegisterTerrainAnchor`;
  - `PostStep`'s untunnel, read-back and `DriveWornShells` (no per-tick `Activate`);
  - the burn and stain scans -- except that a passive coat still drying gets
    one visit on each tick its next level is due (`Mob::NextDryTick`, the
    material's `DryOneLimb` period). The visit is the awake `StainTick` on
    that tick, from the same pot, then the twin sync and a forced ledger
    recount; the skipped ticks are exactly the ones on which an awake corpse
    writes nothing, so the coat ends voxel-for-voxel the same (the
    `corpse-sleep` gate compares against an awake arm). A visit that writes
    anything but drying (contact, rain, wet) wakes it.

  It wakes on any of these:
  - a limb is active in Jolt;
  - any damage entry point (Damage/Cut/Carve/Blunt/Bite/Blast/Splatter/Ignite) touches it;
  - a nearby chunk version changes (the burn `sleepKey` inputs);
  - a window shift or a reload.

  Dead Mobs get their own burn and stain budget pots, as `BurnCorpses` has today, so a
  battlefield cannot starve the living.

### Loot

`Corpses`, `CorpseReport`, `SetOnCorpse`, and the corpse half of `OnBodyGone` are
deleted. The loot panel addresses a dead Mob by id (`lootCorpse` is already a mob
id), and its entries are read live from `worn_` (identity shell, dye,
`CaptureWorn` damage), the held item and `carried_`, in today's order. Take, take
all, drag out and drop to floor become operations on the dead Mob: unwear to kit
or ground, and remove a carried stack. The crosshair asks `FindOwner(body)` on a
dead Mob. `Grabbable` accepts dead-Mob limbs. The avatar still reports no loot
(the duplication argument in mob.h stands).

### Rising

`PendingRise` shrinks to `{mobId, fx, atTick, owner}`, or to a `riseAtTick_` on
the dead Mob, which is then saved with it. `ServiceRisings` reads lattices, pose,
gear and pack straight off the dead Mob. It spawns the turned def, copies the
rig across (the shape of `TurnMob`), and releases the dead rig. Deleted:

- lattice copies and `kRiseVoxelBudget`;
- `bodies`/`bodyMap`;
- the `DestroyBody` of the remains;
- `RiseGear`/`carried` capture.

`avatarKitFn_` stays, because the player's kit lives on the session.

### Avatar

`PlayerAvatar : Mob` is not in `mobs_`, and `Revive` = `Despawn` + `Spawn`
rebuilds the same object. At `Revive`, the dead avatar's rig is **moved into
`mobs_` as a dead Mob** (slice-move of the `Mob` base; `mobs_` is
`std::vector<Mob>` and `Mob` is movable). Avatar-layer bodies are released to
the world layer (`ReleaseToWorldWhenClear`, already done at death). It is not
lootable. Until the move lands, `Revive` falls back to `ReleaseRigToDebris()`,
which is exactly today's behaviour.

### Persistence

MOBS v6 adds `alive` (and `riseAtTick_`). A dead Mob's record restores its lying
pose from the per-limb `BodyTransform` it already stores and enters the dead
state directly. It never runs a standing `Spawn` pose. Dead Mobs park and unpark
like the living, and the dead cap applies at unpark (the oldest decays). v3–v5
records load as before.

### Network

A dead Mob keeps its announce. Its pose streams with the same sleep discount as
debris (`OwnedBodiesNear`). `ApplyPose` honours `alive = 0` by entering the dead
state on the ghost. `kGoneDeath` is no longer sent for deaths. Corpse ghost-debris
goes away, and severed-part ghost-debris stays.

## Packages

| Pkg | Scope | Depends |
|---|---|---|
| **P1 core** | `Die()` keeps the rig; the dead branch in PreTick/PostStep; un-gate the matter passes; caps and eviction via `ReleaseRigToDebris`; sleep; audit every `mobs_` loop (99 of them; a corpse used to be absent, so each now decides alive-only or all); `FindMobById` returns the dead; add `LiveMobCount`; delete `DeathCause` fallback and `DyingStrap`; melee/grab/session recognise dead-Mob limbs as dead flesh; loot on the dead Mob; rising from the dead Mob; avatar `Revive` fallback; the corpse gates rewritten to address dead-Mob limbs | — |
| **P2a save** | MOBS v6, dead records, parking/unpark cap | P1 |
| **P2b avatar** | dead avatar rig moved into `mobs_` at `Revive` | P1 |
| **P2c net** | pose streaming of the dead, ghost dead state, no death `MobGone` | P1 |
| **P3 finish** | rename the dead-flesh passes; delete what is now unreferenced; DESIGN.md, `tuner.html` ARCH_NODES; one `--suite acceptance`; `--rebaseline` | P2* |

## Gates

The existing corpse gates are the regression suite. They assert behaviour
(corpse cut, blunt, bleed, burn, wash, crossheat, worn, splatter, acid, armour,
dismember, intact, loot, zombify, laser-head, mob-burn G). A gate that found its
corpse by debris index is rewritten to find the dead Mob's limb. **Its assertion
is not weakened.** Gates that read "gone" as "dead" (`FindMobById == nullptr`,
`MobCount() == 0`) switch to `!Alive()`.

New assertions:

- **`corpse-sleep`**: a settled dead Mob is asleep after N ticks, costs no anchor or strap activation, and wakes on a cut.
- **`corpse-cap`**: `kMaxDeadMobs + k` deaths → the oldest decays to debris, and the living cap is untouched.
- The `mob` gate's `settled` term counts asleep dead Mobs as settled.

Determinism: the world hash moves (the RNG keys of a corpse change from `CorpseKey(body)`
to the mob key, and passes now run on the dead). Rebaseline once at the end of P3.

Control: `_scratch/control_selftest.log` is the full selftest at clean 8360a8c,
which is the list of gates already red before this work.
