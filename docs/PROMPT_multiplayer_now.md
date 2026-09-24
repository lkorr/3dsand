# PROMPT: execute PLAN_multiplayer_now.md, packages N1–N6

Paste this whole file as the first message of a fresh session. It is the
orchestrator brief. Written 2026-09-20 against `fc593bc`, ten days after the
plan it drives — §2 below is the list of ways that plan is now wrong.

---

## 0. What you are doing

You are the orchestrator for `docs/PLAN_multiplayer_now.md`. The user has asked
for **all six packages, N1 through N6**, not just the compounding ones. N4 and
N6 are in scope even though nothing forces them yet.

This work does **not** build multiplayer. It removes six pieces of debt that get
worse with every gameplay feature layered on top, so that M9 is the plumbing job
DESIGN.md §10 promises. Everything in the audit's "later" column (plan §5) stays
later.

You do not implement packages yourself. You sequence, launch, review, merge, and
run the endgame.

## 1. Read first, in this order

1. `docs/PLAN_multiplayer_now.md` end to end — it is the specification. Each
   package's section is the agent prompt, verbatim.
2. `docs/RESEARCH_multiplayer_readiness.md` §2 and §5 — the audit the findings
   ids (L1…L11, S1…S7, A1…A5) refer to.
3. `CLAUDE.md`, all of "Build and verify". The three inviolable rules bound
   every package.
4. `bash scripts/board.sh active`, then `git status --short`.

## 2. Corrections to the plan — apply these, they override the plan text

The plan was written against `d910f6f` on 2026-09-10. Its *reasoning* is intact;
its *specifics* have rotted. Four things:

**(a) Every `main.cpp` line anchor is stale, by roughly +1,400 lines.** The plan
cites the per-player locals at `:4188-4330`; they are at `:5578-5870` today, and
`main()` is `:4084-13470` (9,386 lines). The input block, the tick body and the
latch list have all moved by comparable amounts. **Instruct every agent to
re-derive its anchors by grep before acting on any `:NNNN` in its section.** The
named symbols are all still correct — only the numbers lie.

Verified still-true structural facts, with today's anchors:

| Plan says | Today |
|---|---|
| `world.cpp:368` `EncodeReadbacks` | `:459` |
| `world.cpp:376` `if (slot < 0) return false;` | `:467` |
| `world.cpp:488` `KickReadback` | `:596` |
| `world.cpp:683-684` `snap_.valid = true` | `:792` |
| `support.cpp:1242` `kPagedSnapshotMaxGap` | `:1394` (env `SANDVOX_SNAP_MAXGAP` `:1396`) |
| `support.cpp:169-176` `SetHarnessSnapshotDrain` | `:1625`, decl `support.h:184` |
| `stream.h` `kWakeLatency = 4` | `:376`, unchanged |
| `stream.h:85` `Stream::Update(IVec3, uint32_t)` | unchanged |
| `farfield.h:116` `FarField::Update(IVec3)` | `:37` |
| `wind.h:136-166` float `cos/sin/atan2` | `:137-162`, unchanged |
| `worldmap.cpp:214` `std::cos(ang), std::sin(ang)` | exact, unchanged |
| `player.h:8-16` `struct PlayerInput` | `:9`; `Update` `:25-26`; `KindFn` `:23` |
| `mob.h:3617` `SetAvatar(Mob*)` | exact |
| `mob.cpp:2383` `SetPlayerActor`, id 0 | exact; actor list built `mob.cpp:6260` |

**(b) The board blocker is dead.** The plan gates N2, N3 and N5 on
`agent-bd9eed`'s `main.cpp` claim. That claim is ten days old and its edits
landed. Treat it as abandoned; re-check `board.sh active` for anything *new*
before launching, and note-and-proceed on anything stale.

**(c) The working tree is dirty and N1 owns files in it.** `git status` at
`fc593bc` shows ~30 modified files including `src/sim/simulation.cpp`,
`src/sim/world.cpp`, `src/sim/world.h`, `src/test/support.cpp`,
`src/game/mob.cpp` and `tests/baseline.json` — all of which N1, N3 or N5 claim.
`src/main.cpp` is clean. **Resolve the tree before branching anything: commit it
or discard it. Never `git stash`** (see memory `workflow-git-stash-hazard`).

**(d) The endgame is wrong. Do NOT run `--suite acceptance`.** The plan §0.7
prescribes full acceptance plus three rebaselines. The user has a standing
instruction against full acceptance runs and against redundant verification
(CLAUDE.md "verification is a BUDGET"; memories `feedback-no-full-acceptance-runs`,
`feedback-test-less-code-more`). Replace the endgame with §5 below.

## 3. Sequence

The plan's wave structure is right and survives the corrections — it is now
simply unblocked. Launch each package as one `Agent` call, `subagent_type:
"general-purpose"`, `model: "opus"`, `isolation: "worktree"`, branched from the
integration branch `mp-now`. The prompt is plan §1 verbatim, then §2 (this
file's corrections, verbatim), then the package's section verbatim, then the
plan §4 report format. **Do not paraphrase a package.**

```
Wave 0  Clean the working tree. Create `mp-now`.

Wave 1  (parallel, one message)
  N1  fixed-latency snapshot pipeline   world.*, support.*        hash MOVES once
  N3  op-stream hygiene + recorder      oprecord.*, sim_mutate.wgsl  unmoved
  N4  integer wind + landform bake      wind.h, worldmap.cpp      hash MOVES once/twice
     -> merge N1 FIRST (it and N3 both touch support.cpp), then N3 rebases onto
        mp-now before its final --verify. Then one `--gate determinism` on the
        merged build, once.

Wave 2  (parallel, one message)
  N2  TickInput + player into the tick  player.*, main.cpp input   unmoved
  N6  InterestSet + DESIGN §10          stream.*, farfield.*       unmoved
     -> confirm the multiplayer MODEL with the user before N6's DESIGN.md
        edit lands. Default is RESEARCH_multiplayer_readiness.md §5:
        host-authoritative op stream, deterministic client CA, chunk authority,
        hash-triggered resync, singleplayer = host with loopback.

Wave 3  (alone — it conflicts with every other package in main.cpp)
  N5  PlayerSession + TickAuthority     main.cpp tick body -> session.cpp  unmoved
```

Dependencies: N2 → N5 (`TickInput` is a `PlayerSession` member). N3 → N5 (its
`--record-ops` recorder is N5's entire acceptance proof — a byte-identical
`cmp` of a 600-tick scripted session before and after the extraction). N1 should
land before N2 so N2's gate runs at the shipped snapshot latency. N4 and N6 are
independent of everything.

## 4. Review standard

Read the diff, then the report, in that order. The review question is the
package's **"done means"** line, not "did the gates go green". Reject a package
that:

- adds a mechanic to keep a gate green (memory: `feedback-no-unasked-mechanics-to-keep-a-gate-green`),
- rebaselines a hash its section said must not move,
- runs the same gate twice on an unchanged tree,
- papers over an extraction failure with a global (N5's kill criterion).

Two package-specific things the plan under-weights, and you should watch for:

- **N2 is the feel risk.** Every movement smoothing constant in `player.cpp` was
  tuned against variable frame dt and will behave differently at a fixed 33 ms.
  `StrikePicker::Feed` (`strike_pick.h`) currently smooths mouse pixels at frame
  rate; at 30 Hz it sees fewer, fatter deltas, so melee flick classification can
  change even with total motion preserved. Require the agent to report on flick
  behaviour explicitly, not just on the gate.
- **N1's K is a gameplay number, not just a plumbing one.** At K=4 every
  snapshot-reading decision (brush placement, mob ground probe, spell ladder)
  reads a world 133 ms old. The plan's kill criterion only measures wait counts.
  Prefer the smallest K whose wait counter is ≈0 over defaulting to 4, and have
  the agent report the histogram either way.

## 5. Endgame — replaces plan §0.7

On `mp-now` after the last merge:

1. `bash scripts/build.sh`
2. **One** `bash scripts/run.sh ./build/Release/sandvox.exe --verify
   determinism,snapshot-latency,tick-input,ops-replay,terrain,streaming,mob-burn`
   (use real gate names from `--selftest --list`; one boot, results in
   `build/last_run.json`).
3. Rebaseline **only** because N1 and N4 moved the hash on purpose:
   `--selftest --rebaseline`, then `--vk-smoke --rebaseline` and
   `--vk-smoke-loud --rebaseline` (N4's landform bake is a worldgen content move,
   which re-pins the smoke tables; `--selftest --rebaseline` does not).
   Do not investigate the new numbers. A moved hash you caused on purpose costs
   exactly one command.
4. **No `--suite acceptance`.** If you believe one is warranted, ask the user
   first and say what claim it would establish that step 2 did not.
5. Fast-forward `main`. Update `ARCH_NODES` in `assets/tuner.html` (a
   "Multiplayer readiness" node, `wip` off, plus the recently-landed list).
   `bash scripts/board.sh done "..."`.
6. Write memories as packages land — wave status, measured numbers, what was
   refuted. Format: the existing `project-*` memories.

## 6. Stop conditions

- A package that cannot meet its kill criterion after two agent iterations is
  **reported to the user, not forced**.
- If N1 cannot find a constant K under `kPagedSnapshotMaxGap` with a ≈0 wait
  counter, that is a design question for the user. Deepening the ring is allowed
  (kilobytes per slot); raising the gap is not yours to decide.
- If N4's integer landform bake moves more than ~0.1% of landform bytes by more
  than 1, stop: the float bake was never what the map author saw, and the user
  decides.
- If N5's `cmp` of the two recordings differs, the recorder's frame header names
  the first differing tick and op index. That is a bug to fix, not a tolerance
  to widen.
