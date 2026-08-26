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
