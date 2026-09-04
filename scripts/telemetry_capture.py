# telemetry_capture.py — passive capture of the live game's --telemetry frames.
# Usage: python scripts/telemetry_capture.py <seconds> <out.jsonl> [port]
# Connects a second WebSocket client to the running sandvox.exe --telemetry
# (port 8080 by default; the tuner's own client is unaffected) and writes one
# PerfSample JSON per line. Zero dependencies. See docs/RESEARCH_streaming_hitch.md §0.
import socket, base64, os, struct, sys, time, json
host, secs, out = "127.0.0.1", float(sys.argv[1]), sys.argv[2]
port = int(sys.argv[3]) if len(sys.argv) > 3 else 8080
s = socket.create_connection((host, port), timeout=5)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
hdr, buf = buf.split(b"\r\n\r\n", 1)
if b"101" not in hdr.split(b"\r\n")[0]: print("handshake failed:", hdr[:200]); sys.exit(1)
s.settimeout(1.0); end = time.time() + secs; n = 0
with open(out, "w") as f:
  while time.time() < end:
    try: chunk = s.recv(65536)
    except socket.timeout: continue
    if not chunk: break
    buf += chunk
    while True:
      if len(buf) < 2: break
      b0, b1 = buf[0], buf[1]; ln = b1 & 0x7f; off = 2
      if ln == 126:
        if len(buf) < 4: break
        ln = struct.unpack(">H", buf[2:4])[0]; off = 4
      elif ln == 127:
        if len(buf) < 10: break
        ln = struct.unpack(">Q", buf[2:10])[0]; off = 10
      if len(buf) < off + ln: break
      payload = buf[off:off+ln]; buf = buf[off+ln:]
      op = b0 & 0xf
      if op == 1:
        f.write(payload.decode("utf-8", "replace") + "\n"); n += 1
print("frames captured:", n)
