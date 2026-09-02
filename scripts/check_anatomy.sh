#!/usr/bin/env bash
# check_anatomy.sh — does the Models tab's ANATOMY row (peel / fill layer /
# apply recipe) work on the real page?
#
# `node scripts/test_anatomy.mjs` covers the arithmetic in anatomy.js. This
# drives assets/anatomy_test.html — the real tuner.html in an iframe, the real
# server, the shipped human — and measures what the InstancedMesh draws under a
# peel, what Fill layer paints, what one Undo restores and what Apply recipe
# brings back.
#
#   bash scripts/check_anatomy.sh              # verdict only
#   bash scripts/check_anatomy.sh --shot out.png
#
# REAL TIME, NOT --virtual-time-budget / --dump-dom. The Models tab runs a
# three.js rAF loop over a 26k-cube InstancedMesh; under a virtual clock every
# 16 ms frame is a real swiftshader render, so a 180 s budget is thousands of
# frames and the dump never comes (measured: nothing after 600 s; the attacks
# harness hangs the same way on this machine). The harness POSTs its verdict
# to /api/testresult instead (the perfview check's pattern) and this shell
# waits for that file, with a hard cap.
#
# Needs Chrome (or Edge). Does NOT need a built sandvox.exe.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ANA_PORT:-8798}"
SHOT=""
CAP="${ANA_CAP_SECONDS:-300}"

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) echo "check_anatomy: unknown argument '$1'" >&2; exit 2;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_anatomy: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/ana_server.log 2>&1 &
SERVER=$!
CHROME_PID=""
cleanup() {
  [ -n "$CHROME_PID" ] && kill "$CHROME_PID" 2>/dev/null || true
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for OUR server, and confirm it serves THIS checkout (SO_REUSEADDR lets
# another tuner share the port on Windows).
ok=0
for _ in $(seq 1 40); do
  if curl -sf "http://127.0.0.1:$PORT/anatomy_test.html" | grep -q 'anatomy peel harness'; then
    ok=1; break
  fi
  sleep 0.25
done
[ "$ok" = 1 ] || { echo "check_anatomy: server on $PORT is not this checkout" >&2; exit 2; }

URL="http://127.0.0.1:$PORT/anatomy_test.html"
RESULT="$ROOT/build/perfview_result.txt"    # the server's one testresult slot
mkdir -p "$ROOT/build"
rm -f "$RESULT"

"$CHROME" --headless=new --disable-gpu --use-angle=swiftshader-webgl \
          --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run \
          --window-size=1440,1000 "$URL" >/dev/null 2>&1 &
CHROME_PID=$!

OUT=""
for _ in $(seq 1 "$((CAP * 2))"); do
  if [ -f "$RESULT" ] && grep -q '^RESULT ' "$RESULT"; then
    OUT="$(cat "$RESULT")"; break
  fi
  sleep 0.5
done
[ -n "$OUT" ] || OUT="(no verdict within ${CAP}s — the page never reached done(); see /tmp/ana_server.log)"
echo "$OUT"

# --shot: a second, real-time run in shot mode, given a fixed settle time.
if [ -n "$SHOT" ]; then
  kill "$CHROME_PID" 2>/dev/null || true
  CHROME_PID=""
  rm -rf "$PROF"; PROF="$(mktemp -d)"
  timeout 240 "$CHROME" --headless=new --disable-gpu --use-angle=swiftshader-webgl \
          --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run \
          --window-size=1440,1000 --virtual-time-budget=20000 \
          --screenshot="$SHOT" "$URL?shot=1" >/dev/null 2>&1 || true
  echo "check_anatomy: wrote $SHOT"
fi

case "$OUT" in
  *"RESULT OK"*) echo "check_anatomy: PASS"; exit 0;;
  *) echo "check_anatomy: FAIL" >&2; exit 1;;
esac
