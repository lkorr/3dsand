#!/usr/bin/env python3
"""check_telemetry.py -- does the live Performance view actually get fed?

The `--perf` harness proves the RECORDED numbers and check_perfview.sh proves the
page draws them. Neither touches the live path, which is a different producer
(main.cpp's frame loop), a different transport (the WebSocket in
src/telemetry.cpp) and a different consumer (perfview.js's pvConnect). All three
can break without failing anything else, and the failure is silent: a page that
sits at "waiting for the first frame" forever.

So this launches the real game with --telemetry, speaks enough WebSocket to
receive, and asserts the v2 samples are actually well-formed:

  - they arrive at all, and at roughly frame rate rather than once;
  - `wallMs` is a plausible frame time;
  - the CPU scope keys are ones perfnodes.h defines;
  - GPU pass times ARRIVE (gpuValid true on most frames) -- the deferred fence
    ring is the part most likely to silently never complete;
  - the v1 `stages` object is still there, so the Engine tab keeps working.

Usage:  bash scripts/run.sh python scripts/check_telemetry.py [--frames 600]

It opens a real window for a few seconds. THE CALLER takes the run mutex, by
being wrapped in scripts/run.sh as above -- this script launches the exe
directly rather than nesting another run.sh inside itself. Nesting was the first
attempt and it does not work: Python resolves `bash` to whichever one is first
on PATH (WSL here, not Git Bash), which cannot read run.sh's CRLF line endings
and reports `set: -: invalid option` for a script that runs fine from a Git Bash
prompt.
"""
import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "sandvox.exe")


def ws_connect(port, timeout=30.0):
    """Minimal RFC6455 client handshake. Enough to receive text frames."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2.0)
        except OSError:
            time.sleep(0.4)
            continue
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET / HTTP/1.1\r\n"
                   "Host: 127.0.0.1:%d\r\n"
                   "Upgrade: websocket\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n" % (port, key)).encode())
        buf = b""
        s.settimeout(5.0)
        try:
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    raise OSError("closed during handshake")
                buf += chunk
        except OSError:
            s.close()
            time.sleep(0.4)
            continue
        if b"101" not in buf.split(b"\r\n")[0]:
            s.close()
            time.sleep(0.4)
            continue
        return s, buf.split(b"\r\n\r\n", 1)[1]
    return None, b""


def ws_frames(sock, rest, want, timeout=25.0):
    """Yield up to `want` text payloads. Server->client frames are unmasked."""
    buf = rest
    sock.settimeout(timeout)
    out = []
    end = time.time() + timeout
    while len(out) < want and time.time() < end:
        while len(buf) < 2:
            buf += sock.recv(65536)
        b1, b2 = buf[0], buf[1]
        opcode = b1 & 0x0F
        masked = b2 & 0x80
        ln = b2 & 0x7F
        off = 2
        if ln == 126:
            while len(buf) < 4:
                buf += sock.recv(65536)
            ln = struct.unpack(">H", buf[2:4])[0]
            off = 4
        elif ln == 127:
            while len(buf) < 10:
                buf += sock.recv(65536)
            ln = struct.unpack(">Q", buf[2:10])[0]
            off = 10
        if masked:
            off += 4
        while len(buf) < off + ln:
            buf += sock.recv(65536)
        payload = buf[off:off + ln]
        buf = buf[off + ln:]
        if opcode == 0x8:      # close
            break
        if opcode == 0x1:
            out.append(payload.decode("utf-8", "replace"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--want", type=int, default=120)
    a = ap.parse_args()

    if not os.path.isfile(EXE):
        print("check_telemetry: build/Release/sandvox.exe not built", file=sys.stderr)
        return 2

    env = dict(os.environ, SANDVOX_NO_CRASH_DIALOG="1")
    # The exe directly; the mutex belongs to the caller (see the module docstring).
    argv = [EXE, "--telemetry", "--telemetry-port", str(a.port),
            "--frames", str(a.frames), "--noaudio"]
    print("launching:", " ".join(argv[1:]))
    proc = subprocess.Popen(argv, cwd=ROOT, env=env)
    try:
        sock, rest = ws_connect(a.port)
        if not sock:
            print("check_telemetry: never connected to ws://127.0.0.1:%d" % a.port,
                  file=sys.stderr)
            return 1
        msgs = ws_frames(sock, rest, a.want)
        sock.close()
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()

    fails = []

    def check(name, cond, detail=""):
        print(("  PASS  " if cond else "  FAIL  ") + name +
              ("   " + detail if detail else ""))
        if not cond:
            fails.append(name)

    check("messages arrive", len(msgs) >= 20, "%d received" % len(msgs))
    if not msgs:
        return 1

    samples = []
    for m in msgs:
        try:
            j = json.loads(m)
        except ValueError:
            continue
        if j.get("v") == 2:
            samples.append(j)

    check("v2 samples arrive", len(samples) >= 20,
          "%d of %d messages" % (len(samples), len(msgs)))
    if not samples:
        return 1

    walls = [s.get("wallMs", 0) for s in samples]
    plausible = [w for w in walls if 0.1 < w < 500]
    check("wallMs is a plausible frame time",
          len(plausible) >= len(walls) * 0.9,
          "median %.2f ms over %d frames" % (sorted(walls)[len(walls)//2], len(walls)))

    # Scope keys must be ones perfnodes.h defines; a typo here shows on the page
    # as a component that silently never appears.
    hdr = open(os.path.join(ROOT, "src", "measure", "perfnodes.h"),
               encoding="utf-8").read()
    known = set()
    blk = hdr.split("kPerfScopeKeys[] = {", 1)
    if len(blk) > 1:
        for tok in blk[1].split("};", 1)[0].split(","):
            tok = tok.strip().strip('"')
            if tok:
                known.add(tok)
    seen = set()
    for s in samples:
        seen |= set((s.get("cpu") or {}).keys())
    check("CPU scope keys are all declared in perfnodes.h",
          bool(seen) and seen <= known,
          "unknown: %s" % sorted(seen - known) if seen - known else
          "%d scopes seen" % len(seen))

    # THE ONE MOST LIKELY TO BREAK SILENTLY. The deferred fence ring is the whole
    # reason live GPU timings are possible; if the maps never retire, every
    # sample is CPU-only and the page's GPU bars are simply absent.
    valid = sum(1 for s in samples if s.get("gpuValid"))
    check("GPU pass times come back through the fence ring",
          valid >= len(samples) * 0.5,
          "%d of %d frames carry GPU data" % (valid, len(samples)))

    gpuKeys = set()
    for s in samples:
        gpuKeys |= set((s.get("gpu") or {}).keys())
    check("GPU time is attributed to named components", len(gpuKeys) >= 2,
          ", ".join(sorted(gpuKeys)) or "none")

    check("the v1 stages object still ships (Engine tab)",
          all("stages" in s for s in samples))

    print("\nRESULT " + ("FAIL (%d)" % len(fails) if fails else "OK"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
