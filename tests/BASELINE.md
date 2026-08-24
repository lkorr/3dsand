# tests/baseline.json — what already fails, and why

`baseline.json` records which selftest gates were failing at a known commit.
`--selftest` diffs against it:

- a gate failing **and** marked `"fail"` → reported as *known-failing at
  baseline (not yours)*, run stays green
- a gate failing and marked `"pass"` → **REGRESSION**, run turns red
- a gate passing and marked `"fail"` → *FIXED since baseline*, flip it to
  `"pass"` in the same commit

It exists to retire the attribution ritual in CLAUDE.md — "build clean `HEAD`
before blaming your change". That took ~15 minutes of rebuild. This takes none.

## `determinismHash` — the golden world hash

`"determinismHash": "7cfa2420"` pins what the determinism gate must actually
produce, and it is checked separately from the pass/fail entries: a mismatch is
a **REGRESSION** and turns the run red even though the gate is marked `"pass"`.

It exists because the gate's own check is weaker than it looks. It runs the sim
twice and compares the two hash sequences — that proves the sim is
*reproducible*, not that it still simulates the *same world*. A change that
makes the sim quietly do less is perfectly self-consistent and stays green. The
Vulkan port's phase 2b found precisely that: a build in which the mutate and
explode passes dispatched **zero workgroups** passed the full suite. Pinning the
value converts "the sim agrees with itself" into "the sim agrees with the world
we recorded", which is what every later change is actually relying on.

**When to flip it.** Any intentional change to hashed state legitimately moves
the hash: a material or reaction edit, a `sim.*` tuning value, worldgen, or a
sim kernel. Those are content changes, not regressions — run the gate, take the
new value, and update the key **in the same commit as the change**, the same
discipline as flipping a known-failure to `"pass"`:

```bash
./build/Release/sandvox.exe --selftest --gate determinism
# determinism: FAIL (final hash 91ab00de over 200 ticks)
#   GOLDEN HASH MISMATCH: baseline says 7cfa2420, ...
```

then set `"determinismHash": "91ab00de"`. The commit message should say what
content changed and why the hash moved — a flip with no explanation is
indistinguishable from someone silencing a real regression, which is the one way
this key can make things worse than not having it.

**When NOT to flip it.** If you did not intend to change sim behaviour, a
mismatch is the gate doing its job: something in the tick path stopped doing
what it did. Do not update the value to make the run green. Note also that a
mismatch on a *different GPU vendor* is a rule-1 cross-vendor determinism
finding (DESIGN.md risk #3), not a reason to re-record.

Deleting the key disables the check: the gate reports `not pinned` and passes on
self-consistency alone, i.e. the old behaviour.

**What it cannot see: anything below y = 0.** The determinism gate's world sits at
the DEFAULT residency-window origin `(0, 0, 0)` (`world.h`, `origin_{0, 0, 0}`) and
never shifts — only the `streaming` gate flies. So the hashed window is **y 0..511**,
and worldgen's whole deep-cave layer is outside it: `caveAt`'s band 2 lives entirely
below y = -2, and only the y >= 0 slice of band 1 is in range. A worldgen change that
rewrites every cavern and every lava pool in the world can therefore leave `7cfa2420`
completely untouched, which looks like "my change was hash-neutral" and is not the
same statement at all.

Measured, not assumed (2026-08-24, the magma-table change): moving the lava fill level
to `LAVA_LEVEL = 10`, inside the window, moves the hash to `f3236b6f`; moving it to
-80, below the window, does not move it at all. If you are changing worldgen below
y = 0, the golden hash is not your gate — `--gate streaming` is, because it flies away
and back and compares hash sequences across 226 shifts.

The file is a **flat string→string map** with no comments: the parser is a
deliberately strict hand-rolled scanner (no JSON dependency, since it runs
before anything else is initialised), and prose keys in the file itself once
caused a comment to absorb a later gate's verdict. Rationale lives here instead.

## Current known failures — recorded at `46b7ec7`, 2026-08-21, RTX 3060 Ti

Both were verified pre-existing: they fail identically at `46b7ec7` with the
old monolithic selftest, before the gate split touched anything.

### `pond-freeze`

```
pond freeze: FAIL (rim 0/96 vs middle 0/25 ice at 250 night ticks;
                   0 ice voxels, 0 frozen with 0 non-water neighbours)
```

**Zero ice forms at all.** This is not the rate-gradient problem the gate was
written to measure (rim should freeze faster than middle) — the water→ice
reaction never fires even once. Rim and middle are both 0.

Where to look: the night gate on the reaction, or its `scaleByNeighbors`
`minCount`. Note the CLAUDE.md entry on light-gated rules never sleeping — a
long-lived condition that skips `keepAwake` interacts with this. Reproduce in
~8 s with:

```bash
./build/Release/sandvox.exe --selftest --gate pond-freeze
```

### `mob`

```
micro body render: FAIL (10 micro slots, 7/14 views drew, ...,
                         0 cube instances from micro limbs)
  critter INVISIBLE from 7 view(s); first is dir (1,-1,1)
```

The `mob` gate aggregates several sub-gates; only **micro body render** fails.
The critter is invisible from 7 of 14 view directions, and no cube instances
come from micro limbs. A solo micro body draws from all 14 views, so the fault
is in how mob *limbs* get instanced into the micro pass, not the pass itself.

Every other mob sub-gate passes: steering, gait, carve, dismember states,
avatar, avatar footfalls, melee.

This one is slow to reproduce (the mob gate is the longest in the suite, ~20 s)
but still far cheaper than the full run:

```bash
./build/Release/sandvox.exe --selftest --gate mob
```

## `daylight-boundary` passes in the suite and FAILS ALONE — and that is the finding

Marked `"pass"`, because it passes in the full run, which is what the entry
means. But `--gate daylight-boundary` on its own **fails**, on clean `f8c1bc7`
and on `HEAD`:

```
solo:     peak pages 32768 / 32768, exhausted=1, pageFaults 0   FAIL
in-suite: (same gate)                                           PASS
```

Do not "fix" this by baselining it to `"fail"` — that was tried, and it is
wrong in the direction that matters: it would turn the *suite's* green into a
recorded failure and hide a real regression later.

**Why the two disagree.** The gate asserts `PagesInUse() < PoolPages()`
throughout a daylight crossing, to prove `PLAN_page_table.md` §3.8's fatal
`std::abort()` stays unreachable. Run alone, the gate starts on a freshly
generated full window with the pool near its worldgen high-water and
`EncodeWakeAll` then dirties all 32,768 slots at once. Run in the suite,
earlier gates have already demoted a large fraction of the window to
EMPTY/UNIFORM/JITTER sentinels, so the same wake-all lands with real headroom
(the suite reports a 15,861/32,768 high water). Same code, different starting
residency.

**So the margin the gate measures is a function of what ran before it, which
means the gate currently does not measure what it claims to.** The abort is
unreachable *in the suite's world*; the solo run is the honest adversarial
case, and it says a daylight boundary on a fully-materialized window has zero
margin. Nothing aborts even then — `everExhausted` is a `>=` watermark, not an
allocation failure, and `pageFaults` stays 0, so no voxel is lost.

Two ways it misleads, both worth knowing before touching it:

  * **`--residency dense` reports the identical `32768 / 32768` and PASSES.**
    Dense is the identity map, so `PagesInUse() == PoolPages()` by
    construction; the gate deliberately gates `everExhausted` on `paged`. That
    pass is not evidence the pool is healthy.
  * **It is not the renderer's.** It reproduces with no shader changes, and
    the far-field/LOD work is render-only (`farVox` is derived data, never
    paged).

Worth fixing properly: either have the gate establish its own residency
precondition (materialize the window first, so solo and in-suite agree), or
narrow §3.8's "intersect nonSentinel" materialization filter so a wake-all
cannot demand the whole window regardless of starting state. The first makes
the gate honest; the second makes the engine safe. They are not the same job.

Reproduce the disagreement in ~1 s:

```bash
./build/Release/sandvox.exe --selftest --gate daylight-boundary --json day.json  # FAIL
./build/Release/sandvox.exe --selftest                                           # PASS
```

## Resolved: melee "hilt in fist"

The melee sub-gate asserts that the sword's authored hilt box overlaps the
hand's box while equipped. It PASSES (gap about -1.5 world voxels, i.e. the
hilt sits well inside the fist).

Worth recording because it took two independent fixes from two sessions:

  * the grip offset and the held-slot placement path (this branch), and
  * `c3431d4`, which fixed the debug collision boxes being offset by the shape
    centre of mass.

The second one mattered here because the grip is derived from
`Physics::GetLocalBounds`; while those bounds were off by the centre-of-mass
shift, the sword was genuinely misplaced in y/z and no amount of reasoning
about the item's own frame would have found it.

The check exists because the sword spent a while lying at the character's feet
while every other melee assertion passed — an edge that has come loose from the
hand still moves and still carves. Do not baseline it away.

## `fluid-react` — FIXED 2026-08-24

Was `"fail"` since the WP1 fluid-lab merge (c4f4ba7). The root cause was
exactly what the diagnosis below predicted: the gate started from
`CurrentTuning()` (the user's live tuning.json) and only pinned five of the
~30 `sim.fluid*` params. The user's tuner session had dragged cohesion to 32.9
and attractDiff to -1.08, which changed the fluid physics enough to shift the
mass consumed by reactions below the gate's threshold.

**Fix:** pin every `sim.fluid*` parameter in both `GateFluidReact` and
`GateFluidExcite` to its `tuning_params.def` default, with the gate's own
overrides (exciteMode, damping, stiffness, settleEps, wakeSpeed) applied on
top. This makes the gates hermetic: their outcome depends only on the .def
values and the gate's own authored overrides, never on the user's tuning.json.
The same fix was applied to `GateFluidExcite` which had the identical weakness.

Gate result after fix: `fluid react: PASS (739 eighths consumed by reactions,
plants 25 -> 123, 315 standing + 1650 live + 739 consumed of 2704 placed,
world hash matches)`.

Original diagnosis for the record: Recorded `"fail"` by the WP4 perf agent,
2026-08-24. Three independent board notes confirmed it was pre-existing (not
WP4): agent-ea1608, agent-fea2b6, agent-085345. The WP1 agent (agent-f65818)
reported it passed at pure `tuning_params.def` values; the merge kept three
user retunes (cohesion 32.9, attractSame 0, attractDiff -1.08) in
`tuning.json`. The `fluid-stain` gate has the same structural weakness
(unpinned fluid params) but is currently passing; it should be hardened the
same way.

## Updating

After fixing a gate, run it alone, confirm it passes, and flip its entry to
`"pass"` in the same commit. Leaving a fixed gate marked `"fail"` means it can
silently regress again without turning anything red — the one way this file can
make things worse than no file at all.

If a gate fails for you that is marked `"pass"` here, and you believe it is not
your change: run it alone at your commit, then `git stash` — no, **don't**
(see the stash hazard in CLAUDE.md) — instead build a clean checkout of the
merge-base in a separate worktree and run that one gate there. The point of the
baseline is that you should rarely need to.
