#!/usr/bin/env bash
# check_perfview.sh — end-to-end check of the tuner's Performance tab.
#
# WHAT THIS COVERS THAT `--perf` DOES NOT. The harness asserts the NUMBERS: that
# attaching the GPU timer does not move the world hash, that every pass row is
# attributed to a component, that the tree actually burned. It says nothing
# about whether the page parses the JSON it wrote, builds the charts from it,
# and lays them out without collisions — and every one of those breaks without
# touching a line of C++.
#
# So this drives the real tuner in real Chrome against the real server, exactly
# as check_worldview.sh does for the Worldgen tab. assets/perfview_test.html is
# the harness; it prints PASS/FAIL lines and this reads the verdict out of the
# dumped DOM.
#
#   bash scripts/check_perfview.sh              # verdict only
#   bash scripts/check_perfview.sh --shot out.png
#
# It needs build/perf.json (run `sandvox --perf` once) and Chrome. It starts and
# stops its own tuner server on a private port, so it does not collide with one
# you have open. It does NOT need the run mutex: nothing here launches sandvox.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${PV_PORT:-8794}"
SHOT=""
QUERY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --scenario) QUERY="scenario=$2"; shift 2;;
    *) shift;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_perfview: no Chrome/Edge found" >&2; exit 2; }
[ -f "$ROOT/build/perf.json" ] || {
  echo "check_perfview: build/perf.json missing — run:" >&2
  echo "  bash scripts/run.sh ./build/Release/sandvox.exe --perf" >&2
  exit 2; }

PROF="$(mktemp -d)"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/tmp/pv_server.log 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
}
trap cleanup EXIT

for _ in $(seq 1 40); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done

URL="http://127.0.0.1:$PORT/perfview_test.html${QUERY:+?$QUERY}"

# The tab is tall on purpose (it is a dashboard), so the window is tall enough
# that the layout checks measure a real layout rather than a squeezed one.
# --virtual-time-budget is what makes headless wait for the poll loop at all.
COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=45000 --window-size=1440,2400)

if [ -n "$SHOT" ]; then
  "$CHROME" "${COMMON[@]}" --screenshot="$SHOT" "$URL" 2>/dev/null || true
  echo "check_perfview: wrote $SHOT"
fi

OUT="$("$CHROME" "${COMMON[@]}" --dump-dom "$URL" 2>/dev/null |
  python -c "
import sys, re, html
s = sys.stdin.read()
m = re.search(r'<pre id=\"out\">(.*?)</pre>', s, re.S)
t = m.group(1) if m else '(no harness output - the page did not run)'
t = re.sub(r'<[^>]+>', '', t)
print(html.unescape(t))
")"
echo "$OUT"
case "$OUT" in
  *"RESULT OK"*) echo "check_perfview: PASS"; exit 0;;
  *) echo "check_perfview: FAIL" >&2; exit 1;;
esac
