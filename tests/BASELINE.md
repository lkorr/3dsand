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
