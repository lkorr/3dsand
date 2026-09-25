#!/usr/bin/env python3
"""Read src/sim/tuning_params.def -- the one tuning table -- from Python.

The C++ side expands the table as an X-macro (tuning.h defaults, LoadTuning's
generated reads, TuningWgslBlock, SetTuningField, the tuning-reach gate).
Every Python consumer goes through THIS parser instead of keeping a copy:

  scripts/tuning_prelude.py      the WGSL prelude for check_shaders.sh
  scripts/gen_tuning_params.py   assets/tuning_params.js for the tuner
  scripts/check_invariants.py    the `tuning` / `tuning reach` checks

A TP_ line this parser cannot read is an ERROR, not a skipped row: a row the
Python side silently drops is exactly the drift the table exists to end.

Row forms (see the .def header):
  TP_F / TP_I / TP_U (group, member, WGSL, default, min, max)
  TP_B / TP_S        (group, member, WGSL, default)
  TP_V3              (group, member, WGSL, x, y, z)
min/max are a literal, a world.h constant (kFarLevels), or TP_OPEN.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEF = ROOT / "src" / "sim" / "tuning_params.def"
WORLD_H = ROOT / "src" / "sim" / "world.h"
TUNING_JSON = ROOT / "assets" / "materials" / "tuning.json"

NO_WGSL = "NO_WGSL"
ARITY = {"F": 6, "I": 6, "U": 6, "B": 4, "S": 4, "V3": 6}
_ROW = re.compile(r"^TP_(F|I|U|B|S|V3)\((.*)\)\s*$")


class DefError(Exception):
    pass


class Row:
    __slots__ = ("kind", "group", "member", "wgsl", "default", "lo", "hi",
                 "lo_tok", "hi_tok", "default_tok", "line")

    @property
    def key(self):
        return f"{self.group}.{self.member}"

    @property
    def has_wgsl(self):
        return self.wgsl != NO_WGSL


def _split(args):
    out, depth, cur, instr = [], 0, "", False
    for i, c in enumerate(args):
        if c == '"' and (i == 0 or args[i - 1] != "\\"):
            instr = not instr
        if not instr:
            if c in "({[":
                depth += 1
            elif c in ")}]":
                depth -= 1
            elif c == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
                continue
        cur += c
    out.append(cur.strip())
    return out


_WC = None


def world_consts():
    """kName -> value for the constexpr integer/float constants in world.h, so
    a row can bound itself by the constant the kernel is sized for."""
    global _WC
    if _WC is not None:
        return _WC
    txt = WORLD_H.read_text(encoding="utf-8") if WORLD_H.exists() else ""
    pend = dict(re.findall(
        r"constexpr\s+(?:u?int\d+_t|int|unsigned|float|double)\s+(k\w+)\s*=\s*"
        r"([^;]+);", txt))
    env = {}
    for _ in range(32):
        progress = False
        for k, e in list(pend.items()):
            ex = re.sub(r"\b(\d+)[uUlL]+\b", r"\1", e)
            ex = re.sub(r"\b(\d+\.\d*|\.\d+)[fF]\b", r"\1", ex)
            ex = re.sub(r"\((?:u?int\d+_t|int|float|double|unsigned)\)", "", ex)
            if "." not in ex:
                ex = ex.replace("/", "//")
            try:
                env[k] = eval(ex, {"__builtins__": {}}, dict(env))
            except Exception:
                continue
            del pend[k]
            progress = True
        if not progress:
            break
    _WC = env
    return env


def num(tok, line=None):
    """A C++ numeric literal (or world.h constant) -> float/int; TP_OPEN -> None."""
    t = tok.strip()
    if t == "TP_OPEN":
        return None
    t = re.sub(r"^\((?:int|float)\)", "", t)
    if re.fullmatch(r"k\w+", t):
        wc = world_consts()
        if t not in wc:
            raise DefError(f"tuning_params.def:{line}: {t} is not a world.h "
                           f"constant this parser can evaluate")
        return wc[t]
    t = re.sub(r"(?<=[0-9.])[fF]$", "", t)
    try:
        return float(t) if re.search(r"[.eE]", t) else int(t)
    except ValueError:
        raise DefError(f"tuning_params.def:{line}: `{tok}` is not a number")


def rows(text=None):
    """Every row, in file order. Raises DefError on a line it cannot read."""
    if text is None:
        text = DEF.read_text(encoding="utf-8")
    out = []
    for n, raw in enumerate(text.splitlines(), 1):
        ln = raw.strip()
        if not ln.startswith("TP_"):
            continue
        m = _ROW.match(ln)
        if not m:
            raise DefError(f"tuning_params.def:{n}: cannot parse `{ln}` "
                           f"(a trailing comment on a row?)")
        kind, args = m.group(1), _split(m.group(2))
        if len(args) != ARITY[kind]:
            raise DefError(f"tuning_params.def:{n}: TP_{kind} takes "
                           f"{ARITY[kind]} fields, found {len(args)}")
        r = Row()
        r.kind, r.group, r.member, r.wgsl = kind, args[0], args[1], args[2]
        r.line = n
        r.lo = r.hi = None
        r.lo_tok = r.hi_tok = "TP_OPEN"
        if kind in ("F", "I", "U"):
            r.default_tok = args[3]
            r.default = num(args[3], n)
            r.lo_tok, r.hi_tok = args[4], args[5]
            r.lo, r.hi = num(args[4], n), num(args[5], n)
        elif kind == "V3":
            r.default_tok = ", ".join(args[3:6])
            r.default = [float(num(a, n)) for a in args[3:6]]
        elif kind == "B":
            if args[3] not in ("true", "false"):
                raise DefError(f"tuning_params.def:{n}: TP_B default must be "
                               f"true or false")
            r.default_tok = args[3]
            r.default = args[3] == "true"
        else:
            if not (args[3].startswith('"') and args[3].endswith('"')):
                raise DefError(f"tuning_params.def:{n}: TP_S default must be "
                               f"a string literal")
            r.default_tok = args[3]
            r.default = json.loads(args[3])
        out.append(r)
    return out


def clamp(r, v):
    """What LoadTuning's generated clamp does to a value of row r."""
    if r.lo is not None and v < r.lo:
        return r.lo
    if r.hi is not None and v > r.hi:
        return r.hi
    return v
