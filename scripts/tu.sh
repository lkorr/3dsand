#!/usr/bin/env bash
# tu.sh <src-path-relative-to-repo> [...more]
#
# Compile ONE translation unit under the machine-global compile mutex, without
# linking and without the GPU lock. The iteration loop for a large refactor of
# a single .cpp: main.cpp alone is ~40 s, a full build.sh is ~140 s plus a link
# that fights every other session's runs, and a compile error does not need a
# binary. Not a substitute for build.sh -- nothing here produces an exe.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/vsenv.sh
source scripts/svlock.sh
svlock_acquire "$SVLOCK_COMPILE" "tu:$(basename "$(pwd)")"
trap 'svlock_release "$SVLOCK_COMPILE"' EXIT
targets=()
for s in "$@"; do
  targets+=("CMakeFiles/sandvox_core.dir/Release/${s}.obj")
done
ninja -C build -f build-Release.ninja "${targets[@]}"
