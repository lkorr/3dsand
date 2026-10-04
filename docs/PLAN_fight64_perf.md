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

## Round 1 result (2026-10-04)

P (39a8b56) and M (59b0750) landed.

| Phase, 64 brawl | Before | After |
|---|---|---|
| Jolt step (same corpse count) | 26.5 | 10.5 |
| Mob side | 29.6 | 18.8 |

- The harness GPU wait (~11 ms) is harness-only: the game frame never blocks
  on it.
- **The real game still cannot hold 30 Hz with 64 creatures.** CPU is about
  35 ms a tick, and the frame loop spirals: catch-up ticks take frames from
  ~30 to ~180 ms over 300 ticks.

## Round 2

### Package R — regressions and determinism (do this first, it gates trust)

1. **`corpse-splatter` fails** since P: "blood coat 0 -> 0". It passed at
   5f5fce7 ("0 -> 9").
   - Likely the corpse hull (P): the splatter flight or coat landing now
     meets the hull instead of the detailed shape, or the "re-check against
     the detailed shape" path misses for droplets.
   - Fix it so blood lands on the corpse as before. A hull is a collider,
     not the body's surface.
2. **Possible scheduling-dependent physics.**
   - P saw the old-collider corpse-heavy arm end with 9/10/11 alive across
     different builds (same build: always the same).
   - M saw run-to-run divergence in one build, first in the Jolt contact
     count at tick 208, with creature inputs identical up to then.
   - The contact-impact lists were already made order-independent
     (DESIGN.md "the contact-impact lists are collected uncapped and capped
     AFTER the step").
   - Find out whether anything is still racy. Run the 64 brawl, natural and
     `killEvery 4`, with `SANDVOX_MOBCAP_DIGEST=1`:
     - 3+ times in one build;
     - across `SANDVOX_MOB_THREADS=1` vs default;
     - across Jolt worker thread counts (`PhysicsWorkerThreads()`).

     Compare the per-tick digests.
   - On a divergence, attribute it to its first differing tick and field
     (add reporter detail rather than eliminate by toggling; CLAUDE.md
     rule 6), then fix the cause.
   - A cross-BUILD difference (different compiled code, same source) is
     worth a sentence on the cause (LTO/inlining/fp contraction), but
     run-to-run and thread-count determinism is the invariant.
   - Make the twice-run digest comparison a permanent part of `mob-cap64`
     (or a new gate), so this is gated from now on.

### Package Q — round-2 performance (target: the real game holds 30 Hz at 64)

**A moved world hash is FINE** (CLAUDE.md rule 1): rebaseline once at the
end. Do not avoid a change because it moves the hash.

Allowed, if deterministic:
- a different but deterministic split of a shared per-tick budget;
- a different voxel order inside a collider.

Name each in the report.

1. **CCD, 6–10 ms.** Dead flesh shoved by kinematic living limbs triggers
   linear casts against the crowd's box compounds. Options:
   - restrict CCD to bodies whose motion could tunnel through TERRAIN (a
     speed relative to their own thickness), and cast against the terrain
     layer only (Jolt object-layer filtering of the cast);
   - cap the velocity a kinematic limb can impart to a corpse;
   - use simpler living-limb shapes for the cast.

   Keep "nothing tunnels through terrain" gated.
2. **Blade carves, ~5 ms.** About 1.8 ms is `DownsampleSkin`'s hash map
   (`src/phys/lattice.h`). Replace it with a dense/flat structure even if
   the collider voxel order (and the hash) changes.
3. **Burn under the shared budget (~3.6 ms) and the shock solve (~2.4 ms).**
   Parallelise them with a deterministic per-creature budget split, e.g. a
   prefix allocation in id order computed before the parallel pass.
4. **The frame-loop spiral.** When the tick costs more than its period the
   game must degrade gracefully, not climb to 180 ms frames.
   - Cap catch-up ticks per frame.
   - Let sim time slow (the tick count stays the authority, so determinism
     is unaffected).
   - Keep the GPU-snapshot deferral sane.

   Measure with `--brawl` in the real windowed loop.
5. Re-profile with `SANDVOX_SAMPLE_PROF=1` and take whatever is next.
6. **Report:** the before/after table, plus `--brawl` real-frame p50/p95.

R and Q may both touch `src/phys/physics.cpp`. R owns
determinism/contact-report code and splatter; Q owns the CCD settings and
`lattice.h`. Keep shared edits small.

### TBD: 64-body fight performance (2026-10-04, package Q wrap-up)

Owner, 2026-10-04: 30 Hz at 64 bodies is unrealistic at the moment and is
deferred. Package Q stopped after item 2. What landed, what was measured, and
what is left, ranked.

**Landed (Q2).** `DownsampleSkin` (`src/phys/lattice.h`) uses a dense block
index over the blocks' bounding box instead of an `unordered_map`, in
thread-local scratch. The output order is now the lattice order (z, y, x)
instead of the map's bucket order. That changes a re-derived collider's box
merge, and with it the 64-brawl's trajectory. The `determinism` gate's hash
did not move. `tests/lattice_test.cpp` passes.

**Current numbers.** `mob-cap64`, 300-tick natural fight, `SANDVOX_RUN_EXCLUSIVE=1`.
Both arms are this worktree's non-LTO build: the before arm is 895e4fb, the
after arm is 895e4fb + Q2.

| Phase, ms per tick | Before | After |
|---|---|---|
| Tick wall mean / p95 | 54.29 / 81.18 | 35.53 / 45.44 |
| Mob side (burnprof) | 22.00 | 17.66 |
| ...carve | 4.30 (3,084 carves) | 2.78 (3,097 carves) |
| ...stroke (contains most carves) | 6.52 | 4.42 |
| ...burn | 4.49 | 3.59 |
| ...stain (incl. splatter 0.63) | 3.00 | 2.72 |
| ...shocks | 2.74 | 2.74 |
| ...bleed | 1.13 | 0.66 |
| ...recount, worst tick | 22.57 | 16.84 |
| Jolt Update | 15.69 | 5.05 |
| ...ccd, wall / cpu | 10.35 / 45.19 | 2.73 / 7.39 |
| ...collide, wall | 3.69 | 1.61 |
| Harness GPU drain (`readbackStall`, harness-only) | 12.37 | 9.57 |

**Only the carve row is attributable to Q2.** It has the same carve count at
-1.5 ms, which matches the ~1.8 ms estimate. The fight itself diverged: 52 vs
55 alive at the end. In the before run 11.3 live ragdolls a tick were
linear-casting box compounds (`rig-limp`); the after run had none. So the
Jolt/CCD drop is mostly a different fight, not the change. Package R's
run-to-run question sits under every Jolt number here as well. `--brawl` (the
real windowed frame) was not re-measured. Its last number is round 1's:
frames climb from ~30 to ~180 ms.

**Remaining, ranked by expected gain:**

1. **CCD: ~5–8 ms of Jolt wall in a ragdoll-heavy tick (10.35 ms wall, 45 ms
   CPU in the before arm).** Who casts, per tick: rig-limp 11.3 (all box
   compounds), rig-dead 18.4 (3.8 compound), debris 10.8, severed-hold 2.8.
   - Jolt decides to cast when a step exceeds 0.75 × the shape's INNER RADIUS.
     For a box compound that is the smallest sub-box, about half a voxel, so a
     shoved limb casts at walking speeds.
   - **"Cast against terrain only" cannot be done through Jolt's API.**
     `JobFindCCDContacts` uses the body's own default broadphase/object-layer
     filters, the same ones collision uses. `SimShapeFilter` cannot tell a cast
     from the narrow phase. Doing it needs a Jolt patch, and Jolt sits in the
     shared `C:/sv-deps` cache.
   - **The doable version:** before each `Update`, set
     `BodyInterface::SetMotionQuality` per CCD-eligible body. Use `LinearCast`
     only when `|v|·dt > 0.375 × min(shape local-bounds extent)`. That is
     Jolt's own ratio applied to the body's real thickness, which is the
     terrain-tunnel criterion. Everything else stays `Discrete`.
   - Also consider casting a living ragdoll as its hull, as corpses already do.
   - Keep the gate that pins "nothing tunnels through terrain" (`selftest_phys`
     big-body / CCD gates).
2. **The frame-loop spiral, from 180 ms frames to about tick+render (~45 ms
   at today's cost).** `main.cpp`'s tick loop has only the 4-tick cap and the
   GPU-lag throttle. There is no CPU budget, so a 35 ms tick runs four times a
   frame.
   - Fix: after a frame's first tick, run another only if
     `spentTickMs + lastTickMs <= kTickDt`. Otherwise drop the debt, as the
     GPU throttle does (`accumulator = min(accumulator, kTickDt)`).
   - This is pure pacing. Sim time slows (to ~75% at 35 ms a tick) and the tick
     count stays the authority. Headless runs and `SANDVOX_TICKS_PER_FRAME`
     never take this path.
   - Add an env A/B arm and a dropped-tick counter beside `g_ticksThrottled`.
   - It is cheap and the largest felt win. It needs a `--brawl` before/after.
3. **Burn head under the shared pot: 3.6–4.5 ms, ~2.5 ms to gain.**
   - Compute every creature's (front, ops) share up front, in pot order, from
     the weights (`BurnShareOf`).
   - Run `BurnTickHead`'s per-limb `BurnOneLimb` in parallel into per-creature
     cell-op/spawn buffers.
   - Defer `FlushBurn` (severs, Jolt rebuilds, kills) to a serial apply in pot
     order.
   - Budget shift to name: a creature no longer inherits the unspent remainder
     of earlier creatures in the same tick, unless a second pass hands it on.
     Flushes also move after all burns.
4. **Shock solve: 2.7 ms, ~2 ms to gain.** The max-plus solve is global over
   touching bodies. Split the parts into connected components by box contact,
   solve the components in parallel, and emit their ops in part order. The
   result is identical, because components do not interact.
5. **The rest of carve (2.8 ms) and stroke (4.4 ms), ~0.5–1 ms, unmeasured.**
   `SpallGrow` in the same header still rebuilds an `unordered_set` per round.
   The carve's other containers are a sampler question
   (`SANDVOX_SAMPLE_PROF=1 --gate mob-cap64`); not profiled in Q.
6. **p95: `RecountBurn` spikes (worst tick 17–23 ms, one creature).** Amortise
   the recount across ticks on a (tick, id) schedule.
7. **Bleed 0.7–1.1 ms and stain 2.7 ms.** Already pooled by M. Small.
