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
     The TUNE_* set has one source: that table drives TuningWgslBlock() and
     scripts/tuning_prelude.py parses it, so the emitter and the shader-check
     prelude cannot name different sets. Checked: the generated
     assets/tuning_params.js is fresh, and every TUNE_* a shader references is
     a WGSL row of the table.

  2b. TUNING REACH    tuning_params.def  <->  tuning.h / tuning.json / schema
     Reads, clamps and defaults are generated from the rows, so what is left
     to check is the joins: each tuning.h member initializes from its OWN row,
     rows are self-consistent, shipped values sit inside their ranges, every
     tuning.json key has a reader, and tuner_schema.js states no second range.

  2c. TUNING USED     tuning_params.def  ->  a reader in src/ or a shader
     The generated read loads every row, so "it is read" says nothing about
     whether the game USES it. Each row must be read by engine code or have
     its TUNE_* in a shader; offline-only readers are an explicit, commented
     allowlist (TUNING_CONSUMER_ALLOWLIST). ~21 dead sliders on 2026-09-24.

  3. RENDER PATHS     assets/tuner.html RENDER_PATHS  <->  materials.cpp keys
     The wiki re-evaluates the shaders' authored-field tests to say which
     render path a material takes. The flag/field names it reads must be ones
     materials.cpp actually parses.

  4. WORLD CONSTANTS  src/sim/world.h  <->  ShaderConstantPrelude (resources.cpp)
     world.h is the single source of truth; a constant used in WGSL but never
     emitted is a compile error only at pipeline-build time, i.e. at runtime.

  5. ARCH MAP         assets/tuner.html ARCH_NODES  <->  the repo tree + kOrder
     The Engine tab's architecture map exists to tell a human or an agent where
     a system lives and how to test it. A stale path or a `--gate` name that no
     longer exists is a confident lie, and nothing else catches it -- the page
     renders exactly as well either way.

  6b. TREE ATLAS      src/sim/treeatlas.h  <->  worldgen.wgsl TA_* offsets
     One buffer, three directories, hand-written word offsets on both sides and
     no generator between them. A field added to the C++ side without the WGSL
     offset makes the shader read the next species' variant pointer.

  6. WORLDGEN MIRROR  assets/shaders/worldgen.wgsl  <->  src/sim/world.cpp
     The terrain math is written twice -- once in WGSL for the GPU, once in C++
     so the CPU can answer "where is the ground". A divergence is a player
     falling through ground they can see, at some seeds, in some places. Both
     sides bracket the shared code with `// MIRROR-BEGIN <tag>` and the token
     streams are compared. See check_worldgen_mirror for what each tag means.

Run standalone, or via the PostToolUse hook in .claude/settings.json, which
passes the edited file so only the relevant checks run.

Exit 0 = agree. Exit 1 = a real mismatch.
"""
import json
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
def _tuning_rows():
    """The parsed .def (scripts/tuning_def.py), or None after reporting why."""
    sys.path.insert(0, str(ROOT / "scripts"))
    try:
        import tuning_def
        return tuning_def.rows()
    except Exception as e:  # DefError, or a missing file
        problems.append(f"src/sim/tuning_params.def: {e}")
        return None


def check_tuning_consts():
    """The TUNE_* set has ONE source: src/sim/tuning_params.def.

    TuningWgslBlock() expands the table and scripts/tuning_prelude.py parses
    it (no generated copy any more), so the emitter and the shader-check
    prelude cannot name different sets. What is checked here is that the
    tuner's generated assets/tuning_params.js is fresh, and that every TUNE_*
    a shader references is a WGSL row of the table.
    """
    if not read("src/sim/tuning_params.def"):
        return
    rows = _tuning_rows()
    if rows is None:
        return
    checked.append("tuning constants")
    emitted = {r.wgsl for r in rows if r.has_wgsl}

    gen = ROOT / "scripts" / "gen_tuning_params.py"
    if gen.exists():
        r = subprocess.run([sys.executable, str(gen), "--check"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            problems.append(
                "assets/tuning_params.js is stale relative to "
                "src/sim/tuning_params.def -- run "
                "`python scripts/gen_tuning_params.py`")

    used = set()
    for w in (ROOT / "assets/shaders").glob("*.wgsl"):
        used |= set(re.findall(r"TUNE_[A-Z_0-9]+",
                               w.read_text(encoding="utf-8", errors="replace")))
    for k in sorted(used - emitted):
        problems.append(
            f"{k} is referenced by a shader but is not a WGSL row in "
            f"src/sim/tuning_params.def -- the pipeline build will fail at "
            f"runtime, not at compile time")


# ------------------------------------------------------- tuning REACH
def _tuning_group_slices(cpp):
    """LoadTuning's hand-written blocks, `if (const json* g = Find(j, "<g>"))`.
    group -> the text from that line to the next top-level group read. Only
    hand reads (keys with no .def row) are looked up here now."""
    marks = [(m.start(), m.group(1)) for m in
             re.finditer(r'Find\(j,\s*"(\w+)"\)', cpp)]
    out = {}
    for i, (pos, g) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(cpp)
        out[g] = out.get(g, "") + cpp[pos:end]
    return out


def _tuning_struct_bodies(header):
    """group -> the body of the nested struct whose instance is `} <group>;`,
    comments stripped (prose like "`waveMode` = 0 ..." must not read as a
    declaration)."""
    body = {}
    lines = header.splitlines()
    for i, ln in enumerate(lines):
        m = re.match(r"^  \}\s*(\w+);", ln)
        if not m:
            continue
        j = i - 1
        while j >= 0 and not re.match(r"^  struct \w+\s*\{", lines[j]):
            j -= 1
        if j >= 0:
            text = "\n".join(lines[j + 1:i])
            text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
            text = re.sub(r"//[^\n]*", "", text)
            body[m.group(1)] = text
    return body


def _schema_rows(schema):
    """(group, key, has_own_min_or_max) for every row of tuner_schema.js.
    min/max are looked for at the row's TOP level only -- a `var:{max:..}`
    popover bound is a different number."""
    out = []
    tab_id = tab_group = None
    for ln in schema.splitlines():
        m = re.match(r"^    id:\s*'(\w+)'", ln)
        if m:
            tab_id, tab_group = m.group(1), None
            continue
        m = re.match(r"^    group:\s*'(\w+)'", ln)
        if m:
            tab_group = m.group(1)
            continue
        m = re.match(r"^\s*\{k:'([\w.]+)'", ln)
        if not m:
            continue
        flat = re.sub(r"'(?:[^'\\]|\\.)*'", "''", ln.strip()[1:])
        gm = re.search(r"\bg:\s*'(\w+)'", ln)
        while re.search(r"\{[^{}]*\}", flat):
            flat = re.sub(r"\{[^{}]*\}", "@", flat)
        own = bool(re.search(r"\b(min|max):\s*-?[0-9.]", flat))
        grp = gm.group(1) if (gm and re.search(r"\bg:\s*''", flat)) else \
            (tab_group or tab_id)
        out.append((grp, m.group(1), own))
    return out


def check_tuning_reach():
    """The .def row is the ONE place a knob's default and range are written.

    Since W2-Q (2026-09-24) LoadTuning's read + clamp, tuning.h's initializer
    and the tuner's min/max are all GENERATED from the row, so the old checks
    ("the .def row is read", "the .def default equals the tuning.h
    initializer") are true by construction. What can still go wrong, and is
    checked here:

      - a tuning.h member not initialized from its own row: `TPD(g, m)` must
        sit on member m of struct g, and every row must have exactly one such
        member (a copy-pasted TPD(player, walkSpeed) on sprintSpeed compiles);
      - a row whose default is outside its own range (the loader would clamp
        the shipped default), or whose min > max, or a bool/string row with a
        WGSL name (TuningWgslBlock would silently emit nothing);
      - a tuning.json value outside its row's range (every load warns);
      - a tuning.json key nothing reads (no row, no hand read) -- the slider
        saves into nothing (render.heatSpillStrength outlived its code);
      - a tuner_schema.js row that states its own min/max for a .def key --
        a second range, which is what this package deleted.
    """
    table = read("src/sim/tuning_params.def")
    header = read("src/sim/tuning.h")
    cpp = read("src/sim/tuning.cpp")
    if not table or not header or not cpp:
        return
    rows = _tuning_rows()
    if rows is None:
        return
    checked.append("tuning reach")
    bodies = _tuning_struct_bodies(header)
    bykey = {}
    for r in rows:
        if r.key in bykey:
            problems.append(f"{r.key}: two rows in src/sim/tuning_params.def "
                            f"(lines {bykey[r.key].line} and {r.line})")
        bykey[r.key] = r

    # 1. tuning.h initializes each member from its own row, once.
    seen = {}
    for group, body in bodies.items():
        for m in re.finditer(r"\b(\w+)\s*(?:\[3\])?\s*=\s*TPD(_V3)?\(\s*(\w+)\s*,"
                             r"\s*(\w+)\s*\)", body):
            member, v3, g2, m2 = m.groups()
            if (g2, m2) != (group, member):
                problems.append(
                    f"src/sim/tuning.h: {group}.{member} is initialized from "
                    f"TPD{v3 or ''}({g2}, {m2}) -- a member must take its OWN "
                    f"row's default")
            seen[f"{group}.{member}"] = seen.get(f"{group}.{member}", 0) + 1
    for r in rows:
        n = seen.get(r.key, 0)
        if n != 1:
            problems.append(
                f"{r.key}: row in src/sim/tuning_params.def but tuning.h "
                f"{'never initializes' if n == 0 else 'initializes twice'} "
                f"`{r.member} = TPD({r.group}, {r.member})` in struct "
                f"`{r.group}`")

    # 2. the row is self-consistent.
    for r in rows:
        if r.kind in ("B", "S") and r.has_wgsl:
            problems.append(f"{r.key}: a TP_{r.kind} row cannot have a WGSL "
                            f"name ({r.wgsl}); TuningWgslBlock emits nothing "
                            f"for it")
        if r.kind not in ("F", "I", "U"):
            continue
        if r.lo is not None and r.hi is not None and r.lo > r.hi:
            problems.append(f"{r.key}: min {r.lo_tok} > max {r.hi_tok}")
        if (r.lo is not None and r.default < r.lo) or \
                (r.hi is not None and r.default > r.hi):
            problems.append(f"{r.key}: default {r.default_tok} is outside its "
                            f"own range [{r.lo_tok}, {r.hi_tok}]")
        if r.kind in ("I", "U"):
            for b, tok in ((r.lo, r.lo_tok), (r.hi, r.hi_tok)):
                if b is not None and b != int(b):
                    problems.append(f"{r.key}: integer row with fractional "
                                    f"bound {tok}")

    # 3. tuning.json: every key has a reader, every row value is in range.
    slices = _tuning_group_slices(cpp)
    try:
        tj = json.loads(read("assets/materials/tuning.json"))
    except Exception:
        tj = None
    if isinstance(tj, dict):
        groups = {r.group for r in rows} | set(slices)
        for group, vals in tj.items():
            if not isinstance(vals, dict) or group not in groups:
                continue
            for k, v in vals.items():
                if k.startswith("_"):
                    continue  # "_comment"-style annotations
                r = bykey.get(f"{group}.{k}")
                if r is None:
                    if f'"{k}"' not in slices.get(group, ""):
                        problems.append(
                            f"assets/materials/tuning.json {group}.{k}: no "
                            f"tuning_params.def row and no hand read in "
                            f"LoadTuning's \"{group}\" group -- an orphan key "
                            f"(delete it, or add the row)")
                    continue
                if r.kind in ("F", "I", "U") and isinstance(v, (int, float)) \
                        and not isinstance(v, bool):
                    # float32, as the loader compares (3.1400001 == 3.14f).
                    import struct
                    f32 = lambda x: struct.unpack("f", struct.pack("f", x))[0]
                    vv = f32(v) if r.kind == "F" else v
                    lo = f32(r.lo) if (r.lo is not None and r.kind == "F") else r.lo
                    hi = f32(r.hi) if (r.hi is not None and r.kind == "F") else r.hi
                    if (lo is not None and vv < lo) or (hi is not None and vv > hi):
                        problems.append(
                            f"assets/materials/tuning.json {group}.{k} = {v} "
                            f"is outside its row's range [{r.lo_tok}, "
                            f"{r.hi_tok}] -- every load clamps it and warns")

    # 4. tuner_schema.js carries no second range for a .def key.
    schema = read("assets/tuner_schema.js")
    for group, k, own in _schema_rows(schema):
        r = bykey.get(f"{group}.{k}")
        if r is not None and own and r.kind in ("F", "I", "U"):
            problems.append(
                f"assets/tuner_schema.js {group}.{k} states its own min/max, "
                f"but the range of a .def key comes from its row "
                f"(tuning_params.js) -- delete the row's min/max")


# ------------------------------------------------------- tuning CONSUMERS
# Rows nothing in the engine reads, but that are still legitimate. Every entry
# says WHO reads it, because "the scan cannot see the reader" is the only
# acceptable reason to be here -- a row the game simply ignores is not
# allowlisted, it is wired or deleted (the 2026-09-24 unused-knob sweep).
#
# A row read ONLY through its generated WGSL constant needs no entry: the check
# looks for the TUNE_* name in assets/shaders/*.wgsl itself. What belongs here
# is a read the scan is blind to: an offline tool reading tuning.json, or a
# TUNE_* consumed only inside a constant the C++ prelude derives from it.
TUNING_CONSUMER_ALLOWLIST = {
    # The FIGURE contract. scripts/test_mobgen.mjs asserts the generated human
    # is halfHeight*2 tall with the eye at halfHeight+eyeOffset. The controller
    # carries the same numbers as Player::kHalfY/kEyeOffset (constexpr, used by
    # ~100 sites incl. collision); player.h static_asserts they equal these
    # rows' defaults so the two cannot drift.
    "player.halfHeight": "scripts/test_mobgen.mjs (figure contract)",
    "player.eyeOffset": "scripts/test_mobgen.mjs (figure contract)",
}


def _strip_cpp(text):
    """Comments and string literals out: a knob named in prose ("the old
    `sunPeakElevation` clamp") or in a warning string is not a reader."""
    # One left-to-right pass, so a `"` inside a comment or a '"' char literal
    # cannot open a phantom string that swallows real code.
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])\'',
                  lambda m: '""' if m.group(0)[0] == '"' else " ", text,
                  flags=re.S)


def check_tuning_consumers():
    """Every .def row drives something.

    The generated read + clamp means a row is ALWAYS loaded, so "the row is
    read" (check 2b) proves nothing about whether the game uses it. On
    2026-09-24 ~25 rows were loaded, clamped, shown as tuner sliders -- and read
    by nothing: sky colours replaced by the scattering model in August, debris
    and grenade constants the tuning pipeline exposed on day one and never
    wired, a sever shove that was never implemented. A slider that moves
    nothing is worse than no slider.

    A row counts as CONSUMED when any of these holds (comments and string
    literals stripped first):
      - its WGSL name appears in an assets/shaders/*.wgsl file;
      - some src/ file other than tuning.cpp (the generated read) and the
        selftest gates reads `<group>.<member>` -- e.g. `tune.grenade.fuse`;
      - some such file reads `.member` / `->member` AND names the group's
        struct (`.group`, `->group` or `Tuning::Group`), which is how aliased
        reads look: `const Tuning::Player& tp = ...; tp.grabDistance`;
      - tuning.h's own inline helpers read it (AddBleedBudget,
        TicksPerDayFromTuning); the struct's `= TPD(...)` initializers do not
        match either pattern, so they are not mistaken for reads.
    src/test/selftest*.cpp does NOT count: a knob only a gate reads is a
    threshold, and thresholds live in tests/baseline.json (CLAUDE.md).
    src/test/support.cpp does count -- it is the shared render plumbing the
    game itself runs.

    Anything else must be in TUNING_CONSUMER_ALLOWLIST with its reader named.
    """
    header = read("src/sim/tuning.h")
    rows = _tuning_rows()
    if rows is None or not header:
        return
    checked.append("tuning consumers")
    # group -> struct type name, from `struct Name { ... } group;` in tuning.h
    types = {}
    lines = header.splitlines()
    for i, ln in enumerate(lines):
        m = re.match(r"^  \}\s*(\w+);", ln)
        if not m:
            continue
        j = i - 1
        while j >= 0 and not re.match(r"^  struct \w+\s*\{", lines[j]):
            j -= 1
        if j >= 0:
            types[m.group(1)] = re.match(r"^  struct (\w+)", lines[j]).group(1)

    skip = {"src/sim/tuning.cpp", "src/sim/tuning_params.def"}
    files = {}
    for p in sorted((ROOT / "src").rglob("*")):
        if p.suffix not in (".cpp", ".h", ".inl"):
            continue
        rel = p.relative_to(ROOT).as_posix()
        if rel in skip or re.match(r"src/test/selftest\w*\.cpp$", rel):
            continue
        files[rel] = _strip_cpp(p.read_text(encoding="utf-8",
                                            errors="replace"))
    member_files, qual = {}, set()
    for rel, t in files.items():
        for m in re.finditer(r"(?:\.|->)\s*([A-Za-z_]\w*)", t):
            member_files.setdefault(m.group(1), set()).add(rel)
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:\.|->)\s*([A-Za-z_]\w*)",
                             t):
            qual.add(m.groups())
    group_files = {}
    for g, ty in types.items():
        pat = re.compile(r"(?:(?:\.|->)\s*" + re.escape(g) + r"|\bTuning::" +
                         re.escape(ty) + r")\b")
        group_files[g] = {rel for rel, t in files.items() if pat.search(t)}
    wgsl = set()
    for p in (ROOT / "assets" / "shaders").glob("*.wgsl"):
        txt = re.sub(r"//[^\n]*", "", p.read_text(encoding="utf-8",
                                                  errors="replace"))
        wgsl |= set(re.findall(r"\bTUNE_\w+", txt))

    keys = {r.key for r in rows}
    for k in TUNING_CONSUMER_ALLOWLIST:
        if k not in keys:
            problems.append(f"scripts/check_invariants.py "
                            f"TUNING_CONSUMER_ALLOWLIST names {k}, which has "
                            f"no tuning_params.def row -- drop the entry")
    for r in rows:
        if r.key in TUNING_CONSUMER_ALLOWLIST:
            continue
        if r.has_wgsl and r.wgsl in wgsl:
            continue
        if (r.group, r.member) in qual:
            continue
        if member_files.get(r.member, set()) & group_files.get(r.group, set()):
            continue
        problems.append(
            f"{r.key}: loaded from tuning.json but nothing reads it -- no "
            f"src/ read of .{r.member} next to the {r.group} group"
            + (f", and no shader uses {r.wgsl}" if r.has_wgsl else "")
            + ". Wire it to the code that does the job, or delete the row "
            f"(.def, tuning.h, tuning.json, tuner_schema.js). An offline "
            f"reader goes in TUNING_CONSUMER_ALLOWLIST with its name.")


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


# ------------------------------------------------------- architecture map
def check_arch_paths():
    """ARCH_NODES `files:` entries  <->  paths that actually exist.

    The Engine tab's architecture map is the thing an AI agent is pointed at to
    learn where a system lives, so a path that has been renamed or deleted is
    worse than no map at all: it sends the next agent to a file that is not
    there, confidently. The map states repo-relative paths and nothing else,
    which makes this mechanically checkable -- so check it.

    A trailing '/' means a directory. Everything else must be a file.
    """
    tuner = read("assets/tuner.html")
    if not tuner:
        return
    block = re.search(r"const ARCH_NODES\s*=\s*\{(.*?)\nconst ARCH_EDGES", tuner, re.S)
    if not block:
        return
    checked.append("arch map paths")

    for arr in re.findall(r"\bfiles:\s*\[(.*?)\]", block.group(1), re.S):
        for path in re.findall(r"'([^']+)'", arr):
            target = ROOT / path
            if path.endswith("/"):
                if not target.is_dir():
                    problems.append(
                        f"ARCH_NODES (assets/tuner.html) points at directory "
                        f"'{path}', which does not exist")
            elif not target.is_file():
                problems.append(
                    f"ARCH_NODES (assets/tuner.html) points at '{path}', which "
                    f"does not exist -- the architecture map is misdirecting "
                    f"whoever reads it next")

    # `tst:` entries are printed as runnable commands, so a name that is not in
    # kOrder is a command that silently runs nothing.
    order = re.search(r"const char\* const kOrder\[\]\s*=\s*\{(.*?)\};",
                      read("src/test/selftest.cpp"), re.S)
    if not order:
        return
    # [a-z0-9-] and not [a-z-]: a gate name may contain a DIGIT, and the
    # narrower class silently dropped such a name from `known` -- which made
    # this check report the opposite of the truth (a gate that IS in kOrder
    # reported as missing from it) the first time one was added. A checker that
    # cries wolf is worse than no checker.
    known = set(re.findall(r'"([a-z0-9-]+)"', order.group(1)))
    cited = set()
    for arr in re.findall(r"\btst:\s*\[(.*?)\]", block.group(1), re.S):
        cited |= set(re.findall(r"'([^']+)'", arr))
    for g in sorted(cited - known):
        problems.append(
            f"ARCH_NODES (assets/tuner.html) offers `--gate {g}`, which is not "
            f"in kOrder (src/test/selftest.cpp) -- that command runs nothing")



# --------------------------------------------------- performance node table
def check_perf_nodes():
    """src/measure/perfnodes.h  <->  ARCH_NODES  and  <->  pass_table.def.

    The Performance tab bills every millisecond of a frame to a component. Three
    lists have to agree for a bar on that page to mean anything:

      1. `node:` in kPerfNodes must be an ARCH_NODES key, or the bar the user
         clicks is not the box they clicked on the Engine map.
      2. every name in `passKeys` must be a real PASS() row, or a component is
         attributed GPU time from a dispatch that does not exist.
      3. every COMPUTE PASS() row must appear in exactly one node's passKeys.

    (3) is the one that matters most and the one nothing else would catch. A
    pass nobody claims is GPU time that silently vanishes from the page: the
    bars still add up to each other, they just stop adding up to the frame. The
    harness counts it as `unattributed` and the page prints a warning, but that
    is a report at runtime -- this is the check that stops it being written.

    Fill/Copy rows (group `nullptr`) are untimed by construction and excluded.
    """
    hdr = read("src/measure/perfnodes.h")
    tuner = read("assets/tuner.html")
    table = read("src/sim/pass_table.def")
    if not (hdr and tuner and table):
        return
    checked.append("perf node table")

    # kPerfNodes rows: {"node", "label", parent, side, scope, "passKeys", ...}
    body = re.search(r"kPerfNodes\[\]\s*=\s*\{(.*?)\n\};", hdr, re.S)
    if not body:
        problems.append("check_perf_nodes: could not find kPerfNodes in "
                        "src/measure/perfnodes.h")
        return
    rows = re.findall(r'\{\s*"([A-Za-z0-9_]+)"\s*,\s*"[^"]*"\s*,'
                      r'\s*(?:nullptr|"[A-Za-z0-9_]+")\s*,'
                      r'\s*PerfSide::\w+\s*,\s*PerfScope::\w+\s*,'
                      r'\s*((?:"[^"]*"\s*)+),', body.group(1), re.S)
    if not rows:
        problems.append("check_perf_nodes: kPerfNodes parsed to zero rows -- "
                        "the row shape changed and this check went blind")
        return

    arch = re.search(r"const ARCH_NODES\s*=\s*\{(.*?)\nconst ARCH_EDGES",
                     tuner, re.S)
    known_nodes = set(re.findall(r"^  ([A-Za-z0-9_]+):\{", arch.group(1), re.M)) \
                  if arch else set()

    # Also collect parents, so a mis-typed parent does not build a broken tree.
    parents = re.findall(r'\{\s*"[A-Za-z0-9_]+"\s*,\s*"[^"]*"\s*,'
                         r'\s*"([A-Za-z0-9_]+)"\s*,\s*PerfSide::', body.group(1))

    for nid in [r[0] for r in rows] + parents:
        if known_nodes and nid not in known_nodes:
            problems.append(
                f"perfnodes.h bills time to '{nid}', which is not an ARCH_NODES "
                f"key in assets/tuner.html -- the Performance tab would show a "
                f"component the Engine map has no box for")

    # Compute rows only: a Fill/Copy row has `nullptr` where the group label goes
    # and is never timed.
    compute = set()
    for name, group in re.findall(r"^PASS\(\s*([A-Za-z0-9_]+)\s*,\s*"
                                  r"(nullptr|\"[^\"]*\")", table, re.M):
        if group != "nullptr":
            compute.add(name)

    claimed = {}
    for nid, keyblob in rows:
        for key in re.findall(r'"([^"]*)"', keyblob):
            for name in [k for k in key.split(";") if k]:
                if name not in compute:
                    problems.append(
                        f"perfnodes.h node '{nid}' claims pass '{name}', which is "
                        f"not a timed PASS() row in src/sim/pass_table.def")
                elif name in claimed:
                    problems.append(
                        f"pass '{name}' is claimed by BOTH '{claimed[name]}' and "
                        f"'{nid}' in perfnodes.h -- its GPU time would be counted "
                        f"twice")
                else:
                    claimed[name] = nid

    for name in sorted(compute - set(claimed)):
        problems.append(
            f"PASS row '{name}' (src/sim/pass_table.def) is claimed by no node in "
            f"src/measure/perfnodes.h -- its GPU time would vanish from the "
            f"Performance tab instead of appearing under some component")


# ------------------------------------------------------ perf scope tooltips
#
# The Performance tab explains every CPU scope on hover, and that prose lives in
# assets/perfview.js (PV_SCOPE_NOTE) rather than in the C++ header, because it
# is page copy: putting it in perfnodes.h would cost a rebuild AND a
# multi-minute `--perf` re-record to fix a typo.
#
# The price of that choice is a second list that can drift from
# `kPerfScopeKeys`, and the drift is silent in both directions -- a new
# PerfScope enumerator gets a bar with no explanation, and a renamed one leaves
# dead prose behind that never appears. So the two are checked here, which is
# what makes the split safe rather than sloppy.
def check_perf_scope_notes():
    hdr = read("src/measure/perfnodes.h")
    js = read("assets/perfview.js")
    if not (hdr and js):
        return
    checked.append("perf scope notes")

    m = re.search(r"kPerfScopeKeys\[\]\s*=\s*\{(.*?)\};", hdr, re.S)
    if not m:
        problems.append("check_perf_scope_notes: could not find kPerfScopeKeys "
                        "in src/measure/perfnodes.h")
        return
    keys = re.findall(r'"([A-Za-z0-9_]+)"', m.group(1))

    j = re.search(r"const PV_SCOPE_NOTE\s*=\s*\{(.*?)\n\};", js, re.S)
    if not j:
        problems.append("check_perf_scope_notes: could not find PV_SCOPE_NOTE "
                        "in assets/perfview.js")
        return
    noted = re.findall(r"^\s{2}([A-Za-z0-9_]+):", j.group(1), re.M)

    for k in keys:
        if k not in noted:
            problems.append(
                f"PerfScope '{k}' (kPerfScopeKeys in src/measure/perfnodes.h) has "
                f"no entry in PV_SCOPE_NOTE (assets/perfview.js) -- its bar on "
                f"the Performance tab would have no hover explanation")
    for k in noted:
        if k not in keys:
            problems.append(
                f"PV_SCOPE_NOTE (assets/perfview.js) explains '{k}', which is not "
                f"a kPerfScopeKeys entry in src/measure/perfnodes.h -- dead prose "
                f"that can never be shown")


# ------------------------------------------------------ CPU/GPU struct pairs
#
# Every struct the CPU fills and a shader reads is declared twice -- once in
# WGSL, once in C++ -- and the two are held in agreement by hand-written pad
# members and a `must match X in Y` comment. Nothing checks them.
#
# The failure is silent in the worst possible way. An under-sized uniform
# binding is not a Vulkan error: the shader just reads past the end of what
# WriteBuffer uploaded, robust buffer access hands back zeros, and the value
# looks like a legitimate default. `poleDir` was deleted from the C++
# RenderParams in ec764e8 while common.wgsl kept declaring and reading it, so
# raymarch.wgsl rotated the star sphere about normalize(vec3f(0)) -- NaN, which
# skyAirglow's max(c, 0.0) turned into pure black. The entire night sky (stars,
# Milky Way, nebulae, aurora) went out for a day with a green build, a green
# selftest, a green vk-validation run and an unmoved world hash.
#
# What is compared is the LAYOUT: the offset and width of every field, and the
# total size. Field NAMES are deliberately not required to match (WGSL BrushOp
# calls its position cx/cy/cz where C++ calls it x/y/z), but a name that DOES
# appear on both sides has to sit at the same offset, which is what catches a
# reordering that happens to preserve the shape.

# WGSL type -> (scalars, alignment in scalars). std140/std430 agree for
# everything here, since no field is larger than a vec4.
_WGSL_TYPES = {
    "f32": (1, 1), "u32": (1, 1), "i32": (1, 1), "bool": (1, 1),
    "vec2f": (2, 2), "vec2u": (2, 2), "vec2i": (2, 2),
    "vec2<f32>": (2, 2), "vec2<u32>": (2, 2), "vec2<i32>": (2, 2),
    "vec3f": (3, 4), "vec3u": (3, 4), "vec3i": (3, 4),
    "vec3<f32>": (3, 4), "vec3<u32>": (3, 4), "vec3<i32>": (3, 4),
    "vec4f": (4, 4), "vec4u": (4, 4), "vec4i": (4, 4),
    "vec4<f32>": (4, 4), "vec4<u32>": (4, 4), "vec4<i32>": (4, 4),
    "mat4x4f": (16, 4), "mat4x4<f32>": (16, 4),
}
_CPP_SCALARS = {"float", "uint32_t", "int32_t", "int", "unsigned"}
# A pad exists only to hold a slot; the two sides name theirs differently
# (_p0/_pdn1 vs pad0/pad_dn1) and neither name means anything.
_PAD = re.compile(r"^_?p(ad)?[_a-z]*\d*$", re.I)


def _strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def _split_top(s, seps=",", opens="<([{", closes=">)]}"):
    """Split on `seps` at bracket depth 0 (array<vec4<i32>, N> is ONE field)."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in opens:
            depth += 1
        elif ch in closes:
            depth -= 1
        if ch in seps and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return [p.strip() for p in out if p.strip()]


def _struct_body(text, name, keyword="struct"):
    m = re.search(r"\b%s\s+%s\s*\{" % (keyword, re.escape(name)), text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(text) and depth:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[m.end():i - 1]


def _wgsl_fields(body, consts):
    """[(name, scalars, align)] or None if a type could not be resolved."""
    out = []
    for f in _split_top(_strip_comments(body)):
        if ":" not in f:
            return None
        name, ty = (p.strip() for p in f.split(":", 1))
        arr = re.match(r"array<\s*(.+?)\s*,\s*(\w+)\s*>$", ty)
        if arr:
            base, count = _WGSL_TYPES.get(arr.group(1)), consts.get(arr.group(2))
            if count is None and arr.group(2).rstrip("u").isdigit():
                count = int(arr.group(2).rstrip("u"))
            if not base or count is None:
                return None
            stride = max(base[1], 4)  # uniform arrays stride to 16 bytes
            out.append((name, stride * count, stride))
            continue
        if ty not in _WGSL_TYPES:
            return None
        out.append((name,) + _WGSL_TYPES[ty])
    return out


def _cpp_fields(body, consts):
    out = []
    for stmt in _split_top(_strip_comments(body), seps=";"):
        m = re.match(r"(?:const\s+)?(\w+)\s+(.*)$", stmt, re.S)
        if not m or m.group(1) not in _CPP_SCALARS:
            if re.match(r"(static_assert|constexpr|using|typedef|\w+\s*\()",
                        stmt.strip()):
                continue
            return None
        for decl in _split_top(m.group(2)):
            decl = decl.split("=")[0].strip().lstrip("*&")
            dm = re.match(r"(\w+)((?:\[\w+\])*)$", decl)
            if not dm:
                return None
            n = 1
            for dim in re.findall(r"\[(\w+)\]", dm.group(2)):
                v = int(dim) if dim.isdigit() else consts.get(dim)
                if v is None:
                    return None
                n *= v
            # Plain C packing: every member here is a 4-byte scalar or an array
            # of them, so alignment is 1 scalar and the hand-written pads are
            # what make the result match std140.
            out.append((dm.group(1), n, 1))
    return out


def _layout(fields):
    """[(name, offset, scalars)], total size in scalars (rounded to a row)."""
    off, out = 0, []
    for name, size, align in fields:
        off = (off + align - 1) // align * align
        out.append((name, off, size))
        off += size
    return out, (off + 3) // 4 * 4


def check_gpu_structs():
    world = read("src/sim/world.h")
    if not world:
        return
    # Array dimensions on the C++ side are named constants from world.h.
    consts = {k: int(v) for k, v in
              re.findall(r"constexpr\s+\w+\s+(\w+)\s*=\s*(\d+)\s*;", world)}
    shaders = {p.name: _strip_comments(p.read_text(encoding="utf-8",
                                                   errors="replace"))
               for p in sorted((ROOT / "assets/shaders").glob("*.wgsl"))}
    cpp_names = set(re.findall(r"^struct\s+(\w+)\s*\{", world, re.M))

    compared = 0
    for fname, text in shaders.items():
        for sname in re.findall(r"^struct\s+(\w+)\s*\{", text, re.M):
            if sname not in cpp_names:
                continue
            wf = _wgsl_fields(_struct_body(text, sname) or "", consts)
            cf = _cpp_fields(_struct_body(world, sname) or "", consts)
            if wf is None or cf is None:
                continue  # a type this parser does not model; not a mismatch
            wl, wsize = _layout(wf)
            cl, csize = _layout(cf)
            compared += 1
            where = f"{sname} (assets/shaders/{fname} <-> src/sim/world.h)"
            if wsize != csize:
                lack = "C++ struct is SHORTER" if csize < wsize else \
                       "C++ struct is LONGER"
                missing = [n for n, _, _ in wl] if csize < wsize else \
                          [n for n, _, _ in cl]
                problems.append(
                    f"{where}: {wsize * 4} bytes in WGSL, {csize * 4} in C++ -- "
                    f"the {lack}, so the shader reads past what WriteBuffer "
                    f"uploads and gets ZEROS, not an error. Fields: "
                    f"{', '.join(missing[-4:])}")
                continue
            shape_w = [(o, s) for _, o, s in wl]
            shape_c = [(o, s) for _, o, s in cl]
            if shape_w != shape_c:
                for i, (a, b) in enumerate(zip(shape_w, shape_c)):
                    if a != b:
                        problems.append(
                            f"{where}: field {i} is '{wl[i][0]}' at scalar "
                            f"offset {a[0]} width {a[1]} in WGSL, but "
                            f"'{cl[i][0]}' at offset {b[0]} width {b[1]} in C++")
                        break
                continue
            byname = {n: o for n, o, _ in cl if not _PAD.match(n)}
            for n, o, _ in wl:
                if not _PAD.match(n) and n in byname and byname[n] != o:
                    problems.append(
                        f"{where}: '{n}' is at scalar offset {o} in WGSL but "
                        f"{byname[n]} in C++ -- the shapes match, so the two "
                        f"structs have been reordered against each other")
    if compared:
        checked.append(f"CPU/GPU struct layouts ({compared})")


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


# ------------------------------------------------------------ fluid substeps
def check_fluid_substeps():
    """world.h's kFluidSubsteps must equal the sim.fluidSubsteps .def default.

    The substep count is a tuning knob, but world.h still carries the fallback
    constant and every comment that reasons about the CFL cap (VMAX =
    0.45*substeps cells/tick, the block-map pad, the ~8/12 m/s terminal speed)
    is written against it. A .def default that drifts from world.h would make
    those comments quietly wrong and, worse, make a fresh tuning.json disagree
    with the C++ fallback about how many times EncodeTick records the substep
    table.
    """
    wh = read("src/sim/world.h")
    dfn = read("src/sim/tuning_params.def")
    if not wh or not dfn:
        return
    m = re.search(r"constexpr\s+uint32_t\s+kFluidSubsteps\s*=\s*(\d+)", wh)
    d = re.search(r"TP_I\(sim,\s*fluidSubsteps,\s*TUNE_FLUID_SUBSTEPS,\s*(\d+)\s*[,)]",
                  dfn)
    if not m or not d:
        return
    checked.append("fluid substeps")
    if m.group(1) != d.group(1):
        problems.append(
            f"world.h kFluidSubsteps = {m.group(1)} but tuning_params.def "
            f"sim.fluidSubsteps defaults to {d.group(1)} -- the fallback and "
            f"the knob's default must agree")


# ------------------------------------------------------ wind primitive layout
def check_wind_prims():
    """world.h's wind primitive ceilings must match common.wgsl's constants.

    The layout itself is covered by check_gpu_structs (a mismatched cap makes
    TickParams a different size and fails there). This catches the OTHER half:
    the shader loops `i < min(count, WIND_PRIM_CAP)` and strides by
    WIND_PRIM_ROWS, so a cap raised in world.h alone would leave the extra
    primitives silently unread -- a fan that exists and does nothing, with a
    green build, a green selftest and an unmoved hash.
    """
    wh = read("src/sim/world.h")
    cw = read("assets/shaders/common.wgsl")
    if not wh or not cw:
        return
    pairs = [("kWindPrimCap", "WIND_PRIM_CAP"),
             ("kWindPrimWords", None)]
    m = re.search(r"constexpr\s+uint32_t\s+kWindPrimCap\s*=\s*(\d+)", wh)
    g = re.search(r"const\s+WIND_PRIM_CAP\s*:\s*u32\s*=\s*(\d+)u", cw)
    r = re.search(r"const\s+WIND_PRIM_ROWS\s*:\s*u32\s*=\s*(\d+)u", cw)
    w = re.search(r"constexpr\s+uint32_t\s+kWindPrimWords\s*=\s*(\d+)", wh)
    if not (m and g and r and w):
        return
    checked.append("wind primitives")
    if m.group(1) != g.group(1):
        problems.append(
            f"world.h kWindPrimCap = {m.group(1)} but common.wgsl "
            f"WIND_PRIM_CAP = {g.group(1)} -- the shader would read a "
            f"different number of primitives than the CPU uploads")
    if int(r.group(1)) * 4 != int(w.group(1)):
        problems.append(
            f"common.wgsl WIND_PRIM_ROWS = {r.group(1)} (x4 scalars per row) "
            f"but world.h kWindPrimWords = {w.group(1)} -- the shader would "
            f"stride past the primitive it is decoding")


def check_water_ledger():
    """The water-body ledger's word map lives in THREE places, positionally.

    world.h's kWaterBodyStateWords is the STRIDE -- every read of the buffer is
    `slot * stride + field`. common.wgsl's WBS_* block is the field map the
    shaders index with. selftest_water.cpp re-declares the same map as a bare
    `enum : uint32_t` with ONE initialiser, so every name after the first takes
    its index from its POSITION IN THE LIST.

    Nothing about that is checked by a compiler. A word added to two of the
    three is a gate that reads the wrong field of the right body (silent: it
    prints a plausible number), or a shader that writes one body's word into
    the next body's block (silent until a conservation sum drifts). W1 added
    twelve words at once, which is exactly the size of change that gets two of
    three right.

    Also checked: the relevel histogram's own arithmetic, because it is sized
    from one constant and indexed from another.
    """
    wh = read("src/sim/world.h")
    cw = read("assets/shaders/common.wgsl")
    st = read("src/test/selftest_water.cpp")
    if not wh or not cw or not st:
        return
    m = re.search(r"constexpr\s+uint32_t\s+kWaterBodyStateWords\s*=\s*(\d+)", wh)
    if not m:
        return
    stride = int(m.group(1))
    checked.append("water ledger words")

    # common.wgsl: WBS_<NAME> : u32 = <n>u
    wgsl = {}
    for name, idx in re.findall(r"\bconst\s+WBS_(\w+)\s*:\s*u32\s*=\s*(\d+)u", cw):
        wgsl[name] = int(idx)
    if wgsl:
        seen = sorted(wgsl.values())
        if seen != list(range(len(seen))):
            problems.append(
                f"common.wgsl's WBS_* indices are not 0..{len(seen) - 1} with "
                f"no gaps or duplicates: {seen} -- the ledger is addressed as "
                f"slot * stride + field, so a gap wastes a word and a duplicate "
                f"makes two fields the same word")
        if len(seen) != stride:
            problems.append(
                f"world.h kWaterBodyStateWords = {stride} but common.wgsl "
                f"declares {len(seen)} WBS_* words -- the stride and the field "
                f"map must be the same number or every body but slot 0 reads "
                f"its neighbour's ledger")

    # selftest_water.cpp: the positional decoder.
    blk = re.search(r"enum\s*:\s*uint32_t\s*\{\s*WBS_STATE\s*=\s*0\s*,(.*?)\n\};",
                    st, re.S)
    if blk:
        body = re.sub(r"//[^\n]*", "", blk.group(1))
        names = [n for n in re.findall(r"\b(WBS_\w+)\b", body)]
        if len(names) + 1 != stride:
            problems.append(
                f"src/test/selftest_water.cpp's WBS_* enum declares "
                f"{len(names) + 1} words but world.h kWaterBodyStateWords = "
                f"{stride} -- the enum is POSITIONAL (one initialiser), so the "
                f"gate is reading the wrong field of the right body and saying "
                f"so with a plausible number")

    # W-D: the per-body FLAG BITS are a protocol between waterbody.cpp (which
    # ORs them into TickParams row 1, word 3) and sim_waterbody.wgsl (which
    # decodes them). Bits 0..4 predate this check and are still literals on the
    # C++ side; the two W-D bits are named in world.h so this can compare them.
    # A mismatch is silent in the worst way -- every probe would be treated as
    # an authored basin, so the size gate and the sticky refusal would simply
    # never fire and discovery would look like it worked.
    flags_h = {n.upper(): int(v) for n, v in re.findall(
        r"constexpr\s+int32_t\s+kWbf(\w+)\s*=\s*(\d+)", wh)}
    flags_w = {n: int(v) for n, v in re.findall(
        r"\bconst\s+WBF_(\w+)\s*:\s*i32\s*=\s*(\d+)\s*;", cw)}
    for name, val in flags_h.items():
        if name not in flags_w:
            problems.append(
                f"world.h declares kWbf{name.capitalize()} = {val} but "
                f"common.wgsl has no WBF_{name} -- the CPU sets a bit no shader "
                f"reads")
        elif flags_w[name] != val:
            problems.append(
                f"world.h kWbf{name.capitalize()} = {val} but common.wgsl "
                f"WBF_{name} = {flags_w[name]} -- the CPU and the ledger "
                f"disagree about which bit means what")

    # The relevel histogram: sized from one constant, indexed from another.
    dm = re.search(r"constexpr\s+uint32_t\s+kWaterRelevelDepthMax\s*=\s*(\d+)", wh)
    bk = re.search(r"constexpr\s+uint32_t\s+kWaterRelevelBuckets\s*=\s*(\d+)", wh)
    if dm and bk and int(bk.group(1)) != 8 * (int(dm.group(1)) + 2):
        problems.append(
            f"world.h kWaterRelevelBuckets = {bk.group(1)} but 8 * "
            f"(kWaterRelevelDepthMax + 2) = {8 * (int(dm.group(1)) + 2)} -- "
            f"wbSurface clamps into the end bucket, so a short block silently "
            f"piles every deep column into one bucket instead of overflowing")


def check_current_prims():
    """world.h's current-primitive ceilings must match common.wgsl's constants.

    The same check check_wind_prims makes, for the same reason and against the
    same failure: the shader loops `i < min(count, CURRENT_PRIM_CAP)` and
    strides by CURRENT_PRIM_ROWS, so a cap raised in world.h alone would leave
    the extra primitives silently unread -- a whirlpool that exists and does
    nothing, with a green build, a green selftest and an unmoved hash.

    The impact ring is here too: kWaveImpactCap sizes a RenderParams array that
    the shader iterates against WAVE_IMPACT_CAP, and a shader cap larger than
    the C++ one reads uninitialised uniform tail as ripple events.
    """
    wh = read("src/sim/world.h")
    cw = read("assets/shaders/common.wgsl")
    if not wh or not cw:
        return
    m = re.search(r"constexpr\s+uint32_t\s+kCurrentPrimCap\s*=\s*(\d+)", wh)
    g = re.search(r"const\s+CURRENT_PRIM_CAP\s*:\s*u32\s*=\s*(\d+)u", cw)
    r = re.search(r"const\s+CURRENT_PRIM_ROWS\s*:\s*u32\s*=\s*(\d+)u", cw)
    w = re.search(r"constexpr\s+uint32_t\s+kCurrentPrimWords\s*=\s*(\d+)", wh)
    iw = re.search(r"constexpr\s+uint32_t\s+kWaveImpactCap\s*=\s*(\d+)", wh)
    ig = re.search(r"const\s+WAVE_IMPACT_CAP\s*:\s*u32\s*=\s*(\d+)u", cw)
    if not (m and g and r and w and iw and ig):
        return
    checked.append("current primitives")
    if m.group(1) != g.group(1):
        problems.append(
            f"world.h kCurrentPrimCap = {m.group(1)} but common.wgsl "
            f"CURRENT_PRIM_CAP = {g.group(1)} -- the shader would read a "
            f"different number of primitives than the CPU uploads")
    if int(r.group(1)) * 4 != int(w.group(1)):
        problems.append(
            f"common.wgsl CURRENT_PRIM_ROWS = {r.group(1)} (x4 scalars per "
            f"row) but world.h kCurrentPrimWords = {w.group(1)} -- the shader "
            f"would stride past the primitive it is decoding")
    if iw.group(1) != ig.group(1):
        problems.append(
            f"world.h kWaveImpactCap = {iw.group(1)} but common.wgsl "
            f"WAVE_IMPACT_CAP = {ig.group(1)} -- the wave shader would read "
            f"past the impacts the CPU wrote")


# ------------------------------------------- the three per-tick count structs
def check_tick_counts():
    """The tick's counts cross THREE structs; all three must carry every field.

    Simulation::RecordCtx (sim/simulation.cpp) -> rhi::TableCtx
    (gpu/rhi_record.h) -> Recorder's RecordCtx (gpu/vk_record.h). Every pass
    row's condition and dispatch extent is resolved from the LAST one, and the
    two copies in between are hand-written field-by-field.

    Miss one copy and the failure is silent in the worst way: the row's
    condition reads a default-zero count, the row is never recorded, and there
    is no error anywhere -- the feature simply does nothing. That is exactly
    what happened to windWakeCount (a wind primitive shipped a wake list every
    tick and no chunk ever woke), which is why this check exists.
    """
    sim = read("src/sim/simulation.cpp")
    rec = read("src/gpu/rhi_record.h")
    vkr = read("gpu/vk_record.h") or read("src/gpu/vk_record.h")
    if not (sim and rec and vkr):
        return
    def fields(text, name, keyword="struct"):
        body = _struct_body(_strip_comments(text), name, keyword)
        if body is None:
            return None
        return {m.group(1) for m in
                re.finditer("(?:uint32_t|bool)[ ]+([A-Za-z_][A-Za-z0-9_]*)[ ]*=", body)}
    a = fields(sim, "RecordCtx")
    b = fields(rec, "TableCtx")
    c = fields(vkr, "RecordCtx")
    if a is None or b is None or c is None:
        return
    checked.append("tick count structs")
    for name, other, where in (("rhi::TableCtx", b, "src/gpu/rhi_record.h"),
                               ("the recorder's RecordCtx", c,
                                "src/gpu/vk_record.h")):
        missing = sorted(a - other)
        if missing:
            problems.append(
                f"{', '.join(missing)} is in Simulation::RecordCtx but not in "
                f"{name} ({where}) -- the count never reaches the recorder, so "
                f"every pass row conditioned on it is silently NEVER RECORDED")
    # And the copies themselves, which are what actually move the values.
    for f in sorted(a & b & c):
        if f"tc.{f} = cx.{f};" not in sim:
            problems.append(
                f"Simulation::RecordTable never copies {f} into rhi::TableCtx "
                f"-- the recorder sees the default, not this tick's value")
    vk = read("src/gpu/rhi_vk.cpp")
    if vk:
        for f in sorted(a & b & c):
            if f"cxv.{f} = cx.{f};" not in vk:
                problems.append(
                    f"rhi_vk.cpp never copies {f} from rhi::TableCtx into the "
                    f"recorder's RecordCtx -- same silent-skip failure")


# ------------------------------------------------------------- worldgen mirror
# worldgen.wgsl's terrain math is written TWICE: once in WGSL for the GPU and
# once in C++ (world.cpp) so the CPU can answer "where is the ground" for spawn
# placement, fixture anchoring and mob probes. A divergence between them is a
# player falling through ground they can see -- at some seeds, in some places,
# silently. Until now the only thing enforcing it was a comment saying "keep in
# sync", and the file had already proved that insufficient: the deleted
# surfHeightAt was a third copy of the same arithmetic and had drifted.
#
# Both sides bracket the shared code with `// MIRROR-BEGIN <tag>` ...
# `// MIRROR-END <tag>`. Blocks with the same tag concatenate IN FILE ORDER, so
# the C++ declarations are deliberately written in the shader's order.
#
# Two comparisons, because the two halves differ in how mechanically alike they
# can be:
#
#   `noise` / `height` -- FULL TOKEN STREAM. Language noise (declaration
#     keywords, type annotations, casts, `;` and `,`) is normalised away and
#     what is left is the arithmetic: identifiers, literals, operators and
#     parentheses. Parens are deliberately KEPT, because `(a+b)*c` vs `a+(b*c)`
#     is exactly the drift worth catching.
#
#   `landheight` -- INTEGER LITERALS ONLY, as a multiset. World::TerrainHeight
#     branches on a process-wide bool where the shader branches on a uniform and
#     discards the fields the CPU has no use for, so its token stream cannot
#     match. What CAN'T differ is the authored geometry -- pool centres, radii,
#     deck heights. That is the drift that actually happened here.
#
# The TUNE_* <-> tuning-member mapping is read out of sim/tuning_params.def
# rather than hardcoded, so renaming a knob keeps this check honest instead of
# silencing it.
MIRROR_RE = re.compile(
    r"^\s*//\s*MIRROR-BEGIN\s+(\w+)\s*$(.*?)^\s*//\s*MIRROR-END\s+\1\s*$",
    re.M | re.S)

# WGSL spellings that have no counterpart token on the C++ side, and vice versa.
_WGSL_DROP = (r"\b(?:let|var|fn|i32|u32|f32|bool"
              r"|N2|Oct|Land|Pond|PondSet|Shore|LandCol|CaveBands|TreeCands|BiomeMix|BiomeRelief)\b")
_CPP_DROP = (r"\b(?:static|inline|const|int|uint32_t|int32_t|unsigned|bool"
             r"|N2|Oct|Land|Pond|PondSet|Shore|IV2|BiomeMix|BiomeRelief)\b")


def _mirror_blocks(text, tag):
    return [m.group(2) for m in MIRROR_RE.finditer(text) if m.group(1) == tag]


def _decomment(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def _tune_map():
    """TUNE_FOO -> the tuning.h member name it stands for."""
    rows = _tuning_rows() or []
    return {r.wgsl: r.member for r in rows if r.has_wgsl}


def _normalise(src, wgsl, tunes):
    src = _decomment(src)
    if wgsl:
        # TUNE_* and the file's own aliases for them become the member name.
        for name, member in tunes.items():
            src = re.sub(r"\b" + name + r"\b", member, src)
        src = src.replace("POND_TILE", "pondTile")
        src = re.sub(r"bitcast<[iu]32>", " ", src)
        # Return types go before vec2<i32> becomes a constructor name, or a
        # `-> vec2<i32>` would survive as a call to iv2.
        src = re.sub(r"->\s*(?:vec2<i32>|\w+)", " ", src)
        src = src.replace("vec2<i32>", "iv2")
        src = re.sub(_WGSL_DROP, " ", src)
    else:
        src = re.sub(r"\[\[\w+\]\]", " ", src)                # [[maybe_unused]]
        src = re.sub(r"\bWG\(\)\.(\w+)", r"\1", src)          # WG().pondTile
        src = re.sub(r"\((?:int|uint32_t|int32_t|unsigned)\)", " ", src)
        src = src.replace("std::", "")
        src = re.sub(_CPP_DROP, " ", src)
    # Numeric suffixes and case-insensitive hex.
    src = re.sub(r"\b(0[xX][0-9a-fA-F]+)[uU]?\b", lambda m: m.group(1).lower(), src)
    src = re.sub(r"\b(\d+)[uUiIfF]?\b", r"\1", src)
    toks = re.findall(r"[A-Za-z_]\w*|0x[0-9a-f]+|\d+|[^\s]", src)
    return [t for t in toks if t not in (";", ",", ":")]


def check_worldgen_mirror():
    wgsl, cpp = read("assets/shaders/worldgen.wgsl"), read("src/sim/world.cpp")
    if not wgsl or not cpp:
        return
    checked.append("worldgen mirror")
    tunes = _tune_map()

    for tag in ("noise", "height"):
        a = _mirror_blocks(wgsl, tag)
        b = _mirror_blocks(cpp, tag)
        if not a or not b:
            problems.append(
                f"worldgen mirror: no `MIRROR-BEGIN {tag}` block in "
                f"{'worldgen.wgsl' if not a else 'world.cpp'} -- the CPU/GPU "
                f"terrain mirror is unenforced")
            continue
        ta = _normalise("\n".join(a), True, tunes)
        tb = _normalise("\n".join(b), False, tunes)
        if ta == tb:
            continue
        # Report the first divergence with a window of context; a raw
        # "they differ" is useless on a 400-token stream.
        i = 0
        while i < min(len(ta), len(tb)) and ta[i] == tb[i]:
            i += 1
        lo = max(0, i - 6)
        problems.append(
            f"worldgen mirror `{tag}`: worldgen.wgsl and world.cpp diverge at "
            f"token {i} (of {len(ta)}/{len(tb)}).\n"
            f"      wgsl: ...{' '.join(ta[lo:i + 8])}\n"
            f"      cpp : ...{' '.join(tb[lo:i + 8])}")

    # landheight: the authored geometry, by integer literal.
    a, b = _mirror_blocks(wgsl, "landheight"), _mirror_blocks(cpp, "landheight")
    if not a or not b:
        problems.append("worldgen mirror: no `MIRROR-BEGIN landheight` block in "
                        "worldgen.wgsl or world.cpp")
        return
    def lits(src, wgsl_side):
        return {int(t) for t in _normalise(src, wgsl_side, tunes) if t.isdigit()}
    # CONTAINMENT, not equality, and the direction is the point. The shader
    # legitimately carries constants the CPU has no use for (fluid surface
    # heights, the sentinel inits), so wgsl-only literals are fine. A literal the
    # CPU has and the shader does NOT is the failure that actually happens here:
    # the shader's authored geometry moved and the hand-written copy did not
    # follow -- exactly how the deleted surfHeightAt went stale. The opposite
    # direction, a rule added to landColumn and never mirrored, is what the
    # `terrain` gate's per-voxel pass C1 exists to catch.
    stale = sorted(lits("\n".join(b), False) - lits("\n".join(a), True))
    if stale:
        problems.append(
            "worldgen mirror `landheight`: World::TerrainHeight (world.cpp) "
            "uses authored constants that landColumn (worldgen.wgsl) no longer "
            f"has: {stale}. The shader moved and the CPU copy did not.")


# ------------------------------------------------- the baked tree atlas layout
# src/sim/treeatlas.h  <->  the TA_* constants in assets/shaders/worldgen.wgsl
#
# The atlas is ONE buffer with three directories in it, written by C++ and read
# by WGSL through hand-written word offsets on both sides. There is no generator
# and no struct: a field added to the species directory in the header without
# the matching TA_S_* bump makes the shader read the NEXT species' variant
# pointer, and what comes out is a forest of trees built from other trees'
# columns -- plausible-looking garbage, at some seeds, in some places. Nothing
# else in the repo looks at both files.
TA_PREFIX = {"kH": "TA_H_", "kS": "TA_S_", "kV": "TA_V_", "kC": "TA_C_"}
TA_SKIP = {"kHMagic", "kHVersion", "kHBiomeCount", "kHTotalWords",
           "kSFlags", "kVRuns", "kVReach", "kVAbove",
           "kVCrownY", "kVCrownR"}
# C++ camelCase enumerator -> the WGSL name, where they are not a plain
# upper-snake transliteration.
TA_ALIAS = {
    "kHSpeciesCount": "TA_H_SPECIES_COUNT", "kHMaxReach": "TA_H_MAX_REACH",
    "kHMaxAbove": "TA_H_MAX_ABOVE", "kHBiomeTable": "TA_H_BIOME_TABLE",
    "kHSpeciesDir": "TA_H_SPECIES_DIR", "kHCondTable": "TA_H_COND_TABLE",
    # The per-(biome, species) condition rows (P-D of PLAN_environment_truth).
    "kCMinY": "TA_C_MIN_Y", "kCMaxY": "TA_C_MAX_Y", "kCMaxSlope": "TA_C_MAX_SLOPE",
    "kCNearWaterMax": "TA_C_NEAR_WATER_MAX", "kCNearWaterMin": "TA_C_NEAR_WATER_MIN",
    "kCPatchThreshold": "TA_C_PATCH_THRESH",
    "kSVariantDir": "TA_S_VARIANT_DIR", "kSVariantCount": "TA_S_VARIANT_CNT",
    "kSReach": "TA_S_REACH", "kSAbove": "TA_S_ABOVE", "kSCrownY": "TA_S_CROWN_Y",
    "kSCrownR": "TA_S_CROWN_R", "kSMinY": "TA_S_MIN_Y", "kSMaxY": "TA_S_MAX_Y",
    "kSMaxSlope": "TA_S_MAX_SLOPE", "kSSparsity": "TA_S_SPARSITY",
    "kSCanopyMat": "TA_S_CANOPY_MAT", "kSShade": "TA_S_SHADE",
    "kSAutumnChance": "TA_S_AUTUMN", "kSLeaf0": "TA_S_LEAF0",
    "kSAutumn0": "TA_S_AUTUMN0",
    "kVNx": "TA_V_NX", "kVNy": "TA_V_NY", "kVNz": "TA_V_NZ",
    "kVAnchorX": "TA_V_ANCHORX", "kVAnchorZ": "TA_V_ANCHORZ",
    "kVColumns": "TA_V_COLUMNS",
}


def check_tree_atlas():
    hdr, wgsl = read("src/sim/treeatlas.h"), read("assets/shaders/worldgen.wgsl")
    if not hdr or not wgsl:
        return
    checked.append("tree atlas layout")

    # C++ side: every `enum : int { ... }` body in the header, flattened.
    cpp = {}
    for body in re.findall(r"enum\s*:\s*int\s*\{(.*?)\}", hdr, re.S):
        nxt = 0
        for tok in re.findall(r"(\w+)\s*(?:=\s*(\d+))?", _decomment(body)):
            name, val = tok
            if not name:
                continue
            nxt = int(val) if val else nxt
            cpp[name] = nxt
            nxt += 1
    for k in ("kSpeciesWords", "kVariantWords", "kHeaderWords", "kBiomeCount",
              "kFileHeaderWords"):
        m = re.search(rf"constexpr int {k} = (\d+);", hdr)
        if m:
            cpp[k] = int(m.group(1))

    # WGSL side.
    wg = {m.group(1): int(m.group(2))
          for m in re.finditer(r"const\s+(TA_\w+)\s*:\s*u32\s*=\s*(\d+)u", wgsl)}

    for name, val in sorted(cpp.items()):
        if name in TA_SKIP or not any(name.startswith(p) for p in TA_PREFIX):
            continue
        want = TA_ALIAS.get(name)
        if not want:
            continue
        if want not in wg:
            problems.append(
                f"tree atlas: treeatlas.h declares {name} = {val} but "
                f"worldgen.wgsl has no `{want}` -- the shader cannot read a "
                f"field it has no offset for")
        elif wg[want] != val:
            problems.append(
                f"tree atlas: {name} = {val} in treeatlas.h but {want} = "
                f"{wg[want]} in worldgen.wgsl. The shader would read the wrong "
                f"word of the directory, which is a forest built from other "
                f"trees' columns")

    for a, b in (("kSpeciesWords", "TA_SPECIES_WORDS"),
                 ("kVariantWords", "TA_VARIANT_WORDS"),
                 ("kCondWords", "TA_COND_WORDS")):
        if a in cpp and b in wg and cpp[a] != wg[b]:
            problems.append(
                f"tree atlas: {a} = {cpp[a]} in treeatlas.h but {b} = {wg[b]} "
                f"in worldgen.wgsl -- every directory entry after the first "
                f"would be read at the wrong stride")

    # The .svtree header width is written by JS and read by C++, and neither
    # would notice a mismatch until a real file failed to parse.
    js = read("assets/editor/treegen.js")
    if js:
        m = re.search(r"const HEADER_WORDS = (\d+);", js)
        if m and "kFileHeaderWords" in cpp and int(m.group(1)) != cpp["kFileHeaderWords"]:
            problems.append(
                f"tree atlas: treegen.js writes a {m.group(1)}-word .svtree "
                f"header, treeatlas.h reads {cpp['kFileHeaderWords']}")


def check_run_word_layout():
    """The .svtree run word is packed in THREE places and must agree in all.

    material(12) | state(4) | y0(Y0_BITS) | len(LEN_BITS), written by
    assets/editor/treegen.js (packRun), read by src/sim/treeatlas.h's constants
    and by assets/shaders/worldgen.wgsl (treeCellFrom).

    A disagreement is not a crash: every side keeps decoding, just at the wrong
    bit offsets, so the atlas parses and the forest comes out as noise -- and
    the world hash moves for a reason nobody can name. The widths moved once
    already (y0 9->11, len 7->5, to fit a 22 m redwood at 5 cm voxels), which is
    exactly the kind of change that lands in two files out of three.
    """
    js, hdr, wgsl = (read("assets/editor/treegen.js"),
                     read("src/sim/treeatlas.h"),
                     read("assets/shaders/worldgen.wgsl"))
    if not js or not hdr or not wgsl:
        return
    checked.append("svtree run word")

    def one(pat, txt, what):
        m = re.search(pat, txt)
        return int(m.group(1)) if m else None

    js_y0 = one(r"export const Y0_BITS = (\d+);", js, "js")
    js_len = one(r"export const LEN_BITS = (\d+);", js, "js")
    h_y0 = one(r"kRunY0Bits = (\d+);", hdr, "h")
    h_len = one(r"kRunLenBits = (\d+);", hdr, "h")
    w_y0mask = one(r"TREE_RUN_Y0_MASK\s*:\s*u32\s*=\s*(\d+)u", wgsl, "wgsl")
    w_shift = one(r"TREE_RUN_LEN_SHIFT\s*:\s*u32\s*=\s*(\d+)u", wgsl, "wgsl")
    w_lenmask = one(r"TREE_RUN_LEN_MASK\s*:\s*u32\s*=\s*(\d+)u", wgsl, "wgsl")

    missing = [n for n, v in (("treegen.js Y0_BITS", js_y0),
                              ("treegen.js LEN_BITS", js_len),
                              ("treeatlas.h kRunY0Bits", h_y0),
                              ("treeatlas.h kRunLenBits", h_len),
                              ("worldgen.wgsl TREE_RUN_Y0_MASK", w_y0mask),
                              ("worldgen.wgsl TREE_RUN_LEN_SHIFT", w_shift),
                              ("worldgen.wgsl TREE_RUN_LEN_MASK", w_lenmask))
               if v is None]
    if missing:
        problems.append("svtree run word: cannot find " + ", ".join(missing) +
                        " -- the three-way layout check cannot run")
        return

    if js_y0 != h_y0 or js_len != h_len:
        problems.append(
            f"svtree run word: treegen.js packs y0/len as {js_y0}/{js_len} bits "
            f"but treeatlas.h reads {h_y0}/{h_len} -- the C++ loader and the "
            f"baker disagree about where a run's height lives")
    if w_y0mask != (1 << h_y0) - 1:
        problems.append(
            f"svtree run word: worldgen.wgsl TREE_RUN_Y0_MASK is {w_y0mask} but "
            f"{h_y0} y0 bits means {(1 << h_y0) - 1} -- the shader would "
            f"truncate or over-read every run's start height")
    if w_shift != 16 + h_y0:
        problems.append(
            f"svtree run word: worldgen.wgsl TREE_RUN_LEN_SHIFT is {w_shift} but "
            f"16 + {h_y0} y0 bits means {16 + h_y0} -- the shader would read a "
            f"run's length out of the wrong bits")
    if w_lenmask != (1 << h_len) - 1:
        problems.append(
            f"svtree run word: worldgen.wgsl TREE_RUN_LEN_MASK is {w_lenmask} "
            f"but {h_len} len bits means {(1 << h_len) - 1}")
    if 16 + h_y0 + h_len != 32:
        problems.append(
            f"svtree run word: material(12) + state(4) + y0({h_y0}) + "
            f"len({h_len}) is {16 + h_y0 + h_len} bits, not 32 -- the fields "
            f"overlap or waste the word")


def check_biome_order():
    """The engine biome list lives in two code places beside the files:
    biomegen.js ENGINE_BIOMES (the tuner) and biomes.cpp kEngineBiomes (the
    biomes gate's engine-id census); treeatlas.h's kBiomeCount must stay gone
    (the count is data). worldgen.wgsl names no biome by id and treegen.js no
    longer bakes per-biome weights, so neither is listed."""
    bg = read("assets/editor/biomegen.js")
    cpp = read("src/sim/biomes.cpp")
    hdr = read("src/sim/treeatlas.h")
    if not (bg and cpp and hdr):
        return
    checked.append("biome order")
    def js_list(src, name):
        m = re.search(rf"export const {name}\s*=\s*\[([^\]]*)\]", src)
        return re.findall(r"'(\w+)'", m.group(1)) if m else None
    bg_order = js_list(bg, "ENGINE_BIOMES")
    m = re.search(r"kEngineBiomes\[kEngineBiomeCount\]\s*=\s*\{([^}]*)\}", cpp)
    cpp_order = re.findall(r'"(\w+)"', m.group(1)) if m else None
    m = re.search(r"constexpr int kBiomeCount = (\d+);", hdr)
    k = int(m.group(1)) if m else None
    lists = {"biomegen.js ENGINE_BIOMES": bg_order, "biomes.cpp kEngineBiomes": cpp_order}
    for name, val in lists.items():
        if not val:
            problems.append(f"biome order: cannot find the list in {name}")
            return
    # Since the world map's P1 the id space is the FILES: assets/biomes/*.json
    # `index` values must be exactly 0..N-1, and the tree atlas / worldMap
    # record tables are laid out in that order at load. The code lists carry
    # only the first four, so they must be a PREFIX of the file order.
    import json as _json
    files = {}
    for p in (ROOT / "assets" / "biomes").glob("*.json"):
        if p.name.startswith("_"):
            continue
        try:
            j = _json.loads(p.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001
            problems.append(f"biome order: {p.name} does not parse: {e}")
            continue
        files[p.stem] = j.get("index")
    n = len(files)
    by_id = {}
    for name, idx in files.items():
        if not isinstance(idx, int) or idx < 0 or idx >= n:
            problems.append(f"biome order: assets/biomes/{name}.json index {idx} is outside 0..{n - 1} -- ids must be contiguous")
        elif idx in by_id:
            problems.append(f"biome order: assets/biomes/{name}.json and {by_id[idx]}.json both claim index {idx}")
        else:
            by_id[idx] = name
    file_order = [by_id[i] for i in range(n) if i in by_id]
    for name, val in lists.items():
        if file_order[:len(val)] != val:
            problems.append(f"biome order: {name} is {val} but assets/biomes/*.json ids start {file_order[:len(val)]}")
    if k is not None:
        problems.append("biome order: treeatlas.h still declares kBiomeCount; the count is data now (TreeAtlas::biomeCount)")

# ----------------------------------------------------------- world map layout
def check_worldmap_layout():
    """src/sim/worldmap.h's header/record/cover-row word offsets and flag bits
    are re-declared in worldgen.wgsl as WM_H_* / WM_B_* / WM_C_* / WM_BF_*
    consts. A slot moved on one side and not the other reads a neighbouring
    field as the skin material, silently. Names map by convention:
    kHBiomeCount -> WM_H_BIOME_COUNT, kB_TreeDensity -> WM_B_TREE_DENSITY,
    kC_HeightVox -> WM_C_HEIGHT (explicit aliases below), kBF_X -> WM_BF_X."""
    hdr = read("src/sim/worldmap.h")
    wgsl = read("assets/shaders/worldgen.wgsl")
    if not (hdr and wgsl):
        return
    checked.append("world map layout")
    cpp = {}
    # kSculpt* / kSc_* (P5): the sculpt layer's geometry and block header.
    for m in re.finditer(r"\b(kH\w+|kB_\w+|kC_\w+|kS_\w+|kStamp_\w+|kW_\w+|kP_\w+|kR_\w+|kSite\w+|kSculpt\w+|kSc_\w+)\s*=\s*(\d+)", hdr):
        cpp[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"kBF_(\w+)\s*=\s*1u\s*<<\s*(\d+)", hdr):
        cpp["kBF_" + m.group(1)] = 1 << int(m.group(2))
    wg = {m.group(1): int(m.group(2))
          for m in re.finditer(r"const\s+(WM_\w+)\s*:\s*u32\s*=\s*(\d+)u", wgsl)}
    def snake(name):
        s = re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()
        return s
    alias = {"kB_PatchThreshold": "WM_B_PATCH_THRESH", "kB_PatchCellLog2": "WM_B_PATCH_LOG2",
             "kB_TreeTileVox": "WM_B_TREE_TILE", "kB_CaveThreshold1": "WM_B_CAVE_T1",
             "kB_CaveThreshold2": "WM_B_CAVE_T2", "kBiomeRecWords": "WM_B_WORDS",
             "kCoverRowWords": "WM_C_WORDS", "kC_HeightVox": "WM_C_HEIGHT",
             "kC_PatchThreshold": "WM_C_PATCH_THRESH", "kHBiomeRecords": "WM_H_BIOME_RECORDS",
             "kHMaxCoverH": "WM_H_MAX_COVER_H", "kB_MaxCoverH": "WM_B_MAX_COVER_H",
             "kHLandformPlane": "WM_H_LANDFORM_PLANE", "kHMoisturePlane": "WM_H_MOISTURE_PLANE",
             "kSiteRecWords": "WM_S_WORDS", "kStampHdrWords": "WM_STAMP_HDR_WORDS",
             "kS_PadMargin": "WM_S_PAD_MARGIN", "kS_StampOff": "WM_S_STAMP_OFF",
             "kStamp_NX": "WM_STAMP_NX", "kStamp_NY": "WM_STAMP_NY", "kStamp_NZ": "WM_STAMP_NZ",
             "kStamp_Columns": "WM_STAMP_COLUMNS",
             # the water preset table (P-E): kW_* -> WM_W_*, shore rows kP_* -> WM_P_*
             "kWaterRecWords": "WM_W_WORDS", "kShoreRowWords": "WM_P_WORDS",
             "kW_MaxPlantH": "WM_W_MAX_PLANT_H",
             # P-F: the biome's water rows kR_* -> WM_R_*, the site kinds
             "kWaterRowWords": "WM_R_WORDS", "kR_ChanceQ16": "WM_R_CHANCE_Q16",
             "kSitePad": "WM_SITE_PAD", "kSiteStamp": "WM_SITE_STAMP", "kSiteWater": "WM_SITE_WATER",
             # P6 (PLAN_map_overhaul): the tree site kind and the per-cell list
             "kSiteTree": "WM_SITE_TREE", "kSiteRotRolled": "WM_SITE_ROT_ROLLED",
             "kSiteCellMax": "WM_SITE_CELL_MAX", "kSiteTreeKeepOut": "WM_SITE_TREE_KEEP_OUT"}
    for m in re.finditer(r"\b(kBiomeRecWords|kCoverRowWords|kSiteRecWords|kStampHdrWords|kWaterRecWords|kShoreRowWords|kWaterRowWords)\s*=\s*(\d+)", hdr):
        cpp[m.group(1)] = int(m.group(2))
    for name, wgname in list(alias.items()):
        pass
    for cname, val in cpp.items():
        if cname in alias:
            wname = alias[cname]
        elif cname.startswith("kH"):
            wname = "WM_H_" + snake(cname[2:])
        elif cname.startswith("kB_"):
            wname = "WM_B_" + snake(cname[3:])
        elif cname.startswith("kC_"):
            wname = "WM_C_" + snake(cname[3:])
        elif cname.startswith("kS_"):
            wname = "WM_S_" + snake(cname[3:])
        elif cname.startswith("kW_"):
            wname = "WM_W_" + snake(cname[3:])
        elif cname.startswith("kP_"):
            wname = "WM_P_" + snake(cname[3:])
        elif cname.startswith("kR_"):
            wname = "WM_R_" + snake(cname[3:])
        elif cname.startswith("kBF_"):
            wname = "WM_BF_" + snake(cname[4:])
        elif cname.startswith("kSculpt"):
            wname = "WM_SCULPT_" + snake(cname[7:])
        elif cname.startswith("kSc_"):
            wname = "WM_SC_" + snake(cname[4:])
        else:
            continue
        if wname not in wg:
            continue  # not every header word is read by the shader (yet)
        if wg[wname] != val:
            problems.append(f"world map layout: worldmap.h {cname} = {val} but worldgen.wgsl {wname} = {wg[wname]}")
    # Every WM_ const the shader declares must be one the header defines.
    known = set()
    for cname in cpp:
        known.add(alias.get(cname, ""))
        if cname.startswith("kH"): known.add("WM_H_" + snake(cname[2:]))
        elif cname.startswith("kB_"): known.add("WM_B_" + snake(cname[3:]))
        elif cname.startswith("kC_"): known.add("WM_C_" + snake(cname[3:]))
        elif cname.startswith("kS_"): known.add("WM_S_" + snake(cname[3:]))
        elif cname.startswith("kW_"): known.add("WM_W_" + snake(cname[3:]))
        elif cname.startswith("kP_"): known.add("WM_P_" + snake(cname[3:]))
        elif cname.startswith("kR_"): known.add("WM_R_" + snake(cname[3:]))
        elif cname.startswith("kBF_"): known.add("WM_BF_" + snake(cname[4:]))
        elif cname.startswith("kSculpt"): known.add("WM_SCULPT_" + snake(cname[7:]))
        elif cname.startswith("kSc_"): known.add("WM_SC_" + snake(cname[4:]))
    for wname in wg:
        if wname not in known:
            problems.append(f"world map layout: worldgen.wgsl declares {wname} but worldmap.h has no matching word")


# ------------------------------------------------------- far cell material bits
def check_plant_tiles():
    """Tile-plant geometry must agree in THREE places.

    plantTileAt() in common.wgsl is what worldgen paints a footprint from and
    what the raymarcher rebuilds the plant from; sim/plants.h is its CPU twin
    (the --shot harness and the `plants` gate place plants with it); and each
    species' `plant` block in materials.json restates tile/foot/minH/maxH for
    the loader. A fern whose footprint is 3 wide in worldgen and 5 wide in the
    renderer draws fronds into cells that were never painted -- they vanish
    silently, with a green build and an unmoved hash. Also holds the trample
    ring literal in common.wgsl to kTrampleCap.
    """
    cw = read("assets/shaders/common.wgsl")
    ph = read("src/sim/plants.h")
    wh = read("src/sim/world.h")
    mj = read("assets/materials/materials.json")
    if not (cw and ph and wh and mj):
        return
    checked.append("plant tiles")
    species = {"FERN": ("Fern", "fern"), "SHROOM": ("Shroom", "mushroom_large")}
    try:
        mats = {m["id"]: m for m in json.loads(mj)["materials"]}
    except Exception as e:  # noqa: BLE001
        problems.append(f"materials.json: {e}")
        return
    for key, (cpp, mat_id) in species.items():
        vals = {}
        for field in ("TILE", "FOOT", "MINH", "MAXH"):
            g = re.search(rf"const\s+PLANT_{key}_{field}\s*:\s*i32\s*=\s*(\d+);", cw)
            c = re.search(rf"kPlant{cpp}{field.title().replace('h', 'H')}\s*=\s*(\d+)", ph)
            if not g or not c:
                problems.append(f"plant tiles: cannot find PLANT_{key}_{field} / kPlant{cpp}{field.title()}")
                continue
            if g.group(1) != c.group(1):
                problems.append(
                    f"common.wgsl PLANT_{key}_{field} = {g.group(1)} but plants.h "
                    f"kPlant{cpp}{field.title()} = {c.group(1)}")
            vals[field] = int(g.group(1))
        gs = re.search(rf"const\s+PLANT_{key}_SALT\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+)u;", cw)
        cs = re.search(rf"kPlant{cpp}Salt\s*=\s*(0x[0-9A-Fa-f]+)u", ph)
        if gs and cs and gs.group(1).lower() != cs.group(1).lower():
            problems.append(f"PLANT_{key}_SALT differs between common.wgsl and plants.h")
        m = mats.get(mat_id, {})
        pl = (m.get("micro") or {}).get("plant") or {}
        for field, jkey in (("TILE", "tile"), ("FOOT", "foot"), ("MINH", "minH"), ("MAXH", "maxH")):
            if field in vals and pl.get(jkey) != vals[field]:
                problems.append(
                    f"materials.json {mat_id}.micro.plant.{jkey} = {pl.get(jkey)} but "
                    f"common.wgsl PLANT_{key}_{field} = {vals[field]}")
    cap = re.search(r"constexpr\s+uint32_t\s+kTrampleCap\s*=\s*(\d+)", wh)
    arr = re.search(r"tramples\s*:\s*array<vec4f,\s*(\d+)>", cw)
    if cap and arr and int(cap.group(1)) * 2 != int(arr.group(1)):
        problems.append(
            f"world.h kTrampleCap = {cap.group(1)} (x2 vec4) but common.wgsl "
            f"RenderParams.tramples is array<vec4f, {arr.group(1)}>")


def check_far_face_word():
    """FarParams.origins[k].w: six per-face counts + a flag, packed in C++ and
    unpacked in WGSL with nothing between them to keep the layout honest.

    FarField::FaceWord (src/sim/farfield.cpp) writes it from world.h's
    kFarFace* constants; raymarch.wgsl's farBox reads it back with literals,
    because a prelude constant only two functions want would cost every shader
    a recompile (CLAUDE.md's common.wgsl note). So the literals are pinned
    here instead.

    The failure this exists for is not a crash. The word says which chunk
    layers of a cascade level are still waiting on the sieve and must NOT be
    marched; a field too narrow silently under-excludes, and the renderer draws
    the stale toroidal slab -- terrain from behind the player, in front of
    them -- until the fill lands. That is exactly what a 4-bit field did
    against a 32-chunk box before 2026-09-12.
    """
    wh = read("src/sim/world.h")
    rm = read("assets/shaders/raymarch.wgsl")
    if not wh or not rm:
        return
    n = re.search(r"constexpr\s+uint32_t\s+kFarN\s*=\s*(\d+)", wh)
    ch = re.search(r"constexpr\s+uint32_t\s+kChunk\s*=\s*(\d+)", wh)
    flag = re.search(r"constexpr\s+uint32_t\s+kFarFaceFlagBit\s*=\s*(\d+)", wh)
    if not n or not ch or not flag:
        return
    checked.append("far pending-face word")

    nchunk = int(n.group(1)) // int(ch.group(1))
    bits = 1
    while (1 << bits) < nchunk:
        bits += 1
    want = {"FAR_FACE_BITS": bits,
            "FAR_FACE_MASK": (1 << bits) - 1,
            "FAR_FACE_ALL": int(flag.group(1))}

    got = {}
    for name in want:
        m = re.search(name + r"\s*:\s*u32\s*=\s*(?:1u\s*<<\s*)?(\d+)u?", rm)
        if m:
            got[name] = int(m.group(1))
    for name, v in want.items():
        if name not in got:
            problems.append(
                f"raymarch.wgsl does not declare {name} -- farBox unpacks the "
                f"pending-face word and world.h owns its layout")
        elif got[name] != v:
            problems.append(
                f"raymarch.wgsl {name} = {got[name]} but world.h derives {v} "
                f"from kFarN/kChunk = {nchunk} chunks per level axis -- the "
                f"valid box would exclude the wrong slab")

    if bits * 6 > int(flag.group(1)):
        problems.append(
            f"world.h: six {bits}-bit face fields do not fit under "
            f"kFarFaceFlagBit = {flag.group(1)}")


def check_far_material_bits():
    """A far cascade cell is a 7-bit FAR PALETTE SLOT + 1 blocker flag (13.2.2).

    Those seven bits used to be a material id outright, which is why the loader
    refused a 129th material. They are now an index into the far palette (world.h
    kFarPaletteBaseGpu), so the ceiling counts DISTINCT FAR COLOURS, not
    materials: a look-alike shares a slot by authoring `"far": "<material>"`.

    Four places have to agree about the width, and one about the contents:
    common.wgsl FAR_PAL_MASK (what every reader masks with), common.wgsl
    MATF_FAR_PAL_MASK (what every writer packs into the material flags word),
    materials.h kMatFarPal* (the C++ mirror of that field), and world.h
    kFarPaletteSlotsGpu (the size of the reverse run). Nothing crashes when they
    stop agreeing -- the far field just paints the wrong material and claims a
    blocker wherever a stray bit falls -- so this is the only thing that would
    say so.

    materials.json is checked HERE and not only in the loader because adding a
    material is a data edit that needs no build, and a data edit that silently
    breaks the horizon should fail at the moment it is made.
    """
    wgsl = read("assets/shaders/common.wgsl")
    hpp = read("src/sim/materials.h")
    wh = read("src/sim/world.h")
    js = read("assets/materials/materials.json")
    if not wgsl or not hpp or not wh or not js:
        return
    checked.append("far cell palette slot bits")

    m = re.search(r"FAR_PAL_MASK\s*:\s*u32\s*=\s*0x([0-9A-Fa-f]+)u", wgsl)
    if not m:
        problems.append("far cell palette slot bits: common.wgsl has no "
                        "FAR_PAL_MASK -- the 7-bit split cannot be checked")
        return
    mask = int(m.group(1), 16)
    want = mask + 1                       # slots 0..mask, so mask+1 of them
    if mask != 0x7F:
        problems.append(f"far cell palette slot bits: FAR_PAL_MASK is 0x{mask:X}, "
                        "not 0x7F -- bit 7 is the blocker flag (FAR_BLOCKER_BIT)")

    # The material -> slot direction rides in the flags word. Both sides of the
    # language boundary declare its shift and mask, and a disagreement would
    # write the slot into the tint base or the wind nibbles.
    def num(text, pat, label):
        mm = re.search(pat, text)
        if not mm:
            problems.append(f"far cell palette slot bits: cannot find {label}")
            return None
        g = mm.group(1)
        return int(g, 16) if g.lower().startswith("0x") else int(g)

    wsh = num(wgsl, r"MATF_FAR_PAL_SHIFT\s*:\s*u32\s*=\s*(\d+)u",
              "common.wgsl MATF_FAR_PAL_SHIFT")
    wmk = num(wgsl, r"MATF_FAR_PAL_MASK\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+)u",
              "common.wgsl MATF_FAR_PAL_MASK")
    csh = num(hpp, r"kMatFarPalShift\s*=\s*(\d+)", "materials.h kMatFarPalShift")
    cmk = num(hpp, r"kMatFarPalMask\s*=\s*(0x[0-9A-Fa-f]+)",
              "materials.h kMatFarPalMask")
    if None not in (wsh, csh) and wsh != csh:
        problems.append(f"far cell palette slot bits: MATF_FAR_PAL_SHIFT is {wsh} "
                        f"in common.wgsl but kMatFarPalShift is {csh} in "
                        "materials.h")
    if None not in (wmk, cmk) and wmk != cmk:
        problems.append(f"far cell palette slot bits: MATF_FAR_PAL_MASK is "
                        f"0x{wmk:X} in common.wgsl but kMatFarPalMask is "
                        f"0x{cmk:X} in materials.h")
    if wmk is not None and wmk != mask:
        problems.append(f"far cell palette slot bits: the flags field holds "
                        f"0x{wmk:X} but the cell byte holds 0x{mask:X} -- a slot "
                        "would survive one and be truncated by the other")
    # The flags word: bits 0..7 MATF_* booleans, 8..15 wind, 16..23 tint base,
    # 24..30 far slot. A shift that let the slot run past bit 31 would drop it.
    if wsh is not None and wmk is not None and wsh + wmk.bit_length() > 32:
        problems.append(f"far cell palette slot bits: a {wmk.bit_length()}-bit "
                        f"slot at shift {wsh} runs off the end of the 32-bit "
                        "material flags word")

    slots = num(wh, r"kFarPaletteSlotsGpu\s*=\s*(\d+)",
                "world.h kFarPaletteSlotsGpu")
    if slots is not None and slots != want:
        problems.append(f"far cell palette slot bits: world.h reserves {slots} "
                        f"far palette entries but FAR_PAL_MASK 0x{mask:X} can "
                        f"only name {want} -- the extra ones are unreachable")
    cslots = num(hpp, r"kMatFarPalSlots\s*=\s*(\d+)", "materials.h kMatFarPalSlots")
    if None not in (slots, cslots) and slots != cslots:
        problems.append(f"far cell palette slot bits: world.h reserves {slots} "
                        f"far palette entries but materials.h hands out {cslots}")

    # The JSON does not list air; the loader prepends it at id 0 and it always
    # owns slot 0. Every material without a "far" alias needs a slot of its own.
    try:
        import json as _json
        rows = _json.loads(js).get("materials", [])
    except Exception as e:                                    # noqa: BLE001
        problems.append(f"far cell palette slot bits: materials.json will not "
                        f"parse ({e})")
        return
    byname = {r.get("id"): r for r in rows if isinstance(r, dict)}
    for r in rows:
        far = r.get("far")
        if not far:
            continue
        if far not in byname:
            problems.append(f'far cell palette slot bits: materials.json "'
                            f'{r.get("id")}" has far alias "{far}", which is '
                            "not a material")
        elif byname[far].get("far"):
            problems.append(f'far cell palette slot bits: materials.json "'
                            f'{r.get("id")}" aliases "{far}", which is itself an '
                            "alias -- name the material that owns the slot")
    owned = 1 + sum(1 for r in rows if not r.get("far"))   # +1 = implicit air
    if owned > want:
        problems.append(
            f"far cell palette slot bits: materials.json needs {owned} distinct "
            f"far palette slots (with the implicit air) but the far cell byte "
            f"holds {want}. Give the look-alikes a \"far\": \"<material>\" so "
            "they share a slot, or widen the far cell.")

# ------------------------------------------------------- readback ring depth
def check_readback_ring():
    """World::kFramesInFlight must equal rhi_vulkan.h's kAcquireSlots.

    The snapshot readback ring is sized from the pipeline depth
    (kMaxTicksPerFrame x (kFramesInFlight + 1)) so that every tick that can be
    in flight at once still has a slot. world.h cannot include a backend
    header to read the real number, so it mirrors it -- and a mirror nobody
    checks is the drift this script exists for. Under-sizing the ring is
    silent: EncodeReadbacks just declines, the CPU page-table mirror goes
    stale, and the frame blocks on a fence to get it back (P2-D,
    docs/RESEARCH_streaming_hitch.md).
    """
    w = read("src/sim/world.h")
    v = read("src/gpu/rhi_vulkan.h")
    if not w or not v:
        return
    mw = re.search(r"kFramesInFlight\s*=\s*(\d+)", w)
    mv = re.search(r"kAcquireSlots\s*=\s*(\d+)", v)
    if not mw or not mv:
        return
    checked.append("readback ring depth")
    if mw.group(1) != mv.group(1):
        problems.append(
            f"world.h kFramesInFlight = {mw.group(1)} but rhi_vulkan.h "
            f"kAcquireSlots = {mv.group(1)} -- the snapshot readback ring is "
            f"sized from the first and the GPU runs ahead by the second")

def check_col_cache_layout():
    """simulation.cpp kColCacheWords / kColCacheHdr  <->  worldgen.wgsl CC_WORDS / CC_HDR.

    The worldgen column cache (worldgen.wgsl's block of that name) is sized
    on the CPU and indexed on the GPU. A shader record wider than the CPU's
    stride reads the next column's words; a narrower one strands the last
    block's tail past the buffer. Neither faults -- the terrain just moves.
    """
    c = read("src/sim/simulation.cpp")
    w = read("assets/shaders/worldgen.wgsl")
    if not c or not w:
        return
    for cname, wname in (("kColCacheWords", "CC_WORDS"), ("kColCacheHdr", "CC_HDR")):
        mc = re.search(cname + r"\s*=\s*(\d+)", c)
        mw = re.search(r"const\s+" + wname + r"\s*:\s*u32\s*=\s*(\d+)u", w)
        if not mc or not mw:
            problems.append(f"column cache layout: cannot find {cname} (simulation.cpp) "
                            f"or {wname} (worldgen.wgsl)")
            continue
        checked.append("column cache " + wname)
        if mc.group(1) != mw.group(1):
            problems.append(f"column cache layout: simulation.cpp {cname} = {mc.group(1)} "
                            f"but worldgen.wgsl {wname} = {mw.group(1)}")

def check_autofly_surface():
    """main.cpp's --autofly-surface clearances  <->  --perf's surface-sprint.

    The `surface-sprint` scenario is a faithful copy of the `--autofly-surface`
    driver, because the harness has no Player to run the real one through: the
    game pins the altitude after Player::Update integrates velocity, and the
    perf harness moves an eye. The copy is legitimate; the two clearance
    constants drifting apart is not, because the whole claim of the scenario is
    "this is the same flight", and a scenario that flies 35 voxels over the
    canopy where the game flies 60 measures a different ray length and a
    different set of resident chunks while looking identical on the page.
    """
    m = read("src/main.cpp")
    ps = read("src/measure/perfsuite.cpp")
    if not (m and ps):
        return
    checked.append("autofly-surface clearances")
    for name in ("kAutoflySurfaceLowVox", "kAutoflySurfaceHighVox"):
        pat = r"constexpr float " + name + r"\s*=\s*([0-9.]+)f"
        a = re.search(pat, m)
        b = re.search(pat, ps)
        if not a or not b:
            problems.append(
                f"check_autofly_surface: could not find `constexpr float {name}` "
                f"in {'src/main.cpp' if not a else 'src/measure/perfsuite.cpp'} "
                f"-- the copy moved and this check went blind")
            continue
        if a.group(1) != b.group(1):
            problems.append(
                f"{name} is {a.group(1)} in src/main.cpp and {b.group(1)} in "
                f"src/measure/perfsuite.cpp -- `--perf --scenario "
                f"surface-sprint` would no longer be flying the same line as "
                f"`--frames --autofly-surface`, so the two sets of numbers stop "
                f"being comparable")
def check_burn_tint_sites():
    """Every path that turns a material's emission into light goes through burnTint.

    A MATF_BURNTINT material (leaf_burning and friends) is authored in the
    palette of the thing it WAS -- leaf green -- and the renderer supplies the
    fire by breathing the albedo toward render.burnTintColor and scaling the
    emission by the same weight (common.wgsl burnTint). A site that skips it
    does not merely look slightly wrong: it draws a leaf-green surface at full
    emissive strength, i.e. a glowing green leaf, which is the exact thing the
    feature exists to prevent.

    That is easy to miss because "shade a voxel" happens in six places across
    four shaders -- the primary hit, the far cascade, secondary rays, the two
    raster body paths, the micro-body march -- plus two that INJECT light
    rather than draw it (the openness walk and the shadow resolve pass, which
    write the same irradiance word and must agree about the value). The first
    version of burnTint covered four of the eight; the other four all showed as
    green glow in one session (owner report 2026-09-03).

    So this refuses the raw conversion anywhere but inside a burnTint() call.
    Text, not semantics: it cannot prove a shader is correct, only that nobody
    reintroduced the exact idiom that was wrong four times. ALLOWED names the
    handful of sites that legitimately read emission for something other than
    shading a surface, each with its reason.
    """
    import glob as _glob

    # (file, needle) -> why this one is not a burnTint site
    ALLOWED = {
        ("common.wgsl", "emission * TUNE_EMISSIVE_STRENGTH"):
            "irrSample takes an ALREADY-tinted emission from its caller",
        ("common.wgsl", "return emission *"):
            "emberFlicker is the fast flicker, applied to a tinted value",
        ("common.wgsl", "c += albedo * emission * 1.7"):
            "litColorO takes an already-tinted emission from its caller",
        ("raymarch.wgsl", "let emis = f32(m.emission) / 255.0;"):
            "shadeMolten: lava, an OPAQUE liquid, which can never be foliage",
        ("raymarch.wgsl", "(f32(materials[mat].emission) / 255.0) * fl"):
            "the media march: gases only, and no gas carries the flag",
    }

    pat = re.compile(r"emission\s*\)\s*/\s*255\.0")
    found = False
    for path in sorted(_glob.glob("assets/shaders/*.wgsl")):
        src = read(path)
        if not src:
            continue
        found = True
        name = path.replace("\\", "/").rsplit("/", 1)[-1]
        lines = src.splitlines()
        for i, line in enumerate(lines):
            if not pat.search(line):
                continue
            # An argument to burnTint() may sit on the call line or the line
            # under it (the call is usually wrapped at 80 columns).
            window = line + " " + (lines[i - 1] if i else "")
            if "burnTint(" in window:
                continue
            if any(k[0] == name and k[1] in line for k in ALLOWED):
                continue
            problems.append(
                f"burn tint: {name}:{i + 1} turns a material's emission into "
                f"light without burnTint() --\n      {line.strip()}\n"
                "    A MATF_BURNTINT material would draw its UNBURNT palette "
                "(leaf green) at full\n    emissive strength here. Pass it "
                "through burnTint() (burnTintWeight for a grid\n    cell, "
                "burnTintWeightH for a moving body, burnTintMean for the "
                "irradiance grid),\n    or add it to ALLOWED in "
                "check_burn_tint_sites with the reason.")
    if found:
        checked.append("burn tint sites")


# ------------------------------------------------------- env predictions
def check_env_predictions():
    """tests/env_predictions.json (written by scripts/test_environment.mjs
    section 8) is the JS side of the env-truth gate's nominal port: the biome
    pages' trees/ha and cover 1-in-N, keyed by an FNV-1a over
    assets/biomes/*.json in biomes::HashFileSet's (name, 0, bytes) sequence.
    The gate refuses a stale file at runtime; this is the same check without
    a GPU, so a biome edit that forgot the export fails here first."""
    p = ROOT / "tests" / "env_predictions.json"
    if not p.exists():
        problems.append("tests/env_predictions.json is missing -- run "
                        "`node scripts/test_environment.mjs` and commit it")
        return
    try:
        j = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        problems.append(f"tests/env_predictions.json does not parse: {e}")
        return
    d = ROOT / "assets" / "biomes"
    h = 0x811C9DC5
    def mix(b):
        nonlocal h
        for x in b:
            h ^= x
            h = (h * 0x01000193) & 0xFFFFFFFF
    for n in sorted(f.name for f in d.iterdir() if f.is_file() and f.suffix == ".json"):
        mix(n.encode("utf-8"))
        mix(b"\0")
        mix((d / n).read_bytes())
    want = f"{h:08x}"
    if j.get("biomesHash") != want:
        problems.append(f"tests/env_predictions.json is STALE: biomesHash "
                        f"{j.get('biomesHash')} but assets/biomes/*.json hashes {want} -- "
                        "run `node scripts/test_environment.mjs` and commit the file")
        return
    names = sorted(f.stem for f in d.iterdir() if f.is_file() and f.suffix == ".json")
    have = sorted((j.get("biomes") or {}).keys())
    if names != have:
        problems.append(f"tests/env_predictions.json lists biomes {have} but "
                        f"assets/biomes has {names}")
        return
    # The nominal formulas, restated: a drift here is the JS and the C++ port
    # disagreeing about what the page prints, which the gate would also catch.
    for n in names:
        b = json.loads((d / (n + ".json")).read_text(encoding="utf-8"))
        t = b.get("trees", {})
        tile, dens = float(t.get("tile", 14.4)), float(t.get("density", 0))
        per_ha = (10000.0 / (tile * tile)) * dens / 100.0 if tile else 0.0
        e = j["biomes"][n]
        if abs(e.get("treesPerHa", -1) - per_ha) > 1e-6 * max(1.0, per_ha):
            problems.append(f"env predictions: {n} treesPerHa {e.get('treesPerHa')} "
                            f"but the biome file gives {per_ha}")
    checked.append("env predictions")


def _wgsl_float(txt, name):
    """A `const NAME : f32 = 1.5;` literal, or None. The integer scraper beside
    this one refuses a decimal point, and the wide box's FILL is a fraction."""
    m = re.search(r"const\s+" + re.escape(name) + r"\s*:\s*f32\s*=\s*"
                  r"([0-9]*\.?[0-9]+)\s*;", txt)
    return float(m.group(1)) if m else None


def check_gas_consts():
    """The gas package's three-way agreement (docs/PLAN_gas_particles.md).

    world.h owns the numbers; sim_gas.wgsl and sim_step.wgsl each declare their
    own copies rather than putting them in common.wgsl, because a common.wgsl
    edit invalidates the whole SPIR-V cache and pays the worldgen far-cascade
    recompile (CLAUDE.md). That is the right trade and it is exactly the shape
    this file exists to police: a cap raised in world.h and not in the shader
    is a spawn list the CA overruns silently.

    It also checks that the shared gas MOTION model has exactly one definition,
    in common.wgsl -- see the block at the bottom of this function for why a
    re-introduced local copy is the failure that nothing else would catch.
    """
    checked.append("gas")
    wh = read(ROOT / "src/sim/world.h")
    gas = read(ROOT / "assets/shaders/sim_gas.wgsl")
    step = read(ROOT / "assets/shaders/sim_step.wgsl")
    common = read(ROOT / "assets/shaders/common.wgsl")
    raymarch = read(ROOT / "assets/shaders/raymarch.wgsl")

    def cxx(name):
        m = re.search(r"\b" + name + r"\s*=\s*(-?\d+)", wh)
        return int(m.group(1)) if m else None

    def wgsl(txt, name):
        m = re.search(r"const\s+" + name + r"\s*:\s*[iu]32\s*=\s*(-?\d+)", txt)
        return int(m.group(1)) if m else None

    pairs = [
        ("kGasParticleCap", "GAS_PARTICLE_CAP", [("sim_gas.wgsl", gas)]),
        ("kGasSpawnPerTick", "GAS_SPAWN_CAP", [("sim_gas.wgsl", gas),
                                               ("sim_step.wgsl", step)]),
        ("kGasCpuSpawnPerTick", "GAS_CPU_SPAWN_CAP", [("sim_gas.wgsl", gas)]),
        ("kGasCeilingVox", "GAS_CEILING_VOX", [("sim_gas.wgsl", gas)]),
        # THREE modules declare their own copies rather than importing them:
        # sim_gas splats parcels into the box, sim_step splats in-window gas
        # VOXELS into it (stage 1b, so the renderer can crossfade between the
        # two representations), and raymarch samples it. A common.wgsl constant
        # would cost the whole SPIR-V cache. That is the right trade and this is
        # the price of it -- any two of the three disagreeing about the cell
        # size is a plume drawn in the wrong place, silently.
        ("kGasOuterN", "GAS_OUTER_N", [("sim_gas.wgsl", gas),
                                       ("sim_step.wgsl", step),
                                       ("raymarch.wgsl", raymarch)]),
        ("kGasOuterShift", "GAS_OUTER_SHIFT", [("sim_gas.wgsl", gas),
                                               ("sim_step.wgsl", step),
                                               ("raymarch.wgsl", raymarch)]),
        # ---- far fire plumes (world.h's kGasFarEmitMax block) -------------
        # The emitter list's LAYOUT, which the CPU writes (src/sim/farplumes.cpp)
        # and sim_gas's gasFarPlume reads. A stride that disagreed would not
        # crash: it would read a neighbouring emitter's coordinate as a
        # strength and draw plumes in the wrong places, which is exactly the
        # class of failure that only a checker catches.
        ("kGasFarEmitMax", "GAS_FAR_EMIT_MAX", [("sim_gas.wgsl", gas)]),
        ("kGasFarEmitHdr", "GAS_FAR_EMIT_HDR", [("sim_gas.wgsl", gas)]),
        ("kGasFarEmitStride", "GAS_FAR_EMIT_STRIDE", [("sim_gas.wgsl", gas)]),
        ("kGasFarEmitMaxWide", "GAS_FAR_WIDE_MAX", [("sim_gas.wgsl", gas)]),
        # The LONG-RANGE box. THREE declarations again and for gasOuter's
        # reason: sim_gas splats into it and raymarch samples it, and a common
        # .wgsl constant would cost the whole SPIR-V cache. Either of them
        # disagreeing about the cell size is a plume drawn in the wrong place,
        # silently.
        ("kGasFarOuterN", "GAS_FAROUT_N", [("sim_gas.wgsl", gas),
                                           ("raymarch.wgsl", raymarch)]),
        ("kGasFarOuterShift", "GAS_FAROUT_SHIFT", [("sim_gas.wgsl", gas),
                                                   ("raymarch.wgsl", raymarch)]),
        # ...and the ANISOTROPIC y shift (2026-09-19): 8-voxel cells vertically.
        # The three copies disagreeing here is a plume drawn at the wrong
        # HEIGHT — the ground-smoke bug this cell size was introduced to fix.
        ("kGasFarOuterShiftY", "GAS_FAROUT_SHIFT_Y", [("sim_gas.wgsl", gas),
                                                      ("raymarch.wgsl", raymarch)]),
    ]
    wgslf = _wgsl_float
    for cname, wname, shaders in pairs:
        want = cxx(cname)
        if want is None:
            problems.append(f"gas: src/sim/world.h has no {cname}")
            continue
        for fname, txt in shaders:
            got = wgsl(txt, wname)
            if got is None:
                problems.append(
                    f"gas: {fname} does not declare {wname}, which must equal "
                    f"world.h's {cname} ({want})")
            elif got != want:
                problems.append(
                    f"gas: {fname} {wname} = {got} but world.h {cname} = "
                    f"{want} -- a cap raised on one side only is a buffer "
                    f"overrun the GPU will not report")

    # kGasFarEmitStrengthMax is DERIVED in world.h (the gasOuter cell area in
    # x/z times the fine chunk height), so it cannot be scraped as a literal --
    # it is recomputed here from the two constants it is built from, which is
    # the point: the shader divides by it to normalise "how much of this column
    # is on fire", and a stale copy would make every far plume the wrong
    # density by a constant factor with nothing to notice.
    # kGasFarEmitWideBase is DERIVED (the header plus the whole fine section),
    # so like kGasFarEmitStrengthMax below it is recomputed here from the parts
    # rather than scraped. It is the word the wide splat starts reading at: get
    # it wrong and the kernel reads a fine emitter's COORDINATE as a wide
    # emitter's strength, which draws plumes at plausible-looking wrong places.
    hdr = cxx("kGasFarEmitHdr")
    stride = cxx("kGasFarEmitStride")
    fineMax = cxx("kGasFarEmitMax")
    if hdr is not None and stride is not None and fineMax is not None:
        want = hdr + fineMax * stride
        got = wgsl(gas, "GAS_FAR_WIDE_BASE")
        if got is None:
            problems.append(
                "gas: sim_gas.wgsl does not declare GAS_FAR_WIDE_BASE, which "
                f"must equal world.h's kGasFarEmitWideBase ({want})")
        elif got != want:
            problems.append(
                f"gas: sim_gas.wgsl GAS_FAR_WIDE_BASE = {got} but world.h "
                f"derives kGasFarEmitWideBase = {want} from kGasFarEmitHdr "
                f"({hdr}) + kGasFarEmitMax ({fineMax}) * kGasFarEmitStride "
                f"({stride})")

    sh = cxx("kGasOuterShift")
    ch = cxx("kChunk")
    if sh is not None and ch is not None:
        want = (1 << sh) * (1 << sh) * ch
        got = wgsl(gas, "GAS_FAR_STRENGTH_MAX")
        if got is None:
            problems.append(
                "gas: sim_gas.wgsl does not declare GAS_FAR_STRENGTH_MAX, "
                f"which must equal world.h's kGasFarEmitStrengthMax ({want})")
        elif got != want:
            problems.append(
                f"gas: sim_gas.wgsl GAS_FAR_STRENGTH_MAX = {got} but world.h "
                f"derives kGasFarEmitStrengthMax = {want} from kGasOuterShift "
                f"({sh}) and kChunk ({ch})")

    # ---- the wide box's own CORE threshold (2026-09-19) ----------------
    # sim_gas.wgsl's wide deposit scales by FAR_PLUME_WIDE_FILL /
    # FAR_PLUME_WIDE_CHORD so that a RAY collects the same integral from either
    # box -- which means the wide box's PER-CELL COUNT is that much lower for
    # the same plume. raymarch.wgsl's gasErode thresholds on a per-cell COUNT,
    # so it needs its own core value derived from the SAME ratio, and it
    # mirrors FILL to get it (CHORD it derives from the two shifts it already
    # declares).
    #
    # These drifting apart does not crash and does not move a hash: it makes
    # distant smoke either vanish (threshold too high -- the bug this pin was
    # written for, where a tree-sized fire sat under GAS_ERODE * core and was
    # carved to nothing while the same fire drew fine at midrange) or turn into
    # a solid slab (too low). Neither is visible to any gate, because the
    # erosion is a RENDER-side function and the gates read the density box.
    fill = wgslf(gas, "FAR_PLUME_WIDE_FILL")
    mirror = wgslf(raymarch, "GAS_CORE_WIDE_FILL")
    if fill is None:
        problems.append("gas: sim_gas.wgsl has no FAR_PLUME_WIDE_FILL")
    elif mirror is None:
        problems.append(
            "gas: raymarch.wgsl does not declare GAS_CORE_WIDE_FILL, which "
            f"must mirror sim_gas.wgsl FAR_PLUME_WIDE_FILL ({fill}) so the "
            "wide box's erosion threshold tracks the wide deposit's scale")
    elif abs(mirror - fill) > 1e-6:
        problems.append(
            f"gas: raymarch.wgsl GAS_CORE_WIDE_FILL = {mirror} but "
            f"sim_gas.wgsl FAR_PLUME_WIDE_FILL = {fill} -- the renderer would "
            "threshold the long-range box at the wrong density and either "
            "erase distant plumes or draw them as slabs, with every gate green")

    # ---- the gas DIRTY-REASON bits ------------------------------------
    # world.h's kDirtyReasonName is a positional table -- the bit is the row
    # index -- and world.h's kDirtyGasMask is built from it BY NAME, which
    # catches a rename but not a renumber. The renumber is the one that
    # matters: kDirtyGasMask is what arms the renderer's "gas may be present"
    # flag (RenderParams bit 3), so a DIRTY_M_GAS* constant moved in
    # sim_step.wgsl without a matching row inserted here turns the whole
    # voxel->parcel crossfade off in every frame, silently, with every gate
    # still green.
    names = re.findall(r'"([^"]*)"', wh.split("kDirtyReasonName")[1]
                       .split("};")[0])
    for wgsl_name, row in (("DIRTY_M_GAS", "gas"),
                           ("DIRTY_M_GASLAT", "gas-lat"),
                           ("DIRTY_M_GASEDGE", "gas-edge")):
        got = wgsl(step, wgsl_name)
        if got is None:
            problems.append(f"gas: sim_step.wgsl does not declare {wgsl_name}")
            continue
        if row not in names:
            problems.append(
                f"gas: world.h kDirtyReasonName has no {row!r} row, so "
                f"kDirtyGasMask cannot name {wgsl_name}")
            continue
        want = 1 << names.index(row)
        if got != want:
            problems.append(
                f"gas: sim_step.wgsl {wgsl_name} = {got} but world.h's "
                f"kDirtyReasonName puts {row!r} at bit {names.index(row)} "
                f"({want}) -- kDirtyGasMask is built from that index, and a "
                f"mismatch leaves the render crossfade permanently off")

    # The gasSpawn header word indices, which BOTH shaders and the C++ readback
    # index into. sim_step writes words 0..2, sim_gas writes 3..8, and
    # World::EncodeReadbacks folds the lot into the snapshot BY OFFSET -- so a
    # mismatch reports one counter's value under another counter's name.
    for cname, wname in [("kGasSpCount", "GAS_SP_COUNT"),
                         ("kGasSpRefused", "GAS_SP_REFUSED"),
                         ("kGasSpEdge", "GAS_SP_EDGE"),
                         ("kGasSpHdr", "GAS_SP_HDR"),
                         ("kGasSpStride", "GAS_SP_STRIDE")]:
        want = cxx(cname)
        for fname, txt in (("sim_gas.wgsl", gas), ("sim_step.wgsl", step)):
            got = wgsl(txt, wname)
            if got is not None and want is not None and got != want:
                problems.append(
                    f"gas: {fname} {wname} = {got} but world.h {cname} = "
                    f"{want} -- the header is read back by offset")

    # ---- ONE DEFINITION, and the checker's job is to keep it that way ------
    # These moved to common.wgsl when the gas particle kernel landed, because
    # "two shaders must AGREE" is the whole criterion for that file: the CA
    # moves a gas VOXEL and sim_gas moves a gas PARCEL, and the plan's
    # requirement is that they make the SAME move.
    #
    # They spent one commit duplicated (common.wgsl was held by another package
    # at the time) with a rule here comparing the two bodies token for token.
    # That rule has nothing left to compare — so it is replaced by the check
    # that matters now, which is that nobody re-introduces a local copy. A
    # second definition of gasIntentK in sim_step.wgsl would not fail to
    # compile and would not fail a gate; it would simply mean the voxel and the
    # parcel had quietly stopped agreeing.
    moved = ["gasRndK", "windLateralCode", "windLateralStartK", "windAxisFrac",
             "gasLateralRot", "gasIntentK", "gasLadderStep"]
    for name in moved:
        if not re.search(r"^fn\s+" + name + r"\s*\(", common, re.M):
            problems.append(
                f"gas: common.wgsl no longer defines {name} -- the CA and the "
                f"gas particle kernel both call it and it is the single "
                f"definition they agree on")
        for fname, txt in (("sim_step.wgsl", step), ("sim_gas.wgsl", gas)):
            if re.search(r"^fn\s+" + name + r"\s*\(", txt, re.M):
                problems.append(
                    f"gas: {fname} defines its OWN {name}, shadowing the one in "
                    f"common.wgsl -- a gas parcel must take the moves a gas "
                    f"voxel would (PLAN_gas_particles.md 2.2), and a local copy "
                    f"is how that stops being true without anything failing")
    for name in ("GasIntent",):
        if not re.search(r"^struct\s+" + name + r"\b", common, re.M):
            problems.append(f"gas: common.wgsl no longer defines struct {name}")
        for fname, txt in (("sim_step.wgsl", step), ("sim_gas.wgsl", gas)):
            if re.search(r"^struct\s+" + name + r"\b", txt, re.M):
                problems.append(
                    f"gas: {fname} defines its own struct {name}, shadowing "
                    f"common.wgsl's")

# ------------------------------------------------------------- material ids
# A material's id IS its position in materials.json (index + 1, air first --
# materials.cpp LoadMaterials), and world.h names a few of those positions as
# kMat* constants so C++ can paint without a lookup. Inserting a material
# anywhere but the end shifts every row after it, and nothing in the build
# notices: `brain` went in at 122 on 2026-09-17 and kMatMushroomLarge kept
# saying 122, so the --shot flora painter drew two toadstools out of brain for
# two days until the `plants` gate asked whether its "mushroom" was a plant.
# Each constant's name is its material id in CamelCase (kMatTallGrassHead ->
# tall_grass_head), which is what makes the pair checkable here.
def check_material_ids():
    world = read("src/sim/world.h")
    raw = read("assets/materials/materials.json")
    if not world or not raw:
        return
    try:
        mats = json.loads(raw)["materials"]
    except (ValueError, KeyError, TypeError):
        return
    ids = {m["id"]: i + 1 for i, m in enumerate(mats) if isinstance(m, dict)}
    checked.append("material ids")
    # The RUNTIME half (rule-unification W1-D): materials.cpp
    # CheckPinnedMaterialIds refuses a materials.json that moved a constant, by
    # a {kMatX, "x"} table. Every world.h constant must have its row there, with
    # the same snake name this check derives -- or the runtime would not refuse.
    mcpp = read("src/sim/materials.cpp") or ""
    pins = dict(re.findall(r"\{(kMat[A-Z][A-Za-z0-9]*),\s*\"([a-z0-9_]+)\"\}", mcpp))
    for name, val in re.findall(r"\b(kMat[A-Z][A-Za-z0-9]*)\s*=\s*(\d+)", world):
        snake = re.sub(r"(?<!^)(?=[A-Z])", "_", name[4:]).lower()
        if mcpp and pins.get(name) != snake:
            problems.append(
                f"world.h {name}: materials.cpp CheckPinnedMaterialIds has "
                f"{'no row' if name not in pins else repr(pins[name])} for it "
                f"(want {{{name}, \"{snake}\"}}) -- the runtime would not "
                f"refuse a materials.json that moved it")
        if snake == "air":
            continue
        if snake not in ids:
            problems.append(
                f"world.h {name} = {val}: no material named '{snake}' in "
                f"materials.json (renamed? the constant's name must be the id "
                f"in CamelCase)")
        elif ids[snake] != int(val):
            problems.append(
                f"world.h {name} = {val} but '{snake}' is at position "
                f"{ids[snake]} in materials.json -- a material was inserted or "
                f"removed above it; move the constant (and grep for the old "
                f"literal)")


# ------------------------------------------------------------- scoop ledger
def check_scoop_ledger():
    """world.h kPageFaultScoop* <-> sim_mutate.wgsl SCOOP_*_WORD.

    The vessels' ledger (game/container.h) is three words of the page-fault
    record: sim_mutate adds to them and World::ReadSnapshot copies them out.
    The WGSL side declares its own indices (a constant one shader reads lives
    in that shader), so nothing but this check keeps the two numbers equal --
    and a mismatch is a flask paid from the wrong counter, silently.
    """
    wh = read("src/sim/world.h")
    sm = read("assets/shaders/sim_mutate.wgsl")
    if not wh or not sm:
        return
    checked.append("scoop ledger")
    for cpp, wgsl in (("kPageFaultScoopEighths", "SCOOP_EIGHTHS_WORD"),
                      ("kPageFaultScoopApplied", "SCOOP_APPLIED_WORD"),
                      ("kPageFaultScoopRefused", "SCOOP_REFUSED_WORD")):
        a = re.search(r"constexpr\s+uint32_t\s+" + cpp + r"\s*=\s*(\d+)", wh)
        b = re.search(r"const\s+" + wgsl + r"\s*:\s*u32\s*=\s*(\d+)u", sm)
        if not a or not b:
            problems.append(f"scoop ledger: {cpp} / {wgsl} not found")
            continue
        if a.group(1) != b.group(1):
            problems.append(f"scoop ledger: world.h {cpp} = {a.group(1)} but "
                            f"sim_mutate.wgsl {wgsl} = {b.group(1)}")
    # ...and the poured-particle bit, same shape: world.h <-> sim_particle.wgsl.
    sp = read("assets/shaders/sim_particle.wgsl")
    for cpp, wgsl in (("kPFlagCalm", "PFLAG_CALM"), ("kPFlagDrip", "PFLAG_DRIP")):
        a = re.search(r"constexpr\s+uint32_t\s+" + cpp + r"\s*=\s*(\d+)u", wh)
        b = re.search(r"const\s+" + wgsl + r"\s*:\s*u32\s*=\s*(\d+)u", sp or "")
        if not a or not b:
            problems.append(f"particle flag: {cpp} / {wgsl} not found")
        elif a.group(1) != b.group(1):
            problems.append(f"particle flag: world.h {cpp} = {a.group(1)} but "
                            f"sim_particle.wgsl {wgsl} = {b.group(1)}")
    # The vessel's MEASURED bit lives beside its only CPU writer.
    ch = read("src/game/container.h")
    a = re.search(r"constexpr\s+uint32_t\s+kPFlagMeasured\s*=\s*(\d+)u", ch or "")
    b = re.search(r"const\s+PFLAG_MEASURED\s*:\s*u32\s*=\s*(\d+)u", sp or "")
    if not a or not b:
        problems.append("particle flag: kPFlagMeasured / PFLAG_MEASURED not found")
    elif a.group(1) != b.group(1):
        problems.append(f"particle flag: container.h kPFlagMeasured = {a.group(1)} "
                        f"but sim_particle.wgsl PFLAG_MEASURED = {b.group(1)}")
    # The held vessel's fill level rides the micro-body instance word above
    # the dye flag; the CPU packs it and microbody.wgsl unpacks it.
    mb = read("assets/shaders/microbody.wgsl")
    a = re.search(r"constexpr\s+uint32_t\s+kHeldFillLevelShift\s*=\s*(\d+)", read("src/phys/fillview.h") or "")
    b = re.search(r"const\s+HELD_FILL_LEVEL_SHIFT\s*:\s*u32\s*=\s*(\d+)u", mb or "")
    if not a or not b:
        problems.append("held fill: kHeldFillLevelShift / HELD_FILL_LEVEL_SHIFT not found")
    elif a.group(1) != b.group(1):
        problems.append(f"held fill: fillview.h kHeldFillLevelShift = {a.group(1)} "
                        f"but microbody.wgsl HELD_FILL_LEVEL_SHIFT = {b.group(1)}")
    w = re.search(r"constexpr\s+uint32_t\s+kPageFaultWords\s*=\s*(\d+)", wh)
    e = re.search(r"constexpr\s+uint32_t\s+kPageFaultScoopRefused\s*=\s*(\d+)", wh)
    if w and e and int(e.group(1)) >= int(w.group(1)):
        problems.append("scoop ledger: kPageFaultScoopRefused is past the end "
                        "of the page-fault record (kPageFaultWords)")


def check_powder_mass():
    """world.h kPowder* <-> common.wgsl POWDER_* (docs/PLAN_powder_mass.md).

    The powder state-nibble encoding (0..2 full, 3..9 = 1..7 eighths) is
    decoded on both sides: the GPU CA/renderer and the CPU mirror, vessels and
    collision. A drift is sand that weighs one thing to the sim and another to
    the player's feet.
    """
    wh = read("src/sim/world.h")
    cw = read("assets/shaders/common.wgsl")
    if not wh or not cw:
        return
    checked.append("powder mass")
    for cpp, wgsl in (("kPowderFull", "POWDER_FULL"),
                      ("kPowderGrainState", "POWDER_GRAIN_STATE"),
                      ("kPowderPartialLo", "POWDER_PARTIAL_LO"),
                      ("kPowderPartialHi", "POWDER_PARTIAL_HI"),
                      ("kPowderBlockMin", "POWDER_BLOCK_MIN")):
        a = re.search(r"constexpr\s+uint32_t\s+" + cpp + r"\s*=\s*(\d+)u", wh)
        b = re.search(r"const\s+" + wgsl + r"\s*:\s*u32\s*=\s*(\d+)u", cw)
        if not a or not b:
            problems.append(f"powder mass: {cpp} / {wgsl} not found")
        elif a.group(1) != b.group(1):
            problems.append(f"powder mass: world.h {cpp} = {a.group(1)} but "
                            f"common.wgsl {wgsl} = {b.group(1)}")
    # The offset (state = mass + 2) is written as a literal on both sides.
    lo = re.search(r"constexpr\s+uint32_t\s+kPowderPartialLo\s*=\s*(\d+)u", wh)
    if lo and int(lo.group(1)) != 3:
        problems.append("powder mass: kPowderPartialLo moved off 3 but the "
                        "`mass + 2` / `s - 2` offset literals did not")


# ------------------------------------------- the reaction-condition gate
# assets/shaders/common.wgsl  <->  src/sim/materials.h (MIRROR-BEGIN reactgate)
#
# One authored reaction table has two evaluators: the GPU runs it over the grid
# (sim_step lightMatches/rainChance) and over gas parcels (sim_gas), the CPU
# over rigid bodies and mob limbs (reactcpu.h ReactLightMatches, and every body
# burner's RainScaledChance). Until rule-unification W1-B1 the condition
# arithmetic was retyped in all three; now each side has ONE definition
# (common.wgsl's reactPhaseOpen / reactWeatherChance / reactGate, materials.h's
# token-for-token copy), and this compares them. A drift is never a crash: it
# is a body that burns in the rain while the voxel beside it is doused.
_REACTGATE_NAMES = {
    "RCOND_SKY": "kCondSky", "RCOND_DAY": "kCondDay",
    "RCOND_NIGHT": "kCondNight", "RCOND_RAIN": "kCondRain",
    "RCOND_RAINDAMP": "kCondRainDamp",
    "RAIN_AMOUNT_MASK": "kRainAmountMask", "RAIN_DAMP_SHIFT": "kRainDampShift",
    "RAIN_WET_SHIFT": "kRainWetShift",
}


def check_react_gate():
    wgsl, hpp = read("assets/shaders/common.wgsl"), read("src/sim/materials.h")
    if not wgsl or not hpp:
        return
    checked.append("reaction gate")
    a, b = _mirror_blocks(wgsl, "reactgate"), _mirror_blocks(hpp, "reactgate")
    if len(a) != 1 or len(b) != 1:
        problems.append(
            "reaction gate: expected exactly one `MIRROR-BEGIN reactgate` block "
            f"in common.wgsl (found {len(a)}) and in materials.h (found "
            f"{len(b)}) -- the GPU/CPU condition mirror is unenforced")
        return
    ta = [_REACTGATE_NAMES.get(t, t) for t in _normalise(a[0], True, {})]
    tb = _normalise(b[0], False, {})
    if ta != tb:
        i = 0
        while i < min(len(ta), len(tb)) and ta[i] == tb[i]:
            i += 1
        lo = max(0, i - 6)
        problems.append(
            "reaction gate: common.wgsl and materials.h `reactgate` diverge at "
            f"token {i} (of {len(ta)}/{len(tb)}) -- the CA and the body burners "
            "no longer evaluate a rule's condition the same way.\n"
            f"      wgsl: ...{' '.join(ta[lo:i + 8])}\n"
            f"      cpp : ...{' '.join(tb[lo:i + 8])}")
    # The constants the two streams read, by value.
    for w, c in _REACTGATE_NAMES.items():
        mw = re.search(r"const\s+" + w + r"\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\s*;", wgsl)
        mc = re.search(r"\b" + c + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?\b", hpp)
        if not mw or not mc:
            problems.append(f"reaction gate: cannot read {w} (common.wgsl) or "
                            f"{c} (materials.h)")
            continue
        if int(mw.group(1), 0) != int(mc.group(1), 0):
            problems.append(f"reaction gate: {w} = {mw.group(1)} in common.wgsl "
                            f"but {c} = {mc.group(1)} in materials.h")


def check_coat_flame():
    """sim_step.wgsl MATF_FLAME <-> materials.h kMatFlagFlame, plus the bit is
    one no other MATF_* in common.wgsl claims. The coat rules (DESIGN.md §6 "A
    coat is a co-located virtual neighbour") release a product only if it
    carries this flag; a drifted bit would release smoke or steam from a film
    (matter from nothing) or no flame at all (oiled ground not flammable)."""
    step, hpp = read("assets/shaders/sim_step.wgsl"), read("src/sim/materials.h")
    common = read("assets/shaders/common.wgsl")
    if not step or not hpp:
        return
    checked.append("coat flame flag")
    mw = re.search(r"const\s+MATF_FLAME\s*:\s*u32\s*=\s*(\d+)u", step)
    mc = re.search(r"kMatFlagFlame\s*=\s*(\d+)", hpp)
    if not mw or not mc:
        problems.append("coat flame flag: cannot read MATF_FLAME (sim_step.wgsl) "
                        "or kMatFlagFlame (materials.h)")
        return
    if int(mw.group(1)) != int(mc.group(1)):
        problems.append(f"coat flame flag: MATF_FLAME = {mw.group(1)} in "
                        f"sim_step.wgsl but kMatFlagFlame = {mc.group(1)} in "
                        "materials.h")
    for m in re.finditer(r"const\s+(MATF_\w+)\s*:\s*u32\s*=\s*(\d+)u", common):
        if int(m.group(2)) == int(mc.group(1)):
            problems.append(f"coat flame flag: bit {mc.group(1)} is also "
                            f"common.wgsl's {m.group(1)}")


# --------------------------------------- the coat rule + stain precedence
# assets/shaders/sim_step.wgsl (coatrule) and common.wgsl (stainprec)  <->
# src/sim/coatrule.h (both tags). rule-unification W2-J2: DESIGN.md §6 "A coat
# is a co-located virtual neighbour" is one rule for the grid and every body,
# and ONE precedence decides which of two stains owns a voxel on the ground and
# on a body. The pure arithmetic of both is written once per language and
# compared here; a drift is a wet body that burns while the wet grass beside it
# does not, or blood that paints a body where it would never paint the ground.
_COATRULE_NAMES = {
    "COAT_VERDICT_MATCH": "kCoatVerdictMatch",
    "COAT_VERDICT_COVERS": "kCoatVerdictCovers",
}
_STAINPREC_NAMES = {
    "STAIN_PREC_REFUSE": "kStainPrecRefuse", "STAIN_PREC_OWN": "kStainPrecOwn",
    "STAIN_PREC_RINSE": "kStainPrecRinse", "STAIN_PREC_OVER": "kStainPrecOver",
}


def _check_mirror_pair(label, tag, wgsl_path, names):
    wgsl, hpp = read(wgsl_path), read("src/sim/coatrule.h")
    if not wgsl or not hpp:
        problems.append(f"{label}: {wgsl_path} or src/sim/coatrule.h not found")
        return
    checked.append(label)
    a, b = _mirror_blocks(wgsl, tag), _mirror_blocks(hpp, tag)
    if len(a) != 1 or len(b) != 1:
        problems.append(
            f"{label}: expected exactly one `MIRROR-BEGIN {tag}` block in "
            f"{wgsl_path} (found {len(a)}) and in coatrule.h (found {len(b)})")
        return
    ta = [names.get(t, t) for t in _normalise(a[0], True, {})]
    tb = _normalise(b[0], False, {})
    if ta != tb:
        i = 0
        while i < min(len(ta), len(tb)) and ta[i] == tb[i]:
            i += 1
        lo = max(0, i - 6)
        problems.append(
            f"{label}: {wgsl_path} and coatrule.h `{tag}` diverge at token {i} "
            f"(of {len(ta)}/{len(tb)}) -- the grid and the bodies no longer "
            "apply the same rule.\n"
            f"      wgsl: ...{' '.join(ta[lo:i + 8])}\n"
            f"      cpp : ...{' '.join(tb[lo:i + 8])}")
    for w, c in names.items():
        mw = re.search(r"const\s+" + w + r"\s*:\s*u32\s*=\s*(\d+)u?\s*;", wgsl)
        mc = re.search(r"\b" + c + r"\s*=\s*(\d+)u?\b", hpp)
        if not mw or not mc:
            problems.append(f"{label}: cannot read {w} ({wgsl_path}) or {c} "
                            "(coatrule.h)")
            continue
        if int(mw.group(1)) != int(mc.group(1)):
            problems.append(f"{label}: {w} = {mw.group(1)} in {wgsl_path} but "
                            f"{c} = {mc.group(1)} in coatrule.h")


def check_coat_rule():
    _check_mirror_pair("coat rule", "coatrule", "assets/shaders/sim_step.wgsl",
                       _COATRULE_NAMES)


def check_stain_prec():
    _check_mirror_pair("stain precedence", "stainprec",
                       "assets/shaders/common.wgsl", _STAINPREC_NAMES)


# ------------------------------------------------------- the solute layer
# assets/shaders/*.wgsl (MIRROR-BEGIN solute)  <->  each other + src/sim/world.h
# (docs/PLAN_solutes.md). The layer's accessors are pasted into every shader
# that binds it rather than living in common.wgsl (an edit there recompiles
# every shader), so the copies must be TEXT-IDENTICAL -- a drift is two
# kernels disagreeing about which half of a word a cell lives in, i.e. mass
# silently moving between cells. And the constants must match world.h's kSol*
# block, which the CPU uses to size, reset, save and read the same buffers.
_SOLUTE_CONSTS = {
    "SOL_UNIFORM_BIT": "kSolUniformBit", "SOL_PAGE_BIT": "kSolPageBit",
    "SOL_PAGE_MASK": "kSolPageMask", "SOL_WORDS_PER_PAGE": "kSolWordsPerPage",
    "SOL_POOL_PAGES": "kSolutePoolPages",
    "SOLM_FREE": "kSolMFree", "SOLM_WANT_COUNT": "kSolMWantCount",
    "SOLM_FAULTS": "kSolMFaults", "SOLM_EXHAUSTED": "kSolMExhausted",
    "SOLM_HIGH_WATER": "kSolMHighWater", "SOLM_DISSOLVED": "kSolMDissolved",
    "SOLM_PRECIP": "kSolMPrecip", "SOLM_DISCARDED": "kSolMDiscarded",
    "SOLM_CONVERTED": "kSolMConverted", "SOLM_FAULT_SLOT": "kSolMFaultSlot",
    "SOLM_FAULT_TICK": "kSolMFaultTick", "SOLM_POURED": "kSolMPoured",
    "SOLM_SCOOPED": "kSolMScooped", "SOLM_SEAM_REFUSED": "kSolMSeamRefused",
    "SOLM_ARGS": "kSolMArgs",
    "SOLM_STACK": "kSolMStackBase", "SOLS_BASE": "kSolSpecBase",
    "SOLS_STRIDE": "kSolSpecStride", "SOLS_MAT_BASE": "kSolSpecMatBase",
}


def check_solute_mirror():
    import glob
    blocks = {}
    for f in sorted(glob.glob(str(ROOT / "assets" / "shaders" / "*.wgsl"))):
        txt = Path(f).read_text(encoding="utf-8", errors="replace")
        b = _mirror_blocks(txt, "solute")
        if b:
            blocks[Path(f).name] = b
    if not blocks:
        return
    checked.append("solute")
    for name, b in blocks.items():
        if len(b) != 1:
            problems.append(f"solute: {name} has {len(b)} `MIRROR-BEGIN solute` "
                            "blocks; expected exactly one")
    ref_name = "sim_solute.wgsl" if "sim_solute.wgsl" in blocks else sorted(blocks)[0]
    ref = blocks[ref_name][0]
    for name, b in blocks.items():
        if b[0] != ref:
            a, c = ref.splitlines(), b[0].splitlines()
            i = 0
            while i < min(len(a), len(c)) and a[i] == c[i]:
                i += 1
            problems.append(
                f"solute: {name}'s solute block differs from {ref_name}'s at line "
                f"{i + 1} of the block -- the copies must be identical.\n"
                f"      {ref_name}: {a[i] if i < len(a) else '<end>'}\n"
                f"      {name}: {c[i] if i < len(c) else '<end>'}")
    wh = read("src/sim/world.h")

    def cpp_value(name, depth=0):
        m = re.search(r"constexpr\s+uint32_t\s+" + name + r"\s*=\s*([^;]+);", wh)
        if not m or depth > 4:
            return None
        expr = m.group(1).strip()
        expr = re.sub(r"(0x[0-9A-Fa-f]+|\d+)u\b", r"\1", expr)

        def sub(mm):
            v = cpp_value(mm.group(0), depth + 1)
            return str(v) if v is not None else mm.group(0)
        expr = re.sub(r"\bk[A-Za-z0-9_]+\b", sub, expr).replace("/", "//")
        try:
            return int(eval(expr, {"__builtins__": {}}, {}))
        except Exception:
            return None

    for w, c in _SOLUTE_CONSTS.items():
        mw = re.search(r"const\s+" + w + r"\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+|\d+)u\s*;", ref)
        cv = cpp_value(c)
        if not mw or cv is None:
            problems.append(f"solute: cannot read {w} (the solute block) or {c} (world.h)")
            continue
        if int(mw.group(1), 0) != cv:
            problems.append(f"solute: {w} = {mw.group(1)} in the WGSL block but "
                            f"{c} = {cv} in world.h")
    # The RENDER reader (raymarch.wgsl solLookAt) declares its own subset of
    # these constants -- it cannot paste the MIRROR block, which binds solMeta
    # and uses atomics a fragment stage must not. Every one it declares must
    # still match world.h.
    rm = read("assets/shaders/raymarch.wgsl") or ""
    for w, c in _SOLUTE_CONSTS.items():
        mw = re.search(r"^const\s+" + w + r"\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+|\d+)u\s*;", rm, re.M)
        if not mw:
            continue
        cv = cpp_value(c)
        if cv is None or int(mw.group(1), 0) != cv:
            problems.append(f"solute: raymarch.wgsl {w} = {mw.group(1)} but "
                            f"{c} = {cv} in world.h")


def check_react_fx():
    """The reaction-effect record (docs/PLAN_alchemy_chemistry.md A): sim_step's
    RFX_* constants <-> world.h kPageFaultReactFx* / kReactFx* and materials.h
    kCondFx*, plus MATF_HEAVY_GAS <-> kMatFlagHeavyGas (and not a bit any
    common.wgsl MATF_* claims). A drifted slot base writes the scoop ledger; a
    drifted scramble decodes every blast into the wrong cell; a drifted flag
    bit makes some other material sink like chlorine. None of those fails loud
    on the GPU."""
    step = read("assets/shaders/sim_step.wgsl")
    wh = read("src/sim/world.h")
    hpp = read("src/sim/materials.h")
    common = read("assets/shaders/common.wgsl")
    if not step or not wh or not hpp:
        return
    checked.append("reaction-effect record")

    def num(txt, pat):
        m = re.search(pat, txt)
        return int(m.group(1), 0) if m else None

    pairs = [
        ("RFX_FIRES", wh, r"kPageFaultReactFxFires\s*=\s*(\w+?)u?;"),
        ("RFX_ORIGIN", wh, r"kPageFaultReactFxOrigin\s*=\s*(\w+?)u?;"),
        ("RFX_TICK", wh, r"kPageFaultReactFxTick\s*=\s*(\w+?)u?;"),
        ("RFX_SLOT0", wh, r"kPageFaultReactFxSlot0\s*=\s*(\w+?)u?;"),
        ("RFX_SLOTS", wh, r"kPageFaultReactFxSlots\s*=\s*(\w+?)u?;"),
        ("RFX_CELL_BITS", wh, r"kReactFxCellBits\s*=\s*(\w+?)u?;"),
        ("RFX_SCRAMBLE", wh, r"kReactFxScramble\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?;"),
        ("RFX_COND_SHIFT", hpp, r"kCondFxShift\s*=\s*(\w+?)u?,"),
        ("RFX_COND_MASK", hpp, r"kCondFxMask\s*=\s*(0x[0-9A-Fa-f]+|\d+)u?;"),
        ("MATF_HEAVY_GAS", hpp, r"kMatFlagHeavyGas\s*=\s*(\d+);"),
    ]
    for wname, src, pat in pairs:
        want = num(src, pat)
        got = num(step, r"const\s+" + wname + r"\s*:\s*u32\s*=\s*(0x[0-9A-Fa-f]+|\d+)u")
        if want is None or got is None:
            problems.append(f"reaction-effect record: cannot read {wname} "
                            "(sim_step.wgsl) or its C++ twin")
        elif want != got:
            problems.append(f"reaction-effect record: sim_step.wgsl {wname} = "
                            f"{got} but the C++ side says {want}")
    hg = num(hpp, r"kMatFlagHeavyGas\s*=\s*(\d+);")
    for m in re.finditer(r"const\s+(MATF_\w+)\s*:\s*u32\s*=\s*(\d+)u", common):
        if hg is not None and int(m.group(2)) == hg:
            problems.append(f"heavy gas flag: bit {hg} is also common.wgsl's "
                            f"{m.group(1)}")


ALL = {
    "solute": check_solute_mirror,
    "reactfx": check_react_fx,
    "coatrule": check_coat_rule,
    "stainprec": check_stain_prec,
    "coatflame": check_coat_flame,
    "reactgate": check_react_gate,
    "scoop": check_scoop_ledger,
    "powdermass": check_powder_mass,
    "envpred": check_env_predictions,
    "autofly": check_autofly_surface,
    "worldgen": check_worldgen_mirror,
    "treeatlas": check_tree_atlas,
    "biomes": check_biome_order,
    "worldmap": check_worldmap_layout,
    "runword": check_run_word_layout,
    "sound": check_sound_slots,
    "substeps": check_fluid_substeps,
    "tuning": check_tuning_consts,
    "tuningreach": check_tuning_reach,
    "tuningused": check_tuning_consumers,
    "render": check_render_paths,
    "world": check_world_consts,
    "arch": check_arch_paths,
    "perfnodes": check_perf_nodes,
    "perfscopes": check_perf_scope_notes,
    "params": check_gpu_structs,
    "windprim": check_wind_prims,
    "curprim": check_current_prims,
    "waterledger": check_water_ledger,
    "counts": check_tick_counts,
    "farbits": check_far_material_bits,
    "farface": check_far_face_word,
    "ringdepth": check_readback_ring,
    "colcache": check_col_cache_layout,
    "burntint": check_burn_tint_sites,
    "plants": check_plant_tiles,
    "gas": check_gas_consts,
    "matids": check_material_ids,
}

# The hook passes the edited file; run only the checks that file can break.
RELEVANT = {
    "assets/sound_schema.js": ["sound"],
    "src/audio/cues.cpp": ["sound"],
    "src/sim/tuning.cpp": ["tuning", "tuningreach"],
    "src/sim/tuning.h": ["tuning", "tuningreach", "tuningused"],
    "src/sim/tuning_params.def": ["tuning", "substeps", "tuningreach",
                                  "tuningused"],
    "assets/materials/tuning.json": ["tuningreach"],
    "scripts/tuning_prelude.py": ["tuning"],
    "scripts/tuning_def.py": ["tuning", "tuningreach", "substeps"],
    "scripts/gen_tuning_params.py": ["tuning"],
    "assets/tuning_params.js": ["tuning"],
    "assets/tuner_schema.js": ["tuningreach"],
    "assets/tuner.html": ["render", "arch", "perfnodes"],
    "assets/perfview.js": ["perfscopes"],
    "src/measure/perfnodes.h": ["perfnodes", "perfscopes"],
    "src/sim/materials.cpp": ["render", "farbits"],
    "assets/materials/materials.json": ["farbits", "plants", "matids"],
    "src/sim/plants.h": ["plants"],
    "src/gpu/resources.cpp": ["world"],
    "src/sim/farfield.cpp": ["farface"],
    "src/sim/farfield.h": ["farface"],
    "src/test/selftest.cpp": ["arch"],
    # ONE entry per file: a duplicate key in a dict literal silently replaces
    # the earlier one, and world.h / perfnodes.h each had two until 2026-09-19.
    "assets/shaders/sim_mutate.wgsl": ["scoop"],
    "assets/shaders/sim_particle.wgsl": ["scoop"],
    "src/sim/world.h": ["scoop", "world", "params", "substeps", "windprim",
                        "curprim", "waterledger", "ringdepth", "matids", "reactfx"],
    "src/test/selftest_water.cpp": ["waterledger"],
    "src/sim/world.cpp": ["worldgen"],
    "assets/shaders/worldgen.wgsl": ["worldgen", "treeatlas"],
    "src/sim/treeatlas.h": ["treeatlas"],
    "assets/editor/treegen.js": ["treeatlas"],
    "src/sim/simulation.cpp": ["counts"],
    "src/gpu/rhi_record.h": ["counts"],
    "src/gpu/vk_record.h": ["counts"],
    "src/gpu/rhi_vk.cpp": ["counts"],
    "src/gpu/rhi_vulkan.h": ["ringdepth"],
    "src/sim/pass_table.def": ["counts", "perfnodes"],
    "src/measure/perfsuite.cpp": ["autofly"],
    "src/main.cpp": ["arch", "autofly"],
    "assets/shaders/sim_gas.wgsl": ["gas"],
    "assets/shaders/raymarch.wgsl": ["gas"],
    "tests/env_predictions.json": ["envpred"],
    "scripts/test_environment.mjs": ["envpred"],
    "src/sim/materials.h": ["reactgate", "coatflame", "reactfx"],
    "assets/shaders/sim_step.wgsl": ["coatflame", "coatrule", "reactfx"],
    "assets/shaders/common.wgsl": ["stainprec", "powdermass"],
    "src/sim/coatrule.h": ["coatrule", "stainprec"],
    "src/sim/reactcpu.h": ["reactgate"],
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
                run += ["reactgate", "tuning", "world", "params", "windprim",
                        "curprim", "waterledger", "burntint", "plants",
                        "farface"]
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
