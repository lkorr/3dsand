#!/usr/bin/env bash
# Serialized exe-run wrapper — only ONE sandvox.exe runs at a time across all
# sessions and worktrees.
#
# Three agents running measurement harnesses simultaneously saturate the GPU
# and throttle the whole machine, and every number measured that way is
# garbage (the runs contend for the same device). This wrapper shares the
# BUILD mutex (C:/sv-build-lock), deliberately: a run also excludes any link
# step (the LNK1104 failure build.sh kills sandvox.exe to prevent), and a
# build excludes any run.
#
# Usage:
#   bash scripts/run.sh ./build/Release/sandvox.exe --frames 1200 --autofly-hard
#   SANDVOX_PT_DEBUG=1 bash scripts/run.sh ./build/Release/sandvox.exe --selftest --gate streaming
#
# Works from any cwd (it only wraps the command), so worktree agents may call
# it by absolute path from the main checkout.
set -eu

LOCK_DIR="C:/sv-build-lock"      # SAME lock as build.sh — runs exclude builds
LOCK_STALE_SEC=600               # 10 min — kill a stuck lock

[ $# -ge 1 ] || { echo "usage: run.sh <command...>" >&2; exit 2; }

cleanup_stale_lock() {
  if [ -d "$LOCK_DIR" ] && [ -f "$LOCK_DIR/pid" ]; then
    local ts
    ts=$(cat "$LOCK_DIR/ts" 2>/dev/null || echo 0)
    local now
    now=$(date +%s)
    local age=$(( now - ts ))
    if [ "$age" -gt "$LOCK_STALE_SEC" ]; then
      echo "run.sh: removing stale lock (age ${age}s, holder pid $(cat "$LOCK_DIR/pid" 2>/dev/null || echo '?'))"
      rm -rf "$LOCK_DIR"
    fi
  fi
}

REFRESH_PID=""
release_lock() {
  [ -n "$REFRESH_PID" ] && kill "$REFRESH_PID" 2>/dev/null || true
  rm -rf "$LOCK_DIR"
}

acquire_lock() {
  local waited=0
  while true; do
    cleanup_stale_lock
    if mkdir "$LOCK_DIR" 2>/dev/null; then
      echo $$ > "$LOCK_DIR/pid"
      echo "run:$(basename "$(pwd)")" > "$LOCK_DIR/who"
      date +%s > "$LOCK_DIR/ts"
      trap release_lock EXIT
      return
    fi
    if [ "$waited" -eq 0 ]; then
      local who
      who=$(cat "$LOCK_DIR/who" 2>/dev/null || echo "unknown")
      echo "run.sh: waiting for build/run lock (held by: $who)..."
    fi
    waited=$(( waited + 1 ))
    sleep 2
  done
}

acquire_lock

# Refresh the lock timestamp while the command runs, so a legitimate long run
# (full selftest, a 1200-frame harness) is not stolen as "stale" at 10 min.
( while true; do sleep 60; date +%s > "$LOCK_DIR/ts" 2>/dev/null || exit 0; done ) &
REFRESH_PID=$!

RUN_EXIT=0
"$@" || RUN_EXIT=$?

release_lock
trap - EXIT
exit "$RUN_EXIT"
