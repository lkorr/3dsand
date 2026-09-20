# PLAN: characters INHERIT the human; zombie is an EFFECT

Status: **LANDED 2026-09-20, phases 1-5a.** Phase 5b (lazy `DefWithEffects` at
spawn + per-mob `effects` in the save) and phase 6 (the player's corpse) are
NOT implemented; see "What landed, and what did not" at the bottom of this file
for the reason and for the two places the plan's own premises did not survive
contact. The design below is otherwise as shipped. Brief:
`docs/PROMPT_character_inheritance.md`. Ground truth below was read off the
tree at `2c4f29b` by four read-only research passes (loader, generator,
zombie/save, anatomy); every claim carries a `file:line`.

## 0. The finding that shapes everything

**The inheritance mechanism already ships, and nothing uses it for a body.**

- `ResolveSidecar` (`src/game/mob.cpp:480-508`) resolves `extends` by RFC 7396
  merge-patch, depth 8. `CollectMobSources` (`:532-569`) pairs every `.vox`
  with its own `.json` FIRST, so a sidecar with `extends: "human"` AND its own
  `<name>.vox` keeps its own art and inherits everything else. That is exactly
  the shape a generated character needs, and it is legal today. (Corollary to
  pin: a `model` key is silently IGNORED whenever a `.vox` of the same stem
  exists, `:556-559`. Undocumented.)
- `assets/anims/` propagates because it is a shared source compiled per rig by
  limb NAME, a sidecar clip of the same name wins, partial fits are kept, zero
  fits are skipped silently (`mob.cpp:292-358`, `:1606-1613`). There is no
  second mechanism like it: `states`, `natural`, `gait`, `chains`, `sockets`,
  `limbs` are read only from the merged sidecar (`:977-1581`).
- A generated sidecar is **97% copy**. `jujunud.json` is 63,557 B; its
  per-body content is 12 anchor Y values, two `hp`, `gait.rideHeight`, one
  socket offset, and the `walk`/`run` clip periods (22 leaves inside 45 KB of
  clips). Thirteen of its fifteen clips are byte-identical to the human's;
  `hop` (5 KB) is selected by no state since `96c86d5`; `attack` is a verbatim
  duplicate of `cast` (`mobgen.js:2119`).
- `zombie.json` is ALREADY an overlay: `undead`, `palette`, `rot`,
  `bleed.woundHeals`, `bite`, `behavior`, `chaseClip`, `speed`, `gait`. Every
  key is an ordinary sidecar key; C++ branches on `undead` in exactly four
  places (`mob.cpp:718`, `:2158`, `:7693`, `WoundsHeal`) and on the string
  `"zombie"` nowhere outside tests. What is missing is only a way to apply
  that overlay to a base other than `human`.
- The engine does not read `anatomy` or `genome` at all (`grep` finds neither
  in `src/`). The interior reaches C++ only as baked `.vox` materials.
- **Nothing in this plan can move `determinismHash`.** `GateDeterminism`
  spawns no mobs (`src/test/selftest_sim.cpp:32+`) and no `MobDef` field is
  hashed (`sim_occupancy.wgsl:167-304`). The three commits that rewrote
  `human.vox` touched neither `tests/baseline.json` nor `tests/BASELINE.md`.
  Mob-sensitive gates compare threshold numbers (`undead*`, the `mob-burn`
  census), and every phase below is a no-op on those numbers by construction.

So the refactor is mostly AUTHORING plus two small loader rules, not a new
system. The two hard seams are exactly the two the brief names: merge-patch
cannot override ONE limb (arrays replace wholesale, `:448-454`), and anatomy
is art.

## 1. Design

### 1.1 Vocabulary

- **Base** — `assets/mobs/human.json` + `human.vox`. The contract: limb names
  and tags, chains, states, gait constants, natural weapons and their strike
  profiles, sockets, bleed, anatomy recipe, the shared clips.
- **Character** — a sidecar that names a base with `extends`, carries its own
  `.vox`, its `genome`, its name, and ONLY its per-body derived numbers. Roughly
  2 to 3 KB against 63 KB today.
- **Effect** — `assets/mobs/effects/<name>.json`: an overlay applied AFTER
  `extends` resolution to whatever body it lands on. `zombie` first; `burned`,
  `skeletal`, `armoured` are the second and must fit the same file shape.
- **Resolved def** — what `LoadMobDefs` builds from base + character +
  effects. It is derived data (design guideline 3): never saved, never hashed,
  reconstructible from the three files.

### 1.2 Inherit the CONTRACT, derive the NUMBERS

The generator stays the deriver. It already computes every per-body number
(`buildSidecar`, `mobgen.js:2555-2849`); it stops copying the contract. The
loader stays the inheritor. The line is drawn by what the loader would need
from the `.vox` to derive a value itself:

| per-body number | who derives it | why not the loader |
|---|---|---|
| 12 limb `anchor`s | generator (`jointTop`/`jointBottom`, `:2580-2590`) | `human.json` authors its anchors by hand; deriving in C++ would move the human's joints, which every combat gate stands on |
| `limbs[].hp` | generator (`hpOf`, `:2596`, base × volume ratio) | could be loader-derived from `weightAtSpawn` (`mob.cpp:2257`) later; not in this plan, because it changes the human's meaning of `hp` |
| `gait.rideHeight` | generator (`gaitNumbers`, `:1924`) | `BuildRig` has `restSoleY_`/`restHipY_` (`mob.cpp:2370-2403`) and could; same argument |
| `sockets[].offset`, `natural[].edge`, `halfWidth` | generator (`partLocal`, `:2734`) | fractions of the part box; loader-derivable in a follow-up |
| `walk`/`run` period | generator (`armCycleMs`, `:1906`) | needs the retime rule (§2, phase 2) so a period can be overridden without restating 5 KB of keys |

Everything else in a character sidecar is inherited. `speed` is derived from
`tuning.json`, not the genome (`:2821`), so it is contract.

### 1.3 Two loader rules, and no more

1. **Named-array merge.** In `ResolveSidecar`, an array whose elements all
   carry `name` merges BY NAME: a child element patches the base element of
   the same name, base order is preserved, unmatched names append. Arrays
   without names keep RFC 7396 replace. This is what lets a character say
   `"limbs": [{"name": "legL.L", "hp": 50, "anchor": [...]}]`. Order
   preservation matters: `TopoSortLimbs` output feeds the save file's
   positional limb records (`mob.cpp:16070`), so a merge that reordered limbs
   would silently misplace every saved mob's damage.
2. **Clip retime.** A clip patch carrying `durationMs` and NO `tracks` scales
   every key time by `new/old`. Without it, `{"clips":{"walk":{"durationMs":638}}}`
   retimes the clip and leaves its keys where they were (`tracks.*.rot` is an
   array, so it replaces).

Both are mirrored line for line in the JS `mergePatch` already living in
`assets/editor/editor.js:4278-4313`, extracted to a shared module so the
Characters page, `test_mobgen.mjs` and the Models tab resolve identically.

### 1.4 Effects

An effect file has two blocks:

```json
{ "patch": { "undead": true, "palette": {...}, "rot": {...}, "bite": {...},
             "behavior": "zombie", "chaseClip": "reach",
             "bleed": {"woundHeals": false}, "gait": {"stepHeight": 0.06} },
  "scale": { "speed": 0.746, "gait.cadence": 0.8 } }
```

`patch` is the same merge (with named arrays) as `extends`. `scale` multiplies
numeric leaves by path. It exists because the zombie's `speed: 23.5` and
`cadence: 6.4` are ABSOLUTE human numbers; applied to a small genome they
would give the wrong stride (`stride = speed/cadence`). A modifier must say
"three quarters of whatever this body walks at". A sidecar names its effects
with `"effects": ["zombie"]`, applied in order after `extends`.

`zombie.json` becomes `{"extends": "human", "effects": ["zombie"]}` and stays,
because six gates and the NPC panel find it by name (§4). A zombie of jujunud
is `jujunud_zombie.json`, three lines.

**Effects are LOAD-TIME, producing derived defs, not per-instance flags.** The
reason is hard: `palette` is a per-def filter applied before `MicroBodyMergeArt`
dedupes by RGB (`mob.cpp:695-708`), and the per-instance GPU struct has no spare
word (`src/sim/microbody.h:138-144`). A per-mob "is a zombie" bit could not
recolour anything. What a spawn-time API needs (player corpse, turning) is a
LAZY derived def: `MobSystem::DefWithEffects(base, effects)` synthesising and
caching `<base>+<effects>` through the same resolve. That is phase 5b, and it
is where per-mob `effects` enter the save.

What an effect may NOT do: rename or remove a part. The rig vocabulary
(`ARCHETYPE`, and the tags/part names C++ binds: `mob.cpp:267-281`,
`avatar.cpp:317-338`) is the contract every other system binds to.

### 1.5 Anatomy: resolve at load, keep the bake as preview and cache (option C)

Decision, stated explicitly as the brief asks:

- **C++ becomes the authority.** `ResolveAnatomy(def.prefab, recipe, mats)`
  runs in `LoadMobDefs` after the art merge (`:695-708`) and BEFORE
  `UpsamplePrefab` (`:952-975`), or depth is measured in upsampled units. The
  recipe comes from the RESOLVED sidecar, so `extends` and effects reach it:
  a `skeletal` effect is `{"anatomy": {"layers": [{"material": "bone"}]}}`
  in `patch`, not a second 200 KB `.vox`.
- **The bake stays**, reclassified as a preview and a cache. The Models tab's
  peel, `check_anatomy.sh` and `test_anatomy.mjs` all read the interior off
  disk (`editor.js:1780-1831`, `test_anatomy.mjs:119-148`) and keep working.
- **It is a no-op on every asset on disk today.** Measured: `planAnatomy`
  over `human`, `jujunud`, `newcomer`, `_harness` against their own recipes
  reports 0 voxels to rewrite. So the resolve lands with zero art migration
  and zero movement in any gate census.
- **Cost:** ~0.3 ms per mob per load (88,128-cell dense field, 26,494 solid
  voxels for the human), one 88 KB scratch buffer. Negligible against a 3 s
  device boot.
- **Parity is bit-exact and must be demanded.** Multi-source 6-connected BFS
  is order-independent; `cellHash` (`anatomy.js:235-240`) is `uint32_t`
  arithmetic once the JS int32 coercion is respected; `fraction * 10000` must
  be computed in `double`. Port `cellHash`, `layerIndexAt`, the garment rule
  and the own-face override with the JS lines as the comment.

Why not B (bake only, make it automatic): a staleness gate is ten lines and
closes today's hole, but the interior stays a derived fact stored in an
authoritative-looking file, an effect cannot override a recipe without its own
art, and `2c4f29b` has already paid for that once (a head recipe with no scalp
layer, fixed by regenerating both bodies wholesale). Why not pure A: three JS
consumers break and there is no reason to break them.

Two live drifts the research found, fixed in the same phase:
`assets/editor/anatomy.js:66-85` `DEFAULT_ANATOMY` is a THIRD copy of the
recipe with head core `flesh` where `human.json` and `anatomyRecipe()` say
`brain`, and `scripts/test_anatomy.mjs:66` pins the stale value. And every
generated recipe carries `garments: ["linen"]` while generated bodies have zero
linen voxels (their shorts are paint, `mobgen.js:1363-1393`) — the clause is
inert, and a generated body has flesh under its painted shorts where the human
has skin.

## 2. Phases

Each phase is independently landable, verified by a gate costing no build and
no GPU lock wherever the phase touches no C++. `node scripts/test_mobgen.mjs`
measured at 7.9 s wall this week (3,794 checks), not the 1 s the brief
remembers; still the bar.

### Phase 1 — shared clips move to the library (WGSL/JSON only, NO build)

- Move `idle jump fall land hang cast limp crawl squirm onearm headless` out
  of `human.json` into `assets/anims/<name>.json`. Delete `attack` (alias of
  `cast`; `attack_styles.json` names `cast` or gets the alias file). Delete
  `hop` or move it; it is dead since `96c86d5`.
- `buildClips` (`mobgen.js:1934-2261`) emits only `walk` and `run`.
  `gen_mobs.mjs --rebake jujunud newcomer`.
- `walk`/`run` stay inline per body for now (phase 2 thins them).
- Gate: `node scripts/test_mobgen.mjs` §L (compares only the keys the
  generator still emits, so `clips` shrinks to walk/run) + ONE
  `--gate mob` run, which already asserts every library file compiled onto
  the human under its stem (`assets/anims/README.md`).
- Risk: library clips scale `pos` keys by their OWN `sidecarVoxelsPerMetre`;
  the moved clips must carry `10`. `chaseClip`/`states[].clip` are validated
  at load after the library merge (`mob.cpp:1617-1627`), so a missing file
  is loud.
- Saves: none affected (clips are not persisted).

### Phase 2 — named-array merge + clip retime (C++, one TU)

- Put the resolver in a NEW file, `src/game/sidecar.cpp/.h`
  (`ResolveSidecar`, `MergeNamedArrays`, `RetimeClipPatch`, `ApplyEffect`),
  called from `LoadMobDefs`. New file, not `mob.cpp`, because two live claims
  hold `mob.cpp` today (agent-02494b, agent-6b01a8 on the board) and the
  resolver has no reason to share a TU with 19,000 lines.
- Mirror in `assets/editor/sidecar.js` (extract `mergePatch`/`resolveExtends`
  from `editor.js:4278-4313`, add the two rules), imported by `editor.js`,
  `breed.js`, `gen_mobs.mjs`, `test_mobgen.mjs`.
- Gate (cheap): new §N in `test_mobgen.mjs`: fixture patches over a fixture
  base exercising name-merge, order preservation, append, `null` delete,
  retime, scalar/array replace. Plus a C++ `--gate sidecar-resolve` that dumps
  each resolved def's JSON into `build/last_run.json`; §N reads that file when
  present and diffs it against the JS resolution of the same sidecars. One exe
  run per C++ change, never re-run for a JSON edit.
- Pin the `model`-ignored-when-own-vox-exists rule with a log line, or make
  `CollectMobSources` honour `model` and warn. Decide in review; the plan
  assumes "warn, keep behaviour".

### Phase 3 — the generator emits THIN sidecars (JS only, NO build)

- `buildSidecar` emits: `extends: "human"`, `model: "<name>"` (honest even
  though ignored), `sidecarVoxelsPerMetre`, `genome`, and overrides only:
  `limbs` (name + anchor + hp where they differ from the base's value at the
  default genome), `gait.rideHeight`, `sockets[held_right].offset`,
  `natural[*].edge/halfWidth`, `clips.walk/run.durationMs`.
- Emission is a DIFF against the resolved base, computed by the shared
  `sidecar.js`, not a hand-picked list: whatever the generator derives that
  equals the base is dropped. That is what keeps phase 3 honest when a later
  commit adds a derived field.
- `gen_mobs.mjs --rebake` all characters. `breed.js` `saveCharacter()`
  (`:411-457`) writes the thin doc; `loadPool` (`:361-392`) needs no change.
  `tuner_server.py` has no schema check, so no server change.
- **§L stays exactly as it is** (generator at the default genome vs
  `human.json`: the base is still the full file, so it is still the right
  test). **§M is re-aimed**: `resolve(onDisk)` must equal
  `resolve(generateThin(onDisk.genome))` and, once, must equal the FAT sidecar
  the same genome produced before this phase (keep `jujunud.json` at
  `2c4f29b` as `tests/fixtures/jujunud_fat.json`). That second claim is the
  proof that thinning changed nothing, and it is deleted after one green run.
- Models tab: derived sidecars are read-only by design (`editor.js:4249-4270`),
  and after this phase every character is derived. Accepted: a generated
  character is re-rolled in the Characters tab, not sculpted. If sculpting a
  character becomes a need, the editor saves the diff through `sidecar.js`
  exactly as the generator does.

### Phase 4 — anatomy resolved at load (C++, one TU, plus JS fixes)

- `ResolveAnatomy` in `src/game/anatomy_resolve.cpp` (new TU), called from
  `LoadMobDefs` between the art merge and `UpsamplePrefab`. Recipe read from
  the resolved sidecar under `anatomy`, materials by name via
  `FindMaterialId` (already used at `:859`).
- Collapse `DEFAULT_ANATOMY` (`anatomy.js:66-85`) to a re-export of
  `mobgen.anatomyRecipe()` and fix `test_anatomy.mjs:66` to `brain`. Drop the
  inert `garments` clause from generated recipes, or make the garment rule read
  the art slot; the plan assumes "drop", stated on the Characters page.
- Gates: `node scripts/test_anatomy.mjs` (fixed pin) and a new
  `test_mobgen.mjs` §O that runs `planAnatomy` over EVERY `.vox` in
  `assets/mobs/` and asserts `report.total === 0` (staleness, ~20 ms per body;
  a stored hash buys nothing over this). C++: `--gate anatomy-parity`, which
  for each def compares the runtime limb-material census against the on-disk
  `.vox` census (resolve must be a no-op on baked art) AND strips one def's
  interior to skin in memory, resolves, and compares to disk (resolve must
  reproduce the bake). The second arm is what makes two implementations safe.
- No baseline numbers move: `undead*`, `mob-burn` census, `impact-*` rot
  counts all read the runtime lattice, which is unchanged.

### Phase 5a — effects as files (C++ in `sidecar.cpp`, JSON)

- `assets/mobs/effects/zombie.json` with `patch` + `scale` as in §1.4.
  `zombie.json` → `{"extends": "human", "effects": ["zombie"]}`.
  `jujunud_zombie.json` as the proof the effect composes.
- `speed`/`cadence` become `scale` ratios of the human's (0.746, 0.8).
  `rot.blob` is in SKIN voxels (`MobRotDef::blob`), the one absolute in the rot
  block; leave it, note it, since a feature size should not shrink with a
  short body.
- `SetMobBehavior` overrides are discarded on hot reload (`mob.cpp:1873-1889`
  re-resolve from `def_->behavior`); effects avoid this entirely because the
  derived def carries `behavior`.
- Gates: `undead` unchanged (spawns `"zombie"` by name, which still exists and
  still resolves to the same numbers; claim A compares prefab size and
  `held_right` survival, which hold). Add a `jujunud_zombie` arm to the
  existing `undead` gate: one spawn, assert rot holes, palette differs from
  `jujunud`, `speed == jujunud.speed * 0.746`. `test_mobgen.mjs` §N gains an
  effect fixture. `zombie-draw`, `lunge`, `bite-target`, `ai-band` keep
  working by name.
- `--shot-strike <def>` and the NPC panel list defs by name; a derived def
  file appears in both with no code change.

### Phase 5b — lazy effects at spawn + save (C++)

- `MobSystem::DefWithEffects(baseIndex, effects)` synthesises and caches the
  derived def (appending to `defs_`; sources are sorted own-art-first so
  existing indices do not renumber, `mob.cpp:546-555`). Each derived def costs
  a micro pack in the shared pool; bound it, and log it the way `physScale` is
  logged.
- `MOBS` save v3: append `effects` per mob after the limb loop
  (`mob.cpp:15987`); loader accepts 2..3 the way `PLYR` does
  (`persist.h:58-85`), since `LoadState` is strict equality today
  (`:15992`). On load, resolve `DefWithEffects` and then spawn under the
  existing `loading_` guard so `RotAtSpawn` does not re-roll (`:2158`).
- `SetDefs` re-seats `def_` by INDEX (`:1867-1878`), safe only because reload
  despawns everything first. Lazy defs make that trap live; switch to name.
- Gate: extend `page-roundtrip`-style save gate: spawn `jujunud` with
  `["zombie"]`, save, load, assert the def name and effect list round-trip
  and the limb count matches. `--gate` only.

### Phase 6 — the player's corpse (sketch, blocked)

No corpse mob exists for anybody: `Mob::Die()` hands limbs to
`DebrisSystem::AdoptBody` (`mob.cpp:15117-15154`), the avatar is a `Mob`
outside `mobs_` (`avatar.h:120`) and deliberately gets no `CorpseReport`
(`mob.h:1397-1399`). Zombifying it needs: capture def name + per-limb lattices
inside `OnDying()` (the only moment the rig is whole, `:15023`),
`DefWithEffects(avatarDef, ["zombie"])`, spawn, overlay the captured lattices
as `LoadState` does (`:16076-16092`), release the debris bodies. Also
infection has no turning today (an infected human rots and dies). This is its
own plan; 5b is designed so it needs nothing new from the resolver.

## 3. What §L and §M become

- **§L stays.** It compares the generator at the default genome against the
  base file, and the base file is still the full contract. It shrinks as
  phases 1 and 3 remove keys the generator no longer emits, and it is the
  gate that catches "someone edited `human.json` and not the generator" for
  the numbers the generator still derives.
- **§M is re-aimed from "the copy is current" to "the thin file resolves to
  the body the genome describes".** After phase 3 the drift §M was written for
  cannot happen: there is no copy to go stale. What it guards instead is the
  resolver and the diff emitter agreeing, which is a different and smaller
  claim.
- Two gates are NEW and permanent: §N (resolver semantics, both languages)
  and §O (every `.vox` is in sync with its recipe).

## 4. Hazards carried from the research

- `"human"` is hardcoded in `avatar.h:504`, `tuning.h:96`, `support.cpp:35`,
  `main.cpp:3203/3714/6483/8353`, and ~15 test sites; `"zombie"` in six test
  sites. None needs to change; the names survive. `tuner_schema.js:67`'s
  player-model dropdown lists `mina/asha/wizard`, all deleted, and neither
  character — fix it while there.
- A def is all-or-nothing (`mob.cpp:1712`). A bad override drops the whole
  character with a log line; §N must assert the failure is loud.
- Authored anchors, socket offsets and natural edges are art-frame numbers of
  the BASE's boxes (`:1268-1308`, `:1398-1455`). A character that forgets to
  override them inherits the human's, silently. The phase 3 diff emitter
  always emits them, and §M's resolve-equals-fat check proves it.
- `flipbooks[].model` is a raw model INDEX (`:1629-1644`), the only
  index-keyed reference; unaffected because no character has flipbooks.
- Per-instance state that survives a hot reload must be re-applied in
  `SetDefs`/`SetBehaviors`; effects sidestep it by living in the def.

## 5. Verification budget, whole plan

| phase | cheap gate | exe runs |
|---|---|---|
| 1 | `test_mobgen.mjs` | 1 (`--gate mob`) |
| 2 | `test_mobgen.mjs` §N | 1 (`--gate sidecar-resolve`) |
| 3 | `test_mobgen.mjs` §L/§M, `check_characters.sh` | 0 |
| 4 | `test_anatomy.mjs`, §O | 1 (`--gate anatomy-parity`) |
| 5a | §N effect fixture | 1 (`--gate undead`) |
| 5b | — | 1 (save round-trip gate) |

No rebaseline anywhere: no phase reaches the determinism hash or moves a
threshold number. Full acceptance once, on the tree that ships phase 5a.

## 6. Coordination

`mob.cpp` is held by two live claims (corpse reactions, zombie bite limbs).
Phases 2, 4 and 5 add NEW TUs and touch `LoadMobDefs` at one call site each;
claim `src/game/sidecar.*`, `src/game/anatomy_resolve.*`, `assets/mobs/`,
`assets/anims/`, `assets/editor/mobgen.js`, `breed.js`, `editor.js`,
`scripts/test_mobgen.mjs`, `scripts/gen_mobs.mjs` on the board before starting,
and rebase the one-line `LoadMobDefs` edits after the two claims land. Phases
1 and 3 need no build and can run in a worktree against main's exe with
`SANDVOX_ASSET_DIR`. Update `ARCH_NODES` in `assets/tuner.html` and
DESIGN.md §"A creature is a variant of another creature" and §"Characters are
generated and bred" in the commit that lands each phase.


---

## 7. What landed, and what did not (2026-09-20)

### Landed

| phase | commit | gate evidence |
|---|---|---|
| 1 — shared clips to the library | `a9c17d0` | `test_mobgen.mjs` §A re-assembles the clip set from `assets/anims/` and still matches the Python reference; `--gate mob` clip library PASS (17 files, 0 not on the human) |
| 2 — named-array merge + clip retime | `a3f787a` | new `--gate sidecar-resolve` (11 sidecars, 0 failed, all six fixture rules); `test_mobgen.mjs` §N diffs the C++ dump against the JS resolution of the same files |
| 3 — the generator emits thin sidecars | `5d6be01` | `test_mobgen.mjs` §L unchanged, §M re-aimed; `--gate scale` still reports jujunud 1.675 m, newcomer 1.650 m |
| 4 — anatomy resolved at load | `4fabed2` | new `--gate anatomy-parity` (0 rewritten over baked art; 73,799 interior voxels rebuilt from bare surface, 0 disagreeing); `test_anatomy.mjs` §5 over every `.vox` |
| 5a — effects as files | `8401f0d` | `undead` gains arm F: `jujunud_zombie` composes 1, speed 23.50 against jujunud's own 31.50, chroma 60.6 → 25.6, lost 0.127 |

`jujunud.json`: 63,557 B → 4,414 B. `newcomer.json`: 63,589 → 3,780.
`zombie.json`: 8,683 → 625, plus a 9,701 B effect file that applies to anybody.
No `.vox` moved; no baseline number moved; nothing rebaselined.

### Two places the plan's premises did not survive contact

**§1.5's `garments: ["linen"]` is NOT inert, and was not dropped.** The research
was right that generated bodies have zero linen voxels (their shorts are paint)
and wrong that the clause therefore does nothing: `human.vox` has 1,112 linen
voxels, because the human's shorts are the MATERIAL — burning a voxel clears its
art colour, so painted clothing would char into bare flesh. The clause is what
puts skin rather than flesh under them. Dropping it from the shared recipe would
have broken the human to tidy up a no-op on everybody else. The real asymmetry
is in the generated bodies' ART and is now written down beside the clause in
`anatomy.js`.

**`--gate anatomy-parity`'s arm B may not strip the SURFACE.** The plan says
"strips one def's interior to skin in memory, resolves, and compares to disk".
Stripping everything to skin deletes the garments, so "skin under the shorts"
stops firing and 1,763 voxels disagree for a reason that has nothing to do with
either implementation. The arm strips depth >= 1 only — which is also what the
recipe says, since the surface layer is `keep` and the bake never writes it.

Two smaller deviations, both stated in their commits: §O went into
`test_anatomy.mjs` (which already had the `.vox` plumbing and the human arm)
rather than `test_mobgen.mjs`, and the `model`-ignored warning fires only on an
EXPLICIT `model` key, not on one implied by `extends` — otherwise every thin
character would warn about its own art.

### Not landed: phase 5b, and why

`MobSystem::DefWithEffects` cannot be built without the LOADER. §1.4 already
establishes why an effect cannot be a per-instance flag: `palette` is applied
before `MicroBodyMergeArt` dedupes by RGB, so a per-mob bit could not recolour
anything, and a field-by-field copy of an existing `MobDef` would give an
unrecoloured zombie. So a derived def has to be built the way every other def
is — which means `LoadMobDefs` has to be able to build ONE def from a virtual
source, which means extracting its ~1,080-line per-source loop body into a
function.

That refactor re-indents the largest function in the most contended file in the
repo. `src/game/mob.cpp` is held by two live claims (`agent-6b01a8`, ~11 hours
old at the time of writing; `agent-830b5e`), and §6 of this plan already says to
rebase the one-line `LoadMobDefs` edits after they land. A re-indent of a
thousand lines is not a rebase.

There is a second, independent blocker: even with the extraction, synthesising a
def mid-session means rebuilding the shared micro-body pool and re-uploading it,
which is `main.cpp`'s job. `MobSystem` cannot trigger that itself.

The per-mob `effects` half of the save was deliberately NOT landed on its own.
Without `DefWithEffects` there is no producer for the field, and a save format
that carries a value nothing can set is the "unasked mechanic to keep a gate
green" this repo has a rule about.

Nothing in phases 1-5a needs changing for 5b: the resolver already accepts an
accumulated `effects` list, `MobSystem::LoadState` already resolves a def BY
NAME (so a derived def round-trips by name with no format change at all), and
`assets/mobs/jujunud_zombie.json` is the file form of exactly what
`DefWithEffects` would synthesise. What is left is the extraction and the
reload path.

Phase 6 (the player's corpse) is unchanged: the plan already declares it a
sketch and its own plan.
