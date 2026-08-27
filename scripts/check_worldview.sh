#!/usr/bin/env bash
# check_worldview.sh — end-to-end check of the tuner's voxel terrain view.
#
# WHAT THIS COVERS THAT `--selftest --gate voxregion` DOES NOT. The gate asserts
# the DATA: that a region dump matches a direct readback, that lod > 1 invents
# no material, that a .svedit lands on the cell index it names. None of that
# says whether the browser half decodes the format, meshes it, uploads it and
# DRAWS it — and every one of those can break without touching a line of C++.
#
# So this drives the real page in real Chrome against the real server: WebGL2
# context, the inline worker, /api/voxregion over HTTP, greedy meshing, LOD
# shells, a pixel readback, a raycast, an edit and an undo. assets/
# worldview_test.html is the harness; it prints PASS/FAIL lines and this reads
# the verdict out of the dumped DOM.
#
#   bash scripts/check_worldview.sh              # verdict only
#   bash scripts/check_worldview.sh --shot out.png [query]
#
# It needs a built sandvox.exe (the server spawns `--voxserve`) and Chrome. It
# starts and stops its own tuner server on a private port, so it does not
# collide with one you have open.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${WV_PORT:-8793}"
SHOT=""
QUERY="levels=2&want=12"

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) QUERY="$1"; shift;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_worldview: no Chrome/Edge found" >&2; exit 2; }
[ -f "$ROOT/build/Release/sandvox.exe" ] || {
  echo "check_worldview: build/Release/sandvox.exe not built" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/wv_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
}
trap cleanup EXIT

# Wait for the port rather than sleeping a guess.
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done

URL="http://127.0.0.1:$PORT/worldview_test.html?$QUERY"

# --virtual-time-budget is what makes a headless screenshot wait at all. The
# harness polls through a real fetch on purpose so the budget is not spent
# fast-forwarding its own timers — see the note at that poll.
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=120000 --window-size=980,620)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_worldview: wrote $SHOT"
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
  *"RESULT OK"*) echo "check_worldview: PASS"; exit 0;;
  *) echo "check_worldview: FAIL" >&2; exit 1;;
esac
