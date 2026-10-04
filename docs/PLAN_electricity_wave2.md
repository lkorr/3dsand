# Electricity wave 2 — orchestration (2026-10-04)

Follow-up to `docs/PLAN_electricity.md` (wave 1, landed f944f6e). Owner asks,
2026-10-04:

- Fix charge spreading through bulk conductors (bug 1) and the ~8-body shock
  query cap (bug 2).
- Zaps and lightning must LOOK like arcs and bolts of electricity, not white
  voxels.
- **Bodies conduct and their physical material decides everything.** No
  hardcoded race rule (no "if sylvan"); a wooden body chars because it is
  wood, a metal one conducts because it is metal. Body microvoxels must behave
  the way the same material behaves in the world CA.
- Raise the live mob cap from 16 to 64.
- Do the perf items from the audit, plus obvious fixes in line with the game's
  goals: high performance, emergent behaviour from material data.

Androids are NOT stun-locked in play (owner observation) — not a package.

## Rules for every package

- Your worktree may be created at a STALE base. First command:
  `git log -1 --oneline`; if it is not `5b816d4` or a descendant of it, run
  `git merge --ff-only main` (or `git reset --hard main` if your branch has
  no commits yet). Re-check that the electricity code exists:
  `assets/shaders/sim_elec.wgsl`.
- Read `CLAUDE.md`, the DESIGN.md electricity sections ("Electricity --
  charge field", "Electricity -- shocks reach bodies", "Lightning: arcs, bolts
  and the strike path"), and `docs/PLAN_electricity.md`.
- `export SANDVOX_NO_CRASH_DIALOG=1`. Build only with `bash scripts/build.sh`,
  run the exe only with `bash scripts/run.sh ...`. WGSL- or JSON-only work
  needs no build: use the main checkout's exe with `SANDVOX_ASSET_DIR`.
- **Test budget (owner rule):** no `--suite acceptance`, no bare
  `--selftest`. Code first. Iterate with single `--gate`s only when
  correctness is genuinely uncertain. End with ONE `--verify <your gates +
  the 2-3 covering the code you touched>`. If the hash moved on purpose:
  `--selftest --gate determinism --rebaseline` once, at the end. A moved hash
  is not a bug; a failed twice-run comparison is.
- When your change regresses a gate, do NOT invent a new mechanic or material
  to keep it green. Record it known-failing with a `tests/BASELINE.md` entry
  and report it.
- Invariants: bit-determinism (integer sim math, no atomics-order outcomes),
  cost scales with activity (every new process bounded, sleeps when idle),
  every mutation through the queue/op record. Keep `ops-replay`
  reproducible: anything new that changes hashed state from the CPU must be
  in the op record.
- Commit on YOUR branch with clear messages; do not merge to main (the
  orchestrator merges). Update DESIGN.md for what you changed, the STATUS
  section of `docs/PLAN_electricity.md` (add a wave-2 row), and
  `assets/tuner.html` ARCH_NODES and Development Status (ASCII `'`
  delimiters only).
- Other packages run in parallel in other worktrees. Keep your edits in your
  files. Where you must touch a shared file, keep the change small and local,
  so it merges cleanly.
- **Final report:** what you changed, your commits, the gate results (exact
  lines), whether the hash moved, what is left open, and any trade-off the
  owner should decide.

## Package A — the charge field (sim core + perf)

Files: `sim_elec.wgsl`, the E2 block of `sim_step.wgsl`, `src/sim/elec.*`,
the elec parts of `simulation.cpp`, the elec rows of `pass_table.def`,
`world.h`/`world.cpp` (elec only), the elec rows of `tuning_params.def`, the
`electric` blocks of `materials.json` for WORLD materials, and
`selftest_elec.cpp`/`selftest_elec_strike.cpp`.

1. **Bug 1: bulk conductors carry charge as far as a wire.** Max-plus has no
   spreading loss. A strike (30,000) into water (resist 6), or into
   rain-wet ground, reaches ~800 cells: it pages the whole window until the
   pool refuses, keeps hundreds of chunks awake for ~50 ticks, and shocks
   every mob on wet ground anywhere. Add a physically motivated spreading
   loss that stays max-plus/Jacobi and order-independent.
   - Suggested: a per-cell entry cost that grows with the number of
     conducting neighbours (current divides in bulk; a wire has 2). Use
     static data within the tick so it stays deterministic.
   - Alternatives are open if they are better. Make the constants knobs.
   - Targets:
     - A copper wire still carries far.
     - A strike into a pond still shocks a mob a few metres away.
     - A strike into the sea, or onto rain-wet ground, stays local (tens of
       cells), with bounded pages and awake chunks.
   - New gate (e.g. `elec-bulk`): a strike on a sea/large-lake fixture and
     on rain-wet ground. Assert the peak pages, the reach radius, and that
     pages return to 0 with awake chunks <= 32 afterwards.
2. **Dry wood:** resist 60 lets lightning run ~500 cells through a connected
   timber frame, rolling ignition on every cell. Real dry wood is close to an
   insulator; wet wood conducts (the wet rule already handles that).
   Re-tune so a strike chars and ignites near the strike point, not across a
   whole village.
3. **Phantom charge after a window shift.** `elecAt`/`elecAtCell`
   (sim_elec mirror, around `:80-93`) do not check `EM_OWNER`. The re-key
   runs after the CA, so a newly arrived chunk reads the departed chunk's P
   for one tick. Fix it with an owner check, or by moving the re-key earlier.
4. **Fast path when nothing is charged.** About 15 passes and barriers run
   on every CA-active tick with zero charge:
   - `elecRounds`+1 single-group elecAlloc dispatches;
   - the copies;
   - the zero-group indirect rounds;
   - settle and purge.

   Skip the whole chain on a deterministic CPU latch (`World::ElecMayBeLive`
   plus "a source op or doorbell is possible this tick"). Prove it is a pure
   function of the tick and the op list. Measure the CA/elec perf nodes
   before and after.
5. **Cache resist and seed per tick.** Each round recomputes them for all
   4,096 cells (`:405-419`): a voxel read, param loads and a stain lookup per
   cell. Compute once in round 0 into page-side storage, or anything cheaper.
6. **Re-key zeroing** of a 16 KiB page is serial in one thread (`:222`). Make
   it workgroup-parallel.
7. **Brine** conducts no better than fresh water. Read the solute layer's
   salt so dissolved salt lowers resist (both shaders that use the wet rule
   must agree).
8. **Missing `electric` blocks:** `sodium`, `acid`, `lava`, `molten_glass`,
   `shore_mud` (plan: wet soil ~30). Check every `metal`/`conductive` tagged
   material has one. Package B owns the BODY materials (flesh, skin, bone,
   hair, cloth, alloy, circuitry, power_cell, …) — do not edit those.
9. **`elec-strike` holes:**
   - Assert pages free and awake chunks <= 32 after the bolt.
   - Replace the `firePeak > 0 || woodEnd < wood0` OR with two real checks.
   - Seed the ~20 `RecordObserved` keys missing from `tests/baseline.json`
     (`elec.arrivalTicksObserved`, `elecIgnite.*Observed`, …).
10. **Nit:** elecSettle declares `R(PageTable)` it never reads; trim
    over-declared bindings only if `check_pass_table.py` agrees.

Run `--verify elec-field,elec-electrolysis,elec-ignite,elec-crackle-bounded,elec-strike,<your new gate>,determinism`.

## Package B — bodies conduct; the material decides (+ bug 2)

Files: `src/game/mob_shock.cpp`, the elecQuery parts of `sim_elec.wgsl`
(the query kernel only — package A owns the field kernels) and `elec.h`
(query layout), body-reaction code in `mob.cpp`/`avatar.cpp`/`debris.cpp`/
`microbody.*` as needed, the `electric` blocks of BODY materials in
`materials.json`, and `selftest_elec_mob.cpp`.

1. **Bug 2:** `mob_shock.cpp:113-117` asks one box per base limb (15 per
   human) against `kElecQueryMax` 128, in spawn order. Bodies past ~8 are
   never shocked. Package C is raising the live cap to 64 (plus 12 dead plus
   players).
   - Restructure: one whole-body box per creature first. Then limb boxes
     only for bodies whose body box came back charged.
   - Or cull bodies whose chunks hold no charge pages.
   - Size the capacities from `kMaxMobs`, so the cap change cannot silently
     break them.
   - Add a gate: 64 bodies with the far ones standing in charged water get
     shocked.
2. **Bodies conduct by material.** Today damage reads only the box's max P,
   the wet coat and armour; the body's own material is ignored.
   - Make a body's microvoxels behave as the same materials do in the world
     field and the E2 CA:
     - charge enters through cells in contact with charged world cells;
     - it propagates through body voxels using each material's `electric`
       resist (the same max-plus rule, the same wet rule on stained/coated
       voxels);
     - ohmic ignition and charring of body voxels use the material's own
       `electric.ignite`/`char` data — the same numbers the world uses.
   - Effects follow from the material:
     - A wooden body (sylvan) chars and ignites because wood does.
     - A metal shell (alloy) conducts and does not burn.
     - Flesh takes damage and stuns.
     - A worn metal armour piece or a held metal weapon conducts into its
       wearer or wielder.
   - Express the damage/stun response as MATERIAL data (e.g. a field in the
     `electric` block, or tags), not race checks. Grep for race/def-name
     special cases in shock, burn and reaction paths, and convert those that
     encode PHYSICAL behaviour to material data.
   - Android materials (`alloy`, `circuitry`, `power_cell`) get `electric`
     blocks; whatever they then do should be emergent.
3. **Bodies pass charge on.** A charged body touching a conductor or another
   body should conduct (two creatures holding hands in a pond, a sword in
   charged water shocking its wielder). If this needs body charge to seed the
   world field, it changes hashed state from the CPU: it MUST go through the
   op record (a recorded stream), bounded per tick.
4. **Audit the parity of body microvoxels with the world CA** for electricity
   specifically:
   - the virtual electric neighbour for body reactions;
   - coats on bodies (water/brine conduct, oil insulates?).

   List any other body-vs-world divergences you find in the report; do not
   fix unrelated ones.
5. Keep the per-tick CPU cost bounded and cheap with 64 bodies (cull early
   on "no charge near"; touch no voxel when nothing is charged).

Run `--verify elec-water-mob,elec-stun,elec-player-stun,elec-replay,android-sparks,mob-burn,<new gates>`.

## Package C — live mob cap 16 -> 64

Files: `mob.h`/`mob.cpp` (the cap and anything sized by it), `world.h`
sizing (micro body pool, model table, render slots), GPU instance buffers,
`ai_*`, and any O(n^2) or per-mob budgets.

1. Raise `MobSystem::kMaxMobs` to 64. Find EVERYTHING sized from, or
   implicitly assuming, 16:
   - `kMicroBodyPoolWordsWorld` and its sizing argument (`world.h:2450+`);
   - the model table, render slot tables and instance buffers;
   - `kElecQueryMax` (package B restructures that query; size it from the
     cap);
   - per-tick budgets (burn front, bleed ops, strikes/blocks vectors);
   - the AI pair loops (`mob.cpp:7681` says "kMaxMobs is 16, so at most 120
     pairs");
   - ai_nav;
   - selftests hard-coding 16 (`selftest_mob.cpp:11461`);
   - the tuner/HUD.

   Decide whether `kMaxDeadMobs` (12) and `kMaxDeadBodies` should scale; say
   why.
2. **Perf:** 64 live NPCs fighting must not tank the frame. Build or reuse a
   harness:
   - spawn 64 mixed NPCs near the player, fighting;
   - measure the CPU frame/tick nodes;
   - fix the hot paths that go quadratic or linear-with-big-constant
     (spatial bucketing for pair queries, culling, amortised AI).

   Report the numbers before and after.
3. A gate that spawns 64 living creatures, asserts all 64 spawn, tick and
   render-slot correctly, and that the 65th is refused cleanly.
4. VRAM: report the added bytes for each resized buffer.

Run `--verify <your gate>,mob-burn,determinism` plus the 2-3 mob gates
covering what you touched.

## Package D — electricity that looks like electricity (render only)

Files: `raymarch.wgsl` (the charge-glow block and the gas shading for
`spark`/`arc`/`lightning`), new render shader(s)/pass rows as needed,
`main.cpp` render plumbing, `lightning.*` only to EXPOSE strike geometry
(no sim change).

Today `spark`/`arc`/`lightning` cells render as white voxels. Owner wants
arcs and bolts. Render-only: the world hash must not move.

1. **Lightning bolts:** a strike already produces a planned path
   (`PlanStrike`/`EmitStrike`).
   - Render a luminous, jagged, BRANCHING bolt along it: thin hot core plus
     a soft halo, flicker and re-strike over a few frames, then fade.
   - It lights the scene for those frames (the flash already exists; make
     the bolt itself the light).
   - Branch shape may use a render-only hash.
   - Storm strikes and the lightning glyph both go through this.
2. **Arcs and zaps:** cells of electric-source gases (`arc`, `spark`, and
   `lightning` cells outside a planned bolt) should read as crackling
   filaments, not cubes:
   - thin bright lines jumping between the cell and adjacent
     charged/conductive cells, re-randomised every frame or two;
   - a tiny bright point or streak for `spark`.
3. **Charged conductors:** the existing charge glow can become a crawling
   filament or creep pattern over the surface, instead of a uniform tint,
   while staying cheap.
4. **Gaps:** the glow is off when the camera is underwater (`!underwater`
   guard) — fix that. Glow on charged bodies (raster) if a cheap path exists
   (package B may expose per-body charge; coordinate via a note, do not
   block on it).
5. **Cost:** zero when nothing electric is on screen (keep
   `elecAnyCharge()`-style early outs); bounded otherwise. Report the
   `--render-budget` numbers with and without.
6. Verify with `--shot` frames (the elec shot frames from E5b exist) and LOOK
   at them. Iterate on the look — this package is judged on the picture.
   Include the final screenshot paths in the report.

Run `--verify determinism` (the hash must not move) and
`--shot-frames <elec frames>`.

## Package E — gameplay fixes

Files: `lightning.*` (targeting only), `spell.cpp`/`spell.h`, the strike and
stun-input parts of `session.cpp`/`session.h`, the HUD in `src/ui/overlay.*`,
`src/audio/cues.*`, `assets/sounds/`.

1. **Strike into canopy/buildings:**
   - Today the scan starts at analytic ground +24 (`kStrikeScanUp`,
     `session.cpp:2967`), so a tree or house taller than that is entered
     from inside and the bolt stops in mid-air.
   - Scan from the real top: the snapshot mirror/chunk cache, or the bolt
     ceiling.
   - Lightning should prefer tall things (trees, towers, a raised rod).
2. **Spell lightning seeks rods beyond the mirror.** Prefetch the chunks as
   storm strikes do (`session.cpp:2976`), or another deterministic answer.
3. **Wards:** `spell.cpp:4089-4097` checks the ward at the aim point only,
   but the target search moves the bolt up to 12 cells. Re-check at the
   struck cell, and fix the false comment.
4. **Spell strength:** `SpellStrike::strengthMille` is recorded and never
   read. Scale the bolt or arc energy by it.
5. **No mana for a refused strike:** a strike refused by the 512-cell budget
   still charges its cost. Refund it, or refuse before charging (budgets are
   charged BEFORE emission, per CLAUDE.md).
6. **A stunned player** can still use `hotbar`, `tool`, `useRef` and `talk`
   (`session.cpp:5636-5648`). Gate the physical actions; decide on talk.
7. **Multiplayer:**
   - Storm strikes roll only around player 0 (`s.index == 0`). Roll per
     session, deterministically.
   - Flash/thunder events should reach every client's render.
8. **Feedback:** a pixel-art HUD cue for being shocked/stunned (the UI stays
   pixel art; text must fit). Sound:
   - add slots for zap/shock (`sound_schema.js` + `Cues::kSlotPrefix`, which
     must agree);
   - fire them from strikes and `ApplyShocks`;
   - thunder has a slot but no `assets/sounds/weather/` — generate
     placeholder thunder/zap samples procedurally (a script under
     `scripts/`, deterministic output, mono, the existing format) and say in
     the report that they are placeholders for the owner to replace.

Run `--verify elec-strike,elec-player-stun,<spell gates you touched>,determinism`.
