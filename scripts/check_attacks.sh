#!/usr/bin/env bash
# check_attacks.sh — does the Models tab's animation preview match the GAME,
# and does the Attacks lane drive a real swing?
#
# THE SPLIT WITH `node scripts/test_melee.mjs`. That one covers the PORT:
# melee.js against the melee.cpp it transcribes, anim.js's new stages against
# the invariants anim.cpp states. It is pure arithmetic and it is fast (~1 s),
# so it is the one to run while iterating.
#
# It cannot answer either of the questions this script exists for, and the
# reported bug is the proof: every function in anim.js was individually correct
# while the walk preview showed IK legs and REST ARMS, because nothing in the
# editor ever started a walk clip. A unit test of a function that is never
# called passes. So this drives the real page in real Chrome against the real
# server and MEASURES the posed limbs over two seconds of walking.
#
#   bash scripts/check_attacks.sh              # verdict only
#   bash scripts/check_attacks.sh --shot out.png
#
# Needs Chrome (or Edge). Does NOT need a built sandvox.exe — the harness only
# uses the tuner's static serving plus /api/model and /api/status, so it runs
# on a fresh checkout. It starts and stops its own server on a private port.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ATK_PORT:-8797}"
SHOT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) echo "check_attacks: unknown argument '$1'" >&2; exit 2;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_attacks: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/atk_server.log 2>&1 &
SERVER=$!
cleanup() { kill "$SERVER" 2>/dev/null || true; rm -rf "$PROF"; }
trap cleanup EXIT

# Wait for OUR server, and confirm it serves THIS checkout — a tuner left
# running on the same port from another worktree would otherwise be measured
# instead (SO_REUSEADDR lets a second process share the port on Windows).
ok=0
for _ in $(seq 1 40); do
  if curl -sf "http://127.0.0.1:$PORT/attacks_test.html" | grep -q 'attacks lane + walk preview harness'; then
    ok=1; break
  fi
  sleep 0.25
done
[ "$ok" = 1 ] || { echo "check_attacks: server on $PORT is not this checkout" >&2; exit 2; }

URL="http://127.0.0.1:$PORT/attacks_test.html"

# The harness measures MOTION OVER TIME, so it needs the rAF loop to actually
# run for several seconds. --virtual-time-budget is generous for that reason;
# the harness polls through real fetches (see its own note) so the budget is
# not spent fast-forwarding its own timers.
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=180000 --window-size=1440,1000)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_attacks: wrote $SHOT"
fi

OUT="$("$CHROME" "${COMMON[@]}" --dump-dom "$URL" 2>/dev/null |
  python -c "
import sys, re, html
s = sys.stdin.read()
m = re.search(r'<pre id=\"out\">(.*?)</pre>', s, re.S)
print(html.unescape(m.group(1)) if m else '(no harness output - the page did not run)')
")"
echo "$OUT"
case "$OUT" in
  *"RESULT OK"*) echo "check_attacks: PASS"; exit 0;;
  *) echo "check_attacks: FAIL" >&2; exit 1;;
esac
