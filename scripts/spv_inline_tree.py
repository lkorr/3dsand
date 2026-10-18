#!/usr/bin/env python3
"""spv_inline_tree.py -- where an entry point's INLINED size comes from.

Every call site of a function is a full copy once spirv-opt's
inline-exhaustive pass (the engine's worldgen recipe, vk_spirv.cpp) and the
driver's own inliner are done with it. This walks a Tint SPIR-V module's call
graph and computes, for each function, its inlined size: its own instruction
count plus the inlined size of every call it makes. It then prints, for one
entry point, the call sites ranked by what they contribute (a function called
from N places is charged N times -- that is the point), and the per-callee
totals across the whole expansion.

  python scripts/spv_inline_tree.py module.spv <entry> [--depth N] [--top N]

The numbers are the PRE-simplification expansion: dead branches a constant
argument folds away still count. Confirm a lever by stubbing it and measuring
with scripts/spv_sizes.sh, which runs the real recipe.
"""
import struct, sys, argparse
from collections import defaultdict

def load(path):
    b = open(path, 'rb').read()
    w = struct.unpack('<%dI' % (len(b) // 4), b)
    names, entries = {}, {}
    funcs = {}            # id -> dict(own=int, calls=[callee ids])
    cur = None
    i = 5
    while i < len(w):
        op = w[i] & 0xFFFF
        n = w[i] >> 16
        if n == 0: break
        ins = w[i:i + n]
        if op == 5:   # OpName
            s = b''.join(struct.pack('<I', x) for x in ins[2:]).split(b'\0')[0].decode()
            names[ins[1]] = s
        elif op == 15:  # OpEntryPoint
            s = b''.join(struct.pack('<I', x) for x in ins[3:]).split(b'\0')[0].decode()
            entries[s] = ins[2]
        elif op == 54:  # OpFunction
            cur = ins[2]
            funcs[cur] = {'own': 0, 'calls': []}
        if cur is not None:
            funcs[cur]['own'] += 1
            if op == 57:  # OpFunctionCall
                funcs[cur]['calls'].append(ins[3])
        if op == 56:
            cur = None
        i += n
    return names, entries, funcs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('spv'); ap.add_argument('entry')
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--min', type=int, default=2000)
    a = ap.parse_args()
    names, entries, funcs = load(a.spv)
    nm = lambda f: names.get(f, '%' + str(f))
    memo = {}
    def size(f):
        if f in memo: return memo[f]
        memo[f] = 0
        s = funcs[f]['own'] + sum(size(c) for c in funcs[f]['calls'])
        memo[f] = s
        return s
    ep = entries.get(a.entry)
    if ep is None:
        sys.exit('no entry %s; have %s' % (a.entry, ', '.join(entries)))
    total = size(ep)
    print('%s: inlined expansion %d instructions (own %d)' % (a.entry, total, funcs[ep]['own']))
    # per-callee totals across the whole expansion: copies x inlined size
    copies = defaultdict(int)
    def walk(f, mult):
        for c in funcs[f]['calls']:
            copies[c] += mult
            walk(c, mult)
    # counting copies by DFS can be exponential; do it by topological propagation instead
    order, seen = [], set()
    def topo(f):
        if f in seen: return
        seen.add(f)
        for c in funcs[f]['calls']: topo(c)
        order.append(f)
    topo(ep)
    mult = defaultdict(int); mult[ep] = 1
    for f in reversed(order):
        for c in funcs[f]['calls']:
            mult[c] += mult[f]
    rows = sorted(((mult[f] * size(f), mult[f], size(f), nm(f)) for f in order if f != ep), reverse=True)
    print('\nper-function: copies x inlined size (a callee nested in another is counted in both)')
    for tot, m, s, n in rows[:a.top * 2]:
        print('  %8d  = %4d x %7d  %s' % (tot, m, s, n))
    print('\ncall tree (sites >= %d instructions):' % a.min)
    def tree(f, d, ind):
        agg = defaultdict(int)
        for c in funcs[f]['calls']: agg[c] += 1
        for c, k in sorted(agg.items(), key=lambda kv: -kv[1] * size(kv[0])):
            if k * size(c) < a.min: continue
            print('%s%8d  %s%s' % (ind, k * size(c), nm(c), ' x%d' % k if k > 1 else ''))
            if d + 1 < a.depth: tree(c, d + 1, ind + '  ')
    tree(ep, 0, '  ')

main()
