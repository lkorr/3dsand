# Alchemy chemistry: reactions in the bench, gases, stoppers, solutes — orchestration plan

Status 2026-09-27: PLAN OF RECORD. Integration branch **`alchemy-chem`**
(worktree `C:/Users/Luke/Desktop/programming/sv-chem`). Orchestrated; each
package below is implemented by one worktree agent and merged by the
orchestrator. This document IS the agents' brief — read all of it, then your
package.

## 0. What the owner asked for (verbatim intent)

1. Reactions work INSIDE the alchemy bench, and the bench's falling-sand +
   liquid sim reacts the way the REAL WORLD (the 3D CA) does — same rules.
2. Acid dissolves most things. When acid dissolves something it gives off
   **noxious gas**: smoke-like, dark green.
3. **Smokes and vapours in the bench sim.** Gas rising out of a vial's mouth
   ENTERS THE REAL WORLD (at the hand holding it) and leaves the vial.
4. **Stoppers**: a bench tool to put a stopper in / take it out of a vial, so
   gas is trapped inside.
5. **Sodium**: reacts with water and EXPLODES. Explosion as a reaction effect
   is new: an explosion plus fire and smoke around it briefly. If it happens
   in the bench: both held vials explode, the player is EJECTED from the bench,
   and a REAL explosion (full damage — it can maim) goes off at the hands.
   Built as a general **reaction-effect / bench-event** system so future
   reactions can eject, flash, shock, etc. by data.
6. **Dissolving**: some powders dissolve in liquids. **Fairy dust** dissolves
   in water -> **enchanted water**, in blood -> **enchanted blood**. Enchanted
   blood laid on wounds HEALS them and RESTORES missing voxels.
7. **Salt**, dissolving into water as a SOLUTE (docs/PLAN_solutes.md, now
   being built for the world too) — a body of water carries the salt.
8. **Sodium from salt by electrolysis**: owner-approved real chemistry — a
   **burner** tool heats a vessel (salt melts to molten salt), an
   **Electrify** button shocks it: molten salt -> sodium + chlorine; brine ->
   chlorine + hydrogen + lye. **Chlorine**: yellow-green, toxic, damages you
   on contact, smoke-like.
9. Then: plenty more substances and reactions, real or magical/lore, and
   integrated systems (package E).

Owner decisions: world solute layer IS in scope (PLAN_solutes P1-P3); burner +
real chemistry; bench explosion is a real explosion at the hands; land on
`alchemy-chem`, orchestrator merges to main.

## 1. Ground rules for every agent

- Read `CLAUDE.md` (repo root) — especially the three rules, the verification
  budget, "What needs a rebuild", and the voxel word table. Read DESIGN.md
  "Vessels" and "Alchemy bench" (search the headings) before touching them.
- **Start your worktree from `alchemy-chem`**: first command in your worktree
  is `git reset --hard alchemy-chem` (your worktree branch was made from
  main; the contract below only exists on alchemy-chem). Confirm
  `docs/PLAN_alchemy_chemistry.md` and `src/sim/solutes.h` exist.
- Build with `bash scripts/build.sh` (sccache). Run the exe ONLY through
  `bash scripts/run.sh ...`. `export SANDVOX_NO_CRASH_DIALOG=1`.
- **Testing budget (owner directive):** never `--suite acceptance`, never a
  bare `--selftest`, never `--vk-smoke*` unless you changed a render path and
  need to know where the image moved. Verify with `--verify <your gates>` (or
  `--selftest --gate <g>`) — your new gates plus the 2-3 existing gates that
  cover code you touched. The world hash WILL move (new rules/materials): do
  NOT investigate or rebaseline it; the orchestrator rebaselines once at the
  end. A twice-run determinism FAILURE, however, is a real bug.
- Iterate FlaskSim changes in a standalone g++ lab (memory:
  `/c/msys64/ucrt64/bin/g++ -std=c++20 -O2 -Isrc`; flasksim.cpp has no engine
  deps) rather than rebuilding the game each time.
- Commit on your worktree branch in coherent commits with clear messages. Do
  NOT merge to main or to alchemy-chem; the orchestrator merges. End with a
  report: what landed, gates + their results, what is NOT done, and any
  contract you had to change (and why).
- Update DESIGN.md (the section you own) and `assets/tuner.html` ARCH_NODES
  per CLAUDE.md for what you land. Keep new materials' `//` comments honest.
- Do not edit another package's owned files except where this doc says so.
  If you must, keep it minimal and say so in the report.
- **The far-field palette is FULL** (128 slots, exactly used): every new
  material needs `"far": "<existing look-alike>"` or the asset load fails.
- 32 reaction TAGS max (materials.cpp TagRegistry); 21 are used. Do not add a
  tag where an exact material name will do.

## 2. The contract (already on alchemy-chem — do not change without saying so)

### 2.1 New materials (appended to materials.json; ids by position)
`salt` (powder), `molten_salt` (liquid, hot), `sodium` (powder, floats on
water), `chlorine` (gas, toxic), `noxious_gas` (gas, toxic), `hydrogen` (gas,
explosive), `lye` (liquid, caustic), `fairy_dust` (powder, magic),
`enchanted_water` (liquid), `enchanted_blood` (liquid), `spark` (gas,
tag:electric — a transient discharge). New tags: salt, metal, alkali, toxic,
gas_heavy, explosive, caustic, magic, electric. Package A may retune their
numbers/tags/colours; ONLY package E appends further materials (appending
from two branches collides on ids).

### 2.2 Reaction effects — `MaterialDef::ruleFx` (src/sim/materials.h)
Authoring, on any rule in reactions.json:
```json
{ "self": "sodium", "neighbor": "water", "chance": 400,
  "selfBecomes": "fire", "neighborBecomes": "lye",
  "effects": [ { "kind": "explode", "radius": 4, "power": 60 } ] }
```
`ReactionEffect {kind, radius, power, amount, what}`; `RuleFx {effects}`;
`MaterialDef::ruleFx[k]` belongs to rule `reactOffset + k` of that material
(including synthesized tail rules — A decides whether those inherit). Kinds
are resolved BY NAME by each consumer; unknown kinds are ignored by a
consumer. First kind: `explode` (radius voxels <= kMaxExplosionRadius, power
= ExplosionOp::power). Package A implements the PARSE (LoadReactionsJson) and
the world consumer; package C implements the bench consumer.

### 2.3 Solutes — `src/sim/solutes.{h,cpp}`, `assets/materials/solutes.json`
`LoadSolutes(path, mats, out, errors)`; `SoluteDef` (species 1-based, `from`
powder, `solvents`, `saturation`, `dissolveChance`, `diffusivity`, `floor`,
`densityPerUnit`, `tint`, `tintStrength`, `glow`, `precipitatesTo`,
`converts[] {solvent, into, cMin}`). Species today: `salt` (brine) and
`fairy` (converts water -> enchanted_water, blood -> enchanted_blood at
cMin 48). Concentration unit: 0..255 per full cell. B may extend the struct
(add fields, never repurpose); C reads it.

### 2.4 Dissolved portions in a vessel (game/composition.h — C owns, B reads)
A `Portion` whose `mat` has **`kDissolvedBit` (0x8000)** set is DISSOLVED
matter: `mat & 0x7FFF` is the powder material (salt, fairy_dust), `eighths` is
how much of that powder, in eighths of a voxel, is dissolved in the vessel's
liquid. It has no layer of its own: it rides the solvent portions (pouring
the solvent pours the dissolved matter in proportion). Conservation counts
salt as salt whether dissolved or not. Unit bridge to the world:
**one dissolved eighth = yieldPerVoxel / 8 world solute units.**

### 2.5 The pour/scoop seam between vessels and the world solute layer
Every path that puts vessel contents into the world (pour, apply, spill,
break, bench stream, thrown-flask splash) must go through ONE function in
container.cpp that turns a Composition into world ops (C establishes it if it
does not exist; it probably does — ContainerSpillStep / the pour path). Until
B lands, a dissolved portion leaving a vessel is emitted as its POWDER
(conserving). B replaces that fallback with liquid cells carrying solute mass,
and extends the scoop ledger so scooping brine yields water + a dissolved
salt portion. B owns the world half of this seam; C owns the vessel half.

**As built (package G, 2026-09-27):** the powder fallback is replaced. A
dissolved share becomes `ContainerSolutePour` entries (landing tick + cell,
container.h) queued on `TickAuthorityCtx::solutePours`; phase H sends the due
ones as `CellOpSolute` cell ops (world.h); sim_mutate.wgsl `solPour` lays the
mass into the solvent at the surface under the cell, or precipitates it as
powder where there is none. The MPM fluid pour's share travels beside the
particles, not in them (container.h ContainerPourFluid: why). The fallback to
grains remains for `sim.soluteMode` 0 and for a caller with no pour queue.
The bench evaluates `RuleFx::solute`/`soluteMin`/`soluteMax` itself
(benchchem.h ChemRule::soluteSpecies).

## 3. Packages

| P | Name | Wave | Owns (primary files) |
|---|---|---|---|
| A | World chemistry | 1 | reactions.json, materials.json (edits to the new rows + existing rows' tags/rules), materials.cpp (effects parse), sim_step.wgsl reaction path, the world explode-effect path (session.cpp / simulation.cpp as needed), body toxicity (mob.cpp inbound pass as needed) |
| B | World solute layer | 1 (long) | PLAN_solutes P1-P3: world.h, common.wgsl, new sim_solute*.wgsl, simulation.cpp, pass_table.def, page-pool-style aux layer, save/worldio, hash, sim_step.wgsl liquid-move hooks, render tint, the world half of 2.5 |
| C | Bench chemistry | 1 | flasksim.*, alchemy_bench.*, composition.h, container.* (vessel half of 2.5), iteminstance (stopper flag + formats), inventory_ui.cpp (bench panel tools), main.cpp bench wiring, session.* bench-event plumbing |
| D | Healing + body effects | 2 (after A merges) | mob.cpp/mob.h coat effects, materials.json coat blocks of the enchanted liquids |
| E | Creative expansion | 3 (after A, C merge) | new materials (append), reactions, solute species, bench events, content |

Overlaps are expected in sim_step.wgsl (A reaction path vs B liquid moves),
materials.cpp (A effects parse vs B solute rule conditions), session.cpp (A
world explode vs C bench events), DESIGN.md and tuner.html. Keep your edits
in those files localized and explained so the orchestrator can merge.

---

## Package A — World chemistry

Goal: every new material behaves right IN THE 3D WORLD, and reactions can
have EFFECTS, starting with a deterministic explosion.

1. **Effects parse**: fill `MaterialDef::ruleFx` in LoadReactionsJson (keep
   the parallel indexing exact, including tail rules from
   ExpandNeighborChance and weather-dropped rules). Validate: known kinds list
   (`kKnownEffectKinds`: explode, and whatever else you or C register —
   start with explode, flash, eject, shock), radius/power ranges.
2. **World explode effect**: a reaction whose rule has an `explode` effect
   causes a real explosion at that cell, through the NORMAL explosion path
   (ExplosionOp -> sim_explode.wgsl + ExplosionHitsBodies), with fire and
   smoke around it briefly (the reaction's own products + a ring of fire/smoke
   — your call how; must be bounded, rule 2). Constraints: rule 1 (no
   scheduling-dependent outcome — the GPU must choose which cells' explosions
   happen with an order-independent reduction, e.g. atomicMin keyed slots,
   never append order), rule 3 (explosions are ExplosionOps; if the GPU
   records a request that the CPU reads back, the latency must be a FIXED
   number of ticks or ride an existing tick-stamped readback like the scoop
   ledger — study `ContainerSettle`/`scoopLedger` and `world.Snap()`),
   kMaxExplosionsPerTick. Chain reactions must be bounded (a pile of sodium in
   a lake: a few blasts per tick, not a runaway). Also works for a body
   (mob.cpp's inbound pass evaluates grid rules onto body voxels) — at least
   do not crash/ignore silently; an explode effect on a body should blast at
   the body voxel.
3. **Reactions (reactions.json)**, all appended at the END of each material's
   rules where possible (rule index is part of the RNG stream; appending keeps
   old rules' streams):
   - sodium + water -> explode (+ lye, hydrogen, fire); sodium + other wet
     things (blood, enchanted_*) likewise weaker; sodium + tag:hot -> burns.
   - hydrogen + tag:hot -> small explode; hydrogen decays slowly (it escapes).
   - salt + tag:hot -> molten_salt (slow); molten_salt cools back to salt
     (decay, slow, when not near hot); molten_salt + tag:electric (spark) ->
     sodium, spark -> chlorine.
   - spark: decays within a couple of ticks; ignites flammables weakly.
   - chlorine, noxious_gas: rise/drift and fade like smoke (chlorine slower to
     fade; if the gas CA can make a heavier-than-air gas linger low, chlorine
     should — say in the report what the CA can do).
   - **Acid dissolves most things**: broaden acid to minerals, metals (iron
     slow, gold never — aqua regia is a later E recipe), glass stays immune (it
     is what vials are made of — keep that), bone stays as its existing rule
     says. EVERY acid-dissolves rule also yields `noxious_gas` (neighbor
     becomes noxious_gas, or an emit) — at a rate that reads as fuming, not a
     gas flood. Acid + sodium/alkali/lye -> neutralization (salt + water +
     heat/steam).
   - lye: caustic — eats organics slowly (weaker than acid), neutralizes
     acid.
   - fairy_dust + water -> enchanted_water, + blood -> enchanted_blood in the
     world as simple pair rules for now (B's solute layer will later make this
     concentration-driven — coordinate: leave the rule, B may replace it).
   - salt + water: once B lands, salt dissolves into the solute layer; for
     now no rule (salt sinks like sand). Leave a note.
   - chlorine + water -> (slow) weak acid? hydrogen + chlorine + spark/light
     -> hydrochloric acid + blast? Use judgement; keep it bounded.
4. **Toxic gases hurt bodies**: chlorine (strong) and noxious_gas (mild)
   touching a creature or the player damages it. Prefer the existing
   machinery: the mob inbound pass already applies grid rules to body voxels
   (e.g. chlorine + skin -> a chemical burn), plus whatever health/"blood"
   drain path exists for non-carving damage. The player must be hurt too.
   Say how in the report.
5. Gates: `chem-sodium` (sodium dropped into water explodes within N ticks:
   an ExplosionOp is issued, a crater exists, hydrogen/lye/fire appear; the
   run is deterministic twice), `chem-acid-fumes` (acid on stone/iron makes
   noxious gas and eats the target; glass untouched), `chem-electrolysis`
   (molten salt + spark -> sodium + chlorine), `chem-toxic` (a mob in
   chlorine takes damage). Keep each runnable with `--gate <name>` alone.

## Package B — World solute layer (the long one)

Build docs/PLAN_solutes.md **P1-P3** (read it all; it is the design of
record, including its "what will actually bite" list) using the contract's
`solutes.json` / `LoadSolutes` (extend SoluteDef if needed). In short:
sparse paged aux layer (`{species, mass}` per cell, SOL_EMPTY / SOL_UNIFORM
sentinels, derived pool size, fatal-abort verdict), advection through every
liquid move, pair-exchange diffusion with the d==0 fixpoint and the
**dilution floor**, per-chunk aggregates, concentration conditions on
reactions (side array, not a bigger ReactionGpu), density modifier (brine
sinks), render tint/glow, dissolve (powder `from` touching a solvent ->
mass) and precipitate (evaporation/boil-off -> `precipitatesTo`), and the
`converts` rows (fairy concentration >= cMin in water -> enchanted_water,
replacing A's simple pair rule). Solute mass is AUTHORITATIVE: hashed, saved
(new save version), carried across the MPM seam or refused there (P1 may
refuse — say which). Rules 1-3 all apply with full force.

Plus the world half of contract 2.5: pouring a vessel's dissolved portion
puts liquid cells carrying solute mass (through the MutationQueue — extend
the cell op / spill path as needed, within the <1 MB/tick budget); scooping a
solution credits water AND a dissolved portion (extend the scoop ledger).

Gates (PLAN_solutes section 6): `solute` (conservation above the floor, settles <=32
awake chunks, uniform within 2 units), `solute-dilute` (a creature-scale
amount into a big body is GONE after N ticks), `solute-evap`,
`solute-seam` (or the refusal, asserted), `solute-vessel` (pour brine from a
flask, scoop it back: salt conserved within the documented rounding), plus
`--sweep` reach for any new `sim.*` knob. Re-measure `--perf` before and
after (PLAN_solutes 7.5) with `SANDVOX_RUN_EXCLUSIVE=1`.

This package may not finish all of P1-P3 tonight. Land in order P1 -> P2 ->
P3 -> seam, each a working commit; stop at a clean commit rather than a
half-done step, and report exactly where you stopped.

## Package C — Bench chemistry

The bench (game/flasksim.*, game/alchemy_bench.*) becomes a chemistry set.
DESIGN.md "Alchemy bench" says reactions must run on `contents` in the game
tick, not on bench particles, because the bench is a UI device. The owner now
wants reactions VISIBLE in the bench sim. Resolution, which is the design you
build:

- **The bench runs the SAME rules as the world.** For each particle / grain,
  its material's compiled rules (`reactions` vector + `MaterialDef.gpu
  .reactOffset/reactCount`, exactly what mob.cpp's CPU evaluator reads) are
  tried against its neighbours in the bench (particles within the kernel
  radius, adjacent grains, glass). Pair rules convert the particle/grain and/or
  the neighbour; decay rules convert over time; emit rules emit a particle of
  the emitted material. Chance is per-mille per 30 Hz tick: scale for the
  bench's 60 Hz step. Rule order/first-match semantics as in the world. No
  hardcoded material ids or names in C++ — everything comes from the tables.
  Bound the work per step (rule 2's spirit: a flask full of reacting stuff
  must not stall the bench thread).
- **The bench has a GAS phase.** Gas-class materials (smoke, steam, fire,
  chlorine, noxious_gas, hydrogen, spark) are light particles or a pixel CA
  (your choice; a CA like the powder one, rising and diffusing, with a
  fade matching the world's decay rules, is likely cheapest). Gas fills the
  vessel's headspace, and **gas leaving the vessel mouth leaves the bench and
  enters the WORLD** at the lip of the flask in the hand (the existing live
  spill stream: DrainSpilled/TakeSpill -> ContainerSpill{pour} — a gas
  material poured is gas voxels in the world). Rendered smoke-like with the
  material's colour/opacity.
- **Stopper tool.** A third bench tool (Hand, Stick, **Stopper**): click a
  vessel's mouth to put a stopper in / take it out. A stoppered vessel is
  closed across the mouth: nothing (gas, liquid, powder) leaves. The stopper
  is PERSISTED on the ItemInstance (save format bumps as DESIGN.md "Vessels"
  describes: PLYR/ITMS/MOBS/net) and drawn on the bench and (if cheap) on the
  3D held flask. A stoppered flask poured/tipped in the world pours nothing.
  **Pressure**: gas produced in a stoppered vessel past its headspace
  capacity pops the stopper or BURSTS the vessel (a bench event, below) —
  your call, make it read well.
- **Burner tool / heat.** A burner under a vessel (toggle per vessel, a
  bench tool or a button) HEATS it. Heat is a per-vessel level that rises
  while the burner is on and falls off; contents near a hot vessel see a
  VIRTUAL `tag:hot` neighbour (the glass), with the rule's chance scaled by
  heat — so the world's own rules do the chemistry: water boils to steam,
  salt melts to molten_salt, sodium ignites, etc. Draw a flame under it.
- **Electrify button.** Shocks the selected vessel for a moment: its liquid /
  molten contents see a VIRTUAL `spark` neighbour (the `spark` material, tag
  electric) — molten salt -> sodium + chlorine via A's rules, brine
  electrolysis via a solute-conditioned rule (coordinate with B's condition
  format; if not landed, the bench may evaluate a solute condition itself
  from its per-particle solute). A visible arc. (Later: spells.)
- **Dissolving.** Using `solutes.json`: a powder grain touching a solvent
  particle dissolves at `dissolveChance` (up to `saturation`), its units
  going into the particles' solute mass; mass diffuses between particles of
  the same solvent; the liquid is tinted/glows by concentration; a particle
  whose concentration reaches a `converts` row's cMin becomes `into` (fairy
  dust + water -> enchanted water). Evaporation (steam boiling off) leaves
  `precipitatesTo`. The vessel's tally reports dissolved matter as
  **dissolved portions (contract 2.4)** — and seeding a vessel with dissolved
  portions puts the mass back into its solvent particles.
- **Conservation with reactions.** Reactions create and destroy materials, so
  ValidateBench's "before == after + spilled + streamed" gains a REACTION
  LEDGER: per material, what reactions consumed and produced (and what
  vented to the world as gas). The validator proves before + produced -
  consumed == after + spilled + streamed + vented. Matter still never
  appears from nothing: every conversion is recorded.
- **Bench events (the robust future-proof part).** A registry keyed by
  effect kind name (`ReactionEffect::kind`) -> handler. When a bench rule with
  effects fires, the bench raises a `BenchEvent {kind, effect params, where
  (vessel, sim px), substances}`. Handlers, starting with `explode`:
  the bench closes (the player is EJECTED from the alchemy UI), BOTH vessels
  in the character's hands break (their remaining contents spill into the
  world as a break does), and a REAL explosion goes off at the hands (an
  ExplosionOp through the tick — the same slot/path a grenade uses; study
  session.h `exps` / grenades; it must reach ExplosionHitsBodies so the
  player's own hands/arms take it), plus fire and smoke around it. Size from
  the effect's radius/power (and optionally how much reacted). The
  conservation validator must accept a session that ended in an event (the
  vessels' contents go to the world as spills — never lost, never
  duplicated). Also `burst` (stopper pressure) as a second handler. Design it
  so a new event kind is one handler + JSON.
- **Pocket/idle reactions (if time allows):** vessels NOT on the bench still
  react — a deterministic integer pass over each carried vessel's
  Composition in the tick (the same rules, portion-level: e.g. sodium +
  water portions -> explode event at that vessel's position). Minimal
  version: a sodium+water flask explodes when the two first meet by a
  scoop/deposit. Say what you did.

Gates (bench gates are C++ gates using FlaskSim headless like the existing
`alchemy-*` gates): `alchemy-react` (acid + sand in a flask: sand eaten,
noxious gas made and vented out the mouth, conservation ledger exact),
`alchemy-stopper` (same with a stopper: gas stays in; pressure event if you
built one), `alchemy-dissolve` (salt into water: grains vanish, dissolved
portion in the tally equals what dissolved; fairy dust + water ->
enchanted_water), `alchemy-electrolysis` (salt + burner -> molten salt;
electrify -> sodium + chlorine), `alchemy-explode` (sodium + water: an
explode BenchEvent fires; the handler's result — spills + an ExplosionOp at
the hands — validates). Existing alchemy gates must stay green
(alchemy-shake/spawn/sand-carry/layers; alchemy-pour is chaotic — see the
memory note; judge it by spread, not one run). Update `--shot-bench` if cheap
so the owner can look (e.g. `SANDVOX_BENCH_A=acid:40,sand:20` style specs).

## Package D — Healing and body effects (wave 2)

After A merges. Enchanted blood (and, weaker, enchanted water) on a body
HEALS: applied (inventory Apply / DouseLimb), poured or splashed onto a limb,
or a limb bathed in it — wounds close and MISSING VOXELS are RESTORED from
the limb's authored template (study the gore/wound code: carve, wound heal,
limb swap; "restore" is the new coat effect, authored in materials.json
`coat.effects`). Bounded: a coat of amount N restores at most a budget of
voxels, then is spent. Note the stain palette is FULL (memory): enchanted
liquids may need to share a stain slot or the coat must key on material —
figure out the clean way. Also: toxic gas effects polish if A left gaps.
Gates: `heal-restore` (carve a limb, apply enchanted blood, voxels come
back, bounded), `heal-wound`.

## Package E — Creative expansion (wave 3)

After A and C merge (B if it landed). Owner: "be creative and go all out".
Add plenty of substances and reactions, real and magical, that work in the
world AND on the bench (through the shared rules), plus new bench events
where they make sense. Candidates (use judgement, ~15-30 new materials):
sulfur / saltpeter / charcoal -> gunpowder (explode on heat); quicklime +
water -> heat (slaked lime, steam); copper + acid -> blue vitriol; aqua regia
(acid + salt-derived) dissolves gold; mercury/quicksilver; phosphorus that
ignites in air; thermite (iron oxide + aluminium) -> molten iron; slime;
philosopher's-stone lore (lead -> gold with a rare magical catalyst);
moonwater/sunwater (light-gated rules exist: when day/night); dragon's blood;
holy water vs undead/ichor/rot; frost salts (endothermic: water -> ice);
glowing potions (light); luminous fungus spores; volatile ether; smoke
bombs; healing salve; poison. New solute species where dissolving is the
point (sugar, ink, copper sulfate tint). Each with a `//` comment saying
what it is and why. Wire new tags only if needed (32-tag limit). Gates for
the headline recipes (`chem-*`). Update the tuner wiki/ARCH_NODES.

## 4. Orchestrator endgame

Merge order: A -> C -> B (as far as it got) -> D -> E. After each merge:
build once, `--verify` the merged packages' gates. At the end: one
`--selftest --gate determinism --rebaseline` (hash moved on purpose), the
alchemy + chem gates in one `--verify`, update DESIGN.md + memory, merge to
main (dirty-main landing procedure).
