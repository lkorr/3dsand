#!/usr/bin/env bash
# Serialized build wrapper — only one build runs at a time across all worktrees.
#
# 5 agents running `cmake --build` simultaneously spawn 5 unbounded MSBuild
# instances that saturate RAM and CPU. This script uses a mkdir-based mutex
# so builds queue instead of fighting, and caps per-build parallelism.
#
# Usage:
#   bash scripts/build.sh                       # build sandvox (Release)
#   bash scripts/build.sh --selftest            # build + run selftest
#   bash scripts/build.sh --config Debug        # build Debug
#   bash scripts/build.sh --configure           # cmake configure first
#   bash scripts/build.sh --target sandvox      # explicit target
#
# The lock is machine-global (lives in C:/sv-deps alongside the shared Dawn
# cache). Agents keep editing and searching while waiting — only the
# compile+link is serialized.
set -eu

# ── Configuration ──────────────────────────────────────────────────────────
MAX_JOBS=6                       # cap cl.exe parallelism (half of 16 cores)
LOCK_DIR="C:/sv-build-lock"      # mkdir-atomic mutex, outside any checkout
LOCK_STALE_SEC=600               # 10 min — kill a stuck lock

# ── Parse arguments ────────────────────────────────────────────────────────
CONFIG="Release"
TARGET="sandvox"
RUN_SELFTEST=false
RUN_CONFIGURE=false
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --selftest)     RUN_SELFTEST=true; shift ;;
    --configure)    RUN_CONFIGURE=true; shift ;;
    --config)       CONFIG="$2"; shift 2 ;;
    --target)       TARGET="$2"; shift 2 ;;
    *)              EXTRA_ARGS+=("$1"); shift ;;
  esac
done

# ── Locate project root ───────────────────────────────────────────────────
if [ -f "CMakeLists.txt" ]; then
  ROOT="$(pwd)"
elif [ -f "$(git rev-parse --show-toplevel 2>/dev/null)/CMakeLists.txt" ]; then
  ROOT="$(git rev-parse --show-toplevel)"
else
  echo "error: can't find CMakeLists.txt — run from project root or a worktree" >&2
  exit 1
fi

# ── Stale lock cleanup ────────────────────────────────────────────────────
# If the lock dir exists and its timestamp file is older than LOCK_STALE_SEC,
# the holder probably crashed. Remove it.
cleanup_stale_lock() {
  if [ -d "$LOCK_DIR" ] && [ -f "$LOCK_DIR/pid" ]; then
    local ts
    ts=$(cat "$LOCK_DIR/ts" 2>/dev/null || echo 0)
    local now
    now=$(date +%s)
    local age=$(( now - ts ))
    if [ "$age" -gt "$LOCK_STALE_SEC" ]; then
      echo "build.sh: removing stale lock (age ${age}s, holder pid $(cat "$LOCK_DIR/pid" 2>/dev/null || echo '?'))"
      rm -rf "$LOCK_DIR"
    fi
  fi
}

# ── Acquire lock ───────────────────────────────────────────────────────────
acquire_lock() {
  local waited=0
  while true; do
    cleanup_stale_lock
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      echo $$ > "$LOCK_DIR/pid"
      echo "$(basename "$ROOT")" > "$LOCK_DIR/who"
      date +%s > "$LOCK_DIR/ts"
      trap release_lock EXIT
      return
    fi
    if [ "$waited" -eq 0 ]; then
      local who
      who=$(cat "$LOCK_DIR/who" 2>/dev/null || echo "unknown")
      echo "build.sh: waiting for build lock (held by: $who)..."
    fi
    waited=$(( waited + 1 ))
    sleep 2
  done
}

release_lock() {
  rm -rf "$LOCK_DIR"
}

# ── Configure if requested or needed ──────────────────────────────────────
# Outside the lock on purpose: configure only writes build system files, and
# making every agent queue for it would serialize the cheap part too.
if [ "$RUN_CONFIGURE" = true ] || [ ! -d "$ROOT/build" ]; then
  echo "build.sh: configuring..."
  cmake -S "$ROOT" -B "$ROOT/build" -G "Visual Studio 17 2022" -A x64
fi

# ── Build (serialized) ────────────────────────────────────────────────────
acquire_lock
echo "build.sh: building $TARGET ($CONFIG) with max $MAX_JOBS parallel jobs..."

# Kill any running sandvox.exe INSIDE the lock. This used to run before
# acquire_lock, which made it useless under the load it exists to handle: agent
# A killed the exe, then waited minutes behind the mutex, and by the time it
# linked, agent B had launched a fresh sandvox.exe — LNK1104 anyway, after the
# full wait. Killing here means nothing can start an exe between the kill and
# our link, because starting one requires this same lock (see the selftest
# below). Our own --selftest is the only sanctioned launcher.
taskkill //F //IM sandvox.exe 2>/dev/null || true

export CMAKE_BUILD_PARALLEL_LEVEL=$MAX_JOBS
# `set -e` would abort the script here on a failed build, skipping the
# diagnostics below, so the failure is captured rather than propagated.
BUILD_EXIT=0
cmake --build "$ROOT/build" --config "$CONFIG" --target "$TARGET" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" || BUILD_EXIT=$?

if [ "$BUILD_EXIT" -ne 0 ]; then
  release_lock
  trap - EXIT
  echo "build.sh: BUILD FAILED (exit $BUILD_EXIT)" >&2
  exit "$BUILD_EXIT"
fi

echo "build.sh: build succeeded."

# ── Selftest — runs while we STILL HOLD the lock ──────────────────────────
# The exe must not be live while another agent links, and this is the only
# place the script starts one. Releasing the lock first (the old behaviour)
# put every selftest run in direct competition with every other agent's link
# step, which is the other half of the LNK1104 problem.
SELFTEST_EXIT=0
if [ "$RUN_SELFTEST" = true ]; then
  echo "build.sh: running selftest..."
  "$ROOT/build/$CONFIG/sandvox.exe" --selftest || SELFTEST_EXIT=$?
fi

release_lock
trap - EXIT
exit "$SELFTEST_EXIT"
