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

## `fluid-det`, `fluid-settle` — CLOSED by WP3 (2026-08-24)

These were recorded `"fail"` at `987c595` ("Fluid stock defaults: the owner's
chosen fast-water look"), together with `fluid-excite` and `fluid-stain`, as
deliberate collateral of a look choice. That commit named the repair itself —
"the principled way to make this look legal is MORE SUBSTEPS (>=9 at this
stiffness), not lower stiffness" — and WP3 did exactly that. Both are back to
`"pass"`; this section is kept, per the removal condition it carried, to say
what closed it rather than vanishing.

* `fluid-det` was never a determinism failure. Both runs always printed
  `world hash matches`; what tripped was the `escaped` sanity assert — 77 of
  512 particles flung out of the basin by a clamp-saturated 6-substep solver.
  At 9 substeps: **0 escaped, 512 of 512 eighths settled, PASS.**
* `fluid-settle` went from 0 settled / 1,280 live / 40 picks all refused to
  **1,280 of 1,280 converted, quiet at tick 40, 0 live / 0 blocks, mass
  exact.** Two things had to be true at once: the substep fix, and WP3's
  free-surface gravity-bias strip (`seamRestVy`) — at 900 vox/s^2 one substep
  of uncancelled gravity is 100 vox/s on every free surface, and since the
  calm test is a MAX over the chunk, a single surface particle was vetoing
  every pool in the engine.

`fluid-stain` is closed too, and it closed itself: the first full-suite run of
the WP3 merge reported it `FIXED since baseline` without anyone touching it.
Same cause as the other two — it asserts on contact staining, which needs
water that actually settles — so it is back to `"pass"`.

`fluid-excite` is the one that does NOT close. It keeps its own entry
immediately below: it fails on main too, WP3 improved it 3.6x without reaching
the threshold, and the honest repair is the fixture's rather than the seam's.

## `fluid-react` — FIXED, flipped to `"pass"` (WP3, 2026-08-24)

It was tuning-sensitive arithmetic, exactly as the note below predicted, and
the fix was already on main before WP3 touched anything: commit 987c595 set
`cohesion` 32.9 -> 0 and `attractSame`/`attractDiff` -> 0, which are the three
tuner-session retunes the WP1 merge had carried into `tuning.json`. Measured
2026-08-24 at main b231920 (739 consumed, plants 25 -> 123, 315 + 1650 + 739 =
2704 EXACT) and on the WP3 branch (960 consumed, plants 25 -> 138, 407 + 1337 +
960 = 2704 EXACT). Both PASS, both mass-exact, both hash-stable.

So the entry had been stale since 987c595 and nobody re-ran it. The original
diagnosis is kept below because it is the one that turned out to be right.

> Recorded `"fail"` by the WP4 perf agent, 2026-08-24. It is NOT a WP4
> regression, and three independent board notes from that day say so before WP4
> started: agent-ea1608 ("post-baseline gate, MPM mass accounting, unrelated"),
> agent-fea2b6 ("still fails — pre-existing, not mine"), agent-085345 ("fluid-
> excite AND fluid-react fail identically with worldgen.wgsl reverted to HEAD ->
> pre-existing, arrived with c4f4ba7, neither is in baseline.json").
>
> The likely cause is on the record too, from the agent who landed WP1
> (agent-f65818): the WP1 branch PASSED this gate at pure `tuning_params.def`
> values, and the merge kept three of the user's tuner-session retunes
> (cohesion 32.9, attractSame 0, attractDiff −1.08) in `tuning.json`. So the
> gate is almost certainly tuning-sensitive arithmetic rather than broken
> plumbing.

## `fluid-excite` — broken by the owner's fluid defaults (987c595), NOT by WP3

**Verify before you inherit the blame: it fails on main.** Measured
2026-08-24 at main b231920, `--gate fluid-excite`:

    206 standing + 3850 live eighths of 4056, 2 settle picks   FAIL

The gate drains a sealed double-shelled chamber through a carved 4x4 plug and
asserts that MOST of the water (`> 3/4`) makes it back to settled voxels with
`live < 800`. It cannot, at gravity 900: a sealed box has nowhere to radiate
energy to, so the drained pool rings between its walls indefinitely, and the
settle test is a MAX over a chunk's particles. Measured residual at the end of
a 340-tick run, with the free-surface gravity bias stripped: **max 81.6 vox/s,
716 of 3278 particles above 0.9 vox/s.** The gate's own `fluidDamping = 0.9`
override exists to bound exactly this and is no longer enough at 9x gravity.

WP3 improved it 3.6x and it still fails:

| tree | standing | live | picks |
|---|---|---|---|
| main b231920 | 206 | 3850 | 2 |
| WP3, block-granular veto | 373 | 3683 | 18 (4 inf, 14 unstable) |
| WP3, column-granular veto | 747 | 3278 | 12 (4 inf, 8 unstable) |

Ruled out, with the instrumentation that says so:
* **Not the stability veto.** Disabling it outright still gives 759 standing
  — the veto is worth ~1% here, not the missing 60%.
* **Not mass written somewhere unaudited.** A WIDE sweep that counts water
  voxels in the walls too reports the identical 747, so the ~31-eighth gap
  between `standing + live` and the 4056 placed is not a mis-scoped audit. It
  is not a seam leak either: the seam's own ledger balances
  (2424 binned = 2094 settled + 330 left in refused columns, 2094 died). The
  gap scales with settled VOLUME (0 at 373 standing, 38 at 759, 31 at 747),
  which points at the gate's one-tick-stale `FA_LIVE` bound on the end-state
  particle sweep rather than at destroyed water. Not chased further.
* **Not reactions.** Every water-consuming rule in `reactions.json` needs
  `needsSky` or a `tag:hot` neighbour; the chamber is sealed under a double
  stone roof and the gate pins dim dawn.

The honest fix is the gate's, not the seam's: either give the chamber somewhere
to dissipate energy, or stop asserting near-total resettlement of a sealed box
at 9x gravity. Left for whoever owns the sealed-box fixtures.

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
