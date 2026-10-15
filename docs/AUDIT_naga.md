# Audit: the naga (2a17f79, 2c14c59)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| NG-1 | MED | Waist tail1 tagged 'spine' takes a share of every spine twist and yaws the whole path-laid trail | `src/game/pose.cpp:1777` | small | no | fixed, gates pending `8a3690b` |
| NG-2 | MED | Aura bolts that hit a naga's trail miss it: bodyIdAt measures to MobBodyBox, which excludes trailing parts | `src/game/session.cpp:4198` | small | no | fixed, gates pending `d7e84ca` |

## Findings

### NG-1 (MED) Waist tail1 tagged 'spine' takes a share of every spine twist and yaws the whole path-laid trail

**Status:** FIXED on branch `audit-1014` in `8a3690b`. ApplySlither lays the trail's yaw in the model frame. tail1, the spine-tagged waist above the chain, then took a share of every later spine stage (torso twist, look/aim, air/climb/hit leans), which swung the whole trail about the hips. pose.cpp now records the local rotations of the spine-tagged ancestors right after ApplySlither, and before the flatten hands the part below the deepest of them (tail2) back whatever they turned. The waist still turns and the trail stays on its path; rigs whose chain hangs from the root (snake, fish) skip this. ApplyAimPart also now subtracts only the shares of spine joints above the aimed part, which fixes the head-look falling short by the waist's share.

**Where:** src/game/pose.cpp:1777 (path yaw solved in the model frame, before the spine stages); assets/mobs/naga.json:103-108 (tail1 tag "spine")

**What:** tail1, the naga's waist, is tagged `spine` on purpose so it shares the look and aim twist. But the trail's path yaw is solved earlier, in the model frame, and assumes tail1 and tail2 add no yaw. Every later spine stage (the stroke's torso twist, the head-look and bite aim, the hit, air and climb leans) yaws tail1 by its share, and that turns tail2 and the whole path-laid trail rigidly about the hips. The look also falls short: ApplyAimPart removes tail1's share from the head's remainder, but the head does not hang from tail1.

**Trace:**
- assets/mobs/naga.json:5316 slither chain = tail3..tail12 (tail1/tail2 excluded)
- src/game/pose.cpp:1755-1762 path yaw = atan2(path dir) - heading_, in the model frame
- src/game/pose.cpp:1781-1785 parentYaw = 0 for tail3 (parent tail2 is not in the chain)
- src/game/pose.cpp:1179 AnimApplySpineTwist(weapon_.torsoTwist), after ApplySlither (pose.cpp:1007-1012)
- src/game/anim.cpp:633-651 nSpine = 2 (torso + tail1), yawPer applied to tail1
- src/game/melee.cpp:2902 torsoTwist up to +-0.5 rad, so tail1 yaws up to 0.25 rad
- src/game/mob.cpp:34442-34455 ApplyAimPart counts tail1 into spineTotal; the head (via torso) never inherits it

**Failure scenario:** An NPC naga swings a sword, punches, bites or looks at a target off its nose. The 3-4 m trail lying on its slithered path swings sideways about the hips by up to about 14 deg, moving its tip several voxels, then swings back as the stroke ends. Its kinematic limb bodies sweep through whatever lies beside it. The idle head-look also undershoots by tail1's share.

**Suggested fix:** Keep the waist turn but stop it carrying the trail. After the spine and aim stages, counter-yaw tail2 (or tail3) by the yaw tail1 picked up, so the trail stays on its path. Alternatively, have the share laws count only spine parts on the root-to-aimed-part chain, which also fixes the head-look shortfall. Retagging tail1 as `tail` is simpler, but it drops the designed 'turns at the waist'.

**Verifier's note:** The claim's 'torso gets half the twist' is the design's own split (DESIGN.md:15532-15534), so it is not a defect on its own. The confirmed defects are the trail being swung off its path (no compensation anywhere after pose.cpp:1179) and the head-look shortfall from ApplyAimPart's remainder arithmetic.

### NG-2 (MED) Aura bolts that hit a naga's trail miss it: bodyIdAt measures to MobBodyBox, which excludes trailing parts

**Status:** FIXED on branch `audit-1014` in `d7e84ca`. Confirmed: bodyIdAt measured only to MobBodyBox, which excludes trailing parts from worldSize, so a status from a bolt resolved on a naga's tail never found the naga. The same defect was in the creature-caster probe in demon_cast.cpp. In both probes, when the box distance misses, each limb with MobLimbDef::trailing is now also measured to its nearest voxel. A cheap LimbVoxelsNearWorld(from, best) > 0 test runs first, which also guards LimbNearestVoxelWorld's empty-limb fallback. Only trailing limbs pay this cost, and only on status adoption. mob.cpp/mob.h were not touched; only existing public accessors are used (FindCreature, LimbCount, LimbDefAt, LimbVoxelsNearWorld, LimbNearestVoxelWorld).

**Where:** src/game/session.cpp:4198 (bodyIdAt measures to MobBodyBox); src/game/mob.cpp:2404 (trailing parts excluded from worldSize)

**What:** A spell's body hit is resolved against real limb bodies, so a bolt can strike a naga's trail. Attaching the resulting `aura` status, though, looks for the nearest live MobBodyBox within the effect radius + 2. That box is the upright figure only, because trailing parts are measured out of worldSize. A hit on most of the 3-4 m trail finds no naga.

**Trace:**
- src/game/spell.cpp:5030-5035 (and the sweep branch above it) impactAt = point on the hit limb body
- src/game/session.cpp:4171-4182 bodyHit = Physics::CastRayBody against moving-layer limb bodies
- src/game/spell.cpp:3915-3929 Sustain creates SpellStatus at that point
- src/game/spell.cpp:4350-4357 r = ScaleRadiusCbrtMille(radius) + 2; bodyIdAt(from, r)
- src/game/session.cpp:4209-4225 nearest live MobBodyBox by box distance
- src/game/mob.cpp:29417-29419 MobBodyBox = origin_ + worldSize
- src/game/mob.cpp:2404 `ld.trailing` parts skipped when measuring worldSize
- src/game/session.cpp:4230-4236 player fallback within r + 9

**Failure scenario:** A player fires a fire-aura or other `aura`-mod bolt at a naga's tail and hits a mid or far trail segment. No naga box is within r, so the status attaches to the caster (if within about 9 + r voxels), to another nearby creature, or to the place, where a sustained mod does nothing. The spell silently fails on the naga.

**Suggested fix:** Carry the hit body's handle out of bodyHit, or the sweep, and resolve the owner with MobSystem::FindOwner. Attach the status to that mob directly and use the box search only when there is no body hit. Alternatively, measure bodyIdAt against posed limb boxes, including trailing parts.

**Verifier's note:** Read the worldSize exclusion, MobBodyBox, both impact branches, the Sustain status creation, the adopt radius and bodyIdAt. Nothing between them maps a hit body to its owner. The claim's per-segment distances (about 29 voxels base, about 40 constrictor) were not re-measured, but DESIGN.md calls the trail 'a four-metre snake', which is far beyond a 3-voxel mod radius.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | OK | Naga-only state is presentation, rebuilt when empty (pose.h:226-233 tailPath; mob.h:5891 tailDrape_). Gameplay state rides the generic per-def MOBS record, and the merge keeps base order (sidecar.cpp:56-74). No gate round-trips a naga record. |
| deterministic | OK | The NPC pose runs at a fixed dt of 1/30 (mob.cpp:9291). Path state is per mob, and the thread_local scratch is cleared on every call (pose.cpp:1651-1667). The closeStyle pick is a pure function of distance and def (mob.cpp:6954-6963). No RNG or frame-local input. |
| sleeps when idle | PARTIAL | Every in-window naga runs the path walk and DrapeTail's roughly 20 GroundHeightAt column scans every tick, even standing still (mob.cpp:6147-6191). Every mob pose does the same; a naga is parked outside the window. |
| cpu mirror | OK | DrapeTail reads ground only through GroundHeightAt's chunk cache, with coalesced fetch requests, and holds on unknown (mob.cpp:6105-6112, 6174). The trail reaches 29-40 voxels behind the body, so it touches more chunks than a human does. |
| mutation queue | OK | No naga code writes voxels. ApplySlither and DrapeTail are pose-only. Bite and venom use the shared BiteHit/StainWoundAs path on mob lattices (mob.cpp:6954 is only a style choice). |

## Coverage (as reported by the finder)

**naga.** Read in full or traced:
- The C++ diffs of 2a17f79 and 2c14c59.
- The JS diffs of 2a17f79 (mobgen, sidecar.js, breed, gen_mobs) and f45c4a7 (mobgen/serpent/breed integer key packing, ranges checked against the comments).
- sidecar.cpp and sidecar.js in full. Tombstone semantics are identical in both, including the shared latent case where a tombstone is copied in when the base lacks the array.
- DESIGN.md sections: naga, snake, inheritance.
- pose.cpp: IsLegChain, PosePipeline branches, ApplySlither follow-path (bounds, wrap, retraction, trim, NaN guards), StepGait height and tilt, gait clip pick.
- mob.cpp:
  - DrapeTail, ConformProneSegments, BuildRig restHipY_ and step-up (the cap equals the human's, about 6 cells).
  - BuildMobDef: trailing, worldSize, slither/slither.water, closeStyle, gait, states, clips.
  - BeginStroke closeStyle, PickTargetLimb, ApplyStrikeAim/ApplyAimPart/AimPartAlong/HitReact, BodyKeepOut, WearOnRig (leg armour refused cleanly), DetachLimb, SetDefs and the R reload, DefWithEffects, TickGhost, MobBodyBox, CarveRadialAll (blasts and melee hit the trail per limb).
- mob_afloat.cpp in full; the spell status adopt and projectile body-hit path; the session.cpp and demon_cast.cpp body probes; mobsync.h; the entitysync handoff note.
- Data: naga.json and the six variants, human.json, zombie.json, the clip library's modes and masks, serpent.js limbs/slither/states/clips, model sizes from naga.vox and constrictor.vox.

Verified as non-issues:
- The IsLegChain change affects only the naga; only critter, human and naga define chains.
- No hard-coded leg names in NPC paths.
- No limb bitmask overflow at 21+ limbs.
- tailPath memory is bounded (about 1.5 tails plus 4 voxels, points at least 0.5 apart).
- The follow loops are guard-bounded.
- closeStyle selection is deterministic.

Not covered:
- naga.js and serpent.js art and tube generation beyond the keys.
- The anatomy bake.
- The venom/envenom path (separate audit).
- test_mobgen section R.
- Ragdoll and corpse behaviour of a 12-segment tail (corpse-* reds are known).
- Swim clip content.
- Which Jolt layers the kinematic tail bodies actually collide with: the player capsule versus debris was not traced.
- The behaviour-profile assignment of an F1-spawned naga.

