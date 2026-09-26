#!/usr/bin/env bash
# trace_attacks.sh -- per-tick smoothness of attack styles on the REAL tuner
# page (assets/attacks_trace.html): hand rotation per tick and blade-tip speed
# and speed change, per style. Needs Chrome; no sandvox.exe.
#
#   bash scripts/trace_attacks.sh                       # the six staples
#   bash scripts/trace_attacks.sh "styles=horizontal_r&verbose=1"
#   bash scripts/trace_attacks.sh "dump=horizontal_r"   # hand path, vs the engine's
#                                                       #   SANDVOX_PS_TRACE=horizontal_r
#
# The page reports by POSTing to the tuner server (build/perfview_result.txt)
# because it runs in REAL time: --virtual-time-budget races its own polling.
# Its own server on a private port; Chrome killed before and after (a stale one
# squats the port and fights the next run).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${TRACE_PORT:-8791}"
Q="${1:-x=1}"
CHROME="/c/Program Files/Google/Chrome/Application/chrome.exe"
[ -x "$CHROME" ] || { echo "trace_attacks: no Chrome at $CHROME" >&2; exit 2; }
taskkill //F //IM chrome.exe >/dev/null 2>&1
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/dev/null 2>&1 &
SERVER=$!
cleanup() { taskkill //F //IM chrome.exe >/dev/null 2>&1; kill "$SERVER" 2>/dev/null; }
trap cleanup EXIT
for _ in $(seq 1 40); do curl -sf "http://127.0.0.1:$PORT/api/status" >/dev/null && break; sleep 0.25; done
rm -f "$ROOT/build/perfview_result.txt"
PROF="$(mktemp -d)"
( timeout 170 "$CHROME" --headless=new --disable-gpu --use-angle=swiftshader-webgl \
    --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run \
    --window-size=900,700 "http://127.0.0.1:$PORT/attacks_trace.html?$Q" >/dev/null 2>&1 & )
for _ in $(seq 1 55); do
  sleep 3
  grep -q "DONE\|EXCEPTION" "$ROOT/build/perfview_result.txt" 2>/dev/null && break
done
[ -f "$ROOT/build/perfview_result.txt" ] || { echo "trace_attacks: the page never reported" >&2; exit 1; }
sed -n '1,/=====DOC=====/p' "$ROOT/build/perfview_result.txt" | grep -v '=====DOC====='
grep -q "DONE" "$ROOT/build/perfview_result.txt"
