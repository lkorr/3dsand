#!/usr/bin/env bash
# set_voxel_scale.sh — change kVoxelMeters and bring the baked assets with it.
#
#   bash scripts/set_voxel_scale.sh 0.05     # 5 cm voxels
#   bash scripts/set_voxel_scale.sh 0.10     # back to 10 cm
#
# WHY THIS EXISTS. Voxel size is meant to be a knob you can flip while
# experimenting, and everything authored in METRES follows it for free: the
# player capsule, mob and item sizes (derived from `artVoxelsPerMetre`), every
# worldgen tuning row, the melee reaches, the mob gait budgets.
#
# The tree atlas is the one thing that CANNOT follow automatically, because it
# is a baked voxel grid rather than a formula — so it has to be re-baked, and
# src/sim/treeatlas.cpp deliberately REFUSES to load an atlas baked at a
# different scale than the engine runs at. That refusal is what makes a stale
# forest a loud failure instead of a silently half-size one, and this script is
# what keeps satisfying it from being a thing you have to remember.
#
# It does NOT build, and it does NOT rebaseline: both take the machine-global
# mutex and both are decisions, not consequences. It prints the two commands.
set -euo pipefail

cd "$(dirname "$0")/.."

# Not `grep -oP`: this shell's grep refuses PCRE outside a unibyte/UTF-8
# locale, and the script has to work in the same Git Bash as everything else.
read_current() {
  python -c "import re;print(re.search(r'kVoxelMeters\s*=\s*([0-9.]+)f', open('src/sim/world.h').read()).group(1))"
}

if [ $# -ne 1 ]; then
  cur=$(read_current)
  echo "usage: bash scripts/set_voxel_scale.sh <metres>    (currently ${cur:-?})" >&2
  echo "  e.g. 0.10 (10 cm, the authoring baseline), 0.05, 0.025" >&2
  exit 2
fi

TARGET="$1"

# The scale has to be the reciprocal of a WHOLE number of voxels per metre.
# kVoxelsPerMetre is an integer (world.h derives it), and every asset scale is
# derived from it by integer division — a non-integer here would silently floor
# and put every authored thing at a size nobody chose.
VPM=$(python -c "
import sys
m = float('$TARGET')
if m <= 0: sys.exit('voxel size must be positive')
v = round(1.0 / m)
if abs(1.0 / v - m) > 1e-9:
    sys.exit(f'{m} is not 1/N for a whole N; nearest is {1.0/v:.6g} ({v} vox/m)')
print(v)
")

CUR=$(read_current)
echo "kVoxelMeters ${CUR} -> ${TARGET}  (${VPM} voxels/metre)"

# Assets are authored at 10 vox/m and can be block-replicated FINER but never
# resampled coarser (that would be lossy and would need a deterministic
# tie-break to stay rule-1 clean). Going below the authoring baseline is
# therefore a real limitation, not a rounding detail — say so rather than
# letting mob defs fail to load one at a time.
if [ "$VPM" -lt 10 ]; then
  echo "REFUSING: ${VPM} vox/m is COARSER than the 10 vox/m the art is authored" >&2
  echo "at. Mob and item art can be replicated finer, never resampled coarser," >&2
  echo "so those defs would fail to load. Re-author the art first." >&2
  exit 1
fi

python - "$TARGET" <<'PY'
import re, sys
target = sys.argv[1]
p = 'src/sim/world.h'
s = open(p, encoding='utf-8').read()
new, n = re.subn(r'(constexpr float kVoxelMeters = )[0-9.]+f',
                 lambda m: m.group(1) + target + 'f', s, count=1)
if n != 1:
    sys.exit('could not find the kVoxelMeters definition in ' + p)
open(p, 'w', encoding='utf-8').write(new)
print('  src/sim/world.h updated')
PY

echo "re-baking the tree atlas at ${VPM} vox/m (this takes ~30 s)..."
node scripts/bake_trees.mjs

cat <<EOF

Done. Two commands left, both of which take the build mutex:

  bash scripts/build.sh
  bash scripts/run.sh ./build/Release/sandvox.exe --selftest --rebaseline

The hash WILL move — a re-baked atlas is new engine input, and that is a
notification, not a regression (CLAUDE.md rule 1). Existing saves will refuse
to load: worldio.cpp compares kVoxelMeters bit-for-bit, which is correct.

Then confirm sizes held with the cheap gate:

  bash scripts/run.sh ./build/Release/sandvox.exe --selftest --gate scale
EOF
