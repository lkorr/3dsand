#!/usr/bin/env bash
# check_characters.sh — end-to-end check of the tuner's Characters tab.
#
# WHAT THIS COVERS THAT `node scripts/test_mobgen.mjs` DOES NOT. The Node gate
# asserts the DATA: that the default genome IS assets/mobs/human.json's rig,
# field for field, that every rolled genome is structurally sound, that mutate and cross
# are reproducible and stay between their parents. None of that says whether the
# TAB boots inside the tuner, whether the bridge hands it the palette, whether
# the software voxel painter puts a single pixel on a canvas, whether a slider
# reaches the preview, or whether the two save buttons produce files the server
# accepts — and every one of those can break without touching a line of
# mobgen.js.
#
# So this drives the real module in real Chrome against the real server:
# assets/characters_test.html is the harness, it prints PASS/FAIL lines, and
# this reads the verdict out of the dumped DOM.
#
#   bash scripts/check_characters.sh              # verdict only
#   bash scripts/check_characters.sh --shot out.png
#
# NEEDS NO BUILT EXE AND NO GPU. Unlike check_worldview.sh, nothing on this page
# calls the engine or opens a WebGL context — the thumbnails are a 2D-canvas
# painter precisely so a litter of thirty does not need thirty GL contexts.
# That is a property worth asserting rather than assuming, and this is what
# asserts it: the run has to pass on a machine with no working GL.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${CHARS_PORT:-8797}"
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
[ -n "$CHROME" ] || { echo "check_characters: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open \
  >/tmp/characters_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
  # The harness SAVES two scratch mobs through the real route (that is the
  # point of it); do not leave them in the tree.
  rm -f "$ROOT/assets/mobs/_harness.vox" "$ROOT/assets/mobs/_harness.json" \
        "$ROOT/assets/mobs/_harness_pale.json"
}
trap cleanup EXIT

# Wait for the port rather than sleeping a guess.
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
# Confirm it is OUR server: a stale tuner from another worktree on the same port
# answers /api/status and then 404s the harness, which reads as "the page did
# not run". Python's HTTPServer sets SO_REUSEADDR, so two CAN bind one port.
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/characters_test.html"; then
  echo "check_characters: something else is serving port $PORT (it does not" >&2
  echo "                  have this checkout's assets/characters_test.html)." >&2
  echo "                  Pass --port." >&2
  exit 2
fi

URL="http://127.0.0.1:$PORT/characters_test.html"

# The harness builds ~25 bodies (a pool, a preview, two litters) plus two
# saves. Each is single-digit milliseconds, but the pool fetches one sidecar per
# mob and the budget has to cover the round trips.
COMMON=(--headless=new --disable-gpu "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=180000 --window-size=1400,900)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_characters: wrote $SHOT"
fi

OUT="$("$CHROME" "${COMMON[@]}" --dump-dom "$URL" 2>/dev/null |
  python -c "
import sys, re
s = sys.stdin.read()
m = re.search(r'<pre id=\"out\">(.*?)</pre>', s, re.S)
print(m.group(1) if m else '(no harness output — the page did not run)')
")"
echo "$OUT"
case "$OUT" in
  *"RESULT OK"*) echo "check_characters: PASS"; exit 0;;
  *) echo "check_characters: FAIL" >&2; exit 1;;
esac
