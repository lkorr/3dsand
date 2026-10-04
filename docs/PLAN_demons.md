# Demons — summoning circles, seals and contracts (orchestration, 2026-10-04)

Owner design (Bartimaeus trilogy). Agreed in conversation on 2026-10-04:

- **Summoning is a spell.** You cast a demon's NAME into a salt circle from
  OUTSIDE it. Inside an intact circle the demon is CONTAINED: it can talk, and
  it can be bound by a contract. Anywhere else (no circle, a broken circle, a
  missed cast) it arrives UNBOUND and HOSTILE. That is the "goof" path, and it
  is deliberate.
- **The circle is for binding, conversing and contracting.** Release a
  contracted demon and it serves you under the contract. A demon that is too
  strong for its binding (its power exceeds the binding strength) OVERPOWERS
  the contract on release and fights. It is never an instant death. Greater
  demons have spells strong enough that the fight is close to certain death
  unless the player is skilled or late-game.
- **Only salt makes a ring** (a closed loop). Every other seal is a SMALL PILE
  of material placed near the circle, inside a band around it (inside or
  outside the ring both count). Each seal cuts one channel:
  - sulfur: casting out of the circle;
  - quicksilver: blinking/teleporting;
  - iron: touch;
  - (more are data).

  Potency scales with how many cells are in the band, measured against the
  demon's resistance for that channel. Seals are ordinary sim matter: wind,
  rain and fire move them, dissolve them or burn them, and so does the demon's
  own gust if it is still allowed to cast INSIDE the circle.
- **Name, candles, gaze.** The name is knowledge. Candles are lit
  fire/light sources around the ring. Gaze type is per demon, data
  (`hold` | `avert`). During the conversation the camera is locked on the
  demon, and looking away is an explicit action.
- **Contracts are drafted beforehand, in their own UI,** and kept as pages
  like grimoire pages. In the conversation you PRESENT one.
  - The contract has a **weight**, computed live by a tariff in JSON (like
    the spell tariff in `glyphs.json` `budgets.rates`): duration (one day is
    cheap, indefinite is dear), each prohibition, each duty and its breadth,
    and selector breadth.
  - **Binding succeeds iff `circle strength >= demon power + contract
    weight`.** Circle strength comes from the salt loop plus the seal piles.
    If it is short, the demon laughs at the draft.
  - **Upkeep:** a bound demon reserves part of the summoner's max mana while
    it is bound (`CasterState::reserved`, which already exists). That limits
    how many demons you hold. A timed contract frees the reservation when it
    expires.
  - **No paper item and no tiers.** STOCK contracts ("Servant, one day",
    "Bodyguard, one day") ship safe against imps, so a first summon is: pour
    a salt ring, cast the name, talk, present the stock contract.
- **Contract language** (extends the AI `Rule`/`Fact` system):
  - SELECTORS: set expressions, e.g.
    `attacked_by(me) & hostile_to(me) & !faction(villager)`;
  - TRIGGERS: `when` facts;
  - DUTIES: intents with arguments, e.g. `fetch(blood, into=flask)`,
    `ward(me, vs=T)`, `guard`, `follow`, `goto`, `spin`;
  - PROHIBITIONS ("cannot X"): a HARD filter enforced by the binding, and
    expensive;
  - PENALTIES ("if X then Y"): cheap, but a deterrent the demon WEIGHS;
  - a few named counters/flags, so a contract can hold a state machine. No
    loops beyond the tick, and the cost is bounded per demon per tick.
- **Demons want you dead and are cunning about it.** Default behaviour is a
  utility arbiter over a ranked motive ladder:

  ```
  survive  >  be free (dismissed / unbound)  >  harm summoner  >  comply
  ```

  Every option is weighed against that ladder:
  - authored SCHEMES (lava floor, lift-then-drop, gust at the ring, ...);
  - authored ways to carry out each kind of duty, honest or twisted (fetch
    blood FROM you, fetch water boiling, guard by attacking allies, the wrong
    ward).

  Each option carries TAGS (what it targets, `direct`, what it creates, its
  footprint). Prohibitions filter on those tags and FOOTPRINT predicates
  (`region_near(me,6)`, `creates(lava)`, `alters(ground_under(me))`,
  `affects_body(me)`). Penalties are weighed:
  - `attacks me -> dismissed` makes the demon attack you, because dismissal
    is FREEDOM;
  - `-> destroyed` stops it, because survival ranks higher;
  - `-> pain` is weighed against the demon's malice.

  Imps know few schemes and twists; greater demons know many. Difficulty is
  the size of that library, in data. **There is no open-ended planning and no
  forking of the sim.** Emergence comes from the tag/predicate gap plus real
  sim chemistry.
- **Expiry:** a higher-tier demon whose term ends while it is away from you
  gets ONE unbound act before it leaves, so "return to me before the term
  ends" is a clause worth writing. Imps simply leave.

Owner decisions for this run:
- the full plan, packages in order, merged as they land;
- worktrees, working alongside the fight64 orchestrator (see Rules);
- the demon model is a GENERATED VARIANT race on the human rig. The IMP is
  the same rig and limb system, much smaller and hunched;
- the player learns the summoning name from a BOOK in a Harrowby basement,
  plus a debug switch that grants every name.

Content picked by the orchestrator (rename freely):
- the first imp is **Skerrick**;
- the first greater demon (D6) is **Vathrael**;
- the basement is under the **smithy** (Osric is the only resident: a quiet
  smith with a secret).

## Facts the plan rests on (grounded 2026-10-04, file:line on 5f5fce7)

- **Race variants:**
  - `assets/editor/mobgen.js` has `RACE_MODS` at :95. Race modules are
    `android.js`, `automaton.js` and `robotkit.js`; the contract is in
    DESIGN.md :9841-9869. Regenerate with
    `node scripts/gen_mobs.mjs <name> --rebake | --preset | --variant-of`.
  - Height is clamped to `HEIGHT_BAND = [1.53, 1.87]` (`mobgen.js:415, :937`,
    asserted by `test_mobgen.mjs:218`), so a small imp needs a RACE-SPECIFIC
    band.
  - There is no hunch gene. Effects in `assets/mobs/effects/` can override
    clips and gait (`zombie.json` uses `chaseClip`/`gait`; `sidecar.h:43-62`).
- **Harrowby:**
  - refs are in `assets/worldmap/default/refs/harrowby.json`, blueprints in
    `assets/structures/harrowby_*.struct.json/.vox`; the smithy is at
    (700,3460), yaw 270.
  - **A structure stamp cannot carve below ground.** Template air leaves the
    world alone (`worldgen.wgsl:4703-4714`), and `kStampSinkMax = 8`. A cellar
    is an AUTHORED EDIT LAYER (`assets/worldedits/*.svedit`, the map's
    `editLayer`, DESIGN.md §9c.4) or the F8 editor plus a re-save.
  - Chests are blueprint slots with `props.items`.
  - **No readable/book item exists** (`ItemKind`, `item.h:72`). Nothing grants
    glyphs or sets flags except the dialogue `do` acts.
- **Spells:**
  - `SpellVerb` (`spell.h:172`) has no spawn verb. Add one by following the
    `Strike` pattern: enum at the end, the parse table (`spell.cpp:183`), a
    loader block (:808), tariff/volume cases (:2017, :2116, :2842), and
    `ApplySpellEffect` (:3448) pushing a new emission into `SpellEmission`
    (`spell.h:~1280`) that `FilterStreams` handles (:4052).
  - The OWNER consumes the emission in `session.cpp` (~:4413, where strikes
    are consumed). The VM never touches the world.
  - **No string arguments**, so **the NAME IS THE GLYPH:** one glyph per
    demon, e.g. `"summon": {"mob": "imp_skerrick"}`. Knowing a name = owning
    its glyph.
  - Ownership: `GlyphInventory::Grant` (`caster.h:32-50`). Everything is
    granted at startup by `GrantAllAndBind` (`main.cpp:7589`, a placeholder).
    Demon NAME glyphs must be EXCLUDED from that, except under the debug
    switch.
  - Mana: `CasterState` (`spell.h:1223`). `reserved` and `EffectiveMax()`
    exist, fed by `spells.ReservationFor(id)` (`session.cpp:4298`).
- **Mobs:**
  - `MobSystem::Spawn(defIndex, atVoxel)` (`mob.h:6024`) takes the min
    corner, so offset by size/2 as `session.cpp:1364` does.
    `SetMobBehavior(id, name)` (`mob.h:5596`).
  - Factions are per profile in `assets/mobs/behaviors.json`.
  - **Mobs cannot cast today.** The only `spells.Cast` callers are the
    player's (`session.cpp:4079`, `:4235`). The VM takes any caster id.
- **Dialogue:**
  - only through the npc ref prop `dialogue` (`refs_npc.cpp:1172`); a def
    cannot carry one, and a spawned demon needs one.
  - `do` acts: set/add/clear/give/take/end. Adding one means `Act::Kind`,
    the parse (`dialogue.cpp:216`), the validation (:445), and `RunActs`
    (:726), which runs in the tick and holds the session.
- **UI:**
  - ImGui with pixel-art chrome (`src/ui/theme.cpp`). The grimoire composer
    is `GrimoireBody` (`src/ui/inventory_ui.cpp:1312`). The dialogue panel is
    `src/ui/dialogue_ui.cpp`. Owner rule: **UI stays pixel art, and text must
    fit its boxes.**
- **Voxel reads on the CPU:**
  - `SnapMatAt` / `WorldSpellProbe(world).matAt` (`spell.cpp:3207`), and
    `World::KindAt`. `Snap()` at tick T is EXACTLY tick T-4
    (`kSnapshotLatency`, `world.h:4985`), so it is deterministic.
  - The mirror only covers 3x3x3 chunks around the player.
- **Player actions:**
  - `TickInput` (`src/sim/tickinput.h:102`) is a fixed POD:
    `static_assert(sizeof == 76)`, `kTickInputVersion = 6`, and TickButton
    bits 16+ are free. Add a feeder in `TickInputFeeder`. Targets travel as
    HASHES.
  - The op record carries the whole struct, so replay follows automatically.

## Rules for every package

- **Base check.** Your worktree may be created at a STALE base. First command:
  `git log -1 --oneline`. If it is not the commit that added this file, or a
  descendant of it, run `git merge --ff-only main` (or `git reset --hard main`
  if your branch has no commits yet). This file must exist in your tree.
- **Read first:** `CLAUDE.md`, the DESIGN.md sections on the AI behaviour
  arbiter, spells/glyphs, dialogue, refs/structures and race modules, and the
  whole of this file.
- **Build and run.** `export SANDVOX_NO_CRASH_DIALOG=1`. Build only with
  `bash scripts/build.sh` (ONCE per C++ change set), and run only with
  `bash scripts/run.sh ...`. JSON/WGSL/JS-only work needs no build: use the
  main checkout's exe with `SANDVOX_ASSET_DIR`.
- **Test budget (owner rule).**
  - No `--suite acceptance`, no bare `--selftest`, no `--vk-smoke*` unless
    you changed a render path.
  - Code first. Use a single `--gate` only while correctness is genuinely
    uncertain.
  - End with ONE `--verify <your new gates + the 2-3 covering code you
    touched>`.
  - If the hash moved on purpose: `--selftest --gate determinism
    --rebaseline` once, at the end. A moved hash is a notification. A failed
    twice-run comparison is a bug.
- **New gates.**
  - Each new gate must pass with `--gate <name>` alone, and drive the tick
    through `support::RunTicks` / `TickCursor` (`src/test/tickrig.h`).
  - Thresholds go in `tests/baseline.json`.
  - Gates use the HARNESS map, so build the fixture there (stamp a pad: see
    the memory notes on FixtureSite). The Harrowby cellar is checked by
    D2's own gate on the default map's data.
- **Regressions.** When your change regresses a gate, do NOT invent a new
  mechanic to keep it green. Record it as known-failing in
  `tests/BASELINE.md` and report it.
- **Invariants:**
  - bit-determinism: all demon/contract/circle logic is CPU gameplay state in
    the 30 Hz tick (`TickAuthority`), uses `rng::Hash3` only, and reads voxels
    only through the T-4 snapshot;
  - cost scales with activity: circle scans are event-driven, bounded, and
    sleep when no circle is live;
  - every voxel change goes through the MutationQueue/op record;
  - every new player action goes through `TickInput`.
- **The fight64 orchestrator** holds claims on `src/phys/`,
  `src/game/mob*.cpp`, `mob.h`, `ai_*` and `melee.*`.
  - Put demon code in NEW files (`src/game/demon*.{h,cpp}`,
    `src/game/contract*.{h,cpp}`, `src/ui/contract_ui.cpp`,
    `src/test/selftest_demon*.cpp`).
  - Edits to its files must be SMALL and ADDITIVE (a hook call, an enum entry
    at the end, a new Fact). Say in your report exactly which lines you
    touched there.
  - Claim on the board (`bash scripts/board.sh claim ...`) for the shared
    files you touch, and post `done` when you stop.
- **Persistence.** Anything that must survive a save (bound demons, their
  contracts, circle state, learned names) goes into the save system the
  existing way (`src/game/persist.*`, entity split). Say what is and is not
  saved.
- **Commits and docs.**
  - Commit on YOUR branch with clear messages. Do NOT merge to main; the
    orchestrator merges.
  - Update DESIGN.md (a new "Demons" section, kept current per package) and
    the STATUS table at the bottom of this file.
  - Update `assets/tuner.html` ARCH_NODES and Development Status (ASCII `'`
    delimiters only).
  - New per-demon / seal / contract content is JSON under `assets/demons/`,
    and hot-reloads with R like glyphs.
- **Final report:**
  - what you changed and your commits;
  - the exact gate result lines;
  - whether the hash moved;
  - the lines you touched in fight64-claimed files;
  - what is left open;
  - any trade-off the owner should decide;
  - **how the owner can see it in the real game** (keys, where to stand).

## D1 — demon race, imp, summon glyph, salt circle, containment

1. **The demon race** (mobgen race module `assets/editor/demon.js`, registered
   in `RACE_MODS`; folder `assets/mobs/demon/`):
   - human rig and limb system;
   - a race-specific height band and gene specs: horns, tail if the rig
     allows it cheaply (else skip), skin palette (reds, ash, sulfur yellow),
     clawed hands, eye glow if an existing mechanism supports it;
   - a race preset, plus a GREATER-demon-sized preset (taller than human is
     fine within the race band);
   - presets and the `test_mobgen.mjs` assertions updated so the human band
     still clamps humans.
2. **The imp** (`assets/mobs/demon/imp_skerrick.*`):
   - the same rig, MUCH smaller (target ~0.7-0.9 m) and HUNCHED;
   - add the hunch the cheapest correct way: a posture gene in the race, or
     an effect overriding idle/walk/chase clips and gait (zombie pattern).
     Check that it walks, falls, burns, bleeds and dies like any human-rig
     mob, and check voxel resolution at that size: limbs must not vanish;
   - a behaviour profile in `behaviors.json`: faction `demon`, hostile,
     agile, light melee (claws/bite);
   - a `assets/demons/<name>.json` def file per demon: mob def, power,
     resistances per channel (used from D3), gaze type, tier, and the names of
     its schemes (used from D6).
3. **The summon glyph** (`SpellVerb::Summon`, following the Strike pattern):
   - a glyph `"summon": {"demon": "skerrick"}`, sort/verb per the grammar;
   - it lands at the impact cell and the session spawns the demon there
     (offset so the feet stand on the floor);
   - a tariff cost in mana;
   - name glyphs are NOT granted by `GrantAllAndBind`, except under a debug
     switch (`SANDVOX_ALL_NAMES=1` plus a dev-panel toggle).
4. **The circle detector** (`src/game/demon_circle.*`):
   - At summon time, from the arrival cell, flood-fill 2D in a short slab
     (2-3 voxels tall at the demon's feet level) over the T-4 snapshot, with
     salt cells as walls.
   - The fill escaping radius R (a knob, ~4-12 m) = no circle. Otherwise the
     circle is the fill region plus its salt boundary: store the cell set or a
     compact mask, and its centre and radius.
   - Re-check ONLY when a chunk overlapping the ring changes, or every N
     ticks while live (bounded).
   - Outside the CPU mirror, hold the last state (document it).
   - Material by name ("salt"), resolved at load. **Brine/dissolved/scattered
     salt does not count.**
5. **Containment:**
   - A contained demon cannot leave the fill region: clamp the demon's
     kinematic drive or desired heading at the boundary. It must NOT be a
     physical wall: the player and items cross freely.
   - Its body and emissions crossing the ring are refused (melee reach across
     the ring included). Casting across comes in D4/D3.
   - Circle broken while contained -> the demon is UNBOUND and HOSTILE at once
     (faction `demon`, target the summoner).
6. **No circle, or a broken circle, at arrival:** spawns hostile. That is the
   goof path.
7. **A minimal "contained" state** visible in game: a HUD line or debug draw
   (a ring outline in debug only). The conversation comes in D5.
8. **Gate `demon-circle`.** On a harness pad:
   - pour a closed salt ring and summon inside -> contained, and the demon
     stays inside for N ticks while targeting you;
   - break one cell of the ring -> hostile, and the demon leaves the circle;
   - summon with no ring -> hostile;
   - a ring with a 1-voxel gap -> not a circle.

   Deterministic: the twice-run trace is identical.

## D2 — the Harrowby cellar, the book, learning names (runs in PARALLEL with D1)

1. **A cellar under the smithy:**
   - authored as an edit layer (or the documented path the editor uses for
     below-ground space);
   - an entrance from inside the smithy (a trapdoor/hatch in the floor, or
     stairs down in a corner);
   - the cellar is ~7x7 m, stone-walled, ~3 m tall;
   - lit: candles (the existing fire/light materials or props);
   - nothing must break the village gate (`village-harrowby`) or its pinned
     trace beyond an expected rebaseline. Ask before moving the smithy.
2. **A ready circle in the cellar:**
   - a closed salt ring (~3 m diameter) on the floor;
   - small piles of sulfur, iron filings (whatever iron material exists) and
     quicksilver beside it, in a vessel or a shallow dish if quicksilver would
     run;
   - candles at the cardinal points.

   It does nothing until D1/D3 land; it is pure world content now.
3. **The book:**
   - a readable world object on a lectern or in a chest. Implement the
     smallest general mechanism: e.g. a `readable` ref kind whose use opens a
     DIALOGUE file (the book's pages are dialogue nodes, with no speaker
     portrait or with "the book" as speaker).
   - Add dialogue `do` acts:
     - `grant` (a glyph by name, which goes through `GlyphInventory::Grant`
       and persists);
     - `learn` (a name, as a world flag `name:<demon>`, if a flag is needed
       beyond glyph ownership).
   - The book teaches:
     - the glyph `summon_skerrick` (this exact name; D1 creates the glyph
       entry: grant by NAME, and if the glyph does not exist yet the grant
       must fail soft with a load-time warning, not a crash);
     - how to draw a circle and what the piles do, in-world prose, 2-4 short
       pages, rural-medieval voice, a smith's notes.
4. **The debug switch:** a dev-panel button "learn all demon names", plus the
   `SANDVOX_ALL_NAMES=1` env. Coordinate the name with D1: D1 owns the env
   read and D2 owns the button, or whichever lands first owns both. Say which
   in the report.
5. **Gate `harrowby-cellar`.** It can run on default-map data or a fixture:
   - the cellar air volume exists under the smithy and is reachable from the
     smithy floor (nav or flood fill);
   - the salt ring is a closed loop by D1's detector rule (if D1 is not
     landed, implement the 2D loop check locally in the gate);
   - reading the book grants the glyph.
6. **A screenshot** of the cellar (`--shot`-style scene or the editor's
   capture) in the report.

## D3 — seal piles, circle strength, demon power, overpower, candles, gaze (after D1)

1. **Seals as data** (`assets/demons/seals.json`):
   `{material, severs, perCell}`.
   - Count each seal material's cells inside the annulus: ring ±k voxels,
     slab height. That is a bounded pass over a known region, re-run on the
     same triggers as the circle.
   - Potency per channel = sum of `perCell x count`. A channel is SEVERED iff
     its potency >= the demon's resistance for that channel.
2. **Channels for now:**
   - `move` (salt);
   - `cast_out` (spells cast by the demon cannot leave the circle: filter the
     demon's emissions whose footprint crosses the ring; this is wired fully
     when D4 lands, so add the hook now);
   - `blink` (D4's teleport is refused);
   - `touch` (melee across the ring);
   - `sight`/`voice` only if trivial.

   An unsevered channel the demon HAS is a loophole: the demon can use it from
   inside the circle.
3. **Circle strength** = f(salt loop integrity/width, seal potencies,
   candles lit). Candles are lit fire/light cells at points around the ring;
   wind or rain putting them out is free sim behaviour. The formula and its
   constants are data.
4. **Overpower:**
   - On RELEASE (an explicit player action, routed through `TickInput`; a
     key or a dialogue choice is fine), compare `power` with
     `strength - contract weight` (weight is 0 until D5).
   - If the demon wins, it is unbound and hostile. With D5, a won check means
     the contract holds.
   - Also continuously: if the circle breaks while the demon is contained,
     it is unbound.
5. **Gaze:**
   - Per demon (`hold`/`avert`). A cone test of camera forward against the
     demon's head, per tick while contained and unbound (camera direction is
     already player input).
   - Breaking the gaze rule raises a STRAIN meter. At max, the circle's
     strength drops by a chunk: no instant fail.
   - The camera lock and the look-away key arrive with D5's conversation.
     Here, gaze is the meter only.
6. **Demon tells:** the demon's taunt lines depend on the margin
   (strength - power - weight). Write a small line table now and wire it into
   D5's dialogue later.
7. **Gate `demon-seals`:**
   - sulfur pile present -> a cast_out attempt is refused; absent -> allowed;
   - a quicksilver pile below resistance -> blink allowed;
   - wind scattering a pile below threshold flips the channel. Use a scripted
     cell removal through the queue if real wind is too slow for a gate;
   - overpower: a strong demon in a weak circle goes hostile on release, a
     weak one does not;
   - deterministic.

## D4 — demons that cast: mob spellcasting, blink, the demon spell kit (after D1; may run alongside D3)

1. **The mob casting path:**
   - the spell VM is player-agnostic, so give mobs a cast request from the AI
     (a new Intent `Cast` at the END of the enum, plus an attack-request-like
     seam). The session calls `spells.Cast` for the mob with its own
     `CasterState` and mana;
   - emissions are owned and filtered exactly like the player's (all voxel
     effects through the queue);
   - budgets: per-mob caps, so 64 casting mobs stay bounded.
2. **Blink:** a short teleport verb for demons (mob locomotion: a target cell
   within range, line-of-sight optional per demon, a cooldown). Refused when
   the `blink` channel is severed, or when the destination is outside the
   circle while contained.
3. **The demon spell kit** (glyph pages in `assets/demons/spells/` or the
   grimoire library, built from EXISTING glyphs where possible):
   - firebolt;
   - gust (aimable at the salt ring);
   - transmute ground under the target to lava;
   - LIFT applied to ANOTHER body (the player floats up, then drops): if
     float/lift cannot target a body today, add that;
   - a ward on self.

   Tag every spell with its footprint metadata now (targets, direct, creates,
   region): D5/D6 filter on these.
4. **Greater-demon lethality:** a greater demon fights with the full kit at
   high power and is near-certain death for an unprepared player. It is
   tunable in data, and NOT an instant kill.
5. **AI:** the imp profile uses gust and firebolt sparingly, and when
   contained with `cast_out` unsevered it casts at the summoner from inside
   the circle. (The emergent "gust the ring" scheme is D6. Here only the
   capability and a simple rule are needed.)
6. **Gate `demon-cast`:**
   - a hostile imp casts a firebolt at the player in the fixture;
   - with sulfur severing cast_out, the bolt is refused at the ring;
   - blink moves the imp and is refused when severed;
   - lift-on-body raises the target;
   - deterministic, bounded.

## D5 — conversation, contracts, weight, upkeep, the contract UI (after D3)

1. **Demon dialogue:**
   - a spawned demon gets a dialogue file from its demon def (extend dialogue
     assignment beyond npc refs, minimally);
   - talking is possible only while the demon is CONTAINED;
   - camera lock plus a look-away action for gaze;
   - the taunt/tell table from D3;
   - acts: `present_contract` (opens a picker of the player's contract pages),
     `release`, `dismiss`.
2. **The contract model** (`src/game/contract.*`, pages saved like grimoire
   pages):
   - clauses = selector + trigger (`when` facts) + kind (duty / prohibition /
     penalty(consequence)) + args;
   - a small, fixed vocabulary, in data where possible;
   - selectors are set expressions over actors: `me`, `attacked_by(me)`,
     `hostile_to(me)`, `faction(x)`, `type(x)`, and & | !;
   - a few named counters/flags;
   - evaluated in the tick, bounded (clause cap, expression node cap).
3. **The weight tariff** (`assets/demons/contract_tariff.json`):
   duration, prohibitions, duties x breadth, selector breadth. Shown live
   in the UI with a per-line breakdown.
4. **Binding:**
   - `present` -> `strength >= power + weight` -> the demon is BOUND under
     that contract, else it refuses and taunts;
   - `release` (the overpower check from D3 now includes the weight) -> the
     demon leaves the circle and serves;
   - upkeep: `reserved` mana proportional to power, for as long as it is
     bound;
   - timed contracts expire, and the demon departs (D6 adds the free act);
   - `dismiss` from anywhere.
5. **Stock contracts:** "Servant, one day", "Bodyguard, one day" and "Fetch,
   one day", written to be safe against imps (given D6's schemes, revisit
   them in D6).
6. **Enforcement in D5:** duties drive intents (follow, guard, goto, fetch
   with a flask/pouch, spin), and prohibitions hard-filter the demon's
   targets and casts by tag. Motive weighing of penalties is D6. Here
   penalties just fire their consequence.
7. **The contract UI** (`src/ui/contract_ui.cpp`, pixel-art chrome, text
   fits):
   - opened from the grimoire;
   - clause cards built from tokens: subject / verb / object / condition /
     consequence;
   - live weight and upkeep;
   - simple by default: a stock template, plus "add clause" with a few
     choices;
   - complex when wanted: selectors, counters, triggers.
8. **TickInput:** present (contract page hash plus demon id), release,
   dismiss and look-away travel through `TickInput` (a version bump), so
   ops-replay holds.
9. **Persistence:** bound demons, their contracts, remaining term and upkeep
   survive save/load.
10. **Gate `demon-contract`:**
    - a weight over strength refuses;
    - the stock servant binds, follows, and never attacks the summoner;
    - a "bodyguard" with the selector `attacked_by(me) & hostile_to(me)`
      attacks only that set;
    - expiry departs and frees the reservation;
    - save/load round-trips a bound demon;
    - ops-replay holds.

## D6 — malice: motives, schemes, penalties weighed, duty twists, greater demon (after D4 + D5)

0. **Owner decision 2026-10-04: SALT BLOCKS BLOWS, IRON WEAKENS.** Revert D3's
   `touch` channel: a contained demon's blows (and grabs) across the ring are
   ALWAYS refused by the salt (D1's original fence behaviour). Iron no longer
   severs a channel; instead iron in the band LOWERS THE CONTAINED DEMON'S
   EFFECTIVE POWER (per-demon `ironSusceptibility`, data; potency from mass like
   every seal, capped so iron alone never zeroes a greater demon). It applies
   only while contained -- the overpower check on release uses the reduced
   power, so iron makes binding strong demons cheaper. HUD `M C B T` loses `T`
   and shows the iron reduction on the CIRCLE line (`POWER 20-6`). Update
   seals.json, demon-seals / demon-circle assertions, DESIGN §17, and the book
   text in assets/dialogue/osric_notes.json if it says what iron does.

1. **The motive ladder** per demon (data): survive > free > harm summoner >
   comply, with `malice`, `cunning`, `spite` and `literalism`. Each think
   tick, the demon scores its permitted options against the ladder:
   - schemes;
   - duty executions;
   - penalty consequences as costs or rewards. Dismissal is a REWARD;
     destruction is a survival cost.
2. **The scheme library** (`assets/demons/schemes.json`): preconditions
   (facts), the action (a D4 spell or move), tags, footprint. Start with:
   lava floor, lift-then-drop, gust at the ring (break containment), fire
   upwind of flammables, lure into water then shock (if electricity allows
   it cheaply), and stand where the summoner falls.
   - Prohibitions filter by tag AND by footprint predicates (`region_near`,
     `creates`, `alters(ground_under)`, `affects_body`).
   - Imps know ~3 schemes; Vathrael knows many.
3. **Duty twists** (`assets/demons/twists.json`): per duty kind, honest plus
   twisted executions, tagged:
   - fetch FROM the summoner / in a harmful state / delivered ONTO;
   - guard by attacking allies, or crowding;
   - the wrong ward;
   - follow by leading astray.

   A fully specified duty leaves only the honest option.
4. **Penalty weighing:** `attacks me -> dismissed` makes it attack; `->
   destroyed` stops it; `-> pain` is weighed against malice.
5. **Expiry free act** for tier >= 2. A clause "return before term" closes
   it.
6. **The cheap outcome clause:** a ring of the demon's recent action
   footprints; harm to the summoner inside one within N ticks fires the
   clause's consequence.
7. **Vathrael**, a greater demon:
   - can talk from the circle and gives a quest hook (a dialogue only; the
     quest content can stay a stub);
   - power beyond any reasonable circle, so release = the fight;
   - his tells say so.
8. **Revisit the stock contracts** against the imp's schemes.
9. **Gate `demon-malice`:**
   - `cannot cast hostile magic at me` alone -> the imp picks lava floor;
   - adding `!alters(ground_under(me))` -> it does not;
   - the dismissal penalty -> it attacks;
   - the destruction penalty -> it does not;
   - an under-specified fetch -> a twisted execution, a fully specified one
     -> honest;
   - deterministic.

## Sequencing

```
D1 ─┬─ D3 ─┐
D2 ─┘      ├─ D5 ── D6
     D4 ───┘   (D4 after D1; it may run alongside D3)
```

D1 and D2 in parallel; then D3 and D4 in parallel; then D5; then D6. The
orchestrator merges each package, rebases the in-flight branches, and rebuilds
main's exe at the end.

## STATUS

| Pkg | State | Commit | Notes |
|---|---|---|---|
| D1 | MERGED main | e5b8097 | demon race + imp Skerrick, `summon_skerrick` (SpellVerb::Summon), salt circle by mass (minEighths 4), containment fence, gate `demon-circle`. Final --verify (demon-circle, harrowby-cellar incl. D1's detector on the cellar ring, mob, determinism, ai-approach): PASS, hash 0fa43063 unmoved. The earlier `mob` "1 awake after settle" was the pre-fight64-P tree (failed with D1 assets removed too); main 39a8b56 and the merged branch both pass. Open: the imp stands ~1 voxel high in --shot-mob (foot clearance +1.05 vs the human's -0.98). |
| D2 | MERGED main | 640834a | Cellar in the default edit layer (scripts/paint_cellar.mjs); `readable` ref kind; dialogue `grant` / `learn`; `game/demon_lore.h` (IsNameGlyph = id prefix `summon_`, GrantAllNames) + dev button "learn all demon names" (D1 owns `SANDVOX_ALL_NAMES` and the GrantAllAndBind exclusion; call demon::GrantAllNames / IsNameGlyph). New materials `tallow`, `candle_flame` appended: if D1 also appends materials, re-run `node scripts/paint_cellar.mjs` after the merge. Gate `harrowby-cellar` pass; village-harrowby rebaselined (now passes). |
| D3 | MERGED main | 13e802b | `game/demon_seals.{h,cpp}`: seal band by mass (seals.json), channels move/cast_out/blink/touch, strength (salt + seals + candles - gaze loss), release on Y (`TB_DEMON_RELEASE`, kTickInputVersion 7; held -> Released + `imp_bound` profile, else overpowered -> unbound), gaze strain meter, tells.json, HUD line 2/3. D4 hooks: `demon::ChannelSevered(const TickAuthorityCtx&, uint64_t, Channel)`, `AllowCastOut(w, id, from, to)`, `AllowBlink(w, players, id, to, tick)`. **Behaviour change:** a ring without iron no longer stops claws across it (touch is a loophole); demon-circle's B1 now asserts the fence was asked. Final --verify (demon-seals, demon-circle, harrowby-cellar, determinism): PASS, hash 0fa43063 unmoved. Not saved (D5). |
| D4 | MERGED main | a05ea1c | `game/demon_cast.{h,cpp}`: `ai::Intent::Cast` (appended before Count) + `CastTuning` (profile `cast` block) + `CastRequest` seam (`MobSystem::CastRequests`); one SpellSystem + mana pool per casting creature (`MobCastTick` after phase H, `MobCastWardFilter` after phase M), emission owned/filtered like the player's, budgets (64 VMs, 8 requests/tick, 32 ops/tick, `maxLive`). Kit `assets/demons/spells/` (firebolt, gust, lava_floor, lift, ward, hellfire, blink) with footprint tags. `MobSystem::BlinkMob`. D3 wired: `demon::AllowBlink` (unsevered blink out = unbound, the loophole; no D4 fence of its own, per the orchestrator), `AllowCastOut` at the ring crossing, `ChannelSevered(CastOut)` for instant/resolve footprints. Imp casts firebolt/gust sparingly (no blink); `fiend` body + profile (full kit, 2400 mana). D1 float FIXED in `pose.cpp` (a STANDING body is drawn no higher than its legs reach its planted feet): flat pad imp +0.50 -> -0.01, human +0.47 -> -0.08; one-voxel step under the footprint imp -0.23..0.02; `--shot-mob imp_skerrick` +1.05 -> +0.06. Speed follows size below a man (mobgen: x sqrt(height/1.53) under the human band; imp 31.5 -> 23.5 vox/s, re-baked). Final --verify (demon-cast, demon-circle, demon-seals, spells, ai-approach, mob, determinism): all PASS but `mob` (crater shape torso, cleaner=0: 3.54 vs 4.18 x 0.8); a control arm with D4's pose rule switched off reproduced the identical numbers, so it is inherited from main (fight64 M/Q). Hash 0fa43063 unmoved. Not saved: casters, mana, statuses, bolts in flight. |
| D5 | planned | | |
| D6 | planned | | |
