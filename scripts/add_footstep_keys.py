#!/usr/bin/env python3
"""One-shot: add "footstep" keys to assets/materials/materials.json.

Inserts a `"footstep": "<set>"` line directly after each named material's
`"id"` line, preserving the file's hand-maintained formatting (a full
json.dump round-trip would reflow every array and produce an unreadable diff).

Idempotent: a material that already has a footstep key is left alone. Safe to
re-run after adding new materials.

The mapping below is the AUTHORED half. Materials absent from it fall back by
tag in src/audio/cues.cpp (FallbackFootstep), which is why liquids and gases
are simply not listed rather than being given an empty value.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PATH = os.path.join(ROOT, "assets", "materials", "materials.json")

# material id -> footstep set (a folder under assets/sounds/footsteps/).
#
# Only four sets exist today, so several surfaces share one. That is the honest
# state and it is visible here rather than hidden in code: when a "grass" or
# "snow" set is recorded, this table is the one place that changes.
MAPPING = {
    # --- stone-like: bright, hard, ringing ---
    "stone": "path",
    "gravel": "path",
    "glass": "path",
    "ice": "path",
    "bone": "path",
    # --- loose ground: the default walking surface ---
    "sand": "path",
    "dirt": "path",
    "ash": "path",
    "snow": "path",
    "grass": "path",
    # --- foliage: soft, rustling ---
    "leaves": "leaf",
    "pine_needles": "leaf",
    "autumn_leaves": "leaf",
    "leaf_green": "leaf",
    "grass_tuft": "leaf",
    "foliage_bush": "leaf",
    "plant": "leaf",
    # --- wood: dry snap underfoot ---
    "wood": "branch",
    "birch_wood": "branch",
    "staff_wood": "branch",
}


def main():
    with open(PATH, "r", encoding="utf-8") as f:
        lines = f.readlines()

    id_re = re.compile(r'^(\s*)"id":\s*"([^"]+)"\s*,?\s*$')
    out = []
    added = 0
    skipped = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        m = id_re.match(line)
        if m:
            indent, mid = m.group(1), m.group(2)
            if mid in MAPPING:
                # Look ahead to the end of this object for an existing key.
                depth = 0
                has = False
                for j in range(i, min(i + 60, len(lines))):
                    if '"footstep"' in lines[j]:
                        has = True
                        break
                    depth += lines[j].count("{") - lines[j].count("}")
                    if j > i and depth <= 0:
                        break
                if has:
                    skipped += 1
                else:
                    out.append('%s"footstep": "%s",\n' % (indent, MAPPING[mid]))
                    added += 1
        i += 1

    with open(PATH, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)

    print("added %d footstep keys (%d already present)" % (added, skipped))
    # Validate: a broken materials.json breaks the whole game, so never leave
    # this script's output unparsed.
    import json
    with open(PATH, "r", encoding="utf-8") as f:
        d = json.load(f)
    n = sum(1 for m in d["materials"] if m.get("footstep"))
    print("materials.json parses; %d materials carry a footstep set" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
