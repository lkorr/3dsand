#!/usr/bin/env bash
# W1-B A/B driver: same exe, same command, two asset trees, back to back.
W="C:/Users/Luke/Desktop/programming/3d sand voxel/.claude/worktrees/agent-aea0af9f265d2ee84"
RUNSH="C:/Users/Luke/Desktop/programming/3d sand voxel/scripts/run.sh"
cd "$W"
export SANDVOX_NO_CRASH_DIALOG=1
export SANDVOX_UNBUFFERED=1
export SANDVOX_RUN_EXCLUSIVE=1
export SANDVOX_PIPELINE_CACHE="C:/Users/Luke/Desktop/programming/3d sand voxel/sandvox_pipeline_cache.bin"

rm -f screenshot_*.bmp
SANDVOX_ASSET_DIR="$W/base_assets" bash "$RUNSH" ./sandvox_w1b.exe \
  --render-budget --budget-cams noon,cascade --budget-arms baseline,nofar \
  --shot-frames screenshot_far,screenshot_cascade,screenshot_ground \
  > /tmp/ab_base.log 2>&1
echo "base exit $?"
rm -rf shots_base
mkdir shots_base
mv screenshot_far.bmp screenshot_cascade.bmp screenshot_ground.bmp shots_base/

SANDVOX_ASSET_DIR="$W/assets" bash "$RUNSH" ./sandvox_w1b.exe \
  --render-budget --budget-cams noon,cascade --budget-arms baseline,nofar \
  --shot-frames screenshot_far,screenshot_cascade,screenshot_ground \
  > /tmp/ab_new.log 2>&1
echo "new exit $?"
rm -rf shots_new
mkdir shots_new
mv screenshot_far.bmp screenshot_cascade.bmp screenshot_ground.bmp shots_new/
echo "AB DONE"
