#!/usr/bin/env python3
"""Attach to whatever is listening on the telemetry port and say what it sends.

Throwaway diagnostic. Launches nothing, so it needs no run mutex and no GPU.
Answers the one question a launch-and-assert harness cannot: when the page says
"connected, waiting for the first frame" forever, is the peer silent, is it
sending v1 only, or is it sending v2 that the page then drops?
"""
import base64, json, os, socket, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

try:
    s = socket.create_connection(("127.0.0.1", port), timeout=3.0)
except OSError as e:
    print("CONNECT FAILED:", e)
    sys.exit(1)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(("GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
           "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n" % (port, key)).encode())
s.settimeout(secs)
buf = b""
t0 = time.time()
while b"\r\n\r\n" not in buf and time.time() - t0 < 5:
    try:
        d = s.recv(4096)
    except socket.timeout:
        break
    if not d:
        break
    buf += d
head, _, rest = buf.partition(b"\r\n\r\n")
print("handshake:", head.split(b"\r\n")[0].decode(errors="replace") if head else "(none)")
if b"101" not in head:
    print("NO 101 UPGRADE -- peer is not a websocket server")
    sys.exit(1)

# minimal unmasked-text-frame reader
def frames(sock, rest, deadline):
    buf = rest
    while time.time() < deadline:
        while len(buf) < 2:
            try:
                d = sock.recv(65536)
            except socket.timeout:
                return
            if not d:
                return
            buf += d
        ln = buf[1] & 0x7F
        off = 2
        if ln == 126:
            while len(buf) < 4:
                buf += sock.recv(65536)
            ln = int.from_bytes(buf[2:4], "big"); off = 4
        elif ln == 127:
            while len(buf) < 10:
                buf += sock.recv(65536)
            ln = int.from_bytes(buf[2:10], "big"); off = 10
        while len(buf) < off + ln:
            try:
                d = sock.recv(65536)
            except socket.timeout:
                return
            if not d:
                return
            buf += d
        yield buf[off:off + ln]
        buf = buf[off + ln:]

n = v1 = v2 = 0
first_v2 = None
sample = None
for payload in frames(s, rest, time.time() + secs):
    n += 1
    try:
        m = json.loads(payload)
    except Exception:
        print("  non-JSON frame, %d bytes: %r" % (len(payload), payload[:80]))
        continue
    if m.get("v") == 2:
        v2 += 1
        sample = m
        if first_v2 is None:
            first_v2 = n
    elif "stages" in m:
        v1 += 1
    if n <= 3:
        print("  msg %d keys: %s" % (n, sorted(m.keys())))

print("\nover %.1fs: %d messages, %d v2, %d v1-stages" % (secs, n, v2, v1))
if sample:
    print("last v2: tick=%s frame=%s wallMs=%s gpuValid=%s gpuKeys=%d cpuKeys=%d"
          % (sample.get("tick"), sample.get("frame"), sample.get("wallMs"),
             sample.get("gpuValid"), len(sample.get("gpu") or {}),
             len(sample.get("cpu") or {})))
print("VERDICT:", "peer sends v2" if v2 else
      ("peer sends ONLY v1 -- the page drops every one" if v1 else
       "peer is SILENT -- connected but broadcasting nothing"))
