#!/usr/bin/env python3
"""Reference interpreter for the magic grammar (DESIGN.md §8, docs/PLAN_magic_grammar.md).

Executable spec: the FOUR parse rules and the lowering are written here once,
and docs/MAGIC_PERMUTATIONS.md is GENERATED from it, so every row of the
permutation tables is derived from the same rules rather than hand-written.
It is also the oracle the C++ `spells-oracle` gate compares against.

  1. A NOUN GOES INTO THE PILE (matter, an effect, or an operator result of
     sort Effect). Order inside the pile does not matter.
  2. A DELIVERY BOXES THE PILE into ONE Effect value of verb `launch` — that
     delivery's record, the pile's effects as its payload, the pile's pending
     mods stuck to its record — and speaking CONTINUES. So deliveries nest.
  3. A MOD is PENDING and sticks to the box that closes the pile, i.e. the next
     delivery spoken; with no delivery it sticks to `hand`, where `shotgun` is
     three fanned resolve points and `float` is the hop and everything else is
     a charged no-op.
  4. `lane` OPENS A LANE SCOPE and `end` CLOSES the innermost open one. A
     DELIVERY boxes the innermost OPEN scope: inside an open lane only that
     lane's items (the box lands in the lane, which stays open), outside any
     lane the shared items plus every closed lane. Lanes are ordered as spoken;
     the box fires max(count, lanes) instances, instance i carries shared +
     lane i+1, and an instance past the last lane carries the shared items
     alone. A mark is a wall for binding and for run merging; a `count` mod is
     record-wide wherever it was spoken.

  python scripts/magic_grammar.py                 # regenerate the doc
  python scripts/magic_grammar.py --oracle        # assets/spells/grammar_oracle.json for the C++ gate
  python scripts/magic_grammar.py explosive projectile shotgun projectile
"""
import itertools
import sys
from collections import OrderedDict

# --------------------------------------------------------------------------
# The glyph table. Sorts: M matter, E effect, D delivery, X mod, Op operator,
# Sep separator. repeat: 'add' (xN of the axis) or 'compose' (applied N times).
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
      arcane='actual x surcharge', axis='voxels')

# ---- nullary effects --------------------------------------------------------
glyph('explosive', 'E', 6, 'An explosion at the point. Power adds with repetition; radius follows the engine law; priced by volume.',
      verb='explode', axis='power', tariff='power x r^3', says='explodes')
glyph('gust', 'E', 5, 'A wind jet from the point along the aim. Repetition doubles speed, not size.',
      verb='wind', axis='speed', tariff='footprint x ticks', says='blows a wind jet along the aim')
glyph('implode', 'E', 5, 'A vacuum burst: pulls loose matter and bodies toward the point.',
      verb='wind(-)', axis='speed', tariff='footprint x ticks', says='pulls everything loose toward it')
glyph('spark', 'E', 2, 'One spark: an electric crackle that lives a tick or two. Pops hydrogen, splits molten salt, and only sometimes lights what burns beside it. The cheapest electricity there is.',
      verb='place(spark,1)', axis='sparks', tariff='1 voxel of spark', says='lays a spark')
glyph('shock', 'E', 4, 'A crackle of arcs where it lands: short jagged discharges that run over what is there. Every electric reaction fires off it, and it sets dry things alight more readily than a spark.',
      verb='strike(arc)', axis='arcs', tariff='arc cells x 0.3', says='goes off in a crackle of arcs')
glyph('lightning', 'E', 9, 'Calls a bolt of lightning down on the point - or on the tallest conductor near it: a lightning rod draws it. Whatever burns where it lands goes up.',
      verb='strike(lightning)', axis='bolt height / arcs', tariff='bolt cells x 0.25',
      says='calls down a bolt of lightning on the tallest conductor near it')

# ---- operators ----------------------------------------------------------------
glyph('transmute', 'Op', 4, 'A transmute B: turns A into B where it resolves. Needs BOTH words; an empty side fizzles (charged).',
      left={'M'}, right={'M'}, result='E', verb='convert', axis='volume',
      tariff='volume x (convert + max(0, value(B)-value(A)))')
glyph('mend', 'Op', 6, 'M mend: draws matter M from the resolve point into the caster\'s missing anatomy cells, one voxel at a time. Needs a source word.',
      left={'M'}, right=None, result='E', verb='graft', axis='voxels per tick',
      tariff='per voxel: value(M) x graft + foreign penalty')
glyph('trail', 'Op', 8, 'E trail: runs E at every marked voxel of the flight path. Takes the ONE item before it (a launch box included). Yields a MOD, so it is pending: it sticks to the next delivery spoken, and on anything that does not travel it is charged and does nothing.',
      left={'E', 'M'}, right=None, result='X', verb='trail', axis='budget',
      tariff='budget voxels x E')
glyph('aura', 'Op', 7, 'E aura: SUSTAINS E (or a mod) on the body at the resolve point - or on the place, if no body is there. Runs every tick, billed per tick to the caster and reserved out of their max mana, until the caster drops it or runs dry. Takes the ONE item before it, which may be a launch box: `explosive projectile aura self` is a status that fires a bolt every tick.',
      left={'E', 'M', 'X'}, right=None, result='E', verb='sustain', axis='attach radius',
      tariff='E per tick, sustained')
glyph('null', 'Op', 5, 'W null: a filter that refuses incoming ops of W\'s kind within the radius. Takes the word before it raw.',
      left={'any'}, right=None, result='E', verb='filter', axis='radius',
      tariff='radius^3 per tick')
glyph('echo', 'Op', 6, 'E echo: E repeats at the point every few ticks for a bounded span. Takes the ONE item before it: `explosive projectile echo` is a turret.',
      left={'E', 'M'}, right=None, result='E', verb='repeat', axis='repeats',
      tariff='repeats x E')

# ---- deliveries ---------------------------------------------------------------
# (id, word, carry, mech, noun, trigger, desc)
DELIV = [
    ('projectile', 6, 3000, 'flight', 'bolt', 'when {it} hits',
     'A bolt from the hand. Medium speed, resolves on impact.'),
    ('bolt', 8, 4000, 'flight', 'dart', 'when {it} hits',
     'Fast and light: twice the speed, half the kinetic impact.'),
    ('lob', 4, 2000, 'flight', 'ball', 'when {it} hits',
     'Slow, arcing, heavy. Resolves on impact.'),
    ('orb', 7, 2500, 'flight', 'orb', 'when {it} hits or its life runs out',
     'Slow, weightless, lingers; resolves on impact or when its life runs out.'),
    ('bomb', 3, 1000, 'flight', 'bomb', 'when its fuse runs down',
     'A dropped rigid body with a fuse. Resolves where it comes to rest when the fuse runs out. The cheapest carrier.'),
    ('beam', 5, 1500, 'continuous', 'beam', 'every tick at the ray hit',
     'Held: resolves at the ray hit every tick, billed per tick.'),
    ('self', 2, 1000, 'instant', 'touch', 'at once, on the caster',
     'At the caster. From the character screen, at the chosen body part.'),
]
for did, word, carry, mech, noun, trig, desc in DELIV:
    glyph(did, 'D', word, desc, carry=carry, mech=mech, noun=noun, trig=trig,
          axis='boxes the pile; nests')

HAND = dict(id='hand', carry=1000, mech='hand', word=0, noun='hand',
            trig='at reach in front of you', sort='D', repeat='add',
            desc='Implicit. At reach: a few voxels in front of the caster along the aim.')

# ---- mods (field edits on the delivery record; repeat = compose) -----------
# (id, word, field, axis-blurb, desc)
MODS = [
    ('shotgun', 5, 'count', 'count x3 (3, 9, 27 ...), fanned',
     'Three of it, fanned. Said again: nine. It sticks to the box that closes the pile, and everything inside that box is paid that many times.'),
    ('float', 4, 'gravity', 'gravity -1g',
     'Anti-gravity. Twice: gravity reversed. On the hand it is a hop.'),
    ('heavy', 3, 'gravity', 'gravity +1g', 'Falls harder, hits harder.'),
    ('swift', 4, 'speed', 'speed x2', 'Faster.'),
    ('slow', 2, 'speed', 'speed /2', 'Slower. A slow explosive bolt is a mine that drifts.'),
    ('long', 3, 'lifetime', 'life x2', 'Lives longer, reaches further; a bomb\'s fuse is longer.'),
    ('wide', 5, 'radius', 'resolve radius x2', 'Everything that box resolves is wider. Priced by volume, so x8 per word.'),
    ('bounce', 4, 'bounces', 'bounces +1', 'Reflects off what it hits once more before resolving.'),
    ('pierce', 5, 'pierce', 'pierce +1', 'Passes through one body or thin wall before resolving.'),
    ('seek', 8, 'seek', 'homing +1', 'Turns toward the nearest body.'),
    ('fuse', 3, 'fuse', 'delay +30 ticks', 'Waits after impact before resolving; on a bomb, a longer fuse.'),
]
for mid, word, field, axis, desc in MODS:
    glyph(mid, 'X', word, desc, field=field, repeat='compose', axis=axis)

# ---- the separator (rule 4) ---------------------------------------------------
glyph('lane', 'Sep', 0,
      'Opens a LANE: a scope of the pile that belongs to ONE instance of the box that closes it; `end` '
      'closes it again. The first lane you open is what instance 0 carries on top of the shared '
      'items, the second what instance 1 carries, and so on; the box fires max(count, lanes) '
      'instances and an instance past the last lane carries the shared items alone. A delivery '
      'spoken INSIDE an open lane boxes only that lane, and the lane stays open - which is how a '
      'socket holds a bolt that fires a bolt. A mark is a wall for binding and for merging; a '
      '`count` mod is record-wide wherever it is spoken.',
      scope='open', axis='lanes (one instance each)')
glyph('end', 'Sep', 0,
      'Closes the innermost open `lane`, back to the scope around it. A lane left open when '
      'the sentence runs out is closed for you; an `end` with nothing open is charged and '
      'does nothing.',
      scope='close', axis='closes a lane')
glyph('twin', 'X', 3, 'Two of it, fanned. Said again: four. The double of `shotgun`\'s triple; '
      'everything behind it is paid that many times.',
      field='count', repeat='compose', axis='count x2 (2, 4, 8 ...), fanned')

# What one utterance of a `count` mod multiplies the fan by. Content, so
# `twin` is a row and not a branch.
G['shotgun']['amount'] = 3
G['twin']['amount'] = 2

MAX_MULT = 6

# WHICH FIELDS A RECORD ACTUALLY HAS (rule 3). A mod that lands on a record
# with no such field is charged and does nothing, and the describe line says so
# rather than letting it edit something nobody reads. Mirrors
# ModMeansAnything() in src/game/spell.cpp.
FIELDS_BY_MECH = {
    'hand': {'count', 'gravity'},
    'instant': {'count', 'gravity', 'radius', 'lifetime'},
    'continuous': {'count', 'radius', 'lifetime'},
    'flight': {'count', 'gravity', 'speed', 'lifetime', 'radius',
               'bounces', 'pierce', 'seek', 'fuse'},
}

# --------------------------------------------------------------------------
# Parse (the three rules)
# --------------------------------------------------------------------------


def sort_of(item):
    if item['kind'] == 'box':
        return 'E'          # RULE 2: a box is an Effect value again
    if item['kind'] == 'raw':
        return G[item['g']]['sort']
    return G[item['op']]['result']


def accepts(slot, item):
    if slot is None:
        return False
    if 'any' in slot:
        return True
    return sort_of(item) in slot


def lane_of(item):
    return item.get('lane', 0)


def key(item):
    # RULE 4: the segment is part of the identity, so `fire lane fire` is two
    # items in two lanes and not `fire x2`. Only a non-shared lane is spelled,
    # so every sentence without a `lane` word keys exactly as it always did.
    ln = '@%d' % lane_of(item) if lane_of(item) else ''
    if item['kind'] == 'box':
        return '[%s|%s]%s' % (','.join('%s#%d' % (key(i), i['n']) for i in item['items']),
                              item['d'], ln)
    if item['kind'] == 'raw':
        return item['g'] + ln
    return '(%s|%s|%s)%s' % (key(item['left']) if item['left'] else '',
                             item['op'], key(item['right']) if item['right'] else '', ln)


def merge_pile(pile):
    """The pile is a SET: identical items collapse (sum n, capped) in
    first-seen order. Rule 1's "order does not matter" is only true because
    this merge is order-insensitive."""
    merged = OrderedDict()
    for it in pile:
        k = key(it)
        if k in merged:
            merged[k]['n'] = min(merged[k]['n'] + it['n'], MAX_MULT)
        else:
            merged[k] = dict(it)
    return list(merged.values())


def box(sc, delivery):
    """RULE 2 + RULE 4: box a whole SCOPE - its shared items first, then one
    segment per closed lane. The items are stamped with their lane HERE, before
    the merge, because the lane is part of key() and the merge must not join
    two segments."""
    allitems = []
    for it in sc['pile']:
        it['lane'] = 0
        allitems.append(it)
    for k, ln in enumerate(sc['lanes']):
        for it in ln:
            it['lane'] = k + 1
            allitems.append(it)
    return dict(kind='box', d=delivery, n=1, items=merge_pile(allitems),
                nlanes=len(sc['lanes']))


def merge_runs(ids):
    """Runs merge — but NOT for deliveries or the separator, because each
    delivery boxes what is in front of it and a merged pair would silently
    drop a nesting."""
    items = []
    for gid in ids:
        mergeable = G[gid]['sort'] not in ('D', 'Sep')
        if mergeable and items and items[-1]['kind'] == 'raw' and items[-1]['g'] == gid:
            items[-1]['n'] = min(items[-1]['n'] + 1, MAX_MULT)
        else:
            items.append(dict(kind='raw', g=gid, n=1))
    return items


def parse(ids):
    """Returns a list of clauses - since `also` was dropped for rule 4 there is
    exactly one (or none, for silence). Each is the ROOT hand box plus the
    payload/mods split of its pile.

    RULE 4 is a SCOPE STACK: `lane` pushes a scope on the current pile, `end`
    pops the innermost open one into its parent's lane list, and a DELIVERY
    boxes the innermost OPEN scope - so a delivery inside a lane boxes that
    lane alone and leaves the lane open."""
    clauses = []
    items = merge_runs(ids)
    scopes = [dict(pile=[], lanes=[])]
    i = 0
    while i < len(items):
        it = items[i]
        g = G[it['g']] if it['kind'] == 'raw' else None
        s = g['sort'] if g else None
        sc = scopes[-1]
        if s == 'Op':
            left = right = None
            # THE ONE ITEM TO ITS LEFT - the top of THIS SCOPE's pile, which may
            # be a box. A mark is a wall: an operator never reaches past one.
            if g['left'] and sc['pile'] and accepts(g['left'], sc['pile'][-1]):
                left = sc['pile'].pop()
            if g['right'] and i + 1 < len(items) and accepts(g['right'], items[i + 1]):
                right = items[i + 1]
                i += 1
            complete = (g['left'] is None or left is not None) and \
                       (g['right'] is None or right is not None)
            sc['pile'].append(dict(kind='group', op=it['g'], n=it['n'], left=left,
                                   right=right, complete=complete))
        elif s == 'D':
            b = box(sc, it['g'])            # RULE 2, scoped by RULE 4
            sc['pile'] = [b]
            sc['lanes'] = []
        elif s == 'Sep':
            if g.get('scope', 'open') == 'open':
                scopes.append(dict(pile=[], lanes=[]))
            elif len(scopes) > 1:
                close_lane(scopes)
            # An `end` with nothing open is a charged no-op.
        else:
            sc['pile'].append(it)           # RULE 1
        i += 1
    if ids:
        while len(scopes) > 1:
            close_lane(scopes)              # an unclosed lane closes implicitly
        clauses.append(clause_of(box(scopes[0], 'hand')))
    return clauses


def close_lane(scopes):
    """Pop the innermost open lane into its parent. A lane that itself opened
    lanes and never boxed them has no record to make them columns OF, so they
    flatten into it - the one place a scope is not a column."""
    sc = scopes.pop()
    pile = sc['pile']
    for sub in sc['lanes']:
        pile.extend(sub)
    scopes[-1]['lanes'].append(pile)


def clause_of(root):
    payload, mods = [], []
    for it in root['items']:
        s = sort_of(it)
        if s in ('E', 'M'):
            payload.append(it)
        elif s == 'X':
            mods.append(it)
    return dict(root=root, delivery='hand', dn=1, bag=root['items'],
                payload=payload, mods=mods)


def split_pile(b):
    """A box's pile, split into payload nouns and the mods stuck to its record."""
    payload, mods = [], []
    for it in b['items']:
        s = sort_of(it)
        if s in ('E', 'M'):
            payload.append(it)
        elif s == 'X':
            mods.append(it)
    return payload, mods

# --------------------------------------------------------------------------
# Show
# --------------------------------------------------------------------------


def x(n):
    return '' if n == 1 else '×%d' % n


def show_items(items, nlanes):
    """A list of pile items with its LANE SCOPES marked: the shared items, then
    each lane as `/ ... /` - the two marks the sentence actually contains, so
    the readout round-trips back to words. ` / ` in both bracket styles,
    because the C++ compares these strings verbatim."""
    parts = [show(it) for it in items if lane_of(it) == 0]
    for k in range(1, nlanes + 1):
        parts.append('/')
        parts += [show(it) for it in items if lane_of(it) == k]
        parts.append('/')
    return ' '.join(parts)


def show(item):
    if item['kind'] == 'box':
        s = show_items(item['items'], item.get('nlanes', 0))
        if s:
            s += ' '
        return '[%s**%s%s**]' % (s, item['d'], x(item['n']))
    if item['kind'] == 'raw':
        return item['g'] + x(item['n'])
    op = G[item['op']]
    l = show(item['left']) if item['left'] else ('_' if op['left'] else '')
    r = show(item['right']) if item['right'] else ('_' if op['right'] else '')
    sym = {'transmute': '⋈'}.get(item['op'], '◂' + item['op'])
    inner = ' '.join(p for p in (l, sym, r) if p)
    return '(%s)%s' % (inner, x(item['n']))


def bracket(ids):
    parts = []
    for c in parse(ids):
        s = show_items(c['bag'], c['root'].get('nlanes', 0))
        parts.append(s or '(empty)')
    return ' '.join(parts)

# --------------------------------------------------------------------------
# Describe: one recursive sentence per cast
# --------------------------------------------------------------------------


def mech_of(d):
    return 'hand' if d == 'hand' else G[d]['mech']


def record_words(d, mods):
    """The mods that stuck to this record, as adjectives before the noun, a
    trailing clause, the fan count, and the ones that were wasted."""
    mech = mech_of(d)
    have = FIELDS_BY_MECH[mech]
    adj, post, fan, wasted, waits = [], None, 1, [], 0
    for m in mods:
        if m['kind'] == 'group':                     # a trail
            if mech != 'flight' or not m['complete']:
                wasted.append(m)
            continue
        g = G[m['g']]
        if g['field'] not in have:
            wasted.append(m)
            continue
        n = m['n']
        if g['field'] == 'count':
            fan *= g.get('amount', 3) ** n
        elif m['g'] == 'swift':
            adj.append('fast')
        elif m['g'] == 'slow':
            adj.append('slow')
        elif m['g'] == 'float':
            adj.append('floating')
        elif m['g'] == 'heavy':
            adj.append('heavy')
        elif m['g'] == 'long':
            adj.append('long-lived')
        elif m['g'] == 'wide':
            adj.append('wide')
        elif m['g'] == 'pierce':
            adj.append('piercing')
        elif m['g'] == 'seek':
            adj.append('seeking')
        elif m['g'] == 'bounce':
            post = 'bounce'
        elif m['g'] == 'fuse':
            waits = 30 * n
    return ' '.join(adj), post, min(fan, 27), wasted, waits


def phrase(item):
    """A short third-person verb phrase: what one payload item DOES."""
    if item['kind'] == 'box':
        return 'fires ' + launch_sentence(item, 'that')
    n = item['n']
    if item['kind'] == 'raw':
        g = G[item['g']]
        if g['sort'] == 'M':
            m = item['g']
            if m == 'air':
                return 'throws nothing'
            if m == 'anything':
                return 'scoops up whatever is there and throws it'
            return 'sprays %s%s' % (m, '' if n == 1 else ' x%d' % n)
        return g['says'] + ('' if n == 1 else ' x%d' % n)
    op = item['op']
    if not item['complete']:
        return 'wastes `%s` (a word it needed was missing)' % op
    if op == 'transmute':
        a, b = item['left']['g'], item['right']['g']
        if a == b:
            return 'converts %s to itself: nothing' % a
        if a == 'air':
            return 'conjures %s into empty space' % b
        if b == 'air':
            return 'unmakes %s' % ('whatever is there' if a == 'anything' else a)
        return 'turns %s into %s' % ('whatever is there' if a == 'anything' else a, b)
    if op == 'mend':
        m = item['left']['g']
        return 'draws %s into the caster\'s missing anatomy' % (
            'whatever matter is there' if m == 'anything' else m)
    if op == 'aura':
        inner = item['left']
        what = ('the mod ' + (inner['g'] if inner['kind'] == 'raw' else inner['op'])) \
            if sort_of(inner) == 'X' else phrase(inner)
        return 'sustains [%s] on whatever it lands on, every tick, billed per tick' % what
    if op == 'null':
        w = item['left']
        name = w['g'] if w['kind'] == 'raw' else (w['d'] if w['kind'] == 'box' else w['op'])
        return 'refuses incoming %s for a moment' % name
    if op == 'echo':
        return 'repeats [%s] every few ticks, a bounded number of times' % phrase(item['left'])
    if op == 'trail':
        return 'lays %s along a path it does not have' % phrase(item['left'])
    return '?'


def lane_items(b, lane):
    return [i for i in b['items'] if lane_of(i) == lane]


def lane_clause(b, lane, noun):
    """WHAT ONE LANE CHANGES, as a fragment: only the difference from what
    every instance carries (rule 4)."""
    parts = []
    mine = lane_items(b, lane)
    mods = [i for i in mine if sort_of(i) == 'X']
    adj, _post, _fan, _w, _waits = record_words(b['d'], mods)
    if adj:
        parts.append('is ' + adj)
    for it in mine:
        if sort_of(it) in ('E', 'M'):
            parts.append('also ' + phrase(it))
    head = 'the %s %s ' % (ORDINAL[lane - 1] if lane <= len(ORDINAL) else 'next', noun)
    if not parts:
        return head + 'carries only what they all carry'
    return head + ' and '.join(parts)


ORDINAL = ['first', 'second', 'third', 'fourth', 'fifth', 'sixth', 'seventh', 'eighth']


def instances_of(b, fan):
    return min(max(fan, b.get('nlanes', 0)), 27)


def launch_sentence(b, that):
    d = b['d']
    g = G[d]
    payload, mods = split_pile(b)
    adj, post, fan, wasted, waits = record_words(d, mods)
    fan = instances_of(b, fan)
    noun = g['noun']
    if fan > 1:
        head = '%d fanned %s%ss' % (fan, adj + ' ' if adj else '', noun)
    else:
        head = 'a %s%s' % (adj + ' ' if adj else '', noun)
    it = 'each' if fan > 1 else that
    for m in mods:
        if m['kind'] == 'group' and m['complete'] and mech_of(d) == 'flight':
            head += ' that %s along its whole path' % phrase(m['left'])
    trig = g['trig'].replace('{it}', it)
    if post == 'bounce':
        trig = 'it bounces once, then when it hits again'
    join = ', '
    if waits:
        trig += ' it waits %d ticks, then' % waits
        join = ' '
    parts = [phrase(p) for p in lane_items(b, 0) if sort_of(p) in ('E', 'M')]
    if not parts:
        parts = ['does nothing but knock what it hit' if mech_of(d) == 'flight'
                 else 'does nothing']
    out = '%s; %s%s%s %s' % (head, trig, join, 'each' if fan > 1 else 'it',
                             ' and '.join(parts))
    for k in range(1, b.get('nlanes', 0) + 1):
        out += '; ' + lane_clause(b, k, noun)
    return out


MISSING_ON = {'count': 'count', 'gravity': 'weight', 'speed': 'speed',
              'lifetime': 'lifetime', 'radius': 'radius',
              'bounces': 'surface to bounce off', 'pierce': 'wall to pierce',
              'seek': 'target to steer for', 'fuse': 'fuse'}


def describe_clause(c):
    root = c['root']
    payload, mods = split_pile(root)
    adj, post, fan, wasted, waits = record_words('hand', mods)
    fan = instances_of(root, fan)
    out = []
    # Nouns first, then the carriers spoken beside them: the sentence reads
    # outward from the caster, the way the spell happens.
    shared = [p for p in lane_items(root, 0) if sort_of(p) in ('E', 'M')]
    for p in shared:
        if p['kind'] != 'box':
            s = phrase(p)
            out.append((s[0].upper() + s[1:]) + ' right in front of you' +
                       (' at %d fanned points' % fan if fan > 1 else '') + '.')
    for p in shared:
        if p['kind'] == 'box':
            out.append(('From %d fanned points in front of you, ' % fan if fan > 1
                        else 'You fire ') + launch_sentence(p, 'it') + '.')
    if not payload:
        out.append('Nothing happens; the words are charged.')
    for k in range(1, root.get('nlanes', 0) + 1):
        s = lane_clause(root, k, 'resolve')
        out.append(s[0].upper() + s[1:] + '.')
    for m in mods:
        if m['kind'] == 'raw' and G[m['g']]['field'] == 'gravity':
            out.append('You hop.' if m['g'] == 'float' else 'You are shoved down.')
    for m in wasted:
        if m['kind'] == 'group':
            out.append('The trail is wasted: your hand does not travel anywhere.')
        else:
            out.append('`%s` is wasted: it landed on your hand, which has no %s.'
                       % (m['g'], MISSING_ON[G[m['g']]['field']]))
    return ' '.join(out)


def describe(ids):
    return ' '.join(describe_clause(c) for c in parse(ids))

# --------------------------------------------------------------------------
# Cost shape (recursive: carry composes multiplicatively down the tree)
# --------------------------------------------------------------------------


def cost_shape_box(b):
    payload, mods = split_pile(b)
    _, _, fan, _, _ = record_words(b['d'], mods)
    d = G.get(b['d'], HAND)
    nlanes = b.get('nlanes', 0)
    inst = instances_of(b, fan)
    terms = []
    for m in mods:
        if m['kind'] == 'group' and m['complete']:
            terms.append('trail[%s]' % key(m['left']))
        elif m['kind'] == 'raw' and m['g'] == 'wide':
            terms.append('vol×%d' % 8 ** m['n'])

    def items_shape(lane):
        pay = []
        for p in b['items']:
            if lane_of(p) != lane or sort_of(p) not in ('E', 'M'):
                continue
            if p['kind'] == 'box':
                pay.append(cost_shape_box(p))
                continue
            k = key(p) + x(p['n'])
            if 'anything' in k:
                k += '(actual value×surcharge, billed on resolve)'
            if p['kind'] == 'group' and p['op'] == 'aura' and p['complete']:
                k += ' per tick, sustained'
            pay.append(k)
        return pay

    empty = 'kinetic' if d['mech'] == 'flight' else 'nothing'
    sharedPay = items_shape(0)
    # RULE 4: the tariff is the SUM over instances of the shared segment plus
    # that instance's lane, which is the old product exactly when L = 0.
    if nlanes:
        arms = []
        for k in range(1, min(nlanes, inst) + 1):
            arms.append('[%s]' % (' + '.join(sharedPay + items_shape(k)) or empty))
        if inst > nlanes:
            arms.append('%d×[%s]' % (inst - nlanes, ' + '.join(sharedPay) or empty))
        s = ' + '.join(arms)
    else:
        s = '[%s]' % (' + '.join(sharedPay) or empty)
        if inst > 1:
            s = '%d×%s' % (inst, s)
    if terms:
        s += '·' + '·'.join(terms)
    if d['carry'] != 1000:
        s += '·carry(%s %.1f)' % (b['d'], d['carry'] / 1000)
    if d['mech'] == 'continuous':
        s += ' per tick'
    return s


def cost_shape(ids):
    parts = [cost_shape_box(c['root']) for c in parse(ids)]
    words = sum(G[g]['word'] for g in ids)
    return ' + '.join(parts) + ' + words %d' % words


def canon(ids):
    """Equivalence key: per clause, the root box's canonical key."""
    return tuple(canon_box(c['root']) for c in parse(ids))


def canon_box(b):
    # RULE 4: the lane is part of an item's key, so two sentences are the same
    # cast only if each SEGMENT holds the same multiset.
    return '[%s|%s|%d]' % (','.join(sorted('%s#%d' % (canon_item(i), i['n'])
                                           for i in b['items'])), b['d'],
                           b.get('nlanes', 0))


def canon_item(it):
    return canon_box(it) if it['kind'] == 'box' else key(it)


def does_nothing(ids):
    """A FIZZLE-ONLY sentence: every clause is an empty hand with nothing but
    charged words in it. `projectile` is NOT one — an empty carrier is still a
    kinetic hit."""
    return all(not c['payload'] for c in parse(ids))

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
            takes = '%s, carry %.1f×, "%s"' % (g['mech'], g['carry'] / 1000, g['noun'])
        elif g['sort'] == 'X':
            takes = g['field']
        elif g['sort'] == 'Sep':
            takes = 'closes a lane' if g.get('scope') == 'close' else 'opens a lane'
        sortname = {'M': 'matter', 'E': 'effect', 'D': 'delivery', 'X': 'mod',
                    'Op': 'operator', 'Sep': 'separator'}[g['sort']]
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


def metric(seqs):
    """Orderings vs distinct spells, and how many of those actually do
    something. The second number is the one that says whether ORDER carries
    meaning: if it equals 1, every ordering said the same thing."""
    seqs = list(seqs)
    distinct = OrderedDict()
    for s in seqs:
        distinct.setdefault(canon(s), s)
    live = [s for s in distinct.values() if not does_nothing(s)]
    return len(seqs), len(distinct), len(live)


WORKED = [
    ('Order decides who fans', [
        'explosive shotgun projectile', 'shotgun explosive projectile',
        'explosive projectile shotgun', 'shotgun projectile explosive']),
    ('Order decides what is inside what', [
        'fire explosive shotgun projectile', 'fire explosive projectile shotgun',
        'explosive shotgun projectile fire']),
    ('Deliveries nest: a bolt that fires a bolt', [
        'explosive projectile projectile', 'explosive projectile fuse projectile',
        'explosive fuse projectile projectile', 'explosive projectile projectile fuse',
        'explosive projectile shotgun projectile', 'explosive shotgun projectile projectile']),
    ('A bomb that fires a bolt', [
        'fire explosive projectile bomb', 'explosive fire projectile bomb',
        'explosive projectile fire bomb', 'explosive projectile bomb fire']),
    ('A unary operator takes the ONE item before it', [
        'fire trail explosive projectile', 'explosive fire trail projectile',
        'fire explosive trail projectile', 'explosive projectile fire trail',
        'explosive projectile echo', 'explosive projectile aura self']),
    ('A mod sticks to the box that closes the pile', [
        'explosive projectile swift projectile', 'explosive swift projectile projectile',
        'explosive projectile projectile swift', 'explosive projectile fire']),
    ('A lane is one instance of the box that closes the pile', [
        'lane explosive projectile end lane blood mend self end',
        'explosive lane sand end lane fire end twin projectile',
        'lane explosive projectile end lane fire end projectile',
        'lane explosive projectile fire end projectile',
        'explosive twin projectile',
        'fire lane trail end projectile', 'fire lane fire end projectile']),
]


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
    'explosive projectile echo',
    'explosive projectile aura self',
    'explosive fuse projectile',
    'slow float explosive orb',
    'implode gust projectile',
    'spark trail bolt',
    'lightning projectile',
    'shock projectile',
    'lava gust gust projectile',
    'projectile explosive',
    'explosive projectile fire bomb',
    'explosive projectile projectile',
    'explosive projectile shotgun projectile',
    'explosive lane fire end projectile',
    'explosive lane sand end lane fire end twin projectile',
    'explosive twin projectile',
    'explosive twin twin projectile',
    'lane explosive projectile end lane blood mend self end',
    'lane explosive projectile end lane fire end projectile',
    'lane explosive projectile fire end projectile',
    'gold lane fire trail end projectile',
    'fire lane fire end',
    'lane lane fire end end',
    'fire fire fire',
    'anything projectile',
    'anything',
    'air',
    'lane',
    'end',
    'lane lane end end',
    'lane end lane end',
]


ALPHA2 = ['fire', 'gold', 'air', 'anything', 'explosive', 'gust', 'transmute', 'mend',
          'trail', 'null', 'aura', 'echo', 'shotgun', 'float', 'lane', 'projectile',
          'bomb', 'self', 'beam', 'twin', 'end']
ALPHA3 = ['fire', 'gold', 'anything', 'transmute', 'trail', 'explosive', 'shotgun',
          'projectile', 'bomb', 'self']


def write_oracle(path):
    """The C++ parser's oracle: every brief sentence, every worked set and
    every pair over ALPHA2, as canonical parse strings. The `spells-oracle`
    gate parses each `spoken` and compares its bracket readout and clause
    structure to `parse`/`clauses`."""
    import json
    worked = [w for _, ws in WORKED for w in ws]
    seqs = ([s.split() for s in NAMED] + [w.split() for w in worked] +
            [list(p) for p in itertools.product(ALPHA2, repeat=2)])
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
    pair_seqs = [list(p) for p in itertools.product(ALPHA2, repeat=2)]
    triple_seqs = [list(p) for p in itertools.product(ALPHA3, repeat=3)]
    pairs, n2 = table(pair_seqs)
    triples, n3 = table(triple_seqs)
    p_ord, p_dist, p_live = metric(pair_seqs)
    t_ord, t_dist, t_live = metric(triple_seqs)

    worked_md = []
    for title, seqs in WORKED:
        worked_md.append('### %s\n' % title)
        worked_md.append('| spoken | what happens |')
        worked_md.append('|---|---|')
        for s in seqs:
            worked_md.append('| `%s` | %s |' % (s, describe(s.split())))
        worked_md.append('')
    worked_md = '\n'.join(worked_md)

    doc = f"""# Magic grammar: glyph table and every permutation

GENERATED by `python scripts/magic_grammar.py` from the rules in DESIGN.md §8.
Do not hand-edit; change the script (the rules) or the glyph table in it and
regenerate. Every row below is *derived* from the same four parse rules, so a
row that reads wrong is a rule that is wrong, not a row to patch.

**The four rules.** (1) A noun — matter, an effect, or an operator result of
sort Effect — goes into the PILE, and order inside a SEGMENT of the pile does
not matter. (2) A Delivery BOXES the whole pile into one Effect value: that
delivery's record, the pile's effects as its payload, the pile's pending mods
stuck to its record — and speaking CONTINUES, so deliveries NEST. (3) A Mod is
PENDING and sticks to the box that closes the pile; with no delivery it sticks
to `hand`, where `shotgun` is three fanned resolve points and `float` is the
hop and everything else is a charged no-op. (4) `lane` OPENS A LANE SCOPE and
`end` CLOSES the innermost open one, and **a delivery boxes the innermost OPEN
scope**: inside an open lane only that lane's items (the box lands in the lane,
which stays open), outside any lane the shared items plus every closed lane —
the multi-socket box. Lanes are ordered as spoken, lane *i* belongs to instance
*i*, the box fires max(count, lanes) instances, and an instance past the last
lane carries the shared items alone. A mark is a wall for binding and for
merging; a `count` mod is record-wide wherever it is spoken. The outermost box
is always `hand`, so the whole utterance is ONE cast — lanes on the hand are
how two unrelated spells are said at once (`lane explosive projectile end lane
blood mend self end`), and `also` is gone.

**How much order buys.** Orderings, the distinct spells they lower to, and how
many of those do anything at all (a sentence of nothing but mods is charged and
fizzles):

| alphabet | orderings | distinct spells | of those, non-fizzle |
|---|---|---|---|
| pairs over {len(ALPHA2)} words | {p_ord} | {p_dist} | {p_live} |
| triples over {len(ALPHA3)} words | {t_ord} | {t_dist} | {t_live} |

Notation: `⋈` transmute (A ⋈ B); `◂` the operator took the word on its
left; `▸` on its right; `_` a required word that was missing (the operator
fizzles, charged); `×N` a merged run; `[ … **delivery**]` a BOX — the pile that
delivery closed; `/ … /` a LANE inside a pile (`lane` … `end`). The outermost
`hand` box is drawn bare.
Cost shape: `N×[payload]·carry(delivery)` — N instances, each paying the
payload tariff, times the delivery's carry premium, plus the word costs; with
lanes it is a SUM of one bracket per instance instead. Carry composes
multiplicatively down a nest.

## 1. The glyph table ({len(G)} glyphs)

Sorts: **matter** names a material; **effect** happens at a point; **delivery**
boxes the pile and decides where and when; **mod** edits the record of the box
that closes the pile; **operator** takes the one item beside it and produces one
of the others; **separator** (`lane`) opens a segment of the pile that belongs
to one instance. `repeat`: `add` = saying it again
adds one more of its axis (fire×2 throws twice the voxels, explosive×2 is twice
the power); `compose` = applying the mod again (shotgun×2 is 3·3 = 9, swift×2 is
×4, float×2 is gravity reversed). Deliveries do NOT merge on repeat: each one
boxes what is in front of it.

{glyph_table()}

Rules the table relies on: an operator with a missing required word is
**incomplete** — charged, does nothing (`transmute` needs both `A` and `B`;
`anything` and `air` are real words for the wildcard and the void). `anything`
is priced as the conversion it turns out to be PLUS the actual matter's value
times a surcharge, billed when it resolves. Every unary operator takes the ONE
item BEFORE it — which may be a launch box, so `explosive projectile echo` is a
turret and `explosive projectile aura self` is a status that fires a bolt every
tick. Only `transmute` is infix.

## 2. The worked sets

The sentences the grammar exists for, and the ones that would read wrong under
any other reading of the three rules.

{worked_md}
## 3. Every single word ({n1})

{singles}

## 4. The sentences from the brief ({nn})

{named}

## 5. Every ordered pair over a {len(ALPHA2)}-word alphabet ({n2} distinct casts from {p_ord} sequences)

Alphabet: {', '.join('`%s`' % a for a in ALPHA2)}. Sequences that lower to the
same cast (rule 1: order inside a pile is irrelevant) are listed once with their
other spellings.

{pairs}

## 6. Every ordered triple over a {len(ALPHA3)}-word core ({n3} distinct casts from {t_ord} sequences)

Core: {', '.join('`%s`' % a for a in ALPHA3)}.

{triples}
"""
    with open('docs/MAGIC_PERMUTATIONS.md', 'w', encoding='utf-8') as f:
        f.write(doc)
    print('wrote docs/MAGIC_PERMUTATIONS.md: %d glyphs, %d singles, %d named, %d pairs, %d triples'
          % (len(G), n1, nn, n2, n3))
    print('pairs   %d orderings -> %d distinct (%d non-fizzle)' % (p_ord, p_dist, p_live))
    print('triples %d orderings -> %d distinct (%d non-fizzle)' % (t_ord, t_dist, t_live))


if __name__ == '__main__':
    main()
