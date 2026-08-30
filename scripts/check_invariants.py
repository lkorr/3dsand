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
    d = re.search(r"TP_I\(sim,\s*fluidSubsteps,\s*TUNE_FLUID_SUBSTEPS,\s*(\d+)\)",
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
              r"|N2|Oct|Land|Pond|Shore|LandCol|CaveBands|TreeCands)\b")
_CPP_DROP = (r"\b(?:static|inline|const|int|uint32_t|int32_t|unsigned|bool"
             r"|N2|Oct|Land|Pond|Shore|IV2)\b")


def _mirror_blocks(text, tag):
    return [m.group(2) for m in MIRROR_RE.finditer(text) if m.group(1) == tag]


def _decomment(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def _tune_map():
    """TUNE_FOO -> the tuning.h member name it stands for."""
    out = {}
    for m in re.finditer(r"^TP_[FIUB]?\w*\((\w+),\s*(\w+),\s*(TUNE_[A-Z0-9_]+),",
                         read("src/sim/tuning_params.def"), re.M):
        out[m.group(3)] = m.group(2)
    return out


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


# --------------------------------------------------- worldgen length units
# Every `worldgen` tuning row is authored at refVoxelsPerMetre voxels/metre and
# LoadTuning rescales it to the live kVoxelMeters -- but only if it declared
# what KIND of quantity it is. A distance must be scaled, a noise cell (a log2
# exponent) must be shifted, a probability or a 0..255 threshold or a Q8 gradient
# must be left alone, and getting that wrong is invisible: the world simply
# stops meaning the same thing at a different voxel size, in one feature.
#
# So the worldgen block of LoadTuning may not use the unit-less ReadI/ReadU at
# all. This is not style -- it is what makes "add a row" a decision rather than a
# default, which matters because package C of the terrain overhaul adds ~45 of
# them. The one exception is refVoxelsPerMetre itself, which IS the scale.
WG_READERS = ("ReadWgLen", "ReadWgCellLog2", "ReadWgPerLen", "ReadWgCount")


def check_worldgen_units():
    tuning, table = read("src/sim/tuning.cpp"), read("src/sim/tuning_params.def")
    if not tuning or not table:
        return
    m = re.search(r'if \(const json\* g = Find\(j, "worldgen"\)\) \{(.*?)\n    // These divide',
                  tuning, re.S)
    if not m:
        problems.append("worldgen units: could not find LoadTuning's worldgen "
                        "block (check this regex against tuning.cpp)")
        return
    checked.append("worldgen length units")
    body = _decomment(m.group(1))

    for name in re.findall(r'\bReadI\(\*g, "(\w+)"', body) + \
                re.findall(r'\bReadU\(\*g, "(\w+)"', body):
        if name == "refVoxelsPerMetre":
            continue
        problems.append(
            f"worldgen.{name} is read with a unit-less ReadI/ReadU. Every row in "
            f"this group must declare whether it is a length, a noise-cell log2, "
            f"a per-length rate or a pure count -- use one of {WG_READERS}, or "
            f"the row silently means something different at a different voxel "
            f"size (src/sim/world.h kVoxelsPerMetre)")

    # And every row in the table must actually be read, or it is dead.
    rows = {m2.group(1) for m2 in
            re.finditer(r"^TP_[IUF]\(worldgen,\s*(\w+),", table, re.M)}
    got = set()
    for fn in WG_READERS + ("ReadI", "ReadU"):
        got |= set(re.findall(rf'\b{fn}\(\*g, "(\w+)"', body))
    for name in sorted(rows - got):
        problems.append(
            f"worldgen.{name} is a row in tuning_params.def but LoadTuning never "
            f"reads it -- it will silently hold its tuning.h default and ignore "
            f"tuning.json")


def check_worldgen_defaults():
    """LoadTuning's worldgen reader <-> WorldgenDefaultsJson().

    The tuner's "Reset terrain" button applies whatever WorldgenDefaultsJson()
    emits. A parameter added to the reader and forgotten in the emitter would
    silently NOT reset -- the button would look like it worked and leave one
    slider on an experimental value, which is the worst possible failure for a
    control whose entire job is "put it back". Neither side has any reason to
    notice, so this is what notices.
    """
    tuning = read("src/sim/tuning.cpp")
    if not tuning:
        return
    m = re.search(r'if \(const json\* g = Find\(j, "worldgen"\)\) \{(.*?)\n    // These divide',
                  tuning, re.S)
    d = re.search(r'std::string WorldgenDefaultsJson\(\) \{(.*?)\n\}', tuning, re.S)
    if not m or not d:
        problems.append("worldgen defaults: could not find LoadTuning's worldgen "
                        "block or WorldgenDefaultsJson (check these regexes "
                        "against tuning.cpp)")
        return
    checked.append("worldgen defaults")
    reader = _decomment(m.group(1))
    dumper = _decomment(d.group(1))

    want = []
    for fn in WG_READERS + ("ReadI", "ReadU", "ReadStr"):
        want += re.findall(r'\b%s\(\*g, "(\w+)"' % fn, reader)
    got = set(re.findall(r'\b[ns]\("(\w+)"', dumper))

    for name in want:
        if name not in got:
            problems.append(
                f"worldgen.{name} is read by LoadTuning but not emitted by "
                f"WorldgenDefaultsJson -- the tuner's Reset terrain button "
                f"would leave it alone instead of restoring it")
    for name in sorted(got - set(want)):
        problems.append(
            f"WorldgenDefaultsJson emits worldgen.{name}, which LoadTuning "
            f"never reads -- either the reader lost a row or the key is a typo, "
            f"and a reset would write a value the engine ignores")



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
TA_PREFIX = {"kH": "TA_H_", "kS": "TA_S_", "kV": "TA_V_"}
TA_SKIP = {"kHMagic", "kHVersion", "kHBiomeCount", "kHTotalWords",
           "kSFlags", "kVRuns", "kVReach", "kVAbove",
           "kVCrownY", "kVCrownR"}
# C++ camelCase enumerator -> the WGSL name, where they are not a plain
# upper-snake transliteration.
TA_ALIAS = {
    "kHSpeciesCount": "TA_H_SPECIES_COUNT", "kHMaxReach": "TA_H_MAX_REACH",
    "kHMaxAbove": "TA_H_MAX_ABOVE", "kHBiomeTable": "TA_H_BIOME_TABLE",
    "kHSpeciesDir": "TA_H_SPECIES_DIR",
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
                 ("kVariantWords", "TA_VARIANT_WORDS")):
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

ALL = {
    "worldgen": check_worldgen_mirror,
    "treeatlas": check_tree_atlas,
    "wgunits": check_worldgen_units,
    "wgdefaults": check_worldgen_defaults,
    "sound": check_sound_slots,
    "substeps": check_fluid_substeps,
    "tuning": check_tuning_consts,
    "render": check_render_paths,
    "world": check_world_consts,
    "arch": check_arch_paths,
    "params": check_gpu_structs,
    "windprim": check_wind_prims,
    "curprim": check_current_prims,
    "counts": check_tick_counts,
}

# The hook passes the edited file; run only the checks that file can break.
RELEVANT = {
    "assets/sound_schema.js": ["sound"],
    "src/audio/cues.cpp": ["sound"],
    "src/sim/tuning.cpp": ["tuning", "wgunits", "wgdefaults"],
    "src/sim/tuning.h": ["tuning"],
    "src/sim/tuning_params.def": ["tuning", "substeps", "wgunits"],
    "scripts/tuning_prelude.py": ["tuning"],
    "assets/tuner.html": ["render", "arch"],
    "src/sim/materials.cpp": ["render"],
    "src/gpu/resources.cpp": ["world"],
    "src/test/selftest.cpp": ["arch"],
    "src/sim/world.h": ["world", "params", "substeps", "windprim",
                        "curprim"],
    "src/sim/world.cpp": ["worldgen"],
    "assets/shaders/worldgen.wgsl": ["worldgen", "treeatlas"],
    "src/sim/treeatlas.h": ["treeatlas"],
    "assets/editor/treegen.js": ["treeatlas"],
    "src/sim/simulation.cpp": ["counts"],
    "src/gpu/rhi_record.h": ["counts"],
    "src/gpu/vk_record.h": ["counts"],
    "src/gpu/rhi_vk.cpp": ["counts"],
    "src/sim/pass_table.def": ["counts"],
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
                run += ["tuning", "world", "params", "windprim",
                        "curprim"]
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
