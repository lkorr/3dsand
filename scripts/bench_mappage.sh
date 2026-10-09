#!/usr/bin/env bash
# bench_mappage.sh — how fast the tuner's Environment > World map page pans and
# zooms. Starts a private tuner server, opens assets/mappage_bench.html in
# headless Chrome IN REAL TIME (no --virtual-time-budget: that fast-forwards
# the clock and every number would be fiction), which loads the real tuner on
# the map page and scripts wheel zooms and right-drag pans, timing every
# paint() (total + per layer, map.js's __mapProf hook) and every frame.
#
#   bash scripts/bench_mappage.sh [--port N] [--map NAME] [--voxels 0|1|both]
#                                 [--gpu] [--flush] [--window W,H] [--passes N] [--json out.json]
#
# --voxels both (the default) runs the bench twice: with the voxel preview
# streaming under the map and without it.
# --gpu lets headless Chrome use the real GPU (the owner's case: accelerated
# canvas). Without it Chrome is on SwiftShader / software raster: the 2D
# canvas numbers are CPU-honest, the voxel view's are pessimistic.
#
# The voxel view needs build/Release/sandvox.exe (the tuner server launches
# --voxserve). A worktree with no build can point build/Release at the main
# checkout's with a junction:
#   python -c "import _winapi; _winapi.CreateJunction(r'<main>\build\Release', r'build\Release')"
# (remove it with python -c "import os; os.rmdir(r'build\Release')" -- never rm -rf it).
# Without an exe the bench still runs; the voxel view just shows its error.
#
# Needs Node >= 22 (scripts/bench_mappage_cdp.mjs speaks CDP over the global WebSocket).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT=8811
MAP=default
VOX=both
GPU=0
PASSES=2
WIN=1920,1080
JSON=""
EXTRA=""
while [ $# -gt 0 ]; do
  case "$1" in
    --port) PORT="$2"; shift 2;;
    --map) MAP="$2"; shift 2;;
    --voxels) VOX="$2"; shift 2;;
    --gpu) GPU=1; shift;;
    --flush) EXTRA="$EXTRA&flush=1"; shift;;
    --window) WIN="$2"; shift 2;;
    --passes) PASSES="$2"; shift 2;;
    --json) JSON="$2"; shift 2;;
    *) echo "bench_mappage: unknown arg $1" >&2; exit 2;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "bench_mappage: no Chrome/Edge found" >&2; exit 2; }

LOG="$(mktemp)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >"$LOG" 2>&1 &
SERVER=$!
PROF=""
CHR=""
cleanup() {
  [ -n "$CHR" ] && kill "$CHR" 2>/dev/null || true
  kill "$SERVER" 2>/dev/null || true
  [ -n "$PROF" ] && rm -rf "$PROF"
  rm -f "$LOG"
}
trap cleanup EXIT
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/mappage_bench.html"; then
  echo "bench_mappage: something else is serving port $PORT (no mappage_bench.html there). Pass --port." >&2
  exit 2
fi

run_one() {   # $1 = voxels 0|1
  PROF="$(mktemp -d)"
  local DPORT=$((PORT + 1000))
  local FLAGS=(--headless=new "--user-data-dir=$PROF" --no-first-run "--window-size=$WIN"
               "--remote-debugging-port=$DPORT" --disable-background-timer-throttling
               --disable-renderer-backgrounding --disable-backgrounding-occluded-windows)
  if [ "$GPU" = 1 ]; then FLAGS+=(--enable-gpu --ignore-gpu-blocklist --use-angle=d3d11)
  else FLAGS+=(--disable-gpu --use-angle=swiftshader-webgl --enable-unsafe-swiftshader); fi
  "$CHROME" "${FLAGS[@]}" about:blank >/dev/null 2>&1 &
  CHR=$!
  local out=""
  [ -n "$JSON" ] && out="${JSON%.json}_vox$1.json"
  echo "=== voxels=$1 gpu=$GPU ==="
  node "$ROOT/scripts/bench_mappage_cdp.mjs" "$DPORT" \
    "http://127.0.0.1:$PORT/mappage_bench.html?map=$MAP&voxels=$1&passes=$PASSES$EXTRA" $out || true
  kill "$CHR" 2>/dev/null || true
  wait "$CHR" 2>/dev/null || true
  CHR=""
  sleep 1; rm -rf "$PROF" 2>/dev/null || true; PROF=""
}
case "$VOX" in
  both) run_one 1; run_one 0;;
  *) run_one "$VOX";;
esac
