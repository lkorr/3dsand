# Stock-HEAD baselines, taken before the terrain overhaul

Recorded 2026-08-26 on `main` @ the commit that adds the `terrain` gate, RTX 3060 Ti,
quiet machine (`tasklist` confirmed no other `sandvox.exe`), every run under
`scripts/run.sh` so nothing else had the GPU.

**Why these exist.** `docs/RESEARCH_worldgen.md` §3.3 argues that a *proportional* scale
pass — one that raises amplitude and wavelength together, so slopes are unchanged — adds
roughly **zero** resident pages, because the extra volume is all `JITTER(stone)` sentinel
interior and residency is paid for by surface AREA, not by depth. That is a falsifiable
claim and it is the single biggest risk in the overhaul: `kPoolPages` exhaustion is a
fatal abort, not a degradation. Taking the before-side costs two commands. Skipping it
means discovering the answer as a crash.

Per CLAUDE.md's verification budget: **this is the cached control arm.** Do not re-run
the stock-HEAD side; diff against this file.

## The commands

```bash
bash scripts/run.sh ./build/Release/sandvox.exe --frames 1200 --autofly-hard
bash scripts/run.sh ./build/Release/sandvox.exe --frames 400
```

`--autofly-hard` is forward + sprint + a strafe cycle + `down` held permanently, on a
fixed tick schedule — a diagonal descent into solid bulk, where almost every chunk needs
a real page rather than a sentinel. It is the only residency instrument that reports
honestly; a standing player and the sky-heavy selftest window both under-report ~2x.

## `--frames 1200 --autofly-hard` — the residency arm

| | value |
|---|---|
| **page pool high water** | **13,631 / 32,768 (41.6%, 213.0 MiB)** |
| pages in use at exit | 1,024 |
| window shifts | 652 |
| whole-frame ms | p50 22.4, p95 38.0, p99 58.8, max 65.6 |
| frames > 33 ms | 107 (9.4%) |
| frames > 100 ms | 0 |
| active chunks | 0 throughout (underground bulk does not tick) |
| avg render+present | 2.24 ms (descending into rock: nothing to draw) |

**13,631 is the number the overhaul is judged against.** CLAUDE.md quotes 14,697 for the
JITTER-sentinel descent, which is the same measurement to within the noise of a different
tick schedule — so this run is consistent with the recorded history rather than a fresh
regime.

## `--frames 400` — the standing-at-spawn frame-cost arm

| | value |
|---|---|
| whole-frame ms | p50 41.6, p95 134.4, p99 140.0, max 160.8 |
| frames > 33 ms | 262 (77.1%) |
| frames > 100 ms | 36 |
| avg render+present | 23.80 ms |
| page pool high water | 12,177 / 32,768 (37.2%) |
| window shifts | 25 |

Note this is a **standing** player at spawn with a full surface scene in view, which is
why it is 2x the 20.3 ms the wind-perf post-mortem recorded — that arm was measured under
a different vantage. The absolute value is not the point; the point is that the
comparison after the scale pass uses the same command.

## What the numbers are for

| Stage | Expected effect | Fails if |
|---|---|---|
| C · scale pass | ~0 change to high water (§3.3's claim: interiors are sentinels) | high water climbs materially → the octave ladder's slope is too steep and is buying surface area, not relief. Lower it before going further. |
| F · 3D caves | **the real exposure.** A single carved cell anywhere in a 16³ chunk destroys its `JITTER` sentinel and costs a real page | high water approaches the pool → the cave province mask is not selective enough. This is why the mask exists. |
| C, E | `--frames 400` p50 rises with cascade fill cost (levels 6–8 finally have something to draw) | no headless gate can see this; §7.1's far per-column hoist is the lever if it does. |

---

## After package B (foundations), 2026-08-26

Same machine, same commands, same `scripts/run.sh` discipline. Package B is
prerequisite plumbing — new noise primitives, three column hoists, a height
contract, perched tarns — so it is not supposed to move the *world* much. It
moves the *cost* a great deal, which is the point of the hoists.

### `--frames 400`, standing at spawn

| | stock HEAD | after B | |
|---|---:|---:|---|
| whole-frame p50 | 41.6 ms | **19.5 ms** | −53% |
| whole-frame p95 | 134.4 ms | **21.9 ms** | −84% |
| whole-frame p99 | 140.0 ms | **23.1 ms** | −83% |
| frames > 33 ms | 262 (77.1%) | **1 (0.3%)** | |
| avg render+present | 23.80 ms | 17.77 ms | |
| page pool high water | 12,177 (37.2%) | 13,399 (40.9%) | +10% |
| window shifts | 25 | 26 | |

p50 is now within 3 ms of the 16.7 ms vsync floor, i.e. standing at spawn is no
longer GPU-bound. The three hoists are where it comes from: `caveAt` out of the
per-cell path (a buried column was evaluating the identical six-sample band
geometry 16 times), `treeAt`'s 25-tile scan reduced to a per-column candidate
set, and the `far` cascade sieve made column-major — 256 columns per level chunk
instead of 4,096, twice over, since `farSurfaceMat` was rebuilding the column a
second time for the skin lookup.

**The +10% on high water is worth watching, not acting on.** It is the tarn
radii growing (48..79 from a first cut at 24..47, forced up by the angle of
repose — see below) plus `World::TerrainHeight` finally including the pond bowl
carve. It is not the octave ladder, which has not landed yet; package C's
prediction of ~0 change is still untested and is measured against **13,399**
now, not against 12,177.

### `--frames 1200 --autofly-hard`

**Not re-run.** Package B does not change residency policy, the sentinel set, or
the fill path — only how many times per column the same answers are computed.
The `--frames 400` arm above already shows the residency direction. The
adversarial arm is what package C and package F must be judged against, and
13,631 is still the number to beat.

### Two rule-2 traps this package turned up

Both were found by `ca-skip` — "the world never reaches a quiet tick" — sixty
lines of output away from anything that mentions terrain, which is exactly why
the `terrain` gate's pass D now names the material keeping a chunk awake.

1. **A pond bowl is steepest at its rim.** The parabolic bowl falls
   `2*(pondDepth - pondDepthRim)/r` voxels per column there, and the bed it lays
   is SAND. Shrinking the radii to 24 (as the plan asked) against an unchanged
   26-voxel depth put that at 1.9 voxels/column against an angle of repose of
   exactly 1.0 — a permanent avalanche. `LoadTuning` now bounds `pondDepth`
   against `pondRadiusMin`, and a swimmable tarn therefore needs `r >= 46`.
2. **Snow is a powder and the authored rims never suppressed it.** Trees, shore
   life and caves all stop at the pool rims; snow did not, which was invisible
   for as long as no ridge happened to stand next to a pool. Put one there and
   snow generates on the lava pool's rim ring, slides down its inner face onto
   the lava two voxels below, and `snow + tag:hot -> water` lights a front that
   `water + tag:hot -> steam` and `steam -> water` sustain forever. It also
   accounted for all 19 page faults the standalone gate was reporting — moving
   matter writing through sentinels, PLAN_page_table.md risk 1.
