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
