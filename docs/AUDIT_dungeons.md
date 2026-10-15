# Audit: dungeons: shell, worldgen and entities (dd56aa1..8a46ed0 and follow-ups)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. Two finder agents (the world side and the gameplay entities) read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| DG-1 | HIGH | Dungeon spawner places creatures in solid rock; Tarn Cairn crypt_dead spawns slot 1 inside host rock | `src/world/refs_dungeon.cpp:1011` | small | no | fixed, gates pending `cfe0e2d` |
| DG-2 | HIGH | Spawner slots resolve by RefId, so a slot's corpse and its replacement collide | `src/world/refs_dungeon.cpp:850` | small | no | fixed, gates pending `556fd68` |
| DG-3 | HIGH | CPU flight probe treats every unfetched underground cell as solid, so arrows and bolts hit phantom walls in dungeons | `src/game/flightprobe.h:44` | medium | yes | open |
| DG-4 | MED | Container props.lock is ignored: the Shardwell and Teeth Caves keyed chests open without keys | `src/world/refs_doors.cpp:1033` | medium | no | fixed, gates pending `862ee11` |
| DG-5 | MED | Two dungeons sharing a 64^3 index cell over the list caps make PackWorldMap fail, refusing boot | `src/sim/worldmap.cpp:2991` | small | no | fixed, gates pending `4613d00` |

## Findings

### DG-1 (HIGH) Dungeon spawner places creatures in solid rock; Tarn Cairn crypt_dead spawns slot 1 inside host rock

**Status:** FIXED on branch `audit-1014` in `cfe0e2d`. The spawner drew a point in its radius disc at its own y and only waited for the fetch cache to be fresh, without reading the cached words, so crypt_dead put zombies into host rock. Now SpawnInto tests each draw against the cached words. The body column above the foot cell must be clear of solid and powder, and there must be support at or below the foot. A closed draw is redrawn with PlanSpawn's new `attempt` parameter (salt 0x5A2 + 0x100*k), up to kSpawnTries = 8 draws, then falls back to the spawner's own pos. Attempt 0 is unchanged, so the dungeon-entities 'planned spot' check still holds when its spot is open.

**Where:** src/world/refs_dungeon.cpp:1011 (PlanSpawn), src/world/refs_dungeon.cpp:788-807 (SpawnInto)

**What:** A spawner picks its spawn point as a hashed (dx,dz) anywhere inside `radius`, at the spawner's own y. Nothing checks that the cell is open. SpawnInto only waits until the fetch cache is fresh over the column and never reads the cached words. The rig is built wherever it is told, and the mob ground probe climbs at most 1.2 m out of matter, so a creature placed in rock stays there.

**Trace:**
- src/world/refs_dungeon.cpp:1011-1022: R = radius*10; dx,dz hashed on (RefHash(id), serial, 0x5A2); foot = pos + (dx, 0, dz)
- src/world/refs_dungeon.cpp:788-799: the 'ground rule' comment, but the code only checks CacheFresh
- src/world/refs_dungeon.cpp:805-807: mobs.Spawn(def, origin from plan.foot)
- src/game/mob.cpp:4982-4988: kEscapeUp = 1.2 m; the comment says an entombed creature stays entombed
- assets/dungeons/heather_cairn/refs.json:85: crypt_dead at x -231, radius 3.5 (35 cells), count 3
- assets/dungeons/heather_cairn/shell.json:35: the crypt room box spans x -251..-220

**Failure scenario:** Default map, Tarn Cairn (yaw 180). Entering gallery_threshold triggers crypt_dead. Serial 2 lands at local (-206,-143,-38), about 14 cells east of the crypt's east wall, where no shell prim exists: the zombie is entombed in rock. Serial 3 (-213,-143,-55) lands at the edge of a wall niche. Every visit the crypt fight is short at least one zombie.

**Suggested fix:** After CacheFresh passes, test the footprint at plan.foot against the cached words: air for the body, support underneath. On failure, draw the next candidate deterministically (Hash3(key, serial, 0x5A2+k) for k < 8), then fall back to r.pos. Also trim crypt_dead's radius and have the dungeon editor warn when a spawner's radius leaves its room.

**Verifier's note:** I recomputed the hash myself; it matches the finder's offsets. I scanned shell.json for prims within 4 cells of each landing point. Serial 2 is clearly in host rock. Serial 3 is marginal (inside the skin box, at the edge of a niche ellipsoid). I did not check whether entombed mobs suffocate; either way the spawn is wrong.

### DG-2 (HIGH) Spawner slots resolve by RefId, so a slot's corpse and its replacement collide

**Status:** FIXED on branch `audit-1014` in `556fd68`. A corpse kept its slot's derived RefId. FindMobByRef (first match, dead or alive) and LoadOne's one-ref-one-body refusal therefore confused the corpse with its replacement. Storing the mob id would not fix this, because Id() is re-issued on every load and unpark. Instead, TickSpawner renames any dead body under the slot id to a unique "<sid>#dead<serial>" when the slot books it dead. It also does this when such a body turns up in an empty or dead slot, which covers older saves and a corpse that unparks first.

**Where:** src/world/refs_dungeon.cpp:850 (TickSpawner's FindMobByRef(sid))

**What:** A spawner slot finds its creature by RefId `<spawner>/s<i>`. A creature that dies keeps that RefId as a corpse, and the replacement gets the same id. FindMobByRef returns the first match in mobs_, dead or alive, and LoadOne refuses any record whose RefId is already taken. Once a corpse and its replacement coexist, one of them is wrong.

**Trace:**
- src/world/refs_dungeon.cpp:811: SetRefId(sid) on the new creature, with no check that sid is free
- src/game/mob.cpp:4400-4405: FindMobByRef returns the first match, alive or not
- src/world/refs_dungeon.cpp:850-856: if that match is dead, state = 2 (dead)
- src/world/refs_dungeon.cpp:1345-1347: onLeave activation resets dead slots to empty, so a replacement spawns
- src/game/persist.cpp:1070: parking keeps the unreleased dead
- src/game/mob.cpp:31564-31569: LoadOne drops a record whose RefId already has a mob

**Failure scenario:** Shipped onLeave spawners (crypt_dead etc.): kill a creature, leave the window (the corpse parks with RefId sid), then return. Activation empties the slot and a replacement spawns. Case 1: the corpse unparks first. The replacement is found behind it and booked dead on its first tick. It never runs its schedule (StepScheduled needs state 1), and on the next visit the slot spawns again, so more than `count` are alive. Case 2: the replacement spawns first. The corpse record is refused and dropped, along with its loot. A save holding both drops one on load the same way. A ticks:N spawner (none shipped) would also re-book every replacement as dead while the corpse lies there and spawn without bound up to the living cap.

**Suggested fix:** Store the slot creature's mob id in SpawnSlot and match on it. After a load or unpark, re-resolve it by RefId among living mobs only. Alternatively, rename the corpse's RefId when the slot is booked dead (e.g. `<sid>#dead<serial>`), so neither the spawner nor LoadOne's one-ref-one-body guard confuses the two.

**Verifier's note:** No shipped spawner uses respawn ticks:N, so the finder's scenario (a) is latent. Scenario (b) with onLeave is shipped content: corpses park and the refusal in LoadOne is unconditional on alive.

### DG-3 (HIGH) CPU flight probe treats every unfetched underground cell as solid, so arrows and bolts hit phantom walls in dungeons

**Where:** src/game/flightprobe.h:44 (tier-3 fallback of FlightSolidAt), used by src/game/arrows.cpp:468 and src/game/spell.cpp (solidAt).

**What:** When a flight leaves the CPU mirror (3x3x3 chunks, about +/-16-32 voxels) and the cell's chunk is not yet in the fetch cache, FlightSolidAt answers `y <= World::TerrainHeight(x,z)`. TerrainHeight has no dungeon or cave term. So every unfetched cell of an underground hall reads as solid. Prefetch reaches only 48 voxels ahead and lands at least 5 ticks later, so an arrow at about 16.7 vox/tick stops on a wall that is not there, a few metres from the shooter.

**Trace:**
- src/game/arrows.cpp:451 prefetch(f.pos, f.vel): requests chunks up to 48 voxels ahead, only where KindAt is Unknown
- src/sim/world.h:5118 kSnapshotLatency = 4, so a fetch lands at the earliest 5 ticks later
- src/game/arrows.cpp:463-474 sweeps the half-voxel sub-steps this tick and calls solidAt(next)
- src/game/flightprobe.h:31-32 the cell is outside the mirror, so Unknown
- src/game/flightprobe.h:34-42 World::Cached misses
- src/game/flightprobe.h:44 `return y <= World::TerrainHeight(...)` is true for a hall below ground
- src/sim/world.cpp:3195-3204 TerrainHeight = land/road/site/water only, with no dungeon or cave
- src/game/arrows.cpp:572ff the arrow becomes a stuck item at wallAt. Spell bolts set impact at `next`.

**Failure scenario:** The player is in a dungeon corridor under a hill, in a region not fetched in the last 600 ticks, and looses a full-draw arrow (drawSpeedMps 50) down a 30 m hall. On tick 2 the arrow is past the mirror. Its chunk is not cached, so the probe answers solid, and the arrow hangs stuck in mid-air about 2-3 m out. An explosive bolt detonates at the same spot, close to the caster. NPC archers deeper in the dungeon lose shots the same way. Natural caves behave identically.

**Suggested fix:** Give the miss path a better answer than the ground. Options: (a) consult the CPU dungeon twin (keep PackDungeons' block on the CPU and use TwinCarveAt / TwinCell over the analytic terrain word) inside a dungeon's world box; (b) treat a below-ground miss as unknown/passable, or hold the flight in place until the fetch lands, since the flight is deterministic per tick in both stores; (c) prefetch further ahead, proportional to speed x (1+kSnapshotLatency). Option (b) also covers caves.

**Owner call:** Which semantics an unknown underground cell should have for a CPU flight: twin-evaluated, passable, or wait-for-fetch. Option (a) fixes only dungeons. Options (b) and (c) fix caves too.

**Verifier's note:** I traced flightprobe.h, the arrow and spell prefetch and sweep, kSnapshotLatency, and TerrainHeight. I checked other RequestChunkFetch callers (refs_dungeon CacheFresh is prop-local, the mob caches are mob-local), and none keeps a corridor warm. Correction: the defect is not dungeon-only. The tier-3 ground contract is wrong for any enclosed underground space, including worldgen caves. Dungeons make it routine because they are built for underground combat.

### DG-4 (MED) Container props.lock is ignored: the Shardwell and Teeth Caves keyed chests open without keys

**Status:** FIXED on branch `audit-1014` in `862ee11`. Containers ignored the door's keyed props.lock, so the Shardwell and Teeth Caves chests opened without their keys. The container kind now validates props.lock as text. The prompt reads 'Locked <title>' and use is refused until KeyInKit finds the key. The unlock is remembered by the container's own delta, which only an open ever writes; the unlocking open writes one if the loot roll did not. So no delta format bump was needed.

**Where:** src/world/refs_doors.cpp:1033 (container chestLocked)

**What:** Containers honour only the boolean `props.locked`, which has no keys. The keyed `props.lock` rule exists for doors only. Two shipped dungeon chests carry `lock: iron_key` / `lock: old_key`, and the validator ignores the field, so those chests open for anyone and their keys unlock nothing.

**Trace:**
- assets/dungeons/shardwell/refs.json:87: assay_chest props {loot, title, lock: iron_key}
- assets/dungeons/teeth_caves/refs.json:78: smugglers_cache props {lock: old_key}
- src/world/refs_doors.cpp:1013-1030: container validate has no 'lock' check
- src/world/refs_doors.cpp:1033-1036: chestLocked reads props.locked only
- src/world/refs_doors.cpp:1041-1048: onUse runs ContainerRollLoot and sets openContainer
- src/world/refs_doors.cpp:595: only the door parses props.lock

**Failure scenario:** Default map, The Shardwell: use the assay chest without the iron key. It says 'Search assay chest', opens, and cave_cache rolls into the loot panel. The shift_key / den_key pickups have no lock to open.

**Suggested fix:** Give containers the door's keyed-lock rule. Validate props.lock as text. Show 'Locked <title>' and refuse the use until the user's kit holds the key (the door's key check). Remember the unlock in the container delta; that needs a delta format bump.

**Verifier's note:** I read both prop sets and the container kind. Neither container validate nor usePrompt nor onUse reads 'lock'. I did not re-check the dungeongen.js comment the finder cites.

### DG-5 (MED) Two dungeons sharing a 64^3 index cell over the list caps make PackWorldMap fail, refusing boot

**Status:** FIXED on branch `audit-1014` in `4613d00`. AnchorDungeon trial-packed each placement alone, but PackWorldMap packs all placed dungeons into one shared index. Two dungeons that each fit alone could overflow a cell together and refuse boot. AnchorDungeon now takes an optional table and trial-packs the placement together with every entry placed before it. In LoadWorldMap's table-order loop, the first overflowing entry is therefore unplaced with a load warning, which --mapcheck now also sees. ReplaceScratchDungeon re-checks the entries after the replaced one the same way. The PlacedOf() helper is now shared with PackWorldMap.

**Where:** src/sim/worldmap.cpp:2991 (PackWorldMap's union pack) vs src/sim/worldmap.cpp:935 (AnchorDungeon's single-dungeon trial pack).

**What:** The load-time trial pack checks each placement alone, so an over-cap index cell is a warning and that dungeon is unplaced. PackWorldMap then packs all placed dungeons into one shared 64^3 index. If two placements together put more than 64 prims or 32 props in one cell, PackDungeons returns false, PackWorldMap fails, and the game refuses to start. That contradicts DESIGN.md's 'load WARNING and the dungeon is not placed'.

**Trace:**
- src/sim/worldmap.cpp:2410-2415 each dungeon is anchored independently
- src/sim/worldmap.cpp:930-941 trial PackDungeons({pl}) on one placement passes
- src/sim/worldmap.cpp:2973-2991 every Placed() dungeon goes into one PackDungeons call
- src/sim/dungeon.cpp:1429-1435 a cell list over kDgListMax/kDgInstMax gives `return false`
- src/main.cpp:7291-7297 'failed to load -- refusing to start', return 1
- src/test/support.cpp:2793 an environment reload keeps the old tables
- src/main.cpp:6955 --mapcheck calls LoadWorldMap only, never PackWorldMap

**Failure scenario:** An author places a second dungeon (D9 multi-placement) close enough to the first that their inflated boxes share a dense index cell whose combined list exceeds 64 prims. Each placement passes AnchorDungeon, --mapcheck reports the map OK, and the map saves. The next game launch prints the index-cell error and exits 1. A running session's environment reload silently keeps the previous tables.

**Suggested fix:** In LoadWorldMap after anchoring, pack placements cumulatively in table order. Unplace (with a warning) the first placement whose addition overflows a cell, which is the same rule AnchorDungeon applies to one dungeon. Alternatively, make PackWorldMap drop that placement with a warning instead of failing. Have --mapcheck run the union pack so the tuner sees it before saving.

**Verifier's note:** I confirmed the trial-vs-union split and the fatal startup path. Correction: the voxserve Dungeon-tool preview (src/tools/voxregion.cpp:897 / :969) does pack the union and surfaces a failure as 'environment reload failed'. So the tuner can see the problem while previewing, but nothing blocks the save, and --mapcheck, hand edits and place_dungeon.mjs do not catch it. I did not re-derive the claim's 78-prim figure.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists (dungeons (world)) | OK | Worldgen input, regenerated from map.json plus the shell. The shell bytes are folded into the env stamp, the worldgen fingerprint and the net Hello envMap (biomes.cpp:514-519, worldio.cpp:501, protocol.h:139). Player damage persists as ordinary chunk snapshots. |
| deterministic (dungeons (world)) | OK | Integer-only evaluator whose twin is held token for token (check_invariants.py:3521). Packing is deterministic: std::map key order and an insertion-ordered hash (dungeon.cpp:1284-1495). Anchors come from integer BareGroundHeight (worldmap.cpp:919). No f32 on genChunk's dgCell path (worldgen.wgsl:6707). |
| sleeps when idle (dungeons (world)) | OK | No per-tick work: dgCell runs only in genChunk, cols and far for listed chunks (worldgen.wgsl:6685-6707). Matter is born at rest: firm shell (dungeon.cpp:2109), contained puddles (2060), rubble at the repose run (2005). Only the fixture's settle is gated (dungeon.maxActive 32); liquid carves can re-level briefly. |
| cpu mirror (dungeons (world)) | GAP | TwinCell and TwinCarveAt are called only by selftests and SurfaceRects. Gameplay fallbacks read TerrainHeight, which has no dungeon term (flightprobe.h:44, world.cpp:1752), so unfetched dungeon air reads as rock (finding 1). |
| mutation queue (dungeons (world)) | OK | Carving is worldgen, the snapshot exception. detail.svedit v3 cells go out as CellOps through WorldEdits::Drain (worldedit.cpp:345-405, session.cpp:5705-5708). The dungeon code makes no CPU voxel writes. |
| persists (dungeons (entities)) | PARTIAL | Persisted as deltas: lever state, plate latch, gate raised+built, trap fires+done, key taken, spawner serial/triggered/slots, door open+unlocked, chest bag (refs_dungeon.cpp:414,421,725-733,896,1114,1296; refs_doors.cpp:151-171,736-746), stored in REFS region buckets that merge dormant records (chunkstore.cpp:718-725). Gaps: pulse deadline (:1115), trap in-flight spikes and collapse batch (:478-499, :629-661), cooldown armAt, and the spawner slot vs corpse RefId clash dropped at load (mob.cpp:31564). |
| deterministic (dungeons (entities)) | OK | Decisions are keyed on rng::Hash3(RefHash(id), serial\|tick) (refs_dungeon.cpp:1002,1014,671; loot.cpp:86) and run in std::map id order (:880-915). The fetch cache lands at the fixed-latency publish (world.cpp:1610-1613) and the schedule clock comes from the sim tick (schedule.cpp:359-362); no wall clock. Refs are unhashed CPU state. Chest take/put runs in the frame layer (main.cpp:18307-18359). |
| sleeps when idle (dungeons (entities)) | OK | TickDungeon returns at once when nothing is active and no door is signal-worked (refs_dungeon.cpp:884); each active entity costs a few compares. Small per-tick residue: the npc tick walks all refs (refs_npc.cpp:1356) and each plate builds a Bodies() vector. |
| cpu mirror (dungeons (entities)) | PARTIAL | Collapse and door capture read a fresh fetch cache (refs_dungeon.cpp:600; refs_doors.cpp:349). The dart wall test reads the 3x3x3 mirror and treats Unknown as open (:515-538, documented). Spawn placement waits for the cached column but never reads it (:788-807), so bodies land inside rock (finding 1). |
| mutation queue (dungeons (entities)) | PARTIAL | Every voxel change is a CellOp or ParticleSpawn in the tick's OpBatch (refs_dungeon.cpp:390,467,490,634,645,675,686; refs_doors.cpp:379,471). No budget is charged before emission: a gate build of up to 320k cells goes out in one tick against the 65,536 cap (:359, :1177; support.cpp:1251), and floods push 16,384 per fire. |

## Coverage (as reported by the finders)

**dungeons (world).** READ AND TRACED, all at 20aa002:
- src/sim/dungeon.h and src/sim/dungeon.cpp in full: JSON and binary loaders, ResolveShell limits, PlacedBounds/InflationOf/LocalBoxes, SurfaceRects, PackDungeons including the hash index, and the whole CPU twin.
- assets/shaders/worldgen_dungeon.wgsli in full.
- worldgen.wgsl dungeon hooks: genChunk 6600-6905 including the generation verdict and the wake; cols colTop 7240-7282; caveBands keep-out 3995-4012; tree, cactus, tile-plant and cover keep-outs.
- worldmap.cpp dungeon code: ParseDungeons, AnchorDungeon, FoldDungeonHashes, ReplaceScratchDungeon, the LoadWorldMap dungeon block and its anchoring scope, and the PackWorldMap dungeon block.
- worldedit.h and worldedit.cpp in full (SVED v3).
- voxregion.cpp DUNGEON preview and fast path; resources.cpp shader assembly; biomes.cpp StampEnvironment; worldio.cpp worldgen fingerprint; net Hello fields.
- The dungeon diffs: 143321a, cf3d514, 541404f, 4cd673b; 1dd67ac's sim-shader edits (solute fault diagnostics, not dungeon code); a94edc5 (render only).
- check_invariants.py's dungeon mirror check; selftest_dungeon.cpp's format and fast-path parts; place_dungeon.mjs, compile_dungeon.mjs, dungeon_build_lib.mjs.
- The seams I followed out of the world side: world/dungeon_refs.cpp and refs.cpp ReadDungeons; flightprobe.h, arrows.cpp, spell.cpp and World::KindAt/KindAtCached/Cached; the stream and worldgen_run gen-class and edit-layer handling; the materials R-reload path.

VERIFIED CLEAN:
- Every product in the evaluator stays below 2^31 for inputs ResolveShell accepts: segment split at 256 cells, Q2 clamp at 8192, Norm/Ell/Box2/Box3 sums.
- The helpers outside the mirror block match on both sides: dgWord, dgFlowy, dgListOfCell, dgSurfNear, fdiv, vsmooth, q15frac, vbilerp, reposeRunOf.
- The rbox r clamp is applied identically on both sides.
- Template rotation composes correctly with RotX/RotZ under multi-placement.
- The hash table is at most half full, stores full keys and records its longest probe, so no chunk is ever silently dropped.
- A 16^3 chunk always lies in exactly one 64^3 index cell.
- The keep-outs are per column (no seams at chunk boundaries), and the magma goes with the cave band.
- The sky ceiling rises to the dungeon box top.
- The genAct verdict reads the post-dungeon words, so carved chunks are never demoted to a sentinel.
- dgWord matches genCellIn's jitter, so carved stone stays JITTER-compressible.
- The shell bytes are folded into the env stamp, the worldgen fingerprint and the net Hello.
- No f32 anywhere in the near-field dungeon path. Packing and anchoring are deterministic.
- Detail-layer ops go through the CellOp queue.
- Materials used by the shipped dungeons: the only reactive pair at rest is candle_flame + water, a one-shot douse.

LATENT, NOT REPORTED:
- InflationOf inflates a prim by its OWN blend, but a later high-blend carve's smooth-min fillet can reach blend/4 cells past a low-blend neighbour. With blend 15-16 (the slider maximum) next to a blend-0 carve, a 1-voxel firm-shell band in flowy terrain can differ across a 64^3 index boundary. The shipped maximum blend is 14.
- A variant never applies its base's detail.svedit (worldedit.cpp:160, worldmap.cpp:873). No detail.svedit exists yet and no tool writes one.
- The union early-outs of dgSurfNear, dgCaveKeep and dgColTop span the whole map when dungeons are far apart, so every call loops over every rect or record. Perf only.
- A materials R reload never repacks the dungeon (or biome) material ids. This is general environment behaviour, not specific to dungeons.
- Liquid carves can re-level briefly at birth where an air carve owns cells below their level (shardwell_flooded p11, carve 82 over the sump). This is bounded.

NOT AUDITED:
- The JS generator and compiler internals (dungeongen.js, the kits, validate, variant resolution and cycle refusal).
- The far-field dungeon pass and far-parity (render only).
- tuner_server.py.
- The dungeon gameplay entities beyond the placement seam (the other agent's scope).
- Real-dungeon rest behaviour beyond the fixture gate. The REST check covers only _fixture.

Note: I wrote one throwaway helper script, dgcells.py, to the OS temp dir for the index-cell arithmetic. Nothing in the repo was touched.

**dungeons (entities).** READ IN FULL at 20aa002: src/world/refs_dungeon.{h,cpp}, refs_doors.{h,cpp}, loot.cpp, dungeon_refs.{h,cpp}, refs.h, refs.cpp, refs_game.cpp, refs_kinds.cpp, refs_npc.{h,cpp}, src/game/schedule.{h,cpp}.

COMMITS: the D3 diffs for ai_behavior (acquireCap), item.h (ItemKind::Key, keyId) and melee.cpp (the `unlocks` line); f8f9c8e (dart wall test); b910021 (validator, kit loot names); the refs_dungeon/refs_npc parts of 4420593 (ArmArcher).

DOCS: DESIGN.md 16.D3 and PLAN_dungeons 3.7, including the D3 amendment.

TRACED INTO OTHER CODE:
- mob.cpp: FindMobByRef, Spawn, BuildRig, Die, EvictDead, the PreTick husk/park sweep, LoadOne's one-ref-one-body rule, the GroundUnder escape cap, BookRising, TurnMob.
- persist.cpp: the MOBS and REFS sections, EntityParking::Unpark.
- worldio.cpp: region bucket save/apply. chunkstore.cpp: WriteEntityRegion merges dormant records, so un-pulled deltas survive a save.
- stream.cpp: modified_ fold; the save path covers the snapshot tail.
- world.cpp: the fetch cache lands at the N1 publish, so it is deterministic.
- support.cpp: SubmitTick cell-op caps and the single-producer audit. opsync Merge.
- main.cpp: boot refs wiring, F7/F9/F10 (devKeys = not typing, so player-reachable), the chest loot panel, load tick resume.

STATIC GEOMETRY CHECKS (Python): replayed PlanSpawn (RefHash + Hash3) for crypt_dead against heather_cairn shell prims; shardwell roof_slab vs its carve ellipsoid; the shardwell incline_gate has rock over its top row (fine); the heather_cairn sanctum_door has a door prop under its leaf box (fine).

CHECKED, NO DEFECT FOUND: loot rolls once and becomes the delta, so no reload re-roll. Key-taken, door-unlocked, gate raised/built and spawner slots all round-trip. Signals are group-scoped and there are no emitter/receiver loops. Multi-placement re-prefixing works for shipped data, whose schedules use bare names. The avatar is not in mobs_, so spikes do not double-hit the player. There is no Mob* use-after-realloc in DamageBodies.

NOT AUDITED:
- teeth_caves and shardwell spawner disks against their noisy cave tubes (only the rectilinear crypt was checked).
- ai_behavior beyond acquireCap; melee.cpp beyond the key line; mob_archery internals.
- MP handoff (ApplyHandoff) RefId handling.
- ui_entities/dungeongen JS beyond the lock validator and the crypt suggestion.
- selftest_dungeon_entities internals (scenario list only); the D9 HUD change.

DOC NOTE (not a finding): DESIGN.md:30697-30698 still says the dart has 'NO wall test', which D9 changed.

