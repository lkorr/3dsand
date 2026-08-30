#!/usr/bin/env bash
# check_trees.sh — end-to-end check of the tuner's Trees tab.
#
# WHAT THIS COVERS THAT `node scripts/test_treegen.mjs` DOES NOT. The Node test
# asserts the DATA: that the generator is deterministic, that the .svtree
# encoder and decoder agree cell for cell, that every shipped species is sane
# and that the committed atlases match their .json. None of that says whether
# the TAB boots inside the tuner, whether the module bridge hands it the
# palette, whether WorldView's local-region path uploads and meshes what
# treegen produced, or whether anything reaches the screen — and every one of
# those can break without touching a line of treegen.js.
#
# So this drives the real page in real Chrome against the real server:
# tuner.html in an iframe, the real ES module, a WebGL2 context, /api/models
# and /api/model, a pixel readback, a slider drag and a save round trip.
# assets/trees_test.html is the harness; it prints PASS/FAIL lines and this
# reads the verdict out of the dumped DOM.
#
#   bash scripts/check_trees.sh              # verdict only
#   bash scripts/check_trees.sh --shot out.png
#
# NEEDS NO BUILT EXE, unlike check_worldview.sh — nothing in this tab calls the
# engine. That is a property worth having (the tree editor works on a fresh
# checkout) and this is what asserts it: the server is started with no
# --voxserve behind it and the run still has to pass.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${TREES_PORT:-8795}"
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
[ -n "$CHROME" ] || { echo "check_trees: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/trees_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
  # The harness writes a scratch species; do not leave it in the tree.
  rm -f "$ROOT/assets/trees/_harness.json" "$ROOT/assets/trees/_harness.svtree"
}
trap cleanup EXIT

# Wait for the port rather than sleeping a guess.
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done

URL="http://127.0.0.1:$PORT/trees_test.html"

# --virtual-time-budget is what makes a headless run wait at all. The harness
# polls through a real fetch on purpose so the budget is not spent
# fast-forwarding its own timers — see the note at that poll.
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=180000 --window-size=1320,900)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_trees: wrote $SHOT"
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
  *"RESULT OK"*) echo "check_trees: PASS"; exit 0;;
  *) echo "check_trees: FAIL" >&2; exit 1;;
esac
