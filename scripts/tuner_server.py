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
  GET  /api/models            list .vox/.json under assets/{models,mobs,microvox}
  GET  /api/model?path=...    read one of those files (bytes for .vox)
  POST /api/model?path=...    write one of those files
  GET  /api/shaders           list assets/shaders/*.wgsl with their text
  GET  /api/notes             list note pages under notes/
  GET  /api/note?name=...     read one note page
  POST /api/note?name=...     write one note page (autosave)
  POST /api/note/delete       delete one note page
  POST /api/build             cmake --build ... --target sandvox
  POST /api/play              launch build/Release/sandvox.exe
  GET  /api/status            build state + whether the exe is running

Usage:
    python scripts/tuner_server.py          # serves on 127.0.0.1:8777, opens a browser
    python scripts/tuner_server.py --port N --no-open

SCOPE / SAFETY. This is a developer tool for one machine, not a service:
  - It binds 127.0.0.1 only, so nothing off this box can reach it.
  - The three JSON writable paths are a fixed allowlist (materials/reactions/
    tuning .json under assets/materials). The page cannot name a path, so a bad
    or malicious request cannot write anywhere else.
  - The model routes DO take a path from the request (the editor must be able
    to name the file it is editing), so every one goes through _model_path(),
    which resolves the path and requires the result to sit inside one of three
    fixed directories with an allowed extension. Traversal, absolute paths,
    symlinks and odd extensions are all rejected there.
  - The note routes take a NAME, not a path, and _note_path() rejects anything
    that is not a plain filename of safe characters before joining it to
    notes/. Shaders are read-only and served from a fixed directory listing.
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
import urllib.parse
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

# Directories the editor may read and write models in, as paths RELATIVE to
# assets/. They are resolved against ASSETS at request time rather than frozen
# here, because tuner_app.py re-points the module's ASSETS global after import
# (it runs from a PyInstaller bundle whose __file__ is not the checkout).
MODEL_DIRS = ("models", "mobs", "microvox")
MODEL_EXTS = (".vox", ".json")


def model_dirs():
    """Absolute, normalised model directories for the CURRENT ASSETS value."""
    return [os.path.abspath(os.path.join(ASSETS, d)) for d in MODEL_DIRS]


def _model_path(rel):
    """Resolve a request-supplied model path, or return None if it is not allowed.

    `rel` is relative to assets/, e.g. "models/goblin.vox". Everything about
    this function is the security boundary for the two /api/model routes:
    the path is resolved (so "..", "." and symlinks are collapsed) and the
    result must live directly inside one of MODEL_DIRS with an allowed
    extension. An absolute path in the request cannot escape either, because
    os.path.join() would adopt it and the containment check then fails.
    """
    if not isinstance(rel, str) or not rel:
        return None
    rel = rel.replace("\\", "/")
    if "\x00" in rel:
        return None
    # os.path.join would silently take over on an absolute or drive-qualified
    # path; reject those outright rather than relying on the check below.
    if rel.startswith("/") or os.path.isabs(rel) or (len(rel) > 1 and rel[1] == ":"):
        return None
    path = os.path.realpath(os.path.join(ASSETS, rel))
    if os.path.splitext(path)[1].lower() not in MODEL_EXTS:
        return None
    for d in model_dirs():
        root = os.path.realpath(d)
        if path.startswith(root + os.sep):
            return path
    return None


def shader_dir():
    """assets/shaders for the CURRENT ASSETS value (see model_dirs)."""
    return os.path.abspath(os.path.join(ASSETS, "shaders"))


def notes_dir():
    """Where note pages live: notes/ beside assets/, NOT inside it.

    Deliberately outside assets/ so notes are ordinary project documents —
    openable in Obsidian, diffable in git — rather than engine assets the
    loader might try to parse. Resolved per call for the same reason
    model_dirs() is: tuner_app.py re-points ROOT after import.
    """
    return os.path.abspath(os.path.join(ROOT, "notes"))


# A note is named, not pathed. Anything outside this alphabet cannot name a
# file, so there is no traversal to defend against in the first place.
_NOTE_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
               "0123456789-_. ()")
NOTE_EXT = ".md"


def _note_path(name):
    """Resolve a note NAME to an absolute .md path, or None if not allowed.

    The name is a bare filename with no directory part: the page title as the
    user typed it. Rejecting on the character allowlist (plus the explicit
    dot-segment and length checks) happens BEFORE any join, so unlike the
    model routes this never depends on a containment test to be safe. The
    containment test is still applied afterwards as a second line of defence.
    """
    if not isinstance(name, str):
        return None
    name = name.strip()
    if not name or len(name) > 120:
        return None
    if name.lower().endswith(NOTE_EXT):
        name = name[:-len(NOTE_EXT)]
    if not name or name in (".", "..") or name != name.strip("."):
        return None
    if any(c not in _NOTE_OK for c in name):
        return None
    root = notes_dir()
    path = os.path.abspath(os.path.join(root, name + NOTE_EXT))
    if os.path.dirname(path) != root:
        return None
    return path


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

    def _query(self):
        q = self.path.split("?", 1)
        return urllib.parse.parse_qs(q[1]) if len(q) > 1 else {}

    def _raw(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

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
                 ".css": "text/css",
                 ".vox": "model/x-vox",
                 ".png": "image/png"}.get(ext, "application/octet-stream")
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
        if p == "/api/models":
            # assets/models/ is the editor's own output directory; create it on
            # first listing so a fresh checkout has somewhere to save to.
            try:
                os.makedirs(os.path.join(ASSETS, MODEL_DIRS[0]), exist_ok=True)
            except OSError:
                pass
            files = []
            for d, absdir in zip(MODEL_DIRS, model_dirs()):
                try:
                    names = sorted(os.listdir(absdir))
                except OSError:
                    continue
                for n in names:
                    if os.path.splitext(n)[1].lower() not in MODEL_EXTS:
                        continue
                    full = os.path.join(absdir, n)
                    if not os.path.isfile(full):
                        continue
                    files.append({"path": "%s/%s" % (d, n), "dir": d, "name": n,
                                  "size": os.path.getsize(full),
                                  "mtime": int(os.path.getmtime(full))})
            return self._json(200, {"ok": True, "dirs": list(MODEL_DIRS),
                                    "files": files})

        if p == "/api/model":
            rel = self._query().get("path", [""])[0]
            path = _model_path(rel)
            if not path:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "not found"})
            ctype = ("application/json" if path.lower().endswith(".json")
                     else "model/x-vox")
            with open(path, "rb") as f:
                return self._send(200, f.read(), ctype)

        if p == "/api/shaders":
            # Read-only: the wiki quotes shader source as context, it does not
            # edit it. Sent as one payload because the pages cross-reference
            # every file and the whole set is well under a megabyte.
            out = {}
            d = shader_dir()
            try:
                names = sorted(n for n in os.listdir(d)
                               if n.lower().endswith(".wgsl"))
            except OSError:
                names = []
            for n in names:
                try:
                    with open(os.path.join(d, n), encoding="utf-8") as f:
                        out[n] = f.read()
                except OSError:
                    continue
            return self._json(200, {"ok": True, "shaders": out})

        if p == "/api/notes":
            d = notes_dir()
            try:
                os.makedirs(d, exist_ok=True)
            except OSError:
                pass
            pages = []
            try:
                names = sorted(os.listdir(d))
            except OSError:
                names = []
            for n in names:
                if not n.lower().endswith(NOTE_EXT):
                    continue
                full = os.path.join(d, n)
                if not os.path.isfile(full):
                    continue
                pages.append({"name": n[:-len(NOTE_EXT)],
                              "size": os.path.getsize(full),
                              "mtime": int(os.path.getmtime(full))})
            return self._json(200, {"ok": True, "dir": d, "pages": pages})

        if p == "/api/note":
            name = self._query().get("name", [""])[0]
            path = _note_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "not found"})
            with open(path, encoding="utf-8") as f:
                return self._json(200, {"ok": True, "name": name,
                                        "text": f.read()})

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

        if p == "/api/model":
            rel = self._query().get("path", [""])[0]
            path = _model_path(rel)
            if not path:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            data = self._raw()
            if not data:
                return self._json(400, {"ok": False, "error": "empty body"})
            is_json = path.lower().endswith(".json")
            if is_json:
                # Same rule as /api/save: never write a .json that will not
                # parse, because the engine would then fail to load the asset.
                try:
                    json.loads(data.decode("utf-8"))
                except Exception as e:
                    return self._json(400, {"ok": False,
                                            "error": "not valid JSON: %s" % e})
            else:
                if data[:4] != b"VOX ":
                    return self._json(400, {"ok": False,
                                            "error": "not a .vox file (bad magic)"})
            try:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "path": rel,
                                    "bytes": len(data)})

        if p == "/api/note":
            name = self._query().get("name", [""])[0]
            path = _note_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            body = self._body()
            text = body.get("text")
            if not isinstance(text, str):
                return self._json(400, {"ok": False, "error": "no text"})
            # An old name means this save is also a rename: write the new file
            # first, then drop the old one, so a failure loses nothing.
            old = _note_path(body.get("rename")) if body.get("rename") else None
            try:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                if old and old != path and os.path.isfile(old):
                    os.remove(old)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "name": name,
                                    "bytes": len(text.encode("utf-8")),
                                    "mtime": int(os.path.getmtime(path))})

        if p == "/api/note/delete":
            path = _note_path(self._body().get("name"))
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            try:
                if os.path.isfile(path):
                    os.remove(path)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True})

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
