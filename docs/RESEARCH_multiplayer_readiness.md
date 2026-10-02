# RESEARCH: multiplayer readiness — audit of the tree against DESIGN.md §10

**Written:** 2026-09-10, against `d910f6f` (main). Five parallel read-only audits
(tick loop, CPU gameplay, world/window structure, shader determinism, mutation
path + readbacks), one hand verification of the mutate-kernel race, and a survey
of how shipped games of this type do it. Companion plan of record:
`docs/PLAN_multiplayer_now.md` (the work that must happen before M9).

**Status (2026-10-02 audit):** §6 "Now" was built as `PLAN_multiplayer_now.md`
N1–N6 (main `54fe241`, 2026-09-20) and the core of §6 "Later" as
`PLAN_multiplayer_m9.md` M9.1–M9.5 (transport, op exchange, per-chunk hash +
resync, entity ownership, host chunk persistence/late join; 2026-09-21). The
finding tables below are the 2026-09-10 state; individual "Later" items
(hardening X1–X5, L9 hash over particle/fluid/gas buffers, Steam/WebRTC) were
not re-audited here.

**Verdict in one paragraph.** Nothing in the tree makes multiplayer impossible.
The foundational bets hold: the GPU CA is integer-only with counter RNG and an
order-independent hash, worldgen is a pure function of seed and world chunk,
every gameplay write is an op, Jolt is built deterministic and stepped inside
the fixed tick, and the device boots headless. Two things have drifted from
§10. First, a class of **gameplay decisions now branch on GPU-readback timing**
and on **per-frame input**, which makes the op stream non-replayable — and the
design doc's claim that the queue "is the replay log" is not implemented
anywhere. Second, the engine assumes **exactly one sim window**, which is the
only item that needs real design work. Pure lockstep (§10 Option A) is ruled out
by the infinite world; the model that fits the code and the comparables is a
**host-authoritative op stream over a deterministic client-side CA with chunk
authority and hash-triggered resync** (§5 below).

---

## 1. What holds (verified against source)

| Claim | Evidence |
|---|---|
| Sim kernels are integer-only, no CAS, no subgroups | `grep -ci subgroup` = 0 over all `sim_*.wgsl` + `worldgen.wgsl`; the only `atomicCompareExchangeWeak` in the tree is the render-only shadow cache in `raymarch.wgsl`. `sim.fluidExciteMode` defaults 0; no `atomicMode` symbol exists. |
| Every atomic in sim is a counter, an idempotent OR, an integer sum, or an `atomicMax` claim | `sim_compact.wgsl:29,46`, `sim_step.wgsl:159`, `sim_particle.wgsl:155,250,258`, `sim_fluid.wgsl:665`, `sim_fluid_seam.wgsl:1892`. None decide a winner by arrival order. |
| Colour lattice is in WORLD coords, so dirty-list order cannot leak | `sim_step.wgsl:1909-1913`; 54 passes = 27 colours × 2 substeps (`simulation.cpp:124-134`). |
| World hash is an order-independent sum, keyed on slot, stamp bits masked | `sim_occupancy.wgsl:241-292`; wrapping u32 sum of per-cell pcg. |
| `worldgen.wgsl` has zero `f32` tokens; keyed on `(seed ^ salt, worldX, worldZ)`; reads only `T.seed/genCount/origin/labMode/genDeferWake/farCount` | census of `T.*` in `worldgen.wgsl`; `T.origin` is used only to turn a slot into a world coord (`:4513,4825,5178`). CPU twin asserted in `world.cpp:775`. |
| Tuning floats reach shaders as compile-time `const` only, precision 9, classic locale | `tuning.cpp:170-183`; all ~40 fixed-point folds are module `const` (`sim_fluid.wgsl:98-253`, `sim_fluid_seam.wgsl:168-221`, `sim_step.wgsl:1675-1682`). |
| Rule 3 is real: debris, mob burn, gore, spells, edit layer all emit ops; microbody carve is render-only | `debris.cpp:946,1003,1007,1057,1829,1932`; `mob.cpp:6484`; `microbody.h:27-32`. Only CPU writes to `voxels`: worldgen/stream refill (sanctioned) and a page-fill sentinel (`pagetable.cpp:2029`). One dev bypass: `main.cpp:6392` "clear fluid" writes `fluidArgsStage` directly. |
| Gameplay is tick-stepped at constant dt | `mob.cpp:3015 dt = 1/30`, `avatar.PreTick(.., kTickDt)`, `melee.Update(kTickDt)`, `phys.Step(kTickDt)` at `main.cpp:7581`, spells have no `dt` at all. |
| Jolt: `CROSS_PLATFORM_DETERMINISTIC ON`, fixed step, no `/fp:fast` | `CMakeLists.txt:326-327,507,640`; `physics.cpp:300`. |
| No stateful RNG on a world-state path; mobs have stable ids | `rng.h:9-14` contract; every world-affecting draw is `Hash3(id ^ salt, tick, i)`. `mt19937` instances: night ambience (`main.cpp:4218`) and audio (`cues.h:282`) — sinks only. `Mob::id_` monotonic u64 (`mob.cpp:1560`). |
| Day/night, wind weather, water bodies, current prims all ride `TickParams`, never wall clock | `support.cpp:440-786` assembles it; no `std::chrono`/`glfwGetTime` inside the tick loop; `DayPhaseForTick` is pure integer (`world.h:158-166`). DESIGN's "wind will ride TickParams in phase 4" is stale — it does. |
| Headless device works | `GpuContext::Init` takes a null window (`context.cpp:64-107`); `--selftest` drives `SubmitTick` directly. |
| Save is an infinite-world region store; far edits survive | `worldio.h`, `chunkstore.h` (`r_x_y_z.svr`, LRU 64 regions), `FarEdits::RebuildFromStore`. |

---

## 2. Findings, ranked

Classes: **HARD** = needs design; **MEDIUM** = refactor inside the current
design; **EASY** = plumbing. "Breaks" says which model it hurts.

### 2.1 Structural — one window (HARD, both models)

| # | Finding | Where |
|---|---|---|
| S1 | `World` holds one `origin_`; every per-slot buffer is sized from `kNumChunks` at compile time; the toroidal mask can only produce that many slots. `voxels` is 512 MiB, within 8× of the 4 GiB single-binding ceiling. | `world.h:48-51, 1340-1364, 2746-2747` |
| S2 | The window follows the local player's chunk, one axis per tick with 2-chunk hysteresis. A recentre generates terrain, dirties chunks, and moves when a plane first acts — the hash is tick-deterministic only because the origin trajectory is a function of ONE player's path. | `main.cpp:6026-6036`, `stream.cpp:229-230, 625+` |
| S3 | The world hash covers the window only and keys on `slot = wc mod 32`. Two clients with different windows produce unrelated hashes; two world chunks 512 apart alias. It is a fine single-window desync detector and useless across players. | `sim_occupancy.wgsl:241-292` |
| S4 | `WaterBodies()` is a process global whose basin registry is a pure function of `(seed, window origin, tuning)`; it rebuilds on every shift. | `waterbody.h:407-421`, `waterbody.cpp:205-212, 785-809, 858` |
| S5 | The CPU mirror is one 27-chunk cube around one player. A second player outside it falls through the world; the player code says so in a `static_assert`. Mobs use the on-demand chunk cache instead, which refuses non-resident chunks. | `world.h:26, 2731-2740`, `player.cpp:11-19`, `world.cpp:330-331` |
| S6 | No interest-set abstraction: `Stream::Update(IVec3 playerChunk)` and `FarField::Update(IVec3)`. The only list-of-points in the engine is `DebrisSystem::AddTerrainAnchor`, which drives the CPU fetch cache only. | `stream.cpp:204`, `farfield.cpp:116`, `debris.h:95` |
| S7 | **The designed seam exists.** Chunk-ticket slot space landed at P0 with `kTicketMax = 0`; `cellResident` has the ticket branch; `FillSlots`/`EvictSlots` take arbitrary slot lists; `ChunkStore` is keyed by world coord. | `world.h:53-93`, `common.wgsl:3203-3206`, `docs/PLAN_chunk_tickets.md`, `docs/tickets_p0_audit.md` |

### 2.2 Lockstep / replay blockers (MEDIUM; the debt that compounds)

| # | Finding | Where |
|---|---|---|
| L1 | **Gameplay branches on readback timing.** The snapshot ring delivers when the fence retires; a tick with no free ring slot gets NO snapshot (`if (slot < 0) return false;`). Consumers that author or gate mutations: brush/prefab/laser/sphere/fill target cell from `snap.pick`; mob ground probe returns "no ground this tick" on a cache miss and feeds pathing; spell bolt collision falls through mirror → cache → analytic height; island detection runs on the tick the chunk readback happened to land and reads the neighbour ring stale on purpose; `windWake`, `particlesActive`, `fluidCount` read the snapshot. The headless harness forces a blocking drain (`SetHarnessSnapshotDrain`), so **the determinism gate never tests the shipped configuration**. | `world.cpp:376, 428-477`; `brush.cpp:9-14`; `mob.cpp:1836-1841, 1971`; `spell.cpp:2176-2190`; `debris.cpp:607-645, 1174-1181`; `support.cpp:564-567`; `main.cpp:7505-7507, 7573`; `support.h:169-176` |
| L1′ | The correct pattern already exists: the deferred shift wake has a CONSTANT latency `Stream::kWakeLatency = 4` and BLOCKS if the map is not ready rather than skipping. Water bodies were fixed the same way (`world.h:2005-2013` names the disease). | `stream.cpp:968-989`, `stream.h:168, 254, 318-326` |
| L2 | **No per-tick input command.** `PlayerInput` is "per-frame movement intent, filled from GLFW polling"; eight direct GLFW reads in `main.cpp`; held state is sampled once per frame and applied to 0–4 ticks; sticky latches were added ad hoc. | `player.h:8-16`, `main.cpp:5156-5163, 5822-5823, 5843-5854, 5940` |
| L3 | **Player controller integrates on variable frame dt** (clamped to 50 ms, not fixed), with `exp`/`pow` smoothing, then feeds Jolt via `MovePlayerBody`, which feeds settle-back cell ops. Ledge grab and the default melee `StrikePicker` also smooth on frame dt and select a strike index that authors a carve. | `main.cpp:5670, 7544, 4969, 5916`; `player.cpp:448-969`; `strike_pick.h:34-40`; `melee.cpp:1391-1395` |
| L4 | **No replay writer, no op-based save, no `--replay`.** The six per-tick op vectors are fixed-size PODs uploaded by `SubmitTick`; nothing serializes them. Save is a raw chunk snapshot; `meta.svm` records neither tick nor seed. DESIGN §2/§10's "the queue is the replay log" is aspirational. | `support.cpp:470-486, 771-786`; `worldio.cpp:112-168` |
| L5 | **Same-tick same-cell write race in `sim_mutate`.** Both entry points `voxStore` unconditionally; two overlapping brush ops with different materials, or two `CellOp`s on one cell, resolve by GPU scheduling. `sim_explode` already solves this with lowest-op-index-wins. No CPU dedupe across producers. Same-material paints are benign (palette variant keys on the cell). | `sim_mutate.wgsl:104, 146` vs `sim_explode.wgsl:14-15, 128-131` |
| L6 | **CPU floats on the hashed stream.** `WindWeather` uses libm `sin/cos/atan2`, quantised to Q16.16 onto `TickParams`; `windMode` defaults to 1. The world map's landform plane is baked with `double cos/sin` then `lround`. `FLUID_FOAM_DECAY` const-evals `exp(log())` in-process via Tint. Debris ejecta positions come from Jolt floats into `ParticleSpawn`. Same-binary clients are fine; cross-platform is not guaranteed. The wind fix is already written down in the header. | `wind.h:136-166, 179-200`; `worldmap.cpp:214, 247`; `sim_fluid.wgsl:185-187`; `avatar.cpp:128-140` |
| L7 | **Tick count is locally paced.** The accumulator drops surplus debt at 4 ticks; the GPU-lag throttle drops ticks when ≥2 maps are pending; hit-stop scales the fill rate with real dt. Tick CONTENT is unaffected; the tick↔wall-clock map is machine- and combat-dependent. No external tick authority exists. | `main.cpp:5811-5820, 5943, 5968-5982, 5790` |
| L8 | **No session identity for tuning.** `CurrentTuning()`, materials and reactions are process globals that reach the hash; nothing hashes them. `biomes::EnvironmentStamp` does exactly this for worldgen assets and is the template. | `tuning.cpp:199`, `biomes.h:264-271`, `main.cpp:3625-3627` |
| L9 | **Hash coverage.** Voxels only — not MPM particles, ballistic particles, gas parcels, the water ledger. A particle-state divergence is invisible until it lands. | `sim_occupancy.wgsl` |
| L10 | Two writers of `world.tickUBO`: `Stream` builds its own default `TickParams` for `EncodeGenList` and submits it inside the tick. A replicated "tick input stream" must account for both. | `stream.cpp:795-806` |
| L11 | Dev UI on the sim path: `vizActive` from a checkbox; `Celestial().SetScale(ui.timeScale)` changes `dayPhase` and therefore the hash (documented as intended). Both must be replicated or client-local. | `main.cpp:7557, 7532`; `world.h:191-193` |

### 2.3 Server-authoritative gaps (MEDIUM/EASY)

| # | Finding | Where |
|---|---|---|
| A1 | Ops are anonymous: no author, no sequence. Identity is `(tick, buffer index)` implicitly. `ExplosionOp.pad0..2` are free; `BrushOp._p0/_p1` are TAKEN by spell transmute; `CellOp` is full. `WindPrim::ownerId` is the one entity-scoped id on the stream — the pattern to copy. | `world.h:337-357, 630-641`; `spell.cpp:1618-1620`; `windprim.h:96-99` |
| A2 | Ground items are keyed on Jolt `BodyID` (index + sequence from Jolt's free list), not a game-authored id. | `worlditems.h:45-47`, `physics.cpp:120-121` |
| A3 | No `Session`/`Game` object. `main()` spans lines 2810–10198; camera, player, brush, placer, avatar, third-person rig, spells, caster, health, melee, grenades are locals. `MobSystem` holds one `avatar_` pointer and a reserved `playerActor_.id = 0`. The `PLYR` save section exists because "there is no object that owns all of it". | `main.cpp:4188-4330`; `mob.h:1713-1736, 2488-2502`; `persist.h:46-57` |
| A4 | `sim_pick.wgsl` runs inside the sim pass list off `RenderParams.camPos/camFwd` (writes only the pick readback). One camera inside the tick encode. | `sim_pick.wgsl:23-25`, `simulation.cpp:160-161` |
| A5 | `SubmitTick` truncates cells/spawns/fluid silently and does NOT clamp `ops`/`exps` at all; two producers (`mob.cpp:3863`, `avatar.cpp:2057`) never check `kMaxOpsPerTick`. | `support.cpp:470-481, 773` |

### 2.4 Cross-platform-only hazards (defer unless clients differ in binary/STL)

| # | Finding | Where |
|---|---|---|
| X1 | `unordered_map` iteration order → voxel array order in the skin downsampler → RNG index (`Hash3(b.serial*C + vi, ..)`) and the Jolt compound-collider build order (truncates at 1024 boxes). Terrain patch eviction iterates `terrain_` and calls `RemoveBody` in hash order — Jolt's stated determinism precondition is API call order. | `lattice.h:82`; `debris.cpp:1295, 1900, 3205-3212, 419`; `physics.cpp:358-392` |
| X2 | `std::sort` with a size-only comparator orders island → body conversion under the per-tick op budget. | `debris.cpp:896-898` |
| X3 | `mobs_` is swap-and-pop; acting order and strict-`<` target ties follow it. Reproducible given identical history; fragile for anything caching an index. | `mob.cpp:3076-3094`; `ai_behavior.cpp:430` |
| X4 | Particle-pool overflow drops a scheduling-chosen set (`atomicAdd` slot ≥ cap). Deferred divergence; the gas path has a zero-refusal gate, the ballistic path does not. | `sim_particle.wgsl:104-107`; `sim_fluid.wgsl:1447` |
| X5 | Tint's `exp`/`log` const-eval path unverified (host libm vs software). | `sim_fluid.wgsl:185-187` |

### 2.5 Corrections to DESIGN.md that this audit found

| DESIGN says | The tree says |
|---|---|
| §2 "the MutationQueue … is also the serialization format for saves, the replication stream for networking, and the replay log for debugging" | Shape is right; nothing writes it out (L4). Saves are chunk snapshots. |
| §9b "wind … will also ride `TickParams` in phase 4" | It does; the note is stale. |
| §10 Option A (pure lockstep) "viable" | Not for an infinite world with per-client windows (S1–S3). The viable form is the hybrid in §5 below. |
| §10 "fixed tick … gameplay separated from render" listed as done-cheaply | Fixed tick: yes for every system except the player controller (L3). Gameplay/render separation: the tick body is inline in `main()` and reads per-frame input (L2, A3). |
| §2 "one tick latent, double-buffered" readback | "One tick latent and can be older when the ring is saturated" (`simulation.h:162-164`); no fixed latency exists (L1). |
| §7 debris is "outside the hashed domain" | True of the body, not of its ejecta (`ParticleSpawn` from Jolt floats, L6). |

---

## 3. How comparable games do it

| Game | World | Model | What it teaches us |
|---|---|---|---|
| **Teardown** (official MP, 2026-03) | Fully destructible voxel + physics, finite scene | "Semi-deterministic": destruction as discrete commands in **fixed-point integer math** over a reliable ordered stream; transforms/velocities/players **state-synced** unreliably with prediction and a per-client budget (~1 Mbit); **host is the server**; late join replays the recorded command stream; join-in-progress disabled when the buffer fills. The 2021 naive "send altered voxels" experiment choked on bandwidth. | This is the MutationQueue design, line for line. It is the shipped proof that "deterministic ops + state-synced entities" works for a destruction sim. |
| **Noita Entangled Worlds** (co-op mod) | Falling-sand pixel grid, infinite | **Chunk-based authority** with RLE deltas; **proximity-based entity authority** that hands off between players; a Rust proxy arbitrates. | The answer to "who sims chunks near a remote player": the nearest player, with the proxy arbitrating overlap. |
| **Factorio** | Finite, deterministic CPU sim | Pure lockstep, inputs only; desync → re-download map; 500-player servers across OS/CPU with no desync. | Lockstep is robust when every client holds the whole world. We cannot (S1). |
| **Minecraft** | Infinite chunked | Singleplayer **always runs an integrated server** on its own thread; client talks to it over an in-memory packet channel; "Open to LAN" exposes the server that was already running. | The singleplayer/multiplayer "toggle" is not a flag; it is a code boundary that exists all the time. |
| **Space Engineers / 7 Days to Die** | Voxel/chunk, server-auth | Server validates everything; chunk/region deltas over a procedural base; rate-limited initial world transfer. | Server-auth with chunk deltas is standard, and bandwidth scales with destruction — the thing Teardown avoided. |
| **Powder Toy (tptmp)** | Finite sand grid | Replicates brush inputs; `/sync` re-sends the whole scene when clients drift. | Input replication with periodic full resync is the cheap fallback when determinism is imperfect. |
| **Valheim** | Heightmap zones | Nearest player owns a zone; server relays/persists; bandwidth-bound at ~10 players. | Same authority idea as Noita EW; shows the ceiling of bandwidth-heavy object sync. |

Sources: Teardown devblog `blog.voxagon.se/2026/03/13/teardown-multiplayer.html`
and the 80.lv interview; `github.com/IntQuant/noita_entangled_worlds` +
DeepWiki; Factorio wiki "Desynchronization" and Alt-F4 #26; Minecraft wiki
"Setting up a LAN world"; Keen support thread "Less server load in Multiplayer
protocol"; `github.com/The-Powder-Toy/tptmp`; Edgegap "Valheim backend deep
dive"; Game Developer "Minimizing the Pain of Lockstep Multiplayer".

---

## 4. Answers to the three questions asked

**Is this the standard approach?** The design doc's two options are the two
textbook ones. The games closest to this one ship neither in pure form; they
ship the hybrid: a deterministic command stream for the world, state sync for
entities, one authority per region. That hybrid is what the MutationQueue was
built for.

**Is server-authoritative toggleable for singleplayer?** Yes, and the right
shape is Minecraft's: singleplayer is a host with zero remote clients, talking
to itself over a loopback. It is not a runtime flag; it is the boundary between
*authority* (tick, op authoring, sim) and *presentation* (render, camera, raw
input, prediction) — which is exactly the boundary A3/L2/L3 say is missing.

**How much work, now vs later?** §6.

---

## 5. Recommended model

**Host-authoritative op stream + deterministic client-side CA + chunk authority
+ hash-triggered resync.** In one paragraph: one machine is the authority for
each chunk (the host by default; the nearest player under distributed
authority, Noita-style). Only the authority runs the op-emitting gameplay for
its chunks (mobs, debris, spells, brush). The resulting per-tick op records
(A1 + L4) go over a reliable ordered stream. Every client runs the identical
integer CA on its own window from the same op records — bandwidth is "ops plus
corrections", not voxel deltas. Entities (players, mobs, bodies) are
state-synced with interest radius and prediction, as Teardown does. Per-chunk
hashes (S3 fixed) detect drift; on mismatch the authority re-sends the chunk
(Factorio's re-download, at chunk granularity). Late join streams chunks from
the store, which is already the right shape.

Why not the alternatives: pure lockstep needs every client to sim the same
set, impossible with per-client windows and a min-spec GPU bound; pure
server-auth with voxel deltas is the model Teardown measured and abandoned for
bandwidth, and it wastes the determinism this engine already paid for.

What this model requires that the tree does not have: L1 (readback latency
fixed in ticks, so the authority's decisions are a function of tick), L2–L3
(inputs and the player as tick commands), L4–L5 (a recorded, deduplicated,
attributed op stream), a decision on chunk authority (which decides whether
the host ever needs more than one window, S1/S7), and the per-chunk hash.

---

## 6. Now vs later

Rule used: **now** = every gameplay system built before the fix makes the fix
more expensive; **later** = additive plumbing whose cost does not grow.

### Now (the plan of record is `docs/PLAN_multiplayer_now.md`)

| Item | Findings | Size | Why it cannot wait |
|---|---|---|---|
| Fixed-latency snapshot pipeline | L1, L1′ | M | Every new decision reading `Snap()` deepens the timing dependence. The gate must test the shipped path. |
| Per-tick input command + player controller into the tick | L2, L3 | M | Every input-driven feature built on per-frame polling gets rewritten later. Unblocks replay. |
| Op stream hygiene: author + sequence, dedupe, recorder/replayer, choke-point clamps | L4, L5, A1, A5 | S | Cheap now; the recorder is a single-player debugging tool the day it lands and the proof for every later refactor. |
| Integer wind weather, integer landform bake | L6 | S | Both fixes are already written in comments; both violate the design's own rule. |
| Decide chunk authority; interest set instead of one point; DESIGN §10 rewrite | S6, S7, §5 | decision + S | Decides whether S1 ever needs solving on the host. |
| Rule: no sim-affecting process global keyed on the window origin; no new player state as `main()` locals; session struct extraction | S4, A3 | M | Both costs are linear in what gets added after today. |

### Later (M9)

Transport/lobby/Steam/WebRTC; entity state sync with interest radius and
prediction (A2 needs game ids for items); late-join chunk streaming; tuning /
materials / reactions hash in the handshake and tick+seed in `meta.svm` (L8);
per-chunk hash and the chunk re-send path (S3); hash over particle/fluid/gas
buffers (L9); ticket slots or a second window on the host if the authority
decision needs it (S7 → P1); container-order and comparator hardening (X1–X3);
particle-cap refusal gate (X4); Tint const-eval check (X5); local pacing under
network pacing (L7); `Stream`'s second `tickUBO` write folded into the tick
record (L10); dev UI sim inputs made client-local or replicated (L11).

---

## 7. Not determined by reading

- Whether the `snapshotStale → WaitIdle` self-defence (`support.cpp:1254-1259`,
  `kPagedSnapshotMaxGap = 4`) can change the STORED chunk set between two
  machines at different frame rates. Bounded, not proven identical. A
  two-framerate run of the `streaming` gate would settle it.
- Real-world staleness distribution of `Snap().tick` vs current tick in the
  windowed game (`SnapshotStallStats` exists, `main.cpp:10064-10073`).
- Whether the shared `C:/sv-deps` cache can ever hand a worktree a Jolt built
  without `CROSS_PLATFORM_DETERMINISTIC` (the `CACHE … FORCE` should prevent it).
- Whether the `gasIdleTicks_` latch's disarm edge is hash-neutral under
  readback jitter (`simulation.cpp:2356-2380`); the `caActive` skip is argued
  neutral and believed.
