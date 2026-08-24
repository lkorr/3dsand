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

  5. ARCH MAP         assets/tuner.html ARCH_NODES  <->  the repo tree + kOrder
     The Engine tab's architecture map exists to tell a human or an agent where
     a system lives and how to test it. A stale path or a `--gate` name that no
     longer exists is a confident lie, and nothing else catches it -- the page
     renders exactly as well either way.

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
    known = set(re.findall(r'"([a-z-]+)"', order.group(1)))
    cited = set()
    for arr in re.findall(r"\btst:\s*\[(.*?)\]", block.group(1), re.S):
        cited |= set(re.findall(r"'([^']+)'", arr))
    for g in sorted(cited - known):
        problems.append(
            f"ARCH_NODES (assets/tuner.html) offers `--gate {g}`, which is not "
            f"in kOrder (src/test/selftest.cpp) -- that command runs nothing")


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


ALL = {
    "sound": check_sound_slots,
    "tuning": check_tuning_consts,
    "render": check_render_paths,
    "world": check_world_consts,
    "arch": check_arch_paths,
    "params": check_gpu_structs,
}

# The hook passes the edited file; run only the checks that file can break.
RELEVANT = {
    "assets/sound_schema.js": ["sound"],
    "src/audio/cues.cpp": ["sound"],
    "src/sim/tuning.cpp": ["tuning"],
    "src/sim/tuning.h": ["tuning"],
    "src/sim/tuning_params.def": ["tuning"],
    "scripts/tuning_prelude.py": ["tuning"],
    "assets/tuner.html": ["render", "arch"],
    "src/sim/materials.cpp": ["render"],
    "src/gpu/resources.cpp": ["world"],
    "src/test/selftest.cpp": ["arch"],
    "src/sim/world.h": ["world", "params"],
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
                run += ["tuning", "world", "params"]
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
