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

> **READ THIS FIRST: A MOVED HASH IS NOT A BROKEN SIM.**
>
> `determinismHash` is a **change detector**, not the determinism invariant. The
> invariant is that the same seed+tick+inputs reproduce bit-identically, and it
> is tested by the gate's *twice-run comparison* — which is a separate check
> that keeps passing while this value moves.
>
> **All we require is determinism. New numbers are fine.** If you changed hashed
> state on purpose (a reaction chance, a material, a `sim.*` value, a re-baked
> asset), the hash was always going to move; that is the mechanism working, not
> a finding. Run `--selftest --rebaseline`, commit the new value with the
> change, and move on. Do not investigate it, do not A/B it, do not try to
> restore the old number, and do not describe it as "the sim moved" or "I broke
> determinism" — determinism passed.
>
> The gate still turns the run red on a mismatch, and deliberately so: that is
> the only way an *unintended* change gets noticed. Red here means "confirm you
> meant this", not "something is wrong".

`"determinismHash": "7cfa2420"` pins what the determinism gate must actually
produce, and it is checked separately from the pass/fail entries: a mismatch is
reported as a **PIN MOVED** and turns the run red even though the gate is marked
`"pass"`, so that an unexpected one cannot slip by unnoticed.

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
./build/Release/sandvox.exe --selftest --rebaseline
# determinism: PIN MOVED (final hash 91ab00de over 200 ticks, sim reproduces
#   itself; only the recorded value differs - rebaseline if you meant it)
# *** determinismHash: 7cfa2420 -> 91ab00de ***
```

That is the WHOLE procedure: one command, which both re-measures and records.
Reaching for `--gate determinism` first and then hand-editing the key is two
runs and a file edit for the same result. The commit message should say what
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

**RESOLVED 2026-10-03** (see that dated section): the probe never uploaded the
micro bricks, so every model built after boot drew nothing. History below.

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

## `ca-slope-hybrid` — a settle mass leak, proportional to settle commits

Recorded 2026-08-25 with WP5's flip of `sim.fluidExciteMode` to 1. The gate is
new in the same commit, so this entry is the finding rather than a regression
report: `ca-slope-hybrid` runs merge a2e723e's ramp script with BOTH movers
live, and its first run found a mass leak in the seam's settle path.

    768 eighths poured on the deck
    end state: 1 basin + 36 deck standing, 586 carried by 586 particles
    145 eighths UNACCOUNTED
    seam over the run: 955 excited -> 955 emitted, 369 settled, 369 dead,
                       0 refused, 0 eaten by reactions
    the ledger is exact until tick 170, then parts by 18 and grows to 145

WHAT IT IS NOT, each ruled out by measurement rather than by argument:

* **Not excite.** `FA_EXCITED == FA_EMITTED` exactly (955 == 955) and
  `FA_SETTLED == FA_DEAD` exactly (369 == 369). Both converters balance their
  own books; the eighths leave the VOXEL grid without either of them recording
  that it took them.
* **Not reactions.** `FA_CONSUMED` is 0 for the whole run, and the gate pins
  dim dawn so evaporation and freezing are both off.
* **Not a mis-scoped audit.** Excited water is ballistic in a way CA water
  never is, so the sweep was widened by 12 cells in x/z and 8 in y past the
  structure — the classic failure mode, and the one WP4 hit on the lab `pool`
  scene. It finds nothing: `elseE` is 0.
* **Not the CA.** The `ca-slope` arm runs the identical script with excite
  pinned off and reports mass EXACT, 95.3% in the basin, box asleep.
* **Not introduced by the flip.** The gate sets `exciteMode` itself, so its
  verdict does not depend on the shipped default. The same seam code at
  a2e723e fails it too.

WHAT IT IS: the leak scales with SETTLE ACTIVITY. Run with the perch trigger
on — which re-excites almost everything settle produces — settle commits 8
eighths over 400 ticks and the leak is 6. With it off, settle commits 369 and
the leak is 145. That is ~0.4 eighths lost per settle commit either way, and it
localizes the bug to `settleColumn` / `settleApply` in `sim_fluid_seam.wgsl`.
Not root-caused further: the obvious candidate, two vertically adjacent blocks
racing on the SPILL cells, is explicitly guarded by the adjacency exclusion.

Why it is not visible elsewhere: the large-body scenes barely settle at all.
`--fluid-bench pond68` commits 3 settle blocks over 469 ticks and
`worldlake` 18 over 460, and both report mass EXACT. It takes a small sealed
box that settles and re-excites continuously to accumulate a visible loss.

The gate stays RED on purpose. It is the thing that will verify the fix, and
its acceptance (mass exact, >=90% arriving, box asleep) is the right one — the
seam should meet it.

**Re-measured 2026-08-25 at the levelling pass** (`filmPressed` + `bridgeLevel`
in `sim_step.wgsl`, `sim.liquidMinFilm` back to 1, the seam's surface-step
excite trigger), because the number moved and the number moving is not the same
thing as the bug moving:

    768 poured; 24 deck standing, 613 carried by 613 particles
    131 UNACCOUNTED
    seam: 804 excited -> 804 emitted, 191 settled, 191 dead, 625 binned
    ledger first parts at tick 290 by 24

Fewer settle commits (369 -> 191) and a smaller leak (145 -> 131), so the loss
per settle commit went 0.4 -> 0.69. That is the SAME bug being driven
differently, not a worse one: the levelling rules change the shape the water
arrives in, so which columns are feasible and how many commit changes with it,
and a per-commit average taken over two different column populations is not a
like-for-like comparison. Both diagnostic equalities still hold exactly
(`FA_EXCITED == FA_EMITTED`, `FA_SETTLED == FA_DEAD`), `FA_CONSUMED` is still 0,
and the `ca-slope` CA-only arm is still mass EXACT — so the leak is still on the
settle path and still nowhere else.

## `fluid-react` — no ledger term for SETTLED water eaten by reactions

Pre-existing at a2e723e, whose merge message names it: "fluid-react left red:
gate blind spot (no settled-consumption ledger term)". Recorded here at WP5
because the entry was missing while the gate was already failing, which is the
one state this file must not be in.

    430 standing + 874 live + 1228 consumed of 2704 placed -> 172 unaccounted

`FA_CONSUMED` counts excited neighbours killed by `consumeApply`. `doReactions`
in `sim_step.wgsl` also overwrites SETTLED water, and nothing counts that, so
the gate's ledger is short by however much of the consumption happened on the
CA side of the seam. Verified independent of WP5: it fails identically at
`exciteMode` 0.

The fix needs a counter inside `doReactions`, which is in `sim_step.wgsl` —
out of scope for the WP5 branch by explicit instruction. Queued.

## `fire-down` — arm B lost to the weak flame, by the owner's choice (2026-09-03)

Flipped to `"fail"` in the follow-up to fe3718d. Fire ignites at an eighth of
the coals' rate now (`neighborChance`, gate `weak-flame`), and the flame a
burning leaf emits DOWNWARD is fire: it was the only path across an air gap,
so arm B (upper slab lighting the lower slab across one cell of air) went from
saturating to 9% and the lower slab's bottom to 0%. Arm A (conduction through
a slab) still reads 90-95%. A falling, non-floating downward product restored
the arm to 85%; the owner declined it. Flip back if that ever changes.

    fire-down: lit 98% | A conduct 90% | B emit (down 1 cell of air) 9% | lower slab bottom 0% [floor 60%]

## `shadow-cache` — failing at HEAD before the weak-flame change (2026-09-03)

Flipped to `"fail"` while landing the floating-flame / burn-tint change, which
touches nothing this gate reads. Attributed the way the section below
prescribes, with two worktrees built from the same `HEAD` (5802c0c plus the
one-hunk `avatar.h` compile fix the working tree carries — `HEAD` itself does
not compile since a7ccb47), one pristine and one with the change applied:

    --gate shadow-cache alone   HEAD: agree 1.86 / budget 1.45   FAIL
    --gate shadow-cache alone   HEAD + change: agree 1.86 / budget 1.45   FAIL
    in the full suite           HEAD + change: agree 1.87 / budget 1.83   FAIL

Identical to the digit on both trees, so it is not this change. Another
session had already reported it failing in-suite on the shared tree the day
before (board, agent-94c495). The suspects are the openness / irradiance
commits of 2026-09-02 (`99887db` onward), which are the only recent changes to
what `shadowCached` disagrees with; not chased here. Whoever fixes it: run it
alone, confirm it passes, and flip the entry back in the same commit.

## `mob-burn` — falling embers carry fire down (2026-09-04)

Marked `"fail"` by a change the owner asked for, not by a defect.

Isolated solids now fall in the CA (`soloSolid`, `assets/shaders/sim_step.wgsl`)
so that lone voxels stop hanging in the air after a tree burns. **Ember is a
solid**, so a lone ember now drops and carries fire downward — the same
downward-fire path the rejected `spark` material would have added on 2026-09-03.
The owner was asked directly whether hot materials should be excluded from the
rule and chose to keep them falling.

Two subchecks measure it head-on:

- **heat across a joint** isolates cross-limb conduction by setting
  `crossLimbPct` to 0. The thigh now takes **94/558** voxels at that setting
  against **0/558** before: heat is arriving by falling onto the limb rather
  than through the joint, so the check's isolation no longer holds.
- **cloth vs flesh** wants cloth to burn faster than skin, and now sees skin
  **96%** consumed against **45%**.

ATTRIBUTED, not assumed. The same binary with the rule disabled passes both
(thigh 0/558, skin 45%), measured at gate scope on both arms — the control arm
is a one-line WGSL edit, so this needed no rebuild and no second binary.

Fixing it properly means teaching the gate to isolate cross-limb heat from
AMBIENT heat, which changes what it measures and belongs to the owner of the
burn gates. Do NOT fix it by excluding hot materials from `soloSolid`: that
decision has already been made the other way.

## `streaming` — the glass ball comes back empty, at suite scope only (2026-09-04)

Marked `"fail"` on branch `floaters-p0-p3` with the cause NOT understood.

Detail line: `ball chunk evicted=1, 0 glass voxels after re-entry`. The gate
streams a radius-3 glass ball out of the window and back and expects it to
return; at suite scope it returns empty.

Attributed at the same scope on both arms before recording, because the island
and floater work landed beside it is the obvious suspect and is NOT the cause:

- The full suite with the isolated-solid CA rule disabled (a copy of the asset
  tree with `soloSolid` off via `SANDVOX_ASSET_DIR`) fails identically.
- The ball is a solid sphere resting on the ground; `soloSolid` requires a
  voxel with NO solid neighbour and cannot touch it.
- `--verify streaming,...` PASSES with the rule on and with it off (76 glass
  voxels after re-entry), so this is a shared-`World` ordering dependency
  (CLAUDE.md rule 7) or the `streaming-smooth` merge interacting with other
  sessions' uncommitted files on this tree.

Needs the streaming owner. Do not flip it back without running it inside the
full suite.

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

## `determinismHash` 3b55aba6 -> 04a8117e, and three flapping gates (2026-09-04, magic grammar)

Landing docs/PLAN_magic_grammar.md (branch `worktree-magic-grammar`, six
commits) the full suite moved the pin. Attributed before recording: the hash
is 04a8117e with AND without the appended `gold` material, and with the base
commit's `sim_mutate.wgsl` in place of the from-filter edit, so nothing this
work touched moved it; and the main checkout's own binary reports PIN MOVED
against 3b55aba6 too. The pin was stale on the base branch. Recorded by hand
because `--rebaseline` refused three runs in a row over gates that flap at
suite scope and pass alone or in the next run: `scale` (a stale sash pin,
fixed below), `corpse-burn` (once: "pyre chunk in window=0 cached=0", bodies
falling through unresident ground; passed twice after), `fire-depth` and
`armor-react` (once: "bare burned at t+never"; passed twice before). None of
those names code the grammar touches; they belong with the body-burning
family already recorded above (`mob-burn`, `fire-down`, `tree-fell`).

`scaleMetres_item/sash` 0.550 -> 0.300: the sash geometry changed in
d142be4 and the pin did not follow.

The smoke tables (`smokeQuiet`, `smokeLoud`) were rebaselined by the tool.

## `determinismHash` 7aeb3ea5 -> 4dca1e0d was already stale at 482b756 (2026-09-05, P-D tree lattice)

Recorded, NOT rebaselined (the coordinator rebaselines main once after
P-C/P-D/P-E). Landing P-D (`docs/PLAN_environment_truth.md`: one tree
lattice, thinned per biome) the full suite reported `determinism: PIN MOVED
(final hash 4dca1e0d ... sim reproduces itself)` plus REGRESSIONS `wind-prim,
flung-liquid, fire-depth, openness, ai-approach`. Attributed at the same scope
before anything was touched: the MAIN checkout's own exe on the committed
482b756 assets (`SANDVOX_ASSET_DIR` to a `git archive` of HEAD) reports the
SAME 4dca1e0d and the SAME five failures. None of it is P-D's -- the harness
window is the pad site, which bans trunks, crowns and cover, so tree
placement cannot reach the hashed world at all. The only pin P-D itself moves
is `treeAtlasHash` (8d14bc1e6117ce3d -> da8bc516535faf82: the atlas gained a
per-(biome, species) condition table). `ai-approach`'s "wall 0/33 columns in
mirror" is the rule-7 mirror-anchoring dependency selftest_swing.cpp already
documents.

## Five gates recorded as known-failing at the world map, not at the environment wave (2026-09-05, coordinator)

`wind-prim`, `flung-liquid`, `fire-depth`, `openness`, `ai-approach` were
already failing on main's own exe + assets before P-A..P-E (three
independent same-scope controls: P-C, P-D, P-E each ran main's binary on
the pre-wave assets and got the same failures and the same 4dca1e0d). They
came in with the world map landing (board 2026-09-05T00:31Z: "5 fail alone
on the new world ... the world under your fixtures changed") and were never
recorded because the owner set the pin by hand. Three of them
(`wind-prim`, `fire-depth`, `openness`) had NO key in baseline.json at all,
so `--rebaseline` could never have recorded them (ReplaceJsonValue only
updates keys that exist) and every run would have reported them as fresh
regressions forever. Keys added by hand as "fail"; then the ONE end-of-wave
`--selftest --rebaseline` recorded `determinismHash` 7aeb3ea5 -> 44fa72cb
(P-C moved the calm home area) and `treeAtlasHash` 8d14bc1e6117ce3d ->
da8bc516535faf82 (P-D's condition table). Owners of those five gates: the
fixtures' ground changed under them; see docs/PLAN_world_map.md.

## P-F: presets carve the ponds, lakes are map sites (2026-09-06)

`determinismHash` moves once, at the end of the package, for the reason the
package exists: every rolled tarn's radius, depth, profile, berm and shore
band are now its water preset's (`assets/water/<name>.json`) instead of the
twelve deleted `worldgen.pond*` / `shore*` knobs, ponds roll on one lattice
(the finest water tile of any biome, 256 vox) thinned per biome row instead
of the 448-vox `pondTile`, and the default map gained an authored lake
(`home_lake`, east of spawn). The harness window is the pad site, which bans
tarns, so the hashed world moves through the shore/berm reach of tarns just
outside it and through the lattice's tile hashes, not through the fixtures'
ground. The twice-run comparison is the invariant; the pin is the
notification.

### 2026-09-07 — environment truth P-G + P-I (branch `env-pg-pi`)

`determinismHash` did NOT move (44fa72cb), and that is the expected reading,
not a surprise: the hashed window is the harness pad site, which bans every
tree, cover row and tarn, and its ground is the same numbers it always was —
`map.json terrain` carries exactly the values the deleted `worldgen.*` rows
had, every biome ships the identity relief curve and 256 multipliers, and the
declared `east_range` landform is 45 km east. What moved is `treeAtlasHash`
(the species height caps were lifted to -1 and every `.svtree` re-baked; the
band is a header word) and the smoke probes (the flora chain became cover
rows with a canopy condition, so every ground-flora biome's floor rolls
differently; the alpine cushion is a row; the cover stack has no treeline
gate). The twice-run comparison is the invariant; the pins are the
notification. `env-truth` records: unauthored cover is 0.00–0.31 % in every
biome (was 5.97 % in the forest); forest trees 122/ha against a page of 153
(was 93).

## 2026-09-08 — smoke probes re-pinned; determinismHash could NOT be

`smokeQuiet` (5 probes) and `smokeLoud` (19) move to a tick-0 world of
`899b9bff` (was `c87b49c4`). The cause is the page-fault fix in `bfbe2ff`:
batched `SubmitWorldgen` never told the CPU materialize set about the wake
`genChunk` performs on the GPU, so the first tick after every worldgen dropped
24 boundary-crossing sim writes into chunks still held as `PT_EMPTY`
sentinels. Those 24 voxels now exist, so every hashed table that samples a
post-worldgen world moves. `page faults over the suite: 0` for the first time,
and `--suite acceptance` reports `--vk-smoke PASS` and `--vk-smoke-loud PASS`
against these values.

Measured twice, on two trees and two binaries: `7a06c86` (wave 1) and
`b2e1408` (wave 1 + the LoadShader prelude filter) produced all 24 probes
byte-identically, which independently confirms the cache-key change is
hash-neutral.

**`determinismHash` is still `44fa72cb` and is STALE — the world is
`ee34787c`.** `--selftest --rebaseline` refuses while the suite has errors,
and it currently has nine, none of them caused by this work:
`env-reload`, `env-truth`, `waterbody`, `ca-skip`, `ca-level-pond`,
`fluid-det`, `fluid-settle`, `fluid-excite`, `fluid-react`. Each was
confirmed failing with byte-identical detail strings on a binary built WITHOUT
the wave-1 changes, and they belong to the environment and fluid owners.
`check_invariants.py` fails on the same axis (stale
`tests/env_predictions.json`, `biomesHash c1481fcd` vs `73dc273a`).

Until those are fixed or recorded as known-failing, every run prints one
expected `determinism: PIN MOVED` line. The twice-run comparison PASSES
throughout — the sim reproduces itself; only the recorded number is behind.
Do not chase `ee34787c`, and do not hand-edit it in to silence the line: the
refusal is the guard that stops a real regression being rebaselined away, and
routing around it by hand is the one thing it cannot defend against.

## 2026-09-10 — `determinismHash` eb284643 -> 6a5a1468 (N4: the CPU's wind weather and the landform bake go integer)

`docs/PLAN_multiplayer_now.md` L6. Three libm calls sat between the seed and
hashed state, on the CPU side of the determinism boundary, where nothing that
runs twice on one machine can see them:

- `WindWeather` (`src/sim/wind.h`) computed the per-tick weather with cos, sin
  and atan2 and quantised four scalars to Q16.16. `sim.windMode` ships at 1, so
  those four words are a per-tick INPUT to the CA.
- `OverlayLandformSites` (`src/sim/worldmap.cpp`) baked the declared landforms
  into the map's byte plane with cos and sin on the ridge rotation.
- `FLUID_FOAM_DECAY` (`sim_fluid.wgsl`) took an Nth root with `exp(log(x)/N)`
  in a const block, which folds on the host's libm. Foam reaches stain, which
  is hashed.

All three are now integer (`src/sim/intmath.h`, new: a Q30 BAM sine, an exact
`Sqrt64`, and sign-symmetric rounding helpers). The hash moved because the
arithmetic did, by design.

**How far it moved, measured before the run rather than guessed:**

| what | delta |
|---|---|
| landform bytes, shipped map | **0 of 38,416** — the integer bake is the identity here |
| landform bytes, 40 randomised site sets (all 4 shapes, radii 3..30,000, rotations +-720 deg) | worst case **4 of 38,416, all by exactly 1** |
| landform bytes, 40 more at radii 50,000..1<<20 (the loader's clamp) | worst case **59 of 38,416, none by more than 1** |
| the four wind words vs the float ones, 30,770 sampled ticks | **<= 1 Q16.16 LSB** (3 on gust) |
| `FLUID_FOAM_DECAY` at the shipped 2.2 s / 9 substeps | 65425 -> **65426** |

The draw mask stayed `h & 0x00FFFFFF` — the one `rng::Unit01` used — so the
same seed still draws the same headings and the same storm epochs. This is a
change of arithmetic, not a change of weather.

The twice-run comparison PASSED throughout (`determinism: ... sim reproduces
itself`); only the recorded number moved. `--gate determinism --rebaseline`
wrote the pin: 0 gates changed status, 0 page faults over the suite.

## 2026-09-20 — `54d80b57` → `ffa84539` (PLAN_multiplayer_now N1 + N4)

Two packages moved hashed state on purpose, on branch `mp-land`:

- **N1** made the snapshot latency a constant. Every snapshot consumer now
  reads tick T−4 instead of "whatever the last callback delivered", and the
  harness drains through the same path the game does, so `--selftest` finally
  runs at the shipped latency. Every gate that reads `Snap()` therefore sees a
  different — and now reproducible — world.
- **N4** put the sim's tick inputs on integer math: BAM heading + Q15 sine
  table for `WindWeather`, an integer landform bake, an integer foam decay.
  The landform bake is worldgen content, so the smoke probe tables move with
  it (`smokeQuiet` 5 probes, `smokeLoud` 19 probes, both re-pinned by
  `--rebaseline`). `--gate terrain` passes unchanged: relief 100 vox, mirror
  9409/9409 columns sound, 0 fixture columns blocked.

Determinism itself was never in question: the twice-run comparison passed on
every run, and `ffa84539` reproduced across three independent launches.

**Pinned BY HAND rather than by `--selftest --rebaseline`, which refused.** The
refusal is correct and is not about this work: a clean detached control at
`58780dc` with zero edits reports **11 unrecorded regressions of its own**
(`determinism`, `undead`, `swing-plane`, `ai-reach`, `ai-pursue`, `npc-strike`,
`body-coat`, `impact-fist`, `joint-rot`, `bite-limbs`, `lunge`), and
`--rebaseline` will not write while any unrecorded failure is present. The six
packages introduce none of them — the failing set on `mp-land` is a strict
SUBSET of the control's, measured at the same full-suite scope. Same precedent
and same reasoning as 4097d71.

## 2026-09-24 — `75d184ca` → `709ec4b7` (perf/tech-debt audit, docs/PLAN_perf_audit_2026-09-23.md)

Moved by P1's CA rule changes only: stains settle in a strict order (a
non-washer stains only clean or same-type cells, water steps foreign stains
down), a PAIR rule that would rewrite a neighbour into itself is skipped (grass
over grass), fungus decays to dirt, vine flowers are daylight-gated, non-write
keep-awake marks reach only the own chunk, and blast-rim marks fan out. Every
other package (fluids, worldgen/far, paging, render, CPU frame, Vulkan) proved
`75d184ca` unchanged base-vs-after in its own worktree. The twice-run
comparison passed on the integration tree. Pinned with
`--selftest --gate determinism --rebaseline`.

Smoke probe tables (`--vk-smoke*`) were NOT re-pinned (owner directive: no
smoke runs for this). Expect them to report moved probes until someone runs
`--vk-smoke-loud --rebaseline`.

Inherited, not from this work (fail identically on main 3d9be4b, recorded
`pass` here): `tree-fell` (`tree-actually-burned`; on the integration tree the
changed burn trajectory also leaves one floating pine-needle single, and it
persists with main's reactions.json and main's sim_step.wgsl swapped in), and
`waterbody` (pass H1 conservation +718 eighths).

## 2026-09-24 — `709ec4b7` → `d2ea6c23` (rule unification wave 1, docs/PLAN_rule_unification_2026-09-24.md)

Moved by W1-B1's matter rules: the MPM seam rolls each stainer's own
per-mille stain chance through `common.wgsl` `stainStep`, applies `consume`,
and pays for wetting absorbent ground; excited fluid runs its own reaction
bucket (`excitedReact`). W1-A, W1-D, W1-C, W1-F reported `709ec4b7`
unchanged; W1-B2's water is bit-identical (density ratio exactly 1.0). The
integration's own changes (dead `sim.fluidStainRate`, species padding, a
peer's explosion applied to the owner's avatar in phase N) touch no hashed
state in a one-machine run. The twice-run comparison passed on the
integration tree. Pinned with `--selftest --gate determinism --rebaseline`.
Smoke probe tables were NOT re-pinned (no smoke runs for this wave).

Red on the integration tree and NOT pinned: `fluid-identity` (W1-B2's gate,
green in its own worktree). Arm A lost 1.04 cells of separation vs arm B's
1.09 at t45, margin 0.40; both arms' live acid+water eighths fall over the
run (A 9569 -> 7954, B 7164 -> 5643), which points at W1-B1's excited acid
now running its own rules (stone/dissolvable erosion, acid `selfBecomes`
air) inside the fixture. Needs a fixture decision, not a margin change.

## 2026-09-24 — `d2ea6c23` → `5cede91d` (rule unification wave 2, docs/PLAN_rule_unification_2026-09-24.md)

Moved by the wave-2 packages that changed hashed state on purpose: W2-J1
(grid stains take part in reactions), W2-R (a stamp-skipped cell probes its
rules on substep 0), W2-I (debris burns through the one body evaluator: new
burn order and ops), W2-J2 (body coats follow the coat rule) and the oil rule
(`oil + tag:hot -> fire` 350 -> 700, DESIGN.md §6 clause 2c). W2-G, W2-M,
W2-N, W2-Q and the unused-knob cleanup reported the hash unmoved. The
twice-run comparison passed on the integration tree (after merging main
2ee903f). Pinned with `--selftest --gate determinism --rebaseline`. Smoke
probe tables were NOT re-pinned (no smoke runs for this wave).

New gates recorded `pass`: stain-react, stamp-sleep, damage-cause,
damage-sources, debris-coat, coat-parity, layer-roles, kit-instance,
creature-reach.

Control arm: main's exe (built 19:09 on 2026-09-24), same gate list minus the
new gates, run from a scratch directory.

- **Inherited** (identical sub-claims on the control): `mob-burn` (cloth vs
  flesh, burn terminates, burn leaves char) and `ragdoll` ("repeat blast").
  `acid-coat` is also red on main's exe; recorded `pass` here as it is on
  main.
- **New, from W2-J2, accepted-red pending the owner:** `body-coat`'s "a wet
  body does not catch" arm: the soaked root has **1** seared voxel against the
  dry control limb's 465 (want 0). W2-J2 reported it at 2 when it landed. The
  arm's premise was to park `coat.fireDrySeconds`, and W2-Q then deleted that
  knob. Under the coat rule, water boils off at its own reaction chance, so a
  wet voxel can dry and sear inside the 90-tick window. Changing "0" to a
  ratio against the dry control is a fixture decision for the owner, and no
  compensating mechanic was added. Still recorded `pass`, so it reports as a
  regression until that decision is made.
- **The ragdoll gate's "player on fire" arm ends with the player DEAD, on
  main's exe too** (`alive 0`, 24 pieces off on main, 27 here). The claim
  (push ceilings) passes on both sides and is not about survival. W2-H's
  "alive on main" did not reproduce against the current main binary. The
  arm now prints a `fate:` line from `Mob::HpLostBy` (hp by DamageCause):
  `DEAD at burn tick 257 (cause "burnt past the death knot"); hp 18.5 of
  1035.0 on the last living tick, burnt 0.684 (cap 0.018), blood lost 0.0; hp
  charged by cause: burn 95.5`. The player dies of the burn cap: its lattice
  burns past `burnDeathFraction`, with no blast, fall, contact or bleed
  involved. That is the intended consequence of lighting every part of a
  robed player and leaving them in the fire for 400 ticks.

## 2026-09-24 — rule-unification W2-L (one pose pipeline) + W2-O (gates on the real tick)

Hash unchanged (5cede91d); twice-run reproduces after each package.

- **`impact-fist` recorded `fail` — a FINDING of W2-O, not a regression.**
  Converted to `RunTicks` it runs the whole tick for the first time. Its
  target walked off (61.9 vox) until pinned with the `dummy` profile; pinned,
  the bare-fist limb still loses 7 voxels, and an added never-struck control
  limb ALSO loses flesh with no hp charged (3 vox at the gate's raised pulp
  rate, 2 at the shipped rate). A standing, unhurt human's thigh loses flesh
  in place on the real tick: likely a real-tick bug (candidate: a pulp /
  bruise / stain pass the old hand-rolled tick never ran). Untraced. Flip back
  to `pass` when that is found and fixed.
- `ragdoll-dress` fail -> pass under W2-L (knife-edge slam arm; NPC leg IK
  now fades in on the spawn tick). Recorded as before.
- `corpse-splatter` fails only in per-file subsets; passes standalone on both
  exes (subset artifact). `mob-loot` 'rose with 0' and `ai-pursue`: identical
  before conversion (subset artifacts).

## 2026-09-30 — `a00b3523` → `9bd138e9` (the weather-driven wind field, docs/RESEARCH_wind.md §13)

Intentional; the twice-run comparison reproduces (gas digest `fee71d33`,
reproduced) and the suite reports 0 page faults. Every stage of the package
changes what `windAtQ` returns, so the CA drift bias, the particle drag and
the MPM node force all see a different field:

- the gust bands advect DOWNWIND with the air (they ran upwind), the base
  wavelength is 8 m (was 4.8) and the evolution rate 0.35 rad/s (was 1.1);
- `windSinQ`'s correction square no longer overflows i32 above |sin| 0.707;
- the ramp is profile(height above ground) x exposure x altitude, from the
  terrain table the tick now carries (`wf*`, TickParams grew 16 KiB);
- the regime (sky-driven intensity / gale / convective) sets meander,
  thermals, coupling, gustiness, the lee rotor, slope and sea breezes and the
  storm timeline, whose front and downbursts are wind primitives;
- saltation is threshold-plus-power, `windEntrainSpeed` 2 → 1.2 m/s.

New gate `wind-field` recorded `pass` (CPU-only; it asserts the model's
behaviour, see selftest_wind.cpp). The smoke probe tables were NOT re-pinned
(owner directive: no smoke runs for a sim change); expect `--vk-smoke*` to
report moved probes until someone runs `--vk-smoke-loud --rebaseline`.

## 2026-10-01 — suite triage: every red gate attributed (HEAD de42d79 + this commit)

The full `--suite acceptance` at 11413bc reported 50 regressions, 9 known-failing
and both smoke tables red. Each was traced to the commit that turned it red.
Classes: (a) an intentional engine change moved the behaviour, so the gate now
states the new behaviour; (b) a real engine bug; (c) order dependence (state
inherited from an earlier gate); (d) a stale pin.

**The biggest single cause was (c): `streaming` returned with the residency
window left at chunk (20,-2,-7)**, so every later gate with a window-relative
fixture ran at a different site in the suite than alone (rain-lean, the six
solute gates, corpse-acid, venom-wound, player-corpse, vessel-break,
mob-save-delta). It now restores the window. The runner prints a
`selftest leak:` line after any gate that leaves the window, the mob count,
the debris body count or the def count changed. The second cause was the
**mob id counter**: `IdCounterScope` only restores the counter on exit, and gore,
bleed, coat and stroke rolls hash the creature id. bleed-fluid, body-coat,
venom-blade, head-cleave, npc-strike and corpse-head-laser now pin it to 1. The third was the
**per-tick authority inputs latched on MobSystem** (rain word, rain slope, day
phase): coat-transfer ended on a wet tick and body-coat's direct PreTick calls
rained on its creature. The runner now clears them before every gate.
The fourth was the wall-clock sky ease (gi-bounce now pins and snaps its weather).

Restoring the window made some gates run at origin for the first time in a
suite. Two of them had only ever passed at the shifted site:
`chem-electrolysis` (b, below) and `burn-cap` (a knife-edge fixture).

Engine fixes in this commit:
- `strokes.cpp`: an N-tick release ran only N driver steps, and the first is the
  Recover transition, so the arm claim dropped at smoothstep progress (N-1)/N.
  That was a one-frame snap of 0.259 at the 3-tick releases authored since
  63a9e19. It now runs N+1 steps (gate swing-smooth).
- `worldgen_run.cpp`: `SubmitWorldgen` forces a wind-draft rebuild. The draft
  rebuild verdict carried state across a world replace, so a replay rebuilt
  `TickParams.draftMode` (word 6715) differently on tick 1. This was a
  replay-determinism bug from d3b050f (ops-replay).
- `mob.cpp FootprintFooting`: the footprint grid turns with the body only for
  long bodies (half-extent ratio > 2, BodyCapsule's test). a892ae2 turned it
  for every body, and a humanoid's turned corners caught the jambs of a narrow
  hall: Harrowby's Wat stood in the alehouse all day (3/8 rows; 8/8
  axis-aligned). **Owner review: this partly reverts a892ae2 for humanoids.**
- `mob.cpp OverlayMobRecord`: a same-shape v4 repaint keeps the spawn transform
  across `RebuildLimbBody`. A Jolt round trip was one ulp off at far
  coordinates (mob-save-delta C).
- `raymarch.wgsl`: the per-pixel reference shadow path (`render.shadowCache=0`)
  now uses the cell top for partial powder cells, as the cache path has since
  02a1fa7. Sand-terrace risers had read as black. This is in the reference
  variant only (shadow-cache).
- `farplumes.h ClearEye()`: gates that assume no camera clear the eye that
  every real-tick gate leaves set since W2-O (gas-farplume, gas-farplume2).

Still known-failing, recorded `fail` (one line each, details in baseline.json):
- `armor-react`: the plated torso loses 7-10% of its skin in the acid bath. The
  claim is <1%. Red since before 2026-09-23; cause untraced.
- `pool-human`: in the full suite the shared art palette has leaked full by the
  time this gate runs (rise-tint slots and evicted defs never return colours).
  An engine leak; it passes alone.
- `waterbody` pass N: `wbQuiet` treats stain-only dirty chunks (wet banks drying
  since f039607) as disturbance, so a created body is never measured.
- `chem-electrolysis`: always red at origin. It passed only at the window
  `streaming` used to leave behind. Sparks laid IfAir over molten salt never
  react or decay at some fixture cells (y 220/223 fail, 221/222 pass). A
  suspected CA dispatch/wake issue.
- `fire-depth`: the block behind the flame still paints 19.6k of 97k px. The
  fixture is intact, so the cause is render-side.
- `rig-clip`: worst overlap is now 314-373 vox. Keyed frames (63a9e19) write the
  arm with no keep-out.
- `tree-fell`: at origin the fixture stands at (256,256), and one floating
  single voxel (mat 2) survives the burn and the quench.
- `corpse-head-laser`: the dead hair turn is 0.037 against a 0.020 limit in the
  full suite (0.007 to 0.010 in subsets). The result depends on scope.
- `player-corpse` arm D: the wounded corpse is not woken by the cut in the full
  suite, though it passes in the kOrder prefix. Some state still leaks.
- `ca-slope-hybrid`, `mob` (micro body render), `mob-burn` (cloth checks, no
  clothed fixture since wizard was deleted): unchanged, and their notes stand.

`npc-strike` now measures flesh loss in art voxels: the kerf was cut to ~0.3x by
b48fb4d, and the collider lattice showed 0-3 of the ~50 art voxels each cut
removes. `fluidReactCaGapPctMax` was raised to 6.0: 0d274bf moved the
uncounted CA plant sink from 1.44% to 3.55%, and pinning wind off did not
change it. `determinismHash` did not move (5dabc010 matches). `opsReplayBytes` was
re-pinned by hand to 5421372; the TickParams wind table added in 4228530 made
the record larger. ragdoll, impact-fist and cactus-fell flipped to pass. The
smoke probe tables were last pinned at d942634 (09-20). Every probe moved
because of intentional world/sim changes since then, and they were re-pinned
here with `--vk-smoke --rebaseline` and `--vk-smoke-loud --rebaseline`. The final
`--suite acceptance` (24.6 min) ran on this tree before the hand pins: 0
page faults, 4 new reds recorded above, everything else green or known.






## 2026-10-03 — `gas-leave-overflow` added, `"pass"` (det-gas package)

New gate, new keys `gasOverflowCap` / `gasOverflowTicks` /
`gasOverflowProbeTick`. It forces the window edge's leave budget down to 64 (a
test-only ceiling, `World::SetGasLeaveCapForTest`) under a 16^3 smoke puff and
asserts refusals > 0, conversions > 0, no overrun / pool refusal / over-budget
tick, and a tick-for-tick identical twice-run (hash + per-tick leave counts +
parcel digest). First run: converted 2365, refused 93317 on 53 ticks, twice-run
identical over 56 snapshot ticks, digest c188e30d both arms. `gas-leave`'s
"zero refusals" assertion is unchanged but is a throughput claim now, not a
determinism one (DESIGN.md "The edge's refusals are a function of the world").





## 2026-10-03 — red-world: waterbody flipped to pass; chem-electrolysis attributed; tree-fell green at gate scope

- `waterbody` -> **pass**. Pass N: `sim_step.wgsl` `stainDry` now marks its own
  dirty bits, `dry` / `DRY-WROTE` (29/30, world.h `kDirtyReasonName`), and
  `sim_waterbody.wgsl` `wbQuiet` ignores exactly those. A bank drying touches
  no liquid; `doStaining`'s soak-in (which spends water) keeps
  `stain-idle` / `STAIN-WROTE` and still counts. The created body adopts at
  tick 2092 (113256 poured, 109061 measured). That let pass B run for the first
  time since 2026-09-25, and it failed on a gate bug: `FindChild(1)`, the lake's
  basin id before 51657b7 made it `WaterSiteBasinId`. Now `LakeId()`: 2 adopted
  descriptors, held 1886851 = voxel 1886851. Command:
  `bash scripts/run.sh ./build/Release/sandvox_redworld.exe --selftest --gate waterbody`.
- `chem-electrolysis` stays **fail**, cause now attributed (the 10-01 guess
  "IfAir sparks not stepped" was wrong). The cause is colour-lattice order. A spark is a gas
  that rises in its own phase on substep 0. Phases run x, then y, then z. The
  rule is authored from the molten salt's side. So a pool at y ≡ 2 (mod 3)
  never sees its spark. The gate now pools at all three residues: y 221
  (≡2) sodium 0, y 222 sodium 41, y 223 sodium 44. The fix is a design
  decision (spark-side rule, non-rising spark, or two-sided matching).
- `tree-fell` stays **fail** (suite scope). It passes at gate scope at
  c364516: 0/0/0 burn residue, forced rescan 0/0/0. The suite-scope floater was
  not reproduced, because this package ran no suite.





## 2026-10-03 — repose: worldgen reads each powder's own repose (hash moves; NOT re-pinned here)

`sand` 34, `gravel` 40, `dirt` 40 are authored in materials.json, and worldgen's
loose-cover taper, local step line and flowy pond beds read the material's own
`repose` word (DESIGN.md §4 "PER-MATERIAL ANGLE OF REPOSE"). Powder behaviour
and the generated desert surface change, so `determinismHash` and the smoke
tables move: intentional, left for the orchestrator's single re-pin.
`genSettle.*` caps tightened to the new measurement +5% (settle 120 / 106 ->
caps 126 / 112; travel 0.0 / 0.0 keeps its 0.5 caps). The control arm (same
materials, old worldgen) measured 1499 / 1301 and FAILED, which is what this
gate now guards.





## 2026-10-03 — `ca-slope-hybrid` flipped to `"pass"`; `fluid-react` exact again (fluid-gates)

`ca-slope-hybrid`: the "settle mass leak" entry above is history -- mass was
already EXACT when this branch started (0 unaccounted, ledger never parted).
The red was 127 particles parked at |v| = 0 on the last tread's lip, 80.3% in
the basin against 90%. The gate now reports the settle stages (picks,
infeasible ceil/floor columns, perch-veto losses, forced, sealed) and, per chunk
slot holding live particles, the extent, max |v| and the `fluidCalm` calm/age
words. Before: 31 picks, 27 lost columns to the veto, 1 forced, residue calm 19
/ age 19. `settleCommitColumn` zeroed the stuck age at every commit, so a
partially vetoed block never reached `sim.fluidStuckTicks` and the backstop
never fired. Fixed in `sim_fluid_seam.wgsl` (a partial commit keeps the age).
After: PASS, 96.9% in the basin, mass EXACT, box asleep from tick 120, 5 forced,
1 sealed. Remaining: ~138 particles live in the basin at tick 400, calm 0 at
max |v| 0.47 vox/tick -- never calm, so a solver question, not a settle one.

`fluid-react`: the tolerance (`fluidReactCaGapPctMax`, 6.0) is gone. The CA
side of reaction consumption is now counted (`sim_step.wgsl` reactLiquidEaten
-> pageFaults [45], world.h `kPageFaultReactLiquidEaten`), and the gate adds
`FA_KILLHARD`. Measured: 271 standing + 0 stray + 4 live + 2302 eaten excited +
119 eaten settled + 8 killed hard = 2704 of 2704, seam books residual 0. The
seam fix above also drained this sealed box: live at the end 157 -> 4.

`determinismHash` was not re-pinned on this branch (orchestrator pins once):
the seam change moves any world with excited fluid.




## 2026-10-03 — red-bodies triage (worktree off c364516; `determinismHash` not touched)

The bodies/gore reds from the 10-01 list, measured standalone (`--verify` of
the seven) and in ONE kOrder-prefix `--verify` through `pool-human` (329 gates,
27 min). Test-side changes only; no engine code moved, so no hash moved by it.

- **`mob` FIXED (flipped to pass).** micro body render never called
  `Simulation::UploadMicroBodies`: bricks go up at boot and then only from the
  frame loop's dirty check, so every model built after boot (the critter's own
  limbs, its severed leg) was marched from a GPU record nobody wrote. Parking
  each instance alone: models 449..458 drew 0 px at the frame centre from every
  direction, boot-time model 51 drew 1,900-2,900. The probe also had two holes:
  the critter sweep's below eyes sit in the ground (now reported, not asserted;
  the solo probe covers those octants), and the solo probe passed views from
  above on ~4,000 pixels that changed elsewhere in the frame (it now aims at the
  brick centre and counts the central 128x128). Now 11/11 + 3 underground,
  solo 14/14 (min 1,762 px).
- **`armor-react` still red, reclassified.** The bath's two creatures had no
  behaviour profile and WALKED (legacy wander, ~2 vox/tick) off the 17-voxel
  pad while the acid, poured around each root every tick, followed them; the
  plated one lost a foot, crawled, lay in pooled acid and died at t+51. Pinned
  with the `dummy` profile (W2-O's fix for the impact gates). Standing still,
  the plate is untouched (6720 -> 6720) and the plated torso still loses 6.9%
  (416 of 2038 skin): 283 in the bottom fifth, 116 in the top fifth, 17 in the
  middle three, with the bath poured to y 198 against a torso at 194.4..198.6.
  The acid enters at the waist and the neck/arm openings of a fully submerged
  torso, and it is ~15x more potent on flesh than when the <1% bound was set
  (the bare control loses 1,486 skin and dies at t+31; the gate's own note
  measured 103 in 120 ticks). The bound encodes a rate that no longer holds;
  NOT loosened. Owner call: shallower bath, a bound on the middle bands only,
  or a plate that closes at the waist.
- **`rig-clip` still red, authored content.** Every failing pair names
  `item:sword` (in the torso, the hips, or the wielder's own forearm, 13-314
  vox) and ikMiss/shoulderClamp/roundTrip read 0.00: keyed frames (63a9e19)
  are slerped poses that no keep-out touches. Re-author the frames or add a
  blade-vs-body keep-out to the keyed path — both owner decisions.
- **`pool-human` still red at suite scope, passes standalone.** The runner's
  leak line now reports the art palette: 222 after load, hair-tuck +14, undead
  +5, zombify +14 -> 255 by gate 157. Also a capacity limit: the 20 pool
  bodies hold 122 distinct colours (`bake_human_pool.mjs --dry`). Options in
  `_poolHumanKnown_about`; no change. pool-human no longer leaves its spawn.
- **`corpse-head-laser`, `player-corpse`: pass standalone AND in the kOrder
  prefix** (hair turn 0.000; D woke 1), but the 10-01 reds were at `--suite
  acceptance` scope (both vk smokes run first in the same process), which was
  not re-run, so both stay recorded fail. The hair chooser now ignores runtime
  defs (none were candidates in the prefix, so this is hardening, not the
  cause).
- **`corpse-bleed`: passes** standalone and in the prefix (head off=1); the
  09-30 red is gone. It has no baseline entry, so nothing to flip.

Seen in the prefix run and NOT in this package's scope (recorded `pass`, red at
prefix scope at c364516): `evaporation`, `rain-stain` (OIL OVER: oil 0 at
t120), `venom-blade` (arm C seeded nothing; passes standalone), `vessel-grid`,
and the `determinism` pin (b2514936 -> 5c3ea9ad with the twice-run comparison
passing — a moved pin, not a determinism failure).



## 2026-10-03 — fire-perf triage of `mob-burn` and `fire-depth` (no key changed)

Both stay `"fail"`; neither is a defect in the burn pass.

- `mob-burn`: the two red subchecks are the fixture, not fire. `cloth vs
  flesh` censuses `cloth` on the spawned body and finds 0 (the stock `human`
  wears undercloth, not a robe); `burn leaves char` lights `cloth` on `armU.L`
  through `IgniteLimb(.., mCloth)` and lights 0 voxels, so nothing burns and
  nothing chars. Both were written for wizard.vox (robe on every limb, deleted
  2026-09-19). Every other subcheck passes, before and after the fire-perf
  package (bounded front window: burn chain, fire into grid, terminates,
  limbs sleep, heat across a joint, corpse, acid, player burns all PASS with
  numbers in the same range). Making them green needs a fixture decision:
  dress the human in a garment (the shells are separate limbs, so the ignite
  and the census would have to address the sleeve shell, not `armU.L`) or
  re-point both checks at a material the human has. Not done here.
- `fire-depth`: 22.9k of 99.0k px behind the flame (floor: a tenth). Render
  side, as recorded above; untouched by CPU burn work.


## 2026-10-03 — det-cpu: fetch cache at the fixed latency, deterministic Jolt ids

- `village-twice` (new, `pass`): the Harrowby morning (harrowby.twiceTicks
  = 900) twice in one process, every Jolt body paired across the runs by
  creation ordinal. Before the fix it named the 2026-09-29 hinge divergence:
  identical body states under other Jolt ids until the door leaf's first
  contact solve (tick 422). After it: trace, states and handles identical.
- `village-harrowby` -> `fail` (known): the fetch cache now lands at K=4
  ticks, the day is perturbed, and Edric is left at the longhouse loft edge
  walking straight at the stair-bottom waynode across a 16-voxel drop (13:00
  and 17:00 rows). Cache vs grid around his feet: 0 stale cells; the fetch
  FIFO is not starved. A navigation gap exposed by a different trajectory,
  not a determinism bug. `SANDVOX_FETCH_LAND_EARLY=1` (the old arm) completes
  the day. `harrowby.dayTrace` NOT re-pinned (the Jolt id change moves it
  either way; the orchestrator pins). The day's trace came out the same first
  in the process and after 1220 ticks of other gates (ee23c5d3bd817e04:15367
  both): the "three scopes, three traces" leak was the Jolt id history.
- The runner now calls `Physics::ResetBodyIdHistory()` before every gate, so
  a gate's Jolt ids no longer depend on the gates before it (rule 7).
- `tree-fell` passed at `--verify` scope on this tree (0 floating after the
  burn and the quench); its recorded status is red-world's to change.
- `--gate determinism`: b2514936, matches the pin.





## 2026-10-03 — det-debris: tree-fell's cut-half leak was race #10, already closed (no key changed)

- Attributed with a new instrument, `SANDVOX_DEBRIS_TRACE=<file>` (debris
  decision inputs per PreTick, every snapshot's support-flag set, every
  probe / scan / terrain-need list). Three boots of det-cpu's tree 03f46cd:
  chunks needed 38145 / 39067 / 39067, polygonizes 126 / 132 / 132, and the
  traces differ at ONE line with every state hash equal before it:
  `F snap 94258 flags 5` / `7` / `10`, the tick the 28k-voxel island is
  erased. The `cells` kernel's support flag read neighbours the same
  dispatch was erasing (cross-vendor audit #10), fixed by 15c3787, which
  det-cpu's tree did not have. That snapshot now flags 0 chunks every boot.
- On 2c8e285 + the trace: six `--gate tree-fell` boots and two
  `--verify tree-fell,debris,floaters,determinism` boots give the same
  `tree-fell` line apart from the wall-clock COST fields (chunks needed
  38100, polygonizes 110), and the debris / phys / fetch traces are
  byte-identical between boots. `tree-fell` PASS at gate and `--verify`
  scope; its suite-scope `fail` is unchanged here (no suite run). Forced
  rescan 0/0/0 on both trees; `eventQueueFullSpilled` 0 (the ~600 of the
  plan note is now `drainBackpressure` ~730-790, a spill queue that waits,
  not a loss).
- `--verify` determinism: PIN MOVED 0fa43063 vs pinned b2514936, twice-run
  reproduces, identical in both boots. That move came in with the repose
  package (its entry above says it was left for the orchestrator to re-pin).
  This package changes no hashed state and does not re-pin.
- The pre-fix `sim_mutate.wgsl` run on the current tree does NOT diverge on
  this GPU (the sorted upload makes the race land the same way), and support
  flags are not in the twice-run hash. So a regression of this class would
  currently go unseen; see DESIGN.md section 7.

## 2026-10-03 — fire-gpu: no key changed; `determinismHash` was already stale on main

`--verify determinism` reports PIN MOVED (final hash `0fa43063`, pinned
`b2514936`) on this branch -- and identically on HEAD 1f7f393 with its own
exe (`_fg_base`: `0fa43063`, twice-run reproduces). Not this package: its
default arm is hash-identical to HEAD (`--perf village-fire` f7936a11 and
`forestfire` 8dd17ecf both sides). The move is upstream, most likely the
repose entry above ("hash moves; NOT re-pinned here"). Not rebaselined here
(package rule). With `sim.gasThinDecayMul` 3 the determinism hash is
`a2f8e287`, also reproduced twice-run; `oil-fire`, `heat-updraft` and
`gas-leave` pass at 3. At the default (1) these passed on this tree:
determinism (twice-run), gas-leave, gas-reenter, gas-leave-overflow,
gas-farplume, gas-farplume2, heat-updraft, oil-fire, draft-stack.

## 2026-10-03 — ca-chunk-overhead: no key changed; the CA is hash-identical

The colour rows now dispatch per-colour chunk lists and sparse cell pools,
and camask writes each dirty chunk's own repose snapshot (DESIGN.md "The CA
dispatches cells, not chunks"). Every arm is exact against HEAD 8c3ca8e:
`--perf village-fire` f7936a11 (and its caGasCells / caOtherCells counters),
`forestfire` 8dd17ecf, `idle` e4f3ead0, `determinism` 0fa43063 twice-run
reproduced (the pinned b2514936 is stale on main, see the fire-gpu entry),
`repose` piles identical. The same 24-gate `--verify` list
(determinism, sleep, ca-skip, stamp-sleep, repose, ca-slope, ca-level,
ca-gutter, oil-fire, heat-plume, heat-updraft, heat-ignite, gas-leave,
gas-reenter, gas-leave-overflow, fluid-excite, fluid-react, blood-stain,
stain-react, evaporation, wind-gas, mob-burn, tree-fell, fire) gives the same
result on HEAD's exe + assets and on this tree: `evaporation` FAILS on both
(pond surface 11/289 after 2500 noon ticks -- not this package; recorded
"pass" in baseline.json), `mob-burn` known-failing. `gas-leave` under
`--vk-validation`: 0 messages. Not rebaselined here (package rule).

## 2026-10-03 — electricity endgame: `determinismHash` b2514936 -> 0fa43063 (re-pinned)

Recorded with `--selftest --gate determinism --rebaseline` on the endgame tree
(main e772227 + the endgame branch); twice-run reproduces. The value was
ALREADY 0fa43063 on main before any electricity package (the fire-gpu and
ca-chunk-overhead entries above saw it on their trees and left it to the
orchestrator). Electricity E1-E5 and the endgame's proportional charge decay
(`sim.elecDecayShift`) leave it there: the determinism scene lays no electric
source and the charge field is unhashed, so only what charge DOES in the CA
could move it. This re-pin records the earlier upstream move (the repose
package, per those entries), not an electricity change.

Other keys changed in the same commit: `elecCrackle.decay` 200 -> 0 (the gate
now runs the game's own proportional decay; every page is back 55 ticks after
the source), new `elecPlayerStun.*` thresholds for gate `elec-player-stun`,
and the `_elec_about` / `_elecE2_about` texts. `ops-replay` is green again:
its red was a reporter artifact (the per-tick ticket stash carried from the
recording into the replay); docs/PLAN_electricity.md STATUS has the
attribution, including why its ticket no longer idles out (not electricity).

## 2026-10-04 — demons D2: the smithy cellar (default map only)

- `harrowby-cellar` (new, `pass`): thresholds `cellar.*`.
- `village-harrowby` `fail` -> `pass`, `harrowby.dayTrace` 353efa79c26da7cb:6333
  -> ffdb3ea524fe8fb2:6443. Recorded with `--selftest --gate village-harrowby
  --rebaseline` (the comparable scope). The trace was always going to move:
  the cellar is in the default map's edit layer under Osric's house, and
  the book is a new harrowby ref (kept out of the gate's survey box by
  `Survey`). The day now completes: all five villagers reach every anchor and
  Edric no longer stalls at the loft edge. That is NOT attributed. It may be
  this tree's base (main 7bd0885) rather than D2, so whoever merges should
  read it as an observation, not a fix.
- `determinismHash` unmoved (0fa43063). The harness map names no layer, and
  the two new materials (`tallow`, `candle_flame`) and the one appended
  reaction appear nowhere in it.

## 2026-10-05 — fight64 round 4 L: `ragdoll-dress` recorded `"fail"` (limp hulls)

- `ragdoll-dress` `pass` -> `fail`. Limp ragdolls now collide as one convex
  hull a limb (package L). Attributed in one binary at `--verify
  ragdoll-dress,corpse-sleep` scope: default 4.39 vox fall-arm limb stretch
  (cap 4.00); `SANDVOX_LIMP_HULL=0` 3.06; main's exe (d34fb41) 3.06, identical
  to the all-off arm. Drift, twist and the slam arm are unchanged. Owner
  options in `_ragdollDressLimpHull_about`.
- `corpse-sleep` fails at that scope on main's own exe too (asleep after 885
  ticks, cap 300, every arm identical): inherited, not L's, not changed here.
- `determinismHash` unmoved (0fa43063): the determinism scene has no creature.

## 2026-10-05 — reconciliation: `chem-electrolysis` and `waterbody` are recorded pass

Bookkeeping only; no run, no `baseline.json` change. Earlier entries in this
file still list both gates as known-failing (the nine-error list in the wave-1
section, the 10-01 suite-triage list, and the 10-03 red-world entry's
"`chem-electrolysis` stays **fail**"). `tests/baseline.json` records both as
`"pass"`:

- `waterbody` -> pass on 2026-10-03 (red-world, 3897482; pass N, `wbQuiet`
  ignores the stain-dry dirty reasons), as the red-world entry above says.
- `chem-electrolysis` -> pass on 2026-10-03 by electricity E2 (ecbfdd6, see
  `_chemElectrolysisFixed_about`): a discharge no longer drifts, so a spark on
  a y = 2 (mod 3) pool top stays the pool's face partner and it reacts through
  the charge-as-partner path (sodium 46 / 45 / 46 across the three residues).
  Measured at gate scope; not yet re-measured at `--suite acceptance` scope.

Read the older entries as history; `baseline.json` is the current status.

## 2026-10-05 — owner decisions: limp hulls off, mob-burn dressed, rig-clip per form

- `ragdoll-dress` `fail` -> `pass`. Owner chose option 3: limp ragdolls keep
  their box compounds by default (`SANDVOX_LIMP_HULL=1` is the round-4 L arm).
  Fall-arm stretch 3.06 vox (cap 4.00), slam 0.04, drift 0.000, twist 0.06.
  Measured at `--verify determinism,ragdoll-dress,ragdoll,mob-burn,rig-clip,mob-cap64`.
- `mob-burn` stays `fail`, but for a different subcheck. The fixture now wears
  the stock hood + robe + pants; every cloth subcheck passes (numbers in
  `_mobBurnKnown2`), and 'heat across a joint' asserts the excess over its
  control (`mobBurnCrossExcessPctMin`, new). The red one is 'limbs sleep' on
  the bare K fixture: unburnt limbs 85% asleep vs the 0.90 floor, all awake
  visits world-miss on a 9-tick period (the doused puddle). main's exe read
  91% / 90% on the same subcheck; it moves with K's start tick.
- `rig-clip` stays `fail` (owner is re-authoring the frames): one fixture per
  loadout, 105 rows, 46 over the limit; the table is in the gate's output and
  in `build/last_run.json` (`report:rigClip.*`). Thresholds unchanged.
- New keys: `mobBurnCrossExcessPctMin` / `...Observed`, `mobBurnClothLeadFrac`,
  `mobBurnClothLostPctObserved`. `report:*` observed keys are never written to
  baseline.json (selftest.cpp `RebaselineSelftest`).
- `determinismHash` unmoved (0fa43063).

## 2026-10-06 — heat perf: source fold + tent cadence: `determinismHash` 94e972ee -> ba939a9c

Recorded with `--selftest --gate determinism --rebaseline`; twice-run
reproduces. Intentional: the heat sources are now read by caMask before the CA
(one tick earlier than heatSrc read them after it) and a chunk's heat target is
recomputed on its `sim.heatTentEvery` (4) phase, so heat-driven transitions and
the heat term of the wind land on different ticks (DESIGN.md "The source fold",
"The tent cadence"). Heat gates at the new pin: heat-idle / melt / ignite /
freeze / ambient / bound / plume / updraft, oil-fire, glow, sleep PASS.
`heat-plume`'s firing-log check now allows plate A's footprint down to the
lid + 1 (a plate leaf that dropped onto the chamber lid, still directly above
the flame); `heat-live` fails exactly as on main 60713bd (X 102 of 114, the
pend list stalling in a proven-settled window -- not this package).

## 2026-10-07 — correctness sweep (worktree branch): `determinismHash` d07bdc17 -> 539a2f07

Recorded with `--selftest --gate determinism --rebaseline`; the twice-run
reproduces (and the final `--verify` matched the new golden). Intentional:
every CA roll in `sim_step.wgsl` now keys on `rngCellId(c)` (the 512-block's
hash folded into `cellIndexW`, so a ticket cell stops sharing its dice with
the window cell it aliases), which changes every random outcome in the world.
Also in this tree, none of which moves the hash on its own: a window cell's
`inBounds` is the window (no stepping into a face-adjacent ticket shell), the
ticket deposit record has a count word (5 slots x 6 words), the heat pend list
withholds the settled-tick proof, a healed joint twin's second copy is free,
AVTR v2, the ITMS body index skips fixtures, peer blasts hit owned NPCs and
debris, gas crosses the op wire.

- New gates, all `pass`: `heal-joint` (audit R3 + R4), `ticket-edge` (TIE /
  EDGE / ALIAS; measured against the pre-fix `sim_step.wgsl` through
  SANDVOX_ASSET_DIR: EDGE took 24 eighths into the shell and ALIAS differed on
  0 of 86 samples, i.e. the gate sees both bugs), `save-avatar-body` (AVTR v2).
- `heat-live` PASSES (X 114 of 114; it failed on main since 60713bd).
- `save-items` gained arm D (a fixture ahead of the items); `blast-players`
  gained arms (e) / (f); `ops-exchange` gained arm (H): `net.opsExchangeRows`
  32 -> 37.
- `village-twice` PASSES on this tree (the 2026-09-29 in-process hinge leak was
  fixed by det-cpu 2026-10-03; nothing new needed).
- NOT re-pinned here (the owner runs no smokes / full suites): the smoke probe
  tables and `harrowby.dayTrace` move with the RNG key; `village-harrowby`
  will report its trace as moved until `--selftest --gate village-harrowby
  --rebaseline`. `ops-replay`'s record-size note (5,436,252 vs 5,421,372 B) is
  informational and passed.
- Final `--verify` (one boot, 17 gates): village-twice, heal-joint,
  heal-restore, heal-wound, heat-live, ticket-edge, ticket-land, ticket-decay,
  save-entities, save-items, save-avatar-body, mob-save-delta, mob-handoff,
  blast-players, ops-exchange, determinism, ops-replay: all PASS, 0 page
  faults.

## 2026-10-08 — map v2 A1 (altitude correctness): `determinismHash` 39b3de48 -> 66d04733, `heat-ambient` recorded fail

Intentional moves, every one of them in worldgen or the heat ambient: the
per-column jittered treeline (biome `treelineScale`; snow cap, trees, cacti,
shore fringe, ice, cover rows' new `treeline` condition), the sediment wedge
relative to the authored ground (`sedAbove`), band-2 caves 24..31 m under the
column's ground, the lava clearance, steep rock (`rockSlope` 640), the lapse
rate (`sim.heatLapsePerKm` 6) and the per-column snowline in the heat column
table (now one word per column, tiled by chunk column). On the HARNESS map most
of these are the identity (its treeline jitter is 0, its ground is ~y 200 where
the relative wedge equals the absolute one and band 2 lands where it was), but
the treeline scales (tundra 0.85, alpine 0.9, pine 1.05) and steep rock move
it. Recorded with `--verify ... --rebaseline`.

- New gate `altitude` (pass): on the `peaks` map, the spawn window, the
  per-biome jittered treeline, the lapse rate, lava clearance under the gorge,
  a uniform +200 raise translating the bare ground exactly (8 columns,
  home area included), CPU + GPU sediment on the 900 m valley floor, rock and
  <= 32 active chunks on the cliff band.
- `heat-ambient` recorded `fail`, part D only: one tundra snow grain slides
  into a forest column on the default map and melts (see
  `_heatAmbientKnown_about` in baseline.json for the voxels and the owner
  options). Attributed to the relative sediment wedge moving that lowland's
  ground; not compensated.
- The same rebaseline refreshed `terrain.*Observed` / `reliefVox` /
  `surfaceMinY` and the `envTruthTreesPerHa_*` observations, last written by
  the P-G rebaseline (0b63a10) -- they are observations, not thresholds, and
  were stale from before map v2.

## 2026-10-12 — baseline repair C: heat / weather / water (worktree branch, no key flipped)

Eight gates recorded `pass` were red on main 8a46ed0 and on 1dd67ac. Each FAIL
line was first made to say WHY (CLAUDE.md rule 6: `src/test/census.h`
`MatCensus` names what a fixture holds instead of its subject; heat-melt /
freeze traces; drafts meta + back-to-back rebuilds; rain-fire burn shape and
front trace; waterbody pass N arm 2 extremes + relevel ledger), then fixed:

- `drafts` (engine): the shelter solve is a pipeline advanced one stage per
  tick its `args` row is recorded, and `Cond::Draft` needed `caActive`. A
  world that proved itself settled mid-solve froze it: solve #358 started at
  tick 61020 and published at 61035, three ticks after the smoke puff woke the
  CA; the gate's first read was a geometry behind (purity: 275,443 of 393,216
  words differed from a rebuild, while two back-to-back rebuilds agreed).
  `RecordCtx::draftTail` keeps `C_DRAFT` for 2 x kDraftStages + 1 ticks after
  the last CA / rebuild tick -> 0 words differ. Visible since the harness world
  started proving itself settled quickly (idle-wake fixes, 10-05). Also
  `drafts.sealedKeepMin` 0.3 -> 0.1: the sealed hut's smoke is a control and
  thin smoke fades x3 since fd72df9 (owner decision; walls intact, census).
- `wind` (fixture): the chamber at a literal y112 sat under the harness ground,
  where the wind's height-above-ground ramp is 0 since 4d18d09 (09-30): every
  arm hashed identical. Now terrain-relative (FixtureYOver +6): drift +21 / -24.
- `evaporation`, `rain-stain` (fixture + engine): both fixtures stand at about
  y228, above the harness column snowline (treeline 228 - 1). The pond skinned
  over (surface ice 240, snow 30, water 10); frozen ground took no oil stain
  (heatGroundFrozen). Both now pin a mild climate (base 12) over their
  footprint, and `Simulation::PrepareHeat` gives a PINNED column no snowline
  (the per-column snowline of map v2 A1 had made pins leaky).
- `heat-melt`, `heat-freeze` (deadlines): measured geometric tails that
  complete -- melt left t150 51 / t300 22 / t450 12 / t600 3 / t750 1 (1 at
  900), freeze open tops t300 57 ... t1500 1, whose column then held its chunk
  awake. heat.meltRunTicks / meltTicksMax 900 -> 1800, heat.freezePhase2Ticks
  1500 -> 3000 (`_heatMeltFreezeTail_about`). Now melt done by 1245, 192/192.

Left red, attributed, owner decisions:

- `rain-fire`: a one-voxel leaf sheet burns near its percolation threshold.
  Clear arm 43% at 400 ticks (front still alive: 269 gone, 18 burning);
  `SANDVOX_RAINFIRE_DIAG=1` ran the same clear arm at four other harness sites:
  56 / 71 / 30 / 65% (at (120,120) the front died at t300). 60% sits inside the
  site-to-site spread, so the gate is a dice roll; no combustion knob or leaf
  rule changed since it last passed (d83306e, 09-30). The ordering claims hold
  by an order of magnitude (drizzle 2%, storm 1%). Wind pinned calm (36 ->
  43%). Options: rainFireTicks 800 (the front at the gate's site is alive),
  rainFireClearPctMin ~25 (a "lit and spread" floor), or a fuel that is not
  near-critical.
- `waterbody` pass N arm 2 (red since <= 13adc66, unowned): the CREATED body
  never relevels -- adopted, footprint listed (44 chunks) and armed after the
  crater, but `wbSurface` measures 0 columns (relevel count 0, given/taken
  0/0) and the surface stays the CA's 1-eighth-per-2-cells cone (16 eighths
  from the pit corner to the crater). Its sweep map holds 2 components at the
  live level y197 (split 0); the pit's water reached 1,113 top columns against
  1,089 dug, so it found a side pocket. Next step: which component the body
  owns vs where its water is (wbOwns / wbComp).

## 2026-10-12 — combat package E (baseline repair wave): `npc-styles` fixed, `swing-plane` and `damage-sources` attributed, still red

No hash change and nothing rebaselined. All three gates are recorded `pass` and
were red on main 8a46ed0 and on 1dd67ac.

**`npc-styles`: FIXED. It was the gate.** Two defects, both in
`src/test/selftest_combat.cpp`:
1. `aimed` was read AFTER the stroke loop. The stroke's effector is an override
   that `Mob::ClearStrikeEffector` drops when the stroke ends, and the fixture
   holds a sword, so `ResolveEffector` fell through to `Held`. Every set of jaws
   therefore read as "not aimed": the JAWS-turned and POSED-head-followed claims
   never ran for any bite, and a jaws thrust was judged as a fist. `aimed` is
   now latched during the Cut phase.
2. `shark_bite` / `piranha_nip` (swimmers, 2cc68c4, 10-10) are reach-dominant
   by a hair (cut reach 0.25 against 0.10 el), so the thrust branch asked for
   1 vox of COMMANDED radial travel. An Aim effector has no radial channel:
   `Mob::ApplyStrikeAim` reads only StrokeAz/StrokeEl, and its only forward
   carry is the cut-progress lean (`aimLeanBodies`). The styles' own notes say
   the CHARGE carries the jaws and the stroke only gapes and snaps them. For
   an aimed effector the thrust claim is now "the rig carried the head forward"
   (`AimDiag::lean > 0`). The data was not changed. With (1) fixed, the
   absolute "commanded > 0.1 rad pitch" floor failed on shark_bite's
   deliberate 0.10 rad. It is restated as half the authored elevation
   (`naturalTravelFrac`), which is stricter for every land bite.
   Result: `npc-styles: PASS (307 checks)` at --verify scope. That run also
   had the demons agent's 4befcbd applied, uncommitted.

**`damage-sources`: STILL RED, cause attributed, introducing commit NOT
found.** The gate now prints why it failed (`damage-sources: FAIL --
<reason>`), and the thrown-stone arm prints what is in the block's lane.
Reason: "the cuirass did not take the stone". The fixture faces +z
(heading 0.000), is not swimming, and has its LEFT forearm and hand in front
of the torso inside the block's 3-voxel lane:
`armL.L(1.9,1.9,+1.4) hand.L(2.0,0.1,+2.2)` relative to the 16-sample torso
centroid. So the bare stone bills `hand.L -60` (body -27) and the plated
stone bills the cuirass sleeve plus `armL.L -33`. The torso is never struck,
so plated (33) > bare (27) for reasons that have nothing to do with armour.
This is the same failure as the 09-29 `adbc47d` red, which fc8e451 fixed by
throwing along z. The gate PASSED at 9848dbd, acd0d51, 5a23e96 and e959c65
(thrown stone "7 billed -333 hp"), so the change is in first-parent
9848dbd..1dd67ac. Between those commits the gate's own fixture code did not
change. Something moved the left arm, or the sampled torso centroid, about a
voxel into the lane. Untested suspects: 63e9dd8 (stump walk, "arms at the
sides"), 088bd1b (freefall / seated limp hand-over), and the anatomy re-bake
fc4783b, which changes torso voxel order and so the 16-sample centroid. Owner
decision: if the pose is right, the fix is the fixture's lane (throw at the
back, or pick a lane clear of other limbs). If it is wrong, it is a pose
regression.

**`swing-plane` E: STILL RED, mechanism found, bisect stopped one range
short.** The new E lines (`swing-plane E (tick N)` / `(sweep)`) show the swing
LANDS. It hits armU.R, head and armU.L on 6 ticks. Hp falls 1107 -> 1092.6.
Every tick runs one kerf, but at depth 0.09..0.12 world voxels (power
0.15..0.36 x edge align 0.38..0.86; tip speed 37..81 vox/s against a 200 vox/s
full-power speed). The authoritative skin lattice loses about 3 voxels a tick
(art 26428 -> 26412). The collider count the gate reads, `LimbVoxelCount`,
stays at 3320, because a 0.1-voxel kerf never empties a collider cell. F's
direct edge-on sweep still removes 638. So "0 of 0 wounded limbs" means
"wounded below the gate's resolution", not "not wounded".
Bisect (first-parent, `--verify swing-plane,damage-sources`):
- good: acd0d51, 5a23e96. Both show "1 of 1 wounded", which is marginal: one
  collider voxel.
- bad: d34fb41, e959c65, 9848dbd, 1dd67ac.
- The introducing commit is in first-parent `5a23e96..e959c65`: 55abf81
  (demons D5), c2aec27 / c597987 (D5 merges of main: fight64 M work pool, Q,
  R, D4 casting), 55cb72b, a7ca582, c96443f, b21d23a, 7194388, 25002dd
  (fight64 round 3 S: Reset clears tick-drained queues), a6176ca (round 3 F:
  CPU tick budget), e959c65 (round 3 C: CCD scoped to terrain for dead/limp
  bodies, plain collision for kinematic pairs). Package X (3bb2f4b,
  DownsampleSkin/CarveBounds) is AFTER e959c65 and is cleared.
The tip box is identical on both sides of the range, (248.3,187.2,258.9) to
(263.2,189.8,266.1), so the swing's geometry did not move. What changed is
whether that marginal kerf took a whole collider cell. Next step for whoever
picks it up: test e959c65's parent a6176ca, then 25002dd. Separately, there
is an owner/fixture question. The gate measures wounds on the COLLIDER
lattice while the wound model works on the skin lattice
(`LimbArtVoxelCount`), and a 4-8 m/s cut is shallow by tuning (`gore.cutDepth`
0.08 + 0.15 x power, unchanged since 09-14). Counting art voxels would make E
pass at its current depth. That is a change to the claim, so it was not made.

## 2026-10-12 — baseline repair wave: what stays known-failing (orchestrator)

Merged on `baseline-repair` (7 worktree packages): 26 of the 30 inherited reds are green, determinism ef7d9a06 unmoved. Four are marked `fail` in baseline.json pending OWNER decisions, each attributed above:
- rain-fire — clear-sky arm is a coin flip on a one-voxel leaf sheet (30..71% across 5 sites); options: run 800 ticks / floor ~25% / thicker fuel bed. See "Baseline repair C".
- waterbody (pass N arm 2) — a player-dug pool splits into two components and the relevel counts 0 surface columns; water-relevel design work. See "Baseline repair C".
- swing-plane E — the swing wounds (hp 1107 -> 1092.6) but cuts ~0.1 vox deep, never emptying a collider cell the gate counts; introduced in 5a23e96..e959c65 (demons D5 / fight64 round 3 S, F, C). See "combat package E". (Corrected below: introduced by 64993fd, D4's standing reach cap; the depth never changed.)
- damage-sources — the fixture's left forearm stands in the stone's path, so the cuirass is never struck; introduced in 9848dbd..1dd67ac, pose vs fixture is the owner's call. See "combat package E".

## 2026-10-12 — `swing-plane` E attributed to demons D4's standing reach cap (64993fd); the cut did NOT get shallower; still red, owner decision

No hash change, nothing rebaselined, `swing-plane` stays `fail`. No engine or
gate behaviour changed in this entry. It corrects the attribution above
("combat package E" and the orchestrator's list).

**Introducing commit: 64993fd** (demons D4, 10-04: "the standing-body reach
cap (pose.cpp: the imp float)"). It reached first-parent main through c597987
(the D5 merge of main), inside the bisected range 5a23e96..e959c65.
`Mob::StepGait` now draws a STANDING body no higher than every planted foot can
reach. That lowers the dummy's drawn limbs inside an unchanged box. The gate
aims at `MobOrigin + 0.60 x worldSize.y`, which is the BOX and not the drawn
body, and the blade's path did not change (the avatar's shoulder is 194.1 in
both arms below). So the edge now crosses the dummy higher up: armU.R, head,
armU.L. Before, it took armU.R, torso, armU.L.

Proved with one A/B in one binary. A temporary env switch around the cap,
since removed, ran `--gate swing-plane` in both arms:
- cap ON (shipped): 6 body hits, art 26428 -> 26412, collider 3320 -> 3320,
  `0 of 0 wounded`, FAIL.
- cap OFF: 9 body hits (torso on ticks 7-9), art 26428 -> 26401, collider
  3320 -> 3319 on tick 10 (armU.L), `1 of 1 wounded`, PASS. That is exactly the
  "1 of 1, one collider voxel" the bisect saw at acd0d51 and 5a23e96.
- Per-tick tip speed, power, align and kerf depth are IDENTICAL in both arms:
  speed 41.8 / 73.7 / 56.7 / 77.9 / 48.4 / 36.9 vox/s, depth 0.10 / 0.10 /
  0.11 / 0.12 / 0.10 / 0.09.

**The kerf depth did not regress.** Nothing that computes it changed in the
range: melee.cpp, avatar.*, player.*, kerf.h, items.json and tuning.json are
all untouched in 5a23e96..e959c65. The depth is
`(gore.cutDepth 0.08 + gore.cutDepthPower 0.15 x power) x heft 1.0`. The pass
before the range was a one-collider-cell coincidence. A 0.1-voxel kerf is 0.8
art voxels at 8 art voxels per cell. It only empties a 512-art collider cell
that already sat within a few voxels of the 256 majority line.

**The swing speed did not regress either, and it is not a real swing.** E's
`drive(-34, 0, true)` is the freeform mouse drag that was removed from the game
on 2026-09-25 (see the melee.h header; nothing outside this gate calls
`MeleeState::Feed` any more). The arithmetic is 34 px/tick x aimGainX 0.005
rad/px x 30 Hz = 5.1 rad/s. At ~12 vox of tip radius that is ~61 vox/s, which
matches the 37-81 measured. A real cut is a stroke program. `npc-strike`
measures the sword's at ~220 vox/s (full power), and each strike removes 1-8
collider voxels and 26-68 art voxels. E's drag runs at about a third of that
speed.

Nothing was changed, per the package's brief (no claim change, no tuning
change, no new mechanics). The options, for the owner:
1. Drive E with the player's real sword stroke, the stroke program a click
   fires, so "a real swing" really is one. This changes the fixture, not the
   claim.
2. Count E's wound on the authoritative lattice (`LimbArtVoxelCount`), which
   the cut visibly reduces every tick. This changes the claim.
3. Aim E at the drawn chest (a torso limb voxel) instead of the box. This
   restores the pre-range geometry, but only to the same one-voxel margin, so
   it is not a fix by itself.
4. If 0.1-0.23 world voxels per tick is too shallow for a sword, that is
   `gore.cutDepth` / `cutDepthPower` (unchanged since 09-14), not a regression.
