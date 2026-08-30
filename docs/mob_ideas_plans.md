# Mob scaling — ideas and findings

Design notes from a 2026-08-28 code read. **Nothing here was measured** — every
claim is from reading `mob.cpp`, `debris.cpp`, `physics.cpp` and
`microbody.wgsl`. No perf run was made. Treat the numbers as structure, not
benchmarks.

Question asked: with mobs built from ~15 procedurally-animated limbs, how many
can we have on screen with real AI?

## The headline

**Limb count is not the cost.** Rendering is already screen-area bound and
animation is cheap float math. The cost is that *"limb" currently implies "Jolt
body"*, which in turn implies *"marching-cubes terrain patches around it"*.
Three couplings, all local. No rewrite needed.

## What's already cheap

- **Rendering.** `microbody.wgsl` draws each limb as one OBB (36 verts) and
  marches the brick per-fragment. Cost scales with **screen area**, not limb or
  voxel count. Only wall is the constant `kMaxBodySlots = 512` (`world.h:376`),
  shared with debris — ~34 mobs at 15 limbs.
- **Animation / IK / gait.** Pure float, no grid contact. Not the bottleneck.
- **Carving.** `CarveLimbRadial` (`mob.cpp:3492`) works on `limb.xf` + the voxel
  lattice. It takes a body handle only as an *identifier*. Mutilation is already
  Jolt-independent.
- **Melee.** Already a swept ray (`melee.h:22` "THE POSE IS THE HITBOX",
  `main.cpp:4016`), not contacts. Mob-vs-mob combat needs **zero** limb
  contacts.
- **Gore budgets** are global (`kBurnOpsPerTick`, `bleedOpsPerTick`,
  `kMaxParticleSpawnsPerTick`), so N mobs degrade quality, not framerate.

## The three couplings to break

1. **Terrain anchors for kinematic mobs.** Living limbs are kinematic
   (`MoveKinematicBody`) — they never respond to terrain. Yet every mob calls
   `AddTerrainAnchor` (`mob.cpp:1930`), pulling ~27 chunks of 5832-sample
   occupancy + marching cubes into `ManageTerrain` (`debris.cpp:1923`) for
   nothing. Gate it on "has a dynamic/held body" (dying, severed, ragdolling).
   **~5 lines, wins even at today's 16 mobs. Do this first.**
2. **`limb.xf` should be authoritative, not Jolt-derived.** Today `PostStep`
   writes it from `GetTransform`, and `AppendXforms`/`AppendMicroInsts` early-out
   on `!limb.body` — so a mob with no physics bodies is *invisible*. Flip it:
   animation writes `limb.xf`; Jolt overwrites only for limbs that have a body.
3. **Non-Jolt limb hit query.** Broad-phase on a per-mob sphere, then ray-vs-OBB
   against `limb.xf` + `limb.size`. The math already exists — it's the slab test
   in `microbody.wgsl:186-196`.

With those, a mid-tier mob is float animation + one capsule + zero terrain
patches + zero limb bodies, rendering and carving identically.

## Fake the physics

Cheap separation/knockback rules beat real contacts here, and not just on cost:
you have a **procedural gait**, so a positional push the gait *reacts to* (feet
re-plant, body leans) looks better than a rigid solve the gait ignores. There is
currently **no mob-vs-mob awareness at all** — `DecideIntent` reads only ground
probes. The seam is marked (`mob.h:886`).

Three traps:
- **Two-pass accumulation** (gather all pushes, then apply) or crowds drift.
- **Horizontal only** — `DriveLocomotion` clamps ground settle to ±0.3/tick
  (`mob.cpp:1451`); vertical push fights it and mobs climb each other.
- **Re-clamp against ground sense after pushing**, or crowded mobs get shoved
  through walls that `sense.clear[]` had already cleared.

Don't route separation through `desiredHeading` alone — turn rate is bounded, so
mobs interpenetrate for the whole turn.

## The fork that changes the answer

If mobs must **physically interact while alive** (pile up, knock back, partially
ragdoll), every nearby mob needs dynamic bodies, Jolt's cost is unavoidable, and
the ceiling is low tens regardless. If they stay kinematic actors that ragdoll on
death — what exists today — the ceiling is high. **Decide this explicitly**; no
later optimization pass rescues it.

## AI

- **Sensing is the easy half.** Mobs are outside the hashed domain, so stagger it
  freely (`tick % N == i % N`). Scheduling, not design.
- **Pathfinding is the hard one**, for an engine-specific reason: the CPU mirror
  is 3×3×3 chunks (~48 voxels) *and* the world is destructible — no static
  navmesh. What mobs navigate *on* is the real open question.
- Two things already in hand: **chunk dirty flags** are exactly the invalidation
  signal, and worldgen column data is a coarse traversability proxy that reaches
  past the mirror.

### GPU sensing

Precedent exists: **`sim_pick.wgsl`** is a DDA answering a CPU spatial query,
read back one tick latent. The async ring (`EncodeReadbacks`/`KickReadback`/
`World::Snap()`) is built.

- **The reason is reach, not throughput.** GPU has all 512³ resident; this takes
  perception from ~48 to ~512 voxels.
- **Split: GPU does queries, CPU does thinking.** Decisions are branchy and need
  CPU-side state; mirroring that state would be the real cost.
- **⚠ Keep sensing outputs INTEGER.** Mobs emit `BrushOp`s/`CellOp`s into the sim
  (`mob.cpp:2098`), so mob behaviour reaches the world hash. GPU float varies
  across vendors; the rule is "same hash *everywhere*". An integer kernel is as
  portable as the CA. **Get this right in v1** — retrofitting is much worse.
- **Flow fields > per-mob A\*.** A coarse distance field costs the same for 10 or
  500 mobs. At chunk granularity that's 32³ = **32 KB** readback. Long-range
  only; existing 8-probe ground sense handles the last 16 voxels.
- **Line of sight is not a cone sweep.** Range check (free) + `dot(fwd, toTarget)
  > cos(halfAngle)` (free) + **one** DDA per surviving candidate. Candidates are
  a short CPU list. ~90 rays for 30 mobs. LOS rays are the *cheap* kind — they
  terminate on first blocker, unlike the far-cascade shadow case.
- Eye position can come from the head limb's `xf`, so sight lines bob with the
  gait for free.
- Long-range LOS could use the far cascade pyramid. Over-occlusion is the safe
  error: "didn't notice me" reads fine, "saw me through a wall" reads broken.

## Lemmings — hundreds of *articulated* creatures

Small (1–2 voxel) creatures, **still microvoxel rigs with limbs, gait and
dismemberment**, but hundreds of them under a hive AI.

**This is not a third entity system.** Articulation was never the cost, so
hundreds of articulated lemmings needs the same three couplings fixed above —
nothing more. It's a flag on the existing `MobDef`:

| `physicsMode` | Limb bodies | Ground | Hits | Death | Count |
|---|---|---|---|---|---|
| `PerLimb` (today) | Jolt per limb | terrain patches | Jolt cast | per-limb ragdoll | tens |
| `Simple` (lemmings) | **none** | 2 grid lookups | ray-vs-OBB | whole-body debris | hundreds |

Reused unchanged: the rig, `UpdateAnimation`, `UpdateGait`, `CarveLimbRadial`,
`BurnLimbView` (`mob.h:852`), the microbody render path. Lemmings stay
dismemberable and burnable because **none of that ever needed Jolt** —
`CarveLimbRadial`'s only Jolt call is the `GetTransform` at `mob.cpp:3502`, which
coupling 2 removes anyway.

- **Voxel-native ground:** "is the cell ahead solid, is the cell below solid" —
  the same thing a falling-sand cell does. No Jolt, no terrain patches, no
  body-pool pressure.
- **Raise `kMaxBodySlots`** (512 → a few thousand). It's a buffer size; 1800
  limbs × 32 B ≈ 57 KB.
- **Animation is the one linear cost** — everything else scales with screen area
  or is constant. Order µs per creature, ~1 ms/tick at 300. Amortize by posing
  distant lemmings every 2–4 ticks.
- Render is the cheapest case for `microbody.wgsl`: `maxSteps = 3*maxDim + 4`, so
  ~16 steps against mina's ~200.

**Ants/swarms should be Lemmings, not particles** — particles do *not* evaluate
reactions in flight (`sim_particle.wgsl` only reads `klass`), so "player sets
them on fire" doesn't work until they land. Keep particles for things with
genuinely no identity (blood, sparks).

### Hive AI

Stigmergy — deposit, diffuse, decay — **is a cellular automaton**, which makes
hive AI the one AI paradigm native to this engine. Trails, danger and food
gradients are each a coarse grid updated by a sim-shaped pass.

- **Read back the FIELD, not per-agent results.** One 32³ byte grid = 32 KB;
  every lemming samples it locally on the CPU for free. No per-agent query
  buffer, no per-agent latency, and **cost independent of population** — the
  whole reason hive AI fits hundreds.
- Individual pathfinding mostly disappears: gradient following + the existing
  8-probe local avoidance *is* the behaviour, and it looks emergent rather than
  like 300 agents running A\*.
- **⚠ Integer field** — lemmings emit gore ops, so anything steering them reaches
  the world hash.
- **⚠ Dispatch over occupied coarse cells only**, via the sim's existing
  compaction pattern. A full-grid diffuse every tick violates rule 2 whether or
  not anything is alive.

**Rename `assets/mobs/critter.json`** — it's an 11-limb skinScale-2 quadruped,
architecturally identical to mina. Having a "critter" that's a Mob alongside a
small-creature system invites confusion, especially with concurrent sessions.

## Suggested order

1. Terrain-anchor gate (independent, wins now)
2. `limb.xf` authoritative + non-Jolt hit query (same project)
3. `MobDef::physicsMode = Simple` + raise `kMaxBodySlots` — this *is* the lemming
   system; 1–3 together are what make hundreds possible
4. Separation rules
5. Hive field pass (integer, activity-dispatched) → gradient follow in
   `DecideIntent`

Note that 3 is nearly free once 1 and 2 land, and that no step here adds a new
entity system — the whole plan is subtraction from the existing mob path.
