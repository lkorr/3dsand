#!/usr/bin/env bash
# Serialized build wrapper — one COMPILE at a time across all worktrees, and
# the link (which overwrites sandvox.exe) excluded from every exe run.
#
# 5 agents running `cmake --build` simultaneously spawn 5 unbounded builds that
# saturate RAM and CPU. This script takes two mkdir-based mutexes
# (scripts/svlock.sh) so builds queue instead of fighting, and caps per-build
# parallelism:
#
#   compile   under C:/sv-compile-lock   — cl.exe over `sandvox_core`
#   link      under C:/sv-gpu-lock       — taskkill sandvox.exe, then link
#   selftest  under C:/sv-gpu-lock       — the only exe launch this script does
#
# A `--gate` check (run.sh, GPU lock only) therefore never waits for anybody's
# compile phase; it waits for a link (seconds) or another run.
#
# Usage:
#   bash scripts/build.sh                       # build sandvox (Release)
#   bash scripts/build.sh --selftest            # build + run selftest
#   bash scripts/build.sh --config Debug        # build Debug
#   bash scripts/build.sh --configure           # cmake configure first
#   bash scripts/build.sh --fresh               # configure from an empty cache
#   bash scripts/build.sh --target sandvox      # explicit target
#   bash scripts/build.sh --gen vs|ninja        # force a generator (see below)
#
# GENERATOR. With sccache on PATH (scoop install sccache) the build dir is
# configured with "Ninja Multi-Config" + CMAKE_CXX_COMPILER_LAUNCHER=sccache,
# because the Visual Studio generator ignores compiler launchers. Objects for
# Tint and Jolt come out of the shared cache at C:/sv-deps/sccache — their
# sources live at one path for every worktree, so the second worktree's cold
# build is mostly hits. A build dir configured with the other generator is
# re-configured in place (`cmake --fresh`) with a message; the exe path is
# build/Release/sandvox.exe under both. SANDVOX_GENERATOR=vs (or --gen vs)
# keeps MSBuild.
set -eu

# ── Configuration ──────────────────────────────────────────────────────────
MAX_JOBS=6                       # cap cl.exe parallelism (half of 16 cores)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SVLOCK_TAG="build.sh"
# shellcheck source=svlock.sh
source "$SCRIPT_DIR/svlock.sh"

# ── Parse arguments ────────────────────────────────────────────────────────
CONFIG="Release"
TARGET="sandvox"
RUN_SELFTEST=false
RUN_CONFIGURE=false
GEN_WANT="${SANDVOX_GENERATOR:-auto}"   # auto | ninja | vs
WANT_FRESH=false                        # --fresh: drop CMakeCache.txt first
EXTRA_ARGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --selftest)     RUN_SELFTEST=true; shift ;;
    --configure)    RUN_CONFIGURE=true; shift ;;
    --fresh)        RUN_CONFIGURE=true; WANT_FRESH=true; shift ;;
    --config)       CONFIG="$2"; shift 2 ;;
    --target)       TARGET="$2"; shift 2 ;;
    --gen)          GEN_WANT="$2"; shift 2 ;;
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

# ── Toolchain: generator + compiler cache ─────────────────────────────────
GEN_VS="Visual Studio 17 2022"
GEN_NINJA="Ninja Multi-Config"
GEN=""
if [ "$GEN_WANT" = "vs" ]; then
  GEN="$GEN_VS"
elif command -v sccache >/dev/null 2>&1 && [ -z "${SANDVOX_NO_SCCACHE:-}" ]; then
  # Ninja needs cl.exe on PATH and INCLUDE/LIB in the environment, which the
  # VS generator never did (MSBuild finds them itself). vsenv.sh captures a
  # vcvarsall x64 environment once per VS install.
  if source "$SCRIPT_DIR/vsenv.sh" && command -v ninja >/dev/null 2>&1; then
    GEN="$GEN_NINJA"
    export SCCACHE_DIR="${SCCACHE_DIR:-C:/sv-deps/sccache}"
    export SCCACHE_CACHE_SIZE="${SCCACHE_CACHE_SIZE:-40G}"
  else
    echo "build.sh: sccache present but no MSVC env/ninja for it; using $GEN_VS" >&2
  fi
fi
if [ -z "$GEN" ]; then
  [ "$GEN_WANT" = "ninja" ] && { echo "build.sh: --gen ninja needs sccache + ninja + VS on this machine" >&2; exit 1; }
  GEN="$GEN_VS"
fi

# ── Configure if requested or needed ──────────────────────────────────────
# Outside the locks on purpose: configure only writes build system files, and
# making every agent queue for it would serialize the cheap part too.
CACHE="$ROOT/build/CMakeCache.txt"
HAVE_GEN=""
[ -f "$CACHE" ] && HAVE_GEN="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$CACHE" | tr -d '\r')"
FRESH=()
[ "$WANT_FRESH" = true ] && FRESH=(--fresh)
if [ -n "$HAVE_GEN" ] && [ "$HAVE_GEN" != "$GEN" ]; then
  echo "build.sh: build/ was configured with '$HAVE_GEN'; switching to '$GEN' (cmake --fresh, one cold build)"
  RUN_CONFIGURE=true
  FRESH=(--fresh)
fi
# The cache file appears at the START of a configure; the generator's own
# build file only at the end, so a configure that died half-way (a failed
# FetchContent step) is retried rather than handed to the build tool.
GEN_FILE="$ROOT/build/build.ninja"
[ "$GEN" = "$GEN_VS" ] && GEN_FILE="$ROOT/build/sandvox.sln"
if [ "$RUN_CONFIGURE" = true ] || [ ! -f "$CACHE" ] || [ ! -f "$GEN_FILE" ]; then
  echo "build.sh: configuring ($GEN)..."
  if [ "$GEN" = "$GEN_VS" ]; then
    cmake "${FRESH[@]+"${FRESH[@]}"}" -S "$ROOT" -B "$ROOT/build" -G "$GEN" -A x64
  else
    cmake "${FRESH[@]+"${FRESH[@]}"}" -S "$ROOT" -B "$ROOT/build" -G "$GEN"
  fi
fi

export CMAKE_BUILD_PARALLEL_LEVEL=$MAX_JOBS
T_START=$(date +%s)

# ── Phase 1: compile (compile lock) ───────────────────────────────────────
# For the `sandvox` target this builds `sandvox_compile` — every object file
# plus the dependency libraries — and links nothing. Any other target
# (movement_test, tint_cmd_tint_cmd) does not touch sandvox.exe, so it builds
# AND links here.
COMPILE_TARGET="$TARGET"
[ "$TARGET" = "sandvox" ] && COMPILE_TARGET="sandvox_compile"

svlock_acquire "$SVLOCK_COMPILE" "build:$(basename "$ROOT")"
echo "build.sh: compiling $COMPILE_TARGET ($CONFIG, $GEN) with max $MAX_JOBS parallel jobs..."
[ "$GEN" = "$GEN_NINJA" ] && sccache --zero-stats >/dev/null 2>&1 || true
# `set -e` would abort the script here on a failed build, skipping the
# diagnostics below, so the failure is captured rather than propagated.
BUILD_EXIT=0
cmake --build "$ROOT/build" --config "$CONFIG" --target "$COMPILE_TARGET" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" || BUILD_EXIT=$?
if [ "$GEN" = "$GEN_NINJA" ]; then
  # One line of cache attribution, so "was this build cold" is answered by
  # the log instead of by re-running it.
  sccache --show-stats 2>/dev/null \
    | grep -E '^(Compile requests executed|Cache hits|Cache misses|Non-cacheable compilations)\s' \
    | sed 's/  */ /g' | tr '\n' ';' | sed 's/^/build.sh: sccache /; s/;$/\n/' || true
fi
svlock_release "$SVLOCK_COMPILE"
T_COMPILE=$(date +%s)

if [ "$BUILD_EXIT" -ne 0 ]; then
  echo "build.sh: BUILD FAILED (exit $BUILD_EXIT)" >&2
  exit "$BUILD_EXIT"
fi
if [ "$TARGET" != "sandvox" ]; then
  echo "build.sh: $TARGET built in $(( T_COMPILE - T_START ))s."
  exit 0
fi

# ── Phase 2: link (+ selftest) under the GPU lock ─────────────────────────
svlock_acquire_gpu "link:$(basename "$ROOT")"

# Kill any running sandvox.exe INSIDE the lock. Killing here means nothing can
# start an exe between the kill and our link, because starting one requires
# this same lock (run.sh, and the selftest below). This used to run before the
# lock was taken, which made it useless under the load it exists to handle:
# agent A killed the exe, then waited minutes behind the mutex, and by the time
# it linked, agent B had launched a fresh sandvox.exe — LNK1104 anyway.
taskkill //F //IM sandvox.exe 2>/dev/null || true

echo "build.sh: linking sandvox ($CONFIG)..."
LINK_EXIT=0
cmake --build "$ROOT/build" --config "$CONFIG" --target sandvox \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" || LINK_EXIT=$?
T_LINK=$(date +%s)
if [ "$LINK_EXIT" -ne 0 ]; then
  svlock_release_gpu
  echo "build.sh: LINK FAILED (exit $LINK_EXIT)" >&2
  exit "$LINK_EXIT"
fi
echo "build.sh: build succeeded (compile $(( T_COMPILE - T_START ))s, link $(( T_LINK - T_COMPILE ))s)."

# ── Selftest — runs while we STILL HOLD the GPU lock ──────────────────────
# The exe must not be live while another agent links, and this is the only
# place the script starts one.
SELFTEST_EXIT=0
if [ "$RUN_SELFTEST" = true ]; then
  echo "build.sh: running selftest..."
  "$ROOT/build/$CONFIG/sandvox.exe" --selftest || SELFTEST_EXIT=$?
fi

svlock_release_gpu
trap - EXIT
exit "$SELFTEST_EXIT"
