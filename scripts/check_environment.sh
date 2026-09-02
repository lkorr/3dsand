#!/usr/bin/env bash
# check_environment.sh — end-to-end check of the tuner's Environment tab.
#
# WHAT THIS COVERS THAT `node scripts/test_environment.mjs` DOES NOT. The Node
# test asserts the DATA: watergen determinism and preset sanity, every biome
# file valid against the libraries it names, every species file's mirrored
# weights in sync, the swatch composing deterministically. None of that says
# whether the TAB boots inside the tuner, whether the sidebar lists the biome
# files, whether the three page modules build their panels, whether WorldView
# meshes a kettle pond and a forest swatch, whether the plan/profile canvases
# paint, whether the deep links between pages work, or whether the save routes
# accept biomes/ and water/ — and every one of those can break without touching
# a line of the generators.
#
# So this drives the real page in real Chrome against the real server:
# assets/environment_test.html imports the real ES module, gets a WebGL2
# context, hits /api/models and /api/model, reads pixels back, drags a slider,
# clicks the profile editor and round-trips two files. It prints PASS/FAIL
# lines and this reads the verdict out of the dumped DOM.
#
#   bash scripts/check_environment.sh              # verdict only
#   bash scripts/check_environment.sh --shot out.png
#
# NEEDS NO BUILT EXE, like check_trees.sh: nothing in this tab calls the
# engine, and the server is started with no --voxserve behind it.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ENV_PORT:-8799}"
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
[ -n "$CHROME" ] || { echo "check_environment: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/env_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
  # The harness writes scratch files; do not leave them in the tree.
  rm -f "$ROOT/assets/biomes/_harness.json" "$ROOT/assets/water/_harness.json"
}
trap cleanup EXIT

for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
# Confirm it is OUR server: a stale tuner from another worktree on the same
# port answers /api/status and then 404s the harness, which reads as "the page
# did not run" (measured: it did, on the first run of this script). Python's
# HTTPServer sets SO_REUSEADDR, so two CAN bind the same port on Windows.
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/environment_test.html"; then
  echo "check_environment: something else is serving port $PORT (it does not have" >&2
  echo "                   this checkout's assets/environment_test.html). Pass --port." >&2
  exit 2
fi

URL="http://127.0.0.1:$PORT/environment_test.html"

# The biome swatch bakes a great oak on first composition (~4 s of real CPU
# under SwiftShader-less headless), so the budget is generous.
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=300000 --window-size=1400,900)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_environment: wrote $SHOT"
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
  *"RESULT OK"*) echo "check_environment: PASS"; exit 0;;
  *) echo "check_environment: FAIL" >&2; exit 1;;
esac
