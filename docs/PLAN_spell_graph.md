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
   operator group is a serial link (it takes the one item before it). A root
   lane is two columns side by side. Mods are tags on the join. Law L2 (pile
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
               hand                   the implicit root; a root lane = a column
```

| Grammar | Graph |
|---|---|
| pile (rule 1, a set) | the lanes above a box; siblings, unordered |
| box (rule 2) | a join node: a bar with the delivery's name, its sockets above it |
| operator group | a serial node with slot pips; `_` = a hollow pip (drop target) |
| pending Mod (rule 3) | a tag hanging off the join it stuck to, showing its edit (`speed x2`) |
| `count` > 1 (shotgun) | N sockets on the join, fanned; `xN` beside the tag |
| nested box | a join node above another join node (a bolt that fires a bolt) |
| `lane` … `end` (rule 4) | a SOCKET on the join above it: one instance's own subtree. On the hand, a second column beside the first |
| hand | the root bar, drawn bare (as the HUD brackets already do) |

Nothing in the runtime changes for this table. Sections 4–6 are the view and
the editor; only §2–3 touch the VM.

## 2. Rule 4: `lane` / `end` — a scope of the pile that belongs to one instance

*(Rewritten 2026-09-21 while package A landed: the first draft of this section
made a lane a SEGMENT and had a delivery box the whole pile, which meant a lane
could never hold a spell with its own trailing delivery — the trailing word
swallowed the earlier lanes. "Socket 1 = a bolt that fires a bolt, socket 2 =
fire" could not be said at all, and the page must be able to say every tree it
can draw. The rule below is explicit SCOPES, and it is what shipped.)*

**Words.** `lane` and `end`, sort `separator`, word cost 0, appended after the
existing glyphs in `glyphs.json` so the twenty bound slots (`GrantAllAndBind`
binds library order) do not shift. Which of the two a separator is, is content:
`"scope": "open" | "close"`.

**Rule 4. `lane` OPENS a lane scope on the current pile; `end` CLOSES the
innermost open one, back to the scope around it. A DELIVERY BOXES THE INNERMOST
OPEN SCOPE** — inside an open lane, only that lane's items (the box lands *in*
the lane, the lane stays open, and more items may follow in it); outside any
open lane, the shared items plus all the closed lanes, which is the
multi-socket box. An unclosed lane at the end of the sentence is closed
implicitly. Lanes are ordered as spoken; lane *i* belongs to instance *i*. The
box fires `instances = max(count, L)` (count is what `shotgun` and `twin`
edit); instance *i* < L carries `shared ∪ lane[i]`, and instance *i* ≥ L
carries `shared` alone. Lane 1 is instance 0, which fires on the aim itself —
so the first lane you speak is the centre bolt, and the page draws that socket
on the aim line.

**What a lane may hold.** Anything the pile may hold: Matter, Effect words,
operator groups (`fire trail` in a lane is a trail on that bolt only), whole
BOXES with their own deliveries (`lane explosive projectile end` is a socket
holding a bolt), and Mods. A Mod inside a lane edits THAT instance's record
(`swift` in lane 2 = the second bolt is fast) — with one exception: **a `count`
Mod is always record-wide**, wherever it is spoken, because count is the fan
and a lane is one instance of it. A `lane end` with nothing between them is an
empty lane: the instance exists and carries the shared payload (it is how you
say "three bolts, only the middle one burns" without `shotgun`). Marks NEST
rather than enumerate — `lane lane fire end end` is one lane holding one lane,
and an inner lane nobody boxed flattens into the outer when it closes, because
there is no record for it to be a column of.

**Walls.** A mark walls operator binding: an operator binds only within its own
scope, so in `fire lane trail` the item under `trail` is the wall, not `fire`,
and `trail` is incomplete (charged, `_`). Runs merge only within a scope:
`fire lane fire end` is two items in two scopes, not `fire×2`.

**Examples** (all of these are the gate's worked sentences):

| Sentence | Instances | What each carries |
|---|---|---|
| `explosive shotgun projectile` | 3 | all: explosive (unchanged from today) |
| `explosive twin projectile` | 2 | both: explosive, fanned |
| `explosive lane sand end lane fire end twin projectile` | 2 | bolt 0: explosive+sand; bolt 1: explosive+fire |
| `explosive lane fire end twin projectile` | 2 | bolt 0: explosive+fire; bolt 1: explosive |
| `explosive lane fire end projectile` | 1 | the one bolt: explosive+fire (≡ `explosive fire projectile`) |
| `lane explosive projectile end lane fire end projectile` | 2 | bolt 0: fires a child that explodes; bolt 1: sprays fire |
| `lane explosive projectile fire end projectile` | 1 | the one bolt: a child that explodes, and fire |
| `lane explosive projectile end lane blood mend self end` | 2 | a bolt at the aim, and a graft on your own body |

The first two rows are the compatibility claim: **a sentence with no mark
lowers bit-for-bit as it does today**, so the pinned world hash of every gate is
expected not to move.

**Price.** Today `tariff = tariff(shared) × instances`. With lanes,
`tariff = Σ_i tariff(shared ∪ lane[i])`, which reduces to the old product when
L = 0. Carry and word costs as before; the leaf cap (`maxInstances`) applies
to `max(count, L)` exactly as it applies to `count` now, and a lane past the
cap is clamped with the same "the fan was cut back" line.

**What rule 4 does NOT do.** There is no per-lane count (a lane cannot itself
fan; put a `twin projectile` box in the lane if you want that, and the leaf cap
prices it). A lane on the `hand` box behaves like `shotgun` on the hand: fanned
resolve points at reach, each with its own payload — except that a lane-carrying
instance is not a copy, so it resolves on the aim rather than beside it. And
there is no sentence separator: **`also` is gone**, and two unrelated spells in
one cast are two root lanes (`lane … end lane … end`), which is where law L4
now attaches.

**`twin`.** One JSON entry, sort `mod`, field `count`, `mul` 2. The "double
spell" the user named; `shotgun` already is the triple. Nothing in C++.

**The cost of a socket is two words**, so `kSpellStackMax` and
`budgets.maxMacroWords` are 32.

## 3. Lowering and runtime, concretely

Every change is a small extension of a struct or a loop that already exists.

- `SpellNode` gains `int32_t lane = 0` (0 = shared, 1..L) on every item of a
  box, and a box node gains `std::vector<int> laneAt` — TWO spoken positions
  per lane, the `lane` word then the `end` word (-1 when the lane was closed
  implicitly), for the HUD highlight and the linearizer, so `laneAt.size() ==
  2 * L`. `ParseSpell` keeps a SCOPE STACK rather than a counter: `lane` pushes
  a scope, `end` pops it into its parent's lane list, a delivery boxes the
  innermost open one, and `CloseBox` stamps the lane onto the items before it
  merges them. `NodeKey` suffixes `@lane` so identical items in different lanes
  never merge.
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
- `ShowNode` draws each lane as `/ … /`: `[explosive twin / sand / / fire /
  PROJECTILE]` — one mark per word the sentence contains, the same string in
  both bracket styles, so the HUD readout, the oracle comparison and the
  linearizer all agree. `DescribeCast` adds one sentence per lane ("The first
  bolt also sprays sand. The second bolt also sprays fire.").
- `scripts/magic_grammar.py` gets the same rule (segments on the pile, keyed
  by lane), `lane` enters the 19-word pair alphabet, `MAGIC_PERMUTATIONS.md`
  and `grammar_oracle.json` are regenerated (never hand-edited).
- Persistence: nothing. A page is still a word list and `lane` is a word.

**Laws.** L2 is restated as: permuting items WITHIN a scope lowers to an
identical cast list. L4 moves from `also` to ROOT LANES: `lane A end lane B
end` costs cost(A) + cost(B) and fires two instances carrying exactly what `A`
and `B` lower to. New **L12 lanes**, over the generated alphabet: (a) `E lane F
end D` lowers to ONE instance carrying exactly what `E F D` carries, at the
same price; (b) `E lane F end twin D` fires two, instance 0 = `E F D`'s
payload, instance 1 = `E D`'s, tariff = their sum; (c) a sentence with no mark
grows no lane anywhere in its lowering; (d) **a lane may hold a box of its
own** — in `lane E D end lane F end D` socket 0 holds exactly the box `E D`
lowers to and socket 1 holds `F`, which is the sentence the scope rule exists
for. The runtime half is check (9): `explosive lane sand end lane fire end twin
projectile` fires two bolts, one with sand and one with fire; `lane explosive
projectile end lane blood mend self end` leaves a bolt AND a graft; `lane
explosive projectile end lane fire end projectile` is one carrier with two
sockets. L1, L3, L5–L11 hold unchanged and are re-run.

## 4. The graph: layout and what each node draws

Lives VM-side, imgui-free, so it is testable: `src/game/spellgraph.h/.cpp`.

**Layout.** The tree is rooted (the hand box, whose root lanes are its
columns) and
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
- a box: its shared items, then for each lane `lane` + that lane's items +
  `end`, then the delivery word ×n (`end` is emitted even for the last lane, so
  a box spoken after it closes the outer scope and not the lane).

Round trip is the gate (§7): for every sentence in the oracle corpus,
`Parse(Linearize(Parse(s)))` equals `Parse(s)` node for node, and
`Lower` of both is identical. The one tree the rules can build that no word
order rebuilds — two incomplete unary operators in one segment where each
accepts the other's result sort (`(_ echo)` and `(_ null)`: `null` takes
`any`, `echo` takes an Effect, which `null`'s result is) — is refused by
`InsertItem` with "an empty `echo` beside an empty `null` cannot be spoken;
fill one first". Incomplete operators are charged no-ops, so nothing
castable is lost. The word bound is the graph's size cap: `kSpellStackMax` (C++) and
`budgets.maxMacroWords` (JSON), raised together to **32** in the phase that
landed lanes, because a socket costs two words (`lane` … `end`) and three
sockets of two effects, a fan, a delivery and a mod do not fit in fewer.

**Speaking a graph page** is unchanged: it expands to words on the stack.
There is nothing a page can hold that the number row cannot speak.

## 6. Code shape

| Piece | Where | Needs a build? |
|---|---|---|
| `lane` / `end` words, `twin` | `assets/spells/glyphs.json` (appended; `also` removed) | no |
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
  sentence contains a mark; new glyphs appended; word costs of
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
