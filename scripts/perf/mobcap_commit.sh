#!/usr/bin/env bash
# mob-cap64 x3 fights (exclusive) on ANOTHER commit's tree and exe, with that tree's own compiled-in assets.
# Usage: git worktree add --detach _old <commit>; (cd _old && SANDVOX_DEPS=C:/sv-deps bash scripts/build.sh)
#        bash scripts/perf/mobcap_commit.sh _old <tag>   -> build/perf/mobcap_<tag>.log
#   Then: python scripts/perf/fightcmp.py build/perf/mobcap_<a>.log build/perf/mobcap_<b>.log
cd "$(dirname "$0")/../.."
tree="$1"; tag="$2"
OUT="$PWD/build/perf/mobcap_$tag.log"
mkdir -p build/perf
cd "$tree" || exit 1
cp build/Release/sandvox.exe "build/Release/sandvox_$tag.exe"
export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_RUN_EXCLUSIVE=1 SANDVOX_MOBCAP_FIGHTS=3
bash scripts/run.sh "./build/Release/sandvox_$tag.exe" --selftest --gate mob-cap64 > "$OUT" 2>&1
echo "mobcap $tag exit $?"
