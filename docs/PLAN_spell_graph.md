# PLAN: the spell graph — lanes in the grammar, a tree on the page

2026-09-21. Design exploration, not yet scheduled. Read after DESIGN.md §8
("The spell system") and `docs/PLAN_magic_grammar.md`; this plan changes one
rule there and adds a view over the rest.

## 0. The idea, and the thesis

The user's picture: a spell drawn as a skill tree. Effects sit at the top,
things that do not depend on each other run in **parallel lanes**, lanes
**sum into a delivery** further down, a fan modifier like `shotgun` shows
`x3` beside it and **splits the tree into three lanes** that can each carry
something different (one bolt sprays gunpowder, the next sprays fire, so one
shot builds its own combo), and every level of the tree shows what it costs so
the multiplicative pricing is visible before the click. Calligraphic strokes
join the nodes.

Two findings drive everything below:

1. **The tree already exists.** `ParseSpell` (the three rules) produces a
   `SpellTree` whose shape IS this picture. Rule 1's pile is an unordered SET,
   which is what "parallel" means: `fire trail` and `explosive` in one pile
   are two lanes that meet at the box. Rule 2's box is the join node. An
   operator group is a serial link (it takes the one item before it). `also`
   is two roots side by side. Mods are tags on the join. Law L2 (pile
   commutativity) is the formal statement that lane order does not matter.
   The graph is a **view** of that tree, and the editor is a set of tree
   operations that write back as words.

2. **One thing the grammar cannot say: different payloads on different
   instances of one fan.** `explosive shotgun projectile` is three IDENTICAL
   exploding bolts; there is no sentence for "the middle bolt also carries
   fire". That is the only semantic extension the picture needs, and it is a
   fourth rule, not a rewrite: a `lane` word that opens a segment of the pile
   belonging to one instance.

So the sentence stays authoritative — grimoire pages stay word lists, the
number row still speaks, `spells-oracle` still gates the parser, PLYR v4 still
loads — and the graph is a second surface over the same truth (design
guideline 3: one owner per fact, derived data reconstructible). Everything the
graph can draw is a sentence; every sentence draws.

## 0b. Decided with the user, 2026-09-21

- **The page is THE interface.** The player builds a spell on the tree,
  binds it to a number key, and the key casts it. The word list is the save
  format and the parser's input, never a player-facing surface. The
  speak-one-word-per-key mode goes; the number row only casts bound spells.
- **Unrelated spells in one cast are a split at the root.** A `twin` on the
  root makes two columns; the left can end in a projectile, the right in a
  heal on self. That is the same split node used everywhere else in the tree,
  so **`also` is dropped**: a root split is what "two spells at once" means.
  In words: `lane explosive projectile lane blood mend self`. Cost adds per
  column exactly as L4 said it did for `also`; L4 moves to root lanes.
- **Copies fan, columns do not.** `SpellFan` spreads only instances that
  carry the SAME payload (the shared-only copies). An instance with its own
  lane fires on the aim, so a projectile in the right-hand column flies at the
  crosshair rather than a few degrees off it.

## 1. What the tree already is

`fire trail explosive shotgun projectile`, as the parser holds it and as the
page would draw it (root at the bottom, the tree grows upward with the number
of effects, which is the direction the user asked for):

```
        fire                          leaves: matter / effect words
          |
      (fire <trail)      explosive    an operator takes the ONE item below it
            \             /           (serial); siblings in a pile are lanes
             \           /            (parallel) and their order is irrelevant
              \         /
          [ ====bus===== ]  shotgun x3       mods are TAGS on the box, not
          [  o    o    o ]  <- 3 sockets     children: they edit its record
          [  PROJECTILE  ]  word 6 + tariff 71 -> x3 carry x3 = 639
                 |
               hand                   the implicit root; `also` = more roots
```

| Grammar | Graph |
|---|---|
| pile (rule 1, a set) | the lanes above a box; siblings, unordered |
| box (rule 2) | a join node: a bar with the delivery's name, its sockets above it |
| operator group | a serial node with slot pips; `_` = a hollow pip (drop target) |
| pending Mod (rule 3) | a tag hanging off the join it stuck to, showing its edit (`speed x2`) |
| `count` > 1 (shotgun) | N sockets on the join, fanned; `xN` beside the tag |
| nested box | a join node above another join node (a bolt that fires a bolt) |
| `also` | a second root beside the first |
| hand | the root bar, drawn bare (as the HUD brackets already do) |

Nothing in the runtime changes for this table. Sections 4–6 are the view and
the editor; only §2–3 touch the VM.

## 2. Rule 4: `lane` — a segment of the pile that belongs to one instance

**Word.** `lane`, sort `separator`, word cost 0, scope `pile` (where `also`'s
scope is `sentence`). Appended AFTER `also` in `glyphs.json` so the twenty
bound slots (`GrantAllAndBind` binds library order) do not shift.

**Rule 4. `lane` OPENS A SEGMENT OF THE PILE. The first segment is SHARED;
every `lane` word opens a segment that belongs to ONE instance of the box that
closes the pile.** When the delivery closes the pile it has a shared segment
and L lane segments. The box fires `instances = max(count, L)` instances
(count is what `shotgun` edits, as today). Instance i < L carries
`shared ∪ lane[i]`; instance i ≥ L carries `shared` alone. Lane segments keep
their SPOKEN order (like deliveries, they do not commute); lane 1 is instance
0, which `SpellFan` puts on the aim itself — so the first lane you speak is the
centre bolt, and the page draws that socket on the aim line.

**What a lane segment may hold.** Anything the pile may hold: Matter, Effect
words, operator groups (`fire trail` in a lane is a trail on that bolt only),
boxes (`explosive projectile` in a lane is a child that only that bolt
fires), and Mods. A Mod in a lane segment edits THAT instance's record
(`swift` in lane 2 = the second bolt is fast) — with one exception: **a
`count` Mod is always record-wide**, wherever it is spoken, because count is
the fan and a lane is one instance. A `lane` word with nothing after it is an
empty lane: the instance exists and carries the shared payload (it is how you
say "three bolts, only the middle one burns" without `shotgun`).

**Walls.** A `lane` mark walls operator binding: in `fire lane trail` the item
under `trail` is the mark, not `fire`, so `trail` is incomplete (charged,
`_`), exactly as if nothing were spoken before it. Runs merge only within a
segment: `fire lane fire` is two items in two lanes, not `fire×2`.

**Examples.**

| Sentence | Instances | What each carries |
|---|---|---|
| `explosive shotgun projectile` | 3 | all: explosive (unchanged from today) |
| `gunpowder lane fire projectile` | 2 | bolt 0: gunpowder; bolt 1: fire (no shared) |
| `explosive lane gunpowder lane fire projectile` | 2 | bolt 0: explosive+gunpowder; bolt 1: explosive+fire |
| `explosive lane fire shotgun projectile` | 3 | bolt 0: explosive+fire; bolts 1,2: explosive |
| `lane swift explosive lane heavy explosive projectile` | 2 | bolt 0: fast; bolt 1: falls hard; both explode |
| `explosive projectile lane fire projectile` | 2 | bolt 0: fires a child that explodes; bolt 1: the same child + fire |
| `explosive lane fire projectile also water self` | 2 + 1 | two bolts, and water at your own body |

The first row is the compatibility claim: **a sentence with no `lane` word
lowers bit-for-bit as it does today**, so the pinned world hash of every
gate is expected not to move.

**Price.** Today `tariff = tariff(shared) × instances`. With lanes,
`tariff = Σ_i tariff(shared ∪ lane[i])`, which reduces to the old product when
L = 0. Carry and word costs as before; the leaf cap (`maxInstances`) applies
to `max(count, L)` exactly as it applies to `count` now, and a lane past the
cap is clamped with the same "the fan was cut back" line.

**What rule 4 does NOT do.** A lane is not a clause: `also` stays the only
sentence separator and L4 (cost additivity) stays on `also`. There is no
per-lane count (a lane cannot itself fan; put a `shotgun projectile` box in
the lane if you want that, and the leaf cap prices it). A lane on the `hand`
box behaves like `shotgun` on the hand: fanned resolve points at reach, each
with its own payload.

**`twin`.** One JSON entry, sort `mod`, field `count`, `mul` 2. The "double
spell" the user named; `shotgun` already is the triple. Nothing in C++.

## 3. Lowering and runtime, concretely

Every change is a small extension of a struct or a loop that already exists.

- `SpellNode` gains `int32_t lane = 0` (0 = shared, 1..L) on every item of a
  box, and a box node gains `std::vector<int> laneAt` (the spoken position of
  each `lane` word, for the HUD highlight and the linearizer). `ParseSpell`
  keeps a `curLane` counter that a `lane` word increments and a closing
  delivery / `also` resets; `CloseBox` stamps it onto the items. `NodeKey`
  suffixes `@lane` so identical items in different lanes never merge.
- `DeliveryRec` gains `std::vector<SpellLane> lanes` with
  `struct SpellLane { DeliveryRec rec; std::vector<EffectInst> extra; }` —
  the same "record holds effects holds records" knot `EffectInst::launch`
  already ties, tied the same way (a vector, not a by-value member). `rec` is
  the shared record with the lane's Mods applied on top and the lane's trail
  merged in; `extra` is the lane's lowered nouns. `LowerBox` builds the shared
  record and shared payload as today, then one `SpellLane` per lane segment,
  and sets `cast.instances = max(count, L)` before the leaf-cap clamp.
- `PriceCast` sums per instance instead of multiplying; `EffectVolume` and the
  rule-2 `ticks` bound take the max over lanes.
- **Runtime: one helper, `InstanceCast(const SpellCast&, int32_t i)`**, returns
  the cast instance i actually flies with — `delivery = lanes[i].rec` when
  i < L, `payload = shared + lanes[i].extra`. `SpellProjectile` and
  `SpellBomb` already carry a `SpellCast` COPY each, so per-instance records
  cost nothing new. The three fan loops call it: `Cast()` (the hand's
  instances), `Launch()` / `RequestBody()`, and `AdoptLaunches` (nested boxes,
  which carry the record and therefore the lanes inside their `SpellLaunchReq`).
  `ApplySpellEffect` is untouched: it never sees a lane, only a payload.
- `ShowNode` draws a lane as `/`: `[explosive / gunpowder / fire PROJECTILE]`
  (Oracle style `∕`), which is also the HUD bracket string and the oracle
  comparison string. `DescribeCast` adds one sentence per lane ("The first
  bolt also sprays gunpowder; the second also sprays fire.").
- `scripts/magic_grammar.py` gets the same rule (segments on the pile, keyed
  by lane), `lane` enters the 19-word pair alphabet, `MAGIC_PERMUTATIONS.md`
  and `grammar_oracle.json` are regenerated (never hand-edited).
- Persistence: nothing. A page is still a word list and `lane` is a word.

**Laws.** L2 is restated as: permuting items WITHIN a segment lowers to an
identical cast list. New **L12 lanes**: for any E, F and flight delivery D,
`E lane F D` lowers to instances = 2 whose instance casts are exactly
`Lower(E D)` and `Lower(E F D)`'s payloads respectively; instances beyond L
equal `Lower(shared D)`; and a lane inside a nested box reaches the runtime
(`explosive lane fire projectile` launches two bolts and exactly one sprays
fire). The `spells` gate asserts L12 over the same generated alphabet; L1, L3,
L4, L6–L11 hold unchanged and are re-run.

## 4. The graph: layout and what each node draws

Lives VM-side, imgui-free, so it is testable: `src/game/spellgraph.h/.cpp`.

**Layout.** The tree is rooted (the hand box; several roots under `also`) and
the drawing is layered: a node's layer is its depth from the root, root at the
bottom, layers stacked upward at a fixed pitch. Within a layer, the classic
tidy-tree rule (leaves take slots left to right in canonical pile order, a
parent is centred over its children, subtrees pushed apart to a minimum gap).
Sizes are integers in chrome pixels (2× sprites), so the layout is exact and
the round trip of a hit test is trivial. `BuildGraph(lib, tree, cast list) →
SpellGraph { nodes[], edges[] }`, where a node is `{ treeNode, kind, x, y, w,
h, label, sortColour, price, socket, lane }`.

**Node kinds and their chrome** (all sprites authored at 1× in
`scripts/gen_ui_chrome.py`, drawn at 2×, zero rounding — the pixel-art rule
holds here as it does on the character screen):

- **Word** (Matter, Effect): the existing glyph cell (`GlyphContents`), sort
  colour on the rim, `×N` badge for multiplicity, hover opens the §9 info box.
- **Operator**: a cell with one pip on the left for a unary word, pips both
  sides for `transmute`. A filled pip has a stroke running into it; an EMPTY
  REQUIRED pip is a hollow ring with `_` — the visual answer to "which words
  need a prefix" — and it is a drop target. An incomplete operator's whole
  node is dimmed and its cost shown struck through (charged, does nothing).
- **Join** (a box): a wide bar with the delivery's noun, `×N` weight, and
  its sockets in a row above the bar when `instances > 1`. Shared lanes run
  into a **bus** (a horizontal stroke above the sockets) that feeds every
  socket; a lane's subtree runs into its own socket only. Socket 0 sits on the
  bar's centre line (the aim). Mods hang as tags on the bar's left end, each
  reading its edit (`speed ×2`, `gravity −1 g`, `count ×3`); a wasted mod tag
  is dimmed with the reason in its tooltip (the same `wastedMods` line).
- **Root**: the hand bar, bare, with the cast's total under it in the three
  parts the HUD already shows (word / tariff / carry) and the mana it would
  take from the pool drawn as a short `ValueBar`.
- **Page**: a word that came from a grimoire page is drawn with the page's
  name as a small seal on the cell (the expansion is what is drawn; the seal
  says where it came from).

**Cost at every level.** `LowerBox` already computes a `SpellCast` per box and
then keeps only its payload; `CastList` gains `boxPrice[node] = { wordCost,
tariff, carryCost, instances, leaves, instancesClamped }` filled on the way
out. A join draws `tariff → ×carry ×instances = subtotal` under its bar, so
`shotgun` reads `×3` on its tag AND the bar under it shows the tripled
number; a nested bolt shows `×3.0 × 3.0`. The user's request that the price be
legible at every level is one side table and one label.

**Strokes.** `ui::InkStroke(dl, from, to, colour, weight)` in `theme.h`: a
cubic from a child's bottom-centre to its parent's top (a vertical S, control
points straight down / straight up), flattened to segments and drawn as an
un-antialiased polyline whose width follows a brush profile — thin at the
tip, full at the belly, thin into the join — with the width quantised to
whole 2 px steps so it reads as a calligraphic stroke drawn with a pixel
brush rather than a vector spline. Gold on obsidian for the trunk, the sort's
colour for a leaf's stroke, a dim stroke for an incomplete or wasted branch.
A fan is N strokes leaving one point below the bar. The bus is a straight
stroke with a small diamond where each lane meets it. `ImDrawListFlags_AntiAliasedLines` is off for the canvas.

## 5. Editing: gestures on the graph, words on the page

The composer keeps `grimoireEditWords` as the thing it owns and undoes. The
graph editor never edits words directly: a gesture becomes a **tree op**, the
op is applied to the parsed tree, and the tree is **linearized** back to
words. Two views, one truth, and the undo stack (`grimoireUndo`, 32 deep,
dropped when the open page changes) is reused unchanged.

**Gestures → ops.** Drag sources are the ones that exist (`kPayloadWord`
from the arsenal table, `kPayloadPage` from the page list, and a node of the
graph itself).

| Drop of… | onto… | Op |
|---|---|---|
| a noun / page | a join's bus, or the empty space above a root | `InsertItem(box, lane 0, glyph)` |
| a noun / page | a socket | `InsertItem(box, lane i, glyph)` (opens the lane if empty) |
| a noun | an operator's hollow pip | `FillSlot(group, side, glyph)` |
| a delivery | a subtree | `WrapInBox(node, delivery)`: that item alone is boxed; its siblings stay in the outer pile |
| a delivery | a join's bus | `WrapInBox(box, delivery)`: nests the whole box (a bolt that fires this) |
| a delivery | empty space | `InsertItem(root, 0, delivery)` as an empty box (the kinetic hit) |
| a mod | a join | `AttachMod(box, lane 0, glyph)` |
| a mod | a socket | `AttachMod(box, lane i, glyph)` |
| a graph node | outside the canvas | `Remove(node)` (the subtree) |
| a graph node | another socket / bus | `Move(node, box, lane)` |
| `ctrl` held | any of the above from the graph | copy instead of move |

Every op is TOTAL (it either applies or is refused with a reason on the
status line, as the word row does now: page full, authored page, would
exceed the instance cap) and every cell PEEKS at the payload
(`AcceptBeforeDelivery`) so the graph draws the edit before the release —
a gold ring on the socket that will take it, a ghost of the node where it
will land, a red ring and a reason where it is refused.

**The linearizer** (`Linearize(lib, tree) → words`), post-order:

- a word: its id, repeated `n` times;
- a group: `left` then the operator then `right` (an infix), or `left` then
  the operator (a unary) — an EMPTY left slot emits the operator FIRST in its
  pile so nothing to its left can bind to it;
- a box: the shared segment's items, then for each lane `lane` + its items,
  then the delivery word `n` times — items ordered so that any group with an
  empty left slot comes first, then the rest in canonical key order;
- a clause: its root's items; clauses joined by `also`.

Round trip is the gate (§7): for every sentence in the oracle corpus,
`Parse(Linearize(Parse(s)))` equals `Parse(s)` node for node, and
`Lower` of both is identical. The one tree the rules can build that no word
order rebuilds — two incomplete unary operators in one segment where each
accepts the other's result sort (`(_ echo)` and `(_ null)`: `null` takes
`any`, `echo` takes an Effect, which `null`'s result is) — is refused by
`InsertItem` with "an empty `echo` beside an empty `null` cannot be spoken;
fill one first". Incomplete operators are charged no-ops, so nothing
castable is lost. The 16-word bounds stay: `kSpellStackMax` (C++) and
`budgets.maxMacroWords` (JSON) are the graph's size cap, and a lane-heavy
page will want them raised together (24 is enough for three lanes of two
effects, a fan, a delivery and a mod); that is one constexpr and one JSON
row, done in the phase that lands lanes.

**Speaking a graph page** is unchanged: it expands to words on the stack.
There is nothing a page can hold that the number row cannot speak.

## 6. Code shape

| Piece | Where | Needs a build? |
|---|---|---|
| `lane` word, `twin` | `assets/spells/glyphs.json` (appended after `also`) | no |
| rule 4 in the parser, lanes on records, `InstanceCast`, per-lane pricing, describe, `/` brackets, `boxPrice` | `src/game/spell.h/.cpp` | yes |
| rule 4 in the reference, regenerated tables | `scripts/magic_grammar.py`, `docs/MAGIC_PERMUTATIONS.md`, `assets/spells/grammar_oracle.json` | no |
| L2', L12 in the laws; lane runtime checks | `src/test/selftest_spell.cpp` | yes |
| layout, linearize, tree ops (imgui-free) | `src/game/spellgraph.h/.cpp` (new) | yes |
| `spell-graph` gate: round trip over the corpus, every op total | `src/test/selftest_spellgraph.cpp` (new, CPU-only, beside `grimoire`) | yes |
| `UIState::SpellGraphUI` mirror (nodes, edges, prices, hover text) filled by main.cpp each frame from the composer's expansion; `GraphEditIntent` latch (op, node, lane, glyphId, copy) consumed by main.cpp, which applies the op and writes the new words into `grimoireEditWords` | `src/ui/overlay.h`, `src/main.cpp` | yes |
| the canvas in the composer, drop targets, peek drawing, the HUD mini-graph | `src/ui/spellgraph_ui.cpp` (new), `inventory_ui.cpp` (`GrimoireBody` gains a canvas above the word row; the row stays as the spoken form), `overlay.cpp` | yes |
| `InkStroke`, pip / socket / bus / seal sprites | `src/ui/theme.h/.cpp`, `scripts/gen_ui_chrome.py`, `assets/ui/chrome.*` | yes |
| DESIGN.md §8 (rule 4, the page), tuner wiki "language" page and `ARCH_NODES` | docs, `assets/tuner.html` | no |

The mirror-and-intent split is the pattern the composer already uses
(`grimoireEditReadout` is filled by main.cpp, `GrimoireIntent` is consumed
by it): `overlay.h` stays imgui-free and spell-free, and the UI never holds a
glyph index across a frame (they die on R reload — §8b, everything crosses by
name).

**HUD.** In magic mode the live sentence is drawn as the same graph at half
scale beside the bracket text (the mirror is the same struct, filled from
`caster.compiled`). Optional; the bracket string stays for the dev panel and
the oracle.

## 7. Verification (a budget, not a reflex)

- `--gate spells` once after phase 1: the laws over the alphabet with `lane`
  in it, plus L12's runtime check. Expected hash: unmoved (no existing
  sentence contains `lane`; new glyphs appended after `also`; word costs of
  existing glyphs untouched). If it moves, that is a notification and
  `--rebaseline` is the whole response.
- `--gate spells-oracle` once after regenerating the tables.
- `--gate spell-graph` while iterating on phase 2 (CPU-only, seconds).
- `--shot-inventory` writes a fourth frame, `screenshot_inventory_graph.bmp`:
  the `duststorm` starter open with a lane added, so the socket, bus, tags and
  per-level prices are all in one picture. That is the whole UI check; do not
  boot the game to look.
- Full acceptance ONCE, on the tree that ships.

## 8. Phases

1. **Lanes** (C++ + Python + JSON; one build). Rule 4, records, runtime,
   pricing, describe, laws, oracle regenerated, `twin`, stack cap to 24.
   Shippable alone: `explosive lane fire projectile` works from the number
   row with nothing drawn.
2. **`spellgraph`** (C++; one build). Layout, linearize, ops, the
   `spell-graph` gate. No UI yet; the gate is the proof.
3. **The page** (C++; one build). Canvas, drops, peek, strokes, sprites,
   the fourth shot frame. DESIGN.md §8 and the tuner in the same commit.
4. **HUD mini-graph** (optional, small).

Each phase is one worktree agent with one build; phases 1 and 2 could run in
parallel since 2 only needs `SpellNode::lane` agreed (this document is the
agreement).

## 9. Open decisions (choose; the plan works either way)

1. **Lane → instance order.** Spoken order, lane 1 = instance 0 = the aim
   (proposed; makes "the middle bolt" sayable). Alternative: canonical key
   order, which keeps L2 total over the whole pile but makes the centre bolt
   whichever lane sorts first — worse for the page, where the user placed it.
2. **A lane past `count` grows the fan** (`instances = max(count, L)`,
   proposed) versus a lane past `count` being wasted like a mod on the hand.
   Growing is what the picture asks for ("double spell" is just two lanes).
3. **`count` inside a lane** is record-wide (proposed) versus refused. Either
   is one line; record-wide keeps every sentence total.
4. **Segment mods and canonical order.** A lane's mods compose in canonical
   order within the lane, as the shared ones do — keeps `swift slow` ≡ `slow
   swift` inside a lane.
5. **The word row** stays under the canvas as the spoken form (proposed) or
   goes. Keeping it costs nothing and is the honest view of what a bound key
   will say.
