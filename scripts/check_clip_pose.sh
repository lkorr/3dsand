#!/usr/bin/env bash
# check_clip_pose.sh -- the clip lane's pose frames (rig.js section 6c) on the
# REAL tuner page (assets/clip_pose_test.html): every limb handle, mirror,
# flip, copy/paste, frame move/delete, undo, and an additive clip. Needs
# Chrome; no sandvox.exe. Real time (the page POSTs its result), ~40 s.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${CLIP_POSE_PORT:-8797}"
CHROME="/c/Program Files/Google/Chrome/Application/chrome.exe"
[ -x "$CHROME" ] || { echo "check_clip_pose: no Chrome at $CHROME" >&2; exit 2; }
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >/dev/null 2>&1 &
SERVER=$!
for _ in $(seq 1 40); do curl -sf "http://127.0.0.1:$PORT/api/status" >/dev/null && break; sleep 0.25; done
rm -f "$ROOT/build/perfview_result.txt"
PROF="$(mktemp -d)"
timeout 150 "$CHROME" --headless=new --disable-gpu --use-angle=swiftshader-webgl \
  --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run \
  --window-size=1400,900 "http://127.0.0.1:$PORT/clip_pose_test.html?${1:-x=1}" >/dev/null 2>&1 &
CH=$!
cleanup() {
  kill $CH 2>/dev/null
  # kill by PID on the port: pkill -f does not reach it on Windows
  for p in $(netstat -ano | grep LISTENING | grep ":$PORT " | awk '{print $5}' | sort -u); do
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
  kill $SERVER 2>/dev/null
}
trap cleanup EXIT
for _ in $(seq 1 45); do
  sleep 3
  grep -q "DONE\|EXCEPTION" "$ROOT/build/perfview_result.txt" 2>/dev/null && break
done
[ -f "$ROOT/build/perfview_result.txt" ] || { echo "check_clip_pose: the page never reported" >&2; exit 1; }
cat "$ROOT/build/perfview_result.txt"
grep -q "check_clip_pose: PASS" "$ROOT/build/perfview_result.txt"
