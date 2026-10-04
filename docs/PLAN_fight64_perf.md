# 64-creature fight performance — orchestration (2026-10-04)

Owner, 2026-10-04: "lets get corpses to have simpler colliders; lets try to
heavily optimize 64 creature fights."

## Baseline (package C of electricity wave 2, `--gate mob-cap64`, 300-tick brawl)

Tick wall 56.4 ms mean, 84.9 ms p95. Breakdown:

| Phase | ms per tick |
|---|---|
| Mob code (burnprof) | 23.0 (about 0.36–0.40 ms per creature, flat from 16 to 64) |
| Jolt step | 17–30 |
| Harness wait on the GPU tick | about 10–13 |

- Contacts are 1,670–2,700 body pairs a tick, by pair kind:
  - living limb vs corpse limb: 910–1,251;
  - debris vs living: about 450;
  - corpse vs corpse: the next largest.
- Splatter replay is still roughly quadratic (1.7 ms).

## Goal

Make a 64-creature fight cheap enough to play.

- **Target:** the CPU tick (mob code + physics) under ~16 ms mean, with p95
  close to it.
- **The bar is the mean, measured the same way before and after:**
  - same gate, same seed;
  - `SANDVOX_RUN_EXCLUSIVE=1`;
  - the main exe as the before-arm, or a cached copy.
- **Report:** a table per phase, before and after.

## Rules for every package

- Base: main at or after the commit that adds this file.
  - First command: `git log -1 --oneline`.
  - If this file is missing, run `git merge --ff-only main` (or
    `git reset --hard main` if you have no commits).
- Read `CLAUDE.md` and the relevant DESIGN.md sections. `export
  SANDVOX_NO_CRASH_DIALOG=1`. Build with `bash scripts/build.sh`, run with
  `bash scripts/run.sh` (perf numbers with `SANDVOX_RUN_EXCLUSIVE=1`).
- **The machine has 16 GB RAM, often only ~4 GB free.** Do not run anything
  memory-hungry in parallel with a build. C: disk is tight: if it drops
  under 5 GB, put your `build/` on F: (junction), as earlier agents did.
- **Determinism is inviolable.** Any parallelism must give bit-identical
  results:
  - per-entity work writes per-entity outputs, merged in id order;
  - no shared mutable state across threads;
  - no order-dependent float accumulation across threads;
  - Jolt stays in its cross-platform deterministic mode.

  Amortising work across ticks is fine when the schedule is a pure function
  of (tick, id). A moved hash is fine: rebaseline ONCE at the end with
  `--selftest --gate determinism --rebaseline`. A failed twice-run
  comparison is a bug.
- **Test budget:** no `--suite acceptance`, no bare `--selftest`. Iterate on
  `--gate mob-cap64` (it prints the timings). End with ONE `--verify`: your
  perf gate, `determinism`, and the 3–5 mob/physics gates covering the code
  you touched.
- **Do not trade away behaviour silently.** Per the owner's decision,
  corpses get SIMPLER colliders; do not stop them colliding. Any other
  gameplay-visible change (AI reacting less often, skipped contacts) must be
  named in your report as a trade-off. If it is visible in normal play,
  propose it rather than land it.
- Commit on your branch with clear messages; do not merge to main. Update
  DESIGN.md for what you changed, and the tuner `ARCH_NODES` / Development
  Status (ASCII `'` delimiters only).
- **Final report:**
  - changes and commits;
  - the before/after timing table;
  - exact gate lines;
  - whether the hash moved;
  - trade-offs;
  - what is left.

## Package P — physics: simpler corpse colliders and a cheaper Jolt step

Files: `src/phys/physics.*`, `src/phys/debris.*` as needed. In
`mob.cpp`/`mob.h`, touch only where a corpse's bodies are built or rebuilt;
keep those edits small (package M works in mob.cpp).

1. **Corpse colliders.**
   - Today limb colliders are box compounds (`StaticCompoundShape`,
     physics.cpp ~1487/3132).
   - When a creature becomes a corpse (or a ragdoll that will settle as
     one), give each limb one simple convex shape: a capsule or box fitted to
     the limb's voxels. Optionally merge a settled corpse's limbs.
   - Severed parts and debris follow the same idea where it is safe.
   - Keep collision with the world, the living and other corpses.
2. **The rest of the step.** Measure Jolt's own breakdown (broadphase,
   narrowphase, solver, sleeping) with the per-pair-kind contact counts C
   added (`Physics::LastStep()`). Then fix what dominates:
   - sleeping thresholds and time-to-sleep for settled corpses and debris;
   - collision layers and broadphase layers, so things that cannot matter
     never pair;
   - solver iteration counts per body class;
   - whether the job system is using the cores (`PhysicsWorkerThreads()`);
   - CCD only where needed (see the big-body CCD memory: CCD on large bodies
     was 3 fps once).
3. Keep the existing physics gates green (corpse-armor, ragdoll, debris,
   etc.). Corpse collider changes WILL move the hash; that is fine.

## Package M — the rest of the 64-creature tick: mob code, AI, render

Files: `src/game/mob*.cpp`, `ai_*`, `melee.*`, `avatar.*`, `session.cpp`
(tick plumbing), `src/sim/microbody.*` uploads; render-side body code if
measurement says so. Package P owns `src/phys/`.

1. **Profile first.**
   - The 23 ms mob code, at function level. Use the `burnprof` stages; add
     finer ones where a stage is opaque.
   - Everything else in `TickAuthority` with 64 creatures: AI decide, nav,
     perception/target selection, melee stroke tests, crowd spacing,
     splatter, bleeding, burning, ApplyShocks, skin/brick uploads, animation
     and IK, the readback consumers.
2. **Fix the hot paths:**
   - spatial bucketing for every pair/neighbour query;
   - early culls;
   - caching per-tick derived data (limb world transforms, AABBs) instead
     of recomputing it per query;
   - removing allocations in the tick;
   - splatter's quadratic term.
3. **Deterministic parallelism** of per-creature work, where the profile
   says it is the big lever. The machine has more cores than one tick uses.
   Per-creature phases whose inputs are tick-start state and whose outputs
   are per-creature (AI decide, animation/IK, limb transform updates) can
   run across a thread pool and merge in id order. Prove it is bit-identical:
   the determinism gate, plus the brawl run twice with the same hash.
4. **The harness's ~10–13 ms GPU wait.**
   - Find out whether the real game frame also waits on it, or whether it is
     harness-only. In the game, the tick should overlap the GPU.
   - If the game waits too, find what forces the sync.
5. **The render side of 64 bodies.**
   - Measure the frame with 64 creatures on screen (an existing
     `--render-budget` camera, or add a crowd camera).
   - If body rendering (raster bodies, micro-body bricks, uploads) is
     significant, fix the dominant term.
6. **Do NOT change what creatures decide or how they fight** to save time,
   unless you name it as a trade-off. "Far creatures think every other tick"
   is a proposal, not a silent change.
