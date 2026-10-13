#!/usr/bin/env bash
# --fluid-bench hill with SANDVOX_FLUID_VARIANTS A/B arms on the same frames, against a FROZEN copy of assets/ (edits mid-run cannot leak in).
# Usage: bash scripts/perf/fluid_ab.sh <tag> <variants e.g. 1,5,9> [n]   -> build/perf/fluid_<tag>/ (logs + fluidarm_*.bmp at ticks 200,400)
#   Compare arm shots with: python scripts/perf/bmpdiff.py a.bmp b.bmp   (v0 vs an unused variant = the noise floor)
cd "$(dirname "$0")/../.."
tag="$1"; vars="$2"; n="${3:-1}"
SNAP="build/perf/snap_$tag"
rm -rf "$SNAP"; mkdir -p "$SNAP"
cp -r assets "$SNAP/assets"; cp -r tests "$SNAP/tests"
export SANDVOX_ASSET_DIR="$PWD/$SNAP/assets"
export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_RUN_EXCLUSIVE=1
EXE_ABS="$PWD/${EXE:-build/Release/sandvox.exe}"
mkdir -p "build/perf/fluid_$tag" && cd "build/perf/fluid_$tag"
for i in $(seq 1 "$n"); do
  SANDVOX_FLUID_VARIANTS="$vars" SANDVOX_FLUID_ARM_SHOTS=200,400 \
    bash ../../../scripts/run.sh "$EXE_ABS" --fluid-bench hill > "hill_$i.log" 2>&1
  echo "$tag run $i exit $?"
  grep -h 'paired fluid arms' "hill_$i.log"
done
