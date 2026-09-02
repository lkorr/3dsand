#!/usr/bin/env bash
# Serialized exe-run wrapper — only ONE sandvox.exe runs at a time across all
# sessions and worktrees.
#
# Three agents running measurement harnesses simultaneously saturate the GPU
# and throttle the whole machine, and every number measured that way is
# garbage (the runs contend for the same device). This wrapper takes the GPU
# lock (C:/sv-gpu-lock, scripts/svlock.sh), which build.sh holds for its LINK
# step and its own selftest: a run excludes any link (the LNK1104 failure
# build.sh kills sandvox.exe to prevent), and a link excludes any run. It does
# NOT take the compile lock — a `--gate` check no longer queues behind five
# minutes of somebody else's cl.exe.
#
# Usage:
#   bash scripts/run.sh ./build/Release/sandvox.exe --frames 1200 --autofly-hard
#   SANDVOX_PT_DEBUG=1 bash scripts/run.sh ./build/Release/sandvox.exe --selftest --gate streaming
#   SANDVOX_RUN_EXCLUSIVE=1 bash scripts/run.sh ./build/Release/sandvox.exe --perf
#
# SANDVOX_RUN_EXCLUSIVE=1 takes the compile lock as well, for a measurement
# that must not share the CPU with a compile (--perf, --render-budget numbers
# you intend to quote). Compile lock first, then GPU, the same order build.sh
# never needs (it holds one at a time), so there is no deadlock.
#
# Works from any cwd (it only wraps the command), so worktree agents may call
# it by absolute path from the main checkout.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SVLOCK_TAG="run.sh"
# shellcheck source=svlock.sh
source "$SCRIPT_DIR/svlock.sh"

[ $# -ge 1 ] || { echo "usage: run.sh <command...>" >&2; exit 2; }

WHO="run:$(basename "$(pwd)")"
if [ -n "${SANDVOX_RUN_EXCLUSIVE:-}" ]; then
  svlock_acquire "$SVLOCK_COMPILE" "$WHO (exclusive)"
fi
svlock_acquire_gpu "$WHO"

RUN_EXIT=0
"$@" || RUN_EXIT=$?

svlock_release_all
trap - EXIT
exit "$RUN_EXIT"
