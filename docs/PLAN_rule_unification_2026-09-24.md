# PLAN: one rule, one place (rule-unification program, 2026-09-24)

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

## Wave 2 (after the corpse refactor lands on main; launched then)
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
