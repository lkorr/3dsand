# Audit: severed-limb buoyancy and carrion (369900f, bff075b)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| SB-1 | HIGH | DBRS save drops Body::dead: reloaded severed limbs float as never-alive, lose carrion/coat | `src/phys/debris.cpp:8201` | small | no | fixed, gates pending `0468793` |
| SB-2 | HIGH | Settle-back of a body resting on a lake bed: IF_AIR ops refused by water, body deleted | `src/phys/debris.cpp:2991` | small | no | fixed, gates pending `731f5d6` |
| SB-3 | MED | Blown-apart bloody piece's ~600-spawn blood burst crowds matter out of the 4,096 spawn ring | `src/phys/debris.cpp:5046` | small | no | fixed, gates pending `0211d6a` |
| SB-4 | MED | Floating debris at rest is re-woken by SettleBodies every ~60+ ticks, forever | `src/phys/debris.cpp:2956` | small | no | fixed, gates pending `4f58fab` |

## Findings

### SB-1 (HIGH) DBRS save drops Body::dead: reloaded severed limbs float as never-alive, lose carrion/coat

**Status:** FIXED on branch `audit-1014` in `0468793`. Confirmed. The 'DBRS' per-body record had no dead flag and LoadState restored every body as never-alive. After a load a severed limb lost corpseAirFraction buoyancy, stopped counting as carrion and dropped out of the coat and burn passes. DebrisSystem::kSaveVersion is now 6, and SaveState writes after each record the same u32 flags word (bit 0 = dead) that the strap-group format already carried. LoadState reads that word before any skip and passes it to RestoreBody, and v4/v5 sections still load as never-alive. LoadBodyGroup now accepts per-body versions 4 through 6, so older ITMS group records still load after the bump.

**Where:** src/phys/debris.cpp:8201 (LoadState), src/phys/debris.cpp:7980 (WriteBodyRecord)

**What:** The 'DBRS' per-body record has no `dead` flag, so LoadState restores every body with dead=false. Since 369900f, `dead` decides whether FloatBodies takes off ragdoll.corpseAirFraction. A severed human limb that floated before a save therefore floats by the never-alive rule after the load and can sink. `dead` also gates IsFlesh and ForEachCoatBody, so the reloaded piece stops being carrion and drops out of the coat and burn passes.

**Trace:**
- src/phys/debris.cpp:7980-8003 WriteBodyRecord: pose, scales, dye, bleedMat, strap, lattices; no dead flag
- src/phys/debris.cpp:8201 `RestoreBody(std::move(s), ps.host < count, false)`, where the third parameter is `dead` (debris.h:2191)
- src/phys/debris.cpp:2809 `FloatingDensity(BodyMatterDensity(b), b.dead, air)`
- src/phys/floatrho.h:58-60 the air fraction applies only when wasAlive
- src/phys/debris.h:296-298 IsFlesh needs dead; src/phys/debris.h:321 ForEachCoatBody needs IsFlesh, item or dead
- src/phys/debris.cpp:8273 / 8302 the strap-group (ITMS) format does save dead, so only item-holding groups survive

**Failure scenario:** A severed leg floats at about 0.93 x 1,010 kg/m3, below water. Save, then load. The leg comes back with dead=false, floats at about 1,010 once something wakes it, and sinks while the corpse (MOBS v6, dead) still floats. After the load, fish no longer target the leg as carrion, and a blood or lava coat on it is never rinsed or dried.

**Suggested fix:** Bump DebrisSystem::kSaveVersion and append a per-body flags word (bit 0 = dead) after WriteBodyRecord in SaveState, as SaveBodyGroup does at debris.cpp:8273. Pass it to RestoreBody, and keep loading v4/v5 as dead=false. Optionally carry the wound, creature and defIndex in the same bump.

**Verifier's note:** I confirmed the save/load asymmetry and both readers of the flag. The missing flag is already called out in comments (debris.h:1538, PLAN_corpse_is_a_mob.md:62) but was never fixed for plain DBRS bodies, and 369900f made it visible as buoyancy. The wound, creature and defIndex losses are secondary. The core defect is the missing dead flag.

### SB-2 (HIGH) Settle-back of a body resting on a lake bed: IF_AIR ops refused by water, body deleted

**Status:** FIXED on branch `audit-1014` in `731f5d6`. SettleBodies judged support only from the cell under each voxel. It then emitted kCellOpIfAir ops and released the body in the same tick, so a body asleep on a lake bed stamped into water, sim_mutate refused every op, and the matter was lost. The emission loop now reads each target cell from the chunk mirror. A cell that is not air (water, smoke, solid) or whose chunk is not fetched (a fetch is requested) holds the settle: the ops are rolled back, the body is not woken, and it is re-tested 30 ticks later. Holds are counted in the new FloaterProbe::settleTargetBlocked.

**Where:** src/phys/debris.cpp:2991 (SettleBodies op emission); assets/shaders/sim_mutate.wgsl:616

**What:** SettleBodies converts an asleep, aligned, supported body into kCellOpIfAir cell ops and releases the body in the same tick. Support is judged only from the cell below each voxel. Nothing checks whether the target cells are free, and sim_mutate refuses an IF_AIR write onto any non-air cell. A body resting on the bed under water therefore writes into water, every op is refused, and the matter is gone.

**Trace:**
- src/phys/debris.cpp:2767 FloatBodies skips asleep bodies, so a sinking piece rests on the bed and sleeps
- src/phys/debris.cpp:2882-2891 asleep for at least settleAfterTicks with the wound closed, tested every 30 ticks
- src/phys/debris.cpp:2639-2656 support = SOLID or POWDER below, so the bed qualifies
- src/phys/debris.cpp:2991 `PackVoxNew(mat, state) | kCellOpIfAir`
- src/phys/debris.cpp:3014-3024 ReleaseBody + erase, unconditionally
- assets/shaders/sim_mutate.wgsl:615-616 `if (prevMat != MAT_AIR) { return; }`

**Failure scenario:** Rock rubble blasted into a lake, a brass automaton's limb, a helmed severed head, or any human piece after a reload (claim 1) sinks and lies within about 20 degrees of axis-aligned on the bed. About 2-3 s after Jolt sleeps it, its voxels are sent as IF_AIR ops into water cells, all are refused, and the body has already been released. The piece vanishes, and any strapped follower is unstrapped and falls alone.

**Suggested fix:** Before emitting, read each target cell from the chunk mirror the support test already uses (abstain on unknown). If any target cell is liquid (or otherwise non-air and non-gas), refuse the settle and leave the body asleep without waking it.

**Verifier's note:** I checked every early-out in SettleBodies and the IF_AIR branch of sim_mutate. There is no liquid guard and no post-write check, since NoteGridWrite records only the intended word. The defect predates the audited commits. The partly submerged case near a shore loses the submerged voxels the same way.

### SB-3 (MED) Blown-apart bloody piece's ~600-spawn blood burst crowds matter out of the 4,096 spawn ring

**Status:** FIXED on branch `audit-1014` in `0211d6a`. When a bloody piece was destroyed, its blood burst (about 600 spawns at default tuning) went into the shared per-tick spawn vector before the piece's own remainder, and it could use the whole kMaxParticleSpawnsPerTick ring. Later emitters' bone and flesh were then silently dropped. The remainder (VoxelsToParticles) is now emitted first, and the burst stops once the ring is half full, so other matter always has the other half. I did not add the suggested counters for spawns that VoxelsToParticles and LimbVoxelsToParticles drop: VoxelsToParticles is const and the probe struct is not mutable.

**Where:** src/phys/debris.cpp:5011-5046 (destroy branch, bff075b)

**What:** A destroyed bloody piece pushes nVox + nDrop blood spawns into the tick's shared spawn vector (595 droplets at default tuning, plus nVox whole blood voxels) before its own remainder goes out through VoxelsToParticles. Matter emitters return silently once the vector reaches kMaxParticleSpawnsPerTick (4,096). So a few bursts in one blast can delete the bone and flesh carved off every body and creature processed after them, and nothing counts it.

**Trace:**
- src/phys/debris.cpp:5019-5025 nVox and nDrop; tuning.json severSpray 1190, bleedGain 0.5, so nDrop = 595
- src/phys/debris.cpp:5026-5044 burst loop, capped only by the ring
- src/phys/debris.cpp:5046 remainder emitted after the burst
- src/phys/debris.cpp:3857 `if (spawns.size() >= kMaxParticleSpawnsPerTick) return;  // ring full: lost`
- src/phys/debris.cpp:4913-4914 later bodies' carved matter goes into the same vector
- src/game/session.cpp:5471 then 5484 CarveMobsRadial shares the vector; src/game/mob.cpp:15437 same silent return

**Failure scenario:** A grenade lands in a pile of severed pieces and the crater destroys about 7 bloody pieces (fewer if heads, or if earlier emitters already filled part of the ring). The ring fills with blood. The remainder of later pieces and the flesh carved off a living creature in the same blast are removed from their bodies or rigs and never emitted.

**Suggested fix:** Emit the remainder before the burst, and cap the burst to a headroom budget (for example, stop at kMaxParticleSpawnsPerTick/2, like the 512 cap spells live under), or pay the gout out over later ticks through a short-lived wound under BleedBodies' rationing. Count the spawns that VoxelsToParticles and LimbVoxelsToParticles drop.

**Verifier's note:** I confirmed the ordering, the shared vector and the silent drop. The exact head figure (~1,240) depends on wound.scale, which I did not re-derive. Without the burst, the ring-full loss would need far larger blasts to reach.

### SB-4 (MED) Floating debris at rest is re-woken by SettleBodies every ~60+ ticks, forever

**Status:** FIXED on branch `audit-1014` in `4f58fab`. SettleFootprintSupported counted only solid and powder below a body as support. An aligned floater asleep on water therefore read as hanging over a void, and SettleBodies reset its counter and woke it every ~60 ticks, forever. The test now returns a three-valued DebrisSystem::SettleSupport {None, Ground, Liquid}. Liquid (liquid below, no ground) leaves the body asleep and unstamped with its clock still running, and it is re-tested every 30 ticks. Only None wakes the body and increments settleWithoutSupport, so the floaters gate's no-settle-in-air arm is unaffected.

**Where:** src/phys/debris.cpp:2956-2962 (SettleBodies' unsupported branch)

**What:** A floater that Jolt has put to sleep reaches the settle test after settleAfterTicks. If it is axis-aligned, the cells under it are water, which the support test does not count as SOLID or POWDER. SettleBodies treats it as hanging over a void, resets its counter and wakes it. It bobs, sleeps and is woken again, for as long as it floats. That contradicts the guarantee FloatBodies and ApplyBuoyancy document: a raft at rest stays asleep (rule 2).

**Trace:**
- src/phys/debris.cpp:2886-2891 asleep for at least 60 ticks, tested every 30
- src/phys/debris.cpp:2926 / 2931-2937 aligned with a non-empty downsample
- src/phys/debris.cpp:2654-2656 water below fails the SOLID/POWDER test, so the result is false
- src/phys/debris.cpp:2956-2962 `settleWithoutSupport++`, `inactiveTicks = 0`, `ActivateBody`
- src/phys/debris.cpp:2767 / 2809 once awake, FloatBodies walks the column and BodyMatterDensity is evaluated again
- src/phys/debris.cpp:2687-2690, src/phys/physics.cpp:3369-3371 the stated never-cost-forever guarantee

**Failure scenario:** A severed thigh, head or plank floats at rest on a pond within about 20 degrees of axis-aligned. Every ~2-3 s it is woken and stepped until Jolt sleeps it again. A dismembered corpse in a lake keeps several bodies cycling indefinitely, so idle cost no longer reaches zero. No gate asserts that debris floaters sleep.

**Suggested fix:** Make the support test three-valued. When liquid is below, leave the body asleep and neither wake it nor reset its counter. Alternatively, skip settle testing for bodies FloatBodies last found wet. Only an affirmatively empty (air) cell below should wake a body to fall.

**Verifier's note:** I checked the guard list in SettleBodies and the support predicate, and nothing exempts floaters. The cycle predates 369900f for planks and logs. 369900f widens it to severed flesh, which used to sink as muscle.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | GAP | The DBRS record has no `dead` flag (debris.cpp:7980-8003), and LoadState calls RestoreBody(..., false) at debris.cpp:8201. The air fraction at 2809 and IsFlesh (debris.h:296) are therefore lost on load. The wound is not saved either. floatRho is derived data and is correctly recomputed. |
| deterministic | OK | FloatRho sums in fixed vector order (floatrho.h:29-38; debris.cpp:2710-2713 walks bodies_ in storage order). The burst RNG is counter-based on serial and k (debris.cpp:5031). Buoyancy only feeds Jolt impulses; nothing float reaches the CA. |
| sleeps when idle | PARTIAL | FloatBodies skips asleep bodies (debris.cpp:2767) and the density is cached. But SettleBodies wakes every aligned floater over water about every 60 ticks (debris.cpp:2956-2962). |
| cpu mirror | OK | The waterline is read from the chunk mirror, and a cell it cannot see counts as dry (debris.cpp:2741-2755, mob.cpp:28640-28653). Density reads only CPU lattices. |
| mutation queue | OK | Buoyancy writes no voxels. Blood goes out as ParticleSpawns, each checked against the cap before its push (debris.cpp:5029) and recorded at SubmitTick. Two caveats are separate findings: the burst crowds out matter in the shared ring (5028-5046), and settle-back's IF_AIR ops into water are refused (2991; sim_mutate.wgsl:616). |

## Coverage (as reported by the finder)

**severed buoyancy.** Static read of the frozen worktree at 20aa002. Nothing was built or run.

**Read and traced:**
- The diffs of 369900f, bff075b and 9557189, plus the stat of dd4def0 for context.
- src/phys/floatrho.h, whole file.
- debris.cpp:
  - FloatBodies, BodyMatterDensity, FloatingDensityOf, SettleBodies, SettleFootprintSupported, DownsampleMicro.
  - FleshOf, ForEachDeadFlesh / ForEachCoatBody / IsFlesh (debris.h).
  - DamageBody in full, including the bff075b destroy branch; PartJointsAt, ArmWound, BleedBodies.
  - DamageBodiesRadial, MeltBodyAt, CutBody (ghost handling), BluntBody (bite centring), VoxelsToParticles.
  - AdoptBody, ReleaseBody, StrapBody / CarryStrap / DriveStraps, MakeGhost / MakeOwned, ApplyBodyAnnounce / BuildAnnounce / ApplyBodyHandoff.
  - Save path: WriteBodyRecord, SaveState, ReadBodyRecord, RestoreBody, LoadState, SaveBodyGroup, LoadBodyGroup; OnMaterialsReloaded.
- mob.cpp:
  - LimbFloatDensity and FloatLimpLimbs.
  - The DetachLimb adopt path (dead=true, severedBody), ReleaseRigToDebris and the EmitCarvedFragment tail.
  - The carrion actor scan (corpses + loose pieces + `bleeding`), BiteHit's centring, LimbVoxelsToParticles, OnMaterialsReloaded.
- melee.cpp loose bite radius; session.cpp ExplosionHitsBodies and PhaseK laser; sim_mutate.wgsl cell-op IF_AIR handling; materials.h fluid lift pack.
- Data: tuning.json gore/ragdoll/grenade/debris values; materials.json densities (human tissue, bone, cloth, undercloth).
- persist.cpp DBRS registration; net/debrissync.h wire records.
- The corpse-float severed-arm diff, the loose-carrion gate, and the debris-float gate.

**Checked and not reported:**
- Rig and debris densities agree for the same piece: same lattice cell volumes, same densityOf_ source, worn shells and strapped followers each folded in once.
- Lift is 15 for every body material. Empty or air-only pieces give 1,000, and FloatingDensity clamps at 1, so no division by zero.
- Every sever path sets `dead`: fragments inherit it (3711, 6306), and DetachLimb, ReleaseRigToDebris and gobbets all pass true.
- Bleeding is bounded: BleedBodies closes paid wounds, and drips are rationed by bleedOpsPerTick.
- Ghost gore is sunk by the melee and pierce paths.
- Wound state staying with the old owner across a handoff is a documented choice (debris.cpp:4323-4325).
- The death photograph is render-only: DeathBody::Follow takes const Physics&, bricks are cloned and freed on respawn, and severedBody resets because Spawn reassigns limbs_.

**Not audited:**
- Jolt's actual sleep thresholds for floaters. The finding 4 cycle assumes floaters do fall asleep, as the corpse rig does; if a debris floater never sleeps, the cost is FloatBodies every tick instead.
- ManageTerrain cost on each wake.
- Mob::Sever and TickSeveredHolds, BurnFleshBodies and PulpTick rewrite rules, ai::Perceive scentRange/scentPull internals, RemoteExplosionsHitOwnBodies, MOBS v6 dead-record internals, and the autopsy peeler.
- Items settling into the grid on dry land: noticed in passing, outside scope, not traced.

