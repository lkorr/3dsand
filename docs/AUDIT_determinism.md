# Audit: determinism hazards in the sim kernels and the tick

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. Five determinism sweeps: sim_step (two halves), the MPM fluid, the kernels landed since the 2026-10-02 audit (heat, electricity, drafts and jets), and the CPU tick + op stream. They hunted runtime f32 in hashed math, ordering-dependent atomics, write reach and same-dispatch read/write races, identities keyed on the wrong index, window-origin dependence, frame-local values in the tick, and uninitialised or stale state. sim_waterbody, sim_solute, sim_gas, sim_particle, sim_mutate / sim_explode and worldgen were covered by the 2026-10-02 audit (docs/PLAN_vulkan_port.md) and were NOT re-swept this time. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| DET-1 | HIGH | CA neighbour rewrites flagSupportLoss(n): reading n+d (distance 2) races a same-colour writer | `assets/shaders/common.wgsl:7720` | small | no | fixed, gates pending `8833133` |
| DET-2 | HIGH | Sparse workgroup's gas-leave tail rebuilds the leaver's chunk from a stale list slot beside a ticket | `assets/shaders/sim_step.wgsl:5967` | small | no | fixed, gates pending `6ff8175` |
| DET-3 | MED | Storm gust front and downbursts are placed at the window centre: peers diverge, bursts jump on shift | `src/sim/windfield.cpp:1049` | small | yes | open |
| DET-4 | MED | MPM particles frozen out of residency never die, so the seam and CA can never prove the world idle | `assets/shaders/sim_fluid.wgsl:655` | medium | yes | open |
| DET-5 | MED | After a window shift, the CA and excite read the evicted chunk's MPM grid through the recycled slot | `assets/shaders/sim_step.wgsl:2489-2499` | small | no | fixed, gates pending `474c61c` |
| DET-6 | MED | settleColumn walks above the window top without a residency test and aliases onto the bottom plane | `assets/shaders/sim_fluid_seam.wgsl:2396-2399` | trivial | no | fixed, gates pending `11a8adc` |
| DET-7 | MED | Elec pond cap and purge are window-wide aggregates; shared chunk charge differs across peers | `assets/shaders/sim_elec.wgsl:586` | medium | yes | open |
| DET-8 | MED | Wind-glyph primitives never go on the wire, so the peer's CA misses the gust and its wake | `src/game/session.h:457` | small | no | fixed, gates pending `882a586` |
| DET-9 | MED | Reaction blasts authored by every peer with no authority gate, so owned bodies are blasted twice | `src/game/session.cpp:5385` | small | no | fixed, gates pending `12a989b + 2b9bc9c` |

## Findings

### DET-1 (HIGH) CA neighbour rewrites flagSupportLoss(n): reading n+d (distance 2) races a same-colour writer

**Status:** FIXED on branch `audit-1014` in `8833133`. Confirmed: when the CA actor c rewrote a face neighbour n = c+d (coat partner, PAIR product, stain consumption), flagSupportLoss(n) read n+d = c+2d, which sits inside the same-colour actor c+3d's write box. Whether that cell's chunk got a support flag therefore depended on GPU scheduling, and the flag decides when islands drop. Added flagSupportLossNbr(c, n, ...) to sim_step.wgsl (not common.wgsl, since only sim_step uses it), modelled on flagSupportLossBrush: it never reads the away face, flags that face's chunk unconditionally (a superset), keeps the other five reads (all within c's own box), and applies common's rule otherwise. The three call sites now use it.

**Where:** assets/shaders/common.wgsl:7720 (`flagSupportLoss` neighbour read), called with the actor's NEIGHBOUR from assets/shaders/sim_step.wgsl:3059, :3367 and :3811.

**What:** When a CA rule rewrites the acting cell's face neighbour n = c+d, the kernel calls `flagSupportLoss(n, ...)`. That helper reads all six neighbours of n, including n+d = c+2d. That cell is inside the write box of the same-colour actor c+3d in the same dispatch, so whether `supportOut[chunk(c+2d)]` gets set depends on GPU scheduling. Audit row #10 of 2026-10-02 fixed this race for sim_mutate and sim_explode only.

**Trace:**
- sim_step.wgsl:1822-1835: lattice note. Same-colour write boxes tile space, so a read at distance >= 2 is a determinism break.
- sim_step.wgsl:3052-3059: coatReact stores the partner `pni` and then calls `flagSupportLoss(n, ...)` with n = c+faceDir(pk).
- sim_step.wgsl:3304, 3355-3367: PAIR neighbour n = c+faceDir(di) is rewritten, then `flagSupportLoss(n, ...)`.
- sim_step.wgsl:3808-3811: stain consumption stores 0 into ni, then `flagSupportLoss(n, nmat.klass, MAT_AIR)`.
- common.wgsl:7718-7722: reads `voxWordAt(n+faceDir(i))` for every i, including the face pointing away from the actor, and sets supportOut on a solid.
- debris.cpp:847-897: QueueSupportEvents turns flags into supportCooldown_ entries, the late re-arm queue and pendingSupport_ order, which set WHEN islands drop.

**Failure scenario:** Acid (or lava, mites, aqua regia) at x and x+3, with a lone 2-cell stone or wood beam at x+1 and x+2, in one substep-0 colour pass. A rewrites x+1 and reads x+2. B rewrites x+2 and reads x+1. Each read sees the pre-pass solid or the other actor's store depending on scheduling, so the flag for chunk(x+1) or chunk(x+2) is present in one run and absent in the other. The chunk goes on the scan cooldown in one run only, a later detachment falls on a different tick, and twice-run hashes diverge.

**Suggested fix:** Add a CA variant, for example `flagSupportLossFrom(n, oldKlass, newMat, awayFace)`, modelled on `flagSupportLossBrush` (sim_mutate.wgsl:336). Never read the face pointing away from the actor. Flag `chunkIndexW(n+d)` unconditionally when that cell is in the window, which gives a superset that is a pure function of the firing. Use it at the three sites. The other probes are at Chebyshev distance 1 from c and stay. Extend audit row #10 to list these sites.

**Verifier's note:** I confirmed that the PAIR and stain neighbours are face directions (faceDir), so n+d lies exactly on the next same-colour actor's box boundary. I confirmed that row #10 names only the brush, cells and blast paths. The flag difference matters only when no other probe flags the same chunk (often chunk(n+d) == chunk(n) and a lateral solid flags it anyway), so the window is narrow but reachable. This is static reasoning; no gate can force the interleaving (row #10 says so).

### DET-2 (HIGH) Sparse workgroup's gas-leave tail rebuilds the leaver's chunk from a stale list slot beside a ticket

**Status:** FIXED on branch `audit-1014` in `6ff8175`. Confirmed: caChunkAtEdge used chunkResident, which counts ticket chunks as resident, but a window cell's inBounds checks only the window. So a window-face chunk next to a flush ticket box could go SPARSE while its gas still deferred as a leaver, and the leave tail rebuilt its chunk from a stale dense-list slot (caMask[gCaList+first+j]). Window slots now ask chunkInWindow and ticket slots keep chunkResident, matching inBounds. The tail is also hardened: a deferral inside a !dense workgroup counts GAS_SP_OVERRUN and leaves the voxel unwritten. Side effect: dense face chunks beside a ticket used to refuse every leaver (allow=0). They now get an edge budget and leak gas as particles, like any other window face.

**Where:** assets/shaders/sim_step.wgsl:5967 (the leave tail), with the predicate mismatch at sim_step.wgsl:4889 and sim_step.wgsl:684.

**What:** camask decides whether a chunk is an edge chunk with `chunkResident`, which counts ticket chunks as resident. A window cell's `inBounds` sees only the window. So a window-face chunk next to a live ticket box can go SPARSE while its gas still defers as a window leaver. The leave tail then rebuilds the leaver's slot as `caMask[gCaList + first + j]`, which is only valid in DENSE workgroups. In a sparse workgroup it reads a stale list entry and moves a word in an unrelated chunk.

**Trace:**
- src/sim/tickets.cpp:108 BoxHitsWindow is a strict overlap test, so a box can sit flush against the window face
- assets/shaders/common.wgsl:5245 chunkResident is true for ticket chunks
- assets/shaders/sim_step.wgsl:4889 caChunkAtEdge returns false when all 26 neighbours are window or ticket
- assets/shaders/sim_step.wgsl:5318 wgCaEdge=0, so sim_step.wgsl:5444 allows sparse
- assets/shaders/sim_step.wgsl:684 inBounds for a window cell is inWindow only
- assets/shaders/sim_step.wgsl:5025 primary step out of the window, so gasLeaveDefer
- assets/shaders/sim_step.wgsl:5838 first = wg.x*pack (sparse wg.x >= dwg); 5848 allow is computed only if dense
- assets/shaders/sim_step.wgsl:5967 ci = caMask[gCaList+first+j] is stale, then stepGasFrom at 5988 runs on the wrong cell

**Failure scenario:** A blast near a window face throws debris out, and a Particle ticket is placed flush against the face. Smoke reaches x=511 of a sparse face chunk opposite the box. The tail resolves a stale slot, and tryMove copies the smoke into a neighbour of an arbitrary cell and overwrites that cell with the neighbour's old word, so terrain gets holes. The location depends on leftover list contents (scheduling-dependent order), so the world hash is not reproducible. In DENSE face chunks the same mismatch only refuses every leaver (allow=0): smoke sheets against the face.

**Suggested fix:** For window slots (ci < NUM_CHUNKS), make caChunkAtEdge test chunkInWindow, the same question inBounds asks. Keep chunkResident for ticket slots. Harden the tail as well: record the leaver's slot when it defers, or refuse and count GAS_SP_OVERRUN for any deferral from a !dense workgroup.

**Verifier's note:** I read inBounds, caChunkAtEdge, chunkResident, the sparse predicate, the sparse pool gather (5904-5922), the gasLeaveDefer record (4855-4862) and the tail. The claim's comment at 5841-5842 ('a sparse workgroup holds no edge chunk') rests on the camask edge test, which disagrees with inBounds. The trigger needs a ticket box touching the face, which PlaceBox allows.

### DET-3 (MED) Storm gust front and downbursts are placed at the window centre: peers diverge, bursts jump on shift

**Where:** src/sim/windfield.cpp:1048-1116 (WeatherPrims)

**What:** The storm's gust-front cone and downbursts are positioned relative to the residency-window centre (`origin*kChunk + kWorldN/2`) plus hashed offsets. They are appended to tp.windPrims, which the CA reads per cell through windPrimAtQ, so they reach hashed state. This is a second sim input keyed on the window origin besides the grandfathered WaterBodies(). Peers with different windows put the storm wind in different world places, and in single-player every window shift moves the live bursts and the front by 16 voxels per shifted chunk.

**Trace:**
- src/test/support.cpp:1578-1581 AppendWeatherPrims(..., world.WindowOrigin(), ...) into tp.windPrims
- src/sim/windfield.cpp:1049-1051 cx/cz = origin*kChunk + half
- src/sim/windfield.cpp:1085-1086 front p.x/p.z = cx/cz - back*dir
- src/sim/windfield.cpp:1114-1115 burst p.x/p.z = cx/cz + hashed ox/oz
- assets/shaders/common.wgsl:4133 windAtQ = windAmbQ + windPrimAtQ
- assets/shaders/sim_step.wgsl:4720 the CA wind adds windPrimAtQ per cell

**Failure scenario:** During a storm (stormBursts=2 and stormFrontJet on by default), two peers whose windows differ by 2 chunks place each downburst 32 voxels apart. Sand and smoke under the bursts are pushed differently and the shared chunks diverge until chunksync repairs them. In single-player, a player running through the storm drags every burst footprint along in 16-voxel steps on each recentre.

**Suggested fix:** Make the positions a pure function of (seed, tick, world position). For example, emit bursts per world super-cell from Hash3(seed, stormCycle, cellX, cellZ) and keep those overlapping the window, and anchor the front to a world-lattice point per cycle. The window then only filters the prims.

**Owner call:** Storms as a world-anchored event versus a player-local effect. A world-anchored storm can miss the player entirely.

**Verifier's note:** I checked the origin source at the call site, the defaults, and that the CA (not only the render path) consumes windPrimAtQ. The code comment's 'reproducible' holds only for a single-machine replay. One related point I did not trace: FillWindField's terrain table is also described as 'centred on the window' (support.cpp:1871-1874), which may be a third window-keyed wind input.

_Also reported independently by: AUDIT_determinism.md (det: sim_step B)._

### DET-4 (MED) MPM particles frozen out of residency never die, so the seam and CA can never prove the world idle

**Where:** assets/shaders/sim_fluid.wgsl:655 (freeze), assets/shaders/sim_fluid_seam.wgsl:499-503 (compaction keeps them), src/sim/simulation.cpp:4114-4120 (idle latch)

**What:** An MLS-MPM particle whose cell leaves the residency window freezes by design (sim_fluid.wgsl:44-46). No path ever ends that state while the window stays away: no kill, no deposit, no hand-off. The particle stays counted in FA_LIVE, and in EX_EXCITED_LIVE if excite created it. FA_LIVE feeds the CPU fluidCount, which counts as a tick input every tick. So the settled-tick skip never fires, and the seam plus the 9-substep table are recorded forever.

**Trace:**
- assets/shaders/sim_fluid.wgsl:655 `mark` returns on !cellResident, so the chunk gets no block. g2p gathers zero weight and the particle freezes (1421-1424).
- src/sim/stream.cpp:465-481 ShiftAxis evicts the plane with no MPM handling.
- assets/shaders/sim_fluid_seam.wgsl:499-503 compactCount keeps every fpAlive particle and counts every FP_EXCITED one, with no residency test.
- src/sim/world.cpp:1333 snapshot fluidLive = FA_LIVE.
- src/game/session.cpp:5955 fluidCount = fluidLive + pending spawns.
- src/sim/simulation.cpp:4114-4120 fluidCount>0 counts as an input, so settledProven_=false and caActive=true.
- src/sim/simulation.cpp:4335 seamActive is true, so the FluidSeam, FluidMap and substep tables are recorded every tick.

**Failure scenario:** A waterfall excites falling water (fluidExciteMode=1). The player walks away, and the plane holding particles in flight leaves the window. From then until the player returns, the world can never prove itself idle (rule 2), and every tick pays the seam and solver over the frozen set. With enough frozen excited particles (~sim.fluidExciteCeiling), exciteScan's ceilRoom (seam:1310-1313) reaches 0, and new splashes near the player stop producing MPM water.

**Suggested fix:** Give MPM a policy for leaving residency. Either force-settle or deposit a plane's particles before EvictSlots (as ballistic far landings do), or count resident-live separately in compactCount and use that count for the CPU latch input, seam recording and the excite ceiling.

**Owner call:** Should water in flight that leaves the window be deposited, deleted or parked? Each choice has a persistence trade-off. A reload drops parked particles, while deposit writes voxels at the window edge.

**Verifier's note:** I confirmed there is no kill or deposit path for non-resident particles (stream.cpp has no fluid code, and the only kills are the FA_KILL* paths, all residency-gated). The freeze itself is documented as intended. The defect is that nothing bounds it. I downgraded HIGH to MED because the standing cost is dispatch overhead over the frozen set, not woken chunks. I did not check whether saves flush MPM.

### DET-5 (MED) After a window shift, the CA and excite read the evicted chunk's MPM grid through the recycled slot

**Status:** FIXED on branch `audit-1014` in `474c61c`. Confirmed: fluidBlockMap's index word and fluidCalm are keyed by slot and carry across ticks. The CA's fluidOccMat and the seam's exciteDetect/consumeApply read them before the next alloc's phase A retires them, and nothing reset them when a slot was recycled. New Stream::ForgetFluidSlots zeroes the index word and fluidCalm for the slots it is given, using deferred queue writes between ticks with consecutive runs coalesced (an x-plane shift costs 2,048 four-byte updates). It is called from ShiftAxis for the leaving plane and from FillTicketSlots for slots a ticket takes over. This one fix also covers the merged det-mpm:2 duplicate. The Y-mask half is render-only and phase A clears it, so it is left alone.

**Where:** assets/shaders/sim_step.wgsl:2489-2499 (fluidOccMat), src/sim/stream.cpp:480-481 (slot recycle)

**What:** The chunk-slot to MPM-block map and the block's grid nodes carry from tick T into tick T+1 and are keyed by SLOT. The CA (PT_TICK) and the seam front half (exciteDetect) read them before the next alloc retires them. ShiftAxis recycles the leaving plane's slots for the entering plane without clearing the map or fluidCalm. On the first tick after a shift, those reads therefore see the evicted chunk's node mass, velocity and intent at the same local cell, 512 cells away.

**Trace:**
- src/sim/stream.cpp:480-481 SetWindowOrigin(no) then FillSlots, with no fluid reset.
- src/sim/pass_table.def:1463-1465, 1576-1578: fluidCalm and fluidBlockMap are filled only on WORLDGEN and LOADRESET.
- src/sim/simulation.cpp:4301 Tick table, then 4339 FluidSeam, then 4345 FluidMap.
- assets/shaders/sim_step.wgsl:2491-2499: chunkSlotOf(wc, T.origin), then fluidBlockMapS[wsl], then fluidGridS and the fluidCellScratch intent.
- assets/shaders/sim_fluid_seam.wgsl:970-976, 1144-1159: exciteDetect's neighbour block and wake-speed trigger read the same stale map and grid.
- assets/shaders/sim_fluid.wgsl:739-749: alloc phase A retires the stale entry only after those reads. The grid clear (826) runs per substep.

**Failure scenario:** The player walks +x past MPM water sloshing at the trailing window face. On the shift, that face's grid aliases onto the entering chunk at the same local coordinates. A PAIR scan there sees phantom excited water, so lava or fire reacts with it and writes a product. exciteDetect may also wake settled water next to a phantom fast node. The result is deterministic for one window trajectory but depends on window history, so M9 peers with different trajectories can diverge.

**Suggested fix:** When slots are recycled (ShiftAxis/FillSlots, ticket re-centre), zero fluidBlockMap[s], its Y-mask word and fluidCalm[s] before the next tick. A cheap alternative is to store each block's world-chunk key beside fluidBlockList and reject mismatches in fluidOccMat, exciteDetect and consumeApply.

**Verifier's note:** I confirmed the record order, the absence of any C++ reset, and that the grid persists across the tick boundary (cleared per substep at sim_fluid.wgsl:826). The window lasts one tick. Impact needs MPM water at the leaving face plus an air or reactive cell probing the aliased spot, which is a narrow case.

_Also reported independently by: AUDIT_determinism.md (det: sim_step A)._

### DET-6 (MED) settleColumn walks above the window top without a residency test and aliases onto the bottom plane

**Status:** FIXED on branch `audit-1014` in `11a8adc`. Confirmed: settleColumn's spill walk (up to 24 cells, or 88 when forced) called voxWordIndex with no residency test. Above the window top that falls back to the masked slot, the bottom-plane chunk 512 cells below, which the check pass read and the commit pass could write into. The walk now applies seamSupport's rule first: a cell that is not cellResident is a blocker, and idx stays PT_NO_WORD so the commit cannot write it.

**Where:** assets/shaders/sim_fluid_seam.wgsl:2396-2399 (walk) and 2492-2501 (commit write)

**What:** settleColumn walks up to 24 cells above a settling block's base, or 88 when the block is forced, through voxWordIndex with no residency test. For a block in the window's top chunk plane, those cells are outside the window. Without a ticket, voxSlotOfCell falls back to `c & WORLD_MASK`, which is the window's BOTTOM-plane chunk 512 cells below. The feasibility pass reads it, and the commit pass can write pooled eighths into it.

**Trace:**
- assets/shaders/sim_fluid_seam.wgsl:2388 yTop = 16 + SETTLE_SPILL (+ SEAM_FORCE_REACH when forced).
- assets/shaders/sim_fluid_seam.wgsl:2397-2399 voxWordIndex(c), with no cellResident check.
- assets/shaders/common.wgsl:6960-6968 a ticket miss falls back to the masked window slot (the comment says only a contract-breaking caller reaches it).
- assets/shaders/common.wgsl:7498-7508 a real page gives a real word index.
- assets/shaders/sim_fluid_seam.wgsl:2499-2501 `voxels[idx] = nw; markDirtyNext(...)`: the dirty mark goes to the out-of-window cell, not the aliased chunk.
- Contrast assets/shaders/sim_fluid_seam.wgsl:2231 (seamSupport) and 1038 (excite), which apply the residency rule.

**Failure scenario:** A clifftop pond or waterfall in the top chunk plane, with the player in a valley, has MPM water that settles. If the bottom-plane chunk at that x,z holds a real page (a cave or lake), the spill rows read its water. The block can then be refused forever (pool>0 && !floorOk), or the commit writes eighths into the aliased cells, undirtied. The outcome depends on the window origin. If the aliased chunk is a sentinel, the result is a permanent refusal instead.

**Suggested fix:** In settleColumn, treat `!cellResident(c, T.origin)` as blocked before calling voxWordIndex, which is the residency rule seamSupport already applies.

**Verifier's note:** Confirmed the walk, the fallback and that excite can seed top-plane particles. If a ticket covers the chunk above, the probe returns that ticket's slot instead, which is correct. Real impact needs a real page in the bottom plane at the same x,z.

### DET-7 (MED) Elec pond cap and purge are window-wide aggregates; shared chunk charge differs across peers

**Where:** assets/shaders/sim_elec.wgsl:578-588 (pond cap) and :519 (purge decision).

**What:** The liquid charge cap is Q*V/max(N,V), where N and Q are aggregated over every charged chunk in the residency window. The whole field is purged at the tick tail when no chunk anywhere in the window was marked. Both are deterministic on one machine. However, two M9 peers whose windows hold different charged regions compute a different cap, or a different purge decision, for a chunk they both simulate. P feeds hashed CA outcomes in sim_step: electrolysis, ohmic ignition and crackle.

**Trace:**
- assets/shaders/sim_elec.wgsl:875-884 per-round liqN (sum) and qmax (max) are accumulated over all live chunks
- assets/shaders/sim_elec.wgsl:584-586 `EM_LIQ_CAP = min((q*vol)/max(liqN, vol), 65535)`, one value for the window
- assets/shaders/sim_elec.wgsl:966-971 EM_MARKED is incremented by any settled chunk with a dirty neighbour
- assets/shaders/sim_elec.wgsl:519 `purge = tail && EM_MARKED == 0`, a window-wide decision to drop all charge
- DESIGN.md:328-333 CA outcomes are meant to be independent of the peer's window

**Failure scenario:** The host's window holds a large charged body with N > V that the client's window does not hold. A brine puddle both peers simulate is capped lower on the host, so its electrolysis rolls differ. In the second case, charge stranded in a shared chunk is kept on the machine whose window has some other marked field and purged on the other. When the chunk wakes, it reacts on one machine only.

**Suggested fix:** Localise both aggregates. For the cap, compute N and Q per chunk neighbourhood or per connected liquid body. For the purge, decide per chunk from its own N27 dirty test, which elecSettle already has as wgDirtyNb, instead of from the window-wide EM_MARKED.

**Owner call:** DESIGN.md:2418 accepts global pond dilution within one world. Peers with different windows need a decision on whether the pond rule should be local.

**Verifier's note:** I read sim_elec.wgsl:505-525, 560-615, 860-890 and 955-980. The code is as claimed. The reductions are order-independent, so single-machine twice-run determinism is not affected; this is peer divergence only. The cap only diverges when N exceeds V = elecPoolCells on at least one machine. The purge split is easier to reach. Charge decays quickly, so both cases need concurrent charge fields, which keeps this at MED.

### DET-8 (MED) Wind-glyph primitives never go on the wire, so the peer's CA misses the gust and its wake

**Status:** FIXED on branch `audit-1014` in `882a586`. Wind primitives were spawned only into the casting machine's WindPrims(), which feeds only that machine's TickParams. OpBatch gains a seventh vector, `winds`: it is in WalkBatch, Merge and MergeStats.remoteWinds, and kOpsWireVersion goes from 2 to 3. A new SpawnWindPrim(w, out, p, tick) is now the only way into the list for player spell gusts and creature casts (demon_cast Route). Single-player stamps spawnTick and spawns at once, as before. When connected it stages the gust on out.winds; phase N then spawns the merged list with spawnTick = label tick on both machines and counts refusals in ui.windPrimsDropped. The dev-panel fan stays local on purpose, because it is a debug control.

**Where:** src/game/session.h:457 (OpBatch has no wind vector).

**What:** A Wind glyph's primitive is spawned only into the casting machine's WindPrims() list, which is copied into that machine's TickParams. OpBatch and Merge carry no wind primitives, so the peer's CA never sees the gust: no drift, no entrainment licence, no footprint wake.

**Trace:**
- src/game/session.cpp:4708 WindPrims().Spawn(w) for emit.winds
- src/test/support.cpp:1565 WindPrims() copied into tp.windPrims; the wake is built from it
- src/game/session.h:457 OpBatch: ops, exps, cells, spawns, fluid, gas only
- src/net/opsync.cpp:162 Merge has no wind handling
- assets/shaders/sim_step.wgsl:4720 caWindAt; sim_step.wgsl:6479 windPrimEntrainsQ licence

**Failure scenario:** A client casts an entraining gust over a sand bank both machines hold. The client lifts grains and bends smoke; the host's bank stays put. The chunks diverge until a quiet-chunk resync.

**Suggested fix:** Add a `winds` vector to OpBatch, push local spawns into it in phase N, and Spawn() the merged list on both machines at the label tick with spawnTick = label, mirroring gas spawns.

**Verifier's note:** I confirmed there is no wind field in OpBatch or Merge and no WindPrim reference in src/net. I did not check whether the dev fan should stay local-only.

### DET-9 (MED) Reaction blasts authored by every peer with no authority gate, so owned bodies are blasted twice

**Status:** FIXED on branch `audit-1014` in `12a989b + 2b9bc9c`. Both machines decoded the same reaction firings and each authored the ExplosionOp. Both copies crossed the wire, so every owned body near a reaction blast was hit twice. ReactFxToBlasts now drops grid firings in chunks this machine does not own, counted in a new reactFx.notOwned field, before any blast or flash is authored. It does not use net::Hook(), as the finding suggested, because nothing installs that hook, so the test would never run. Instead it asks DebrisSystem::ChunkOwned (EntitySync::ChunkOwned behind it), the same authority question the crater's island scan asks. When unbound, as in single-player and the harnesses, that answers 'mine', so nothing is filtered. Body firings are not filtered.

**Where:** src/game/session.cpp:5385 (ReactFxToBlasts in the primary session, with no authority test).

**What:** Each machine decodes the same reaction firings for chunks it simulates and authors an ExplosionOp for each one, with no chunk-authority gate. Both copies cross the wire. sim_explode's lowest-op-wins rule makes the crater idempotent, but owned bodies are hit by the local copy in phase K and again by the peer's copy in phase N.

**Trace:**
- src/game/session.cpp:5383 `if (s.index == 0) { ReactFxAftermath; ReactFxToBlasts(w, tick, exps); }`
- src/game/session.cpp:4946 ReactFxToBlasts pushes exps with no ownership check
- src/game/session.cpp:5397 phase K: ExplosionHitsBodies on locally authored exps
- src/game/session.cpp:5834 PushLocal sends them
- src/net/opsync.cpp:265 Merge appends the remote copy, no dedupe
- src/game/session.cpp:5508 RemoteExplosionsHitOwnBodies applies it to owned bodies again

**Failure scenario:** Sodium in a pond near two connected players fires. Both machines author the blast, and every owned NPC or avatar nearby takes double damage and double launch impulse, in multiplayer only. Where the two machines' 16-slot reactFx winners differ, they blast different cells.

**Suggested fix:** When connected, author reaction blasts and flashes only for firings in chunks this machine owns (net::ChunkAuthority), inside an ExpAuthorScope with a world producer so the authority audit covers it. Consider filtering by authority before the 16-slot selection.

**Verifier's note:** I read the call site, the PushLocal path, the Merge exps append and the remote body-hit function. I did not re-trace the R1 key fix; the claim says it holds. ReactFxAftermath cell ops are also duplicated, but keep-first canonicalization makes them harmless.

## Also found by this sweep (canonical entry elsewhere)

- Draft shelter box follows each machine's local player, so peers' CA wind differs in shared chunks: see **DJ-1** in `AUDIT_draft_jets.md`.
- Draft box follows each peer's own player; sheltered CA wind differs between M9 peers: see **DJ-1** in `AUDIT_draft_jets.md`.
- Draft shelter box is centred on the LOCAL player, so CA wind differs between peers in shared chunks: see **DJ-1** in `AUDIT_draft_jets.md`.

## Coverage by kernel / file

| Kernel / file | Status | Note |
|---|---|---|
| sim_step.wgsl solute helpers (solStoreAt/solSwap/solTransfer/solOnReplace/solTryMix/solTryDissolve/solTrySink/solTryConvert) | OK | Integer; reads/writes within reach 1; pool halves written with and/or on disjoint bits; pages pre-allocated by solWant over N27 |
| sim_step.wgsl heat/elec mirror blocks (heatLapse/heatAmbient/heatX, elecAt/elecResistRaw/elecRampChance/elecDeposit) | OK | Integer, last-tick fields; heatShift releases moved pages before the CA; elec has its owner check |
| sim_step.wgsl inBounds/rngCellId/markDirtyR/solMarkFan | OK | RNG key is world-based; dirty marks are atomicOr |
| sim_step.wgsl tryMove/tryCrush/tryPowderOnto/tryFineRepose/soloSolid/canDisplace | OK | Every src is the acting cell; writes at reach 1; support flags read only at reach 1 here |
| sim_step.wgsl angle of repose (reposesnap prepass, ring ownership, reposeSnapOpen, matReposeCode, reposeDiagAllowed) | OK | Tick-start snapshot keyed by slot plus monotonic epoch; ownership is a pure function of dirtyIn; barriers correct |
| sim_step.wgsl seesSky/rainMapExposed/rainExposed/rainChance/lightMatches | OK | One-cell reads plus a same-tick snapshot map; window-top edge semantics are by design |
| sim_step.wgsl fluidOccMat/excitedReact/flagFluidConsume | GAP | Last tick's block map read under the new origin on a window-shift tick: phantom fluid (finding 2) |
| sim_step.wgsl reactWriteSelf/scaledChance/productState/carriedState | OK | Integer, reach 1 |
| sim_step.wgsl coatReact/stainDry/coatRelease/coatSpend + doReactions/doStaining neighbour writes | GAP | flagSupportLoss on the partner reads c+2d, a racy support flag (finding 1) |
| sim_step.wgsl reactFxNote/reactLiquidEaten | OK | Hash-slotted atomicMax over window-wrapped coordinates; counters diagnostic |
| common.wgsl flagSupportLoss | PARTIAL | Correct when called on the acting cell; racy when called on a neighbour (CA sites 3059/3367/3811) |
| common.wgsl page/ticket accessors, dirtyFanSlot, packVox/stain/powder, pcg/hash3, reactgate, daylightStrength, synthJitterState | OK | Integer and pure; ticket miss falls back to the aliased slot only for callers that skipped the residency test (none in range) |
| sim_rain_expo.wgsl build | OK | World-aligned texels, built in full each rainy tick before the CA |
| sim_heat.wgsl heatBegin/heatShift | OK | Releases pages whose owner left before the CA; same C_CAACTIVE gating as the CA (rest of file not audited) |
| sim_solute.wgsl solWant/solAlloc | OK | N27 paging before the CA; which page a slot gets is unobservable |
| sim_fluid.wgsl mark/alloc + sim_fluid_seam.wgsl consumeApply | GAP | Map rebuilt only after the CA; no shift or ticket invalidation (finding 2) |
| simulation.cpp EncodeTick order / PrepareHeat column table | OK | Column tiles refreshed for the current origin before EncodeTick; integer inputs |
| sim_step.wgsl reactFxNote / reaction-effect record | OK | Key and slot use world-mod-N slot coordinates (R1 fix holds); cleared every tick by fill_reactFx. The consumer is F5. |
| sim_step.wgsl doReactions | PARTIAL | Rolls keyed on rngCellId; writes reach 1. A neighbour rewrite's flagSupportLoss reads at distance 2 (F6). |
| sim_step.wgsl heatReact / heatGroundFrozen | OK | Last tick's X plus a per-column climate table; integer; heatShift releases stale pages before the CA. |
| sim_step.wgsl elecReact / E2 virtual neighbour / elecDeposit | OK | Owner-checked P; dice keyed on the slot cell; writes reach 1. |
| sim_step.wgsl doStaining / stainDry / coatReact | PARTIAL | Slot-keyed rolls, writes reach 1; the consumption and coat-partner support flags share F6's distance-2 read. |
| sim_step.wgsl liquid descent/film/bridge/canFlowAnywhere/stepLiquid | OK | All reads and writes within reach 1; the licence comes from dirtyIn; mirror and mover agree. |
| sim_step.wgsl reservoir stillWant/stillReservoir | OK | Table is a pure function of the world column and tag-checked; roll on world mod N; writes the own cell or the cell above. |
| sim_step.wgsl caWindAt/gasIntent/windLateralStart | GAP | Single-machine replay is OK. Across peers: per-player draft box (F2), window-centred storm primitives (F3), spell primitives only on the caster (F4). |
| sim_step.wgsl gas edge budget + main leave-resolve tail | GAP | Sparse workgroups decode leavers to a stale chunk when a ticket box touches the window face (F1). |
| sim_step.wgsl stepGas/stepHeavyGas | OK | Writes reach 1, salted per-cell streams; only the wind inputs are per-machine (F2-F4). |
| sim_step.wgsl windEntrain/windGrain | PARTIAL | Entrainment licence comes from primitives that exist only on the caster (F4). |
| sim_step.wgsl camask | OK | Mask, heat fold and doorbells use OR/min/max or own-page writes; pool reservation order is irrelevant. caChunkAtEdge's predicate feeds F1. |
| sim_step.wgsl calist | OK | Blocked prefix scan in dirty-list order; cursors re-zeroed per substep. |
| sim_step.wgsl main gather/caRow/caRowBits/pool | OK | Which thread runs a cell is irrelevant under the colour lattice. The tail defect is F1. |
| sim_step.wgsl caCell | OK | Per-cell private state reset; the probe path writes only own-chunk reason bits. |
| common.wgsl windAmbQ/windTerrQ/windRampQ/draftApplyQ | PARTIAL | Terrain table is world-anchored with full coverage inside the window; the draft term is per-player (F2). |
| common.wgsl windPrimAtQ/windPrimEntrainsQ | GAP | Integer, but the TickParams list differs per machine (F3, F4). |
| common.wgsl windHeatK/windHeatLiftIn/draftStackAtK | OK | Integer; heat is window-only, so differences appear only near window edges. |
| common.wgsl gasIntentW/gasLadderStep/gasRndK/windAxisFrac | OK | Integer; keyed on the cell's RNG id and substep. |
| common.wgsl flagSupportLoss (CA callers) | PARTIAL | Base function used for neighbour rewrites; the distance-2 read races (F6). |
| common.wgsl page/ticket accessors | OK | voxSlotOfCell, voxIndexAndWord, voxStore, chunkResident, slotWorldChunk and dirtyFanSlot are consistent; RNG identity uses rngCellId. |
| session.cpp ReactFxToBlasts (phase K reaction slot) | GAP | Multiplayer: every machine authors blasts, so owned bodies are hit twice (F5). |
| support.cpp SubmitTick wind/draft inputs | GAP | draftOrigin from the local player, weather primitives from the window, WindPrims from the local process (F2-F4). |
| windfield.cpp WeatherPrims | GAP | Storm primitive positions keyed on the window centre (F3). |
| sim_heat.wgsl / sim_elec.wgsl / sim_solute.wgsl / sim_draft.wgsl | UNCHECKED | Only the interfaces the CA reads were checked. |
| sim_fluid.wgsl mark/alloc | OK | atomicOr marks plus a single-workgroup slot-order scan; the box and near bits are render-only |
| sim_fluid.wgsl clearGrid/p2g1/p2g2/gridUpdate | OK | Q16.16 integer; all floats const-eval; scatters are atomicAdd/atomicMax; voxels static during substeps |
| sim_fluid.wgsl g2p | PARTIAL | sim state is deterministic; the foam word-7 saturating add is order-dependent (render-only); splash/foam RNG keyed on pool slot gi (diverges between peers) |
| sim_fluid_seam.wgsl compaction/spawnAppend | OK | slot-order scans; drain block written one thread per slot |
| sim_fluid_seam.wgsl exciteDetect/Scan/Emit | PARTIAL | pure function of pre-seam state, excite bits consumed the same tick; reads last tick's block map through the new origin after a window shift |
| sim_fluid_seam.wgsl consumeApply/cellClear/particleTick | PARTIAL | order-free atomics and a slot-order donor bid; consumeApply is stale after a shift |
| sim_fluid_seam.wgsl stainApply | GAP | consume -> flagSupportLoss reads neighbours consumed in the same dispatch |
| sim_fluid_seam.wgsl settleJudge/settleScan | PARTIAL | deterministic, but ticket slots are never judged and the ticket clash test is wrong (latent) |
| sim_fluid_seam.wgsl settleColumn/Check/Commit/Kill | GAP | no residency test, so it aliases 512 cells down; the forced reach branches on page state beyond the materialized ring |
| sim_fluid_seam.wgsl mirrorFold | OK | reads this tick's map; the CPU consumes it against the snapshot's own mirrorBase |
| sim_step.wgsl fluidOccMat/excitedReact/flagFluidConsume | PARTIAL | lattice-safe; stale map at T+1 after a shift; consume flag not consulted (latent double consume) |
| common.wgsl fluid consts/fp*/stainStep/voxWordIndex | OK | const-eval and integer only |
| pass_table.def PT_FLUIDMAP/PT_FLUID/PT_FLUIDSEAM/PT_FLUIDSETTLE | PARTIAL | R/W sets read sane; settleJudge's 128 groups do not cover NUM_SLOTS |
| simulation.cpp EncodeTick seam recording | OK | predicate uses fixed-latency Snap, pending spawns and the latch |
| simulation.cpp EncodeTick seam recording | GAP | frozen out-of-residency particles keep the seam, solver and CA recorded every tick and block the settled skip |
| support.cpp SubmitTick fluid ring / page table | PARTIAL | LatestDelivered-driven by design; the seam's PT_NO_WORD branches make that observable |
| session.cpp fluidCount estimate | OK | Snap() is exactly T-4 |
| sim_waterbody.wgsl wbDrain spawn ops | OK | one thread per reserved op, hash keyed on gid |
| sim_particle.wgsl ring commit (MPM spray/foam appends) | OK | commit-or-refuse covers the g2p appends |
| net/chunksync quiet rule vs MPM | GAP | QuietTicks ignores live MPM; ReplaceChunk does not flush the particles |
| worldio save/load of MPM | PARTIAL | documented drop of in-flight MPM; the plan's force-settle is not implemented |
| sim_heat.wgsl | OK | Integer math; slot-order prefix-sum alloc; free-stack page identity unobservable (heatSrc rewrites the only nonzero nibble); tent passes only skimmed |
| sim_elec.wgsl | PARTIAL | Single machine OK (commutative aggregates, stamps cleared by fill rows); pond cap and purge are window-global, so peers diverge |
| sim_draft.wgsl | PARTIAL | Kernel integer and race-free incl. 10-14 jets; but box origin = local player chunk, so differs per peer |
| support.cpp SubmitTick (heat-wind Q, draftOrigin) | PARTIAL | Q conversions are plain clamped IEEE double; draftOrigin from local playerChunk is not shared with peers |
| heat.cpp | OK | Integer ambient/lapse helpers; lround on per-mille chance only at load |
| windprim.cpp | OK | Hand-rounded Q16, IEEE sqrt, analytic-in-time position, 64-bit travel |
| windfield.cpp | PARTIAL | Terrain table is a pure function of world position; mirror path not read |
| world.h ElecMayBeLive / world.cpp elec hit publish | OK | Latch from ops plus fixed-latency snapshot; hits delivered in tick order |
| pass_table.def heat/elec/draft rows | OK | check_pass_table.py clean for these rows (fails only on sim_fluid fluidAlloc, out of scope) |
| mob_shock.cpp / lightning.cpp | UNCHECKED | Header and plumbing only; widest-path solve claimed order-independent, not verified |
| session.cpp TickAuthority | PARTIAL | Phase order fixed and per-session index order; only the preamble + phase list read, not every phase body |
| workpool.h/.cpp + mob.cpp ParallelFor sites | PARTIAL | 2 of 9 mob sites read: per-mob writes, merged serially in fixed order; workpool.cpp not read |
| mob_shock.cpp pool phases | OK | Goes serial if a creature repeats or under debug; per-slot/per-part writes |
| debris.cpp terrain patches | OK | Parallel shape build, sequential Jolt add; evict iterates unordered_map (same-binary deterministic) |
| physics.cpp corpse hulls cache | OK | Pointer-keyed map used for lookup/eviction only |
| support.cpp SubmitTick draft box | GAP | Box follows the local player; CA wind differs across peers (finding 1) |
| windfield.cpp WeatherPrims | GAP | Storm prims centred on window centre; window-keyed CA input (finding 2) |
| windfield.cpp terrain table | OK | World-aligned lattice padded by rMax; values window-independent where covered |
| oprecord.h/.cpp | PARTIAL | Records whole TickParams + ops, but header stamps no tuning/reaction identity (finding 3) |
| simulation.cpp PrepareHeat | OK | Tuning + world-column function; CPU float->x16 rounding on one binary |
| world.cpp elec query publish | OK | Answers delivered in tick order through the fixed-latency snapshot publish |

## Coverage (as reported by the finders)

**det: sim_step A.** Static only: nothing built or run, no files written. Commit 20aa002 in the audit-1014 worktree.

READ IN FULL: assets/shaders/sim_step.wgsl lines 1-3200, i.e. bindings and constants; the solute MIRROR block (solStoreAt, solCellValue, solFault, solRuleAllows); the heat MIRROR block (heatBlockOf, heatColWord, heatLapse, heatAmbient, heatX); the elec MIRROR block and E2 helpers (elecAt with the owner check, elecResistBase/Raw, elecCellCharge, elecRampChance, elecDeposit); markVoxActive, inBounds, rngCellId, markDirtyR, the DIRTY_M and FILM_LICENCE notes, gasThinMul, liquidSubmerged, soloSolid, canDisplace, solSwap, solTransfer, solClearStale, solOnReplace, solTryMix, solTryDissolve, solTrySink, solTryConvert, tryMove, tryCrush, tryPowderOnto, tryFineRepose, fineDiagSteep, powderSurface; the whole angle-of-repose block (reposeSnapOpen, the ring ownership including tickets, the reposesnap prepass with its workgroup barriers, matReposeCode, reposeDiagAllowed); transferLiquid, productState, cellEighths, carriedState, seesSky, rainMapExposed(Near), rainOpen, rainExposed, rainChance, lightMatches, fluidOccMat, excitedReact, flagFluidConsume, reactWriteSelf, scaledChance, the coat block (coatRuleVerdict, coatMatOf, stainDry, coatReleaseFace, coatRelease, coatSpend, coatReact), reactLiquidEaten, reactFxNote.

Targeted reads past 3200 to trace callers: doReactions 3192-3455, elecReact and heatReact 3472-3625, doStaining consumption 3760-3816, the camask tail 5420-5580, caRow/main/caCell 5800-6487.

common.wgsl helpers those call: page and ticket accessors (voxSlotOfCell, voxWordAt, voxIndexAndWord, voxWordIndex, voxStore, ticketSlotOf, chunkSlotOf, cellResident, slotWorldChunk), dirtyFanSlot, cellIndexW, inWindow, flagSupportLoss, faceDir, nbrMatches, packVox, the stain and powder-mass helpers, pcg/hash3, synthJitterState/synthWordAt, reposeEighthsOf, reactPhaseOpen/reactWeatherChance/reactWantsExposure, daylightStrength, isRayBlocker, matCanAct, stampFor.

Cross-checked: pass_table.def CA, caMask, reposeSnap and fluid rows plus the full pass order; simulation.cpp EncodeTick table order and PrepareHeat's column table; sim_solute solWant/solAlloc; sim_heat heatBegin/heatShift (window-shift release, same C_CAACTIVE gating as the CA); sim_rain_expo build (world-aligned texels); sim_fluid mark/alloc; sim_fluid_seam consumeApply; stream.cpp ShiftAxis, FillSlots and InstallChunkWords; debris.cpp QueueSupportEvents; oprecord's snapEpoch exemption; sim_compact; sim_mutate's flagSupportLossBrush/Cells precedent.

Verified OK: no runtime f32 anywhere in 1-3200. Every RNG key is rngCellId (world-based), a world position, or window-wrapped slot coordinates; none is a page index. Every write is within reach 1. All other reads are at reach 1 or come from tick-start snapshots (repose snapshot, rain map, caMask planes, last-tick heat and elec fields). Atomics are order-free or diagnostic-only. Heat and elec are safe across window shifts; solute pages are allocated before the CA.

Deliberately not reported: rain and sky exposure depend on the window top, and tickets treat cells as rain-exposed; both are window-edge or ticket semantics by design.

NOT AUDITED: stepLiquid, stepGas, the wind, draft and 2026-10-14 jet readers (windAtQ, caWindAt, windLateralStart, around 3830-5400), gasLeave and the edge budget, camask's heat source fold and sparse-pool internals beyond a skim, sim_heat beyond begin/shift, sim_elec, MPM solver internals, and the per-slot fluidCalm and excite scratch across window shifts. That last one is suspected to be the same class as finding 2 but was not traced.

**det: sim_step B.** READ AND TRACED (static only, snapshot 20aa002):
- sim_step.wgsl 3100-6486, every line: the reactFx record and reactLiquidEaten; doReactions, including the coat-as-partner and E2 charge-as-partner blocks; elecReact; heatReact and heatGroundFrozen; doStaining; cornerDir, liquidWall, cornerDescentOpen, tryDescend and canDescend; film floor, filmStepAllowed, filmPressed and bridgeLevel; canFlowAnywhere; the reservoir (stillWant, stillReservoir); stepLiquid; the WIND IN THE CA block (caWindAt, windLateralStart, gasIntent); gasOuterSplat, gasConvert and gasLeaveDefer/Allow/Key/Count with caChunkAtEdge; stepHeavyGas, stepGas and stepGasFrom; windEntrain and windGrain; camask, including the heat source fold, the sparse pool reservation and scatter, the reposeSnap publish, the doorbells and the caWind cache; calist; caRow and caRowBits; main, including the leave-resolve tail; caCell.
- Helpers those call, elsewhere in sim_step: the solute, heat and elec MIRROR blocks; inBounds, rngCellId and markDirtyR; FILM_LICENCE; gasThinMul; soloSolid; canDisplace; solSwap, solTransfer, solOnReplace, solTryMix (including the 10-11 sink-into swap), solTryDissolve, solTrySink and solTryConvert; tryMove and tryCrush; matReposeCode and transferLiquid; productState and carriedState; seesSky; the rain map and rainChance; lightMatches; fluidOccMat, excitedReact, flagFluidConsume and reactWriteSelf; scaledChance; the coat block, stainDry and coatReact.
- common.wgsl: draftApplyQ and the jets index; windHeat* and draftStackAtK; windTerrQ, windRampQ, windAmbQ, windPrimAtQ and windPrimEntrainsQ; gasRndK, windLateralStartW, windAxisFrac, gasIntentW and gasLadderStep; the ticket probe, chunkSlotOf, chunkResident, slotWorldChunk and dirtyFanSlot; voxSlotOfCell, voxWordAt, voxIndexAndWord and voxStore; synthWordAt; nbrMatches; flagSupportLoss.
- CPU side, as far as the CA inputs lead: support.cpp SubmitTick (draft box, WindPrims copy, AppendWeatherPrims, the authority audit); the windfield.cpp terrain table (world-anchored, full coverage inside the window) and WeatherPrims; simulation.cpp SetDraft; buffer sizing of caWind and fluidBlockMap (both kNumSlots); session.cpp ReactFxToBlasts, ReactFxAftermath, phase K blast body pass and RemoteExplosionsHitOwnBodies; opsync Merge; tickets.cpp placement, recentre and landing requests; world.cpp reactFx decode; pass_table fill_reactFx; heat.cpp/sim_heat heatShift (pages released before the CA); ElecMayBeLive (fixed-latency snapshot).

CHECKED CLEAN:
- The R1 fix holds: reactFxNote keys slot and key on world-mod-N slot coordinates, and the record is cleared every tick (C_ALWAYS).
- No runtime f32/f16 in the range; only const-eval rounds.
- Every RNG in the range is keyed on rngCellId or world coordinates, or on cellIndexW (world mod N) for the reservoir.
- Every write reaches at most 1 cell, apart from the defects reported.
- Order-sensitive atomics in the range are cursors whose order nothing consumes (pool reservation, wgCaSeg gather, HM_RELAX list, gasSpawn records) or commutative reductions (OR, min, max, add).
- The elec and heat pages carry owner checks across window shifts.
- The still-water table is a pure function of the world column (tag-checked).
- gasThinMul reads the substep-start mask.

NOT COVERED:
- sim_step.wgsl 1-3100 beyond the helpers above (tryFineRepose, the reposesnap prepass body, tryPowderOnto).
- The producing kernels: sim_heat (heatRelax/target), sim_elec, sim_solute (solAlloc/solCompact), sim_draft (the solve itself), sim_gas (rank placement), sim_particle and sim_fluid. Findings 2 and 3 likely extend to their windAtQ reads.
- sim_mutate and sim_explode.
- Whether a refilled slot inherits the departed chunk's dirtyIn reasons (the film licence on the first tick after a shift).
- Worldgen `cols` coverage of the still-water table for columns loaded only from the store.
- The unexplained 10-11 solute store fault: no unguarded solStoreAt was found in this range.

**det: MPM fluid.** READ IN FULL AND TRACED: assets/shaders/sim_fluid.wgsl (all 1953 lines): const block, mark, alloc, clearGrid, p2g1, p2g2, gridUpdate, g2p incl. splash/foam. assets/shaders/sim_fluid_seam.wgsl (all 2873 lines): solute mirror, compaction, spawnAppend, exciteDetect/Scan/Emit, consumeApply, cellClear, particleTick, seamReactWake, stainApply, settleJudge/Scan/Bin/Column/Check/Commit/Kill, mirrorFold. pass_table.def: the PT_FLUIDMAP/PT_FLUID/PT_FLUIDSEAM/PT_FLUIDSETTLE rows plus the worldgen/genlist/loadreset fills. common.wgsl: the FLUID_* consts (all const-eval), FluidParticle/fp* accessors, the FA map, slot/ticket addressing (voxSlotOfCell, voxWordIndex, chunkSlotOf, dirtyFanSlot), flagSupportLoss, stainStep/stainFires/stainConsumes, synthWord/JITTER, currentAtQ, windAtScaledQ/windDragRampQ. sim_step.wgsl: fluidOccMat, excitedReact, flagFluidConsume, the PAIR/coat scans that synthesize excited fluid, and the caRow sentinel skip. CPU side: simulation.cpp EncodeTick seam recording, NoteTickInputs/NoteSnapshot/settled latch and the seam bind groups/ping-pong; support.cpp SubmitTick fluid clamps, page-table contributors (UpdateFluidChunks, LatestDelivered), EncodeTick call, FlipPage; session.cpp fluidCount estimate, mpm tool, clear-fluid button; world.h/world.cpp snapshot fluid parse and Snap() fixed latency; stream.cpp Update/ShiftAxis/CompleteDueShifts/ApplyGenVerdict/HarvestDemotes/RevisitFullChunks; pagetable.cpp UpdateFluidChunks/Materialize/ConsumeOccupancy; net/chunksync.h quiet rule; sim_waterbody.wgsl wbDrain spawn block; sim_particle.wgsl ring commit-or-refuse (it does cover MPM spray/foam appends).

CONFIRMED OK: no runtime f32 in either fluid kernel. Every sim.fluid*/wind float is folded at const time, including the `let kd`/`let buoy` const-expressions in g2p. P2G is i32 atomicAdd/atomicMax (wrapping sums are associative). Compaction, spawn and excite slot assignment are slot-order scans. Excite bits 19-23 are set by detect and consumed by emit for accepted AND refused slots in the same tick, and detect and emit share C_CAACTIVE. The seam recording predicate reads only fixed-latency Snap() plus pending spawns. The page flips every tick and the seam is recorded whenever particles exist, so the ping-pong stays aligned. exciteDetect's sentinel skip is content-equivalent. FA_LASTSLOT's last-writer race is audio-only.

NOT AUDITED / NOTES FOR THE OWNER: windAtQ internals and sim_draft.wgsl (only confirmed they are tick-synchronous integer inputs); the raymarch MPM surface beyond the foam read; debris.wgsl vsFluid; lab/fluid-bench scenes; container.cpp pour float math (CPU op authoring, carried in the op record); the rest of the net code. Noticed but not escalated: (1) fluidOccMat ignores the consume flag, so after a prodNbr==air reaction a later colour pass can consume the same excited bin again. That is latent: no current rule turns a seam-liquid neighbour into air while also producing mass. (2) The settle column walk re-packs existing solute-carrying water without moving its solute (the seam never calls solStoreAt); not traced to a concrete case. (3) world.h:926-927 drops MPM on save/load, while PLAN_mpm_fluids §5.3 says force-settle; this is documented, so not filed separately, but it loses excited water mass.

**det: heat/elec/draft kernels.** Read and traced, statically at 20aa002. sim_heat.wgsl: header, list/alloc helpers, heatShift, heatPend, heatRelax; every atomic grepped. I checked the free-stack identity argument: page contents are zero except plane 0's top nibble, which heatSrc rewrites, and the relax free path checks planes 1-2 fully. Alloc rank is a prefix sum in slot order. Lists are append order only. heatRelax's neighbour summary reads are stable because a non-freeing store preserves HS_SOURCE. sim_elec.wgsl: header; the elecAlloc head rekey, scan, pond cap and tail purge; elecSweep; the cell cache and its tick+1 stamp (the WORLDGEN and LOADRESET fill rows zero elecPool including the stamps, so no stale-cache hit across world resets); elecSettle and elecPurge; the ElecMayBeLive latch (world.h:5238, all snapshot or op driven, fixed latency); the elec-hit publish path in world.cpp PublishSnapshotsUpTo. sim_draft.wgsl: header determinism argument, cellMasks/maskDirty, args, and the full 2026-10-14 jetPrep/jets march. Barrier placement is correct (C/0 -> barrier -> A -> barrier -> B -> barrier); jetSqrt is integer and the march is integer only. Also read: the C_DRAFT tail logic and caActive/settledProven in simulation.cpp:3975-4160; heat.cpp (PrepareHeat helpers, which use integer math and lround of a per-mille only); windprim.cpp Q16/WindPrimAim (IEEE sqrt is correctly rounded, so exact) and WindPrimResolve; windfield.cpp terrain table (values are a pure function of world position on a toroidal 64x32-voxel table covering 2048 voxels, a superset of the window; atan2/sqrt there are display only); the SubmitTick heat-wind Q conversions in support.cpp:1495-1540 (plain IEEE double, clamped, deterministic). Ran scripts/check_pass_table.py (static, reads files only): the heat, elec and draft rows match their bindings. It FAILS on sim_fluid.wgsl:alloc (row fluidAlloc reads T and pageTable undeclared, so a missing RAW barrier). That is MPM and outside my assignment, so I flag it here for the MPM agent rather than as a finding. Considered and dropped: heat and elec pages are neither saved nor transferred on resync or join, so a late joiner's X/P history differs transiently. This is documented as intended ('one path for load, streaming and peers', PLAN_temperature.md:167-171 and section 9). NOT covered: heat tent passes (heatTent x/y/z, lines ~800-925) and heatDue were only skimmed via grep. The elecRound halo build (lines 800-860) and elecQuery (the body box query) were not read in detail. coarseBuild/coarseFaces/coarseSolve/fine passes of sim_draft were checked only via grep for atomics: the atomics are commutative, and the packed-key atomicMax uses ties broken by label, which is deterministic. elec.cpp, lightning.cpp and mob_shock.cpp were read only at the header and plumbing level. The windfield.cpp mirror (MirrorLocal etc.) was not read.

**det: CPU tick + op stream.** This was a static, reduced-effort pass on snapshot 20aa002. Prior-art first: I read the PLAN_vulkan_port.md 2026-10-02 audit table (lines 805-844).

(1) TickAuthority, session.cpp 6151-6310: read the phase sequence and the stun/dialogue preamble. The workpool.h contract was read. I sampled two ParallelFor bodies in mob.cpp: PrecomputeBurnWalks at 21150 (per-mob writes only) and the splatter pass at 23660-23720 (per-mob SplatSink, then merged serially in (event, mob, group) order). mob_shock.cpp's pool guard at 366-421 was also read; it runs serial if a creature repeats or debug is on, and its pointer sort is used only for uniqueness. I did NOT read the other mob.cpp ParallelFor sites (16179, 21372-21415, 23494-23604) or workpool.cpp itself.

(2) Grepped game/phys/sim for unordered_* declarations, range-for over unordered members, rand/random_device/mt19937/time seeds (none found) and steady_clock (all are profiling or printf-only). Checked: debris terrain_ eviction loop (7279: unordered order, but same-binary deterministic; Jolt add order is kept sequential), the physics hulls pointer-keyed map (eviction only, benign), and the debris island-trace static (diagnostic only). I did not audit std::sort comparators for tie instability.

(3) Read oprecord.h in full and the oprecord.cpp header read/validation. In SubmitTick (support.cpp 1146-2090) I read the uploads outside the record: heat params, elec knobs, elec queries (read-only, answers via the fixed-latency publish, world.cpp 1597-1640), and far plumes (render). I did not read CanonicalizeCells or the clamp code itself.

(4) Window dependence: checked windfield FillWindField/EnsureTable (the terrain table is world-lattice aligned and padded, so OK), WeatherPrims (finding 2), the draft box (finding 1) and PrepareHeat's column table (world columns, OK by inspection).

Not covered: flowfield (assigned to another agent), the chunk tickets/stream, dungeons, electricity GPU kernels, the mob AI's use of unordered containers, and net/opsync merge order.

