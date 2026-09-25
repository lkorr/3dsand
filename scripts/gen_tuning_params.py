#!/usr/bin/env python3
"""Generate assets/tuning_params.js from src/sim/tuning_params.def.

The tuner is a browser page and cannot read src/, so the part of the table it
needs -- each row's type, default and ONE range -- is projected into a
checked-in JS file that tuner.html loads before tuner_schema.js. The schema
keeps only what a person writes (name, description, unit, step, sections);
its rows carry no min/max of their own for a .def key, and a join at the
bottom of tuner_schema.js fills them in from TUNING_PARAMS by group.key. So
the slider range IS the load clamp, and changing one changes both.

Run after editing the .def:

    python scripts/gen_tuning_params.py          # rewrite assets/tuning_params.js
    python scripts/gen_tuning_params.py --check  # exit 1 if it is stale

check_invariants.py runs --check, so a stale file fails the invariant check
rather than shipping a tuner whose ranges disagree with the loader.

(Replaces gen_tuning_prelude.py, whose only job -- a generated copy of the
table in scripts/tuning_prelude.py -- went away when that script started
parsing the .def itself.)
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tuning_def  # noqa: E402

OUT = tuning_def.ROOT / "assets" / "tuning_params.js"

HEADER = """/* GENERATED from src/sim/tuning_params.def by scripts/gen_tuning_params.py
   -- do not edit; edit the .def and re-run the generator.

   One entry per .def row, keyed 'group.member':
     t    'f' | 'i' | 'u' | 'b' | 's' | 'v3'
     def  the default (the same constant tuning.h initializes from)
     min, max  THE range: LoadTuning clamps to it and the tuner's slider spans
               it. Absent = unbounded on that side (TP_OPEN).
     wgsl the shader constant, when the row has one

   tuner_schema.js joins min/max onto its hand-written rows by key. */
"""


def js_num(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    s = repr(float(v))
    if s.endswith(".0"):
        s = s[:-2]
    return s


def render():
    lines = [HEADER, "const TUNING_PARAMS = {"]
    for r in tuning_def.rows():
        parts = ["t:'%s'" % r.kind.lower()]
        if r.kind == "V3":
            parts.append("def:[%s]" % ",".join(js_num(x) for x in r.default))
        elif r.kind == "S":
            parts.append("def:%s" % json.dumps(r.default))
        else:
            parts.append("def:%s" % js_num(r.default))
        if r.lo is not None:
            parts.append("min:%s" % js_num(r.lo))
        if r.hi is not None:
            parts.append("max:%s" % js_num(r.hi))
        if r.has_wgsl:
            parts.append("wgsl:'%s'" % r.wgsl)
        lines.append("  '%s':{%s}," % (r.key, ",".join(parts)))
    lines.append("};")
    lines.append("if (typeof module !== 'undefined') module.exports = { TUNING_PARAMS };")
    return "\n".join(lines) + "\n"


def main():
    try:
        text = render()
    except tuning_def.DefError as e:
        sys.stderr.write("gen_tuning_params: %s\n" % e)
        return 1
    if "--check" in sys.argv:
        cur = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if cur.replace("\r\n", "\n") != text:
            sys.stderr.write(
                "assets/tuning_params.js is stale relative to "
                "src/sim/tuning_params.def -- run "
                "`python scripts/gen_tuning_params.py`\n")
            return 1
        return 0
    OUT.write_text(text, encoding="utf-8", newline="\n")
    n = text.count("\n  '")
    sys.stderr.write("wrote %s (%d rows)\n" % (OUT.name, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
