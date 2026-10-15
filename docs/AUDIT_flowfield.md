# Audit: the terrain flow field and the river current (c524834, 3637c2f)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| FF-1 | HIGH | A treading player is sampled ~5 cells above the flow field's surf+1.5 cutoff, so rivers never carry it | `src/game/player.cpp:1401` | small | no | fixed, gates pending `7e4659f` |
| FF-2 | MED | Current tracers never spawn on rivers/lakes/sea: wind_streak respawn gated on currentPrimCount | `assets/shaders/wind_streak.wgsl:287` | trivial | no | fixed, gates pending `a39aaa4` |

## Findings

### FF-1 (HIGH) A treading player is sampled ~5 cells above the flow field's surf+1.5 cutoff, so rivers never carry it

**Status:** FIXED on branch `audit-1014` in `7e4659f`. Confirmed: Player::Update sampled CurrentAtCpuSim at the figure-box centre, which sits about 5 cells above the top water cell for a treading body. WaterFlowField::Sample returns zero above surf+1.5, so rivers, lakes and the sea never carried a floating player. The probe is now the middle of the submerged span (feet to min(head, waterSurfaceY)) whenever a waterline was found. That point lies in the water like the NPC's mid-body sample. A fully submerged swimmer's probe is still its centre.

**Where:** src/game/player.cpp:1401 (with src/sim/flowfield.cpp:714)

**What:** The player samples the current at `pos`, the centre of its 17-cell figure box. A floating or treading player settles with that centre about 5.25 cells above the top water cell. `WaterFlowField::Sample` returns zero for any point higher than `surf + 1.5`. So the terrain flow field (rivers, lakes, sea) never carries a surface-floating player, while NPC swimmers, sampled at mid-body inside the water, drift. 3637c2f's promise that letting go of the keys drifts at the river's speed does not hold for the player.

**Trace:**
- src/game/player.cpp:928-951: submersion = wet count of 5 samples at pos.y + {-8.5,-4.25,0,+4.25,+8.5}
- src/game/player.cpp:1312-1331: gravity scaled by 1 - 0.75*s, applied unless swimming (s >= 0.75, player.cpp:291)
- src/game/player.cpp:1504-1505: tread thrust 175*s vox/s^2 while buoyed && !swimming; net +1.3 at s=0.4, -48.4 at s=0.2, so the body rests at pos.y ~ W+5.25
- src/game/player.cpp:1400-1401: carried = inLiquid && (swimming || (afloat && !rising)), which is true; CurrentAtCpuSim(pos)
- src/sim/currentprim.cpp:368: WaterFlow().Sample(posVox)
- src/sim/flowfield.cpp:714: p.y > surf + 1.5 returns {0,0,0}; surf = top water cell (flowfield.cpp:270)
- src/game/mob_afloat.cpp:100: NPCs sample at origin.y + 0.5H, which is inside the column, so they are carried

**Failure scenario:** On the default map the player jumps into a river and lets go of the keys. currentVel stays 0 and the player holds position, while an NPC treading beside them drifts downstream and the surface streaks show moving water.

**Suggested fix:** Sample the player's current at a point inside the water, e.g. `probe.y = min(pos.y, waterSurfaceY - 1)` or the midpoint of the submerged span, to match the NPC's mid-body sample. Add a flow-field arm (WaterFlow().UpdateAt over a synthetic river) to the player-current gate. Both drift gates today use slab-less primitives and never reach flowfield.cpp:714.

**Verifier's note:** I re-derived the equilibrium from the shipped tuning.json values (gravity 9.81, liquidGravityScale 0.25, swimUp 17.5) and confirmed that WaterColumn.water is the top water cell. Even if surf meant the first air cell, the cutoff would be W+2.5, still well below W+5.25. A diver (s >= 0.75, centre near or under the surface) is carried, so only the float/tread case is broken.

### FF-2 (MED) Current tracers never spawn on rivers/lakes/sea: wind_streak respawn gated on currentPrimCount

**Status:** FIXED on branch `audit-1014` in `a39aaa4`. Confirmed: the current-tracer respawn returned whenever currentPrimCount==0, but since c524834 terrain water lives in flowGrid, which wind_streak does bind (binding 35, so FLOW_BOUND is true). The gate now uses the same test as currentConvergeAt: (currentPrimCount==0 && (!FLOW_BOUND || flowWord(2u)==0u)). The shader header comment and the tuning.h currentStreak comment are updated (comment-only change to tuning.h). check_shaders passes. Render-only.

**Where:** assets/shaders/wind_streak.wgsl:287

**What:** The current-tracer respawn returns early when `R.currentPrimCount == 0u`. Since c524834, rivers, lakes and the sea live in `flowGrid`, not on the primitive list, which now holds only live drains. So in normal play no current tracer ever spawns on terrain water, and while a drain is live anywhere in the window they spawn on every river near the camera.

**Trace:**
- src/test/support.cpp:1081-1083: rp.currentPrimCount = CurrentPrims().Count()
- src/sim/currentprim.cpp:231-236: the list carries events (drains) only; the stream arm is gone
- assets/shaders/wind_streak.wgsl:287: `if (R.currentRenderOn == 0u || R.currentPrimCount == 0u) { return; }`
- assets/shaders/wind_streak.wgsl:320 and common.wgsl:4468-4470: currentAt = currentPrimAt + flowGridAt, so the spawn would see the flow field if it got past line 287
- assets/shaders/common.wgsl:4483: currentConvergeAt already uses the flow-aware gate

**Failure scenario:** Stand on a default-map river bank with no drain active: no current tracers appear (they did before c524834). Dig into a lake to start a drain vortex and tracers appear on every nearby river until it decays.

**Suggested fix:** `if (R.currentRenderOn == 0u || (R.currentPrimCount == 0u && (!FLOW_BOUND || flowWord(2u) == 0u))) { return; }`, the same form as common.wgsl:4483. Update the comments at wind_streak.wgsl:42-48 and tuning.h:4106-4113.

**Verifier's note:** I confirmed that currentAt includes flowGridAt, so a one-line gate change is enough. This is render-only, with no hash or determinism impact. The claim is accurate.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | N/A | Derived data, a pure function of (map, seed, sim.currentStreamScale), rebuilt on demand (flowfield.cpp:594-607). Neither entities.sve nor chunk snapshots need it. The key misses the biome set, so F7 can leave it stale (finding 3). |
| deterministic | OK | Composition waits on a pending block or solves it synchronously (flowfield.cpp:523-541), so thread timing cannot change a word. Values are world-anchored inside the grid (rivers J). Consumers run on the tick thread in fixed phase order (session.cpp:851, 2935, 5936). |
| sleeps when idle | OK | Holding still does only Harvest plus a 25-entry Prefetch scan per tick: no solve and no upload (flowfield.cpp:613-618, 739-743). Solves are bounded by travel, with at most 2 in flight (flowfield.cpp:561-576). |
| cpu mirror | OK | Sample reads the same quantised words with the same bilinear weights, gate and profile as common.wgsl flowGridAt (flowfield.cpp:694-719 vs common.wgsl:4427-4459). The player, however, queries it at its box centre, above the gate (finding 1). |
| mutation queue | N/A | Writes no voxel and no sim buffer. flowGrid is render-only (pass_table.h FlowGrid; renderBGL 48 / shadowBGL 35), and the bodies it moves emit their ops through the normal queue paths. |

## Coverage (as reported by the finder)

**flow field.** I read all of src/sim/flowfield.{h,cpp} and both landing diffs (c524834, 3637c2f). I then traced every consumer of WaterFlow(), and only two reach gameplay. Player::Update (player.cpp:1401, run from PhaseA at session.cpp:851) and NPC walkers afloat (mob_afloat.cpp:100 in DriveAfloat; PhaseH mobs.PreTick at session.cpp:2935; the pose at mob.cpp:9244). Debris, corpses, items, fish (mob_swim) and remote-player ghosts do not read the current. The GPU sim kernels use currentAtQ, which is primitives only. Render reads go through currentAt and flowGridAt in raymarch, wind_streak and debug_current. These consumers reach hashed state only through body positions, which feed ops. Under M9 the owner machine simulates its own mobs and player.

Determinism. BlockAt either waits on a pending future or solves synchronously (flowfield.cpp:523-541), so async timing never changes a composed word. Blocks are world-anchored. The grid covers [base-8, base+520), and NPCs outside the window stop being afloat because CellCache returns Solid there, so no consumer ever sees a window-dependent value. Stream::Update runs per tick (session.cpp:1080). I checked thread safety: the CPU terrain mirror (world.cpp) has no function-local statics or caches, WorldMapData has no mutable members, and the Net is immutable after BuildNet (every Phi0 and C is resolved). I also checked every writer of Slot(): the LoadWorldMap temporary install comes after the quiesce at 2383, and SetCurrentWorldMap quiesces at 3031. Other threads I checked: the farFuture build only compiles pipelines; map_ui threads only read and are joined; the mob ParallelFor joins; FTZ is set only on the audio thread.

Lifetime and bounds. Harvest, Quiesce and eviction iterators are safe. pending_ is capped at 2. The prefetch ring stays within ±2 blocks of the centre while eviction is at 3, so there is no thrash. Tent indices stay in range for 16-aligned bases. There is no NaN or division hazard (depth >= 1, Tr floors at 3, sk > 0, tUp > 0). Units are cells/s throughout. The sign of the source and sink terms is correct. I followed the BC logic for multi-piece rivers, tributaries, the fallback pin and sea classification. A python SOR replay on a synthetic straight channel with a narrows converged to within ~4% at 260 sweeps. Render side: I checked flowGridAt against the Sample transcription, the FLOW_BOUND strip in resources.cpp, the BGL entries, the version-gated upload, and flowFrame, flowStreakSlope, flowBoils and flowFoamLines for range and divide-by-zero. A python replay of the player's float dynamics produced finding 1. I also surveyed routing in the shipped maps: river_eldwater leaves the large lake_tarn_9, and no small spring lake lacks an anchor node.

Not covered: barrier generation in vk_record for FlowGrid in the raymarch draw (it relies on the pre-existing 'renderBGL view is not a row' pattern); far-field and horizon water; solve quality on the real meandering default-map rivers and junctions (only the synthetic test); cross-machine reproducibility of std::pow and std::log in SolveBlock (CPU body physics is float everywhere and M9 mobs have a single owner); ghost-mob pose parity; Mob::SaveOne not carrying swimming_ or swimVel_ (pre-existing, outside these commits); mob_swim.cpp; the F4 overlay (known); tuner.html and flow_view.py. Smaller things seen but not reported: swimmers read a stale grid for one tick after a window jump (deterministic, negligible); block solves still run on maps with no water or with scale 0 (cost scales with travel, not per tick).

