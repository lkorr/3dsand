#!/usr/bin/env python3
"""fake_telemetry.py -- a stand-in for the game's live telemetry socket.

WHY THIS EXISTS. The live Performance view has three independently breakable
parts: the producer (main.cpp's frame loop), the transport (src/telemetry.cpp's
WebSocket) and the consumer (perfview.js's pvConnect/onmessage). Testing the
consumer by launching the real game costs a 20-90 s boot, the run mutex and a
GPU, and when it fails it cannot tell you WHICH of the three was at fault.

So this speaks the exact wire format `Telemetry::BroadcastSample` writes --
unmasked 0x81 text frames, one v2 JSON object per frame, zeros dropped, the v1
`stages` object still attached -- and replays a real recorded scenario out of
build/perf.json. Anything the page fails against this is the page's fault, and
it fails in two seconds instead of two minutes.

  python scripts/fake_telemetry.py                 # 60 Hz on :8080 until killed
  python scripts/fake_telemetry.py --port 8081 --hz 120 --scenario treeburn
  python scripts/fake_telemetry.py --v1-only       # reproduce a stale peer that
                                                   # only speaks the Engine tab's
                                                   # v1 `stages` messages
It needs no mutex and no GPU: it launches nothing.
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def load_series(scenario):
    """Replay real recorded numbers rather than invented ones, so a chart that
    looks wrong against this looks wrong for a reason the recorded tab shares."""
    path = os.path.join(ROOT, "build", "perf.json")
    if not os.path.exists(path):
        return None
    j = json.load(open(path))
    runs = [s for s in j["scenarios"] if not s.get("skipped")]
    if not runs:
        return None
    sc = next((s for s in runs if s["id"] == scenario), runs[0])
    return sc["series"]


def sample_json(se, i, v1_only=False):
    """Byte-for-byte the shape of Telemetry::BroadcastSample (src/telemetry.cpp).

    Zeros are DROPPED exactly as the C++ drops them -- a page that only works
    when every key is present would pass a lazier fake and fail the real game.
    """
    n = len(se["wallMs"])
    k = i % n
    wall = se["wallMs"][k]
    stages_src = {name: se["cpu"].get(name, [0] * n)[k]
                  for name in ("stream", "submit", "physics", "post", "render",
                               "readback")}
    stages = {"stream": {"ms": se["cpu"].get("stream", [0] * n)[k]},
              "submit": {"ms": se["cpu"].get("submit", [0] * n)[k]},
              "physics": {"ms": se["cpu"].get("physics", [0] * n)[k]},
              "post": {"ms": se["cpu"].get("postStep", [0] * n)[k]},
              "render": {"ms": se["cpu"].get("renderCpu", [0] * n)[k]},
              "readback": {"ms": se["cpu"].get("readback", [0] * n)[k]}}
    if v1_only:
        # A stale older build: the Engine tab's v1 message and nothing else.
        return json.dumps({"tick": int(se["tick"][k]), "stages": stages})
    m = {"v": 2,
         "tick": int(se["tick"][k]),
         "frame": i,
         "wallMs": round(wall, 3),
         "gpuValid": bool(se.get("gpuValid", [1] * n)[k]),
         "cpu": {a: round(b[k], 3) for a, b in se["cpu"].items() if b[k] > 0},
         "gpu": {a: round(b[k], 3) for a, b in se["gpu"].items() if b[k] > 0},
         "counters": {a: round(b[k]) for a, b in se["counters"].items() if b[k] > 0},
         "stages": stages}
    return json.dumps(m)


def frame(payload):
    """Server->client frames are UNMASKED, per RFC6455 and per telemetry.cpp."""
    d = payload.encode()
    n = len(d)
    if n < 126:
        return bytes([0x81, n]) + d
    if n < 65536:
        return bytes([0x81, 126]) + struct.pack(">H", n) + d
    return bytes([0x81, 127]) + struct.pack(">Q", n) + d


def handshake(conn):
    buf = b""
    conn.settimeout(5.0)
    while b"\r\n\r\n" not in buf:
        d = conn.recv(4096)
        if not d:
            return False
        buf += d
    key = ""
    for line in buf.decode(errors="replace").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    if not key:
        return False
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"
                  % acc).encode())
    return True


def serve_client(conn, addr, se, hz, v1_only, stop):
    if not handshake(conn):
        conn.close()
        return
    print("fake_telemetry: client %s:%d attached" % addr, flush=True)
    conn.settimeout(None)
    conn.setblocking(True)
    i = 0
    dt = 1.0 / hz
    nxt = time.time()
    try:
        while not stop.is_set():
            conn.sendall(frame(sample_json(se, i, v1_only)))
            i += 1
            if i % (hz * 2) == 0:
                print("fake_telemetry: sent %d samples" % i, flush=True)
            nxt += dt
            time.sleep(max(0.0, nxt - time.time()))
    except OSError:
        pass
    finally:
        print("fake_telemetry: client detached after %d samples" % i, flush=True)
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--hz", type=float, default=60.0)
    ap.add_argument("--scenario", default="idle")
    ap.add_argument("--v1-only", action="store_true")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="exit after N seconds (0 = run until killed)")
    a = ap.parse_args()

    se = load_series(a.scenario)
    if not se:
        print("fake_telemetry: need build/perf.json (run sandvox --perf once)",
              file=sys.stderr)
        return 2

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # NOTE: deliberately NOT SO_REUSEADDR. This is a test peer; if something
    # already holds the port we want to hear about it, not silently share it.
    try:
        srv.bind(("127.0.0.1", a.port))
    except OSError as e:
        print("fake_telemetry: bind(:%d) failed -- something already holds it "
              "(%s)" % (a.port, e), file=sys.stderr)
        return 2
    srv.listen(4)
    print("fake_telemetry: ws://127.0.0.1:%d/  %s @ %g Hz%s"
          % (a.port, a.scenario, a.hz, "  (v1 only)" if a.v1_only else ""),
          flush=True)

    stop = threading.Event()
    if a.seconds:
        threading.Timer(a.seconds, stop.set).start()
    srv.settimeout(0.5)
    threads = []
    try:
        while not stop.is_set():
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            t = threading.Thread(target=serve_client,
                                 args=(conn, addr, se, a.hz, a.v1_only, stop),
                                 daemon=True)
            t.start()
            threads.append(t)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        srv.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
