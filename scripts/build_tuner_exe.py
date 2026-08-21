#!/usr/bin/env python3
"""Package the tuner as dist/sandvox_tuner.exe.

    python scripts/build_tuner_exe.py

Produces a single windowed exe (no console) that opens the tuner in a native
WebView2 window. Requires `pip install pywebview pyinstaller` once.

WHAT IS AND IS NOT BUNDLED. Only the Python code goes in. assets/, build/ and
sandvox.exe are deliberately left out: this tool edits a live checkout, so it
must read the real files on disk rather than a frozen snapshot of them —
otherwise saving would write into a temp dir and nothing would change. That is
also why the exe belongs in the project root; tuner_app.project_root() walks up
looking for assets/materials and shows a message box if it cannot find it.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = os.path.join(ROOT, "scripts")


def main():
    try:
        import PyInstaller  # noqa: F401
    except ImportError:
        print("pyinstaller is not installed. Run:\n"
              "    pip install pywebview pyinstaller")
        return 1
    try:
        import webview  # noqa: F401
    except ImportError:
        print("pywebview is not installed. Run:\n"
              "    pip install pywebview pyinstaller")
        return 1

    work = os.path.join(ROOT, "build", "tuner_pkg")
    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconfirm",
        "--onefile",
        "--windowed",                 # no console window
        "--name", "sandvox_tuner",
        "--distpath", os.path.join(ROOT, "dist"),
        "--workpath", work,
        "--specpath", work,
        # tuner_server is imported dynamically by tuner_app; name it so the
        # analyzer keeps it.
        "--hidden-import", "tuner_server",
        "--paths", SCRIPTS,
        os.path.join(SCRIPTS, "tuner_app.py"),
    ]
    print("running:", " ".join(cmd[:6]), "...")
    p = subprocess.run(cmd, cwd=ROOT)
    if p.returncode != 0:
        print("\npyinstaller failed (exit %d)" % p.returncode)
        return p.returncode

    exe = os.path.join(ROOT, "dist", "sandvox_tuner.exe")
    if not os.path.isfile(exe):
        print("build reported success but %s is missing" % exe)
        return 1

    # The exe has to sit beside assets/ to find the project, so put a copy
    # where it will actually be double-clicked.
    dest = os.path.join(ROOT, "sandvox_tuner.exe")
    try:
        shutil.copy2(exe, dest)
    except PermissionError:
        # A RUNNING TUNER HOLDS THIS FILE, and falling back to the dist copy
        # with a friendly note is how the root exe silently stayed months
        # stale: the build "succeeded" every time, so nobody looked. The tool
        # you double-click is the root one, so failing to replace it means the
        # build did not do its job. Fail loudly and say exactly what to do.
        print("\nERROR: %s is in use and could not be replaced." % dest)
        print("       Close the tuner window(s), or run:")
        print("           taskkill //F //IM sandvox_tuner.exe")
        print("       then re-run this script. The fresh build is at:")
        print("           %s" % exe)
        return 1

    print("\nbuilt: %s  (%.1f MB)" % (dest, os.path.getsize(dest) / 1e6))
    print("double-click it from the project root.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
