#!/usr/bin/env bash
# The machine-global mutexes build.sh and run.sh share. SOURCED, not run.
#
# TWO locks since 2026-09-02, where there used to be one (C:/sv-build-lock):
#
#   C:/sv-compile-lock   held while cl.exe runs. Caps the machine at ONE
#                        compile at a time (six jobs), which is the whole reason
#                        the mutex exists — five agents' MSBuilds at once
#                        saturated RAM and CPU.
#   C:/sv-gpu-lock       held during every sandvox.exe run AND during the link
#                        step. A link overwrites build/Release/sandvox.exe and
#                        must not race a live run (LNK1104 — build.sh taskkills
#                        under this lock, and a run holding it is what keeps
#                        that taskkill from killing somebody's selftest).
#
# Before the split, a `--gate determinism` check (~5 s) queued behind a
# worktree's five-minute compile it had no conflict with. Measured 2026-09-01:
# seven agents, eight wall-clock hours, most of it in this queue.
#
# Semantics (unchanged from a460282, which every caller relied on):
#   * mkdir-atomic; the holder's pid/who/ts live inside the directory.
#   * A lock whose ts is older than SVLOCK_STALE_SEC is presumed abandoned and
#     removed. A heartbeat refreshes ts every 60 s while we hold it, so a
#     legitimate long build or run is never stolen.
#   * Only the OWNER releases (pid check). A lock stolen as stale while we
#     still held it belongs to the new holder; deleting it would let the next
#     build.sh taskkill the new holder's live run.
#
# scripts/tuner_server.py has a Python mirror of the GPU lock (RunLock) that
# must agree with this file about the directory and the stale window.
#
#   source scripts/svlock.sh
#   svlock_acquire "$SVLOCK_GPU" "run:$(basename "$(pwd)")"
#   ...
#   svlock_release "$SVLOCK_GPU"        # or let the EXIT trap do it

SVLOCK_COMPILE="C:/sv-compile-lock"
SVLOCK_GPU="C:/sv-gpu-lock"
SVLOCK_STALE_SEC="${SVLOCK_STALE_SEC:-600}"   # 10 min — kill a stuck lock

declare -A SVLOCK_HB=()      # dir -> heartbeat pid, for the locks we hold
SVLOCK_TAG="${SVLOCK_TAG:-$(basename "${BASH_SOURCE[-1]:-$0}")}"

svlock_cleanup_stale() {
  local dir=$1
  if [ -d "$dir" ] && [ -f "$dir/pid" ]; then
    local ts now age
    # A MISSING OR EMPTY ts IS NOT "AGE 1.7 BILLION SECONDS". It used to be
    # read as 0, so a waiter that caught the holder between `mkdir` and its
    # first `date > ts` (or the heartbeat mid-rewrite) declared a fresh lock
    # stale, removed it, and build.sh's pre-link taskkill then killed the
    # holder's run — three full acceptance suites died at exactly 10 min on
    # 2026-09-02 that way, always with "age 1788378854s" in the killer's log.
    # Fall back to the lock directory's own mtime, which exists from mkdir on.
    ts=$(cat "$dir/ts" 2>/dev/null || true)
    [ -n "$ts" ] || ts=$(stat -c %Y "$dir" 2>/dev/null || date +%s)
    now=$(date +%s)
    age=$(( now - ts ))
    if [ "$age" -gt "$SVLOCK_STALE_SEC" ]; then
      echo "$SVLOCK_TAG: removing stale $(basename "$dir") (age ${age}s, holder pid $(cat "$dir/pid" 2>/dev/null || echo '?'))"
      rm -rf "$dir"
    fi
  fi
}

# Blocks until acquired. Installs an EXIT trap that releases every lock this
# shell holds, and starts a heartbeat for this one.
svlock_acquire() {
  local dir=$1 who=$2 waited=0
  while true; do
    svlock_cleanup_stale "$dir"
    if mkdir "$dir" 2>/dev/null; then
      # ts BEFORE pid: the stale check only looks at directories with a pid
      # file, so the timestamp must already be there when it does.
      date +%s > "$dir/ts"
      echo "$who" > "$dir/who"
      echo $$ > "$dir/pid"
      trap svlock_release_all EXIT
      # Refresh only while the pid file is still ours, so a stolen lock is
      # never kept alive by the loser. Written to a temp name and renamed so
      # a reader never sees the truncated file `>` leaves mid-write.
      ( while true; do
          sleep 60
          [ "$(cat "$dir/pid" 2>/dev/null)" = "$$" ] || exit 0
          date +%s > "$dir/ts.new" 2>/dev/null || exit 0
          mv -f "$dir/ts.new" "$dir/ts" 2>/dev/null || exit 0
        done ) &
      SVLOCK_HB[$dir]=$!
      return 0
    fi
    if [ "$waited" -eq 0 ]; then
      echo "$SVLOCK_TAG: waiting for $(basename "$dir") (held by: $(cat "$dir/who" 2>/dev/null || echo unknown))..."
    fi
    waited=$(( waited + 1 ))
    sleep 2
  done
}

svlock_release() {
  local dir=$1
  if [ -n "${SVLOCK_HB[$dir]:-}" ]; then
    kill "${SVLOCK_HB[$dir]}" 2>/dev/null || true
    # Reap it here, or bash reports "Terminated ( while true; do ..." into
    # the caller's output on the next prompt.
    wait "${SVLOCK_HB[$dir]}" 2>/dev/null || true
    unset 'SVLOCK_HB[$dir]'
  fi
  if [ "$(cat "$dir/pid" 2>/dev/null)" = "$$" ]; then
    rm -rf "$dir"
  elif [ -d "$dir" ]; then
    echo "$SVLOCK_TAG: $(basename "$dir") no longer ours (holder: $(cat "$dir/who" 2>/dev/null || echo '?')) - not releasing" >&2
  fi
}

svlock_release_all() {
  local d
  for d in "${!SVLOCK_HB[@]}"; do svlock_release "$d"; done
}

# TRANSITION. Checkouts that predate the split still take the single
# C:/sv-build-lock around their builds AND their runs, and their build.sh
# taskkills sandvox.exe under it. Until every live worktree carries this
# file, a GPU-lock holder ALSO holds the legacy lock, so an old build.sh
# cannot kill our run and an old run.sh cannot overlap our link. Old-style
# builds still overlap our COMPILE phase (CPU contention only — nothing an
# old build does to the exe can hurt a compile). Delete this once no worktree
# is on the old scripts.
SVLOCK_LEGACY="C:/sv-build-lock"
svlock_acquire_gpu() {
  local who=$1
  svlock_acquire "$SVLOCK_GPU" "$who"
  svlock_acquire "$SVLOCK_LEGACY" "$who"
}
svlock_release_gpu() {
  svlock_release "$SVLOCK_LEGACY"
  svlock_release "$SVLOCK_GPU"
}

# `bash scripts/svlock.sh` prints who holds what.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  for d in "$SVLOCK_COMPILE" "$SVLOCK_GPU"; do
    if [ -d "$d" ]; then
      printf '%-22s held by %s (pid %s, %ss ago)\n' "$(basename "$d")" \
        "$(cat "$d/who" 2>/dev/null || echo '?')" "$(cat "$d/pid" 2>/dev/null || echo '?')" \
        "$(( $(date +%s) - $(cat "$d/ts" 2>/dev/null || echo 0) ))"
    else
      printf '%-22s free\n' "$(basename "$d")"
    fi
  done
fi
