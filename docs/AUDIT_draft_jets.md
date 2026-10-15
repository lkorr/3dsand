# Audit: draft jets: indoor wind keeps its momentum (908dfb8)

2026-10-14. Overnight adversarial **static** audit: no builds, no runs. One finder agent read the landing commits and traced the code paths. Every HIGH or MED finding went to a separate verifier agent told to refute it by reading the same code; **only findings the verifier CONFIRMED are listed here** (unconfirmed leads and unverified LOW findings are in `AUDIT_INDEX.md`). Line numbers are at `20aa002` (locked worktree `.claude/worktrees/audit-1014`, branch `audit-1014`).

Severity: HIGH = a wrong outcome a player or peer can hit, a crash, or a determinism / persistence hole; MED = wrong in a narrower case, or a stated guarantee not kept; LOW = latent, edge-case, or diagnostics.

Budget note: the run was re-planned mid-way to cut token use. Finders ran at max effort for the ten systems, sim_step and the MPM fluid, and at high effort for the remaining four sweeps; verification ran in batches (one verifier per finder, effort high) and covered HIGH and MED findings only. LOW findings were not verified and are listed, as unverified leads, in `AUDIT_INDEX.md`.

## Summary

| # | Sev | Finding | Where | Fix size | Owner call | Status |
|---|---|---|---|---|---|---|
| DJ-1 | HIGH | Draft box origin comes from each peer's local player and is not on the wire, so shared chunks diverge | `src/test/support.cpp:1895` | medium | yes | fixed (owner decision), gates pending `b33d2ab` |
| DJ-2 | MED | Jet cover test counts a cell's own floor, so outdoor partly-filled ground cells carry jets | `assets/shaders/sim_draft.wgsl:1280` | small | no | fixed, gates pending `d8e4b9c` |
| DJ-3 | MED | LoadWorld never forces a draft rebuild, so pre-load shelter and jet fields stay live | `src/sim/worldio.cpp:1021` | trivial | no | fixed, gates pending `1f9e85b` |

## Findings

### DJ-1 (HIGH) Draft box origin comes from each peer's local player and is not on the wire, so shared chunks diverge

**Status:** FIXED on branch `audit-1014` in `b33d2ab`. Owner decision 2026-10-14: two players must see the same wind, and the box must not be centred on each machine's own player. The draft boxes now belong to the SESSION: one per player on a fixed world lattice (the even-chunk snap, no per-window clamp), each machine authors its own player's box on its op batch (OpBatch::drafts, kOpsWireVersion 4), and every peer solves the same merged list (host first) at the label tick; single-player hands its one box over at once. kDraftMaxBoxes = 2 (host + one client; past the cap the canonical first two win), so the draft buffer doubles to 30.5 MB. Box cells outside a machine's window are masked as open air, the same window-edge limit the CA has (chunksync repairs it). The list is in TickParams, so ops-replay records it.

**Where:** src/test/support.cpp:1886-1903 (`tp.draftOrigin` from `playerChunk`), src/net/protocol.h:214-225

**What:** Each machine computes the draft (shelter) box origin from its own primary player's chunk and clamps it into its own window. Nothing on the wire carries the origin. The CA (gas and powder drift through `caWind`), particles and MPM all read the box through `windAmbQ -> draftApplyQ`. So two peers that share chunks solve different boxes and push the same smoke and grains differently. The jets make the indoor transfers larger, so the gap between machines grows.

**Trace:**
- src/game/session.cpp:5664: `pc` is the local session's player chunk, passed to the submit
- src/test/support.cpp:1895-1901: `place(playerChunk.*, ..., w0.*)` gives draftOrigin, then `sim.SetDraft`, then `tp.draftMode`
- src/net/protocol.h:214-225: TickBatchHeader has no draft origin (no hit for draftOrigin in src/net)
- assets/shaders/common.wgsl:4204: `windAmbQ` returns `draftApplyQ(p, amb, T)`
- assets/shaders/sim_step.wgsl:5657: the caWind cache reads `windAmbQ`, which drives gas and powder drift (hashed)
- DESIGN.md:28359-28372: every client runs the identical CA on its own window from the same records

**Failure scenario:** Host and client stand a few metres apart in one longhouse with a fire. Their boxes snap to different even-chunk origins, so R and the four jet transfers differ in every shared cell. Smoke, ash and sand drift differently on each machine. Once the chunks go quiet their digests mismatch and chunksync re-sends them, giving a visible pop and chunk traffic every time something moves there.

**Suggested fix:** (a) The authority decides the box origin and carries it in TickBatchHeader (or the op record), and every peer solves that box. (b) Solve one box per player in a canonical order on every machine. (c) Force sim.draftMode 0 in networked sessions until (a) or (b) lands.

**Owner call:** Which box a networked session solves (host's, per-player, or none) is a design decision, and it affects wire format and cost.

**Verifier's note:** I checked the placement code, the wire header, the grep across src/net, the shader read path and the default draftMode = 1. The issue predates the jets (drafts landed 2026-09-30). The divergence is repaired by chunksync rather than left silent, which is why this is not worse, but it is real interior drift between peers.

_Also reported independently by: AUDIT_determinism.md (det: sim_step B); AUDIT_determinism.md (det: heat/elec/draft kernels); AUDIT_determinism.md (det: CPU tick + op stream)._

### DJ-2 (MED) Jet cover test counts a cell's own floor, so outdoor partly-filled ground cells carry jets

**Status:** FIXED on branch `audit-1014` in `d8e4b9c`. Confirmed: jetPrep set cov from the cell's own +y face before assigning the cell's flag. That face ORs both cells' masks, and the masks carry no height inside a cell, so a partly filled outdoor ground cell (12-15/16 columns) was flagged ELIG under open sky. I did not simply move the cov update after the flag (the audit's suggestion), because that would also strip CLOSED from a full roof-slab cell and lose the documented 'smoke under a ceiling rides the jet' rule. Instead, a cell's own face now covers the cells below it, and covers the cell itself only when the cell holds no air (CLOSED). A full ground slab outdoors stays inert as before, because its neighbours' jv is 0. check_shaders passes.

**Where:** assets/shaders/sim_draft.wgsl:1280-1287 (`jetPrep` cover test)

**What:** `jetPrep` marks a cell covered when its own +y face is at most 4/16 open, and it tests that before assigning the cell's flag. A face's openness counts a y-row as blocked if any voxel in either cell blocks it, so ground at the bottom of a cell blocks that cell's own top face. An outdoor ground-surface cell with ground in 12-15 of its 16 columns (anything short of the 16/16 slab rule) is therefore flagged covered air (ELIG), and the jet march carries momentum along the ground layer under open sky.

**Trace:**
- assets/shaders/sim_draft.wgsl:241-251: `my |= 1u << (lx + 4u*lz)` for a blocker at any ly
- assets/shaders/sim_draft.wgsl:260-262: the cell is solid only if a mask is 0xFFFF
- assets/shaders/sim_draft.wgsl:397-410, 479-480: KBASE +y = countOneBits(~(mask(g) | mask(g+y))) * 16
- assets/shaders/sim_draft.wgsl:1280: `if (on && kGet(kb,1u) <= JET_ROOF_K) { cov = true; }` runs before `fl` for g
- assets/shaders/sim_draft.wgsl:1286: `fl = JET_ELIG` when the cell has any open row
- assets/shaders/sim_draft.wgsl:1427-1444: `T = jetPick(sf, c)`, plus the turn along the closed face ahead
- assets/shaders/sim_draft.wgsl:1193-1204: the stated contract that uncovered cells keep T = S

**Failure scenario:** Wind along +X over sloped terrain toward a boulder or wall, no roof. The surface cells along the wind are ELIG. In front of the obstacle the projection's forward flow falls while the carried c does not, so T = c instead of ±R, and the blocked share is turned along the face. Dust and smoke in those cells' few air voxels move by this wind through `caWind`, so outdoor hashed state departs from the projection. That brings back the stagnation-before-a-wall error the cover rule exists to exclude.

**Suggested fix:** Update `cov` after assigning g's flag, so a face covers only the cells below it. The other option is to test the face of g+y against the cell above's own mask only. Add a drafts-gate arm with a partly filled ground layer under open sky that asserts T == ±R there.

**Verifier's note:** I traced the mask, the face coefficient, the ordering in jetPrep and the march. Flat ground that fills a whole cell layer is a slab (solid/CLOSED), so the issue needs partial surface cells, which are common on any slope or uneven ground. The drafts gate uses a full stone pad, so it cannot see this. I did not quantify the magnitude, because `keep` and the face shares damp c.

### DJ-3 (MED) LoadWorld never forces a draft rebuild, so pre-load shelter and jet fields stay live

**Status:** FIXED on branch `audit-1014` in `1f9e85b`. Confirmed: LoadWorld replaced the window without forcing a draft rebuild, so under an unmoved box the pre-load shelter and jet fields stayed live. That made the post-load trajectory depend on what ran before the load. Simulation::ForceDraftRebuild is already public (worldgen_run.cpp calls it), so no simulation.* edit was needed. It is now called in LoadWorld after ReloadWindow, and at the two other whole-window replacements in main.cpp: the map teleport (main.cpp ~15549) and the net late-join ReloadWindow (~11855).

**Where:** src/sim/worldio.cpp:1015-1027 (LoadWorld grid replace)

**What:** LoadWorld replaces the whole window but never calls `sim.ForceDraftRebuild()`. If the reloaded player sits in the same even-chunk box snap, SetDraft sees no move and does not burst. The CA, particles and MPM keep reading the pre-load world's shelter R and jet transfers until an incremental solve publishes, about 6 ticks later (more if a solve was in flight). sim_draft.wgsl:332-336 promises that a load bursts. worldgen_run.cpp:62-71 already fixed the same bug class for worldgen.

**Trace:**
- src/sim/worldio.cpp:1020-1024: InvalidateSnapshot, ReloadWindow, EncodeLoadReset (no draft force)
- src/sim/simulation.cpp:3543-3556: EncodeLoadReset resets wake, heat and glow only
- src/sim/simulation.cpp:3405: `draftRebuild_ = on && (draftForce_ || moved || !draftLastOn_)`
- src/sim/simulation.cpp:991, 1672 and src/sim/worldgen_run.cpp:71: the only places that set draftForce_
- assets/shaders/sim_draft.wgsl:345-352: without a burst, an in-flight solve continues and publishes its pre-load snapshot

**Failure scenario:** The player saves inside a hut, the hut burns down, and the player reloads without leaving the box snap. For several ticks the restored hut's smoke is driven by the roofless (outdoor) transfers. The post-load trajectory also depends on the draft buffer and stage word left by whatever ran before the load, so loading the same save after different sessions diverges. That is a reproducibility hole on load.

**Suggested fix:** Call `sim.ForceDraftRebuild()` once in `Simulation::EncodeLoadReset`. That covers LoadWorld and any other caller of the load reset, such as the map-teleport path. Also call it on the net late-join ReloadWindow.

**Verifier's note:** I confirmed that no load-path caller reaches UploadTables or ForceDraftRebuild. The only main.cpp UploadTables call (main.cpp:15232) is the material hot-reload. The bug is not jets-specific, but the jet field is now part of what goes stale.

## Guideline 5 for this system

| Property | Status | Note |
|---|---|---|
| persists | PARTIAL | Derived and never saved. A burst rebuilds it on worldgen/map swap (worldgen_run.cpp:71), a box move or a knob change (simulation.cpp:3405), but LoadWorld forces no burst (worldio.cpp:1015-1027), so a load with an unmoved box keeps the pre-load T for ~6-11 ticks. |
| deterministic | PARTIAL | Kernels are integer, barrier-separated, atomic-free, one writer per word (sim_draft.wgsl:1305-1456). The field becomes history-dependent after an in-session load, and the box is per-machine in multiplayer (support.cpp:1886-1903, session.cpp:5664). |
| sleeps when idle | OK | Both rows ride fineLast's stage-6 record (pass_table.def:471-474), which dispatches zero groups unless a solve publishes (sim_draft.wgsl:377-382). Nothing is recorded after the C_DRAFT tail (simulation.cpp:4151-4156), and the rows write no dirty marks. |
| cpu mirror | GAP | Documented open item (RESEARCH_wind.md:1176-1177): windfield.cpp's CPU mirror (debris, corpses, trees, the F1 readout) reads the ambient field and sees neither R nor the jets. |
| mutation queue | N/A | Writes only the derived draftField buffer (sim_draft.wgsl:1293-1301, 1362-1364), never a voxel. |

## Coverage (as reported by the finder)

**draft jets.** Static only; no builds or runs; every path cited is at 20aa002 in the frozen worktree.

READ AND TRACED:
- The whole 908dfb8 diff: sim_draft.wgsl jetPrep/jets, the common.wgsl readers, the world.h layout, the pass_table.def/.h/.cpp rows, Simulation pipelines and NoteDraftSolverKnobs, SubmitTick's knob notes, the tuning rows, the check_invariants/check_pass_table additions and the selftest_wind.cpp jets arms.
- All of sim_draft.wgsl (masks, the args pipeline, coarse and fine solves, fineLast's publish), so I know exactly what jetPrep reads and when.
- The common.wgsl drafts and heat blocks, plus windAmbQ, windAtQ and windAtScaledQ.
- Every caller of draftApplyQ and draftApplyF. The sim consumers are sim_step caWind/gas intent, sim_particle and sim_fluid. draftApplyF is reached only from render windAt (wind_streak, debug_wind; cloud is unbound), so the float path never reaches hashed state.
- The draft-block pass rows, the consumers' R(Draft) and the LoadReset table.
- Buffer creation, SetDraft, the C_DRAFT tail, EncodeLoadReset and ReloadShaders.
- SubmitTick's wind block and box placement, session.cpp's pc, LoadWorld, worldgen_run, the F5/F10/teleport/late-join reload paths and the TickBatch header.
- DESIGN 9b (Drafts and Jets) and RESEARCH_wind 14.4, 14.10 and 14.12.

VERIFIED BY HAND, NO DEFECT FOUND:
- Prep X/Z layouts against the jets reads, and the slice-major output against draftJetIndex for all four directions.
- The kDraftWords and check_invariants constants (1982468 / 3031044).
- Both rows touch only binding 46 plus the indirect record, so the pass-table sets are complete.
- Dispatch extents: 256 groups covers the 16 groups jetPrep needs and the 4 jets needs.
- Barriers separate every jf/jv write from every read, the loops containing barriers are uniform, and one thread owns each output word.
- No atomics anywhere in the jets.
- Integer bounds hold: e < 2^23, c <= 32752, lat*lost < 2^31, and reader products < 2^31.
- The -R-by-sign pick reproduces R*ax to the bit, and draftJet 0 is an exact identity.
- Rule 2 holds: both rows ride the stage-6 record, so zero groups run unless a solve publishes.

NOT FILED (by design, or no traced wrong outcome):
- jetPrep reads the LIVE masks for its air flag while R and the faces come from the snapshot. That contradicts sim_draft.wgsl:323-325, but the effect is neutral-to-helpful.
- Any roof opening of 0.4 m or more (a smoke hole) uncovers its column and cuts a jet that crosses under it.
- Dead-end openings carry the inflow half of the mouth exchange inward with no return flow. The commit's own door-only hut went 0.035 -> 0.139; wide mouths and porches against a wall were not measured.
- The vertical `open` term is taken from the sign-picked transfers, so it can step when amb.x crosses 0 indoors.
- A ~1-ULP lateral component of S decides which way a fully blocked jet turns.
- If F5's ReloadShaders fails ('kept old'), NoteDraftSolverKnobs records the new knob values but bursts with the old constants (dev-only).

NOT AUDITED:
- The drafts gate's CA/smoke, sleep and purity arms beyond the jets lines.
- Register and spill cost of the 1024-thread march.
- wind_streak and debug_wind beyond the reader.
- check_shaders.sh and the tuner UI.
- Other window-keyed wind inputs in multiplayer: windfield::WeatherPrims takes the window origin; the terrain table looked world-aligned.
- Magnitudes in findings 2 and 3 are reasoned from the code, not measured.

