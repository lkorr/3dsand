#!/usr/bin/env bash
# check_worldbuilder.sh — the World map page's WORLDBUILDER tools in real
# headless Chrome against the real tuner server (docs/PLAN_worldbuilder.md K1):
# the tool registry, the brush mask, the Range tool (draw, live preview, bake
# in the terrain worker, undo / redo, rebake, unbake), the Generate panel, the
# stream-power erosion, the overlays, the Measure tool. Drives
# assets/worldbuilder_test.html; prints its PASS / FAIL lines.
#
# What Node cannot see (scripts/test_terraingen.mjs, test_hydrology.mjs,
# test_spline.mjs cover the maths): the module worker booting, the transfer of
# a job's typed arrays both ways, the page's pending / undo plumbing, the
# canvas painting the overlays.
#
#   bash scripts/check_worldbuilder.sh [--port N] [--shot out.png]
#
# Needs no built exe. Writes a synthetic map to assets/worldmap/_k1test/
# (deleted on exit). ~1-2 minutes (the bakes are real).
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${K1_PORT:-8798}"
SHOT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) shift;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_worldbuilder: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/k1_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF" "$ROOT/assets/worldmap/_k1test"
}
trap cleanup EXIT
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/worldbuilder_test.html"; then
  echo "check_worldbuilder: something else is serving port $PORT (no worldbuilder_test.html there). Pass --port." >&2
  exit 2
fi
URL="http://127.0.0.1:$PORT/worldbuilder_test.html"
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl --enable-unsafe-swiftshader "--user-data-dir=$PROF"
        --no-first-run --virtual-time-budget=600000 --window-size=1600,1000)
if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_worldbuilder: wrote $SHOT"
fi
OUT="$("$CHROME" "${COMMON[@]}" --dump-dom "$URL" 2>/dev/null | python -c "
import sys, re
s = sys.stdin.read()
m = re.search(r'<pre id=\"out\">(.*?)</pre>', s, re.S)
print(m.group(1) if m else '(no harness output -- the page did not run)')
")"
echo "$OUT"
case "$OUT" in
  *"RESULT OK"*) echo "check_worldbuilder: PASS"; exit 0;;
  *) echo "check_worldbuilder: FAIL" >&2; exit 1;;
esac
