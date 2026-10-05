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

#### Package R result (2026-10-04, cut short by owner directive)

1. **`corpse-splatter`: fixed, and it was not the hull.** `SANDVOX_SPLAT_TRACE=1`
   (new, permanent: every limb a burst is flown at, its trials, each landing)
   showed the gate's burst landing on 9 of the corpse's 17 limbs and missing
   only the one the gate counts. The flight is analytic against each limb's
   voxel lattice; it never consults a collider, so the hull is not in its
   path. What P changed is the pose the corpse settles in. The gate counts
   the root limb (the pelvis), the one limb the corpse's own gout never
   coats. The wide burst (cone 0.35) gave it ~5 of 24 trials, each with
   ~30% odds of hitting, so roughly a 1-in-6 chance of no landing at all.
   It was "0 -> 9" before P and "0 -> 0" after. The gate now throws a
   focused splash (cone 0.12), which sends every trial at the root limb.
   No engine behaviour changed.
2. **Determinism soak: NOT DONE (TBD).** No digest runs were made before the
   owner's wrap-up directive. Still open:
   - the natural and `killEvery 4` arms with `SANDVOX_MOBCAP_DIGEST=1`, 3+
     runs each;
   - `SANDVOX_MOB_THREADS=1` vs default;
   - `SANDVOX_PHYS_THREADS=1/2/7`.

   Attribution tools already exist:
   - `SANDVOX_PHYS_TRACE=<file>` writes per-step body-state hashes, before
     (P) and after (S) `Update`. A differing S after a matching P means
     Jolt's step itself. A differing P after a matching S means our code
     between steps.
   - `Physics::DebugBodyStates` gives per-body float bits (see
     village-twice).

   Hypothesis for M's tick-208 contact-count split: still unknown. No
   permanent twice-run gate has landed.

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

## Round 3 (2026-10-05) — "optimize the hell out of all of the systems at play"

Owner, 2026-10-05: carry on the 64-creature work and optimise every system in
the fight. The "30 Hz is TBD" deferral is lifted: round 3 works the whole
ranked list above, plus the determinism soak that round 2 skipped.

### Fresh baseline (main b21d23a, exe built from HEAD)

`SANDVOX_RUN_EXCLUSIVE=1 --selftest --gate mob-cap64`. The full output is in
`F:/sv-fight64/baseline_b21d23a_mobcap64.txt`. A frozen copy of the exe is at
`F:/sv-fight64/sandvox_b21d23a.exe`. Use it as the before-arm for C++-only
packages: it reads main's assets through the compiled-in path, or set
`SANDVOX_ASSET_DIR`.

| Phase, ms per tick | b21d23a |
|---|---|
| Tick wall mean / p95 / worst | 31.73 / 39.90 / 47.31 |
| Mob side (burnprof mean / worst) | 14.29 / 32.77 |
| ...stroke (worst 15.78) | 3.78 |
| ...carve (worst 9.98) | 2.38 |
| ...burn (burnLimbs; worst 13.55) | 2.83 |
| ...recount (worst 12.09, one creature) | 0.47 |
| ...stain (stainContact 1.02) | 2.28 |
| ...shocks | 2.18 |
| ...bleed / splatter / twinSync / anim | 0.58 / 0.47 / 0.42 / 0.41 |
| Jolt Update | 3.80 |
| ...ccd wall / cpu | 1.99 / 7.36 |
| ...collide wall / cpu | 1.24 / 7.41 |
| pageTableCpu / terrainMesh | 0.91 / 0.83 |
| readbackStall (harness-only) | 10.75 |

The end state was 54 alive, with 18.9 rig-dead linear casts a tick.

**Target.** Mob side plus physics under 10 ms mean, and the worst tick under
16 ms. In the real windowed `--brawl` loop, frames stop spiralling and hold
p50 at or under ~33 ms with 64 bodies on screen.

### Round 3 rules (in addition to "Rules for every package" above)

- **Perf numbers are only comparable on the SAME fight.**
  - A change that is meant to leave behaviour bit-identical must show it:
    `SANDVOX_MOBCAP_DIGEST=1` per-tick digests identical to the before-arm.
    Then the timing delta is clean attribution.
  - A change that deliberately moves the fight (a budget split, a collider
    order) must say so. It must also report the counts that set the cost:
    alive at the end, carves, linear-cast bodies, front size. The reader
    then knows whether the gain is the change or a different fight.
- **Noise.** Run the before-arm and after-arm back to back under
  `SANDVOX_RUN_EXCLUSIVE=1`. Quote the mean of 2 runs per arm when the delta
  is under 1 ms.
- **Write cost into instruments, not into prose.** If a stage is opaque, add
  a `burnprof` stage or a Jolt phase line. These are permanent.
- **Disk.** C: has ~50 GB free. Put a worktree `build/` on F:
  (`F:/sv-wt/<name>`, junctioned) if you see less than 10 GB.
- **File ownership** is listed per package. `mob.cpp` is 32k lines and shared
  by B, X and K. Each owns a named REGION, so keep diffs inside it and do not
  reformat or move code you do not own.

### Package S — determinism soak and a permanent twice-run gate (gates trust; highest priority)

Owns: `src/test/selftest_combat.cpp` (mob-cap64 and any new gate),
`tests/baseline.json` rows for it, and whatever a found race lives in (name
the file in a board note before touching another package's file).

This is round 2's package R item 2, verbatim scope:
- 3+ runs each of the natural and `killEvery 4` arms with
  `SANDVOX_MOBCAP_DIGEST=1`;
- `SANDVOX_MOB_THREADS=1` vs the default;
- `SANDVOX_PHYS_THREADS=1/2/7`.

Attribute any divergence to its first tick and field with
`SANDVOX_PHYS_TRACE` and `Physics::DebugBodyStates`, then FIX the cause.

Land a permanent gate (e.g. `mob-cap64-twice`). It runs a shorter brawl
twice in-process, with different mob and phys thread counts, and requires
identical per-tick digests. It must be fast enough to sit in every perf
package's `--verify` (aim for under 20 s).

Report whether the divergence M saw was real. If it was, give the root cause.

#### Package S result (2026-10-05)

- **Cross-process soak, frozen b21d23a exe: no race.** `mob-cap64` with
  `SANDVOX_MOBCAP_DIGEST=1`, natural and `killEvery 4`, each run as default x3,
  `SANDVOX_MOB_THREADS=1`, and `SANDVOX_PHYS_THREADS=1/2/7`: 14 boots, all 300
  digests identical within each arm. M's tick-208 split did not reproduce.
- **In-process: two real leaks, both fixed.** Gate `mob-cap64-twice` found
  them (DESIGN.md "The brawl is gated reproducible"):
  - `MobSystem::Reset` kept three tick-to-tick queues: pending drips, splatter
    bursts and body bursts.
  - `Mob::DetachLimb` / `ReleaseRigToDebris` handed an empty collider lattice
    to `AdoptBody`, which refuses it. The result was an unowned Jolt body
    falling forever.
- **The fight moves.** The leak fix changes a fresh-process fight: natural
  `mob-cap64` parts from b21d23a at tick 130, 53 alive instead of 54. A perf
  package measured against the b21d23a before-arm is no longer on the same
  fight as a tree that includes S. Re-take the before-arm on main after S
  merges.
- The `determinism` hash did not move (0fa43063).

### Package C — physics: CCD, collide, terrain mesh

Owns: `src/phys/physics.*`, `src/phys/debris.*`, and the terrain-collider
mesher (whatever `terrainMesh` times).
1. Per-body motion quality before each `Update`. CCD (`LinearCast`) only when
   `|v|·dt > 0.375 × min(local-bounds extent)`; otherwise `Discrete`. See the
   ranked list item 1 above. Consider casting living ragdolls as their hull,
   as corpses already are. Keep the terrain no-tunnel gates green.
2. `collide` is 7.4 ms CPU across the Jolt workers.
   - Measure which pair kinds dominate the narrow phase now.
   - Fix it with layers, cheaper shapes, or sleep thresholds for settled
     corpses and debris.
3. `terrainMesh` 0.83 ms. Find out what it re-meshes per tick in a fight, and
   whether it needs to.

### Package F — the real frame: pacing spiral + render side of 64 bodies

Owns: `src/main.cpp` frame loop / tick pacing and `--brawl`; render-side body
code (`src/gpu/`, body raster shaders) if measurement says so.
1. The spiral. See the ranked list item 2: a CPU tick budget. After the first
   tick of a frame, run another only if `spentTickMs + lastTickMs <= kTickDt`.
   Otherwise drop the debt. Add a dropped-tick counter, and an env A/B arm.
   Headless runs and `SANDVOX_TICKS_PER_FRAME` must not take this path.
2. Measure `--brawl` in the real windowed loop, before and after: frame
   p50/p95/worst, ticks per second, dropped ticks.
3. Then the render side with 64 creatures on screen. Use a crowd camera
   (add a `--render-budget` arm or a `--brawl` view if needed). If body
   rendering, micro-body brick uploads, or per-frame CPU body work is
   significant, fix the dominant term.
4. The per-frame CPU outside the tick, e.g. anything that walks all 64 rigs
   every frame for interpolation, audio, or the HUD.

### Package B — burn: parallel head under a prefix-split budget, amortised recount

Owns, in `mob.cpp`: the burn region (`BurnTickHead`, `BurnOneLimb`,
`FlushBurn`, `BurnShareOf`, `RecountBurn` and their helpers),
`src/game/burnprof.h`, and `src/game/workpool.*` if extended.
1. Ranked item 3 above. Shares are computed up front in pot order. Per-limb
   burns run in parallel into per-creature buffers. Then a serial flush in
   pot order. Name the budget shift.
2. Ranked item 6: `RecountBurn`'s 12 ms one-creature spike. Make it
   incremental, or amortise it on a (tick, id) schedule. The worst tick is
   the target here.
3. Keep `mob-burn`, `burn-*`, `corpse-*` burn gates green.

### Package X — stroke and carve (the blade path)

Owns, in `mob.cpp`: the stroke/strike/carve region (`CarveLimb`, the stroke
tests, sever/rebuild). Also `src/phys/lattice.h` (`SpallGrow`,
`DownsampleSkin`) and `src/game/melee.*`.
1. `SANDVOX_SAMPLE_PROF=1 --gate mob-cap64`: the stroke 3.78 ms (worst 15.78)
   and carve 2.38 (worst 9.98) at function level.
2. Fix the dominant terms:
   - `SpallGrow`'s per-round `unordered_set`;
   - allocations per carve;
   - collider rebuild cost (`rebuild` 0.32);
   - redundant stroke tests (spatial culls against limb AABBs cached per
     tick).
3. Can carves or rebuilds for DIFFERENT creatures run in parallel, with a
   serial apply in id order? Do it if the profile says it is the lever.
   The worst tick (32 carves on one tick) is the target as much as the mean.
4. Keep the blade, wound, sever and cleave gates green.

### Package K — shocks, stain, splatter, bleed, twinSync

Owns: `src/game/mob_shock.cpp`. In `mob.cpp`: the stain / splatter / bleed /
twinSync regions.
1. Ranked item 4: split the shock solve into connected components by box
   contact. Solve them in parallel and emit ops in part order. The result
   must be bit-identical: show digest equality.
2. Stain 2.28 ms (stainContact 1.02 over 138k calls a tick): cull contacts
   that cannot transfer anything, cache per-tick limb data, parallelise per
   creature with an id-order merge.
3. Splatter 0.47, bleed 0.58, twinSync 0.42: whatever the profile shows is
   cheap to take.
4. Keep `elec-*`, `corpse-splatter`, `stain-*`, and the bleed gates green.

### Merge order

S first. If S finds a race, fix it before the perf numbers are trusted. Then
C, F, B, X and K, as each is reviewed. The orchestrator re-measures the
combined tree once, rebaselines the determinism pin once, and rebuilds
main's exe.

### Round 3 result (2026-10-05, all six merged, main d34fb41)

Merge order: S, F, C, X, B, K. The determinism hash did not move in any
package (0fa43063), so there was no rebaseline. Final `--verify` (15 gates)
passes: `determinism`, `mob-cap64`, `mob-cap64-twice`, `body-fastfall`,
`big-body-collider`, `ragdoll`, `corpse-armor`, `debris`, `blade-wounds`,
`cut-path`, `corpse-burn`, `garment-burn`, `elec-crowd`, `corpse-splatter`
and `wound-rebleed`.

Each package's change, measured clean on its own:

| Package | Measured change |
|---|---|
| S | No race: 14 boots identical across processes and mob/Jolt thread counts. Two in-process leaks fixed. New gate `mob-cap64-twice`. |
| F | CPU tick budget: a forced 35 ms tick's frame p95 went 161.6 -> 44.3 ms. At today's cost the real `--brawl` holds 30.0 ticks/s, p50 19.9 / p95 30.9 ms. Bodies on screen cost about 1.5 ms of CPU per frame. |
| C | Jolt 3.83 -> 2.37 ms; ccd 2.01 -> 0.61; terrainMesh 0.82 -> 0.56. |
| X | Exact. stroke 3.79 -> 3.13 (worst 15.3 -> 11.1); carve 2.38 -> 1.78. |
| B | burnLimbs 2.85 -> 1.56; recount worst 12.2 -> 0.68. Budget shift named in DESIGN.md. |
| K | Exact. shocks 2.15 -> 1.17; stain 2.23 -> 1.77. |

Combined tree, `mob-cap64`, two runs:

| | b21d23a | 5 merged (no K) | all 6 |
|---|---|---|---|
| tick wall mean / p95 / worst | 31.73 / 39.90 / 47.31 | 28.39 / 36.51 / 41.11 | 30.28 / 39.24 / 47.18 |
| mob side mean / worst | 14.29 / 32.77 | 12.15 / 22.06 | 11.38 / 26.14 |
| Jolt Update | 3.80 | 3.66 | 5.75 |
| alive at end / debris bodies | 54 / 97 | 54 / 110 | 50 / 156 |

**The single 300-tick fight is now too chaotic to rank combined trees.** The
all-6 run's fight had 40.8 limp ragdolls a tick casting as box compounds
(the 5-merged run had none listed). Living-vs-limp contacts were 442
manifolds a tick against 266, and body-body contacts 1,099 against 623,
so Jolt rose 2 ms in a tree with no new physics code. The mob side keeps
falling (14.29 -> 11.38) despite a bigger fire: front 3,057 vs 2,346, and
seed probes 2,688 vs 1 a tick.

## Round 4 (2026-10-05)

What the round 3 numbers point at:

### Package W — the measuring stick, the work pool, the page table

1. **A stable perf measure.**
   - Add `SANDVOX_MOBCAP_FIGHTS=<n>` (or a baseline.json knob): it runs
     `mob-cap64`'s brawl n times with n different deterministic seeds
     (spawn jitter or mix order) in one process.
   - Report per-stage means across fights, plus the spread (min/max of each
     fight's mean).
   - Every later package quotes n=3.
   - The default stays one fight, so `--verify` cost is unchanged.
2. **Work pool wake latency** (package K's measurement: ~100 µs of wake and
   join per pool call, 15-20 calls a tick). Add a bounded spin before the
   condition-variable sleep, on the workers and on the waiting caller.
   - Prove it with an instrument: per-call overhead, i.e. wall time minus
     the longest task.
   - Determinism is unaffected (scheduling only); `mob-cap64-twice` stays
     green.
   - Do not burn a core when the game is idle. The spin must end in a
     sleep within a fraction of a millisecond.
3. **`pageTableCpu` ~0.9 ms a tick.** Find what it does per tick in a fight
   and cut it. `src/sim/pagetable.*`; read `docs/PLAN_page_table.md` first.

#### Package W result (2026-10-05)

DESIGN.md "Three fights, not one" has the detail.

- **The measure.** `SANDVOX_MOBCAP_FIGHTS=3` (or `mobCap64.fights`) runs
  fight 0 (the brawl, unchanged) plus variants 1 and 2 (the mix rotated, each
  creature nudged by a voxel) in one process. It ends with
  `mob-cap64: fights x3 <metric> <mean> [<min> .. <max>]` for every metric.
  On one tree the tick-wall mean spans ~1.7 ms across the three fights, the
  mob side ~0.5 ms, and Jolt ~1 ms.
- **Pool.** Items-done join (generation-tagged CAS claim), with a 200 us
  bounded spin on the workers and the caller. New instrument line:
  `mob-cap64: work pool (...)`.
- **Page table.** New instrument line: `mob-cap64: pageTableCpu by step`.
  Big `SlotSet` dilations and unions are word-wide; the occupancy and
  dirty-flag scans use SSE2.

n=3, one binary, back to back (before = `SANDVOX_POOL_LEGACY=1
SANDVOX_PT_BULK=0`):

| ms per tick | before | after |
|---|---|---|
| pool overhead | 0.612 [0.583 .. 0.628] | 0.127 [0.116 .. 0.135] |
| pageTableCpu | 0.925 [0.907 .. 0.941] | 0.362 [0.350 .. 0.374] |
| mob side | 10.95 [10.78 .. 11.08] | 10.04 [9.78 .. 10.27] |
| tick wall mean | 26.10 [25.36 .. 26.82] | 24.18 [23.21 .. 24.96] |

Same fights: fight 0's per-tick digests are identical to main's exe. The
`determinism` hash did not move.

Left:
- the pool's remaining ~0.13 ms (a 1,000 us spin halves it, but the mob side
  does not change);
- in pageTableCpu: the occupancy streak scan (0.065), the free probe's own
  submit (0.064) and harvest (0.037).

### Package L — physics: knocked-down creatures and the contact solve

1. **Limp ragdolls are box compounds.** In the all-6 run they cast 40.8 a
   tick and press against living limbs (442 manifolds a tick).
   - Give a limp ragdoll's limbs a convex hull collider while limp, as
     corpses already have (package P). Restore the detailed shape when the
     creature gets up.
   - Melee rays re-check the exact shape, as for corpses.
   - Measure the swap cost (hull build) against the gain. Cache hulls per
     limb lattice version.
2. `solveVel` 8.9 ms CPU and `collide` 15.7 ms CPU in that fight. With hulls
   on the downed, measure again. Then consider velocity-iteration counts per
   body class and sleeping thresholds for limp bodies under a living body.
3. Package C's open item: living limbs as hulls for body-body pairs, while
   melee keeps exact. Measure the melee re-cast cost before deciding.
4. Gates: `ragdoll` (all sub-checks; get-up must still work), `corpse-armor`,
   `body-fastfall`, `big-body-collider`, `debris`, `mob-cap64`,
   `mob-cap64-twice`.

### Round 4 result (2026-10-05, W + L merged, main 28c8209)

Measured with W's multi-fight arm: `SANDVOX_MOBCAP_FIGHTS=3 --gate
mob-cap64`, exclusive. Each value is the mean of three fights, with
[min .. max] across them.

| ms per tick | b21d23a (1 fight) | round 4 (n=3) |
|---|---|---|
| tick wall mean | 31.73 | 21.98 [21.32 .. 22.62] |
| tick wall p95 | 39.90 | 27.43 [25.93 .. 28.88] |
| tick wall worst | 47.31 | 33.98 [29.46 .. 42.62] |
| mob side mean | 14.29 | 9.92 [9.61 .. 10.23] |
| mob side worst | 32.77 | 18.06 [16.21 .. 18.98] |
| Jolt Update | 3.80 | 1.52 [1.18 .. 1.70] |
| pageTableCpu | 0.91 | 0.35 |
| readbackStall (harness-only) | 10.75 | 8.19 |

Mob side + Jolt is about 11.4 ms, against the round-3 target of under 10.
The mob-side worst tick is 18 ms, against a target of under 16.

What's left, by stage:
- stroke 3.10 (serial in mob order);
- stain 1.58;
- burnLimbs 1.20;
- shocks 0.95;
- woundStain 0.46.

Open owner decisions:
- `ragdoll-dress` fails since L: a 200-voxel fall stretches a limp limb's
  joints 4.39 voxels against a 4.00 cap. The options are written in
  `tests/baseline.json`:
  - raise the cap;
  - keep box shapes on a fast-falling limp limb;
  - drop limp hulls.
- The B, C, F and L trade-offs, as named in DESIGN.md.
- Parallel carves across creatures. That changes the fight, because later
  probes in the same tick meet an earlier carve's rebuilt body.
