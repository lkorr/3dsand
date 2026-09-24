# PLAN: better spells — magnitude, triggers, duration, shape, condition

2026-09-23. Supersedes `docs/PLAN_spell_axes.md` (deleted). Read after DESIGN.md
§8 ("The spell system") and `docs/PLAN_spell_graph.md`.

## 0. Why the old plan was replaced

`PLAN_spell_axes` had the right diagnosis — every word answers WHAT or WHERE,
and the spells that make Noita/Morrowind deep also answer HOW MUCH, WHEN, HOW
LONG and IF — but it spent most of its complexity on an interface the player
never sees. Every idea was bent into a spoken WORD so the text grammar, the
linearizer, `Speakable` and the Python oracle could carry it: timing became six
unary-operator words folded onto edges and unfolded again; magnitude needed a
law reconciling grades with repetition; conditions became two lanes because
the grammar has no `else`. It then added ~30 words to a thesis that said "the
player learns six concepts, not fifty words".

**The player only ever uses the 2D spellbook page.** So:

- **The graph is the authoring surface; the word list is the SERIALIZATION.**
  Pages stay word lists (no save-format break), but a new feature does not have
  to be a word with a place in word order. A per-node or per-edge PROPERTY is
  serialized as a suffix on the word it belongs to (`float@0.5`). The word-order
  grammar, the oracle and the round-trip laws stay exactly as they are for
  everything they already cover; properties are tested by C++ gates.
- **A number is edited on the node, not by repeating a word.** `float float`
  keeps working (old pages, the oracle corpus), but the page offers magnitude.
- **One component per quantity.** Opposite pairs (`float`/`heavy`,
  `swift`/`slow`, `gust`/`implode`) become one component with a signed or
  scaled magnitude (M2).
- **Legibility over ornament.** No six stroke styles, no timeline view, no
  draggable hourglass beads. A property is a number in the hover card and one
  small mark on the canvas.

What does NOT move: CLAUDE.md rules 1–3 (every effect leaves as ops on the
MutationQueue; every sustained thing is bounded; the VM is integer fixed-point),
and **compatibility**: a page with no suffix lowers bit-for-bit as today, so no
pinned spell price or spell gate moves while this lands.

## 1. Packages, in order

| # | Package | Size | Depends |
|---|---|---|---|
| **M1** | Magnitude: one number per node, real units, convex price, scroll to edit — **LANDED 2026-09-23** (gate `spell-magnitude`) | S–M | — |
| **M2** | Signed components: `lift`, `speed`, `wind` replace the opposite pairs — **LANDED 2026-09-23** (in `spell-magnitude`) | S | M1 |
| **M3** | Triggers on payload items: hit / bounce / expire / launch / every N / delay; `echo` magnitude = repeat — **LANDED 2026-09-23** (gate `spell-timing`) | M | M1 |
| **M4** | Duration: conjured (decaying) matter | M | M1 |
| **M5** | Shapes: wall / pillar / disc / line | M (touches `common.wgsl`) | — |
| **M6** | Conditions (hit body / hit terrain / caster state) + teleport + body impulse | M | M3 |
| later | Enchanting hooks, flight paths, acquisition (found glyphs, mob casters) | L | M3 |

Each package lands on its own, updates DESIGN §8 and the tuner `ARCH_NODES` in
the same commit.

---

## 2. M1 — Magnitude

### 2.1 Model

Every spoken word carries a **magnitude** `mag`, an integer in per-mille of
the glyph's authored quantity. Default 1000 = today's word. It lives on the
spoken stack (`SpellStack::mags`, parallel to `spoken`), on the parse node
(`SpellNode::mag`) and on the lowered effect (`EffectInst::mag`).

Multiplicity stays. The effective scale of an item is `n × mag / 1000`
(`n` from repetition, `mag` from the page). At `mag = 1000` every formula
reduces to the integer-`n` one it replaces, **exactly** — that is the
compatibility law (L-M0) and the gate asserts it.

### 2.2 What it scales

Per glyph, in `glyphs.json`:

```json
"magnitude": { "min": 0.25, "max": 4, "step": 0.25 }
"magnitude": false
```

Defaults when absent: Matter, Effect and non-count Mods are graded 0.25..4 in
steps of 0.25; `bounce`/`pierce`/`seek` step by whole units (a half bounce is
nothing); Deliveries, Separators and count Mods (`shotgun`, `twin`) are
ungraded; Operators are graded only where they opt in (`transmute`, `mend`,
`trail`, `null`).

| Kind | Magnitude scales | At `mag` = 500 / 2000 |
|---|---|---|
| Matter (spray) | voxels sprayed | half / double |
| `place`, `transmute`, `null` | volume (radius × ∛) | |
| `explosive` | power and volume | |
| `gust`/`implode` | wind speed | |
| `mend` | voxels per tick | |
| `trail` | voxel budget | |
| Mod, op `add` (`float`, `heavy`, `bounce`, `fuse` …) | the amount added | `float` −0.5 g / −2 g |
| Mod, op `mul` (`swift`, `long`, `wide`) | the factor: `amount × mag` | `swift` ×1 / ×4 |
| Mod, op `div` (`slow`) | the divisor | |

The multiplier on a `mul` mod is the displayed number, linear in the handle:
`swift` at 0.75 is ×1.5. (The old plan interpolated `swift@½` to ×1.5, which no
player could predict.) A factor is clamped so it never inverts (≥ 1 for mul,
divisor ≥ 1 for div).

### 2.3 Price

The tariff already prices the world effect (bigger blast, more voxels), so it
follows `mag` for free. The WORD cost is the only price on a mod with no tariff
(`swift`, `seek`, `float`), and it is **convex**:
`word × mag² / 1000²`, rounded up, minimum 1. Morrowind's sliders collapsed to
"max magnitude" because price was linear; quadratic makes the middle values the
efficient ones. At `mag = 1000` it is `word`, unchanged.

### 2.4 Serialization

`name@value` in a page, where value is the magnitude as a decimal
(`float@0.5`, `explosive@2.25`). Omitted = 1. Parsed by `SplitMagnitude` at the
two places words become glyphs (`ExpandWords`, `WordsToGlyphs`/`ParseWords`);
written by `Linearize` and page capture. A value off the glyph's step is
snapped; out of range is clamped; on an ungraded glyph the suffix is ignored.
Runs merge only between equal magnitudes; `NodeKey` carries `~mag` when it is
not 1000, so every existing key is unchanged.

### 2.5 On the page

- **Scroll over a node** steps its magnitude by the glyph's step (ctrl = ×4).
  The wheel over the dark canvas still zooms.
- The hover card shows the value in units ("gravity −0.50 g", "speed ×1.5",
  "power 330") and the live price.
- A graded node whose magnitude is not 1 shows the value as a small numeral in
  the cell's corner; that is the whole visual.
- `EditResult SetMagnitude(lib, tree, node, mag)` is the op, total and
  self-proving like every other (`spellgraph.h`).

### 2.6 Gate `spell-magnitude`

1. L-M0: for every sentence in the `spells` gate's alphabet, the stack with
   explicit `mag = 1000` lowers identical to the stack without `mags`.
2. `float@0.5 aura self` lifts less than `float aura self`; `float@2` more
   (price and the impulse number, not a physics run).
3. `explosive@0.5` power is half, `explosive@2` double; `swift@1.5` speed is ×3
   of base; `slow@2` is /4.
4. Word price is non-decreasing in `mag` for every graded glyph, and convex.
5. `name@x` round-trips through `Linearize(ParseWords(...))` and through a
   grimoire page expansion; ungraded suffixes are dropped; out-of-range values
   clamp.

## 3. M2 — Signed components

**As landed:** the gravity component is named `lift` (magnitude = g of lift,
default 1, range −3..3) so scrolling UP means MORE antigravity; `speed` is a
multiplier (default ×2, 0.25..4); `wind` is a burst (default 1, −3..3,
negative pulls). Glyphs gained `magnitude.default` and `hidden`. The
carrier-preset collapse below was not done.

`gravity` (−3 g..+3 g), `speed` (×0.25..×4), `wind` (push/pull, signed speed)
replace `float`/`heavy`, `swift`/`slow`, `gust`/`implode` in the word column.
The old words stay loadable as aliases that lower to the new component at the
equivalent magnitude, so old pages do not break; they are hidden from the
page's word column. Oracle and `MAGIC_PERMUTATIONS.md` regenerate. Also:
consider collapsing `projectile`/`bolt`/`lob`/`orb` to fewer presets once
speed/gravity/life are magnitudes — decide with the user after M2.

## 4. M3 — Triggers on the box edge

**As landed:** the timing lives on EVERY payload item (a word, an operator
group, or a nested box), not only on nested boxes — "the edge to the parent"
is the same thing for all of them. Suffix grammar `id[@mag][!trigger[N]][+D]`.
Repeat is `echo` with a magnitude rather than a new edge property. The page
opens the timing menu on a plain click of the cell (not a click on the stroke),
and the tag sits at the cell's top edge where the stroke leaves. A delayed or
`every` child box is born at generation 0 from the echo queue (see DESIGN §8).

The edge from a nested box (or a payload item) to its parent gets:

| Property | Values | Default |
|---|---|---|
| trigger | `hit`, `bounce`, `expire`, `launch`, `every N ticks` | `hit` |
| delay | ticks after the trigger | 0 |
| repeat | fires `n` times, `k` ticks apart | once |

Unifies the three unrelated timing mechanisms that exist now: `fuse` (mod),
`echo` (operator), `trail` (operator yielding a mod — which is `every voxel`).
Those words stay and lower onto the same runtime record.

Runtime: `SpellEcho` generalizes to one pending queue
(`{inner, at, dir, delay, period, left}`), with a per-caster cap in
`budgets`. Trigger sites in the flight loop: bounce at the `bouncesLeft--`
branch, expire at the `ticksLeft` branch, launch at `Launch()`, `every N` on a
per-projectile phase counter. Each goes through the same resolve the impact
uses. Price: a trigger that can fire k times is priced × k (bounces + 1,
life / period); delay is free.

Page: click an edge → a small picker. One icon on the stroke per non-default
trigger, a numeral for delay/repeat. Serialization: a suffix on the box's
delivery word (`projectile!bounce`, `projectile+20`, `projectile*3/10`).

## 5. M4 — Duration: conjured matter

A Matter node gets a duration property: permanent (default) / brief (~2 s) /
held (~10 s) / lasting (~60 s). Implemented as a MATERIAL, not a timer: the
loader expands a `conjurable` list into `conjured_<m>_<tier>` materials with a
decay-to-air reaction at the tier's per-tick chance, a paler arcane tint, and
`arcane × tier` so temporary matter is cheaper. It survives being moved,
blown and saved with no ledger; decay is statistical, which is accepted. Rule
2: conjured cells keep chunks awake until gone — the gate asserts they sleep
again (≤32 awake at rest). Material ids append (id IS index).

## 6. M5 — Shapes

A shape property on the resolve: sphere (default), wall, pillar, disc, line.
`BrushOp` grows to carry a shape and a quantized axis; `sim_mutate.wgsl` tests
membership with integer math; shape 0 is today's sphere path exactly, so the
hash does not move. Touches `world.h` and `common.wgsl` (claim both; a
`common.wgsl` edit is a full shader-cache miss — do it once). `EffectVolume`
computes each shape's exact cell count so the tariff stays per voxel. With M4
this is walls, bridges, cages — the building/defence category the system lacks.

## 7. M6 — Conditions, teleport, impulse

- **Conditions** as an edge property (M3's edge): `if hit body`, `if hit
  terrain`, `if caster hurt`, `if caster airborne`, and their negations. All
  exact at any range (body ray, caster state). Material conditions are NOT in
  v1: the CPU mirror only sees ~48 voxels, so they would be silently false at
  range, and the per-cell case is already `transmute`'s GPU from-filter. A
  guarded item is billed on resolve, so a branch that did not fire costs only
  its word.
- **`teleport`** (effect): moves the caster to the resolve point, destination
  checked clear by probe, else fizzles charged. With a body at the point it
  swaps. Movement magic is the biggest missing category.
- **`impulse`** (effect, signed magnitude): a velocity change on the body at
  the point, through `Mob::AddLift`'s seam generalized to a direction.

## 8. Later

Enchanting (a page bound to an item event: strike, struck, step, worn),
flight paths (orbit, wave, boomerang), acquisition (found glyphs raise a
per-glyph magnitude ceiling; mob casters drop them), wild surges. All wait on
M3. The old plan's `drain`, `mimic`, `sacrifice`, the alchemical refund and
self-recursion are dropped.

## 9. Open questions

- M2: do old words stay visible, or only the signed components?
- M3: does a bomb get bounce events from Jolt contacts cheaply?
- M4: conjured liquids decay by fullness or whole cell?
