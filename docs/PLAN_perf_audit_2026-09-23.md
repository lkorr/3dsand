# Perf / tech-debt audit — implementation packages (2026-09-23)

**Status (2026-10-02 audit): LANDED on main 2026-09-24** — P1–P7 merged into
`perf-audit-int` (seven worktree merges, integration fix `5031f41`) and that onto
main at `87fee29`. Items the packages marked "analysis only, no code" stay as
written below.

Orchestrated: one Opus worktree agent per package, the orchestrator reviews diffs,
merges onto an integration branch, rebaselines ONCE at the end. Findings came from a
read-only audit; every line number is from code reading at main `baa2e1b` and may
have drifted — re-find by content. Items marked (suspected) must be CONFIRMED in the
code before you change anything; if a finding turns out wrong, skip it and say so in
your report. Do not invent new gameplay mechanics beyond what an item says.

## Shared rules for every package (read all of this)

1. Read `CLAUDE.md` (repo root) first. Rules 1–3 are inviolable. Memory notes live
   in `C:/Users/Luke/.claude/projects/C--Users-Luke-Desktop-programming-3d-sand-voxel/memory/`
   (index `MEMORY.md`) — read the ones relevant to your files before editing.
2. **Stay inside your package's file list.** Other packages run concurrently in other
   worktrees. If an item needs a file outside your list, do the minimum there, keep
   the hunk small and self-contained, and name it in your report. Never reformat or
   reflow code you are not changing.
3. **GPU lock protocol (mandatory):** every `scripts/build.sh` / `scripts/run.sh`
   invocation uses `run_in_background: true` or a Bash timeout ≥ 600000 ms. Never
   `taskkill` by image name — only PIDs you started. Max 2 retries of a GPU command;
   `bash scripts/svlock.sh` shows who holds a lock (held = queue, not failure). Expect
   multi-minute queues: six packages build concurrently.
4. `export SANDVOX_NO_CRASH_DIALOG=1` in every shell. Read `crash.log` after a crash
   (check its frames name YOUR worktree, see CLAUDE.md sccache note).
5. **Verification budget (owner directive):** NO `--suite acceptance`, NO bare
   `--selftest`, NO `--vk-smoke*` unless the question is "where did the image move".
   Finish with ONE `--verify <gates>` boot covering what you touched. Static checkers
   (`scripts/check_shaders.sh`, `python scripts/check_invariants.py`,
   `python scripts/check_pass_table.py`) are free — run them per edit.
   Main carries inherited red gates (memory: gotcha-main-has-11-unrecorded-regressions,
   gotcha-spells-gate-red-at-clean-head, gotcha-limb-alias-dies-in-verify-subsets);
   a gate that is red for reasons unrelated to your change is not yours — confirm with
   the base-arm below, don't chase it.
6. **Hash protocol.** Each item is tagged `[neutral]` (must NOT move the world hash) or
   `[moves]` (may move it — that is fine, do NOT rebaseline, do NOT investigate).
   To prove `[neutral]` items: record the `--selftest --gate determinism` hash of your
   UNCHANGED base first (C++ package: `bash scripts/build.sh` once at the start — sccache
   makes it mostly cache hits — then run the gate; WGSL-only package: run main's exe
   with `SANDVOX_ASSET_DIR` pointing at your untouched worktree), save it to a file, and
   compare after your changes. If a package mixes neutral and moving items, land the
   neutral ones first, check the hash, commit, then do the moving ones.
   The twice-run comparison inside the gate failing is a real bug — stop and report.
7. **Never run `--rebaseline`.** The orchestrator rebaselines once on the integration tree.
8. WGSL-only package: do NOT build. Use
   `SANDVOX_ASSET_DIR="$PWD/assets" bash "C:/Users/Luke/Desktop/programming/3d sand voxel/scripts/run.sh" "C:/Users/Luke/Desktop/programming/3d sand voxel/build/Release/sandvox.exe" ...`
   from your worktree. Check the first stdout line names your assets dir.
   **`common.wgsl` edits cost a ~9 min pipeline recompile on next run** — batch them,
   and put constants only one shader needs in that shader.
9. Update `DESIGN.md` in the same commit if you contradict it, and fix stale comments
   you touch. Do NOT edit `assets/tuner.html` ARCH_NODES (orchestrator does it once).
   Do NOT edit `CLAUDE.md`.
10. Commit in your worktree branch with clear messages (several commits fine). Leave the
    tree clean. Do not merge into main.
11. **Report** (your final message): per item — done / skipped (why) / partially;
    `[neutral]` proof (base hash vs after hash); gates run and results; any file touched
    outside your list; anything you found that is wrong in this plan. Keep it factual.

---

## P1 — CA rules + CA kernel cost  (WGSL + JSON only, NO BUILD)

Files: `assets/shaders/sim_step.wgsl`, `assets/shaders/sim_explode.wgsl`,
`assets/shaders/sim_mutate.wgsl`, `assets/shaders/sim_gas.wgsl`,
`assets/materials/reactions.json`, `assets/materials/materials.json` (only if needed),
`common.wgsl` ONLY for the shared dirty-mark helper (item 1). You also own the
`markDirtyR` copies in `sim_particle.wgsl`, `sim_fluid_seam.wgsl`, `sim_waterbody.wgsl`
(replace them with the shared helper, touch nothing else in those files — P2 owns them).

Hash-neutral first:
1. `[neutral]` **Dirty-mark loop does 8 same-address atomics for interior cells**
   (`sim_step.wgsl:~144 markDirtyR`). For an interior cell xs/ys/zs are {0,0}, so all 8
   iterations compute the same chunk. Iterate only the distinct offsets (1/2/4/8). Put
   ONE copy in `common.wgsl` and delete the copies in sim_step, sim_mutate (~72, marks two
   buffers — keep that semantics via a parameter or a second helper), sim_gas (~517),
   sim_particle (~201), sim_fluid_seam (~235), sim_waterbody (~424). Check each copy's
   exact semantics before unifying (buffer, reason bits, markVoxActive).
2. `[neutral]` **Sentinel chunks run the full CA** (`sim_step.wgsl:~2388-2473`). A dirty
   PT_EMPTY chunk, or UNIFORM/JITTER of a material that `!matCanAct`, is provably a no-op
   (confirm this claim against soloSolid/lone-solid logic and every rule that can fire on
   such a cell — including rules triggered by a NEIGHBOUR chunk's content, which read
   across the face: an early-out must only skip cells of THIS chunk, and only when no
   in-chunk cell can act). Load `pageTable[ci]` once, workgroup-uniform, early-return.
   Keep it in the CA, not in compact (reposeRingOwned derives ownership from dirtyIn).
   If you cannot prove it no-op for every rule kind (light-gated, PAIR with neighbour
   across the face, gas, stain), narrow it to the cases you can prove, and say so.
3. `[neutral]` **Dead `canFlowAnywhere` after failed stepLiquid** (`sim_step.wgsl:~2532`).
   Audit claim: after `stepLiquid` fails, `canFlowAnywhere` re-checks the same stages
   against the same state and always returns false, so `DIRTY_R_FLOW` is never set.
   VERIFY this (stages identical? any state change in between?). If true, delete the call
   and document `DIRTY_R_FLOW` as retired (common.wgsl doc mention — batch with item 1).
   Keep the viscous off-tick use (~2515).
4. `[neutral]` **Redundant neighbour/page lookups**: `tryMove` resolves the source index
   again although `main` has `idx`; `doStaining` re-reads `selfWord` (~1331); the caller of
   stepLiquid re-reads `lw` (~2521); `sim_mutate.wgsl` `cells` reads the same word twice
   (~209/225); `hash3` computed before `lightMatches` (~1112) — move after the gate if
   the RNG draw doesn't need to be consumed (stateless hash, so it doesn't). Add a
   helper returning index+word from one page-table resolution where it helps.
   Do NOT attempt the full 27-entry workgroup page cache unless the above is done and
   you have budget — if you do, it must be exact.

Behaviour changes (`[moves]`):
5. **Two stains on one cell never settle** (`sim_step.wgsl:~1372-1461`). Non-washing
   stainers (blood, oil, ichor) overwrite foreign stains; water steps foreign stains down
   by 1 → blood+water loop forever; blood vs ichor / oil vs blood overwrite each other;
   each event also rolls `consume` (6/6/4 per mille) → the surface erodes and the chunk
   never sleeps. Fix with a strict order: a non-washer only stains clean or same-type
   cells (so "blood recolours wet ground" goes away — accepted). Make sure the wash path
   still terminates.
6. **Grass self-matches its own soil rule** (`reactions.json:~888`, self grass,
   neighbor `tag:soil`, neighborBecomes grass; grass carries `soil`). Grass→grass rewrites
   re-dirty the chunk (DIRTY_R_REACTW) and grant FILM_LICENCE every daylight tick.
   Fix on the GPU: a PAIR match whose neighbour product equals the current neighbour
   material and whose self product is keep/self is skipped (no write, no mark). Check no
   other rule relies on the no-op firing. The spread onto sand/ash/dirt/mud is existing
   content — leave it.
7. **Fungus spreads without bound** (`reactions.json:~703, ~918`): no decay, converts every
   `tag:organic` neighbour incl. grass (which regrows). Rule 2 requires a bound. Use the
   least-invasive bound that makes expected offspring < 1 over a fungus voxel's life —
   e.g. a slow decay rule to an existing material (dirt/ash) — and document the math in
   the rule's note. No new materials.
8. **Hanging vines keep chunks awake + create flowers from nothing**
   (`reactions.json:~688-698`: vine emits flower sideways at 2‰ with vine unchanged,
   marking keep-awake whenever a side is air). Bound it the same way (e.g. the emit
   consumes/ages the vine, or only fires with a light/neighbour condition that settles),
   minimal change, documented. `source_*` materials are intentionally permanent — leave.
9. (suspected) **Stain depth on stone capped at 1** (`sim_step.wgsl:~1366`,
   `ceiling = min(addAmt, max(capacity,1u))` vs the comment at ~1409-1412 saying stone
   gets the stainer's full amount). Determine intent from comments/DESIGN.md/git log;
   fix whichever (code or comment) is wrong.
10. **Keep-awake marks fan out to 7 neighbour chunks**: non-write reasons (REACT, STAIN,
    VISCOUS, FLOW) go through markDirtyR's face fan-out. Mark only the own chunk for
    non-write reasons (anything in a neighbour that has work marks itself). Check FILM
    and repose ownership don't depend on the fan-out for those reasons.
11. **Blast rim can leave powder floating** (`sim_explode.wgsl:~54-58`, suspected):
    `markBoth` in `apply` marks only the destroyed cell's own chunk; a rim cell on a chunk
    face never wakes the neighbour. Use the neighbour-aware mark.

Gates to verify with (one `--verify`): determinism plus the stain / fire / plant / fungus /
liquid gates that exist — find them with `--selftest --list` and grep the test sources
for the reactions you touched.

---

## P2 — Fluids + gas fixed costs  (WGSL + C++)

Files: `assets/shaders/sim_fluid.wgsl`, `sim_fluid_seam.wgsl`, `sim_waterbody.wgsl`,
`sim_particle.wgsl`, `sim_gas.wgsl` (spawn only, item 8), `src/sim/simulation.cpp`
(fluid/gas recording only), `src/sim/pass_table.def` (fluid/gas rows),
`src/sim/world.cpp` (fluid fill sizes only), `src/sim/waterbody.cpp`.
Do NOT touch the `markDirtyR` copies (P1 replaces them with a common helper).

1. `[neutral]` **Fluid pipeline recorded on every CA-awake tick with zero water**
   (`simulation.cpp:~2717-2738`: `seamActive = fluidCount>0 || fluidSpawnCount>0 ||
   (exciteOn && cx.caActive)`, fluidExciteMode=1 default). Costs ~1.4 MB fills
   (seam_fill_excite 393 KB, seam_fill_settle 787 KB, fluid_fill_blockMap 256 KB), full
   32K-slot scans (exciteScan, alloc), settleJudge 128 WGs rewriting fluidCalm, mirrorFold,
   table replayed 9×. Particles born by excite this tick must still simulate this tick, so
   not a plain gate. Do: clear settle bins after use instead of whole-region fill; skip
   settleJudge when nothing live; a per-workgroup early exit in exciteDetect when the
   chunk holds no seam liquid (derive from data already available — occupancy/material
   census — or a bit the CA can cheaply set; if it needs a P1-owned file, describe it in
   the report instead). Anything else that makes the no-water case near-free.
2. `[neutral]` **Same-address atomics** → workgroup reduce then one global op:
   Y-occupancy mask `atomicOr` per node (`sim_fluid.wgsl:~923`; also consider building it
   on the last substep only — render-only mask); `stainApply` (`seam:~1383`);
   `FA_EXSEEN` (~736), `FA_EXCANDID` (~889), `EX_COUNTS+ci` (~918), exciteEmit counters
   (~1205), wbShave WBS_* (`sim_waterbody.wgsl:~1390`), wbSurface RVCOUNT/RVSUM/histogram
   (~1544), particleTick SP_SPEED (seam ~1302), g2p FA_CALMSUBM (~1133), settleBin
   FA_BINNED (~1630). Integer add/or/max → identical results.
3. `[neutral]` **nodeBlock() per tap**: 27 in p2g1 (~709), 54 in p2g2 (~772/859), 27 in g2p
   (~1210) — each worldChunkOf+chunkSlotOf+atomic load. The 3-tap support spans ≤2 chunks
   per axis: resolve ≤8 block ids once per particle.
4. `[neutral]` **Whole 128-byte particle written back to update one field**: p2g2 (~792)
   density; settleKill (seam ~2234), consumeApply (~1254), g2p hard-kill (~1169) attr.
   Store the field only.
5. `[neutral]` **mark: 8 unconditional atomicOr per particle** (~494) → load first, OR only
   when needed.
6. `[neutral]` **settleBin/settleKill run over all particles when nothing settles**
   (seam ~1615/~2216): have settleScan write indirect args 0 when its count is 0.
7. `[neutral]` **exciteDetect / exciteEmit per-cell redundancy**: 6 slot+blockmap lookups per
   liquid cell for the wake trigger (~861-886) → one workgroup-level check; exciteEmit
   strided `li*16+s` layout (~1114/1136) → coalesced; `voxWordIndex` twice per cell →
   resolve page base once per workgroup.
8. `[neutral]` **Gas fixed costs whenever CA runs** (`simulation.cpp:~2638` gasActive ==
   caActive): `fill_gasSpawn` clears ~2 MB (`pass_table.def:~136`, VK_WHOLE_SIZE) though
   readers stop at the header count — fill only the header; `gasSpawnStep` fixed
   1,042-WG dispatch → indirect from the cursor if simple.
9. **Stale comments** (fix): sim_fluid.wgsl:15-17 ("fluid never writes a voxel…"), 31-33
   (map rebuild per-substep), 272-275 (sim.* fluid are floats now), "CPU-owned monotone
   count, never a readback" at sim_fluid ~517, seam ~1497, pass_table.def ~1056;
   world.h:~799 "allocated per substep" (world.h is P4's — leave it, list it in report);
   support.cpp:~1105 waterBodyMode default (P4's file — list it); seam:~223 SEAM_JFLOOR
   "matches FLUID_JMIN" (0.70 vs 0.60); phiQ doc above isqrtI (~289); seam:873 hard-coded
   `<16` for FLUID_MASS_MIN → use the constant; SEAM_GRAV_SUB derive.
10. `[neutral]` **waterbody.cpp brush-ball cell count O(r³)** (~363-368) → closed form or table,
    must give the identical count.
11. `[moves]`, optional, only if 1–8 are done: foam read-then-add race (sim_fluid ~1526,
    render-only) → add unconditionally, clamp in gridUpdate.

Report but do NOT change: particle struct shrink (world.h/common.wgsl layout), spatial
sort, speed-based mark pad, per-node solid mask — list your assessment.

Gates: determinism + the water/fluid/seam/waterbody gates (`--selftest --list`).

---

## P3 — Worldgen column work + far field  (WGSL + C++)

Files: `assets/shaders/worldgen.wgsl`, `src/sim/farfield.cpp/.h`, `src/sim/faredits.cpp/.h`,
`src/sim/farplumes.cpp/.h`, `src/sim/pass_table.def` (worldgen/far rows only),
far-level loop in `assets/shaders/raymarch.wgsl` (item 6 ONLY, ~4231-4475), the
`modified_`/eviction-hook lines in `src/sim/stream.cpp` (item 5 only), the FarPlumes
caller in `src/test/support.cpp` (~1430-1446 only).

**Compile-cliff warning:** the `far` entry sits near a 746 s driver compile cliff
(memory: gotcha-worldgen-far-entry-compile-cliff, gotcha-worldgen-curve-knots-stall-
driver-compile). Measure with `SANDVOX_SHADER_TIMING=1`. A change that pushes `far`
over the cliff is a regression even if faster at runtime — report it.

1. `[neutral]` **`far` sieve does per-column work per cell** (`worldgen.wgsl:~5118` →
   `genCellCol` ~4345 with caveValid=false, treeValid=false, canopyMemo=-1): fresh
   pondScan per cell, caveBands per stone cell, 25-tile treeAt per air cell, 25-tile
   undergrowthSite per air cell without genChunk's height guard. Replicate genChunk's
   per-column setup inside far's column loop (~5047): cave bands, treeCandsInto, pondScan
   once, canopy memo with skyMargin guard, then call genCellIn with the valid flags. Use
   the per-column trees.top in the sky ceiling (~5100) instead of global treeMaxTop().
   Output must be bit-identical (it feeds far rendering, and far-downsample gates).
2. `[neutral]` **genChunk recomputes column work per chunk** (`worldgen.wgsl:~4625-4737`):
   all 32 chunks stacked on an X/Z shift plane (and full-window worldgen) recompute
   genColumn/pondScan/treeCandsInto. Add a column pre-pass (one invocation per unique
   (x,z)) writing Col/colTop/cave-band inputs to a scratch buffer genChunk reads; sky
   chunks can early-out on the per-chunk max colTop before per-cell work. This needs a new
   buffer + pass-table row: keep it in the pass table / simulation plumbing with the
   smallest footprint; if it requires `world.h`, add only the constant/struct you need and
   name it in the report (P4 also edits world.h). If the plumbing is disproportionate,
   do only the sky early-out and report.
3. `[neutral]` **fardown skip signature mixes all 8 level origins** (`worldgen.wgsl:~5359`;
   `farfield.cpp:~79,93,279`): each level-1 step invalidates every dirty chunk's skip.
   Resident chunks always lie inside level 1's box, so the origins term is redundant;
   drop it. On a single-level reset invalidate only that level, not the whole farSig.
4. `[neutral]` **farfield queue**: single FIFO, all levels equal (`farfield.cpp:~307-359`) →
   per-level queues, finest first; drop a pending plane with the same (level, axis, slot)
   on reversal (~219-230); dedupe the shared edge on diagonal steps; make the per-tick
   `list` vector (~301) a member.
5. **FarEdits treats "was ever dirty" as edited** (`stream.cpp:~490-493, 661-672`,
   `modified_ |= snap.dirtyFlags` ~1603): smoke/water/wake become permanent 585-word
   patches in an uncapped unordered_map; re-noting appends (header says replaces).
   Key harvesting on an actual edit (e.g. compare the chunk's far signature against a
   pristine one, or a mutation-touched bit) — pick the simplest sound signal; fix the
   append-vs-replace; add a cap with a counter; group `farpatch` work per column
   (`worldgen.wgsl:~5232-5258`, words sorted z,y,x → 16 cells share one genColumn).
   Also (suspected) a level reset refills from procgen+FarEdits while fardown only
   re-downsamples dirty chunks → sleeping resident edits vanish from the reset level;
   confirm and fix if real.
6. `[neutral for sim; render-only]` **Sky rays walk all 8 far levels**
   (`raymarch.wgsl:~4231-4475`, note at ~4280): keep a per-level max occupied Y
   (atomicMax in far/fardown), and skip a level when the ray is above it and climbing.
   Image must not change (compare `--shot` frames before/after, one boot each).
7. `[neutral]` **FarPlumes::Build full rebuild every 2 voxels of eye motion**
   (`farplumes.cpp:~247-560`, per tick from support.cpp ~1446): cache per window origin,
   recompute only weights on eye-only moves, reuse containers. Fix stale comment
   support.cpp ~1430.
8. **A6 from the audit — farDown skipped on hash ticks** (`pass_table.def:~639`,
   suspected): the dirty-list rows are skipped on hash ticks; `dirtyOut(T+1)` only has
   T+1's marks, so a chunk whose last write landed on the hash tick is never
   downsampled (openness/glow have rolling refresh; farDown doesn't). Confirm, then run the
   dirty-list rows on hash ticks too (skip only occupancyDirty) or give farDown a backstop.
9. Stale comments: farfield.h:11 + world.h:3305 "256^3" (world.h: list it, P4's file),
   farfield.h:15, 138-145, 152, 221; farfield.cpp:105 "bit 24"; main.cpp:6319 (list it);
   raymarch.wgsl:4145 "~16 ticks per plane". fardown sentinel branch hashing 4,096 cells
   for a signature that is a function of the material (~5344) → closed form.

Gates: determinism, far-downsample, worldgen/terrain/farfield gates from `--list`; one
`--verify` also taking `--shot-frames screenshot_far` before/after for item 6.

---

## P4 — Readback, paging, streaming  (C++, touches world.h)

Files: `src/sim/world.cpp/.h`, `src/sim/pagetable.cpp/.h`, `src/sim/stream.cpp/.h`
(NOT the modified_/eviction-hook lines — P3), `src/sim/chunkstore.cpp/.h`,
`src/test/support.cpp` (SubmitWorldgen, snapshot plumbing, BuildWake; NOT the FarPlumes
caller), `src/sim/windprim.cpp`, the genChunk verdict reduction in `worldgen.wgsl`
(item 3 only; P3 is restructuring genChunk's column setup — keep your hunk near the
existing `genAct` atomics ~4787-4793 and minimal).

1. `[moves only if the wake set changes]` **A1 — wind-primitive wake list depends on
   readback timing** (`support.cpp:~1085-1091` filters BuildWake through
   `world.Snap().occupancy`; used at `windprim.cpp:~283`; result → tp.windWake, capped by
   windWakeChunks). Snapshot age depends on fence retirement → nondeterministic across
   runs/machines; ops-replay records it after the fact so it can't catch it. Fix: drop
   the CPU occupancy filter and budget by footprint (or filter on the GPU in the consumer
   pass). This is a DETERMINISM fix — highest priority in this package.
2. `[neutral]` **Per-tick snapshot readback ~1.05 MiB at rest** (`world.cpp:~25-73,
   588-642`): 27-chunk mirror 432 KiB, dirty 128 KiB, occupancy 128 KiB, support 128 KiB,
   chunkHash 128 KiB, fluidMirror 108 KiB. Breaks rule 3 (<1 MB/tick) and scales with
   window. Do: copy chunkHash only on hash ticks; skip the fluidMirror GPU copy when the
   previous snapshot showed no live fluid (the CPU memcpy is already skipped ~886);
   pack support (and dirty, if the CPU only tests !=0 outside diagnostics — verify) into
   bits or read back the compacted dirty list instead of the full array; skip unchanged
   mirror chunks if simple. Keep the CPU-visible API stable for callers. Record before/
   after bytes per tick in your report (compute from sizes).
3. `[neutral]` **Paged SubmitWorldgen reads back 512 MiB synchronously**
   (`support.cpp:~2139-2167`, 16×32 MiB ReadVoxelsSync + CPU Classify of 32K chunks;
   startup, menu regen, vk_smoke, ~222 selftest sites). genChunk already writes per-chunk
   occupancy: count 0 ⇒ PT_EMPTY without reading words (ApplyGenVerdict makes the same
   argument ~stream.cpp:1358); mixed chunks can't demote; only FULL chunks need words.
   Better: have genChunk reduce a "every cell == synthWordAt(JITTER|mat0,c)" / uniform
   flag with the workgroup atomics it already uses and publish UNIFORM/JITTER(mat) next
   to genAct, so neither SubmitWorldgen nor shift planes need to read words
   (`stream.cpp:~1405-1419, 1471-1596`, `pagetable.cpp:~557-613`). Final page table must
   be identical to what Classify produces today — assert that in a debug path or gate.
4. `[neutral]` **~8 CPU passes over all kNumSlots per tick** (world.cpp ~781, ~1045;
   stream.cpp ~1603 [P3 owns that line — coordinate by leaving it]; pagetable.cpp
   TightenFromSnapshot ~786, ConsumeOccupancy ~1342, RunCensus ~1762/1773,
   ApplyParticleShell ~968). RunCensus unconditional "for telemetry" → on request / every
   N ticks; ConsumeOccupancy + RunCensus gate on `snap.tick` changed (L2); drive what
   you can from the compacted dirty list / a resident-page list.
5. `[neutral]` small ones: TightenFromSnapshot `IntersectWith(snap)` then `UnionWith(snap)`
   == assignment (~828) — simplify, fix the "NEVER an assignment" comment; cache the 22
   `getenv` calls in pagetable.cpp (stream.cpp's PtDbg pattern); hoist per-tick SlotSet /
   vector allocations to members; `actVoxViz` 17 MiB always allocated (world.cpp ~30/143)
   → lazy or 4-byte dummy when overlay off; chunkHash memcpy every snapshot (~841) → skip
   when tick word unchanged; one `SynthChunkWords(entry, wc, seed, dst)` helper for the
   4–5 JITTER synthesis copies (world.cpp ~737, fetch ~930, RleEncodeSentinelChunk,
   Classify, ReadVoxelsSync support.cpp ~2620 slow form); ResetAllEmpty/ResetIdentity
   shared ClearTransientState (~89-211); merge per-page vkCmdFillBuffer runs (~2019);
   coalesce X-plane page-table/dirty uploads above a threshold into one covering range
   (~1957-1979, stream.cpp ~1440).
6. (suspected) **HarvestDemotes retry forever** (`stream.cpp:~1535-1585`,
   kDemoteFreshTicks=3 < kReadbackSlots 16): when frames run several ticks, batches are
   always "stale", re-copied and never classified. Confirm; replace the age bound with the
   write-reach clock (`reachTick_[s] < copyTick`). If item 3 removes the harvest for
   generated planes, check what still uses it.
7. **Analysis only, no code:** `ReleasePage` retire quarantine (2,048 pages, 32 MiB) vs
   `SetSentinel` → immediate `Free()` (pagetable.cpp ~395, ~1249). One of them is wrong or
   the quarantine is unnecessary. Write up which, with the barrier argument, in the report.
8. chunkstore: a `Get` miss creates an empty RAM region (`Touch`) and may open a file
   on the shift path (~91-96, 256-270, 322-340) → don't create a region on a miss. Moving
   spills to a worker thread: report only.
9. Stale comments listed by other packages in your files: world.h ~799 "allocated per
   substep" (fluid blocks are per tick), world.h ~3305 "256^3" (kFarN=512),
   support.cpp ~1105 waterBodyMode default (tuning.json sets 1), support.cpp ~2226-2242
   (possibly stale 134M sentinel-store faults note — verify).
10. Derive from kReadbackSlots where they mean it: kGenBatch, kMaxPendingDemotes, shift
    backstop 8, kDemoteFreshTicks, kPagedSnapshotMaxGap (only where the relationship is
    real).

Gates: determinism, page-roundtrip, residency/paging/stream gates from `--list`, voxregion,
ops-replay; `--residency dense` differential if you change page-table construction
(dense is the oracle). One `--verify`.

---

## P5 — Render path  (WGSL + C++)

Files: `assets/shaders/raymarch.wgsl` (NOT the far-level loop ~4231-4475 — P3),
`microbody.wgsl`, `debris.wgsl`, `cloud.wgsl`, `shadow_resolve.wgsl`, `denoise.wgsl`,
render-lighting functions in `common.wgsl`, render draw/record section of
`src/sim/simulation.cpp` (~3400-3780), `WriteRenderParams` and RenderParams struct
(wherever they live), `src/sim/tuning.h` (lodHandoffDist default only).
Render-only: world hash must not move at all. Judge images with `--shot-frames` (one
boot before, one after) and cost with `--budget-arms` / pass timers / `--shader-stats`.

1. **Terrain ignores overcast + lightning** (`raymarch.wgsl:~5087-5106 ambientAt` vs
   `common.wgsl:~1826-1855 ambientAtP`, changed in 8124088). Also duplicated
   `keyLightColor` (~1259) vs `keyLightColorP`, `keyLightDir` (~1294) vs `keyLightDirP`,
   `eclipseDim`/`dayWeight` (~1130) vs `eclipseDayWeightP`. Unify to ONE definition. Best:
   compute frame-constant light dir / colour / ambient terms on the CPU in
   WriteRenderParams and read them from RenderParams (also removes per-pixel smoothsteps,
   21+18 call sites) — then terrain and bodies can't diverge. Terrain WILL now grey under
   overcast / flash with lightning — that is the fix.
2. **Micro bodies: full fs on hidden pixels** (`microbody.wgsl:~500-588`, pipeline
   `simulation.cpp:~3434-3451`, draw ~3762): fs writes depth + discards → no early-Z; the
   boxes are front-face-culled. Add a depth-only pre-pass (march + depth write) then colour
   with depth test Equal, no depth write — as `DrawBodies` already does for debris
   (~3737-3748). Measure `rm_micro`.
3. **GI gather per pixel, not per face** (`raymarch.wgsl:~5322-5332`): every pixel on a
   due face runs the 9-ray gather and races to write. Elect one gatherer per face per
   frame (the CAS election `shadowSlotRead` uses) and let others read. Related: every
   near pixel RMWs `irradiance[wi]` (~10545) → one writer per face per frame. The image
   converges the same; small temporal difference acceptable, describe it.
4. **Unreachable shadow fallback compiled in** (`raymarch.wgsl:~10424-10428`): with
   SHADOW_CACHE the `else sunShadowAt` arm can't run; make the else call only
   `farShadowed` (or fold). Check register count via `--shader-stats`.
5. **Water veil clear per pixel + render-pass split every frame** (`raymarch.wgsl:~9947`,
   `simulation.cpp:~3669-3672`): frame-index tag in w0 instead of clearing; split the
   rendering scope lazily only when bodies/micro bodies will actually draw.
6. **Cloud weather map regenerated in full every frame** (`cloud.wgsl:~298-343`, ~31M
   hashes + sincos): amortize (regen when origin/evolve moves past a threshold, or a band
   of rows per frame) and replace cos/sin with a gradient table. Visual must stay the
   same within noise.
7. `gasOuterCountAt` 8-read filter in trace()'s innermost loop just to test >0
   (~3420-3425) → one coarse-cell read or cache last coarse key. `voxWordAt(h.liqCell)`
   re-read 4× in fs (~10107/10783/10921/10923) → read once.
8. `lodHandoffDist` code default 24 (`tuning.h:~4088`) resurrects the removed LOD circle
   when the key is absent → default to the shipped value (26 / disabled). Stale comments
   ~2793-2803, ~2842 (handoff is off), GI reach comments ~10502/~5155 (shipped
   giGatherBlocks is 12).
9. Hand-rolled voxel addressing in debug views (~3160-3169, ~10682-10694) → one read-only
   helper through the page-table path. `tracePlant` blade loop cap (~2244) like siblings.
   `bodyVnHash`/`bodyValueNoise` (microbody.wgsl ~281) duplicate raymarch ~5391 — if you
   are editing common.wgsl anyway for item 1, fold them there. Rename
   `MicroBodyModel._pad` → `cutFaces` only if it's WGSL-local.
10. Report only (don't implement): shadow-resolve staggered refresh (M4), surfaceGrain
    16 hashes/pixel, dead compiled experiments list (SUBOCC_SKIP, TAA, denoise,
    raymarch variants) — give a prune recommendation.

Gates: the render gates from `--list` (clouds, screenshot-based ones), determinism (must be
unchanged); `--verify` with `--shot-frames screenshot,screenshot_far` and `--budget-arms
baseline` before/after, plus `--shader-stats` for the raymarch fragment row.

---

## P6 — CPU frame loop  (C++)

Files: `src/main.cpp`, `src/game/session.cpp/.h`, `src/game/bodyreg.cpp/.h`,
`src/game/mob.cpp/.h` (AppendMicroHolders + FindMobById area only), `src/phys/debris.cpp/.h`
(handle lookup only), `src/sim/weather.cpp/.h`, `src/game/ai_behavior.cpp`
(perception cadence only), `src/ui/overlay.cpp` if a gate needs it.
Note: another session has UNCOMMITTED edits in main's checkout to mob.cpp/.h,
session.cpp/.h, physics, player, tuning — you work on HEAD in your worktree, which is
fine; keep hunks tight so the landing merge is clean.

1. `[neutral]` **FillBodyUI rescans every limb's voxels 10+×/frame** (`main.cpp:~11763`,
   `~1160-1195` → `PartMaterialCount` per material, walking skinVoxels/voxels
   `avatar.cpp:~2656`): one pass per limb into a histogram, cached on a per-limb edit
   counter/generation (add one if none exists; burn/carve paths bump it).
2. `[neutral]` **Inventory/grimoire mirrors rebuilt every frame with the screen closed**
   (`main.cpp:~12504-12735` under `if (target)`; glyphsOwned 49×49 string builds;
   describeWords runs ExpandWords+CompileSpell+DescribeSpell per glyph/page;
   `~11781-11799` glyphSlotReadouts compiles each hotbar glyph slot per frame). Gate on
   `ui.inventoryOpen` except fields the HUD reads; cache compiled readouts keyed on
   (glyph library generation, grimoire generation, page words); build glyphsOwned on reload.
3. `[neutral]` `BodyRegistry::AuditMicroModels` (main.cpp ~13510, bodyreg.cpp ~117 →
   `Mob::AppendMicroHolders` mob.cpp ~18086 snprintf + std::string per limb per frame):
   model-ids-only fast path; labelled list only when `suspect`.
4. `[neutral]` AI panel labels built every frame even closed (main.cpp ~11608-11628) → gate
   on aiWindowOpen. Dead `DescribeCast(...); line.clear();` (main.cpp ~11737) → delete.
   `BodySlotFor` two std::strings per call (session.cpp ~104) → resolve once per rig.
5. `[neutral]` **WorldScratch built fresh every tick** (`session.cpp:~3659`, comment ~195-215
   says it is held to avoid allocating) + `std::vector<PlayerScratch> scratch(...)` → keep
   both in TickAuthorityCtx, clear per tick.
6. `[neutral]` **Debris handle lookups are linear** (~40 `for (Body& b : bodies_)` scans;
   per tick: DriveStraps debris.cpp ~4257 O(followers×bodies), RefreshOwnership strap pass
   ~4441, BodyIsDeadFlesh debris.h ~212): add a handle→index map maintained where
   bodies_ changes (or store host index on the follower, re-resolved on change). Iteration
   ORDER of anything hashed must not change.
7. `[neutral]` UploadMicroBodies copies/uploads the whole model table + art palette every
   dirty frame (simulation.cpp ~1187, SetArtPalette ~932) → modelsDirty flag + palette
   stamp, member scratch. (simulation.cpp is shared with P2/P5 — tiny hunk only.)
8. **A7 — weather feeds the sim through float libm** (`weather.cpp:~183-196, 409-431`
   SimRainWord: std::exp, float smoothstep/lerp, lround → TickParams). Cross-machine risk
   for multiplayer. Compute SimRainWord with integer/fixed-point math (or a precomputed
   table built from integer inputs). `[moves]` only where the rounded word changes.
   Also compute it ONCE per tick (session.cpp ~1771-1772 calls it twice; each call does
   8× Scheduled copying Presets with std::strings). Weather dead code: gEasedTarget (~202),
   targetKey built and discarded each frame (~313-339), unused camXM/camZM; tick rate
   `/30.0` (~412) → the real tick constant.
9. `[neutral]` Magic caster id `0x9134A5EEu` in 14 places (session.cpp ~2420-2819,
   main.cpp ~11736-11748) → one named constant. Report (don't change) whether `w.ownerId`
   (~2819) should be per-session.
10. `[neutral]` AI probes: `Mob::CellSupportsWeight` (mob.cpp ~3460) does an unordered_map
    lookup per sample → last-chunk memo in AiProbeCtx (as GroundHeightAt does). Perception
    cadence change would move behaviour — report only.
11. Report only: main() split plan (10.5k lines; phases: input / event drains / UI mirrors /
    render-instance build / present), mob.cpp split plan by section banners, header
    fan-out (mob.h 53 TUs, world.h 96, tuning.h 64) — concrete proposal, no code.

Gates: determinism, the body-UI/inventory/spell/debris/strap/mob gates your edits touch
(from `--list`), rain/weather gates for item 8. One `--verify`.

---

## P7 — Vulkan backend + tooling  (C++)

Files: `src/gpu/*` (rhi.h, rhi_impl.h, rhi_vk.cpp, rhi_vulkan.cpp/.h, vk_record.cpp,
context.cpp, resources.cpp, passtimer.cpp, vk_info.cpp, vk_spirv.cpp),
`src/sim/pass_table.h`, and `simulation.cpp` ONLY for the pipeline-cache save call sites
and the RecordCtx unification (item 7).

1. **Pipeline cache (394 MB on disk) rewritten 3–5× per launch** (`SavePipelineCache`
   rhi_vulkan.cpp ~2231; called unconditionally at simulation.cpp ~1528, ~1651, ~3561,
   ~1793, ~3534, Shutdown). Save only when a create missed the cache
   (`VkPipelineCreationFeedback`, core 1.3 — check `APPLICATION_PIPELINE_CACHE_HIT_BIT`),
   plus a size cap that starts fresh past a limit (pick something sane, e.g. 512 MB,
   make it a named constant).
2. **Save leaves a no-file window** (~2267-2268: `std::remove` then `std::rename`) →
   `std::filesystem::rename` (atomic replace on MSVC; the SPIR-V cache does this at ~1418).
3. **shader_cache/ never pruned** (2.5 GB, 35,405 files, content-keyed ~1381): touch on
   hit + startup LRU sweep (e.g. delete entries unused for 14 days) — cheap, bounded
   (don't stat 35k files on the frame path; do it at startup, or on a background thread).
4. (suspected) **Swapchain acquire sync**: acquire semaphore waits at
   COLOR_ATTACHMENT_OUTPUT only (rhi_vulkan.cpp ~2077); first transition of an image in a
   command buffer uses srcStage NONE (vk_record.cpp ~941, imgState_ reset in Begin);
   render-scale path blits into the swapchain (BLIT/TRANSFER stage) not covered. Fix wait
   stages + the presentable image's first-transition source stage. Validate with
   `--vk-validation` in a WINDOWED run (`--frames 200 --vk-validation`), since headless
   runs never acquire.
5. **F5 reload leaks every pipeline/module/layout** (`pipelines_`, `moduleCache_`,
   `pipeLayouts_`, `setLayouts_` destroyed only in Shutdown ~2292; wrappers have no
   destructors rhi_vk.cpp ~144) → release through handle destructors, deferred behind the
   existing graveyard/serial pattern buffers use.
6. Dead Dawn-era API: `rhi::ComputePass`, `CommandEncoder::BeginComputePass`,
   `PassTimestampWrites`, `ComputePassImpl`, `PassTimer::BeginPass` (passtimer.cpp ~28),
   `CommandEncoder::ClearBuffer`/`Recorder::FillUntracked` if truly uncalled — delete.
   `--vk-info` (vk_info.cpp hand-written layouts ~320-341, `passUBO 54*256`) and
   `Backend::ZeroInitAll` only reachable from it — retire them if nothing else uses them
   (grep scripts/, docs, tests too). Stale comments: rhi_vulkan.h header block (says Dawn
   is live, refers to rhi_dawn.cpp), rhi_vulkan.cpp ~1310-1336, vk_record.cpp ~435/473.
7. **Tick context hand-copied across 3 structs twice per RecordTable**
   (Simulation::RecordCtx → rhi::TableCtx simulation.cpp ~2150-2177 → vk::RecordCtx
   rhi_vk.cpp ~827-869; 3 past bugs from fields that didn't cross) + TableBindings
   rebuilt per call (~160 shared_ptr copies, then again into vk::Bindings), up to
   fluidSubsteps+4 times a tick. One plain struct in pass_table.h used by all three;
   cache resolved bindings per page (0/1), rebuild when bind groups rebuild. Keep the
   simulation.cpp hunk confined to RecordTable plumbing (P2/P5 edit other parts).
8. Smaller: `CopyTrackedRegions(srcId, src, dst, span<VkBufferCopy>)` API (one tracker
   touch, one copy command) and convert the loop callers in `src/gpu` (leave callers in
   world.cpp/stream.cpp/support.cpp for a follow-up — list them); command-buffer recycling
   (free list or per-frame transient pool reset) + reuse one Recorder (rhi_vulkan.cpp
   ~1096-1103, rhi_vk.cpp ~241); per-flush arena for ≤4 KiB inline uploads and remove the
   quadratic duplicate-dest scan (~979, ~1026); skip CreateBuffer's zero fill when the
   first queued write covers the whole buffer (~824); skip redundant
   CmdBindDescriptorSets when layout+sets unchanged (vk_record.cpp ~618);
   `GpuBufferBytesTotal` never subtracts destroyed buffers and is an unlocked static
   (resources.cpp ~34-44); `AssembleShaderSource` re-reads common.wgsl + rebuilds the
   prelude per shader (resources.cpp ~517-566) → cache per load pass; descriptor pool
   sizes (~354) / query stride 8 (rhi_vk.cpp ~686) / texelBytes 4 (vk_record.cpp ~1160)
   → named/derived.
9. Report only: the head-of-command-buffer ALL_COMMANDS barrier (vk_record.cpp ~195-217)
   — propose cross-submit last-access tracking; the Sledgehammer/Precise A/B measures it.

Gates: determinism (must not move), `--vk-validation` on a gate run AND a windowed
`--frames 200 --vk-validation` for item 4, a pipeline-cache warm-launch check (second
launch should not rewrite the file — check mtime), hot-reload leak check if a gate exists.
