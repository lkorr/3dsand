#!/usr/bin/env bash
# W1-B: (1) image noise floor — render the BASE tree's shots a second time and
# diff them against the first base run; (2) --shader-stats for both trees.
W="C:/Users/Luke/Desktop/programming/3d sand voxel/.claude/worktrees/agent-aea0af9f265d2ee84"
RUNSH="C:/Users/Luke/Desktop/programming/3d sand voxel/scripts/run.sh"
cd "$W"
export SANDVOX_NO_CRASH_DIALOG=1
export SANDVOX_UNBUFFERED=1
export SANDVOX_PIPELINE_CACHE="C:/Users/Luke/Desktop/programming/3d sand voxel/sandvox_pipeline_cache.bin"

rm -f screenshot_*.bmp
SANDVOX_ASSET_DIR="$W/base_assets" bash "$RUNSH" ./sandvox_w1b.exe \
  --shot-frames screenshot_far,screenshot_cascade,screenshot_ground \
  > /tmp/shot_base2.log 2>&1
echo "base shots exit $?"
rm -rf shots_base2
mkdir shots_base2
mv screenshot_far.bmp screenshot_cascade.bmp screenshot_ground.bmp shots_base2/

SANDVOX_ASSET_DIR="$W/base_assets" bash "$RUNSH" ./sandvox_w1b.exe --shader-stats \
  > /tmp/stats_base.log 2>&1
echo "base shader-stats exit $?"
cp build/shader_stats.json /tmp/shader_stats_base.json

SANDVOX_ASSET_DIR="$W/assets" bash "$RUNSH" ./sandvox_w1b.exe --shader-stats \
  > /tmp/stats_new.log 2>&1
echo "new shader-stats exit $?"
cp build/shader_stats.json /tmp/shader_stats_new.json
echo "AB2 DONE"
