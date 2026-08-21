#!/usr/bin/env python3
"""One-shot companion to split_selftest.py: adapt the moved gate bodies.

Deliberately CONSERVATIVE. The moved bodies keep every one of their original
printf calls, including the ones nested sub-gates emit ("mob gait: PASS ...",
"body blast: PASS ..."), because those lines are the gate's real diagnostic
output and rewriting them mechanically is how you silently lose a number.

So this does exactly two things:

  1. Binds the names the old body used as function PARAMETERS (ctx, world, sim,
     mats, phys, debris, mobs, stream, items) plus the render scaffolding
     (W, H, offscreen, view, grab) to the shared Ctx, as references at the top
     of the gate. The bodies stay byte-identical below that, so they remain
     diffable against git history.

  2. Appends `return <okvar> ? Status::Pass : Status::Fail;` at the end of each
     gate, naming the flag that gate's final verdict already computed. The
     mapping is explicit below rather than inferred — guessing which of a dozen
     bools is the verdict is precisely the kind of thing that produces a gate
     that always passes.

Not part of the build; kept as the record of how the split was performed.
"""
import re
from pathlib import Path

OUT = Path("src/test")

# gate function -> the bool holding its verdict at the end of the moved body.
VERDICT = {
    "GateDeterminism": "deterministic",
    "GateSleep": "sleepOk",
    "GatePondFreeze": "pondOk",
    "GateEvaporation": "evapOk",
    "GateBloodStain": "stainOk",
    "GateFlungLiquid": "fullOk",
    "GateFarFog": "fogOk",
    "GateFarDownsample": "farDownOk",
    "GateScreenshots": None,      # writes BMPs; nothing to assert
    "GatePlayerWalk": "walkOk",
    "GateDebris": "debrisOk",
    "GatePrefab": "prefabOk",
    "GateMob": "mobOk",
    "GateSettleBack": "settleOk",
    "GatePlayerBody": "pushOk",
    "GateSaveLoad": "saveOk",
    "GateRegionStore": "storeOk",
    "GateStreaming": "streamOk",
    "GateSpells": "spellOk",
    "GatePerf": "perfOk",
}

BINDINGS = [
    ("ctx", "GpuContext& ctx = c.ctx;"),
    ("world", "World& world = c.world;"),
    ("sim", "Simulation& sim = c.sim;"),
    ("mats", "const std::vector<MaterialDef>& mats = c.mats;"),
    ("phys", "Physics& phys = c.phys;"),
    ("debris", "DebrisSystem& debris = c.debris;"),
    ("mobs", "MobSystem& mobs = c.mobs;"),
    ("stream", "Stream& stream = c.stream;"),
    ("items", "const ItemLibrary& items = c.items;"),
    ("W", "const uint32_t W = c.width;"),
    ("H", "const uint32_t H = c.height;"),
    ("offscreen", "wgpu::Texture& offscreen = c.offscreen;"),
    ("view", "wgpu::TextureView& view = c.view;"),
]


def func_span(text, start):
    """Return (open_brace_idx, close_brace_idx) of the function body at start."""
    i = text.index("{", start)
    d, j = 0, i
    while j < len(text):
        if text[j] == "{":
            d += 1
        elif text[j] == "}":
            d -= 1
            if d == 0:
                return i, j
        j += 1
    raise SystemExit("unbalanced braces")


for f in sorted(OUT.glob("selftest_*.cpp")):
    t = f.read_text(encoding="utf-8")
    # Walk gates from the END so earlier indices stay valid as we splice.
    hits = list(re.finditer(r"Status (Gate\w+)\(Ctx& c, std::string& detail\) \{", t))
    for m in reversed(hits):
        name = m.group(1)
        i, j = func_span(t, m.start())
        body = t[i + 1:j]

        verdict = VERDICT.get(name, None)
        tail = ""
        if verdict:
            tail = (f"\n  // Verdict: the flag the moved body already computed.\n"
                    f"  return {verdict} ? Status::Pass : Status::Fail;\n")
        else:
            tail = "\n  return Status::Pass;\n"

        binds = [d for n, d in BINDINGS if re.search(r"\b" + re.escape(n) + r"\b", body)]
        # `grab` is a lambda in the render gate; route it at the Ctx.
        if re.search(r"\bgrab\(", body) and "auto grab" not in body:
            binds.append("auto grab = [&](const char* p) { c.Grab(p); };")
        head = ("\n" + "\n".join("  " + b for b in binds)) if binds else ""

        t = t[:i + 1] + head + body + tail + t[j:]
    f.write_text(t, encoding="utf-8", newline="\n")
    print("adapted", f)
