#!/usr/bin/env python3
"""Local dev server for assets/tuner.html.

Opened as a file:// page the tuner is sandboxed: it cannot find the assets
folder on its own (the File System Access API only hands out a directory after
an explicit user gesture), and it certainly cannot build or launch the engine.
Serving it from here lifts all three limits, because the privileged work
happens in this process rather than in the page:

  GET  /                      the tuner, served from assets/
  GET  /api/files             materials.json + reactions.json + tuning.json
  POST /api/save              write those files back
  POST /api/build             cmake --build ... --target sandvox
  POST /api/play              launch build/Release/sandvox.exe
  GET  /api/status            build state + whether the exe is running

Usage:
    python scripts/tuner_server.py          # serves on 127.0.0.1:8777, opens a browser
    python scripts/tuner_server.py --port N --no-open

SCOPE / SAFETY. This is a developer tool for one machine, not a service:
  - It binds 127.0.0.1 only, so nothing off this box can reach it.
  - The three writable paths are a fixed allowlist (materials/reactions/tuning
    .json under assets/materials). The page cannot name a path, so a bad or
    malicious request cannot write anywhere else.
  - /api/build and /api/play run one hardcoded command each with a fixed
    argument list and shell=False. Nothing from the request reaches a command
    line.
Anyone exposing this beyond localhost is opting into arbitrary local builds.
"""
import argparse
import json
import os
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
MATDIR = os.path.join(ASSETS, "materials")

# The only files the page may write, by logical name. Values are absolute paths
# resolved once here, so a request can never introduce a path of its own.
WRITABLE = {
    "materials": os.path.join(MATDIR, "materials.json"),
    "reactions": os.path.join(MATDIR, "reactions.json"),
    "tuning": os.path.join(MATDIR, "tuning.json"),
}

BUILD_CMD = ["cmake", "--build", "build", "--config", "Release",
             "--target", "sandvox"]
EXE = os.path.join(ROOT, "build", "Release", "sandvox.exe")

# Build state, shared with the poller. Guarded because ThreadingHTTPServer
# handles the build POST and the status GETs on different threads.
_lock = threading.Lock()
_build = {"running": False, "ok": None, "log": "", "returncode": None}
_play_procs = []


def _text(s):
    return s if isinstance(s, str) else s.decode("utf-8", "replace")


def run_build():
    """Run the build, capturing output. Errors are reported, never raised."""
    global _build
    with _lock:
        _build = {"running": True, "ok": None, "log": "", "returncode": None}
    try:
        p = subprocess.run(BUILD_CMD, cwd=ROOT, capture_output=True,
                           shell=False, timeout=1800)
        out = _text(p.stdout) + _text(p.stderr)
        # The Dawn build log is enormous; keep the lines that name our own
        # sources plus anything that looks like a diagnostic, so the panel is
        # readable instead of 10k lines of third-party noise.
        keep = []
        for line in out.splitlines():
            low = line.lower()
            if ("error" in low or "warning c" in low
                    or "\\src\\" in low or "/src/" in low
                    or ".vcxproj ->" in low):
                if "_deps" in low or "third_party" in low:
                    continue
                keep.append(line.rstrip())
        tail = keep[-200:] if keep else out.splitlines()[-40:]
        with _lock:
            _build = {"running": False, "ok": p.returncode == 0,
                      "log": "\n".join(tail), "returncode": p.returncode}
    except FileNotFoundError:
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "cmake not found on PATH. Open a shell that has it "
                             "(or the VS developer prompt) and try there."}
    except subprocess.TimeoutExpired:
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "build timed out after 30 minutes"}
    except Exception as e:  # never let a build failure kill the server
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "build failed to start: %r" % (e,)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):  # quiet; the console is for build output
        pass

    # ---- helpers ----
    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        # The tuner edits files; a cached copy is worse than a slow one.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj))

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    def _serve_asset(self, rel):
        # Static files come from assets/ only, and the resolved path must stay
        # inside it — otherwise a crafted URL could walk up into the repo.
        path = os.path.abspath(os.path.join(ASSETS, rel))
        if not path.startswith(ASSETS + os.sep) and path != ASSETS:
            return self._send(403, "forbidden", "text/plain")
        if not os.path.isfile(path):
            return self._send(404, "not found", "text/plain")
        ext = os.path.splitext(path)[1].lower()
        ctype = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".json": "application/json",
                 ".css": "text/css"}.get(ext, "application/octet-stream")
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    # ---- routes ----
    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/":
            return self._serve_asset("tuner.html")
        if p == "/favicon.ico":
            # Browsers request this unprompted; answering keeps a spurious 404
            # out of the page's console.
            return self._send(204, b"", "image/x-icon")
        if p == "/api/files":
            out = {}
            for name, path in WRITABLE.items():
                try:
                    with open(path, encoding="utf-8") as f:
                        out[name] = f.read()
                except FileNotFoundError:
                    out[name] = None
            out["root"] = ROOT
            return self._json(200, out)
        if p == "/api/status":
            with _lock:
                b = dict(_build)
            alive = [q for q in _play_procs if q.poll() is None]
            _play_procs[:] = alive
            b["exeExists"] = os.path.isfile(EXE)
            b["playing"] = len(alive)
            return self._json(200, b)
        return self._serve_asset(p.lstrip("/"))

    def do_POST(self):
        p = self.path.split("?")[0]
        if p == "/api/save":
            body = self._body()
            written = []
            for name, text in (body.get("files") or {}).items():
                path = WRITABLE.get(name)      # allowlist: unknown names ignored
                if not path or not isinstance(text, str):
                    continue
                try:
                    json.loads(text)           # never write a file that won't parse
                except Exception as e:
                    return self._json(400, {"ok": False,
                                            "error": "%s is not valid JSON: %s" % (name, e)})
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
                written.append(name)
            return self._json(200, {"ok": True, "written": written})

        if p == "/api/build":
            with _lock:
                if _build["running"]:
                    return self._json(409, {"ok": False, "error": "build already running"})
            threading.Thread(target=run_build, daemon=True).start()
            return self._json(200, {"ok": True, "started": True})

        if p == "/api/play":
            if not os.path.isfile(EXE):
                return self._json(400, {"ok": False,
                                        "error": "sandvox.exe not found — build first"})
            try:
                q = subprocess.Popen([EXE], cwd=ROOT, shell=False)
                _play_procs.append(q)
                return self._json(200, {"ok": True, "pid": q.pid})
            except Exception as e:
                return self._json(500, {"ok": False, "error": repr(e)})

        if p == "/api/kill":
            n = 0
            for q in _play_procs:
                if q.poll() is None:
                    q.kill()
                    n += 1
            _play_procs.clear()
            return self._json(200, {"ok": True, "killed": n})

        return self._json(404, {"ok": False, "error": "no such endpoint"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    url = "http://127.0.0.1:%d/" % a.port
    print("sandvox tuner -> %s" % url)
    print("   assets: %s" % ASSETS)
    print("   ctrl-c to stop")
    if not a.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    sys.exit(main())
