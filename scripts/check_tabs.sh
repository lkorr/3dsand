#!/usr/bin/env bash
# check_tabs.sh — does each tuner tab show ITS panel and nothing else?
#
# WHAT THIS COVERS THAT NOTHING ELSE DOES. Every tab panel in tuner.html is
# styled by the tab's own module, and tuner.html switches between them with
# exactly two rules:
#
#     .view{display:none}          specificity (0,1,0)
#     .view.active{display:block}  specificity (0,2,0)
#
# A module that writes a bare `#view-<name>{ ... display: ... }` wins on ID
# specificity (1,0,0) and pins its own panel visible on EVERY tab. That is a
# whole-app regression authored inside one tab's stylesheet, and it is invisible
# from that tab: the Trees tab looked perfect while the WIKI tab rendered the
# tree editor on top of itself. `check_trees.sh` passed throughout.
#
# Nothing in C++ can see this and no gate can reach it, so the only way to
# answer it is to open the real page in a real browser and read back the
# computed display of every panel after clicking every tab. assets/
# tabs_test.html is that harness; this drives it and reads the verdict.
#
#   bash scripts/check_tabs.sh              # verdict only
#   bash scripts/check_tabs.sh --shot out.png
#
# Needs Chrome (or Edge). Does NOT need a built sandvox.exe — the harness only
# uses the tuner's static file serving and /api/status, so it runs on a fresh
# checkout. It starts and stops its own server on a private port.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${TAB_PORT:-8796}"
SHOT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    *) echo "check_tabs: unknown argument '$1'" >&2; exit 2;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_tabs: no Chrome/Edge found" >&2; exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/tabs_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
}
trap cleanup EXIT

# Wait for OUR server on OUR port, and confirm it is serving THIS checkout's
# assets — a stale tuner left running on the same port from another worktree
# answers /api/status happily and then 404s the harness, which reads as "the
# page did not run" and costs a debugging detour.
for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
if ! curl -sf -o /dev/null "http://127.0.0.1:$PORT/tabs_test.html"; then
  echo "check_tabs: something else is serving port $PORT (it does not have this" >&2
  echo "            checkout's assets/tabs_test.html). Pass --port to pick another." >&2
  exit 2
fi

URL="http://127.0.0.1:$PORT/tabs_test.html"

# --virtual-time-budget is what makes headless wait at all. The harness polls
# through real fetches rather than setTimeout, because virtual time
# fast-forwards timers and would measure the page before the tab modules have
# injected their stylesheets — see the note at its sleep().
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=120000 --window-size=1280,900)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_tabs: wrote $SHOT"
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
  *"RESULT OK"*) echo "check_tabs: PASS"; exit 0;;
  *) echo "check_tabs: FAIL" >&2; exit 1;;
esac
