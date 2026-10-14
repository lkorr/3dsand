#!/usr/bin/env bash
# The 2026-10-11_12-10-09 flight: n exclusive --frames 1200 runs at the ticket camera, 13.75 m/s +x after a 600-frame hold.
# Usage: bash scripts/perf/fly_ticket.sh <tag> [n]   -> build/perf/fly_<tag>_<i>.log ("whole-frame ms", the GPU tables, the shift line)
# SANDVOX_FRAMES_NO_RELOAD=1 by default (2026-10-14): a default --frames 1200 fires an F5 at frame 600 --
# the moment the flight starts -- and the far pipelines' re-arrival queues a whole far FullRefill, which
# put box-face seam handoffs and a refill's GPU load into every earlier number. Export
# SANDVOX_FRAMES_NO_RELOAD=0 to keep the reload (the pre-2026-10-14 harness).
cd "$(dirname "$0")/../.."
tag="${1:-fly}"; n="${2:-3}"
export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_RUN_EXCLUSIVE=1
export SANDVOX_FRAMES_NO_RELOAD="${SANDVOX_FRAMES_NO_RELOAD:-1}"
[ "$SANDVOX_FRAMES_NO_RELOAD" = "0" ] && unset SANDVOX_FRAMES_NO_RELOAD
export SANDVOX_AT_FLY=13.75,0,0 SANDVOX_AT_FLY_AFTER=600 SANDVOX_FRAMES_STATS_FROM=605
mkdir -p build/perf
for i in $(seq 1 "$n"); do
  bash scripts/run.sh "${EXE:-./build/Release/sandvox.exe}" --frames 1200 \
    --at 10585.33,686.62,-10815.28,0.5,0.4 > "build/perf/fly_${tag}_$i.log" 2>&1
  echo "fly $tag run $i exit $?"
done
