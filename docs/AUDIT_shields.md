# Audit: shields (20aa6a2 / d8b10e9, 5a74e6a, 2fba0a5)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| SH-1 | HIGH | Load/unpark/handoff restore hp and carved lattice but not the charged baselines; next carve re-bills old loss | `src/game/mob.cpp:31471` | small | no | fixed, gates pending `76ae1b5` |
| SH-2 | MED | Limp/get-up NPCs never step the shield: a knocked-down guard keeps raise>=0.5 and blocks | `src/game/mob.cpp:9787` | trivial | no | fixed, gates pending `e0a4216` |
| SH-3 | MED | A carve that rebases a held item's lattice leaves HeldHand::gripBody stale; the item drifts in the fist | `src/game/mob.cpp:10624` | small | no | fixed, gates pending `a025aa1` |

## Findings

### SH-1 (HIGH) Load/unpark/handoff restore hp and carved lattice but not the charged baselines; next carve re-bills old loss

**Status:** FIXED on branch `audit-1014` in `76ae1b5`. Confirmed. When a record overlays a carved lattice onto a rig that was rebuilt pristine, the saved hp came back but voxelsCharged, weightCharged and organCharged stayed at the spawn census, so the next CarveLimb billed all the earlier loss a second time. OverlayLimbLattice now re-derives the three baselines from the lattice it installs, using the same rule as RestoreShellLattice; the *AtSpawn denominators are left alone. That one function serves both the MOBS v4 overlay and the AVTR v2 avatar restore. The v3 branch of OverlayMobRecord got the same two lines.

**Where:** src/game/mob.cpp:31471 (OverlayMobRecord -> OverlayLimbLattice, 31344-31401)

**What:** A creature that comes back from a save load, an unpark or a multiplayer handoff is rebuilt pristine. Rebuilding sets each limb's voxelsCharged, weightCharged and organCharged to the spawn census. The overlay then restores the saved hp and the carved lattice but never re-syncs those three baselines. CarveLimb charges damage as the difference from those baselines, so the next carve on any limb that was damaged before (body limbs and the NPC's held shield) bills all the earlier loss again: limb hp a second time, and organ hp a second time.

**Trace:**
- src/game/mob.cpp:4786-4791 / 32869-32872: rig build and EquipItem set the charged fields to the pristine census
- src/game/mob.cpp:31644: LoadOne runs ApplyWireGear (the held item is equipped pristine)
- src/game/mob.cpp:31443: `L.hp = rec.limbs[i].hp` (already net of the earlier charges)
- src/game/mob.cpp:31380-31401: OverlayLimbLattice swaps in the carved lattice; the charged fields are untouched
- src/game/mob.cpp:17931-17978: CarveLimb charges `lostWeight = weightCharged(pristine) - nowWeight`
- src/game/mob.cpp:17997-18008: the same for organCharged (flat organ hp)
- Contrast src/game/mob.cpp:33698-33702: RestoreShellLattice does re-sync

**Failure scenario:** An NPC's round shield has had about 15% of its weighted matter chipped away and sits at hp 25. The NPC parks and later unparks. Its next block's small chip charges about 15 hp instead of about 2, and the following blow breaks the shield. In the same way, a creature that lost brain cells before a save takes the full brain-organ charge again from its next scratch after load, which can kill it.

**Suggested fix:** At the end of OverlayLimbLattice (or once per limb at the end of OverlayMobRecord, and in the AVTR restore), set `L.voxelsCharged = live count; LimbWoundTotals(L, L.weightCharged, L.organCharged);` as RestoreShellLattice does. Leave voxelsAtSpawn and weightAtSpawn alone.

**Verifier's note:** I confirmed that no other writer of the charged fields runs on the load, unpark or handoff path (grep of voxelsCharged and LimbWoundTotals). The effect covers every carved limb, not only shields. I did not open the AVTR path at avatar.cpp:287.

_Also reported independently by: AUDIT_venom_necrosis.md (venom necrosis); AUDIT_done_sweep.md (G5: persists)._

### SH-2 (MED) Limp/get-up NPCs never step the shield: a knocked-down guard keeps raise>=0.5 and blocks

**Status:** FIXED on branch `audit-1014` in `e0a4216`. NPCs called StepShield, where a ragdoll's raise gets lowered, only in the standing branch. A guard knocked limp with its shield up kept raise >= 0.5 through limp and get-up and still counted as a guard. The NPC limp branch, the get-up branch and the went-limp-this-tick exit before `continue` now call mob.StepShield().

**Where:** src/game/mob.cpp:9787 (Limp branch), 9803 (GetUp branch), 9881 (fall-to-limp `continue`)

**What:** StepShield is where a ragdoll's shield comes down (`up` requires `ragdoll_ == None`), but NPCs call it only from the standing branch. A creature knocked down with its shield raised keeps shieldRaise_ frozen at or above 0.5 through the whole limp and get-up. During that time ShieldUpForWatchers() stays true: FindShieldBlock treats the flopping shield as a guard, the actor list publishes Guard, and peers receive kShieldUp. The player avatar steps its shield every tick (session.cpp:3350), so the player and NPCs behave differently.

**Trace:**
- src/game/mob.cpp:9787-9811: the Limp and GetUp branches have no StepShield
- src/game/mob.cpp:9881-9885: the tick UpdateFall goes limp, `continue` runs before the 9916 StepShield
- src/game/mob_shield.cpp:93-94: the ragdoll rule that never runs for these NPCs
- src/game/mob.h:2397: `ShieldUpForWatchers` reads `shieldRaise_ >= 0.5f`
- src/game/mob_shield.cpp:209-216: the block test gates only on alive and ShieldUpForWatchers

**Failure scenario:** A guard with its shield fully up is shocked or blasted into a ragdoll. For the roughly 2.5 s of limp plus 1.5 s of get-up, the player's follow-up cuts that cross the shield are arrested with a clang and chip it. AI profiles keyed on TargetGuarding feint or circle a body lying on the ground.

**Suggested fix:** Call `mob.StepShield()` in the Limp and GetUp branches and before the 9881 `continue`, or zero shieldRaise_ and clear the want in StartRagdoll.

**Verifier's note:** I confirmed by grep that shieldRaise_ is written only in StepShield and BreakHeld and that StepShield has a single NPC call site. I did not check the pose-clip weight during get-up beyond the claim's citation.

### SH-3 (MED) A carve that rebases a held item's lattice leaves HeldHand::gripBody stale; the item drifts in the fist

**Status:** FIXED on branch `audit-1014` in `a025aa1`. ReskinLimbMicro corrected xf, restOffset and anchorLimb for a lattice rebase but not HeldHand::gripBody, which places held items every tick. A min-plane carve therefore slid the item off the fist by the shift each time. For a held slot, gripBody now moves by -d like anchorLimb. So do the slot's limbDefs_ edgeFrom/edgeTo/haftFrom/haftTo, which are points in the same body frame (WeaponEdge, the aim tip, coat transfer). Moving only the grip would have left the weapon edge d off the blade.

**Where:** src/game/mob.cpp:10624 (held-item placement using hh.gripBody); rebase at 15971-15995

**What:** A held item's grip point in its body frame is measured once, at EquipItem. When a carve (a ChipShield block, a mace dent, burning) removes every cell of a min-side layer, MicroBodyEdit rebases the lattice and ReskinLimbMicro moves the limb's transform and rig offsets to compensate. Held items, however, are re-placed every tick from the stale gripBody. As a result the art, the collider and the lattice FindShieldBlock tests all shift by the rebase amount relative to the fist, and the shift accumulates with each such carve.

**Trace:**
- src/sim/microbody.cpp:671-672: originShift = new lattice minimum
- src/game/mob.cpp:15983-15995: xf.pos, restOffset, rebaseUnbuilt and anchorLimb are corrected by d; gripBody is not
- src/game/mob.cpp:32926-32929: gripBody = GetLocalBounds min + gripLocal, set only at equip
- src/game/mob.cpp:10624-10630: `pos = socketW - Rotate(itemQ, hh.gripBody)` overrides the corrected xf
- src/game/mob.cpp:36409-36426: the weapon edge also reads the stale gripBody

**Failure scenario:** A chip takes the heater shield's 10-cell steel point (its min-Y plane). The lattice rebases by a few skin cells, and from the next tick the shield sits about 1-4 cm off the hand. Further chips on min planes add to that.

**Suggested fix:** In ReskinLimbMicro, when the limb is a held slot, subtract d from held_[hand].gripBody, or re-measure gripBody from GetLocalBounds after RebuildLimbBody as EquipItem does.

**Verifier's note:** I confirmed by grep that gripBody has a single writer and that the GetLocalBounds frame is body-origin-local. The drift per event is small (skin-cell scale). The defect is general to held items, not only shields.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Shield hp and lattice ride MOBS limb records (appended slots are always kLimbStored, mob.cpp:30930) and PLYR via KitFlushWorn/CaptureHeld (persist.cpp:212, coattransfer.cpp:72). However, the MOBS/unpark/handoff overlay never re-syncs voxelsCharged/weightCharged (mob.cpp:31344 vs 33698), so old chip volume is re-billed at the first chip after it comes back. The raise, stagger and pending attack are deliberately unsaved (mob.h:3665). |
| deterministic | OK | All CPU logic runs in fixed ForEachCreature order (mob.h:6786) with counter-derived seeds (melee.cpp:1686) and tick-recorded input (session.cpp:3346); getenv is used only for traces. Across peers the attacker judges by design (melee.cpp:1687). |
| sleeps when idle | OK | StepShield is O(1) per creature per tick (mob.cpp:9916, session.cpp:3350). FindShieldBlock runs only inside a cutting sweep and exits early on ShieldUpForWatchers (mob_shield.cpp:216). The raise clip is retired at zero (pose.cpp:751). GearKeyOf costs a few mixes per visible mob (entitysync.cpp:520). |
| cpu mirror | N/A | Shields never read the voxel grid. The block test reads only the holder's own slot lattice (mob_shield.cpp:243-258), so there is no world-mirror dependency. |
| mutation queue | OK | Chip ejecta go into the caller's spawns (melee.cpp:1463,1480), or into a discarded sink on a ghost (melee.cpp:1704). EmitCarvedFragment refuses on ghosts (mob.cpp:17482), and BreakHeld's remains become a DebrisSystem body (mob_shield.cpp:136). There are no direct voxel writes. |

## Coverage (as reported by the finder)

**shields.** Read in full: src/game/mob_shield.cpp; the landing diffs of 20aa6a2, 5a74e6a and 3fd2cb2 (shield parts) and 2fba0a5's stat; DESIGN.md '### Shields' (25175-25352).
Traced in melee.cpp: LoadItems shield parse, ChipShield, ResolveShieldBlock, the MeleeSweepDamage shield block and first-contact pre-check, FindParry ordering, the probe loop's held-slot skip and worn-shell retarget, the ApplyOneRemoteHit Parry/shield branch, and BuildStrikeParts' inputs vs the wire.
Traced in mob.cpp: EquipItem (strike hand, census, gripBody), ArmForStyle/StrokeHandFor/StyleUsable, ApplyAiArm, the NPC PreTick loop (limp/getup/standing branches, pending attack, StepShield), Steer/DriveLocomotion caps, PushBlockEvent, Damage's hp-0 path and HpZeroSevers, CutLimb's shell chip, CarveLimb's charge accounting, ReskinLimbMicro, RebuildLimbBody, DetachLimb, TickSeveredHolds orphan sweep, SubmitPose held placement, GearKeyOf, RedressFromWire/ApplyWireGear/ApplyAnnounce/ApplyHandoff/ApplyState, BuildPoseOf/ApplyPose/PlaceFromOwnerPose, SaveOne/LoadOne/OverlayMobRecord/OverlayLimbLattice/AlignRecordToRig, CaptureGear, KitFlushWorn, CaptureHeld/EquipItemAsWas/RestoreShellLattice.
Also traced: session.cpp (shield block, bank/shieldYield, ResolveHands, actor publish, dress seam, the player's sweep arrest/haft); remoteplayer.cpp; entitysync.cpp's re-announce and PlayerBody; persist.cpp's kit flush and unpark; pierce.cpp (arrows into a shield); microbody.cpp MicroBodyEdit; voxload.cpp's rebase; gen_shield_items.py geometry, including the per-axis min-plane cell counts computed in throwaway Python.
NOT audited: the selftest gates (shield, net-shield, net-player-shield). PlayerAvatar::PreTick/PosePipeline for the player's shield pitchFollow. The burn pass on held slots beyond the known 'burnt shield drops as the item'. ShedGearBeforeDetach in full. HitReact internals. DriveSwim/DriveAfloat (they do not apply moveScale, which I did not pursue). CarveLimb's collapse/split rules on a Shell held slot (can a chip collapse-sever a shield and drop it as the item?). The meleehit.h wire rules beyond the shield branch. The F1 overlay UI and main.cpp's stub kit and help text. Jolt convex-radius effects on GetLocalBounds.
Noted but not filed:
- Arrows, blasts and acid can take shield hp to 0 or below without breaking it (BreakHeld runs only from ApplyShieldBlock), so the next melee block breaks it regardless of force.
- lowerMs is clamped to 2000 ms while a bank lives 30 ticks, so a shield authored above about 1.5 s lowerMs can never strike from a full raise (no shipped shield is that slow).
- The player's respawn re-equips from the kit's last flushed damage (no KitFlushWorn at death); this is general to all gear and a design question.
F1 (the post-load charge re-bill) is general to every carved limb; it is filed here because it breaks shield wear accounting.

