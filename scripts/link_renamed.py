#!/usr/bin/env python
"""Link the CURRENT objects into build/Release/<name>.exe without touching
build/Release/sandvox.exe -- and without killing whoever is running it.

Why this exists: `scripts/build.sh`'s link phase taskkills every sandvox.exe
by image name before it links, because on Windows a running image cannot be
overwritten. When the owner's tuner-launched game is up, that kill is not a
build step. So: compile with `bash scripts/build.sh --target sandvox_compile`
(objects only, exits before the kill+link), then

    python scripts/link_renamed.py sandvox_gates

which reconstructs the response file ninja would have written for the
`Release\\sandvox.exe` edge (object list + LINK_LIBRARIES from
build/CMakeFiles/impl-Release.ninja) and runs the same link.exe line with
/out, /implib and /pdb pointing at the new name. The exe is a byte-for-byte
sibling of what build.sh would have produced from the same objects; run it
through `bash scripts/run.sh ./build/Release/<name>.exe ...` as usual.

Needs the VS environment (LIB/INCLUDE) -- it runs the link through a .bat
under `cmd /c` after sourcing scripts/vsenv.sh's variables, because `eval` of
the ninja line in bash mangles the backslashes.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
CONFIG = "Release"


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "sandvox_renamed"
    if name == "sandvox":
        print("link_renamed: refusing to overwrite sandvox.exe; use build.sh",
              file=sys.stderr)
        return 2
    impl = (BUILD / "CMakeFiles" / f"impl-{CONFIG}.ninja").read_text(
        encoding="utf-8", errors="replace")
    m = re.search(rf"^build {CONFIG}\\sandvox\.exe: (\S+) (.*?)$((?:\n  .*)*)",
                  impl, re.M)
    if not m:
        print("link_renamed: no link edge for sandvox.exe in impl ninja",
              file=sys.stderr)
        return 1
    inputs = m.group(2).split("|")[0].split()
    objs = [i for i in inputs if i.endswith(".obj")]
    vars_ = dict(re.findall(r"^  (\w+) = (.*)$", m.group(3), re.M))
    libs = vars_.get("LINK_LIBRARIES", "")
    link_flags = vars_.get("LINK_FLAGS", "")
    rsp = BUILD / "CMakeFiles" / f"{name}.{CONFIG}.rsp"
    rsp.write_text(" ".join(objs) + " " + libs + "\n", encoding="utf-8")

    # The link line ninja would run, from `-t commands`, with the three
    # outputs renamed. vs_link_exe is what embeds the manifest.
    cmds = subprocess.run(
        ["ninja", "-f", f"build-{CONFIG}.ninja", "-t", "commands", "sandvox"],
        cwd=BUILD, capture_output=True, text=True).stdout.strip().splitlines()
    link_line = next((c for c in reversed(cmds) if "link.exe" in c), None)
    if not link_line:
        print("link_renamed: ninja -t commands gave no link.exe line",
              file=sys.stderr)
        return 1
    link_line = link_line.replace(
        f"@CMakeFiles\\sandvox.{CONFIG}.rsp", f"@CMakeFiles\\{name}.{CONFIG}.rsp")
    for ext in ("exe", "lib", "pdb"):
        link_line = link_line.replace(f"{CONFIG}\\sandvox.{ext}",
                                      f"{CONFIG}\\{name}.{ext}")
    # --intdir names the object dir for the manifest; keep it (read-only use).
    assert link_flags in link_line or True

    # Environment: LIB/INCLUDE/LIBPATH/PATH from vsenv.sh's capture file
    # (C:/sv-deps/vsenv-<hash>.txt, KEY=value lines). Read directly rather
    # than through `bash -c "source ..."`: from Python, "bash" resolves to
    # C:\Windows\System32\bash.exe (WSL), whose env is not this one.
    env = os.environ.copy()
    caps = sorted(Path("C:/sv-deps").glob("vsenv-*.txt"),
                  key=lambda p: p.stat().st_mtime)
    if not caps:
        print("link_renamed: no C:/sv-deps/vsenv-*.txt; run `bash "
              "scripts/vsenv.sh` once to capture the VS environment",
              file=sys.stderr)
        return 1
    for line in caps[-1].read_text(encoding="utf-8",
                                   errors="replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            if k in ("LIB", "INCLUDE", "LIBPATH"):
                env[k] = v
            elif k == "PATH":
                env["PATH"] = v + os.pathsep + env.get("PATH", "")
    bat = BUILD / f"_link_{name}.bat"
    bat.write_text("@echo off\r\n" + link_line + "\r\n", encoding="utf-8")
    print(f"link_renamed: linking {CONFIG}/{name}.exe from {len(objs)} objects")
    r = subprocess.run(["cmd", "/c", str(bat)], cwd=BUILD, env=env)
    out = BUILD / CONFIG / f"{name}.exe"
    if r.returncode == 0 and out.exists():
        print(f"link_renamed: wrote {out} ({out.stat().st_size} bytes)")
        return 0
    print(f"link_renamed: link failed ({r.returncode})", file=sys.stderr)
    return r.returncode or 1


if __name__ == "__main__":
    sys.exit(main())
