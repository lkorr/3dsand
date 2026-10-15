# Audit index: the 2026-10-14 overnight sweep

Snapshot `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`). Static only: no builds, no runs.

Method: 18 finder agents (11 per-system audits of the last two weeks' landings, 2 guideline-5 sweeps, 5 determinism sweeps). HIGH and MED findings went to verifier agents told to refute them (batched per finder). Of 87 findings reported: **47 CONFIRMED**, 7 PLAUSIBLE, 0 REFUTED, 33 LOW not verified. 8 duplicate(s) across sweeps were merged into their canonical entry.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Documents

| Doc | HIGH | MED | LOW | Confirmed |
|---|---|---|---|---|
| [AUDIT_flowfield.md](AUDIT_flowfield.md) | 1 | 1 | 0 | 2 |
| [AUDIT_draft_jets.md](AUDIT_draft_jets.md) | 1 | 2 | 0 | 3 |
| [AUDIT_naga.md](AUDIT_naga.md) | 0 | 2 | 0 | 2 |
| [AUDIT_shields.md](AUDIT_shields.md) | 1 | 2 | 0 | 3 |
| [AUDIT_archery.md](AUDIT_archery.md) | 2 | 3 | 0 | 5 |
| [AUDIT_projectiles.md](AUDIT_projectiles.md) | 0 | 2 | 0 | 2 |
| [AUDIT_dungeons.md](AUDIT_dungeons.md) | 3 | 2 | 0 | 5 |
| [AUDIT_venom_necrosis.md](AUDIT_venom_necrosis.md) | 1 | 0 | 0 | 1 |
| [AUDIT_scar_heal.md](AUDIT_scar_heal.md) | 2 | 2 | 0 | 4 |
| [AUDIT_severed_buoyancy.md](AUDIT_severed_buoyancy.md) | 2 | 2 | 0 | 4 |
| [AUDIT_done_sweep.md](AUDIT_done_sweep.md) | 0 | 0 | 0 | 0 |
| [AUDIT_determinism.md](AUDIT_determinism.md) | 2 | 7 | 0 | 9 |

## Read these first: every confirmed HIGH

- **FF-1** (AUDIT_flowfield.md) A treading player is sampled ~5 cells above the flow field's surf+1.5 cutoff, so rivers never carry it (`src/game/player.cpp:1401`)
- **DJ-1** (AUDIT_draft_jets.md) Draft box origin comes from each peer's local player and is not on the wire, so shared chunks diverge (`src/test/support.cpp:1895`): owner call
- **SH-1** (AUDIT_shields.md) Load/unpark/handoff restore hp and carved lattice but not the charged baselines; next carve re-bills old loss (`src/game/mob.cpp:31471`)
- **AR-1** (AUDIT_archery.md) An arrow that crosses the window face sticks there as a floating kinematic item instead of expiring (`src/game/arrows.cpp:468`)
- **AR-2** (AUDIT_archery.md) NPC archers abort every draw at a peer's player: TargetNow cannot resolve ghost actor ids (`src/game/mob_archery.cpp:256`)
- **DG-1** (AUDIT_dungeons.md) Dungeon spawner places creatures in solid rock; Tarn Cairn crypt_dead spawns slot 1 inside host rock (`src/world/refs_dungeon.cpp:1011`)
- **DG-2** (AUDIT_dungeons.md) Spawner slots resolve by RefId, so a slot's corpse and its replacement collide (`src/world/refs_dungeon.cpp:850`)
- **DG-3** (AUDIT_dungeons.md) CPU flight probe treats every unfetched underground cell as solid, so arrows and bolts hit phantom walls in dungeons (`src/game/flightprobe.h:44`): owner call
- **VN-1** (AUDIT_venom_necrosis.md) Venom necrosis never settles the organ/weight delta it causes; the next unrelated carve bills it (`src/game/mob.cpp:22342`): owner call
- **SC-1** (AUDIT_scar_heal.md) Machine races (android, automaton) regrow plating, circuitry and clockwork as pink `scar` flesh (`src/game/mob.cpp:16771`)
- **SC-2** (AUDIT_scar_heal.md) NPC amputee's open stump is lost on save/load, park/unpark and handoff, then fully regenerates (`src/game/mob.cpp:31512`)
- **SB-1** (AUDIT_severed_buoyancy.md) DBRS save drops Body::dead: reloaded severed limbs float as never-alive, lose carrion/coat (`src/phys/debris.cpp:8201`)
- **SB-2** (AUDIT_severed_buoyancy.md) Settle-back of a body resting on a lake bed: IF_AIR ops refused by water, body deleted (`src/phys/debris.cpp:2991`)
- **DET-1** (AUDIT_determinism.md) CA neighbour rewrites flagSupportLoss(n): reading n+d (distance 2) races a same-colour writer (`assets/shaders/common.wgsl:7720`)
- **DET-2** (AUDIT_determinism.md) Sparse workgroup's gas-leave tail rebuilds the leaver's chunk from a stale list slot beside a ticket (`assets/shaders/sim_step.wgsl:5967`)

## Confirmed, needing an owner decision

- **DJ-1** (HIGH) [DECIDED 2026-10-14, FIXED `b33d2ab`] Draft box origin comes from each peer's local player and is not on the wire, so shared chunks diverge
- **DG-3** (HIGH) CPU flight probe treats every unfetched underground cell as solid, so arrows and bolts hit phantom walls in dungeons
- **VN-1** (HIGH) [DECIDED 2026-10-14, FIXED `420230c`] Venom necrosis never settles the organ/weight delta it causes; the next unrelated carve bills it
- **SC-4** (MED) Piece cut off below the sever fraction (fingers, ear, nose) regrows in place as boneless scar
- **DET-3** (MED) Storm gust front and downbursts are placed at the window centre: peers diverge, bursts jump on shift
- **DET-4** (MED) MPM particles frozen out of residency never die, so the seam and CA can never prove the world idle
- **DET-7** (MED) Elec pond cap and purge are window-wide aggregates; shared chunk charge differs across peers

## Unconfirmed leads (PLAUSIBLE: the code is wrong as claimed, but the verifier could not prove reachability or impact; NOT in the per-system docs)

- (shields, MED) Block cancelled by an earlier body contact: later sub-steps cut the body behind the raised shield (`src/game/melee.cpp:1844`)
- (archery, MED) Arrow exiting a body past its tick segment restarts inside it; can be charged flat hp twice (`src/game/arrows.cpp:604`)
- (dungeons (entities), MED) Pulse lever saved or deactivated mid-pulse stays ON forever (deadline not persisted) (`src/world/refs_dungeon.cpp:1102`)
- (dungeons (entities), MED) Ref and dungeon entity state has no multiplayer authority; each machine ticks its own (`src/game/session.cpp:6290`)
- (det: MPM fluid, MED) Chunk-sync's quiet rule ignores live MPM particles, contradicting chunksync.h's no-in-flight-matter argument (`src/net/chunksync.h:84-91`)
- (G5: idle / mirror / queue, LOW) Dart wall test reads Unknown (outside the 3x3x3 mirror) as open, so a bolt can hit through unseen rock (`src/world/refs_dungeon.cpp:532`)

## LOW findings, NOT verified (budget): raw finder claims, treat as leads

**AUDIT_flowfield.md**

- (flow field) Flow-field cache is keyed only on map contentHash, so an F7 environment regen keeps stale blocks (`src/sim/flowfield.cpp:597`)
- (flow field) At exit, a background flow solve can read the world-map static after it is destroyed (`src/sim/worldmap.cpp:2383`)

**AUDIT_naga.md**

- (naga) The path-laid trail ignores tail1's own yaw: the walk/run carriage wags the whole tail +-5-7 deg (`assets/editor/serpent.js:796`)
- (naga) naga+zombie regains a gait from the zombie effect, so DrapeTail never runs on its tail (`assets/mobs/naga.json:5356`)
- (naga) Trail path and drape are dropped in one tick on water entry or handoff; MoveKinematicBody sweeps the tail (`src/game/pose.cpp:1653`)

**AUDIT_shields.md**

- (shields) Gear re-announce re-equips a ghost's held shield pristine, wiping the attacker's own chips (`src/game/mob.cpp:30872`)
- (shields) Stale peer-player stream leaves ghostShieldUp_ latched: a ghost's lowered shield still blocks (`src/game/remoteplayer.cpp:351`)
- (shields) Dual shields: the unraised second shield guards, and blocks read the other shield's spec (`src/game/mob_shield.cpp:23`)

**AUDIT_archery.md**

- (archery) An arrow embedded in a strapped shell or arrow cannot strap and hangs where it hit (`src/game/arrows.cpp:526-547`)
- (archery) Player bow skips the NPC draw rules: shoots one-armed, ragdolled or mid-swing (`src/game/session.cpp:6123-6134`)

**AUDIT_projectiles.md**

- (projectiles) Spell strike draw key comes from p.seq, a never-saved session counter documented as renderer-only (`src/game/spell.cpp:4983`)

**AUDIT_dungeons.md**

- (dungeons (world)) A dungeon the world loader refuses is not carved, but its entities still spawn inside the solid rock (`src/sim/worldmap.cpp:1104`)
- (dungeons (world)) shell.bin's tube/scatter range check wraps in uint32, so a bad reference passes and is read out of bounds at boot (`src/sim/dungeon.cpp:407`)
- (dungeons (entities)) Raised spikes and a half-done collapse are not in the trap delta; a save or window leave freezes them (`src/world/refs_dungeon.cpp:420-422`)
- (dungeons (entities)) A collapse_floor with once:false re-runs its collapse on every activation and load (`src/world/refs_dungeon.cpp:1214-1218`)
- (dungeons (entities)) collapse_floor's 65,536-cell scan cap silently skips the top of the shipped Shardwell roof_slab (`src/world/refs_dungeon.cpp:36`)
- (dungeons (entities)) Gate build emits an unbudgeted CellOp burst; a valid large gate is truncated at submit yet marked built (`src/world/refs_dungeon.cpp:359`)
- (dungeons (entities)) F7 regen (RefStore::LoadMap) leaves the old world's gates, traps and spawners ticking against the new world (`src/world/refs.cpp:542-557`)
- (dungeons (entities)) Ruins crypt 'guardians' spawner suggestion writes radius in voxels and a nonexistent 'guard' profile (`assets/editor/dungeon/kit_ruins.js:780`)

**AUDIT_venom_necrosis.md**

- (venom necrosis) A bite's stale woundWas entry reverts a necrotic cell to healthy flesh after a later cut (`src/game/mob.cpp:27015`)

**AUDIT_scar_heal.md**

- (scar heal) Ghosts never heal and living bodies have no shape stream, so peers keep healed wounds (`src/game/mob.cpp:21577`)
- (scar heal) Healthy limbs re-walk their whole recipe whenever another limb's burn fraction moves (`src/game/mob.cpp:16551`)
- (scar heal) With gore.stumpBleedsOpen off, a stump that stopped bleeding still blocks all regen forever (`src/game/mob.cpp:27955`)

**AUDIT_severed_buoyancy.md**

- (severed buoyancy) Any destroyed scrap pays a full amputation's blood, whatever the cause or size (`src/phys/debris.cpp:5011`)
- (severed buoyancy) Cached floating densities survive a material hot reload (R) (`src/phys/debris.cpp:2709`)

**AUDIT_done_sweep.md**

- (G5: idle / mirror / queue) Spike-trap retraction list is neither saved nor honoured on WindowLeft, so spikes can stay up forever (`src/world/refs_dungeon.cpp:420-421`)

**AUDIT_determinism.md**

- (det: sim_step B) CA neighbour rewrites raise support flags from a distance-2 read another thread may write (`assets/shaders/sim_step.wgsl:3367`)
- (det: MPM fluid) settleJudge dispatches 32,768 threads but NUM_SLOTS is 34,816, so MPM in a ticket box can never settle (`src/sim/pass_table.def:2304`)
- (det: MPM fluid) The forced settle walk reaches past the materialized ring, so its blocked/open verdict depends on timing-dependent page state (`assets/shaders/sim_fluid_seam.wgsl:37-46`)
- (det: MPM fluid) stainApply's consume calls flagSupportLoss on neighbours the same dispatch may also consume (`assets/shaders/sim_fluid_seam.wgsl:1919-1924`)
- (det: MPM fluid) MPM outcomes in a shared chunk depend on everything else in each peer's window (`assets/shaders/sim_fluid.wgsl:1736-1738`)
- (det: MPM fluid) The foam field (grid word 7) persists by block index, which alloc renumbers every tick, so foam jumps between chunks (`assets/shaders/sim_fluid.wgsl:830-853`)
- (det: CPU tick + op stream) Op-record header stamps no tuning/reaction identity; a replay under different knobs diverges unattributed (`src/sim/oprecord.h:313-326`)


## Not covered this run

- Determinism re-sweeps of sim_waterbody, sim_solute, sim_gas, sim_particle, sim_mutate / sim_explode / hash, and worldgen (covered by the 2026-10-02 audit; dropped for budget).
- The guideline-5 sweep ran as two merged agents at reduced effort, not one per property per domain.

## Fix wave: branch `audit-1014` (NOT merged to main)

- 33 of the 40 confirmed findings needed no owner decision, and all 33 were fixed, one commit per finding: PJ-1 as a stop-gap, the other 32 in full. The branch is in the locked worktree `.claude/worktrees/audit-1014` and is based on `20aa002`. Two more commits sit on top: `7018a6e` (DESIGN.md follows SB-1, DJ-2, PJ-1 and DET-8) and `2b9bc9c` (the build fix for DET-9: `DebrisSystem::ChunkOwned` made public). Each finding's Status cell names its commit.
- Each fix agent re-checked its finding in code before editing. None of the 33 turned out to be wrong.
- **Built:** yes, with one build. The only compile error was DET-9's, fixed in `2b9bc9c`.
- **Verified: NO.** The single `--verify` boot (49 gates) was killed by Claude Code because the machine ran critically low on memory, after 1 of 49 gates (`tick-input`: PASS). Its orphaned exe was killed by PID. The determinism twice-run, ops-replay and all the behaviour gates have NOT run.
- **The hash is expected to move.** DET-1, DET-2, DET-5, DET-6, DJ-2 and SB-2 all change hashed state. Once the twice-run agrees: `--selftest --gate determinism --rebaseline`. The smoke probe tables need re-pinning when the branch lands.
- **Format bumps:**
  - `DBRS` v5 -> v6 (SB-1). Older saves still load.
  - `kTickInputVersion` 9 -> 10 (AR-3). Older op recordings are refused.
  - `kOpsWireVersion` 3 (DET-8). Peers must run the same build.
- **To verify:** one boot, from the worktree. The fresh worktree's `shader_cache/` is empty, so copying main's first avoids a cold compile of every shader, which is likely what made the boot slow and memory-hungry. This is optional.

```bash
cp -r "../../../shader_cache" . 2>/dev/null   # optional: run from .claude/worktrees/audit-1014
bash scripts/run.sh ./build/Release/sandvox.exe --verify determinism,ops-replay,tick-input,ops-exchange,mob-save-delta,mob-handoff,mob-park,save-entities,save-avatar-body,save-load,save-items,item-park,scar-heal,bleed-out,stump-walk,heal-wound,robot-races,shield,ragdoll,net-shield,net-player-melee,naga,floaters,settle-back,tree-fell,debris-float,severed-hand,corpse-bleed,dungeon-gen,dungeon-entities,loot,worldmap,arrow,archer,pierce,spell-pierce,demon-cast,current,player-current,drafts,wind-prim,gas-leave,gas-leave-overflow,ticket-edge,support-flag-post,fluid-det,fluid-excite,liquid-seam,streaming
```

The DET-5 fix assumes that `Queue::WriteBuffer` drains at the head of the next command buffer. A short `--vk-validation` run of `streaming,ticket-edge,fluid-det,liquid-seam` confirms that assumption raises no sync hazard.

### Follow-ups the fix agents found (not done)

1. **Same defect class as SH-3:** a natural weapon's edge (`nw->edgeFrom/edgeTo`) does not follow a carve rebase either. `MobDef` is shared, so the fix needs a per-mob offset.
2. **PJ-1's full fix:** apply a splash or lodged-lump coat on the owner's machine through a `MeleeHit` variant carrying material, amount and radius. It needs `mob.cpp` and `melee.*`.
3. **DET-9 has a residual gap.** The GPU's 16 reactFx winner slots can differ between machines, so a firing that lost its slot here but won it on the peer is now blasted by nobody. The full fix filters by authority before the slot selection, which is a GPU change.
4. **The single-producer audit is inert online.** `net::Hook()` / `SetAuthorityHook` is never installed, including in `main.cpp`, so `SubmitTick`'s single-producer check does nothing in real connected games.
5. **DJ-3's cleaner home is `Simulation::EncodeLoadReset`.** The calls currently sit in `worldio.cpp` and the `main.cpp` teleport. The net late-join path needs the `main.cpp` call either way.
6. **AR-4 leaves restored arrows aside.** Stuck arrows restored by a load are not re-entered into `stuck_`, so they sit outside the 48-arrow cap. They also come back non-kinematic.
7. **Owner call:** SC-1 leaves hp regeneration on for machine races. Should machines regain hp?
8. **Behaviour changes beyond the bugs** (watch these gates):
   - SC-3: a partly-air layer now costs its fraction, as in `PierceChannel`.
   - DET-2: dense window-face chunks beside a ticket box now get a share of the gas-leave edge budget (`gas-leave`, `ticket-edge`).
   - DG-1: a spawn into a closed spot can take a few extra fetch rounds.
   - SB-2: settle-back holds while the mirror says a target cell is not air (`settle-back`, `tree-fell`).
9. **DET-1 added a third copy of the support-loss rule** (`flagSupportLossNbr` in `sim_step.wgsl`). The header of `sim_mutate.wgsl`'s `flagSupportLossBrush` could list it.
10. **PJ-2 uses the def-level `skinScale`.** A per-limb floor needs a public accessor in `mob.h`.

### Caveat on the confirmations

Mid-run, the verification pass was re-planned to save tokens: batched, effort `high`. It refuted **none** of 54 HIGH/MED claims, and a refute rate of zero is unusual. The fix agents' re-checks, and the gates once they run, are the stronger evidence.

## Update (later on 2026-10-14)

- The owner decided DJ-1 and VN-1, and both are fixed on the branch: `b33d2ab` and `420230c` (see their entries).
- Main's newer commits were merged into the branch (`c2c00f9`), with no textual conflicts. The SC-1 machine-race guard still covers main's per-cell scar code, because both paths call the same `isTissue` predicate.
- These docs are committed on the branch.
- **Still pending: build, verify, rebaseline, then fast-forward main.** The build of the merged branch was reaped by Claude Code under low system memory, so the merged tree is NOT built and NOT verified, and main has NOT been updated.
