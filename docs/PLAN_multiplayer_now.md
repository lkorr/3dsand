# PLAN: multiplayer "now" packages — orchestration order

**Source:** `docs/RESEARCH_multiplayer_readiness.md` (the audit; finding ids
L1…L11, S1…S7, A1…A5 below refer to its §2 tables).
**Written:** 2026-09-10 against `d910f6f`.
**Status:** plan of record. **One orchestrator (Claude Fable 5.1), Opus 5 agents
per package, one worktree per package, integration branch `mp-now`.** Each
package states its owner scope, verified facts to build on, its hash impact,
and the ONE verification that closes it.

This plan does NOT build multiplayer. It removes the six pieces of debt that
grow with every gameplay feature built on top of them, so that M9 is the
plumbing job DESIGN.md §10 promised. Everything in the audit's "later" column
stays later.

---

## 0. Orders for the orchestrator

You are the Fable 5.1 session reading this. Do not implement packages
yourself; you review, sequence, merge, and run the endgame. Concretely:

1. **Read first:** this file end to end, `docs/RESEARCH_multiplayer_readiness.md`
   §2 and §5, `CLAUDE.md` (all of "Build and verify"), and
   `bash scripts/board.sh active`. Then `git status --short` in the main
   checkout: on 2026-09-10 it had uncommitted ragdoll-bug edits in
   `src/game/mob.cpp`, `mob.h`, `src/main.cpp`, `src/phys/*`,
   `src/test/selftest_mob.cpp`, `tests/baseline.json` under an open claim by
   `agent-bd9eed`. **Do not branch packages that touch `main.cpp` until that
   claim is `done` and the edits are committed** (see §1 and the wave table).
2. **Create the integration branch** `mp-now` from the commit you start on.
   Packages merge into `mp-now`; `mp-now` fast-forwards `main` once, at the end.
3. **Launch each package as one `Agent` call**: `subagent_type: "general-purpose"`,
   `model: "opus"`, `isolation: "worktree"`, and the prompt is the package's
   section below **verbatim**, preceded by §1 verbatim and followed by the
   report format in §4. Do not paraphrase a package; the line anchors are the
   point.
4. **Wave discipline** (§3): launch a wave's packages in ONE message; do not
   launch the next wave until every package in the current one has reported
   and merged. N1 and N3 both touch `src/test/support.cpp`; merge N1 first, then
   have N3 rebase onto `mp-now` before its final `--verify`.
5. **Review every diff before merging.** The review question is the package's
   "done means" line, not "did the gates go green". Reject a package that
   adds a mechanic to keep a gate green, that rebaselines a hash it did not
   expect to move, or that runs the same gate twice (CLAUDE.md "verification
   is a budget").
6. **Hash policy:** N1, N4 move `determinismHash` once each, at the commit that
   lands them. N2, N3, N5, N6 must leave it unmoved — that is their cheapest
   correctness proof. A package that moves it unexpectedly is a finding, not a
   rebaseline; send it back with the twice-run comparison result.
7. **Endgame, once:** on `mp-now` after the last merge, `bash scripts/build.sh`,
   then `bash scripts/run.sh ./build/Release/sandvox.exe --suite acceptance`,
   then the three `--rebaseline`s if and only if N1/N4 moved the hash and the
   smokes (a worldgen content move re-pins all three; `selftest --rebaseline`
   does not re-pin smokes). `page-roundtrip` failing inside the suite but
   passing standalone is known. Then fast-forward `main`, update
   `ARCH_NODES` in `assets/tuner.html` (one node "Multiplayer readiness",
   `wip` off, the recently-landed list), and post `board.sh done`.
8. **Write memory** as packages land (the `project-lin-followups-orchestration`
   memory is the format): wave status, measured numbers, what was refuted.
9. **Stop conditions.** A package that cannot meet its kill criterion after two
   agent iterations is reported to the user, not forced. If N1's wait counter
   is non-zero at steady state (§2 N1), that is a design question for the
   user, not something to tune away.

---

## 1. Ground rules for every package (paste verbatim into every agent prompt)

- **One worktree per package, branched from `mp-now`.** Claim your files on the
  board first: `bash scripts/board.sh claim "<files>" "<package id>: <what>"`.
  Re-check `board.sh active` before build and before commit. Post
  `board.sh done` when you stop.
- **Build with `bash scripts/build.sh` in your worktree, ONCE.** Never raw
  cmake. Never launch `sandvox.exe` directly: every run goes through
  `bash "<main checkout>/scripts/run.sh" <exe> <args>`. `export
  SANDVOX_NO_CRASH_DIALOG=1`. Read `crash.log` after any crash and check whether
  its frames name ANOTHER worktree's paths before diagnosing anything else.
- **WGSL and JSON edits need no rebuild.** `bash scripts/check_shaders.sh`
  validates WGSL. `python scripts/check_invariants.py` and
  `python scripts/check_pass_table.py` must stay green.
- **Verification is a budget.** `--gate <name>` while iterating; ONE
  `--verify <gates>` at the end; never a gate and then the suite that contains
  it; never the same gate twice on an unchanged tree. Every run writes
  `build/last_run.json` — read it instead of re-running.
- **Hash discipline.** Your package section says whether `determinismHash` may
  move. If it says "unmoved" and it moved, that is a bug in your change: run the
  twice-run comparison (`--gate determinism`) and report, do not rebaseline. If
  it says "moves once", rebaseline ONLY at your final commit with
  `--selftest --rebaseline`, and do not investigate the new value.
- **Rules 1–3 of CLAUDE.md are inviolable:** integer-only sim math, cost scales
  with activity, all mutations through the op stream. Put thresholds in
  `tests/baseline.json`, not C++. A new gate must be verifiable with
  `--gate <name>` alone.
- **Do not add mechanics to keep a gate green.** If a fixture breaks because
  your change is correct, fix the fixture and say so.
- **DESIGN.md is architecture truth.** If your change contradicts it, update
  the contradicted paragraph in the same commit (claim `DESIGN.md` on the board
  first). Keep the update to the paragraph you contradicted.
- **Report format** is fixed (§4). Numbers go in a table. Name the commit(s),
  the branch, the gate lines, the hash before/after, and what you did NOT do.

---

## 2. Packages

### N1 · Fixed-latency snapshot pipeline (L1, L1′) — C++ only, hash moves once

**Owner scope:** `src/sim/world.h`, `src/sim/world.cpp` (readback ring:
`EncodeReadbacks` `:368-477`, `KickReadback` `:488`, the `MapReadAsync`
callback `:488-684`), `src/test/support.cpp` / `support.h` (`SubmitTick`'s
drain block `:1195-1290`, `SetHarnessSnapshotDrain` `:169-176`), a new gate in
`src/test/selftest_sim.cpp` (or a new `selftest_snapshot.cpp` registered in
`kOrder`), `tests/baseline.json`. NOT `main.cpp` (claimed), NOT `stream.cpp`
(reference only).

**Verified facts to build on:**
- The ring has `kReadbackSlots = kMaxTicksPerFrame * (kFramesInFlight + 1)` = 16
  slots (`world.h:3303-3306`). A tick with no free slot gets no snapshot:
  `world.cpp:376 if (slot < 0) return false;`.
- The snapshot lands whenever `ctx.ProcessEvents()` retires the fence;
  `snap_.tick = sl.tick; snap_.valid = true;` at `world.cpp:683-684`.
  `simulation.h:162-164` documents "one tick latent … can be older when the
  ring is saturated".
- The harness path blocks for the map every tick (`SetHarnessSnapshotDrain`,
  called from `selftest.cpp:762`, `vk_smoke.cpp:305`, `lab.cpp:777`,
  `measure.cpp:815`, `main.cpp:2058/3971`). The windowed game does not. So the
  determinism gate tests a fixed 1-tick latency the game never runs at.
- Paged mode ALSO defends a bounded staleness: `kPagedSnapshotMaxGap = 4`
  (`support.cpp:1242`, env `SANDVOX_SNAP_MAXGAP`) with a `WaitIdle()` fallback,
  because the page-table mirror (`TightenFromSnapshot`, `main.cpp:5990-6010`)
  starves without snapshots. Any K you pick must be ≤ this gap or you must
  raise both together.
- **The pattern to copy** is `Stream::CompleteDueShifts` (`stream.cpp:968-989`):
  a constant `kWakeLatency = 4`, a `PendingShift` recording what is owed at
  T+K, `map.Wait()` if not ready with a counter (`timing_.wakeWaits`), never a
  skip. Its argument (`stream.h:318-326`): K constant ⇒ the outcome is a pure
  function of (inputs, tick).
- Consumers that must NOT change in this package (they become deterministic by
  construction once the snapshot they read is T−K exactly): `Brush::BuildOp`
  (`brush.cpp:9-14`), mob ground probe (`mob.cpp:1836-1841`), spell ladder
  (`spell.cpp:2176-2190`), island `EventReady` (`debris.cpp:607-645`),
  `windWake` (`support.cpp:564-567`), `particlesActive`/`fluidCount`
  (`main.cpp:7505-7507, 7573`).

**Build:**
1. Introduce `World::kSnapshotLatency` (constant, start at the value the
   deferred wake uses, 4; put the number beside `kWakeLatency` with a comment
   that they are the same argument). `World::Snap()` at tick T returns the
   snapshot of tick T−K exactly, or blocks (with a counted wait) until it can.
   A tick that would have found no free ring slot must instead wait for one —
   `return false` at `world.cpp:376` becomes impossible on the sim path.
2. Make the harness drain the SAME code path: `SetHarnessSnapshotDrain(true)`
   becomes a no-op or is deleted, so `--selftest` runs at the shipped latency.
   Expect the hash to move once, because every snapshot consumer now reads
   T−4 instead of T−1. That is the intended move; rebaseline at your final
   commit only.
3. Expose the wait counter and a staleness histogram (`SnapshotStallStats`
   already exists, `main.cpp:10064-10073` reads it — do not edit `main.cpp`;
   expose through `World` and let the existing HUD line pick it up if it
   compiles, otherwise leave the HUD to N5).
4. New gate `snapshot-latency`: drives `SubmitTick` for ≥64 ticks through a
   worldgen'd window, asserts on EVERY tick that `Snap().tick == tick − K` and
   that the ring never declined; then runs the same 64 ticks with an
   artificially deep GPU queue (submit 4 ticks before the first
   `ProcessEvents`, the `kMaxTicksPerFrame` case) and asserts the same
   equality and the same hash. Threshold values in `tests/baseline.json`.

**Kill criterion:** in a `--frames 600 --autofly-surface` run with
`SANDVOX_FRAMES_NO_RELOAD=1`, the wait counter must be ≤ 1% of ticks. If a
constant K cannot be found under `kPagedSnapshotMaxGap` that satisfies this,
STOP and report the histogram — deepening the ring is allowed (it is
kilobytes per slot), raising the gap is a user decision.

**Done means:** "a gameplay decision that reads the snapshot at tick T reads
the world at tick T−K, on every machine, at every frame rate" is true and
the `snapshot-latency` gate says so. Update DESIGN.md §2's "one tick latent"
sentence and `simulation.h:162-164` to say K.

**Verification, ONE launch at the end:**
`--verify determinism,snapshot-latency,streaming,page-roundtrip,mob-burn`
then the `--frames` kill-criterion run. Hash: moves once → `--selftest
--rebaseline` at the commit.

---

### N2 · Per-tick input command + player controller into the tick (L2, L3) — C++ only, hash unmoved

**Owner scope:** `src/game/player.h`, `src/game/player.cpp`,
`src/game/strike_pick.h`, `src/game/melee.cpp` (`:1391-1395` only),
`src/main.cpp` (input block `:4729-5163`, `player.Update` `:5670`, mouse
sampling `:5822-5823, 5940`, the latches `:5843-5854`, `strikePicker.Feed`
`:4969`), `src/test/selftest_player.cpp` or wherever the player gates live,
`tests/baseline.json`. **Blocked until `agent-bd9eed`'s `main.cpp` claim is
done and committed** — check the board; if it is still open, do
`player.*`/`strike_pick.h`/`melee.cpp` first and the `main.cpp` wiring last.

**Verified facts to build on:**
- `PlayerInput` (`player.h:8-16`) is "per-frame movement intent, filled from
  GLFW polling"; built at `main.cpp:5156-5163`; eight direct GLFW reads in
  `main.cpp`, zero elsewhere in `src/`. Mouse buttons `:5822-5823`; `mouseL`
  sampled once per frame at `:5940` and read on every tick of the
  `while (accumulator >= kTickDt …)` loop at `:5943`.
- `player.Update(dt, pin, …)` runs per FRAME at `main.cpp:5670` with frame `dt`
  clamped to 50 ms (`player.cpp:448-450`), integrating with `exp`/`pow`
  smoothing (`:490, 799, 826, 848, 866`) and timers (`:592-773`). Its output
  `player.pos` reaches world state through `phys.MovePlayerBody(playerBody,
  player.pos, kTickDt)` at `:7544` (→ Jolt → settle-back cell ops) and through
  every tool that authors an op at the player's position.
- The sticky latches (`castQueued`, `strikeQueued`, `ui.placePrefab`,
  `ui.spawnMob`, `dropStatusQueued`, `main.cpp:5843-5854`) exist because the
  tick loop runs zero times on most frames at 50 fps. They are the ad-hoc
  half of the command struct.
- `StrikePicker::Feed(dx, dy, dt)` smooths on frame dt (`strike_pick.h:34-40`)
  and `Pick()` selects the strike index that authors a carve (`main.cpp:5916`).
  `melee.cpp:1391-1395` drains a per-frame pixel accumulator per tick.
- `Camera::ApplyMouse` (`camera.cpp:7-12`) has no dt and is already
  frame-rate independent — leave it per frame.
- Every OTHER gameplay system already steps at `kTickDt` inside the loop
  (`mobs.PreTick`, `avatar.PreTick`, `melee.Update`, `spells.Tick`, `phys.Step`).
- The perf suite has its own accumulator loop (`perfsuite.cpp:1406-1418`) and
  `--selftest` never runs the game loop, so this package cannot move the
  selftest hash. Its proof is a gate you write.

**Build:**
1. `struct TickInput` in `player.h`: move axes, look delta (accumulated pixels
   since the last tick), jump/crouch/sprint held, button HELD bits, button
   PRESSED-edge bits (edges are latched by the frame layer until a tick
   consumes them — this replaces the ad-hoc latches), tool/hotbar selection,
   cast/strike/place/spawn/drop as pressed edges, and the frame-layer's
   current camera basis (forward/right) captured at tick time. POD,
   fixed-size, versioned — it will be recorded by N3 and is a network message
   later.
2. A frame-layer accumulator in `main.cpp` builds `TickInput` from GLFW and
   hands one per tick to the loop. Held state is sampled AT the tick, not once
   per frame for all ticks. Pressed edges are consumed by exactly one tick.
3. `Player::Update(kTickDt, const TickInput&, KindFn)` runs INSIDE the tick
   loop, before `mobs.PreTick`. Store `prevPos/pos` so the render layer can
   interpolate the camera by the accumulator fraction (the celestial clock
   already does this for the sky, `celestial.h:20-24` — copy the pattern; the
   sim must not read the interpolated value).
4. `StrikePicker` and melee mode 1 consume the per-tick look delta with
   `kTickDt`; delete their frame-dt paths.
5. Gate `tick-input`: a scripted `TickInput` sequence (walk, jump, turn,
   strike) fed for 300 ticks twice, once with 1 tick per "frame" and once with
   4, asserting identical `player.pos` trajectory to the bit and identical op
   vectors per tick (N3's recorder if merged, else compare the vectors
   directly). Put the trajectory's final position in `tests/baseline.json`.

**Kill criterion:** feel. A `--frames 300 --autofly-hard` run must be clean, and
the third-person camera must not visibly stutter at 144 fps (interpolation
is what prevents it). If interpolation cannot be made smooth without the sim
reading it, STOP and report; do not let the sim read a frame value.

**Done means:** "the same recorded `TickInput` sequence produces the same
player trajectory and the same op stream at any frame rate" and the
`tick-input` gate says so. `grep glfwGet src/game/` is empty; the only GLFW
reads are in the frame layer. Update DESIGN.md §8 (player) and §10's
"fixed tick" bullet.

**Verification, ONE launch at the end:**
`--verify determinism,tick-input,ledge-grab,player-styles` (use the real gate
names from `--selftest --list`) plus the `--frames` feel run. Hash: unmoved.

---

### N3 · Op-stream hygiene: recorder/replayer, author + sequence, dedupe, clamps (L4, L5, A1, A5) — C++ + one WGSL edit, hash unmoved

**Owner scope:** new `src/sim/oprecord.h/.cpp`, `src/test/support.cpp`
(`SubmitTick` entry and the clamp block `:470-486`), `src/sim/world.h` (op
structs `:337-357, 630-641` — comments and `ExplosionOp.pad0` rename only, no
layout change), `assets/shaders/sim_mutate.wgsl` (`:60-104` and `:120-146`),
`src/main.cpp` arg parsing (`:2929-3297`, two flags — **only after
`agent-bd9eed`'s claim is done**; until then wire the flags through
`selftest`/`support` and leave `main.cpp` for last), a new gate in
`src/test/selftest_sim.cpp`, `tests/baseline.json`, DESIGN.md §2 paragraph.

**Verified facts to build on:**
- The queue is six per-tick `std::vector`s (`BrushOp` 32 B cap 64,
  `ExplosionOp` 32 B cap 8, `CellOp` 8 B cap 65536, `ParticleSpawn` 32 B cap
  4096, `FluidSpawnOp` 32 B cap 4096, `GasSpawnOp` 32 B) uploaded by ONE
  `WriteBuffer` each per tick in `SubmitTick` (`support.cpp:771-786`). Ops carry
  no tick and no sequence; identity is `(tick, buffer index)`.
- CPU push order IS deterministic (fixed sequence in the tick body, mob burn
  rotates by a tick-derived start, `WorldEdits::Drain` walks an ordered vector,
  `worldedit.cpp:114-133`). The shader just does not honour it.
- `sim_mutate.wgsl:104` and `:146` are plain `voxStore`s; `sim_explode.wgsl:14-15,
  128-131` already implements "lowest op index that destroyed a cell owns it"
  with a two-phase mark/apply. Same-material paints are benign (palette
  variant keys on `slotIdx`, not the op).
- `SubmitTick` truncates `cells`/`spawns`/`fluid` silently (`support.cpp:470-478`)
  and does not clamp `ops`/`exps` at all (`:480-481`, buffer sized
  `kMaxOpsPerTick * 32` at `world.cpp:106`); `mob.cpp:3863` and `avatar.cpp:2057`
  push `BrushOp`s without checking the cap.
- `BrushOp._p0/_p1` are TAKEN (spell transmute filter, `spell.cpp:1618-1620`).
  `ExplosionOp.pad0..2` are free. `CellOp` has no room. `WindPrim::ownerId`
  (`windprim.h:96-99`, set at `main.cpp:7196`) is the precedent for an owner
  on a tick-stream object.
- Many `sim.*` tuning words ride `TickParams` per tick on purpose "so a replay
  reproduces the stream" (`world.h:2000-2003`). The record must carry the
  whole `TickParams` struct, not just the op payloads. `Stream` writes a second
  `TickParams` for `EncodeGenList` (`stream.cpp:795-806`) — record the gen list
  it dispatches too, or the replay cannot regenerate the same planes.
- The dev "clear fluid" button (`main.cpp:6392-6393`) writes `fluidArgsStage`
  directly — the one rule-3 bypass on the gameplay path.

**Build:**
1. **Author, CPU-side only.** A parallel `std::vector<OpMeta>` per op vector
   (`{author: u32 entity id or 0 = local player, producer: u8 enum brush /
   mob / debris / spell / avatar / editlayer / lab}`), pushed beside every op
   by the producer that emits it. NOT uploaded — the GPU does not need it; the
   record and the future network layer do. Rename `ExplosionOp.pad0` →
   `author` and fill it, since it is free and explosions are the one op that
   fans out. No layout change anywhere.
2. **Recorder.** `--record-ops <file>`: at `SubmitTick` entry, append one
   framed record per tick: `{tick, seed, TickParams (whole struct), the
   `TickInput` if N2 has merged, the six op vectors with their `OpMeta`, the
   Stream gen-list for that tick}`. Versioned header with `kWorldN`, `kChunk`,
   `kVoxelMeters` bit pattern, material name table hash (copy `worldio.cpp:
   146-168`'s refusal style). LZ4 or none — size is not the point yet.
3. **Replayer.** `--replay-ops <file> [--ticks N]`: boots the same worldgen
   (seed from the header), then feeds `SubmitTick` from the record instead of
   the game, and prints the world hash every 15 ticks plus the final one. Also
   usable from a gate.
4. **Dedupe, deterministic.** Cell ops: CPU-side stable sort by `cellIdx` +
   keep-first before upload (≤65536 ops, one `std::stable_sort`, push order is
   the priority). Brush ops: shader-side, copy `sim_explode`'s rule — a thread
   for op `i` at cell `c` returns if any op `j < i` covers `c` and would write
   it (same sphere test + same mode/filter gate). n ≤ 64 so the loop is cheap.
   Document the rule in the shader header.
5. **Clamps at the choke point.** `SubmitTick` clamps `ops` and `exps` to their
   caps and counts every truncation in all six streams into a per-run counter
   surfaced in `build/last_run.json`; the two unchecked producers check the cap
   before pushing.
6. **The fluid-clear bypass** becomes an op (a `FluidSpawnOp` with a "clear"
   species, or a `TickParams` flag that the fluid kernel honours) — or, if that
   is disproportionate, add it to CLAUDE.md rule 3's exception list with the
   line number and the reason. Say which you did.
7. Gate `ops-replay`: run a scripted 200-tick scene (brush, an explosion, a
   prefab stamp, a mob burn — reuse an existing fixture) with the recorder on,
   then replay the file and assert hash-for-hash equality at every 15-tick
   probe AND that two overlapping same-tick brush ops of different materials
   produce the same cell contents on both runs (this is the L5 test; before
   step 4 it must be able to fail). Byte-size of the record for the scene goes
   in `tests/baseline.json` as an informational pin.

**Kill criterion:** none expected. If the dedupe MOVES the selftest hash, some
existing gate has overlapping same-tick ops; report which gate and which cell
(the recorder tells you), and rebaseline only if the orchestrator agrees the
overlap was a latent race.

**Done means:** "a recorded session replays to the same hash on the same
binary" and "two ops on one cell in one tick have a defined winner" are both
true and the `ops-replay` gate says so. Rewrite DESIGN.md §2's "it is also the
serialization format … replay log" sentence to state what is now implemented
and what is not (saves are still chunk snapshots).

**Verification, ONE launch at the end:**
`--verify determinism,ops-replay,mob-burn,armor-react,spells`. Hash: unmoved
(see kill criterion).

---

### N4 · Integer wind weather and integer landform bake (L6) — C++ only, hash moves once (maybe twice)

**Owner scope:** `src/sim/wind.h` (`WindWeather`, `:136-166`; the fix note
`:179-200`), `src/sim/worldmap.cpp` (`:214, :247` and the profile at `:123`),
`src/sim/worldmap.h` if a helper is needed, `assets/shaders/sim_fluid.wgsl:185-187`
(`FLUID_FOAM_DECAY`), `src/test/selftest_worldgen.cpp` (`terrain` gate) if the
bake changes a byte, `tests/baseline.json`, `tests/env_predictions.json` if the
Environment tab's predictions pin landform bytes. **Board conflict:**
`agent-f4fd05` claims `worldmap.cpp` for package 2 (sandstone) — check
`board.sh active`; if still open, do `wind.h` first and coordinate the
`worldmap.cpp` hunk through the orchestrator.

**Verified facts to build on:**
- `wind.h:136-166` computes `cos/sin/atan2` on floats, then quantises four
  scalars to Q16.16 (`common.wgsl:559-561`) per tick. The header's own note
  (`:179-200`) says libm is not bit-identical across platforms and names the
  fix: integer BAM heading + the existing integer `isqrt`. `windMode` defaults
  to 1 (`tuning.h:1769`), so this is on for everyone.
- `worldmap.cpp:214`: `const double ang = st.rotation * π / 180.0; ca =
  std::cos(ang), sa = std::sin(ang);` feeds the landform accumulator;
  `:247` `lround(acc[i])` → `uint8_t`. `:123`'s `ProfileAt(.., std::sqrt(..))`
  is `+ - * / sqrt` only (IEEE-exact) — leave it. The map content hash
  (`worldmap.cpp:1076-1077`) is over the FILES, so it does not catch a bake
  difference.
- The worldgen CPU twin is token-mirrored and checked by `check_invariants.py`
  and compared per voxel by `--gate terrain`. A landform byte change moves
  terrain everywhere → hash moves and all three baselines re-pin.
- `FLUID_FOAM_DECAY` is the ONLY transcendental in any const block; it folds
  in-process through Tint, so the fold runs on host libm. Foam → stain →
  hashed.

**Build:**
1. `WindWeather` end-to-end integer: heading as a 16-bit BAM, direction from
   an integer sine table (Q15, 256 or 1024 entries, one table shared with the
   worldgen `isin16` if it exists in C++ — check `world.cpp`'s mirror), speed
   and gust as Q16.16 with integer lerp. Output the same four Q words; only
   the last-bit values change. Keep `WindWeather` a pure function of
   (tuning, seed, tick) — the wind gates depend on it.
2. Landform bake: rotation degrees → BAM, `cos/sin` from the same integer
   table, accumulate in fixed point, round with an explicit integer half-up.
   Expect at most ±1 in a few bytes; the `terrain` gate will name them. If
   more than a handful of bytes move, your table is too coarse — 1024 entries
   Q15 is enough for a 20 km map at 8 bits.
3. `FLUID_FOAM_DECAY`: replace `exp(log(...)/N)` with either an integer Nth-root
   by binary search in Q16.16 (const-eval is fine, it is integer) or by
   making the tuning knob the per-substep decay directly. Say which.
4. `--sweep sim.windMode=0,1` and a wind gate prove reachability; the
   `--gate terrain` pass D output names any moved bytes.

**Kill criterion:** if the integer landform bake moves more than ~0.1% of
landform bytes by more than 1, stop and report — that means the float bake was
never the thing the map author saw, and the user decides.

**Done means:** `grep -n "std::\(sin\|cos\|atan2\|exp\|log\|pow\)" src/sim/wind.h
src/sim/worldmap.cpp` is empty, and the sim's tick input stream is produced by
integer math only, on every machine. Update DESIGN.md §9b's wind paragraph and
the world-map bake paragraph.

**Verification, ONE launch at the end:**
`--verify determinism,terrain,wind-drift,biomes` (real gate names from
`--list`). Hash: moves once (wind) and possibly again (landform) — rebaseline
at the final commit; the orchestrator re-pins smokes in the endgame.

---

### N5 · Session boundary: `PlayerSession`, `TickAuthority`, no new `main()` locals (A3, S4 rule) — C++ only, hash unmoved, PROVEN by N3's recorder

**Owner scope:** `src/main.cpp` (the locals at `:4188-4330`, the tick body
`:5943-7631`, the frame body around it), new `src/game/session.h/.cpp`,
`src/game/mob.h/.cpp` (`SetAvatar`/`SetPlayerActor` → list-shaped, `:1713-1736,
2488-2502`), `src/game/persist.cpp` (`PLYR` section reads the struct instead of
scattered refs, `:421-478`), `src/sim/waterbody.h` comment, DESIGN.md §10.
**Runs LAST, alone (wave 3): it conflicts with every other package in
`main.cpp`.** Requires N3 merged (its recorder is this package's proof) and
`agent-bd9eed` done.

**Verified facts to build on:**
- `main()` is `main.cpp:2810-10198`. Per-player state is a set of locals:
  `Camera cam; Player player; Brush brush; PrefabPlacer placer; PlayerAvatar
  avatar; ThirdPersonRig tpRig; CameraMode camMode; float avatarHeading;
  float fovNow; float respawnTimer; SpellSystem spells; PlayerCaster caster;
  CasterHealth playerHealth; MeleeState melee; std::vector<Grenade> grenades;
  bool captured; uint64_t playerBody` (`:4188-4330`, `:4258`).
- `MobSystem::SetAvatar(Mob*)` is registered once for the session
  (`main.cpp:4203-4207`); `playerActor_.id = 0` is reserved
  (`mob.cpp:1378-1387`); the AI actor list is rebuilt per tick as
  `[playerActor_] + mobs_` (`mob.cpp:3040-3057`) and is already faction-keyed
  (`ai_behavior.h:276-287`). The plumbing is singular; the design is not.
- `Player::KindFn` is a `std::function` (`player.h:24`) — per-player collision
  sources plumb through it.
- Process globals that are sim-affecting: `CurrentTuning()`, `CurrentWorldMap()`,
  `WaterBodies()`, `WorldEditLayer()`, `Celestial()`, `WindPrims()`,
  `CurrentPrims()`, `World::LabWorld()` (audit §2.1 S4 and the table under
  "Globals"). `WaterBodies()` is the one keyed on the window origin
  (`waterbody.cpp:205-212, 785-809`).
- The tick body already separates from the render block (`if (target)` at
  `:7660`); `tick` advances when `AcquireFrame()` returns null. There is no
  headless continuous-play loop — only gate/harness loops.

**Build (mechanical; the diff must be behaviour-free):**
1. `struct PlayerSession` owning the locals above plus its `TickInput`
   accumulator (from N2). One instance in `main()` today; a `std::vector` is
   NOT required yet, but nothing may assume there is one (no static, no
   global, no `Mob*` cached across ticks).
2. `void TickAuthority(World&, Simulation&, Stream&, MobSystem&, DebrisSystem&,
   …, PlayerSession&, const TickInput&, uint32_t tick, OpBatch& out)` — the
   tick body between the accumulator loop's braces moved into a function in
   `session.cpp`, taking the six op vectors as an `OpBatch` struct. The frame
   loop calls it; `--frames`/autofly paths call it; a future headless server
   loop calls it. No behaviour change: the recorder proves it (step 5).
3. `MobSystem::SetPlayerActor` → `SetPlayerActors(span)`, `SetAvatar` →
   `SetAvatars(span)`; reserved id 0 stays for "the local player" for now.
4. A comment block at the top of `session.h` stating the rule: *per-player
   state lives here; sim-affecting process globals must not be keyed on the
   window origin (WaterBodies is the grandfathered exception, tracked in
   RESEARCH_multiplayer_readiness.md S4)*. Add the same rule as one line under
   CLAUDE.md "Conventions" (claim `CLAUDE.md`).
5. **Proof:** before the refactor, record a 600-tick `--frames --autofly-hard`
   session with N3's `--record-ops` on the `mp-now` binary. After the refactor,
   record the same session with the same seed and inputs (autofly is scripted)
   and `cmp` the two files — they must be byte-identical. That is the whole
   acceptance; a gate is not needed for a mechanical move. If they differ, the
   diff's first differing tick and op index (the recorder's frame header) is
   the bug.

**Kill criterion:** if `TickAuthority` cannot be extracted without a behaviour
change inside two iterations (e.g. a frame-layer value the tick body reads
that N2 missed), report the value, do not paper over it with a global.

**Done means:** "a second `PlayerSession` could be constructed without a
compile error and without touching a global", the recorded session is
byte-identical, and DESIGN.md §10 has a new paragraph "the authority /
presentation boundary" naming `TickAuthority` and `PlayerSession` — and
recording the model decision from N6.

**Verification:** the `cmp` above, then ONE `--verify determinism,mob-burn,
ragdoll,player-styles`. Hash: unmoved.

---

### N6 · Interest set + the model decision written down (S6, S7, §5) — C++ small, docs, hash unmoved

**Owner scope:** `src/sim/stream.h/.cpp` (`Stream::Update(IVec3, tick)`
`:204-230`), `src/sim/farfield.h/.cpp` (`FarField::Update(IVec3)` `:116`),
one call site in `main.cpp` (`:6026-6045` — after `agent-bd9eed` is done, or
leave a one-line adapter for N5 to switch), DESIGN.md §10 (claim it), a boot
line in `src/main.cpp:3625-3627` beside the environment stamp (optional, see
step 3).

**Verified facts to build on:**
- `Stream::Update(IVec3 playerChunk, uint32_t tick)` shifts one axis per call
  with 2-chunk hysteresis from one point (`stream.cpp:229-230`). `FarField`
  has the same shape. There is no interest-set type anywhere; the nearest
  thing is `DebrisSystem::AddTerrainAnchor(Vec3, float)` (`debris.h:95`),
  which drives the CPU fetch cache only.
- The chunk-ticket plan (`docs/PLAN_chunk_tickets.md` §0–1, P0 landed with
  `kTicketMax = 0`) is the designed seam for "resident outside the window".
  Its non-goals (no reduced tick rate, no EMIT/PAIR reactions in tickets)
  bound what a remote-player region could simulate.
- `biomes::EnvironmentStamp` (`biomes.h:264-271`, printed at boot
  `main.cpp:3625-3627`) hashes worldgen assets; nothing hashes tuning,
  materials or reactions (L8).

**Build:**
1. `struct InterestSet { std::vector<IVec3> chunks; int primary; }` (or
   points+radius) in `stream.h`. `Stream::Update(const InterestSet&, tick)`
   picks the origin from `primary` today — behaviour identical, hash unmoved —
   and `FarField::Update` takes the same. The point is the signature: the
   next person cannot add a second point of interest without going through
   it.
2. DESIGN.md §10 rewrite, ONE screen: strike "both options viable"; state the
   model from `RESEARCH_multiplayer_readiness.md` §5 as the decision of
   record (host-authoritative op stream, deterministic client CA, chunk
   authority, hash-triggered resync, singleplayer = host with loopback); list
   what M9 still needs (the audit's "later" column); state the two rules
   (no sim-affecting global keyed on window origin; no new gameplay decision
   reads the snapshot outside the fixed-latency path). **The orchestrator
   confirms the model with the user before this lands**; the default is §5.
3. Optional, ≤40 lines: `TuningStamp` — FNV over `tuning.json`, `materials.json`
   and the reaction files, printed on the same boot line as the environment
   stamp and written into `build/last_run.json`. It is in the "later" column,
   but it is the cheapest possible desync-cause eliminator and costs no
   rebuild once written. Do it if the rest of N6 is done in under a day.

**Done means:** the interest-set signature exists and is the only way to move
the window; DESIGN.md §10 says what we are building and why; hash unmoved.

**Verification, ONE launch:** `--verify determinism,streaming,far-fog`. Hash:
unmoved.

---

## 3. Waves and schedule

```
Wave 1 (parallel, disjoint files except support.cpp — merge N1 before N3 rebases)
  N1 snapshot latency (world.*, support.*)          hash moves once
  N3 op hygiene (oprecord.*, sim_mutate.wgsl, support.cpp entry)   unmoved
  N4 integer wind + landform (wind.h, worldmap.cpp)  hash moves once/twice
        ▼ all three merged into mp-now; orchestrator: --gate determinism once on the merged build
Wave 2 (parallel; BOTH gated on agent-bd9eed's main.cpp claim being done+committed)
  N2 tick input + player in tick (player.*, main.cpp input block)   unmoved
  N6 interest set + DESIGN §10 (stream.*, farfield.*, DESIGN.md)   unmoved
        ▼ merged; orchestrator confirms the model decision with the user before N6's DESIGN edit lands
Wave 3 (alone)
  N5 session boundary (main.cpp tick body → session.cpp)            unmoved, proven by N3's recorder
        ▼
Endgame: build, --suite acceptance ONCE, rebaseline the three tables (N1/N4 moved them), ff main, ARCH_NODES, memory, board done.
```

Dependencies: N2 → N5 (`TickInput` is a `PlayerSession` member); N3 → N5
(proof); N1 is independent but should land first so N2's gate runs at the
shipped snapshot latency. N4 and N6 are independent of everything.

Rough sizes: N1 M, N2 M, N3 S–M, N4 S, N5 M, N6 S. With three Opus agents in
wave 1 and two in wave 2, the wall-clock is dominated by N5's serial wave and
the two cold builds that `main.cpp` packages cannot avoid.

---

## 4. Report format (every agent, verbatim headings)

```
## <package id> — <one-line result>
Branch / commits:
Files touched:
Hash before → after (and whether the section allowed a move):
Gate lines (paste from build/last_run.json, not re-run):
Numbers (table):
What I did NOT do, and why:
DESIGN.md / CLAUDE.md paragraphs updated:
Board: claim posted at <time>, done posted at <time>
```

The orchestrator reads the diff, then the report, in that order.

---

## 5. Deferred with triggers (from the audit's "later" column)

| Item | Trigger to pull it forward |
|---|---|
| Per-chunk hash keyed on world coord (S3) | The first two-window or two-client test. |
| Ticket slots P1 / second host window (S7) | The chunk-authority decision says the host sims near remote players. |
| Hash over particle/fluid/gas buffers (L9) | The first desync that N3's recorder cannot localise to a voxel tick. |
| Items keyed on game ids not Jolt handles (A2) | Entity state sync design. |
| Container-order hardening (X1–X3) | Any client on a different binary or STL. |
| Particle-cap refusal gate (X4) | Any run that reaches `PARTICLE_CAP` outside the fixture that already did. |
| Local pacing under network pacing (L7) | Transport lands. |
| Fold `Stream`'s second `tickUBO` write into the tick record (L10) | N3's replayer fails to regenerate a streamed plane. |
