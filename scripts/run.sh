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

# SHARED COMPILE CACHES, machine-global, unless the caller already chose.
# Both caches were CWD-relative by default, so a worktree's first run paid
# Tint + spirv-opt for every entry point (shader_cache/) AND the driver's full
# compile of every pipeline (sandvox_pipeline_cache.bin) that the main checkout
# had already done from identical source. Both are content-keyed on the engine
# side: the SPIR-V file name is a hash of (assembled source, entry point,
# optimizer recipe), and the driver blob is keyed by the driver on the SPIR-V
# it was handed - so a worktree sharing them with main hits exactly when its
# shaders are byte-identical and misses exactly when they are not. Same
# directory as the sccache object cache, for the same reason it lives there.
#
# The first switch-over seeds the shared pipeline cache from the CWD's warm
# one, if there is one, so nobody pays a cold compile for the move.
export SANDVOX_SHADER_CACHE="${SANDVOX_SHADER_CACHE:-C:/sv-deps/shader_cache}"
export SANDVOX_PIPELINE_CACHE="${SANDVOX_PIPELINE_CACHE:-C:/sv-deps/sandvox_pipeline_cache.bin}"
if [ ! -f "$SANDVOX_PIPELINE_CACHE" ] && [ -f sandvox_pipeline_cache.bin ]; then
  mkdir -p "$(dirname "$SANDVOX_PIPELINE_CACHE")"
  cp sandvox_pipeline_cache.bin "$SANDVOX_PIPELINE_CACHE.seed.$$" \
    && mv "$SANDVOX_PIPELINE_CACHE.seed.$$" "$SANDVOX_PIPELINE_CACHE" \
    && echo "run.sh: seeded $SANDVOX_PIPELINE_CACHE from ./sandvox_pipeline_cache.bin"
fi

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
