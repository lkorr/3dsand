#!/usr/bin/env python3
"""sandvox tuner — native desktop app.

Wraps the same UI and the same endpoints as tuner_server.py in a real window
(WebView2 on Windows) instead of a browser tab: no address bar, no console, no
localhost URL to remember. Double-clicking the packaged exe is the whole
workflow.

    python scripts/tuner_app.py            # run from source
    python scripts/build_tuner_exe.py      # produce dist/sandvox_tuner.exe

The HTTP server still exists underneath, on a random free port bound to
localhost. That is deliberate rather than lazy: the page is the same file
that works in a browser, so there is exactly one UI to maintain, and the
browser path stays available for debugging with devtools.

PACKAGING. When frozen by PyInstaller the assets are not next to this file
any more, so ROOT is resolved differently in that case — see project_root().
The engine (sandvox.exe, the build tree, assets/) is NEVER bundled: this tool
edits a checkout in place, so it has to find the real one on disk.
"""
import os
import socket
import sys
import threading

# Import the server as a module so there is one implementation of the API.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def project_root():
    """Directory holding assets/ and build/.

    Running from source that is the repo root, two levels up from this file.
    Frozen, the exe may sit anywhere, so walk up from it looking for the
    marker directories, and fall back to the cwd. Getting this wrong is the
    single most likely packaging failure, so it fails loudly below rather
    than silently editing nothing.
    """
    if getattr(sys, "frozen", False):
        here = os.path.dirname(os.path.abspath(sys.executable))
        for cand in (here, os.path.dirname(here),
                     os.path.dirname(os.path.dirname(here))):
            if os.path.isdir(os.path.join(cand, "assets", "materials")):
                return cand
        return os.getcwd()
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    root = project_root()
    marker = os.path.join(root, "assets", "materials", "materials.json")
    if not os.path.isfile(marker):
        _fatal("Could not find the sandvox project.\n\n"
               "Looked in:\n  %s\n\n"
               "Put sandvox_tuner.exe in the project root (next to assets/ and\n"
               "build/), or run it from there." % root)
        return 2

    import tuner_server as ts
    # Point the server at the resolved root rather than its own file location,
    # which is wrong once frozen.
    ts.ROOT = root
    ts.ASSETS = os.path.join(root, "assets")
    ts.MATDIR = os.path.join(ts.ASSETS, "materials")
    # REBUILT, NOT PATCHED, and that is a standing hazard: this dict is a second
    # copy of tuner_server.WRITABLE and the two silently drift. `items` was
    # missing here for exactly that reason -- the browser could save items.json
    # and the packaged app could not, with no error either side. Derive the keys
    # from the module rather than retyping them, so a new writable file is added
    # once.
    ts.WRITABLE = {
        "materials": os.path.join(ts.MATDIR, "materials.json"),
        "reactions": os.path.join(ts.MATDIR, "reactions.json"),
        "tuning": os.path.join(ts.MATDIR, "tuning.json"),
        "items": os.path.join(ts.ASSETS, "items", "items.json"),
    }
    ts.EXE = os.path.join(root, "build", "Release", "sandvox.exe")

    port = free_port()
    from http.server import ThreadingHTTPServer
    srv = ThreadingHTTPServer(("127.0.0.1", port), ts.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    import webview
    win = webview.create_window(
        "sandvox tuner",
        "http://127.0.0.1:%d/" % port,
        width=1500, height=1000, min_size=(900, 600),
        background_color="#0e1116",   # matches the page, so no white flash
        text_select=True,
    )

    def on_closing():
        # Killing the game with the editor would be surprising; leave any
        # launched sandvox.exe running and just stop serving.
        srv.shutdown()

    win.events.closing += on_closing
    # gui=None lets pywebview pick the platform default (EdgeChromium here).
    webview.start()
    return 0


def _fatal(msg):
    """Report a startup failure even with no console attached (windowed exe)."""
    sys.stderr.write(msg + "\n")
    try:
        import ctypes
        ctypes.windll.user32.MessageBoxW(None, msg, "sandvox tuner", 0x10)
    except Exception:
        pass


if __name__ == "__main__":
    sys.exit(main() or 0)
