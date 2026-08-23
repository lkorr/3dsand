#!/usr/bin/env python3
"""Keep src/sim/pass_table.def honest against the WGSL it claims to describe.

This is the fifth "two places that must agree" pair, and the one with the
sharpest failure mode. Phase 3 of the Vulkan port (docs/PLAN_vulkan_port.md)
GENERATES vkCmdPipelineBarrier2 calls from the pass table's declared read/write
sets — no barrier is ever hand-written at a call site
(docs/vulkan_barrier_graph.md §1.1, §8). So if a row omits a buffer its entry
point actually reads, phase 3 emits no barrier for that hazard: not a weak one,
none. Under Dawn (which generates barriers itself) that costs nothing and the
selftest stays green, which is precisely why it has to be caught mechanically
now rather than discovered as a cross-vendor desync later.

Direction matters and is asymmetric:

  - a use the shader has and the table lacks  -> FAIL (a missing barrier)
  - a use the table has and the shader lacks  -> WARN (a spurious barrier)

WHAT MAKES THIS HARD: THE WALK MUST BE ROOTED
---------------------------------------------
A WGSL file declares its storage bindings at MODULE scope, shared by every entry
point in the file. The set declared in the file is therefore much larger than
the set any one entry point touches. A checker that compares the table against
module-scope declarations disagrees with a CORRECT table on most rows in this
codebase — worldgen.wgsl's `far` entry touches none of the group-0 storage
buffers the file declares, and sim_compact's two entry points each touch exactly
one of the two dirty buffers both are declared against.

So the binding set is computed by a BFS over the call graph ROOTED AT THE ENTRY
POINT: only bindings referenced by the entry point or by a function transitively
reachable from it. A name declared at module scope but reachable only from a
DIFFERENT entry point is excluded. Both halves are load-bearing — rooted (so the
first four regression cases below pass) and transitive (so the last two do):

    worldgen.wgsl   far        declares voxels/dirtyIn/dirtyOut/occupancy/
                               genList, touches none of them
    sim_particle    args1      declares 6 storage buffers, touches counts+pArgs
    sim_particle    args2      same
    sim_compact     main       declares dirtyIn AND dirtyOut, reads dirtyIn only
    sim_compact     mainNext   same, reads dirtyOut only
    sim_step        main       reaches dirtyOut only through markDirty()
    worldgen        list       reaches voxels/occupancy/dirtyIn/dirtyOut only
                               through genChunk()

`--selfcheck` asserts all seven are silent. A checker that warns on a correct row
trains people to ignore it, at which point it is worse than not having one,
because it launders a false sense of coverage.

READ VS WRITE COMES FROM THE ACCESS, NOT THE QUALIFIER
------------------------------------------------------
sim_step.wgsl and sim_occupancy.wgsl:mainDirty both declare `dirtyList` as
`var<storage, read_write>` and only ever read it. Trusting the qualifier would
upgrade 54 CA-loop barriers from RAW to WAW. Classification is therefore by
actual syntax: an assignment target or an atomic RMW/store is a write, anything
else is a read.

Exit 0 = agree. Exit 1 = a real mismatch. Accepts an optional edited-file
argument so the PostToolUse hook can skip the work when the file cannot break
the pair.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHADERS = ROOT / "assets" / "shaders"
DEF = ROOT / "src" / "sim" / "pass_table.def"
HDR = ROOT / "src" / "sim" / "pass_table.h"
SIM = ROOT / "src" / "sim" / "simulation.cpp"

problems = []   # FAIL
warnings = []   # WARN (printed, does not fail the run)


def read(p):
    return p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""


# ---------------------------------------------------------------------------
# 1. The table: which WGSL entry point each row runs, and its declared R/W set.
# ---------------------------------------------------------------------------
# A row names a PIPE_*; the pipeline -> (file, entry) mapping lives in
# BuildPipelines (simulation.cpp) as MakePipeline(..., "<file>.wgsl", "<entry>").
# Deriving it from there rather than hardcoding it here means a kernel that is
# repointed at a different entry point cannot silently keep the old row's R/W
# expectations.
PIPE_TO_MEMBER = {
    "PIPE_WORLDGEN": "worldgen_",
    "PIPE_WORLDGEN_LIST": "worldgenList_",
    "PIPE_MUTATE": "mutate_",
    "PIPE_MUTATE_CELLS": "mutateCells_",
    "PIPE_COMPACT": "compact_",
    "PIPE_COMPACT_NEXT": "compactNext_",
    "PIPE_STEP": "step_",
    "PIPE_OCCUPANCY": "occupancy_",
    "PIPE_OCCUPANCY_DIRTY": "occupancyDirty_",
    "PIPE_PICK": "pick_",
    "PIPE_EXPLODE_MARK": "explodeMark_",
    "PIPE_EXPLODE_APPLY": "explodeApply_",
    "PIPE_P_ARGS1": "pArgs1_",
    "PIPE_P_SPAWN": "pSpawn_",
    "PIPE_P_INTEGRATE": "pIntegrate_",
    "PIPE_P_ARGS2": "pArgs2_",
    "PIPE_P_RESOLVE": "pResolve_",
    "PIPE_FAR_FILL": "farFill_",
    "PIPE_FAR_DOWN": "farDown_",
    "PIPE_FLUID_SPAWN": "fluidSpawn_",
    "PIPE_FLUID_MARK": "fluidMark_",
    "PIPE_FLUID_ALLOC": "fluidAlloc_",
    "PIPE_FLUID_CLEAR": "fluidClear_",
    "PIPE_FLUID_P2G": "fluidP2g_",
    "PIPE_FLUID_P2G2": "fluidP2g2_",
    "PIPE_FLUID_GRIDUP": "fluidGridUp_",
    "PIPE_FLUID_G2P": "fluidG2p_",
}

# Table buffer id -> the WGSL identifier(s) it is bound as. One id can appear
# under different names in different shaders (dirtyList is `farDirty` in
# worldgen's group 1; the particle pages are pRead/pWrite/pReadBuf), which is a
# property of the bind groups, not of the buffer.
BUF_TO_WGSL = {
    "Voxels": {"voxels"},
    "DirtyIn": {"dirtyIn"},
    "DirtyOut": {"dirtyOut"},
    "Dirty0": {"dirtyIn", "dirtyOut"},
    "Dirty1": {"dirtyIn", "dirtyOut"},
    "Materials": {"materials"},
    "TickUBO": {"T"},
    "PassUBO": {"P"},
    "OpsBuf": {"ops"},
    "Occupancy": {"occupancy"},
    "Hash": {"worldHash"},
    "Pick": {"pick"},
    "RenderUBO": {"R"},
    "Reactions": {"reactions"},
    "DirtyList": {"dirtyList", "farDirty"},
    "ArgsStage": {"args"},
    "CellOps": {"cellOps"},
    "Support": {"supportOut"},
    "GenList": {"genList"},
    "ParticlesRead": {"pRead", "pReadBuf"},
    "ParticlesWrite": {"pWrite"},
    "ParticleCounts": {"counts"},
    "Claim": {"claim"},
    "PArgsStage": {"pArgs"},
    "ExpOps": {"expOps"},
    "ExpMask": {"expMask"},
    "SpawnOps": {"spawnOps"},
    "FarVox": {"farVox"},
    "FarOcc": {"farOcc"},
    "FarList": {"farList"},
    "FarUBO": {"F"},
    "PageTable": {"pageTable"},
    "PageFaults": {"pageFaults"},
    # MLS-MPM fluid prototype (sim_fluid.wgsl).
    "FluidParticles": {"fluidParticles", "fluid"},
    "FluidSpawnOps": {"fluidSpawnOps"},
    "FluidBlockMap": {"fluidBlockMap"},
    "FluidBlockList": {"fluidBlockList"},
    "FluidGrid": {"fluidGrid"},
    "FluidArgsStage": {"fluidArgs"},
    # Indirect-args and transfer-only buffers are never bound in a bind group,
    # so no WGSL name maps to them and the walk cannot see them. Correct: they
    # are consumed by vkCmdDispatchIndirect / vkCmdCopyBuffer, not by a shader.
    "DispatchArgs": set(),
    "PDispatchArgs": set(),
    "DrawArgs": set(),
    "FluidDispatchArgs": set(),
}

READ_ACCS = {"R", "U", "I", "TR"}
WRITE_ACCS = {"W", "RW", "A", "TW"}

# What each pipeline LAYOUT can bind, as the WGSL names the kernels use for
# those slots. Direction of the check is layout ⊇ used, never equality:
# worldgen:list binds all 17 of simPL_'s slots and uses six, which is correct
# and normal (barrier_graph §2.5.2). This exists to catch the opposite — a
# kernel placed in a pass whose layout has no slot for a binding it references.
#
# Group 0 of simPL_ is simBGL_ (17 bindings); simPL2_ pairs simSlimBGL_
# (bindings 0..4) with particleBGL_; farPL_ pairs simSlimBGL_ with farBGL_.
# The slim group exists because pairing the full simBGL_ with particleBGL_ would
# exceed Dawn's 16-storage-buffer per-stage layout limit.
_SIM_GROUP0 = {
    "voxels", "dirtyIn", "dirtyOut", "materials", "T", "P", "ops", "occupancy",
    "worldHash", "pick", "R", "reactions", "dirtyList", "args", "cellOps",
    "supportOut", "genList", "pageTable", "pageFaults",
}
# The slim group is 0..4 PLUS the two page buffers at 17/18 — not a dense
# prefix any more. One WGSL identifier cannot carry two binding numbers
# across modules that share common.wgsl, so pageTable/pageFaults keep the
# same numbers here that they have in simBGL_ (PLAN_page_table.md §5.2).
_SLIM_GROUP0 = {"voxels", "dirtyIn", "dirtyOut", "materials", "T",
                "pageTable", "pageFaults"}
_PARTICLE_GROUP1 = {"pRead", "pReadBuf", "pWrite", "counts", "claim", "pArgs",
                    "expOps", "expMask", "spawnOps"}
_FAR_GROUP1 = {"farVox", "farOcc", "farList", "F", "farDirty"}
_FLUID_GROUP1 = {"fluidParticles", "fluidSpawnOps", "fluidBlockMap",
                 "fluidBlockList", "fluidGrid", "fluidArgs"}

LAYOUT_BINDINGS = {
    "simPL_": _SIM_GROUP0,
    "simPL2_": _SLIM_GROUP0 | _PARTICLE_GROUP1,
    "farPL_": _SLIM_GROUP0 | _FAR_GROUP1,
    "fluidPL_": _SLIM_GROUP0 | _FLUID_GROUP1,
}


def parse_pipeline_entries():
    """pipeline member -> (wgsl file, entry point, pipeline layout).

    Scraped from BuildPipelines rather than hardcoded, so a kernel repointed at
    a different entry point cannot silently keep the old row's R/W expectations.
    The shape there is:

        mStep    = mod("sim_step.wgsl");
        step_    = MakeComputePipeline(device, simPL_, mStep, "main", "step");
    """
    txt = read(SIM)
    mods = {}   # module variable -> shader file name
    for m in re.finditer(r"(\w+)\s*=\s*mod\(\s*\"([\w.]+\.wgsl)\"\s*\)", txt):
        mods[m.group(1)] = m.group(2)

    out = {}
    for m in re.finditer(
            r"(\w+_)\s*=\s*MakeComputePipeline\(\s*\w+\s*,\s*(\w+)\s*,\s*(\w+)\s*,"
            r"\s*\"(\w+)\"", txt):
        member, layout, module, entry = m.groups()
        f = mods.get(module)
        if f:
            out[member] = (f, entry, layout)
    return out


def parse_table():
    """Rows from pass_table.def: name, pipe, kind, cond, repeat, uses, comments."""
    txt = read(DEF)
    if not txt:
        return []
    rows = []
    # Strip the leading banner so its prose cannot be scraped as a row comment.
    for m in re.finditer(
            r"^PASS\((\w+),\s*(nullptr|\"[^\"]*\"),\s*(PT_\w+),\s*(PIPE_\w+),\s*"
            r"(K_\w+),(.*?)\bUSES\((.*?)\)\)\s*$",
            txt, re.M | re.S):
        name, group, table, pipe, kind, mid, uses = m.groups()
        cond = re.search(r"\b(C_\w+)\b", mid)
        rep = re.findall(r",\s*(\d+)\s*,\s*$", mid)
        # Comment block immediately preceding the row (used for the
        # "read-only in practice" requirement).
        start = m.start()
        head = txt[:start].rstrip().split("\n")
        comment = []
        for line in reversed(head):
            if line.lstrip().startswith("//"):
                comment.append(line)
            else:
                break
        u = []
        for acc, buf in re.findall(r"\b(RW|TR|TW|R|W|A|U|I)\((\w+)\)", uses):
            u.append((acc, buf))
        rows.append({
            "name": name,
            "table": table,
            "pipe": pipe,
            "kind": kind,
            "cond": cond.group(1) if cond else "C_ALWAYS",
            "repeat": int(rep[0]) if rep else 1,
            "uses": u,
            "comment": "\n".join(reversed(comment)),
        })
    return rows


# ---------------------------------------------------------------------------
# 2. The WGSL: a call-graph walk ROOTED at the entry point.
# ---------------------------------------------------------------------------
WGSL_KEYWORDS = {
    "if", "else", "for", "while", "loop", "switch", "case", "default", "return",
    "break", "continue", "let", "var", "const", "fn", "struct", "array", "vec2",
    "vec3", "vec4", "mat2x2", "mat3x3", "mat4x4", "u32", "i32", "f32", "bool",
    "min", "max", "abs", "clamp", "select", "step", "mix", "floor", "ceil",
    "sqrt", "pow", "exp", "log", "sin", "cos", "tan", "dot", "cross", "length",
    "normalize", "bitcast", "workgroupBarrier", "storageBarrier", "atomicLoad",
    "atomicStore", "atomicAdd", "atomicSub", "atomicMax", "atomicMin",
    "atomicAnd", "atomicOr", "atomicXor", "atomicExchange",
    "atomicCompareExchangeWeak", "arrayLength", "sign", "fract", "round",
    "trunc", "all", "any", "countOneBits", "reverseBits", "insertBits",
    "extractBits", "firstLeadingBit", "firstTrailingBit", "saturate", "smoothstep",
    "inverseSqrt", "degrees", "radians", "atan2", "asin", "acos", "atan",
    "modf", "sinh", "cosh", "tanh", "ldexp", "frexp", "determinant", "transpose",
    "faceForward", "reflect", "refract", "distance", "dpdx", "dpdy", "fwidth",
}


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def parse_module(src):
    """(bindings, functions, entry_points) for one WGSL source string.

    bindings: name -> declared access ('read', 'read_write', 'uniform')
    functions: name -> body text
    entry_points: set of @compute fn names
    """
    src = strip_comments(src)
    bindings = {}
    for m in re.finditer(
            r"@group\(\d+\)\s*@binding\(\d+\)\s*var\s*<\s*storage\s*,\s*(read_write|read)\s*>"
            r"\s*(\w+)\s*:", src):
        bindings[m.group(2)] = m.group(1)
    for m in re.finditer(
            r"@group\(\d+\)\s*@binding\(\d+\)\s*var\s*<\s*uniform\s*>\s*(\w+)\s*:", src):
        bindings[m.group(1)] = "uniform"

    # Function bodies by brace matching (WGSL bodies nest, so a regex will not do).
    funcs = {}
    entries = set()
    for m in re.finditer(r"(@compute[^\n]*\n\s*)?fn\s+(\w+)\s*\(", src):
        name = m.group(2)
        # Find the opening brace of this function's body.
        i = src.find("{", m.end())
        if i < 0:
            continue
        depth, j = 0, i
        while j < len(src):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        funcs[name] = src[i:j + 1]
        if m.group(1):
            entries.add(name)
    # A @compute attribute can sit on its own line above `fn`; catch those too.
    for m in re.finditer(r"@compute[^;{]*?fn\s+(\w+)\s*\(", src, re.S):
        entries.add(m.group(1))
    return bindings, funcs, entries


def reachable(entry, funcs):
    """BFS over the call graph from `entry`. Returns the set of reachable fns."""
    seen, queue = {entry}, [entry]
    while queue:
        fn = queue.pop()
        body = funcs.get(fn, "")
        for m in re.finditer(r"\b(\w+)\s*\(", body):
            g = m.group(1)
            if g in funcs and g not in seen and g not in WGSL_KEYWORDS:
                seen.add(g)
                queue.append(g)
    return seen


def accesses(body, names):
    """Classify each binding name in `body` as read and/or written.

    By ACTUAL access, never by the declaration qualifier: sim_step and
    sim_occupancy:mainDirty declare dirtyList read_write and only read it.
    """
    reads, writes = set(), set()
    for n in names:
        # Writes: an assignment target `n[...] = ` / `n = `, or an atomic RMW /
        # store through `&n[...]`. atomicLoad is a read.
        wrote = False
        if re.search(rf"\b{re.escape(n)}\s*\[[^\]]*\]\s*=(?!=)", body):
            wrote = True
        if re.search(rf"\b{re.escape(n)}\s*=(?!=)", body):
            wrote = True
        if re.search(rf"atomic(?:Store|Add|Sub|Max|Min|And|Or|Xor|Exchange|"
                     rf"CompareExchangeWeak)\s*\(\s*&\s*{re.escape(n)}\b", body):
            wrote = True
        # Any other mention is a read (including atomicLoad and plain indexing).
        mentioned = re.search(rf"\b{re.escape(n)}\b", body) is not None
        if wrote:
            writes.add(n)
        if mentioned:
            # An entry that is only ever an assignment TARGET still counts as a
            # read here only if it appears somewhere that is not the target; the
            # table's W() vs RW() distinction is checked as "declared write is
            # present", so over-reporting a read would produce WARN noise. Keep
            # it precise: strip assignment targets, then look for a residual use.
            residual = re.sub(rf"\b{re.escape(n)}\s*(\[[^\]]*\])?\s*=(?!=)", " ", body)
            residual = re.sub(
                rf"atomic(?:Store)\s*\(\s*&\s*{re.escape(n)}\b[^;]*;", " ", residual)
            if re.search(rf"\b{re.escape(n)}\b", residual):
                reads.add(n)
    return reads, writes


def written_by_other_entry(fname, entry):
    """Bindings some OTHER entry point in this module writes.

    Used to scope the "read-only in practice" comment requirement to the case
    that actually looks like an oversight — see its call site.
    """
    mod = module_for(fname)
    if mod is None:
        return set()
    bindings, funcs, entries = mod
    out = set()
    for e in entries:
        if e == entry or e not in funcs:
            continue
        fns = reachable(e, funcs)
        _, w = accesses("\n".join(funcs[f] for f in fns), set(bindings))
        out |= w
    return out


_module_cache = {}


def module_for(fname):
    if fname in _module_cache:
        return _module_cache[fname]
    common = read(SHADERS / "common.wgsl")
    body = read(SHADERS / fname)
    if not body:
        _module_cache[fname] = None
        return None
    # LoadShader prepends common.wgsl; the call graph spans both.
    _module_cache[fname] = parse_module(common + "\n" + body)
    return _module_cache[fname]


def walk(fname, entry):
    """Rooted walk. -> (reads, writes, declared) as sets of WGSL binding names."""
    mod = module_for(fname)
    if mod is None:
        return None
    bindings, funcs, _ = mod
    if entry not in funcs:
        return None
    fns = reachable(entry, funcs)
    body = "\n".join(funcs[f] for f in fns)
    reads, writes = accesses(body, set(bindings))
    return reads, writes, bindings


# ---------------------------------------------------------------------------
# 3. The checks.
# ---------------------------------------------------------------------------
def check_table_vs_wgsl():
    rows = parse_table()
    if not rows:
        problems.append(
            "could not parse any row out of src/sim/pass_table.def — check this "
            "script's PASS regex against the file's macro form")
        return
    pipes = parse_pipeline_entries()
    if not pipes:
        problems.append(
            "could not scrape any pipeline -> (shader, entry) mapping out of "
            "BuildPipelines (src/sim/simulation.cpp) — check the regex")
        return

    covered = set()   # (file, entry) pairs some row names

    for r in rows:
        if r["pipe"] == "PIPE_NONE":
            continue
        member = PIPE_TO_MEMBER.get(r["pipe"])
        if not member:
            problems.append(
                f"row '{r['name']}' names {r['pipe']}, which this script does "
                f"not map to a Simulation pipeline member (add it to "
                f"PIPE_TO_MEMBER)")
            continue
        fe = pipes.get(member)
        if not fe:
            problems.append(
                f"row '{r['name']}': BuildPipelines has no MakePipeline call for "
                f"'{member}' that this script can read")
            continue
        fname, entry, layout = fe
        w = walk(fname, entry)
        if w is None:
            problems.append(
                f"row '{r['name']}': cannot walk {fname}:{entry} (missing file "
                f"or entry point)")
            continue
        reads, writes, declared = w
        covered.add((fname, entry))

        # The row's declared sets, translated into WGSL names.
        row_read, row_write = set(), set()
        for acc, buf in r["uses"]:
            names = BUF_TO_WGSL.get(buf)
            if names is None:
                problems.append(
                    f"row '{r['name']}' uses buffer id '{buf}', which is not in "
                    f"this script's BUF_TO_WGSL map (and may not exist)")
                continue
            if acc in READ_ACCS or acc == "RW":
                row_read |= names
            if acc in WRITE_ACCS:
                row_write |= names
            if acc == "RW":
                row_write |= names

        # FAIL: the shader reads something the row does not declare at all.
        for n in sorted(reads - row_read - row_write):
            problems.append(
                f"{fname}:{entry} (row '{r['name']}') READS binding '{n}', which "
                f"the row does not declare — phase 3 would emit no RAW barrier "
                f"for it")
        # FAIL: the shader writes something the row does not declare as a write.
        for n in sorted(writes - row_write):
            problems.append(
                f"{fname}:{entry} (row '{r['name']}') WRITES binding '{n}', which "
                f"the row does not declare as a write — every later reader would "
                f"be unsynchronized")
        # WARN: the row declares a use the rooted walk does not see. Spurious
        # barriers only. MUST be silent on the seven regression cases.
        for n in sorted((row_read | row_write) - reads - writes):
            if n not in declared:
                continue  # bound but not declared in this module: not the row's fault
            warnings.append(
                f"{fname}:{entry} (row '{r['name']}') declares '{n}' but the "
                f"rooted walk never reaches it — a spurious barrier")

        # FAIL: read_write in WGSL, read-only in practice, with NO comment.
        #
        # Scope matters here, and getting it wrong is how a checker becomes
        # noise. A binding declared read_write that NO entry point in the module
        # writes is unremarkable — WGSL has no per-entry-point access modes, and
        # `voxels` is declared read_write in every sim shader including the two
        # that only read it. Demanding a comment on all of those is the WARN spam
        # §6.1 says trains people to ignore the check.
        #
        # The noteworthy case, and the one the doc names, is narrower: a binding
        # that ANOTHER entry point in the same module DOES write, so the row's
        # R() looks like an oversight against its own file. The two live cases
        # are dirtyList in sim_step.wgsl and in sim_occupancy.wgsl:mainDirty,
        # both of which sit in modules whose compaction/other entry points write
        # it. Those are worth a sentence; `voxels` in sim_pick is not.
        for acc, buf in r["uses"]:
            if acc != "R":
                continue
            for n in BUF_TO_WGSL.get(buf, ()):
                if declared.get(n) != "read_write" or n in writes:
                    continue
                if n not in written_by_other_entry(fname, entry):
                    continue
                if "read-only in practice" not in r["comment"]:
                    problems.append(
                        f"row '{r['name']}' declares R({buf}) on '{n}', which "
                        f"{fname} declares read_write and ANOTHER entry point in "
                        f"that file writes — {entry} only reads it. Declaring it "
                        f"a read is correct and saves 54 WAW barriers in the CA "
                        f"loop, but the row must carry a "
                        f"'// read-only in practice: <reason>' comment, or the "
                        f"next reader will 'fix' it back")

        # FAIL: the pipeline layout must be able to BIND everything the entry
        # point uses. Note the direction — layout ⊇ used, never layout == used.
        # worldgen:list binds simPL_'s 17 bindings and uses six; requiring
        # equality would fail every correct row in the table (barrier_graph
        # §2.5.2, §6.1).
        binds = LAYOUT_BINDINGS.get(layout)
        if binds is not None:
            for n in sorted((reads | writes) - binds):
                problems.append(
                    f"{fname}:{entry} (row '{r['name']}') uses binding '{n}', "
                    f"which {layout} cannot bind — the kernel is in a pass whose "
                    f"layout has no slot for it")

        # FAIL: a repeat span that reads indirect args nothing inside it writes.
        # barrier_graph §3.6 point 1 / §6.1: the CA loop's single global memory
        # barrier does NOT cover INDIRECT_COMMAND_READ, so its soundness rests
        # on nothing writing dispatchArgs between iterations.
        if r["repeat"] > 1:
            ind = {b for a, b in r["uses"] if a == "I"}
            wrt = {b for a, b in r["uses"] if a in WRITE_ACCS}
            for b in sorted(ind & wrt):
                problems.append(
                    f"row '{r['name']}' has repeat={r['repeat']} and both reads "
                    f"'{b}' as indirect args and writes it inside the span — the "
                    f"per-iteration global barrier does not cover "
                    f"INDIRECT_COMMAND_READ (barrier_graph §3.6)")

    # FAIL: an entry point in a sim shader that no row references is either dead
    # code or an untabled pass — and an untabled pass gets no barriers at all.
    for f in sorted(SHADERS.glob("*.wgsl")):
        if not (f.name.startswith("sim_") or f.name == "worldgen.wgsl"):
            continue
        mod = module_for(f.name)
        if not mod:
            continue
        _, _, entries = mod
        for e in sorted(entries):
            if (f.name, e) not in covered:
                problems.append(
                    f"{f.name}:{e} is a compute entry point that no pass_table.def "
                    f"row references — it is either dead code or a pass recorded "
                    f"outside the table, which would get no barriers at all")


def check_symbolic_pages():
    """DirtyIn and DirtyOut must never resolve to the same buffer.

    barrier_graph §4.1 [NEW EDGE]: a tick's dirtyOut fill would otherwise
    silently clobber a day/night wake-all, and the world would fail to wake at a
    phase boundary. The resolution lives in Simulation::PassBuffer.
    """
    txt = read(SIM)
    m = re.search(r"case B::DirtyIn:\s*return\s+([^;]+);", txt)
    n = re.search(r"case B::DirtyOut:\s*return\s+([^;]+);", txt)
    if not m or not n:
        problems.append(
            "Simulation::PassBuffer has no readable DirtyIn/DirtyOut cases — the "
            "symbolic page resolution cannot be checked")
        return
    a, b = m.group(1).strip(), n.group(1).strip()
    if a == b:
        problems.append(
            f"PassBuffer resolves DirtyIn and DirtyOut to the same expression "
            f"({a}) — a tick's dirtyOut fill would clobber a day/night wake-all")
    if "page_" not in a or "page_" not in b:
        problems.append(
            "PassBuffer resolves DirtyIn/DirtyOut without page_ — they are "
            "supposed to be symbolic (barrier_graph §2.2)")


def check_buffer_ids():
    """Every Buf enumerator the table uses must exist in pass_table.h."""
    hdr = read(HDR)
    m = re.search(r"enum class Buf\s*:\s*\w+\s*\{(.*?)\}", hdr, re.S)
    if not m:
        return
    known = set(re.findall(r"^\s*(\w+),", m.group(1), re.M))
    for r in parse_table():
        for _, buf in r["uses"]:
            if buf not in known:
                problems.append(
                    f"row '{r['name']}' uses buffer id '{buf}', which is not an "
                    f"enumerator of pass::Buf in src/sim/pass_table.h")


# The seven cases from barrier_graph §6.1 that a module-scope implementation
# fails. Each is a real in-tree entry point; the checker must be SILENT on all
# of them.
REGRESSIONS = [
    ("worldgen.wgsl", "far",
     ["voxels", "dirtyIn", "dirtyOut", "occupancy", "genList"], []),
    ("sim_particle.wgsl", "args1",
     ["voxels", "dirtyOut", "pRead", "pWrite", "claim", "spawnOps"], ["counts", "pArgs"]),
    ("sim_particle.wgsl", "args2",
     ["voxels", "dirtyOut", "pRead", "pWrite", "claim", "spawnOps"], ["counts", "pArgs"]),
    ("sim_compact.wgsl", "main", ["dirtyOut"], ["dirtyIn"]),
    ("sim_compact.wgsl", "mainNext", ["dirtyIn"], ["dirtyOut"]),
    ("sim_step.wgsl", "main", [], ["dirtyOut"]),      # transitive via markDirty
    ("worldgen.wgsl", "list", [],
     ["voxels", "occupancy", "dirtyIn", "dirtyOut"]),  # transitive via genChunk
]


def selfcheck():
    """Assert the walk itself behaves, independently of the table."""
    bad = []
    for fname, entry, must_exclude, must_include in REGRESSIONS:
        w = walk(fname, entry)
        if w is None:
            bad.append(f"{fname}:{entry}: could not walk")
            continue
        reads, writes, _ = w
        touched = reads | writes
        for n in must_exclude:
            if n in touched:
                bad.append(
                    f"{fname}:{entry}: walk INCLUDED '{n}', which is declared at "
                    f"module scope for a different entry point. The walk is "
                    f"module-scope, not rooted — every check degenerates.")
        for n in must_include:
            if n not in touched:
                bad.append(
                    f"{fname}:{entry}: walk MISSED '{n}', which the entry point "
                    f"reaches transitively. The walk is body-local, not "
                    f"transitive.")
    if bad:
        print("pass-table walk SELFCHECK FAILED:\n", file=sys.stderr)
        for b in bad:
            print(f"  - {b}", file=sys.stderr)
        return 1
    print(f"pass-table walk selfcheck OK "
          f"({len(REGRESSIONS)} rooted/transitive regression cases silent)")
    return 0


RELEVANT = ("pass_table.def", "pass_table.h", "pass_table.cpp", "simulation.cpp",
            ".wgsl")

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if "--selfcheck" in sys.argv:
        sys.exit(selfcheck())
    if args:
        norm = [a.replace("\\", "/") for a in args]
        if not any(n.endswith(r) for n in norm for r in RELEVANT):
            sys.exit(0)  # edited file cannot break this pair

    check_buffer_ids()
    check_table_vs_wgsl()
    check_symbolic_pages()

    for w in warnings:
        print(f"pass-table WARN: {w}", file=sys.stderr)

    if problems:
        print("\npass table check FAILED — src/sim/pass_table.def and the WGSL "
              "it describes do not agree:\n", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("\nA missing read or write here is a MISSING VULKAN BARRIER in "
              "phase 3 (docs/vulkan_barrier_graph.md §6.1), which is silent "
              "under Dawn.", file=sys.stderr)
        sys.exit(1)

    if not args:
        print("pass table OK (rows agree with the WGSL bindings their entry "
              "points reach)")
