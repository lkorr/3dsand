#!/usr/bin/env python3
"""Generate scripts/tuning_prelude.py from src/sim/tuning_params.def.

The shader-facing tuning constants used to be listed by hand in two places —
TuningWgslBlock() in sim/tuning.cpp and the SPEC table here — with nothing
comparing their TYPES or their DEFAULTS. check_invariants.py caught a name
present in one and missing from the other, but a parameter declared `f` in C++
and `i` in Python, or defaulted to 1.4 in one and 1.5 in the other, validated
green and then shaded differently in the real build.

Now sim/tuning_params.def is the single table and this script projects it into
the Python the shader checker needs. Run it after editing the .def:

    python scripts/gen_tuning_prelude.py          # rewrite the prelude
    python scripts/gen_tuning_prelude.py --check  # verify it is up to date

--check is what CI/the invariant checker wants: exit 1 if the generated file
has drifted from the table, without touching the tree.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEF = ROOT / "src" / "sim" / "tuning_params.def"
OUT = ROOT / "scripts" / "tuning_prelude.py"

ROW = re.compile(
    r"^TP_(F|I|U|V3)\((\w+),\s*(\w+),\s*(TUNE_[A-Z0-9_]+),\s*(.+)\)\s*$", re.M)
BANNER = re.compile(r"^// ---- (.+?) ----\s*$")


def num(tok):
    """A C++ float/int literal -> a Python literal, dropping the f suffix."""
    tok = tok.strip().rstrip("f")
    return float(tok) if ("." in tok or "e" in tok.lower()) else int(tok)


def parse():
    """(kind, group, member, wgsl_name, default) rows plus their banners."""
    rows = []
    for line in DEF.read_text(encoding="utf-8").splitlines():
        b = BANNER.match(line.strip())
        if b and not line.startswith("//   "):
            rows.append(("BANNER", b.group(1)))
            continue
        m = ROW.match(line)
        if not m:
            continue
        kind, group, member, name, default = m.groups()
        if kind == "V3":
            value = [num(x) for x in default.split(",")]
        else:
            value = num(default)
        rows.append(("ROW", kind.lower(), group, member, name, value))
    return rows


HEADER = '''#!/usr/bin/env python3
"""Emit the WGSL tuning constants, mirroring TuningWgslBlock() in tuning.cpp.

GENERATED FILE -- do not edit. The table lives in src/sim/tuning_params.def
and this file is produced from it by scripts/gen_tuning_prelude.py, which is
also what keeps the names, the TYPES and the DEFAULTS identical to the ones
the engine compiles in. Edit the .def and re-run the generator.

check_shaders.sh has to compile each shader exactly the way LoadShader() does,
and LoadShader prepends this block after the world prelude. Rather than
re-parse JSON in bash, the script shells out here.

Values are read from assets/materials/tuning.json; anything missing falls back
to the same default the C++ struct carries, so the two agree on a partial file.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNING = os.path.join(ROOT, "assets", "materials", "tuning.json")

# (group, key, wgsl_name, kind, default) -- kind is 'f', 'i', 'u' or 'v3'.
SPEC = [
'''

FOOTER = ''']


def fmt(v):
    s = repr(float(v))
    return s if ("." in s or "e" in s) else s + ".0"


def main():
    data = {}
    if os.path.exists(TUNING):
        try:
            with open(TUNING, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as e:  # a broken file must not silently use defaults
            sys.stderr.write("tuning_prelude: cannot parse %s: %s\\n" % (TUNING, e))
            return 1

    out = []
    for group, key, name, kind, default in SPEC:
        v = data.get(group, {}).get(key, default)
        if kind == "f":
            out.append("const %s : f32 = %s;" % (name, fmt(v)))
        elif kind == "i":
            out.append("const %s : i32 = %d;" % (name, int(v)))
        elif kind == "u":
            out.append("const %s : u32 = %du;" % (name, max(0, int(v))))
        else:
            out.append("const %s : vec3f = vec3f(%s, %s, %s);"
                       % (name, fmt(v[0]), fmt(v[1]), fmt(v[2])))
    sys.stdout.write("\\n".join(out) + "\\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''


def render():
    body = []
    for row in parse():
        if row[0] == "BANNER":
            body.append("\n    # %s" % row[1])
            continue
        _, kind, group, member, name, value = row
        body.append('    ("%s", "%s", "%s", "%s", %r),'
                    % (group, member, name, kind, value))
    return HEADER + "\n".join(body).lstrip("\n") + "\n" + FOOTER


def main():
    text = render()
    if "--check" in sys.argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            sys.stderr.write(
                "scripts/tuning_prelude.py is stale relative to "
                "src/sim/tuning_params.def -- run "
                "`python scripts/gen_tuning_prelude.py`\n")
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    n = sum(1 for r in parse() if r[0] == "ROW")
    sys.stderr.write("wrote %s (%d parameters)\n" % (OUT.name, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
