#!/usr/bin/env python3
"""Reference interpreter for the magic grammar (docs/PLAN_magic_grammar.md).

Executable spec: the six parse rules and the lowering are written here once,
and docs/MAGIC_PERMUTATIONS.md is GENERATED from it, so every row of the
permutation tables is derived from the same rules rather than hand-written.
Later this is the oracle the C++ `spells` gate compares against.

  python scripts/magic_grammar.py                 # regenerate the doc
  python scripts/magic_grammar.py --oracle        # assets/spells/grammar_oracle.json for the C++ gate
  python scripts/magic_grammar.py fire trail explosive shotgun shotgun projectile
"""
import itertools
import sys
from collections import OrderedDict

# --------------------------------------------------------------------------
# The glyph table. Sorts: M matter, E effect, D delivery, X mod, Op operator.
# repeat: 'add' (xN of the axis) or 'compose' (applied N times: exponential).
# --------------------------------------------------------------------------
G = OrderedDict()


def glyph(id, sort, word, desc, **kw):
    d = dict(id=id, sort=sort, word=word, desc=desc, left=None, right=None,
             result=None, repeat='add', axis='')
    d.update(kw)
    G[id] = d


# ---- matter (arcane value is the per-voxel tariff base) -------------------
MATTER = [
    ('air', 0, 'Nothing. The word for empty space. Sprayed it does nothing; as a target it unmakes; as a source it conjures.'),
    ('water', 1, 'The word for water.'),
    ('sand', 1, 'Loose grains. The cheapest matter there is.'),
    ('dirt', 1, 'Soil.'),
    ('stone', 2, 'Rock.'),
    ('wood', 2, 'Timber. Burns.'),
    ('fire', 3, 'Flame. Sprayed it ignites what it lands on.'),
    ('blood', 3, 'Blood. The cheapest thing a body can be mended with.'),
    ('flesh', 4, 'Meat.'),
    ('bone', 5, 'Bone.'),
    ('acid', 6, 'Eats stone and meat alike.'),
    ('lava', 8, 'Molten stone.'),
    ('steel', 12, 'Does not burn, does not dissolve. Dear.'),
    ('gold', 40, 'The dearest common matter. Making it is what kills alchemists.'),
]
for mid, val, desc in MATTER:
    glyph(mid, 'M', 1, desc, arcane=val, axis='voxels (spray) / volume share')
glyph('anything', 'M', 2,
      'The wildcard. Matches whatever is actually there. DANGEROUS: it is priced as the '
      'matter it turns out to be, plus a surcharge, and billed when it resolves - you do '
      'not know the price until it lands. The surcharge is a knob (anythingSurchargeMille, +50% to start).',
      arcane='actual × surcharge', axis='voxels')

# ---- nullary effects --------------------------------------------------------
glyph('explosive', 'E', 6, 'An explosion at the point. Power adds with repetition; radius follows the engine law; priced by volume.',
      verb='explode', axis='power', tariff='power x r^3')
glyph('gust', 'E', 5, 'A wind jet from the point along the aim. Repetition doubles speed, not size.',
      verb='wind', axis='speed', tariff='footprint x ticks')
glyph('implode', 'E', 5, 'A vacuum burst: pulls loose matter and bodies toward the point.',
      verb='wind(-)', axis='speed', tariff='footprint x ticks')
glyph('spark', 'E', 2, 'One ember. Ignites a flammable cell it touches, nothing more. The cheap way to light things.',
      verb='place(fire,1)', axis='embers', tariff='1 voxel of fire')

# ---- operators ----------------------------------------------------------------
glyph('transmute', 'Op', 4, 'A transmute B: turns A into B where it resolves. Needs BOTH words; an empty side fizzles (charged).',
      left={'M'}, right={'M'}, result='E', verb='convert', axis='volume',
      tariff='volume x (convert + max(0, value(B)-value(A)))')
glyph('mend', 'Op', 6, 'M mend: draws matter M from the resolve point into the caster\'s missing anatomy cells, one voxel at a time. Needs a source word.',
      left={'M'}, right=None, result='E', verb='graft', axis='voxels per tick',
      tariff='per voxel: value(M) x graft + foreign penalty')
glyph('trail', 'Op', 8, 'E trail: runs E at every marked voxel of the flight path. Needs a left word (matter is sprayed).',
      left={'E', 'M'}, right=None, result='X', verb='trail', axis='budget',
      tariff='budget voxels x E')
glyph('aura', 'Op', 7, 'E aura: SUSTAINS E (or a mod) on the body at the resolve point - or on the place, if no body is there. Runs every tick, billed per tick to the caster and reserved out of their max mana, until the caster drops it or runs dry. `aura self` is a ward on yourself; `aura projectile` puts the same thing on whoever the bolt hits.',
      left={'E', 'M', 'X'}, right=None, result='E', verb='sustain', axis='attach radius',
      tariff='E per tick, sustained')
glyph('null', 'Op', 5, 'W null: a filter that refuses incoming ops of W\'s kind within the radius. Takes the word before it raw.',
      left={'any'}, right=None, result='E', verb='filter', axis='radius',
      tariff='radius^3 per tick')
glyph('echo', 'Op', 6, 'E echo: E repeats at the point every few ticks for a bounded span.',
      left={'E', 'M'}, right=None, result='E', verb='repeat', axis='repeats',
      tariff='repeats x E')

# ---- deliveries ---------------------------------------------------------------
DELIV = [
    ('projectile', 6, 3000, 'flight', 'A bolt from the hand. Medium speed, resolves on impact.'),
    ('bolt', 8, 4000, 'flight', 'Fast and light: twice the speed, half the kinetic impact.'),
    ('lob', 4, 2000, 'flight', 'Slow, arcing, heavy. Resolves on impact.'),
    ('orb', 7, 2500, 'flight', 'Slow, weightless, lingers; resolves on impact or when its life runs out.'),
    ('bomb', 3, 1000, 'flight', 'A dropped rigid body with a fuse. Resolves where it comes to rest when the fuse runs out. The cheapest carrier.'),
    ('beam', 5, 1500, 'continuous', 'Held: resolves at the ray hit every tick, billed per tick.'),
    ('self', 2, 1000, 'instant', 'At the caster. From the character screen, at the chosen body part.'),
]
for did, word, carry, mech, desc in DELIV:
    glyph(did, 'D', word, desc, carry=carry, mech=mech, axis='weight (speed, life, kinetic impact)')

# ---- mods (field edits on the delivery record; repeat = compose) -----------
MODS = [
    ('shotgun', 5, 'count x3 (3, 9, 27 ...), fanned', 'Three of it, fanned. Said again: nine. Everything behind it is paid that many times.'),
    ('split', 6, 'children x2 on resolve, gen+1', 'On resolve, two children with the same payload. Generation-capped.'),
    ('float', 4, 'gravity -1g', 'Anti-gravity. Twice: gravity reversed. On a body-anchored delivery it lifts the body.'),
    ('heavy', 3, 'gravity +1g', 'Falls harder, hits harder.'),
    ('swift', 4, 'speed x2', 'Faster.'),
    ('slow', 2, 'speed /2', 'Slower. A slow explosive bolt is a mine that drifts.'),
    ('long', 3, 'life x2', 'Lives longer, reaches further; a bomb\'s fuse is longer.'),
    ('wide', 5, 'resolve radius x2', 'Everything resolves over a wider radius. Priced by volume, so x8 per word.'),
    ('bounce', 4, 'bounces +1', 'Reflects off what it hits once more before resolving.'),
    ('pierce', 5, 'pierce +1', 'Passes through one body or thin wall before resolving.'),
    ('seek', 8, 'homing +1', 'Turns toward the nearest body.'),
    ('fuse', 3, 'delay +30 ticks', 'Waits after impact before resolving; on a bomb, a longer fuse.'),
]
for mid, word, field, desc in MODS:
    glyph(mid, 'X', word, desc, field=field, repeat='compose', axis=field)

HAND = dict(id='hand', carry=1000, mech='instant', word=0,
            desc='Implicit. At reach: a few voxels in front of the caster along the aim.')

MAX_MULT = 6

# --------------------------------------------------------------------------
# Parse
# --------------------------------------------------------------------------


def sort_of(item):
    if item['kind'] == 'raw':
        return G[item['g']]['sort']
    return G[item['op']]['result']


def accepts(slot, item):
    if slot is None:
        return False
    if 'any' in slot:
        return True
    return sort_of(item) in slot


def merge_runs(ids):
    items = []
    for gid in ids:
        if items and items[-1]['kind'] == 'raw' and items[-1]['g'] == gid:
            items[-1]['n'] = min(items[-1]['n'] + 1, MAX_MULT)
        else:
            items.append(dict(kind='raw', g=gid, n=1))
    return items


def bind(items):
    """R2/R3: operators bind greedily on their declared side, left to right.
    An operator whose required slot is empty is INCOMPLETE: it stays as a
    charged fizzle of its result sort."""
    out = []
    i = 0
    while i < len(items):
        it = items[i]
        if it['kind'] == 'raw' and G[it['g']]['sort'] == 'Op':
            op = G[it['g']]
            left = right = None
            if op['left'] and out and accepts(op['left'], out[-1]):
                left = out.pop()
            if op['right'] and i + 1 < len(items) and accepts(op['right'], items[i + 1]):
                right = items[i + 1]
                i += 1
            complete = (op['left'] is None or left is not None) and \
                       (op['right'] is None or right is not None)
            out.append(dict(kind='group', op=it['g'], n=it['n'], left=left,
                            right=right, complete=complete))
        else:
            out.append(it)
        i += 1
    return out


def split_clauses(items):
    clauses, cur = [], []
    for it in items:
        cur.append(it)
        if it['kind'] == 'raw' and G[it['g']]['sort'] == 'D':
            clauses.append(cur)
            cur = []
    if cur:
        clauses.append(cur)   # headless -> hand
    return clauses


def key(item):
    if item['kind'] == 'raw':
        return item['g']
    return '(%s|%s|%s)' % (key(item['left']) if item['left'] else '',
                            item['op'], key(item['right']) if item['right'] else '')


def parse(ids):
    """Returns a list of clauses: dict(delivery, dn, payload, mods, fizzles)."""
    clauses = []
    for cl in split_clauses(bind(merge_runs(ids))):
        head = cl[-1] if cl[-1]['kind'] == 'raw' and G[cl[-1]['g']]['sort'] == 'D' else None
        bag = cl[:-1] if head else cl
        # R6: bag is a set; identical items merge (sum n, capped)
        merged = OrderedDict()
        for it in bag:
            k = key(it)
            if k in merged:
                merged[k]['n'] = min(merged[k]['n'] + it['n'], MAX_MULT)
            else:
                merged[k] = dict(it)
        payload, mods = [], []
        for it in merged.values():
            s = sort_of(it)
            if s in ('E', 'M'):
                payload.append(it)
            elif s == 'X':
                mods.append(it)
        clauses.append(dict(delivery=head['g'] if head else 'hand',
                            dn=head['n'] if head else 1,
                            payload=payload, mods=mods, bag=list(merged.values())))
    return clauses

# --------------------------------------------------------------------------
# Describe
# --------------------------------------------------------------------------


def x(n):
    return '' if n == 1 else '×%d' % n


def show(item):
    if item['kind'] == 'raw':
        return item['g'] + x(item['n'])
    op = G[item['op']]
    l = show(item['left']) if item['left'] else ('_' if op['left'] else '')
    r = show(item['right']) if item['right'] else ('_' if op['right'] else '')
    sym = {'transmute': '⋈', 'mend': '◂mend', 'trail': '◂trail',
           'aura': '◂aura', 'null': '◂null', 'echo': '◂echo'}[item['op']]
    inner = ' '.join(p for p in (l, sym, r) if p)
    return '(%s)%s' % (inner, x(item['n']))


def bracket(ids):
    cls = parse(ids)
    parts = []
    for c in cls:
        s = ' '.join(show(i) for i in c['bag'])
        if c['delivery'] != 'hand':
            s = (s + ' ' if s else '') + '**' + c['delivery'] + x(c['dn']) + '**'
        parts.append(s or '(empty)')
    return ' ‖ '.join(parts)


def matter_name(item):
    return item['g'] if item['kind'] == 'raw' else None


def desc_effect(item, at):
    """What one payload item does at a point `at`."""
    n = item['n']
    if item['kind'] == 'raw':
        g = G[item['g']]
        if g['sort'] == 'M':
            m = item['g']
            if m == 'air':
                return 'throws nothing (charged the word)'
            if m == 'anything':
                return 'scoops whatever is at %s and throws it (priced as that matter × the anything surcharge, billed on resolve)%s' % (at, x(n))
            return 'throws %d voxels of %s' % (3 * n, m)
        return {
            'explosive': 'explosion, power%s, radius by the engine law' % (x(n) or '×1'),
            'gust': 'wind jet along the aim, speed%s' % (x(n) or '×1'),
            'implode': 'vacuum burst pulling loose matter in, speed%s' % (x(n) or '×1'),
            'spark': '%d ember(s); ignites a flammable cell' % n,
        }[item['g']]
    op = item['op']
    if not item['complete']:
        return '%s with a missing word: fizzles, the word is charged' % op
    if op == 'transmute':
        a, b = item['left']['g'], item['right']['g']
        vol = 'volume%s' % x(n)
        if a == b:
            return 'converts %s to itself: no change, charged' % a
        if a == 'air' and b == 'anything':
            return 'conjures "whatever": undefined source, fizzles, charged'
        if a == 'air':
            return 'conjures %s into empty space (%s), priced at %s\'s full value' % (b, vol, b)
        if b == 'air':
            return ('unmakes whatever is there (%s): convert base PLUS value(actual) × the anything surcharge, billed on resolve' % vol) if a == 'anything' else ('unmakes %s (%s), cheap: value goes down' % (a, vol))
        if a == 'anything' and b == 'anything':
            return 'converts whatever is there into itself: no change, charged +25% of its value'
        if a == 'anything':
            return 'converts whatever is there into %s (%s); priced as convert(actual→%s) PLUS value(actual) × the anything surcharge, billed on resolve' % (b, vol, b)
        if b == 'anything':
            return 'converts %s into whatever is beside it (%s); priced by the actual result +25%%' % (a, vol)
        gap = G[b]['arcane'] - G[a]['arcane']
        price = 'cheap (value falls %d)' % -gap if gap < 0 else ('base only' if gap == 0 else 'value gap +%d per voxel' % gap)
        return 'converts %s into %s (%s), %s' % (a, b, vol, price)
    if op == 'mend':
        m = item['left']['g']
        src = 'whatever matter is there' if m == 'anything' else m
        if m == 'air':
            return 'mends from nothing: fizzles, charged'
        pen = '' if m in ('blood', 'flesh', 'bone') else '; the new cells ARE %s (burn/dissolve/resist as %s does), foreign penalty' % (m, m)
        return 'draws %s at %s into the caster\'s missing anatomy cells, %d voxel(s)/tick, billed per voxel%s' % (src, at, n, pen)
    if op == 'aura':
        inner = item['left']
        what = show(inner)
        where = 'the caster' if at == 'the caster' else 'the body at %s (or the place, if no body)' % at
        radius = ' over radius%s' % x(n) if n > 1 else ''
        if sort_of(inner) == 'X':
            g = inner['g'] if inner['kind'] == 'raw' else inner['op']
            body = {'float': 'floaty' if inner['n'] == 1 else 'gravity reversed: lifts away',
                    'heavy': 'pinned down, ×%d gravity' % (inner['n'] + 1),
                    'swift': 'hastened ×%d' % 2 ** inner['n'],
                    'slow': 'slowed ÷%d' % 2 ** inner['n'],
                    'shotgun': 'every effect on it fans ×%d' % 3 ** inner['n'],
                    'wide': 'every effect on it resolves ×%d wider' % 2 ** inner['n'],
                    'long': 'reach ×%d' % 2 ** inner['n'],
                    'trail': 'the body lays %s along its own path as it moves' % (show(inner['left']) if inner['kind'] == 'group' and inner['left'] else 'nothing'),
                    'split': 'every effect that resolves on it spawns %d children' % 2 ** inner['n'],
                    'seek': 'it is drawn toward the nearest other body',
                    'bounce': 'it rebounds off what it hits',
                    'pierce': 'it passes through thin walls',
                    'fuse': 'every effect on it resolves %d ticks late' % (30 * inner['n'])}.get(g, 'the mod %s (no meaning on a body: charged)' % what)
            return 'sustains %s on %s%s: %s; billed per tick to the caster until dropped or dry' % (what, where, radius, body)
        return 'sustains on %s%s, every tick, billed per tick to the caster until dropped or dry: %s' % (where, radius, desc_effect(inner, 'the body'))
    if op == 'null':
        w = item['left']
        name = w['g'] if w['kind'] == 'raw' else ('all %s' % w['op'] if not w['complete'] else 'exactly ' + show(w))
        return 'an anti-%s field (radius%s) at %s: incoming %s ops are refused; one tick unless sustained by aura' % (name, x(n), at, name)
    if op == 'echo':
        return 'repeats at %s every few ticks for a bounded span: %s' % (at, desc_effect(item['left'], at))
    return '?'


def desc_mods(mods, delivery):
    outs = []
    anchored = G.get(delivery, HAND)['mech'] != 'flight'
    for m in mods:
        n = m['n']
        if m['kind'] == 'group':
            if not m['complete']:
                outs.append('trail of nothing (charged)')
            else:
                inner = m['left']
                if anchored:
                    outs.append('trail on a non-flight delivery: nothing to lay along, charged')
                else:
                    outs.append('lays along the path%s: %s' % (x(n), desc_effect(inner, 'each marked voxel')))
            continue
        g = m['g']
        if g == 'shotgun':
            k = 3 ** n
            outs.append('%d fanned instances (everything behind it paid %d×)' % (k, k) if not anchored
                        else '%d fanned resolve points around the anchor (paid %d×)' % (k, k))
        elif g == 'split':
            outs.append('on resolve %d children with the same payload, generation-capped' % (2 ** n))
        elif g == 'float':
            if anchored:
                outs.append('anti-gravity on the caster\'s body: %s' % ('floaty' if n == 1 else 'reversed, lifts away'))
            else:
                outs.append('flight gravity %s' % ('zero (drifts straight)' if n == 1 else 'reversed (climbs)'))
        elif g == 'heavy':
            outs.append('gravity +%dg on %s' % (n, 'the caster' if anchored else 'the flight'))
        elif g == 'swift':
            outs.append('speed ×%d' % (2 ** n) if not anchored else 'caster speed ×%d for the effect (charged)' % (2 ** n))
        elif g == 'slow':
            outs.append('speed ÷%d' % (2 ** n) if not anchored else 'no flight to slow (charged)')
        elif g == 'long':
            outs.append('life/fuse ×%d' % (2 ** n) if not anchored else 'reach ×%d' % (2 ** n))
        elif g == 'wide':
            outs.append('resolve radius ×%d (volume ×%d)' % (2 ** n, 8 ** n))
        elif g == 'bounce':
            outs.append('%d bounce(s) before resolving' % n if not anchored else 'nothing to bounce (charged)')
        elif g == 'pierce':
            outs.append('passes through %d body/wall(s)' % n if not anchored else 'nothing to pierce (charged)')
        elif g == 'seek':
            outs.append('homing ×%d' % n if not anchored else 'nothing to steer (charged)')
        elif g == 'fuse':
            outs.append('resolves %d ticks after impact' % (30 * n) if not anchored else 'resolves %d ticks after the cast' % (30 * n))
    return outs


def desc_delivery(c):
    d, n = c['delivery'], c['dn']
    at = {'hand': 'reach', 'self': 'the caster', 'beam': 'the beam hit'}.get(d, 'the impact point')
    heads = {
        'hand': 'from the hand, at reach',
        'projectile': 'a bolt; resolves on impact',
        'bolt': 'a fast light bolt; resolves on impact',
        'lob': 'a slow arcing lob; resolves on impact',
        'orb': 'a slow weightless orb; resolves on impact or expiry',
        'bomb': 'a dropped bomb; resolves where it rests when the fuse runs out',
        'beam': 'held beam; resolves at the ray hit EVERY TICK, billed per tick',
        'self': 'on the caster (or the clicked body part)',
    }[d]
    if n > 1:
        heads += ', weight ×%d' % n
    return heads, at


def describe(ids):
    cls = parse(ids)
    out = []
    for c in cls:
        head, at = desc_delivery(c)
        pay = [desc_effect(p, at) for p in c['payload']]
        if not pay:
            pay = ['empty payload: kinetic impact only' if G.get(c['delivery'], HAND)['mech'] == 'flight'
                   else 'empty payload: nothing, the word is charged']
        parts = [head, '; '.join(pay)]
        mods = desc_mods(c['mods'], c['delivery'])
        if mods:
            parts.append('mods: ' + '; '.join(mods))
        out.append(' — '.join(parts))
    return ' ‖ '.join(out)


def cost_shape(ids):
    cls = parse(ids)
    parts = []
    for c in cls:
        inst = 1
        terms = []
        for m in c['mods']:
            if m['kind'] == 'raw' and m['g'] == 'shotgun':
                inst *= 3 ** m['n']
            if m['kind'] == 'raw' and m['g'] == 'wide':
                terms.append('vol×%d' % 8 ** m['n'])
            if m['kind'] == 'raw' and m['g'] == 'split':
                terms.append('children×%d' % 2 ** m['n'])
            if m['kind'] == 'group' and m['complete']:
                terms.append('trail[%s]' % key(m['left']))
        pay = []
        for p in c['payload']:
            k = key(p) + x(p['n'])
            if 'anything' in k:
                k += '(actual value×surcharge, billed on resolve)'
            if p['kind'] == 'group' and p['op'] == 'aura' and p['complete']:
                k += ' per tick, sustained'
            pay.append(k)
        d = G.get(c['delivery'], HAND)
        empty = 'kinetic' if d['mech'] == 'flight' else 'nothing'
        s = '%s×[%s]' % (inst, ' + '.join(pay) or empty) if inst > 1 else '[%s]' % (' + '.join(pay) or empty)
        if terms:
            s += '·' + '·'.join(terms)
        s += '·carry(%s %.1f)' % (c['delivery'], d['carry'] / 1000)
        if d['mech'] == 'continuous':
            s += ' per tick'
        parts.append(s)
    words = sum(G[g]['word'] for g in ids)
    return ' + '.join(parts) + ' + words %d' % words


def canon(ids):
    """Equivalence key under R6: per clause, sorted bag keys + delivery."""
    return tuple((c['delivery'], c['dn'], tuple(sorted((key(i), i['n']) for i in c['bag'])))
                 for c in parse(ids))

# --------------------------------------------------------------------------
# Doc generation
# --------------------------------------------------------------------------


def glyph_table():
    rows = ['| glyph | sort | word | repeat | axis | takes | says |', '|---|---|---|---|---|---|---|']
    for g in G.values():
        takes = ''
        if g['sort'] == 'Op':
            l = '/'.join(sorted(g['left'])) + ' ◂ ' if g['left'] else ''
            r = ' ▸ ' + '/'.join(sorted(g['right'])) if g['right'] else ''
            takes = '%s%s%s → %s' % (l, g['id'], r, g['result'])
        elif g['sort'] == 'M':
            takes = 'value %s' % g['arcane']
        elif g['sort'] == 'D':
            takes = '%s, carry %.1f×' % (g['mech'], g['carry'] / 1000)
        elif g['sort'] == 'X':
            takes = g['field']
        sortname = {'M': 'matter', 'E': 'effect', 'D': 'delivery', 'X': 'mod', 'Op': 'operator'}[g['sort']]
        rows.append('| `%s` | %s | %d | %s | %s | %s | %s |' % (
            g['id'], sortname, g['word'], g['repeat'], g['axis'], takes, g['desc'].replace('|', '/')))
    return '\n'.join(rows)


def table(seqs, dedupe=True):
    seen = OrderedDict()
    for s in seqs:
        k = canon(s)
        if dedupe and k in seen:
            seen[k]['also'].append(' '.join(s))
            continue
        seen[k] = dict(seq=s, also=[])
    rows = ['| spoken | parse | what happens | cost shape |', '|---|---|---|---|']
    for e in seen.values():
        s = e['seq']
        spoken = '`' + ' '.join(s) + '`'
        if e['also']:
            spoken += ' (+%d same: %s)' % (len(e['also']), ', '.join('`%s`' % a for a in e['also'][:3]) + (' …' if len(e['also']) > 3 else ''))
        rows.append('| %s | %s | %s | %s |' % (spoken, bracket(s), describe(s), cost_shape(s)))
    return '\n'.join(rows), len(seen)


NAMED = [
    'dirt transmute water projectile',
    'water transmute gold',
    'water transmute gold beam',
    'anything transmute gold projectile',
    'anything transmute air projectile',
    'air transmute stone projectile',
    'fire transmute air',
    'shotgun projectile',
    'shotgun shotgun shotgun projectile',
    'explosive shotgun shotgun shotgun projectile',
    'fire trail explosive shotgun shotgun shotgun projectile',
    'fire trail bomb',
    'explosive bomb',
    'explosive explosive bomb',
    'explosive projectile',
    'explosive self',
    'fire self',
    'blood mend',
    'blood mend self',
    'wood mend',
    'steel mend',
    'anything mend',
    'float self',
    'float aura self',
    'float float aura self',
    'float float aura projectile',
    'float aura',
    'transmute null aura self',
    'projectile null aura self',
    'transmute null aura projectile',
    'explosive null self',
    'fire aura self',
    'fire aura projectile',
    'explosive aura bomb',
    'explosive aura aura projectile',
    'blood mend aura self',
    'anything transmute air aura self',
    'water transmute gold aura self',
    'fire aura aura self',
    'aura self',
    'explosive trail projectile',
    'acid transmute projectile',
    'transmute acid projectile',
    'anything transmute acid projectile',
    'explosive echo orb',
    'split split explosive projectile',
    'explosive fuse projectile',
    'slow float explosive orb',
    'implode gust projectile',
    'spark trail bolt',
    'lava gust gust projectile',
    'projectile explosive',
    'explosive projectile fire bomb',
    'fire fire fire',
    'anything projectile',
    'anything',
    'air',
]


ALPHA2 = ['fire', 'gold', 'air', 'anything', 'explosive', 'gust', 'transmute', 'mend',
          'trail', 'null', 'aura', 'echo', 'shotgun', 'float', 'split', 'projectile',
          'bomb', 'self', 'beam']
ALPHA3 = ['fire', 'gold', 'anything', 'transmute', 'trail', 'explosive', 'shotgun',
          'projectile', 'bomb', 'self']


def write_oracle(path):
    """The C++ parser's oracle: every brief sentence and every pair over ALPHA2,
    as canonical parse strings. The `spells-oracle` check in the selftest
    parses each `spoken` and compares its bracket readout to `parse`."""
    import json
    seqs = [s.split() for s in NAMED] + [list(p) for p in itertools.product(ALPHA2, repeat=2)]
    entries = []
    seen = set()
    for ids in seqs:
        k = ' '.join(ids)
        if k in seen:
            continue
        seen.add(k)
        entries.append(dict(spoken=k, parse=bracket(ids), clauses=[
            dict(delivery=c['delivery'], weight=c['dn'],
                 payload=[key(i) + x(i['n']) for i in c['payload']],
                 mods=[key(i) + x(i['n']) for i in c['mods']]) for c in parse(ids)]))
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(dict(comment='GENERATED by scripts/magic_grammar.py --oracle; the C++ parser '
                       'must reproduce `parse` and `clauses` for every `spoken`. Regenerate '
                       'after changing the rules or the alphabet; never hand-edit.',
                       glyphs=[g['id'] for g in G.values()], entries=entries),
                  f, indent=1, ensure_ascii=False)
    return len(entries)


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    if len(sys.argv) > 1 and sys.argv[1] == '--oracle':
        n = write_oracle('assets/spells/grammar_oracle.json')
        print('wrote assets/spells/grammar_oracle.json: %d entries' % n)
        return
    if len(sys.argv) > 1:
        ids = sys.argv[1:]
        bad = [i for i in ids if i not in G]
        if bad:
            sys.exit('unknown glyph(s): %s' % ', '.join(bad))
        print('parse : ' + bracket(ids))
        print('does  : ' + describe(ids))
        print('cost  : ' + cost_shape(ids))
        return

    singles, n1 = table([[g] for g in G], dedupe=False)
    named, nn = table([s.split() for s in NAMED], dedupe=False)
    pairs, n2 = table([list(p) for p in itertools.product(ALPHA2, repeat=2)])
    triples, n3 = table([list(p) for p in itertools.product(ALPHA3, repeat=3)])

    doc = f"""# Magic grammar: glyph table and every permutation

GENERATED by `python scripts/magic_grammar.py` from the rules in
`docs/PLAN_magic_grammar.md`. Do not hand-edit; change the script (the rules)
or the glyph table in it and regenerate. Every row below is *derived* from the
same six parse rules, so a row that reads wrong is a rule that is wrong, not a
row to patch.

Notation: `⋈` transmute (A ⋈ B); `◂` the operator took the word on its
left; `▸` on its right; `_` a required word that was missing (the operator
fizzles, charged); `×N` a merged run; `‖` a clause boundary (a second
delivery); **bold** is the clause's delivery, `hand` when none was spoken.
Cost shape: `N×[payload]·carry(delivery)` — N instances, each paying the
payload tariff, times the delivery's carry premium, plus the word costs.

## 1. The glyph table ({len(G)} glyphs)

Sorts: **matter** names a material; **effect** happens at a point; **delivery**
decides where and when; **mod** edits the delivery record; **operator** takes
words on its declared side(s) and produces one of the others. `repeat`: `add`
= saying it again adds one more of its axis (fire×2 throws twice the voxels,
explosive×2 is twice the power); `compose` = applying the mod again
(shotgun×2 is 3·3 = 9, swift×2 is ×4, float×2 is gravity reversed).

{glyph_table()}

Rules the table relies on (from the plan): an operator with a missing required
word is **incomplete** — it is charged and does nothing (`transmute` needs
both `A` and `B`; `anything` and `air` are real words for the wildcard and the
void). `anything` is priced as the conversion it turns out to be PLUS the
actual matter's value times a surcharge (a knob, +50% to start), billed when it
resolves. A mod on a body-anchored delivery (`hand`, `self`, `beam`) acts
ONCE on the caster's body where that means anything (`float self` hops you up)
and is otherwise a charged no-op (`bounce self`). `aura` is the one way an
effect or a mod becomes SUSTAINED: `E aura` takes the word before it and
attaches it to the body at the resolve point (or the place, if no body), every
tick, billed per tick to the caster until dropped or dry. So `float aura self`
is a floaty ward on yourself and `float aura projectile` puts the same thing on
whoever the bolt hits. Every unary operator takes the word BEFORE it
(`fire trail`, `blood mend`, `float aura`, `transmute null`, `explosive echo`);
only `transmute` is infix.

## 2. Every single word ({n1})

{singles}

## 3. The sentences from the brief ({nn})

{named}

## 4. Every ordered pair over a 19-word alphabet ({n2} distinct casts from {len(ALPHA2) ** 2} sequences)

Alphabet: {', '.join('`%s`' % a for a in ALPHA2)}. Sequences that lower to the
same cast (R6: bag order is irrelevant) are listed once with their other
spellings.

{pairs}

## 5. Every ordered triple over a 10-word core ({n3} distinct casts from {len(ALPHA3) ** 3} sequences)

Core: {', '.join('`%s`' % a for a in ALPHA3)}.

{triples}
"""
    with open('docs/MAGIC_PERMUTATIONS.md', 'w', encoding='utf-8') as f:
        f.write(doc)
    print('wrote docs/MAGIC_PERMUTATIONS.md: %d glyphs, %d singles, %d named, %d pairs, %d triples'
          % (len(G), n1, nn, n2, n3))


if __name__ == '__main__':
    main()
