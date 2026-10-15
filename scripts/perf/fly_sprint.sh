#!/usr/bin/env bash
# SPRINT flight (32.5 m/s = player.flySprint) along the 2026-10-11_12-10-09 ticket route: n exclusive
# --frames 1200 runs, 600-frame hold then 595 measured flight frames, no mid-run F5, GPU pass rows on.
# Usage: bash scripts/perf/fly_sprint.sh <tag> [n] [extra env as VAR=val ...]  -> build/perf/sprint_<tag>_<i>.log
cd "$(dirname "$0")/../.."
tag="${1:-sprint}"; n="${2:-1}"; shift 2 2>/dev/null
export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_RUN_EXCLUSIVE=1 SANDVOX_FRAMES_NO_RELOAD=1 SANDVOX_FRAMES_PASSES="${SANDVOX_FRAMES_PASSES:-build/perf/passes_$tag.csv}" SANDVOX_FRAMES_CSV="${SANDVOX_FRAMES_CSV:-build/perf/frames_$tag.csv}"
export SANDVOX_AT_FLY="${SANDVOX_AT_FLY:-32.5,0,0}" SANDVOX_AT_FLY_AFTER=600 SANDVOX_FRAMES_STATS_FROM=605
mkdir -p build/perf
for i in $(seq 1 "$n"); do
  env "$@" bash scripts/run.sh "${EXE:-./build/Release/sandvox.exe}" --frames 1200 \
    --at "${SANDVOX_SPRINT_AT:-10585.33,686.62,-10815.28,0.5,0.4}" > "build/perf/sprint_${tag}_$i.log" 2>&1
  echo "sprint $tag run $i exit $? :: $(grep -o 'whole-frame ms.*' build/perf/sprint_${tag}_$i.log)"
done
