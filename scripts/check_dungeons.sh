#!/usr/bin/env bash
# check_dungeons.sh — the tuner's Dungeons tab end to end in real headless
# Chrome (docs/PLAN_dungeons.md 6, package D4).
#
# WHAT THIS COVERS THAT NOTHING ELSE DOES. The gates (`dungeon-format`,
# `dungeon-gen`) assert the DATA: a shell loads, the GPU carves what the CPU
# twin says. `node scripts/test_dungeongen.mjs` asserts the generator library.
# Neither can see whether the TAB works: that a piece previews through
# /api/dungeon/preview -> voxserve DUNGEON ISOLATE -> /api/voxregion -> the
# worker mesher -> pixels; that a drop onto a socket snaps and links; that a
# grow proposal appears; that undo restores; that Save writes files the
# engine's compiler agrees are current. assets/dungeons_test.html drives the
# real tuner.html for all of it; this script runs it and then checks the
# saved dungeon with `node scripts/compile_dungeon.mjs <id> --check`.
#
#   bash scripts/check_dungeons.sh                     # verdict only
#   bash scripts/check_dungeons.sh --shots build/dg    # + screenshots of both pages
#   bash scripts/check_dungeons.sh --keep              # keep the saved harness dungeon
#
# Needs Chrome (or Edge) and a sandvox.exe for the voxel server: this
# checkout's build/Release/sandvox.exe, or SANDVOX_EXE=<path> (a JS-only
# worktree borrows another checkout's exe; tuner_server.py then points it at
# THIS checkout's assets with SANDVOX_ASSET_DIR). Every voxserve request
# takes the machine-wide GPU lock per request (tuner_server.py RunLock).
# It starts and stops its own tuner server on a private port.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${DG_PORT:-8797}"
SHOTS=""
KEEP=0
ID="_d4_check"

while [ $# -gt 0 ]; do
  case "$1" in
    --shots) SHOTS="$2"; shift 2;;
    --port) PORT="$2"; shift 2;;
    --keep) KEEP=1; shift;;
    *) echo "check_dungeons: unknown argument '$1'" >&2; exit 2;;
  esac
done

CHROME=""
for c in "/c/Program Files/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Google/Chrome/Application/chrome.exe" \
         "/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"; do
  [ -x "$c" ] && { CHROME="$c"; break; }
done
[ -n "$CHROME" ] || { echo "check_dungeons: no Chrome/Edge found" >&2; exit 2; }
EXE="${SANDVOX_EXE:-$ROOT/build/Release/sandvox.exe}"
[ -f "$EXE" ] || { echo "check_dungeons: no sandvox.exe at $EXE (build, or set SANDVOX_EXE)" >&2; exit 2; }
export SANDVOX_NO_CRASH_DIALOG=1

PROF="$(mktemp -d)"
LOG="$ROOT/build/check_dungeons_server.log"
mkdir -p "$ROOT/build"
python "$ROOT/scripts/tuner_server.py" --port "$PORT" --no-open >"$LOG" 2>&1 &
SERVER=$!
cleanup() {
  kill "$SERVER" 2>/dev/null || true
  rm -rf "$PROF"
  if [ "$KEEP" = 0 ]; then rm -rf "$ROOT/assets/dungeons/$ID"; fi
}
trap cleanup EXIT

for _ in $(seq 1 60); do
  curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/status" && break
  sleep 0.25
done
# The voxel server boots on the first request (~3 s + the SPIR-V cache);
# warm it here so the harness's first preview is not the one that pays.
curl -sf -o /dev/null "http://127.0.0.1:$PORT/api/voxpalette" || true

COMMON=(--headless=new --disable-gpu --use-angle=swiftshader-webgl
        --enable-unsafe-swiftshader "--user-data-dir=$PROF" --no-first-run
        --virtual-time-budget=400000 --window-size=1400,900)

URL="http://127.0.0.1:$PORT/dungeons_test.html?id=$ID"
OUT="$("$CHROME" "${COMMON[@]}" --dump-dom "$URL" 2>/dev/null |
  python -c "
import sys, re, html
s = sys.stdin.read()
m = re.search(r'<pre id=\"out\">(.*?)</pre>', s, re.S)
print(html.unescape(m.group(1)) if m else '(no harness output: the page did not run)')
")"
echo "$OUT"

VERDICT=0
case "$OUT" in *"RESULT OK"*) ;; *) VERDICT=1;; esac

# The saved dungeon must be what the compiler script would write.
if [ -f "$ROOT/assets/dungeons/$ID/dungeon.json" ]; then
  if node "$ROOT/scripts/compile_dungeon.mjs" "$ID" --check; then
    echo "PASS  compile_dungeon.mjs $ID --check: the tab's shell.json + shell.bin are current"
  else
    echo "FAIL  compile_dungeon.mjs $ID --check"; VERDICT=1
  fi
  python -c "
import json,sys
j=json.load(open(sys.argv[1],encoding='utf-8'))
ok=j.get('group')=='dungeon/'+sys.argv[2] and all(r['id'].startswith('dungeon/'+sys.argv[2]+'/') for r in j['refs'])
print(('PASS' if ok else 'FAIL')+'  refs.json: group dungeon/'+sys.argv[2]+', '+str(len(j['refs']))+' refs, ids prefixed')
sys.exit(0 if ok else 1)" "$ROOT/assets/dungeons/$ID/refs.json" "$ID" || VERDICT=1
else
  echo "FAIL  the harness saved nothing (assets/dungeons/$ID/dungeon.json missing)"; VERDICT=1
fi

if [ -n "$SHOTS" ]; then
  # Real-time screenshots over the DevTools protocol (scripts/cdp_shot.mjs):
  # Chrome's own --screenshot waits on a virtual clock that a page streaming
  # voxels on requestAnimationFrame never lets run out.
  mkdir -p "$SHOTS"
  READY="(() => { const d = window.__tunerDungeons; const p = d && (d.page() === 'kit' ? d.kit : d.stitch); const v = p && p.view.wv; if (!v || !(p.S ? p.S.prog : p.st.program)) return false; const s = v.stats; if (!(s.live > 0 && s.ready >= s.live && !v.inflight && !v.staleCount() && !s.queued)) { window.__dgStable = 0; return false; } return (window.__dgStable = (window.__dgStable || 0) + 1) >= 4; })()"
  node "$ROOT/scripts/cdp_shot.mjs" "http://127.0.0.1:$PORT/tuner.html?tab=dungeons&dgpage=kit"     --wait "$READY" --shot "$SHOTS/dungeons_kit.png" --timeout 300 || true
  node "$ROOT/scripts/cdp_shot.mjs" "http://127.0.0.1:$PORT/tuner.html?tab=dungeons&dungeon=$ID"     --wait "$READY" --run "(() => { const st = window.__tunerDungeons.stitch; st.select({t: 'piece', id: 'p1'}); st.S.showWay = true; return 1; })()"     --wait "$READY" --shot "$SHOTS/dungeons_stitch.png" --timeout 300 || true
  echo "check_dungeons: screenshots in $SHOTS"
fi

if [ "$VERDICT" = 0 ]; then echo "check_dungeons: PASS"; else echo "check_dungeons: FAIL" >&2; fi
exit $VERDICT
