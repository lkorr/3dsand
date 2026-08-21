#!/usr/bin/env python3
"""Mechanically enforce the "two places that must agree" pairs in this repo.

CLAUDE.md documents four of these. Each was previously enforced by an agent
remembering to read CLAUDE.md before editing, which is not enforcement -- every
one of them is a silent failure: the build stays green, the tuner keeps
rendering, and the wrong thing happens at runtime or the wiki confidently
explains behaviour the shaders do not have.

Checks:

  1. SOUND SLOTS      assets/sound_schema.js  <->  Cues::kSlotPrefix (cues.cpp)
     A slot in one and not the other means the tuner writes a binding the
     engine silently resolves to nothing.

  2. TUNING CONSTANTS sim/tuning_params.def  ->  everything downstream of it
     The TUNE_* set has one source now: that table drives TuningWgslBlock() and
     GENERATES scripts/tuning_prelude.py, so the emitter and the shader-check
     prelude cannot name different sets. What is checked here is that the
     generated file was regenerated, that each row names a member tuning.h
     really declares, and that every TUNE_* a shader references is in the table.

  3. RENDER PATHS     assets/tuner.html RENDER_PATHS  <->  materials.cpp keys
     The wiki re-evaluates the shaders' authored-field tests to say which
     render path a material takes. The flag/field names it reads must be ones
     materials.cpp actually parses.

  4. WORLD CONSTANTS  src/sim/world.h  <->  ShaderConstantPrelude (resources.cpp)
     world.h is the single source of truth; a constant used in WGSL but never
     emitted is a compile error only at pipeline-build time, i.e. at runtime.

Run standalone, or via the PostToolUse hook in .claude/settings.json, which
passes the edited file so only the relevant checks run.

Exit 0 = agree. Exit 1 = a real mismatch.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
problems = []
checked = []


def read(p):
    f = ROOT / p
    return f.read_text(encoding="utf-8", errors="replace") if f.exists() else ""


# ---------------------------------------------------------------- sound slots
def check_sound_slots():
    schema, cues = read("assets/sound_schema.js"), read("src/audio/cues.cpp")
    if not schema or not cues:
        return
    checked.append("sound slots")

    # schema rows look like  {k:'footstep', ... prefix:'footsteps', ...}
    js = {}
    for row in re.finditer(r"\{[^{}]*?k:\s*'([a-z]+)'[^{}]*?\}", schema, re.S):
        pm = re.search(r"prefix:\s*'([a-z/]+)'", row.group(0))
        if pm:
            js[row.group(1)] = pm.group(1)

    cpp = {}
    block = re.search(r"kSlotPrefix\s*=\s*\{(.*?)\n\};", cues, re.S)
    if block:
        for k, v in re.findall(r'\{"([a-z]+)",\s*"([a-z/]+)"\}', block.group(1)):
            cpp[k] = v

    if not js or not cpp:
        problems.append("sound slots: could not parse one of the two tables "
                        "(check the regexes in this script against the files)")
        return

    for k in sorted(set(js) - set(cpp)):
        problems.append(
            f"sound slot '{k}' is in assets/sound_schema.js but NOT in "
            f"Cues::kSlotPrefix (audio/cues.cpp) -- the tuner will offer it and "
            f"the engine will resolve it to nothing")
    for k in sorted(set(cpp) - set(js)):
        problems.append(
            f"sound slot '{k}' is in Cues::kSlotPrefix (audio/cues.cpp) but NOT "
            f"in assets/sound_schema.js -- nothing in the tuner can bind it")
    for k in sorted(set(js) & set(cpp)):
        if js[k] != cpp[k]:
            problems.append(
                f"sound slot '{k}' prefix disagrees: sound_schema.js says "
                f"'{js[k]}', cues.cpp says '{cpp[k]}' -- bindings resolve into "
                f"the wrong namespace")


# ------------------------------------------------------------ tuning TUNE_*
def check_tuning_consts():
    """The TUNE_* set now has ONE source: src/sim/tuning_params.def.

    TuningWgslBlock() expands that table, and scripts/tuning_prelude.py is
    generated from it, so the old emitter-vs-prelude name diff cannot fail by
    construction. What is still worth checking is that the generated file has
    actually been regenerated, that every row declares a member that exists in
    tuning.h, and that no shader references a constant the table never emits.
    """
    table = read("src/sim/tuning_params.def")
    header = read("src/sim/tuning.h")
    if not table or not header:
        return
    checked.append("tuning constants")

    rows = re.findall(
        r"^TP_(F|I|U|V3)\((\w+),\s*(\w+),\s*(TUNE_[A-Z0-9_]+),", table, re.M)
    emitted = {name for _, _, _, name in rows}

    # The generated prelude must be in step with the table it comes from.
    gen = ROOT / "scripts" / "gen_tuning_prelude.py"
    if gen.exists():
        r = subprocess.run([sys.executable, str(gen), "--check"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            problems.append(
                "scripts/tuning_prelude.py is stale relative to "
                "src/sim/tuning_params.def -- run "
                "`python scripts/gen_tuning_prelude.py`")

    # Every row has to name a real member, or TuningWgslBlock will not compile
    # -- but the error lands in a macro expansion, which is a miserable read.
    # Catching it here names the row instead.
    for kind, group, member, name in rows:
        decl = rf"\b{member}\s*(\[3\])?\s*(=|,|;)"
        if not re.search(decl, header):
            problems.append(
                f"{name}: src/sim/tuning_params.def names {group}.{member}, "
                f"which is not declared in src/sim/tuning.h")

    used = set()
    for w in (ROOT / "assets/shaders").glob("*.wgsl"):
        used |= set(re.findall(r"TUNE_[A-Z_0-9]+",
                               w.read_text(encoding="utf-8", errors="replace")))
    for k in sorted(used - emitted):
        problems.append(
            f"{k} is referenced by a shader but is not a row in "
            f"src/sim/tuning_params.def -- the pipeline build will fail at "
            f"runtime, not at compile time")


# ----------------------------------------------------------- RENDER_PATHS
def check_render_paths():
    tuner, materials = read("assets/tuner.html"), read("src/sim/materials.cpp")
    if not tuner or not materials:
        return
    block = re.search(r"const RENDER_PATHS\s*=\s*\[(.*?)\n\];", tuner, re.S)
    if not block:
        return
    checked.append("render paths")

    # Authored material fields the wiki's predicates read off a material.
    fields = set(re.findall(r"\bm\.([a-zA-Z_][a-zA-Z0-9_]*)", block.group(1)))
    # Fields materials.cpp actually parses out of JSON, plus the ones the
    # tuner synthesises itself rather than reading from the file.
    parsed = set(re.findall(r'"([a-zA-Z_][a-zA-Z0-9_]*)"', materials))
    SYNTHETIC = {"class", "name", "id", "flags", "gpu", "tags", "micro"}

    for f in sorted(fields - parsed - SYNTHETIC):
        problems.append(
            f"RENDER_PATHS (assets/tuner.html) reads material field '{f}', "
            f"which sim/materials.cpp never parses -- the wiki's render-path "
            f"table is explaining behaviour from a field that does not exist")


# --------------------------------------------------------- world constants
def check_world_consts():
    prelude = read("src/gpu/resources.cpp")
    if not prelude:
        return
    block = re.search(r"ShaderConstantPrelude[^{]*\{(.*?)\n\}", prelude, re.S)
    if not block:
        return
    checked.append("world constants")

    emitted = set(re.findall(r"\b(?:const|let)\s+([A-Z][A-Z_0-9]*)\b", block.group(1)))
    emitted |= set(re.findall(r'"\s*([A-Z][A-Z_0-9]{2,})\s*(?:=|:)', block.group(1)))
    if not emitted:
        return

    common = read("assets/shaders/common.wgsl")
    # Names common.wgsl uses but never binds itself: candidates for the prelude.
    declared = set(re.findall(r"\b(?:const|let|var)\s+([A-Za-z_][A-Za-z0-9_]*)",
                              common))
    for w in sorted((ROOT / "assets/shaders").glob("*.wgsl")):
        txt = w.read_text(encoding="utf-8", errors="replace")
        for name in re.findall(r"\b(WORLD_[A-Z_0-9]+|CHUNK[A-Z_0-9]*|"
                               r"VOXEL_METERS)\b", txt):
            if name not in emitted and name not in declared:
                problems.append(
                    f"{w.name} uses {name}, which is neither declared in the "
                    f"shader nor emitted by ShaderConstantPrelude "
                    f"(gpu/resources.cpp) -- add it to world.h and the prelude")


ALL = {
    "sound": check_sound_slots,
    "tuning": check_tuning_consts,
    "render": check_render_paths,
    "world": check_world_consts,
}

# The hook passes the edited file; run only the checks that file can break.
RELEVANT = {
    "assets/sound_schema.js": ["sound"],
    "src/audio/cues.cpp": ["sound"],
    "src/sim/tuning.cpp": ["tuning"],
    "src/sim/tuning.h": ["tuning"],
    "src/sim/tuning_params.def": ["tuning"],
    "scripts/tuning_prelude.py": ["tuning"],
    "assets/tuner.html": ["render"],
    "src/sim/materials.cpp": ["render"],
    "src/gpu/resources.cpp": ["world"],
    "src/sim/world.h": ["world"],
}

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    run = list(ALL)
    if args:
        run = []
        for a in args:
            norm = a.replace("\\", "/")
            for key, checks in RELEVANT.items():
                if norm.endswith(key):
                    run += checks
            if norm.endswith(".wgsl"):
                run += ["tuning", "world"]
        run = list(dict.fromkeys(run))
        if not run:
            sys.exit(0)  # edited file cannot break any pair

    for name in run:
        ALL[name]()

    if problems:
        print("invariant check FAILED -- two places that must agree do not:\n",
              file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("\nSee the 'two places that must agree' notes in CLAUDE.md.",
              file=sys.stderr)
        sys.exit(1)

    # Quiet on success when the hook invoked us with a specific file: a line of
    # output after every single edit is noise that trains you to ignore it. A
    # bare run (no args) still confirms it actually looked at something.
    if checked and not args:
        print(f"invariants OK ({', '.join(checked)})")
