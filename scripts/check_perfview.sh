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
LIVE=0
FAKE=0
FAKEV1=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shot) SHOT="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --scenario) QUERY="scenario=$2"; shift 2;;
    # --live launches the game with --telemetry and drives the page's own
    # connect path. The one arm check_telemetry.py cannot cover: it proves
    # the server SENDS, and a page that filters every message still shows
    # "waiting for the first frame" forever.
    --live) QUERY="live=1"; LIVE=1; shift;;
    # --fake drives the same live arm against scripts/fake_telemetry.py instead
    # of the game. It needs no GPU, no run mutex and no 20-90 s boot, and it
    # separates the three things --live conflates: producer, transport, page.
    # Anything that fails here is the PAGE's fault.
    --fake) QUERY="live=1"; FAKE=1; shift;;
    # --fake-v1 serves ONLY v1 `stages` messages, reproducing a stale older
    # sandvox still holding port 8080. The page must say so, not sit silent.
    --fake-v1) QUERY="live=1"; FAKE=1; FAKEV1="--v1-only"; shift;;
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
GAME=""
FAKEPID=""
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  [ -n "$GAME" ] && kill "$GAME" 2>/dev/null
  [ -n "$FAKEPID" ] && kill "$FAKEPID" 2>/dev/null
  [ -n "${CHROME_PID:-}" ] && kill "$CHROME_PID" 2>/dev/null
  # Chrome holds files in the throwaway profile for a moment after SIGTERM, and
  # rm's complaints about them are longer than this script's actual output —
  # they buried the verdict the first time. Give it a beat, then discard.
  sleep 1
  rm -rf "$PROF" 2>/dev/null || true
}
trap cleanup EXIT

if [ "$FAKE" = "1" ]; then
  # Refuse rather than silently testing against the wrong peer: fake_telemetry.py does
  # not set SO_REUSEADDR, so if the game (or a stale one) holds 8080 the bind
  # fails loudly instead of two servers sharing the port.
  python "$ROOT/scripts/fake_telemetry.py" --port 8080 ${FAKEV1:+$FAKEV1} \
      >"$ROOT/build/fake_telemetry.log" 2>&1 &
  FAKEPID=$!
  sleep 1.5
  if ! netstat -ano 2>/dev/null | grep -q "127.0.0.1:8080 .*LISTENING"; then
    echo "check_perfview: fake telemetry failed to listen:" >&2
    cat "$ROOT/build/fake_telemetry.log" >&2
    exit 2
  fi
  echo "check_perfview: fake telemetry listening on 8080${FAKEV1:+ (v1 only)}"
fi

if [ "$LIVE" = "1" ]; then
  # The game must already be LISTENING before Chrome opens, or the page spends
  # its whole budget in reconnect backoff. Boot is 20-90 s with a cold SPIR-V
  # cache, so wait for the port rather than sleeping a guess.
  SANDVOX_NO_CRASH_DIALOG=1 "$ROOT/build/Release/sandvox.exe" --telemetry       --frames 2000 --noaudio >"$ROOT/build/telemetry_game.log" 2>&1 &
  GAME=$!
  echo "check_perfview: waiting for the game to listen on 8080..."
  for _ in $(seq 1 240); do
    if netstat -ano 2>/dev/null | grep -q "127.0.0.1:8080 .*LISTENING"; then break; fi
    sleep 0.5
  done
fi

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

# ---- the live/fake arm runs in REAL time ----
# --virtual-time-budget cannot be used here: it does not deliver WebSocket
# events to JS at all. Measured with scripts/fake_telemetry.py — the server log
# showed two clients attached and 600 samples streamed to each, while the page's
# onopen never fired and every probe reported a timeout at 160 ms of page clock.
# So the live arm drops the flag, runs Chrome for real, and waits for the page to
# POST its verdict to /api/testresult.
if [ "$LIVE" = "1" ] || [ "$FAKE" = "1" ]; then
  rm -f "$ROOT/build/perfview_result.txt"
  "$CHROME" --headless=new --disable-gpu --use-angle=swiftshader-webgl \
            --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run \
            --window-size=1440,2400 "$URL" >/dev/null 2>&1 &
  CHROME_PID=$!
  OUT=""
  for _ in $(seq 1 120); do
    if [ -f "$ROOT/build/perfview_result.txt" ]; then
      OUT="$(cat "$ROOT/build/perfview_result.txt")"; break
    fi
    sleep 0.5
  done
  kill "$CHROME_PID" 2>/dev/null || true
  # Stop the game BEFORE reading its log. stdio block-buffers a redirected
  # stdout, so a still-running game can have written nothing to disk yet — the
  # first version of this reported "the game logged NOTHING about telemetry" for
  # a run whose page had just received 33 samples from it.
  if [ -n "$GAME" ]; then kill "$GAME" 2>/dev/null || true; GAME=""; sleep 1; fi
  [ -n "$OUT" ] || OUT="(the page never posted a verdict in 60 s)"
  echo "$OUT"
  if [ "$FAKE" = "1" ] && [ -f "$ROOT/build/fake_telemetry.log" ]; then
    echo "---- peer side (fake_telemetry) ----"
    tail -3 "$ROOT/build/fake_telemetry.log"
  fi
  if [ "$LIVE" = "1" ] && [ -f "$ROOT/build/telemetry_game.log" ]; then
    echo "---- peer side (game) ----"
    grep -i "telemetry" "$ROOT/build/telemetry_game.log" ||
      echo "(the game logged NOTHING about telemetry — did it get --telemetry?)"
  fi
  case "$OUT" in
    *"RESULT OK"*) echo "check_perfview: PASS"; exit 0;;
    *) echo "check_perfview: FAIL" >&2; exit 1;;
  esac
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
