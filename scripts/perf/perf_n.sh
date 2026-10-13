#!/usr/bin/env bash
# n exclusive runs of one --perf scenario -> build/perf/<tag>_<scenario>_<i>.{json,log}; EXE= overrides the exe.
# Usage: bash scripts/perf/perf_n.sh <tag> <scenario> [n] [extra args...]
#   Summarise with: python scripts/perf/perfsum.py build/perf/<tag>_*.json  (and passsum.py)
cd "$(dirname "$0")/../.."
tag="$1"; sc="$2"; n="${3:-3}"; shift 3 || shift $#
export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_RUN_EXCLUSIVE=1
mkdir -p build/perf
for i in $(seq 1 "$n"); do
  bash scripts/run.sh "${EXE:-./build/Release/sandvox.exe}" --perf --scenario "$sc" \
    --perf-out "build/perf/${tag}_${sc}_$i.json" "$@" > "build/perf/${tag}_${sc}_$i.log" 2>&1
  echo "$tag $sc run $i exit $?"
done
