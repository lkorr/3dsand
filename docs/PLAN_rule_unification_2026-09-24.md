# PLAN: one rule, one place (rule-unification program, 2026-09-24)

**Status (2026-10-02 audit): COMPLETE, 2026-09-24.** Wave 1 (W1-A `f9a32a9`,
W1-B1 `c2b93d4`, W1-B2 `99f9d95`, W1-D `bb25b61`, W1-E `2bdaaea`, W1-F
`7c5df1b`) and every wave-2 package (W2-G `2a4b68f`, W2-H `c881f05`, W2-I
`c6d53a3`, W2-J1 `8dda85c`, W2-J2 `65804e3`, W2-K `874e9c2`, W2-L `bc76c8c`,
W2-M `127396b`, W2-N `3c56f7c`, W2-O `7fc6887`, W2-Q `3941ac0`, W2-R `930ef03`)
are on main; hash pinned `ec0b915`. W2-P was DROPPED by owner decision (see
W2-Q). The "Wave 2 (remaining)" list at the end is the launch list, kept as
history.

Orchestrated from the main checkout; each package is one Opus worktree agent.
Source: the six-domain audit of 2026-09-24 (duplicated rules / parallel entity
kinds). The corpse refactor (`docs/PLAN_corpse_is_a_mob.md`, branch
`corpse-is-a-mob`) is a SEPARATE program running concurrently and is out of scope
here; wave 2 waits for it to land because it rewrites most of `mob.cpp`.

Common rules for every package:
- Read CLAUDE.md first. Rules 1-3 are inviolable. The hash WILL move; do NOT
  rebaseline — the orchestrator pins once at integration. The twice-run
  determinism comparison must still pass.
- Build ONCE in your worktree (`bash scripts/build.sh`); every exe run goes
  through `bash scripts/run.sh`. `export SANDVOX_NO_CRASH_DIALOG=1`.
- Verify with ONE `--verify <gates>` boot at the end: your new gate(s) plus the
  2-3 gates covering the path you touched. No `--suite acceptance`, no bare
  `--selftest`, no `--vk-smoke*` unless you changed a render path.
- Static checks are free: `bash scripts/check_shaders.sh`,
  `python scripts/check_invariants.py`, `python scripts/check_pass_table.py`.
- A `common.wgsl` edit costs ~9 min of shader compile. Only the package that
  owns it this wave may touch it; batch your edits to it.
- Behaviour-preserving unless the package says the behaviour change is the point.
  If a requested change turns a gate red, report it; do NOT invent compensating
  mechanics.
- Commit on your worktree branch (stage by path). Do not touch the main checkout.
- Update `DESIGN.md` where contradicted and `ARCH_NODES` in `assets/tuner.html`
  if you changed a system's shape.
- Report: commits, what changed, gates run + results (compare against the
  known-red list in memory/tests/BASELINE.md before calling a red yours), any
  behaviour changes, anything left undone.

## Wave 1 (parallel, now; avoids the corpse refactor's surface)

### W1-A tuning reach
1. `render.waveDispersion, waveSteepness, waveShoreDepth, waveFlowScale,
   waveFoamThreshold, waveFoamGain, waveImpactSpeed, waveImpactDecay,
   waveImpactLen` have `.def` rows, `tuning.h` fields, `tuning.json` values and
   tuner rows but NO read in `LoadTuning` (`src/sim/tuning.cpp`) since 9a79eba —
   the sliders are dead. Add the reads + clamps.
2. Delete the orphan `render.heatSpillStrength` from `tuning.json` (c80850a
   removed its code).
3. `.def` default vs `tuning.h` default disagree: `render.fluidFoam`
   (0.55 vs 0.35), `sim.fluidExciteMode` (1 vs 0). Make them agree on the value
   the engine actually runs with (tuning.json's), and explain in the report.
4. `SetSimField` (`tuning.cpp:~227`) is dead — only `SetTuningField` is called.
   Delete it.
5. Add to `check_invariants.py`: every `.def` row's `group.member` is read in
   `tuning.cpp` (quoted key present), and the `.def` default equals the
   `tuning.h` initializer. Make it fail on the tree BEFORE your fix (prove it
   would have caught this), pass after.
6. Scan for any other `.def`/`tuning.json`/`tuner_schema.js` rows with no reader
   and report (fix obvious ones the same way).
Files: `src/sim/tuning.cpp`, `tuning.h`, `tuning_params.def`,
`assets/materials/tuning.json`, `scripts/check_invariants.py`. Do NOT add/remove
`.def` rows beyond the above (a `.def` row change is a prelude-wide cache miss).
Verify: `--sweep render.waveSteepness=0,1` gives different render output is not
hash-visible — instead prove reach with a unit assert or `--shot` diff; the
cheapest honest proof is fine, say which.

### W1-B1 matter rules (owns `common.wgsl` this wave)
1. **Liquid staining in one function.** CA `doStaining` (`sim_step.wgsl:~1356`)
   and MPM `stainApplyCell` (`sim_fluid_seam.wgsl:~1556`) drifted twice
   (13b2c51 → 1dfe699) and still differ: per-material `matStainChance` vs global
   `TUNE_FLUID_STAIN_RATE`; `consume` CA-only; absorption spends liquid CA-only;
   clean absorbent cell starts at 1 vs ceiling; same-type non-absorbent add
   `+addAmt` vs `+1`. Put the DECISION in `common.wgsl` as one function (e.g.
   `stainStep(liquidMat, carriedAmount, nbrWord) -> {newWord, spend, consume}`),
   CA semantics are the truth; both kernels only perform the write/spend. MPM
   wetting an absorbent cell must spend particle mass the same way the CA spends
   fullness (mass conservation — see memory gotcha-water-audit-must-count-mpm-and-wind).
   If `TUNE_FLUID_STAIN_RATE` becomes unused, leave the `.def` row (W1-A owns
   tuning rows) and note it.
2. **Excited (MPM) fluid runs its OWN rules.** Today particles are only the
   neighbour side of a reaction (`fluidOccMat`, `sim_step.wgsl:~1216-1250`).
   Every acid rule has acid as `self`, and `seamLiquid` admits acid
   (moveEvery defaults to 1), so splashing acid corrodes nothing; water as
   particles never becomes steam beside `tag:hot`. Make a particle's material
   evaluate its self/pair rules against the cells around it, consuming particle
   mass via the existing flag-and-consume path (`consumeApply`), deterministic,
   write reach ≤1, subcritical. Pick the least invasive correct design and
   justify it. New gate `fluid-self-react`: excited acid next to stone/organic
   corrodes it; excited water beside lava yields steam. (The existing
   `fluid-react` gate covers only the neighbour side.)
3. **One reaction-condition gate.** Rain/light conditions are evaluated in
   `sim_step.wgsl:~967-997` (`rainChance`/`lightMatches`), in
   `src/sim/reactcpu.h` + `RainScaledChance` (`materials.h:~543`), and retyped
   inline in `sim_gas.wgsl:~552-580` (`gasDecayProduct`). Make one WGSL
   `reactGate(...)` in `common.wgsl` used by sim_step and sim_gas, and add a
   `check_invariants.py` check that the CPU `reactcpu.h` arithmetic matches it
   (same constants/formula).
4. **Far plume "is on fire" from the reaction table.** `FarPlumes::HotEmissive`
   (`src/sim/farplumes.cpp:~87-103`) treats any non-gas `tag:hot` emissive as
   smoking, so lava lakes plume at distance but not up close. Derive it from
   the compiled reactions: the material has a decay/emit rule whose product is
   fire or smoke (or is itself fire). Near and far must agree.
Files: `common.wgsl`, `sim_step.wgsl`, `sim_fluid_seam.wgsl`, `sim_fluid.wgsl`
(only if needed for 2), `sim_gas.wgsl`, `reactcpu.h`, `materials.*`,
`farplumes.cpp`, `check_invariants.py`, selftest for the new gate. Do NOT touch
the `species` code or `raymarch.wgsl`/`debris.wgsl` (W1-B2 owns those).
Verify gates: fluid-self-react (new), fluid-react, the stain gates, rain-fire,
the far-plume/far-fire gates, determinism.

### W1-B2 fluid identity + liquid optics (do NOT edit `common.wgsl`)
1. **Delete "species".** Excited fluid has a second identity
   `(mat-1)&3` (`sim_fluid_seam.wgsl:~1337`, `sim_fluid.wgsl:~767`,
   `container.cpp:~466/520/621`) that picks render colour
   (`raymarch.wgsl:~8928` via `TUNE_FLUID_COLOR0..3`, `debris.wgsl:~449`) and the
   attraction terms (`fluidAttractSame/Diff`, default 0). Acid and water are both
   species 0 → splashing acid draws water-blue; the seam blend adds every settled
   liquid (oil, lava) to species 0 (`raymarch.wgsl:~8913`). `fluidSplashMat` in
   `TickParams` is a third copy. The particle already carries its material in
   `attr`: colour from `materials[mat]`, same/diff attraction = same material id
   or not, density from `materials[].density` (MPM currently uses a uniform
   `TUNE_FLUID_REST_DENSITY`; the CA and ballistic particles layer by density).
   Excited WATER must look the same as today — if the material colour differs
   from the tuned fluid colour, report it with before/after `--shot`s rather
   than silently editing `materials.json`. Retire the now-dead tuning rows in
   `tuning_params.def`/`tuning.h`/`tuning.json`/`tuner_schema.js` and any
   `LoadTuning` reads (coordinate: W1-A edits other rows of the same files —
   keep your hunks to the species rows).
2. **One liquid optics model.** `isWater = tagMask != 0 && opacity < 0.45` is
   still live at `raymarch.wgsl:~7448` (`shadeWater`) and `~9698`
   (`shadeMpmFluid`) though the comment at `~7042` declares that test wrong and
   replaced it with an opacity-driven `SubProfile` only in the underwater path.
   The MPM shade has its own absorption model (the "blue rectangle", 7aee945,
   2912190). Make one `liquidOptics(mat) -> {absorbK, scatter, fresnelScale, ...}`
   from `SubProfile`, used by all three paths; declare it in `raymarch.wgsl`
   (not common). Watch the raymarch register budget (memory:
   gotcha-raymarch-fs-has-no-register-headroom / register-cliff) — check
   `--shader-stats` before/after.
Verify: `--verify` with the fluid gates + `--shot-frames` of a water scene and
an acid splash, before/after; `--shader-stats` delta in the report.

### W1-D material names in saves
Material id = position in `materials.json`. Saves store raw ids
(`src/game/persist.cpp:~196-199` admits a renumber repaints the world).
1. Write a material-name table in the save header (chunk snapshots + delta
   saves + anything else persisting raw material ids — entity/mob records with
   voxel lattices, debris lattices, worn items). Remap on load when the table
   differs from the running materials; old saves without the table load as
   identity (and say so in a log line).
2. Resolve `world.h`'s `kMat*` literals against names once at material load
   and fail loudly on mismatch (check_material_ids already pins them
   statically; this makes the runtime refuse too). Do NOT turn them into
   runtime variables (they feed the WGSL prelude).
New gate `save-material-remap`: save, reorder/insert a material in a test
material set, load, world hash of the region matches by NAME. Files:
`persist.cpp`, `chunkstore.*`, save/load plumbing, `materials.*`,
`src/test/selftest_worldio.cpp`. Verify with the save gates.

### W1-E generator parity runs itself
1. `scripts/test_mobgen.mjs`, `test_anatomy.mjs`, `test_environment.mjs` are run
   by nothing automatic. Run them from `scripts/post_edit_check.sh` when an
   `assets/editor/*.js`, `assets/mobs/*`, `assets/biomes/*` or `assets/trees/*`
   file changes, and add a selftest gate (or a `--verify`-reachable check) that
   shells out to node if present and SKIPS (not fails) if node is absent.
2. `scripts/gen_human.py` no longer reproduces `human.vox`; `test_mobgen.mjs`
   §A still pins mobgen to it (that pairing froze a stale rig once). `human.json`
   is the truth: delete `gen_human.py` and §A, and any other `gen_*.py` that no
   longer reproduces its asset (check; list what you deleted).
3. `CLAUDE.md` says `assets/water/` presets are "not yet read by worldgen" —
   stale (`biomes.cpp:~487`, `worldmap.cpp:~899`). Leave CLAUDE.md alone (the
   orchestrator edits it) but report the exact line.
Note: another session has UNCOMMITTED `mobgen.js`/`test_mobgen.mjs` edits in the
main checkout; keep your edits to those files minimal and surgical.

### W1-F quick mob fixes (small, surgical `mob.cpp` edits only)
The corpse refactor rewrites most of `mob.cpp` concurrently: keep every hunk
local, no drive-by cleanup, no renames.
1. **Multiplayer explosions.** `session.cpp:~3386-3418` applies each session's
   OWN explosions to `mobs` + that session's own `avatar`;
   `MobSystem::CarveMobsRadial`/`BlastMobsRadial` (`mob.cpp:~17660, ~7961`)
   loop `mobs_` only. So player A's grenade never carves or launches player B.
   First find how cross-player damage works today for melee (authority: who
   applies damage to a remote player's avatar — the owner machine or the
   author?) and make explosions follow the SAME authority rule. The fix is
   that the radial `MobSystem` entry points cover `avatars_` where authority
   allows and the per-avatar special calls in session.cpp go away. Include
   `AppendLiveLimbBodies` (the impulse skip list). New/extended gate in
   `selftest_net.cpp` (two-player harness exists): A's grenade carves and
   launches B.
2. **"Burnt" is data.** `Mob::BurnStageOfMaterialName` (`mob.cpp:~8343-8357`) is
   a 12-name `if` list driving the burn hp cap, `burntAway`, the wet guard,
   `burnable_` and the HUD (`main.cpp:~1047-1060` `ResolveBurnMats`). Add an
   authored `burnStage` (0 none / 1 seared / 2 cooked / 3 charred — match the
   existing enum) to `materials.json`, compiled into the CPU material table;
   delete the name list; the HUD reads the same field. Behaviour identical
   (same 12 materials get the same stages) — prove with the burn gates.
3. **Charred limbs sleep.** `matSelfActive_` (`mob.cpp:~2062-2072`) counts
   neighbour-gated rules as self-active; debris splits those out as
   `matSelfScaled_` (`debris.cpp:~436-446`, memory
   gotcha-light-gated-rules-never-sleep). `flesh_charred`/`flesh_cooked` have only
   gated rules, so a charred live limb stays `alight` and never engages the
   burn `sleepKey` (469abb3). Apply the same split on the mob side. Confirm the
   hypothesis with a measurement first (e.g. a burnt limb's front count / sleep
   state after the fire is out) and add it as an assertion to the mob-burn
   gate.
Files: `session.cpp`, `mob.cpp`/`mob.h` (surgical), `materials.*`,
`materials.json`, `main.cpp` (HUD hunk only), `selftest_net.cpp`, burn gates.

## Wave 2a (launched 2026-09-24 after corpse-is-a-mob landed, main 3af438e)

Common rules above still apply. Every package touches mob.cpp regions another
wave-2 package also touches: keep hunks local, no drive-by renames, and do not
reformat. The corpse refactor is now ON MAIN (a corpse is a dead Mob;
severed parts are dead-flesh debris, passes `BurnDeadFlesh`/`FleshView`):
read DESIGN.md "Corpses are Mobs" first.

### W2-G explicit damage cause
Six ambient flags (`inBurnFlush_`, `inSpawnRot_`, `inBladeCut_`,
`inBluntCarve_`, `inUnarmedBlunt_`, `inBite_`; 40 refs in mob.cpp, 11 in mob.h)
plus `BladeCutScope` are set by scope guards around damage entry points and
read by `Damage`/`HpZeroSevers`/`CarveLimb`/`Sever` in combinations
(`inBurnFlush_ && !inBluntCarve_`, `!inBluntCarve_ && !inSpawnRot_ && ...`).
Every new cause re-patched every sever rule: b4daa6e, fd90f11, 904dd30,
719df2d, 9d1701d. Debris already has `DamageCause { Other, Blade, Blunt, Bite,
Beam, Blast }` (`debris.h:~330`).
1. Promote one cause enum to a shared header (extend it with the causes the
   flags encode: Burn, Rot/Spawn, Unarmed-blunt, Fall, ...; keep debris using
   the same enum).
2. Pass the cause EXPLICITLY through Damage/CarveLimb/Sever/HpZeroSevers (a
   small `DamageCtx`/`CarveCause` struct if more than the enum is needed:
   bleeds, mayDetach, vitalDetaches, impactSevers, bloodScale, deathCause).
   Delete the six flags and the scope guards.
3. The sever/detach/bleed policy becomes ONE table keyed by (cause, tissue
   class), in code or data — your call, justify it. Every current behaviour is
   preserved exactly (enumerate the current combinations before you change
   anything and make each a table row; a gate or unit assert that walks the
   table vs the old predicates is the proof).
4. `undead` currently has a third consequence hidden at a flag site (blunt
   dismembers the undead, mob.cpp ~8909, contradicting mob.h's "two
   consequences" contract): move it to def data (e.g. a tissue/def field the
   zombie effect overlay sets), not an `undead` check.
5. The audio `MobSystem::bladeCut_` flag: fold into the cause if it means the
   same thing, else leave and document.
Behaviour-preserving. Verify: the wound/impact/combat gates that cover
severing (grep selftest_wound.cpp/selftest_impact.cpp for sever/decap/blunt/
bite/laser/burn), mob-burn, corpse gates, determinism (twice-run).

### W2-I one body-reaction evaluator + limb sleep
`MobSystem::BurnOneLimb` (mob.cpp ~12680) evaluates reactions.json over a live
limb / dead-Mob limb / dead-flesh part (`BurnDeadFlesh`). `DebrisSystem::
BurnBodies` (debris.cpp ~2918) is a SECOND CPU copy for non-flesh debris
(logs, dropped items, shells, cloth), with its own derived flag tables
(`debris.h ~1741` "twins of mob.h's"); `BurnFleshBodies` (debris.cpp ~2804)
may be a third. Fixes were ported by hand (3760ba5 -> ab2250f). BurnBodies
lacks coats, wet-doesn't-burn, wound decay slowdown, joint heat.
1. Make ONE population-neutral evaluator over a body-lattice view (the
   BurnLimbView/FleshView shape), used by live limbs, dead Mobs, dead-flesh
   parts and non-flesh debris. Delete the debris copy. Debris keeps only its
   scheduling (cursor scan budget, BurnTail) and population-specific inputs
   supplied through the view (no coat ledger -> no coat, etc.).
2. ONE derived material-flags builder (self-active ungated vs neighbour-gated
   `matSelfScaled_`), used by both systems.
3. Limb sleep (found by W1-F): unburnt limbs on a live creature never reach
   burn sleep — their burn index stays held and the idle-tick counter keeps
   resetting. Find why (attribute, don't eliminate: add the reason to the
   reporter first, CLAUDE.md rule 6), fix, and make mob-burn subtest K's sleep
   assertion falsifiable (it is currently relative to limbs that also never
   sleep). Note corpse P1 added `BodyBurnState::idle` / idle verdict for the
   dead — reuse it for the living if it is the right mechanism.
Behaviour change expected (debris now gets the full rule set: an oiled/wet
plank burns like an oiled/wet limb). Verify: mob-burn, corpse-burn, the debris
burn / tree-fire / forest-fire gates, corpse-sleep, a perf number for the
forest-fire harness before/after (`--perf forestfire` under
SANDVOX_RUN_EXCLUSIVE=1, one run each side) since this is a hot path.

### W2-J1 grid stains take part in reactions (owns common.wgsl this wave)
Bodies treat a coat as matter that reacts (mob.cpp BurnOneLimb: hot coat,
wet coat blocks hot products + douses, fuel coat flashes, corrosive coat
eats). The grid ignores its own stains in reactions (`sim_step.wgsl` touches
stain only in `doStaining`): wet ground burns like dry, oiled ground is not
flammable.
1. Read the body coat semantics and write them down ONCE as a rule in
   DESIGN.md ("a coat is a co-located virtual neighbour: the voxel's pair rules
   see the coat material as a neighbour, and the coat material's pair rules see
   the voxel; a rule fired through the coat spends coat amount instead of
   rewriting the voxel, unless ..."). This spec is what W2-J2 will make the body
   evaluator match, so make it precise, data-driven (no material names), and
   expressible from reactions.json + the stain block.
2. Implement it in the CA (`matOfStainType` gives the stain's material; bodyOnly
   stains have no ground type bits and stay out). Deterministic, write reach
   ≤1, subcritical: a burning oil film must be bounded by its stain amount.
   Mind the stain palette is FULL (memory project-lava-oil-body-coats).
3. Gate `stain-react`: oil-stained ground next to fire ignites and burns out
   (bounded); wet (water-stained) flammable ground resists ignition relative to
   dry; a clean control is unchanged. Check `rain-fire` and `blood-stain` still
   pass.
Behaviour change is the point. Watch rule 2: stains are everywhere after rain;
a rule gated on a stain must not keep chunks awake (memory
gotcha-light-gated-rules-never-sleep).

W2-J1 DONE (8dda85c): the coat rule is DESIGN.md §6 "A coat is a co-located
virtual neighbour"; the same section lists BurnOneLimb's six divergences = W2-J2's
to-do. Found: a cell with a live tick stamp that then sits still is skipped 1
tick in 7, which can drop the only keep-awake mark in its chunk and let the chunk
sleep with work left (rule-2 correctness) — fix in W2-R.

W2-G DONE (2a4b68f): `src/phys/damagecause.h` (DamageCause + DamageCtx
{cause, eaten, severity}), `src/game/severpolicy.h` kCauseRows (cause, eaten,
tissue) table, gate damage-cause. Rotten tissue = def data `bodyTissue`.

## Wave 2b

### W2-H one damage event, one shell response
Base: branch `rule-unif-w2` (has W2-G's DamageCause/DamageCtx — build on them,
do not re-invent). Today only melee has a unified model (StrikeProfile, one
sweep); every other source rolls its own hp/armour/knockback.
1. **ShellResponse.** Three copies of the worn-shell hardness read with three
   curves and three tuning pairs: `CutLimb` (cutHardnessRef/Min), `BluntHit`
   (bluntHardnessRef/Min), `BiteHit` (biteThroughSoft/Hard) — all read
   `skinVoxels[0]` instead of the struck voxel. Make ONE
   `ShellResponse(shellMaterial, DamageCtx/cause) -> {absorbed, passed}` with
   the per-cause parameters in one table (keep today's numbers per cause so
   melee is unchanged; reading the STRUCK voxel instead of skinVoxels[0] is an
   intended fix — report its effect).
2. **Every source goes through it.** Explosions (`CarveRadialAll`, carve hp =
   volume x hard-coded `kCarveDamagePerVolume` 1.5 in mob.h — move to tuning),
   the laser (`session.cpp`, flat `tools.laserDamage`; also drop its redundant
   `avatar.Damage` + `mobs.Damage` double call), fall damage, spell overcast.
   An iron cuirass must now resist a grenade the way stone does in
   `sim_explode.wgsl` (hardness along the ray); body blast damage scales with
   blast POWER like the terrain model, not radius alone.
3. **Fall damage is the body's.** `PlayerAvatar::ApplyFallDamage` (avatar.cpp)
   bills landings only for the player though `Mob` measures `ragdollImpact_`
   for every creature. Move it to Mob so NPCs take it (blasted NPCs landing
   hard get hurt); constants (`4.0f + impactMs*0.1f`, 400 droplets, 30 blood
   voxels, 0.3 leg bleed) to tuning; `name.find("leg")` becomes limb role data;
   its `ApplyRadialImpulse` gets the living-limb skip list like explosions.
   `PlayerAvatar::SpendHealth`'s hp-zero "sever any limb" contradicts the
   hp-kills-in-place policy — route it through Damage with the Fall/Other cause
   and W2-G's table. `SelfDestruct` severs instead of carving "because there is
   no CarveLimb" — use `CarveRadial`.
4. **One knockback path**: blast reach has three knobs
   (`explosionBodyDamageScale`, `explosionImpulseRadiusScale`,
   `ragdoll.blastRadiusScale`) + two impulse scales. Consolidate to one reach
   and one impulse definition consumed by carve, debris impulse and rig launch
   (keep current effective values).
5. **(phase 2, only if 1-4 are clean)** Contact damage: the Jolt contact
   listener feeds only audio (debris.cpp) and vessel breaks (container.cpp).
   Thrown rocks, falling trees and flung debris never hurt a creature. Add a
   bounded, tunable contact-damage path producing a Blunt DamageEvent from
   relative impulse above a threshold, through ShellResponse. Deterministic
   (contact events in a sorted order). Gate it.
Also: `kCoverReach = 6.0f` declared twice in melee.cpp — one constant.
Gates: a new `damage-sources` gate (grenade vs armoured vs unarmoured
creature; NPC fall damage; laser once not twice), plus the melee/impact/
armour/wound/blast gates vs a control arm on main's exe.

### W2-M one item instance, one kit
1. `ItemInstance{name, count, dye, fill, damage}` embedded by every item
   record: `ItemStack` (item.h, no damage), `CarriedItem` (mob.h, no fill),
   `WorldItem` (worlditems.h, damage always 0), `WireGear` (mobsync.h),
   `ItemGrant` + BodyAnnounce item fields (debrissync.h, no fill),
   `PlayerKit.wornDamage` (equipment.h, keyed by NAME so two tunics share one
   damage record). Fixes: flask contents lost on network pickup and when a
   corpse's pack rises; item damage threaded through three structs and never
   written.
2. **One kit per creature.** The player's gear lives twice: `PlayerKit` on the
   session and `Mob::worn_/heldItem_` on the rig, reconciled every tick in
   session.cpp (wearTried/wearDye latches, CaptureWorn copy-back). NPCs use
   `carried_` + worn_. Make one `Kit` on Mob (bag/equipment/instances); the
   rig's worn/held slots are DERIVED from it and hold no separate truth; the
   session reconcile loop and `avatarKitFn_` go away if they can.
3. Saves (players/*.svp, MOBS records) and net records carry ItemInstance;
   bump versions minimally; old saves load.
Gates: extend the equipment/inventory/save/net gates: two identical tunics
keep separate damage; a flask keeps its fill through drop -> network pickup ->
save -> load -> corpse rise; loot panel still works on a dead Mob.

W2-I DONE (c6d53a3): every body burns through MobSystem::BurnOneLimb
(debris via SetBodyReactor -> BurnLooseBody); src/sim/bodyreact.h
BuildBodyReactFlags; living limbs sleep (wet/stain passes held the index).
W2-R DONE (930ef03): substep-0 probe keeps a stamp-aliased matched cell's
chunk awake; gate stamp-sleep; CA +2.5%.

### W2-J2 body coats follow the coat rule
Base `rule-unif-w2` (has W2-J1's rule text in DESIGN.md §6 "A coat is a
co-located virtual neighbour" and W2-I's unified evaluator). `BurnOneLimb`'s
four hand-written coat sections (hot / wet / fuel / corrosive, each with its
own RNG index space +96/+128/+160) differ from the rule in six ways, listed in
that DESIGN section: wet boils at `coat.fireDrySeconds` not water's rule
chance; a douse costs 4 levels not 1; wet blocks only hot/burn-stage products
instead of covering; an oil flash spends the whole coat and jumps the voxel to
its burning form; a hot coat counts as every open face widened to world pitch
instead of one partner; an acid bite charges a depth-based layer price and
carries the coat inward.
1. Replace the four sections with ONE implementation of the §6 rule over the
   body lattice, shared in spirit (and in code where possible, e.g. a pure
   C++ `CoatReact` whose GPU twin is W2-J1's `coatReact` — add a
   check_invariants parity check like `reactgate`).
2. Where a current body behaviour is load-bearing and the rule can't express
   it (e.g. acid depth pricing, world-pitch widening for fine-lattice bodies —
   a body voxel is smaller than a world voxel), EXTEND the rule in DESIGN.md
   §6 with a data-driven, population-neutral clause that the grid satisfies
   trivially (pitch = 1), rather than keeping a body-only special case.
   Justify each clause.
3. One stain-precedence function: ground (`stainStep`, sim_step/common.wgsl)
   and body (`bodystain.cpp` RaiseBodyStain/AddBodyStain/CoatBeneath) differ
   (foreign coat overwritten by any larger amount on bodies; ground never
   paints over a foreign stain). Make one precedence rule (C++ + WGSL twins +
   parity check); document where body vs ground legitimately differ (material
   id vs palette slot).
4. (report only) `BurnLimbView::Set` zeroes the art colour on every reaction,
   so cloth/linen/undercloth each needed a cloned burn chain. Make reaction
   products KEEP the voxel's art/tint slot (like the grid's MATF_TINTED) if it
   is a contained change; then LIST which cloned material chains could now
   collapse — do NOT delete materials (content; the owner decides; deleting a
   material also turns saved instances into air under W1-D's remap).
Gates: acid-coat, lava-oil-coat, rain-oil, corpse-acid, mob-burn, debris-coat,
stain-react, blood-stain, rain-fire, a new `coat-parity` fixture that runs the
same coat scenario on a grid cell and a body voxel (coarse pitch) and asserts
the same outcome class. Behaviour changes are expected; report them with
numbers, do not retune to compensate.

### W2-N collision layer derived from role
A body's Jolt layer (MOVING / AVATAR / PROP / THROWN) is set imperatively at
~24 sites (`SetBodyAvatarLayer` x6, `SetBodyPropLayer` x4,
`ReleaseToWorldWhenClear` x9, `DisableCollisionsAmong` x6; mob.cpp, grab.h,
session.cpp, physics.*). Memory project-held-weapon-prop-layer: "the flag is
not set once, and every miss is a separate bug" (d960a54; also 4689b1 thrown
flask). AVATAR exempts a body from EVERY player's capsule, not just its owner's
(physics.cpp ~62-70) — wrong with two players.
1. A body role enum (rig limb live / rig limb dead / held prop / worn shell /
   loose debris / thrown / clearing-release, owner id) stored per body; ONE
   `ResolveLayer(role, owner)` computes the layer + pair filter; every site
   sets the ROLE, never the layer.
2. Owner-scoped exemption: a body exempt from its owner's capsule only (Jolt
   group filter / object-layer pair filter with owner ids). Gate with two
   avatars: A's limbs do not shove A, DO collide with B.
3. `ReleaseToWorldWhenClear` becomes a role transition with the clearing
   logic in one place.
Gates: vessel-break, grab/held-prop gates, remote-ghost, two-players, ragdoll,
player-corpse, a new `layer-roles` gate.

### W2-Q tuning generated from the .def table
Base: main (e312b51 or newer). Owner decisions 2026-09-24: W2-P (severed-limb
flesh part) DROPPED; the nine dead render knobs are DELETED here.
1. Delete `render.skyGradient, skyHorizonOffset, sunDiscPower, sunHaloPower,
   sunHaloGain, emberBrightness, emberRise, emberRate, emberDensity` from every
   place (confirm first that nothing in C++/WGSL reads them).
2. A new knob today needs up to 8 hand edits, 2 generated. Widen
   `tuning_params.def` rows to carry (group, member, WGSL name or none, type,
   default, min, max) and GENERATE from them: the `tuning.h` default
   initializers (no second copy of the default), the plain `LoadTuning`
   read+clamp for rows with no custom logic, and the tuner schema's
   `k/min/max` (descriptions/units/steps stay hand-written in
   `tuner_schema.js`, joined by key; one min/max truth — today e.g.
   `windDragRef` is max 120 in the schema and 200 in the clamp: pick the clamp's
   and report every disagreement you resolved). Hand code stays only for real
   logic (enum gates, derived values) and is marked as such.
3. Rows that are not WGSL-visible (most `player.*`, `gore.*`, ...) must not end
   up in the shader prelude — check how `gen_tuning_prelude.py` /
   `tuning_prelude.py` decide, keep that behaviour. No trailing comments on
   `.def` rows (the parser chokes). A `.def` change is a prelude-wide shader
   cache miss — expect one ~9 min compile, once.
4. Delete the redundant `scripts/tuning_prelude.py` table if it duplicates the
   `.def` (W1-B1 found the same row in both).
5. W1-A's `check_invariants.py` "tuning reach" + the `tuning-reach` gate must
   stay green and should get SIMPLER (most of what they check becomes true by
   construction).
Concurrency: W2-H/W2-M may add knobs on `rule-unif-w2` in the old hand style;
that is fine — the orchestrator converts them at integration. Keep the
generated and hand-written styles able to coexist.
Gates: tuning-reach, combat-tuning, determinism (must NOT move: shipped
values are unchanged), a `--sweep` on one converted `sim.*` row and one
`render.*` row proving reach.

W2-H DONE (c881f05) — owner chose to keep ALL behaviour changes (armour vs
blasts/laser/falls, power-scaled blasts, NPC fall damage, contact damage).
W2-N DONE (3c56f7c) Physics::SetBodyRole/ResolveLayer, owner-scoped OWNED(P).
W2-M DONE (127396b) ItemInstance everywhere, player Kit on the avatar; NPC
rig-from-kit DEFERRED to W2-K.

### W2-K one creature list
Base `rule-unif-w2`. `MobSystem` keeps players in `avatars_` apart from
`mobs_`; ~99 loops walk `mobs_`, ~8 also walk `avatars_`, ~14 by-id lookups
repeat `if (!mob) mob = AvatarById(id)`. Every world effect that forgot the
avatar list is a player-only (or NPC-only) bug (W1-F's explosions were one).
1. ONE way to reach "every creature": a controller attribute on Mob (Ai /
   LocalPlayer / RemoteGhost) and one iteration + lookup API
   (`ForEachCreature`, `FindCreature(id)`) — collapsing the storage into one
   container is preferred if `PlayerAvatar : Mob` slicing allows it; if not,
   keep two containers behind the one API and say why.
2. Audit EVERY `mobs_`/`avatars_` loop and by-id lookup: world/matter effects
   (burn, stain, wet, rain, blast, carve, contact damage, crowd spacing,
   BlockedByMob, splatter...) reach all creatures; agency passes (AI,
   steering, target selection) are gated by the controller, not by which list
   the creature is in. Produce the audit table (site -> all / controller-gated
   / list-specific-with-reason) in the report.
3. Contact damage reaches the player: W2-H's `ApplyContactDamage` never sees
   player limbs because the contact listener filters the player/owned layers
   (physics.cpp). Report player limbs to the damage path for contacts with
   loose bodies WITHOUT changing collision response (W2-N's role/layer
   table). [pending owner OK — do it; the orchestrator will revert if the
   owner declines]
4. (from W2-M) NPC worn/held gear derived from its Kit like the player's
   (`Mob::DressFromKit`), so loot, corpse gear capture, rising, handoff and
   load read the kit, not the rig. Keep save/net formats (W2-M already bumped
   them to ItemInstance).
Gates: two-players, remote-ghost, blast-players, player-corpse, mob-loot,
loot, kit-instance, npc-block, damage-sources (+ a player contact-damage arm),
mob, and a new `creature-reach` gate that applies each world effect once and
asserts it reached an NPC AND the local avatar.

## Wave 2 (remaining; launched as dependencies land) — ALL LANDED 2026-09-24 except W2-P (dropped); see the status line at the top
- W2-R stamp-skip sleep hole (above): make "matched but not fired" keep its
  chunk awake regardless of the substep stamp gate; gate that reproduces the
  stranded-cell case.
- W2-G explicit damage cause: replace `inBurnFlush_/inSpawnRot_/inBladeCut_/
  inBluntCarve_/inUnarmedBlunt_/inBite_` + `BladeCutScope` with one cause value
  (promote debris's `DamageCause`) passed through Damage/CarveLimb/Sever; sever
  policy = one table keyed (cause, tissue); undead blunt-dismember becomes def
  data.
- W2-H one damage event: `DamageEvent` + one `ShellResponse(material, cause)`
  used by cut/blunt/bite/blast/laser/fall; explosions scale body damage by power
  and respect shells; fall damage moves to `Mob` (NPCs take it); one knockback
  path with the living-limb skip list; contact damage from thrown/falling
  bodies.
- W2-I one body-reaction evaluator: `BurnOneLimb` becomes population-neutral
  over a body-lattice view; debris's `BurnBodies` copy deleted; one derived
  material-flags builder. ALSO (found by W1-F, 2026-09-24): unburnt limbs on a
  live creature never reach burn sleep either — their burn index stays held and
  the idle-tick counter keeps resetting (cause not found). Rule 2 violation;
  find and fix, and make mob-burn subtest K's sleep assertion falsifiable.
- W2-J coats are neighbours: coat = co-located virtual neighbour, rules both
  directions, one spend hook; the grid honours stains the same way (oiled
  ground flammable, wet ground damped); one stain-precedence function shared by
  ground and body; reaction products keep the art/tint slot (collapse cloned
  burn chains).
- W2-K one creature list: avatars in `mobs_` with a controller attribute
  (AI / local / remote).
- W2-L one pose pipeline: `Mob::PosePipeline(inputs)` shared by NPC and avatar;
  one gait; one go-limp rule + body-velocity accessor.
- W2-M one kit + `ItemInstance{name,count,dye,fill,damage}` embedded by every
  item record; `PlayerKit` becomes the avatar's `Kit`.
- W2-N collision layer derived from body role, not set at ~24 sites.
- W2-O selftest gates tick through `TickAuthority` (support::RunTicks).
- W2-P severed-limb "flesh part" (only if the corpse refactor leaves the
  MobSystem→DebrisSystem limb handoff; its plan currently keeps it).
- W2-Q tuning generated from `.def` (min/max in rows → LoadTuning reads + schema).
