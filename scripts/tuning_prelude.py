#!/usr/bin/env python3
"""Emit the WGSL tuning constants, mirroring TuningWgslBlock() in tuning.cpp.

check_shaders.sh has to compile each shader exactly the way LoadShader() does,
and LoadShader prepends this block after the world prelude. Rather than
re-parse JSON in bash, the script shells out here.

NOT a table. Until W2-Q (2026-09-24) this file carried a generated copy of
src/sim/tuning_params.def; now it parses the .def directly through
scripts/tuning_def.py, so there is no copy to regenerate or to go stale. Only
rows with a WGSL name are emitted (NO_WGSL rows are CPU knobs), in .def order,
with the same type mapping as TuningWgslBlock.

Values are read from assets/materials/tuning.json and clamped to the row's
range exactly as LoadTuning does; anything missing falls back to the .def
default, which is also the C++ struct's, so the two agree on a partial file.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tuning_def  # noqa: E402


def fmt(v):
    s = repr(float(v))
    return s if ("." in s or "e" in s) else s + ".0"


def main():
    data = {}
    if tuning_def.TUNING_JSON.exists():
        try:
            data = json.loads(tuning_def.TUNING_JSON.read_text(encoding="utf-8"))
        except Exception as e:  # a broken file must not silently use defaults
            sys.stderr.write("tuning_prelude: cannot parse %s: %s\n"
                             % (tuning_def.TUNING_JSON, e))
            return 1
    try:
        rows = tuning_def.rows()
    except tuning_def.DefError as e:
        sys.stderr.write("tuning_prelude: %s\n" % e)
        return 1

    out = []
    for r in rows:
        if not r.has_wgsl or r.kind in ("B", "S"):
            continue
        grp = data.get(r.group, {})
        v = grp.get(r.member, r.default) if isinstance(grp, dict) else r.default
        if r.kind == "F":
            out.append("const %s : f32 = %s;" % (r.wgsl, fmt(tuning_def.clamp(r, v))))
        elif r.kind == "I":
            out.append("const %s : i32 = %d;" % (r.wgsl, int(tuning_def.clamp(r, v))))
        elif r.kind == "U":
            out.append("const %s : u32 = %du;"
                       % (r.wgsl, max(0, int(tuning_def.clamp(r, v)))))
        else:
            out.append("const %s : vec3f = vec3f(%s, %s, %s);"
                       % (r.wgsl, fmt(v[0]), fmt(v[1]), fmt(v[2])))
    sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
