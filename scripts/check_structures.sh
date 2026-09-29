#!/usr/bin/env bash
# check_structures.sh — end-to-end check of Environment -> Structures.
#
# WHAT THIS COVERS THAT `node scripts/test_housegen.mjs` DOES NOT. The Node
# gate pins the GENERATOR and the FILES (determinism, slot contracts, the .vox
# round trip, the samples, the materials). None of that says whether the PAGE
# mounts, lists what is on disk with its hand-edited badge, opens a sample's
# .vox, labels every slot, reaches the framebuffer, regenerates on a drag as
# one undo step, saves through the tuner server, or whether the hand-edit
# guard holds in the page AND in the server. assets/structures_test.html does,
# in real headless Chrome against the real server; this reads its verdict.
#
#   bash scripts/check_structures.sh                         # verdict only
#   bash scripts/check_structures.sh --shot out.png [--structure samples/smithy]
#        [--view whole|noroof|ground|upper] [--yaw 0.7] [--pitch -0.45] [--dist 1.0] [--clean]
#                                                            # one framed picture
#
# NEEDS NO BUILT EXE: the page never calls the engine.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${STRUCT_PORT:-8797}"
SHOT=""
QS=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --structure) QS="$QS&shot=$2"; shift 2;;
    --view) QS="$QS&view=$2"; shift 2;;
    --yaw) QS="$QS&yaw=$2"; shift 2;;
    --pitch) QS="$QS&pitch=$2"; shift 2;;
    --dist) QS="$QS&dist=$2"; shift 2;;
    --clean) QS="$QS&labels=0&slots=0&big=1"; shift 1;;
    *) shift;;
  esac
done
if [ -n "$SHOT" ] && [ -z "$(printf '%s' "$QS" | grep -o 'shot=' || true)" ]; then
  QS="$QS&shot=samples/longhouse"
fi

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_structures: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/structures_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
  # The harness writes scratch structures; do not leave them in the tree.
  rm -f "$ROOT"/assets/structures/_harness*.vox "$ROOT"/assets/structures/_harness*.struct.json
}
trap cleanup EXIT

for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
# Confirm it is OUR server (a stale tuner from another worktree can share the
# port on Windows: SO_REUSEADDR).
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/structures_test.html"; then
  echo "check_structures: something else is serving port $PORT (it does not have" >&2
  echo "                  this checkout's assets/structures_test.html). Pass --port." >&2
  exit 2
fi

URL="http://127.0.0.1:$PORT/structures_test.html"
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=180000 --window-size=1300,1000)

if [ -n "$SHOT" ]; then
  # Chrome resolves a relative --screenshot against ITS cwd, not ours.
  case "$SHOT" in /*|?:*) ;; *) SHOT="$PWD/$SHOT";; esac
  command -v cygpath >/dev/null 2>&1 && SHOT="$(cygpath -w "$SHOT")"
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL?${QS#&}" 2>/dev/null || true
  echo "check_structures: wrote $SHOT (${QS#&})"
  exit 0
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
  *"RESULT OK"*) echo "check_structures: PASS"; exit 0;;
  *) echo "check_structures: FAIL" >&2; exit 1;;
esac
