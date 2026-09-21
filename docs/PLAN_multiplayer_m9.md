# PLAN: M9 — two independent players in one world

**Written:** 2026-09-20 against `54fe241` (main), the commit that landed
`docs/PLAN_multiplayer_now.md` N1–N6. **Source audit:**
`docs/RESEARCH_multiplayer_readiness.md` (finding ids L*/S*/A*/X* below refer to
its §2). **Model of record:** DESIGN.md §10. **Status:** plan of record for M9.
One orchestrator (Fable 5.1) reviews, sequences and merges; **Opus 5 worktree
agents implement**, one per package; integration branch `mp-two` (off `54fe241`),
fast-forwarded to `main` at the end of each stage.

## Why this plan exists, in one paragraph

N1–N6 removed the debt; nothing in the tree opens a game socket and no second
player can exist: one alias-bound `PlayerSession` in `main()`, one avatar id
(`PlayerAvatar() { id_ = 0x5A11EDU; }`), one physics proxy singleton
(`Physics::playerBody_`), one function-static (`wasInLiquid`, `session.cpp:292`),
one CPU mirror cube, and `Stream::Update` ignores every `InterestSet` entry but
`Primary()`. The thing DESIGN.md §10 under-states is the size of the residency
window: `kWorldN = 512` at `kVoxelMeters = 0.10` is a **51.2 m cube**. Two players
walking independently leave each other's window in seconds, so the host cannot
simulate the remote player's surroundings (no window there, no mirror, and
`kTicketMax = 0`). Therefore **each machine is authoritative for its own player's
controller and for the chunks and entities nearest its own player** — the
"nearest player under distributed authority, later" of the audit's §5, and
"later" is now. The host's remaining jobs are the tick pacing, relay, persistence
and lobby. This also answers S7: no second host window and no ticket slots for two
players; chunk tickets stay at P0.

## The model, stated for two players

| Concern | Owner | On the wire |
|---|---|---|
| Tick pacing | delayed lockstep, D = 4 ticks | one `TickBatch` per direction per tick (§4 finding 5) |
| Player P's controller, brush, spells, melee | P's machine | nothing — its RESULTS cross |
| Player P's pose | P's machine | `PlayerState` every tick (~64 B) incl. `windowOrigin`; peer drives a ghost `PlayerAvatar` from it |
| World ops | authoring machine | in the `TickBatch`, tagged `(author, tick+D)`; both sides apply at T+D in canonical order; receiver drops ops for chunks it does not hold |
| CA on chunk C | every machine with C resident, identically (rule 1) | nothing — determinism is the transport; drift is detected by per-chunk hash and corrected by re-send from C's authority |
| Mobs, debris bodies (M9.4) | nearest RESIDENT player's machine | `EntityState` per tick; the per-mob `SaveState` record on handoff |
| Modified chunks a machine evicts (M9.5) | host `ChunkStore` | `ChunkPut` by the authority; manifest + `ChunkGet` before regenerating |
| Build/asset identity | handshake | `StampTuning`, `EnvironmentStamp`, `opstream::Header` fields, `kTickInputVersion`, seed |

`TickInput` never goes on the wire; it stays the record/replay/gate unit. The
network carries OUTCOMES. Overlapping windows (the players meet) is the only time
the op stream and the chunk hash matter, and that is the "shared edits" milestone.

## Stages

| Stage | Name | Size | Lets you… |
|---|---|---|---|
| **M9.1** | Two `PlayerSession`s in one process (packages P1–P3, §2) | M | build every later stage against a real N-player tick |
| M9.2 | `net::Link` (TCP + loopback), handshake, tick pacing, ghost players | M | walk two windows and see each other |
| M9.3 | Op exchange + per-chunk hash + chunk resync | M | share edits: the "play together" milestone |
| M9.4 | Entity ownership: mobs/debris nearest-player, handoff | L | fight together |
| M9.5 | Chunk persistence through the host, late join, disconnect | M | leave and come back to each other's edits |

User decisions (2026-09-20): transport = LAN / direct-IP TCP, no library; first
"play together" milestone = walk, see each other, shared world edits; mobs after.

---

## 0. Orders for the orchestrator

1. Read this file, `CLAUDE.md` "Build and verify", `bash scripts/board.sh active`,
   `git status --short`. Stale claims (days old, landed) are abandoned: note and
   proceed.
2. `mp-two` is branched from `54fe241`. Packages merge into it; it fast-forwards
   `main` once per stage.
3. **Before any M9.1 package merges,** record the oracle on main's exe TWICE and
   `cmp` the two (the control arm; N5 needed `SANDVOX_TICKS_PER_FRAME` for it):
   ```bash
   export SANDVOX_NO_CRASH_DIALOG=1 SANDVOX_TICKS_PER_FRAME=1 SANDVOX_FRAMES_NO_RELOAD=1
   bash scripts/run.sh ./build/Release/sandvox.exe --frames 600 --autofly-hard --record-ops build/pre_a.svops
   ```
4. Launch each package as one `Agent` call: `subagent_type: "general-purpose"`,
   `model: "opus"`, `isolation: "worktree"`; the prompt is §1 verbatim, then the
   package section verbatim, then §5. Do not paraphrase a package.
5. Review the DIFF, then the report. Reject a package that adds a mechanic to keep
   a gate green, rebaselines a hash its section said must not move, runs a gate
   twice on an unchanged tree, or papers over an extraction failure with a global.
6. Endgame per stage: build `mp-two` once, ONE `--verify` with the stage's gate
   list, rebaseline ONLY if a package said the hash may move, ff `main`, update
   `ARCH_NODES` in `assets/tuner.html` (the `mpReadiness` node's "still
   single-player" paragraph and a Recently Landed entry), `board.sh done`, memory.
   **No `--suite acceptance`.**

## 1. Rules for every agent (prepended to every package prompt)

- You are in a git worktree off `mp-two`. Create branch `m9-<package>` from it.
  Commit on that branch; report the commit hashes. Do not touch `main`.
- **Re-derive every `:NNNN` line anchor by grep before acting on it.** Anchors
  were verified at `54fe241`; symbol names are the truth.
- Read `CLAUDE.md` in full before building. Then: `bash scripts/build.sh` ONCE
  (sccache shares objects; only your TUs compile). WGSL/JSON edits need no build.
  Every run goes through `bash scripts/run.sh` (absolute path to the main
  checkout's script is fine). `export SANDVOX_NO_CRASH_DIALOG=1`. Read
  `crash.log` after a crash and check its frames name YOUR worktree's paths
  (CLAUDE.md "sibling worktree's objects").
- Verification is a budget. One `--gate` while iterating; ONE `--verify` at the
  end with your section's list. Never the same gate twice on an unchanged tree.
- The three inviolable rules bind you: integer-only deterministic CA; cost scales
  with activity; every mutation is an op. Plus DESIGN.md §10's two rules: no
  sim-affecting process global keyed on the window origin; no gameplay decision
  reads the snapshot outside the fixed-latency path.
- Hash: your section says whether the `determinismHash` may move. If it says NO
  and it moved, that is a bug to find, not a number to re-pin. If it says MAY and
  it moved, `--selftest --rebaseline` once at your final commit and do not
  investigate.
- Write high-density comments where the code says something non-obvious
  (this repo's style: the WHY, the measured number, the bug it prevents).
- Post `bash scripts/board.sh claim "<files>" "<what>"` before editing and
  `board.sh done "<what landed>"` when you stop. Never edit the board by hand.
- Report in the §5 format. The report is read AFTER the diff.

---

## 2. Stage M9.1 — two players in one process

**Goal.** `TickAuthority` takes a LIST of sessions; the world half runs once, the
player half runs per session, in an order byte-identical to today's for one
session. Every singleton that assumed one player becomes per-player. A gate walks
two scripted players in one window and pins both trajectories.

```
Wave 1 (parallel, disjoint files)                                       STATUS 2026-09-21
  P1  TickAuthority phase split          session.h/.cpp, main.cpp (tiny)    LANDED 108f82d: oracle cmp identical, hash unmoved
  P2  per-player singletons              physics.*, avatar.h, mob.h/.cpp,
                                         ai_behavior.h                      LANDED be0814b: hash unmoved (no tie-break saw the band)
Wave 2 (after P1+P2 merged into mp-two, 56a9f87)               P3 LANDED 452e77e: gate green, hash unmoved, fallback retires at tick 0
  P3  KindAtCached + gate two-players    world.h/.cpp, selftest_player.cpp,
                                         selftest.cpp (kOrder), baseline.json   hash UNMOVED
Endgame: build mp-two once; --verify determinism,two-players,tick-input,mob-burn,ops-replay;
         post-refactor oracle cmp; rebaseline iff P2 moved the hash; ff main; ARCH_NODES; DESIGN §10.
```

### P1 · `TickAuthority` becomes N-player (phase split) — C++ only, hash UNMOVED, proven by the op-record oracle

> **LANDED 2026-09-21 (`108f82d`, merged `2285920`) — with a correction to the
> table below.** The nine-phase table was WRONG: at `54fe241` the body
> alternates player/world work thirteen times before the submit (the laser ray
> runs after the window shift; the dev panel's spawn runs before the brush;
> `ui.aiApplyBehavior` → `mobs.SetMobBehavior` at `:895-903` was missing from
> the table entirely), so merging the player runs would have reordered the op
> stream. The agent split at the file's real segment boundaries instead:
> **sixteen phases** `A* B C* D E* F G* H I* J K* L M* N O* P` (`*` = once per
> session, in index order), and `TickAuthority` is exactly their alternation.
> For one session the concatenation is the old body line for line, which is
> why the oracle matched by construction. The mapping from the plan's letters:
> plan A = A,C,E,G · plan B = B,D,H · plan C = I · plan D = J · plan E = K ·
> plan F = L · plan G = M · plan H = N · plan I = O; F is the omitted world
> block, P the perf tail. A session with `localView = false` binds the
> presentation seam to a `PresentationSink` (bound once by name, not guarded at
> forty sites) and auto-revives. `mobs.SetAvatars` is still the one-time
> `main.cpp` registration — a second session's avatar enters the mob lookup
> tables only when its constructor site registers it (P3's gate does; M9.2's
> ghost list will). Keep the table below as the ORIGINAL spec for the record.

**Owner scope:** `src/game/session.h`, `src/game/session.cpp`, `src/main.cpp`
(only the `TickAuthority` call at `:8481` and the `session.index/localView`
init beside `PlayerSession session;` at `:5643`). Nothing else. Do NOT touch
`mob.*`, `physics.*`, `avatar.h` (P2 owns them) or `world.*` (P3).

**Verified facts (at `54fe241`):**
- `TickAuthority(TickAuthorityCtx& w, PlayerSession& s, const FrameIntent&,
  const TickInput&, uint32_t tick, OpBatch& out)` — `session.cpp:130`, decl
  `session.h:477`. Alias unpack `:137-244`, then one brace block to the end.
- Block map, with scope (P = reads/writes `s`; W = world):

  | Phase | Blocks (today's lines) | Scope |
  |---|---|---|
  | A | controller + look feed + strike quantize (:245-360); laser (:442-535); wardrobe (:795-893); sphere (:905-996); fluid pour (:998-1057); brush + prefab (:1059-1085) | per player |
  | B | far-plume eye (:371-374) and interest set (:375-386) from ALL sessions, primary 0; stream/far (:387-432); dev spawn/AI panel (:537-794); day phase (:1087-1100); `SetPlayerActors(all)` (:1102-1110); `mobs.PreTick` (:1114) | world |
  | C | avatar block incl. death/respawn (:1116-1604); magic (:1606-2006) | per player |
  | D | `fellTree` hook (:2008); `debris.QueueSupportEvents/PreTick` (:2013-2018); fluid lab (:2019-2033) | world |
  | E | laser kerf (:2034-2047); sword bite (:2048-2249); `placer.PreTick` (:2251); explosions incl. grenades (:2254-2334) | per player; `ui.pendingDetonate` fires once, with session 0 |
  | F | dirty marks / particlesActive / celestial / edit layer (:2335-2384, minus `pc`) | world |
  | G | `MovePlayerBody` (:2385), `grab.Tick` (:2398), wards `spells.FilterStreams` (:2400-2407), swimmer wake (:2408-2436) | per player, session order |
  | H | `SubmitTick` with `pc` = session 0's chunk (:2437-2464); `phys.Step`, `debris.PostStep`, `mobs.PostStep` (:2465-2468) | world |
  | I | `avatar.PostStep` (:2469), ragdoll follow (:2470-2485), push-out (:2486-2490) | per player |

  Blocks :433-440 (`spanGame`, brush radius/material from `ui`) are per player
  and precede A's tools. Perf accounting :2491-2516 stays at the end.
- The ONE static in the file: `static bool wasInLiquid` at `session.cpp:292`.
- `mobs.SetPlayerActor(...)` at `:1108` is the span-of-one wrapper of
  `MobSystem::SetPlayerActors(std::span<const PlayerActorDesc>)` (`mob.h:3608`);
  `mobs.SetAvatar(&avatar)` is called once in `main.cpp:5663` and has a span
  form `SetAvatars` (`mob.h:3644`). You may CALL the span forms; you may not
  edit `mob.*`.
- `InterestSet` (`src/sim/interest.h`): `chunks` + `primary`; `Stream::Update`
  and `FarField::Update` consume only `Primary()`. Extra entries are information.
- The one-session op record must be byte-identical across this package. The
  orchestrator recorded `build/pre_a.svops` on main's exe with
  `SANDVOX_TICKS_PER_FRAME=1 SANDVOX_FRAMES_NO_RELOAD=1 --frames 600 --autofly-hard`.

**Build:**
1. In `session.h`: `struct SessionTick { PlayerSession* s; FrameIntent intent;
   TickInput ti; };` and
   `void TickAuthority(TickAuthorityCtx& w, std::span<SessionTick> players,
   uint32_t tick, OpBatch& out);`. Keep the existing one-player signature as an
   inline wrapper that builds a one-element array. `PlayerSession` gains
   `int index = 0;`, `bool localView = true;` (session 0 = the window's; the
   presentation seam — `ui.deathScreen`, `ui.respawnRequest`, `deathBody`,
   `deathFrozen`, `ui.spellRefused`, hit-stop requests, combat cue requests —
   is written only when `localView`; a non-view session auto-revives after
   `av.respawnDelay` and its cues are dropped), and `bool wasInLiquid = false;`
   (replaces the static).
2. In `session.cpp`: nine `static void PhaseA..I(...)` functions taking
   `TickAuthorityCtx&` plus either one `SessionTick&` or the span; the alias
   unpack becomes per-function (a small `struct WorldRefs`/`struct PlayerRefs`
   built once per call is acceptable). `TickAuthority` is then the loop:
   `A per s; B; C per s; D; E per s; F; G per s; H; I per s`. Op push ORDER for
   one session is unchanged by construction — check it against the table.
3. Phase B builds the `InterestSet` from every session's chunk, `primary = 0`,
   calls `mobs.SetPlayerActors(span)` with one `PlayerActorDesc` per session
   (index order), and uses session 0 for the far-plume eye. Phase H uses
   session 0's `pc`. Document both as "the window follows the primary".
4. `main.cpp`: set `session.index = 0; session.localView = true;` beside the
   declaration; the call at `:8481` may stay on the wrapper. Nothing else.
5. Comment block at the top of `session.h` extended: what a phase is, why the
   order is what it is, which phases a remote ghost (M9.2) plugs into (B and C).

**Kill criterion:** if the split cannot be made byte-identical in two
iterations, report the first differing tick and op index from the record's
frame header. Do not paper over with a global or a static.

**Done means:** "a second `PlayerSession` in the span runs a full tick with no
global and no static touched" and the one-session record is byte-identical.

**Verification, ONE launch at the end:** build; record
`SANDVOX_TICKS_PER_FRAME=1 SANDVOX_FRAMES_NO_RELOAD=1
bash scripts/run.sh <your exe> --frames 600 --autofly-hard --record-ops build/post_p1.svops` (argv, not the env var: the env var arms only the selftest); `cmp` against the
orchestrator's `build/pre_a.svops` (path in the main checkout); then
`--verify determinism,tick-input,mob-burn,ops-replay`. Hash: UNMOVED.

### P2 · The one-player singletons — C++ only, hash MAY move once (actor id band)

**Owner scope:** `src/phys/physics.h/.cpp`, `src/game/avatar.h` (ctor only),
`src/game/mob.h/.cpp` (`SetPlayerActors`, `FindCombatantById`, comments),
`src/game/ai_behavior.h` (`kPlayerActorId`) and every consumer of these symbols
outside `session.*` (grep). Do NOT touch `session.*` or `main.cpp` beyond the
`PlayerAvatar` construction if the ctor change forces it (it should not: keep the
default argument).

**Verified facts:**
- `PlayerAvatar() { id_ = 0x5A11EDU; }` — `avatar.h:122`. The id seeds gore
  variance (`Hash3(id ^ salt, tick, i)`), so session 0 must keep it.
- `Physics::playerBody_` — `physics.h:571`; set `physics.cpp:1260`, read
  `:1975` (`ReleaseToWorldWhenClear`) and `:1997` (`TickPendingReleases`),
  cleared `:2108`, reset `:329`. `CreatePlayerBody(halfXZ, halfY)` `physics.h:430`.
- `MobSystem::SetPlayerActors` — `mob.cpp:2401-2420`: `a.id = (uint64_t)i`; the
  comment there says a second session collides with mob id 1 TODAY.
  `FindCombatantById` `mob.cpp:2595-2599`: `if (id < avatars_.size()) return
  avatars_[id];`. `ai::kPlayerActorId = 0` — `ai_behavior.h:367`. Mob ids from
  `nextId_ = 1` (`mob.h:4871`, `mob.cpp:2628`); saves restore the counter via
  `SetNextIdCounter` (`mob.h:4151`) — do NOT change mob id allocation (save
  format).
- Player actor ids are not saved (the AI brain is not in `SaveState`).

**Build:**
1. `explicit PlayerAvatar(uint64_t id = 0x5A11EDU) { id_ = id; }`. Session i
   will pass `0x5A11EDU + i` (P1/P3 do the passing; you provide the ctor).
2. `playerBody_` → `std::vector<uint64_t> playerBodies_`: `CreatePlayerBody`
   pushes; `RemoveBody` erases; `ReleaseToWorldWhenClear` and
   `TickPendingReleases` test EVERY proxy (a piece is released only when clear
   of all of them). One body ⇒ identical behaviour: say so in the comment.
3. Actor id band: `ai::kPlayerActorBase = 1ull << 62`, `kPlayerActorId =
   kPlayerActorBase` (player 0), player i = base + i. `SetPlayerActors` assigns
   it; `FindCombatantById` maps `id >= base && id - base < avatars_.size()` →
   `avatars_[id - base]`, else `FindMobById`. Grep every use of
   `kPlayerActorId`, `avatars_.size()`, `IsAvatar`, `AvatarById` and any `id ==
   0` that means "the player" (`ai_behavior.cpp`, `ai_nav.cpp`, `mob.cpp`,
   `strokes.cpp`, `melee.cpp`) and route them through the band. Keep
   `kPlayerActorId` as the symbol everybody compares against.
4. Rewrite the `mob.cpp:2406-2411` comment and the stale `mob.h:3599` line
   ("mob ids still start at kMaxPlayers" — false; there is no `kMaxPlayers`).

**Kill criterion:** none expected. If a gate other than `determinism` changes
verdict, the band leaked into a decision by VALUE (a `<` on ids, X3): report
which gate and which comparison; fix the comparison, do not rebaseline a
verdict.

**Done means:** two `PlayerAvatar`s with distinct ids, two proxies and two actor
ids can coexist; `grep -n "playerBody_" src/` is empty; `grep -n "== 0" src/game/ai_*.cpp`
shows no player-id test by literal.

**Verification, ONE launch:** `--verify determinism,mob-burn,npc-strike,crowd,
corpse-armor,ragdoll-dress` (use real names from `--selftest --list`; the
last four exercise proxies, targeting and ids). Hash: MAY move (id band in a
tie-break) → `--selftest --rebaseline` once at your final commit; say whether
it moved.

### P3 · A collision source outside the mirror, and gate `two-players` — C++ only, hash UNMOVED

**Owner scope:** `src/sim/world.h/.cpp` (`KindAtCached` beside `KindAt`),
`src/game/session.h` (`PrefetchAround` declaration only — a 10-line inline is
fine; P1 has merged by the time you branch), `src/test/selftest_player.cpp`,
`src/test/selftest.cpp` (`kOrder` only), `tests/baseline.json` (new keys).
Branch from `mp-two` AFTER P1 and P2 have merged (the orchestrator tells you).

**Verified facts:**
- `World::KindAt(IVec3, classOf)` — `world.cpp:997-1022`: `Unknown` with no
  snapshot; `Solid` outside the window (`:999-1005`); `Unknown` outside the
  3×3×3 mirror (`:1006-1010`); else the class. `Unknown` is PASSABLE for the
  player controller's sweeps? — check `player.cpp` `Collides`; the engine map
  says "Unknown mirror cells are SOLID for the player" — verify which and
  design to the truth.
- Fetch cache: `RequestChunkFetch(IVec3 wc, FetchSource)` `world.h:3617`,
  `Cached(IVec3 wc) -> const CachedChunk*` `world.h:3649`, refuses non-resident
  (`world.cpp:379-382`), 64 chunks/tick, lands 1–2 ticks + K later. Callers'
  pattern: `mob.cpp:2974-2977`. `FetchSource::Mob` exists (`world.h:3608-3615`).
- Analytic column: `World::TerrainHeight(seed, x, z)` (static, `world.h:3827+`).
- `tick-input` gate — `selftest_player.cpp:891-1112`: `TickInputScriptBlock`,
  `RunTickInputArm`, `TickInputFeeder` pattern, block-scripted commands
  (`kTicksPerBlock = 4`), `RecordObserved`/`BaselineNumber` pins. It uses a
  synthetic `kindAt` and no world. Your gate needs the world: copy the fixture
  shape of a mob gate that does worldgen (`selftest_mob.cpp`, `SubmitWorldgen`
  + `SubmitTick` loop + `mobs.PreTick`), not `tick-input`'s.
- Gate registration: function + entry in `PlayerGates()` + name in `kOrder`
  (`selftest.cpp:77+`); a new observed key must be seeded into
  `tests/baseline.json` by hand once or `--rebaseline` writes nothing
  (`selftest.cpp:~785`).
- `MobSystem::SetAvatars(span)`, `SetPlayerActors(span)`; after P2, player i's
  actor id is `ai::kPlayerActorBase + i`.

**Build:**
1. `CellKind World::KindAtCached(IVec3 cell, const std::vector<uint32_t>& classOf)`:
   Solid outside the window; `Cached(chunk)` hit → the same class mapping
   `KindAt` uses (factor it, do not copy it); miss → `RequestChunkFetch(chunk,
   FetchSource::Mob)` and return the analytic answer: Solid at/below
   `TerrainHeight`, Air above. Fluid: same `FluidEighthsAt` fold `main.cpp:6428`
   applies — check whether `fluidMirror` is mirror-bounded; if it is, the
   cached path answers Air for fluid and the comment says so.
2. `PlayerSession::PrefetchAround(World&)`: request the 27 chunks around
   `player.pos` (cheap: refused/duplicate requests are counted, not queued twice
   — verify in `RequestChunkFetch`). Called by P1's phase A for sessions with
   `index > 0`. (If P1's phase A has no such hook, add the one line there —
   it is the one exception to "do not touch session.cpp".)
3. Gate `two-players` in `selftest_player.cpp`: worldgen at a harness site;
   two `Player`s at ground level 12 chunks apart in X, both inside the window;
   two `PlayerAvatar`s (`0x5A11EDU`, `0x5A11EDU + 1`), `Init/SetDefs/Spawn`,
   `mobs.SetAvatars({&a0,&a1})`; player 0's `KindFn` = `KindAt` with the
   mirror centred on it (`SubmitTick`'s `playerChunk`), player 1's =
   `KindAtCached` + `PrefetchAround`; 300 ticks of block-scripted `TickInput`
   (player 0 walks +X, player 1 walks −X, both jump once), per tick:
   `p.Update(kTickDt, ti, kindAt)` each, `SetPlayerActors` ×2,
   `avatar.PreTick` each, `mobs.PreTick`, `SubmitTick`, `PostStep`s. Asserts:
   - both end positions pinned (`twoPlayers.p0EndX/Y/Z`, `p1EndX/Y/Z`,
     tolerance key `twoPlayers.endTolVox` = 0.05);
   - player 1's `pos.y` never drops below `TerrainHeight − 1` at its column
     (no fall-through on a cache miss);
   - one hostile mob spawned midway targets the nearer actor:
     `FindCombatantById(ai::kPlayerActorBase + 1)` is avatar 1 and the mob's
     target id after 60 ticks is the nearer one;
   - the whole 300 ticks run TWICE; final hashes equal (twice-run contract).
   Record byte-size of nothing; record the fetch counts (`FetchReport`) as
   informational observed values.

**Kill criterion:** if player 1 falls through on a cache miss even with the
analytic fallback, the fallback is wrong for the site (a cave or an overhang):
move the site, do not widen the tolerance.

**Done means:** a player anywhere in the window has a collision source, and the
`two-players` gate walks two of them deterministically.

**Verification, ONE launch:** `--verify determinism,two-players,tick-input,
streaming`. Hash: UNMOVED (the gate is new state, but `determinism` runs first
in `kOrder` — confirm placement).

---

## 3. Stages M9.2–M9.5 (package sections written when each stage starts)

### M9.2 — transport, handshake, tick pacing, ghost players
- `src/net/link.h/.cpp`: `class Link { Listen(port) / Connect(ip,port), Poll(),
  Send(type, bytes), bool Recv(Msg&) }` over non-blocking Winsock TCP polled
  once per frame before the tick loop — `src/telemetry.cpp`'s pattern
  (ws2_32 already linked, `CMakeLists.txt:587`) plus `TCP_NODELAY`. Frame
  `{u32 len, u16 type, u16 ver, payload}` via `ByteWriter/ByteReader`
  (`src/sim/bytestream.h`). `LoopbackLink` pair for gates.
- Handshake `Hello`: `opstream::Header` fields + `StampTuning` +
  `EnvironmentStamp` + `kTickInputVersion` + protocol version + seed
  (`kDefaultSeed` today — becomes a value the host sends). Mismatch refuses
  naming the field (worldio's style).
- Pacing: ONE `TickBatch{tick, PlayerState, windowOrigin, ops (empty until
  M9.3), timeScale, vizActive}` per direction per tick: sent BEFORE waiting,
  empty every tick, D=4 pre-sent at connect (§4 finding 5). Frame loop gains
  a third pacing case beside `HarnessTicksPerFrame()` (`main.cpp:8216`): a
  tick runs only when the peer's batch for it has arrived; while stalled skip
  the tick, keep rendering, poll every frame; 3 s silence → free-run. The
  host's `timeScale`/`vizActive` win (L11).
- `RemotePlayer { id, PlayerState last, Player ghostCtl, PlayerAvatar avatar,
  uint64_t proxyBody }` list on `TickAuthorityCtx`; phase B registers
  `SetAvatars(sessions + ghosts)` / `SetPlayerActors(…)`; phase C runs
  `ghost.avatar.PreTick(tick, ghost.ghostCtl, heading, …)` with the one-tick
  edges (`jumped`, `impactDeltaV`) drained as `session.cpp:1599-1603` does.
  `PreTick` reads ~15 plain `Player` fields and no `Camera` (verified).
  Ghosts add a chunk to the `InterestSet` as INFORMATION.
- argv `--host [port]`, `--join ip[:port]`. Gate `net-loopback`: link
  round-trip, handshake refusal matrix, pacing catch-up. The live check is two
  exes on one machine (they share the GPU; the frame rate halves; that is a
  smoke, not a number to quote).

### M9.3 — op exchange, per-chunk hash, chunk resync (the "play together" milestone)
- `sim_occupancy.wgsl:281-300` also stores `wgHash` into a new
  `chunkHash[kNumChunks]` buffer; rides the snapshot ring at the `hashEnable`
  cadence; `World::ChunkHash(slot)`. Pass table + `check_pass_table.py`. World
  hash unchanged. On the wire a two-level Merkle (512 block hashes of 4³
  chunks ≈ 2 KiB; drill down on mismatch), finding 8.
- Op exchange: phase H submits `own(T) ∪ remote(T)` merged in canonical order
  (author id, then push order) at T; every machine defers its own batch by
  D=4. The batch carries the sender's window origin; the receiver
  reconstructs wc per op (CellOps are SLOT-indexed, finding 3) and drops ops
  for chunks it does not hold. `OpMeta.producer` becomes mandatory with a
  debug gate at `SubmitTick` asserting `Authority::Owns(producer, wcOf(op))`
  (finding 4).
- Comparability + resync (findings 1–2): compare only chunks resident on both
  AND ≥2 chunks inside both windows (origins as of T−K from a K-deep ring)
  AND dirty-clear ≥ K+D ticks on the authority. Mismatch → the authority
  sends `ChunkSync{tick, wc, rle}` in ≤4 KiB slices; the receiver applies it
  through `Stream`'s RLE→slot refill upload inside the tick, flushing
  fluid/gas/particles in the chunk's bounds, recorded as a new `oprecord`
  frame kind so `ops-replay` still reproduces. Rate-limited, nearest first.
- Gate `ops-exchange`: two op streams merged through `LoopbackLink` replay to
  the same hash as one merged record (`ops-replay` fixture reused).

### M9.4 — entity ownership (mobs, debris)
- Chunk authority = nearest among machines whose SENT window contains C with
  margin ≥1, at T−D, ties → lower id, 2-chunk hysteresis (finding 6); a pure
  function of both `PlayerState`s. Gate `QueueSupportEvents`
  (`debris.cpp:743-747`) and `MobSystem::PreTick` STEPPING by `IsMine(wc)`.
- `EntityState` per owned mob/body within the peer's interest radius; ghost
  mobs = `Mob` with no brain, kinematic follow. Handoff payload = the per-mob
  record inside `MobSystem::SaveState` (`mob.cpp:16494-16513`) extracted as
  `Mob::SaveOne/LoadOne` + equipment by name + brain target.
- Needs items keyed on game ids (A2) for held/ground items.

### M9.5 — persistence through the host, late join, disconnect
- `ChunkPut` on `Stream::EvictSlots` of a modified chunk (`stream.cpp:394-397`
  is where "modified" is known) by the AUTHORITY only, tick-tagged, acked,
  re-offered on rejoin (finding 7); host publishes a stored-chunk MANIFEST; a
  refill consults it before regenerating; a slot awaiting a `ChunkGet` stays
  inert until the answer lands; the host answers from `ChunkStore` or
  forwards to the machine that has it resident (the fetch cache).
- Late join = handshake + `ChunkGet` for the joiner's window; disconnect =
  host keeps the peer's last pushed chunks; peer-owned entities revert to host
  authority (M9.4's handoff without a reply). `meta.svm` gains tick + seed (L8).

---

## 4. Protocol review findings (adversarial critique, 2026-09-20)

1. **Overlap is never geometrically bit-identical** (boundary chunks see SOLID
   on one side and real neighbours on the other; fluid/gas/particles are
   unhashed and die at an edge). Treat hash mismatch as convergence work.
   Comparable(C) = resident on both ∧ ≥2 chunks inside BOTH windows, origins as
   of T−K (`PlayerState` carries `windowOrigin`; both sides keep a K-deep ring).
2. **A resync payload is K ticks stale**; resyncing an ACTIVE chunk thrashes.
   Resync only dirty-clear ≥ K+D chunks; rate-limit; flush particles in bounds.
3. **Never fan a chunk out as CellOps.** `CellOp.cellIdx` is a SLOT index
   (`sim_mutate.wgsl:184`); a remote CellOp cannot be residency-checked and an
   aliased chunk (wc+32k) would be painted silently. Batches carry the sender's
   origin; chunk replace goes through the refill upload path.
4. **Single-producer rule**, gating points: island detection at
   `QueueSupportEvents`; mob STEPPING by feet chunk; debris landing by body
   owner; water-body ladder and global-RNG producers host-only. `OpMeta.producer`
   mandatory + a debug gate at `SubmitTick`.
5. **No deadlock:** send T+D before waiting for T; empty batch every tick
   ("no ops" ≠ "not arrived"); pre-send D empties; one `TickBatch` channel;
   `TCP_NODELAY`; 3 s silence → local authority; fix D=4.
6. **Authority must be resident:** nearest among machines whose SENT window
   contains C (Euclidean-nearest can name a machine without the chunk).
7. **Persistence:** authority-only `ChunkPut`, tick-tagged, acked; pending
   `ChunkGet` slots stay inert; a manifest instead of 1,024 `ChunkGet`s per
   plane shift.
8. **Bandwidth:** steady ≈150 B/tick/direction; Merkle instead of 128 KiB
   hash dumps; worst tick ≈ 64–128 KiB; `ChunkSync` in ≤4 KiB slices to
   avoid TCP head-of-line blocking.

**Recorded alternative, not taken:** tether both players inside ONE shared
window (host origin replicated, ~40 m leash). Removes findings 1–3 and 6–7
for a first milestone, contradicts the goal (independent walking), and would
be undone. It is the fallback if M9.3's convergence work proves unstable.

## 5. Report format (every agent, verbatim headings)

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
