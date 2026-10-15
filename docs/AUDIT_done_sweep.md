# Audit: design guideline 5, "is it actually done": persists / deterministic / sleeps when idle / CPU mirror / MutationQueue

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. Two guideline-5 sweeps (persistence; idle cost + CPU mirror + MutationQueue) ran across the engine, and the per-system auditors judged the same five properties for the ten recent systems; the matrix merges both. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

No confirmed findings.

## Also found by this sweep (canonical entry elsewhere)

- Overlaying a saved carved limb leaves the carve-charge counters at spawn totals, so old damage is billed again: see **SH-1** in `AUDIT_shields.md`.
- Saved, parked or handed-off NPC amputee returns with its stump closed and stops bleeding out: see **SC-2** in `AUDIT_scar_heal.md`.

## The five-property matrix

OK = verified in code; GAP = missing; PARTIAL = some paths; N/A = the property does not apply; · = not judged. Where two agents disagree both statuses are shown. The older systems were judged by the two guideline-5 sweeps; the last ten rows by each system's own auditor.

| System | persists | deterministic | sleeps when idle | cpu mirror | mutation queue |
|---|---|---|---|---|---|
| CA core (sim_step) | OK | · | OK | · | · |
| MPM fluid | OK | · | · | · | OK |
| water bodies / reservoir | PARTIAL | · | PARTIAL | · | · |
| solutes | OK | · | · | · | · |
| heat / temperature | OK | · | OK | · | OK |
| electricity / lightning | OK | · | · | · | · |
| gas & particles / far plumes | OK | · | · | · | · |
| wind / heat-wind / drafts | OK | · | OK | · | · |
| weather / rain exposure | UNCHECKED | · | PARTIAL | · | · |
| debris / islands / physics | UNCHECKED | · | · | OK | · |
| trees / felling | UNCHECKED | · | · | · | · |
| bodies: wounds / coats / burn / blood / infection | GAP | · | PARTIAL | · | · |
| corpses / ragdoll | PARTIAL | · | · | · | · |
| AI / schedules / NPC residents | OK | · | · | · | · |
| demons | OK | · | · | PARTIAL | · |
| items / equipment / held props | PARTIAL | · | · | · | · |
| world items / containers / vessels | UNCHECKED | · | · | OK | · |
| spells / caster / brush / explosions | OK | · | · | OK | · |
| world refs / doors / structures / locales | UNCHECKED / GAP | · | · | GAP | PARTIAL |
| worldgen / streaming / page table / tickets | OK | · | · | · | OK |
| player / avatar / session | GAP | · | · | · | · |
| net sync | PARTIAL | · | · | · | · |
| alchemy bench | UNCHECKED | · | · | · | · |
| dialogue | UNCHECKED | · | · | · | · |
| flow field | N/A | OK | OK | OK | N/A |
| draft jets | PARTIAL | PARTIAL | OK | GAP | N/A |
| naga | OK | OK | PARTIAL | OK | OK |
| shields | PARTIAL | OK | OK | N/A | OK |
| archery | PARTIAL | PARTIAL | OK | GAP | OK |
| projectiles | PARTIAL | PARTIAL | OK | OK | OK |
| dungeons | OK / PARTIAL | OK | OK | GAP / PARTIAL | OK / PARTIAL |
| venom necrosis | PARTIAL | OK | OK | N/A | OK |
| scar heal | PARTIAL | OK | PARTIAL | N/A | N/A |
| severed-limb buoyancy | GAP | OK | PARTIAL | OK | OK |

### Notes on every GAP / PARTIAL / UNCHECKED cell

- **draft jets, persists (PARTIAL):** Derived and never saved. A burst rebuilds it on worldgen/map swap (worldgen_run.cpp:71), a box move or a knob change (simulation.cpp:3405), but LoadWorld forces no burst (worldio.cpp:1015-1027), so a load with an unmoved box keeps the pre-load T for ~6-11 ticks.
- **draft jets, deterministic (PARTIAL):** Kernels are integer, barrier-separated, atomic-free, one writer per word (sim_draft.wgsl:1305-1456). The field becomes history-dependent after an in-session load, and the box is per-machine in multiplayer (support.cpp:1886-1903, session.cpp:5664).
- **draft jets, cpu mirror (GAP):** Documented open item (RESEARCH_wind.md:1176-1177): windfield.cpp's CPU mirror (debris, corpses, trees, the F1 readout) reads the ambient field and sees neither R nor the jets.
- **naga, sleeps when idle (PARTIAL):** Every in-window naga runs the path walk and DrapeTail's roughly 20 GroundHeightAt column scans every tick, even standing still (mob.cpp:6147-6191). Every mob pose does the same; a naga is parked outside the window.
- **shields, persists (PARTIAL):** Shield hp and lattice ride MOBS limb records (appended slots are always kLimbStored, mob.cpp:30930) and PLYR via KitFlushWorn/CaptureHeld (persist.cpp:212, coattransfer.cpp:72). However, the MOBS/unpark/handoff overlay never re-syncs voxelsCharged/weightCharged (mob.cpp:31344 vs 33698), so old chip volume is re-billed at the first chip after it comes back. The raise, stagger and pending attack are deliberately unsaved (mob.h:3665).
- **archery, persists (PARTIAL):** Stuck, dropped and strapped arrows are ITMS ground items, and ammo rides the saved kit. Embeds and in-flight arrows are dropped on save (embeds are a known issue), stuck arrows come back non-kinematic (debris.cpp:8121), and ArrowFlights/MobArcheryWorld are never reset on load (arrows.h:230 Clear() has no caller).
- **archery, deterministic (PARTIAL):** Flights are 24.8 fixed point, stepped once a tick in TickAuthority (session.cpp:6341) with hash seeds, and NPC draws use rng::Hash3. However, flights, draws and stuck_ ids survive LoadWorld/F7 (main.cpp:15661), so the post-load world depends on pre-load history.
- **archery, cpu mirror (GAP):** FlightSolidAt treats out-of-window cells as solid (flightprobe.h:30), so a flight leaving the window becomes a floating stuck item. In-window cells outside the mirror correctly use the fetch cache plus TerrainHeight with prefetch (arrows.cpp:405-417).
- **projectiles, persists (PARTIAL):** Channel wounds persist as limb/body lattice state, and organ totals are recomputed from the restored lattice (mob.cpp:33702). In-flight matter flights and nextSeq_ are not saved and survive an F10 load (main.cpp:15657-15665; spell.h:1703). organHpTaken is not saved (naming only).
- **projectiles, deterministic (PARTIAL):** PierceChannel uses a total-order sort with fixed-order sums (kerf.h:714-719, 763-798), and PierceAlongRay follows Jolt ray order. But the strike seed is Hash3(p.seq, ...) with p.seq a session-lifetime counter (spell.cpp:4048, 4983), so the soak and glance draws depend on flight history.
- **dungeons, cpu mirror (GAP):** TwinCell and TwinCarveAt are called only by selftests and SurfaceRects. Gameplay fallbacks read TerrainHeight, which has no dungeon term (flightprobe.h:44, world.cpp:1752), so unfetched dungeon air reads as rock (finding 1).
- **dungeons, persists (PARTIAL):** Persisted as deltas: lever state, plate latch, gate raised+built, trap fires+done, key taken, spawner serial/triggered/slots, door open+unlocked, chest bag (refs_dungeon.cpp:414,421,725-733,896,1114,1296; refs_doors.cpp:151-171,736-746), stored in REFS region buckets that merge dormant records (chunkstore.cpp:718-725). Gaps: pulse deadline (:1115), trap in-flight spikes and collapse batch (:478-499, :629-661), cooldown armAt, and the spawner slot vs corpse RefId clash dropped at load (mob.cpp:31564).
- **dungeons, cpu mirror (PARTIAL):** Collapse and door capture read a fresh fetch cache (refs_dungeon.cpp:600; refs_doors.cpp:349). The dart wall test reads the 3x3x3 mirror and treats Unknown as open (:515-538, documented). Spawn placement waits for the cached column but never reads it (:788-807), so bodies land inside rock (finding 1).
- **dungeons, mutation queue (PARTIAL):** Every voxel change is a CellOp or ParticleSpawn in the tick's OpBatch (refs_dungeon.cpp:390,467,490,634,645,675,686; refs_doors.cpp:379,471). No budget is charged before emission: a gate build of up to 320k cells goes out in one tick against the 65,536 cap (:359, :1177; support.cpp:1251), and floods push 16,384 per fire.
- **venom necrosis, persists (PARTIAL):** Necrotic cells ride the MOBS v4/AVTR v2 lattice (mob.cpp:30930-30942) with by-name remap (31278-31287), and the latch is re-derived on load (31657, avatar.cpp:293). But the overlay never restores the carve-charge baseline (OverlayLimbLattice, mob.cpp:31344-31402; finding 2), and the bite's woundWas table is unsaved by design (mob.h:1982).
- **scar heal, persists (PARTIAL):** Scar cells persist in limb lattices: SaveOne PodVec plus by-name remap (mob.cpp:30941-30942, 31278-31282) and AVTR (avatar.cpp:1290, 1335-1339). scarSig is derived and the pace is stateless. The pause/regen gate's inputs (stumpOpen/bleedBudget/gushTicks) are not saved, and NPC loads do not re-derive stumps (mob.cpp:31512-31518, 31655-31657); see finding 2.
- **scar heal, sleeps when idle (PARTIAL):** A settled limb costs the wait checks and a modulo per tick, plus ScarSigOf every 3rd tick (mob.cpp:16561-16578). But the signature includes the creature-wide burnFrac_ (16551), so healthy limbs re-walk the full recipe while any other limb burns or mends (finding 6). There is also one full walk per limb after every spawn or load (scarSig 0).
- **severed-limb buoyancy, persists (GAP):** The DBRS record has no `dead` flag (debris.cpp:7980-8003), and LoadState calls RestoreBody(..., false) at debris.cpp:8201. The air fraction at 2809 and IsFlesh (debris.h:296) are therefore lost on load. The wound is not saved either. floatRho is derived data and is correctly recomputed.
- **severed-limb buoyancy, sleeps when idle (PARTIAL):** FloatBodies skips asleep bodies (debris.cpp:2767) and the density is cached. But SettleBodies wakes every aligned floater over water about every 60 ticks (debris.cpp:2956-2962).
- **water bodies / reservoir, persists (PARTIAL):** WTRB section exists (src/sim/waterbody.cpp:554); contents not audited
- **weather / rain exposure, persists (UNCHECKED):** rain exposure stated history-free (src/sim/rainexpo.h:34); weather state not traced
- **debris / islands / physics, persists (UNCHECKED):** DBRS registered (src/game/persist.cpp:1005); record contents not read
- **trees / felling, persists (UNCHECKED):** not traced
- **bodies: wounds / coats / burn / blood / infection, persists (GAP):** lattice and infection round-trip, but carve-charge counters are not resynced (src/game/mob.cpp:31344) and NPC open stumps are not reopened (src/game/mob.cpp:31517)
- **corpses / ragdoll, persists (PARTIAL):** dead records saved with cause and pending rise (src/game/mob.cpp:31025); EnterLoadedDead not read
- **items / equipment / held props, persists (PARTIAL):** mob gear and pack saved by name (src/game/mob.cpp:31018); PLYR/ITMS not read
- **world items / containers / vessels, persists (UNCHECKED):** ITMS v9 not read
- **world refs / doors / structures / locales, persists (UNCHECKED):** REFS registered (src/game/persist.cpp:1100); not read
- **player / avatar / session, persists (GAP):** AVTR v2 restores the lattice through OverlayLimbLattice without resyncing charge counters (src/game/avatar.cpp:286)
- **net sync, persists (PARTIAL):** handoff shares OverlayMobRecord, so it inherits both body gaps (src/game/mob.cpp:31404)
- **alchemy bench, persists (UNCHECKED):** not traced
- **dialogue, persists (UNCHECKED):** DLGF registered (src/game/persist.cpp:1173); not read
- **world refs / doors / structures / locales, cpu mirror (GAP):** dart wall test uses mirror-only KindAt, Unknown=open (src/world/refs_dungeon.cpp:532)
- **world refs / doors / structures / locales, mutation queue (PARTIAL):** all trap/gate writes go via c.ops->cells, but none checks kMaxCellOpsPerTick before emitting (refs_dungeon.cpp:467,490)
- **world refs / doors / structures / locales, persists (GAP):** SaveTrap stores only fires/done; spikes' placed list lost (refs_dungeon.cpp:421)
- **weather / rain exposure, sleeps when idle (PARTIAL):** rainExpo window-sized every rain/wet tick by design (simulation.cpp:3381-3387); rainFall fixed 64 groups C_ALWAYS (pass_table.def:227)
- **water bodies / reservoir, sleeps when idle (PARTIAL):** dispatch shaped by D_WATERCHUNKS (pass_table.def:859-1001); kernel internals not audited
- **bodies: wounds / coats / burn / blood / infection, sleeps when idle (PARTIAL):** ScarTick settled-signature gate (mob.cpp:16578); InfectTick/BleedTick per-corpse cost not traced
- **demons, cpu mirror (PARTIAL):** ScanNear reads host mirror only; peer summoner sees nothing (demon_malice.cpp:198)

## Coverage (as reported by the finders)

**G5: persists.** Read: worldio.cpp SaveWorld/LoadWorld (lines 640-1084) and the section-file plumbing. pass_table.def PT_LOADRESET (which GPU layers a load zeroes: particles, gas, MPM args/calm, heat, charge, openness, glow). Simulation::EncodeLoadReset. Stream::FlushResident (live ticket slots are saved; mask = modified_, sized kNumSlots, so no out-of-range) and ReloadWindow (tickets dropped, solutes captured and reset). Tickets.h. The main.cpp save/load handler (15595-15690: tick resumes above the save, MPM count cleared, grenades cleared). persist.cpp section registrations DBRS/MOBG/MOBS/REFS. Mob::SaveOne, OverlayMobRecord, OverlayLimbLattice and LoadOne in full. The AVTR save/load and PlayerAvatar::Spawn's restore. The bleed/stump code (Sever, DetachLimb, DrainBlood) and the CarveLimb charge accounting. The DMNS/CNTR demon sections and restore matching. The sim_draft.wgsl history notes (a load bursts a fresh solve: documented). The flowfield.h authority notes. Checked against the design and dropped as intended: heat and charge zeroed on load, particles/gas/MPM in flight zeroed, draft field re-solved by a burst, unbound demons load loose, ids fresh with MOBG kept at max(), parked far landings are not saved, AI memory limited to 5 facts, stunUntil_/shieldStagger_ transient. Not read: ITMS/PLYR/REFS/DLGF/TIME payload internals, WTRB contents (only confirmed it exists and has a gate), the DebrisSystem DBRS record (BodyWound persistence for debris corpses), the world items/vessels/alchemy bench sections, net sync, the Weather/windfield CPU state, the solute file beyond its registration. Shields, archery, dungeons, venom and scar heal were left to the dedicated agents beyond the shared counters noted in finding 1.

**G5: idle / mirror / queue.** Static, reduced effort, grep-driven. READ: every WriteBuffer/UploadBuffer/CmdUpdateBuffer site in src/ outside tests (simulation.cpp, world.cpp, flowfield.cpp, boltfx.cpp, session.cpp, main.cpp), classified as follows. Material, reaction, heat, elec and param tables, the cloud, rain and god maps, and the draft, flow and wind buffers are derived or render data. world.cpp:880 is the page-table ticket table (derived). EncodeWakeAll writes the dirty set (scheduling, mirrored to cpuDirty). session.cpp:1944 is the documented clear-fluid exception; CLAUDE.md still cites main.cpp:6392 for it, a stale reference and not a defect. No voxel/side-layer write bypassing the queue was found; stream.cpp and pagetable.cpp writes were not opened individually and were assumed to be snapshot restores and derived page-table data. Pass table: listed every PT_TICK direct (non-indirect) dispatch. rainFall (64 groups, C_ALWAYS), heatDue (128 groups), draftMaskAll (2048 groups, rebuild ticks only) and compact/compactNext (O(chunks)) are fixed or window-proportional by documented design. rainExpo (simulation.cpp:3381-3387) runs window-sized every tick while it rains or the rain word carries wetness; the pass_table comment says this is intended. Not reported, marked PARTIAL. CPU mirror: traced KindAt and the direct snapshot-mirror readers: refs_dungeon dart (finding 1), container.cpp ContainerSnapWord (player scoop, demons), spell.cpp SnapMatAt, arrows (FlightSolidAt plus prefetch, OK), mob_cellcache/mob_swim (KindAt then cache fallback, OK), demon_malice ScanNear (around the summoner; it uses the host mirror, so a peer summoner sees nothing, a multiplayer gap that was not traced further). The CPU current (currentprim.cpp:368 to WaterFlow().Sample) reads a window-composed field built from a pure function of map/seed/tuning; OK. Dungeon traps: also read flood/spikes/collapse/fire_vent emission. Trap cell ops are pushed with no room check against kMaxCellOpsPerTick (contrary to 'charge before emission'), relying on the SubmitTick clamp; a once-only flood would then record 'flooded' with cells refused. Not reported because it needs ~49k other cell ops on the same tick. NOT AUDITED: session.cpp TickAuthority per-creature loops in depth (only read Mob::HealTick/ScarTick, which are settled-signature gated, and MobSystem::PreTick's head, which runs BurnLimbs behind an admission pot); InfectTick, BleedTick and AI think cost per corpse; sim_heat/sim_elec WGSL internals; the reservoir/water-body passes beyond their D_WATERCHUNKS dispatch shape; heat-wind updraft kernels; any WGSL; stream.cpp and pagetable.cpp writes line by line; synchronous readbacks (every WaitIdle/ReadBufferBlocking found in main.cpp sits in harness/shot/save/load/teleport/dev paths, none in the per-frame play path, but the 22 sites were not all opened).

