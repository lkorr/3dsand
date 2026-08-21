#!/usr/bin/env python3
"""One-shot: slice the old RunSelftest body into per-domain gate files.

Kept in the tree because it documents EXACTLY how the split was made — which
source range became which gate, and which locals had to be promoted to the
shared Ctx. If a gate later looks wrong, this is the map back to the original.

Not part of the build. Input is the extracted RunSelftest body.
"""
import re
import sys
from pathlib import Path

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else "/tmp/runselftest.cpp")
OUT = Path("src/test")

lines = SRC.read_text(encoding="utf-8").split("\n")


def block(a, b):
    """Lines a..b inclusive, 1-indexed, dedented by one level (2 spaces)."""
    out = []
    for l in lines[a - 1:b]:
        out.append(l[2:] if l.startswith("  ") else l)
    return "\n".join(out)


# (gate name, group, deps, advisory, first_line, last_line)
# Ranges are the depth-1 blocks plus the ok-flag declaration and printf that
# bracket them. Verified against the depth map before slicing.
GATES = [
    ("determinism", "sim",   [],              False,   9,   33),
    ("sleep",       "sim",   [],              False,  35,  141),
    ("pond-freeze", "sim",   [],              False, 143,  286),
    ("evaporation", "sim",   [],              False, 288,  408),
    ("blood-stain", "sim",   [],              False, 410,  553),
    ("flung-liquid","sim",   [],              False, 555,  644),
    ("far-fog",     "render",[],              False, 646,  702),
    ("far-downsample","render",[],            False, 704,  772),
    ("screenshots", "render",[],              False, 774,  895),
    ("player-walk", "player",[],              False, 897, 1019),
    ("debris",      "phys",  [],              False, 1021, 1124),
    ("prefab",      "sim",   [],              False, 1126, 1190),
    ("mob",         "mob",   [],              False, 1192, 2603),
    ("settle-back", "phys",  [],              False, 2605, 2843),
    ("player-body", "phys",  [],              False, 2845, 2899),
    ("save-load",   "worldio",[],             False, 2901, 2927),
    ("region-store","worldio",[],             False, 2929, 2955),
    ("streaming",   "worldio",[],             False, 2957, 3062),
    ("spells",      "spell", ["streaming"],   False, 3064, 3284),
    ("perf",        "sim",   ["screenshots"], True,  3286, 3288),
]

GROUP_FILE = {
    "sim": "selftest_sim.cpp",
    "render": "selftest_render.cpp",
    "player": "selftest_player.cpp",
    "phys": "selftest_phys.cpp",
    "mob": "selftest_mob.cpp",
    "worldio": "selftest_worldio.cpp",
    "spell": "selftest_spell.cpp",
}

REGISTRY_FN = {
    "sim": "SimGates", "render": "RenderGates", "player": "PlayerGates",
    "phys": "BodyGates", "mob": "MobGates", "worldio": "WorldIoGates",
    "spell": "SpellGates",
}

HEADER = """// {fname} — {group} selftest gates.
//
// Bodies moved verbatim out of the old monolithic RunSelftest; see
// scripts/split_selftest.py for the exact source ranges. Each gate returns a
// Status and fills `detail` with the parenthetical the old printf carried, so
// the console output is unchanged and --json can carry the same numbers.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {{
namespace {{
"""


def fn_name(gate):
    return "Gate" + "".join(p.capitalize() for p in re.split(r"[-_]", gate))


by_group = {}
for name, group, deps, advisory, a, b in GATES:
    by_group.setdefault(group, []).append((name, deps, advisory, a, b))

for group, gates in by_group.items():
    fname = GROUP_FILE[group]
    parts = [HEADER.format(fname=fname, group=group)]
    for name, deps, advisory, a, b in gates:
        parts.append(f"\n// ---- {name} " + "-" * (66 - len(name)) + "\n")
        parts.append(f"Status {fn_name(name)}(Ctx& c, std::string& detail) {{\n")
        parts.append(block(a, b))
        parts.append("\n}\n")
    parts.append("\n}  // namespace\n\n")
    parts.append(f"const std::vector<Gate>& {REGISTRY_FN[group]}() {{\n")
    parts.append("  static const std::vector<Gate> g = {\n")
    for name, deps, advisory, a, b in gates:
        d = ", ".join(f'"{x}"' for x in deps)
        parts.append(
            f'      {{"{name}", "{group}", {{{d}}}, {"true" if advisory else "false"}, '
            f"{fn_name(name)}}},\n")
    parts.append("  };\n  return g;\n}\n\n}  // namespace selftest\n")
    (OUT / fname).write_text("".join(parts), encoding="utf-8", newline="\n")
    print("wrote", OUT / fname)
