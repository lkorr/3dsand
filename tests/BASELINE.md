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

## In progress: melee "hilt in fist" (NOT baselined — deliberately red)

The melee sub-gate now asserts that the sword's authored hilt box actually
overlaps the hand's box while equipped, and **that assertion currently fails**
(`hilt in fist=0`, gap ~2.75 world voxels). It is NOT in `baseline.json`, so
the mob gate reports a REGRESSION and the run exits 1.

That is intentional. The check was added because the sword was hanging at the
character's feet while every existing melee assertion passed — an edge that has
come loose from the hand still moves and still carves, so nothing caught it.
Baselining the new check would recreate exactly the blind spot it exists to
close.

Fixed so far: the grip offset (was `-SWORD_GRIP` micro, authored as if the
socket sat at the pommel butt rather than the fist centre), and the held slot's
placement path (it now goes hilt-to-hand directly from the hand's live
transform, instead of through the `anchorLimb`/`restOffset` pair, which is
measured against the model corner and does not survive Jolt's centre-of-mass
recentring of a compound shape).

Remaining: the X axis lands inside the fist; Y and Z are still ~2.75 voxels
out. The hilt box's scene→engine conversion has been verified against
`scripts/geometry.py` and is correct, so the residual is elsewhere in the
compose — most likely the socket point or the hand-frame rotation, not the
item.

```bash
./build/Release/sandvox.exe --selftest --gate mob   # look for "melee:"
```

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
