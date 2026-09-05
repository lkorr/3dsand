# PLAN: the magic grammar

Status: LANDED (2026-09-04, P0–P5 on branch `worktree-magic-grammar`; DESIGN.md
§8 "The spell system" is the record of what shipped and where it differs from
the text below — L6 is stated over every group's neighbourhood, Matter under
`trail` lowers to `place`, PLYR went to v4 (it was already v3), the cauterise
rule measures a third of the exposed surface, a severed limb is not regrown,
and mob bodies have no impulse seam for a sustained `float`). The exploratory
slice in `game/spell.*` + `assets/spells/glyphs.json` stayed; this plan replaced
its *language* while keeping its four structural commitments:

1. only output is `SpellEmission` → MutationQueue ops,
2. `ApplySpellEffect(payload, at, dir)` is position-parameterized (backfire is
   the same call at the caster),
3. integer 24.8 VM,
4. not player-coupled (`CasterHealth` callback, opaque caster id).

The goal is stated by the brief: **hundreds of glyphs, uncountably many
sequences, every one of them does the predictable literal thing, and nobody
ever writes a rule for a specific combination.** The way to get that is the
way programming languages get it: a handful of *sorts* (types), a fixed
*valence* per word (what it binds and on which side), a tiny set of
*primitives* every effect lowers to, and a *tariff* that prices what the spell
does to the world rather than the words it used. Then "will X + Y + Z work" is
answered by a type rule, not by a test of X + Y + Z.

---

## 0. What the current slice does, and what changes

| Today (`CompileSpell`) | This plan |
|---|---|
| Three glyph types: element / form / modifier | Four sorts: **Matter / Effect / Delivery / Mod**, plus operators over them |
| A different element REPLACES (`water lava` throws lava) | Both are free Matter → both are cast (`water lava` sprays both). "Made of two things" now has the literal meaning: two sprays |
| Modifiers are an order-free bag | Free items in a clause are still a bag. Only **operators** care about neighbours |
| Repeats DOUBLE (2^(N−1)) and cost doubles | Matter and Effects **add** (×N: fire×2 is twice the fire, explosive×2 twice the power). Mods **compose** (applied again: shotgun 3/9/27, swift ×2/×4, float −1g/−2g). Cost comes from the *tariff on the expansion*, so shotgun³ pays everything behind it 27× |
| `transmute` converts *anything* to the element on the stack | `transmute` is an infix operator `A transmute B`: convert A into B. Both words are required; `anything` and `air` are real words (§3) |
| One form per spell, last wins | Each Delivery **closes a clause**; a spell is a list of clauses cast together |
| Spray is the default form | Unchanged: no Delivery ⇒ `hand` |

The existing `firebolt`/`acid_lance` conjoined entries get re-spelled in the new
grammar (`acid transmute projectile` becomes `anything transmute acid
projectile`). They
were always content.

---

## 1. The five sorts

| Sort | What it denotes | Examples | Lowers to |
|---|---|---|---|
| **Matter** (M) | a material, by name | `fire water dirt gold blood wood steel` | nothing on its own; an argument |
| **Effect** (E) | *something that happens at a point* — a function of `(at, dir)` | `explosive`, `gust`, `spark`, `mend`, `(dirt transmute water)`, `(fire trail)`… | a list of ops via `ApplySpellEffect` |
| **Delivery** (D) | *how an Effect reaches a point* — a function Effect → Cast | `hand` (implicit), `projectile`, `bolt`, `lob`, `orb`, `bomb`, `beam`, `self` | the delivery record a live cast carries |
| **Mod** (Δ) | a change to a Delivery's record | `shotgun`, `float` (anti-gravity), `swift`, `slow`, `bounce`, `pierce`, `seek`, `long`, `fuse` | field edits on the delivery record |
| **Operator** | a word with *argument slots*, producing one of the above. Every unary operator takes the word BEFORE it; only `transmute` is infix | `transmute` (M ⋈ M → E), `trail` (E ◂ → Δ), `aura` (E/Δ ◂ → E, sustained), `null` (word ◂ → E), `echo` (E ◂ → E), `mend` (M ◂ → E) | whichever sort it declares |

Two things worth saying out loud:

- **Effects are the value type.** A Delivery only decides *where and when*
  `ApplySpellEffect` runs. That is exactly commitment 2, generalised: the
  payload of a projectile, of a bomb, of `self`, of a sustained aura tick and of a
  backfire is the same `vector<EffectInst>` run at a different `at`.
- **Matter alone is not an effect.** A free Matter in a clause is coerced to
  `spray(M)` (the current rule), which is what keeps the one-word spell alive.
  Everywhere else Matter is an argument: `dirt transmute water`, `fire trail`,
  `blood mend`.

Sorts are declared per glyph in `glyphs.json` and shown in the glyph's info box
(§9). Nothing in C++ knows which words exist.

---

## 2. The six parse rules

A spoken sequence is parsed left to right into **clauses**, each clause into a
**bag** of free items plus zero or one head. These rules are the whole grammar.

**R1 — Runs merge.** N consecutive utterances of the same glyph are ONE item
with multiplicity ×N. `shotgun shotgun shotgun` is `shotgun×3`; `fire fire`
is `fire×2`. Matter and Effects add, Mods compose (§4); capped
(`kMaxMultiplicity`, rule 2).

**R2 — Operators bind their neighbours, greedily, on their declared side.**
An operator with a left slot of sort S takes the nearest item to its left *if
it is of sort S and unbound*; same on the right. Binding happens in spoken
order, left to right, so when two operators want the same atom the earlier one
wins and the later one is incomplete (R3). A bound group is one item of
the operator's result sort. The HUD draws the group inside the bracket glyph —
the bracket is *derived from the binding*, so what you see is what bound.

**R3 — An operator with an empty required slot is INCOMPLETE.** It is charged
its word cost and does nothing; the HUD shows `_` in the empty slot. There are
no defaults: `transmute` needs both words, and the wildcard and the void are
words of their own — `anything` (matches whatever is actually there; priced
as that matter **plus 25%**, billed when it resolves, so you do not know the
price until it lands) and `air` (nothing; as a target it unmakes, as a source
it conjures). So "unmake whatever is there" is spelled `anything transmute
air`, and it costs what the thing turns out to be worth.

**R4 — A Delivery closes the clause.** Everything unbound to its left is the
clause's bag: Effects go into the **payload**, Mods onto the **delivery
record**, free Matter becomes `spray(M)` in the payload. The next word starts a
new clause. `explosive projectile fire bomb` is two casts:
`[explosive] projectile | [spray fire] bomb`.

**R5 — A clause without a Delivery is delivered by `hand`.** `hand` resolves
the payload at *reach* (a few voxels in front of the caster, along the aim).
That is where a bare `explosive` goes off, and yes, it hurts — the ancient
language grants the literal request. (`self` exists for "on me"; §7.)

**R6 — Order inside a bag does not matter.** `explosive shotgun projectile` and
`shotgun explosive projectile` are the same cast. Only R2 (operators) and R4
(clause boundaries) are order-sensitive. This is the rule that makes the
permutation space small: a clause is a *set* of effects and a *set* of mods
around one delivery.

That is it. Everything the brief asks for is a consequence:

- `(dirt transmute water) projectile` — R2 binds `dirt`, `water`; R4 puts the
  Effect in the payload; the bolt converts dirt to water where it lands.
- `(fire trail) explosive (shotgun×3 projectile)` — R2 binds `fire` to
  `trail` (a Mod); R1 merges the shotguns; R4: payload `[explosive]`, mods
  `[trail(spray fire), shotgun×3]`, delivery `projectile`. Twenty-seven pellets,
  each with a fire trail, each exploding. Cost is 27 times a fire-trail
  explosive bolt (§4), which is the point.
- `explosive×2 bomb` — R1, R4. One bomb, explosion at ×2 power.
- `fire self` — R4 with `self`: `spray fire` at the caster. From the character
  screen, at the chosen body part (§7).
- `blood mend` — R2 binds `blood` as `mend`'s source filter; R5 delivers by
  hand: channel from whatever blood is within reach into the caster's missing
  cells (§7).
- `float×2 aura self` — R1 merges the floats; R2: `aura` takes the word
  before it and produces an Effect that SUSTAINS it on the body it resolves
  on; R4: `self`. Per-tick anti-gravity at ×2 on the caster, which crosses
  zero and lifts them, billed per tick until dropped (§7). Change the last
  word and it is someone else's problem: `float×2 aura projectile` sustains
  it on whoever the bolt hits. `float×2 self` without `aura` is a one-off hop,
  and `float×2 projectile` makes the bolt itself climb: a Mod acts on the
  delivery unless `aura` moves it onto a body.

---

## 3. Totalization: every mismatch has one fixed answer

| Situation | Answer | Why this one |
|---|---|---|
| Free Matter in a bag | `spray(M)` in the payload | the current one-word-spell rule |
| No Delivery in the last clause | `hand` | the current Spray default |
| Mod but no Delivery | Mod applies to `hand` (`shotgun fire` = a wider, ×3 spray) | Mods are field edits; `hand` has the fields |
| Operator slot empty | incomplete: charged, does nothing, HUD shows `_` | no defaults; `anything` and `air` are the explicit words |
| `anything` anywhere | matches the matter actually there; priced as it **+25%**, billed on resolve | the dangerous word: you learn the price when it lands |
| Mod on a body-anchored delivery (`hand`/`self`/`aura`/`beam`) | acts on the caster's body where that means anything (`float`, `heavy`, `shotgun` = fanned resolve points); otherwise a charged no-op (`bounce self`) | Mods edit a record; the anchored record is the body |
| Operator sees the wrong sort beside it (`explosive transmute water`) | slot stays empty → default; `explosive` stays free | operators bind only their declared sort; nothing is stolen |
| Delivery with an empty payload (`projectile`) | the delivery's *kinetic* effect only (small impact, today's `impactRadius`) | a thrown nothing still hits; `self` and `aura` with no payload do nothing and charge the word |
| Delivery repeated (`projectile×2`) | ×N **weight**: speed, lifetime and kinetic impact | count is `shotgun`'s job; a heavier bolt is the other axis |
| Effect repeated (`explosive×2`) | ×N of the Effect's declared *scale axis* (power for explode, volume for convert, voxels for spray) | every Effect names ONE axis its multiplicity scales; the tariff prices the result |
| Mod repeated (`shotgun×3`) | the Mod applied N times: count 3^N, speed 2^N, gravity −N·g | compose, not add: a Mod is a function on the record |
| Multiplicity over the cap | extra utterances are free and do nothing (today's rule) | honest; rule 2 |
| Same Effect twice, not adjacent (`explosive fire explosive`) | merged as ×2 (R1 across the bag, since R6 says order is irrelevant) | otherwise R6 would be false |
| Silence | not a spell | unchanged |

There is no "misfire" state. Every sequence lowers to a definite cast list, and
`DescribeSpell` prints it. The imprecision penalty is entirely in §4 (casting
into health), where it already lives.

---

## 4. Cost: you pay for voxels, not for words

This is the load-bearing change and the one the brief's economics need.

**Every glyph has a small fixed `word` cost. The real price is the tariff on
the ops the cast emits.** The lowering pass (§6) knows, before anything is
cast, exactly which ops an instant delivery will emit, so the price of an
instant spell is computable up front (the HUD needs that; the mana/health
crossover mechanic needs that). A channelled or persistent spell pays the same
tariff **as it emits** — that is the brief's "mana drain over time, part of the
system" and it needs no second mechanism: when the pool empties the cast
resolves Unstable, then Fatal, exactly as today.

The tariff has three parts, all content:

1. **Material value.** One integer per material in `materials.json`:
   `arcane` (0 for air, small for dirt/sand/water, large for gold, absurd for
   the gem materials). Not a from×to table — one scalar per material keeps
   hundreds of materials O(N) and lets a modder price a new one with one key.
   - `spray(M)` / `place(M)`: voxels × `arcane(M)` × `rate.place`.
   - `convert(A→B)`: voxels × (`rate.convert` + max(0, `arcane(B) − arcane(A)`)).
     Going *down* in value (gold→dirt) costs the base only; going *up* costs
     the gap. Water→gold is the gap between the cheapest and dearest things
     there are, per voxel, over a pool of thousands of voxels. Nobody affords
     that, which is the story the brief wants told.
   - `mend(from M)`: voxels × `arcane(M)` × `rate.graft`, plus a **foreign
     penalty** when M is not the anatomy's own material for that cell
     (blood/flesh/bone: none; wood: some; steel: a lot).
2. **Volume.** `explode`: `power × r³ × rate.explode`. Repeating `explosive`
   scales power ×N; the engine's radius/power relation gives the radius, and the
   price follows the *volume touched*, so ×2 power is more than ×2 mana. This is
   the only place the cost curve is superlinear per word and it is because the
   *world* effect is.
3. **Delivery premium.** Each Delivery declares `carry`, a per-mille multiplier
   on its payload's tariff, plus a flat flight cost per instance:
   `bomb` 1000 (you drop it, it sits, the world does the work), `self` 1000,
   `hand` 1000, `projectile` ~3000, `bolt` (fast) ~4000, `beam` per tick.
   That is the brief's "explosive projectile costs much more than explosive
   bomb" as one number per delivery instead of a table. `shotgun×3` multiplies
   *instances*, so the premium and the payload tariff both go ×9 — the fire
   trail explosive shotgun³ is nine bolts' worth of everything and reads as a
   lethal number on the HUD before you commit.

**Repetition: Matter and Effects add, Mods compose.** Saying `fire` again
throws one more fire's worth; saying `explosive` again adds one more
explosion's power. Saying a Mod again *applies it again*: `shotgun×3` is
3·3·3 = 27 instances and everything behind it is paid 27×; `swift×2` is ×4
speed; `float×2` is gravity reversed. No separate doubling curve is needed —
the tariff on the expansion is what makes repetition expensive, and it is
expensive exactly where the world effect is bigger.

**`anything` is priced when it lands.** Every other cast has a price the HUD
can show before you commit. `anything` matches the matter actually there, so
its tariff is that matter's value plus a 25% surcharge, and it is billed on
resolve. An `anything transmute gold projectile` fired into a gold vein is
cheap; the same bolt fired into a lake bills the water→gold gap for the whole
resolve volume the instant it lands, walks the caster into Unstable and
Fatal, and the melt-mode share boils the impact site. That is the danger the
word is for.

**Imprecision degrades the product, not just the aim.** Today an Unstable cast
wobbles the trajectory. Add one rule: an Unstable *convert* emits a fraction of
its ops (proportional to `instability`) as **melt-mode** (BrushOp mode 2)
instead of overwrite (mode 1). Melt mode converts each cell to its own authored
heat product — `materials.json` already says water → steam, stone → lava, sand
→ molten glass. So the caster who tries `water transmute gold` on a tarn spends
the pool, runs into health, and the last ops boil the water instead of gilding
it; then the cast goes Fatal and the payload runs at the caster. That is the
brief's picture, and it is one `if` on an existing op mode, not a new system.
The same rule gives every other effect a thematic failure for free: an unstable
`spray(water)` sprays steam.

---

## 5. Primitives: what an Effect can lower to

Hundreds of glyphs, but the C++ knows only these verbs. Each maps to ONE op
type on the MutationQueue or to one existing engine seam. A glyph is
`{ sort, verb, args }` — new glyphs are JSON.

| Verb | Emits | Notes |
|---|---|---|
| `spray(M, n, speed)` | `ParticleSpawn` | today's Spray |
| `place(M, r)` | `BrushOp` mode 0 (into air) | e.g. `wall`, a `fire` trail mark |
| `convert(A, B, r)` | `BrushOp` mode 1 (overwrite) with a **from filter** in `pad0` (0 = any) | `transmute`; `A→air` is disintegrate; Unstable ⇒ some ops in mode 2 |
| `explode(power, r)` | `ExplosionOp` | `explosive` |
| `wind(prim)` | `WindPrim` | `gust`, today's block |
| `mend(M?, n)` | world: `convert(cell→air)` at the source; body: a **restore** request to the avatar/mob (new, §7) | the graft loop |
| `sustain(E or Δ, r)` | attaches E (or a Mod) to the body at the point — or to the place, if no body — as a status that runs every tick, billed per tick to the caster, until dropped or the caster runs dry. Bounded by a hard tick cap and a per-caster status cap | `aura`: `E aura self` is a ward, `E aura projectile` puts it on the target |
| `filter(what, r)` | an entry in the caster's **op filter** for this tick | wards: refuse ops of kind `what` within r of the caster. Op-stream filtering, not the CA (DESIGN §8: acid already flowing still flows) |
| `gravity(g)` | a per-tick impulse on the body a sustained Mod sits on, or on the projectile itself when used as a plain Mod | `float`; ×N crosses zero |

Deliveries are three *mechanisms*, parameterised:

| Mechanism | Deliveries | Record fields |
|---|---|---|
| **instant** at a point | `hand` (reach in front), `self` (caster / chosen part) | reach, radius clamp |
| **flight** | `projectile`, `bolt`, `lob`, `bomb` | speed, gravity, drag, lifetime, bounce, `fuse` (0 = resolve on impact; >0 = resolve when the fuse runs out, sitting where it landed as a rigid body), count, spread, seek, pierce, trail |
| **continuous** | `beam` (held: resolve at the ray hit every tick) | per-tick tariff |

There is no `aura` delivery. Sustained effects are the `aura` OPERATOR (§7)
riding any delivery, so "on me" and "on them" are the same word with a
different last word.

`bomb` is `flight` with low speed, gravity on, `fuse > 0` — it is not a new
code path. A rigid-body bomb that carries `(fire trail)` lays fire while it
rolls because the trail mod runs on the flight record regardless of speed.
Mods are literally field multipliers on this record: `shotgun` ×count,
`float` −gravity, `swift` ×speed, `long` ×lifetime, `fuse` ×fuse. Adding a Mod
is one JSON entry naming a field.

---

## 6. The compiler

```
spoken glyph ids
  │  Parse: R1 merge runs → R2/R3 bind operators → R4 split clauses
  ▼
SpellTree   (what the HUD draws: brackets are the binding groups)
  │  Lower: per clause → Cast { DeliveryRec, payload: vector<EffectInst>,
  │                             instances (count), estimatedOps }
  ▼
CastList    (what Cast() runs; what a projectile carries; what backfire runs)
  │  Price: Σ tariff(estimatedOps) × carry + Σ word costs  → HUD, ResolveCast
  ▼
ApplySpellEffect(payload, at, dir, strength) → SpellEmission     (unchanged seam)
```

`Spell` today folds element/form/modifiers into one struct. It becomes
`CastList`. `SpellProjectile` carries one `Cast` (its payload + mods + trail
effect list) instead of a `Spell`. `ApplySpellEffect` iterates the payload's
`EffectInst`s and switches on the *verb* — the only switch in the system.

Nothing about the four commitments moves. Mobs cast a `CastList` through the
same `Cast()`.

---

## 7. The awkward cases the brief names, in the grammar

**Cast on self, on a body part.** `self` is instant delivery at the caster.
From the character screen the player clicks a part; the screen calls the same
`Cast()` with `at` = that part's position and the delivery's radius clamped to
the part. Nothing in the VM knows what a part is (commitment 4); the screen
passes a position. **Cauterise** is then `fire self` on a bleeding stump:
`spray(fire)` at the stump chars the exposed flesh, and a charred cell is no
longer a bleed source — that is a body rule, not a spell rule, and it is the
one engine change this scenario needs (check `PartBleeding` /
`bleedBudget` against burnt cells; today a burnt limb still counts its
`bleedBudget`). Once that holds, cauterising is emergent: any fire, from any
delivery, on any bleeding part, stops it and costs the burn.

**Mend (heal / restore / "cyborg").** `mend` is an Effect-operator with a left
Matter slot (source filter, default *anything*). Delivered by `hand` it is a
channel: each tick, take up to `n` voxels of matching material from within
reach (a `convert(M→air)` op in the world) and post a *restore* request to the
caster's body: fill the next missing cell of the anatomy recipe with M. The
anatomy `.vox` is already the recipe of what should be there (skin / flesh /
muscle / bone by depth), so "missing" is well-defined and so is "which cell
next" (nearest-to-root first, so a stump regrows outward). The restored cell IS
material M: wood burns, steel does not, acid eats flesh and not glass. The
tariff (§4) makes blood/flesh/bone cheap and steel dear, and the per-tick
billing means an over-long channel walks the caster into Unstable (restores
land as M's heat product — a mend from wood under strain grafts charcoal) and
then Fatal. The starter spell is the conjoined `blood mend` ("heal"): it can
only draw on blood. `mend self` is literal too: it draws from the caster's own
body to fill the caster's own body, i.e. it moves a leg into a face. Name
candidates: `mend`, `knit`, `graft`. `mend` reads best next to Matter
(`wood mend`).

**Wards / auras: one operator.** `aura` takes the word before it (an Effect
or a Mod) and produces an Effect that SUSTAINS it on the body it resolves on —
or on the place, if no body is there. The status runs every tick and is
billed every tick to the caster as a *reservation*: the HUD shows effective
max mana reduced by the per-tick tariff × the regen horizon, which is the
brief's "lowers your maximum mana" without a second number. It ends when the
caster drops it, dies, or runs dry (rule 2: also a hard tick cap and a
per-caster cap on live statuses). The delivery is whatever comes after:

- `float aura self` — floaty. `float float aura self` — lifted away.
- `float float aura projectile` — the same status on whoever the bolt hits.
  They float away as long as you keep paying; drop it and they fall.
- `transmute null aura self` — `null` takes the word before it and yields a
  filter Effect; sustained on you, it refuses incoming transmute ops within
  radius, so nobody turns the ground under you to acid. `projectile null aura
  self` absorbs bolts. Filtering is on the op stream (DESIGN §8): acid that is
  *already* flowing still reaches you. Counterplay is physics, on purpose.
  `transmute null aura projectile` plants an anti-transmute zone where it
  lands (no body there ⇒ the place).
- `fire aura self` sprays fire from your body every tick. `fire aura
  projectile` sets a body alight and keeps it alight while you pay.
- `fire trail aura self` — a sustained Mod acts on the body as if the body
  were the delivery: you lay fire wherever you walk.
- `explosive aura aura projectile` — repeating `aura` widens the attach
  radius: everyone near the impact gets the status.
- `aura` with nothing before it is incomplete: charged, nothing.

"Ward" as a word for the *player* is the name of the conjoined `… aura self`
spells. The grammar does not need it.

**Trail needs a prefix.** `trail` is an operator: `E ◂ trail → Δ`. It takes an
Effect (or a Matter, coerced to `spray`) on its left and produces a Mod that
runs that Effect at each marked voxel of the flight path. `explosive trail` is
therefore legal and means a bolt that explodes along its whole path; the tariff
prices it as budget-voxels × explode, so it is priced as the ruin it is. A
bare `trail` is incomplete: charged, lays nothing.

---

## 8. Do the permutations behave? The generated tables, and the laws

The grammar is executable: `scripts/magic_grammar.py` is a reference
interpreter of R1–R6 over the full glyph table, and
`docs/MAGIC_PERMUTATIONS.md` is GENERATED from it — every single word, every
sentence in the brief, every ordered pair over a 19-word alphabet and every
ordered triple over a 10-word core, each with its parse, what it does, and its
cost shape. A row that reads wrong is a rule that is wrong; fix the rule and
regenerate. `python scripts/magic_grammar.py fire trail explosive shotgun
projectile` prints one sentence.

The tables are where the free rides show up. With no defaults, the accidental
spells come from `anything` and `air` instead: `fire transmute air` is
extinguish, `anything transmute fire` is ignite-by-touch, `anything transmute
air projectile` is a disintegrator priced by what it hits, `air transmute
stone projectile` builds. Nothing authored any of them.

**The laws, which are the gate.** Rather than pin sequences, the `spells` gate
should assert *algebraic* properties over every sequence of length ≤ 3 drawn
from a fixed test alphabet (the five above plus `trail`, `bomb`, `self`),
generated in the test, not listed:

- **L1 totality** — every sequence lowers to a non-empty `CastList` with
  a finite cost and a non-empty description.
- **L2 bag commutativity** — permuting the free items of a clause (not moving
  words across an operator's slot or a clause boundary) lowers to an identical
  `CastList`.
- **L3 multiplicity** — `g g` ≡ `g×2`; cost is monotone non-decreasing in N;
  the scaled axis is exactly ×N until the cap.
- **L4 clause independence** — `cost(A ‖ B) = cost(A) + cost(B)` and the
  emissions are the union.
- **L5 delivery invariance** — the payload of `E… projectile`, `E… bomb`,
  `E… self` is identical; only the delivery record differs.
- **L6 local binding** — an operator's bound arguments do not change when a
  word is inserted or removed outside its immediate neighbourhood.
- **L7 tariff monotonicity** — for `A transmute B`, cost is non-decreasing in
  `arcane(B) − arcane(A)` and in the volume; for any spray, in the voxel
  count.
- **L8 budgets** — every lowered cast declares finite `ticks`, `voxels`,
  `instances`, `generation`; nothing lowers to an unbounded process (rule 2).

A change that breaks a law breaks a *class* of spells, which is what the test
should say; a change that moves one spell's numbers is a rebaseline.

---

## 9. The glyph info box

Every field is read from the glyph's JSON entry, so the box is never wrong
about the glyph and a modder's glyph gets a box for free.

```
┌ TRANSMUTE ─────────────────────────────────────┐
│ operator · matter ◂ ⋈ ▸ matter → effect         │
│ "Turns the first into the second."              │
│ left empty: anything   right empty: air         │
│ word 4 · tariff: volume × (convert + value gap) │
│ again: ×N volume                                │
│ delivers by: hand, projectile, bolt, lob, bomb, │
│              beam, self                         │
│ example: dirt transmute water projectile        │
└─────────────────────────────────────────────────┘
```

Fields: `sort`, `valence` (which sides, which sorts, defaults), `word` cost,
`tariff` formula name, `axis` (what repeating scales), `verb`, `desc`,
`example`. The HUD's live readout shows the same bracket structure over the
spoken sequence plus the running price broken into word / tariff / carry, so
"why is this 900 mana" is visible before casting.

---

## 10. Implementation, in order

Each step keeps `--gate spells` green or extends it, and none needs a
rebuild for content.

- **P0 — schema and parser.** `sort`, `valence`, `verb`, `axis`, `default`
  per glyph in `glyphs.json`; `ParseSpell` (R1–R4) producing `SpellTree`;
  `DescribeSpell` prints brackets. Re-spell the three conjoined entries. Lower
  the existing verbs (`spray`, `convert`, `explode`, `wind`, trail) onto the
  tree. Laws L1, L2, L3, L6 in the gate. Repetition goes linear.
- **P1 — tariff.** `arcane` per material in `materials.json` (one column,
  default by density or authored), `rate.*` and `carry` in `glyphs.json`
  budgets; `PriceCast`; HUD split. Unstable-convert → melt-mode share. L4, L7.
- **P2 — deliveries as records.** `flight` record with fuse/gravity/count;
  `bomb`, `bolt`, `lob` as content; `shotgun`, `float`, `swift`, `long`,
  `fuse` as field-mods; `self` with an `at` override for the character screen.
  L5.
- **P3 — sustained.** `beam` with per-tick billing; the `aura` operator's
  status list per body (caster-billed, reservation display, drop UI, tick
  cap, per-caster cap); `null` + op-stream filter. L8.
- **P4 — mend.** Anatomy-recipe restore request on avatar/mob; charred cells
  stop bleeding (the cauterise rule); `blood mend` grimoire starter.
- **P5 — the tongue and the grimoire (§12).** Two banks on the number row,
  the arsenal sorted by sort with the info box, macros as saved word lists
  that expand inline, `=` to capture the live stack, `PLYR` v3, `KitSpace::
  Grimoire`, gate `grimoire`.

---

## 11. Open decisions (choose; the plan works either way)

1. ~~`shotgun` repeated~~ DECIDED 2026-09-04: Mods compose (3/9/27).
2. ~~Doubling → linear~~ DECIDED: Matter and Effects add (×N); Mods compose.
   The current gate's exact-doubling assertion becomes exact-×N for spray.
2b. ~~Operator defaults~~ DECIDED: none. `anything` and `air` are glyphs;
   `anything` is priced as the actual matter +25%, billed on resolve.
3. **`explosive×N` radius.** The brief says √2 per doubling of power; the
   engine's explosion already relates `power` and `radius` (`ExplosionOp`), so
   the plan scales `power` and lets the existing law set the radius. If the
   look is wrong that is a tuning row, not a grammar change.
4. **Second Delivery: new clause (assumed) or replace (today).** New clause is
   the only total reading that never discards a spoken word.
5. **Names.** `aura` for the sustain operator (DECIDED 2026-09-04: grip and
   aura are one word; aura-on-self is just aura + self), `null` for the
   negation operator, `mend` for the graft, `echo` for the repeater. "Ward"
   stays as the player-facing name for `… aura self` conjoined spells.
5b. **Operator direction.** Every unary operator takes the word BEFORE it
   (`fire trail`, `blood mend`, `float aura`, `transmute null`, `explosive
   echo`); only `transmute` is infix. One rule to learn. Valence is per-glyph
   data, so any single word can be flipped later without touching the parser.
5c. **`anything` surcharge.** Cost of `anything ⋈ B` = convert(actual→B) PLUS
   value(actual) × `anythingSurchargeMille` (+50% to start), billed on
   resolve. The surcharge is on the wildcard, on top of the conversion.
6. **Spray vs place for free Matter on a projectile impact.** Plan keeps spray
   everywhere (uniform, one rule); a `wall`/`place` glyph exists for the
   fill-a-radius behaviour when someone wants it explicitly.

---

## 12. Choosing words, and the grimoire (macros)

Today (`game/caster.h`): the player owns every glyph, the first ten are bound
to the number row in file order, `Z` enters magic mode, a number SPEAKS,
right-click casts, Backspace clears. `ConjoinedGlyph` is a loader-only shape
(a name and a list of glyph indices) that nothing expands. The character
screen's arsenal shows owned glyphs and the ten bound slots, crossing by name
(DESIGN.md §8b). With 44 glyphs and a grammar, ten live words is the right
constraint and the wrong ergonomics. Two changes.

### 12a. The tongue: what you can say right now

- **Two banks on the number row.** `1`–`0` speak bank A; `Shift+1`–`0` speak
  bank B. Twenty live words, one hand, no menu. `kGlyphSlots` becomes 20 with
  the bank as `slot / 10`; the HUD strip draws two rows and highlights the
  bank Shift is holding. Sprint is on Shift outside magic mode already, and
  magic mode captures the number row, so nothing collides.
- **The arsenal is sorted by sort.** Five columns — matter, effect, operator,
  delivery, mod — each glyph drawn with its sort's colour and its valence
  marks (`◂`, `⋈`), so a player looking for "the thing that goes after fire"
  looks in one column. Hover opens the §9 info box. The same colours are used
  in the live readout, so the sentence you are speaking and the panel you
  bound it from agree visually.
- **A slot holds a glyph OR a macro.** Same drag, same strip, same
  `bindGlyph` intent; the mirror carries a `kind` so the strip can draw a
  macro with its bracket readout under the name. `KitSpace::Grimoire` is the
  fourth `KitRef` space.
- **Ownership is real.** `GrantAllAndBind` becomes the debug default only;
  the acquisition loop (loot, tutors) is out of scope here but `Grant` and
  `Owns` are already the seam, and the arsenal draws unowned glyphs greyed
  with their name hidden, so the shape of a glyph you have not learned is
  visible and its word is not.

### 12b. The grimoire: macros

A **macro** is a saved list of glyph names with a name of its own. Speaking a
macro pushes its words onto the stack exactly as if you had spoken them, and
the six rules apply to the result. That single sentence is the whole
mechanic; everything below is consequence.

- **Macros are fragments, not spells.** A macro need not be complete or even
  well-formed. `(fire trail) explosive shotgun shotgun` saved as `hellfire`
  has no delivery, so `hellfire projectile` and `hellfire bomb` are both
  live sentences, and `hellfire hellfire` is 81 pellets by R1 (the two
  expansions merge, shotgun×4). A macro that ends in an operator waiting for
  a word (`anything transmute`) is a live prefix: `[it] gold projectile`.
- **Macros nest.** A macro may name another macro. Expansion is recursive
  with a depth cap of 4 and a cycle check at save time (a macro that would
  contain itself is refused with the reason shown). Expanded length is capped
  by the existing 16-word stack bound; a macro whose expansion would overflow
  the stack speaks as much as fits and the HUD says so. Rule 2: no unbounded
  expansion, ever.
- **Macros are by name, through and through.** A macro stores glyph NAMES
  and macro NAMES. A name that no longer resolves (content removed, glyph
  renamed) drops that word with a log line and the readout shows a `?` in
  its place; the macro is not deleted. This is the DESIGN §8b contract.
- **Two ways to make one.**
  1. **Capture.** In magic mode, with a sentence on the stack, press `=`.
     The stack is saved to the first empty grimoire page under an auto-name
     derived from its readout (`fire-trail-explosive-shotgun2`), and a HUD
     line confirms it. Renaming is on the screen. This is the fast path: you
     just spoke something that worked and you want it as one key.
  2. **Compose.** The character screen's arsenal gets a **grimoire** region:
     a page list on the left, and for the selected page a word row you can
     drag glyphs and macros into and reorder, a name field, the derived
     bracket readout, the price (lowered as if cast alone, `?` if it depends
     on `anything`), and Save / Delete / Bind-to-slot. The row is the same
     `SpellStack` type the live sentence uses and is described by the same
     `DescribeSpell`, so the panel can never disagree with the game about
     what a page means.
- **Editing a page rewires every slot bound to it**, because slots hold the
  page's name. The strip redraws its readout next frame.
- **Authored starters live in `glyphs.json`'s `conjoined` block** with the
  same shape (`id`, `glyphs` by name, `desc`). They are granted like glyphs
  and appear in the grimoire as read-only pages; "duplicate to my grimoire"
  makes an editable copy. `heal` = `blood mend`, `firebolt` = `fire trail
  projectile`, `ward` = `transmute null aura self`. So the brief's "in the
  beginning it is two runes joined" is the starter set, and the "later
  you'll get the individual runes" is `Grant`.
- **Limits, as data in `glyphs.json` budgets:** `maxGrimoirePages` (32),
  `maxMacroWords` (16), `maxMacroDepth` (4).

### 12c. Persistence and verification

- `PLYR` v3 appends the grimoire: page count, then per page a name and a
  list of words (each a length-prefixed name), then the 20 bound slots as
  `(kind, name)` pairs. v2 files load with an empty grimoire and the ten
  bound slots in bank A. A truncated payload is refused, as today.
- Gate `grimoire` (CPU-only, own fixtures, beside `player-kit`): expansion
  equals speaking the words (compile both, compare the lowered `CastList`);
  nesting expands to depth and refuses a cycle with a reason; the overflow
  cap truncates and reports; a missing name drops one word and keeps the
  page; a `PLYR` v3 round trip compares by name; a v2 payload loads with the
  bank-A slots intact; binding a macro to a slot and speaking that slot
  produces the same `CastList` as the expansion.
- `--shot-inventory` gains a third frame, `screenshot_inventory_grimoire.bmp`,
  with a page selected and a word row populated, so the panel's job — being
  looked at — is judged by a picture like the other two.

---

## 13. Brief for the implementing agent

Copy this verbatim into a fresh session in the main checkout.

```
Implement docs/PLAN_magic_grammar.md in full: the magic grammar (§1–§9),
phases P0–P5 (§10), and the tongue + grimoire (§12). Read, in this order,
before writing code: CLAUDE.md (all of it — build.sh/run.sh, the board, the
verification budget, the rebaseline rule), DESIGN.md §8 "The spell system"
and §8b (the character screen), docs/PLAN_magic_grammar.md,
docs/MAGIC_PERMUTATIONS.md (skim §1 and §3; it is generated), and
scripts/magic_grammar.py — that script is the executable reference for the
parser and the lowering, and the C++ must agree with it.

Ground rules that are not negotiable:
- The four structural commitments in src/game/spell.h stay: op-stream-only
  output, position-parameterized ApplySpellEffect, integer 24.8 VM, not
  player-coupled. Mobs cast through the same Cast().
- Every sustained thing (aura, beam, trail, echo, split) declares a finite
  budget; rule 2. The generation counter caps split.
- Glyphs are content: sort, valence, verb, axis, costs live in
  assets/spells/glyphs.json; per-material `arcane` value lives in
  assets/materials/materials.json. No material ids, no glyph names in C++.
- Everything crosses UI and save by NAME (DESIGN §8b). KitRef gets
  KitSpace::Grimoire; PLYR goes to v3 and still loads v2.
- Claim files on the board before editing (bash scripts/board.sh claim ...):
  main.cpp, DESIGN.md, assets/materials/, assets/spells/, assets/tuner*.
- Build with bash scripts/build.sh, run only via bash scripts/run.sh. Never
  raw cmake, never a bare sandvox.exe.

Deliver in six commits, one per phase, each leaving the tree green:
  P0 parser + lowering + DescribeSpell brackets + laws L1/L2/L3/L6 in the
     `spells` gate, driven by a generated alphabet, not a pinned list. Add a
     `spells-oracle` check: assets/spells/grammar_oracle.json is written by
     `python scripts/magic_grammar.py --oracle` (add that flag: every pair
     over the 19-word alphabet as canonical parse strings) and the gate
     compares the C++ parse of each sequence to it.
  P1 tariff + PriceCast + HUD price split + Unstable-convert melt share +
     L4/L7. Add the `anything` surcharge knob to glyphs.json budgets.
  P2 deliveries as flight/instant/continuous records; bolt, lob, orb, bomb
     (rigid body with fuse through the existing debris path); the twelve
     mods as field edits; self with an `at` override from the character
     screen's clicked part; L5.
  P3 beam; the aura operator's per-body status list (caster-billed per tick,
     reservation shown on the mana bar, drop UI, tick cap, per-caster cap);
     null + the op-stream filter at the MutationQueue splice; echo; L8.
  P4 mend: anatomy-recipe restore on avatar and mob (nearest-to-root first),
     source voxels removed through convert(cell→air) ops; the cauterise rule
     (a charred cell is not a bleed source); starter pages heal/firebolt/
     ward in glyphs.json `conjoined`.
  P5 two banks on the number row, arsenal sorted by sort with the §9 info
     box, grimoire pages (capture with `=`, compose on the screen, nesting
     depth 4, cycle refusal, 16-word overflow), PLYR v3, KitSpace::Grimoire,
     gate `grimoire`, third --shot-inventory frame. Pixel-art UI only.

Verification budget: --gate spells / --gate grimoire / --gate player-kit
while iterating; ONE --suite acceptance on the final tree; --selftest
--rebaseline once at the end if the world hash moved (glyph content and the
arcane column will move it; that is expected and not a finding). Update
DESIGN.md §8 in the P0 commit and the ARCH_NODES entry in assets/tuner.html
in the P5 commit. Report per phase: what landed, the gate line, anything in
the plan you had to change and why.
```
